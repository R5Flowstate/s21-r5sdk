//=============================================================================//
//
// Purpose: Weapon heat shared declarations
//
//=============================================================================//
#if defined(CLIENT_DLL)
// Server-only. The S21 client owns heat natively; duplicates here shadowed its natives
// and wrote S3 struct offsets that address nothing on the client.
#else // !CLIENT_DLL
#ifndef WEAPON_HEAT_H
#define WEAPON_HEAT_H

#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;

void WeaponHeat_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);
void WeaponHeat_LevelShutdown();

// Driven from the engine's per-usercmd weapon functions so both sides write the same networked memory.
void WeaponHeat_Derive(void* pWeapon);   // decay + fully-heated release + modvar ramp
void WeaponHeat_OnFired(void* pWeapon);  // per-shot accumulation + fully-heated latch

// Wire accessors: appended slots alias live m_modVars, so heat lives in a sidecar.
float WeaponHeat_WireGetHeatValue(void* pWeapon);
float WeaponHeat_WireGetHeatValueOnLastFire(void* pWeapon);
int   WeaponHeat_WireGetFullyHeated(void* pWeapon);

// charge_overheats_when_full && m_lastChargeFrac == 1.0. Raw field reads only --
// safe from the snapshot pack worker (WeapState_XlatProxy).
bool WeaponHeat_IsChargeOverheated(void* pWeapon);

// Copies the weapon KV's burst_fire_count into m_burstFireCount, which the S3
// server never does for itself. Runs ahead of both heat passes because the
// fully-heated latch is only correct once the count is right.
void WeaponHeat_SeedBurstFireCount(void* pWeapon);

SQRESULT Script_Global_Weapon_GetBaseClassName(HSQUIRRELVM v);
SQRESULT Script_Global_Weapon_GetBaseClassNameOrEmpty(HSQUIRRELVM v);

// Engine: PlayWeaponEffectNoCull native SQRESULT function (parses args from VM stack)
inline SQRESULT(*v_PlayWeaponEffectNoCull_Native)(HSQUIRRELVM v) = nullptr;

// CWeaponX::GetChargeFraction: linear [0,1] in xmm0, rcx = weapon.
inline float(__fastcall* v_CWeaponX_GetChargeFraction)(void* pWeapon) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VWeaponHeat : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WEAPON_HEAT_H
#endif // CLIENT_DLL
