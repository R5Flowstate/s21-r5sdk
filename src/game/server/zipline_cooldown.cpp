//=============================================================================//
//
// Purpose: escalating zipline re-mount lockout. Ships off: lockout is
// per-player state that never crosses the wire. Enabling needs it networked.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "zipline_cooldown.h"
#include "game/shared/sdk_entity_state.h"
#include "public/edict.h" // CGlobalVars (curTime + realTime)
#include <cmath>

// gpGlobals is defined in the game module; declare locally -- same pattern
// jetdrive.cpp / mantle_boost.cpp use.
extern CGlobalVars* gpGlobals;

//-----------------------------------------------------------------------------
// CPlayer layout -- server half only.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t ZC_PLAYER_OFF_ACTIVEZIPLINE   = 26580; // EHANDLE, -1 = none
static constexpr ptrdiff_t ZC_PLAYER_OFF_LASTDETACHTIME  = 26588; // float, networked m_lastZiplineDetachTime

//-----------------------------------------------------------------------------
// ConVars -- defaults match the S21 client's zipline_cooldown_* values.
// Changing one side without the other re-opens the divergence this closes.
//-----------------------------------------------------------------------------
static ConVar bridge_zip_cooldown_enable("bridge_zip_cooldown_enable", "0", FCVAR_RELEASE,
	"Zipline re-mount lockout on the dedi. Ships OFF: the live client does not "
	"refuse in step with it, so from the third re-mount -- where the ladder first "
	"exceeds a re-jump -- the server refuses a mount the client already made and "
	"the player is yanked off the rope and drops the weapon. 1 = enable the "
	"ladder below (A/B only; expect that failure).");

static ConVar bridge_zip_cooldown_time_0("bridge_zip_cooldown_time_0", "0.1", FCVAR_RELEASE,
	"Zipline re-mount lockout ladder step 0. Mirrors client zipline_cooldown_time_0; "
	"change both sides or the cooldown diverges.");

static ConVar bridge_zip_cooldown_time_1("bridge_zip_cooldown_time_1", "0.2", FCVAR_RELEASE,
	"Zipline re-mount lockout ladder step 1. Mirrors client zipline_cooldown_time_1; "
	"change both sides or the cooldown diverges.");

static ConVar bridge_zip_cooldown_time_2("bridge_zip_cooldown_time_2", "1.0", FCVAR_RELEASE,
	"Zipline re-mount lockout ladder step 2. Mirrors client zipline_cooldown_time_2; "
	"change both sides or the cooldown diverges.");

static ConVar bridge_zip_cooldown_time_3("bridge_zip_cooldown_time_3", "3.0", FCVAR_RELEASE,
	"Zipline re-mount lockout ladder step 3. Mirrors client zipline_cooldown_time_3; "
	"change both sides or the cooldown diverges.");

static ConVar bridge_zip_cooldown_time_4("bridge_zip_cooldown_time_4", "5.0", FCVAR_RELEASE,
	"Zipline re-mount lockout ladder step 4 (saturates here). Mirrors client "
	"zipline_cooldown_time_4; change both sides or the cooldown diverges.");

static ConVar bridge_zip_cooldown_decay("bridge_zip_cooldown_decay", "0", FCVAR_RELEASE,
	"Selects the escalation path, mirroring client zipline_cooldown_decay. "
	"0 = the lockout is stepped by matching its current value against the ladder "
	"and nothing decays it. 1 = the lockout is read out of the ladder by index, "
	"and bleeds back down at decay_rate while riding. Only the decay path decays "
	"at all -- turning this on alone desyncs the two engines.");

static ConVar bridge_zip_cooldown_decay_rate("bridge_zip_cooldown_decay_rate", "0.5", FCVAR_RELEASE,
	"Lockout length bled off per second of ride time, on the decay path only. "
	"Mirrors client zipline_cooldown_decay_rate; change both sides or the "
	"cooldown diverges.");

static ConVar bridge_zip_cooldown_debug("bridge_zip_cooldown_debug", "0", FCVAR_DEVELOPMENTONLY,
	"[ZIP-CD] Log every gate decision, not just the first few.");

//-----------------------------------------------------------------------------
// EHandle-keyed per-player state (recycling cannot sticky-true a pointer).
//-----------------------------------------------------------------------------
struct ZiplineCooldown_t
{
	float flCooldownTime; // current lockout length, starts 0
	int   nCooldownIndex; // ladder position; the decay path steps this instead
	bool  bWasOnGround;   // previous command's ground state, for the landing edge
};

static SDKEntityMap<ZiplineCooldown_t> s_zipCooldownMap(ESide::Server, "zipCooldown.srv");

static uint64_t s_nRefusals      = 0;
static uint64_t s_nMountsGranted = 0;
static uint64_t s_nEscalations   = 0;
static uint64_t s_nGroundResets  = 0;
static uint64_t s_nLogEvents     = 0;

//-----------------------------------------------------------------------------
// Ladder table as an array so the escalate scan stays readable.
//-----------------------------------------------------------------------------
static void ZiplineCooldown_GetTimes(float times[5])
{
	times[0] = bridge_zip_cooldown_time_0.GetFloat();
	times[1] = bridge_zip_cooldown_time_1.GetFloat();
	times[2] = bridge_zip_cooldown_time_2.GetFloat();
	times[3] = bridge_zip_cooldown_time_3.GetFloat();
	times[4] = bridge_zip_cooldown_time_4.GetFloat();
}

// Decay: rate * frametime per command, floored at the first rung. Same integral as the client.
void ZiplineCooldown_OnRideCommand(void* player)
{
	if (!bridge_zip_cooldown_enable.GetBool() || !bridge_zip_cooldown_decay.GetBool()
		|| !player || !gpGlobals)
		return;

	float times[5];
	ZiplineCooldown_GetTimes(times);

	ZiplineCooldown_t& st = s_zipCooldownMap[player];

	const float flRate = bridge_zip_cooldown_decay_rate.GetFloat();

	if (flRate > 0.0f && st.flCooldownTime > times[0])
		st.flCooldownTime = fmaxf(times[0], st.flCooldownTime - (flRate * gpGlobals->frameTime));
}

//-----------------------------------------------------------------------------
// Clear lockout on grounded + not ziplining. Edge-triggered so a standing mount is not undone.
//-----------------------------------------------------------------------------
void ZiplineCooldown_OnGroundState(void* player, const bool bOnGround, const bool bZiplining)
{
	if (!bridge_zip_cooldown_enable.GetBool() || !player)
		return;

	ZiplineCooldown_t& st = s_zipCooldownMap[player];

	const bool bLanded = bOnGround && !st.bWasOnGround;
	st.bWasOnGround = bOnGround;

	if (!bLanded || bZiplining)
		return;

	// Nothing to say when the ladder is already down; landing is a per-life
	// commonplace and this would otherwise drown the log.
	if (st.flCooldownTime == 0.0f && st.nCooldownIndex == 0)
		return;

	st.flCooldownTime = 0.0f;
	st.nCooldownIndex = 0;
	++s_nGroundResets;

	if (bridge_zip_cooldown_debug.GetBool())
		Msg(eDLL_T::SERVER, "[ZIP-CD] ground-reset player=%p resets=%llu\n",
			player, s_nGroundResets);
}

static bool ZiplineCooldown_ShouldLog(void)
{
	++s_nLogEvents;
	return bridge_zip_cooldown_debug.GetBool()
		|| s_nLogEvents <= 64
		|| (s_nLogEvents % 16) == 0;
}

//-----------------------------------------------------------------------------
// Mount gate. Elapsed = curTime; decay = realTime. Do not unify. Only detour Zipline_Use here.
//-----------------------------------------------------------------------------
bool ZiplineCooldown_ShouldRefuseMount(void* player)
{
	if (!bridge_zip_cooldown_enable.GetBool() || !player || !gpGlobals)
		return false;

	ZiplineCooldown_t& st = s_zipCooldownMap[player];

	const float flDetach = *reinterpret_cast<const float*>(
		reinterpret_cast<const uint8_t*>(player) + ZC_PLAYER_OFF_LASTDETACHTIME);
	const float flElapsed = fmaxf(0.0f, gpGlobals->curTime - flDetach);

	if (st.flCooldownTime <= flElapsed)
		return false;

	++s_nRefusals;

	if (ZiplineCooldown_ShouldLog())
	{
		const int nActiveZip = *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(player) + ZC_PLAYER_OFF_ACTIVEZIPLINE);
		Msg(eDLL_T::SERVER,
			"[ZIP-CD] refuse player=%p activeZip=0x%08X cd=%.3f idx=%d elapsed=%.3f detach=%.3f "
			"refusals=%llu mounts=%llu escalations=%llu resets=%llu\n",
			player, static_cast<unsigned>(nActiveZip),
			st.flCooldownTime, st.nCooldownIndex, flElapsed, flDetach,
			s_nRefusals, s_nMountsGranted, s_nEscalations, s_nGroundResets);
	}

	return true;
}

void ZiplineCooldown_OnMountGranted(void* player)
{
	if (!bridge_zip_cooldown_enable.GetBool() || !player || !gpGlobals)
		return;

	float times[5];
	ZiplineCooldown_GetTimes(times);

	ZiplineCooldown_t& st = s_zipCooldownMap[player];

	++s_nMountsGranted;

	if (bridge_zip_cooldown_decay.GetBool())
	{
		// Index path: the lockout is read out of the ladder by position and
		// the position walks to the last rung and stops.
		const int nIndex = st.nCooldownIndex < 0 ? 0
			: (st.nCooldownIndex > 4 ? 4 : st.nCooldownIndex);

		st.flCooldownTime = times[nIndex];

		if (nIndex < 4)
			st.nCooldownIndex = nIndex + 1;
	}
	else if (st.flCooldownTime == 0.0f)
	{
		st.flCooldownTime = times[0];
	}
	else
	{
		// Float equality is deliberate -- mirrors the client's ladder step.
		// It only ever matches because nothing else writes this value.
		for (int i = 0; i < 4; ++i) // scans 0..3 only; saturates at times[4]
		{
			if (st.flCooldownTime == times[i])
			{
				st.flCooldownTime = times[i + 1];
				break;
			}
		}
	}

	++s_nEscalations;

	if (ZiplineCooldown_ShouldLog())
	{
		const float flDetach = *reinterpret_cast<const float*>(
			reinterpret_cast<const uint8_t*>(player) + ZC_PLAYER_OFF_LASTDETACHTIME);
		const int nActiveZip = *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(player) + ZC_PLAYER_OFF_ACTIVEZIPLINE);
		Msg(eDLL_T::SERVER,
			"[ZIP-CD] escalate player=%p activeZip=0x%08X cd=%.3f idx=%d elapsed=%.3f detach=%.3f "
			"refusals=%llu mounts=%llu escalations=%llu resets=%llu\n",
			player, static_cast<unsigned>(nActiveZip),
			st.flCooldownTime, st.nCooldownIndex,
			fmaxf(0.0f, gpGlobals->curTime - flDetach), flDetach,
			s_nRefusals, s_nMountsGranted, s_nEscalations, s_nGroundResets);
	}
}

