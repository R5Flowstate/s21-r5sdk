//=============================================================================//
//
// Purpose: per-weapon raise of ammo_stockpile_max on the dedicated server.
//
//=============================================================================//
#ifndef WEAPON_STOCKPILE_BONUS_H
#define WEAPON_STOCKPILE_BONUS_H

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;

void WeaponStockpileBonus_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);

///////////////////////////////////////////////////////////////////////////////
class VWeaponStockpileBonus : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_STOCKPILE_BONUS_H
