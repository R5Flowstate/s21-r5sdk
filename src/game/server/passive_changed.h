#ifndef PASSIVE_CHANGED_BRIDGE_H
#define PASSIVE_CHANGED_BRIDGE_H

//=============================================================================//
//
// Purpose: Bridge for passive-changed replication events
//
//=============================================================================//
#ifndef CLIENT_DLL

#include "thirdparty/detours/include/idetour.h"

//-----------------------------------------------------------------------------
// S3 GivePassive/RemovePassive write the bitfield but never dispatch
// CodeCallback_OnPassiveChanged. Fire it on a genuine flip only.
//-----------------------------------------------------------------------------
class VPassiveChangedBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // !CLIENT_DLL

#endif // PASSIVE_CHANGED_BRIDGE_H
