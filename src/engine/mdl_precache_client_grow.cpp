//=============================================================================//
//
// Purpose: Heap shadow for modelprecache idx [0x2000, 0x4000).
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_bridge_addrs.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "tier0/memory_patch.h"
#include "thirdparty/detours/include/detours.h"
#include "game/shared/heap_canary.h"
#include "mdl_precache_client_grow.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//-----------------------------------------------------------------------------
// Geometry
//-----------------------------------------------------------------------------
static constexpr uint32_t kShadowBase    = 0x2000;          // 8192 -- first idx the shadow covers
static constexpr uint32_t kShadowEnd     = 0x4000;          // 16384 exclusive (matches dedi kNewArrayCapacity)
static constexpr uint32_t kShadowEntries = kShadowEnd - kShadowBase;
static constexpr uint32_t kEntrySize     = 16;              // CPrecacheItem = { u32 flags @0, model_t* @8 }
static constexpr uint32_t kShadowBytes   = kShadowEntries * kEntrySize;  // 0x20000 = 128 KiB

// S21 client items base + loader-ready flag -- inline CClientState
// globals;
// RVAs stable per S21 build.

//-----------------------------------------------------------------------------
// Runtime state
//-----------------------------------------------------------------------------
static uint8_t*  g_pShadowItems         = nullptr;  // heap shadow array for idx 8192..16383
static std::atomic<uint32_t> g_writerHits{0};
static std::atomic<uint32_t> g_readerHits{0};
static std::atomic<uint32_t> g_readerOOBRejects{0};

//-----------------------------------------------------------------------------
// Default 1; 0 + restart restores the native 8192-slot reader/writer.
//-----------------------------------------------------------------------------
static ConVar sdk_client_modelprecache_grow_s21(
	"sdk_client_modelprecache_grow_s21", "1", FCVAR_RELEASE,
	"S21 client modelprecache grow: server table can reach 16384; client "
	"inline items[] at CClientState+0x21348 is still 8192, so "
	"GetModelByIndex for idx>=8192 reads OOB into sibling precache arrays "
	"and binds high-idx assets to brush/sound stubs. Shadows idx "
	"8192..16383 on a heap buffer for writer+reader. Set 0 + restart to "
	"revert.");

//-----------------------------------------------------------------------------
// GetModelByIndex (vftable slot 1) and modelprecache OnStringChanged.
//-----------------------------------------------------------------------------
#define v_CL_GetModelByIndex   v_CL_GetModelByIndex_Grow
#define v_CL_StringChanged     v_CL_StringChanged_Grow
#define v_CL_ClientStateClear  v_CL_ClientStateClear_Grow
#define v_CL_SetModelByIndex   v_CL_SetModelByIndex_Grow

static uint8_t* s_pEvictCave = nullptr;
static uint8_t* s_pEvictLea = nullptr;
static uint8_t s_evictLeaOrig[7] = {};
static bool s_bEvictPatched = false;
alignas(16) static uint8_t s_evictSink[16] = {};


//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static void* AllocNearModule(uintptr_t nearAddr, size_t size)
{
	SYSTEM_INFO si = {};
	GetSystemInfo(&si);
	const uintptr_t gran = si.dwAllocationGranularity;
	uintptr_t addr = (nearAddr + gran - 1) & ~(uintptr_t)(gran - 1);
	for (int i = 0; i < 8192; ++i)
	{
		void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size,
			MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (p) return p;
		addr += gran;
	}
	return nullptr;
}

//-----------------------------------------------------------------------------
// Hook: writer (OnStringChanged for modelprecache)
//-----------------------------------------------------------------------------
static char* __fastcall Hook_CL_StringChanged(__int64 a1, __int64 table,
											   int idx, const char* name,
											   __int64 userdata, __int64 userdataLen)
{
	if (static_cast<uint32_t>(idx) < kShadowBase)
		return v_CL_StringChanged(a1, table, idx, name, userdata, userdataLen);
	if (static_cast<uint32_t>(idx) >= kShadowEnd)
		return nullptr;
	if (!g_pShadowItems)
		return nullptr;

	// Charm filter from the original: if the name starts with "mdl/props/charm"
	// we skip the write (the original's strstr check).
	if (name && strstr(name, "mdl/props/charm") == name)
		return nullptr;

	// OnStringChanged userdata is the 32-byte blob; SKIN/BODY/RENDER-BIN
	// must be forwarded or legendary variants resolve to the base model.
	const uintptr_t modBase = g_GameDll.GetModuleBase();
	if (!modBase) return nullptr;

	void** const modelInfoPP =
		reinterpret_cast<void**>(NetObs_Sym(NetObsSym_t::ModelInfoClient));
	if (!modelInfoPP) return nullptr;
	// ModelInfoClient is an ADDRESS-TAKEN global (the singleton itself lives
	// at that address as a vtable-like layout); index [4] holds the resolver
	// function pointer.
	void* const resolveFnPtr = modelInfoPP[4];
	if (!resolveFnPtr) return nullptr;

	// Build the 32-byte resolver blob: copy the engine-provided userdata in
	// when present (carries the SKIN/BODY/RENDER-BIN variant fields), then
	// overlay the name pointer at +8 per the original blob layout.
	uint8_t blob[32] = {};
	if (userdata && userdataLen >= 32)
	{
		memcpy(blob, reinterpret_cast<const void*>(userdata), 32);
	}
	else
	{
		// Fallback: legacy zero-init + name. Warn once so missing-variant
		// cases are visible in warning.log instead of silently regressing.
		static std::atomic<uint32_t> s_fallbackHits{0};
		if (s_fallbackHits.fetch_add(1, std::memory_order_relaxed) == 0)
		{
			Warning(eDLL_T::CLIENT,
				"[CLG-S21] WRITER fallback: idx=%d name='%s' userdata=%p "
				"len=%lld -- variant fields zeroed (legendary skin will "
				"resolve to base; further fallbacks suppressed)\n",
				idx, name ? name : "(null)", (void*)userdata,
				static_cast<long long>(userdataLen));
		}
	}
	// Always overlay name at +8 (defensive: matches original blob layout).
	*reinterpret_cast<uintptr_t*>(blob + 8) = reinterpret_cast<uintptr_t>(name);

	typedef __int64 (__fastcall* ResolveFn)(void* singleton, void* blob);
	const ResolveFn resolveFn = reinterpret_cast<ResolveFn>(resolveFnPtr);
	const __int64 modelPtr = resolveFn(modelInfoPP, blob);

	// Shadow-buffer slot for this idx.
	uint8_t* const   slot     = g_pShadowItems + (idx - kShadowBase) * kEntrySize;
	uint32_t* const  flags_p  = reinterpret_cast<uint32_t*>(slot);
	uintptr_t* const model_p  = reinterpret_cast<uintptr_t*>(slot + 8);

	// flags = (existing & 0xFFFFFFF8) | 1; keep low 3 bits if modelPtr.
	uint32_t flags = (*flags_p & 0xFFFFFFF8u) | 1u;
	*model_p = static_cast<uintptr_t>(modelPtr);
	*flags_p = flags;
	if (modelPtr)
		*flags_p = flags & 7u;

	const uint32_t n = g_writerHits.fetch_add(1, std::memory_order_relaxed);
	if (n < 8 || (n & 0xFFu) == 0)
	{
		DevMsg(eDLL_T::CLIENT,
			"[CLG-S21] writer SHADOW idx=%d name='%s' model=0x%llX #%u\n",
			idx, name ? name : "(null)",
			static_cast<unsigned long long>(modelPtr), n + 1);
	}

	// Original returns the model_t* (cast to char*); we mirror that.
	return reinterpret_cast<char*>(modelPtr);
}

//-----------------------------------------------------------------------------
// Hook: reader (CModelInfoClient::GetModelByIndex)
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_CL_GetModelByIndex(void* mi, int idx)
{
	if (static_cast<uint32_t>(idx) < kShadowBase)
		return v_CL_GetModelByIndex(mi, idx);
	if (static_cast<uint32_t>(idx) >= kShadowEnd)
	{
		g_readerOOBRejects.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}
	if (!g_pShadowItems)
		return 0;

	// Mirror the original's early-out on the stringtable ptr check.
	const uintptr_t modBase = g_GameDll.GetModuleBase();
	if (!modBase) return 0;
	const uintptr_t loaderPtrAddr = NetObs_Sym(NetObsSym_t::ModelPrecacheTablePtr);
	const uintptr_t loaderPtr = *reinterpret_cast<uintptr_t*>(loaderPtrAddr);
	if (loaderPtr == 0) return 0;

	uint8_t* const  slot  = g_pShadowItems + (idx - kShadowBase) * kEntrySize;
	uintptr_t const model = *reinterpret_cast<uintptr_t*>(slot + 8);
	if (model == 0) return 0;

	// Mirror the original's refcount bump.
	*reinterpret_cast<uint32_t*>(slot) += 8;

	const uint32_t n = g_readerHits.fetch_add(1, std::memory_order_relaxed);
	if (n < 8 || (n & 0x1FFu) == 0)
	{
		DevMsg(eDLL_T::CLIENT,
			"[CLG-S21] reader SHADOW idx=%d model=0x%llX #%u\n",
			idx, static_cast<unsigned long long>(model), n + 1);
	}
	return static_cast<__int64>(model);
}

static void Shadow_StoreModel(const uint32_t idx, const uintptr_t modelPtr)
{
	uint8_t* const slot = g_pShadowItems + (idx - kShadowBase) * kEntrySize;
	uint32_t flags = (*reinterpret_cast<uint32_t*>(slot) & 0xFFFFFFF9u) | 1u;
	*reinterpret_cast<uintptr_t*>(slot + 8) = modelPtr;
	*reinterpret_cast<uint32_t*>(slot) = modelPtr ? (flags & 7u) : flags;
}

static const char* ModelPrecache_GetString(const unsigned int idx)
{
	if (!v_CL_ModelPrecacheStringTableLoc)
		return nullptr;
	void* const obj = *v_CL_ModelPrecacheStringTableLoc;
	if (!obj)
		return nullptr;
	void** const vt = *reinterpret_cast<void***>(obj);
	if (!vt || !vt[3])
		return nullptr;
	using GetStringFn = const char* (__fastcall*)(void*, unsigned int);
	return reinterpret_cast<GetStringFn>(vt[3])(obj, idx);
}

static __int64 __fastcall Hook_CL_SetModelByIndex(unsigned int idx)
{
	if (idx < kShadowBase)
		return v_CL_SetModelByIndex(idx);
	if (idx >= kShadowEnd || !g_pShadowItems)
		return 0;

	const char* const name = ModelPrecache_GetString(idx);
	if (!name)
		return 0;

	void** const modelInfoPP =
		reinterpret_cast<void**>(NetObs_Sym(NetObsSym_t::ModelInfoClient));
	if (!modelInfoPP || !modelInfoPP[4])
		return 0;

	uint8_t blob[32] = {};
	*reinterpret_cast<uintptr_t*>(blob) = reinterpret_cast<uintptr_t>(name);

	typedef __int64 (__fastcall* ResolveFn)(void* singleton, void* blob);
	const ResolveFn resolveFn = reinterpret_cast<ResolveFn>(modelInfoPP[4]);
	const __int64 modelPtr = resolveFn(modelInfoPP, blob);
	Shadow_StoreModel(idx, static_cast<uintptr_t>(modelPtr));

	const uint32_t n = g_writerHits.fetch_add(1, std::memory_order_relaxed);
	if (n < 8 || (n & 0xFFu) == 0)
	{
		DevMsg(eDLL_T::CLIENT,
			"[CLG-S21] set-index SHADOW idx=%u name='%s' model=0x%llX #%u\n",
			idx, name, static_cast<unsigned long long>(modelPtr), n + 1);
	}
	return modelPtr;
}

static void EvictRedirect_Remove(void)
{
	if (s_bEvictPatched && s_pEvictLea)
		Mem_PatchCode(s_pEvictLea, s_evictLeaOrig, sizeof(s_evictLeaOrig));
	s_bEvictPatched = false;
	s_pEvictLea = nullptr;
	if (s_pEvictCave)
	{
		VirtualFree(s_pEvictCave, 0, MEM_RELEASE);
		s_pEvictCave = nullptr;
	}
}

static bool EvictRedirect_Install(void)
{
	CMemory leaMem = Module_FindPattern(g_GameDll,
		"48 8D 0D ?? ?? ?? ?? 33 DB 48 8B C5 48 C1 E0 04");
	s_pEvictLea = leaMem.RCast<uint8_t*>();
	if (!s_pEvictLea)
	{
		Warning(eDLL_T::CLIENT, "[CLG-S21] evict items lea unresolved\n");
		return false;
	}

	int32_t disp = 0;
	memcpy(&disp, s_pEvictLea + 3, 4);
	uint8_t* const items = s_pEvictLea + 7 + disp;
	memcpy(s_evictLeaOrig, s_pEvictLea, sizeof(s_evictLeaOrig));

	s_pEvictCave = Mem_AllocNearModule(g_GameDll, 128);
	if (!s_pEvictCave)
	{
		Warning(eDLL_T::CLIENT, "[CLG-S21] evict cave alloc failed\n");
		s_pEvictLea = nullptr;
		return false;
	}

	DWORD old = 0;
	VirtualProtect(s_pEvictCave, 128, PAGE_EXECUTE_READWRITE, &old);

	uint8_t* const resume = s_pEvictLea + 7;
	const uint64_t itemsAddr = reinterpret_cast<uint64_t>(items);
	const uint64_t sinkAddr = reinterpret_cast<uint64_t>(s_evictSink);
	const uint64_t shadowMinus = g_pShadowItems
		? reinterpret_cast<uint64_t>(g_pShadowItems) - (static_cast<uint64_t>(kShadowBase) * kEntrySize)
		: 0;

	uint8_t* p = s_pEvictCave;
	auto emitJmp = [&p](uint8_t* dest)
	{
		*p++ = 0xE9;
		const int32_t rel = static_cast<int32_t>(dest - (p + 4));
		memcpy(p, &rel, 4);
		p += 4;
	};
	auto emitMovRcx = [&p](uint64_t imm)
	{
		*p++ = 0x48; *p++ = 0xB9;
		memcpy(p, &imm, 8);
		p += 8;
	};

	// cmp ebp, 2000h / jb stock
	*p++ = 0x81; *p++ = 0xFD; *p++ = 0x00; *p++ = 0x20; *p++ = 0x00; *p++ = 0x00;
	*p++ = 0x72;
	uint8_t* const jbStock = p++;

	uint8_t* jaeDummy = nullptr;
	uint8_t* jmpDummy = nullptr;
	if (g_pShadowItems)
	{
		// cmp ebp, 4000h / jae dummy
		*p++ = 0x81; *p++ = 0xFD; *p++ = 0x00; *p++ = 0x40; *p++ = 0x00; *p++ = 0x00;
		*p++ = 0x73;
		jaeDummy = p++;
		emitMovRcx(shadowMinus);
		emitJmp(resume);
	}
	else
	{
		*p++ = 0xEB;
		jmpDummy = p++;
	}

	*jbStock = static_cast<uint8_t>(p - (jbStock + 1));
	emitMovRcx(itemsAddr);
	emitJmp(resume);

	if (jaeDummy)
		*jaeDummy = static_cast<uint8_t>(p - (jaeDummy + 1));
	if (jmpDummy)
		*jmpDummy = static_cast<uint8_t>(p - (jmpDummy + 1));
	*p++ = 0x48; *p++ = 0x8B; *p++ = 0xC5;
	*p++ = 0x48; *p++ = 0xC1; *p++ = 0xE0; *p++ = 0x04;
	emitMovRcx(sinkAddr);
	*p++ = 0x48; *p++ = 0x2B; *p++ = 0xC8;
	emitJmp(resume);

	FlushInstructionCache(GetCurrentProcess(), s_pEvictCave, 128);

	uint8_t jmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
	const int32_t rel = static_cast<int32_t>(s_pEvictCave - (s_pEvictLea + 5));
	memcpy(jmp + 1, &rel, 4);
	if (!Mem_PatchCode(s_pEvictLea, jmp, sizeof(jmp)))
	{
		Warning(eDLL_T::CLIENT, "[CLG-S21] evict lea patch failed\n");
		EvictRedirect_Remove();
		return false;
	}
	s_bEvictPatched = true;
	return true;
}

//-----------------------------------------------------------------------------
// Hook: CClientState::Clear
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_CL_ClientStateClear(void* clstate)
{
	const __int64 result = v_CL_ClientStateClear(clstate);
	if (g_pShadowItems)
	{
		memset(g_pShadowItems, 0, kShadowBytes);
		static std::atomic<uint32_t> s_clears{0};
		if (s_clears.fetch_add(1, std::memory_order_relaxed) == 0)
		{
			Msg(eDLL_T::CLIENT, "[CLG-S21] shadow cleared\n");
		}
	}
	return result;
}

//-----------------------------------------------------------------------------
// Stats accessor for the dump probe (sdk_dump_modelprecache_full).
//-----------------------------------------------------------------------------
void MdlPrecacheShadow_GetStats(MdlPrecacheShadowStats* out)
{
	out->pBase      = g_pShadowItems;
	out->shadowBase = kShadowBase;
	out->shadowEnd  = kShadowEnd;
	out->populated  = 0;
	if (!g_pShadowItems) return;
	for (uint32_t i = 0; i < kShadowEntries; ++i)
	{
		const uintptr_t modelPtr =
			*reinterpret_cast<const uintptr_t*>(g_pShadowItems + i * kEntrySize + 8);
		if (modelPtr) ++out->populated;
	}
}

// Do not mask the shared stringtable-overflow Error: it also guards
// ParticleEffectNames, weaponprecache, and decals.
//-----------------------------------------------------------------------------
// Detour lives here; GetAdr/GetFun/GetVar/GetCon are inline in the header.
//-----------------------------------------------------------------------------
void VModelPrecacheClientGrowS21::Detour(const bool bAttach) const
{
	if (bAttach)
	{
		if (!v_CL_GetModelByIndex || !v_CL_StringChanged)
		{
			Warning(eDLL_T::CLIENT,
				"[CLG-S21] pattern unresolved (reader=%p writer=%p); "
				"shadow NOT attached.\n",
				v_CL_GetModelByIndex, v_CL_StringChanged);
			return;
		}

		if (sdk_client_modelprecache_grow_s21.GetBool())
		{
			const uintptr_t modBase = g_GameDll.GetModuleBase();
			const uintptr_t modSize = g_GameDll.GetModuleSize();
			g_pShadowItems = static_cast<uint8_t*>(
				AllocNearModule(modBase + modSize, kShadowBytes + HeapCanary::kTailBytes));
			if (!g_pShadowItems)
			{
				Warning(eDLL_T::CLIENT,
					"[CLG-S21] VirtualAlloc near module failed -- idx>=8192 dropped, no shadow\n");
			}
			else
				HeapCanary::RegisterTail("clg-s21-modelprecache", g_pShadowItems, kShadowBytes);
		}
		else
		{
			Msg(eDLL_T::CLIENT,
				"[CLG-S21] grow off -- hooks still reject idx>=8192 (stock items[] is 8192)\n");
		}

		DetourSetup(&v_CL_GetModelByIndex, &Hook_CL_GetModelByIndex, true);
		DetourSetup(&v_CL_StringChanged,   &Hook_CL_StringChanged,   true);
		if (v_CL_ClientStateClear)
			DetourSetup(&v_CL_ClientStateClear, &Hook_CL_ClientStateClear, true);
		else
		{
			Warning(eDLL_T::CLIENT,
				"[CLG-S21] Clear pattern unresolved -- shadow will not "
				"zero on map change.\n");
		}

		if (v_CL_SetModelByIndex)
			DetourSetup(&v_CL_SetModelByIndex, &Hook_CL_SetModelByIndex, true);
		else
		{
			Warning(eDLL_T::CLIENT,
				"[CLG-S21] SetModelByIndex pattern unresolved -- leftover "
				"writer still indexes the 8192-slot array.\n");
		}

		if (EvictRedirect_Install())
		{
			Msg(eDLL_T::CLIENT,
				"[CLG-S21] evict items lea redirected (stock/shadow/sink)\n");
		}

		Msg(eDLL_T::CLIENT,
			"[CLG-S21] writer/reader hooked; shadow=%p (%u entries). "
			"idx>=%u never reaches the 8192-slot inline array.\n",
			g_pShadowItems, kShadowEntries, kShadowBase);
	}
	else
	{
		if (v_CL_GetModelByIndex)
			DetourSetup(&v_CL_GetModelByIndex, &Hook_CL_GetModelByIndex, false);
		if (v_CL_StringChanged)
			DetourSetup(&v_CL_StringChanged,   &Hook_CL_StringChanged,   false);
		if (v_CL_ClientStateClear)
			DetourSetup(&v_CL_ClientStateClear, &Hook_CL_ClientStateClear, false);
		if (v_CL_SetModelByIndex)
			DetourSetup(&v_CL_SetModelByIndex, &Hook_CL_SetModelByIndex, false);
		EvictRedirect_Remove();
		if (g_pShadowItems)
		{
			HeapCanary::Unregister(g_pShadowItems);
			VirtualFree(g_pShadowItems, 0, MEM_RELEASE);
			g_pShadowItems = nullptr;
		}
		Msg(eDLL_T::CLIENT, "[CLG-S21] detached + shadow buffer freed.\n");
	}
}
