//=============================================================================//
//
// Purpose: client [MOVE-TRACE] twin of src\game\server\move_sim_trace.cpp.
//
//=============================================================================//
#ifndef MOVE_SIM_TRACE_CLIENT_H
#define MOVE_SIM_TRACE_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VMoveSimTraceClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MOVE_SIM_TRACE_CLIENT_H
