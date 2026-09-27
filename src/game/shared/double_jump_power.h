//=============================================================================//
//
// Purpose: Sparrow's double-jump power. Neither engine's settings layout has
// the drain/charge fields and both compile the jump-power refill rate to 0,
// so a double jump never costs anything and landing resets the meter. For a
// player with this enabled, a double jump needs and drains a full meter, the
// ground no longer resets it, and it refills over time. The dedi and the
// client prediction twin must run the same numbers.
//
//=============================================================================//
#ifndef DOUBLE_JUMP_POWER_H
#define DOUBLE_JUMP_POWER_H

// Sparrow setfile values (doubleJumpPowerDrainMin/Max, doubleJumpPowerChargeRate).
constexpr float DOUBLE_JUMP_POWER_DRAIN       = 100.0f;
constexpr float DOUBLE_JUMP_POWER_CHARGE_RATE = 66.0f;
constexpr float DOUBLE_JUMP_POWER_MAX         = 100.0f;

// Recharge starts on the ground and, once started, keeps going in the air.
constexpr float DOUBLE_JUMP_POWER_KEEP_CHARGING = 0.05f;

// A ground touch that is not a landing (mantle start, wall latch) still starts the recharge.
inline float DoubleJumpPower_OnTouchGround(const float flPower)
{
	return flPower > DOUBLE_JUMP_POWER_KEEP_CHARGING ? flPower : DOUBLE_JUMP_POWER_KEEP_CHARGING * 2.0f;
}

inline float DoubleJumpPower_Recharge(const float flPower, const bool bOnGround, const bool bOnWall,
	const float flRegenScale, const float flFrameTime)
{
	if (!((bOnGround && !bOnWall) || flPower > DOUBLE_JUMP_POWER_KEEP_CHARGING))
		return flPower;
	if (!(flFrameTime > 0.0f) || !(flRegenScale > 0.0f))
		return flPower;

	const float flNext = flPower + DOUBLE_JUMP_POWER_CHARGE_RATE * flRegenScale * flFrameTime;
	return flNext < DOUBLE_JUMP_POWER_MAX ? flNext : DOUBLE_JUMP_POWER_MAX;
}

#endif // DOUBLE_JUMP_POWER_H
