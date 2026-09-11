//=============================================================================
//
// Purpose: widen offhand activation from 6 slots to 8; slots 6/7 resolve through the SDK L1 map.
//
//=============================================================================
#ifndef OFFHAND_ACTIVATION_PATCHES_H
#define OFFHAND_ACTIVATION_PATCHES_H

#include "thirdparty/detours/include/idetour.h"

class VOffhandActivationPatches : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

// Invokes CWeapon::Deploy so slot 6/7 Give populates the anim-event handler table (weapon+5752).
bool OffhandActivation_InvokeDeploy(void* pWeapon);

#ifndef CLIENT_DLL
void OffhandActivation_SetOneHanded(void* pPlayer, bool on);
bool OffhandActivation_TossDiagEnabled(void);
#endif // !CLIENT_DLL

#endif // OFFHAND_ACTIVATION_PATCHES_H
