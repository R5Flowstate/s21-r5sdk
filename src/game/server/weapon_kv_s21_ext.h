//=============================================================================//
//
// Purpose: S21-only weapon KV keys the S3 schema cannot hold.
//
//=============================================================================//
#ifndef WEAPON_KV_S21_EXT_H
#define WEAPON_KV_S21_EXT_H

#include <string>
#include <vector>

// Bool keys whose value follows the weapon's active mods, as the client's
// mod assembly resolves them.
enum class WeaponS21Bool_e : uint8_t
{
	OFFHAND_INSTANT_SWAP_TO_OFFHAND,
	OFFHAND_ALLOW_SWAP_TO_ON_ZIPLINE,
	OFFHAND_ONLY_SWAP_TO_ON_GROUND,
	OFFHAND_CANCELLED_BY_MELEE,
	OFFHAND_MATCH_PLAYER_SKIN,
	OFFHAND_DISABLE_OTHER_OFFHANDS,
	UPDATE_PLAYER_LAST_FIRE_TIME,

	COUNT
};

struct WeaponKVS21ModBool_t
{
	std::string     modName;
	WeaponS21Bool_e key;
	bool            value;
	mutable int     nBit = -2; // -2 unresolved, -1 not a mod of this weapon
};

struct WeaponKVS21Ext_t
{
	float flInheritBaseVelocityScale = 1.0f;  // projectile_inherit_base_velocity_scale
	float flAirFriction              = 0.0f;  // projectile_air_friction
	float flAirFriction2             = 0.0f;  // projectile_air_friction_2
	float flAirFrictionFinal         = 0.0f;  // projectile_air_friction_final
	float flGravityScale2            = 0.0f;  // projectile_gravity_scale_2
	float flGravityScaleTime2        = 0.0f;  // projectile_gravity_scale_time_2
	float flGravityScaleFinal        = 0.0f;  // projectile_gravity_scale_final
	float flGravityScaleTimeFinal    = 0.0f;  // projectile_gravity_scale_time_final
	float flSpreadMinKick            = 0.0f;  // spread_min_kick
	float flRegenAmmoForcedDelay     = 0.0f;  // regen_ammo_forced_delay
	bool  bSpreadUpdateHipfireInAds  = false; // spread_update_hipfire_in_ads
	bool  bOffhandJumpToggle         = false; // offhand_deactivate_on_jump_toggle_or_release
	bool  bOffhandInstantSwap        = false; // offhand_instant_swap_to_offhand
	bool  bOffhandHoldsOnTactical    = false; // offhand_holds_on_tactical
	bool  bOffhandSwitchSlot         = false; // offhand_switch_slot
	bool  bIsHeirloom                = false; // is_heirloom
	bool  bIsArtifact                = false; // is_artifact
	bool  bHasHeirloomMod            = false; // Mods { "heirloom" }
	bool  bLoaded                    = false;
	bool  bHasSpreadMinKick          = false;

	// Client parse-table defaults, in WeaponS21Bool_e order.
	bool  abBool[static_cast<size_t>(WeaponS21Bool_e::COUNT)] = { false, true, false, false, false, false, true };
	std::vector<WeaponKVS21ModBool_t> modBools;
};

const WeaponKVS21Ext_t& WeaponKVS21Ext_Get(const char* pszWeaponName);
// Base value, then every active mod that sets the key, in Mods order.
bool WeaponKVS21Ext_GetBool(const void* pWeapon, WeaponS21Bool_e key);
void WeaponKVS21Ext_LevelShutdown(void);

#endif // WEAPON_KV_S21_EXT_H
