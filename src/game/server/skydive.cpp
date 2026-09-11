//=============================================================================//
//
// Purpose: server-side skydive simulation, one step per executed usercmd.
// Velocity is built from the previous step's strafe/yaw against this step's
// speed and dive angle.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "skydive.h"
#include "trigger_updraft.h"
#include "baseentity.h"
#include "game/shared/usercmd.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "public/edict.h" // CGlobalVars (curTime)
#include "mathlib/vector.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cmath>

// gpGlobals is defined in the game module; declare locally -- same pattern
// jetdrive.cpp / zipline_cooldown.cpp use.
extern CGlobalVars* gpGlobals;

//-----------------------------------------------------------------------------
// Server CPlayer layout. Client twin in this binary is 0x3AC8 lower.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t SD_OFF_ABSVELOCITY          = 0x3DC;
static constexpr ptrdiff_t SD_OFF_MOVETYPE             = 0x308; // char; 4 and 5 dispatch FullTossMove
static constexpr ptrdiff_t SD_OFF_FWDPOSEVALUETARGET   = 0x7B48;
static constexpr ptrdiff_t SD_OFF_FWDPOSEVALUECURRENT  = 0x7B4C;
static constexpr ptrdiff_t SD_OFF_SIDEPOSEVALUETARGET  = 0x7B54;
static constexpr ptrdiff_t SD_OFF_FREEFALLSTATE        = 0x7B60; // 0 none, 1 diving, 2 anticipating
static constexpr ptrdiff_t SD_OFF_FREEFALLSTARTTIME    = 0x7B64;
static constexpr ptrdiff_t SD_OFF_DIVEANGLE            = 0x7B78;
static constexpr ptrdiff_t SD_OFF_SPEED                = 0x7B80;
static constexpr ptrdiff_t SD_OFF_STRAFEANGLE          = 0x7B84;
static constexpr ptrdiff_t SD_OFF_FREELOOKENABLED      = 0x7B88;
static constexpr ptrdiff_t SD_OFF_FREELOOKLOCKEDANGLE  = 0x7B8C;
static constexpr ptrdiff_t SD_OFF_PLAYERPITCH          = 0x7B98; // the lerped body pitch
static constexpr ptrdiff_t SD_OFF_PLAYERYAW            = 0x7B9C;
static constexpr ptrdiff_t SD_OFF_FOLLOWING            = 0x7BA0; // bool
// m_skydiveIsNearLeviathan -- renames to m_skydiveIsNearDisableSkydiveEndEntity
static constexpr ptrdiff_t SD_OFF_ISNEARDISABLEEND     = 0x7BB0;

// Skydive_MovementIn / Skydive_MovementOut. Only these three matter: while an
// updraft owns the ride the step's own vertical result is discarded.
static constexpr ptrdiff_t SD_MOVEIN_OFF_DT        = 0x10;
static constexpr ptrdiff_t SD_MOVEIN_OFF_VELOCITY  = 0x20;
static constexpr ptrdiff_t SD_MOVEOUT_OFF_VELOCITY = 0x44;

// The forward pose spring travels toward +90; the body pitch is the dive angle
// scaled by how far along that travel it is. Same lerp the client applies.
static constexpr float SD_FWDPOSE_RANGE = 90.0f;

// A single command may not advance the springs by more than this. The engine
// bounds usercmd frametime upstream; this is the backstop that keeps a
// pathological dt from throwing the integrator somewhere it cannot recover from.
static constexpr float SD_MAX_STEP = 0.5f;

// How long after the engine last authored a player's skydive this module keeps
// standing down. Generous on purpose: curTime only advances per server tick, so
// several commands can share one stamp, and a dive lasts many seconds.
static constexpr float SD_ENGINE_DRIVE_GRACE = 0.5f;

//-----------------------------------------------------------------------------
// Validating wrappers the script natives call (player + floats; they read settings themselves).
//-----------------------------------------------------------------------------
static float (*v_Skydive_CalculatePitch)(void* player, const Vector3D* velocity, float viewPitch, float dt) = nullptr;
static float (*v_Skydive_CalculateYaw)(void* player, float speed, float playerYaw, float viewYaw, float dt) = nullptr;
static float (*v_Skydive_CalculateSpeed)(void* player, float speed, float playerPitch, float inputForward, float dt) = nullptr;
static float (*v_Skydive_CalculateStrafeAngle)(void* player, float inputRight, float strafeAngle, float dt) = nullptr;
static void  (*v_Skydive_UpdatePoseParameterTargets)(void* player, float inputForward,
	float inputRight, float viewYaw, float playerYaw) = nullptr;

// One arg, no dt: steps pose members at gpGlobals->frameTime.
static void  (*v_Skydive_UpdatePoseParameters)(void* player) = nullptr;

// Returns its out-pointer; the Vector is returned through rcx, which is why the
// player lands in the second slot. No dt -- speed scales the direction and the
// caller has already stepped the springs.
static Vector3D* (*v_Skydive_CalculateVelocity)(Vector3D* out, void* player, float diveAngle,
	float playerYaw, float viewPitch, float viewYaw, float speed,
	float strafeAngle, float inputForward) = nullptr;

// Freefall path skips animstate/eye resync. Also stamps -9999 into every anim layer cycle (stride 448).
static void (*v_CPlayer__SetFeetAngles)(void* player, const Vector3D* angles) = nullptr;

static void (*v_CBaseEntity__SetAbsVelocity)(void* entity, const Vector3D* velocity) = nullptr;

// CGameMovement::FullTossMove, server half (`this+8` = player). Dispatched for movetype 4/5.
static int64_t (*v_CGameMovement__FullTossMove)(int64_t movement) = nullptr;

// Engine freefall entry/exit -- the S3 names for what the client calls skydive.
// Validating wrappers the script natives call; each assert-guards its preconditions.
static void (*v_CPlayer__BeginFreefall)(void* pPlayer, const Vector3D* pInitialVelocity) = nullptr;
static void (*v_CPlayer__EndFreefall)(void* pPlayer) = nullptr;
static void (*v_CPlayer__BeginFreefallAnticipate)(void* pPlayer) = nullptr;
static void (*v_CPlayer__EndFreefallAnticipate)(void* pPlayer) = nullptr;

// The skydive movement step -- only the two blocks; FullTossMove is the sole
// server caller. Signature: void (*)(const void* pIn, void* pOut).
static void (*v_Skydive_Movement)(const void* pIn, void* pOut) = nullptr;

// CPlayer::IsClassModAvailableForPlayerSetting(const char*) -- holds the named
// class mod. Server half only; call at most once per dive (undefined-mod warn).
static bool (*v_CPlayer__IsClassModAvailableForPlayerSetting)(void* pPlayer, const char* pszModName) = nullptr;

// The movement step takes only its two blocks. FullTossMove is its single
// caller, so the player travels through here rather than being re-derived.
// Saved and restored so a nested move cannot leak the wrong player.
static CPlayer* s_pMoveStepPlayer = nullptr;

static ConVar bridge_skydive_server_sim("bridge_skydive_server_sim", "1", FCVAR_RELEASE,
	"Simulate the skydive on the server once per executed usercmd, so the client's "
	"predicted skydive reconciles against values derived from the same command stream.");

static ConVar bridge_skydive_engine_standdown("bridge_skydive_engine_standdown", "1", FCVAR_RELEASE,
	"Skip this module's simulation for any player the engine's own skydive movement is "
	"already authoring. 0 = simulate anyway, which puts two authors on the same springs.");

static ConVar bridge_skydive_diag("bridge_skydive_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[SKYDIVE-DIAG] rate-limited dump of one skydiving player's simulation inputs and "
	"outputs. 0 = silent.");

static ConVar bridge_skydive_updraft_lift("bridge_skydive_updraft_lift", "1", FCVAR_RELEASE,
	"Carry the updraft lift through the skydive movement step, as the S21 client does. "
	"0 leaves the S3 step authoring the vertical axis, which stops pushing the player "
	"the moment the dive starts and drops them out of the ride.");

//-----------------------------------------------------------------------------
// Per-player bookkeeping. The engine holds every simulated value itself; this
// only tracks cadence.
//-----------------------------------------------------------------------------
struct SkydiveBridgeState_t
{
	// curTime of the last animation-layer invalidation, so it happens once per
	// server tick rather than once per command.
	float m_flLastFeetAnglesTime = -1.0f;
	float m_flLastDiagTime = -1.0f;
	// Separate from m_flLastDiagTime -- Think and the movement lift hook both
	// run in the same tick and must not silence each other.
	float m_flLastLiftDiagTime = -1.0f;

	// curTime at which the engine's own FullTossMove last simulated this player's
	// skydive. -1 means it never has.
	float m_flLastEngineDriveTime = -1.0f;
};

static SDKEntityMap<SkydiveBridgeState_t> s_skydiveStateMap(ESide::Server, "skydive.srv");

static uint32_t s_nEngineDriveTicks = 0;

static bool SkydiveBridge_Resolved(void)
{
	return v_Skydive_CalculatePitch && v_Skydive_CalculateYaw && v_Skydive_CalculateSpeed
		&& v_Skydive_CalculateStrafeAngle && v_Skydive_UpdatePoseParameterTargets
		&& v_Skydive_UpdatePoseParameters && v_Skydive_CalculateVelocity
		&& v_CPlayer__SetFeetAngles && v_CBaseEntity__SetAbsVelocity;
}

static inline float SkydiveBridge_Clamp(const float flValue, const float flMin, const float flMax)
{
	return fminf(fmaxf(flValue, flMin), flMax);
}

static void Hook_CPlayer__EndFreefall(void* pPlayer);
static void Hook_CPlayer__BeginFreefall(void* pPlayer, const Vector3D* pInitialVelocity);

//-----------------------------------------------------------------------------
// Purpose: end an in-flight freefall via the engine path (hooked EndFreefall)
//-----------------------------------------------------------------------------
static void SkydiveBridge_EndFreefall(void* pPlayer)
{
	if (!pPlayer || !v_CPlayer__EndFreefall)
		return;

	const int nState = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(pPlayer) + SD_OFF_FREEFALLSTATE);
	if (nState == PLAYER_FREEFALL_STATE_NONE)
		return;

	// Call the hook, not the trampoline -- DetourSetup rewrites v_ to the original.
	Hook_CPlayer__EndFreefall(pPlayer);
}

//-----------------------------------------------------------------------------
// Updraft-only freefall end. Stall arm covers a ceiling pin (trace arm is inside the engine move).
//-----------------------------------------------------------------------------
static void SkydiveBridge_UpdraftEndArms(CPlayer* player, float flOriginZBefore)
{
	if (!player || !UpdraftBridge_SkydiveHandoffEnabled())
		return;
	if (!UpdraftBridge_IsSkydiveFromUpdraft(player))
		return;

	const uintptr_t base = reinterpret_cast<uintptr_t>(player);
	const float flNow = gpGlobals ? gpGlobals->curTime : 0.0f;
	const float flStart = *reinterpret_cast<const float*>(base + SD_OFF_FREEFALLSTARTTIME);
	const float flElapsed = flNow - flStart;

	// Every arm is blocked for the first two seconds.
	if (flElapsed < 2.0f)
	{
		UpdraftBridge_SetSlowTime(player, 0.0f);
		return;
	}

	// A dive near an entity that disables skydive-end is never ended.
	if (*reinterpret_cast<const uint8_t*>(base + SD_OFF_ISNEARDISABLEEND))
		return;

	// The realised vertical speed after collision, not the requested one -- this
	// is what detects the player being pinned against geometry.
	const float flFrame = gpGlobals ? gpGlobals->frameTime : 0.0f;
	if (flFrame > 0.0f)
	{
		const float flOriginZ = reinterpret_cast<CBaseEntity*>(player)->Diag_AbsOrigin().z;
		const float flActualUpSpeed = (flOriginZ - flOriginZBefore) / flFrame;
		UpdraftBridge_UpdateSlowTime(player, flActualUpSpeed, flNow);
	}

	if (UpdraftBridge_IsPhaseShiftedAndParented(player)
		|| UpdraftBridge_IsStalled(player, flNow)
		|| (!UpdraftBridge_IsInsideUpdraftTrigger(player)
			&& !UpdraftBridge_IsSkydivingOutUpdraftTrigger(player)))
	{
		SkydiveBridge_EndFreefall(player);
	}
}

//-----------------------------------------------------------------------------
// Purpose: !fromUpdraft anticipation latch -- suppress the clear
//-----------------------------------------------------------------------------
static void Hook_CPlayer__EndFreefallAnticipate(void* pPlayer)
{
	// Updraft dives skip the anticipation clear; this engine has no such gate.
	if (pPlayer && UpdraftBridge_AnticipateLatchEnabled()
		&& SkydiveBridge_ForceAnticipation(pPlayer))
		return;

	v_CPlayer__EndFreefallAnticipate(pPlayer);
}

//-----------------------------------------------------------------------------
// Seed an updraft dive at zero speed (native would seed a 70% forward dive from the view).
//-----------------------------------------------------------------------------
static void Hook_CPlayer__BeginFreefall(void* pPlayer, const Vector3D* pInitialVelocity)
{
	const bool bFromUpdraft = pPlayer && UpdraftBridge_IsInsideUpdraftTrigger(pPlayer);
	Vector3D vecBefore(0.0f, 0.0f, 0.0f);
	if (bFromUpdraft)
		vecBefore = reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_AbsVelocity();

	v_CPlayer__BeginFreefall(pPlayer, pInitialVelocity);

	if (!bFromUpdraft)
		return;

	*reinterpret_cast<float*>(
		reinterpret_cast<uintptr_t>(pPlayer) + SD_OFF_SPEED) = 0.0f;

	if (v_CBaseEntity__SetAbsVelocity)
		v_CBaseEntity__SetAbsVelocity(pPlayer, &vecBefore);

	UpdraftBridge_OnSkydiveStartedFromUpdraft(pPlayer);
}

//-----------------------------------------------------------------------------
// Updraft lift: re-seed vertical from incoming velocity, then clamp.
//-----------------------------------------------------------------------------
static void Hook_Skydive_Movement(const void* pIn, void* pOut)
{
	v_Skydive_Movement(pIn, pOut);

	if (!pIn || !pOut || !bridge_skydive_updraft_lift.GetBool())
		return;

	CPlayer* const player = s_pMoveStepPlayer;
	if (!player || !UpdraftBridge_IsSkydiveFromUpdraft(player))
		return;

	float flLiftSpeed = 0.0f;
	float flLiftAccel = 0.0f;
	if (!UpdraftBridge_GetLiftParams(player, &flLiftSpeed, &flLiftAccel))
		return;

	const float dt = *reinterpret_cast<const float*>(
		static_cast<const uint8_t*>(pIn) + SD_MOVEIN_OFF_DT);
	const float flOldZ = *reinterpret_cast<const float*>(
		static_cast<const uint8_t*>(pIn) + SD_MOVEIN_OFF_VELOCITY + 8);

	// A non-finite dt or seed velocity would strand the player's z at NaN for
	// the rest of the dive; no later command recovers from that.
	if (!std::isfinite(dt) || dt < 0.0f || dt > SD_MAX_STEP || !std::isfinite(flOldZ))
		return;

	float flNewZ = flOldZ;
	if (flLiftSpeed > flOldZ)
		flNewZ = fminf(flLiftSpeed, flOldZ + dt * flLiftAccel);

	float* const pOutVel = reinterpret_cast<float*>(
		static_cast<uint8_t*>(pOut) + SD_MOVEOUT_OFF_VELOCITY);
	pOutVel[2] = flNewZ;

	if (bridge_skydive_diag.GetBool())
	{
		SkydiveBridgeState_t& state = s_skydiveStateMap[player];
		const float flNow = gpGlobals ? gpGlobals->curTime : 0.0f;
		if ((flNow - state.m_flLastLiftDiagTime) > 0.5f)
		{
			state.m_flLastLiftDiagTime = flNow;
			Msg(eDLL_T::SERVER,
				"[SKYDIVE-LIFT] dt=%.4f oldZ=%.1f newZ=%.1f liftSpeed=%.1f liftAccel=%.1f\n",
				dt, flOldZ, flNewZ, flLiftSpeed, flLiftAccel);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: updraft stage reset on end-skydive that this engine cannot perform
//-----------------------------------------------------------------------------
static void Hook_CPlayer__EndFreefall(void* pPlayer)
{
	// Clear m_skydiveFromUpdraft before the original so nested EndFreefallAnticipate is unsuppressed.
	if (pPlayer)
		UpdraftBridge_OnFreefallEnded(pPlayer);

	v_CPlayer__EndFreefall(pPlayer);
}

//-----------------------------------------------------------------------------
// Purpose: records that the engine authored this player's skydive this tick,
// and supplies the updraft end arms after the original has declined to end.
//-----------------------------------------------------------------------------
static int64_t Hook_CGameMovement__FullTossMove(int64_t movement)
{
	CPlayer* player = nullptr;
	float flOriginZBefore = 0.0f;

	if (movement)
	{
		player = *reinterpret_cast<CPlayer**>(movement + 8);

		if (player)
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(player);
			const int nState = *reinterpret_cast<const int*>(base + SD_OFF_FREEFALLSTATE);

			// The non-freefall half of this function is an unrelated glide path.
			if (nState != 0)
			{
				++s_nEngineDriveTicks;
				s_skydiveStateMap[player].m_flLastEngineDriveTime =
					gpGlobals ? gpGlobals->curTime : 0.0f;

				static bool s_bAnnounced = false;
				if (!s_bAnnounced)
				{
					s_bAnnounced = true;
					Msg(eDLL_T::SERVER,
						"[SKYDIVE-ENGINE] the authority's own skydive movement is running "
						"(state=%d movetype=%d dt=%.5f) -- it authors every replicated "
						"skydive field, so this module's simulation stands down\n",
						nState, *reinterpret_cast<const int8_t*>(base + SD_OFF_MOVETYPE),
						gpGlobals ? gpGlobals->frameTime : -1.0f);
				}

				// Origin z before the move -- end arms need realised climb rate.
				if (UpdraftBridge_IsSkydiveFromUpdraft(player))
					flOriginZBefore = reinterpret_cast<CBaseEntity*>(player)->Diag_AbsOrigin().z;
			}
		}
	}

	CPlayer* const pPrevStepPlayer = s_pMoveStepPlayer;
	s_pMoveStepPlayer = player;
	const int64_t result = v_CGameMovement__FullTossMove(movement);
	s_pMoveStepPlayer = pPrevStepPlayer;

	if (player)
		SkydiveBridge_UpdraftEndArms(player, flOriginZBefore);

	return result;
}

//-----------------------------------------------------------------------------
// Purpose: raise the engine's freefall with the given seed velocity
//-----------------------------------------------------------------------------
bool SkydiveBridge_BeginFreefall(void* pPlayer, const Vector3D& vecInitialVelocity)
{
	if (!v_CPlayer__BeginFreefall)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYDIVE] Player_BeginFreefall unresolved -- updraft skydive handoff "
				"cannot start a dive\n");
		}
		return false;
	}

	if (!pPlayer)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[SKYDIVE] BeginFreefall rejected null player\n");
		}
		return false;
	}

	if (!std::isfinite(vecInitialVelocity.x) || !std::isfinite(vecInitialVelocity.y)
		|| !std::isfinite(vecInitialVelocity.z))
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYDIVE] BeginFreefall rejected non-finite velocity -- would NaN the "
				"dive orientation permanently\n");
		}
		return false;
	}

	const int nState = *reinterpret_cast<const int*>(
		static_cast<const uint8_t*>(pPlayer) + SD_OFF_FREEFALLSTATE);
	if (nState != PLAYER_FREEFALL_STATE_NONE)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYDIVE] BeginFreefall refused -- player already freefalling (state=%d)\n",
				nState);
		}
		return false;
	}

	// Call the hook, not the trampoline -- DetourSetup rewrites v_ to the original.
	Hook_CPlayer__BeginFreefall(pPlayer, &vecInitialVelocity);
	return true;
}

bool SkydiveBridge_ForceAnticipation(const void* pPlayer)
{
	if (!pPlayer)
		return false;

	return UpdraftBridge_IsSkydiveFromUpdraft(pPlayer)
		|| UpdraftBridge_HasForceAnticipationMod(pPlayer);
}

bool SkydiveBridge_IsClassModActive(void* pPlayer, const char* pszModName)
{
	if (!pPlayer || !pszModName || !pszModName[0])
		return false;

	if (!v_CPlayer__IsClassModAvailableForPlayerSetting)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYDIVE] IsClassModAvailableForPlayerSetting unresolved -- "
				"fissure_updraft probe is inert\n");
		}
		return false;
	}

	return v_CPlayer__IsClassModAvailableForPlayerSetting(pPlayer, pszModName);
}

//-----------------------------------------------------------------------------
// Purpose: force the landing-anticipation sub-state on a live dive
//-----------------------------------------------------------------------------
bool SkydiveBridge_BeginFreefallAnticipate(void* pPlayer)
{
	if (!v_CPlayer__BeginFreefallAnticipate)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYDIVE] Player_BeginFreefallAnticipate unresolved -- updraft handoff "
				"cannot force the landing pose\n");
		}
		return false;
	}

	if (!pPlayer)
		return false;

	const int nState = *reinterpret_cast<const int*>(
		static_cast<const uint8_t*>(pPlayer) + SD_OFF_FREEFALLSTATE);
	if (nState == PLAYER_FREEFALL_STATE_NONE
		|| nState == PLAYER_FREEFALL_STATE_ANTICIPATING)
		return false;

	v_CPlayer__BeginFreefallAnticipate(pPlayer);
	return true;
}

bool SkydiveBridge_IsFreefalling(const void* pPlayer)
{
	if (!pPlayer)
		return false;

	const int nState = *reinterpret_cast<const int*>(
		static_cast<const uint8_t*>(pPlayer) + SD_OFF_FREEFALLSTATE);
	return nState != PLAYER_FREEFALL_STATE_NONE;
}

int SkydiveBridge_GetFreefallState(const void* pPlayer)
{
	if (!pPlayer)
		return PLAYER_FREEFALL_STATE_NONE;

	return *reinterpret_cast<const int*>(
		static_cast<const uint8_t*>(pPlayer) + SD_OFF_FREEFALLSTATE);
}

//-----------------------------------------------------------------------------
// Purpose: one skydive step for one executed command
//-----------------------------------------------------------------------------
void SkydiveBridge_Think(CPlayer* player, CUserCmd* ucmd, float flFrameTime)
{
	if (!player || !ucmd || !bridge_skydive_server_sim.GetBool() || !SkydiveBridge_Resolved())
		return;

	const uintptr_t base = reinterpret_cast<uintptr_t>(player);

	const int nState = *reinterpret_cast<const int*>(base + SD_OFF_FREEFALLSTATE);
	if (nState == 0)
		return;

	// Follow mode is script-owned: the follower's angles come from the leader.
	if (*reinterpret_cast<const uint8_t*>(base + SD_OFF_FOLLOWING))
		return;

	const float dt = flFrameTime;
	if (!(dt > 0.0f) || dt > SD_MAX_STEP)
		return;

	SkydiveBridgeState_t& state = s_skydiveStateMap[player];
	const float flNow = gpGlobals ? gpGlobals->curTime : 0.0f;

	// Engine already simulated this tick: do not author a second set of springs.
	if (bridge_skydive_engine_standdown.GetBool() && state.m_flLastEngineDriveTime >= 0.0f
		&& (flNow - state.m_flLastEngineDriveTime) <= SD_ENGINE_DRIVE_GRACE)
		return;

	// Steering follows the aim in the command being executed. Freelook parks it
	// on the locked angle instead, exactly as the client's copy does.
	float flViewPitch;
	float flViewYaw;

	if (*reinterpret_cast<const int*>(base + SD_OFF_FREELOOKENABLED))
	{
		const float* const pLocked = reinterpret_cast<const float*>(base + SD_OFF_FREELOOKLOCKEDANGLE);
		flViewPitch = pLocked[0];
		flViewYaw = pLocked[1];
	}
	else
	{
		flViewPitch = ucmd->viewangles.x;
		flViewYaw = ucmd->viewangles.y;
	}

	// The command is attacker-controlled. A non-finite angle propagates straight
	// into the springs and leaves the player's orientation permanently NaN, which
	// no later command can recover from.
	if (!std::isfinite(flViewPitch) || !std::isfinite(flViewYaw))
		return;

	// Replicated m_forwardMove / m_sideMove are written during movement; before
	// it they still hold the previous command. Use this command's axes instead.
	const float flInputForward = SkydiveBridge_Clamp(ucmd->forwardmove, -1.0f, 1.0f);
	const float flInputRight = SkydiveBridge_Clamp(ucmd->sidemove, -1.0f, 1.0f);

	const float flSpeed = *reinterpret_cast<const float*>(base + SD_OFF_SPEED);
	const float flYaw = *reinterpret_cast<const float*>(base + SD_OFF_PLAYERYAW);
	const float flStrafe = *reinterpret_cast<const float*>(base + SD_OFF_STRAFEANGLE);
	const float flBodyPitchPrev = *reinterpret_cast<const float*>(base + SD_OFF_PLAYERPITCH);

	// Pitch derives both the current dive angle and the minimum glide angle from
	// this vector. The step runs before movement, so the entity's velocity is the
	// pre-movement value the client's copy is fed.
	const Vector3D* const pVelocity =
		reinterpret_cast<const Vector3D*>(base + SD_OFF_ABSVELOCITY);

	const float flSpeedIn = sqrtf(pVelocity->x * pVelocity->x +
		pVelocity->y * pVelocity->y + pVelocity->z * pVelocity->z);

	const float flNewStrafe = v_Skydive_CalculateStrafeAngle(player, flInputRight, flStrafe, dt);
	const float flNewSpeed = v_Skydive_CalculateSpeed(player, flSpeed, flBodyPitchPrev, flInputForward, dt);
	const float flNewPitch = v_Skydive_CalculatePitch(player, pVelocity, flViewPitch, dt);

	// Direction is dive angle + player yaw as a pitch/yaw pair, rotated about Z
	// by the strafe angle and scaled by the speed.
	Vector3D vecNewVelocity(0.0f, 0.0f, 0.0f);
	v_Skydive_CalculateVelocity(&vecNewVelocity, player, flNewPitch, flYaw,
		flViewPitch, flViewYaw, flNewSpeed, flNewStrafe, flInputForward);

	const float flNewYaw = v_Skydive_CalculateYaw(player, flNewSpeed, flYaw, flViewYaw, dt);

	// Side-pose: wrapped yaw difference, -30..30 mapped onto -90..90. Engine order: produced yaw, then view yaw.
	v_Skydive_UpdatePoseParameterTargets(player, flInputForward, flInputRight, flNewYaw, flViewYaw);
	v_Skydive_UpdatePoseParameters(player);

	const float flForwardPose = *reinterpret_cast<const float*>(base + SD_OFF_FWDPOSEVALUECURRENT);
	const float flBodyPitch = flNewPitch *
		SkydiveBridge_Clamp(flForwardPose / SD_FWDPOSE_RANGE, 0.0f, 1.0f);

	*reinterpret_cast<float*>(base + SD_OFF_DIVEANGLE) = flNewPitch;
	*reinterpret_cast<float*>(base + SD_OFF_SPEED) = flNewSpeed;
	*reinterpret_cast<float*>(base + SD_OFF_STRAFEANGLE) = flNewStrafe;
	*reinterpret_cast<float*>(base + SD_OFF_PLAYERPITCH) = flBodyPitch;
	*reinterpret_cast<float*>(base + SD_OFF_PLAYERYAW) = flNewYaw;

	v_CBaseEntity__SetAbsVelocity(player, &vecNewVelocity);

	// Orientation is authored once per server tick: SetFeetAngles stamps -9999
	// into every animation layer's cycle field. Per-command pitch/yaw the client
	// needs travel on m_skydivePlayerPitch / m_skydivePlayerYaw above.
	const Vector3D vecAngles(flBodyPitch, flNewYaw, 0.0f);

	if (state.m_flLastFeetAnglesTime != flNow)
	{
		state.m_flLastFeetAnglesTime = flNow;
		v_CPlayer__SetFeetAngles(player, &vecAngles);
	}

	MarkEntityEdictDirty(player);

	if (bridge_skydive_diag.GetBool() && (flNow - state.m_flLastDiagTime) > 0.5f)
	{
		state.m_flLastDiagTime = flNow;

		const float flFwdTgt = *reinterpret_cast<const float*>(base + SD_OFF_FWDPOSEVALUETARGET);
		const float flSideTgt = *reinterpret_cast<const float*>(base + SD_OFF_SIDEPOSEVALUETARGET);

		// |velIn| drives min glide angle; poseDt is frameTime, not this command's dt.
		Msg(eDLL_T::SERVER,
			"[SKYDIVE-DIAG] st=%d mt=%d dt=%.4f poseDt=%.4f engDrv=%u |velIn|=%.1f "
			"speed=%.1f->%.1f dive=%.1f "
			"view=%.1f fwdPose=%.1f fwdTgt=%.1f sideTgt=%.1f body=%.1f yaw=%.1f->%.1f "
			"in=(%.2f,%.2f) |velOut|=%.1f\n",
			nState, *reinterpret_cast<const int8_t*>(base + SD_OFF_MOVETYPE), dt,
			gpGlobals ? gpGlobals->frameTime : -1.0f, s_nEngineDriveTicks,
			flSpeedIn, flSpeed, flNewSpeed, flNewPitch, flViewPitch,
			flForwardPose, flFwdTgt, flSideTgt, flBodyPitch, flYaw, flNewYaw,
			flInputForward, flInputRight,
			sqrtf(vecNewVelocity.x * vecNewVelocity.x + vecNewVelocity.y * vecNewVelocity.y +
				vecNewVelocity.z * vecNewVelocity.z));
	}

	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		DevMsg(eDLL_T::SERVER, "[SKYDIVE-SIM] per-usercmd server skydive simulation is live\n");
	}
}

//-----------------------------------------------------------------------------
// Squirrel binding -- Player_BeginSkydive only. Skydive_IsFromSkywardLaunch is
// already registered by Script_RegisterPlayerScriptFunctions.
//-----------------------------------------------------------------------------
static SQRESULT Script_Player_BeginSkydive(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	const SQVector3D* pVec = nullptr;
	sq_getvector(v, 2, &pVec);
	if (!pVec)
		return SQ_ERROR;

	const Vector3D vecInitial(pVec->x, pVec->y, pVec->z);
	SkydiveBridge_BeginFreefall(pEntity, vecInitial);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void SkydiveBridge_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"Player_BeginSkydive",
		"Script_Player_BeginSkydive",
		"Begin freefall / skydive with the given seed velocity",
		"void",
		"vector initialVelocity",
		false,
		Script_Player_BeginSkydive);
}

void VSkydiveBridge::GetAdr(void) const
{
	LogFunAdr("Skydive_CalculatePitch", v_Skydive_CalculatePitch);
	LogFunAdr("Skydive_CalculateYaw", v_Skydive_CalculateYaw);
	LogFunAdr("Skydive_CalculateSpeed", v_Skydive_CalculateSpeed);
	LogFunAdr("Skydive_CalculateStrafeAngle", v_Skydive_CalculateStrafeAngle);
	LogFunAdr("Skydive_UpdatePoseParameterTargets", v_Skydive_UpdatePoseParameterTargets);
	LogFunAdr("Skydive_UpdatePoseParameters", v_Skydive_UpdatePoseParameters);
	LogFunAdr("Skydive_CalculateVelocity", v_Skydive_CalculateVelocity);
	LogFunAdr("CPlayer::SetFeetAngles", v_CPlayer__SetFeetAngles);
	LogFunAdr("CBaseEntity::SetAbsVelocity", v_CBaseEntity__SetAbsVelocity);
	LogFunAdr("CGameMovement::FullTossMove", v_CGameMovement__FullTossMove);
	LogFunAdr("CPlayer::Player_BeginFreefall", v_CPlayer__BeginFreefall);
	LogFunAdr("CPlayer::Player_EndFreefall", v_CPlayer__EndFreefall);
	LogFunAdr("CPlayer::Player_BeginFreefallAnticipate", v_CPlayer__BeginFreefallAnticipate);
	LogFunAdr("CPlayer::Player_EndFreefallAnticipate", v_CPlayer__EndFreefallAnticipate);
	LogFunAdr("Skydive_Movement", v_Skydive_Movement);
	LogFunAdr("CPlayer::IsClassModAvailableForPlayerSetting",
		v_CPlayer__IsClassModAvailableForPlayerSetting);
}

void VSkydiveBridge::GetFun(void) const
{
	// All nine confirmed unique on r5apex_ds -- one hit each, on the SERVER half.
	// The wildcards cover rip-relative displacements and call targets only.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 60 0F 29 74 24 50 0F 28 F3 0F 29 7C 24 40 0F 28 FA "
		"48 8B FA 48 8B D9 48 85 C9 ?? ?? ?? ?? 01 FF 90 E8 02 00 00 84 C0 74 44")
		.GetPtr(v_Skydive_CalculatePitch);

	Module_FindPattern(g_GameDll,
		"40 53 48 81 EC 80 00 00 00 0F 29 74 24 70 0F 28 F2 0F 29 7C 24 60 0F 28 FB "
		"44 0F 29 44 24 50 44 0F 28 C1 48 8B D9 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 01 FF 90 E8 02 00 00")
		.GetPtr(v_Skydive_CalculateYaw);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 60 0F 29 74 24 50 0F 28 F1 0F 29 7C 24 40 0F 28 FB "
		"44 0F 29 44 24 30 44 0F 28 C2 48 8B D9 48 85 C9 ?? ?? ?? ?? 01 FF 90 E8 02 00 00 84 C0 74 34")
		.GetPtr(v_Skydive_CalculateSpeed);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 60 0F 29 74 24 50 0F 28 F2 0F 29 7C 24 40 0F 28 FB "
		"44 0F 29 44 24 30 44 0F 28 C1 48 8B D9 48 85 C9 ?? ?? ?? ?? 01 FF 90 E8 02 00 00 84 C0 74 26")
		.GetPtr(v_Skydive_CalculateStrafeAngle);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 70 0F 29 74 24 60 0F 28 F3 0F 29 7C 24 50 0F 28 FA "
		"44 0F 29 44 24 40 44 0F 28 C1 48 8B D9 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 01 FF 90 E8 02 00 00")
		.GetPtr(v_Skydive_UpdatePoseParameterTargets);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 70 48 8B D9 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 01 FF 90 E8 02 00 00 "
		"84 C0 0F 84 ?? ?? ?? ?? 8B 8B 10 03 00 00")
		.GetPtr(v_Skydive_UpdatePoseParameters);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 81 EC 80 00 00 00 0F 29 74 24 70 0F 28 F3 0F 29 7C 24 60 0F 28 FA "
		"48 8B FA 48 8B D9 48 85 D2 ?? ?? ?? ?? 02 48 8B CA FF 90 E8 02 00 00")
		.GetPtr(v_Skydive_CalculateVelocity);

	Module_FindPattern(g_GameDll,
		"40 53 48 81 EC B0 03 00 00 F3 0F 10 12 48 8B D9 F3 0F 10 25 ?? ?? ?? ?? "
		"0F 2F D4 F3 0F 10 4A 04 F3 0F 10 42 08 F3 0F 11 54 24 40")
		.GetPtr(v_CPlayer__SetFeetAngles);

	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 48 81 EC B0 00 00 00 "
		"48 8B 1D ?? ?? ?? ?? 48 8B E9 44 0F 29 40 C8 48 8B F2 0F BF 41 58")
		.GetPtr(v_CBaseEntity__SetAbsVelocity);

	// engine routine. The `mov r13d, [rdx+7B60h]` tail is load-bearing: it reads
	// m_freefallState at the SERVER CPlayer offset, which is what separates this
	// from the byte-similar client twin in the same binary. One hit.
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 18 55 57 41 55 41 56 41 57 48 8D A8 E8 FC FF FF "
		"48 81 EC F0 03 00 00 48 8B 51 08 45 33 FF 0F 29 70 C8 48 8B F9 0F 29 78 B8 "
		"44 0F 29 40 A8 4C 89 BD 60 01 00 00 44 8B AA 60 7B 00 00")
		.GetPtr(v_CGameMovement__FullTossMove);

	// CPlayer::Player_BeginFreefall(const Vector&) -- the literal 7B60 displacement
	// is the server-half discriminator; the client twin in this same binary reads
	// 0x4098.
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 10 48 89 68 18 57 48 81 EC C0 00 00 00 "
		"83 B9 60 7B 00 00 00 48 8B FA 48 89 70 08 48 8B D9")
		.GetPtr(v_CPlayer__BeginFreefall);

	// CPlayer::Player_EndFreefall -- the two consecutive near calls after the
	// assert (Anim_Stop, then EndFreefallAnticipate) separate it from the three
	// byte-similar siblings, which call nothing there.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 83 B9 60 7B 00 00 00 "
		"48 8B D9 75 0C 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ??")
		.GetPtr(v_CPlayer__EndFreefall);

	// CPlayer::Player_BeginFreefallAnticipate -- byte-identical to
	// EndFreefallAnticipate up to the state it writes, so the pattern has to run
	// all the way to that immediate: 02 here, 01 in the sibling below.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 40 83 B9 60 7B 00 00 00 48 8B D9 75 0C "
		"48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F B7 43 58 41 B8 00 02 00 00 "
		"66 83 F8 FF 74 16 48 0F BF D0 48 8B 05 ?? ?? ?? ?? 48 8B 48 78 "
		"66 F0 44 09 44 51 40 C7 83 60 7B 00 00 02 00 00 00")
		.GetPtr(v_CPlayer__BeginFreefallAnticipate);

	// CPlayer::Player_EndFreefallAnticipate -- same shape, writes 1.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 40 83 B9 60 7B 00 00 00 48 8B D9 75 0C "
		"48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F B7 43 58 41 B8 00 02 00 00 "
		"66 83 F8 FF 74 16 48 0F BF D0 48 8B 05 ?? ?? ?? ?? 48 8B 48 78 "
		"66 F0 44 09 44 51 40 C7 83 60 7B 00 00 01 00 00 00")
		.GetPtr(v_CPlayer__EndFreefallAnticipate);

	// Server FullTossMove call site (prologue is shared with the client twin).
	CMemory site = Module_FindPattern(g_GameDll,
		"0F 10 81 9C 01 00 00 F3 41 0F 10 89 A0 01 00 00 "
		"F3 0F 11 45 E8 F3 0F 11 4D EC 48 8D 55 60 48 8D 4D 90 E8 ?? ?? ?? ??");

	if (site)
		site.Offset(34).FollowNearCallSelf().GetPtr(v_Skydive_Movement);

	// Server-half discriminator: settings guid +0x5DF0, mod mask +0x5EF8 (client 0x2128 / 0x2230).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 74 ?? "
		"80 3A 00 74 ?? 48 8B 89 F0 5D 00 00 4C 8D 44 24 38 E8 ?? ?? ?? ?? 84 C0 75 ??")
		.GetPtr(v_CPlayer__IsClassModAvailableForPlayerSetting);

	if (!SkydiveBridge_Resolved())
		Warning(eDLL_T::SERVER,
			"[SKYDIVE-SIM] one or more skydive patterns unresolved -- server skydive "
			"simulation disabled (the client will be the only author)\n");

	// Without this the module cannot tell whether the authority is already
	// authoring the dive, and would compete with it instead of standing down.
	if (!v_CGameMovement__FullTossMove)
		Warning(eDLL_T::SERVER,
			"[SKYDIVE-ENGINE] CGameMovement::FullTossMove pattern unresolved -- cannot "
			"detect the engine's own skydive movement, so the stand-down is inactive\n");

	if (!v_CPlayer__BeginFreefall)
		Warning(eDLL_T::SERVER,
			"[SKYDIVE] Player_BeginFreefall pattern unresolved -- updraft skydive "
			"handoff cannot start a dive\n");
	if (!v_CPlayer__EndFreefall)
		Warning(eDLL_T::SERVER,
			"[SKYDIVE] Player_EndFreefall pattern unresolved -- freefall teardown and "
			"updraft stage reset on end are disabled\n");
	if (!v_CPlayer__BeginFreefallAnticipate)
		Warning(eDLL_T::SERVER,
			"[SKYDIVE] Player_BeginFreefallAnticipate pattern unresolved -- updraft "
			"handoff cannot force the landing-anticipation pose\n");
	if (!v_CPlayer__EndFreefallAnticipate)
		Warning(eDLL_T::SERVER,
			"[SKYDIVE] Player_EndFreefallAnticipate pattern unresolved -- the updraft "
			"anticipation latch cannot attach and the landing pose will clear from "
			"proximity\n");
	if (!v_Skydive_Movement)
		Warning(eDLL_T::SERVER,
			"[SKYDIVE-LIFT] Skydive_Movement pattern unresolved -- the updraft ride "
			"stops the moment the dive starts\n");
	if (!v_CPlayer__IsClassModAvailableForPlayerSetting)
		Warning(eDLL_T::SERVER,
			"[SKYDIVE] IsClassModAvailableForPlayerSetting pattern unresolved -- "
			"fissure_updraft probe cannot run and the two descent speed overrides "
			"are inert\n");
}

void VSkydiveBridge::Detour(const bool bAttach) const
{
	if (v_CGameMovement__FullTossMove)
		DetourSetup(&v_CGameMovement__FullTossMove, &Hook_CGameMovement__FullTossMove, bAttach);
	if (v_CPlayer__EndFreefall)
		DetourSetup(&v_CPlayer__EndFreefall, &Hook_CPlayer__EndFreefall, bAttach);
	if (v_CPlayer__EndFreefallAnticipate)
		DetourSetup(&v_CPlayer__EndFreefallAnticipate, &Hook_CPlayer__EndFreefallAnticipate, bAttach);
	if (v_CPlayer__BeginFreefall)
		DetourSetup(&v_CPlayer__BeginFreefall, &Hook_CPlayer__BeginFreefall, bAttach);
	if (v_Skydive_Movement)
		DetourSetup(&v_Skydive_Movement, &Hook_Skydive_Movement, bAttach);
}

