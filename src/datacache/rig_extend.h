//=============================================================================//
//
// Purpose: append sequences to a resident animrig's list at virtual-model
//          build time, from platform/rig_extend.txt ("<rig guid> <seq guid>").
//
//=============================================================================//
#ifndef DATACACHE_RIG_EXTEND_H
#define DATACACHE_RIG_EXTEND_H

#include "thirdparty/detours/include/idetour.h"

class VRigExtend : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // DATACACHE_RIG_EXTEND_H
