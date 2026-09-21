//=============================================================================//
//
// Purpose: ADS/spread divergence probe (dedi side), plus FireWeaponBolt
// owner-velocity parity with the S21 client.
//
//=============================================================================//
#ifndef BRIDGE_ZOOM_GATE_H
#define BRIDGE_ZOOM_GATE_H
#ifdef _WIN32
#pragma once
#endif

#include "thirdparty/detours/include/idetour.h"

bool ZoomGate_AkimboCanZoom(void);

///////////////////////////////////////////////////////////////////////////////
class VBridgeZoomGate : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const;
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BRIDGE_ZOOM_GATE_H
