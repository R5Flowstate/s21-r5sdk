//=============================================================================//
//
// Purpose: Bolt hit-size grow schedule parity. The S3 bolt only grows when all
// three grow times are non-zero and never floors or orders the stage ticks;
// the S21 client grows when any time is positive, floors stage 1 to the next
// tick and clamps each later stage up to the previous one. Recompute the
// schedule the S21 way after the server creates the bolt.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "public/edict.h"
#include "game/server/gameinterface.h"
#include "bolt_hitsize_parity.h"

static ConVar bridge_bolt_hitsize_parity("bridge_bolt_hitsize_parity", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Recompute the bolt hit-size grow schedule the way the S21 client does "
	"(any positive grow time grows, stage 1 floored to the next tick, stages ordered).");

static ConVar bridge_bolt_hitsize_tap("bridge_bolt_hitsize_tap", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log one [HITSIZE] line per bolt whose grow schedule was rewritten.");

// CWeaponX modvars (base weapon+6112): bolt_hitsize_grow{1,2,final}_{time,size}.
static constexpr ptrdiff_t WEAPON_OFF_GROW1_TIME     = 6796;
static constexpr ptrdiff_t WEAPON_OFF_GROW1_SIZE     = 6800;
static constexpr ptrdiff_t WEAPON_OFF_GROW2_TIME     = 6804;
static constexpr ptrdiff_t WEAPON_OFF_GROW2_SIZE     = 6808;
static constexpr ptrdiff_t WEAPON_OFF_GROWFINAL_TIME = 6812;
static constexpr ptrdiff_t WEAPON_OFF_GROWFINAL_SIZE = 6816;

// CCrossbowBolt grow block as the server Create writes it.
static constexpr ptrdiff_t BOLT_OFF_DOESGROW    = 9384;
static constexpr ptrdiff_t BOLT_OFF_STAGE1_TICK = 9392;
static constexpr ptrdiff_t BOLT_OFF_STAGE1_SIZE = 9396;
static constexpr ptrdiff_t BOLT_OFF_STAGE2_TICK = 9400;
static constexpr ptrdiff_t BOLT_OFF_STAGE2_SIZE = 9404;
static constexpr ptrdiff_t BOLT_OFF_FINAL_TICK  = 9408;
static constexpr ptrdiff_t BOLT_OFF_FINAL_SIZE  = 9412;

static_assert(offsetof(CGlobalVarsBase, curTime) == 16, "engine curtime");
static_assert(offsetof(CGlobalVarsBase, tickInterval) == 68, "engine tick interval");

static long s_nRewritten = 0;
static bool s_bLiveWarned = false;

static int Hitsize_TickFor(const float flTime, const float flCur, const float flInterval)
{
	if (flTime < 0.0f)
		return 0;

	return static_cast<int>((flCur + flTime) / flInterval + 0.5f);
}

static void Hitsize_ApplyS21Schedule(const __int64 bolt, const __int64 weapon)
{
	if (!bolt || !weapon || !gpGlobals)
		return;

	const float flInterval = gpGlobals->tickInterval;

	if (!(flInterval > 0.0f))
		return;

	const float t1 = *reinterpret_cast<const float*>(weapon + WEAPON_OFF_GROW1_TIME);
	const float t2 = *reinterpret_cast<const float*>(weapon + WEAPON_OFF_GROW2_TIME);
	const float tf = *reinterpret_cast<const float*>(weapon + WEAPON_OFF_GROWFINAL_TIME);

	if (!isfinite(t1) || !isfinite(t2) || !isfinite(tf))
		return;

	unsigned char* const pDoesGrow = reinterpret_cast<unsigned char*>(bolt + BOLT_OFF_DOESGROW);
	const unsigned char nHadGrow = *pDoesGrow;

	if (!(t1 > 0.0f || t2 > 0.0f || tf > 0.0f))
	{
		*pDoesGrow = 0;
		return;
	}

	const float flCur = gpGlobals->curTime;
	const int nFloor = static_cast<int>(flCur / flInterval + 0.5f) + 1;

	int k1 = Hitsize_TickFor(t1, flCur, flInterval);
	if (k1 < nFloor)
		k1 = nFloor;

	int k2 = Hitsize_TickFor(t2, flCur, flInterval);
	if (k2 < k1)
		k2 = k1;

	int kf = Hitsize_TickFor(tf, flCur, flInterval);
	if (kf < k2)
		kf = k2;

	int* const pTick1 = reinterpret_cast<int*>(bolt + BOLT_OFF_STAGE1_TICK);
	int* const pTick2 = reinterpret_cast<int*>(bolt + BOLT_OFF_STAGE2_TICK);
	int* const pTickF = reinterpret_cast<int*>(bolt + BOLT_OFF_FINAL_TICK);

	const bool bChanged = !nHadGrow || *pTick1 != k1 || *pTick2 != k2 || *pTickF != kf;

	*pDoesGrow = 1;
	*pTick1 = k1;
	*pTick2 = k2;
	*pTickF = kf;
	*reinterpret_cast<float*>(bolt + BOLT_OFF_STAGE1_SIZE) = *reinterpret_cast<const float*>(weapon + WEAPON_OFF_GROW1_SIZE);
	*reinterpret_cast<float*>(bolt + BOLT_OFF_STAGE2_SIZE) = *reinterpret_cast<const float*>(weapon + WEAPON_OFF_GROW2_SIZE);
	*reinterpret_cast<float*>(bolt + BOLT_OFF_FINAL_SIZE)  = *reinterpret_cast<const float*>(weapon + WEAPON_OFF_GROWFINAL_SIZE);

	if (!bChanged)
		return;

	++s_nRewritten;

	if (!s_bLiveWarned)
	{
		s_bLiveWarned = true;
		Warning(eDLL_T::SERVER,
			"[HITSIZE] parity live: grow schedule rewritten (hadGrow=%d t=%.3f/%.3f/%.3f ticks=%d/%d/%d floor=%d)\n",
			nHadGrow, t1, t2, tf, k1, k2, kf, nFloor);
	}

	if (bridge_bolt_hitsize_tap.GetBool())
		Msg(eDLL_T::SERVER,
			"[HITSIZE] hadGrow=%d t=%.3f/%.3f/%.3f ticks=%d/%d/%d floor=%d total=%ld\n",
			nHadGrow, t1, t2, tf, k1, k2, kf, nFloor, s_nRewritten);
}

static __int64 __fastcall Hook_CrossbowBolt_Create(__int64 pOrigin, float* pDir, __int64 damage,
	float speed, __int64 owner, int impactTable, unsigned int modelIndex, float hitSize,
	unsigned char usesGravity, int projectileIndex, __int64 weapon)
{
	const __int64 bolt = v_CrossbowBolt_Create(pOrigin, pDir, damage, speed, owner,
		impactTable, modelIndex, hitSize, usesGravity, projectileIndex, weapon);

	if (bolt && bridge_bolt_hitsize_parity.GetBool())
		Hitsize_ApplyS21Schedule(bolt, weapon);

	return bolt;
}

///////////////////////////////////////////////////////////////////////////////
void VBoltHitsizeParity::GetFun(void) const
{
	// Server CrossbowBolt_Create; the client twin's prologue
	// differs from the first byte after the home-space stores.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 70 48 8B BC 24 E0 00 00 00 "
		"4C 8B FA 48 8B F1 0F 29 7C 24 50 49 8B CF 48 8D 54 24 40")
		.GetPtr(v_CrossbowBolt_Create);

	if (!v_CrossbowBolt_Create)
		Warning(eDLL_T::SERVER,
			"[HITSIZE] CrossbowBolt_Create pattern unresolved -- parity disabled\n");
}

void VBoltHitsizeParity::Detour(const bool bAttach) const
{
	if (v_CrossbowBolt_Create)
		DetourSetup(&v_CrossbowBolt_Create, &Hook_CrossbowBolt_Create, bAttach);
}
///////////////////////////////////////////////////////////////////////////////
