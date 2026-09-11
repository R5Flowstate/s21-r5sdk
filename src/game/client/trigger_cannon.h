//=============================================================================//
//
// Purpose: prediction twin of the dedi's gravity-cannon air-control lock. The
// predicted launch arms a flight window; AirMove projects the along-the-launch
// component out of the wish direction for its duration, and the tap-strafe hook
// denies the lurch over the same window. See trigger_cannon_client.cpp.
//
//=============================================================================//
#ifndef TRIGGER_CANNON_CLIENT_H
#define TRIGGER_CANNON_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

// C_GameMovement::ApplyGravityCannonLaunch (S21). a2 is the
// C_TriggerCylinderHeavy. Hooked to arm the flight window off the same trigger
// fields the dedi arms from.
inline void (*C_GameMovement__ApplyGravityCannonLaunch)(void* ctx, void* pTrigger) = nullptr;

// C_GameMovement::AirMove (S21). Hooked to apply the wish-direction projection
// this build only performs for its own launcher-flight status effect.
inline __int64 (*C_GameMovement__AirMove)(void* ctx, float flFrameTime, char a3) = nullptr;

// True while the local player is inside a cannon flight window. Read by the
// tap-strafe hook in mantle_boost_client.cpp, which already owns the only skip
// point for the lurch.
bool TriggerCannonClient_IsFlightLocked(const void* pPlayer);

///////////////////////////////////////////////////////////////////////////////
class VTriggerCannonClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TRIGGER_CANNON_CLIENT_H
