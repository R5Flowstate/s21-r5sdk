//=============================================================================//
//
// Purpose: server half of the titan class test (game/shared/titan_gate.h).
//
//=============================================================================//
#ifndef SERVER_TITAN_GATE_H
#define SERVER_TITAN_GATE_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VTitanGate : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // SERVER_TITAN_GATE_H
