//=============================================================================//
//
// Purpose: Name and survive an efct whose child links point at an unpublished
//          particle-effect asset.
//
//=============================================================================//
#ifndef EFCT_CHILD_LINK_GUARD_S21_H
#define EFCT_CHILD_LINK_GUARD_S21_H

#include "thirdparty/detours/include/idetour.h"

class VEffectChildLinkGuardS21 : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // EFCT_CHILD_LINK_GUARD_S21_H
