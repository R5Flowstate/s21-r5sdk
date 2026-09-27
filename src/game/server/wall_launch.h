//=============================================================================//
//
// Purpose: Sparrow wall launch on the dedicated server -- the climb high jump
// out of a wall jump and the edge air control that follows it.
//
//=============================================================================//
#ifndef WALL_LAUNCH_H
#define WALL_LAUNCH_H

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;

// Sampled from VJetDrive's FullWalkMove hook. That class owns the only attach.
void WallLaunch_AfterFullWalkMove(void* ctx);
void WallLaunch_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

///////////////////////////////////////////////////////////////////////////////
class VWallLaunch : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WALL_LAUNCH_H
