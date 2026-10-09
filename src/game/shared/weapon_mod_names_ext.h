//=============================================================================//
//
// Purpose: grow the weapon mod-name string table (stock 7168 bytes / 448 names).
//
//=============================================================================//
#ifndef WEAPON_MOD_NAMES_EXT_H
#define WEAPON_MOD_NAMES_EXT_H

#include "thirdparty/detours/include/idetour.h"

class VWeaponModNamesExt : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // WEAPON_MOD_NAMES_EXT_H
