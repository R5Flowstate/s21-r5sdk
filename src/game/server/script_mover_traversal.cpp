//=============================================================================//
//
// Purpose: Non-physics script mover traversal. See script_mover_traversal.h.
//
// Nothing in the engine implements this API, so the controller is built from the
// script contract: every tunable arrives from script. Each tick the mover is
// handed a one-tick NonPhysicsMoveTo segment, which the client already
// interpolates for non-physics movers.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "script_mover_traversal.h"
#include "entity_script_ext.h"
#include "baseentity.h"
#include "trigger_cannon.h"
#include "vscript_server.h"
#include "game/shared/collisionproperty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/util_shared.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "mathlib/mathlib.h"
#include "public/cmodel.h"
#include "public/gametrace.h"
#include "public/engine/IEngineTrace.h"
#include "public/edict.h"
#include "engine/enginetrace.h"
#include <cfloat>
#include <cmath>
#include <cstring>
#include <initializer_list>

extern CGlobalVars* gpGlobals;

static constexpr ptrdiff_t SMT_OFF_PHYSICS_MOVER = 0x15CC; // set for SpawnAsPhysicsMover movers

static constexpr int VTBL_ISNPC_SLOT = 70;    // +560
static constexpr int VTBL_ISPLAYER_SLOT = 93; // +744

static constexpr int   SMT_MAX_MOVERS = 64;
static constexpr int   SMT_MAX_EDGE_ATTEMPTS = 16;
static constexpr int   SMT_MOVE_PARENT_DEPTH = 32;
static constexpr float SMT_WORLD_LIMIT = 131072.0f;
static constexpr float SMT_CLEAR_FRACTION = 0.9f;
static constexpr float SMT_BLOCKED_STOP_TIME = 0.5f;    // no step, slide or progress for this long
static constexpr float SMT_HORIZONTAL_ACCEL_SCALE = 4.0f; // reach cruise speed in a quarter second
static constexpr float SMT_MIN_HULL_HALF = 1.0f;
static constexpr float SMT_YAW_ACCEL_SCALE = 4.0f;      // reach full turn rate in a quarter second

// The drone hull the stock script uses for every traversal trace of its own;
// the mover's model bounds are the folded deployable and start inside the ground.
static const Vector3D SMT_HULL_MINS(-9.0f, -9.0f, -10.0f);
static const Vector3D SMT_HULL_MAXS(9.0f, 9.0f, 10.0f);

static ConVar bridge_traversal_trace("bridge_traversal_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[TRAV] traversal enable/goal/stop lines; 2 also prints the per-tick steer decision.");

struct TraversalState
{
	bool     m_bEnabled = false;
	float    m_flCheckDist = 0.0f;
	float    m_flLookahead = 0.0f;
	float    m_flSideVelocity = 0.0f;
	float    m_flMaxWidthCorrection = 0.0f;
	float    m_flTraceOffset = 0.0f;
	float    m_flForceDecay = 0.0f;
	int      m_nMask = 0;
	int      m_nCollisionGroup = 0;
	bool     m_bCollidePlayers = false;
	bool     m_bCollideNPCs = false;

	float    m_flHoverHeight = 0.0f;
	float    m_flGroundCheckDist = 0.0f;
	int      m_nGroundMask = 0;
	int      m_nGroundCollisionGroup = 0;

	float    m_flMaxSpeed = 0.0f;
	float    m_flInitialSpeed = 0.0f;
	float    m_flInitialDecayTime = 0.0f;
	float    m_flEnableTime = 0.0f;
	float    m_flDeaccelDist = 0.0f;
	float    m_flDeaccelMinSpeed = 0.0f;

	float    m_flYawAccelScale = 180.0f;
	bool     m_bYawDirSet = false;
	Vector3D m_vecYawDir = vec3_origin;

	bool     m_bLedge = false;
	float    m_flLedgeIgnoreResetDelay = 0.0f;
	float    m_flLedgeStepDist = 0.0f;
	float    m_flLedgeMaxDrop = 0.0f;
	float    m_flLedgeIgnoreUntil = 0.0f;

	bool     m_bSlide = false;
	float    m_flSlideMaxDist = 0.0f;
	int      m_nSlideEdgeAttempts = 0;
	float    m_flSlideGoalDist = 0.0f;
	float    m_flSlideMaxDot = 1.0f;
	float    m_flSlideTestDepth = 0.0f;

	bool     m_bStepOver = false;
	float    m_flStepMaxHeight = 0.0f;
	float    m_flStepCheckRange = 0.0f;
	float    m_flStepGoalDist = 0.0f;

	bool     m_bGravity = false;
	float    m_flGravityScale = 0.0f;

	bool     m_bHasGoal = false;
	Vector3D m_vecGoal = vec3_origin;     // world space, or ground-local when m_hGround is set
	uint32_t m_hGround = INVALID_EHANDLE_INDEX;

	bool     m_bStopped = false;
	float    m_flBlockedTime = 0.0f;
	Vector3D m_vecVelocity = vec3_origin;
	float    m_flYawRate = 0.0f;          // deg/s
};

static SDKEntityMap<TraversalState> s_travMap(ESide::Server, "moverTraversal.srv");

static ScriptClassDescriptor_t* s_pScriptMoverDesc = nullptr;
static void (*v_Script_RegisterScriptMoverClassFuncs)() = nullptr;
static void (*v_CScriptMover__NonPhysicsMoveTo)(void* pMover, const Vector3D* pPos, float flTime, float flEaseIn, float flEaseOut) = nullptr;
static void (*v_CScriptMover__NonPhysicsRotateTo)(void* pMover, const QAngle* pAngles, float flTime, float flEaseIn, float flEaseOut) = nullptr;

using ScriptNative_t = SQRESULT(*)(HSQUIRRELVM);
static ScriptNative_t s_origSetMaxSpeed = nullptr;
static ScriptNative_t s_origSetMinimalHeightGround = nullptr;
static ScriptNative_t s_origClearDesiredYaw = nullptr;
static ScriptNative_t s_origGetMoveToPositionWorld = nullptr;

static HSCRIPT s_hStoppedCallback = nullptr;
static HSQUIRRELVM s_hStoppedCallbackVM = nullptr;
static ConVar* s_pGravityVar = nullptr;
static ConVar* s_pMoverSupportVar = nullptr;

class CTraversalEntityAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_hMoveParent;
};

static void* SMT_MoveParent(void* pEntity)
{
	const CTraversalEntityAccess* const pAccess = static_cast<CTraversalEntityAccess*>(reinterpret_cast<CBaseEntity*>(pEntity));
	return SDKEntityState_Resolve(SDKEntityHandle(static_cast<uint32_t>(pAccess->m_hMoveParent.ToInt())), ESide::Server);
}

static void* SMT_RootMoveParent(void* pEntity)
{
	void* pRoot = pEntity;
	for (int i = 0; i < SMT_MOVE_PARENT_DEPTH; ++i)
	{
		void* const pParent = SMT_MoveParent(pRoot);
		if (!pParent)
			break;
		pRoot = pParent;
	}
	return pRoot;
}

static bool SMT_VirtualBool(void* pEntity, const int nSlot)
{
	void** const vtable = *reinterpret_cast<void***>(pEntity);
	return reinterpret_cast<bool(__fastcall*)(void*)>(vtable[nSlot])(pEntity);
}

static float SMT_Gravity(void)
{
	if (!s_pGravityVar && g_pCVar)
		s_pGravityVar = g_pCVar->FindVar("sv_gravity");
	const float fl = s_pGravityVar ? s_pGravityVar->GetFloat() : 750.0f;
	return std::isfinite(fl) ? fl : 750.0f;
}

static bool SMT_MoverSupport(void)
{
	if (!s_pMoverSupportVar && g_pCVar)
		s_pMoverSupportVar = g_pCVar->FindVar("script_mover_traversal_mover_support");
	return s_pMoverSupportVar && s_pMoverSupportVar->GetBool();
}

static inline float SMT_Dist2D(const Vector3D& a, const Vector3D& b)
{
	const float dx = a.x - b.x;
	const float dy = a.y - b.y;
	return sqrtf(dx * dx + dy * dy);
}

static inline bool SMT_IsFiniteVec(const Vector3D& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)
		&& fabsf(v.x) <= SMT_WORLD_LIMIT && fabsf(v.y) <= SMT_WORLD_LIMIT && fabsf(v.z) <= SMT_WORLD_LIMIT;
}

static inline Vector3D SMT_Rotate2D(const Vector3D& dir, const float flDegrees)
{
	const float rad = DEG2RAD(flDegrees);
	const float c = cosf(rad);
	const float s = sinf(rad);
	return Vector3D(dir.x * c - dir.y * s, dir.x * s + dir.y * c, 0.0f);
}

//-----------------------------------------------------------------------------
// Probes. The mover never hits itself, anything riding it, or players / NPCs
// unless the traversal asked to collide with them.
//-----------------------------------------------------------------------------
class CTraversalTraceFilter : public CTraceFilterSimple
{
public:
	CTraversalTraceFilter(void* pMover, const int nCollisionGroup, const bool bPlayers, const bool bNPCs)
		: CTraceFilterSimple(static_cast<const IHandleEntity*>(pMover), nCollisionGroup),
		m_pMover(pMover), m_bPlayers(bPlayers), m_bNPCs(bNPCs)
	{
	}

	virtual bool ShouldHitEntity(IHandleEntity* const pEntity, const int contentsMask) override
	{
		if (pEntity)
		{
			void* const pEnt = pEntity;
			if (SMT_RootMoveParent(pEnt) == m_pMover)
				return false;
			if (!m_bPlayers && SMT_VirtualBool(pEnt, VTBL_ISPLAYER_SLOT))
				return false;
			if (!m_bNPCs && SMT_VirtualBool(pEnt, VTBL_ISNPC_SLOT))
				return false;
		}
		return CTraceFilterSimple::ShouldHitEntity(pEntity, contentsMask);
	}

private:
	void* m_pMover;
	bool  m_bPlayers;
	bool  m_bNPCs;
};

static void SMT_TraceHull(const Vector3D& start, const Vector3D& end, const Vector3D& mins, const Vector3D& maxs,
	const unsigned int nMask, ITraceFilter* pFilter, trace_t& tr)
{
	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1.0f;
	tr.endpos = end;
	if (!g_pEngineTraceServer)
		return;

	alignas(16) Ray_t ray;
	memset(&ray, 0, sizeof(ray));
	const bool bPoint = (maxs.x - mins.x) < SMT_MIN_HULL_HALF && (maxs.y - mins.y) < SMT_MIN_HULL_HALF
		&& (maxs.z - mins.z) < SMT_MIN_HULL_HALF;
	if (!bPoint && v_Ray_t_InitStartEndMinsMaxsUp)
	{
		const Vector3D up(0.0f, 0.0f, 1.0f);
		v_Ray_t_InitStartEndMinsMaxsUp(&ray, &start, &end, &mins, &maxs, &up);
	}
	else
	{
		ray.Init(start, end, 0x3f800000, 0);
	}
	g_pEngineTraceServer->TraceRayFiltered(ray, nMask, pFilter, &tr);
}

// A probe that starts inside geometry says nothing about the path ahead.
static inline bool SMT_Clear(const trace_t& tr)
{
	return tr.startsolid || (!tr.allsolid && tr.fraction >= SMT_CLEAR_FRACTION);
}

//-----------------------------------------------------------------------------
// Stop + callback.
//-----------------------------------------------------------------------------
static HSCRIPT SMT_StoppedCallback(void)
{
	if (!g_pServerScript)
		return nullptr;
	const HSQUIRRELVM hVM = g_pServerScript->GetVM();
	if (!hVM)
		return nullptr;
	if (s_hStoppedCallbackVM != hVM)
	{
		s_hStoppedCallbackVM = hVM;
		s_hStoppedCallback = g_pServerScript->FindFunction("CodeCallback_ScriptMoverTraversalStopped", nullptr, nullptr);
	}
	return s_hStoppedCallback;
}

static void SMT_Stop(void* pMover, TraversalState& s, const bool bBlocked, const char* pszWhy)
{
	if (s.m_bStopped)
		return;

	s.m_bStopped = true;
	s.m_flBlockedTime = 0.0f;
	s.m_vecVelocity = vec3_origin;

	if (bridge_traversal_trace.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[TRAV] stop mover=%p blocked=%d (%s)\n", pMover, bBlocked ? 1 : 0, pszWhy);

	const HSCRIPT hFunc = SMT_StoppedCallback();
	const HSCRIPT hMover = hFunc ? reinterpret_cast<CBaseEntity*>(pMover)->GetScriptInstance() : nullptr;
	if (!hMover)
		return;

	ScriptVariant_t args[2];
	args[0] = hMover;
	args[1] = bBlocked;
	if (g_pServerScript->ExecuteFunction(hFunc, args, 2, nullptr, nullptr) == SCRIPT_ERROR)
		Warning(eDLL_T::SERVER, "[TRAV] CodeCallback_ScriptMoverTraversalStopped SCRIPT_ERROR mover=%p\n", pMover);
}

static Vector3D SMT_GoalWorld(const TraversalState& s)
{
	if (s.m_hGround == INVALID_EHANDLE_INDEX)
		return s.m_vecGoal;

	void* const pGround = SDKEntityState_Resolve(SDKEntityHandle(s.m_hGround), ESide::Server);
	if (!pGround || !v_CBaseEntity_EntityToWorldTransform)
		return s.m_vecGoal;

	TriggerPass_EnsureAbsOrigin(pGround);
	const matrix3x4_t* const mat = v_CBaseEntity_EntityToWorldTransform(reinterpret_cast<CBaseEntity*>(pGround));
	if (!mat)
		return s.m_vecGoal;
	Vector3D world;
	VectorTransform(s.m_vecGoal, *mat, world);
	return world;
}

//-----------------------------------------------------------------------------
// One tick of one mover.
//-----------------------------------------------------------------------------
static void SMT_Tick(void* pMover, TraversalState& s, const float dt)
{
	CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pMover);
	TriggerPass_EnsureAbsOrigin(pMover);
	const Vector3D origin = pEnt->Diag_AbsOrigin();
	const Vector3D& mins = SMT_HULL_MINS;
	const Vector3D& maxs = SMT_HULL_MAXS;
	const float flNow = gpGlobals->curTime;

	CTraversalTraceFilter moveFilter(pMover, s.m_nCollisionGroup, s.m_bCollidePlayers, s.m_bCollideNPCs);
	CTraversalTraceFilter groundFilter(pMover, s.m_nGroundCollisionGroup, s.m_bCollidePlayers, s.m_bCollideNPCs);
	trace_t tr;

	// Ground under the mover.
	const Vector3D groundEnd(origin.x, origin.y, origin.z - (s.m_flHoverHeight + s.m_flGroundCheckDist));
	SMT_TraceHull(origin, groundEnd, mins, maxs, static_cast<unsigned int>(s.m_nGroundMask), &groundFilter, tr);
	// Starting inside the ground holds the current height instead of climbing forever.
	const bool bGround = tr.startsolid || (tr.fraction < 1.0f && !tr.allsolid);
	const float flGroundZ = tr.startsolid ? origin.z - s.m_flHoverHeight : tr.endpos.z;
	Vector3D carry = vec3_origin;
	if (bGround && tr.hit_entity)
	{
		void* const pGroundRoot = SMT_RootMoveParent(tr.hit_entity);
		if (pGroundRoot && pGroundRoot != pMover)
			carry = EntityScriptExt_AbsVelocityAtPoint(pGroundRoot, origin);
	}

	// Horizontal.
	Vector3D steer = vec3_origin;
	float flSpeed = 0.0f;
	float flClimbTarget = -FLT_MAX;
	const Vector3D goal = SMT_GoalWorld(s);

	if (!s.m_bStopped && s.m_bHasGoal)
	{
		const float flDist2D = SMT_Dist2D(goal, origin);
		if (flDist2D > s.m_flCheckDist)
		{
			flSpeed = s.m_flMaxSpeed;
			const float flSince = flNow - s.m_flEnableTime;
			if (flSince < s.m_flInitialDecayTime && s.m_flInitialSpeed > flSpeed && s.m_flInitialDecayTime > 0.0f)
				flSpeed = s.m_flInitialSpeed + (flSpeed - s.m_flInitialSpeed) * (flSince / s.m_flInitialDecayTime);

			const Vector3D toGoal = goal - origin;
			const float flDist3D = sqrtf(toGoal.x * toGoal.x + toGoal.y * toGoal.y + toGoal.z * toGoal.z);
			if (s.m_flDeaccelDist > 0.0f && flDist3D < s.m_flDeaccelDist)
				flSpeed = s.m_flDeaccelMinSpeed + (flSpeed - s.m_flDeaccelMinSpeed) * (flDist3D / s.m_flDeaccelDist);

			const Vector3D dir((goal.x - origin.x) / flDist2D, (goal.y - origin.y) / flDist2D, 0.0f);
			const Vector3D probeStart(origin.x, origin.y, origin.z + s.m_flTraceOffset);
			const float flLook = fminf(s.m_flLookahead, flDist2D);
			SMT_TraceHull(probeStart, probeStart + dir * flLook, mins, maxs, static_cast<unsigned int>(s.m_nMask), &moveFilter, tr);

			if (SMT_Clear(tr))
			{
				steer = dir;
			}
			else
			{
				const Vector3D wallNormal = tr.plane.normal;
				const float flHitDist = tr.fraction * flLook;

				if (s.m_bStepOver && flHitDist <= s.m_flStepCheckRange && flDist2D > s.m_flStepGoalDist)
				{
					const Vector3D upStart(probeStart.x, probeStart.y, probeStart.z + s.m_flStepMaxHeight);
					trace_t trUp;
					SMT_TraceHull(probeStart, upStart, mins, maxs, static_cast<unsigned int>(s.m_nMask), &moveFilter, trUp);
					if (SMT_Clear(trUp))
					{
						SMT_TraceHull(upStart, upStart + dir * flLook, mins, maxs, static_cast<unsigned int>(s.m_nMask), &moveFilter, trUp);
						if (SMT_Clear(trUp))
						{
							steer = dir;
							flClimbTarget = origin.z + s.m_flStepMaxHeight;
						}
					}
				}

				if (steer == vec3_origin && s.m_bSlide && flDist2D > s.m_flSlideGoalDist && flDist2D < s.m_flSlideMaxDist)
				{
					const int nAttempts = s.m_nSlideEdgeAttempts;
					const int nPerSide = (nAttempts + 1) / 2;
					const float flStepDeg = nPerSide > 0 ? 90.0f / static_cast<float>(nPerSide) : 90.0f;
					for (int i = 0; i < nAttempts; ++i)
					{
						const float flSign = (i & 1) ? -1.0f : 1.0f;
						const Vector3D cand = SMT_Rotate2D(dir, flSign * flStepDeg * static_cast<float>(i / 2 + 1));
						if (DotProduct(cand, -wallNormal) >= s.m_flSlideMaxDot)
							continue;
						trace_t trSlide;
						SMT_TraceHull(probeStart, probeStart + cand * s.m_flSlideTestDepth, mins, maxs,
							static_cast<unsigned int>(s.m_nMask), &moveFilter, trSlide);
						if (!SMT_Clear(trSlide))
							continue;
						if (SMT_Dist2D(trSlide.endpos, goal) < flDist2D)
						{
							steer = cand;
							break;
						}
					}
				}
			}

			if (steer == vec3_origin)
			{
				s.m_flBlockedTime += dt;
				flSpeed = 0.0f;
				if (s.m_flBlockedTime >= SMT_BLOCKED_STOP_TIME)
					SMT_Stop(pMover, s, true, "blocked");
			}
			else
			{
				s.m_flBlockedTime = 0.0f;
			}

			// A new goal set after a stop re-arms ledge checks only after the reset delay.
			if (!s.m_bStopped && steer != vec3_origin && s.m_bLedge && flNow >= s.m_flLedgeIgnoreUntil)
			{
				const Vector3D ledgeProbe = origin + steer * s.m_flLedgeStepDist;
				trace_t trLedge;
				SMT_TraceHull(origin, ledgeProbe, mins, maxs, static_cast<unsigned int>(s.m_nMask), &moveFilter, trLedge);
				if (SMT_Clear(trLedge))
				{
					SMT_TraceHull(ledgeProbe, Vector3D(ledgeProbe.x, ledgeProbe.y, ledgeProbe.z - s.m_flLedgeMaxDrop),
						mins, maxs, static_cast<unsigned int>(s.m_nGroundMask), &groundFilter, trLedge);
					if (trLedge.fraction >= 1.0f)
						SMT_Stop(pMover, s, false, "ledge");
				}
			}

			if (bridge_traversal_trace.GetInt() >= 2)
				Msg(eDLL_T::SERVER, "[TRAV] mover=%p at=(%.1f %.1f %.1f) goal=(%.1f %.1f %.1f) dist=%.1f speed=%.1f steer=(%.2f %.2f) blocked=%.2f\n",
					pMover, origin.x, origin.y, origin.z, goal.x, goal.y, goal.z, flDist2D, flSpeed, steer.x, steer.y, s.m_flBlockedTime);
		}
	}
	if (s.m_bStopped)
	{
		steer = vec3_origin;
		flSpeed = 0.0f;
	}

	// Horizontal velocity eases toward the steered cruise velocity.
	const Vector3D wantH = steer * flSpeed;
	const float flAccel = fmaxf(fmaxf(s.m_flMaxSpeed, s.m_flSideVelocity), 1.0f) * SMT_HORIZONTAL_ACCEL_SCALE * dt;
	Vector3D velH(s.m_vecVelocity.x, s.m_vecVelocity.y, 0.0f);
	const Vector3D deltaH = wantH - velH;
	const float flDeltaLen = sqrtf(deltaH.x * deltaH.x + deltaH.y * deltaH.y);
	if (flDeltaLen <= flAccel || flDeltaLen <= 0.0f)
		velH = wantH;
	else
		velH += deltaH * (flAccel / flDeltaLen);

	// Vertical: settle to the hover height; the gravity scale sets how hard it
	// falls toward it and how fast it climbs a step.
	float flVelZ = s.m_vecVelocity.z;
	const float flGravity = SMT_Gravity() * (s.m_bGravity ? s.m_flGravityScale : 1.0f);
	if (bGround || flClimbTarget > -FLT_MAX)
	{
		float flTargetZ = bGround ? flGroundZ + s.m_flHoverHeight : origin.z;
		if (flClimbTarget > flTargetZ)
			flTargetZ = flClimbTarget;
		const float flErr = flTargetZ - origin.z;
		if (!s.m_bGravity)
		{
			flVelZ = flErr / fmaxf(dt, 1.0e-4f);
		}
		else
		{
			const float flWant = (flErr >= 0.0f ? 1.0f : -1.0f) * sqrtf(2.0f * flGravity * fabsf(flErr));
			const float flStep = 2.0f * flGravity * dt;
			flVelZ = (fabsf(flWant - flVelZ) <= flStep) ? flWant : flVelZ + (flWant > flVelZ ? flStep : -flStep);
			if ((flErr > 0.0f && flVelZ * dt > flErr) || (flErr < 0.0f && flVelZ * dt < flErr))
				flVelZ = flErr / fmaxf(dt, 1.0e-4f);
		}
	}
	else
	{
		flVelZ -= flGravity * dt;
	}

	s.m_vecVelocity = Vector3D(velH.x, velH.y, flVelZ);
	Vector3D target = origin + (s.m_vecVelocity + carry) * dt;
	SMT_TraceHull(origin, target, mins, maxs, static_cast<unsigned int>(s.m_nMask), &moveFilter, tr);
	if (!tr.startsolid && tr.fraction < 1.0f)
		target = tr.endpos;
	if (!SMT_IsFiniteVec(target))
		target = origin;

	if (v_CScriptMover__NonPhysicsMoveTo)
		v_CScriptMover__NonPhysicsMoveTo(pMover, &target, dt, 0.0f, 0.0f);

	// Traversal never turns the mover on its own: travel keeps the current facing
	// and only a desired yaw direction from script rotates it.
	if (!s.m_bYawDirSet)
	{
		s.m_flYawRate = 0.0f;
		return;
	}
	const Vector3D faceDir = s.m_vecYawDir;
	if (faceDir.x * faceDir.x + faceDir.y * faceDir.y > 1.0e-4f)
	{
		const float flWantYaw = RAD2DEG(atan2f(faceDir.y, faceDir.x));
		// RotateTo rejects angles beyond +-360, so step from the live yaw.
		const float flBaseYaw = AngleNormalize(pEnt->Diag_AbsRotation().y);
		const float flDiff = AngleDiff(flWantYaw, flBaseYaw);

		// Ease in and out: the rate ramps at the yaw accel and brakes to land on the heading.
		const float flMaxRate = fmaxf(s.m_flYawAccelScale, 1.0f);
		const float flAccel = flMaxRate * SMT_YAW_ACCEL_SCALE;
		const float flWantRate = copysignf(fminf(flMaxRate, sqrtf(2.0f * flAccel * fabsf(flDiff))), flDiff);
		const float flRateStep = flAccel * dt;
		s.m_flYawRate += fmaxf(-flRateStep, fminf(flRateStep, flWantRate - s.m_flYawRate));
		float flStep = s.m_flYawRate * dt;
		if (flStep * flDiff > 0.0f && fabsf(flStep) >= fabsf(flDiff))
		{
			flStep = flDiff;
			s.m_flYawRate = 0.0f;
		}

		if (v_CScriptMover__NonPhysicsRotateTo && fabsf(flStep) > 0.001f)
		{
			const QAngle angles(0.0f, flBaseYaw + flStep, 0.0f);
			v_CScriptMover__NonPhysicsRotateTo(pMover, &angles, dt, 0.0f, 0.0f);
		}
	}
}

void ScriptMoverTraversal_Frame(void)
{
	if (!gpGlobals || !s_travMap.Size())
		return;

	const float dt = gpGlobals->tickInterval > 0.0f ? gpGlobals->tickInterval : gpGlobals->frameTime;
	if (dt <= 0.0f)
		return;

	// Script callbacks may destroy movers; walk a snapshot of the handles.
	SDKEntityHandle handles[SMT_MAX_MOVERS];
	int nHandles = 0;
	for (const auto& kv : s_travMap)
	{
		if (nHandles >= SMT_MAX_MOVERS)
			break;
		if (kv.second.m_bEnabled)
			handles[nHandles++] = kv.first;
	}

	for (int i = 0; i < nHandles; ++i)
	{
		void* const pMover = SDKEntityState_Resolve(handles[i], ESide::Server);
		TraversalState* const pState = pMover ? s_travMap.Find(handles[i]) : nullptr;
		if (!pState || !pState->m_bEnabled || SMT_MoveParent(pMover))
			continue;
		SMT_Tick(pMover, *pState, dt);
	}
}

void ScriptMoverTraversal_LevelShutdown(void)
{
	s_travMap.Clear();
	s_hStoppedCallback = nullptr;
	s_hStoppedCallbackVM = nullptr;
}

//-----------------------------------------------------------------------------
// Script natives.
//-----------------------------------------------------------------------------
static void* SMT_ScriptThis(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return nullptr;
	return pEntity;
}

static TraversalState* SMT_ActiveState(void* pMover)
{
	TraversalState* const s = pMover ? s_travMap.Find(pMover) : nullptr;
	return (s && s->m_bEnabled) ? s : nullptr;
}

static bool SMT_ReadFloats(HSQUIRRELVM v, const SQInteger nFirst, float* pOut, const int nCount, const char* pszNative)
{
	for (int i = 0; i < nCount; ++i)
	{
		SQFloat f = 0.0f;
		if (SQ_FAILED(sq_getfloat(v, nFirst + i, &f)) || !std::isfinite(f))
		{
			Warning(eDLL_T::SERVER, "[TRAV] %s rejected argument %d\n", pszNative, static_cast<int>(nFirst + i - 1));
			return false;
		}
		pOut[i] = static_cast<float>(f);
	}
	return true;
}

// Script-reached state lives only in active traversal; natives on other movers are no-ops.
static TraversalState* SMT_RequireTraversal(HSQUIRRELVM v, void** ppMover, const char* pszNative)
{
	*ppMover = SMT_ScriptThis(v);
	TraversalState* const s = SMT_ActiveState(*ppMover);
	if (!s && *ppMover)
		Warning(eDLL_T::SERVER, "[TRAV] %s called on a mover without EnableNonPhysicsTraversal\n", pszNative);
	return s;
}

static SQRESULT Script_EnableNonPhysicsTraversal(HSQUIRRELVM v)
{
	void* const pMover = SMT_ScriptThis(v);
	if (!pMover)
		return SQ_ERROR;
	if (*reinterpret_cast<const uint8_t*>(static_cast<uint8_t*>(pMover) + SMT_OFF_PHYSICS_MOVER))
	{
		Warning(eDLL_T::SERVER, "[TRAV] EnableNonPhysicsTraversal is not valid with physics-movement scriptmovers\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	float f[6];
	SQInteger nMask = 0, nGroup = 0;
	SQBool bPlayers = SQFalse, bNPCs = SQFalse;
	if (!SMT_ReadFloats(v, 2, f, 6, "EnableNonPhysicsTraversal"))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	sq_getinteger(v, 8, &nMask);
	sq_getinteger(v, 9, &nGroup);
	sq_getbool(v, 10, &bPlayers);
	sq_getbool(v, 11, &bNPCs);

	TraversalState& s = s_travMap[pMover];
	s = TraversalState();
	s.m_bEnabled = true;
	s.m_flCheckDist = f[0];
	s.m_flLookahead = f[1];
	s.m_flSideVelocity = f[2];
	s.m_flMaxWidthCorrection = f[3];
	s.m_flTraceOffset = f[4];
	s.m_flForceDecay = f[5];
	s.m_nMask = static_cast<int>(nMask);
	s.m_nCollisionGroup = static_cast<int>(nGroup);
	s.m_bCollidePlayers = bPlayers != SQFalse;
	s.m_bCollideNPCs = bNPCs != SQFalse;
	s.m_nGroundMask = s.m_nMask;
	s.m_nGroundCollisionGroup = s.m_nCollisionGroup;
	s.m_flEnableTime = gpGlobals ? gpGlobals->curTime : 0.0f;
	s.m_vecVelocity = reinterpret_cast<CBaseEntity*>(pMover)->Diag_AbsVelocity();
	if (!SMT_IsFiniteVec(s.m_vecVelocity))
		s.m_vecVelocity = vec3_origin;
	s.m_flYawRate = 0.0f;

	if (bridge_traversal_trace.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[TRAV] enable mover=%p check=%.1f look=%.1f mask=0x%X group=%d\n",
			pMover, s.m_flCheckDist, s.m_flLookahead, s.m_nMask, s.m_nCollisionGroup);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetMoveToPositionGroundNonPhysics(HSQUIRRELVM v)
{
	void* pMover = nullptr;
	TraversalState* const s = SMT_RequireTraversal(v, &pMover, "SetMoveToPositionGroundNonPhysics");
	if (!pMover)
		return SQ_ERROR;
	if (!s)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	const SQVector3D* pPos = nullptr;
	if (SQ_FAILED(sq_getvector(v, 2, &pPos)) || !pPos)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	const Vector3D goal(pPos->x, pPos->y, pPos->z);
	if (!SMT_IsFiniteVec(goal))
	{
		Warning(eDLL_T::SERVER, "[TRAV] SetMoveToPositionGroundNonPhysics rejected an out-of-range goal\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* pGround = nullptr;
	const SQObjectPtr& o = stack_get(v, 3);
	if (o._type == OT_ENTITY && o._unVal.pInstance)
		pGround = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o._unVal.pInstance) + 0x50);

	s->m_vecGoal = goal;
	s->m_hGround = INVALID_EHANDLE_INDEX;
	if (pGround && pGround != pMover && SMT_MoverSupport() && v_CBaseEntity_EntityToWorldTransform)
	{
		TriggerPass_EnsureAbsOrigin(pGround);
		const SDKEntityHandle hGround = SDKEntityState_GetHandle(pGround);
		const matrix3x4_t* const mat = v_CBaseEntity_EntityToWorldTransform(reinterpret_cast<CBaseEntity*>(pGround));
		// The world is not a carrier, and its transform does not round-trip.
		if (mat && hGround.IsValid() && hGround.ToBaseHandle().GetEntryIndex() != 0)
		{
			Vector3D local, back;
			VectorITransform(goal, *mat, local);
			VectorTransform(local, *mat, back);
			const Vector3D err = back - goal;
			if (err.x * err.x + err.y * err.y + err.z * err.z < 1.0f)
			{
				s->m_vecGoal = local;
				s->m_hGround = hGround.Raw();
			}
		}
	}
	s->m_bHasGoal = true;
	s->m_flBlockedTime = 0.0f;
	if (s->m_bStopped)
	{
		s->m_bStopped = false;
		s->m_flLedgeIgnoreUntil = (gpGlobals ? gpGlobals->curTime : 0.0f) + s->m_flLedgeIgnoreResetDelay;
	}

	if (bridge_traversal_trace.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[TRAV] goal mover=%p (%.1f %.1f %.1f) ground=%p\n", pMover, goal.x, goal.y, goal.z, pGround);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetInitialSpeed(HSQUIRRELVM v)
{
	void* pMover = nullptr;
	TraversalState* const s = SMT_RequireTraversal(v, &pMover, "SetInitialSpeed");
	if (!pMover)
		return SQ_ERROR;
	float f[2];
	if (s && SMT_ReadFloats(v, 2, f, 2, "SetInitialSpeed"))
	{
		s->m_flInitialSpeed = f[0];
		s->m_flInitialDecayTime = f[1];
		s->m_flEnableTime = gpGlobals ? gpGlobals->curTime : 0.0f;
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_EnableDeaccelerationApproachingDest(HSQUIRRELVM v)
{
	void* pMover = nullptr;
	TraversalState* const s = SMT_RequireTraversal(v, &pMover, "EnableDeaccelerationApproachingDest");
	if (!pMover)
		return SQ_ERROR;
	float f[2];
	if (s && SMT_ReadFloats(v, 2, f, 2, "EnableDeaccelerationApproachingDest"))
	{
		s->m_flDeaccelDist = f[0];
		s->m_flDeaccelMinSpeed = f[1];
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetYawAccelerationScale(HSQUIRRELVM v)
{
	void* pMover = nullptr;
	TraversalState* const s = SMT_RequireTraversal(v, &pMover, "SetYawAccelerationScale");
	if (!pMover)
		return SQ_ERROR;
	float f = 0.0f;
	if (s && SMT_ReadFloats(v, 2, &f, 1, "SetYawAccelerationScale"))
		s->m_flYawAccelScale = fmaxf(f, 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_EnableLedgeChecking(HSQUIRRELVM v)
{
	void* pMover = nullptr;
	TraversalState* const s = SMT_RequireTraversal(v, &pMover, "EnableLedgeChecking");
	if (!pMover)
		return SQ_ERROR;
	float f[3];
	if (s && SMT_ReadFloats(v, 2, f, 3, "EnableLedgeChecking"))
	{
		s->m_bLedge = true;
		s->m_flLedgeIgnoreResetDelay = f[0];
		s->m_flLedgeStepDist = f[1];
		s->m_flLedgeMaxDrop = f[2];
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_EnableSliding(HSQUIRRELVM v)
{
	void* pMover = nullptr;
	TraversalState* const s = SMT_RequireTraversal(v, &pMover, "EnableSliding");
	if (!pMover)
		return SQ_ERROR;
	float flMaxDist = 0.0f;
	float f[3];
	SQInteger nAttempts = 0;
	if (s && SMT_ReadFloats(v, 2, &flMaxDist, 1, "EnableSliding") && SQ_SUCCEEDED(sq_getinteger(v, 3, &nAttempts))
		&& SMT_ReadFloats(v, 4, f, 3, "EnableSliding"))
	{
		s->m_bSlide = true;
		s->m_flSlideMaxDist = flMaxDist;
		s->m_nSlideEdgeAttempts = static_cast<int>(nAttempts < 0 ? 0 : (nAttempts > SMT_MAX_EDGE_ATTEMPTS ? SMT_MAX_EDGE_ATTEMPTS : nAttempts));
		s->m_flSlideGoalDist = f[0];
		s->m_flSlideMaxDot = f[1];
		s->m_flSlideTestDepth = f[2];
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_EnableStepOver(HSQUIRRELVM v)
{
	void* pMover = nullptr;
	TraversalState* const s = SMT_RequireTraversal(v, &pMover, "EnableStepOver");
	if (!pMover)
		return SQ_ERROR;
	float f[3];
	if (s && SMT_ReadFloats(v, 2, f, 3, "EnableStepOver"))
	{
		s->m_bStepOver = true;
		s->m_flStepMaxHeight = f[0];
		s->m_flStepCheckRange = f[1];
		s->m_flStepGoalDist = f[2];
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_EnableGravityAboveMinHeight(HSQUIRRELVM v)
{
	void* pMover = nullptr;
	TraversalState* const s = SMT_RequireTraversal(v, &pMover, "EnableGravityAboveMinHeight");
	if (!pMover)
		return SQ_ERROR;
	float f = 0.0f;
	if (s && SMT_ReadFloats(v, 2, &f, 1, "EnableGravityAboveMinHeight"))
	{
		s->m_bGravity = true;
		s->m_flGravityScale = fmaxf(f, 0.0f);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetDesiredYawDir(HSQUIRRELVM v)
{
	void* pMover = nullptr;
	TraversalState* const s = SMT_RequireTraversal(v, &pMover, "SetDesiredYawDir");
	if (!pMover)
		return SQ_ERROR;
	const SQVector3D* pDir = nullptr;
	if (s && SQ_SUCCEEDED(sq_getvector(v, 2, &pDir)) && pDir)
	{
		const Vector3D dir(pDir->x, pDir->y, 0.0f);
		if (std::isfinite(dir.x) && std::isfinite(dir.y) && (dir.x != 0.0f || dir.y != 0.0f))
		{
			s->m_vecYawDir = dir;
			s->m_bYawDirSet = true;
		}
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Overrides: traversal movers keep their own state, every other mover gets the engine native.
static SQRESULT Script_SetMaxSpeed(HSQUIRRELVM v)
{
	TraversalState* const s = SMT_ActiveState(SMT_ScriptThis(v));
	if (!s)
		return s_origSetMaxSpeed ? s_origSetMaxSpeed(v) : SQ_ERROR;
	float f = 0.0f;
	if (SMT_ReadFloats(v, 2, &f, 1, "SetMaxSpeed"))
		s->m_flMaxSpeed = fmaxf(f, 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetMinimalHeightGround(HSQUIRRELVM v)
{
	TraversalState* const s = SMT_ActiveState(SMT_ScriptThis(v));
	if (!s)
		return s_origSetMinimalHeightGround ? s_origSetMinimalHeightGround(v) : SQ_ERROR;
	float f[2];
	if (SMT_ReadFloats(v, 2, f, 2, "SetMinimalHeightGround"))
	{
		s->m_flHoverHeight = f[0];
		s->m_flGroundCheckDist = fmaxf(f[1], 0.0f);
	}
	if (sq_gettop(v) >= 5)
	{
		SQInteger nMask = 0, nGroup = 0;
		if (SQ_SUCCEEDED(sq_getinteger(v, 4, &nMask)) && nMask != 0)
			s->m_nGroundMask = static_cast<int>(nMask);
		if (SQ_SUCCEEDED(sq_getinteger(v, 5, &nGroup)))
			s->m_nGroundCollisionGroup = static_cast<int>(nGroup);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_ClearDesiredYaw(HSQUIRRELVM v)
{
	TraversalState* const s = SMT_ActiveState(SMT_ScriptThis(v));
	if (!s)
		return s_origClearDesiredYaw ? s_origClearDesiredYaw(v) : SQ_ERROR;
	s->m_bYawDirSet = false;
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetMoveToPositionWorld(HSQUIRRELVM v)
{
	void* const pMover = SMT_ScriptThis(v);
	TraversalState* const s = SMT_ActiveState(pMover);
	if (!s)
		return s_origGetMoveToPositionWorld ? s_origGetMoveToPositionWorld(v) : SQ_ERROR;
	const Vector3D pos = s->m_bHasGoal ? SMT_GoalWorld(*s) : reinterpret_cast<CBaseEntity*>(pMover)->Diag_AbsOrigin();
	const SQVector3D out(pos.x, pos.y, pos.z);
	sq_pushvector(v, &out);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static ScriptNative_t SMT_FindOriginal(const char* pszName)
{
	if (!s_pScriptMoverDesc)
		return nullptr;
	ScriptNative_t pFound = nullptr;
	for (CUtlVector<ScriptFunctionBinding_t>* pVec : { &s_pScriptMoverDesc->m_NumTypedFunctions, &s_pScriptMoverDesc->m_StrTypedFunctions })
	{
		for (int i = 0; i < pVec->Count(); ++i)
		{
			const ScriptFunctionBinding_t& b = (*pVec)[i];
			if (b.m_Descriptor.m_ScriptName && strcmp(b.m_Descriptor.m_ScriptName, pszName) == 0)
				pFound = reinterpret_cast<ScriptNative_t>(b.m_pFunction);
		}
	}
	if (!pFound)
		Warning(eDLL_T::SERVER, "[TRAV] CScriptMover.%s not found -- non-traversal movers lose it\n", pszName);
	return pFound;
}

static void SMT_RegisterFunctions(void)
{
	ScriptClassDescriptor_t* const d = s_pScriptMoverDesc;
	if (!d || !d->m_ScriptName || strcmp(d->m_ScriptName, "CScriptMover") != 0)
	{
		Warning(eDLL_T::SERVER, "[TRAV] CScriptMover descriptor not found -- mover traversal natives missing\n");
		return;
	}

	s_origSetMaxSpeed = SMT_FindOriginal("SetMaxSpeed");
	s_origSetMinimalHeightGround = SMT_FindOriginal("SetMinimalHeightGround");
	s_origClearDesiredYaw = SMT_FindOriginal("ClearDesiredYaw");
	s_origGetMoveToPositionWorld = SMT_FindOriginal("GetMoveToPositionWorld");
	EntityScriptExt_RenameBinding(d, "SetMaxSpeed", "SetMaxSpeed_Engine");
	EntityScriptExt_RenameBinding(d, "SetMinimalHeightGround", "SetMinimalHeightGround_Engine");
	EntityScriptExt_RenameBinding(d, "ClearDesiredYaw", "ClearDesiredYaw_Engine");
	EntityScriptExt_RenameBinding(d, "GetMoveToPositionWorld", "GetMoveToPositionWorld_Engine");

	d->AddFunction("EnableNonPhysicsTraversal", "Script_EnableNonPhysicsTraversal",
		"Drives this non-physics mover over ground toward its goal", "void",
		"float checkDist, float lookaheadDist, float sideVelocity, float maxWidthCorrectionVelocity, float traceOffset, "
		"float forceDecay, int traceMask, int collisionGroup, bool collidePlayers, bool collideNPCs",
		false, Script_EnableNonPhysicsTraversal);
	d->AddFunction("SetMoveToPositionGroundNonPhysics", "Script_SetMoveToPositionGroundNonPhysics",
		"Sets the traversal goal, relative to groundEnt when it is a mover", "void",
		"vector pos, entity groundEnt", false, Script_SetMoveToPositionGroundNonPhysics);
	d->AddFunction("SetInitialSpeed", "Script_SetInitialSpeed",
		"Start speed, easing to the max speed over decayTime", "void", "float speed, float decayTime",
		false, Script_SetInitialSpeed);
	d->AddFunction("EnableDeaccelerationApproachingDest", "Script_EnableDeaccelerationApproachingDest",
		"Inside dist of the goal, speed eases down to minSpeed", "void", "float dist, float minSpeed",
		false, Script_EnableDeaccelerationApproachingDest);
	d->AddFunction("SetYawAccelerationScale", "Script_SetYawAccelerationScale",
		"Yaw turn rate in degrees per second", "void", "float scale", false, Script_SetYawAccelerationScale);
	d->AddFunction("EnableLedgeChecking", "Script_EnableLedgeChecking",
		"Stops the traversal at drops deeper than maxDrop", "void",
		"float ignoreResetDelay, float stepDist, float maxDrop", false, Script_EnableLedgeChecking);
	d->AddFunction("EnableSliding", "Script_EnableSliding",
		"Slides along blocking geometry toward the goal", "void",
		"float maxSlideDistance, int edgeDetectAttempts, float slideTargetGoalDistance, float maxDot, float slideTestDepth",
		false, Script_EnableSliding);
	d->AddFunction("EnableStepOver", "Script_EnableStepOver",
		"Steps over obstacles up to maxHeight", "void", "float maxHeight, float checkRange, float targetGoalDistance",
		false, Script_EnableStepOver);
	d->AddFunction("EnableGravityAboveMinHeight", "Script_EnableGravityAboveMinHeight",
		"Settles to the hover height under scaled gravity", "void", "float scale",
		false, Script_EnableGravityAboveMinHeight);
	d->AddFunction("SetDesiredYawDir", "Script_SetDesiredYawDir",
		"Holds the mover facing dir until ClearDesiredYaw", "void", "vector dir", false, Script_SetDesiredYawDir);

	if (s_origSetMaxSpeed)
		d->AddFunction("SetMaxSpeed", "Script_TraversalSetMaxSpeed", "Sets the max speed", "void",
			"float speed", false, Script_SetMaxSpeed);
	if (s_origSetMinimalHeightGround)
		d->AddFunction("SetMinimalHeightGround", "Script_TraversalSetMinimalHeightGround",
			"Hover height above ground", "void",
			"float height, float groundCheckDist, int traceMask = 0, int collisionGroup = 0",
			false, Script_SetMinimalHeightGround);
	if (s_origClearDesiredYaw)
		d->AddFunction("ClearDesiredYaw", "Script_TraversalClearDesiredYaw", "Clears the desired yaw", "void",
			"", false, Script_ClearDesiredYaw);
	if (s_origGetMoveToPositionWorld)
		d->AddFunction("GetMoveToPositionWorld", "Script_TraversalGetMoveToPositionWorld",
			"Current move goal in world space", "vector", "", false, Script_GetMoveToPositionWorld);

	Msg(eDLL_T::SERVER, "[TRAV] CScriptMover traversal natives registered\n");
}

static void Hook_Script_RegisterScriptMoverClassFuncs(void)
{
	v_Script_RegisterScriptMoverClassFuncs();

	static bool s_bRegistered = false;
	if (s_bRegistered)
		return;
	s_bRegistered = true;
	SMT_RegisterFunctions();
}

void VScriptMoverTraversal::GetAdr(void) const
{
	LogFunAdr("Script_RegisterScriptMoverClassFuncs", v_Script_RegisterScriptMoverClassFuncs);
	LogFunAdr("CScriptMover::NonPhysicsMoveTo", v_CScriptMover__NonPhysicsMoveTo);
	LogFunAdr("CScriptMover::NonPhysicsRotateTo", v_CScriptMover__NonPhysicsRotateTo);
	LogVarAdr("g_serverScriptScriptMoverStruct", s_pScriptMoverDesc);
}

void VScriptMoverTraversal::GetFun(void) const
{
	// CScriptMover registrar: "Script Mover" description, then the class name store.
	const CMemory registrar = Module_FindPattern(g_GameDll,
		"40 55 48 8B EC 48 83 EC 30 80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? "
		"48 89 5C 24 40 48 89 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 74 24 48 "
		"48 89 05 ?? ?? ?? ?? 33 F6 48 8D 05 ?? ?? ?? ?? 48 89 7C 24 50 48 89 05 ?? ?? ?? ?? 8B CE B0 43");
	registrar.GetPtr(v_Script_RegisterScriptMoverClassFuncs);
	// First "mov [rip], rdx" stores the class name into the descriptor's first field.
	if (registrar)
		registrar.FindPattern("48 89 15", CMemory::Direction::DOWN, 0x80)
			.ResolveRelativeAddressSelf(0x3, 0x7).GetPtr(s_pScriptMoverDesc);

	// Both reject physics movers at +0x15CC; the move twin differs from its
	// world-to-local sibling only in the size of its early-out jump.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 57 48 83 EC 60 80 B9 CC 15 00 00 00 48 8B FA 0F 29 74 24 50 48 8B D9 "
		"0F 29 7C 24 40 0F 28 F2 0F 28 FB 74 11 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E9 B1 03 00 00")
		.GetPtr(v_CScriptMover__NonPhysicsMoveTo);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 57 48 81 EC A0 00 00 00 80 B9 CC 15 00 00 00 48 8B FA 44 0F 29 4C 24 60 "
		"48 8B D9 44 0F 29 54 24 50 44 0F 28 CA 44 0F 28 D3")
		.GetPtr(v_CScriptMover__NonPhysicsRotateTo);

	if (!v_Script_RegisterScriptMoverClassFuncs || !s_pScriptMoverDesc)
		Warning(eDLL_T::SERVER, "[TRAV] CScriptMover registrar unresolved -- EnableNonPhysicsTraversal is missing\n");
	if (!v_CScriptMover__NonPhysicsMoveTo || !v_CScriptMover__NonPhysicsRotateTo)
		Warning(eDLL_T::SERVER, "[TRAV] NonPhysicsMoveTo/RotateTo unresolved (move=%d rotate=%d) -- traversal movers do not move\n",
			v_CScriptMover__NonPhysicsMoveTo ? 1 : 0, v_CScriptMover__NonPhysicsRotateTo ? 1 : 0);
}

void VScriptMoverTraversal::Detour(const bool bAttach) const
{
	if (v_Script_RegisterScriptMoverClassFuncs)
		DetourSetup(&v_Script_RegisterScriptMoverClassFuncs, &Hook_Script_RegisterScriptMoverClassFuncs, bAttach);
}
