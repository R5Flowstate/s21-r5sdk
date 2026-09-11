//=============================================================================//
//
// Purpose: Server-side player hitbox / collision-hull debug draw.
// Bounds and bone transforms live here; S2C overlay replicate carries them.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "mathlib/vector.h"
#include "mathlib/mathlib.h"
#include "public/idebugoverlay.h"
#include "engine/debugoverlay.h"
#include "common/global.h"
#include "engine/server/server.h"
#include "game/server/player.h"
#include "game/server/baseanimating.h"
#include "game/server/util_server.h"
#include "game/shared/collisionproperty.h"
#include "hitbox_debug.h"

// CHEAT rather than DEVELOPMENTONLY: a dedi that was not launched with dev
// cvars hides DEVELOPMENTONLY names from the command dispatch entirely, so the
// lever cannot be reached even over authenticated RCON.
static ConVar bridge_show_player_hitboxes("bridge_show_player_hitboxes", "0",
	FCVAR_CHEAT | FCVAR_GAMEDLL,
	"Draw live players on connected clients. 1 = collision hull, 2 = per-bone hitboxes.");

// An oriented box is one S2C item, so a whole roster fits in very few frames.
// The remainder of the batch stays free for the script overlays sharing it.
static constexpr int kBoxBudgetPerFrame = 48;
static constexpr int kHullsPerFrame = 12;

// Lifetime covers one refresh. N refreshes blend N copies on the client.
static constexpr float kSweepMargin = 1.5f;
static constexpr float kMinDuration = 0.02f;
static constexpr float kMaxDuration = 1.0f;

static int s_nextClient = 0;
static int s_nextBox = 0;

// The box cursor belongs to one player. Without this, a player leaving mid-set
// hands their offset to whoever occupies the slot next.
static int s_resumeSlot = -1;

static float s_sweepStartTime = -1.0f;
static float s_sweepPeriod = 0.0f;

//-----------------------------------------------------------------------------
// Purpose: how long a shape drawn this frame must live to survive exactly one
//          refresh, from the measured length of the last full roster pass
//-----------------------------------------------------------------------------
static float HitboxDebug_DrawDuration(void)
{
	const float period = (s_sweepPeriod > 0.0f) ? s_sweepPeriod : kMinDuration;

	return Clamp(period * kSweepMargin, kMinDuration, kMaxDuration);
}

static void HitboxDebug_SweepComplete(void)
{
	const float now = g_pServer->GetTime();

	if (s_sweepStartTime >= 0.0f && now > s_sweepStartTime)
		s_sweepPeriod = now - s_sweepStartTime;

	s_sweepStartTime = now;
}

static void HitboxDebug_ResetSweep(void)
{
	s_nextClient = 0;
	s_nextBox = 0;
	s_resumeSlot = -1;
	s_sweepStartTime = -1.0f;
	s_sweepPeriod = 0.0f;
}

//-----------------------------------------------------------------------------
// Purpose: an axis-aligned hull, sent through the oriented-box adder so it
//          costs one S2C item like every other shape here
//-----------------------------------------------------------------------------
static void HitboxDebug_DrawHull(const Vector3D& origin, const Vector3D& mins, const Vector3D& maxs,
	const int r, const int g, const int b)
{
	matrix3x4_t transforms;
	AngleMatrix(QAngle(0.f, 0.f, 0.f), origin, transforms);

	g_pDebugOverlay->AddTransformedBoxOverlay(transforms, mins, maxs, r, g, b, 16, true, HitboxDebug_DrawDuration());
}

static CPlayer* HitboxDebug_PlayerInSlot(const int slot)
{
	const CClient* const pClient = g_pServer->GetClient(slot);

	if (!pClient || !pClient->IsActive())
		return nullptr;

	return UTIL_PlayerByIndex(pClient->GetHandle());
}

//-----------------------------------------------------------------------------
// Purpose: spend one frame's item budget across as many players as it covers,
//          resuming mid-set when a player does not fit
//-----------------------------------------------------------------------------
static void HitboxDebug_DrawHitboxSlice(const int maxClients)
{
	int budget = kBoxBudgetPerFrame;
	int examined = 0;
	const float duration = HitboxDebug_DrawDuration();

	for (int i = 0; i < maxClients && budget > 0; i++)
	{
		examined = i + 1;
		const int slot = (s_nextClient + i) % maxClients;
		CPlayer* const pPlayer = HitboxDebug_PlayerInSlot(slot);

		if (!pPlayer)
			continue;

		const int firstBox = (slot == s_resumeSlot) ? s_nextBox : 0;
		const int numBoxes = pPlayer->DrawServerHitboxRange(firstBox, budget, duration);

		if (numBoxes <= 0 || firstBox >= numBoxes)
		{
			s_resumeSlot = -1;
			continue;
		}

		const int drawn = numBoxes - firstBox;

		if (drawn > budget)
		{
			// This player did not fit; resume mid-set on the next frame.
			s_nextBox = firstBox + budget;
			s_resumeSlot = slot;
			s_nextClient = slot;
			return;
		}

		budget -= drawn;
		s_resumeSlot = -1;
	}

	// Only a pass that reached every slot refreshed every shape; one that ran
	// out of budget leaves the rest for the next frame and is not a sweep.
	if (examined >= maxClients)
		HitboxDebug_SweepComplete();

	s_nextClient = (s_nextClient + 1) % maxClients;
}

static void HitboxDebug_DrawHullSlice(const int maxClients)
{
	int drawn = 0;
	int examined = 0;

	for (int i = 0; i < maxClients && drawn < kHullsPerFrame; i++)
	{
		examined = i + 1;
		const int slot = (s_nextClient + i) % maxClients;
		CPlayer* const pPlayer = HitboxDebug_PlayerInSlot(slot);

		if (!pPlayer)
			continue;

		const CCollisionProperty* const pColl = pPlayer->CollisionProp();

		if (!pColl)
			continue;

		// Bots draw orange and humans cyan, so a bot hull is never mistaken for
		// a human one while comparing against client-side prediction.
		const bool bBot = g_pServer->GetClient(slot)->IsFakeClient();

		HitboxDebug_DrawHull(pPlayer->Diag_AbsOrigin(), pColl->OBBMins(), pColl->OBBMaxs(),
			bBot ? 255 : 0, bBot ? 128 : 255, bBot ? 0 : 255);

		drawn++;
	}

	if (examined >= maxClients)
		HitboxDebug_SweepComplete();

	// Advance past the slots actually examined, not by the budget: empty slots
	// would otherwise push the cursor past players that were never drawn.
	s_nextClient = (s_nextClient + (examined > 0 ? examined : 1)) % maxClients;
}

void HitboxDebug_DrawFrame(void)
{
	const int mode = bridge_show_player_hitboxes.GetInt();

	if (mode <= 0 || !g_pDebugOverlay || !g_pServer)
		return;

	// Broadcast reaches every client and the shapes ignore depth, so without
	// this the lever is a server-wide wallhack rather than a debug view.
	if (!sv_cheats || !sv_cheats->GetBool())
	{
		static bool s_warnedCheats = false;

		if (!s_warnedCheats)
		{
			s_warnedCheats = true;
			Warning(eDLL_T::SERVER, "[HITBOX-DEBUG] ignored: requires sv_cheats\n");
		}

		return;
	}

	// Think is per frame; redraw only when GetTime (tick * interval) moves.
	static float s_lastDrawTime = -1.0f;
	const float now = g_pServer->GetTime();

	if (now == s_lastDrawTime)
		return;

	s_lastDrawTime = now;

	const int maxClients = g_ServerGlobalVariables->maxClients;

	if (maxClients <= 0)
	{
		HitboxDebug_ResetSweep();
		return;
	}

	// The two modes share the roster cursor, so a switch restarts the sweep
	// rather than resuming at a box index the other mode never had. The
	// measured period goes with it: the two modes cost different passes.
	static int s_lastMode = 0;

	if (mode != s_lastMode)
	{
		s_lastMode = mode;
		HitboxDebug_ResetSweep();
	}

	if (mode >= 2)
		HitboxDebug_DrawHitboxSlice(maxClients);
	else
		HitboxDebug_DrawHullSlice(maxClients);
}
