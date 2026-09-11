//=============================================================================//
//
// Purpose: on auto-detach apply the client's exit-velocity rules and stop
// at the client's auto-detach alpha so both engines leave the rope together.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "zipline_exit_parity.h"
#include "entitylist.h" // g_serverEntityList
#include "player.h" // CPlayer__EyePosition
#include "baseentity.h"
#include "engine/cmodel.h"
#include "engine/server/snapshot_diag.h"
#include "engine/server/zipline_validation.h" // ZiprailDedi_GetWireBlock
#include "trigger_cannon.h" // TriggerPass_MovementTime
#include "zipline_cooldown.h" // ZiplineCooldown_ShouldRefuseMount / _OnMountGranted
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstdarg>

//-----------------------------------------------------------------------------
// CPlayer / CZipline layout -- server half only.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t ZE_PLAYER_OFF_ACTIVEZIPLINE = 26580; // EHANDLE, -1 = none
static constexpr ptrdiff_t ZE_PLAYER_OFF_SETTINGS      = 24328; // SettingsBlockData data ptr
static constexpr ptrdiff_t ZE_PLAYER_OFF_ZIPLINESTATE  = 26596; // int m_ziplineState
static constexpr ptrdiff_t ZE_PLAYER_OFF_ZIPUSEPOS     = 26664; // Vector m_ziplineUsePosition
static constexpr ptrdiff_t ZE_PLAYER_OFF_SLIDINGALPHA  = 26676; // float, ride-integrated alpha
static constexpr ptrdiff_t ZE_PLAYER_OFF_NETSTATECHANGED = 26600; // CNetworkVar change functor
static constexpr ptrdiff_t ZE_PLAYER_OFF_ZIPREVERSE    = 26692; // bool, riding the rope backwards
// bit 0x02: impulse applied; bit 0x4000004: duck-detach speed clamp when enabled
static constexpr ptrdiff_t ZE_PLAYER_OFF_PLAYERFLAGS   = 24800;
static constexpr ptrdiff_t ZE_PLAYER_OFF_JUMPOFFDIR    = 26156; // Vector m_jumpOffImpulseDir
static constexpr ptrdiff_t ZE_PLAYER_OFF_PHASE_START   = 5556;  // float; gravity-bridge verified
static constexpr ptrdiff_t ZE_PLAYER_OFF_PHASE_END     = 5560;  // float; gravity-bridge verified

static constexpr ptrdiff_t ZE_ENT_OFF_LIFESTATE        = 1177;  // char m_lifeState
static constexpr ptrdiff_t ZE_ENT_OFF_REALMSBITMASK    = 0xAE8; // i64 m_realmsBitMask

static constexpr ptrdiff_t ZE_ZIP_OFF_TYPE         = 2844;  // int; 2 = vertical
// DT_Zipline puts m_ziplinePhysics at CZipline+2832, and DT_ZiplinePhysics places
// m_attachedEntities at physics+576 (8 elements, stride 32) with the element's
// EHANDLE at element+8, m_numAttachedEntities at physics+832.
static constexpr ptrdiff_t ZE_ZIP_OFF_ATTACHED_ENTS  = 3416; // EHANDLE, stride 32
static constexpr ptrdiff_t ZE_ZIP_OFF_ATTACHED_COUNT = 3664; // int
// DT_ZiplinePhysicsExlusive census fields (absolute CZipline offsets).
static constexpr ptrdiff_t ZE_ZIP_OFF_NUM_NODES      = 3384; // int m_numNodes
static constexpr ptrdiff_t ZE_ZIP_OFF_SPRING_DIST    = 3388; // float m_springDistance
static constexpr ptrdiff_t ZE_ZIP_OFF_SPRING_SCALE   = 3392; // float m_springDistanceScale
static constexpr ptrdiff_t ZE_ZIP_OFF_REMAIN_UNSIM   = 3396; // float m_remainingUnsimulatedTime
static constexpr ptrdiff_t ZE_ZIP_OFF_NUM_REST_POS   = 3904; // int m_numZiplineRestPositions
static constexpr ptrdiff_t ZE_ZIP_OFF_SPEED_SCALE    = 3912; // float m_ziplineSpeedScale
static constexpr ptrdiff_t ZE_ZIP_OFF_NUM_ZIP_POINTS = 3920; // int m_numZiplinePoints
static constexpr ptrdiff_t ZE_ZIP_VTBL_ISZIPLINE    = 0x310; // byte offset into the vtable
static constexpr ptrdiff_t ZE_ZIP_VTBL_ISZIPLINEEND = 0x320; // byte offset into the vtable

// CBaseEntity::IsPlayer -- vtable slot 93 (updraft/slip-diag verified).
static constexpr int ZE_VTBL_SLOT_ISPLAYER = 93;

// GetPoints overwrites *ioCount with the entity's count (not a capacity). Cap 32.
static constexpr int ZE_MAX_POINTS = 32;
static constexpr int ZE_MAX_ATTACHED = 8; // m_attachedEntities element count
static constexpr int ZE_ZIP_TYPE_VERTICAL = 2;

// Engine's own degenerate-length guard for the re-derived direction (sqrt form).
static const float ZE_SQRT_FLT_MIN = sqrtf(FLT_MIN);

//-----------------------------------------------------------------------------
// Engine function pointers.
//-----------------------------------------------------------------------------
// CPlayer::Zipline_CheckAutoDetach -- returns 1 when the ride must stop.
static char (*v_CPlayer__Zipline_CheckAutoDetach)(void* player, float* vel) = nullptr;

// CZipline::GetPoints -- outDistances may be null.
static __int64 (*v_CZipline__GetPoints)(void* zip, float* outPoints, float* outDistances, int* ioCount) = nullptr;

// CPlayer::Zipline_MoveAlongRope. bTypeOne is m_ziplineType==1 (dangling ceiling + half-hull offset).
static float* (*v_Zipline_MoveAlongRope)(char bReverse, char bTypeOne,
	float flAcceleration, float flZiplineSpeed,
	const float* pPoints, const float* pDistances, int nPoints,
	float flAlphaIn, const float* pPlayerVelocity,
	float* pOutPosition, float* pOutVelocity, float* pOutAlpha) = nullptr;

// CPlayer::Zipline_MoveUpdateSlide / MoveUpdateMount -- the two ride passes. Both
// are wrapped only to publish the current player to the MoveAlongRope hook.
static char (*v_Zipline_MoveUpdateSlide)(void* player, float* position, float* velocity,
	const void* pAbsViewAngles, float flForwardMove, float flSideMove) = nullptr;
static char (*v_Zipline_MoveUpdateMount)(void* player, float* position, float* velocity,
	const void* pAbsViewAngles, float flForwardMove, float flSideMove) = nullptr;

// Player collision hull height (maxs.z - mins.z).
static float (*v_Zipline_GetEntityHullHeight)(void* player) = nullptr;

// Authored auto-detach radius of a zipline entity; 350.0 when the entity is none
// of the three zipline classes.
static float (*v_Zipline_GetAutoDetachDistanceForEntity)(void* zipEnt) = nullptr;

// Next / previous entity in the rope chain; null at the respective chain end.
static void* (*v_Zipline_GetNextEntity)(void* zipEnt) = nullptr;
static void* (*v_Zipline_GetPrevEntity)(void* zipEnt) = nullptr;

// CBaseEntity::GetAbsOrigin -- resolves the dirty flag before returning.
static const float* (*v_CBaseEntity_GetAbsOrigin)(void* pEntity) = nullptr;

// CPlayer::EyePosition(Vector* out, bool bFollowViewEntity) -- the three-argument
// core, which is what the auto-detach distance gate calls; the two-argument thunk
// in player.h is a different symbol and is not on that path.
static float* (*v_CPlayer__EyePositionCore)(void* player, float* pOut,
	char bFollowViewEntity) = nullptr;

// CPlayer::Zipline_Use -- +use mount entry; false before Find means permission gate.
static bool (*v_Zipline_Use)(uintptr_t player, bool forGrappleZipline) = nullptr;

// Zipline_Find -- mount search (7 args on dedi; no rail-direction out-param).
static bool (*v_Zipline_Find)(uintptr_t player,
	const float* playerPosition, const float* eyeDirection,
	uintptr_t* outZipline, float* outMountStartPosition,
	float* outUsePosition, bool* outReverse) = nullptr;

// Zipline_JumpOff -- manual detach; returns non-zero when the jump was applied.
static char (*v_Zipline_JumpOff)(void* zip, void* player, float* velocity,
	const void* eyeAngles, float flForwardMove, float flSideMove) = nullptr;

// Entity Use handler that is the only direct caller of Zipline_Use on the mount path.
static char (*v_CZipline_HandleUse)(uintptr_t player, void* zipEnt,
	uintptr_t caller, int nUseType) = nullptr;

// Runtime-filled settings-field offset for ziplineSpeed (0xFFFFFFFF until loaded).
static const uint32_t* g_pZiplineSpeedFieldOffset = nullptr;

//-----------------------------------------------------------------------------
// Ride context -- published by the two pass wrappers, consumed by MoveAlongRope
// and CheckAutoDetach in the same call frame on the movement thread.
//-----------------------------------------------------------------------------
struct ZiplineRideCtx_t
{
	void*  pPlayer;          // player currently inside a ride pass
	float* pMovePosition;    // slide only: the pass's position vector
	bool   bSlide;           // false = mount pass
	bool   bAlphaMaxValid;   // MoveAlongRope published a ceiling this tick
	bool   bReverse;
	float  flAlphaMax;
};
static ZiplineRideCtx_t s_rideCtx = {};

//-----------------------------------------------------------------------------
// ConVars.
//-----------------------------------------------------------------------------
static ConVar bridge_zip_exit_parity("bridge_zip_exit_parity", "1", FCVAR_RELEASE,
	"Master switch for zipline auto-detach exit-velocity parity on the dedi. "
	"0 = leave velocity as the ride left it (old server behaviour).");

static ConVar bridge_zip_exit_debug("bridge_zip_exit_debug", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-EXIT] Log every auto-detach rewrite, not just the sampled set.");

static ConVar bridge_zip_detach_alpha_parity("bridge_zip_detach_alpha_parity", "1", FCVAR_RELEASE,
	"Stop the ride at the same point along the rope the client stops at, and let the "
	"auto-detach gate fire there. 0 = ride to the end of the rope (old server behaviour).");

static ConVar bridge_zip_alpha_diag("bridge_zip_alpha_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-ALPHA] Log the rope stop-point census every tick of every ride.");

static ConVar bridge_zip_detach_ref_parity("bridge_zip_detach_ref_parity", "1", FCVAR_RELEASE,
	"Measure the auto-detach radius from origin+hullHeight like the S21 client, "
	"instead of the eye position the S3 dedi uses.");

static ConVar bridge_zip_detach_ref_diag("bridge_zip_detach_ref_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log every substituted auto-detach reference point and its distance from the eye.");

static ConVar bridge_zip_player_in_front("bridge_zip_player_in_front", "1", FCVAR_RELEASE,
	"Scale ride speed/accel when another living player is ahead on the same rope "
	"(speed*0.25, accel*0.5). 0 = full speed regardless of riders ahead.");

static ConVar bridge_zip_front_diag("bridge_zip_front_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-FRONT] Census every player-in-front evaluation (reject reasons, fire tally).");

static ConVar bridge_zip_rail_ride_parity("bridge_zip_rail_ride_parity", "1", FCVAR_RELEASE,
	"Integrate a ziprail ride with the client's own slope-aware speed model over the "
	"published path length. 0 = ride it as a plain rope at a flat capped speed "
	"(old server behaviour, diverges from the client on every sloped rail).");

static ConVar bridge_zip_interior_no_clamp("bridge_zip_interior_no_clamp", "1", FCVAR_RELEASE,
	"Leave the alpha ceiling at the engine's own end-of-segment value while the rope "
	"continues past the segment being ridden. The handoff to the next segment fires on "
	"an exact alpha == 1.0 compare, so any lower ceiling strands the ride short of the "
	"node. 0 = clamp interior segments too.");

static ConVar bridge_zip_rail_diag("bridge_zip_rail_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-RAIL] Log the ziprail ride integration every tick, including the "
	"path-length-vs-polyline delta that measures ride-geometry reconciliation.");

static ConVar bridge_zip_geom_diag("bridge_zip_geom_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-GEOM] Log the ride polyline census (points + raw segment distances) "
	"as the dedicated server sees it each tick of a ride.");

static ConVar bridge_zip_rail_find_inject("bridge_zip_rail_find_inject", "1", FCVAR_RELEASE,
	"Name a nearby ziprail as the mount candidate when the engine's own search "
	"returns nothing. A promoted rail is not reachable through the spatial partition "
	"the search discovers candidates with, so without this it can never be mounted. "
	"Rails only; a miss on any other zipline is left a miss.");

static ConVar bridge_zip_rail_find_range("bridge_zip_rail_find_range", "120", FCVAR_RELEASE,
	"Distance from the player's search origin within which a ziprail may be named as "
	"the mount candidate. Matches the engine's own collector radius; raising it lets "
	"players mount rails the S21 client would not offer.", true, 0.0f, true, 400.0f);

static ConVar bridge_zip_mount_diag("bridge_zip_mount_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log the dedicated server's zipline mount attempts (Zipline_Use / Zipline_Find).");

static ConVar bridge_zip_mount_alpha_seed("bridge_zip_mount_alpha_seed", "1", FCVAR_RELEASE,
	"Seed the ride alpha from the mount position. The engine leaves the previous "
	"ride's alpha in place, and its auto-detach gate compares that value to 1.0 "
	"exactly, so a single-segment rope detaches on the first mount tick without this.");

static ConVar bridge_zip_rail_mount_parity("bridge_zip_rail_mount_parity", "1", FCVAR_RELEASE,
	"Recompute a ziprail mount's reverse flag and use position from the baked rail path.");

// S21-only rail settings. The dedicated-server binary carries none of these
// fields, which makes the convar lever the only supply path on a normal run.
static ConVar sdk_ziprail_tune_accel("sdk_ziprail_tune_accel", "150", FCVAR_RELEASE,
	"Override player ziprailAcceleration. S21 default is 150; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_tune_grav_accel("sdk_ziprail_tune_grav_accel", "400", FCVAR_RELEASE,
	"Override player pathGravityAcceleration. S21 default is 400; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_tune_max_speed_mul("sdk_ziprail_tune_max_speed_mul", "1.20000005", FCVAR_RELEASE,
	"Override player maxSpeedMultiplier. S21 default is 1.20000005; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_tune_min_speed_frac("sdk_ziprail_tune_min_speed_frac", "0.569999993", FCVAR_RELEASE,
	"Override player minSpeedFraction. S21 default is 0.569999993; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_tune_detach_speed("sdk_ziprail_tune_detach_speed", "450", FCVAR_RELEASE,
	"Override player ziprailDetachSpeed. S21 default is 450; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_tune_max_jumpoff("sdk_ziprail_tune_max_jumpoff", "550", FCVAR_RELEASE,
	"Override player maxJumpOffSpeed. S21 default is 550; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_tune_min_jumpoff("sdk_ziprail_tune_min_jumpoff", "100", FCVAR_RELEASE,
	"Override player minJumpOffSpeed. S21 default is 100; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_tune_view_influence("sdk_ziprail_tune_view_influence", "0.5", FCVAR_RELEASE,
	"Override player viewInfluenceOnJumpDirection. S21 default is 0.5; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_tune_detach_dir_max_down("sdk_ziprail_tune_detach_dir_max_down", "10", FCVAR_RELEASE,
	"Override player detachDirMaxDownAngle (degrees). S21 default is 10; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_tune_detach_dir_max_up("sdk_ziprail_tune_detach_dir_max_up", "26", FCVAR_RELEASE,
	"Override player detachDirMaxUpAngle (degrees). S21 default is 26; -1 falls back to the player settings layout.");
static ConVar sdk_ziprail_duck_detach("sdk_ziprail_duck_detach", "0", FCVAR_RELEASE,
	"When non-zero, a ducked rail jump-off clamps speed to minJumpOffSpeed.");

//-----------------------------------------------------------------------------
// Counters / one-shot warn flags.
//-----------------------------------------------------------------------------
static uint64_t s_nVerticalApps = 0;
static uint64_t s_nRopeApps     = 0;
static uint64_t s_nRailExitApps = 0;
static uint64_t s_nRailExitDegenerateWarns = 0;
static uint64_t s_nZeroSegWarns = 0;
static uint64_t s_nRailJumpoffApps = 0;
static uint64_t s_nLogEvents    = 0;
static uint64_t s_nMountUseLogs = 0;
static uint64_t s_nMountFindLogs = 0;
static uint64_t s_nMountInjectLogs = 0;
static uint64_t s_nMountAlphaLogs = 0;

// Z the engine's collector adds to the player origin before searching.
static constexpr float ZE_MOUNT_SEARCH_EYE_RAISE = 72.0f;

// Zipline_Find runs about five times a second while a player is alive, so this
// puts the nearest-rail report a few seconds apart.
static constexpr uint64_t kMountFindMissReportEvery = 16;
static uint64_t s_nMountFindMisses = 0;
static uint64_t s_nMountUseDispatchLogs = 0;
static uint64_t s_nBadCountWarns = 0;
static uint64_t s_nSettingsWarns = 0;

// Armed only across the original Zipline_CheckAutoDetach call, so the eye
// substitution can never reach an unrelated EyePosition caller.
static void* s_pDetachRefPlayer = nullptr;
static uint64_t s_nDetachRefSubst = 0;
static uint64_t s_nDetachRefLogs = 0;
static uint64_t s_nDetachRefWarns = 0;

static uint64_t s_nAlphaLogEvents = 0;
static uint64_t s_nGeomLogEvents = 0;
static uint64_t s_nAlphaNullFarWarns = 0;
static uint64_t s_nAlphaBadMaxWarns = 0;
static uint64_t s_nAlphaNullHelperWarns = 0;
static uint64_t s_nAlphaNullRefWarns = 0;

static uint64_t s_nFrontEvals  = 0;
static uint64_t s_nFrontFired  = 0;
static uint64_t s_nFrontBadCountWarns = 0;
static bool s_bWarnedDetachSpeedField = false;
static bool s_bWarnedMaxVertField = false;
static bool s_bWarnedJumpHeightField = false;
static bool s_bWarnedNearVerticalLook = false;
static bool s_bWarnedSvGravity = false;

static uint32_t s_nDetachSpeedFieldOff = 0xFFFFFFFFu;
static uint32_t s_nMaxVertDetachFieldOff = 0xFFFFFFFFu;
static uint32_t s_nJumpHeightFieldOff = 0xFFFFFFFFu;
static bool s_bDetachSpeedFieldResolved = false;
static bool s_bMaxVertDetachFieldResolved = false;
static bool s_bJumpHeightFieldResolved = false;

//-----------------------------------------------------------------------------
// Ziprail ride settings from the loaded asset (no CZiprail class in this binary).
//-----------------------------------------------------------------------------
struct ZiplineSettingsField_t
{
	const char* pszName;
	uint32_t    nOffset;
	bool        bResolved;
	bool        bWarned;
};

static ZiplineSettingsField_t s_railAccel      = { "ziprailAcceleration",     0xFFFFFFFFu, false, false };
static ZiplineSettingsField_t s_railGravAccel  = { "pathGravityAcceleration", 0xFFFFFFFFu, false, false };
static ZiplineSettingsField_t s_railMaxSpeedMul = { "maxSpeedMultiplier",     0xFFFFFFFFu, false, false };
static ZiplineSettingsField_t s_railMinSpeedFrac = { "minSpeedFraction",      0xFFFFFFFFu, false, false };
static ZiplineSettingsField_t s_railDetachSpeed = { "ziprailDetachSpeed",     0xFFFFFFFFu, false, false };
static ZiplineSettingsField_t s_railMaxJumpOff  = { "maxJumpOffSpeed",        0xFFFFFFFFu, false, false };
static ZiplineSettingsField_t s_railMinJumpOff  = { "minJumpOffSpeed",        0xFFFFFFFFu, false, false };
static ZiplineSettingsField_t s_railViewInfl    = { "viewInfluenceOnJumpDirection", 0xFFFFFFFFu, false, false };
static ZiplineSettingsField_t s_railDetachDown  = { "detachDirMaxDownAngle",  0xFFFFFFFFu, false, false };
static ZiplineSettingsField_t s_railDetachUp    = { "detachDirMaxUpAngle",    0xFFFFFFFFu, false, false };

static uint64_t s_nRailTicks = 0;
static uint64_t s_nRailLogEvents = 0;
static unsigned s_nRailUnavailWarnMask = 0xFFFFFFFFu;
static unsigned s_nAlphaGuardWarnMask = 0xFFFFFFFFu;

// The rewrite is permanent, so the per-exit line is debug-only. The first few
// still print unprompted: a silent rewrite is indistinguishable from a hook that
// never attached.
static bool ZiplineExit_ShouldLog(void)
{
	++s_nLogEvents;
	return bridge_zip_exit_debug.GetBool() || s_nLogEvents <= 4;
}

static bool ZiplineExit_ShouldWarnSample(uint64_t& nCounter)
{
	++nCounter;
	return bridge_zip_exit_debug.GetBool()
		|| nCounter <= 64
		|| (nCounter % 16) == 0;
}

static bool ZiplineAlpha_ShouldLog(void)
{
	++s_nAlphaLogEvents;
	return bridge_zip_alpha_diag.GetBool() || s_nAlphaLogEvents <= 4;
}

static bool ZiplineAlpha_ShouldWarnSample(uint64_t& nCounter)
{
	++nCounter;
	return bridge_zip_alpha_diag.GetBool()
		|| nCounter <= 64
		|| (nCounter % 16) == 0;
}

static void ZiplineAlpha_ReportGuard(unsigned nMask, int nPoints)
{
	if (nMask == 0 || nMask == s_nAlphaGuardWarnMask)
		return;

	s_nAlphaGuardWarnMask = nMask;
	Warning(eDLL_T::SERVER,
		"[ZIP-ALPHA] guard blocked mask=0x%03X nPoints=%d "
		"(exitParity=%d detachParity=%d ctxPlayer=%d outAlpha=%d outPos=%d "
		"outVel=%d points=%d dists=%d nPtsLo=%d nPtsHi=%d ropeLen=%d) "
		"-- alpha-ceiling parity skipped for this ride\n",
		nMask, nPoints,
		(nMask & 0x001u) ? 0 : 1,
		(nMask & 0x002u) ? 0 : 1,
		(nMask & 0x004u) ? 0 : 1,
		(nMask & 0x008u) ? 0 : 1,
		(nMask & 0x010u) ? 0 : 1,
		(nMask & 0x020u) ? 0 : 1,
		(nMask & 0x040u) ? 0 : 1,
		(nMask & 0x080u) ? 0 : 1,
		(nMask & 0x100u) ? 0 : 1,
		(nMask & 0x200u) ? 0 : 1,
		(nMask & 0x400u) ? 0 : 1);
}

static bool ZiplineGeom_ShouldLog(void)
{
	++s_nGeomLogEvents;
	return bridge_zip_geom_diag.GetBool() || s_nGeomLogEvents <= 4;
}

static bool ZiplineRail_ShouldLog(void)
{
	++s_nRailLogEvents;
	return bridge_zip_rail_diag.GetBool() || s_nRailLogEvents <= 4;
}

static bool ZiprailMount_ShouldLog(uint64_t& nCounter)
{
	++nCounter;
	return bridge_zip_mount_diag.GetBool() || nCounter <= 4;
}

// Single-call [ZIP-GEOM] line -- hard-bounded, no allocation.
static char s_geomBuf[8192];

static bool ZiplineGeom_BufAppend(char* buf, size_t cap, size_t* pUsed, const char* fmt, ...)
{
	if (!buf || !pUsed || !fmt || *pUsed >= cap)
		return false;

	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(buf + *pUsed, cap - *pUsed, fmt, ap);
	va_end(ap);

	if (n < 0 || static_cast<size_t>(n) >= cap - *pUsed)
	{
		buf[cap - 1] = '\0';
		*pUsed = cap - 1;
		return false;
	}

	*pUsed += static_cast<size_t>(n);
	return true;
}

static float ZiplineExit_Dot3(const float* a, const float* b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float ZiplineExit_PointDistance(const float* pPoints, int iA, int iB)
{
	const float flDx = pPoints[iB * 3 + 0] - pPoints[iA * 3 + 0];
	const float flDy = pPoints[iB * 3 + 1] - pPoints[iA * 3 + 1];
	const float flDz = pPoints[iB * 3 + 2] - pPoints[iA * 3 + 2];
	return sqrtf(flDx * flDx + flDy * flDy + flDz * flDz);
}

static float ZiplineRope_Segment(const float* pPoints, const float* pDistances, int iSeg)
{
	// A negative published distance means "measure the segment".
	const float flSeg = pDistances[iSeg];
	if (flSeg == 0.0f)
	{
		const float flChord = ZiplineExit_PointDistance(pPoints, iSeg, iSeg + 1);
		if (flChord > 0.0f && ZiplineExit_ShouldWarnSample(s_nZeroSegWarns))
		{
			Warning(eDLL_T::SERVER,
				"[ZIP-EXIT] published segment distance is 0.0f at iSeg=%d "
				"but endpoints are not coincident (chord=%.3f)\n",
				iSeg, flChord);
		}
	}
	return flSeg < 0.0f ? ZiplineExit_PointDistance(pPoints, iSeg, iSeg + 1) : flSeg;
}

static float ZiplineRope_Length(const float* pPoints, const float* pDistances, int nCount)
{
	float flTotal = 0.0f;
	for (int i = 0; i < nCount - 1; ++i)
		flTotal += ZiplineRope_Segment(pPoints, pDistances, i);

	return flTotal;
}

// Locate the arc distance flArc along the ride polyline, the same walk the engine
// runs: step segments until one contains the remainder, and past the last point
// collapse onto the final pair.
static void ZiplineRope_Locate(const float* pPoints, const float* pDistances, int nCount,
	float flArc, int* pOutLo, int* pOutHi, float* pOutPos)
{
	float flRemain = flArc;
	float flSeg = 0.0f;
	int   iLo = 0;
	int   nStep = 1;
	bool  bAtEnd = false;

	for (;;)
	{
		flSeg = ZiplineRope_Segment(pPoints, pDistances, iLo);
		if (flSeg >= flRemain)
			break;

		flRemain -= flSeg;
		++iLo;
		++nStep;

		if (nStep >= nCount)
		{
			bAtEnd = true;
			break;
		}
	}

	if (bAtEnd)
	{
		*pOutLo = nCount - 2;
		*pOutHi = nCount - 1;

		if (pOutPos)
		{
			for (int i = 0; i < 3; ++i)
				pOutPos[i] = pPoints[(nCount - 1) * 3 + i];
		}
		return;
	}

	*pOutLo = iLo;
	*pOutHi = iLo + 1;

	if (!pOutPos)
		return;

	if (flSeg <= 0.0f)
	{
		for (int i = 0; i < 3; ++i)
			pOutPos[i] = pPoints[iLo * 3 + i];
		return;
	}

	const float flT = flRemain / flSeg;
	for (int i = 0; i < 3; ++i)
	{
		pOutPos[i] = pPoints[iLo * 3 + i]
			+ (pPoints[(iLo + 1) * 3 + i] - pPoints[iLo * 3 + i]) * flT;
	}
}

static void ZiplineRope_Tangent(const float* pPoints, int iLo, int iHi, float outDir[3])
{
	for (int i = 0; i < 3; ++i)
		outDir[i] = pPoints[iHi * 3 + i] - pPoints[iLo * 3 + i];

	// FLT_MIN mirrors the engine's own degenerate-length guard.
	const float flInv = 1.0f / fmaxf(sqrtf(ZiplineExit_Dot3(outDir, outDir)), ZE_SQRT_FLT_MIN);
	for (int i = 0; i < 3; ++i)
		outDir[i] *= flInv;
}

// Same raw g_serverEntityList lookup the other server-side resolvers use.
static void* ZiplineExit_ResolveHandle(const unsigned nHandle)
{
	if (nHandle == INVALID_EHANDLE_INDEX || !g_serverEntityList)
		return nullptr;

	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(nHandle));
	return g_serverEntityList->LookupEntity(handle);
}

static bool ZiplineExit_CallVtblBool(void* pEnt, ptrdiff_t nVtblByteOff)
{
	if (!pEnt)
		return false;

	void** const pVtbl = *reinterpret_cast<void***>(pEnt);
	if (!pVtbl)
		return false;

	auto pfn = *reinterpret_cast<bool(__fastcall**)(void*)>(
		reinterpret_cast<uint8_t*>(pVtbl) + nVtblByteOff);
	if (!pfn)
		return false;

	return pfn(pEnt);
}

static bool ZiplineExit_IsZipline(void* zip)
{
	return ZiplineExit_CallVtblBool(zip, ZE_ZIP_VTBL_ISZIPLINE);
}

// Read a float from the player's settings block at a known data offset.
static bool ZiplineExit_ReadSettingsFloat(void* player, uint32_t nOff, float* outValue)
{
	if (!player || !outValue || nOff == 0xFFFFFFFFu)
		return false;

	const uint64_t settings = *reinterpret_cast<const uint64_t*>(
		reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_SETTINGS);
	if (!settings)
		return false;

	const float flValue = *reinterpret_cast<const float*>(settings + nOff);
	if (!isfinite(flValue) || !(flValue > 0.0f) || !(flValue < 100000.0f))
		return false;

	*outValue = flValue;
	return true;
}

// Player ziplineSpeed setting via the runtime-filled field offset.
static bool ZiplineExit_ReadZiplineSpeed(void* player, float* outSpeed)
{
	if (!g_pZiplineSpeedFieldOffset)
		return false;

	return ZiplineExit_ReadSettingsFloat(player, *g_pZiplineSpeedFieldOffset, outSpeed);
}

// Lazy name-resolved player-settings float. Caches a hit; retries a miss so a
// call before the layout loads can still succeed later.
static bool ZiplineExit_ReadNamedSettingsFloat(void* player, const char* pszFieldName,
	uint32_t* pCachedOff, bool* pResolved, bool* pWarned, float* outValue)
{
	if (!player || !pszFieldName || !pCachedOff || !pResolved || !pWarned || !outValue)
		return false;

	if (!*pResolved)
	{
		const uint32_t nOff = Bridge_LookupPlayerSettingsFieldOffset(pszFieldName);
		if (nOff == 0xFFFFFFFFu)
		{
			// Before the layout loads, Lookup returns 0xFFFFFFFF without caching
			// that failure -- retry quietly. Once the layout is present, a miss
			// is permanent.
			if (Bridge_HasPlayerSettingsLayout())
			{
				*pCachedOff = 0xFFFFFFFFu;
				*pResolved = true;
				if (!*pWarned)
				{
					*pWarned = true;
					Warning(eDLL_T::SERVER,
						"[ZIP-EXIT] settings field '%s' not found in player layout -- "
						"that exit rewrite branch is skipped\n",
						pszFieldName);
				}
			}
			return false;
		}
		*pCachedOff = nOff;
		*pResolved = true;
	}

	if (*pCachedOff == 0xFFFFFFFFu)
		return false;

	if (!ZiplineExit_ReadSettingsFloat(player, *pCachedOff, outValue))
	{
		if (!*pWarned)
		{
			*pWarned = true;
			Warning(eDLL_T::SERVER,
				"[ZIP-EXIT] settings field '%s' unreadable or bad value "
				"(settings null / non-finite / out of range) -- that exit rewrite "
				"branch is skipped\n",
				pszFieldName);
		}
		return false;
	}

	return true;
}

// Priority: lever > 0, else player-layout field, else unavailable (one-shot warn).
static bool ZiprailTune_Read(void* player, ZiplineSettingsField_t* pField,
	const ConVar& lever, float* outValue)
{
	if (!player || !pField || !outValue)
		return false;

	const float flLever = lever.GetFloat();
	if (flLever > 0.0f)
	{
		*outValue = flLever;
		return true;
	}

	if (!pField->bResolved)
	{
		const uint32_t nOff = Bridge_LookupPlayerSettingsFieldOffset(pField->pszName);
		if (nOff != 0xFFFFFFFFu)
		{
			pField->nOffset = nOff;
			pField->bResolved = true;
		}
		else if (Bridge_HasPlayerSettingsLayout())
		{
			pField->nOffset = 0xFFFFFFFFu;
			pField->bResolved = true;
		}
	}

	if (pField->bResolved && pField->nOffset != 0xFFFFFFFFu
		&& ZiplineExit_ReadSettingsFloat(player, pField->nOffset, outValue))
		return true;

	if (!pField->bWarned
		&& (pField->bResolved || Bridge_HasPlayerSettingsLayout()))
	{
		pField->bWarned = true;
		Warning(eDLL_T::SERVER,
			"[ZIP-RAIL] settings field '%s' unavailable -- set %s > 0 "
			"(or ensure the player layout carries the field)\n",
			pField->pszName, lever.GetName());
	}
	return false;
}

// True when a further segment exists (entity two links along). Chooses handoff over auto-detach.
static bool ZiplineExit_HasFurtherSegment(void* pActive, const bool bReverse)
{
	if (!pActive)
		return false;

	if (bReverse)
		return v_Zipline_GetPrevEntity && v_Zipline_GetPrevEntity(pActive) != nullptr;

	if (!v_Zipline_GetNextEntity)
		return false;

	void* const pNext = v_Zipline_GetNextEntity(pActive);
	return pNext && v_Zipline_GetNextEntity(pNext) != nullptr;
}

static bool ZiplineExit_IsPlayer(void* pEntity)
{
	if (!pEntity)
		return false;

	void** const pVtbl = *reinterpret_cast<void***>(pEntity);
	if (!pVtbl)
		return false;

	using PFN_IsPlayer = bool (__fastcall*)(void* pThis);
	const PFN_IsPlayer pfn =
		reinterpret_cast<PFN_IsPlayer>(pVtbl[ZE_VTBL_SLOT_ISPLAYER]);
	return pfn ? pfn(pEntity) : false;
}

static bool ZiplineExit_WorldSpaceCenter(void* pEntity, float outCenter[3])
{
	if (!pEntity || !outCenter)
		return false;

	CBaseEntity* const pBase = reinterpret_cast<CBaseEntity*>(pEntity);
	CCollisionProperty* const pColl = pBase->CollisionProp();
	if (!pColl)
		return false;

	Vector3D center;
	CM_WorldSpaceCenter(pColl, &center);
	outCenter[0] = center.x;
	outCenter[1] = center.y;
	outCenter[2] = center.z;
	return true;
}

// True when another living, same-realm player is riding ahead on this rope
// within 100 units and in the direction of travel.
static bool ZiplineExit_HasDetectedPlayerInFront(void* player, const float* pVelocity)
{
	if (!player || !pVelocity)
		return false;

	if (!v_Zipline_GetNextEntity)
		return false;

	const uint64_t nEval = ++s_nFrontEvals;
	const bool bLog = bridge_zip_front_diag.GetBool() || nEval <= 4;

	float self[3];
	if (!ZiplineExit_WorldSpaceCenter(player, self))
	{
		if (bLog)
		{
			Msg(eDLL_T::SERVER,
				"[ZIP-FRONT] eval=%llu fire=0 reason=no_self_center fired=%llu total=%llu\n",
				nEval, s_nFrontFired, nEval);
		}
		return false;
	}

	const unsigned nSelfHandle = *reinterpret_cast<const unsigned*>(
		reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ACTIVEZIPLINE);
	void* const zip = ZiplineExit_ResolveHandle(nSelfHandle);
	if (!zip)
	{
		if (bLog)
		{
			Msg(eDLL_T::SERVER,
				"[ZIP-FRONT] eval=%llu fire=0 reason=no_active_zip fired=%llu total=%llu\n",
				nEval, s_nFrontFired, nEval);
		}
		return false;
	}

	const int nType = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(zip) + ZE_ZIP_OFF_TYPE);
	if (nType == ZE_ZIP_TYPE_VERTICAL)
	{
		if (bLog)
		{
			Msg(eDLL_T::SERVER,
				"[ZIP-FRONT] eval=%llu fire=0 reason=vertical_rope fired=%llu total=%llu\n",
				nEval, s_nFrontFired, nEval);
		}
		return false;
	}

	const int nCount = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(zip) + ZE_ZIP_OFF_ATTACHED_COUNT);
	if (nCount < 0 || nCount > ZE_MAX_ATTACHED)
	{
		if (ZiplineExit_ShouldWarnSample(s_nFrontBadCountWarns))
		{
			Warning(eDLL_T::SERVER,
				"[ZIP-FRONT] attached count=%d outside [0,%d] -- player-in-front skipped\n",
				nCount, ZE_MAX_ATTACHED);
		}
		return false;
	}

	if (nCount <= 1)
	{
		if (bLog)
		{
			Msg(eDLL_T::SERVER,
				"[ZIP-FRONT] eval=%llu fire=0 reason=count_le1 count=%d fired=%llu total=%llu\n",
				nEval, nCount, s_nFrontFired, nEval);
		}
		return false;
	}

	const int64_t nSelfRealms = *reinterpret_cast<const int64_t*>(
		reinterpret_cast<const uint8_t*>(player) + ZE_ENT_OFF_REALMSBITMASK);
	const float flNow = TriggerPass_MovementTime();

	int nRejNull = 0;
	int nRejNotPlayer = 0;
	int nRejSelf = 0;
	int nRejRealms = 0;
	int nRejNoZip = 0;
	int nRejNotRiding = 0;
	int nRejLife = 0;
	int nRejPhase = 0;
	int nRejDist = 0;
	int nRejDot = 0;

	const uint8_t* const pAttachBase =
		reinterpret_cast<const uint8_t*>(zip) + ZE_ZIP_OFF_ATTACHED_ENTS;

	for (int i = 0; i < nCount; ++i)
	{
		const unsigned nOtherHandle =
			*reinterpret_cast<const unsigned*>(pAttachBase + static_cast<size_t>(i) * 32);
		void* const other = ZiplineExit_ResolveHandle(nOtherHandle);
		if (!other)
		{
			++nRejNull;
			continue;
		}
		if (!ZiplineExit_IsPlayer(other))
		{
			++nRejNotPlayer;
			continue;
		}
		if (other == player)
		{
			++nRejSelf;
			continue;
		}

		const int64_t nOtherRealms = *reinterpret_cast<const int64_t*>(
			reinterpret_cast<const uint8_t*>(other) + ZE_ENT_OFF_REALMSBITMASK);
		if ((nOtherRealms & nSelfRealms) == 0)
		{
			++nRejRealms;
			continue;
		}

		const unsigned nOtherZipHandle = *reinterpret_cast<const unsigned*>(
			reinterpret_cast<const uint8_t*>(other) + ZE_PLAYER_OFF_ACTIVEZIPLINE);
		void* const otherZip = ZiplineExit_ResolveHandle(nOtherZipHandle);
		if (!otherZip)
		{
			++nRejNoZip;
			continue;
		}
		if (!v_Zipline_GetNextEntity(otherZip))
		{
			++nRejNotRiding;
			continue;
		}

		const char lifeState = *reinterpret_cast<const char*>(
			reinterpret_cast<const uint8_t*>(other) + ZE_ENT_OFF_LIFESTATE);
		if (lifeState != 0)
		{
			++nRejLife;
			continue;
		}

		const float flPhaseStart = *reinterpret_cast<const float*>(
			reinterpret_cast<const uint8_t*>(other) + ZE_PLAYER_OFF_PHASE_START);
		const float flPhaseEnd = *reinterpret_cast<const float*>(
			reinterpret_cast<const uint8_t*>(other) + ZE_PLAYER_OFF_PHASE_END);
		if (isfinite(flPhaseStart) && isfinite(flPhaseEnd)
			&& flNow >= flPhaseStart && flNow <= flPhaseEnd)
		{
			++nRejPhase;
			continue;
		}

		float o[3];
		if (!ZiplineExit_WorldSpaceCenter(other, o))
		{
			++nRejNull;
			continue;
		}

		const float flDx = o[0] - self[0];
		const float flDy = o[1] - self[1];
		const float flDz = o[2] - self[2];
		const float flDistSqr = flDx * flDx + flDy * flDy + flDz * flDz;
		// Client continues into the dot test when distSqr <= 10000.
		if (flDistSqr > 10000.0f)
		{
			++nRejDist;
			continue;
		}

		const float d[3] = { flDx, flDy, flDz };
		const float flInv = 1.0f / fmaxf(sqrtf(ZiplineExit_Dot3(d, d)), ZE_SQRT_FLT_MIN);
		const float flDot =
			(d[0] * flInv) * pVelocity[0]
			+ (d[1] * flInv) * pVelocity[1]
			+ (d[2] * flInv) * pVelocity[2];
		if (flDot > 0.0f)
		{
			++s_nFrontFired;
			if (bLog)
			{
				Msg(eDLL_T::SERVER,
					"[ZIP-FRONT] eval=%llu fire=1 count=%d dist=%.3f dot=%.4f "
					"rej(null=%d notPlr=%d self=%d realms=%d noZip=%d notRide=%d "
					"life=%d phase=%d dist=%d dot=%d) fired=%llu total=%llu\n",
					nEval, nCount, sqrtf(flDistSqr), flDot,
					nRejNull, nRejNotPlayer, nRejSelf, nRejRealms, nRejNoZip,
					nRejNotRiding, nRejLife, nRejPhase, nRejDist, nRejDot,
					s_nFrontFired, nEval);
			}
			return true;
		}
		++nRejDot;
	}

	if (bLog)
	{
		Msg(eDLL_T::SERVER,
			"[ZIP-FRONT] eval=%llu fire=0 count=%d "
			"rej(null=%d notPlr=%d self=%d realms=%d noZip=%d notRide=%d "
			"life=%d phase=%d dist=%d dot=%d) fired=%llu total=%llu\n",
			nEval, nCount,
			nRejNull, nRejNotPlayer, nRejSelf, nRejRealms, nRejNoZip,
			nRejNotRiding, nRejLife, nRejPhase, nRejDist, nRejDot,
			s_nFrontFired, nEval);
	}

	return false;
}

//-----------------------------------------------------------------------------
// Ride-pass wrappers -- publish the current player for MoveAlongRope /
// CheckAutoDetach, then restore so nested or later calls see a clean context.
//-----------------------------------------------------------------------------
static char __fastcall Hook_Zipline_MoveUpdateSlide(void* player, float* position, float* velocity,
	const void* pAbsViewAngles, float flForwardMove, float flSideMove)
{
	const ZiplineRideCtx_t prev = s_rideCtx;
	s_rideCtx = {};
	s_rideCtx.pPlayer = player;
	s_rideCtx.pMovePosition = position;
	s_rideCtx.bSlide = true;

	const char nResult = v_Zipline_MoveUpdateSlide(player, position, velocity,
		pAbsViewAngles, flForwardMove, flSideMove);

	s_rideCtx = prev;
	return nResult;
}

static char __fastcall Hook_Zipline_MoveUpdateMount(void* player, float* position, float* velocity,
	const void* pAbsViewAngles, float flForwardMove, float flSideMove)
{
	const ZiplineRideCtx_t prev = s_rideCtx;
	s_rideCtx = {};
	s_rideCtx.pPlayer = player;
	s_rideCtx.pMovePosition = nullptr;
	s_rideCtx.bSlide = false;

	const char nResult = v_Zipline_MoveUpdateMount(player, position, velocity,
		pAbsViewAngles, flForwardMove, flSideMove);

	s_rideCtx = prev;
	return nResult;
}

//-----------------------------------------------------------------------------
// Reproduce the client's ziprail speed step. pDirDot is the along-path direction
// (negated when alphaIn > alphaCeiling); pDirGrav is the raw path direction
// (negated only when reverse). Two separate sign conventions.
static float ZiplineRail_Speed(const float* pDirDot, const float* pDirGrav,
	const float* pPlayerVelocity, float flZiplineSpeed, float flFrameTime,
	float flRailAccel, float flGravAccel, float flMaxSpeedMul, float flMinSpeedFrac)
{
	const float flAlong = fmaxf(0.0099999998f,
		ZiplineExit_Dot3(pDirDot, pPlayerVelocity));
	const float flSlope = -pDirGrav[2] * flGravAccel;

	if (flZiplineSpeed <= flAlong)
	{
		return fminf(flFrameTime * flSlope + flAlong,
			flZiplineSpeed * flMaxSpeedMul);
	}

	float flAccel = flSlope + flRailAccel;
	if (flZiplineSpeed * flMinSpeedFrac > flAlong)
		flAccel = fmaxf(flAccel, flRailAccel);

	return flFrameTime * flAccel + flAlong;
}

static float ZiplineRail_MinProbeDist2(const float node[3], const float probes[3][3])
{
	float flMin = FLT_MAX;
	for (int k = 0; k < 3; ++k)
	{
		const float dx = node[0] - probes[k][0];
		const float dy = node[1] - probes[k][1];
		const float dz = node[2] - probes[k][2];
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 < flMin)
			flMin = d2;
	}
	return flMin;
}

// This engine has no m_ziplineMountReverseDistance; the S21 client falls back to
// the entity's auto-detach distance when that field is zero, so this IS the
// reverse radius.
static bool ZiplineRail_ShouldReverse(void* player, void* zip,
	const ZiprailWireBlock* pWire, const float pathDir[3],
	const float closest[3], const float* eyeDirection)
{
	if (!player || !zip || !pWire || !pathDir || !closest || !eyeDirection)
		return false;
	if (pWire->numNodes < 2)
		return false;

	const float flDot = ZiplineExit_Dot3(eyeDirection, pathDir);
	const float flHullHeight = v_Zipline_GetEntityHullHeight
		? v_Zipline_GetEntityHullHeight(player) : 0.0f;
	// Auto-detach on a rail start is often 0 (clamped to 0.01); that is not
	// the reverse radius. Authored ziprailMountReverseDistance is 200.
	float flRevDist = pWire->mountReverseDistance;
	if (!(flRevDist > 0.0f))
		flRevDist = 200.0f;
	const float flRevDist2 = flRevDist * flRevDist;

	float probes[3][3];
	probes[0][0] = closest[0]; probes[0][1] = closest[1]; probes[0][2] = closest[2];
	probes[1][0] = closest[0]; probes[1][1] = closest[1];
	probes[1][2] = closest[2] + flHullHeight;
	probes[2][0] = closest[0]; probes[2][1] = closest[1];
	probes[2][2] = closest[2] - flHullHeight;

	if (flDot >= 0.0f)
	{
		void* pNext = (v_Zipline_GetNextEntity) ? v_Zipline_GetNextEntity(zip) : nullptr;
		void* pNext2 = (pNext && v_Zipline_GetNextEntity)
			? v_Zipline_GetNextEntity(pNext) : nullptr;
		if (!pNext2)
		{
			const float* const pLast = pWire->positions[pWire->numNodes - 1];
			return flRevDist2 >= ZiplineRail_MinProbeDist2(pLast, probes);
		}
	}
	else
	{
		const bool bPrevInvalid = !v_Zipline_GetPrevEntity
			|| v_Zipline_GetPrevEntity(zip) == nullptr;
		if (bPrevInvalid)
		{
			const float* const pFirst = pWire->positions[0];
			if (flRevDist2 >= ZiplineRail_MinProbeDist2(pFirst, probes))
				return false;
		}
	}

	return flDot < 0.0f;
}

//-----------------------------------------------------------------------------
// Pre-scale speed when ahead (slide); post-orig rewrite alpha/pos/vel. No m_ziplinePreventManualDetach here.
//-----------------------------------------------------------------------------
static float* __fastcall Hook_Zipline_MoveAlongRope(char bReverse, char bTypeOne,
	float flAcceleration, float flZiplineSpeed,
	const float* pPoints, const float* pDistances, int nPoints,
	float flAlphaIn, const float* pPlayerVelocity,
	float* pOutPosition, float* pOutVelocity, float* pOutAlpha)
{
	// Client applies this in the slide pass only; mount also calls this hook.
	if (s_rideCtx.bSlide && s_rideCtx.pPlayer && pPlayerVelocity
		&& (bridge_zip_player_in_front.GetBool()
			|| bridge_zip_front_diag.GetBool()))
	{
		const bool bFront = ZiplineExit_HasDetectedPlayerInFront(
			s_rideCtx.pPlayer, pPlayerVelocity);
		if (bFront && bridge_zip_player_in_front.GetBool())
		{
			flZiplineSpeed *= 0.25f;
			flAcceleration *= 0.5f;
		}
	}

	// Authored m_ziplinePreventManualDetach also scales accel (client slide).
	if (s_rideCtx.pPlayer)
	{
		const unsigned nZipHandle = *reinterpret_cast<const unsigned*>(
			reinterpret_cast<const uint8_t*>(s_rideCtx.pPlayer)
			+ ZE_PLAYER_OFF_ACTIVEZIPLINE);
		void* const pZipScale = ZiplineExit_ResolveHandle(nZipHandle);
		const ZiprailWireBlock* const pWireScale = pZipScale
			? ZiprailDedi_GetWireBlock(reinterpret_cast<uintptr_t>(pZipScale))
			: nullptr;
		if (pWireScale && pWireScale->preventManualDetach
			&& pWireScale->speedScale > 0.0f)
		{
			flAcceleration *= pWireScale->speedScale;
		}
	}

	// The scaled speed is what the ziprail model caps against, so keep it.
	const float flRideSpeed = flZiplineSpeed;

	float* const pResult = v_Zipline_MoveAlongRope(bReverse, bTypeOne,
		flAcceleration, flZiplineSpeed, pPoints, pDistances, nPoints,
		flAlphaIn, pPlayerVelocity, pOutPosition, pOutVelocity, pOutAlpha);

	// [ZIP-GEOM] observe-only; needs a usable polyline, not the parity path.
	if (pPoints && pDistances && nPoints >= 2 && nPoints <= ZE_MAX_POINTS
		&& ZiplineGeom_ShouldLog())
	{
		const float flGeomTotal = ZiplineRope_Length(pPoints, pDistances, nPoints);
		if (flGeomTotal > 0.0f)
		{
			void* pGeomZip = nullptr;
			if (s_rideCtx.pPlayer)
			{
				const unsigned nGeomHandle = *reinterpret_cast<const unsigned*>(
					reinterpret_cast<const uint8_t*>(s_rideCtx.pPlayer)
					+ ZE_PLAYER_OFF_ACTIVEZIPLINE);
				pGeomZip = ZiplineExit_ResolveHandle(nGeomHandle);
			}

			int nNumNodes = -1;
			float flSpringDist = -1.0f;
			float flSpringScale = -1.0f;
			float flRemainUnsim = -1.0f;
			int nNumRestPos = -1;
			int nNumZipPts = -1;
			int nNumAttached = -1;
			int nZipType = -1;

			if (pGeomZip)
			{
				const uint8_t* const pZip = reinterpret_cast<const uint8_t*>(pGeomZip);
				nNumNodes     = *reinterpret_cast<const int*>(pZip + ZE_ZIP_OFF_NUM_NODES);
				flSpringDist  = *reinterpret_cast<const float*>(pZip + ZE_ZIP_OFF_SPRING_DIST);
				flSpringScale = *reinterpret_cast<const float*>(pZip + ZE_ZIP_OFF_SPRING_SCALE);
				flRemainUnsim = *reinterpret_cast<const float*>(pZip + ZE_ZIP_OFF_REMAIN_UNSIM);
				nNumRestPos   = *reinterpret_cast<const int*>(pZip + ZE_ZIP_OFF_NUM_REST_POS);
				nNumZipPts    = *reinterpret_cast<const int*>(pZip + ZE_ZIP_OFF_NUM_ZIP_POINTS);
				nNumAttached  = *reinterpret_cast<const int*>(pZip + ZE_ZIP_OFF_ATTACHED_COUNT);
				nZipType      = *reinterpret_cast<const int*>(pZip + ZE_ZIP_OFF_TYPE);
			}

			size_t nUsed = 0;
			s_geomBuf[0] = '\0';
			ZiplineGeom_BufAppend(s_geomBuf, sizeof(s_geomBuf), &nUsed,
				"[ZIP-GEOM] pass=%s nPts=%d total=%.4f alphaIn=%.6f "
				"numNodes=%d springDist=%.4f springScale=%.4f remainUnsim=%.4f "
				"numRestPos=%d numZipPts=%d numAttached=%d zipType=%d pts=",
				s_rideCtx.bSlide ? "slide" : "mount",
				nPoints, flGeomTotal, flAlphaIn,
				nNumNodes, flSpringDist, flSpringScale, flRemainUnsim,
				nNumRestPos, nNumZipPts, nNumAttached, nZipType);

			for (int i = 0; i < nPoints; ++i)
			{
				if (!ZiplineGeom_BufAppend(s_geomBuf, sizeof(s_geomBuf), &nUsed,
					" (%.4f %.4f %.4f)",
					pPoints[i * 3 + 0], pPoints[i * 3 + 1], pPoints[i * 3 + 2]))
					break;
			}

			ZiplineGeom_BufAppend(s_geomBuf, sizeof(s_geomBuf), &nUsed, " dists=");
			for (int i = 0; i < nPoints - 1; ++i)
			{
				if (!ZiplineGeom_BufAppend(s_geomBuf, sizeof(s_geomBuf), &nUsed,
					" %.4f", pDistances[i]))
					break;
			}

			ZiplineGeom_BufAppend(s_geomBuf, sizeof(s_geomBuf), &nUsed, "\n");
			Msg(eDLL_T::SERVER, "%s", s_geomBuf);
		}
	}

	unsigned nGuardMask = 0;
	if (!bridge_zip_exit_parity.GetBool())
		nGuardMask |= 0x001u;
	if (!bridge_zip_detach_alpha_parity.GetBool())
		nGuardMask |= 0x002u;
	if (!s_rideCtx.pPlayer)
		nGuardMask |= 0x004u;
	if (!pOutAlpha)
		nGuardMask |= 0x008u;
	if (!pOutPosition)
		nGuardMask |= 0x010u;
	if (!pOutVelocity)
		nGuardMask |= 0x020u;
	if (!pPoints)
		nGuardMask |= 0x040u;
	if (!pDistances)
		nGuardMask |= 0x080u;
	if (nPoints < 2)
		nGuardMask |= 0x100u;
	if (nPoints > ZE_MAX_POINTS)
		nGuardMask |= 0x200u;
	if (nGuardMask)
	{
		ZiplineAlpha_ReportGuard(nGuardMask, nPoints);
		return pResult;
	}

	void* const player = s_rideCtx.pPlayer;
	const int nCount = nPoints;

	const float flTotal = ZiplineRope_Length(pPoints, pDistances, nCount);
	if (!(flTotal > 0.0f))
	{
		ZiplineAlpha_ReportGuard(0x400u, nPoints);
		return pResult;
	}

	const unsigned nHandle = *reinterpret_cast<const unsigned*>(
		reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ACTIVEZIPLINE);
	void* const pActive = ZiplineExit_ResolveHandle(nHandle);

	// C_Ziprail alphas divide by published path length, not the polyline this walk produces.
	const ZiprailWireBlock* const pWire = pActive
		? ZiprailDedi_GetWireBlock(reinterpret_cast<uintptr_t>(pActive))
		: nullptr;
	const bool  bRail     = pWire && pWire->pathLen > 0.0f;
	const float flRideLen = bRail ? pWire->pathLen : flTotal;

	const float flEngineMax = bReverse ? 0.0f
		: (bTypeOne ? fmaxf(0.0f, flTotal - 30.0f) / flTotal : 1.0f);

	float flDetachDist  = 0.0f;
	float flHullHeight  = 0.0f;
	float flEyeDelta    = 0.0f;
	float flEyeHullDiff = 0.0f;
	float flStopShort   = 0.0f;
	float flAlphaMax    = 0.0f;

	if (!bReverse && bTypeOne)
	{
		flAlphaMax = fmaxf(0.0f, flRideLen - 30.0f) / flRideLen;
	}
	else
	{
		if (!v_Zipline_GetAutoDetachDistanceForEntity
			|| !v_Zipline_GetEntityHullHeight
			|| !CPlayer__EyePosition
			|| (!bReverse && !v_Zipline_GetNextEntity)
			|| (!s_rideCtx.bSlide && !v_CBaseEntity_GetAbsOrigin))
		{
			if (ZiplineAlpha_ShouldWarnSample(s_nAlphaNullHelperWarns))
			{
				Warning(eDLL_T::SERVER,
					"[ZIP-ALPHA] helper unresolved (GetAutoDetach/GetHullHeight/EyePosition/"
					"GetNextEntity/GetAbsOrigin) -- alpha-ceiling parity skipped\n");
			}
			return pResult;
		}

		void* const pFar = bReverse ? pActive
			: (pActive ? v_Zipline_GetNextEntity(pActive) : nullptr);

		if (!pActive || !pFar)
		{
			if (ZiplineAlpha_ShouldWarnSample(s_nAlphaNullFarWarns))
			{
				Warning(eDLL_T::SERVER,
					"[ZIP-ALPHA] active/far entity null (handle=0x%08X active=%p far=%p rev=%d) "
					"-- alpha-ceiling parity skipped\n",
					nHandle, pActive, pFar, bReverse ? 1 : 0);
			}
			return pResult;
		}

		flDetachDist = v_Zipline_GetAutoDetachDistanceForEntity(pFar);
		flHullHeight = v_Zipline_GetEntityHullHeight(player);

		Vector3D eye;
		CPlayer__EyePosition(reinterpret_cast<CPlayer*>(player), &eye);

		float flRefZ = 0.0f;
		if (s_rideCtx.bSlide)
		{
			if (!s_rideCtx.pMovePosition)
			{
				if (ZiplineAlpha_ShouldWarnSample(s_nAlphaNullRefWarns))
				{
					Warning(eDLL_T::SERVER,
						"[ZIP-ALPHA] slide pass has null move position -- alpha-ceiling parity skipped\n");
				}
				return pResult;
			}
			flRefZ = s_rideCtx.pMovePosition[2];
		}
		else
		{
			const float* const pAbs = v_CBaseEntity_GetAbsOrigin(player);
			if (!pAbs)
			{
				if (ZiplineAlpha_ShouldWarnSample(s_nAlphaNullRefWarns))
				{
					Warning(eDLL_T::SERVER,
						"[ZIP-ALPHA] GetAbsOrigin returned null -- alpha-ceiling parity skipped\n");
				}
				return pResult;
			}
			flRefZ = pAbs[2];
		}

		flEyeDelta    = eye.z - flRefZ;
		flEyeHullDiff = fmaxf(0.0f, flHullHeight - flEyeDelta);
		flStopShort   = fmaxf(0.0f, flDetachDist - flEyeHullDiff);

		const float flForwardMax = fmaxf(0.0f, flRideLen - flStopShort) / flRideLen;
		flAlphaMax = bReverse ? (1.0f - flForwardMax) : flForwardMax;
	}

	// Handoff tests alpha == 1.0 (0.0 reversed) before auto-detach. Do not pin a continuing segment short.
	const float flCeilingNatural = flAlphaMax;
	const bool  bFurther = ZiplineExit_HasFurtherSegment(pActive, bReverse != 0);

	if (bFurther && bridge_zip_interior_no_clamp.GetBool())
		flAlphaMax = bReverse ? 0.0f : 1.0f;

	if (!isfinite(flAlphaMax) || flAlphaMax < 0.0f || flAlphaMax > 1.0f)
	{
		if (ZiplineAlpha_ShouldWarnSample(s_nAlphaBadMaxWarns))
		{
			Warning(eDLL_T::SERVER,
				"[ZIP-ALPHA] alphaMax=%.6f not finite or outside [0,1] "
				"(total=%.3f detachDist=%.3f stopShort=%.3f) -- alpha-ceiling parity skipped\n",
				flAlphaMax, flTotal, flDetachDist, flStopShort);
		}
		return pResult;
	}

	s_rideCtx.flAlphaMax = flAlphaMax;
	s_rideCtx.bReverse = (bReverse != 0);
	s_rideCtx.bAlphaMaxValid = true;

	const float flAlphaOutPre = *pOutAlpha;
	int nClamped = 0;

	float flRailSettings[4] = {};
	const bool bRailVel    = pPlayerVelocity != nullptr;
	const bool bRailParity = bridge_zip_rail_ride_parity.GetBool();
	const bool bRailTune   = bRail && bRailVel && bRailParity
		&& ZiprailTune_Read(player, &s_railAccel,        sdk_ziprail_tune_accel,         &flRailSettings[0])
		&& ZiprailTune_Read(player, &s_railGravAccel,    sdk_ziprail_tune_grav_accel,    &flRailSettings[1])
		&& ZiprailTune_Read(player, &s_railMaxSpeedMul,  sdk_ziprail_tune_max_speed_mul, &flRailSettings[2])
		&& ZiprailTune_Read(player, &s_railMinSpeedFrac, sdk_ziprail_tune_min_speed_frac,&flRailSettings[3]);
	const bool bRailRide   = bRailTune;

	if (bRailRide)
	{
		// Own the whole step: the engine integrated a flat capped speed over its
		// own polyline length, and both of those differ from what the client did.
		int iLo = 0;
		int iHi = 1;
		ZiplineRope_Locate(pPoints, pDistances, nCount, flTotal * flAlphaIn,
			&iLo, &iHi, nullptr);

		float railDir[3];
		const bool bHavePathDir = ZiprailDedi_GetPathDirectionAtArcDistance(
			reinterpret_cast<uintptr_t>(pActive), flRideLen * flAlphaIn, railDir);

		float dirDot[3];
		float dirGrav[3];
		if (bHavePathDir)
		{
			dirDot[0] = railDir[0]; dirDot[1] = railDir[1]; dirDot[2] = railDir[2];
			dirGrav[0] = railDir[0]; dirGrav[1] = railDir[1]; dirGrav[2] = railDir[2];
		}
		else
		{
			ZiplineRope_Tangent(pPoints, iLo, iHi, dirDot);
			dirGrav[0] = dirDot[0]; dirGrav[1] = dirDot[1]; dirGrav[2] = dirDot[2];
		}

		// Dot term: negate when running past the alpha ceiling.
		if (flAlphaIn > flAlphaMax)
		{
			dirDot[0] = -dirDot[0];
			dirDot[1] = -dirDot[1];
			dirDot[2] = -dirDot[2];
		}
		// Gravity term: negate only on reverse.
		if (bReverse)
		{
			dirGrav[0] = -dirGrav[0];
			dirGrav[1] = -dirGrav[1];
			dirGrav[2] = -dirGrav[2];
		}

		const float flFrameTime = TriggerPass_FrameTime();
		const float flSpeed = ZiplineRail_Speed(dirDot, dirGrav, pPlayerVelocity,
			flRideSpeed, flFrameTime, flRailSettings[0], flRailSettings[1],
			flRailSettings[2], flRailSettings[3]);

		const float flStep = (flFrameTime * flSpeed) / flRideLen;
		const float flAlphaNew = (flAlphaIn <= flAlphaMax)
			? fminf(flAlphaMax, flAlphaIn + flStep)
			: fmaxf(flAlphaMax, flAlphaIn - flStep);

		float flPos[3];
		int nPolyPos = 0;
		if (!ZiprailDedi_GetPathPointAtArcDistance(
				reinterpret_cast<uintptr_t>(pActive), flRideLen * flAlphaNew, flPos))
		{
			nPolyPos = 1;
			ZiplineRope_Locate(pPoints, pDistances, nCount, flTotal * flAlphaNew,
				&iLo, &iHi, flPos);
		}

		float dirOut[3];
		const bool bHavePathDirOut = ZiprailDedi_GetPathDirectionAtArcDistance(
			reinterpret_cast<uintptr_t>(pActive), flRideLen * flAlphaNew, dirOut);
		if (!bHavePathDirOut)
			ZiplineRope_Tangent(pPoints, iLo, iHi, dirOut);
		if (bReverse)
		{
			dirOut[0] = -dirOut[0];
			dirOut[1] = -dirOut[1];
			dirOut[2] = -dirOut[2];
		}

		*pOutAlpha = flAlphaNew;
		for (int i = 0; i < 3; ++i)
		{
			pOutPosition[i] = flPos[i];
			pOutVelocity[i] = dirOut[i] * flSpeed;
		}

		nClamped = (flAlphaNew != flAlphaOutPre) ? 1 : 0;
		++s_nRailTicks;

		if (ZiplineRail_ShouldLog())
		{
			float flEntScale = -1.0f;
			if (pActive)
				flEntScale = *reinterpret_cast<const float*>(
					reinterpret_cast<const uint8_t*>(pActive) + ZE_ZIP_OFF_SPEED_SCALE);
			Msg(eDLL_T::SERVER,
				"[ZIP-RAIL] pass=%s nPts=%d rev=%d pathLen=%.3f polyLen=%.3f "
				"lenDelta=%.3f speed=%.3f scale=%.3f wireScale=%.3f "
				"dot=%.3f dirInZ=%.4f dirGravZ=%.4f "
				"pathDir=%d polyPos=%d step=%.6f alphaIn=%.6f alphaEngine=%.6f "
				"alphaOut=%.6f ceil=%.6f ticks=%llu\n",
				s_rideCtx.bSlide ? "slide" : "mount",
				nCount, bReverse ? 1 : 0, flRideLen, flTotal, flRideLen - flTotal,
				flSpeed, flEntScale, pWire ? pWire->speedScale : -1.0f,
				ZiplineExit_Dot3(dirDot, pPlayerVelocity), dirDot[2],
				dirGrav[2], bHavePathDir ? 1 : 0, nPolyPos,
				flStep, flAlphaIn, flAlphaOutPre, flAlphaNew, flAlphaMax,
				s_nRailTicks);
		}
	}
	else if (bRail)
	{
		s_rideCtx.bAlphaMaxValid = false;

		const unsigned nMask = (bRail ? 1u : 0u)
			| (bRailVel ? 2u : 0u)
			| (bRailParity ? 4u : 0u)
			| (bRailTune ? 8u : 0u);
		if (nMask != s_nRailUnavailWarnMask)
		{
			s_nRailUnavailWarnMask = nMask;
			Warning(eDLL_T::SERVER,
				"[ZIP-RAIL] ziprail ride model unavailable (wire=%d vel=%d parity=%d tune=%d) "
				"-- alpha ceiling left to the engine; the plain-rope detach clamp is not applied to a rail\n",
				bRail ? 1 : 0, bRailVel ? 1 : 0, bRailParity ? 1 : 0, bRailTune ? 1 : 0);
		}
	}
	else if (flAlphaMax != flEngineMax)
	{
		const float flClamped = bReverse ? fmaxf(flAlphaOutPre, flAlphaMax)
		                                 : fminf(flAlphaOutPre, flAlphaMax);
		if (flClamped != flAlphaOutPre)
		{
			*pOutAlpha = flClamped;
			nClamped = 1;

			// Re-derive position and direction at flClamped; speed is unchanged.
			int   iLo = 0;
			int   iHi = 1;
			float flPos[3];
			ZiplineRope_Locate(pPoints, pDistances, nCount, flTotal * flClamped,
				&iLo, &iHi, flPos);

			float dir[3];
			ZiplineRope_Tangent(pPoints, iLo, iHi, dir);
			if (bReverse)
			{
				dir[0] = -dir[0];
				dir[1] = -dir[1];
				dir[2] = -dir[2];
			}

			const float flSpeed = sqrtf(ZiplineExit_Dot3(pOutVelocity, pOutVelocity));
			for (int i = 0; i < 3; ++i)
			{
				pOutPosition[i] = flPos[i];
				pOutVelocity[i] = dir[i] * flSpeed;
			}
		}
	}

	if (ZiplineAlpha_ShouldLog())
	{
		Msg(eDLL_T::SERVER,
			"[ZIP-ALPHA] pass=%s nPts=%d rev=%d typeOne=%d rail=%d wire=%d vel=%d "
			"parity=%d tune=%d further=%d "
			"rideLen=%.3f total=%.3f "
			"detachDist=%.3f hullHeight=%.3f eyeDelta=%.3f eyeHullDiff=%.3f "
			"stopShort=%.3f engineMax=%.6f ceilNat=%.6f alphaMax=%.6f "
			"alphaIn=%.6f alphaOut=%.6f clamped=%d\n",
			s_rideCtx.bSlide ? "slide" : "mount",
			nCount, bReverse ? 1 : 0, bTypeOne ? 1 : 0, bRailRide ? 1 : 0,
			bRail ? 1 : 0, bRailVel ? 1 : 0, bRailParity ? 1 : 0, bRailTune ? 1 : 0,
			bFurther ? 1 : 0,
			flRideLen, flTotal,
			flDetachDist, flHullHeight, flEyeDelta, flEyeHullDiff,
			flStopShort, flEngineMax, flCeilingNatural, flAlphaMax,
			flAlphaOutPre, *pOutAlpha, nClamped);
	}

	return pResult;
}

//-----------------------------------------------------------------------------
// Auto-detach radius from origin + hull height, not the eye. Pass-through outside the armed window.
//-----------------------------------------------------------------------------
static float* __fastcall Hook_CPlayer_EyePosition(void* player, float* pOut,
	char bFollowViewEntity)
{
	if (player != s_pDetachRefPlayer
		|| !pOut
		|| !v_CBaseEntity_GetAbsOrigin
		|| !v_Zipline_GetEntityHullHeight)
		return v_CPlayer__EyePositionCore(player, pOut, bFollowViewEntity);

	const bool bLog = bridge_zip_detach_ref_diag.GetBool() || s_nDetachRefLogs < 4;

	float flEye[3] = {};
	if (bLog)
	{
		v_CPlayer__EyePositionCore(player, flEye, bFollowViewEntity);
		++s_nDetachRefLogs;
	}

	const float* const pOrigin = v_CBaseEntity_GetAbsOrigin(player);
	if (!pOrigin)
	{
		if (ZiplineExit_ShouldWarnSample(s_nDetachRefWarns))
		{
			Warning(eDLL_T::SERVER,
				"[ZIP-DETACHREF] GetAbsOrigin null -- auto-detach radius left on the eye\n");
		}
		return v_CPlayer__EyePositionCore(player, pOut, bFollowViewEntity);
	}

	const float flHullHeight = v_Zipline_GetEntityHullHeight(player);

	pOut[0] = pOrigin[0];
	pOut[1] = pOrigin[1];
	pOut[2] = pOrigin[2] + flHullHeight;

	++s_nDetachRefSubst;

	if (bLog)
	{
		Msg(eDLL_T::SERVER,
			"[ZIP-DETACHREF] player=%p hull=%.3f origin=(%.3f %.3f %.3f) "
			"eye=(%.3f %.3f %.3f) ref=(%.3f %.3f %.3f) subst=%llu\n",
			player, flHullHeight,
			pOrigin[0], pOrigin[1], pOrigin[2],
			flEye[0], flEye[1], flEye[2],
			pOut[0], pOut[1], pOut[2], s_nDetachRefSubst);
	}

	return pOut;
}

//-----------------------------------------------------------------------------
// CheckAutoDetach: gate then post-orig velocity. Return is always the original's.
//-----------------------------------------------------------------------------
static char __fastcall Hook_CPlayer_Zipline_CheckAutoDetach(void* player, float* vel)
{
	float flPre[3] = {};
	if (vel)
	{
		flPre[0] = vel[0];
		flPre[1] = vel[1];
		flPre[2] = vel[2];
	}

	// Present the hard-coded end-of-rope alpha the original gate tests, then
	// restore. *pRide was written by our clamp from the same flAlphaMax so the
	// bit-identical compare is correct.
	float* pAlpha = nullptr;
	float  flSavedAlpha = 0.0f;

	if (bridge_zip_exit_parity.GetBool()
		&& bridge_zip_detach_alpha_parity.GetBool()
		&& s_rideCtx.bAlphaMaxValid
		&& s_rideCtx.bSlide
		&& s_rideCtx.pPlayer == player)
	{
		float* const pRide = reinterpret_cast<float*>(
			reinterpret_cast<uint8_t*>(player) + ZE_PLAYER_OFF_SLIDINGALPHA);
		const float flGate = s_rideCtx.bReverse ? 0.0f : 1.0f;

		if (*pRide == s_rideCtx.flAlphaMax && *pRide != flGate)
		{
			pAlpha = pRide;
			flSavedAlpha = *pRide;
			*pRide = flGate;
		}
	}

	void* const pPrevRefPlayer = s_pDetachRefPlayer;
	if (bridge_zip_exit_parity.GetBool() && bridge_zip_detach_ref_parity.GetBool())
		s_pDetachRefPlayer = player;

	const char nResult = v_CPlayer__Zipline_CheckAutoDetach(player, vel);

	s_pDetachRefPlayer = pPrevRefPlayer;

	if (pAlpha)
	{
		*pAlpha = flSavedAlpha;

		if (ZiplineAlpha_ShouldLog())
		{
			Msg(eDLL_T::SERVER,
				"[ZIP-ALPHA] gate alpha=%.6f presented=%.6f result=%d\n",
				flSavedAlpha, s_rideCtx.bReverse ? 0.0f : 1.0f, static_cast<int>(nResult));
		}
	}

	// Far-post is inside end-entity detach radius; detach at path end. Skip if already at S21 ceiling.
	if (nResult && player && !pAlpha)
	{
		const unsigned nHandle = *reinterpret_cast<const unsigned*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ACTIVEZIPLINE);
		void* const pZip = ZiplineExit_ResolveHandle(nHandle);
		if (pZip && ZiprailDedi_HasPath(reinterpret_cast<uintptr_t>(pZip)))
		{
			const float flAlpha = *reinterpret_cast<const float*>(
				reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_SLIDINGALPHA);
			const bool bRev = (*reinterpret_cast<const uint8_t*>(
				reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPREVERSE)) != 0;
			const float flEnd = bRev ? 0.0f : 1.0f;
			if (fabsf(flAlpha - flEnd) > 0.02f)
				return 0;
		}
	}

	if (nResult == 0
		|| !bridge_zip_exit_parity.GetBool()
		|| !player
		|| !vel)
		return nResult;

	// Client refuses auto-detach rewrite unless the player is ziplining.
	const int nZipState = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPLINESTATE);
	if (nZipState == 0)
		return nResult;

	const unsigned nHandle = *reinterpret_cast<const unsigned*>(
		reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ACTIVEZIPLINE);
	void* const zip = ZiplineExit_ResolveHandle(nHandle);
	if (!zip)
		return nResult;

	if (!ZiplineExit_IsZipline(zip))
		return nResult;

	const int nType = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(zip) + ZE_ZIP_OFF_TYPE);

	if (nType == ZE_ZIP_TYPE_VERTICAL)
	{
		// Original returns detach without writing vel when the end-height
		// check fails; clamping then would diverge from the client.
		if (vel[0] == flPre[0] && vel[1] == flPre[1] && vel[2] == flPre[2])
			return nResult;

		float flMaxVert = 0.0f;
		if (!ZiplineExit_ReadNamedSettingsFloat(player, "ziplineMaxVerticalDetachSpeed",
			&s_nMaxVertDetachFieldOff, &s_bMaxVertDetachFieldResolved,
			&s_bWarnedMaxVertField, &flMaxVert))
			return nResult;

		const float flLen2 = ZiplineExit_Dot3(vel, vel);
		const float flMax2 = flMaxVert * flMaxVert;
		if (flLen2 > flMax2)
		{
			const float flScale = flMaxVert / sqrtf(flLen2);
			vel[0] *= flScale;
			vel[1] *= flScale;
			vel[2] *= flScale;
		}

		++s_nVerticalApps;

		if (ZiplineExit_ShouldLog())
		{
			Msg(eDLL_T::SERVER,
				"[ZIP-EXIT] branch=vertical player=%p zip=%p type=%d nPts=- rev=- "
				"dir=- velPre=(%.3f %.3f %.3f) velPost=(%.3f %.3f %.3f) "
				"floor=- ceil=%.3f ropeApps=%llu vertApps=%llu\n",
				player, zip, nType,
				flPre[0], flPre[1], flPre[2],
				vel[0], vel[1], vel[2],
				flMaxVert, s_nRopeApps, s_nVerticalApps);
		}

		return nResult;
	}

	const bool bReverse = *reinterpret_cast<const uint8_t*>(
		reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPREVERSE) != 0;

	const ZiprailWireBlock* const pWire =
		ZiprailDedi_GetWireBlock(reinterpret_cast<uintptr_t>(zip));
	if (pWire && pWire->pathLen > 0.0f && pWire->numNodes >= 2)
	{
		float dir[3] = {};
		const char* dirRule = "none";
		bool bHaveDir = false;
		bool bChordDegenerate = false;
		float flChordLen2 = 0.0f;

		if (v_CBaseEntity_GetAbsOrigin)
		{
			void* const pFar = bReverse ? zip
				: (v_Zipline_GetNextEntity ? v_Zipline_GetNextEntity(zip) : nullptr);
			if (pFar)
			{
				const float* const pFarOrigin = v_CBaseEntity_GetAbsOrigin(pFar);
				if (pFarOrigin)
				{
					const int nNodeIdx = bReverse ? 0 : (pWire->numNodes - 1);
					if (nNodeIdx >= 0
						&& nNodeIdx < pWire->numNodes
						&& nNodeIdx < kZiprailWireMaxNodes)
					{
						const float* const pNode = pWire->positions[nNodeIdx];

						dir[0] = pFarOrigin[0] - pNode[0];
						dir[1] = pFarOrigin[1] - pNode[1];
						dir[2] = pFarOrigin[2] - pNode[2];
						flChordLen2 = ZiplineExit_Dot3(dir, dir);
						if (flChordLen2 > 1.0e-6f)
						{
							const float flInv = 1.0f / sqrtf(flChordLen2);
							dir[0] *= flInv;
							dir[1] *= flInv;
							dir[2] *= flInv;
							bHaveDir = true;
							dirRule = "chord";
						}
						else
						{
							bChordDegenerate = true;
							dir[0] = 0.0f;
							dir[1] = 0.0f;
							dir[2] = 0.0f;
						}
					}
				}
			}
		}

		if (!bHaveDir)
		{
			if (bChordDegenerate
				&& ZiplineExit_ShouldWarnSample(s_nRailExitDegenerateWarns))
			{
				Warning(eDLL_T::SERVER,
					"[ZIP-EXIT] rail exit chord degenerate (len=%.6f) chain=%p -- "
					"far entity coincides with end path node; falling through to tangent\n",
					sqrtf(flChordLen2), zip);
			}

			bHaveDir = ZiprailDedi_GetPathDirectionAtArcDistance(
				reinterpret_cast<uintptr_t>(zip),
				bReverse ? 0.0f : pWire->pathLen, dir);
			if (bHaveDir)
			{
				if (bReverse)
				{
					dir[0] = -dir[0];
					dir[1] = -dir[1];
					dir[2] = -dir[2];
				}
				dirRule = "tangent";
			}
		}

		if (!bHaveDir || ZiplineExit_Dot3(dir, dir) < 1.0e-6f)
		{
			if (ZiplineExit_ShouldWarnSample(s_nRailExitDegenerateWarns))
			{
				Warning(eDLL_T::SERVER,
					"[ZIP-EXIT] rail exit direction is degenerate (dirRule=%s) -- "
					"leaving the engine's exit velocity alone\n", dirRule);
			}
			return nResult;
		}

		// S21 m_ziprailUseAutoDetachSpeed is a bool. Nonzero selects
		// ziprailDetachSpeed; zero is min(|vel|, maxJumpOffSpeed). A KV of 300
		// still sends 1 on the wire -- it is not a speed.
		float flCeil = 0.0f;
		bool bHaveCeil = false;
		if (pWire->useAutoDetachSpeed)
		{
			bHaveCeil = ZiprailTune_Read(player, &s_railDetachSpeed,
				sdk_ziprail_tune_detach_speed, &flCeil);
			if (!bHaveCeil)
				return nResult;
		}
		else
		{
			float flMaxJump = 0.0f;
			if (!ZiprailTune_Read(player, &s_railMaxJumpOff,
				sdk_ziprail_tune_max_jumpoff, &flMaxJump))
				return nResult;
			flCeil = fminf(sqrtf(ZiplineExit_Dot3(vel, vel)), flMaxJump);
			bHaveCeil = true;
		}

		float flFloor = 0.0f;
		if (!ZiplineExit_ReadZiplineSpeed(player, &flFloor))
		{
			if (ZiplineExit_ShouldWarnSample(s_nSettingsWarns))
			{
				Warning(eDLL_T::SERVER,
					"[ZIP-EXIT] ziplineSpeed settings field unreadable "
					"-- rail exit rewrite skipped\n");
			}
			return nResult;
		}

		if (bHaveCeil)
		{
			const float flSpeed = fminf(
				fmaxf(ZiplineExit_Dot3(vel, dir), flFloor), flCeil);
			vel[0] = dir[0] * flSpeed;
			vel[1] = dir[1] * flSpeed;
			vel[2] = dir[2] * flSpeed;

			++s_nRailExitApps;

			if (ZiplineExit_ShouldLog())
			{
				Msg(eDLL_T::SERVER,
					"[ZIP-EXIT] branch=rail player=%p zip=%p type=%d nPts=- "
					"rev=%d dir=(%.3f %.3f %.3f) dirRule=%s "
					"velPre=(%.3f %.3f %.3f) velPost=(%.3f %.3f %.3f) "
					"useAutoDetach=%d floor=%.3f ceil=%.3f "
					"railApps=%llu ropeApps=%llu vertApps=%llu\n",
					player, zip, nType, bReverse ? 1 : 0,
					dir[0], dir[1], dir[2], dirRule,
					flPre[0], flPre[1], flPre[2],
					vel[0], vel[1], vel[2],
					pWire->useAutoDetachSpeed, flFloor, flCeil,
					s_nRailExitApps, s_nRopeApps, s_nVerticalApps);
			}
			return nResult;
		}
	}

	// Non-vertical: direction * clamp(dot, floor, ceiling). Band can collapse to detach ceiling.
	if (!v_CZipline__GetPoints || !g_pZiplineSpeedFieldOffset)
		return nResult;

	// Bound GetPoints from m_numZiplinePoints first; *ioCount is not a capacity.
	const int nZipPoints =
		*(const int*)(reinterpret_cast<const uintptr_t>(zip) + ZE_ZIP_OFF_NUM_ZIP_POINTS);
	if (nZipPoints > ZE_MAX_POINTS)
	{
		if (ZiplineExit_ShouldWarnSample(s_nBadCountWarns))
		{
			Warning(eDLL_T::SERVER,
				"[ZIP-EXIT] entity point count %d exceeds buffer bound %d -- rope exit rewrite skipped\n",
				nZipPoints, ZE_MAX_POINTS);
		}
		return nResult;
	}

	float pts[ZE_MAX_POINTS * 3];
	int nCount = ZE_MAX_POINTS; // defensive seed only; callee overwrites with entity count
	v_CZipline__GetPoints(zip, pts, nullptr, &nCount);

	if (nCount < 2 || nCount > ZE_MAX_POINTS)
	{
		if (ZiplineExit_ShouldWarnSample(s_nBadCountWarns))
		{
			Warning(eDLL_T::SERVER,
				"[ZIP-EXIT] GetPoints nCount=%d out of range [2,%d] -- rope exit rewrite skipped "
				"(buffer bound ZE_MAX_POINTS=%d)\n",
				nCount, ZE_MAX_POINTS, ZE_MAX_POINTS);
		}
		return nResult;
	}

	float dir[3];
	if (bReverse)
	{
		// p[0] - p[1]
		dir[0] = pts[0] - pts[3];
		dir[1] = pts[1] - pts[4];
		dir[2] = pts[2] - pts[5];
	}
	else
	{
		// p[n-1] - p[n-2]
		const int iLast = (nCount - 1) * 3;
		const int iPrev = (nCount - 2) * 3;
		dir[0] = pts[iLast + 0] - pts[iPrev + 0];
		dir[1] = pts[iLast + 1] - pts[iPrev + 1];
		dir[2] = pts[iLast + 2] - pts[iPrev + 2];
	}

	// FLT_MIN mirrors the engine's own degenerate-length guard.
	const float flInv = 1.0f / fmaxf(sqrtf(ZiplineExit_Dot3(dir, dir)), FLT_MIN);
	dir[0] *= flInv;
	dir[1] *= flInv;
	dir[2] *= flInv;

	float flFloor = 0.0f;
	if (!ZiplineExit_ReadZiplineSpeed(player, &flFloor))
	{
		if (ZiplineExit_ShouldWarnSample(s_nSettingsWarns))
		{
			Warning(eDLL_T::SERVER,
				"[ZIP-EXIT] ziplineSpeed settings field unreadable "
				"(offset unresolved / still 0xFFFFFFFF / settings null / bad value) "
				"-- rope exit rewrite skipped\n");
		}
		return nResult;
	}

	float flCeil = 0.0f;
	if (!ZiplineExit_ReadNamedSettingsFloat(player, "ziplineDetachSpeed",
		&s_nDetachSpeedFieldOff, &s_bDetachSpeedFieldResolved,
		&s_bWarnedDetachSpeedField, &flCeil))
		return nResult;

	const float flSpeed = fminf(fmaxf(ZiplineExit_Dot3(vel, dir), flFloor), flCeil);
	vel[0] = dir[0] * flSpeed;
	vel[1] = dir[1] * flSpeed;
	vel[2] = dir[2] * flSpeed;

	++s_nRopeApps;

	if (ZiplineExit_ShouldLog())
	{
		Msg(eDLL_T::SERVER,
			"[ZIP-EXIT] branch=rope player=%p zip=%p type=%d nPts=%d rev=%d "
			"dir=(%.3f %.3f %.3f) velPre=(%.3f %.3f %.3f) velPost=(%.3f %.3f %.3f) "
			"floor=%.3f ceil=%.3f ropeApps=%llu vertApps=%llu\n",
			player, zip, nType, nCount, bReverse ? 1 : 0,
			dir[0], dir[1], dir[2],
			flPre[0], flPre[1], flPre[2],
			vel[0], vel[1], vel[2],
			flFloor, flCeil, s_nRopeApps, s_nVerticalApps);
	}

	return nResult;
}

static bool Hook_Zipline_Find(uintptr_t player,
	const float* playerPosition, const float* eyeDirection,
	uintptr_t* outZipline, float* outMountStartPosition,
	float* outUsePosition, bool* outReverse)
{
	bool found = v_Zipline_Find(player, playerPosition, eyeDirection,
		outZipline, outMountStartPosition, outUsePosition, outReverse);

	// Promoted ziprail is not in the spatial partition; name it from the chain registry.
	if (!found && bridge_zip_rail_find_inject.GetBool()
		&& playerPosition && outZipline && outMountStartPosition && outUsePosition
		&& outReverse)
	{
		// Same reference point the engine's own collector searches from.
		const float searchAt[3] = {
			playerPosition[0], playerPosition[1],
			playerPosition[2] + ZE_MOUNT_SEARCH_EYE_RAISE };

		float flDist = -1.0f;
		const uintptr_t rail = ZiprailDedi_FindNearestRail(
			searchAt, bridge_zip_rail_find_range.GetFloat(), &flDist);

		float closest[3] = {};
		float pathDir[3] = {};
		float flArc = 0.0f;
		if (rail && ZiprailDedi_ClosestPointOnPath(rail, searchAt, closest, pathDir, &flArc))
		{
			const ZiprailWireBlock* const pWire = ZiprailDedi_GetWireBlock(rail);
			if (pWire)
			{
				*outZipline = rail;
				outMountStartPosition[0] = closest[0];
				outMountStartPosition[1] = closest[1];
				outMountStartPosition[2] = closest[2];
				outUsePosition[0] = closest[0];
				outUsePosition[1] = closest[1];
				outUsePosition[2] = closest[2];
				*outReverse = ZiplineRail_ShouldReverse(
					reinterpret_cast<void*>(player), reinterpret_cast<void*>(rail),
					pWire, pathDir, closest, eyeDirection);
				found = true;

				if (ZiprailMount_ShouldLog(s_nMountInjectLogs))
				{
					Msg(eDLL_T::SERVER,
						"[ZIPRAIL-INJECT] rail=%p dist=%.1f use=(%.2f %.2f %.2f) rev=%d\n",
						reinterpret_cast<void*>(rail), flDist,
						closest[0], closest[1], closest[2], *outReverse ? 1 : 0);
				}
			}
		}
	}

	// Snapshot the engine's own mount decision before any rail rewrite.
	int revEngine = 0;
	float useEngine[3] = {};
	if (found)
	{
		if (outReverse)
			revEngine = (*outReverse) ? 1 : 0;
		if (outUsePosition)
		{
			useEngine[0] = outUsePosition[0];
			useEngine[1] = outUsePosition[1];
			useEngine[2] = outUsePosition[2];
		}
	}

	// Accept/reject stays engine-owned. Only rewrite reverse + use position for
	// a ziprail that has a baked path (S3 has no rail mount concept).
	int railParity = 0;
	float useRail[3] = {};
	int revRail = 0;
	if (found && outZipline && outUsePosition && outReverse
		&& playerPosition && eyeDirection
		&& bridge_zip_rail_mount_parity.GetBool())
	{
		const uintptr_t zip = *outZipline;
		float closest[3] = {};
		float pathDir[3] = {};
		float flArc = 0.0f;
		if (ZiprailDedi_ClosestPointOnPath(zip, playerPosition,
			closest, pathDir, &flArc))
		{
			const ZiprailWireBlock* const pWire = ZiprailDedi_GetWireBlock(zip);
			if (pWire)
			{
				outUsePosition[0] = closest[0];
				outUsePosition[1] = closest[1];
				outUsePosition[2] = closest[2];
				*outReverse = ZiplineRail_ShouldReverse(
					reinterpret_cast<void*>(player),
					reinterpret_cast<void*>(zip),
					pWire, pathDir, closest, eyeDirection);
				useRail[0] = closest[0];
				useRail[1] = closest[1];
				useRail[2] = closest[2];
				revRail = (*outReverse) ? 1 : 0;
				railParity = 1;
			}
		}
	}

	// Collector range is 120u; empty search is normal away from a rope.
	if (!found && bridge_zip_mount_diag.GetBool()
		&& (s_nMountFindMisses++ % kMountFindMissReportEvery) == 0)
	{
		ZiprailDedi_ReportNearestRail(player, "find miss");
	}

	if (!ZiprailMount_ShouldLog(s_nMountFindLogs))
		return found;

	uintptr_t zip = 0;
	int rail = 0;
	float mountStart[3] = {};

	if (found && outZipline)
	{
		zip = *outZipline;
		rail = ZiprailDedi_HasPath(zip) ? 1 : 0;
		if (outMountStartPosition)
		{
			mountStart[0] = outMountStartPosition[0];
			mountStart[1] = outMountStartPosition[1];
			mountStart[2] = outMountStartPosition[2];
		}
	}

	Msg(eDLL_T::SERVER,
		"[ZIPRAIL-FIND] player=%p found=%d zip=%p rail=%d rev=%d "
		"mountStart=(%.2f %.2f %.2f) use=(%.2f %.2f %.2f) "
		"railParity=%d useRail=(%.2f %.2f %.2f) revRail=%d\n",
		reinterpret_cast<void*>(player), found ? 1 : 0,
		reinterpret_cast<void*>(zip), rail, revEngine,
		mountStart[0], mountStart[1], mountStart[2],
		useEngine[0], useEngine[1], useEngine[2],
		railParity, useRail[0], useRail[1], useRail[2], revRail);

	return found;
}

static char __fastcall Hook_Zipline_JumpOff(void* zip, void* player, float* velocity,
	const void* eyeAngles, float flForwardMove, float flSideMove)
{
	float flPre[3] = {};
	if (velocity)
	{
		flPre[0] = velocity[0];
		flPre[1] = velocity[1];
		flPre[2] = velocity[2];
	}

	if (!v_Zipline_JumpOff)
		return 0;

	// Jump-to-mount still has IN_JUMP pressed. Native JumpOff treats that as
	// detach. JumpOff is only valid while already riding.
	if (player)
	{
		const int nState = *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPLINESTATE);
		const unsigned nHandle = *reinterpret_cast<const unsigned*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ACTIVEZIPLINE);
		if (nState == 0 || nHandle == 0xFFFFFFFFu
			|| !ZiplineExit_ResolveHandle(nHandle))
			return 0;
	}

	const char nResult = v_Zipline_JumpOff(zip, player, velocity, eyeAngles,
		flForwardMove, flSideMove);

	if (nResult == 0
		|| !bridge_zip_exit_parity.GetBool()
		|| !player
		|| !velocity
		|| !eyeAngles
		|| !zip)
		return nResult;

	if (!ZiplineExit_IsZipline(zip))
		return nResult;

	const int nType = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(zip) + ZE_ZIP_OFF_TYPE);
	if (nType == ZE_ZIP_TYPE_VERTICAL)
		return nResult;

	const ZiprailWireBlock* const pWire =
		ZiprailDedi_GetWireBlock(reinterpret_cast<uintptr_t>(zip));
	if (!pWire || !(pWire->pathLen > 0.0f))
		return nResult;

	QAngle eye;
	eye.x = reinterpret_cast<const float*>(eyeAngles)[0];
	eye.y = reinterpret_cast<const float*>(eyeAngles)[1];
	eye.z = reinterpret_cast<const float*>(eyeAngles)[2];
	Vector3D fwd;
	AngleVectors(eye, &fwd);

	// Near-vertical look uses an unresolved S3 move-direction helper -- decline.
	if (fabsf(fwd.x) < 0.01f && fabsf(fwd.y) < 0.01f)
	{
		if (!s_bWarnedNearVerticalLook)
		{
			s_bWarnedNearVerticalLook = true;
			Warning(eDLL_T::SERVER,
				"[ZIP-JUMPOFF] near-vertical eye look (|fwd.xy| < 0.01) -- "
				"rail jump-off rewrite declined (move-direction helper unresolved)\n");
		}
		return nResult;
	}

	float flMinJump = 0.0f;
	float flMaxJump = 0.0f;
	float flViewInfl = 0.0f;
	float flDownDeg = 0.0f;
	float flUpDeg = 0.0f;
	if (!ZiprailTune_Read(player, &s_railMinJumpOff, sdk_ziprail_tune_min_jumpoff, &flMinJump)
		|| !ZiprailTune_Read(player, &s_railMaxJumpOff, sdk_ziprail_tune_max_jumpoff, &flMaxJump)
		|| !ZiprailTune_Read(player, &s_railViewInfl, sdk_ziprail_tune_view_influence, &flViewInfl)
		|| !ZiprailTune_Read(player, &s_railDetachDown, sdk_ziprail_tune_detach_dir_max_down, &flDownDeg)
		|| !ZiprailTune_Read(player, &s_railDetachUp, sdk_ziprail_tune_detach_dir_max_up, &flUpDeg))
		return nResult;

	const float flHx = fwd.x;
	const float flHy = fwd.y;
	const float flHLen = sqrtf(flHx * flHx + flHy * flHy) + FLT_MIN;
	const float flUx = flHx / flHLen;
	const float flUy = flHy / flHLen;

	const float flDown = DEG2RAD(fminf(90.0f, flDownDeg));
	const float flUp = DEG2RAD(fminf(90.0f, flUpDeg));
	const float flSinUp = sinf(flUp);
	const float flCosUp = cosf(flUp);
	const float flSinDown = sinf(flDown);
	const float flCosDown = cosf(flDown);

	float d[3];
	const char* pszCone = "clamped-up";
	d[0] = flUx * flCosUp;
	d[1] = flUy * flCosUp;
	d[2] = flSinUp;
	if (fwd.z <= flSinUp)
	{
		if (-flSinDown <= fwd.z)
		{
			d[0] = fwd.x;
			d[1] = fwd.y;
			d[2] = fwd.z;
			pszCone = "raw";
		}
		else
		{
			d[0] = flUx * flCosDown;
			d[1] = flUy * flCosDown;
			d[2] = -flSinDown;
			pszCone = "clamped-down";
		}
	}

	{
		const float flInv = 1.0f / fmaxf(sqrtf(ZiplineExit_Dot3(d, d)), ZE_SQRT_FLT_MIN);
		d[0] *= flInv;
		d[1] *= flInv;
		d[2] *= flInv;
	}

	float flBlendT = fminf(fmaxf(flViewInfl, 0.0f), 1.0f);
	float rd[3] = {};
	const float flAlpha = *reinterpret_cast<const float*>(
		reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_SLIDINGALPHA);
	if (ZiprailDedi_GetPathDirectionAtArcDistance(
		reinterpret_cast<uintptr_t>(zip), pWire->pathLen * flAlpha, rd))
	{
		const float flMaxAbs = fmaxf(fmaxf(fabsf(rd[0]), fabsf(rd[1])), fabsf(rd[2]));
		if (flMaxAbs > 0.01f)
		{
			const bool bReverse = *reinterpret_cast<const uint8_t*>(
				reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPREVERSE) != 0;
			float r[3] = { rd[0], rd[1], rd[2] };
			if (bReverse)
			{
				r[0] = -r[0];
				r[1] = -r[1];
				r[2] = -r[2];
			}
			d[0] = d[0] * flBlendT + r[0] * (1.0f - flBlendT);
			d[1] = d[1] * flBlendT + r[1] * (1.0f - flBlendT);
			d[2] = d[2] * flBlendT + r[2] * (1.0f - flBlendT);

			const float flInv = 1.0f / fmaxf(sqrtf(ZiplineExit_Dot3(d, d)), ZE_SQRT_FLT_MIN);
			d[0] *= flInv;
			d[1] *= flInv;
			d[2] *= flInv;
		}
	}

	const float flDot = ZiplineExit_Dot3(d, flPre);
	float flSpeed = fminf(fmaxf(flDot, flMinJump), flMaxJump);

	const unsigned nFlags = *reinterpret_cast<const unsigned*>(
		reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_PLAYERFLAGS);
	if (sdk_ziprail_duck_detach.GetBool() && (nFlags & 0x4000004u))
		flSpeed = flMinJump;

	// Resolve impulse inputs before any write so a partial failure cannot strip
	// the engine's impulse from an already-overwritten velocity.
	const bool bApplyImpulse = (nFlags & 0x2u) != 0;
	float flJumpHeight = 0.0f;
	float flGravity = 0.0f;
	if (bApplyImpulse)
	{
		if (!ZiplineExit_ReadNamedSettingsFloat(player, "jumpHeight",
			&s_nJumpHeightFieldOff, &s_bJumpHeightFieldResolved,
			&s_bWarnedJumpHeightField, &flJumpHeight))
			return nResult;

		if (g_pCVar)
		{
			ConVar* const pGrav = g_pCVar->FindVar("sv_gravity");
			if (pGrav)
				flGravity = pGrav->GetFloat();
		}
		if (!(flGravity > 0.0f))
		{
			if (!s_bWarnedSvGravity)
			{
				s_bWarnedSvGravity = true;
				Warning(eDLL_T::SERVER,
					"[ZIP-JUMPOFF] sv_gravity unavailable or non-positive -- "
					"rail jump-off rewrite skipped (impulse requires it)\n");
			}
			return nResult;
		}
	}

	velocity[0] = d[0] * flSpeed;
	velocity[1] = d[1] * flSpeed;
	velocity[2] = d[2] * flSpeed;

	// Engine applies this impulse when flags bit 0x02 is set.
	if (bApplyImpulse)
	{
		const float* const pImp = reinterpret_cast<const float*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_JUMPOFFDIR);
		const float flScale = sqrtf((flGravity + flGravity) * flJumpHeight);
		velocity[0] += pImp[0] * flScale;
		velocity[1] += pImp[1] * flScale;
		velocity[2] += pImp[2] * flScale;
	}

	++s_nRailJumpoffApps;

	if (ZiplineExit_ShouldLog())
	{
		Msg(eDLL_T::SERVER,
			"[ZIP-JUMPOFF] branch=rail player=%p zip=%p cone=%s t=%.3f dot=%.3f "
			"speed=%.3f velPre=(%.3f %.3f %.3f) velPost=(%.3f %.3f %.3f) apps=%llu\n",
			player, zip, pszCone, flBlendT, flDot, flSpeed,
			flPre[0], flPre[1], flPre[2],
			velocity[0], velocity[1], velocity[2],
			s_nRailJumpoffApps);
	}

	return nResult;
}

// Ride alpha of a world position. A rail is parameterised by its baked path;
// anything else by the entity's own rope polyline.
static bool ZiplineExit_RideAlphaAtPosition(void* zip, const float pos[3], float* outAlpha)
{
	if (!zip || !pos || !outAlpha)
		return false;

	const ZiprailWireBlock* const pWire =
		ZiprailDedi_GetWireBlock(reinterpret_cast<uintptr_t>(zip));
	if (pWire && pWire->pathLen > 0.0f)
	{
		float closest[3];
		float dir[3];
		float flArc = 0.0f;
		if (!ZiprailDedi_ClosestPointOnPath(reinterpret_cast<uintptr_t>(zip),
			pos, closest, dir, &flArc))
			return false;
		*outAlpha = flArc / pWire->pathLen;
		return true;
	}

	if (!v_CZipline__GetPoints)
		return false;

	const int nZipPoints =
		*(const int*)(reinterpret_cast<const uintptr_t>(zip) + ZE_ZIP_OFF_NUM_ZIP_POINTS);
	if (nZipPoints > ZE_MAX_POINTS)
		return false;

	float pts[ZE_MAX_POINTS * 3];
	float dists[ZE_MAX_POINTS];
	for (int i = 0; i < ZE_MAX_POINTS; ++i)
		dists[i] = -1.0f;
	int nCount = ZE_MAX_POINTS;
	v_CZipline__GetPoints(zip, pts, dists, &nCount);
	if (nCount < 2 || nCount > ZE_MAX_POINTS)
		return false;

	const float flTotal = ZiplineRope_Length(pts, dists, nCount);
	if (!(flTotal > 0.0f))
		return false;

	float flBestD2 = FLT_MAX;
	float flBestArc = 0.0f;
	float flArcBefore = 0.0f;

	for (int i = 0; i < nCount - 1; ++i)
	{
		const float flSegLen = ZiplineRope_Segment(pts, dists, i);
		const float* const a = &pts[i * 3];
		const float* const b = &pts[(i + 1) * 3];
		const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		const float ap[3] = { pos[0] - a[0], pos[1] - a[1], pos[2] - a[2] };
		const float ab2 = ZiplineExit_Dot3(ab, ab);
		float t = (ab2 > 0.0f) ? (ZiplineExit_Dot3(ap, ab) / ab2) : 0.0f;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;

		const float q[3] = {
			a[0] + t * ab[0],
			a[1] + t * ab[1],
			a[2] + t * ab[2] };
		const float d[3] = { pos[0] - q[0], pos[1] - q[1], pos[2] - q[2] };
		const float d2 = ZiplineExit_Dot3(d, d);
		if (d2 < flBestD2)
		{
			flBestD2 = d2;
			flBestArc = flArcBefore + t * flSegLen;
		}
		flArcBefore += flSegLen;
	}

	*outAlpha = flBestArc / flTotal;
	return true;
}

static bool Hook_Zipline_Use(uintptr_t player, bool forGrappleZipline)
{
	static bool s_bFirstCall = false;
	if (!s_bFirstCall)
	{
		s_bFirstCall = true;
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-USE] first call player=%p grapple=%d\n",
			reinterpret_cast<void*>(player), forGrappleZipline ? 1 : 0);
	}

	// Refusing means not calling the original; the engine reads a false return
	// as a refused mount.
	if (ZiplineCooldown_ShouldRefuseMount(reinterpret_cast<void*>(player)))
		return false;

	uint32_t activeBefore = 0xFFFFFFFFu;
	int stateBefore = 0;
	if (player)
	{
		activeBefore = *reinterpret_cast<const uint32_t*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ACTIVEZIPLINE);
		stateBefore = *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPLINESTATE);

		// Leftover sliding alpha is 0 or 1 from the last ride. Native Use
		// CheckAutoDetach-compares those exact values before we can seed.
		if (bridge_zip_mount_alpha_seed.GetBool())
		{
			static constexpr float kPreSeedEps = 1.0e-4f;
			uint8_t* const pBytes = reinterpret_cast<uint8_t*>(player);
			float* const pRide = reinterpret_cast<float*>(
				pBytes + ZE_PLAYER_OFF_SLIDINGALPHA);
			const float flCur = *pRide;
			if (flCur <= kPreSeedEps || flCur >= 1.0f - kPreSeedEps)
			{
				void* const pChange = pBytes + ZE_PLAYER_OFF_NETSTATECHANGED;
				(**reinterpret_cast<void(__fastcall***)(void*, void*)>(pChange))(
					pChange, pRide);
				*pRide = kPreSeedEps;
			}
		}
	}

	const bool result = v_Zipline_Use(player, forGrappleZipline);

	if (result && player && bridge_zip_mount_alpha_seed.GetBool())
	{
		const unsigned nHandle = *reinterpret_cast<const unsigned*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ACTIVEZIPLINE);
		void* const zip = ZiplineExit_ResolveHandle(nHandle);
		if (zip)
		{
			const float* const pUse = reinterpret_cast<const float*>(
				reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPUSEPOS);
			float flAlpha = 0.0f;
			if (ZiplineExit_RideAlphaAtPosition(zip, pUse, &flAlpha))
			{
				static constexpr float kSeedEps = 1.0e-4f;
				flAlpha = fminf(fmaxf(flAlpha, kSeedEps), 1.0f - kSeedEps);

				uint8_t* const pBytes = reinterpret_cast<uint8_t*>(player);
				float* const pRide = reinterpret_cast<float*>(
					pBytes + ZE_PLAYER_OFF_SLIDINGALPHA);
				const float flAlphaPre = *pRide;
				if (*pRide != flAlpha)
				{
					void* const pChange = pBytes + ZE_PLAYER_OFF_NETSTATECHANGED;
					(**reinterpret_cast<void(__fastcall***)(void*, void*)>(pChange))(
						pChange, pRide);
					*pRide = flAlpha;
				}

				if (bridge_zip_mount_diag.GetBool()
					&& ZiprailMount_ShouldLog(s_nMountAlphaLogs))
				{
					const int nRail = ZiprailDedi_HasPath(
						reinterpret_cast<uintptr_t>(zip)) ? 1 : 0;
					const int nRev = (*reinterpret_cast<const uint8_t*>(
						pBytes + ZE_PLAYER_OFF_ZIPREVERSE)) ? 1 : 0;
					Msg(eDLL_T::SERVER,
						"[ZIP-MOUNTALPHA] player=%p zip=%p rail=%d rev=%d "
						"use=(%.2f %.2f %.2f) alphaPre=%.6f alphaSeed=%.6f\n",
						reinterpret_cast<void*>(player), zip, nRail, nRev,
						pUse[0], pUse[1], pUse[2], flAlphaPre, flAlpha);
				}
			}
		}
	}

	if (result)
		ZiplineCooldown_OnMountGranted(reinterpret_cast<void*>(player));
	else if (bridge_zip_mount_diag.GetBool())
		ZiprailDedi_ReportNearestRail(player, "mount refused");

	if (!ZiprailMount_ShouldLog(s_nMountUseLogs))
		return result;

	uint32_t activeAfter = 0xFFFFFFFFu;
	int stateAfter = 0;
	float use[3] = {};
	int rev = 0;
	if (player)
	{
		activeAfter = *reinterpret_cast<const uint32_t*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ACTIVEZIPLINE);
		stateAfter = *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPLINESTATE);
		const float* pUse = reinterpret_cast<const float*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPUSEPOS);
		use[0] = pUse[0];
		use[1] = pUse[1];
		use[2] = pUse[2];
		rev = (*reinterpret_cast<const uint8_t*>(
			reinterpret_cast<const uint8_t*>(player) + ZE_PLAYER_OFF_ZIPREVERSE)) ? 1 : 0;
	}

	Msg(eDLL_T::SERVER,
		"[ZIPRAIL-USE] player=%p grapple=%d result=%d activeZip=%08X->%08X "
		"state=%d->%d use=(%.2f %.2f %.2f) rev=%d\n",
		reinterpret_cast<void*>(player),
		forGrappleZipline ? 1 : 0,
		result ? 1 : 0,
		activeBefore, activeAfter,
		stateBefore, stateAfter,
		use[0], use[1], use[2], rev);

	return result;
}

static char __fastcall Hook_CZipline_HandleUse(uintptr_t player, void* zipEnt,
	uintptr_t caller, int nUseType)
{
	static bool s_bFirstCall = false;
	if (!s_bFirstCall)
	{
		s_bFirstCall = true;
		Warning(eDLL_T::SERVER,
			"[ZIP-USEDISPATCH] first call player=%p ent=%p useType=%d\n",
			reinterpret_cast<void*>(player), zipEnt, nUseType);
	}

	const char nResult = v_CZipline_HandleUse
		? v_CZipline_HandleUse(player, zipEnt, caller, nUseType) : 0;

	if (!ZiprailMount_ShouldLog(s_nMountUseDispatchLogs))
		return nResult;

	const int nIsZip = ZiplineExit_CallVtblBool(zipEnt, ZE_ZIP_VTBL_ISZIPLINE) ? 1 : 0;
	const int nIsZipEnd = ZiplineExit_CallVtblBool(zipEnt, ZE_ZIP_VTBL_ISZIPLINEEND) ? 1 : 0;
	const int nRail = zipEnt && ZiprailDedi_HasPath(reinterpret_cast<uintptr_t>(zipEnt)) ? 1 : 0;

	Msg(eDLL_T::SERVER,
		"[ZIP-USEDISPATCH] player=%p ent=%p useType=%d zipline=%d ziplineEnd=%d "
		"rail=%d result=%d\n",
		reinterpret_cast<void*>(player), zipEnt, nUseType,
		nIsZip, nIsZipEnd, nRail, static_cast<int>(nResult));

	return nResult;
}

void VZiplineExitParity::GetAdr(void) const
{
	LogFunAdr("CPlayer::Zipline_CheckAutoDetach", v_CPlayer__Zipline_CheckAutoDetach);
	LogFunAdr("CZipline::GetPoints", v_CZipline__GetPoints);
	LogFunAdr("CPlayer::Zipline_MoveAlongRope", v_Zipline_MoveAlongRope);
	LogFunAdr("CPlayer::Zipline_MoveUpdateSlide", v_Zipline_MoveUpdateSlide);
	LogFunAdr("CPlayer::Zipline_MoveUpdateMount", v_Zipline_MoveUpdateMount);
	LogFunAdr("Zipline_GetEntityHullHeight", v_Zipline_GetEntityHullHeight);
	LogFunAdr("Zipline_GetAutoDetachDistanceForEntity", v_Zipline_GetAutoDetachDistanceForEntity);
	LogFunAdr("Zipline_GetNextEntity", v_Zipline_GetNextEntity);
	LogFunAdr("Zipline_GetPrevEntity", v_Zipline_GetPrevEntity);
	LogFunAdr("CBaseEntity::GetAbsOrigin", v_CBaseEntity_GetAbsOrigin);
	LogFunAdr("CPlayer::EyePosition", v_CPlayer__EyePositionCore);
	LogFunAdr("CPlayer::Zipline_Use", v_Zipline_Use);
	LogFunAdr("Zipline_Find", v_Zipline_Find);
	LogFunAdr("Zipline_JumpOff", v_Zipline_JumpOff);
	LogFunAdr("CZipline_HandleUse", v_CZipline_HandleUse);
	LogVarAdr("g_pZiplineSpeedFieldOffset", g_pZiplineSpeedFieldOffset);
}

void VZiplineExitParity::GetFun(void) const
{
	// CPlayer::Zipline_CheckAutoDetach. The [rcx+67D4h] immediate is
	// m_activeZipline on the server half; the client twin uses a different offset.
	Module_FindPattern(g_GameDll,
		"40 55 56 41 55 48 81 EC ?? ?? ?? ?? 44 8B 81 D4 67 00 00 48 8B F2 48 8B E9")
		.GetPtr(v_CPlayer__Zipline_CheckAutoDetach);

	// CZipline::GetPoints. Narrow enough to exclude the listen-server client twin.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 01 "
		"49 8B F1 4D 8B F0 48 8B FA 48 8B D9 FF 90 10 03 00 00 84 C0 0F 84 ?? ?? ?? ?? "
		"8B 83 38 0D 00 00")
		.GetPtr(v_CZipline__GetPoints);

	// Settings-field offset for ziplineSpeed -- dword filled when settings load.
	g_pZiplineSpeedFieldOffset = Module_FindPattern(g_GameDll,
		"F3 44 0F 10 34 18 8B 05 ?? ?? ?? ?? F3 44 0F 10 2C 18 48 8D 9F 1C 68 00 00")
		.Offset(6).ResolveRelativeAddress(2, 6).RCast<const uint32_t*>();

	// MoveAlongRope: no unique prologue (client twin is byte-identical). Anchor on gpGlobals frametime at +0x294.
	Module_FindPattern(g_GameDll,
		"48 8B 05 0D B5 51 0C F3 0F 58 C3 F3 0F 10 58 30 F3 0F 58 C2 F3 0F 10 15 98 A4 5E 00")
		.Offset(-0x294)
		.GetPtr(v_Zipline_MoveAlongRope);

	Module_FindPattern(g_GameDll,
		"48 89 74 24 ?? 55 57 41 54 41 55 41 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B F1")
		.GetPtr(v_Zipline_MoveUpdateSlide);

	Module_FindPattern(g_GameDll,
		"48 89 54 24 ?? 55 53 56 57 41 54 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B F9")
		.GetPtr(v_Zipline_MoveUpdateMount);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 40 48 8B 01 48 8B D9 33 FF FF 90 E8 02 00 00 84 C0 "
		"8B 83 40 03 00 00 89 44 24 38 8B 83 4C 03 00 00 48 0F 45 FB 89 44 24 28 "
		"48 85 FF 74 14 8B 87 04 66 00 00")
		.GetPtr(v_Zipline_GetEntityHullHeight);

	// The 0x320 vtable slot and the 0xB70 field displacement are the server half; the
	// client twin uses 0x4F0 and 0xA00.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 8B 01 48 8B D9 FF 90 20 03 00 00 84 C0 74 ?? "
		"F3 0F 10 05 ?? ?? ?? ?? F3 0F 5F 83 70 0B 00 00")
		.GetPtr(v_Zipline_GetAutoDetachDistanceForEntity);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 8B 01 48 8B D9 FF 90 ?? ?? ?? ?? 84 C0 74 ?? "
		"8B 8B ?? ?? ?? ?? 83 F9 ?? 74 ?? 0F B7 C1 48 8D 15")
		.GetPtr(v_Zipline_GetNextEntity);

	// Same shape as the next-entity getter; the m_prevZipline displacement 0xE64 is
	// the discriminator (m_nextZipline is 0xE68) and is unique in the binary.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 90 10 03 00 00 84 C0 74 ?? "
		"8B 8B 64 0E 00 00 83 F9 FF")
		.GetPtr(v_Zipline_GetPrevEntity);

	// The 0x230 dirty-flag and 0x450 origin displacements pick GetAbsOrigin out of a
	// cluster of three byte-identical accessors.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 8B 81 30 02 00 00 48 8B D9 C1 E8 0B A8 01 74 12 "
		"E8 ?? ?? ?? ?? 48 8D 83 50 04 00 00 48 83 C4 20 5B C3 48 8D 81 50 04 00 00")
		.GetPtr(v_CBaseEntity_GetAbsOrigin);

	// CPlayer::EyePosition(Vector*, bool) -- the three-argument core. Single hit.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 7C 24 ?? 55 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? "
		"48 8B FA 48 8B D9 45 84 C0")
		.GetPtr(v_CPlayer__EyePositionCore);

	// CPlayer::Zipline_Use -- mount entry from +use.
	Module_FindPattern(g_GameDll,
		"40 55 53 57 48 8D 6C 24 B9 48 81 EC F0 00 00 00 0F B6 DA 48 8B F9 E8 ?? ?? ?? ?? 84 C0")
		.GetPtr(v_Zipline_Use);

	// Zipline_Find -- stack probe B8 48 10 00 00 (0x1048) is the uniqueness anchor.
	Module_FindPattern(g_GameDll,
		"4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 41 55 41 57 "
		"48 8D AC 24 B8 F0 FF FF B8 48 10 00 00 E8 ?? ?? ?? ?? 48 2B E0 4D 8B F8 "
		"48 8B DA 4C 8B E9")
		.GetPtr(v_Zipline_Find);

	// Server Zipline_JumpOff. The [rdx+60E0h] flags test is the twin discriminator.
	Module_FindPattern(g_GameDll,
		"40 55 57 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC A0 00 00 00 "
		"F7 82 E0 60 00 00 06 00 00 04 4D 8B E9 49 8B F8 4C 8B F2 4C 8B F9")
		.GetPtr(v_Zipline_JumpOff);

	// Entity Use handler that calls Zipline_Use(player, false) on the mount path.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 30 48 8B 02 48 8B F1 48 8B CA "
		"41 8B E9 48 8B DA FF 90 20 03 00 00 84 C0 75 10 48 8B 03 48 8B CB "
		"FF 90 10 03 00 00 84 C0 74 14 40 F6 C5 01")
		.GetPtr(v_CZipline_HandleUse);

	if (!v_CPlayer__Zipline_CheckAutoDetach)
		Warning(eDLL_T::SERVER,
			"[ZIP-EXIT] CPlayer::Zipline_CheckAutoDetach pattern unresolved -- exit-velocity parity disabled\n");

	if (!v_CZipline__GetPoints)
		Warning(eDLL_T::SERVER,
			"[ZIP-EXIT] CZipline::GetPoints pattern unresolved -- plain-rope exit rewrite disabled\n");

	if (!v_CPlayer__EyePositionCore)
		Warning(eDLL_T::SERVER,
			"[ZIP-DETACHREF] CPlayer::EyePosition pattern unresolved -- the auto-detach radius "
			"stays on the eye and will cross it a tick away from the client\n");

	if (!g_pZiplineSpeedFieldOffset)
		Warning(eDLL_T::SERVER,
			"[ZIP-EXIT] ziplineSpeed field-offset pattern unresolved -- plain-rope exit rewrite disabled\n");

	if (!v_Zipline_MoveAlongRope)
		Warning(eDLL_T::SERVER,
			"[ZIP-ALPHA] CPlayer::Zipline_MoveAlongRope pattern unresolved -- alpha-ceiling parity disabled\n");

	if (!v_Zipline_MoveUpdateSlide)
		Warning(eDLL_T::SERVER,
			"[ZIP-ALPHA] CPlayer::Zipline_MoveUpdateSlide pattern unresolved -- alpha-ceiling parity disabled\n");

	if (!v_Zipline_MoveUpdateMount)
		Warning(eDLL_T::SERVER,
			"[ZIP-ALPHA] CPlayer::Zipline_MoveUpdateMount pattern unresolved -- alpha-ceiling parity disabled\n");

	if (!v_Zipline_GetEntityHullHeight)
		Warning(eDLL_T::SERVER,
			"[ZIP-ALPHA] Zipline_GetEntityHullHeight pattern unresolved -- alpha-ceiling parity disabled\n");

	if (!v_Zipline_GetAutoDetachDistanceForEntity)
		Warning(eDLL_T::SERVER,
			"[ZIP-ALPHA] Zipline_GetAutoDetachDistanceForEntity pattern unresolved -- alpha-ceiling parity disabled\n");

	if (!v_Zipline_GetNextEntity)
		Warning(eDLL_T::SERVER,
			"[ZIP-ALPHA] Zipline_GetNextEntity pattern unresolved -- alpha-ceiling parity disabled\n");

	if (!v_Zipline_GetPrevEntity)
		Warning(eDLL_T::SERVER,
			"[ZIP-ALPHA] Zipline_GetPrevEntity pattern unresolved -- a reverse ride cannot "
			"detect a further segment, so its ceiling stays clamped and the handoff cannot fire\n");

	if (!v_CBaseEntity_GetAbsOrigin)
		Warning(eDLL_T::SERVER,
			"[ZIP-ALPHA] CBaseEntity::GetAbsOrigin pattern unresolved -- alpha-ceiling parity disabled\n");

	if (!v_Zipline_Use)
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-USE] CPlayer::Zipline_Use pattern unresolved -- mount probe disabled\n");

	if (!v_Zipline_Find)
		Warning(eDLL_T::SERVER,
			"[ZIPRAIL-FIND] Zipline_Find pattern unresolved -- mount search probe disabled\n");

	if (!v_Zipline_JumpOff)
		Warning(eDLL_T::SERVER,
			"[ZIP-JUMPOFF] Zipline_JumpOff pattern unresolved -- rail jump-off rewrite disabled\n");

	if (!v_CZipline_HandleUse)
		Warning(eDLL_T::SERVER,
			"[ZIP-USEDISPATCH] CZipline_HandleUse pattern unresolved -- use-dispatch probe disabled\n");
}

void VZiplineExitParity::Detour(const bool bAttach) const
{
	// Print resolved originals before DetourSetup rewrites the pointers to trampolines.
	if (bAttach)
	{
		const uintptr_t base = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
		const void* const pMoveAlong = reinterpret_cast<const void*>(v_Zipline_MoveAlongRope);
		const void* const pSlide = reinterpret_cast<const void*>(v_Zipline_MoveUpdateSlide);
		const void* const pMount = reinterpret_cast<const void*>(v_Zipline_MoveUpdateMount);
		const void* const pCheck = reinterpret_cast<const void*>(v_CPlayer__Zipline_CheckAutoDetach);
		const void* const pUse = reinterpret_cast<const void*>(v_Zipline_Use);
		const void* const pFind = reinterpret_cast<const void*>(v_Zipline_Find);
		const void* const pJump = reinterpret_cast<const void*>(v_Zipline_JumpOff);
		const void* const pHandle = reinterpret_cast<const void*>(v_CZipline_HandleUse);

		Warning(eDLL_T::SERVER,
			"[ZIP-ATTACH] MoveAlongRope=%p rva=0x%llX ok=%d "
			"MoveUpdateSlide=%p rva=0x%llX ok=%d "
			"MoveUpdateMount=%p rva=0x%llX ok=%d "
			"CheckAutoDetach=%p rva=0x%llX ok=%d "
			"Zipline_Use=%p rva=0x%llX ok=%d "
			"Zipline_Find=%p rva=0x%llX ok=%d "
			"Zipline_JumpOff=%p rva=0x%llX ok=%d "
			"CZipline_HandleUse=%p rva=0x%llX ok=%d\n",
			pMoveAlong,
			static_cast<unsigned long long>(pMoveAlong && base
				? reinterpret_cast<uintptr_t>(pMoveAlong) - base : 0),
			pMoveAlong ? 1 : 0,
			pSlide,
			static_cast<unsigned long long>(pSlide && base
				? reinterpret_cast<uintptr_t>(pSlide) - base : 0),
			pSlide ? 1 : 0,
			pMount,
			static_cast<unsigned long long>(pMount && base
				? reinterpret_cast<uintptr_t>(pMount) - base : 0),
			pMount ? 1 : 0,
			pCheck,
			static_cast<unsigned long long>(pCheck && base
				? reinterpret_cast<uintptr_t>(pCheck) - base : 0),
			pCheck ? 1 : 0,
			pUse,
			static_cast<unsigned long long>(pUse && base
				? reinterpret_cast<uintptr_t>(pUse) - base : 0),
			pUse ? 1 : 0,
			pFind,
			static_cast<unsigned long long>(pFind && base
				? reinterpret_cast<uintptr_t>(pFind) - base : 0),
			pFind ? 1 : 0,
			pJump,
			static_cast<unsigned long long>(pJump && base
				? reinterpret_cast<uintptr_t>(pJump) - base : 0),
			pJump ? 1 : 0,
			pHandle,
			static_cast<unsigned long long>(pHandle && base
				? reinterpret_cast<uintptr_t>(pHandle) - base : 0),
			pHandle ? 1 : 0);
	}

	if (v_CPlayer__Zipline_CheckAutoDetach)
		DetourSetup(&v_CPlayer__Zipline_CheckAutoDetach, &Hook_CPlayer_Zipline_CheckAutoDetach, bAttach);

	if (v_Zipline_MoveAlongRope)
		DetourSetup(&v_Zipline_MoveAlongRope, &Hook_Zipline_MoveAlongRope, bAttach);

	if (v_Zipline_MoveUpdateSlide)
		DetourSetup(&v_Zipline_MoveUpdateSlide, &Hook_Zipline_MoveUpdateSlide, bAttach);

	if (v_Zipline_MoveUpdateMount)
		DetourSetup(&v_Zipline_MoveUpdateMount, &Hook_Zipline_MoveUpdateMount, bAttach);

	if (v_Zipline_Use)
		DetourSetup(&v_Zipline_Use, &Hook_Zipline_Use, bAttach);

	if (v_Zipline_Find)
		DetourSetup(&v_Zipline_Find, &Hook_Zipline_Find, bAttach);

	if (v_Zipline_JumpOff)
		DetourSetup(&v_Zipline_JumpOff, &Hook_Zipline_JumpOff, bAttach);

	if (v_CZipline_HandleUse)
		DetourSetup(&v_CZipline_HandleUse, &Hook_CZipline_HandleUse, bAttach);

	if (v_CPlayer__EyePositionCore)
		DetourSetup(&v_CPlayer__EyePositionCore, &Hook_CPlayer_EyePosition, bAttach);
}

