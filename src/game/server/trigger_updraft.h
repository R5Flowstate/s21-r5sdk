//=============================================================================//
//
// Purpose: Server-authoritative updraft state + movement. S3 dedi has none.
//
//=============================================================================//
#ifndef TRIGGER_UPDRAFT_BRIDGE_H
#define TRIGGER_UPDRAFT_BRIDGE_H

#include "thirdparty/detours/include/idetour.h"

// Per-player updraft state. SDKEntityMap-backed -- S3 has no members for any
// of this. The twelve wire fields come first (value-proxy offsetof order);
// touch bookkeeping after them never ships.
struct UpdraftState
{
	int   m_updraftCount;
	int   m_updraftStage;                       // UPDRAFT_STAGE_*
	float m_updraftEnterTime;
	float m_updraftLeaveTime;
	float m_updraftMinShakeActivationHeight;
	float m_updraftMaxShakeActivationHeight;
	float m_updraftLiftActivationHeight;
	float m_updraftLiftSpeed;
	float m_updraftLiftAcceleration;
	float m_updraftLiftExitDuration;
	float m_updraftSlowTime;
	int   m_skydiveFromUpdraft;

	// Touch bookkeeping -- MAX_TOUCHING_TRIGGERS is 6. Handles are the raw
	// EHANDLE dword (entity+8); INVALID is 0xFFFFFFFF.
	uint32_t m_touchingUpdraftTriggers[6];
	int      m_touchingUpdraftTriggersCount;

	// Probed once per dive through the engine's own class-mod accessor. The
	// fissure_updraft mod is what raises skydive_forceAnticipation, a field this
	// build has no slot for.
	int m_bFissureUpdraftMod;
};

static constexpr int UPDRAFT_MAX_TOUCHING_TRIGGERS = 6;

enum UpdraftStage_t
{
	UPDRAFT_STAGE_FALLING = 0,
	UPDRAFT_STAGE_LIFTING = 1,
	UPDRAFT_STAGE_SKYDIVE = 2
};

// Script-facing. Each returns false and warns on a rejected argument.
bool UpdraftBridge_EnterUpdraft(void* pPlayer, const float params[6]);
bool UpdraftBridge_LeaveUpdraft(void* pPlayer);
bool UpdraftBridge_IsInsideUpdraftTrigger(const void* pPlayer);
bool UpdraftBridge_IsSkydiveFromUpdraft(const void* pPlayer);
bool UpdraftBridge_IsSkydivingOutUpdraftTrigger(const void* pPlayer);

// Per-tick pass. Called from Hook_JumpPad_ApplyLaunchPass after
// TriggerGravity_ApplyPass.
void UpdraftBridge_ApplyPass(void* pCtx);

// Read by the dt_extend value proxies. Returns false when this player has no
// slot or the wire lever is off; the caller then emits zeros, which IS the
// correct idle state for all twelve props.
bool UpdraftBridge_GetWire(const void* pPlayer, UpdraftState* pOut);

bool UpdraftBridge_WireEnabled(void);
void UpdraftBridge_Wire_LevelShutdown(void);
// bridge_updraft_diag non-zero -- gates [UPDRAFT-WIRE] proxy hit spam.
bool UpdraftBridge_DiagEnabled(void);
// bridge_updraft_skydive_handoff -- end-of-ride freefall handoff lever.
bool UpdraftBridge_SkydiveHandoffEnabled(void);
// bridge_updraft_anticipate_latch -- hold landing pose for updraft dives.
bool UpdraftBridge_AnticipateLatchEnabled(void);

// Standing pose speed for the FALLING horizontal bleed -- reads the player's
// own settings block through the engine accessor. 0 when unresolved.
float UpdraftBridge_GetPoseSpeedNormal(const void* pPlayer);
// True when the player is phase-shifted AND has a move parent.
bool  UpdraftBridge_IsPhaseShiftedAndParented(const void* pPlayer);
// True while this player's updraft is actively lifting -- the one state in which
// the S21 client applies no gravity at all.
bool UpdraftBridge_IsLifting(const void* pPlayer);

// Called from the EndFreefall hook -- stage/slow-time reset on dive end.
void UpdraftBridge_OnFreefallEnded(void* pPlayer);
// Slow-time helpers for the updraft end arms (realised climb stall detection).
void UpdraftBridge_SetSlowTime(void* pPlayer, float flTime);
void UpdraftBridge_UpdateSlowTime(void* pPlayer, float flActualUpSpeed, float flNow);
bool UpdraftBridge_IsStalled(const void* pPlayer, float flNow);

// Lift speed/acceleration for the running ride. False when this player has no
// slot -- the caller must then leave the velocity alone rather than lift by 0.
bool UpdraftBridge_GetLiftParams(const void* pPlayer, float* pflSpeed, float* pflAccel);

// A dive started while the player is still inside the column. Latches the
// from-updraft flag the S21 client's own skydive entry sets, and probes the mod once.
void UpdraftBridge_OnSkydiveStartedFromUpdraft(void* pPlayer);

// True while the player holds the updraft class mod. Probed once per dive --
// the engine accessor warns on an undefined mod name, so it is never per-command.
bool UpdraftBridge_HasForceAnticipationMod(const void* pPlayer);

// Alias trigger_updraft onto the CTriggerMultiple factory. Call once per
// level before map entities are parsed (CreateNetworkStringTables boundary).
void UpdraftBridge_InstallEntityFactory(void);

// Touch path from VTriggerStartTouchDedupe -- StartTouch after the original,
// EndTouch after the original. Classname-gates on "trigger_updraft".
void UpdraftBridge_OnTriggerStartTouch(void* pTrigger, void* pOther);
void UpdraftBridge_OnTriggerEndTouch(void* pTrigger, void* pOther);

struct ScriptClassDescriptor_t;
void UpdraftBridge_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct);

///////////////////////////////////////////////////////////////////////////////
class VTriggerUpdraftBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	// Touch path still rides VTriggerStartTouchDedupe; the movement pass still
	// rides VJumpPadParity. This class owns the half-gravity gate.
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TRIGGER_UPDRAFT_BRIDGE_H
