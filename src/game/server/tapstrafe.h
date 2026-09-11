//=============================================================================//
//
// Purpose: S21 mantle_boost suppression gate for native S3 tap-strafe.
// Masks directional press bits around AirMove while state==4.
//
//=============================================================================//
#ifndef TAPSTRAFE_BRIDGE_H
#define TAPSTRAFE_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VTapStrafeBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TAPSTRAFE_BRIDGE_H
