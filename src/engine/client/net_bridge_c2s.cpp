//=============================================================================//
//
// Purpose: net_bridge -- C2S encode (SendDatagram / usercmd transcoder).
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_observer.h"
#include "engine/client/net_bridge_internal.h"
#include "core/bridge_stats.h"
#include "engine/client/bridge_join_auth.h"
#include "engine/client/bridge_connect_password.h"
#include "engine/sys_integrity.h"
#include "engine/mdl_precache_client_grow.h"
#include "tier0/memvalidate.h"
#include "tier0/commandline.h"
#include "tier1/lzss.h"

#include "engine/cmd.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "engine/client/clientstate.h"
#include "engine/client/client.h"
#include "engine/client/cl_rcon.h"
#include "engine/client/cl_rcon_launcher.h"
#include "engine/server/sv_rcon.h"
#include "ebisusdk/EbisuSDK.h"
#include "public/tier1/cmd.h"
#include "public/bspflags.h"
#include "public/globalvars_base.h"
#include "tier1/cvar.h"
#include "rtech/playlists/playlists.h"
#include "windows/pso_cache.h"

#include "game/shared/activity.h"
#include "game/shared/activity_s3_to_s21_client.h"

#include "game/client/c_baseentity.h"
#include "game/client/mantle_boost.h"
#include "game/client/pred_authority.h"
#include "game/shared/heap_canary.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/vsquirrel_s21.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#pragma comment(lib, "psapi.lib")
#include <ctime>
#include <thread>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

// Client global vars (curTime/tickCount) for the [ANIM-WATCH] clock-domain probe.
// Defined in cdll_engine_int.cpp; same extern pattern as status_effects_sdk.cpp.
extern CGlobalVarsBase* gpGlobals;

// Silence C4456/C4459 on this TU: inner locals intentionally shadow outer names.
// C4244 stays enabled.
#pragma warning(disable: 4456 4459)
#include "engine/client/net_bridge_split.h"
#include "pluginsystem/modsystem.h"

static double   s_lastC2SFlushMs    = 0.0;         // GetTickCount64 at last successful FlushC2SNow
static bool     s_sendDisconnect    = false;

static ConVar bridge_signon_echo_ms("bridge_signon_echo_ms", "5000", FCVAR_RELEASE,
    "Retransmit window in ms for the C2S receipt rungs (FIRST_SNAP, FULL), which the server never answers. "
    "Rungs up to SPAWN keep re-asking until S2C proves the server moved. 0 = single shot.");

static ConVar bridge_c2s_keepalive_ms("bridge_c2s_keepalive_ms", "250", FCVAR_RELEASE,
    "Max gap in ms without a C2S flush before the off-thread keepalive sends one. "
    "The self-clock rides PollReceive on the main thread, so a cold map load blocks "
    "it for a minute and the dedi sees a silent client. 0 disables the keepalive.");

static ConVar bridge_signon_dead_ms("bridge_signon_dead_ms", "60000", FCVAR_RELEASE,
    "Name the server dead after re-asking one signon rung this long with zero S2C "
    "packets received since the rung armed. 0 never names it.");

// Reliable data to send via subchannel (serialized net_SignonState message).
// Retransmitted every packet until the server ACKs our nonce.
static uint8_t  s_reliableBuf[128]  = {};
static PendingSignon_s s_signonQueue[16] = {};
static int             s_signonQHead = 0;   // next to send
static int             s_signonQCount = 0;
static SRWLOCK         s_signonQLock = SRWLOCK_INIT;
static int       s_signonEchoSpawn    = 0;
static ULONGLONG s_signonEchoNextMs   = 0;
static const ULONGLONG kSignonEchoIntervalMs = 250;
static const ULONGLONG kSignonEchoLogIntervalMs = 5000;
static ULONGLONG s_signonEchoArmedMs   = 0;
static ULONGLONG s_signonEchoLogNextMs = 0;
static long long s_signonEchoSends     = 0;
static uint32_t  s_signonEchoArmedInSeq = 0;   // s_bridgeInSeqNr when the rung armed
static long      s_signonEchoArmedPso = 0;
static bool      s_signonEchoDeadWarned = false;

void S21Bridge_QueueSignon(int state, int spawn);
static bool S21Bridge_DequeueSignon(int* pState, int* pSpawn);
static bool S21Bridge_HasPendingSignon(void);
void S21Bridge_ClearPendingSignon(void);
static void S21Bridge_WriteC2SSignonState(bf_write& send, int sigState, int sigSpawn);

static void S21Bridge_WriteC2SSignonState(bf_write& send, int sigState, int sigSpawn)
{
	send.WriteUBitLong(5, NETMSG_TYPE_BITS); // net_SignonState (S3 type 5)
	send.WriteByte(sigState);
	send.WriteLong(sigSpawn);
	send.WriteString("");  // map name (server already knows)
	send.WriteString("");  // game mode
	send.WriteLong(0);     // int64 low
	send.WriteLong(0);     // int64 high
	send.WriteString("");  // playlist
}

void S21Bridge_QueueSignon(int state, int spawn)
{
	// S3 reconnects on any echo above CONNECTED whose spawn count differs from its own.
	// Unknown spawn above CONNECTED is dropped; CONNECTED is exempt.
	if (state > 2 && spawn < 0)
	{
		if (s_signonSeqSpawn < 0)
		{
			Warning(eDLL_T::ENGINE,
				"[BRIDGE-SIGNON] refusing C2S state=%d with no known server spawn count\n",
				state);
			return;
		}
		Warning(eDLL_T::ENGINE,
			"[BRIDGE-SIGNON] C2S state=%d spawn %d -> %d (server count)\n",
			state, spawn, s_signonSeqSpawn);
		spawn = s_signonSeqSpawn;
	}

	bool dropped = false;
	AcquireSRWLockExclusive(&s_signonQLock);
	if (s_signonQCount >= 16)
	{
		dropped = true;
	}
	else
	{
		const int tail = (s_signonQHead + s_signonQCount) & 15;
		s_signonQueue[tail].state = state;
		s_signonQueue[tail].spawn = spawn;
		++s_signonQCount;
	}
	ReleaseSRWLockExclusive(&s_signonQLock);
	if (dropped)
		Warning(eDLL_T::ENGINE,
			"[BRIDGE-SIGNON] signon queue full, dropped state=%d\n", state);
	if (state >= 2 && state <= 8)
	{
		const ULONGLONG now = GetTickCount64();
		s_signonEchoState   = state;
		s_signonEchoSpawn   = spawn;
		s_signonEchoUntilMs = now + (ULONGLONG)bridge_signon_echo_ms.GetInt();
		s_signonEchoNextMs  = now + kSignonEchoIntervalMs;
		s_signonEchoArmedMs   = now;
		s_signonEchoLogNextMs = 0;
		s_signonEchoSends     = 0;
		s_signonEchoArmedInSeq  = s_bridgeInSeqNr;
		s_signonEchoArmedPso    = g_pPsoCreateCount ? *g_pPsoCreateCount : 0;
		s_signonEchoDeadWarned  = false;
	}
}

static bool S21Bridge_DequeueSignon(int* pState, int* pSpawn)
{
	AcquireSRWLockExclusive(&s_signonQLock);
	if (s_signonQCount <= 0)
	{
		ReleaseSRWLockExclusive(&s_signonQLock);
		return false;
	}
	*pState = s_signonQueue[s_signonQHead].state;
	*pSpawn = s_signonQueue[s_signonQHead].spawn;
	s_signonQHead = (s_signonQHead + 1) & 15;
	--s_signonQCount;
	ReleaseSRWLockExclusive(&s_signonQLock);
	return true;
}

static bool S21Bridge_HasPendingSignon(void)
{
	AcquireSRWLockExclusive(&s_signonQLock);
	const bool has = (s_signonQCount > 0);
	ReleaseSRWLockExclusive(&s_signonQLock);
	return has;
}

void S21Bridge_ClearPendingSignon(void)
{
	AcquireSRWLockExclusive(&s_signonQLock);
	s_signonQHead = 0;
	s_signonQCount = 0;
	ReleaseSRWLockExclusive(&s_signonQLock);
	s_signonEchoState = -1;
	s_signonEchoUntilMs = 0;
}
// Per-type C2S relays. clc_Move 57->46, clc_ClientTick 65->60.
static ConVar bridge_c2s_clc_move("bridge_c2s_clc_move", "1", FCVAR_RELEASE,
	"Relay the engine's real clc_Move (S21 t=57 -> S3 t=46) to the S3 dedi. "
	"0 = off. 1 = on.");
static ConVar bridge_c2s_clc_tick("bridge_c2s_clc_tick", "1", FCVAR_RELEASE,
	"Relay the engine's real clc_ClientTick (S21 t=65 -> S3 t=60) to the S3 dedi. "
	"Default on: populates m_nDeltaAckTick through ProcessClientTick.");
// Native S21 C2S datagram is suppressed in Hook_sendto; these gates re-relay captured messages.
static ConVar bridge_c2s_stringcmd("bridge_c2s_stringcmd", "1", FCVAR_RELEASE,
	"Relay reliable NET_StringCmd (client commands: noclip/recharge/3rdperson/etc.) S21 t=3 -> S3 t=3 "
	"to the dedi. 1 = ON (DEFAULT). Cheat commands additionally require sv_cheats 1 on the dedi.");
static ConVar bridge_c2s_stringcmd_seq("bridge_c2s_stringcmd_seq", "1", FCVAR_RELEASE,
	"Prefix relayed client commands with a bridge sequence and send them redundantly; the dedi strips and dedupes. 0 = legacy single unreliable send.");
static ConVar bridge_c2s_stringcmd_redundancy("bridge_c2s_stringcmd_redundancy", "2", FCVAR_RELEASE,
	"Extra redundant sends per relayed client command.", true, 0.f, true, 7.f);
static ConVar bridge_c2s_stringcmd_resend_ms("bridge_c2s_stringcmd_resend_ms", "50", FCVAR_RELEASE,
	"Spacing in milliseconds between redundant sends.", true, 10.f, true, 1000.f);
static ConVar bridge_c2s_setconvar("bridge_c2s_setconvar", "1", FCVAR_RELEASE,
	"Relay runtime NET_SetConVar S21 t=5 -> S3 t=4. 1=ON (userinfo keys must exist in the connect-time blob). 0=connect-time snapshot only.");
static ConVar bridge_c2s_setconvar_redundancy("bridge_c2s_setconvar_redundancy", "1", FCVAR_RELEASE,
	"Resend each runtime NET_SetConVar relay once after 50ms (idempotent on the dedi); 0 = single-shot.");
static ConVar bridge_c2s_misc("bridge_c2s_misc", "1", FCVAR_RELEASE,
	"Relay misc C2S: clc_ClaimClientSidePickup (loot, S21 t=63 -> S3 57), clc_GamepadMsg (S21 t=74 -> S3 65). "
	"1 = ON (DEFAULT). Set 0 to disable loot-pickup + gamepad C2S relay. "
	"(Chat is NOT here -- see bridge_c2s_chat.)");
static ConVar bridge_c2s_chat("bridge_c2s_chat", "1", FCVAR_RELEASE,
	"Relay clc_ClientSayText (text chat) S21 t=66 -> S3 t=61. 1 = ON (DEFAULT). "
	"Wire format matches S3 clc_ClientSayText.");
static ConVar bridge_c2s_scriptremote("bridge_c2s_scriptremote", "1", FCVAR_RELEASE,
	"Relay S21 ScriptRemote C2S (Remote_ServerCallFunction: loot + ServerCallFunction RPCs) "
	"S21 t=4 -> S3 net_ScriptMessage(68). 1 = ON (DEFAULT). Requires dedi net_ScriptMessage "
	"to decode the S21 frame (deploy server.dll first).");
// Redundant explicit cycleslot on an updateCycleWeapon edge. The select edge
// itself is carried by emit-on-change against the null-cmd baseline.
static ConVar bridge_weap_cycle_fix("bridge_weap_cycle_fix", "1", FCVAR_RELEASE,
	"Deliver the S21 weaponSelect=0+updateCycleWeapon=1 first-weapon (slot 0) select to "
	"the dedi by force-emitting an explicit cycleslot on the updateCycleWeapon edge. "
	"1 = ON (DEFAULT, fix for unreachable rspn). 0 = stock emit-on-change.");

// Helpers: a single 'any C2S relay active' check the drain + slice paths share,
// and per-message gates for the per-type convars.
static inline bool S21Bridge_C2S_ClcMoveOn() {
	return bridge_c2s_clc_move.GetBool();
}
static inline bool S21Bridge_C2S_ClcTickOn() {
	return bridge_c2s_clc_tick.GetBool();
}
static inline bool S21Bridge_C2S_ReliableOn() {
	return bridge_c2s_stringcmd.GetBool() || bridge_c2s_setconvar.GetBool() || bridge_c2s_misc.GetBool() || bridge_c2s_chat.GetBool() || bridge_c2s_scriptremote.GetBool();
}
static inline bool S21Bridge_C2S_AnyOn() {
	return S21Bridge_C2S_ClcMoveOn() || S21Bridge_C2S_ClcTickOn() || S21Bridge_C2S_ReliableOn();
}
// S21 clc_Move=57, clc_ClientTick=65 (vtable GetType). Old 62/70 constants miss every frame.
enum { kS21_clc_Move = 57, kS21_clc_ClientTick = 65, kS3_clc_Move = 46, kS3_clc_ClientTick = 60 };
// clc_ClientTick body = [i32 m_nTick][i32 m_nRawTick]; dedi delta base = m_nRawTick;
// m_nTick == -1 is a full-update request. 7-bit type + 64-bit body = 71 bits.
enum { kClcClientTickBodyBits = 64, kClcClientTickTotalBits = 7 + kClcClientTickBodyBits };

//-----------------------------------------------------------------------------
// clc_Move slice is the gap between observed SendNetMsg writes, not "bit 0".
//-----------------------------------------------------------------------------
static int s_unrelSeenEnd = 0;   // end bit of the last SendNetMsg write we saw
static int s_unrelMoveB0  = -1;  // clc_Move slice
static int s_unrelMoveB1  = -1;
static int s_unrelTickB0  = -1;  // newest clc_ClientTick slice (freshest ack)
static int s_unrelTickB1  = -1;

// Unrelayed-move accounting. Counters always run; only the emission is gated.
static ConVar bridge_c2s_move_drop_log("bridge_c2s_move_drop_log", "1",
	FCVAR_DEVELOPMENTONLY,
	"[C2S-MOVE] announce clc_Move batches the relay could not place on the wire. "
	"A non-zero drop count is lost or late player input.");

static long long s_moveRelayed = 0;
static long long s_moveDropped = 0;
// Relayed batches whose move did not start at bit 0 -- i.e. exactly the ones
// the previous bit-0 heuristic discarded. This is the measurement of what that
// bug cost; if it stays 0 across a real match, the bug never fired here.
static long long s_moveRecovered = 0;

static inline void S21Bridge_C2SUnrelSlices_ResetForDatagram(void)
{
	s_unrelSeenEnd = 0;
	s_unrelMoveB0 = s_unrelMoveB1 = -1;
	s_unrelTickB0 = s_unrelTickB1 = -1;
}

void S21Bridge_C2SUnrelSlices_Reset(void)
{
	S21Bridge_C2SUnrelSlices_ResetForDatagram();
	s_moveRelayed = 0;
	s_moveDropped = 0;
	s_moveRecovered = 0;
}

// Returns the S3 type to relay this S21 C2S message as, or -1 to skip (gated off or
// not a relayable type). clc_Move(57)/clc_ClientTick(65) deliberately return -1 here
static inline int S21Bridge_C2S_MsgRelayS3Type(int s21Type) {
	switch (s21Type) {
	case 3:  return bridge_c2s_stringcmd.GetBool() ? 3  : -1; // net_StringCmd (client commands: noclip, etc.)
	case 4:  return bridge_c2s_scriptremote.GetBool() ? 68 : -1;
	case 5:  return bridge_c2s_setconvar.GetBool() ? 4  : -1; // net_SetConVar (runtime convar/userinfo update)
	case 63: return bridge_c2s_misc.GetBool()      ? 57 : -1; // clc_ClaimClientSidePickup (loot) -- S21 63 -VERIFIED (GetType; RFB reads 10-bit field). -5 shift from 68. -> S3 57.
	case 66: return bridge_c2s_chat.GetBool()      ? 61 : -1; // clc_ClientSayText (text chat) -- S21 66 -VERIFIED (NOT 71); verbatim -> S3 61
	case 71: return -1;                                       // DISPROVEN: S21 71 is a BINARY msg, NOT chat. Never relay (would corrupt the dedi packet).
	case 74: return bridge_c2s_misc.GetBool()      ? 65 : -1; // clc_GamepadMsg -- S21 74 -VERIFIED (GetType; RFB reads 8-bit field). No shift (74 == S21 74). -> S3 65.
	default: return -1;
	}
}

//=============================================================================
// Per-cmd clc_Move S21->S3: append two zero bits S3 still reads or ProcessMove kicks.
//=============================================================================

// Helper: copy nBits from input bf_read to output bf_write, returns false on
// either-side overflow.
static inline bool S21BR_CmdCopyBits(bf_read& r, bf_write& w, int nBits)
{
	if (nBits <= 0) return true;
	if (r.GetNumBitsLeft() < nBits) return false;
	return w.WriteBitsFromBuffer(&r, nBits) && !r.IsOverflowed() && !w.IsOverflowed();
}

// Helper: read+write a single bit, return its value. Sets *ok=false on overflow.
static inline int S21BR_CmdCopyOneBit(bf_read& r, bf_write& w, bool* ok)
{
	if (r.GetNumBitsLeft() < 1 || w.GetNumBitsLeft() < 1) { *ok = false; return 0; }
	const int b = r.ReadOneBit();
	w.WriteOneBit(b);
	if (r.IsOverflowed() || w.IsOverflowed()) { *ok = false; return b; }
	return b;
}

// =============================================================================
// Parse-emit transcoder: S21 NormalizedFloat vs S3 1+0/32 BitFloat. Bit-copy shears.
// =============================================================================


namespace S21BridgeCmd {

// ------ Parse helpers ------

static inline uint32_t ParseDeltaU(bf_read& r, int nBits, uint32_t prev) {
	if (!r.ReadOneBit()) return prev;
	return r.ReadUBitLong(nBits);
}
static inline int32_t ParseDeltaS(bf_read& r, int nBits, int32_t prev) {
	if (!r.ReadOneBit()) return prev;
	return r.ReadSBitLong(nBits);
}
static inline float ParseDeltaFloat(bf_read& r, float prev) {
	if (!r.ReadOneBit()) return prev;
	return r.ReadFloat();
}
static inline uint32_t ParseDeltaU32_Inc(bf_read& r, uint32_t prev) {
	if (!r.ReadOneBit()) return prev + 1;
	return r.ReadUBitLong(32);
}
// 1/3/4/33-bit WriteUserCmdDeltaNormalizedFloat (the cause encoding).
static inline float ParseDeltaNormalizedFloat(bf_read& r, float prev) {
	if (!r.ReadOneBit()) return prev;
	if (r.ReadOneBit()) {
		if (!r.ReadOneBit()) return 0.0f;
		return r.ReadOneBit() ? -1.0f : 1.0f;
	}
	return r.ReadFloat();
}
// 1/2/34-bit ternary for baseSnapshotTickCount.
static inline uint32_t ParseTernaryU32(bf_read& r, uint32_t prev) {
	if (!r.ReadOneBit()) return prev;
	if (!r.ReadOneBit()) return prev + 1;
	return r.ReadUBitLong(32);
}
// S21 ping long encoding (L4): 31 bits unsigned + 1 sign bit.
// If signed-value >= 0: writes (value, sign=0). If < 0: writes
static inline int32_t ParseS21PingLong(bf_read& r) {
	const uint32_t low31 = r.ReadUBitLong(31);
	const uint32_t sign  = r.ReadOneBit() ? 0x80000000u : 0u;
	return (int32_t)(low31 | sign);
}

// ------ Emit helpers ------

static inline void EmitDeltaU(bf_write& w, uint32_t cur, int nBits, uint32_t prev) {
	if (cur == prev) {
		w.WriteOneBit(0);
	} else {
		w.WriteOneBit(1);
		w.WriteUBitLong(cur, nBits);
	}
}
static inline void EmitDeltaS(bf_write& w, int32_t cur, int nBits, int32_t prev) {
	if (cur == prev) {
		w.WriteOneBit(0);
	} else {
		w.WriteOneBit(1);
		w.WriteSBitLong(cur, nBits);
	}
}
static inline void EmitDeltaFloat(bf_write& w, float cur, float prev) {
	uint32_t curBits, prevBits;
	memcpy(&curBits,  &cur,  sizeof(curBits));
	memcpy(&prevBits, &prev, sizeof(prevBits));
	if (curBits == prevBits) {
		w.WriteOneBit(0);
	} else {
		w.WriteOneBit(1);
		w.WriteUBitLong(curBits, 32);
	}
}
// [SEC] Finite + magnitude clamp for C2S float fields (fire focus / camera / knockback).
static inline float S21Bridge_SecClampFinite(float v, float maxAbs)
{
	if (!(v == v)) // NaN
		return 0.0f;
	if (maxAbs < 0.0f)
		maxAbs = 0.0f;
	if (v < -maxAbs || v > maxAbs)
		return 0.0f;
	return v;
}
static inline void EmitDeltaU32_Inc(bf_write& w, uint32_t cur, uint32_t prev) {
	if (cur == prev + 1) {
		w.WriteOneBit(0);
	} else {
		w.WriteOneBit(1);
		w.WriteUBitLong(cur, 32);
	}
}
static inline void EmitTernaryU32(bf_write& w, uint32_t cur, uint32_t prev) {
	if (cur == prev) {
		w.WriteOneBit(0);
	} else if (cur == prev + 1) {
		w.WriteOneBit(1);
		w.WriteOneBit(0);
	} else {
		w.WriteOneBit(1);
		w.WriteOneBit(1);
		w.WriteUBitLong(cur, 32);
	}
}

} // namespace S21BridgeCmd

// Parse one S21 usercmd body into typed `cur`. Returns false on overflow or
// unsupported feature (bulletTraceTestData=1 with its 600-byte payload).
static bool S21Bridge_ParseS21Cmd(bf_read& r, S21BridgeCmd::State& cur,
                                  const S21BridgeCmd::State& prev)
{
	using namespace S21BridgeCmd;

	// Diagnostic bit-position tracking: log how many bits consumed at each major
	// schema checkpoint. If consumption diverges from per-cmd expectation, the
	// checkpoints pinpoint which field group misaligned.
	const ssize_t parseStart = r.GetNumBitsRead();
	static long long s_parseDiag = 0;
	const bool diagOn = false;
	#define S21BR_DIAG(LABEL) \
		do { if (diagOn) Warning(eDLL_T::ENGINE, "[BRIDGE-OUT] parse-diag #%lld " LABEL " @+%lld\n", \
			++s_parseDiag, (long long)(r.GetNumBitsRead() - parseStart)); } while (0)

	cur.commandNumber          = ParseDeltaU32_Inc(r, prev.commandNumber);
	S21BR_DIAG("after_cmdNr(#1)");
	// S21 EMITS snapshot_interp_acc (field #2, struct +8).
	// Prior memory was wrong. Consume it.
	cur.snapshotInterpAcc      = ParseDeltaFloat(r, prev.snapshotInterpAcc);
	cur.commandViewAngles[0]   = ParseDeltaFloat(r, prev.commandViewAngles[0]);
	cur.commandViewAngles[1]   = ParseDeltaFloat(r, prev.commandViewAngles[1]);
	cur.commandViewAngles[2]   = ParseDeltaFloat(r, prev.commandViewAngles[2]);
	S21BR_DIAG("after_cmdViewAng(#4)");
	cur.viewSpringCorr[0]      = ParseDeltaFloat(r, prev.viewSpringCorr[0]);
	cur.viewSpringCorr[1]      = ParseDeltaFloat(r, prev.viewSpringCorr[1]);
	S21BR_DIAG("after_viewSpring(#6)");
	// S21 cmd+0x20 viewSpringCorrRoll (writer field after spring yaw; has none).
	// Emit maps onto S3 pitchangles.z -- see State comment.
	cur.viewSpringCorrRoll     = ParseDeltaFloat(r, prev.viewSpringCorrRoll);
	if (cur.viewSpringCorrRoll != prev.viewSpringCorrRoll)
	{
		static long s_springZLog = 0;
		if (++s_springZLog <= 16)
			Warning(eDLL_T::ENGINE,
				"[C2S-SPRING-Z] cmd=%u viewSpringCorrRoll(+0x20) %g -> %g (emit -> S3 pitchangles.z)\n",
				cur.commandNumber,
				static_cast<double>(prev.viewSpringCorrRoll),
				static_cast<double>(cur.viewSpringCorrRoll));
	}
	S21BR_DIAG("after_viewSpringCorrRoll(#8)");
	cur.forwardmove            = ParseDeltaNormalizedFloat(r, prev.forwardmove);
	cur.sidemove               = ParseDeltaNormalizedFloat(r, prev.sidemove);
	cur.upmove                 = ParseDeltaFloat(r, prev.upmove);
	S21BR_DIAG("after_fwdSideUp(#11)");
	// S21 EMITS leftTrigger + rightTrigger via WriteUserCmdDeltaNormalizedFloat
	// (calls #3 and #4 in the writer). Prior memory was wrong.
	cur.leftTrigger            = ParseDeltaNormalizedFloat(r, prev.leftTrigger);
	cur.rightTrigger           = ParseDeltaNormalizedFloat(r, prev.rightTrigger);
	cur.buttons                = ParseDeltaU(r, 32, prev.buttons);
	S21BR_DIAG("after_buttons(#14)");
	// S21 EMITS impulse (delta-u8 at +60) between buttons and weaponSelect.
	cur.impulse                = (uint8_t)ParseDeltaU(r, 8, prev.impulse);
	cur.weaponSelect           = ParseDeltaS(r, 5, prev.weaponSelect);
	cur.weaponSelectType       = ParseDeltaS(r, 3, prev.weaponSelectType);
	cur.realtimeWeaponMod      = ParseDeltaU(r, 16, prev.realtimeWeaponMod);
	cur.weaponToggleAkimbo     = (uint8_t)r.ReadOneBit();
	cur.weaponCustomActivity   = ParseDeltaS(r, 11, prev.weaponCustomActivity);
	// [C2S-ACT-NS] weaponCustomActivity is an ACTIVITY ENUM INDEX passed
	// verbatim to the S3 dedi. The S21 and S3 activity namespaces are 18
	if (cur.weaponCustomActivity != prev.weaponCustomActivity)
	{
		static long s_actNsLog = 0;
		if (++s_actNsLog <= 8)
			Warning(eDLL_T::ENGINE,
				"[C2S-ACT-NS] cmd=%u weaponCustomActivity %d -> %d (S21 index on S3 wire -- verify namespace)\n",
				cur.commandNumber, prev.weaponCustomActivity, cur.weaponCustomActivity);
	}
	cur.meleetarget            = ParseDeltaU(r, 32, prev.meleetarget);
	S21BR_DIAG("after_weapons(#19)");
	cur.controllerMode         = (uint8_t)r.ReadOneBit();
	cur.vehicleCameraControls  = (uint8_t)r.ReadOneBit();
	cur.updateCycleWeapon      = (uint8_t)r.ReadOneBit();
	S21BR_DIAG("after_ctrlFlags(#22)");
	for (int i = 0; i < 3; ++i)
		cur.hasWrittenAngle[i] = (uint8_t)ParseDeltaU(r, 8, prev.hasWrittenAngle[i]);
	S21BR_DIAG("after_hasWrittenAngle(#25)");
	cur.startEnergize          = (uint8_t)r.ReadOneBit();
	S21BR_DIAG("after_startEnergize(#26)");
	// Only a real key press flips this, so logging on change is cheap and makes
	// a single press of the energize-bound key (scriptCommand3 / in_togglefire)
	// directly visible when tracing the C2S path.
	if (cur.startEnergize != prev.startEnergize)
	{
		DevMsg(eDLL_T::ENGINE, "[C2S-ENERGIZE] cmd=%u startEnergize: %u -> %u\n",
			cur.commandNumber, prev.startEnergize, cur.startEnergize);
	}
	// Zipline command (S21 writer field #29 -- now verified via
	// ). S21 EMITS the SAME wire format as: 1 bit gate, then
	cur.ziplinePresent = r.ReadOneBit() != 0;
	if (cur.ziplinePresent) {
		cur.ziplineHandle = r.ReadUBitLong(32);
		for (int k = 0; k < 13; ++k)
			cur.ziplineFloats[k] = r.ReadFloat();
		static long long s_zipLog = 0;
		if (++s_zipLog <= 5)
			SDK_Log("[BRIDGE-OUT] zipline active: handle=0x%08X (consumed 14 dwords)\n",
				cur.ziplineHandle);
	} else {
		cur.ziplineHandle = 0;
		memset(cur.ziplineFloats, 0, sizeof(cur.ziplineFloats));
	}
	S21BR_DIAG("after_zipline(#29)");
	// commandObjectPlacement (S21 #28) -- DROP.
	// Success path is handle-only. Pose floats exist only on last-known-good
	{
		if (r.ReadOneBit()) {
			(void)r.ReadUBitLong(32);
			if (r.ReadOneBit()) {
				for (int k = 0; k < 6; ++k) (void)r.ReadUBitLong(32);
				(void)r.ReadUBitLong(32);
			}
		}
	}
	S21BR_DIAG("after_objPlace(#28)");
	// Ping commands (S21 #31-34) -- S21 writes 4 longs per ping using 31s+sign
	// encoding (NOT 3 longs, NOT plain 32-bit -- prior memory was wrong on both).
	for (int j = 0; j < 4; ++j) {
		cur.pings[j].present = r.ReadOneBit() != 0;
		if (cur.pings[j].present) {
			cur.pings[j].typeBits = (uint8_t)r.ReadUBitLong(3);
			if (cur.pings[j].typeBits > 4)
				cur.pings[j].typeBits = 0;
			for (int k = 0; k < 4; ++k)
				cur.pings[j].longs[k] = (uint32_t)ParseS21PingLong(r);
			Vector3D vec;
			r.ReadBitVec3Coord(vec);
			cur.pings[j].pos[0] = vec.x;
			cur.pings[j].pos[1] = vec.y;
			cur.pings[j].pos[2] = vec.z;
		} else {
			cur.pings[j].typeBits = 0;
			memset(cur.pings[j].longs, 0, sizeof(cur.pings[j].longs));
			memset(cur.pings[j].pos, 0, sizeof(cur.pings[j].pos));
		}
	}
	S21BR_DIAG("after_pings(#34)");
	cur.respawnInputDebounce = (uint8_t)r.ReadOneBit();
	cur.queuePrimaryAttack   = (uint8_t)r.ReadOneBit();
	// useCameraOverride (S21 #35)
	cur.useCameraOverride = r.ReadOneBit() != 0;
	if (cur.useCameraOverride) {
		for (int k = 0; k < 6; ++k)
			cur.cameraFloats[k] = ParseDeltaFloat(r, prev.cameraFloats[k]);
	} else {
		memcpy(cur.cameraFloats, prev.cameraFloats, sizeof(cur.cameraFloats));
	}
	S21BR_DIAG("after_camOvr(#35)");
	cur.hasThirdPersonAttackFocus = r.ReadOneBit() != 0;
	if (cur.hasThirdPersonAttackFocus) {
		for (int k = 0; k < 3; ++k)
			cur.thirdPersonFocus[k] = ParseDeltaFloat(r, prev.thirdPersonFocus[k]);
	} else {
		memcpy(cur.thirdPersonFocus, prev.thirdPersonFocus, sizeof(cur.thirdPersonFocus));
	}
	S21BR_DIAG("after_3rdPerson(#36)");
	cur.hasKnockBack = r.ReadOneBit() != 0;
	if (cur.hasKnockBack) {
		for (int k = 0; k < 3; ++k)
			cur.knockBackPos[k] = ParseDeltaFloat(r, prev.knockBackPos[k]);
	} else {
		memcpy(cur.knockBackPos, prev.knockBackPos, sizeof(cur.knockBackPos));
	}
	S21BR_DIAG("after_knockBack(#37)");
	cur.skydiveUnfollow        = (uint8_t)r.ReadOneBit();
	cur.baseSnapshotTickCount  = ParseTernaryU32(r, prev.baseSnapshotTickCount);
	S21BR_DIAG("after_baseSnap(#39)");
	cur.predictedServerEventAck = ParseDeltaU(r, 32, prev.predictedServerEventAck);
	S21BR_DIAG("after_predictedAck(#42)");
	// S21 EMITS bulletTraceTestData as a 1-bit placeholder always 0 (writer
	// unconditionally emits `(a1, 0)` here). Consume the bit.
	cur.bulletTracePresent     = r.ReadOneBit() != 0;
	cur.frametime              = ParseDeltaFloat(r, prev.frametime);
	S21BR_DIAG("after_frametime(#44_END)");

	#undef S21BR_DIAG
	return !r.IsOverflowed();
}

// C2S transcoder: emit the REAL tick as S3 CUserCmd::tick_count instead of the
// prev+1 default. S3 v_ReadUserCmd decodes tick_count (+0x04) with a prev+1
static ConVar bridge_c2s_fire_focus("bridge_c2s_fire_focus", "1", FCVAR_RELEASE,
	"[C2S-FIRE-FOCUS] Correct S3 usercmd tail-block mapping: camera override "
	"-> C/D/E (+0x18A/+0x190/+0x19C), thirdPersonAttackFocus -> F (+0x1A8; "
	"the S3 fire natives aim at this point when set), knockback -> G, "
	"skydiveUnfollow -> H. 0 = strip/legacy (camera split across F/G, "
	"focus/knockback/skydive dropped). NaN/Inf/absurd magnitudes are always clamped.");

// [SEC] Magnitude cap for fire-focus / camera / knockback floats on the C2S wire.
static ConVar bridge_c2s_fire_focus_max("bridge_c2s_fire_focus_max", "100000", FCVAR_RELEASE,
	"[SEC] Absolute magnitude clamp for camera/focus/knockback floats on C2S usercmd. "
	"Non-finite values become 0. Default 100000 world units.");

static ConVar bridge_c2s_tickcount_real("bridge_c2s_tickcount_real", "1", FCVAR_RELEASE,
	"C2S transcoder: emit the real tick (baseSnapshotTickCount) as S3 CUserCmd::tick_count "
	"instead of the prev+1 default that made the dedi see 1..totalCmds (breaks lag-comp). "
	"1 = real tick (default), 0 = legacy no-change bit.");

// [SEC] Tight lag-comp window clamps for client-emitted tick/time (C2S still carries client tick).
static ConVar bridge_c2s_tick_delta_max("bridge_c2s_tick_delta_max", "256", FCVAR_RELEASE,
	"[SEC] Max |baseSnapshotTickCount - prev| accepted on C2S emit. Larger jumps clamp to "
	"prev+/-this (default 256 ticks ~12.8s @20Hz). 0 = no delta clamp. Does not expand unlag.");
static ConVar bridge_c2s_cmdtime_max("bridge_c2s_cmdtime_max", "86400", FCVAR_RELEASE,
	"[SEC] Absolute max for synthesized CUserCmd::command_time (seconds). Non-finite or "
	"above this clamps to 0 (fail safe). Default 86400.");

// [C2S-CMDTIME] S3 CUserCmd::command_time (+0x08) is not an S21 wire field. Prior
// emit always wrote gate=0 so the dedi saw 0 forever (or only the post-read
static ConVar bridge_c2s_cmdtime_emit("bridge_c2s_cmdtime_emit", "1", FCVAR_RELEASE,
	"[C2S-CMDTIME] Emit S3 command_time from baseSnapshotTickCount + snapshot_interp_acc "
	"( UserCmd_ComputeCommandTime). 1 = on (default). 0 = legacy gate-0 (dedi tick synth).");

// C2S ping field remap. S3's CUserCmd ping struct is [commandType+0, pingType+4,
// entityHandle+8, userTicketId+12, pingOrigin+16] and the reader takes 4 raw longs
static ConVar bridge_ping_remap("bridge_ping_remap", "1", FCVAR_RELEASE,
	"C2S transcoder: remap S21 ping fields into the correct S3 ping slots (pingType <- "
	"typeBits, entityHandle <- ehandle) instead of a straight long passthrough that "
	"scrambled pingType. 1 = remap (default), 0 = legacy passthrough.");
static constexpr uint8_t kS21ImpulseRealValueMask =
	uint8_t(~(kS21ExtraFlag_StartEnergize | kS21ExtraFlag_ToggleAkimbo));

static ConVar bridge_c2s_energize_flag("bridge_c2s_energize_flag", "1", FCVAR_RELEASE,
	"C2S transcoder: smuggle S21 startEnergize (wire field #26; S3 has no native slot) "
	"into impulse's unused top bit so dedi can read it from CUserCmd::impulse. "
	"1 = on (default). 0 = legacy passthrough.");


// Emit one cmd in S3's wire format from typed `cur` + delta source `prev`.
// Walker order mirrors S3 v_ReadUserCmd field-by-field.
static bool S21Bridge_EmitS3Cmd(bf_write& w, const S21BridgeCmd::State& cur,
                                const S21BridgeCmd::State& prev)
{
	using namespace S21BridgeCmd;

	// 1. command_number (+0x00) -- 1+0/32 (default=prev+1)
	EmitDeltaU32_Inc(w, cur.commandNumber, prev.commandNumber);
	// 2. tick_count (+0x04) -- S3 reads this with a prev+1 DEFAULT (not "no-change").
	// Emit the real tick (baseSnapshotTickCount) as a 32-bit literal so lag-comp works.
	// [SEC] Clamp jump vs prev so a corrupt wire tick cannot expand the unlag window.
	if (bridge_c2s_tickcount_real.GetBool())
	{
		uint32_t tickEmit = cur.baseSnapshotTickCount;
		const int dMax = bridge_c2s_tick_delta_max.GetInt();
		if (dMax > 0 && prev.baseSnapshotTickCount != 0)
		{
			const int64_t d = (int64_t)tickEmit - (int64_t)prev.baseSnapshotTickCount;
			if (d > dMax || d < -dMax)
			{
				const int64_t dClamped = (d > dMax) ? (int64_t)dMax : -(int64_t)dMax;
				if (d > dMax)
					tickEmit = prev.baseSnapshotTickCount + (uint32_t)dMax;
				else
					tickEmit = prev.baseSnapshotTickCount - (uint32_t)dMax;

				static long s_c2sTickClampN = 0;
				const long nClamp = ++s_c2sTickClampN;
				if (nClamp <= 8 || (nClamp % 256) == 0)
				{
					Warning(eDLL_T::ENGINE,
						"[SEC][C2S-TICK] clamp raw_delta=%lld clamped_delta=%lld count=%ld\n",
						(long long)d, (long long)dClamped, nClamp);
				}
			}
		}
		w.WriteOneBit(1);
		w.WriteUBitLong(tickEmit, 32);
	}
	else
		w.WriteOneBit(0);
	// 3. command_time (+0x08) -- S3 only; computed in TransformOneUsercmd
	// (UserCmd_ComputeCommandTime). Gate-0 when disabled / still zero.
	// [SEC] Drop non-finite / absurd command_time (fail safe -> gate 0).
	if (bridge_c2s_cmdtime_emit.GetBool() && cur.commandTime > 0.0f)
	{
		const float ctMax = bridge_c2s_cmdtime_max.GetFloat();
		const float ct = S21Bridge_SecClampFinite(cur.commandTime, (ctMax > 0.0f) ? ctMax : 86400.0f);
		const float pt = S21Bridge_SecClampFinite(prev.commandTime, (ctMax > 0.0f) ? ctMax : 86400.0f);
		if (ct > 0.0f)
			EmitDeltaFloat(w, ct, pt);
		else
			w.WriteOneBit(0);
	}
	else
		w.WriteOneBit(0);
	// 4-6. viewangles xyz (+0x0C/+0x10/+0x14)
	const float kC2sEmitMax = 1.0e9f;
	EmitDeltaFloat(w,
		S21Bridge_SecClampFinite(cur.commandViewAngles[0], kC2sEmitMax),
		S21Bridge_SecClampFinite(prev.commandViewAngles[0], kC2sEmitMax));
	EmitDeltaFloat(w,
		S21Bridge_SecClampFinite(cur.commandViewAngles[1], kC2sEmitMax),
		S21Bridge_SecClampFinite(prev.commandViewAngles[1], kC2sEmitMax));
	EmitDeltaFloat(w,
		S21Bridge_SecClampFinite(cur.commandViewAngles[2], kC2sEmitMax),
		S21Bridge_SecClampFinite(prev.commandViewAngles[2], kC2sEmitMax));
	// 7-9. pitchangles xyz (+0x18/+0x1C/+0x20) -- S3 dump target for spring.
	// S21: spring pitch/yaw + optional roll residual at cmd+0x20.
	EmitDeltaFloat(w, cur.viewSpringCorr[0], prev.viewSpringCorr[0]);
	EmitDeltaFloat(w, cur.viewSpringCorr[1], prev.viewSpringCorr[1]);
	EmitDeltaFloat(w, cur.viewSpringCorrRoll, prev.viewSpringCorrRoll);
	// 10-12. fwd/side/up (+0x24/+0x28/+0x2C) plain 1+0/32 -- TRANSCODE from normalized
	EmitDeltaFloat(w,
		S21Bridge_SecClampFinite(cur.forwardmove, kC2sEmitMax),
		S21Bridge_SecClampFinite(prev.forwardmove, kC2sEmitMax));
	EmitDeltaFloat(w,
		S21Bridge_SecClampFinite(cur.sidemove, kC2sEmitMax),
		S21Bridge_SecClampFinite(prev.sidemove, kC2sEmitMax));
	EmitDeltaFloat(w,
		S21Bridge_SecClampFinite(cur.upmove, kC2sEmitMax),
		S21Bridge_SecClampFinite(prev.upmove, kC2sEmitMax));
	// 13. buttons (+0x30) -- 1+0/32
	EmitDeltaU(w, cur.buttons, 32, prev.buttons);
	// 14. impulse (+0x34) -- 1+0/8. S21 EMITS impulse (parse now reads it).
	// [S21-EXTRA-FLAGS] OR the smuggled startEnergize bit into impulse's top
	uint8_t s3ImpulseCur = (uint8_t)(cur.impulse & kS21ImpulseRealValueMask);
	if (bridge_c2s_energize_flag.GetBool() && cur.startEnergize)
		s3ImpulseCur = (uint8_t)(s3ImpulseCur | kS21ExtraFlag_StartEnergize);
	if (cur.weaponToggleAkimbo)
		s3ImpulseCur = (uint8_t)(s3ImpulseCur | kS21ExtraFlag_ToggleAkimbo);
	EmitDeltaU(w, s3ImpulseCur, 8, s_bridgeC2sPrevS3ImpulseWire);
	s_bridgeC2sPrevS3ImpulseWire = s3ImpulseCur;
	// weaponSelect is 5-bit signed (-16..15). Select event: value-change or updateCycleWeapon.
	const bool selectEvent =
		bridge_weap_cycle_fix.GetBool() && cur.updateCycleWeapon != 0
		&& cur.weaponSelect >= 0 && cur.weaponSelectType >= 0;
	const bool emitSelect = (cur.weaponSelect != prev.weaponSelect) || selectEvent;
	if (emitSelect)
	{
		w.WriteOneBit(1);
		w.WriteSBitLong(cur.weaponSelect, 5);
	}
	else
	{
		w.WriteOneBit(0);
	}
	// 16. weaponindex (+0x36) -- 1+0/3s. S21 weaponSelectType is also 3 bits
	// signed (range -4..3); width matches S3's 3-bit slot. Map weaponSelectType here.
	if (emitSelect)
	{
		w.WriteOneBit(1);
		w.WriteSBitLong(cur.weaponSelectType, 3);
	}
	else
	{
		EmitDeltaS(w, cur.weaponSelectType, 3, prev.weaponSelectType);
	}

	// 17. weaponselect (+0x37) -- 1+0/16
	EmitDeltaU(w, cur.realtimeWeaponMod, 16, prev.realtimeWeaponMod);
	// S21 emits weaponToggleAkimbo (1-bit always) between realtimeWeaponMod and weaponCustomActivity;
	// it is now smuggled in impulse 0x40.
	EmitDeltaS(w, cur.weaponCustomActivity, 11, prev.weaponCustomActivity);
	// 19. nUnk3C (+0x3C) -- 1+0/32
	EmitDeltaU(w, cur.meleetarget, 32, prev.meleetarget);
	// 20-22. controllermode/fixangles/setlastcycleslot (3 * 1-bit always)
	w.WriteOneBit(cur.controllerMode ? 1 : 0);
	w.WriteOneBit(cur.vehicleCameraControls ? 1 : 0);
	w.WriteOneBit(cur.updateCycleWeapon ? 1 : 0);

	// unkData section (+0x48..+0xD7): 2 outer iterations, each 18 gated-dword
	// fields. S21 never populates these; emit all-zero gates (inherit from prev
	for (int outer = 0; outer < 2; ++outer) {
		// v230[0] and v230[1]
		w.WriteOneBit(0); w.WriteOneBit(0);
		// inner loop 1: 4 iterations, 2 gated fields each
		for (int i = 0; i < 4; ++i) { w.WriteOneBit(0); w.WriteOneBit(0); }
		// inner loop 2: 4 iterations, 2 gated fields each
		for (int i = 0; i < 4; ++i) { w.WriteOneBit(0); w.WriteOneBit(0); }
	}

	// Zipline gate (+0xD8) -- 1-bit gate. If 1, payload is 1 raw u32 handle +
	// 13 raw 32-bit floats (matching emit format -- bf_write::WriteUBitLong
	bool zipPresent = cur.ziplinePresent;
	if (zipPresent)
	{
		for (int k = 0; k < 13; ++k)
		{
			const float zf = cur.ziplineFloats[k];
			if (!(zf == zf) || zf - zf != 0.0f)
			{
				zipPresent = false;
				break;
			}
		}
	}
	w.WriteOneBit(zipPresent ? 1 : 0);
	if (zipPresent) {
		w.WriteUBitLong(cur.ziplineHandle, 32);
		for (int k = 0; k < 13; ++k) {
			const float zf = S21Bridge_SecClampFinite(cur.ziplineFloats[k], kC2sEmitMax);
			uint32_t bits;
			memcpy(&bits, &zf, sizeof(bits));
			w.WriteUBitLong(bits, 32);
		}
	}

	// Ping commands: 4 iterations (v429=4), each 28 bytes at a2+284+(i*28).
	// Per ping: 1-bit gate. If gate=0: writes *(v428-2)=0, skips.
	for (int j = 0; j < 4; ++j) {
		w.WriteOneBit(cur.pings[j].present ? 1 : 0);
		if (cur.pings[j].present) {
			// S3 reader takes 4 raw longs into [commandType+0, pingType+4,
			// entityHandle+8, userTicketId+12] + BitVec3Coord(pingOrigin+16). S21
			if (bridge_ping_remap.GetBool()) {
				// commandType (+0) is the DISPATCH SELECTOR: S3 indexes the 4-entry
				// CodeCallback_PingCommand* table @ (EnemySpotted/QueueTrace/
				uint32_t typeBits = cur.pings[j].typeBits;
				if (typeBits > 4)
					typeBits = 0;
				w.WriteUBitLong(typeBits, 32);  // commandType (+0) <- S21 3-bit op (THE dispatch key)
				w.WriteUBitLong(cur.pings[j].longs[0], 32);  // pingType (+4) <- S21 longs[0] (category param)
				w.WriteUBitLong(cur.pings[j].longs[1], 32);  // entityHandle (+8) <- S21 ehandle
				w.WriteUBitLong(0, 32);                      // userTicketId (+12): server-assigned, 0
			} else {
				w.WriteUBitLong(cur.pings[j].longs[0], 32);
				w.WriteUBitLong(cur.pings[j].longs[1], 32);
				w.WriteUBitLong(cur.pings[j].longs[2], 32);
				w.WriteUBitLong(cur.pings[j].longs[3], 32);
			}
			Vector3D vec(cur.pings[j].pos[0], cur.pings[j].pos[1], cur.pings[j].pos[2]);
			w.WriteBitVec3Coord(vec);
		}
	}

	// Tail section: A,B,C then conditionally D,E, then F,G, H,I, J,K, L, M, N
	// A (+0x188) -- 1-bit always
	w.WriteOneBit(cur.respawnInputDebounce ? 1 : 0);
	// B (+0x189) -- 1-bit always (queuePrimaryAttack, S21 field 34)
	w.WriteOneBit(cur.queuePrimaryAttack ? 1 : 0);

	// [C2S-FIRE-FOCUS] Tail: C camera override (gate + 6 floats); F thirdPersonAttackFocus;
	// G knockBackEyePosition; H skydiveUnfollow. Mis-mapping camera into F/G breaks fire direction.
	if (bridge_c2s_fire_focus.GetBool())
	{
		// [SEC] Clamp camera/focus/knockback to finite magnitude before emit.
		const float kFocusMax = bridge_c2s_fire_focus_max.GetFloat();
		// C (+0x18A) gate + D (+0x190 vec3) + E (+0x19C vec3) = camera override
		if (cur.useCameraOverride) {
			w.WriteOneBit(1);
			for (int k = 0; k < 6; ++k)
				EmitDeltaFloat(w,
					S21Bridge_SecClampFinite(cur.cameraFloats[k], kFocusMax),
					S21Bridge_SecClampFinite(prev.cameraFloats[k], kFocusMax));
		} else {
			w.WriteOneBit(0);
		}
		// F (+0x1A8 gate + 3 floats at +0x1AC/+0x1B0/+0x1B4) = attack focus
		if (cur.hasThirdPersonAttackFocus) {
			w.WriteOneBit(1);
			for (int k = 0; k < 3; ++k)
				EmitDeltaFloat(w,
					S21Bridge_SecClampFinite(cur.thirdPersonFocus[k], kFocusMax),
					S21Bridge_SecClampFinite(prev.thirdPersonFocus[k], kFocusMax));
		} else {
			w.WriteOneBit(0);
		}
		// G (+0x1B8 gate + 3 floats at +0x1BC/+0x1C0/+0x1C4) = knockback eye pos
		if (cur.hasKnockBack) {
			w.WriteOneBit(1);
			for (int k = 0; k < 3; ++k)
				EmitDeltaFloat(w,
					S21Bridge_SecClampFinite(cur.knockBackPos[k], kFocusMax),
					S21Bridge_SecClampFinite(prev.knockBackPos[k], kFocusMax));
		} else {
			w.WriteOneBit(0);
		}
		// H (+0x18B) -- 1-bit always = skydiveUnfollow
		w.WriteOneBit(cur.skydiveUnfollow ? 1 : 0);
		// I (+0x18C) -- 1-bit always, S3-only bool with no S21 source; 0.
		w.WriteOneBit(0);
	}
	else
	{
		// Legacy mapping: C=0, camera 6-pack split across F+G,
		// focus/knockback/skydive dropped. Keep byte-identical to the old
		// wire for A/B rollback.
		w.WriteOneBit(0);
		if (cur.useCameraOverride) {
			w.WriteOneBit(1);
			EmitDeltaFloat(w, cur.cameraFloats[0], prev.cameraFloats[0]);
			EmitDeltaFloat(w, cur.cameraFloats[1], prev.cameraFloats[1]);
			EmitDeltaFloat(w, cur.cameraFloats[2], prev.cameraFloats[2]);
		} else {
			w.WriteOneBit(0);
		}
		if (cur.useCameraOverride) {
			w.WriteOneBit(1);
			EmitDeltaFloat(w, cur.cameraFloats[3], prev.cameraFloats[3]);
			EmitDeltaFloat(w, cur.cameraFloats[4], prev.cameraFloats[4]);
			EmitDeltaFloat(w, cur.cameraFloats[5], prev.cameraFloats[5]);
		} else {
			w.WriteOneBit(0);
		}
		w.WriteOneBit(0);
		w.WriteOneBit(0);
	}
	// J (+0x1C8) -- ternary (baseSnapshotTickCount)
	EmitTernaryU32(w, cur.baseSnapshotTickCount, prev.baseSnapshotTickCount);
	// K (+0x1CC) -- ternary, S3 only: SYNTHESIZE no-change
	w.WriteOneBit(0);
	// L (+0x1D0) -- predictedServerEventAck (1+0/32)
	EmitDeltaU(w, cur.predictedServerEventAck, 32, prev.predictedServerEventAck);
	// M -- 1-bit gate for N (frametime). If 0, reader returns early, frametime
	// inherits from prologue Copy. Must be 1 so frametime is consumed.
	w.WriteOneBit(1);
	// N (+0x1D8) -- frametime (1+0/32 BitFloat, NaN trap)
	EmitDeltaFloat(w, cur.frametime, prev.frametime);

	return !w.IsOverflowed();
}


//-----------------------------------------------------------------------------
// [C2S-FT-RESTAMP] Marked maps stamp usercmd frametime=0; restamp from measured wall interval so dedi ClampUserCmd does not floor every cmd.
//-----------------------------------------------------------------------------
static ConVar bridge_c2s_ft_restamp("bridge_c2s_ft_restamp", "1", FCVAR_RELEASE,
	"[C2S-FT-RESTAMP] Restamp usercmds that reach the C2S transcoder with "
	"frametime <= 0 using the measured per-cmd wall interval (fixes the "
	"marked-map slow-mo: the dedi min-clamps zero-frametime cmds to 2.857ms "
	"= ~39%% sim speed). 1 = on (default), 0 = legacy pass-through.");

// Parse one S21 cmd from r into a typed cmd, then emit it in S3 wire format to w.
static bool S21Bridge_TransformOneUsercmd(bf_read& r, bf_write& w)
{
	S21BridgeCmd::State cur;
	memset(&cur, 0, sizeof(cur));

	// Snapshot prev BEFORE update so the diag log can show what was used as delta source.
	const uint32_t prevCmdNr     = s_bridgeC2sPrevCmd.commandNumber;
	const float    prevFrametime = s_bridgeC2sPrevCmd.frametime;

	const int emitBitsBefore = w.GetNumBitsWritten();

	if (!S21Bridge_ParseS21Cmd(r, cur, s_bridgeC2sPrevCmd)) {
		static long long s_parseFailLog = 0;
		if (++s_parseFailLog <= 5)
			Warning(eDLL_T::ENGINE, "[BRIDGE-OUT] S21 parse failed (overflow/unsupported); dropping cmd\n");
		return false;
	}

	// [C2S-FT-RESTAMP] estimator: advance on NEW command numbers only (backup
	// cmds re-walk older numbers and must not feed the interval measurement).
	if (cur.commandNumber > s_ftEstLastCmdNr)
	{
		const double   ftNow = Plat_FloatTime();
		const uint32_t nCmd  = cur.commandNumber - s_ftEstLastCmdNr;
		if (s_ftEstLastWall > 0.0 && nCmd <= 64)
		{
			const double dt = ftNow - s_ftEstLastWall;
			if (dt > 0.0 && dt < 0.5)
			{
				const float perCmd = static_cast<float>(dt / static_cast<double>(nCmd));
				s_ftEstPerCmd = (s_ftEstPerCmd > 0.0f)
					? (0.9f * s_ftEstPerCmd + 0.1f * perCmd)
					: perCmd;
			}
		}
		s_ftEstLastCmdNr = cur.commandNumber;
		s_ftEstLastWall  = ftNow;
	}

	if (bridge_c2s_ft_restamp.GetBool() && cur.frametime <= 0.0f)
	{
		// 1/140 fallback until the estimator primes (~one batch); clamp so a
		// hitch can never inject a huge integration step server-side.
		float ft = (s_ftEstPerCmd > 0.0f) ? s_ftEstPerCmd : 0.00714f;
		if (ft < 0.001f) ft = 0.001f;
		if (ft > 0.05f)  ft = 0.05f;
		cur.frametime = ft;

		static volatile LONG s_ftRestampCount = 0;
		const LONG n = InterlockedIncrement(&s_ftRestampCount);
		if (n <= 8 || (n % 2000) == 0)
			Warning(eDLL_T::CLIENT,
				"[C2S-FT-RESTAMP] #%ld cmd=%u frametime<=0 on wire -> restamped %.5f (est %.2fms/cmd)\n",
				n, cur.commandNumber, static_cast<double>(cur.frametime),
				static_cast<double>(s_ftEstPerCmd * 1000.0f));
	}

	// [C2S-CMDTIME] UserCmd_ComputeCommandTime: time = (tick + interp_acc) * interval.
	// Prefer PredNative_TickInterval (engine gpGlobals_Client+0x44); fall back 0.05.
	if (bridge_c2s_cmdtime_emit.GetBool() && cur.baseSnapshotTickCount > 0)
	{
		float interval = PredNative_TickInterval();
		if (interval <= 0.0f)
		{
			static bool s_cmdtimeIntervalFallbackWarned = false;
			if (!s_cmdtimeIntervalFallbackWarned)
			{
				s_cmdtimeIntervalFallbackWarned = true;
				Warning(eDLL_T::CLIENT,
					"[C2S-CMDTIME] engine tick interval unresolved -- assuming 0.05; "
					"lag compensation will be wrong if the server is not 20 Hz\n");
			}
			interval = 0.05f;
		}
		const float acc = cur.snapshotInterpAcc;
		// Clamp interp accumulator to a single tick of fractional credit;
		// uses it raw but a corrupt/misaligned parse could inject huge lag-comp.
		const float accClamped = (acc > -1.0f && acc < 2.0f) ? acc : 0.0f;
		cur.commandTime = (static_cast<float>(cur.baseSnapshotTickCount) + accClamped) * interval;
		if (cur.commandTime < 0.0f)
			cur.commandTime = 0.0f;
		// [SEC] Cap absolute command_time; non-finite -> 0 (fail safe, no unlag expand).
		{
			const float ctMax = bridge_c2s_cmdtime_max.GetFloat();
			const float cap = (ctMax > 0.0f) ? ctMax : 86400.0f;
			if (!(cur.commandTime == cur.commandTime) || cur.commandTime > cap)
				cur.commandTime = 0.0f;
		}
	}

	if (!S21Bridge_EmitS3Cmd(w, cur, s_bridgeC2sPrevCmd)) {
		static long long s_emitFailLog = 0;
		if (++s_emitFailLog <= 5)
			Warning(eDLL_T::ENGINE, "[BRIDGE-OUT] S3 emit overflow; dropping cmd\n");
		return false;
	}
	s_bridgeC2sPrevCmd = cur;

	static long long s_xformLog = 0;
	++s_xformLog;
	// Steady-state movement capture: log the first 16, then every moving cmd
	// (fwd/side/up nonzero) rate-limited, plus a periodic heartbeat. This shows
	const bool moving = (cur.forwardmove != 0.0f || cur.sidemove != 0.0f || cur.upmove != 0.0f);
	if (s_xformLog <= 16 || (moving && (s_xformLog % 30) == 0) || (s_xformLog % 1000) == 0)
		SDK_Log("[BRIDGE-OUT] xform #%lld: emit=%d cmdNr=%u(prev=%u) fwd=%.3f side=%.3f up=%.3f buttons=0x%X "
			"frametime=%.5f(prev=%.5f) cmdtime=%.4f tick=%u move=%d cam=%d zip=%d\n",
			s_xformLog, w.GetNumBitsWritten() - emitBitsBefore,
			cur.commandNumber, prevCmdNr,
			cur.forwardmove, cur.sidemove, cur.upmove, cur.buttons,
			cur.frametime, prevFrametime, cur.commandTime, cur.baseSnapshotTickCount,
			moving ? 1 : 0,
			(int)cur.useCameraOverride, (int)cur.ziplinePresent);

	return true;
}


// Flush C2S when a clc_Move lands in s_c2sPend; the self-clock is PollReceive on the main thread.
static ConVar bridge_c2s_flush_on_move("bridge_c2s_flush_on_move", "1", FCVAR_RELEASE,
	"S21 bridge: flush the C2S packet immediately when a clc_Move is queued (native "
	"send cadence) instead of on the S2C-driven self-clock (60-120ms batches -> server "
	"timeBase rail corrections -> prediction error icon). Default 1.");

// Native C2S datagram never reaches the wire (Hook_sendto discards it); m_StreamReliable is local debt.
static ConVar bridge_c2s_reliable_drain("bridge_c2s_reliable_drain", "1", FCVAR_RELEASE,
	"S21 bridge: keep the native netchan's reliable stream empty (its packets never reach "
	"the wire; content is delivered by the message relay). Kills the waiting-list "
	"retransmit debt = the 1200B packet bloat = the rate-choke trigger. Default 1.");
bool S21Bridge_FlushC2SNow(const char* reason);

static bool S21Bridge_WriteFilteredSetConVar(bf_write& dst, bf_read& r, const int bodyBits)
{
	if (bodyBits < 8)
		return false;

	const int nBodyStart = r.GetNumBitsRead();
	const int nCount = r.ReadByte();
	if (nCount < 0 || nCount > 255)
		return false;

	char szName[260];
	char szValue[260];
	int nKeep = 0;
	for (int i = 0; i < nCount; ++i)
	{
		if (!r.ReadString(szName, sizeof(szName)) || !r.ReadString(szValue, sizeof(szValue)))
			return false;
		if (V_stricmp(szName, "sdk_mods") == 0)
			continue;
		++nKeep;
	}

	if (nKeep == 0)
		return true;

	if (!r.Seek(nBodyStart))
		return false;
	r.ReadByte();

	if (dst.GetNumBitsLeft() < 7 + 8 + bodyBits)
		return false;

	dst.WriteUBitLong(4, 7);
	dst.WriteByte(nKeep);
	for (int i = 0; i < nCount; ++i)
	{
		if (!r.ReadString(szName, sizeof(szName)) || !r.ReadString(szValue, sizeof(szValue)))
			return false;
		if (V_stricmp(szName, "sdk_mods") == 0)
			continue;
		dst.WriteString(szName);
		dst.WriteString(szValue);
	}

	return true;
}

// Append the captured C2S message at m_StreamUnreliable bits [bitStart, bitEnd)
// to s_c2sPend, rewriting its leading 7-bit type from s21Type to s3Type.
// uData/uBytes describe a read view over m_StreamUnreliable's buffer.
static void S21Bridge_RelayUnrelMsg(const uint8_t* uData, int uBytes,
                                    int bitStart, int bitEnd, int s21Type, int s3Type)
{
	const int totalBits = bitEnd - bitStart;
	if (totalBits < 7 || s3Type < 0)
		return;
	const int bodyBits = totalBits - 7;

	AcquireSRWLockExclusive(&s_c2sTxLock);
	if (s_c2sPend.GetNumBitsLeft() < totalBits + 8)
	{
		ReleaseSRWLockExclusive(&s_c2sTxLock);
		static long long s_ovf = 0;
		if (++s_ovf <= 5) Warning(eDLL_T::ENGINE, "[BRIDGE-C2S] s_c2sPend overflow, dropping msg s21=%d (%d bits)\n", s21Type, totalBits);
		return;
	}
	{
		bf_read r(uData, uBytes, bitEnd);
		if (!r.Seek(bitStart))
			goto unlock_out;
		const int gotType = r.ReadUBitLong(7);
		if (gotType != s21Type)
			goto unlock_out; // slice didn't start where we thought -- bail rather than corrupt

		// clc_Move: parse 4+3+16 header, walk per-cmd fields, append 2 trailing
		// zero bits per cmd, write new header with adjusted m_nLength.
		if (s3Type == kS3_clc_Move)
		{
			if (r.GetNumBitsLeft() < 23) goto unlock_out;
			const uint32_t newCmds      = r.ReadUBitLong(4);
			const uint32_t backupCmds   = r.ReadUBitLong(3);
			const uint32_t s21BodyBits  = r.ReadUBitLong(16);
			const int totalCmds         = (int)(newCmds + backupCmds);
			if (totalCmds <= 0 || totalCmds > 16 || s21BodyBits > 16384)
			{
				static long long s_bad = 0;
				if (++s_bad <= 5)
					Warning(eDLL_T::ENGINE, "[BRIDGE-OUT] clc_Move header insane: new=%u bk=%u len=%u\n",
						newCmds, backupCmds, s21BodyBits);
				goto unlock_out;
			}
			if (r.GetNumBitsLeft() < (int)s21BodyBits) goto unlock_out;

			// Diagnostic: dump first body row of S21 wire to allow hand decoding.
			{
				static long long s_dumpLog = 0;
				if (++s_dumpLog <= 3) {
					const int bitOff = (int)r.GetNumBitsRead();
					const int byteStart = bitOff >> 3;
					const int byteBit   = bitOff & 7;
					char hex[3 * 32 + 1] = {0};
					char* hp = hex;
					int nBytes = (int)((s21BodyBits + byteBit + 7) >> 3);
					if (nBytes > 32) nBytes = 32;
					for (int k = 0; k < nBytes; ++k) {
						int b = (byteStart + k < uBytes) ? uData[byteStart + k] : 0;
						hp += sprintf(hp, "%02X ", b);
					}
					Warning(eDLL_T::ENGINE, "[BRIDGE-OUT] s21body=%u, cmds=%d, body starts at byte=%d bit=%d, first %d bytes: %s\n",
						s21BodyBits, totalCmds, byteStart, byteBit, nBytes, hex);
				}
			}

			// Transform per cmd into scratch buffer (per-cmd budget ~600 bits typ.;
			// 16 cmds * 1024 bits = 2 KB ceiling, safely within stack budget).
			alignas(4) uint8_t scratch[2048] = {0};
			bf_write w("clc_Move_xform", scratch, sizeof(scratch));

			// Reset prev-cmd state at the start of each clc_Move batch. Both the S21
			// writer and the S3 ReadUserCmd loop use a CUserCmd::Reset() null cmd
			// as the baseline for the first cmd (backup or new).
			S21BridgeCmd::ResetToNullCmd(s_bridgeC2sPrevCmd);
			s_bridgeC2sPrevS3ImpulseWire = 0; // [S21-EXTRA-FLAGS] same per-batch null baseline

			// Mid-batch parse/emit failure: keep already-transcoded cmds; do not return the whole batch.
			int succeededCmds = 0;
			int completedBits = 0; // body bits through the LAST complete command
			for (int i = 0; i < totalCmds; ++i)
			{
				const ssize_t inBefore = r.GetNumBitsRead();
				const int outBefore = w.GetNumBitsWritten();
				if (!S21Bridge_TransformOneUsercmd(r, w))
				{
					static long long s_fail = 0;
					if (++s_fail <= 10)
						Warning(eDLL_T::ENGINE, "[BRIDGE-OUT] clc_Move xform FAILED cmd %d/%d "
							"(s21body=%u, inRead=%lld, outWritten=%d) -- salvaging %d "
							"already-transcoded cmd(s) instead of dropping the whole batch\n",
							i, totalCmds, s21BodyBits,
							(long long)r.GetNumBitsRead(), w.GetNumBitsWritten(), i);
					break;
				}
				succeededCmds = i + 1;
				completedBits = w.GetNumBitsWritten();
				{
					static long long s_cmdLog = 0;
					if (++s_cmdLog <= 20)
						SDK_Log("[BRIDGE-OUT] xform cmd %d/%d: in=%lld out=%d "
							"(pad=%d)\n",
							i, totalCmds,
							(long long)(r.GetNumBitsRead() - inBefore),
							w.GetNumBitsWritten() - outBefore,
							(w.GetNumBitsWritten() - outBefore) -
							(int)(r.GetNumBitsRead() - inBefore));
				}
			}
			if (succeededCmds <= 0) goto unlock_out;   // nothing salvageable -- unchanged from prior behavior

			// If a failure truncated the run short of totalCmds, the original
			// newCmds/backupCmds header values are now wrong for the body we
			uint32_t backupCmdsToSend = backupCmds;
			uint32_t newCmdsToSend    = newCmds;
			if (succeededCmds < totalCmds)
			{
				backupCmdsToSend = (succeededCmds < (int)backupCmds) ? (uint32_t)succeededCmds : backupCmds;
				newCmdsToSend    = (uint32_t)succeededCmds - backupCmdsToSend;
				static long long s_partial = 0;
				if (++s_partial <= 20)
					Warning(eDLL_T::ENGINE, "[BRIDGE-OUT] clc_Move PARTIAL SEND: %d/%d cmds salvaged "
						"(new %u->%u, backup %u->%u)\n",
						succeededCmds, totalCmds, newCmds, newCmdsToSend, backupCmds, backupCmdsToSend);
			}

			// Truncate at the last COMPLETE command: a mid-cmd emit failure can
			// leave partial bits in w; shipping them past the rewritten command
			// count makes the dedi parse them as the next message.
			const int s3BodyBits = completedBits;
			if (s3BodyBits < 0 || s3BodyBits > 65535) goto unlock_out;

			// Need room: 7 (type) + 4 (new) + 3 (backup) + 16 (m_nLength) + s3BodyBits
			const int s3TotalBits = 7 + 4 + 3 + 16 + s3BodyBits;
			if (s_c2sPend.GetNumBitsLeft() < s3TotalBits + 8)
			{
				ReleaseSRWLockExclusive(&s_c2sTxLock);
				static long long s_ovf2 = 0;
				if (++s_ovf2 <= 5)
					Warning(eDLL_T::ENGINE, "[BRIDGE-OUT] s_c2sPend overflow on clc_Move xform (%d bits)\n", s3TotalBits);
				return;
			}

			s_c2sPend.WriteUBitLong((unsigned)s3Type, 7);
			s_c2sPend.WriteUBitLong(newCmdsToSend, 4);
			s_c2sPend.WriteUBitLong(backupCmdsToSend, 3);
			s_c2sPend.WriteUBitLong((unsigned)s3BodyBits, 16);
			{
				bf_read scratchRead(scratch, sizeof(scratch), s3BodyBits);
				s_c2sPend.WriteBitsFromBuffer(&scratchRead, s3BodyBits);
			}

			static long long s_xformLog = 0;
			if (++s_xformLog <= 10 || (s_xformLog % 500) == 0)
				SDK_Log("[BRIDGE-OUT] clc_Move xform #%lld: cmds=%d/%d s21body=%u -> s3body=%d (+%d pad/cmd)\n",
					s_xformLog, succeededCmds, totalCmds, s21BodyBits, s3BodyBits,
					(s3BodyBits - (int)s21BodyBits) / succeededCmds);

			// [C2S-PACE] flush happens at the END of the SendDatagram relay scan (after the
			// trailing clc_ClientTick is also queued), NOT here -- see the hook site.
		}
		else if (s21Type == 5)
		{
			const int scvStartBit = s_c2sPend.GetNumBitsWritten();
			if (!S21Bridge_WriteFilteredSetConVar(s_c2sPend, r, bodyBits))
			{
				static long long s_scvFilt = 0;
				if (++s_scvFilt <= 8)
					Warning(eDLL_T::ENGINE, "[BRIDGE-C2S] dropped malformed net_SetConVar relay\n");
				goto unlock_out;
			}
			if (bridge_c2s_setconvar_redundancy.GetBool())
			{
				const int msgBits = s_c2sPend.GetNumBitsWritten() - scvStartBit;
				if (msgBits > 0 && ((msgBits + 7) >> 3) <= 1024)
				{
					SetConVarResend_s& slot = s_setConVarRing[s_nSetConVarRingHead];
					if (slot.active)
					{
						static long long s_scvDrop = 0;
						if ((++s_scvDrop % 256) == 1)
							Warning(eDLL_T::ENGINE,
								"[C2S-SCV] resend ring full, dropping oldest (#%lld)\n",
								s_scvDrop);
					}
					memset(slot.data, 0, sizeof(slot.data));
					bf_read src(s_c2sPend.GetData(), s_c2sPend.GetNumBytesWritten(),
						s_c2sPend.GetNumBitsWritten());
					src.Seek(scvStartBit);
					bf_write dst(slot.data, (int)sizeof(slot.data));
					dst.WriteBitsFromBuffer(&src, msgBits);
					slot.nBits = msgBits;
					slot.nextDueMs = static_cast<double>(GetTickCount64()) + 50.0;
					slot.active = true;
					s_nSetConVarRingHead = (s_nSetConVarRingHead + 1) % 4;
				}
			}
			static long long s_relayLog = 0;
			if (++s_relayLog <= 10 || (s_relayLog % 2000) == 0)
				SDK_Log("[BRIDGE-C2S] #%lld relayed s21=%d->s3=%d (%d body bits)\n",
					s_relayLog, s21Type, s3Type, bodyBits);
		}
		else
		{
			// Non-clc_Move: bit-for-bit pass-through.
			const int scvStartBit = s_c2sPend.GetNumBitsWritten();
			s_c2sPend.WriteUBitLong((unsigned)s3Type, 7);
			if (bodyBits > 0)
				s_c2sPend.WriteBitsFromBuffer(&r, bodyBits); // r is positioned at bitStart+7
			if (s3Type == 4 && bridge_c2s_setconvar_redundancy.GetBool())
			{
				const int msgBits = s_c2sPend.GetNumBitsWritten() - scvStartBit;
				if (msgBits > 0 && ((msgBits + 7) >> 3) <= 1024)
				{
					SetConVarResend_s& slot = s_setConVarRing[s_nSetConVarRingHead];
					if (slot.active)
					{
						static long long s_scvDrop = 0;
						if ((++s_scvDrop % 256) == 1)
							Warning(eDLL_T::ENGINE,
								"[C2S-SCV] resend ring full, dropping oldest (#%lld)\n",
								s_scvDrop);
					}
					memset(slot.data, 0, sizeof(slot.data));
					bf_read src(s_c2sPend.GetData(), s_c2sPend.GetNumBytesWritten(),
						s_c2sPend.GetNumBitsWritten());
					src.Seek(scvStartBit);
					bf_write dst(slot.data, (int)sizeof(slot.data));
					dst.WriteBitsFromBuffer(&src, msgBits);
					slot.nBits = msgBits;
					slot.nextDueMs = static_cast<double>(GetTickCount64()) + 50.0;
					slot.active = true;
					s_nSetConVarRingHead = (s_nSetConVarRingHead + 1) % 4;
				}
			}
			static long long s_relayLog = 0;
			if (++s_relayLog <= 10 || (s_relayLog % 2000) == 0)
				SDK_Log("[BRIDGE-C2S] #%lld relayed s21=%d->s3=%d (%d body bits)\n",
					s_relayLog, s21Type, s3Type, bodyBits);
		}
	}

unlock_out:
	ReleaseSRWLockExclusive(&s_c2sTxLock);
}

//-----------------------------------------------------------------------------
// Build a complete S3-format C2S netchannel packet.
//-----------------------------------------------------------------------------
int S21Bridge_BuildS3Packet(uint8_t* outBuf, int outBufSize,
	uint32_t seq, uint32_t ack)
{
	if (outBufSize < 128)
		return 0;

	bf_write send(outBuf, outBufSize);

	// === HEADER ===
	send.WriteLong(seq);
	send.WriteLong(ack);
	const int flagsBitPos = send.GetNumBitsWritten(); // save position for backpatch
	send.WriteUBitLong(0x00, 8); // placeholder, backpatched at end

	uint8_t flags = 0;

	// === NONCE SECTION ===
	// After receiving server's first subchannel packet, we must ACK its nonce.
	if (s_serverNonceCaptured && s_serverNonce != 0)
	{
		send.WriteUBitLong(1, 1);              // nonce_present = 1
		send.WriteUBitLong(0xFDBAC34D, 32);    // nonce magic

		// nonce_ack: 1 when we have a new nonce to ACK, 0 to reuse stored
		if (s_needNonceAck)
		{
			send.WriteUBitLong(1, 1);              // nonce_ack = 1
			send.WriteUBitLong(s_serverNonce, 32); // server's nonce value
			s_needNonceAck = false;                // clear after writing (matches a1+345=0)
		}
		else
		{
			send.WriteUBitLong(0, 1);              // nonce_ack = 0 (reuse stored)
		}

		// subchan_ack_seq: how many server subchannel entries we've received
		send.WriteUBitLong(s_serverSubSeqRecv, 32);
		send.WriteUBitLong(s_serverSubSeqRecv & 0x3FF, 10);

		static long long s_nonceLog = 0;
		if (++s_nonceLog <= 10)
			SDK_Log("[BRIDGE-C2S] nonce ACK: nonce=0x%08X subAck=%u\n",
				s_serverNonce, s_serverSubSeqRecv);
	}
	else
	{
		send.WriteUBitLong(0, 1); // nonce_present = 0
	}

	// === WRITESUBCHANNELDATA ===
	// Only written when we have reliable data to send. Sets flags |= 0x01.
	const bool haveReliable = false; // (s_reliableSize > 0 && !s_serverAckedUs);
	if (haveReliable)
	{
		flags |= 0x01;

		send.WriteUBitLong(0xABCDEF01, 32);       // subchannel magic
		send.WriteUBitLong(s_bridgeSubSeq, 32);    // subchannel sequence
		const uint32_t reqId = s_bridgeSubSeq & 0x3FF;
		send.WriteUBitLong(reqId, 10);             // request_id (low 10 bits)

		// req_id == 0 special path (always true when bridgeSubSeq=0)
		if (reqId == 0)
		{
			if (s_bridgeSubSeq == 0)
			{
				send.WriteUBitLong(1, 1);
				send.WriteUBitLong(s_bridgeNonceHost, 32);
			}
			else
			{
				send.WriteUBitLong(0, 1);
			}
		}

		// ENTRY: one fragment containing our reliable data
		// (No continuation bit for first entry)
		send.WriteUBitLong(s_bridgeSubSeq, 32);    // entry_seq
		send.WriteUBitLong(1, 1);                  // is_first_fragment = 1
		send.WriteUBitLong(s_reliableSize, 19);    // transfer_size
		send.WriteUBitLong(0, 1);                  // compressed = 0
		// Write fragment data
		send.WriteBits(s_reliableBuf, s_reliableSize * 8);

		// End of entries
		send.WriteUBitLong(0, 1);                  // more_entries = 0

		static long long s_subLog = 0;
		if (++s_subLog <= 20 || (s_subLog % 200) == 0)
			SDK_Log("[BRIDGE-C2S] #%lld subchan: seq=%u reqid=%u size=%d nonce=0x%08X\n",
				s_subLog, s_bridgeSubSeq, reqId, s_reliableSize, s_bridgeNonceHost);
	}
	// When no reliable data: do NOT write WriteSubChannelData at all.
	// flags bit 0 stays 0, server skips ReadSubChannelData.

	// === UNRELIABLE MESSAGES ===
	if (s_sendDisconnect)
	{
		send.WriteUBitLong(1, NETMSG_TYPE_BITS); // net_Disconnect
		send.WriteString("Disconnect by user.");
	}
	else
	{
	// Drain pending net_SignonState as UNRELIABLE; S3 sends no netchan during signon.
	{
		int drained = 0;
		while (drained < 8 && S21Bridge_HasPendingSignon())
		{
			if ((outBufSize * 8 - send.GetNumBitsWritten()) < (256 * 8))
				break;

			int sigState = 0;
			int sigSpawn = 0;
			if (!S21Bridge_DequeueSignon(&sigState, &sigSpawn))
				break;

			S21Bridge_WriteC2SSignonState(send, sigState, sigSpawn);

			if (S21Bridge_RewalkTraceSignon())
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-C2S] drain SignonState(%d) spawn=%d\n", sigState, sigSpawn);
			++drained;
		}

		const ULONGLONG echoNow = GetTickCount64();
		const bool bEchoOnce = (bridge_signon_echo_ms.GetInt() == 0);
		// Rungs up to SPAWN are requests: a higher S2C rung is the answer, so
		// they stay open until it arrives. FIRST_SNAP and FULL are the client's
		// own receipts and nothing comes back for them -- redundancy inside the
		// window is all an unreliable datagram needs.
		const bool bReceiptRung = (s_signonEchoState > static_cast<int>(SIGNONSTATE::SIGNONSTATE_SPAWN));
		const bool bEchoOpen = (!bEchoOnce && !bReceiptRung)
			|| (echoNow < s_signonEchoUntilMs);
		if (drained == 0 && s_signonEchoState >= 2
			&& bEchoOpen && echoNow >= s_signonEchoNextMs
			&& (outBufSize * 8 - send.GetNumBitsWritten()) >= (256 * 8))
		{
			S21Bridge_WriteC2SSignonState(send, s_signonEchoState, s_signonEchoSpawn);
			s_signonEchoNextMs = echoNow + kSignonEchoIntervalMs;

			// A park is unbounded in time, so the trace has to be bounded in
			// RATE, not in total lines: a line-count budget goes silent exactly
			++s_signonEchoSends;
			const bool bFirst = (s_signonEchoSends == 1);
			if (bFirst || echoNow >= s_signonEchoLogNextMs)
			{
				Warning(eDLL_T::ENGINE,
					"[BRIDGE-C2S] re-ask SignonState(%d) spawn=%d x%lld unanswered "
					"(parked %llu ms)\n",
					s_signonEchoState, s_signonEchoSpawn, s_signonEchoSends,
					echoNow - s_signonEchoArmedMs);
				s_signonEchoLogNextMs = echoNow + kSignonEchoLogIntervalMs;
			}

			// A re-ask that goes unanswered is normal for seconds. A re-ask
			// that goes unanswered while not one S2C packet has arrived since
			const int nDeadMs = bridge_signon_dead_ms.GetInt();
			const long nPsoNow = g_pPsoCreateCount ? *g_pPsoCreateCount : 0;
			if (!s_signonEchoDeadWarned && nDeadMs > 0
				&& s_bridgeInSeqNr == s_signonEchoArmedInSeq
				&& (echoNow - s_signonEchoArmedMs) >= (ULONGLONG)nDeadMs
				&& nPsoNow <= s_signonEchoArmedPso)
			{
				s_signonEchoDeadWarned = true;
				Error(eDLL_T::ENGINE, NO_ERROR,
					"Server stopped answering: re-asked signon rung %d for %llu ms "
					"and received no S2C packet at all. The dedicated server is "
					"unreachable or was shut down.\n",
					s_signonEchoState, echoNow - s_signonEchoArmedMs);
			}
		}
	}

	// === USERINFO (NET_SetConVar, S3 type 4) ===
	// After CONNECTED(2), the S3 server expects client ConVars (UserInfo).
	if (s_pendingUserInfo)
	{
		const ULONGLONG userInfoNow = GetTickCount64();
		if (s_userInfoSendsLeft <= 0)
		{
			S21Bridge_ClearUserInfo();
		}
		else if (s_userInfoNextMs != 0 && userInfoNow < s_userInfoNextMs)
		{
			// Hold the blob; retry on the next interval.
		}
		else
		{
			s_userInfoNextMs = userInfoNow + kBridgeUserInfoIntervalMs;

		// Essential ConVars from a real S3 client connection
		static const char* s_userInfoKV[][2] = {
			{"cl_updaterate_mp", "20"},
			{"cl_predict", "1"},
			{"cl_predictweapons", "1"},
			{"cl_lagcompensation", "1"},
			{"cl_fovScale", "1.54715"},
			{"net_maxroutable", "1200"},
			{"net_wifi", "0"},
			{"hasMic", "1"},
			{"persistence_clForceNew", "0"},
			{"match_partySize", ""},
			{"match_myDatacenter", "east us"},
			{"match_playlist", ""},
			{"match_partyChangeNum", ""},
			{"match_searching", "0"},
			{"match_myTeam", "0"},
			{"match_goodReputation", "1"},
			{"match_teamNoFill", "0"},
			{"match_myBestDatacenter", "east us"},
			{"match_myRankedDatacenter", "east us"},
			{"match_partySub", ""},
			{"voice_loopback", "0"},
			{"weapon_setting_autocycle_on_empty", "0"},
			// Pre-register these two so CClient::ProcessSetConVar's FindKey guard
			// accepts LATER runtime updates -- it silently
			{"laserSightColor", "0000"},
			{"laserSightColorCustomized", "0"},
			{"hasAnyAssetsWithDiscardedStreamableData", "0"},
			{"cl_isUnderAge", "0"},
			{"hasPartialInstall", "0"},
			{"inPartyChat", "0"},
			{"nucleus_id", "0"},
			{"nucleus_pid", "unknown"},
			{"pin_sid", "0:1776272811:2"},
			{"pin_plat_id", "0"},
			{"net_minimumPacketLossDC", "0"},
			{"community", ""},
			// The join token deliberately does NOT appear here. It rides in
			// C2S_CONNECT, which is the only delivery early enough for the server
			{"stryder_security", ""},
			{"locationInfo", "51"},
			{"locationInfo_nucleus", ""},
			{"twitch_prime_rewards", ""},
			// 14 FCVAR_USERINFO gap found via sdk_dump_userinfo_convars (entity_dump.cpp
			// walking g_pCVar's real registry -- 54 total FCVAR_USERINFO ConVars found
			{"player_setting_stickysprintforward", "0"},
			{"player_setting_holdtosprint", "0"},
			{"toggle_on_jump_to_deactivate", "1"},
			// Mantle-boost activation input (0=Off,1=Jump,2=Crouch,3=Movement/custom).
			// FCVAR_USERINFO on the client ConVar; pre-registered here so the S3 dedi's
			{"mantle_boost_input_setting", "2"},
			{"sdk_mods", "-"},
		};
		const int numVars = sizeof(s_userInfoKV) / sizeof(s_userInfoKV[0]);

		send.WriteUBitLong(4, NETMSG_TYPE_BITS); // NET_SetConVar (S3 type 4)
		send.WriteByte(numVars);
		for (int i = 0; i < numVars; ++i)
		{
			// [LIVE-USERINFO] s_userInfoKV[i][1] is a FALLBACK default, not the
			// value to send -- read the client's ACTUAL current ConVar at
			const char* value = s_userInfoKV[i][1];
			char szMods[MOD_ATTESTATION_MAX_LEN];
			if (V_strcmp(s_userInfoKV[i][0], "sdk_mods") == 0)
			{
				if (!ModSystem_BuildAttestation(szMods, sizeof(szMods)))
				{
					szMods[0] = '-';
					szMods[1] = '\0';
				}
				value = szMods;
			}
			else if (g_pCVar)
			{
				ConVar* const liveVar = g_pCVar->FindVar(s_userInfoKV[i][0]);
				if (liveVar)
				{
					const char* const liveStr = liveVar->GetString();
					if (liveStr && liveStr[0])
						value = liveStr;
				}
			}
			send.WriteString(s_userInfoKV[i][0]); // name
			send.WriteString(value);              // LIVE value (fallback: hardcoded default)
		}

		--s_userInfoSendsLeft;
		SDK_Log("[BRIDGE-C2S] sent NET_SetConVar with %d UserInfo ConVars (%d copies left)\n",
			numVars, s_userInfoSendsLeft);
		if (s_userInfoSendsLeft <= 0 || s_signonSeqMax >= 4)
			S21Bridge_ClearUserInfo();
		}
	}

	// === C2S GAMEPLAY INPUT (clc_Move / clc_ClientTick, type-translated) ===
	// Drain whatever the SendDatagram hook captured this cycle (Phase 1; per-msg gated by
	if (S21Bridge_C2S_AnyOn() && s_c2sPend.GetNumBitsWritten() > 0)
	{
		const int pendBits = s_c2sPend.GetNumBitsWritten();
		if (send.GetNumBitsLeft() >= pendBits + 8)
		{
			bf_read pr(s_c2sPend.GetData(), s_c2sPend.GetNumBytesWritten(), pendBits);
			pr.Seek(0);
			send.WriteBitsFromBuffer(&pr, pendBits);
			s_c2sPend.Reset();
			static long long s_drainLog = 0;
			if (++s_drainLog <= 10 || (s_drainLog % 2000) == 0)
				SDK_Log("[BRIDGE-C2S] #%lld drained %d input bits into S3 packet\n", s_drainLog, pendBits);
		}
		else
		{
			static long long s_drainOvf = 0;
			if (++s_drainOvf <= 5)
				Warning(eDLL_T::ENGINE, "[BRIDGE-C2S] outBuf too small for %d input bits -- holding for next flush\n", pendBits);
		}
	}
	} // !s_sendDisconnect

	if (!s_sendDisconnect && !send.IsOverflowed())
	{
		const long rttMs = s_bridgeReportedRttMs;
		if (rttMs >= 0 && send.GetNumBitsLeft() >= 72)
		{
			static int s_lastSentRttMs = -1;
			static double s_lastSentRttTime = 0.0;
			const double now = Plat_FloatTime();
			if (s_lastSentRttMs < 0
				|| (rttMs > s_lastSentRttMs ? rttMs - s_lastSentRttMs : s_lastSentRttMs - rttMs) >= 2
				|| (now - s_lastSentRttTime) >= 0.25)
			{
				uint8_t payload[6];
				payload[0] = 0x42; // 'BRT1' LE
				payload[1] = 0x54;
				payload[2] = 0x52;
				payload[3] = 0x31;
				payload[4] = static_cast<uint8_t>(rttMs & 0xFF);
				payload[5] = static_cast<uint8_t>((static_cast<unsigned>(rttMs) >> 8) & 0xFF);
				send.WriteUBitLong(68, NETMSG_TYPE_BITS); // net_ScriptMessage
				send.WriteOneBit(0);
				send.WriteShort(6);
				send.WriteBytes(payload, 6);
				s_lastSentRttMs = static_cast<int>(rttMs);
				s_lastSentRttTime = now;
			}
		}
	}

	// === PAD to minimum 16 bytes ===
	while ((send.GetNumBitsWritten() + 7) / 8 < 16)
		send.WriteUBitLong(0, 7); // NOP padding

	// Byte-align
	const int remainder = send.GetNumBitsWritten() % 8;
	if (remainder)
		send.WriteUBitLong(0, 8 - remainder);

	// === BACKPATCH FLAGS ===
	// Write the flags byte at the saved position
	{
		uint8_t* flagsByte = outBuf + (flagsBitPos / 8);
		const int bitOff = flagsBitPos % 8;
		// flags byte starts at bit 64 (byte 8), always byte-aligned
		if (bitOff == 0)
			*flagsByte = flags;
		else
		{
			// Shouldn't happen (flags is at byte boundary), but be safe
			*flagsByte = (*flagsByte & ((1 << bitOff) - 1)) | (flags << bitOff);
		}
	}

	if (send.IsOverflowed())
	{
		Warning(eDLL_T::ENGINE, "S21Bridge_BuildS3Packet: overflow!\n");
		return 0;
	}

	return (send.GetNumBitsWritten() + 7) / 8;
}

void S21Bridge_SendDisconnect(void)
{
	if (!s_bridgeActive || s_bridgeSocket == INVALID_SOCKET || !s_origSendto)
		return;

	s_sendDisconnect = true;
	const bool bSent = S21Bridge_FlushC2SNow("disconnect");
	s_sendDisconnect = false;
	Msg(eDLL_T::ENGINE, "[BRIDGE] net_Disconnect %s\n", bSent ? "sent" : "FAILED");
}

bool S21Bridge_FlushC2SNow(const char* reason)
{
	if (!s_bridgeActive || s_bridgeSocket == INVALID_SOCKET || !s_origSendto)
		return false;

	AcquireSRWLockExclusive(&s_c2sTxLock);
	S21Bridge_C2SRel_PumpLocked();

	const int pendingBitsBefore = s_c2sPend.GetNumBitsWritten();
	AcquireSRWLockExclusive(&s_signonQLock);
	const int pendingSignonCount = s_signonQCount;
	ReleaseSRWLockExclusive(&s_signonQLock);

	const int sbLevel = BridgeBudget_Level();
	static long s_c2sBudgetN = 0;
	const long sbN = (sbLevel > 0) ? InterlockedIncrement(&s_c2sBudgetN) : 0;
	const bool sbArm = BridgeBudget_Armed(sbLevel, sbN);
	LARGE_INTEGER sbFreq = {}, sbQ0 = {}, sbQBuild = {}, sbQSend = {}, sbQ1 = {};
	if (sbArm)
	{
		QueryPerformanceFrequency(&sbFreq);
		QueryPerformanceCounter(&sbQ0);
	}

	uint8_t s3pkt[8192];
	const uint32_t bridgeSeq = ++s_c2sSeqCounter;
	s_relaySendTime[bridgeSeq & 0x3FF] = Bridge_NetTime(); // for RTT (acked in ProcessPacket)

	// [ACK-XLATE] snapshot the engine's own outgoing sequence (CNetChan +4) at flush time
	// everything the engine emitted up to now is carried by this bridge packet, so when
	// the dedi acks bridgeSeq the engine may consider outSeq <= this value delivered.
	{
		AckXlate_s& x = s_ackXlate[bridgeSeq & 0x3FF];
		x.bridgeSeq = bridgeSeq;
		CNetChan* const pChan = s_bridgeChan;
		x.engineSeq = (pChan && s_bridgeActive) ? *(int*)((char*)pChan + 4) : -1;
	}
	const int s3len = S21Bridge_BuildS3Packet(s3pkt, sizeof(s3pkt),
		bridgeSeq, s_bridgeInSeqNr);
	if (sbArm)
		QueryPerformanceCounter(&sbQBuild);
	if (s3len <= 0)
	{
		ReleaseSRWLockExclusive(&s_c2sTxLock);
		return false;
	}

	const int sent = s_origSendto(s_bridgeSocket,
		reinterpret_cast<const char*>(s3pkt), s3len, 0,
		reinterpret_cast<const sockaddr*>(&s_bridgeDest),
		sizeof(s_bridgeDest));
	if (sbArm)
		QueryPerformanceCounter(&sbQSend);

	if (sent > 0)
		s_outBytesAcc += sent + 28; // wire bytes + UDP/IP overhead (real outgoing data rate)

	s_lastBridgeC2STime = GetTickCount64() / 1000.0;

	static long long s_flushLog = 0;
	if (++s_flushLog <= 20 || (s_flushLog % 2000) == 0)
	{
		SDK_Log("[BRIDGE-FLUSH] #%lld: reason=%s seq=%u ack=%u len=%d sent=%d pendingBits=%d signonQ=%d nonce=%d\n",
			s_flushLog, reason ? reason : "?",
			bridgeSeq, s_bridgeInSeqNr, s3len, sent,
			pendingBitsBefore, pendingSignonCount,
			s_serverNonceCaptured ? 1 : 0);
	}

	// UserInfo + first clc_Move can exceed one packet. Pend is kept when it
	// does not fit; send it alone now that UserInfo is off the wire.
	if (s_c2sPend.GetNumBitsWritten() > 0)
	{
		const uint32_t retrySeq = ++s_c2sSeqCounter;
		s_relaySendTime[retrySeq & 0x3FF] = Bridge_NetTime();
		AckXlate_s& rx = s_ackXlate[retrySeq & 0x3FF];
		rx.bridgeSeq = retrySeq;
		CNetChan* const pRetryChan = s_bridgeChan;
		rx.engineSeq = (pRetryChan && s_bridgeActive) ? *(int*)((char*)pRetryChan + 4) : -1;
		const int retryLen = S21Bridge_BuildS3Packet(s3pkt, sizeof(s3pkt),
			retrySeq, s_bridgeInSeqNr);
		if (retryLen > 0)
		{
			const int retrySent = s_origSendto(s_bridgeSocket,
				reinterpret_cast<const char*>(s3pkt), retryLen, 0,
				reinterpret_cast<const sockaddr*>(&s_bridgeDest),
				sizeof(s_bridgeDest));
			if (retrySent > 0)
				s_outBytesAcc += retrySent + 28;
		}
	}

	if (sbArm)
	{
		QueryPerformanceCounter(&sbQ1);
		Warning(eDLL_T::CLIENT, "[C2S-BUDGET] #%ld reason=%s seq=%u bits=%d len=%d "
			"build=%.2f send=%.2f retry=%.2f total=%.2f\n",
			sbN, reason ? reason : "?",
			bridgeSeq, pendingBitsBefore, s3len,
			BridgeBudget_Ms(sbQ0.QuadPart, sbQBuild.QuadPart, sbFreq.QuadPart),
			BridgeBudget_Ms(sbQBuild.QuadPart, sbQSend.QuadPart, sbFreq.QuadPart),
			BridgeBudget_Ms(sbQSend.QuadPart, sbQ1.QuadPart, sbFreq.QuadPart),
			BridgeBudget_Ms(sbQ0.QuadPart, sbQ1.QuadPart, sbFreq.QuadPart));
	}

	if (sent > 0)
		s_lastC2SFlushMs = static_cast<double>(GetTickCount64());

	const bool ok = (sent == s3len);
	ReleaseSRWLockExclusive(&s_c2sTxLock);
	return ok;
}

//-----------------------------------------------------------------------------
// C2S self-clock rides PollReceive on the main thread; a cold pak load blocks it.
//-----------------------------------------------------------------------------
static volatile LONG s_keepaliveStarted = 0;

static void S21Bridge_KeepaliveWorker(void)
{
	long long nFired = 0;
	ULONGLONG ullLogNextMs = 0;

	while (true)
	{
		Sleep(50);

		if (!s_bridgeActive
			|| s_bridgeSocket == INVALID_SOCKET || !s_origSendto)
			continue;

		S21Bridge_PumpWhileStalled();

		const int nGapMs = bridge_c2s_keepalive_ms.GetInt();
		if (nGapMs <= 0)
			continue;

		const ULONGLONG ullNow = GetTickCount64();
		const ULONGLONG ullSince = (s_lastC2SFlushMs == 0.0)
			? (ULONGLONG)nGapMs
			: (ullNow - static_cast<ULONGLONG>(s_lastC2SFlushMs));
		if (ullSince < static_cast<ULONGLONG>(nGapMs))
			continue;

		if (!S21Bridge_FlushC2SNow("stall-keepalive"))
			continue;

		++nFired;
		if (ullNow >= ullLogNextMs)
		{
			Warning(eDLL_T::ENGINE,
				"[BRIDGE-C2S] keepalive: main thread stalled %llu ms, sent C2S off-thread "
				"(x%lld total)\n", ullSince, nFired);
			ullLogNextMs = ullNow + 5000;
		}
	}
}

void S21Bridge_StartKeepaliveThread(void)
{
	if (InterlockedCompareExchange(&s_keepaliveStarted, 1, 0) != 0)
		return;

	std::thread(S21Bridge_KeepaliveWorker).detach();
	Warning(eDLL_T::ENGINE, "[BRIDGE-C2S] keepalive thread armed (gap %d ms)\n",
		bridge_c2s_keepalive_ms.GetInt());
}

// STRINGCMD_MAX_LEN is 512 in engine/client/client.cpp (not visible to the client product).
static constexpr int kBridgeStringCmdMaxLen = 512;

static bool S21Bridge_EmitStringCmdToPend(const char* pszCmd)
{
	if (!pszCmd)
		return false;
	const int nBits = 7 + static_cast<int>((strlen(pszCmd) + 1) * 8);
	AcquireSRWLockExclusive(&s_c2sTxLock);
	if (s_c2sPend.GetNumBitsLeft() < nBits + 8)
	{
		ReleaseSRWLockExclusive(&s_c2sTxLock);
		return false;
	}
	s_c2sPend.WriteUBitLong(3, 7);
	s_c2sPend.WriteString(pszCmd);
	ReleaseSRWLockExclusive(&s_c2sTxLock);
	return true;
}

static void S21Bridge_StoreStringCmdResend(uint32_t nSeq, const char* pszPrefixed)
{
	StringCmdResend_s& e = s_stringCmdRing[s_nStringCmdRingHead];
	e.seq = nSeq;
	V_strncpy(e.cmd, pszPrefixed, sizeof(e.cmd));
	e.sendsLeft = bridge_c2s_stringcmd_redundancy.GetInt();
	e.nextDueMs = static_cast<double>(GetTickCount64())
		+ static_cast<double>(bridge_c2s_stringcmd_resend_ms.GetFloat());
	s_nStringCmdRingHead = (s_nStringCmdRingHead + 1) % 32;
}

static bool S21Bridge_RelayStringCmdSeq(const uint8_t* d, int sliceBytes, int b0, int b1)
{
	char szOrig[1024];
	szOrig[0] = '\0';
	bf_read rb(d, sliceBytes, b1);
	if (!rb.Seek(b0 + 7) || !rb.ReadString(szOrig, sizeof(szOrig)))
	{
		static bool s_bReadFail = false;
		if (!s_bReadFail)
		{
			s_bReadFail = true;
			Warning(eDLL_T::ENGINE, "[BRIDGE-SCMD] failed to read stringcmd, sent legacy\n");
		}
		return false;
	}

	const int nLen = static_cast<int>(strlen(szOrig));
	if (nLen + 16 >= kBridgeStringCmdMaxLen)
	{
		static bool s_bTooLong = false;
		if (!s_bTooLong)
		{
			s_bTooLong = true;
			Warning(eDLL_T::ENGINE,
				"[BRIDGE-SCMD] cmd too long for seq lane, sent legacy (len=%d)\n", nLen);
		}
		return false;
	}

	const uint32_t nSeq = ++s_stringCmdSeq;
	char szPrefixed[1024];
	V_snprintf(szPrefixed, sizeof(szPrefixed), "brq %u %s", nSeq, szOrig);
	if (!S21Bridge_EmitStringCmdToPend(szPrefixed))
	{
		static bool s_bNoRoom = false;
		if (!s_bNoRoom)
		{
			s_bNoRoom = true;
			Warning(eDLL_T::ENGINE, "[BRIDGE-SCMD] s_c2sPend overflow, sent legacy\n");
		}
		return false;
	}

	S21Bridge_StoreStringCmdResend(nSeq, szPrefixed);
	return true;
}

static void S21Bridge_PumpStringCmdResends(void)
{
	if (!bridge_c2s_stringcmd_seq.GetBool())
		return;

	const double flNow = static_cast<double>(GetTickCount64());
	const double flSpacing = static_cast<double>(bridge_c2s_stringcmd_resend_ms.GetFloat());

	for (int i = 0; i < 32; ++i)
	{
		StringCmdResend_s& e = s_stringCmdRing[i];
		if (e.sendsLeft <= 0 || flNow < e.nextDueMs)
			continue;
		if (!S21Bridge_EmitStringCmdToPend(e.cmd))
			continue;

		static bool s_bAnnounced = false;
		if (!s_bAnnounced)
		{
			s_bAnnounced = true;
			Warning(eDLL_T::ENGINE,
				"[BRIDGE-SCMD] redundant-send lane ACTIVE (redundancy=%d spacing=%dms)\n",
				bridge_c2s_stringcmd_redundancy.GetInt(),
				bridge_c2s_stringcmd_resend_ms.GetInt());
		}

		--e.sendsLeft;
		e.nextDueMs = flNow + flSpacing;
	}
}

static void S21Bridge_PumpSetConVarResends(void)
{
	if (!bridge_c2s_setconvar_redundancy.GetBool())
		return;

	const double flNow = static_cast<double>(GetTickCount64());
	AcquireSRWLockExclusive(&s_c2sTxLock);
	for (int i = 0; i < 4; ++i)
	{
		SetConVarResend_s& e = s_setConVarRing[i];
		if (!e.active || flNow < e.nextDueMs)
			continue;
		if (e.nBits <= 0 || s_c2sPend.GetNumBitsLeft() < e.nBits + 8)
			continue;

		bf_read rb(e.data, (int)sizeof(e.data), e.nBits);
		s_c2sPend.WriteBitsFromBuffer(&rb, e.nBits);
		e.active = false;
		e.nBits = 0;
	}
	ReleaseSRWLockExclusive(&s_c2sTxLock);
}

//-----------------------------------------------------------------------------
// CNetChan::SendDatagram: when the bridge is active, build S3 netchannel headers.
//-----------------------------------------------------------------------------
int S21Bridge_Hook_SendDatagram(CNetChan* pChan, bf_write* pMsg)
{
	// HOOK-COST timer + [PIPELINE]/[SNAP-LEDGER]/[NET-LEDGER]
	// telemetry removed.

	// === C2S input capture (runs BEFORE the original, which flushes+resets the streams) ===
	// m_StreamUnreliable, in an active frame, is expected to be exactly
	if (s_bridgeActive && (s_bridgeChan == nullptr || pChan == s_bridgeChan))
	{
		// Heartbeat UNGATED (stage-1 close): logs rate/choked so a run
		// shows whether the engine's own rate holds without choking (it does now
		// that [C2S-RELIABLE] keeps packets ~90B).
		{
			static long long s_chokeHb = 0;
			if ((++s_chokeHb % 2000) == 0)
				SDK_Log("[C2S-PACE] hb: chokedPackets=%d rate=%u clearTime=%.3f\n",
					S21_NC_ChokedPackets(pChan),
					*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(pChan) + 0x2100),
					*reinterpret_cast<double*>(reinterpret_cast<char*>(pChan) + 0x2108));
		}

		// Engine bf_write layout (per S21 SendDatagram disasm): m_pData@+0,
		// m_nDataBytes@+8, m_nDataBits@+0xC, m_iCurBit@+0x10, m_bOverflow@+0x14.
		auto rawBits = [&](unsigned off) -> int {
			return *reinterpret_cast<int*>(reinterpret_cast<char*>(pChan) + off + 0x10);
		};
		auto rawData = [&](unsigned off) -> const uint8_t* {
			return *reinterpret_cast<uint8_t**>(reinterpret_cast<char*>(pChan) + off);
		};
		const int      relBits   = rawBits(0x2040);
		      int      unrelBits = rawBits(0x2078);
		const int      voiceBits = rawBits(0x20B0);
		// Sanity vs. uninit / garbage.
		if (unrelBits < 0 || unrelBits > 0x40000) unrelBits = 0;
		const uint8_t* uData = (unrelBits > 0) ? rawData(0x2078) : nullptr;
		const int      uBytes = ((unrelBits + 7) >> 3) + 4; // read-view; real buffer is larger

		// Diagnostic: UNCONDITIONAL (whether unrel/rel/voice are 0 or not).
		// Cross-checks raw vs. SDK-class reads so a layout mismatch is visible.
		// Also peeks the firstType (bit 0) and lastType (bit unrel-71) when there's room.
		{
			static long long s_diag = 0;
			if (++s_diag <= 30 || (s_diag % 1000) == 0)
			{
				bf_write* const uStreamSdk = S21_NC_StreamUnreliable(pChan);
				const int sdkUnrel = uStreamSdk ? uStreamSdk->GetNumBitsWritten() : -1;
				int t0 = -1, tLast = -1;
				if (unrelBits > 0 && uData)
				{
					bf_read r(uData, uBytes, unrelBits);
					if (r.Seek(0)) t0 = (int)r.ReadUBitLong(7);
					if (unrelBits >= kClcClientTickTotalBits && r.Seek(unrelBits - kClcClientTickTotalBits))
						tLast = (int)r.ReadUBitLong(7);
				}
				SDK_Log("[BRIDGE-C2S-DIAG] #%lld raw: rel=%d unrel=%d voice=%d | sdk_unrel=%d | firstType=%d lastType@-71=%d | s3PendQ=%d relay=%d (move=%d tick=%d) moveRelayed=%lld moveDropped=%lld moveRecovered=%lld\n",
					s_diag, relBits, unrelBits, voiceBits, sdkUnrel,
					t0, tLast, s_c2sPend.GetNumBitsWritten(),
					S21Bridge_C2S_AnyOn() ? 1 : 0,
					S21Bridge_C2S_ClcMoveOn() ? 1 : 0,
					S21Bridge_C2S_ClcTickOn() ? 1 : 0,
					s_moveRelayed, s_moveDropped, s_moveRecovered);
			}
		}

		// C2S relay -- clc_ClientTick is the delta-ack feedback channel and MUST reach
		// the dedi every transmit; a permanent miss freezes the dedi m_nDeltaAckTick ->
		if (S21Bridge_C2S_AnyOn() && uData && unrelBits > 0)
		{
			if (s_unrelTickB0 < 0 && unrelBits >= kClcClientTickTotalBits)
			{
				const int tb = unrelBits - kClcClientTickTotalBits;
				bf_read chk(uData, uBytes, unrelBits);
				int t0 = -1, tt = -1;
				if (chk.Seek(0))  t0 = (int)chk.ReadUBitLong(7);
				if (chk.Seek(tb)) tt = (int)chk.ReadUBitLong(7);
				if (tt == kS21_clc_ClientTick)
				{
					s_unrelTickB0 = tb;
					s_unrelTickB1 = unrelBits;
					if (s_unrelMoveB0 < 0 && t0 == kS21_clc_Move && tb >= 14)
					{
						s_unrelMoveB0 = 0;
						s_unrelMoveB1 = tb;
					}
				}
			}

			// A move is present but unplaceable when the stream holds more than a
			// bare clc_ClientTick and no slice was derived. That is lost or late
			// player input, so it is counted and announced, never silent.
			const bool bMoveSliceOk = (s_unrelMoveB0 >= 0
				&& s_unrelMoveB1 > s_unrelMoveB0 + 13);

			if (S21Bridge_C2S_ClcMoveOn() && bMoveSliceOk)
			{
				++s_moveRelayed;

				// A move that does not start at bit 0 is one the previous bit-0
				// heuristic threw away. Counting it is the only way to say what
				// that bug actually cost instead of estimating it.
				if (s_unrelMoveB0 > 0)
				{
					++s_moveRecovered;
					if (bridge_c2s_move_drop_log.GetBool()
						&& (s_moveRecovered == 1 || (s_moveRecovered % 500) == 0))
					{
						int tHead = -1;
						bf_read chk(uData, uBytes, unrelBits);
						if (chk.Seek(0)) tHead = (int)chk.ReadUBitLong(7);
						Warning(eDLL_T::ENGINE,
							"[C2S-MOVE] #%lld recovered a batch the bit-0 heuristic dropped "
							"(moveAt=%d firstType=%d relayed=%lld)\n",
							s_moveRecovered, s_unrelMoveB0, tHead, s_moveRelayed);
					}
				}

				static long long s_firstMove = 0;
				if (++s_firstMove == 1)
					SDK_Log("[BRIDGE-OUT] first clc_Move relayed: bits=%d s21t=%d s3t=%d (any=%d)\n",
						s_unrelMoveB1 - s_unrelMoveB0, kS21_clc_Move, kS3_clc_Move,
						S21Bridge_C2S_AnyOn() ? 1 : 0);
				S21Bridge_RelayUnrelMsg(uData, uBytes, s_unrelMoveB0, s_unrelMoveB1,
					kS21_clc_Move, kS3_clc_Move);
			}
			else if (S21Bridge_C2S_ClcMoveOn()
				&& unrelBits > kClcClientTickTotalBits + 13)
			{
				++s_moveDropped;
				if (bridge_c2s_move_drop_log.GetBool()
					&& (s_moveDropped == 1 || (s_moveDropped % 200) == 0))
				{
					int t0 = -1;
					bf_read chk(uData, uBytes, unrelBits);
					if (chk.Seek(0)) t0 = (int)chk.ReadUBitLong(7);
					Warning(eDLL_T::ENGINE,
						"[C2S-MOVE] #%lld usercmd batch NOT relayed -- no clc_Move slice "
						"(unrel=%d firstType=%d seenEnd=%d relayed=%lld)\n",
						s_moveDropped, unrelBits, t0, s_unrelSeenEnd, s_moveRelayed);
				}
			}

			if (S21Bridge_C2S_ClcTickOn() && s_unrelTickB0 >= 0)
			{
				const int tickBit = s_unrelTickB0;
				{
					static long long s_firstTick = 0;
					if (++s_firstTick == 1)
						SDK_Log("[BRIDGE-OUT] first clc_ClientTick relayed: bits=%d s21t=%d s3t=%d (any=%d)\n",
							s_unrelTickB1 - tickBit, kS21_clc_ClientTick, kS3_clc_ClientTick, S21Bridge_C2S_AnyOn() ? 1 : 0);

					// Parse clc_ClientTick body: [7 type][i32 m_nTick][i32 m_nRawTick].
					// S21 i32 encoding = WriteUBitLong(31)+WriteBit(sign); S3
					int32_t parsedTick = -2;
					int32_t parsedRawAck = -2;
					{
						bf_read tr(uData, uBytes, unrelBits);
						if (tr.Seek(tickBit + 7))
						{
							parsedTick   = (int32_t)tr.ReadUBitLong(32);
							parsedRawAck = (int32_t)tr.ReadUBitLong(32);
						}
					}
					// Change-driven + periodic on rawAck (the dedi's delta base).
					const LONG ackN = InterlockedIncrement(&s_tickAckLog);
					const bool bChanged = (!s_haveLastAck || parsedRawAck != s_lastRawAck);
					const bool bBackward = (s_haveLastAck
						&& parsedRawAck >= 0 && s_lastRawAck >= 0
						&& parsedRawAck < s_lastRawAck);
					const int prevRawAck = s_lastRawAck;
					s_lastRawAck  = parsedRawAck;
					s_haveLastAck = true;

					if (parsedTick == -1)
					{
						const LONG fullN = InterlockedIncrement(&s_ackFullReqLog);
						if (fullN == 1 || (fullN % 100) == 0)
							Warning(eDLL_T::CLIENT,
								"[ACK-FULLREQ] #%ld tick=-1 rawAck=%d -- dedi will answer "
								"with a NoDelta full update\n",
								fullN, parsedRawAck);
					}
					if (parsedTick != -2 && parsedRawAck != -2 && parsedRawAck < parsedTick)
					{
						const LONG invN = InterlockedIncrement(&s_ackInvariantLog);
						if (invN == 1 || (invN % 100) == 0)
							Warning(eDLL_T::CLIENT,
								"[ACK-INVARIANT] #%ld rawAck=%d < tick=%d -- stream slice "
								"is not clc_ClientTick as expected\n",
								invN, parsedRawAck, parsedTick);
					}

					if (bBackward)
						Warning(eDLL_T::CLIENT,
							"[ACK-REGRESS] #%ld clc_ClientTick rawAck %d -> %d "
							"(tick=%d) -- RAW ack regressed\n",
							ackN, prevRawAck, parsedRawAck, parsedTick);

					if (bChanged || ackN <= 30 || (ackN % 50) == 0)
						SDK_Log("[BRIDGE-OUT-ACK] #%ld clc_ClientTick: tick=%d rawAck=%d (outSeq=%d outAck=%d)\n",
							ackN, parsedTick, parsedRawAck,
							S21_NC_OutSeqNr(pChan), S21_NC_OutSeqNrAck(pChan));

					S21Bridge_RelayUnrelMsg(uData, uBytes, tickBit, s_unrelTickB1, kS21_clc_ClientTick, kS3_clc_ClientTick);
				}
			}

		}

		if (s_bridgeActive && !S21Bridge_HasPendingSignon())
		{
			S21Bridge_PumpStringCmdResends();
			S21Bridge_PumpSetConVarResends();
		}

		// Ship the just-queued Move+ClientTick at SendDatagram cadence, not the S2C-driven self-clock.
		if (bridge_c2s_flush_on_move.GetBool() && s_bridgeActive &&
			!S21Bridge_HasPendingSignon() && s_c2sPend.GetNumBitsWritten() > 0)
		{
			static bool s_bAnnounced = false;
			if (!s_bAnnounced)
			{
				s_bAnnounced = true;
				Warning(eDLL_T::ENGINE, "[C2S-PACE] ACTIVE -- flushing C2S per "
					"SendDatagram relay (native cadence), self-clock demoted to "
					"idle keepalive\n");
			}
			S21Bridge_FlushC2SNow("move-fill");
		}

		// The original resets the streams below, so the slice bookkeeping that
		// describes them retires here.
		S21Bridge_C2SUnrelSlices_ResetForDatagram();
	}

	// Diagnostic: log seq/ack state
	{
		static long long s_callCount = 0;
		if (++s_callCount <= 10 || (s_callCount % 5000) == 0)
		{
			const int outSeq = S21_NC_OutSeqNr(pChan);
			const int inSeq = S21_NC_InSeqNr(pChan);
			const int outAck = S21_NC_OutSeqNrAck(pChan);
			SDK_Log("[BRIDGE-SD] hook #%lld: seq=%d inSeq=%d outAck=%d bridge=%d s3PendQ=%d\n",
				s_callCount, outSeq, inSeq, outAck, s_bridgeActive ? 1 : 0,
				s_c2sPend.GetNumBitsWritten());
		}
	}

	// ALWAYS let the original S21 SendDatagram run. The S21 engine writes
	// tick messages INSIDE SendDatagram (not via pMsg or the stream buffers).
	{
		static long long s_fallthrough = 0;
		if (++s_fallthrough <= 5 || (s_fallthrough % 500) == 0)
		{
			const int seq = S21_NC_OutSeqNr(pChan);
			SDK_Log("[BRIDGE-SD] #%lld: falling through to original SendDatagram (seq=%d)\n",
				s_fallthrough, seq);
		}
	}
	// Outgoing flow stats are authored from real traffic in S21Bridge_Hook_FlowUpdate
	// (fed by the rolling counters in S21Bridge_Hook_ProcessPacket); the native
	// SendDatagram FlowUpdate runs through that hook and gets overwritten with truth.
	return CNetChan__SendDatagram(pChan, pMsg);
}

// CNetChan::FlowUpdate is the sole writer of per-flow avg stats. Bridge overwrites.
__int64 S21Bridge_Hook_FlowUpdate(CNetChan* pChan, int flow)
{
	const __int64 ret = CNetChan__FlowUpdate(pChan, flow);

	if (bridge_net_flow_reconcile.GetBool() && s_bridgeActive
		&& (s_bridgeChan == nullptr || pChan == s_bridgeChan))
	{
		char* const nc = reinterpret_cast<char*>(pChan);
		for (int f = 0; f < 2; ++f)
		{
			char* const fb = nc + (size_t)f * 0x840;
			*reinterpret_cast<float*>(fb + 0x21FC) = s_flowStat[f].avgbytes;
			*reinterpret_cast<float*>(fb + 0x2200) = s_flowStat[f].avgpackets;
			*reinterpret_cast<float*>(fb + 0x2204) = s_flowStat[f].avgloss;
			*reinterpret_cast<float*>(fb + 0x2208) = s_flowStat[f].avgchoke;
			*reinterpret_cast<float*>(fb + 0x220C) = s_flowStat[f].avglatency; // avglatency (ping)
		}
	}
	return ret;
}

// Connection ping for UI (GetConnectionPingMs); avglatency at netchan+0x220C.
static int (*v_NET_GetSPing)(void) = nullptr;

static void S21_ResolveNetGetSPing(void)
{
	if (v_NET_GetSPing)
		return;
	// load netchan; test; cmp signon FULL; movss xmm0,[rax+0x220C]
	Module_FindPattern(g_GameDll,
		"48 8B 05 ?? ?? ?? ?? 48 85 C0 74 4F 83 3D ?? ?? ?? ?? 08 75 46 "
		"F3 0F 10 80 0C 22 00 00")
		.GetPtr(v_NET_GetSPing);
	if (!v_NET_GetSPing)
		Warning(eDLL_T::ENGINE, "[BRIDGE-PING] NET_GetSPing pattern unresolved\n");
}

int S21Bridge_GetConnectionPingMs(void)
{
	S21_ResolveNetGetSPing();
	if (!v_NET_GetSPing)
		return 0;

	const int sping = v_NET_GetSPing();
	if (sping <= 0)
		return 0; // offline / not FULL -- engine returns 0
	return (sping > 9999) ? 9999 : sping;
}

// CNetChan::SendNetMsg diagnostic: types that route here vs inline CL_Move writes.
char S21Bridge_Hook_SendNetMsg(void* pChan, void* pMsg, char bForceReliable, char bVoice)
{
	// DIAGNOSTIC HOOK -- announces every SendNetMsg call regardless of the
	// per-message C2S relay convars. The hook itself is a pure pass-through
	if (!s_bridgeActive || (s_bridgeChan != nullptr && pChan != s_bridgeChan))
		return CNetChan__SendNetMsg(pChan, pMsg, bForceReliable, bVoice);

	// Read all three streams' m_iCurBit via RAW offsets (bypass any potential
	// SDK bf_write layout mismatch). Engine layout: stream@netchan+{0x2040 rel,
	// 0x2078 unrel, 0x20B0 voice}, m_iCurBit at +0x10 within each.
	auto rawBits = [&](unsigned off) -> int {
		return *reinterpret_cast<int*>(reinterpret_cast<char*>(pChan) + off + 0x10);
	};
	auto rawData = [&](unsigned off) -> const uint8_t* {
		return *reinterpret_cast<uint8_t**>(reinterpret_cast<char*>(pChan) + off);
	};
	const int b0u = rawBits(0x2078), b0r = rawBits(0x2040), b0v = rawBits(0x20B0);
	const char ret = CNetChan__SendNetMsg(pChan, pMsg, bForceReliable, bVoice);
	const int b1u = rawBits(0x2078), b1r = rawBits(0x2040), b1v = rawBits(0x20B0);

	// Determine which stream the engine just wrote this message into, and the
	// [b0, b1) bit slice it occupies. After the original SendNetMsg runs, the slice
	// simpler and safer than re-serializing via WriteToBuffer.
	int s21Type = -1, grewBits = 0;
	const char* which = "none";
	const uint8_t* d = nullptr; int b0 = 0, b1 = 0;
	if (b1u > b0u) { which = "unrel"; d = rawData(0x2078); b0 = b0u; b1 = b1u; }
	else if (b1r > b0r) { which = "rel"; d = rawData(0x2040); b0 = b0r; b1 = b1r; }
	else if (b1v > b0v) { which = "voice"; d = rawData(0x20B0); b0 = b0v; b1 = b1v; }
	if (d && (b1 - b0) >= 7)
	{
		grewBits = b1 - b0;
		bf_read r(d, ((b1 + 7) >> 3) + 4, b1);
		if (r.Seek(b0)) s21Type = (int)r.ReadUBitLong(7);
	}

	// [C2S-SLICE] record where each unreliable message actually sits, so
	// SendDatagram relays by exact extent instead of guessing by position.
	if (d && b1u > b0u && b1 > b0)
	{
		// A gap since the last observed write is content the engine wrote
		// directly -- that is clc_Move, and nothing else takes that path.
		if (b0 > s_unrelSeenEnd && (b0 - s_unrelSeenEnd) >= 7)
		{
			bf_read gap(d, ((b0 + 7) >> 3) + 4, b0);
			if (gap.Seek(s_unrelSeenEnd)
				&& (int)gap.ReadUBitLong(7) == kS21_clc_Move)
			{
				s_unrelMoveB0 = s_unrelSeenEnd;
				s_unrelMoveB1 = b0;
			}
		}

		if (s21Type == kS21_clc_Move)
		{
			// Not the path the engine takes today; kept so a build that starts
			// routing the move through SendNetMsg does not silently lose it.
			s_unrelMoveB0 = b0;
			s_unrelMoveB1 = b1;
		}
		else if (s21Type == kS21_clc_ClientTick)
		{
			s_unrelTickB0 = b0;
			s_unrelTickB1 = b1;
		}

		s_unrelSeenEnd = b1;
	}

	// [C2S-SR-RAW] verified capture for the open "drop item sends wrong
	// value" investigation. Logs

	// [1b sign][10b funcIndex][32b reserved/null-target-entity][args...] -- 66-bit header.
	// Reserved field is WriteUBitLong(0,31) plus one inlined zero bit; skipping it shifts arg bits and doubles decoded values.
	if (s21Type == 4 && d && grewBits >= 66)
	{
		bf_read sr(d, ((b1 + 7) >> 3) + 4, b1);
		if (sr.Seek(b0))
		{
			sr.ReadUBitLong(7);
			const unsigned flag = sr.ReadOneBit();
			const unsigned payloadByteLen = sr.ReadUBitLong(15);
			const unsigned sign = sr.ReadOneBit();
			const unsigned funcIndex = sr.ReadUBitLong(10);
			sr.ReadUBitLong(31); // reserved/null-target-entity, low 31 bits
			sr.ReadOneBit();     // its 32nd bit (inlined WriteOneBit(0) in the native)

			const int argBits = grewBits - 66;
			char hex[128]; hex[0] = 0;
			int nBytes = (argBits > 0) ? ((argBits + 7) / 8) : 0;
			if (nBytes > 32) nBytes = 32;
			int o = 0;
			for (int k = 0; k < nBytes && o < (int)sizeof(hex) - 4; ++k)
			{
				const int bitsLeft = argBits - k * 8;
				const int take = bitsLeft >= 8 ? 8 : bitsLeft;
				o += snprintf(hex + o, sizeof(hex) - o, "%02x ",
					(unsigned)sr.ReadUBitLong(take) & 0xFF);
			}

			static long long s_srRawLog = 0;
			if (++s_srRawLog <= 50 || (s_srRawLog % 500) == 0)
				SDK_Log("[C2S-SR-RAW] #%lld funcIndex=%u flag=%u payloadByteLen=%u sign=%u argBits=%d argBytes[ %s]\n",
					s_srRawLog, funcIndex, flag, payloadByteLen, sign, argBits, hex);
		}
	}

	// Native C2S datagram is suppressed in Hook_sendto; relay captured messages here.
	if (s21Type >= 0 && d && grewBits >= 7)
	{
		const int s3Type = S21Bridge_C2S_MsgRelayS3Type(s21Type);
		if (s3Type >= 0)
		{
			const int sliceBytes = ((b1 + 7) >> 3) + 4;
			bool bSeqRelayed = false;
			if (s21Type == 3 && s3Type == 3 && bridge_c2s_stringcmd_seq.GetBool())
				bSeqRelayed = S21Bridge_RelayStringCmdSeq(d, sliceBytes, b0, b1);
			else if (s21Type == 4 && s3Type == 68)
				bSeqRelayed = S21Bridge_C2SRel_RelayScriptRemote(d, sliceBytes, b0, b1);
			else if (s21Type == 66 && s3Type == 61)
				bSeqRelayed = S21Bridge_C2SRel_RelayChat(d, sliceBytes, b0, b1);
			if (!bSeqRelayed)
				S21Bridge_RelayUnrelMsg(d, sliceBytes, b0, b1, s21Type, s3Type);
			static long long s_relayMsgLog = 0;
			if (++s_relayMsgLog <= 20 || (s_relayMsgLog % 1000) == 0)
				SDK_Log("[BRIDGE-C2S] SendNetMsg relayed s21=%d->s3=%d (%d bits, stream=%s)\n",
					s21Type, s3Type, grewBits, which);
		}
	}

	// Truncate the message back out of m_StreamReliable: native C2S never reaches the wire.
	if (bridge_c2s_reliable_drain.GetBool() && b1r > b0r)
	{
		*reinterpret_cast<int*>(reinterpret_cast<char*>(pChan) + 0x2040 + 0x10) = b0r;
		static long long s_drainLog = 0;
		const int s3Mapped = (s21Type >= 0) ? S21Bridge_C2S_MsgRelayS3Type(s21Type) : -1;
		if (++s_drainLog <= 10 || (s_drainLog % 500) == 0 || s3Mapped < 0)
		{
			static long long s_unrelayedLog = 0;
			if (s3Mapped >= 0 || ++s_unrelayedLog <= 20)
				SDK_Log("[C2S-RELIABLE] #%lld truncated %d reliable bits s21Type=%d (%s)\n",
					s_drainLog, b1r - b0r, s21Type,
					s3Mapped >= 0 ? "relayed" : "UNRELAYED -- content dropped");
		}
	}

	// [CHAT-HUNT] Log every C2S message that is NOT clc_Move(57)/clc_ClientTick(65).
	// Those two are per-tick; everything else (commands, convars, chat, script RPCs) is
	if (s21Type >= 0 && s21Type != 57 && s21Type != 65)
	{
		char peek[64]; peek[0] = 0;
		if (d && grewBits > 7)
		{
			bf_read rb(d, ((b1 + 7) >> 3) + 4, b1);
			if (rb.Seek(b0 + 7))
			{
				int n = (grewBits - 7) / 8; if (n > 16) n = 16;
				int o = 0;
				for (int k = 0; k < n && o < (int)sizeof(peek) - 4; ++k)
					o += snprintf(peek + o, sizeof(peek) - o, "%02x ", (unsigned)rb.ReadUBitLong(8) & 0xFF);
			}
		}
		SDK_Log("[CHAT-HUNT] C2S s21Type=%d stream=%s grewBits=%d fReliable=%d s3MapGuess=%d body[ %s]\n",
			s21Type, which, grewBits, (int)bForceReliable, S21Bridge_C2S_MsgRelayS3Type(s21Type), peek);
	}

	static long long s_log = 0;
	if (++s_log <= 30 || (s_log % 2000) == 0)
		SDK_Log("[BRIDGE-C2S-DIAG] #%lld SendNetMsg fReliable=%d fVoice=%d wroteTo=%s grew=%dbits s21Type=%d\n",
			s_log, (int)bForceReliable, (int)bVoice, which, grewBits, s21Type);
	return ret;
}

double S21BridgeDiag_MsSinceC2SFlush(void)
{
	if (s_lastC2SFlushMs == 0.0)
		return -1.0;
	return static_cast<double>(GetTickCount64()) - s_lastC2SFlushMs;
}
