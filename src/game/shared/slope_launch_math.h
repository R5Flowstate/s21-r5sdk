//=============================================================================//
//
// Purpose: Source ramp launch, shared by both engines. Source clips a grounded
// player's velocity against the sloped floor it runs into; when the clip points
// up faster than a grounded player may move, the player leaves the ground. Apex
// keeps grounded velocity flat and only slides the position along the slope.
//
//=============================================================================//
#ifndef SLOPE_LAUNCH_MATH_H
#define SLOPE_LAUNCH_MATH_H

// CategorizePosition leaves the ground above this upward speed on both engines.
inline constexpr float SLOPE_LAUNCH_MIN_UP = 140.0f;
// Standable floor starts here; anything flatter than the second never launches.
inline constexpr float SLOPE_LAUNCH_MIN_NORMAL_Z = 0.7f;
inline constexpr float SLOPE_LAUNCH_FLAT_NORMAL_Z = 0.999f;

// push: a horizontal push that carried the grounded move without riding in the
// velocity. Source clips velocity plus push, then takes the push back out.
inline bool SlopeLaunch_Velocity(const float vel[3], const float push[2], const float normal[3], float out[3])
{
	if (normal[2] < SLOPE_LAUNCH_MIN_NORMAL_Z || normal[2] >= SLOPE_LAUNCH_FLAT_NORMAL_Z)
		return false;

	const float in[3] = { vel[0] + push[0], vel[1] + push[1], vel[2] };
	const float flBackoff = in[0] * normal[0] + in[1] * normal[1] + in[2] * normal[2];
	if (!(flBackoff < 0.0f))
		return false;

	out[0] = in[0] - normal[0] * flBackoff - push[0];
	out[1] = in[1] - normal[1] * flBackoff - push[1];
	out[2] = in[2] - normal[2] * flBackoff;
	return out[2] > SLOPE_LAUNCH_MIN_UP;
}

#endif // SLOPE_LAUNCH_MATH_H
