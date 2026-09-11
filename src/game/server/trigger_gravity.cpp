//=============================================================================//
//
// Purpose: Server-authoritative TT_GRAVITY_LIFT / TT_BLACKHOLE force. See header.
//
//=============================================================================//
#include "core/stdafx.h"


#include "trigger_gravity.h"
#include "trigger_cannon.h"
#include "player.h"
#include "baseentity.h"
#include "entitylist.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/alliance_compat.h"
#include "mathlib/mathlib.h"
#include "engine/cmodel.h"
#include "public/gametrace.h"
#include <cmath>
#include <cstring>

//-----------------------------------------------------------------------------
// Raw layout -- movement ctx, CMoveData, player, trigger.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t TG_CTX_OFF_PLAYER   = 8;   // CPlayer*
static constexpr ptrdiff_t TG_CTX_OFF_MOVEDATA = 16;  // CMoveData*

static constexpr ptrdiff_t TG_MV_OFF_MOVEDIR2D = 120; // float[3]
static constexpr ptrdiff_t TG_MV_OFF_VELOCITY  = 304; // float[3] -- dedi, not client 292

// CBaseEntity fields shared by player and trigger.
static constexpr ptrdiff_t TG_ENT_OFF_EFLAGS       = 560;  // bit 0x800 = abs transform dirty
static constexpr ptrdiff_t TG_ENT_OFF_MOVETYPE     = 776;  // char; 12 == MOVETYPE_TRAVERSE
static constexpr ptrdiff_t TG_ENT_OFF_UP_X         = 1032; // matrix3x4 third column
static constexpr ptrdiff_t TG_ENT_OFF_UP_Y         = 1048;
static constexpr ptrdiff_t TG_ENT_OFF_UP_Z         = 1064;
static constexpr ptrdiff_t TG_ENT_OFF_ABS_ORIGIN   = 1104; // float[3]
static constexpr ptrdiff_t TG_ENT_OFF_LIFESTATE    = 1177; // char
static constexpr ptrdiff_t TG_ENT_OFF_TEAMNUM      = 1428; // int

// CPlayer.
static constexpr ptrdiff_t TG_PLAYER_OFF_PHASE_START      = 5556;
static constexpr ptrdiff_t TG_PLAYER_OFF_PHASE_END        = 5560;
static constexpr ptrdiff_t TG_PLAYER_OFF_FLOORHEIGHT      = 23968; // m_wallrunLatestFloorHeight
static constexpr ptrdiff_t TG_PLAYER_OFF_UPDIR            = 26156; // float[3]
static constexpr ptrdiff_t TG_PLAYER_OFF_WALLHANGING      = 26265; // bool
static constexpr ptrdiff_t TG_PLAYER_OFF_GRAPPLEPULLING   = 26497; // bool
static constexpr ptrdiff_t TG_PLAYER_OFF_GRAPPLEACTIVE    = 26552; // bool
static constexpr ptrdiff_t TG_PLAYER_OFF_TOUCHED_TRIG     = 27096; // EHANDLE[16]
static constexpr ptrdiff_t TG_PLAYER_OFF_TOUCHED_COUNT    = 27160; // int64

// CTriggerCylinderHeavy native fields.
static constexpr ptrdiff_t TG_TRIG_OFF_RADIUS      = 3344;
static constexpr ptrdiff_t TG_TRIG_OFF_ABOVEHEIGHT = 3348;
static constexpr ptrdiff_t TG_TRIG_OFF_TRIGGERTYPE = 3384;

static constexpr int TG_TOUCHED_CAP = 16;
static constexpr int TG_TRIGGER_TYPE_GRAVITY_LIFT = 4;
static constexpr int TG_TRIGGER_TYPE_BLACKHOLE    = 8;
static constexpr int TG_MOVETYPE_TRAVERSE         = 12;

// S21 gravity-lift / blackhole folded constants.
static constexpr float TG_EJECT_DEBOUNCE         = 2.0f;    // lift entry gate only
static constexpr float TG_MOVE_UP_THRESHOLD      = 5.0f;
static constexpr float TG_GRAPPLE_PULL_MOVE_MULT = 2.5f;
static constexpr float TG_EJECT_TRAVEL_THRESH_SQ = 1600.0f; // 40 units squared
static constexpr float TG_TOCENTER_INNER         = 20.0f;
static constexpr float TG_WALLRUN_UP_DOT         = 0.999f;
static constexpr float TG_SQRT_FLT_MIN           = 1.0e-19f;
static constexpr float TG_BLACKHOLE_LOS_BASE_Z    = 64.0f; // S21-folded LOS probe base height
static constexpr float TG_BLACKHOLE_LOS_CAND_SCALE = 32.0f; // S21-folded candidate spacing

// Candidate probe offsets: origin, up, down, four cardinals, four diagonals.
static constexpr float s_blackholeLosOffsets[][3] = {
	{  0.0f,  0.0f,  0.0f },
	{  0.0f,  0.0f,  2.0f },
	{  0.0f,  0.0f, -1.5f },
	{  0.0f, -2.0f,  0.0f },
	{  0.0f,  2.0f,  0.0f },
	{ -2.0f,  0.0f,  0.0f },
	{  2.0f,  0.0f,  0.0f },
	{  1.4f, -1.4f,  0.0f },
	{  1.4f,  1.4f,  0.0f },
	{ -1.4f, -1.4f,  0.0f },
	{ -1.4f,  1.4f,  0.0f },
};

// UTIL_TraceLine_IgnoreEntity_WithDetail
static void (*v_UTIL_TraceLine_IgnoreEntity_WithDetail)(
	const Vector3D* pStart, const Vector3D* pEnd, unsigned int nMask,
	const IHandleEntity* pIgnore, int nCollisionGroup, int nDetailLevel,
	int nUpdateDirty, trace_t* pTrace, void* pOptionalEnt) = nullptr;

//-----------------------------------------------------------------------------
// ConVars.
//-----------------------------------------------------------------------------
static ConVar bridge_trigger_gravity_lift(
	"bridge_trigger_gravity_lift", "1", FCVAR_RELEASE,
	"Apply the server-authoritative TT_GRAVITY_LIFT force for Heavy triggers of "
	"type 4. The S21 client already predicts this type; without it the server "
	"never lifts and the player rubber-bands back.");

// Fires the trigger's script enter callback the first tick a player is inside.
static ConVar bridge_trigger_gravity_enter_cb(
	"bridge_trigger_gravity_enter_cb", "1", FCVAR_RELEASE,
	"Raise the trigger's script enter callback when a player enters a gravity "
	"lift or blackhole. Off skips the callback and applies the force only.");

// Hard-capped census of triggers the gravity pass walks and the type it reads.
static ConVar bridge_trigger_gravity_walk_log(
	"bridge_trigger_gravity_walk_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log the first few triggers the gravity pass walks, with the type it read.");

// The pass runs several times per frame, so a low cap burns before a player can
// walk into anything.
static constexpr int TG_WALK_LOG_CAP = 256;
static int s_nWalkLogCount = 0;

static ConVar bridge_trigger_blackhole(
	"bridge_trigger_blackhole", "1", FCVAR_RELEASE,
	"Apply the server-authoritative TT_BLACKHOLE force for Heavy triggers of "
	"type 8. The S21 client already predicts this type; without it the server "
	"never pulls and the player rubber-bands back.");

// Boot-time read: dt_extend decides whether to append at SendTable_Init.
static ConVar bridge_trigger_gravity_wire(
	"bridge_trigger_gravity_wire", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Replicate m_gravityLiftActive / m_blackholeActive to the S21 client. "
	"Off must mean not on the wire at all -- an appended zero still ships every "
	"snapshot and is what produced the prediction-correction storm. "
	"Set it as a launch arg, not in-console.");

static ConVar bridge_trigger_gravity_diag(
	"bridge_trigger_gravity_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log [TRIG-GRAV] lift/blackhole branch predicates and skip reasons "
	"(hard-capped per session).");

static constexpr int TG_DIAG_CAP = 256;
static int s_nDiagCount = 0;

//-----------------------------------------------------------------------------
// Skip census. Each reason has its own budget -- the pass runs several times per frame.
//-----------------------------------------------------------------------------
enum TriggerGravitySite_t
{
	TG_SITE_LIFT = 0,
	TG_SITE_BLACKHOLE,
	TG_SITE_COUNT
};

enum TriggerGravitySkip_t
{
	TG_SKIP_LEVER = 0,
	TG_SKIP_PARAMS,
	TG_SKIP_LOS,
	TG_SKIP_DEBOUNCE,
	TG_SKIP_PHASE,
	TG_SKIP_MOVETYPE,
	TG_SKIP_WALLHANG,
	TG_SKIP_WALLRUN,
	TG_SKIP_ARMOREDLEAP,
	TG_SKIP_COUNT
};

static constexpr int TG_SKIP_CAP = 8; // per site, per reason, per session
static int s_nSkipCount[TG_SITE_COUNT][TG_SKIP_COUNT] = {};

// True exactly TG_SKIP_CAP times per (site, reason); the caller then logs.
static bool TriggerGravity_ShouldLogSkip(TriggerGravitySite_t site, TriggerGravitySkip_t reason)
{
	if (!bridge_trigger_gravity_diag.GetBool())
		return false;

	if (s_nSkipCount[site][reason] >= TG_SKIP_CAP)
		return false;

	++s_nSkipCount[site][reason];
	return true;
}

static void TriggerGravity_ResetSkipCensus(void)
{
	memset(s_nSkipCount, 0, sizeof(s_nSkipCount));
	s_nDiagCount = 0;
	s_nWalkLogCount = 0;
}

//-----------------------------------------------------------------------------
// Per-player state. No entity memory for any of the seven fields.
//-----------------------------------------------------------------------------
struct GravityLiftState
{
	bool  m_gravityLiftActive = false;
	float m_gravityLiftEnterTime = 0.0f;
	float m_gravityLiftEjectTime = 0.0f;
	float m_gravityLiftHoverTime = 0.0f;
	float m_gravityLiftLastOrigin[3] = {};
	float m_gravityLiftEjectVelocity[3] = {};
	bool  m_blackholeActive = false;
};

static SDKEntityMap<GravityLiftState> s_gravityMap(ESide::Server, "trigGrav.srv");

//-----------------------------------------------------------------------------
// Wire sidecar -- two bits in one LONG. No seqlock: one aligned word cannot tear.
//-----------------------------------------------------------------------------
struct TriggerGravityWireSlot
{
	volatile uint64_t handleKey; // 0 = free
	volatile LONG     bits;      // bit0 lift, bit1 blackhole
};

static TriggerGravityWireSlot s_trigGravWireSlots[64];
static LONG s_trigGravWireCursor = 0;
static volatile LONG s_trigGravWireUsed = 0;

static inline uint64_t TriggerGravityWire_PackHandle(const SDKEntityHandle& h)
{
	return h.IsValid() ? static_cast<uint64_t>(h.Raw()) : 0;
}

static TriggerGravityWireSlot* TriggerGravityWire_FindSlot(uint64_t key)
{
	if (!key || !s_trigGravWireUsed)
		return nullptr;

	for (TriggerGravityWireSlot& slot : s_trigGravWireSlots)
	{
		if (slot.handleKey == key)
			return &slot;
	}
	return nullptr;
}

static void TriggerGravityWire_Publish(void* pPlayer, int nLiftActive, int nBlackholeActive)
{
	const uint64_t key = TriggerGravityWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	if (!key)
		return;

	const LONG bits =
		(nLiftActive ? 1 : 0) | (nBlackholeActive ? 2 : 0);

	TriggerGravityWireSlot* slot = TriggerGravityWire_FindSlot(key);
	if (!slot)
	{
		for (TriggerGravityWireSlot& cand : s_trigGravWireSlots)
		{
			if (cand.handleKey == 0)
			{
				slot = &cand;
				break;
			}
		}
		if (slot)
			InterlockedIncrement(&s_trigGravWireUsed);
		else
		{
			const LONG idx = (InterlockedIncrement(&s_trigGravWireCursor) - 1) & 63;
			slot = &s_trigGravWireSlots[idx];
		}
		slot->handleKey = 0;
	}

	InterlockedExchange(&slot->bits, bits);
	slot->handleKey = key;

	static volatile LONG s_pubN = 0;
	const LONG n = InterlockedIncrement(&s_pubN);
	if (n <= 8)
		Warning(eDLL_T::SERVER,
			"[TRIGGRAV-WIRE] publish #%d player=%p key=0x%llX lift=%d blackhole=%d\n",
			static_cast<int>(n), pPlayer, static_cast<unsigned long long>(key),
			nLiftActive, nBlackholeActive);
}

// Publish + dirty only when the two wire bits change.
static void TriggerGravity_Mirror(void* pPlayer, const GravityLiftState& s)
{
	if (!pPlayer)
		return;

	const uint64_t key = TriggerGravityWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	const LONG want =
		(s.m_gravityLiftActive ? 1 : 0) | (s.m_blackholeActive ? 2 : 0);

	TriggerGravityWireSlot* const slot = TriggerGravityWire_FindSlot(key);
	const LONG had = slot ? slot->bits : 0;
	if (slot && had == want)
		return;

	TriggerGravityWire_Publish(pPlayer,
		s.m_gravityLiftActive ? 1 : 0,
		s.m_blackholeActive ? 1 : 0);
	MarkEntityEdictDirty(pPlayer);
}

bool TriggerGravity_WireEnabled(void)
{
	return bridge_trigger_gravity_wire.GetBool();
}

bool TriggerGravity_IsLiftActive(const void* pPlayer)
{
	if (!pPlayer)
		return false;

	const GravityLiftState* const pState = s_gravityMap.Find(pPlayer);
	return pState && pState->m_gravityLiftActive;
}

bool TriggerGravity_GetWire(const void* pPlayer, TriggerGravityWire* pOut)
{
	if (!pPlayer || !pOut || !bridge_trigger_gravity_wire.GetBool())
		return false;

	const uint64_t key = TriggerGravityWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	TriggerGravityWireSlot* const slot = TriggerGravityWire_FindSlot(key);
	if (!slot)
		return false;

	const LONG bits = InterlockedCompareExchange(&slot->bits, 0, 0);
	if (slot->handleKey != key)
		return false;

	pOut->m_gravityLiftActive = (bits & 1) ? 1 : 0;
	pOut->m_blackholeActive = (bits & 2) ? 1 : 0;
	return true;
}

void TriggerGravity_Wire_LevelShutdown(void)
{
	InterlockedExchange(&s_trigGravWireUsed, 0);
	memset(s_trigGravWireSlots, 0, sizeof(s_trigGravWireSlots));
	InterlockedExchange(&s_trigGravWireCursor, 0);
	TriggerGravity_ResetSkipCensus();
}

//-----------------------------------------------------------------------------
// Appended prop offsets -- resolved lazily after datatable extend.
// 0 = not yet tried, -1 = failed, >0 = entity offset.
//-----------------------------------------------------------------------------
static int s_nLiftUpSpeedOff = 0;
static int s_nLiftUpAccelOff = 0;
static int s_nAirMoveSpeedOff = 0;
static int s_nAirMoveAccelOff = 0;
static int s_nLiftToCenterSpeedOff = 0;
static int s_nLiftToCenterAccelOff = 0;
static int s_nLiftEjectUpOff = 0;
static int s_nLiftEjectFwdOff = 0;
static int s_nLiftMaxEjectOff = 0;
static int s_nLiftMaxHoverOff = 0;
static int s_nBhStrongAddlOff = 0;
static int s_nBhOuterPullOff = 0;
static int s_nBhInnerPullOff = 0;
static int s_nBhOuterMoveOff = 0;
static int s_nBhInnerMoveOff = 0;
static int s_nBhInnerRadiusOff = 0;
static int s_nBhStrongPullingOff = 0;


static int TriggerGravity_ResolveOne(const char* pszName, int* pSlot)
{
	if (*pSlot != 0)
		return *pSlot;

	const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", pszName);
	*pSlot = nOff > 0 ? nOff : -1;
	if (nOff <= 0)
		Warning(eDLL_T::SERVER,
			"[TRIG-GRAV] %s offset unresolved -- gravity lift/blackhole setters disabled\n",
			pszName);
	return *pSlot;
}

static bool TriggerGravity_ResolveLiftParams(void)
{
	bool bOk = true;
	bOk = TriggerGravity_ResolveOne("m_gravityLiftUpSpeed", &s_nLiftUpSpeedOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_gravityLiftUpAccel", &s_nLiftUpAccelOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_airControlMoveSpeed", &s_nAirMoveSpeedOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_airControlMoveAccel", &s_nAirMoveAccelOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_gravityLiftToCenterSpeed", &s_nLiftToCenterSpeedOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_gravityLiftToCenterAccel", &s_nLiftToCenterAccelOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_gravityLiftEjectUpSpeed", &s_nLiftEjectUpOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_gravityLiftEjectForwardSpeed", &s_nLiftEjectFwdOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_gravityLiftMaxEjectTime", &s_nLiftMaxEjectOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_gravityLiftMaxHoverTime", &s_nLiftMaxHoverOff) > 0 && bOk;
	return bOk;
}

static bool TriggerGravity_ResolveBlackholeParams(void)
{
	bool bOk = true;
	bOk = TriggerGravity_ResolveOne("m_blackholeStrongPullAddlSpeed", &s_nBhStrongAddlOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_blackholeOuterPullSpeed", &s_nBhOuterPullOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_blackholeInnerPullSpeed", &s_nBhInnerPullOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_blackholeOuterMoveSpeed", &s_nBhOuterMoveOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_blackholeInnerMoveSpeed", &s_nBhInnerMoveOff) > 0 && bOk;
	bOk = TriggerGravity_ResolveOne("m_blackholeInnerRadius", &s_nBhInnerRadiusOff) > 0 && bOk;
	return bOk;
}

static bool TriggerGravity_ResolveBlackholeStrong(void)
{
	return TriggerGravity_ResolveOne("m_blackholeIsStrongPulling", &s_nBhStrongPullingOff) > 0;
}

static void TriggerGravity_ResolvePlayerExtras(void)
{
}

//-----------------------------------------------------------------------------
// Helpers.
//-----------------------------------------------------------------------------
static float TriggerGravity_GraphCapped(float flX, float flInLo, float flInHi,
	float flOutLo, float flOutHi)
{
	if (flInHi == flInLo)
		return flOutLo;

	float flT = (flX - flInLo) / (flInHi - flInLo);
	if (flT < 0.0f)
		flT = 0.0f;
	else if (flT > 1.0f)
		flT = 1.0f;
	return flOutLo + (flOutHi - flOutLo) * flT;
}

static bool TriggerGravity_IsFinite3(const float v[3])
{
	return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// A living player inside the phase-shift window is immune; everyone else is
// affected. Upper bound is inclusive.
static bool TriggerGravity_IsPhaseShiftImmune(const uint8_t* pPlayerBytes, float flNow)
{
	const char lifeState = *reinterpret_cast<const char*>(pPlayerBytes + TG_ENT_OFF_LIFESTATE);
	if (lifeState != 0)
		return false;

	const float flStart = *reinterpret_cast<const float*>(pPlayerBytes + TG_PLAYER_OFF_PHASE_START);
	const float flEnd = *reinterpret_cast<const float*>(pPlayerBytes + TG_PLAYER_OFF_PHASE_END);
	if (!std::isfinite(flStart) || !std::isfinite(flEnd))
		return false;

	return flNow >= flStart && flNow <= flEnd;
}

// The predicate's three raw inputs. A living player outside any phase shift is
// expected to read start == end == 0; anything else means the window is being
// held open by a value the old liveness-only test never had to look at.
static void TriggerGravity_LogPhaseSkip(TriggerGravitySite_t site,
	const uint8_t* pPlayerBytes, float flNow)
{
	if (!TriggerGravity_ShouldLogSkip(site, TG_SKIP_PHASE))
		return;

	Warning(eDLL_T::SERVER,
		"[TRIG-GRAV] %s skip: phase life=%d start=%.3f end=%.3f now=%.3f\n",
		site == TG_SITE_LIFT ? "lift" : "blackhole",
		static_cast<int>(*reinterpret_cast<const char*>(pPlayerBytes + TG_ENT_OFF_LIFESTATE)),
		*reinterpret_cast<const float*>(pPlayerBytes + TG_PLAYER_OFF_PHASE_START),
		*reinterpret_cast<const float*>(pPlayerBytes + TG_PLAYER_OFF_PHASE_END),
		flNow);
}

static int TriggerGravity_ArmoredLeapPhase(const uint8_t* pPlayerBytes)
{
	return PlayerExtend_GetI32(pPlayerBytes, offsetof(PlayerExtendWire, m_armoredLeapPhase));
}

// S21 form of IsWallRunning with identity gravity transform.
static bool TriggerGravity_IsWallRunning(const uint8_t* pPlayerBytes)
{
	if (TriggerGravity_ArmoredLeapPhase(pPlayerBytes) != 0)
		return false;

	const float flUpZ = *reinterpret_cast<const float*>(
		pPlayerBytes + TG_PLAYER_OFF_UPDIR + 8);
	return flUpZ < TG_WALLRUN_UP_DOT;
}

static void TriggerGravity_ReadUpAxis(const uint8_t* pTrigBytes, float outUp[3])
{
	outUp[0] = *reinterpret_cast<const float*>(pTrigBytes + TG_ENT_OFF_UP_X);
	outUp[1] = *reinterpret_cast<const float*>(pTrigBytes + TG_ENT_OFF_UP_Y);
	outUp[2] = *reinterpret_cast<const float*>(pTrigBytes + TG_ENT_OFF_UP_Z);
}

// 11-candidate LOS probe; both traces must be clear for a candidate to accept.
static bool TriggerGravity_CanBlackholeSeePlayer(void* pPlayer, const float triggerOrigin[3])
{
	if (!pPlayer || !triggerOrigin)
		return false;

	if (!v_UTIL_TraceLine_IgnoreEntity_WithDetail)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] UTIL_TraceLine_IgnoreEntity_WithDetail unresolved -- "
				"LOS gate disabled, pull works through walls\n");
		}
		return true;
	}

	// Rendered and collision center are the same for a player (no render-bounds override).
	Vector3D center;
	CM_WorldSpaceCenter(
		reinterpret_cast<CBaseEntity*>(pPlayer)->CollisionProp(), &center);

	const Vector3D rawOrigin(triggerOrigin[0], triggerOrigin[1], triggerOrigin[2]);
	const Vector3D base(
		triggerOrigin[0],
		triggerOrigin[1],
		triggerOrigin[2] + TG_BLACKHOLE_LOS_BASE_Z);

	const size_t nCand = sizeof(s_blackholeLosOffsets) / sizeof(s_blackholeLosOffsets[0]);
	for (size_t i = 0; i < nCand; ++i)
	{
		const Vector3D cand(
			base.x + s_blackholeLosOffsets[i][0] * TG_BLACKHOLE_LOS_CAND_SCALE,
			base.y + s_blackholeLosOffsets[i][1] * TG_BLACKHOLE_LOS_CAND_SCALE,
			base.z + s_blackholeLosOffsets[i][2] * TG_BLACKHOLE_LOS_CAND_SCALE);

		// Zero-init: unwritten result reads as fraction 0 / blocked.
		trace_t trToPlayer{};
		v_UTIL_TraceLine_IgnoreEntity_WithDetail(
			&cand, &center, 0x420Bu,
			reinterpret_cast<const IHandleEntity*>(pPlayer),
			21, 2, 1, &trToPlayer, nullptr);
		if (trToPlayer.fraction < 1.0f || trToPlayer.allsolid || trToPlayer.startsolid)
			continue;

		// Second leg targets the raw blackhole origin (no +64).
		trace_t trToOrigin{};
		v_UTIL_TraceLine_IgnoreEntity_WithDetail(
			&cand, &rawOrigin, 0x420Bu,
			reinterpret_cast<const IHandleEntity*>(pPlayer),
			21, 2, 1, &trToOrigin, nullptr);
		if (trToOrigin.fraction < 1.0f || trToOrigin.allsolid || trToOrigin.startsolid)
			continue;

		return true;
	}

	return false;
}

static bool TriggerGravity_ValidateNonNegFinite(const float* pVals, int nCount, const char* pszWhere)
{
	for (int i = 0; i < nCount; ++i)
	{
		if (!std::isfinite(pVals[i]) || pVals[i] < 0.0f)
		{
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] %s rejected param[%d]=%f (must be finite and non-negative)\n",
				pszWhere, i, pVals[i]);
			return false;
		}
	}
	return true;
}

//-----------------------------------------------------------------------------
// Script setters.
//-----------------------------------------------------------------------------
bool TriggerGravity_SetGravityLiftParams(void* pTrigger, const float params[10])
{
	if (!pTrigger || !params)
		return false;

	if (!TriggerGravity_ValidateNonNegFinite(params, 10, "SetGravityLiftParams"))
		return false;

	if (!TriggerGravity_ResolveLiftParams())
		return false;

	uint8_t* const pBytes = static_cast<uint8_t*>(pTrigger);
	*reinterpret_cast<float*>(pBytes + s_nLiftUpSpeedOff) = params[0];
	*reinterpret_cast<float*>(pBytes + s_nLiftUpAccelOff) = params[1];
	*reinterpret_cast<float*>(pBytes + s_nAirMoveSpeedOff) = params[2];
	*reinterpret_cast<float*>(pBytes + s_nAirMoveAccelOff) = params[3];
	*reinterpret_cast<float*>(pBytes + s_nLiftToCenterSpeedOff) = params[4];
	*reinterpret_cast<float*>(pBytes + s_nLiftToCenterAccelOff) = params[5];
	*reinterpret_cast<float*>(pBytes + s_nLiftEjectUpOff) = params[6];
	*reinterpret_cast<float*>(pBytes + s_nLiftEjectFwdOff) = params[7];
	*reinterpret_cast<float*>(pBytes + s_nLiftMaxEjectOff) = params[8];
	*reinterpret_cast<float*>(pBytes + s_nLiftMaxHoverOff) = params[9];
	MarkEntityEdictDirty(pTrigger);
	return true;
}

bool TriggerGravity_SetBlackholeParams(void* pTrigger, const float params[6])
{
	if (!pTrigger || !params)
		return false;

	if (!TriggerGravity_ValidateNonNegFinite(params, 6, "SetBlackholeParams"))
		return false;

	if (!TriggerGravity_ResolveBlackholeParams())
		return false;

	float flInner = params[5];
	const float flRadius = *reinterpret_cast<const float*>(
		static_cast<uint8_t*>(pTrigger) + TG_TRIG_OFF_RADIUS);
	if (std::isfinite(flRadius) && flRadius > 0.0f && flInner > flRadius)
		flInner = flRadius;

	uint8_t* const pBytes = static_cast<uint8_t*>(pTrigger);
	*reinterpret_cast<float*>(pBytes + s_nBhStrongAddlOff) = params[0];
	*reinterpret_cast<float*>(pBytes + s_nBhOuterPullOff) = params[1];
	*reinterpret_cast<float*>(pBytes + s_nBhInnerPullOff) = params[2];
	*reinterpret_cast<float*>(pBytes + s_nBhOuterMoveOff) = params[3];
	*reinterpret_cast<float*>(pBytes + s_nBhInnerMoveOff) = params[4];
	*reinterpret_cast<float*>(pBytes + s_nBhInnerRadiusOff) = flInner;
	MarkEntityEdictDirty(pTrigger);
	return true;
}

bool TriggerGravity_SetBlackholeIsStrongPulling(void* pTrigger, bool bStrong)
{
	if (!pTrigger)
		return false;

	if (!TriggerGravity_ResolveBlackholeStrong())
		return false;

	*reinterpret_cast<int*>(static_cast<uint8_t*>(pTrigger) + s_nBhStrongPullingOff) =
		bStrong ? 1 : 0;
	MarkEntityEdictDirty(pTrigger);
	return true;
}

//-----------------------------------------------------------------------------
// Lift force -- returns true if a lift ran; may set *pForceEject.
// ShouldIgnoreGravityAbilities is unreachable on this dedi (no player-launch
// system, no m_playerLaunch* props) and is intentionally not gated.
//-----------------------------------------------------------------------------
static bool TriggerGravity_ApplyLiftForce(void* pPlayer, void* pMoveData, void* pTrigger,
	GravityLiftState& s, bool* pForceEject)
{
	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);
	uint8_t* const pTrigBytes = static_cast<uint8_t*>(pTrigger);
	const float flNow = TriggerPass_MovementTime();
	const float flFrame = TriggerPass_FrameTime();

	if (TriggerGravity_IsPhaseShiftImmune(pPlayerBytes, flNow))
	{
		TriggerGravity_LogPhaseSkip(TG_SITE_LIFT, pPlayerBytes, flNow);
		return false;
	}

	if (TriggerGravity_ArmoredLeapPhase(pPlayerBytes) != 0)
	{
		if (TriggerGravity_ShouldLogSkip(TG_SITE_LIFT, TG_SKIP_ARMOREDLEAP))
			Warning(eDLL_T::SERVER, "[TRIG-GRAV] lift skip: armoredleap phase=%d\n",
				TriggerGravity_ArmoredLeapPhase(pPlayerBytes));
		return false;
	}

	if (!TriggerGravity_ResolveLiftParams())
	{
		if (TriggerGravity_ShouldLogSkip(TG_SITE_LIFT, TG_SKIP_PARAMS))
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] lift skip: params unresolved -- no force this touch\n");
		return false;
	}

	// lastOrigin tracks the trigger for the whole dwell (full Vector on S21).
	TriggerPass_EnsureAbsOrigin(pTrigger);
	const float* const pTrigOrigin = reinterpret_cast<const float*>(
		pTrigBytes + TG_ENT_OFF_ABS_ORIGIN);
	s.m_gravityLiftLastOrigin[0] = pTrigOrigin[0];
	s.m_gravityLiftLastOrigin[1] = pTrigOrigin[1];
	s.m_gravityLiftLastOrigin[2] = pTrigOrigin[2];

	if (flNow - s.m_gravityLiftEjectTime < TG_EJECT_DEBOUNCE)
	{
		if (TriggerGravity_ShouldLogSkip(TG_SITE_LIFT, TG_SKIP_DEBOUNCE))
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] lift skip: eject debounce since=%.3f now=%.3f\n",
				s.m_gravityLiftEjectTime, flNow);
		return false;
	}

	if (!s.m_gravityLiftActive)
	{
		if (bridge_trigger_gravity_enter_cb.GetBool())
			TriggerPass_EnterScriptCallback(pTrigger, pPlayer);
		s.m_gravityLiftActive = true;
		s.m_gravityLiftEnterTime = flNow;
		s.m_gravityLiftEjectTime = 0.0f;
		s.m_gravityLiftHoverTime = 0.0f;
		TriggerGravity_Mirror(pPlayer, s);
	}

	const float flMaxEject = *reinterpret_cast<const float*>(pTrigBytes + s_nLiftMaxEjectOff);
	const float flMaxHover = *reinterpret_cast<const float*>(pTrigBytes + s_nLiftMaxHoverOff);
	const float flUpSpeed = *reinterpret_cast<const float*>(pTrigBytes + s_nLiftUpSpeedOff);
	const float flUpAccel = *reinterpret_cast<const float*>(pTrigBytes + s_nLiftUpAccelOff);
	const float flAirSpeed = *reinterpret_cast<const float*>(pTrigBytes + s_nAirMoveSpeedOff);
	const float flAirAccel = *reinterpret_cast<const float*>(pTrigBytes + s_nAirMoveAccelOff);
	const float flToCenterSpeed = *reinterpret_cast<const float*>(pTrigBytes + s_nLiftToCenterSpeedOff);
	const float flToCenterAccel = *reinterpret_cast<const float*>(pTrigBytes + s_nLiftToCenterAccelOff);
	const float flEjectUp = *reinterpret_cast<const float*>(pTrigBytes + s_nLiftEjectUpOff);
	const float flEjectFwd = *reinterpret_cast<const float*>(pTrigBytes + s_nLiftEjectFwdOff);
	const float flAboveHeight = *reinterpret_cast<const float*>(pTrigBytes + TG_TRIG_OFF_ABOVEHEIGHT);
	const float flRadius = *reinterpret_cast<const float*>(pTrigBytes + TG_TRIG_OFF_RADIUS);

	static bool s_bLoggedDims = false;
	if (!s_bLoggedDims && bridge_trigger_gravity_diag.GetBool())
	{
		s_bLoggedDims = true;
		Warning(eDLL_T::SERVER,
			"[TRIG-GRAV] first lift touch radius=%.1f aboveHeight=%.1f\n",
			flRadius, flAboveHeight);
	}

	const float flDwell = flNow - s.m_gravityLiftEnterTime;
	float flS = 1.0f;
	if (flMaxEject > 0.0f)
	{
		flS = flDwell / flMaxEject;
		if (flS > 1.0f)
			flS = 1.0f;
	}
	s.m_gravityLiftEjectVelocity[0] = flEjectFwd * flS;
	s.m_gravityLiftEjectVelocity[1] = 0.0f;
	s.m_gravityLiftEjectVelocity[2] = flEjectUp * flS;

	TriggerPass_EnsureAbsOrigin(pPlayer);
	const float* const pPlayerOrigin = reinterpret_cast<const float*>(
		pPlayerBytes + TG_ENT_OFF_ABS_ORIGIN);

	float d[3] = {
		pTrigOrigin[0] - pPlayerOrigin[0],
		pTrigOrigin[1] - pPlayerOrigin[1],
		pTrigOrigin[2] - pPlayerOrigin[2]
	};

	float up[3];
	TriggerGravity_ReadUpAxis(pTrigBytes, up);

	float* const pVel = reinterpret_cast<float*>(
		static_cast<uint8_t*>(pMoveData) + TG_MV_OFF_VELOCITY);
	const float* const pMoveDir = reinterpret_cast<const float*>(
		static_cast<uint8_t*>(pMoveData) + TG_MV_OFF_MOVEDIR2D);

	const float flVelZ = pVel[2];
	float flHeightFrac = 0.0f;
	if (flAboveHeight > 0.0f && std::isfinite(flAboveHeight))
	{
		const float flDot = (-d[0]) * up[0] + (-d[1]) * up[1] + (-d[2]) * up[2];
		flHeightFrac = flDot / flAboveHeight;
		if (flHeightFrac < 0.0f)
			flHeightFrac = 0.0f;
	}

	// No UpAccel > 0 branch on S21 -- always integrate and clamp.
	float flTargetUp;
	if (flHeightFrac < 0.9f)
	{
		flTargetUp = flUpAccel * flFrame + flVelZ;
		if (flTargetUp < TG_MOVE_UP_THRESHOLD)
			flTargetUp = TG_MOVE_UP_THRESHOLD;
		if (flTargetUp > flUpSpeed)
			flTargetUp = flUpSpeed;
	}
	else
	{
		flTargetUp = TriggerGravity_GraphCapped(flHeightFrac, 0.9f, 1.0f, 0.5f, -0.5f);
	}

	if (flVelZ >= TG_MOVE_UP_THRESHOLD)
		s.m_gravityLiftHoverTime = 0.0f;
	else
	{
		s.m_gravityLiftHoverTime += flFrame;
		if (s.m_gravityLiftHoverTime > flMaxHover)
			*pForceEject = true;
	}

	const float flMoveDirSq = pMoveDir[0] * pMoveDir[0] + pMoveDir[1] * pMoveDir[1]
		+ pMoveDir[2] * pMoveDir[2];
	const bool bHasInput = flMoveDirSq > 0.0f;

	float steered[3] = {};
	float flAccel = flAirAccel;

	if (bHasInput)
	{
		const float flHorizSpeed = std::sqrt(pVel[0] * pVel[0] + pVel[1] * pVel[1]);
		float flSpeed = flHorizSpeed > flAirSpeed ? flHorizSpeed : flAirSpeed;
		const float flLen = std::sqrt(flMoveDirSq);
		if (flLen > 0.0f)
		{
			steered[0] = (pMoveDir[0] / flLen) * flSpeed;
			steered[1] = (pMoveDir[1] / flLen) * flSpeed;
			steered[2] = (pMoveDir[2] / flLen) * flSpeed;
		}
	}
	else
	{
		const float flDist = std::sqrt(d[0] * d[0] + d[1] * d[1]);
		const float flSpeed = TriggerGravity_GraphCapped(flDist, TG_TOCENTER_INNER, flRadius,
			flToCenterSpeed * 0.5f, flToCenterSpeed);
		if (flDist > 0.0f)
		{
			steered[0] = (d[0] / flDist) * flSpeed;
			steered[1] = (d[1] / flDist) * flSpeed;
			steered[2] = 0.0f;
		}
		flAccel = flToCenterAccel;
	}

	if (flAccel > 0.0f)
	{
		float base[3] = {};
		if (s.m_gravityLiftEnterTime != flNow)
		{
			base[0] = pVel[0];
			base[1] = pVel[1];
		}

		const float flVelHorizSq = pVel[0] * pVel[0] + pVel[1] * pVel[1];
		const float flSteeredSq = steered[0] * steered[0] + steered[1] * steered[1]
			+ steered[2] * steered[2];
		const float flCap = flVelHorizSq > flSteeredSq ? flVelHorizSq : flSteeredSq;

		const float flSteeredLen = std::sqrt(flSteeredSq);
		float v[3] = { base[0], base[1], base[2] };
		if (flSteeredLen > 0.0f)
		{
			const float flStep = flFrame * flAccel;
			v[0] += (steered[0] / flSteeredLen) * flStep;
			v[1] += (steered[1] / flSteeredLen) * flStep;
			v[2] += (steered[2] / flSteeredLen) * flStep;
		}

		const float flVSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
		if (flVSq > flCap && flVSq > 0.0f)
		{
			const float flScale = std::sqrt(flCap / flVSq);
			v[0] *= flScale;
			v[1] *= flScale;
			v[2] *= flScale;
		}
		steered[0] = v[0];
		steered[1] = v[1];
		steered[2] = v[2];
	}

	float newVel[3] = {
		up[0] * flTargetUp + steered[0],
		up[1] * flTargetUp + steered[1],
		up[2] * flTargetUp + steered[2]
	};

	if (!TriggerGravity_IsFinite3(newVel))
	{
		if (bridge_trigger_gravity_diag.GetBool() && s_nDiagCount < TG_DIAG_CAP)
		{
			++s_nDiagCount;
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] lift non-finite vel discarded\n");
		}
		return true;
	}

	pVel[0] = newVel[0];
	pVel[1] = newVel[1];
	pVel[2] = newVel[2];
	TriggerPass_SetGroundEntityNull(pPlayer);

	if (bridge_trigger_gravity_diag.GetBool() && s_nDiagCount < TG_DIAG_CAP)
	{
		++s_nDiagCount;
		Warning(eDLL_T::SERVER,
			"[TRIG-GRAV] lift t=%.3f branch=%s f=%.2f targetUp=%.1f vel=(%.1f,%.1f,%.1f)\n",
			flNow, bHasInput ? "input" : "no-input", flHeightFrac, flTargetUp,
			pVel[0], pVel[1], pVel[2]);
	}

	return true;
}

//-----------------------------------------------------------------------------
// Eject -- S21 direction priority: flat view, then travel delta > 40u, then move.
// No eject debounce on S21 (the 2.0s debounce is the lift entry gate only).
//-----------------------------------------------------------------------------
static void TriggerGravity_ApplyLiftEject(void* pPlayer, void* pMoveData, GravityLiftState& s)
{
	if (!s.m_gravityLiftActive)
		return;

	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);
	const float flNow = TriggerPass_MovementTime();

	s.m_gravityLiftActive = false;
	s.m_gravityLiftEjectTime = flNow;

	TriggerPass_EnsureAbsOrigin(pPlayer);
	const float* const pOrigin = reinterpret_cast<const float*>(
		pPlayerBytes + TG_ENT_OFF_ABS_ORIGIN);
	*reinterpret_cast<float*>(pPlayerBytes + TG_PLAYER_OFF_FLOORHEIGHT) = pOrigin[2];

	TriggerGravity_Mirror(pPlayer, s);

	if (TriggerGravity_IsPhaseShiftImmune(pPlayerBytes, flNow))
		return;

	float dir[3] = {};

	if (CPlayer__EyeAngles)
	{
		QAngle eyeAngles;
		CPlayer__EyeAngles(reinterpret_cast<CPlayer*>(pPlayer), &eyeAngles);
		Vector3D viewDir;
		AngleVectors(eyeAngles, &viewDir);
		const float flFlatSq = viewDir.x * viewDir.x + viewDir.y * viewDir.y;
		if (flFlatSq > 0.0f)
		{
			const float flInv = 1.0f / std::sqrt(flFlatSq);
			dir[0] = viewDir.x * flInv;
			dir[1] = viewDir.y * flInv;
			dir[2] = 0.0f;
		}
	}

	TriggerPass_EnsureAbsOrigin(pPlayer);
	const float flTravelX = pOrigin[0] - s.m_gravityLiftLastOrigin[0];
	const float flTravelY = pOrigin[1] - s.m_gravityLiftLastOrigin[1];
	const float flTravelSq = flTravelX * flTravelX + flTravelY * flTravelY;
	if (flTravelSq > TG_EJECT_TRAVEL_THRESH_SQ)
	{
		const float flInv = 1.0f / std::sqrt(flTravelSq);
		dir[0] = flTravelX * flInv;
		dir[1] = flTravelY * flInv;
		dir[2] = 0.0f;
	}

	if (dir[0] == 0.0f && dir[1] == 0.0f && dir[2] == 0.0f)
	{
		const float* const pMoveDir = reinterpret_cast<const float*>(
			static_cast<uint8_t*>(pMoveData) + TG_MV_OFF_MOVEDIR2D);
		dir[0] = pMoveDir[0];
		dir[1] = pMoveDir[1];
		dir[2] = pMoveDir[2];
	}

	const float flEjF = s.m_gravityLiftEjectVelocity[0];
	const float flEjU = s.m_gravityLiftEjectVelocity[2];
	float* const pVel = reinterpret_cast<float*>(
		static_cast<uint8_t*>(pMoveData) + TG_MV_OFF_VELOCITY);

	const float newVel[3] = {
		dir[0] * flEjF,
		dir[1] * flEjF,
		dir[2] * flEjF + flEjU
	};

	if (TriggerGravity_IsFinite3(newVel))
	{
		pVel[0] = newVel[0];
		pVel[1] = newVel[1];
		pVel[2] = newVel[2];
	}
	else if (bridge_trigger_gravity_diag.GetBool() && s_nDiagCount < TG_DIAG_CAP)
	{
		++s_nDiagCount;
		Warning(eDLL_T::SERVER, "[TRIG-GRAV] eject non-finite vel discarded\n");
	}

	s.m_gravityLiftLastOrigin[0] = 0.0f;
	s.m_gravityLiftLastOrigin[1] = 0.0f;
	s.m_gravityLiftLastOrigin[2] = 0.0f;

	if (bridge_trigger_gravity_diag.GetBool() && s_nDiagCount < TG_DIAG_CAP)
	{
		++s_nDiagCount;
		Warning(eDLL_T::SERVER,
			"[TRIG-GRAV] eject t=%.3f dir=(%.2f,%.2f,%.2f) vel=(%.1f,%.1f,%.1f)\n",
			flNow, dir[0], dir[1], dir[2], pVel[0], pVel[1], pVel[2]);
	}
}

//-----------------------------------------------------------------------------
// Blackhole force.
// Strong-pull bonus applies to BOTH pull speeds; the final clamp re-reads the
// un-bonused m_blackholeInnerPullSpeed.
//-----------------------------------------------------------------------------
static bool TriggerGravity_ApplyBlackholeForce(void* pPlayer, void* pMoveData, void* pTrigger,
	GravityLiftState& s)
{
	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);
	uint8_t* const pTrigBytes = static_cast<uint8_t*>(pTrigger);
	const float flNow = TriggerPass_MovementTime();

	if (!TriggerGravity_ResolveBlackholeParams() || !TriggerGravity_ResolveBlackholeStrong())
	{
		if (TriggerGravity_ShouldLogSkip(TG_SITE_BLACKHOLE, TG_SKIP_PARAMS))
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] blackhole skip: params unresolved -- never arms, never pulls\n");
		return false;
	}

	TriggerGravity_ResolvePlayerExtras();

	if (!s.m_blackholeActive)
	{
		if (bridge_trigger_gravity_enter_cb.GetBool())
			TriggerPass_EnterScriptCallback(pTrigger, pPlayer);
		s.m_blackholeActive = true;
		TriggerGravity_Mirror(pPlayer, s);
	}

	TriggerPass_EnsureAbsOrigin(pPlayer);
	TriggerPass_EnsureAbsOrigin(pTrigger);

	const float* const pTrigOrigin = reinterpret_cast<const float*>(
		pTrigBytes + TG_ENT_OFF_ABS_ORIGIN);
	const float* const pPlayerOrigin = reinterpret_cast<const float*>(
		pPlayerBytes + TG_ENT_OFF_ABS_ORIGIN);

	float d[3] = {
		pTrigOrigin[0] - pPlayerOrigin[0],
		pTrigOrigin[1] - pPlayerOrigin[1],
		pTrigOrigin[2] - pPlayerOrigin[2]
	};
	const float flDist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);

	const float flInnerRadius = *reinterpret_cast<const float*>(
		pTrigBytes + s_nBhInnerRadiusOff);
	if (flDist > flInnerRadius && !TriggerGravity_CanBlackholeSeePlayer(pPlayer, pTrigOrigin))
	{
		// Geometry, cover and approach angle all land here. The probe base is
		// triggerOrigin.z + 64 with candidates 32 units apart, so a trigger that
		// spawns low or partly embedded starts several candidates solid.
		if (TriggerGravity_ShouldLogSkip(TG_SITE_BLACKHOLE, TG_SKIP_LOS))
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] blackhole skip: los dist=%.1f innerR=%.1f origin=(%.1f,%.1f,%.1f)\n",
				flDist, flInnerRadius, pTrigOrigin[0], pTrigOrigin[1], pTrigOrigin[2]);
		return false;
	}

	{
		const float flDebounce = PlayerExtend_GetF32(pPlayerBytes,
			offsetof(PlayerExtendWire, m_jumpPadDebounceExpireTime));
		if (flDebounce >= flNow)
		{
			// Only the jump pad arms this, for bridge_jumppad_debounce_time
			// seconds -- the cannon and the lift do not.
			if (TriggerGravity_ShouldLogSkip(TG_SITE_BLACKHOLE, TG_SKIP_DEBOUNCE))
				Warning(eDLL_T::SERVER,
					"[TRIG-GRAV] blackhole skip: jumppad debounce until=%.3f now=%.3f\n",
					flDebounce, flNow);
			return false;
		}
	}

	if (TriggerGravity_IsPhaseShiftImmune(pPlayerBytes, flNow))
	{
		TriggerGravity_LogPhaseSkip(TG_SITE_BLACKHOLE, pPlayerBytes, flNow);
		return false;
	}

	const char moveType = *reinterpret_cast<const char*>(pPlayerBytes + TG_ENT_OFF_MOVETYPE);
	if (static_cast<unsigned char>(moveType) == TG_MOVETYPE_TRAVERSE)
	{
		if (TriggerGravity_ShouldLogSkip(TG_SITE_BLACKHOLE, TG_SKIP_MOVETYPE))
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] blackhole skip: movetype=%d (traverse)\n",
				static_cast<int>(static_cast<unsigned char>(moveType)));
		return false;
	}

	if (*reinterpret_cast<const uint8_t*>(pPlayerBytes + TG_PLAYER_OFF_WALLHANGING))
	{
		if (TriggerGravity_ShouldLogSkip(TG_SITE_BLACKHOLE, TG_SKIP_WALLHANG))
			Warning(eDLL_T::SERVER, "[TRIG-GRAV] blackhole skip: wallhang\n");
		return false;
	}

	if (TriggerGravity_IsWallRunning(pPlayerBytes))
	{
		if (TriggerGravity_ShouldLogSkip(TG_SITE_BLACKHOLE, TG_SKIP_WALLRUN))
			Warning(eDLL_T::SERVER, "[TRIG-GRAV] blackhole skip: wallrun upZ=%.4f\n",
				*reinterpret_cast<const float*>(pPlayerBytes + TG_PLAYER_OFF_UPDIR + 8));
		return false;
	}

	if (TriggerGravity_ArmoredLeapPhase(pPlayerBytes) != 0)
	{
		if (TriggerGravity_ShouldLogSkip(TG_SITE_BLACKHOLE, TG_SKIP_ARMOREDLEAP))
			Warning(eDLL_T::SERVER, "[TRIG-GRAV] blackhole skip: armoredleap phase=%d\n",
				TriggerGravity_ArmoredLeapPhase(pPlayerBytes));
		return false;
	}

	const float flDenom = flDist > TG_SQRT_FLT_MIN ? flDist : TG_SQRT_FLT_MIN;
	const float n[3] = { d[0] / flDenom, d[1] / flDenom, d[2] / flDenom };

	const int nPlayerTeam = *reinterpret_cast<const int*>(pPlayerBytes + TG_ENT_OFF_TEAMNUM);
	const int nTrigTeam = *reinterpret_cast<const int*>(pTrigBytes + TG_ENT_OFF_TEAMNUM);
	const bool bAlly = AllianceCompat_IsFriendlyTeam(nPlayerTeam, nTrigTeam);

	const bool bGrappleActive = *reinterpret_cast<const uint8_t*>(
		pPlayerBytes + TG_PLAYER_OFF_GRAPPLEACTIVE) != 0;
	const bool bGrapplePulling = *reinterpret_cast<const uint8_t*>(
		pPlayerBytes + TG_PLAYER_OFF_GRAPPLEPULLING) != 0;
	const bool bPulling = bGrappleActive && bGrapplePulling;

	const bool bStrong = *reinterpret_cast<const int*>(
		pTrigBytes + s_nBhStrongPullingOff) != 0;

	float flInnerPull = *reinterpret_cast<const float*>(pTrigBytes + s_nBhInnerPullOff);
	float flOuterPull = *reinterpret_cast<const float*>(pTrigBytes + s_nBhOuterPullOff);
	// Bonus on BOTH pull speeds when strong and not ally.
	if (bStrong && !bAlly)
	{
		const float flAddl = *reinterpret_cast<const float*>(pTrigBytes + s_nBhStrongAddlOff);
		flInnerPull += flAddl;
		flOuterPull += flAddl;
	}

	float* const pVel = reinterpret_cast<float*>(
		static_cast<uint8_t*>(pMoveData) + TG_MV_OFF_VELOCITY);
	const float* const pMoveDir = reinterpret_cast<const float*>(
		static_cast<uint8_t*>(pMoveData) + TG_MV_OFF_MOVEDIR2D);

	const float flMoveDirSq = pMoveDir[0] * pMoveDir[0] + pMoveDir[1] * pMoveDir[1]
		+ pMoveDir[2] * pMoveDir[2];
	const bool bHasMoveInput = flMoveDirSq > 0.0f;

	const float flRadius = *reinterpret_cast<const float*>(pTrigBytes + TG_TRIG_OFF_RADIUS);
	const float flInnerMove = *reinterpret_cast<const float*>(pTrigBytes + s_nBhInnerMoveOff);
	const float flOuterMove = *reinterpret_cast<const float*>(pTrigBytes + s_nBhOuterMoveOff);

	// Cap branch: moving away with input (or grapple-pulling); else pull branch.
	const bool bCapBranch = bPulling || (bHasMoveInput && (!bStrong || bAlly));

	static bool s_bLoggedBhDims = false;
	if (!s_bLoggedBhDims && bridge_trigger_gravity_diag.GetBool())
	{
		s_bLoggedBhDims = true;
		Warning(eDLL_T::SERVER,
			"[TRIG-GRAV] first blackhole touch radius=%.1f aboveHeight=%.1f innerR=%.1f\n",
			flRadius,
			*reinterpret_cast<const float*>(pTrigBytes + TG_TRIG_OFF_ABOVEHEIGHT),
			flInnerRadius);
	}

	float newVel[3] = { pVel[0], pVel[1], pVel[2] };

	if (bCapBranch)
	{
		float flMs = TriggerGravity_GraphCapped(flDist, 0.0f, flRadius, flInnerMove, flOuterMove);
		if (bAlly)
			flMs *= 2.0f;
		else if (bPulling)
			flMs *= TG_GRAPPLE_PULL_MOVE_MULT;

		const float flSpeed = std::sqrt(
			newVel[0] * newVel[0] + newVel[1] * newVel[1] + newVel[2] * newVel[2]);
		if (flSpeed > 0.0f)
		{
			const float flAwayDot =
				(newVel[0] / flSpeed) * n[0] +
				(newVel[1] / flSpeed) * n[1] +
				(newVel[2] / flSpeed) * n[2];
			const float flHoriz = std::sqrt(newVel[0] * newVel[0] + newVel[1] * newVel[1]);
			if (flAwayDot < 0.0f && flHoriz > flMs && flHoriz > 0.0f)
			{
				const float flScale = flMs / flHoriz;
				newVel[0] *= flScale;
				newVel[1] *= flScale;
			}
		}
	}
	else
	{
		const float flOldVelZ = newVel[2];
		float flPull = 0.0f;
		if (flDist >= flInnerRadius)
			flPull = TriggerGravity_GraphCapped(flDist, flInnerRadius, flRadius,
				flInnerPull, flOuterPull);

		newVel[0] += n[0] * flPull;
		newVel[1] += n[1] * flPull;
		newVel[2] += n[2] * flPull;

		// Final clamp re-reads the un-bonused inner pull speed.
		float flClamp = *reinterpret_cast<const float*>(pTrigBytes + s_nBhInnerPullOff);
		if (bAlly)
			flClamp *= 0.5f;

		const float flSpeed = std::sqrt(
			newVel[0] * newVel[0] + newVel[1] * newVel[1] + newVel[2] * newVel[2]);
		if (flSpeed > flClamp && flSpeed > 0.0f)
		{
			const float flScale = flClamp / flSpeed;
			newVel[0] *= flScale;
			newVel[1] *= flScale;
			newVel[2] *= flScale;
		}

		if (flOldVelZ <= 0.0f)
			newVel[2] = flOldVelZ;
	}

	if (!TriggerGravity_IsFinite3(newVel))
	{
		if (bridge_trigger_gravity_diag.GetBool() && s_nDiagCount < TG_DIAG_CAP)
		{
			++s_nDiagCount;
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] blackhole non-finite vel -- left untouched this tick\n");
		}
		return true;
	}

	pVel[0] = newVel[0];
	pVel[1] = newVel[1];
	pVel[2] = newVel[2];

	if (bridge_trigger_gravity_diag.GetBool() && s_nDiagCount < TG_DIAG_CAP)
	{
		++s_nDiagCount;
		Warning(eDLL_T::SERVER,
			"[TRIG-GRAV] blackhole t=%.3f branch=%s dist=%.1f vel=(%.1f,%.1f,%.1f) "
			"strong=%d ally=%d\n",
			flNow, bCapBranch ? "cap" : "pull", flDist,
			pVel[0], pVel[1], pVel[2], bStrong ? 1 : 0, bAlly ? 1 : 0);
	}

	return true;
}

//-----------------------------------------------------------------------------
// Per-tick pass -- walks the same touching-Heavy list as the cannon.
//-----------------------------------------------------------------------------
void TriggerGravity_ApplyPass(void* pCtx)
{
	if (!pCtx || !g_serverEntityList)
		return;

	if (!bridge_trigger_gravity_lift.GetBool() && !bridge_trigger_blackhole.GetBool())
		return;

	void* const pPlayer = *reinterpret_cast<void**>(
		static_cast<uint8_t*>(pCtx) + TG_CTX_OFF_PLAYER);
	void* const pMoveData = *reinterpret_cast<void**>(
		static_cast<uint8_t*>(pCtx) + TG_CTX_OFF_MOVEDATA);
	if (!pPlayer || !pMoveData)
		return;

	TriggerGravity_ResolvePlayerExtras();

	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);

	int64_t nCount = *reinterpret_cast<int64_t*>(pPlayerBytes + TG_PLAYER_OFF_TOUCHED_COUNT);
	if (nCount <= 0)
		nCount = 0;
	if (nCount > TG_TOUCHED_CAP)
		nCount = TG_TOUCHED_CAP;

	GravityLiftState& s = s_gravityMap[pPlayer];

	bool bAnyLiftRan = false;
	bool bAnyBlackholeRan = false;
	bool bForceEject = false;

	for (int i = 0; i < static_cast<int>(nCount); ++i)
	{
		const uint32_t rawHandle = *reinterpret_cast<uint32_t*>(
			pPlayerBytes + TG_PLAYER_OFF_TOUCHED_TRIG + 4 * i);
		if (rawHandle == 0xFFFFFFFFu)
			continue;

		const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
		void* const pTrigger = g_serverEntityList->LookupEntity(handle);
		if (!pTrigger)
			continue;

		const int nType = *reinterpret_cast<const int*>(
			static_cast<const uint8_t*>(pTrigger) + TG_TRIG_OFF_TRIGGERTYPE);

		if (bridge_trigger_gravity_walk_log.GetBool() && s_nWalkLogCount < TG_WALK_LOG_CAP)
		{
			++s_nWalkLogCount;
			Warning(eDLL_T::SERVER,
				"[TRIG-GRAV] walk slot=%d trigger=%p type=%d\n", i, pTrigger, nType);
		}

		// Dispatch is a switch on the int, not a bitmask.
		switch (nType)
		{
		case TG_TRIGGER_TYPE_GRAVITY_LIFT:
			// Presence of a type-4 counts as "ran" for the post-loop eject, even
			// when an inner guard bails -- the client flags it before the call.
			if (bridge_trigger_gravity_lift.GetBool())
			{
				TriggerGravity_ApplyLiftForce(pPlayer, pMoveData, pTrigger, s, &bForceEject);
				bAnyLiftRan = true;
			}
			else if (TriggerGravity_ShouldLogSkip(TG_SITE_LIFT, TG_SKIP_LEVER))
				Warning(eDLL_T::SERVER,
					"[TRIG-GRAV] lift skip: bridge_trigger_gravity_lift is 0\n");
			break;

		case TG_TRIGGER_TYPE_BLACKHOLE:
			// Immune players must fall through to the post-loop clear.
			if (!bridge_trigger_blackhole.GetBool())
			{
				if (TriggerGravity_ShouldLogSkip(TG_SITE_BLACKHOLE, TG_SKIP_LEVER))
					Warning(eDLL_T::SERVER,
						"[TRIG-GRAV] blackhole skip: bridge_trigger_blackhole is 0\n");
			}
			else if (TriggerGravity_IsPhaseShiftImmune(pPlayerBytes, TriggerPass_MovementTime()))
			{
				// The body is never entered, so this is the only line that can
				// report the gate the client applies at the dispatch case.
				TriggerGravity_LogPhaseSkip(TG_SITE_BLACKHOLE, pPlayerBytes,
					TriggerPass_MovementTime());
			}
			else
			{
				TriggerGravity_ApplyBlackholeForce(pPlayer, pMoveData, pTrigger, s);
				bAnyBlackholeRan = true;
			}
			break;

		default:
			break;
		}
	}

	if (!bAnyLiftRan || bForceEject)
		TriggerGravity_ApplyLiftEject(pPlayer, pMoveData, s);

	if (!bAnyBlackholeRan && s.m_blackholeActive)
	{
		s.m_blackholeActive = false;
		TriggerGravity_Mirror(pPlayer, s);
	}
}

//-----------------------------------------------------------------------------
// IDetour -- visibility only.
//-----------------------------------------------------------------------------
void VTriggerGravityBridge::GetAdr(void) const
{
	LogFunAdr("TriggerGravity_ApplyPass", reinterpret_cast<void*>(&TriggerGravity_ApplyPass));
	LogFunAdr("TriggerGravity_GetWire", reinterpret_cast<void*>(&TriggerGravity_GetWire));
	LogFunAdr("UTIL_TraceLine_IgnoreEntity_WithDetail", v_UTIL_TraceLine_IgnoreEntity_WithDetail);
}

void VTriggerGravityBridge::GetFun(void) const
{
	// The lea displacement is the CTraceFilterSimple vtable load -- only those
	// four bytes separate the server half from its byte-identical client twin;
	// leave them literal so the pattern matches exactly once.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 81 EC D0 00 00 00 48 8D 05 CC 3D 8B 00 4C 89 4C 24 40 48 89 44 24 30 "
		"48 8B DA 8B 84 24 00 01 00 00 33 D2 89 44 24 50 44 8B CA")
		.GetPtr(v_UTIL_TraceLine_IgnoreEntity_WithDetail);
}

