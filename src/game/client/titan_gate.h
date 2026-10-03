//=============================================================================//
//
// Purpose: client half of the titan class test (game/shared/titan_gate.h).
//
//=============================================================================//
#ifndef CLIENT_TITAN_GATE_H
#define CLIENT_TITAN_GATE_H

#include "thirdparty/detours/include/idetour.h"

// The local player when it is in a titan class.
bool TitanGate_IsLocalPlayerTitan(void);

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

#endif // CLIENT_TITAN_GATE_H
