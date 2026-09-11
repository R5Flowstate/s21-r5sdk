//=============================================================================//
//
// Purpose: mantle_boost tap-strafe suppression on S3's native inline lurch.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tapstrafe.h"
#include "mantle_boost.h"
#include "trigger_cannon.h"
#include "player.h"

//-----------------------------------------------------------------------------
// Raw layout constants -- -verified in (r5apex_ds).
//-----------------------------------------------------------------------------
// CGameMovement ctx (same offsets mantle_boost.cpp already established)
static constexpr ptrdiff_t TS_CTX_OFF_PLAYER   = 8;    // CPlayer*
static constexpr ptrdiff_t TS_CTX_OFF_MOVEDATA = 16;   // CMoveData*

// CMoveData (S3): m_nButtonsPressed at mv+0x2C. Masking it suppresses only the lurch.
static constexpr ptrdiff_t TS_MV_OFF_BUTTONS_PRESSED = 44;

// IN_FORWARD|IN_BACK|IN_MOVELEFT|IN_MOVERIGHT -- the native lurch press gate.
static constexpr uint32_t TS_LURCH_PRESS_MASK = 0x618;

// CMoveData (S3): m_vecVelocity at mv+304.
static constexpr ptrdiff_t TS_MV_OFF_VELOCITY = 304;

// The S21 client skips the lurch above jump_grace_cutoff_speed; the S3 inline
// lurch has no speed gate, so the dedi mirrors the client's refusal here.
static ConVar bridge_lurch_cutoff_speed("bridge_lurch_cutoff_speed", "1100", FCVAR_RELEASE,
	"[TAPSTRAFE] Horizontal speed above which the dedi suppresses the tap-strafe lurch, "
	"matching the S21 client's jump_grace_cutoff_speed. 0 = never suppress.",
	true, 0.f, true, 100000.f);

static bool TapStrafe_AboveCutoff(const uint8_t* const mv)
{
	const float flCutoff = bridge_lurch_cutoff_speed.GetFloat();
	if (flCutoff <= 0.0f)
		return false;

	const float* const vel = reinterpret_cast<const float*>(mv + TS_MV_OFF_VELOCITY);
	return (vel[0] * vel[0] + vel[1] * vel[1]) > (flCutoff * flCutoff);
}

//-----------------------------------------------------------------------------
// CGameMovement::AirMove. Float in XMM1 must be forwarded exactly.
//-----------------------------------------------------------------------------
static __int64 (*v_CGameMovement__AirMove)(void* ctx, float flFrameTime, char a3) = nullptr;

static ConVar bridge_tapstrafe_debug("bridge_tapstrafe_debug", "0", FCVAR_DEVELOPMENTONLY,
	"[TAPSTRAFE] Verbose [TAPSTRAFE] suppression debug logging.");

//-----------------------------------------------------------------------------
// Mask directional press bits while mantle_boost_disables_tap_strafes && state==4.
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_CGameMovement_AirMove(void* ctx, float flFrameTime, char a3)
{
	uint32_t* pPressed = nullptr;
	uint32_t  nSaved   = 0;
	uint8_t*  pLockedMove = nullptr;
	float     savedDir[3] = {};

	if (ctx)
	{
		CPlayer* const player = *reinterpret_cast<CPlayer**>(
			reinterpret_cast<uintptr_t>(ctx) + TS_CTX_OFF_PLAYER);
		uint8_t* const mv = *reinterpret_cast<uint8_t**>(
			reinterpret_cast<uintptr_t>(ctx) + TS_CTX_OFF_MOVEDATA);

		// A gravity-cannon flight restricts the wish direction as well as the
		// lurch, so it borrows the same window: this detour is the only place
		// the wish direction can be edited before the engine integrates it.
		if (TriggerCannon_BeginFlightLock(player, mv, savedDir))
			pLockedMove = mv;

		const bool bAboveCutoff = mv && TapStrafe_AboveCutoff(mv);

		if (player && mv && (pLockedMove || bAboveCutoff || MantleBoost_ShouldSuppressTapStrafe(player)))
		{
			pPressed = reinterpret_cast<uint32_t*>(mv + TS_MV_OFF_BUTTONS_PRESSED);
			nSaved   = *pPressed;
			*pPressed = nSaved & ~TS_LURCH_PRESS_MASK;

			if (bridge_tapstrafe_debug.GetBool() && (nSaved & TS_LURCH_PRESS_MASK))
				DevMsg(eDLL_T::SERVER, "[TAPSTRAFE] lurch press suppressed (%s, pressed=0x%X)\n",
					pLockedMove ? "cannon flight" : bAboveCutoff ? "above cutoff speed" : "mantle boost active", nSaved);
		}
	}

	const __int64 ret = v_CGameMovement__AirMove(ctx, flFrameTime, a3);

	if (pPressed)
		*pPressed = nSaved;   // AirMove is the only masked window; later readers see the real press

	if (pLockedMove)
		TriggerCannon_EndFlightLock(pLockedMove, savedDir);

	return ret;
}

void VTapStrafeBridge::GetAdr(void) const
{
	LogFunAdr("CGameMovement::AirMove", v_CGameMovement__AirMove);
}

void VTapStrafeBridge::GetFun(void) const
{
	// CGameMovement::AirMove at. Pattern verified unique in

	Module_FindPattern(g_GameDll, "48 8B C4 48 89 58 ? 55 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 0F 29 70 ? 48 8B D9 48 8B 49")
		.GetPtr(v_CGameMovement__AirMove);

	if (!v_CGameMovement__AirMove)
		Warning(eDLL_T::SERVER, "[TAPSTRAFE] CGameMovement::AirMove pattern unresolved -- mantle-boost lurch suppression disabled\n");
}

void VTapStrafeBridge::Detour(const bool bAttach) const
{
	if (v_CGameMovement__AirMove)
		DetourSetup(&v_CGameMovement__AirMove, &Hook_CGameMovement_AirMove, bAttach);
}

