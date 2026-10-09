//=============================================================================//
//
// Purpose: grow the weapon mod-name string table (stock 7168 bytes / 448 names).
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/commandline.h"
#include "weapon_mod_names_ext.h"
#if !defined(CLIENT_DLL)
#include "weapon_script_vars.h"
#endif // !CLIENT_DLL

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(CLIENT_DLL)
#define MODNAMES_DLL eDLL_T::CLIENT
#else
#define MODNAMES_DLL eDLL_T::SERVER
#endif // CLIENT_DLL

// Past the stock size weapon loading aborts with "Hit weapon mod names string data
// limit"; every reader goes through this descriptor, so the grow repoints it.
struct ModNameTable_t
{
	char*    pPool;
	char**   ppEntries;
	uint32_t nPoolBytes;
	uint32_t nMaxEntries;
};

// Name indices are stored as int16 with 0x7FFF meaning "no mod".
static constexpr uint32_t kGrownPoolBytes  = 0x20000;
static constexpr uint32_t kGrownMaxEntries = 0x2000;
static_assert(kGrownMaxEntries < 0x7FFF, "mod index is a 15-bit value");

// The add routine's capacity check: entry ptr against pool base + pool size.
static constexpr char kCapacityCheck[] =
	"48 FF C7 8B 05 ?? ?? ?? ?? 48 2B C6 48 03 05 ?? ?? ?? ?? 48 3B F8 7E ?? 4D 8B";
static constexpr ptrdiff_t kPoolDispOff = 12; // add rax,[rip+disp32] -> ModNameTable_t::pPool

static bool GrowTable(ModNameTable_t* const pTable)
{
	if (!pTable->pPool || !pTable->ppEntries || pTable->nMaxEntries == 0)
	{
		Warning(MODNAMES_DLL, "[MODNAMES] table not initialised (pool %p, entries %p, max %u); left at stock\n",
			pTable->pPool, pTable->ppEntries, pTable->nMaxEntries);
		return false;
	}
	if (pTable->nPoolBytes >= kGrownPoolBytes && pTable->nMaxEntries >= kGrownMaxEntries)
		return true;
	// The game's static init points entry 0 at the pool; before that, a grown copy would
	// keep a null first entry and the first add would write through it.
	if (pTable->ppEntries[0] != pTable->pPool)
	{
		Warning(MODNAMES_DLL, "[MODNAMES] entry 0 (%p) does not point at the pool (%p) yet; table left at stock\n",
			pTable->ppEntries[0], pTable->pPool);
		return false;
	}

	// Never shrink a dimension that is already past the grow.
	const uint32_t nNewBytes = pTable->nPoolBytes > kGrownPoolBytes ? pTable->nPoolBytes : kGrownPoolBytes;
	const uint32_t nNewMax = pTable->nMaxEntries > kGrownMaxEntries ? pTable->nMaxEntries : kGrownMaxEntries;
	char* const pNewPool = static_cast<char*>(calloc(nNewBytes, 1));
	char** const ppNewEntries = static_cast<char**>(calloc(nNewMax, sizeof(char*)));
	if (!pNewPool || !ppNewEntries)
	{
		free(pNewPool);
		free(ppNewEntries);
		Warning(MODNAMES_DLL, "[MODNAMES] allocation failed; table left at stock\n");
		return false;
	}

	char* const pOldPool = pTable->pPool;
	const uint32_t nOldBytes = pTable->nPoolBytes;
	memcpy(pNewPool, pOldPool, nOldBytes);

	// Entry pointers address the pool; the first unused entry marks the next free byte.
	for (uint32_t i = 0; i < pTable->nMaxEntries; i++)
	{
		char* const p = pTable->ppEntries[i];
		if (p >= pOldPool && p <= pOldPool + nOldBytes)
			ppNewEntries[i] = pNewPool + (p - pOldPool);
		else
			ppNewEntries[i] = p;
	}

	Msg(MODNAMES_DLL, "[MODNAMES] weapon mod-name table %u -> %u bytes, %u -> %u names\n",
		nOldBytes, nNewBytes, pTable->nMaxEntries, nNewMax);

	pTable->ppEntries = ppNewEntries;
	pTable->pPool = pNewPool;
	pTable->nMaxEntries = nNewMax;
	pTable->nPoolBytes = nNewBytes;
	return true;
}

static ModNameTable_t* TableFromCheck(uint8_t* const pCheck)
{
	return CMemory(pCheck).Offset(kPoolDispOff).ResolveRelativeAddress(3, 7).RCast<ModNameTable_t*>();
}

void VWeaponModNamesExt::Detour(const bool bAttach) const
{
	if (!bAttach)
		return;

	if (CommandLine()->CheckParm("-sdk_stock_mod_names"))
	{
		Msg(MODNAMES_DLL, "[MODNAMES] -sdk_stock_mod_names: weapon mod-name table left at stock\n");
		return;
	}

#if defined(CLIENT_DLL)
	uint8_t* const pCheck = Module_FindPattern(g_GameDll, kCapacityCheck).RCast<uint8_t*>();
	if (!pCheck)
	{
		Warning(MODNAMES_DLL, "[MODNAMES] capacity check pattern unresolved; table left at stock\n");
		return;
	}
	GrowTable(TableFromCheck(pCheck));
#else
	// The dedi carries the listen-client twin of the table; grow both halves.
	static constexpr size_t kCapacityCheckLen = 26;
	uint8_t* hits[4] = {};
	const int n = WeaponScriptVars_FindAllPatternMatches(kCapacityCheck, kCapacityCheckLen, hits, 4);
	if (n != 2)
	{
		Warning(MODNAMES_DLL, "[MODNAMES] expected 2 capacity checks (client + server half), found %d; table left at stock\n", n);
		return;
	}

	for (int i = 0; i < n; i++)
		GrowTable(TableFromCheck(hits[i]));
#endif // CLIENT_DLL
}
