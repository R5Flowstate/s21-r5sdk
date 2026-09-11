//=============================================================================//
//
// Purpose: Heap-backed baseStruct item overflow for pdef parse.
// Engine itemPool is fixed 8640 bytes (180*48). Overflow items go to a heap
// buffer (itemPool swapped during AddVarToStruct when count>=180); writeout
// is patched so Src + overflow both land in items_dict.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/commandline.h"
#include "tier1/convar.h"
#include "thirdparty/detours/include/idetour.h"
#include "persistence_basevar_overflow.h"
#include "persistence_ext.h"
#include "game/shared/heap_canary.h"

#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static ConVar sdk_pdef_basevar_overflow("sdk_pdef_basevar_overflow", "1",
	FCVAR_REPLICATED,
	"Allow > 180 baseStruct items in pdef parse via heap-backed overflow "
	"buffer. Required for S21-capacity pdefs.");

static constexpr size_t kSrcCapBytes   = 8640;       // engine's local Src
static constexpr size_t kSrcCapItems   = 180;        // 8640 / 48
static constexpr size_t kOverflowCap   = 4096;       // total cap incl. Src
static constexpr size_t kOverflowItems = kOverflowCap - kSrcCapItems; // 3916
static constexpr size_t kOverflowBytes = kOverflowItems * 48;         // ~188 KB

static uint8_t* g_pOverflowBuf = nullptr;
static uint8_t* g_pMemmoveThunk = nullptr;
static bool s_bArmed = false;

void PersistenceExt_ApplyItemsAllocatedRaise(void);

bool PersistenceBvo_IsArmed(void)
{
	return s_bArmed;
}

//-----------------------------------------------------------------------------
// Allocate a small executable thunk page within +-2 GB of `nearAddr`. Used
// because the SDK DLL loads ~22 GB away from r5apex_ds.exe so a direct E8
// CALL can't reach our Hook function -- we bounce through a thunk that does
// `mov rax, imm64; jmp rax` (12 bytes).
//-----------------------------------------------------------------------------
static uint8_t* AllocateNearThunk(uintptr_t nearAddr, size_t size)
{
	for (int dir = 1; dir >= -1; dir -= 2)
	{
		uintptr_t scanAddr = (nearAddr + dir * 0x10000000ULL) & ~0xFFFFULL;
		for (int attempt = 0; attempt < 4096; ++attempt)
		{
			const uintptr_t tryAddr = scanAddr + dir * attempt * 0x10000ULL;
			const int64_t   dist    = (int64_t)tryAddr - (int64_t)nearAddr;
			if (dist > 0x70000000LL || dist < -0x70000000LL) break;

			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery((void*)tryAddr, &mbi, sizeof(mbi)) != sizeof(mbi))
				continue;
			if (mbi.State != MEM_FREE || mbi.RegionSize < size)
				continue;

			void* p = VirtualAlloc((void*)tryAddr, size,
				MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (p) return (uint8_t*)p;
		}
	}
	return nullptr;
}

// AddVarToStruct: (pdef, struct_ptr, type, meta, name, array_spec) -> bool/char
typedef char(__fastcall* AddVarToStruct_t)(__int64, __int64, int, unsigned __int64,
	char*, char*);
static AddVarToStruct_t v_AddVarToStruct = nullptr;

// memmove signature (matches CRT's __cdecl on x64 = unified ABI)
typedef void*(*memmove_t)(void*, const void*, size_t);
static memmove_t v_memmove_orig = nullptr;

// Tracking: total items in baseStruct after each AddVarToStruct call. Used
// by the memmove trampoline to know how many overflow items to append.
// Single-threaded (pdef parse runs on the main thread).
static uint64_t s_lastBaseCount = 0;

static char __fastcall Hook_AddVarToStruct(__int64 a1, __int64 a2, int a3,
	unsigned __int64 a4, char* Source, char* a6)
{
	if (g_pSdkPdataDef && g_pOverflowBuf)
	{
		const __int64 baseStructPtr =
			(__int64)(g_pSdkPdataDef + kSdkPdefSubTableOff);

		if (a2 == baseStructPtr)
		{
			const uint64_t count = *(uint64_t*)(a2 + 40);
			const uint64_t dictUsed = *(uint64_t*)(g_pSdkPdataDef + 0x30000);
			if (count >= kOverflowCap || dictUsed + count > kOverflowCap)
			{
				static bool s_bWarnedCap = false;
				if (!s_bWarnedCap)
				{
					Warning(eDLL_T::ENGINE,
						"[PDEF-BVO] AddVarToStruct count %llu dictUsed %llu cap %zu; reject\n",
						(unsigned long long)count, (unsigned long long)dictUsed, kOverflowCap);
					s_bWarnedCap = true;
				}
				return 0;
			}
			if (count >= kSrcCapItems)
			{
				// Engine writes at *(a2+24) + 48*count. We want target =
				// overflow_buf + 48*(count - 180), so set *(a2+24) =
				// overflow_buf - 48*180.
				const __int64 origPool = *(__int64*)(a2 + 24);
				*(__int64*)(a2 + 24) =
					(__int64)g_pOverflowBuf -
					(__int64)(48 * kSrcCapItems);

				const char r = v_AddVarToStruct(a1, a2, a3, a4, Source, a6);

				*(__int64*)(a2 + 24) = origPool;
				s_lastBaseCount = count + (r ? 1 : 0);
				return r;
			}
			s_lastBaseCount = count + 1; // engine will inc on success; close enough
		}
	}
	return v_AddVarToStruct(a1, a2, a3, a4, Source, a6);
}

// Replaces the `call memmove` at +0x53A. The engine call passes
// (dst=items_dict_slot, src=local_Src, n=48*count). For count > 180 the
// engine reads beyond Src; we copy the in-Src portion (180 items) and append
// our overflow buffer's (count-180) items.
static void* __cdecl Hook_BaseVarMemmove(void* dst, const void* src, size_t n)
{
	if (g_pSdkPdataDef && dst)
	{
		uint8_t* const dict = g_pSdkPdataDef;
		uint8_t* const dictEnd = dict + (4096 * 48);
		uint8_t* const p = static_cast<uint8_t*>(dst);
		if (p >= dict && p < dictEnd)
		{
			const size_t remain = static_cast<size_t>(dictEnd - p);
			if (n > remain)
				n = remain;
		}
	}
	if (n > kSrcCapBytes && g_pOverflowBuf)
	{
		const size_t first = (kSrcCapBytes <= n) ? kSrcCapBytes : n;
		v_memmove_orig(dst, src, first);
		if (n > kSrcCapBytes)
		{
			const size_t extra = n - kSrcCapBytes;
			const size_t toCopy = (extra <= kOverflowBytes) ? extra : kOverflowBytes;
			v_memmove_orig((uint8_t*)dst + kSrcCapBytes, g_pOverflowBuf, toCopy);
		}
		return dst;
	}
	return v_memmove_orig(dst, src, n);
}

void VPersistenceBaseVarOverflow::GetAdr(void) const { }
void VPersistenceBaseVarOverflow::GetVar(void) const { }

void VPersistenceBaseVarOverflow::Detour(const bool bAttach) const
{
	if (!bAttach) return;
	if (!sdk_pdef_basevar_overflow.GetBool() ||
		CommandLine()->CheckParm("-disable_pdef_bvo"))
	{
		Msg(eDLL_T::ENGINE,
			"[PDEF-BVO] disabled (cmdline -disable_pdef_bvo or "
			"sdk_pdef_basevar_overflow=0)\n");
		return;
	}

	g_pOverflowBuf = (uint8_t*)std::calloc(kOverflowBytes + HeapCanary::kTailBytes, 1);
	if (!g_pOverflowBuf)
	{
		Warning(eDLL_T::ENGINE,
			"[PDEF-BVO] failed to allocate %zu-byte overflow buffer\n",
			kOverflowBytes);
		return;
	}
	HeapCanary::RegisterTail("pdef-basevar-overflow", g_pOverflowBuf, kOverflowBytes);

	const uintptr_t moduleBase = g_GameDll.GetModuleBase();

	// Hook AddVarToStruct for overflow redirection.
	v_AddVarToStruct = (AddVarToStruct_t)(moduleBase + 0x35DF80);
	if (DetourSetup(&v_AddVarToStruct, &Hook_AddVarToStruct, bAttach) != NO_ERROR)
	{
		Warning(eDLL_T::ENGINE,
			"[PDEF-BVO] AddVarToStruct hook failed\n");
		std::free(g_pOverflowBuf);
		g_pOverflowBuf = nullptr;
		s_bArmed = false;
		return;
	}

	// Patch the `call memmove` at (5 bytes: E8 disp32) so the
	// writeout reads from both Src and our overflow buffer.
	{
		uint8_t* pCall = (uint8_t*)(moduleBase + 0x35F6EA);
		if (pCall[0] != 0xE8)
		{
			Warning(eDLL_T::ENGINE,
				"[PDEF-BVO] expected E8 (CALL) at RVA 0x35F6EA, got 0x%02X "
				"-- engine binary changed?\n", pCall[0]);
			std::free(g_pOverflowBuf);
			g_pOverflowBuf = nullptr;
			s_bArmed = false;
			return;
		}

		// Resolve original target so we can call it from our trampoline.
		const int32_t origDisp = *(int32_t*)(pCall + 1);
		v_memmove_orig =
			(memmove_t)((uintptr_t)pCall + 5 + (intptr_t)origDisp);

		// Allocate a near-thunk so the 5-byte CALL can reach. Engine and SDK
		// DLL can be > 2 GB apart so a direct E8 disp32 to our Hook function
		// is out of range. The thunk does `mov rax, imm64; jmp rax`.
		g_pMemmoveThunk = AllocateNearThunk((uintptr_t)pCall, 16);
		if (!g_pMemmoveThunk)
		{
			Warning(eDLL_T::ENGINE,
				"[PDEF-BVO] failed to allocate near-thunk within +-2GB of "
				"call site 0x%p\n", (void*)pCall);
			std::free(g_pOverflowBuf);
			g_pOverflowBuf = nullptr;
			s_bArmed = false;
			return;
		}

		// Write thunk: 48 B8 <imm64> FF E0 (mov rax, hook; jmp rax)
		g_pMemmoveThunk[0] = 0x48;
		g_pMemmoveThunk[1] = 0xB8;
		*(uint64_t*)(g_pMemmoveThunk + 2) = (uint64_t)&Hook_BaseVarMemmove;
		g_pMemmoveThunk[10] = 0xFF;
		g_pMemmoveThunk[11] = 0xE0;
		FlushInstructionCache(GetCurrentProcess(), g_pMemmoveThunk, 12);

		// Compute new disp from call site to thunk.
		const int64_t newDisp =
			(int64_t)g_pMemmoveThunk - ((int64_t)pCall + 5);
		if (newDisp > INT32_MAX || newDisp < INT32_MIN)
		{
			Warning(eDLL_T::ENGINE,
				"[PDEF-BVO] near-thunk at 0x%p still out of +-2GB from "
				"call site 0x%p (disp=0x%llX) -- AllocateNearThunk bug\n",
				(void*)g_pMemmoveThunk, (void*)pCall,
				(long long)newDisp);
			std::free(g_pOverflowBuf);
			g_pOverflowBuf = nullptr;
			s_bArmed = false;
			return;
		}

		DWORD oldProt = 0;
		if (!VirtualProtect(pCall + 1, 4, PAGE_EXECUTE_READWRITE, &oldProt))
		{
			Warning(eDLL_T::ENGINE,
				"[PDEF-BVO] VirtualProtect failed on memmove call\n");
			std::free(g_pOverflowBuf);
			g_pOverflowBuf = nullptr;
			s_bArmed = false;
			return;
		}
		const int32_t newDisp32 = (int32_t)newDisp;
		std::memcpy(pCall + 1, &newDisp32, 4);
		VirtualProtect(pCall + 1, 4, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), pCall, 5);
	}

	s_bArmed = true;
	PersistenceExt_ApplyItemsAllocatedRaise();

	Msg(eDLL_T::ENGINE,
		"[PDEF-BVO] active: overflow buffer 0x%p (%zu items, %zu bytes), "
		"AddVarToStruct hooked, memmove call at 0x35F6EA -> thunk 0x%p -> "
		"hook 0x%p (orig=0x%p)\n",
		(void*)g_pOverflowBuf, kOverflowItems, kOverflowBytes,
		(void*)g_pMemmoveThunk, (void*)&Hook_BaseVarMemmove,
		(void*)v_memmove_orig);
}
