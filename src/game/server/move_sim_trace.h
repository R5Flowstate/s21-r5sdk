//=============================================================================//
//
// Purpose: [MOVE-TRACE] per-command FullWalkMove state dump for prediction
// parity joins against the client twin.
//
//=============================================================================//
#ifndef MOVE_SIM_TRACE_H
#define MOVE_SIM_TRACE_H

#include "thirdparty/detours/include/idetour.h"

// Sampled from VJetDrive's FullWalkMove hook. That class owns the only attach.
void MoveSimTrace_BeforeFullWalkMove(void* ctx);
void MoveSimTrace_AfterFullWalkMove(void* ctx);

///////////////////////////////////////////////////////////////////////////////
class VMoveSimTrace : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MOVE_SIM_TRACE_H
