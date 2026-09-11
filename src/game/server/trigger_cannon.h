//=============================================================================//
//
// Purpose: Server-authoritative TT_GRAVITY_CANNON launch after the predicted-trigger pass.
//
//=============================================================================//
#ifndef TRIGGER_CANNON_BRIDGE_H
#define TRIGGER_CANNON_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"

// After the predicted-trigger pass. ctx+8 CPlayer, ctx+16 CMoveData.
void TriggerCannon_ApplyPass(void* pCtx);

// Cannon-specific accessors. Bound as entity methods in weapon_script_vars.cpp.
bool TriggerCannon_SetLaunchDelay(void* pTrigger, float flDelay);
bool TriggerCannon_IsPreparingLaunch(const void* pTrigger);
bool TriggerCannon_SetLaunchTargetLocation(void* pTrigger, const float target[3]);
bool TriggerCannon_GetLaunchDir(const void* pTrigger, float outDir[3]);
bool TriggerCannon_SetEnableDoubleJump(void* pTrigger, bool bEnable);
bool TriggerCannon_SetLaunchAirControlParams(void* pTrigger, float flSpeed, float flAccel);
bool TriggerCannon_SetLimitedAirControl(void* pTrigger, bool bLimited);
void TriggerCannon_OnJumpPadLaunched(void* pPlayer, void* pTrigger);

// Air-control lock for the launch arc. Caller must restore saved dir after
// AirMove and suppress lurch for the same window.
bool TriggerCannon_BeginFlightLock(void* pPlayer, void* pMoveData, float savedDir[3]);
void TriggerCannon_EndFlightLock(void* pMoveData, const float savedDir[3]);

// Shared with the lift/blackhole pass -- both hang off the same predicted-trigger
// walk, so the engine pointers have exactly one owner.
void  TriggerPass_EnterScriptCallback(void* pTrigger, void* pOther);
void  TriggerPass_SetGroundEntityNull(void* pPlayer);
void  TriggerPass_SetGroundEntity(void* pPlayer, void* pGround);
void  TriggerPass_EnsureAbsOrigin(void* pEntity);
float TriggerPass_MovementTime(void);
float TriggerPass_FrameTime(void);

///////////////////////////////////////////////////////////////////////////////
class VTriggerCannonBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	// EndTouch closes the charge window when the last toucher leaves; the pass
	// itself opens it and owns the launch, riding VJumpPadParity's ApplyLaunchPass.
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TRIGGER_CANNON_BRIDGE_H
