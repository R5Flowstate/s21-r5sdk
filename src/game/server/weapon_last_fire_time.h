//=============================================================================//
//
// Purpose: update_player_last_fire_time on the dedi attack paths.
//
// The S21 client stamps the owner's m_lastFiredTime / m_lastFiredWeapon on a
// primary attack only when the weapon sets update_player_last_fire_time
// (default on; abilities turn it off). The dedi attack paths always stamp.
//
//=============================================================================//
#ifndef WEAPON_LAST_FIRE_TIME_H
#define WEAPON_LAST_FIRE_TIME_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VWeaponLastFireTime : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_LAST_FIRE_TIME_H
