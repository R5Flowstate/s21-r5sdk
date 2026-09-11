//=============================================================================//
//
// Purpose: Let a weapon mod override ammo_pool_type on the S3 dedi.
//
// The engine only applies mod entries inside the 0x1150 settings block
// (WeaponInfo+0x12E8). ammo_pool_type lives at WeaponInfo+0x268, so a mod like
// "alt_ammo" { "ammo_pool_type" "bullet" } is rejected at parse. This retargets
// CWeaponX+0x16B8 to a per-entity byte-clone of WeaponInfo with the alt pool
// written at +0x268 whenever such a mod is active.
//
//=============================================================================//
#ifndef WEAPON_AMMO_POOL_MOD_H
#define WEAPON_AMMO_POOL_MOD_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VWeaponAmmoPoolMod : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

void WeaponAmmoPoolMod_LevelShutdown(void);

#endif // WEAPON_AMMO_POOL_MOD_H
