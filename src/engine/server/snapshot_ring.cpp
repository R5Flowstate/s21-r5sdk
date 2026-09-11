//=============================================================================//
//
// Purpose: Live snapshot frame-store ring deepen.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "snapshot_diag.h"
#include "snapshot_ring.h"
#include "snapshot_dump.h"

#include <cstdint>
#include <climits>
#include <atomic>

// sv_max_snapshots_* is DEVELOPMENTONLY and registers after detours attach, so
// launch args and FindVar miss it -- the allocator hook is the only way in.
static ConVar sdk_bridge_snapshot_ring("sdk_bridge_snapshot_ring", "160",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Minimum snapshot frame-store ring depth in ticks (the delta-baseline search space). "
	"Stock engine value is 160. 0 = leave the engine value alone.");
// Snapshot_Initialize builds the descriptor pool at mgr+0x110B00 with
// maxSnapshots+1 entries; pool+0x40 holds them, 32 bytes each, the snapshot
// block at +8. The modulus the commit path indexes by lives at mgr+8.
static constexpr ptrdiff_t SNAPMGR_PTR_RVA      = 0x234DE408;
static constexpr ptrdiff_t SNAPMGR_OFF_DEPTH    = 0x08;
static constexpr ptrdiff_t SNAPMGR_OFF_TICKS    = 0x18;
static constexpr ptrdiff_t SNAPMGR_OFF_RINGBASE = 0x110B40;
static constexpr ptrdiff_t SNAPMGR_OFF_RINGMOD  = 0x08;

uintptr_t SnapshotRing_Mgr(void)
{
	const uintptr_t base = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!base)
		return 0;
	return static_cast<uintptr_t>(*reinterpret_cast<uint64_t*>(base + SNAPMGR_PTR_RVA));
}

int SnapshotRing_Depth(void)
{
	const uintptr_t mgr = SnapshotRing_Mgr();
	return mgr ? *reinterpret_cast<int*>(mgr + SNAPMGR_OFF_DEPTH) : 0;
}

bool SnapshotRing_HasTick(int nTick, int* pOldest, int* pNewest)
{
	if (pOldest) *pOldest = -1;
	if (pNewest) *pNewest = -1;

	const uintptr_t mgr = SnapshotRing_Mgr();
	if (!mgr)
		return false;

	const int        depth = *reinterpret_cast<int*>(mgr + SNAPMGR_OFF_DEPTH);
	const int* const ticks = *reinterpret_cast<int**>(mgr + SNAPMGR_OFF_TICKS);
	if (depth <= 0 || depth > 65536 || !ticks)
		return false;

	bool bFound = false;
	int  oldest = INT_MAX;
	int  newest = INT_MIN;
	for (int i = 0; i < depth; ++i)
	{
		const int t = ticks[i];
		if (t == nTick)
			bFound = true;
		if (t <= 0)
			continue;
		if (t < oldest) oldest = t;
		if (t > newest) newest = t;
	}
	if (pOldest && oldest != INT_MAX) *pOldest = oldest;
	if (pNewest && newest != INT_MIN) *pNewest = newest;
	return bFound;
}

//-----------------------------------------------------------------------------
// maxSnapshots sizes the ring and every co-array derived from it.
//-----------------------------------------------------------------------------
static int64_t Hook_Snapshot_Initialize(int64_t mgr, uint8_t replaySupport,
	uint8_t replayPredSmooth, int maxClients, int maxSnapshots, int maxProps, int maxTempents)
{
	const int nWant = sdk_bridge_snapshot_ring.GetInt();
	const int nUse  = (nWant > maxSnapshots) ? nWant : maxSnapshots;

	static std::atomic<uint32_t> s_initCount{ 0 };
	const uint32_t n = s_initCount.fetch_add(1, std::memory_order_relaxed) + 1;

	// Log every call, including pass-through -- those size every pack pool.
	Msg(eDLL_T::SERVER,
		"[SNAP-RING] #%u Snapshot_Initialize mgr=%p replay=%d/%d clients=%d "
		"max_snapshots %d -> %d props=%d tempents=%d\n",
		n, (void*)mgr, replaySupport, replayPredSmooth, maxClients,
		maxSnapshots, nUse, maxProps, maxTempents);

	// A ring rebuild is a per-map event. Anything approaching per-frame means
	// the reinit compare is thrashing and the deepen must be reconsidered.
	if (n == 64)
		Warning(eDLL_T::SERVER,
			"[SNAP-RING] Snapshot_Initialize has run %u times -- ring rebuild is "
			"far more frequent than per-map; check the reinit param compare.\n", n);

	const int64_t r = v_Snapshot_Initialize(mgr, replaySupport, replayPredSmooth,
		maxClients, nUse, maxProps, maxTempents);

	// Did this call actually bind the ring blocks it just sized?
	__try
	{
		const uintptr_t ring = *reinterpret_cast<uintptr_t*>(
			static_cast<uintptr_t>(mgr) + SNAPMGR_OFF_RINGBASE);
		const int nMod = *reinterpret_cast<int*>(
			static_cast<uintptr_t>(mgr) + SNAPMGR_OFF_RINGMOD);
		const uintptr_t e0 = ring
			? *reinterpret_cast<uintptr_t*>(ring + 8) : 0;
		Msg(eDLL_T::SERVER,
			"[SNAP-RING] #%u after init: ring=%p mod=%d entry[0]=%p\n",
			n, (void*)ring, nMod, (void*)e0);
		SnapshotDump_OnRingInit(ring, e0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	return r;
}


void VSnapshotRingDeepen::GetFun(void) const
{
	// Snapshot_Initialize -- the ring/co-array allocator. Prologue signature is
	// safe here: single build, verified 1 hit.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 54 41 55 41 56 41 57 "
		"48 83 EC ?? 48 63 AC 24")
		.GetPtr(v_Snapshot_Initialize);
	if (!v_Snapshot_Initialize)
		Warning(eDLL_T::SERVER,
			"[SNAP-RING] Snapshot_Initialize pattern unresolved -- snapshot ring "
			"stays at the engine default (160 ticks); an ack stall longer than "
			"8s will force a full-entity rebuild on the client.\n");
}

void VSnapshotRingDeepen::Detour(const bool bAttach) const
{
	if (!v_Snapshot_Initialize)
	{
		Warning(eDLL_T::SERVER,
			"[SNAP-RING] Snapshot_Initialize pattern unresolved -- snapshot ring "
			"stays at the engine default (160 ticks); an ack stall longer than "
			"8s will force a full-entity rebuild on the client.\n");
		return;
	}
	const LONG result = bAttach
		? DetourAttach(reinterpret_cast<void**>(&v_Snapshot_Initialize),
			reinterpret_cast<void*>(&Hook_Snapshot_Initialize))
		: DetourDetach(reinterpret_cast<void**>(&v_Snapshot_Initialize),
			reinterpret_cast<void*>(&Hook_Snapshot_Initialize));
	if (bAttach)
	{
		Msg(eDLL_T::SERVER,
			"[SNAP-RING] DetourAttach Snapshot_Initialize result=0x%lX "
			"(target=0x%p, ring>=%d)\n", result, (void*)v_Snapshot_Initialize,
			sdk_bridge_snapshot_ring.GetInt());
	}
}
