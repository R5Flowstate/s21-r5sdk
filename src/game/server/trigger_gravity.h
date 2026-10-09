//=============================================================================//
//
// Purpose: Server-authoritative TT_GRAVITY_LIFT (type 4) and TT_BLACKHOLE (type 8).
//
//=============================================================================//
#ifndef TRIGGER_GRAVITY_BRIDGE_H
#define TRIGGER_GRAVITY_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"

// Script setters, called from weapon_script_vars.cpp. Each returns false and
// warns on a rejected argument.
bool TriggerGravity_SetGravityLiftParams(void* pTrigger, const float params[10]);
bool TriggerGravity_SetBlackholeParams(void* pTrigger, const float params[6]);
bool TriggerGravity_SetBlackholeIsStrongPulling(void* pTrigger, bool bStrong);

// The per-tick pass. Called from Hook_JumpPad_ApplyLaunchPass, after
// TriggerCannon_ApplyPass.
void TriggerGravity_ApplyPass(void* pCtx);

// The two networked bits, read by the dt_extend value proxies. Returns false
// when this player has no slot or the wire lever is off; the caller then emits
// zeros, which IS the correct idle state for both bits.
struct TriggerGravityWire
{
	int m_gravityLiftActive;
	int m_blackholeActive;
};
bool TriggerGravity_GetWire(const void* pPlayer, TriggerGravityWire* pOut);

// bridge_trigger_gravity_wire, read by dt_extend at SendTable_Init to decide
// whether to append the two props AT ALL. Boot-time read -- set it as a launch
// arg, not in-console.
bool TriggerGravity_WireEnabled(void);

// True while a gravity lift is carrying this player. Second term of the S21
// client's own gravity-suppression gate, so the movement pass has to read it
// independently of whether the wire lever is on.
bool TriggerGravity_IsLiftActive(const void* pPlayer);

// Clears the sidecar. Called wherever JetDrive_Wire_LevelShutdown is already
// called.
void TriggerGravity_Wire_LevelShutdown(void);

// Line trace ignoring one entity. False when the engine trace is unresolved.
// nTraceType is the filter's +8 word, which dedi only hands to the entity
// partition query (script TraceLine passes 0); world geometry is always traced.
class Vector3D;
class CGameTrace;
bool TriggerGravity_TraceLine(const Vector3D& start, const Vector3D& end, unsigned int nMask,
	const void* pIgnore, int nCollisionGroup, CGameTrace* pTrace, int nDetail = 2, int nTraceType = 1);

///////////////////////////////////////////////////////////////////////////////
class VTriggerGravityBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	// No detour of its own -- the pass rides VJumpPadParity's ApplyLaunchPass,
	// same shape as VJetDrive / VTriggerCannonBridge's launch half.
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // TRIGGER_GRAVITY_BRIDGE_H
