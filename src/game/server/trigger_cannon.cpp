//=============================================================================//
//
// Purpose: server-authoritative TT_GRAVITY_CANNON launch. See header.
// Walks the Heavy-trigger list after the native pass and applies the launch.
//
//=============================================================================//
#include "core/stdafx.h"


#include "trigger_cannon.h"
#include "player.h"
#include "baseentity.h"
#include "entitylist.h" // g_serverEntityList
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"
#include <cmath>

//-----------------------------------------------------------------------------
// Raw layout -- movement ctx, CMoveData, player, trigger.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t TC_CTX_OFF_PLAYER   = 8;   // CPlayer*
static constexpr ptrdiff_t TC_CTX_OFF_MOVEDATA = 16;  // CMoveData*

static constexpr ptrdiff_t TC_MV_OFF_VELOCITY = 304;  // float[3] m_vecVelocity

// CBaseEntity fields shared by player and trigger.
static constexpr ptrdiff_t TC_ENT_OFF_EFLAGS     = 560;  // bit 0x800 = abs origin dirty
static constexpr ptrdiff_t TC_ENT_OFF_ABS_ORIGIN = 1104; // float[3]

// CPlayer.
static constexpr ptrdiff_t TC_PLAYER_OFF_MINS           = 824;   // float[3] m_Collision mins
static constexpr ptrdiff_t TC_PLAYER_OFF_MAXS           = 836;   // float[3] m_Collision maxs
static constexpr ptrdiff_t TC_PLAYER_OFF_FLOORHEIGHT_CB = 23184; // change callback object
static constexpr ptrdiff_t TC_PLAYER_OFF_FLOORHEIGHT    = 23968; // float m_wallrunLatestFloorHeight
static constexpr ptrdiff_t TC_PLAYER_OFF_GRAPPLEACTIVE  = 26552; // bool
static constexpr ptrdiff_t TC_PLAYER_OFF_LANDINGTYPE    = 26560; // int m_landingType
static constexpr ptrdiff_t TC_PLAYER_OFF_TOUCHED_TRIG   = 27096; // EHANDLE[16]
static constexpr ptrdiff_t TC_PLAYER_OFF_TOUCHED_COUNT  = 27160; // int64 live entry count
static constexpr ptrdiff_t TC_PLAYER_OFF_LAUNCHCOUNT    = 27172; // int m_launchCount
static constexpr ptrdiff_t TC_PLAYER_OFF_PUNCHBASE_CB   = 27464; // change callback object
static constexpr ptrdiff_t TC_PLAYER_OFF_PUNCHBASE      = 27508; // float[3] m_vecPunchBase_Angle

// CTriggerCylinderHeavy native SendProp offsets.
static constexpr ptrdiff_t TC_TRIG_OFF_LAUNCHPOWER   = 3368; // float m_launchPower
static constexpr ptrdiff_t TC_TRIG_OFF_PUNCHSOFT     = 3372; // float m_punchSoftAmount
static constexpr ptrdiff_t TC_TRIG_OFF_PUNCHHARD     = 3376; // float m_punchHardAmount
static constexpr ptrdiff_t TC_TRIG_OFF_PUNCHRANDOM   = 3380; // float m_punchRandomBoost
static constexpr ptrdiff_t TC_TRIG_OFF_TRIGGERTYPE   = 3384; // int m_triggerType
static constexpr ptrdiff_t TC_TRIG_OFF_LAUNCHDIR     = 3416; // float[3] m_launchDir

// CBaseTrigger m_hTouchingEntities (CUtlVector) live entry count.
static constexpr ptrdiff_t TC_TRIG_OFF_TOUCH_COUNT = 3232;

// CMoveData m_moveDir2D -- the wish direction AirMove reads on its first line.
static constexpr ptrdiff_t TC_MV_OFF_MOVEDIR2D = 120; // float[3]

// AirMove compares its own time fields against gpGlobals->curTime (+16), so the
// flight window is stamped and tested on that clock and not on the predicted
// time the launch pass itself gates on.
static constexpr ptrdiff_t TC_GLOBALS_OFF_CURTIME = 16;

// Native pass gates on gpGlobals+40 (latest predicted time), not curTime (+16).
static constexpr ptrdiff_t TC_GLOBALS_OFF_MOVEMENT_TIME = 40;

static constexpr int TC_TOUCHED_CAP = 16;
static constexpr int TC_TRIGGER_TYPE_GRAVITY_CANNON = 32; // TT_GRAVITY_CANNON

// Entity eflags bit that means "abs origin needs recomputing before it is read".
static constexpr uint32_t TC_EFL_DIRTY_ABSTRANSFORM = 0x800;

// Near-zero launch power: leave fields alone rather than write a NaN dir.
static constexpr float TC_LAUNCH_POWER_EPS = 1e-6f;

// Script always calls SetLaunchDelay, but a cannon authored without it must
// still open a visible charge window rather than firing on the first tick.
static constexpr float TC_DEFAULT_LAUNCH_DELAY = 1.0f;

// Sits between the float rounding error on now + delay and one server tick.
static constexpr float TC_CHARGE_LATCH_EPS = 0.01f;

//-----------------------------------------------------------------------------
// Server-half pointers from the native jump-pad launch body.
//-----------------------------------------------------------------------------
// CTriggerCylinderNetworked::EnterScriptCallback(trigger, other).
static void (*v_CTriggerCylinder__EnterScriptCallback)(void* pTrigger, void* pOther) = nullptr;
// CPlayer::GrappleDetach(player).
static void (*v_CPlayer__GrappleDetach)(void* pPlayer) = nullptr;
// CBaseEntity::SetGroundEntity(player, ground).
static void (*v_CBaseEntity__SetGroundEntity)(void* pEntity, void* pGround) = nullptr;
// CBaseEntity::CalcAbsolutePosition(entity).
static void (*v_CBaseEntity__CalcAbsolutePosition)(void* pEntity) = nullptr;
// CalcPredictedViewPunch(out, origin, mins, maxs, eyeAngles, originCopy, amount, boost).
static void (*v_CalcPredictedViewPunch)(void* pOut, const float* pOrigin,
	const float* pMins, const float* pMaxs, const float* pEyeAngles,
	const float* pOriginCopy, float flAmount, float flBoost) = nullptr;
// CPlayer::ViewPunchBase(player, punch).
static void (*v_CPlayer__ViewPunchBase)(void* pPlayer, const float* pPunch) = nullptr;

// CTriggerCylinderHeavy::EndTouch -- charge disarm when last toucher leaves.
static __int64 (*v_CTriggerCylinderHeavy__EndTouch)(void* self, void* other) = nullptr;

//-----------------------------------------------------------------------------
// ConVars.
//-----------------------------------------------------------------------------
// Inert until content authors a trigger of type 32, so it ships enabled.
static ConVar bridge_trigger_cannon(
	"bridge_trigger_cannon", "1", FCVAR_RELEASE,
	"Apply the server-authoritative TT_GRAVITY_CANNON launch for Heavy triggers "
	"of type 32. The S21 client already predicts this type; without it the server "
	"never launches and the player rubber-bands back.");

// The client inlines a 1.0s launch debounce; keep the two in step.
static ConVar bridge_trigger_cannon_debounce_time(
	"bridge_trigger_cannon_debounce_time", "1.0", FCVAR_RELEASE,
	"Seconds added to the movement clock when arming m_jumpPadDebounceExpireTime "
	"after a gravity-cannon launch. 1.0 matches the client's inlined value.");

// Scales the gravity cannon's flight time, and with it the whole arc shape.
static ConVar bridge_trigger_cannon_arc_loft(
	"bridge_trigger_cannon_arc_loft", "1.25", FCVAR_RELEASE,
	"How lofted a gravity cannon throws, as a multiple of its minimum-energy flight "
	"time. 1.0 is the flattest arc that still reaches the landing point; higher flies "
	"higher and longer, lower is flatter and faster. Clamped to 0.5 - 2.5.");

// The client half registers the same lever. Both engines must agree: a lock on
// one side only is worse than no lock, because the free side keeps predicting
// turns the locked side refuses.
static ConVar bridge_trigger_cannon_lock(
	"bridge_trigger_cannon_lock", "1", FCVAR_RELEASE,
	"Restrict air control for the duration of a gravity-cannon flight, so the "
	"launch lands where it was aimed. Removes the along-the-launch component of "
	"the movement wish direction and denies the lurch, matching how a limited "
	"air-control launcher flies. Must be set the same on client and server.");

static ConVar bridge_trigger_cannon_lock_max_time(
	"bridge_trigger_cannon_lock_max_time", "8.0", FCVAR_RELEASE,
	"Upper bound on how long one gravity-cannon flight may hold air control. The "
	"window is normally the solved flight time; this only catches an absurd solve.");

static ConVar bridge_trigger_cannon_diag(
	"bridge_trigger_cannon_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log [TRIG-CANNON] launches and the reason a candidate trigger was skipped.");

static int s_nLaunchDiagCount = 0;
static int s_nArmDiagCount = 0;
static int s_nDisarmDiagCount = 0;

// Appended prop offsets; resolved lazily after datatable extend.
// 0 = not yet tried, -1 = failed, >0 = entity offset.
static int s_nNextLaunchTimeOff = 0;
static int s_nLaunchDelayOff = 0;
static int s_nLaunchFlightTimeOff = 0;
static int s_nEnableDoubleJumpOff = 0;
static int s_nLimitedAirControlOff = 0;
static int s_nAirControlMoveSpeedOff = 0;
static int s_nAirControlMoveAccelOff = 0;

static ConVar* s_pGravityCvar = nullptr;

//-----------------------------------------------------------------------------
// Purpose: resolve the appended props once; warn once on failure.
//-----------------------------------------------------------------------------
static bool TriggerCannon_ResolveOffsets(void)
{
	if (s_nNextLaunchTimeOff == 0)
	{
		const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", "m_nextLaunchTime");
		s_nNextLaunchTimeOff = nOff > 0 ? nOff : -1;
		if (nOff <= 0)
			Warning(eDLL_T::SERVER,
				"[TRIG-CANNON] m_nextLaunchTime offset unresolved -- "
				"gravity-cannon launch is disabled\n");
	}

	if (s_nLaunchDelayOff == 0)
	{
		const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", "m_launchDelayAmount");
		s_nLaunchDelayOff = nOff > 0 ? nOff : -1;
		if (nOff <= 0)
			Warning(eDLL_T::SERVER,
				"[TRIG-CANNON] m_launchDelayAmount offset unresolved -- "
				"gravity-cannon launch is disabled\n");
	}

	if (s_nLaunchFlightTimeOff == 0)
	{
		const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", "m_launchFlightTime");
		s_nLaunchFlightTimeOff = nOff > 0 ? nOff : -1;
		if (nOff <= 0)
			Warning(eDLL_T::SERVER,
				"[TRIG-CANNON] m_launchFlightTime offset unresolved -- "
				"SetLaunchTargetLocation will not write flight time\n");
	}

	if (s_nEnableDoubleJumpOff == 0)
	{
		const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", "m_enableDoubleJump");
		s_nEnableDoubleJumpOff = nOff > 0 ? nOff : -1;
		if (nOff <= 0)
			Warning(eDLL_T::SERVER,
				"[TRIG-CANNON] m_enableDoubleJump offset unresolved -- "
				"SetEnableDoubleJump will not write\n");
	}

	if (s_nLimitedAirControlOff == 0)
	{
		const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", "m_limitedAirControl");
		s_nLimitedAirControlOff = nOff > 0 ? nOff : -1;
		if (nOff <= 0)
			Warning(eDLL_T::SERVER,
				"[TRIG-CANNON] m_limitedAirControl offset unresolved -- "
				"SetLimitedAirControl will not write\n");
	}

	if (s_nAirControlMoveSpeedOff == 0)
	{
		const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", "m_airControlMoveSpeed");
		s_nAirControlMoveSpeedOff = nOff > 0 ? nOff : -1;
		if (nOff <= 0)
			Warning(eDLL_T::SERVER,
				"[TRIG-CANNON] m_airControlMoveSpeed offset unresolved -- "
				"SetLaunchAirControlParams will not write\n");
	}

	if (s_nAirControlMoveAccelOff == 0)
	{
		const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", "m_airControlMoveAccel");
		s_nAirControlMoveAccelOff = nOff > 0 ? nOff : -1;
		if (nOff <= 0)
			Warning(eDLL_T::SERVER,
				"[TRIG-CANNON] m_airControlMoveAccel offset unresolved -- "
				"SetLaunchAirControlParams will not write\n");
	}

	// Launch path needs next/delay/debounce. Flight time is only for the arc
	// author; its absence does not disable launches that scripts authored
	// through SetLaunchDir / SetLaunchScaleValues.
	return s_nNextLaunchTimeOff > 0 && s_nLaunchDelayOff > 0;
}

static float TriggerCannon_MovementTime(void);
static void TriggerCannon_FireCallback(void* pTrigger, const char* pszFunc,
	const int* pIntArg, void* pEntArg = nullptr);
static float TriggerCannon_Gravity(void);
static void TriggerCannon_ArmFlightLock(const void* pPlayer, const void* pTrigger);

static int TriggerCannon_TouchCount(const void* pTrigger)
{
	if (!pTrigger)
		return 0;

	return *reinterpret_cast<const int*>(
		static_cast<const uint8_t*>(pTrigger) + TC_TRIG_OFF_TOUCH_COUNT);
}

// SetLaunchDelay / GravityCannonIsPreparingLaunch over appended props.
bool TriggerCannon_SetLaunchDelay(void* pTrigger, float flDelay)
{
	if (!pTrigger || !std::isfinite(flDelay) || flDelay < 0.0f)
		return false;

	if (!TriggerCannon_ResolveOffsets())
		return false;

	*reinterpret_cast<float*>(static_cast<uint8_t*>(pTrigger) + s_nLaunchDelayOff) = flDelay;
	MarkEntityEdictDirty(pTrigger);
	return true;
}

bool TriggerCannon_IsPreparingLaunch(const void* pTrigger)
{
	if (!pTrigger || !TriggerCannon_ResolveOffsets())
		return false;

	const uint8_t* const pBytes = static_cast<const uint8_t*>(pTrigger);
	const float flNextLaunch = *reinterpret_cast<const float*>(pBytes + s_nNextLaunchTimeOff);
	const float flDelay = *reinterpret_cast<const float*>(pBytes + s_nLaunchDelayOff);
	const float flNow = TriggerCannon_MovementTime();

	// False on the arming tick (remaining ~= delay). Margin covers float round-trip vs uptime.
	return flNextLaunch > flNow
		&& (flDelay - (flNextLaunch - flNow)) > TC_CHARGE_LATCH_EPS;
}

bool TriggerCannon_GetLaunchDir(const void* pTrigger, float outDir[3])
{
	if (!pTrigger || !outDir)
		return false;

	const float* const pDir = reinterpret_cast<const float*>(
		static_cast<const uint8_t*>(pTrigger) + TC_TRIG_OFF_LAUNCHDIR);

	if (!std::isfinite(pDir[0]) || !std::isfinite(pDir[1]) || !std::isfinite(pDir[2]))
		return false;

	outDir[0] = pDir[0];
	outDir[1] = pDir[1];
	outDir[2] = pDir[2];
	return true;
}

static int TriggerCannon_ResolveNamed(const char* pszName, int* pSlot)
{
	if (*pSlot != 0)
		return *pSlot;

	const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", pszName);
	*pSlot = nOff > 0 ? nOff : -1;
	if (nOff <= 0)
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] %s offset unresolved -- setter will not write\n", pszName);
	return *pSlot;
}

bool TriggerCannon_SetEnableDoubleJump(void* pTrigger, bool bEnable)
{
	if (!pTrigger)
		return false;

	if (TriggerCannon_ResolveNamed("m_enableDoubleJump", &s_nEnableDoubleJumpOff) <= 0)
		return false;

	*reinterpret_cast<int*>(static_cast<uint8_t*>(pTrigger) + s_nEnableDoubleJumpOff) =
		bEnable ? 1 : 0;
	MarkEntityEdictDirty(pTrigger);
	return true;
}

bool TriggerCannon_SetLimitedAirControl(void* pTrigger, bool bLimited)
{
	if (!pTrigger)
		return false;

	if (TriggerCannon_ResolveNamed("m_limitedAirControl", &s_nLimitedAirControlOff) <= 0)
		return false;

	*reinterpret_cast<int*>(static_cast<uint8_t*>(pTrigger) + s_nLimitedAirControlOff) =
		bLimited ? 1 : 0;
	MarkEntityEdictDirty(pTrigger);
	return true;
}

bool TriggerCannon_SetLaunchAirControlParams(void* pTrigger, float flSpeed, float flAccel)
{
	if (!pTrigger || !std::isfinite(flSpeed) || !std::isfinite(flAccel))
		return false;

	if (TriggerCannon_ResolveNamed("m_airControlMoveSpeed", &s_nAirControlMoveSpeedOff) <= 0
		|| TriggerCannon_ResolveNamed("m_airControlMoveAccel", &s_nAirControlMoveAccelOff) <= 0)
		return false;

	uint8_t* const pBytes = static_cast<uint8_t*>(pTrigger);
	*reinterpret_cast<float*>(pBytes + s_nAirControlMoveSpeedOff) = flSpeed;
	*reinterpret_cast<float*>(pBytes + s_nAirControlMoveAccelOff) = flAccel;
	MarkEntityEdictDirty(pTrigger);
	return true;
}

void TriggerCannon_OnJumpPadLaunched(void* pPlayer, void* pTrigger)
{
	if (!pPlayer || !pTrigger)
		return;

	if (TriggerCannon_ResolveNamed("m_limitedAirControl", &s_nLimitedAirControlOff) <= 0)
		return;

	if (*reinterpret_cast<const int*>(
			static_cast<const uint8_t*>(pTrigger) + s_nLimitedAirControlOff) == 0)
		return;

	TriggerCannon_ArmFlightLock(pPlayer, pTrigger);
}

// Picks the launch angle for a speed scaled off the target, rather than the speed
// for a fixed flight time -- the latter made every long shot near-vertical.
bool TriggerCannon_SetLaunchTargetLocation(void* pTrigger, const float target[3])
{
	if (!pTrigger || !target)
		return false;

	if (!std::isfinite(target[0]) || !std::isfinite(target[1]) || !std::isfinite(target[2]))
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] SetLaunchTargetLocation rejected non-finite target\n");
		return false;
	}

	TriggerCannon_ResolveOffsets();

	uint8_t* const pTrigBytes = static_cast<uint8_t*>(pTrigger);

	// Abs origin must be current before the solve reads it.
	const uint32_t nFlags = *reinterpret_cast<const uint32_t*>(
		pTrigBytes + TC_ENT_OFF_EFLAGS);
	if ((nFlags & TC_EFL_DIRTY_ABSTRANSFORM) != 0 && v_CBaseEntity__CalcAbsolutePosition)
		v_CBaseEntity__CalcAbsolutePosition(pTrigger);

	const float* const pOrigin = reinterpret_cast<const float*>(
		pTrigBytes + TC_ENT_OFF_ABS_ORIGIN);
	if (!std::isfinite(pOrigin[0]) || !std::isfinite(pOrigin[1]) || !std::isfinite(pOrigin[2]))
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] SetLaunchTargetLocation rejected non-finite origin\n");
		return false;
	}

	const float flDeltaX = target[0] - pOrigin[0];
	const float flDeltaY = target[1] - pOrigin[1];
	const float flDeltaZ = target[2] - pOrigin[2];
	if (!std::isfinite(flDeltaX) || !std::isfinite(flDeltaY) || !std::isfinite(flDeltaZ))
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] SetLaunchTargetLocation rejected non-finite delta\n");
		return false;
	}

	const float flHoriz = std::sqrt(flDeltaX * flDeltaX + flDeltaY * flDeltaY);
	if (!std::isfinite(flHoriz))
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] SetLaunchTargetLocation rejected non-finite horiz\n");
		return false;
	}

	const float flGravity = TriggerCannon_Gravity();

	// Flight time is the trajectory (min-energy arc * loft). Longer = higher/slower.
	const float flSlantRange = std::sqrt(flHoriz * flHoriz + flDeltaZ * flDeltaZ);
	const float flRise = flDeltaZ + flSlantRange;
	if (!std::isfinite(flRise) || flRise <= TC_LAUNCH_POWER_EPS)
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] SetLaunchTargetLocation skipped: target coincides with "
			"the cannon or lies straight below it\n");
		return false;
	}

	float flLoft = bridge_trigger_cannon_arc_loft.GetFloat();
	if (!std::isfinite(flLoft) || flLoft < 0.5f)
		flLoft = 0.5f;
	else if (flLoft > 2.5f)
		flLoft = 2.5f;

	const float flRefFlight = std::sqrt(flHoriz * flHoriz + flRise * flRise)
		/ std::sqrt(flGravity * flRise);
	const float flFlight = flLoft * flRefFlight;

	if (!std::isfinite(flFlight) || flFlight <= 0.0f)
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] SetLaunchTargetLocation rejected non-finite flight time\n");
		return false;
	}

	// Horizontal speed carries the player across; vertical both covers the rise and
	// pays for staying up for the whole flight.
	const float flHorizSpeed = flHoriz / flFlight;
	const float flVelX = flHoriz > TC_LAUNCH_POWER_EPS
		? (flDeltaX / flHoriz) * flHorizSpeed : 0.0f;
	const float flVelY = flHoriz > TC_LAUNCH_POWER_EPS
		? (flDeltaY / flHoriz) * flHorizSpeed : 0.0f;
	const float flVelZ = flDeltaZ / flFlight + 0.5f * flGravity * flFlight;

	if (!std::isfinite(flVelX) || !std::isfinite(flVelY) || !std::isfinite(flVelZ))
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] SetLaunchTargetLocation rejected non-finite velocity\n");
		return false;
	}

	const float flPower = std::sqrt(flVelX * flVelX + flVelY * flVelY + flVelZ * flVelZ);
	if (!std::isfinite(flPower) || flPower < TC_LAUNCH_POWER_EPS)
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] SetLaunchTargetLocation skipped: power ~0 (target coincides "
			"with origin or solves to zero velocity)\n");
		return false;
	}

	const float flDirX = flVelX / flPower;
	const float flDirY = flVelY / flPower;
	const float flDirZ = flVelZ / flPower;
	if (!std::isfinite(flDirX) || !std::isfinite(flDirY) || !std::isfinite(flDirZ))
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] SetLaunchTargetLocation rejected non-finite dir\n");
		return false;
	}

	float* const pLaunchDir = reinterpret_cast<float*>(pTrigBytes + TC_TRIG_OFF_LAUNCHDIR);
	pLaunchDir[0] = flDirX;
	pLaunchDir[1] = flDirY;
	pLaunchDir[2] = flDirZ;
	*reinterpret_cast<float*>(pTrigBytes + TC_TRIG_OFF_LAUNCHPOWER) = flPower;

	if (s_nLaunchFlightTimeOff > 0)
		*reinterpret_cast<float*>(pTrigBytes + s_nLaunchFlightTimeOff) = flFlight;

	MarkEntityEdictDirty(pTrigger);

	// The arc shape is ours, not a recovered S21 solve, so print what it was
	// derived from -- apex and flight are what the trajectory is judged on.
	if (bridge_trigger_cannon_diag.GetBool())
	{
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] arc horiz=%.0f dz=%.0f g=%.0f loft=%.2f refflight=%.2f "
			"flight=%.2f apex=%.0f vert/horiz=%.2f power=%.0f\n",
			flHoriz, flDeltaZ, flGravity, flLoft, flRefFlight,
			flFlight, (flVelZ * flVelZ) / (2.0f * flGravity),
			flHorizSpeed > 1.0f ? flVelZ / flHorizSpeed : 0.0f,
			flPower);
	}

	return true;
}

static float TriggerCannon_Gravity(void)
{
	if (!s_pGravityCvar && g_pCVar)
		s_pGravityCvar = g_pCVar->FindVar("sv_gravity");

	const float flGravity = s_pGravityCvar ? s_pGravityCvar->GetFloat() : 0.0f;
	return (std::isfinite(flGravity) && flGravity > 0.0f) ? flGravity : 750.0f;
}

// The movement clock the native pass itself compares against.
static float TriggerCannon_MovementTime(void)
{
	if (!gpGlobals)
		return 0.0f;

	return *reinterpret_cast<const float*>(
		reinterpret_cast<const uint8_t*>(gpGlobals) + TC_GLOBALS_OFF_MOVEMENT_TIME);
}

// Engine reads abs origin only after clearing the dirty-transform flag.
static void TriggerCannon_EnsureAbsOrigin(void* pEntity)
{
	const uint32_t nFlags = *reinterpret_cast<const uint32_t*>(
		static_cast<uint8_t*>(pEntity) + TC_ENT_OFF_EFLAGS);

	if ((nFlags & TC_EFL_DIRTY_ABSTRANSFORM) != 0 && v_CBaseEntity__CalcAbsolutePosition)
		v_CBaseEntity__CalcAbsolutePosition(pEntity);
}

// Networked field write: notify first, exactly as the engine's own writers do.
static void TriggerCannon_NotifyNetworkChange(uint8_t* pBase, ptrdiff_t nNotifyOff, ptrdiff_t nFieldOff)
{
	void* const pNotify = pBase + nNotifyOff;
	uintptr_t* const pVtbl = *reinterpret_cast<uintptr_t**>(pNotify);
	if (!pVtbl || !pVtbl[0])
		return;

	reinterpret_cast<void(__fastcall*)(void*, void*)>(pVtbl[0])(pNotify, pBase + nFieldOff);
}

static bool TriggerCannon_IsFiniteVec(const float v[3])
{
	return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

//-----------------------------------------------------------------------------
// Open a charge window at now + m_launchDelayAmount. One window, one presentation.
//-----------------------------------------------------------------------------
static void TriggerCannon_ArmCharge(void* pTrigger, void* pPlayer)
{
	if (!pTrigger || !TriggerCannon_ResolveOffsets())
		return;

	uint8_t* const pTrigBytes = static_cast<uint8_t*>(pTrigger);

	const float flDelay = *reinterpret_cast<const float*>(pTrigBytes + s_nLaunchDelayOff);
	const float flNow = TriggerCannon_MovementTime();
	const float flUseDelay = (std::isfinite(flDelay) && flDelay > 0.0f)
		? flDelay
		: TC_DEFAULT_LAUNCH_DELAY;

	*reinterpret_cast<float*>(pTrigBytes + s_nNextLaunchTimeOff) = flNow + flUseDelay;
	MarkEntityEdictDirty(pTrigger);

	// Touch once per command, not per frame, or several cmds in one frame restart the charge FX.
	TriggerCannon_FireCallback(pTrigger, "GravityCannon_OnTouchTrigger_Server",
		nullptr, pPlayer);

	if (bridge_trigger_cannon_diag.GetBool() && s_nArmDiagCount < 16)
	{
		++s_nArmDiagCount;
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] armed t=%.3f delay=%.3f fires=%.3f\n",
			flNow, flUseDelay, flNow + flUseDelay);
	}
}

//-----------------------------------------------------------------------------
// Purpose: clear m_nextLaunchTime when the last toucher leaves a type-32 heavy.
//-----------------------------------------------------------------------------
static void TriggerCannon_DisarmCharge(void* pTrigger)
{
	if (!pTrigger || !TriggerCannon_ResolveOffsets())
		return;

	float* const pNextLaunch = reinterpret_cast<float*>(
		static_cast<uint8_t*>(pTrigger) + s_nNextLaunchTimeOff);

	if (!(*pNextLaunch > 0.0f))
		return;

	*pNextLaunch = 0.0f;
	MarkEntityEdictDirty(pTrigger);

	if (bridge_trigger_cannon_diag.GetBool() && s_nDisarmDiagCount < 16)
	{
		++s_nDisarmDiagCount;
		Warning(eDLL_T::SERVER, "[TRIG-CANNON] disarmed (last toucher left)\n");
	}
}

static __int64 __fastcall Hook_CTriggerCylinderHeavy_EndTouch(void* self, void* other)
{
	const __int64 nRet = v_CTriggerCylinderHeavy__EndTouch
		? v_CTriggerCylinderHeavy__EndTouch(self, other)
		: 0;

	if (!self || !bridge_trigger_cannon.GetBool())
		return nRet;

	const int nType = *reinterpret_cast<const int*>(
		static_cast<const uint8_t*>(self) + TC_TRIG_OFF_TRIGGERTYPE);
	if (nType != TC_TRIGGER_TYPE_GRAVITY_CANNON)
		return nRet;

	// Original has already removed this toucher; count 0 => last one left.
	if (TriggerCannon_TouchCount(self) != 0)
		return nRet;

	TriggerCannon_DisarmCharge(self);
	return nRet;
}

//-----------------------------------------------------------------------------
// Strip wish along the launch; deny lurch. Window is solved flight time (no lock if unsolved).
//-----------------------------------------------------------------------------
static constexpr int TC_FLIGHT_LOCK_SLOTS = 32;

// Horizontal launch dir is degenerate below this; a straight-up cannon has no
// direction to lock against.
static constexpr float TC_FLIGHT_LOCK_MIN_HORIZ = 1e-6f;

struct TriggerCannonFlightLock_t
{
	const void* pPlayer;
	float flExpireTime;
	float flDirX;
	float flDirY;
};

static TriggerCannonFlightLock_t s_flightLocks[TC_FLIGHT_LOCK_SLOTS] = {};
static int s_nFlightLockDiagCount = 0;

static float TriggerCannon_CurTime(void)
{
	if (!gpGlobals)
		return 0.0f;

	return *reinterpret_cast<const float*>(
		reinterpret_cast<const uint8_t*>(gpGlobals) + TC_GLOBALS_OFF_CURTIME);
}

static TriggerCannonFlightLock_t* TriggerCannon_FindFlightLock(const void* pPlayer, float flNow)
{
	for (int i = 0; i < TC_FLIGHT_LOCK_SLOTS; ++i)
	{
		if (s_flightLocks[i].pPlayer == pPlayer && s_flightLocks[i].flExpireTime > flNow)
			return &s_flightLocks[i];
	}

	return nullptr;
}

//-----------------------------------------------------------------------------
// Claim a lock slot by player, else by expired entry.
//-----------------------------------------------------------------------------
static void TriggerCannon_ArmFlightLock(const void* pPlayer, const void* pTrigger)
{
	if (!bridge_trigger_cannon_lock.GetBool())
		return;

	if (s_nLaunchFlightTimeOff == 0)
	{
		const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", "m_launchFlightTime");
		s_nLaunchFlightTimeOff = nOff > 0 ? nOff : -1;
	}

	if (s_nLaunchFlightTimeOff <= 0)
		return;

	const uint8_t* const pTrigBytes = static_cast<const uint8_t*>(pTrigger);

	const float flFlight = *reinterpret_cast<const float*>(pTrigBytes + s_nLaunchFlightTimeOff);
	if (!std::isfinite(flFlight) || flFlight <= 0.0f)
		return;

	const float* const pLaunchDir = reinterpret_cast<const float*>(
		pTrigBytes + TC_TRIG_OFF_LAUNCHDIR);

	const float flHoriz = std::sqrt(pLaunchDir[0] * pLaunchDir[0] + pLaunchDir[1] * pLaunchDir[1]);
	if (!std::isfinite(flHoriz) || flHoriz < TC_FLIGHT_LOCK_MIN_HORIZ)
		return;

	float flMax = bridge_trigger_cannon_lock_max_time.GetFloat();
	if (!std::isfinite(flMax) || flMax <= 0.0f)
		flMax = 8.0f;

	const float flNow = TriggerCannon_CurTime();
	const float flDuration = flFlight < flMax ? flFlight : flMax;

	TriggerCannonFlightLock_t* pSlot = nullptr;
	for (int i = 0; i < TC_FLIGHT_LOCK_SLOTS; ++i)
	{
		if (s_flightLocks[i].pPlayer == pPlayer)
		{
			pSlot = &s_flightLocks[i];
			break;
		}

		if (!pSlot && s_flightLocks[i].flExpireTime <= flNow)
			pSlot = &s_flightLocks[i];
	}

	if (!pSlot)
		return;

	pSlot->pPlayer = pPlayer;
	pSlot->flExpireTime = flNow + flDuration;
	pSlot->flDirX = pLaunchDir[0] / flHoriz;
	pSlot->flDirY = pLaunchDir[1] / flHoriz;

	if (bridge_trigger_cannon_diag.GetBool() && s_nFlightLockDiagCount < 16)
	{
		++s_nFlightLockDiagCount;
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] air-control locked t=%.3f for %.2fs along (%.3f,%.3f)\n",
			flNow, flDuration, pSlot->flDirX, pSlot->flDirY);
	}
}

bool TriggerCannon_BeginFlightLock(void* pPlayer, void* pMoveData, float savedDir[3])
{
	if (!pPlayer || !pMoveData || !savedDir || !bridge_trigger_cannon_lock.GetBool())
		return false;

	const TriggerCannonFlightLock_t* const pLock =
		TriggerCannon_FindFlightLock(pPlayer, TriggerCannon_CurTime());
	if (!pLock)
		return false;

	float* const pWish = reinterpret_cast<float*>(
		static_cast<uint8_t*>(pMoveData) + TC_MV_OFF_MOVEDIR2D);

	savedDir[0] = pWish[0];
	savedDir[1] = pWish[1];
	savedDir[2] = pWish[2];

	// The projection axis is horizontal, so the vertical wish is untouched --
	// same as the native block, whose axis has its gravity component removed
	// before it is normalised.
	const float flAlong = pWish[0] * pLock->flDirX + pWish[1] * pLock->flDirY;
	if (!std::isfinite(flAlong))
		return true;

	pWish[0] -= flAlong * pLock->flDirX;
	pWish[1] -= flAlong * pLock->flDirY;
	return true;
}

void TriggerCannon_EndFlightLock(void* pMoveData, const float savedDir[3])
{
	if (!pMoveData || !savedDir)
		return;

	float* const pWish = reinterpret_cast<float*>(
		static_cast<uint8_t*>(pMoveData) + TC_MV_OFF_MOVEDIR2D);

	pWish[0] = savedDir[0];
	pWish[1] = savedDir[1];
	pWish[2] = savedDir[2];
}

//-----------------------------------------------------------------------------
// One launch: m_nextLaunchTime gate, no m_launchDir normalize, no m_vertOverride, extra m_launchPower.
//-----------------------------------------------------------------------------
static void TriggerCannon_Launch(void* pCtx, void* pPlayer, void* pTrigger, float flNow)
{
	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);
	const uint8_t* const pTrigBytes = static_cast<const uint8_t*>(pTrigger);

	const float flPower = *reinterpret_cast<const float*>(pTrigBytes + TC_TRIG_OFF_LAUNCHPOWER);
	const float* const pLaunchDir = reinterpret_cast<const float*>(pTrigBytes + TC_TRIG_OFF_LAUNCHDIR);

	// Map/script authored values reach here unvalidated; a non-finite launch
	// would poison the movement integrator for the rest of the session.
	if (!std::isfinite(flPower) || !TriggerCannon_IsFiniteVec(pLaunchDir))
	{
		if (bridge_trigger_cannon_diag.GetBool())
			Warning(eDLL_T::SERVER,
				"[TRIG-CANNON] skipped: non-finite power=%f dir=(%f,%f,%f)\n",
				flPower, pLaunchDir[0], pLaunchDir[1], pLaunchDir[2]);
		return;
	}

	float* const pVelocity = reinterpret_cast<float*>(
		static_cast<uint8_t*>(*reinterpret_cast<void**>(
			static_cast<uint8_t*>(pCtx) + TC_CTX_OFF_MOVEDATA)) + TC_MV_OFF_VELOCITY);

	TriggerPass_EnterScriptCallback(pTrigger, pPlayer);

	if (*reinterpret_cast<const uint8_t*>(pPlayerBytes + TC_PLAYER_OFF_GRAPPLEACTIVE)
		&& v_CPlayer__GrappleDetach)
		v_CPlayer__GrappleDetach(pPlayer);

	// Straight set, and the direction is used raw -- the cannon has no
	// normalisation and no vert-override fallback, unlike the jump pad.
	const float flVelZ = pLaunchDir[2] * flPower;
	pVelocity[0] = pLaunchDir[0] * flPower;
	pVelocity[1] = pLaunchDir[1] * flPower;
	pVelocity[2] = flVelZ;

	if (v_CBaseEntity__SetGroundEntity)
		v_CBaseEntity__SetGroundEntity(pPlayer, nullptr);

	// Two view punches, soft then hard, both taking m_punchRandomBoost.
	const float flRandomBoost = *reinterpret_cast<const float*>(pTrigBytes + TC_TRIG_OFF_PUNCHRANDOM);
	float softPunch[3] = {};
	float hardPunch[3] = {};

	if (v_CalcPredictedViewPunch && CPlayer__EyeAngles)
	{
		const float* const pMins = reinterpret_cast<const float*>(pPlayerBytes + TC_PLAYER_OFF_MINS);
		const float* const pMaxs = reinterpret_cast<const float*>(pPlayerBytes + TC_PLAYER_OFF_MAXS);

		for (int nPass = 0; nPass < 2; ++nPass)
		{
			TriggerCannon_EnsureAbsOrigin(pPlayer);

			const float* const pOrigin = reinterpret_cast<const float*>(
				pPlayerBytes + TC_ENT_OFF_ABS_ORIGIN);
			float originCopy[3] = { pOrigin[0], pOrigin[1], pOrigin[2] };

			QAngle eyeAngles;
			CPlayer__EyeAngles(reinterpret_cast<CPlayer*>(pPlayer), &eyeAngles);

			const float flAmount = *reinterpret_cast<const float*>(
				pTrigBytes + (nPass == 0 ? TC_TRIG_OFF_PUNCHSOFT : TC_TRIG_OFF_PUNCHHARD));

			v_CalcPredictedViewPunch(nPass == 0 ? softPunch : hardPunch,
				pOrigin, pMins, pMaxs,
				reinterpret_cast<const float*>(&eyeAngles), originCopy,
				flAmount, flRandomBoost);
		}

		if (v_CPlayer__ViewPunchBase)
			v_CPlayer__ViewPunchBase(pPlayer, softPunch);

		// The hard punch is added straight into m_vecPunchBase_Angle.
		float* const pPunchBase = reinterpret_cast<float*>(pPlayerBytes + TC_PLAYER_OFF_PUNCHBASE);
		if (TriggerCannon_IsFiniteVec(hardPunch)
			&& (hardPunch[0] != 0.0f || hardPunch[1] != 0.0f || hardPunch[2] != 0.0f))
		{
			TriggerCannon_NotifyNetworkChange(pPlayerBytes,
				TC_PLAYER_OFF_PUNCHBASE_CB, TC_PLAYER_OFF_PUNCHBASE);
			pPunchBase[0] += hardPunch[0];
			pPunchBase[1] += hardPunch[1];
			pPunchBase[2] += hardPunch[2];
		}
	}

	*reinterpret_cast<int*>(pPlayerBytes + TC_PLAYER_OFF_LANDINGTYPE) = 1;

	const int nLaunchCount = *reinterpret_cast<int*>(pPlayerBytes + TC_PLAYER_OFF_LAUNCHCOUNT) + 1;
	*reinterpret_cast<int*>(pPlayerBytes + TC_PLAYER_OFF_LAUNCHCOUNT) = nLaunchCount;

	// Arming inside the launch is what stops a second cannon in the same tick,
	// and it is the gate the client's own predicted cannon reads.
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_jumpPadDebounceExpireTime),
		flNow + bridge_trigger_cannon_debounce_time.GetFloat());

	MarkEntityEdictDirty(pPlayer);

	// Hold the arc for the flight. The client half arms the same window off the
	// same trigger fields in its own predicted launch.
	TriggerCannon_ArmFlightLock(pPlayer, pTrigger);

	// Apex prediction. The extra m_launchPower factor is not a typo -- both the
	// S21 client and its reference build square (velZ * m_launchPower).
	TriggerCannon_EnsureAbsOrigin(pPlayer);
	const float flApexSpeed = flVelZ * flPower;
	const float flGravity = TriggerCannon_Gravity();
	const float flOriginZ = *reinterpret_cast<const float*>(
		pPlayerBytes + TC_ENT_OFF_ABS_ORIGIN + 8);
	const float flFloorHeight = ((flApexSpeed * flApexSpeed) / (flGravity + flGravity)) + flOriginZ;

	float* const pFloorHeight = reinterpret_cast<float*>(pPlayerBytes + TC_PLAYER_OFF_FLOORHEIGHT);
	if (std::isfinite(flFloorHeight) && *pFloorHeight != flFloorHeight)
	{
		TriggerCannon_NotifyNetworkChange(pPlayerBytes,
			TC_PLAYER_OFF_FLOORHEIGHT_CB, TC_PLAYER_OFF_FLOORHEIGHT);
		*pFloorHeight = flFloorHeight;
	}

	if (bridge_trigger_cannon_diag.GetBool() && s_nLaunchDiagCount < 32)
	{
		++s_nLaunchDiagCount;
		Warning(eDLL_T::SERVER,
			"[TRIG-CANNON] launch t=%.3f power=%.1f dir=(%.3f,%.3f,%.3f) "
			"vel=(%.1f,%.1f,%.1f) launches=%d apexZ=%.1f\n",
			flNow, flPower, pLaunchDir[0], pLaunchDir[1], pLaunchDir[2],
			pVelocity[0], pVelocity[1], pVelocity[2], nLaunchCount, flFloorHeight);
	}
}

//-----------------------------------------------------------------------------
// Fire script callback (trigger), (trigger, int), or (trigger, entity).
//-----------------------------------------------------------------------------
static void TriggerCannon_FireCallback(void* pTrigger, const char* const pszFunc,
	const int* const pIntArg, void* pEntArg)
{
	if (!g_pServerScript || !pTrigger || !pszFunc)
		return;

	const HSCRIPT hTrigger =
		reinterpret_cast<CBaseEntity*>(pTrigger)->GetScriptInstance();
	if (!hTrigger)
		return;

	const HSCRIPT hFunc = g_pServerScript->FindFunction(pszFunc, nullptr, nullptr);
	if (!hFunc)
		return;

	ScriptVariant_t args[2];
	args[0] = hTrigger;
	int nArgs = 1;

	if (pIntArg)
	{
		args[1] = *pIntArg;
		nArgs = 2;
	}
	else if (pEntArg)
	{
		const HSCRIPT hEnt = reinterpret_cast<CBaseEntity*>(pEntArg)->GetScriptInstance();
		if (!hEnt)
			return;

		args[1] = hEnt;
		nArgs = 2;
	}

	g_pServerScript->ExecuteFunction(hFunc, args, nArgs, nullptr, nullptr);
}

//-----------------------------------------------------------------------------
// Purpose: walk the player's already-built touching-Heavy list. The pass owns
// arming, gating and launching for type 32; EndTouch owns only the disarm.
//-----------------------------------------------------------------------------
void TriggerCannon_ApplyPass(void* pCtx)
{
	if (!pCtx || !bridge_trigger_cannon.GetBool() || !g_serverEntityList)
		return;

	void* const pPlayer = *reinterpret_cast<void**>(
		static_cast<uint8_t*>(pCtx) + TC_CTX_OFF_PLAYER);
	void* const pMoveData = *reinterpret_cast<void**>(
		static_cast<uint8_t*>(pCtx) + TC_CTX_OFF_MOVEDATA);
	if (!pPlayer || !pMoveData)
		return;

	if (!TriggerCannon_ResolveOffsets())
		return;

	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);

	int64_t nCount = *reinterpret_cast<int64_t*>(pPlayerBytes + TC_PLAYER_OFF_TOUCHED_COUNT);
	if (nCount <= 0)
		return;
	if (nCount > TC_TOUCHED_CAP)
		nCount = TC_TOUCHED_CAP;

	const float flNow = TriggerCannon_MovementTime();

	for (int i = 0; i < static_cast<int>(nCount); ++i)
	{
		const uint32_t rawHandle = *reinterpret_cast<uint32_t*>(
			pPlayerBytes + TC_PLAYER_OFF_TOUCHED_TRIG + 4 * i);
		if (rawHandle == 0xFFFFFFFFu)
			continue;

		const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
		void* const pTrigger = g_serverEntityList->LookupEntity(handle);
		if (!pTrigger)
			continue;

		uint8_t* const pTrigBytes = static_cast<uint8_t*>(pTrigger);
		if (*reinterpret_cast<const int*>(pTrigBytes + TC_TRIG_OFF_TRIGGERTYPE)
			!= TC_TRIGGER_TYPE_GRAVITY_CANNON)
			continue;

		const float flNextLaunch = *reinterpret_cast<const float*>(
			pTrigBytes + s_nNextLaunchTimeOff);

		if (!(flNextLaunch > 0.0f))
		{
			// Presence opens the window -- this is the arming the dedi engine
			// does not do for type 32. Arming also raises the touch callback.
			TriggerCannon_ArmCharge(pTrigger, pPlayer);
		}
		else if (flNow > flNextLaunch)
		{
			// LaunchOnTouch: the window elapsed. The per-player debounce is the
			// only thing separating two launches, on the server and in the
			// client's prediction alike.
			const float flDebounce = PlayerExtend_GetF32(pPlayerBytes,
				offsetof(PlayerExtendWire, m_jumpPadDebounceExpireTime));

			if (flNow > flDebounce)
			{
				TriggerCannon_Launch(pCtx, pPlayer, pTrigger, flNow);

				const int nLaunched = 1;
				TriggerCannon_FireCallback(pTrigger,
					"CodeCallback_GravityCannonLaunch", &nLaunched);

				// Close the window; presence re-arms. Do not reopen here (restarts charge FX).
				*reinterpret_cast<float*>(pTrigBytes + s_nNextLaunchTimeOff) = 0.0f;
				MarkEntityEdictDirty(pTrigger);
			}
		}
	}
}

//-----------------------------------------------------------------------------
// IDetour
//-----------------------------------------------------------------------------
void VTriggerCannonBridge::GetAdr(void) const
{
	LogFunAdr("CTriggerCylinderNetworked::EnterScriptCallback", v_CTriggerCylinder__EnterScriptCallback);
	LogFunAdr("CPlayer::GrappleDetach", v_CPlayer__GrappleDetach);
	LogFunAdr("CBaseEntity::SetGroundEntity", v_CBaseEntity__SetGroundEntity);
	LogFunAdr("CBaseEntity::CalcAbsolutePosition", v_CBaseEntity__CalcAbsolutePosition);
	LogFunAdr("CalcPredictedViewPunch", v_CalcPredictedViewPunch);
	LogFunAdr("CPlayer::ViewPunchBase", v_CPlayer__ViewPunchBase);
	LogFunAdr("CTriggerCylinderHeavy::EndTouch", v_CTriggerCylinderHeavy__EndTouch);
}

void VTriggerCannonBridge::GetFun(void) const
{
	// Server half: jump-pad launch body. Twin discriminator: [rcx+0CB8h] vs client [rcx+0AE8h].
	Module_FindPattern(g_GameDll,
		"48 89 74 24 ?? 57 48 83 EC ?? 81 B9 B8 0C 00 00 01 00 00 01 48 8B F9 74 ?? 48 8B CA")
		.GetPtr(v_CTriggerCylinder__EnterScriptCallback);

	// mov eax,[rcx+230h]; shr eax,0Ch; test al,1 -- grapple state bit test.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 30 8B 81 30 02 00 00 48 8B D9 C1 E8 0C A8 01 74 05 E8")
		.GetPtr(v_CPlayer__GrappleDetach);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 41 56 41 57 48 83 EC ?? 4C 8D B1")
		.GetPtr(v_CBaseEntity__SetGroundEntity);

	Module_FindPattern(g_GameDll,
		"40 55 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 8B 81")
		.GetPtr(v_CBaseEntity__CalcAbsolutePosition);

	Module_FindPattern(g_GameDll,
		"48 8B C4 53 48 81 EC ?? ?? ?? ?? F3 0F 10 0D")
		.GetPtr(v_CalcPredictedViewPunch);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? F3 0F 10 05 ?? ?? ?? ?? 48 8D 99 ?? ?? ?? ?? F3 0F 10 53")
		.GetPtr(v_CPlayer__ViewPunchBase);

	// Server-half field offsets in the immediates separate these from the
	// listen-server client twin; do not wildcard them.
	Module_FindPattern(g_GameDll,
		"40 53 55 41 56 48 83 EC 20 4C 8B F2 48 8B E9 48 85 D2 74 05 "
		"8B 42 08 EB 05 B8 FF FF FF FF 48 8D 54 24 40 89 44 24 40 48 81 C1 88 0C 00 00")
		.GetPtr(v_CTriggerCylinderHeavy__EndTouch);

	if (!v_CTriggerCylinder__EnterScriptCallback)
		Warning(eDLL_T::SERVER, "[TRIG-CANNON] EnterScriptCallback pattern unresolved -- cannon triggers will not fire script enter events\n");
	if (!v_CPlayer__GrappleDetach)
		Warning(eDLL_T::SERVER, "[TRIG-CANNON] GrappleDetach pattern unresolved -- a grappling player will keep the grapple through a cannon launch\n");
	if (!v_CBaseEntity__SetGroundEntity)
		Warning(eDLL_T::SERVER, "[TRIG-CANNON] SetGroundEntity pattern unresolved -- launched players stay parented to the ground\n");
	if (!v_CBaseEntity__CalcAbsolutePosition)
		Warning(eDLL_T::SERVER, "[TRIG-CANNON] CalcAbsolutePosition pattern unresolved -- view punch and apex use a possibly stale origin\n");
	if (!v_CalcPredictedViewPunch)
		Warning(eDLL_T::SERVER, "[TRIG-CANNON] CalcPredictedViewPunch pattern unresolved -- cannon launches without view punch\n");
	if (!v_CPlayer__ViewPunchBase)
		Warning(eDLL_T::SERVER, "[TRIG-CANNON] ViewPunchBase pattern unresolved -- the soft view punch is dropped\n");
	if (!v_CTriggerCylinderHeavy__EndTouch)
		Warning(eDLL_T::SERVER, "[TRIG-CANNON] CTriggerCylinderHeavy::EndTouch pattern unresolved -- leave without launch leaves a stale charge\n");
}

void VTriggerCannonBridge::Detour(const bool bAttach) const
{
	if (v_CTriggerCylinderHeavy__EndTouch)
		DetourSetup(&v_CTriggerCylinderHeavy__EndTouch,
			&Hook_CTriggerCylinderHeavy_EndTouch, bAttach);
}

//-----------------------------------------------------------------------------
// Shared movement-pass helpers for the lift/blackhole sibling. Null-safe: an
// unresolved pattern leaves the wrapper a no-op / zero return.
//-----------------------------------------------------------------------------
void TriggerPass_EnterScriptCallback(void* pTrigger, void* pOther)
{
	if (pTrigger && pOther && v_CTriggerCylinder__EnterScriptCallback)
		v_CTriggerCylinder__EnterScriptCallback(pTrigger, pOther);
}

void TriggerPass_SetGroundEntityNull(void* pPlayer)
{
	if (pPlayer && v_CBaseEntity__SetGroundEntity)
		v_CBaseEntity__SetGroundEntity(pPlayer, nullptr);
}

void TriggerPass_SetGroundEntity(void* pPlayer, void* pGround)
{
	if (pPlayer && v_CBaseEntity__SetGroundEntity)
		v_CBaseEntity__SetGroundEntity(pPlayer, pGround);
}

void TriggerPass_EnsureAbsOrigin(void* pEntity)
{
	if (!pEntity)
		return;

	TriggerCannon_EnsureAbsOrigin(pEntity);
}

float TriggerPass_MovementTime(void)
{
	return TriggerCannon_MovementTime();
}

float TriggerPass_FrameTime(void)
{
	if (!gpGlobals)
		return 0.0f;

	return gpGlobals->frameTime;
}

