//=============================================================================//
//
// Purpose: Skip unrelocated rpak PagePtrs in the shdr destructor walk.
//
//=============================================================================//
#ifndef SHADER_TEARDOWN_GUARD_S21_H
#define SHADER_TEARDOWN_GUARD_S21_H

#include "thirdparty/detours/include/idetour.h"

inline void(__fastcall* v_ShaderDestroy)(__int64) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VShaderTeardownGuardS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("ShaderDestroy", v_ShaderDestroy);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SHADER_TEARDOWN_GUARD_S21_H
