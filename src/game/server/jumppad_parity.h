//=============================================================================//
//
// Purpose: Jump-pad parity on the dedi -- ducked m_vertOverride scale during
// the launch pass, and per-player relaunch debounce authored into
// m_jumpPadDebounceExpireTime so the client stops re-punching every tick.
//
//=============================================================================//
#ifndef JUMPPAD_PARITY_H
#define JUMPPAD_PARITY_H

#include "thirdparty/detours/include/idetour.h"

// JumpPad launch pass -- ctx+8 is the CPlayer, ctx+16 the CMoveData.
inline int64_t (*JumpPad__ApplyLaunchPass)(void* pCtx) = nullptr;

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
