//=============================================================================//
//
// Purpose: Resize DT_WeaponInventory.offhandWeapons from 6 to 8 elements.
//
//=============================================================================//
#ifndef ENGINE_SERVER_OFFHAND_DT_RESIZE_H
#define ENGINE_SERVER_OFFHAND_DT_RESIZE_H

#include "thirdparty/detours/include/idetour.h"

class VOffhandDTResize : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_SERVER_OFFHAND_DT_RESIZE_H
