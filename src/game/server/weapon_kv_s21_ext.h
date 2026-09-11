//=============================================================================//
//
// Purpose: S21-only weapon KV keys the S3 schema cannot hold.
//
//=============================================================================//
#ifndef WEAPON_KV_S21_EXT_H
#define WEAPON_KV_S21_EXT_H

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
	bool  bSpreadUpdateHipfireInAds  = false; // spread_update_hipfire_in_ads
	bool  bLoaded                    = false;
	bool  bHasSpreadMinKick          = false;
};

const WeaponKVS21Ext_t& WeaponKVS21Ext_Get(const char* pszWeaponName);
void WeaponKVS21Ext_LevelShutdown(void);

#endif // WEAPON_KV_S21_EXT_H
