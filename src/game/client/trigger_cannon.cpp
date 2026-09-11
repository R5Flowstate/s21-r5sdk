//=============================================================================//
//
// Purpose: gravity-cannon air-control lock, client half. See header.
//
// AirMove strips the along-the-launch component of wish direction whenever the
// launcher-flight status effect is up, but only the jump-pad launch path raises
// that effect. This applies the same projection from the trigger the predicted
// launch was handed -- the one field guaranteed to match what the dedi locked
// against. The window is the trigger's solved flight time stamped on curtime at
// launch; both engines derive it identically at the same predicted instant, so
// the two locks open and close together.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "engine/client/net_bridge_internal.h"
#include "game/client/trigger_cannon.h"

#include <cmath>

//-----------------------------------------------------------------------------
// Raw layout -- movement ctx, C_MoveData, C_Player, C_TriggerCylinderHeavy.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t TCC_CTX_OFF_PLAYER   = 8;   // C_Player*
static constexpr ptrdiff_t TCC_CTX_OFF_MOVEDATA = 16;  // C_MoveData*

static constexpr ptrdiff_t TCC_MV_OFF_MOVEDIR2D = 120; // float[3] m_moveDir2D

// The launch stamps this with curtime + 1.0, which is how a launch that actually
// happened is told apart from a pass that failed its own gates.
static constexpr ptrdiff_t TCC_PLAYER_OFF_DEBOUNCE = 848; // float m_jumpPadDebounceExpireTime

static constexpr ptrdiff_t TCC_TRIG_OFF_LAUNCHFLIGHTTIME = 2592; // float
static constexpr ptrdiff_t TCC_TRIG_OFF_LAUNCHDIR        = 2708; // float[3]

// Below this the horizontal launch direction is degenerate and there is no axis
// to lock against.
static constexpr float TCC_MIN_HORIZ = 1e-6f;

//-----------------------------------------------------------------------------
// ConVars. The dedi registers the same two; they must agree.
//-----------------------------------------------------------------------------
static ConVar bridge_trigger_cannon_lock(
	"bridge_trigger_cannon_lock", "1", FCVAR_RELEASE | FCVAR_CLIENTDLL,
	"Restrict air control for the duration of a gravity-cannon flight, so the "
	"launch lands where it was aimed. Removes the along-the-launch component of "
	"the movement wish direction and denies the lurch, matching how a limited "
	"air-control launcher flies. Must be set the same on client and server.");

static ConVar bridge_trigger_cannon_lock_max_time(
	"bridge_trigger_cannon_lock_max_time", "8.0", FCVAR_RELEASE | FCVAR_CLIENTDLL,
	"Upper bound on how long one gravity-cannon flight may hold air control. The "
	"window is normally the solved flight time; this only catches an absurd solve.");

static ConVar bridge_trigger_cannon_lock_diag(
	"bridge_trigger_cannon_lock_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log [TRIG-CANNON-CL] flight-lock arm and expiry.");

// Movement only ever runs for the locally predicted player, so one window is
// enough. The player pointer is kept so a lock cannot survive into another
// entity's pass if that ever changes.
static const void* s_pLockedPlayer = nullptr;
static float s_flLockExpireTime = 0.0f;
static float s_flLockDirX = 0.0f;
static float s_flLockDirY = 0.0f;
static int s_nLockDiagCount = 0;

//-----------------------------------------------------------------------------
// Purpose: gpGlobals_Client->curtime, the clock AirMove's own time comparisons
// run on.
//-----------------------------------------------------------------------------
static float TriggerCannonClient_CurTime(void)
{
	const uintptr_t pGlobals = *reinterpret_cast<const uintptr_t*>(
		NetObs_Sym(NetObsSym_t::GlobalVarsPtr));

	return pGlobals ? *reinterpret_cast<const float*>(pGlobals + 0x10) : 0.0f;
}

bool TriggerCannonClient_IsFlightLocked(const void* pPlayer)
{
	if (!pPlayer || pPlayer != s_pLockedPlayer || !bridge_trigger_cannon_lock.GetBool())
		return false;

	return s_flLockExpireTime > TriggerCannonClient_CurTime();
}

//-----------------------------------------------------------------------------
// Purpose: open the window for one predicted launch. Everything it reads is
// either restored by a prediction replay or a networked trigger field, so a
// replayed launch command re-derives the identical window instead of extending
// the one the first pass opened.
//-----------------------------------------------------------------------------
static void TriggerCannonClient_ArmFlightLock(const void* pPlayer, const void* pTrigger)
{
	const uint8_t* const pTrigBytes = static_cast<const uint8_t*>(pTrigger);

	const float flFlight = *reinterpret_cast<const float*>(
		pTrigBytes + TCC_TRIG_OFF_LAUNCHFLIGHTTIME);
	if (!std::isfinite(flFlight) || flFlight <= 0.0f)
		return;

	const float* const pLaunchDir = reinterpret_cast<const float*>(
		pTrigBytes + TCC_TRIG_OFF_LAUNCHDIR);

	const float flHoriz = std::sqrt(pLaunchDir[0] * pLaunchDir[0] + pLaunchDir[1] * pLaunchDir[1]);
	if (!std::isfinite(flHoriz) || flHoriz < TCC_MIN_HORIZ)
		return;

	float flMax = bridge_trigger_cannon_lock_max_time.GetFloat();
	if (!std::isfinite(flMax) || flMax <= 0.0f)
		flMax = 8.0f;

	const float flNow = TriggerCannonClient_CurTime();

	s_pLockedPlayer = pPlayer;
	s_flLockExpireTime = flNow + (flFlight < flMax ? flFlight : flMax);
	s_flLockDirX = pLaunchDir[0] / flHoriz;
	s_flLockDirY = pLaunchDir[1] / flHoriz;

	if (bridge_trigger_cannon_lock_diag.GetBool() && s_nLockDiagCount < 16)
	{
		++s_nLockDiagCount;
		Warning(eDLL_T::CLIENT,
			"[TRIG-CANNON-CL] air-control locked t=%.3f until %.3f along (%.3f,%.3f)\n",
			flNow, s_flLockExpireTime, s_flLockDirX, s_flLockDirY);
	}
}

//-----------------------------------------------------------------------------
// The launch has four gates of its own and silently does nothing when any of
// them fails, so the debounce stamp it writes on success is what says a launch
// happened. Reading it twice around the original is cheaper and more honest than
// re-evaluating the gates.
//-----------------------------------------------------------------------------
static void __fastcall Hook_ApplyGravityCannonLaunch(void* ctx, void* pTrigger)
{
	if (!ctx || !pTrigger || !bridge_trigger_cannon_lock.GetBool())
	{
		C_GameMovement__ApplyGravityCannonLaunch(ctx, pTrigger);
		return;
	}

	const uint8_t* const pPlayer = *reinterpret_cast<uint8_t**>(
		static_cast<uint8_t*>(ctx) + TCC_CTX_OFF_PLAYER);
	if (!pPlayer)
	{
		C_GameMovement__ApplyGravityCannonLaunch(ctx, pTrigger);
		return;
	}

	const float flDebounceBefore = *reinterpret_cast<const float*>(
		pPlayer + TCC_PLAYER_OFF_DEBOUNCE);

	C_GameMovement__ApplyGravityCannonLaunch(ctx, pTrigger);

	const float flDebounceAfter = *reinterpret_cast<const float*>(
		pPlayer + TCC_PLAYER_OFF_DEBOUNCE);

	if (flDebounceAfter != flDebounceBefore)
		TriggerCannonClient_ArmFlightLock(pPlayer, pTrigger);
}

//-----------------------------------------------------------------------------
// Same edit and the same restore as the dedi's AirMove detour. AirMove copies
// the wish direction into locals on its first lines and never re-reads the
// field, so the window closes the moment the original returns.
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_AirMove(void* ctx, float flFrameTime, char a3)
{
	uint8_t* pLockedMove = nullptr;
	float savedDir[3] = {};

	if (ctx)
	{
		const void* const pPlayer = *reinterpret_cast<void**>(
			static_cast<uint8_t*>(ctx) + TCC_CTX_OFF_PLAYER);
		uint8_t* const pMove = *reinterpret_cast<uint8_t**>(
			static_cast<uint8_t*>(ctx) + TCC_CTX_OFF_MOVEDATA);

		if (pMove && TriggerCannonClient_IsFlightLocked(pPlayer))
		{
			float* const pWish = reinterpret_cast<float*>(pMove + TCC_MV_OFF_MOVEDIR2D);

			savedDir[0] = pWish[0];
			savedDir[1] = pWish[1];
			savedDir[2] = pWish[2];
			pLockedMove = pMove;

			// Horizontal axis, so the vertical wish is untouched -- the native
			// block's axis has its gravity component removed before it is
			// normalised, which leaves the same zero there.
			const float flAlong = pWish[0] * s_flLockDirX + pWish[1] * s_flLockDirY;
			if (std::isfinite(flAlong))
			{
				pWish[0] -= flAlong * s_flLockDirX;
				pWish[1] -= flAlong * s_flLockDirY;
			}
		}
	}

	const __int64 ret = C_GameMovement__AirMove(ctx, flFrameTime, a3);

	if (pLockedMove)
	{
		float* const pWish = reinterpret_cast<float*>(pLockedMove + TCC_MV_OFF_MOVEDIR2D);
		pWish[0] = savedDir[0];
		pWish[1] = savedDir[1];
		pWish[2] = savedDir[2];
	}

	return ret;
}

//-----------------------------------------------------------------------------
// IDetour
//-----------------------------------------------------------------------------
void VTriggerCannonClient::GetAdr(void) const
{
	LogFunAdr("C_GameMovement::ApplyGravityCannonLaunch", C_GameMovement__ApplyGravityCannonLaunch);
	LogFunAdr("C_GameMovement::AirMove", C_GameMovement__AirMove);
}

void VTriggerCannonClient::GetFun(void) const
{
	// The trigger's m_nextLaunchTime load in the prologue (movss xmm1,
	// [rdx+0A18h]) is what makes this unique -- the surrounding predicted-trigger
	// handlers share the same frame setup.
	Module_FindPattern(g_GameDll,
		"4C 8B DC 49 89 6B 18 56 48 81 EC A0 00 00 00 F3 0F 10 8A 18 0A 00 00 0F 57 C0 0F 2F C8")
		.GetPtr(C_GameMovement__ApplyGravityCannonLaunch);

	// AirMove's prologue ends on the launcher-flight status-effect index load,
	// which no other movement function opens with.
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 57 41 54 41 57 48 8D A8 68 FC FF FF 48 81 EC 78 04 00 00 8B 15")
		.GetPtr(C_GameMovement__AirMove);
}

void VTriggerCannonClient::Detour(const bool bAttach) const
{
	// Either half alone is worse than neither: arming without the projection
	// locks nothing, and projecting without the arm can never trigger. Both or
	// nothing.
	if (!C_GameMovement__ApplyGravityCannonLaunch || !C_GameMovement__AirMove)
	{
		if (bAttach)
			Warning(eDLL_T::CLIENT,
				"[TRIG-CANNON-CL] pattern unresolved (launch=%p airmove=%p) -- gravity-cannon "
				"air-control lock disabled on this side; set bridge_trigger_cannon_lock 0 on the "
				"server so the two halves still agree\n",
				reinterpret_cast<void*>(C_GameMovement__ApplyGravityCannonLaunch),
				reinterpret_cast<void*>(C_GameMovement__AirMove));
		return;
	}

	DetourSetup(&C_GameMovement__ApplyGravityCannonLaunch, &Hook_ApplyGravityCannonLaunch, bAttach);
	DetourSetup(&C_GameMovement__AirMove, &Hook_AirMove, bAttach);
}

