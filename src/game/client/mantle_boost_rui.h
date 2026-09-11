//=============================================================================//
//
// Purpose: the six client getters the mantle-boost timing RUI reads every
// frame. Every value is local-player-only, so these register as free functions
// rather than as C_Player members.
//
//=============================================================================//
#ifndef GAME_CLIENT_MANTLE_BOOST_RUI_CL_H
#define GAME_CLIENT_MANTLE_BOOST_RUI_CL_H

#include "thirdparty/detours/include/idetour.h"

class CSquirrelVM;

void MantleBoostRui_RegisterClientFunctions(CSquirrelVM* s);

// Bakes the S21 traversal camera curve for the current traversal state into
// platform/cfg/mantle_boost_curves.txt (see game/shared/mantle_boost_curves.h).
// Called per TraversalMove tick on the local player; no-op once baked.
void MantleBoostCurveDump_Think(uintptr_t pPlayer);

// C_Player::GetViewVector -- AngleVectors(GetAimAngles()), i.e. the aim
// direction including punch and view drift.
inline void* (*v_C_Player_GetViewVector)(void* pPlayer, void* pOutVec) = nullptr;

// C_BaseAnimating::SetCycle. Also latches the cached cycle and the frame stamp,
// so no separate cycle-advance call is needed to hold the pose.
inline void (*v_C_BaseAnimating_SetCycle)(void* pEnt, float flCycle) = nullptr;

// C_Player::EyeAngles. The sweet-spot scrub has to seed the traversal view
// sampler with the same angles the trigger gate uses, or the on-screen window
// and the gate disagree.
inline void* (*v_C_Player_EyeAngles)(void* pPlayer, void* pOutAngles) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VMantleBoostRuiCl : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_Player::GetViewVector", v_C_Player_GetViewVector);
		LogFunAdr("C_BaseAnimating::SetCycle", v_C_BaseAnimating_SetCycle);
		LogFunAdr("C_Player::EyeAngles", v_C_Player_EyeAngles);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // GAME_CLIENT_MANTLE_BOOST_RUI_CL_H
