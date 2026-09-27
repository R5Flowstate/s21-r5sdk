//=============================================================================//
//
// Purpose: S21 jetpack on the dedicated server -- the fuel natives the S3
// script VM lacks and the S21 flight model the S21 client predicts.
//
//=============================================================================//
#ifndef JETPACK_BRIDGE_H
#define JETPACK_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"
#include <cstdint>

struct ScriptClassDescriptor_t;

void Jetpack_LevelShutdown(void);
void Jetpack_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

void Jetpack_ClearGround(uint8_t* pPlayer);
bool Jetpack_IsZiplining(void* pPlayer);

// Around CGameMovement::AirMove: while the release post effect runs, the
// engine's airDrag read is pointed at jetpackPostEffectDrag. True = End owed.
bool Jetpack_AirMoveBegin(void* pPlayer);
void Jetpack_AirMoveEnd(void);

///////////////////////////////////////////////////////////////////////////////
class VJetpackBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // JETPACK_BRIDGE_H
