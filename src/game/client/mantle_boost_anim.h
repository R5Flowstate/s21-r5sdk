//=============================================================================//
//
// Purpose: ACT_MP_MANTLE_BOOST_AIR selection in the player animstate.
//
//=============================================================================//
#ifndef MANTLE_BOOST_ANIM_CLIENT_H
#define MANTLE_BOOST_ANIM_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

// C_MultiPlayerAnimState::IsOnGround: FL_ONGROUND with the post-jump and dodge
// grace windows the activity chain itself uses.
inline char (*C_MultiPlayerAnimState__IsOnGround)(void* pAnimState) = nullptr;

// Replicated m_mantleBoostState of any player, captured from the wire so other
// players' third-person animstates can select the boost-air activity.
void MantleBoostAnim_OnWireState(int nEdict, int nState);
void MantleBoostAnim_OnSessionReset(void);

///////////////////////////////////////////////////////////////////////////////
class VMantleBoostAnimClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MANTLE_BOOST_ANIM_CLIENT_H
