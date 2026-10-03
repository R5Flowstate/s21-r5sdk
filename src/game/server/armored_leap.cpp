//=============================================================================//
//
// Purpose: Armored leap (Newcastle ultimate) on the dedicated server. See
// armored_leap.h. The S21 client predicts the same leap from the same math,
// so every formula here follows the client body line for line.
//
// The client sets MOVETYPE_FLY at leap start and its PlayerMove turns it back
// to WALK before dispatch, so the leap always runs inside FullWalkMove. This
// engine has no such pre-dispatch, so the leap never leaves MOVETYPE_WALK.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "armored_leap.h"
#include "baseentity.h"
#include "player.h"
#include "trigger_cannon.h"
#include "player_stance.h"
#include "jetdrive.h"
#include "halfduck_zip_parity.h"
#include "game/shared/activity.h"
#include "vscript_server.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/titan_gate.h"
#include "game/shared/collisionproperty.h"
#include "game/shared/util_shared.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/sqarray.h"
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

extern CGlobalVars* gpGlobals;

enum ArmoredLeapPhase_e
{
	AL_PHASE_NONE = 0,
	AL_PHASE_PREP,
	AL_PHASE_TRAVEL_AIR,
	AL_PHASE_TRAVEL_AIR_HOVER,
	AL_PHASE_TRAVEL_GROUND,
	AL_PHASE_ARRIVAL,
	AL_PHASE_INTERRUPTED
};

enum ArmoredLeapType_e
{
	AL_TYPE_NONE = 0,
	AL_TYPE_DASH,
	AL_TYPE_JUMP,
	AL_TYPE_AIR_DIVE,
	AL_TYPE_AIR_DIVE_LONG
};

enum ArmoredLeapAnimState_e
{
	AL_STATE_NONE = 0,
	AL_STATE_AIR_START,
	AL_STATE_AIR_UP,
	AL_STATE_AIR_HOVER,
	AL_STATE_AIR_DIVE,
	AL_STATE_AIR_DIVE_LONG,
	AL_STATE_AIR_END,
	AL_STATE_GROUND_START,
	AL_STATE_GROUND_DASH,
	AL_STATE_GROUND_END,
	AL_STATE_COUNT
};

// Blocked-check bits shared by GetBlockedCheckTypes and IsTravelBlocked.
static constexpr unsigned int AL_BLOCK_PATH      = 0x2;
static constexpr unsigned int AL_BLOCK_HIGH      = 0x4;
static constexpr unsigned int AL_BLOCK_AHEAD     = 0x8;
static constexpr unsigned int AL_BLOCK_DEST      = 0x10;

static constexpr float AL_TURN_IGNORE_DIST       = 80.0f;     // no turning inside this 2D distance to endPos
static constexpr float AL_TURN_RATE              = 8.0f;
static constexpr float AL_HIGH_BLOCK_Z_BOOST     = 100000.0f; // per second while only the path is blocked
static constexpr float AL_AHEAD_FORWARD_BOOST    = 125000.0f; // per second while the path is blocked but not ahead
static constexpr float AL_END_DIST               = 15.0f;
static constexpr float AL_STALL_SPEED_FRAC       = 0.25f;
static constexpr float AL_BLOCKED_DOT_MIN        = 0.45f;
static constexpr float AL_BLOCKED_DEST_DIST      = 150.0f;
static constexpr float AL_LOOK_AHEAD_DIST        = 150.0f;
static constexpr float AL_HIGH_BLOCK_STEP        = 50.0f;
static constexpr int   AL_HIGH_BLOCK_STEPS_DASH  = 1;
static constexpr int   AL_HIGH_BLOCK_STEPS_JUMP  = 3;
static constexpr float AL_AHEAD_OFFSET           = 30.0f;
static constexpr float AL_GROUND_PATH_LIFT       = 5.0f;
static constexpr float AL_SQRT_FLT_MIN           = 1.08420217e-19f;

// CONTENTS mask and PLAYER_MOVEMENT collision group of the client's probes;
// the group is 9 on the client and 8 in this engine's enum.
static constexpr unsigned int AL_TRACE_MASK      = 0x0201400Bu;
static constexpr int AL_COLLISION_GROUP_PLAYER_MOVEMENT = 8;

static constexpr int kMaxIgnoreEnts = 32;

// Server CPlayer layout.
static constexpr ptrdiff_t AL_OFF_EFLAGS          = 0x230;  // m_iEFlags
static constexpr ptrdiff_t AL_OFF_FLAGS           = 0x234;  // m_fFlags
static constexpr ptrdiff_t AL_OFF_MOVETYPE        = 0x308;
static constexpr ptrdiff_t AL_OFF_LIFESTATE       = 0x499;
static constexpr ptrdiff_t AL_OFF_PHASESHIFT_START = 0x15B4;
static constexpr ptrdiff_t AL_OFF_PHASESHIFT_END  = 0x15B8;
static constexpr ptrdiff_t AL_OFF_FORCESTANCE     = 0x5AAC;
static constexpr ptrdiff_t AL_OFF_JUMPPENDING     = 0x5AB8; // cleared by the jump check before any other rule
static constexpr ptrdiff_t AL_OFF_HULLSTATE       = 0x6280; // 0 and 5 use the pose hull table below
static constexpr ptrdiff_t AL_OFF_HULL_STAND_MINS = 0x65FC;
static constexpr ptrdiff_t AL_OFF_HULL_STAND_MAXS = 0x6608;
static constexpr ptrdiff_t AL_OFF_HULL_DUCK_MINS  = 0x6614;
static constexpr ptrdiff_t AL_OFF_HULL_DUCK_MAXS  = 0x6620;
static constexpr ptrdiff_t AL_OFF_SLIDING         = 0x67C5;
static constexpr ptrdiff_t AL_OFF_FORCEAIRBORNE   = 0x6944; // categorize treats the player as airborne while set

static constexpr int AL_EFL_KILLME       = 0x1;
static constexpr int AL_FL_DUCKING       = 0x2;
static constexpr int AL_FORCE_STANCE_STAND = 1;
static constexpr char AL_MOVETYPE_WALK   = 2;
static constexpr char AL_MOVETYPE_FLY    = 4;

// CGameMovement / CMoveData.
static constexpr ptrdiff_t AL_CTX_OFF_PLAYER   = 8;
static constexpr ptrdiff_t AL_CTX_OFF_MV       = 16;
static constexpr ptrdiff_t AL_MV_OFF_VELOCITY  = 304;

static constexpr int VTBL_ISPLAYER_SLOT = 93; // +744

static ConVar armored_leap_disable_player_move_input("armored_leap_disable_player_move_input", "1",
	FCVAR_RELEASE | FCVAR_REPLICATED,
	"Ignore forward/side/up move input while an armored leap is active.");

static ConVar bridge_armored_leap_trace("bridge_armored_leap_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[AL-PHASE] per leap phase change; 2 also prints [AL-BLOCK] whenever the blocked bits change.");

//-----------------------------------------------------------------------------
// Per-player leap state. Mirrors the client's C_Player fields; the five
// networked ones are published through the PlayerExtendWire sidecar.
//-----------------------------------------------------------------------------
struct ArmoredLeapState
{
	int      m_nType = AL_TYPE_NONE;
	int      m_nPhase = AL_PHASE_NONE;
	int      m_nState = AL_STATE_NONE;
	int      m_nStateDeprecated = 0;
	float    m_flStartTime = 0.0f;
	float    m_flPhaseStartTime = 0.0f;
	Vector3D m_vecAirPos = vec3_origin;
	Vector3D m_vecEndPos = vec3_origin;
	Vector3D m_vecStartPos = vec3_origin;
	float    m_flPrepSpeed = 0.0f;
	float    m_flPrepAccel = 0.0f;
	float    m_flJumpSpeed = 0.0f;
	float    m_flJumpAccel = 0.0f;
	float    m_flTransitionSpeed = 0.0f;
	float    m_flTransitionAccel = 0.0f;
	float    m_flTransitionDist = 0.0f;
	float    m_flHoverDist = 0.0f;
	float    m_flDiveSpeed = 0.0f;
	float    m_flDiveAccel = 0.0f;
	float    m_flPrepDuration = 0.0f;
	float    m_flArrivalDuration = 0.0f;
	float    m_flTimeOut = 0.0f;
	uint32_t m_ignoreEnts[kMaxIgnoreEnts] = {};
	int      m_nIgnoreEnts = 0;
	unsigned int m_nLastBlocked = 0;
};

static SDKEntityMap<ArmoredLeapState> s_leapMap(ESide::Server, "armoredLeap.srv");
// Players with a live phase; keeps the per-trace collision hook to one size test.
static SDKEntityMap<uint8_t> s_leapActive(ESide::Server, "armoredLeap.active");

static void (*v_CGameMovement__PlayerMove)(void* ctx) = nullptr;
static void (*v_CGameMovement__CategorizePosition)(void* ctx, bool bStayOnGround) = nullptr;
static bool (*v_CGameMovement__CheckJumpButton)(void* ctx) = nullptr;
static bool (*v_GameMovement_ShouldHitEntity)(void* pMover, void* pHit) = nullptr;
static void (*v_CGameMovement__CheckVelocity)(void* ctx) = nullptr;
static void (*v_CGameMovement__SetGroundEntity)(void* ctx, void* pTrace) = nullptr;
static void (*v_CBaseEntity__SetAbsAngles)(void* pEntity, const QAngle* pAngles) = nullptr;

static HSCRIPT s_hPhaseCallback = nullptr;
static HSQUIRRELVM s_hPhaseCallbackVM = nullptr;

struct ArmoredLeapMoveFrame
{
	void* ctx = nullptr;
	bool  bArmed = false;
	bool  bGravityApplied = false;
	float flFrameTime = 0.0f;
};

static thread_local ArmoredLeapMoveFrame s_moveFrame;
static thread_local int s_nSavedForceStance = -1;

static inline float AL_CurTime(void)
{
	return gpGlobals ? gpGlobals->curTime : 0.0f;
}

static inline uint8_t* AL_Bytes(const void* p)
{
	return static_cast<uint8_t*>(const_cast<void*>(p));
}

template <typename T>
static inline T& AL_Field(const void* p, const ptrdiff_t off)
{
	return *reinterpret_cast<T*>(AL_Bytes(p) + off);
}

static inline Vector3D AL_AbsOrigin(void* pPlayer)
{
	TriggerPass_EnsureAbsOrigin(pPlayer);
	return reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_AbsOrigin();
}

static inline bool AL_IsFinite(const float f)
{
	return std::isfinite(f);
}

static inline bool AL_IsFiniteVec(const Vector3D& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static inline Vector3D AL_NormalizeOrZero(const Vector3D& v, float* pLen = nullptr)
{
	const float flLen = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (pLen)
		*pLen = flLen;
	const float flInv = 1.0f / fmaxf(flLen, AL_SQRT_FLT_MIN);
	return Vector3D(v.x * flInv, v.y * flInv, v.z * flInv);
}

static inline float AL_Dist(const Vector3D& a, const Vector3D& b)
{
	const Vector3D d = a - b;
	return sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
}

// Column 0 of the entity-to-world transform.
static Vector3D AL_Forward(void* pPlayer)
{
	if (v_CBaseEntity_EntityToWorldTransform)
	{
		if (const matrix3x4_t* const mat = v_CBaseEntity_EntityToWorldTransform(
			reinterpret_cast<CBaseEntity*>(pPlayer)))
			return Vector3D(mat->m_flMatVal[0][0], mat->m_flMatVal[1][0], mat->m_flMatVal[2][0]);
	}
	return Vector3D(1.0f, 0.0f, 0.0f);
}

static void AL_HullBounds(void* pPlayer, Vector3D& mins, Vector3D& maxs)
{
	const int nHullState = AL_Field<int>(pPlayer, AL_OFF_HULLSTATE);
	if (nHullState != 0 && nHullState != 5)
	{
		const CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
		mins = pEnt->Diag_CollMins();
		maxs = pEnt->Diag_CollMaxs();
		return;
	}

	const bool bDucked = (AL_Field<int>(pPlayer, AL_OFF_FLAGS) & AL_FL_DUCKING) != 0;
	mins = AL_Field<Vector3D>(pPlayer, bDucked ? AL_OFF_HULL_DUCK_MINS : AL_OFF_HULL_STAND_MINS);
	maxs = AL_Field<Vector3D>(pPlayer, bDucked ? AL_OFF_HULL_DUCK_MAXS : AL_OFF_HULL_STAND_MAXS);
}

static Vector3D AL_WorldSpaceCenter(void* pPlayer)
{
	const CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
	const Vector3D origin = AL_AbsOrigin(pPlayer);
	return origin + (pEnt->Diag_CollMins() + pEnt->Diag_CollMaxs()) * 0.5f;
}

static bool AL_IsPhaseShifted(void* pPlayer)
{
	if (AL_Field<uint8_t>(pPlayer, AL_OFF_LIFESTATE) != 0 || !gpGlobals)
		return false;
	const float flNow = gpGlobals->exactCurTime;
	return flNow >= AL_Field<float>(pPlayer, AL_OFF_PHASESHIFT_START)
		&& AL_Field<float>(pPlayer, AL_OFF_PHASESHIFT_END) >= flNow;
}

//-----------------------------------------------------------------------------
// Wire + callback.
//-----------------------------------------------------------------------------
static void AL_Mirror(void* pPlayer, const ArmoredLeapState& s)
{
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_armoredLeapPhase), s.m_nPhase);
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_armoredLeapType), s.m_nType);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_armoredLeapStartTime), s.m_flStartTime);
	const float air[3] = { s.m_vecAirPos.x, s.m_vecAirPos.y, s.m_vecAirPos.z };
	const float end[3] = { s.m_vecEndPos.x, s.m_vecEndPos.y, s.m_vecEndPos.z };
	PlayerExtend_SetVec(pPlayer, offsetof(PlayerExtendWire, m_armoredLeapAirPos), air);
	PlayerExtend_SetVec(pPlayer, offsetof(PlayerExtendWire, m_armoredLeapEndPos), end);
	MarkEntityEdictDirty(pPlayer);

	if (s.m_nPhase != AL_PHASE_NONE)
		s_leapActive[pPlayer] = 1;
	else
		s_leapActive.Erase(pPlayer);
}

// FindFunction allocates a handle per call; cache it per live VM.
static HSCRIPT AL_PhaseCallback(void)
{
	if (!g_pServerScript)
		return nullptr;

	const HSQUIRRELVM hVM = g_pServerScript->GetVM();
	if (!hVM)
		return nullptr;

	if (s_hPhaseCallbackVM != hVM)
	{
		s_hPhaseCallbackVM = hVM;
		s_hPhaseCallback = g_pServerScript->FindFunction("CodeCallback_ArmoredLeapPhaseChange", nullptr, nullptr);
		if (!s_hPhaseCallback)
			Warning(eDLL_T::SERVER, "[AL-PHASE] CodeCallback_ArmoredLeapPhaseChange not found in server VM\n");
	}
	return s_hPhaseCallback;
}

static void AL_FirePhaseCallback(void* pPlayer, const int nNewPhase, const int nOldPhase)
{
	const HSCRIPT hFunc = AL_PhaseCallback();
	if (!hFunc)
		return;

	const HSCRIPT hPlayer = reinterpret_cast<CBaseEntity*>(pPlayer)->GetScriptInstance();
	if (!hPlayer)
		return;

	ScriptVariant_t args[3];
	args[0] = hPlayer;
	args[1] = nNewPhase;
	args[2] = nOldPhase;
	if (g_pServerScript->ExecuteFunction(hFunc, args, 3, nullptr, nullptr) == SCRIPT_ERROR)
		Warning(eDLL_T::SERVER, "[AL-PHASE] CodeCallback_ArmoredLeapPhaseChange SCRIPT_ERROR player=%p %d -> %d\n",
			pPlayer, nOldPhase, nNewPhase);
}

static void AL_SetPhase(void* pPlayer, ArmoredLeapState& s, const int nPhase)
{
	if (nPhase == s.m_nPhase)
		return;

	const int nOld = s.m_nPhase;
	s.m_flPhaseStartTime = AL_CurTime();
	s.m_nPhase = nPhase;
	AL_Mirror(pPlayer, s);

	if (bridge_armored_leap_trace.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[AL-PHASE] player=%p type=%d %d -> %d t=%.3f\n",
			pPlayer, s.m_nType, nOld, nPhase, s.m_flPhaseStartTime);

	AL_FirePhaseCallback(pPlayer, nPhase, nOld);
}

//-----------------------------------------------------------------------------
// Getters.
//-----------------------------------------------------------------------------
static float AL_GetSpeed(const ArmoredLeapState& s)
{
	switch (s.m_nPhase)
	{
	case AL_PHASE_PREP:              return s.m_flPrepSpeed;
	case AL_PHASE_TRAVEL_AIR:        return s.m_flJumpSpeed;
	case AL_PHASE_TRAVEL_AIR_HOVER:  return s.m_flTransitionSpeed;
	case AL_PHASE_TRAVEL_GROUND:     return s.m_flDiveSpeed;
	default:                         return 0.0f;
	}
}

static float AL_GetAccel(const ArmoredLeapState& s)
{
	switch (s.m_nPhase)
	{
	case AL_PHASE_PREP:              return s.m_flPrepAccel;
	case AL_PHASE_TRAVEL_AIR:        return s.m_flJumpAccel;
	case AL_PHASE_TRAVEL_AIR_HOVER:  return s.m_flTransitionAccel;
	case AL_PHASE_TRAVEL_GROUND:     return s.m_flDiveAccel;
	default:                         return 0.0f;
	}
}

static Vector3D AL_GetTargetPosition(void* pPlayer, const ArmoredLeapState& s)
{
	switch (s.m_nPhase)
	{
	case AL_PHASE_PREP:
		if (s.m_nType == AL_TYPE_AIR_DIVE || s.m_nType == AL_TYPE_AIR_DIVE_LONG)
			return s.m_vecEndPos;
		return AL_AbsOrigin(pPlayer);
	case AL_PHASE_TRAVEL_AIR:
	case AL_PHASE_TRAVEL_AIR_HOVER:
		return s.m_vecAirPos;
	case AL_PHASE_TRAVEL_GROUND:
	case AL_PHASE_ARRIVAL:
		return s.m_vecEndPos;
	default:
		return vec3_invalid;
	}
}

static bool AL_InArrivalWindow(const ArmoredLeapState& s)
{
	return (s.m_nPhase == AL_PHASE_ARRIVAL || s.m_nPhase == AL_PHASE_INTERRUPTED)
		&& s.m_flArrivalDuration >= AL_CurTime() - s.m_flPhaseStartTime;
}

static bool AL_AtGroundDestination(void* pPlayer, const ArmoredLeapState& s)
{
	return s.m_nPhase == AL_PHASE_TRAVEL_GROUND
		&& AL_Dist(s.m_vecEndPos, AL_AbsOrigin(pPlayer)) <= AL_END_DIST;
}

//-----------------------------------------------------------------------------
// End / unduck.
//-----------------------------------------------------------------------------
static void AL_End(void* pPlayer, ArmoredLeapState& s)
{
	s.m_flPrepSpeed = s.m_flPrepAccel = 0.0f;
	s.m_flJumpSpeed = s.m_flJumpAccel = 0.0f;
	s.m_flTransitionSpeed = s.m_flTransitionAccel = 0.0f;
	s.m_flDiveSpeed = s.m_flDiveAccel = 0.0f;
	s.m_flPrepDuration = s.m_flArrivalDuration = 0.0f;
	s.m_flTransitionDist = s.m_flHoverDist = 0.0f;
	s.m_vecAirPos = vec3_invalid;
	s.m_vecEndPos = vec3_invalid;
	s.m_vecStartPos = vec3_invalid;
	s.m_flTimeOut = 0.0f;
	s.m_flPhaseStartTime = 0.0f;
	s.m_nType = AL_TYPE_NONE;

	if (s.m_nPhase != AL_PHASE_NONE)
		AL_SetPhase(pPlayer, s, AL_PHASE_NONE);
	else
		AL_Mirror(pPlayer, s);

	s.m_nState = AL_STATE_NONE;
	s.m_nIgnoreEnts = 0;
	s.m_nLastBlocked = 0;

	char& moveType = AL_Field<char>(pPlayer, AL_OFF_MOVETYPE);
	if (moveType == AL_MOVETYPE_FLY)
	{
		moveType = AL_MOVETYPE_WALK;
		MarkEntityEdictDirty(pPlayer);
	}
}

// Instant stand, off the ground and out of any slide, on the same command.
static void AL_Unduck(void* pPlayer)
{
	PlayerStance_SetInstant(pPlayer, false);
	TriggerPass_SetGroundEntityNull(pPlayer);
	AL_Field<uint8_t>(pPlayer, AL_OFF_SLIDING) = 0;
	MarkEntityEdictDirty(pPlayer);
}

//-----------------------------------------------------------------------------
// Travel-blocked probes.
//-----------------------------------------------------------------------------
class CArmoredLeapTraceFilter : public CTraceFilterSimple
{
public:
	CArmoredLeapTraceFilter(const IHandleEntity* pPass, const ArmoredLeapState& s)
		: CTraceFilterSimple(pPass, AL_COLLISION_GROUP_PLAYER_MOVEMENT), m_state(s)
	{
	}

	virtual bool ShouldHitEntity(IHandleEntity* const pEntity, const int contentsMask) override
	{
		if (pEntity)
		{
			const uint32_t nRaw = SDKEntityState_GetHandle(pEntity).Raw();
			for (int i = 0; i < m_state.m_nIgnoreEnts; ++i)
			{
				if (m_state.m_ignoreEnts[i] == nRaw)
					return false;
			}
		}
		return CTraceFilterSimple::ShouldHitEntity(pEntity, contentsMask);
	}

private:
	const ArmoredLeapState& m_state;
};

static void AL_TraceHull(const Vector3D& start, const Vector3D& end, const Vector3D& mins, const Vector3D& maxs,
	const char nSolidType, ITraceFilter* pFilter, trace_t& tr)
{
	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1.0f;
	tr.endpos = end;
	if (!g_pEngineTraceServer || !v_Ray_t_InitStartEndMinsMaxsUp)
		return;

	alignas(16) Ray_t ray;
	memset(&ray, 0, sizeof(ray));
	const Vector3D up(0.0f, 0.0f, 1.0f);
	v_Ray_t_InitStartEndMinsMaxsUp(&ray, &start, &end, &mins, &maxs, &up);
	ray.m_nSolidType = nSolidType;
	ray.m_nDetailLevel = 0;
	g_pEngineTraceServer->TraceRayFiltered(ray, AL_TRACE_MASK, pFilter, &tr);
}

static inline bool AL_TraceClear(const trace_t& tr)
{
	return (tr.fraction >= 1.0f && !tr.allsolid && !tr.startsolid) || tr.fraction >= 0.99f;
}

static unsigned int AL_GetBlockedCheckTypes(void* pPlayer, const ArmoredLeapState& s)
{
	float flSpeed = AL_GetSpeed(s);
	if (flSpeed <= 0.0f)
		return 0;

	const Vector3D origin = AL_AbsOrigin(pPlayer);
	float flDist = 0.0f;
	const Vector3D dir = AL_NormalizeOrZero(AL_GetTargetPosition(pPlayer, s) - origin, &flDist);

	unsigned int nBits = 0;
	if (flDist > AL_END_DIST)
	{
		const float flElapsed = fmaxf(AL_CurTime() - s.m_flPhaseStartTime, 0.0f);
		const float flAccel = AL_GetAccel(s);
		if (flAccel == 0.0f)
			flSpeed = 0.0f;
		else if (flElapsed < flSpeed / flAccel)
			flSpeed = flAccel * flElapsed;

		const Vector3D& vel = reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_AbsVelocity();
		const float flVel = sqrtf(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
		if (flSpeed > 0.0f && flVel / flSpeed < AL_STALL_SPEED_FRAC)
		{
			nBits = AL_BLOCK_PATH;
			if (DotProduct(dir, AL_Forward(pPlayer)) >= AL_BLOCKED_DOT_MIN)
				nBits = AL_BLOCK_PATH | AL_BLOCK_HIGH;
			if (-dir.z >= AL_BLOCKED_DOT_MIN)
				nBits |= AL_BLOCK_AHEAD;
		}
	}
	if (flDist <= AL_BLOCKED_DEST_DIST)
		nBits |= AL_BLOCK_DEST;
	return nBits;
}

static unsigned int AL_IsTravelBlocked(void* pPlayer, ArmoredLeapState& s, const unsigned int nChecks)
{
	if (!nChecks || !pPlayer)
		return 0;

	const int nPhase = s.m_nPhase;
	const int nType = s.m_nType;
	CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
	const char nSolidType = pEnt->CollisionProp()->Diag_SolidType();

	const Vector3D center = AL_WorldSpaceCenter(pPlayer);
	Vector3D target = AL_GetTargetPosition(pPlayer, s);

	Vector3D hullMins, hullMaxs;
	AL_HullBounds(pPlayer, hullMins, hullMaxs);
	const float flHullHeight = hullMaxs.z - hullMins.z;
	const Vector3D smallMins(hullMins.x * 0.25f, hullMins.y * 0.25f, -(flHullHeight * 0.125f));
	const Vector3D smallMaxs(hullMaxs.x * 0.25f, hullMaxs.y * 0.25f, flHullHeight * 0.125f);

	const Vector3D origin = AL_AbsOrigin(pPlayer);
	target.z += center.z - origin.z;
	const float flLookDist = fminf(AL_LOOK_AHEAD_DIST, AL_Dist(target, center));

	CArmoredLeapTraceFilter filter(static_cast<const IHandleEntity*>(pPlayer), s);
	trace_t tr;
	unsigned int nBlocked = 0;

	if (nChecks & AL_BLOCK_PATH)
	{
		Vector3D start = origin;
		Vector3D end = AL_GetTargetPosition(pPlayer, s);
		if (nPhase == AL_PHASE_TRAVEL_GROUND)
		{
			start.z += AL_GROUND_PATH_LIFT;
			end.z += AL_GROUND_PATH_LIFT;
		}
		end = start + AL_NormalizeOrZero(end - start) * flLookDist;
		AL_TraceHull(start, end, hullMins, hullMaxs, nSolidType, &filter, tr);
		if (!AL_TraceClear(tr))
			nBlocked = AL_BLOCK_PATH;
	}

	if (nChecks & AL_BLOCK_HIGH)
	{
		const int nSteps = (nType == AL_TYPE_DASH) ? AL_HIGH_BLOCK_STEPS_DASH : AL_HIGH_BLOCK_STEPS_JUMP;
		int nStep = 1;
		for (; nStep <= nSteps; ++nStep)
		{
			const float flOffset = static_cast<float>(nStep) * AL_HIGH_BLOCK_STEP;
			const Vector3D start(center.x, center.y, center.z + flOffset);
			const Vector3D aim(target.x, target.y, target.z + flOffset);
			float flLen = 0.0f;
			const Vector3D dir = AL_NormalizeOrZero(aim - start, &flLen);
			const float flStep = fminf(AL_LOOK_AHEAD_DIST, flLen);
			// The client keeps this probe level: the vertical component is dropped.
			const Vector3D end(start.x + dir.x * flStep, start.y + dir.y * flStep, start.z + flStep * 0.0f);
			AL_TraceHull(start, end, smallMins, smallMaxs, nSolidType, &filter, tr);
			if (AL_TraceClear(tr))
				break;
		}
		if (nStep > nSteps)
			nBlocked |= AL_BLOCK_HIGH;
	}

	if (nChecks & AL_BLOCK_AHEAD)
	{
		const Vector3D fwd = AL_Forward(pPlayer);
		const Vector3D ahead = center + fwd * AL_AHEAD_OFFSET;
		AL_TraceHull(center, ahead, smallMins, smallMaxs, nSolidType, &filter, tr);
		if (AL_TraceClear(tr))
		{
			const Vector3D aimAhead = target + fwd * AL_AHEAD_OFFSET;
			float flLen = 0.0f;
			const Vector3D dir = AL_NormalizeOrZero(aimAhead - ahead, &flLen);
			const Vector3D end = ahead + dir * fminf(AL_LOOK_AHEAD_DIST, flLen);
			AL_TraceHull(ahead, end, smallMins, smallMaxs, nSolidType, &filter, tr);
			if (!AL_TraceClear(tr))
			{
				const float dx = tr.endpos.x - target.x;
				const float dy = tr.endpos.y - target.y;
				const float flDist2D = sqrtf(dx * dx + dy * dy);
				if (tr.fraction < 0.9f || tr.endpos.z - target.z > 10.0f || flDist2D > AL_AHEAD_OFFSET)
					nBlocked |= AL_BLOCK_AHEAD;
			}
		}
	}

	if (nChecks & AL_BLOCK_DEST)
	{
		const Vector3D center2 = AL_WorldSpaceCenter(pPlayer);
		const Vector3D dir = AL_NormalizeOrZero(center2 - target);
		const Vector3D end = target + dir * fminf(AL_BLOCKED_DEST_DIST, AL_Dist(target, center2));
		AL_TraceHull(target, end, smallMins, smallMaxs, nSolidType, &filter, tr);
		const bool bClear = tr.fraction >= 1.0f && !tr.allsolid && !tr.startsolid && tr.fraction > 0.99f;
		if (!bClear)
		{
			nBlocked |= AL_BLOCK_DEST;
			AL_SetPhase(pPlayer, s, AL_PHASE_INTERRUPTED);
		}
	}

	if (bridge_armored_leap_trace.GetInt() >= 2 && nBlocked != s.m_nLastBlocked)
		Msg(eDLL_T::SERVER, "[AL-BLOCK] player=%p phase=%d checks=0x%X blocked=0x%X\n",
			pPlayer, s.m_nPhase, nChecks, nBlocked);
	s.m_nLastBlocked = nBlocked;
	return nBlocked;
}

//-----------------------------------------------------------------------------
// Phase and anim-state machines.
//-----------------------------------------------------------------------------
static void AL_UpdatePhases(void* pPlayer, ArmoredLeapState& s)
{
	if (AL_Field<uint8_t>(pPlayer, AL_OFF_LIFESTATE) != 0)
	{
		AL_End(pPlayer, s);
		return;
	}

	const int nType = s.m_nType;
	const unsigned int nChecks = AL_GetBlockedCheckTypes(pPlayer, s);
	const int nPhase = s.m_nPhase;
	if (nPhase == AL_PHASE_NONE)
	{
		AL_End(pPlayer, s);
		return;
	}

	const float flNow = AL_CurTime();
	int nNew = AL_PHASE_NONE;

	const bool bInterrupt = (AL_Field<int>(pPlayer, AL_OFF_EFLAGS) & AL_EFL_KILLME) != 0 || AL_IsPhaseShifted(pPlayer);
	if (bInterrupt && nPhase != AL_PHASE_INTERRUPTED)
	{
		nNew = AL_PHASE_INTERRUPTED;
	}
	else if (flNow - s.m_flStartTime > s.m_flTimeOut)
	{
		if (nPhase != AL_PHASE_INTERRUPTED)
			nNew = AL_PHASE_INTERRUPTED;
		else
			nNew = AL_InArrivalWindow(s) ? AL_PHASE_INTERRUPTED : AL_PHASE_NONE;
	}
	else
	{
		switch (nPhase)
		{
		case AL_PHASE_PREP:
			if (s.m_flPrepDuration < flNow - s.m_flPhaseStartTime)
			{
				if (nType == AL_TYPE_JUMP)
					nNew = AL_PHASE_TRAVEL_AIR;
				else if (nType == AL_TYPE_DASH || nType == AL_TYPE_AIR_DIVE || nType == AL_TYPE_AIR_DIVE_LONG)
					nNew = AL_PHASE_TRAVEL_GROUND;
			}
			else
			{
				nNew = AL_PHASE_PREP;
			}
			break;
		case AL_PHASE_ARRIVAL:
			nNew = AL_InArrivalWindow(s) ? AL_PHASE_ARRIVAL : AL_PHASE_NONE;
			break;
		case AL_PHASE_INTERRUPTED:
			nNew = AL_InArrivalWindow(s) ? AL_PHASE_INTERRUPTED : AL_PHASE_NONE;
			break;
		default:
			if (nChecks && (nChecks & AL_IsTravelBlocked(pPlayer, s, nChecks)) == nChecks)
			{
				if (s.m_nPhase == AL_PHASE_INTERRUPTED)
					return;
				nNew = AL_AtGroundDestination(pPlayer, s) ? AL_PHASE_ARRIVAL : AL_PHASE_INTERRUPTED;
			}
			else
			{
				// Re-read: the destination probe above may already have interrupted the leap.
				switch (s.m_nPhase)
				{
				case AL_PHASE_TRAVEL_AIR:
					if (nType == AL_TYPE_JUMP)
						nNew = (s.m_flHoverDist >= AL_Dist(s.m_vecAirPos, AL_AbsOrigin(pPlayer)))
							? AL_PHASE_TRAVEL_AIR_HOVER : AL_PHASE_TRAVEL_AIR;
					break;
				case AL_PHASE_TRAVEL_AIR_HOVER:
					if (nType == AL_TYPE_JUMP)
						nNew = (s.m_flTransitionDist >= AL_Dist(s.m_vecAirPos, AL_AbsOrigin(pPlayer)))
							? AL_PHASE_TRAVEL_GROUND : AL_PHASE_TRAVEL_AIR_HOVER;
					break;
				case AL_PHASE_TRAVEL_GROUND:
					nNew = AL_AtGroundDestination(pPlayer, s) ? AL_PHASE_ARRIVAL : AL_PHASE_TRAVEL_GROUND;
					break;
				default:
					break;
				}
			}
			break;
		}
	}

	if (s.m_nPhase != nNew)
	{
		AL_SetPhase(pPlayer, s, nNew);
		if (nNew == AL_PHASE_NONE)
			AL_End(pPlayer, s);
	}
}

static void AL_UpdateState(ArmoredLeapState& s)
{
	int nState = AL_STATE_NONE;
	const int nType = s.m_nType;
	switch (s.m_nPhase)
	{
	case AL_PHASE_PREP:
		if (nType == AL_TYPE_DASH)
			nState = AL_STATE_GROUND_START;
		else if (nType == AL_TYPE_JUMP)
			nState = AL_STATE_AIR_START;
		else if (nType == AL_TYPE_AIR_DIVE || nType == AL_TYPE_AIR_DIVE_LONG)
			nState = AL_STATE_AIR_HOVER;
		break;
	case AL_PHASE_TRAVEL_AIR:
		nState = (nType == AL_TYPE_JUMP) ? AL_STATE_AIR_UP : AL_STATE_NONE;
		break;
	case AL_PHASE_TRAVEL_AIR_HOVER:
		nState = (nType == AL_TYPE_JUMP) ? AL_STATE_AIR_HOVER : AL_STATE_NONE;
		break;
	case AL_PHASE_TRAVEL_GROUND:
		if (nType == AL_TYPE_DASH)
			nState = AL_STATE_GROUND_DASH;
		else if (nType == AL_TYPE_JUMP || nType == AL_TYPE_AIR_DIVE)
			nState = AL_STATE_AIR_DIVE;
		else if (nType == AL_TYPE_AIR_DIVE_LONG)
			nState = AL_STATE_AIR_DIVE_LONG;
		break;
	case AL_PHASE_ARRIVAL:
	case AL_PHASE_INTERRUPTED:
		if (nType == AL_TYPE_DASH)
			nState = AL_STATE_GROUND_END;
		else if (nType >= AL_TYPE_JUMP && nType <= AL_TYPE_AIR_DIVE_LONG)
			nState = AL_STATE_AIR_END;
		break;
	default:
		break;
	}
	s.m_nState = nState;
}

//-----------------------------------------------------------------------------
// Per-tick movement, run after the first half-gravity + CheckVelocity.
//-----------------------------------------------------------------------------
static void AL_Accel(void* ctx, const float dt)
{
	void* const pPlayer = *reinterpret_cast<void**>(AL_Bytes(ctx) + AL_CTX_OFF_PLAYER);
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(AL_Bytes(ctx) + AL_CTX_OFF_MV);
	if (!pPlayer || !mv)
		return;

	ArmoredLeapState* const pState = s_leapMap.Find(pPlayer);
	if (!pState || pState->m_nPhase == AL_PHASE_NONE)
		return;
	ArmoredLeapState& s = *pState;

	const int nPhaseBefore = s.m_nPhase;
	AL_UpdatePhases(pPlayer, s);
	AL_UpdateState(s);
	const int nPhase = s.m_nPhase;
	if (nPhase == AL_PHASE_NONE)
		return;

	Vector3D& mvVel = *reinterpret_cast<Vector3D*>(mv + AL_MV_OFF_VELOCITY);
	const Vector3D vel = mvVel;
	const Vector3D target = AL_GetTargetPosition(pPlayer, s);
	const Vector3D origin = AL_AbsOrigin(pPlayer);
	const Vector3D dir = AL_NormalizeOrZero(target - origin);

	if (nPhase == AL_PHASE_ARRIVAL || nPhase == AL_PHASE_INTERRUPTED)
	{
		if (nPhaseBefore != nPhase)
			mvVel = vec3_origin;
		return;
	}

	const float ex = s.m_vecEndPos.x - origin.x;
	const float ey = s.m_vecEndPos.y - origin.y;
	const float flDist2DSqr = ex * ex + ey * ey;
	if (sqrtf(flDist2DSqr) >= AL_TURN_IGNORE_DIST && v_CBaseEntity__SetAbsAngles)
	{
		const float ez = s.m_vecEndPos.z - origin.z;
		const float flInv = 1.0f / fmaxf(sqrtf(ez * ez + flDist2DSqr), AL_SQRT_FLT_MIN);
		QAngle desired;
		VectorAngles(Vector3D(ex * flInv, ey * flInv, 0.0f), desired);

		const Vector3D& cur = reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_AbsRotation();
		const float flTurn = dt * AL_TURN_RATE;
		const QAngle turned(cur.x + AngleDiff(desired.x, cur.x) * flTurn,
			cur.y + AngleDiff(desired.y, cur.y) * flTurn, desired.z);
		v_CBaseEntity__SetAbsAngles(pPlayer, &turned);
	}

	const unsigned int nChecks = AL_GetBlockedCheckTypes(pPlayer, s);
	const unsigned int nBlocked = AL_IsTravelBlocked(pPlayer, s, nChecks);
	if (nChecks & nBlocked & AL_BLOCK_DEST)
		return;

	const float flSpeed = AL_GetSpeed(s);
	const float flAccel = AL_GetAccel(s) * dt;
	const float flProj = DotProduct(vel, dir);
	const Vector3D proj = dir * flProj;
	const float flCur = sqrtf(proj.x * proj.x + proj.y * proj.y + proj.z * proj.z);
	const float flNew = (flSpeed <= flCur) ? fmaxf(flCur - flAccel, flSpeed) : fminf(flAccel + flCur, flSpeed);

	Vector3D newVel = dir * flNew;
	if ((nChecks & AL_BLOCK_HIGH) && (nBlocked & (AL_BLOCK_PATH | AL_BLOCK_HIGH)) == AL_BLOCK_PATH)
		newVel.z += dt * AL_HIGH_BLOCK_Z_BOOST;
	if ((nChecks & AL_BLOCK_AHEAD) && (nBlocked & AL_BLOCK_PATH) && !(nBlocked & AL_BLOCK_AHEAD))
		newVel += AL_Forward(pPlayer) * (dt * AL_AHEAD_FORWARD_BOOST);

	if (v_CGameMovement__SetGroundEntity)
		v_CGameMovement__SetGroundEntity(ctx, nullptr);
	mvVel = newVel;
}

//-----------------------------------------------------------------------------
// Movement hooks.
//-----------------------------------------------------------------------------
// CalcMainActivity leap branch: the code-driven state while a phase is live,
// then the script-driven state (which has no long-dive entry).
int ArmoredLeap_AnimActivity(const void* pPlayer)
{
	if (!pPlayer || AL_Field<uint8_t>(pPlayer, AL_OFF_LIFESTATE) != 0)
		return -1;
	const ArmoredLeapState* const s = s_leapMap.Find(pPlayer);
	if (!s)
		return -1;

	static const char* const s_pszActivities[AL_STATE_COUNT] =
	{
		nullptr,
		"ACT_MP_ARMORED_LEAP_AIR_START",
		"ACT_MP_ARMORED_LEAP_AIR_UP",
		"ACT_MP_ARMORED_LEAP_AIR_HOVER",
		"ACT_MP_ARMORED_LEAP_AIR_DIVE",
		"ACT_MP_ARMORED_LEAP_AIR_DIVE_LONG",
		"ACT_MP_ARMORED_LEAP_AIR_END",
		"ACT_MP_ARMORED_LEAP_GROUND_START",
		"ACT_MP_ARMORED_LEAP_GROUND_DASH",
		"ACT_MP_ARMORED_LEAP_GROUND_END",
	};
	static int s_nIds[AL_STATE_COUNT];
	static int s_nGeneration = -1;
	const int nGeneration = ActivityList_Generation();
	if (nGeneration != s_nGeneration)
	{
		s_nGeneration = nGeneration;
		for (int i = 0; i < AL_STATE_COUNT; ++i)
		{
			s_nIds[i] = s_pszActivities[i] ? FindActivityByName(s_pszActivities[i]) : -1;
			if (s_pszActivities[i] && s_nIds[i] < 0)
				Warning(eDLL_T::SERVER, "[AL-PHASE] %s is not registered -- that leap state keeps the default activity\n", s_pszActivities[i]);
		}
	}

	int nState = AL_STATE_NONE;
	if (s->m_nPhase != AL_PHASE_NONE)
		nState = s->m_nState;
	if (nState == AL_STATE_NONE && s->m_nStateDeprecated != AL_STATE_AIR_DIVE_LONG)
		nState = s->m_nStateDeprecated;
	if (nState <= AL_STATE_NONE || nState >= AL_STATE_COUNT)
		return -1;
	return s_nIds[nState];
}

bool ArmoredLeap_IsActive(const void* pPlayer)
{
	if (!pPlayer || !s_leapActive.Size() || TitanGate_IsTitanPlayer(pPlayer))
		return false;
	const ArmoredLeapState* const s = s_leapMap.Find(pPlayer);
	return s && s->m_nPhase != AL_PHASE_NONE;
}

static int AL_ActivePhase(const void* pPlayer)
{
	if (!pPlayer || !s_leapActive.Size())
		return AL_PHASE_NONE;
	const ArmoredLeapState* const s = s_leapMap.Find(pPlayer);
	return s ? s->m_nPhase : AL_PHASE_NONE;
}

void ArmoredLeap_BeginFullWalkMove(void* ctx)
{
	s_moveFrame = ArmoredLeapMoveFrame();
	s_moveFrame.ctx = ctx;
	s_moveFrame.bArmed = ctx != nullptr;
}

void ArmoredLeap_EndFullWalkMove(void)
{
	s_moveFrame = ArmoredLeapMoveFrame();
}

void ArmoredLeap_NoteHalfGravity(void* ctx, float flFrameTime)
{
	if (!s_moveFrame.bArmed || s_moveFrame.ctx != ctx || s_moveFrame.bGravityApplied)
		return;
	s_moveFrame.bGravityApplied = true;
	s_moveFrame.flFrameTime = flFrameTime;
}

static void Hook_CGameMovement_CheckVelocity(void* ctx)
{
	v_CGameMovement__CheckVelocity(ctx);

	if (!s_moveFrame.bArmed || !s_moveFrame.bGravityApplied || s_moveFrame.ctx != ctx)
		return;
	s_moveFrame.bArmed = false;
	// Client movement order: JetDriveAccel, then ArmoredLeapAccel.
	JetDrive_AccelInMove(ctx, s_moveFrame.flFrameTime);
	AL_Accel(ctx, s_moveFrame.flFrameTime);
}

static void Hook_CGameMovement_PlayerMove(void* ctx)
{
	if (ctx && armored_leap_disable_player_move_input.GetBool())
	{
		void* const pPlayer = *reinterpret_cast<void**>(AL_Bytes(ctx) + AL_CTX_OFF_PLAYER);
		uint8_t* const mv = *reinterpret_cast<uint8_t**>(AL_Bytes(ctx) + AL_CTX_OFF_MV);
		if (mv && ArmoredLeap_IsActive(pPlayer))
		{
			// Same fields the engine clears for a frozen player.
			*reinterpret_cast<float*>(mv + 0x30) = 0.0f;   // forward
			*reinterpret_cast<float*>(mv + 0x34) = 0.0f;   // side
			*reinterpret_cast<float*>(mv + 0x38) = 0.0f;   // up
			memset(mv + 0x78, 0, 12);
			memset(mv + 0x84, 0, 12);
		}
	}
	v_CGameMovement__PlayerMove(ctx);
}

void ArmoredLeap_BeforeDuck(void* ctx)
{
	s_nSavedForceStance = -1;
	void* const pPlayer = ctx ? *reinterpret_cast<void**>(AL_Bytes(ctx) + AL_CTX_OFF_PLAYER) : nullptr;
	if (!ArmoredLeap_IsActive(pPlayer))
		return;

	// The engine's own forced-stand rule is the client's "no duck while leaping".
	int& nForceStance = AL_Field<int>(pPlayer, AL_OFF_FORCESTANCE);
	s_nSavedForceStance = nForceStance;
	nForceStance = AL_FORCE_STANCE_STAND;
}

void ArmoredLeap_AfterDuck(void* ctx)
{
	void* const pPlayer = ctx ? *reinterpret_cast<void**>(AL_Bytes(ctx) + AL_CTX_OFF_PLAYER) : nullptr;
	if (!pPlayer)
		return;

	if (s_nSavedForceStance >= 0)
	{
		AL_Field<int>(pPlayer, AL_OFF_FORCESTANCE) = s_nSavedForceStance;
		s_nSavedForceStance = -1;
	}

	// Launcher flight ends while a leap, a jet drive or a zipline ride is live.
	if (ArmoredLeap_IsActive(pPlayer) || JetDrive_IsActive(reinterpret_cast<CPlayer*>(pPlayer))
		|| HalfDuck_PlayerIsZiplining(pPlayer))
		TriggerCannon_CancelFlightLock(pPlayer);

	ArmoredLeapState* const pState = ArmoredLeap_IsActive(pPlayer) ? s_leapMap.Find(pPlayer) : nullptr;
	if (!pState)
		return;
	AL_UpdatePhases(pPlayer, *pState);
	AL_UpdateState(*pState);
}

static void Hook_CGameMovement_CategorizePosition(void* ctx, bool bStayOnGround)
{
	void* const pPlayer = ctx ? *reinterpret_cast<void**>(AL_Bytes(ctx) + AL_CTX_OFF_PLAYER) : nullptr;
	const int nPhase = AL_ActivePhase(pPlayer);
	if (nPhase != AL_PHASE_PREP && nPhase != AL_PHASE_INTERRUPTED)
		return v_CGameMovement__CategorizePosition(ctx, bStayOnGround);

	uint8_t& bAirborne = AL_Field<uint8_t>(pPlayer, AL_OFF_FORCEAIRBORNE);
	const uint8_t bSaved = bAirborne;
	bAirborne = 1;
	v_CGameMovement__CategorizePosition(ctx, bStayOnGround);
	bAirborne = bSaved;
}

static bool Hook_CGameMovement_CheckJumpButton(void* ctx)
{
	void* const pPlayer = ctx ? *reinterpret_cast<void**>(AL_Bytes(ctx) + AL_CTX_OFF_PLAYER) : nullptr;
	if (!ArmoredLeap_IsActive(pPlayer))
		return v_CGameMovement__CheckJumpButton(ctx);

	// A pending jump is still consumed (and refused) by the original.
	if (AL_Field<uint8_t>(pPlayer, AL_OFF_JUMPPENDING))
		return v_CGameMovement__CheckJumpButton(ctx);
	return false;
}

static bool AL_EntityIsPlayer(void* pEntity)
{
	void** const vtable = *reinterpret_cast<void***>(pEntity);
	return reinterpret_cast<bool(__fastcall*)(void*)>(vtable[VTBL_ISPLAYER_SLOT])(pEntity);
}

class CArmoredLeapClassnameAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_iClassname;
};

static bool AL_EntityIsDeathBox(void* pEntity)
{
	const CArmoredLeapClassnameAccess* const pAccess = static_cast<CArmoredLeapClassnameAccess*>(
		reinterpret_cast<CBaseEntity*>(pEntity));
	const char* const pszClass = STRING(pAccess->m_iClassname);
	return pszClass && strcmp(pszClass, "prop_death_box") == 0;
}

// A leaping player passes through other players and death boxes.
static bool Hook_GameMovement_ShouldHitEntity(void* pMover, void* pHit)
{
	if (pHit && ArmoredLeap_IsActive(pMover) && (AL_EntityIsPlayer(pHit) || AL_EntityIsDeathBox(pHit)))
		return false;
	return v_GameMovement_ShouldHitEntity(pMover, pHit);
}

//-----------------------------------------------------------------------------
// Script natives.
//-----------------------------------------------------------------------------
static void* AL_ScriptPlayer(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return nullptr;
	return pEntity;
}

static bool AL_ReadFloats(HSQUIRRELVM v, const SQInteger nFirst, float* pOut, const int nCount, const char* pszNative)
{
	for (int i = 0; i < nCount; ++i)
	{
		SQFloat f = 0.0f;
		if (SQ_FAILED(sq_getfloat(v, nFirst + i, &f)) || !AL_IsFinite(f))
		{
			Warning(eDLL_T::SERVER, "[AL-PHASE] %s rejected non-finite argument %d\n", pszNative, static_cast<int>(nFirst + i - 1));
			return false;
		}
		pOut[i] = static_cast<float>(f);
	}
	return true;
}

static bool AL_ReadVector(HSQUIRRELVM v, const SQInteger nIdx, Vector3D& out, const char* pszNative)
{
	const SQVector3D* pVec = nullptr;
	if (SQ_FAILED(sq_getvector(v, nIdx, &pVec)) || !pVec)
		return false;
	out = Vector3D(pVec->x, pVec->y, pVec->z);
	if (!AL_IsFiniteVec(out))
	{
		Warning(eDLL_T::SERVER, "[AL-PHASE] %s rejected non-finite vector argument %d\n", pszNative, static_cast<int>(nIdx - 1));
		return false;
	}
	return true;
}

static void AL_ReadIgnoreEnts(HSQUIRRELVM v, const SQInteger nIdx, ArmoredLeapState& s)
{
	s.m_nIgnoreEnts = 0;
	const SQObjectPtr& arrObj = stack_get(v, nIdx);
	if (arrObj._type != OT_ARRAY || !_array(arrObj))
		return;

	const SQArray* const pArr = _array(arrObj);
	const SQInteger nSize = pArr->Size();
	for (SQInteger i = 0; i < nSize && s.m_nIgnoreEnts < kMaxIgnoreEnts; ++i)
	{
		const SQObjectPtr& el = pArr->_values[i];
		if (el._type != OT_ENTITY || !el._unVal.pInstance)
			continue;
		void* const pEnt = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(el._unVal.pInstance) + 0x50);
		const SDKEntityHandle h = pEnt ? SDKEntityState_GetHandle(pEnt) : SDKEntityHandle();
		if (h.IsValid())
			s.m_ignoreEnts[s.m_nIgnoreEnts++] = h.Raw();
	}
	if (nSize > kMaxIgnoreEnts)
		Warning(eDLL_T::SERVER, "[AL-PHASE] leap ignore list truncated %d -> %d\n", static_cast<int>(nSize), kMaxIgnoreEnts);
}

static void AL_Begin(void* pPlayer, ArmoredLeapState& s, const int nType)
{
	const float flNow = AL_CurTime();
	s.m_nType = nType;
	s.m_flStartTime = flNow;
	s.m_nPhase = AL_PHASE_PREP;
	s.m_flPhaseStartTime = flNow;
	s.m_vecStartPos = AL_AbsOrigin(pPlayer);
	s.m_nLastBlocked = 0;
	AL_Mirror(pPlayer, s);
	AL_Unduck(pPlayer);

	if (bridge_armored_leap_trace.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[AL-PHASE] player=%p start type=%d end=(%.1f %.1f %.1f) timeout=%.2f ignore=%d\n",
			pPlayer, nType, s.m_vecEndPos.x, s.m_vecEndPos.y, s.m_vecEndPos.z, s.m_flTimeOut, s.m_nIgnoreEnts);
}

static SQRESULT Script_StartArmoredLeapJump(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;

	float f[10];
	Vector3D airPos, endPos;
	float flTimeOut = 0.0f;
	if (!AL_ReadFloats(v, 3, f, 10, "StartArmoredLeapJump")
		|| !AL_ReadVector(v, 13, airPos, "StartArmoredLeapJump")
		|| !AL_ReadVector(v, 14, endPos, "StartArmoredLeapJump")
		|| !AL_ReadFloats(v, 15, &flTimeOut, 1, "StartArmoredLeapJump"))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	ArmoredLeapState& s = s_leapMap[pPlayer];
	AL_ReadIgnoreEnts(v, 2, s);
	s.m_flJumpSpeed = f[0];
	s.m_flJumpAccel = f[1];
	s.m_flTransitionSpeed = f[2];
	s.m_flTransitionAccel = f[3];
	s.m_flDiveSpeed = f[4];
	s.m_flDiveAccel = f[5];
	s.m_flPrepDuration = f[6];
	s.m_flArrivalDuration = f[7];
	s.m_flTransitionDist = f[8];
	s.m_flHoverDist = f[9];
	s.m_vecAirPos = airPos;
	s.m_vecEndPos = endPos;
	s.m_flTimeOut = flTimeOut;
	s.m_flPrepSpeed = 0.0f;
	s.m_flPrepAccel = 0.0f;
	AL_Begin(pPlayer, s, AL_TYPE_JUMP);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_StartArmoredLeapDash(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;

	float f[4];
	Vector3D endPos;
	float flTimeOut = 0.0f;
	if (!AL_ReadFloats(v, 3, f, 4, "StartArmoredLeapDash")
		|| !AL_ReadVector(v, 7, endPos, "StartArmoredLeapDash")
		|| !AL_ReadFloats(v, 8, &flTimeOut, 1, "StartArmoredLeapDash"))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	ArmoredLeapState& s = s_leapMap[pPlayer];
	AL_ReadIgnoreEnts(v, 2, s);
	s.m_flDiveSpeed = f[0];
	s.m_flDiveAccel = f[1];
	s.m_flPrepDuration = f[2];
	s.m_flArrivalDuration = f[3];
	s.m_flPrepSpeed = 0.0f;
	s.m_flPrepAccel = 0.0f;
	s.m_vecEndPos = endPos;
	s.m_flTimeOut = flTimeOut;
	AL_Begin(pPlayer, s, AL_TYPE_DASH);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_StartArmoredLeapAirDive(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;

	float f[6];
	Vector3D endPos;
	float flTimeOut = 0.0f;
	SQBool bLong = SQFalse;
	if (!AL_ReadFloats(v, 3, f, 6, "StartArmoredLeapAirDive")
		|| !AL_ReadVector(v, 9, endPos, "StartArmoredLeapAirDive")
		|| !AL_ReadFloats(v, 10, &flTimeOut, 1, "StartArmoredLeapAirDive")
		|| SQ_FAILED(sq_getbool(v, 11, &bLong)))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	ArmoredLeapState& s = s_leapMap[pPlayer];
	AL_ReadIgnoreEnts(v, 2, s);
	s.m_flPrepSpeed = f[0];
	s.m_flPrepAccel = f[1];
	s.m_flDiveSpeed = f[2];
	s.m_flDiveAccel = f[3];
	s.m_flPrepDuration = f[4];
	s.m_flArrivalDuration = f[5];
	s.m_vecEndPos = endPos;
	s.m_flTimeOut = flTimeOut;
	AL_Begin(pPlayer, s, bLong ? AL_TYPE_AIR_DIVE_LONG : AL_TYPE_AIR_DIVE);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsArmoredLeapActive(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;
	sq_pushbool(v, ArmoredLeap_IsActive(pPlayer));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_EndArmoredLeap(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;
	AL_End(pPlayer, s_leapMap[pPlayer]);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_UpdateArmoredLeapEndPos(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;
	Vector3D endPos;
	if (AL_ReadVector(v, 2, endPos, "UpdateArmoredLeapEndPos"))
	{
		ArmoredLeapState& s = s_leapMap[pPlayer];
		s.m_vecEndPos = endPos;
		AL_Mirror(pPlayer, s);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetArmoredLeapState(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;
	SQInteger nState = 0;
	sq_getinteger(v, 2, &nState);
	if (nState < AL_STATE_NONE || nState >= AL_STATE_COUNT)
	{
		Warning(eDLL_T::SERVER, "[AL-PHASE] SetArmoredLeapState out of range %d\n", static_cast<int>(nState));
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	s_leapMap[pPlayer].m_nState = static_cast<int>(nState);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetArmoredLeapState(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;
	const ArmoredLeapState* const s = s_leapMap.Find(pPlayer);
	sq_pushinteger(v, s ? s->m_nState : AL_STATE_NONE);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetArmoredLeapStateDeprecated(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;
	SQInteger nState = 0;
	sq_getinteger(v, 2, &nState);
	s_leapMap[pPlayer].m_nStateDeprecated = static_cast<int>(nState);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_FinishArmoredLeapDeprecated(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;
	if (ArmoredLeapState* const s = s_leapMap.Find(pPlayer))
		s->m_nStateDeprecated = 0;
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetArmoredLeapStateDeprecated(HSQUIRRELVM v)
{
	void* const pPlayer = AL_ScriptPlayer(v);
	if (!pPlayer)
		return SQ_ERROR;
	const ArmoredLeapState* const s = s_leapMap.Find(pPlayer);
	sq_pushinteger(v, s ? s->m_nStateDeprecated : 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void ArmoredLeap_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction("StartArmoredLeapJump", "Script_StartArmoredLeapJump",
		"Starts an armored leap that climbs to airPos and dives to endPos", "void",
		"array< entity > ignoreEnts, float jumpSpeed, float jumpAccel, float transitionSpeed, float transitionAccel, "
		"float diveSpeed, float diveAccel, float prepDuration, float arrivalDuration, float transitionDistanceFromAirPos, "
		"float hoverDistanceFromAirPos, vector airPos, vector endPos, float timeOut",
		false, Script_StartArmoredLeapJump);
	playerStruct->AddFunction("StartArmoredLeapDash", "Script_StartArmoredLeapDash",
		"Starts an armored leap that dashes along the ground to endPos", "void",
		"array< entity > ignoreEnts, float diveSpeed, float diveAccel, float prepDuration, float arrivalDuration, "
		"vector endPos, float timeOut",
		false, Script_StartArmoredLeapDash);
	playerStruct->AddFunction("StartArmoredLeapAirDive", "Script_StartArmoredLeapAirDive",
		"Starts an armored leap that dives from the air to endPos", "void",
		"array< entity > ignoreEnts, float prepSpeed, float prepAccel, float diveSpeed, float diveAccel, "
		"float prepDuration, float arrivalDuration, vector endPos, float timeOut, bool isAirDiveLong",
		false, Script_StartArmoredLeapAirDive);
	playerStruct->AddFunction("IsArmoredLeapActive", "Script_IsArmoredLeapActive",
		"Is an armored leap in progress", "bool", "", false, Script_IsArmoredLeapActive);
	playerStruct->AddFunction("EndArmoredLeap", "Script_EndArmoredLeap",
		"Ends the armored leap immediately", "void", "", false, Script_EndArmoredLeap);
	playerStruct->AddFunction("UpdateArmoredLeapEndPos", "Script_UpdateArmoredLeapEndPos",
		"Moves the destination of the armored leap", "void", "vector endPos", false, Script_UpdateArmoredLeapEndPos);
	playerStruct->AddFunction("SetArmoredLeapState", "Script_SetArmoredLeapState",
		"Sets the armored leap animation state", "void", "int state", false, Script_SetArmoredLeapState);
	playerStruct->AddFunction("GetArmoredLeapState", "Script_GetArmoredLeapState",
		"Gets the armored leap animation state", "int", "", false, Script_GetArmoredLeapState);
	playerStruct->AddFunction("Player_SetArmoredLeapState_Depricated", "Script_SetArmoredLeapStateDeprecated",
		"Sets the script-driven armored leap state", "void", "int state", false, Script_SetArmoredLeapStateDeprecated);
	playerStruct->AddFunction("Player_FinishArmoredLeap_Depricated", "Script_FinishArmoredLeapDeprecated",
		"Clears the script-driven armored leap state", "void", "", false, Script_FinishArmoredLeapDeprecated);
	playerStruct->AddFunction("GetArmoredLeapState_Depricated", "Script_GetArmoredLeapStateDeprecated",
		"Gets the script-driven armored leap state", "int", "", false, Script_GetArmoredLeapStateDeprecated);
}

void ArmoredLeap_RegisterScriptConstants(CSquirrelVM* s)
{
	if (!s)
		return;

	s->RegisterConstant("PLAYER_ARMORED_LEAP_PHASE_NONE", AL_PHASE_NONE);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_PHASE_PREP", AL_PHASE_PREP);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_PHASE_TRAVEL_AIR", AL_PHASE_TRAVEL_AIR);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_PHASE_TRAVEL_AIR_HOVER", AL_PHASE_TRAVEL_AIR_HOVER);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_PHASE_TRAVEL_GROUND", AL_PHASE_TRAVEL_GROUND);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_PHASE_ARRIVAL", AL_PHASE_ARRIVAL);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_PHASE_INTERRUPTED", AL_PHASE_INTERRUPTED);

	s->RegisterConstant("PLAYER_ARMORED_LEAP_TYPE_NONE", AL_TYPE_NONE);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_TYPE_DASH", AL_TYPE_DASH);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_TYPE_JUMP", AL_TYPE_JUMP);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_TYPE_AIR_DIVE", AL_TYPE_AIR_DIVE);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_TYPE_AIR_DIVE_LONG", AL_TYPE_AIR_DIVE_LONG);

	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_NONE", AL_STATE_NONE);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_AIR_START", AL_STATE_AIR_START);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_AIR_UP", AL_STATE_AIR_UP);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_AIR_HOVER", AL_STATE_AIR_HOVER);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_AIR_DIVE", AL_STATE_AIR_DIVE);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_AIR_DIVE_LONG", AL_STATE_AIR_DIVE_LONG);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_AIR_END", AL_STATE_AIR_END);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_GROUND_START", AL_STATE_GROUND_START);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_GROUND_DASH", AL_STATE_GROUND_DASH);
	s->RegisterConstant("PLAYER_ARMORED_LEAP_STATE_GROUND_END", AL_STATE_GROUND_END);
}

void ArmoredLeap_LevelShutdown(void)
{
	s_leapMap.Clear();
	s_leapActive.Clear();
	s_hPhaseCallback = nullptr;
	s_hPhaseCallbackVM = nullptr;
	s_moveFrame = ArmoredLeapMoveFrame();
	s_nSavedForceStance = -1;
}

//-----------------------------------------------------------------------------
// Detour registration.
//-----------------------------------------------------------------------------
void VArmoredLeap::GetAdr(void) const
{
	LogFunAdr("CGameMovement::PlayerMove", v_CGameMovement__PlayerMove);
	LogFunAdr("CGameMovement::CategorizePosition", v_CGameMovement__CategorizePosition);
	LogFunAdr("CGameMovement::CheckJumpButton", v_CGameMovement__CheckJumpButton);
	LogFunAdr("GameMovement_ShouldHitEntity", v_GameMovement_ShouldHitEntity);
	LogFunAdr("CGameMovement::CheckVelocity", v_CGameMovement__CheckVelocity);
	LogFunAdr("CGameMovement::SetGroundEntity", v_CGameMovement__SetGroundEntity);
	LogFunAdr("CBaseEntity::SetAbsAngles", v_CBaseEntity__SetAbsAngles);
}

void VArmoredLeap::GetFun(void) const
{
	// Server halves: frame sizes and server-only field offsets separate each
	// of these from its client twin.
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 41 55 48 8D A8 C8 FA FF FF 48 81 EC 28 06 00 00 4C 8B 41 08")
		.GetPtr(v_CGameMovement__PlayerMove);
	Module_FindPattern(g_GameDll,
		"40 55 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 41 08 44 0F B6 D2 48 8B F9 "
		"41 BB 00 02 00 00 C7 80 E0 6C 00 00 00 00 80 3F")
		.GetPtr(v_CGameMovement__CategorizePosition);
	Module_FindPattern(g_GameDll,
		"40 55 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 51 08 48 8B F9 80 BA 5C 5E 00 00 00")
		.GetPtr(v_CGameMovement__CheckJumpButton);
	Module_FindPattern(g_GameDll,
		"40 53 57 48 83 EC 28 F6 82 30 02 00 00 01 48 8B DA 48 8B F9")
		.GetPtr(v_GameMovement_ShouldHitEntity);
	// Velocity at CMoveData+0x130 is the server layout; the twin reads +0x124.
	Module_FindPattern(g_GameDll,
		"40 55 48 8B EC 48 83 EC 50 48 8B 51 10 4C 8B C9 F3 0F 10 89 6C 04 00 00 "
		"F3 0F 10 91 70 04 00 00 F3 0F 10 81 74 04 00 00 F3 0F 5C 92 34 01 00 00")
		.GetPtr(v_CGameMovement__CheckVelocity);
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 56 57 48 8D A8 18 FE FF FF 48 81 EC D0 02 00 00 44 0F 29 A0 78 FF FF FF 48 8B F2")
		.GetPtr(v_CGameMovement__SetGroundEntity);
	Module_FindPattern(g_GameDll,
		"40 55 53 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? "
		"F3 0F 10 07 0F 2E 83 5C 04 00 00 7A ?? 75 ?? F3 0F 10 47 04 0F 2E 83 60 04 00 00")
		.GetPtr(v_CBaseEntity__SetAbsAngles);

	if (!v_CGameMovement__PlayerMove || !v_CGameMovement__CategorizePosition || !v_CGameMovement__CheckJumpButton
		|| !v_GameMovement_ShouldHitEntity || !v_CGameMovement__CheckVelocity)
		Warning(eDLL_T::SERVER,
			"[AL-PHASE] movement pattern unresolved (move=%d categorize=%d jump=%d hit=%d checkvel=%d) -- "
			"the leap mispredicts on every client\n",
			v_CGameMovement__PlayerMove ? 1 : 0, v_CGameMovement__CategorizePosition ? 1 : 0,
			v_CGameMovement__CheckJumpButton ? 1 : 0, v_GameMovement_ShouldHitEntity ? 1 : 0,
			v_CGameMovement__CheckVelocity ? 1 : 0);
	if (!v_CGameMovement__SetGroundEntity || !v_CBaseEntity__SetAbsAngles)
		Warning(eDLL_T::SERVER,
			"[AL-PHASE] helper pattern unresolved (ground=%d angles=%d)\n",
			v_CGameMovement__SetGroundEntity ? 1 : 0, v_CBaseEntity__SetAbsAngles ? 1 : 0);
}

void VArmoredLeap::Detour(const bool bAttach) const
{
	if (v_CGameMovement__PlayerMove)
		DetourSetup(&v_CGameMovement__PlayerMove, &Hook_CGameMovement_PlayerMove, bAttach);
	if (v_CGameMovement__CategorizePosition)
		DetourSetup(&v_CGameMovement__CategorizePosition, &Hook_CGameMovement_CategorizePosition, bAttach);
	if (v_CGameMovement__CheckJumpButton)
		DetourSetup(&v_CGameMovement__CheckJumpButton, &Hook_CGameMovement_CheckJumpButton, bAttach);
	if (v_GameMovement_ShouldHitEntity)
		DetourSetup(&v_GameMovement_ShouldHitEntity, &Hook_GameMovement_ShouldHitEntity, bAttach);
	if (v_CGameMovement__CheckVelocity)
		DetourSetup(&v_CGameMovement__CheckVelocity, &Hook_CGameMovement_CheckVelocity, bAttach);
}
