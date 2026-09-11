//=============================================================================
//
// Purpose: grow WeaponInfo legendary slots 8 -> 32 and relocate the view-name array.
// Twin halves have different alloc sizes; never carry an offset across that boundary.
//
//=============================================================================
#ifndef WEAPON_LEGENDARY_EXT_H
#define WEAPON_LEGENDARY_EXT_H

#include "thirdparty/detours/include/idetour.h"

// Live server-half WeaponInfo allocation. Follows the legendary-cap grow;
// stock S3 undershoots the relocated view-name array.
uint32_t WeaponLegendaryExt_ServerWeaponInfoSize(void);

class VWeaponLegendaryExt : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // WEAPON_LEGENDARY_EXT_H
