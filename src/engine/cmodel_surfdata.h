//=============================================================================//
//
// Purpose: Restore the engine's own surface-data lookup contract on the S21
// client's shared per-model surface-data accessor.
//
//=============================================================================//
#ifndef SURFDATA_FALLBACK_GUARD_S21_H
#define SURFDATA_FALLBACK_GUARD_S21_H

#include "thirdparty/detours/include/idetour.h"

class VSurfDataFallbackGuardS21 : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // SURFDATA_FALLBACK_GUARD_S21_H
