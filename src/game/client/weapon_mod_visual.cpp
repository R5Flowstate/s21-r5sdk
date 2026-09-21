//=============================================================================//
//
// Purpose: weapon_mod_visual.h implementation. See that header.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/platform.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "weapon_mod_visual.h"

//-----------------------------------------------------------------------------
// ConVars
//-----------------------------------------------------------------------------
static ConVar sdk_weap_mod_bodygroup("sdk_weap_mod_bodygroup", "1",
	FCVAR_RELEASE,
	"After RecalcMods, call RequestBodygroupUpdate so held-weapon attachment "
	"bodygroups apply without a holster/re-equip.");

static ConVar sdk_weap_mod_bodygroup_log("sdk_weap_mod_bodygroup_log", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log RecalcMods -> RequestBodygroupUpdate. [WEAP-MOD-VIS]");

//-----------------------------------------------------------------------------
// Engine symbols
//-----------------------------------------------------------------------------
static char (__fastcall* v_C_WeaponX_RecalcMods)(void* pWeapon) = nullptr;
static void (__fastcall* v_C_WeaponX_RequestBodygroupUpdate)(void* pWeapon) = nullptr;
static void* (__fastcall* v_C_BaseAnimating_GetModelPtr)(void* pEntity) = nullptr;
static int (__fastcall* v_Inv_GetSlotForActiveWeapon)(void* pInv, void* pWeapon) = nullptr;
static const char* s_pEntityList = nullptr;

static bool s_bInRecalcMods = false;
static uint32_t s_pendingHandle[64] = {};
static uint32_t s_coalesceHandle[64] = {};
static double s_coalesceTime[64] = {};

static void* EntityFromHandle(const uint32_t nHandle)
{
	if (nHandle == 0xFFFFFFFFu || !s_pEntityList)
		return nullptr;
	const char* const pSlot = s_pEntityList + (static_cast<size_t>(nHandle & 0xFFFFu) << 5);
	if (*reinterpret_cast<const uint32_t*>(pSlot + 8) != (nHandle >> 16))
		return nullptr;
	return *reinterpret_cast<void* const*>(pSlot);
}

// Same owner -> slot -> m_hViewModels[slot] walk RequestBodygroupUpdate uses.
static void* ViewmodelForBodygroup(void* pWeapon)
{
	const uint32_t nOwner = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<char*>(pWeapon) + 0x1560);
	void* const pOwner = EntityFromHandle(nOwner);
	if (!pOwner)
		return nullptr;
	void** const pVtable = *reinterpret_cast<void***>(pOwner);
	if (!pVtable)
		return nullptr;
	using FnMyPlayer = void* (__fastcall*)(void*);
	FnMyPlayer const fnMyPlayer = reinterpret_cast<FnMyPlayer>(pVtable[0x588 / 8]);
	if (!fnMyPlayer)
		return nullptr;
	void* const pPlayer = fnMyPlayer(pOwner);
	if (!pPlayer)
		return nullptr;
	const int nSlot = v_Inv_GetSlotForActiveWeapon(
		reinterpret_cast<char*>(pPlayer) + 0x18D8, pWeapon);
	if (static_cast<unsigned>(nSlot) >= 3u)
		return nullptr;
	const uint32_t nViewModel = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<char*>(pPlayer) + 0x2DA8 + nSlot * 4);
	return EntityFromHandle(nViewModel);
}

// CStudioHdr at entity+0x1000; bodygroup walk reads studio at hdr+8.
static bool StudioHdrLive(void* pEntity)
{
	if (!pEntity)
		return false;
	void* const pHdr = v_C_BaseAnimating_GetModelPtr(pEntity);
	if (!pHdr)
		return false;
	return *reinterpret_cast<void**>(reinterpret_cast<char*>(pHdr) + 8) != nullptr;
}

static constexpr uintptr_t kViewModelWeaponHandleOffset = 0x1CF4;

bool WeaponModVisual_ViewmodelShowsWeapon(void* pWeapon)
{
	if (!pWeapon || !v_Inv_GetSlotForActiveWeapon || !s_pEntityList)
		return false;
	void* const pViewModel = ViewmodelForBodygroup(pWeapon);
	if (!pViewModel)
		return true;
	const uint32_t nWeapon = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const char*>(pWeapon) + 8);
	return *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const char*>(pViewModel) + kViewModelWeaponHandleOffset) == nWeapon;
}

static char __fastcall Hook_C_WeaponX_RecalcMods(void* pWeapon)
{
	const char result = v_C_WeaponX_RecalcMods(pWeapon);
	if (!pWeapon || !sdk_weap_mod_bodygroup.GetBool())
		return result;
	if (!v_C_WeaponX_RequestBodygroupUpdate || !v_C_BaseAnimating_GetModelPtr
		|| !v_Inv_GetSlotForActiveWeapon || !s_pEntityList)
		return result;

	const uint32_t nHandle = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const char*>(pWeapon) + 8);
	const int nHash = static_cast<int>(nHandle & 63u);

	if (*reinterpret_cast<const uint32_t*>(
			reinterpret_cast<const char*>(pWeapon) + 0x1560) == 0xFFFFFFFFu)
	{
		s_pendingHandle[nHash] = 0;
		return result;
	}
	if (s_bInRecalcMods)
		return result;

	const bool bPending = (s_pendingHandle[nHash] == nHandle);
	if (!result && !bPending)
		return result;

	if (!StudioHdrLive(ViewmodelForBodygroup(pWeapon)))
	{
		s_pendingHandle[nHash] = nHandle;
		return result;
	}

	const double flNow = Plat_FloatTime();
	if (!bPending && s_coalesceHandle[nHash] == nHandle && (flNow - s_coalesceTime[nHash]) < 0.016)
		return result;
	s_coalesceHandle[nHash] = nHandle;
	s_coalesceTime[nHash] = flNow;
	s_pendingHandle[nHash] = 0;

	s_bInRecalcMods = true;
	v_C_WeaponX_RequestBodygroupUpdate(pWeapon);
	s_bInRecalcMods = false;

	if (sdk_weap_mod_bodygroup_log.GetBool())
	{
		Msg(eDLL_T::CLIENT,
			"[WEAP-MOD-VIS] RequestBodygroupUpdate weapon=%p\n", pWeapon);
	}
	return result;
}

//-----------------------------------------------------------------------------
// IDetour
//-----------------------------------------------------------------------------
void VWeaponModVisual::GetAdr(void) const
{
	LogFunAdr("C_WeaponX::RecalcMods", v_C_WeaponX_RecalcMods);
	LogFunAdr("C_WeaponX::RequestBodygroupUpdate", v_C_WeaponX_RequestBodygroupUpdate);
	LogFunAdr("C_BaseAnimating::GetModelPtr", v_C_BaseAnimating_GetModelPtr);
	LogFunAdr("Inv_GetSlotForActiveWeapon", v_Inv_GetSlotForActiveWeapon);
}

void VWeaponModVisual::GetFun(void) const
{
	// RecalcMods: current-mod dword +0x171C, WpnData ptr +0x17A8, modVars +0x19D0.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 83 EC 40 48 8B F9 E8 ?? ?? ?? ?? "
		"48 8B 05 ?? ?? ?? ?? 44 8B 97 20 17 00 00 48 8B 97 A8 17 00 00 "
		"8B 8F 1C 17 00 00")
		.GetPtr(v_C_WeaponX_RecalcMods);
	if (!v_C_WeaponX_RecalcMods)
		Warning(eDLL_T::CLIENT,
			"[WEAP-MOD-VIS] RecalcMods pattern unresolved -- feature disabled\n");

	// RequestBodygroupUpdate: owner handle +0x1560. Same prologue as SetViewModel
	// except this one saves rsi (74) rather than rbp (6C) before the entity-list lea.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 8B 81 60 15 00 00 48 8B D9 83 F8 FF "
		"0F 84 ?? ?? ?? ?? 0F B7 C8 48 C1 E1 05 48 89 74 24 ?? 48 8D 35")
		.GetPtr(v_C_WeaponX_RequestBodygroupUpdate);
	if (!v_C_WeaponX_RequestBodygroupUpdate)
		Warning(eDLL_T::CLIENT,
			"[WEAP-MOD-VIS] RequestBodygroupUpdate pattern unresolved -- feature disabled\n");

	if (v_C_WeaponX_RequestBodygroupUpdate)
	{
		const CMemory bodygroup(v_C_WeaponX_RequestBodygroupUpdate);
		const CMemory entLea = bodygroup.FindPattern("48 8D 35");
		if (entLea.IsValid())
			s_pEntityList = entLea.ResolveRelativeAddress(3, 7).RCast<const char*>();
		bodygroup.FindPattern("48 8D 88 D8 18 00 00 48 8B D3 E8")
			.Offset(10)
			.FollowNearCall()
			.GetPtr(v_Inv_GetSlotForActiveWeapon);
	}
	if (!s_pEntityList || !v_Inv_GetSlotForActiveWeapon)
		Warning(eDLL_T::CLIENT,
			"[WEAP-MOD-VIS] viewmodel resolve unresolved -- feature disabled\n");

	// GetViewModel (owner +0x1560, inventory +0x18D8, m_hViewModels +0x2DA8) then
	// GetModelPtr. cmp [player+0x4092] makes this unique vs the sibling getter.
	const CMemory getVm = Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 81 60 15 00 00 "
		"48 8B F9 83 F8 FF 0F 84 ?? ?? ?? ?? 0F B7 D0 48 8D 35 ?? ?? ?? ?? "
		"48 C1 E2 05 C1 E8 10 39 44 32 08 0F 85 ?? ?? ?? ?? 48 8B 0C 32 "
		"48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 01 FF 90 88 05 00 00 48 8B D8 "
		"48 85 C0 74 ?? 80 B8 92 40 00 00 00");
	if (getVm.IsValid())
	{
		getVm.FindPattern("48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B C3")
			.Offset(3)
			.FollowNearCall()
			.GetPtr(v_C_BaseAnimating_GetModelPtr);
	}
	if (!v_C_BaseAnimating_GetModelPtr)
		Warning(eDLL_T::CLIENT,
			"[WEAP-MOD-VIS] GetModelPtr pattern unresolved -- feature disabled\n");
}

void VWeaponModVisual::Detour(const bool bAttach) const
{
	if (v_C_WeaponX_RecalcMods && v_C_WeaponX_RequestBodygroupUpdate
		&& v_C_BaseAnimating_GetModelPtr && v_Inv_GetSlotForActiveWeapon && s_pEntityList)
		DetourSetup(&v_C_WeaponX_RecalcMods, &Hook_C_WeaponX_RecalcMods, bAttach);
}
