//=============================================================================//
//
// Purpose: mantle_boost on S3 TraversalMove -- sweet-spot trigger pre-orig,
// Region-3 finish boost post-orig.
//
//=============================================================================//
#ifndef MANTLE_BOOST_BRIDGE_H
#define MANTLE_BOOST_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"

class CPlayer;

// Gap B query: does THIS player currently have lurch/tap-strafe suppressed by
// mantle_boost (state==4)? Called from tapstrafe.cpp at its own hook
// entry -- see mantle_boost.cpp's banner above the implementation.
bool MantleBoost_ShouldSuppressTapStrafe(const CPlayer* const player);

// Per-player FSM state: 0 idle, 1 hang, 3 mantle jump, 4 boost armed/airborne.
int MantleBoost_GetState(const CPlayer* const player);

///////////////////////////////////////////////////////////////////////////////
class VMantleBoostBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MANTLE_BOOST_BRIDGE_H
