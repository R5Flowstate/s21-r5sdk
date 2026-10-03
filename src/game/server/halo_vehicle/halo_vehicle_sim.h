//=============================================================================//
//
// Purpose: Halo mass-point vehicle simulation, engine independent.
//
// Units are Halo world units (1 wu = 10 ft = 120 in) and 30 Hz ticks: every
// velocity is wu/tick, every acceleration wu/tick^2. The caller owns unit
// conversion and the world queries (IHaloWorld).
//
//=============================================================================//
#ifndef HALO_VEHICLE_SIM_H
#define HALO_VEHICLE_SIM_H

#include <cstdint>

static constexpr float HALO_TICKS_PER_SECOND = 30.0f;
static constexpr float HALO_WU_TO_INCHES = 120.0f;

static constexpr int HALO_MAX_MASS_POINTS = 32;
static constexpr int HALO_MAX_POWERED_MASS_POINTS = 8;
static constexpr int HALO_MAX_SUSPENSIONS = 8;

struct HaloVec3
{
	float i, j, k;
};

struct HaloQuat
{
	HaloVec3 v;
	float w;
};

struct HaloPlane
{
	HaloVec3 n;
	float d;
};

// Rows are the basis vectors, as in the original layout.
struct HaloMatrix3x3
{
	HaloVec3 forward;
	HaloVec3 left;
	HaloVec3 up;
};

struct HaloMatrix4x3
{
	float scale;
	HaloVec3 forward;
	HaloVec3 left;
	HaloVec3 up;
	HaloVec3 position;
};

//-----------------------------------------------------------------------------
// Definitions (immutable, loaded once per vehicle type)
//-----------------------------------------------------------------------------
enum HaloFrictionType_t : int16_t
{
	HALO_FRICTION_POINT = 0,
	HALO_FRICTION_FORWARD,
	HALO_FRICTION_LEFT,
	HALO_FRICTION_UP,
};

enum HaloPoweredFlags_t : uint32_t
{
	HALO_POWERED_GROUND_FRICTION = 1u << 0,
	HALO_POWERED_WATER_FRICTION  = 1u << 1,
	HALO_POWERED_AIR_FRICTION    = 1u << 2,
	HALO_POWERED_WATER_LIFT      = 1u << 3,
	HALO_POWERED_AIR_LIFT        = 1u << 4,
	HALO_POWERED_THRUST          = 1u << 5,
	HALO_POWERED_ANTIGRAV        = 1u << 6,
};

struct HaloPoweredMassPointDef
{
	uint32_t flags;
	float antigravStrength;
	float antigravOffset;
	float antigravHeight;
	float antigravDampFraction;
	float antigravNormalK1;
	float antigravNormalK0;
	// Jeep steering: rotation applied to this point's frame per unit of turn.
	// 1 = steers with the wheel, -1 = counter-steers, 0 = fixed.
	float steerFactor;
};

struct HaloMassPointDef
{
	int16_t poweredIndex; // -1 = unpowered
	int16_t frictionType;
	uint32_t flags;
	float mass;
	float density;
	HaloVec3 position;
	HaloVec3 forward;
	HaloVec3 up;
	HaloVec3 normal; // suspension-trace direction, model space, down if absent
	float frictionParallelScale;
	float frictionPerpendicularScale;
	float radius;
};

struct HaloPhysicsDef
{
	float radius;   // > 0 selects the scalar-moment integrator
	float moment;
	float mass;
	HaloVec3 centerOfMass;
	float gravityScale;
	float groundFriction;
	float groundDepth;
	float groundDampFraction;
	float groundNormalK1;
	float groundNormalK0;
	float waterFriction;
	float waterDepth;
	float waterDensity;
	float airFriction;
	// Halo 3: per-point normal force cap as a multiple of the vehicle weight, 0 = off.
	float maximumNormalForceScale;
	// Halo 3: gravity fades out between these speeds (wu/tick), both 0 = off.
	float fullGravitySpeed;
	float zeroGravitySpeed;
	float xxMoment;
	float yyMoment;
	float zzMoment;
	HaloMatrix3x3 inverseInertia;

	int poweredCount;
	HaloPoweredMassPointDef powered[HALO_MAX_POWERED_MASS_POINTS];
	int massPointCount;
	HaloMassPointDef massPoints[HALO_MAX_MASS_POINTS];
};

enum HaloVehicleType_t : int16_t
{
	HALO_VEHICLE_HUMAN_TANK = 0,
	HALO_VEHICLE_HUMAN_JEEP,
	HALO_VEHICLE_HUMAN_BOAT,
	HALO_VEHICLE_HUMAN_PLANE,
	HALO_VEHICLE_ALIEN_SCOUT,
	HALO_VEHICLE_ALIEN_FIGHTER,
	HALO_VEHICLE_TURRET,
	HALO_VEHICLE_TYPE_COUNT
};

enum HaloVehicleDefFlags_t : uint32_t
{
	HALO_VDEF_SPEED_WAKES        = 1u << 0,
	HALO_VDEF_TURN_WAKES         = 1u << 1,
	HALO_VDEF_DRIVER_POWER_WAKES = 1u << 2,
	HALO_VDEF_GUNNER_POWER_WAKES = 1u << 3,
	HALO_VDEF_BRAKE_ON_REVERSE   = 1u << 4,
	HALO_VDEF_SLIDE_WAKES        = 1u << 5,
	HALO_VDEF_KILLS_RIDERS_ON_FALL = 1u << 6,
	HALO_VDEF_COLLISION_DAMAGE   = 1u << 7,
};

// Layout matters: speedParams and slideParams are read as
// {positive scale, negative scale, acceleration, deceleration}.
struct HaloSpeedParams
{
	float positiveScale;
	float negativeScale;
	float acceleration;
	float deceleration;
};

struct HaloSuspensionDef
{
	int16_t massPointIndex;
	float fullExtensionGroundDepth;
	float fullCompressionGroundDepth;
};

struct HaloVehicleDef
{
	int16_t type;
	uint32_t flags;
	HaloSpeedParams speed;     // forward max, reverse max, accel, decel (wu/tick)
	// Jeeps: degrees. Alien fighters: bank angle scale in radians.
	float maximumLeftTurn;
	float maximumRightTurn;
	float wheelCircumference;  // wu
	// Jeeps: degrees per second. Alien fighters: max angular speed, radians per tick.
	float turnRate;
	float blurSpeed;
	HaloSpeedParams slide;
	float upendingRollMin;
	float upendingRollMax;
	float fighterPitch;        // radians, banked pitch offset for alien fighters
	float seatPowerUpTicks;
	float seatPowerDownTicks;
	float vehicleFloor;        // wu, 0 = none (planes/fighters only)
	float vehicleCeiling;
	HaloVec3 boundsMin;        // wu, model space, the retail mesh extents
	HaloVec3 boundsMax;
	int suspensionCount;
	HaloSuspensionDef suspensions[HALO_MAX_SUSPENSIONS];
	HaloPhysicsDef physics;
};

//-----------------------------------------------------------------------------
// Runtime state
//-----------------------------------------------------------------------------
enum HaloObjectFlags_t : uint32_t
{
	HALO_OBJ_AT_REST        = 1u << 0,
	HALO_OBJ_ON_GROUND      = 1u << 1,
	HALO_OBJ_ON_MEDIA       = 1u << 2,
	HALO_OBJ_PARTIALLY_UNDER_MEDIA = 1u << 3,
	HALO_OBJ_WHOLLY_UNDER_MEDIA    = 1u << 4,
	HALO_OBJ_NO_PHYSICS     = 1u << 5,
};

enum HaloVehicleStateFlags_t : uint16_t
{
	HALO_VEH_BLUR     = 1u << 0,
	HALO_VEH_HOVERING = 1u << 1,
	HALO_VEH_CROUCH   = 1u << 2,
	HALO_VEH_BRAKE    = 1u << 3,
	HALO_VEH_UPENDING = 1u << 4,
};

enum HaloControlFlags_t : uint32_t
{
	HALO_CONTROL_CROUCH = 1u << 0,
	HALO_CONTROL_JUMP   = 1u << 1,
};

struct HaloVehicleState
{
	// object: position is the centre of mass in world space
	HaloVec3 position;
	HaloVec3 forward;
	HaloVec3 up;
	HaloVec3 translationalVelocity;
	HaloVec3 angularVelocity;
	uint32_t objectFlags;

	// unit: driver input, already in vehicle conventions
	float throttleForward;     // [-1, 1]
	float throttleLeft;        // [-1, 1]
	HaloVec3 desiredFacing;    // unit vector, world space
	uint32_t controlFlags;
	float seatPower[2];        // driver, gunner
	bool driverPresent;
	bool gunnerPresent;

	// vehicle
	uint16_t flags;
	int16_t stopTime;
	uint8_t airborneTicks;
	uint8_t upendingType;
	uint8_t upendingTicks;
	uint8_t onGroundTicks;
	float speed;
	float slide;
	float turn;
	float wheel;
	float wheelRear;
	float leftTread;
	float rightTread;
	float hover;
	float thrust;
	uint8_t suspension[HALO_MAX_SUSPENSIONS];
	HaloVec3 collisionForce;
	HaloVec3 collisionTorque;
	uint32_t stuckMassPointFlags;
	uint32_t groundedMassPointFlags;
	uint32_t antigravMassPointFlags;

	// Per-tick event outputs, [0, 1], zero when nothing happened.
	float crashScale;
	float suspensionImpactScale;
};

// Overlay inputs for the model's pose parameters, each already in the
// overlay's own [0, 1] range unless noted.
struct HaloVehiclePose
{
	float steering;            // radians, the turn angle
	float speedBlend;          // forward/reverse speed overlay
	float slideBlend;
	float wheelPosition;       // wheel rotation fraction
	float wheelPositionRear;
	float suspension[HALO_MAX_SUSPENSIONS];
	float hover;
	float thrust;
	float speedFraction;       // |speed| / max speed
};

//-----------------------------------------------------------------------------
// World queries
//-----------------------------------------------------------------------------
struct HaloTraceHit
{
	float t;          // [0, 1] along the swept vector
	HaloPlane plane;
	HaloVec3 point;
	bool volatileSurface; // dynamic object or breakable surface
};

struct HaloSphereContact
{
	float depth;      // radius minus distance to the nearest surface, > 0
	HaloPlane plane;
	bool volatileSurface;
};

class IHaloWorld
{
public:
	virtual ~IHaloWorld() { }
	// Swept ray, front faces only: a ray that starts inside solid must not hit.
	virtual bool TestVector(const HaloVec3& point, const HaloVec3& vector, HaloTraceHit& hit) = 0;
	virtual bool SphereContact(const HaloVec3& center, float radius, HaloSphereContact& contact) = 0;
	virtual float WaterDepth(const HaloVec3& point) = 0;
};

//-----------------------------------------------------------------------------
// API
//-----------------------------------------------------------------------------
bool HaloSim_ValidateDef(const HaloVehicleDef& def, char* pszError, size_t nErrorSize);

// Places the vehicle with its model origin at modelOrigin (wu) and the given basis.
void HaloSim_Reset(const HaloVehicleDef& def, HaloVehicleState& state,
	const HaloVec3& modelOrigin, const HaloVec3& forward, const HaloVec3& up);

// One 30 Hz tick.
void HaloSim_Tick(const HaloVehicleDef& def, HaloVehicleState& state, IHaloWorld& world);

void HaloSim_GetModelOrigin(const HaloVehicleDef& def, const HaloVehicleState& state, HaloVec3& origin);
void HaloSim_GetPose(const HaloVehicleDef& def, const HaloVehicleState& state, HaloVehiclePose& pose);
bool HaloSim_IsFlipped(const HaloVehicleState& state);

// Starts the flip-upright impulse; type 1..4 picks the roll axis and sign.
void HaloSim_StartUpending(HaloVehicleState& state, uint8_t type);

// Sphere-pair contact between two vehicles; accumulates into both states.
bool HaloSim_VehicleCollision(const HaloVehicleDef& defA, HaloVehicleState& stateA,
	const HaloVehicleDef& defB, HaloVehicleState& stateB);

// Shoves a biped the vehicle overlaps. Returns the push velocity (wu/tick)
// and whether the hit should deal collision damage.
void HaloSim_BipedImpact(const HaloVehicleState& vehicle, const HaloVec3& vehicleCenter,
	const HaloVec3& bipedCenter, const HaloVec3& bipedVelocity,
	HaloVec3& pushVelocity, bool& causeDamage);

#endif // HALO_VEHICLE_SIM_H
