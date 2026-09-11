#if defined(CLIENT_DLL)
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
//===========================================================================//
#ifndef UTIL_SHARED_H
#define UTIL_SHARED_H
#include "public/engine/IEngineTrace.h"

class CTraceFilterSimple;
class CBaseEntity; // Server-only type; used here only as an opaque pointer (UTIL_GetEntityScriptInfo has no callers linked into the client build).
typedef bool (*ShouldHitFunc_t)(IHandleEntity* pHandleEntity, int contentsMask);

const char* UTIL_GetEntityScriptInfo(CBaseEntity* pEnt);

inline bool(*v_TraceFilter_ShouldHitEntity)(IHandleEntity* pHandleEntity, const IHandleEntity* pPassEntity, const IHandleEntity* pOtherEntity, const int contentsMask, const int collisionGroup);
inline const char*(*v_UTIL_GetEntityScriptInfo)(CBaseEntity* pEnt);

//-----------------------------------------------------------------------------
// traceline methods
//-----------------------------------------------------------------------------
class CTraceFilterSimple : public CTraceFilter
{
public:
	// It does have a base, but we'll never network anything below here..
	//DECLARE_CLASS_NOBASE(CTraceFilterSimple);

	CTraceFilterSimple(const IHandleEntity* pPassEntity, const int collisionGroup, ShouldHitFunc_t pExtraShouldHitCheckFn = NULL);

	virtual bool ShouldHitEntity(IHandleEntity* const pEntity, const int contentsMask);
	virtual bool ShouldBlockTrace(trace_t* const pTrace);

	virtual void SetPassEntity(const IHandleEntity* pPassEntity) { m_pPassEntity = pPassEntity; }
	virtual void SetCollisionGroup(const int iCollisionGroup) { m_collisionGroup = iCollisionGroup; }

	virtual void SetExtraShouldHitFunc(ShouldHitFunc_t shouldHitFunc) { m_pExtraShouldHitCheckFunction = shouldHitFunc; }

	const IHandleEntity* GetPassEntity(void) { return m_pPassEntity; }
	int GetCollisionGroup(void) const { return m_collisionGroup; }

private:
	int m_reserved; // Likely debug-only; no use cases found in the engine.
	const IHandleEntity* m_pPassEntity;
	ShouldHitFunc_t m_pExtraShouldHitCheckFunction;
	int m_collisionGroup;
};

///////////////////////////////////////////////////////////////////////////////
class V_UTIL_Shared : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("TraceFilter_ShouldHitEntity", v_TraceFilter_ShouldHitEntity);
		LogFunAdr("UTIL_GetEntityScriptInfo", v_UTIL_GetEntityScriptInfo);
	}
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const
	{
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 3B 5F").FollowNearCallSelf().GetPtr(v_TraceFilter_ShouldHitEntity);
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 4C 8B 5E ??").FollowNearCallSelf().GetPtr(v_UTIL_GetEntityScriptInfo);
		if (!v_TraceFilter_ShouldHitEntity)
			Warning(eDLL_T::CLIENT, "[UTIL_Shared] TraceFilter_ShouldHitEntity pattern unresolved\n");
		if (!v_UTIL_GetEntityScriptInfo)
			Warning(eDLL_T::CLIENT, "[UTIL_Shared] UTIL_GetEntityScriptInfo pattern unresolved\n");
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // !UTIL_SHARED_H
#else // !CLIENT_DLL
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
//===========================================================================//
#ifndef UTIL_SHARED_H
#define UTIL_SHARED_H
#include "public/engine/IEngineTrace.h"

class CTraceFilterSimple;
typedef bool (*ShouldHitFunc_t)(IHandleEntity* pHandleEntity, int contentsMask);

const char* UTIL_GetEntityScriptInfo(CBaseEntity* pEnt);

inline bool(*v_TraceFilter_ShouldHitEntity)(IHandleEntity* pHandleEntity, const IHandleEntity* pPassEntity, const IHandleEntity* pOtherEntity, const int contentsMask, const int collisionGroup);
inline const char*(*v_UTIL_GetEntityScriptInfo)(CBaseEntity* pEnt);

// SimulateGrenadeImpactPos launch state + bounce helpers (server flavors).
// See ServerScript_SimulateGrenadeImpactPos in vscript_server.cpp.
inline void(*v_WeaponX_GetFiringPositionAndAimDirection)(void* weapon, void* player, Vector3D* outPos, Vector3D* outDir);
inline Vector3D*(*v_WeaponX_GrenadeVelocityFromDir)(void* weapon, Vector3D* result, const Vector3D* dir, bool fromNpcThrow);
inline void(*v_CalculateBounceVelocity)(Vector3D* velocity, const Vector3D* normal, const float* modVars, bool* rollOut, bool forceRollFrac);

//-----------------------------------------------------------------------------
// traceline methods
//-----------------------------------------------------------------------------
class CTraceFilterSimple : public CTraceFilter
{
public:
	// It does have a base, but we'll never network anything below here..
	//DECLARE_CLASS_NOBASE(CTraceFilterSimple);

	CTraceFilterSimple(const IHandleEntity* pPassEntity, const int collisionGroup, ShouldHitFunc_t pExtraShouldHitCheckFn = NULL);

	virtual bool ShouldHitEntity(IHandleEntity* const pEntity, const int contentsMask);
	virtual bool ShouldBlockTrace(trace_t* const pTrace);

	virtual void SetPassEntity(const IHandleEntity* pPassEntity) { m_pPassEntity = pPassEntity; }
	virtual void SetCollisionGroup(const int iCollisionGroup) { m_collisionGroup = iCollisionGroup; }

	virtual void SetExtraShouldHitFunc(ShouldHitFunc_t shouldHitFunc) { m_pExtraShouldHitCheckFunction = shouldHitFunc; }

	const IHandleEntity* GetPassEntity(void) { return m_pPassEntity; }
	int GetCollisionGroup(void) const { return m_collisionGroup; }

private:
	int m_reserved; // Likely debug-only; no use cases found in the engine.
	const IHandleEntity* m_pPassEntity;
	ShouldHitFunc_t m_pExtraShouldHitCheckFunction;
	int m_collisionGroup;
};

///////////////////////////////////////////////////////////////////////////////
class V_UTIL_Shared : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("TraceFilter_ShouldHitEntity", v_TraceFilter_ShouldHitEntity);
		LogFunAdr("UTIL_GetEntityScriptInfo", v_UTIL_GetEntityScriptInfo);
		LogFunAdr("WeaponX_GetFiringPositionAndAimDirection", v_WeaponX_GetFiringPositionAndAimDirection);
		LogFunAdr("WeaponX_GrenadeVelocityFromDir", v_WeaponX_GrenadeVelocityFromDir);
		LogFunAdr("CalculateBounceVelocity", v_CalculateBounceVelocity);
	}
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const
	{
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 3B 5F").FollowNearCallSelf().GetPtr(v_TraceFilter_ShouldHitEntity);
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 4C 8B 5E ??").FollowNearCallSelf().GetPtr(v_UTIL_GetEntityScriptInfo);
		if (!v_TraceFilter_ShouldHitEntity)
			Warning(eDLL_T::SERVER, "[UTIL_Shared] TraceFilter_ShouldHitEntity pattern unresolved\n");
		if (!v_UTIL_GetEntityScriptInfo)
			Warning(eDLL_T::SERVER, "[UTIL_Shared] UTIL_GetEntityScriptInfo pattern unresolved\n");

		// SIMGRENADE: resolve via server-only caller anchors + FollowNearCall so full-function
		// patterns do not hit client twins first (those crash on dedi).
		// Anchor 1: server zipline-grenade path -- GetFiring then GrenadeVelocityFromDir.
		const CMemory simGrenadeLaunchSite = Module_FindPattern(g_GameDll,
			"4C 8D 4F ?? 48 89 37 4C 8D 47 ?? 48 8B D0 48 8B CE 48 8B E8 E8 ?? ?? ?? ?? "
			"45 33 C9 4C 8D 47 ?? 48 8D 54 24 ?? 48 8B CE E8 ?? ?? ?? ?? F3 0F 10 00");
		if (simGrenadeLaunchSite)
		{
			simGrenadeLaunchSite.Offset(0x14).FollowNearCall().GetPtr(v_WeaponX_GetFiringPositionAndAimDirection); // E8 at pattern+0x14
			simGrenadeLaunchSite.Offset(0x28).FollowNearCall().GetPtr(v_WeaponX_GrenadeVelocityFromDir);           // E8 at pattern+0x28
		}
		if (!v_WeaponX_GetFiringPositionAndAimDirection)
			Warning(eDLL_T::SERVER, "[SIMGRENADE] WeaponX_GetFiringPositionAndAimDirection pattern unresolved\n");
		if (!v_WeaponX_GrenadeVelocityFromDir)
			Warning(eDLL_T::SERVER, "[SIMGRENADE] WeaponX_GrenadeVelocityFromDir pattern unresolved\n");

		// Anchor 2: server grenade bounce handler call to CalculateBounceVelocity.
		Module_FindPattern(g_GameDll,
			"8D 86 ?? ?? ?? ?? F2 0F 10 B6 ?? ?? ?? ?? 4C 8D 8D ?? ?? ?? ?? 89 44 24 ?? "
			"48 8D 53 ?? 0F B6 86 ?? ?? ?? ?? 48 8D 4C 24 ?? 88 44 24 ?? E8 ?? ?? ?? ??")
			.Offset(0x2D).FollowNearCall().GetPtr(v_CalculateBounceVelocity); // E8 at pattern+0x2D
		if (!v_CalculateBounceVelocity)
			Warning(eDLL_T::SERVER, "[SIMGRENADE] CalculateBounceVelocity pattern unresolved\n");
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { }
};
///////////////////////////////////////////////////////////////////////////////

#endif // !UTIL_SHARED_H
#endif // CLIENT_DLL
