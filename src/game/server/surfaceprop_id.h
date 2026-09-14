//=============================================================================//
//
// Purpose: S3 surface-id lookup stops at 127; S21 does not.
//
//=============================================================================//
#ifndef SURFACEPROP_ID_H
#define SURFACEPROP_ID_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VSurfPropIdUnclamp : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SURFACEPROP_ID_H
