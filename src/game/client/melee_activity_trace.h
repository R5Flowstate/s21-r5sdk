//=============================================================================//
//
// Purpose: client half of the melee attack-lifetime trace. See
// melee_activity_trace.cpp for what it measures and why.
//
//=============================================================================//
#ifndef MELEE_ACTIVITY_TRACE_H
#define MELEE_ACTIVITY_TRACE_H

#include "thirdparty/detours/include/idetour.h"

// C_WeaponX::StartCustomActivity_Internal(activity, flags, forceDuration,
// optionalGesture, usePlayerTimeBase) -- stamps m_customActivityEndTime from
// the viewmodel sequence duration.
inline char (*C_WeaponX__StartCustomActivity_Internal)(void* weapon, uint16_t activity,
	uint16_t flags, float forceDuration, uint16_t gesture, bool usePlayerTimeBase) = nullptr;

// C_WeaponX::OnCustomActivityFinished -- clears the owner's melee attack state.
inline int64_t (*C_WeaponX__OnCustomActivityFinished)(void* weapon) = nullptr;

// C_Player::PlayerMelee_EndAttack / ClearActiveAttackState -- the two writers
// that end attackActive.
inline void (*C_Player__PlayerMelee_EndAttack)(void* player) = nullptr;
inline void (*C_Player__PlayerMelee_ClearActiveAttackState)(void* player) = nullptr;

// C_Player::Lunge_ClearTarget -- the lunge exit.
inline void (*C_Player__Lunge_ClearTarget)(void* player) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VMeleeActivityTrace : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_WeaponX::StartCustomActivity_Internal", C_WeaponX__StartCustomActivity_Internal);
		LogFunAdr("C_WeaponX::OnCustomActivityFinished", C_WeaponX__OnCustomActivityFinished);
		LogFunAdr("C_Player::PlayerMelee_EndAttack", C_Player__PlayerMelee_EndAttack);
		LogFunAdr("C_Player::PlayerMelee_ClearActiveAttackState", C_Player__PlayerMelee_ClearActiveAttackState);
		LogFunAdr("C_Player::Lunge_ClearTarget", C_Player__Lunge_ClearTarget);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MELEE_ACTIVITY_TRACE_H
