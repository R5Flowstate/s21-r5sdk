#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: StatusEffect_GetTotalSeverity - SDK native implementation
// Provides SUM semantics for status effect severity accumulation.
//
//=============================================================================//
#ifndef STATUS_EFFECTS_SDK_H
#define STATUS_EFFECTS_SDK_H


#endif // STATUS_EFFECTS_SDK_H
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: StatusEffect_GetTotalSeverity - SDK native implementation
// Provides SUM semantics for status effect severity accumulation.
//
//=============================================================================//
#ifndef STATUS_EFFECTS_SDK_H
#define STATUS_EFFECTS_SDK_H

#include "thirdparty/detours/include/idetour.h"

class CSquirrelVM;

void StatusEffects_SDK_RegisterServerFunctions(CSquirrelVM* vm);
void StatusEffects_SDK_RegisterClientFunctions(CSquirrelVM* vm);
void StatusEffects_SDK_RegisterUIFunctions(CSquirrelVM* vm);

///////////////////////////////////////////////////////////////////////////////
// NOP the S3 parser's fatal log when a code-required effect name is absent.
///////////////////////////////////////////////////////////////////////////////
class VStatusEffectParseFix : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // STATUS_EFFECTS_SDK_H
#endif // CLIENT_DLL
