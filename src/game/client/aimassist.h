//=============================================================================//
//
// Purpose: Client aim-assist magnet / pull / sniper-scope miss / look PLV.
//
//=============================================================================//
#ifndef CLIENT_AIMASSIST_H
#define CLIENT_AIMASSIST_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VAimAssist : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_AIMASSIST_H
