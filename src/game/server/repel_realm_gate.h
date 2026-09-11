//=============================================================================//
//
// Purpose: cross-realm player repel gate. See repel_realm_gate.cpp.
//
//=============================================================================//
#ifndef REPEL_REALM_GATE_H
#define REPEL_REALM_GATE_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VRepelRealmGate : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // REPEL_REALM_GATE_H
