//=============================================================================//
//
// Purpose: make the dedi's move-scale weapon term agree with the client's.
// See movescale_weapon_parity.cpp for the measurement behind it.
//
//=============================================================================//
#ifndef MOVESCALE_WEAPON_PARITY_H
#define MOVESCALE_WEAPON_PARITY_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VMoveScaleWeaponParity : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MOVESCALE_WEAPON_PARITY_H
