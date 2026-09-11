//=============================================================================//
//
// Purpose: WeaponNames dispatcher binds RegisterWeaponInfo (disp32 LSB 0xF4->0xB4).
//
//=============================================================================//

#include "core/stdafx.h"
#include "weapon_precache_redirect.h"
#include "common/global.h"
#include "tier0/dbg.h"
#include "thirdparty/detours/include/detours.h"

#include <windows.h>
#include <cstring>

namespace
{
    // Patch 1 (disabled): CALL [rax+0x688] then MOV rcx, [rip+disp32].
    constexpr const char* kAnchorPattern =
        "FF 90 88 06 00 00 48 8B 0D B0 04 D5 06 48 8B D6";

    // Patch 1 (disabled): disp32 LSB 0xB0 -> 0xA0.
    constexpr size_t kDisp32LowByteOffset = 9;
    constexpr uint8_t kExpectedOldByte    = 0xB0;
    constexpr uint8_t kPatchedNewByte     = 0xA0;

    // WeaponNames lea r8 callback is 0x40 below ScriptNames (0xF4 -> 0xB4).
    constexpr const char* kDispatcherAnchorPattern =
        "4C 8D 05 F4 FA FF FF 33 D2 48 8B C8 48 89 05";

    // Pattern[3] is the disp32 LSB of the lea (the byte we change).
    constexpr size_t kDispatcherDispLowByteOffset = 3;
    constexpr uint8_t kDispatcherExpectedOldByte  = 0xF4;
    constexpr uint8_t kDispatcherPatchedNewByte   = 0xB4;
}

void VWeaponPrecacheRedirectS21::Detour(const bool bAttach) const
{
	if (!bAttach)
	{
		// Code patch is not reverted on detach.
		return;
	}

	// Patch 1 disabled: validator already reads ScriptNames, where weapon_cubemap lives.
	(void)kAnchorPattern;
	(void)kDisp32LowByteOffset;
	(void)kExpectedOldByte;
	(void)kPatchedNewByte;

	Msg(eDLL_T::ENGINE,
		"[wpn-precache-redirect] disp32 redirect DISABLED -- validator "
		"reads ScriptNames natively, where weapon_cubemap lives "
		"(verified via wpn-diag at runtime).\n");

	// WeaponNames branch binds RegisterWeaponInfo.
	CMemory dispAnchor =
		Module_FindPattern(g_GameDll, kDispatcherAnchorPattern);
	if (!dispAnchor)
	{
		Warning(eDLL_T::ENGINE,
			"[wpn-precache-redirect] dispatcher anchor not found; "
			"WeaponNames will keep binding the S3 callback "
			"( no RegisterWeaponInfo).\n");
		return;
	}

	uint8_t* dispatcherByte = reinterpret_cast<uint8_t*>(
		dispAnchor.GetPtr() + kDispatcherDispLowByteOffset);

	if (*dispatcherByte != kDispatcherExpectedOldByte)
	{
		Warning(eDLL_T::ENGINE,
			"[wpn-precache-redirect] dispatcher: expected 0x%02X at %p but "
			"found 0x%02X; skipping patch (binary changed?).\n",
			kDispatcherExpectedOldByte, dispatcherByte, *dispatcherByte);
		return;
	}

	DWORD oldProt2 = 0;
	if (!VirtualProtect(dispatcherByte, 1, PAGE_EXECUTE_READWRITE, &oldProt2))
	{
		Warning(eDLL_T::ENGINE,
			"[wpn-precache-redirect] dispatcher VirtualProtect RW failed "
			"at %p (gle=%lu)\n", dispatcherByte, GetLastError());
		return;
	}

	*dispatcherByte = kDispatcherPatchedNewByte;

	DWORD restoredProt2 = 0;
	VirtualProtect(dispatcherByte, 1, oldProt2, &restoredProt2);
	FlushInstructionCache(GetCurrentProcess(), dispatcherByte, 1);

	Msg(eDLL_T::ENGINE,
		"[wpn-precache-redirect] patched dispatcher byte @ %p"
		"(0x%02X -> 0x%02X): WeaponNames branch now binds"
		"(RegisterWeaponInfo) instead of (S3-era).\n",
		dispatcherByte, kDispatcherExpectedOldByte, kDispatcherPatchedNewByte);
}
