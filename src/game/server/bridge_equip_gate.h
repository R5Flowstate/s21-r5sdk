//=============================================================================//
//
// Purpose: S3 Weapon_IsPlaying3pEquipActivity accepts the full ACT_MP_EQUIP_* block.
//
//=============================================================================//
#ifndef BRIDGE_EQUIP_GATE_H
#define BRIDGE_EQUIP_GATE_H
#ifdef _WIN32
#pragma once
#endif

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VBridgeEquipGate : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const;
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BRIDGE_EQUIP_GATE_H
