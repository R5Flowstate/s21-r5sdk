//=============================================================================//
//
// Purpose: Portal surface renderer (client). Records world-space quads into
// the main scene's transparent pass, right after opaque geometry: a magenta
// test billboard (spike S2) and one quad per visible portal (L2). The quad
// shader is our own HLSL compiled at runtime; it touches no descriptor heap
// and rebinds no render target, so the engine's list state survives us.
//
//=============================================================================//
#ifndef CLIENT_PORTAL_RENDER_H
#define CLIENT_PORTAL_RENDER_H

#include "thirdparty/detours/include/idetour.h"

inline int64_t(*v_PortalSurf_Transparent)(int64_t a1, unsigned int* a2, int64_t a3,
	unsigned int a4, unsigned int a5, int64_t a6) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VPortalSurface : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("PortalSurf_Transparent", v_PortalSurf_Transparent);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_PORTAL_RENDER_H
