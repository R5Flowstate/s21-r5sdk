//=============================================================================//
//
// Purpose: client prediction twin of the Source trigger_push
// (game/server/source_push.cpp).
//
//=============================================================================//
#ifndef SOURCE_PUSH_CLIENT_H
#define SOURCE_PUSH_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

class CSquirrelVM;

// Bracket VMoveSimTraceClient's FullWalkMove hook. That class owns the only attach.
void SourcePushClient_BeforeFullWalkMove(void* ctx);
void SourcePushClient_AfterFullWalkMove(void* ctx);
// Called from VWallLaunchClient's air accelerate hook, which owns that attach.
void SourcePushClient_AfterAirAccelerate(void* ctx);
// Horizontal push that carried this command's grounded move; zero when none.
const float* SourcePushClient_GroundPushUsed(void* ctx);

// Late CLIENT VM natives: SourcePush_ClearVolumes, SourcePush_AddVolume.
void SourcePushClient_RegisterClientFunctions(CSquirrelVM* s);

///////////////////////////////////////////////////////////////////////////////
class VSourcePushClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SOURCE_PUSH_CLIENT_H
