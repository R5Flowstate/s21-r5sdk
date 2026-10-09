//=============================================================================//
//
// Purpose: prediction twin of the dedi's jump pad launch state
// (game/server/jumppad_parity.cpp): flight slow-mo and the relaunch ground
// reset the pad scripts apply server-side, on the command they happen, and the
// live tick that keeps a new pad from launching before this side has it.
//
//=============================================================================//
#ifndef CLIENT_JUMPPAD_PREDICT_H
#define CLIENT_JUMPPAD_PREDICT_H

#include "thirdparty/detours/include/idetour.h"

// C_GameMovement jump pad launch (S21): ctx+8 is the C_Player, a2 the touched
// C_TriggerCylinderHeavy. Writes m_jumpPadDebounceExpireTime only when it launches.
inline char (*C_GameMovement__JumpPadLaunch)(void* ctx, void* pTrigger) = nullptr;

// Around C_GameMovement::PlayerMove for one predicted command; called from that hook's owner.
void JumpPadPredict_OnPlayerMoveBegin(void* ctx);
void JumpPadPredict_OnPlayerMoveEnd(void* ctx);

// A jump pad or gravity cannon launched the local player on this command.
void JumpPadPredict_OnLauncherLaunched(void* pPlayer);

///////////////////////////////////////////////////////////////////////////////
class VJumpPadPredict : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_GameMovement::JumpPadLaunch", C_GameMovement__JumpPadLaunch);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_JUMPPAD_PREDICT_H
