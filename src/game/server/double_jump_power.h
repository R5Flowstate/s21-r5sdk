//=============================================================================//
//
// Purpose: Sparrow's double-jump power on the dedicated server -- a full
// meter per double jump that refills over time instead of on landing.
//
//=============================================================================//
#ifndef DOUBLE_JUMP_POWER_H_SERVER
#define DOUBLE_JUMP_POWER_H_SERVER

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;

void DoubleJumpPower_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

///////////////////////////////////////////////////////////////////////////////
class VDoubleJumpPower : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // DOUBLE_JUMP_POWER_H_SERVER
