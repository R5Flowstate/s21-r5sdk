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

static constexpr int MBNK_BANK_INDEX_OFF = 0xA0;  // KNBC CompiledBank.BankIndex
static constexpr int MPRJ_BANK_COUNT_OFF = 0xF2;  // JRPC CompiledSettings.BankCount

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

static __int64 __fastcall Hook_MilesShared_LoadBanksListFromFile(char* bankList, __int64 a2, __int64 a3, __int64 a4)
{
	const __int64 result = v_MilesShared_LoadBanksListFromFile(bankList, a2, a3, a4);
	if (!bankList)
		return result;

	unsigned int count = *reinterpret_cast<unsigned int*>(bankList + BANK_COUNT_OFF);
	if (count > BANK_CAP)
		count = BANK_CAP;

	bool hasCustom = false;
	for (unsigned int i = 0; i < count; i++)
	{
		const char* name = bankList + (static_cast<size_t>(i) * BANK_NAME_STRIDE);
		Msg(eDLL_T::AUDIO, "[MILES-BANK] list[%u]='%s'\n", i, name);
		if (name[0] && V_stricmp(name, "custom") == 0)
			hasCustom = true;
	}

	if (!miles_bank_disk.GetBool())
		return result;

	if (hasCustom)
	{
		Msg(eDLL_T::AUDIO, "[MILES-BANK] list already has custom (count=%u)\n", count);
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
}

void VMilesBankListS21::Detour(const bool bAttach) const
{
	if (v_MilesShared_LoadBanksListFromFile)
		DetourSetup(&v_MilesShared_LoadBanksListFromFile, &Hook_MilesShared_LoadBanksListFromFile, bAttach);
}
