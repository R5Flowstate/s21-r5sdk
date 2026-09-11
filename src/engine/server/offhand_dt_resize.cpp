//=============================================================================//
//
// Purpose: Resize DT_WeaponInventory.offhandWeapons from 6 to 8.
// Alloc 0x270->0x340, loop bound 6->8, nProps 6->8. Slots 6/7 are side-band.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "offhand_dt_resize.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static bool OffhandDTResize_WriteBytes(void* addr, const void* data, size_t len)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-DT-RESIZE] VirtualProtect failed @ %p (gle=%lu)\n",
			addr, GetLastError());
		return false;
	}
	memcpy(addr, data, len);
	DWORD restored = 0;
	VirtualProtect(addr, len, oldProt, &restored);
	FlushInstructionCache(GetCurrentProcess(), addr, len);
	return true;
}

void VOffhandDTResize::Detour(const bool bAttach) const
{
	if (!bAttach) return;

	// Anchor: mov r9, r15; mov r8d, 6; mov rdx, rbx; mov rcx, rax; call.
	CMemory site = Module_FindPattern(g_GameDll,
		"4D 8B CF 41 B8 06 00 00 00 48 8B D3 48 8B C8 E8");
	if (!site)
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-DT-RESIZE] anchor pattern not found; DT extension "
			"NOT APPLIED. offhandWeapons stays at 6 elements; SDK slots "
			"6/7 keep using the shadow-RPC L2 path. Dedi engine rebuild "
			"may have shifted layout -- re-find anchor.\n");
		return;
	}

	uint8_t* anchor = reinterpret_cast<uint8_t*>(site.GetPtr());

	// Anchor -0x1D1 alloc imm32, -0x31 loop imm8, +5 nProps imm32.
	uint8_t* allocImm = anchor - 0x1D1;
	uint8_t* loopImm  = anchor - 0x31;
	uint8_t* regImm   = anchor + 5;

	// Abort if any site's expected bytes have moved.
	if (memcmp(allocImm, "\x70\x02\x00\x00", 4) != 0)
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-DT-RESIZE] alloc imm32 sig mismatch @ %p "
			"(got %02X %02X %02X %02X, expected 70 02 00 00); aborting\n",
			allocImm, allocImm[0], allocImm[1], allocImm[2], allocImm[3]);
		return;
	}
	if (*loopImm != 0x06)
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-DT-RESIZE] loop imm8 sig mismatch @ %p "
			"(got %02X, expected 06); aborting\n", loopImm, *loopImm);
		return;
	}
	if (memcmp(regImm, "\x06\x00\x00\x00", 4) != 0)
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-DT-RESIZE] register imm32 sig mismatch @ %p "
			"(got %02X %02X %02X %02X, expected 06 00 00 00); aborting\n",
			regImm, regImm[0], regImm[1], regImm[2]);
		return;
	}

	// Apply: alloc 0x270 -> 0x340 (624 -> 832 = 8 templates of 104B each).
	const uint8_t newAlloc[4] = { 0x40, 0x03, 0x00, 0x00 };
	if (!OffhandDTResize_WriteBytes(allocImm, newAlloc, 4))
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-DT-RESIZE] failed to write alloc patch; aborting\n");
		return;
	}

	// Apply: loop count 6 -> 8.
	const uint8_t newLoop = 0x08;
	if (!OffhandDTResize_WriteBytes(loopImm, &newLoop, 1))
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-DT-RESIZE] failed to write loop patch; "
			"alloc was applied but templates won't be filled -- DT "
			"WILL CORRUPT. Reverting alloc.\n");
		const uint8_t revertAlloc[4] = { 0x70, 0x02, 0x00, 0x00 };
		OffhandDTResize_WriteBytes(allocImm, revertAlloc, 4);
		return;
	}

	// Apply: register count 6 -> 8.
	const uint8_t newReg[4] = { 0x08, 0x00, 0x00, 0x00 };
	if (!OffhandDTResize_WriteBytes(regImm, newReg, 4))
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-DT-RESIZE] failed to write register patch; "
			"templates filled to 8 but registration only sees 6 -- "
			"SendTable inconsistent. Reverting alloc + loop.\n");
		const uint8_t revertAlloc[4] = { 0x70, 0x02, 0x00, 0x00 };
		const uint8_t revertLoop = 0x06;
		OffhandDTResize_WriteBytes(allocImm, revertAlloc, 4);
		OffhandDTResize_WriteBytes(loopImm, &revertLoop, 1);
		return;
	}

	Msg(eDLL_T::SERVER,
		"[OFFHAND-DT-RESIZE] PATCHED DT_WeaponInventory.offhandWeapons "
		"6 -> 8 elements (alloc=%p loop=%p reg=%p anchor=%p). Wire now "
		"carries 8 entries; SDK slots 6/7 storage is side-band.\n",
		allocImm, loopImm, regImm, anchor);
}
