//=============================================================================//
//
// Purpose: weapon_regen.h implementation.
//
// The S21 client holds an ammo_drains_to_empty_on_fire weapon in its drain
// window for regen_ammo_forced_delay seconds after the last primary attack,
// and backdates a regen reset by the same amount. The dedi server uses
// fireDuration for both; the key replaces it for the length of the regen call.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "weapon_regen.h"
#include "weapon_kv_s21_ext.h"

// Server CWeaponX layout.
static constexpr ptrdiff_t WR_WEAPON_OFF_CLASSNAME    = 0x15B0;
static constexpr ptrdiff_t WR_WEAPON_OFF_FIREDURATION = 0x180C; // m_modVars.fireDuration

static ConVar bridge_regen_forced_delay("bridge_regen_forced_delay", "1", FCVAR_RELEASE,
	"Apply the S21 regen_ammo_forced_delay weapon key to server clip regen, matching the client's prediction.");

static int64_t (*v_WeaponX_RegenerateAmmo)(void* pWeapon) = nullptr;

static int64_t Hook_WeaponX_RegenerateAmmo(void* pWeapon)
{
	if (!pWeapon || !bridge_regen_forced_delay.GetBool())
		return v_WeaponX_RegenerateAmmo(pWeapon);

	const char* const pszName = static_cast<const char*>(pWeapon) + WR_WEAPON_OFF_CLASSNAME;
	const float flForced = WeaponKVS21Ext_Get(pszName).flRegenAmmoForcedDelay;
	if (flForced <= 0.0f)
		return v_WeaponX_RegenerateAmmo(pWeapon);

	float* const pFireDuration = reinterpret_cast<float*>(static_cast<uint8_t*>(pWeapon) + WR_WEAPON_OFF_FIREDURATION);
	const float flSaved = *pFireDuration;
	*pFireDuration = flForced;
	const int64_t nResult = v_WeaponX_RegenerateAmmo(pWeapon);
	*pFireDuration = flSaved;
	return nResult;
}

void VWeaponRegen::GetAdr(void) const
{
	LogFunAdr("WeaponX_RegenerateAmmo", v_WeaponX_RegenerateAmmo);
}

void VWeaponRegen::GetFun(void) const
{
	// Server twin: m_lastRegenTime at +0x1548, m_modVars.burstFireCount at +0x1814.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 40 F3 0F 10 81 48 15 00 00 48 8B D9 0F 2E 05 ?? ?? ?? ?? 0F 8A ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 83 B9 14 18 00 00 00")
		.GetPtr(v_WeaponX_RegenerateAmmo);
	if (!v_WeaponX_RegenerateAmmo)
		Warning(eDLL_T::SERVER,
			"[WeaponRegen] CWeaponX regen pattern unresolved -- regen_ammo_forced_delay ignored, client cooldown will fight the server\n");
}

void VWeaponRegen::Detour(const bool bAttach) const
{
	if (v_WeaponX_RegenerateAmmo)
		DetourSetup(&v_WeaponX_RegenerateAmmo, &Hook_WeaponX_RegenerateAmmo, bAttach);
}
