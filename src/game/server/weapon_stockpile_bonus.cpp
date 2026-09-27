//=============================================================================//
//
// Purpose: per-weapon raise of the stockpile cap (ammo_stockpile_max) on the
// dedicated server. Stands in for a weapon mod when the weapon already uses
// every mod slot (Sparrow's +10 Bocek arrows).
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "weapon_stockpile_bonus.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <algorithm>
#include <vector>

// CWeaponX (server half): the modded-settings block the var builder writes on
// every mod change, and ammo_stockpile_max inside it (the value
// SetWeaponPrimaryAmmoCount clamps the stockpile to).
static constexpr ptrdiff_t WSB_WEAPONX_OFF_MODDED_VARS = 0x17E0;
static constexpr ptrdiff_t WSB_VARS_OFF_STOCKPILE_MAX  = 0x2EC;

static constexpr int WSB_MAX_BONUS = 1000;

static ConVar bridge_weapon_stockpile_bonus_diag("bridge_weapon_stockpile_bonus_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[STOCKPILE-BONUS] log each bonus set and each var rebuild it is applied to.");

static uint8_t (*v_WeaponVars_Build)(uint64_t modBits, void* pInfo, uint8_t* pDst, uint64_t a4, uint64_t a5, uint64_t a6) = nullptr;

static SDKEntityMap<int> s_bonusMap(ESide::Server, "weaponStockpileBonus.srv");

// Weapons that hold a bonus. The builder also fills scratch blocks, and the map
// lookup reads the entity, so only a pointer listed here is ever looked up.
static std::vector<void*> s_bonusWeapons;

static EntityDestroySubscription* s_pDestroySub = nullptr;

static void WeaponStockpileBonus_Unlist(void* const pWeapon)
{
	s_bonusWeapons.erase(std::remove(s_bonusWeapons.begin(), s_bonusWeapons.end(), pWeapon), s_bonusWeapons.end());
}

static void WeaponStockpileBonus_OnDestroy(SDKEntityHandle handle, void* pEntity, ESide side, void* userData)
{
	(void)side; (void)userData;
	if (!handle.IsValid())
		s_bonusWeapons.clear();
	else if (pEntity)
		WeaponStockpileBonus_Unlist(pEntity);
}

static int* WeaponStockpileBonus_Cap(void* const pWeapon)
{
	return reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(pWeapon)
		+ WSB_WEAPONX_OFF_MODDED_VARS + WSB_VARS_OFF_STOCKPILE_MAX);
}

static uint8_t __fastcall Hook_WeaponVars_Build(uint64_t modBits, void* pInfo, uint8_t* pDst, uint64_t a4, uint64_t a5, uint64_t a6)
{
	const uint8_t result = v_WeaponVars_Build(modBits, pInfo, pDst, a4, a5, a6);
	if (!pDst || s_bonusWeapons.empty())
		return result;

	void* const pWeapon = pDst - WSB_WEAPONX_OFF_MODDED_VARS;
	if (std::find(s_bonusWeapons.begin(), s_bonusWeapons.end(), pWeapon) == s_bonusWeapons.end())
		return result;

	const int* const pBonus = s_bonusMap.Find(pWeapon);
	if (!pBonus || *pBonus <= 0)
	{
		WeaponStockpileBonus_Unlist(pWeapon);
		return result;
	}

	int* const pCap = WeaponStockpileBonus_Cap(pWeapon);
	if (*pCap > 0)
		*pCap += *pBonus;

	if (bridge_weapon_stockpile_bonus_diag.GetBool())
		Msg(eDLL_T::SERVER, "[STOCKPILE-BONUS] rebuild weapon=%p cap=%d (+%d)\n", pWeapon, *pCap, *pBonus);
	return result;
}

//-----------------------------------------------------------------------------
// Purpose: weapon.SetStockpileMaxBonus( int bonus )
//-----------------------------------------------------------------------------
static SQRESULT Script_SetStockpileMaxBonus(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
		return SQ_ERROR;

	SQInteger nBonus = 0;
	if (SQ_FAILED(sq_getinteger(v, 2, &nBonus)))
		return SQ_ERROR;

	if (!v_WeaponVars_Build)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[STOCKPILE-BONUS] SetStockpileMaxBonus ignored: weapon var builder unresolved\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (nBonus < 0)
		nBonus = 0;
	else if (nBonus > WSB_MAX_BONUS)
		nBonus = WSB_MAX_BONUS;

	const int* const pOld = s_bonusMap.Find(pWeapon);
	const int nOld = pOld ? *pOld : 0;
	const int nNew = static_cast<int>(nBonus);

	int* const pCap = WeaponStockpileBonus_Cap(pWeapon);
	if (*pCap > 0)
		*pCap += nNew - nOld;
	if (!s_pDestroySub)
		s_pDestroySub = SDKEntityState_SubscribeDestroy(WeaponStockpileBonus_OnDestroy, nullptr, ESide::Server);
	s_bonusMap[pWeapon] = nNew;
	WeaponStockpileBonus_Unlist(pWeapon);
	if (nNew > 0)
		s_bonusWeapons.push_back(pWeapon);

	if (bridge_weapon_stockpile_bonus_diag.GetBool())
		Msg(eDLL_T::SERVER, "[STOCKPILE-BONUS] set weapon=%p bonus %d -> %d cap=%d\n", pWeapon, nOld, nNew, *pCap);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void WeaponStockpileBonus_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	if (!weaponStruct)
		return;

	weaponStruct->AddFunction(
		"SetStockpileMaxBonus",
		"Script_SetStockpileMaxBonus",
		"Raises this weapon's ammo_stockpile_max by bonus until set back to 0",
		"void",
		"int bonus",
		false,
		Script_SetStockpileMaxBonus);
}

void VWeaponStockpileBonus::GetAdr(void) const
{
	LogFunAdr("WeaponVars_Build", v_WeaponVars_Build);
}

void VWeaponStockpileBonus::GetFun(void) const
{
	// Copies WeaponInfo+0x1300 (0x1150 bytes) into the destination
	// block, then applies the mod bits.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 70 4D 8B F0 "
		"48 8D 82 00 13 00 00 4C 8B EA 48 89 84 24 B8 00 00 00 44 8B F9 48 8B D0 49 8B CE 41 B8 50 11 00 00")
		.GetPtr(v_WeaponVars_Build);

	if (!v_WeaponVars_Build)
		Warning(eDLL_T::SERVER, "[STOCKPILE-BONUS] weapon var builder unresolved; SetStockpileMaxBonus will be ignored\n");
}

void VWeaponStockpileBonus::Detour(const bool bAttach) const
{
	if (!v_WeaponVars_Build)
		return;

	DetourSetup(&v_WeaponVars_Build, &Hook_WeaponVars_Build, bAttach);
}
