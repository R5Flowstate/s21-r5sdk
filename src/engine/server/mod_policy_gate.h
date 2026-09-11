//=============================================================================//
//
// Purpose: Enforce the dedicated server's required/allowed mod policy.
//
//=============================================================================//
#ifndef ENGINE_MOD_POLICY_GATE_H
#define ENGINE_MOD_POLICY_GATE_H
#include "thirdparty/detours/include/idetour.h"

inline void(*v_CHostState_State_GameShutDown)(class CHostState* thisptr) = nullptr;

void ModPolicyGate_OnFrame(void);
void ModPolicyGate_ResetAll(void);

///////////////////////////////////////////////////////////////////////////////
class VModPolicyGate : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CHostState::State_GameShutDown", v_CHostState_State_GameShutDown);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // !ENGINE_MOD_POLICY_GATE_H
