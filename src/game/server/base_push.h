//=============================================================================//
//
// Purpose: script-driven base velocity that also carries a grounded player
// (Source-style push triggers).
//
//=============================================================================//
#ifndef BASE_PUSH_H
#define BASE_PUSH_H

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;

void BasePush_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

///////////////////////////////////////////////////////////////////////////////
class VBasePush : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BASE_PUSH_H
