//=============================================================================
//
// Purpose: S21 dual-wield partner is N+7, not S3's N+5. Rewrite both pairing sites.
//
//=============================================================================
#ifndef WEAPON_DUALWIELD_SLOT_PATCH_H
#define WEAPON_DUALWIELD_SLOT_PATCH_H

#include "thirdparty/detours/include/idetour.h"

class VWeaponDualWieldSlotPatch : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // WEAPON_DUALWIELD_SLOT_PATCH_H
