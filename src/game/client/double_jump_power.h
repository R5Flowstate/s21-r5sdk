//=============================================================================//
//
// Purpose: client prediction twin of Sparrow's double-jump power
// (game/server/double_jump_power.cpp).
//
//=============================================================================//
#ifndef DOUBLE_JUMP_POWER_H_CLIENT
#define DOUBLE_JUMP_POWER_H_CLIENT

#include "thirdparty/detours/include/idetour.h"

class CSquirrelVM;

// Late CLIENT VM native: SetLocalDoubleJumpPowerEnabled.
void DoubleJumpPowerClient_RegisterClientFunctions(CSquirrelVM* s);

///////////////////////////////////////////////////////////////////////////////
class VDoubleJumpPowerClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // DOUBLE_JUMP_POWER_H_CLIENT
