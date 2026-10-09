//=============================================================================//
//
// Purpose: S21 'offhand_deactivate_on_jump_toggle_or_release' on the dedicated
// server -- a held jump selects the offhand, a jump press holsters it.
//
//=============================================================================//
#ifndef OFFHAND_JUMP_TOGGLE_H
#define OFFHAND_JUMP_TOGGLE_H

#include "thirdparty/detours/include/idetour.h"

// CWeaponX::PlayerWeapon_BusyFrame is detoured by VWeaponHeat; that hook reports here after the engine body.
void OffhandJumpToggle_OnBusyFrame(void* pWeapon);
// Diagnostic only: VTranslocation's HolsterInternal hook reports every holster here.
void OffhandJumpToggle_OnHolster(void* pWeapon, bool bFast, const void* pCaller);
// The main hand holds a jump-toggle offhand that this command's offhand frame
// has already holstered; its deactivate callback runs after movement.
bool OffhandJumpToggle_IsReleasing(const void* pPlayer);
// The glide ended on the ground: a jump offhand still charging in the main hand
// holsters now instead of waiting for the jump button to be released.
void OffhandJumpToggle_ReleaseOnLanding(void* pPlayer);
// Player_SwitchToOffhand is detoured by VOffhandActivationPatches; that hook reports the switched offhand here.
void OffhandJumpToggle_PostSwitchToOffhand(void* pPlayer, void* pOffhand);

///////////////////////////////////////////////////////////////////////////////
class VOffhandJumpToggle : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // OFFHAND_JUMP_TOGGLE_H
