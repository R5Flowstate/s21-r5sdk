//=============================================================================//
//
// Purpose: 1p weapon activity modifiers. See weapon_activity_modifiers.cpp.
//
//=============================================================================//
#ifndef WEAPON_ACTIVITY_MODIFIERS_H
#define WEAPON_ACTIVITY_MODIFIERS_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VWeaponActivityModifiers : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_ACTIVITY_MODIFIERS_H
