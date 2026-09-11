//=============================================================================//
//
// Purpose: stamp the dedi weapon-sim window's latestPredictedTime from the
// executing command's command_time (bounded), so client and server derive
// every weapon time from the same clock for the same command.
//
//=============================================================================//
#ifndef BRIDGE_FIRE_CLOCK_H
#define BRIDGE_FIRE_CLOCK_H
#ifdef _WIN32
#pragma once
#endif

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VBridgeFireClock : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const;
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BRIDGE_FIRE_CLOCK_H
