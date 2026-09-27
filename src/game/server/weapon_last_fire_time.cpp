//=============================================================================//
//
// Purpose: weapon_last_fire_time.h implementation.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "weapon_last_fire_time.h"
#include "weapon_kv_s21_ext.h"
#include "game/shared/sdk_entity_state.h"

// Server CWeaponX.
static constexpr ptrdiff_t WLF_WEAPON_OFF_OWNER = 0x11F0; // m_weaponOwner
// Server CBaseCombatCharacter.
static constexpr ptrdiff_t WLF_OWNER_OFF_LASTFIREDTIME   = 0x15C0; // m_lastFiredTime
static constexpr ptrdiff_t WLF_OWNER_OFF_LASTFIREDWEAPON = 0x15C4; // m_lastFiredWeapon

static constexpr uint32_t WLF_INVALID_HANDLE = 0xFFFFFFFFu;

static ConVar bridge_weapon_last_fire_time("bridge_weapon_last_fire_time", "1", FCVAR_RELEASE,
	"Honor update_player_last_fire_time: weapons that turn it off leave the owner's "
	"m_lastFiredTime / m_lastFiredWeapon untouched when they attack.");

// Every path takes the weapon first.
static __int64 (*v_WeaponX_AnimEventPrimaryAttack)(void* pWeapon) = nullptr;
static __int64 (*v_WeaponX_FireAttack)(void* pWeapon, float a2, float a3, char a4) = nullptr;
static int (*v_WeaponX_TossAttack)(void* pWeapon, __int64 a2, __int64 a3, __int64 a4, __int64 a5, __int64 a6,
	__int64 a7, __int64 a8, __int64 a9, __int64 a10, __int64 a11, __int64 a12) = nullptr;

struct WLF_Saved_t
{
	uint8_t* pOwner = nullptr;
	float    flTime = 0.0f;
	uint32_t hWeapon = WLF_INVALID_HANDLE;
};

static WLF_Saved_t WLF_Save(void* pWeapon)
{
	WLF_Saved_t saved;
	if (!pWeapon || !bridge_weapon_last_fire_time.GetBool()
		|| WeaponKVS21Ext_GetBool(pWeapon, WeaponS21Bool_e::UPDATE_PLAYER_LAST_FIRE_TIME))
		return saved;

	const uint32_t hOwner = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pWeapon) + WLF_WEAPON_OFF_OWNER);
	if (hOwner == WLF_INVALID_HANDLE)
		return saved;
	saved.pOwner = static_cast<uint8_t*>(SDKEntityState_Resolve(SDKEntityHandle(hOwner), ESide::Server));
	if (saved.pOwner)
	{
		saved.flTime = *reinterpret_cast<const float*>(saved.pOwner + WLF_OWNER_OFF_LASTFIREDTIME);
		saved.hWeapon = *reinterpret_cast<const uint32_t*>(saved.pOwner + WLF_OWNER_OFF_LASTFIREDWEAPON);
	}
	return saved;
}

static void WLF_Restore(const WLF_Saved_t& saved)
{
	if (!saved.pOwner)
		return;
	*reinterpret_cast<float*>(saved.pOwner + WLF_OWNER_OFF_LASTFIREDTIME) = saved.flTime;
	*reinterpret_cast<uint32_t*>(saved.pOwner + WLF_OWNER_OFF_LASTFIREDWEAPON) = saved.hWeapon;
}

static __int64 Hook_WeaponX_AnimEventPrimaryAttack(void* pWeapon)
{
	const WLF_Saved_t saved = WLF_Save(pWeapon);
	const __int64 result = v_WeaponX_AnimEventPrimaryAttack(pWeapon);
	WLF_Restore(saved);
	return result;
}

static __int64 Hook_WeaponX_FireAttack(void* pWeapon, float a2, float a3, char a4)
{
	const WLF_Saved_t saved = WLF_Save(pWeapon);
	const __int64 result = v_WeaponX_FireAttack(pWeapon, a2, a3, a4);
	WLF_Restore(saved);
	return result;
}

static int Hook_WeaponX_TossAttack(void* pWeapon, __int64 a2, __int64 a3, __int64 a4, __int64 a5, __int64 a6,
	__int64 a7, __int64 a8, __int64 a9, __int64 a10, __int64 a11, __int64 a12)
{
	const WLF_Saved_t saved = WLF_Save(pWeapon);
	const int result = v_WeaponX_TossAttack(pWeapon, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
	WLF_Restore(saved);
	return result;
}

void VWeaponLastFireTime::GetAdr(void) const
{
	LogFunAdr("WeaponX_AnimEventPrimaryAttack", v_WeaponX_AnimEventPrimaryAttack);
	LogFunAdr("WeaponX_FireAttack", v_WeaponX_FireAttack);
	LogFunAdr("WeaponX_TossAttack", v_WeaponX_TossAttack);
}

void VWeaponLastFireTime::GetFun(void) const
{
	// The three server paths that write the owner's m_lastFiredTime (+0x15C0).
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 BA 05 00 00 00 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? "
		"8B 8B F0 11 00 00")
		.GetPtr(v_WeaponX_AnimEventPrimaryAttack);
	// Two float arguments arrive in xmm1 / xmm2.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 60 48 8B 05 ?? ?? ?? ?? "
		"48 8B F1 0F 29 74 24 50 48 8D 0D ?? ?? ?? ?? 0F 29 7C 24 40 41 0F B6 F9 0F 28 F2 0F 28 F9 FF 50 08 "
		"8B 8E F0 11 00 00")
		.GetPtr(v_WeaponX_FireAttack);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 74 24 18 44 89 4C 24 20 55 57 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC B0 00 00 00 "
		"48 8B D9 41 8B F1 8B 89 F0 11 00 00 49 8B F8 4C 8B FA")
		.GetPtr(v_WeaponX_TossAttack);

	if (!v_WeaponX_AnimEventPrimaryAttack || !v_WeaponX_FireAttack || !v_WeaponX_TossAttack)
		Warning(eDLL_T::SERVER,
			"[WEAP-LASTFIRE] attack path unresolved (anim=%d fire=%d toss=%d) -- those paths always stamp the owner's last fire time\n",
			v_WeaponX_AnimEventPrimaryAttack ? 1 : 0, v_WeaponX_FireAttack ? 1 : 0, v_WeaponX_TossAttack ? 1 : 0);
}

void VWeaponLastFireTime::Detour(const bool bAttach) const
{
	if (v_WeaponX_AnimEventPrimaryAttack)
		DetourSetup(&v_WeaponX_AnimEventPrimaryAttack, &Hook_WeaponX_AnimEventPrimaryAttack, bAttach);
	if (v_WeaponX_FireAttack)
		DetourSetup(&v_WeaponX_FireAttack, &Hook_WeaponX_FireAttack, bAttach);
	if (v_WeaponX_TossAttack)
		DetourSetup(&v_WeaponX_TossAttack, &Hook_WeaponX_TossAttack, bAttach);
}
