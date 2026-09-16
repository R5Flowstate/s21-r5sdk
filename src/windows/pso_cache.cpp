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

static ConVar sdk_pso_cache_burst_n("sdk_pso_cache_burst_n", "0", FCVAR_RELEASE,
	"Flush psoCache.pso after this many new pipeline creates while dirty. "
	"0 disables the count trigger.", true, 0.f, true, 4096.f);

static ConVar sdk_pso_cache_burst_secs("sdk_pso_cache_burst_secs", "60", FCVAR_RELEASE,
	"Also flush psoCache.pso when dirty for this many seconds during creates. "
	"0 disables the time trigger.", true, 0.f, true, 3600.f);

static ConVar sdk_pso_cache_settle_secs("sdk_pso_cache_settle_secs", "8", FCVAR_RELEASE,
	"Flush psoCache.pso this many seconds after the last pipeline create "
	"while dirty. 0 disables.", true, 0.f, true, 3600.f);

static ConVar sdk_pso_cache_retry_secs("sdk_pso_cache_retry_secs", "15", FCVAR_RELEASE,
	"Seconds to wait after a failed psoCache write before retrying. "
	"0 retries immediately.", true, 0.f, true, 3600.f);

static ConVar sdk_pso_cache_diag("sdk_pso_cache_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log every psoCache flush attempt and its cost.");

static SRWLOCK s_writeLock = SRWLOCK_INIT;
static double s_flLastFlushTime = 0.0;
static double s_flLastCreateTime = 0.0;
static double s_flNextRetryTime = 0.0;
static uint64_t s_nFlushCount = 0;
static uint64_t s_nBytesWritten = 0;
static uint64_t s_nOrphansSwept = 0;
static uint64_t s_nOrphanBytes = 0;
static long s_nLastBurstCount = 0;
static DWORD s_nLastWinErr = 0;
static ULONGLONG s_ullLastPumpMs = 0;
static volatile LONG s_nSweptOnce = 0;

extern void S21Bridge_PumpWhileStalled(void);

static const char* PsoCache_FileName(const char* const pszPath)
{
	if (!pszPath || !pszPath[0])
		return "";

	const char* pszName = pszPath;
	for (const char* p = pszPath; *p; ++p)
	{
		if (*p == '\\' || *p == '/')
			pszName = p + 1;
	}
	return pszName;
}

static bool PsoCache_IsCacheFileName(const char* const pszPath)
{
	return _stricmp(PsoCache_FileName(pszPath), "psoCache.pso") == 0;
}

static bool PsoCache_IsOrphanName(const char* const pszName)
{
	if (!pszName || strchr(pszName, '\\') || strchr(pszName, '/'))
		return false;
	if (_stricmp(pszName, "psoCache.pso") == 0)
		return false;
	if (_strnicmp(pszName, "psoCache.pso", 12) != 0)
		return false;

	const char* const pszRest = pszName + 12;
	if (_stricmp(pszRest, ".tmp") == 0)
		return true;
	if (_strnicmp(pszRest, ".tmp.", 5) == 0)
		return true;

	const size_t nRest = strlen(pszRest);
	return nRest >= 9 && _stricmp(pszRest + nRest - 9, ".deleteme") == 0;
}

static bool PsoCache_BuildPath(char* const pszOut, const size_t nOut)
{
	pszOut[0] = '\0';

	if (!v_PsoCache_GetUserDir(pszOut, int(nOut), 0))
		return false;

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
	return g_pPsoCacheCtx && g_pPsoCacheEnabled && v_PsoCache_GetUserDir;
}

static void PsoCache_SweepOrphans(const char* const pszCachePath)
{
	if (!pszCachePath || !pszCachePath[0])
		return;

	const char* const pszName = PsoCache_FileName(pszCachePath);
	if (!pszName || pszName == pszCachePath)
		return;

	char szDir[MAX_PATH];
	const size_t nDir = static_cast<size_t>(pszName - pszCachePath);
	if (nDir == 0 || nDir >= sizeof(szDir))
		return;

	memcpy(szDir, pszCachePath, nDir);
	szDir[nDir] = '\0';

	char szSearch[MAX_PATH];
	const int nSearch = snprintf(szSearch, sizeof(szSearch), "%spsoCache.pso*", szDir);
	if (nSearch <= 0 || nSearch >= static_cast<int>(sizeof(szSearch)))
		return;

	WIN32_FIND_DATAA fd;
	const HANDLE hFind = FindFirstFileA(szSearch, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return;

	int nSwept = 0;
	uint64_t nBytes = 0;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (!PsoCache_IsOrphanName(fd.cFileName))
			continue;
		if (nSwept >= 64)
			break;

		char szOrphan[MAX_PATH];
		const int nOrphan = snprintf(szOrphan, sizeof(szOrphan), "%s%s", szDir, fd.cFileName);
		if (nOrphan <= 0 || nOrphan >= static_cast<int>(sizeof(szOrphan)))
			continue;

		if (DeleteFileA(szOrphan))
		{
			nSwept++;
			nBytes += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
		}
	} while (FindNextFileA(hFind, &fd));

	FindClose(hFind);

	if (nSwept > 0)
	{
		s_nOrphansSwept += static_cast<uint64_t>(nSwept);
		s_nOrphanBytes += nBytes;
		Warning(eDLL_T::MS, "[PSO-CACHE] swept %d orphans (%llu bytes)\n",
			nSwept, nBytes);
	}
}

static void PsoCache_TrySweepOnce(void)
{
	if (s_nSweptOnce)
		return;
	if (!PsoCache_Resolved())
		return;

	char szPath[MAX_PATH];
	if (!PsoCache_BuildPath(szPath, sizeof(szPath)))
		return;
	if (InterlockedCompareExchange(&s_nSweptOnce, 1, 0) != 0)
		return;

	PsoCache_SweepOrphans(szPath);
}

static DWORD PsoCache_WriteAtomic(const char* const pszPath, const void* const pData, const size_t nSize)
{
	if (!pszPath || !pszPath[0] || (nSize && !pData))
		return ERROR_INVALID_PARAMETER;

	PsoCache_SweepOrphans(pszPath);

	const DWORD nAttr = GetFileAttributesA(pszPath);
	if (nAttr != INVALID_FILE_ATTRIBUTES)
	{
		if (nAttr & FILE_ATTRIBUTE_DIRECTORY)
			return ERROR_DIRECTORY;
		if (nAttr & FILE_ATTRIBUTE_REPARSE_POINT)
			return ERROR_ACCESS_DENIED;
	}

	char szTmp[MAX_PATH + 8];
	if (strlen(pszPath) + 5 >= sizeof(szTmp))
		return ERROR_BUFFER_OVERFLOW;
	snprintf(szTmp, sizeof(szTmp), "%s.tmp", pszPath);
	DeleteFileA(szTmp);

	const HANDLE hFile = CreateFileA(szTmp, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return GetLastError();

	const uint8_t* pCur = static_cast<const uint8_t*>(pData);
	size_t nLeft = nSize;
	while (nLeft)
	{
		const DWORD nChunk = nLeft > 0x100000 ? 0x100000 : static_cast<DWORD>(nLeft);
		DWORD nWrote = 0;
		if (!WriteFile(hFile, pCur, nChunk, &nWrote, nullptr) || nWrote != nChunk)
		{
			const DWORD nErr = GetLastError();
			CloseHandle(hFile);
			DeleteFileA(szTmp);
			return nErr ? nErr : ERROR_WRITE_FAULT;
		}
		pCur += nChunk;
		nLeft -= nChunk;
	}

	FlushFileBuffers(hFile);
	CloseHandle(hFile);

	if (nAttr == INVALID_FILE_ATTRIBUTES)
	{
		if (MoveFileA(szTmp, pszPath))
			return 0;
	}
	else if (MoveFileExA(szTmp, pszPath, MOVEFILE_REPLACE_EXISTING))
		return 0;

	return GetLastError();
}

static bool __fastcall Hook_PsoCache_WriteFile(const char* pszPath, uint8_t nLogChannel, const void* pData, size_t nSize)
{
	if (!PsoCache_IsCacheFileName(pszPath))
		return v_PsoCache_WriteFile(pszPath, nLogChannel, pData, nSize);

	AcquireSRWLockExclusive(&s_writeLock);
	s_nLastWinErr = PsoCache_WriteAtomic(pszPath, pData, nSize);
	ReleaseSRWLockExclusive(&s_writeLock);
	return s_nLastWinErr == 0;
}

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

	const double flNow = Plat_StallClockSeconds();
	if (!bForce && s_flNextRetryTime > 0.0 && flNow < s_flNextRetryTime)
		return false;

	char szPath[MAX_PATH];
	if (!PsoCache_BuildPath(szPath, sizeof(szPath)))
	{
		Warning(eDLL_T::MS, "[PSO-CACHE] could not resolve the user directory\n");
		return false;
	}

	AcquireSRWLockExclusive(&s_writeLock);

	if (!g_pPsoCacheCtx->m_bDirty && !bForce)
	{
		ReleaseSRWLockExclusive(&s_writeLock);
		return true;
	}

	s_flLastFlushTime = Plat_StallClockSeconds();
	if (g_pPsoCreateCount)
		s_nLastBurstCount = *g_pPsoCreateCount;

	const PSRWLOCK pLock = reinterpret_cast<PSRWLOCK>(&g_pPsoCacheCtx->m_SRWLock);
	const double flStart = Plat_StallClockSeconds();

	AcquireSRWLockExclusive(pLock);
	g_pPsoCacheCtx->m_bDirty = 0;

	const SIZE_T nBlob = pLibrary->GetSerializedSize();
	if (!nBlob)
	{
		ReleaseSRWLockExclusive(pLock);
		ReleaseSRWLockExclusive(&s_writeLock);
		return true;
	}

	const uint64_t nCap = uint64_t(g_pPsoCacheCtx->m_nMaxCacheMB) << 20;
	if (uint64_t(nBlob) + sizeof(kPsoCacheMagic) >= nCap)
	{
		ReleaseSRWLockExclusive(pLock);
		ReleaseSRWLockExclusive(&s_writeLock);
		Warning(eDLL_T::MS, "[PSO-CACHE] %llu bytes exceeds pso_max_cache_size_MB (%u) -- not written\n",
			uint64_t(nBlob), g_pPsoCacheCtx->m_nMaxCacheMB);
		return false;
	}

	const size_t nTotal = size_t(nBlob) + sizeof(kPsoCacheMagic);
	uint8_t* const pBlob = static_cast<uint8_t*>(malloc(nTotal));
	if (!pBlob)
	{
		ReleaseSRWLockExclusive(pLock);
		ReleaseSRWLockExclusive(&s_writeLock);
		Warning(eDLL_T::MS, "[PSO-CACHE] out of memory for a %zu byte blob\n", nTotal);
		return false;
	}

	*reinterpret_cast<uint32_t*>(pBlob) = kPsoCacheMagic;
	const HRESULT hr = pLibrary->Serialize(pBlob + sizeof(kPsoCacheMagic), nBlob);
	ReleaseSRWLockExclusive(pLock);

	DWORD nErr = 0;
	if (SUCCEEDED(hr))
		nErr = PsoCache_WriteAtomic(szPath, pBlob, nTotal);
	else
		nErr = ERROR_INVALID_DATA;

	free(pBlob);

	const double flElapsed = Plat_StallClockSeconds() - flStart;
	s_nLastWinErr = nErr;
	s_flLastFlushTime = Plat_StallClockSeconds();

	if (nErr)
	{
		g_pPsoCacheCtx->m_bDirty = 1;
		const float flRetry = sdk_pso_cache_retry_secs.GetFloat();
		if (flRetry > 0.f)
			s_flNextRetryTime = s_flLastFlushTime + double(flRetry);
		Warning(eDLL_T::MS, "[PSO-CACHE] write failed winerr=%u hr=0x%08X '%s'\n",
			nErr, unsigned(hr), szPath);
		ReleaseSRWLockExclusive(&s_writeLock);
		return false;
	}

	s_flNextRetryTime = 0.0;
	s_nFlushCount++;
	s_nBytesWritten = nTotal;
	if (g_pPsoCreateCount)
		s_nLastBurstCount = *g_pPsoCreateCount;

	if (s_nFlushCount == 1 || sdk_pso_cache_diag.GetBool())
		Msg(eDLL_T::MS, "[PSO-CACHE] wrote %zu bytes in %.1f ms -> '%s'\n",
			nTotal, flElapsed * 1000.0, szPath);

	ReleaseSRWLockExclusive(&s_writeLock);
	return true;
}

void PsoCache_Frame(void)
{
	if (!PsoCache_Resolved())
		return;

	PsoCache_TrySweepOnce();

	if (!*g_pPsoCacheEnabled || !g_pPsoCacheCtx->m_pLibrary || !g_pPsoCacheCtx->m_bDirty)
		return;

	const double flNow = Plat_StallClockSeconds();
	if (s_flLastFlushTime == 0.0)
		s_flLastFlushTime = flNow;

	const float flSettle = sdk_pso_cache_settle_secs.GetFloat();
	if (flSettle > 0.f && s_flLastCreateTime > 0.0
		&& (flNow - s_flLastCreateTime) >= double(flSettle))
	{
		PsoCache_Flush(false);
		return;
	}

	const float flInterval = sdk_pso_cache_autoflush_secs.GetFloat();
	if (flInterval > 0.f && (flNow - s_flLastFlushTime) >= double(flInterval))
		PsoCache_Flush(false);
}

static void PsoCache_OnCreated(void)
{
	const ULONGLONG ullNow = GetTickCount64();
	if (ullNow - s_ullLastPumpMs >= 50)
	{
		s_ullLastPumpMs = ullNow;
		S21Bridge_PumpWhileStalled();
	}

	s_flLastCreateTime = Plat_StallClockSeconds();
	PsoCache_TrySweepOnce();

	if (!PsoCache_Resolved() || !*g_pPsoCacheEnabled || !g_pPsoCacheCtx->m_pLibrary)
		return;
	if (!g_pPsoCacheCtx->m_bDirty)
		return;

	const int nBurst = sdk_pso_cache_burst_n.GetInt();
	const float flBurstSecs = sdk_pso_cache_burst_secs.GetFloat();
	if (nBurst <= 0 && flBurstSecs <= 0.f)
		return;

	const long nCreated = g_pPsoCreateCount ? *g_pPsoCreateCount : 0;
	const double flNow = s_flLastCreateTime;
	if (s_flLastFlushTime == 0.0)
		s_flLastFlushTime = flNow;

	const bool bByCount = (nBurst > 0) && (nCreated - s_nLastBurstCount >= nBurst);
	const bool bByTime = (flBurstSecs > 0.f)
		&& (flNow - s_flLastFlushTime >= double(flBurstSecs));
	if (!bByCount && !bByTime)
		return;

	PsoCache_Flush(false);
}

static void* __fastcall Hook_PsoCreateGraphics(void* pA1, void* pCtx)
{
	void* const p = v_PsoCreateGraphics(pA1, pCtx);
	static volatile LONG s_once = 0;
	if (InterlockedCompareExchange(&s_once, 1, 0) == 0)
		Msg(eDLL_T::MS, "[PSO-CACHE] create-hook live\n");
	PsoCache_OnCreated();
	return p;
}

static void* __fastcall Hook_PsoCreateCompute(void* pThis)
{
	void* const p = v_PsoCreateCompute(pThis);
	PsoCache_OnCreated();
	return p;
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
	else
		PsoCache_SweepOrphans(szPath);

	int64_t nOnDisk = -1;
	if (szPath[0])
	{
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (GetFileAttributesExA(szPath, GetFileExInfoStandard, &fad))
			nOnDisk = (int64_t(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
	}

	const double flNow = Plat_StallClockSeconds();
	const double flBackoff = (s_flNextRetryTime > flNow) ? (s_flNextRetryTime - flNow) : 0.0;

	Msg(eDLL_T::MS, "[PSO-CACHE] enabled=%d library=%p dirty=%d capMB=%u\n",
		int(*g_pPsoCacheEnabled), g_pPsoCacheCtx->m_pLibrary,
		int(g_pPsoCacheCtx->m_bDirty), g_pPsoCacheCtx->m_nMaxCacheMB);
	Msg(eDLL_T::MS, "[PSO-CACHE] path '%s' onDisk=%lld\n", szPath, nOnDisk);
	Msg(eDLL_T::MS, "[PSO-CACHE] flushes=%llu lastWrite=%llu bytes lastWinErr=%u backoff=%.1fs\n",
		s_nFlushCount, s_nBytesWritten, s_nLastWinErr, flBackoff);
	Msg(eDLL_T::MS, "[PSO-CACHE] orphansSwept=%llu orphanBytes=%llu\n",
		s_nOrphansSwept, s_nOrphanBytes);

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
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 56 41 57 48 83 EC ?? 48 8B EA 4C 8B F1 45 85 C0")
		.GetPtr(v_PsoCache_GetUserDir);

	Module_FindPattern(g_GameDll,
		"48 89 6C 24 ?? 56 57 41 56 48 83 EC ?? 49 8B F1")
		.GetPtr(v_PsoCache_WriteFile);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B F2 33 FF")
		.GetPtr(v_PsoCreateGraphics);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B 51 ?? 4C 8D 84 24")
		.GetPtr(v_PsoCreateCompute);

	if (!v_PsoCache_GetUserDir)
		Warning(eDLL_T::MS, "[PSO-CACHE] GetUserDir unresolved -- flush disabled\n");
	if (!v_PsoCache_WriteFile)
		Warning(eDLL_T::MS, "[PSO-CACHE] WriteFile unresolved -- shutdown redirect off\n");
	if (!v_PsoCreateGraphics)
		Warning(eDLL_T::MS, "[PSO-CACHE] graphics create unresolved -- load flush off (DX11 is fine)\n");
}

void VPsoCacheS21::Detour(const bool bAttach) const
{
	if (v_PsoCreateGraphics)
		DetourSetup(&v_PsoCreateGraphics, &Hook_PsoCreateGraphics, bAttach);
	if (v_PsoCreateCompute)
		DetourSetup(&v_PsoCreateCompute, &Hook_PsoCreateCompute, bAttach);
	if (v_PsoCache_WriteFile)
		DetourSetup(&v_PsoCache_WriteFile, &Hook_PsoCache_WriteFile, bAttach);
}

void VPsoCacheS21::GetVar(void) const
{
	g_pPsoCacheCtx = Module_FindPattern(g_GameDll, "48 8D 2D ?? ?? ?? ?? 44 8B C3 4C 8D 0D")
		.ResolveRelativeAddress(0x3, 0x7).RCast<PsoCacheCtx_t*>();

	g_pPsoCacheEnabled = Module_FindPattern(g_GameDll, "C6 05 ?? ?? ?? ?? 01 44 8B 50 64")
		.ResolveRelativeAddress(0x2, 0x7).RCast<uint8_t*>();

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
