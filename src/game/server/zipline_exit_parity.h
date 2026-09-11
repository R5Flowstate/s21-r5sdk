//=============================================================================//
//
// Purpose: apply the client's zipline auto-detach exit-velocity rewrite on the
// dedicated server. See zipline_exit_parity.cpp.
//
//=============================================================================//
#ifndef ZIPLINE_EXIT_PARITY_H
#define ZIPLINE_EXIT_PARITY_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VZiplineExitParity : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ZIPLINE_EXIT_PARITY_H
