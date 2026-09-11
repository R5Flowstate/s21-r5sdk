//=============================================================================//
//
// Purpose: player class-var natives, client half
//
//=============================================================================//
#ifndef GAME_CLIENT_CLASSVAR_NATIVES_CL_H
#define GAME_CLIENT_CLASSVAR_NATIVES_CL_H

#include "thirdparty/detours/include/idetour.h"

class CSquirrelVM;

void ClassVar_RegisterClientFunctions(CSquirrelVM* s);

// Returns true when the player's settings changed and the block was rebuilt.
inline bool(__fastcall* v_C_Player_ApplySettingsChange)(void*) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VClassVarNativesCl : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_Player_ApplySettingsChange", v_C_Player_ApplySettingsChange);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // GAME_CLIENT_CLASSVAR_NATIVES_CL_H
