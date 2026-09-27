//=============================================================================//
//
// Purpose: glide terms the S21 player settings layout has no fields for.
// Shared by the server glide and the client prediction twin; both must run
// the same numbers or every glide mispredicts.
//
//=============================================================================//
#ifndef GLIDE_TUNING_H
#define GLIDE_TUNING_H

#include <cmath>

// Lifeline setfile values.
constexpr float GLIDE_MIN_DECAY_THRUST          = 0.2f; // thrust at an empty meter
constexpr float GLIDE_THRUST_DECAY_SHARPNESS    = 5.5f; // exponent on the spent meter fraction
constexpr float GLIDE_SPEED_DECAY_HALF_LIFE     = 0.9f; // seconds, horizontal speed above glideMaxSpeed
constexpr float GLIDE_UPBOOST_INTENSITY_SCALE   = 2.0f; // initial thrust (g) per second of fall
constexpr float GLIDE_UPBOOST_MAX_THRUST        = 2.5f; // g
constexpr float GLIDE_UPBOOST_DURATION_SCALE    = 1.5f; // boost seconds per second of fall
constexpr float GLIDE_UPBOOST_MAX_DURATION      = 1.5f; // seconds

constexpr float GLIDE_LN2 = 0.69314718f;

struct GlideBoost_t
{
	float flEndTime = -1.0f;
	float flFallRatio = 0.0f; // fall speed / gravity, at activation
};

// Activation while falling arms a thrust proportional to the fall speed.
inline GlideBoost_t Glide_ArmBoost(const float flVelZ, const float flGravity, const float flNow)
{
	GlideBoost_t boost;
	if (flVelZ >= 0.0f || flGravity <= 0.0f)
		return boost;

	boost.flFallRatio = fabsf(flVelZ / flGravity);
	boost.flEndTime = flNow + fminf(GLIDE_UPBOOST_MAX_DURATION, GLIDE_UPBOOST_DURATION_SCALE * boost.flFallRatio);
	return boost;
}

// Upward thrust in g; intensity * ratio decaying linearly to zero over the boost. Below the duration
// cap the rate does not depend on the ratio, so a ratio of 0 plus the networked end time is exact.
inline float Glide_BoostThrust(const float flEndTime, const float flFallRatio, const float flNow)
{
	const float flLeft = flEndTime - flNow;
	if (flLeft <= 0.0f)
		return 0.0f;

	const float flRate = fmaxf(GLIDE_UPBOOST_INTENSITY_SCALE / GLIDE_UPBOOST_DURATION_SCALE,
		GLIDE_UPBOOST_INTENSITY_SCALE * flFallRatio / GLIDE_UPBOOST_MAX_DURATION);
	return fminf(GLIDE_UPBOOST_MAX_THRUST, flRate * flLeft);
}

// glideThrust eases toward GLIDE_MIN_DECAY_THRUST as the meter drains.
inline float Glide_DecayedThrust(const float flThrust, const float flMeter, const float flDuration)
{
	if (flDuration <= 0.0f)
		return flThrust;

	const float flSpent = fminf(fmaxf(1.0f - flMeter / flDuration, 0.0f), 1.0f);
	return (GLIDE_MIN_DECAY_THRUST - flThrust) * powf(flSpent, GLIDE_THRUST_DECAY_SHARPNESS) + flThrust;
}

// Horizontal speed above flMax halves every GLIDE_SPEED_DECAY_HALF_LIFE seconds; vertical is untouched.
inline void Glide_DecayHorizontal(float* vel, const float flMax, const float dt)
{
	const float flSpeed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
	if (flSpeed <= flMax || flSpeed <= 0.0f)
		return;

	const float flScale = expf((-GLIDE_LN2 / GLIDE_SPEED_DECAY_HALF_LIFE) * dt);
	vel[0] *= flScale;
	vel[1] *= flScale;
}

#endif // GLIDE_TUNING_H
