//=============================================================================//
//
// Purpose: SNDC registration-layer hooks -- AllocateInternalVar, category bound,
// scriptNetCategories 7-entry copy so cats 5-6 do not OOB the 5-entry array.
//
//=============================================================================//
#ifndef SNDC_ALLOC_HOOK_H
#define SNDC_ALLOC_HOOK_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VSNDCAllocHook : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SNDC_ALLOC_HOOK_H
