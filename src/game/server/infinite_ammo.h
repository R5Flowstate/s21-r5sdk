//=============================================================================//
//
// Purpose: honor weapon.SetInfiniteAmmoState(INFINITEAMMO_CLIPS) on S3 natives.
// Detour the live CWeaponX cluster; the unused twin is a silent no-op.
//
//=============================================================================//
#ifndef INFINITE_AMMO_DEDI_H
#define INFINITE_AMMO_DEDI_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VInfiniteAmmoDedi : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // INFINITE_AMMO_DEDI_H
