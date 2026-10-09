//=============================================================================//
//
// Purpose: Source ramp launch on the dedicated server; the client twin is
// game/client/slope_launch.cpp.
//
//=============================================================================//
#ifndef SLOPE_LAUNCH_H
#define SLOPE_LAUNCH_H

#include "thirdparty/detours/include/idetour.h"

// Sampled from VJetDrive's FullWalkMove hook. That class owns the only attach.
void SlopeLaunch_AfterFullWalkMove(void* ctx);

///////////////////////////////////////////////////////////////////////////////
class VSlopeLaunch : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // SLOPE_LAUNCH_H
