//=============================================================================//
//
// Purpose: S3 CWeaponX::DeployWeapon first-raise / sprint order parity with S21.
//
//=============================================================================//
#ifndef BRIDGE_DEPLOY_GATE_H
#define BRIDGE_DEPLOY_GATE_H
#ifdef _WIN32
#pragma once
#endif

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VBridgeDeployGate : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const;
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BRIDGE_DEPLOY_GATE_H
