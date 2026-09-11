//=============================================================================//
//
// Purpose: client half of the melee lunge-boost probe. See
// melee_lunge_probe.cpp for what it measures and why.
//
//=============================================================================//
#ifndef MELEE_LUNGE_PROBE_H
#define MELEE_LUNGE_PROBE_H

#include "thirdparty/detours/include/idetour.h"

// C_GameMovement::MeleeWalkMove -- the sole writer of m_melee.lungeBoost.
// ctx: +8 C_Player*, +16 C_MoveData*.
inline void (*C_GameMovement__MeleeWalkMove)(void* ctx) = nullptr;

// The two scalars behind the lunge speed cap: maxSpeed = 2 * range / duration.
inline float (*GetLungeDuration)(void* player) = nullptr;
inline float (*GetLungeNoTargetRange)(void* player) = nullptr;

// The ordinary walk-speed cap. The lunge chain is fully exonerated, so this is
// the candidate for the plateau the two engines settle at.
inline float (*C_Player__GetPlayerMoveMaxSpeed)(void* player) = nullptr;

// The status-effect severity walker: (ent, typeIndex, &severity, &time,
// ignorePaused, getTotal).
inline void (*GetStatusEffectSeverityAndTime)(void* ent, int type, float* sev,
	float* time, bool ignorePaused, bool getTotal) = nullptr;

// Registry index of the speed-boost status-effect type.
inline int* g_pStatusEffectTypeIndex_SpeedBoost = nullptr;

// The two remaining move-scale factors.
inline float (*GetStatusEffectSeverity_MoveSlow)(void* player) = nullptr;
inline float (*C_Player__GetRecentLandingMoveSpeedScale)(void* player) = nullptr;

// PlayerMelee_ItemPreFrame -- the melee press machine, and the sole writer of
// m_Local.m_meleePressTime.
inline void (*PlayerMelee_ItemPreFrame)(void* player) = nullptr;

// C_Player::PlayerMelee_SetAttackHitEntity -- the only writer of
// m_melee.attackHitEntity / attackHitEntityTime / attackLastHitNonWorldEntity.
// Reached from script only, off the melee-swing animation event.
inline int64_t (*C_Player__PlayerMelee_SetAttackHitEntity)(void* player, void* ent, void* unused) = nullptr;

// C_BaseAnimating::ProcessPredictedAnimEvents -- fires every stored predicted
// anim event whose stamp has come due and consumes it by zeroing that slot.
// It runs on the frame path, so a prediction rebase (which restores the whole
// predicted block from the ack) can hand back a slot this client already
// consumed, and the event fires a second time.
inline void (*C_BaseAnimating__ProcessPredictedAnimEvents)(void* animating) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VMeleeLungeProbe : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_GameMovement::MeleeWalkMove", C_GameMovement__MeleeWalkMove);
		LogFunAdr("GetLungeDuration", GetLungeDuration);
		LogFunAdr("GetLungeNoTargetRange", GetLungeNoTargetRange);
		LogFunAdr("C_Player::GetPlayerMoveMaxSpeed", C_Player__GetPlayerMoveMaxSpeed);
		LogFunAdr("GetStatusEffectSeverityAndTime", GetStatusEffectSeverityAndTime);
		LogFunAdr("GetStatusEffectSeverity_MoveSlow", GetStatusEffectSeverity_MoveSlow);
		LogFunAdr("C_Player::GetRecentLandingMoveSpeedScale", C_Player__GetRecentLandingMoveSpeedScale);
		LogVarAdr("StatusEffectTypeIndex_SpeedBoost", g_pStatusEffectTypeIndex_SpeedBoost);
		LogFunAdr("PlayerMelee_ItemPreFrame", PlayerMelee_ItemPreFrame);
		LogFunAdr("C_Player::PlayerMelee_SetAttackHitEntity", C_Player__PlayerMelee_SetAttackHitEntity);
		LogFunAdr("C_BaseAnimating::ProcessPredictedAnimEvents", C_BaseAnimating__ProcessPredictedAnimEvents);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MELEE_LUNGE_PROBE_H
