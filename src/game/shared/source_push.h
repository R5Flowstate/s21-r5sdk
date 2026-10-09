//=============================================================================//
//
// Purpose: Source trigger_push, shared by both engines so the client predicts
// exactly what the dedi runs. While a player touches push volumes:
//  - the vertical push accelerates them, and lifts them off the ground;
//  - the horizontal push moves them without friction or acceleration ever
//    seeing it: it rides in the velocity only for the move itself, so walls
//    and steps slide it the way they slide the player.
// Stepping out leaves the last horizontal push in their velocity.
// Boost mode (sv_source_push_boost) instead turns the speed into each volume
// entered and adds its push, stacking up to a cap.
//
//=============================================================================//
#ifndef SOURCE_PUSH_H
#define SOURCE_PUSH_H

#include <cmath>
#include <cstdint>

inline constexpr int SOURCE_PUSH_MAX_VOLUMES = 1024;
inline constexpr float SOURCE_PUSH_MAX_SPEED = 10000.0f;
inline constexpr float SOURCE_PUSH_MAX_COORD = 65536.0f;

// Boost mode: the fastest a run of pushes can make the player's horizontal speed.
inline constexpr float SOURCE_PUSH_BOOST_MAX_SPEED = 3500.0f;
inline constexpr int SOURCE_PUSH_TOUCH_WORDS = SOURCE_PUSH_MAX_VOLUMES / 32;

// Standing player hull, the box Source tests trigger touches with.
inline constexpr float SOURCE_PUSH_HULL_HALF_WIDTH = 16.0f;
inline constexpr float SOURCE_PUSH_HULL_HEIGHT = 72.0f;

struct SourcePushVolume_t
{
	float m_flMins[3];
	float m_flMaxs[3];
	float m_flPush[3];
};

struct SourcePushVolumes_t
{
	SourcePushVolume_t m_Volumes[SOURCE_PUSH_MAX_VOLUMES];
	int m_nCount = 0;
};

// Carried from one command to the next.
struct SourcePushState_t
{
	float m_flLast[2] = {}; // horizontal push of the last command spent inside a volume
	uint32_t m_Touching[SOURCE_PUSH_TOUCH_WORDS] = {}; // volumes touched last command
	bool m_bInside = false;
};

// One command's horizontal push, applied once by whichever move runs.
struct SourcePushMove_t
{
	float m_flPush[2] = {};
	float m_flInVelocity[2] = {};
	float m_flMoveStart[2] = {}; // horizontal velocity the move started from, push included
	float m_flGroundUsed[2] = {};
	bool m_bPending = false;
	bool m_bInVelocity = false;
};

inline bool SourcePush_AddVolume(SourcePushVolumes_t& v, const float mins[3], const float maxs[3], const float push[3])
{
	if (v.m_nCount >= SOURCE_PUSH_MAX_VOLUMES)
		return false;

	SourcePushVolume_t& out = v.m_Volumes[v.m_nCount];
	for (int i = 0; i < 3; i++)
	{
		if (!std::isfinite(mins[i]) || !std::isfinite(maxs[i]) || !std::isfinite(push[i]))
			return false;
		out.m_flMins[i] = fminf(fmaxf(fminf(mins[i], maxs[i]), -SOURCE_PUSH_MAX_COORD), SOURCE_PUSH_MAX_COORD);
		out.m_flMaxs[i] = fminf(fmaxf(fmaxf(mins[i], maxs[i]), -SOURCE_PUSH_MAX_COORD), SOURCE_PUSH_MAX_COORD);
		out.m_flPush[i] = fminf(fmaxf(push[i], -SOURCE_PUSH_MAX_SPEED), SOURCE_PUSH_MAX_SPEED);
	}
	v.m_nCount++;
	return true;
}

// Sum of the pushes of every volume the player hull at origin overlaps.
inline bool SourcePush_Touching(const SourcePushVolumes_t& v, const float origin[3], float push[3],
	uint32_t touching[SOURCE_PUSH_TOUCH_WORDS])
{
	push[0] = push[1] = push[2] = 0.0f;
	for (int w = 0; w < SOURCE_PUSH_TOUCH_WORDS; w++)
		touching[w] = 0;
	const float mins[3] = { origin[0] - SOURCE_PUSH_HULL_HALF_WIDTH, origin[1] - SOURCE_PUSH_HULL_HALF_WIDTH, origin[2] };
	const float maxs[3] = { origin[0] + SOURCE_PUSH_HULL_HALF_WIDTH, origin[1] + SOURCE_PUSH_HULL_HALF_WIDTH, origin[2] + SOURCE_PUSH_HULL_HEIGHT };

	bool bTouching = false;
	for (int i = 0; i < v.m_nCount; i++)
	{
		const SourcePushVolume_t& vol = v.m_Volumes[i];
		if (mins[0] >= vol.m_flMaxs[0] || maxs[0] <= vol.m_flMins[0] ||
			mins[1] >= vol.m_flMaxs[1] || maxs[1] <= vol.m_flMins[1] ||
			mins[2] >= vol.m_flMaxs[2] || maxs[2] <= vol.m_flMins[2])
			continue;

		push[0] += vol.m_flPush[0];
		push[1] += vol.m_flPush[1];
		push[2] += vol.m_flPush[2];
		touching[i >> 5] |= 1u << (i & 31);
		bTouching = true;
	}
	return bTouching;
}

// Boost mode: every volume entered turns the horizontal speed into its push
// direction and adds the push speed, so a run of pushes steers along the track
// and stacks speed. The vertical push still accelerates while inside.
inline bool SourcePush_Boost(SourcePushState_t& st, const SourcePushVolumes_t& v,
	const uint32_t touching[SOURCE_PUSH_TOUCH_WORDS], const float push[3], float vel[3], bool bGrounded, float dt)
{
	bool bLift = false;
	for (int i = 0; i < v.m_nCount; i++)
	{
		const uint32_t bit = 1u << (i & 31);
		if (!(touching[i >> 5] & bit) || (st.m_Touching[i >> 5] & bit))
			continue;

		const float* const p = v.m_Volumes[i].m_flPush;
		const float flPush = sqrtf(p[0] * p[0] + p[1] * p[1]);
		if (flPush > 0.0f)
		{
			const float flSpeed = fminf(sqrtf(vel[0] * vel[0] + vel[1] * vel[1]) + flPush, SOURCE_PUSH_BOOST_MAX_SPEED);
			vel[0] = p[0] / flPush * flSpeed;
			vel[1] = p[1] / flPush * flSpeed;
		}
		if (p[2] > 0.0f && bGrounded)
			bLift = true;
	}

	vel[2] += push[2] * dt;
	for (int w = 0; w < SOURCE_PUSH_TOUCH_WORDS; w++)
		st.m_Touching[w] = touching[w];
	st.m_bInside = true;
	return bLift;
}

// Before the move. Returns true when the player must first be lifted one unit
// and taken off the ground. bBoost picks boost mode over Source's carry.
inline bool SourcePush_BeforeMove(SourcePushState_t& st, SourcePushMove_t& move, const SourcePushVolumes_t& v,
	const float origin[3], float vel[3], bool bGrounded, float dt, bool bBoost)
{
	move = SourcePushMove_t();

	float push[3];
	uint32_t touching[SOURCE_PUSH_TOUCH_WORDS];
	if (!SourcePush_Touching(v, origin, push, touching))
	{
		// Stepping out keeps the push, plus the half frame Source adds with it.
		if (st.m_bInside && !bBoost)
		{
			const float flScale = 1.0f + dt * 0.5f;
			vel[0] += st.m_flLast[0] * flScale;
			vel[1] += st.m_flLast[1] * flScale;
		}
		st = SourcePushState_t();
		return false;
	}

	if (bBoost)
		return SourcePush_Boost(st, v, touching, push, vel, bGrounded, dt);

	for (int w = 0; w < SOURCE_PUSH_TOUCH_WORDS; w++)
		st.m_Touching[w] = touching[w];
	vel[2] += push[2] * dt;
	move.m_flPush[0] = push[0];
	move.m_flPush[1] = push[1];
	move.m_bPending = push[0] != 0.0f || push[1] != 0.0f;

	st.m_flLast[0] = push[0];
	st.m_flLast[1] = push[1];
	st.m_bInside = true;
	return push[2] > 0.0f && bGrounded;
}

// After acceleration (air or ground), before the move integrates the velocity.
inline void SourcePush_IntoVelocity(SourcePushMove_t& move, float vel[3], bool bGrounded)
{
	if (!move.m_bPending)
		return;

	vel[0] += move.m_flPush[0];
	vel[1] += move.m_flPush[1];
	move.m_flInVelocity[0] = move.m_flPush[0];
	move.m_flInVelocity[1] = move.m_flPush[1];
	move.m_flMoveStart[0] = vel[0];
	move.m_flMoveStart[1] = vel[1];
	if (bGrounded)
	{
		move.m_flGroundUsed[0] = move.m_flPush[0];
		move.m_flGroundUsed[1] = move.m_flPush[1];
	}
	move.m_bInVelocity = true;
	move.m_bPending = false;
}

// After the move: the push leaves the velocity again. Speed a wall took away is
// charged to the push first, so the player's own velocity never keeps the
// blocked part of a push (a wall would otherwise leave them flying away from it,
// and the next move's speed drop reads as an impact). The grounded push stays
// readable until the next command, for the slope launch to clip with.
inline void SourcePush_AfterMove(SourcePushMove_t& move, float vel[3])
{
	if (move.m_bInVelocity)
	{
		float out[2] = { move.m_flInVelocity[0], move.m_flInVelocity[1] };
		const float lost[2] = { move.m_flMoveStart[0] - vel[0], move.m_flMoveStart[1] - vel[1] };
		const float flLost = sqrtf(lost[0] * lost[0] + lost[1] * lost[1]);
		if (flLost > 0.001f)
		{
			const float dir[2] = { lost[0] / flLost, lost[1] / flLost };
			const float flBlocked = fminf(flLost, out[0] * dir[0] + out[1] * dir[1]);
			if (flBlocked > 0.0f)
			{
				out[0] -= dir[0] * flBlocked;
				out[1] -= dir[1] * flBlocked;
			}
		}
		vel[0] -= out[0];
		vel[1] -= out[1];
	}
	move.m_bInVelocity = false;
	move.m_bPending = false;
}

#endif // SOURCE_PUSH_H
