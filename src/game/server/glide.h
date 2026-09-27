//=============================================================================//
//
// Purpose: S21 script-activated glide on the dedicated server -- engage,
// flight model, script natives and code callbacks the S21 client predicts.
//
//=============================================================================//
#ifndef GLIDE_BRIDGE_H
#define GLIDE_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"

class CPlayer;
class CUserCmd;
struct ScriptClassDescriptor_t;

void Glide_PreRunCommand(CPlayer* pPlayer, CUserCmd* pUserCmd);
void Glide_LevelShutdown(void);
void Glide_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

///////////////////////////////////////////////////////////////////////////////
class VGlideBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // GLIDE_BRIDGE_H
