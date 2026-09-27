//=============================================================================//
//
// Purpose: player class-var natives (movement tuning from script)
//
//=============================================================================//
#ifndef GAME_SERVER_CLASSVAR_NATIVES_H
#define GAME_SERVER_CLASSVAR_NATIVES_H

#include "thirdparty/detours/include/idetour.h"

class CSquirrelVM;

void Script_RegisterClassVarNatives(CSquirrelVM* s);
void ClassVar_BindShipped(void);

///////////////////////////////////////////////////////////////////////////////
class VClassVarNatives : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // GAME_SERVER_CLASSVAR_NATIVES_H
