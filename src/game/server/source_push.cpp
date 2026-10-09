//=============================================================================//
//
// Purpose: Source trigger_push on the dedicated server. Map scripts register
// the push volumes on both VMs (SourcePush_AddVolume); every command then runs
// the shared model (game/shared/source_push.h) around FullWalkMove, so the
// client predicts the same push.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "source_push.h"
#include "trigger_cannon.h"
#include "game/shared/source_push.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"

// CGameMovement ctx -- server half.
static constexpr ptrdiff_t SP_CTX_OFF_PLAYER = 0x08;
static constexpr ptrdiff_t SP_CTX_OFF_MV     = 0x10;

static constexpr ptrdiff_t SP_MV_OFF_ORIGIN        = 0x124;
static constexpr ptrdiff_t SP_MV_OFF_VELOCITY      = 0x130;
static constexpr ptrdiff_t SP_OFF_GROUNDENT        = 0x3C4;
static constexpr ptrdiff_t SP_OFF_CURRENTCOMMAND   = 0x6578;

static ConVar sv_source_push_boost("sv_source_push_boost", "1", FCVAR_RELEASE | FCVAR_REPLICATED,
	"1 = each push volume entered steers the player's speed into it and adds its speed (stacking, 3500 cap); "
	"0 = Source trigger_push: carried while inside, the last push kept on leaving.");

static ConVar bridge_source_push_diag("bridge_source_push_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[SOURCE-PUSH] log push volume enter/leave per player. Join on cmd= with the client.");

static void (*v_CGameMovement__SetGroundEntity)(void* ctx, void* pTrace) = nullptr;
static __int64 (*v_GameMovement_UpdateGroundMoveExtra)(void* ctx) = nullptr;

static SourcePushVolumes_t s_volumes;
static SDKEntityMap<SourcePushState_t> s_states(ESide::Server, "sourcePush.srv");

// Movement runs one command at a time; this is the command in progress.
static SourcePushMove_t s_move;
static void* s_pMoveCtx = nullptr;
static const float s_flNoPush[2] = { 0.0f, 0.0f };

static int SourcePush_CmdNumber(const uint8_t* player)
{
	const __int64 pCmd = *reinterpret_cast<const __int64*>(player + SP_OFF_CURRENTCOMMAND);
	return pCmd ? *reinterpret_cast<const int*>(pCmd) : -1;
}

void SourcePush_BeforeFullWalkMove(void* ctx)
{
	s_pMoveCtx = nullptr;
	if (!ctx || s_volumes.m_nCount <= 0)
		return;

	uint8_t* const player = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SP_CTX_OFF_PLAYER);
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SP_CTX_OFF_MV);
	if (!player || !mv)
		return;

	float* const origin = reinterpret_cast<float*>(mv + SP_MV_OFF_ORIGIN);
	float* const vel = reinterpret_cast<float*>(mv + SP_MV_OFF_VELOCITY);
	const bool bGrounded = *reinterpret_cast<const int*>(player + SP_OFF_GROUNDENT) != -1;

	SourcePushState_t& st = s_states[player];
	const bool bWasInside = st.m_bInside;
	const bool bLift = SourcePush_BeforeMove(st, s_move, s_volumes, origin, vel, bGrounded, TriggerPass_FrameTime(), sv_source_push_boost.GetBool());
	s_pMoveCtx = ctx;

	if (bLift && v_CGameMovement__SetGroundEntity)
	{
		v_CGameMovement__SetGroundEntity(ctx, nullptr);
		origin[2] += 1.0f;
	}

	if (bridge_source_push_diag.GetBool() && bWasInside != st.m_bInside)
	{
		Msg(eDLL_T::SERVER, "[SOURCE-PUSH] %s cmd=%d push=%.0f %.0f lift=%d vel=%.1f %.1f %.1f\n",
			st.m_bInside ? "enter" : "leave", SourcePush_CmdNumber(player),
			st.m_bInside ? st.m_flLast[0] : 0.0f, st.m_bInside ? st.m_flLast[1] : 0.0f,
			bLift ? 1 : 0, vel[0], vel[1], vel[2]);
	}
}

void SourcePush_AfterAirAccelerate(void* ctx)
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

const float* SourcePush_GroundPushUsed(void* ctx)
{
	return (ctx && ctx == s_pMoveCtx) ? s_move.m_flGroundUsed : s_flNoPush;
}

void SourcePush_AfterFullWalkMove(void* ctx)
{
	if (!ctx || ctx != s_pMoveCtx)
		return;

	uint8_t* const mv = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + SP_CTX_OFF_MV);
	if (mv)
		SourcePush_AfterMove(s_move, reinterpret_cast<float*>(mv + SP_MV_OFF_VELOCITY));
}

//-----------------------------------------------------------------------------
// Purpose: SourcePush_ClearVolumes() -- drops every push volume
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_SourcePush_ClearVolumes(HSQUIRRELVM v)
{
	s_volumes.m_nCount = 0;
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: SourcePush_AddVolume( vector mins, vector maxs, vector push ) --
// a world-aligned box that pushes every player touching it by push (units/s)
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_SourcePush_AddVolume(HSQUIRRELVM v)
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
		Warning(eDLL_T::SERVER, "[SOURCE-PUSH] volume refused (count=%d, bad value or full)\n", s_volumes.m_nCount);

	sq_pushbool(v, bAdded ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void SourcePush_RegisterServerFunctions(CSquirrelVM* s)
{
	// A new SERVER VM is a new map: its script registers the volumes again.
	s_volumes.m_nCount = 0;
	s_pMoveCtx = nullptr;

	Script_RegisterFuncNamed(s, "SourcePush_ClearVolumes", "Script_SourcePush_ClearVolumes",
		"Removes every Source push volume", "void", "", false, ServerScript_SourcePush_ClearVolumes);
	Script_RegisterFuncNamed(s, "SourcePush_AddVolume", "Script_SourcePush_AddVolume",
		"Adds a world-aligned box that pushes players touching it, as a Source trigger_push. Register the same volumes on the client",
		"bool", "vector mins, vector maxs, vector push", false, ServerScript_SourcePush_AddVolume);
}

void VSourcePush::GetAdr(void) const
{
	LogFunAdr("CGameMovement::SetGroundEntity", v_CGameMovement__SetGroundEntity);
	LogFunAdr("GameMovement_UpdateGroundMoveExtra", v_GameMovement_UpdateGroundMoveExtra);
}

void VSourcePush::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 56 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 44 0F 29 A0")
		.GetPtr(v_CGameMovement__SetGroundEntity);

	// WalkMove calls it once, after friction and acceleration; the player+0x6ECC test and +0x6EE8 store are the unique part.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 20 56 48 81 EC A0 00 00 00 48 8B 51 08 48 8B F1 80 BA CC 6E 00 00 00 75 4B 48 8D 9A E8 6E 00 00")
		.GetPtr(v_GameMovement_UpdateGroundMoveExtra);

	if (!v_CGameMovement__SetGroundEntity || !v_GameMovement_UpdateGroundMoveExtra)
		Warning(eDLL_T::SERVER, "[SOURCE-PUSH] pattern unresolved (ground=%p extra=%p) -- push volumes disabled\n",
			reinterpret_cast<void*>(v_CGameMovement__SetGroundEntity),
			reinterpret_cast<void*>(v_GameMovement_UpdateGroundMoveExtra));
}

void VSourcePush::Detour(const bool bAttach) const
{
	if (v_GameMovement_UpdateGroundMoveExtra)
		DetourSetup(&v_GameMovement_UpdateGroundMoveExtra, &Hook_GameMovement_UpdateGroundMoveExtra, bAttach);
}
