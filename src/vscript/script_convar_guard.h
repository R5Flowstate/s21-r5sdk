//=============================================================================//
//
// Purpose: keeps script (and therefore mod) code away from credential and
//          trust convars. The stock SetConVar*/GetConVarString natives accept
//          any name, and the dedi's ServerCommand runs any console line.
//
//=============================================================================//

#ifndef SCRIPT_CONVAR_GUARD_H
#define SCRIPT_CONVAR_GUARD_H

#include "thirdparty/detours/include/idetour.h"

bool ScriptConVarGuard_IsSecret(const char* pszName);
bool ScriptConVarGuard_IsWriteLocked(const char* pszName);

///////////////////////////////////////////////////////////////////////////////
class VScriptConVarGuard : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const {}
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SCRIPT_CONVAR_GUARD_H
