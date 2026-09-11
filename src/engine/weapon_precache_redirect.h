//=============================================================================//
//
// Purpose: Redirect PrecacheWeapon's ScriptNames lookup to WeaponNames.
//
//=============================================================================//

#ifndef WEAPON_PRECACHE_REDIRECT_S21_H
#define WEAPON_PRECACHE_REDIRECT_S21_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VWeaponPrecacheRedirectS21 : public IDetour
{
    virtual void GetAdr(void) const {}
    virtual void GetFun(void) const {}
    virtual void GetVar(void) const {}
    virtual void GetCon(void) const {}
    virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_PRECACHE_REDIRECT_S21_H
