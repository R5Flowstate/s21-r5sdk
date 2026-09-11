//=============================================================================//
//
// Purpose: Free the live reliable-receive buffer before the first-fragment
// store overwrites it (leak-on-reenter).
//
//=============================================================================//
#ifndef ENGINE_SERVER_NETCHAN_RELRECV_FREE_H
#define ENGINE_SERVER_NETCHAN_RELRECV_FREE_H

#include "thirdparty/detours/include/idetour.h"

class VNetChanRelRecvFree : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_SERVER_NETCHAN_RELRECV_FREE_H
