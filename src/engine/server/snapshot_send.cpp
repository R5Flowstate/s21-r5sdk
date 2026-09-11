//=============================================================================//
//
// Purpose: Live CClient::SendSnapshot / CServer::SendClientMessages path.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/shared/s21_bridge_compat.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "engine/net_chan.h"
#include "engine/tick_budget.h"
#include "snapshot_diag.h"
#include "snapshot_send.h"
#include "tier0/memvalidate.h"

#include <cstdint>
#include <cstdio>
#include <atomic>
#include <climits>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

extern CGlobalVars* gpGlobals;
class CServerGameEnts;
extern CServerGameEnts* g_pServerGameEntities;

//-----------------------------------------------------------------------------
// +0x3C8 m_nDeltaAckTick, +0x59C waitTick, +0x3B0 signonState, +0x10 slot.
//-----------------------------------------------------------------------------
ConVar sdk_bridge_activate_player("sdk_bridge_activate_player", "1",
	FCVAR_RELEASE,
	"[BRIDGE-ACTIVATE] One-shot: call CClient::ActivatePlayer for the bridge slot when "
	"signonState >= 6. Fixes the server-side player-spawn callback the bridge skips. 1=on.");
// One NoDelta FULL, then waitTick keepalive. Do not clear bSendSnapshots.
static std::atomic<int>      s_pacedAwaitTick{ -1 };
static std::atomic<uint64_t> s_pacedArmQpc{ 0 };
static std::atomic<int>      s_pacedSkipRearm{ 0 };
static std::atomic<uintptr_t> s_lastActivatedClient{ 0 };

static ConVar sdk_bridge_full_pace("sdk_bridge_full_pace", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"1 = one NoDelta FULL then waitTick-keepalive until the client acks that tick. "
	"0 = legacy per-tick (the storm).");

// Safety-release if the in-flight FULL is unacked this long. Must exceed one full decode.
static ConVar sdk_bridge_full_pace_safety_ms("sdk_bridge_full_pace_safety_ms", "30000",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Max ms to HOLD an unacked FULL for the bridge slot before assuming the ack is lost and "
	"re-bootstrapping. Must exceed the client's worst-case single full-decode time.");

// Pin waitTick to the ring head when the delta ack is frozen.
struct EarlyPaceSlot_t
{
	std::atomic<int>      nFrozenAck{ -1 };
	std::atomic<uint64_t> nArmQpc{ 0 };
	// Ack the client must pass before a hold may re-arm. Set on safety-release
	// and on FULL-PACE release so the resync gets a turn instead of being
	// immediately re-held on the very ack that ended the previous hold.
	std::atomic<int>      nRearmBlockAck{ -1 };
	// Frozen means the ack has not moved for a wall-clock window.
	std::atomic<int>      nLastAck{ -1 };
	std::atomic<uint64_t> nLastAckQpc{ 0 };
};
static EarlyPaceSlot_t s_earlyPace[64];

static ConVar bridge_full_pace_stale_ticks("bridge_full_pace_stale_ticks", "30",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Keepalive-hold a bridge client whose delta ack is this many ticks behind the "
	"snapshot ring head (downlink stall). Caps what one frozen client costs the sim "
	"frame. 0 = disable.", true, 0.f, true, 160.f);

// Unbounded hold is a freeze; past this bound drop it and force a FULL.
static ConVar bridge_early_pace_safety_ms("bridge_early_pace_safety_ms", "1500",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Max ms to keepalive-hold an ack-frozen client before releasing it to the "
	"forced-FULL resync path. Should not exceed the snapshot ring's span in ms.",
	true, 250.f, true, 60000.f);

// Ack must sit unchanged this long before a hold may arm.
static ConVar bridge_early_pace_stale_ms("bridge_early_pace_stale_ms", "750",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Ms the delta ack must stay unchanged before a keepalive-hold may arm. "
	"0 = arm on ring-age alone (pre-liveness behaviour).",
	true, 0.f, true, 10000.f);

// Off by default: log I/O on the sim thread.
static ConVar bridge_ack_age_heartbeat("bridge_ack_age_heartbeat", "0",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Print [ACK-AGE] every N snapshots for the bridge slot: current and peak "
	"baseline age behind the ring head. 20 = once a second. 0 = off.",
	true, 0.f, true, 200.f);

static long long EarlyPace_HeldMs(uint64_t armQpc)
{
	if (!armQpc)
		return 0;
	LARGE_INTEGER nowQpc, qpcFreq;
	QueryPerformanceCounter(&nowQpc);
	QueryPerformanceFrequency(&qpcFreq);
	if (qpcFreq.QuadPart <= 0)
		return 0;
	return (long long)((nowQpc.QuadPart - (LONGLONG)armQpc) * 1000LL / qpcFreq.QuadPart);
}

static ConVar sdk_snap_send_qpc("sdk_snap_send_qpc", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"1 = QPC around CClient::SendSnapshot for [ENGINE-LONG]. 0 = off.");

static ConVar sdk_snap_tx_diag("sdk_snap_tx_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[SNAP-TX-1S] per-second SendSnapshot calls, keepalive holds and encodes for slot 0. 0 = off.");
static ConVar sdk_snap_budget("sdk_snap_budget", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"[SNAP-SEND] QPC around SendSnapshot and SendClientMessages. "
	"0 = off, 1 = every 20th, 2 = every call.", true, 0.f, true, 2.f);

static bool SnapBudget_Armed(int level, long n)
{
	return (level >= 2) || (level == 1 && (n % 20) == 0);
}

static double SnapBudget_Ms(LONGLONG a, LONGLONG b, LONGLONG freq)
{
	if (freq <= 0)
		return 0.0;
	return 1000.0 * (double)(b - a) / (double)freq;
}

// +0x3C8 m_nDeltaAckTick, +0x3D4 m_lastSnapshotTick. a3 is the real ack tick.

//-----------------------------------------------------------------------------
// BitpackDelta (vtbl 19). Native nTick == nRawTick; do not restamp.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t kBitpackDeltaSlot = 19;

// Exec count captured at BitpackDelta per slot (SNAP-ANCHOR probe).
std::atomic<int> g_bridgePackExecRing[128] = {};

// Wire nRawTick is delta+0x20, not necessarily gpGlobals->tickCount.
static constexpr ptrdiff_t SNAP_DELTA_OFF_TO_RAW = 0x20;   // wire nRawTick
static std::atomic<int> s_bridgeWireTickBias{ 0 };
static std::atomic<bool> s_bridgeWireTickSeen{ false };

int Bridge_GetWireSnapshotTick(void)
{
	if (!gpGlobals)
		return 0;
	return gpGlobals->tickCount + s_bridgeWireTickBias.load(std::memory_order_relaxed);
}

// serverGameEnts->BitpackDelta original (vtbl slot 19): bool(this, slot, delta, bf_write, info).
static bool (__fastcall* v_SGE_BitpackDelta)(int64_t, uint32_t, int64_t, int64_t, int64_t) = nullptr;
static std::atomic<bool> s_bitpackHooked{ false };

// Read-only: info+0x14 cmdRun vs executed-cmd anchors. Never writes info.
static ConVar bridge_snap_anchor_probe("bridge_snap_anchor_probe", "0", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"[SNAP-ANCHOR] read-only per-pack probe: engine cmdRun stamp (info+0x14, last RECEIVED) vs "
	"executed-cmd anchors (execNow/execPrev), stamp-repeat rate. Default 0.");

static ConVar bridge_snap_phase("bridge_snap_phase", "0",
	FCVAR_DEVELOPMENTONLY,
	"[SNAP-PHASE] QPC probe: SendClientMessages entry/orig/wait vs BitpackDelta "
	"timing (sync vs async drift). 0 = off.");

static std::atomic<LONGLONG> s_snapPhaseScmEntryQpc{ 0 };
static std::atomic<LONGLONG> s_snapPhaseScmEntryFreq{ 0 };

static bool __fastcall Hook_SGE_BitpackDelta(int64_t thisptr, uint32_t slot,
	int64_t delta, int64_t bfWrite, int64_t info)
{
	// [S2C-SR-TICK] publish the wire<->tickCount bias for the S->C ScriptRemote stamp.
	if (delta && gpGlobals)
	{
		const int nWireRaw = *reinterpret_cast<const int*>(delta + SNAP_DELTA_OFF_TO_RAW);
		const int nBias = nWireRaw - gpGlobals->tickCount;
		s_bridgeWireTickBias.store(nBias, std::memory_order_relaxed);

		if (!s_bridgeWireTickSeen.exchange(true, std::memory_order_relaxed))
			Msg(eDLL_T::SERVER, "[S2C-SR-TICK] wire nRawTick=%d tickCount=%d bias=%d\n",
				nWireRaw, gpGlobals->tickCount, nBias);
	}

	if (slot < 128)
	{
		extern std::atomic<int> g_bridgeLastExecCmdNumber[MAX_PLAYERS];
		const int nExec = (slot < MAX_PLAYERS)
			? g_bridgeLastExecCmdNumber[slot].load(std::memory_order_relaxed) : 0;
		const int nPrevPackExec = g_bridgePackExecRing[slot].load(std::memory_order_relaxed);
		g_bridgePackExecRing[slot].store(nExec, std::memory_order_relaxed);

		if (bridge_snap_anchor_probe.GetBool() && info)
		{
			const int nEngine = *reinterpret_cast<const int*>(info + 0x14);
			static int s_nPrevEngine[128] = {};
			static int s_nRepeatCount[128] = {};
			const bool bRepeat = (nEngine > 0 && nEngine == s_nPrevEngine[slot]);
			s_nPrevEngine[slot] = nEngine;
			if (bRepeat) ++s_nRepeatCount[slot];

			// Per-pack line: slots 0..3 only; first 32 packs then every 64th; repeats ALWAYS (capped 200 total).
			static int s_nPackLog[4] = {};
			static int s_nRepeatLog = 0;
			if (slot < 4)
			{
				const int nLog = s_nPackLog[slot]++;
				const bool bWant = (nLog < 32) || ((nLog % 64) == 0) || (bRepeat && s_nRepeatLog < 200);
				if (bRepeat) ++s_nRepeatLog;
				if (bWant)
					Msg(eDLL_T::SERVER, "[SNAP-ANCHOR] slot=%u engine=%d execNow=%d execPrev=%d dNow=%d dPrev=%d%s\n",
						slot, nEngine, nExec, nPrevPackExec, nEngine - nExec, nEngine - nPrevPackExec,
						bRepeat ? " REPEAT" : "");
			}

			// Slot-0 windowed summary every 128 packs: min/max/avg of dNow and dPrev + repeat count.
			if (slot == 0)
			{
				static int s_nWin = 0, s_nRepWin = 0;
				static int s_dNowMin = INT_MAX, s_dNowMax = INT_MIN; static long long s_dNowSum = 0;
				static int s_dPrevMin = INT_MAX, s_dPrevMax = INT_MIN; static long long s_dPrevSum = 0;
				const int dNow = nEngine - nExec, dPrev = nEngine - nPrevPackExec;
				++s_nWin; if (bRepeat) ++s_nRepWin;
				s_dNowSum += dNow;  if (dNow  < s_dNowMin)  s_dNowMin  = dNow;  if (dNow  > s_dNowMax)  s_dNowMax  = dNow;
				s_dPrevSum += dPrev; if (dPrev < s_dPrevMin) s_dPrevMin = dPrev; if (dPrev > s_dPrevMax) s_dPrevMax = dPrev;
				if (s_nWin >= 128)
				{
					Msg(eDLL_T::SERVER,
						"[SNAP-ANCHOR] slot=0 win=%d dNow{min=%d max=%d avg=%.2f} dPrev{min=%d max=%d avg=%.2f} repeats=%d\n",
						s_nWin, s_dNowMin, s_dNowMax, (double)s_dNowSum / s_nWin,
						s_dPrevMin, s_dPrevMax, (double)s_dPrevSum / s_nWin, s_nRepWin);
					s_nWin = 0; s_nRepWin = 0;
					s_dNowMin = INT_MAX; s_dNowMax = INT_MIN; s_dNowSum = 0;
					s_dPrevMin = INT_MAX; s_dPrevMax = INT_MIN; s_dPrevSum = 0;
				}
			}
		}
	}

	if (bridge_snap_phase.GetBool() && slot < 4)
	{
		static long s_packPhaseN[4] = {};
		const long n = ++s_packPhaseN[slot];
		if ((n % 20) == 0)
		{
			LARGE_INTEGER now = {};
			QueryPerformanceCounter(&now);
			const LONGLONG entry = s_snapPhaseScmEntryQpc.load(std::memory_order_relaxed);
			const LONGLONG freq = s_snapPhaseScmEntryFreq.load(std::memory_order_relaxed);
			int64_t usSinceScm = 0;
			if (freq > 0 && entry != 0)
				usSinceScm = (now.QuadPart - entry) * 1000000LL / freq;
			Msg(eDLL_T::SERVER,
				"[SNAP-PHASE] pack slot=%u tid=%u qpc=%lld\n",
				slot, GetCurrentThreadId(), (long long)usSinceScm);
		}
	}

	return v_SGE_BitpackDelta(thisptr, slot, delta, bfWrite, info);
}

static void SnapPack_EnsureHooked()
{
	if (s_bitpackHooked.load(std::memory_order_acquire))
		return;
	if (!g_pServerGameEntities)
		return;
	bool expected = false;
	if (!s_bitpackHooked.compare_exchange_strong(expected, true))
		return;

	const uintptr_t vtbl     = *reinterpret_cast<uintptr_t*>(g_pServerGameEntities);
	const uintptr_t origSlot = *reinterpret_cast<uintptr_t*>(
		vtbl + kBitpackDeltaSlot * sizeof(void*));
	CMemory::HookVirtualMethod(vtbl, reinterpret_cast<void*>(&Hook_SGE_BitpackDelta),
		kBitpackDeltaSlot, reinterpret_cast<void**>(&v_SGE_BitpackDelta));
	Msg(eDLL_T::SERVER,
		"[SNAP-PACK] hooked BitpackDelta (obj=%p vtbl=%p slot=%lld orig=%p hook=%p)\n",
		reinterpret_cast<void*>(g_pServerGameEntities), reinterpret_cast<void*>(vtbl),
		static_cast<long long>(kBitpackDeltaSlot),
		reinterpret_cast<void*>(origSlot), reinterpret_cast<void*>(&Hook_SGE_BitpackDelta));
}

// Clients that took the encode path this tick. Fake players never reach it, so
// this is the only number the snapshot-send cost actually scales with. Written
// from the job workers when the send is parallel, hence the atomic.
static std::atomic<int> s_snapRecipients{ 0 };

static int64_t Hook_CClient_SendSnapshot(int64_t a1, int64_t a2,
                                          int a3, int a4)
{
	const int clientSlot   = *reinterpret_cast<int*>(a1 + 0x10);
	const int signonState  = *reinterpret_cast<int*>(a1 + 0x3B0);
	const int lastSnapTick = *reinterpret_cast<int*>(a1 + 0x3D4);
	int* const pWaitTick   = reinterpret_cast<int*>(a1 + 0x59C);
	const int waitTick     = *pWaitTick;
	const bool isFake      = *reinterpret_cast<uint8_t*>(a1 + 0x5A0) != 0;

	// [PACK-FREEZE] LevelShutdown..LevelInit: force native keepalive-exit so
	// parallel pack guts never walk freed entity bases mid-changelevel.
	// Uses the same waitTick>0 path as FULL-PACE hold.
	if (SnapshotDiag_IsPackFrozen())
	{
		if (pWaitTick && *pWaitTick <= 0)
			*pWaitTick = 1;
		const int n = SnapshotDiag_PackFreezeLogInc();
		if (n < 8)
			Warning(eDLL_T::SERVER,
				"[PACK-FREEZE] SendSnapshot keepalive slot=%d signon=%d a3=%d "
				"(entity pack suppressed)\n",
				clientSlot, signonState, a3);
		return v_CClient_SendSnapshot(a1, a2, a3, a4);
	}

	SnapPack_EnsureHooked();

	if (!isFake)
		s_snapRecipients.fetch_add(1, std::memory_order_relaxed);

	const int entryDeltaAck = a3;

	// Ring miss on a3 is a silent NoDelta FULL; scan so pacing can treat it as one.
	bool bBaselineMiss = false;
	if (clientSlot == 0 && !isFake && entryDeltaAck >= 0)
	{
		int ringOldest = -1, ringNewest = -1;
		const bool bHave = SnapshotRing_HasTick(entryDeltaAck, &ringOldest, &ringNewest);
		const int  depth = SnapshotRing_Depth();
		bBaselineMiss = !bHave;

		// Age of the baseline the client is holding us to. Once it reaches the
		// ring depth the next snapshot is a forced FULL.
		const int age = (ringNewest >= 0) ? (ringNewest - entryDeltaAck) : -1;

		// Log only when the baseline is in trouble (evicted / ring nearly full).
		// Steady-state "ok" heartbeats were pure log I/O on the sim thread.
		const int hb = bridge_ack_age_heartbeat.GetInt();
		if (hb > 0)
		{
			static int s_ackAgeTick = 0;
			static int s_ackAgePeak = 0;
			if (age > s_ackAgePeak)
				s_ackAgePeak = age;
			if ((++s_ackAgeTick % hb) == 0)
			{
				Warning(eDLL_T::SERVER,
					"[ACK-AGE] slot=0 base=%d age=%d peak=%d ring{depth=%d newest=%d}\n",
					entryDeltaAck, age, s_ackAgePeak, depth, ringNewest);
				s_ackAgePeak = 0;
			}
		}

		static int s_ackDiagTick = 0;
		const bool bNoisy = bBaselineMiss || (depth > 0 && age >= (depth * 3) / 4);
		if (bNoisy && (bBaselineMiss || (++s_ackDiagTick % 32) == 0))
			Warning(eDLL_T::SERVER,
				"[ACK-DIAG] slot=0 base=%d age=%d ring{depth=%d oldest=%d newest=%d} %s\n",
				entryDeltaAck, age, depth, ringOldest, ringNewest,
				bBaselineMiss ? "BASELINE EVICTED -> forced NoDelta FULL" : "ring nearly exhausted");
	}

	// a3=-1 or lastSnapTick=-1 is a NoDelta FULL. Skip count while waitTick>0.
	if (clientSlot == 0 && !isFake && waitTick <= 0
		&& (a3 == -1 || lastSnapTick == -1 || bBaselineMiss))
	{
		static std::atomic<uint32_t> s_fullOut{0};
		const uint32_t n = s_fullOut.fetch_add(1, std::memory_order_relaxed) + 1;
		const char* reason = (a3 == -1) ? "a3=-1"
		                   : (lastSnapTick == -1) ? "lastSnap=-1"
		                   : "baseline evicted";
		if (n <= 8 || (n % 64) == 0)
		{
			Warning(eDLL_T::SERVER,
				"[FULL-OUT] #%u slot=0 signon=%d a3=%d lastSnap=%d waitTick=%d (%s)\n",
				n, signonState, a3, lastSnapTick, waitTick, reason);
		}
	}

	static std::atomic<int> s_lastRingCap{ -1 };
	if (clientSlot == 0 && signonState >= 6 && s_lastRingCap.load(std::memory_order_relaxed) < 0)
	{
		const uintptr_t base =
			static_cast<uintptr_t>(g_GameDll.GetModuleBase());
		if (base)
		{
			const uint64_t mgr =
				*reinterpret_cast<uint64_t*>(base + 0x234DE408);
			if (mgr)
			{
				const int ringCap = *reinterpret_cast<int*>(mgr + 8);
				if (s_lastRingCap.exchange(ringCap, std::memory_order_relaxed) < 0)
					Warning(eDLL_T::SERVER,
						"[RING-CAP] snapshot ring max_snapshots=%d "
						"(sv_max_snapshots_multiplayer default 160)\n", ringCap);
			}
		}
	}

	// ===== [EARLY-PACE] cap what one ack-frozen client costs the sim frame.
	if (clientSlot >= 0 && clientSlot < 64)
	{
		EarlyPaceSlot_t& st = s_earlyPace[clientSlot];

		if (isFake || signonState < 6 || entryDeltaAck < 0)
		{
			// Bootstrap / reconnect / full-update request: FULL-PACE owns those.
			if (st.nFrozenAck.exchange(-1, std::memory_order_relaxed) >= 0)
				st.nArmQpc.store(0, std::memory_order_relaxed);
			st.nRearmBlockAck.store(-1, std::memory_order_relaxed);
		}
		else
		{
			LARGE_INTEGER ackQpc;
			QueryPerformanceCounter(&ackQpc);
			if (st.nLastAck.exchange(entryDeltaAck, std::memory_order_relaxed) != entryDeltaAck
				|| !st.nLastAckQpc.load(std::memory_order_relaxed))
			{
				st.nLastAckQpc.store(static_cast<uint64_t>(ackQpc.QuadPart),
					std::memory_order_relaxed);
			}
			const long long ackStillMs =
				EarlyPace_HeldMs(st.nLastAckQpc.load(std::memory_order_relaxed));

			const int frozen = st.nFrozenAck.load(std::memory_order_relaxed);
			// FULL-PACE arms for slot 0 only, so its hold can only mask this one
			// there. Consulting it for every slot disabled EARLY-PACE server-wide
			// for the duration of any bridge-slot FULL.
			const bool bFullPaceOwns = (clientSlot == 0)
				&& s_pacedAwaitTick.load(std::memory_order_relaxed) >= 0;

			if (frozen >= 0 && entryDeltaAck > frozen)
			{
				const long long heldMs =
					EarlyPace_HeldMs(st.nArmQpc.exchange(0, std::memory_order_relaxed));
				st.nFrozenAck.store(-1, std::memory_order_relaxed);
				st.nRearmBlockAck.store(-1, std::memory_order_relaxed);
				Warning(eDLL_T::SERVER,
					"[EARLY-PACE] released slot=%d: ack %d -> %d after %lldms held "
					"-- deltas resume\n", clientSlot, frozen, entryDeltaAck, heldMs);
			}
			else if (frozen >= 0)
			{
				const long long heldMs =
					EarlyPace_HeldMs(st.nArmQpc.load(std::memory_order_relaxed));
				if (heldMs > bridge_early_pace_safety_ms.GetInt())
				{
					st.nFrozenAck.store(-1, std::memory_order_relaxed);
					st.nArmQpc.store(0, std::memory_order_relaxed);
					st.nRearmBlockAck.store(frozen, std::memory_order_relaxed);
					Warning(eDLL_T::SERVER,
						"[EARLY-PACE] safety-release slot=%d: ack STUCK at %d after %lldms "
						"held (cap=%dms) -- releasing to the forced-FULL resync, no re-arm "
						"until the ack passes %d\n",
						clientSlot, frozen, heldMs,
						bridge_early_pace_safety_ms.GetInt(), frozen);
				}
				// Re-pin every call so the keepalive exit survives the engine
				// clearing waitTick when it accepts an intermediate ack.
				else if (!bFullPaceOwns)
				{
					int ringNewest = -1;
					SnapshotRing_HasTick(entryDeltaAck, nullptr, &ringNewest);
					if (ringNewest > 0)
						*pWaitTick = ringNewest;
				}
			}
			else if (!bFullPaceOwns && sdk_bridge_full_pace.GetBool()
				&& bridge_full_pace_stale_ticks.GetInt() > 0 && !bBaselineMiss
				&& entryDeltaAck > st.nRearmBlockAck.load(std::memory_order_relaxed))
			{
				int ringOldest = -1, ringNewest = -1;
				SnapshotRing_HasTick(entryDeltaAck, &ringOldest, &ringNewest);
				const int age = (ringNewest >= 0) ? (ringNewest - entryDeltaAck) : -1;
				if (age >= bridge_full_pace_stale_ticks.GetInt() && ringNewest > 0
					&& ackStillMs >= bridge_early_pace_stale_ms.GetInt())
				{
					LARGE_INTEGER nowQpc;
					QueryPerformanceCounter(&nowQpc);
					st.nFrozenAck.store(entryDeltaAck, std::memory_order_relaxed);
					st.nArmQpc.store(static_cast<uint64_t>(nowQpc.QuadPart),
						std::memory_order_relaxed);
					*pWaitTick = ringNewest;
					Warning(eDLL_T::SERVER,
						"[EARLY-PACE] slot=%d armed: ack frozen at %d (age=%d/%d, unchanged %lldms) -- "
						"holding keepalive until it advances\n",
						clientSlot, entryDeltaAck, age,
						SnapshotRing_Depth(), ackStillMs);
				}
			}
		}
	}

	// -1 sentinels pass NULL checks and AV on deref.
	if (clientSlot == 0)
	{
		// CObserverMode trackers at handle_table+0xB10. Gated on signonState>=6.
		if (signonState >= 6)
		{
			EnsureObserverModeTrackers();
		}

		// Signon skips ActivatePlayer; invoke it once per connection at signonState>=6.
		extern ConVar sdk_bridge_activate_player;
		if (signonState < 6)
			s_lastActivatedClient.store(0, std::memory_order_relaxed);
		if (signonState >= 6 && sdk_bridge_activate_player.GetBool())
		{
			const uintptr_t curClient = static_cast<uintptr_t>(a1);
			if (s_lastActivatedClient.load(std::memory_order_relaxed) != curClient)
			{
				uintptr_t expected = s_lastActivatedClient.load(std::memory_order_relaxed);
				if (s_lastActivatedClient.compare_exchange_strong(expected, curClient,
					std::memory_order_acq_rel))
				{
					if (CClient__ActivatePlayer)
					{
						// Read server-side bridge entity field values BEFORE activation.
						// Closed-form null chain; ActivatePlayer foreign call stays SEH-fenced.
						uint8_t  pre0x35BA = 0xFF;
						float    pre0x35BC = 0.0f;
						uint32_t pre0x2DF8 = 0xCAFEBABEu;
						uintptr_t entPtrSnap = 0;
						{
							const uintptr_t base =
								static_cast<uintptr_t>(g_GameDll.GetModuleBase());
							const uint64_t structPtr = base
								? *reinterpret_cast<const uint64_t*>(base + 0xD4EC368)
								: 0;
							if (structPtr)
							{
								const uint64_t entTable =
									*reinterpret_cast<const uint64_t*>(structPtr + 0x78);
								if (entTable)
								{
									entPtrSnap = *reinterpret_cast<uintptr_t*>(
										entTable + 8ULL * 0 + 0x3C048);
									if (entPtrSnap)
									{
										pre0x35BA = *reinterpret_cast<const uint8_t*>(entPtrSnap + 0x35BA);
										pre0x35BC = *reinterpret_cast<const float*>(entPtrSnap + 0x35BC);
										pre0x2DF8 = *reinterpret_cast<const uint32_t*>(entPtrSnap + 0x2DF8);
									}
								}
							}
						}

						Warning(eDLL_T::SERVER,
							"[BRIDGE-ACTIVATE] PRE  client=0x%llX entity=0x%llX +0x35BA=%u +0x35BC=%.3f +0x2DF8=0x%08X signonState=%d\n",
							(unsigned long long)a1, (unsigned long long)entPtrSnap,
							pre0x35BA, pre0x35BC, pre0x2DF8, signonState);

						__try
						{
							CClient__ActivatePlayer(reinterpret_cast<CClient*>(a1));
						}
						__except (EXCEPTION_EXECUTE_HANDLER)
						{
							Warning(eDLL_T::SERVER,
								"[BRIDGE-ACTIVATE] EXCEPTION during ActivatePlayer\n");
							s_lastActivatedClient.store(0, std::memory_order_release);
							goto activateEnd;
						}

						// Read AFTER (the server-side fields should be populated now if
						// ClientActive reached them).
						uint8_t  post0x35BA = 0xFF;
						float    post0x35BC = 0.0f;
						uint32_t post0x2DF8 = 0xCAFEBABEu;
						if (entPtrSnap)
						{
							post0x35BA = *reinterpret_cast<const uint8_t*>(entPtrSnap + 0x35BA);
							post0x35BC = *reinterpret_cast<const float*>(entPtrSnap + 0x35BC);
							post0x2DF8 = *reinterpret_cast<const uint32_t*>(entPtrSnap + 0x2DF8);
						}

						Warning(eDLL_T::SERVER,
							"[BRIDGE-ACTIVATE] POST entity=0x%llX +0x35BA=%u (was %u) +0x35BC=%.3f (was %.3f) +0x2DF8=0x%08X (was 0x%08X)\n",
							(unsigned long long)entPtrSnap,
							post0x35BA, pre0x35BA,
							post0x35BC, pre0x35BC,
							post0x2DF8, pre0x2DF8);
					}
					else
					{
						Warning(eDLL_T::SERVER,
							"[BRIDGE-ACTIVATE] CClient__ActivatePlayer is null -- detour not resolved\n");
						s_lastActivatedClient.store(0, std::memory_order_release);
					}
				}
			}
		}
activateEnd: ;

		// Known module offsets + null chain; Ensure* is self-guarded.
		{
			const uintptr_t base =
				static_cast<uintptr_t>(g_GameDll.GetModuleBase());
			const uint64_t structPtr = base
				? *reinterpret_cast<const uint64_t*>(base + 0xD4EC368)
				: 0;
			if (structPtr)
			{
				const uint64_t entTable =
					*reinterpret_cast<const uint64_t*>(structPtr + 0x78);
				if (entTable)
				{
					const uint64_t entPtr = *reinterpret_cast<uint64_t*>(
						entTable + 8ULL * 0 + 0x3C048);
					if (entPtr)
						EnsurePlayerSettingsApplied(entPtr, "sendsnapshot-pre");
				}
			}
		}
	}

	// a3 is the real ack. -1 is a NoDelta FULL.

	int64_t snapResult = 0;
	LARGE_INTEGER engStart = {}, engEnd = {}, engFreq = {};
	const bool timeSend = sdk_snap_send_qpc.GetBool();
	const int sbLevel = sdk_snap_budget.GetInt();
	static std::atomic<long> s_snapSendN{0};
	const long sbN = (sbLevel > 0 && clientSlot == 0 && !isFake)
		? (s_snapSendN.fetch_add(1, std::memory_order_relaxed) + 1) : 0;
	const bool sbArm = SnapBudget_Armed(sbLevel, sbN);
	if (timeSend || sbArm)
	{
		QueryPerformanceCounter(&engStart);
		QueryPerformanceFrequency(&engFreq);
	}
	snapResult = v_CClient_SendSnapshot(a1, a2, a3, a4);

	if (clientSlot == 0 && !isFake && sdk_snap_tx_diag.GetBool())
	{
		static ULONGLONG s_txMs = 0;
		static int s_txCalls = 0, s_txHeld = 0, s_txEncoded = 0;
		++s_txCalls;
		if (waitTick > 0)
			++s_txHeld;
		const int postSnapTick = *reinterpret_cast<int*>(a1 + 0x3D4);
		if (postSnapTick != lastSnapTick)
			++s_txEncoded;
		const ULONGLONG nowMs = GetTickCount64();
		if (s_txMs == 0)
			s_txMs = nowMs;
		else if (nowMs - s_txMs >= 1000)
		{
			Warning(eDLL_T::SERVER,
				"[SNAP-TX-1S] slot=0 calls=%d held=%d encoded=%d a3=%d lastSnap=%d waitTick=%d ret=%lld\n",
				s_txCalls, s_txHeld, s_txEncoded, a3, postSnapTick, *pWaitTick,
				static_cast<long long>(snapResult));
			s_txMs = nowMs;
			s_txCalls = s_txHeld = s_txEncoded = 0;
		}
	}

	// FULL-PACING: arm after a NoDelta FULL. Hold/release lives in
	// SendClientMessages so the inlined world-frame add does not run.
	if (clientSlot == 0 && !isFake && sdk_bridge_full_pace.GetBool()
		&& (entryDeltaAck < 0 || bBaselineMiss)
		&& s_pacedAwaitTick.load(std::memory_order_relaxed) < 0)
	{
		if (s_pacedSkipRearm.exchange(0, std::memory_order_relaxed) != 0)
		{
			Warning(eDLL_T::SERVER,
				"[FULL-PACE] skip re-arm after safety-release (slot=0 a3=%d miss=%d)\n",
				entryDeltaAck, bBaselineMiss ? 1 : 0);
		}
		else
		{
			const int sentTick = *reinterpret_cast<int*>(a1 + 0x3D4);
			// Do not arm on a tick the client has already acked.
			if (sentTick <= entryDeltaAck)
			{
				static std::atomic<uint32_t> s_staleArm{0};
				const uint32_t sn = s_staleArm.fetch_add(1, std::memory_order_relaxed) + 1;
				if (sn <= 8 || (sn % 512) == 0)
					Warning(eDLL_T::SERVER,
						"[FULL-PACE] #%u skip arm: sentTick=%d <= a3=%d -- nothing to wait for\n",
						sn, sentTick, entryDeltaAck);
			}
			else if (sentTick >= 0)
			{
				LARGE_INTEGER armQpc;
				QueryPerformanceCounter(&armQpc);
				s_pacedArmQpc.store(static_cast<uint64_t>(armQpc.QuadPart), std::memory_order_relaxed);
				s_pacedAwaitTick.store(sentTick, std::memory_order_relaxed);
				Warning(eDLL_T::SERVER,
					"[FULL-PACE] armed slot=0: sent NoDelta FULL tick=%d -- holding (keepalive) "
					"until client acks it (deltas were storming before this)\n", sentTick);
			}
		}
	}

	if (timeSend || sbArm)
	{
		QueryPerformanceCounter(&engEnd);
		if (clientSlot == 0 && !isFake && engFreq.QuadPart > 0)
		{
			const double sendMs = SnapBudget_Ms(engStart.QuadPart, engEnd.QuadPart, engFreq.QuadPart);
			if (sbArm)
				Warning(eDLL_T::SERVER,
					"[SNAP-SEND] #%ld a3=%d lastSnap=%d send=%.2fms\n",
					sbN, a3, *reinterpret_cast<int*>(a1 + 0x3D4), sendMs);
			if (timeSend)
			{
				const int64_t engMs = static_cast<int64_t>(sendMs);
				if (engMs > 30)
				{
					static std::atomic<uint32_t> s_engLongCount{0};
					const uint32_t n = s_engLongCount.fetch_add(1, std::memory_order_relaxed) + 1;
					if (n <= 100)
						Warning(eDLL_T::SERVER,
							"[ENGINE-LONG] #%u v_CClient_SendSnapshot took %lldms a3=%d lastSnap=%d\n",
							n, engMs, a3, *reinterpret_cast<int*>(a1 + 0x3D4));
				}
			}
		}
	}

	return snapResult;
}


// World-frame add is inlined here. SendSnapshot only encodes.
// CClient[0] is at CServer+0x400 (signon at server+1968 == client+0x3B0).
static constexpr ptrdiff_t SERVER_OFF_CLIENT0      = 0x400;
static constexpr ptrdiff_t CLIENT_OFF_SLOT         = 0x10;
static constexpr ptrdiff_t CLIENT_OFF_DELTAACKTICK = 0x3C8;
static constexpr ptrdiff_t CLIENT_OFF_WAITTICK     = 0x59C;

static int64_t FullPace_HeldMs(void)
{
	LARGE_INTEGER nowQpc, qpcFreq;
	QueryPerformanceCounter(&nowQpc);
	QueryPerformanceFrequency(&qpcFreq);
	const uint64_t armQpc = s_pacedArmQpc.load(std::memory_order_relaxed);
	if (!armQpc || qpcFreq.QuadPart <= 0)
		return 0;
	return (int64_t)((nowQpc.QuadPart - (int64_t)armQpc) * 1000LL / qpcFreq.QuadPart);
}

static void FullPace_Release(int* pWaitTick, int awaiting, int ack, int64_t heldMs, bool bSafety)
{
	s_pacedAwaitTick.store(-1, std::memory_order_relaxed);
	if (pWaitTick)
		*pWaitTick = -1;
	// The hold itself aged this ack behind the ring head. Make EARLY-PACE wait
	// for the client to move past it rather than read that age as a stall.
	if (ack >= 0)
		s_earlyPace[0].nRearmBlockAck.store(ack, std::memory_order_relaxed);
	if (bSafety)
	{
		s_pacedSkipRearm.store(1, std::memory_order_relaxed);
		Warning(eDLL_T::SERVER,
			"[FULL-PACE] safety-release slot=0: FULL tick=%d UNACKED after %lldms "
			"(a3=%d, cap=%dms) -- next FULL will not re-arm hold\n",
			awaiting, (long long)heldMs, ack, sdk_bridge_full_pace_safety_ms.GetInt());
	}
	else
		Warning(eDLL_T::SERVER,
			"[FULL-PACE] released slot=0: client acked FULL tick=%d (a3=%d, held %lldms) "
			"-- deltas resume\n", awaiting, ack, (long long)heldMs);
}

// sv_parallel_sendsnapshot only picks job vs inline SendSnapshot, not the entity pack.
static ConVar bridge_parallel_sendsnapshot("bridge_parallel_sendsnapshot", "1",
	FCVAR_RELEASE,
	"1 = keep the engine's own sv_parallel_sendsnapshot (per-client snapshot send "
	"runs on job workers). 0 = force it off and pack every client serially on the "
	"frame thread.");

void Bridge_ApplyParallelSendPolicy(void)
{
	if (!g_pCVar)
		return;

	ConVar* const pParallel = g_pCVar->FindVar("sv_parallel_sendsnapshot");
	ConVar* const pSingleCore = g_pCVar->FindVar("sv_single_core_dedi");

	if (!pParallel)
	{
		Warning(eDLL_T::SERVER,
			"[SNAP-SEND] sv_parallel_sendsnapshot not found -- send path unknown\n");
		return;
	}

	if (!bridge_parallel_sendsnapshot.GetBool())
		pParallel->SetValue(0);

	// Both engine gates decide the dispatch; print them together so a run says
	// which path it took instead of leaving it to be inferred.
	Warning(eDLL_T::SERVER,
		"[SNAP-SEND] per-client fan-out: sv_parallel_sendsnapshot=%d "
		"sv_single_core_dedi=%d -> %s\n",
		pParallel->GetInt(),
		pSingleCore ? pSingleCore->GetInt() : -1,
		(pParallel->GetBool() && (!pSingleCore || !pSingleCore->GetBool()))
			? "job workers" : "SERIAL on the frame thread");
}

static ConVar bridge_snap_sync_send("bridge_snap_sync_send", "1",
	FCVAR_RELEASE,
	"Wait the snapshot pack/send jobs before SendClientMessages returns so the "
	"payload is this tick's post-sim state. 0 = native async (one tick stale).");

typedef void (__fastcall* PFN_JobWait)(unsigned int id, unsigned int a2, __int64 a3);

static uint32_t* s_pSnapSendJob = nullptr;
static PFN_JobWait s_pfnJobWait = nullptr;
static bool s_snapSyncResolved = false;
static bool s_snapSyncOk = false;
static bool s_snapSyncFaulted = false;
static const uint8_t* s_snapSCMPristine = nullptr;

static void SnapSync_ResolveOnce(void)
{
	if (s_snapSyncResolved)
		return;
	s_snapSyncResolved = true;

	if (!s_snapSCMPristine)
	{
		Warning(eDLL_T::SERVER, "[SNAP-SYNC] resolve failed: pristine base missing\n");
		return;
	}

	const uint8_t* const pBase = s_snapSCMPristine;
	const uint8_t* pMov = nullptr;
	for (size_t i = 0; i + 6 <= 0x60; ++i)
	{
		if (pBase[i] == 0x8B && pBase[i + 1] == 0x0D)
		{
			pMov = pBase + i;
			break;
		}
	}
	if (!pMov)
	{
		Warning(eDLL_T::SERVER, "[SNAP-SYNC] resolve failed: no 8B 0D in prologue\n");
		return;
	}

	const int32_t disp = *reinterpret_cast<const int32_t*>(pMov + 2);
	uint32_t* const pJob = reinterpret_cast<uint32_t*>(
		reinterpret_cast<uintptr_t>(pMov + 6) + static_cast<intptr_t>(disp));

	const uint8_t* pCall = nullptr;
	for (const uint8_t* p = pMov + 6; p + 5 <= pBase + 0x100; ++p)
	{
		if (*p == 0xE8)
		{
			pCall = p;
			break;
		}
	}
	if (!pCall)
	{
		Warning(eDLL_T::SERVER, "[SNAP-SYNC] resolve failed: no JobWait call after job load\n");
		return;
	}

	const int32_t rel = *reinterpret_cast<const int32_t*>(pCall + 1);
	PFN_JobWait const pfn = reinterpret_cast<PFN_JobWait>(
		reinterpret_cast<uintptr_t>(pCall + 5) + static_cast<intptr_t>(rel));

	if (!Mem_IsReadable(pJob, sizeof(uint32_t))
		|| !Mem_IsReadable(reinterpret_cast<const void*>(pfn), 1))
	{
		Warning(eDLL_T::SERVER, "[SNAP-SYNC] resolve failed: JobWait/job handle not readable\n");
		return;
	}

	s_pSnapSendJob = pJob;
	s_pfnJobWait = pfn;
	s_snapSyncOk = true;
	Msg(eDLL_T::SERVER, "[SNAP-SYNC] in-frame wait armed (job=%p wait=%p)\n",
		s_pSnapSendJob, s_pfnJobWait);
}

bool Bridge_SnapSyncSendActive(void)
{
	if (!bridge_snap_sync_send.GetBool() || s_snapSyncFaulted)
		return false;
	if (s_snapSyncResolved && !s_snapSyncOk)
		return false;
	return true;
}

static void SnapSync_WaitJobs(int64_t thisptr)
{
	if (s_snapSyncFaulted)
		return;
	SnapSync_ResolveOnce();
	if (!s_snapSyncOk || !thisptr || !s_pSnapSendJob || !s_pfnJobWait)
		return;

	__try
	{
		const uint32_t id1 = *s_pSnapSendJob;
		if (id1 != 0)
		{
			s_pfnJobWait(id1, 0, -1LL);
			*s_pSnapSendJob = 0;
		}

		// S3 CServer snapshot-manager offsets
		const uint64_t mgr = *reinterpret_cast<uint64_t*>(
			reinterpret_cast<char*>(thisptr) + 47855112);
		if (Mem_IsReadable(reinterpret_cast<const void*>(mgr + 1084096), 4))
		{
			uint32_t* const pId2 = reinterpret_cast<uint32_t*>(mgr + 1084096);
			const uint32_t id2 = *pId2;
			if (id2 != 0)
			{
				s_pfnJobWait(id2, 0, -1LL);
				*pId2 = 0;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		s_snapSyncFaulted = true;
		Warning(eDLL_T::SERVER,
			"[SNAP-SYNC] wait faulted (code=0x%lX) -- disabled for process\n",
			GetExceptionCode());
	}
}

static int64_t Hook_CServer_SendClientMessages(int64_t thisptr, char bSendSnapshots)
{
	static std::atomic<int> s_holdStoreLog{ 0 };
	const int awaiting = s_pacedAwaitTick.load(std::memory_order_relaxed);
	if (bSendSnapshots && awaiting >= 0 && thisptr
		&& *reinterpret_cast<int*>(thisptr + SERVER_OFF_CLIENT0 + CLIENT_OFF_SLOT) == 0)
	{
		int64_t pClient = thisptr + SERVER_OFF_CLIENT0;
		const int ack = *reinterpret_cast<int*>(pClient + CLIENT_OFF_DELTAACKTICK);
		int* const pWaitTick = reinterpret_cast<int*>(pClient + CLIENT_OFF_WAITTICK);
		const int64_t heldMs = FullPace_HeldMs();
		int ringNewest = -1;
		SnapshotRing_HasTick(ack, nullptr, &ringNewest);

		if (ack >= awaiting && (ringNewest < 0 || ack <= ringNewest))
		{
			s_holdStoreLog.store(0, std::memory_order_relaxed);
			FullPace_Release(pWaitTick, awaiting, ack, heldMs, false);
		}
		else if (heldMs > sdk_bridge_full_pace_safety_ms.GetInt())
		{
			s_holdStoreLog.store(0, std::memory_order_relaxed);
			FullPace_Release(pWaitTick, awaiting, ack, heldMs, true);
		}
		else
		{
			// waitTick>0 so the FULL ack is not dropped. Do not clear bSendSnapshots.
			*pWaitTick = (awaiting > 0) ? awaiting : 1;
			if (s_holdStoreLog.fetch_add(1, std::memory_order_relaxed) == 0)
				Warning(eDLL_T::SERVER,
					"[FULL-PACE] hold: keepalive via waitTick -- ring keeps advancing\n");
		}
	}
	else
		s_holdStoreLog.store(0, std::memory_order_relaxed);

	const bool bPhase = bridge_snap_phase.GetBool();
	const int sbLevel = sdk_snap_budget.GetInt();
	const bool bBudget = TickBudget_Armed();
	static std::atomic<long> s_scmN{0};
	const long sbN = (sbLevel > 0)
		? (s_scmN.fetch_add(1, std::memory_order_relaxed) + 1) : 0;
	const bool sbArm = SnapBudget_Armed(sbLevel, sbN);

	if (bBudget)
		s_snapRecipients.store(0, std::memory_order_relaxed);

	LARGE_INTEGER tEntry = {}, tAfterOrig = {}, tAfterWait = {}, freq = {};
	if (bPhase || sbArm || bBudget)
		QueryPerformanceFrequency(&freq);
	if (bPhase)
	{
		QueryPerformanceCounter(&tEntry);
		s_snapPhaseScmEntryQpc.store(tEntry.QuadPart, std::memory_order_relaxed);
		s_snapPhaseScmEntryFreq.store(freq.QuadPart, std::memory_order_relaxed);
	}
	else if (sbArm || bBudget)
		QueryPerformanceCounter(&tEntry);

	const int64_t r = v_CServer_SendClientMessages(thisptr, bSendSnapshots);

	if (bPhase || sbArm || bBudget)
		QueryPerformanceCounter(&tAfterOrig);

	const bool bDoSync = bSendSnapshots && bridge_snap_sync_send.GetBool() && !s_snapSyncFaulted;
	if (bDoSync)
		SnapSync_WaitJobs(thisptr);

	if (bPhase || bBudget)
		QueryPerformanceCounter(&tAfterWait);

	if (bBudget && freq.QuadPart > 0)
	{
		TickBudget_ReportScm(
			(tAfterOrig.QuadPart - tEntry.QuadPart) * 1000000LL / freq.QuadPart,
			bDoSync ? ((tAfterWait.QuadPart - tAfterOrig.QuadPart) * 1000000LL / freq.QuadPart) : 0,
			s_snapRecipients.load(std::memory_order_relaxed));
	}

	if (bPhase)
	{
		static std::atomic<long> s_phaseN{ 0 };
		const long n = s_phaseN.fetch_add(1, std::memory_order_relaxed) + 1;
		if ((n % 20) == 0)
		{
			static LONGLONG s_phaseBase = 0;
			if (s_phaseBase == 0)
				s_phaseBase = tEntry.QuadPart;
			const int64_t entryUs = (freq.QuadPart > 0)
				? ((tEntry.QuadPart - s_phaseBase) * 1000000LL / freq.QuadPart) : 0;
			const int64_t origUs = (freq.QuadPart > 0)
				? ((tAfterOrig.QuadPart - tEntry.QuadPart) * 1000000LL / freq.QuadPart) : 0;
			const int64_t waitUs = (bDoSync && freq.QuadPart > 0)
				? ((tAfterWait.QuadPart - tAfterOrig.QuadPart) * 1000000LL / freq.QuadPart) : 0;
			const int tick = gpGlobals ? gpGlobals->tickCount : 0;
			Msg(eDLL_T::SERVER,
				"[SNAP-PHASE] scm entry=%lld orig=%lld wait=%lld tick=%d\n",
				(long long)entryUs, (long long)origUs, (long long)waitUs, tick);
		}
	}

	if (sbArm)
	{
		Warning(eDLL_T::SERVER,
			"[SNAP-SCM] #%ld sendSnaps=%d scm=%.2fms\n",
			sbN, (int)(unsigned char)bSendSnapshots,
			SnapBudget_Ms(tEntry.QuadPart, tAfterOrig.QuadPart, freq.QuadPart));
	}
	return r;
}


void VCClientSendSnapshotDiag::GetFun() const
{

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 20 55 56 41 55 41 56 41 57 48 8D 6C 24 A0 "
		"48 81 EC 60 01 00 00 48 63 41 10")
		.GetPtr(v_CClient_SendSnapshot);

	// CServer::SendClientMessages -- alloca 0x10B0, bSendSnapshots in dl.
	Module_FindPattern(g_GameDll,
		"40 55 56 41 55 41 56 41 57 B8 B0 10 00 00 E8 ?? ?? ?? ?? "
		"48 2B E0 0F 29 B4 24 90 10 00 00 0F B6 EA 48 8B F1")
		.GetPtr(v_CServer_SendClientMessages);
	// Resolver must read pre-detour bytes.
	s_snapSCMPristine = reinterpret_cast<const uint8_t*>(v_CServer_SendClientMessages);
	SnapSync_ResolveOnce();

}

void VCClientSendSnapshotDiag::Detour(const bool bAttach) const
{
	if (!v_CClient_SendSnapshot)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] CClient::SendSnapshot pattern unresolved -- "
			"post-FULL snapshot diagnostic NOT active.\n");
	}
	else
	{
		const LONG result = bAttach
			? DetourAttach(reinterpret_cast<void**>(&v_CClient_SendSnapshot),
				reinterpret_cast<void*>(&Hook_CClient_SendSnapshot))
			: DetourDetach(reinterpret_cast<void**>(&v_CClient_SendSnapshot),
				reinterpret_cast<void*>(&Hook_CClient_SendSnapshot));
		if (bAttach)
		{
			Msg(eDLL_T::SERVER,
				"[s21-bridge] DetourAttach CClient::SendSnapshot result=0x%lX "
				"(target=0x%p)\n", result, (void*)v_CClient_SendSnapshot);
		}
	}

	if (!v_CServer_SendClientMessages)
	{
		Warning(eDLL_T::SERVER,
			"[FULL-PACE] CServer::SendClientMessages pattern unresolved -- "
			"world ring still advances during FULL hold\n");
	}
	else
	{
		const LONG result = bAttach
			? DetourAttach(reinterpret_cast<void**>(&v_CServer_SendClientMessages),
				reinterpret_cast<void*>(&Hook_CServer_SendClientMessages))
			: DetourDetach(reinterpret_cast<void**>(&v_CServer_SendClientMessages),
				reinterpret_cast<void*>(&Hook_CServer_SendClientMessages));
		if (bAttach)
		{
			Msg(eDLL_T::SERVER,
				"[FULL-PACE] DetourAttach CServer::SendClientMessages result=0x%lX "
				"(target=0x%p)\n", result, (void*)v_CServer_SendClientMessages);
		}
	}
}

void SnapshotSend_LevelShutdown(void)
{
	s_lastActivatedClient.store(0, std::memory_order_relaxed);
	s_pacedAwaitTick.store(-1, std::memory_order_relaxed);
	s_pacedArmQpc.store(0, std::memory_order_relaxed);
	for (EarlyPaceSlot_t& st : s_earlyPace)
	{
		st.nFrozenAck.store(-1, std::memory_order_relaxed);
		st.nArmQpc.store(0, std::memory_order_relaxed);
		st.nRearmBlockAck.store(-1, std::memory_order_relaxed);
	}
}

void SnapshotSend_OnPackFreezeReleased(void)
{
	if (!g_pServer)
		return;
	int* const pWaitTick = reinterpret_cast<int*>(
		reinterpret_cast<char*>(g_pServer) + SERVER_OFF_CLIENT0 + CLIENT_OFF_WAITTICK);
	if (*pWaitTick == 1)
		*pWaitTick = -1;
}
