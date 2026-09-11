//=============================================================================//
//
// Purpose: client prediction twin of mantle_boost. One TraversalMove detour.
//
//=============================================================================//
#ifndef MANTLE_BOOST_CLIENT_H
#define MANTLE_BOOST_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

// C_GameMovement::TraversalMove (S21). ctx: +8 C_Player*,
// +16 C_MoveData*, +24 settings-block ptr. a2 = justStarted. THE hook.
inline char (*C_GameMovement__TraversalMove)(void* ctx, char justStarted) = nullptr;

// C_GameMovement::Jump (S21). Call-only -- forced boost jump adds jumpDir*sqrt(2gh) on top of boost velocity.
inline char (*C_GameMovement__Jump)(void* ctx) = nullptr;

// C_GameMovement::AirMove_TapStrafe (S21). HOOKED -- add state==4 tap-strafe suppress; native has no mantle_boost gate.
inline char (*C_GameMovement__AirMove_TapStrafe)(void* ctx) = nullptr;

// C_GameMovement::Duck (S21). Call-only -- crouch FAILED finish forces Duck instead of Jump; never both.
inline char (*C_GameMovement__Duck)(void* ctx) = nullptr;

// C_Player::GetPoseSpeed_Sprint. Boost exit speed reads PLAYERPOSE_STANDING.
inline float (*C_Player__GetPoseSpeed_Sprint)(uintptr_t pPlayer, int nPose) = nullptr;

// C_PredictedFirstPersonProxy::GetTraversalViewPosition(origin, angles).
// S21 is 3-arg (no setViewCorrection) -- it always writes the proxy view-correction
// block at +5568; callers snapshot/restore that block.
inline void (*C_PredictedFirstPersonProxy__GetTraversalViewPosition)(uintptr_t pProxy, float* pEyeOrigin, float* pEyeAngles) = nullptr;

// DT_Player.m_mantleBoostState from snapshot decode. Local player only;
// arriving value overrides prediction.
void MantleBoostClient_OnAuthoritativeState(int nEntIndex, int nState);

// The FSM state the timing RUI displays, with the dedi's authoritative value
// already applied: 0 idle, 1 hang, 3 mantle jump, 4 sweet spot.
int MantleBoostClient_GetState(void);

// The sweet-spot angle gate in degrees. The traversal-aware form is the one the
// gate itself calls; the argument-free form returns whatever it last derived,
// for callers that hold no traversal inputs.
float MantleBoostClient_GetSweetSpotAngle(int nTravState, const Vector3D& vecFwd,
	const QAngle& eyeAngles);
float MantleBoostClient_GetSweetSpotAngle(void);

// The traversal ledge forward dir (m_traversalForwardDir). False when the
// engine has not initialized the traversal fields yet.
bool MantleBoostClient_GetTraversalFwd(uintptr_t pPlayer, float out[3]);

// Drains the boost FX queued by the handler above. Must be called from a frame,
// never from the decode: the FX runs client-VM script and the decode is still
// walking the entity list when the state arrives.
void MantleBoostClient_FrameUpdate(void);
void MantleBoostClient_OnSessionReset(void);

// bridge_mantle_boost_authoritative. Checked BEFORE the decode capture touches
// anything, so with it off the prop dispatch is byte-for-byte what it was before
// the capture existed.
bool MantleBoostClient_AuthoritativeEnabled();

///////////////////////////////////////////////////////////////////////////////
class VMantleBoostClient : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_GameMovement::TraversalMove", C_GameMovement__TraversalMove);
		LogFunAdr("C_GameMovement::Jump", C_GameMovement__Jump);
		LogFunAdr("C_GameMovement::Duck", C_GameMovement__Duck);
		LogFunAdr("C_GameMovement::AirMove_TapStrafe", C_GameMovement__AirMove_TapStrafe);
		LogFunAdr("C_Player::GetPoseSpeed_Sprint", C_Player__GetPoseSpeed_Sprint);
		LogFunAdr("C_PredictedFirstPersonProxy::GetTraversalViewPosition",
			C_PredictedFirstPersonProxy__GetTraversalViewPosition);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MANTLE_BOOST_CLIENT_H
