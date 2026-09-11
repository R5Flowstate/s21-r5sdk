//=============================================================================//
//
// Purpose: dedi half of the melee lunge-boost probe. Twin of
// src\game\client\melee_lunge_probe.cpp -- same measurement, same log shape.
//
//=============================================================================//
#ifndef MELEE_LUNGE_PROBE_SERVER_H
#define MELEE_LUNGE_PROBE_SERVER_H

#include "thirdparty/detours/include/idetour.h"

// CGameMovement::MeleeWalkMove -- the sole writer of m_melee.lungeBoost.
// ctx: +8 CPlayer*, +16 CMoveData*.
inline void (*CGameMovement__MeleeWalkMove)(void* ctx) = nullptr;

// The two scalars behind the lunge speed cap: maxSpeed = 2 * range / duration.
inline float (*GetLungeDuration)(void* player) = nullptr;
inline float (*GetLungeNoTargetRange)(void* player) = nullptr;

// The ordinary walk-speed cap -- candidate for the plateau the engines settle at.
inline float (*CPlayer__GetPlayerMoveMaxSpeed)(void* player) = nullptr;

// The status-effect severity walker: (ent, typeIndex, &severity, &time,
// ignorePaused). No getTotal flag on this engine.
inline void (*GetStatusEffectSeverityAndTime)(void* ent, int type, float* sev,
	float* time, bool ignorePaused) = nullptr;

// Registry index of the speed-boost status-effect type.
inline int* g_pStatusEffectTypeIndex_SpeedBoost = nullptr;

// The two remaining move-scale factors.
inline float (*GetStatusEffectSeverity_MoveSlow)(void* player) = nullptr;
inline float (*CPlayer__GetRecentLandingMoveSpeedScale)(void* player) = nullptr;

// PlayerMelee_ItemPreFrame -- the melee press machine. Unlike the client's, this
// one has no launch-state gate on the press queue and no held-melee re-trigger.
inline void (*PlayerMelee_ItemPreFrame)(void* player) = nullptr;

// CPlayer::PlayerMelee_SetAttackHitEntity -- the only writer of
// m_melee.attackHitEntity / attackHitEntityTime / attackLastHitNonWorldEntity.
inline int (*CPlayer__PlayerMelee_SetAttackHitEntity)(void* player, void* ent) = nullptr;

// CPlayer::Lunge_SetTargetEntity -- writes the networked lunge start/end offsets.
inline char (*CPlayer__Lunge_SetTargetEntity)(void* player, void* target) = nullptr;

// Lag-comp ring walk that rewinds a player's origin history for the lunge setter.
// First arg is unused by the engine body; writes 3 floats into outVec3 and returns it.
inline float* (*LungeLagCompBacktrack)(void* unused, float* outVec3, void* player) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VMeleeLungeProbeServer : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CGameMovement::MeleeWalkMove", CGameMovement__MeleeWalkMove);
		LogFunAdr("GetLungeDuration", GetLungeDuration);
		LogFunAdr("GetLungeNoTargetRange", GetLungeNoTargetRange);
		LogFunAdr("CPlayer::GetPlayerMoveMaxSpeed", CPlayer__GetPlayerMoveMaxSpeed);
		LogFunAdr("GetStatusEffectSeverityAndTime", GetStatusEffectSeverityAndTime);
		LogFunAdr("GetStatusEffectSeverity_MoveSlow", GetStatusEffectSeverity_MoveSlow);
		LogFunAdr("CPlayer::GetRecentLandingMoveSpeedScale", CPlayer__GetRecentLandingMoveSpeedScale);
		LogVarAdr("StatusEffectTypeIndex_SpeedBoost", g_pStatusEffectTypeIndex_SpeedBoost);
		LogFunAdr("PlayerMelee_ItemPreFrame", PlayerMelee_ItemPreFrame);
		LogFunAdr("CPlayer::PlayerMelee_SetAttackHitEntity", CPlayer__PlayerMelee_SetAttackHitEntity);
		LogFunAdr("CPlayer::Lunge_SetTargetEntity", CPlayer__Lunge_SetTargetEntity);
		LogFunAdr("LungeLagCompBacktrack", LungeLagCompBacktrack);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MELEE_LUNGE_PROBE_SERVER_H
