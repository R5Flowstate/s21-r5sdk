//=============================================================================//
//
// Purpose: Relax UpdateAcknowledgedFramecount so the dedi accepts late S21 acks.
//
//=============================================================================//
#ifndef ENGINE_SERVER_ACK_GATE_RELAX_H
#define ENGINE_SERVER_ACK_GATE_RELAX_H

#include "thirdparty/detours/include/idetour.h"

class VAckGateRelax : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_SERVER_ACK_GATE_RELAX_H
