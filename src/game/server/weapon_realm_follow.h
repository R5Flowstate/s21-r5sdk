//=============================================================================//
//
// Purpose: carried-weapon realm follow. See weapon_realm_follow.cpp.
//
//=============================================================================//
#ifndef WEAPON_REALM_FOLLOW_H
#define WEAPON_REALM_FOLLOW_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VWeaponRealmFollow : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

// Activation-time stamp for weapon_select_mirror's SetActiveWeapon post-hook.
// Adopts the owner's live realms mask onto the weapon being activated.
void WeaponRealmFollow_StampActiveWeapon(void* const player, const __int64 weaponEnt);

#endif // WEAPON_REALM_FOLLOW_H
