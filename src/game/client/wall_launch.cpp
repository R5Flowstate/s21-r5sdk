//=============================================================================//
//
// Purpose: client prediction twin of the Sparrow wall launch. Runs the same
// launch and edge-air math as the dedi (game/shared/wall_launch_math.h) for
// the local player, and keeps the launch state per command so a re-predicted
// command starts from what the first prediction saw.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "engine/enginetrace.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/vsquirrel_s21.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/wall_launch_math.h"
#include "wall_launch.h"
#include <cmath>

// C_GameMovement ctx.
static constexpr ptrdiff_t WL_CTX_OFF_PLAYER   = 0x08;
static constexpr ptrdiff_t WL_CTX_OFF_MV       = 0x10;
static constexpr ptrdiff_t WL_CTX_OFF_SETTINGS = 0x18;

// C_MoveData.
static constexpr ptrdiff_t WL_MV_OFF_FORWARD  = 0x3C;
static constexpr ptrdiff_t WL_MV_OFF_MOVEDIR  = 0x78;
static constexpr ptrdiff_t WL_MV_OFF_ORIGIN   = 0x118;
static constexpr ptrdiff_t WL_MV_OFF_VELOCITY = 0x124;

// C_Player.
static constexpr ptrdiff_t WL_OFF_GROUNDENT      = 0x324;
static constexpr ptrdiff_t WL_OFF_SUPERJUMPS     = 0x1DC0; // m_Local.m_superJumpsUsed
static constexpr ptrdiff_t WL_OFF_WALLNORMAL     = 0x2AB0; // wall normal while on a wall, else +Z
static constexpr ptrdiff_t WL_OFF_WALLHANGING    = 0x2BB9;
static constexpr ptrdiff_t WL_OFF_CURRENTCOMMAND = 0x34B8;
static constexpr ptrdiff_t WL_OFF_AIRSPEED       = 0x36CC;
static constexpr ptrdiff_t WL_OFF_AIRACCEL       = 0x36D0;

// SettingsFieldFinder leaves; each holds the field's offset into the settings block.
static constexpr uint32_t WL_RVA_CLIMBFINALJUMPUP = 0x1ABFAA8;
static constexpr uint32_t WL_RVA_AIRSTRAFEENABLED = 0x1AC3A58;
static constexpr uint32_t WL_SETTINGS_OFF_CAP     = 0x100000u;

static ConVar bridge_wall_launch("bridge_wall_launch", "1", FCVAR_RELEASE,
	"Wall launch (climb high jump) prediction for the local player. Must match the dedi.");

static ConVar bridge_wall_launch_diag("bridge_wall_launch_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[WALL-LAUNCH] 1 = log each predicted launch, 2 = also edge-air frames and state clears. "
	"Replays are not logged. Join on cmd=.");

static void (*v_GameMovement_JumpOutOfWallRun)(void* ctx) = nullptr;
static void (*v_GameMovement_AirAccelerate)(void* ctx, float* wishDir, float wishSpeed, float accel, float dt) = nullptr;

struct WallLaunchRing_t
{
	int nCmd = -1;
	WallLaunchState_t st;
};

static constexpr int WL_RING_SIZE = 128;
static WallLaunchRing_t s_ring[WL_RING_SIZE];
static WallLaunchState_t s_state;
static bool s_bEnabled = false;
static int s_nHighWater = -1;
static int s_nCurCmd = -1;
static bool s_bReplay = false;
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
	const __int64 pCmd = *reinterpret_cast<const __int64*>(player + WL_OFF_CURRENTCOMMAND);
	return pCmd ? *reinterpret_cast<const int*>(pCmd) : -1;
}

static bool WallLaunch_FindLedge(const float origin[3], const float wallNormal[3], float* pLedgeZ)
{
	CEngineTraceClient* const pTrace = EngineTrace_GetClient();
	if (!pTrace)
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
	pTrace->TraceRay(ray, WALL_LAUNCH_LEDGE_MASK, &tr);

	if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || tr.plane.normal.z < WALL_LAUNCH_LEDGE_MIN_NORMAL_Z)
		return false;

	*pLedgeZ = tr.endpos.z;
	return true;
}

void WallLaunchClient_BeforeFullWalkMove(void* ctx)
{
	const uint8_t* const player = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_PLAYER) : nullptr;
	if (!player)
		return;

	s_nCurCmd = WallLaunch_CmdNumber(player);
	if (s_nCurCmd < 0)
	{
		s_bReplay = false;
		return;
	}

	// A new session restarts command numbering far below the old high water.
	if (s_nHighWater - s_nCurCmd > 100000)
	{
		s_nHighWater = -1;
		for (WallLaunchRing_t& e : s_ring)
			e.nCmd = -1;
	}

	s_bReplay = s_nCurCmd <= s_nHighWater;
	if (!s_bReplay)
		return;

	const WallLaunchRing_t& prev = s_ring[(s_nCurCmd - 1) & (WL_RING_SIZE - 1)];
	if (prev.nCmd == s_nCurCmd - 1)
		s_state = prev.st;
}

void WallLaunchClient_AfterFullWalkMove(void* ctx)
{
	const uint8_t* const player = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_PLAYER) : nullptr;
	if (!player || s_nCurCmd < 0)
		return;

	s_state.m_bEnabled = s_bEnabled;
	const bool bWasUsed = s_state.m_bUsed, bWasEdge = s_state.m_bEdgeAir;
	const bool bOnGround = *reinterpret_cast<const int*>(player + WL_OFF_GROUNDENT) != -1;
	const bool bOnWall = reinterpret_cast<const float*>(player + WL_OFF_WALLNORMAL)[2] < 0.999f;
	WallLaunch_AfterMove(s_state, bOnGround, bOnWall, *reinterpret_cast<const int*>(player + WL_OFF_SUPERJUMPS));

	if (!s_bReplay && bridge_wall_launch_diag.GetInt() > 1 && (bWasUsed != s_state.m_bUsed || bWasEdge != s_state.m_bEdgeAir))
	{
		Msg(eDLL_T::CLIENT, "[WALL-LAUNCH] clear cmd=%d used=%d edge=%d ground=%d wall=%d\n",
			s_nCurCmd, s_state.m_bUsed ? 1 : 0, s_state.m_bEdgeAir ? 1 : 0, bOnGround ? 1 : 0, bOnWall ? 1 : 0);
	}

	WallLaunchRing_t& slot = s_ring[s_nCurCmd & (WL_RING_SIZE - 1)];
	slot.nCmd = s_nCurCmd;
	slot.st = s_state;
	if (s_nCurCmd > s_nHighWater)
		s_nHighWater = s_nCurCmd;
}

static void __fastcall Hook_GameMovement_JumpOutOfWallRun(void* ctx)
{
	uint8_t* const player = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_PLAYER) : nullptr;
	uint8_t* const mv = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_MV) : nullptr;
	s_state.m_bEnabled = s_bEnabled;
	if (!player || !mv || !s_state.m_bEnabled || !bridge_wall_launch.GetBool())
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

	if (!WallLaunch_ShouldLaunch(s_state, inputDir, wallNormal))
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
	s_state.m_bUsed = true;
	s_state.m_bEdgeAir = true;
	memcpy(s_state.m_flWallNormal, wallNormal, sizeof(wallNormal));

	if (!s_bReplay && bridge_wall_launch_diag.GetInt() > 0)
	{
		Msg(eDLL_T::CLIENT, "[WALL-LAUNCH] launch cmd=%d h=%.1f ledge=%d/%.1f n=%.3f %.3f %.3f v=%.1f %.1f %.1f\n",
			s_nCurCmd, flHeight, bLedge ? 1 : 0, flLedgeZ,
			wallNormal[0], wallNormal[1], wallNormal[2], out[0], out[1], out[2]);
	}
}

static void __fastcall Hook_GameMovement_AirAccelerate(void* ctx, float* wishDir, float wishSpeed, float accel, float dt)
{
	uint8_t* const player = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_PLAYER) : nullptr;
	uint8_t* const mv = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + WL_CTX_OFF_MV) : nullptr;
	if (!player || !mv || !wishDir || !s_state.m_bEdgeAir || !bridge_wall_launch.GetBool())
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
	const bool bEdge = WallLaunch_EdgeAir(s_state, reinterpret_cast<const float*>(mv + WL_MV_OFF_VELOCITY), dir,
		*reinterpret_cast<const float*>(player + WL_OFF_AIRSPEED),
		*reinterpret_cast<const float*>(player + WL_OFF_AIRACCEL), &flWishSpeed, &flAccel);

	if (bEdge && !s_bReplay && bridge_wall_launch_diag.GetInt() > 1)
	{
		Msg(eDLL_T::CLIENT, "[WALL-LAUNCH] edge cmd=%d wish=%.1f->%.1f accel=%.1f->%.1f\n",
			s_nCurCmd, wishSpeed, flWishSpeed, accel, flAccel);
	}

	v_GameMovement_AirAccelerate(ctx, bEdge ? dir : wishDir, flWishSpeed, flAccel, dt);
}

//-----------------------------------------------------------------------------
// Purpose: SetLocalWallLaunchEnabled( bool ) -- the local player's prediction
// of the dedi's player.SetWallLaunchEnabled.
//-----------------------------------------------------------------------------
static SQRESULT ClientScript_SetLocalWallLaunchEnabled(HSQUIRRELVM v)
{
	SQBool b = SQFalse;
	sq_getbool(v, 2, &b);
	s_bEnabled = b != SQFalse;
	s_state.m_bEnabled = s_bEnabled;
	if (!s_bEnabled)
		s_state.m_bEdgeAir = false;

	if (bridge_wall_launch_diag.GetInt() > 0)
		Msg(eDLL_T::CLIENT, "[WALL-LAUNCH] enabled local %d\n", s_bEnabled ? 1 : 0);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: HasLocalUsedWallLaunch() -- predicted launch since the last ground touch
//-----------------------------------------------------------------------------
static SQRESULT ClientScript_HasLocalUsedWallLaunch(HSQUIRRELVM v)
{
	sq_pushbool(v, s_state.m_bUsed ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void WallLaunchClient_RegisterClientFunctions(CSquirrelVM* s)
{
	if (!s)
		return;

	// A new CLIENT VM is a new session: nothing the old script enabled survives it.
	s_bEnabled = false;
	s_state = WallLaunchState_t();

	if (Script_RegisterFuncTC_S21(s, "SetLocalWallLaunchEnabled",
			reinterpret_cast<void*>(ClientScript_SetLocalWallLaunchEnabled), "void", "bool enabled") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[S21-REG] SetLocalWallLaunchEnabled registration FAILED\n");

	if (Script_RegisterFuncTC_S21(s, "HasLocalUsedWallLaunch",
			reinterpret_cast<void*>(ClientScript_HasLocalUsedWallLaunch), "bool", "") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[S21-REG] HasLocalUsedWallLaunch registration FAILED\n");
}

void VWallLaunchClient::GetAdr(void) const
{
	LogFunAdr("GameMovement_JumpOutOfWallRun", v_GameMovement_JumpOutOfWallRun);
	LogFunAdr("GameMovement_AirAccelerate", v_GameMovement_AirAccelerate);
}

void VWallLaunchClient::GetFun(void) const
{
	// Wall jump; the frame layout is the unique part.
	Module_FindPattern(g_GameDll,
		"48 8B C4 53 48 81 EC D0 00 00 00 0F 29 70 E8 48 8B D9 0F 29 78 D8 44 0F 29 40 C8 "
		"44 0F 29 48 B8 44 0F 29 50 A8 44 0F 29 58 98 44 0F 29 60 88")
		.GetPtr(v_GameMovement_JumpOutOfWallRun);

	// Air accelerate, called once from AirMove. The
	// player+0x252C byte test is kept literal.
	Module_FindPattern(g_GameDll,
		"48 83 EC 68 48 8B 41 08 0F 28 E3 44 0F 29 54 24 10 44 0F 28 D2 80 B8 2C 25 00 00 00 0F 85")
		.GetPtr(v_GameMovement_AirAccelerate);

	if (!v_GameMovement_JumpOutOfWallRun || !v_GameMovement_AirAccelerate)
		Warning(eDLL_T::CLIENT, "[WALL-LAUNCH] pattern unresolved (jump-out=%p air-accel=%p); wall launch prediction disabled\n",
			reinterpret_cast<void*>(v_GameMovement_JumpOutOfWallRun), reinterpret_cast<void*>(v_GameMovement_AirAccelerate));
}

void VWallLaunchClient::Detour(const bool bAttach) const
{
	if (!v_GameMovement_JumpOutOfWallRun || !v_GameMovement_AirAccelerate)
		return;

	DetourSetup(&v_GameMovement_JumpOutOfWallRun, &Hook_GameMovement_JumpOutOfWallRun, bAttach);
	DetourSetup(&v_GameMovement_AirAccelerate, &Hook_GameMovement_AirAccelerate, bAttach);
}
