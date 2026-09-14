//=============================================================================//
//
// Purpose: Holstered reselect of a leftover-active weapon must write
// m_selectedWeapons. See weapon_holster_reselect.cpp.
//
//=============================================================================//
#ifndef WEAPON_HOLSTER_RESELECT_H
#define WEAPON_HOLSTER_RESELECT_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VHolsterReselect : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_HOLSTER_RESELECT_H
