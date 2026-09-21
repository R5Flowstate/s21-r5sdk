//=============================================================================//
//
// Purpose: Damage-blocked-by-obstacle gate for CrossbowBolt touch (dedi side).
//
//=============================================================================//
#ifndef PLAYER_DAMAGE_OBSTACLE_H
#define PLAYER_DAMAGE_OBSTACLE_H
#ifdef _WIN32
#pragma once
#endif

#include "thirdparty/detours/include/idetour.h"

class CBaseEntity;
class CGameTrace;

class Vector3D;

typedef __int64 (__fastcall* PFN_CCrossbowBolt_BoltTouch)(__int64 bolt, CBaseEntity* pTouched, CGameTrace* pTrace);
inline PFN_CCrossbowBolt_BoltTouch v_CCrossbowBolt_BoltTouch = nullptr;

typedef __int64 (__fastcall* PFN_PlayerMelee_AttackTraces)(__int64 player, float* pPos, float* pDir,
	__int64 a4, void* pFilter, CGameTrace* pTrace);
inline PFN_PlayerMelee_AttackTraces v_PlayerMelee_AttackTraces = nullptr;

bool Obstacle_IsPlayer(const CBaseEntity* const pEntity);
bool Player_IsDamageBlockedByObstacle(CBaseEntity* const pPlayer, const Vector3D& hitPos);

///////////////////////////////////////////////////////////////////////////////
class VPlayerDamageObstacle : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CCrossbowBolt__BoltTouch", v_CCrossbowBolt_BoltTouch);
		LogFunAdr("PlayerMelee_AttackTraces", v_PlayerMelee_AttackTraces);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const;
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // PLAYER_DAMAGE_OBSTACLE_H
