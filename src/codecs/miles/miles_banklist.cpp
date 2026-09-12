//=============================================================================//
//
// Purpose: Packed banks.rson is general-only; append disk "custom" after the
//          stock list load so CSOM MilesBankLoad sees the second bank.
//
//          A bank is loaded through its audio.mprj BankEntries[BankIndex]
//          slot, so the bank file and the project are one consistent set. The
//          append is gated on the deployed project actually declaring that
//          slot -- a stock audio.mprj alongside a BankIndex 1 bank would index
//          past the table.
//
//          CSOM always formats audio/ship/<bank>_<miles_language>.mstr when
//          language is set. A missing localize fails MilesBankGetStatus
//          closed. custom.mbnk StreamOffsets[1] is the 32-byte english stub
//          (size 32, digest 0); Miles skips the hash when the file meow is 0.
//          Write or accept that stub -- do not treat header-only as poison.
//
//          We ship English banks only. ClientSoundMiles_Initialize pins the
//          cvar and the CSOM latch to "english" so a non-English OS locale
//          cannot format general_japanese.mstr and kill the mixer.
//
//=============================================================================//

#include "core/stdafx.h"
#include "core/logdef.h"
#include "miles_banklist.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"

#include <Windows.h>

static ConVar miles_bank_disk(
	"miles_bank_disk", "1", FCVAR_RELEASE,
	"When 1, append 'custom' to the Miles bank list if audio/ship/custom.mbnk exists.");

static constexpr int BANK_NAME_STRIDE = 64;
static constexpr int BANK_COUNT_OFF = 1024; // CSOM bankList.count
static constexpr unsigned int BANK_CAP = 16;

static const char* const MILES_CUSTOM_BANK_PATH = "audio/ship/custom.mbnk";
static const char* const MILES_PROJECT_PATH = "audio/ship/audio.mprj";
static const char* const MILES_LANGUAGE_FORCE = "english";

static constexpr int MBNK_BANK_INDEX_OFF = 0xA0;  // KNBC CompiledBank.BankIndex
static constexpr int MBNK_BUILDTAG_OFF = 0x10;    // KNBC CompiledBank.BuildTag
static constexpr int MPRJ_BANK_COUNT_OFF = 0xF2;  // JRPC CompiledSettings.BankCount
static constexpr DWORD MILES_MSTR_HEADER_ONLY = 32; // RTSC header, no samples

static bool MilesBankDisk_ReadHeader(const char* pszPath, unsigned char* pBuf, const DWORD nSize)
{
	const HANDLE hFile = CreateFileA(pszPath, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	DWORD nRead = 0;
	const bool bOk = ReadFile(hFile, pBuf, nSize, &nRead, NULL) != FALSE && nRead == nSize;
	CloseHandle(hFile);

	return bOk;
}

static bool MilesBankDisk_GetBankIndex(unsigned int* pBankIndex)
{
	unsigned char header[MBNK_BANK_INDEX_OFF + 1] = {};

	if (!MilesBankDisk_ReadHeader(MILES_CUSTOM_BANK_PATH, header, sizeof(header)))
		return false;

	if (memcmp(header, "KNBC", 4) != 0)
		return false;

	*pBankIndex = header[MBNK_BANK_INDEX_OFF];
	return true;
}

static bool MilesBankDisk_ProjectDeclaresBank(const unsigned int nBankIndex)
{
	unsigned char header[MPRJ_BANK_COUNT_OFF + 1] = {};

	if (!MilesBankDisk_ReadHeader(MILES_PROJECT_PATH, header, sizeof(header)))
		return false;

	if (memcmp(header, "JRPC", 4) != 0)
		return false;

	return header[MPRJ_BANK_COUNT_OFF] > nBankIndex;
}

static unsigned int MilesBankDisk_ListCount(const char* bankList)
{
	const unsigned int count = *reinterpret_cast<const unsigned int*>(bankList + BANK_COUNT_OFF);
	return count > BANK_CAP ? BANK_CAP : count;
}

static const char* MilesBankDisk_Language(void)
{
	if (!g_pCVar)
		return "";

	ConVar* const lang = g_pCVar->FindVar("miles_language");
	if (!lang)
		return "";

	const char* const psz = lang->GetString();
	return psz ? psz : "";
}

static void MilesBankDisk_ForceEnglish(void)
{
	const char* pszLatch = "";
	if (s_ppszMilesLanguageLatch && *s_ppszMilesLanguageLatch)
		pszLatch = *s_ppszMilesLanguageLatch;

	if (s_ppszMilesLanguageLatch)
		*s_ppszMilesLanguageLatch = MILES_LANGUAGE_FORCE;

	ConVar* lang = nullptr;
	if (g_pCVar)
		lang = g_pCVar->FindVar("miles_language");

	const char* pszCvar = "";
	if (lang)
	{
		const char* const psz = lang->GetString();
		if (psz)
			pszCvar = psz;
		if (V_stricmp(pszCvar, MILES_LANGUAGE_FORCE) != 0)
			lang->SetValue(MILES_LANGUAGE_FORCE);
	}

	if (V_stricmp(pszLatch, MILES_LANGUAGE_FORCE) == 0
		&& V_stricmp(pszCvar, MILES_LANGUAGE_FORCE) == 0)
		return;

	Warning(eDLL_T::AUDIO,
		"[MILES-BANK] forced miles_language english (latch='%s' cvar='%s') -- "
		"English banks only\n", pszLatch, pszCvar);
}

static char __fastcall Hook_ClientSoundMiles_Initialize(void)
{
	MilesBankDisk_ForceEnglish();
	return v_ClientSoundMiles_Initialize();
}

static unsigned int MilesBankDisk_CustomBuildTag(void)
{
	unsigned char header[MBNK_BUILDTAG_OFF + 4] = {};
	if (!MilesBankDisk_ReadHeader(MILES_CUSTOM_BANK_PATH, header, sizeof(header)))
		return 0;
	if (memcmp(header, "KNBC", 4) != 0)
		return 0;

	unsigned int nTag = 0;
	memcpy(&nTag, header + MBNK_BUILDTAG_OFF, sizeof(nTag));
	return nTag;
}

static bool MilesBankDisk_WriteEnglishStub(const char* pszPath, const unsigned int nBuildTag)
{
	unsigned char hdr[MILES_MSTR_HEADER_ONLY] = {};
	memcpy(hdr, "RTSC", 4);

	const unsigned short nVer = 2;
	const unsigned short nLang = 0;
	const unsigned int nMeow = 0;
	const unsigned long long nFileSize = MILES_MSTR_HEADER_ONLY;
	memcpy(hdr + 4, &nVer, sizeof(nVer));
	memcpy(hdr + 6, &nLang, sizeof(nLang));
	memcpy(hdr + 16, &nBuildTag, sizeof(nBuildTag));
	memcpy(hdr + 20, &nMeow, sizeof(nMeow));
	memcpy(hdr + 24, &nFileSize, sizeof(nFileSize));

	const HANDLE hFile = CreateFileA(pszPath, GENERIC_WRITE, FILE_SHARE_READ,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	DWORD nWritten = 0;
	const bool bOk = WriteFile(hFile, hdr, sizeof(hdr), &nWritten, NULL) != FALSE
		&& nWritten == sizeof(hdr);
	CloseHandle(hFile);
	return bOk;
}

static bool MilesBankDisk_EnglishStubIsValid(const char* pszPath, const unsigned int nBuildTag)
{
	unsigned char hdr[MILES_MSTR_HEADER_ONLY] = {};
	if (!MilesBankDisk_ReadHeader(pszPath, hdr, sizeof(hdr)))
		return false;
	if (memcmp(hdr, "RTSC", 4) != 0)
		return false;

	unsigned short nVer = 0;
	unsigned short nLang = 0;
	unsigned int nTag = 0;
	unsigned int nMeow = 0;
	memcpy(&nVer, hdr + 4, sizeof(nVer));
	memcpy(&nLang, hdr + 6, sizeof(nLang));
	memcpy(&nTag, hdr + 16, sizeof(nTag));
	memcpy(&nMeow, hdr + 20, sizeof(nMeow));
	return nVer == 2 && nLang == 0 && nTag == nBuildTag && nMeow == 0;
}

static bool MilesBankDisk_LocalizedStreamIsPoison(const char* pszBank)
{
	const char* const pszLang = MilesBankDisk_Language();
	if (!pszLang[0] || !pszBank || !pszBank[0])
		return false;

	char path[MAX_PATH] = {};
	V_snprintf(path, sizeof(path), "audio/ship/%s_%s.mstr", pszBank, pszLang);

	const unsigned int nBuildTag = MilesBankDisk_CustomBuildTag();
	if (!nBuildTag)
	{
		Warning(eDLL_T::AUDIO,
			"[MILES-BANK] %s has no readable BuildTag -- cannot validate %s\n",
			MILES_CUSTOM_BANK_PATH, path);
		return true;
	}

	WIN32_FILE_ATTRIBUTE_DATA fad = {};
	const bool bExists = GetFileAttributesExA(path, GetFileExInfoStandard, &fad) != FALSE;
	ULARGE_INTEGER sz = {};
	if (bExists)
	{
		sz.LowPart = fad.nFileSizeLow;
		sz.HighPart = fad.nFileSizeHigh;
	}

	if (bExists && sz.QuadPart > MILES_MSTR_HEADER_ONLY)
		return false;

	if (bExists && sz.QuadPart == MILES_MSTR_HEADER_ONLY
		&& MilesBankDisk_EnglishStubIsValid(path, nBuildTag))
		return false;

	if (bExists && sz.QuadPart == MILES_MSTR_HEADER_ONLY)
	{
		Warning(eDLL_T::AUDIO,
			"[MILES-BANK] %s is a 32-byte file but not a valid english stub "
			"(want RTSC v2 lang=0 meow=0 build %08x) -- rewriting\n",
			path, nBuildTag);
	}
	else if (!bExists)
	{
		Warning(eDLL_T::AUDIO,
			"[MILES-BANK] %s missing -- writing the 32-byte english stub "
			"custom.mbnk StreamOffsets[1] expects\n",
			path);
	}
	else
	{
		Warning(eDLL_T::AUDIO,
			"[MILES-BANK] %s is truncated (%llu bytes) -- Miles will fail custom\n",
			path, static_cast<unsigned long long>(sz.QuadPart));
		return true;
	}

	if (MilesBankDisk_WriteEnglishStub(path, nBuildTag)
		&& MilesBankDisk_EnglishStubIsValid(path, nBuildTag))
	{
		Msg(eDLL_T::AUDIO,
			"[MILES-BANK] wrote %s (32-byte english stub, build %08x)\n",
			path, nBuildTag);
		return false;
	}

	Warning(eDLL_T::AUDIO,
		"[MILES-BANK] could not write %s -- refusing custom so a missing "
		"localize cannot fail CSOM_Initialize closed\n",
		path);
	return true;
}

static bool MilesBankDisk_RemoveName(char* bankList, const char* pszName)
{
	const unsigned int count = MilesBankDisk_ListCount(bankList);
	for (unsigned int i = 0; i < count; i++)
	{
		char* const slot = bankList + (static_cast<size_t>(i) * BANK_NAME_STRIDE);
		if (V_stricmp(slot, pszName) != 0)
			continue;

		if (i + 1 < count)
		{
			memmove(slot, slot + BANK_NAME_STRIDE,
				static_cast<size_t>(count - i - 1) * BANK_NAME_STRIDE);
		}

		memset(bankList + (static_cast<size_t>(count - 1) * BANK_NAME_STRIDE),
			0, BANK_NAME_STRIDE);
		*reinterpret_cast<unsigned int*>(bankList + BANK_COUNT_OFF) = count - 1;
		return true;
	}

	return false;
}

static __int64 __fastcall Hook_MilesShared_LoadBanksListFromFile(char* bankList, __int64 a2, __int64 a3, __int64 a4)
{
	const __int64 result = v_MilesShared_LoadBanksListFromFile(bankList, a2, a3, a4);
	if (!bankList)
		return result;

	unsigned int count = MilesBankDisk_ListCount(bankList);

	bool hasCustom = false;
	for (unsigned int i = 0; i < count; i++)
	{
		const char* name = bankList + (static_cast<size_t>(i) * BANK_NAME_STRIDE);
		Msg(eDLL_T::AUDIO, "[MILES-BANK] list[%u]='%s'\n", i, name);
		if (name[0] && V_stricmp(name, "custom") == 0)
			hasCustom = true;
	}

	const bool bLangPoison = MilesBankDisk_LocalizedStreamIsPoison("custom");
	if (hasCustom && bLangPoison)
	{
		if (MilesBankDisk_RemoveName(bankList, "custom"))
		{
			count = MilesBankDisk_ListCount(bankList);
			hasCustom = false;
			Warning(eDLL_T::AUDIO,
				"[MILES-BANK] stripped custom (miles_language='%s') -- localized "
				"stream missing or invalid\n",
				MilesBankDisk_Language());
		}
	}

	if (!miles_bank_disk.GetBool())
		return result;

	if (hasCustom)
	{
		Msg(eDLL_T::AUDIO, "[MILES-BANK] list already has custom (count=%u)\n", count);
		return result;
	}

	if (bLangPoison)
	{
		Warning(eDLL_T::AUDIO,
			"[MILES-BANK] refusing to append custom (miles_language='%s') -- "
			"localized stream missing or invalid\n",
			MilesBankDisk_Language());
		return result;
	}

	unsigned int bankIndex = 0;
	if (!MilesBankDisk_GetBankIndex(&bankIndex))
	{
		Msg(eDLL_T::AUDIO, "[MILES-BANK] no readable KNBC at %s -- count=%u\n",
			MILES_CUSTOM_BANK_PATH, count);
		return result;
	}

	// Slot 0 is general's, so a custom bank claiming it is misbuilt -- validating
	// "the slot exists" would pass it and point a second bank at general's entry.
	if (bankIndex == 0)
	{
		Warning(eDLL_T::AUDIO,
			"[MILES-BANK] %s has BankIndex 0, which belongs to general -- rebuild it "
			"with bank_index 1\n", MILES_CUSTOM_BANK_PATH);
		return result;
	}

	if (!MilesBankDisk_ProjectDeclaresBank(bankIndex))
	{
		Warning(eDLL_T::AUDIO,
			"[MILES-BANK] audio.mprj declares fewer than %u banks -- refusing to append "
			"custom (BankIndex %u); redeploy the patched project\n",
			bankIndex + 1, bankIndex);
		return result;
	}

	if (count >= BANK_CAP)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] bankList full (%u) -- cannot append custom\n", count);
		return result;
	}

	char* slot = bankList + (static_cast<size_t>(count) * BANK_NAME_STRIDE);
	V_strncpy(slot, "custom", BANK_NAME_STRIDE);
	*reinterpret_cast<unsigned int*>(bankList + BANK_COUNT_OFF) = count + 1;
	Msg(eDLL_T::AUDIO, "[MILES-BANK] appended custom count=%u->%u\n", count, count + 1);
	return result;
}

void VMilesBankListS21::GetFun(void) const
{
	// MilesShared_LoadBanksListFromFile. Unique on S21.
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 83 EC 78 48 89 58 08 48 8D 15 ?? ?? ?? ?? 48 89 68 10")
		.GetPtr(v_MilesShared_LoadBanksListFromFile);

	if (!v_MilesShared_LoadBanksListFromFile)
		Warning(eDLL_T::AUDIO, "[MILES-BANK] LoadBanksListFromFile pattern unresolved\n");

	// ClientSoundMiles_Initialize. Unique on S21 -- wraps CSOM_Initialize.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 83 EC ?? 0F B6 05 ?? ?? ?? ?? BB ?? ?? ?? ?? 84 C0 75 ?? E8 "
		"?? ?? ?? ?? 65 48 8B 04 25 ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? 48 8B 08 48 63 14 19 "
		"48 8D 04 11 41 C6 04 00 ?? 8D 42 ?? 89 04 19 0F B6 05 ?? ?? ?? ?? BF ?? ?? ?? ?? "
		"84 C0 75 ?? E8 ?? ?? ?? ?? 65 48 8B 04 25 ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? 48 8B 08 "
		"48 63 14 39 48 8D 04 11 41 C6 04 00 ?? 8D 42 ?? 89 04 39 E8")
		.GetPtr(v_ClientSoundMiles_Initialize);

	if (!v_ClientSoundMiles_Initialize)
		Warning(eDLL_T::AUDIO, "[MILES-BANK] ClientSoundMiles_Initialize pattern unresolved\n");
}

void VMilesBankListS21::GetVar(void) const
{
	// CSOM copies miles_language from this latch, not the cvar.
	// `mov rbx, cs:latch` / `mov r10d, 0FFFFh` -- unique on S21.
	const CMemory latchLoad = Module_FindPattern(g_GameDll,
		"48 8B 1D ?? ?? ?? ?? 41 BA FF FF 00 00");
	if (latchLoad)
		s_ppszMilesLanguageLatch = latchLoad.ResolveRelativeAddress(3, 7).RCast<const char**>();
	else
		Warning(eDLL_T::AUDIO, "[MILES-BANK] miles_language latch pattern unresolved\n");
}

void VMilesBankListS21::Detour(const bool bAttach) const
{
	if (v_MilesShared_LoadBanksListFromFile)
		DetourSetup(&v_MilesShared_LoadBanksListFromFile, &Hook_MilesShared_LoadBanksListFromFile, bAttach);
	if (v_ClientSoundMiles_Initialize)
		DetourSetup(&v_ClientSoundMiles_Initialize, &Hook_ClientSoundMiles_Initialize, bAttach);
}
