//=============================================================================//
//
// Purpose: Sparrow wall launch on the dedicated server. Neither engine has
// the climb high jump, so it rides the wall jump and the air-accelerate step
// on both sides; the client twin is game/client/wall_launch.cpp.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "wall_launch.h"
#include "engine/enginetrace.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/wall_launch_math.h"
#include "game/shared/titan_gate.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cmath>

// CGameMovement ctx -- server half.
static constexpr ptrdiff_t WL_CTX_OFF_PLAYER   = 0x08;
static constexpr ptrdiff_t WL_CTX_OFF_MV       = 0x10;
static constexpr ptrdiff_t WL_CTX_OFF_SETTINGS = 0x18;

// CMoveData -- server half.
static constexpr ptrdiff_t WL_MV_OFF_FORWARD  = 0x3C;
static constexpr ptrdiff_t WL_MV_OFF_MOVEDIR  = 0x78;
static constexpr ptrdiff_t WL_MV_OFF_ORIGIN   = 0x124;
static constexpr ptrdiff_t WL_MV_OFF_VELOCITY = 0x130;

// CPlayer -- server half.
static constexpr ptrdiff_t WL_OFF_GROUNDENT   = 0x3C4;
static constexpr ptrdiff_t WL_OFF_SUPERJUMPS  = 0x5AB4; // m_Local.m_superJumpsUsed
static constexpr ptrdiff_t WL_OFF_WALLNORMAL  = 0x662C; // wall normal while on a wall, else +Z
static constexpr ptrdiff_t WL_OFF_WALLHANGING = 0x6699;
static constexpr ptrdiff_t WL_OFF_AIRSPEED    = 0x694C;
static constexpr ptrdiff_t WL_OFF_AIRACCEL    = 0x6950;

// SettingsFieldFinder leaves; each holds the field's offset into the settings block.
static constexpr uint32_t WL_RVA_CLIMBFINALJUMPUP = 0x23877C8;
static constexpr uint32_t WL_RVA_AIRSTRAFEENABLED = 0x2387A38;
static constexpr uint32_t WL_SETTINGS_OFF_CAP     = 0x100000u;

static ConVar bridge_wall_launch("bridge_wall_launch", "1", FCVAR_RELEASE,
	"Wall launch (climb high jump) for players whose script enabled it. Must match the client.");

static ConVar bridge_wall_launch_diag("bridge_wall_launch_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[WALL-LAUNCH] 1 = log each launch, 2 = also edge-air frames and state clears. Join on cmd=.");

static void (*v_GameMovement_JumpOutOfWallRun)(void* ctx) = nullptr;
static void (*v_GameMovement_AirAccelerate)(void* ctx, float* wishDir, float wishSpeed, float accel, float dt) = nullptr;

static SDKEntityMap<WallLaunchState_t> s_wallLaunchMap(ESide::Server, "wallLaunch.srv");
static ConVar* s_pGravityVar = nullptr;

static float WallLaunch_Gravity(void)
{
	if (!s_pGravityVar && g_pCVar)
		s_pGravityVar = g_pCVar->FindVar("sv_gravity");
	const float fl = s_pGravityVar ? s_pGravityVar->GetFloat() : 750.0f;
	return std::isfinite(fl) ? fl : 750.0f;
}

static uint32_t WallLaunch_FinderOff(const uint32_t nRva)
{
	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	const uintptr_t nSize = static_cast<uintptr_t>(g_GameDll.GetModuleSize());
	if (!mod || nRva + sizeof(uint32_t) > nSize)
		return WL_SETTINGS_OFF_CAP;
	return *reinterpret_cast<const uint32_t*>(mod + nRva);
}

static int WallLaunch_CmdNumber(const uint8_t* player)
{
	static constexpr ptrdiff_t WL_OFF_CURRENTCOMMAND = 0x6578;
	const __int64 pCmd = *reinterpret_cast<const __int64*>(player + WL_OFF_CURRENTCOMMAND);
	return pCmd ? *reinterpret_cast<const int*>(pCmd) : -1;
}

static bool WallLaunch_FindLedge(const float origin[3], const float wallNormal[3], float* pLedgeZ)
{
	if (!g_pEngineTraceServer)
		return false;

	const float flLen = sqrtf(wallNormal[0] * wallNormal[0] + wallNormal[1] * wallNormal[1]);
	if (flLen <= 0.0f)
		return false;

	const float x = origin[0] - wallNormal[0] / flLen * WALL_LAUNCH_LEDGE_INSET;
	const float y = origin[1] - wallNormal[1] / flLen * WALL_LAUNCH_LEDGE_INSET;
	const Vector3D start(x, y, origin[2] + WALL_LAUNCH_MAX_HEIGHT + WALL_LAUNCH_EXTRA_HEIGHT);
	const Vector3D end(x, y, origin[2]);

	Ray_t ray(start, end);
	trace_t tr;
	memset(&tr, 0, sizeof(tr));
	g_pEngineTraceServer->TraceRay(ray, WALL_LAUNCH_LEDGE_MASK, &tr);

	if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || tr.plane.normal.z < WALL_LAUNCH_LEDGE_MIN_NORMAL_Z)
		return false;

	*pLedgeZ = tr.endpos.z;
	return true;
}

static void __fastcall Hook_GameMovement_JumpOutOfWallRun(void* ctx)
{
	uint8_t* const player = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_PLAYER) : nullptr;
	uint8_t* const mv = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_MV) : nullptr;
	if (!player || !mv || !bridge_wall_launch.GetBool() || TitanGate_IsTitanPlayer(player))
	{
		v_GameMovement_JumpOutOfWallRun(ctx);
		return;
	}

	WallLaunchState_t* const pState = s_wallLaunchMap.Find(player);
	if (!pState || !pState->m_bEnabled)
	{
		v_GameMovement_JumpOutOfWallRun(ctx);
		return;
	}

	// The jump-out reads these before it moves anything; sample them first.
	float wallNormal[3], inputDir[3];
	memcpy(wallNormal, player + WL_OFF_WALLNORMAL, sizeof(wallNormal));
	WallLaunch_InputDir(reinterpret_cast<const float*>(mv + WL_MV_OFF_MOVEDIR),
		reinterpret_cast<const float*>(mv + WL_MV_OFF_FORWARD),
		*(player + WL_OFF_WALLHANGING) != 0, inputDir);

	v_GameMovement_JumpOutOfWallRun(ctx);

	if (!WallLaunch_ShouldLaunch(*pState, inputDir, wallNormal))
		return;

	const float* const origin = reinterpret_cast<const float*>(mv + WL_MV_OFF_ORIGIN);
	float* const vel = reinterpret_cast<float*>(mv + WL_MV_OFF_VELOCITY);

	const uint8_t* const pSettings = *reinterpret_cast<const uint8_t* const*>(
		reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_SETTINGS);
	const uint32_t nFinalOff = WallLaunch_FinderOff(WL_RVA_CLIMBFINALJUMPUP);
	const float flFinalJumpUp = (pSettings && nFinalOff < WL_SETTINGS_OFF_CAP)
		? *reinterpret_cast<const float*>(pSettings + nFinalOff) : 0.0f;

	float flLedgeZ = 0.0f;
	const bool bLedge = WallLaunch_FindLedge(origin, wallNormal, &flLedgeZ);
	const float flHeight = WallLaunch_Height(bLedge, flLedgeZ, origin[2], flFinalJumpUp);

	float out[3];
	if (!WallLaunch_Velocity(wallNormal, vel, flHeight, WallLaunch_Gravity(), out))
		return;

	vel[0] = out[0]; vel[1] = out[1]; vel[2] = out[2];
	pState->m_bUsed = true;
	pState->m_bEdgeAir = true;
	memcpy(pState->m_flWallNormal, wallNormal, sizeof(wallNormal));

	if (bridge_wall_launch_diag.GetInt() > 0)
	{
		Msg(eDLL_T::SERVER, "[WALL-LAUNCH] launch cmd=%d h=%.1f ledge=%d/%.1f n=%.3f %.3f %.3f v=%.1f %.1f %.1f\n",
			WallLaunch_CmdNumber(player), flHeight, bLedge ? 1 : 0, flLedgeZ,
			wallNormal[0], wallNormal[1], wallNormal[2], out[0], out[1], out[2]);
	}
}

static void __fastcall Hook_GameMovement_AirAccelerate(void* ctx, float* wishDir, float wishSpeed, float accel, float dt)
{
	uint8_t* const player = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_PLAYER) : nullptr;
	uint8_t* const mv = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_MV) : nullptr;
	WallLaunchState_t* const pState = player ? s_wallLaunchMap.Find(player) : nullptr;
	if (!pState || !pState->m_bEdgeAir || !mv || !wishDir || !bridge_wall_launch.GetBool() || TitanGate_IsTitanPlayer(player))
	{
		v_GameMovement_AirAccelerate(ctx, wishDir, wishSpeed, accel, dt);
		return;
	}

	// Class air strafing replaces the air speed term; the edge terms stand down with it.
	const uint8_t* const pSettings = *reinterpret_cast<const uint8_t* const*>(
		reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_SETTINGS);
	const uint32_t nStrafeOff = WallLaunch_FinderOff(WL_RVA_AIRSTRAFEENABLED);
	if (pSettings && nStrafeOff < WL_SETTINGS_OFF_CAP && pSettings[nStrafeOff])
	{
		v_GameMovement_AirAccelerate(ctx, wishDir, wishSpeed, accel, dt);
		return;
	}

	float dir[3] = { wishDir[0], wishDir[1], wishDir[2] };
	float flWishSpeed = wishSpeed, flAccel = accel;
	const bool bEdge = WallLaunch_EdgeAir(*pState, reinterpret_cast<const float*>(mv + WL_MV_OFF_VELOCITY), dir,
		*reinterpret_cast<const float*>(player + WL_OFF_AIRSPEED),
		*reinterpret_cast<const float*>(player + WL_OFF_AIRACCEL), &flWishSpeed, &flAccel);

	if (bEdge && bridge_wall_launch_diag.GetInt() > 1)
	{
		Msg(eDLL_T::SERVER, "[WALL-LAUNCH] edge cmd=%d wish=%.1f->%.1f accel=%.1f->%.1f\n",
			WallLaunch_CmdNumber(player), wishSpeed, flWishSpeed, accel, flAccel);
	}

	v_GameMovement_AirAccelerate(ctx, bEdge ? dir : wishDir, flWishSpeed, flAccel, dt);
}

void WallLaunch_AfterFullWalkMove(void* ctx)
{
	uint8_t* const player = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_PLAYER) : nullptr;
	WallLaunchState_t* const pState = player ? s_wallLaunchMap.Find(player) : nullptr;
	if (!pState || !pState->m_bEnabled)
		return;

	const bool bWasUsed = pState->m_bUsed, bWasEdge = pState->m_bEdgeAir;
	const bool bOnGround = *reinterpret_cast<const int*>(player + WL_OFF_GROUNDENT) != -1;
	const bool bOnWall = reinterpret_cast<const float*>(player + WL_OFF_WALLNORMAL)[2] < 0.999f;
	WallLaunch_AfterMove(*pState, bOnGround, bOnWall, *reinterpret_cast<const int*>(player + WL_OFF_SUPERJUMPS));

	if (bridge_wall_launch_diag.GetInt() > 1 && (bWasUsed != pState->m_bUsed || bWasEdge != pState->m_bEdgeAir))
	{
		Msg(eDLL_T::SERVER, "[WALL-LAUNCH] clear cmd=%d used=%d edge=%d ground=%d wall=%d\n",
			WallLaunch_CmdNumber(player), pState->m_bUsed ? 1 : 0, pState->m_bEdgeAir ? 1 : 0,
			bOnGround ? 1 : 0, bOnWall ? 1 : 0);
	}
}

//-----------------------------------------------------------------------------
// Purpose: player.SetWallLaunchEnabled( bool )
//-----------------------------------------------------------------------------
static SQRESULT Script_SetWallLaunchEnabled(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQBool bEnabled = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bEnabled)))
		return SQ_ERROR;

	WallLaunchState_t& st = s_wallLaunchMap[pPlayer];
	st.m_bEnabled = bEnabled != SQFalse;
	if (!st.m_bEnabled)
		st.m_bEdgeAir = false;

	if (bridge_wall_launch_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[WALL-LAUNCH] enabled player=%p %d\n", pPlayer, st.m_bEnabled ? 1 : 0);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: player.HasUsedWallLaunch() -- launched since the last ground touch
//-----------------------------------------------------------------------------
static SQRESULT Script_HasUsedWallLaunch(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	const WallLaunchState_t* const pState = s_wallLaunchMap.Find(pPlayer);
	sq_pushbool(v, (pState && pState->m_bUsed) ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void WallLaunch_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"SetWallLaunchEnabled",
		"Script_SetWallLaunchEnabled",
		"Lets this player launch up a wall by jumping while climbing into it",
		"void",
		"bool enabled",
		false,
		Script_SetWallLaunchEnabled);

	playerStruct->AddFunction(
		"HasUsedWallLaunch",
		"Script_HasUsedWallLaunch",
		"True once this player has wall launched since last touching the ground",
		"bool",
		"",
		false,
		Script_HasUsedWallLaunch);
}

void VWallLaunch::GetAdr(void) const
{
	LogFunAdr("GameMovement_JumpOutOfWallRun", v_GameMovement_JumpOutOfWallRun);
	LogFunAdr("GameMovement_AirAccelerate", v_GameMovement_AirAccelerate);
}

void VWallLaunch::GetFun(void) const
{
	// Server-half wall jump; the frame layout is the unique part.
	Module_FindPattern(g_GameDll,
		"48 8B C4 53 48 81 EC F0 00 00 00 0F 29 70 D8 48 8B D9 44 0F 29 58 88 45 0F 57 DB")
		.GetPtr(v_GameMovement_JumpOutOfWallRun);

	// Air accelerate, called once from the server AirMove. The
	// player+0x5E5C byte test is kept literal; the client half inlines its twin.
	Module_FindPattern(g_GameDll,
		"48 83 EC 68 48 8B 41 08 0F 28 E3 44 0F 29 54 24 10 44 0F 28 D2 80 B8 5C 5E 00 00 00 0F 85")
		.GetPtr(v_GameMovement_AirAccelerate);

	if (!v_GameMovement_JumpOutOfWallRun || !v_GameMovement_AirAccelerate)
		Warning(eDLL_T::SERVER, "[WALL-LAUNCH] pattern unresolved (jump-out=%p air-accel=%p); wall launch disabled\n",
			reinterpret_cast<void*>(v_GameMovement_JumpOutOfWallRun), reinterpret_cast<void*>(v_GameMovement_AirAccelerate));
}

void VWallLaunch::Detour(const bool bAttach) const
{
	if (!v_GameMovement_JumpOutOfWallRun || !v_GameMovement_AirAccelerate)
		return;

	DetourSetup(&v_GameMovement_JumpOutOfWallRun, &Hook_GameMovement_JumpOutOfWallRun, bAttach);
	DetourSetup(&v_GameMovement_AirAccelerate, &Hook_GameMovement_AirAccelerate, bAttach);
}
