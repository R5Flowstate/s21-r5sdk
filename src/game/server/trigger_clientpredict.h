//=============================================================================//
//
// Purpose: Author m_bClientSidePredicted on CTriggerCylinderHeavy so the
// S21 client can predict jump-pad touch.
//
//=============================================================================//
#ifndef TRIGGER_CLIENTPREDICT_H
#define TRIGGER_CLIENTPREDICT_H

#include "thirdparty/detours/include/idetour.h"

// CTriggerCylinderHeavy::CTriggerCylinderHeavy(this)
inline void* (*CTriggerCylinderHeavy__Ctor)(void* pThis) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VTriggerClientPredict : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CTriggerCylinderHeavy::CTriggerCylinderHeavy", CTriggerCylinderHeavy__Ctor);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TRIGGER_CLIENTPREDICT_H
