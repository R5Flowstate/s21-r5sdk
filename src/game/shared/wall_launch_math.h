//=============================================================================//
//
// Purpose: Sparrow wall launch -- the climb high jump and its edge air
// control. Shared by the dedi and the client prediction twin; both must run
// the same numbers or every launch mispredicts.
//
//=============================================================================//
#ifndef WALL_LAUNCH_MATH_H
#define WALL_LAUNCH_MATH_H

#include <cmath>

// Sparrow setfile values (climbHighJumpMaxHeight, climbJumpEdgeAir*); neither
// engine's settings layout has the fields.
constexpr float WALL_LAUNCH_MAX_HEIGHT          = 300.0f;
constexpr float WALL_LAUNCH_EDGE_AIR_SPEED      = 320.0f;
constexpr float WALL_LAUNCH_EDGE_AIR_ACCEL      = 800.0f;

constexpr float WALL_LAUNCH_EXTRA_HEIGHT        = 100.0f;
constexpr float WALL_LAUNCH_OUTWARD_VEL_MIN     = 150.0f;
constexpr float WALL_LAUNCH_OUTWARD_VEL_MAX     = 450.0f;
constexpr float WALL_LAUNCH_OUTWARD_MIN_DIST    = 100.0f;
constexpr float WALL_LAUNCH_OUTWARD_MAX_DIST    = 200.0f;
constexpr float WALL_LAUNCH_EDGE_MAX_ANGLE_DEG  = 7.0f;
constexpr float WALL_LAUNCH_INTO_WALL_DOT       = -0.5f;

// Ledge probe: a downward line this far past the wall face, from the top of
// the reachable height to the feet. World brushes only, so both engines see
// the same geometry; PHYSICSCLIP because map terrain carries no SOLID bit.
constexpr float WALL_LAUNCH_LEDGE_INSET         = 40.0f;
constexpr float WALL_LAUNCH_LEDGE_MIN_NORMAL_Z  = 0.7f;
constexpr unsigned int WALL_LAUNCH_LEDGE_MASK   = 0x1420Bu;

struct WallLaunchState_t
{
	bool  m_bEnabled = false;
	bool  m_bUsed = false;          // one launch per ground touch
	bool  m_bEdgeAir = false;       // last jump was a wall launch
	bool  m_bPrevOnWall = false;
	int   m_nPrevSuperJumps = 0;
	float m_flWallNormal[3] = { 0.0f, 0.0f, 0.0f };
};

// JumpOutOfWallRun input direction: while hanging, a short stick is
// topped up toward the facing direction.
inline void WallLaunch_InputDir(const float moveDir[3], const float forward[3], const bool bHanging, float out[3])
{
	out[0] = moveDir[0]; out[1] = moveDir[1]; out[2] = moveDir[2];
	if (!bHanging)
		return;

	const float flLen = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	if (flLen >= 1.0f)
		return;

	for (int i = 0; i < 3; ++i)
		out[i] += (1.0f - flLen) * forward[i];
}

inline bool WallLaunch_ShouldLaunch(const WallLaunchState_t& st, const float inputDir[3], const float wallNormal[3])
{
	if (!st.m_bEnabled || st.m_bUsed)
		return false;

	const float flDot = inputDir[0] * wallNormal[0] + inputDir[1] * wallNormal[1] + inputDir[2] * wallNormal[2];
	return flDot < WALL_LAUNCH_INTO_WALL_DOT;
}

// flLedgeZ < 0 relative height means no ledge was found.
inline float WallLaunch_Height(const bool bLedge, const float flLedgeZ, const float flOriginZ, const float flFinalJumpUpHeight)
{
	if (!bLedge)
		return WALL_LAUNCH_MAX_HEIGHT;

	const float flToLedge = (flLedgeZ - flOriginZ) + WALL_LAUNCH_EXTRA_HEIGHT;
	if (flFinalJumpUpHeight <= flToLedge)
		return fminf(WALL_LAUNCH_MAX_HEIGHT, flToLedge);
	return flFinalJumpUpHeight;
}

// Returns false when the wall normal has no horizontal part.
inline bool WallLaunch_Velocity(const float wallNormal[3], const float vel[3], const float flHeight,
	const float flGravity, float out[3])
{
	const float flLen2D = sqrtf(wallNormal[0] * wallNormal[0] + wallNormal[1] * wallNormal[1]);
	if (flLen2D <= 0.0f)
		return false;

	const float wx = wallNormal[0] / flLen2D;
	const float wy = wallNormal[1] / flLen2D;
	const float flJumpVel = sqrtf(2.0f * flGravity * flHeight);

	// Keep only the component away from the wall; sideways speed is dropped.
	const float flAway = vel[0] * wx + vel[1] * wy;
	float x = wx * flAway;
	float y = wy * flAway;
	const float z = fminf(vel[2] + 1.5f * flJumpVel, flJumpVel);

	float flOutward;
	if (WALL_LAUNCH_OUTWARD_MIN_DIST == WALL_LAUNCH_OUTWARD_MAX_DIST)
	{
		flOutward = (flHeight - WALL_LAUNCH_OUTWARD_MAX_DIST < 0.0f)
			? WALL_LAUNCH_OUTWARD_VEL_MIN : WALL_LAUNCH_OUTWARD_VEL_MAX;
	}
	else
	{
		const float flFrac = fminf(fmaxf((flHeight - WALL_LAUNCH_OUTWARD_MIN_DIST)
			/ (WALL_LAUNCH_OUTWARD_MAX_DIST - WALL_LAUNCH_OUTWARD_MIN_DIST), 0.0f), 1.0f);
		flOutward = WALL_LAUNCH_OUTWARD_VEL_MIN + (WALL_LAUNCH_OUTWARD_VEL_MAX - WALL_LAUNCH_OUTWARD_VEL_MIN) * flFrac;
	}

	const float flAdd = fmaxf(0.0f, flOutward - (x * wx + y * wy));
	x += flAdd * wx;
	y += flAdd * wy;

	out[0] = x; out[1] = y; out[2] = z;
	return true;
}

// Rotates wishDir about +Z so it lies within flMaxDeg of target (2D), keeping
// its horizontal length and Z.
inline void WallLaunch_CapToAngle(float wishDir[3], const float target2D[2], const float flMaxDeg)
{
	const float flLen = sqrtf(wishDir[0] * wishDir[0] + wishDir[1] * wishDir[1]);
	if (flLen <= 0.0f)
		return;

	const float flCross = target2D[0] * wishDir[1] - target2D[1] * wishDir[0];
	const float flDot = target2D[0] * wishDir[0] + target2D[1] * wishDir[1];
	const float flAngle = atan2f(flCross, flDot);
	const float flMax = flMaxDeg * 0.017453292f;
	if (fabsf(flAngle) <= flMax)
		return;

	const float flRot = (flAngle > 0.0f) ? flMax : -flMax;
	const float c = cosf(flRot), s = sinf(flRot);
	wishDir[0] = (target2D[0] * c - target2D[1] * s) * flLen;
	wishDir[1] = (target2D[0] * s + target2D[1] * c) * flLen;
}

// Edge air control after a launch. Scales the engine's air-accelerate inputs
// from the class air speed/accel to the edge values and pulls the wish
// direction onto the wall. Returns false when the launch terms do not apply.
inline bool WallLaunch_EdgeAir(const WallLaunchState_t& st, const float vel[3], float wishDir[3],
	const float flAirSpeed, const float flAirAccel, float* pWishSpeed, float* pAccel)
{
	if (!st.m_bEdgeAir)
		return false;
	if (!(WALL_LAUNCH_EDGE_AIR_SPEED > flAirSpeed || WALL_LAUNCH_EDGE_AIR_ACCEL > flAirAccel))
		return false;

	float dir[2] = { -st.m_flWallNormal[0], -st.m_flWallNormal[1] };
	if (fmaxf(fabsf(dir[0]), fabsf(dir[1])) <= 0.01f)
		return false;

	const float flLen = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
	dir[0] /= flLen; dir[1] /= flLen;

	const bool bMovingAway = -flAirSpeed > (dir[0] * vel[0] + dir[1] * vel[1]);
	const bool bWishIn = (dir[0] * wishDir[0] + dir[1] * wishDir[1]) > 0.5f;
	if (!bMovingAway && !bWishIn)
		return false;

	WallLaunch_CapToAngle(wishDir, dir, WALL_LAUNCH_EDGE_MAX_ANGLE_DEG);
	if (flAirSpeed > 0.0f)
		*pWishSpeed *= WALL_LAUNCH_EDGE_AIR_SPEED / flAirSpeed;
	if (flAirAccel > 0.0f)
		*pAccel *= WALL_LAUNCH_EDGE_AIR_ACCEL / flAirAccel;
	return true;
}

// Post-move bookkeeping: a ground touch re-arms the launch, and a new wall,
// a double jump or the ground ends the edge air control.
inline void WallLaunch_AfterMove(WallLaunchState_t& st, const bool bOnGround, const bool bOnWall, const int nSuperJumps)
{
	if (bOnGround && !bOnWall)
		st.m_bUsed = false;

	if (bOnGround || (bOnWall && !st.m_bPrevOnWall) || nSuperJumps > st.m_nPrevSuperJumps)
		st.m_bEdgeAir = false;

	st.m_bPrevOnWall = bOnWall;
	st.m_nPrevSuperJumps = nSuperJumps;
}

#endif // WALL_LAUNCH_MATH_H
