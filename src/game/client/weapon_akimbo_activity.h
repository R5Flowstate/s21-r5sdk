//=============================================================================//
//
// Purpose: While a player dual wields, the reload activities of the pistol
// viewmodels translate to the ACT_VM_ONEHANDED_AKIMBO_RELOAD* set that
// scripts/activity_types.txt registers, so both hands play the two-gun
// reload clips. The retail S21 translate chain stops at the one-handed step.
//
//=============================================================================//
#ifndef WEAPON_AKIMBO_ACTIVITY_H
#define WEAPON_AKIMBO_ACTIVITY_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VWeaponAkimboActivity : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_AKIMBO_ACTIVITY_H
