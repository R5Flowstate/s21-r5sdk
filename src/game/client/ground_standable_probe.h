//=============================================================================//
//
// Purpose: client [GROUND-REJECT] probe -- names the collider CategorizePosition
// refuses to stand on, and which clause refused it.
//
//=============================================================================//
#ifndef GROUND_STANDABLE_PROBE_H
#define GROUND_STANDABLE_PROBE_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VGroundStandableProbe : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // GROUND_STANDABLE_PROBE_H
