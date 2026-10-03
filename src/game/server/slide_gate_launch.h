//=============================================================================//
//
// Purpose: Nitro Gate launch for the dedicated server. A gate contact sets the
// player's horizontal velocity along their travel direction and begins a real
// slide through the movement code, so the slide state, the slide jump and the
// stand/jump-out rules are the native ones.
//
//=============================================================================//
#ifndef SLIDE_GATE_LAUNCH_H
#define SLIDE_GATE_LAUNCH_H

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;

void SlideGateLaunch_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

// CGameMovement::FullWalkMove entry: consumes a pending launch for the mover.
void SlideGateLaunch_BeginFullWalkMove(void* ctx);

///////////////////////////////////////////////////////////////////////////////
class VSlideGateLaunch : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // SLIDE_GATE_LAUNCH_H
