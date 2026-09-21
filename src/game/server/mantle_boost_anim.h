//=============================================================================//
//
// Purpose: ACT_MP_MANTLE_BOOST_AIR selection in the server player animstate.
//
//=============================================================================//
#ifndef MANTLE_BOOST_ANIM_SERVER_H
#define MANTLE_BOOST_ANIM_SERVER_H

#include "thirdparty/detours/include/idetour.h"

// CMultiPlayerAnimState::IsOnGround: FL_ONGROUND with the post-jump and dodge
// grace windows the activity chain itself uses.
inline char (*CMultiPlayerAnimState__IsOnGround)(void* pAnimState) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VMantleBoostAnimServer : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MANTLE_BOOST_ANIM_SERVER_H
