//=============================================================================//
//
// Purpose: client half of the movement-ability input routing.
//
//=============================================================================//
#ifndef CLIENT_MOVEMENT_ABILITY_INPUT_H
#define CLIENT_MOVEMENT_ABILITY_INPUT_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VMovementAbilityInput : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_MOVEMENT_ABILITY_INPUT_H
