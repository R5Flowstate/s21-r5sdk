//=============================================================================//
//
// Purpose: Call CodeCallback_CanUseEntity with its declared arguments from the
//          player-inside-titan "use self" check.
//
//=============================================================================//
#ifndef CLIENT_TITAN_USE_SELF_H
#define CLIENT_TITAN_USE_SELF_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VTitanUseSelf : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_TITAN_USE_SELF_H
