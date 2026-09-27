//=============================================================================//
//
// Purpose: Server-authoritative skyward launch. S3 dedi has none.
//
//=============================================================================//
#ifndef SKYWARD_BRIDGE_H
#define SKYWARD_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"
#include <cstdint>

struct ScriptClassDescriptor_t;

void SkywardBridge_PreRunCommand(void* pPlayer);
void SkywardBridge_PreTossMove(int64_t movement);
void SkywardBridge_PostTossMove(int64_t movement);
bool SkywardBridge_IsActive(const void* pPlayer);
void SkywardBridge_LevelShutdown(void);
bool SkywardBridge_WireEnabled(void);
void SkywardBridge_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

///////////////////////////////////////////////////////////////////////////////
class VSkywardBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SKYWARD_BRIDGE_H
