//=============================================================================//
//
// Purpose: client prediction twin of the Sparrow wall launch
// (game/server/wall_launch.cpp).
//
//=============================================================================//
#ifndef WALL_LAUNCH_CLIENT_H
#define WALL_LAUNCH_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

class CSquirrelVM;

// Bracket VMoveSimTraceClient's FullWalkMove hook. That class owns the only attach.
void WallLaunchClient_BeforeFullWalkMove(void* ctx);
void WallLaunchClient_AfterFullWalkMove(void* ctx);

// Late CLIENT VM natives: SetLocalWallLaunchEnabled, HasLocalUsedWallLaunch.
void WallLaunchClient_RegisterClientFunctions(CSquirrelVM* s);

///////////////////////////////////////////////////////////////////////////////
class VWallLaunchClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WALL_LAUNCH_CLIENT_H
