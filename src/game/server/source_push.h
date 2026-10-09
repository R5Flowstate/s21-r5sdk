//=============================================================================//
//
// Purpose: Source trigger_push on the dedicated server; the model is
// game/shared/source_push.h and the client twin is game/client/source_push.cpp.
//
//=============================================================================//
#ifndef SOURCE_PUSH_SERVER_H
#define SOURCE_PUSH_SERVER_H

#include "thirdparty/detours/include/idetour.h"

class CSquirrelVM;

// Bracket VJetDrive's FullWalkMove hook. That class owns the only attach.
void SourcePush_BeforeFullWalkMove(void* ctx);
void SourcePush_AfterFullWalkMove(void* ctx);
// Called from VWallLaunch's air accelerate hook, which owns that attach.
void SourcePush_AfterAirAccelerate(void* ctx);
// Horizontal push that carried this command's grounded move; zero when none.
const float* SourcePush_GroundPushUsed(void* ctx);

// SERVER VM natives: SourcePush_ClearVolumes, SourcePush_AddVolume.
void SourcePush_RegisterServerFunctions(CSquirrelVM* s);

///////////////////////////////////////////////////////////////////////////////
class VSourcePush : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SOURCE_PUSH_SERVER_H
