//=============================================================================//
//
// Purpose: Author m_bClientSidePredicted on CTriggerCylinderHeavy. See
// trigger_clientpredict.h.
//
//=============================================================================//
#include "core/stdafx.h"


#include "trigger_clientpredict.h"

// CBaseTrigger::m_bClientSidePredicted -- one byte; the S21 client reads it to
// decide whether the cylinder joins the predicted-trigger partition.
static constexpr ptrdiff_t TRG_OFF_CLIENT_SIDE_PREDICTED = 0xCA8;

//-----------------------------------------------------------------------------
// Purpose: CTriggerCylinderHeavy ctor -- author m_bClientSidePredicted after
// the original body has initialised the entity.
//-----------------------------------------------------------------------------
static void* Hook_CTriggerCylinderHeavy_Ctor(void* pThis)
{
	void* const pRet = CTriggerCylinderHeavy__Ctor(pThis);

	if (pRet)
		*reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(pRet) + TRG_OFF_CLIENT_SIDE_PREDICTED) = 1;

	return pRet;
}

void VTriggerClientPredict::GetFun(void) const
{
	// CTriggerCylinderHeavy ctor. Anchored on the vtable store plus the first
	// field-clear at +0xD20; the two rel32/rip displacements (base-ctor call
	// and vtable lea) are wildcarded.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 45 33 C0 48 8D 05 ?? ?? ?? ?? 48 89 03 "
		"41 B9 00 02 00 00 4C 89 83 20 0D 00 00")
		.GetPtr(CTriggerCylinderHeavy__Ctor);

	if (!CTriggerCylinderHeavy__Ctor)
		Warning(eDLL_T::SERVER,
			"[TRIG-PRED] CTriggerCylinderHeavy::CTriggerCylinderHeavy pattern unresolved -- "
			"m_bClientSidePredicted stays 0 on every trigger\n");
}

void VTriggerClientPredict::Detour(const bool bAttach) const
{
	if (CTriggerCylinderHeavy__Ctor)
		DetourSetup(&CTriggerCylinderHeavy__Ctor, &Hook_CTriggerCylinderHeavy_Ctor, bAttach);
}

