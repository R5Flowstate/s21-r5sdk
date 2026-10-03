//=============================================================================//
//
// Purpose: Halo vehicle definition JSON parser.
//
//=============================================================================//
#if !defined(HALO_SIM_STANDALONE)
#include "core/stdafx.h"
#else
#include "rapidjson/document.h"
#endif // !HALO_SIM_STANDALONE
#include "halo_vehicle_defs.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{

struct NamedValue_t
{
	const char* name;
	uint32_t value;
};

constexpr NamedValue_t kVehicleTypes[] =
{
	{ "human_tank", HALO_VEHICLE_HUMAN_TANK },
	{ "human_jeep", HALO_VEHICLE_HUMAN_JEEP },
	{ "human_boat", HALO_VEHICLE_HUMAN_BOAT },
	{ "human_plane", HALO_VEHICLE_HUMAN_PLANE },
	{ "alien_scout", HALO_VEHICLE_ALIEN_SCOUT },
	{ "alien_fighter", HALO_VEHICLE_ALIEN_FIGHTER },
	{ "turret", HALO_VEHICLE_TURRET },
};

constexpr NamedValue_t kVehicleFlags[] =
{
	{ "speed_wakes", HALO_VDEF_SPEED_WAKES },
	{ "turn_wakes", HALO_VDEF_TURN_WAKES },
	{ "driver_power_wakes", HALO_VDEF_DRIVER_POWER_WAKES },
	{ "gunner_power_wakes", HALO_VDEF_GUNNER_POWER_WAKES },
	{ "brake_on_reverse", HALO_VDEF_BRAKE_ON_REVERSE },
	{ "slide_wakes", HALO_VDEF_SLIDE_WAKES },
	{ "kills_riders_on_fall", HALO_VDEF_KILLS_RIDERS_ON_FALL },
	{ "collision_damage", HALO_VDEF_COLLISION_DAMAGE },
};

constexpr NamedValue_t kPoweredFlags[] =
{
	{ "ground_friction", HALO_POWERED_GROUND_FRICTION },
	{ "water_friction", HALO_POWERED_WATER_FRICTION },
	{ "air_friction", HALO_POWERED_AIR_FRICTION },
	{ "water_lift", HALO_POWERED_WATER_LIFT },
	{ "air_lift", HALO_POWERED_AIR_LIFT },
	{ "thrust", HALO_POWERED_THRUST },
	{ "antigrav", HALO_POWERED_ANTIGRAV },
};

constexpr NamedValue_t kFrictionTypes[] =
{
	{ "point", HALO_FRICTION_POINT },
	{ "forward", HALO_FRICTION_FORWARD },
	{ "left", HALO_FRICTION_LEFT },
	{ "up", HALO_FRICTION_UP },
};

// Rejects magnitudes no vehicle needs; keeps garbage out of the integrator.
constexpr float kMaxMagnitude = 1.0e7f;

class Reader
{
public:
	Reader(char* pszError, size_t nErrorSize) : m_pszError(pszError), m_nErrorSize(nErrorSize), m_bFailed(false) { }

	bool Failed(void) const { return m_bFailed; }

	bool Fail(const char* pszKey, const char* pszWhat)
	{
		if (!m_bFailed && m_pszError && m_nErrorSize)
			snprintf(m_pszError, m_nErrorSize, "'%s': %s", pszKey, pszWhat);
		m_bFailed = true;
		return false;
	}

	float Float(const rapidjson::Value& obj, const char* pszKey, const float flDefault, const bool bRequired)
	{
		const rapidjson::Value::ConstMemberIterator it = obj.FindMember(pszKey);
		if (it == obj.MemberEnd())
		{
			if (bRequired)
				Fail(pszKey, "missing");
			return flDefault;
		}
		return CheckedFloat(it->value, pszKey);
	}

	float CheckedFloat(const rapidjson::Value& v, const char* pszKey)
	{
		if (!v.IsNumber())
		{
			Fail(pszKey, "not a number");
			return 0.0f;
		}
		const double d = v.GetDouble();
		if (!std::isfinite(d) || fabs(d) > kMaxMagnitude)
		{
			Fail(pszKey, "out of range");
			return 0.0f;
		}
		return static_cast<float>(d);
	}

	void Floats(const rapidjson::Value& obj, const char* pszKey, float* pOut, const int nCount, const bool bRequired)
	{
		const rapidjson::Value::ConstMemberIterator it = obj.FindMember(pszKey);
		if (it == obj.MemberEnd())
		{
			if (bRequired)
				Fail(pszKey, "missing");
			return;
		}
		if (!it->value.IsArray() || static_cast<int>(it->value.Size()) != nCount)
		{
			Fail(pszKey, "wrong array size");
			return;
		}
		for (int i = 0; i < nCount; ++i)
			pOut[i] = CheckedFloat(it->value[i], pszKey);
	}

	HaloVec3 Vec3(const rapidjson::Value& obj, const char* pszKey, const HaloVec3& vDefault, const bool bRequired)
	{
		float v[3] = { vDefault.i, vDefault.j, vDefault.k };
		Floats(obj, pszKey, v, 3, bRequired);
		return { v[0], v[1], v[2] };
	}

	template<size_t N>
	uint32_t Enum(const rapidjson::Value& obj, const char* pszKey, const NamedValue_t (&table)[N], const uint32_t nDefault)
	{
		const rapidjson::Value::ConstMemberIterator it = obj.FindMember(pszKey);
		if (it == obj.MemberEnd())
			return nDefault;
		if (!it->value.IsString())
		{
			Fail(pszKey, "not a string");
			return nDefault;
		}
		for (const NamedValue_t& entry : table)
		{
			if (strcmp(entry.name, it->value.GetString()) == 0)
				return entry.value;
		}
		Fail(pszKey, "unknown value");
		return nDefault;
	}

	template<size_t N>
	uint32_t Flags(const rapidjson::Value& obj, const char* pszKey, const NamedValue_t (&table)[N])
	{
		const rapidjson::Value::ConstMemberIterator it = obj.FindMember(pszKey);
		if (it == obj.MemberEnd())
			return 0;
		if (!it->value.IsArray())
		{
			Fail(pszKey, "not an array");
			return 0;
		}

		uint32_t flags = 0;
		for (rapidjson::SizeType i = 0; i < it->value.Size(); ++i)
		{
			const rapidjson::Value& name = it->value[i];
			bool bKnown = false;
			if (name.IsString())
			{
				for (const NamedValue_t& entry : table)
				{
					if (strcmp(entry.name, name.GetString()) == 0)
					{
						flags |= entry.value;
						bKnown = true;
						break;
					}
				}
			}
			if (!bKnown)
				Fail(pszKey, "unknown flag");
		}
		return flags;
	}

	const rapidjson::Value* Array(const rapidjson::Value& obj, const char* pszKey, const int nMax)
	{
		const rapidjson::Value::ConstMemberIterator it = obj.FindMember(pszKey);
		if (it == obj.MemberEnd())
			return nullptr;
		if (!it->value.IsArray())
		{
			Fail(pszKey, "not an array");
			return nullptr;
		}
		if (static_cast<int>(it->value.Size()) > nMax)
		{
			Fail(pszKey, "too many entries");
			return nullptr;
		}
		return &it->value;
	}

private:
	char* m_pszError;
	size_t m_nErrorSize;
	bool m_bFailed;
};

void ReadSpeedParams(Reader& r, const rapidjson::Value& obj, const char* pszKey, HaloSpeedParams& out, const bool bRequired)
{
	float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	r.Floats(obj, pszKey, v, 4, bRequired);
	out = { v[0], v[1], v[2], v[3] };
}

void ReadPhysics(Reader& r, const rapidjson::Value& obj, HaloPhysicsDef& p)
{
	p.radius = r.Float(obj, "radius", 0.0f, false);
	p.moment = r.Float(obj, "moment", 0.0f, false);
	p.mass = r.Float(obj, "mass", 0.0f, true);
	p.centerOfMass = r.Vec3(obj, "centerOfMass", { 0.0f, 0.0f, 0.0f }, true);
	p.gravityScale = r.Float(obj, "gravityScale", 1.0f, false);
	p.groundFriction = r.Float(obj, "groundFriction", 0.0f, true);
	p.groundDepth = r.Float(obj, "groundDepth", 0.0f, true);
	p.groundDampFraction = r.Float(obj, "groundDampFraction", 0.0f, true);
	p.groundNormalK1 = r.Float(obj, "groundNormalK1", 0.0f, true);
	p.groundNormalK0 = r.Float(obj, "groundNormalK0", 0.0f, true);
	p.waterFriction = r.Float(obj, "waterFriction", 0.0f, false);
	p.waterDepth = r.Float(obj, "waterDepth", 0.0f, false);
	p.waterDensity = r.Float(obj, "waterDensity", 1.0f, false);
	p.airFriction = r.Float(obj, "airFriction", 0.0f, false);
	p.maximumNormalForceScale = r.Float(obj, "maximumNormalForceScale", 0.0f, false);
	p.fullGravitySpeed = r.Float(obj, "fullGravitySpeed", 0.0f, false);
	p.zeroGravitySpeed = r.Float(obj, "zeroGravitySpeed", 0.0f, false);

	float moments[3] = { 0.0f, 0.0f, 0.0f };
	r.Floats(obj, "moments", moments, 3, true);
	p.xxMoment = moments[0];
	p.yyMoment = moments[1];
	p.zzMoment = moments[2];

	float inverse[9] = { 0.0f };
	r.Floats(obj, "inverseInertia", inverse, 9, false);
	p.inverseInertia.forward = { inverse[0], inverse[1], inverse[2] };
	p.inverseInertia.left = { inverse[3], inverse[4], inverse[5] };
	p.inverseInertia.up = { inverse[6], inverse[7], inverse[8] };

	if (const rapidjson::Value* powered = r.Array(obj, "powered", HALO_MAX_POWERED_MASS_POINTS))
	{
		p.poweredCount = static_cast<int>(powered->Size());
		for (int i = 0; i < p.poweredCount && !r.Failed(); ++i)
		{
			const rapidjson::Value& e = (*powered)[i];
			if (!e.IsObject())
			{
				r.Fail("powered", "entry is not an object");
				break;
			}
			HaloPoweredMassPointDef& pp = p.powered[i];
			pp.flags = r.Flags(e, "flags", kPoweredFlags);
			float antigrav[6] = { 0.0f };
			r.Floats(e, "antigrav", antigrav, 6, false);
			pp.antigravStrength = antigrav[0];
			pp.antigravOffset = antigrav[1];
			pp.antigravHeight = antigrav[2];
			pp.antigravDampFraction = antigrav[3];
			pp.antigravNormalK1 = antigrav[4];
			pp.antigravNormalK0 = antigrav[5];
			pp.steerFactor = r.Float(e, "steerFactor", 0.0f, false);
		}
	}

	const rapidjson::Value* points = r.Array(obj, "massPoints", HALO_MAX_MASS_POINTS);
	if (!points)
	{
		r.Fail("massPoints", "missing");
		return;
	}

	p.massPointCount = static_cast<int>(points->Size());
	for (int i = 0; i < p.massPointCount && !r.Failed(); ++i)
	{
		const rapidjson::Value& e = (*points)[i];
		if (!e.IsObject())
		{
			r.Fail("massPoints", "entry is not an object");
			break;
		}
		HaloMassPointDef& mp = p.massPoints[i];
		const float powered = r.Float(e, "powered", -1.0f, false);
		mp.poweredIndex = static_cast<int16_t>(powered < 0.0f ? -1 : static_cast<int>(powered));
		mp.frictionType = static_cast<int16_t>(r.Enum(e, "frictionType", kFrictionTypes, HALO_FRICTION_POINT));
		mp.flags = 0;
		mp.mass = r.Float(e, "mass", 0.0f, true);
		mp.density = r.Float(e, "density", 0.0f, false);
		mp.position = r.Vec3(e, "position", { 0.0f, 0.0f, 0.0f }, true);
		mp.forward = r.Vec3(e, "forward", { 1.0f, 0.0f, 0.0f }, false);
		mp.up = r.Vec3(e, "up", { 0.0f, 0.0f, 1.0f }, false);
		// H3 friction points carry no contact normal; CE measures suspension
		// travel along it, down in model space for a ground vehicle.
		mp.normal = r.Vec3(e, "normal", { 0.0f, 0.0f, -1.0f }, false);
		float frictionScale[2] = { 1.0f, 1.0f };
		r.Floats(e, "frictionScale", frictionScale, 2, false);
		mp.frictionParallelScale = frictionScale[0];
		mp.frictionPerpendicularScale = frictionScale[1];
		mp.radius = r.Float(e, "radius", 0.0f, true);
	}
}

} // namespace

bool HaloDefs_Parse(const char* pszText, size_t nTextSize, HaloVehicleDef& def,
	char* pszError, size_t nErrorSize)
{
	if (pszError && nErrorSize)
		pszError[0] = '\0';

	memset(&def, 0, sizeof(def));

	Reader r(pszError, nErrorSize);
	if (!pszText || nTextSize == 0 || nTextSize > HALO_DEF_MAX_JSON_BYTES)
		return r.Fail("file", "empty or too large");

	rapidjson::Document doc;
	doc.Parse(pszText, nTextSize);
	if (doc.HasParseError() || !doc.IsObject())
		return r.Fail("file", "invalid JSON");

	def.type = static_cast<int16_t>(r.Enum(doc, "type", kVehicleTypes, HALO_VEHICLE_TYPE_COUNT));
	if (def.type == HALO_VEHICLE_TYPE_COUNT)
		return r.Fail("type", "missing");

	def.flags = r.Flags(doc, "flags", kVehicleFlags);
	ReadSpeedParams(r, doc, "speed", def.speed, true);
	ReadSpeedParams(r, doc, "slide", def.slide, false);
	def.maximumLeftTurn = r.Float(doc, "maximumLeftTurn", 0.0f, false);
	def.maximumRightTurn = r.Float(doc, "maximumRightTurn", 0.0f, false);
	def.wheelCircumference = r.Float(doc, "wheelCircumference", 0.0f, false);
	def.turnRate = r.Float(doc, "turnRate", 0.0f, false);
	def.blurSpeed = r.Float(doc, "blurSpeed", 0.0f, false);
	def.fighterPitch = r.Float(doc, "fighterPitch", 0.0f, false);
	def.vehicleFloor = r.Float(doc, "vehicleFloor", 0.0f, false);
	def.vehicleCeiling = r.Float(doc, "vehicleCeiling", 0.0f, false);

	float upending[2] = { 0.0f, 0.0f };
	r.Floats(doc, "upendingRoll", upending, 2, false);
	def.upendingRollMin = upending[0];
	def.upendingRollMax = upending[1];

	// Entity bbox, published through CollisionProp at register. World units,
	// model space, measured from the retail mesh (same frame as the sim).
	def.boundsMin = r.Vec3(doc, "boundsMin", { 0.0f, 0.0f, 0.0f }, true);
	def.boundsMax = r.Vec3(doc, "boundsMax", { 0.0f, 0.0f, 0.0f }, true);
	if (!r.Failed())
	{
		if (!(def.boundsMin.i < def.boundsMax.i && def.boundsMin.j < def.boundsMax.j && def.boundsMin.k < def.boundsMax.k))
			r.Fail("boundsMin", "must be below boundsMax on every axis");
		else if (def.boundsMax.i - def.boundsMin.i > 50.0f || def.boundsMax.j - def.boundsMin.j > 50.0f ||
			def.boundsMax.k - def.boundsMin.k > 50.0f)
			r.Fail("boundsMax", "span out of range");
	}

	float seatPower[2] = { 1.0f, 1.0f };
	r.Floats(doc, "seatPowerTicks", seatPower, 2, false);
	def.seatPowerUpTicks = seatPower[0];
	def.seatPowerDownTicks = seatPower[1];

	if (const rapidjson::Value* suspensions = r.Array(doc, "suspensions", HALO_MAX_SUSPENSIONS))
	{
		def.suspensionCount = static_cast<int>(suspensions->Size());
		for (int i = 0; i < def.suspensionCount && !r.Failed(); ++i)
		{
			const rapidjson::Value& e = (*suspensions)[i];
			if (!e.IsObject())
			{
				r.Fail("suspensions", "entry is not an object");
				break;
			}
			def.suspensions[i].massPointIndex = static_cast<int16_t>(r.Float(e, "massPoint", -1.0f, true));
			def.suspensions[i].fullExtensionGroundDepth = r.Float(e, "extension", 0.0f, true);
			def.suspensions[i].fullCompressionGroundDepth = r.Float(e, "compression", 0.0f, true);
		}
	}

	const rapidjson::Value::ConstMemberIterator physics = doc.FindMember("physics");
	if (physics == doc.MemberEnd() || !physics->value.IsObject())
		return r.Fail("physics", "missing");
	ReadPhysics(r, physics->value, def.physics);

	if (r.Failed())
		return false;

	return HaloSim_ValidateDef(def, pszError, nErrorSize);
}
