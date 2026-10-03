//=============================================================================//
//
// Purpose: Halo vehicles on the dedicated server. Script creates the prop and
//          seats players; this file owns the simulation and moves the prop.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "mathlib/mathlib.h"
#include "public/bspflags.h"
#include "public/game/shared/in_buttons.h"
#include "engine/enginetrace.h"
#include "game/shared/util_shared.h"
#include "game/shared/usercmd.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/edict_dirty.h"
#include "game/server/baseentity.h"
#include "game/server/player.h"
#include "game/server/translocation.h"
#include "game/server/vscript_server.h"
#include "pluginsystem/modsystem.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "halo_vehicle.h"
#include "halo_vehicle_defs.h"
#include "halo_vehicle_sim.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern CGlobalVars* gpGlobals;

static ConVar halo_vehicle_max("halo_vehicle_max", "16", FCVAR_RELEASE,
	"Maximum Halo vehicles alive at once.", true, 0.f, true, 64.f);
static ConVar halo_vehicle_debug("halo_vehicle_debug", "0", FCVAR_DEVELOPMENTONLY,
	"Log Halo vehicle input and state (1 = on).");
static ConVar halo_vehicle_debug_rate("halo_vehicle_debug_rate", "1.0", FCVAR_DEVELOPMENTONLY,
	"Seconds between [HALO-VEH] debug lines.", true, 0.05f, true, 10.f);
static ConVar halo_vehicle_look_clamp("halo_vehicle_look_clamp", "30", FCVAR_RELEASE,
	"Degrees the driver may steer the body away from its own heading.", true, 0.f, true, 180.f);

static constexpr int kMaxVehicles = 64;
static constexpr int kMaxDefs = 16;
static constexpr int kMaxSubsteps = 3;
static constexpr float kTick = 1.0f / HALO_TICKS_PER_SECOND;
// Usercmd move axes arrive either normalised or scaled to this full-speed value.
static constexpr float kFullMove = 450.0f;
static constexpr float kSphereProbeDiagonal = 0.70710678f;

struct HaloDefEntry_t
{
	char name[32];
	HaloVehicleDef def;
};

struct HaloVehicleEntry_t
{
	bool used;
	int defIndex;
	SDKEntityHandle prop;
	SDKEntityHandle seats[HALO_SEAT_COUNT];
	HaloVehicleState state;
	float accumulator;
	float lastLog;

	float inputForward;
	float inputLeft;
	HaloVec3 inputFacing;
	uint32_t inputControl;
};

static std::vector<HaloDefEntry_t> s_defs;
static HaloVehicleEntry_t s_vehicles[kMaxVehicles];
static bool s_bFirstTickLogged = false;

//-----------------------------------------------------------------------------
// units
//-----------------------------------------------------------------------------
static inline Vector3D ToInches(const HaloVec3& v)
{
	return Vector3D(v.i * HALO_WU_TO_INCHES, v.j * HALO_WU_TO_INCHES, v.k * HALO_WU_TO_INCHES);
}

static inline HaloVec3 ToWorldUnits(const Vector3D& v)
{
	return { v.x / HALO_WU_TO_INCHES, v.y / HALO_WU_TO_INCHES, v.z / HALO_WU_TO_INCHES };
}

static inline bool IsFiniteVec(const HaloVec3& v)
{
	return std::isfinite(v.i) && std::isfinite(v.j) && std::isfinite(v.k);
}

// Pitch/yaw/roll in degrees from an orthonormal forward/up pair (Source convention).
static void BasisToAngles(const HaloVec3& f, const HaloVec3& u, float out[3])
{
	const float xy = sqrtf(f.i * f.i + f.j * f.j);
	if (xy > 0.001f)
	{
		const HaloVec3 left = { u.j * f.k - u.k * f.j, u.k * f.i - u.i * f.k, u.i * f.j - u.j * f.i };
		const float upZ = left.j * f.i - left.i * f.j;
		out[0] = RAD2DEG(atan2f(-f.k, xy));
		out[1] = RAD2DEG(atan2f(f.j, f.i));
		out[2] = RAD2DEG(atan2f(left.k, upZ));
	}
	else
	{
		out[0] = RAD2DEG(atan2f(-f.k, xy));
		out[1] = RAD2DEG(atan2f(-u.i, u.j));
		out[2] = 0.0f;
	}
}

static HaloVec3 AnglesToForward(const QAngle& a)
{
	const float p = DEG2RAD(a.x), y = DEG2RAD(a.y);
	return { cosf(p) * cosf(y), cosf(p) * sinf(y), -sinf(p) };
}

//-----------------------------------------------------------------------------
// world queries over the server trace
//-----------------------------------------------------------------------------
class CHaloWorld : public IHaloWorld
{
public:
	explicit CHaloWorld(const void* pIgnore) : m_pIgnore(pIgnore) { }

	bool TestVector(const HaloVec3& point, const HaloVec3& vector, HaloTraceHit& hit) override
	{
		trace_t tr;
		if (!Trace(ToInches(point), ToInches({ point.i + vector.i, point.j + vector.j, point.k + vector.k }), tr))
			return false;
		hit.t = tr.fraction;
		hit.point = ToWorldUnits(tr.endpos);
		hit.plane.n = { tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z };
		hit.plane.d = hit.plane.n.i * hit.point.i + hit.plane.n.j * hit.point.j + hit.plane.n.k * hit.point.k;
		hit.volatileSurface = false;
		return true;
	}

	// Nearest surface within the sphere, from probes along the axes and the lower diagonals.
	bool SphereContact(const HaloVec3& center, float radius, HaloSphereContact& contact) override
	{
		static const HaloVec3 kDirs[] =
		{
			{ 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 },
			{ kSphereProbeDiagonal, 0, -kSphereProbeDiagonal }, { -kSphereProbeDiagonal, 0, -kSphereProbeDiagonal },
			{ 0, kSphereProbeDiagonal, -kSphereProbeDiagonal }, { 0, -kSphereProbeDiagonal, -kSphereProbeDiagonal },
		};

		bool found = false;
		float bestDepth = 0.0f;
		for (const HaloVec3& d : kDirs)
		{
			const HaloVec3 end = { center.i + d.i * radius, center.j + d.j * radius, center.k + d.k * radius };
			trace_t tr;
			if (!Trace(ToInches(center), ToInches(end), tr))
				continue;

			const HaloVec3 n = { tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z };
			const HaloVec3 p = ToWorldUnits(tr.endpos);
			const float distance = (center.i - p.i) * n.i + (center.j - p.j) * n.j + (center.k - p.k) * n.k;
			const float depth = radius - distance;
			if (depth > 0.0f && (!found || depth > bestDepth))
			{
				found = true;
				bestDepth = depth;
				contact.depth = depth;
				contact.plane.n = n;
				contact.plane.d = n.i * p.i + n.j * p.j + n.k * p.k;
				contact.volatileSurface = false;
			}
		}
		return found;
	}

	float WaterDepth(const HaloVec3& point) override
	{
		(void)point;
		return 0.0f;
	}

private:
	bool Trace(const Vector3D& start, const Vector3D& end, trace_t& tr)
	{
		if (!g_pEngineTraceServer)
			return false;
		Ray_t ray;
		ray.Init(start, end, 0x3f800000, 0);
		memset(&tr, 0, sizeof(tr));
		tr.fraction = 1.0f;
		CTraceFilterSimple filter(reinterpret_cast<const IHandleEntity*>(m_pIgnore), 0);
		g_pEngineTraceServer->TraceRayFiltered(ray, TRACE_MASK_PLAYERSOLID_BRUSHONLY, &filter, &tr);
		// Front faces only: a ray that starts in solid reports no hit.
		if (tr.startsolid || tr.allsolid)
			return false;
		return tr.fraction < 1.0f;
	}

	const void* m_pIgnore;
};

//-----------------------------------------------------------------------------
// definitions
//-----------------------------------------------------------------------------
static bool HaloVehicle_IsSafeName(const char* pszName)
{
	const size_t n = strlen(pszName);
	if (n == 0 || n >= sizeof(HaloDefEntry_t::name))
		return false;
	for (size_t i = 0; i < n; ++i)
	{
		const char c = pszName[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return true;
}

static int HaloVehicle_LoadDef(const char* pszName)
{
	for (size_t i = 0; i < s_defs.size(); ++i)
	{
		if (strcmp(s_defs[i].name, pszName) == 0)
			return static_cast<int>(i);
	}
	if (s_defs.size() >= kMaxDefs || !HaloVehicle_IsSafeName(pszName))
		return -1;

	std::string text;
	CModSystem* const pModSystem = ModSystem();
	if (pModSystem)
	{
		for (const CModSystem::ModInstance_t* pMod : pModSystem->GetResolvedModList())
		{
			if (!pMod || !pMod->IsEnabled())
				continue;
			const std::string path = std::string(pMod->GetBasePath().String()) + "scripts/cafefps_halovehicles/" + pszName + ".json";
			FILE* f = fopen(path.c_str(), "rb");
			if (!f)
				continue;
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() <= HALO_DEF_MAX_JSON_BYTES)
				text.append(buf, n);
			fclose(f);
			break;
		}
	}

	if (text.empty())
	{
		Warning(eDLL_T::SERVER, "[HALO-VEH] no definition 'scripts/cafefps_halovehicles/%s.json' in any enabled mod\n", pszName);
		return -1;
	}

	HaloDefEntry_t entry;
	char error[256];
	if (!HaloDefs_Parse(text.data(), text.size(), entry.def, error, sizeof(error)))
	{
		Warning(eDLL_T::SERVER, "[HALO-VEH] definition '%s' rejected: %s\n", pszName, error);
		return -1;
	}
	strncpy_s(entry.name, sizeof(entry.name), pszName, _TRUNCATE);
	s_defs.push_back(entry);
	Msg(eDLL_T::SERVER, "[HALO-VEH] loaded definition '%s' (type %d, %d mass points)\n",
		pszName, entry.def.type, entry.def.physics.massPointCount);
	return static_cast<int>(s_defs.size() - 1);
}

//-----------------------------------------------------------------------------
// registry
//-----------------------------------------------------------------------------
static HaloVehicleEntry_t* HaloVehicle_FindByProp(const void* pProp)
{
	if (!pProp)
		return nullptr;
	const SDKEntityHandle h = SDKEntityState_GetHandle(pProp);
	if (!h.IsValid())
		return nullptr;
	for (HaloVehicleEntry_t& e : s_vehicles)
	{
		if (e.used && e.prop == h)
			return &e;
	}
	return nullptr;
}

static HaloVehicleEntry_t* HaloVehicle_FindBySeated(const void* pPlayer, int* pSeat)
{
	if (!pPlayer)
		return nullptr;
	const SDKEntityHandle h = SDKEntityState_GetHandle(pPlayer);
	if (!h.IsValid())
		return nullptr;
	for (HaloVehicleEntry_t& e : s_vehicles)
	{
		if (!e.used)
			continue;
		for (int s = 0; s < HALO_SEAT_COUNT; ++s)
		{
			if (e.seats[s] == h)
			{
				if (pSeat)
					*pSeat = s;
				return &e;
			}
		}
	}
	return nullptr;
}

static int HaloVehicle_Count(void)
{
	int n = 0;
	for (const HaloVehicleEntry_t& e : s_vehicles)
		n += e.used ? 1 : 0;
	return n;
}

// Inbound hull probe: fires at the body from outside along a model-space axis
// with an UNFILTERED engine trace, so it reports what the engine built for this
// prop (vcollide/entity box), not what our sim thinks. Returns the hit fraction
// (expect ~0.62 when the hull covers that face), 1.0 on a clean miss, negative
// when the ray starts inside solid or there is no trace backend.
static float HaloVehicle_ProbeHull(const HaloVec3& originWu, const HaloVec3& axisWu, float boundWu)
{
	if (!g_pEngineTraceServer)
		return -2.0f;
	const HaloVec3 fromWu = { originWu.i + axisWu.i * (boundWu + 0.8f),
		originWu.j + axisWu.j * (boundWu + 0.8f), originWu.k + axisWu.k * (boundWu + 0.8f) };
	const HaloVec3 toWu = { originWu.i + axisWu.i * (boundWu - 0.5f),
		originWu.j + axisWu.j * (boundWu - 0.5f), originWu.k + axisWu.k * (boundWu - 0.5f) };
	const Vector3D from(fromWu.i * HALO_WU_TO_INCHES, fromWu.j * HALO_WU_TO_INCHES, fromWu.k * HALO_WU_TO_INCHES);
	const Vector3D to(toWu.i * HALO_WU_TO_INCHES, toWu.j * HALO_WU_TO_INCHES, toWu.k * HALO_WU_TO_INCHES);
	Ray_t ray;
	ray.Init(from, to, 0x3f800000, 0);
	trace_t tr;
	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1.0f;
	CTraceFilterSimple filter(nullptr, 0);
	g_pEngineTraceServer->TraceRayFiltered(ray, TRACE_MASK_SHOT, &filter, &tr);
	if (tr.startsolid || tr.allsolid)
		return -1.0f;
	return tr.fraction;
}

static void HaloVehicle_WriteProp(HaloVehicleEntry_t& e, void* pProp)
{
	const HaloVehicleDef& def = s_defs[e.defIndex].def;
	HaloVec3 origin;
	HaloSim_GetModelOrigin(def, e.state, origin);
	if (!IsFiniteVec(origin) || !IsFiniteVec(e.state.forward) || !IsFiniteVec(e.state.up))
		return;

	const Vector3D o = ToInches(origin);
	const float flOrigin[3] = { o.x, o.y, o.z };
	float flAngles[3];
	BasisToAngles(e.state.forward, e.state.up, flAngles);
	const Vector3D vel = ToInches(e.state.translationalVelocity);
	const float flVel[3] = { vel.x * HALO_TICKS_PER_SECOND, vel.y * HALO_TICKS_PER_SECOND, vel.z * HALO_TICKS_PER_SECOND };

	Translocation_SetAbsOrigin3(pProp, flOrigin);
	Translocation_SetAbsAngles3(pProp, flAngles);
	Translocation_SetAbsVelocity3(pProp, flVel);
}

void HaloVehicle_Frame(void)
{
	if (!gpGlobals)
		return;
	const float dt = gpGlobals->frameTime;
	if (!(dt > 0.0f) || dt > 1.0f)
		return;

	for (HaloVehicleEntry_t& e : s_vehicles)
	{
		if (!e.used)
			continue;

		void* const pProp = SDKEntityState_Resolve(e.prop, ESide::Server);
		if (!pProp)
		{
			e = HaloVehicleEntry_t{};
			continue;
		}

		for (int s = 0; s < HALO_SEAT_COUNT; ++s)
		{
			if (e.seats[s].IsValid() && !SDKEntityState_Resolve(e.seats[s], ESide::Server))
				e.seats[s] = SDKEntityHandle();
		}

		const HaloVehicleDef& def = s_defs[e.defIndex].def;
		CHaloWorld world(pProp);
		e.accumulator = fminf(e.accumulator + dt, kTick * kMaxSubsteps);

		int steps = 0;
		while (e.accumulator >= kTick && steps < kMaxSubsteps)
		{
			e.accumulator -= kTick;
			++steps;

			e.state.driverPresent = e.seats[HALO_SEAT_DRIVER].IsValid();
			e.state.gunnerPresent = e.seats[HALO_SEAT_GUNNER].IsValid();
			e.state.throttleForward = e.inputForward;
			e.state.throttleLeft = e.inputLeft;
			e.state.desiredFacing = e.inputFacing;
			e.state.controlFlags = e.inputControl;

			const HaloVehicleState lastGood = e.state;
			HaloSim_Tick(def, e.state, world);
			if (!IsFiniteVec(e.state.position) || !IsFiniteVec(e.state.translationalVelocity) ||
				!IsFiniteVec(e.state.angularVelocity))
			{
				e.state = lastGood;
				e.state.translationalVelocity = { 0, 0, 0 };
				e.state.angularVelocity = { 0, 0, 0 };
				Warning(eDLL_T::SERVER, "[HALO-VEH] non-finite state on '%s' -- reset to last good\n", s_defs[e.defIndex].name);
			}

			if (!s_bFirstTickLogged)
			{
				s_bFirstTickLogged = true;
				Msg(eDLL_T::SERVER, "[HALO-VEH] first simulation tick ('%s')\n", s_defs[e.defIndex].name);
			}
		}

		if (steps > 0)
			HaloVehicle_WriteProp(e, pProp);

		if (halo_vehicle_debug.GetBool() && gpGlobals->curTime - e.lastLog > halo_vehicle_debug_rate.GetFloat())
		{
			e.lastLog = gpGlobals->curTime;
			const HaloVec3& fwd = e.state.forward;
			const HaloVec3& vel = e.state.translationalVelocity;
			HaloVehiclePose pose;
			HaloSim_GetPose(s_defs[e.defIndex].def, e.state, pose);
			const float yawVeh = RAD2DEG(atan2f(fwd.j, fwd.i));
			const float yawIn = RAD2DEG(atan2f(e.inputFacing.j, e.inputFacing.i));
			const float yawVel = RAD2DEG(atan2f(vel.j, vel.i));
			HaloVec3 originWu;
			HaloSim_GetModelOrigin(s_defs[e.defIndex].def, e.state, originWu);
			const HaloVec3 leftWu = { e.state.up.j * fwd.k - e.state.up.k * fwd.j,
				e.state.up.k * fwd.i - e.state.up.i * fwd.k, e.state.up.i * fwd.j - e.state.up.j * fwd.i };
			const HaloVec3 negFwd = { -fwd.i, -fwd.j, -fwd.k };
			const HaloVec3 negLeft = { -leftWu.i, -leftWu.j, -leftWu.k };
			const HaloVehicleDef& probeDef = s_defs[e.defIndex].def;
			const float hullNose = HaloVehicle_ProbeHull(originWu, fwd, probeDef.boundsMax.i);
			const float hullTail = HaloVehicle_ProbeHull(originWu, negFwd, -probeDef.boundsMin.i);
			const float hullLeft = HaloVehicle_ProbeHull(originWu, leftWu, probeDef.boundsMax.j);
			const float hullRight = HaloVehicle_ProbeHull(originWu, negLeft, -probeDef.boundsMin.j);
			Msg(eDLL_T::SERVER, "[HALO-VEH] '%s' pos=(%.0f %.0f %.0f) speed=%.1f in/s veh_speed=%.3f turn=%.2f up_k=%.2f grounded=0x%X driver=%d in=(%.2f %.2f) facing=(%.2f %.2f %.2f) rest=%d yaw_veh=%.0f yaw_in=%.0f yaw_vel=%.0f slip=%.0f clamp=%.0f hull=(nose%.2f tail%.2f left%.2f right%.2f) pose=(steer%.1f wheel%.2f susp%.2f %.2f %.2f %.2f)\n",
				s_defs[e.defIndex].name, e.state.position.i * HALO_WU_TO_INCHES, e.state.position.j * HALO_WU_TO_INCHES,
				e.state.position.k * HALO_WU_TO_INCHES,
				sqrtf(vel.i * vel.i + vel.j * vel.j) * HALO_WU_TO_INCHES * HALO_TICKS_PER_SECOND,
				e.state.speed, e.state.turn, e.state.up.k, e.state.groundedMassPointFlags,
				e.state.driverPresent ? 1 : 0, e.inputForward, e.inputLeft,
				e.inputFacing.i, e.inputFacing.j, e.inputFacing.k, (e.state.objectFlags & HALO_OBJ_AT_REST) ? 1 : 0,
				yawVeh, yawIn, yawVel, yawVeh - yawVel, halo_vehicle_look_clamp.GetFloat(),
				RAD2DEG(pose.steering), pose.wheelPosition,
				pose.suspension[0], pose.suspension[1], pose.suspension[2], pose.suspension[3]);
		}
	}
}

void HaloVehicle_OnRunCommand(CPlayer* pPlayer, CUserCmd* pCmd)
{
	if (!pPlayer || !pCmd)
		return;

	int seat = -1;
	HaloVehicleEntry_t* const e = HaloVehicle_FindBySeated(pPlayer, &seat);
	if (!e)
		return;

	if (seat == HALO_SEAT_DRIVER)
	{
		auto axis = [](float v)
		{
			if (!std::isfinite(v))
				return 0.0f;
			if (fabsf(v) > 1.0f)
				v /= kFullMove;
			return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
		};

		e->inputForward = axis(pCmd->forwardmove);
		// Usercmd side axis is positive to the right; the sim's is positive to the left.
		e->inputLeft = -axis(pCmd->sidemove);

		QAngle view = pCmd->viewangles;
		if (std::isfinite(view.x) && std::isfinite(view.y))
		{
			view.x = view.x < -89.0f ? -89.0f : (view.x > 89.0f ? 89.0f : view.x);

			// The steering servo points the body at desiredFacing and has authority
			// over the whole +-60 deg wheel range. CE/H3 only ever drive it from a
			// cockpit camera locked to the body, so the offset stays small. Ours is a
			// free-look chase camera, so bound the yaw offset here: an unclamped
			// absolute view angle lets the body aim far off its own axis and the
			// vehicle crabs sideways across the player's view.
			const float bodyYaw = RAD2DEG(atan2f(e->state.forward.j, e->state.forward.i));
			float delta = view.y - bodyYaw;
			while (delta > 180.0f)
				delta -= 360.0f;
			while (delta < -180.0f)
				delta += 360.0f;
			const float flLimit = halo_vehicle_look_clamp.GetFloat();
			if (delta > flLimit)
				delta = flLimit;
			else if (delta < -flLimit)
				delta = -flLimit;
			view.y = bodyYaw + delta;
			e->inputFacing = AnglesToForward(view);
		}
		e->inputControl = (pCmd->buttons & IN_JUMP) ? HALO_CONTROL_JUMP : 0u;
	}

	// The sim moves the player with the vehicle; the player's own movement is off.
	pCmd->forwardmove = 0.0f;
	pCmd->sidemove = 0.0f;
	pCmd->upmove = 0.0f;
	pCmd->buttons &= ~(IN_JUMP | IN_DUCK | IN_SPEED | IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
}

void HaloVehicle_LevelShutdown(void)
{
	for (HaloVehicleEntry_t& e : s_vehicles)
		e = HaloVehicleEntry_t{};
}

//-----------------------------------------------------------------------------
// script natives
//-----------------------------------------------------------------------------
static void* HaloVehicle_EntityFromStack(HSQUIRRELVM v, SQInteger idx)
{
	const SQObjectPtr& o = stack_get(v, idx);
	if (sq_isnull(o) || o._type != OT_ENTITY || !o._unVal.pInstance)
		return nullptr;
	return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o._unVal.pInstance) + 0x50);
}

static bool HaloVehicle_VectorFromStack(HSQUIRRELVM v, SQInteger idx, Vector3D& out)
{
	const SQVector3D* pVec = nullptr;
	if (SQ_FAILED(sq_getvector(v, idx, &pVec)) || !pVec)
		return false;
	out = Vector3D(pVec->x, pVec->y, pVec->z);
	return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z) &&
		fabsf(out.x) < 1.0e6f && fabsf(out.y) < 1.0e6f && fabsf(out.z) < 1.0e6f;
}

// HaloVehicle_Register(entity prop, string def, vector origin, vector angles) -> bool
static SQRESULT ServerScript_HaloVehicleRegister(HSQUIRRELVM v)
{
	void* const pProp = HaloVehicle_EntityFromStack(v, 2);
	const SQChar* pszDef = nullptr;
	Vector3D origin, angles;
	bool ok = false;

	if (pProp && SQ_SUCCEEDED(sq_getstring(v, 3, &pszDef)) && pszDef &&
		HaloVehicle_VectorFromStack(v, 4, origin) && HaloVehicle_VectorFromStack(v, 5, angles) &&
		!HaloVehicle_FindByProp(pProp) && HaloVehicle_Count() < halo_vehicle_max.GetInt())
	{
		const int defIndex = HaloVehicle_LoadDef(pszDef);
		const SDKEntityHandle h = SDKEntityState_GetHandle(pProp);
		if (defIndex >= 0 && h.IsValid())
		{
			for (HaloVehicleEntry_t& e : s_vehicles)
			{
				if (e.used)
					continue;
				e = HaloVehicleEntry_t{};
				e.used = true;
				e.defIndex = defIndex;
				e.prop = h;
				const float yaw = DEG2RAD(angles.y);
				const HaloVec3 forward = { cosf(yaw), sinf(yaw), 0.0f };
				HaloSim_Reset(s_defs[defIndex].def, e.state, ToWorldUnits(origin), forward, { 0.0f, 0.0f, 1.0f });
				e.inputFacing = forward;
				ok = true;
				// The S3 prop inherits a degenerate box; publish the authored
				// model-space bounds so the retail client culls and traces it.
				if (CCollisionProperty* const pColl = reinterpret_cast<CBaseEntity*>(pProp)->CollisionProp())
				{
					const HaloVehicleDef& regDef = s_defs[defIndex].def;
					pColl->SetBounds(ToInches(regDef.boundsMin), ToInches(regDef.boundsMax));
					MarkEntityEdictDirty(pProp);
					Msg(eDLL_T::SERVER, "[HALO-VEH] registered '%s' at (%.0f %.0f %.0f) yaw %.0f bounds=(%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n",
						pszDef, origin.x, origin.y, origin.z, angles.y,
						regDef.boundsMin.i * HALO_WU_TO_INCHES, regDef.boundsMin.j * HALO_WU_TO_INCHES,
						regDef.boundsMin.k * HALO_WU_TO_INCHES, regDef.boundsMax.i * HALO_WU_TO_INCHES,
						regDef.boundsMax.j * HALO_WU_TO_INCHES, regDef.boundsMax.k * HALO_WU_TO_INCHES);
				}
				else
				{
					Warning(eDLL_T::SERVER, "[HALO-VEH] registered '%s' with no CollisionProp -- bounds unpublished\n", pszDef);
				}
				break;
			}
		}
	}

	sq_pushbool(v, ok ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// HaloVehicle_SetSeat(entity prop, int seat, entity player /* null frees it */) -> bool
static SQRESULT ServerScript_HaloVehicleSetSeat(HSQUIRRELVM v)
{
	HaloVehicleEntry_t* const e = HaloVehicle_FindByProp(HaloVehicle_EntityFromStack(v, 2));
	SQInteger seat = -1;
	sq_getinteger(v, 3, &seat);
	void* const pPlayer = HaloVehicle_EntityFromStack(v, 4);
	bool ok = false;

	if (e && seat >= 0 && seat < HALO_SEAT_COUNT)
	{
		if (!pPlayer)
		{
			e->seats[seat] = SDKEntityHandle();
			if (seat == HALO_SEAT_DRIVER)
			{
				e->inputForward = 0.0f;
				e->inputLeft = 0.0f;
				e->inputControl = 0;
			}
			ok = true;
		}
		else if (!e->seats[seat].IsValid() && !HaloVehicle_FindBySeated(pPlayer, nullptr))
		{
			e->seats[seat] = SDKEntityState_GetHandle(pPlayer);
			e->state.objectFlags &= ~HALO_OBJ_AT_REST;
			ok = e->seats[seat].IsValid();
		}
	}

	sq_pushbool(v, ok ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// HaloVehicle_GetSeatOf(entity player) -> int, -1 when not seated
static SQRESULT ServerScript_HaloVehicleGetSeatOf(HSQUIRRELVM v)
{
	int seat = -1;
	HaloVehicle_FindBySeated(HaloVehicle_EntityFromStack(v, 2), &seat);
	sq_pushinteger(v, seat);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// HaloVehicle_IsVehicle(entity prop) -> bool
static SQRESULT ServerScript_HaloVehicleIsVehicle(HSQUIRRELVM v)
{
	sq_pushbool(v, HaloVehicle_FindByProp(HaloVehicle_EntityFromStack(v, 2)) ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// HaloVehicle_IsFlipped(entity prop) -> bool
static SQRESULT ServerScript_HaloVehicleIsFlipped(HSQUIRRELVM v)
{
	const HaloVehicleEntry_t* const e = HaloVehicle_FindByProp(HaloVehicle_EntityFromStack(v, 2));
	sq_pushbool(v, (e && HaloSim_IsFlipped(e->state)) ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// HaloVehicle_Flip(entity prop) -> bool: rolls a flipped vehicle back onto its wheels
static SQRESULT ServerScript_HaloVehicleFlip(HSQUIRRELVM v)
{
	HaloVehicleEntry_t* const e = HaloVehicle_FindByProp(HaloVehicle_EntityFromStack(v, 2));
	bool ok = false;
	if (e && HaloSim_IsFlipped(e->state))
	{
		// Roll toward whichever side is lower.
		const HaloVec3& f = e->state.forward;
		const HaloVec3& u = e->state.up;
		const float leftZ = u.i * f.j - u.j * f.i;
		HaloSim_StartUpending(e->state, leftZ > 0.0f ? 1 : 2);
		ok = true;
	}
	sq_pushbool(v, ok ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// HaloVehicle_GetSpeed(entity prop) -> float, inches per second
static SQRESULT ServerScript_HaloVehicleGetSpeed(HSQUIRRELVM v)
{
	const HaloVehicleEntry_t* const e = HaloVehicle_FindByProp(HaloVehicle_EntityFromStack(v, 2));
	float speed = 0.0f;
	if (e)
	{
		const HaloVec3& vel = e->state.translationalVelocity;
		speed = sqrtf(vel.i * vel.i + vel.j * vel.j + vel.k * vel.k) * HALO_WU_TO_INCHES * HALO_TICKS_PER_SECOND;
	}
	sq_pushfloat(v, speed);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// HaloVehicle_GetPose(entity prop) -> array<float>:
// [steering deg, wheel, wheel_rear, susp_lb, susp_lf, susp_rb, susp_rf, velocity]
static SQRESULT ServerScript_HaloVehicleGetPose(HSQUIRRELVM v)
{
	const HaloVehicleEntry_t* const e = HaloVehicle_FindByProp(HaloVehicle_EntityFromStack(v, 2));
	float out[8] = {};
	if (e)
	{
		const HaloVehicleDef& def = s_defs[e->defIndex].def;
		HaloVehiclePose pose;
		HaloSim_GetPose(def, e->state, pose);

		if (def.type == HALO_VEHICLE_ALIEN_FIGHTER)
		{
			// Fighters bank from yaw rate; the overlay screen spans 30 degrees each way.
			const HaloVec3& w = e->state.angularVelocity;
			const HaloVec3& u = e->state.up;
			const float yawRate = w.i * u.i + w.j * u.j + w.k * u.k;
			const float rate = def.turnRate > 0.0f ? yawRate / def.turnRate : 0.0f;
			out[0] = (rate < -1.0f ? -1.0f : (rate > 1.0f ? 1.0f : rate)) * 30.0f;
		}
		else
			out[0] = RAD2DEG(pose.steering);

		out[1] = pose.wheelPosition;
		out[2] = pose.wheelPositionRear;
		for (int i = 0; i < 4 && i < def.suspensionCount; ++i)
			out[3 + i] = pose.suspension[i];
		out[7] = pose.speedFraction;
	}

	sq_newarray(v, 0);
	for (const float f : out)
	{
		sq_pushfloat(v, f);
		sq_arrayappend(v, -2);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// HaloVehicle_Unregister(entity prop): stops simulating; seats are freed
static SQRESULT ServerScript_HaloVehicleUnregister(HSQUIRRELVM v)
{
	HaloVehicleEntry_t* const e = HaloVehicle_FindByProp(HaloVehicle_EntityFromStack(v, 2));
	if (e)
		*e = HaloVehicleEntry_t{};
	sq_pushbool(v, e ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void HaloVehicle_RegisterServerNatives(CSquirrelVM* s)
{
	if (!s)
		return;
	Script_RegisterFuncNamed(s, "HaloVehicle_Register", "Server_Script_HaloVehicle_Register",
		"Starts simulating a vehicle prop from scripts/cafefps_halovehicles/<def>.json", "bool",
		"entity prop, string def, vector origin, vector angles", false, ServerScript_HaloVehicleRegister);
	Script_RegisterFuncNamed(s, "HaloVehicle_SetSeat", "Server_Script_HaloVehicle_SetSeat",
		"Puts a player in a vehicle seat (0 driver, 1 gunner, 2 passenger); null frees it", "bool",
		"entity prop, int seat, entity player", false, ServerScript_HaloVehicleSetSeat);
	Script_RegisterFuncNamed(s, "HaloVehicle_GetSeatOf", "Server_Script_HaloVehicle_GetSeatOf",
		"Seat the player occupies, -1 when not in a vehicle", "int",
		"entity player", false, ServerScript_HaloVehicleGetSeatOf);
	Script_RegisterFuncNamed(s, "HaloVehicle_IsVehicle", "Server_Script_HaloVehicle_IsVehicle",
		"True when the prop is a simulated Halo vehicle", "bool",
		"entity prop", false, ServerScript_HaloVehicleIsVehicle);
	Script_RegisterFuncNamed(s, "HaloVehicle_IsFlipped", "Server_Script_HaloVehicle_IsFlipped",
		"True when the vehicle is upside down", "bool",
		"entity prop", false, ServerScript_HaloVehicleIsFlipped);
	Script_RegisterFuncNamed(s, "HaloVehicle_Flip", "Server_Script_HaloVehicle_Flip",
		"Rolls a flipped vehicle back onto its wheels", "bool",
		"entity prop", false, ServerScript_HaloVehicleFlip);
	Script_RegisterFuncNamed(s, "HaloVehicle_GetSpeed", "Server_Script_HaloVehicle_GetSpeed",
		"Vehicle speed in inches per second", "float",
		"entity prop", false, ServerScript_HaloVehicleGetSpeed);
	Script_RegisterFuncNamed(s, "HaloVehicle_GetPose", "Server_Script_HaloVehicle_GetPose",
		"Overlay pose values: steering deg, wheel, wheel_rear, susp_lb, susp_lf, susp_rb, susp_rf, velocity", "array<float>",
		"entity prop", false, ServerScript_HaloVehicleGetPose);
	Script_RegisterFuncNamed(s, "HaloVehicle_Unregister", "Server_Script_HaloVehicle_Unregister",
		"Stops simulating the vehicle and frees its seats", "bool",
		"entity prop", false, ServerScript_HaloVehicleUnregister);
	Msg(eDLL_T::SERVER, "[HALO-VEH] server natives registered\n");
}
