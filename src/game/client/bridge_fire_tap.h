//=============================================================================//
//
// Purpose: seed C_WeaponX::m_burstFireCount from the burst_fire_count
// modvar at predicted PrimaryAttack when the engine's own mod-application
// push left it at zero.
//
//=============================================================================//
#ifndef BRIDGE_FIRE_TAP_CLIENT_H
#define BRIDGE_FIRE_TAP_CLIENT_H
#ifdef _WIN32
#pragma once
#endif

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VBridgeFireTapClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const;
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BRIDGE_FIRE_TAP_CLIENT_H
