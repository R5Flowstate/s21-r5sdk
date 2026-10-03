//=============================================================================//
//
// Purpose: Let collision-prop update jobs read under the entity transform lock
//          that their dispatcher already holds exclusively.
//
// The dispatcher holds the lock and waits on these jobs, so a worker that has to
// recompute a dirty transform under it (AIClass titan NPCs with IK chains) would
// block forever; inside the job the worker borrows the dispatcher's hold instead.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "coll_prop_job.h"

static ConVar cl_collprop_job_lock_borrow("cl_collprop_job_lock_borrow", "1", FCVAR_RELEASE,
	"Collision-prop update jobs borrow the transform lock their dispatcher holds "
	"exclusively instead of deadlocking on it. 0 = stock behavior.");

typedef void(__fastcall* PFN_CollPropJob)(__int64 ctx, unsigned __int64 start, __int64 count);
typedef char(__fastcall* PFN_TransformLockShared)(__int64 a1, __int64 a2, __int64 a3);
typedef void(__fastcall* PFN_TransformUnlockShared)(__int64 a1, __int64 a2, __int64 a3);

static PFN_CollPropJob v_CollPropJob = nullptr;
static PFN_TransformLockShared v_TransformLockShared = nullptr;
static PFN_TransformUnlockShared v_TransformUnlockShared = nullptr;

static thread_local int t_collPropJobDepth = 0;
static thread_local int t_borrowedHolds = 0;
static volatile LONG s_borrowAnnounced = 0;

static void __fastcall Hook_CollPropJob(__int64 ctx, unsigned __int64 start, __int64 count)
{
	++t_collPropJobDepth;
	v_CollPropJob(ctx, start, count);
	--t_collPropJobDepth;
}

static char __fastcall Hook_TransformLockShared(__int64 a1, __int64 a2, __int64 a3)
{
	if (t_collPropJobDepth > 0 && cl_collprop_job_lock_borrow.GetBool())
	{
		if (InterlockedCompareExchange(&s_borrowAnnounced, 1, 0) == 0)
		{
			Msg(eDLL_T::CLIENT, "[COLLPROP-LOCK] update job borrowed the dispatcher's "
				"transform lock (dirty entity needs a locked recompute)\n");
		}

		++t_borrowedHolds;
		return 2;
	}

	return v_TransformLockShared(a1, a2, a3);
}

static void __fastcall Hook_TransformUnlockShared(__int64 a1, __int64 a2, __int64 a3)
{
	// Borrowed holds never touched the engine's per-thread count; pair them here.
	if (t_borrowedHolds > 0)
	{
		--t_borrowedHolds;
		return;
	}

	v_TransformUnlockShared(a1, a2, a3);
}

void VCollPropJobLock::GetAdr(void) const
{
	LogFunAdr("CollPropUpdateJob", v_CollPropJob);
	LogFunAdr("TransformLockShared", v_TransformLockShared);
	LogFunAdr("TransformUnlockShared", v_TransformUnlockShared);
}

void VCollPropJobLock::GetFun(void) const
{
	// Job body: per-entity bounds refit, then a TryAcquire'd tree insert.
	Module_FindPattern(g_GameDll,
		"40 57 48 83 EC ?? 48 89 6C 24 ?? 48 8B F9 4A 8D 2C 02")
		.GetPtr(v_CollPropJob);

	// Reentrant shared acquire: per-thread count += 2, AcquireSRWLockShared on 0.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 65 48 8B 04 25 58 00 00 00 48 8B 18 B8 ?? ?? ?? ?? "
		"80 3C 18 00 75 ?? E8 ?? ?? ?? ?? B8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? "
		"8B 1C 18 48 03 D9 0F B6 03 84 C0 75 ?? 48 8D 0D ?? ?? ?? ?? "
		"FF 15 ?? ?? ?? ?? 0F B6 03 04 02 88 03")
		.GetPtr(v_TransformLockShared);

	// Matching release: per-thread count -= 2, tail-jumps ReleaseSRWLockShared at 0.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 65 48 8B 04 25 58 00 00 00 48 8B 18 B8 ?? ?? ?? ?? "
		"80 3C 18 00 75 ?? E8 ?? ?? ?? ?? B8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? "
		"8B 04 18 80 04 08 FE 75 ?? 48 8D 0D ?? ?? ?? ?? 48 83 C4 20 5B 48 FF 25")
		.GetPtr(v_TransformUnlockShared);

	if (!v_CollPropJob || !v_TransformLockShared || !v_TransformUnlockShared)
	{
		Warning(eDLL_T::CLIENT, "[COLLPROP-LOCK] pattern unresolved (job=%p lock=%p unlock=%p) "
			"-- deadlock guard NOT installed\n",
			reinterpret_cast<void*>(v_CollPropJob),
			reinterpret_cast<void*>(v_TransformLockShared),
			reinterpret_cast<void*>(v_TransformUnlockShared));
	}
}

void VCollPropJobLock::Detour(const bool bAttach) const
{
	// All three or none: a borrow without its paired release would leak a hold.
	if (!v_CollPropJob || !v_TransformLockShared || !v_TransformUnlockShared)
		return;

	DetourSetup(&v_CollPropJob, &Hook_CollPropJob, bAttach);
	DetourSetup(&v_TransformLockShared, &Hook_TransformLockShared, bAttach);
	DetourSetup(&v_TransformUnlockShared, &Hook_TransformUnlockShared, bAttach);
}
