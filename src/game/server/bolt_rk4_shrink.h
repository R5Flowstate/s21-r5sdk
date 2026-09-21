//=============================================================================//
//
// Purpose: Bolt re-simulation surface-retry step shrink (dedi side).
//
//=============================================================================//
#ifndef BOLT_RK4_SHRINK_H
#define BOLT_RK4_SHRINK_H
#ifdef _WIN32
#pragma once
#endif

#include "thirdparty/detours/include/idetour.h"

inline uint8_t* g_pBoltResimRetrySite = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VBoltRk4Shrink : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogVarAdr("BoltResimRetrySite", g_pBoltResimRetrySite);
	}
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BOLT_RK4_SHRINK_H
