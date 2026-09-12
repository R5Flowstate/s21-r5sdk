//=============================================================================//
//
// Purpose: Raise the S21 client FOV ceiling to 120 (render cap + text box)
// and expose a degrees-based setter that bypasses the menu slider.
//
//=============================================================================//
#ifndef CLIENT_FOV_LIMIT_H
#define CLIENT_FOV_LIMIT_H

#include "thirdparty/detours/include/idetour.h"

// Shared render-cap float: GetClientFOV and the ADS/zoom helpers all clamp
// through this one dword. Proven by xref, asserted again at resolve time.
inline float* g_pFovRenderCap = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VFOVLimit : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogVarAdr("FOVSharedRenderCap", g_pFovRenderCap);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_FOV_LIMIT_H
