//=============================================================================//
//
// Purpose: Mid-run flush of the S21 DX12 pipeline library to psoCache.pso.
//
//=============================================================================//
#include "core/stdafx.h"
#include "windows/pso_cache.h"
#include "tier0/dbg.h"
#include "tier0/platform.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"

#include <d3d12.h>

static_assert(offsetof(PsoCacheCtx_t, m_SRWLock) == 0x08, "PsoCacheCtx_t lock slot");
static_assert(offsetof(PsoCacheCtx_t, m_nMaxCacheMB) == 0x18, "PsoCacheCtx_t cap slot");
static_assert(offsetof(PsoCacheCtx_t, m_bDirty) == 0x1C, "PsoCacheCtx_t dirty slot");

static ConVar sdk_pso_cache_autoflush_secs("sdk_pso_cache_autoflush_secs", "300", FCVAR_RELEASE,
	"Seconds between automatic psoCache.pso flushes. 0 disables; a flush stalls "
	"pipeline creation for the length of the serialize.", true, 0.f, true, 3600.f);

static ConVar sdk_pso_cache_diag("sdk_pso_cache_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log every psoCache flush attempt and its cost.");

static double s_flLastFlushTime = 0.0;
static uint64_t s_nFlushCount = 0;
static uint64_t s_nBytesWritten = 0;

//-----------------------------------------------------------------------------
// Purpose: <Saved Games>/Respawn/<profile>/local/psoCache.pso
//-----------------------------------------------------------------------------
static bool PsoCache_BuildPath(char* const pszOut, const size_t nOut)
{
	pszOut[0] = '\0';

	if (!v_PsoCache_GetUserDir(pszOut, int(nOut), 0))
		return false;

	// The engine assembles the file name into a buffer just below the context.
	const char* const pszName =
		reinterpret_cast<const char*>(g_pPsoCacheCtx) + kPsoCacheNameOffset;

	const size_t nUsed = strlen(pszOut);
	if (nUsed + strlen(pszName) + 2 > nOut)
		return false;

	snprintf(pszOut + nUsed, nOut - nUsed, "\\%s", pszName);
	return true;
}

static bool PsoCache_Resolved(void)
{
	return g_pPsoCacheCtx && g_pPsoCacheEnabled
		&& v_PsoCache_GetUserDir && v_PsoCache_WriteFile;
}

//-----------------------------------------------------------------------------
// Purpose: serialize the live pipeline library and write it to disk
//-----------------------------------------------------------------------------
bool PsoCache_Flush(const bool bForce)
{
	if (!PsoCache_Resolved())
	{
		Warning(eDLL_T::MS, "[PSO-CACHE] patterns unresolved -- flush unavailable\n");
		return false;
	}

	if (!*g_pPsoCacheEnabled)
	{
		Warning(eDLL_T::MS, "[PSO-CACHE] disabled by -no_pso_caching or -renderdoc\n");
		return false;
	}

	ID3D12PipelineLibrary* const pLibrary = g_pPsoCacheCtx->m_pLibrary;
	if (!pLibrary)
	{
		Warning(eDLL_T::MS, "[PSO-CACHE] no pipeline library on the device\n");
		return false;
	}

	if (!g_pPsoCacheCtx->m_bDirty && !bForce)
	{
		if (sdk_pso_cache_diag.GetBool())
			DevMsg(eDLL_T::MS, "[PSO-CACHE] clean, nothing to write\n");
		return true;
	}

	char szPath[MAX_PATH];
	if (!PsoCache_BuildPath(szPath, sizeof(szPath)))
	{
		Warning(eDLL_T::MS, "[PSO-CACHE] could not resolve the user directory\n");
		return false;
	}

	const PSRWLOCK pLock = reinterpret_cast<PSRWLOCK>(&g_pPsoCacheCtx->m_SRWLock);
	const double flStart = Plat_StallClockSeconds();

	AcquireSRWLockExclusive(pLock);

	// Clear first: a StorePipeline racing in behind us re-dirties, so its
	// pipeline is caught by the next flush instead of being silently dropped.
	g_pPsoCacheCtx->m_bDirty = 0;

	const SIZE_T nBlob = pLibrary->GetSerializedSize();
	if (!nBlob)
	{
		ReleaseSRWLockExclusive(pLock);
		return true;
	}

	const uint64_t nCap = uint64_t(g_pPsoCacheCtx->m_nMaxCacheMB) << 20;
	if (uint64_t(nBlob) + sizeof(kPsoCacheMagic) >= nCap)
	{
		ReleaseSRWLockExclusive(pLock);
		Warning(eDLL_T::MS, "[PSO-CACHE] %llu bytes exceeds pso_max_cache_size_MB (%u) -- not written\n",
			uint64_t(nBlob), g_pPsoCacheCtx->m_nMaxCacheMB);
		return false;
	}

	const size_t nTotal = size_t(nBlob) + sizeof(kPsoCacheMagic);
	uint8_t* const pBlob = static_cast<uint8_t*>(malloc(nTotal));
	if (!pBlob)
	{
		ReleaseSRWLockExclusive(pLock);
		Warning(eDLL_T::MS, "[PSO-CACHE] out of memory for a %zu byte blob\n", nTotal);
		return false;
	}

	*reinterpret_cast<uint32_t*>(pBlob) = kPsoCacheMagic;
	const HRESULT hr = pLibrary->Serialize(pBlob + sizeof(kPsoCacheMagic), nBlob);

	ReleaseSRWLockExclusive(pLock);

	bool bWritten = false;
	if (SUCCEEDED(hr))
		bWritten = v_PsoCache_WriteFile(szPath, 0, pBlob, nTotal);

	free(pBlob);

	const double flElapsed = Plat_StallClockSeconds() - flStart;
	s_flLastFlushTime = Plat_StallClockSeconds();

	if (!bWritten)
	{
		g_pPsoCacheCtx->m_bDirty = 1;
		Warning(eDLL_T::MS, "[PSO-CACHE] write failed (hr=0x%08X) '%s'\n",
			unsigned(hr), szPath);
		return false;
	}

	s_nFlushCount++;
	s_nBytesWritten = nTotal;

	if (sdk_pso_cache_diag.GetBool())
		Msg(eDLL_T::MS, "[PSO-CACHE] wrote %zu bytes in %.1f ms -> '%s'\n",
			nTotal, flElapsed * 1000.0, szPath);

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: per-frame auto-flush tick
//-----------------------------------------------------------------------------
void PsoCache_Frame(void)
{
	const float flInterval = sdk_pso_cache_autoflush_secs.GetFloat();
	if (flInterval <= 0.f || !PsoCache_Resolved())
		return;

	if (!*g_pPsoCacheEnabled || !g_pPsoCacheCtx->m_pLibrary || !g_pPsoCacheCtx->m_bDirty)
		return;

	const double flNow = Plat_StallClockSeconds();
	if (s_flLastFlushTime == 0.0)
	{
		s_flLastFlushTime = flNow;
		return;
	}

	if (flNow - s_flLastFlushTime < double(flInterval))
		return;

	PsoCache_Flush(false);
}

void PsoCache_PrintStatus(void)
{
	if (!PsoCache_Resolved())
	{
		Msg(eDLL_T::MS, "[PSO-CACHE] unresolved -- this build is not on a DX12 host\n");
		return;
	}

	char szPath[MAX_PATH];
	if (!PsoCache_BuildPath(szPath, sizeof(szPath)))
		szPath[0] = '\0';

	int64_t nOnDisk = -1;
	if (szPath[0])
	{
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (GetFileAttributesExA(szPath, GetFileExInfoStandard, &fad))
			nOnDisk = (int64_t(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
	}

	Msg(eDLL_T::MS, "[PSO-CACHE] enabled=%d library=%p dirty=%d capMB=%u\n",
		int(*g_pPsoCacheEnabled), g_pPsoCacheCtx->m_pLibrary,
		int(g_pPsoCacheCtx->m_bDirty), g_pPsoCacheCtx->m_nMaxCacheMB);
	Msg(eDLL_T::MS, "[PSO-CACHE] path '%s' onDisk=%lld\n", szPath, nOnDisk);
	Msg(eDLL_T::MS, "[PSO-CACHE] flushes=%llu lastWrite=%llu bytes\n",
		s_nFlushCount, s_nBytesWritten);

	if (g_pPsoCreateCount && g_pPsoCreateMs)
		Msg(eDLL_T::MS, "[PSO-CACHE] created %ld pipelines this session, %.2f s spent\n",
			*g_pPsoCreateCount, double(*g_pPsoCreateMs) / 1000.0);
}

static void CC_PsoCache_Flush_f(const CCommand& args)
{
	PsoCache_Flush(true);
}
static ConCommand sdk_pso_cache_flush("sdk_pso_cache_flush", CC_PsoCache_Flush_f,
	"Serialize the DX12 pipeline library to psoCache.pso now.",
	FCVAR_RELEASE | FCVAR_CLIENTDLL);

static void CC_PsoCache_Status_f(const CCommand& args)
{
	PsoCache_PrintStatus();
}
static ConCommand sdk_pso_cache_status("sdk_pso_cache_status", CC_PsoCache_Status_f,
	"Report DX12 pipeline library state and this session's pipeline-creation cost.",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL);

void VPsoCacheS21::GetFun(void) const
{
	// Builds the Saved Games sub-path, owns the -fnf branch.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 56 41 57 48 83 EC ?? 48 8B EA 4C 8B F1 45 85 C0")
		.GetPtr(v_PsoCache_GetUserDir);

	// WriteWholeFile.
	Module_FindPattern(g_GameDll,
		"48 89 6C 24 ?? 56 57 41 56 48 83 EC ?? 49 8B F1")
		.GetPtr(v_PsoCache_WriteFile);

	if (!v_PsoCache_GetUserDir || !v_PsoCache_WriteFile)
		Warning(eDLL_T::MS, "[PSO-CACHE] function patterns unresolved -- flush disabled\n");
}

void VPsoCacheS21::GetVar(void) const
{
	// Inside the PSO cache init: 'lea rbp, ctx' feeding CreatePipelineLibrary.
	g_pPsoCacheCtx = Module_FindPattern(g_GameDll, "48 8D 2D ?? ?? ?? ?? 44 8B C3 4C 8D 0D")
		.ResolveRelativeAddress(0x3, 0x7).RCast<PsoCacheCtx_t*>();

	// Same function: 'mov byte, 1' on the branch that survives both opt-out args.
	g_pPsoCacheEnabled = Module_FindPattern(g_GameDll, "C6 05 ?? ?? ?? ?? 01 44 8B 50 64")
		.ResolveRelativeAddress(0x2, 0x7).RCast<uint8_t*>();

	// Tail of the graphics PSO create: 'lock xadd ms' then 'lock inc count'.
	const CMemory counters = Module_FindPattern(g_GameDll,
		"2B C3 F0 0F C1 05 ?? ?? ?? ?? F0 FF 05");

	if (counters.IsValid())
	{
		g_pPsoCreateMs = counters.Offset(0x2)
			.ResolveRelativeAddress(0x4, 0x8).RCast<volatile long*>();
		g_pPsoCreateCount = counters.Offset(0xA)
			.ResolveRelativeAddress(0x3, 0x7).RCast<volatile long*>();
	}

	if (!g_pPsoCacheCtx || !g_pPsoCacheEnabled)
		Warning(eDLL_T::MS, "[PSO-CACHE] variable patterns unresolved -- flush disabled\n");
}
