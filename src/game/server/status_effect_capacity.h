//=============================================================================//
//
// Purpose: 256 status-effect types on the dedicated server (the S21 client
// already has them). Widens the type field in seComboVars to 8 bits.
//
//=============================================================================//
#ifndef STATUS_EFFECT_CAPACITY_H
#define STATUS_EFFECT_CAPACITY_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VStatusEffectCapacity : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // STATUS_EFFECT_CAPACITY_H
