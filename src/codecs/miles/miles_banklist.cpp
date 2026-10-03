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
#include "tier1/utlstring.h"
#include "tier1/utlvector.h"
#include "pluginsystem/modsystem.h"

#include <Windows.h>
#include <mutex>
#include <vector>

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
static constexpr int MPRJ_BUS_COUNT_OFF = 0xE8;   // JRPC CompiledSettings.BusCount
// custom.mbnk routes legend VO/foley onto buses appended past the stock 669;
// an older project resolves those ids to nothing and the mixer traps.
static constexpr unsigned int MPRJ_MIN_BUSES_FOR_CUSTOM = 687;
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

static unsigned int MilesBankDisk_ProjectBusCount(void)
{
	unsigned char header[MPRJ_BUS_COUNT_OFF + 4] = {};

	if (!MilesBankDisk_ReadHeader(MILES_PROJECT_PATH, header, sizeof(header)))
		return 0;

	if (memcmp(header, "JRPC", 4) != 0)
		return 0;

	return *reinterpret_cast<const unsigned int*>(header + MPRJ_BUS_COUNT_OFF);
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
	{
		if (MilesBankDisk_EnglishStubIsValid(path, nBuildTag))
			return false;

		Warning(eDLL_T::AUDIO,
			"[MILES-BANK] %s oversized without valid RTSC header -- refusing custom\n",
			path);
		return true;
	}

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

//-----------------------------------------------------------------------------
// Per-mod bank: one enabled mod may ship audio/ship/<ns>__*.mbnk, authored
// BankIndex 2, and have it appended after custom.
//
// The bank file, its english stub and its stream resolve through the async
// mod fallback, so no path work is needed there. Two things do need the SDK:
// the bank-list append here, and the project the bank loads through. A bank
// loads through audio.mprj BankEntries[BankIndex], and the shipped project
// declares only general/custom; a third bank would index past the table (or
// past the loaded_banks array Miles sized from BankCount). The SDK serves a
// patched project image with BankCount 3 and the mod bank's digest in
// entries[2]. audio.mprj itself is never written.
//-----------------------------------------------------------------------------
static constexpr unsigned int MOD_BANK_INDEX = 2;
static const char* const MILES_MOD_AUDIO_SUBDIR = "audio/ship";
static const char* const MILES_MOD_BANK_EXT = ".mbnk";
static const char* const MILES_PROJECT_SIDECAR = "audio/ship/audio.mprj.modcache";

static constexpr unsigned int MBNK_VERSION_OFF = 0x04;
static constexpr unsigned int MBNK_FILESIZE_OFF = 0x08;
static constexpr unsigned int MBNK_MEOW_OFF = 0x14;
static constexpr unsigned int MBNK_NAMEPTR_OFF = 0x18;
static constexpr unsigned int MBNK_HEADER_SIZE = 0xC0;

static constexpr unsigned int MPRJ_VERSION_OFF = 0x04;
static constexpr unsigned int MPRJ_FILESIZE_OFF = 0x08;
static constexpr unsigned int MPRJ_MEOW_OFF = 0x0C;
static constexpr unsigned int MPRJ_STRINGS_OFF = 0x18;
static constexpr unsigned int MPRJ_BANKENTRIES_OFF = 0x68;
static constexpr unsigned int MPRJ_BANKENTRY_SIZE = 8;
static constexpr unsigned int MPRJ_EXPECTED_VERSION = 42;

static constexpr unsigned long long MODBANK_FILE_CAP = 64ull * 1024ull * 1024ull;

static std::mutex s_modBankMtx;
static bool s_modBankResolved = false;
static bool s_modBankActive = false;
static char s_modBankName[64] = {};
static char s_modBankPath[MAX_PATH] = {};

static unsigned int ModBank_U32(const unsigned char* const pBuf, const unsigned int nOff)
{
	unsigned int nVal = 0;
	memcpy(&nVal, pBuf + nOff, sizeof(nVal));
	return nVal;
}

static unsigned long long ModBank_U64(const unsigned char* const pBuf, const unsigned int nOff)
{
	unsigned long long nVal = 0;
	memcpy(&nVal, pBuf + nOff, sizeof(nVal));
	return nVal;
}

static bool ModBank_FileSize(const char* const pszPath, unsigned long long* const pSize)
{
	WIN32_FILE_ATTRIBUTE_DATA fad = {};
	if (GetFileAttributesExA(pszPath, GetFileExInfoStandard, &fad) == FALSE)
		return false;

	ULARGE_INTEGER sz = {};
	sz.LowPart = fad.nFileSizeLow;
	sz.HighPart = fad.nFileSizeHigh;
	*pSize = sz.QuadPart;
	return true;
}

static bool ModBank_ReadFile(const char* const pszPath, std::vector<unsigned char>& out)
{
	out.clear();

	unsigned long long nSize = 0;
	if (!ModBank_FileSize(pszPath, &nSize) || !nSize || nSize > MODBANK_FILE_CAP)
		return false;

	const HANDLE hFile = CreateFileA(pszPath, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	out.resize(static_cast<size_t>(nSize));
	DWORD nRead = 0;
	const bool bOk = ReadFile(hFile, out.data(), static_cast<DWORD>(nSize), &nRead, NULL) != FALSE
		&& nRead == nSize;
	CloseHandle(hFile);

	if (!bOk)
		out.clear();
	return bOk;
}

static bool ModBank_WriteFile(const char* const pszPath, const std::vector<unsigned char>& data)
{
	if (data.empty() || data.size() > MODBANK_FILE_CAP)
		return false;

	const HANDLE hFile = CreateFileA(pszPath, GENERIC_WRITE, FILE_SHARE_READ,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	DWORD nWritten = 0;
	const bool bOk = WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &nWritten, NULL) != FALSE
		&& nWritten == data.size();
	CloseHandle(hFile);
	return bOk;
}

// True when the bank leaf carries the mod's namespace ("<ns>__*.mbnk").
static bool ModBank_LeafIsOwned(const char* const pszLeaf, const char* const pszNs)
{
	if (!pszLeaf || !pszNs || !pszNs[0])
		return false;

	const size_t nNs = strlen(pszNs);
	const size_t nLeaf = strlen(pszLeaf);
	const size_t nExt = strlen(MILES_MOD_BANK_EXT);
	if (nLeaf <= nNs + 2 + nExt)
		return false;

	if (_strnicmp(pszLeaf, pszNs, nNs) != 0 || pszLeaf[nNs] != '_' || pszLeaf[nNs + 1] != '_')
		return false;

	return _stricmp(pszLeaf + nLeaf - nExt, MILES_MOD_BANK_EXT) == 0;
}

static bool ModBank_ReadBankU32(const char* const pszPath, const unsigned int nOff, unsigned int* const pVal)
{
	unsigned char buf[MBNK_HEADER_SIZE] = {};
	if (!MilesBankDisk_ReadHeader(pszPath, buf, sizeof(buf)) || nOff + 4 > sizeof(buf))
		return false;

	*pVal = ModBank_U32(buf, nOff);
	return true;
}

// Live chain base: custom.mbnk must exist and parse, since the mod bank is
// defined as appending after it.
static bool ModBank_CustomField(const unsigned int nOff, unsigned int* const pVal)
{
	return ModBank_ReadBankU32(MILES_CUSTOM_BANK_PATH, nOff, pVal);
}

// The root stub is authoritative (Miles opens base first); a valid stub the
// mod ships is accepted so a read-only install with a complete mod still loads.
static bool ModBank_EnsureEnglishStub(const char* const pszBank, const char* const pszModDir,
	const unsigned int nBuildTag)
{
	char rootStub[MAX_PATH] = {};
	V_snprintf(rootStub, sizeof(rootStub), "%s/%s_english.mstr", MILES_MOD_AUDIO_SUBDIR, pszBank);

	if (MilesBankDisk_EnglishStubIsValid(rootStub, nBuildTag))
		return true;

	if (pszModDir && pszModDir[0])
	{
		char modStub[MAX_PATH] = {};
		V_snprintf(modStub, sizeof(modStub), "%s%s/%s_english.mstr",
			pszModDir, MILES_MOD_AUDIO_SUBDIR, pszBank);
		if (MilesBankDisk_EnglishStubIsValid(modStub, nBuildTag))
		{
			Msg(eDLL_T::AUDIO, "[MILES-BANK] %s stub served from mod (install root is read-only or bare)\n",
				pszBank);
			return true;
		}
	}

	Warning(eDLL_T::AUDIO, "[MILES-BANK] %s missing -- writing the 32-byte english stub\n", rootStub);
	if (MilesBankDisk_WriteEnglishStub(rootStub, nBuildTag)
		&& MilesBankDisk_EnglishStubIsValid(rootStub, nBuildTag))
	{
		Msg(eDLL_T::AUDIO, "[MILES-BANK] wrote %s (32-byte english stub, build %08x)\n",
			rootStub, nBuildTag);
		return true;
	}

	Warning(eDLL_T::AUDIO, "[MILES-BANK] could not write %s -- refusing mod bank '%s'\n",
		rootStub, pszBank);
	return false;
}

static bool ModBank_StreamExists(const char* const pszBank, const char* const pszModDir)
{
	char rootStream[MAX_PATH] = {};
	V_snprintf(rootStream, sizeof(rootStream), "%s/%s_stream.mstr", MILES_MOD_AUDIO_SUBDIR, pszBank);

	unsigned long long nSize = 0;
	if (ModBank_FileSize(rootStream, &nSize))
		return true;

	if (pszModDir && pszModDir[0])
	{
		char modStream[MAX_PATH] = {};
		V_snprintf(modStream, sizeof(modStream), "%s%s/%s_stream.mstr",
			pszModDir, MILES_MOD_AUDIO_SUBDIR, pszBank);
		if (ModBank_FileSize(modStream, &nSize))
			return true;
	}

	Warning(eDLL_T::AUDIO, "[MILES-BANK] %s is missing -- refusing mod bank (Miles fails closed without it)\n",
		rootStream);
	return false;
}

// Patched project image: BankCount 3, entries table regrown with the mod
// bank's digest in entries[2]. Offsets below are file offsets; the loader
// rebases them the same way it rebases the stock table.
static bool ModBank_BuildProject(const std::vector<unsigned char>& disk, const unsigned int nBankMeow,
	const char* const pszBank, std::vector<unsigned char>& out)
{
	out.clear();
	if (disk.size() < 0x100 || memcmp(disk.data(), "JRPC", 4) != 0
		|| ModBank_U32(disk.data(), MPRJ_VERSION_OFF) != MPRJ_EXPECTED_VERSION)
	{
		return false;
	}

	const unsigned int nFileSize = ModBank_U32(disk.data(), MPRJ_FILESIZE_OFF);
	const unsigned int nMeow = ModBank_U32(disk.data(), MPRJ_MEOW_OFF);
	const unsigned long long nStrings = ModBank_U64(disk.data(), MPRJ_STRINGS_OFF);
	const unsigned long long nEntries = ModBank_U64(disk.data(), MPRJ_BANKENTRIES_OFF);
	if (disk[MPRJ_BANK_COUNT_OFF] != 2 || nFileSize < 0x100 || nFileSize > disk.size()
		|| nStrings > nFileSize || nEntries + 2 * MPRJ_BANKENTRY_SIZE > nFileSize)
	{
		return false;
	}

	const size_t nNameLen = strlen(pszBank);
	if (!nNameLen || nNameLen > 48)
		return false;

	out.assign(disk.begin(), disk.begin() + nFileSize);
	while (out.size() & 15)
		out.push_back(0);

	const unsigned long long nTable = out.size();
	out.insert(out.end(), disk.begin() + nEntries, disk.begin() + nEntries + 2 * MPRJ_BANKENTRY_SIZE);

	const unsigned long long nNameOff = nTable + 3 * MPRJ_BANKENTRY_SIZE;
	if (nNameOff <= nStrings || nNameOff - nStrings > 0xFFFFFFFFull)
		return false;

	const unsigned int nNameRel = static_cast<unsigned int>(nNameOff - nStrings);
	unsigned char entry[MPRJ_BANKENTRY_SIZE] = {};
	memcpy(entry, &nNameRel, sizeof(nNameRel));
	memcpy(entry + 4, &nBankMeow, sizeof(nBankMeow));
	out.insert(out.end(), entry, entry + sizeof(entry));
	out.insert(out.end(), pszBank, pszBank + nNameLen + 1);
	while (out.size() & 15)
		out.push_back(0);

	unsigned char* const pOut = out.data();
	const unsigned int nNewSize = static_cast<unsigned int>(out.size());
	memcpy(pOut + MPRJ_BANKENTRIES_OFF, &nTable, sizeof(nTable));
	pOut[MPRJ_BANK_COUNT_OFF] = 3;
	memcpy(pOut + MPRJ_FILESIZE_OFF, &nNewSize, sizeof(nNewSize));

	char trailer[64] = {};
	V_snprintf(trailer, sizeof(trailer), "SZ %u HASH %08x END", nNewSize, nMeow);
	out.insert(out.end(), trailer, trailer + strlen(trailer));
	return true;
}

static bool ModBank_VerifyProjectBytes(const std::vector<unsigned char>& img,
	const unsigned int nBankMeow, const unsigned int nBusCount,
	const unsigned int nDigest0, const unsigned int nDigest1)
{
	if (img.size() < 0x100 || memcmp(img.data(), "JRPC", 4) != 0
		|| ModBank_U32(img.data(), MPRJ_VERSION_OFF) != MPRJ_EXPECTED_VERSION)
	{
		return false;
	}

	const unsigned int nFileSize = ModBank_U32(img.data(), MPRJ_FILESIZE_OFF);
	if (img[MPRJ_BANK_COUNT_OFF] != 3 || nFileSize > img.size()
		|| ModBank_U32(img.data(), MPRJ_BUS_COUNT_OFF) != nBusCount)
	{
		return false;
	}

	const unsigned long long nEntries = ModBank_U64(img.data(), MPRJ_BANKENTRIES_OFF);
	if (nEntries + 3 * MPRJ_BANKENTRY_SIZE > nFileSize)
		return false;

	return ModBank_U32(img.data(), static_cast<unsigned int>(nEntries)) == 0
		&& ModBank_U32(img.data(), static_cast<unsigned int>(nEntries) + 4) == nDigest0
		&& ModBank_U32(img.data(), static_cast<unsigned int>(nEntries) + 12) == nDigest1
		&& ModBank_U32(img.data(), static_cast<unsigned int>(nEntries) + 20) == nBankMeow;
}

// An existing sidecar is reused only when it still matches the disk project
// and the bank; anything else rebuilds.
static bool ModBank_SidecarIsCurrent(const unsigned int nBankMeow, const unsigned int nBusCount,
	const unsigned int nDigest0, const unsigned int nDigest1)
{
	std::vector<unsigned char> img;
	return ModBank_ReadFile(MILES_PROJECT_SIDECAR, img)
		&& ModBank_VerifyProjectBytes(img, nBankMeow, nBusCount, nDigest0, nDigest1);
}

static bool ModBank_BuildSidecar(const std::vector<unsigned char>& diskProject,
	const unsigned int nBankMeow, const char* const pszBank,
	const unsigned int nBusCount, const unsigned int nDigest0, const unsigned int nDigest1)
{
	if (ModBank_SidecarIsCurrent(nBankMeow, nBusCount, nDigest0, nDigest1))
		return true;

	std::vector<unsigned char> img;
	if (!ModBank_BuildProject(diskProject, nBankMeow, pszBank, img)
		|| !ModBank_VerifyProjectBytes(img, nBankMeow, nBusCount, nDigest0, nDigest1)
		|| !ModBank_WriteFile(MILES_PROJECT_SIDECAR, img))
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] project sidecar build failed -- refusing mod bank '%s'\n",
			pszBank);
		return false;
	}

	Msg(eDLL_T::AUDIO, "[MILES-BANK] project sidecar ready (%s, BankCount 2->3, entries[2] %08x)\n",
		MILES_PROJECT_SIDECAR, nBankMeow);
	return true;
}

static constexpr int MODBANK_MAX_CANDIDATES = 8;

static bool ModBank_ValidateAndBuild(char* const pszOutName, const size_t nNameCap,
	char* const pszOutPath, const size_t nPathCap)
{
	if (!ModSystem()->IsEnabled())
		return false;

	// CSOM formats "<bank>_<miles_language>.mstr" for every bank; only the
	// english stub exists, so any other language would fail the mod bank
	// closed inside Miles.
	const char* const pszLang = MilesBankDisk_Language();
	if (!pszLang[0] || V_stricmp(pszLang, MILES_LANGUAGE_FORCE) != 0)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] refusing mod bank (miles_language='%s') -- English banks only\n",
			pszLang[0] ? pszLang : "");
		return false;
	}

	struct Candidate_t
	{
		char modDir[MAX_PATH];
		char leaf[MAX_PATH];
	};
	Candidate_t found[MODBANK_MAX_CANDIDATES];
	int nFound = 0;

	ModSystem()->LockModList();
	FOR_EACH_VEC(ModSystem()->GetResolvedModList(), i)
	{
		const CModSystem::ModInstance_t* const mod = ModSystem()->GetResolvedModList()[i];
		if (!mod || !mod->IsEnabled())
			continue;

		CUtlVector<CUtlString> leaves;
		if (!ModSystem_ListModFiles(mod, MILES_MOD_AUDIO_SUBDIR, MILES_MOD_BANK_EXT, leaves))
			continue;

		FOR_EACH_VEC(leaves, nLeaf)
		{
			const char* const pszLeaf = leaves[nLeaf].String();
			if (!ModBank_LeafIsOwned(pszLeaf, mod->nameSpace.String()))
				continue;

			if (nFound < MODBANK_MAX_CANDIDATES)
			{
				V_strncpy(found[nFound].modDir, mod->GetBasePath().String(), sizeof(found[nFound].modDir));
				V_strncpy(found[nFound].leaf, pszLeaf, sizeof(found[nFound].leaf));
			}
			++nFound;
		}
	}
	ModSystem()->UnlockModList();

	if (!nFound)
	{
		Msg(eDLL_T::AUDIO, "[MILES-BANK] no mod bank -- list ends after custom\n");
		return false;
	}

	if (nFound > 1)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] %d mod banks found, at most one loads:\n", nFound);
		for (int n = 0; n < MODBANK_MAX_CANDIDATES && n < nFound; ++n)
			Warning(eDLL_T::AUDIO, "[MILES-BANK]   %s%s\n", found[n].modDir, found[n].leaf);
		return false;
	}

	const char* const pszLeaf = found[0].leaf;
	const size_t nLeafLen = strlen(pszLeaf);
	if (nLeafLen <= strlen(MILES_MOD_BANK_EXT) || nLeafLen >= nNameCap + strlen(MILES_MOD_BANK_EXT))
		return false;

	char szBank[64] = {};
	memcpy(szBank, pszLeaf, nLeafLen - strlen(MILES_MOD_BANK_EXT));

	char rootBank[MAX_PATH] = {};
	V_snprintf(rootBank, sizeof(rootBank), "%s/%s", MILES_MOD_AUDIO_SUBDIR, pszLeaf);
	char modBank[MAX_PATH] = {};
	V_snprintf(modBank, sizeof(modBank), "%s%s/%s", found[0].modDir, MILES_MOD_AUDIO_SUBDIR, pszLeaf);

	unsigned long long nRootSize = 0;
	const char* const pszBankPath = ModBank_FileSize(rootBank, &nRootSize) ? rootBank : modBank;
	if (pszBankPath == modBank)
		Msg(eDLL_T::AUDIO, "[MILES-BANK] mod bank '%s' served from %s\n", szBank, modBank);

	unsigned int nCustomVersion = 0, nCustomTag = 0;
	if (!ModBank_CustomField(MBNK_VERSION_OFF, &nCustomVersion)
		|| !ModBank_CustomField(MBNK_BUILDTAG_OFF, &nCustomTag))
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] custom.mbnk unreadable -- refusing mod bank '%s'\n", szBank);
		return false;
	}

	unsigned char header[MBNK_HEADER_SIZE] = {};
	if (!MilesBankDisk_ReadHeader(pszBankPath, header, sizeof(header))
		|| memcmp(header, "KNBC", 4) != 0)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] '%s' is not a KNBC bank -- refusing\n", pszBankPath);
		return false;
	}

	const unsigned int nVersion = ModBank_U32(header, MBNK_VERSION_OFF);
	const unsigned int nFileSize = ModBank_U32(header, MBNK_FILESIZE_OFF);
	const unsigned int nBuildTag = ModBank_U32(header, MBNK_BUILDTAG_OFF);
	const unsigned int nMeow = ModBank_U32(header, MBNK_MEOW_OFF);

	if (nVersion != nCustomVersion)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] '%s' is KNBC v%u, live banks are v%u -- rebuild it for this build\n",
			pszBankPath, nVersion, nCustomVersion);
		return false;
	}

	if (header[MBNK_BANK_INDEX_OFF] != MOD_BANK_INDEX)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] '%s' has BankIndex %u, mod banks are authored %u -- rebuild it\n",
			pszBankPath, header[MBNK_BANK_INDEX_OFF], MOD_BANK_INDEX);
		return false;
	}

	if (nBuildTag != nCustomTag)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] '%s' build %08x does not match live %08x -- rebuild it\n",
			pszBankPath, nBuildTag, nCustomTag);
		return false;
	}

	if (!nMeow)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] '%s' has a zero digest -- rebuild it\n", pszBankPath);
		return false;
	}

	unsigned long long nActual = 0;
	if (!ModBank_FileSize(pszBankPath, &nActual) || nActual < nFileSize || nActual > nFileSize + 64)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] '%s' size field %u does not match %llu on disk -- refusing\n",
			pszBankPath, nFileSize, nActual);
		return false;
	}

	const unsigned long long nNamePtr = ModBank_U64(header, MBNK_NAMEPTR_OFF);
	if (nNamePtr >= nActual)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] '%s' name pointer past end of file -- refusing\n", pszBankPath);
		return false;
	}

	const HANDLE hBank = CreateFileA(pszBankPath, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hBank == INVALID_HANDLE_VALUE)
		return false;

	char szInternal[64] = {};
	DWORD nNameRead = 0;
	LARGE_INTEGER li = {};
	li.QuadPart = static_cast<LONGLONG>(nNamePtr);
	const bool bNameOk = SetFilePointerEx(hBank, li, NULL, FILE_BEGIN)
		&& ReadFile(hBank, szInternal, sizeof(szInternal) - 1, &nNameRead, NULL) != FALSE;
	CloseHandle(hBank);

	if (!bNameOk || V_strcmp(szInternal, szBank) != 0)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] '%s' internal name '%s' does not match '%s' -- author them equal\n",
			pszBankPath, bNameOk ? szInternal : "", szBank);
		return false;
	}

	if (!ModBank_EnsureEnglishStub(szBank, found[0].modDir, nBuildTag)
		|| !ModBank_StreamExists(szBank, found[0].modDir))
	{
		return false;
	}

	std::vector<unsigned char> diskProject;
	if (!ModBank_ReadFile(MILES_PROJECT_PATH, diskProject)
		|| diskProject.size() < 0x100 || memcmp(diskProject.data(), "JRPC", 4) != 0
		|| ModBank_U32(diskProject.data(), MPRJ_VERSION_OFF) != MPRJ_EXPECTED_VERSION
		|| diskProject[MPRJ_BANK_COUNT_OFF] != 2)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] %s is not the BankCount=2 v42 project -- refusing mod bank '%s'\n",
			MILES_PROJECT_PATH, szBank);
		return false;
	}

	const unsigned int nBusCount = ModBank_U32(diskProject.data(), MPRJ_BUS_COUNT_OFF);
	if (nBusCount < MPRJ_MIN_BUSES_FOR_CUSTOM)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] %s has %u buses, mod banks need %u -- refusing '%s'\n",
			MILES_PROJECT_PATH, nBusCount, MPRJ_MIN_BUSES_FOR_CUSTOM, szBank);
		return false;
	}

	const unsigned long long nEntries = ModBank_U64(diskProject.data(), MPRJ_BANKENTRIES_OFF);
	const unsigned int nFileSizeP = ModBank_U32(diskProject.data(), MPRJ_FILESIZE_OFF);
	if (nEntries + 2 * MPRJ_BANKENTRY_SIZE > nFileSizeP || nFileSizeP > diskProject.size())
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] %s bank table out of range -- refusing mod bank '%s'\n",
			MILES_PROJECT_PATH, szBank);
		return false;
	}

	unsigned int nGeneralMeow = 0, nCustomMeow = 0;
	if (!ModBank_ReadBankU32("audio/ship/general.mbnk", MBNK_MEOW_OFF, &nGeneralMeow)
		|| !ModBank_ReadBankU32(MILES_CUSTOM_BANK_PATH, MBNK_MEOW_OFF, &nCustomMeow))
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] stock banks unreadable -- refusing mod bank '%s'\n", szBank);
		return false;
	}

	const unsigned int nDigest0 = ModBank_U32(diskProject.data(), static_cast<unsigned int>(nEntries) + 4);
	const unsigned int nDigest1 = ModBank_U32(diskProject.data(), static_cast<unsigned int>(nEntries) + 12);
	if (nDigest0 != nGeneralMeow || nDigest1 != nCustomMeow)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] %s digests %08x/%08x do not match live banks %08x/%08x -- refusing '%s'\n",
			MILES_PROJECT_PATH, nDigest0, nDigest1, nGeneralMeow, nCustomMeow, szBank);
		return false;
	}

	if (!ModBank_BuildSidecar(diskProject, nMeow, szBank, nBusCount, nGeneralMeow, nCustomMeow))
		return false;

	V_strncpy(pszOutName, szBank, nNameCap);
	V_strncpy(pszOutPath, pszBankPath, nPathCap);
	Msg(eDLL_T::AUDIO, "[MILES-BANK] mod bank '%s' ready (index %u, build %08x)\n",
		szBank, MOD_BANK_INDEX, nBuildTag);
	return true;
}

bool MilesBankDisk_ResolveModBank(void)
{
	std::lock_guard<std::mutex> lk(s_modBankMtx);
	if (s_modBankResolved)
		return s_modBankActive;

	s_modBankResolved = true;

	char szName[sizeof(s_modBankName)] = {};
	char szPath[sizeof(s_modBankPath)] = {};
	s_modBankActive = ModBank_ValidateAndBuild(szName, sizeof(szName), szPath, sizeof(szPath));
	if (s_modBankActive)
	{
		V_strncpy(s_modBankName, szName, sizeof(s_modBankName));
		V_strncpy(s_modBankPath, szPath, sizeof(s_modBankPath));
	}
	return s_modBankActive;
}

const char* MilesBankDisk_ModBankName(void)
{
	std::lock_guard<std::mutex> lk(s_modBankMtx);
	return s_modBankActive ? s_modBankName : nullptr;
}

const char* MilesBankDisk_ModProjectPath(void)
{
	if (!MilesBankDisk_ResolveModBank())
		return nullptr;

	std::lock_guard<std::mutex> lk(s_modBankMtx);
	return s_modBankActive ? MILES_PROJECT_SIDECAR : nullptr;
}

bool MilesBankDisk_ShouldServeProject(const char* const pszPath)
{
	if (!pszPath || !pszPath[0] || V_IsAbsolutePath(pszPath))
		return false;

	char rel[MAX_PATH] = {};
	V_strncpy(rel, pszPath, sizeof(rel));
	for (char* q = rel; *q; ++q)
	{
		if (*q == '\\')
			*q = '/';
	}

	return V_stricmp(rel, MILES_PROJECT_PATH) == 0;
}

static void MilesBankDisk_AppendModBank(char* const bankList)
{
	unsigned int count = MilesBankDisk_ListCount(bankList);

	if (!MilesBankDisk_ResolveModBank())
		return;

	const char* const pszModBank = MilesBankDisk_ModBankName();
	if (!pszModBank || !pszModBank[0] || V_strlen(pszModBank) >= BANK_NAME_STRIDE)
		return;

	for (unsigned int i = 0; i < count; i++)
	{
		if (V_stricmp(bankList + (static_cast<size_t>(i) * BANK_NAME_STRIDE), pszModBank) == 0)
			return;
	}

	if (count >= BANK_CAP)
	{
		Warning(eDLL_T::AUDIO, "[MILES-BANK] bankList full (%u) -- cannot append mod bank '%s'\n",
			count, pszModBank);
		return;
	}

	char* const slot = bankList + (static_cast<size_t>(count) * BANK_NAME_STRIDE);
	V_strncpy(slot, pszModBank, BANK_NAME_STRIDE);
	*reinterpret_cast<unsigned int*>(bankList + BANK_COUNT_OFF) = count + 1;
	Msg(eDLL_T::AUDIO, "[MILES-BANK] appended mod bank '%s' count=%u->%u\n", pszModBank, count, count + 1);
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
		MilesBankDisk_AppendModBank(bankList);
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

	const unsigned int nBuses = MilesBankDisk_ProjectBusCount();
	if (nBuses < MPRJ_MIN_BUSES_FOR_CUSTOM)
	{
		Warning(eDLL_T::AUDIO,
			"[MILES-BANK] audio.mprj has %u buses, custom needs %u -- refusing to append "
			"custom; ship custom.mbnk and audio.mprj together\n",
			nBuses, MPRJ_MIN_BUSES_FOR_CUSTOM);
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
	MilesBankDisk_AppendModBank(bankList);
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
