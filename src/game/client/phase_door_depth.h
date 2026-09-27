//=============================================================================//
//
// Purpose: Alter tactical maximum wall depth (client placement).
//
//=============================================================================//
#ifndef CLIENT_PHASE_DOOR_DEPTH_H
#define CLIENT_PHASE_DOOR_DEPTH_H

#include "thirdparty/detours/include/idetour.h"

// Squared max entrance-to-exit depth read only by the placement exit search.
inline float* g_pPhaseDoorMaxDepthSqr = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VPhaseDoorDepth : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogVarAdr("PhaseDoorMaxDepthSqr", g_pPhaseDoorMaxDepthSqr);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_PHASE_DOOR_DEPTH_H
