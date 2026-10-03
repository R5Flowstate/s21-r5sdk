//=============================================================================//
//
// Purpose: send the projectile trail temp entity for weapons whose trail is
//          defined under the S21 third-person keys.
//
//=============================================================================//
#ifndef PROJECTILE_TRAIL_TE_H
#define PROJECTILE_TRAIL_TE_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VProjectileTrailTE : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // PROJECTILE_TRAIL_TE_H
