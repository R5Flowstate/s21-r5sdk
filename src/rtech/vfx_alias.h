//=============================================================================//
//
// Purpose: Skip weapon-inspect VFX-alias apply when the settings root is NULL.
//
//=============================================================================//
#ifndef VFX_ALIAS_NULL_GUARD_H
#define VFX_ALIAS_NULL_GUARD_H

#include "thirdparty/detours/include/idetour.h"

class VVfxAliasNullGuardS21 : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // VFX_ALIAS_NULL_GUARD_H
