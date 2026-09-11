#ifndef TRANSLOCATION_BRIDGE_H
#define TRANSLOCATION_BRIDGE_H

//=============================================================================//
//
// Purpose: S3 native gap-fills for Loba translocation (and any other caller
// of the same S21 surface).
//
//=============================================================================//

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;
class CSquirrelVM;

void Translocation_RegisterProjectileFuncs(ScriptClassDescriptor_t* projectileStruct);
void Translocation_RegisterPlayerFuncs(ScriptClassDescriptor_t* playerStruct);
void Translocation_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);
void Translocation_RegisterFreeFuncs(CSquirrelVM* s);
void Translocation_OnPlayerRunCommand(void* pPlayer);
void Translocation_SetOneHandedForPlayer(void* pPlayer, bool on);
void Translocation_EndNoProjTossForPlayer(void* pPlayer);
void Translocation_BeginNoProjTossForPlayer(void* pPlayer);
void Translocation_LevelShutdown(void);
// Issues the ORIGINAL CWeaponX::HolsterInternal, bypassing Hook_HolsterInternal.
void Translocation_HolsterWeaponOriginal(void* pWeapon);

///////////////////////////////////////////////////////////////////////////////
class VTranslocation : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TRANSLOCATION_BRIDGE_H
