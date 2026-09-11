//=============================================================================//
//
// Purpose: Raise hardcoded max-entries on SettingsAssets. See header.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/commandline.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "public/tier0/tier0_iface.h"
#include "string_table_grow.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//-----------------------------------------------------------------------------
// Power of two, uint16 wire maxEntries. 0x8000 is the largest legal value.
//-----------------------------------------------------------------------------
static ConVar sdk_settings_assets_table_size("sdk_settings_assets_table_size",
	"32768", FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Maximum entries in the SettingsAssets network string table "
	"(default engine cap is 8192). Range [8192, 32768] -- must be a power "
	"of 2 and fit in uint16 wire encoding.");

// ParticleEffectNames table-only grow 2048->4096. Parallel structure stays 2048.
static ConVar sdk_particle_effect_table_grow("sdk_particle_effect_table_grow",
	"1", FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Grow ParticleEffectNames network string table 2048->4096. 1=grow, 0=stock 2048. Restart required.");

//-----------------------------------------------------------------------------
// modelprecache sizing is owned by weapon_legendary_ext.cpp (table + model_t* array together).
//-----------------------------------------------------------------------------

void VStringTableGrow::Detour(const bool bAttach) const
{
	if (!bAttach) return;

	// modelprecache sizing is in weapon_legendary_ext.cpp.

	const int target = sdk_settings_assets_table_size.GetInt();
	if (target < 0x2000 || target > 0x8000 || (target & (target - 1)) != 0)
	{
		Warning(eDLL_T::ENGINE,
			"[string-table] sdk_settings_assets_table_size %d invalid "
			"(must be a power of 2 in [0x2000, 0x8000]); skipping. Engine "
			"retains stock 8192-entry cap.\n",
			target);
		return;
	}

	// SettingsAssets CreateStringTable: unique LEA disp32 C2 3A 65 00; imm32 at +16.
	CMemory hit = Module_FindPattern(g_GameDll,
		"4C 8D 05 C2 3A 65 00 48 89 05 ?? ?? ?? ?? 41 B9 00 20 00 00");
	if (!hit)
	{
		Warning(eDLL_T::ENGINE,
			"[string-table] SettingsAssets imm32 site not found -- engine "
			"retains stock 8192-entry cap.\n");
		return;
	}

	uint8_t* pImm = reinterpret_cast<uint8_t*>(hit.GetPtr() + 16);
	const uint32_t expected = 0x2000u;
	uint32_t current = 0;
	memcpy(&current, pImm, 4);
	if (current != expected)
	{
		Warning(eDLL_T::ENGINE,
			"[string-table] SettingsAssets imm32 mismatch @ %p "
			"(expected 0x%X got 0x%X) -- not patching.\n",
			pImm, expected, current);
		return;
	}

	const uint32_t newSize = static_cast<uint32_t>(target);
	DWORD oldProt = 0;
	if (!VirtualProtect(pImm, 4, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[string-table] VirtualProtect failed @ %p (gle=%lu)\n",
			pImm, GetLastError());
		return;
	}
	memcpy(pImm, &newSize, 4);
	VirtualProtect(pImm, 4, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), pImm, 4);

	Msg(eDLL_T::ENGINE,
		"[string-table] SettingsAssets max entries patched: 0x%X -> 0x%X "
		"(@ %p)\n", expected, newSize, pImm);

	// ParticleEffectNames 0x800 -> 0x1000. Read +sdk_particle_effect_table_grow from the command line (this Detour runs before launch-cvar apply).
	bool growEnabled = sdk_particle_effect_table_grow.GetBool();
	{
		const char* pszVal = nullptr;
		if (CommandLine() &&
			CommandLine()->CheckParm("+sdk_particle_effect_table_grow", &pszVal) && pszVal)
		{
			growEnabled = (atoi(pszVal) != 0);
		}
	}
	if (!growEnabled)
	{
		Msg(eDLL_T::ENGINE,
			"[string-table] ParticleEffectNames grow DISABLED (sdk_particle_effect_table_grow 0) "
			"-- stock 2048 cap retained.\n");

		// Stock 2048 fatals at boot. Flip table-full helper tail jmp Error (E9 @ +0x1E) to ret.
		CMemory fullHit = Module_FindPattern(g_GameDll,
			"48 8D 0D ?? ?? ?? ?? 4C 8B C6 48 8B 5C 24 30 48 8B 6C 24 38 "
			"48 8B 74 24 40 48 83 C4 20 5F E9");
		if (!fullHit)
		{
			Warning(eDLL_T::ENGINE,
				"[string-table] table-full helper tail-jmp NOT FOUND -- dedi will "
				"still FATAL at 'String table is full'; cannot boot the 2048 A/B.\n");
		}
		else
		{
			uint8_t* pJmp = reinterpret_cast<uint8_t*>(fullHit.GetPtr() + 0x1E);
			if (*pJmp != 0xE9)
			{
				Warning(eDLL_T::ENGINE,
					"[string-table] table-full tail-jmp byte mismatch @ %p "
					"(got 0x%02X, expected 0xE9) -- not patching.\n", pJmp, *pJmp);
			}
			else
			{
				DWORD oldP = 0;
				if (VirtualProtect(pJmp, 1, PAGE_EXECUTE_READWRITE, &oldP))
				{
					*pJmp = 0xC3;
					VirtualProtect(pJmp, 1, oldP, &oldP);
					FlushInstructionCache(GetCurrentProcess(), pJmp, 1);
					Msg(eDLL_T::ENGINE,
						"[string-table] table-full made NON-FATAL (skip-on-full @ %p) "
						"-- dedi boots at stock 2048; effects past 2048 are skipped.\n",
						pJmp);
				}
				else
				{
					Warning(eDLL_T::ENGINE,
						"[string-table] VirtualProtect failed on table-full jmp @ %p "
						"(gle=%lu)\n", pJmp, GetLastError());
				}
			}
		}
	}
	else
	{
		CMemory particleHit = Module_FindPattern(g_GameDll,
			"4C 8D 05 82 3C 65 00 48 89 05 ?? ?? ?? ?? 41 B9 00 08 00 00");
		if (!particleHit)
		{
			Warning(eDLL_T::ENGINE,
				"[string-table] ParticleEffectNames imm32 site not found -- "
				"engine retains stock 2048-entry cap.\n");
		}
		else
		{
			uint8_t* pPartImm = reinterpret_cast<uint8_t*>(particleHit.GetPtr() + 16);
			const uint32_t partExpected = 0x0800u;
			uint32_t partCurrent = 0;
			memcpy(&partCurrent, pPartImm, 4);
			if (partCurrent != partExpected)
			{
				Warning(eDLL_T::ENGINE,
					"[string-table] ParticleEffectNames imm32 mismatch @ %p "
					"(expected 0x%X got 0x%X) -- not patching.\n",
					pPartImm, partExpected, partCurrent);
			}
			else
			{
				const uint32_t newPartSize = 0x1000u;
				DWORD oldPartProt = 0;
				if (!VirtualProtect(pPartImm, 4, PAGE_EXECUTE_READWRITE, &oldPartProt))
				{
					Warning(eDLL_T::ENGINE,
						"[string-table] VirtualProtect failed @ %p (gle=%lu)\n",
						pPartImm, GetLastError());
				}
				else
				{
					memcpy(pPartImm, &newPartSize, 4);
					VirtualProtect(pPartImm, 4, oldPartProt, &oldPartProt);
					FlushInstructionCache(GetCurrentProcess(), pPartImm, 4);
					Msg(eDLL_T::ENGINE,
						"[string-table] ParticleEffectNames max entries patched: "
						"0x%X -> 0x%X (@ %p)\n",
						partExpected, newPartSize, pPartImm);
				}
			}
		}
	}

	// Force stringtable_compress ON: SettingsAssets baselines exceed the 22-bit dataLen ceiling. cmp imm at +27.
	CMemory compHit = Module_FindPattern(g_GameDll,
		"48 8B 05 ?? ?? ?? ?? 83 C3 07 C1 FB 03 48 C7 85 "
		"?? ?? ?? ?? 00 00 08 00 83 78 6C 00 0F 84");
	if (!compHit)
	{
		Warning(eDLL_T::ENGINE,
			"[string-table] stringtable_compress pattern not found -- "
			"compression remains gated on the engine convar.\n");
	}
	else
	{
		uint8_t* pCmpImm = reinterpret_cast<uint8_t*>(compHit.GetPtr() + 27);
		if (*pCmpImm == 0x00)
		{
			DWORD oldProtComp = 0;
			if (VirtualProtect(pCmpImm, 1, PAGE_EXECUTE_READWRITE, &oldProtComp))
			{
				*pCmpImm = 0xFF;
				VirtualProtect(pCmpImm, 1, oldProtComp, &oldProtComp);
				FlushInstructionCache(GetCurrentProcess(), pCmpImm, 1);
				Msg(eDLL_T::ENGINE,
					"[string-table] stringtable_compress forced ON: cmp imm "
					"0x00 -> 0xFF @ %p (compression branch always taken).\n",
					pCmpImm);
			}
		}
		else if (*pCmpImm == 0xFF)
		{
			Msg(eDLL_T::ENGINE,
				"[string-table] stringtable_compress already patched @ %p.\n",
				pCmpImm);
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[string-table] stringtable_compress cmp imm unexpected value "
				"0x%02X @ %p -- not patching.\n", *pCmpImm, pCmpImm);
		}
	}
}
