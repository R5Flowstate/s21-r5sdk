//=============================================================================//
//
// Purpose: S3 port of C_Player::PlayerLaunch / CGameMovement::ApplyPlayerLaunch.
// Sidecar + DT_LocalPlayerExclusive value proxies; S3 has no native cluster.
//
//=============================================================================//
#ifndef PLAYER_LAUNCH_H
#define PLAYER_LAUNCH_H

#include "thirdparty/detours/include/idetour.h"

struct PlayerLaunchWire
{
	int   m_activate;
	int   m_avoidedMantle;
	int   m_lock3pRotation;
	float m_velocity[3];
	float m_startTime;
};

bool PlayerLaunch_GetWire(const void* pPlayer, PlayerLaunchWire* pOut);
void PlayerLaunch_LevelShutdown(void);
void PlayerLaunch_NoteActivateSampled(const void* pPlayer);

void PlayerLaunch_Latch(void* pPlayer, float velX, float velY, float velZ, bool lock3pRotation);
void PlayerLaunch_ApplyFromMoveCtx(void* ctx);
bool PlayerLaunch_KeepsToss(const void* pPlayer);

void PlayerLaunch_BeginFullWalkMove(void* ctx);
void PlayerLaunch_EndFullWalkMove(void);

///////////////////////////////////////////////////////////////////////////////
class VPlayerLaunch : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // PLAYER_LAUNCH_H
