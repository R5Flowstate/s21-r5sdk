//=============================================================================
//
// Purpose: grow WeaponInfo legendary slots 8 -> 32 and grow modelprecache 4096 -> 8192.
// Twin halves have different alloc sizes; never carry an offset across that boundary.
//
//=============================================================================
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/memory_patch.h"
#include "tier0/memvalidate.h"
#include "tier0/commandline.h"
#include "tier1/convar.h"
#include "thirdparty/detours/include/detours.h"
#include "weapon_legendary_ext.h"
#include "heap_canary.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//-----------------------------------------------------------------------------
// Runtime gate. Disable + restart to revert to stock S3 (8-slot cap).
//-----------------------------------------------------------------------------
static ConVar sdk_legendary_cap("sdk_legendary_cap", "1", FCVAR_REPLICATED,
	"Lift legendary-skin slot cap from S3's 8 to S21's 32 (server-side). "
	"Grows server WeaponInfo struct by 12288 bytes per weapon and relocates "
	"the view-model names array. Disable + restart to revert to stock S3.");

// Two WeaponInfo twins. Never carry an offset across the client/server half boundary.
namespace s3Cl {
	constexpr uint32_t kStructSize = 0x589D8;
	constexpr uint32_t kWorldOff   = 0x579D8;
	constexpr uint32_t kViewOff    = 0x581D8;
	constexpr uint8_t  kCap        = 8;
}

// Client-half: 32 slots. World stays; view array moves after the expanded world names.
namespace oursCl {
	constexpr uint32_t kSlotBytes  = 256;
	constexpr uint8_t  kCap        = 32;
	constexpr uint32_t kWorldOff   = s3Cl::kWorldOff;
	constexpr uint32_t kViewOff    = s3Cl::kWorldOff + (uint32_t)kCap * kSlotBytes;
	constexpr uint32_t kStructSize = kViewOff + (uint32_t)kCap * kSlotBytes;
}

static_assert(oursCl::kViewOff   == 0x599D8, "view offset math (world end)");
static_assert(oursCl::kStructSize == 0x5B9D8, "struct size math");
static_assert(oursCl::kStructSize > s3Cl::kStructSize, "struct must grow");

// Server-half is the one CWeaponX reads. Re-derive every offset on that half.
namespace s3Sv {
	constexpr uint32_t kStructSize = 0x580A8;
	constexpr uint32_t kWorldOff   = 0x570A4;
	constexpr uint32_t kViewOff    = 0x578A4;
}
namespace oursSv {
	constexpr uint32_t kWorldOff   = s3Sv::kWorldOff;
	constexpr uint32_t kViewOff    = s3Sv::kWorldOff + (uint32_t)oursCl::kCap * oursCl::kSlotBytes;
	constexpr uint32_t kStructSize = kViewOff + (uint32_t)oursCl::kCap * oursCl::kSlotBytes
	                                + (s3Sv::kStructSize - (s3Sv::kViewOff + 8 * oursCl::kSlotBytes));
}
static_assert(oursSv::kViewOff    == 0x590A4, "server-half view offset math");
static_assert(oursSv::kStructSize == 0x5B0A8, "server-half struct size math");

static bool s_bServerStructGrown = false;
static bool s_bServerAllocFailed = false;

uint32_t WeaponLegendaryExt_ServerWeaponInfoSize(void)
{
	if (s_bServerStructGrown)
		return oursSv::kStructSize;
	if (s_bServerAllocFailed)
	{
		static volatile LONG s_nSizeWarn = 0;
		if (InterlockedIncrement(&s_nSizeWarn) <= 8)
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] server alloc subgroup failed "
				"(RVA 0xC72BE2 / 0xC72BFD / 0xD67D5C) -- using stock size 0x580A8\n");
	}
	return s3Sv::kStructSize;
}

// Immediate patch table: cap bytes, alloc/memset sizes, printed-max args.
struct ImmPatch
{
	uint32_t    rva;
	uint8_t     size;
	uint32_t    s3_imm;
	uint32_t    new_imm;
	const char* tag;
};

static constexpr ImmPatch kClientImmPatches[] = {
	{ 0x8E37F2, 4, s3Cl::kStructSize, oursCl::kStructSize, "alloc size  (mov edx, 589D8h)" },
	{ 0x8E380D, 4, s3Cl::kStructSize, oursCl::kStructSize, "memset size (mov r8d, 589D8h)" },
	{ 0x8D5ADC, 1, s3Cl::kCap,        oursCl::kCap,        "writer cap (cmp r14d, 8)" },
	{ 0x8D5AE1, 4, s3Cl::kCap,        oursCl::kCap,        "writer max (mov r8d, 8)"  },
	{ 0xA3CF8E, 1, s3Cl::kCap,        oursCl::kCap,        "SLMI cap (cmp eax, 8)"   },
	{ 0xA3CF93, 4, s3Cl::kCap,        oursCl::kCap,        "SLMI max (mov r8d, 8)"   },
};

static constexpr ImmPatch kServerAllocImmPatches[] = {
	{ 0xC72BE2, 4, s3Sv::kStructSize, oursSv::kStructSize, "server alloc size  (mov edx, 580A8h)" },
	{ 0xC72BFD, 4, s3Sv::kStructSize, oursSv::kStructSize, "server memset size (mov r8d, 580A8h)" },
	{ 0xD67D5C, 4, s3Sv::kStructSize, oursSv::kStructSize, "server free-call size" },
};

static constexpr ImmPatch kServerCapImmPatches[] = {
	{ 0xC68C4C, 1, s3Cl::kCap,         oursCl::kCap,         "server writer cap (cmp r14d, 8)" },
	{ 0xC68C51, 4, s3Cl::kCap,         oursCl::kCap,         "server writer max (mov r8d, 8)"  },
	{ 0x100230F, 1, s3Cl::kCap,        oursCl::kCap,         "server SLMI cap (cmp edi, 8)"   },
	{ 0x1002314, 4, s3Cl::kCap,        oursCl::kCap,         "server SLMI max (mov r8d, 8)"   },
};

// Disp32 patch tables for view-array relocations.
struct Disp32Patch
{
	uint32_t    rva;
	uint32_t    s3_imm;
	uint32_t    new_imm;
	const char* tag;
};

static constexpr Disp32Patch kClientHalfViewDisp32Patches[] = {
	{ 0x7CEDFB, s3Cl::kViewOff, oursCl::kViewOff, "view@0x7CEDFB"                   },
	{ 0x7D2B0F, s3Cl::kViewOff, oursCl::kViewOff, "view@0x7D2B0F"                   },
	{ 0x7D43C0, s3Cl::kViewOff, oursCl::kViewOff, "view@0x7D43C0"                   },
	{ 0x7D4440, s3Cl::kViewOff, oursCl::kViewOff, "view@0x7D4440"                   },
	{ 0x860E23, s3Cl::kViewOff, oursCl::kViewOff, "view@0x860E23"                   },
	{ 0x860F19, s3Cl::kViewOff, oursCl::kViewOff, "view@0x860F19"                   },
	{ 0x8D5BA6, s3Cl::kViewOff, oursCl::kViewOff, "view@SetWeaponLegendaryModel"    },
	{ 0x8E395D, s3Cl::kViewOff, oursCl::kViewOff, "view@allocator zero-fill"        },
	{ 0x9FDB42, s3Cl::kViewOff, oursCl::kViewOff, "view@0x9FDB42"                   },
	{ 0x9FDC98, s3Cl::kViewOff, oursCl::kViewOff, "view@0x9FDC98"                   },
	{ 0x9FFFA7, s3Cl::kViewOff, oursCl::kViewOff, "view@0x9FFFA7"                   },
	{ 0xA00528, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA00528"                   },
	{ 0xA0075B, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA0075B"                   },
	{ 0xA00FFC, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA00FFC"                   },
	{ 0xA35881, s3Cl::kViewOff, oursCl::kViewOff, "view@"              },
	{ 0xA3595F, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA3595F"                   },
	{ 0xA35B53, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA35B53"                   },
	{ 0xA39293, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA39293"                   },
	{ 0xA39BFF, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA39BFF"                   },
	{ 0xA3A533, s3Cl::kViewOff, oursCl::kViewOff, "view@"              },
	{ 0xA3A735, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA3A735"                   },
	{ 0xA3C7A4, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA3C7A4"                   },
	{ 0xA3CFBA, s3Cl::kViewOff, oursCl::kViewOff, "view@SetLegendaryModelIndex add" },
	{ 0xA4017D, s3Cl::kViewOff, oursCl::kViewOff, "view@0xA4017D"                   },
	{ 0xA50EAC, s3Cl::kViewOff, oursCl::kViewOff, "view@"              },
};

static constexpr Disp32Patch kServerHalfViewDisp32Patches[] = {
	{ 0xB52C89, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xB52C89"                 },
	{ 0xB5802F, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xB5802F"                 },
	{ 0xB589C9, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xB589C9"                 },
	{ 0xB589EE, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xB589EE"                 },
	{ 0xB58A8A, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xB58A8A"                 },
	{ 0xB58AB0, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xB58AB0"                 },
	{ 0xBE89DA, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xBE89DA"                 },
	{ 0xBE89FF, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xBE89FF"                 },
	{ 0xC68D06, s3Sv::kViewOff, oursSv::kViewOff, "Cview@SetWeaponLegendaryModel"  },
	{ 0xC72D4D, s3Sv::kViewOff, oursSv::kViewOff, "Cview@allocator zero-fill"      },
	{ 0xFC4C32, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xFC4C32"                 },
	{ 0xFF8B6C, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xFF8B6C"                 },
	{ 0xFF8BBC, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xFF8BBC"                 },
	{ 0xFF8D9D, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xFF8D9D"                 },
	{ 0xFF8DC0, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xFF8DC0"                 },
	{ 0xFFC4E2, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xFFC4E2"                 },
	{ 0xFFCD74, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xFFCD74"                 },
	{ 0xFFD54D, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0xFFD54D"                 },
	{ 0x1001376, s3Sv::kViewOff, oursSv::kViewOff, "Cview@0x1001376"               },
	{ 0x1002368, s3Sv::kViewOff, oursSv::kViewOff, "Cview@SetLegendaryModelIndex"  },
};

// Grow modelprecache 4096 -> 8192 so legendary indices cannot alias adjacent CClientState fields.
static constexpr uint32_t kNewArrayCapacity = 0x4000;        // 16384 entries (lifted from S21's 8192)
static constexpr uint32_t kNewArrayBytes    = kNewArrayCapacity * 16;
static void* g_pNewModelArray = nullptr;

// VirtualAlloc a buffer near `nearAddr` (within signed-32-bit reach).
// Returns nullptr if no slot fits within the search range.
static void* AllocateNearAddress(uintptr_t nearAddr, size_t size)
{
	SYSTEM_INFO si = {};
	GetSystemInfo(&si);
	const uintptr_t gran = si.dwAllocationGranularity;
	uintptr_t addr = (nearAddr + gran - 1) & ~(uintptr_t)(gran - 1);

	for (int i = 0; i < 4096; ++i)
	{
		void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size,
			MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (p) return p;
		addr += gran;
	}
	return nullptr;
}

static bool ApplyModelPrecacheGrow(uintptr_t moduleBase, uintptr_t moduleSize)
{
	// (1) Allocate the replacement array near the engine module.
	const uintptr_t hint = moduleBase + moduleSize;
	g_pNewModelArray = AllocateNearAddress(hint, kNewArrayBytes + HeapCanary::kTailBytes);
	if (!g_pNewModelArray)
	{
		Warning(eDLL_T::ENGINE,
			"[WLEG-EXT] grow: VirtualAlloc near 0x%llX failed; "
			"modelprecache stays at 4096-entry hard cap.\n",
			(unsigned long long)hint);
		return false;
	}
	HeapCanary::RegisterTail("legendary-modelprecache", g_pNewModelArray, kNewArrayBytes);

	// Copy the existing 4096-entry table into the new buffer before redirecting readers.
	const uintptr_t oldTable = moduleBase + 0x234E13D8;  // unk_1634E13D8
	const size_t    oldBytes = 0x10000;                   // 4096 * 16
	void* const pOldTable = reinterpret_cast<void*>(oldTable);
	if (Mem_InModule(g_GameDll, pOldTable, oldBytes))
	{
		std::memcpy(g_pNewModelArray, pOldTable, oldBytes);
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"[WLEG-EXT] grow: migration source 0x%llX outside module -- "
			"pre-existing entries WILL be stale; SendServerInfo may AV in "
			"WriteString\n",
			(unsigned long long)oldTable);
	}

	// Verify both patch sites are within signed-32-bit disp32 range.
	const uintptr_t bufAddr     = reinterpret_cast<uintptr_t>(g_pNewModelArray);
	const uintptr_t leaSite258  = moduleBase + 0x258BF7;     // lea rax,...
	const uintptr_t leaSite313  = moduleBase + 0x313916;     // add rbx,...
	const int64_t   d258        = (int64_t)bufAddr - (int64_t)(leaSite258 + 7);
	const int64_t   d313        = (int64_t)bufAddr - (int64_t)(leaSite313 + 7);
	if (d258 < INT32_MIN || d258 > INT32_MAX ||
	    d313 < INT32_MIN || d313 > INT32_MAX)
	{
		VirtualFree(g_pNewModelArray, 0, MEM_RELEASE);
		g_pNewModelArray = nullptr;
		Warning(eDLL_T::ENGINE,
			"[WLEG-EXT] grow: replacement buffer too far for disp32 "
			"(d258=0x%llX, d313=0x%llX)\n",
			(long long)d258, (long long)d313);
		return false;
	}

	// (2) Patch the LEA at +0x07: keep `48 8D 05` (lea rax),
	// replace disp32.
	{
		uint8_t* p = reinterpret_cast<uint8_t*>(leaSite258);
		if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x05)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] grow: LEA opcode mismatch at +0x258BF7: "
				"%02X %02X %02X (expected 48 8D 05)\n",
				p[0], p[1], p[2]);
			return false;
		}
		const int32_t disp32 = static_cast<int32_t>(d258);
		if (!Mem_PatchCode(p + 3, &disp32, 4))
			return false;
	}

	// (3) Patch +0x36: replace 7-byte `add rbx, imm32`
	// (48 81 C3 D8 65 DA 02) with `lea rbx, [rip+disp32]`
	// (48 8D 1D disp32). Same length, no shift required.
	{
		uint8_t* p = reinterpret_cast<uint8_t*>(leaSite313);
		if (p[0] != 0x48 || p[1] != 0x81 || p[2] != 0xC3 ||
		    p[3] != 0xD8 || p[4] != 0x65 || p[5] != 0xDA || p[6] != 0x02)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] grow: ADD opcode mismatch at +0x313916: "
				"%02X %02X %02X %02X %02X %02X %02X "
				"(expected 48 81 C3 D8 65 DA 02)\n",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
			return false;
		}
		uint8_t newBytes[7] = { 0x48, 0x8D, 0x1D, 0, 0, 0, 0 };
		const int32_t disp32 = static_cast<int32_t>(d313);
		std::memcpy(newBytes + 3, &disp32, 4);
		if (!Mem_PatchCode(p, newBytes, 7))
			return false;
	}

	// (4) Bump maxEntries literal: at the instruction
	// `mov r9d, 0x1000` (41 B9 00 10 00 00) has its imm32 at +2.
	{
		uint8_t* p = reinterpret_cast<uint8_t*>(moduleBase + 0x3136B6);
		if (p[0] != 0x41 || p[1] != 0xB9)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] grow: mov r9d opcode mismatch at +0x3136B6: "
				"%02X %02X (expected 41 B9)\n", p[0], p[1]);
			return false;
		}
		const uint32_t cur = *reinterpret_cast<uint32_t*>(p + 2);
		if (cur != 0x1000)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] grow: maxEntries literal mismatch: "
				"got 0x%X, expected 0x1000\n", cur);
			return false;
		}
		const uint32_t newMax = kNewArrayCapacity;
		if (!Mem_PatchCode(p + 2, &newMax, 4))
			return false;
	}

	// Grow the per-map Clear memset that zeroes modelprecache.
	{
		uint8_t* p = reinterpret_cast<uint8_t*>(moduleBase + 0x310159);
		if (p[0] != 0x41 || p[1] != 0xB8)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] grow: Clear memset-size opcode mismatch at "
				"+0x310159: %02X %02X (expected 41 B8)\n", p[0], p[1]);
			return false;
		}
		const uint32_t curSize = *reinterpret_cast<uint32_t*>(p + 2);
		if (curSize != 0x10000)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] grow: Clear memset-size literal mismatch: "
				"got 0x%X, expected 0x10000\n", curSize);
			return false;
		}
		const uint32_t newClearSize = kNewArrayBytes;   // 0x20000 = 8192 * 16
		if (!Mem_PatchCode(p + 2, &newClearSize, 4))
			return false;
	}

	return true;
}

// Redirect every disp32 that lands in the old modelprecache range onto the grown buffer.


// Confirm the decoded operand actually lands in the modelprecache range before patching.
struct ModelArrayDispPatch
{
	uint32_t    rva;          // RVA of the disp32 field within the instruction
	uint32_t    s3_disp32;    // OLD singleton-relative offset (per-site)
	const char* tag;
};

static constexpr ModelArrayDispPatch kModelArrayPatches[] = {
	{ 0x310153, 0x2DA65D8, "+0x133 memset LEA (flags base)"         },
	{ 0x3132D2, 0x2DA65D8, "+0x552 flags read (Add path 2)"         },
	{ 0x3132DD, 0x2DA65E0, "+0x55D PAYLOAD WRITE (Add path 2)"        },
	{ 0x3132E6, 0x2DA65D8, "+0x566 flags write (Add path 2)"         },
	{ 0x3132F5, 0x2DA65D8, "+0x575 flags update (Add path 2)"         },
	{ 0x313883, 0x2DA65D8, "+0x43 flags read (Add path 1)"         },
	{ 0x31388E, 0x2DA65E0, "+0x4E PAYLOAD WRITE (Add path 1, orphan source)" },
	{ 0x313899, 0x2DA65D8, "+0x59 flags write (Add path 1)"         },
	{ 0x3138A9, 0x2DA65D8, "+0x69 flags update (Add path 1)"         },
	{ 0x30F7BB, 0x2DA65F0, "+0x13B slot 1 PAYLOAD read (SVC_ServerInfo map model)" },
	{ 0x30F7CB, 0x2DA65E8, "+0x14B slot 1 FLAGS bump (SVC_ServerInfo map model)" },
};

static int ApplyModelArrayPatches(uintptr_t moduleBase)
{
	if (!g_pNewModelArray)
		return 0;

	const uintptr_t oldTable = moduleBase + 0x234E13D8;  // unk_1634E13D8
	const intptr_t delta = (intptr_t)g_pNewModelArray - (intptr_t)oldTable;
	if (delta < INT32_MIN || delta > INT32_MAX)
	{
		Warning(eDLL_T::ENGINE,
			"[WLEG-EXT] modelarray delta out of disp32 range: 0x%llX\n",
			(long long)delta);
		return 0;
	}

	int ok = 0, fail = 0;
	for (const ModelArrayDispPatch& p : kModelArrayPatches)
	{
		uint8_t* pDisp = (uint8_t*)(moduleBase + p.rva);
		const uint32_t cur = *(uint32_t*)pDisp;
		if (cur != p.s3_disp32)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] modelarray MISMATCH '%s' RVA=0x%X "
				"got=0x%X expected=0x%X\n",
				p.tag, p.rva, cur, p.s3_disp32);
			++fail;
			continue;
		}

		const uint32_t newDisp32 = static_cast<uint32_t>(
			static_cast<int32_t>(p.s3_disp32) + static_cast<int32_t>(delta));
		if (!Mem_PatchCode(pDisp, &newDisp32, 4))
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] modelarray VirtualProtect failed '%s' RVA=0x%X\n",
				p.tag, p.rva);
			++fail;
			continue;
		}
		++ok;
	}
	Msg(eDLL_T::ENGINE,
		"[WLEG-EXT] modelarray ADD-path sites: %d OK, %d failed (of %zd, delta=0x%llX)\n",
		ok, fail, sizeof(kModelArrayPatches)/sizeof(kModelArrayPatches[0]),
		(long long)delta);
	return fail;
}

//-----------------------------------------------------------------------------
// Per-site validating patchers
//-----------------------------------------------------------------------------
static int ApplyImmTable(uintptr_t moduleBase, const ImmPatch* table, size_t n)
{
	int ok = 0, fail = 0;
	for (size_t i = 0; i < n; ++i)
	{
		const ImmPatch& p = table[i];
		uint8_t* pImm = (uint8_t*)(moduleBase + p.rva);
		uint32_t cur  = (p.size == 1) ? (uint32_t)pImm[0] : *(uint32_t*)pImm;

		if (cur != p.s3_imm)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] imm MISMATCH '%s' RVA=0x%X size=%u "
				"got=0x%X expected=0x%X\n",
				p.tag, p.rva, p.size, cur, p.s3_imm);
			++fail;
			continue;
		}

		bool wrote;
		if (p.size == 1)
		{
			uint8_t b = (uint8_t)p.new_imm;
			wrote = Mem_PatchCode(pImm, &b, 1);
		}
		else
		{
			wrote = Mem_PatchCode(pImm, &p.new_imm, 4);
		}
		if (!wrote)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] imm VirtualProtect failed '%s' RVA=0x%X\n",
				p.tag, p.rva);
			++fail;
			continue;
		}
		++ok;
	}
	Msg(eDLL_T::ENGINE, "[WLEG-EXT] imm sites: %d OK, %d failed (of %zu)\n",
		ok, fail, n);
	return fail;
}

template <size_t N>
static int ApplyDisp32Table(uintptr_t moduleBase, const Disp32Patch (&table)[N], const char* label)
{
	int ok = 0, fail = 0;
	for (const Disp32Patch& p : table)
	{
		uint8_t* pDisp = (uint8_t*)(moduleBase + p.rva);
		uint32_t cur   = *(uint32_t*)pDisp;
		if (cur != p.s3_imm)
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] disp32 MISMATCH '%s' RVA=0x%X "
				"got=0x%X expected=0x%X\n",
				p.tag, p.rva, cur, p.s3_imm);
			++fail;
			continue;
		}
		if (!Mem_PatchCode(pDisp, &p.new_imm, 4))
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] disp32 VirtualProtect failed '%s' RVA=0x%X\n",
				p.tag, p.rva);
			++fail;
			continue;
		}
		++ok;
	}
	Msg(eDLL_T::ENGINE, "[WLEG-EXT] %s disp32 sites: %d OK, %d failed (of %zd)\n",
		label, ok, fail, N);
	return fail;
}

// -disable_wleg_legendary keeps the S3 cap so a bisect can isolate the slot-grow from the precache grow.
typedef __int64 (__fastcall* SetWeaponLegendaryModel_t)(
	unsigned char* weaponName, int slot,
	char* worldModel, unsigned char* viewModel);

// Both halves carry SetWeaponLegendaryModel. Detour both; the client half registers later.
static SetWeaponLegendaryModel_t v_SetWeaponLegendaryModel_ClientHalf = nullptr;
static SetWeaponLegendaryModel_t v_SetWeaponLegendaryModel_ServerHalf = nullptr;
static bool g_legendarySkipMode = false;
static std::atomic<uint32_t> g_legendarySkipCount{0};

static __int64 __fastcall Hook_SetWeaponLegendaryModel_ClientHalf(
	unsigned char* weaponName, int slot,
	char* worldModel, unsigned char* viewModel)
{
	if (g_legendarySkipMode && slot >= (int)s3Cl::kCap)
	{
		const uint32_t n = g_legendarySkipCount.fetch_add(
			1, std::memory_order_relaxed);
		if (n < 4 || (n & 0xFFu) == 0)
		{
			DevMsg(eDLL_T::ENGINE,
				"[WLEG-EXT] skipped client-half SetWeaponLegendaryModel(slot=%d) "
				"#%u -- bisect mode\n", slot, n + 1);
		}
		return 0;
	}
	return v_SetWeaponLegendaryModel_ClientHalf(
		weaponName, slot, worldModel, viewModel);
}

static __int64 __fastcall Hook_SetWeaponLegendaryModel_ServerHalf(
	unsigned char* weaponName, int slot,
	char* worldModel, unsigned char* viewModel)
{
	if (g_legendarySkipMode && slot >= (int)s3Cl::kCap)
	{
		const uint32_t n = g_legendarySkipCount.fetch_add(
			1, std::memory_order_relaxed);
		if (n < 4 || (n & 0xFFu) == 0)
		{
			DevMsg(eDLL_T::ENGINE,
				"[WLEG-EXT] skipped server-half SetWeaponLegendaryModel(slot=%d) "
				"#%u -- bisect mode\n", slot, n + 1);
		}
		return 0;
	}
	return v_SetWeaponLegendaryModel_ServerHalf(
		weaponName, slot, worldModel, viewModel);
}

//-----------------------------------------------------------------------------
// VWeaponLegendaryExt
//-----------------------------------------------------------------------------
void VWeaponLegendaryExt::GetAdr(void) const { }

void VWeaponLegendaryExt::Detour(const bool bAttach) const
{
	if (!bAttach) return;

	s_bServerStructGrown = false;
	s_bServerAllocFailed = false;

	if (!sdk_legendary_cap.GetBool() ||
		CommandLine()->CheckParm("-disable_wleg"))
	{
		Msg(eDLL_T::ENGINE,
			"[WLEG-EXT] disabled (cmdline -disable_wleg or "
			"sdk_legendary_cap=0) -- stock S3 8-slot cap "
			"(SetWeaponLegendaryModel rejects index >= 8)\n");
		return;
	}

	const uintptr_t moduleBase = g_GameDll.GetModuleBase();
	const uintptr_t moduleSize = g_GameDll.GetModuleSize();

	// Grow modelprecache 4096 -> 8192.
	ApplyModelPrecacheGrow(moduleBase, moduleSize);
	const int modelArrayFail = ApplyModelArrayPatches(moduleBase);

	// Bisect gate: skip the 8 -> 32 slot-cap patches.
	const bool skipLegendary =
		CommandLine()->CheckParm("-disable_wleg_legendary");

	int immFail = 0, dispClientHalfFail = 0, dispServerHalfFail = 0;
	int serverAllocFail = 0;
	if (skipLegendary)
	{
		Msg(eDLL_T::ENGINE,
			"[WLEG-EXT] LEGENDARY-CAP DISABLED via -disable_wleg_legendary -- "
			"modelprecache grow still active (so the dedi boots) but cap 8->32, "
			"WeaponInfo grow, and view-model disp32 relocations are SKIPPED\n");

		// Detour both SetWeaponLegendaryModel halves so index >= 8 is not rejected.

		g_legendarySkipMode = true;

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 "
			"48 83 EC 20 4C 63 F2 49 8B D9 49 8B F8 48 8B F1 "
			"48 85 C9 0F 84 67 01")
			.GetPtr(v_SetWeaponLegendaryModel_ClientHalf);
		if (v_SetWeaponLegendaryModel_ClientHalf)
		{
			const LONG r = DetourAttach(
				reinterpret_cast<void**>(&v_SetWeaponLegendaryModel_ClientHalf),
				reinterpret_cast<void*>(&Hook_SetWeaponLegendaryModel_ClientHalf));
			Msg(eDLL_T::ENGINE,
				"[WLEG-EXT] client-half SetWeaponLegendaryModel detour attach "
				"result=0x%lX (target=%p)\n",
				r, (void*)v_SetWeaponLegendaryModel_ClientHalf);
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] client-half SetWeaponLegendaryModel pattern unresolved\n");
		}

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 "
			"48 83 EC 20 4C 63 F2 49 8B D9 49 8B F8 48 8B F1 "
			"48 85 C9 0F 84 57 01")
			.GetPtr(v_SetWeaponLegendaryModel_ServerHalf);
		if (v_SetWeaponLegendaryModel_ServerHalf)
		{
			const LONG r = DetourAttach(
				reinterpret_cast<void**>(&v_SetWeaponLegendaryModel_ServerHalf),
				reinterpret_cast<void*>(&Hook_SetWeaponLegendaryModel_ServerHalf));
			Msg(eDLL_T::ENGINE,
				"[WLEG-EXT] server-half SetWeaponLegendaryModel detour attach "
				"result=0x%lX (target=%p)\n",
				r, (void*)v_SetWeaponLegendaryModel_ServerHalf);
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] server-half SetWeaponLegendaryModel pattern unresolved "
				"-- vscript will still error on slot >= 8 (the client-half binding "
				"shadows the server in listen-server builds). Bisect blocked.\n");
		}
	}
	else
	{
		serverAllocFail = ApplyImmTable(moduleBase, kServerAllocImmPatches,
			sizeof(kServerAllocImmPatches) / sizeof(kServerAllocImmPatches[0]));
		s_bServerAllocFailed = (serverAllocFail != 0);
		s_bServerStructGrown = (serverAllocFail == 0);

		int serverCapFail = 0;
		if (serverAllocFail == 0)
		{
			serverCapFail = ApplyImmTable(moduleBase, kServerCapImmPatches,
				sizeof(kServerCapImmPatches) / sizeof(kServerCapImmPatches[0]));
			dispServerHalfFail = ApplyDisp32Table(moduleBase, kServerHalfViewDisp32Patches, "server-half view");
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[WLEG-EXT] server alloc subgroup failed -- skipped remaining "
				"server-half cap/disp32 patches (RVA 0xC72BE2 / 0xC72BFD / 0xD67D5C)\n");
		}

		const int clientImmFail = ApplyImmTable(moduleBase, kClientImmPatches,
			sizeof(kClientImmPatches) / sizeof(kClientImmPatches[0]));
		dispClientHalfFail = ApplyDisp32Table(moduleBase, kClientHalfViewDisp32Patches, "client-half view");
		immFail = serverAllocFail + serverCapFail + clientImmFail;
	}

	const int dispFail = dispClientHalfFail + dispServerHalfFail + modelArrayFail;

	if (immFail + dispFail != 0)
	{
		Warning(eDLL_T::ENGINE,
			"[WLEG-EXT] partial application -- %d imm + %d disp32 mismatches. "
			"WeaponInfo state is now MIXED (some sites grew, some still S3-sized) "
			"-- legendary skin reads will likely be corrupt for slot 8+. Disable "
			"via sdk_legendary_cap=0 + restart, or update the patch catalog "
			"if the engine binary changed.\n",
			immFail, dispFail);
	}
}
