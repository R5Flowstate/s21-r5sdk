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
void Translocation_SetMoveType(void* pEnt, int moveType);
bool Translocation_SetMoveTypeResolved(void);

// Portal teleport apply primitives (resolved + twin-guarded in translocation.cpp).
void Translocation_SetAbsOrigin3(void* pEnt, const float flOrigin[3]);
void Translocation_SetAbsVelocity3(void* pEnt, const float flVel[3]);
void Translocation_SetAbsAngles3(void* pEnt, const float flAngles[3]);
// Any entity: EF_NOINTERP for this frame (no player-only parity touch).
void Translocation_SetNoInterpEffect(void* pEnt);
void Translocation_AddNoInterpFlip(void* pEnt);
void Translocation_DuckImmediateNow(void* pPlayer);
void Translocation_GetPlayerHull(void* pPlayer, bool bDucked, float flMins[3], float flMaxs[3]);

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
