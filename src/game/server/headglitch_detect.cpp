//=============================================================================//
//
// Purpose: Server-side head-glitch abuse detection. See headglitch_detect.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "mathlib/vector.h"
#include "mathlib/mathlib.h"
#include "public/cmodel.h"
#include "public/bspflags.h"
#include "public/engine/IEngineTrace.h"
#include "common/global.h"
#include "engine/enginetrace.h"
#include "engine/server/server.h"
#include "game/server/player.h"
#include "game/server/util_server.h"
#include "game/shared/collisionproperty.h"
#include "game/shared/util_shared.h"
#include "public/game/shared/in_buttons.h"
#include "headglitch_detect.h"

// FCVAR_RELEASE: DEVELOPMENTONLY names are hidden from dispatch on a non-dev dedi, including RCON.
static ConVar bridge_headglitch_detect("bridge_headglitch_detect", "0",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Measure head-glitch line-of-sight asymmetry. 0 = off, 1 = always, "
	"2 = only while mp_gamemode matches bridge_headglitch_gamemode.",
	true, 0.0f, true, 2.0f);

static ConVar bridge_headglitch_gamemode("bridge_headglitch_gamemode", "fs_1v1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Gamemode that arms detection under bridge_headglitch_detect 2.");

static ConVar bridge_headglitch_rate("bridge_headglitch_rate", "10",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Pair evaluations per second.", true, 1.0f, true, 60.0f);

static ConVar bridge_headglitch_exposure("bridge_headglitch_exposure", "0.15",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Silhouette fraction the target may see before the pose stops counting as a glitch.",
	true, 0.0f, true, 1.0f);

static ConVar bridge_headglitch_crest("bridge_headglitch_crest", "12",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Max units the shooter's eye may sit above the cover crest.", true, 1.0f, true, 128.0f);

static ConVar bridge_headglitch_hold("bridge_headglitch_hold", "0.75",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Seconds the pose must hold continuously before it scores. Shorter than this is a peek.",
	true, 0.0f, true, 10.0f);

static ConVar bridge_headglitch_halflife("bridge_headglitch_halflife", "4",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Score half-life in seconds.", true, 0.5f, true, 120.0f);

static ConVar bridge_headglitch_flag("bridge_headglitch_flag", "3",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Score at which a player is reported as abusing. 0 disables reporting.",
	true, 0.0f, true, 1000.0f);

static ConVar bridge_headglitch_range("bridge_headglitch_range", "3000",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Max shooter-to-target distance considered.", true, 128.0f, true, 32768.0f);

static ConVar bridge_headglitch_cone("bridge_headglitch_cone", "0.9",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Min cos(angle) between aim and the target before a pair is traced at all.",
	true, 0.0f, true, 1.0f);

static ConVar bridge_headglitch_events("bridge_headglitch_events", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Log one [HEADGLITCH] line when a scoring episode opens and closes.");

static ConVar bridge_headglitch_diag("bridge_headglitch_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log every scoring sample as [HEADGLITCH] so thresholds can be fitted from real matches.");

// Glass and grates pass sight and bullets. Entities never block; TRACE_EVERYTHING still lets static props.
static constexpr unsigned int kCoverMask = CONTENTS_SOLID | CONTENTS_MOVEABLE;

// Vertical sample ladder, feet to head. Top band is the head (0.10 under the default threshold).
static constexpr int kLadderPoints = 5;
static constexpr float kLadderFrac[kLadderPoints] = { 0.08f, 0.30f, 0.52f, 0.74f, 0.94f };
static constexpr float kLadderWeight[kLadderPoints] = { 0.25f, 0.25f, 0.25f, 0.15f, 0.10f };

// Bisection steps between the highest blocked and lowest clear ladder point.
// Three steps resolve the crest to about a quarter of the band spacing.
static constexpr int kCrestSteps = 3;

// The crest may not wander further than this between samples and still count as
// the same piece of cover. A vault, slide or ramp moves it far more.
static constexpr float kCrestDriftMax = 8.0f;

// Hard ceiling on traces per sample tick, independent of roster size.
static constexpr int kMaxEvalsPerSample = 4;

// Firing is the abuse; aiming is intent; merely standing there barely counts.
static constexpr float kWeightFiring = 1.0f;
static constexpr float kWeightAiming = 0.75f;
static constexpr float kWeightIdle = 0.25f;

struct HeadGlitchState_t
{
	// Live pose.
	float score;
	float exposure;
	float crestZ;
	float crestDelta;
	float holdStart;
	int targetSlot;
	bool active;
	bool flagged;

	// Current scoring episode, opened once the hold is satisfied.
	bool episodeOpen;
	float episodeStart;
	float episodeScoreBase;
	float episodeMinExposure;
	int episodeTargetSlot;

	// Session tally. Survives death so a round loss does not erase the round
	// that earned it; cleared only when the slot goes away.
	float peakScore;
	float totalTime;
	int episodes;
	int flagCount;

	// Last seen net name, so an episode that ends with the player already dead
	// still names them.
	char name[32];
};

static HeadGlitchState_t s_state[MAX_PLAYERS];
static float s_lastSampleTime = 0.0f;
static int s_evalCursor = 0;
static bool s_bWasEnabled = false;

//-----------------------------------------------------------------------------
// Purpose: world and static props block, players and other entities do not
//-----------------------------------------------------------------------------
class CTraceFilterCoverOnly : public CTraceFilter
{
public:
	virtual bool ShouldHitEntity(IHandleEntity* const pEntity, const int contentsMask)
	{
		NOTE_UNUSED(pEntity);
		NOTE_UNUSED(contentsMask);
		return false;
	}

	virtual bool ShouldBlockTrace(trace_t* const pTrace)
	{
		NOTE_UNUSED(pTrace);
		return false;
	}
};

//-----------------------------------------------------------------------------
// Purpose: end the live pose and its score, keeping the session tally
//-----------------------------------------------------------------------------
static void HeadGlitch_ResetPose(const int slot)
{
	HeadGlitchState_t& st = s_state[slot];

	st.score = 0.0f;
	st.exposure = 0.0f;
	st.crestZ = 0.0f;
	st.crestDelta = 0.0f;
	st.holdStart = -1.0f;
	st.targetSlot = -1;
	st.active = false;
	st.flagged = false;
	st.episodeOpen = false;
	st.episodeStart = 0.0f;
	st.episodeScoreBase = 0.0f;
	st.episodeMinExposure = 1.0f;
	st.episodeTargetSlot = -1;
}

static void HeadGlitch_ClearSlot(const int slot)
{
	memset(&s_state[slot], 0, sizeof(s_state[slot]));

	HeadGlitch_ResetPose(slot);
}

static void HeadGlitch_ClearAll(void)
{
	for (int i = 0; i < MAX_PLAYERS; i++)
		HeadGlitch_ClearSlot(i);
}

static int HeadGlitch_SlotOf(CPlayer* const pPlayer)
{
	if (!pPlayer)
		return -1;

	const int slot = pPlayer->GetEdict() - 1;

	return (slot >= 0 && slot < MAX_PLAYERS) ? slot : -1;
}

//-----------------------------------------------------------------------------
// Purpose: whether detection is armed right now, honouring the gamemode gate
//-----------------------------------------------------------------------------
bool HeadGlitch_IsEnabled(void)
{
	const int mode = bridge_headglitch_detect.GetInt();

	if (mode <= 0)
		return false;

	if (mode == 1)
		return true;

	if (!mp_gamemode)
		return false;

	return _stricmp(mp_gamemode->GetString(), bridge_headglitch_gamemode.GetString()) == 0;
}

static bool HeadGlitch_TraceClear(const Vector3D& start, const Vector3D& end)
{
	Ray_t ray(start, end);
	CTraceFilterCoverOnly filter;
	trace_t tr;

	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1.0f;
	tr.endpos = end;

	g_pEngineTraceServer->TraceRayFiltered(ray, kCoverMask, &filter, &tr);

	return tr.fraction >= 1.0f && !tr.startsolid && !tr.allsolid;
}

//-----------------------------------------------------------------------------
// Purpose: height where the target's view of the shooter flips blocked to clear.
//-----------------------------------------------------------------------------
static float HeadGlitch_FindCrest(const Vector3D& viewer, const Vector3D& axis,
	float blockedZ, float clearZ)
{
	Vector3D probe = axis;

	for (int i = 0; i < kCrestSteps; i++)
	{
		probe.z = (blockedZ + clearZ) * 0.5f;

		if (HeadGlitch_TraceClear(viewer, probe))
			clearZ = probe.z;
		else
			blockedZ = probe.z;
	}

	return clearZ;
}

//-----------------------------------------------------------------------------
// Purpose: true when the shooter is in head-glitch geometry against this target.
//-----------------------------------------------------------------------------
static bool HeadGlitch_EvaluatePair(CPlayer* const pShooter, CPlayer* const pTarget,
	float& outExposure, float& outCrestZ, float& outCrestDelta)
{
	const CCollisionProperty* const pShooterColl = pShooter->CollisionProp();
	const CCollisionProperty* const pTargetColl = pTarget->CollisionProp();

	if (!pShooterColl || !pTargetColl)
		return false;

	Vector3D shooterEye;
	Vector3D targetEye;

	if (!CPlayer__EyePosition(pShooter, &shooterEye) || !CPlayer__EyePosition(pTarget, &targetEye))
		return false;

	const Vector3D& shooterOrigin = pShooter->Diag_AbsOrigin();
	const Vector3D& targetOrigin = pTarget->Diag_AbsOrigin();

	// Torso rather than eye: a target whose own head is the only thing showing
	// is peeking back, not being glitched.
	const Vector3D& targetMins = pTargetColl->OBBMins();
	const Vector3D& targetMaxs = pTargetColl->OBBMaxs();
	const Vector3D targetChest(targetOrigin.x, targetOrigin.y,
		targetOrigin.z + targetMins.z + (targetMaxs.z - targetMins.z) * 0.6f);

	if (!HeadGlitch_TraceClear(shooterEye, targetChest))
		return false;

	const Vector3D& shooterMins = pShooterColl->OBBMins();
	const Vector3D& shooterMaxs = pShooterColl->OBBMaxs();
	const float hullBase = shooterOrigin.z + shooterMins.z;
	const float hullHeight = shooterMaxs.z - shooterMins.z;

	if (hullHeight <= 1.0f)
		return false;

	Vector3D probe(shooterOrigin.x, shooterOrigin.y, hullBase);
	float exposed = 0.0f;
	float total = 0.0f;
	float highestBlockedZ = hullBase;
	float lowestClearZ = -FLT_MAX;
	bool bTopClear = false;

	for (int i = 0; i < kLadderPoints; i++)
	{
		probe.z = hullBase + hullHeight * kLadderFrac[i];
		total += kLadderWeight[i];

		if (HeadGlitch_TraceClear(targetEye, probe))
		{
			exposed += kLadderWeight[i];

			if (lowestClearZ == -FLT_MAX)
				lowestClearZ = probe.z;

			if (i == kLadderPoints - 1)
				bTopClear = true;
		}
		else
		{
			// A clear band under a blocked one is a gap, not a crest; the
			// bracket has to be the last blocked point below the clear run.
			highestBlockedZ = probe.z;
			lowestClearZ = -FLT_MAX;
			bTopClear = false;
		}
	}

	// Only the top of the silhouette showing is the whole signature. Nothing
	// showing is ordinary cover, and a broad exposure is an ordinary fight.
	if (!bTopClear || lowestClearZ == -FLT_MAX || total <= 0.0f)
		return false;

	outExposure = exposed / total;

	if (outExposure > bridge_headglitch_exposure.GetFloat())
		return false;

	outCrestZ = HeadGlitch_FindCrest(targetEye, probe, highestBlockedZ, lowestClearZ);
	outCrestDelta = shooterEye.z - outCrestZ;

	return outCrestDelta > 0.0f && outCrestDelta < bridge_headglitch_crest.GetFloat();
}

static bool HeadGlitch_SlotConnected(const int slot)
{
	const CClient* const pClient = g_pServer->GetClient(slot);

	return pClient && pClient->IsActive();
}

static CPlayer* HeadGlitch_PlayerInSlot(const int slot)
{
	const CClient* const pClient = g_pServer->GetClient(slot);

	if (!pClient || !pClient->IsActive())
		return nullptr;

	CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());

	if (!pPlayer || pPlayer->GetLifeState() != 0)
		return nullptr;

	return pPlayer;
}

// The stored name outlives the player pointer, so an episode closed by a death
// still reports who it belonged to.
static const char* HeadGlitch_NameInSlot(const int slot)
{
	if (slot < 0 || slot >= MAX_PLAYERS)
		return "?";

	return s_state[slot].name[0] ? s_state[slot].name : "?";
}

static void HeadGlitch_StoreName(const int slot, const char* const pszName)
{
	if (!pszName)
		return;

	strncpy(s_state[slot].name, pszName, sizeof(s_state[slot].name) - 1);
	s_state[slot].name[sizeof(s_state[slot].name) - 1] = '\0';
}

//-----------------------------------------------------------------------------
// Purpose: the enemy nearest the shooter's aim axis, or none.
//-----------------------------------------------------------------------------
static int HeadGlitch_PickTarget(CPlayer* const pShooter, const int shooterSlot, const int maxClients)
{
	QAngle eyeAngles;

	if (!CPlayer__EyeAngles(pShooter, &eyeAngles))
		return -1;

	Vector3D forward;
	AngleVectors(eyeAngles, &forward);

	const Vector3D& shooterOrigin = pShooter->Diag_AbsOrigin();
	const float maxRange = bridge_headglitch_range.GetFloat();
	const float minCone = bridge_headglitch_cone.GetFloat();

	int bestSlot = -1;
	float bestDot = minCone;

	for (int i = 0; i < maxClients; i++)
	{
		if (i == shooterSlot)
			continue;

		CPlayer* const pTarget = HeadGlitch_PlayerInSlot(i);

		if (!pTarget || pTarget->GetTeamNum() == pShooter->GetTeamNum())
			continue;

		Vector3D delta = pTarget->Diag_AbsOrigin() - shooterOrigin;
		const float dist = delta.Length();

		if (dist < 1.0f || dist > maxRange)
			continue;

		delta /= dist;
		const float dot = delta.Dot(forward);

		if (dot > bestDot)
		{
			bestDot = dot;
			bestSlot = i;
		}
	}

	return bestSlot;
}

static void HeadGlitch_CloseEpisode(const int slot, const float now)
{
	HeadGlitchState_t& st = s_state[slot];

	if (!st.episodeOpen)
		return;

	const float held = now - st.episodeStart;

	st.episodeOpen = false;
	st.totalTime += held;
	st.episodes++;

	if (bridge_headglitch_events.GetBool())
	{
		Msg(eDLL_T::SERVER,
			"[HEADGLITCH] end   '%s' vs '%s' held=%.2fs gained=%.2f minExposure=%.2f score=%.2f\n",
			HeadGlitch_NameInSlot(slot), HeadGlitch_NameInSlot(st.episodeTargetSlot),
			held, st.score - st.episodeScoreBase, st.episodeMinExposure, st.score);
	}

	st.episodeMinExposure = 1.0f;
	st.episodeTargetSlot = -1;
}

//-----------------------------------------------------------------------------
// Purpose: a death ends the pose but banks the episode; the session tally survives.
//-----------------------------------------------------------------------------
static void HeadGlitch_EndPose(const int slot, const float now)
{
	HeadGlitch_CloseEpisode(slot, now);
	HeadGlitch_ResetPose(slot);
}

static void HeadGlitch_Accumulate(const int slot, CPlayer* const pShooter,
	const bool bGlitching, const float now, const float dt)
{
	HeadGlitchState_t& st = s_state[slot];

	st.score *= expf(-dt * 0.69314718f / bridge_headglitch_halflife.GetFloat());
	st.active = bGlitching;

	if (!bGlitching)
	{
		HeadGlitch_CloseEpisode(slot, now);
		st.holdStart = -1.0f;
		st.targetSlot = -1;
		return;
	}

	if (st.holdStart < 0.0f)
		st.holdStart = now;

	if (now - st.holdStart < bridge_headglitch_hold.GetFloat())
		return;

	// The target changing mid-episode is a new episode against a new victim.
	if (st.episodeOpen && st.episodeTargetSlot != st.targetSlot)
		HeadGlitch_CloseEpisode(slot, now);

	if (!st.episodeOpen)
	{
		st.episodeOpen = true;
		st.episodeStart = now;
		st.episodeScoreBase = st.score;
		st.episodeMinExposure = st.exposure;
		st.episodeTargetSlot = st.targetSlot;

		if (bridge_headglitch_events.GetBool())
		{
			Msg(eDLL_T::SERVER,
				"[HEADGLITCH] begin '%s' vs '%s' exposure=%.2f crest=%.1f score=%.2f\n",
				pShooter->GetNetName(), HeadGlitch_NameInSlot(st.targetSlot),
				st.exposure, st.crestDelta, st.score);
		}
	}

	st.episodeMinExposure = fminf(st.episodeMinExposure, st.exposure);

	const bool bFiring = (pShooter->Diag_Buttons() & IN_ATTACK) != 0;
	const float weight = bFiring ? kWeightFiring
		: (pShooter->IsZooming() ? kWeightAiming : kWeightIdle);

	st.score += dt * weight * (1.0f - st.exposure);
	st.peakScore = fmaxf(st.peakScore, st.score);

	const float flagAt = bridge_headglitch_flag.GetFloat();

	if (flagAt > 0.0f && !st.flagged && st.score >= flagAt)
	{
		st.flagged = true;
		st.flagCount++;
		Warning(eDLL_T::SERVER,
			"[HEADGLITCH] FLAG  '%s' score=%.2f exposure=%.2f crest=%.1f held=%.2fs episodes=%d\n",
			pShooter->GetNetName(), st.score, st.exposure, st.crestDelta,
			now - st.holdStart, st.episodes + 1);
	}
}

//-----------------------------------------------------------------------------
// Purpose: one sample tick across the roster, called from the server think
//-----------------------------------------------------------------------------
void HeadGlitch_Frame(void)
{
	const bool bEnabled = HeadGlitch_IsEnabled();

	if (!bEnabled)
	{
		// Leaving the armed gamemode must not leave stale scores behind for
		// the next match to inherit.
		if (s_bWasEnabled)
		{
			HeadGlitch_ClearAll();
			s_bWasEnabled = false;
		}
		return;
	}

	if (!s_bWasEnabled)
	{
		HeadGlitch_ClearAll();
		s_bWasEnabled = true;
		s_lastSampleTime = 0.0f;

		Msg(eDLL_T::SERVER, "[HEADGLITCH] armed (mode %d, gamemode '%s')\n",
			bridge_headglitch_detect.GetInt(),
			mp_gamemode ? mp_gamemode->GetString() : "?");
	}

	if (!g_pEngineTraceServer || !CPlayer__EyePosition || !CPlayer__EyeAngles)
		return;

	const float now = g_pServer->GetTime();
	const float interval = 1.0f / bridge_headglitch_rate.GetFloat();

	// A changelevel rewinds the server clock; treat that as a fresh start
	// rather than banking one enormous dt into every score.
	if (now < s_lastSampleTime)
	{
		HeadGlitch_ClearAll();
		s_lastSampleTime = now;
		return;
	}

	const float dt = now - s_lastSampleTime;

	if (dt < interval)
		return;

	s_lastSampleTime = now;

	const int maxClients = g_ServerGlobalVariables->maxClients;

	if (maxClients <= 0)
		return;

	int evals = 0;

	for (int i = 0; i < maxClients && i < MAX_PLAYERS; i++)
	{
		const int slot = (s_evalCursor + i) % maxClients;
		CPlayer* const pShooter = HeadGlitch_PlayerInSlot(slot);

		if (!pShooter)
		{
			// Dying ends the pose but keeps the tally; only a slot that has
			// gone away starts over.
			if (HeadGlitch_SlotConnected(slot))
				HeadGlitch_EndPose(slot, now);
			else
				HeadGlitch_ClearSlot(slot);
			continue;
		}

		HeadGlitch_StoreName(slot, pShooter->GetNetName());

		if (evals >= kMaxEvalsPerSample)
			continue;

		const int targetSlot = HeadGlitch_PickTarget(pShooter, slot, maxClients);
		CPlayer* const pTarget = targetSlot >= 0 ? HeadGlitch_PlayerInSlot(targetSlot) : nullptr;

		if (!pTarget)
		{
			HeadGlitch_Accumulate(slot, pShooter, false, now, dt);
			continue;
		}

		evals++;

		HeadGlitchState_t& st = s_state[slot];
		float exposure = 0.0f;
		float crestZ = 0.0f;
		float crestDelta = 0.0f;
		const bool bGlitching = HeadGlitch_EvaluatePair(pShooter, pTarget, exposure, crestZ, crestDelta);

		if (bGlitching)
		{
			// A crest that jumped is a different piece of cover, so the hold
			// restarts even though the pose still qualifies.
			if (st.targetSlot != targetSlot || fabsf(crestZ - st.crestZ) > kCrestDriftMax)
				st.holdStart = now;

			st.exposure = exposure;
			st.crestZ = crestZ;
			st.crestDelta = crestDelta;
			st.targetSlot = targetSlot;
		}

		HeadGlitch_Accumulate(slot, pShooter, bGlitching, now, dt);

		if (bGlitching && bridge_headglitch_diag.GetBool())
		{
			Msg(eDLL_T::SERVER,
				"[HEADGLITCH] sample '%s' vs '%s' exposure=%.3f crest=%.1f held=%.2f score=%.2f\n",
				pShooter->GetNetName(), pTarget->GetNetName(), exposure, crestDelta,
				st.holdStart >= 0.0f ? now - st.holdStart : 0.0f, st.score);
		}
	}

	s_evalCursor = (s_evalCursor + 1) % maxClients;
}

//-----------------------------------------------------------------------------
// Purpose: whole-roster readout, reachable over RCON
//-----------------------------------------------------------------------------
static void CC_HeadGlitch_Status(const CCommand& args)
{
	NOTE_UNUSED(args);

	const int mode = bridge_headglitch_detect.GetInt();

	Msg(eDLL_T::SERVER, "[HEADGLITCH] mode=%d armed=%s gamemode='%s' gate='%s'\n",
		mode, HeadGlitch_IsEnabled() ? "yes" : "no",
		mp_gamemode ? mp_gamemode->GetString() : "?",
		bridge_headglitch_gamemode.GetString());
	Msg(eDLL_T::SERVER,
		"[HEADGLITCH] thresholds exposure<=%.2f crest<%.0fu hold>=%.2fs halflife=%.1fs flag>=%.2f\n",
		bridge_headglitch_exposure.GetFloat(), bridge_headglitch_crest.GetFloat(),
		bridge_headglitch_hold.GetFloat(), bridge_headglitch_halflife.GetFloat(),
		bridge_headglitch_flag.GetFloat());
	Msg(eDLL_T::SERVER,
		"[HEADGLITCH] slot name             score  peak  expo crest  eps  time flags target\n");

	const int maxClients = g_ServerGlobalVariables ? g_ServerGlobalVariables->maxClients : 0;
	int shown = 0;

	// Connected rather than alive: a dead player's session tally is exactly
	// what an operator opens this command to read.
	for (int slot = 0; slot < maxClients && slot < MAX_PLAYERS; slot++)
	{
		if (!HeadGlitch_SlotConnected(slot))
			continue;

		const HeadGlitchState_t& st = s_state[slot];

		Msg(eDLL_T::SERVER,
			"[HEADGLITCH] %4d %-16.16s %5.2f %5.2f %4.2f %5.1f %4d %5.1f %5d %s\n",
			slot, HeadGlitch_NameInSlot(slot), st.score, st.peakScore,
			st.active ? st.exposure : 1.0f, st.active ? st.crestDelta : 0.0f,
			st.episodes, st.totalTime, st.flagCount,
			st.active ? HeadGlitch_NameInSlot(st.targetSlot) : "-");
		shown++;
	}

	if (!shown)
		Msg(eDLL_T::SERVER, "[HEADGLITCH] no connected players\n");
}

static ConCommand headglitch_status("headglitch_status", CC_HeadGlitch_Status,
	"Print head-glitch scores and episode tallies for every live player.",
	FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_CHEAT);

static void CC_HeadGlitch_Reset(const CCommand& args)
{
	NOTE_UNUSED(args);

	HeadGlitch_ClearAll();
	Msg(eDLL_T::SERVER, "[HEADGLITCH] all scores and tallies cleared\n");
}

static ConCommand headglitch_reset("headglitch_reset", CC_HeadGlitch_Reset,
	"Clear every player's head-glitch score, flag and episode tally.",
	FCVAR_RELEASE | FCVAR_GAMEDLL | FCVAR_CHEAT);

float HeadGlitch_GetScore(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	return slot >= 0 ? s_state[slot].score : 0.0f;
}

float HeadGlitch_GetPeakScore(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	return slot >= 0 ? s_state[slot].peakScore : 0.0f;
}

float HeadGlitch_GetExposure(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	return (slot >= 0 && s_state[slot].active) ? s_state[slot].exposure : 1.0f;
}

float HeadGlitch_GetHoldTime(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	if (slot < 0 || !s_state[slot].active || s_state[slot].holdStart < 0.0f)
		return 0.0f;

	return g_pServer->GetTime() - s_state[slot].holdStart;
}

float HeadGlitch_GetTotalTime(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	return slot >= 0 ? s_state[slot].totalTime : 0.0f;
}

int HeadGlitch_GetEpisodeCount(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	return slot >= 0 ? s_state[slot].episodes : 0;
}

bool HeadGlitch_IsActive(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	return slot >= 0 ? s_state[slot].active : false;
}

bool HeadGlitch_IsFlagged(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	return slot >= 0 ? s_state[slot].flagged : false;
}

int HeadGlitch_GetFlagCount(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	return slot >= 0 ? s_state[slot].flagCount : 0;
}

void HeadGlitch_ResetScore(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	if (slot >= 0)
		HeadGlitch_EndPose(slot, g_pServer->GetTime());
}

void HeadGlitch_ClearPlayer(CPlayer* const pPlayer)
{
	const int slot = HeadGlitch_SlotOf(pPlayer);

	if (slot >= 0)
		HeadGlitch_ClearSlot(slot);
}
