//=============================================================================//
//
// Purpose: net_bridge -- leftover install patches + VEH + observer hooks.
//
//=============================================================================//
#include "common/callback.h"
#include "windows/pso_cache.h"
#include "core/stdafx.h"
#include <thread>

#include "engine/client/net_observer.h"
#include "engine/client/net_bridge_internal.h"
#include "core/bridge_stats.h"
#include "engine/client/bridge_join_auth.h"
#include "engine/client/bridge_connect_password.h"
#include "engine/sys_integrity.h"
#include "rtech/pak/pak_lobby_world.h"
#include "engine/mdl_precache_client_grow.h"
#include "tier0/memvalidate.h"
#include "tier0/commandline.h"
#include "tier1/lzss.h"

#include "engine/cmd.h"
#include "engine/sys_mainwind.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "engine/client/clientstate.h"
#include "engine/client/client.h"
#include "engine/client/cl_rcon.h"
#include "engine/client/cl_rcon_launcher.h"
#include "engine/server/sv_rcon.h"
#include "windows/id3dx.h"
#include "ebisusdk/EbisuSDK.h"
#include "public/tier1/cmd.h"
#include "public/bspflags.h"
#include "public/globalvars_base.h"
#include "tier1/cvar.h"
#include "common/global.h"
#include "rtech/playlists/playlists.h"

#include "game/shared/activity.h"
#include "game/shared/activity_s3_to_s21_client.h"

// [S2C-SCRIPTREMOTE] name-resolve (C_BaseScriptRemoteFunctions local entries) +
// injection (CSquirrelVM::ExecuteFunction) surface for the dedi(S3)->client(S21) ScriptRemote lane.
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
#pragma comment(lib, "winmm.lib")
#include <timeapi.h>
#include <processthreadsapi.h>
#include <ctime>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

// Client global vars (curTime/tickCount) for the [ANIM-WATCH] clock-domain probe.
// Defined in cdll_engine_int.cpp; same extern pattern as status_effects_sdk.cpp.
extern CGlobalVarsBase* gpGlobals;

// Silence C4505/C4189/C4456/C4459; leave C4244 enabled.
#pragma warning(disable: 4456 4459)
#include "engine/client/net_bridge_split.h"


// SDK_Log + the file-only bridge_trace redirect macro + BridgeTrace_Log are now
// declared in engine/client/net_bridge_internal.h (shared by every net_bridge TU).

// Identity: g_NucleusID / g_PersonaName. C2S platform-user-id is Nucleus id.
uint64_t S21Bridge_GetConnectNucleusID()
{
	if (g_NucleusID && *g_NucleusID != 0)
		return *g_NucleusID;

	// Pre-identity window only (online, before account callback). Deterministic.
	return static_cast<uint64_t>(FAKE_BASE_NUCLEUD_ID);
}

// Runtime-VA address accessors moved to engine/client/net_bridge_internal.h

// SDK_Log in this TU is file-only (bridge_trace); see net_bridge_internal.h.

// g_sdkObserveNet: 0 quiet, 1 WSASocket/NET_SendPacket inner/Cbuf/VNET-RECV,
// 2 also NET_SendPacket public. Warnings/errors stay ungated.
int g_sdkObserveNet = 0;

// SDK_Log gates stdout / overlay output behind g_bSdkObserveInit (defined
// in dllmain.cpp). g_sdkObserveNet is mirrored into it elsewhere at runtime.
extern bool g_bSdkObserveInit;

// Stub RecvTable diagnostic sink: permanently off.

// Live x64 user-mode pointer: r5apex image or heap arena, 8-byte aligned.
static inline bool Bridge_IsCanonPtr(uint64_t v)
{
    if (v & 7ULL) return false;                              // pointers are >=8-aligned
    if ((v & 0xFFFFFULL) == 0) return false;                 // 1MB-aligned => DATA (e.g. 0x1FF00000000,
                                                             //), not a real heap/code ptr
    if (v >= 0x140000000ULL && v < 0x142000000ULL) return true;   // r5apex.text/.rdata
    if (v >= 0x010000000000ULL && v < 0x800000000000ULL) return true; // heap arena
    return false;
}

// [HW-WATCH] shared VEH remains; per-entity DR0 ConVars removed.

// [ANIMGATE] IsWatchingReplay flag (killcam/replay), not "local slot 0 active".
// Forcing it seizes the camera. 1P play correctly tears down local animstate when
// ShouldDrawLocalPlayer is false (expected in 1P; see AnimWireSkip_IsClientAuthored).
static uintptr_t s_animgateByte994 = 0;   // runtime VA of IsWatchingReplay flag

// Local view-handle so native prediction does not early-out. Not splitscreen.
// 1=once a2=0, 2=once a2=1, 3=per-tick a2=0.
static ConVar bridge_classinfo_diag("bridge_classinfo_diag", "0", FCVAR_DEVELOPMENTONLY,
	"ClassInfo CHANGELEVEL diagnostic pack: [CI-GATE] [CI-ST] [CI-WALK] SEH inventory + flush + emergency minidump. 0=off 1=on 2=verbose.");

static ConVar bridge_diag_firehose("bridge_diag_firehose", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Per-create and per-snapshot bridge diagnostic streams (SNAP-WALK, CNE-LATE, SPAWN-MEM tick sampler).");

bool Bridge_DiagFirehoseEnabled(void)
{
	return bridge_diag_firehose.GetBool();
}

static ConVar bridge_local_view_install("bridge_local_view_install", "1", FCVAR_RELEASE,
    "Install local view-handle for native prediction. "
    "1=once a2=0, 2=once a2=1, 3=per-tick a2=0.");
static volatile LONG s_localViewInstalled = 0;
static uintptr_t s_localViewFnRT  = 0;
static uintptr_t s_viewHandleRT   = 0;

// [BONEFOLLOW-GUARD] Bounds-check server m_boneIndex / collision-part against the
// client model before C_BoneFollower::TestCollision. Mismatched bone tables from a
// cross-version dedi otherwise yield misaligned BVH walks. Invalid -> skip trace.
static ConVar bridge_bonefollow_guard("bridge_bonefollow_guard", "1",
    FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS,
    "[BONEFOLLOW-GUARD] Validate C_BoneFollower m_boneIndex and collision-part index "
    "against the owner model before BVH trace. Invalid -> skip and log. "
    "1 = on (default), 0 = observer-only.");


// [SIGNON-TRACE] hooks CClientState::SetSignonState + CL_FullyConnected
// -- pass-through only.
typedef char    (__fastcall* PFN_CClientState_SetSignonState)(__int64 a1, int newState, int spawnCount, __int64 pMsg);
typedef __int64 (__fastcall* PFN_CL_FullyConnected)(char* mapname);
static PFN_CClientState_SetSignonState   s_origCS_SetSignonState   = nullptr;
static PFN_CL_FullyConnected s_origCL_FullyConnected = nullptr;

typedef uintptr_t     (__fastcall* PFN_GetLocalPlayer)(int slot);
static PFN_GetLocalPlayer s_pathAGetLocal  = nullptr;

// Activity selector: S3 dedi activity ordinals are not S21 ordinals (e.g. jump).
// Translate queued activity S3->S21 before the selector emits a sequence.
typedef __int16 (__fastcall* PFN_ActivitySelector)(__int64 animstate);
static PFN_ActivitySelector s_origActivitySelector = nullptr;

static volatile LONG s_hwHit      = 0;
static volatile LONG s_hwCaptured = 0;
static volatile LONG s_hwLogged   = 0;   // lines actually emitted (latch on this, not raw hits)
static uintptr_t     s_hwAddr     = 0;
static int           s_hwWatchKind = 0;  // 0=dynprop(+0x15D8), 1=m_iHealth, 2=m_armorType, 3=model-index, 11=ptr-guard victim
static uint32_t      s_hwLastVal   = 0xFFFFFFFFu;  // prev low dword (m_iHealth @+0x328), flags transitions
static uint32_t      s_hwLastHi    = 0xFFFFFFFFu;  // prev high dword (the +0x32C field, NOT maxhealth@0x470)
static constexpr int kHwMaxHits   = 64;  // latch after this many LOGGED (non-memset) prop applies

// Uncached: cold forensic/dump callers want a fresh answer.
static bool Bridge_RegionReadable(uintptr_t a, size_t n)
{
    return Mem_IsReadable(reinterpret_cast<const void*>(a), n);
}

// Cached form for frame/snapshot hot paths (thread-local region ring).
static bool Bridge_RegionReadableCached(uintptr_t a, size_t n)
{
    return Mem_IsReadableCached(reinterpret_cast<const void*>(a), n);
}

// Snapshot-path decoder pointer check -- cached via Bridge_RegionReadableCached.
static bool Bridge_DecoderReadable(uintptr_t a9, size_t n)
{
    return Bridge_RegionReadableCached(a9, n);
}

// Raw byte dump of [base, base+bytes) to a file via plain WriteFile -- no
// dbghelp, no MiniDumpWriteDump, so it sidesteps the Crashpad inline-hook that
// caps the engine's own dumps to stack-only. Clamps to the readable prefix.
static void Bridge_RawDumpRegion(const char* path, uintptr_t base, size_t bytes)
{
    if (!base || !bytes) return;
    if (!Bridge_RegionReadable(base, bytes))
    {
        size_t okBytes = 0;
        while (okBytes < bytes && Bridge_RegionReadable(base + okBytes, 0x40)) okBytes += 0x40;
        bytes = okBytes;
    }
    if (!bytes) return;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    // Readable prefix already clamped via Bridge_RegionReadable above.
    WriteFile(h, reinterpret_cast<LPCVOID>(base), static_cast<DWORD>(bytes), &wrote, nullptr);
    CloseHandle(h);
}

//-----------------------------------------------------------------------------
// High-volume packet logs go to net_trace.log, not stdout.
//-----------------------------------------------------------------------------
static void NetObs_LogPacket(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	const int len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len <= 0)
		return;

	// Route to the net_trace logger -- created in logdef.cpp alongside all
	// the other session-folder loggers, and SpdLog_Init set flush_on(trace)
	// on it so we won't lose the last 5 seconds of packet history on a crash.
	std::shared_ptr<spdlog::logger> logger = spdlog::get("net_trace");
	if (logger)
		logger->info(std::string(buf, (size_t)len));

	// Mirror into bridge_trace.log via "%s" so '%' in the payload is literal.
	if (g_sdkObserveNet >= 1)
		SDK_Log("%s", buf);
}

//-----------------------------------------------------------------------------
// File-only SDK_Log sink (bridge_trace.log). No-op before SpdLog_Init.
//-----------------------------------------------------------------------------
void BridgeTrace_Log(const char* fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	const int len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len <= 0)
		return;

	std::shared_ptr<spdlog::logger> logger = spdlog::get("bridge_trace");
	if (!logger)
		return;

	// Pass std::string so spdlog does not re-interpret '%' in the payload.
	logger->info(std::string(buf, (size_t)len));
}

// Flush bridge_trace so a [FATAL-OBS] line survives TerminateProcess.
void BridgeTrace_Flush()
{
	if (auto logger = spdlog::get("bridge_trace"))
		logger->flush();
}

//-----------------------------------------------------------------------------
// [HOOK-COST]/[PIPELINE]/[HFLOW] live-telemetry definitions removed.

// [HOOK-COST] scope timer + dump removed.

// [VEH-AV] bridge AV-capture handler removed (was AddVectoredExceptionHandler
// crash_diag.log dumper). The legacy r5sdk CCrashHandler VEH remains.

//-----------------------------------------------------------------------------
// BridgeStubDiag: permanent no-op; call sites stay compiling.
//-----------------------------------------------------------------------------
void BridgeStubDiag(const char* fmt, ...)
{
    (void)fmt;
}

//-----------------------------------------------------------------------------
// Module state
//-----------------------------------------------------------------------------
static bool s_bInstalled = false;

//-----------------------------------------------------------------------------
// After handshake, NET_ReceiveDatagram polls this socket for S2C.
//-----------------------------------------------------------------------------
SOCKET      s_bridgeSocket  = INVALID_SOCKET;
sockaddr_in6 s_bridgeDest   = {};        // S3 server address (IPv6 dual-stack)
bool        s_bridgeActive  = false;     // true after bridge handshake completes

// --- Async handshake state machine ---
// The engine thread returns from NET_SendPacket immediately; stage
// transitions are driven by PollReceive's non-blocking recvfrom.

bool s_connAcceptDone = false;
static volatile LONG s_needNativeConnected = 0;
static volatile LONG s_nativeConnectedTries = 0;
BridgeHsStage s_hsStage         = BridgeHsStage::Idle;
ULONGLONG     s_hsStageDeadline = 0;     // GetTickCount64 absolute ms
static ULONGLONG     s_lastBypassAttempt = 0;   // retry rate limit (3 s)
CNetChan*   s_bridgeChan    = nullptr;   // S21 engine's CNetChan (captured from ProcessPacket)
static uintptr_t   s_clientStatePtr = 0;         // S21 CClientState* (captured from SetSignonState hook)

CNetChan* S21Bridge_GetActiveChan(void)
{
	return s_bridgeChan;
}

// Bare map name from CONNACCEPT. CClientState+0x1BC is later overlapped at +0x1C4;
// ziprail.ent keys off this captured string, not the stomped field.
char g_bridgeConnMapName[64] = {};

const char* Bridge_GetLevelBaseName()
{
	// Prefer the clean CONNACCEPT-captured name. Fall back to the (racy,
	// potentially overlapped) CClientState field only if we never captured one.
	if (g_bridgeConnMapName[0])
		return g_bridgeConnMapName;

	static char s_nameBuf[64];
	if (!s_clientStatePtr)
		return "";
	__try
	{
		const char* pLevelBase = reinterpret_cast<const char*>(s_clientStatePtr + 0x1BC);
		strncpy(s_nameBuf, pLevelBase, sizeof(s_nameBuf) - 1);
		s_nameBuf[sizeof(s_nameBuf) - 1] = '\0';
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		s_nameBuf[0] = '\0';
	}
	return s_nameBuf;
}

static uintptr_t S21Bridge_ClientStatePtr(void)
{
	if (s_clientStatePtr)
		return s_clientStatePtr;
	return NetObs_ClientStateSingletonSlotAddr();
}

uintptr_t Bridge_ClientStatePtr(void)
{
	return s_clientStatePtr;
}

// S21 never reaches native CONNECTED from the rewritten 0x04 alone. Drive
// CClientState::SetSignonState(2) on the frame thread once CONNACCEPT lands.
// pMsg is unused in case 2; spawnCount -1 skips the server-count mismatch reject.
static void S21Bridge_TryDriveNativeConnected(void)
{
	if (InterlockedCompareExchange(&s_needNativeConnected, 0, 0) == 0)
		return;
	if (!s_origCS_SetSignonState)
		return;

	const uintptr_t cl = S21Bridge_ClientStatePtr();
	if (!cl)
		return;

	int cur = -1;
	uintptr_t chan = 0;
	__try
	{
		cur = *reinterpret_cast<int*>(cl + 0xAC); // m_nSignonState
		chan = *reinterpret_cast<uintptr_t*>(cl + 0x60); // CNetChan*
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return;
	}

	if (cur >= 2)
	{
		Msg(eDLL_T::ENGINE, "[BRIDGE] native signon already %d -- skip drive\n", cur);
		InterlockedExchange(&s_needNativeConnected, 0);
		return;
	}

	const LONG n = InterlockedIncrement(&s_nativeConnectedTries);
	if (!chan)
	{
		if (n == 1 || (n % 64) == 0)
			Msg(eDLL_T::ENGINE,
				"[BRIDGE] native CONNECTED wait -- no netchan (signon=%d tries=%ld)\n",
				cur, n);
		if (n >= 600)
			InterlockedExchange(&s_needNativeConnected, 0);
		return;
	}

	Msg(eDLL_T::ENGINE,
		"[BRIDGE] driving native CONNECTED cl=%p signon=%d chan=%p\n",
		(void*)cl, cur, (void*)chan);

	char ok = 0;
	__try
	{
		ok = s_origCS_SetSignonState(static_cast<__int64>(cl), 2, -1, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE] native CONNECTED faulted\n");
		if (n >= 8)
			InterlockedExchange(&s_needNativeConnected, 0);
		return;
	}

	int after = -1;
	__try { after = *reinterpret_cast<int*>(cl + 0xAC); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	Msg(eDLL_T::ENGINE, "[BRIDGE] native CONNECTED ret=%d now=%d\n", (int)ok, after);
	if (after >= 2 || n >= 8)
		InterlockedExchange(&s_needNativeConnected, 0);
}

// Forward decl: defined later, needed by Hook_Connect_Worker stale-bridge teardown.
static void S21Bridge_ResetAllState();
SplitQueueEntry s_splitQueue[SPLIT_QUEUE_SIZE];
volatile long s_splitQueueHead = 0; // written by Hook_sendto / Hook_recvfrom
volatile long s_splitQueueTail = 0; // read by PollReceive
volatile long s_splitSessionGen = 1;
volatile long s_splitQueueGen[SPLIT_QUEUE_SIZE] = {};
volatile long s_splitSlotGen[4] = {};

static void S21Bridge_EnqueueS2C(const char* buf, int len)
{
	if (!buf || len <= 0)
		return;
	if (len > SPLIT_QUEUE_PKT_MAX)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE] S2C enqueue drop len=%d\n", len);
		return;
	}
	const long gen = s_splitSessionGen;
	const long head = s_splitQueueHead;
	const long next = (head + 1) % SPLIT_QUEUE_SIZE;
	if (next == s_splitQueueTail)
	{
		static long s_qfull = 0;
		if (++s_qfull <= 8)
			Warning(eDLL_T::ENGINE, "[BRIDGE] S2C queue full, drop %d bytes\n", len);
		return;
	}
	if (gen != s_splitSessionGen)
		return;
	s_splitQueue[head].len = len;
	memcpy(s_splitQueue[head].data, buf, static_cast<size_t>(len));
	s_splitQueueGen[head] = gen;
	if (gen != s_splitSessionGen)
		return;
	s_splitQueueHead = next;
}

void S21Bridge_DrainSocketToQueue(void)
{
	if (!s_bridgeActive || s_bridgeSocket == INVALID_SOCKET || !s_origRecvfrom)
		return;

	char szBuf[SPLIT_QUEUE_PKT_MAX];
	sockaddr_in6 from = {};
	for (int i = 0; i < 128; ++i)
	{
		int nFromLen = sizeof(from);
		const int n = s_origRecvfrom(s_bridgeSocket, szBuf, sizeof(szBuf), 0,
			reinterpret_cast<sockaddr*>(&from), &nFromLen);
		if (n <= 0)
		{
			// A datagram larger than the queue entry is consumed and lost.
			if (n < 0 && WSAGetLastError() == WSAEMSGSIZE)
			{
				static long s_msgSize = 0;
				if (++s_msgSize <= 8)
					Warning(eDLL_T::ENGINE,
						"[BRIDGE] pak-wait drain lost a datagram over %d bytes (#%ld)\n",
						SPLIT_QUEUE_PKT_MAX, s_msgSize);
				continue;
			}
			break;
		}
		if (!S21Bridge_FromMatchesPeer(reinterpret_cast<sockaddr*>(&from), nFromLen))
			continue;
		S21Bridge_EnqueueS2C(szBuf, n);
	}
}

// S21 RecvTable names: match -> needsDecoder=1, else 0.
std::unordered_set<std::string> s_s21RecvTableNames;

// Populate s_s21RecvTableNames from the S21 engine's RecvTable registry.
// Called lazily on first SVC_SendTable encounter (engine is fully initialized by then).
void S21Bridge_ExtractRecvTableNames()
{
	if (!s_s21RecvTableNames.empty()) return; // already populated

	uintptr_t base = NetObs_GetExeModuleBase();
	if (!base) return;

	uintptr_t* pListBase = reinterpret_cast<uintptr_t*>(NetObs_RecvTableListBaseAddr());
	uint16_t* pListHead = reinterpret_cast<uint16_t*>(NetObs_RecvTableListHeadAddr());
	uintptr_t listBase = *pListBase;
	uint16_t slot = *pListHead;

	if (!listBase) {
		SDK_Log("[BRIDGE-ST] WARNING: RecvTable list base is NULL\n");
		return;
	}

	while (slot != 0xFFFF)
	{
		uintptr_t node = listBase + 16ULL * slot;
		uintptr_t recvTable = *reinterpret_cast<uintptr_t*>(node);
		if (recvTable)
		{
			const char* name = *reinterpret_cast<const char**>(recvTable + 1224);
			if (name && name[0])
				s_s21RecvTableNames.insert(std::string(name));
		}
		slot = *reinterpret_cast<uint16_t*>(node + 10);
	}

	SDK_Log("[BRIDGE-ST] extracted %d S21 RecvTable names for SendTable filtering\n",
		(int)s_s21RecvTableNames.size());

	SDK_Log("[BRIDGE-ST] --- S21 RecvTable name dump (%d tables) ---\n",
		(int)s_s21RecvTableNames.size());
	int dumpIdx = 0;
	for (const auto& n : s_s21RecvTableNames)
	{
		if (dumpIdx < 300)
			SDK_Log("[BRIDGE-ST]   recv[%d] '%s'\n", dumpIdx, n.c_str());
		dumpIdx++;
	}
}

// Helper: check if a table name exists in the S21 RecvTable set.
// Uses case-insensitive matching because S3 and S21 can differ in casing
// (e.g. S3 sends 'DT_WORLD', S21 has 'DT_World').
bool S21Bridge_HasRecvTable(const char* name)
{
	S21Bridge_ExtractRecvTableNames(); // lazy init
	if (s_s21RecvTableNames.count(std::string(name)) > 0)
		return true;
	for (const auto& rn : s_s21RecvTableNames)
	{
		if (_stricmp(rn.c_str(), name) == 0)
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Stub RecvTable/RecvProp: RecvProp=0x68, RecvTable=0x4E0. Node +0x00 tbl +0x0A next.
// RecvTable +0x008 props +0x4C0 decoder; RecvProp +0x00 type +0x04 off +0x20 dt +0x28 name +0x48 proxy.
//-----------------------------------------------------------------------------

struct StubRecvTableEntry {
	std::string name;     // canonical (case-fixed) table name
	uintptr_t   ptr;      // VirtualAlloc'd RecvTable pointer
};
static std::vector<StubRecvTableEntry> s_pendingStubs;
static std::unordered_map<std::string, uintptr_t> s_stubRecvTables;
static bool s_stubsRegistered = false;
static size_t s_registeredStubCount = 0;
static uintptr_t s_stubOwnedListBase = 0;

bool Bridge_IsBareMapName(const char* psz);

// STL wrappers so __try callers (ProcessMessages) avoid MSVC C2712.
static constexpr size_t kMaxStubRecvTables = 256;
static constexpr uint32_t kMaxStubProps = 512;
static constexpr int kMaxStubChildDataTables = 64;

bool S21Bridge_CanRecordStub(const char* name)
{
	if (!name || !Bridge_IsBareMapName(name))
		return false;
	if (s_pendingStubs.size() >= kMaxStubRecvTables)
		return false;
	return true;
}

void S21Bridge_RecordStub(const char* name, uintptr_t ptr)
{
	if (!S21Bridge_CanRecordStub(name))
	{
		static int s_stubCapLog = 0;
		if (++s_stubCapLog <= 8)
			Warning(eDLL_T::ENGINE, "[BRIDGE-ST] stub refused '%s' (cap %zu or bad name)\n",
				name ? name : "?", kMaxStubRecvTables);
		return;
	}
	s_pendingStubs.push_back({std::string(name), ptr});
	s_stubRecvTables[std::string(name)] = ptr;
	s_s21RecvTableNames.insert(std::string(name));
}

uintptr_t S21Bridge_FindStub(const char* name)
{
	auto it = s_stubRecvTables.find(std::string(name));
	if (it == s_stubRecvTables.end()) return 0;
	return it->second;
}

static void __fastcall DummyRecvProxy(__int64, __int64, __int64*);

// Build stub RecvTable from SendTable wire; cursor must sit at nProps.
uintptr_t S21Bridge_BuildStubRecvTable(
	const char* tblName,
	const uint8_t* signonData,
	int totalBits,
	int& bitPos)
{
	// Re-implement the bit reader so this helper does not depend on the
	// caller's lambdas (lets us extract this routine cleanly).
	auto readBits = [&](int nBits) -> uint32_t {
		uint32_t val = 0;
		for (int b = 0; b < nBits && (bitPos + b) < totalBits; b++) {
			int bp = bitPos + b;
			if (signonData[bp / 8] & (1 << (bp % 8))) val |= (1u << b);
		}
		bitPos += nBits;
		return val;
	};
	auto readString = [&](char* out, int maxLen) {
		int i = 0;
		for (; i < maxLen - 1; i++) {
			if (bitPos + 8 > totalBits) { out[i] = 0; return; }
			uint32_t ch = readBits(8);
			out[i] = (char)ch;
			if (ch == 0) return;
		}
		out[maxLen - 1] = 0;
	};

	uint32_t nProps = readBits(10);
	if (nProps > kMaxStubProps) {
		BridgeStubDiag("[STUB] '%s' nProps=%u over cap %u, abort\n",
			tblName, nProps, kMaxStubProps);
		return 0;
	}

	// Allocate RecvTable (zeroed). 0x500 bytes covers fields through +0x4C8.
	uint8_t* rt = (uint8_t*)VirtualAlloc(
		nullptr, 0x500, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!rt) {
		BridgeStubDiag("[STUB] '%s' VirtualAlloc(rt) failed\n", tblName);
		return 0;
	}

	// Per-prop slot holds an array of CRecvProp* (pointers), each 8 bytes.
	// The CRecvProp struct itself is 0x68; allocate a separate contiguous
	// block for the prop bodies and fill the pointer array to point into it.
	uint8_t** propPtrArr = nullptr;
	uint8_t*  propBodies = nullptr;
	if (nProps > 0) {
		const size_t ptrsBytes  = (size_t)nProps * sizeof(uint8_t*);
		const size_t bodyBytes  = (size_t)nProps * 0x68;
		propPtrArr = (uint8_t**)VirtualAlloc(
			nullptr, ptrsBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		propBodies = (uint8_t*)VirtualAlloc(
			nullptr, bodyBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!propPtrArr || !propBodies) {
			if (propPtrArr) VirtualFree(propPtrArr, 0, MEM_RELEASE);
			if (propBodies) VirtualFree(propBodies, 0, MEM_RELEASE);
			VirtualFree(rt, 0, MEM_RELEASE);
			BridgeStubDiag("[STUB] '%s' VirtualAlloc(props %u) failed\n",
				tblName, nProps);
			return 0;
		}
	}

	int nChildDt = 0;
	// Parse each SendProp body, mirror into the stub RecvProp slot.
	for (uint32_t i = 0; i < nProps; i++) {
		uint32_t pType = readBits(5);
		char pName[256] = {};
		readString(pName, 256);
		uint32_t pFlags = readBits(16);
		(void)readBits(8); // priority -- SendProp-only, no RecvProp slot

		char    pDtName[256] = {};
		uint32_t pNElements = 0;

		if (pType == 10 /*DPT_DataTable*/ || (pFlags & 0x40) /*SPROP_EXCLUDE*/) {
			readString(pDtName, 256);
		} else if (pType == 5 /*DPT_Array*/) {
			pNElements = readBits(10);
		} else {
			// Skip wire-only fLow/fHigh/nBits -- engine reads these from the
			// wire SendProp side during CRecvDecoder construction.
			(void)readBits(32);
			(void)readBits(32);
			(void)readBits(7);
		}

		uint8_t* p = propBodies + (size_t)i * 0x68;
		// Canonical RecvProp layout (symbols)
		*(int*)         (p + 0x00) = (int)pType;             // recvType
		*(int*)         (p + 0x04) = 0;                      // offset (no real entity for stub class)
		*(uint32_t*)    (p + 0x14) = pFlags;                 // flags
		// stringBufferSize @ +0x18 -- left zero; engine sets defaults

		// dataTable @ +0x20: DPT_DataTable AssignClassIDs memcpy-clones 1240 bytes.
		// Zero-init clone (propCount=0) so the recursive walk is a no-op.
		if (pType == 10 /*DPT_DataTable*/) {
			if (++nChildDt > kMaxStubChildDataTables) {
				*(void**)(p + 0x20) = nullptr;
			} else {
				uint8_t* childBuf = (uint8_t*)VirtualAlloc(
					nullptr, 0x500, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				if (childBuf) {
					if (pDtName[0]) {
						*(const char**)(childBuf + 0x4C8) = _strdup(pDtName);
					}
					*(void**)(p + 0x20) = childBuf;
				}
			}
		}

		*(const char**) (p + 0x28) = _strdup(pName);         // pVarName
		// arrayProp @ +0x38, DataTableProxyFn @ +0x50 left null
		*(int*)         (p + 0x5C) = (int)pNElements;        // elementCount
		*(void**)       (p + 0x48) = (void*)&DummyRecvProxy;

		propPtrArr[i] = p;
	}

	// Canonical RecvTable layout (symbols)
	*(int*)        (rt + 0x000) = -1;                  // precalcDatatableIndex (unset)
	*(uint8_t***)  (rt + 0x008) = propPtrArr;          // props (RecvProp**)
	*(int*)        (rt + 0x010) = (int)nProps;         // propCount
	*(const char**)(rt + 0x4C8) = _strdup(tblName);    // netTableName
	*(uint8_t*)    (rt + 0x4D0) = 1;                   // initialized = true
	*(uint8_t*)    (rt + 0x4D1) = 0;                   // inMainList = false (set on register)

	BridgeStubDiag("[STUB] BUILT '%s' rt=%p propPtrs=%p bodies=%p nProps=%u\n",
		tblName, (void*)rt, (void*)propPtrArr, (void*)propBodies, nProps);

	return (uintptr_t)rt;
}

// Register stubs in the engine RecvTable list after Phase 4.5. Once, main thread.
void S21Bridge_RegisterStubsInList()
{
	if (s_pendingStubs.empty()) {
		BridgeStubDiag("[STUB-REG] no pending stubs, skip\n");
		return;
	}
	if (s_registeredStubCount >= s_pendingStubs.size()) {
		BridgeStubDiag("[STUB-REG] no new stubs, skip (registered=%zu)\n",
			s_registeredStubCount);
		return;
	}

	uintptr_t base = NetObs_GetExeModuleBase();
	if (!base) {
		BridgeStubDiag("[STUB-REG] no exe base; abort\n");
		return;
	}

	uintptr_t* pListBase = reinterpret_cast<uintptr_t*>(NetObs_RecvTableListBaseAddr());
	uint16_t*  pListHead = reinterpret_cast<uint16_t*>(NetObs_RecvTableListHeadAddr());

	uintptr_t oldListBase = *pListBase;
	uint16_t  oldHead     = *pListHead;
	if (!oldListBase) {
		BridgeStubDiag("[STUB-REG] engine listBase NULL; abort\n");
		return;
	}

	// Pool: +0x00 m_pData +0x08 m_nCapacity +0x18 m_heads (usedHead/freeTail).
	// Copy the full array so the free-list chain stays valid, then append stubs.
	uint16_t* pCapacity = reinterpret_cast<uint16_t*>(
		NetObs_RecvTableListCapacityAddr());
	const int oldCapacity = (int)*pCapacity;
	if (oldCapacity <= 0 || oldCapacity > 8192) {
		BridgeStubDiag("[STUB-REG] engine capacity=%d looks wrong; abort\n",
			oldCapacity);
		return;
	}

	// Walk used chain for diagnostic logging only.
	int chainLen = 0;
	{
		uint16_t s = oldHead;
		while (s != 0xFFFF && chainLen < 4096) {
			s = *reinterpret_cast<uint16_t*>(oldListBase + 16ULL * s + 10);
			++chainLen;
		}
	}

	const size_t stubStart = s_registeredStubCount;
	int stubCount    = (int)(s_pendingStubs.size() - stubStart);
	if (oldCapacity + stubCount > 0xFFFF)
		stubCount = 0xFFFF - oldCapacity;
	if (stubCount <= 0)
	{
		BridgeStubDiag("[STUB-REG] uint16 slot space exhausted (cap=%d); drop %zu stubs\n",
			oldCapacity, s_pendingStubs.size() - stubStart);
		return;
	}
	const int newCapacity  = oldCapacity + stubCount;

	BridgeStubDiag("[STUB-REG] chain=%d cap=%d newStubs=%d totalStubs=%zu newCap=%d head=%u\n",
		chainLen, oldCapacity, stubCount, s_pendingStubs.size(), newCapacity, (unsigned)oldHead);

	uint8_t* newArray = (uint8_t*)VirtualAlloc(
		nullptr, (size_t)newCapacity * 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!newArray) {
		BridgeStubDiag("[STUB-REG] VirtualAlloc(newArray %d*16) failed\n",
			newCapacity);
		return;
	}

	// Copy the ENTIRE old array (all capacity slots, including free-list
	// entries). This preserves the engine's free-list chain integrity, prev
	// pointers, and any metadata at +0x08/+0x0C per node.
	memcpy(newArray, (void*)oldListBase, (size_t)oldCapacity * 16);

	// Append stubs at [oldCapacity..); indices cannot collide with the free list.
	uint16_t newHead = oldHead;
	for (int i = 0; i < stubCount; i++) {
		const size_t stubIdx = stubStart + (size_t)i;
		const int slotIdx = oldCapacity + i;
		uint8_t* node = newArray + (size_t)slotIdx * 16;
		// Zero the slot, then set +0x00 (RecvTable*) and +0x0A (next index).
		// +0x08 / +0x0C left zero; if the engine's lookup needs them populated
		// the BridgeStubDiag trail will surface the failure.
		memset(node, 0, 16);
		*reinterpret_cast<uintptr_t*>(node + 0)  = s_pendingStubs[stubIdx].ptr;
		*reinterpret_cast<uint16_t*>(node + 10)  = newHead;
		newHead = (uint16_t)slotIdx;
		BridgeStubDiag("[STUB-REG] slot[%d] '%s' rt=%p next=%u\n",
			slotIdx, s_pendingStubs[stubIdx].name.c_str(),
			(void*)s_pendingStubs[stubIdx].ptr,
			(unsigned)*reinterpret_cast<uint16_t*>(node + 10));
	}

	// Order: listBase first, then capacity, then head. A reader between
	// writes sees (newArray, oldHead) which is safe because newArray's
	// [0..oldCapacity) is a verbatim copy of the engine's old entries.
	*pListBase = (uintptr_t)newArray;
	*pCapacity = (uint16_t)newCapacity;
	*pListHead = newHead;
	if (s_stubOwnedListBase && s_stubOwnedListBase == oldListBase)
		VirtualFree(reinterpret_cast<void*>(s_stubOwnedListBase), 0, MEM_RELEASE);
	s_stubOwnedListBase = (uintptr_t)newArray;

	BridgeStubDiag("[STUB-REG] swap done: listBase=%p head=%u cap=%u\n",
		(void*)newArray, (unsigned)newHead, (unsigned)*pCapacity);

	// Verification pass: walk the new list from the new head and confirm
	// every stub name is findable. This runs immediately after the swap on
	// the same thread so there's no race.
	{
		uintptr_t vBase = *pListBase;
		uint16_t  vHead = *pListHead;
		for (size_t i = stubStart; i < s_pendingStubs.size(); ++i) {
			auto& stub = s_pendingStubs[i];
			bool found = false;
			uint16_t vs = vHead;
			int vSafety = 0;
			while (vs != 0xFFFF && vSafety++ < 8192) {
				uintptr_t vNode = vBase + 16ULL * vs;
				uintptr_t vRT   = *reinterpret_cast<uintptr_t*>(vNode);
				if (vRT) {
					const char* vName = *reinterpret_cast<const char**>(vRT + 1224);
					if (vName && strcmp(vName, stub.name.c_str()) == 0) {
						found = true;
						break;
					}
				}
				vs = *reinterpret_cast<uint16_t*>(vNode + 10);
			}
			BridgeStubDiag("[STUB-VERIFY] '%s' %s (walked %d nodes)\n",
				stub.name.c_str(), found ? "FOUND" : "NOT FOUND", vSafety);
		}
	}

	s_stubsRegistered = true;
	s_registeredStubCount = s_pendingStubs.size();
}

// [CI-*] pack defined later (after MiniDump hook resolves s_origMiniDumpWriteDump).
bool S21Bridge_CIDiag_On(void);
void S21Bridge_CIDiag_Gate(const char* where);
void S21Bridge_CIDiag_STSummary(const char* tag, int matched, int flipped, int noDecoder);
void S21Bridge_CIDiag_Flush(void);

bool s_dbSawSendTables = false; // Did ANY transfer contain svc_SendTable?

//-----------------------------------------------------------------------------
// Packet capture removed (per-packet fopen froze the game thread).
// S3 SendDatagram / WriteSubChannelData header layout matches live captures.
//-----------------------------------------------------------------------------
uint32_t s_c2sSeqCounter     = 0;          // Bridge-local C2S sequence counter

//-----------------------------------------------------------------------------
// Flow stats: index 0 FLOW_OUTGOING, 1 FLOW_INCOMING. Native FlowUpdate is blind.
//-----------------------------------------------------------------------------
BridgeFlowStat s_flowStat[2] = {};          // published (read by the FlowUpdate hook)
// RTT: relay-send time vs ack, in engine net_time. avglatency is not native here.
double    s_relaySendTime[1024] = {};       // net_time when relay seq (idx) was sent
long long s_outBytesAcc    = 0;             // outgoing relay bytes this window (FlushC2SNow)

// Engine net_time (seconds) -- the same clock the native latency math samples.
uint32_t s_bridgeNonceHost   = 0x0000CAFE;  // Our nonce (sent in WriteSubChannelData)
uint32_t s_serverNonce       = 0;           // Server's nonce (from its WriteSubChannelData)
bool     s_needNonceAck      = false;       // Need to ACK server's nonce in next C2S
bool     s_serverAckedUs     = false;       // Server ACKed our nonce (reliable delivered)
uint32_t s_bridgeSubSeq      = 0;           // Our subchannel sequence number

// Phase 5: Track S2C sequence for proper ack values in self-clocked C2S
uint32_t s_bridgeInSeqNr     = 0;           // Highest S2C seq received from server
// Phase 2: Server subchannel state for correct ACK
uint32_t s_serverSubSeqRecv  = 0;           // Server subchannel entries received
// The dedi retransmits its whole waiting list at subSeq 0 until we ack it, so
// the list-restart rebase has to be one-shot per restart. Rebasing on every
// retransmit re-dispatches entry 0 forever.
bool     s_subSeqRebased     = false;
bool     s_serverNonceCaptured = false;      // Have we captured server's nonce from subchannel?
// Phase 1: Self-clocking C2S sender
double   s_lastBridgeC2STime = 0.0;         // Timestamp of last self-clocked C2S send
static double   s_lastEngineFrameMs = 0.0;         // GetTickCount64 at last Hook_EngineFrame
bool     s_dbBlockStatus[BRIDGE_DB_MAX_FRAGMENTS] = {}; // Per-block received status
uint16_t s_dbBlockSizes[BRIDGE_DB_MAX_FRAGMENTS] = {};  // Bytes actually received per block
int      s_dbTransferSize    = 0;             // Total transfer size from server
int      s_dbTotalBlocks     = 0;             // ceil(transferSize / 1024)
int      s_dbBlocksReceived  = 0;             // Blocks received so far
uint16_t s_dbTransferId      = 0;             // Current transfer ID
uint16_t s_dbTransferNr      = 0xFFFF;        // Current transfer Nr (0xFFFF = none)
bool     s_dbComplete        = false;         // Current transfer complete
int      s_dbTransferCount   = 0;             // How many transfers completed
// s_dbSawSendTables is declared above the reliable SendTable preprocessor so
// both DataBlock and netchannel-reliable paths update the same phase flag.

// Signon DataBlock: net thread copies compressed payload; Hook_Cbuf_Execute drains
// on the main thread. Two buffers + deferred ACK keep transfers sequential.
uint8_t*      s_pendingSignonBuf    = nullptr;  // net thread writes (lazy 4MB)
int           s_pendingSignonSize   = 0;
volatile bool s_pendingSignonReady  = false;
static uint8_t*      s_processingSignonBuf = nullptr;  // main thread working copy (lazy 4MB)

// Defer net_SignonState(NEW=3) until SendTables exist so case 3 is not skipped
// (early-return a2 <= currentState).
bool     s_deferredSignon3       = false;
int      s_deferredSignon3Bits   = 0;
int      s_deferredSignon3Spawn  = -1;
int      s_reliableSize      = 0;           // 0 = nothing to send
int             s_lastSentSignonState = -1; // monotonic: only send if state > last sent
bool            s_pendingUserInfo    = false; // send NET_SetConVar after CONNECTED
int             s_userInfoSendsLeft  = 0;
ULONGLONG       s_userInfoNextMs     = 0;

void S21Bridge_ArmUserInfo(void)
{
	s_pendingUserInfo = true;
	s_userInfoSendsLeft = kBridgeUserInfoCopies;
	s_userInfoNextMs = 0;
}

void S21Bridge_ClearUserInfo(void)
{
	s_pendingUserInfo = false;
	s_userInfoSendsLeft = 0;
	s_userInfoNextMs = 0;
}

// S3 CHANGELEVEL=9, S21 MAYRECONNECT=9 / CHANGELEVEL=10. Remap 9->10.
// Track sequence key vs spawn count separately; SV_ActivateServer sends spawn -1.
int  s_signonSeqKey   = -1;   // raw wire spawn that identifies the sequence
int  s_signonSeqSpawn = -1;   // last real spawn count, used on C2S echoes
int  s_signonSeqMax   = -1;   // highest signon state dispatched this sequence
bool s_signonSeqDidCL = false;// CHANGELEVEL already dispatched for this sequence
bool s_signonSawStateSinceCL = false; // ordinary signon state seen since last CHANGELEVEL
bool s_signonRewalk   = false;// post-changelevel walk in progress (loud tell)

// Rewalk trace is budgeted per changelevel. Per-packet vs signon emitters share
// no budget: a long load must not spend it before the hanging rung.
int  s_rewalkTraceLeft = 0;
int  s_rewalkTraceSignonLeft = 0;
bool S21Bridge_RewalkTrace(void)
{
	if (!s_signonRewalk || s_rewalkTraceLeft <= 0)
		return false;
	--s_rewalkTraceLeft;
	return true;
}

bool S21Bridge_RewalkTraceSignon(void)
{
	if (!s_signonRewalk || s_rewalkTraceSignonLeft <= 0)
		return false;
	--s_rewalkTraceSignonLeft;
	return true;
}

// Re-ask the current signon rung until the server answers or the window expires.
int       s_signonEchoState    = -1;
ULONGLONG s_signonEchoUntilMs  = 0;

void S21Bridge_ResetReliableRecv(const char* reason);

//-----------------------------------------------------------------------------
// C2S: slice clc_Move + clc_ClientTick from m_StreamUnreliable before SendDatagram.
// IDs 62->46, 70->60. ClientTick is last 71 bits; skip the frame if types mismatch.
//-----------------------------------------------------------------------------
static uint8_t   s_c2sPendBuf[4096];
bf_write  s_c2sPend(s_c2sPendBuf, (int)sizeof(s_c2sPendBuf));
// Main thread fills s_c2sPend (CL_Move relay / stringcmd); NET thread drains it
// (SelfClockC2S via PollReceive). Covers pend writes and the whole FlushC2SNow body.
SRWLOCK   s_c2sTxLock = SRWLOCK_INIT;
uint32_t  s_stringCmdSeq = 0; // last assigned seq (first sent is 1)
StringCmdResend_s s_stringCmdRing[32];
int s_nStringCmdRingHead = 0;
SetConVarResend_s s_setConVarRing[4];
int s_nSetConVarRingHead = 0;
// [SEC prio14] Gate permanent net crypto force-off (S3 interop still needs plaintext today).
static ConVar bridge_force_encryption_off("bridge_force_encryption_off", "1", FCVAR_RELEASE,
	"[SEC] Force net_encryptionEnable/DTLS off for S3 bridge interop. Default 1 (required "
	"today; traffic is plaintext on the wire -- MITM risk on shared nets). Set 0 to leave "
	"engine crypto defaults (handshake will fail against stock S3 dedi until crypto matches).");
// [SEC C8] Opt-in DEVGATE neuter; also auto-on with -devsdk. Default 0 = ship-safe.
static ConVar sdk_devgate_neuter("sdk_devgate_neuter", "0", FCVAR_RELEASE,
	"[SEC] Opt-in: neuter engine FCVAR_DEVELOPMENTONLY / FCVAR_HIDDEN set gates so "
	"dev/hidden cvars are console-settable. Default 0 = stock reject (ship-safe). "
	"Also auto-enabled when -devsdk is on the command line.");

// Native FlowNewPacket(FLOW_INCOMING) never runs. Re-issue it so outgoing loss
// is truthful (else avgloss pins ~100% and the packet-loss icon stays lit).
ConVar bridge_net_flow_reconcile("bridge_net_flow_reconcile", "1", FCVAR_RELEASE,
	"Feed the native netchannel flow accounting the inputs the bridge's ProcessPacket "
	"substitution drops, so the netgraph shows REAL loss (not a forced 0). ON (DEFAULT): "
	"(a) FlowNewPacket(FLOW_INCOMING) per inbound packet -> real incoming loss from S2C "
	"sequence gaps; (b) mark outgoing frames acked up to the dedi's sequenceAck -> real "
	"outgoing loss from FlowUpdate (0 when the dedi acks us, >0 on genuine loss). 0 = OFF: "
	"outgoing avgloss pins ~100% (icon lit), incoming frozen.");

// Previous cmd state for delta defaults. Reset in S21Bridge_ResetAllState.
// Plain static (not thread_local) -- emit always runs on the bridge thread and we want
// S21Bridge_ResetAllState to clear it regardless of which thread calls reset.
S21BridgeCmd::State s_bridgeC2sPrevCmd = {};

// Last emitted S3 impulse byte (post-smuggle), not client impulse. Delta baseline
// must match the dedi's last store. Reset with s_bridgeC2sPrevCmd.
uint8_t s_bridgeC2sPrevS3ImpulseWire = 0;

uint32_t s_ftEstLastCmdNr = 0;    // newest command_number the estimator has seen
double   s_ftEstLastWall  = 0.0;  // Plat_FloatTime at that observation
float    s_ftEstPerCmd    = 0.0f; // EWMA per-cmd wall interval (0 = not primed)
AckXlate_s s_ackXlate[1024];

// Forward: defined with the other sockaddr helpers further down.
static void FormatSockaddr(const sockaddr* addr, char* buf, size_t bufSize);

// Engine sendto sockaddr (often loopback). Distinct from s_bridgeDest (real S3 peer).
static sockaddr_storage s_engineServerAddr = {};
static int              s_engineServerAddrLen = 0;
static bool             s_engineServerAddrCaptured = false;

// Connect-string target (authoritative for remote). Set in Connect_Worker
// before the first C2S_CHALLENGE so handshake never falls back to 127.0.0.1
// when the user typed a public host:port. Port 0 / empty = unset.
static constexpr uint16_t kBridgeDefaultGamePort = 37015;
static uint16_t s_connectTargetPort = 0; // host order
static bool     s_connectTargetIsLoopback = false;
static bool     s_bridgeDestResolved = false; // s_bridgeDest has a real peer
bool S21Bridge_DestResolved(void) { return s_bridgeDestResolved; }

bool S21Bridge_FromMatchesPeer(const sockaddr* pFrom, int nFromLen)
{
	if (!pFrom || !s_bridgeDestResolved)
		return false;

	sockaddr_in6 from6 = {};
	if (pFrom->sa_family == AF_INET6 && nFromLen >= (int)sizeof(sockaddr_in6))
	{
		memcpy(&from6, pFrom, sizeof(sockaddr_in6));
	}
	else if (pFrom->sa_family == AF_INET && nFromLen >= (int)sizeof(sockaddr_in))
	{
		const sockaddr_in* v4 = reinterpret_cast<const sockaddr_in*>(pFrom);
		from6.sin6_family = AF_INET6;
		from6.sin6_port = v4->sin_port;
		from6.sin6_addr.u.Byte[10] = 0xFF;
		from6.sin6_addr.u.Byte[11] = 0xFF;
		memcpy(from6.sin6_addr.u.Byte + 12, &v4->sin_addr, 4);
	}
	else
		return false;

	if (from6.sin6_port != s_bridgeDest.sin6_port)
		return false;
	static const uint8_t kV4MapPrefix[12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };
	const bool bFromV4 = memcmp(from6.sin6_addr.u.Byte, kV4MapPrefix, 12) == 0;
	const bool bDstV4 = memcmp(s_bridgeDest.sin6_addr.u.Byte, kV4MapPrefix, 12) == 0;
	if (bFromV4 != bDstV4)
		return false;
	const int nCmpBytes = bFromV4 ? 4 : 16;
	return memcmp(from6.sin6_addr.u.Byte + (bFromV4 ? 12 : 0),
		s_bridgeDest.sin6_addr.u.Byte + (bDstV4 ? 12 : 0), nCmpBytes) == 0;
}
static char     s_connectTargetStr[256] = {};

static uint16_t Bridge_ReadHostPortConvar(void)
{
	if (g_pCVar)
	{
		ConVar* const p = g_pCVar->FindVar("hostport");
		if (p)
		{
			const int v = p->GetInt();
			if (v > 0 && v <= 65535)
				return static_cast<uint16_t>(v);
		}
	}
	return kBridgeDefaultGamePort;
}

static uint16_t Bridge_SockaddrPortHost(const sockaddr* to)
{
	if (!to)
		return 0;
	if (to->sa_family == AF_INET6)
		return ntohs(reinterpret_cast<const sockaddr_in6*>(to)->sin6_port);
	if (to->sa_family == AF_INET)
		return ntohs(reinterpret_cast<const sockaddr_in*>(to)->sin_port);
	return 0;
}

static bool Bridge_SockaddrIsLoopback(const sockaddr* to)
{
	if (!to)
		return false;
	if (to->sa_family == AF_INET)
	{
		const uint32_t a = ntohl(reinterpret_cast<const sockaddr_in*>(to)->sin_addr.s_addr);
		return (a >> 24) == 127;
	}
	if (to->sa_family == AF_INET6)
	{
		const unsigned char* const ip =
			reinterpret_cast<const unsigned char*>(
				&reinterpret_cast<const sockaddr_in6*>(to)->sin6_addr);
		//::1
		bool allZero = true;
		for (int i = 0; i < 15; ++i)
		{
			if (ip[i]) { allZero = false; break; }
		}
		if (allZero && ip[15] == 1)
			return true;
		//::ffff:127.x.x.x
		bool v4m = true;
		for (int i = 0; i < 10; ++i)
		{
			if (ip[i]) { v4m = false; break; }
		}
		if (v4m && ip[10] == 0xFF && ip[11] == 0xFF && ip[12] == 127)
			return true;
	}
	return false;
}

static uint16_t Bridge_GetActiveGamePort(void)
{
	if (s_connectTargetPort)
		return s_connectTargetPort;
	if (s_engineServerAddrCaptured)
	{
		const uint16_t p = Bridge_SockaddrPortHost(
			reinterpret_cast<const sockaddr*>(&s_engineServerAddr));
		if (p)
			return p;
	}
	if (s_bridgeDestResolved)
	{
		const uint16_t p = ntohs(s_bridgeDest.sin6_port);
		if (p)
			return p;
	}
	return Bridge_ReadHostPortConvar();
}

// True if this UDP dest port is the game server we are connecting/connected to.
// Replaces the hard-coded "destPort == 37015" gates that broke every non-default
// hostport and every VPS listen port.
static bool Bridge_IsBridgeGamePort(const uint16_t port)
{
	if (!port)
		return false;
	if (s_connectTargetPort && port == s_connectTargetPort)
		return true;
	if (s_bridgeDestResolved)
	{
		const uint16_t bp = ntohs(s_bridgeDest.sin6_port);
		if (bp && port == bp)
			return true;
	}
	if (s_engineServerAddrCaptured)
	{
		const uint16_t cp = Bridge_SockaddrPortHost(
			reinterpret_cast<const sockaddr*>(&s_engineServerAddr));
		if (cp && port == cp)
			return true;
	}
	// Pre-connect (no target yet): accept hostport + default so the first
	// challenge still arms the bridge on a stock local setup.
	if (!s_connectTargetPort && !s_bridgeDestResolved && !s_engineServerAddrCaptured)
	{
		if (port == Bridge_ReadHostPortConvar() || port == kBridgeDefaultGamePort)
			return true;
	}
	return false;
}

static void Bridge_ClearConnectTarget(void)
{
	s_connectTargetPort = 0;
	s_connectTargetIsLoopback = false;
	s_bridgeDestResolved = false;
	s_connectTargetStr[0] = '\0';
	s_engineServerAddrCaptured = false;
	s_engineServerAddrLen = 0;
	memset(&s_engineServerAddr, 0, sizeof(s_engineServerAddr));
	memset(&s_bridgeDest, 0, sizeof(s_bridgeDest));
}

static void Bridge_SetBridgeDestV4Mapped(const uint8_t a, const uint8_t b,
	const uint8_t c, const uint8_t d, const uint16_t portHost)
{
	memset(&s_bridgeDest, 0, sizeof(s_bridgeDest));
	s_bridgeDest.sin6_family = AF_INET6;
	s_bridgeDest.sin6_port = htons(portHost);
	s_bridgeDest.sin6_addr.u.Byte[10] = 0xFF;
	s_bridgeDest.sin6_addr.u.Byte[11] = 0xFF;
	s_bridgeDest.sin6_addr.u.Byte[12] = a;
	s_bridgeDest.sin6_addr.u.Byte[13] = b;
	s_bridgeDest.sin6_addr.u.Byte[14] = c;
	s_bridgeDest.sin6_addr.u.Byte[15] = d;
	s_bridgeDestResolved = true;
}

static void Bridge_SetBridgeDestFromSockaddr(const sockaddr* to, const int tolen)
{
	if (!to || tolen <= 0)
		return;

	if (to->sa_family == AF_INET6 && tolen >= (int)sizeof(sockaddr_in6))
	{
		memcpy(&s_bridgeDest, to, sizeof(sockaddr_in6));
		s_bridgeDestResolved = true;
		return;
	}
	if (to->sa_family == AF_INET && tolen >= (int)sizeof(sockaddr_in))
	{
		const sockaddr_in* const p4 = reinterpret_cast<const sockaddr_in*>(to);
		const unsigned char* const ip =
			reinterpret_cast<const unsigned char*>(&p4->sin_addr);
		Bridge_SetBridgeDestV4Mapped(ip[0], ip[1], ip[2], ip[3], ntohs(p4->sin_port));
	}
}

// Parse "host", "host:port", "[v6]:port" into s_bridgeDest + connect target state.
// Returns true if a usable peer address was resolved.
static bool Bridge_SetConnectTargetFromString(const char* addrStr)
{
	if (!addrStr || !addrStr[0])
		return false;

	// Strip surrounding quotes / whitespace.
	while (*addrStr == ' ' || *addrStr == '\t' || *addrStr == '"')
		++addrStr;
	char raw[256];
	V_strncpy(raw, addrStr, sizeof(raw));
	size_t n = strlen(raw);
	while (n > 0 && (raw[n - 1] == ' ' || raw[n - 1] == '\t' || raw[n - 1] == '"'
		|| raw[n - 1] == '\n' || raw[n - 1] == '\r'))
		raw[--n] = '\0';
	if (!raw[0])
		return false;

	V_strncpy(s_connectTargetStr, raw, sizeof(s_connectTargetStr));

	char host[256];
	uint16_t port = Bridge_ReadHostPortConvar();
	host[0] = '\0';

	if (raw[0] == '[')
	{
		// [ipv6]:port
		const char* br = strchr(raw, ']');
		if (!br)
			return false;
		const size_t hn = static_cast<size_t>(br - raw - 1);
		if (hn == 0 || hn >= sizeof(host))
			return false;
		memcpy(host, raw + 1, hn);
		host[hn] = '\0';
		if (br[1] == ':')
		{
			const int p = atoi(br + 2);
			if (p > 0 && p <= 65535)
				port = static_cast<uint16_t>(p);
		}
	}
	else
	{
		// IPv4 host:port (single colon) or bare host / multi-colon IPv6 without brackets.
		const char* colon = strchr(raw, ':');
		if (colon && !strchr(colon + 1, ':'))
		{
			const size_t hn = static_cast<size_t>(colon - raw);
			if (hn == 0 || hn >= sizeof(host))
				return false;
			memcpy(host, raw, hn);
			host[hn] = '\0';
			const int p = atoi(colon + 1);
			if (p > 0 && p <= 65535)
				port = static_cast<uint16_t>(p);
		}
		else
		{
			V_strncpy(host, raw, sizeof(host));
		}
	}

	s_connectTargetPort = port;

	// Keep engine hostport in sync so any path that reads the convar (not the
	// connect string) still hits the VPS listen port.
	if (g_pCVar)
	{
		ConVar* const pHostPort = g_pCVar->FindVar("hostport");
		if (pHostPort && pHostPort->GetInt() != static_cast<int>(port))
		{
			pHostPort->SetValue(static_cast<int>(port));
			SDK_Log("[NET-OBS] hostport -> %u (from connect '%s')\n",
				(unsigned)port, s_connectTargetStr);
		}
	}

	// Loopback hostnames
	if (!V_stricmp(host, "localhost") || !V_stricmp(host, "localhost.")
		|| !V_stricmp(host, "::1") || !V_strncmp(host, "127.", 4))
	{
		s_connectTargetIsLoopback = true;
		Bridge_SetBridgeDestV4Mapped(127, 0, 0, 1, port);
		// Also seed engine capture as v4-mapped loopback so early paths work.
		sockaddr_in6 local6 = {};
		local6.sin6_family = AF_INET6;
		local6.sin6_port = htons(port);
		local6.sin6_addr.u.Byte[10] = 0xFF;
		local6.sin6_addr.u.Byte[11] = 0xFF;
		local6.sin6_addr.u.Byte[12] = 127;
		local6.sin6_addr.u.Byte[15] = 1;
		memcpy(&s_engineServerAddr, &local6, sizeof(local6));
		s_engineServerAddrLen = sizeof(local6);
		s_engineServerAddrCaptured = true;
		SDK_Log("[NET-OBS] connect target LOOPBACK %s -> [::ffff:127.0.0.1]:%u\n",
			host, (unsigned)port);
		return true;
	}

	// Dotted IPv4
	{
		unsigned a = 0, b = 0, c = 0, d = 0;
		if (sscanf_s(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4
			&& a <= 255 && b <= 255 && c <= 255 && d <= 255
			&& !strchr(host, ':'))
		{
			s_connectTargetIsLoopback = (a == 127);
			Bridge_SetBridgeDestV4Mapped(
				static_cast<uint8_t>(a), static_cast<uint8_t>(b),
				static_cast<uint8_t>(c), static_cast<uint8_t>(d), port);
			sockaddr_in6 v4m = s_bridgeDest;
			memcpy(&s_engineServerAddr, &v4m, sizeof(v4m));
			s_engineServerAddrLen = sizeof(v4m);
			s_engineServerAddrCaptured = true;
			SDK_Log("[NET-OBS] connect target IPv4 %u.%u.%u.%u:%u (v4-mapped)\n",
				a, b, c, d, (unsigned)port);
			return true;
		}
	}

	// Literal IPv6 (no brackets left in host)
	{
		sockaddr_in6 v6 = {};
		v6.sin6_family = AF_INET6;
		v6.sin6_port = htons(port);
		if (inet_pton(AF_INET6, host, &v6.sin6_addr) == 1)
		{
			s_connectTargetIsLoopback = Bridge_SockaddrIsLoopback(
				reinterpret_cast<const sockaddr*>(&v6));
			memcpy(&s_bridgeDest, &v6, sizeof(v6));
			s_bridgeDestResolved = true;
			memcpy(&s_engineServerAddr, &v6, sizeof(v6));
			s_engineServerAddrLen = sizeof(v6);
			s_engineServerAddrCaptured = true;
			SDK_Log("[NET-OBS] connect target IPv6 '%s':%u\n", host, (unsigned)port);
			return true;
		}
	}

	// Hostname -- resolve via getaddrinfo (prefer IPv4 for dual-stack VPS).
	{
		addrinfo hints = {};
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		addrinfo* res = nullptr;
		char portStr[16];
		_snprintf_s(portStr, sizeof(portStr), _TRUNCATE, "%u", (unsigned)port);
		const int ga = getaddrinfo(host, portStr, &hints, &res);
		if (ga == 0 && res)
		{
			bool ok = false;
			// Prefer AF_INET then AF_INET6
			for (int pass = 0; pass < 2 && !ok; ++pass)
			{
				const int want = (pass == 0) ? AF_INET : AF_INET6;
				for (addrinfo* p = res; p; p = p->ai_next)
				{
					if (p->ai_family != want || !p->ai_addr)
						continue;
					Bridge_SetBridgeDestFromSockaddr(p->ai_addr, (int)p->ai_addrlen);
					if (p->ai_addrlen <= sizeof(s_engineServerAddr))
					{
						memcpy(&s_engineServerAddr, p->ai_addr, p->ai_addrlen);
						s_engineServerAddrLen = (int)p->ai_addrlen;
						s_engineServerAddrCaptured = true;
					}
					s_connectTargetIsLoopback = Bridge_SockaddrIsLoopback(p->ai_addr);
					ok = s_bridgeDestResolved;
					break;
				}
			}
			freeaddrinfo(res);
			if (ok)
			{
				SDK_Log("[NET-OBS] connect target resolved '%s' -> port %u loopback=%d\n",
					host, (unsigned)port, s_connectTargetIsLoopback ? 1 : 0);
				return true;
			}
		}
		else
		{
			SDK_Log("[NET-OBS] connect target getaddrinfo('%s') failed ga=%d\n",
				host, ga);
		}
	}

	SDK_Log("[NET-OBS] connect target PARSE FAILED for '%s' (port=%u)\n",
		raw, (unsigned)port);
	return false;
}

// Capture engine peer sockaddr for recvfrom mirroring. Never demote a resolved
// remote s_bridgeDest to loopback when the engine later sends to 127.0.0.1.
static void Bridge_NoteEngineServerSendto(const sockaddr* to, const int tolen)
{
	if (!to || tolen <= 0 || tolen > (int)sizeof(s_engineServerAddr))
		return;

	const uint16_t destPort = Bridge_SockaddrPortHost(to);
	if (!Bridge_IsBridgeGamePort(destPort))
		return;

	const bool isLb = Bridge_SockaddrIsLoopback(to);

	// Always refresh the engine-visible peer (validation / from spoof).
	memcpy(&s_engineServerAddr, to, tolen);
	s_engineServerAddrLen = tolen;
	s_engineServerAddrCaptured = true;

	// Real S3 dest: keep remote if we already have one and this send is loopback
	// (post-netchan local shortcut). Update when non-loopback, or when connect
	// itself is loopback, or when we have no dest yet.
	if (!isLb || s_connectTargetIsLoopback || !s_bridgeDestResolved)
	{
		Bridge_SetBridgeDestFromSockaddr(to, tolen);
		if (!s_connectTargetPort)
			s_connectTargetPort = destPort;
		if (isLb)
			s_connectTargetIsLoopback = true;
	}

	static long long s_capLog = 0;
	if (++s_capLog <= 8 || (s_capLog % 64) == 0)
	{
		char ab[96];
		FormatSockaddr(to, ab, sizeof(ab));
		char db[96];
		FormatSockaddr(reinterpret_cast<const sockaddr*>(&s_bridgeDest), db, sizeof(db));
		SDK_Log("[NET-OBS] BRIDGE: engine peer=%s bridgeDest=%s port=%u lb=%d resolved=%d\n",
			ab, db, (unsigned)destPort, isLb ? 1 : 0, s_bridgeDestResolved ? 1 : 0);
	}
}

// Originals captured by DetourAttach. We point to the game's function
// initially, then DetourAttach swaps them to trampolines that jump to the
// real implementation.
typedef void (*PFN_Cbuf_AddText)(ECommandTarget_t eTarget, const char* pText, cmd_source_t cmdSource);
typedef void (*PFN_Cbuf_Execute)(void);
typedef __int64 (*PFN_Cmd_ExecuteString)(unsigned int player, void* parsedCmd, int source);
typedef void (*PFN_Connect_Worker)(void** addrPtrPtr);
typedef SOCKET (WSAAPI *PFN_WSASocketW)(int af, int type, int protocol,
                                        LPWSAPROTOCOL_INFOW lpProtocolInfo,
                                        GROUP g, DWORD dwFlags);
typedef int  (WSAAPI *PFN_WSASendTo)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
                                     LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
                                     const sockaddr* lpTo, int iTolen,
                                     LPWSAOVERLAPPED lpOverlapped,
                                     LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
typedef int  (WSAAPI *PFN_WSARecvFrom)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
                                       LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
                                       sockaddr* lpFrom, LPINT lpFromlen,
                                       LPWSAOVERLAPPED lpOverlapped,
                                       LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

// Engine NET_SendPacket (not OS sendto). Public form has 6 args
// (mov r10d, [rsp+78h] reads a6 as 32-bit from the second stack slot).
typedef __int64 (*PFN_NET_SendPacket_Public)(__int64 a1, unsigned int a2, __int64 a3,
                                             const void* a4, unsigned int a5, unsigned int a6);
typedef __int64 (*PFN_NET_SendPacket_Inner)(__int64 a1, __int64 a2, __int64 a3,
                                            const void* a4, int a5, unsigned int* a6, char a7);
// NET_SendLoopback: 4-arg calls are ABI-compatible with the variadic decl.
typedef void    (*PFN_NET_SendLoopback)(unsigned int a1, int a2, const void* a3, __int64 a4);
typedef char    (*PFN_CBaseClient_SetSignonState)(__int64 a1, unsigned int a2, int a3, __int64 a4);
typedef bool    (*PFN_NET_ReceiveDatagram)(int iSocket, void* pInpacket, bool bEncrypted);

// Case-3 init: skip if struct +0xA8/+0xB0 are sentinel 0xFF..FF (memset would AV).
typedef void    (*PFN_Case3InitHelper)(void);


static PFN_Cbuf_AddText             s_origCbufAddText            = nullptr;
static PFN_Cbuf_Execute             s_origCbufExecute            = nullptr;
static PFN_Cmd_ExecuteString        s_origCmdExecuteString       = nullptr;
static PFN_Connect_Worker           s_origConnectWorker          = nullptr;
static PFN_WSASocketW               s_origWSASocketW             = nullptr;
PFN_sendto                   s_origSendto                 = nullptr;
static PFN_WSASendTo                s_origWSASendTo              = nullptr;
PFN_recvfrom                 s_origRecvfrom               = nullptr;
static PFN_WSARecvFrom              s_origWSARecvFrom            = nullptr;
static SOCKET                       s_challengeSock              = INVALID_SOCKET;
static volatile LONG                s_dest04Injected             = 0;
static PFN_NET_SendPacket_Public    s_origNetSendPacketPublic    = nullptr;
static PFN_NET_SendPacket_Inner     s_origNetSendPacketInner     = nullptr;
static PFN_NET_SendLoopback         s_origNetSendLoopback        = nullptr;
static PFN_CBaseClient_SetSignonState s_origSetSignonState       = nullptr;
static PFN_Case3InitHelper             s_origCase3InitHelper             = nullptr;

static void BridgeSnapBudget_Changed(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t)
{
	NOTE_UNUSED(flOldValue);
	const int now = var ? static_cast<ConVar*>(var)->GetInt() : -1;
	Warning(eDLL_T::CLIENT, "[SNAP-BUDGET] lever=%d (was %s)\n",
		now, pOldValue ? pOldValue : "?");
}

static ConVar bridge_snap_budget("bridge_snap_budget", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL,
	"[SNAP-BUDGET] blit+decode+C2S+packet+frame cost. "
	"0 = off, 1 = every 20th, 2 = every item. "
	"Tags: [SNAP-BUDGET] [C2S-BUDGET] [PP-BUDGET].",
	true, 0.f, true, 2.f, BridgeSnapBudget_Changed);

int BridgeBudget_Level(void)
{
	const int live = bridge_snap_budget.GetInt();
	if (live > 0)
		return live;
	static int s_cmdLine = -1;
	if (s_cmdLine < 0)
	{
		s_cmdLine = 0;
		const char* const cl = GetCommandLineA();
		const char* p = cl ? strstr(cl, "+bridge_snap_budget") : nullptr;
		if (p)
		{
			p += 19;
			while (*p == ' ' || *p == '\t')
				++p;
			const int v = atoi(p);
			s_cmdLine = (v > 2) ? 2 : ((v > 0) ? v : 1);
			Warning(eDLL_T::CLIENT, "[SNAP-BUDGET] cmdline fallback level=%d (GetInt was 0)\n", s_cmdLine);
		}
	}
	return s_cmdLine;
}

bool BridgeBudget_Armed(int level, long n)
{
	return (level >= 2) || (level == 1 && (n % 20) == 0);
}

double BridgeBudget_Ms(LONGLONG a, LONGLONG b, LONGLONG freq)
{
	if (freq <= 0)
		return 0.0;
	return 1000.0 * (double)(b - a) / (double)freq;
}


// Safe bounded string copy out of game memory (preflight, no SEH).
void DF_CopyStr(uintptr_t p, char* out, int cap)
{
	out[0] = 0;
	if (!p || cap < 2) return;
	// Clamp to the readable prefix (page-edge strings may not cover full cap).
	size_t want = static_cast<size_t>(cap);
	size_t okBytes = want;
	if (!Bridge_RegionReadable(p, want))
	{
		okBytes = 0;
		while (okBytes + 0x40 <= want && Bridge_RegionReadable(p + okBytes, 0x40))
			okBytes += 0x40;
		while (okBytes < want && Bridge_RegionReadable(p + okBytes, 1))
			++okBytes;
	}
	if (!okBytes)
		return;
	const char* s = reinterpret_cast<const char*>(p);
	const int maxCopy = static_cast<int>(
		(okBytes < static_cast<size_t>(cap - 1)) ? okBytes : static_cast<size_t>(cap - 1));
	int j = 0;
	for (; j < maxCopy && s[j]; ++j)
		out[j] = s[j];
	out[j] = 0;
}

uintptr_t NetObs_ModelPrecacheTablePtrAddr() // exposed for world_model_invis_diag_s21
{
	return NetObs_Sym(NetObsSym_t::ModelPrecacheTablePtr);
}

uintptr_t NetObs_ModelPrecacheItemsAddr() // exposed for world_model_invis_diag_s21
{
	return NetObs_Sym(NetObsSym_t::ModelPrecacheItems);
}


void Bridge_InstallApplyPipeHooks(void);     // forward decl


//=============================================================================
// Apply-pipeline probe: A frame gates, B per-entity, C per-prop.
//=============================================================================
typedef char     (__fastcall *PFN_ClProcessFrame)(__int64 a1);
typedef __int64  (__fastcall *PFN_TransitionEntity)(__int64, __int64, int*, unsigned int, __int64*, __int64, __int64, int, int, char, unsigned int, char);
typedef __int64  (__fastcall *PFN_PropApplyLoop)(__int64, __int64, int*, unsigned int, __int64*, __int64, __int64, int, int, char, unsigned int, char);
typedef void     (__fastcall *PFN_ClProcessSnapshotWT)(__int64 a1, __int64 a2, __int64 a3, __int64 a4);
typedef void     (__fastcall *PFN_ClSendTick)();
typedef char     (__fastcall *PFN_EngineFrame)(void* a1);
typedef double   (__fastcall *PFN_HostFrame)(double a1, float a2);
typedef void     (__fastcall *PFN_HostStateFrame)(__int64 a1, double a2, float a3);
// PlayerRunCommand_Prediction wrapper. Observe whether the entity reaches this site.
typedef __int64  (__fastcall *PFN_PlayerRunCmdPred)(__int64 a1, __int64 a2, __int64 a3, __int64* a4);
// [NOCLIP-SIM] == C_GameMovement::PlayerMove (twin, mapped via the
// m_MoveType (+0x3B2) switch: pattern "48 8B C4 55 41 54 48 8D A8 28 F9 FF FF 48 81 EC C8 07 00 00

// S21 PlayerMove movetype case 9 (NOCLIP) falls through to default; sv_noclipspeed* absent -- no client noclip sim.
typedef void     (__fastcall *PFN_PlayerMoveClient)(__int64 thisptr);
// -- the client clock-drift controller. a2 = server tick.
// Computes [0x13C] = clamp(18 / sum_of_18_rate_samples, 0.1, 1.0), where
// sample = (dReal / dGame) and dGame = (tick - prevTick) * interval_per_tick.
typedef __int64  (__fastcall *PFN_ClockDrift)(__int64 a1, __int64 a2, __int64 a3, __int64 a4);
// Threaded gate: m_nSignonState>=6 && a1->m_metaData.bIsDelta(+44) &&
// net_threadedProcessPacket. Non-threaded branch double-feeds SetServerTick.
typedef char     (__fastcall *PFN_ClProcessSnapshotDispatch)(__int64 a1);
// Native S21 parsers: PlayerNotifyDidDamage, RemoteBulletFired, RemoteWeaponReload,
// WeapProjFireCB. CViewRender::SetupSky vtbl slot 2 writes m_has3DSky at +0x11A375.
typedef __int64  (__fastcall *PFN_SetupSky)(uintptr_t viewRender, uintptr_t viewBundle);
// S21 render orchestrator: 8 args (not 10), no drawFlags / 0x80000 gate.
// Sole early gate is m_has3DSky. Previous 10-arg decl was ABI-safe but wrong.
typedef void* (__fastcall *PFN_RenderOrchestrator)(
	uintptr_t viewRender,
	uintptr_t a2,
	uintptr_t a3,
	uintptr_t a4_renderViewSetup,
	uintptr_t a5_flagByte,           // BYTE in S21 -- declared uintptr_t (low 8 b only used)
	uintptr_t a6_renderList,
	uintptr_t a7,
	uintptr_t a8_outEntity            // OUT
);

static PFN_ClProcessFrame       s_origClProcessFrame       = nullptr;
static PFN_TransitionEntity     s_origTransitionEntity     = nullptr;
static PFN_PropApplyLoop        s_origPropApplyLoop        = nullptr;
static PFN_ClProcessSnapshotWT  s_origClProcessSnapshotWT  = nullptr;
static PFN_ClSendTick           s_origClSendTick           = nullptr;
static PFN_EngineFrame          s_origEngineFrame          = nullptr;

static thread_local bool s_s2cApplyCaptureActive = false;
static thread_local bool s_s2cApplyCaptureFailed = false;
static thread_local bool s_s2cApplyCaptureValid = false;
static thread_local uint32_t s_s2cApplyCaptureTick = 0;

// [NOCLIP-SIM] S21 client PlayerMove case 9 is empty; re-run FullNoClipMove-style step after native PlayerMove so noclipping predicts origin/velocity.
static ConVar bridge_noclip_sim("bridge_noclip_sim", "1", FCVAR_RELEASE,
	"[NOCLIP-SIM] client-side noclip move simulation (fills PlayerMove case 9). 1=on 0=native "
	"(no predicted noclip motion). Default 1.");
static ConVar bridge_noclip_speed("bridge_noclip_speed", "5", FCVAR_RELEASE,
	"[NOCLIP-SIM] noclip speed factor (x sv_maxspeed). Mirrors S3 sv_noclipspeed default 5.");
static ConVar bridge_noclip_speed_fast("bridge_noclip_speed_fast", "30", FCVAR_RELEASE,
	"[NOCLIP-SIM] fast (buttons & 0x30000, right-click boost) speed factor. Mirrors S3 "
	"sv_noclipspeed_fast default 30.");
static ConVar bridge_noclip_speed_slow("bridge_noclip_speed_slow", "0.25", FCVAR_RELEASE,
	"[NOCLIP-SIM] slow (buttons & 0x4000004) speed factor. Mirrors S3 sv_noclipspeed_slow 0.25.");
static ConVar bridge_noclip_accel("bridge_noclip_accel", "10000", FCVAR_RELEASE,
	"[NOCLIP-SIM] noclip acceleration. Mirrors S3 sv_noclipaccelerate default 10000.");
static ConVar bridge_noclip_accel_fast("bridge_noclip_accel_fast", "50000", FCVAR_RELEASE,
	"[NOCLIP-SIM] fast noclip acceleration. Mirrors S3 sv_noclipaccelerate_fast default 50000.");
static ConVar bridge_noclip_accel_slow("bridge_noclip_accel_slow", "10000", FCVAR_RELEASE,
	"[NOCLIP-SIM] slow noclip acceleration. Mirrors S3 sv_noclipaccelerate_slow default 10000.");
static ConVar bridge_noclip_maxspeed("bridge_noclip_maxspeed", "320", FCVAR_RELEASE,
	"[NOCLIP-SIM] base maxspeed the noclip factors scale (S3 sv_maxspeed default 320).");
static ConVar bridge_noclip_friction("bridge_noclip_friction", "4", FCVAR_RELEASE,
	"[NOCLIP-SIM] noclip friction (S3 sv_friction default 4; surface friction assumed 1.0).");
// Both engines scale the accel cap AND the friction drop by player->m_surfaceFriction.
// -1 reads the live field; a positive value forces it (A/B only).
static ConVar bridge_noclip_surface_friction("bridge_noclip_surface_friction", "-1", FCVAR_RELEASE,
	"[NOCLIP-SIM] -1 = read the live player->m_surfaceFriction (correct); >0 forces a value.");
static ConVar bridge_noclip_diag("bridge_noclip_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[NOCLIP-SIM] windowed telemetry: frametime source/spread, simRatio (integrated sim time "
	"vs elapsed curtime, ~1.0 = step size matches the dedi), tier split, wishspeed/speed.");
// [DUCK-RESTORE] Duck timer at player+0x242C is replay-mutated; S3/S21 recv tables do not pair the networked duck transition field.
// Ring by cmd number: record post-cmd timer after K, restore K-1 value before re-executing K so replays do not multi-decrement.
static ConVar bridge_duck_restore("bridge_duck_restore", "1", FCVAR_RELEASE,
	"[DUCK-RESTORE] per-command restore chain for the working duck-transition timer "
	"(player+0x242C) -- kills the prediction-replay multi-decrement that made crouch "
	"transitions complete instantly (the crouch prediction-error root). 1=on. Default 1.");
static PFN_HostFrame            s_origHostFrame            = nullptr;
static PFN_HostStateFrame       s_origHostStateFrame       = nullptr;
static PFN_PlayerRunCmdPred     s_origPlayerRunCmdPred     = nullptr;   //
static PFN_PlayerMoveClient     s_origPlayerMoveClient     = nullptr;   // [NOCLIP-SIM]
static PFN_ClockDrift           s_origClockDrift           = nullptr;
static void ClockDrift_FlushPending(void);
static PFN_ClProcessSnapshotDispatch s_origClProcessSnapshotDispatch = nullptr;
PFN_PlayerDidDamageParse s_origPlayerDidDamageParse = nullptr;   // [DMG-DIAG-NATIVE]
PFN_RemoteBulletFired  s_origRemoteBulletFired  = nullptr;
PFN_RemoteWeaponReload s_origRemoteWeaponReload = nullptr;
PFN_WeapProjFireCB     s_origWeapProjFireCB     = nullptr;

// [ANIM-FIX] Client C_BaseAnimating::StudioFrameAdvance -- the anchor-
// stabilization fix for frozen server-animated anims.
typedef void (__fastcall *PFN_StudioFrameAdvance)(uintptr_t thisEntity);
static PFN_StudioFrameAdvance s_origStudioFrameAdvance = nullptr;          // C_BaseAnimating (plain props)
// C_BaseAnimatingOverlay::StudioFrameAdvance -- the variant PLAYERS/NPCs use
// (computes the base cycle inline + the overlay layers; does NOT call the base function above).
typedef void (__fastcall *PFN_StudioFrameAdvanceOverlay)(uintptr_t thisEntity, uintptr_t a2, uintptr_t a3, uintptr_t a4);
static PFN_StudioFrameAdvanceOverlay s_origStudioFrameAdvanceOverlay = nullptr;

// C_Player::MovePostThink. m_PlayerAnimState at +0x3610 (do not copy other-build offsets).
typedef void (__fastcall *PFN_MovePostThink)(uintptr_t player);
static PFN_MovePostThink s_origMovePostThink = nullptr;
// SetSequence: log caller RIP + seq. m_animSequence at +0xE44.
typedef double (__fastcall *PFN_SetSequence)(uintptr_t thisEntity, unsigned __int16 seq);
static PFN_SetSequence s_origSetSequence = nullptr;

// Per-frame anim-state apply copies m_animSequence (+0xE44). Restore wire seq for gate==0.
typedef __int64 (__fastcall *PFN_AnimStateApply)(uintptr_t thisEntity, unsigned int a2);
static PFN_AnimStateApply s_origAnimStateApply = nullptr;

// C_Player::SkipsAnimationData vtbl slot 45. Cycle +0xF4, animTime +0x3B4.
typedef unsigned char (__fastcall* PFN_PlayerSkipsAnimData)(uintptr_t);
static PFN_PlayerSkipsAnimData s_origPlayerSkipsAnimData = nullptr;

// Overlay InterpolateFieldsInternal. Force m_bPredictable (ent+1876) for the call.
typedef __int64 (__fastcall* PFN_OverlayInterpFields)(uintptr_t, float, __int64, float);
static PFN_OverlayInterpFields s_origOverlayInterpFields = nullptr;

// [CLOCK-FEED-FIX] Native queued-server-tick drain + its client-object
// global. CL_ProcessSnapshot calls this drain ONLY on the synchronous
// branch; the bridge routes snapshots to the THREADED branch, which returns before it.
static PFN_SetupSky             s_origSetupSky             = nullptr;
static PFN_RenderOrchestrator s_origRenderOrchestrator = nullptr;

static volatile LONG s_clProcessFrameCalls   = 0;
static volatile LONG s_clProcessFrameGate1   = 0; // signon state < 2
static volatile LONG s_clProcessFrameGate2   = 0; // +100 cvar
static volatile LONG s_clProcessFrameGate3   = 0;
static volatile LONG s_clProcessFramePassed  = 0; // reached the apply-iter section
static volatile LONG s_transitionEntityCalls = 0;
static volatile LONG s_propApplyCalls        = 0;
static volatile LONG s_clSendTickCalls       = 0; // (per-frame C2S send) probe

// NULL RecvProp slots abort GenerateDeltas. Fill dummies and leave them in.
// Dummy: +0x00 type +0x10 netDataType +0x28 name +0x38 arrayProp +0x48 proxy.
// decoder +0x18 SendProp** +0x4080 RecvProp** +0x4098 m_Size.
typedef __int64 (__fastcall *PFN_GenerateDeltas)(
	__int64, __int64, unsigned int, char, unsigned int, int,
	__int64*, __int64, char*, __int64, int, uint8_t*, __int64);
static PFN_GenerateDeltas s_origGenerateDeltas = nullptr;

static void __fastcall DummyRecvProxy(__int64, __int64, __int64*) {}

// Global dummy RecvProp (DPT_Int) for decoder-builder NULL slots.
static uint8_t s_globalDummy[0x68] = {};
static bool    s_globalDummyInit   = false;

static void EnsureGlobalDummy()
{
	if (s_globalDummyInit) return;
	s_globalDummyInit = true;
	memset(s_globalDummy, 0, sizeof(s_globalDummy));
	static const char s_name[] = "?_unmatched";
	*(const char**)(s_globalDummy + 0x28) = s_name;
	*(void**)(s_globalDummy + 0x38)       = (void*)s_globalDummy;  // arrayProp = self
	*(void**)(s_globalDummy + 0x48)       = (void*)&DummyRecvProxy;
}

const void* NetBridge_GlobalDummyRecvProp(void)
{
	EnsureGlobalDummy();
	return s_globalDummy;
}


static volatile LONG s_genDeltaFilledTotal = 0;
static volatile LONG s_genDeltaCalls       = 0;

// WriteBlock dummy-skip cave counters: Path A futureFields, Path B wire-read, Path C undo.
static volatile LONG* s_pWriteBlockSkipCounter  = nullptr;  // Path B (wire)
static volatile LONG* s_pWriteBlockSkipCounterA = nullptr;  // Path A (future)
static volatile LONG* s_pWriteBlockSkipCounterC = nullptr;  // Path C (inner undo)

// Unmatched DPT_Array: consume per-element wire bits; do not write the delta buffer.
static uintptr_t      s_decoderFuncsTable      = 0;   // per-prop wire decoder table, set at cave install
static volatile LONG  s_unmatchedArrayConsumed = 0;

static void Bridge_BitReaderOverflow(uintptr_t bitReader)
{
	if (bitReader < 0x10000)
		return;
	__try
	{
		*reinterpret_cast<unsigned char*>(bitReader + 0x08) = 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

static void Bridge_ConsumeUnmatchedArrayElems(
	uintptr_t sendProp, uintptr_t bitReader, int count)
{
	if (!sendProp || !bitReader || !s_decoderFuncsTable)
	{
		Bridge_BitReaderOverflow(bitReader);
		return;
	}
	if (count <= 0)
		return;
	if (count > 1023)
	{
		Bridge_BitReaderOverflow(bitReader);
		return;
	}

	uintptr_t elemTmpl = 0;
	int       elemType = -1;
	__try {
		elemTmpl = *reinterpret_cast<uintptr_t*>(sendProp + 0x18);  // SendProp.m_pArrayProp
		if (elemTmpl)
			elemType = *reinterpret_cast<int*>(elemTmpl);           // element SendProp.m_Type
	} __except (EXCEPTION_EXECUTE_HANDLER) { elemTmpl = 0; }

	// Source array elements are always scalars. ty=5 (nested array) and ty=10
	// (DataTable -> funcs[10] is NULL) cannot be consumed by a single-level
	// loop -- make the (impossible) case loud rather than silently desync.
	if (!elemTmpl || elemType < 0 || elemType > 9 || elemType == 5)
	{
		Warning(eDLL_T::ENGINE,
			"[ARR-CONSUME] unconsumable unmatched array: elemTmpl=%p elemType=%d "
			"-- overflowing wire\n", (void*)elemTmpl, elemType);
		Bridge_BitReaderOverflow(bitReader);
		return;
	}

	typedef void (__fastcall *PFN_WireDecoder)(void*, uintptr_t, uintptr_t, void*);
	PFN_WireDecoder decoder = nullptr;
	__try {
		decoder = *reinterpret_cast<PFN_WireDecoder*>(
			s_decoderFuncsTable + 0x30 * static_cast<uintptr_t>(elemType));
	} __except (EXCEPTION_EXECUTE_HANDLER) { decoder = nullptr; }
	if (!decoder)
	{
		Warning(eDLL_T::ENGINE,
			"[ARR-CONSUME] NULL wire decoder for elemType=%d -- overflowing wire\n",
			elemType);
		Bridge_BitReaderOverflow(bitReader);
		return;
	}

	// Scratch sinks for the decoder's a1 (decode context) and a4 (value).
	// 64 bytes each is far past the widest element (Quaternion = 16 bytes).
	unsigned char ctxScratch[64];
	unsigned char valScratch[64];
	volatile int consumed = 0;   // volatile: read in the __except handler
	__try {
		for (; consumed < count; ++consumed)
		{
			decoder(ctxScratch, elemTmpl, bitReader, valScratch);
			if (*reinterpret_cast<unsigned char*>(bitReader + 0x08))
				break;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Warning(eDLL_T::ENGINE,
			"[ARR-CONSUME] wire decoder faulted at elem %d/%d (elemType=%d)\n",
			consumed, count, elemType);
		Bridge_BitReaderOverflow(bitReader);
		return;
	}

	const LONG n = InterlockedIncrement(&s_unmatchedArrayConsumed);
	if (n <= 8 || (n % 1024) == 0)
		Warning(eDLL_T::ENGINE,
			"[ARR-CONSUME] consumed unmatched ty=5 array: %d elem(s) elemType=%d (#%ld)\n",
			count, elemType, n);
}

// ---------------------------------------------------------------------------
// Connect-time unmatched-prop budget (read-only). One-shot Warning summary.
// ---------------------------------------------------------------------------
static ConVar bridge_unmatch_budget("bridge_unmatch_budget", "0", FCVAR_DEVELOPMENTONLY,
	"[UNMATCH-BUDGET] Log unique unmatched SendProps (name/type/count) when "
	"dummy RecvProps are installed. 0=off, 1=on (default).");

struct UnmatchBudgetRow_t
{
	char name[96];
	int  type;
	int  count;
};
static UnmatchBudgetRow_t s_unmatchBudget[512];
static volatile LONG      s_unmatchBudgetRows = 0;
static volatile LONG      s_unmatchBudgetTotal = 0;
static volatile LONG      s_unmatchBudgetSummaryDumped = 0;

static void UnmatchBudget_Note(const char* name, int type)
{
	if (!bridge_unmatch_budget.GetBool())
		return;
	if (!name || !name[0])
		name = "?";

	// Linear scan is fine: at most 512 unique unmatched names for a connect.
	const LONG rows = s_unmatchBudgetRows;
	for (LONG i = 0; i < rows && i < 512; ++i)
	{
		if (s_unmatchBudget[i].type == type
			&& strncmp(s_unmatchBudget[i].name, name, sizeof(s_unmatchBudget[i].name)) == 0)
		{
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(&s_unmatchBudget[i].count));
			InterlockedIncrement(&s_unmatchBudgetTotal);
			return;
		}
	}

	const LONG slot = InterlockedIncrement(&s_unmatchBudgetRows) - 1;
	if (slot < 0 || slot >= 512)
		return;
	strncpy_s(s_unmatchBudget[slot].name, name, _TRUNCATE);
	s_unmatchBudget[slot].type = type;
	s_unmatchBudget[slot].count = 1;
	InterlockedIncrement(&s_unmatchBudgetTotal);

	// First sight of each unique name (cap log spam to first 64 uniques).
	if (slot < 64)
	{
		Warning(eDLL_T::ENGINE,
			"[UNMATCH-BUDGET] first unmatched SendProp type=%d name='%s'\n",
			type, s_unmatchBudget[slot].name);
	}
}

static void UnmatchBudget_MaybeDumpSummary(void)
{
	if (!bridge_unmatch_budget.GetBool())
		return;
	// Dump once after we have accumulated a useful residual set.
	const LONG total = s_unmatchBudgetTotal;
	if (total < 8)
		return;
	if (InterlockedCompareExchange(&s_unmatchBudgetSummaryDumped, 1, 0) != 0)
		return;

	const LONG rows = s_unmatchBudgetRows;
	Warning(eDLL_T::ENGINE,
		"[UNMATCH-BUDGET] === summary: %ld unique names, %ld dummy fills (cap 512) ===\n",
		(long)rows, (long)total);
	const LONG dumpN = (rows < 128) ? rows : 128;
	for (LONG i = 0; i < dumpN; ++i)
	{
		Warning(eDLL_T::ENGINE,
			"[UNMATCH-BUDGET]  type=%d count=%d name='%s'\n",
			s_unmatchBudget[i].type,
			s_unmatchBudget[i].count,
			s_unmatchBudget[i].name);
	}
	if (rows > dumpN)
	{
		Warning(eDLL_T::ENGINE,
			"[UNMATCH-BUDGET]  ... %ld more unique names not listed\n",
			(long)(rows - dumpN));
	}
}

// Dummy RecvProps stay in the decoder until ClassInfo rebuilds it.
// After the first walk there are no NULLs left -- skip the per-prop scan.
static uintptr_t s_gdFilledDec[512] = {};

// Dummy pool outlives GenerateDeltas workers. Recycle after ClassInfo, not ResetAllState.
static uint8_t       s_gdDummyPool[16384][0x68];
volatile LONG s_gdDummyPoolNext = 0;
static const char    s_gdDummyName[] = "?_unmatched";

static bool GenerateDeltas_DecoderFilled(uintptr_t decoder)
{
	unsigned i = static_cast<unsigned>((decoder >> 6) & 511);
	for (int n = 0; n < 512; ++n)
	{
		const uintptr_t v = s_gdFilledDec[i];
		if (v == decoder)
			return true;
		if (v == 0)
			return false;
		i = (i + 1) & 511;
	}
	return false;
}

static void GenerateDeltas_MarkFilled(uintptr_t decoder)
{
	unsigned i = static_cast<unsigned>((decoder >> 6) & 511);
	for (int n = 0; n < 512; ++n)
	{
		const uintptr_t v = s_gdFilledDec[i];
		if (v == decoder || v == 0)
		{
			s_gdFilledDec[i] = decoder;
			return;
		}
		i = (i + 1) & 511;
	}
}

void GenerateDeltas_ClearFilled(void)
{
	memset(s_gdFilledDec, 0, sizeof(s_gdFilledDec));
}

static __int64 __fastcall Hook_GenerateDeltas(
	__int64 a1, __int64 a2, unsigned int a3, char a4, unsigned int a5, int a6,
	__int64* a7, __int64 a8, char* a9, __int64 a10, int a11, uint8_t* a12, __int64 a13)
{
	SNAPB_T0(_sbT);
	InterlockedIncrement(&s_genDeltaCalls);

	if (a9 && GenerateDeltas_DecoderFilled(reinterpret_cast<uintptr_t>(a9)))
	{
		SNAPB_T0(_sbO);
		const __int64 r = s_origGenerateDeltas(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
		SNAPB_ADD_ORIG(SNAPB_GENDELTAS, _sbO);
		SNAPB_ADD_TOTAL(SNAPB_GENDELTAS, _sbT);
		return r;
	}

	// a9 is the decoder; validate before deref (engine can pass junk).
	if (!a9 || !Bridge_DecoderReadable(reinterpret_cast<uintptr_t>(a9), 0x40A0))
	{
		static volatile LONG s_badDecoderN = 0;
		const LONG n = InterlockedIncrement(&s_badDecoderN);
		if (n <= 20 || (n % 1000) == 0)
			Warning(eDLL_T::ENGINE,
				"[GEN-DELTAS] unreadable decoder a9=%p -- passing through (#%ld)\n",
				(void*)a9, (long)n);
		{
			SNAPB_T0(_sbO);
			const __int64 r = s_origGenerateDeltas(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
			SNAPB_ADD_ORIG(SNAPB_GENDELTAS, _sbO);
			SNAPB_ADD_TOTAL(SNAPB_GENDELTAS, _sbT);
			return r;
		}
	}

	// numProps at decoder+0x4098 (not +0x20). RecvProp** +0x4080. SendProp** +0x18.
	const int numProps = *(int*)(a9 + 0x4098);
	__int64* propArray = *(__int64**)(a9 + 0x4080);
	__int64* sendProps = *(__int64**)(a9 + 0x18);

	if (!propArray || !sendProps || numProps <= 0 || numProps > 4096 || !a12)
	{
		SNAPB_T0(_sbO);
		const __int64 r = s_origGenerateDeltas(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
		SNAPB_ADD_ORIG(SNAPB_GENDELTAS, _sbO);
		SNAPB_ADD_TOTAL(SNAPB_GENDELTAS, _sbT);
		return r;
	}

	int nFilled = 0;
	bool bPoolExhausted = false;

	for (int i = 0; i < numProps; ++i)
	{
		if (propArray[i] != 0 && !IsBadRecvPropPtr(static_cast<uintptr_t>(propArray[i])))
			continue;

		__int64 sp = sendProps[i];
		if (!sp)
			continue;  // No SendProp either -- nothing we can do.

		const LONG slot = InterlockedIncrement(&s_gdDummyPoolNext) - 1;
		if (slot < 0 || slot >= 16384)
		{
			bPoolExhausted = true;
			break;
		}

		uint8_t* d = s_gdDummyPool[slot];
		memset(d, 0, 0x68);

		// Mirror SendProp's type so the bit reader consumes the correct width.
		// SendProp's type is at +0x00 (mirrors RecvPropData layout).
		const int spType = *(int*)sp;
		*(int*)(d + 0x00)        = spType;
		// netDataType, flags, stringBufferSize already zero from memset.
		*(const char**)(d + 0x28) = s_gdDummyName;
		// arrayProp = self so DPT_Array's inner loop sees a valid pointer
		// when reading [arrayProp+0x10] for netDataType (which is 0 = safe).
		*(void**)(d + 0x38)       = (void*)d;
		// Proxy is a no-op so even if the apply phase calls it, nothing happens.
		*(void**)(d + 0x48)       = (void*)&DummyRecvProxy;

		// Budget: SendProp name is at +0x40 (same as flat dump / DF_CopyStr).
		const char* spName = nullptr;
		__try { spName = *(const char**)(sp + 0x40); }
		__except (EXCEPTION_EXECUTE_HANDLER) { spName = nullptr; }
		UnmatchBudget_Note(spName, spType);

		propArray[i]              = (__int64)d;
		++nFilled;
	}

	if (nFilled > 0)
		UnmatchBudget_MaybeDumpSummary();

	if (!bPoolExhausted)
		GenerateDeltas_MarkFilled(reinterpret_cast<uintptr_t>(a9));

	__int64 result;
	{
		SNAPB_T0(_sbO);
		result = s_origGenerateDeltas(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
		SNAPB_ADD_ORIG(SNAPB_GENDELTAS, _sbO);
	}

	// Leave dummies in: ApplyBufferDeltasToEntity AVs on a NULL RecvProp.
	// DummyRecvProxy is a no-op; offset 0 is unused.
	int cleared = 0;
	(void)cleared;

	if (nFilled > 0)
	{
		InterlockedExchangeAdd(&s_genDeltaFilledTotal, nFilled);
		const LONG callN = s_genDeltaCalls;
		if (callN <= 5 || (callN % 200) == 0)
		{
			const LONG caveB = s_pWriteBlockSkipCounter
				? *s_pWriteBlockSkipCounter : -1;
			const LONG caveA = s_pWriteBlockSkipCounterA
				? *s_pWriteBlockSkipCounterA : -1;
			const LONG caveC = s_pWriteBlockSkipCounterC
				? *s_pWriteBlockSkipCounterC : -1;
			Warning(eDLL_T::ENGINE,
				"[GEN-DELTA] call#%ld decoder=%p numProps=%d filled=%d cleared=%d "
				"origResult=%lld -> adjusted=%lld  cave A=%ld B=%ld C=%ld\n",
				callN, (void*)a9, numProps, nFilled, cleared,
				(long long)result, (long long)(result - cleared),
				(long)caveA, (long)caveB, (long)caveC);
		}
	}

	SNAPB_ADD_TOTAL(SNAPB_GENDELTAS, _sbT);
	return result - cleared;
}

static char __fastcall Hook_ClProcessFrame(__int64 a1)
{
	InterlockedIncrement(&s_clProcessFrameCalls);

	// Mirror the original 4-condition entry gate so we know WHICH one short-
	// circuits. We can't safely call vtable[37] before original, so we infer
	// it by gate1-3 check then trust original to handle gate4 internally.
	int   gate1Val = 0;
	DWORD gate2Val = 0;
	BYTE  gate3Val = 0;
	__try {
		gate1Val = *(int*)(a1 + 172);

		uintptr_t exeBase = NetObs_GetExeModuleBase();
		if (exeBase) {
			uintptr_t cvarBase = *(uintptr_t*)NetObs_CvarGatePtrAddr();
			if (cvarBase) gate2Val = *(DWORD*)(cvarBase + 100);
			gate3Val = *(BYTE*)NetObs_InGameFlagAddr();
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}

	const bool gate1Fail = (gate1Val < 2);
	const bool gate2Fail = (gate2Val != 0);
	const bool gate3Fail = (gate3Val == 0);

	if (gate1Fail) InterlockedIncrement(&s_clProcessFrameGate1);
	if (gate2Fail) InterlockedIncrement(&s_clProcessFrameGate2);
	if (gate3Fail) InterlockedIncrement(&s_clProcessFrameGate3);

	char result = 0;
	s_s2cApplyCaptureActive = true;
	s_s2cApplyCaptureFailed = false;
	s_s2cApplyCaptureValid = false;
	s_s2cApplyCaptureTick = 0;
	__try { result = s_origClProcessFrame(a1); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		result = 0;
		s_s2cApplyCaptureFailed = true;
	}
	s_s2cApplyCaptureActive = false;

	if (result != 0 && !s_s2cApplyCaptureFailed && s_s2cApplyCaptureValid)
		S21Bridge_S2CScriptRemote_OnSnapshotApplied(s_s2cApplyCaptureTick);

	// If original returned non-zero AND none of gates 1-3 failed, we likely
	// reached the apply iter section.
	if (!gate1Fail && !gate2Fail && !gate3Fail && result != 0)
		InterlockedIncrement(&s_clProcessFramePassed);

	return result;
}

//=============================================================================
// Snapshot-chain walk before ForceFullUpdateFromWorker. requested tick at a1+20,
// chain head *off+32, node tick +48, next +72.
//=============================================================================
// AsyncSnapshotData: +12 isDelta, +20 rawDeltaFrom, +60 tick, +64 rawTick.
// CClientState: main ack +348/+352, worker ack +356/+360, workerIsNew +364.
// The worker only stamps its ack after a clean parse; a delta that leaves
// it unstamped is the frozen-ack case.
static void __fastcall Hook_ClProcessSnapshotWT_Body(
	__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
	const uintptr_t cs = s_clientStatePtr;
	const uintptr_t snap = static_cast<uintptr_t>(a1);
	int preW0 = -2, preW1 = -2, preNew = -2;
	if (cs && snap)
	{
		preW0  = *reinterpret_cast<const int*>(cs + 356);
		preW1  = *reinterpret_cast<const int*>(cs + 360);
		preNew = *reinterpret_cast<const uint8_t*>(cs + 364);
	}

	if (s_origClProcessSnapshotWT)
		s_origClProcessSnapshotWT(a1, a2, a3, a4);

	if (!cs || !snap || !bridge_net_flow_diag.GetBool())
		return;
	const int  isDelta = *reinterpret_cast<const uint8_t*>(snap + 12);
	const int  rawFrom = *reinterpret_cast<const int*>(snap + 20);
	const int  rawTick = *reinterpret_cast<const int*>(snap + 64);
	const int  postW1  = *reinterpret_cast<const int*>(cs + 360);
	const int  postNew = *reinterpret_cast<const uint8_t*>(cs + 364);
	const bool dropped = isDelta && postW1 != rawTick;

	static long s_wtN = 0;
	static long s_wtDropN = 0;
	const long n = InterlockedIncrement(&s_wtN);
	const long d = dropped ? InterlockedIncrement(&s_wtDropN) : 0;
	if (n <= 40 || (n % 100) == 0 || (dropped && (d <= 20 || (d % 200) == 0)))
	{
		Warning(eDLL_T::ENGINE,
			"[WT-PROBE] #%ld isDelta=%d from=%d raw=%d | worker pre{%d %d new=%d} post{%d %d new=%d} main{%d %d}%s\n",
			n, isDelta, rawFrom, rawTick, preW0, preW1, preNew,
			*reinterpret_cast<const int*>(cs + 356), postW1, postNew,
			*reinterpret_cast<const int*>(cs + 348), *reinterpret_cast<const int*>(cs + 352),
			dropped ? " -- DROPPED" : "");
	}
}

static void __fastcall Hook_ClProcessSnapshotWT(
	__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
	Hook_ClProcessSnapshotWT_Body(a1, a2, a3, a4);
}

//=============================================================================
// [FTW]/[HTS] hardware-watchpoint subsystem (DR0/DR1 + FtwVeh + FtwArmThread/
// FtwArmAllThreads) removed: the frametime-collapse investigation it served is
// closed (HFLOW proved the sim clock healthy). Legacy CCrashHandler VEH stays.
//=============================================================================

//-----------------------------------------------------------------------------
// Replay +bridge_/+sdk_ launch args after ConVar registration finishes.
//-----------------------------------------------------------------------------
static bool BootArgs_IsObserveOnly(const char* name)
{
	if (!name || !name[0])
		return false;
	if (strcmp(name, "sdk_pak_census") == 0)
		return false;
	if (strcmp(name, "sdk_observe_net") == 0)
		return true;
	if (strcmp(name, "bridge_clk_health") == 0)
		return true;
	if (strcmp(name, "bridge_unmatch_budget") == 0)
		return true;
	if (strstr(name, "_diag") || strstr(name, "_probe") || strstr(name, "_trace")
		|| strstr(name, "_dump") || strstr(name, "_debug") || strstr(name, "_tap")
		|| strstr(name, "_census") || strstr(name, "_watch") || strstr(name, "_firehose"))
		return true;
	const size_t n = strlen(name);
	return n > 4 && strcmp(name + n - 4, "_log") == 0;
}

static void BootArgs_ReplaySdkConvars(void)
{
	const char* const pszCmdLine = GetCommandLineA();
	if (!pszCmdLine)
		return;

	int nApplied = 0;
	int nSkipped = 0;
	const char* p = pszCmdLine;
	while (*p)
	{
		// Tokenize (quotes treated as plain delimiter-protected spans).
		while (*p == ' ' || *p == '\t') ++p;
		if (!*p) break;
		size_t n = 0; bool bQuote = false;
		char szCur[256];
		for (; *p && (bQuote || (*p != ' ' && *p != '\t')); ++p)
		{
			if (*p == '"') { bQuote = !bQuote; continue; }
			if (n < sizeof(szCur) - 1) szCur[n++] = *p;
		}
		szCur[n] = '\0';

		if (szCur[0] == '+' &&
			(strncmp(szCur + 1, "bridge_", 7) == 0 || strncmp(szCur + 1, "sdk_", 4) == 0))
		{
			// Peek the value token (next token not starting with +/-); default "1".
			const char* q = p;
			while (*q == ' ' || *q == '\t') ++q;
			size_t m = 0; bool bQ2 = false;
			char szVal[256]; szVal[0] = '\0';
			for (const char* r = q; *r && (bQ2 || (*r != ' ' && *r != '\t')); ++r)
			{
				if (*r == '"') { bQ2 = !bQ2; continue; }
				if (m < sizeof(szVal) - 1) szVal[m++] = *r;
			}
			szVal[m] = '\0';
			const bool bHasValue = szVal[0] && szVal[0] != '+' && szVal[0] != '-';

			char szCmd[512];
			if (BootArgs_IsObserveOnly(szCur + 1))
			{
				// Leftover +probe/+diag on a shortcut must not tax a play boot.
				// Force 0 so an earlier engine +arg apply cannot stay on.
				snprintf(szCmd, sizeof(szCmd), "%s 0\n", szCur + 1);
				if (s_origCbufAddText)
					s_origCbufAddText(ECommandTarget_t(0), szCmd, cmd_source_t(0));
				++nSkipped;
				Warning(eDLL_T::CLIENT, "[BOOT-ARGS] skipped observe +arg: %s", szCmd);
				continue;
			}

			snprintf(szCmd, sizeof(szCmd), "%s %s\n", szCur + 1, bHasValue ? szVal : "1");
			if (s_origCbufAddText)
				s_origCbufAddText(ECommandTarget_t(0), szCmd, cmd_source_t(0));
			++nApplied;
			Warning(eDLL_T::CLIENT, "[BOOT-ARGS] replayed: %s", szCmd);
		}
	}
	Warning(eDLL_T::CLIENT, "[BOOT-ARGS] replayed %d bridge_/sdk_ launch-arg convars, skipped %d observe\n",
		nApplied, nSkipped);
}

// One-shot guard shared by the pre-connect bootstrap worker and the frame-30
// fallback: whichever sees the cvar system + Cbuf ready first runs the replay.
static volatile LONG s_bootArgsReplayed = 0;
static void BootArgs_ReplaySdkConvarsOnce(void)
{
	if (InterlockedCompareExchange(&s_bootArgsReplayed, 1, 0) != 0)
		return;
	BootArgs_ReplaySdkConvars();
}

#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
typedef struct _PROCESS_POWER_THROTTLING_STATE {
	ULONG Version;
	ULONG ControlMask;
	ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE;
#endif

static ConVar client_unthrottle("client_unthrottle", "1", FCVAR_RELEASE,
	"Opt the client process out of Windows background power/timer throttling. "
	"Win11 ignores timeBeginPeriod(1) while occluded, which lengthens unfocused "
	"frames past usercmd_frametime_max and starves prediction.", "bool");

static void ClientUnthrottle_OnFrame(void)
{
	static bool s_bLatched = false;
	if (s_bLatched || !client_unthrottle.GetBool())
		return;
	s_bLatched = true;

	PROCESS_POWER_THROTTLING_STATE state;
	memset(&state, 0, sizeof(state));
	state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;

	state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
	state.StateMask = 0;
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));

	state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
	state.StateMask = 0;
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));

	timeBeginPeriod(1);
}

//=============================================================================
// Unfocused Present: SyncInterval=0 | DXGI_PRESENT_DO_NOT_WAIT. Vtable slot 8.
//=============================================================================
static bool ClSendTick_WindowUnfocused(void); // defined with [CLKDRIFT]

typedef LONG (__fastcall *PFN_IDXGISwapChain_Present)(void* self, unsigned int syncInterval, unsigned int flags);
static PFN_IDXGISwapChain_Present s_origSwapChainPresent = nullptr;
static volatile LONG              s_pumpGateInstalled    = 0;
static volatile LONG              s_pumpGateActive       = 0;
static volatile LONG              s_pumpGateCount        = 0;
static volatile LONG              s_pumpGateTransitionLog = 0;
static constexpr LONG             kPumpGateTransitionLogMax = 8;
static volatile LONG              s_pumpGateInstalling   = 0; // serializes vtable patching between the bootstrap worker and the frame path
static volatile LONG              s_pumpGateFlagFails    = 0;
static uintptr_t                  s_pumpGateVftable      = 0; // vtable currently patched; recreated swapchains can expose another class vtable
static bool                       s_pumpGateFlagOk       = true;

static constexpr LONG s_nDxgiInvalidCallHr   = static_cast<LONG>(0x887A0001); // DXGI_ERROR_INVALID_CALL
static constexpr LONG s_nDxgiWasStillDrawing = static_cast<LONG>(0x887A000A); // DXGI_ERROR_WAS_STILL_DRAWING

// A DO_NOT_WAIT present hands the engine an error status instead of pacing it,
// and the DX12 main loop then free-runs for the rest of the session: every
// host frame gets a microsecond dt, floored to 1 ms, and the world sims at a
// fraction of real time. Off by default; the flip-model swapchain does not
// block on an occluded window anyway.
static ConVar bridge_present_unfocus("bridge_present_unfocus", "0", FCVAR_RELEASE,
	"While the game window is not foreground: present without the vsync wait. "
	"Leaves the DX12 main loop unpaced after focus returns (slow motion); keep 0.");

// dx11 exe resolves dxgi dynamically and stores its swapchain in a bare global;
// on the dx12 exe this path returns nullptr.
static void* PumpGate_LegacySwapChain(void)
{
	// This global belongs to the dx11 exe; on any other binary it is
	// unrelated data and dereferencing it is a crash.
	if (NetObs_IsDx12Exe())
		return nullptr;

	const uintptr_t pGlobal = NetObs_Sym(NetObsSym_t::LegacySwapChainGlobal);
	if (!pGlobal)
		return nullptr;
	void* sc = nullptr;
	__try
	{
		sc = *reinterpret_cast<void**>(pGlobal);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return sc;
}

// Candidate tables/pointers are validated before any patch attempt: the
// self-heal pass runs against renderer state that can be mid-teardown, and a
// stale global briefly holds garbage during swapchain destruction.
static bool PumpGate_SaneVftable(const uintptr_t vt)
{
	if (vt < 0x10000)
		return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(reinterpret_cast<void*>(vt), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return false;
	const DWORD prot = mbi.Protect & 0xFF;
	return prot == PAGE_READONLY || prot == PAGE_READWRITE
		|| prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE
		|| prot == PAGE_EXECUTE_WRITECOPY || prot == PAGE_WRITECOPY;
}

static bool PumpGate_SaneFnPtr(const void* p)
{
	const uintptr_t fn = reinterpret_cast<uintptr_t>(p);
	if (fn < 0x10000)
		return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return false;
	const DWORD prot = mbi.Protect & 0xFF;
	return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ
		|| prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

// dx12: engine swapchain is the one the render overlay already captures.
// dx11: no such capture; use the exe's own swapchain global.
static void* PumpGate_TargetSwapChain(void)
{
	if (NetObs_IsDx12Exe())
		return Dx12_GetGameSwapChain();
	return PumpGate_LegacySwapChain();
}

static LONG __fastcall Hook_IDXGISwapChain_Present(void* self, unsigned int syncInterval, unsigned int flags)
{
	const bool bEligible = bridge_present_unfocus.GetBool() && self && ClSendTick_WindowUnfocused();
	bool bForced         = false;

	if (bEligible)
	{
		if (self == PumpGate_TargetSwapChain())
		{
			syncInterval = 0;
			if (s_pumpGateFlagOk)
				flags |= 0x8u; // DXGI_PRESENT_DO_NOT_WAIT
			bForced = true;

			const LONG n = InterlockedIncrement(&s_pumpGateCount);
			if (InterlockedCompareExchange(&s_pumpGateActive, 1, 0) == 0
				&& InterlockedIncrement(&s_pumpGateTransitionLog) <= kPumpGateTransitionLogMax)
				Warning(eDLL_T::CLIENT,
					"[PUMP-GATE] unfocused: Present sync forced 0%s\n",
					s_pumpGateFlagOk ? " | DO_NOT_WAIT" : "");
			else if (n <= 6 || (n % 1200) == 0)
				Warning(eDLL_T::CLIENT,
					"[PUMP-GATE] unfocused present #%ld\n", n);
		}
	}

	const LONG r = s_origSwapChainPresent(self, syncInterval, flags);

	if (bForced)
	{
		if (s_pumpGateFlagOk)
		{
			// WAS_STILL_DRAWING means the GPU was busy -- the mechanism working, not a failure.
			if (r == s_nDxgiInvalidCallHr)
			{
				if (InterlockedIncrement(&s_pumpGateFlagFails) >= 8)
				{
					s_pumpGateFlagOk = false;
					Warning(eDLL_T::CLIENT,
						"[PUMP-GATE] swapchain rejected DO_NOT_WAIT; sync-interval 0 only\n");
				}
			}
			else if (r != s_nDxgiWasStillDrawing)
			{
				InterlockedExchange(&s_pumpGateFlagFails, 0);
			}
		}
	}
	else if (!bEligible && InterlockedCompareExchange(&s_pumpGateActive, 0, 1) == 1
		&& InterlockedIncrement(&s_pumpGateTransitionLog) <= kPumpGateTransitionLogMax)
	{
		Warning(eDLL_T::CLIENT, "[PUMP-GATE] %s: native Present restored\n",
			bridge_present_unfocus.GetBool() ? "foreground again" : "gate disabled");
	}

	return r;
}

static void PumpGate_PatchVftable(const uintptr_t vftable)
{
	CMemory::HookVirtualMethod(vftable,
		reinterpret_cast<const void*>(&Hook_IDXGISwapChain_Present), 8,
		reinterpret_cast<void**>(&s_origSwapChainPresent));
	s_pumpGateVftable = vftable;
}

// Adopt an already-hooked swapchain vtable; never recapture slot 8 as original.
static void PumpGate_PatchNewVftable(const uintptr_t vt)
{
	if (!vt || vt == s_pumpGateVftable || !PumpGate_SaneVftable(vt))
		return;
	if (InterlockedCompareExchange(&s_pumpGateInstalling, 1, 0) != 0)
		return;

	void* curSlot8 = nullptr;
	bool bOurs = false;
	__try
	{
		curSlot8 = *reinterpret_cast<void**>(vt + 8 * sizeof(void*));
		bOurs = (curSlot8 == reinterpret_cast<void*>(&Hook_IDXGISwapChain_Present));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		InterlockedExchange(&s_pumpGateInstalling, 0);
		return;
	}

	if (bOurs)
	{
		s_pumpGateVftable = vt;
		InterlockedExchange(&s_pumpGateInstalling, 0);
		return;
	}
	if (!PumpGate_SaneFnPtr(curSlot8))
	{
		static volatile LONG s_nBadTargets = 0;
		if (InterlockedIncrement(&s_nBadTargets) <= 3)
			Warning(eDLL_T::CLIENT,
				"[PUMP-GATE] candidate vtable %p slot8 %p not executable -- skipped\n",
				reinterpret_cast<void*>(vt), curSlot8);
		InterlockedExchange(&s_pumpGateInstalling, 0);
		return;
	}

	const uintptr_t prev = s_pumpGateVftable;
	PumpGate_PatchVftable(vt);

	if (InterlockedCompareExchange(&s_pumpGateInstalled, 1, 0) == 0)
	{
		Warning(eDLL_T::CLIENT,
			"[PUMP-GATE] IDXGISwapChain::Present gated (vtable %p slot 8 orig %p)\n",
			reinterpret_cast<void*>(vt),
			reinterpret_cast<void*>(s_origSwapChainPresent));
	}
	else if (prev)
	{
		Warning(eDLL_T::CLIENT,
			"[PUMP-GATE] swapchain vtable changed; re-gated (vtable %p slot 8 orig %p)\n",
			reinterpret_cast<void*>(vt),
			reinterpret_cast<void*>(s_origSwapChainPresent));
	}

	InterlockedExchange(&s_pumpGateInstalling, 0);
}

// Arm once a swapchain exists; re-check recreation at low cadence (VirtualQuery).
static bool PumpGate_EnsureInstalled(void)
{
	static int s_nFrames = 0;
	if (InterlockedCompareExchange(&s_pumpGateInstalled, -1, -1) != 0)
	{
		if (++s_nFrames < 60)
			return false;
		s_nFrames = 0;
	}

	void* const sc = PumpGate_TargetSwapChain();
	if (!sc)
		return false;

	uintptr_t vt = 0;
	__try
	{
		vt = *reinterpret_cast<uintptr_t*>(sc);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	if (!vt || vt == s_pumpGateVftable)
		return false;

	PumpGate_PatchNewVftable(vt);
	return InterlockedCompareExchange(&s_pumpGateInstalled, -1, -1) != 0;
}

// Pre-connect pump: unthrottle, +bridge_/+sdk_ replay, unfocused Present.
static void PumpGate_BootstrapWorker(void)
{
	const uint64_t nStartMs = GetTickCount64();
	while (GetTickCount64() - nStartMs < 120000)
	{
		ClientUnthrottle_OnFrame();

		if (g_pCVar && s_origCbufAddText)
			BootArgs_ReplaySdkConvarsOnce();

		PumpGate_EnsureInstalled();
		const bool bGateArmed = InterlockedCompareExchange(&s_pumpGateInstalled, -1, -1) != 0;
		const bool bArgsDone  = InterlockedCompareExchange(&s_bootArgsReplayed, -1, -1) != 0;
		if (bGateArmed && bArgsDone)
		{
			Warning(eDLL_T::CLIENT, "[PUMP-GATE] armed pre-connect (early install)\n");
			return;
		}
		Sleep(250);
	}
}

void Bridge_PumpGate_Bootstrap(void)
{
	try
	{
		std::thread(PumpGate_BootstrapWorker).detach();
	}
	catch (...)
	{
		Warning(eDLL_T::CLIENT,
			"[PUMP-GATE] bootstrap worker failed to start; frame-path install only\n");
	}
}

static char __fastcall Hook_EngineFrame(void* a1)
{
	ClientUnthrottle_OnFrame();

	const double lastMs = s_lastEngineFrameMs;
	const double nowMs  = static_cast<double>(GetTickCount64());
	s_lastEngineFrameMs = nowMs;

	PumpGate_EnsureInstalled();

	// [CLKDRIFT] slowframe discriminator: wall dt vs what
	// ClSendTick will actually see, plus the focus-sleep flag. One alt-tab of
	// this line settles Present-block vs sleep vs filter questions.
	if (lastMs > 0.0 && nowMs - lastMs > 250.0 && g_pCVar && ClSendTick_WindowUnfocused())
	{
		ConVar* const pHealth = g_pCVar->FindVar("bridge_clk_health");
		if (pHealth && pHealth->GetBool())
		{
			static double s_lastSlowLogMs = 0.0;
			if (nowMs - s_lastSlowLogMs > 1000.0)
			{
				s_lastSlowLogMs = nowMs;

				float ft = -1.0f, accum = -1.0f;
				int sleepActive = -1;
				__try
				{
					ft           = *reinterpret_cast<float*>(NetObs_FrameTimeAddr());
					accum        = *reinterpret_cast<float*>(NetObs_SendAccumAddr());
					sleepActive  = a1 ? *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(a1) + 0x34) : -1; // CEngine::m_notFocusSleepActive
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}

				Warning(eDLL_T::CLIENT,
					"[CLKDRIFT] slowframe qpcDt=%.3fs frametime=%.4f sleepActive=%d sendAccum=%.4f\n",
					(nowMs - lastMs) / 1000.0, static_cast<double>(ft),
					sleepActive, static_cast<double>(accum));
			}
		}
	}

	// Client-side counterpart of the dedi's _Host_RunFrame poll: catches the frame
	// a registered expansion buffer's tail is stomped, before the allocator aborts.
	HeapCanary::PollTick();
	Mat_CrossHair_TryPatch();
	PsoCache_Frame();

	// [BOOT-ARGS] one-shot replay; the pre-connect bootstrap usually wins this
	// race -- this only fires if the worker timed out before Cbuf was ready.
	static int s_nBootArgFrames = 0;
	if (s_nBootArgFrames >= 0 && ++s_nBootArgFrames == 30)
	{
		s_nBootArgFrames = -1;
		__try { BootArgs_ReplaySdkConvarsOnce(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Warning(eDLL_T::CLIENT, "[BOOT-ARGS] replay faulted\n"); }
	}

	// [MB] fire any boost FX the snapshot decode queued -- see the handler banner.
	MantleBoostClient_FrameUpdate();

	S21Bridge_TryDriveNativeConnected();

	const char r = s_origEngineFrame ? s_origEngineFrame(a1) : 0;
	ClockDrift_FlushPending();
	return r;
}

static void __fastcall Hook_HostStateFrame(__int64 a1, double a2, float a3)
{
	// CHostState::FrameUpdate is skipped; pump RCON here instead.
	RCON_LauncherClient_Think();
	RCONClient()->RunFrame();

	if (s_origHostStateFrame)
		s_origHostStateFrame(a1, a2, a3);
}

// Same pump at SDK init. Single-attach: skip HostStateFrame if this wins.
static PFN_HostStateFrame s_origHostStateFrameEarly = nullptr;
static bool               s_bHostStateEarlyHooked   = false;

static void __fastcall Hook_HostStateFrameEarly(__int64 a1, double a2, float a3)
{
	static volatile LONG s_nFirstPump = 0;
	if (InterlockedCompareExchange(&s_nFirstPump, 1, 0) == 0)
		Warning(eDLL_T::CLIENT, "[EARLY-PIPE] host-console pump alive (first frame)\n");

	RCON_LauncherClient_Think();
	RCONClient()->RunFrame();

	if (s_origHostStateFrameEarly)
		s_origHostStateFrameEarly(a1, a2, a3);
}

void Bridge_InstallEarlyPipeHooks(void)
{
	static volatile LONG s_installTried = 0;
	if (InterlockedCompareExchange(&s_installTried, 1, 0) != 0)
		return;

	s_origHostStateFrameEarly = (PFN_HostStateFrame)NetObs_Sym(NetObsSym_t::HostStateFrame);
	if (!s_origHostStateFrameEarly)
	{
		SDK_Log("[EARLY-PIPE] HostStateFrame resolve failed; lazy path stays armed\n");
		return;
	}
	if (DetourTransactionBegin() != NO_ERROR)
	{
		s_origHostStateFrameEarly = nullptr;
		SDK_Log("[EARLY-PIPE] DetourTransactionBegin failed\n");
		return;
	}
	DetourUpdateThread(GetCurrentThread());
	const LONG r = DetourAttach(reinterpret_cast<PVOID*>(&s_origHostStateFrameEarly),
	                            reinterpret_cast<PVOID>(&Hook_HostStateFrameEarly));
	if (r == NO_ERROR)
	{
		DetourTransactionCommit();
		s_bHostStateEarlyHooked = true;
		SDK_Log("[EARLY-PIPE] HostStateFrame RCON pump armed pre-connect\n");
	}
	else
	{
		DetourTransactionAbort();
		s_origHostStateFrameEarly = nullptr;
		SDK_Log("[EARLY-PIPE] HostStateFrame attach failed err=%ld\n", r);
	}
}

//=============================================================================
// [GTSFIX] Bridge leaves m_gameTimescale unmatched so it stays garbage; clamp/seed it so host_frametime cannot underflow and starve C2S send rate.
// Hook g_ClientDLL->GetGameTimescale and force ~1.0 when out of range (S3 does not network this field).
//=============================================================================
typedef float (__fastcall *PFN_GetGameTimescale)(void* thisptr);
static PFN_GetGameTimescale s_origGetGameTimescale = nullptr;
static volatile LONG        s_gtsInstalled         = 0;
static volatile LONG        s_gtsCalls             = 0;
static float                s_gtsLastLogged        = -1.0f;

// A zero here collapses host_frametime to its floor, which starves usercmd sim
// time. Correct it by default; 0 exposes the raw value for an A/B.
static ConVar bridge_gts_force("bridge_gts_force", "1", FCVAR_RELEASE,
	"Force the game timescale to 1.0 when it is 0, NaN, or >= 20. "
	"1 = correct it (default). 0 = observe only. Values in (0, 0.05] pass.");

static float __fastcall Hook_GetGameTimescale(void* thisptr)
{
	const float r = s_origGetGameTimescale ? s_origGetGameTimescale(thisptr) : 1.0f;

	// 0 / NaN / huge is decode garbage (marked-map) and would starve cmds.
	// (0, 0.05] is a deliberate freeze on the wire -- pass it through so the
	// client remainder matches listen-server host_timescale.
	const bool bGarbage = !(r == r) || r <= 0.0f || r >= 20.0f;
	const float out = (bridge_gts_force.GetBool() && bGarbage) ? 1.0f : r;

	const LONG n = InterlockedIncrement(&s_gtsCalls);
	const bool bChanged = (r != s_gtsLastLogged);
	if (n <= 10 || bChanged || (n % 2000) == 0)
	{
		s_gtsLastLogged = r;
		Warning(eDLL_T::CLIENT, "[GTS-PROBE] #%ld GetGameTimescale=%.9g%s\n",
			n, static_cast<double>(r), (out != r) ? " -> forced 1.0" : "");
	}
	return out;
}

// The inner game-timescale getter. CL_CalcMoveFrametime reads it through a
// wrapper (vtable +0x820 on the client-dll object) that returns
// inner * host_timescale, and host_frametime is scaled by it upstream -- so the
// inner value is the one that must never be zero. Idempotent; if the object is
// not live yet it leaves s_gtsInstalled clear so the next host frame retries.
static constexpr ptrdiff_t GTS_VTABLE_SLOT = 0x4E0;

static void InstallGameTimescaleFix(void)
{
	if (InterlockedCompareExchange(&s_gtsInstalled, 1, 0) != 0)
		return;
	__try
	{
		void* gClientDll = *reinterpret_cast<void**>(NetObs_ClientDllSlotAddr());
		if (!gClientDll)
		{
			InterlockedExchange(&s_gtsInstalled, 0);   // not ready -- retry next frame
			return;
		}
		void** vtbl = *reinterpret_cast<void***>(gClientDll);
		void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(vtbl) + GTS_VTABLE_SLOT);

		// A wrong slot silently corrupts an unrelated virtual's return value, so
		// refuse anything that is not a code pointer inside the game module.
		const uintptr_t base = NetObs_GetExeModuleBase();
		const uintptr_t fn = reinterpret_cast<uintptr_t>(*slot);
		if (!base || fn < base || fn >= base + 0x10000000)
		{
			s_origGetGameTimescale = nullptr;
			Warning(eDLL_T::CLIENT,
				"[GTS-PROBE] vtable+0x%tX holds %p, outside the game module -- not installed\n",
				GTS_VTABLE_SLOT, *slot);
			return;
		}

		s_origGetGameTimescale = reinterpret_cast<PFN_GetGameTimescale>(*slot);
		DWORD oldProt = 0;
		if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt))
		{
			*slot = reinterpret_cast<void*>(&Hook_GetGameTimescale);
			VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
			Warning(eDLL_T::CLIENT, "[GTS-PROBE] installed -- obj=%p vtable+0x%tX orig=%p\n",
				gClientDll, GTS_VTABLE_SLOT, reinterpret_cast<void*>(s_origGetGameTimescale));
		}
		else
		{
			s_origGetGameTimescale = nullptr;
			Warning(eDLL_T::CLIENT, "[GTS-PROBE] VirtualProtect FAILED -- not installed\n");
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		s_origGetGameTimescale = nullptr;
		InterlockedExchange(&s_gtsInstalled, 0);
	}
}

// [GTSFIX] Default ON: marked-map decode can write m_gameTimescale=0. Force 1.0
// on 0 / NaN / >= 20. Deliberate freeze values in (0, 0.05] pass through.
static ConVar bridge_gtsfix("bridge_gtsfix", "1", FCVAR_RELEASE,
	"Install the GetGameTimescale clamp (vtable+0x4E0). Force 1.0 on 0 / NaN / >= 20. "
	"1 = on (default), 0 = off.");

static uintptr_t Bridge_GetEntityBySlot(unsigned int slot); // defined later in this TU

// Dump sequence table from cached studiohdr (ent+0x1000): vmodel=hdr+0x10,
// seqCount u16 at vmodel+0x02. No LockStudioHdr.

// Dedup table for ALL-mode: tracks which model names have already been dumped
// this sweep so a lobby full of the same Legend model doesn't write the same
// sequence table thousands of times.
static char s_seqTableDumped[256][80];
static int s_seqTableDumpedCount;

static bool SeqTable_AlreadyDumped(const char* model)
{
	for (int i = 0; i < s_seqTableDumpedCount; ++i)
	{
		if (strcmp(s_seqTableDumped[i], model) == 0)
			return true;
	}
	if (s_seqTableDumpedCount < 256)
	{
		strncpy(s_seqTableDumped[s_seqTableDumpedCount], model, 79);
		s_seqTableDumped[s_seqTableDumpedCount][79] = 0;
		++s_seqTableDumpedCount;
		return false;
	}
	Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) dedup table full (256), dumping anyway\n");
	return false;
}

// GetEntityBySlot returns an interface, not the entity base. Probe header deltas
// {0,8,0x10,0x18}; lock the first that yields a printable model name + sane seqCount.
static int s_seqIfaceDelta = -1;   // -1 = not yet resolved earlier

// Sweep skip-reason counters (reset per ALL-mode invocation, printed in the summary).
static int s_seqSweepEnt, s_seqSweepNoHdr, s_seqSweepNoResolve, s_seqSweepDumped;

// Validate the CStudioHdr chain hanging off candidate entity base 'base': fills the
// out params and returns true only if studio/vmodel resolve, the model name at
// studio+0x10 is printable ASCII (>= 4 chars), and seqCount is sane. Pure reads, SEH.
static bool SeqTable_TryResolve(uintptr_t base, uintptr_t& hdrOut, uintptr_t& studioOut,
	uintptr_t& vmodelOut, unsigned int& seqCountOut, char (&model)[80])
{
	hdrOut = studioOut = vmodelOut = 0;
	seqCountOut = 0;
	model[0] = 0;
	__try
	{
		const uintptr_t hdr = *reinterpret_cast<uintptr_t*>(base + 0x1000);   // cached CStudioHdr (GetModelPtr)
		if (!hdr)
			return false;
		const uintptr_t studio = *reinterpret_cast<uintptr_t*>(hdr + 0x08);
		const uintptr_t vmodel = *reinterpret_cast<uintptr_t*>(hdr + 0x10);
		if (!studio || !vmodel)
			return false;
		int n = 0;
		for (; n < 64; ++n)
		{
			const char ch = *reinterpret_cast<char*>(studio + 0x10 + n);   // S21 studiohdr name @ +0x10
			model[n] = ch;
			if (!ch) break;
			if (ch < 0x20 || ch > 0x7E)
				return false;   // unprintable byte = not a model name = wrong delta / garbage
		}
		model[n < 79 ? n : 79] = 0;
		if (n < 4)
			return false;
		const unsigned int cnt = *reinterpret_cast<uint16_t*>(vmodel + 0x02);   // S21 vmodel seqCount (u16)
		if (cnt == 0 || cnt >= 4096)
			return false;
		hdrOut = hdr;
		studioOut = studio;
		vmodelOut = vmodel;
		seqCountOut = cnt;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void SeqTable_DumpForSlot(int slot, bool dedup = false)
{
	if (slot < 0 || slot >= 16384)
	{
		if (!dedup)
			Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) bad slot %d (usage: sdk_dump_seqtable [slot|all], default all)\n", slot);
		return;
	}

	const uintptr_t iface = Bridge_GetEntityBySlot((unsigned int)slot);
	if (!iface)
	{
		if (!dedup)
			Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) slot=%d: no entity at that slot (are you in a match?)\n", slot);
		return;
	}
	++s_seqSweepEnt;

	static const int kDeltas[4] = { 0x00, 0x08, 0x10, 0x18 };
	uintptr_t hdr = 0, studio = 0, vmodel = 0, mseq = 0;
	unsigned int seqCount = 0;
	char model[80] = {};
	bool ok = false;
	if (s_seqIfaceDelta >= 0)
	{
		// Delta locked earlier earlier: a null/invalid chain is now a genuine
		// "model not cached" skip, not a candidate mismatch.
		ok = SeqTable_TryResolve(iface - s_seqIfaceDelta, hdr, studio, vmodel, seqCount, model);
		if (!ok)
		{
			++s_seqSweepNoHdr;
			if (!dedup)
				Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) slot=%d iface=%p delta=0x%X -- studiohdr chain not resolvable (model not loaded?)\n",
					slot, reinterpret_cast<void*>(iface), s_seqIfaceDelta);
			return;
		}
	}
	else
	{
		for (int d = 0; d < 4; ++d)
		{
			if (SeqTable_TryResolve(iface - kDeltas[d], hdr, studio, vmodel, seqCount, model))
			{
				s_seqIfaceDelta = kDeltas[d];
				Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) interface->entity delta RESOLVED = 0x%X (slot=%d model='%s') -- record this in RE docs\n",
					s_seqIfaceDelta, slot, model);
				ok = true;
				break;
			}
		}
		if (!ok)
		{
			++s_seqSweepNoResolve;
			if (!dedup)
				Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) slot=%d iface=%p -- no candidate delta {0,8,0x10,0x18} validated; model not loaded or non-animating\n",
					slot, reinterpret_cast<void*>(iface));
			return;
		}
	}

	__try
	{
		mseq = *reinterpret_cast<uintptr_t*>(vmodel + 0x10);   // S21 m_seq array base
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }

	if (dedup && model[0] && SeqTable_AlreadyDumped(model))
		return;
	++s_seqSweepDumped;

	if (seqCount > 4096) seqCount = 4096;   // sanity clamp
	// Full table goes to bridge_trace.log (file sink, no console spam, no console-
	// render AV); console gets only the summary so the operator knows it ran.
	Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) slot=%d model='%s' seqCount=%u -> bridge_trace.log\n",
		slot, model, seqCount);
	BridgeTrace_Log("[SEQTABLE](cl) slot=%d model='%s' seqCount=%u vmodel=%p ===BEGIN===\n",
		slot, model, seqCount, reinterpret_cast<void*>(vmodel));

	for (unsigned int i = 0; i < seqCount; ++i)
	{
		// Copy the seq label inside __try. 192 bytes; rseq paths are ~70-100 chars.
		char nm[192];
		__try
		{
			const uintptr_t seqdesc = *reinterpret_cast<uintptr_t*>(mseq + 0x10ull * i + 0x08);
			if (!seqdesc) { nm[0] = '-'; nm[1] = 0; }
			else
			{
				const uint16_t w = *reinterpret_cast<uint16_t*>(seqdesc);
				const char* s = reinterpret_cast<const char*>(seqdesc + ((unsigned)(w & 0xFFFE) << (4 * (w & 1))));
				int k = 0;
				for (; k < 191; ++k) { const char c = s[k]; nm[k] = c; if (!c) break; }
				nm[k] = 0;
				if (nm[0] == 0) { nm[0] = '?'; nm[1] = 0; }
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { nm[0] = '<'; nm[1] = 'x'; nm[2] = '>'; nm[3] = 0; }
		BridgeTrace_Log("[SEQTABLE](cl) %4u %s\n", i, nm);
	}
	BridgeTrace_Log("[SEQTABLE](cl) slot=%d ===END=== (%u seqs)\n", slot, seqCount);
	Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) slot=%d done (%u seqs written to bridge_trace.log)\n", slot, seqCount);
}

static void CC_DumpSeqTable(const CCommand& args)
{
	if (args.ArgC() > 1 && atoi(args.Arg(1)) > 0)
	{
		SeqTable_DumpForSlot(atoi(args.Arg(1)), false);   // explicit single slot
		return;
	}
	// ALL mode (default): sweep every slot, dump each unique loaded model once.
	s_seqTableDumpedCount = 0;   // fresh sweep each invocation
	s_seqSweepEnt = s_seqSweepNoHdr = s_seqSweepNoResolve = s_seqSweepDumped = 0;
	Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) ALL-mode sweep begin (slots 0..16383)\n");
	int dumped = 0;
	for (int slot = 0; slot < 16384; ++slot)
	{
		const int before = s_seqTableDumpedCount;
		SeqTable_DumpForSlot(slot, true);
		if (s_seqTableDumpedCount > before) ++dumped;
	}
	Warning(eDLL_T::CLIENT, "[SEQTABLE](cl) ALL-mode sweep done: %d unique models dumped -> bridge_trace.log "
		"(ents=%d chain-null=%d no-delta-match=%d tables=%d delta=0x%X)\n",
		dumped, s_seqSweepEnt, s_seqSweepNoHdr, s_seqSweepNoResolve, s_seqSweepDumped, s_seqIfaceDelta);
}
static ConCommand sdk_dump_seqtable("sdk_dump_seqtable", CC_DumpSeqTable,
	"Dump sequence tables (index->name) to bridge_trace.log. No arg = all unique loaded models; [slot] = that entity slot only.", FCVAR_RELEASE);

// Last local-player SelectWeightedSequence snapshot. invokeCount==0 => hook never fired.
struct SeqPickSnapshot_t
{
	int          act;
	int          nMods;
	uint16_t     mods[64];
	int          ret;
	unsigned int invokeCount;
};

// Locomotion fallback: 689+dir walk / 679+dir run. LEFT/RIGHT unbound on legends.
// Translate player+8118 queued activity S3->S21. Unlisted IDs pass through.
static ConVar bridge_act_xlat_qact("bridge_act_xlat_qact", "1", FCVAR_RELEASE,
	"[ACT-XLAT] SHIP DEFAULT 1 (jump T-pose PROVEN FIXED): translate queued activity "
	"(player+8118) S3 enum -> S21 before reads it. Fixes the"
	"jump T-pose where the dedi sends S3's 693=JUMP_START which lands as S21's UNBOUND "
	"STAGGERED_WALK_RIGHT. 0=off,1=on. Flip OFF only to A/B-test or to confirm the bug returns.");

// Six local writes of +8118, all in cases 6/7/8/9. Other qAct values are wire.
static inline bool Bridge_IsLocalWrittenQAct(uint16_t a)
{
	if (a == 713) return true;                          // case 6: ACT_MP_JUMP_START
	if (a == 740) return true;                          // case 7: ACT_MP_DOUBLEJUMP
	if (a == 744) return true;                          // case 8: ACT_MP_DODGE
	if (a >= 771 && a <= 773) return true;              // case 9: ACT_MP_WALLJUMP_LEFT/RIGHT/UP
	return false;
}

static __int16 __fastcall Hook_ActivitySelector(__int64 animstate)
{
	const bool xlatOn = bridge_act_xlat_qact.GetBool();

	// Fast path: nothing to do (no translation) -> straight passthrough
	if (!animstate || !xlatOn)
		return s_origActivitySelector ? s_origActivitySelector(animstate) : (__int16)-1;

	bool translated = false;
	uintptr_t player = 0;
	uint16_t qActOld = 0;
	int qActNew = 0;

	if (xlatOn)
	{
		__try
		{
			player = *(uintptr_t*)(animstate + 464);
			if (player)
			{
				const uint8_t qFlag = *(uint8_t*)(player + 8108);
				if (qFlag)
				{
					qActOld = *(uint16_t*)(player + 8118);
					// Skip translation for local-written S21 values (713 = JUMP_START, not S3 713).
					if (!Bridge_IsLocalWrittenQAct(qActOld))
					{
						qActNew = Bridge_TranslateS3ActivityToS21_Static((int)qActOld);
						if (qActNew != (int)qActOld && qActNew > 0 && qActNew < 0x10000)
						{
							// TRANSLATE-AND-RESTORE: write S21 value so orig reads it,
							// then put the S3 value back after orig returns so the next
							// frame translates from the same S3 input (no chaining).
							*(uint16_t*)(player + 8118) = (uint16_t)qActNew;
							translated = true;
						}
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { translated = false; }
	}

	const __int16 ret = s_origActivitySelector ? s_origActivitySelector(animstate) : (__int16)-1;

	if (translated)
	{
		__try
		{
			*(uint16_t*)(player + 8118) = qActOld;     // RESTORE original S3 value
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	return ret;
}


// Local-player speed measure vs walk/fast thresholds. player=*(ctx+464), settings=*(player+9624).
// Formats a captured frame list as module+0xRVA pairs.
static void Bridge_FormatFrameList(void* const* ppFrames, int nFrames,
	HMODULE hSelfModule, char* pOut, size_t outSize)
{
	if (!pOut || outSize == 0)
		return;
	pOut[0] = '\0';
	if (!ppFrames || nFrames <= 0)
		return;

	int off = 0;
	for (int i = 0; i < nFrames && off < (int)outSize; ++i)
	{
		HMODULE hm = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(ppFrames[i]), &hm);

		const uintptr_t rva = hm ? (reinterpret_cast<uintptr_t>(ppFrames[i]) - reinterpret_cast<uintptr_t>(hm))
		                          : reinterpret_cast<uintptr_t>(ppFrames[i]);

		char modBuf[24];
		const char* modName;
		if (hm && hm == GetModuleHandleW(NULL))
			modName = "r5apex";
		else if (hm && hm == hSelfModule)
			modName = "client.dll";
		else if (hm)
		{
			_snprintf_s(modBuf, sizeof(modBuf), _TRUNCATE, "0x%llX", (unsigned long long)reinterpret_cast<uintptr_t>(hm));
			modName = modBuf;
		}
		else
		{
			modName = "?";
		}

		const int n = _snprintf_s(pOut + off, outSize - (size_t)off, _TRUNCATE,
			"%s+0x%llX ", modName, (unsigned long long)rva);
		if (n < 0)
			break;
		off += n;
	}
}

// GetSequenceName walks studiohdr mid-swap and AVs; VEH beats frame SEH. Names unused.

// Base C_BaseAnimating::StudioFrameAdvance (plain animating props, no overlays).
// [ANIM-FIX]/[SEQ-HOLD]/[ANIM-FIX-TRACE] removed (PM12) -- pure pass-through now.
static void __fastcall Hook_StudioFrameAdvance(uintptr_t ent)
{
	if (s_origStudioFrameAdvance)
		s_origStudioFrameAdvance(ent);
}

// C_BaseAnimatingOverlay::StudioFrameAdvance (players / NPCs). Same anchor fields; this is the
// path that actually animates player bodies.
static void __fastcall Hook_StudioFrameAdvanceOverlay(uintptr_t ent, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
	if (s_origStudioFrameAdvanceOverlay)
		s_origStudioFrameAdvanceOverlay(ent, a2, a3, a4);
}

// Small shared registry of C_Player pointers. Any pointer passed to C_Player::MovePostThink IS a
// player, so this lets the apply hooks -- which fire for ALL C_BaseAnimating entities -- scope
// player-only lookups without a risky +0x3610 read on a non-player.
static uintptr_t s_knownPlayers[16] = { 0 };
static void BridgeAnim_NotePlayer(uintptr_t p)
{
	if (!p) return;
	int freeSlot = -1;
	for (int i = 0; i < 16; ++i) {
		if (s_knownPlayers[i] == p) return;
		if (!s_knownPlayers[i] && freeSlot < 0) freeSlot = i;
	}
	if (freeSlot >= 0) s_knownPlayers[freeSlot] = p;
}

// compare an entity against the bridge local player never depend on another subsystem having
// resolved it first -- BridgeLocalPlayer performs its own resolve on first use, so the
// hooks stay correct regardless of what any other subsystem has resolved.
static inline uintptr_t BridgeLocalPlayer(void)
{
	if (!s_pathAGetLocal)
		s_pathAGetLocal = (PFN_GetLocalPlayer) NetObs_Sym(NetObsSym_t::GetLocalPlayer);
	return s_pathAGetLocal ? s_pathAGetLocal(0) : 0;
}

// SetSignonState / CL_FullyConnected: pass-through, log every transition.
bool s_bridgePdefReady = false;
static void S21Bridge_ClearPlaylistOverrides(void);
static void S21Bridge_OnSessionEnded(const char* reason);
static void S21Bridge_ResetForNewConnect(void);
static bool S21Bridge_StartHandshake(void);
void S21Bridge_OnConnAccept(const char* mapName, const char* gameMode);
void S21Bridge_OnConnReject(const char* reason);
void S21Bridge_RememberChallengeMap(const char* pszMap);

static char __fastcall Hook_CS_SetSignonState(__int64 a1, int newState, int spawnCount, __int64 pMsg)
{
	if (a1 && !s_clientStatePtr)
		s_clientStatePtr = static_cast<uintptr_t>(a1);

	// Server playlist var overrides are per-connection state. Drop them below
	// SIGNONSTATE_CONNECTED (NONE on disconnect, CHALLENGE on a fresh connect) so
	// one server's overrides can never be read on the next one, or in the lobby.
	if (newState < 2)
	{
		S21Bridge_ClearPlaylistOverrides();
		S21Bridge_FlushS2CScriptRemote("signon < CONNECTED");
		s_bridgePdefReady = false;
	}
	if (newState == 10)
		S21Bridge_S2CScriptRemote_ResetAppliedTick();
	if (newState == 0)
		S21Bridge_OnSessionEnded("signon NONE");
	char phaseIn[32];
	char phaseOut[32];
	V_snprintf(phaseIn, sizeof(phaseIn), "signon-%d-entry", newState);
	V_snprintf(phaseOut, sizeof(phaseOut), "signon-%d-return", newState);
	HeapCanary::Checkpoint(phaseIn);

	const char result = s_origCS_SetSignonState
		? s_origCS_SetSignonState(a1, newState, spawnCount, pMsg)
		: 0;

	HeapCanary::Checkpoint(phaseOut);

	// CBaseClient::SetSignonState never runs on this client; C2S NEW..FULL
	// has to be queued from the CClientState hook that actually fires.
	if (s_bridgeActive && newState >= 2 && newState <= 8
		&& newState < s_lastSentSignonState)
	{
		S21Bridge_ClearPendingSignon();
		s_lastSentSignonState = -1;
		S21Bridge_ResetReliableRecv("CS signon rewind");
		Warning(eDLL_T::ENGINE,
			"[BRIDGE-SIGNON] CS rewind to %d (was C2S floor above it)\n",
			newState);
	}
	if (s_bridgeActive && newState >= 2 && newState <= 8
		&& newState >= s_lastSentSignonState)
	{
		// S2C used -1 so S21 skips the future-spawn reject. S3 still
		// wants the real spawn count on C2S or the walk dies at PRESPAWN.
		int c2sSpawn = spawnCount;
		if (s_signonRewalk && s_signonSeqSpawn >= 0)
			c2sSpawn = s_signonSeqSpawn;
		S21Bridge_QueueSignon(newState, c2sSpawn);
		s_lastSentSignonState = newState;
		SDK_Log("[BRIDGE-SIGNON] CS queued net_SignonState(%d) spawn=%d\n",
			newState, c2sSpawn);

		if (s_signonRewalk)
		{
			Warning(eDLL_T::ENGINE,
				"[BRIDGE-SIGNON] re-walk C2S net_SignonState(%d) spawn=%d\n",
				newState, c2sSpawn);
			if (newState >= 8)
				s_signonRewalk = false;
		}
	}

	return result;
}
static void S21Bridge_ResetClockAckState(void); // defined with Hook_ClockDrift

static __int64 __fastcall Hook_CL_FullyConnected(char* mapname)
{
	// Re-arm clock-drift de-dupe + ack monitors before the connection's first feeds.
	S21Bridge_ResetClockAckState();
	S21Bridge_FlushS2CScriptRemote("CL_FullyConnected");

	return s_origCL_FullyConnected ? s_origCL_FullyConnected(mapname) : 0;
}

static void __fastcall Hook_MovePostThink(uintptr_t player)
{
	BridgeAnim_NotePlayer(player);   // [APPLY-ORDER]/FIX1 scope: this ptr is a C_Player


	// [LOCAL-VIEW-INSTALL] PATH A -- call (player+24, a2) to set.
	// Modes: 1=one-shot a2=0, 2=one-shot a2=1, 3=per-tick a2=0.
	{
		const int  viewSel = bridge_local_view_install.GetInt();
		const bool perTick = (viewSel == 3);
		const int  a2_arg  = (viewSel == 2) ? 1 : 0;
		if (player && viewSel >= 1 && viewSel <= 3
		    && (perTick || InterlockedCompareExchange(&s_localViewInstalled, 1, 0) == 0))
		{
			if (!s_localViewFnRT)
				s_localViewFnRT = NetObs_Sym(NetObsSym_t::LocalViewFn);
			if (!s_viewHandleRT)
				s_viewHandleRT = NetObs_Sym(NetObsSym_t::ViewHandle);

			if (s_localViewFnRT && s_viewHandleRT)
			{
				using PFN_LocalViewInstall = __int64(__fastcall*)(uintptr_t, int);
				const PFN_LocalViewInstall fn = reinterpret_cast<PFN_LocalViewInstall>(s_localViewFnRT);
				const uintptr_t subobj = player + 24;   // secondary-vtable `this` (function does a1-24 to reach player base)

				uint32_t preHandle = 0xDEADBEEF, postHandle = 0xDEADBEEF;
				uint32_t mEHandle  = 0xDEADBEEF;
				__try { preHandle = *reinterpret_cast<const uint32_t*>(s_viewHandleRT); } __except (EXCEPTION_EXECUTE_HANDLER) {}
				__try { mEHandle  = *reinterpret_cast<const uint32_t*>(player + 8); }     __except (EXCEPTION_EXECUTE_HANDLER) {}

				__int64 ret = 0;
				__try { ret = fn(subobj, a2_arg); } __except (EXCEPTION_EXECUTE_HANDLER) { ret = 0xBADC0DE; }

				__try { postHandle = *reinterpret_cast<const uint32_t*>(s_viewHandleRT); } __except (EXCEPTION_EXECUTE_HANDLER) {}

				// Log: first fire always; per-tick mode logs only on CHANGE (skip steady-state)
				static volatile LONG s_firstLogged = 0;
				const bool firstFire   = (InterlockedExchange(&s_firstLogged, 1) == 0);
				const bool stateChange = (preHandle != postHandle);
				if (firstFire || stateChange)
				{
					Warning(eDLL_T::CLIENT,
						"[LOCAL-VIEW-INSTALL] (player+24=0x%llX, a2=%d) --"
						"PRE=0x%08X POST=0x%08X | bridge.m_EHandle=0x%08X | ret=0x%llX%s\n",
						(unsigned long long)subobj, a2_arg,
						preHandle, postHandle, mEHandle,
						(unsigned long long)ret,
						firstFire ? " (first fire)" : " (state changed)");
				}
			}
			else if (InterlockedCompareExchange(&s_localViewInstalled, 1, 0) == 0)
			{
				Warning(eDLL_T::CLIENT,
					"[LOCAL-VIEW-INSTALL] runtime VA resolve failed: fn=0x%llX viewHandle=0x%llX\n",
					(unsigned long long)s_localViewFnRT, (unsigned long long)s_viewHandleRT);
			}
		}
		else if (viewSel == 0 && s_localViewInstalled)
		{
			InterlockedExchange(&s_localViewInstalled, 0);   // reset latch when user toggles off
		}
	}

	if (s_origMovePostThink)
		s_origMovePostThink(player);
}

//-----------------------------------------------------------------------------
// Prediction ack at CPrediction+0xCC. Finalizer zeroes it when byte185 flaps;
// restore the pre-call value so depth stays nLatestCmd-nAcked.
//-----------------------------------------------------------------------------
typedef __int64 (__fastcall* PFN_PredFinalize)(__int64);
static PFN_PredFinalize s_origPredFinalize = nullptr;
static uintptr_t s_byteA9 = 0;           // (partner gate of the splitscreen predicate)
static uintptr_t s_predConvarGlobal = 0; // global qword @ (holds the predicate's convar/obj ptr)

static ConVar bridge_ack_fix("bridge_ack_fix", "1", FCVAR_RELEASE,
	"FIX: restore the prediction command-ack accumulator (CPrediction+0xCC) after the finalizer "
	"spuriously zeroes it each frame for the bridge -> prediction depth collapses to native, prediction "
	"stays ON. 1 = fix on (default), 0 = native (rubber-band returns).");

static __int64 __fastcall Hook_PredFinalize(__int64 a1)
{
	const bool wantFix = bridge_ack_fix.GetBool();
	int ackBefore = 0;
	if (wantFix && a1)
	{
		__try { ackBefore = *reinterpret_cast<int*>(a1 + 204); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	const __int64 r = s_origPredFinalize ? s_origPredFinalize(a1) : 0;

	if (wantFix && a1)
	{
		__try
		{
			const int  ackAfterOrig = *reinterpret_cast<int*>(a1 + 204);
			const bool didReset = (ackAfterOrig < ackBefore);
			if (didReset)
				*reinterpret_cast<int*>(a1 + 204) = ackBefore;   // undo the spurious per-frame ack reset
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	return r;
}

//-----------------------------------------------------------------------------
// Script particle handle is queued; a non-negative handle does not mean it spawned.
// Realm gate vs detail/frustum/perf cull. Pass-through unless bridge_pfx_gate_trace.
//-----------------------------------------------------------------------------
typedef __int64(__fastcall* PFN_PfxCreateFromDef)(__int64 particleProp, __int64 pDef,
	__int64 attachType, __int64 attachmentId, float* originOffset,
	unsigned __int64 createInRealms, char persistent, __int64 matOffset);
typedef __int64(__fastcall* PFN_PfxEffectFactory)(__int64 ownerEnt, __int64 pDef,
	const float* spawnCullPos, const char* pszName);
typedef bool(__fastcall* PFN_PfxIsSpawnCulled)(uintptr_t mgr, __int64 pDef, const float* pos);

static PFN_PfxCreateFromDef s_origPfxCreateFromDef = nullptr;
static PFN_PfxEffectFactory s_origPfxEffectFactory = nullptr;
static PFN_PfxIsSpawnCulled s_pfnPfxIsSpawnCulled = nullptr;

// C_BaseEntity: m_Particles (CParticleProperty) @ +0x778, m_realmsBitMask @ +0x938.
static constexpr ptrdiff_t kPfxEntParticles = 0x778;
static constexpr ptrdiff_t kPfxEntRealms = 0x938;
// C_ParticleSystemDefinition: the name pointer @ +504 is only live while the CUtlString
// length @ +528 is non-zero. Detail-level requirements @ +0x19C/+0x1A0, m_bSkipIfOverbudget @ +0x1F0.
static constexpr ptrdiff_t kPfxDefName = 504;
static constexpr ptrdiff_t kPfxDefNameLen = 528;
static constexpr ptrdiff_t kPfxDefCpuLevel = 0x19C;
static constexpr ptrdiff_t kPfxDefGpuLevel = 0x1A0;
static constexpr ptrdiff_t kPfxDefSkipOver = 0x1F0;
// CParticleSystemMgr_Client: detail-level caps @ +0xE2C/+0xE30, perf budget fraction @ +0x1D8.
static constexpr ptrdiff_t kPfxMgrCpuLevel = 0xE2C;
static constexpr ptrdiff_t kPfxMgrGpuLevel = 0xE30;
static constexpr ptrdiff_t kPfxMgrPerfFrac = 0x1D8;
// C_EntInfo stride is 0x20 on S21 (entity @ +0, serial @ +8); do not reuse other-build strides.
static constexpr size_t kPfxEntInfoStride = 0x20;

static const int* s_pfxLocalViewPlayer = nullptr;   // ehandle: low16 index, high16 serial
static const uint8_t* s_pfxEntInfoList = nullptr;
static uintptr_t* s_pfxMgrSlot = nullptr;           // g_pParticleSystemMgr_Client
static const uint8_t* s_pfxDetailSkip = nullptr;    // non-zero = detail-level cull bypassed

static ConVar bridge_pfx_gate_trace("bridge_pfx_gate_trace", "0", FCVAR_DEVELOPMENTONLY,
	"Report which gate discarded a particle effect ([PFX-GATE]). 1 = discards only, "
	"2 = every create. Pair with bridge_pfx_gate_filter to narrow it to one effect.");
static ConVar bridge_pfx_gate_filter("bridge_pfx_gate_filter", "", FCVAR_RELEASE,
	"Substring an effect name must contain to be reported by [PFX-GATE]. Empty = every effect.");

static LONG s_pfxGateLines = 0;

static const char* PfxGate_DefName(const __int64 pDef, char(&buf)[96])
{
	buf[0] = '\0';
	if (!pDef)
		return "(nodef)";

	__try
	{
		if (*reinterpret_cast<volatile uint64_t*>(pDef + kPfxDefNameLen))
		{
			const char* const nm = *reinterpret_cast<const char* volatile*>(pDef + kPfxDefName);
			if (reinterpret_cast<uintptr_t>(nm) > 0x10000)
			{
				size_t i = 0;
				for (; i < sizeof(buf) - 1 && nm[i]; ++i)
					buf[i] = nm[i];
				buf[i] = '\0';
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }

	return buf[0] ? buf : "(unnamed)";
}

static bool PfxGate_NameWanted(const char* const pszName)
{
	const char* const pszFilter = bridge_pfx_gate_filter.GetString();
	if (!pszFilter || !pszFilter[0])
		return true;
	return pszName && strstr(pszName, pszFilter) != nullptr;
}

static unsigned __int64 PfxGate_ReadRealms(const uintptr_t ent)
{
	if (!ent)
		return 0;

	unsigned __int64 realms = 0;
	__try { realms = *reinterpret_cast<const volatile unsigned __int64*>(ent + kPfxEntRealms); }
	__except (EXCEPTION_EXECUTE_HANDLER) { realms = 0; }
	return realms;
}

// Local view player realm mask. Zero also means "no view player yet", which is itself a
// discard reason -- the create path bails before it ever looks at the source entity.
static unsigned __int64 PfxGate_LocalViewRealms(void)
{
	if (!s_pfxLocalViewPlayer || !s_pfxEntInfoList)
		return 0;

	const int handle = *s_pfxLocalViewPlayer;
	if (handle == -1)
		return 0;

	const uint8_t* const info = s_pfxEntInfoList +
		static_cast<size_t>(static_cast<uint16_t>(handle)) * kPfxEntInfoStride;

	uintptr_t ent = 0;
	__try
	{
		if (*reinterpret_cast<const volatile int*>(info + 8) ==
			static_cast<int>(static_cast<unsigned>(handle) >> 16))
			ent = *reinterpret_cast<const volatile uintptr_t*>(info);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { ent = 0; }

	return PfxGate_ReadRealms(ent);
}

static bool PfxGate_ShouldPrint(void)
{
	const LONG n = InterlockedIncrement(&s_pfxGateLines);
	return n <= 200 || (n & 0x3F) == 0;
}

// Re-runs the three culls on a NULL return so the discard names itself. IsSpawnCulled is
// called through the ORIGINAL pointer -- it is a pure frustum/distance test, no side effects.
static __int64 __fastcall Hook_PfxEffectFactory(__int64 ownerEnt, __int64 pDef,
	const float* spawnCullPos, const char* pszName)
{
	const __int64 r = s_origPfxEffectFactory(ownerEnt, pDef, spawnCullPos, pszName);

	const int level = bridge_pfx_gate_trace.GetInt();
	if (level <= 0 || (r && level < 2))
		return r;

	char nameBuf[96];
	const char* const name = PfxGate_DefName(pDef, nameBuf);
	if (!PfxGate_NameWanted(name))
		return r;

	const char* reason = r ? "ok" : "alloc/ctor";
	if (!r)
	{
		const uintptr_t mgr = s_pfxMgrSlot ? *s_pfxMgrSlot : 0;
		__try
		{
			if (mgr && s_pfnPfxIsSpawnCulled && s_pfnPfxIsSpawnCulled(mgr, pDef, spawnCullPos))
			{
				reason = "SPAWNCULL";
			}
			else if (mgr && *reinterpret_cast<const volatile float*>(mgr + kPfxMgrPerfFrac) > 1.0f &&
				*reinterpret_cast<const volatile unsigned char*>(pDef + kPfxDefSkipOver))
			{
				reason = "PERFCULL";
			}
			else if (mgr && s_pfxDetailSkip && !*s_pfxDetailSkip)
			{
				const int mgrCpu = *reinterpret_cast<const volatile int*>(mgr + kPfxMgrCpuLevel);
				const int mgrGpu = *reinterpret_cast<const volatile int*>(mgr + kPfxMgrGpuLevel);
				const int defCpu = *reinterpret_cast<const volatile int*>(pDef + kPfxDefCpuLevel);
				const int defGpu = *reinterpret_cast<const volatile int*>(pDef + kPfxDefGpuLevel);
				if (defCpu > (mgrCpu > 1 ? mgrCpu : 1) || defGpu > (mgrGpu > 1 ? mgrGpu : 1))
					reason = "DETAILCULL";
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { reason = "probe-fault"; }
	}

	if (PfxGate_ShouldPrint())
		Warning(eDLL_T::CLIENT,
			"[PFX-GATE] factory '%s' ent=0x%llX cullPos=%d(%.1f %.1f %.1f) -> %s\n",
			name, (unsigned long long)ownerEnt, spawnCullPos ? 1 : 0,
			spawnCullPos ? spawnCullPos[0] : 0.0f,
			spawnCullPos ? spawnCullPos[1] : 0.0f,
			spawnCullPos ? spawnCullPos[2] : 0.0f, reason);

	return r;
}

// Only the realm gate is reported here; anything that gets past it and still fails is
// named by Hook_PfxEffectFactory, so the two hooks never double-report one discard.
static __int64 __fastcall Hook_PfxCreateFromDef(__int64 particleProp, __int64 pDef,
	__int64 attachType, __int64 attachmentId, float* originOffset,
	unsigned __int64 createInRealms, char persistent, __int64 matOffset)
{
	const __int64 r = s_origPfxCreateFromDef(particleProp, pDef, attachType, attachmentId,
		originOffset, createInRealms, persistent, matOffset);

	if (r || bridge_pfx_gate_trace.GetInt() <= 0)
		return r;

	const unsigned __int64 viewRealms = PfxGate_LocalViewRealms();
	if ((viewRealms & createInRealms) != 0)
		return r;

	char nameBuf[96];
	const char* const name = PfxGate_DefName(pDef, nameBuf);
	if (!PfxGate_NameWanted(name))
		return r;

	const uintptr_t ent = particleProp
		? static_cast<uintptr_t>(particleProp) - kPfxEntParticles : 0;

	if (PfxGate_ShouldPrint())
		Warning(eDLL_T::CLIENT,
			"[PFX-GATE] REALM '%s' ent=0x%llX entRealms=0x%llX createIn=0x%llX "
			"viewRealms=0x%llX attach=%d/%d\n",
			name, (unsigned long long)ent,
			(unsigned long long)PfxGate_ReadRealms(ent),
			(unsigned long long)createInRealms,
			(unsigned long long)viewRealms,
			(int)attachType, (int)attachmentId);

	return r;
}

//-----------------------------------------------------------------------------
// Purpose: resolve + attach the [PFX-GATE] pair. Both anchors are interior landmarks
// (the realm gate and the cull chain), each a single hit module-wide.
//-----------------------------------------------------------------------------
static void PfxGate_Install(void)
{
	// CreateFromDefWithOffset realm gate at +0x9D. m_realmsBitMask at +0x938.
	const CMemory realmGate = Module_FindPattern(g_GameDll,
		"8B 05 ?? ?? ?? ?? 83 F8 FF 0F 84 ?? ?? ?? ?? 0F B7 D0 48 8D 05 ?? ?? ?? ?? "
		"48 C1 E2 05 48 03 D0 0F B7 05 ?? ?? ?? ?? 39 42 08 0F 85 ?? ?? ?? ?? "
		"48 8B 02 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 80 38 09 00 00");

	// CParticleEffect cull chain at +0x9F: IsSpawnCulled, detailSkipFlag.
	const CMemory cullChain = Module_FindPattern(g_GameDll,
		"48 8B D8 48 8B 3D ?? ?? ?? ?? 75 34 8B 97 2C 0E 00 00 B8 01 00 00 00 "
		"44 8B 87 30 0E 00 00 3B D0 8B C8 0F 4F CA 39 8B 9C 01 00 00 0F 8F ?? ?? ?? ?? "
		"44 3B C0 41 0F 4F C0 39 83 A0 01 00 00 7F 7C 4C 8B C6 48 8B D3 48 8B CF "
		"E8 ?? ?? ?? ?? 84 C0 75 6A");

	if (!realmGate || !cullChain)
	{
		Warning(eDLL_T::CLIENT,
			"[PFX-GATE] pattern unresolved (realmGate=0x%llX cullChain=0x%llX) -- "
			"particle discard reporting DISABLED.\n",
			(unsigned long long)realmGate.GetPtr(), (unsigned long long)cullChain.GetPtr());
		return;
	}

	s_origPfxCreateFromDef = realmGate.Offset(-0x9D).RCast<PFN_PfxCreateFromDef>();
	s_pfxLocalViewPlayer = realmGate.ResolveRelativeAddress(2, 6).RCast<const int*>();
	s_pfxEntInfoList = realmGate.Offset(0x12).ResolveRelativeAddress(3, 7).RCast<const uint8_t*>();

	s_origPfxEffectFactory = cullChain.Offset(-0x9F).RCast<PFN_PfxEffectFactory>();
	s_pfxMgrSlot = cullChain.Offset(0x03).ResolveRelativeAddress(3, 7).RCast<uintptr_t*>();
	s_pfnPfxIsSpawnCulled = cullChain.Offset(0x49).FollowNearCall().RCast<PFN_PfxIsSpawnCulled>();
	s_pfxDetailSkip = cullChain.Offset(-0x07).ResolveRelativeAddress(2, 7).RCast<const uint8_t*>();

	if (DetourTransactionBegin() != NO_ERROR)
	{
		s_origPfxCreateFromDef = nullptr; s_origPfxEffectFactory = nullptr;
		return;
	}
	DetourUpdateThread(GetCurrentThread());
	const auto rCD = DetourAttach(reinterpret_cast<PVOID*>(&s_origPfxCreateFromDef), reinterpret_cast<PVOID>(&Hook_PfxCreateFromDef));
	const auto rEF = DetourAttach(reinterpret_cast<PVOID*>(&s_origPfxEffectFactory), reinterpret_cast<PVOID>(&Hook_PfxEffectFactory));
	if (rCD != NO_ERROR || rEF != NO_ERROR)
	{
		DetourTransactionAbort();
		s_origPfxCreateFromDef = nullptr; s_origPfxEffectFactory = nullptr;
		Warning(eDLL_T::CLIENT, "[PFX-GATE] hook install FAILED (rCD=%ld rEF=%ld)\n", (long)rCD, (long)rEF);
		return;
	}
	DetourTransactionCommit();

	Warning(eDLL_T::CLIENT,
		"[PFX-GATE] installed: CreateFromDefWithOffset @ 0x%p, effect factory @ 0x%p, "
		"IsSpawnCulled @ 0x%p, mgrSlot @ 0x%p, viewPlayerHandle @ 0x%p, entInfo @ 0x%p "
		"(bridge_pfx_gate_trace gates all output)\n",
		(void*)s_origPfxCreateFromDef, (void*)s_origPfxEffectFactory,
		(void*)s_pfnPfxIsSpawnCulled, (void*)s_pfxMgrSlot,
		(void*)s_pfxLocalViewPlayer, (void*)s_pfxEntInfoList);
}

// [SLOT-KEEPALIVE] mgr2 per-slot DEACTIVATOR -- the sibling of the SLOT-INSTALL
// setter (mgr2-vtbl[38]). bridge_slot_keepalive removed; hook is a pass-through.
typedef __int64(__fastcall* PFN_SlotDeactivate)(__int64, __int64, __int64, __int64);
static PFN_SlotDeactivate s_origSlotDeactivate = nullptr;

// TE event batch: per event [1 bit full/delta][class id if full][payload].
// Delta events chain off the previous; one mis-size shears the batch.
typedef __int64(__fastcall* PFN_EvtWireDecode)(__int64 rt, __int64 prevBuf, __int64 wireBuf, __int64 outBuf, __int64 a5, __int64 a6);
static PFN_EvtWireDecode s_origEvtWireDecode = nullptr;

static __int64 __fastcall Hook_EvtWireDecode(__int64 rt, __int64 prevBuf, __int64 wireBuf, __int64 outBuf, __int64 a5, __int64 a6)
{
	NetObsTe_ResetIdxSeq();
	if (rt && wireBuf)
	{
		const __int64 dec = *reinterpret_cast<const __int64*>(rt + 0x4C0);
		if (!dec)
			Bridge_BitReaderOverflow(static_cast<uintptr_t>(wireBuf));
	}
	return s_origEvtWireDecode ? s_origEvtWireDecode(rt, prevBuf, wireBuf, outBuf, a5, a6) : 0;
}

// [SLOT-KEEPALIVE] Intercept (the per-slot DEACTIVATOR). bridge_slot_keepalive
// removed -- pure pass-through now.
static __int64 __fastcall Hook_SlotDeactivate(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
	return s_origSlotDeactivate ? s_origSlotDeactivate(a1, a2, a3, a4) : 0;
}

// [SEQ-WRITER]/[SEQ-BLOCK] Detour on the SetSequence-equivalent.
// bridge_seq_writer_trace / bridge_seq_block removed -- pure pass-through now.
static double __fastcall Hook_SetSequence(uintptr_t ent, unsigned __int16 seq)
{
	return s_origSetSequence ? s_origSetSequence(ent, seq) : 0.0;
}

// Snapshot m_animSequence at entry, run orig, restore for gate==0 (server-animated).
static ConVar bridge_anim_wire_skip("bridge_anim_wire_skip", "1", FCVAR_RELEASE,
	"[ANIM-WIRE-SKIP] force C_Player::SkipsAnimationData TRUE for the bridge local player ONLY while "
	"its client animstate is live (3P windows: skydive/rides/spawn-blend), so S3 wire anim cannot "
	"stomp the client author; null animstate = native wire apply (S21 SkipsAnimationData). 1=on (ship).");

// Clamp moveDirection 4 (RIGHT, unbound) -> 3 (LEFT, mirrored) before activity resolve.
static ConVar bridge_anim_dir_clamp("bridge_anim_dir_clamp", "0", FCVAR_RELEASE,
	"[ANIM-DIR-CLAMP] clamp animstate moveDirection RIGHT->LEFT before activity resolve. "
	"Leftover for an unbound-693 T-pose that was never RUN_RIGHT; leave off. Bridge local player only.");

// S21 C_Player::m_animActive @ +0x99E. SkipsAnimationData yields wire while set.
// Used by wire_skip (AnimWireSkip_IsClientAuthored) -- the authorship yield gate.
static constexpr ptrdiff_t kPlayerOff_AnimActive = 0x99E;

// ComputeMainSequence: clamp animstate+0x64 direction 4->3 before 689+dir resolve.
static __int64 (__fastcall *s_origComputeMainSeq)(__int64 a1) = nullptr;
static __int64 __fastcall Hook_ComputeMainSeq(__int64 a1)
{
	if (bridge_anim_dir_clamp.GetBool() && a1)
	{
		__try
		{
			const uintptr_t player = *reinterpret_cast<const uintptr_t*>(a1 + 464);  // m_player = *(a1+0x1D0)
			if (player && player == BridgeLocalPlayer())
			{
				int dir = *reinterpret_cast<int*>(a1 + 100);  // moveDirection at animstate+0x64
				if (dir == 4)
				{
					*reinterpret_cast<int*>(a1 + 100) = 3;     // MOVE_RIGHT -> MOVE_LEFT
					*reinterpret_cast<int*>(a1 + 96)  = 3;     // moveDirectionQuad too
					static uint32_t s_dcn = 0;
					if (s_dcn < 10)
					{
						++s_dcn;
						Warning(eDLL_T::CLIENT,
							"[ANIM-DIR-CLAMP] moveDirection 4->3 (MOVE_RIGHT->MOVE_LEFT) for bridge local player\n");
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	return s_origComputeMainSeq ? s_origComputeMainSeq(a1) : 0;
}

// SelectWeightedSequence snapshot for the local player (incl. transient weapon mods).
typedef unsigned short (__fastcall* PFN_SelectWeightedSeq_Hook)(uintptr_t ent, __int16 activity, uint16_t* mods, int nMods);
static PFN_SelectWeightedSeq_Hook s_origSelectWeightedSeq = nullptr;


static unsigned short __fastcall Hook_SelectWeightedSeq(uintptr_t ent, __int16 activity, uint16_t* mods, int nMods)
{
	(void)ent;
	return s_origSelectWeightedSeq
		? s_origSelectWeightedSeq(ent, activity, mods, nMods)
		: (unsigned short)0xFFFF;
}

// Null player+0x3610 around MovePostThink to skip animstate Update only.
// Separate Detours transaction so it chains outside Hook_MovePostThink.
typedef __int64 (__fastcall* PFN_AnimPredGate)(uintptr_t player);
static PFN_AnimPredGate s_origAnimPredGate = nullptr;
static uintptr_t        s_animPredGateGetLocal = 0;   // GetLocalPlayer(0)

static ConVar bridge_anim_pred_gate("bridge_anim_pred_gate", "1", FCVAR_RELEASE,
	"[ANIM-PRED-GATE] SHIP DEFAULT 1: skip animstate Update for REPLAYED"
	"(ftp=0) prediction cmds on bridge local -- match IsFirstTimePredicted. "
	"Viewmodel/camera/weapon think still run. 0=off (native-raw).");

static ConVar sdk_trav_cycle_diag("sdk_trav_cycle_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[TRAV-CYCLE] log the traversal anim phase (activity band 838..849) for the bridge "
	"local player: cycle, m_traversalAnimProgress, ground entity.");

static __int64 __fastcall Hook_AnimPredGate(uintptr_t player)
{
	if (!bridge_anim_pred_gate.GetBool())
		return s_origAnimPredGate ? s_origAnimPredGate(player) : 0;

	static uint32_t s_nSkips = 0, s_nAllows = 0;

	// Replay path: null animstate, call orig, restore. Kept outside the outer
	// __try so a fault inside orig cannot skip the restore.
	bool bReplayAnimSkip = false;
	uintptr_t* pAnimState = nullptr;
	uintptr_t savedAnimState = 0;

	__try
	{
		if (!s_animPredGateGetLocal)
			s_animPredGateGetLocal = NetObs_Sym(NetObsSym_t::GetLocalPlayer);

		const uintptr_t localPlayer = s_animPredGateGetLocal
			? reinterpret_cast<PFN_GetLocalPlayer>(s_animPredGateGetLocal)(0) : 0;

		if (player && localPlayer && player == localPlayer)
		{
			const uintptr_t predSlotAddr = NetObs_PredictionSingletonPtrAddr();
			const uintptr_t pred = predSlotAddr
				? *reinterpret_cast<const uintptr_t*>(predSlotAddr) : 0;

			if (pred)
			{
				const uint8_t bInPred = *reinterpret_cast<const uint8_t*>(pred + 0xB8);
				const uint8_t bFtp    = *reinterpret_cast<const uint8_t*>(pred + 0xC8);

				if (bInPred && !bFtp)
				{
					// REPLAYED command -- skip only the animstate update (player+0x3610).
					++s_nSkips;

					pAnimState = reinterpret_cast<uintptr_t*>(player + 0x3610); // m_PlayerAnimState
					savedAnimState = *pAnimState;
					if (savedAnimState)
					{
						bReplayAnimSkip = true;
						*pAnimState = 0;
					}
					// Fall through: call orig with animstate nulled, then restore.
				}
				else
				{
					++s_nAllows;

					// [TRAV-CYCLE] traversal phase on first-time-predicted local calls.
					if (sdk_trav_cycle_diag.GetBool())
					{
						const float flProg = *reinterpret_cast<const float*>(player + 0x2234); // m_traversalAnimProgress
						const float flCycle = *reinterpret_cast<const float*>(player + 0xF4);  // m_flCycle
						const unsigned nGround = *reinterpret_cast<const unsigned*>(player + 0x324); // m_hGroundEntity
						const int nAnimActive = static_cast<int>(
							*reinterpret_cast<const unsigned char*>(player + 0x99E)); // m_animActive

						static float s_prevProg = 0.0f;
						static uint32_t s_nTrav = 0;
						const bool bEntry = (s_prevProg == 0.0f && flProg != 0.0f);
						const bool bExit  = (s_prevProg != 0.0f && flProg == 0.0f);
						if (flProg != 0.0f || s_prevProg != 0.0f)
						{
							++s_nTrav;
							if (bEntry || bExit || (s_nTrav % 16) == 0)
							{
								Warning(eDLL_T::CLIENT,
									"[TRAV-CYCLE] #%u prog=%.4f cycle=%.4f ground=0x%08X animActive=%d\n",
									s_nTrav, flProg, flCycle, nGround, nAnimActive);
							}
						}
						s_prevProg = flProg;
					}


				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	// __finally restores animstate on unwind without swallowing faults from orig.
	__int64 r = 0;
	__try
	{
		r = s_origAnimPredGate ? s_origAnimPredGate(player) : 0;
	}
	__finally
	{
		if (bReplayAnimSkip && pAnimState)
			*pAnimState = savedAnimState;
	}
	return r;
}

static __int64 __fastcall Hook_AnimStateApply(uintptr_t ent, unsigned int a2)
{
	return s_origAnimStateApply ? s_origAnimStateApply(ent, a2) : 0;
}

// [ANIM-WIRE-SKIP] ClientThink tears down local animstate when !ShouldDrawLocalPlayer (normal 1P). Force skip only while animstate live (+0x3610 / 3P windows) so S3 wire cannot stomp client author.
static bool AnimWireSkip_IsClientAuthored(uintptr_t ent)
{
	if (!bridge_anim_wire_skip.GetBool() || !ent)
		return false;
	if (ent != BridgeLocalPlayer())
		return false;

	// m_animActive set: yield client-auth skip so wire seq/cycle/layers reach the player.
	if (*reinterpret_cast<const uint8_t*>(ent + kPlayerOff_AnimActive) != 0)
	{
		static uint32_t s_yieldN = 0;
		if (s_yieldN < 8)
		{
			++s_yieldN;
			Warning(eDLL_T::CLIENT,
				"[ANIM-WIRE-SKIP] YIELD scripted m_animActive=1 ent=%08X -- wire owns seq "
				"(locomotion skip resumes when active clears)\n",
				(unsigned)(ent & 0xFFFFFFFF));
		}
		return false;
	}

	return *reinterpret_cast<const uintptr_t*>(ent + 0x3610) != 0;   // live client animstate = the ONLY skip window
}

// C_Player::SkipsAnimationData vtbl slot 45. Force 1 while client animstate is live.
static unsigned char __fastcall Hook_PlayerSkipsAnimData(uintptr_t ent)
{
	if (bridge_anim_wire_skip.GetBool() && ent)
	{
		__try
		{
			// CRASH-FIX precedent: gate ent == localPlayer BEFORE any +0x3610 deref
			// (now inside the shared helper, which does the same ent==local check first).
			if (AnimWireSkip_IsClientAuthored(ent))
			{
				static uint32_t s_n = 0;
				if (s_n < 1)
				{
					++s_n;
					Warning(eDLL_T::CLIENT,
						"[ANIM-WIRE-SKIP] armed on bridge local player ent=%08X (SkipsAnimationData "
						"forced TRUE; native wire-anim apply + per-frame interp writer disabled; "
						"m_bPredictable=%d)\n",
						static_cast<unsigned>(ent & 0xFFFFFFFF),
						(int)*reinterpret_cast<const uint8_t*>(ent + 1876));
				}
				return 1;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	return s_origPlayerSkipsAnimData ? s_origPlayerSkipsAnimData(ent) : 0;
}

// Overlay InterpolateFieldsInternal: force m_bPredictable (ent+1876) for the call.
static __int64 __fastcall Hook_OverlayInterpFields(uintptr_t ent, float curTime, __int64 secondSnap, float secondSnapTime)
{
	uint8_t saved = 0;
	bool forced = false;
	if (bridge_anim_wire_skip.GetBool() && ent)
	{
		__try
		{
			if (AnimWireSkip_IsClientAuthored(ent))
			{
				saved = *reinterpret_cast<uint8_t*>(ent + 1876);   // m_bPredictable
				if (saved == 0)
				{
					*reinterpret_cast<uint8_t*>(ent + 1876) = 1;
					forced = true;
				}

			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	const __int64 ret = s_origOverlayInterpFields ? s_origOverlayInterpFields(ent, curTime, secondSnap, secondSnapTime) : 0;
	if (forced)
	{
		__try { *reinterpret_cast<uint8_t*>(ent + 1876) = saved; }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	return ret;
}


// PlayerRunCommand_Prediction: log FULL/LITE/SKIPPED, always call orig.
static __int64 __fastcall Hook_PlayerRunCmdPred(__int64 a1, __int64 a2, __int64 a3, __int64* a4)
{
	// Re-anchor duck timer (player+0x242C) to command K-1. command_number at cmd+0x00.
	struct DuckRemRing { uint32_t nCmd; int nRem; };
	static DuckRemRing s_duckRing[256] = {};
	uint32_t nDuckCmd = 0;
	bool bDuckArmed = false;
	if (bridge_duck_restore.GetBool() && a2 && a3)
	{
		__try {
			nDuckCmd = *reinterpret_cast<const uint32_t*>(a3);
			if (nDuckCmd > 0 && nDuckCmd < 0x40000000u)
			{
				bDuckArmed = true;
				const DuckRemRing& prev = s_duckRing[(nDuckCmd - 1) & 255];
				if (prev.nCmd == nDuckCmd - 1)
				{
					int* const pRem = reinterpret_cast<int*>(a2 + 0x242C);
					static uint32_t s_nRestores = 0, s_nRuns = 0;
					++s_nRuns;
					if (*pRem != prev.nRem)
					{
						*pRem = prev.nRem;
						++s_nRestores;
					}
					static bool s_bAnnounced = false;
					if (!s_bAnnounced && s_nRuns >= 64)
					{
						s_bAnnounced = true;
						Warning(eDLL_T::CLIENT,
							"[DUCK-RESTORE] ACTIVE -- cmd chain live (cmd=%u runs=%u restores=%u); "
							"working timer @ +0x242C re-anchored per command\n",
							nDuckCmd, s_nRuns, s_nRestores);
					}
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) { bDuckArmed = false; }
	}

	const __int64 nPredResult = s_origPlayerRunCmdPred ? s_origPlayerRunCmdPred(a1, a2, a3, a4) : 0;

	// [DUCK-RESTORE] post-cmd: record the post-execution timer for command K.
	if (bDuckArmed)
	{
		__try {
			s_duckRing[nDuckCmd & 255].nCmd = nDuckCmd;
			s_duckRing[nDuckCmd & 255].nRem = *reinterpret_cast<const int*>(a2 + 0x242C);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	return nPredResult;
}

// S3 PlayerMove case 9: pick factor/accel from buttons, then FullNoClipMove.
// S3 SetMoveType does not zero velocity; leftover |vel| is Accelerate's max(old, wish) cap.
static void NoClipSim_Run(__int64 thisptr)
{
	const uintptr_t pPlayer = *reinterpret_cast<const uintptr_t*>(thisptr + 0x08);
	const uintptr_t pMove   = *reinterpret_cast<const uintptr_t*>(thisptr + 0x10);
	if (!pPlayer || !pMove)
		return;
	if (*reinterpret_cast<const uint8_t*>(pPlayer + 0x3B2) != 9)   // m_MoveType != MOVETYPE_NOCLIP
		return;

	// frametime: gpGlobals +0x30, interval_per_tick +0x44 fallback. Tickcount at +0x40.
	float flFrametime = 0.0f;
	float flCurTime   = 0.0f;
	int   nFtSource   = 0;   // 0 = gpGlobals->frametime, 1 = interval_per_tick, 2 = literal
	const uintptr_t pGlobals = *reinterpret_cast<const uintptr_t*>(
		NetObs_Sym(NetObsSym_t::GlobalVarsPtr));
	if (pGlobals)
	{
		flCurTime   = *reinterpret_cast<const float*>(pGlobals + 0x10);
		flFrametime = *reinterpret_cast<const float*>(pGlobals + 0x30);
		if (!(flFrametime > 0.0f) || flFrametime > 0.25f)
		{
			flFrametime = *reinterpret_cast<const float*>(pGlobals + 0x44);   // interval_per_tick
			nFtSource   = 1;
		}
	}
	if (!(flFrametime > 0.0f) || flFrametime > 0.25f)
	{
		flFrametime = 0.05f;   // last-ditch: S3 server tick interval
		nFtSource   = 2;
	}
	if (nFtSource != 0)
	{
		static uint32_t s_nFtFallback = 0;
		if (++s_nFtFallback <= 4 || (s_nFtFallback % 2000) == 0)
			Warning(eDLL_T::CLIENT,
				"[NOCLIP-SIM] frametime fallback #%u (source=%s, ft=%.5f) -- the dedi "
				"integrates with its own per-command dt, so a substituted dt rescales the "
				"friction bleed and the predicted flight tops out off-speed\n",
				s_nFtFallback, nFtSource == 1 ? "interval_per_tick" : "literal 0.05",
				flFrametime);
	}

	// C_MoveData offsets are S21 client: angles 0x0C, buttons 0x24, moves 0x30/0x34/0x38, origin 0x118, velocity 0x124.

	// Factor/accel selection matches dedi case-9: buttons & 0x30000 fast; & 0x4000004 slow; else normal.
	const int nButtons = *reinterpret_cast<const int*>(pMove + 0x24);
	float flFactor, flAccel;
	int   nTier;
	if (nButtons & 0x30000)        { flFactor = bridge_noclip_speed_fast.GetFloat(); flAccel = bridge_noclip_accel_fast.GetFloat(); nTier = 2; }
	else if (nButtons & 0x4000004) { flFactor = bridge_noclip_speed_slow.GetFloat(); flAccel = bridge_noclip_accel_slow.GetFloat(); nTier = 0; }
	else                           { flFactor = bridge_noclip_speed.GetFloat();      flAccel = bridge_noclip_accel.GetFloat();      nTier = 1; }

	const float flBaseMax  = bridge_noclip_maxspeed.GetFloat();
	const float flMaxSpeed = flFactor * flBaseMax;

	// AngleVectors(mv->m_vecAbsViewAngles) -- Source convention, degrees.
	const float* pAng = reinterpret_cast<const float*>(pMove + 0x0C);
	const float flDeg2Rad = 3.14159265358979f / 180.0f;
	const float sp = sinf(pAng[0] * flDeg2Rad), cp = cosf(pAng[0] * flDeg2Rad);
	const float sy = sinf(pAng[1] * flDeg2Rad), cy = cosf(pAng[1] * flDeg2Rad);
	const float sr = sinf(pAng[2] * flDeg2Rad), cr = cosf(pAng[2] * flDeg2Rad);
	float fwd[3]   = { cp * cy, cp * sy, -sp };
	float right[3] = { -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };

	// fmove/smove are factor * normalized-move * sv_maxspeed; upmove adds factor * m_flUpMove
	// (NO maxspeed multiplier -- matches the disassembly exactly).
	const float flFwdMove  = (flFactor * *reinterpret_cast<const float*>(pMove + 0x30)) * flBaseMax;
	const float flSideMove = (flFactor * *reinterpret_cast<const float*>(pMove + 0x34)) * flBaseMax;
	const float flUpMove   =  flFactor * *reinterpret_cast<const float*>(pMove + 0x38);

	float wish[3];
	for (int i = 0; i < 3; ++i)
		wish[i] = flSideMove * right[i] + flFwdMove * fwd[i];
	wish[2] += flUpMove;

	float flWishSpeed = sqrtf(wish[0] * wish[0] + wish[1] * wish[1] + wish[2] * wish[2]);
	float wishdir[3] = { 0.0f, 0.0f, 0.0f };
	if (flWishSpeed > 1e-6f)
	{
		const float flInv = 1.0f / flWishSpeed;
		wishdir[0] = wish[0] * flInv; wishdir[1] = wish[1] * flInv; wishdir[2] = wish[2] * flInv;
	}
	if (flWishSpeed > flMaxSpeed)
	{
		const float flScale = flMaxSpeed / flWishSpeed;
		wish[0] *= flScale; wish[1] *= flScale; wish[2] *= flScale;
		flWishSpeed = flMaxSpeed;
	}

	float* vel = reinterpret_cast<float*>(pMove + 0x124);
	float* org = reinterpret_cast<float*>(pMove + 0x118);
	const float flSpdIn = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);

	// C_Player fields used here: deadflag +9516, m_hovering +12269, m_surfaceFriction +16480 (S21; do not extrapolate).
	const bool bNoAccel = *reinterpret_cast<const uint8_t*>(pPlayer + 9516) != 0
	                   || *reinterpret_cast<const uint8_t*>(pPlayer + 12269) != 0;
	float flSurfFric = bridge_noclip_surface_friction.GetFloat();
	if (flSurfFric < 0.0f)
		flSurfFric = *reinterpret_cast<const float*>(pPlayer + 16480);

	bool bIntegrate = true;
	if (flAccel <= 0.0f)
	{
		vel[0] = wish[0]; vel[1] = wish[1]; vel[2] = wish[2];
	}
	else
	{
		// Accelerate (r5 variant): accelspeed = min(addspeed, accel*frametime*surfaceFriction),
		// then |vel| clamped to max(oldspeed, wishspeed). The whole step is skipped while dead
		// or hovering -- friction and integration still run, exactly as the native does.
		const float flCurSq = vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2];
		const float flCapSq = (flCurSq > flWishSpeed * flWishSpeed) ? flCurSq : (flWishSpeed * flWishSpeed);
		const float flAdd   = flWishSpeed - (vel[0] * wishdir[0] + vel[1] * wishdir[1] + vel[2] * wishdir[2]);
		if (flAdd > 0.0f && !bNoAccel)
		{
			float flAccSpd = flAccel * flFrametime * flSurfFric;
			if (flAccSpd > flAdd) flAccSpd = flAdd;
			vel[0] += wishdir[0] * flAccSpd; vel[1] += wishdir[1] * flAccSpd; vel[2] += wishdir[2] * flAccSpd;
			const float flNewSq = vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2];
			if (flNewSq > flCapSq && flNewSq > 1e-12f)
			{
				const float flClamp = sqrtf(flCapSq / flNewSq);
				vel[0] *= flClamp; vel[1] *= flClamp; vel[2] *= flClamp;
			}
		}
		// Friction (mirrors FullNoClipMove: speed<1 -> zero velocity AND SKIP integration).
		const float flSpd = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
		if (flSpd < 1.0f)
		{
			vel[0] = 0.0f; vel[1] = 0.0f; vel[2] = 0.0f;
			bIntegrate = false;
		}
		else
		{
			const float flControl = (flSpd > flMaxSpeed * 0.25f) ? flSpd : (flMaxSpeed * 0.25f);
			const float flDrop    = flSurfFric * bridge_noclip_friction.GetFloat() * flControl * flFrametime;
			float flNew = flSpd - flDrop;
			if (flNew < 0.0f) flNew = 0.0f;
			flNew /= flSpd;
			vel[0] *= flNew; vel[1] *= flNew; vel[2] *= flNew;
		}
	}

	if (bIntegrate)
	{
		// origin += velocity * frametime, bounded to world size (UTIL_BoundToWorldSize clamps
		// each axis to +/-65535; MoveData_SetAbsOrigin itself is a plain store).
		for (int i = 0; i < 3; ++i)
		{
			float v = org[i] + flFrametime * vel[i];
			if (v >  65535.0f) v =  65535.0f;
			if (v < -65535.0f) v = -65535.0f;
			org[i] = v;
		}
	}
	if (flAccel < 0.0f)
	{
		vel[0] = 0.0f; vel[1] = 0.0f; vel[2] = 0.0f;
	}

	// Loud first fire (the stub-observability rule).
	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		Warning(eDLL_T::CLIENT,
			"[NOCLIP-SIM] FIRST FIRE -- client noclip sim ACTIVE (PlayerMove case 9). "
			"ft=%.5f(src=%d) tier=%d factor=%.2f accel=%.0f maxspeed=%.1f "
			"buttons=0x%X sf=%.2f org=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f)\n",
			flFrametime, nFtSource, nTier, flFactor, flAccel, flMaxSpeed, nButtons, flSurfFric,
			org[0], org[1], org[2], vel[0], vel[1], vel[2]);
	}

	// simRatio = integrated sim / curtime advanced. Dedi is one noclip step per usercmd.
	if (bridge_noclip_diag.GetBool())
	{
		static uint32_t s_nCalls = 0, s_nSrc[3] = { 0, 0, 0 }, s_nTier[3] = { 0, 0, 0 };
		static float    s_flDtSum = 0.0f, s_flT0 = 0.0f;
		static float    s_flFtMin = 1e9f, s_flFtMax = 0.0f;
		static float    s_flWishMax = 0.0f, s_flSpdMax = 0.0f;

		if (s_nCalls == 0)
			s_flT0 = flCurTime;
		++s_nCalls;
		++s_nSrc[nFtSource];
		++s_nTier[nTier];
		s_flDtSum += flFrametime;
		if (flFrametime < s_flFtMin) s_flFtMin = flFrametime;
		if (flFrametime > s_flFtMax) s_flFtMax = flFrametime;
		if (flWishSpeed > s_flWishMax) s_flWishMax = flWishSpeed;
		const float flSpdOut = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
		if (flSpdOut > s_flSpdMax) s_flSpdMax = flSpdOut;

		const float flElapsed = flCurTime - s_flT0;
		if (s_nCalls >= 600 || flElapsed >= 3.0f)
		{
			Warning(eDLL_T::CLIENT,
				"[NOCLIP-SIM] calls=%u over %.2fs (%.0f/s) simRatio=%.2f ft=%.5f/%.5f/%.5f "
				"src{gp=%u tick=%u lit=%u} tier{slow=%u norm=%u fast=%u} sf=%.2f wishMax=%.0f "
				"spdMax=%.0f spdIn=%.0f maxspeed=%.0f\n",
				s_nCalls, flElapsed, flElapsed > 0.0f ? s_nCalls / flElapsed : 0.0f,
				flElapsed > 0.0f ? s_flDtSum / flElapsed : 0.0f,
				s_flFtMin, s_nCalls ? s_flDtSum / s_nCalls : 0.0f, s_flFtMax,
				s_nSrc[0], s_nSrc[1], s_nSrc[2],
				s_nTier[0], s_nTier[1], s_nTier[2], flSurfFric,
				s_flWishMax, s_flSpdMax, flSpdIn, flMaxSpeed);
			s_nCalls = 0; s_nSrc[0] = s_nSrc[1] = s_nSrc[2] = 0;
			s_nTier[0] = s_nTier[1] = s_nTier[2] = 0;
			s_flDtSum = 0.0f; s_flFtMin = 1e9f; s_flFtMax = 0.0f;
			s_flWishMax = 0.0f; s_flSpdMax = 0.0f;
		}
	}
}

// [NOCLIP-SIM] after native PlayerMove (case 9 is empty on S21), inject noclip move on live C_MoveData so FinishMove consumes it.
static void __fastcall Hook_PlayerMoveClient(__int64 thisptr)
{
	if (!bridge_noclip_sim.GetBool())
	{
		if (s_origPlayerMoveClient)
			s_origPlayerMoveClient(thisptr);
		return;
	}

	__try {
		if (s_origPlayerMoveClient)
			s_origPlayerMoveClient(thisptr);
		NoClipSim_Run(thisptr);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		static uint32_t s_nFaults = 0;
		if (++s_nFaults <= 8)
			Warning(eDLL_T::CLIENT, "[NOCLIP-SIM] exception #%u in sim -- skipped this command\n", s_nFaults);
	}
}

static double __fastcall Hook_HostFrame(double a1, float a2)
{
	if (bridge_gtsfix.GetBool())
		InstallGameTimescaleFix();

	const double r = s_origHostFrame ? s_origHostFrame(a1, a2) : 0.0;
	ClockDrift_FlushPending();
	return r;
}

static void DidDmgDiag_Resolve()
{
	if (s_origPlayerDidDamageParse)
		return;
	CMemory sig = Module_FindPattern(g_GameDll,
		"48 8B C4 41 54 41 55 48 81 EC ?? ?? ?? ?? 44 8B 41 ?? 4C 8D 25 ?? ?? ?? ?? 44 8B 51 ?? "
		"F2 0F 10 1D ?? ?? ?? ?? 0F 29 70 ?? 44 0F 29 40 ?? 48 89 58");
	uint8_t* const base = sig.RCast<uint8_t*>();
	if (!base)
	{
		Warning(eDLL_T::CLIENT, "[DMG-DIAG-NATIVE] pattern NOT FOUND --"
			"signature drift, native did-damage bypass disabled.\n");
		return;
	}
	s_origPlayerDidDamageParse = reinterpret_cast<PFN_PlayerDidDamageParse>(base);
	SDK_Log("[DMG-DIAG-NATIVE] resolved parser=%p\n", (void*)base);
}

//=============================================================================
// RemoteBulletFired / Reload / WeapProjFireCB. msgArray[49] is not a bare bf_read.
//=============================================================================

//=============================================================================
static void RemoteMsgHandlers_Resolve()
{
	static bool s_remoteMsgResolved = false;
	if (s_remoteMsgResolved) return;
	s_remoteMsgResolved = true;
	CMemory sigBullet = Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC ?? "
		"81 65 ?? ?? ?? ?? ?? 4C 8D 3D ?? ?? ?? ?? 81 65");
	CMemory sigReload = Module_FindPattern(g_GameDll,
		"40 53 56 57 48 83 EC ?? 44 8B 59 ?? 45 33 D2 44 8B 49 ?? 4C 8B C1 F2 0F 10 0D ?? ?? ?? ?? "
		"48 89 6C 24 ?? BD ?? ?? ?? ?? 4C 89 74 24");
	// -- push rbp/r12/r14/r15; sub rsp,78h; mov r9d,[rcx+24h]; lea r12,...
	CMemory sigWeapProj = Module_FindPattern(g_GameDll,
		"40 55 41 54 41 56 41 57 48 83 EC ?? 44 8B 49 ?? 4C 8D 25 ?? ?? ?? ?? 44 8B 41");
	s_origRemoteBulletFired  = reinterpret_cast<PFN_RemoteBulletFired>(sigBullet.RCast<uint8_t*>());
	s_origRemoteWeaponReload = reinterpret_cast<PFN_RemoteWeaponReload>(sigReload.RCast<uint8_t*>());
	s_origWeapProjFireCB     = reinterpret_cast<PFN_WeapProjFireCB>(sigWeapProj.RCast<uint8_t*>());
	if (!s_origRemoteBulletFired)
		Warning(eDLL_T::CLIENT, "[REMOTE-MSG] RemoteBulletFired pattern NOT FOUND -- signature drift.\n");
	if (!s_origRemoteWeaponReload)
		Warning(eDLL_T::CLIENT, "[REMOTE-MSG] RemoteWeaponReload pattern NOT FOUND -- signature drift.\n");
	if (!s_origWeapProjFireCB)
		Warning(eDLL_T::CLIENT, "[REMOTE-MSG] WeapProjFireCB pattern NOT FOUND -- signature drift.\n");
	SDK_Log("[REMOTE-MSG] resolved RemoteBulletFired=%p RemoteWeaponReload=%p WeapProjFireCB=%p\n",
		(void*)s_origRemoteBulletFired, (void*)s_origRemoteWeaponReload, (void*)s_origWeapProjFireCB);
}

//=============================================================================
// CClockDriftMgr::SetServerTick. Drop dGame==0 samples; flush unfocused bursts once.
//=============================================================================
static ConVar bridge_clk_health("bridge_clk_health", "0", FCVAR_DEVELOPMENTONLY,
	"[CLK-HEALTH] Once-per-second summary of the client clock vs the server tick "
	"(lag, accepted feed rate, frame-time-scale average) plus warnings on drift, "
	"starvation, or a collapsed timescale. 0 = off.");
static ConVar bridge_clk_coalesce("bridge_clk_coalesce", "1", FCVAR_RELEASE,
	"Flush at most one SetServerTick per engine frame, using the highest tick "
	"seen that frame. Stops unfocused ServerTick bursts from poisoning the "
	"clock-drift ring. 0 = same-tick dedup only.");

static constexpr LONG kClockDriftTickNone = 0x7FFFFFFF;
static volatile LONG s_clockDriftLastTick = kClockDriftTickNone;
static volatile LONG s_clockDriftPendingTick = kClockDriftTickNone;
static volatile LONG64 s_clockDriftPendingMgr = 0;
static volatile LONG s_clockDriftDedup = 0;
static volatile LONG s_clockDriftCoalesced = 0;
static __int64 s_clockDriftPendingA3 = 0;
static __int64 s_clockDriftPendingA4 = 0;

// [CLK-HEALTH] + [ACK-REGRESS] session state -- reset on FullyConnected.
static volatile LONG s_clkHealthAccepted     = 0;
static volatile LONG s_clkHealthAcceptedWin0 = 0;
static volatile LONG s_clkHealthDedupWin0  = 0;
static LONG          s_clkHealthLastAcceptedTick = 0x7FFFFFFF;
static LARGE_INTEGER s_clkHealthLastAcceptedQpc  = {};
static LARGE_INTEGER s_clkHealthWindowStartQpc   = {};
static bool          s_clkHealthHaveWindow       = false;
static bool          s_clkHealthEverAccepted     = false;
static int           s_clkHealthTsBadWindows     = 0;
static int           s_clkHealthPrevLag          = 0;
static bool          s_clkHealthHavePrevLag      = false;
static int           s_clkHealthDriftDir         = 0;
static int           s_clkHealthDriftSameDir     = 0;
static int           s_clkHealthDriftAccum       = 0;
static LARGE_INTEGER s_clkHealthLastTsWarn       = {};
static LARGE_INTEGER s_clkHealthLastDriftWarn    = {};
static LARGE_INTEGER s_clkHealthLastStarvedWarn  = {};

volatile LONG s_tickAckLog     = 0;
bool          s_haveLastAck    = false;
int           s_lastRawAck     = 0;
volatile LONG s_ackFullReqLog  = 0;
volatile LONG s_ackInvariantLog = 0;

static void S21Bridge_ResetClockAckState(void)
{
	InterlockedExchange(&s_clockDriftLastTick, kClockDriftTickNone);
	InterlockedExchange(&s_clockDriftPendingTick, kClockDriftTickNone);
	InterlockedExchange64(&s_clockDriftPendingMgr, 0);
	InterlockedExchange(&s_clockDriftDedup, 0);
	InterlockedExchange(&s_clockDriftCoalesced, 0);
	InterlockedExchange(&s_clkHealthAccepted, 0);
	InterlockedExchange(&s_clkHealthAcceptedWin0, 0);
	InterlockedExchange(&s_clkHealthDedupWin0, 0);
	s_clkHealthLastAcceptedTick = 0x7FFFFFFF;
	s_clkHealthLastAcceptedQpc.QuadPart = 0;
	s_clkHealthWindowStartQpc.QuadPart = 0;
	s_clkHealthHaveWindow = false;
	s_clkHealthEverAccepted = false;
	s_clkHealthTsBadWindows = 0;
	s_clkHealthPrevLag = 0;
	s_clkHealthHavePrevLag = false;
	s_clkHealthDriftDir = 0;
	s_clkHealthDriftSameDir = 0;
	s_clkHealthDriftAccum = 0;
	s_clkHealthLastTsWarn.QuadPart = 0;
	s_clkHealthLastDriftWarn.QuadPart = 0;
	s_clkHealthLastStarvedWarn.QuadPart = 0;
	InterlockedExchange(&s_tickAckLog, 0);
	s_haveLastAck = false;
	s_lastRawAck = 0;
	InterlockedExchange(&s_ackFullReqLog, 0);
	InterlockedExchange(&s_ackInvariantLog, 0);
	SDK_Log("[CLK-HEALTH] clock/ack monitors re-armed for new connection\n");
}

// Offsets match CClockDriftMgr in engine/clockdriftmgr.h (+0x78 tsAvg, +0x8C srv, +0x90 cli).
static void S21Bridge_ClockHealthSample(__int64 a1)
{
	if (!bridge_clk_health.GetBool() || !a1)
		return;

	LARGE_INTEGER now = {}, freq = {};
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	if (freq.QuadPart <= 0)
		return;

	if (!s_clkHealthHaveWindow)
	{
		s_clkHealthWindowStartQpc = now;
		s_clkHealthAcceptedWin0 = s_clkHealthAccepted;
		s_clkHealthDedupWin0 = s_clockDriftDedup;
		s_clkHealthHaveWindow = true;
		return;
	}

	const double elapsed = static_cast<double>(now.QuadPart - s_clkHealthWindowStartQpc.QuadPart)
		/ static_cast<double>(freq.QuadPart);
	if (elapsed < 1.0)
		return;

	float flTsAvg = 1.0f;
	int nSrvTick = 0;
	int nCliTick = 0;
	bool bReadOk = false;
	__try
	{
		flTsAvg = *reinterpret_cast<const float*>(a1 + 0x78);
		nSrvTick = *reinterpret_cast<const int*>(a1 + 0x8C);
		nCliTick = *reinterpret_cast<const int*>(a1 + 0x90);
		bReadOk = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		bReadOk = false;
	}

	const LONG acceptedNow = s_clkHealthAccepted;
	const LONG dedupNow = s_clockDriftDedup;
	const LONG acceptedWin = acceptedNow - s_clkHealthAcceptedWin0;
	const LONG dedupWin = dedupNow - s_clkHealthDedupWin0;
	const double feedsPerSec = (elapsed > 0.0)
		? (static_cast<double>(acceptedWin) / elapsed) : 0.0;

	if (bReadOk)
	{
		const int nLag = nSrvTick - nCliTick;
		SDK_Log("[CLK-HEALTH] srvTick=%d cliTick=%d lag=%d accepted=%ld dedup=%ld "
			"coal=%ld feed/s=%.1f tsAvg=%.4f\n",
			nSrvTick, nCliTick, nLag, acceptedWin, dedupWin,
			s_clockDriftCoalesced, feedsPerSec,
			static_cast<double>(flTsAvg));

		// Timescale collapse: outside [0.90, 1.10] for 3 consecutive windows.
		const bool bTsBad = (flTsAvg < 0.90f || flTsAvg > 1.10f);
		if (bTsBad)
			++s_clkHealthTsBadWindows;
		else
			s_clkHealthTsBadWindows = 0;
		if (s_clkHealthTsBadWindows >= 3)
		{
			const double sinceWarn = (s_clkHealthLastTsWarn.QuadPart > 0)
				? (static_cast<double>(now.QuadPart - s_clkHealthLastTsWarn.QuadPart)
					/ static_cast<double>(freq.QuadPart))
				: 999.0;
			if (sinceWarn >= 5.0)
			{
				s_clkHealthLastTsWarn = now;
				Warning(eDLL_T::CLIENT,
					"[CLK-HEALTH] timescale tsAvg=%.4f outside [0.90,1.10] for %d windows\n",
					static_cast<double>(flTsAvg), s_clkHealthTsBadWindows);
			}
		}

		// Drift: lag moves same direction by >10 ticks across 5 consecutive windows.
		if (s_clkHealthHavePrevLag)
		{
			const int dLag = nLag - s_clkHealthPrevLag;
			if (dLag != 0)
			{
				const int dir = (dLag > 0) ? 1 : -1;
				if (dir == s_clkHealthDriftDir)
				{
					++s_clkHealthDriftSameDir;
					s_clkHealthDriftAccum += (dLag > 0) ? dLag : -dLag;
				}
				else
				{
					s_clkHealthDriftDir = dir;
					s_clkHealthDriftSameDir = 1;
					s_clkHealthDriftAccum = (dLag > 0) ? dLag : -dLag;
				}
				if (s_clkHealthDriftSameDir >= 5 && s_clkHealthDriftAccum > 10)
				{
					const double sinceWarn = (s_clkHealthLastDriftWarn.QuadPart > 0)
						? (static_cast<double>(now.QuadPart - s_clkHealthLastDriftWarn.QuadPart)
							/ static_cast<double>(freq.QuadPart))
						: 999.0;
					if (sinceWarn >= 5.0)
					{
						s_clkHealthLastDriftWarn = now;
						Warning(eDLL_T::CLIENT,
							"[CLK-HEALTH] drift lag moved %s by %d ticks over %d windows "
							"(lag=%d prev=%d)\n",
							(dir > 0) ? "up" : "down", s_clkHealthDriftAccum,
							s_clkHealthDriftSameDir, nLag, s_clkHealthPrevLag);
					}
				}
			}
			else
			{
				s_clkHealthDriftSameDir = 0;
				s_clkHealthDriftAccum = 0;
				s_clkHealthDriftDir = 0;
			}
		}
		s_clkHealthPrevLag = nLag;
		s_clkHealthHavePrevLag = true;
	}
	else
	{
		SDK_Log("[CLK-HEALTH] accepted=%ld dedup=%ld feed/s=%.1f (mgr read failed)\n",
			acceptedWin, dedupWin, feedsPerSec);
	}

	// Starved: no accepted feed for >1.0 s (armed after first accept).
	if (s_clkHealthEverAccepted && s_clkHealthLastAcceptedQpc.QuadPart > 0)
	{
		const double sinceAccept = static_cast<double>(
			now.QuadPart - s_clkHealthLastAcceptedQpc.QuadPart)
			/ static_cast<double>(freq.QuadPart);
		if (sinceAccept > 1.0)
		{
			const double sinceWarn = (s_clkHealthLastStarvedWarn.QuadPart > 0)
				? (static_cast<double>(now.QuadPart - s_clkHealthLastStarvedWarn.QuadPart)
					/ static_cast<double>(freq.QuadPart))
				: 999.0;
			if (sinceWarn >= 5.0)
			{
				s_clkHealthLastStarvedWarn = now;
				Warning(eDLL_T::CLIENT,
					"[CLK-HEALTH] starved lastAcceptedTick=%ld age=%.2fs\n",
					s_clkHealthLastAcceptedTick, sinceAccept);
			}
		}
	}

	s_clkHealthWindowStartQpc = now;
	s_clkHealthAcceptedWin0 = acceptedNow;
	s_clkHealthDedupWin0 = dedupNow;
}

static void ClockDrift_FlushPending(void)
{
	const LONG tick = InterlockedExchange(&s_clockDriftPendingTick, kClockDriftTickNone);
	const LONG64 mgrBits = InterlockedExchange64(&s_clockDriftPendingMgr, 0);
	if (tick == kClockDriftTickNone || !mgrBits || !s_origClockDrift)
		return;

	InterlockedIncrement(&s_clkHealthAccepted);
	s_clkHealthLastAcceptedTick = tick;
	s_clkHealthEverAccepted = true;
	if (bridge_clk_health.GetBool())
		QueryPerformanceCounter(&s_clkHealthLastAcceptedQpc);

	s_origClockDrift(mgrBits, static_cast<__int64>(static_cast<unsigned int>(tick)),
		s_clockDriftPendingA3, s_clockDriftPendingA4);
	if (bridge_clk_health.GetBool())
		S21Bridge_ClockHealthSample(mgrBits);
}

// a1 is the live CClockDriftMgr in BOTH feed paths (drain + ProcessServerTick).
static __int64 __fastcall Hook_ClockDrift(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
	const LONG tick = static_cast<LONG>(static_cast<unsigned int>(a2));
	const LONG prev = InterlockedExchange(&s_clockDriftLastTick, tick);
	if (tick == prev)
	{
		const LONG d = InterlockedIncrement(&s_clockDriftDedup);
		if (d <= 20 || (d % 1000) == 0)
			SDK_Log("[CLKDRIFT] dropped duplicate tick=%ld feed (#%ld) -- "
				"prevents dGame=0 NaN that collapses tsAvg to 0.1\n",
				tick, d);
		if (bridge_clk_health.GetBool())
			S21Bridge_ClockHealthSample(a1);
		return 0;
	}

	if (!bridge_clk_coalesce.GetBool())
	{
		InterlockedIncrement(&s_clkHealthAccepted);
		s_clkHealthLastAcceptedTick = tick;
		s_clkHealthEverAccepted = true;
		if (bridge_clk_health.GetBool())
			QueryPerformanceCounter(&s_clkHealthLastAcceptedQpc);

		const __int64 r = s_origClockDrift ? s_origClockDrift(a1, a2, a3, a4) : 0;
		if (bridge_clk_health.GetBool())
			S21Bridge_ClockHealthSample(a1);
		return r;
	}

	s_clockDriftPendingA3 = a3;
	s_clockDriftPendingA4 = a4;
	InterlockedExchange64(&s_clockDriftPendingMgr, a1);
	for (;;)
	{
		const LONG old = s_clockDriftPendingTick;
		if (old != kClockDriftTickNone && tick <= old)
			return 0;
		if (InterlockedCompareExchange(&s_clockDriftPendingTick, tick, old) != old)
			continue;
		if (old != kClockDriftTickNone)
		{
			const LONG c = InterlockedIncrement(&s_clockDriftCoalesced);
			if (c <= 20 || (c % 1000) == 0)
				Warning(eDLL_T::CLIENT,
					"[CLKDRIFT] coalesced burst n=%ld maxTick=%ld (was %ld)\n",
					c, tick, old);
		}
		return 0;
	}
}


//=============================================================================
// CViewRender::SetupSky. m_has3DSky at +0x11A375. Observability only.
//=============================================================================
static __int64 __fastcall Hook_SetupSky(uintptr_t viewRender, uintptr_t viewBundle)
{
	const __int64 result = s_origSetupSky ? s_origSetupSky(viewRender, viewBundle) : 0;
	return result;
}

//=============================================================================
// Render orchestrator. Sole gate is m_has3DSky. Observability only.
//=============================================================================
static void* __fastcall Hook_RenderOrchestrator(
	uintptr_t viewRender, uintptr_t a2, uintptr_t a3,
	uintptr_t a4_renderViewSetup, uintptr_t a5_flagByte,
	uintptr_t a6_renderList, uintptr_t a7, uintptr_t a8_outEntity)
{
	if (s_origRenderOrchestrator) {
		return s_origRenderOrchestrator(
			viewRender, a2, a3, a4_renderViewSetup, a5_flagByte,
			a6_renderList, a7, a8_outEntity);
	}
	return nullptr;
}

//=============================================================================
// CL_ProcessSnapshot. Threaded gate: signon>=6 && bIsDelta && net_threadedProcessPacket.
//=============================================================================
static char __fastcall Hook_ClProcessSnapshotDispatch(__int64 a1)
{
	const char r = s_origClProcessSnapshotDispatch ? s_origClProcessSnapshotDispatch(a1) : 0;
	return r;
}

//=============================================================================
// ClSendTick: kill hitch smoother; overflow goes to skip-carry.
//=============================================================================
static ConVar bridge_clk_unfocus("bridge_clk_unfocus", "1", FCVAR_RELEASE,
	"While the game window is not foreground: force cl_checkForFrametimeHitch 0 "
	"and carry usercmd_frametime_max overflow into the next command. "
	"Stops alt-tab from discarding predicted time.");

// The engine hitch smoother substitutes the 128-frame ring average for any
// frame whose dt exceeds avg * cl_frametime_hitch_threshold and discards the
// difference. Under spiky-but-fast frame pacing that under-supplies usercmd
// time every few frames: the world sims below real time at a normal frame
// rate. The dedi already clamps per-command supply, so the client never needs
// to discard.
static ConVar bridge_clk_nohitch("bridge_clk_nohitch", "0", FCVAR_RELEASE,
	"[CLKDRIFT] Never discard predicted frame time: keep cl_checkForFrametimeHitch "
	"forced 0 and carry usercmd_frametime_max overflow into the next command "
	"regardless of window focus. 0 = stock hitch smoother while focused (default).");

// Every entry in m_serverFrameTimeScales is wallSecondsElapsed / serverSecondsAdvanced,
// sampled in CClockDriftMgr::SetServerTick off each incoming net_Tick. The trimmed mean
// of the 24 becomes m_serverFrameTimeScaleAverage, clamped to [0.1, 1.0], and that
// multiplies host_frametime every frame -- so a server clock that advances slower than
// wall time throttles the whole client world by the same ratio.
static ConVar bridge_clk_probe("bridge_clk_probe", "0", FCVAR_DEVELOPMENTONLY,
	"Dump CClockDriftMgr once per second: the scale ring, its average, the "
	"ahead-drain pair and the server/client tick pair.");

static ConVar bridge_snap_pair_diag("bridge_snap_pair_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"[SNAP-PAIR] Once-per-second spacing of the snapshot pair the world lerps "
	"between: how many frames had no future snapshot, how many extrapolated past "
	"it, and the dt every per-snapshot derivative divides by. 0 = off.");

static void ClSendTick_DumpClockDrift(void)
{
	ClockDriftView_t drift;
	bool bRead = false;
	float flHostFt = -1.0f, flClFt = -1.0f, flInterval = -1.0f, flClientTime = -1.0f;

	__try
	{
		const ClockDriftView_t* const pDrift = NetObs_ClockDrift();
		if (pDrift)
		{
			memcpy(&drift, pDrift, sizeof(drift));
			flHostFt     = *reinterpret_cast<float*>(NetObs_HostFrameTimeAddr());
			flClFt       = *reinterpret_cast<float*>(NetObs_FrameTimeAddr());
			flInterval   = *reinterpret_cast<float*>(NetObs_IntervalPerTickAddr());
			flClientTime = *reinterpret_cast<float*>(NetObs_ClientStateBaseAddr() + 0xC0);
			bRead = true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { bRead = false; }

	if (!bRead)
	{
		Warning(eDLL_T::CLIENT, "[CLK-DRIFT] clock drift block unreadable\n");
		return;
	}

	// Host_AccumulateTime takes host_framerate as a fixed step (1/x above 1)
	// when sv_cheats is on, with no floor and no fps dependence.
	static ConVar* s_pHostFramerate = nullptr;
	static ConVar* s_pSvCheats = nullptr;
	if (!s_pHostFramerate && g_pCVar) s_pHostFramerate = g_pCVar->FindVar("host_framerate");
	if (!s_pSvCheats && g_pCVar) s_pSvCheats = g_pCVar->FindVar("sv_cheats");
	static ConVar* s_pHostTimescale = nullptr;
	if (!s_pHostTimescale && g_pCVar) s_pHostTimescale = g_pCVar->FindVar("host_timescale");
	const float flHostFr = s_pHostFramerate ? s_pHostFramerate->GetFloat() : -1.0f;
	const float flHostTs = s_pHostTimescale ? s_pHostTimescale->GetFloat() : -1.0f;
	const int nCheats = s_pSvCheats ? s_pSvCheats->GetInt() : -1;

	// Host_AccumulateTime's two branches leave different tracks: the clamp
	// path stores the raw dt in host_frametime_unbounded; the scale path stores
	// scale*dt there and the clamped raw dt in the sibling cell.
	// When the local server state is 2 or higher the engine takes the
	// timescale from the local server object instead of the client getter.
	float flUnbounded = -1.0f, flClampedRaw = -1.0f, flSvTs = -1.0f;
	int nSvState = -1;
	__try
	{
		flUnbounded  = *reinterpret_cast<float*>(NetObs_Sym(NetObsSym_t::HostFrameTimeUnbounded));
		flClampedRaw = *reinterpret_cast<float*>(NetObs_Sym(NetObsSym_t::HostFrameTimeClamped));
		nSvState     = *reinterpret_cast<int*>(NetObs_Sym(NetObsSym_t::ServerStateInt));
		void* const pSv = *reinterpret_cast<void**>(NetObs_Sym(NetObsSym_t::ServerObjectPtr));
		if (pSv && nSvState >= 2)
		{
			typedef float (__fastcall *PFN_SvTimescale)(void*);
			flSvTs = (*reinterpret_cast<PFN_SvTimescale*>(*reinterpret_cast<char**>(pSv) + 0x1C8))(pSv);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	// The main-loop pacers: host_sleep and not_focus_sleep put a Sleep in every
	// host frame; fps_max (or fps_max_vsync/use_refresh) is what should hold it.
	static const char* const s_pacerNames[] = {
		"host_sleep", "not_focus_sleep", "fps_max", "fps_max_vsync",
		"fps_max_use_refresh", "sleep_when_meeting_framerate", "fps_max_rt" };
	static ConVar* s_pPacers[7] = {};
	char szPacers[192]; int nPacerLen = 0;
	for (int i = 0; i < 7; ++i)
	{
		if (!s_pPacers[i] && g_pCVar) s_pPacers[i] = g_pCVar->FindVar(s_pacerNames[i]);
		const int n = snprintf(szPacers + nPacerLen, sizeof(szPacers) - nPacerLen, " %s=%s",
			s_pacerNames[i], s_pPacers[i] ? s_pPacers[i]->GetString() : "?");
		if (n < 0 || n >= static_cast<int>(sizeof(szPacers)) - nPacerLen) break;
		nPacerLen += n;
	}

	Warning(eDLL_T::CLIENT,
		"[CLK-DRIFT] hostFt=%.5f clFt=%.5f unb=%.5f rawClamp=%.5f svState=%d svTs=%.4f hostFr=%.4f hostTs=%.4f cheats=%d "
		"tsAvg=%.4f ahead=%.4f within=%.4f "
		"svTick=%d clTick=%d lastSvTime=%.3f clTime=%.3f interval=%.5f "
		"off=%.4f/%.4f/%.4f/%.4f |%s\n",
		static_cast<double>(flHostFt), static_cast<double>(flClFt),
		static_cast<double>(flUnbounded), static_cast<double>(flClampedRaw),
		nSvState, static_cast<double>(flSvTs),
		static_cast<double>(flHostFr), static_cast<double>(flHostTs), nCheats,
		static_cast<double>(drift.m_serverFrameTimeScaleAverage),
		static_cast<double>(drift.m_aheadBy), static_cast<double>(drift.m_correctWithin),
		drift.m_nServerTick, drift.m_nClientTick,
		static_cast<double>(drift.m_lastServerTime), static_cast<double>(flClientTime),
		static_cast<double>(flInterval),
		static_cast<double>(drift.m_ClockOffsets[0]), static_cast<double>(drift.m_ClockOffsets[1]),
		static_cast<double>(drift.m_ClockOffsets[2]), static_cast<double>(drift.m_ClockOffsets[3]),
		szPacers);

	char szRing[24 * 12 + 1];
	int nUsed = 0;
	for (int i = 0; i < 24; ++i)
	{
		const int nLeft = static_cast<int>(sizeof(szRing)) - nUsed;
		if (nLeft <= 1)
			break;
		const int nWrote = snprintf(szRing + nUsed, nLeft, "%.2f ",
			static_cast<double>(drift.m_serverFrameTimeScales[i]));
		if (nWrote <= 0)
			break;
		nUsed += (nWrote < nLeft) ? nWrote : (nLeft - 1);
	}
	szRing[sizeof(szRing) - 1] = '\0';
	Warning(eDLL_T::CLIENT, "[CLK-DRIFT] scales idx=%d: %s\n",
		drift.m_serverFrameTimeScaleIndex, szRing);
}

static bool ClSendTick_WindowUnfocused(void)
{
	if (!g_pGame)
		return false;
	const HWND hGame = g_pGame->GetWindow();
	if (!hGame)
		return false;
	const HWND hFg = GetForegroundWindow();
	return hFg && hFg != hGame;
}

static void __fastcall Hook_ClSendTick()
{
	const LONG n = InterlockedIncrement(&s_clSendTickCalls);
	const bool wantLog = (n <= 120 || (n % 200) == 0);

	int       signon = -1, outSeq0 = -1, cvarV = -2, fixedStep = -1, inGame = -1;
	uintptr_t nchan = 0;
	float     interval = -1.0f, v12 = -1.0f, accum0 = 0.0f, frametime = 0.0f;
	double    now = 0.0, lastClock = 0.0, fixedClock = 0.0;

	float trueDt = 0.0f;
	__try
	{
		trueDt = *reinterpret_cast<float*>(NetObs_FrameTimeAddr());
		accum0 = *reinterpret_cast<float*>(NetObs_SendAccumAddr());
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	ConVar* pHitch = nullptr;
	int nHitchRestore = -1;
	const bool bNoHitch = bridge_clk_nohitch.GetBool();
	const bool bUnfocus = bridge_clk_unfocus.GetBool() && ClSendTick_WindowUnfocused();
	if ((bNoHitch || bUnfocus) && g_pCVar)
	{
		pHitch = g_pCVar->FindVar("cl_checkForFrametimeHitch");
		if (pHitch && pHitch->GetInt() != 0)
		{
			nHitchRestore = pHitch->GetInt();
			pHitch->SetValue(0);
			static volatile LONG s_nHitchOff = 0;
			if (InterlockedIncrement(&s_nHitchOff) == 1)
				Warning(eDLL_T::CLIENT,
					"[CLKDRIFT] cl_checkForFrametimeHitch forced 0 (%s)\n",
					bNoHitch ? "bridge_clk_nohitch" : "unfocused");
		}
	}

	if (wantLog)
	{
		__try
		{
			const uintptr_t b = NetObs_GetExeModuleBase();
			if (b)
			{
				signon     = *reinterpret_cast<int*>(NetObs_ClientStateSignonAddr());
				now        = *reinterpret_cast<double*>(NetObs_NetTimeAddr());
				nchan      = *reinterpret_cast<uintptr_t*>(NetObs_NetChanAddr());
				outSeq0    = nchan ? *reinterpret_cast<int*>(nchan + 4) : -1;
				frametime  = trueDt;
				fixedStep  = *reinterpret_cast<unsigned char*>(NetObs_FixedStepFlagAddr());
				inGame     = *reinterpret_cast<unsigned char*>(NetObs_InGameFlagAddr());
				lastClock  = *reinterpret_cast<double*>(NetObs_SendLastClockAddr());
				fixedClock = *reinterpret_cast<double*>(NetObs_FixedClockAddr());
				const uintptr_t s1 = *reinterpret_cast<uintptr_t*>(NetObs_SendIntervalPrimaryPtrAddr());
				interval   = s1 ? *reinterpret_cast<float*>(s1 + 0x60) : -1.0f;
				const uintptr_t s2 = *reinterpret_cast<uintptr_t*>(NetObs_SendIntervalSecondaryPtrAddr());
				v12        = s2 ? *reinterpret_cast<float*>(s2 + 0x60) : -1.0f;
				const uintptr_t cv = *reinterpret_cast<uintptr_t*>(NetObs_CvarGatePtrAddr());
				cvarV      = cv ? *reinterpret_cast<int*>(cv + 0x64) : -2;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	if (s_origClSendTick)
		s_origClSendTick();

	// Restore only for the unfocused-only mode; nohitch keeps the smoother off.
	if (!bNoHitch && nHitchRestore >= 0 && pHitch)
		pHitch->SetValue(nHitchRestore);

	if ((bNoHitch || bUnfocus) && trueDt > 0.0f)
	{
		float flMax = 0.100f;
		if (g_pCVar)
		{
			ConVar* const pMax = g_pCVar->FindVar("usercmd_frametime_max");
			if (pMax && pMax->GetFloat() > 0.0f)
				flMax = pMax->GetFloat();
		}
		if (trueDt > flMax)
		{
			float flCarry = trueDt - flMax;
			if (flCarry > 0.25f)
				flCarry = 0.25f;
			__try
			{
				float* const pAccum = reinterpret_cast<float*>(NetObs_SendAccumAddr());
				if (pAccum && *pAccum < flCarry)
					*pAccum = flCarry;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}

			static volatile LONG s_nCarry = 0;
			const LONG c = InterlockedIncrement(&s_nCarry);
			if (c <= 20 || (c % 200) == 0)
				Warning(eDLL_T::CLIENT,
					"[CLKDRIFT] carried %.3fs discarded usercmd dt (true=%.3f max=%.3f) #%ld\n",
					flCarry, trueDt, flMax, c);
		}
	}

	// [SNAP-PAIR] the two snapshots the world is interpolated between. Every
	// per-snapshot derivative the client takes -- remote-player velocity, and so
	// the aim-assist magnet and dampen release -- divides by fut-cur, so a
	// collapsed pair zeroes all of them at once. lerp > 1 means the client is
	// extrapolating past the future snapshot.
	if (bridge_snap_pair_diag.GetBool())
	{
		static double s_dPairWinStart = 0.0;
		static int    s_nPairFrames = 0;
		static int    s_nPairZero = 0;
		static int    s_nPairOver = 0;
		static float  s_flPairDtMin = 0.0f;
		static float  s_flPairDtMax = 0.0f;
		static float  s_flPairLerpMax = 0.0f;

		float flLast = 0.0f, flCur = 0.0f, flFut = 0.0f, flLerp = 0.0f;
		if (PredNative_SnapTimes(&flLast, &flCur, &flFut, &flLerp))
		{
			const float flDt = flFut - flCur;
			const double dNow = Plat_FloatTime();
			if (s_dPairWinStart == 0.0)
			{
				s_dPairWinStart = dNow;
				s_flPairDtMin = flDt;
				s_flPairDtMax = flDt;
			}
			++s_nPairFrames;
			if (flDt <= 0.0f)
				++s_nPairZero;
			if (flLerp > 1.0f)
				++s_nPairOver;
			if (flDt < s_flPairDtMin) s_flPairDtMin = flDt;
			if (flDt > s_flPairDtMax) s_flPairDtMax = flDt;
			if (flLerp > s_flPairLerpMax) s_flPairLerpMax = flLerp;

			if (dNow - s_dPairWinStart >= 1.0)
			{
				Warning(eDLL_T::CLIENT,
					"[SNAP-PAIR] frames=%d collapsed=%d extrapolating=%d "
					"dt{min=%.4f max=%.4f} lerpMax=%.2f last=%.3f cur=%.3f fut=%.3f\n",
					s_nPairFrames, s_nPairZero, s_nPairOver,
					static_cast<double>(s_flPairDtMin),
					static_cast<double>(s_flPairDtMax),
					static_cast<double>(s_flPairLerpMax),
					static_cast<double>(flLast), static_cast<double>(flCur),
					static_cast<double>(flFut));
				s_dPairWinStart = dNow;
				s_nPairFrames = 0;
				s_nPairZero = 0;
				s_nPairOver = 0;
				s_flPairDtMin = flDt;
				s_flPairDtMax = flDt;
				s_flPairLerpMax = flLerp;
			}
		}
	}

	// [CMD-RATE] once-per-second usercmd supply. Loud on its own only for the
	// slow-motion signature (focused, frame rate healthy, cmd rate collapsed);
	// full line every second under bridge_clk_health.
	{
		static double s_dWinStart = 0.0;
		static int    s_nWinFrames = 0;
		static int    s_nWinCmd0 = INT_MIN;

		int nCmdNow = INT_MIN;
		__try { nCmdNow = *reinterpret_cast<int*>(NetObs_CmdNumberAddr()); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}

		const double dNow = Plat_FloatTime();
		if (s_dWinStart == 0.0)
		{
			s_dWinStart = dNow;
			s_nWinCmd0 = nCmdNow;
		}
		++s_nWinFrames;

		// A non-zero host_framerate replaces the host frame dt outright.
		{
			static ConVar* s_pHostFramerateWatch = nullptr;
			static float   s_flLastHostFr = 0.0f;
			if (!s_pHostFramerateWatch && g_pCVar)
				s_pHostFramerateWatch = g_pCVar->FindVar("host_framerate");
			const float flHostFr = s_pHostFramerateWatch ? s_pHostFramerateWatch->GetFloat() : 0.0f;
			if (flHostFr != s_flLastHostFr)
			{
				Warning(eDLL_T::CLIENT, "[CMD-RATE] host_framerate %.4f -> %.4f (cmd=%d)\n",
					static_cast<double>(s_flLastHostFr), static_cast<double>(flHostFr), nCmdNow);
				s_flLastHostFr = flHostFr;
			}
		}

		const double dWin = dNow - s_dWinStart;
		if (dWin >= 1.0)
		{
			const double dFps = s_nWinFrames / dWin;
			const double dCmdRate = (nCmdNow != INT_MIN && s_nWinCmd0 != INT_MIN && nCmdNow >= s_nWinCmd0)
				? (nCmdNow - s_nWinCmd0) / dWin : -1.0;
			const bool bFocused = !ClSendTick_WindowUnfocused();

			// Slow motion latches itself: a focused window whose command rate has
			// fallen well under the frame rate is the signature, and the clock block
			// is dumped on the spot so a random occurrence is never missed. The
			// 300 cmd/s ceiling keeps the stock 350/s gate from reading as a collapse
			// at very high frame rates.
			const bool bLow = bFocused && dCmdRate >= 0.0 && dCmdRate < 300.0
				&& dCmdRate < 0.60 * dFps && dFps > 40.0;

			if (bridge_clk_health.GetBool())
			{
				// cl.m_frameTime is what the send loop accumulates against
				// usercmd_frametime_min, and it is AdjustFrameTime(host_frametime).
				// Printing host_frametime beside it separates a dead host clock from
				// a CClockDriftMgr that is scaling the world down.
				float flHostFt = -1.0f, flClFt = -1.0f, flScale = -1.0f;
				float flAhead = -1.0f, flWithin = -1.0f;
				__try
				{
					flHostFt = *reinterpret_cast<float*>(NetObs_HostFrameTimeAddr());
					flClFt   = *reinterpret_cast<float*>(NetObs_FrameTimeAddr());
					const ClockDriftView_t* const pDrift = NetObs_ClockDrift();
					if (pDrift)
					{
						flScale  = pDrift->m_serverFrameTimeScaleAverage;
						flAhead  = pDrift->m_aheadBy;
						flWithin = pDrift->m_correctWithin;
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}

				Warning(eDLL_T::CLIENT,
					"[CMD-RATE] fps=%.0f cmd/s=%.0f hostFt=%.5f clFt=%.5f wallFt=%.5f "
					"tsAvg=%.3f ahead=%.4f within=%.4f focus=%d nohitch=%d\n",
					dFps, dCmdRate, static_cast<double>(flHostFt),
					static_cast<double>(flClFt),
					(dFps > 0.0) ? (1.0 / dFps) : -1.0,
					static_cast<double>(flScale), static_cast<double>(flAhead),
					static_cast<double>(flWithin),
					bFocused ? 1 : 0, bNoHitch ? 1 : 0);
			}
			else if (bLow)
			{
				static volatile LONG s_nLowLogs = 0;
				const LONG l = InterlockedIncrement(&s_nLowLogs);
				if (l <= 30 || (l % 60) == 0)
					Warning(eDLL_T::CLIENT,
						"[CMD-RATE] LOW usercmd supply: fps=%.0f but cmd/s=%.0f "
						"(focused; world will sim below real time) #%ld\n",
						dFps, dCmdRate, l);
			}

			if (bridge_clk_probe.GetBool())
				ClSendTick_DumpClockDrift();
			else if (bLow)
			{
				static volatile LONG s_nLowDumps = 0;
				const LONG d = InterlockedIncrement(&s_nLowDumps);
				if (d <= 20 || (d % 30) == 0)
					ClSendTick_DumpClockDrift();
			}

			s_dWinStart = dNow;
			s_nWinFrames = 0;
			s_nWinCmd0 = nCmdNow;
		}
	}

	if (wantLog)
	{
		__try
		{
			const uintptr_t b = NetObs_GetExeModuleBase();
			int    outSeq1  = nchan ? *reinterpret_cast<int*>(nchan + 4) : -1;
			float  accum1   = 0.0f;
			double nextSend = 0.0;
			if (b)
			{
				accum1   = *reinterpret_cast<float*>(NetObs_SendAccumAddr());
				nextSend = *reinterpret_cast<double*>(NetObs_ClientStateNextSendAddr());
			}
			SDK_Log("[SENDTICK] #%ld signon=%d now=%.4f | interval=%.6f v12=%.6f "
				"cvar=%d inGame=%d fixedStep=%d | accum %.6f->%.6f frametime=%.6f "
				"lastClk=%.4f fixedClk=%.4f nextSend=%.4f | outSeq %d->%d%s\n",
				n, signon, now, interval, v12, cvarV, inGame, fixedStep,
				accum0, accum1, frametime, lastClock, fixedClock, nextSend,
				outSeq0, outSeq1,
				(outSeq1 != outSeq0) ? "  *** TRANSMITTED ***" : "");
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
}

// Slot -> classID map captured by Hook_CL_CopyNewEntity. Stored as
// (classID + 1) so 0 = "unset" while still allowing classID 0. Diagnostic-
// only now: lets per-slot logs show which class each slot's entity is.
int s_slotClassID[16384] = {};

// Cache TransitionEntity a5 keyed by slot.
static uintptr_t s_entityPtrBySlot[16384] = {};


static uintptr_t Bridge_GetEntityBySlot(unsigned int slot)
{
	uintptr_t entPtr = 0;
	__try
	{
		const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(NetObs_EntityManagerAddr());
		if (mgr)
		{
			void** vt = *reinterpret_cast<void***>(mgr);
			typedef uintptr_t (__fastcall *GetEnt)(uintptr_t, unsigned int);
			entPtr = reinterpret_cast<GetEnt>(vt[1])(mgr, slot);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		entPtr = 0;
	}
	return entPtr;
}

// Set while PropApplyLoop applies player slot 1 (packed-read leaf scope).
volatile long g_pbReadActive = 0;

static void Bridge_PromoteScriptNameToTargetName(__int64 entity);

// PropApplyLoop a5 is parentBase, not the entity. Entity is a5-0x18.
static constexpr ptrdiff_t PROPAPPLY_ENTITY_ADJ = 0x18;

static bool Bridge_NameLooksLive(const uint8_t* p)
{
	if (!p)
		return false;
	unsigned char c = 0;
	__try { c = p[0x481]; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return c >= 0x20 && c < 0x7F;
}

static int Bridge_ReadScriptIdx(const uint8_t* p)
{
	int idx = 0;
	if (!p)
		return 0;
	__try { idx = *reinterpret_cast<const int*>(p + 0x588); }
	__except (EXCEPTION_EXECUTE_HANDLER) { idx = 0; }
	return idx;
}

static const char* Bridge_ReadName(const uint8_t* p)
{
	if (!p || !Bridge_NameLooksLive(p))
		return "";
	return reinterpret_cast<const char*>(p + 0x481);
}

static uint8_t* Bridge_ScriptMoverEntity(uint8_t* a5)
{
	if (!a5 || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(a5)))
		return nullptr;
	uint8_t* const adj = a5 - PROPAPPLY_ENTITY_ADJ;
	if (Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(adj)))
		return adj;
	return a5;
}

static uint8_t s_scriptMoverCreateNotified[16384] = {};
static const char s_scriptMoverSignifier[] = "script_mover";

static int*    s_pNotifyScriptEntsCount = nullptr;

static ConVar bridge_script_create_diag("bridge_script_create_diag", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Bridge [SCRIPT-CREATE]: dump g_NotifyScriptEnts at every "
	"C_BaseEntity::NotifyScriptOfNewEntities and log the m_iSignifierName gate result.");

static ConVar bridge_deathfield_probe("bridge_deathfield_probe", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Bridge [DEATHFIELD-CL]: log the ring index the client asks for and the C_World ring "
	"arrays it answers from.");

static const uint8_t* Bridge_EntInfoArray(void)
{
	static const uint8_t* s_pArray = nullptr;
	static bool s_tried = false;
	if (!s_tried)
	{
		s_tried = true;
		s_pArray = Module_FindPattern(g_GameDll,
			"4C 8B 79 10 4C 8D 2D ?? ?? ?? ?? 4C 8B 61 18 48 8B D9 49 63 C0 4C 8D 04 C0")
			.ResolveRelativeAddress(7, 11)
			.RCast<const uint8_t*>();
		if (!s_pArray)
			Warning(eDLL_T::ENGINE,
				"[DEATHFIELD] EntInfo array pattern unresolved\n");
	}
	return s_pArray;
}

static uint32_t Bridge_BindScriptMoverHandle(unsigned slot, uint8_t* ent)
{
	if (!ent || slot >= 16384)
		return 0xFFFFFFFFu;
	uint32_t handle = 0xFFFFFFFFu;
	__try { handle = *reinterpret_cast<uint32_t*>(ent + 8); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
	if (handle != 0xFFFFFFFFu)
		return handle;
	const uint8_t* const arr = Bridge_EntInfoArray();
	if (!arr)
		return 0xFFFFFFFFu;
	uint8_t* const info = const_cast<uint8_t*>(arr) + static_cast<size_t>(slot) * 0x20;
	if (!Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(info)))
		return 0xFFFFFFFFu;
	void* listed = nullptr;
	int serial = 0;
	__try
	{
		listed = *reinterpret_cast<void**>(info);
		serial = *reinterpret_cast<int*>(info + 8);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
	if (serial <= 0)
		serial = 1;
	handle = (static_cast<uint32_t>(serial) << 16) | slot;
	__try
	{
		*reinterpret_cast<uint32_t*>(ent + 8) = handle;
		if (!listed)
			*reinterpret_cast<void**>(info) = ent;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
	return handle;
}

static bool Bridge_SigLooksLive(const char* sig)
{
	if (!sig || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(sig)))
		return false;
	const unsigned char c = static_cast<unsigned char>(sig[0]);
	return c >= 0x20 && c < 0x7F;
}

static void Bridge_EnsureScriptCreateNotify(unsigned slot, uint8_t* ent)
{
	if (slot >= 16384 || !ent || s_scriptMoverCreateNotified[slot])
		return;
	if (!Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(ent)))
		return;

	const uint32_t handle = Bridge_BindScriptMoverHandle(slot, ent);
	if (handle == 0xFFFFFFFFu)
		return;

	__try
	{
		const char** const ppSig = reinterpret_cast<const char**>(ent + 0x478);
		if (!Bridge_SigLooksLive(*ppSig))
			*ppSig = s_scriptMoverSignifier;

		int notifyIdx = *reinterpret_cast<int*>(ent + 0x92C);
		if (notifyIdx < 0 && s_pFakeRecreate)
		{
			*reinterpret_cast<uint8_t*>(ent + 0x691) = 1;
			s_pFakeRecreate(ent + 0x18);
			notifyIdx = *reinterpret_cast<int*>(ent + 0x92C);
		}

		if (notifyIdx >= 0)
			s_scriptMoverCreateNotified[slot] = 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return;
	}
}

static void __fastcall Hook_NotifyScriptOfNewEntities(void)
{
	if (bridge_script_create_diag.GetBool()
		&& s_ppNotifyScriptEnts && s_pNotifyScriptEntsCount && s_pAcceptEntsForScript)
	{
		static volatile LONG s_diagLines = 0;
		__try
		{
			void** arr = *s_ppNotifyScriptEnts;
			int n = *s_pNotifyScriptEntsCount;
			if (n < 0)
				n = 0;
			if (n > 4096)
				n = 4096;
			if (arr && Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(arr)))
			{
				for (int i = 0; i < n; ++i)
				{
					if (InterlockedIncrement(&s_diagLines) > 200)
						break;
					uint8_t* ent = reinterpret_cast<uint8_t*>(arr[i]);
					if (!ent || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(ent)))
					{
						Warning(eDLL_T::ENGINE,
							"[SCRIPT-CREATE] queue[%d/%d] ent=%p notifyIdx=%d sig='' name='' local=(0,0,0) abs=(0,0,0)\n",
							i, n, ent, -1);
						continue;
					}
					int notifyIdx = -1;
					const char* sig = "";
					const char* name = "";
					float localX = 0.f, localY = 0.f, localZ = 0.f;
					float absX = 0.f, absY = 0.f, absZ = 0.f;
					__try
					{
						notifyIdx = *reinterpret_cast<int*>(ent + 0x92C);
						const char* const rawSig = *reinterpret_cast<const char* const*>(ent + 0x478);
						if (Bridge_SigLooksLive(rawSig))
							sig = rawSig;
						name = Bridge_ReadName(ent);
						const float* const loc = reinterpret_cast<const float*>(ent + 0x188);
						localX = loc[0]; localY = loc[1]; localZ = loc[2];
						const float* const absO = reinterpret_cast<const float*>(ent + 0x17C);
						absX = absO[0]; absY = absO[1]; absZ = absO[2];
					}
					__except (EXCEPTION_EXECUTE_HANDLER)
					{
					}
					Warning(eDLL_T::ENGINE,
						"[SCRIPT-CREATE] queue[%d/%d] ent=%p notifyIdx=%d sig='%s' name='%s' "
						"local=(%.0f,%.0f,%.0f) abs=(%.0f,%.0f,%.0f)\n",
						i, n, ent, notifyIdx, sig, name,
						localX, localY, localZ, absX, absY, absZ);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	if (s_pNotifyScriptOfNewEntities)
		s_pNotifyScriptOfNewEntities();
}

static float __fastcall Hook_DeathFieldRadiusForTime(float flTime, int nRingIndex)
{
	const float ret = s_pDeathFieldRadiusForTime
		? s_pDeathFieldRadiusForTime(flTime, nRingIndex)
		: 0.f;

	if (!bridge_deathfield_probe.GetBool())
		return ret;

	static volatile LONG s_probeN = 0;
	const LONG n = InterlockedIncrement(&s_probeN);
	if (n > 20 && (n % 600) != 0)
		return ret;

	uint8_t* world = nullptr;
	unsigned active = 0;
	float rStart = 0.f, rEnd = 0.f;
	float tStart = 0.f, tEnd = 0.f;
	float ox = 0.f, oy = 0.f, oz = 0.f;
	const bool idxOk = (nRingIndex >= 0 && nRingIndex < 64);

	if (s_ppClientWorld && Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(s_ppClientWorld)))
	{
		__try { world = *s_ppClientWorld; }
		__except (EXCEPTION_EXECUTE_HANDLER) { world = nullptr; }
	}

	if (!world || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(world)))
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-CL] idx=%d time=%.1f ret=%.1f active=0 r=0->0 t=0.0->0.0 origin=(0,0,0) world=(null)\n",
			nRingIndex, flTime, ret);
		return ret;
	}

	if (idxOk)
	{
		__try
		{
			active = world[0x9B0 + nRingIndex];
			const float* const org = reinterpret_cast<const float*>(world + 0x9F0 + 12 * nRingIndex);
			ox = org[0]; oy = org[1]; oz = org[2];
			rStart = *reinterpret_cast<const float*>(world + 0xCF0 + 4 * nRingIndex);
			rEnd   = *reinterpret_cast<const float*>(world + 0xDF0 + 4 * nRingIndex);
			tStart = *reinterpret_cast<const float*>(world + 0xEF0 + 4 * nRingIndex);
			tEnd   = *reinterpret_cast<const float*>(world + 0xFF0 + 4 * nRingIndex);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	Warning(eDLL_T::ENGINE,
		"[DEATHFIELD-CL] idx=%d time=%.1f ret=%.1f active=%u r=%.0f->%.0f t=%.1f->%.1f origin=(%.0f,%.0f,%.0f) world=%p\n",
		nRingIndex, flTime, ret, active, rStart, rEnd, tStart, tEnd, ox, oy, oz, world);
	return ret;
}

static void Bridge_AfterEntityApply(unsigned slot, __int64* a5)
{
	if (slot >= 16384 || !a5)
		return;
	if (!Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(a5)))
		return;
	if (s_entityPtrBySlot[slot] != (uintptr_t)a5)
		s_scriptMoverCreateNotified[slot] = 0;
	s_entityPtrBySlot[slot] = (uintptr_t)a5;
	if (s_slotClassID[slot] <= 0)
		return;
	const char* const cn = DT_ClassName(s_slotClassID[slot] - 1);
	if (!cn || strcmp(cn, "CScriptMover") != 0)
		return;
	uint8_t* const raw = reinterpret_cast<uint8_t*>(a5);
	uint8_t* const ent = Bridge_ScriptMoverEntity(raw);
	if (!ent)
		return;
	Bridge_PromoteScriptNameToTargetName(reinterpret_cast<__int64>(ent));

	int notifyIdxPre = -1;
	const char* sigPre = "";
	float localX = 0.f, localY = 0.f, localZ = 0.f;
	__try
	{
		notifyIdxPre = *reinterpret_cast<int*>(ent + 0x92C);
		const char* const rawSig = *reinterpret_cast<const char* const*>(ent + 0x478);
		if (Bridge_SigLooksLive(rawSig))
			sigPre = rawSig;
		const float* const loc = reinterpret_cast<const float*>(ent + 0x188);
		localX = loc[0]; localY = loc[1]; localZ = loc[2];
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}

	Bridge_EnsureScriptCreateNotify(slot, ent);
	static unsigned s_logSlot = ~0u;
	static int s_logN = 0;
	if (s_logSlot != slot)
	{
		s_logSlot = slot;
		s_logN = 0;
	}
	if (!bridge_deathfield_probe.GetBool() || s_logN >= 2)
		return;
	++s_logN;
	uint8_t* const adj = raw - 0x18;
	__try
	{
		const char* const sig = *reinterpret_cast<const char* const*>(ent + 0x478);
		const uint32_t eh = *reinterpret_cast<const uint32_t*>(ent + 8);
		const float* const org = reinterpret_cast<const float*>(ent + 0x17C);
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD] apply slot=%d ent=%p handle=0x%08X name='%s' scriptIdx=%d sig='%s' "
			"notifyIdx=%d sigPre='%s' local=(%.0f,%.0f,%.0f) "
			"origin=(%.0f,%.0f,%.0f) a5name='%s' a5idx=%d adjname='%s' adjidx=%d\n",
			slot, ent, eh,
			Bridge_ReadName(ent),
			Bridge_ReadScriptIdx(ent),
			(sig && sig[0] >= 0x20 && sig[0] < 0x7F) ? sig : "",
			notifyIdxPre, sigPre, localX, localY, localZ,
			org[0], org[1], org[2],
			Bridge_ReadName(raw),
			Bridge_ReadScriptIdx(raw),
			Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(adj)) ? Bridge_ReadName(adj) : "",
			Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(adj)) ? Bridge_ReadScriptIdx(adj) : 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

static __int64 __fastcall Hook_TransitionEntity(
	__int64 a1, __int64 a2, int* a3, unsigned int a4, __int64* a5,
	__int64 a6, __int64 a7, int a8, int a9, char a10, unsigned int a11, char a12)
{
	InterlockedIncrement(&s_transitionEntityCalls);

	// A fault here is an entity the snapshot could not transition, not a reason
	// to lose the frame: the apply loop below already treats result==0 as "no
	// tick to capture", and the sibling apply hooks are guarded the same way.
	__int64 result = 0;
	__try { result = s_origTransitionEntity(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		result = 0;
		if (s_s2cApplyCaptureActive)
			s_s2cApplyCaptureFailed = true;
	}

	if (a4 < 16384 && a5 && Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(a5)))
		s_entityPtrBySlot[a4] = (uintptr_t)a5;

	if (s_s2cApplyCaptureActive && !s_s2cApplyCaptureFailed && result != 0
		&& a2 && Bridge_IsCanonPtr(static_cast<uint64_t>(a2)))
	{
		uint32_t tick = 0;
		__try { tick = *reinterpret_cast<const uint32_t*>(a2 + 0x30); }
		__except (EXCEPTION_EXECUTE_HANDLER) { tick = 0; }

		if (tick && S21Bridge_S2CScriptRemote_TickNearAccepted(tick))
		{
			if (!s_s2cApplyCaptureValid
				|| static_cast<int32_t>(tick - s_s2cApplyCaptureTick) > 0)
			{
				s_s2cApplyCaptureTick = tick;
				s_s2cApplyCaptureValid = true;
			}
		}
	}
	return result;
}

// ScriptNames table, resolved post svc_CreateStringTable. Name at entity+0x590.
static void* s_scriptNamesTable = nullptr;

static void Bridge_ResolveScriptNamesTable()
{
	if (s_scriptNamesTable) return;
	const uintptr_t base = NetObs_GetExeModuleBase();
	if (!base) return;
	void* container = nullptr;
	__try { container = *(void**)NetObs_StringTableContainerAddr(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	if (!container) return;
	__try {
		void** cv = *(void***)container;
		typedef void* (__fastcall *FindTbl)(void*, const char*);
		s_scriptNamesTable = ((FindTbl)cv[3])(container, "ScriptNames");
	} __except (EXCEPTION_EXECUTE_HANDLER) { s_scriptNamesTable = nullptr; }
	if (s_scriptNamesTable)
		Warning(eDLL_T::ENGINE,
			"[SCRIPTNAME] resolved ScriptNames table=%p\n",
			s_scriptNamesTable);
}

// GetString may return NULL, "", or a non-pointer sentinel (-1). if (ptr)
// accepts -1; the next byte/%s walk is the AV. GetString may not reject OOB
// on this build; bound-check the live table first.
static bool Bridge_NamePtrLive(const char* name)
{
	if (!name || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(name)))
		return false;
	unsigned char c = 0;
	__try { c = static_cast<unsigned char>(name[0]); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return c >= 0x20 && c < 0x7F;
}

static int Bridge_StringTableNumEntries(void* table)
{
	if (!table || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(table)))
		return 0;
	int n = 0;
	__try
	{
		void** const tv = *reinterpret_cast<void***>(table);
		if (!tv || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(tv)))
			return 0;
		const void* const fn = tv[3];
		if (!fn || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(fn)))
			return 0;
		typedef int (__fastcall *NumStr)(void*);
		n = ((NumStr)fn)(table);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
	if (n <= 0 || n > 32768)
		return 0;
	return n;
}

static const char* Bridge_StringTableGet(void* table, int idx)
{
	if (idx < 0)
		return nullptr;
	const int n = Bridge_StringTableNumEntries(table);
	if (idx >= n)
		return nullptr;
	const char* name = nullptr;
	__try
	{
		void** const tv = *reinterpret_cast<void***>(table);
		if (!tv || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(tv)))
			return nullptr;
		const void* const fn = tv[11];
		if (!fn || !Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(fn)))
			return nullptr;
		typedef const char* (__fastcall *GetStr)(void*, int);
		name = ((GetStr)fn)(table, idx);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
	if (!Bridge_NamePtrLive(name))
		return nullptr;
	return name;
}

// S3 m_iName is a string_t pointer; S21 GetTargetName reads char[260] at +0x481.
// If the string never lands, copy ScriptNames[m_scriptNameIndex] into that buffer
// after apply and before PostDataUpdate / OnEntityCreation.
static void Bridge_PromoteScriptNameToTargetName(__int64 entity)
{
	if (!entity || !Bridge_IsCanonPtr(static_cast<uint64_t>(entity)))
		return;
	if (!s_scriptNamesTable)
		Bridge_ResolveScriptNamesTable();
	if (!s_scriptNamesTable)
		return;

	__try
	{
		uint8_t* const p = reinterpret_cast<uint8_t*>(entity);
		if (p[0x481] != 0)
			return;
		const int idx = *reinterpret_cast<int*>(p + 0x588);
		if (idx <= 0)
			return;
		const char* const name = Bridge_StringTableGet(s_scriptNamesTable, idx);
		if (!name)
			return;
		char buf[260];
		DF_CopyStr(reinterpret_cast<uintptr_t>(name), buf, static_cast<int>(sizeof(buf)));
		if (static_cast<unsigned char>(buf[0]) < 0x20)
			return;
		size_t n = 0;
		while (n < 259 && buf[n])
		{
			p[0x481 + n] = static_cast<uint8_t>(buf[n]);
			++n;
		}
		p[0x481 + n] = 0;
		static volatile LONG s_tnN = 0;
		const LONG hits = InterlockedIncrement(&s_tnN);
		if (hits <= 32)
			Warning(eDLL_T::ENGINE,
				"[DEATHFIELD] targetname from script idx=%d -> '%s' ent=%p\n",
				idx, buf, reinterpret_cast<void*>(entity));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

// [CHURN] per-slot ENTERPVS counter (in Hook_RecvTableDecodeMain). Flags
// entities re-created every snapshot instead of preserved.
int s_enterPvsCount[16384] = {};


// modelprecache OnStringChanged runs at signon before paks are resident; stubs stick.
// Re-run the loader once the asset is in. Main thread, chunked.
extern unsigned __int64 Pak_StringToGuid(const char* const string);
static void*        s_mpErrAsset      = nullptr;  // mdl/error.rmdl asset (the fallback) -> a stub marker
static int          s_mpScanCursor    = 1;
static int          s_mpFixedThisSweep = 0;
static int          s_mpStableSweeps  = 0;        // latch after this many consecutive no-fix sweeps
static ConVar sdk_modelprecache_fix(
    "sdk_modelprecache_fix", "1", FCVAR_RELEASE,
    "Refresh stale stub modelprecache entries (assets not resident when the dedi's table "
    "replicated at signon) by re-running the engine studio-load once the asset loads. Fixes "
    "invisible/wrong models. 1=on (default), 0=off.");

static int Bridge_RefreshStubPrecacheChunk(int startIdx, int win)
{
	int fixed = 0;
	const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	if (!base) return 0;
	using FindAssetFn  = void* (__fastcall*)(unsigned __int64, unsigned int*);
	using LoadStudioFn = void* (__fastcall*)(__int64, __int64*, const char*);
	FindAssetFn  findAsset  =
		reinterpret_cast<FindAssetFn>(NetObs_Sym(NetObsSym_t::PakFindAsset));
	LoadStudioFn loadStudio =
		reinterpret_cast<LoadStudioFn>(NetObs_Sym(NetObsSym_t::LoadStudioHdr));

	uintptr_t table = 0, items = 0;
	__try {
		table = *reinterpret_cast<uintptr_t*>(NetObs_ModelPrecacheTablePtrAddr());
		items = NetObs_ModelPrecacheItemsAddr();
	} __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	if (!Bridge_IsCanonPtr(table) || !items) return 0;

	if (!s_mpErrAsset)
		__try { s_mpErrAsset = findAsset(Pak_StringToGuid("mdl/error.rmdl"), nullptr); }
		__except (EXCEPTION_EXECUTE_HANDLER) { s_mpErrAsset = nullptr; }

	for (int idx = startIdx; idx < startIdx + win && idx < 8192; ++idx)
	{
		__try {
			const char* name = Bridge_StringTableGet(reinterpret_cast<void*>(table), idx);
			if (!name || name[0] == '*') continue;
			__int64* entry = *reinterpret_cast<__int64**>(items + (uintptr_t)idx * 16 + 8);
			if (!Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(entry))) continue;
			void* studio = reinterpret_cast<void*>(entry[6]);                          // entry+0x30
			if (Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(studio)) && studio != s_mpErrAsset)
				continue;  // already wired to a real (non-fallback) asset -> leave it
			void* canon = findAsset(Pak_StringToGuid(name), nullptr);
			if (!Bridge_IsCanonPtr(reinterpret_cast<uint64_t>(canon)) || canon == s_mpErrAsset)
				continue;  // asset still not resident -> retry next sweep
			loadStudio(0, entry, name);   // re-run engine studio-load -> wires the real model in
			++fixed;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	return fixed;
}

// [PROPAPPLY-BASE-ADJ] Hook_PropApplyLoop's `a5` is parentBase (see
// PROPAPPLY_ENTITY_ADJ). Skipping the correction reads 0x18 off and
// returns plausible garbage rather than faulting.
static __int64 __fastcall Hook_PropApplyLoop(
	__int64 a1, __int64 a2, int* a3, unsigned int a4, __int64* a5,
	__int64 a6, __int64 a7, int a8, int a9, char a10, unsigned int a11, char a12)
{
	InterlockedIncrement(&s_propApplyCalls);

	// [MPRECACHE-FIX] refresh stale stub precache entries a chunk at a time (main thread). The
	// dedi's precache replicates before assets are resident -> stubs; re-resolve once they load.
	if (sdk_modelprecache_fix.GetBool() && s_mpStableSweeps < 8)
	{
		const int kWin = 512;
		s_mpFixedThisSweep += Bridge_RefreshStubPrecacheChunk(s_mpScanCursor, kWin);
		s_mpScanCursor += kWin;
		if (s_mpScanCursor >= 8192)   // wrapped a full sweep
		{
			if (s_mpFixedThisSweep == 0)
				++s_mpStableSweeps;
			else
			{
				s_mpStableSweeps = 0;
				Warning(eDLL_T::CLIENT, "[MPRECACHE-FIX] sweep refreshed %d stub model-precache entries\n",
					s_mpFixedThisSweep);
			}
			s_mpFixedThisSweep = 0;
			s_mpScanCursor = 1;
		}
	}

	// [PP-IDLE]/[PIPELINE] probe removed.

	// [PROPAPPLY-BASE-ADJ] true entity pointer = a5 - 0x18 (see the PROPAPPLY_ENTITY_ADJ banner).
	const uintptr_t weapEntBase = a5 ? ((uintptr_t)a5 - PROPAPPLY_ENTITY_ADJ) : 0;
	(void)weapEntBase;
	__int64 result = 0;
	__try { result = s_origPropApplyLoop(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		result = 0;
		if (s_s2cApplyCaptureActive)
			s_s2cApplyCaptureFailed = true;
	}

	Bridge_AfterEntityApply(a4, a5);
	return result;
}

// Dump instancebaseline userdata per classID. Empty baseline => default origin/name.
void Bridge_DumpInstanceBaseline(void)
{
	const uintptr_t base = NetObs_GetExeModuleBase();
	if (!base)
	{
		Warning(eDLL_T::ENGINE, "[IB-DUMP] no exe base\n");
		return;
	}

	void* container = nullptr;
	__try {
		container = *(void**)NetObs_StringTableContainerAddr();
	} __except(EXCEPTION_EXECUTE_HANDLER) {}

	if (!container)
	{
		Warning(eDLL_T::ENGINE, "[IB-DUMP] string-table container NULL\n");
		return;
	}

	void* ibTable = nullptr;
	__try {
		void** cv = *(void***)container;
		typedef void* (__fastcall *FindTbl)(void*, const char*);
		ibTable = ((FindTbl)cv[3])(container, "instancebaseline");
	} __except(EXCEPTION_EXECUTE_HANDLER) {}

	if (!ibTable)
	{
		Warning(eDLL_T::ENGINE, "[IB-DUMP] instancebaseline table NOT FOUND in container=%p\n", container);
		return;
	}

	void** tv = *(void***)ibTable;
	int numEntries = 0;
	__try {
		typedef int (__fastcall *NumStr)(void*);
		numEntries = ((NumStr)tv[3])(ibTable);
	} __except(EXCEPTION_EXECUTE_HANDLER) {}

	Warning(eDLL_T::ENGINE,
		"[IB-DUMP] instancebaseline container=%p table=%p numEntries=%d\n",
		container, ibTable, numEntries);

	if (numEntries <= 0 || numEntries > 4096)
	{
		Warning(eDLL_T::ENGINE, "[IB-DUMP] entries out of range, abort\n");
		return;
	}

	const uintptr_t classRegBase = NetObs_ClientClassArrayAddr();
	const uintptr_t maxClassPtr  = NetObs_ClientClassCountAddr();
	int maxClass = 0;
	__try { maxClass = *(int*)maxClassPtr; } __except(EXCEPTION_EXECUTE_HANDLER) {}
	const uintptr_t classRegArr  = *(uintptr_t*)classRegBase;

	int totalNonEmpty = 0;
	int totalZeroLen  = 0;
	int totalSizeSum  = 0;

	for (int i = 0; i < numEntries; i++)
	{
		const char* key = Bridge_StringTableGet(ibTable, i);

		uint8_t* userData = nullptr;
		int userLenBits = 0;
		__try {
			typedef uint8_t* (__fastcall *GetUd)(void*, int, int*);
			userData = ((GetUd)tv[16])(ibTable, i, &userLenBits);
		} __except(EXCEPTION_EXECUTE_HANDLER) {}

		const int userLenBytes = (userLenBits + 7) / 8;
		if (userLenBytes > 0) { ++totalNonEmpty; totalSizeSum += userLenBytes; }
		else                  { ++totalZeroLen; }

		// Resolve classID -> className via the ClientClass registry.
		// Each entry is 32 bytes; the className pointer lives at +0x08.
		// The instancebaseline KEY is the ASCII-decimal classID.
		const char* className = "?";
		int classID = -1;
		if (key) {
			__try { classID = atoi(key); } __except(EXCEPTION_EXECUTE_HANDLER) {}
			if (classRegArr && classID >= 0 && classID < maxClass)
			{
				__try {
					const char* nm = *(const char**)(classRegArr + 32ULL * classID + 8);
					if (Bridge_NamePtrLive(nm))
						className = nm;
				} __except(EXCEPTION_EXECUTE_HANDLER) {}
			}
		}

		// Dump up to first 32 bytes hex.
		char hexBuf[3 * 32 + 1] = {};
		const int dumpN = userLenBytes < 32 ? userLenBytes : 32;
		if (userData && dumpN > 0)
		{
			for (int b = 0; b < dumpN; b++)
			{
				char tmp[4];
				_snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%02X ", userData[b]);
				strcat_s(hexBuf, sizeof(hexBuf), tmp);
			}
		}

		// FNV-1a of the baseline blob (cap 1024), same range as dedi sdk_dump_class_baseline.
		uint64_t fnv = 1469598103934665603ull;
		if (userData && userLenBytes > 0)
		{
			const int crcN = userLenBytes < 1024 ? userLenBytes : 1024;
			__try { for (int b = 0; b < crcN; b++) { fnv ^= userData[b]; fnv *= 1099511628211ull; } }
			__except(EXCEPTION_EXECUTE_HANDLER) { fnv = 0; }
		}

		Warning(eDLL_T::ENGINE,
			"[IB-DUMP] [%3d] key='%-5s' classID=%-3d cls='%s'  bits=%d bytes=%d  fnv=%016llX  hex: %s\n",
			i, key ? key : "?", classID, className,
			userLenBits, userLenBytes, (unsigned long long)fnv,
			(userLenBytes > 0) ? hexBuf : "(EMPTY)");
	}

	Warning(eDLL_T::ENGINE,
		"[IB-DUMP] === SUMMARY === entries=%d nonEmpty=%d zeroLen=%d totalUserBytes=%d\n",
		numEntries, totalNonEmpty, totalZeroLen, totalSizeSum);
}
// sdk_sndc_client_read / sdk_sndc_read_slot ConCommands removed (CC_SNDCClientRead,
// CC_SNDCReadSlot). Both were self-contained diagnostic dumpers with no cross-file
// callers.

// sdk_read_entity_slot ConCommand removed (CC_ReadEntitySlot) -- self-contained
// diagnostic dumper, no cross-file callers.

// sdk_dump_invisible ConCommand removed (CC_DumpInvisibleEntities) -- self-contained
// snapshot dumper of client entity model binds, no cross-file callers.

// sdk_deploy_dump ConCommand removed (CC_DumpDeployables, DeployClassMatch,
// Deploy_LookupEntBySlot) -- self-contained diagnostic dumper, no cross-file
// callers.


// sdk_probe_entity ConCommand removed (Bridge_ProbeEntity_f) -- self-contained
// diagnostic dumper, no cross-file callers.

// String-prop apply calls RecvProp+0x48 proxy. Writer is ClientDLL vtable slot 30 (+0xF0).
typedef __int64 (__fastcall *PFN_ClWriteMoveCmds)();
static PFN_ClWriteMoveCmds s_origClWriteMoveCmds = nullptr;

static __int64 __fastcall Hook_ClWriteMoveCmds()
{
	return s_origClWriteMoveCmds ? s_origClWriteMoveCmds() : 0;
}

// ParseDeltaHeader: passthrough. s_dtSnap / DT_ReadCursor still used by decode TU.
typedef unsigned __int64 (__fastcall *PFN_ParseDeltaHeader)(__int64 a1);
static PFN_ParseDeltaHeader s_origParseDeltaHeader = nullptr;

int       s_dtSnap     = 0;     // snapshot counter

// Per-entity wire body-bit ring. Dump on header desync only.
struct DtBodyCrumb { int seq; int nNewEntity; long long bodyBits; };
static DtBodyCrumb s_dtBodyCrumbs[256] = {};
static int         s_dtBodyHead = 0;
static int         s_dtBodyCount = 0;

// Read the engine CBitRead cursor; false if the struct is not readable.
bool DT_ReadCursor(uintptr_t bitbuf, uintptr_t& curPtr, int& bitsAvail)
{
	if (bitbuf < 0x10000) return false;
	__try {
		curPtr    = *(uintptr_t*)(bitbuf + 0x28);
		bitsAvail = *(int*)      (bitbuf + 0x24);
	} __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_dispSehHits); return false; }
	return true;
}

// Per-prop decode trace. thread_local: entity and TE decode run on different threads.
thread_local bool      s_ptActive     = false;
thread_local int       s_ptEntIdx     = -1;
thread_local int       s_ptPropN      = 0;
thread_local int       s_ptLastIdx    = -1;
thread_local long long s_ptPrevPos    = 0;
long      s_ptTotalLines = 0;
thread_local bool      s_ptEmitLines  = false;

thread_local PropTraceCrumb s_ptCrumbs[32];
thread_local int            s_ptCrumbHead = 0;
thread_local int            s_ptCrumbCount = 0;

// Resolve a classID to its network class name via the ClientClass array
// (0x20 stride, networkName at +8).
const char* DT_ClassName(int cid)
{
	if (cid < 0) return "?";
	__try {
		const uintptr_t base = NetObs_GetExeModuleBase();
		if (!base) return "?";
		const uintptr_t arr = *(uintptr_t*)(NetObs_ClientClassArrayAddr());
		const int n = *(int*)(NetObs_ClientClassCountAddr());
		if (!arr || cid >= n) return "?";
		const char* nm = *(const char**)(arr + (uintptr_t)cid * 0x20 + 8);
		return (nm && (uintptr_t)nm > 0x10000) ? nm : "?";
	} __except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
}

const char* DT_TableName(int cid)
{
	if (cid < 0) return "?";
	__try {
		const uintptr_t base = NetObs_GetExeModuleBase();
		if (!base) return "?";
		const uintptr_t arr = *(uintptr_t*)(NetObs_ClientClassArrayAddr());
		const int n = *(int*)(NetObs_ClientClassCountAddr());
		if (!arr || cid >= n) return "?";
		const char* nm = *(const char**)(arr + (uintptr_t)cid * 0x20 + 0x10);
		return (nm && (uintptr_t)nm > 0x10000) ? nm : "?";
	} __except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
}

static bool DT_IsZiplineOrZiprailName(const char* s)
{
	return s &&
		(strstr(s, "Zipline") || strstr(s, "Ziprail") ||
		 strstr(s, "zipline") || strstr(s, "ziprail"));
}

bool DT_IsZiplineOrZiprailClass(int cid)
{
	return DT_IsZiplineOrZiprailName(DT_ClassName(cid)) ||
		DT_IsZiplineOrZiprailName(DT_TableName(cid));
}


// sdk_dump_mp_idx ConCommand removed (CC_DumpMpIdx, prior batch) -- Bridge_ModelIdxNames was
// kept then because Bridge_DumpModelIdxDivergence still called it. Both are now removed
// (this batch, via the sdk_modelidx_autodump removal above).

// sdk_dump_player_bind ConCommand removed (CC_DumpPlayerBind) -- self-contained
// diagnostic dumper, no cross-file callers.

// sdk_modelloader_probe ConCommand removed (CC_ModelLoaderProbe) -- self-contained
// diagnostic dumper, no cross-file callers.


// sdk_dump_stub_detail ConCommand removed (CC_DumpStubDetail) -- self-contained
// diagnostic dumper, no cross-file callers.

// sdk_dump_modelprecache_full ConCommand removed (CC_DumpModelPrecacheFull) --
// self-contained diagnostic dumper, no cross-file callers. MdlPrecacheShadow_GetStats
// itself is defined in engine/mdl_precache_client_grow.cpp and is untouched.

// sdk_dump_skinnames_table ConCommand removed (CC_DumpSkinnamesTable) --
// self-contained diagnostic dumper, no cross-file callers.

// sdk_dump_propsurvival_slots ConCommand removed (CC_DumpPropSurvivalSlots) --
// self-contained diagnostic dumper, no cross-file callers.

// sdk_verify_entity_modelidx ConCommand removed (CC_VerifyEntityModelIdx_f + its helper
// EModel_ScanEnt) -- self-contained diagnostic dumper, no cross-file callers.

// sdk_dump_models ConCommand removed (CC_DumpModels + its helper ModelDiag_ScanEnt) --
// self-contained diagnostic dumper, no cross-file callers.

// sdk_anim_probe ConCommand removed (CC_AnimProbe) -- self-contained diagnostic
// dumper, no cross-file callers.

// sdk_anim_hdr ConCommand removed (CC_AnimHdr + its helper StudioMagicOk) --
// self-contained diagnostic dumper, no cross-file callers.


// ParseDeltaHeader passthrough; attach stays in the shared DetourTransaction.
static unsigned __int64 __fastcall Hook_ParseDeltaHeader(__int64 a1)
{
	const unsigned __int64 result = s_origParseDeltaHeader(a1);

	// Signed header-index compare lets a wrap go OOB. Sanitize index/base; overflow the bitbuf.
	if (*reinterpret_cast<const uint32_t*>(a1 + 24) >= 0x4000u)
	{
		*reinterpret_cast<uint32_t*>(a1 + 24) = 0;
		*reinterpret_cast<uint32_t*>(a1 + 28) = 0;

		uint8_t* const pBitBuf = *reinterpret_cast<uint8_t**>(a1 + 48);
		if (pBitBuf)
			pBitBuf[8] = 1;

		static int s_nBadIndexDrops = 0;
		if (++s_nBadIndexDrops <= 10)
			Warning(eDLL_T::ENGINE, "[BRIDGE-SNAP] dropped out-of-range entity index (hostile/buggy server) -- frame discarded\n");
	}

	return result;
}

void Bridge_DumpBodyBitsRing(const char* tag)
{
	BridgeTrace_Log("[DT-BODY] ===== wire body-bit ring on %s (last %d entities) =====\n",
		tag ? tag : "?", s_dtBodyCount);
	const int bs = (s_dtBodyHead - s_dtBodyCount + 256) & 255;
	for (int i = 0; i < s_dtBodyCount; ++i)
	{
		const DtBodyCrumb& b = s_dtBodyCrumbs[(bs + i) & 255];
		BridgeTrace_Log("[DT-BODY]   hdr#%d ent=%d bodyBits=%lld\n",
			b.seq, b.nNewEntity, b.bodyBits);
	}
}


S21PlaylistOverride_t s_playlistOverrides[S21BR_PLO_MAX_ENTRIES];

// Written by the net thread, read by the CLIENT/UI script VMs. Entries are always
// filled before the count is published and the count is zeroed before entries are
// rewritten, so a racing reader sees an old entry or no entry -- never a torn one.
volatile long s_nPlaylistOverrides = 0;

static ConVar bridge_playlist_overrides("bridge_playlist_overrides", "1", FCVAR_RELEASE,
	"Apply the server's runtime playlist var overrides (svc_PlaylistOverrides). "
	"0 = ignore them and always read the local playlist file value.");

static void S21Bridge_ClearPlaylistOverrides(void)
{
	if (s_nPlaylistOverrides == 0)
		return;

	s_nPlaylistOverrides = 0;
	SDK_Log("[BRIDGE-PLO] override table cleared\n");
}

// Case-insensitive, mirroring the dedi's own override lookup.
static const char* S21Bridge_FindPlaylistOverride(const char* pszVar)
{
	if (!pszVar || !pszVar[0] || !bridge_playlist_overrides.GetBool())
		return nullptr;

	const long count = s_nPlaylistOverrides;
	for (long i = 0; i < count && i < S21BR_PLO_MAX_ENTRIES; i++)
	{
		if (_stricmp(s_playlistOverrides[i].m_szName, pszVar) == 0)
			return s_playlistOverrides[i].m_szValue;
	}
	return nullptr;
}

typedef __int64(__fastcall* PFN_PlaylistGetCurrentVar)(__int64 thisptr, const char* pszVar);
typedef __int64(__fastcall* PFN_PlaylistGetVar)(__int64 thisptr, const char* pszPlaylist, const char* pszVar);

static PFN_PlaylistGetCurrentVar s_origPlaylistGetCurrentVar = nullptr;
static PFN_PlaylistGetVar s_origPlaylistGetVar = nullptr;

static __int64 __fastcall Hook_PlaylistGetCurrentVar(__int64 thisptr, const char* pszVar)
{
	const char* const pszOverride = S21Bridge_FindPlaylistOverride(pszVar);
	if (pszOverride)
		return reinterpret_cast<__int64>(pszOverride);

	return s_origPlaylistGetCurrentVar(thisptr, pszVar);
}

static __int64 __fastcall Hook_PlaylistGetVar(__int64 thisptr, const char* pszPlaylist, const char* pszVar)
{
	const char* const pszOverride = S21Bridge_FindPlaylistOverride(pszVar);
	if (pszOverride)
		return reinterpret_cast<__int64>(pszOverride);

	return s_origPlaylistGetVar(thisptr, pszPlaylist, pszVar);
}

static void S21Bridge_ResolvePlaylistVarGetters(void)
{
	// Playlist singleton vtable slots 90/92: `if (currentPlaylistNode) tail-call
	// the KeyValues var resolver`. Both rip-relative operands are wildcarded; the
	// jmp opcode anchors the tail call.
	Module_FindPattern(g_GameDll,
		"48 8B 0D ?? ?? ?? ?? 48 85 C9 75 03 33 C0 C3 4C 8B 05 ?? ?? ?? ?? 4C 8B CA 48 8B 15 ?? ?? ?? ?? E9")
		.GetPtr(s_origPlaylistGetCurrentVar);

	// Slots 75/76: resolve the named playlist first, then the same resolver.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 49 8B D8 48 8D 4C 24 ?? 45 33 C0 E8 ?? ?? ?? ?? 84 C0 75 08 33 C0 48 83 C4 20 5B C3 "
		"4C 8B 05 ?? ?? ?? ?? 4C 8B CB 48 8B 15 ?? ?? ?? ?? 48 8B 4C 24 ?? 48 83 C4 20 5B E9")
		.GetPtr(s_origPlaylistGetVar);

	if (!s_origPlaylistGetCurrentVar)
		Warning(eDLL_T::CLIENT, "[BRIDGE-PLO] current-playlist var getter pattern unresolved -- "
			"server playlist var overrides will not apply\n");
	if (!s_origPlaylistGetVar)
		Warning(eDLL_T::CLIENT, "[BRIDGE-PLO] by-name playlist var getter pattern unresolved -- "
			"overrides will only apply to the current playlist\n");
}

static void CC_DumpPlaylistOverrides(const CCommand& args)
{
	NOTE_UNUSED(args);

	const long count = s_nPlaylistOverrides;
	Msg(eDLL_T::CLIENT, "[BRIDGE-PLO] %ld server playlist var override(s), apply=%d "
		"(current-getter=%p by-name-getter=%p)\n",
		count, bridge_playlist_overrides.GetBool() ? 1 : 0,
		reinterpret_cast<void*>(s_origPlaylistGetCurrentVar),
		reinterpret_cast<void*>(s_origPlaylistGetVar));

	for (long i = 0; i < count && i < S21BR_PLO_MAX_ENTRIES; i++)
		Msg(eDLL_T::CLIENT, "[BRIDGE-PLO]   %s = %s\n",
			s_playlistOverrides[i].m_szName, s_playlistOverrides[i].m_szValue);
}
static ConCommand bridge_playlist_overrides_dump("bridge_playlist_overrides_dump", CC_DumpPlaylistOverrides,
	"Lists the playlist var overrides received from the server.", FCVAR_RELEASE);

void Bridge_InstallApplyPipeHooks(void)
{
	if (s_origClProcessFrame) return;

	uintptr_t base = NetObs_GetExeModuleBase();
	if (!base) return;

	s_origClProcessFrame      = (PFN_ClProcessFrame)     NetObs_Sym(NetObsSym_t::ClProcessFrame);
	s_origTransitionEntity    = (PFN_TransitionEntity)   NetObs_Sym(NetObsSym_t::TransitionEntity);
	s_origPropApplyLoop       = (PFN_PropApplyLoop)      NetObs_Sym(NetObsSym_t::PropApplyLoop);
	s_origClProcessSnapshotWT = (PFN_ClProcessSnapshotWT)NetObs_Sym(NetObsSym_t::ClProcessSnapshotWT);
	s_origClWriteMoveCmds     = (PFN_ClWriteMoveCmds)    NetObs_Sym(NetObsSym_t::ClWriteMoveCmds);
	s_origParseDeltaHeader    = (PFN_ParseDeltaHeader)   NetObs_Sym(NetObsSym_t::ParseDeltaHeader);
	s_origClSendTick          = (PFN_ClSendTick)         NetObs_Sym(NetObsSym_t::ClSendTick);
	s_origEngineFrame         = (PFN_EngineFrame)        NetObs_Sym(NetObsSym_t::EngineFrame);
	s_origHostStateFrame      = (PFN_HostStateFrame)     NetObs_Sym(NetObsSym_t::HostStateFrame);
	s_origHostFrame           = (PFN_HostFrame)          NetObs_Sym(NetObsSym_t::HostFrame);
	// [PRED-HOOK] -- (PlayerRunCommand_Prediction).
	s_origPlayerRunCmdPred    = (PFN_PlayerRunCmdPred)   NetObs_Sym(NetObsSym_t::PlayerRunCmdPred);
	// clock-drift controller.
	s_origClockDrift          = (PFN_ClockDrift)         NetObs_Sym(NetObsSym_t::ClockDrift);
	s_origClProcessSnapshotDispatch = (PFN_ClProcessSnapshotDispatch) NetObs_Sym(NetObsSym_t::ClProcessSnapshotDispatch);
	// [DMG-DIAG-NATIVE] resolved via verified byte signature

	DidDmgDiag_Resolve();
	RemoteMsgHandlers_Resolve();
	// [BRIDGE-PLO] pattern-resolved via unique signatures (not a DX11/DX12 VA pair).
	S21Bridge_ResolvePlaylistVarGetters();
	// [SKY-SETUP] CViewRender::SetupSky entry trace.
	s_origSetupSky            = (PFN_SetupSky)           NetObs_Sym(NetObsSym_t::SetupSky);
	// Render orchestrator entry. Sole gate is m_has3DSky.
	s_origRenderOrchestrator = (PFN_RenderOrchestrator)
		NetObs_Sym(NetObsSym_t::RenderOrchestrator);
	// [ANIM-FIX] client C_BaseAnimating::StudioFrameAdvance.
	s_origStudioFrameAdvance  = (PFN_StudioFrameAdvance) NetObs_Sym(NetObsSym_t::StudioFrameAdvance);
	// [ANIM-FIX] C_BaseAnimatingOverlay::StudioFrameAdvance -- the PLAYER/NPC path

	s_origStudioFrameAdvanceOverlay = (PFN_StudioFrameAdvanceOverlay) NetObs_Sym(NetObsSym_t::StudioFrameAdvanceOverlay);
	// [SEQ-WRITER] SetSequence-equivalent -- detoured to log the phantom-seq writer's caller.
	s_origSetSequence = (PFN_SetSequence) NetObs_Sym(NetObsSym_t::SetSequence);
	// [SEQ-APPLY] client per-frame anim-state apply -- protect the networked seq it
	// stomps from the mis-fed interp source for gate==0 (server-animated) bridged entities.
	s_origAnimStateApply = (PFN_AnimStateApply) NetObs_Sym(NetObsSym_t::AnimStateApply);
	// [ANIM-WIRE-SKIP] C_Player::SkipsAnimationData (C_Player vtbl slot 45).
	s_origPlayerSkipsAnimData = (PFN_PlayerSkipsAnimData) NetObs_Sym(NetObsSym_t::PlayerSkipsAnimData);
	// [ANIM-WIRE-SKIP] C_BaseAnimatingOverlay::InterpolateFieldsInternal.
	s_origOverlayInterpFields = (PFN_OverlayInterpFields) NetObs_Sym(NetObsSym_t::OverlayInterpFields);
	// [MPT-PROBE] C_Player::MovePostThink -- STEP-1 native-anim verified probe.
	s_origMovePostThink = (PFN_MovePostThink) NetObs_Sym(NetObsSym_t::MovePostThink);
	// [SIGNON-TRACE] CClientState::SetSignonState + CL_FullyConnected.
	s_origCS_SetSignonState = (PFN_CClientState_SetSignonState) NetObs_Sym(NetObsSym_t::CS_SetSignonState);
	s_origCL_FullyConnected = (PFN_CL_FullyConnected) NetObs_Sym(NetObsSym_t::CL_FullyConnected);
	// [ANIM-DIR-CLAMP] FIX 4: ComputeMainSequence -- clamp moveDirection 4->3.
	s_origComputeMainSeq = reinterpret_cast<decltype(s_origComputeMainSeq)>(
		NetObs_Sym(NetObsSym_t::ComputeMainSequence));
	// [SEQ-PICK] SelectWeightedSequence -- capture the real transient modifier set.
	s_origSelectWeightedSeq = reinterpret_cast<decltype(s_origSelectWeightedSeq)>(
		NetObs_Sym(NetObsSym_t::SelectWeightedSequence));
	// [NOCLIP-SIM] C_GameMovement::PlayerMove (; pattern-verified unique, see the
	// PFN typedef banner).
	s_origPlayerMoveClient = (PFN_PlayerMoveClient) NetObs_Sym(NetObsSym_t::PlayerMoveClient);

	if (DetourTransactionBegin() != NO_ERROR) {
		s_origClProcessFrame = nullptr; s_origTransitionEntity = nullptr;
		s_origPropApplyLoop = nullptr; s_origClProcessSnapshotWT = nullptr;
		s_origClWriteMoveCmds = nullptr; s_origParseDeltaHeader = nullptr;
		s_origClSendTick = nullptr; s_origEngineFrame = nullptr;
		s_origHostStateFrame = nullptr; s_origHostFrame = nullptr;
		s_origClockDrift = nullptr; s_origClProcessSnapshotDispatch = nullptr;
		s_origPlayerDidDamageParse = nullptr;
		s_origSetupSky = nullptr; s_origRenderOrchestrator = nullptr;
		s_origStudioFrameAdvance = nullptr; s_origStudioFrameAdvanceOverlay = nullptr;
		s_origSetSequence = nullptr; s_origAnimStateApply = nullptr;
		s_origPlayerSkipsAnimData = nullptr; s_origOverlayInterpFields = nullptr;
		s_origMovePostThink = nullptr;
		s_origCS_SetSignonState = nullptr; s_origCL_FullyConnected = nullptr;
		s_origPlayerMoveClient = nullptr;
		return;
	}
	DetourUpdateThread(GetCurrentThread());
	const auto rA = s_origClProcessFrame ? DetourAttach(reinterpret_cast<PVOID*>(&s_origClProcessFrame),      reinterpret_cast<PVOID>(&Hook_ClProcessFrame)) : (LONG)NO_ERROR;
	const auto rB = s_origTransitionEntity ? DetourAttach(reinterpret_cast<PVOID*>(&s_origTransitionEntity),    reinterpret_cast<PVOID>(&Hook_TransitionEntity)) : (LONG)NO_ERROR;
	const auto rC = s_origPropApplyLoop ? DetourAttach(reinterpret_cast<PVOID*>(&s_origPropApplyLoop),       reinterpret_cast<PVOID>(&Hook_PropApplyLoop)) : (LONG)NO_ERROR;
	const auto rD = s_origClProcessSnapshotWT ? DetourAttach(reinterpret_cast<PVOID*>(&s_origClProcessSnapshotWT), reinterpret_cast<PVOID>(&Hook_ClProcessSnapshotWT)) : (LONG)NO_ERROR;
	const auto rE = s_origClWriteMoveCmds ? DetourAttach(reinterpret_cast<PVOID*>(&s_origClWriteMoveCmds),     reinterpret_cast<PVOID>(&Hook_ClWriteMoveCmds)) : (LONG)NO_ERROR;
	const auto rF = s_origParseDeltaHeader ? DetourAttach(reinterpret_cast<PVOID*>(&s_origParseDeltaHeader),    reinterpret_cast<PVOID>(&Hook_ParseDeltaHeader)) : (LONG)NO_ERROR;
	const auto rG = s_origClSendTick ? DetourAttach(reinterpret_cast<PVOID*>(&s_origClSendTick),          reinterpret_cast<PVOID>(&Hook_ClSendTick)) : (LONG)NO_ERROR;
	const auto rH = s_origEngineFrame ? DetourAttach(reinterpret_cast<PVOID*>(&s_origEngineFrame),         reinterpret_cast<PVOID>(&Hook_EngineFrame)) : (LONG)NO_ERROR;
	// [EARLY-PIPE] already owns HostStateFrame when its init-time attach won.
	const auto rI = (s_bHostStateEarlyHooked || !s_origHostStateFrame) ? (LONG)NO_ERROR
	                                        : DetourAttach(reinterpret_cast<PVOID*>(&s_origHostStateFrame), reinterpret_cast<PVOID>(&Hook_HostStateFrame));
	const auto rJ = s_origHostFrame ? DetourAttach(reinterpret_cast<PVOID*>(&s_origHostFrame),           reinterpret_cast<PVOID>(&Hook_HostFrame)) : (LONG)NO_ERROR;
	const auto rJpred = s_origPlayerRunCmdPred ? DetourAttach(reinterpret_cast<PVOID*>(&s_origPlayerRunCmdPred), reinterpret_cast<PVOID>(&Hook_PlayerRunCmdPred)) : (LONG)NO_ERROR;
	if (rJpred != NO_ERROR) Warning(eDLL_T::CLIENT, "[PRED-HOOK] DetourAttach failed err=%ld\n", rJpred);
	const auto rK = s_origClockDrift ? DetourAttach(reinterpret_cast<PVOID*>(&s_origClockDrift),          reinterpret_cast<PVOID>(&Hook_ClockDrift)) : (LONG)NO_ERROR;
	const auto rL = s_origClProcessSnapshotDispatch ? DetourAttach(reinterpret_cast<PVOID*>(&s_origClProcessSnapshotDispatch), reinterpret_cast<PVOID>(&Hook_ClProcessSnapshotDispatch)) : (LONG)NO_ERROR;
	const auto rM = s_origSetupSky ? DetourAttach(reinterpret_cast<PVOID*>(&s_origSetupSky),            reinterpret_cast<PVOID>(&Hook_SetupSky)) : (LONG)NO_ERROR;
	const auto rN = s_origRenderOrchestrator ? DetourAttach(reinterpret_cast<PVOID*>(&s_origRenderOrchestrator), reinterpret_cast<PVOID>(&Hook_RenderOrchestrator)) : (LONG)NO_ERROR;
	const auto rO = s_origStudioFrameAdvance ? DetourAttach(reinterpret_cast<PVOID*>(&s_origStudioFrameAdvance),   reinterpret_cast<PVOID>(&Hook_StudioFrameAdvance)) : (LONG)NO_ERROR;
	const auto rP = s_origStudioFrameAdvanceOverlay ? DetourAttach(reinterpret_cast<PVOID*>(&s_origStudioFrameAdvanceOverlay), reinterpret_cast<PVOID>(&Hook_StudioFrameAdvanceOverlay)) : (LONG)NO_ERROR;
	const auto rQ = s_origSetSequence ? DetourAttach(reinterpret_cast<PVOID*>(&s_origSetSequence),         reinterpret_cast<PVOID>(&Hook_SetSequence)) : (LONG)NO_ERROR;
	const auto rR = s_origAnimStateApply ? DetourAttach(reinterpret_cast<PVOID*>(&s_origAnimStateApply),      reinterpret_cast<PVOID>(&Hook_AnimStateApply)) : (LONG)NO_ERROR;
	const auto rANS = s_origPlayerSkipsAnimData ? DetourAttach(reinterpret_cast<PVOID*>(&s_origPlayerSkipsAnimData), reinterpret_cast<PVOID>(&Hook_PlayerSkipsAnimData)) : (LONG)NO_ERROR;
	const auto rOIF = s_origOverlayInterpFields ? DetourAttach(reinterpret_cast<PVOID*>(&s_origOverlayInterpFields), reinterpret_cast<PVOID>(&Hook_OverlayInterpFields)) : (LONG)NO_ERROR;
	const auto rT = s_origMovePostThink ? DetourAttach(reinterpret_cast<PVOID*>(&s_origMovePostThink),       reinterpret_cast<PVOID>(&Hook_MovePostThink)) : (LONG)NO_ERROR;
	const auto rZ = s_origCS_SetSignonState ? DetourAttach(reinterpret_cast<PVOID*>(&s_origCS_SetSignonState),   reinterpret_cast<PVOID>(&Hook_CS_SetSignonState)) : (LONG)NO_ERROR;
	const auto rAA= s_origCL_FullyConnected ? DetourAttach(reinterpret_cast<PVOID*>(&s_origCL_FullyConnected),   reinterpret_cast<PVOID>(&Hook_CL_FullyConnected)) : (LONG)NO_ERROR;
	const auto rBB= s_origComputeMainSeq ? DetourAttach(reinterpret_cast<PVOID*>(&s_origComputeMainSeq), reinterpret_cast<PVOID>(&Hook_ComputeMainSeq)) : (LONG)NO_ERROR;
	const auto rSP= s_origSelectWeightedSeq ? DetourAttach(reinterpret_cast<PVOID*>(&s_origSelectWeightedSeq), reinterpret_cast<PVOID>(&Hook_SelectWeightedSeq)) : (LONG)NO_ERROR;
	const auto rNC= s_origPlayerMoveClient ? DetourAttach(reinterpret_cast<PVOID*>(&s_origPlayerMoveClient), reinterpret_cast<PVOID>(&Hook_PlayerMoveClient)) : (LONG)NO_ERROR;
	if (rNC != NO_ERROR) Warning(eDLL_T::CLIENT, "[NOCLIP-SIM] DetourAttach failed err=%ld\n", rNC);
	const auto rPLOa = s_origPlaylistGetCurrentVar ? DetourAttach(reinterpret_cast<PVOID*>(&s_origPlaylistGetCurrentVar), reinterpret_cast<PVOID>(&Hook_PlaylistGetCurrentVar)) : (LONG)NO_ERROR;
	const auto rPLOb = s_origPlaylistGetVar ? DetourAttach(reinterpret_cast<PVOID*>(&s_origPlaylistGetVar), reinterpret_cast<PVOID>(&Hook_PlaylistGetVar)) : (LONG)NO_ERROR;
	if (rPLOa != NO_ERROR) Warning(eDLL_T::CLIENT, "[BRIDGE-PLO] current-getter DetourAttach failed err=%ld\n", rPLOa);
	if (rPLOb != NO_ERROR) Warning(eDLL_T::CLIENT, "[BRIDGE-PLO] by-name-getter DetourAttach failed err=%ld\n", rPLOb);
	if (rBB != NO_ERROR) Warning(eDLL_T::CLIENT, "[ANIM-DIR-CLAMP] DetourAttach failed err=%ld\n", rBB);
	if (rSP != NO_ERROR) Warning(eDLL_T::CLIENT, "[SEQ-PICK] DetourAttach failed err=%ld\n", rSP);
	if (rA != NO_ERROR || rB != NO_ERROR || rC != NO_ERROR || rD != NO_ERROR || rE != NO_ERROR || rF != NO_ERROR || rG != NO_ERROR || rH != NO_ERROR || rI != NO_ERROR || rJ != NO_ERROR || rK != NO_ERROR || rL != NO_ERROR || rM != NO_ERROR || rN != NO_ERROR || rO != NO_ERROR || rP != NO_ERROR || rQ != NO_ERROR || rR != NO_ERROR || rANS != NO_ERROR || rOIF != NO_ERROR || rT != NO_ERROR || rZ != NO_ERROR || rAA != NO_ERROR) {
		DetourTransactionAbort();
		s_origClProcessFrame = nullptr; s_origTransitionEntity = nullptr;
		s_origPropApplyLoop = nullptr; s_origClProcessSnapshotWT = nullptr;
		s_origClWriteMoveCmds = nullptr; s_origParseDeltaHeader = nullptr;
		s_origClSendTick = nullptr; s_origEngineFrame = nullptr;
		s_origHostStateFrame = nullptr; s_origHostFrame = nullptr;
		s_origClockDrift = nullptr; s_origClProcessSnapshotDispatch = nullptr;
		s_origPlayerDidDamageParse = nullptr;
		s_origSetupSky = nullptr; s_origRenderOrchestrator = nullptr;
		s_origStudioFrameAdvance = nullptr; s_origStudioFrameAdvanceOverlay = nullptr;
		s_origSetSequence = nullptr; s_origAnimStateApply = nullptr;
		s_origPlayerSkipsAnimData = nullptr; s_origOverlayInterpFields = nullptr;
		s_origMovePostThink = nullptr;
		s_origCS_SetSignonState = nullptr; s_origCL_FullyConnected = nullptr;
		s_origComputeMainSeq = nullptr;
		s_origSelectWeightedSeq = nullptr;
		Warning(eDLL_T::ENGINE,
			"[APPLY-PIPE] hook install FAILED (rA=%ld rB=%ld rC=%ld rD=%ld rE=%ld rF=%ld rG=%ld rH=%ld rI=%ld rJ=%ld rK=%ld rL=%ld rM=%ld rN=%ld rO=%ld rP=%ld rQ=%ld rR=%ld rANS=%ld rOIF=%ld rT=%ld rZ=%ld rAA=%ld)\n",
			rA, rB, rC, rD, rE, rF, rG, rH, rI, rJ, rK, rL, rM, rN, rO, rP, rQ, rR, rANS, rOIF, rT, rZ, rAA);
		return;
	}
	DetourTransactionCommit();

	const int nSkippedHook = (!s_origClProcessFrame) + (!s_origTransitionEntity) + (!s_origPropApplyLoop)
		+ (!s_origClProcessSnapshotWT) + (!s_origClWriteMoveCmds) + (!s_origParseDeltaHeader)
		+ (!s_origClSendTick) + (!s_origEngineFrame) + (!s_origHostStateFrame) + (!s_origHostFrame)
		+ (!s_origPlayerRunCmdPred) + (!s_origClockDrift) + (!s_origClProcessSnapshotDispatch)
		+ (!s_origSetupSky) + (!s_origRenderOrchestrator) + (!s_origStudioFrameAdvance)
		+ (!s_origStudioFrameAdvanceOverlay) + (!s_origSetSequence) + (!s_origAnimStateApply)
		+ (!s_origPlayerSkipsAnimData) + (!s_origOverlayInterpFields) + (!s_origMovePostThink)
		+ (!s_origCS_SetSignonState) + (!s_origCL_FullyConnected) + (!s_origComputeMainSeq)
		+ (!s_origSelectWeightedSeq) + (!s_origPlayerMoveClient) + (!s_origPlaylistGetCurrentVar)
		+ (!s_origPlaylistGetVar);
	if (nSkippedHook)
		Warning(eDLL_T::ENGINE, "[APPLY-PIPE] hook install skipped %d null target(s)\n", nSkippedHook);

	// [ACK-RESET] standalone install of the prediction-finalizer hook in its own
	// transaction (kept out of the main detour batch above). Resolves the globals used by the diagnostic log.
	s_origPredFinalize = (PFN_PredFinalize) NetObs_Sym(NetObsSym_t::PredFinalize);
	if (!s_animgateByte994) s_animgateByte994 = NetObs_Sym(NetObsSym_t::AnimGateByte994);
	s_byteA9           = NetObs_Sym(NetObsSym_t::AnimGateByteA9);
	s_predConvarGlobal = NetObs_Sym(NetObsSym_t::PredConvarGlobal);
	if (s_origPredFinalize)
	{
		if (DetourTransactionBegin() == NO_ERROR)
		{
			DetourUpdateThread(GetCurrentThread());
			const auto rAck = DetourAttach(reinterpret_cast<PVOID*>(&s_origPredFinalize), reinterpret_cast<PVOID>(&Hook_PredFinalize));
			if (rAck != NO_ERROR) { DetourTransactionAbort(); s_origPredFinalize = nullptr; Warning(eDLL_T::ENGINE, "[ACK-RESET] attach FAILED (%ld)\n", rAck); }
			else { DetourTransactionCommit(); Warning(eDLL_T::ENGINE, "[ACK-RESET] prediction-finalizer hook installed @ %p (ack-fix default ON)\n", (void*)s_origPredFinalize); }
		}
		else s_origPredFinalize = nullptr;
	}
	else Warning(eDLL_T::ENGINE, "[ACK-RESET] resolve FAILED \n");

	// Translate player+8118 S3->S21 on the activity selector. Gated by bridge_act_xlat_qact.
	s_origActivitySelector = (PFN_ActivitySelector) NetObs_Sym(NetObsSym_t::ActivitySelector);
	if (s_origActivitySelector && DetourTransactionBegin() == NO_ERROR) {
		DetourUpdateThread(GetCurrentThread());
		const auto rAX = DetourAttach(reinterpret_cast<PVOID*>(&s_origActivitySelector), reinterpret_cast<PVOID>(&Hook_ActivitySelector));
		if (rAX == NO_ERROR) {
			DetourTransactionCommit();
		} else {
			DetourTransactionAbort(); s_origActivitySelector = nullptr;
			Warning(eDLL_T::CLIENT, "[ACT-XLAT] hook install FAILED rAX=%ld\n", (long)rAX);
		}
	}

	// [SLOT-KEEPALIVE] separate transaction: hook the mgr2 per-slot DEACTIVATOR
	// so the bridge-installed slot 0 survives the per-snapshot tear-down. Default ON via convar.
	s_origSlotDeactivate = (PFN_SlotDeactivate) NetObs_Sym(NetObsSym_t::SlotDeactivate);
	if (s_origSlotDeactivate && DetourTransactionBegin() == NO_ERROR) {
		DetourUpdateThread(GetCurrentThread());
		const auto rSK = DetourAttach(reinterpret_cast<PVOID*>(&s_origSlotDeactivate), reinterpret_cast<PVOID>(&Hook_SlotDeactivate));
		if (rSK == NO_ERROR) {
			DetourTransactionCommit();
			Warning(eDLL_T::ENGINE, "[SLOT-KEEPALIVE] deactivate hook installed @ 0x%p\n", (void*)s_origSlotDeactivate);
		} else {
			DetourTransactionAbort(); s_origSlotDeactivate = nullptr;
			Warning(eDLL_T::ENGINE, "[SLOT-KEEPALIVE] hook install FAILED rSK=%ld\n", (long)rSK);
		}
	}

	// [EVT-BATCH] separate transaction: wire-time TE event decode. Logs per-event
	// table + bit cost so a sheared batch names its origin (TE_EVENT_TABLE_DRIFT_MASTER.md).
	s_origEvtWireDecode = (PFN_EvtWireDecode) NetObs_Sym(NetObsSym_t::RecvDecode);
	if (s_origEvtWireDecode && DetourTransactionBegin() == NO_ERROR) {
		DetourUpdateThread(GetCurrentThread());
		const auto rEB = DetourAttach(reinterpret_cast<PVOID*>(&s_origEvtWireDecode), reinterpret_cast<PVOID>(&Hook_EvtWireDecode));
		if (rEB == NO_ERROR) {
			DetourTransactionCommit();
			Warning(eDLL_T::ENGINE, "[EVT-BATCH] wire event decode hook installed @ 0x%p\n", (void*)s_origEvtWireDecode);
		} else {
			DetourTransactionAbort(); s_origEvtWireDecode = nullptr;
			Warning(eDLL_T::ENGINE, "[EVT-BATCH] hook install FAILED rEB=%ld\n", (long)rEB);
		}
	}

	// AnimPredGate in a later transaction so it chains outside the MovePostThink hook.
	s_origAnimPredGate = (PFN_AnimPredGate) NetObs_Sym(NetObsSym_t::MovePostThink);
	if (s_origAnimPredGate && DetourTransactionBegin() == NO_ERROR) {
		DetourUpdateThread(GetCurrentThread());
		const auto rAPG = DetourAttach(reinterpret_cast<PVOID*>(&s_origAnimPredGate), reinterpret_cast<PVOID>(&Hook_AnimPredGate));
		if (rAPG == NO_ERROR) {
			DetourTransactionCommit();
		} else {
			DetourTransactionAbort(); s_origAnimPredGate = nullptr;
			Warning(eDLL_T::CLIENT, "[ANIM-PRED-GATE] hook install FAILED rAPG=%ld\n", (long)rAPG);
		}
	}

	// [PFX-GATE] separate transaction: the two silent NULL returns on the particle
	// create path, so a missing effect names the gate that ate it instead of vanishing.
	PfxGate_Install();

	Warning(eDLL_T::ENGINE,
		"[APPLY-PIPE] hooks installed:\n"
		"  ClProcessFrame       @ 0x%p\n"
		"  TransitionEntity     @ 0x%p\n"
		"  PropApplyLoop        @ 0x%p\n"
		"  ClProcessSnapshotWT  @ 0x%p  (chain-miss diag)\n"
		"  CL_WriteMoveCmds     @ 0x%p  (vtable[30] writer-RVA probe)\n"
		"  CL_ParseDeltaHeader  @ 0x%p  (inert passthrough)\n"
		"  ClSendTick           @ 0x%p  (per-frame C2S send gate probe -- [SENDTICK])\n"
		"EngineFrame @ 0x%p ( write-watchpoint -- [FTW]/[FTPOLL])\n"
		"  HostStateFrame       @ 0x%p  (frametime-input trace -- [FTIN-HS])\n"
		"  HostFrame            @ 0x%p  (frametime-input trace -- [FTIN-HF])\n"
		"ClockDrift @ 0x%p ( dedupe -- fixes [0x13C]=0.1 sim slowdown)\n"
		"SetupSky @ 0x%p ( -- [SKY-SETUP] entry-trace;\n"
		"                                writes m_has3DSky at viewRender+0x11A375)\n"
		"RenderOrchestrator @ 0x%p ( -- [SKY-ORCH] entry-trace;\n"
		"                                m_has3DSky gate confirmed open both modes.\n"
		"Inner-sub divergence is in /\n"
		"/ -- see audit.)\n"
		"MovePostThink @ 0x%p ( -- [MPT-PROBE] client-anim\n"
		"STEP-1 verified; gate + m_PlayerAnimState per player)\n",
		(void*)s_origClProcessFrame, (void*)s_origTransitionEntity,
		(void*)s_origPropApplyLoop, (void*)s_origClProcessSnapshotWT,
		(void*)s_origClWriteMoveCmds, (void*)s_origParseDeltaHeader,
		(void*)s_origClSendTick, (void*)s_origEngineFrame,
		(void*)s_origHostStateFrame, (void*)s_origHostFrame,
		(void*)s_origClockDrift, (void*)s_origSetupSky,
		(void*)s_origRenderOrchestrator, (void*)s_origMovePostThink);
}


// ClientClass registry, 32B stride: +0x00 CreateFn +0x08 C++ name +0x10 DT name +0x18 null.


//-----------------------------------------------------------------------------
// Write ClientClass name to entity+0x478 if zero. Registry name at +0x08, 32B stride.
//-----------------------------------------------------------------------------


// Global signon state tracker for ExitProcess suppression.
static volatile LONG s_bridgeSignonState = 0;

// [HEARTBEAT] off-main-thread liveness probe removed.

// USERINFO at CClientState+0x21338, 0x131 bytes/slot. NULL early is expected.
typedef char (__fastcall *PFN_EntityDataCopy)(__int64 a1, int a2, void* a3);
static PFN_EntityDataCopy s_origEntityDataCopy = nullptr;

// ExitProcess hook retained ONLY for the load-bearing signon-suppression guard
// (see Hook_ExitProcess). The diagnostic death-interception hooks (exit/_exit/
// _cexit/abort/TerminateProcess/RtlExitUserProcess + stack dumps) were removed.
typedef void (WINAPI *PFN_ExitProcess)(UINT uExitCode);
static PFN_ExitProcess s_origExitProcess = nullptr;

// TerminateProcess backstop. Pass every call through.
typedef BOOL (WINAPI *PFN_TerminateProcess)(HANDLE hProcess, UINT uExitCode);
static PFN_TerminateProcess s_origTerminateProcess = nullptr;

// Image-base VA (0x140000000 + file RVA) for a runtime return address.
unsigned long long DeathObs_ImageVA(void* ret)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base || !ret) return 0;
    return 0x140000000ULL + (reinterpret_cast<uintptr_t>(ret) - base);
}

//-----------------------------------------------------------------------------
// First-chance VEH. CRT-free: stack buffer + WriteFile. Always CONTINUE_SEARCH.
//-----------------------------------------------------------------------------
static HANDLE         s_vehLog   = INVALID_HANDLE_VALUE;
static volatile LONG  s_vehCount = 0;
static uintptr_t      s_vehModBase = 0;   // r5apex base
static uintptr_t      s_vehModEnd  = 0;   // r5apex base + SizeOfImage
static uintptr_t      s_gsdkBase   = 0;   // client.dll base
static uintptr_t      s_gsdkEnd    = 0;   // client.dll base + SizeOfImage

static void Veh_AppendStr(char*& p, char* end, const char* s)
{
    while (*s && p < end) *p++ = *s++;
}
static void Veh_AppendHex(char*& p, char* end, unsigned long long v, int digits)
{
    static const char* k = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0 && p < end; --i)
        *p++ = k[(v >> (i * 4)) & 0xF];
}
// Image VA for a runtime r5apex address (0 if not in the main module).
static unsigned long long Veh_ImageVA(uintptr_t a)
{
    if (s_vehModBase && a >= s_vehModBase && a < s_vehModEnd)
        return 0x140000000ULL + (a - s_vehModBase);
    return 0;
}
// Log r5apex VA (0x140...) or client.dll RVA (g+0x...).
static bool Veh_AppendFrame(char*& p, char* end, uintptr_t a)
{
    const unsigned long long imageVa = Veh_ImageVA(a);
    if (imageVa)
    {
        Veh_AppendStr(p, end, " 0x");
        Veh_AppendHex(p, end, imageVa, 9);
        return true;
    }
    if (s_gsdkBase && a >= s_gsdkBase && a < s_gsdkEnd)
    {
        Veh_AppendStr(p, end, " g+0x");
        Veh_AppendHex(p, end, a - s_gsdkBase, 6);
        return true;
    }
    return false;
}

// Resolved original dbghelp!MiniDumpWriteDump (set at hook install). Declared here so
// Bridge_VEH can call it directly to write its own full-heap dump.
PFN_MiniDumpWriteDump s_origMiniDumpWriteDump = nullptr;

static LONG CALLBACK Bridge_VEH(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const unsigned long code = ep->ExceptionRecord->ExceptionCode;
    switch (code)
    {
        case 0xC0000005UL: // ACCESS_VIOLATION
        case 0xC0000006UL: // IN_PAGE_ERROR
        case 0xC000001DUL: // ILLEGAL_INSTRUCTION
        case 0xC0000094UL: // INTEGER_DIVIDE_BY_ZERO
        case 0xC00000FDUL: // STACK_OVERFLOW
        case 0xC0000374UL: // HEAP_CORRUPTION
        case 0xC0000409UL: // STACK_BUFFER_OVERRUN / __fastfail
        case 0x80000003UL: // BREAKPOINT -- the heap allocator's consistency-fail
                           // LABEL_89 __debugbreak before abort; __fastfail bypasses VEH, int3 does not.
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH; // ignore C++ EH, single-step, etc.
    }
    if (s_vehLog == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* const c = ep->ContextRecord;
    void* const addr = ep->ExceptionRecord->ExceptionAddress;
    const uintptr_t rip = c ? static_cast<uintptr_t>(c->Rip)
                            : reinterpret_cast<uintptr_t>(addr);

    // A WRITE access violation is the corruptor's instruction -- under PageHeap,
    // the guard-page hit AT the bad write. A READ AV is the allocator victim.
    bool isWrite = false;
    uintptr_t faultAddr = 0;
    if (code == 0xC0000005UL && ep->ExceptionRecord->NumberParameters >= 2)
    {
        isWrite   = (ep->ExceptionRecord->ExceptionInformation[0] == 1);
        faultAddr = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    }

    // Drop benign system-DLL READ first-chance AVs. Write-AVs and heap fail always pass.
    const bool ripIsApp = (rip >= s_vehModBase && rip < s_vehModEnd) ||
                          (s_gsdkBase && rip >= s_gsdkBase && rip < s_gsdkEnd);
    if (code == 0xC0000005UL && !isWrite && !ripIsApp)
        return EXCEPTION_CONTINUE_SEARCH;

    const LONG n = InterlockedIncrement(&s_vehCount);
    if (n > 512) return EXCEPTION_CONTINUE_SEARCH;

    char buf[1024];
    char* p = buf;
    char* const end = buf + sizeof(buf);
    Veh_AppendStr(p, end, isWrite ? "[VEH-WRITER] tid=0x" : "[VEH-CRASH] tid=0x");
    Veh_AppendHex(p, end, GetCurrentThreadId(), 8);
    Veh_AppendStr(p, end, " code=0x");
    Veh_AppendHex(p, end, code, 8);
    Veh_AppendStr(p, end, " addr_va=0x");
    Veh_AppendHex(p, end, Veh_ImageVA(reinterpret_cast<uintptr_t>(addr)), 12);
    if (s_gsdkBase && reinterpret_cast<uintptr_t>(addr) >= s_gsdkBase &&
        reinterpret_cast<uintptr_t>(addr) < s_gsdkEnd)
    {
        Veh_AppendStr(p, end, " ripg+0x"); // faulting RIP is in the bridge
        Veh_AppendHex(p, end, reinterpret_cast<uintptr_t>(addr) - s_gsdkBase, 6);
    }
    Veh_AppendStr(p, end, " rawaddr=0x");
    Veh_AppendHex(p, end, reinterpret_cast<unsigned long long>(addr), 16);
    if (code == 0xC0000005UL)
    {
        // ExceptionInformation[1] is the invalid VA.
        Veh_AppendStr(p, end, isWrite ? " wrote@0x" : " readat@0x");
        Veh_AppendHex(p, end, faultAddr, 16);
    }

    // Faulting registers + a stack walk (r5apex addrs AND client.dll RVAs) for
    // the first few faults AND every write-AV -- names the corruptor's call chain.
    // Reading the thread's own stack is safe (always mapped).
    if (c && (isWrite || n <= 6))
    {
        Veh_AppendStr(p, end, "\n[VEH-CRASH]   rip=0x");
        Veh_AppendHex(p, end, c->Rip, 16);
        Veh_AppendStr(p, end, " rcx=0x");      Veh_AppendHex(p, end, c->Rcx, 16);
        Veh_AppendStr(p, end, " rdx=0x");      Veh_AppendHex(p, end, c->Rdx, 16);
        Veh_AppendStr(p, end, " r8=0x");       Veh_AppendHex(p, end, c->R8, 16);
        // punpcklwd xmm0,[r11]: r14=ctx, r9=node base, r11=r9+nodeIdx*64.
        Veh_AppendStr(p, end, " r9=0x");       Veh_AppendHex(p, end, c->R9, 16);
        Veh_AppendStr(p, end, " r11=0x");      Veh_AppendHex(p, end, c->R11, 16);
        Veh_AppendStr(p, end, " r14=0x");      Veh_AppendHex(p, end, c->R14, 16);
        Veh_AppendStr(p, end, "\n[VEH-CRASH]   stack(va):");
        // Walk only [gs:0x10, gs:0x08). Worker Rsp can sit at the stack top.
        const uintptr_t stackTop = static_cast<uintptr_t>(__readgsqword(0x08));
        const uintptr_t stackBot = static_cast<uintptr_t>(__readgsqword(0x10));
        const uintptr_t rsp = static_cast<uintptr_t>(c->Rsp);
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(rsp);
        int found = 0;
        if (rsp >= stackBot && rsp < stackTop)
        {
            for (int i = 0; i < 512 && found < 16 && p < end - 16; ++i)
            {
                if (reinterpret_cast<uintptr_t>(&sp[i]) + sizeof(uintptr_t) > stackTop)
                    break; // next qword would cross the stack top -> stop
                if (Veh_AppendFrame(p, end, sp[i]))
                    ++found;
            }
        }
    }
    if (p < end) *p++ = '\n';

    DWORD wrote = 0;
    WriteFile(s_vehLog, buf, static_cast<DWORD>(p - buf), &wrote, nullptr);
    FlushFileBuffers(s_vehLog);

    // One-shot dump of rcx/rdx/rip neighborhood on allocator AV/breakpoint.
    static volatile LONG s_vehDumped = 0;
    if (c && InterlockedExchange(&s_vehDumped, 1) == 0)
    {
        __try
        {
            Bridge_RawDumpRegion("platform\\veh_smash_rdx.bin", (uintptr_t)c->Rdx - 0x80, 0x280);
            Bridge_RawDumpRegion("platform\\veh_smash_rcx.bin", (uintptr_t)c->Rcx - 0x80, 0x280);
            Bridge_RawDumpRegion("platform\\veh_smash_rip.bin", (uintptr_t)c->Rip - 0x40, 0x100);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Single-step VEH for the DR0 hit. Raw-logs (CRT-free) the writer RIP + regs +
// stack to veh_crash.log, acknowledges DR6, and CONTINUES so decode proceeds.
static LONG CALLBACK Bridge_HwWatchVEH(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != (DWORD)EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    if ((ep->ContextRecord->Dr6 & 1ULL) == 0)         // DR0 didn't fire
        return EXCEPTION_CONTINUE_SEARCH;

    const LONG hit = InterlockedIncrement(&s_hwHit);
    CONTEXT* const c = ep->ContextRecord;

    // Trap fires after the store. val0 = +0x328 m_iHealth; val1 = +0x32C (not maxhealth).
    uint32_t val0 = 0xBADC0DE0u, val1 = 0xBADC0DE1u;
    __try { val0 = *(volatile uint32_t*)s_hwAddr; val1 = *(volatile uint32_t*)(s_hwAddr + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Skip construction memset (rep stosb). Real applies are mov via RecvProp.
    const unsigned long long ripVa = Veh_ImageVA((uintptr_t)c->Rip);
    if (static_cast<uintptr_t>(c->Rip) == NetObs_Sym(NetObsSym_t::HwWatchMemsetRip))
    {
        s_hwLastVal = val0; s_hwLastHi = val1;
        c->Dr6 &= ~1ULL;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // [STUDIO-FIELD kind=10] The watched field is an 8-byte heap pointer (the model anim-array
    // base). Legit per-model sets write a CANONICAL pointer; the bug writes POISON. Skip the
    // canonical writes so the log captures ONLY the corruptor/reuser (the non-canonical store).
    if (s_hwWatchKind == 10)
    {
        const uint64_t wv = ((uint64_t)val1 << 32) | (uint64_t)val0;
        if (Bridge_IsCanonPtr(wv))
        {
            s_hwLastVal = val0; s_hwLastHi = val1;
            c->Dr6 &= ~1ULL;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    // [PTRW-WATCH kind=11] filter removed with sdk_hw_watch_ptr / sdk_hw_watch_ptr_heaponly --
    // nothing arms kind=11 anymore.
    const bool changed = (val0 != s_hwLastVal) || (val1 != s_hwLastHi);

    if (s_vehLog != INVALID_HANDLE_VALUE && s_hwLogged < kHwMaxHits)   // log every non-memset write
    {
        InterlockedIncrement(&s_hwLogged);
        char buf[1024]; char* p = buf; char* const end = buf + sizeof(buf);
        Veh_AppendStr(p, end, "[HW-WATCH] kind="); Veh_AppendHex(p, end, (unsigned)s_hwWatchKind, 1);
        Veh_AppendStr(p, end, " hit#"); Veh_AppendHex(p, end, (unsigned)hit, 2);
        Veh_AppendStr(p, end, changed ? " CHANGED" : " same   ");
        Veh_AppendStr(p, end, " val0=0x"); Veh_AppendHex(p, end, val0, 8);
        Veh_AppendStr(p, end, " val1=0x"); Veh_AppendHex(p, end, val1, 8);
        if (hit > 1)
        {
            Veh_AppendStr(p, end, " prev0=0x"); Veh_AppendHex(p, end, s_hwLastVal, 8);
            Veh_AppendStr(p, end, " prev1=0x"); Veh_AppendHex(p, end, s_hwLastHi, 8);
        }
        Veh_AppendStr(p, end, " writerRIP_va=0x"); Veh_AppendHex(p, end, ripVa, 12);
        Veh_AppendStr(p, end, " raw=0x"); Veh_AppendHex(p, end, c->Rip, 16);
        Veh_AppendStr(p, end, " watch=0x"); Veh_AppendHex(p, end, s_hwAddr, 16);
        Veh_AppendStr(p, end, "\n[HW-WATCH]   rax=0x"); Veh_AppendHex(p, end, c->Rax, 16);
        Veh_AppendStr(p, end, " rcx=0x"); Veh_AppendHex(p, end, c->Rcx, 16);
        Veh_AppendStr(p, end, " rdx=0x"); Veh_AppendHex(p, end, c->Rdx, 16);
        Veh_AppendStr(p, end, " r8=0x");  Veh_AppendHex(p, end, c->R8, 16);
        Veh_AppendStr(p, end, " rsi=0x"); Veh_AppendHex(p, end, c->Rsi, 16);
        Veh_AppendStr(p, end, " rdi=0x"); Veh_AppendHex(p, end, c->Rdi, 16);
        Veh_AppendStr(p, end, "\n[HW-WATCH]   stack(va):");
        __try {
            const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
            int found = 0;
            for (int i = 0; i < 64 && found < 16 && p < end - 16; ++i)
            {
                const unsigned long long imageVa = Veh_ImageVA(sp[i]);
                if (imageVa) { Veh_AppendStr(p, end, " 0x"); Veh_AppendHex(p, end, imageVa, 9); ++found; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (p < end) *p++ = '\n';
        DWORD wrote = 0; WriteFile(s_vehLog, buf, (DWORD)(p - buf), &wrote, nullptr);
        FlushFileBuffers(s_vehLog);
    }

    s_hwLastVal = val0; s_hwLastHi = val1;
    c->Dr6 &= ~1ULL;                                  // ack the trap
    if (s_hwLogged >= kHwMaxHits)
    {
        c->Dr0 = 0; c->Dr7 = 0;                       // disable in-context
        InterlockedExchange(&s_hwCaptured, 1);
        s_hwAddr = 0;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void Bridge_InstallVEH()
{
    if (s_vehLog == INVALID_HANDLE_VALUE)
    {
        // Relative to the game CWD (the s21-full dir) -> platform\veh_crash.log.
        s_vehLog = CreateFileA("platform\\veh_crash.log",
            FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    // Cache the r5apex module range so the VEH can map raw addresses to
    // addrs and recognise r5apex return addresses on the stack.
    {
        HMODULE hExe = GetModuleHandleW(nullptr);
        MODULEINFO mi{};
        if (hExe && GetModuleInformation(GetCurrentProcess(), hExe, &mi, sizeof(mi)))
        {
            s_vehModBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            s_vehModEnd  = s_vehModBase + mi.SizeOfImage;
        }
        // client.dll range too, so the VEH can name a BRIDGE writer's frames
        // (g+0xRVA) instead of dropping them -- essential under PageHeap.
        HMODULE hSdk = GetModuleHandleA("client.dll");
        MODULEINFO si{};
        if (hSdk && GetModuleInformation(GetCurrentProcess(), hSdk, &si, sizeof(si)))
        {
            s_gsdkBase = reinterpret_cast<uintptr_t>(si.lpBaseOfDll);
            s_gsdkEnd  = s_gsdkBase + si.SizeOfImage;
        }
    }
    AddVectoredExceptionHandler(1 /*first*/, &Bridge_HwWatchVEH); // DR0 single-step (first)
    AddVectoredExceptionHandler(1 /*first*/, &Bridge_VEH);
    SDK_Log("[VEH-CRASH] vectored crash observer installed (log=platform\\veh_crash.log, handle=%p)\n",
        (void*)s_vehLog);
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static void FormatSockaddr(const sockaddr* addr, char* buf, size_t bufSize)
{
	if (!addr)
	{
		strncpy_s(buf, bufSize, "<null>", _TRUNCATE);
		return;
	}

	if (addr->sa_family == AF_INET)
	{
		const sockaddr_in* const sin = reinterpret_cast<const sockaddr_in*>(addr);
		const unsigned char* const ip = reinterpret_cast<const unsigned char*>(&sin->sin_addr);
		_snprintf_s(buf, bufSize, _TRUNCATE, "%u.%u.%u.%u:%u",
			ip[0], ip[1], ip[2], ip[3], (unsigned)ntohs(sin->sin_port));
	}
	else if (addr->sa_family == AF_INET6)
	{
		const sockaddr_in6* const sin6 = reinterpret_cast<const sockaddr_in6*>(addr);
		const unsigned char* const ip = reinterpret_cast<const unsigned char*>(&sin6->sin6_addr);

		// Detect IPv4-mapped IPv6 (::ffff:a.b.c.d) and IPv4-compatible
		// (::0:0:a.b.c.d). For loopback::1 show as "::1". For anything
		// else, full 16-byte hex pairs plus port.
		bool isV4Mapped = true;
		for (int i = 0; i < 10; ++i) if (ip[i]) { isV4Mapped = false; break; }
		if (isV4Mapped && ip[10] == 0xFF && ip[11] == 0xFF)
		{
			_snprintf_s(buf, bufSize, _TRUNCATE, "[::ffff:%u.%u.%u.%u]:%u",
				ip[12], ip[13], ip[14], ip[15],
				(unsigned)ntohs(sin6->sin6_port));
		}
		else
		{
			// Compact IPv6 representation - we just print all 8 groups
			// without RFC 4291 zero-compression because this is for
			// diagnostics and clarity beats brevity.
			_snprintf_s(buf, bufSize, _TRUNCATE,
				"[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]:%u",
				ip[0],  ip[1],  ip[2],  ip[3],  ip[4],  ip[5],  ip[6],  ip[7],
				ip[8],  ip[9],  ip[10], ip[11], ip[12], ip[13], ip[14], ip[15],
				(unsigned)ntohs(sin6->sin6_port));
		}
	}
	else
	{
		_snprintf_s(buf, bufSize, _TRUNCATE, "<family=%d>", (int)addr->sa_family);
	}
}

// Cap sendto logs: first N per dest, then 1/256.
struct DestCounter
{
	uint8_t  family;    // AF_INET or AF_INET6
	uint8_t  addr[16];  // AF_INET uses the first 4 bytes; AF_INET6 uses all 16
	uint16_t port;
	uint32_t seen;
};

static constexpr size_t kMaxTrackedDests = 16;
static constexpr uint32_t kVerboseFirstN = 12;
static DestCounter s_destTable[kMaxTrackedDests] = {};
static size_t s_destCount = 0;

// Dest tracker is AF_INET6 (game socket is v6-mapped). Lock-free; races are OK.
static uint32_t BumpDest(const sockaddr* addr)
{
	if (!addr)
		return 1;

	uint8_t family = (uint8_t)addr->sa_family;
	uint8_t key[16] = {};
	uint16_t port = 0;

	if (addr->sa_family == AF_INET)
	{
		const sockaddr_in* const sin = reinterpret_cast<const sockaddr_in*>(addr);
		memcpy(key, &sin->sin_addr, sizeof(sin->sin_addr));
		port = sin->sin_port;
	}
	else if (addr->sa_family == AF_INET6)
	{
		const sockaddr_in6* const sin6 = reinterpret_cast<const sockaddr_in6*>(addr);
		memcpy(key, &sin6->sin6_addr, sizeof(sin6->sin6_addr));
		port = sin6->sin6_port;
	}
	else
	{
		return 1; // genuinely rare family -- keep always-verbose
	}

	for (size_t i = 0; i < s_destCount; ++i)
	{
		if (s_destTable[i].family == family && s_destTable[i].port == port &&
			memcmp(s_destTable[i].addr, key, sizeof(key)) == 0)
			return ++s_destTable[i].seen;
	}
	if (s_destCount < kMaxTrackedDests)
	{
		s_destTable[s_destCount].family = family;
		memcpy(s_destTable[s_destCount].addr, key, sizeof(key));
		s_destTable[s_destCount].port = port;
		s_destTable[s_destCount].seen = 1;
		++s_destCount;
	}
	return 1;
}

//-----------------------------------------------------------------------------
// Quote `connect ip:port` so Tokenize does not split on ':'. +cvar batches too.
//-----------------------------------------------------------------------------
static const char* FindConnectVerb(const char* pText)
{
	if (!pText || !*pText)
		return nullptr;

	bool atLine = true;
	for (const char* s = pText; *s; ++s)
	{
		if (*s == ' ' || *s == '\t')
			continue;
		if (atLine && strncmp(s, "connect", 7) == 0
			&& (s[7] == ' ' || s[7] == '\t'))
			return s;
		if (*s == '\n' || *s == '\r' || *s == ';')
			atLine = true;
		else
			atLine = false;
	}
	return nullptr;
}

static bool ExtractConnectAddr(const char* pVerb, char* addr, size_t addrLen)
{
	if (!pVerb || !addr || addrLen < 2)
		return false;

	const char* pAddr = pVerb + 7;
	while (*pAddr == ' ' || *pAddr == '\t')
		++pAddr;
	if (*pAddr == '"')
		++pAddr;

	size_t n = 0;
	while (pAddr[n] && n + 1 < addrLen
		&& pAddr[n] != '"' && pAddr[n] != ';'
		&& pAddr[n] != '\n' && pAddr[n] != '\r'
		&& pAddr[n] != ' ' && pAddr[n] != '\t')
	{
		++n;
	}
	if (n == 0)
		return false;
	memcpy(addr, pAddr, n);
	addr[n] = '\0';
	return true;
}

static bool RewriteConnectIfNeeded(const char* pText, char* pOutBuf, size_t outBufSize)
{
	if (!pText || !*pText)
		return false;

	const char* pVerb = FindConnectVerb(pText);
	if (!pVerb)
		return false;

	if (strchr(pVerb + 7, '"'))
		return false;

	if (!strchr(pVerb + 7, ':'))
		return false;

	const char* pAddrStart = pVerb + 7;
	while (*pAddrStart == ' ' || *pAddrStart == '\t')
		++pAddrStart;

	const char* pAddrEnd = pAddrStart;
	while (*pAddrEnd && *pAddrEnd != '\n' && *pAddrEnd != '\r' && *pAddrEnd != ';')
		++pAddrEnd;

	const size_t addrLen = static_cast<size_t>(pAddrEnd - pAddrStart);
	if (addrLen == 0 || addrLen > 256)
		return false;

	const size_t prefixLen = static_cast<size_t>(pVerb - pText);
	const int n = _snprintf_s(pOutBuf, outBufSize, _TRUNCATE,
		"%.*sconnect \"%.*s\"%s",
		static_cast<int>(prefixLen), pText,
		static_cast<int>(addrLen), pAddrStart, pAddrEnd);
	if (n <= 0)
		return false;

	return true;
}

static void Cbuf_RunPrefixDropConnect(ECommandTarget_t eTarget, cmd_source_t cmdSource,
	const char* pFinal, const char* pVerb)
{
	if (!pFinal || !pVerb || pVerb <= pFinal)
		return;

	char prefix[1024];
	const size_t n = static_cast<size_t>(pVerb - pFinal);
	if (n == 0 || n >= sizeof(prefix))
		return;
	memcpy(prefix, pFinal, n);
	prefix[n] = '\0';
	size_t end = n;
	while (end > 0 && (prefix[end - 1] == ' ' || prefix[end - 1] == '\t'
		|| prefix[end - 1] == '\n' || prefix[end - 1] == '\r' || prefix[end - 1] == ';'))
	{
		prefix[--end] = '\0';
	}
	if (end > 0)
		s_origCbufAddText(eTarget, prefix, cmdSource);
}

static void Hook_Cbuf_AddText(ECommandTarget_t eTarget, const char* pText, cmd_source_t cmdSource)
{
	// Strip trailing newline; also mirror to the overlay via SDK_Log.
	if (pText && *pText)
	{
		size_t n = strlen(pText);
		while (n > 0 && (pText[n - 1] == '\n' || pText[n - 1] == '\r'))
			--n;
		char tmp[512];
		const size_t copyLen = n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1;
		memcpy(tmp, pText, copyLen);
		tmp[copyLen] = '\0';
		NetObs_LogPacket("Cbuf_AddText tgt=%d src=%d: %s\n",
			(int)eTarget, (int)cmdSource, tmp);
		SDK_Log("[NET-OBS] Cbuf_AddText: %s\n", tmp);

		if (V_strnicmp(tmp, "disconnect", 10) == 0
			&& (tmp[10] == '\0' || tmp[10] == ' ' || tmp[10] == '\t' || tmp[10] == ';'))
			S21Bridge_OnSessionEnded("disconnect cmd");
		if (V_strnicmp(tmp, "connect", 7) == 0
			&& (tmp[7] == '\0' || tmp[7] == ' ' || tmp[7] == '\t'))
		{
			Warning(eDLL_T::ENGINE, "[BRIDGE-CONNECT] Cbuf connect: '%s' src=%d\n",
				tmp, (int)cmdSource);
		}
	}

	// Rewrite unquoted connect ip:port -> quoted. Other commands unchanged.
	char rewriteBuf[1024];
	const char* pFinal = pText;
	if (RewriteConnectIfNeeded(pText, rewriteBuf, sizeof(rewriteBuf)))
	{
		SDK_Log("[NET-OBS] Cbuf_AddText REWRITE -> %s\n", rewriteBuf);
		pFinal = rewriteBuf;
	}

	// Mint join token at the single connect chokepoint (browser / console /
	// Reconnect / +connect after other +cvars). Blocking HTTPS -- only when
	// a command is queued, never on a per-frame path.
	const char* pVerb = pFinal ? FindConnectVerb(pFinal) : nullptr;
	char addr[256];
	if (pVerb && ExtractConnectAddr(pVerb, addr, sizeof(addr)))
	{
		if (Bridge_ShouldSuppressConnect(addr))
		{
			Msg(eDLL_T::ENGINE,
				"[JOIN-AUTH] duplicate connect to '%s' swallowed\n", addr);
			Cbuf_RunPrefixDropConnect(eTarget, cmdSource, pFinal, pVerb);
			return;
		}

		Bridge_LogJoinAuthState(addr);

		char reason[512];
		reason[0] = '\0';
		if (!Bridge_EnsureJoinToken(addr, reason, sizeof(reason)))
		{
			Cbuf_RunPrefixDropConnect(eTarget, cmdSource, pFinal, pVerb);
			if (Bridge_JoinAuthWasDeferred())
			{
				Msg(eDLL_T::ENGINE,
					"[JOIN-AUTH] connect to '%s' waiting on platform sign-in; will start on its own\n",
					addr);
				return;
			}
			Warning(eDLL_T::ENGINE,
				"[JOIN-AUTH] connect to '%s' refused: %s\n",
				addr, reason[0] ? reason : "unknown");
			if (Bridge_IsTrueLoopbackHost(addr) && !Bridge_JoinAuthBlocksOnFailure())
			{
				Warning(eDLL_T::ENGINE,
					"[JOIN-AUTH] loopback connect proceeding without token\n");
				S21Bridge_ResetForNewConnect();
				s_origCbufAddText(eTarget, pFinal, cmdSource);
				Bridge_NoteConnectDispatched(addr);
				return;
			}
			return;
		}

		S21Bridge_ResetForNewConnect();
		s_origCbufAddText(eTarget, pFinal, cmdSource);
		Bridge_NoteConnectDispatched(addr);
		return;
	}

	s_origCbufAddText(eTarget, pFinal, cmdSource);
}

// Instrumentation counter: only log a few Cbuf_Execute calls so we can
// confirm the engine's main-loop command buffer is actually running
// without flooding the log at ~60 fps.
static volatile long long s_cbufExecuteCount = 0;

//-----------------------------------------------------------------------------
// RSS sampler across signon / Cbuf / DataBlock.
//-----------------------------------------------------------------------------
static uint64_t SDK_GetRSSBytes()
{
    PROCESS_MEMORY_COUNTERS pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (uint64_t)pmc.WorkingSetSize;
    return 0;
}

static void SDK_LogRSS(const char* tag)
{
    uint64_t rss = SDK_GetRSSBytes();
    SDK_Log("[SPAWN-MEM] %s rss=%llu MB (%.2f GB)\n",
        tag, (unsigned long long)(rss / (1024 * 1024)),
        (double)rss / (1024.0 * 1024.0 * 1024.0));
}

// Defined later; called from the main-thread drain below.
void S21Bridge_OnDataBlockComplete(const uint8_t* rawBuf, int rawSize);

static void Hook_Cbuf_Execute(void)
{
	// Drain deferred signon DataBlock on the main thread. Copy, clear flag, then process.
	if (s_pendingSignonReady)
	{
		if (!s_processingSignonBuf)
		{
			s_processingSignonBuf = static_cast<uint8_t*>(
				malloc(BRIDGE_DB_SCRATCH_SIZE + HeapCanary::kTailBytes));
			if (s_processingSignonBuf)
				HeapCanary::RegisterTail("bridge-processing-signon",
					s_processingSignonBuf, BRIDGE_DB_SCRATCH_SIZE);
		}
		const int sz = s_pendingSignonSize;
		if (s_processingSignonBuf && sz > 0 && sz <= BRIDGE_DB_SCRATCH_SIZE)
		{
			memcpy(s_processingSignonBuf, s_pendingSignonBuf, sz);
			s_pendingSignonReady = false;
			S21Bridge_OnDataBlockComplete(s_processingSignonBuf, sz);
		}
		else
		{
			s_pendingSignonReady = false;
		}
	}

	static int s_cbufHookDepth = 0;
	++s_cbufHookDepth;
	if (s_cbufHookDepth == 1)
		Bridge_PumpDeferredConnect();

	const long long n = ++s_cbufExecuteCount;
	if (SDK_OBSERVE_NET_GE(1) && (n <= 3 || (n % 600) == 0))
	{
		SDK_Log("[NET-OBS] Cbuf_Execute call #%lld\n", n);
	}

	// Sample RSS every 30 ticks (~0.5 sec at 60 Hz). The first ramp identifies
	// the leak window. Gated: per-tick stream is firehose.
	if (Bridge_DiagFirehoseEnabled() && (n % 30) == 0)
	{
		char tickTag[48];
		snprintf(tickTag, sizeof(tickTag), "Cbuf_Execute tick #%lld", n);
		SDK_LogRSS(tickTag);
	}

	s_origCbufExecute();
	--s_cbufHookDepth;
}

// Connect worker: last hook before client-state dispatch. CClientState RVA 0x17E8B70,
// vtable slot 11 at +0x58.
static void Hook_Connect_Worker(void** addrPtrPtr)
{
	const char* addrStr = nullptr;
	if (addrPtrPtr && *addrPtrPtr)
	{
		addrStr = reinterpret_cast<const char*>(*addrPtrPtr);
		SDK_Log("[NET-OBS] Connect_Worker addr='%s'\n", addrStr);
	}
	else
	{
		SDK_Log("[NET-OBS] Connect_Worker addr=<null>\n");
	}

	// Tear down a live bridge before a new `connect` so handshake state is clean.
	if (s_bridgeActive || s_bridgeSocket != INVALID_SOCKET ||
	    s_hsStage != BridgeHsStage::Idle)
	{
		SDK_Log("[NET-OBS] Connect_Worker: stale bridge state detected "
			"(active=%d socket=%lld stage=%d) -- forcing teardown before "
			"new connect attempt\n",
			s_bridgeActive ? 1 : 0,
			(long long)s_bridgeSocket,
			(int)s_hsStage);
		if (s_bridgeSocket != INVALID_SOCKET)
		{
			closesocket(s_bridgeSocket);
			s_bridgeSocket = INVALID_SOCKET;
		}
		s_bridgeActive = false;
		s_hsStage = BridgeHsStage::Idle;
		s_lastBypassAttempt = 0;
		S21Bridge_ResetAllState();
	}

	// Authoritative connect target from the address string. Must run before
	// the first C2S_CHALLENGE so S21Bridge_ResolveDest never falls back to
	// 127.0.0.1 for a public VPS host:port (or any non-default hostport).
	Bridge_ClearConnectTarget();
	if (addrStr && addrStr[0])
	{
		if (!Bridge_SetConnectTargetFromString(addrStr))
		{
			SDK_Log("[NET-OBS] Connect_Worker: failed to resolve connect target "
				"'%s' -- handshake may fall back to loopback\n", addrStr);
		}
		else
		{
			char db[96];
			FormatSockaddr(reinterpret_cast<const sockaddr*>(&s_bridgeDest),
				db, sizeof(db));
			SDK_Log("[NET-OBS] Connect_Worker: bridgeDest=%s port=%u loopback=%d\n",
				db, (unsigned)s_connectTargetPort,
				s_connectTargetIsLoopback ? 1 : 0);
		}
	}

	// Dump the runtime state of the CClientState singleton and its vtable.
	{
		if (NetObs_GetExeModuleBase())
		{
			void** const pClientStateArray =
				reinterpret_cast<void**>(NetObs_ClientStateSingletonSlotAddr());
			void* const pVtable = *pClientStateArray;
			SDK_Log("[NET-OBS]   CClientState* = %p (runtime addr)\n",
				(void*)pClientStateArray);
			SDK_Log("[NET-OBS]   vtable        = %p\n", pVtable);
			if (pVtable)
			{
				void** const vtbl = reinterpret_cast<void**>(pVtable);
				// Slot 11 = +0x58 bytes. Dump a few slots around it
				// so we can see the neighborhood in.
				SDK_Log("[NET-OBS]   vtable[ 9]    = %p\n", vtbl[9]);
				SDK_Log("[NET-OBS]   vtable[10]    = %p\n", vtbl[10]);
				SDK_Log("[NET-OBS]   vtable[11]    = %p  <-- Apex connect dispatcher\n", vtbl[11]);
				SDK_Log("[NET-OBS]   vtable[12]    = %p\n", vtbl[12]);
				SDK_Log("[NET-OBS]   vtable[13]    = %p\n", vtbl[13]);
			}
		}
	}

	s_origConnectWorker(addrPtrPtr);

	// The Inner 0x48 sniff is not a connect chokepoint: after disconnect the
	// engine may not emit another C2S_CHALLENGE (it already has dest 0x04).
	if (!s_bridgeActive && s_hsStage == BridgeHsStage::Idle)
	{
		if (S21Bridge_StartHandshake())
			Msg(eDLL_T::ENGINE, "[BRIDGE] handshake start (connect)\n");
	}

	SDK_Log("[NET-OBS] Connect_Worker returned\n");
}

// Cmd_ExecuteString. CCommand: +0x04 argc +0x10 raw +0x410 argv[0]. RVA 0x2232A0.
static __int64 Hook_Cmd_ExecuteString(unsigned int player, void* parsedCmd, int source)
{
	if (parsedCmd)
	{
		const char* const rawStr = reinterpret_cast<const char*>(parsedCmd) + 0x10;
		const int argc = *reinterpret_cast<const int*>(
			reinterpret_cast<const char*>(parsedCmd) + 0x04);
		// argv[0] lives at offset 0x410 (1040) - first pointer in the
		// argv array (which is at the end of CCommand). Bounds-check
		// argc to avoid reading junk if we got a weird CCommand.
		const char* cmdName = "<empty>";
		if (argc > 0)
		{
			const char* const* const argv = reinterpret_cast<const char* const*>(
				reinterpret_cast<const char*>(parsedCmd) + 0x410);
			if (*argv)
				cmdName = *argv;
		}
		SDK_Log("[NET-OBS] Cmd_ExecuteString player=%u src=%d argc=%d cmd='%s'\n",
			player, source, argc, cmdName);
		// Log the full raw string too, for the commands we care about.
		SDK_Log("[NET-OBS]   raw=\"%.256s\"\n", rawStr);

		// Native `]` console never hits ImGui IConsole. When the host RCON
		// session is live, dedi-owned verbs must not run as stringcmd.
		if (argc > 0 && RCON_LauncherClient_Ready()
			&& RCON_LauncherClient_ShouldForward(cmdName))
		{
			char szLine[512];
			size_t n = 0;
			while (n + 1 < sizeof(szLine) && rawStr[n]
				&& rawStr[n] != '\n' && rawStr[n] != '\r')
			{
				szLine[n] = rawStr[n];
				++n;
			}
			szLine[n] = '\0';
			if (n > 0 && RCON_LauncherClient_QueueExec(szLine))
			{
				Msg(eDLL_T::CLIENT, "[BRIDGE-CONSOLE] '%s' -> dedi via host RCON\n", cmdName);
				return 0;
			}
		}

	}
	else
	{
		SDK_Log("[NET-OBS] Cmd_ExecuteString player=%u src=%d parsedCmd=NULL\n",
			player, source);
	}

	const __int64 ret = s_origCmdExecuteString(player, parsedCmd, source);
	SDK_Log("[NET-OBS] Cmd_ExecuteString returned 0x%llX\n", (unsigned long long)ret);
	return ret;
}

// WSASocketW: IPV6_V6ONLY=0 on AF_INET6 DGRAM, before bind (later is WSAEINVAL).
static SOCKET WSAAPI Hook_WSASocketW(int af, int type, int protocol,
                                     LPWSAPROTOCOL_INFOW lpProtocolInfo,
                                     GROUP g, DWORD dwFlags)
{
	const SOCKET sock = s_origWSASocketW(af, type, protocol, lpProtocolInfo, g, dwFlags);

	if (sock != INVALID_SOCKET && af == AF_INET6 && type == SOCK_DGRAM)
	{
		DWORD v6only = 0;
		int r = setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
			reinterpret_cast<const char*>(&v6only), sizeof(v6only));
		if (SDK_OBSERVE_NET_GE(1))
		{
			SDK_Log("[NET-OBS] WSASocketW(AF_INET6, DGRAM) -> socket=%llu, SET IPV6_V6ONLY=0: %s\n",
				(unsigned long long)sock, r == 0 ? "OK" : "FAILED");
		}
	}

	return sock;
}

static int WSAAPI Hook_sendto(SOCKET s, const char* buf, int len, int flags,
                              const sockaddr* to, int tolen)
{
	const uint32_t seen = BumpDest(to);
	if (seen <= kVerboseFirstN || (seen % 256) == 0)
	{
		char addrBuf[64];
		FormatSockaddr(to, addrBuf, sizeof(addrBuf));

		unsigned char head[8] = {};
		const int copyLen = len < 8 ? len : 8;
		if (buf && copyLen > 0)
			memcpy(head, buf, copyLen);

		NetObs_LogPacket("sendto[%u] dst=%s len=%d head=%02x%02x%02x%02x%02x%02x%02x%02x\n",
			(unsigned)seen, addrBuf, len,
			head[0], head[1], head[2], head[3],
			head[4], head[5], head[6], head[7]);
	}

	// Track engine peer + real bridge dest. Connect_Worker seeds the dest from
	// the connect string (remote VPS). sendto only refines it; loopback sends
	// never demote a resolved public dest (see Bridge_NoteEngineServerSendto).
	if (to && tolen > 0)
		Bridge_NoteEngineServerSendto(to, tolen);

	// Relay NA_IP sendto(loopback:gameport) through the bridge socket to s_bridgeDest.
	if (s_bridgeActive && s_bridgeSocket != INVALID_SOCKET && to && tolen > 0)
	{
		const uint16_t destPort = Bridge_SockaddrPortHost(to);

		if (Bridge_IsBridgeGamePort(destPort))
		{
			const unsigned char* pkt = reinterpret_cast<const unsigned char*>(buf);
			const bool isOOB = (len >= 4 && pkt[0] == 0xFF && pkt[1] == 0xFF
				&& pkt[2] == 0xFF && pkt[3] == 0xFF);

			// The dedi's delta-ack (CClient::m_nDeltaAckTick) comes from the native
			// clc_Move/clc_ClientTick relay (bridge_c2s_clc_move / _clc_tick); no
			// OOB tick report is sent here -- the dedi discards OOB acks unread.

			// Listen-server sendto: forward split (0xFFFFFFFE) and OOB (0xFFFFFFFF); drop netchan.
			const bool isSplitPacket = (len >= 4 && pkt[0] == 0xFE
				&& pkt[1] == 0xFF && pkt[2] == 0xFF && pkt[3] == 0xFF);

			int sent;
			if (isSplitPacket)
			{
				// Do not queue split packets from this hook: they are S21-format C2S, not S3 S2C.
				static long long s_splitSuppressed = 0;
				if (++s_splitSuppressed <= 10 || (s_splitSuppressed % 500) == 0)
					SDK_Log("[BRIDGE-OUT] #%lld: suppressed native split C2S %d bytes (reqID=%d)\n",
						s_splitSuppressed, len,
						len >= 8 ? *reinterpret_cast<const int32_t*>(pkt + 4) : -1);

				sent = len; // consumed (suppressed)
			}
			else if (!isOOB && len >= 9)
			{
				// Regular netchan -> suppress (S21 engine C2S, replaced by self-clock).
				// C2S still flows via the 50ms self-clock.
				sent = len; // pretend success (consumed)
			}
			else
			{
				// OOB or tiny packet -- forward as-is
				sent = s_origSendto(s_bridgeSocket, buf, len, 0,
					reinterpret_cast<const sockaddr*>(&s_bridgeDest),
					sizeof(s_bridgeDest));
			}

			static long long s_rewriteCount = 0;
			if (++s_rewriteCount <= 10 || (s_rewriteCount % 200) == 0)
			{
				SDK_Log("[BRIDGE-OUT] #%lld: %s len=%d sent=%d\n",
					s_rewriteCount, isOOB ? "OOB" : "netchan", len, sent);
			}

			static long long s_outRelayCount = 0;
			if (++s_outRelayCount <= 5 || (s_outRelayCount % 100) == 0)
				NetObs_LogPacket("BRIDGE OUT #%lld: relayed %d/%d bytes to S3 server\n",
					s_outRelayCount, sent, len);

			return len;
		}
	}

	return s_origSendto(s, buf, len, flags, to, tolen);
}

static int WSAAPI Hook_WSASendTo(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
                                 LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
                                 const sockaddr* lpTo, int iTolen,
                                 LPWSAOVERLAPPED lpOverlapped,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	if (lpTo && iTolen > 0)
		Bridge_NoteEngineServerSendto(lpTo, iTolen);

	const uint32_t seen = BumpDest(lpTo);
	if (seen <= kVerboseFirstN || (seen % 256) == 0)
	{
		char addrBuf[64];
		FormatSockaddr(lpTo, addrBuf, sizeof(addrBuf));

		DWORD totalSize = 0;
		for (DWORD i = 0; i < dwBufferCount; ++i)
			totalSize += lpBuffers[i].len;

		unsigned char head[8] = {};
		if (dwBufferCount > 0 && lpBuffers[0].buf && lpBuffers[0].len > 0)
		{
			const DWORD copyLen = lpBuffers[0].len < 8 ? lpBuffers[0].len : 8;
			memcpy(head, lpBuffers[0].buf, copyLen);
		}

		NetObs_LogPacket("WSASendTo[%u] dst=%s bufs=%u len=%u head=%02x%02x%02x%02x%02x%02x%02x%02x\n",
			(unsigned)seen, addrBuf, (unsigned)dwBufferCount, (unsigned)totalSize,
			head[0], head[1], head[2], head[3],
			head[4], head[5], head[6], head[7]);
	}

	return s_origWSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
		lpTo, iTolen, lpOverlapped, lpCompletionRoutine);
}

// Recvfrom: S3 S2C_CHALLENGE is 0x49 + body. S21 wants 0x04 + bare map name.
// Map from dedi suffix, else g_bridgeConnMapName, else mp_lobby.
bool Bridge_IsBareMapName(const char* psz)
{
	if (!psz || !psz[0])
		return false;
	size_t n = 0;
	for (; psz[n]; ++n)
	{
		const unsigned char c = static_cast<unsigned char>(psz[n]);
		if (n >= 63)
			return false;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	if (n >= 5 && _strnicmp(psz, "root_", 5) == 0)
		return false;
	return n >= 3;
}

bool Bridge_CopyBoundedCString(const char* pSrc, size_t nMax, char* pOut, size_t nOut)
{
	if (!pSrc || !pOut || nOut == 0)
		return false;
	size_t n = 0;
	for (; n + 1 < nOut && n < nMax; ++n)
	{
		pOut[n] = pSrc[n];
		if (pSrc[n] == '\0')
			return true;
	}
	pOut[n] = '\0';
	return false;
}

const char* S21Bridge_PickChallengeMap(const unsigned char* pPkt, int nLen)
{
	if (pPkt && nLen > 9 && pPkt[nLen - 1] == 0)
	{
		int end = nLen - 1;
		int start = end;
		while (start > 5)
		{
			const unsigned char c = pPkt[start - 1];
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '_'))
				break;
			--start;
		}
		if (end - start >= 3 &&
			Bridge_IsBareMapName(reinterpret_cast<const char*>(pPkt + start)))
			return reinterpret_cast<const char*>(pPkt + start);
	}
	if (pPkt && nLen > 9)
	{
		const int nAvail = nLen - 9;
		bool bTerm = false;
		for (int i = 0; i < nAvail && i < 63; ++i)
		{
			if (pPkt[9 + i] == 0)
			{
				bTerm = true;
				break;
			}
		}
		if (bTerm &&
			Bridge_IsBareMapName(reinterpret_cast<const char*>(pPkt + 9)))
			return reinterpret_cast<const char*>(pPkt + 9);
	}
	if (Bridge_IsBareMapName(g_bridgeConnMapName))
		return g_bridgeConnMapName;
	return nullptr;
}

int S21Bridge_WriteChallenge04(unsigned char* pBuf, int nBufLen, const char* pszMap)
{
	const int nName = static_cast<int>(strlen(pszMap));
	const int nNeed = 4 + 1 + nName + 4;
	if (!pBuf || nBufLen < nNeed)
		return 0;
	pBuf[0] = pBuf[1] = pBuf[2] = pBuf[3] = 0xFF;
	pBuf[4] = 0x04;
	memcpy(pBuf + 5, pszMap, static_cast<size_t>(nName));
	memset(pBuf + 5 + nName, 0, 4);
	return nNeed;
}

bool S21Bridge_RewritePollToDestChallenge04(unsigned char* pBuf, int nBufLen, int* pOutLen)
{
	if (!pBuf || !pOutLen || nBufLen < 16)
		return false;
	if (!PakLobby_DirectMapLoad())
		return false;
	if (!Bridge_IsBareMapName(g_bridgeConnMapName) ||
		_strnicmp(g_bridgeConnMapName, "mp_lobby", 8) == 0)
		return false;
	if (InterlockedCompareExchange(&s_dest04Injected, 1, 0) != 0)
		return false;
	const int nWrote = S21Bridge_WriteChallenge04(pBuf, nBufLen, g_bridgeConnMapName);
	if (nWrote <= 0)
	{
		InterlockedExchange(&s_dest04Injected, 0);
		return false;
	}
	*pOutLen = nWrote;
	Msg(eDLL_T::ENGINE, "[BRIDGE] challenge 0x04 map='%s'\n", g_bridgeConnMapName);
	return true;
}

static int WSAAPI Hook_recvfrom(SOCKET s, char* buf, int len, int flags,
                                sockaddr* from, int* fromlen)
{
	int ret = s_origRecvfrom(s, buf, len, flags, from, fromlen);

	// Reformat S2C_CHALLENGE in place. Decryption is already off while the bridge is active.
	if (ret >= 9 && buf)
	{
		unsigned char* r = reinterpret_cast<unsigned char*>(buf);
		if (S21Bridge_DestResolved() && from
			&& !S21Bridge_FromMatchesPeer(from, fromlen ? *fromlen : 0)
			&& r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF && r[3] == 0xFF
			&& (r[4] == 0x49 || r[4] == 0x4A || r[4] == 0x4B))
		{
			WSASetLastError(WSAEWOULDBLOCK);
			return SOCKET_ERROR;
		}
		if (r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF && r[3] == 0xFF && r[4] == 0x49)
		{
			s_challengeSock = s;
			const uint32_t s3Challenge = *reinterpret_cast<uint32_t*>(r + 5);
			// Never hold: an unnamed load sprintf's uninitialized
			// "<junk>_client_temp.rpak" and pops ErrorDialog. No dest map
			// yet -> well-formed mp_lobby (perm/temp still stripped).
			const char* pszMap = S21Bridge_PickChallengeMap(r, ret);
			if (pszMap)
				S21Bridge_RememberChallengeMap(pszMap);
			// Dest 0x04 arms the loadscreen. pszMap may point into r; WriteChallenge04 overwrites r.
			char szWrite[64] = {};
			const char* pszWrite = "mp_lobby";
			if (PakLobby_DirectMapLoad() && pszMap && Bridge_IsBareMapName(pszMap))
				pszWrite = pszMap;
			Bridge_CopyBoundedCString(pszWrite, 63, szWrite, sizeof(szWrite));
			if (!szWrite[0])
				Bridge_CopyBoundedCString("mp_lobby", 8, szWrite, sizeof(szWrite));
			const int nWrote = S21Bridge_WriteChallenge04(r, len, szWrite);
			if (nWrote > 0)
			{
				InterlockedExchange(&s_dest04Injected, 1);
				ret = nWrote;
				SDK_Log("[NET-OBS] BRIDGE: reformatted S2C_CHALLENGE 0x49->0x04 "
					"(%d bytes, challenge=0x%08X, map='%s')\n",
					ret, s3Challenge, szWrite);
				static long s_chal04 = 0;
				if (++s_chal04 <= 8)
					Msg(eDLL_T::ENGINE, "[BRIDGE] challenge 0x04 map='%s'\n", szWrite);
			}
			else
			{
				SDK_Log("[NET-OBS] BRIDGE: S2C_CHALLENGE reformat skipped "
					"(bufLen=%d map='%s')\n", len, pszMap);
			}
		}
		else if (r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF && r[3] == 0xFF
			&& r[4] == 0x4A && ret >= 6)
		{
			char mapBuf[64] = {};
			char modeBuf[64] = {};
			const size_t nBody = (ret > 5) ? static_cast<size_t>(ret - 5) : 0;
			Bridge_CopyBoundedCString(reinterpret_cast<const char*>(r + 5), nBody, mapBuf, sizeof(mapBuf));
			const size_t nMap = strnlen(mapBuf, sizeof(mapBuf));
			const size_t nModeOff = nMap + 1;
			if (nModeOff < nBody)
				Bridge_CopyBoundedCString(reinterpret_cast<const char*>(r + 5) + nModeOff,
					nBody - nModeOff, modeBuf, sizeof(modeBuf));
			S21Bridge_OnConnAccept(mapBuf, modeBuf);
			const char* pszKeep = (mapBuf[0] && Bridge_IsBareMapName(mapBuf))
				? mapBuf : (g_bridgeConnMapName[0] ? g_bridgeConnMapName : "mp_lobby");
			const int nWrote = S21Bridge_WriteChallenge04(r, len, pszKeep);
			if (nWrote > 0)
				ret = nWrote;
		}
		else if (r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF && r[3] == 0xFF
			&& r[4] == 0x4B)
		{
			char szReason[128] = {};
			const size_t nBody = (ret > 5) ? static_cast<size_t>(ret - 5) : 0;
			if (nBody)
				Bridge_CopyBoundedCString(reinterpret_cast<const char*>(r + 5), nBody,
					szReason, sizeof(szReason));
			S21Bridge_OnConnReject(szReason[0] ? szReason : "CONNREJECT");
		}
		else if (s_bridgeActive && from
			&& S21Bridge_FromMatchesPeer(from, fromlen ? *fromlen : 0))
		{
			S21Bridge_EnqueueS2C(buf, ret);
			static long s_s2cQ = 0;
			if (++s_s2cQ <= 16 || (s_s2cQ % 100) == 0)
				Msg(eDLL_T::ENGINE, "[BRIDGE] engine-sock S2C queued len=%d n=%ld oob=%d\n",
					ret, s_s2cQ, r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF && r[3] == 0xFF);
			WSASetLastError(WSAEWOULDBLOCK);
			return SOCKET_ERROR;
		}
	}

	// --- Bridge packet injection lives in PollReceive (VNet hook) ---
	// DO NOT read from s_bridgeSocket here.

	if (ret > 0)
	{
		const uint32_t seen = BumpDest(from);
		if (seen <= kVerboseFirstN || (seen % 256) == 0)
		{
			char addrBuf[64];
			FormatSockaddr(from, addrBuf, sizeof(addrBuf));

			unsigned char head[16] = {};
			const int copyLen = ret < 16 ? ret : 16;
			if (buf && copyLen > 0)
				memcpy(head, buf, copyLen);

			NetObs_LogPacket("recvfrom[%u] src=%s len=%d head=%02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x\n",
				(unsigned)seen, addrBuf, ret,
				head[0], head[1], head[2],  head[3],
				head[4], head[5], head[6],  head[7],
				head[8], head[9], head[10], head[11],
				head[12], head[13], head[14], head[15]);
		}
	}

	return ret;
}

static int WSAAPI Hook_WSARecvFrom(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
                                   LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
                                   sockaddr* lpFrom, LPINT lpFromlen,
                                   LPWSAOVERLAPPED lpOverlapped,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	const int ret = s_origWSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
		lpFrom, lpFromlen, lpOverlapped, lpCompletionRoutine);

	// Synchronous path only -- overlapped completes via callback or GetOverlappedResult,
	// which we'd need to hook separately. Most game traffic is synchronous-with-poll
	// so this covers the interesting case for connect handshake.
	if (ret == 0 && lpNumberOfBytesRecvd && *lpNumberOfBytesRecvd > 0)
	{
		const uint32_t seen = BumpDest(lpFrom);
		if (seen <= kVerboseFirstN || (seen % 256) == 0)
		{
			char addrBuf[64];
			FormatSockaddr(lpFrom, addrBuf, sizeof(addrBuf));

			unsigned char head[16] = {};
			if (dwBufferCount > 0 && lpBuffers[0].buf && lpBuffers[0].len > 0)
			{
				const DWORD copyLen = lpBuffers[0].len < 16 ? lpBuffers[0].len : 16;
				memcpy(head, lpBuffers[0].buf, copyLen);
			}

			NetObs_LogPacket("WSARecvFrom[%u] src=%s len=%u head=%02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x\n",
				(unsigned)seen, addrBuf, (unsigned)*lpNumberOfBytesRecvd,
				head[0], head[1], head[2],  head[3],
				head[4], head[5], head[6],  head[7],
				head[8], head[9], head[10], head[11],
				head[12], head[13], head[14], head[15]);
		}
	}

	return ret;
}

//-----------------------------------------------------------------------------
// NET_SendPacket chain sits between the connect dispatcher and ws2_32!sendto.
//-----------------------------------------------------------------------------

// netadr_t: +0x00 type +0x04 addr[16] +0x14 port host-order.
static void FormatNetadr(__int64 pNetadr, char* buf, size_t bufSize)
{
    if (!pNetadr || !buf || bufSize == 0)
    {
        if (buf && bufSize) strncpy_s(buf, bufSize, "<null>", _TRUNCATE);
        return;
    }
    const unsigned char* const p = reinterpret_cast<const unsigned char*>(pNetadr);
    const int type = *reinterpret_cast<const int*>(p + 0);
    const unsigned char* const ip = p + 4;
    // Port is stored in NETWORK byte order (big-endian) inside the netadr_t,
    // so we need ntohs to read it as a host-order int. Without this the
    // display shows 38800 instead of 37015 (0x9097 byte-swapped to 0x9790).
    const unsigned short port = ntohs(*reinterpret_cast<const unsigned short*>(p + 0x14));

    const char* typeName = "?";
    switch (type)
    {
    case 0: typeName = "NONE";     break;
    case 1: typeName = "LOOPBACK"; break;
    case 2: typeName = "IP_v2";    break;
    case 3: typeName = "IP";       break;
    case 4: typeName = "SNS";      break;
    default: break;
    }

    // Detect IPv4-mapped IPv6 (::ffff:a.b.c.d)
    bool isV4Mapped = true;
    for (int i = 0; i < 10; ++i) { if (ip[i]) { isV4Mapped = false; break; } }
    if (isV4Mapped && ip[10] == 0xFF && ip[11] == 0xFF)
    {
        _snprintf_s(buf, bufSize, _TRUNCATE,
            "type=%d(%s) %u.%u.%u.%u:%u",
            type, typeName, ip[12], ip[13], ip[14], ip[15], port);
    }
    else
    {
        _snprintf_s(buf, bufSize, _TRUNCATE,
            "type=%d(%s) [%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]:%u",
            type, typeName,
            ip[0],  ip[1],  ip[2],  ip[3],  ip[4],  ip[5],  ip[6],  ip[7],
            ip[8],  ip[9],  ip[10], ip[11], ip[12], ip[13], ip[14], ip[15],
            port);
    }
}

// Log first 32 sends per dest, then every 256th.
static volatile long long s_netSendPublicCount = 0;
static volatile long long s_netSendInnerCount  = 0;
static volatile long long s_netSendLoopCount   = 0;

//=============================================================================
// Handshake: Idle -> ChallengeSent -> ConnectSent -> Active. Driven by PollReceive.
//=============================================================================

// Build the S3 C2S_CHALLENGE datagram ("connect" OOB + platform user id).
static int S21Bridge_BuildS3Challenge(uint8_t* out, size_t outSize)
{
    if (outSize < 21) return 0;
    const uint64_t bridgeNucleusID = S21Bridge_GetConnectNucleusID();
    out[0] = 0xFF; out[1] = 0xFF; out[2] = 0xFF; out[3] = 0xFF;
    out[4] = 0x48; // C2S_CHALLENGE type 'H'
    memcpy(out + 5, "connect", 8); // "connect\0"
    memcpy(out + 13, &bridgeNucleusID, 8); // platform user id (Nucleus) LE
    return 21;
}

// Dest: connect-string / non-loopback s_bridgeDest, then captured peer, then loopback.
static void S21Bridge_ResolveDest(sockaddr_in6& dest)
{
	dest = {};
	dest.sin6_family = AF_INET6;

	if (s_bridgeDestResolved && (!Bridge_SockaddrIsLoopback(
			reinterpret_cast<const sockaddr*>(&s_bridgeDest))
		|| s_connectTargetIsLoopback))
	{
		dest = s_bridgeDest;
		return;
	}

	if (s_engineServerAddrCaptured
		&& s_engineServerAddr.ss_family == AF_INET6)
	{
		const sockaddr_in6* const p6 =
			reinterpret_cast<const sockaddr_in6*>(&s_engineServerAddr);
		// Prefer captured non-loopback; allow loopback capture only for local connects.
		if (!Bridge_SockaddrIsLoopback(reinterpret_cast<const sockaddr*>(p6))
			|| s_connectTargetIsLoopback || !s_bridgeDestResolved)
		{
			dest = *p6;
			return;
		}
	}

	if (s_bridgeDestResolved)
	{
		dest = s_bridgeDest;
		return;
	}

	// Local default only.
	const uint16_t port = Bridge_GetActiveGamePort();
	dest.sin6_port = htons(port);
	dest.sin6_addr.u.Byte[10] = 0xFF;
	dest.sin6_addr.u.Byte[11] = 0xFF;
	dest.sin6_addr.u.Byte[12] = 127;
	dest.sin6_addr.u.Byte[15] = 1;
	SDK_Log("[NET-OBS] ResolveDest FALLBACK loopback port=%u (target='%s' resolved=%d)\n",
		(unsigned)port,
		s_connectTargetStr[0] ? s_connectTargetStr : "(none)",
		s_bridgeDestResolved ? 1 : 0);
}

// Stage 0 -> 1. Creates a dual-stack IPv6 socket, sends S3 C2S_CHALLENGE,
// arms the stage deadline. Non-blocking from here -- S2C_CHALLENGE will be
// picked up in PollReceive.
static bool S21Bridge_StartHandshake()
{
    if (s_bridgeActive || s_hsStage != BridgeHsStage::Idle)
        return false;

    if (!IsOriginDisabled() && !EbisuSDK_IsConnectIdentityReady())
    {
        SDK_Log("[NET-OBS] HANDSHAKE hold: Origin identity not ready\n");
        return false;
    }

    // Fresh connect: drop stale CONNACCEPT map from a prior session.
    g_bridgeConnMapName[0] = '\0';
    s_connAcceptDone = false;
    PakLobby_OnSessionReset();
    MantleBoostClient_OnSessionReset();
    s_challengeSock = INVALID_SOCKET;
    InterlockedExchange(&s_dest04Injected, 0);

    SOCKET s4 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s4 == INVALID_SOCKET)
    {
        SDK_Log("[NET-OBS] HANDSHAKE: socket() failed WSA=%d\n", WSAGetLastError());
        return false;
    }

    // IPV6_V6ONLY=0 so the server's IPv4-mapped responses reach us.
    DWORD v6only = 0;
    setsockopt(s4, IPPROTO_IPV6, IPV6_V6ONLY,
        reinterpret_cast<const char*>(&v6only), sizeof(v6only));

    // Non-blocking. The old synchronous bypass used SO_RCVTIMEO + blocking
    // recvfrom; now PollReceive drains this socket every frame.
    u_long nonBlock = 1;
    ioctlsocket(s4, FIONBIO, &nonBlock);

    sockaddr_in6 dest;
    S21Bridge_ResolveDest(dest);

    // Refuse to "handshake to self" when the user asked for a remote host and
    // we only have loopback -- that is the old silent-fail mode.
    if (!s_connectTargetIsLoopback && s_connectTargetStr[0]
        && Bridge_SockaddrIsLoopback(reinterpret_cast<const sockaddr*>(&dest)))
    {
        SDK_Log("[NET-OBS] HANDSHAKE ABORT: connect target '%s' is remote but "
            "resolved dest is loopback (parse/capture failed)\n",
            s_connectTargetStr);
        closesocket(s4);
        return false;
    }

    uint8_t chalPkt[32];
    const int chalLen = S21Bridge_BuildS3Challenge(chalPkt, sizeof(chalPkt));

    const int sent = sendto(s4, reinterpret_cast<const char*>(chalPkt), chalLen, 0,
        reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));

    {
        char db[96];
        FormatSockaddr(reinterpret_cast<const sockaddr*>(&dest), db, sizeof(db));
        SDK_Log("[NET-OBS] HANDSHAKE: sent S3 C2S_CHALLENGE %d/%d bytes via socket %llu "
            "dest=%s target='%s' port=%u\n",
            sent, chalLen, (unsigned long long)s4, db,
            s_connectTargetStr[0] ? s_connectTargetStr : "(none)",
            (unsigned)ntohs(dest.sin6_port));
    }

    // Stash socket+dest on the bridge state so PollReceive can drive.
    // Bridge is NOT yet "active" -- we only flip that after S2C_CHALLENGE.
    s_bridgeSocket = s4;
    s_bridgeDest   = dest;
    s_hsStage      = BridgeHsStage::ChallengeSent;
    s_hsStageDeadline = GetTickCount64() + 15000; // 3 s max per stage
    return true;
}

// CONNACCEPT (ffffffff 4A): queue SignonState(CONNECTED)+UserInfo; send immediately.
void S21Bridge_OnConnAccept(const char* mapName, const char* gameMode)
{
    if (s_connAcceptDone)
        return;
    if (s_hsStage != BridgeHsStage::ConnectSent && !s_bridgeActive)
        return;
    s_connAcceptDone = true;

    SDK_Log("[NET-OBS] HANDSHAKE: *** S2C_CONNACCEPT *** map='%s' mode='%s'\n",
        mapName ? mapName : "(null)", gameMode ? gameMode : "(null)");
    Msg(eDLL_T::ENGINE, "[BRIDGE] CONNACCEPT map='%s' -- queuing CONNECTED\n",
        mapName && mapName[0] ? mapName : "?");

    // Write map name into CClientState. +0x17C m_szLevelFileName, +0x1BC m_szLevelBaseName.
    // g_pClientState is null here; the live object is the DX11/DX12 singleton.
    if (!s_clientStatePtr)
        s_clientStatePtr = NetObs_ClientStateSingletonSlotAddr();
    const char* pszUse = (mapName && Bridge_IsBareMapName(mapName)) ? mapName : "mp_lobby";
    if (mapName && mapName[0] && pszUse != mapName)
    {
        Warning(eDLL_T::ENGINE, "[BRIDGE] CONNACCEPT map '%s' rejected; using mp_lobby\n",
            mapName);
    }

    if (s_clientStatePtr)
    {
        char* pLevelFile = reinterpret_cast<char*>(s_clientStatePtr + 0x17C);
        char* pLevelBase = reinterpret_cast<char*>(s_clientStatePtr + 0x1BC);
        snprintf(pLevelFile, 64, "maps/%s.bsp", pszUse);
        strncpy(pLevelBase, pszUse, 63);
        pLevelBase[63] = '\0';
        SDK_Log("[NET-OBS] CONNACCEPT: wrote map name to CClientState: "
            "levelFile='%s' levelBase='%s'\n", pLevelFile, pLevelBase);
    }

    // Capture the clean map name for Bridge_GetLevelBaseName (the CClientState
    // field above gets partially re-stomped by the engine; this copy does not).
    {
        extern char g_bridgeConnMapName[64];
        strncpy(g_bridgeConnMapName, pszUse, sizeof(g_bridgeConnMapName) - 1);
        g_bridgeConnMapName[sizeof(g_bridgeConnMapName) - 1] = '\0';
    }

    // Queue unreliable net_SignonState(CONNECTED=2). The reliable subchannel
    // path caused server state regression in earlier testing -- keep it off.
    S21Bridge_QueueSignon(2, 0);
    s_lastSentSignonState = 2;
    S21Bridge_ClearUserInfo();

    uint8_t initPkt[8192];
    int initLen = S21Bridge_BuildS3Packet(
        initPkt, sizeof(initPkt), ++s_c2sSeqCounter, 0);
    if (initLen > 0 && s_bridgeSocket != INVALID_SOCKET && s_origSendto)
    {
        const int initSent = s_origSendto(s_bridgeSocket,
            reinterpret_cast<const char*>(initPkt), initLen, 0,
            reinterpret_cast<const sockaddr*>(&s_bridgeDest), sizeof(s_bridgeDest));
        Msg(eDLL_T::ENGINE,
            "[BRIDGE] CONNECTED c2s len=%d sent=%d seq=%u sock=%d\n",
            initLen, initSent, s_c2sSeqCounter,
            s_bridgeSocket != INVALID_SOCKET ? 1 : 0);
    }
    else
    {
        Warning(eDLL_T::ENGINE,
            "[BRIDGE] CONNECTED c2s FAILED len=%d sock=%d sendto=%p\n",
            initLen, s_bridgeSocket != INVALID_SOCKET ? 1 : 0,
            reinterpret_cast<void*>(s_origSendto));
    }

    S21Bridge_ArmUserInfo();
    initLen = S21Bridge_BuildS3Packet(
        initPkt, sizeof(initPkt), ++s_c2sSeqCounter, 0);
    if (initLen > 0 && s_bridgeSocket != INVALID_SOCKET && s_origSendto)
    {
        const int uiSent = s_origSendto(s_bridgeSocket,
            reinterpret_cast<const char*>(initPkt), initLen, 0,
            reinterpret_cast<const sockaddr*>(&s_bridgeDest), sizeof(s_bridgeDest));
        Msg(eDLL_T::ENGINE,
            "[BRIDGE] USERINFO c2s len=%d sent=%d seq=%u\n",
            initLen, uiSent, s_c2sSeqCounter);
    }

    InterlockedExchange(&s_needNativeConnected, 1);
    InterlockedExchange(&s_nativeConnectedTries, 0);
    s_hsStage = BridgeHsStage::Idle; // handshake complete, drain loop takes over
}

// Stage 2 -> Idle. Called on S2C_CONNREJECT (ffffffff 4B). Logs the reason,
// tears down the bridge so the engine's retry timer can try again.
static void S21Bridge_ResetAllState()
{
    s_bridgeChan          = nullptr;
    s_c2sSeqCounter       = 0;
    s_bridgeNonceHost     = 0x0000CAFE;
    s_serverNonce         = 0;
    s_needNonceAck        = false;
    s_serverAckedUs       = false;
    s_bridgeSubSeq        = 0;
    s_stringCmdSeq        = 0;
    s_nStringCmdRingHead  = 0;
    for (int i = 0; i < 32; ++i)
        s_stringCmdRing[i].sendsLeft = 0;
    S21Bridge_C2SRel_Reset();
    s_bridgeInSeqNr       = 0;
    s_serverSubSeqRecv    = 0;
    s_subSeqRebased       = false;
    s_serverNonceCaptured = false;
    s_lastBridgeC2STime   = 0.0;
    S21Bridge_ClearPendingSignon();
    s_pendingSignonReady  = false;
    s_pendingSignonSize   = 0;
    s_lastSentSignonState = -1;
    S21Bridge_ClearUserInfo();
    s_signonSeqKey        = -1;
    s_signonSeqSpawn      = -1;
    s_signonSeqMax        = -1;
    s_signonSeqDidCL      = false;
    s_signonSawStateSinceCL = false;
    s_signonRewalk        = false;
    AcquireSRWLockExclusive(&s_c2sTxLock);
    s_c2sPend.Reset();
    s_nSetConVarRingHead = 0;
    for (int i = 0; i < 4; ++i)
    {
        s_setConVarRing[i].active = false;
        s_setConVarRing[i].nBits = 0;
    }
    ReleaseSRWLockExclusive(&s_c2sTxLock);
    s_reliableSize        = 0;
    s_bridgeChan          = nullptr;
    s_dbSawSendTables     = false;
    s_deferredSignon3     = false;
    s_deferredSignon3Bits = 0;
    s_deferredSignon3Spawn = -1;
    s_dbComplete          = false;
    s_dbTransferCount     = 0;
    s_dbTransferNr        = 0xFFFF;
    s_dbBlocksReceived    = 0;
    s_dbTransferSize      = 0;
    s_dbTotalBlocks       = 0;
    memset(s_dbBlockStatus, 0, sizeof(s_dbBlockStatus));
    S21Bridge_C2SUnrelSlices_Reset(); // [C2S-SLICE] extents describe a stream that is gone
    InterlockedIncrement(&s_splitSessionGen);
    s_splitQueueHead = 0;
    s_splitQueueTail = 0;
    for (int i = 0; i < SPLIT_QUEUE_SIZE; ++i)
        s_splitQueueGen[i] = 0;
    for (int i = 0; i < 4; ++i)
        s_splitSlotGen[i] = 0;
    S21Bridge_ResetSplitReassembly();
    memset(s_relaySendTime, 0, sizeof(s_relaySendTime));
    s_outBytesAcc = 0;
    S21Bridge_FlowStatsReset();
    S21BridgeCmd::ResetToNullCmd(s_bridgeC2sPrevCmd);
    s_bridgeC2sPrevS3ImpulseWire = 0; // [S21-EXTRA-FLAGS] same reset lifecycle as s_bridgeC2sPrevCmd
    s_ftEstLastCmdNr = 0;             // [C2S-FT-RESTAMP] estimator reset (cmd numbers restart per connection)
    s_ftEstLastWall  = 0.0;
    s_ftEstPerCmd    = 0.0f;
    GenerateDeltas_ClearFilled();
    s_challengeSock = INVALID_SOCKET;
    memset(s_ackXlate, 0, sizeof(s_ackXlate));
    PredAuth_ResetSession();
    InterlockedExchange(&s_dest04Injected, 0);
    s_connAcceptDone = false;
    PakLobby_OnSessionReset();
    MantleBoostClient_OnSessionReset();
    InterlockedExchange(&s_needNativeConnected, 0);
    InterlockedExchange(&s_nativeConnectedTries, 0);
    s_bridgePdefReady = false;
    // Do NOT clear connect target here -- Connect_Worker re-seeds it after
    // ResetAllState on a fresh connect. Mid-session resets (reject/timeout)
    // must keep s_bridgeDest so retries still hit the same VPS.
    SDK_Log("[BRIDGE] full state reset (all netchan/DataBlock/signon vars cleared)\n");
}

void S21Bridge_RememberChallengeMap(const char* pszMap)
{
	if (!pszMap || !pszMap[0] || !Bridge_IsBareMapName(pszMap))
		return;
	strncpy(g_bridgeConnMapName, pszMap, sizeof(g_bridgeConnMapName) - 1);
	g_bridgeConnMapName[sizeof(g_bridgeConnMapName) - 1] = '\0';
}

static void S21Bridge_OnSessionEnded(const char* reason)
{
	Bridge_NotifyConnectSessionEnded();
	s_connAcceptDone = false;
	PakLobby_OnSessionReset();
	MantleBoostClient_OnSessionReset();
	if (!s_bridgeActive && s_hsStage == BridgeHsStage::Idle
		&& s_bridgeSocket == INVALID_SOCKET)
		return;

	SDK_Log("[NET-OBS] session ended (%s) -- bridge teardown for reconnect\n",
		reason ? reason : "?");
	// Close without this and the dedi keeps the slot until timeout (~400s);
	// reconnect then sits on dest 0x04 waiting for CONNACCEPT that never comes.
	S21Bridge_SendDisconnect();
	if (s_bridgeSocket != INVALID_SOCKET)
	{
		closesocket(s_bridgeSocket);
		s_bridgeSocket = INVALID_SOCKET;
	}
	s_bridgeActive = false;
	s_hsStage = BridgeHsStage::Idle;
	s_lastBypassAttempt = 0;
	S21Bridge_ResetAllState();
}

static void S21Bridge_ResetForNewConnect(void)
{
	if (s_hsStage != BridgeHsStage::Idle || s_bridgeActive
		|| s_bridgeSocket != INVALID_SOCKET)
		S21Bridge_OnSessionEnded("new connect");
}

void S21Bridge_OnConnReject(const char* reason)
{
    SDK_Log("[NET-OBS] HANDSHAKE: S2C_CONNREJECT reason='%s'\n",
        reason ? reason : "(null)");
    S21Bridge_OnSessionEnded(reason ? reason : "CONNREJECT");
}

static __int64 Hook_NET_SendPacket_Public(__int64 a1, unsigned int a2, __int64 a3,
                                          const void* a4, unsigned int a5, unsigned int a6)
{
    const long long n = ++s_netSendPublicCount;
    if (n <= 32 || (n % 256) == 0)
    {
        char addrBuf[96];
        // a3 is netadr_t*. Only deref canonical user addresses (not kernel-half / sub-page).
        if (a3 && a3 < 0x0000800000000000LL)
            FormatNetadr(a3, addrBuf, sizeof(addrBuf));
        else
            strncpy_s(addrBuf, sizeof(addrBuf), "<invalid netadr>", _TRUNCATE);

        unsigned char head[8] = {};
        const unsigned int copyLen = a5 < 8 ? a5 : 8;
        if (a4 && copyLen > 0)
            memcpy(head, a4, copyLen);

        if (SDK_OBSERVE_NET_GE(2))
        {
            SDK_Log("[NET-OBS] NET_SendPacket(pub) #%lld sock=%u len=%u flags=0x%x %s head=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                n, a2, a5, a6, addrBuf,
                head[0], head[1], head[2], head[3],
                head[4], head[5], head[6], head[7]);
        }
    }
    return s_origNetSendPacketPublic(a1, a2, a3, a4, a5, a6);
}

// Track socket handle from connect sends for response probing
static volatile SOCKET s_lastConnectSocket = INVALID_SOCKET;
static volatile long long s_challengesSent = 0;

static __int64 Hook_NET_SendPacket_Inner(__int64 a1, __int64 a2, __int64 a3,
                                         const void* a4, int a5, unsigned int* a6, char a7)
{
    // S21 Bridge: strip per-packet encrypt flag for active bridge traffic when
    // force-off is enabled (S3 cannot decrypt S21 DTLS). [SEC] Gated by
    // bridge_force_encryption_off; when 0, leave engine encrypt bit alone.
    if (s_bridgeActive && a7 && bridge_force_encryption_off.GetBool())
    {
        static bool s_plainOnce = false;
        if (!s_plainOnce)
        {
            s_plainOnce = true;
            Warning(eDLL_T::ENGINE, "[SEC] bridge path forcing plaintext packets "
                "(bridge_force_encryption_off=1; MITM risk on shared nets)\n");
        }
        a7 = 0;
    }

    const long long n = ++s_netSendInnerCount;

    // Detect C2S_CHALLENGE sends (ffffffff48) to our game port (not hard 37015).
    bool isChallengeSend = false;
    if (a4 && a5 >= 5 && a6)
    {
        const unsigned char* const p = static_cast<const unsigned char*>(a4);
        if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF && p[4] == 0x48)
        {
            // netadr_t: +0x04 addr[16], +0x14 port network byte order (see FormatNetadr).
            const unsigned char* const na = reinterpret_cast<const unsigned char*>(a6);
            const unsigned short port = ntohs(*reinterpret_cast<const unsigned short*>(na + 0x14));
            if (Bridge_IsBridgeGamePort(port))
            {
                isChallengeSend = true;
                // Seed/refresh dest from the engine netadr (covers connect paths
                // that never hit our string parse, and keeps remote IP alive).
                sockaddr_in6 sa = {};
                sa.sin6_family = AF_INET6;
                memcpy(&sa.sin6_addr, na + 4, 16);
                sa.sin6_port = *reinterpret_cast<const unsigned short*>(na + 0x14);
                Bridge_NoteEngineServerSendto(
                    reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
            }
        }
    }

    if (n <= 32 || (n % 256) == 0 || isChallengeSend)
    {
        char addrBuf[96];
        FormatNetadr(reinterpret_cast<__int64>(a6), addrBuf, sizeof(addrBuf));

        const int type = a6 ? *reinterpret_cast<int*>(a6) : -1;
        const char* branch = (type == 4) ? "SNS" : "sendto";

        unsigned char head[8] = {};
        const int copyLen = a5 < 8 ? a5 : 8;
        if (a4 && copyLen > 0)
            memcpy(head, a4, copyLen);

        if (SDK_OBSERVE_NET_GE(1))
        {
            SDK_Log("[NET-OBS] NET_SendPacket(in)  #%lld len=%d %s -> %s  a7=%d head=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                n, a5, addrBuf, branch, (int)a7,
                head[0], head[1], head[2], head[3],
                head[4], head[5], head[6], head[7]);
        }
    }

    // Call the original send
    const __int64 ret = s_origNetSendPacketInner(a1, a2, a3, a4, a5, a6, a7);

    // On C2S_CHALLENGE, start the async S3 handshake. Waits run in PollReceive, not here.
    if (isChallengeSend && a3 && a3 != (__int64)INVALID_SOCKET)
    {
        ++s_challengesSent;
        s_lastConnectSocket = (SOCKET)a3;

        // Rate limit: the engine retries C2S_CHALLENGE every ~500 ms during
        // connect. Only kick off a fresh attempt every 3 s so we don't
        // thrash sockets while the server is warming up.
        const ULONGLONG now = GetTickCount64();

        // A live bridge already completed the S3 handshake. The engine keeps
        // emitting C2S_CHALLENGE on its own socket; another handshake would
        // be a second client on the dedi.
        if (s_bridgeActive)
            return ret;

        if (s_hsStage == BridgeHsStage::Idle &&
            (now - s_lastBypassAttempt) > 3000)
        {
            s_lastBypassAttempt = now;
            S21Bridge_StartHandshake();
        }
    }

    return ret;
}

static void Hook_NET_SendLoopback(unsigned int a1, int a2, const void* a3, __int64 a4)
{
    const long long n = ++s_netSendLoopCount;
    if (n <= 32 || (n % 256) == 0)
    {
        unsigned char head[8] = {};
        const int copyLen = a2 < 8 ? a2 : 8;
        if (a3 && copyLen > 0)
            memcpy(head, a3, copyLen);

        // CRITICAL: if our connect packet ends up here it means the engine
        // routed it through the in-process queue and it WILL NEVER reach
        // the wire. The S3 dedicated server will never see it.
        SDK_Log("[NET-OBS] NET_SendLoopback   #%lld sock=%u len=%d head=%02x%02x%02x%02x%02x%02x%02x%02x  <-- IN-PROCESS, not wire\n",
            n, a1, a2,
            head[0], head[1], head[2], head[3],
            head[4], head[5], head[6], head[7]);
    }
    s_origNetSendLoopback(a1, a2, a3, a4);
}

// Skip if struct +0xA8/+0xB0 are -1 (memset dests not allocated yet).
static void Hook_Case3InitHelper(void)
{
    // Read the global struct pointer this function uses.
    // SEH-guard the deref in case the global itself is NULL (unlikely once
    // the engine has initialized far enough to dispatch case-3, but cheap).
    __try
    {
        uintptr_t base = NetObs_GetExeModuleBase();
        if (base)
        {
            void** const pStructPtr = reinterpret_cast<void**>(
                NetObs_Case3InitHelperStructPtrAddr());
            void* const pStruct = *pStructPtr;

            if (pStruct)
            {
                const uintptr_t ptrA8 = *reinterpret_cast<uintptr_t*>(
                    static_cast<char*>(pStruct) + NetObs_Case3InitHelperBufferAOffset());
                const uintptr_t ptrB0 = *reinterpret_cast<uintptr_t*>(
                    static_cast<char*>(pStruct) + NetObs_Case3InitHelperBufferBOffset());

                // Canonical x64 user pointer: past page 0, upper 17 bits zero.
                auto isBadPtr = [](uintptr_t p) -> bool {
                    return p < 0x10000ULL ||
                           (p & 0xFFFF800000000000ULL) != 0;
                };

                if (isBadPtr(ptrA8) || isBadPtr(ptrB0))
                {
                    static long long s_skipCount = 0;
                    if (++s_skipCount <= 10 || (s_skipCount % 100) == 0)
                    {
                        SDK_Log("[NET-OBS] #%lld skipped"
                            "struct=0x%p +0xA8=0x%llX +0xB0=0x%llX "
                            "(non-canonical pointer)\n",
                            s_skipCount, pStruct,
                            (unsigned long long)ptrA8,
                            (unsigned long long)ptrB0);
                    }
                    return;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        SDK_Log("[NET-OBS] sentinel probe SEH-faulted;"
            "letting original run\n");
    }

    // Field validity confirmed (or probe failed and we let it through).
    // Wrap the original call in SEH as a final safety net so any RARE
    // crash inside it doesn't take the client down.
    __try
    {
        if (s_origCase3InitHelper) s_origCase3InitHelper();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        SDK_Log("[NET-OBS] SEH-caught after sentinel-probe pass\n");
    }
}

// Userinfo string table reader guard. is the USERINFO table
// (CClientState+0x21338), NOT entity baselines. Returns NULL when user data
// hasn't arrived via svc_UpdateStringTable yet (timing race, expected).
static char __fastcall Hook_EntityDataCopy(__int64 a1, int a2, void* a3)
{
    int idx = a2 - 1;
    int maxEnts = *reinterpret_cast<int*>(NetObs_MaxEntsAddr());
    uintptr_t uiTable = *reinterpret_cast<uintptr_t*>(NetObs_UiEntityTableAddr());

    if (idx < 0 || idx >= maxEnts || !uiTable)
    {
        memset(a3, 0, 0x131);
        return 0;
    }

    uintptr_t vtbl = *reinterpret_cast<uintptr_t*>(uiTable);
    typedef uintptr_t (__fastcall *GetUserData_fn)(uintptr_t, int, uintptr_t);
    GetUserData_fn getUserData = *reinterpret_cast<GetUserData_fn*>(vtbl + 0x80);
    uintptr_t dataPtr = getUserData(uiTable, idx, 0);

    if (dataPtr < 0x10000 || dataPtr > 0x7FFFFFFFFFFFULL)
    {
        memset(a3, 0, 0x131);
        static int s_guardCount = 0;
        if (++s_guardCount <= 2)
            printf("[NET-OBS] userinfo table: no user data for slot %d (#%d)\n",
                a2, s_guardCount);
        return 0;
    }

    return s_origEntityDataCopy(a1, a2, a3);
}


//=============================================================================
// Suppress ExitProcess(1) while mid-signon. Load-bearing, not a diagnostic.
//=============================================================================
static void WINAPI Hook_ExitProcess(UINT uExitCode)
{
    const LONG signonState = InterlockedCompareExchange(&s_bridgeSignonState, 0, 0);

    // [DEATH-OBS] Log EVERY exit (not just the suppressed one) with caller
    // site, flushed, so the silent PRESPAWN self-close names its trigger.
    {
        void* const ret = _ReturnAddress();
        Warning(eDLL_T::ENGINE,
            "[DEATH-OBS] ExitProcess(code=%u) signon=%d caller_va=0x%llX\n",
            uExitCode, (int)signonState, DeathObs_ImageVA(ret));
        BridgeTrace_Log(
            "[DEATH-OBS] ExitProcess(code=%u) signon=%d caller_va=0x%llX\n",
            uExitCode, (int)signonState, DeathObs_ImageVA(ret));
        BridgeTrace_Flush();
    }

    if (uExitCode == 1 && s_bridgeActive && signonState > 0 && signonState < 8)
    {
        Warning(eDLL_T::ENGINE,
            "[BRIDGE] suppressed ExitProcess(1) during signon (state=%d)\n",
            signonState);
        return;
    }
    s_origExitProcess(uExitCode);
}

static BOOL WINAPI Hook_TerminateProcess(HANDLE hProcess, UINT uExitCode)
{
    // [DEATH-OBS] log-only; never alters control flow. Only self-terminations
    // are interesting (hProcess == current/pseudo-handle -1).
    const bool self = (hProcess == GetCurrentProcess()) ||
                      (hProcess == (HANDLE)(intptr_t)-1);
    void* const ret = _ReturnAddress();
    Warning(eDLL_T::ENGINE,
        "[DEATH-OBS] TerminateProcess(%s, code=0x%X) caller_va=0x%llX\n",
        self ? "SELF" : "other", uExitCode, DeathObs_ImageVA(ret));
    BridgeTrace_Log(
        "[DEATH-OBS] TerminateProcess(%s, code=0x%X) caller_va=0x%llX\n",
        self ? "SELF" : "other", uExitCode, DeathObs_ImageVA(ret));
    BridgeTrace_Flush();
    return s_origTerminateProcess(hProcess, uExitCode);
}

// [DUMP-FULL] Optional force of heap into crash minidumps (corruptor hunt).
// (PFN_MiniDumpWriteDump + s_origMiniDumpWriteDump are declared above Bridge_VEH.)
static BOOL WINAPI Hook_MiniDumpWriteDump(HANDLE hProc, DWORD pid, HANDLE hFile,
    DWORD dumpType, void* exc, void* usr, void* cb)
{
    // Let the engine's dump complete FIRST -- dbghelp is not re-entrant.
    const BOOL engineResult = s_origMiniDumpWriteDump
        ? s_origMiniDumpWriteDump(hProc, pid, hFile, dumpType, exc, usr, cb)
        : FALSE;
    return engineResult;
}

//=============================================================================
// ClassInfo / CHANGELEVEL probes. Flush so the last line survives process death.
//=============================================================================

// Canonical 18 S3-only DTs first-join stubs (CLASS-MAP NULL set).
static const char* const s_ciS3OnlyDts[] = {
	"DT_Beam",
	"DT_PhysBox",
	"DT_PlayerResource",
	"DT_SpotlightEnd",
	"DT_Sprite",
	"DT_SpriteOriented",
	"DT_BaseBeam",
	"DT_TEBeamEntPoint",
	"DT_TEBeamEnts",
	"DT_TEBeamFollow",
	"DT_TEBeamLaser",
	"DT_TEBeamPoints",
	"DT_TEBeamRing",
	"DT_TEBeamRingPoint",
	"DT_TEBeamSpline",
	"DT_TriggerNoGrapple",
	"DT_TriggerNoZipline",
	"DT_TriggerSlip",
};
static constexpr int kCiS3OnlyDtCount = (int)(sizeof(s_ciS3OnlyDts) / sizeof(s_ciS3OnlyDts[0]));

bool S21Bridge_CIDiag_On(void)
{
	return bridge_classinfo_diag.GetInt() > 0;
}

static bool S21Bridge_CIDiag_Verbose(void)
{
	return bridge_classinfo_diag.GetInt() >= 2;
}

void S21Bridge_CIDiag_Flush(void)
{
	fflush(stdout);
	BridgeTrace_Flush();
	// Also flush spdlog message/warning so Warning lines land before kill.
	if (auto w = spdlog::get("sdk(warning)"))
		w->flush();
	if (auto m = spdlog::get("sdk"))
		m->flush();
}

// CClientState+0x1BC level base name. Prefer the CONNACCEPT capture over this field.
static constexpr uintptr_t kCS_LevelBase = 0x1BC;

static uintptr_t S21Bridge_ResolveClientState(void)
{
	// Captured from the SetSignonState hook; null until the first signon.
	// Every caller already treats 0 as "not available".
	return s_clientStatePtr;
}

static void S21Bridge_CIDiag_ReadMap(char* out, int outLen)
{
	if (!out || outLen <= 0)
		return;
	out[0] = '\0';
	if (g_bridgeConnMapName[0])
	{
		strncpy(out, g_bridgeConnMapName, (size_t)outLen - 1);
		out[outLen - 1] = '\0';
		return;
	}
	const uintptr_t cs = S21Bridge_ResolveClientState();
	if (!cs)
		return;
	__try
	{
		const char* base = reinterpret_cast<const char*>(cs + kCS_LevelBase);
		if (base && base[0])
		{
			strncpy(out, base, (size_t)outLen - 1);
			out[outLen - 1] = '\0';
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out[0] = '\0';
	}
}

void S21Bridge_CIDiag_ReadClassMeta(int* outN, uintptr_t* outArr)
{
	if (outN) *outN = -1;
	if (outArr) *outArr = 0;
	__try
	{
		const uintptr_t arrAddr = NetObs_ClientClassArrayAddr();
		const uintptr_t cntAddr = NetObs_ClientClassCountAddr();
		if (outArr && arrAddr)
			*outArr = *reinterpret_cast<uintptr_t*>(arrAddr);
		if (outN && cntAddr)
			*outN = *reinterpret_cast<int*>(cntAddr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (outN) *outN = -2;
		if (outArr) *outArr = 0;
	}
}

// Snapshot of S3-only DT health: hasName (HasRecvTable), stubPtr, pending count.
static void S21Bridge_CIDiag_LogS3Only(const char* where)
{
	int hasName = 0, hasStub = 0, miss = 0;
	for (int i = 0; i < kCiS3OnlyDtCount; ++i)
	{
		const char* dt = s_ciS3OnlyDts[i];
		const bool hn = S21Bridge_HasRecvTable(dt);
		const uintptr_t st = S21Bridge_FindStub(dt);
		if (hn) ++hasName;
		if (st) ++hasStub;
		if (!hn && !st) ++miss;
		if (S21Bridge_CIDiag_Verbose() || (!hn && !st))
		{
			SDK_Log("[CI-ST] %s dt='%s' hasName=%d stub=%p\n",
				where ? where : "?", dt, hn ? 1 : 0, (void*)st);
		}
	}
	SDK_Log("[CI-ST] %s s3only: hasName=%d/%d hasStub=%d/%d miss=%d pendingStubs=%zu regStubs=%zu nameSet=%zu\n",
		where ? where : "?",
		hasName, kCiS3OnlyDtCount,
		hasStub, kCiS3OnlyDtCount,
		miss,
		s_pendingStubs.size(),
		s_registeredStubCount,
		s_s21RecvTableNames.size());
}

// Full gate: map + class registry + stub census + S3-only DT health.
void S21Bridge_CIDiag_Gate(const char* where)
{
	if (!S21Bridge_CIDiag_On())
		return;

	char map[64] = {};
	S21Bridge_CIDiag_ReadMap(map, (int)sizeof(map));

	int nClasses = -1;
	uintptr_t arr = 0;
	S21Bridge_CIDiag_ReadClassMeta(&nClasses, &arr);

	PROCESS_MEMORY_COUNTERS pmc = {};
	pmc.cb = sizeof(pmc);
	const uint64_t rssMb = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))
		? (uint64_t)(pmc.WorkingSetSize / (1024ull * 1024ull)) : 0ull;

	const uintptr_t cs = S21Bridge_ResolveClientState();
	int signon = -1;
	if (cs)
	{
		__try { signon = *reinterpret_cast<int*>(cs + 0x9C); } // best-effort; layout may vary
		__except (EXCEPTION_EXECUTE_HANDLER) { signon = -2; }
	}

	SDK_Log("[CI-GATE] %s map='%s' nClasses=%d arr=%p stubsPending=%zu stubsReg=%zu "
		"recvNameSet=%zu rssMb=%llu cs=%p signonGuess=%d\n",
		where ? where : "?",
		map[0] ? map : "(none)",
		nClasses, (void*)arr,
		s_pendingStubs.size(), s_registeredStubCount,
		s_s21RecvTableNames.size(),
		(unsigned long long)rssMb,
		(void*)cs, signon);
	S21Bridge_CIDiag_LogS3Only(where);
	S21Bridge_CIDiag_Flush();
}

// ST preprocess summary for the DB signon path (matched/flipped/noDecoder).
void S21Bridge_CIDiag_STSummary(const char* tag, int matched, int flipped, int noDecoder)
{
	if (!S21Bridge_CIDiag_On())
		return;
	char map[64] = {};
	S21Bridge_CIDiag_ReadMap(map, (int)sizeof(map));
	SDK_Log("[CI-ST] %s map='%s' matched=%d flipped=%d noDecoder=%d "
		"(reload-healthy often 128/0 after first join stubs sticky; first-join 110/18)\n",
		tag ? tag : "DB",
		map[0] ? map : "(none)",
		matched, flipped, noDecoder);
	S21Bridge_CIDiag_LogS3Only(tag ? tag : "DB");
	S21Bridge_CIDiag_Flush();
}

static char Hook_CBaseClient_SetSignonState(__int64 a1, unsigned int state, int spawncount, __int64 a4)
{
    static const char* const stateNames[] = {
        "NONE", "CHALLENGE", "CONNECTED", "NEW", "PRESPAWN",
        "GETTINGDATA", "SPAWN", "FIRSTSNAP", "FULL", "?9", "CHANGELEVEL", "?11", "?12", "FAIL13"
    };
    const char* const name = (state < sizeof(stateNames)/sizeof(stateNames[0]))
        ? stateNames[state] : "?";

    // Capture CClientState pointer for direct field writes (e.g. map name
    // from CONNACCEPT). The pointer is stable for the process lifetime.
    if (a1 && !s_clientStatePtr)
        s_clientStatePtr = (uintptr_t)a1;

    InterlockedExchange(&s_bridgeSignonState, (LONG)state);

    SDK_Log("[NET-OBS] SetSignonState ENTRY  client=%p state=%u(%s) spawncount=%d\n",
        (void*)a1, state, name, spawncount);

    char ssEntryTag[64];
    snprintf(ssEntryTag, sizeof(ssEntryTag),
        "SetSignonState(%u/%s) ENTRY", state, name);
    SDK_LogRSS(ssEntryTag);

    const char ret = s_origSetSignonState(a1, state, spawncount, a4);

    // Read the new state out of the client struct (offset 172 = 0xAC).
    int newState = -1;
    if (a1)
        newState = *reinterpret_cast<int*>(a1 + 172);

    SDK_Log("[NET-OBS] SetSignonState RETURN client=%p ret=%d newState=%d\n",
        (void*)a1, (int)ret, newState);

    char ssReturnTag[64];
    snprintf(ssReturnTag, sizeof(ssReturnTag),
        "SetSignonState(%u/%s) RETURN newState=%d", state, name, newState);
    SDK_LogRSS(ssReturnTag);

    // Full teardown on signon state=0 so the next connect starts a new handshake.
    if (state == 0)
        S21Bridge_OnSessionEnded("CBaseClient NONE");

    // Changelevel (S21 state 10): reset monotonic signon guard so NEW..FULL re-confirm.
    if (state == 10)
    {
        s_lastSentSignonState = -1;
        // Clear s_dbSawSendTables on CHANGELEVEL so state=3 waits for the new SendTable.
        s_dbSawSendTables = false;
        s_deferredSignon3 = false;
        s_deferredSignon3Bits = 0;
        s_deferredSignon3Spawn = -1;
        SDK_Log("[BRIDGE-SIGNON] CHANGELEVEL(10) -- reset C2S monotonic + "
            "SendTable/defer-3 gates for re-signon\n");
        // Level teardown wipes weapon const-types dataStruct counts without
        // clearing registry counts; re-fill before weapon/HUD scripts re-init
        // so GetWeaponSettingEnum(cooldown_type, eWeaponCooldownType) works.
        WeapConst_EnsurePopulated_S21();
        // District same-map reload crash hunt: snapshot class/stub state at CL edge.
        S21Bridge_CIDiag_Gate("CHANGELEVEL");
    }
    // Belt-and-suspenders: re-ensure when re-entering CONNECTED / SPAWN after
    // changelevel (teardown may finish after the CHANGELEVEL state edge).
    if (state == 2 || state == 6)
        WeapConst_EnsurePopulated_S21();

    // Queue synthetic net_SignonState; S21 C2S body is not S3-parseable.
    if (s_bridgeActive && state >= 2 && state <= 8)
    {
        // Echo only. Dedi reconnects on spawn mismatch (state>2) or skip-ahead;
        // lower is a no-op; equal runs that rung's work. Same-state re-queue
        // covers a dropped PRESPAWN (dedi stays at NEW retransmitting ClassInfo).
        if ((int)state >= s_lastSentSignonState)
        {
            S21Bridge_QueueSignon((int)state, spawncount);
            s_lastSentSignonState = (int)state;
            SDK_Log("[BRIDGE-SIGNON] queued net_SignonState(%u=%s) unreliable for S3 server\n",
                state, name);
        }
        else
        {
            SDK_Log("[BRIDGE-SIGNON] dropped regressing net_SignonState(%u=%s) -- last sent was %d\n",
                state, name, s_lastSentSignonState);
        }
    }

    return ret;
}

//-----------------------------------------------------------------------------
// Install -- called once from SDK_Init after the manual pattern scan.
//-----------------------------------------------------------------------------
static bool AttachOne(PVOID* ppTarget, PVOID pDetour, const char* name)
{
	LONG err = DetourTransactionBegin();
	if (err != NO_ERROR)
	{
		SDK_Log("[NET-OBS] %s: DetourTransactionBegin failed (%ld)\n", name, err);
		return false;
	}
	DetourUpdateThread(GetCurrentThread());
	err = DetourAttach(ppTarget, pDetour);
	if (err != NO_ERROR)
	{
		DetourTransactionAbort();
		SDK_Log("[NET-OBS] %s: DetourAttach failed (%ld)\n", name, err);
		return false;
	}
	err = DetourTransactionCommit();
	if (err != NO_ERROR)
	{
		SDK_Log("[NET-OBS] %s: DetourTransactionCommit failed (%ld)\n", name, err);
		return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// Bridge reliable data reassembly state
//-----------------------------------------------------------------------------
void BridgeReliable_s::Free()
{
	if (buffer)
	{
		HeapCanary::Unregister(buffer);
		free(buffer);
		buffer = nullptr;
	}
	capacity = 0;
	totalSize = 0;
	receivedSize = 0;
	isCompressed = false;
	uncompressedSize = 0;
	active = false;
	entrySeq = 0;
}

void BridgeReliable_s::Reset()
{
	totalSize = 0;
	receivedSize = 0;
	isCompressed = false;
	uncompressedSize = 0;
	active = false;
	entrySeq = 0;
}

BridgeReliable_s s_bridgeReliable = {};

// Only for a signon rewind, where the walk we already answered is being
// restarted. The ActivateServer list-restart case is handled at the subchannel
// header, once per restart -- never call this from inside the entry loop.
void S21Bridge_ResetReliableRecv(const char* reason)
{
	if (s_serverSubSeqRecv == 0 && !s_bridgeReliable.active)
		return;
	Warning(eDLL_T::ENGINE,
		"[BRIDGE-REL] reset recv floor %u (%s)\n",
		s_serverSubSeqRecv, reason ? reason : "?");
	s_serverSubSeqRecv = 0;
	s_bridgeReliable.Reset();
}

//-----------------------------------------------------------------------------
// ExecuteCallQueue(this, snapshotTick). Borrow tick; native remote queue stays empty.
//-----------------------------------------------------------------------------
void VScriptCreateNotifyS21::GetFun(void) const
{
	CMemory fakeRecreate = Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 80 B9 79 06 00 00 00 48 8B D9 0F 84 ?? ?? ?? ?? 48 8D 0D");
	fakeRecreate.GetPtr(s_pFakeRecreate);
	if (!s_pFakeRecreate)
	{
		Warning(eDLL_T::ENGINE, "[SCRIPT-CREATE] C_BaseEntity::FakeRecreate unresolved\n");
	}
	else if (fakeRecreate.Offset(0x56).CheckOpCodes({ 0x48, 0x8D, 0x0D }))
	{
		s_ppNotifyScriptEnts = fakeRecreate.Offset(0x56)
			.ResolveRelativeAddress(3, 7)
			.RCast<void***>();
	}
	else
	{
		s_ppNotifyScriptEnts = nullptr;
		Warning(eDLL_T::ENGINE, "[SCRIPT-CREATE] g_NotifyScriptEnts unresolved\n");
	}

	CMemory notify = Module_FindPattern(g_GameDll,
		"48 81 EC ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 C6 05");
	notify.GetPtr(s_pNotifyScriptOfNewEntities);
	if (!s_pNotifyScriptOfNewEntities)
	{
		Warning(eDLL_T::ENGINE, "[SCRIPT-CREATE] C_BaseEntity::NotifyScriptOfNewEntities unresolved\n");
	}
	else
	{
		if (notify.Offset(0x07).CheckOpCodes({ 0x83, 0x3D }))
		{
			s_pNotifyScriptEntsCount = notify.Offset(0x07)
				.ResolveRelativeAddress(2, 7)
				.RCast<int*>();
		}
		else
		{
			s_pNotifyScriptEntsCount = nullptr;
			Warning(eDLL_T::ENGINE, "[SCRIPT-CREATE] g_NotifyScriptEnts.Count unresolved\n");
		}

		if (notify.Offset(0x0E).CheckOpCodes({ 0xC6, 0x05 }))
		{
			s_pAcceptEntsForScript = notify.Offset(0x0E)
				.ResolveRelativeAddress(2, 7)
				.RCast<uint8_t*>();
		}
		else
		{
			s_pAcceptEntsForScript = nullptr;
			Warning(eDLL_T::ENGINE, "[SCRIPT-CREATE] g_acceptEntsForScript unresolved\n");
		}
	}

	CMemory dfRadius = Module_FindPattern(g_GameDll,
		"48 8B 0D ?? ?? ?? ?? 0F 28 C8 48 63 C2 80 BC 08 B0 09 00 00 00 75 09");
	dfRadius.GetPtr(s_pDeathFieldRadiusForTime);
	if (!s_pDeathFieldRadiusForTime)
	{
		s_ppClientWorld = nullptr;
		Warning(eDLL_T::ENGINE, "[DEATHFIELD-CL] DeathField_GetRadiusForTime unresolved\n");
	}
	else
	{
		s_ppClientWorld = dfRadius.ResolveRelativeAddress(3, 7).RCast<uint8_t**>();
		if (!s_ppClientWorld)
			Warning(eDLL_T::ENGINE, "[DEATHFIELD-CL] g_pClientWorld unresolved\n");
	}
}

void VScriptCreateNotifyS21::Detour(const bool bAttach) const
{
	if (s_pNotifyScriptOfNewEntities)
		DetourSetup(&s_pNotifyScriptOfNewEntities, &Hook_NotifyScriptOfNewEntities, bAttach);
	if (s_pDeathFieldRadiusForTime)
		DetourSetup(&s_pDeathFieldRadiusForTime, &Hook_DeathFieldRadiusForTime, bAttach);
}


//=============================================================================
// S21->S3 Network Bridge: State Query
//=============================================================================
bool S21Bridge_IsActive()
{
	return s_bridgeActive;
}

double S21BridgeDiag_MsSinceEngineFrame(void)
{
	if (s_lastEngineFrameMs == 0.0)
		return -1.0;
	return static_cast<double>(GetTickCount64()) - s_lastEngineFrameMs;
}

//=============================================================================
// Translate 7-bit netmessage IDs S3<->S21. -1 = suppress.
//=============================================================================

// S3->S21 translation table (for incoming messages from S3 server)
// Index = S3 type ID, value = S21 type ID (-1 = suppress/skip)

// The old table was from analysis and had wrong type numbers for S21.
// S21 renumbered all svc_ types (inserted new types, different offsets from S3).
const int s_S3ToS21[69] = {
	/* 0 */ 0, // net_NOP
	/* 1 */ 1, // net_Disconnect
	/* 2 */ 2, // (reserved)
	/* 3 */ 3, // net_StringCmd [: RFB reads string(1024)]
	/* 4 */ 5, // net_SetConVar [: RFB reads 8b count + string pairs]
	/* 5 */ 6, // net_SignonState [: S21 slot 6 -- FORMAT BRIDGE (22-bit extra fields)]
	/* 6 */ 7, // net_MTXUserMsg
	/* 7 */ 11, // svc_ServerInfo [: RFB reads 16b+32b+bools+fields+strings -- FORMAT CHANGED]
	/* 8 */ 12, // svc_SendTable
	/* 9 */ 13, // svc_ClassInfo
	/* 10 */ -1, // svc_SetPause (removed in S21)
	/* 11 */ 14, // svc_Playlists [: RFB reads 1b+32b+data, shared with slot 10]
	/* 12 */ 15, // svc_CreateStringTable [: RFB reads string+16b+log2+24b+flags+seek]
	/* 13 */ 16, // svc_UpdateStringTable
	/* 14 */ 17, // svc_VoiceData
	/* 15 */ 18, // svc_DurangoVoiceData
	/* 16 */ -2, // svc_Print [S21 adds 4-bit prefix -- FORMAT BRIDGE]
	/* 17 */ -2, // svc_Sounds [now dedi-bridge-native, see game/server/sound.cpp on the dedi -- FORMAT BRIDGE, was an unverified bare mapping]
	/* 18 */ 21, // svc_FixAngle [: RFB reads 3 floats]
	/* 19 */ 22, // svc_CrosshairAngle
	/* 20 */ 23, // svc_GrantClientSidePickup
	/* 21 */ -1, // (gap in S3)
	/* 22 */ -2, // svc_ServerTick [S21 type TBD -- skip for now, format changed]
	/* 23 */ 25, // svc_PersistenceDefFile [VERIFIED: msgArray[25] vtbl GetType=25 and its vtable is the one SendServerInfo writes on the full-send path; 26 is UseCached]
	/* 24 */ 26, // svc_UseCachedPersistenceDefFile [VERIFIED: slot 26 RFB reads 64-bit version into msg+32, matches s3. Old table mapped to 27 = Baseline -- latent bug, s3=24 hasn't flowed yet]
	/* 25 */ 27, // svc_PersistenceBaseline [ verified: slot 27 vtbl= -> ]
	/* 26 */ 28, // svc_PersistenceUpdateVar [verified: msgArray[28] vtbl GetType=28, RFB, Process slot 30 -> wrapper -> apply]
	/* 27 */ 29, // svc_PersistenceNotifySaved [verified: msgArray[29] vtbl GetType=29, Process slot 31 -> (++savedCount@+132184, savedVer@+132180). Old table mapped to 30 which is actually DLCNotifyOwnership]
	/* 28 */ -1, // svc_DLCNotifyOwnership -- BODY-SKIP (64b bitfield + N*16 balances; N via bridge_s3_dlc_balance_count)
	/* 29 */ 31, // svc_MatchmakingETAs
	/* 30 */ 32, // svc_MatchmakingStatus
	/* 31 */ 33, // svc_MTXUserInfo
	/* 32 */ -2, // svc_PlaylistChange [S21 type TBD -- skip for now, format changed to 7b index]
	/* 33 */ 35, // svc_SetTeam [S21 35 reads 7 bits like S3; 36 is the 15b-length blob]
	/* 34 */ -1, // svc_PlaylistOverrides (removed in S21)
	/* 35 */ -1, // svc_AntiCheat -- consume-and-drop; never native ProcessAntiCheat
	/* 36 */ -1, // svc_AntiCheatChallenge (removed in S21)
	/* 37 */ 49, // svc_UserMessage -- PLACEHOLDER INDEX, NEVER DISPATCHED. S21 slot
	              // Slot 49 is svc_Menu, not svc_UserMessage. Keep it in range; s3cmd==37 dispatches native.
	/* 38 */ -1, // (gap in S3)
	/* 39 */ -1, // (gap in S3)
	/* 40 */ 50, // svc_Snapshot
	/* 41 */ 48, // svc_TempEntities -- S21 type 48 (NOT 51; 51 is an unrelated inline-records msg).: RFB reads 32b m_tick(@+0x88) + 10b m_nNumEntries(@+0x20) + 24b m_nLength + blob, then Seek -- field offsets match S3. FORMAT BRIDGE (S3 22b len -> S21 24b len).
	/* 42 */ -1, // svc_Menu -- suppressed. The old mapping to S21 52 was wrong (52's
	              // Dedi never emits VGUI menu; do not feed unverified KV to the binary parser.
	/* 43 */ -1, // svc_CmdKeyValues (removed in S21)
	/* 44 */ -2, // svc_DatatableChecksum -- FORMAT BRIDGE (S3 [64b checksum][string];
	              // S21 slot 53 body differs and under-consumed, shearing the stream)
	/* 45 */ -1, // clc_ClientInfo
	/* 46 */ -1, // clc_Move -- -VERIFIED via CLC_Move::GetType (= `return 57`)
	/* 47 */ -1, // clc_VoiceData
	/* 48 */ -1, // clc_DurangoVoiceData
	/* 49 */ -1, // (gap in S3)
	/* 50 */ -1, // clc_FileCRCCheck (removed in S21)
	/* 51 */ -1, // (gap in S3)
	/* 52 */ -1, // clc_LoadingProgress (UNVERIFIED on S21 -- type may differ)
	/* 53 */ -1, // clc_PersistenceRequestSave (removed in S21)
	/* 54 */ -1, // clc_PersistenceClientToken
	/* 55 */ -1, // clc_SetClientEntitlements
	/* 56 */ -1, // clc_SetPlaylistVarOverride (removed in S21)
	/* 57 */ -1, // clc_ClaimClientSidePickup (UNVERIFIED on S21 -- type may differ)
	/* 58 */ -1, // (gap in S3)
	/* 59 */ -1, // clc_CmdKeyValues (removed in S21)
	/* 60 */ -1, // clc_ClientTick -- -VERIFIED via CLC_ClientTick::GetType (= `return 65`)
	/* 61 */ -1, // clc_ClientSayText -> S21 71 DISPROVEN (S21 71 is binary, not chat); -symbol guess. Real S21 chat type unknown.
	/* 62 */ -1, // clc_PINTelemetryData (removed in S21)
	/* 63 */ -1, // clc_AntiCheat -- consume-and-drop; never native ProcessAntiCheat
	/* 64 */ -1, // clc_AntiCheatChallenge (removed in S21)
	/* 65 */ -1, // clc_GamepadMsg
	/* 66 */ -1, // svc_SetClassVar (SDK custom -- not sent to S21)
	/* 67 */ -1, // svc_SystemSayText (SDK custom -- not sent to S21)
	/* 68 */ -1, // net_ScriptMessage (SDK custom -- not sent to S21)
};

void S21Bridge_SendDataBlockAck()
{
	if (!s_origSendto || s_bridgeSocket == INVALID_SOCKET)
		return;

	// C2S_DATABLOCK_ACK 'P'=0x50: transferId, transferNr, 1-bit bitmap flag.
	// transferId == currentId+1 ACKs the whole transfer.
	uint8_t ackBuf[64];
	int pos = 0;

	// WriteLong: CONNECTIONLESS_HEADER
	uint32_t hdr = 0xFFFFFFFF;
	memcpy(ackBuf + pos, &hdr, 4); pos += 4;

	// WriteByte: C2S_DATABLOCK_ACK
	ackBuf[pos++] = 0x50;

	// WriteShort: transferId + 1 (signals "all blocks received, transfer complete")
	uint16_t tid = (uint16_t)(s_dbTransferId + 1);
	memcpy(ackBuf + pos, &tid, 2); pos += 2;

	// WriteShort: transferNr
	uint16_t tnr = (uint16_t)s_dbTransferNr;
	memcpy(ackBuf + pos, &tnr, 2); pos += 2;

	// WriteBit: per-block ACK flag = 0 (no per-block bitmap needed, transferId+1
	// triggers the "complete all" fast path on the server regardless)
	ackBuf[pos++] = 0x00;

	const int ackResult = s_origSendto(s_bridgeSocket,
		reinterpret_cast<const char*>(ackBuf), pos, 0,
		reinterpret_cast<const sockaddr*>(&s_bridgeDest),
		sizeof(s_bridgeDest));
	const int ackErr = (ackResult <= 0) ? WSAGetLastError() : 0;

	SDK_Log("[BRIDGE-DB] sent ACK: transferId=%d(+1=%d) transferNr=%d sendto=%d wsa=%d\n",
		s_dbTransferId, tid, s_dbTransferNr, ackResult, ackErr);
}

void S21Bridge_OnDataBlockComplete(const uint8_t* rawBuf, int rawSize)
{
	SDK_Log("[BRIDGE-DB] TRANSFER #%d COMPLETE: %d blocks, %d bytes total\n",
		s_dbTransferCount + 1, s_dbTotalBlocks, s_dbTransferSize);

	char dbTag[48];
	snprintf(dbTag, sizeof(dbTag), "DataBlock TRANSFER #%d COMPLETE",
		s_dbTransferCount + 1);
	SDK_LogRSS(dbTag);

	// Send ACK to server to stop retransmissions
	S21Bridge_SendDataBlockAck();

	// Check compression flag (byte 0 of reassembled data = ServerDataBlockHeader_s.isCompressed)
	const bool isCompressed = rawBuf[0] != 0;
	const uint8_t* signonData = nullptr;
	int signonSize = 0;

	if (isCompressed)
	{
		// LZ4 decompress (LZ4 is linked: thirdparty/lz4/lz4.h via stdafx.h)
		static uint8_t s_decompBuf[BRIDGE_DB_SCRATCH_SIZE];
		const int compressedSize = rawSize - 1; // minus the 1-byte compression flag
		const int decompSize = LZ4_decompress_safe(
			reinterpret_cast<const char*>(rawBuf + 1),
			reinterpret_cast<char*>(s_decompBuf),
			compressedSize,
			BRIDGE_DB_SCRATCH_SIZE);

		if (decompSize < 0)
		{
			SDK_Log("[BRIDGE-DB] ERROR: LZ4 decompression failed (ret=%d), compressedSize=%d\n",
				decompSize, compressedSize);
			s_dbComplete = true;
			s_dbTransferCount++;
			return;
		}

		signonData = s_decompBuf;
		signonSize = decompSize;
		SDK_Log("[BRIDGE-DB] LZ4 decompressed: %d -> %d bytes\n", compressedSize, decompSize);
	}
	else
	{
		signonData = rawBuf + 1;
		signonSize = rawSize - 1;
		SDK_Log("[BRIDGE-DB] uncompressed signon data: %d bytes\n", signonSize);
	}

	// (signon-transfer disk dump removed: it was an ungated synchronous fwrite of up
	// to 86 KB to the slow G: SMR HDD on the net thread inside NET_ReceiveDatagram,
	// widening the signon-injection deadlock window. Pure diagnostic, no consumer.)

	// Log first few bytes and scan for message types
	if (signonSize >= 4)
	{
		SDK_Log("[BRIDGE-DB] first 16 bytes: "
			"%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
			signonData[0], signonData[1], signonData[2], signonData[3],
			signonSize > 4 ? signonData[4] : 0, signonSize > 5 ? signonData[5] : 0,
			signonSize > 6 ? signonData[6] : 0, signonSize > 7 ? signonData[7] : 0,
			signonSize > 8 ? signonData[8] : 0, signonSize > 9 ? signonData[9] : 0,
			signonSize > 10 ? signonData[10] : 0, signonSize > 11 ? signonData[11] : 0,
			signonSize > 12 ? signonData[12] : 0, signonSize > 13 ? signonData[13] : 0,
			signonSize > 14 ? signonData[14] : 0, signonSize > 15 ? signonData[15] : 0);

		// Read first 7-bit message type ID (LSB-first bit packing)
		const uint32_t firstByte = signonData[0];
		const uint32_t msgType = firstByte & 0x7F; // 7 bits from first byte
		const int s21Type = (msgType < 69) ? s_S3ToS21[msgType] : -1;
		SDK_Log("[BRIDGE-DB] first message type: S3=%d -> S21=%d\n", msgType, s21Type);
	}

	bool tablesOk = true;
	if (signonSize > 0)
	{
		uint8_t* mutSignon = const_cast<uint8_t*>(signonData);
		tablesOk = S21Bridge_PreprocessSendTablesInBuffer(mutSignon, signonSize, "DB");
		if (!tablesOk)
		{
			Warning(eDLL_T::ENGINE,
				"[BRIDGE-ST:DB] rejected SendTables, dropping signon inject\n");
			S21Bridge_RequestValidatorDisconnect("DB");
		}
	}

	// Inject signon via ProcessMessages (S3 IDs translated at dispatch).

	// The cached channel must still be a live entry in the engine's registry;
	// a pointer that outlived a disconnect points at freed memory.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uintptr_t* pArrayPtr  = reinterpret_cast<uintptr_t*>(NetObs_CNetChanRegistryArrayPtrAddr());
			int*        pCount    = reinterpret_cast<int*>(NetObs_CNetChanRegistryCountAddr());
			int count = *pCount;
			uintptr_t arrayBase = *pArrayPtr;

			CNetChan* pLive = nullptr;
			bool cachedIsLive = false;
			if (arrayBase && count > 0 && count < 64)
			{
				for (int i = 0; i < count; i++)
				{
					CNetChan* pChan = *reinterpret_cast<CNetChan**>(arrayBase + 8 * i);
					if (!pChan)
						continue;
					if (pChan == s_bridgeChan)
						cachedIsLive = true;
					if (!pLive)
						pLive = pChan;
				}
			}

			if (!cachedIsLive)
			{
				if (s_bridgeChan)
					Warning(eDLL_T::ENGINE,
						"[BRIDGE-DB] cached CNetChan=%p not in engine registry (count=%d) -- %s\n",
						(void*)s_bridgeChan, count, pLive ? "re-resolved" : "dropping inject");
				s_bridgeChan = pLive;
				if (pLive)
					SDK_Log("[BRIDGE-DB] found CNetChan=%p from engine registry\n", (void*)pLive);
			}

			if (!s_bridgeChan)
				SDK_Log("[BRIDGE-DB] WARNING: no CNetChan in engine registry (count=%d)\n", count);
		}
	}

	if (tablesOk && s_bridgeChan && signonSize > 0)
	{
		SDK_Log("[BRIDGE-DB] injecting %d bytes via ProcessMessages (CNetChan=%p signon=%d)\n",
			signonSize, (void*)s_bridgeChan,
			(int)InterlockedCompareExchange(&s_bridgeSignonState, 0, 0));

		bf_read signonBuf(signonData, signonSize, signonSize * 8);

		// [SNAP-PROF] time the whole inject + count entity-creates vs table-decodes so we
		// can localize the ~9s in-game stall (re-create churn vs re-decode of existing ents).
		SnapProf_Reset();
		LARGE_INTEGER _spFreq, _spT0, _spT1;
		QueryPerformanceFrequency(&_spFreq);
		QueryPerformanceCounter(&_spT0);
		const bool result = S21Bridge_ProcessMessages(s_bridgeChan, &signonBuf);
		QueryPerformanceCounter(&_spT1);
		const double _spMs = (_spFreq.QuadPart > 0)
			? (double)(_spT1.QuadPart - _spT0.QuadPart) * 1000.0 / (double)_spFreq.QuadPart
			: 0.0;

		SDK_Log("[BRIDGE-DB] ProcessMessages returned %d, %d bits consumed of %d\n",
			result ? 1 : 0, signonBuf.GetNumBitsRead(), signonSize * 8);
		const double _spCreateMs = (_spFreq.QuadPart > 0)
			? (double)g_snapProfCreateTicks * 1000.0 / (double)_spFreq.QuadPart
			: 0.0;
		SDK_Log("[SNAP-PROF] inject %d bytes took %.1f ms: CL_CopyNewEntity %.1f ms (%.0f%% of inject) "
			"creates=%lld decodes=%lld signon=%d\n",
			signonSize, _spMs, _spCreateMs, (_spMs > 0.0 ? _spCreateMs / _spMs * 100.0 : 0.0),
			g_snapProfCreateN, g_snapProfDecodeN,
			(int)InterlockedCompareExchange(&s_bridgeSignonState, 0, 0));
		g_snapProfOn = false;
	}
	else if (tablesOk && !s_bridgeChan)
	{
		SDK_Log("[BRIDGE-DB] WARNING: no CNetChan found, cannot inject signon data\n");
	}

	s_dbComplete = true;
	s_dbTransferCount++;

}


// sdk_dump_player_sndc ConCommand removed (Bridge_DumpPlayerSNDC_f) --
// self-contained diagnostic dumper, no cross-file callers.

// sdk_dump_sndc_state ConCommand removed (Bridge_DumpSNDCState_f) --
// self-contained diagnostic dumper, no cross-file callers.

// sdk_dump_client_registry_full ConCommand removed (Bridge_DumpClientRegistryFull_f) --
// self-contained diagnostic dumper, no cross-file callers.

//-----------------------------------------------------------------------------
// sdk_dump_netvar_callbacks ConCommand removed (Bridge_DumpNetVarCallbacks_f) --
// self-contained diagnostic dumper, no cross-file callers.


// sdk_dump_pg_int32s_structs ConCommand removed (Bridge_DumpPgInt32sStructs_f) --
// self-contained diagnostic dumper, no cross-file callers.


//-----------------------------------------------------------------------------
// AttachInTxn: DetourAttach only. Nested Begin/Commit fails inside the registry txn.
//-----------------------------------------------------------------------------
bool AttachInTxn(PVOID* ppTarget, PVOID pDetour, const char* name)
{
	if (!ppTarget || !*ppTarget)
		return false;
	const LONG err = DetourAttach(ppTarget, pDetour);
	if (err != NO_ERROR)
	{
		SDK_Log("[NET-OBS] %s: DetourAttach (in-txn) failed (%ld)\n", name, err);
		return false;
	}
	return true;
}


//-----------------------------------------------------------------------------
// C_BoneFollower::TestCollision vtbl 51. m_hOwnerEntity +0x39C, m_modelIndex +2416,
// m_boneIndex +2420. studiohdr numBones u16@0x74. Skip trace on any bounds miss.
//-----------------------------------------------------------------------------
using PFN_EntityColltreeResolve = void* (__fastcall*)(__int64 a1, __int64 a2);
static PFN_EntityColltreeResolve v_EntityColltreeResolve = nullptr;
static uintptr_t s_pEntityEhandleTable = 0; // twin, 0x20-stride entries

static inline uint32_t BoneFollow_FixOffset(uint16_t v)
{
	// studiohdr packed self-relative offset (verified disasm @)
	return static_cast<uint32_t>(v & 0xFFFE) << (4 * (v & 1));
}

static void* __fastcall Hook_EntityColltreeResolve(__int64 a1, __int64 a2)
{
	// Validate m_boneIndex / coll-part against the client owner model; skip on miss.
	if (!bridge_bonefollow_guard.GetBool())
		return v_EntityColltreeResolve(a1, a2);

	bool skip = false;
	const char* skipWhy = nullptr;
	uint32_t entHandleRaw = 0;
	uint64_t entPtr = 0;
	int      hullSel = -1;
	char     ownerModelName[40] = {};
	int      ownerNumBones = -1;
	int      collPartOut = -2;
	int      hdrCountOut = -2;

	__try
	{
		const uint32_t packed = *reinterpret_cast<const uint32_t*>(a1 + 0x39C);
		entHandleRaw = packed;
		if (packed != 0xFFFFFFFFu && s_pEntityEhandleTable)
		{
			const uintptr_t tableEntry = s_pEntityEhandleTable
				+ (static_cast<uint64_t>(static_cast<uint16_t>(packed)) << 5);
			const uint32_t serial = *reinterpret_cast<const uint32_t*>(tableEntry + 8);
			if (serial == (packed >> 16))
				entPtr = *reinterpret_cast<const uint64_t*>(tableEntry);
		}
		const int16_t boneIdx = *reinterpret_cast<const int16_t*>(a1 + 2420);
		hullSel = boneIdx;

		// -- validation pass: re-derive exactly what will read --
		if (!entPtr)
		{
			// engine would deref (0 + 4096) unconditionally -> instant AV
			skip = true; skipWhy = "owner handle unresolved";
		}
		else
		{
			const uint64_t wrapper = *reinterpret_cast<const uint64_t*>(entPtr + 4096);
			const uint64_t hdr = wrapper ? *reinterpret_cast<const uint64_t*>(wrapper + 8) : 0;
			if (!hdr)
			{
				// CStudioHdr not built yet (engine lazily creates it inside the original;
				// skipping one trace this early is behaviorally invisible and avoids
				// validating against a header we cannot see)
				skip = true; skipWhy = "owner studiohdr not resident";
			}
			else
			{
				// debugname: inline 33-byte field at hdr+0xA, not guaranteed terminated
				for (int i = 0; i < 33; ++i)
				{
					const char ch = *reinterpret_cast<const char*>(hdr + 0xA + i);
					ownerModelName[i] = (ch >= 0x20 && ch < 0x7F) ? ch : (ch ? '?' : '\0');
					if (!ch) break;
				}
				ownerModelName[33] = '\0';

				const uint16_t numBones = *reinterpret_cast<const uint16_t*>(hdr + 0x74);
				ownerNumBones = numBones;
				if (boneIdx < 0 || boneIdx >= static_cast<int>(numBones))
				{
					skip = true; skipWhy = "m_boneIndex out of bone table";
				}
				else
				{
					const uint32_t boneOfs =
						BoneFollow_FixOffset(*reinterpret_cast<const uint16_t*>(hdr + 0x78));
					const int8_t collPart = *reinterpret_cast<const int8_t*>(
						hdr + boneOfs + 128u * static_cast<uint32_t>(boneIdx) + 124);
					collPartOut = collPart;
					const uint16_t bvhW = *reinterpret_cast<const uint16_t*>(hdr + 0xD6);
					// collPart < 0 or no collision block -> engine early-outs natively; passthrough
					if (collPart >= 0 && bvhW)
					{
						const uint64_t collBase = hdr + BoneFollow_FixOffset(bvhW);
						const int32_t hdrCount = *reinterpret_cast<const int32_t*>(collBase + 12);
						hdrCountOut = hdrCount;
						if (collPart >= hdrCount)
						{
							skip = true; skipWhy = "collPartIdx out of header table";
						}
						else
						{
							const uint32_t nodesOfs = *reinterpret_cast<const uint32_t*>(
								collBase + 4u * (10u * static_cast<uint32_t>(collPart) + 5u));
							if (((collBase + nodesOfs) & 15u) != 0)
							{
								skip = true; skipWhy = "node array base misaligned";
							}
						}
					}
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		// a faulting read while validating means the engine's own walk would fault too
		skip = true; skipWhy = "fault while validating";
	}

	if (skip)
	{
		static volatile LONG s_bfGuardLogs = 0;
		if (InterlockedIncrement(&s_bfGuardLogs) <= 50)
		{
			Warning(eDLL_T::CLIENT,
				"[BONEFOLLOW-GUARD] skipped bone-follower trace: %s (owner_handle=0x%08X "
				"owner_model='%s' numBones=%d m_boneIndex=%d collPart=%d headerCount=%d)\n",
				skipWhy ? skipWhy : "?", entHandleRaw,
				ownerModelName[0] ? ownerModelName : "?",
				ownerNumBones, hullSel, collPartOut, hdrCountOut);
		}
		return nullptr; // native no-collision early-out semantics (return value unused by callers)
	}

	return v_EntityColltreeResolve(a1, a2);
}

static void InstallObserverHooks_S21()
{
	int nHooked = 0;

	// --- [BONEFOLLOW-GUARD] (entity-hitbox collision-tree resolver) ---
	// Observer for base-district's Crash B path (see Hook_EntityColltreeResolve comment).

	// EHANDLE-table pointer (the same table the removed static-prop resolver did NOT
	// use) is reached via the 'lea rcx, <table>' at entry+0x2D inside this same match --
	// pattern-resolved off the found CMemory rather than hardcoded, since it's reachable here.
	{
		const CMemory entColltreeFn = Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 8B 81 ?? ?? ?? ?? 33 ED 48 8B F2 48 8B F9 83 F8");
		entColltreeFn.GetPtr(v_EntityColltreeResolve);
		s_pEntityEhandleTable = entColltreeFn.Offset(0x2D).ResolveRelativeAddress(3, 7).GetPtr();

		if (v_EntityColltreeResolve && s_pEntityEhandleTable)
		{
			if (AttachInTxn(reinterpret_cast<PVOID*>(&v_EntityColltreeResolve),
			              reinterpret_cast<PVOID>(&Hook_EntityColltreeResolve),
			              "EntityColltreeResolve"))
			{
				SDK_Log("[BONEFOLLOW-GUARD] hooked (orig at %p, ehandleTable at 0x%llX)\n",
					(void*)v_EntityColltreeResolve, (unsigned long long)s_pEntityEhandleTable);
				++nHooked;
			}
		}
		else
		{
			SDK_Log("[BONEFOLLOW-GUARD] pattern UNRESOLVED (fn=%p table=0x%llX) -- probe not installed\n",
				(void*)v_EntityColltreeResolve, (unsigned long long)s_pEntityEhandleTable);
		}
	}

	// --- Cbuf_AddText ---
	if (Cbuf_AddText)
	{
		s_origCbufAddText = Cbuf_AddText;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origCbufAddText),
		              reinterpret_cast<PVOID>(&Hook_Cbuf_AddText),
		              "Cbuf_AddText"))
		{
			SDK_Log("[NET-OBS] hooked Cbuf_AddText (orig at %p)\n", (void*)s_origCbufAddText);
			++nHooked;
		}
	}
	else
	{
		SDK_Log("[NET-OBS] Cbuf_AddText not resolved, skipping hook\n");
	}

	// GetPlayerNetTime / GetPlayerNetInt. m_times +3048, m_int32s +2912.
	// Migrated to VNetVarDiagS21.

	// [MIGRATED -> VNetFrameDiagS21] ZiprailPostDataUpdate

	// --- Cbuf_Execute ---
	// Only log first few calls; see Hook_Cbuf_Execute for why.
	if (Cbuf_Execute)
	{
		s_origCbufExecute = Cbuf_Execute;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origCbufExecute),
		              reinterpret_cast<PVOID>(&Hook_Cbuf_Execute),
		              "Cbuf_Execute"))
		{
			SDK_Log("[NET-OBS] hooked Cbuf_Execute (orig at %p)\n", (void*)s_origCbufExecute);
			++nHooked;
		}
	}
	else
	{
		SDK_Log("[NET-OBS] Cbuf_Execute not resolved, skipping hook\n");
	}

	// Cmd_ExecuteString: every parsed command, including flag-dropped ones.
	if (v_S21_Cmd_ExecuteString)
	{
		s_origCmdExecuteString = v_S21_Cmd_ExecuteString;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origCmdExecuteString),
		              reinterpret_cast<PVOID>(&Hook_Cmd_ExecuteString),
		              "Cmd_ExecuteString"))
		{
			SDK_Log("[NET-OBS] hooked Cmd_ExecuteString (orig at %p)\n",
				(void*)s_origCmdExecuteString);
			++nHooked;
		}
	}
	else
	{
		SDK_Log("[NET-OBS] Cmd_ExecuteString not resolved, skipping hook\n");
	}

	// Connect worker: address string + CClientState vtable dump.
	if (v_S21_Connect_Worker)
	{
		s_origConnectWorker = v_S21_Connect_Worker;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origConnectWorker),
		              reinterpret_cast<PVOID>(&Hook_Connect_Worker),
		              "Connect_Worker"))
		{
			SDK_Log("[NET-OBS] hooked Connect_Worker (orig at %p)\n",
				(void*)s_origConnectWorker);
			++nHooked;
		}
	}
	else
	{
		SDK_Log("[NET-OBS] Connect_Worker not resolved, skipping hook\n");
	}

	// NET_SendPacket chain: Connect_Worker -> SetSignonState(1) -> pub -> inner -> sendto.

	if (v_S21_NET_SendPacket_Public)
	{
		s_origNetSendPacketPublic = v_S21_NET_SendPacket_Public;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origNetSendPacketPublic),
		              reinterpret_cast<PVOID>(&Hook_NET_SendPacket_Public),
		              "NET_SendPacket(pub)"))
		{
			SDK_Log("[NET-OBS] hooked NET_SendPacket(pub) (orig at %p)\n",
				(void*)s_origNetSendPacketPublic);
			++nHooked;
		}
	}
	else
	{
		SDK_Log("[NET-OBS] NET_SendPacket(pub) not resolved, skipping hook\n");
	}

	if (v_S21_NET_SendPacket_Inner)
	{
		s_origNetSendPacketInner = v_S21_NET_SendPacket_Inner;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origNetSendPacketInner),
		              reinterpret_cast<PVOID>(&Hook_NET_SendPacket_Inner),
		              "NET_SendPacket(in)"))
		{
			SDK_Log("[NET-OBS] hooked NET_SendPacket(in) (orig at %p)\n",
				(void*)s_origNetSendPacketInner);
			++nHooked;
		}
	}
	else
	{
		SDK_Log("[NET-OBS] NET_SendPacket(in) not resolved, skipping hook\n");
	}

	if (v_S21_NET_SendLoopback)
	{
		s_origNetSendLoopback = v_S21_NET_SendLoopback;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origNetSendLoopback),
		              reinterpret_cast<PVOID>(&Hook_NET_SendLoopback),
		              "NET_SendLoopback"))
		{
			SDK_Log("[NET-OBS] hooked NET_SendLoopback (orig at %p)\n",
				(void*)s_origNetSendLoopback);
			++nHooked;
		}
	}
	else
	{
		SDK_Log("[NET-OBS] NET_SendLoopback not resolved, skipping hook\n");
	}

	if (v_S21_CBaseClient_SetSignonState)
	{
		s_origSetSignonState = v_S21_CBaseClient_SetSignonState;
		if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origSetSignonState),
		              reinterpret_cast<PVOID>(&Hook_CBaseClient_SetSignonState),
		              "CBaseClient::SetSignonState"))
		{
			SDK_Log("[NET-OBS] hooked CBaseClient::SetSignonState (orig at %p)\n",
				(void*)s_origSetSignonState);
			++nHooked;
		}
	}
	else
	{
		SDK_Log("[NET-OBS] CBaseClient::SetSignonState not resolved, skipping hook\n");
	}

	// sentinel guard -- prevents the case-3 deferred-replay
	// crash on memset(struct->ptr_at_0xA8,..., count) when ptr_at_0xA8 is
	// the 0xFFFFFFFFFFFFFFFF sentinel of an uninitialized engine struct.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			s_origCase3InitHelper = reinterpret_cast<PFN_Case3InitHelper>(
				NetObs_Sym(NetObsSym_t::Case3InitHelper));
			if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origCase3InitHelper),
			              reinterpret_cast<PVOID>(&Hook_Case3InitHelper),
			              "(case-3 init helper)"))
			{
				SDK_Log("[NET-OBS] hooked sentinel guard"
					"(orig at %p)\n", (void*)s_origCase3InitHelper);
				++nHooked;
			}
			else
			{
				s_origCase3InitHelper = nullptr;
			}
		}
	}

	// [MIGRATED -> VNetDecodeDiagS21] DataTable_SetupRecv+JsonParser+DatatableChecksum

	// Entity copy: vtable[16] can return 1 instead of a pointer. SEH + zero-fill.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			s_origEntityDataCopy = reinterpret_cast<PFN_EntityDataCopy>(
				NetObs_Sym(NetObsSym_t::EntityDataCopy));
			if (AttachInTxn(reinterpret_cast<PVOID*>(&s_origEntityDataCopy),
			              reinterpret_cast<PVOID>(&Hook_EntityDataCopy),
			              "EntityDataCopy"))
			{
				SDK_Log("[NET-OBS] hooked EntityDataCopy"
					"(orig at %p) -- SEH guard for bad entity ptrs\n",
					(void*)s_origEntityDataCopy);
				++nHooked;
			}
			else
			{
				s_origEntityDataCopy = nullptr;
			}
		}
	}

	// ExitProcess(1) suppressed while mid-signon. Other death hooks removed.
	{
		HMODULE hK32 = GetModuleHandleA("kernel32.dll");
		if (hK32)
		{
			s_origExitProcess = (PFN_ExitProcess)GetProcAddress(hK32, "ExitProcess");
			if (s_origExitProcess && AttachInTxn(reinterpret_cast<PVOID*>(&s_origExitProcess),
			                                    reinterpret_cast<PVOID>(&Hook_ExitProcess),
			                                    "kernel32!ExitProcess"))
			{
				SDK_Log("[BRIDGE] hooked kernel32!ExitProcess for signon-suppression (orig at %p)\n",
					(void*)s_origExitProcess);
				++nHooked;
			}

			// TerminateProcess backstop for dumpless PRESPAWN close.
			s_origTerminateProcess = (PFN_TerminateProcess)GetProcAddress(hK32, "TerminateProcess");
			if (s_origTerminateProcess && AttachInTxn(reinterpret_cast<PVOID*>(&s_origTerminateProcess),
			                                          reinterpret_cast<PVOID>(&Hook_TerminateProcess),
			                                          "kernel32!TerminateProcess"))
			{
				SDK_Log("[DEATH-OBS] hooked kernel32!TerminateProcess (log-only, orig at %p)\n",
					(void*)s_origTerminateProcess);
				++nHooked;
			}
		}

		// [DUMP-FULL] force HEAP into the engine crash minidump so the next crash
		// captures the smashed object (corruptor hunt). dbghelp is linked (dbghelp.lib).
		{
			HMODULE hDbg = GetModuleHandleA("dbghelp.dll");
			if (!hDbg) hDbg = LoadLibraryA("dbghelp.dll");
			if (hDbg)
			{
				s_origMiniDumpWriteDump = (PFN_MiniDumpWriteDump)GetProcAddress(hDbg, "MiniDumpWriteDump");
				if (s_origMiniDumpWriteDump && AttachInTxn(reinterpret_cast<PVOID*>(&s_origMiniDumpWriteDump),
				                                           reinterpret_cast<PVOID>(&Hook_MiniDumpWriteDump),
				                                           "dbghelp!MiniDumpWriteDump"))
				{
					SDK_Log("[DUMP-FULL] hooked dbghelp!MiniDumpWriteDump (force heap capture, orig at %p)\n",
						(void*)s_origMiniDumpWriteDump);
					++nHooked;
				}
			}
		}

		// [VEH-CRASH] process-wide first-chance observer -- catches a
		// worker-thread / heap-corruption fatal that bypasses the kernel32
		// exit APIs and the main-thread SEH guards.
		Bridge_InstallVEH();
	}

	// VEH removed; permanent decoder dummies. Logging from VEH re-entered CRT.
	SDK_Log("[NET-OBS] VEH handler removed (permanent dummies eliminate NULL RecvProps)\n");

	// --- ws2_32!sendto and !WSASendTo ---
	HMODULE hWs2 = GetModuleHandleA("ws2_32.dll");
	if (!hWs2)
		hWs2 = LoadLibraryA("ws2_32.dll"); // shouldn't happen -- engine loads it

	if (hWs2)
	{
		s_origWSASocketW = reinterpret_cast<PFN_WSASocketW>(GetProcAddress(hWs2, "WSASocketW"));
		s_origSendto     = reinterpret_cast<PFN_sendto>    (GetProcAddress(hWs2, "sendto"));
		s_origWSASendTo  = reinterpret_cast<PFN_WSASendTo> (GetProcAddress(hWs2, "WSASendTo"));
		s_origRecvfrom   = reinterpret_cast<PFN_recvfrom>  (GetProcAddress(hWs2, "recvfrom"));
		s_origWSARecvFrom= reinterpret_cast<PFN_WSARecvFrom>(GetProcAddress(hWs2, "WSARecvFrom"));

		// WSASocketW hook -- force dual-stack on AF_INET6 DGRAM sockets
		if (s_origWSASocketW && AttachInTxn(reinterpret_cast<PVOID*>(&s_origWSASocketW),
		                                  reinterpret_cast<PVOID>(&Hook_WSASocketW),
		                                  "ws2_32!WSASocketW"))
		{
			SDK_Log("[NET-OBS] hooked ws2_32!WSASocketW (orig at %p)\n", (void*)s_origWSASocketW);
			++nHooked;
		}

		if (s_origSendto && AttachInTxn(reinterpret_cast<PVOID*>(&s_origSendto),
		                              reinterpret_cast<PVOID>(&Hook_sendto),
		                              "ws2_32!sendto"))
		{
			SDK_Log("[NET-OBS] hooked ws2_32!sendto (orig at %p)\n", (void*)s_origSendto);
			++nHooked;
		}

		if (s_origWSASendTo && AttachInTxn(reinterpret_cast<PVOID*>(&s_origWSASendTo),
		                                 reinterpret_cast<PVOID>(&Hook_WSASendTo),
		                                 "ws2_32!WSASendTo"))
		{
			SDK_Log("[NET-OBS] hooked ws2_32!WSASendTo (orig at %p)\n", (void*)s_origWSASendTo);
			++nHooked;
		}

		if (s_origRecvfrom && AttachInTxn(reinterpret_cast<PVOID*>(&s_origRecvfrom),
		                                reinterpret_cast<PVOID>(&Hook_recvfrom),
		                                "ws2_32!recvfrom"))
		{
			SDK_Log("[NET-OBS] hooked ws2_32!recvfrom (orig at %p)\n", (void*)s_origRecvfrom);
			++nHooked;
		}

		if (s_origWSARecvFrom && AttachInTxn(reinterpret_cast<PVOID*>(&s_origWSARecvFrom),
		                                   reinterpret_cast<PVOID>(&Hook_WSARecvFrom),
		                                   "ws2_32!WSARecvFrom"))
		{
			SDK_Log("[NET-OBS] hooked ws2_32!WSARecvFrom (orig at %p)\n", (void*)s_origWSARecvFrom);
			++nHooked;
		}
	}
	else
	{
		SDK_Log("[NET-OBS] ws2_32.dll not available, skipping socket hooks\n");
	}
	(void)nHooked;
}

void VNetObserverDiagS21::Detour(const bool bAttach) const
{
	if (bAttach)
		InstallObserverHooks_S21();
}


int NetObserver_Install()
{
	if (s_bInstalled)
	{
		SDK_Log("[NET-OBS] already installed, ignoring\n");
		return 0;
	}
	s_bInstalled = true;

	// [SEC C8] DEVGATE: only neuter DEVELOPMENTONLY/HIDDEN when -devsdk is present
	// OR sdk_devgate_neuter is explicitly set. Ship default leaves both gates STOCK.
	const bool bDevGate =
		(CommandLine() && CommandLine()->CheckParm("-devsdk")) ||
		sdk_devgate_neuter.GetBool();
	if (!bDevGate)
	{
		SDK_Log("[SEC][DEVGATE] skip DEVELOPMENTONLY/HIDDEN neuter (need -devsdk or +sdk_devgate_neuter 1)\n");
	}
	else
	{
	// Patch IsFlagSet(FCVAR_DEVELOPMENTONLY) test so DEVELOPMENTONLY convars remain settable from console and launch args.
	// Flip the mov edx,2 immediate to 0 in the matched gate; leave the FCVAR_CHEAT gate alone.
	{
		CMemory sigDevGate = Module_FindPattern(g_GameDll,
			"48 8B 03 BA 00 00 00 10 48 8B CB FF 50 10 84 C0 0F 84 ?? ?? ?? ?? 48 8B 03 BA 02 00 00 00 48 8B CB FF 50 10 84 C0 0F 85");
		uint8_t* const gate = sigDevGate.RCast<uint8_t*>();
		if (gate)
		{
			uint8_t* imm = nullptr;
			for (int i = 0; i < 48; ++i)
			{
				// the DEVELOPMENTONLY mov is `BA 02 00 00 00 48 8B CB` -- distinct
				// from the preceding CLIENTCMD_CAN_EXECUTE mov `BA 00 00 00 10`.
				if (gate[i] == 0xBA && gate[i + 1] == 0x02 && gate[i + 2] == 0x00 &&
					gate[i + 3] == 0x00 && gate[i + 4] == 0x00 &&
					gate[i + 5] == 0x48 && gate[i + 6] == 0x8B && gate[i + 7] == 0xCB)
				{
					imm = &gate[i + 1];
					break;
				}
			}
			if (imm && *imm == 0x02)
			{
				DWORD oldProt = 0;
				if (VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &oldProt))
				{
					*imm = 0x00;
					VirtualProtect(imm, 1, oldProt, &oldProt);
					FlushInstructionCache(GetCurrentProcess(), imm, 1);
					SDK_Log("[DEVGATE] neutered FCVAR_DEVELOPMENTONLY console gate @ %p (2->0) -- all dev cvars now settable\n", (void*)imm);
					Warning(eDLL_T::ENGINE, "[DEVGATE] FCVAR_DEVELOPMENTONLY console gate neutered -- all dev cvars settable\n");
				}
				else
					SDK_Log("[DEVGATE] VirtualProtect failed err=%lu -- gate NOT patched\n", GetLastError());
			}
			else
				SDK_Log("[DEVGATE] DEVELOPMENTONLY mov not located in matched region -- gate NOT patched\n");
		}
		else
			SDK_Log("[DEVGATE] Cmd_ExecuteCommand dev-gate pattern unresolved -- NOT patched\n");
	}

	// ConVar set path: IsFlagSet(DEVELOPMENTONLY=2) and HIDDEN=16. Patch both immediates to 0.
	// HIDDEN neuter only with -devsdk / sdk_devgate_neuter.
	{
		CMemory sigCvarGate = Module_FindPattern(g_GameDll,
			"4C 8B F0 48 85 C0 0F 84 ?? ?? ?? ?? 4C 8B 00 BA 02 00 00 00 48 8B C8 41 FF 50 10 84 C0 0F 85 ?? ?? ?? ?? 4D 8B 06 BA 10 00 00 00 49 8B CE 41 FF 50 10 84 C0 0F 85");
		uint8_t* const cg = sigCvarGate.RCast<uint8_t*>();
		if (cg)
		{
			uint8_t* devImm = nullptr;
			uint8_t* hidImm = nullptr;
			for (int i = 0; i < 64; ++i)
			{
				if (!devImm && cg[i] == 0xBA && cg[i + 1] == 0x02 && cg[i + 2] == 0x00 &&
					cg[i + 3] == 0x00 && cg[i + 4] == 0x00 &&
					cg[i + 5] == 0x48 && cg[i + 6] == 0x8B && cg[i + 7] == 0xC8)
					devImm = &cg[i + 1];
				if (!hidImm && cg[i] == 0xBA && cg[i + 1] == 0x10 && cg[i + 2] == 0x00 &&
					cg[i + 3] == 0x00 && cg[i + 4] == 0x00 &&
					cg[i + 5] == 0x49 && cg[i + 6] == 0x8B && cg[i + 7] == 0xCE)
					hidImm = &cg[i + 1];
			}
			int patched = 0;
			DWORD oldProt = 0;
			if (devImm && *devImm == 0x02 && VirtualProtect(devImm, 1, PAGE_EXECUTE_READWRITE, &oldProt))
			{
				*devImm = 0x00;
				VirtualProtect(devImm, 1, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), devImm, 1);
				++patched;
			}
			if (hidImm && *hidImm == 0x10 && VirtualProtect(hidImm, 1, PAGE_EXECUTE_READWRITE, &oldProt))
			{
				*hidImm = 0x00;
				VirtualProtect(hidImm, 1, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), hidImm, 1);
				++patched;
			}
			if (patched == 2)
			{
				SDK_Log("[DEVGATE] neutered ConVar-set DEVELOPMENTONLY+HIDDEN gate @ %p -- all dev cvars now settable\n", (void*)cg);
				Warning(eDLL_T::ENGINE, "[DEVGATE] ConVar-set DEVELOPMENTONLY/HIDDEN gate neutered -- dev cvars now settable\n");
			}
			else
				SDK_Log("[DEVGATE] ConVar-set gate partial (patched=%d dev=%p hid=%p) -- CHECK\n", patched, (void*)devImm, (void*)hidImm);
		}
		else
			SDK_Log("[DEVGATE] ConVar-set gate pattern unresolved -- NOT patched\n");
	}
	} // bDevGate

	// (Bridge VEH-AV crash-capture handler removed; legacy CCrashHandler VEH
	// remains as the sole exception reporter.)

	// Mirror sdk_observe_net into g_bSdkObserveInit here (after static init).
	if (g_sdkObserveNet >= 1)
		g_bSdkObserveInit = true;

	int nHooked = 0;

	const bool isDx12Exe = NetObs_IsDx12Exe();
	if (isDx12Exe)
		SDK_Log("[NET-OBS] DX12 executable detected; using DX12 RVA map for direct install patches\n");

	// Dual-pool selector: mode==2 with gate closed => pool never allocated.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			const uintptr_t platState = NetObs_PlatStateAddr();
			const uintptr_t platGate = NetObs_PlatformModeGateAddr();
			const uintptr_t altInit = NetObs_AltEntityInitFlagAddr();
			const uintptr_t altPool = NetObs_AltEntityPoolAddr();
			const uintptr_t originPool = NetObs_OriginEntityPoolAddr();
			if (!platState || !platGate || !altInit || !altPool || !originPool)
			{
				Warning(eDLL_T::ENGINE,
					"[NET-OBS] PlatState addrs unresolved -- skipped BRIDGE-MODE dump\n");
			}
			else
			{
			int     mode    = *reinterpret_cast<int*>     (platState);
			uint8_t gate    = *reinterpret_cast<uint8_t*> (platGate);
			uint8_t initFlg = *reinterpret_cast<uint8_t*> (altInit);
			void*   poolS21 = *reinterpret_cast<void**>   (altPool);
			void*   poolS3  = *reinterpret_cast<void**>   (originPool);
			LPSTR   cmdline = GetCommandLineA();
			SDK_Log("[BRIDGE-MODE] =%d =%d =%d"
			        "=%p =%p\n",
			        mode, (int)gate, (int)initFlg, poolS21, poolS3);
			SDK_Log("[BRIDGE-MODE] cmdline=%s\n", cmdline ? cmdline : "(null)");
			BridgeStubDiag("[BRIDGE-MODE] mode=%d gate=%d initFlag=%d poolS21=%p poolS3=%p",
			               mode, (int)gate, (int)initFlg, poolS21, poolS3);
			BridgeStubDiag("[BRIDGE-MODE] cmdline=%s", cmdline ? cmdline : "(null)");
			}
		}
	}

	// Disable net_use_valve_relay -- S21 tries valve relay during signon
	// which crashes (NULL relay interface) in standalone/R5Flowstate setups.
	// ConVar struct at, integer value at +0x64.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			int* pValveRelay = reinterpret_cast<int*>(
				NetObs_Sym(NetObsSym_t::RelayCvarIntValue));
			int* pValveRelayInt = reinterpret_cast<int*>(
				NetObs_Sym(NetObsSym_t::RelayCvarFloatValue));
			if (!pValveRelay || !pValveRelayInt)
			{
				Warning(eDLL_T::ENGINE,
					"[NET-OBS] valve relay cvar addrs unresolved -- skip patch\n");
			}
			else
			{
				*pValveRelay = 0;
				*pValveRelayInt = 0;
				SDK_Log("[NET-OBS] patched net_use_valve_relay = 0 (disable valve relay)\n");
			}
		}
	}

	// S21 RecvTable name extraction is now lazy -- happens on first SVC_SendTable
	// encounter in S21Bridge_ProcessMessages, when the engine is fully initialized.

	// NOP the 'Client missing DT class' error call. NULL class pointer is already correct.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pPatch = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_SignonStateGuard));
			if (!pPatch)
			{
				Warning(eDLL_T::ENGINE,
					"[NET-OBS] ClassInfo patch site unresolved -- skip\n");
			}
			else if (pPatch[0] == 0xE8) // verify it's a CALL instruction
			{
				DWORD oldProt;
				VirtualProtect(pPatch, 5, PAGE_EXECUTE_READWRITE, &oldProt);
				memset(pPatch, 0x90, 5); // NOP x5
				VirtualProtect(pPatch, 5, oldProt, &oldProt);
				SDK_Log("[NET-OBS] patched ClassInfo: missing DT classes now silent\n");
			}
			else
			{
				SDK_Log("[NET-OBS] WARNING: ClassInfo patch site mismatch (expected E8, got %02X)\n",
					pPatch[0]);
			}
		}
	}

	// SetSignonState case 3: skip playlist check and client.dll CRC (path is S21-absent).
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			// Playlist validation bypass (jnz short -> jmp short)
			uint8_t* pPlaylist = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_PlaylistValidation));
			if (!pPlaylist)
			{
				Warning(eDLL_T::ENGINE,
					"[NET-OBS] playlist validation patch site unresolved -- skip\n");
			}
			else if (*pPlaylist == 0x75)
			{
				DWORD oldProt;
				VirtualProtect(pPlaylist, 1, PAGE_EXECUTE_READWRITE, &oldProt);
				*pPlaylist = 0xEB;
				VirtualProtect(pPlaylist, 1, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), pPlaylist, 1);
				SDK_Log("[NET-OBS] patched playlist validation: jnz->jmp @ %p\n", pPlaylist);
			}

			// Client DLL CRC bypass (0F 84 near jz -> 90 E9 nop+jmp)
			uint8_t* pCrc = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_ClientDllCrc));
			if (!pCrc)
			{
				Warning(eDLL_T::ENGINE,
					"[NET-OBS] client DLL CRC patch site unresolved -- skip\n");
			}
			else if (pCrc[0] == 0x0F && pCrc[1] == 0x84)
			{
				DWORD oldProt;
				VirtualProtect(pCrc, 2, PAGE_EXECUTE_READWRITE, &oldProt);
				pCrc[0] = 0x90; // NOP
				pCrc[1] = 0xE9; // JMP near (reuses the existing rel32 displacement)
				VirtualProtect(pCrc, 2, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), pCrc, 2);
				SDK_Log("[NET-OBS] patched client DLL CRC check: jz->jmp @ %p\n", pCrc);
			}

			// Force cl_loadBspFromServerInfo so ProcessServerInfo registers model_precache[1]; otherwise worldmodel stays NULL and Host_Error disconnects.
			// ConVar int gate at object+0x64 (m_nValue); set to 1.
			{
				int* pNValue = reinterpret_cast<int*>(
					NetObs_Sym(NetObsSym_t::GateCvarIntValue));
				float* pFValue = reinterpret_cast<float*>(
					NetObs_Sym(NetObsSym_t::GateCvarFloatValue));
				if (!pNValue || !pFValue)
				{
					Warning(eDLL_T::ENGINE,
						"[NET-OBS] cl_loadBspFromServerInfo gate addrs unresolved -- skip\n");
				}
				else
				{
				DWORD oldProt;
				VirtualProtect(pNValue, 8, PAGE_READWRITE, &oldProt);
				const int oldNValue = *pNValue;
				*pNValue = 1;
				*pFValue = 1.0f;
				VirtualProtect(pNValue, 8, oldProt, &oldProt);
				SDK_Log("[NET-OBS] patched cl_loadBspFromServerInfo: m_nValue %d -> 1 "
					"(forces native SetModel(1, m_szWorldModel) path during ServerInfo)\n",
					oldNValue);
				}
			}
		}
	}

	// Decoder builder: jump mismatch to next prop (LABEL_33) instead of return 0.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pPatch = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_DecodeAbort));
			if (!pPatch)
			{
				Warning(eDLL_T::ENGINE,
					"[NET-OBS] DecodeAbort patch site unresolved -- skip\n");
			}
			else if (pPatch[0] == 0x32 && pPatch[1] == 0xC0 && pPatch[2] == 0xEB && pPatch[3] == 0xE4)
			{
				// Name-miss + wire type DataTable: recurse into the wire sub-table against current recv.
				// Cave: test rbp; cmp [r15],0Ah; call recurse; jmp LABEL_33.
				void* cave = NULL;
				uintptr_t siteAddr = (uintptr_t)pPatch;
				for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
				     probe < siteAddr + 0x08000000;
				     probe += 0x10000)
				{
					cave = VirtualAlloc((void*)probe, 64, MEM_COMMIT | MEM_RESERVE,
						PAGE_EXECUTE_READWRITE);
					if (cave) break;
				}
				if (!cave)
				{
					SDK_Log("[NET-OBS] matcher transparency cave alloc FAILED; "
						"falling back to LABEL_33-skip patch\n");
					const uint8_t jmpToNextProp = isDx12Exe ? 0xBC : 0xBE;
					DWORD oldProt;
					VirtualProtect(pPatch, 4, PAGE_EXECUTE_READWRITE, &oldProt);
					pPatch[0] = 0xEB;
					pPatch[1] = jmpToNextProp;
					pPatch[2] = 0x90;
					pPatch[3] = 0x90;
					VirtualProtect(pPatch, 4, oldProt, &oldProt);
					SDK_Log("[NET-OBS] patched decoder builder (fallback): ALL prop mismatches -> skip\n");
				}
				else
				{
					uint8_t* c = reinterpret_cast<uint8_t*>(cave);
					memset(c, 0, 64);
					int pos = 0;

					// test rbp, rbp
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xED;
					// jne short skip (offset filled below)
					c[pos++] = 0x75; int jne1Pos = pos++; c[pos - 1] = 0;
					// cmp dword ptr [r15], 0Ah
					c[pos++] = 0x41; c[pos++] = 0x83; c[pos++] = 0x3F; c[pos++] = 0x0A;
					// jne short skip
					c[pos++] = 0x75; int jne2Pos = pos++; c[pos - 1] = 0;
					// mov rcx, rsi
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0xCE;
					// mov rdx, [rsp+98h]
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x94; c[pos++] = 0x24;
					c[pos++] = 0x98; c[pos++] = 0x00; c[pos++] = 0x00; c[pos++] = 0x00;
					// mov r8, [r15+70h]
					c[pos++] = 0x4D; c[pos++] = 0x8B; c[pos++] = 0x47; c[pos++] = 0x70;
					// mov r9, [rsp+0A8h]
					c[pos++] = 0x4C; c[pos++] = 0x8B; c[pos++] = 0x8C; c[pos++] = 0x24;
					c[pos++] = 0xA8; c[pos++] = 0x00; c[pos++] = 0x00; c[pos++] = 0x00;
					// call (RIP-relative)
					c[pos++] = 0xE8;
					{
						uintptr_t target = NetObs_Sym(NetObsSym_t::DecodeOwnerFunc);
						int32_t rel = (int32_t)(target - ((uintptr_t)c + pos + 4));
						memcpy(c + pos, &rel, 4);
						pos += 4;
					}
					// skip_transparency
					int skipPos = pos;
					c[jne1Pos] = (uint8_t)(skipPos - (jne1Pos + 1));
					c[jne2Pos] = (uint8_t)(skipPos - (jne2Pos + 1));
					// jmp loc_140230046 (LABEL_33)
					c[pos++] = 0xE9;
					{
						uintptr_t target = NetObs_Sym(NetObsSym_t::PatchSite_DecodeOwnerResume);
						int32_t rel = (int32_t)(target - ((uintptr_t)c + pos + 4));
						memcpy(c + pos, &rel, 4);
						pos += 4;
					}

					// Patch original 4 bytes + 1 alignment-padding byte (5 bytes total)
					// with E9 rel32 -> cave.
					DWORD oldProt;
					VirtualProtect(pPatch, 5, PAGE_EXECUTE_READWRITE, &oldProt);
					pPatch[0] = 0xE9;
					int32_t caveRel = (int32_t)((uintptr_t)cave - ((uintptr_t)pPatch + 5));
					memcpy(pPatch + 1, &caveRel, 4);
					VirtualProtect(pPatch, 5, oldProt, &oldProt);

					SDK_Log("[NET-OBS] patched matcher transparency: unmatched DataTable "
						"children recurse against current recv table (cave=%p, %d bytes)\n",
						cave, pos);
				}
			}
			else
			{
				SDK_Log("[NET-OBS] WARNING: decoder builder patch site mismatch "
					"(expected 32 C0 EB E4, got %02X %02X %02X %02X)\n",
					pPatch[0], pPatch[1], pPatch[2], pPatch[3]);
			}

			Warning(eDLL_T::ENGINE, "[NET-OBS] CreateDecoders patch: entering install block\n");
			// CreateDecoders: write &s_globalDummy instead of NULL into decoder->m_Props[idx].
			{
				EnsureGlobalDummy();
				uint8_t* pNull = reinterpret_cast<uint8_t*>(
					NetObs_Sym(NetObsSym_t::PatchSite_NullClassDummy));
				if (pNull[0] == 0x49 && pNull[1] == 0x8B && pNull[2] == 0x04 && pNull[3] == 0x24
				    && pNull[4] == 0x4A && pNull[5] == 0x89 && pNull[6] == 0x14 && pNull[7] == 0xF0)
				{
					void* cave = NULL;
					uintptr_t siteAddr = (uintptr_t)pNull;
					for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
					     probe < siteAddr + 0x08000000;
					     probe += 0x10000)
					{
						cave = VirtualAlloc((void*)probe, 96, MEM_COMMIT | MEM_RESERVE,
							PAGE_EXECUTE_READWRITE);
						if (cave) break;
					}
					if (cave)
					{
						uint8_t* c = reinterpret_cast<uint8_t*>(cave);
						memset(c, 0, 96);
						int pos = 0;

						// mov rax, [r12] (original instruction 1)
						c[pos++] = 0x49; c[pos++] = 0x8B; c[pos++] = 0x04; c[pos++] = 0x24;
						// test rdx, rdx
						c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xD2;
						// jz use_dummy
						c[pos++] = 0x74;
						int jzDummyOff = pos++;
						// mov rcx, rdx
						c[pos++] = 0x48; c[pos++] = 0x89; c[pos++] = 0xD1;
						// shr rcx, 47 (valid user pointers have high bits clear)
						c[pos++] = 0x48; c[pos++] = 0xC1; c[pos++] = 0xE9; c[pos++] = 0x2F;
						// jne use_dummy
						c[pos++] = 0x75;
						int jneHighOff = pos++;
						// test dl, 7 (RecvProp pointers are at least 8-byte aligned)
						c[pos++] = 0xF6; c[pos++] = 0xC2; c[pos++] = 0x07;
						// jne use_dummy
						c[pos++] = 0x75;
						int jneAlignOff = pos++;
						// movabs rcx, 0x0000010000000000 (below normal Win64 heap/module ranges)
						c[pos++] = 0x48; c[pos++] = 0xB9;
						uintptr_t minUserPtr = 0x0000010000000000ULL;
						memcpy(c + pos, &minUserPtr, 8);
						pos += 8;
						// cmp rdx, rcx
						c[pos++] = 0x48; c[pos++] = 0x39; c[pos++] = 0xCA;
						// jb use_dummy
						c[pos++] = 0x72;
						int jbLowOff = pos++;
						// jmp real_write
						c[pos++] = 0xEB;
						int jmpRealOff = pos++;
						// use_dummy
						int useDummyPos = pos;
						// movabs rdx, &s_globalDummy (10 bytes)
						c[pos++] = 0x48; c[pos++] = 0xBA;
						uintptr_t dummyAddr = (uintptr_t)&s_globalDummy;
						memcpy(c + pos, &dummyAddr, 8);
						pos += 8;
						int realWritePos = pos;
						c[jzDummyOff]  = (uint8_t)(useDummyPos - jzDummyOff - 1);
						c[jneHighOff]  = (uint8_t)(useDummyPos - jneHighOff - 1);
						c[jneAlignOff] = (uint8_t)(useDummyPos - jneAlignOff - 1);
						c[jbLowOff]    = (uint8_t)(useDummyPos - jbLowOff - 1);
						c[jmpRealOff]  = (uint8_t)(realWritePos - jmpRealOff - 1);
						// real_write: mov [rax+r14*8], rdx (original instruction 2)
						c[pos++] = 0x4A; c[pos++] = 0x89; c[pos++] = 0x14; c[pos++] = 0xF0;
						// jmp back to
						c[pos++] = 0xE9;
						{
							uintptr_t target = NetObs_Sym(NetObsSym_t::PatchSite_NullClassDummyResume);
							int32_t rel = (int32_t)(target - ((uintptr_t)c + pos + 4));
							memcpy(c + pos, &rel, 4);
							pos += 4;
						}

						// Patch original 8 bytes: JMP cave + 3 NOPs
						DWORD oldProt;
						VirtualProtect(pNull, 8, PAGE_EXECUTE_READWRITE, &oldProt);
						pNull[0] = 0xE9;
						int32_t caveRel = (int32_t)((uintptr_t)cave - ((uintptr_t)pNull + 5));
						memcpy(pNull + 1, &caveRel, 4);
						pNull[5] = 0x90;
						pNull[6] = 0x90;
						pNull[7] = 0x90;
						VirtualProtect(pNull, 8, oldProt, &oldProt);

						Warning(eDLL_T::ENGINE,
							"[NET-OBS] patched CreateDecoders NULL/bad RecvProp write -> &s_globalDummy (%p) cave=%p\n",
							(void*)&s_globalDummy, cave);
					}
					else
					{
						Warning(eDLL_T::ENGINE,
							"[NET-OBS] CreateDecoders NULL-fill cave alloc FAILED\n");
					}
				}
				else
				{
					Warning(eDLL_T::ENGINE,
						"[NET-OBS] CreateDecoders NULL write site MISMATCH: %02X %02X %02X %02X %02X %02X %02X %02X\n",
						pNull[0], pNull[1], pNull[2], pNull[3],
						pNull[4], pNull[5], pNull[6], pNull[7]);
				}
			}
		}
	}

	// NULL RecvProp crash is handled by VEH-SKIP at +0x2E150B.
	// No Detour needed -- VEH skips the faulting instruction to loop increment.

	// GenerateDeltas: fill NULL RecvProp slots with dummies before the orig call.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			s_origGenerateDeltas = (PFN_GenerateDeltas)
				NetObs_Sym(NetObsSym_t::GenerateDeltas);
			if (AttachOne(reinterpret_cast<PVOID*>(&s_origGenerateDeltas),
			              reinterpret_cast<PVOID>(&Hook_GenerateDeltas),
			              "GenerateDeltas"))
			{
				SDK_Log("[NET-OBS] hooked GenerateDeltas for NULL RecvProp fill\n");
				++nHooked;
			}
		}
	}

	// WriteBlock dummy-skip (Path B). Marker pVarName=="?_unmatched".
	// Type 5 arrays: Bridge_ConsumeUnmatchedArrayElems then skip the write.
	// Site: mov r8d,[rsp+0x68] -> jmp cave. rbx=RecvProp, rdi=SendProp.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pSite = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_DeltaGuardA));
			Warning(eDLL_T::ENGINE,
				"[NET-OBS] WriteBlock patch site %p bytes pre: %02X %02X %02X %02X %02X\n",
				pSite, pSite[0], pSite[1], pSite[2], pSite[3], pSite[4]);
			if (pSite[0] == 0x44 && pSite[1] == 0x8B && pSite[2] == 0x44
			    && pSite[3] == 0x24 && pSite[4] == 0x68)
			{
				// DX12 twin uses the same site with a fixed VA delta (see SelectRuntimeVA args).
				uint8_t* pSkip = reinterpret_cast<uint8_t*>(
					NetObs_Sym(NetObsSym_t::PatchSite_DeltaGuardSkip));
				if (!(pSkip[0] == 0x4C && pSkip[1] == 0x8B && pSkip[2] == 0xB5
				      && pSkip[3] == 0x38 && pSkip[4] == 0x1D
				      && pSkip[5] == 0x00 && pSkip[6] == 0x00))
				{
					Warning(eDLL_T::ENGINE,
						"[NET-OBS] WriteBlock skip target mismatch at %p "
						"(expected 4C 8B B5 38 1D 00 00, got %02X %02X %02X %02X %02X %02X %02X) "
						"-- patch not applied\n",
						pSkip,
						pSkip[0], pSkip[1], pSkip[2], pSkip[3],
						pSkip[4], pSkip[5], pSkip[6]);
				}
				else
				{
					// Allocate cave WITHIN +/-2GB of the patch site so rel32
					// jumps fit. Same scan pattern as the entity-init caves.
					void* cave = NULL;
					uintptr_t siteAddr = (uintptr_t)pSite;
					for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
					     probe < siteAddr + 0x08000000;
					     probe += 0x10000)
					{
						cave = VirtualAlloc((void*)probe, 64, MEM_COMMIT | MEM_RESERVE,
							PAGE_EXECUTE_READWRITE);
						if (cave) break;
					}
					intptr_t dist = 0;
					if (cave)
					{
						dist = (intptr_t)((uintptr_t)cave - siteAddr);
						if (dist > 0x7FFFFF00LL || dist < -0x7FFFFF00LL)
						{
							Warning(eDLL_T::ENGINE,
								"[NET-OBS] WriteBlock cave at %p too far from site %p (dist=%lld) -- aborting\n",
								cave, pSite, (long long)dist);
							VirtualFree(cave, 0, MEM_RELEASE);
							cave = NULL;
						}
					}
					if (cave)
					{
						uint8_t* c = reinterpret_cast<uint8_t*>(cave);
						memset(c, 0, 256);

						// Resolve the per-prop wire decoder table from the WriteBlock array-element call (disp32 from live insn).
						// 43 FF 94 D4 <disp32> call [r12 + r10*8 + disp32]; r12 = image base.
						{
							const uint8_t* callIns = pSite + 0xCD;
							if (callIns[0] == 0x43 && callIns[1] == 0xFF &&
							    callIns[2] == 0x94 && callIns[3] == 0xD4)
							{
								uint32_t rva = *reinterpret_cast<const uint32_t*>(callIns + 4);
								s_decoderFuncsTable = base + rva;
								Warning(eDLL_T::ENGINE,
									"[NET-OBS] WriteBlock: funcs table resolved @ %p (rva=0x%X)\n",
									(void*)s_decoderFuncsTable, rva);
							}
							else
							{
								Warning(eDLL_T::ENGINE,
									"[NET-OBS] WriteBlock: array-decode call mismatch at %p "
									"(%02X %02X %02X %02X) -- dummy-skip NOT installed\n",
									callIns, callIns[0], callIns[1], callIns[2], callIns[3]);
							}
						}

						if (!s_decoderFuncsTable)
						{
							Warning(eDLL_T::ENGINE,
								"[NET-OBS] WriteBlock: no decoder table -- refusing dummy-skip cave\n");
						}
						else
						{

						int pos = 0;
						int relRP[5];          // rel8 operand offsets of the jumps to real_prop
						int nRP = 0;
						int relSkip = 0;       // rel8 operand offset of the jne to skip
						int lockDisp = 0;      // disp32 operand offset of the counter lock-inc

						// test rbx, rbx; jz real_prop
						c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xDB;
						c[pos++] = 0x74; relRP[nRP++] = pos; c[pos++] = 0x00;
						// mov rax, [rbx+0x28]; test rax, rax; jz real_prop
						c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x43; c[pos++] = 0x28;
						c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
						c[pos++] = 0x74; relRP[nRP++] = pos; c[pos++] = 0x00;
						// cmp byte [rax], '?'; jne real_prop
						c[pos++] = 0x80; c[pos++] = 0x38; c[pos++] = 0x3F;
						c[pos++] = 0x75; relRP[nRP++] = pos; c[pos++] = 0x00;
						// cmp byte [rax+1], '_'; jne real_prop
						c[pos++] = 0x80; c[pos++] = 0x78; c[pos++] = 0x01; c[pos++] = 0x5F;
						c[pos++] = 0x75; relRP[nRP++] = pos; c[pos++] = 0x00;
						// cmp byte [rax+2], 'u'; jne real_prop
						c[pos++] = 0x80; c[pos++] = 0x78; c[pos++] = 0x02; c[pos++] = 0x75;
						c[pos++] = 0x75; relRP[nRP++] = pos; c[pos++] = 0x00;
						// --- dummy confirmed ---
						// lock inc dword [rip+disp32] (counter slot at cave+0x80)
						c[pos++] = 0xF0; c[pos++] = 0xFF; c[pos++] = 0x05;
						lockDisp = pos; pos += 4;
						// cmp dword [rdi], 5; SendProp.m_Type == DPT_Array?
						c[pos++] = 0x83; c[pos++] = 0x3F; c[pos++] = 0x05;
						// jne skip; non-array dummy -> plain skip
						c[pos++] = 0x75; relSkip = pos; c[pos++] = 0x00;
						// --- dummy ARRAY: consume count*elem element bits ---
						// mov rcx, rdi; arg0 = SendProp (v55)
						c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0xCF;
						// mov rdx, [rbp+0x1D28]; arg1 = wire bf_read (a8)
						c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x95;
						c[pos++] = 0x28; c[pos++] = 0x1D; c[pos++] = 0x00; c[pos++] = 0x00;
						// mov r8d, [rbp-0x18]; arg2 = element count (v128[0])
						c[pos++] = 0x44; c[pos++] = 0x8B; c[pos++] = 0x45; c[pos++] = 0xE8;
						// sub rsp, 0x20; shadow space (rsp stays 16-aligned)
						c[pos++] = 0x48; c[pos++] = 0x83; c[pos++] = 0xEC; c[pos++] = 0x20;
						// mov rax, &Bridge_ConsumeUnmatchedArrayElems; call rax
						c[pos++] = 0x48; c[pos++] = 0xB8;
						{
							uint64_t hp = reinterpret_cast<uint64_t>(&Bridge_ConsumeUnmatchedArrayElems);
							memcpy(c + pos, &hp, 8); pos += 8;
						}
						c[pos++] = 0xFF; c[pos++] = 0xD0;
						// add rsp, 0x20
						c[pos++] = 0x48; c[pos++] = 0x83; c[pos++] = 0xC4; c[pos++] = 0x20;
						// skip: jmp mov r14,[rbp+0x1D38] (reload before LABEL_102)
						const int posSkip = pos;
						c[pos++] = 0xE9;
						{
							// DX12 twin: same patch site via SelectRuntimeVA pair.
							uintptr_t target = NetObs_Sym(NetObsSym_t::PatchSite_DeltaGuardSkip);
							int32_t rel = (int32_t)(target - ((uintptr_t)c + pos + 4));
							memcpy(c + pos, &rel, 4); pos += 4;
						}
						// real_prop: re-execute the displaced instruction, resume
						const int posRealProp = pos;
						c[pos++] = 0x44; c[pos++] = 0x8B; c[pos++] = 0x44;
						c[pos++] = 0x24; c[pos++] = 0x68;            // mov r8d, [rsp+0x68]
						c[pos++] = 0xE9;                             // jmp loc_14023307C
						{
							uintptr_t target = NetObs_Sym(NetObsSym_t::PatchSite_DeltaGuardAResume);
							int32_t rel = (int32_t)(target - ((uintptr_t)c + pos + 4));
							memcpy(c + pos, &rel, 4); pos += 4;
						}

						// Back-patch the rel8 branch displacements now that the
						// real_prop / skip anchors are known.
						for (int k = 0; k < nRP; ++k)
							c[relRP[k]] = (uint8_t)(posRealProp - (relRP[k] + 1));
						c[relSkip] = (uint8_t)(posSkip - (relSkip + 1));

						// Counter slot at cave+0x80 (well past the ~92-byte body).
						{
							int32_t cntDisp = 0x80 - (lockDisp + 4);
							memcpy(c + lockDisp, &cntDisp, 4);
						}
						s_pWriteBlockSkipCounter = reinterpret_cast<volatile LONG*>(c + 0x80);

						// Patch original site: jmp cave (5 bytes, no NOPs needed)
						DWORD oldProt;
						VirtualProtect(pSite, 5, PAGE_EXECUTE_READWRITE, &oldProt);
						pSite[0] = 0xE9;
						int32_t caveRel = (int32_t)((uintptr_t)cave - ((uintptr_t)pSite + 5));
						memcpy(pSite + 1, &caveRel, 4);
						VirtualProtect(pSite, 5, oldProt, &oldProt);

						Warning(eDLL_T::ENGINE,
							"[NET-OBS] WriteBlock dummy-skip INSTALLED: cave=%p (dist=%+lld) "
							"counter=%p site_post=%02X %02X %02X %02X %02X\n",
							cave, (long long)dist, s_pWriteBlockSkipCounter,
							pSite[0], pSite[1], pSite[2], pSite[3], pSite[4]);
						}
					}
					else
					{
						Warning(eDLL_T::ENGINE,
							"[NET-OBS] WriteBlock cave allocation FAILED near %p -- patch not applied\n",
							pSite);
					}
				}
			}
			else
			{
				Warning(eDLL_T::ENGINE,
					"[NET-OBS] WriteBlock patch site mismatch (expected 44 8B 44 24 68) "
					"-- patch not applied\n");
			}
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[NET-OBS] WriteBlock patch: r5apex module handle not found -- patch not applied\n");
		}
	}

	// WriteBlock dummy-skip (Path A, futureFieldsChangedBits). RecvProp via
	// decoder+0x4080 [r13]. Site: mov r8,[rbp+0x1D50] (7 bytes) -> jmp cave + 2 NOP.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pSite = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_DeltaBoundsA));
			Warning(eDLL_T::ENGINE,
				"[NET-OBS] WriteBlock(A) patch site %p bytes pre: %02X %02X %02X %02X %02X %02X %02X\n",
				pSite, pSite[0], pSite[1], pSite[2], pSite[3], pSite[4], pSite[5], pSite[6]);
			if (pSite[0] == 0x4C && pSite[1] == 0x8B && pSite[2] == 0x85
			    && pSite[3] == 0x50 && pSite[4] == 0x1D
			    && pSite[5] == 0x00 && pSite[6] == 0x00)
			{
				void* cave = NULL;
				uintptr_t siteAddr = (uintptr_t)pSite;
				for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
				     probe < siteAddr + 0x08000000;
				     probe += 0x10000)
				{
					cave = VirtualAlloc((void*)probe, 128, MEM_COMMIT | MEM_RESERVE,
						PAGE_EXECUTE_READWRITE);
					if (cave) break;
				}
				intptr_t dist = 0;
				if (cave)
				{
					dist = (intptr_t)((uintptr_t)cave - siteAddr);
					if (dist > 0x7FFFFF00LL || dist < -0x7FFFFF00LL)
					{
						Warning(eDLL_T::ENGINE,
							"[NET-OBS] WriteBlock(A) cave at %p too far from site %p (dist=%lld) -- aborting\n",
							cave, pSite, (long long)dist);
						VirtualFree(cave, 0, MEM_RELEASE);
						cave = NULL;
					}
				}
				if (cave)
				{
					uint8_t* c = reinterpret_cast<uint8_t*>(cave);
					memset(c, 0, 128);
					int pos = 0;

					// mov rax, [rbp+0x1D30]
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x85;
					c[pos++] = 0x30; c[pos++] = 0x1D; c[pos++] = 0x00; c[pos++] = 0x00;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz real_prop
					c[pos++] = 0x74; c[pos++] = 0x3E;          // +62 -> pos 74
					// mov rax, [rax+0x4080]
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x80;
					c[pos++] = 0x80; c[pos++] = 0x40; c[pos++] = 0x00; c[pos++] = 0x00;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz real_prop
					c[pos++] = 0x74; c[pos++] = 0x32;          // +50 -> pos 74
					// movsxd rcx, r13d
					c[pos++] = 0x49; c[pos++] = 0x63; c[pos++] = 0xCD;
					// mov rax, [rax+rcx*8]
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x04; c[pos++] = 0xC8;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz real_prop
					c[pos++] = 0x74; c[pos++] = 0x26;          // +38 -> pos 74
					// mov rax, [rax+0x28]
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x40; c[pos++] = 0x28;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz real_prop
					c[pos++] = 0x74; c[pos++] = 0x1D;          // +29 -> pos 74
					// cmp byte [rax], '?'
					c[pos++] = 0x80; c[pos++] = 0x38; c[pos++] = 0x3F;
					// jne real_prop
					c[pos++] = 0x75; c[pos++] = 0x18;          // +24 -> pos 74
					// cmp byte [rax+1], '_'
					c[pos++] = 0x80; c[pos++] = 0x78; c[pos++] = 0x01; c[pos++] = 0x5F;
					// jne real_prop
					c[pos++] = 0x75; c[pos++] = 0x12;          // +18 -> pos 74
					// cmp byte [rax+2], 'u'
					c[pos++] = 0x80; c[pos++] = 0x78; c[pos++] = 0x02; c[pos++] = 0x75;
					// jne real_prop
					c[pos++] = 0x75; c[pos++] = 0x0C;          // +12 -> pos 74
					// lock inc dword [rip+disp32]; counter at pos 88
					c[pos++] = 0xF0; c[pos++] = 0xFF; c[pos++] = 0x05;
					{
						int32_t cntDisp = 88 - (pos + 4); // pos=65, 88 - 69 = 19
						memcpy(c + pos, &cntDisp, 4);
						pos += 4;
					}
					// jmp loc_140232E14 (LABEL_82 -- skip WriteBlock + advance)
					c[pos++] = 0xE9;
					{
						uintptr_t target = NetObs_Sym(NetObsSym_t::PatchSite_DeltaBoundsAResume);
						int32_t rel = (int32_t)(target - ((uintptr_t)c + pos + 4));
						memcpy(c + pos, &rel, 4);
						pos += 4;
					}
					// mov r8, [rbp+0x1D50]
					c[pos++] = 0x4C; c[pos++] = 0x8B; c[pos++] = 0x85;
					c[pos++] = 0x50; c[pos++] = 0x1D; c[pos++] = 0x00; c[pos++] = 0x00;
					// jmp loc_140232C73 (next instruction after our 7-byte patch)
					c[pos++] = 0xE9;
					{
						uintptr_t target = NetObs_Sym(NetObsSym_t::PatchSite_DeltaBoundsANext);
						int32_t rel = (int32_t)(target - ((uintptr_t)c + pos + 4));
						memcpy(c + pos, &rel, 4);
						pos += 4;
					}
					// Counter at offset 88 (already zeroed by memset).
					s_pWriteBlockSkipCounterA = reinterpret_cast<volatile LONG*>(c + 88);

					// Patch original site: jmp cave (5 bytes) + 2 NOPs to fill 7
					DWORD oldProt;
					VirtualProtect(pSite, 7, PAGE_EXECUTE_READWRITE, &oldProt);
					pSite[0] = 0xE9;
					int32_t caveRel = (int32_t)((uintptr_t)cave - ((uintptr_t)pSite + 5));
					memcpy(pSite + 1, &caveRel, 4);
					pSite[5] = 0x90;  // nop
					pSite[6] = 0x90;  // nop
					VirtualProtect(pSite, 7, oldProt, &oldProt);

					Warning(eDLL_T::ENGINE,
						"[NET-OBS] WriteBlock(A) dummy-skip INSTALLED: cave=%p (dist=%+lld) "
						"counter=%p site_post=%02X %02X %02X %02X %02X %02X %02X\n",
						cave, (long long)dist, s_pWriteBlockSkipCounterA,
						pSite[0], pSite[1], pSite[2], pSite[3], pSite[4], pSite[5], pSite[6]);
				}
				else
				{
					Warning(eDLL_T::ENGINE,
						"[NET-OBS] WriteBlock(A) cave allocation FAILED near %p -- patch not applied\n",
						pSite);
				}
			}
			else
			{
				Warning(eDLL_T::ENGINE,
					"[NET-OBS] WriteBlock(A) patch site mismatch (expected 4C 8B 85 50 1D 00 00) "
					"-- patch not applied\n");
			}
		}
	}

	// WriteBlock dummy-skip (Path C, Net_UndoClientDelta). Same as Path A; JMP targets differ.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pSite = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_DeltaBoundsB));
			Warning(eDLL_T::ENGINE,
				"[NET-OBS] WriteBlock(C) patch site %p bytes pre: %02X %02X %02X %02X %02X %02X %02X\n",
				pSite, pSite[0], pSite[1], pSite[2], pSite[3], pSite[4], pSite[5], pSite[6]);
			if (pSite[0] == 0x4C && pSite[1] == 0x8B && pSite[2] == 0x85
			    && pSite[3] == 0x50 && pSite[4] == 0x1D
			    && pSite[5] == 0x00 && pSite[6] == 0x00)
			{
				void* cave = NULL;
				uintptr_t siteAddr = (uintptr_t)pSite;
				for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
				     probe < siteAddr + 0x08000000;
				     probe += 0x10000)
				{
					cave = VirtualAlloc((void*)probe, 128, MEM_COMMIT | MEM_RESERVE,
						PAGE_EXECUTE_READWRITE);
					if (cave) break;
				}
				intptr_t dist = 0;
				if (cave)
				{
					dist = (intptr_t)((uintptr_t)cave - siteAddr);
					if (dist > 0x7FFFFF00LL || dist < -0x7FFFFF00LL)
					{
						Warning(eDLL_T::ENGINE,
							"[NET-OBS] WriteBlock(C) cave too far (dist=%lld) -- aborting\n",
							(long long)dist);
						VirtualFree(cave, 0, MEM_RELEASE);
						cave = NULL;
					}
				}
				if (cave)
				{
					uint8_t* c = reinterpret_cast<uint8_t*>(cave);
					memset(c, 0, 128);
					int pos = 0;

					// mov rax, [rbp+0x1D30] -- decoder
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x85;
					c[pos++] = 0x30; c[pos++] = 0x1D; c[pos++] = 0x00; c[pos++] = 0x00;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz real_prop (+62 -> pos 74)
					c[pos++] = 0x74; c[pos++] = 0x3E;
					// mov rax, [rax+0x4080] -- recvProps base
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x80;
					c[pos++] = 0x80; c[pos++] = 0x40; c[pos++] = 0x00; c[pos++] = 0x00;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz real_prop (+50 -> pos 74)
					c[pos++] = 0x74; c[pos++] = 0x32;
					// movsxd rcx, r13d
					c[pos++] = 0x49; c[pos++] = 0x63; c[pos++] = 0xCD;
					// mov rax, [rax+rcx*8]
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x04; c[pos++] = 0xC8;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz real_prop (+38 -> pos 74)
					c[pos++] = 0x74; c[pos++] = 0x26;
					// mov rax, [rax+0x28] -- pVarName
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x40; c[pos++] = 0x28;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz real_prop (+29 -> pos 74)
					c[pos++] = 0x74; c[pos++] = 0x1D;
					// cmp byte [rax], '?'
					c[pos++] = 0x80; c[pos++] = 0x38; c[pos++] = 0x3F;
					// jne real_prop (+24 -> pos 74)
					c[pos++] = 0x75; c[pos++] = 0x18;
					// cmp byte [rax+1], '_'
					c[pos++] = 0x80; c[pos++] = 0x78; c[pos++] = 0x01; c[pos++] = 0x5F;
					// jne real_prop (+18 -> pos 74)
					c[pos++] = 0x75; c[pos++] = 0x12;
					// cmp byte [rax+2], 'u'
					c[pos++] = 0x80; c[pos++] = 0x78; c[pos++] = 0x02; c[pos++] = 0x75;
					// jne real_prop (+12 -> pos 74)
					c[pos++] = 0x75; c[pos++] = 0x0C;
					// lock inc dword [rip+disp32]; counter at pos 88
					c[pos++] = 0xF0; c[pos++] = 0xFF; c[pos++] = 0x05;
					{
						int32_t cntDisp = 88 - (pos + 4);  // 88 - 69 = 19
						memcpy(c + pos, &cntDisp, 4);
						pos += 4;
					}
					// jmp loc_14023351B (skip WriteBlock, advance iteration)
					c[pos++] = 0xE9;
					{
						uintptr_t target = NetObs_Sym(NetObsSym_t::PatchSite_DeltaBoundsBResume);
						int32_t rel = (int32_t)(target - ((uintptr_t)c + pos + 4));
						memcpy(c + pos, &rel, 4);
						pos += 4;
					}
					// mov r8, [rbp+0x1D50] -- original instruction
					c[pos++] = 0x4C; c[pos++] = 0x8B; c[pos++] = 0x85;
					c[pos++] = 0x50; c[pos++] = 0x1D; c[pos++] = 0x00; c[pos++] = 0x00;
					// jmp loc_140233378 (next instruction after our 7-byte patch)
					c[pos++] = 0xE9;
					{
						uintptr_t target = NetObs_Sym(NetObsSym_t::PatchSite_DeltaBoundsBNext);
						int32_t rel = (int32_t)(target - ((uintptr_t)c + pos + 4));
						memcpy(c + pos, &rel, 4);
						pos += 4;
					}
					// Counter at offset 88 (already zeroed by memset).
					s_pWriteBlockSkipCounterC = reinterpret_cast<volatile LONG*>(c + 88);

					// Patch original site: jmp cave (5 bytes) + 2 NOPs
					DWORD oldProt;
					VirtualProtect(pSite, 7, PAGE_EXECUTE_READWRITE, &oldProt);
					pSite[0] = 0xE9;
					int32_t caveRel = (int32_t)((uintptr_t)cave - ((uintptr_t)pSite + 5));
					memcpy(pSite + 1, &caveRel, 4);
					pSite[5] = 0x90;
					pSite[6] = 0x90;
					VirtualProtect(pSite, 7, oldProt, &oldProt);

					Warning(eDLL_T::ENGINE,
						"[NET-OBS] WriteBlock(C) dummy-skip INSTALLED: cave=%p (dist=%+lld) "
						"counter=%p site_post=%02X %02X %02X %02X %02X %02X %02X\n",
						cave, (long long)dist, s_pWriteBlockSkipCounterC,
						pSite[0], pSite[1], pSite[2], pSite[3], pSite[4], pSite[5], pSite[6]);
				}
				else
				{
					Warning(eDLL_T::ENGINE,
						"[NET-OBS] WriteBlock(C) cave allocation FAILED near %p\n", pSite);
				}
			}
			else
			{
				Warning(eDLL_T::ENGINE,
					"[NET-OBS] WriteBlock(C) patch site mismatch (expected 4C 8B 85 50 1D 00 00)\n");
			}
		}
	}

	// CheckSnapshotTransition gate 4: NOP the jnz so post-signon apply keeps running.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pPatch = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_ProcessFrameGuard));
			if (pPatch[0] == 0x0F && pPatch[1] == 0x85)
			{
				DWORD oldProt;
				VirtualProtect(pPatch, 6, PAGE_EXECUTE_READWRITE, &oldProt);
				memset(pPatch, 0x90, 6);
				VirtualProtect(pPatch, 6, oldProt, &oldProt);
				SDK_Log("[NET-OBS] patched CheckSnapshotTransition gate 4: snapshot apply loop now runs every frame\n");
			}
			else
			{
				SDK_Log("[NET-OBS] WARNING: gate 4 patch site mismatch "
					"(expected 0F 85, got %02X %02X)\n",
					pPatch[0], pPatch[1]);
			}
		}
	}

	// Entity init NULL skip (class). Cave: test rcx; mov rdx,[rcx+18h]; mov rbx,[rdx+4C0h];
	// test rbx; skip to loop increment if either is NULL.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pSiteA = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_ApplyNullClassA));
			// Verify original bytes: 48 8B 51 18 48 8B 9A C0 04 00 00
			if (pSiteA[0] == 0x48 && pSiteA[1] == 0x8B && pSiteA[2] == 0x51 && pSiteA[3] == 0x18
				&& pSiteA[4] == 0x48 && pSiteA[5] == 0x8B && pSiteA[6] == 0x9A
				&& pSiteA[7] == 0xC0 && pSiteA[8] == 0x04)
			{
				// Allocate cave within rel32 range
				void* caveA = NULL;
				uintptr_t siteAddrA = (uintptr_t)pSiteA;
				for (uintptr_t probe = (siteAddrA - 0x08000000) & ~0xFFFFULL;
					 probe < siteAddrA + 0x08000000;
					 probe += 0x10000)
				{
					caveA = VirtualAlloc((void*)probe, 64, MEM_COMMIT | MEM_RESERVE,
						PAGE_EXECUTE_READWRITE);
					if (caveA) break;
				}
				if (caveA)
				{
					uint8_t* c = reinterpret_cast<uint8_t*>(caveA);
					int pos = 0;
					uintptr_t loopIncr = NetObs_Sym(NetObsSym_t::PatchSite_ApplyLoopIncr);
					uintptr_t afterPatch = NetObs_Sym(NetObsSym_t::PatchSite_ApplyNullClassAResume);

					// test rcx, rcx (class entry NULL check)
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC9;
					// jz skip_class
					c[pos++] = 0x0F; c[pos++] = 0x84;
					int jzClassOff = pos; // placeholder for 4-byte offset
					pos += 4;

					// mov rdx, [rcx+18h] (original instruction 1)
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x51; c[pos++] = 0x18;
					// mov rbx, [rdx+4C0h] (original instruction 2)
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x9A;
					c[pos++] = 0xC0; c[pos++] = 0x04; c[pos++] = 0x00; c[pos++] = 0x00;
					// test rbx, rbx (decoder NULL check)
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xDB;
					// jz skip_class
					c[pos++] = 0x0F; c[pos++] = 0x84;
					int jzDecoderOff = pos; // placeholder
					pos += 4;

					// jmp afterPatch (= "test rbx,rbx" -- redundant but correct)
					c[pos++] = 0xE9;
					int32_t backRel = (int32_t)(afterPatch - ((uintptr_t)c + pos + 4));
					memcpy(c + pos, &backRel, 4); pos += 4;

					// skip_class: jmp loop increment
					int skipPos = pos;
					// Fix both jz offsets
					int32_t jzRel1 = (int32_t)((uintptr_t)c + skipPos - ((uintptr_t)c + jzClassOff + 4));
					memcpy(c + jzClassOff, &jzRel1, 4);
					int32_t jzRel2 = (int32_t)((uintptr_t)c + skipPos - ((uintptr_t)c + jzDecoderOff + 4));
					memcpy(c + jzDecoderOff, &jzRel2, 4);

					c[pos++] = 0xE9;
					int32_t skipRel = (int32_t)(loopIncr - ((uintptr_t)c + pos + 4));
					memcpy(c + pos, &skipRel, 4); pos += 4;

					// Patch original site: jmp cave + 6 NOPs
					DWORD oldProt;
					VirtualProtect(pSiteA, 11, PAGE_EXECUTE_READWRITE, &oldProt);
					pSiteA[0] = 0xE9;
					int32_t caveRel = (int32_t)((uintptr_t)caveA - ((uintptr_t)pSiteA + 5));
					memcpy(pSiteA + 1, &caveRel, 4);
					memset(pSiteA + 5, 0x90, 6); // NOP x6
					VirtualProtect(pSiteA, 11, oldProt, &oldProt);

					SDK_Log("[NET-OBS] patched entity init: class+decoder NULL skip via cave at %p "
						"(dist=%lld)\n", caveA, (long long)((intptr_t)((uintptr_t)caveA - siteAddrA)));
				}
				else
				{
					SDK_Log("[NET-OBS] WARNING: could not allocate cave A near %p\n", pSiteA);
				}
			}
			else
			{
				SDK_Log("[NET-OBS] WARNING: entity init class-NULL patch site mismatch\n");
			}

			// Also NOP the error dialog CALL at for robustness.
			// With Patch A above, NULL classes/decoders never reach here. But if
			// something unexpected happens, this prevents the blocking error dialog.
			uint8_t* pCallSite = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_ApplyErrorCall));
			if (pCallSite[0] == 0xE8)
			{
				DWORD oldProt;
				VirtualProtect(pCallSite, 5, PAGE_EXECUTE_READWRITE, &oldProt);
				pCallSite[0] = 0xE9; // jmp near
				const uintptr_t target = NetObs_Sym(NetObsSym_t::PatchSite_ApplyLoopIncr);
				const int32_t rel = (int32_t)(target - ((uintptr_t)pCallSite + 5));
				memcpy(pCallSite + 1, &rel, 4);
				VirtualProtect(pCallSite, 5, oldProt, &oldProt);
				SDK_Log("[NET-OBS] patched entity init: decoder error dialog -> skip class (backup)\n");
			}
		}
	}

	// Prop-level NULL: if RecvProp* is NULL set r10d=0x7F (switch default, 0 bytes).
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pSite = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_ApplyNullDecoder));
			SDK_Log("[NET-OBS] entity init prop site bytes: %02X %02X %02X %02X %02X %02X %02X\n",
				pSite[0], pSite[1], pSite[2], pSite[3], pSite[4], pSite[5], pSite[6]);
			if ((pSite[0] == 0x4A || pSite[0] == 0x49) && pSite[1] == 0x8B
				&& pSite[2] == 0x0C && pSite[3] == 0xC8
				&& pSite[4] == 0x4C && pSite[5] == 0x63 && pSite[6] == 0x11)
			{
				// Allocate code cave WITHIN +-2GB of the patch site.
				// VirtualAlloc(NULL,...) can return addresses >2GB away,
				// which truncates the jmp rel32 offset.
				void* cave = NULL;
				uintptr_t siteAddr = (uintptr_t)pSite;
				// Scan downward from 128MB below site in 64KB steps (allocation granularity)
				for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
					 probe < siteAddr + 0x08000000;
					 probe += 0x10000)
				{
					cave = VirtualAlloc((void*)probe, 96, MEM_COMMIT | MEM_RESERVE,
						PAGE_EXECUTE_READWRITE);
					if (cave) break;
				}
				if (!cave)
				{
					SDK_Log("[NET-OBS] WARNING: could not allocate code cave near %p\n", pSite);
				}
				else
				{
					// Verify the cave is actually within rel32 range
					intptr_t dist = (intptr_t)((uintptr_t)cave - siteAddr);
					if (dist > 0x7FFFFF00LL || dist < -0x7FFFFF00LL)
					{
						SDK_Log("[NET-OBS] WARNING: cave at %p too far from site %p (dist=%lld)\n",
							cave, pSite, (long long)dist);
						VirtualFree(cave, 0, MEM_RELEASE);
						cave = NULL;
					}
				}
				if (cave)
				{
					uint8_t* c = reinterpret_cast<uint8_t*>(cave);
					memset(c, 0, 96);
					int pos = 0;

					// mov rcx, [rax+r9*8] (original, REX.WX = 0x4A)
					c[pos++] = 0x4A; c[pos++] = 0x8B; c[pos++] = 0x0C; c[pos++] = 0xC8;
					// test rcx, rcx
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC9;
					// jz null_prop
					c[pos++] = 0x74;
					int jzOffset = pos;
					c[pos++] = 0x00; // placeholder
					// mov rdx, rcx
					c[pos++] = 0x48; c[pos++] = 0x89; c[pos++] = 0xCA;
					// shr rdx, 47 (valid user pointers have high bits clear)
					c[pos++] = 0x48; c[pos++] = 0xC1; c[pos++] = 0xEA; c[pos++] = 0x2F;
					// jne null_prop
					c[pos++] = 0x75;
					int jneHighOff = pos;
					c[pos++] = 0x00; // placeholder
					// test cl, 7 (RecvProp pointers are at least 8-byte aligned)
					c[pos++] = 0xF6; c[pos++] = 0xC1; c[pos++] = 0x07;
					// jne null_prop
					c[pos++] = 0x75;
					int jneAlignOff = pos;
					c[pos++] = 0x00; // placeholder
					// movabs rdx, 0x0000010000000000 (reject low aligned garbage)
					c[pos++] = 0x48; c[pos++] = 0xBA;
					uintptr_t minUserPtr = 0x0000010000000000ULL;
					memcpy(c + pos, &minUserPtr, 8);
					pos += 8;
					// cmp rcx, rdx
					c[pos++] = 0x48; c[pos++] = 0x39; c[pos++] = 0xD1;
					// jb null_prop
					c[pos++] = 0x72;
					int jbLowOff = pos;
					c[pos++] = 0x00; // placeholder

					// -- valid prop path --
					// movsxd r10, dword ptr [rcx] (original instruction)
					c[pos++] = 0x4C; c[pos++] = 0x63; c[pos++] = 0x11;
					// jmp back to (after the 7 patched bytes)
					c[pos++] = 0xE9;
					uintptr_t backTarget = NetObs_Sym(NetObsSym_t::PatchSite_ApplyNullDecoderBack);
					int32_t backRel = (int32_t)(backTarget - ((uintptr_t)c + pos + 4));
					memcpy(c + pos, &backRel, 4); pos += 4;

					// -- null prop path --
					c[jzOffset] = (uint8_t)(pos - jzOffset - 1); // fix jz offset
					c[jneHighOff] = (uint8_t)(pos - jneHighOff - 1);
					c[jneAlignOff] = (uint8_t)(pos - jneAlignOff - 1);
					c[jbLowOff] = (uint8_t)(pos - jbLowOff - 1);
					// movabs rcx, &s_globalDummy (keep downstream rcx uses valid)
					c[pos++] = 0x48; c[pos++] = 0xB9;
					uintptr_t dummyAddr = (uintptr_t)&s_globalDummy;
					memcpy(c + pos, &dummyAddr, 8);
					pos += 8;
					// mov r10d, 0x7F (value > 10 -> switch default -> size=0)
					c[pos++] = 0x41; c[pos++] = 0xBA; c[pos++] = 0x7F;
					c[pos++] = 0x00; c[pos++] = 0x00; c[pos++] = 0x00;
					// jmp back to (same target -- enter switch with type=0x7F)
					c[pos++] = 0xE9;
					int32_t nullRel = (int32_t)(backTarget - ((uintptr_t)c + pos + 4));
					memcpy(c + pos, &nullRel, 4); pos += 4;

					// Patch original site: jmp cave + 2 NOPs
					DWORD oldProt;
					VirtualProtect(pSite, 7, PAGE_EXECUTE_READWRITE, &oldProt);
					pSite[0] = 0xE9;
					int32_t caveRel = (int32_t)((uintptr_t)cave - ((uintptr_t)pSite + 5));
					memcpy(pSite + 1, &caveRel, 4);
					pSite[5] = 0x90; // nop
					pSite[6] = 0x90; // nop
					VirtualProtect(pSite, 7, oldProt, &oldProt);

					SDK_Log("[NET-OBS] patched entity init: NULL/bad prop -> type=0x7F (default, size=0) via cave at %p "
						"(dist=%lld from site)\n", cave, (long long)((intptr_t)((uintptr_t)cave - siteAddr)));
				}
			}
			else
			{
				SDK_Log("[NET-OBS] WARNING: entity init prop-NULL patch site mismatch\n");
			}
		}
	}

	// Array companion pointer guard. Bad ptr: skip multiplier, keep accumulated size.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pSite = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_ApplyBounds));
			SDK_Log("[NET-OBS] entity init array-prop site bytes: "
				"%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
				pSite[0], pSite[1], pSite[2], pSite[3],
				pSite[4], pSite[5], pSite[6], pSite[7],
				pSite[8], pSite[9], pSite[10], pSite[11]);

			if (pSite[0] == 0x48 && pSite[1] == 0x8B && pSite[2] == 0x43 && pSite[3] == 0x18
				&& (pSite[4] == 0x4A || pSite[4] == 0x49) && pSite[5] == 0x8B
				&& pSite[6] == 0x04 && pSite[7] == 0xC8
				&& pSite[8] == 0x44 && pSite[9] == 0x8B && pSite[10] == 0x40 && pSite[11] == 0x28)
			{
				void* cave = NULL;
				uintptr_t siteAddr = (uintptr_t)pSite;
				for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
					 probe < siteAddr + 0x08000000;
					 probe += 0x10000)
				{
					cave = VirtualAlloc((void*)probe, 128, MEM_COMMIT | MEM_RESERVE,
						PAGE_EXECUTE_READWRITE);
					if (cave) break;
				}

				if (cave)
				{
					uint8_t* c = reinterpret_cast<uint8_t*>(cave);
					memset(c, 0, 128);
					int pos = 0;

					const uintptr_t afterPatch = NetObs_Sym(NetObsSym_t::PatchSite_ApplyBoundsResume);
					const uintptr_t loopIncr = NetObs_Sym(NetObsSym_t::PatchSite_ApplyLoopIncr);
					const uintptr_t minUserPtr = 0x0000010000000000ULL;

					// mov rax, [rbx+18h]
					c[pos++] = 0x48; c[pos++] = 0x8B; c[pos++] = 0x43; c[pos++] = 0x18;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz skip_array
					c[pos++] = 0x0F; c[pos++] = 0x84;
					int jzBaseOff = pos; pos += 4;
					// mov rax, [rax+r9*8]
					c[pos++] = 0x4A; c[pos++] = 0x8B; c[pos++] = 0x04; c[pos++] = 0xC8;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jz skip_array
					c[pos++] = 0x0F; c[pos++] = 0x84;
					int jzPropOff = pos; pos += 4;
					// mov rdx, rax
					c[pos++] = 0x48; c[pos++] = 0x89; c[pos++] = 0xC2;
					// shr rdx, 47
					c[pos++] = 0x48; c[pos++] = 0xC1; c[pos++] = 0xEA; c[pos++] = 0x2F;
					// jne skip_array
					c[pos++] = 0x0F; c[pos++] = 0x85;
					int jneHighOff = pos; pos += 4;
					// test al, 7
					c[pos++] = 0xA8; c[pos++] = 0x07;
					// jne skip_array
					c[pos++] = 0x0F; c[pos++] = 0x85;
					int jneAlignOff = pos; pos += 4;
					// movabs rdx, minUserPtr
					c[pos++] = 0x48; c[pos++] = 0xBA;
					memcpy(c + pos, &minUserPtr, 8); pos += 8;
					// cmp rax, rdx
					c[pos++] = 0x48; c[pos++] = 0x39; c[pos++] = 0xD0;
					// jb skip_array
					c[pos++] = 0x0F; c[pos++] = 0x82;
					int jbLowOff = pos; pos += 4;
					// mov r8d, [rax+28h]
					c[pos++] = 0x44; c[pos++] = 0x8B; c[pos++] = 0x40; c[pos++] = 0x28;
					// jmp afterPatch
					c[pos++] = 0xE9;
					int32_t afterRel = (int32_t)(afterPatch - ((uintptr_t)c + pos + 4));
					memcpy(c + pos, &afterRel, 4); pos += 4;

					const int skipPos = pos;
					int32_t jzBaseRel = (int32_t)((uintptr_t)c + skipPos - ((uintptr_t)c + jzBaseOff + 4));
					int32_t jzPropRel = (int32_t)((uintptr_t)c + skipPos - ((uintptr_t)c + jzPropOff + 4));
					int32_t jneHighRel = (int32_t)((uintptr_t)c + skipPos - ((uintptr_t)c + jneHighOff + 4));
					int32_t jneAlignRel = (int32_t)((uintptr_t)c + skipPos - ((uintptr_t)c + jneAlignOff + 4));
					int32_t jbLowRel = (int32_t)((uintptr_t)c + skipPos - ((uintptr_t)c + jbLowOff + 4));
					memcpy(c + jzBaseOff, &jzBaseRel, 4);
					memcpy(c + jzPropOff, &jzPropRel, 4);
					memcpy(c + jneHighOff, &jneHighRel, 4);
					memcpy(c + jneAlignOff, &jneAlignRel, 4);
					memcpy(c + jbLowOff, &jbLowRel, 4);

					// skip_array: jmp loop increment
					c[pos++] = 0xE9;
					int32_t skipRel = (int32_t)(loopIncr - ((uintptr_t)c + pos + 4));
					memcpy(c + pos, &skipRel, 4); pos += 4;

					DWORD oldProt;
					VirtualProtect(pSite, 12, PAGE_EXECUTE_READWRITE, &oldProt);
					pSite[0] = 0xE9;
					int32_t caveRel = (int32_t)((uintptr_t)cave - ((uintptr_t)pSite + 5));
					memcpy(pSite + 1, &caveRel, 4);
					memset(pSite + 5, 0x90, 7);
					VirtualProtect(pSite, 12, oldProt, &oldProt);

					SDK_Log("[NET-OBS] patched entity init: NULL/bad array-prop companion -> skip array size via cave at %p "
						"(dist=%lld from site)\n", cave, (long long)((intptr_t)((uintptr_t)cave - siteAddr)));
				}
				else
				{
					SDK_Log("[NET-OBS] WARNING: could not allocate array-prop cave near %p\n", pSite);
				}
			}
			else
			{
				SDK_Log("[NET-OBS] WARNING: entity init array-prop patch site mismatch\n");
			}
		}
	}

	// Prop-to-offset map NULL: after load, if rax==0 jump to epilog.
	{
		uintptr_t base = NetObs_GetExeModuleBase();
		if (base)
		{
			uint8_t* pSite = reinterpret_cast<uint8_t*>(
				NetObs_Sym(NetObsSym_t::PatchSite_ApplyTail));
			if (pSite[0] == 0x49 && pSite[1] == 0x8B && pSite[2] == 0x84
				&& pSite[3] == 0xC7)
			{
				void* cave = NULL;
				uintptr_t siteAddr = (uintptr_t)pSite;
				for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
					 probe < siteAddr + 0x08000000;
					 probe += 0x10000)
				{
					cave = VirtualAlloc((void*)probe, 64, MEM_COMMIT | MEM_RESERVE,
						PAGE_EXECUTE_READWRITE);
					if (cave) break;
				}
				if (cave)
				{
					uint8_t* c = reinterpret_cast<uint8_t*>(cave);
					int pos = 0;
					uintptr_t nextInstr = NetObs_Sym(NetObsSym_t::PatchSite_ApplyTailNext);
					// DX12 epilog start = movaps xmm9 (full callee-saved restore of
					// xmm6-9/r14/r12); was +0x29 too far and skipped them,
					// corrupting the caller's non-volatile regs on the NULL-guard path.
					uintptr_t funcEpilog = NetObs_Sym(NetObsSym_t::PatchSite_ApplyEpilog);

					// Original mapping load (same opcode, version-specific displacement).
					memcpy(c + pos, pSite, 8); pos += 8;
					// test rax, rax
					c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xC0;
					// jnz.continue (skip 5 bytes)
					c[pos++] = 0x75; c[pos++] = 0x05;
					// jmp funcEpilog (NULL mapping -> clean return)
					c[pos++] = 0xE9;
					int32_t epilogRel = (int32_t)(funcEpilog - ((uintptr_t)c + pos + 4));
					memcpy(c + pos, &epilogRel, 4); pos += 4;
					//.continue: jmp nextInstr
					c[pos++] = 0xE9;
					int32_t backRel = (int32_t)(nextInstr - ((uintptr_t)c + pos + 4));
					memcpy(c + pos, &backRel, 4); pos += 4;

					// Patch original site
					DWORD oldProt;
					VirtualProtect(pSite, 8, PAGE_EXECUTE_READWRITE, &oldProt);
					pSite[0] = 0xE9;
					int32_t caveRel = (int32_t)((uintptr_t)cave - ((uintptr_t)pSite + 5));
					memcpy(pSite + 1, &caveRel, 4);
					pSite[5] = 0x90; pSite[6] = 0x90; pSite[7] = 0x90;
					VirtualProtect(pSite, 8, oldProt, &oldProt);

					SDK_Log("[NET-OBS] patched RecvTable_Decode packed init: "
						"NULL mapping guard via cave at %p (dist=%lld)\n",
						cave, (long long)((intptr_t)((uintptr_t)cave - siteAddr)));
				}
				else
				{
					SDK_Log("[NET-OBS] WARNING: could not allocate cave for mapping NULL guard\n");
				}
			}
			else
			{
				SDK_Log("[NET-OBS] WARNING: RecvTable_Decode mapping patch site mismatch "
					"(%02X %02X %02X %02X)\n", pSite[0], pSite[1], pSite[2], pSite[3]);
			}
		}
	}

	// SVC_Snapshot vtable +0x20 ReadFromBuffer: point at the real snapshot reader.
	{
		const uintptr_t vtableReadFromBuf =
			NetObs_Sym(NetObsSym_t::SvcSnapshotVtableReadFromBuffer);
		const uintptr_t wrongReader =
			NetObs_Sym(NetObsSym_t::SvcSnapshotWrongReader);
		const uintptr_t correctReader =
			NetObs_Sym(NetObsSym_t::SvcSnapshotReadFromBuffer);

		uint64_t* pSlot = reinterpret_cast<uint64_t*>(vtableReadFromBuf);
		if (*pSlot == wrongReader)
		{
			DWORD oldProt;
			VirtualProtect(pSlot, 8, PAGE_READWRITE, &oldProt);
			*pSlot = correctReader;
			VirtualProtect(pSlot, 8, oldProt, &oldProt);
			SDK_Log("[NET-OBS] patched SVC_Snapshot vtable: ReadFromBuffer "
				"0x%llX -> 0x%llX (real snapshot reader)\n",
				(unsigned long long)wrongReader, (unsigned long long)correctReader);
		}
		else
		{
			SDK_Log("[NET-OBS] SVC_Snapshot vtable slot already correct or unexpected value: 0x%llX\n",
				(unsigned long long)*pSlot);
		}
	}

	// [MIGRATED -> VNetDecodeDiagS21] CreateDecoders

	// RegisterNetworkedVariable capture. Migrated to VNetVarDiagS21.

	// [MIGRATED -> VNetDecodeDiagS21] BuildPropOffsetTable

	// [MIGRATED -> VNetDecodeDiagS21] CL_CopyNewEntity

	// GetNetworkedClassName detour removed; VM uses ScriptGetSignifierName.

	// [MIGRATED -> VNetDecodeDiagS21] RecvTableDecode

	// [MIGRATED -> VNetDecodeDiagS21] PropDecodeDispatch+ReadPropIdx

	// [MIGRATED -> VNetDecodeDiagS21] RecvTableDecodeMain

	// [MIGRATED -> VNetFrameDiagS21] ClientFrameUpdate

	// [MIGRATED -> VNetDecodeDiagS21] ModelCamoLookupSentinel patch

	// [MIGRATED -> VNetDecodeDiagS21] ModelDrawInfoBuild

	// [MIGRATED -> VNetObserverDiagS21] cbuf / cmd / connect / net-send / signon / sub2429d0 / entity-copy / exit / winsock observer hooks

	SDK_Log("[NET-OBS] install complete: %d hook(s) attached\n", nHooked);
	return nHooked;
}

//-----------------------------------------------------------------------------
// Client convars to match dedi: sockets-for-loopback, no random key.
// Encryption force-off gated by bridge_force_encryption_off.
//-----------------------------------------------------------------------------
void NetObserver_PushClientConvars()
{
	if (!Cbuf_AddText || !Cbuf_Execute)
	{
		SDK_Log("[NET-OBS] Cbuf_AddText/Execute not resolved, cannot push convars\n");
		return;
	}

	// Trailing \n required. Do not set developer 1 here (UI VM references missing symbols).
	char script[512];
	if (bridge_force_encryption_off.GetBool())
	{
		static bool s_mitmLogged = false;
		if (!s_mitmLogged)
		{
			s_mitmLogged = true;
			Warning(eDLL_T::ENGINE, "[SEC] bridge_force_encryption_off=1 -- net crypto forced off "
				"for S3 interop (plaintext on wire; MITM risk on shared nets). "
				"Set bridge_force_encryption_off 0 to leave engine defaults.\n");
		}
		snprintf(script, sizeof(script),
			"net_usesocketsforloopback 1;"
			"net_useRandomKey 0;"
			"net_encryptionEnable 0;"
			"net_encrypt_dtls 0;"
			"net_encrypt_dtls_hkdf 0;"
			"net_encrypt_multiKey 0;"
			"sv_showconnecting 1\n");
	}
	else
	{
		snprintf(script, sizeof(script),
			"net_usesocketsforloopback 1;"
			"net_useRandomKey 0;"
			"sv_showconnecting 1\n");
		SDK_Log("[SEC] bridge_force_encryption_off=0 -- leaving net_encryptionEnable/DTLS at engine default\n");
	}

	SDK_Log("[NET-OBS] pushing client convars: %s", script);

	// After NetObserver_Install, this call goes through Hook_Cbuf_AddText.
	// That's fine -- the hook logs once and then calls the real function.
	Cbuf_AddText(ECommandTarget_t::CBUF_FIRST_PLAYER, script, cmd_source_t::kCommandSrcCode);
	Cbuf_Execute();
}
