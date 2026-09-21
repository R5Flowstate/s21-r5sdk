//=============================================================================//
//
// Purpose: Server-side damage-blocked-by-obstacle gate. A hit outside the
// victim's collision capsule whose line to the victim's vertical axis is
// blocked by geometry is ignored: a bolt touch is dropped (no damage, no
// stick, the bolt keeps flying) and a melee attack trace reports no victim.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "mathlib/vector.h"
#include "mathlib/mathlib.h"
#include "public/cmodel.h"
#include "public/bspflags.h"
#include "public/gametrace.h"
#include "public/engine/IEngineTrace.h"
#include "game/shared/collisionproperty.h"
#include "game/shared/util_shared.h"
#include "game/server/baseentity.h"
#include "engine/enginetrace.h"
#include "player_damage_obstacle.h"

static ConVar bridge_obstacle_gate("bridge_obstacle_gate", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Drop bolt touches on players whose hit point is outside the victim's "
	"capsule and occluded from the victim's axis (cover-edge graze parity).");

static ConVar bridge_obstacle_tap("bridge_obstacle_tap", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log one [OBSTACLE] line per blocked bolt touch or melee hit.");

typedef const float* (__fastcall* PFN_CBaseEntity_GetAbsOrigin)(void* pEntity);
static PFN_CBaseEntity_GetAbsOrigin v_ObstacleGetAbsOrigin = nullptr;

typedef unsigned char (__fastcall* PFN_CBaseEntity_IsPlayer)(__int64 entity);
static constexpr int VTBL_ISPLAYER_SLOT = 93; // +744; CPlayer answers 1, CBaseEntity 0

// S21 probe contents: solid, glass, slime, water, moveable, bullet clip,
// monsters, debris.
static constexpr unsigned int kObstacleMask =
	CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_SLIME | CONTENTS_WATER |
	CONTENTS_MOVEABLE | CONTENTS_BULLETCLIP | CONTENTS_MONSTER | CONTENTS_DEBRIS;
static_assert(kObstacleMask == 0x6404033, "obstacle probe mask must match the S21 client");
static_assert(offsetof(CGameTrace, endpos) == 16, "engine trace endpos");
static_assert(offsetof(CGameTrace, fraction) == 48, "engine trace fraction");
static_assert(offsetof(CGameTrace, hit_entity) == 96, "engine trace hit entity");

// S3 weapon collision group (S21 inserted a group above debris, shifting the
// same group to 14 there; the engine resolves group behaviour natively).
static constexpr int kCollisionGroupWeapon = 13;

static long s_nBlocked = 0;
static long s_nMeleeBlocked = 0;
static bool s_bLiveWarned = false;
static bool s_bMeleeLiveWarned = false;
static bool s_bTurretWarned = false;
static bool s_bHelperWarned = false;

//-----------------------------------------------------------------------------
// Purpose: player test through the IsPlayer virtual (slot 93)
//-----------------------------------------------------------------------------
bool Obstacle_IsPlayer(const CBaseEntity* const pEntity)
{
	if (!pEntity)
		return false;

	void* const* const pVtbl = *reinterpret_cast<void* const* const*>(pEntity);

	if (!pVtbl)
		return false;

	const PFN_CBaseEntity_IsPlayer fn =
		reinterpret_cast<PFN_CBaseEntity_IsPlayer>(pVtbl[VTBL_ISPLAYER_SLOT]);

	if (!fn)
		return false;

	return fn(reinterpret_cast<__int64>(const_cast<CBaseEntity*>(pEntity))) != 0;
}

//-----------------------------------------------------------------------------
// Purpose: S21 capsule test over the collision OBB (square centred bounds)
//-----------------------------------------------------------------------------
static bool Obstacle_PointInBoundsCapsule(const Vector3D& point,
	const Vector3D& origin, const Vector3D& mins, const Vector3D& maxs)
{
	const float flRadius = maxs.x;
	const float flDz = point.z - origin.z;

	if (flDz < mins.z || flDz > maxs.z)
		return false;

	const float flR2 = flRadius * flRadius;
	const float flDx = point.x - origin.x;
	const float flDy = point.y - origin.y;

	if (flDz < mins.z + flRadius)
	{
		const float flDcz = point.z - (origin.z + mins.z + flRadius);
		return flR2 > (flDx * flDx + flDy * flDy + flDcz * flDcz);
	}

	if (flDz > maxs.z - flRadius)
	{
		const float flDcz = point.z - (origin.z + maxs.z - flRadius);
		return flR2 > (flDx * flDx + flDy * flDy + flDcz * flDcz);
	}

	return flR2 > (flDx * flDx + flDy * flDy);
}

//-----------------------------------------------------------------------------
// Purpose: closest point on the victim's vertical axis segment to the hit
//-----------------------------------------------------------------------------
static Vector3D Obstacle_ClosestPointOnAxis(const Vector3D& hitPos,
	const Vector3D& bot, const Vector3D& top)
{
	const Vector3D axis = top - bot;
	const float flLen2 = axis.LengthSqr();

	if (flLen2 <= 0.0f)
		return bot;

	float flT = (hitPos - bot).Dot(axis) / flLen2;
	flT = fmaxf(0.0f, fminf(1.0f, flT));

	return bot + axis * flT;
}

static bool Obstacle_TraceReady(void)
{
	if (v_ObstacleGetAbsOrigin && g_pEngineTraceServer && v_TraceFilter_ShouldHitEntity)
		return true;

	if (!s_bHelperWarned)
	{
		s_bHelperWarned = true;
		Warning(eDLL_T::SERVER,
			"[OBSTACLE] trace helpers unresolved -- gate disabled, touches pass through\n");
	}

	return false;
}

static const char* Obstacle_BlockerInfo(CBaseEntity* const pBlocker)
{
	if (!pBlocker)
		return "world";

	if (!v_UTIL_GetEntityScriptInfo)
		return "(unresolved)";

	return UTIL_GetEntityScriptInfo(pBlocker);
}

static bool Obstacle_InfoMentionsTurret(const char* const pszInfo)
{
	if (!pszInfo)
		return false;

	for (const char* p = pszInfo; *p; ++p)
	{
		if (_strnicmp(p, "turret", 6) == 0)
			return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: S21 damage-blocked-by-obstacle rule. True = drop the touch.
//-----------------------------------------------------------------------------
static bool Obstacle_BlockedByCover(CBaseEntity* const pPlayer,
	const Vector3D& hitPos, Vector3D& outClosest, trace_t& outProbe)
{
	if (!pPlayer || !hitPos.IsValid())
		return false;

	const CCollisionProperty* const pColl = pPlayer->CollisionProp();

	if (!pColl)
		return false;

	if (!Obstacle_TraceReady())
		return false;

	const float* const pOrigin = v_ObstacleGetAbsOrigin(pPlayer);

	if (!pOrigin)
		return false;

	const Vector3D origin(pOrigin[0], pOrigin[1], pOrigin[2]);

	if (!origin.IsValid())
		return false;

	const Vector3D& mins = pColl->OBBMins();
	const Vector3D& maxs = pColl->OBBMaxs();

	if (!mins.IsValid() || !maxs.IsValid())
		return false;

	if (Obstacle_PointInBoundsCapsule(hitPos, origin, mins, maxs))
		return false;

	const Vector3D bot(origin.x, origin.y, origin.z + mins.z);
	const Vector3D top(origin.x, origin.y, origin.z + maxs.z);
	const Vector3D closest = Obstacle_ClosestPointOnAxis(hitPos, bot, top);

	Ray_t ray(hitPos, closest);
	CTraceFilterSimple filter(static_cast<const IHandleEntity*>(pPlayer), kCollisionGroupWeapon);
	trace_t probe;

	// Fail open: an unwritten result reads as fraction 1 / clear, so a
	// failed trace falls through to legacy damage, never a silent drop.
	memset(&probe, 0, sizeof(probe));
	probe.fraction = 1.0f;
	probe.endpos = closest;

	g_pEngineTraceServer->TraceRayFiltered(ray, kObstacleMask, &filter, &probe);

	outClosest = closest;
	outProbe = probe;

	if (probe.fraction >= 1.0f && !probe.allsolid && !probe.startsolid)
		return false;

	// No turret exemption on this engine: there is no player turret handle to
	// compare against, so a turret-class blocker counts as cover. Say so once.
	if (!s_bTurretWarned && Obstacle_InfoMentionsTurret(Obstacle_BlockerInfo(probe.hit_entity)))
	{
		s_bTurretWarned = true;
		Warning(eDLL_T::SERVER,
			"[OBSTACLE] blocker is a turret class; no turret exemption exists here, treating as cover\n");
	}

	return true;
}

static __int64 __fastcall Hook_CCrossbowBolt_BoltTouch(__int64 bolt,
	CBaseEntity* pTouched, trace_t* pTr)
{
	if (bridge_obstacle_gate.GetBool() && pTouched && pTr && Obstacle_IsPlayer(pTouched))
	{
		Vector3D closest(0.0f, 0.0f, 0.0f);
		trace_t probe;
		memset(&probe, 0, sizeof(probe));

		if (Obstacle_BlockedByCover(pTouched, pTr->endpos, closest, probe))
		{
			++s_nBlocked;

			CBaseEntity* const pBolt = reinterpret_cast<CBaseEntity*>(bolt);
			const int nBoltEdict = pBolt ? static_cast<int>(pBolt->GetEdict()) : -1;
			const int nVictimEdict = static_cast<int>(pTouched->GetEdict());
			const char* const pszBlocker = Obstacle_BlockerInfo(probe.hit_entity);

			if (!s_bLiveWarned)
			{
				s_bLiveWarned = true;
				Warning(eDLL_T::SERVER,
					"[OBSTACLE] gate live: first blocked bolt touch dropped "
					"(bolt=%d victim=%d frac=%.3f blocker='%s' total=%ld)\n",
					nBoltEdict, nVictimEdict, probe.fraction, pszBlocker, s_nBlocked);
			}

			if (bridge_obstacle_tap.GetBool())
			{
				Msg(eDLL_T::SERVER,
					"[OBSTACLE] bolt=%d victim=%d hit=(%.1f %.1f %.1f) "
					"closest=(%.1f %.1f %.1f) frac=%.3f blocker='%s' total=%ld\n",
					nBoltEdict, nVictimEdict,
					pTr->endpos.x, pTr->endpos.y, pTr->endpos.z,
					closest.x, closest.y, closest.z,
					probe.fraction, pszBlocker, s_nBlocked);
			}

			return 0;
		}
	}

	return v_CCrossbowBolt_BoltTouch(bolt, pTouched, pTr);
}

bool Player_IsDamageBlockedByObstacle(CBaseEntity* const pPlayer, const Vector3D& hitPos)
{
	Vector3D closest(0.0f, 0.0f, 0.0f);
	trace_t probe;
	memset(&probe, 0, sizeof(probe));

	return Obstacle_BlockedByCover(pPlayer, hitPos, closest, probe);
}

//-----------------------------------------------------------------------------
// Purpose: melee attack traces report no victim when the hit is occluded, the
// way the S21 attack-trace native nulls it before building the result table.
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_PlayerMelee_AttackTraces(__int64 player, float* pPos, float* pDir,
	__int64 a4, void* pFilter, trace_t* pTr)
{
	const __int64 result = v_PlayerMelee_AttackTraces(player, pPos, pDir, a4, pFilter, pTr);

	if (!bridge_obstacle_gate.GetBool() || !pTr)
		return result;

	CBaseEntity* const pVictim = pTr->hit_entity;

	if (!pVictim || !Obstacle_IsPlayer(pVictim))
		return result;

	Vector3D closest(0.0f, 0.0f, 0.0f);
	trace_t probe;
	memset(&probe, 0, sizeof(probe));

	if (!Obstacle_BlockedByCover(pVictim, pTr->endpos, closest, probe))
		return result;

	pTr->hit_entity = nullptr;
	++s_nMeleeBlocked;

	if (!s_bMeleeLiveWarned)
	{
		s_bMeleeLiveWarned = true;
		Warning(eDLL_T::SERVER,
			"[OBSTACLE] melee gate live: first blocked melee victim dropped "
			"(victim=%d frac=%.3f blocker='%s')\n",
			static_cast<int>(pVictim->GetEdict()), probe.fraction,
			Obstacle_BlockerInfo(probe.hit_entity));
	}

	if (bridge_obstacle_tap.GetBool())
	{
		Msg(eDLL_T::SERVER,
			"[OBSTACLE] melee victim=%d hit=(%.1f %.1f %.1f) closest=(%.1f %.1f %.1f) "
			"frac=%.3f blocker='%s' total=%ld\n",
			static_cast<int>(pVictim->GetEdict()),
			pTr->endpos.x, pTr->endpos.y, pTr->endpos.z,
			closest.x, closest.y, closest.z,
			probe.fraction, Obstacle_BlockerInfo(probe.hit_entity), s_nMeleeBlocked);
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////////
void VPlayerDamageObstacle::GetFun(void) const
{
	// Server CCrossbowBolt::BoltTouch. Prologue
	// plus the m_boltPassEntities-count displacement; the client twin opens
	// with register pushes, so this resolves exactly once.
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 48 8D 6C 24 80 48 81 EC 80 01 00 00 80 B9 88 23 00 00 00")
		.GetPtr(v_CCrossbowBolt_BoltTouch);

	if (!v_CCrossbowBolt_BoltTouch)
		Warning(eDLL_T::SERVER,
			"[OBSTACLE] CCrossbowBolt::BoltTouch pattern unresolved -- gate disabled\n");

	// Server melee attack-trace helper, the only callee of the
	// PlayerMelee_AttackTrace native. The client twin shares the prologue; the
	// 'cmp rax, cs:qword' at +0x39 (client: 'cmp dword [rax+38h], 0') splits them.
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 70 18 48 89 78 20 55 41 56 41 57 48 8D 68 B1 48 81 EC F0 00 00 00 "
		"4C 8B 75 7F 49 8B F0 44 0F 29 58 88 48 8B FA 44 0F 28 DB 4C 8B F9 49 8B 46 60 "
		"48 85 C0 74 0D 48 3B 05")
		.GetPtr(v_PlayerMelee_AttackTraces);

	if (!v_PlayerMelee_AttackTraces)
		Warning(eDLL_T::SERVER,
			"[OBSTACLE] PlayerMelee attack-trace pattern unresolved -- melee gate disabled\n");
}

void VPlayerDamageObstacle::GetVar(void) const
{
	// The 0x230 dirty-flag and 0x450 origin displacements pick GetAbsOrigin
	// out of a cluster of three byte-identical accessors.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 8B 81 30 02 00 00 48 8B D9 C1 E8 0B A8 01 74 12 "
		"E8 ?? ?? ?? ?? 48 8D 83 50 04 00 00 48 83 C4 20 5B C3 48 8D 81 50 04 00 00")
		.GetPtr(v_ObstacleGetAbsOrigin);

	if (!v_ObstacleGetAbsOrigin)
		Warning(eDLL_T::SERVER,
			"[OBSTACLE] CBaseEntity::GetAbsOrigin pattern unresolved -- gate disabled\n");
}

void VPlayerDamageObstacle::GetCon(void) const { }

void VPlayerDamageObstacle::Detour(const bool bAttach) const
{
	if (v_CCrossbowBolt_BoltTouch)
		DetourSetup(&v_CCrossbowBolt_BoltTouch, &Hook_CCrossbowBolt_BoltTouch, bAttach);
	if (v_PlayerMelee_AttackTraces)
		DetourSetup(&v_PlayerMelee_AttackTraces, &Hook_PlayerMelee_AttackTraces, bAttach);
}
///////////////////////////////////////////////////////////////////////////////
