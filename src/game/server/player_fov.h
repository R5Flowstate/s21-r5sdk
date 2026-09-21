//=============================================================================//
//
// Purpose: CPlayer.GetDefaultFOV() server native. S3 only exposes GetFOV on
// the client class; the S21 scripts size third-person camera distance off the
// player's own FOV setting.
//
//=============================================================================//
#ifndef PLAYER_FOV_H
#define PLAYER_FOV_H

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;

void PlayerFov_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

///////////////////////////////////////////////////////////////////////////////
class VPlayerFov : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // PLAYER_FOV_H
