//=============================================================================//
//
// Purpose: Source ramp launch on the dedicated server. After each move, a
// grounded player whose velocity runs into a sloped floor fast enough is
// released with the clipped velocity, so ramps and kickers throw them into
// the air the way Source does. Off unless sv_slope_launch is set.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "slope_launch.h"
#include "game/shared/slope_launch_math.h"
#include "source_push.h"

// CGameMovement ctx -- server half.
static constexpr ptrdiff_t SL_CTX_OFF_PLAYER = 0x08;
static constexpr ptrdiff_t SL_CTX_OFF_MV     = 0x10;

static constexpr ptrdiff_t SL_MV_OFF_VELOCITY      = 0x130;
static constexpr ptrdiff_t SL_OFF_GROUNDENT        = 0x3C4;
static constexpr ptrdiff_t SL_OFF_GROUNDNORMAL     = 0x5DA4; // m_groundNormal, rewritten by every SetGroundEntity
static constexpr ptrdiff_t SL_OFF_CURRENTCOMMAND   = 0x6578;

static ConVar sv_slope_launch("sv_slope_launch", "0", FCVAR_RELEASE | FCVAR_REPLICATED,
	"Grounded players running into a sloped floor fast enough are launched off it, as in Source.");

static ConVar bridge_slope_launch_diag("bridge_slope_launch_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[SLOPE-LAUNCH] log each launch. Join on cmd= with the client.");

static void (*v_CGameMovement__SetGroundEntity)(void* ctx, void* pTrace) = nullptr;

void SlopeLaunch_AfterFullWalkMove(void* ctx)
{
	if (!sv_slope_launch.GetBool() || !ctx || !v_CGameMovement__SetGroundEntity)
		return;

	uint8_t* const player = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SL_CTX_OFF_PLAYER);
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SL_CTX_OFF_MV);
	if (!player || !mv || *reinterpret_cast<const int*>(player + SL_OFF_GROUNDENT) == -1)
		return;

	float normal[3];
	memcpy(normal, player + SL_OFF_GROUNDNORMAL, sizeof(normal));
	float* const vel = reinterpret_cast<float*>(mv + SL_MV_OFF_VELOCITY);

	float out[3];
	if (!SlopeLaunch_Velocity(vel, SourcePush_GroundPushUsed(ctx), normal, out))
		return;

	v_CGameMovement__SetGroundEntity(ctx, nullptr);
	vel[0] = out[0];
	vel[1] = out[1];
	vel[2] = out[2];

	if (bridge_slope_launch_diag.GetBool())
	{
		const __int64 pCmd = *reinterpret_cast<const __int64*>(player + SL_OFF_CURRENTCOMMAND);
		Msg(eDLL_T::SERVER, "[SLOPE-LAUNCH] cmd=%d n=%.3f %.3f %.3f v=%.1f %.1f %.1f\n",
			pCmd ? *reinterpret_cast<const int*>(pCmd) : -1,
			normal[0], normal[1], normal[2], out[0], out[1], out[2]);
	}
}

void VSlopeLaunch::GetAdr(void) const
{
	LogFunAdr("CGameMovement::SetGroundEntity", v_CGameMovement__SetGroundEntity);
}

void VSlopeLaunch::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 56 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 44 0F 29 A0")
		.GetPtr(v_CGameMovement__SetGroundEntity);

	if (!v_CGameMovement__SetGroundEntity)
		Warning(eDLL_T::SERVER, "[SLOPE-LAUNCH] SetGroundEntity pattern unresolved -- slope launch disabled\n");
}
