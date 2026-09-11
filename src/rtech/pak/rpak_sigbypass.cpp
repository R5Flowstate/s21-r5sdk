//=============================================================================//
// S21 RPak signature bypass -- opt-in via sdk_rpak_sig_bypass.
// Default ON for bridge custom content; set 0 to run stock verify.
//=============================================================================//

#include "core/stdafx.h"
#include "rpak_sigbypass.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier1/convar.h"
#include <cstddef>

// Behaviour lever: 1 = accept unsigned paks (custom bridge content) and skip
// Authenticode on allowlisted HAS_MODULE sibling DLLs. 0 = stock verify.
static ConVar sdk_rpak_sig_bypass("sdk_rpak_sig_bypass", "1", FCVAR_RELEASE,
	"Bypass RPak RSA/hash signature checks so unsigned custom paks load, and "
	"skip Authenticode on allowlisted pak sibling modules. "
	"1 = bypass (default for bridge content). 0 = stock verify only.");

static bool s_bypassedAnnounced = false;
static bool s_stockAnnounced = false;

static int __fastcall Hook_Pak_VerifySignature_S21(
	__int64 pakContext, __int64 a2, __int64 a3, __int64 a4)
{
	if (!sdk_rpak_sig_bypass.GetBool())
	{
		if (!s_stockAnnounced)
		{
			s_stockAnnounced = true;
			Msg(eDLL_T::RTECH,
				"[SEC-BYPASS] RPak signature STOCK (sdk_rpak_sig_bypass 0) -- "
				"unsigned paks will fail verify\n");
		}
		if (v_Pak_VerifySignature_S21)
			return v_Pak_VerifySignature_S21(pakContext, a2, a3, a4);
		Warning(eDLL_T::RTECH,
			"[SEC-BYPASS] RPak signature orig null with bypass OFF -- refusing\n");
		return 1;
	}

	if (!s_bypassedAnnounced)
	{
		s_bypassedAnnounced = true;
		Msg(eDLL_T::RTECH,
			"[SEC-BYPASS] RPak signature bypass ACTIVE (sdk_rpak_sig_bypass 1) -- "
			"unsigned custom paks accepted\n");
	}
	return 0;
}

// WINTRUST_ACTION_GENERIC_VERIFY_V2 -- the only action the pak loader uses.
static const GUID kWinTrustGenericVerifyV2 =
{
	0x00AAC56Bu, 0xCD44, 0x11D0,
	{ 0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE }
};

enum { kWtdChoiceFile = 1 };

struct PakWinTrustFileInfo
{
	DWORD cbStruct;
	LPCWSTR pcwszFilePath;
	HANDLE hFile;
	GUID* pgKnownSubject;
};

struct PakWinTrustData
{
	DWORD cbStruct;
	LPVOID pPolicyCallbackData;
	LPVOID pSIPCallbackData;
	DWORD dwUIChoice;
	DWORD fdwRevocationChecks;
	DWORD dwUnionChoice;
	PakWinTrustFileInfo* pFile;
};

typedef LONG (WINAPI *PFN_WinVerifyTrust)(HWND hwnd, GUID* action, LPVOID data);

static PFN_WinVerifyTrust v_WinVerifyTrust = nullptr;
static PFN_WinVerifyTrust s_WinVerifyTrustOrig = nullptr;

// Exact basenames only -- same names as the Pak_SetupBuffersAndLoad allowlist.
// A prefix rule would let a dropped file squat it.
static const wchar_t* const s_allowedModuleDlls[] =
{
	L"ui.dll",
	L"ui_fs.dll",
};

static bool PakModule_IsTrustAllowlisted(const wchar_t* const path)
{
	if (!path || !*path)
		return false;

	for (const wchar_t* p = path; *p; ++p)
	{
		if (p[0] == L'.' && p[1] == L'.')
			return false;
	}

	const wchar_t* base = path;
	for (const wchar_t* p = path; *p; ++p)
	{
		if (*p == L'\\' || *p == L'/')
			base = p + 1;
	}

	for (size_t i = 0; i < SDK_ARRAYSIZE(s_allowedModuleDlls); ++i)
	{
		if (_wcsicmp(base, s_allowedModuleDlls[i]) == 0)
			return true;
	}
	return false;
}

static LONG WINAPI Hook_WinVerifyTrust(HWND hwnd, GUID* action, LPVOID pWVTData)
{
	if (sdk_rpak_sig_bypass.GetBool() && action && pWVTData &&
		memcmp(action, &kWinTrustGenericVerifyV2, sizeof(GUID)) == 0)
	{
		const PakWinTrustData* const data =
			static_cast<const PakWinTrustData*>(pWVTData);

		if (data->cbStruct >= offsetof(PakWinTrustData, pFile) + sizeof(void*) &&
			data->dwUnionChoice == kWtdChoiceFile &&
			data->pFile && data->pFile->pcwszFilePath &&
			PakModule_IsTrustAllowlisted(data->pFile->pcwszFilePath))
		{
			Msg(eDLL_T::RTECH,
				"[PAK-TRUST] Authenticode skipped for allowlisted '%ls'\n",
				data->pFile->pcwszFilePath);
			return ERROR_SUCCESS;
		}
	}

	if (!v_WinVerifyTrust)
		return 1;

	return v_WinVerifyTrust(hwnd, action, pWVTData);
}

void VRPakSigBypassS21::Detour(const bool bAttach) const
{
	if (v_Pak_VerifySignature_S21)
	{
		DetourSetup(&v_Pak_VerifySignature_S21,
			&Hook_Pak_VerifySignature_S21, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::RTECH,
			"[SEC-BYPASS] Pak_VerifySignature pattern unresolved -- not attaching\n");
	}

	if (!g_WinVerifyTrustIat_S21)
	{
		if (bAttach)
		{
			Warning(eDLL_T::RTECH,
				"[PAK-TRUST] WINTRUST.dll!WinVerifyTrust IAT unresolved -- "
				"unsigned pak modules will fail Authenticode\n");
		}
		return;
	}

	if (bAttach)
	{
		CMemory::HookImportedFunction(g_WinVerifyTrustIat_S21,
			reinterpret_cast<const void*>(&Hook_WinVerifyTrust),
			reinterpret_cast<void**>(&s_WinVerifyTrustOrig));
		v_WinVerifyTrust = s_WinVerifyTrustOrig;

		if (!v_WinVerifyTrust)
		{
			Warning(eDLL_T::RTECH,
				"[PAK-TRUST] WinVerifyTrust IAT original was null -- not hooked\n");
			return;
		}

		Msg(eDLL_T::RTECH,
			"[PAK-TRUST] WinVerifyTrust IAT hooked (allowlisted pak modules only)\n");
	}
	else if (s_WinVerifyTrustOrig)
	{
		void* ignored = nullptr;
		CMemory::HookImportedFunction(g_WinVerifyTrustIat_S21,
			reinterpret_cast<const void*>(s_WinVerifyTrustOrig),
			&ignored);
		v_WinVerifyTrust = s_WinVerifyTrustOrig;
	}
}
