//=============================================================================//
//
// Purpose: client prediction twin of the Source ramp launch
// (game/server/slope_launch.cpp).
//
//=============================================================================//
#ifndef SLOPE_LAUNCH_CLIENT_H
#define SLOPE_LAUNCH_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

// Sampled from VMoveSimTraceClient's FullWalkMove hook. That class owns the only attach.
void SlopeLaunchClient_AfterFullWalkMove(void* ctx);

///////////////////////////////////////////////////////////////////////////////
class VSlopeLaunchClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // SLOPE_LAUNCH_CLIENT_H
