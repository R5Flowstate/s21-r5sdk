//=============================================================================//
//
// Purpose: client half of the airborne dodge rules around C_GameMovement::Jump.
//
//=============================================================================//
#ifndef CLIENT_DODGE_RULES_H
#define CLIENT_DODGE_RULES_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VDodgeRules : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_DODGE_RULES_H
