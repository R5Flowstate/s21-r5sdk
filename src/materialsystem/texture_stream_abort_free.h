//=============================================================================//
//
// Purpose: Free the texture streamer's staging block when a mip request set
// aborts. The engine frees it only on the success path.
//
//=============================================================================//
#ifndef TEXTURE_STREAM_ABORT_FREE_H
#define TEXTURE_STREAM_ABORT_FREE_H
#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VTextureStreamAbortFree : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TEXTURE_STREAM_ABORT_FREE_H
