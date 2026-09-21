//=============================================================================//
//
// Purpose: After RecalcMods rebuilds m_modVars from the replicated bitfield,
// call RequestBodygroupUpdate so optic/attachment meshes apply while the
// weapon is still held. Native RecalcMods never does this; only deploy
// (SetWeaponModel) and a realtime-mod queue pop do. Inventory SetMods is
// server-script and never queues, so a custom weapon with a base activity
// modifier (alt_anim) keeps the pre-attach bodygroups until holster.
//
//=============================================================================//
#ifndef WEAPON_MOD_VISUAL_H
#define WEAPON_MOD_VISUAL_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
// False while the owner's viewmodel for this weapon still shows another
// weapon's model (deploy pending); bodygroup writes against it would index
// a bodygroup table that model does not have.
bool WeaponModVisual_ViewmodelShowsWeapon(void* pWeapon);

class VWeaponModVisual : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_MOD_VISUAL_H
