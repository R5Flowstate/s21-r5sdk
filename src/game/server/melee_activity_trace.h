//=============================================================================//
//
// Purpose: dedi half of the melee attack-lifetime trace. Twin of
// src\game\client\melee_activity_trace.cpp -- same events, same log shape.
//
//=============================================================================//
#ifndef MELEE_ACTIVITY_TRACE_SERVER_H
#define MELEE_ACTIVITY_TRACE_SERVER_H

#include "thirdparty/detours/include/idetour.h"

// CWeaponX::StartCustomActivity_Internal is already detoured by VTranslocation;
// that hook reports here after the engine stamped m_customActivityEndTime.
void MeleeActivityTrace_OnStart(void* weapon, unsigned int activity, unsigned char flags, char result, const void* pRet);

// CWeaponX::OnCustomActivityFinished -- clears the owner's melee attack state.
inline int64_t (*CWeaponX__OnCustomActivityFinished)(void* weapon) = nullptr;

// CPlayer::PlayerMelee_EndAttack -- the only writer that ends attackActive.
inline void (*CPlayer__PlayerMelee_EndAttack)(void* player) = nullptr;

// CPlayer::Lunge_ClearTarget -- the lunge exit.
inline void (*CPlayer__Lunge_ClearTarget)(void* player) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VMeleeActivityTraceServer : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CWeaponX::OnCustomActivityFinished", CWeaponX__OnCustomActivityFinished);
		LogFunAdr("CPlayer::PlayerMelee_EndAttack", CPlayer__PlayerMelee_EndAttack);
		LogFunAdr("CPlayer::Lunge_ClearTarget", CPlayer__Lunge_ClearTarget);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MELEE_ACTIVITY_TRACE_SERVER_H
