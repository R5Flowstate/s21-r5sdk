//=============================================================================//
//
// Purpose: client prediction twin of the Source ramp launch. Runs the same
// post-move check as the dedi (game/shared/slope_launch_math.h) on the local
// player; it reads only state the move itself produced, so a replayed command
// launches exactly where the first prediction did.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "slope_launch.h"
#include "game/shared/slope_launch_math.h"
#include "source_push.h"

// C_GameMovement ctx.
static constexpr ptrdiff_t SL_CTX_OFF_PLAYER = 0x08;
static constexpr ptrdiff_t SL_CTX_OFF_MV     = 0x10;

static constexpr ptrdiff_t SL_MV_OFF_VELOCITY    = 0x124;
static constexpr ptrdiff_t SL_OFF_GROUNDENT      = 0x324;
static constexpr ptrdiff_t SL_OFF_GROUNDNORMAL   = 0x2080; // m_groundNormal, rewritten by every SetGroundEntity
static constexpr ptrdiff_t SL_OFF_CURRENTCOMMAND = 0x34B8;

static ConVar sv_slope_launch("sv_slope_launch", "0", FCVAR_RELEASE | FCVAR_REPLICATED,
	"Grounded players running into a sloped floor fast enough are launched off it, as in Source.");

static ConVar bridge_slope_launch_diag("bridge_slope_launch_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[SLOPE-LAUNCH] log each predicted launch, replays included. Join on cmd= with the dedi.");

static void (*v_GameMovement_SetGroundEntity)(void* ctx, void* pTrace, __int64 nUnused) = nullptr;

void SlopeLaunchClient_AfterFullWalkMove(void* ctx)
{
	if (!sv_slope_launch.GetBool() || !ctx || !v_GameMovement_SetGroundEntity)
		return;

	uint8_t* const player = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SL_CTX_OFF_PLAYER);
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SL_CTX_OFF_MV);
	if (!player || !mv || *reinterpret_cast<const int*>(player + SL_OFF_GROUNDENT) == -1)
		return;

	float normal[3];
	memcpy(normal, player + SL_OFF_GROUNDNORMAL, sizeof(normal));
	float* const vel = reinterpret_cast<float*>(mv + SL_MV_OFF_VELOCITY);

	float out[3];
	if (!SlopeLaunch_Velocity(vel, SourcePushClient_GroundPushUsed(ctx), normal, out))
		return;

	v_GameMovement_SetGroundEntity(ctx, nullptr, 0);
	vel[0] = out[0];
	vel[1] = out[1];
	vel[2] = out[2];

	if (bridge_slope_launch_diag.GetBool())
	{
		const __int64 pCmd = *reinterpret_cast<const __int64*>(player + SL_OFF_CURRENTCOMMAND);
		Msg(eDLL_T::CLIENT, "[SLOPE-LAUNCH] cmd=%d n=%.3f %.3f %.3f v=%.1f %.1f %.1f\n",
			pCmd ? *reinterpret_cast<const int*>(pCmd) : -1,
			normal[0], normal[1], normal[2], out[0], out[1], out[2]);
	}
}

void VSlopeLaunchClient::GetAdr(void) const
{
	LogFunAdr("GameMovement_SetGroundEntity", v_GameMovement_SetGroundEntity);
}

void VSlopeLaunchClient::GetFun(void) const
{
	// The ground-handle read at player+0x324 is the unique part.
	Module_FindPattern(g_GameDll,
		"48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC B0 00 00 00 33 F6 4C 8B F2 48 8B E9 "
		"48 85 D2 74 06 48 8B 7A 60 EB 03 48 8B FE 4C 8B 69 08 48 8D 15 ?? ?? ?? ?? 41 BC FF FF FF FF 41 8B 8D 24 03 00 00")
		.GetPtr(v_GameMovement_SetGroundEntity);

	if (!v_GameMovement_SetGroundEntity)
		Warning(eDLL_T::CLIENT, "[SLOPE-LAUNCH] SetGroundEntity pattern unresolved -- slope launch prediction disabled\n");
}
