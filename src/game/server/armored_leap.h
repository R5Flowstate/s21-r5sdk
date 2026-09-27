//=============================================================================//
//
// Purpose: Armored leap (Newcastle ultimate) on the dedicated server -- the
// S21 client's leap natives, phase machine and per-tick movement, plus the
// movement rules that change while a leap is active.
//
//=============================================================================//
#ifndef ARMORED_LEAP_H
#define ARMORED_LEAP_H

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;
class CSquirrelVM;

void ArmoredLeap_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);
void ArmoredLeap_RegisterScriptConstants(CSquirrelVM* s);
void ArmoredLeap_LevelShutdown(void);

bool ArmoredLeap_IsActive(const void* pPlayer);

// Leap activity for CalcMainActivity, or -1 to fall through to the normal chain.
int ArmoredLeap_AnimActivity(const void* pPlayer);

// CGameMovement::FullWalkMove brackets; the leap accel runs after the first
// half-gravity + CheckVelocity pair inside it.
void ArmoredLeap_BeginFullWalkMove(void* ctx);
void ArmoredLeap_EndFullWalkMove(void);
void ArmoredLeap_NoteHalfGravity(void* ctx, float flFrameTime);

// CGameMovement::Duck brackets: no ducking while leaping, then the per-command
// phase/state update.
void ArmoredLeap_BeforeDuck(void* ctx);
void ArmoredLeap_AfterDuck(void* ctx);

///////////////////////////////////////////////////////////////////////////////
class VArmoredLeap : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ARMORED_LEAP_H
