//=============================================================================
//
// Purpose: expand client scriptNetCategories 5 -> 7 so SNDC_GLOBAL_NON_REWIND can register.
// Stock 5-entry array cannot grow in place; VirtualAlloc a 7-entry copy.
//
//=============================================================================
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "game/server/sndc_alloc.h"
#include "game/shared/heap_canary.h"

#include <cstdint>
#include <cstring>

namespace {

constexpr int SNDC_CLI_STRIDE     = 264;  // 0x108
constexpr int SNDC_ORIG_CATS      = 5;
constexpr int SNDC_EXPANDED_CATS  = 7;
constexpr int SNDC_NTYPES         = 5;

// Per-category limits {Bool, Range, BigInt, Time, Entity}. A limit above the DT array overflows backing memory.
static const int s_s21Limits[SNDC_EXPANDED_CATS][SNDC_NTYPES] = {
	{18, 34, 18, 26, 10}, // 0 GLOBAL 
	{18, 30, 34, 14,  6}, // 1 PLAYER_GLOBAL 
	{32, 30,  6, 10, 22}, // 2 PLAYER_EXCLUSIVE 
	{10, 18,  4, 10,  6}, // 3 TITAN_SOUL 
	{ 5,  4, 14,  3,  3}, // 4 DEATH_BOX 
	{18, 18, 10, 18,  6}, // 5 NON_REWIND 
	{ 0,  0,  0,  0,  0}, // 6 spare
};

//-----------------------------------------------------------------------------
// Resolved at GetVar / allocated at Detour
//-----------------------------------------------------------------------------
	static uint8_t* g_pOrigClientCats = nullptr;   // engine client cat array
static uint8_t* g_pExpandedCats   = nullptr;   // VirtualAlloc'd 7-entry copy
static uint8_t* g_pCatBoundSrv    = nullptr;
static uint8_t* g_pCatBoundCli    = nullptr;

// CLIENT AllocateInternalVar 
typedef __int64 (__fastcall* PFN_AllocCli)(__int64 catStruct, int type, const char* typeName);
static PFN_AllocCli v_AllocInternalVarCli = nullptr;

// ClientInit 
static __int64 (__fastcall* v_ClientInit)(__int64 a1) = nullptr;

// AllocateInternalVar: handle all cats here so the engine OOB printf is never hit.
static __int64 __fastcall Hook_AllocInternalVarCli(__int64 catStruct, int type, const char* /*typeName*/)
{
	if (type < 0 || type >= SNDC_NTYPES || !catStruct)
		return -1;

	// Read limits from s_s21Limits at alloc time. ClientInit vs RegisterNetworkedVariable race *pMax.
	int cat = -1;
	if (g_pExpandedCats
		&& catStruct >= (__int64)g_pExpandedCats
		&& catStruct < (__int64)(g_pExpandedCats + SNDC_EXPANDED_CATS * SNDC_CLI_STRIDE))
	{
		cat = (int)(((__int64)catStruct - (__int64)g_pExpandedCats) / SNDC_CLI_STRIDE);
	}
	else if (g_pOrigClientCats
		&& catStruct >= (__int64)g_pOrigClientCats
		&& catStruct < (__int64)(g_pOrigClientCats + SNDC_ORIG_CATS * SNDC_CLI_STRIDE))
	{
		cat = (int)(((__int64)catStruct - (__int64)g_pOrigClientCats) / SNDC_CLI_STRIDE);
	}

	int* pCount = (int*)(catStruct + 0x14 + 4LL * type);
	int count = *pCount;
	int max = (cat >= 0 && cat < SNDC_EXPANDED_CATS)
		? s_s21Limits[cat][type]
		: *(int*)(catStruct + 4LL * type); // unrecognized catStruct -- fall back to whatever is stored

	if (count < max)
	{
		*pCount = count + 1;
		return count;
	}

	Warning(eDLL_T::ENGINE,
		"[SNDC] AllocateInternalVar(cli) overflow: cat=%d type=%d count=%d max=%d\n",
		cat, type, count, max);
	return -1;
}

// ClientInit: re-prime cat 5/6 limits after the engine memset zeroes only entries 0-4.
static __int64 __fastcall Hook_ClientInit(__int64 a1)
{
	__int64 result = v_ClientInit(a1);

	// ClientInit zeroes g_pOrigClientCats, not g_pExpandedCats. Re-prime the live array too.
	if (g_pOrigClientCats)
	{
		for (int c = 0; c < SNDC_ORIG_CATS; ++c)
		{
			uint8_t* entry = g_pOrigClientCats + c * SNDC_CLI_STRIDE;
			for (int t = 0; t < SNDC_NTYPES; ++t)
				*(int*)(entry + 4 * t) = s_s21Limits[c][t];
		}
	}

	// The original memset'd entries 0-4 (0x528 bytes) of the expanded copy
	// and wrote S3 limits. Re-prime ALL entries with S21 limits (the SDK is
	// the authority). Entries 5-6 also get their counts zeroed.
	if (g_pExpandedCats)
	{
		for (int c = 0; c < SNDC_EXPANDED_CATS; ++c)
		{
			uint8_t* entry = g_pExpandedCats + c * SNDC_CLI_STRIDE;
			if (c >= SNDC_ORIG_CATS)
				memset(entry, 0, SNDC_CLI_STRIDE); // zero counts+data for cats 5-6
			// Overwrite max with S21 limits (cats 0-4 keep engine-init'd counts)
			for (int t = 0; t < SNDC_NTYPES; ++t)
				*(int*)(entry + 4 * t) = s_s21Limits[c][t];
		}
	}
	return result;
}

//-----------------------------------------------------------------------------
static uintptr_t FindNextMatch(uintptr_t after, const uint8_t* needle, size_t len)
{
	uintptr_t end = g_GameDll.GetModuleBase() + g_GameDll.GetModuleSize();
	for (uintptr_t p = after + 1; p + len < end; ++p)
		if (memcmp((const void*)p, needle, len) == 0) return p;
	return 0;
}

//-----------------------------------------------------------------------------
// VirtualAlloc a buffer near the module (within ~256 MB) for safe RIP-relative.
//-----------------------------------------------------------------------------
static uint8_t* AllocNearModule(size_t size)
{
	uintptr_t base = g_GameDll.GetModuleBase();
	for (int dir = -1; dir <= 1; dir += 2)
	{
		for (int attempt = 1; attempt < 4096; ++attempt)
		{
			uintptr_t addr = (base + dir * attempt * 0x10000ULL) & ~0xFFFFULL;
			if (dir < 0 && addr > base) continue; // underflow guard
			int64_t dist = (int64_t)addr - (int64_t)base;
			if (dist > 0x10000000LL || dist < -0x10000000LL) break; // +-256 MB
			MEMORY_BASIC_INFORMATION mbi;
			if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)
				&& mbi.State == MEM_FREE && mbi.RegionSize >= size)
			{
				void* p = VirtualAlloc((void*)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				if (p) return (uint8_t*)p;
			}
		}
	}
	return nullptr;
}

// Re-point every .text LEA into [origBase, origEnd). Scan .text only.
static int PatchLeaRefs(uint8_t* origBase, int origBytes, uint8_t* newBase)
{
	uintptr_t oBase = (uintptr_t)origBase;
	uintptr_t oEnd  = oBase + origBytes;
	uintptr_t nBase = (uintptr_t)newBase;

	// Scan only.text (roughly the first ~18 MB, well under the data sections).
	// Use the module's entry point as a proxy for.text extent; conservatively
	// scan up to 1/4 of the module size (covers.text, not.data/.bss).
	uintptr_t modBase = g_GameDll.GetModuleBase();
	uintptr_t scanEnd = modBase + g_GameDll.GetModuleSize() / 4;
	uint8_t*  text    = (uint8_t*)modBase;
	int patched = 0;

	for (uintptr_t off = 0; (uintptr_t)(text + off + 7) < scanEnd; ++off)
	{
		uint8_t* ip = text + off;
		if ((ip[0] == 0x48 || ip[0] == 0x4C) && ip[1] == 0x8D && (ip[2] & 0xC7) == 0x05)
		{
			int32_t disp; memcpy(&disp, ip + 3, 4);
			uintptr_t resolved = (uintptr_t)(ip + 7) + disp;
			if (resolved >= oBase && resolved < oEnd)
			{
				uintptr_t newTarget = nBase + (resolved - oBase);
				int64_t newDisp64 = (int64_t)newTarget - (int64_t)(ip + 7);
				if (newDisp64 > INT32_MAX || newDisp64 < INT32_MIN)
				{
					Warning(eDLL_T::ENGINE, "[SNDC] LEA disp overflow at 0x%p\n", (void*)ip);
					continue;
				}
				int32_t newDisp = (int32_t)newDisp64;
				DWORD oldProt;
				VirtualProtect(ip + 3, 4, PAGE_EXECUTE_READWRITE, &oldProt);
				memcpy(ip + 3, &newDisp, 4);
				VirtualProtect(ip + 3, 4, oldProt, &oldProt);
				patched++;
			}
		}
	}
	return patched;
}

} // namespace

//-----------------------------------------------------------------------------
void VSNDCAllocHook::GetAdr(void) const
{
	LogFunAdr("ClientInit", v_ClientInit);
	LogFunAdr("AllocInternalVar(cli)", v_AllocInternalVarCli);
}

void VSNDCAllocHook::GetFun(void) const
{
	// ClientInit
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B 15 ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? C7 41 18 FF FF 7F FF")
		.GetPtr(v_ClientInit);

	// AllocateInternalVar (CLIENT): (second match of the pattern)
	static const uint8_t kAlloc[] = { 0x48,0x83,0xEC,0x28, 0x48,0x63,0xC2, 0x4D,0x8B,0xD8, 0x8B,0x54,0x81,0x14 };
	CMemory first = Module_FindPattern(g_GameDll, "48 83 EC 28 48 63 C2 4D 8B D8 8B 54 81 14");
	if (first)
		v_AllocInternalVarCli = (PFN_AllocCli)FindNextMatch(first.GetPtr(), kAlloc, sizeof(kAlloc));
}

void VSNDCAllocHook::GetVar(void) const
{
	// Resolve the client cat array from ClientInit+0x91 (lea rcx, [rip+disp32])
	if (v_ClientInit)
	{
		CMemory leaSite = CMemory((uintptr_t)v_ClientInit).Offset(0x91);
		uint8_t* p = leaSite.RCast<uint8_t*>();
		if (p && p[0] == 0x48 && p[1] == 0x8D && (p[2] & 0xC7) == 0x05)
			g_pOrigClientCats = leaSite.ResolveRelativeAddress(0x3, 0x7).RCast<uint8_t*>();
	}
	if (!g_pOrigClientCats)
		g_pOrigClientCats = (uint8_t*)(g_GameDll.GetModuleBase() + 0x295091F0);
	Warning(eDLL_T::ENGINE, "[SNDC] client cat array = 0x%p\n", (void*)g_pOrigClientCats);

	// Category bound sites
	static const uint8_t kBound[] = { 0x41,0x83,0xFF,0x04, 0x0F,0x87 };
	CMemory bnd1 = Module_FindPattern(g_GameDll, "41 83 FF 04 0F 87");
	g_pCatBoundSrv = bnd1.RCast<uint8_t*>();
	if (bnd1) g_pCatBoundCli = (uint8_t*)FindNextMatch(bnd1.GetPtr(), kBound, sizeof(kBound));
	Warning(eDLL_T::ENGINE, "[SNDC] catBound: srv=0x%p cli=0x%p\n",
		(void*)g_pCatBoundSrv, (void*)g_pCatBoundCli);
}

void VSNDCAllocHook::Detour(const bool bAttach) const
{
	// --- Expand client cat array (VirtualAlloc near module + LEA-patch) ---
	if (bAttach && g_pOrigClientCats && !g_pExpandedCats)
	{
		const int expandedBytes = SNDC_EXPANDED_CATS * SNDC_CLI_STRIDE;
		g_pExpandedCats = AllocNearModule(expandedBytes + (int)HeapCanary::kTailBytes);
		if (g_pExpandedCats)
		{
			memset(g_pExpandedCats, 0, expandedBytes);
			HeapCanary::RegisterTail("sndc-cat-array", g_pExpandedCats, expandedBytes);
			memcpy(g_pExpandedCats, g_pOrigClientCats, SNDC_ORIG_CATS * SNDC_CLI_STRIDE);
			// Set S21 limits for ALL cats (0-6) -- SDK is the authority.
			for (int c = 0; c < SNDC_EXPANDED_CATS; ++c)
			{
				uint8_t* entry = g_pExpandedCats + c * SNDC_CLI_STRIDE;
				for (int t = 0; t < SNDC_NTYPES; ++t)
					*(int*)(entry + 4 * t) = s_s21Limits[c][t];
			}
			int refs = PatchLeaRefs(g_pOrigClientCats, SNDC_ORIG_CATS * SNDC_CLI_STRIDE, g_pExpandedCats);
			Warning(eDLL_T::ENGINE,
				"[SNDC] client cat array expanded: 5->7 @ 0x%p (near-alloc), %d LEA refs patched\n",
				(void*)g_pExpandedCats, refs);
		}
		else
			Warning(eDLL_T::ENGINE, "[SNDC] FATAL: VirtualAlloc near module failed\n");
	}

	// --- Category bound patches ---
	if (bAttach)
	{
		auto patchBound = [](uint8_t* site, const char* label) {
			if (!site) { Warning(eDLL_T::ENGINE, "[SNDC] %s: unresolved\n", label); return; }
			if (site[3] == 0x04) {
				DWORD p; VirtualProtect(site+3, 1, PAGE_EXECUTE_READWRITE, &p);
				site[3] = 0x06; VirtualProtect(site+3, 1, p, &p);
				Warning(eDLL_T::ENGINE, "[SNDC] %s: cmp r15d, 4 -> 6\n", label);
			} else if (site[3] >= 0x05)
				Warning(eDLL_T::ENGINE, "[SNDC] %s: already >= 5 (0x%02X)\n", label, site[3]);
			else
				Warning(eDLL_T::ENGINE, "[SNDC] %s: unexpected 0x%02X\n", label, site[3]);
		};
		patchBound(g_pCatBoundSrv, "catBound srv");
		patchBound(g_pCatBoundCli, "catBound cli");
	}

	// --- Hook AllocateInternalVar (CLIENT) ---
	if (v_AllocInternalVarCli)
	{
		DetourSetup(&v_AllocInternalVarCli, &Hook_AllocInternalVarCli, bAttach);
		if (bAttach) Warning(eDLL_T::ENGINE, "[SNDC] hooked AllocInternalVar(cli) @ 0x%p\n", (void*)v_AllocInternalVarCli);
	}

	// --- Hook ClientInit (re-prime cat 5/6 each level) ---
	if (v_ClientInit)
	{
		DetourSetup(&v_ClientInit, &Hook_ClientInit, bAttach);
		if (bAttach) Warning(eDLL_T::ENGINE, "[SNDC] hooked ClientInit @ 0x%p\n", (void*)v_ClientInit);
	}
}
