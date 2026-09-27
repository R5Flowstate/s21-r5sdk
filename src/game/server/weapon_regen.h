//=============================================================================//
//
// Purpose: S21 'regen_ammo_forced_delay' on the dedicated server clip regen.
//
//=============================================================================//
#ifndef WEAPON_REGEN_H
#define WEAPON_REGEN_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VWeaponRegen : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_REGEN_H
