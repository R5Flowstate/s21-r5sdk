//=============================================================================//
//
// Purpose: Raise the hardcoded 512-entry camo-skins cap. See header for design.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "public/tier0/tier0_iface.h"
#include "camo_skins_cap.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//-----------------------------------------------------------------------------
// Range [0x200, 0x10000]; outside that range the patch is skipped.
//-----------------------------------------------------------------------------
static ConVar sdk_camo_skins_cap("sdk_camo_skins_cap", "4096",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Maximum camo skins per pad rpak (default engine cap is 512). "
	"Range [512, 65536]; outside that range disables the patch.");

//-----------------------------------------------------------------------------
// Each site: +0x04 cmp imm32 cap, +0x13 mov r9d error-arg. Stock 0x200.
// Module_FindPattern needs a string literal at the call site.
//-----------------------------------------------------------------------------
static constexpr size_t kCapImmOff = 4;
static constexpr size_t kArgImmOff = 19;

//-----------------------------------------------------------------------------
// Write a 4-byte imm32 after validating current == expected. Caller owns
// all-or-nothing policy.
//-----------------------------------------------------------------------------
static bool WriteImm32(uintptr_t addr, uint32_t newVal, uint32_t expected,
	const char* siteLabel, const char* role)
{
	const uint32_t current = *reinterpret_cast<const uint32_t*>(addr);
	if (current != expected)
	{
		Warning(eDLL_T::ENGINE,
			"[CamoSkinsCap] %s %s mismatch @ 0x%p: expected 0x%X got 0x%X\n",
			siteLabel, role, reinterpret_cast<void*>(addr), expected, current);
		return false;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(addr), 4,
		PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[CamoSkinsCap] %s %s VirtualProtect failed @ 0x%p (gle=%lu)\n",
			siteLabel, role, reinterpret_cast<void*>(addr), GetLastError());
		return false;
	}

	memcpy(reinterpret_cast<void*>(addr), &newVal, 4);
	VirtualProtect(reinterpret_cast<void*>(addr), 4, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(),
		reinterpret_cast<void*>(addr), 4);
	return true;
}

static bool RestoreImm32(uintptr_t addr, uint32_t stockVal)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(addr), 4,
		PAGE_EXECUTE_READWRITE, &oldProt))
		return false;
	memcpy(reinterpret_cast<void*>(addr), &stockVal, 4);
	VirtualProtect(reinterpret_cast<void*>(addr), 4, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(),
		reinterpret_cast<void*>(addr), 4);
	return true;
}

void VCamoSkinsCap::Detour(const bool bAttach) const
{
	if (!bAttach) return;

	const int target = sdk_camo_skins_cap.GetInt();
	if (target < 0x200 || target > 0x10000)
	{
		Warning(eDLL_T::ENGINE,
			"[CamoSkinsCap] cap %d (0x%X) out of range [0x200, 0x10000]; "
			"skipping patch -- engine retains stock 512-entry cap.\n",
			target, target);
		return;
	}

	const uint32_t newCap = static_cast<uint32_t>(target);
	constexpr uint32_t kStock = 0x200u;

	CMemory hit1 = Module_FindPattern(g_GameDll,
		"48 81 78 10 00 02 00 00 48 89 05 ?? ?? ?? ?? 76 1A 41 B9 00 02 00 00");
	CMemory hit2 = Module_FindPattern(g_GameDll,
		"48 81 7F 10 00 02 00 00 48 89 3D ?? ?? ?? ?? 76 1A 41 B9 00 02 00 00");

	if (!hit1 || !hit2)
	{
		Warning(eDLL_T::ENGINE,
			"[CamoSkinsCap] expansion OFF -- pattern miss (registrar=%d renderer=%d); "
			"stock 512-entry cap retained\n",
			hit1 ? 1 : 0, hit2 ? 1 : 0);
		return;
	}

	const uintptr_t b1 = hit1.GetPtr();
	const uintptr_t b2 = hit2.GetPtr();
	const uintptr_t slots[4] = {
		b1 + kCapImmOff, b1 + kArgImmOff,
		b2 + kCapImmOff, b2 + kArgImmOff,
	};
	const char* labels[4] = {
		"registrar cmp", "registrar arg",
		"renderer cmp", "renderer arg",
	};

	// Preflight stock imm32 at every slot.
	for (int i = 0; i < 4; ++i)
	{
		const uint32_t cur = *reinterpret_cast<const uint32_t*>(slots[i]);
		if (cur != kStock)
		{
			Warning(eDLL_T::ENGINE,
				"[CamoSkinsCap] expansion OFF -- %s preflight got 0x%X want 0x%X\n",
				labels[i], cur, kStock);
			return;
		}
	}

	int applied = 0;
	for (int i = 0; i < 4; ++i)
	{
		if (!WriteImm32(slots[i], newCap, kStock, labels[i], "imm32"))
			break;
		++applied;
	}

	if (applied != 4)
	{
		for (int i = 0; i < applied; ++i)
			RestoreImm32(slots[i], kStock);
		Warning(eDLL_T::ENGINE,
			"[CamoSkinsCap] expansion OFF -- %d/4 slots; rolled back, stock 512 retained\n",
			applied);
		return;
	}

	Msg(eDLL_T::ENGINE,
		"[CamoSkinsCap] ACTIVE: 4/4 imm32 slots (target=%u)\n", newCap);
}
