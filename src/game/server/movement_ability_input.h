//=============================================================================//
//
// Purpose: server half of the movement-ability input routing at the S3 Jump.
//
//=============================================================================//
#ifndef SERVER_MOVEMENT_ABILITY_INPUT_H
#define SERVER_MOVEMENT_ABILITY_INPUT_H

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

#endif // SERVER_MOVEMENT_ABILITY_INPUT_H
