//=============================================================================//
//
// Purpose: Tripwire for a zero dt reaching _Host_RunFrame, plus an optional census.
//
//=============================================================================//
#ifndef ENGINE_CLIENT_HOST_FRAME_PROBE_H
#define ENGINE_CLIENT_HOST_FRAME_PROBE_H

#include "thirdparty/detours/include/idetour.h"

class VHostFrameProbe : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_CLIENT_HOST_FRAME_PROBE_H
