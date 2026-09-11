//=============================================================================//
//
// Purpose: Diagnose missing jumppad view punch on the S21 bridge client.
//
//=============================================================================//
#ifndef CLIENT_JUMPPAD_VIEWPUNCH_DIAG_H
#define CLIENT_JUMPPAD_VIEWPUNCH_DIAG_H

#include "thirdparty/detours/include/idetour.h"

// C_TriggerCylinderHeavy::StartTouch(this, other)
// S21 -- unique pattern verified.
inline void (*C_TriggerCylinderHeavy__StartTouch)(void* trigger, void* other) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VJumpPadViewPunchDiag : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_TriggerCylinderHeavy::StartTouch", C_TriggerCylinderHeavy__StartTouch);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

// C_TriggerCylinderNetworked::UpdatePartitionListEntry(this)
inline void (*C_TriggerCylinderNetworked__UpdatePartitionListEntry)(void* pThis) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VTriggerClientPredictForce : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_TriggerCylinderNetworked::UpdatePartitionListEntry",
			C_TriggerCylinderNetworked__UpdatePartitionListEntry);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_JUMPPAD_VIEWPUNCH_DIAG_H
