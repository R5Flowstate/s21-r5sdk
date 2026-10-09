//=============================================================================//
//
// Purpose: Jump-pad parity on the dedi -- per-command touch, launch gates, and
// the launch state the pad scripts set, on the command the client predicts it.
//
//=============================================================================//
#ifndef JUMPPAD_PARITY_H
#define JUMPPAD_PARITY_H

#include "thirdparty/detours/include/idetour.h"

// JumpPad launch pass -- ctx+8 is the CPlayer, ctx+16 the CMoveData.
inline int64_t (*JumpPad__ApplyLaunchPass)(void* pCtx) = nullptr;

// Start and end of CGameMovement::PlayerMove for one command; the PlayerMove hook's owner calls both.
void JumpPad_OnPlayerMoveBegin(void* pCtx);
void JumpPad_OnPlayerMoveEnd(void* pCtx);

// A jump pad or gravity cannon launched the player on this command.
void JumpPad_OnLauncherLaunched(void* pPlayer);

///////////////////////////////////////////////////////////////////////////////
class VJumpPadParity : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("JumpPad::ApplyLaunchPass", JumpPad__ApplyLaunchPass);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // JUMPPAD_PARITY_H
