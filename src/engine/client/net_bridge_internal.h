#ifndef CORE_NET_BRIDGE_INTERNAL_H
#define CORE_NET_BRIDGE_INTERNAL_H

//=============================================================================//
//
// Purpose: Shared internal surface for the net_bridge translation units.
// Include AFTER core/stdafx.h. Engine symbol accessors resolve through net_bridge_addrs.
//
//=============================================================================//
#include <cstdint>
#include <cstddef>
#include "public/const.h"
#include "engine/client/net_observer.h"
#include "engine/client/net_bridge_addrs.h"
#include "tier0/memvalidate.h"

// Module base and dx11/dx12 selection are process-lifetime. Resolve once:
// ~60 accessors hit this on the per-entity apply path.
inline uintptr_t NetObs_GetExeModuleBase()
{
	static uintptr_t s_base = 0;
	if (s_base)
		return s_base;
	// Live client is r5apex.exe (optional DX12 binary name as fallback).
	HMODULE hExe = GetModuleHandleA("r5apex.exe");
	if (!hExe) hExe = GetModuleHandleA("r5apex_dx12.exe");
	if (!hExe) hExe = GetModuleHandleA(NULL);
	s_base = reinterpret_cast<uintptr_t>(hExe);
	return s_base;
}

inline bool NetObs_IsDx12Exe()
{
	static int s_isDx12 = -1;   // -1 = unresolved, 0/1 = cached result
	if (s_isDx12 >= 0)
		return s_isDx12 != 0;
	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(NULL, exePath, SDK_ARRAYSIZE(exePath));
	s_isDx12 = (V_stristr(exePath, "dx12") != nullptr) ? 1 : 0;
	return s_isDx12 != 0;
}

inline uintptr_t NetObs_RecvTableListBaseAddr()
{
	// Recv-table registry (second list in DataTable_SetupReceiveTableFromSendTable).
	// Do not use the first SendTable list here.
	return NetObs_Sym(NetObsSym_t::RecvTableListBase);
}

inline uintptr_t NetObs_RecvTableListCapacityAddr()
{
	return NetObs_Sym(NetObsSym_t::RecvTableListCapacity);
}

inline uintptr_t NetObs_RecvTableListHeadAddr()
{
	return NetObs_Sym(NetObsSym_t::RecvTableListHead);
}

inline uintptr_t NetObs_ClientClassArrayAddr()
{
	// BuildPropOffsetTable walks this CClientClass registry.
	return NetObs_Sym(NetObsSym_t::ClientClassArray);
}

inline uintptr_t NetObs_ClientClassCountAddr()
{
	return NetObs_Sym(NetObsSym_t::ClientClassCount);
}

inline uintptr_t NetObs_FindClassNameAddr()
{
	return NetObs_Sym(NetObsSym_t::FindClassName);
}

inline uintptr_t NetObs_FindOrCreateClassNameAddr()
{
	return NetObs_Sym(NetObsSym_t::FindOrCreateClassName);
}

inline uintptr_t NetObs_EntityHandleTableAddr()
{
	return NetObs_Sym(NetObsSym_t::EntityHandleTable);
}

inline uintptr_t NetObs_PropMappingTableAddr()
{
	return NetObs_Sym(NetObsSym_t::PropMappingTable);
}

inline uintptr_t NetObs_ClientStateSignonAddr()
{
	return NetObs_Sym(NetObsSym_t::ClientStateSignon);
}

inline uintptr_t NetObs_ClientStateNextSendAddr()
{
	return NetObs_Sym(NetObsSym_t::ClientStateNextSend);
}

inline uintptr_t NetObs_ClientStateSingletonSlotAddr()
{
	return NetObs_Sym(NetObsSym_t::ClientStateBase);
}

inline uintptr_t NetObs_NetChanAddr()
{
	return NetObs_Sym(NetObsSym_t::NetChan);
}

inline uintptr_t NetObs_NetTimeAddr()
{
	return NetObs_Sym(NetObsSym_t::NetTime);
}

inline uintptr_t NetObs_CvarGatePtrAddr()
{
	return NetObs_Sym(NetObsSym_t::CvarGatePtr);
}

inline uintptr_t NetObs_InGameFlagAddr()
{
	return NetObs_Sym(NetObsSym_t::InGameFlag);
}

// cl.m_frameTime (cl+0x20DF8) -- CClientState::GetFrameTime's backing
// field, NOT host_frametime. _Host_RunFrame stores
// AdjustFrameTime(host_frametime) here, so a small value means the
// clock-drift correction is subtracting, not that host dt is small.
inline uintptr_t NetObs_FrameTimeAddr()
{
	return NetObs_Sym(NetObsSym_t::FrameTime);
}

// host_state.interval_per_tick. Drives the ideal client clock in SetServerTick.
inline uintptr_t NetObs_IntervalPerTickAddr()
{
	return NetObs_Sym(NetObsSym_t::IntervalPerTick);
}

// The real host_frametime: Host_AccumulateTime stores max(min(dt,0.1),0.001)
// here, and _Host_RunFrame feeds it to CClockDriftMgr::AdjustFrameTime. Pinned
// at 0.001 means the engine received no frame dt; any larger value means the
// drift manager, not the host clock, is scaling the world down.
inline uintptr_t NetObs_HostFrameTimeAddr()
{
	return NetObs_Sym(NetObsSym_t::HostFrameTime);
}

// CClientState base. cl.m_frameTime is +0x20DF8 and cl.m_clientTime is +0xC0,
// both read straight off the inlined AdjustFrameTime in _Host_RunFrame.
inline uintptr_t NetObs_ClientStateBaseAddr()
{
	return NetObs_Sym(NetObsSym_t::ClientStateBase);
}

// cl.m_ClockDriftMgr. m_serverFrameTimeScaleAverage is the multiplier applied to
// host_frametime every frame; m_aheadBy/m_correctWithin are the ahead-drain pair
// that can take the product to zero. Both paths end in cl.m_frameTime.
struct ClockDriftView_t
{
	float m_ClockOffsets[4];              // +0x00
	int   m_iCurClockOffset;              // +0x10
	float m_serverFrameTimeScales[24];    // +0x14
	int   m_serverFrameTimeScaleIndex;    // +0x74
	float m_serverFrameTimeScaleAverage;  // +0x78
	float m_aheadBy;                      // +0x7C
	float m_correctWithin;                // +0x80
	unsigned int m_lastPlatTime;          // +0x84
	float m_lastServerTime;               // +0x88
	int   m_nServerTick;                  // +0x8C
	int   m_nClientTick;                  // +0x90
};

inline const ClockDriftView_t* NetObs_ClockDrift()
{
	const uintptr_t cl = NetObs_ClientStateBaseAddr();
	return cl ? reinterpret_cast<const ClockDriftView_t*>(cl + 0xC4) : nullptr;
}

inline uintptr_t NetObs_CmdNumberAddr()
{
	// ClSendTick writes the last CreateMove command number here (cl.m_frameTime + 4).
	return NetObs_Sym(NetObsSym_t::CmdNumber);
}

inline uintptr_t NetObs_SnapshotHeadFramePtrAddr()
{
	return NetObs_Sym(NetObsSym_t::SnapshotHeadFramePtr);
}

inline uintptr_t NetObs_SnapshotBaseLerpFramePtrAddr()
{
	return NetObs_Sym(NetObsSym_t::SnapshotBaseLerpFramePtr);
}

inline uintptr_t NetObs_SendAccumAddr()
{
	return NetObs_Sym(NetObsSym_t::SendAccum);
}

inline uintptr_t NetObs_SendLastClockAddr()
{
	return NetObs_Sym(NetObsSym_t::SendLastClock);
}

inline uintptr_t NetObs_FixedStepFlagAddr()
{
	return NetObs_Sym(NetObsSym_t::FixedStepFlag);
}

inline uintptr_t NetObs_FixedClockAddr()
{
	return NetObs_Sym(NetObsSym_t::FixedClock);
}

inline uintptr_t NetObs_SendIntervalPrimaryPtrAddr()
{
	return NetObs_Sym(NetObsSym_t::SendIntervalPrimaryPtr);
}

inline uintptr_t NetObs_SendIntervalSecondaryPtrAddr()
{
	return NetObs_Sym(NetObsSym_t::SendIntervalSecondaryPtr);
}

inline uintptr_t NetObs_SnapshotManagerSlotAddr()
{
	return NetObs_Sym(NetObsSym_t::SnapshotManagerSlot);
}

inline uintptr_t NetObs_StringTableContainerAddr()
{
	return NetObs_Sym(NetObsSym_t::StringTableContainer);
}

inline uintptr_t NetObs_EntityManagerAddr()
{
	return NetObs_Sym(NetObsSym_t::EntityManager);
}

inline uintptr_t NetObs_ClientDllSlotAddr()
{
	return NetObs_Sym(NetObsSym_t::ClientDllSlot);
}

// Object whose vtable carries GetGameTimescale (slot +0x820), the divisor in
// CL_CalcMoveFrametime. NOT the ClientDllSlot object above -- that one is CInput,
// which reaches CreateMove at +0xD8.
inline uintptr_t NetObs_GameTimescaleObjAddr()
{
	return NetObs_Sym(NetObsSym_t::GameTimescaleObj);
}

inline uintptr_t NetObs_PredictionSingletonPtrAddr()
{
	// CPrediction singleton pointer slot written once by ClientDLL_Init; one dereference yields the live object.
	return NetObs_Sym(NetObsSym_t::PredictionSingletonPtr);
}

inline uintptr_t NetObs_MaxEntsAddr()
{
	return NetObs_Sym(NetObsSym_t::MaxEnts);
}

inline uintptr_t NetObs_UiEntityTableAddr()
{
	return NetObs_Sym(NetObsSym_t::UiEntityTable);
}

inline uintptr_t NetObs_NonRewindClientClassHeadAddr()
{
	return NetObs_Sym(NetObsSym_t::NonRewindClientClassHead);
}

inline uintptr_t NetObs_NonRewindRecvTableAddr()
{
	return NetObs_Sym(NetObsSym_t::NonRewindRecvTable);
}

inline uintptr_t NetObs_NonRewindNameAddr()
{
	return NetObs_Sym(NetObsSym_t::NonRewindName);
}

inline uintptr_t NetObs_NonRewindEntityPtrAddr()
{
	return NetObs_Sym(NetObsSym_t::NonRewindEntityPtr);
}

inline uintptr_t NetObs_SetScriptNameAddr()
{
	return NetObs_Sym(NetObsSym_t::SetScriptName);
}

inline uintptr_t NetObs_NetSignonStateReadFromBufferAddr()
{
	return NetObs_Sym(NetObsSym_t::NetSignonStateReadFromBuffer);
}

inline uintptr_t NetObs_NetSignonStateProcessAddr()
{
	return NetObs_Sym(NetObsSym_t::NetSignonStateProcess);
}

inline uintptr_t NetObs_PropDecodeFnTableAddr()
{
	return NetObs_Sym(NetObsSym_t::PropDecodeFnTable);
}

inline uintptr_t NetObs_Case3InitHelperStructPtrAddr()
{
	return NetObs_Sym(NetObsSym_t::Case3InitHelperStructPtr);
}

inline size_t NetObs_Case3InitHelperBufferAOffset()
{
	return NetObs_IsDx12Exe() ? 0xA0 : 0xA8;
}

inline size_t NetObs_Case3InitHelperBufferBOffset()
{
	return NetObs_IsDx12Exe() ? 0xA8 : 0xB0;
}

inline uintptr_t NetObs_PdefCompressedBufferAddr()
{
	return NetObs_Sym(NetObsSym_t::PdefCompressedBuffer);
}

inline uintptr_t NetObs_PdefCompressedSizeAddr()
{
	return NetObs_Sym(NetObsSym_t::PdefCompressedSize);
}

inline uintptr_t NetObs_Bz2DecompressAddr()
{
	return NetObs_Sym(NetObsSym_t::Bz2Decompress);
}

inline uintptr_t NetObs_OodleLZCompressAddr()
{
	return NetObs_Sym(NetObsSym_t::OodleLZCompress);
}

inline uintptr_t NetObs_PdefParseAddr()
{
	return NetObs_Sym(NetObsSym_t::PdefParse);
}

inline uintptr_t NetObs_PdefGlobalsAddr()
{
	return NetObs_Sym(NetObsSym_t::PdefGlobals);
}

inline uintptr_t NetObs_PdefLoadedFlagAddr()
{
	return NetObs_Sym(NetObsSym_t::PdefLoadedFlag);
}

inline uintptr_t NetObs_PdefVersionCacheAddr()
{
	return NetObs_Sym(NetObsSym_t::PdefVersionCache);
}

inline uintptr_t NetObs_PdefPbCountAddr()
{
	return NetObs_Sym(NetObsSym_t::PdefPbCount);
}

inline uintptr_t NetObs_PdefPbArrayAddr()
{
	return NetObs_Sym(NetObsSym_t::PdefPbArray);
}

inline uintptr_t NetObs_CNetChanRegistryArrayPtrAddr()
{
	return NetObs_Sym(NetObsSym_t::CNetChanRegistryArrayPtr);
}

inline uintptr_t NetObs_CNetChanRegistryCountAddr()
{
	return NetObs_Sym(NetObsSym_t::CNetChanRegistryCount);
}

inline uintptr_t NetObs_PlatStateAddr()
{
	return NetObs_Sym(NetObsSym_t::PlatState);
}

inline uintptr_t NetObs_PlatformModeGateAddr()
{
	return NetObs_Sym(NetObsSym_t::PlatformModeGate);
}

inline uintptr_t NetObs_AltEntityInitFlagAddr()
{
	return NetObs_Sym(NetObsSym_t::AltEntityInitFlag);
}

inline uintptr_t NetObs_AltEntityPoolAddr()
{
	return NetObs_Sym(NetObsSym_t::AltEntityPool);
}

inline uintptr_t NetObs_OriginEntityPoolAddr()
{
	return NetObs_Sym(NetObsSym_t::OriginEntityPool);
}

// EHandle (idx|serial) -> entity pointer via the handle table (stride 4 qwords).
// Serial-checked, readable preflight (no SEH -- VEH fires first).
inline uintptr_t NetObs_ResolveEHandle(uint32_t eh)
{
	if (eh == 0xFFFFFFFF) return 0;
	const uintptr_t tableVA = NetObs_EntityHandleTableAddr();
	if (!tableVA) return 0;
	uintptr_t* tableAddr = reinterpret_cast<uintptr_t*>(tableVA);
	// Wire handles carry a 16-bit index but the table only holds MAX_EDICTS
	// stride-4 slots -- past that is foreign heap, not an entity.
	const uint32_t idx = eh & 0xFFFF;
	if (idx >= MAX_EDICTS) return 0;
	const uint16_t expectedSerial = static_cast<uint16_t>(eh >> 16);
	// Slot is two qwords at table[idx*4] / table[idx*4+1].
	const uintptr_t slotAddr = reinterpret_cast<uintptr_t>(&tableAddr[idx * 4]);

	if (!Mem_IsReadableCached(reinterpret_cast<const void*>(slotAddr),
			2 * sizeof(uintptr_t)))
		return 0;

	const uintptr_t entPtr = tableAddr[idx * 4];
	const uint64_t serialQword = tableAddr[idx * 4 + 1];
	const uint32_t actualSerial = static_cast<uint32_t>(serialQword & 0xFFFFFFFF);
	if (actualSerial != static_cast<uint32_t>(expectedSerial)) return 0;
	return entPtr;
}

// S2C ScriptRemote frame magic (S3 net_ScriptMessage 68, isTyped=0). Must match the dedi.
static constexpr uint32_t BRIDGE_S2C_SCRIPTREMOTE_MAGIC = 0x53523244u;
static constexpr uint32_t BRIDGE_S2C_MANTLEBOOST_MAGIC = 0x3156424Du;

//-----------------------------------------------------------------------------
// Cross-TU shared symbols (defined in net_observer.cpp unless noted).
//-----------------------------------------------------------------------------
extern bool AttachInTxn(PVOID* ppTarget, PVOID pDetour, const char* name);
extern const char* DT_ClassName(int cid);   // recvtable class name by client classID
extern const char* DT_TableName(int cid);   // recvtable DT name by client classID
extern int s_slotClassID[16384];            // per-slot client classID (+1 biased)

// SEH-guarded fixed-cap C-string copy; defined in net_observer.cpp, used by the
// decode TU dumps and the core model-precache / player-state dumps.
extern void DF_CopyStr(uintptr_t p, char* out, int cap);
// Global dummy RecvProp that unmatched wire slots bind to (decoder-builder cave).
// Defined in engine/client/net_bridge_install.cpp; used by the bind-census walk.
extern const void* NetBridge_GlobalDummyRecvProp(void);
// Model-precache items array base; defined in net_observer.cpp.
extern uintptr_t NetObs_ModelPrecacheItemsAddr();
// S21 exe image base (cached); defined in net_observer.cpp, used by the decode
// TU classname registry and the core S21 bf_read resolver.
extern uintptr_t S21_GetExeBase();
// Map bare base name from CClientState+0x1BC (m_szLevelBaseName); "" if not connected.
extern const char* Bridge_GetLevelBaseName();
// S21 CClientState*; 0 before the first signon.
extern uintptr_t Bridge_ClientStatePtr(void);
// Fire a client-VM callback taking one entity arg. Caller must gate on IsFirstTimePredicted.
extern void NetBridge_FireClientPlayerCallback(const char* funcName, void* pPlayerEnt);

//-----------------------------------------------------------------------------
// Shared decode <-> apply-pipe surface.
//-----------------------------------------------------------------------------
extern bool IsBadRecvPropPtr(uintptr_t p);            // defined in net_bridge_decode.cpp
extern bool DT_ReadCursor(uintptr_t bitbuf, uintptr_t& curPtr, int& bitsAvail);
// [TE-IDXSEQ] per-TE-decode prop-index recorder. thread_local: decode is multi-threaded.
extern thread_local int  g_teIdxSeq[64];
extern thread_local int  g_teIdxSeqCount;
extern void NetObsTe_ResetIdxSeq(void);
extern bool DT_IsZiplineOrZiprailClass(int cid);
extern void Bridge_InstallApplyPipeHooks(void);
extern void Bridge_InstallEarlyPipeHooks(void); // pre-connect RCON host-console pump
extern void Bridge_PumpGate_Bootstrap(void); // arms the unfocused-Present gate pre-connect
extern void BridgeStubDiag(const char* fmt, ...);

extern ConVar sdk_hdelta_trace;
extern ConVar bridge_net_flow_diag;
extern ConVar bridge_decode_seh;
// [PB-READ] set by Hook_PropApplyLoop only while applying the PLAYER
// (slot 1); gates the packed-read leaf hook in net_bridge_decode.cpp so it
// logs only the player's PASS-B reads. See [PB-READ-LEAF].
extern volatile long g_pbReadActive;
extern int s_enterPvsCount[16384];

// [SNAP-PROF] per-inject create/decode counters. Reset before each inject.
extern long long g_snapProfCreateN;
extern long long g_snapProfDecodeN;
extern long long g_snapProfCreateTicks;   // QPC ticks spent inside native CL_CopyNewEntity (decode)
extern bool      g_snapProfOn;            // armed only around the SNAP-PROF inject window
void SnapProf_Reset(void);

// Readable-probe / SEH counters for the per-prop path. Reset with SnapBudget_Reset.
// g_dispVqCalls: real VirtualQuery syscalls (readable-cache misses).
// g_dispSehHits: caught exceptions in the per-prop dispatch path.
extern volatile long g_dispVqCalls;
extern volatile long g_dispSehHits;

// [CMD-RATE] once-per-second usercmd supply. C2S adds stamped frametime; ClSendTick
// adds frames / created cmds / host_frametime. Reset on FullyConnected.

// [SNAP-BUDGET] per-snapshot cost: m_nTotalTicks is the hook, m_nOrigTicks the native wrap.
// TSC not QPC -- dispatch is the highest-call-count path.
enum SnapBudgetSlot_t
{
	SNAPB_GENDELTAS = 0,   // Hook_GenerateDeltas       -- per entity
	SNAPB_DISPATCH,        // Hook_PropDecodeDispatch   -- per prop
	SNAPB_RTDECODE,        // Hook_RecvTableDecode      -- per entity/table
	SNAPB_RTDECODEMAIN,    // Hook_RecvTableDecodeMain  -- per entity/table
	SNAPB_COPYNEWENT,      // Hook_CL_CopyNewEntity_Body-- per created entity
	SNAPB_COUNT
};

struct SnapBudgetEntry_t
{
	volatile long long m_nTotalTicks;
	volatile long long m_nOrigTicks;
	volatile long      m_nCalls;
};

// Profiler counters only: decode runs on more than one thread, and a lost sample
// costs a slightly low number, never correctness. Aligned 8-byte loads do not tear.
extern SnapBudgetEntry_t g_snapBudget[SNAPB_COUNT];
extern bool              g_snapBudgetOn;   // armed for the sampled snapshot only
void SnapBudget_Reset(void);

// No RAII here on purpose: several of these hooks contain __try/__except, and MSVC
// rejects an object requiring unwinding in the same function (C2712).
#define SNAPB_T0(v)          unsigned long long v = g_snapBudgetOn ? __rdtsc() : 0ULL
#define SNAPB_ADD_TOTAL(s,v) do { if (v) { g_snapBudget[s].m_nTotalTicks += (long long)(__rdtsc() - (v)); g_snapBudget[s].m_nCalls++; } } while (0)
#define SNAPB_ADD_ORIG(s,v)  do { if (v) { g_snapBudget[s].m_nOrigTicks  += (long long)(__rdtsc() - (v)); } } while (0)

// Delta-header / per-prop decode trace (PROP-TRACE) state.
extern int s_dtSnap;
// thread_local per-decode-thread trace context (s_ptTotalLines stays the shared
// process-lifetime emit budget) -- see s_currentDecodeRecvTable in the decode TU.
extern thread_local bool      s_ptActive;
extern thread_local int       s_ptEntIdx;
extern thread_local int       s_ptPropN;
extern thread_local int       s_ptLastIdx;
extern thread_local long long s_ptPrevPos;
extern long      s_ptTotalLines;
extern thread_local bool      s_ptEmitLines;
struct PropTraceCrumb
{
	int propN;
	long long idx;
	long long pos;
	long long idxBits;
	long long prevValBits;
};
extern thread_local PropTraceCrumb s_ptCrumbs[32];
extern thread_local int            s_ptCrumbHead;
extern thread_local int            s_ptCrumbCount;

//-----------------------------------------------------------------------------
// File-only SDK_Log redirect: gated traces go to bridge_trace.log, not stdout.
//-----------------------------------------------------------------------------
extern void SDK_Log(const char* fmt, ...);
extern void BridgeTrace_Log(const char* fmt, ...);
bool Bridge_DiagFirehoseEnabled(void);      // bridge_diag_firehose
extern void Bridge_DumpInstanceBaseline(void);
// Dump the per-entity wire body-bit ring to bridge_trace.log.
extern void Bridge_DumpBodyBitsRing(const char* tag);
// Dump the entity decode crumb ring from the C2S SignonState __except.
extern void Bridge_DumpDecodeCrumbsOnCrash(const char* tag);
#define SDK_Log(...) \
    do { if (SDK_OBSERVE_NET_GE(1)) BridgeTrace_Log(__VA_ARGS__); } while (0)

#endif // CORE_NET_BRIDGE_INTERNAL_H
