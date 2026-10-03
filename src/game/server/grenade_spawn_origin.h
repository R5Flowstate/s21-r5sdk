//=============================================================================//
//
// Purpose: thrown-grenade spawn origin. The dedicated server creates every
// FireWeaponGrenade projectile at the owner's eye and sweeps it to the
// requested position, so a script that launches a projectile from a remote
// point (cluster bomb bursts) has it pulled back toward the owner whenever
// geometry blocks the line between them. The projectile is created at the
// requested position instead.
//
//=============================================================================//
#ifndef GRENADE_SPAWN_ORIGIN_H
#define GRENADE_SPAWN_ORIGIN_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VGrenadeSpawnOrigin : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // GRENADE_SPAWN_ORIGIN_H
