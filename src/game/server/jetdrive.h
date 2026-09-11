//=============================================================================//
//
// Purpose: JetDrive player-movement (Vantage tactical). Dedi half plus
// the 14 DT_LocalPlayerExclusive value proxies.
//
//=============================================================================//
#ifndef JETDRIVE_H
#define JETDRIVE_H

#include "thirdparty/detours/include/idetour.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/predictioncopy.h"
#include "mathlib/vector.h"

class CPlayer;
class CBaseEntity;

//-----------------------------------------------------------------------------
// Per-player JetDrive state. SDKEntityMap-backed. m_jetDriveTargetEntOffset is a Vector on the wire.
//-----------------------------------------------------------------------------
struct JetDriveState
{
	bool     m_jetDriveWasActive = false;
	bool     m_jetDriveActive = false;
	Vector3D m_jetDriveTargetPos = vec3_origin;
	EHANDLE  m_jetDriveTargetEnt;
	Vector3D m_jetDriveTargetEntOffset = vec3_origin;
	Vector3D m_jetDriveStartPos = vec3_origin;
	float    m_jetDriveStartTime = 0.0f;
	float    m_jetDriveSpeed = 0.0f;
	float    m_jetDriveAccel = 0.0f;
	float    m_jetDriveDecelWindowTimeOutTime = -1.0f;
	bool     m_jetDriveInDecelWindow = false;
	float    m_jetDriveTimeout = 0.0f;
	Vector3D m_jetDriveDoubleJumpVelocity = vec3_invalid;
	float    m_jetDriveDoubleJumpVelBackFrac = 0.0f;

	float    m_jetDriveAnimTime = -1.0f;
	char     m_jetDriveDoubleJumpSound1p[64] = {};
	char     m_jetDriveDoubleJumpSound3p[64] = {};
};

//-----------------------------------------------------------------------------
// Wire-side flattening. Snapshot pack is off-thread; offset 20000 has no entity-memory slot.
//-----------------------------------------------------------------------------
struct JetDriveWire
{
	int   m_wasActive;
	int   m_active;
	int   m_targetEnt;                // S21 RecvPropEHandle: (ser<<14)|idx
	int   m_inDecelWindow;
	float m_speed;
	float m_accel;
	float m_timeout;
	float m_doubleJumpVelBackFrac;
	float m_startTime;
	float m_decelWindowTimeOutTime;
	float m_targetPos[3];
	float m_targetEntOffset[3];
	float m_startPos[3];
	float m_doubleJumpVelocity[3];
};

// Sidecar read. When the wire is on and this player has no slot, fills ctor
// idle (not all-zeros: DJVel=invalid, decel timeout=-1, handle=0x00FFFFFF).
bool JetDrive_GetWire(const void* pPlayer, JetDriveWire* pOut);

// Boot-time SendTable_Init read. Off = props not appended.
bool JetDrive_WireEnabled(void);

void JetDrive_Wire_LevelShutdown(void);

void JetDrive_Begin(CPlayer* player, float speed, float accel,
	const Vector3D& targetPos, CBaseEntity* targetEnt, const Vector3D& targetEntOffset, float timeOut);

// Per-tick mover. ctx is CGameMovement (player +8, mv +16). Writes S3
// CMoveData::m_vecVelocity at +304. Called from FullWalkMove after orig.
void JetDrive_AccelFromMoveCtx(void* ctx);
void JetDrive_Accel(CPlayer* player, void* mv, float dt);

void JetDrive_End(CPlayer* player);

void JetDrive_EnableDoubleJump(CPlayer* player, const Vector3D& launchVelocity,
	float jumpBackVelFrac, const char* sound1p, const char* sound3p);

bool JetDrive_IsActive(CPlayer* player);
bool JetDrive_IsInDecelWindow(CPlayer* player);

// S21 keeps companion_launch up for the whole drive.
bool JetDrive_ShouldHoldOffhand(void* pWeapon);
bool JetDrive_NoteOffhandHolster(void* pWeapon);
void JetDrive_TickHolds(void* pPlayer);

// Whistle stays the 1p host for the whole drive. Block guns/melee.
bool JetDrive_ShouldBlockSetActiveWeapon(void* pPlayer, void* pWeapon);

// True: caller must not apply newState (keep current). Bind ptpov as a side effect.
bool JetDrive_FilterWeaponState(void* pWeapon, unsigned int newState);
// True: caller must not apply this ideal activity (idle/sprint/holster while locked).
bool JetDrive_FilterIdealActivity(void* pWeapon, unsigned int activity);

struct ScriptClassDescriptor_t;
void JetDrive_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

///////////////////////////////////////////////////////////////////////////////
class VJetDrive : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const;
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // JETDRIVE_H
