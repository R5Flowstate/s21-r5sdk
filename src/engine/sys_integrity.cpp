//=============================================================================//
//
// Purpose: Origin-online weapon crash: disarm integrity telemetry; if
// weapon const-types load empty, refill from platform\scripts\weapons\constant_types.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_bridge_addrs.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/commandline.h"
#include "tier0/memaddr.h"
#include "tier0/memvalidate.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "sys_integrity.h"
#include "ebisusdk/EbisuSDK.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <cstdint>
#include <cstdio>

// -- (1) integrity/telemetry neuter ---------------------

typedef void* (__fastcall* PFN_IntegrityInit)(size_t);
static PFN_IntegrityInit v_IntegrityInit_S21 = nullptr;
static volatile uint64_t* g_pIntegrityEnable_S21 = nullptr;

// FCVAR_RELEASE: behavior lever. Default 1 = neuter on for this product.
// Set 0 to leave stock integrity armed.
static ConVar sdk_integrity_neuter("sdk_integrity_neuter", "1", FCVAR_RELEASE,
	"Disarm the engine EOS/EAC integrity net (caller stack-walk + PIN telemetry). "
	"Default 1 = on for this product. 0 = leave stock integrity armed. "
	"Not the weapon fix (const-types path).");

// Private/lab launch-arg test (-sdk_private / -devsdk); currently unused.
bool SDK_IsPrivateLabMode(void)
{
	if (!CommandLine())
		return false;
	// Whole-token flags only (CheckParm is token-exact).
	if (CommandLine()->CheckParm("-sdk_private"))
		return true;
	if (CommandLine()->CheckParm("-devsdk"))
		return true;
	return false;
}

bool SDK_IntegrityNeuterActive(void)
{
	return sdk_integrity_neuter.GetBool();
}

static void IntegrityNeuter_AnnounceIfActive(void)
{
	static volatile LONG s_announced = 0;
	if (!SDK_IntegrityNeuterActive())
		return;
	if (InterlockedCompareExchange(&s_announced, 1, 0) != 0)
		return;
	Msg(eDLL_T::ENGINE,
		"[SEC-BYPASS] integrity neuter ACTIVE (sdk_integrity_neuter 1) -- "
		"EOS/EAC integrity enable forced off.\n");
}

// -- (2) weapon const-types repopulate --------------------
// .data RVAs differ DX11 vs DX12.


// FUNCTIONS -> pattern-resolved (function RVAs drift unpacked-vs-runtime).
typedef int64_t (__fastcall* PFN_ConstTypesLoad)(void);
typedef uintptr_t (__fastcall* PFN_AllocPooledString)(const char*);
typedef int      (__fastcall* PFN_GetEnumTypeIndex)(const char*);
static PFN_ConstTypesLoad    v_ConstTypesLoad_S21    = nullptr;
static PFN_AllocPooledString v_AllocPooledString_S21 = nullptr;
static PFN_GetEnumTypeIndex  v_GetEnumTypeIndex_S21  = nullptr;

static int32_t* g_pWeaponConstReg_S21 = nullptr;         // [6*8]=AmmoPoolType_t, [6*7]=eWeaponCooldownType
static uint8_t* g_pConstGuard_S21     = nullptr;
static const uintptr_t* g_pConstTypesTable_S21 = nullptr;

//-----------------------------------------------------------------------------
static uint64_t DisarmIntegrityEnable(void)
{
	if (!g_pIntegrityEnable_S21)
		return static_cast<uint64_t>(-1);
	const uint64_t was = *g_pIntegrityEnable_S21;
	*g_pIntegrityEnable_S21 = 0;
	return was;
}

static void* __fastcall Hook_IntegrityInit_S21(size_t a1)
{
	void* const ret = v_IntegrityInit_S21 ? v_IntegrityInit_S21(a1) : nullptr;
	if (SDK_IntegrityNeuterActive())
	{
		IntegrityNeuter_AnnounceIfActive();
		const uint64_t was = DisarmIntegrityEnable();
		static volatile LONG s_log = 0;
		if (InterlockedIncrement(&s_log) <= 4)
			Msg(eDLL_T::ENGINE,
				"[INTEGRITY-NEUTER] bootstrap ran post-install; enable %llu -> 0\n",
				(unsigned long long)was);
	}
	return ret;
}

//-----------------------------------------------------------------------------
// Read platform\<rel> into buf (NUL-terminated). Returns false on any failure.
//-----------------------------------------------------------------------------
static bool CT_ReadDisk(const char* rel, char* buf, DWORD cap, DWORD* outLen)
{
	char disk[1024];
	if (_snprintf_s(disk, sizeof(disk), _TRUNCATE, "platform\\%s", rel) < 0)
		return false;
	const HANDLE h = CreateFileA(disk, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD rd = 0;
	const BOOL ok = ReadFile(h, buf, cap - 1, &rd, nullptr);
	CloseHandle(h);
	if (!ok)
		return false;
	buf[rd] = '\0';
	*outLen = rd;
	return true;
}

// Next token: whitespace-delimited, with // line + /* */ block comments and
// "quoted" spans -- matches the engine ParseFile behaviour for this content.
static bool CT_NextToken(const char*& p, const char* end, char* tok, size_t cap)
{
	for (;;)
	{
		while (p < end && static_cast<unsigned char>(*p) <= ' ') ++p;
		if (p + 1 < end && p[0] == '/' && p[1] == '/')
		{ while (p < end && *p != '\n') ++p; continue; }
		if (p + 1 < end && p[0] == '/' && p[1] == '*')
		{ p += 2; while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p; if (p + 1 < end) p += 2; continue; }
		break;
	}
	if (p >= end)
		return false;
	size_t n = 0;
	if (*p == '"')
	{
		++p;
		while (p < end && *p != '"' && n + 1 < cap) tok[n++] = *p++;
		if (p < end) ++p;
	}
	else
	{
		while (p < end && static_cast<unsigned char>(*p) > ' ' && n + 1 < cap) tok[n++] = *p++;
	}
	tok[n] = '\0';
	return n > 0;
}

static int CT_RegCount(int idx)
{
	if (!g_pWeaponConstReg_S21)
		return -1;
	return g_pWeaponConstReg_S21[6 * idx];
}

// dataStruct u16 count. Teardown zeroes this without clearing registry dwords.
static int CT_DataStructCount(int tableEntry)
{
	if (!g_pConstTypesTable_S21 || tableEntry < 0 || tableEntry > 1)
		return -1;
	__try
	{
		const uintptr_t* const base =
			g_pConstTypesTable_S21 + 5 * tableEntry;
		char* const dataStruct = reinterpret_cast<char*>(base[1]);
		if (!dataStruct)
			return -1;
		return static_cast<int>(*reinterpret_cast<uint16_t*>(dataStruct));
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static bool CT_NeedsRepopulate(void)
{
	// Registry counts (idx 8 / 7) OR live token arrays wiped by teardown.
	if (CT_RegCount(8) <= 0 || CT_RegCount(7) <= 0)
		return true;
	if (CT_DataStructCount(0) <= 0 || CT_DataStructCount(1) <= 0)
		return true;
	return false;
}

//-----------------------------------------------------------------------------
// Table entry N at table+5*N: filename, enumTypeName at base[-2], dataStruct at base[1].
//-----------------------------------------------------------------------------
static void CT_Repopulate(void)
{
	if (!g_pConstTypesTable_S21 || !v_AllocPooledString_S21 ||
		!v_GetEnumTypeIndex_S21 || !g_pWeaponConstReg_S21)
		return;

	const uintptr_t* const T = g_pConstTypesTable_S21;
	for (int e = 0; e < 2; ++e)
	{
		__try
		{
			const uintptr_t* const base = T + 5 * e;
			const char* const enumName  = reinterpret_cast<const char*>(base[-2]);
			const char* const filename  = reinterpret_cast<const char*>(base[0]);
			char* const       dataStruct= reinterpret_cast<char*>(base[1]);
			const unsigned     maxCount = static_cast<unsigned>(base[2] & 0xFFFF);
			if (!enumName || !filename || !dataStruct || maxCount == 0 || maxCount > 4096)
				continue;

			static char fileBuf[8192];
			DWORD flen = 0;
			if (!CT_ReadDisk(filename, fileBuf, sizeof(fileBuf), &flen))
				continue;

			const char* p = fileBuf;
			const char* const end = fileBuf + flen;
			char tok[128];
			unsigned i = 0;
			while (i < maxCount && CT_NextToken(p, end, tok, sizeof(tok)))
			{
				const uintptr_t pooled = v_AllocPooledString_S21(tok);
				*reinterpret_cast<uintptr_t*>(dataStruct + 8 + 8 * i) = pooled;
				++i;
			}
			*reinterpret_cast<uint16_t*>(dataStruct) = static_cast<uint16_t>(i);

			const int idx = v_GetEnumTypeIndex_S21(enumName);
			if (idx >= 0 && idx < 64)
				g_pWeaponConstReg_S21[6 * idx] = static_cast<int32_t>(i);

			// Log every repopulate for a while -- changelevel path must show up.
			static volatile LONG s_log = 0;
			if (InterlockedIncrement(&s_log) <= 32)
				Msg(eDLL_T::ENGINE,
					"[WEAP-CONST-FIX] repopulated '%s' idx=%d count=%u (max %u) "
					"from disk (native empty or post-teardown dataStruct wipe)\n",
					enumName, idx, i, maxCount);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { /* skip this entry, try the other */ }
	}

	// Latch parse-once; Hook_ConstTypesLoad force-clears it on the next connect.
	if (g_pConstGuard_S21)
		*g_pConstGuard_S21 = 1;
}

static int64_t __fastcall Hook_ConstTypesLoad_S21(void)
{
	// Force parse-once to 0 so reconnects re-run instead of no-op'ing.
	if (g_pConstGuard_S21)
		*g_pConstGuard_S21 = 0;

	const int64_t r = v_ConstTypesLoad_S21 ? v_ConstTypesLoad_S21() : 0;

	// AmmoPoolType_t = idx 8, eWeaponCooldownType = idx 7.
	if (CT_NeedsRepopulate())
	{
		Msg(eDLL_T::ENGINE,
			"[WEAP-CONST-FIX] need repopulate after load: reg8=%d reg7=%d "
			"ds0=%d ds1=%d\n",
			CT_RegCount(8), CT_RegCount(7),
			CT_DataStructCount(0), CT_DataStructCount(1));
		CT_Repopulate();
	}

	return r;
}

void WeapConst_EnsurePopulated_S21(void)
{
	if (!g_pWeaponConstReg_S21 || !g_pConstTypesTable_S21)
		return;
	if (!CT_NeedsRepopulate())
		return;
	Msg(eDLL_T::ENGINE,
		"[WEAP-CONST-FIX] ensure (changelevel/late): reg8=%d reg7=%d "
		"ds0=%d ds1=%d -- repopulating\n",
		CT_RegCount(8), CT_RegCount(7),
		CT_DataStructCount(0), CT_DataStructCount(1));
	CT_Repopulate();
}

//-----------------------------------------------------------------------------
void VIntegrityNeuterS21::GetAdr(void) const
{
	LogFunAdr("IntegrityInit", v_IntegrityInit_S21);
	LogFunAdr("ConstTypesLoad", v_ConstTypesLoad_S21);
	LogVarAdr("IntegrityEnable",
		reinterpret_cast<const void*>(const_cast<uint64_t*>(g_pIntegrityEnable_S21)));
}

void VIntegrityNeuterS21::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 89 4C 24 08 55 53 56 57 41 55 41 56 48 8D AC 24 B8 F1 FF FF "
		"48 81 EC 48 0F 00 00 45 33 ED C7 05 ?? ?? ?? ?? FF FF 00 00")
		.GetPtr(v_IntegrityInit_S21);
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 81 EC 18 01 00 00 80 3D ?? ?? ?? ?? 00 0F 85")
		.GetPtr(v_ConstTypesLoad_S21);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B")
		.GetPtr(v_AllocPooledString_S21);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 4C 8B D9 4C 8D 0D ?? ?? ?? ?? 45 33 D2 48 8D 1D "
		"?? ?? ?? ?? 0F 1F 80")
		.GetPtr(v_GetEnumTypeIndex_S21);

	if (!v_IntegrityInit_S21)
		Warning(eDLL_T::ENGINE, "[INTEGRITY-NEUTER] unresolved\n");
	if (!v_ConstTypesLoad_S21 || !v_AllocPooledString_S21 || !v_GetEnumTypeIndex_S21)
		Warning(eDLL_T::ENGINE,
			"[WEAP-CONST-FIX] pattern(s) unresolved (load=%p alloc=%p idx=%p) -- "
			"const-types repopulate DISABLED\n",
			(void*)v_ConstTypesLoad_S21, (void*)v_AllocPooledString_S21,
			(void*)v_GetEnumTypeIndex_S21);
}

void VIntegrityNeuterS21::GetVar(void) const
{
	// Fixed r5apex .data: Mem_InModule once at install, then plain null-checked access.
	void* const pEnable = reinterpret_cast<void*>(NetObs_Sym(NetObsSym_t::IntegrityEnable));
	if (Mem_InModule(g_GameDll, pEnable, sizeof(uint64_t)))
		g_pIntegrityEnable_S21 = static_cast<volatile uint64_t*>(pEnable);
	else
	{
		g_pIntegrityEnable_S21 = nullptr;
		Warning(eDLL_T::ENGINE,
			"[INTEGRITY-NEUTER] IntegrityEnable RVA outside module -- neuter disabled\n");
	}

	// 6 dwords per enum idx; CT_Repopulate may write idx < 64; CT_RegCount uses 7/8.
	void* const pReg = reinterpret_cast<void*>(NetObs_Sym(NetObsSym_t::WeaponConstReg));
	if (Mem_InModule(g_GameDll, pReg, 6 * 64 * sizeof(int32_t)))
		g_pWeaponConstReg_S21 = static_cast<int32_t*>(pReg);
	else
	{
		g_pWeaponConstReg_S21 = nullptr;
		Warning(eDLL_T::ENGINE,
			"[WEAP-CONST-FIX] WeaponConstReg RVA outside module -- repopulate disabled\n");
	}

	void* const pGuard = reinterpret_cast<void*>(NetObs_Sym(NetObsSym_t::ConstGuard));
	if (Mem_InModule(g_GameDll, pGuard, sizeof(uint8_t)))
		g_pConstGuard_S21 = static_cast<uint8_t*>(pGuard);
	else
	{
		g_pConstGuard_S21 = nullptr;
		Warning(eDLL_T::ENGINE,
			"[WEAP-CONST-FIX] ConstGuard RVA outside module\n");
	}

	// Two 5-qword entries; entry0 base[-2] needs 2 qwords before table base.
	void* const pTableHead = reinterpret_cast<void*>(
		NetObs_Sym(NetObsSym_t::ConstTypesTable) - 2 * sizeof(uintptr_t));
	if (Mem_InModule(g_GameDll, pTableHead, (2 + 5 * 2) * sizeof(uintptr_t)))
		g_pConstTypesTable_S21 = reinterpret_cast<const uintptr_t*>(
			NetObs_Sym(NetObsSym_t::ConstTypesTable));
	else
	{
		g_pConstTypesTable_S21 = nullptr;
		Warning(eDLL_T::ENGINE,
			"[WEAP-CONST-FIX] ConstTypesTable RVA outside module -- repopulate disabled\n");
	}
}

void VIntegrityNeuterS21::Detour(const bool bAttach) const
{
	if (bAttach && SDK_IntegrityNeuterActive())
	{
		IntegrityNeuter_AnnounceIfActive();
		const uint64_t was = DisarmIntegrityEnable();
		Msg(eDLL_T::ENGINE,
			"[INTEGRITY-NEUTER] install: enable %llu -> 0 @ %p (integrity PIN "
			"telemetry disarmed)\n",
			(unsigned long long)was, (void*)g_pIntegrityEnable_S21);
	}
	else if (bAttach)
	{
		Msg(eDLL_T::ENGINE,
			"[INTEGRITY-NEUTER] install: left ARMED (sdk_integrity_neuter 0)\n");
	}

	// IntegrityInit always attaches; the hook body no-ops when the lever is off.
	if (v_IntegrityInit_S21)
		DetourSetup(&v_IntegrityInit_S21, &Hook_IntegrityInit_S21, bAttach);
	if (v_ConstTypesLoad_S21)
		DetourSetup(&v_ConstTypesLoad_S21, &Hook_ConstTypesLoad_S21, bAttach);
}
