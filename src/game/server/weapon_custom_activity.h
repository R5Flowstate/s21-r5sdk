#ifndef WEAPON_CUSTOM_ACTIVITY_H
#define WEAPON_CUSTOM_ACTIVITY_H

//=============================================================================//
//
// Purpose: S21 WCAF_* custom-activity semantics on the S3 weapon.
// Scripts pass S21 flags; the engine keeps S3 bits; the wire carries S21.
//
//=============================================================================//

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;
class CSquirrelVM;

// S21 m_customActivityFlags for the send proxy, given the weapon's S3 byte.
int  WeaponCustomAct_WireFlags(void* pWeapon, int s3Flags);
// Called from the engine StartCustomActivity detour on every start.
void WeaponCustomAct_OnEngineStart(void* pWeapon);
// The owner's viewmodel for this weapon, or null.
void* WeaponCustomAct_GetViewmodel(void* pWeapon);
void WeaponCustomAct_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);
void WeaponCustomAct_RegisterConstants(CSquirrelVM* s);
void WeaponCustomAct_LevelShutdown(void);

///////////////////////////////////////////////////////////////////////////////
class VWeaponCustomActivity : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_CUSTOM_ACTIVITY_H
