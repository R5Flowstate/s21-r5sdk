//=============================================================================//
//
// Purpose: Keep the pak relink drain from spinning forever.
//
//=============================================================================//
#pragma once
#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VPakRelinkGuard : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
