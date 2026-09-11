//=============================================================================//
//
// Purpose: NOP the weapon-mod "Unrecognized entry" logger CALL; parse continues.
//
//=============================================================================//
#ifndef ENGINE_SOFT_WARNINGS_H
#define ENGINE_SOFT_WARNINGS_H

#include "thirdparty/detours/include/idetour.h"

class VEngineSoftWarnings : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_SOFT_WARNINGS_H
