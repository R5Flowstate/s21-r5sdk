//=============================================================================//
//
// Purpose: client prediction twin of the Source trigger_push. The map script
// registers the same volumes on the CLIENT VM as on the dedi, and the local
// player runs the shared model (game/shared/source_push.h) around every
// predicted command. The push state is kept per command so a replayed command
// starts from what the first prediction saw.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/vsquirrel_s21.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/source_push.h"
#include "game/client/pred_authority.h"
#include "source_push.h"

// C_GameMovement ctx.
static constexpr ptrdiff_t SP_CTX_OFF_PLAYER = 0x08;
static constexpr ptrdiff_t SP_CTX_OFF_MV     = 0x10;

static constexpr ptrdiff_t SP_MV_OFF_ORIGIN       = 0x118;
static constexpr ptrdiff_t SP_MV_OFF_VELOCITY     = 0x124;
static constexpr ptrdiff_t SP_OFF_GROUNDENT       = 0x324;
static constexpr ptrdiff_t SP_OFF_CURRENTCOMMAND  = 0x34B8;

static ConVar sv_source_push_boost("sv_source_push_boost", "1", FCVAR_RELEASE | FCVAR_REPLICATED,
	"1 = each push volume entered steers the player's speed into it and adds its speed (stacking, 3500 cap); "
	"0 = Source trigger_push: carried while inside, the last push kept on leaving.");

static ConVar bridge_source_push_diag("bridge_source_push_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[SOURCE-PUSH] log predicted push volume enter/leave. Replays are not logged. Join on cmd= with the dedi.");

static void (*v_GameMovement_SetGroundEntity)(void* ctx, void* pTrace, __int64 nUnused) = nullptr;
static __int64 (*v_GameMovement_UpdateGroundMoveExtra)(void* ctx) = nullptr;

struct SourcePushRing_t
{
	int nCmd = -1;
	SourcePushState_t st;
};

static constexpr int SP_RING_SIZE = 128;
static SourcePushRing_t s_ring[SP_RING_SIZE];
static SourcePushVolumes_t s_volumes;
static SourcePushState_t s_state;
static SourcePushMove_t s_move;
static void* s_pMoveCtx = nullptr;
static int s_nHighWater = -1;
static int s_nCurCmd = -1;
static const float s_flNoPush[2] = { 0.0f, 0.0f };

static int SourcePush_CmdNumber(const uint8_t* player)
{
	const __int64 pCmd = *reinterpret_cast<const __int64*>(player + SP_OFF_CURRENTCOMMAND);
	return pCmd ? *reinterpret_cast<const int*>(pCmd) : -1;
}

static void SourcePush_ResetRing(void)
{
	s_nHighWater = -1;
	for (SourcePushRing_t& e : s_ring)
		e.nCmd = -1;
	s_state = SourcePushState_t();
}

void SourcePushClient_BeforeFullWalkMove(void* ctx)
{
	s_pMoveCtx = nullptr;
	if (!ctx || s_volumes.m_nCount <= 0)
		return;

	uint8_t* const player = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SP_CTX_OFF_PLAYER);
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SP_CTX_OFF_MV);
	if (!player || !mv)
		return;

	s_nCurCmd = SourcePush_CmdNumber(player);
	if (s_nCurCmd < 0)
		return;

	// A new session restarts command numbering far below the old high water.
	if (s_nHighWater - s_nCurCmd > 100000)
		SourcePush_ResetRing();

	const bool bReplay = s_nCurCmd <= s_nHighWater;
	if (bReplay)
	{
		const SourcePushRing_t& prev = s_ring[(s_nCurCmd - 1) & (SP_RING_SIZE - 1)];
		s_state = (prev.nCmd == s_nCurCmd - 1) ? prev.st : SourcePushState_t();
	}

	float* const origin = reinterpret_cast<float*>(mv + SP_MV_OFF_ORIGIN);
	float* const vel = reinterpret_cast<float*>(mv + SP_MV_OFF_VELOCITY);
	const bool bGrounded = *reinterpret_cast<const int*>(player + SP_OFF_GROUNDENT) != -1;

	const bool bWasInside = s_state.m_bInside;
	const bool bLift = SourcePush_BeforeMove(s_state, s_move, s_volumes, origin, vel, bGrounded, PredNative_FrameTime(), sv_source_push_boost.GetBool());
	s_pMoveCtx = ctx;

	if (bLift && v_GameMovement_SetGroundEntity)
	{
		v_GameMovement_SetGroundEntity(ctx, nullptr, 0);
		origin[2] += 1.0f;
	}

	if (!bReplay && bridge_source_push_diag.GetBool() && bWasInside != s_state.m_bInside)
	{
		Msg(eDLL_T::CLIENT, "[SOURCE-PUSH] %s cmd=%d push=%.0f %.0f lift=%d vel=%.1f %.1f %.1f\n",
			s_state.m_bInside ? "enter" : "leave", s_nCurCmd,
			s_state.m_bInside ? s_state.m_flLast[0] : 0.0f, s_state.m_bInside ? s_state.m_flLast[1] : 0.0f,
			bLift ? 1 : 0, vel[0], vel[1], vel[2]);
	}
}

void SourcePushClient_AfterAirAccelerate(void* ctx)
{
	if (!ctx || ctx != s_pMoveCtx)
		return;

	uint8_t* const mv = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SP_CTX_OFF_MV);
	if (mv)
		SourcePush_IntoVelocity(s_move, reinterpret_cast<float*>(mv + SP_MV_OFF_VELOCITY), false);
}

static __int64 __fastcall Hook_GameMovement_UpdateGroundMoveExtra(void* ctx)
{
	const __int64 ret = v_GameMovement_UpdateGroundMoveExtra(ctx);
	// Runs inside WalkMove after friction and acceleration, right before the move.
	if (ctx && ctx == s_pMoveCtx)
	{
		uint8_t* const mv = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SP_CTX_OFF_MV);
		if (mv)
			SourcePush_IntoVelocity(s_move, reinterpret_cast<float*>(mv + SP_MV_OFF_VELOCITY), true);
	}
	return ret;
}

const float* SourcePushClient_GroundPushUsed(void* ctx)
{
	return (ctx && ctx == s_pMoveCtx) ? s_move.m_flGroundUsed : s_flNoPush;
}

void SourcePushClient_AfterFullWalkMove(void* ctx)
{
	if (!ctx || ctx != s_pMoveCtx)
		return;

	uint8_t* const mv = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SP_CTX_OFF_MV);
	if (mv)
		SourcePush_AfterMove(s_move, reinterpret_cast<float*>(mv + SP_MV_OFF_VELOCITY));

	SourcePushRing_t& slot = s_ring[s_nCurCmd & (SP_RING_SIZE - 1)];
	slot.nCmd = s_nCurCmd;
	slot.st = s_state;
	if (s_nCurCmd > s_nHighWater)
		s_nHighWater = s_nCurCmd;
}

//-----------------------------------------------------------------------------
// Purpose: SourcePush_ClearVolumes() -- drops every push volume
//-----------------------------------------------------------------------------
static SQRESULT ClientScript_SourcePush_ClearVolumes(HSQUIRRELVM v)
{
	s_volumes.m_nCount = 0;
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: SourcePush_AddVolume( vector mins, vector maxs, vector push ) --
// the local prediction of the dedi's push volume of the same name
//-----------------------------------------------------------------------------
static SQRESULT ClientScript_SourcePush_AddVolume(HSQUIRRELVM v)
{
	const SQVector3D* pMins = nullptr;
	const SQVector3D* pMaxs = nullptr;
	const SQVector3D* pPush = nullptr;
	if (SQ_FAILED(sq_getvector(v, 2, &pMins)) || SQ_FAILED(sq_getvector(v, 3, &pMaxs)) ||
		SQ_FAILED(sq_getvector(v, 4, &pPush)) || !pMins || !pMaxs || !pPush)
	{
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const float mins[3] = { pMins->x, pMins->y, pMins->z };
	const float maxs[3] = { pMaxs->x, pMaxs->y, pMaxs->z };
	const float push[3] = { pPush->x, pPush->y, pPush->z };
	const bool bAdded = SourcePush_AddVolume(s_volumes, mins, maxs, push);
	if (!bAdded)
		Warning(eDLL_T::CLIENT, "[SOURCE-PUSH] volume refused (count=%d, bad value or full)\n", s_volumes.m_nCount);

	sq_pushbool(v, bAdded ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void SourcePushClient_RegisterClientFunctions(CSquirrelVM* s)
{
	if (!s)
		return;

	// A new CLIENT VM is a new map: its script registers the volumes again.
	s_volumes.m_nCount = 0;
	s_pMoveCtx = nullptr;
	SourcePush_ResetRing();

	if (Script_RegisterFuncTC_S21(s, "SourcePush_ClearVolumes",
			reinterpret_cast<void*>(ClientScript_SourcePush_ClearVolumes), "void", "") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[S21-REG] SourcePush_ClearVolumes registration FAILED\n");

	if (Script_RegisterFuncTC_S21(s, "SourcePush_AddVolume",
			reinterpret_cast<void*>(ClientScript_SourcePush_AddVolume), "bool", "vector mins, vector maxs, vector push") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[S21-REG] SourcePush_AddVolume registration FAILED\n");
}

void VSourcePushClient::GetAdr(void) const
{
	LogFunAdr("GameMovement_SetGroundEntity", v_GameMovement_SetGroundEntity);
	LogFunAdr("GameMovement_UpdateGroundMoveExtra", v_GameMovement_UpdateGroundMoveExtra);
}

void VSourcePushClient::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC B0 00 00 00 33 F6 4C 8B F2 48 8B E9 "
		"48 85 D2 74 06 48 8B 7A 60 EB 03 48 8B FE 4C 8B 69 08 48 8D 15 ?? ?? ?? ?? 41 BC FF FF FF FF 41 8B 8D 24 03 00 00")
		.GetPtr(v_GameMovement_SetGroundEntity);

	// WalkMove calls it once, after friction and acceleration; the player+0x322C test and +0x3248 store are the unique part.
	Module_FindPattern(g_GameDll,
		"40 57 48 81 EC A0 00 00 00 48 8B 41 08 48 8B F9 80 B8 2C 32 00 00 00 75 39 F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 80 48 32 00 00")
		.GetPtr(v_GameMovement_UpdateGroundMoveExtra);

	if (!v_GameMovement_SetGroundEntity || !v_GameMovement_UpdateGroundMoveExtra)
		Warning(eDLL_T::CLIENT, "[SOURCE-PUSH] pattern unresolved (ground=%p extra=%p) -- push prediction disabled\n",
			reinterpret_cast<void*>(v_GameMovement_SetGroundEntity),
			reinterpret_cast<void*>(v_GameMovement_UpdateGroundMoveExtra));
}

void VSourcePushClient::Detour(const bool bAttach) const
{
	if (v_GameMovement_UpdateGroundMoveExtra)
		DetourSetup(&v_GameMovement_UpdateGroundMoveExtra, &Hook_GameMovement_UpdateGroundMoveExtra, bAttach);
}
