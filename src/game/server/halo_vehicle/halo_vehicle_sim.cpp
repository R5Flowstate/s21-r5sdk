//=============================================================================//
//
// Purpose: Halo mass-point vehicle simulation. A faithful port of the Halo CE
//          physics and vehicle update: same operations, same order, same
//          constants. Collision is reached only through IHaloWorld.
//
//=============================================================================//
#if !defined(HALO_SIM_STANDALONE)
#include "core/stdafx.h"
#endif // !HALO_SIM_STANDALONE
#include "halo_vehicle_sim.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRealEpsilon = 0.0001f;
constexpr float kGlobalGravity = 0.0035651792f;
constexpr float kGlobalWaterDensity = 1.0f;
constexpr float kPhysicsCollisionDepth = 0.2f;
// Base of the world: ground plane used when no surface is near a mass point.
constexpr HaloPlane kDepthsOfHell = { { 0.0f, 0.0f, 1.0f }, -256.0f };
// Vehicle angular acceleration cap, 0.8 degrees per tick squared.
constexpr float kVehicleAngularAcceleration = 0.0139626344f;

constexpr HaloVec3 kZero = { 0.0f, 0.0f, 0.0f };
constexpr HaloVec3 kUp = { 0.0f, 0.0f, 1.0f };
constexpr HaloVec3 kDown = { 0.0f, 0.0f, -1.0f };
constexpr HaloVec3 kForward = { 1.0f, 0.0f, 0.0f };
constexpr HaloQuat kIdentityQuat = { { 0.0f, 0.0f, 0.0f }, 1.0f };

enum MassPointFlags_t : uint32_t
{
	POINT_AT_REST = 1u << 0,
	POINT_ON_GROUND = 1u << 1,
	POINT_ON_VOLATILE_SURFACE = 1u << 2,
	POINT_IN_WATER = 1u << 3,
	POINT_ANTIGRAVING = 1u << 4,
};

struct FrictionDatum_t
{
	HaloVec3 friction;
	HaloVec3 parallel;
	HaloVec3 perpendicular;
};

struct PoweredState_t
{
	float groundFrictionVelocity;
	float waterFrictionVelocity;
	float airFrictionVelocity;
	float waterLiftRatio;
	float airLiftRatio;
	float thrustFraction;
	float antigravFraction;
	HaloQuat rotation;
	HaloMatrix4x3 rotationMatrix;
};

struct MassPointState_t
{
	uint32_t flags;
	HaloVec3 position;
	HaloVec3 forward;
	HaloVec3 up;
	HaloVec3 radius;
	HaloVec3 velocity;
	HaloVec3 velocityRelativeToGround;
	HaloPlane groundPlane;
	float groundDepth;
	float waterDepth;
	float normalForceMagnitude;
	HaloVec3 normalForce;
	FrictionDatum_t groundFriction;
	float waterPressureMagnitude;
	HaloVec3 waterPressure;
	FrictionDatum_t waterFriction;
	FrictionDatum_t airFriction;
	HaloVec3 poweredForce;
	HaloVec3 force;
	HaloVec3 torque;
};

//-----------------------------------------------------------------------------
// vector math
//-----------------------------------------------------------------------------
inline float Dot(const HaloVec3& a, const HaloVec3& b)
{
	return a.i * b.i + a.j * b.j + a.k * b.k;
}

inline HaloVec3 Cross(const HaloVec3& a, const HaloVec3& b)
{
	return { a.j * b.k - a.k * b.j, a.k * b.i - a.i * b.k, a.i * b.j - a.j * b.i };
}

inline HaloVec3 Add(const HaloVec3& a, const HaloVec3& b)
{
	return { a.i + b.i, a.j + b.j, a.k + b.k };
}

inline HaloVec3 Sub(const HaloVec3& a, const HaloVec3& b)
{
	return { a.i - b.i, a.j - b.j, a.k - b.k };
}

inline HaloVec3 Scale(const HaloVec3& a, const float s)
{
	return { a.i * s, a.j * s, a.k * s };
}

// point + vector * t
inline HaloVec3 PointFromLine(const HaloVec3& point, const HaloVec3& vector, const float t)
{
	return { vector.i * t + point.i, vector.j * t + point.j, vector.k * t + point.k };
}

inline float MagnitudeSquared(const HaloVec3& a)
{
	return Dot(a, a);
}

inline float Magnitude(const HaloVec3& a)
{
	return sqrtf(MagnitudeSquared(a));
}

inline float Normalize(HaloVec3& v)
{
	float magnitude = Magnitude(v);
	if (!(kRealEpsilon > fabsf(magnitude)))
		v = Scale(v, 1.0f / magnitude);
	else
		magnitude = 0.0f;
	return magnitude;
}

inline float Pin(const float value, const float lo, const float hi)
{
	return value < lo ? lo : (value > hi ? hi : value);
}

inline float PinFraction(const float value, const float begin, const float end)
{
	if (begin < end)
	{
		if (value <= begin)
			return 0.0f;
		if (value >= end)
			return 1.0f;
		return (value - begin) / (end - begin);
	}

	if (value <= end)
		return 1.0f;
	if (value >= begin)
		return 0.0f;
	return (begin - value) / (begin - end);
}

inline bool Limit(HaloVec3& v, const float length)
{
	const float dot = MagnitudeSquared(v);
	if (dot > length * length)
	{
		v = Scale(v, length / sqrtf(dot));
		return true;
	}
	return false;
}

inline float PlaneDistance(const HaloPlane& plane, const HaloVec3& point)
{
	return Dot(plane.n, point) - plane.d;
}

inline void ComponentVectorsFromNormal(const HaloVec3& vector, const HaloVec3& normal,
	HaloVec3& parallel, HaloVec3& perpendicular)
{
	parallel = Scale(normal, Dot(vector, normal));
	perpendicular = Sub(vector, parallel);
}

inline void PitchVectors(HaloVec3& forward, HaloVec3& up, const float sine, const float cosine)
{
	const HaloVec3 backward = Scale(forward, -1.0f);
	forward = { forward.i * cosine + up.i * sine, forward.j * cosine + up.j * sine, forward.k * cosine + up.k * sine };
	up = { up.i * cosine + backward.i * sine, up.j * cosine + backward.j * sine, up.k * cosine + backward.k * sine };
}

// Rotates forward about up.
inline void YawVectors(HaloVec3& forward, const HaloVec3& up, const float sine, const float cosine)
{
	const HaloVec3 cross = Cross(up, forward);
	forward = { forward.i * cosine + cross.i * sine, forward.j * cosine + cross.j * sine, forward.k * cosine + cross.k * sine };
}

inline void RotateVectorAboutAxis(HaloVec3& v, const HaloVec3& n, const float sine, const float cosine)
{
	const float oneMinusCosVDotN = Dot(n, v) * (1.0f - cosine);
	const float cx = n.k * v.j - v.k * n.j;
	const float cy = n.i * v.k - v.i * n.k;
	const float cz = n.j * v.i - v.j * n.i;

	v.i = cosine * v.i + oneMinusCosVDotN * n.i - sine * cx;
	v.j = cosine * v.j + oneMinusCosVDotN * n.j - sine * cy;
	v.k = cosine * v.k + oneMinusCosVDotN * n.k - sine * cz;
}

inline float AngleBetweenVectors(const HaloVec3& a, const HaloVec3& b)
{
	const float denominator = Magnitude(a) * Magnitude(b);
	if (denominator <= 0.0f)
		return 0.0f;
	return acosf(Pin(Dot(a, b) / denominator, -1.0f, 1.0f));
}

inline uint8_t QuantizeToByteLowerBound(const float value)
{
	long test = static_cast<long>(Pin(value, 0.0f, 1.0f) * 255.0f);
	while (test > 0 && value < static_cast<float>(test) / 255.0f)
		--test;
	return static_cast<uint8_t>(test);
}

//-----------------------------------------------------------------------------
// matrix math (rows are basis vectors; n[3] of the 4x3 is the position)
//-----------------------------------------------------------------------------
inline float (*Rows(HaloMatrix3x3& m))[3] { return reinterpret_cast<float(*)[3]>(&m.forward); }
inline const float (*Rows(const HaloMatrix3x3& m))[3] { return reinterpret_cast<const float(*)[3]>(&m.forward); }
inline float (*Rows(HaloMatrix4x3& m))[3] { return reinterpret_cast<float(*)[3]>(&m.forward); }
inline const float (*Rows(const HaloMatrix4x3& m))[3] { return reinterpret_cast<const float(*)[3]>(&m.forward); }

constexpr int kNextAxis[3] = { 1, 2, 0 };

HaloVec3 TransformPoint(const HaloMatrix4x3& m, const HaloVec3& p)
{
	float x = p.i, y = p.j, z = p.k;
	if (m.scale != 1.0f)
	{
		x *= m.scale;
		y *= m.scale;
		z *= m.scale;
	}
	return {
		m.up.i * z + m.left.i * y + m.forward.i * x + m.position.i,
		m.up.j * z + m.left.j * y + m.forward.j * x + m.position.j,
		m.up.k * z + m.left.k * y + m.forward.k * x + m.position.k };
}

HaloVec3 TransformNormal(const HaloMatrix4x3& m, const HaloVec3& n)
{
	return {
		n.i * m.forward.i + n.j * m.left.i + n.k * m.up.i,
		n.i * m.forward.j + n.j * m.left.j + n.k * m.up.j,
		n.i * m.forward.k + n.j * m.left.k + n.k * m.up.k };
}

HaloVec3 TransformVector(const HaloMatrix4x3& m, const HaloVec3& v)
{
	HaloVec3 s = v;
	if (m.scale != 1.0f)
		s = Scale(v, m.scale);
	return TransformNormal(m, s);
}

HaloVec3 InverseTransformVector(const HaloMatrix4x3& m, const HaloVec3& v)
{
	HaloVec3 s = v;
	if (m.scale != 1.0f)
		s = Scale(v, 1.0f / m.scale);
	return { Dot(s, m.forward), Dot(s, m.left), Dot(s, m.up) };
}

// Applies b, then a.
HaloMatrix4x3 Multiply(const HaloMatrix4x3& a, const HaloMatrix4x3& b)
{
	const float (*an)[3] = Rows(a);
	const float (*bn)[3] = Rows(b);
	HaloMatrix4x3 result;
	float (*rn)[3] = Rows(result);

	for (int row = 0; row < 4; ++row)
	{
		for (int column = 0; column < 3; ++column)
		{
			float value = bn[row][0] * an[0][column] + bn[row][1] * an[1][column] + bn[row][2] * an[2][column];
			if (row == 3)
				value = value * a.scale + an[3][column];
			rn[row][column] = value;
		}
	}
	result.scale = a.scale * b.scale;
	return result;
}

HaloMatrix4x3 RotationFromVectors(const HaloVec3& forward, const HaloVec3& up)
{
	HaloMatrix4x3 m;
	m.scale = 1.0f;
	m.forward = forward;
	m.left = Cross(up, forward);
	m.up = up;
	m.position = kZero;
	return m;
}

HaloMatrix4x3 FromPointAndVectors(const HaloVec3& point, const HaloVec3& forward, const HaloVec3& up)
{
	HaloMatrix4x3 m = RotationFromVectors(forward, up);
	m.position = point;
	return m;
}

HaloMatrix4x3 RotationFromQuaternion(const HaloQuat& q)
{
	float scale = q.v.i * q.v.i + q.v.j * q.v.j + q.v.k * q.v.k + q.w * q.w;
	scale = (scale != 0.0f) ? 2.0f / scale : 0.0f;

	const float x = scale * q.v.i, y = scale * q.v.j, z = scale * q.v.k;
	const float wx = q.w * x, wy = q.w * y, wz = q.w * z;
	const float xx = q.v.i * x, xy = q.v.i * y, xz = q.v.i * z;
	const float yy = q.v.j * y, yz = q.v.j * z, zz = q.v.k * z;

	HaloMatrix4x3 m;
	m.scale = 1.0f;
	m.forward = { 1.0f - (yy + zz), xy - wz, xz + wy };
	m.left = { xy + wz, 1.0f - (xx + zz), yz - wx };
	m.up = { xz - wy, yz + wx, 1.0f - (xx + yy) };
	m.position = kZero;
	return m;
}

void Transpose(HaloMatrix4x3& m)
{
	float (*n)[3] = Rows(m);
	float t;
	t = n[1][0]; n[1][0] = n[0][1]; n[0][1] = t;
	t = n[2][0]; n[2][0] = n[0][2]; n[0][2] = t;
	t = n[2][1]; n[2][1] = n[1][2]; n[1][2] = t;
}

HaloMatrix4x3 RotationFromAxisAndAngle(const HaloVec3& axis, const float sine, const float cosine)
{
	HaloMatrix4x3 m;
	float (*n)[3] = Rows(m);
	const HaloVec3 squared = { axis.i * axis.i, axis.j * axis.j, axis.k * axis.k };
	const HaloVec3 scaled = Scale(axis, sine);
	const float omc = 1.0f - cosine;

	m.scale = 1.0f;
	n[0][0] = (1.0f - squared.i) * cosine + squared.i;
	n[0][1] = axis.j * axis.i * omc;
	n[1][0] = n[0][1] - scaled.k;
	n[0][1] = scaled.k + n[0][1];
	n[1][1] = (1.0f - squared.j) * cosine + squared.j;
	n[0][2] = axis.k * axis.i * omc;
	n[2][0] = n[0][2] + scaled.j;
	n[0][2] = n[0][2] - scaled.j;
	n[2][2] = (1.0f - squared.k) * cosine + squared.k;
	n[1][2] = axis.k * axis.j * omc;
	n[2][1] = n[1][2] - scaled.i;
	n[1][2] = scaled.i + n[1][2];
	m.position = kZero;
	return m;
}

HaloMatrix4x3 Inverse(const HaloMatrix4x3& m)
{
	HaloMatrix4x3 r;
	if (m.scale == 0.0f)
	{
		memset(&r, 0, sizeof(r));
		return r;
	}

	const float (*mn)[3] = Rows(m);
	float (*rn)[3] = Rows(r);
	float x = -m.position.i, y = -m.position.j, z = -m.position.k;

	if (m.scale != 1.0f)
	{
		const float scale = 1.0f / m.scale;
		r.scale = scale;
		x *= scale;
		y *= scale;
		z *= scale;
	}
	else
		r.scale = 1.0f;

	rn[0][0] = mn[0][0]; rn[1][1] = mn[1][1]; rn[2][2] = mn[2][2];
	rn[1][0] = mn[0][1]; rn[0][1] = mn[1][0];
	rn[2][0] = mn[0][2]; rn[0][2] = mn[2][0];
	rn[2][1] = mn[1][2]; rn[1][2] = mn[2][1];

	rn[3][0] = x * rn[0][0] + y * rn[1][0] + z * rn[2][0];
	rn[3][1] = x * rn[0][1] + y * rn[1][1] + z * rn[2][1];
	rn[3][2] = x * rn[0][2] + y * rn[1][2] + z * rn[2][2];
	return r;
}

template<typename M>
HaloQuat RotationToQuaternion(const M& m)
{
	const float (*n)[3] = Rows(m);
	HaloQuat q;
	const float trace = n[0][0] + n[1][1] + n[2][2];

	if (trace > 0.0f)
	{
		float s = sqrtf(trace + 1.0f);
		q.w = 0.5f * s;
		s = 0.5f / s;
		q.v.i = (n[2][1] - n[1][2]) * s;
		q.v.j = (n[0][2] - n[2][0]) * s;
		q.v.k = (n[1][0] - n[0][1]) * s;
		return q;
	}

	int i = 0;
	if (n[1][1] > n[0][0])
		i = 1;
	if (n[2][2] > n[i][i])
		i = 2;
	const int j = kNextAxis[i];
	const int k = kNextAxis[j];

	float qv[3];
	float s = sqrtf(n[i][i] - (n[k][k] + n[j][j]) + 1.0f);
	qv[i] = s * 0.5f;
	if (s != 0.0f)
		s = 0.5f / s;
	qv[j] = (n[i][j] + n[j][i]) * s;
	qv[k] = (n[i][k] + n[k][i]) * s;
	q.w = (n[k][j] - n[j][k]) * s;
	q.v = { qv[0], qv[1], qv[2] };
	return q;
}

void QuaternionToAngleAndVector(const HaloQuat& q, float& angle, HaloVec3& v)
{
	v = q.v;
	angle = 2.0f * atan2f(Normalize(v), q.w);
	if (angle > kPi)
	{
		v = Scale(v, -1.0f);
		angle = kPi * 2.0f - angle;
	}
}

HaloMatrix3x3 Matrix3x3FromForwardAndUp(const HaloVec3& forward, const HaloVec3& up)
{
	return { forward, Cross(up, forward), up };
}

HaloMatrix3x3 Matrix3x3Multiply(const HaloMatrix3x3& a, const HaloMatrix3x3& b)
{
	HaloMatrix3x3 r;
	r.forward.i = b.forward.i * a.forward.i + b.forward.j * a.left.i + a.up.i * b.forward.k;
	r.forward.j = b.forward.j * a.left.j + b.forward.i * a.forward.j + b.forward.k * a.up.j;
	r.forward.k = b.forward.j * a.left.k + b.forward.i * a.forward.k + b.forward.k * a.up.k;
	r.left.i = b.left.j * a.left.i + a.forward.i * b.left.i + a.up.i * b.left.k;
	r.left.j = b.left.k * a.up.j + b.left.j * a.left.j + b.left.i * a.forward.j;
	r.left.k = b.left.k * a.up.k + b.left.j * a.left.k + b.left.i * a.forward.k;
	r.up.i = b.up.i * a.forward.i + b.up.j * a.left.i + b.up.k * a.up.i;
	r.up.j = b.up.k * a.up.j + b.up.j * a.left.j + b.up.i * a.forward.j;
	r.up.k = b.up.k * a.up.k + b.up.j * a.left.k + b.up.i * a.forward.k;
	return r;
}

HaloMatrix3x3 Matrix3x3Transpose(const HaloMatrix3x3& m)
{
	HaloMatrix3x3 r;
	r.forward = { m.forward.i, m.left.i, m.up.i };
	r.left = { m.forward.j, m.left.j, m.up.j };
	r.up = { m.forward.k, m.left.k, m.up.k };
	return r;
}

HaloVec3 Matrix3x3TransformVector(const HaloMatrix3x3& m, const HaloVec3& v)
{
	return {
		m.up.i * v.k + m.left.i * v.j + m.forward.i * v.i,
		m.up.j * v.k + m.forward.j * v.i + m.left.j * v.j,
		m.up.k * v.k + m.forward.k * v.i + m.left.k * v.j };
}

//-----------------------------------------------------------------------------
// physics variables
//-----------------------------------------------------------------------------
void SpeedUpdate(float& speed, const HaloSpeedParams& p, const float delta)
{
	const float magnitude = fabsf(delta);
	const float acceleration = magnitude * p.acceleration;
	const float deceleration = magnitude * p.deceleration;

	if (delta > 0.0f)
	{
		if (speed <= -deceleration)
			speed += deceleration;
		else if (speed >= 0.0f)
			speed += acceleration;
		else
			speed = (speed / deceleration + 1.0f) * acceleration;
		speed = fminf(speed, magnitude * p.positiveScale);
		return;
	}

	if (delta < 0.0f)
	{
		if (speed >= deceleration)
			speed -= deceleration;
		else if (speed <= 0.0f)
			speed -= acceleration;
		else
			speed = (speed / deceleration - 1.0f) * acceleration;
		speed = fmaxf(-magnitude * p.negativeScale, speed);
	}
}

bool SpeedUpdateSeek(float& speed, const HaloSpeedParams& p, const float target, const float delta)
{
	if (speed > target)
	{
		SpeedUpdate(speed, p, -delta);
		if (speed <= target)
		{
			speed = target;
			return true;
		}
		return false;
	}
	if (speed < target)
	{
		SpeedUpdate(speed, p, delta);
		if (speed >= target)
		{
			speed = target;
			return true;
		}
		return false;
	}
	return true;
}

// limits = { upper, lower }
float PositionSeekDirection(const float* limits, const float position, const bool wrap, const float target)
{
	float direction = target - position;
	if (direction != 0.0f)
	{
		if (wrap && fabsf(direction) > (limits[0] - limits[1]) * 0.5f)
			direction = -direction;
		direction = direction > 0.0f ? 1.0f : -1.0f;
	}
	return direction;
}

void PositionUpdate(float& position, const float* limits, const bool wrap, const float delta)
{
	position += delta;
	if (position < limits[1])
	{
		position = wrap ? position + (limits[0] - limits[1]) : limits[1];
		return;
	}
	if (position > limits[0])
		position = wrap ? position - (limits[0] - limits[1]) : limits[0];
}

bool PositionUpdateSeek(float& position, const float* limits, const bool wrap, const float target, const float delta)
{
	const float direction = PositionSeekDirection(limits, position, wrap, target);
	if (direction != 0.0f)
	{
		PositionUpdate(position, limits, wrap, direction * delta);
		if (PositionSeekDirection(limits, position, wrap, target) == direction)
			return false;
	}
	position = target;
	return true;
}

//-----------------------------------------------------------------------------
// physics
//-----------------------------------------------------------------------------
struct PhysicsInstance_t
{
	const HaloPhysicsDef* physics;
	HaloMatrix4x3 worldMatrix;
};

HaloVec3 NegatedCenterOfMass(const HaloPhysicsDef& physics)
{
	return { -physics.centerOfMass.i, -physics.centerOfMass.j, -physics.centerOfMass.k };
}

float EffectiveGravity(const HaloPhysicsDef& physics, const HaloVehicleState& state)
{
	float gravity = physics.gravityScale * kGlobalGravity;
	if (physics.zeroGravitySpeed > 0.0f || physics.fullGravitySpeed > 0.0f)
		gravity *= PinFraction(Magnitude(state.translationalVelocity), physics.zeroGravitySpeed, physics.fullGravitySpeed);
	return gravity;
}

void PhysicsInstanceNew(PhysicsInstance_t& instance, const HaloPhysicsDef& physics, const HaloVehicleState& state)
{
	instance.physics = &physics;
	instance.worldMatrix = FromPointAndVectors(state.position, state.forward, state.up);
	instance.worldMatrix.position = TransformPoint(instance.worldMatrix, NegatedCenterOfMass(physics));
}

void ComputeGroundPlane(IHaloWorld& world, MassPointState_t& mp, const HaloMassPointDef& def)
{
	mp.groundPlane = kDepthsOfHell;
	mp.groundDepth = def.radius - PlaneDistance(mp.groundPlane, mp.position);

	HaloSphereContact contact;
	if (world.SphereContact(mp.position, def.radius, contact))
	{
		mp.groundPlane = contact.plane;
		mp.groundDepth = contact.depth;
		if (contact.volatileSurface)
			mp.flags |= POINT_ON_VOLATILE_SURFACE;
		else
			mp.flags &= ~POINT_ON_VOLATILE_SURFACE;
	}
}

void FrictionEvaluate(const int16_t type, const float parallelScale, const float perpendicularScale,
	FrictionDatum_t& friction, const HaloVec3& forward, const HaloVec3& up)
{
	if (type == HALO_FRICTION_POINT)
	{
		friction.parallel = friction.friction;
		friction.perpendicular = kZero;
		return;
	}

	switch (type)
	{
	case HALO_FRICTION_FORWARD:
		ComponentVectorsFromNormal(friction.friction, forward, friction.parallel, friction.perpendicular);
		break;
	case HALO_FRICTION_LEFT:
		ComponentVectorsFromNormal(friction.friction, Cross(up, forward), friction.parallel, friction.perpendicular);
		break;
	case HALO_FRICTION_UP:
		ComponentVectorsFromNormal(friction.friction, up, friction.parallel, friction.perpendicular);
		break;
	default:
		return;
	}

	friction.parallel = Scale(friction.parallel, parallelScale);
	friction.perpendicular = Scale(friction.perpendicular, perpendicularScale);
	friction.friction = Add(friction.parallel, friction.perpendicular);
}

// Per-point environment: ground, water and air friction, lift, thrust, antigrav.
void ComputeMassPoint(IHaloWorld& world, const HaloPhysicsDef& physics, const HaloVehicleState& state,
	const PoweredState_t* poweredStates, const HaloMatrix4x3& worldMatrix, const bool shiftByCom,
	const int index, MassPointState_t& mp)
{
	const HaloMassPointDef& def = physics.massPoints[index];
	const HaloPoweredMassPointDef* poweredDef = nullptr;
	const PoweredState_t* powered = nullptr;
	const float gravity = EffectiveGravity(physics, state);

	if (def.poweredIndex >= 0 && def.poweredIndex < physics.poweredCount && poweredStates)
	{
		poweredDef = &physics.powered[def.poweredIndex];
		powered = &poweredStates[def.poweredIndex];
	}

	mp.flags = 0;
	HaloVec3 localPosition = def.position;
	if (shiftByCom)
		localPosition = Sub(def.position, physics.centerOfMass);
	mp.position = TransformPoint(worldMatrix, localPosition);

	if (powered)
	{
		const HaloMatrix4x3 poweredWorld = Multiply(worldMatrix, powered->rotationMatrix);
		mp.forward = TransformNormal(poweredWorld, def.forward);
		mp.up = TransformNormal(poweredWorld, def.up);
	}
	else
	{
		mp.forward = TransformNormal(worldMatrix, def.forward);
		mp.up = TransformNormal(worldMatrix, def.up);
	}

	mp.radius = Sub(mp.position, state.position);
	mp.velocity = Add(state.translationalVelocity, Cross(state.angularVelocity, mp.radius));

	ComputeGroundPlane(world, mp, def);
	mp.waterDepth = world.WaterDepth(mp.position);

	if (mp.groundDepth > 0.0f && physics.groundDepth > 0.0f)
	{
		const float normalVelocity = Dot(mp.velocity, mp.groundPlane.n);
		const float groundScale = -def.mass * physics.groundFriction;

		mp.normalForceMagnitude = physics.mass *
			(kGlobalGravity / physics.groundDepth * mp.groundDepth - normalVelocity * physics.groundDampFraction);
		if (physics.maximumNormalForceScale > 0.0f)
			mp.normalForceMagnitude = fminf(mp.normalForceMagnitude, physics.maximumNormalForceScale * physics.mass * kGlobalGravity);
		mp.normalForce = Scale(mp.groundPlane.n, mp.normalForceMagnitude);

		mp.velocityRelativeToGround = Add(Scale(mp.groundPlane.n, -normalVelocity), mp.velocity);
		mp.groundFriction.friction = Scale(mp.velocityRelativeToGround, groundScale);

		if (poweredDef && (poweredDef->flags & HALO_POWERED_GROUND_FRICTION) && powered->groundFrictionVelocity != 0.0f)
		{
			const float fraction = PinFraction(mp.groundPlane.n.k, physics.groundNormalK0, physics.groundNormalK1);
			const float alignment = Pin(Dot(mp.up, mp.groundPlane.n), 0.0f, 1.0f);
			const float weight = alignment * alignment * fraction * fraction * groundScale;
			const HaloVec3 poweredVelocity = Scale(mp.forward, -powered->groundFrictionVelocity);
			HaloVec3 projected = Scale(mp.groundPlane.n, -Dot(poweredVelocity, mp.groundPlane.n));

			projected = Add(projected, poweredVelocity);
			mp.velocityRelativeToGround = Add(mp.velocityRelativeToGround, projected);
			mp.groundFriction.friction = Add(mp.groundFriction.friction, Scale(projected, weight));
		}

		FrictionEvaluate(def.frictionType, def.frictionParallelScale, def.frictionPerpendicularScale,
			mp.groundFriction, mp.forward, mp.up);
	}

	if (mp.waterDepth > 0.0f)
	{
		const float depthFraction = (mp.waterDepth < physics.waterDepth) ? mp.waterDepth / physics.waterDepth : 1.0f;
		const float waterScale = -def.mass * physics.waterFriction;

		if (def.density > 0.0f && physics.waterDepth > 0.0f)
		{
			mp.waterPressureMagnitude = def.mass / def.density * physics.waterDensity * depthFraction * gravity;
			mp.waterPressure = { 0.0f, 0.0f, mp.waterPressureMagnitude };
		}

		if (poweredDef && (poweredDef->flags & HALO_POWERED_WATER_FRICTION) && powered->waterFrictionVelocity != 0.0f)
		{
			const HaloVec3 poweredVelocity = Add(Scale(mp.forward, -powered->waterFrictionVelocity), mp.velocity);
			mp.waterFriction.friction = Scale(poweredVelocity, waterScale);
		}
		else
			mp.waterFriction.friction = Scale(mp.velocity, waterScale);

		FrictionEvaluate(def.frictionType, def.frictionParallelScale, def.frictionPerpendicularScale,
			mp.waterFriction, mp.forward, mp.up);

		if (poweredDef && (poweredDef->flags & HALO_POWERED_WATER_LIFT) && powered->waterLiftRatio != 0.0f)
		{
			const float lift = fabsf(Dot(mp.forward, mp.velocity)) * powered->waterLiftRatio * physics.mass * depthFraction;
			mp.poweredForce = Add(mp.poweredForce, Scale(mp.up, lift));
		}
	}

	{
		const float airScale = -def.mass * physics.airFriction;

		if (poweredDef && (poweredDef->flags & HALO_POWERED_AIR_FRICTION) && powered->airFrictionVelocity != 0.0f)
		{
			const HaloVec3 poweredVelocity = Add(Scale(mp.forward, -powered->airFrictionVelocity), mp.velocity);
			mp.airFriction.friction = Scale(poweredVelocity, airScale);
		}
		else
			mp.airFriction.friction = Scale(mp.velocity, airScale);

		FrictionEvaluate(def.frictionType, def.frictionParallelScale, def.frictionPerpendicularScale,
			mp.airFriction, mp.forward, mp.up);

		if (poweredDef && (poweredDef->flags & HALO_POWERED_AIR_LIFT) && powered->airLiftRatio != 0.0f)
		{
			const float lift = fabsf(Dot(mp.forward, mp.velocity)) * physics.mass * powered->airLiftRatio;
			mp.poweredForce = Add(mp.poweredForce, Scale(mp.up, lift));
		}
	}

	if (MagnitudeSquared(mp.velocity) < 0.0011111111f)
		mp.flags |= POINT_AT_REST;
	if (mp.groundDepth > 0.0f)
		mp.flags |= POINT_ON_GROUND;
	if (mp.waterDepth > 0.0f)
		mp.flags |= POINT_IN_WATER;

	if (poweredDef)
	{
		if (poweredDef->flags & HALO_POWERED_THRUST)
			mp.poweredForce = Add(mp.poweredForce, Scale(mp.forward, powered->thrustFraction * physics.mass));

		if (poweredDef->flags & HALO_POWERED_ANTIGRAV)
		{
			const float probeLength = def.radius + poweredDef->antigravHeight;
			HaloTraceHit hit;

			if (world.TestVector(mp.position, Scale(kDown, probeLength), hit))
			{
				const float height = probeLength * hit.t - def.radius;
				const float alignment = PinFraction(mp.up.k, poweredDef->antigravNormalK0, poweredDef->antigravNormalK1);
				const float groundEffect = height > 0.0f ? 1.0f - height / poweredDef->antigravHeight : 1.0f;
				const float magnitude = (groundEffect * groundEffect * kGlobalGravity -
					Dot(hit.plane.n, mp.velocity) * poweredDef->antigravDampFraction) *
					powered->antigravFraction * poweredDef->antigravStrength * physics.mass * alignment;

				mp.poweredForce = Add(mp.poweredForce, Scale(hit.plane.n, magnitude));
				mp.flags |= POINT_ANTIGRAVING;
			}
		}
	}

	mp.force = Add(mp.normalForce, mp.groundFriction.friction);
	mp.force = Add(mp.force, mp.waterPressure);
	mp.force = Add(mp.force, mp.waterFriction.friction);
	mp.force = Add(mp.force, mp.airFriction.friction);
	mp.force = Add(mp.force, mp.poweredForce);
	mp.torque = Cross(mp.radius, mp.force);
}

void PhysicsComputeNew(IHaloWorld& world, const PhysicsInstance_t& instance, const HaloVehicleState& state,
	const PoweredState_t* poweredStates, MassPointState_t* massPoints, HaloVec3& totalForce, HaloVec3& totalTorque)
{
	const HaloPhysicsDef& physics = *instance.physics;
	const float gravity = EffectiveGravity(physics, state);

	totalForce = { 0.0f, 0.0f, -physics.mass * gravity };
	totalTorque = kZero;
	memset(massPoints, 0, sizeof(MassPointState_t) * physics.massPointCount);

	for (int i = 0; i < physics.massPointCount; ++i)
	{
		ComputeMassPoint(world, physics, state, poweredStates, instance.worldMatrix, false, i, massPoints[i]);
		totalForce = Add(totalForce, massPoints[i].force);
		totalTorque = Add(totalTorque, massPoints[i].torque);
	}
}

void RotateVectorsByAngularVelocity(const HaloVec3& forward, const HaloVec3& up, const HaloVec3& angularVelocity,
	HaloVec3& rotatedForward, HaloVec3& rotatedUp)
{
	HaloVec3 axis = angularVelocity;
	const float magnitude = Normalize(axis);

	if (magnitude != 0.0f)
	{
		const HaloMatrix4x3 rotation = RotationFromAxisAndAngle(axis, sinf(magnitude), cosf(magnitude));
		rotatedForward = TransformVector(rotation, forward);
		rotatedUp = TransformVector(rotation, up);
		Normalize(rotatedForward);

		const float dot = -Dot(rotatedUp, rotatedForward);
		rotatedUp = Add(rotatedUp, Scale(rotatedForward, dot));
		Normalize(rotatedUp);
	}
	else
	{
		rotatedForward = forward;
		rotatedUp = up;
	}
}

void SetPosition(HaloVehicleState& state, const HaloVec3& position, const HaloVec3& forward, const HaloVec3& up)
{
	state.position = position;
	state.forward = forward;
	state.up = up;
}

void UpdateObjectFlags(HaloVehicleState& state, const HaloPhysicsDef& physics, const MassPointState_t* massPoints,
	const HaloVec3& linearVelocity, const HaloVec3& angularVelocity,
	const HaloVec3& linearAcceleration, const HaloVec3& angularAcceleration)
{
	int atRest = 0, onGround = 0, onVolatile = 0, inWater = 0;
	state.groundedMassPointFlags = 0;
	state.antigravMassPointFlags = 0;

	for (int i = 0; i < physics.massPointCount; ++i)
	{
		const uint32_t f = massPoints[i].flags;
		atRest += (f & POINT_AT_REST) ? 1 : 0;
		onGround += (f & POINT_ON_GROUND) ? 1 : 0;
		onVolatile += (f & POINT_ON_VOLATILE_SURFACE) ? 1 : 0;
		inWater += (f & POINT_IN_WATER) ? 1 : 0;
		if (f & POINT_ON_GROUND)
			state.groundedMassPointFlags |= 1u << i;
		if (f & POINT_ANTIGRAVING)
			state.antigravMassPointFlags |= 1u << i;
	}

	const bool rest = atRest == physics.massPointCount && onGround >= 3 && onVolatile == 0 &&
		MagnitudeSquared(linearVelocity) <= 0.0011111111f &&
		MagnitudeSquared(angularVelocity) <= 0.0027415568f &&
		MagnitudeSquared(linearAcceleration) <= 0.00000030864197f &&
		MagnitudeSquared(angularAcceleration) <= 0.0000030461742f;

	auto setFlag = [&state](const uint32_t bit, const bool on)
	{
		if (on)
			state.objectFlags |= bit;
		else
			state.objectFlags &= ~bit;
	};

	setFlag(HALO_OBJ_AT_REST, rest);
	setFlag(HALO_OBJ_ON_GROUND, onGround > 0);
	setFlag(HALO_OBJ_ON_MEDIA, inWater > 0);
	setFlag(HALO_OBJ_PARTIALLY_UNDER_MEDIA, inWater > 0);
	setFlag(HALO_OBJ_WHOLLY_UNDER_MEDIA, inWater == physics.massPointCount);
}

void PhysicsUpdateNew(IHaloWorld& world, const PhysicsInstance_t& instance, HaloVehicleState& state,
	const MassPointState_t* massPoints, const HaloVec3& totalForce, const HaloVec3& totalTorque)
{
	const HaloPhysicsDef& physics = *instance.physics;
	if (!(physics.mass > 0.0f))
		return;

	const HaloVec3 linearAcceleration = Scale(totalForce, 1.0f / physics.mass);
	HaloVec3 linearVelocity = Add(linearAcceleration, state.translationalVelocity);
	HaloVec3 position = Add(state.position, linearVelocity);

	HaloVec3 angularAcceleration;
	{
		const HaloMatrix3x3 frame = Matrix3x3FromForwardAndUp(state.forward, state.up);
		HaloMatrix3x3 worldInverseInertia = Matrix3x3Multiply(frame, physics.inverseInertia);
		worldInverseInertia = Matrix3x3Multiply(worldInverseInertia, Matrix3x3Transpose(frame));
		angularAcceleration = Matrix3x3TransformVector(worldInverseInertia, totalTorque);
	}

	HaloVec3 angularVelocity = Add(angularAcceleration, state.angularVelocity);
	HaloVec3 forward, up;
	RotateVectorsByAngularVelocity(state.forward, state.up, angularVelocity, forward, up);

	state.translationalVelocity = linearVelocity;
	state.angularVelocity = angularVelocity;

	// Penetration freeze: sweep each mass point to its new spot, back off on the earliest hit.
	uint32_t stuckFlags = 0;
	for (int passesRemaining = 4; passesRemaining-- > 0;)
	{
		bool foundCollision = false;
		HaloVec3 worstDelta = kZero;
		HaloTraceHit worstHit = {};

		stuckFlags = 0;
		HaloMatrix4x3 worldMatrix = FromPointAndVectors(position, forward, up);
		worldMatrix.position = TransformPoint(worldMatrix, NegatedCenterOfMass(physics));

		for (int i = 0; i < physics.massPointCount; ++i)
		{
			const HaloVec3 swept = TransformPoint(worldMatrix, physics.massPoints[i].position);
			const HaloVec3 delta = Sub(swept, massPoints[i].position);
			HaloTraceHit hit;

			if (world.TestVector(massPoints[i].position, delta, hit))
			{
				stuckFlags |= 1u << i;
				if (!foundCollision || worstHit.t > hit.t)
				{
					foundCollision = true;
					worstDelta = delta;
					worstHit = hit;
				}
			}
		}

		if (!foundCollision)
		{
			SetPosition(state, position, forward, up);
			break;
		}

		const float normalDotDelta = Dot(worstDelta, worstHit.plane.n);
		const float epsilon = (normalDotDelta != 0.0f) ? 0.0078125f / fabsf(normalDotDelta) : 0.03125f;
		const float t = fmaxf(worstHit.t - epsilon, 0.0f);
		const float normalDotVelocity = Dot(worstHit.plane.n, linearVelocity);

		if (normalDotVelocity < 0.0f)
		{
			linearVelocity = PointFromLine(linearVelocity, worstHit.plane.n, (t - 1.0f) * normalDotVelocity);
			state.translationalVelocity = linearVelocity;
			position = Add(state.position, linearVelocity);
		}

		angularVelocity = Scale(angularVelocity, t);
		state.angularVelocity = angularVelocity;
		RotateVectorsByAngularVelocity(state.forward, state.up, angularVelocity, forward, up);
	}
	state.stuckMassPointFlags = stuckFlags;

	UpdateObjectFlags(state, physics, massPoints, linearVelocity, angularVelocity, linearAcceleration, angularAcceleration);
}

// Scalar-moment integrator, used when the definition has a radius.
void PhysicsUpdateOld(IHaloWorld& world, const HaloPhysicsDef& physics, HaloVehicleState& state,
	PoweredState_t* poweredStates, MassPointState_t* massPoints, const HaloVec3* magicForce, const HaloVec3* magicTorque)
{
	const float gravity = EffectiveGravity(physics, state);
	const HaloMatrix4x3 worldMatrix = FromPointAndVectors(state.position, state.forward, state.up);
	HaloVec3 totalForce = { 0.0f, 0.0f, -physics.mass * gravity };
	HaloVec3 totalTorque = kZero;
	HaloVec3 translationalAcceleration = kZero;
	HaloVec3 angularAcceleration = kZero;

	if (poweredStates)
	{
		for (int i = 0; i < physics.poweredCount; ++i)
		{
			poweredStates[i].rotationMatrix = RotationFromQuaternion(poweredStates[i].rotation);
			Transpose(poweredStates[i].rotationMatrix);
		}
	}

	memset(massPoints, 0, sizeof(MassPointState_t) * physics.massPointCount);

	if (magicForce)
		totalForce = Add(totalForce, *magicForce);
	if (magicTorque)
		totalTorque = *magicTorque;

	for (int i = 0; i < physics.massPointCount; ++i)
	{
		ComputeMassPoint(world, physics, state, poweredStates, worldMatrix, true, i, massPoints[i]);
		totalForce = Add(totalForce, massPoints[i].force);
		totalTorque = Add(totalTorque, massPoints[i].torque);
	}

	if (physics.mass != 0.0f)
		translationalAcceleration = Scale(totalForce, 1.0f / physics.mass);

	{
		HaloVec3 torqueAxis = totalTorque;
		if (Normalize(torqueAxis) != 0.0f)
		{
			float momentOfInertia = 0.0f;
			for (int i = 0; i < physics.massPointCount; ++i)
			{
				const HaloMassPointDef& def = physics.massPoints[i];
				const float projection = -Dot(massPoints[i].radius, torqueAxis);
				const HaloVec3 perpendicularRadius = Add(Scale(torqueAxis, projection), massPoints[i].radius);
				momentOfInertia += (MagnitudeSquared(perpendicularRadius) + def.radius * def.radius * 0.4f) *
					def.mass * physics.moment;
			}
			if (momentOfInertia != 0.0f)
				angularAcceleration = Scale(totalTorque, 1.0f / momentOfInertia);
		}
	}

	state.translationalVelocity = Add(state.translationalVelocity, translationalAcceleration);
	state.angularVelocity = Add(state.angularVelocity, angularAcceleration);
	state.position = Add(state.position, state.translationalVelocity);

	HaloVec3 rotationAxis = state.angularVelocity;
	const float angularSpeed = Normalize(rotationAxis);
	if (angularSpeed != 0.0f)
	{
		const float s = sinf(angularSpeed);
		const float c = cosf(angularSpeed);
		RotateVectorAboutAxis(state.forward, rotationAxis, s, c);
		RotateVectorAboutAxis(state.up, rotationAxis, s, c);
		Normalize(state.forward);
		const float orthogonalization = -Dot(state.up, state.forward);
		state.up = Add(state.up, Scale(state.forward, orthogonalization));
		Normalize(state.up);
	}

	UpdateObjectFlags(state, physics, massPoints, state.translationalVelocity, state.angularVelocity,
		translationalAcceleration, angularAcceleration);
}

void PhysicsUpdate(IHaloWorld& world, const HaloPhysicsDef& physics, HaloVehicleState& state,
	PoweredState_t* poweredStates, MassPointState_t* massPoints, const HaloVec3* magicForce, const HaloVec3* magicTorque)
{
	if (physics.radius > 0.0f)
	{
		PhysicsUpdateOld(world, physics, state, poweredStates, massPoints, magicForce, magicTorque);
		return;
	}

	PhysicsInstance_t instance;
	PhysicsInstanceNew(instance, physics, state);

	if (poweredStates)
	{
		for (int i = 0; i < physics.poweredCount; ++i)
		{
			poweredStates[i].rotationMatrix = RotationFromQuaternion(poweredStates[i].rotation);
			Transpose(poweredStates[i].rotationMatrix);
		}
	}

	HaloVec3 totalForce, totalTorque;
	PhysicsComputeNew(world, instance, state, poweredStates, massPoints, totalForce, totalTorque);

	totalForce = Add(totalForce, state.collisionForce);
	totalTorque = Add(totalTorque, state.collisionTorque);
	state.collisionForce = kZero;
	state.collisionTorque = kZero;

	if (magicForce)
		totalForce = Add(totalForce, *magicForce);
	if (magicTorque)
		totalTorque = Add(totalTorque, *magicTorque);

	PhysicsUpdateNew(world, instance, state, massPoints, totalForce, totalTorque);
}

//-----------------------------------------------------------------------------
// vehicle type updates
//-----------------------------------------------------------------------------
void ResetPoweredStates(PoweredState_t* states, const int count)
{
	memset(states, 0, sizeof(PoweredState_t) * count);
	for (int i = 0; i < count; ++i)
		states[i].rotation = kIdentityQuat;
}

// Quaternion about local up for a rotation of angle radians.
HaloQuat YawQuaternion(const float angle)
{
	return { { 0.0f, 0.0f, sinf(angle * 0.5f) }, cosf(angle * 0.5f) };
}

void WrapWheel(float& wheel, const float delta, const float circumference)
{
	if (!(circumference > 0.0f))
		return;
	wheel = fmodf(wheel + delta, circumference);
	if (wheel < 0.0f)
		wheel += circumference;
}

void UpdateHumanJeep(IHaloWorld& world, const HaloVehicleDef& def, HaloVehicleState& state,
	PoweredState_t* powered, MassPointState_t* massPoints)
{
	const HaloPhysicsDef& physics = def.physics;

	WrapWheel(state.wheel, state.speed, def.wheelCircumference);
	WrapWheel(state.wheelRear, (state.flags & HALO_VEH_BRAKE) ? 0.0f : state.speed, def.wheelCircumference);

	if (physics.poweredCount >= 2)
	{
		for (int i = 0; i < physics.poweredCount; ++i)
		{
			powered[i].groundFrictionVelocity = state.speed;
			powered[i].rotation = YawQuaternion(state.turn * physics.powered[i].steerFactor);
		}
		PhysicsUpdate(world, physics, state, powered, massPoints, nullptr, nullptr);
	}
	else
		PhysicsUpdate(world, physics, state, nullptr, massPoints, nullptr, nullptr);
}

void UpdateHumanTank(IHaloWorld& world, const HaloVehicleDef& def, HaloVehicleState& state,
	PoweredState_t* powered, MassPointState_t* massPoints)
{
	const HaloPhysicsDef& physics = def.physics;
	const float left = state.speed - state.turn;
	const float right = state.turn + state.speed;

	WrapWheel(state.leftTread, left, def.wheelCircumference);
	WrapWheel(state.rightTread, right, def.wheelCircumference);

	if (physics.poweredCount == 2)
	{
		powered[0].groundFrictionVelocity = left;
		powered[0].rotation = kIdentityQuat;
		powered[1].groundFrictionVelocity = right;
		powered[1].rotation = kIdentityQuat;
		PhysicsUpdate(world, physics, state, powered, massPoints, nullptr, nullptr);
	}
	else
		PhysicsUpdate(world, physics, state, nullptr, massPoints, nullptr, nullptr);
}

// gravity is the acceleration the vehicle actually feels, so the command cancels it exactly.
HaloVec3 ComputeAcceleration(const HaloVec3& a, const HaloVec3& b, float maximum, const float minimum, const float gravity)
{
	HaloVec3 result = Sub(a, b);
	result.k += gravity;

	const float dot = Dot(a, result);
	if (dot > kRealEpsilon)
		maximum = (maximum - minimum) * ((dot * dot / MagnitudeSquared(result)) / MagnitudeSquared(a)) + minimum;
	else
		maximum = minimum;

	Limit(result, maximum);
	return result;
}

void UpdateAlienFighterNew(IHaloWorld& world, const HaloVehicleDef& def, HaloVehicleState& state,
	PoweredState_t* powered, MassPointState_t* massPoints)
{
	const HaloPhysicsDef& physics = def.physics;

	if (physics.poweredCount != 2)
	{
		PhysicsUpdate(world, physics, state, nullptr, massPoints, nullptr, nullptr);
		return;
	}

	HaloVec3 magicForce;
	{
		const HaloVec3 desiredVelocity = Scale(state.forward, state.speed);
		const float throttle = (state.speed > 0.0f) ? state.speed / def.speed.positiveScale
			: -(state.speed / def.speed.negativeScale);
		const HaloVec3 acceleration = ComputeAcceleration(desiredVelocity, state.translationalVelocity,
			throttle * def.speed.acceleration, throttle * def.speed.deceleration, EffectiveGravity(physics, state));
		magicForce = Scale(Scale(acceleration, physics.mass), state.seatPower[0]);
	}

	HaloVec3 desiredAngularVelocity;
	{
		const HaloMatrix3x3 current = Matrix3x3FromForwardAndUp(state.forward, state.up);
		HaloMatrix3x3 desired;
		desired.forward = state.desiredFacing;
		desired.up = PointFromLine(kUp, desired.forward, -desired.forward.k);
		if (Normalize(desired.up) == 0.0f)
			desired.up = kForward;

		PitchVectors(desired.forward, desired.up, sinf(def.fighterPitch), cosf(def.fighterPitch));

		const float yaw = (desired.forward.i * state.translationalVelocity.j - desired.forward.j * state.translationalVelocity.i) /
			def.speed.positiveScale * def.maximumLeftTurn;
		YawVectors(desired.up, desired.forward, sinf(yaw), cosf(yaw));
		desired.left = Cross(desired.up, desired.forward);

		const HaloMatrix3x3 rotation = Matrix3x3Multiply(desired, Matrix3x3Transpose(current));
		float angle;
		HaloVec3 axis;
		QuaternionToAngleAndVector(RotationToQuaternion(rotation), angle, axis);
		desiredAngularVelocity = Scale(axis, (-angle) * def.turnRate * (1.0f / kPi));
	}

	const HaloVec3 angularAcceleration = Sub(desiredAngularVelocity, state.angularVelocity);
	const HaloVec3 magicTorque = Scale(Scale(angularAcceleration,
		(physics.zzMoment + physics.yyMoment + physics.xxMoment) * (1.0f / 3.0f)), state.seatPower[0]);

	const float spin = Magnitude(state.angularVelocity) / def.turnRate;
	float thrustDelta;
	if (spin > state.thrust)
	{
		thrustDelta = Pin((1.0f - state.thrust) * (1.0f - state.thrust) * 0.2f, 0.01f, 0.05f);
		thrustDelta = fminf(spin - state.thrust, thrustDelta);
	}
	else
	{
		thrustDelta = -fmaxf(state.thrust * state.thrust * 0.05f, 0.005f);
		thrustDelta = fmaxf(spin - state.thrust, thrustDelta);
	}
	state.thrust += thrustDelta;

	for (int i = 0; i < 2; ++i)
	{
		powered[i].antigravFraction = state.seatPower[0];
		powered[i].rotation = kIdentityQuat;
	}

	PhysicsUpdate(world, physics, state, powered, massPoints, &magicForce, &magicTorque);
}

void UpdateAlienFighterOld(IHaloWorld& world, const HaloVehicleDef& def, HaloVehicleState& state,
	PoweredState_t* powered, MassPointState_t* massPoints)
{
	const HaloPhysicsDef& physics = def.physics;

	if (physics.poweredCount != 2)
	{
		PhysicsUpdate(world, physics, state, nullptr, massPoints, nullptr, nullptr);
		return;
	}

	HaloVec3 facing = state.desiredFacing;
	HaloVec3 perpendicular = { -facing.k * facing.i, -(facing.k * facing.j), 1.0f - facing.k * facing.k };
	if (Normalize(perpendicular) == 0.0f)
		perpendicular = { 1.0f, 0.0f, 0.0f };

	const float speed = Dot(state.forward, state.translationalVelocity);
	const float thrust = (state.speed - speed) * physics.mass * 0.05f;
	const float lift = (fabsf(speed / def.speed.positiveScale) * physics.mass) * kGlobalGravity * 1.05f;
	HaloVec3 force = Add(Scale(state.up, lift), Scale(state.forward, thrust));

	const float yaw = (facing.i * state.translationalVelocity.j - facing.j * state.translationalVelocity.i) *
		(kPi * 0.5f) / fabsf(def.speed.positiveScale);
	YawVectors(perpendicular, facing, sinf(yaw), cosf(yaw));

	HaloVec3 scaled;
	{
		const HaloMatrix4x3 actual = RotationFromVectors(state.forward, state.up);
		const HaloMatrix4x3 desired = Inverse(RotationFromVectors(facing, perpendicular));
		const HaloMatrix4x3 difference = Multiply(actual, desired);
		float angle;
		HaloVec3 axis;
		QuaternionToAngleAndVector(RotationToQuaternion(difference), angle, axis);
		scaled = Scale(axis, angle * (4.0f / 30.0f));
	}

	const float scale = physics.radius * physics.radius * physics.mass * 0.05f;
	HaloVec3 torque = Scale(Sub(scaled, state.angularVelocity), scale);

	for (int i = 0; i < 2; ++i)
	{
		powered[i].antigravFraction = state.seatPower[0];
		powered[i].rotation = kIdentityQuat;
	}

	force = Scale(force, state.seatPower[0]);
	torque = Scale(torque, state.seatPower[0]);
	PhysicsUpdate(world, physics, state, powered, massPoints, &force, &torque);
}

void UpdateAlienScout(IHaloWorld& world, const HaloVehicleDef& def, HaloVehicleState& state, const float steering,
	PoweredState_t* powered, MassPointState_t* massPoints)
{
	const HaloPhysicsDef& physics = def.physics;
	const float waterDepth = world.WaterDepth(state.position);
	HaloVec3 magicForce = kZero;
	HaloVec3 magicTorque = kZero;
	const float antigrav = state.seatPower[0];

	for (int i = 0; i < physics.poweredCount; ++i)
	{
		powered[i].antigravFraction = antigrav;
		powered[i].rotation = kIdentityQuat;
	}

	if (waterDepth < 0.5f && state.up.k > -0.2f)
	{
		const HaloVec3& forward = state.forward;
		const HaloVec3& up = state.up;
		const HaloVec3& angularVelocity = state.angularVelocity;
		const HaloMatrix4x3 vehicleMatrix = FromPointAndVectors(state.position, forward, up);
		const HaloVec3 localVelocity = InverseTransformVector(vehicleMatrix, state.translationalVelocity);

		if (state.hover > 0.0f)
		{
			float maximumSpeed = def.speed.positiveScale;
			if (state.flags & HALO_VEH_BRAKE)
				maximumSpeed *= 0.8f;
			float maximumAcceleration = def.speed.acceleration;

			HaloVec3 acceleration = {
				maximumSpeed * state.throttleForward - localVelocity.i,
				maximumSpeed * state.throttleLeft - localVelocity.j,
				0.0f };

			if (state.onGroundTicks > 0 && fabsf(steering) > 0.785398185f)
				maximumAcceleration *= 1.0f - fminf(state.onGroundTicks * 0.05f, 0.98f);

			Limit(acceleration, maximumAcceleration);
			acceleration = TransformVector(vehicleMatrix, acceleration);
			magicForce = Add(magicForce, Scale(acceleration, physics.mass * state.hover));
		}

		if (state.hover > 0.0f)
		{
			const float current = Dot(up, angularVelocity);
			const float sign = steering != 0.0f ? (steering < 0.0f ? -1.0f : 1.0f) : 0.0f;
			float desired = sqrtf(fabsf(steering) * 0.0069813174f) * sign;
			if (fabsf(desired) > kRealEpsilon && steering / desired < 2.0f)
				desired = steering * 0.5f;

			const float error = Pin(desired - current, -0.0034906587f, 0.0034906587f);
			const float torque = error * physics.zzMoment * state.hover;
			magicTorque = Add(magicTorque, Scale(up, torque));
		}

		if (state.hover < 1.0f)
		{
			const HaloVec3 left = Cross(up, forward);
			float forward2[2] = { forward.i, forward.j };
			float left2[2] = { left.i, left.j };
			auto normalize2 = [](float* v)
			{
				const float m = sqrtf(v[0] * v[0] + v[1] * v[1]);
				if (!(kRealEpsilon > fabsf(m)))
				{
					v[0] /= m;
					v[1] /= m;
				}
			};
			normalize2(forward2);
			normalize2(left2);

			float controlTorque[2] = { 0.0f, 0.0f };
			float torqueA, torqueB;

			if (up.k > 0.0f)
			{
				const float upv[2] = { up.i, up.j };
				const float av[2] = { angularVelocity.i, angularVelocity.j };
				const float alignment[2] = { upv[0] * forward2[0] + upv[1] * forward2[1], upv[0] * left2[0] + upv[1] * left2[1] };
				const float rate[2] = { av[0] * left2[0] + av[1] * left2[1], -(av[0] * forward2[0] + av[1] * forward2[1]) };
				float level[2] = { -alignment[0] - 15.0f * rate[0], -alignment[1] - 15.0f * rate[1] };

				const float pa = state.throttleForward * level[0];
				const float pb = state.throttleLeft * level[1];
				const float weightA = fabsf(level[0]) * (pa != 0.0f ? (pa < 0.0f ? -1.0f : 1.0f) : 0.0f);
				const float weightB = fabsf(level[1]) * (pb != 0.0f ? (pb < 0.0f ? -1.0f : 1.0f) : 0.0f);
				controlTorque[0] += state.throttleForward * Pin(weightA + 1.0f, 0.3f, 2.5f) * 0.0015514038f;
				controlTorque[1] += state.throttleLeft * Pin(weightB + 1.0f, 0.3f, 2.5f) * 0.0015514038f;

				const float levelScale = (1.0f - up.k) * 0.0038785094f;
				torqueA = levelScale * level[0] + controlTorque[0];
				torqueB = levelScale * level[1];
			}
			else
			{
				torqueA = state.throttleForward * 0.0015514038f + controlTorque[0];
				torqueB = state.throttleLeft * 0.0015514038f;
			}
			torqueB += controlTorque[1];

			const float leftScale = physics.yyMoment * torqueA;
			const float forwardScale = -(physics.xxMoment * torqueB);
			const HaloVec3 torque = Add(Scale(left, leftScale), Scale(forward, forwardScale));
			magicTorque = Add(magicTorque, Scale(torque, 1.0f - state.hover));
		}

		if (state.flags & HALO_VEH_BRAKE)
		{
			const float speed = Pin(Dot(forward, state.translationalVelocity) / def.speed.positiveScale, 0.0f, 1.0f);
			const HaloVec3 left = Cross(up, forward);

			if (speed > 0.0f)
			{
				magicTorque = Add(magicTorque, Scale(left, physics.yyMoment * speed * state.hover * -0.005817764f));
				magicForce = Add(magicForce, Scale(kUp, physics.mass * speed * state.hover * 0.004f));
			}

			if (state.airborneTicks > 0)
			{
				HaloVec3 axis = Cross(left, kUp);
				if (Normalize(axis) > 0.0f)
				{
					const float fade = Pin(1.0f - state.airborneTicks * (1.0f / 30.0f), 0.0f, 1.0f);
					magicForce = Add(magicForce, Scale(axis, (1.0f - state.hover) * physics.mass * fade * 0.002f));
					magicForce = Add(magicForce, Scale(kUp, (1.0f - state.hover) * physics.mass * fade * 0.001f));
				}
			}
		}

		magicForce = Scale(magicForce, antigrav);
		magicTorque = Scale(magicTorque, antigrav);
	}

	PhysicsUpdate(world, physics, state, powered, massPoints, &magicForce, &magicTorque);

	{
		const float maximum = fmaxf(state.up.k, 0.4f);
		int poweredCount = 0, groundedCount = 0;
		for (int i = 0; i < physics.massPointCount; ++i)
		{
			if (physics.massPoints[i].poweredIndex >= 0)
			{
				++poweredCount;
				if (massPoints[i].flags & POINT_ANTIGRAVING)
					++groundedCount;
			}
		}

		const float ratio = poweredCount > 0 ? static_cast<float>(groundedCount) / static_cast<float>(poweredCount) : 0.0f;
		float target = Pin(ratio * maximum, 0.0f, 1.0f);
		if (target - state.hover > 0.1f)
			target = state.hover + 0.1f;
		else if (target - state.hover < -0.1f)
			target = state.hover - 0.1f;
		state.hover = target;
	}
}

void UpdateHumanPlane(IHaloWorld& world, const HaloVehicleDef& def, HaloVehicleState& state, MassPointState_t* massPoints)
{
	const HaloPhysicsDef& physics = def.physics;

	if (state.flags & HALO_VEH_HOVERING)
	{
		memset(massPoints, 0, physics.massPointCount * sizeof(MassPointState_t));
		return;
	}

	float throttle = Pin(state.speed, 0.0f, def.speed.positiveScale) / def.speed.positiveScale;
	throttle = throttle * throttle;

	float factor = !(state.flags & HALO_VEH_CROUCH) ? ((state.flags & HALO_VEH_BRAKE) ? 1.0f : 0.75f) : 0.25f;
	factor *= 1.0f - throttle;
	state.hover += Pin(factor * state.seatPower[0] - state.hover, -0.05f, 0.05f);
	state.thrust = throttle * state.seatPower[0];

	HaloVec3 desiredForward = state.desiredFacing;
	HaloVec3 desiredUp = { -(desiredForward.k * desiredForward.i), -(desiredForward.k * desiredForward.j),
		1.0f - desiredForward.k * desiredForward.k };
	if (Normalize(desiredUp) == 0.0f)
		desiredUp = { 1.0f, 0.0f, 0.0f };

	HaloVec3 force;
	{
		const float dot = Dot(state.translationalVelocity, state.forward);
		const float drive = (state.speed - dot) * state.thrust * physics.mass * 0.05f;
		const float lift = (fabsf(dot / def.speed.positiveScale) * 1.05f + state.hover * 1.3f) * kGlobalGravity * physics.mass;
		force = Add(Scale(state.up, lift), Scale(state.forward, drive));
	}

	{
		const float yaw = (state.translationalVelocity.j * desiredForward.i - state.translationalVelocity.i * desiredForward.j) *
			(kPi / 2.0f) / fabsf(def.speed.positiveScale);
		YawVectors(desiredUp, desiredForward, sinf(yaw), cosf(yaw));
	}

	HaloVec3 torque;
	{
		const HaloMatrix4x3 vehicleRotation = RotationFromVectors(state.forward, state.up);
		const HaloMatrix4x3 desiredRotation = Inverse(RotationFromVectors(desiredForward, desiredUp));
		float angle;
		HaloVec3 axis;
		QuaternionToAngleAndVector(RotationToQuaternion(Multiply(vehicleRotation, desiredRotation)), angle, axis);
		const HaloVec3 scaled = Scale(axis, angle * (1.0f / 30.0f));
		const float scale = physics.radius * physics.radius * physics.mass * 0.05f;
		torque = Scale(Sub(scaled, state.angularVelocity), scale);
	}

	force = Scale(force, state.seatPower[0]);
	torque = Scale(torque, state.seatPower[0]);
	PhysicsUpdate(world, physics, state, nullptr, massPoints, &force, &torque);
}

void UpdateHumanBoat(IHaloWorld& world, const HaloVehicleDef& def, HaloVehicleState& state,
	PoweredState_t* powered, MassPointState_t* massPoints)
{
	const HaloPhysicsDef& physics = def.physics;

	if (physics.poweredCount != 3)
	{
		PhysicsUpdate(world, physics, state, nullptr, massPoints, nullptr, nullptr);
		return;
	}

	const float speed = fabsf(Magnitude(state.translationalVelocity) * 2.5f);
	const float maximumAngle = state.turn * 0.5f;
	const float angle = (1.0f - fminf(speed, 1.0f)) * maximumAngle;

	powered[0].waterFrictionVelocity = state.speed;
	powered[0].waterLiftRatio = 0.003f;
	powered[0].rotation = { { 0.0f, 0.0f, sinf(angle) }, cosf(angle) };
	powered[1].waterLiftRatio = 0.003f;
	powered[1].rotation = kIdentityQuat;
	powered[2].waterLiftRatio = 0.005f;
	powered[2].rotation = kIdentityQuat;

	HaloVec3 thrust = kZero;
	const float negativeK = -state.forward.k;
	HaloVec3 upRelative = Add(Scale(state.forward, negativeK), kUp);

	if (Normalize(upRelative) != 0.0f)
	{
		const HaloVec3 cross = Cross(state.up, state.forward);
		const float spin = Dot(Cross(state.forward, state.translationalVelocity), kUp) * 2.0f * kPi;
		RotateVectorAboutAxis(upRelative, state.forward, sinf(spin), cosf(spin));

		float rollAngle = AngleBetweenVectors(upRelative, state.up);
		if (Dot(cross, upRelative) > 0.0f)
			rollAngle = -rollAngle;

		const float dot = Dot(state.angularVelocity, state.forward);
		const float sign = rollAngle != 0.0f ? (rollAngle < 0.0f ? -1.0f : 1.0f) : 0.0f;
		thrust = Scale(state.forward,
			Pin(sqrtf(fabsf(rollAngle) * 2.0f * kVehicleAngularAcceleration) * sign - dot,
				-kVehicleAngularAcceleration, kVehicleAngularAcceleration) * physics.xxMoment);
	}

	const HaloVec3 zero = kZero;
	PhysicsUpdate(world, physics, state, powered, massPoints, &zero, &thrust);
}

// Visual suspension: traces each wheel's travel and stores compression in [0, 255].
bool UpdateSuspension(IHaloWorld& world, const HaloVehicleDef& def, HaloVehicleState& state)
{
	const HaloPhysicsDef& physics = def.physics;
	const HaloMatrix4x3 matrix = FromPointAndVectors(state.position, state.forward, state.up);
	float maximumShift = 0.0f;

	for (int i = 0; i < def.suspensionCount; ++i)
	{
		const HaloSuspensionDef& suspension = def.suspensions[i];
		if (suspension.massPointIndex < 0 || suspension.massPointIndex >= physics.massPointCount)
			continue;

		const HaloMassPointDef& massPoint = physics.massPoints[suspension.massPointIndex];
		const float current = state.suspension[i] == 0xFF ? 1.0f : state.suspension[i] * (1.0f / 255.0f);
		const HaloVec3 point = TransformPoint(matrix, massPoint.position);
		const HaloVec3 normal = TransformNormal(matrix, massPoint.normal);
		const float extent = suspension.fullExtensionGroundDepth - suspension.fullCompressionGroundDepth;
		const float offset = (suspension.fullCompressionGroundDepth - physics.centerOfMass.k) - extent;
		const HaloVec3 start = PointFromLine(point, normal, offset);
		const HaloVec3 vector = Scale(normal, extent + extent);

		HaloTraceHit hit;
		if (!world.TestVector(start, vector, hit))
			hit.t = 1.0f;

		const float shift = Pin((1.0f - hit.t) + (1.0f - hit.t), 0.0f, 1.0f);
		if (shift - current > maximumShift)
			maximumShift = shift - current;

		state.suspension[i] = QuantizeToByteLowerBound((shift + current) * 0.5f);
	}

	if (maximumShift > 0.3f)
	{
		state.suspensionImpactScale = Pin((maximumShift - 0.3f) * (1.0f / (0.9f - 0.3f)), 0.0f, 1.0f);
		return true;
	}
	return false;
}

void ComputeCrash(const HaloPhysicsDef& physics, HaloVehicleState& state, const HaloVec3& previousVelocity,
	const MassPointState_t* massPoints)
{
	const HaloVec3 delta = Sub(state.translationalVelocity, previousVelocity);
	const float speed = Magnitude(delta);
	if (speed <= 0.02f)
		return;

	for (int i = 0; i < physics.massPointCount; ++i)
	{
		if (massPoints[i].flags & POINT_ON_GROUND)
		{
			state.crashScale = Pin((speed - 0.02f) * 45.454544f, 0.0f, 1.0f);
			return;
		}
	}
}

void ComputeAirborneTicks(const HaloPhysicsDef& physics, HaloVehicleState& state, const MassPointState_t* massPoints)
{
	if (state.airborneTicks < 0xFF)
		++state.airborneTicks;

	for (int i = 0; i < physics.massPointCount; ++i)
	{
		if (massPoints[i].flags & POINT_ON_GROUND)
		{
			state.airborneTicks = 0;
			if (state.onGroundTicks < 0xFF)
				++state.onGroundTicks;
			return;
		}
		if (massPoints[i].flags & POINT_ANTIGRAVING)
			state.airborneTicks = 0;
	}
	state.onGroundTicks = 0;
}

void SlowlyStop(HaloVehicleState& state)
{
	--state.stopTime;
	state.translationalVelocity = Scale(state.translationalVelocity, 0.835f);
	state.angularVelocity = Scale(state.angularVelocity, 0.835f);

	const HaloVec3 position = Add(state.translationalVelocity, state.position);
	HaloVec3 forward = state.forward, up = state.up;
	HaloVec3 axis = state.angularVelocity;
	const float magnitude = Normalize(axis);
	if (magnitude != 0.0f)
	{
		const HaloMatrix4x3 rotation = RotationFromAxisAndAngle(axis, sinf(magnitude), cosf(magnitude));
		forward = TransformVector(rotation, state.forward);
		up = TransformVector(rotation, state.up);
	}

	if (!state.stopTime)
	{
		state.translationalVelocity = kZero;
		state.angularVelocity = kZero;
	}
	SetPosition(state, position, forward, up);
}

void UpdateSeatPower(const HaloVehicleDef& def, HaloVehicleState& state)
{
	const bool present[2] = { state.driverPresent, state.gunnerPresent };
	for (int i = 0; i < 2; ++i)
	{
		const float up = def.seatPowerUpTicks > 0.0f ? 1.0f / def.seatPowerUpTicks : 1.0f;
		const float down = def.seatPowerDownTicks > 0.0f ? 1.0f / def.seatPowerDownTicks : 1.0f;
		state.seatPower[i] = present[i] ? fminf(state.seatPower[i] + up, 1.0f) : fmaxf(state.seatPower[i] - down, 0.0f);
	}
}

inline float DegreesToRadians(const float degrees)
{
	return degrees * kPi / 180.0f;
}

} // namespace

//-----------------------------------------------------------------------------
// Purpose: rejects definitions the simulation cannot run safely
//-----------------------------------------------------------------------------
bool HaloSim_ValidateDef(const HaloVehicleDef& def, char* pszError, size_t nErrorSize)
{
	auto fail = [pszError, nErrorSize](const char* pszWhat)
	{
		if (pszError && nErrorSize)
			snprintf(pszError, nErrorSize, "%s", pszWhat);
		return false;
	};
	auto finite = [](const float v) { return std::isfinite(v); };

	const HaloPhysicsDef& p = def.physics;
	if (def.type < 0 || def.type >= HALO_VEHICLE_TYPE_COUNT)
		return fail("vehicle type out of range");
	if (!(p.mass > 0.0f) || !finite(p.mass))
		return fail("mass must be positive");
	if (p.massPointCount < 1 || p.massPointCount > HALO_MAX_MASS_POINTS)
		return fail("mass point count out of range");
	if (p.poweredCount < 0 || p.poweredCount > HALO_MAX_POWERED_MASS_POINTS)
		return fail("powered mass point count out of range");
	if (def.suspensionCount < 0 || def.suspensionCount > HALO_MAX_SUSPENSIONS)
		return fail("suspension count out of range");
	if (!(def.speed.positiveScale > 0.0f) || !(def.speed.negativeScale >= 0.0f))
		return fail("speed limits must be positive");
	if (!(p.groundDepth >= 0.0f) || !(p.waterDepth >= 0.0f))
		return fail("depths must be non-negative");
	if (!(p.radius > 0.0f) && (!finite(p.inverseInertia.forward.i) || !finite(p.inverseInertia.left.j) || !finite(p.inverseInertia.up.k)))
		return fail("inverse inertia must be finite");

	for (int i = 0; i < p.massPointCount; ++i)
	{
		const HaloMassPointDef& mp = p.massPoints[i];
		if (!(mp.radius > 0.0f) || !finite(mp.radius) || !finite(mp.mass))
			return fail("mass point radius/mass invalid");
		if (mp.poweredIndex >= p.poweredCount)
			return fail("mass point powered index out of range");
		if (mp.frictionType < HALO_FRICTION_POINT || mp.frictionType > HALO_FRICTION_UP)
			return fail("mass point friction type out of range");
	}
	for (int i = 0; i < p.poweredCount; ++i)
	{
		const HaloPoweredMassPointDef& pp = p.powered[i];
		if ((pp.flags & HALO_POWERED_ANTIGRAV) && !(pp.antigravHeight > 0.0f))
			return fail("antigrav height must be positive");
	}
	for (int i = 0; i < def.suspensionCount; ++i)
	{
		if (def.suspensions[i].massPointIndex >= p.massPointCount)
			return fail("suspension mass point out of range");
	}
	return true;
}

void HaloSim_Reset(const HaloVehicleDef& def, HaloVehicleState& state,
	const HaloVec3& modelOrigin, const HaloVec3& forward, const HaloVec3& up)
{
	memset(&state, 0, sizeof(state));
	state.forward = forward;
	state.up = up;

	const HaloMatrix4x3 matrix = FromPointAndVectors(modelOrigin, forward, up);
	state.position = TransformPoint(matrix, def.physics.centerOfMass);
	state.desiredFacing = forward;
}

void HaloSim_GetModelOrigin(const HaloVehicleDef& def, const HaloVehicleState& state, HaloVec3& origin)
{
	HaloMatrix4x3 matrix = FromPointAndVectors(state.position, state.forward, state.up);
	origin = TransformPoint(matrix, NegatedCenterOfMass(def.physics));
}

bool HaloSim_IsFlipped(const HaloVehicleState& state)
{
	return state.up.k < 0.2f;
}

void HaloSim_StartUpending(HaloVehicleState& state, const uint8_t type)
{
	if (type < 1 || type > 4)
		return;
	state.flags |= HALO_VEH_UPENDING;
	state.upendingType = type;
	state.upendingTicks = 0;
	state.objectFlags &= ~HALO_OBJ_AT_REST;
}

void HaloSim_Tick(const HaloVehicleDef& def, HaloVehicleState& state, IHaloWorld& world)
{
	MassPointState_t massPoints[HALO_MAX_MASS_POINTS];
	PoweredState_t powered[HALO_MAX_POWERED_MASS_POINTS];

	memset(massPoints, 0, sizeof(massPoints));
	ResetPoweredStates(powered, HALO_MAX_POWERED_MASS_POINTS);
	state.crashScale = 0.0f;
	state.suspensionImpactScale = 0.0f;

	UpdateSeatPower(def, state);
	if (!state.driverPresent)
	{
		state.throttleForward = 0.0f;
		state.throttleLeft = 0.0f;
		state.controlFlags = 0;
		state.desiredFacing = state.forward;
	}

	auto setFlag = [&state](const uint16_t bit, const bool on)
	{
		if (on)
			state.flags |= bit;
		else
			state.flags &= ~bit;
	};

	setFlag(HALO_VEH_CROUCH, (state.controlFlags & HALO_CONTROL_CROUCH) != 0);
	setFlag(HALO_VEH_BRAKE, (state.controlFlags & HALO_CONTROL_JUMP) ||
		((def.flags & HALO_VDEF_BRAKE_ON_REVERSE) &&
			((state.throttleForward > 0.0f && state.speed < 0.0f) || (state.throttleForward < 0.0f && state.speed > 0.0f))));

	float steeringAngle;
	{
		const HaloVec3 left = Cross(state.up, state.forward);
		steeringAngle = atan2f(Dot(left, state.desiredFacing), Dot(state.desiredFacing, state.forward));
	}

	if ((state.flags & HALO_VEH_UPENDING) && state.upendingType && state.upendingTicks < 30 && state.up.k <= 0.9f)
	{
		float torque = (state.upendingType == 2 || state.upendingType == 4) ? 0.3f : -0.3f;
		HaloVec3 torqueAxis = (state.upendingType == 4 || state.upendingType == 3) ? Cross(state.forward, state.up) : state.forward;

		float roll = state.up.k * -2.0f;
		if (roll < def.upendingRollMin)
			roll = def.upendingRollMin;
		else if (roll > def.upendingRollMax)
			roll = def.upendingRollMax;

		torque *= roll;
		state.objectFlags &= ~HALO_OBJ_AT_REST;

		if (state.upendingType == 2 || state.upendingType == 1)
			torqueAxis = PointFromLine(torqueAxis, Cross(state.forward, state.up), -state.forward.k);

		state.angularVelocity = Scale(torqueAxis, torque);

		if (def.type == HALO_VEHICLE_ALIEN_FIGHTER)
			state.translationalVelocity.k = fminf(-0.01f, state.translationalVelocity.k);
		else if (def.type == HALO_VEHICLE_HUMAN_TANK)
			state.translationalVelocity = Scale(state.forward, Dot(state.forward, state.translationalVelocity));

		++state.upendingTicks;
	}
	else
	{
		state.upendingTicks = 0;
		state.upendingType = 0;
		state.flags &= ~HALO_VEH_UPENDING;
	}

	if (state.flags & HALO_VEH_BRAKE)
		SpeedUpdateSeek(state.speed, def.speed, 0.0f, 1.0f);
	else
	{
		SpeedUpdateSeek(state.speed, def.speed, state.throttleForward, 1.0f);
		SpeedUpdateSeek(state.slide, def.slide, state.throttleLeft, 1.0f);
	}

	if (def.type != HALO_VEHICLE_HUMAN_TANK)
	{
		float desiredPosition = state.speed < 0.0f ? -steeringAngle : steeringAngle;
		if (desiredPosition < DegreesToRadians(def.maximumRightTurn))
			desiredPosition = DegreesToRadians(def.maximumRightTurn);
		else if (desiredPosition > DegreesToRadians(def.maximumLeftTurn))
			desiredPosition = DegreesToRadians(def.maximumLeftTurn);

		const float limits[2] = { def.maximumLeftTurn, def.maximumRightTurn };
		PositionUpdateSeek(state.turn, limits, false, desiredPosition,
			DegreesToRadians(def.turnRate) * (1.0f / HALO_TICKS_PER_SECOND));
	}
	else if (state.speed == 0.0f)
		SpeedUpdateSeek(state.turn, def.speed, 0.0f, 1.0f);
	else
		SpeedUpdateSeek(state.turn, def.speed, Pin(steeringAngle * 0.63661975f, -1.0f, 1.0f) * def.speed.positiveScale, 2.0f);

	if (((def.flags & HALO_VDEF_SPEED_WAKES) && state.speed != 0.0f) ||
		((def.flags & HALO_VDEF_TURN_WAKES) && state.turn != 0.0f) ||
		((def.flags & HALO_VDEF_DRIVER_POWER_WAKES) && state.seatPower[0] != 0.0f) ||
		((def.flags & HALO_VDEF_GUNNER_POWER_WAKES) && state.seatPower[1] != 0.0f) ||
		((def.flags & HALO_VDEF_SLIDE_WAKES) && state.slide != 0.0f))
	{
		state.objectFlags &= ~HALO_OBJ_AT_REST;
	}

	if (!(state.objectFlags & HALO_OBJ_AT_REST))
	{
		const HaloVec3 previousVelocity = state.translationalVelocity;

		switch (def.type)
		{
		case HALO_VEHICLE_HUMAN_TANK:
			UpdateHumanTank(world, def, state, powered, massPoints);
			break;
		case HALO_VEHICLE_HUMAN_JEEP:
			UpdateHumanJeep(world, def, state, powered, massPoints);
			break;
		case HALO_VEHICLE_HUMAN_BOAT:
			UpdateHumanBoat(world, def, state, powered, massPoints);
			break;
		case HALO_VEHICLE_HUMAN_PLANE:
			UpdateHumanPlane(world, def, state, massPoints);
			break;
		case HALO_VEHICLE_ALIEN_SCOUT:
			UpdateAlienScout(world, def, state, steeringAngle, powered, massPoints);
			break;
		case HALO_VEHICLE_ALIEN_FIGHTER:
			if (def.physics.radius > 0.0f)
				UpdateAlienFighterOld(world, def, state, powered, massPoints);
			else
				UpdateAlienFighterNew(world, def, state, powered, massPoints);
			break;
		case HALO_VEHICLE_TURRET:
		default:
			PhysicsUpdate(world, def.physics, state, nullptr, massPoints, nullptr, nullptr);
			break;
		}

		if (!UpdateSuspension(world, def, state))
			ComputeCrash(def.physics, state, previousVelocity, massPoints);
		ComputeAirborneTicks(def.physics, state, massPoints);

		if (state.objectFlags & HALO_OBJ_AT_REST)
			state.stopTime = 15;

		if (def.type == HALO_VEHICLE_HUMAN_PLANE || def.type == HALO_VEHICLE_ALIEN_FIGHTER)
		{
			if (def.vehicleFloor != 0.0f && state.position.k < def.vehicleFloor)
				state.translationalVelocity.k += ((def.vehicleFloor - state.position.k) * 0.015625f -
					state.translationalVelocity.k * 0.0625f) * state.seatPower[0];
			if (def.vehicleCeiling != 0.0f && state.position.k > def.vehicleCeiling)
				state.translationalVelocity.k -= ((state.position.k - def.vehicleCeiling) * 0.015625f +
					state.translationalVelocity.k * 0.0625f) * state.seatPower[0];
		}
	}
	else if (state.stopTime > 0)
	{
		SlowlyStop(state);
		UpdateSuspension(world, def, state);
	}

	setFlag(HALO_VEH_BLUR, fabsf(state.speed) >= def.blurSpeed && def.blurSpeed > 0.0f);
}

void HaloSim_GetPose(const HaloVehicleDef& def, const HaloVehicleState& state, HaloVehiclePose& pose)
{
	memset(&pose, 0, sizeof(pose));
	const float forwardSpeed = fabsf(def.speed.positiveScale);
	const float reverseSpeed = fabsf(def.speed.negativeScale);
	const float maximumSpeed = fmaxf(forwardSpeed, reverseSpeed);

	pose.steering = state.turn;

	if (state.speed < 0.0f)
		pose.speedBlend = reverseSpeed > 0.0f ? 0.5f - state.speed / reverseSpeed * 0.5f : 0.5f;
	else
		pose.speedBlend = forwardSpeed > 0.0f ? (state.speed / forwardSpeed + 1.0f) * 0.5f : 0.5f;
	pose.speedBlend = Pin(pose.speedBlend, 0.0f, 1.0f);

	if (forwardSpeed > 0.0f)
	{
		const float lateral = Dot(Cross(state.up, state.forward), state.translationalVelocity);
		pose.slideBlend = Pin((lateral / forwardSpeed + 1.0f) * 0.5f, 0.0f, 1.0f);
	}

	if (def.wheelCircumference > 0.0f)
	{
		pose.wheelPosition = state.wheel / def.wheelCircumference;
		pose.wheelPositionRear = state.wheelRear / def.wheelCircumference;
	}

	for (int i = 0; i < def.suspensionCount; ++i)
		pose.suspension[i] = state.suspension[i] == 0xFF ? 1.0f : state.suspension[i] * (1.0f / 255.0f);

	pose.hover = state.hover;
	pose.thrust = state.thrust;
	pose.speedFraction = maximumSpeed > 0.0f ? Pin(fabsf(state.speed) / maximumSpeed, 0.0f, 1.0f) : 0.0f;
}

bool HaloSim_VehicleCollision(const HaloVehicleDef& defA, HaloVehicleState& stateA,
	const HaloVehicleDef& defB, HaloVehicleState& stateB)
{
	const HaloPhysicsDef& pa = defA.physics;
	const HaloPhysicsDef& pb = defB.physics;
	const float massScale = sqrtf(pa.mass * pb.mass);
	HaloVec3 force0 = kZero, force1 = kZero, torque0 = kZero, torque1 = kZero;
	bool collision = false;

	PhysicsInstance_t ia, ib;
	PhysicsInstanceNew(ia, pa, stateA);
	PhysicsInstanceNew(ib, pb, stateB);

	for (int i = 0; i < pa.massPointCount; ++i)
	{
		const HaloMassPointDef& mp0 = pa.massPoints[i];
		const HaloVec3 point0 = TransformPoint(ia.worldMatrix, mp0.position);

		for (int j = 0; j < pb.massPointCount; ++j)
		{
			const HaloMassPointDef& mp1 = pb.massPoints[j];
			const float radius = mp0.radius + mp1.radius;
			const HaloVec3 point1 = TransformPoint(ib.worldMatrix, mp1.position);
			HaloVec3 direction = Sub(point1, point0);
			const float distance = Normalize(direction);

			if (distance < radius && distance > 0.0f)
			{
				const float penetration = (radius - distance) * 0.5f;
				const float forceMagnitude = 2.0f * massScale * kGlobalGravity / kPhysicsCollisionDepth * penetration;
				const HaloVec3 collisionForce0 = Scale(direction, -forceMagnitude);
				const HaloVec3 collisionForce1 = Scale(direction, forceMagnitude);
				const HaloVec3 collisionPoint = PointFromLine(point0, direction, mp0.radius - penetration);

				force0 = Add(force0, collisionForce0);
				force1 = Add(force1, collisionForce1);
				torque0 = Add(torque0, Cross(Sub(collisionPoint, stateA.position), collisionForce0));
				torque1 = Add(torque1, Cross(Sub(collisionPoint, stateB.position), collisionForce1));
				collision = true;
			}
		}
	}

	if (collision)
	{
		stateA.collisionForce = Add(stateA.collisionForce, force0);
		stateA.collisionTorque = Add(stateA.collisionTorque, torque0);
		stateA.objectFlags &= ~HALO_OBJ_AT_REST;

		if (!(pb.radius > 0.0f))
		{
			stateB.collisionForce = Add(stateB.collisionForce, force1);
			stateB.collisionTorque = Add(stateB.collisionTorque, torque1);
			stateB.objectFlags &= ~HALO_OBJ_AT_REST;
		}
	}
	return collision;
}

void HaloSim_BipedImpact(const HaloVehicleState& vehicle, const HaloVec3& vehicleCenter,
	const HaloVec3& bipedCenter, const HaloVec3& bipedVelocity,
	HaloVec3& pushVelocity, bool& causeDamage)
{
	const float vehicleSpeed = Magnitude(vehicle.translationalVelocity);
	HaloVec3 acceleration = Sub(bipedCenter, vehicleCenter);

	Normalize(acceleration);
	acceleration.k += 0.8f;
	Normalize(acceleration);
	acceleration = Scale(acceleration, fmaxf(vehicleSpeed, 0.1f));
	acceleration = Add(acceleration, vehicle.translationalVelocity);
	pushVelocity = Scale(acceleration, 0.5f);

	causeDamage = vehicleSpeed > 0.06666667f ||
		MagnitudeSquared(Sub(vehicle.translationalVelocity, bipedVelocity)) > 0.0011111111f;
}
