//=============================================================================
//
// Purpose: force-clear active weapons whose type bit is in the disabled mask (reason 7).
// Type flags at weapon+0x1A40. WPT_VIEWHANDS stays sticky.
//
//=============================================================================
#ifndef WEAPON_ENFORCE_H
#define WEAPON_ENFORCE_H

#include <cstdint>

enum class WeaponEnforceResult : uint8_t
{
	Allow         = 0,  // weapon is allowed (matches WEAPON_DISABLE_REASON_NONE)
	BlockedByType = 7,  // mirrors reason 7 - weapon type bit matches disabled mask
};

// Read-only check. Consults the per-player disabled mask (server-side
// SDKEntityMap<WeaponTypeDisableState>) and ANDs with the weapon's type flags
// at weapon+0x1A40. Returns Allow on null args or if the ConVar is off.
WeaponEnforceResult WeaponEnforce_Check(const void* pPlayer, const void* pWeapon);

#ifndef CLIENT_DLL
// Holster then Term any active slot whose type is now disabled; dirty-mark the player.
// Re-entry guard. WPT_VIEWHANDS stays sticky.
void WeaponEnforce_ForceSwapIfNowDisabled(void* pPlayer);

// Per-tick sweep: engine-driven switches bypass Script_DisableWeaponTypes.
void WeaponEnforce_TickAllServer();
#endif // !CLIENT_DLL

#endif // WEAPON_ENFORCE_H
