//=============================================================================//
//
// Purpose: Grow s_settingsLayoutRuntimeData hash table. See header for design.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/commandline.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "public/tier0/tier0_iface.h"
#include "thirdparty/detours/include/detours.h"
#include "layout_cache_grow.h"
#include "game/shared/heap_canary.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//-----------------------------------------------------------------------------
// Runtime gate. Set to 0 to skip; engine retains stock 64-slot cache and will
// re-fatal on overflow. Restart required after toggling.
//-----------------------------------------------------------------------------
static ConVar sdk_layout_cache_grow("sdk_layout_cache_grow", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Relocate s_settingsLayoutRuntimeData onto a 128-slot DLL-resident array. "
	"Disable + restart to revert to stock 64-slot cache.");

//-----------------------------------------------------------------------------
// 128-slot replacement array. Same per-slot layout as the engine's
// (5 qwords / 40 bytes). Zero-init = "all slots empty"
// which matches engine expectations on a fresh table.
//
// CRITICAL: r14-relative LEA/CMP sites encode the array address as a 32-bit
// signed displacement from r14 (= module base). The DLL's own.data section
// commonly loads several GiB from r5apex_ds.exe and so cannot be reached.
// We allocate the array via Detours' jump-bounds allocator which guarantees
// a region within +-2GiB of the target.
//-----------------------------------------------------------------------------
static constexpr int kNewSlotCount = 128;
static constexpr int kNewMask      = 0x7F;   // kNewSlotCount - 1
static constexpr int kNewLimit     = 0x7F;   // imm8-safe probe limit (127 probes)
static constexpr size_t kCacheBytes = kNewSlotCount * 5 * sizeof(uint64_t);

static uint64_t* s_pNewLayoutCache = nullptr;

//-----------------------------------------------------------------------------

// (S3 r5apex.exe == r5apex_ds.exe). Image base in is.
//-----------------------------------------------------------------------------
static constexpr uintptr_t kImageBase = 0x140000000ull;

// LEA reg, [rip+disp32] -- 7-byte instr, disp32 at +3.
// Form: 4C 8D 15 disp32 (LEA r10, [rip+...])
static const uintptr_t kLeaRipSites[] = {
	0x14014c905ull,
	0x14014ca65ull,
};

// Special LEA reg, [rip+disp32] -- 7-byte instr, disp32 at +3, references
// OLD_table+8 (slot[0].a4) instead of OLD_table+0. Form: 48 8D 3D disp32.
// New disp32 = (newArray + 8) - (instr + 7).
// Verified single occurrence in (the consumer that walks the
// 64-slot cache). If this site isn't patched, the consumer reads from the
// stock OLD_table at even after the other 24 sites point to
// our new array, and crashes when it sees the OLD table's empty layout
// while the new array is the live one.
static const uintptr_t kLeaRipPlus8Sites[] = {
	0x140fb2f30ull,   // consumer iteration base
};

// LEA reg, [r14+disp32] -- 7-byte instr, disp32 at +3.
// Form: 49 8D 9E disp32 (LEA rbx, [r14+...]) with r14 = module base.
static const uintptr_t kLeaR14Sites[] = {
	0x1405c2f57ull,   // search-loop base
	0x1405c2fb5ull,   // insert-path base
	0x140d5a2b7ull,   // search-loop base
	0x140d5a315ull,   // insert-path base
};

// CMP qword [r14+rax*8+disp32], (imm8|reg) -- disp32 at +4.
// Form: 49 83 BC C6 disp32 imm8 (8 bytes + imm8) OR 49 39 BC C6 disp32 (8 bytes).
static const uintptr_t kCmpR14Sites[] = {
	0x1405c2f8full,   // insert-loop slot-empty test
	0x140d5a2efull,   // insert-loop slot-empty test
};

// AND r32, 0x3F -- 3-byte instr, imm8 at +2 (current 0x3F, target 0x7F).
static const uintptr_t kMaskSites[] = {
	0x14014c913ull, 0x14014c944ull,
	0x14014ca73ull, 0x14014caa4ull,
	0x1405c2f54ull, 0x1405c2f88ull,
	0x140d5a2b4ull, 0x140d5a2e8ull,
};

// CMP r32, 0x40 -- 3-byte instr, imm8 at +2 (current 0x40, target 0x7F).
static const uintptr_t kLimitSites[] = {
	0x14014c92eull, 0x14014c954ull,
	0x14014ca8eull, 0x14014cab4ull,
	0x1405c2f76ull, 0x1405c2f9cull,
	0x140d5a2d6ull, 0x140d5a2fbull,
};

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static inline uint8_t* RvaToRuntime(uintptr_t preferredVa, uintptr_t actualBase)
{
	return reinterpret_cast<uint8_t*>(actualBase + (preferredVa - kImageBase));
}

static bool WriteBytes(void* addr, const void* data, size_t len)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[layout-cache] VirtualProtect failed @ %p (gle=%lu)\n",
			addr, GetLastError());
		return false;
	}
	memcpy(addr, data, len);
	VirtualProtect(addr, len, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), addr, len);
	return true;
}

void VLayoutCacheGrow::Detour(const bool bAttach) const
{
	if (!bAttach) return;

	if (!sdk_layout_cache_grow.GetBool() ||
		CommandLine()->CheckParm("-disable_layout_grow"))
	{
		Msg(eDLL_T::ENGINE,
			"[layout-cache] disabled (cmdline -disable_layout_grow or "
			"convar=0); stock 64-slot cache retained.\n");
		return;
	}

	const uintptr_t actualBase =
		reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));
	if (!actualBase)
	{
		Warning(eDLL_T::ENGINE,
			"[layout-cache] GetModuleHandleA(NULL) returned 0; aborting.\n");
		return;
	}

	// Allocate the cache region within +-2GiB of module base so r14-relative
	// disp32 references can reach it.
	if (!s_pNewLayoutCache)
	{
		DWORD allocSize = 0;
		void* region = DetourAllocateRegionWithinJumpBounds(
			reinterpret_cast<LPCVOID>(actualBase), &allocSize);
		if (!region || allocSize < kCacheBytes + HeapCanary::kTailBytes)
		{
			Warning(eDLL_T::ENGINE,
				"[layout-cache] DetourAllocateRegionWithinJumpBounds failed "
				"(region=%p size=%lu need=%zu); cannot patch r14-relative "
				"sites. Aborting (stock 64-slot cache retained).\n",
				region, allocSize, kCacheBytes);
			return;
		}
		// Zero-init -- "all slots empty" per engine convention.
		memset(region, 0, kCacheBytes);
		s_pNewLayoutCache = reinterpret_cast<uint64_t*>(region);
		HeapCanary::RegisterTail("layout-cache", s_pNewLayoutCache, kCacheBytes);
	}

	const uintptr_t newArrayAddr =
		reinterpret_cast<uintptr_t>(s_pNewLayoutCache);

	// r14-relative sites need (newArray - moduleBase) to fit signed int32.
	const int64_t deltaToBase =
		static_cast<int64_t>(newArrayAddr) - static_cast<int64_t>(actualBase);
	if (deltaToBase < INT32_MIN || deltaToBase > INT32_MAX)
	{
		Warning(eDLL_T::ENGINE,
			"[layout-cache] new array @ %p is %lld bytes from module base "
			"%p -- exceeds int32 range; cannot patch r14-relative LEA/CMP "
			"sites. Aborting (stock 64-slot cache retained).\n",
			(void*)newArrayAddr, (long long)deltaToBase, (void*)actualBase);
		return;
	}
	const int32_t newR14Disp = static_cast<int32_t>(deltaToBase);

	// Preflight: every site must match stock signatures before any write.
	// On apply failure, restore originals from a small undo log (max 25).
	struct UndoSite
	{
		void* addr;
		uint8_t data[4];
		size_t len;
	};
	UndoSite undo[32];
	int undoCount = 0;
	bool preflightOk = true;

	auto addUndo = [&](void* addr, size_t len) -> bool
	{
		if (undoCount >= 32 || len > 4)
			return false;
		memcpy(undo[undoCount].data, addr, len);
		undo[undoCount].addr = addr;
		undo[undoCount].len = len;
		++undoCount;
		return true;
	};
	auto rollback = [&]()
	{
		for (int i = undoCount - 1; i >= 0; --i)
			WriteBytes(undo[i].addr, undo[i].data, undo[i].len);
		undoCount = 0;
	};

	for (uintptr_t preferredVa : kLeaRipSites)
	{
		uint8_t* p = RvaToRuntime(preferredVa, actualBase);
		if (p[0] != 0x4C || p[1] != 0x8D || p[2] != 0x15)
		{
			Warning(eDLL_T::ENGINE,
				"[layout-cache] LEA-RIP signature mismatch @ %p "
				"(got %02X %02X %02X, expected 4C 8D 15)\n",
				p, p[0], p[1], p[2]);
			preflightOk = false;
		}
	}
	for (uintptr_t preferredVa : kLeaRipPlus8Sites)
	{
		uint8_t* p = RvaToRuntime(preferredVa, actualBase);
		if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x3D)
		{
			Warning(eDLL_T::ENGINE,
				"[layout-cache] LEA-RIP+8 signature mismatch @ %p "
				"(got %02X %02X %02X, expected 48 8D 3D)\n",
				p, p[0], p[1], p[2]);
			preflightOk = false;
		}
	}
	for (uintptr_t preferredVa : kLeaR14Sites)
	{
		uint8_t* p = RvaToRuntime(preferredVa, actualBase);
		if (p[0] != 0x49 || p[1] != 0x8D || p[2] != 0x9E)
		{
			Warning(eDLL_T::ENGINE,
				"[layout-cache] LEA-r14 signature mismatch @ %p "
				"(got %02X %02X %02X, expected 49 8D 9E)\n",
				p, p[0], p[1], p[2]);
			preflightOk = false;
		}
	}
	for (uintptr_t preferredVa : kCmpR14Sites)
	{
		uint8_t* p = RvaToRuntime(preferredVa, actualBase);
		const bool isCmpImm = (p[0] == 0x49 && p[1] == 0x83 && p[2] == 0xBC && p[3] == 0xC6);
		const bool isCmpReg = (p[0] == 0x49 && p[1] == 0x39 && p[2] == 0xBC && p[3] == 0xC6);
		if (!isCmpImm && !isCmpReg)
		{
			Warning(eDLL_T::ENGINE,
				"[layout-cache] CMP-r14 signature mismatch @ %p "
				"(got %02X %02X %02X %02X)\n",
				p, p[0], p[1], p[2], p[3]);
			preflightOk = false;
		}
	}
	for (uintptr_t preferredVa : kMaskSites)
	{
		uint8_t* p = RvaToRuntime(preferredVa, actualBase);
		if (p[0] != 0x83 || p[2] != 0x3F)
		{
			Warning(eDLL_T::ENGINE,
				"[layout-cache] mask signature mismatch @ %p "
				"(got %02X .. %02X, expected 83 .. 3F)\n",
				p, p[0], p[2]);
			preflightOk = false;
		}
	}
	for (uintptr_t preferredVa : kLimitSites)
	{
		uint8_t* p = RvaToRuntime(preferredVa, actualBase);
		if (p[0] != 0x83 || p[2] != 0x40)
		{
			Warning(eDLL_T::ENGINE,
				"[layout-cache] limit signature mismatch @ %p "
				"(got %02X .. %02X, expected 83 .. 40)\n",
				p, p[0], p[2]);
			preflightOk = false;
		}
	}

	if (!preflightOk)
	{
		Warning(eDLL_T::ENGINE,
			"[layout-cache] expansion OFF -- preflight failed, stock 64-slot cache retained\n");
		return;
	}

	bool applyOk = true;
	auto patchDisp32 = [&](uint8_t* pDisp, int32_t newVal) -> bool
	{
		if (!addUndo(pDisp, 4))
			return false;
		if (!WriteBytes(pDisp, &newVal, 4))
			return false;
		return true;
	};
	auto patchU8 = [&](uint8_t* pByte, uint8_t newVal) -> bool
	{
		if (!addUndo(pByte, 1))
			return false;
		if (!WriteBytes(pByte, &newVal, 1))
			return false;
		return true;
	};

	for (uintptr_t preferredVa : kLeaRipSites)
	{
		uint8_t* p = RvaToRuntime(preferredVa, actualBase);
		const int64_t delta = static_cast<int64_t>(newArrayAddr) -
			static_cast<int64_t>(reinterpret_cast<uintptr_t>(p + 7));
		if (delta < INT32_MIN || delta > INT32_MAX ||
			!patchDisp32(p + 3, static_cast<int32_t>(delta)))
		{
			applyOk = false;
			break;
		}
	}
	if (applyOk)
	{
		for (uintptr_t preferredVa : kLeaRipPlus8Sites)
		{
			uint8_t* p = RvaToRuntime(preferredVa, actualBase);
			const uintptr_t targetAddr = newArrayAddr + 8;
			const int64_t delta = static_cast<int64_t>(targetAddr) -
				static_cast<int64_t>(reinterpret_cast<uintptr_t>(p + 7));
			if (delta < INT32_MIN || delta > INT32_MAX ||
				!patchDisp32(p + 3, static_cast<int32_t>(delta)))
			{
				applyOk = false;
				break;
			}
		}
	}
	if (applyOk)
	{
		for (uintptr_t preferredVa : kLeaR14Sites)
		{
			uint8_t* p = RvaToRuntime(preferredVa, actualBase);
			if (!patchDisp32(p + 3, newR14Disp))
			{
				applyOk = false;
				break;
			}
		}
	}
	if (applyOk)
	{
		for (uintptr_t preferredVa : kCmpR14Sites)
		{
			uint8_t* p = RvaToRuntime(preferredVa, actualBase);
			if (!patchDisp32(p + 4, newR14Disp))
			{
				applyOk = false;
				break;
			}
		}
	}
	if (applyOk)
	{
		const uint8_t newMask = static_cast<uint8_t>(kNewMask);
		for (uintptr_t preferredVa : kMaskSites)
		{
			uint8_t* p = RvaToRuntime(preferredVa, actualBase);
			if (!patchU8(p + 2, newMask))
			{
				applyOk = false;
				break;
			}
		}
	}
	if (applyOk)
	{
		const uint8_t newLimit = static_cast<uint8_t>(kNewLimit);
		for (uintptr_t preferredVa : kLimitSites)
		{
			uint8_t* p = RvaToRuntime(preferredVa, actualBase);
			if (!patchU8(p + 2, newLimit))
			{
				applyOk = false;
				break;
			}
		}
	}

	if (!applyOk)
	{
		rollback();
		Warning(eDLL_T::ENGINE,
			"[layout-cache] expansion OFF -- apply failed after %d sites; "
			"rolled back, stock 64-slot cache retained\n",
			undoCount);
		return;
	}

	Msg(eDLL_T::ENGINE,
		"[layout-cache] grow ACTIVE: %d/25 sites; new array @ %p, slots=%d, "
		"mask=0x%X, limit=0x%X\n",
		undoCount, (void*)newArrayAddr,
		kNewSlotCount, kNewMask, kNewLimit);
}
