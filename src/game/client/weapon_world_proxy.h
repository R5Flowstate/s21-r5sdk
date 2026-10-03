//=============================================================================//
//
// Purpose: Skip the weapon world-model proxy update when the weapon's owner is
//          not a player (titan weapons held by an NPC titan).
//
//=============================================================================//
#ifndef CLIENT_WEAPON_WORLD_PROXY_H
#define CLIENT_WEAPON_WORLD_PROXY_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VWeaponWorldProxy : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_WEAPON_WORLD_PROXY_H
