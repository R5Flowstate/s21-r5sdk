//=============================================================================//
//
// Purpose: Sparse surface-data path can return NULL; clamp/fallback to index 0 before callers deref +12 on footstep/impact dispatch.
// Hook the per-model surface-data accessor: on null for a non-zero index, retry index 0 (same contract as CPhysicsSurfaceProps::GetSurfaceData).
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "cmodel_surfdata.h"

static ConVar sdk_surfdata_fallback_guard("sdk_surfdata_fallback_guard", "1", FCVAR_RELEASE,
	"Restore the index-0 fallback on the S21 client's per-model surface-data "
	"accessor when the requested surface index has no entry"
	"in its sparse override table. Mirrors the engine's own "
	"CPhysicsSurfaceProps::GetSurfaceData contract, which never returns "
	"null. 0 = pass-through (let it crash).");

typedef __int64(__fastcall* PFN_SurfDataByIndex)(__int64 /*a1*/, unsigned __int16 /*a2*/);
static PFN_SurfDataByIndex v_SurfDataByIndex = nullptr;

static __int64 __fastcall Hook_SurfDataByIndex(__int64 a1, unsigned __int16 a2)
{
	const __int64 result = v_SurfDataByIndex(a1, a2);

	if (result || a2 == 0 || !sdk_surfdata_fallback_guard.GetBool())
		return result;

	const __int64 fallback = v_SurfDataByIndex(a1, 0);

	static volatile LONG s_logCount = 0;
	const LONG n = InterlockedIncrement(&s_logCount);
	if (n <= 50)
	{
		Warning(eDLL_T::ENGINE,
			"[SURFDATA-FALLBACK-GUARD] #%ld surface index %u missing from override "
			"table (this=%p) -- fell back to index 0 (result=%p) instead of "
			"crashing.\n",
			n, (unsigned)a2, (void*)a1, (void*)fallback);
	}

	return fallback;
}

void VSurfDataFallbackGuardS21::GetAdr(void) const
{
	LogFunAdr("SurfDataByIndex", v_SurfDataByIndex);
}

void VSurfDataFallbackGuardS21::GetFun(void) const
{
	// prologue (S21 @ )
	// mov rax, [rcx+10h]; test rax, rax; jnz +0x1D; mov r9, [rcx+8];...

	Module_FindPattern(g_GameDll,
		"48 8B 41 ?? 48 85 C0 75 ?? 4C 8B 49 ?? 45 0F B7 41")
		.GetPtr(v_SurfDataByIndex);

	if (!v_SurfDataByIndex)
		Warning(eDLL_T::ENGINE, "[SURFDATA-FALLBACK-GUARD] pattern UNRESOLVED -- fallback NOT installed\n");
}

void VSurfDataFallbackGuardS21::Detour(const bool bAttach) const
{
	if (v_SurfDataByIndex)
		DetourSetup(&v_SurfDataByIndex, &Hook_SurfDataByIndex, bAttach);
}
