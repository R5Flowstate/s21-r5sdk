//=============================================================================//
//
// Purpose: "Visual clutter" setting (cl_visual_clutter). Minimal mode drops
// all weapon muzzle flashes and body/armor shield bursts at the client
// particle funnels, and pins cl_is_softened_locale so kill/death gibs use the
// softened path. See visual_clutter.cpp.
//
//=============================================================================//
#ifndef CLIENT_VISUAL_CLUTTER_H
#define CLIENT_VISUAL_CLUTTER_H

#include "thirdparty/detours/include/idetour.h"

// Merged PlayWeaponParticleEffect funnel: script PlayWeaponEffect* variants,
// viewmodel/worldmodel setup and the OnIdx resolve all bottom out here with
// plain effect names.
inline char(__fastcall* v_VisClutter_PlayWeaponFx)(int64_t, const char*, const char*, char, char, int, char, char, int) = nullptr;

// Anim-event weapon-FX dispatcher. a4 selects the event class; 18/20/115 is
// the weapon-FX branch (view+world name pair with a barrel selector).
inline char(__fastcall* v_VisClutter_AnimWeaponFx)(int64_t, int, int, int, int, void*, int64_t, int64_t) = nullptr;

// Entity-attached particle spawn. a2 is the ParticleEffectNames index; the
// engine's own CPU-culled path returns -1, which scripts already treat as an
// invalid handle.
inline int64_t(__fastcall* v_VisClutter_StartParticleOnEntity)(int64_t, int64_t, int64_t, int64_t, void*, int64_t, int, char, char) = nullptr;

// Index-based world particle create (ctx, index, origin). The impact-table
// dispatcher spawns "shieldhit" effects through it; callers null-check the
// result.
inline int64_t(__fastcall* v_VisClutter_CreateParticleByIndex)(int64_t, int, int64_t) = nullptr;

// Address of the client ParticleEffectNames network string table pointer.
// FindStringIndex at vtable slot 10 (+80) maps asset name to index (0xFFFF
// on a miss) and seeds the shield denylist at runtime.
inline void** s_ppVisClutterParticleMgr = nullptr;

// Engine cvars register after the detour bring-up, so the softened-locale
// pin is applied from the launch-override pass and on each cvar change.
void VisualClutter_ApplyPin(void);

///////////////////////////////////////////////////////////////////////////////
class VVisualClutter : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("VisClutter_PlayWeaponFx", v_VisClutter_PlayWeaponFx);
		LogFunAdr("VisClutter_AnimWeaponFx", v_VisClutter_AnimWeaponFx);
		LogFunAdr("VisClutter_StartParticleOnEntity", v_VisClutter_StartParticleOnEntity);
		LogFunAdr("VisClutter_CreateParticleByIndex", v_VisClutter_CreateParticleByIndex);
		LogVarAdr("VisClutter_ParticleMgr", reinterpret_cast<const void*>(s_ppVisClutterParticleMgr));
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_VISUAL_CLUTTER_H
