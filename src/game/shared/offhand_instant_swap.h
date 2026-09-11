//=============================================================================//
//
// Purpose: OR offhand_instant_swap_to_offhand at the dispatcher switch-away gate.
//
//=============================================================================//
#ifndef OFFHAND_INSTANT_SWAP_H
#define OFFHAND_INSTANT_SWAP_H

#include "thirdparty/detours/include/idetour.h"

class VOffhandInstantSwap : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // OFFHAND_INSTANT_SWAP_H
