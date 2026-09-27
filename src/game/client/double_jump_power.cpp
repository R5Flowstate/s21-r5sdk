//=============================================================================//
//
// Purpose: client prediction twin of Sparrow's double-jump power
// (game/shared/double_jump_power.h, dedi half game/server/double_jump_power.cpp).
// m_flSuitJumpPower is a predicted field, so a replayed command starts from
// the networked value and rebuilds the same meter the dedi does.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/vsquirrel_s21.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/double_jump_power.h"
#include "game/client/pred_authority.h"
#include "game/client/classvar_natives.h"
#include "double_jump_power.h"

// C_GameMovement ctx.
static constexpr ptrdiff_t DJP_CTX_OFF_PLAYER = 0x08;

// C_Player.
static constexpr ptrdiff_t DJP_OFF_GROUNDENT  = 0x324;
static constexpr ptrdiff_t DJP_OFF_JUMPPOWER  = 0x1DDC; // m_Local.m_flSuitJumpPower
static constexpr ptrdiff_t DJP_OFF_REGENSCALE = 0x1E04; // m_Local.m_powerRegenRateScale
static constexpr ptrdiff_t DJP_OFF_WALLNORMAL = 0x2AB0; // wall normal while on a wall, else +Z

// S21 ConVar: m_pParent at +0x40, value float/int at parent +0x60/+0x64.
static constexpr ptrdiff_t DJP_CVAR_OFF_PARENT = 0x40;
static constexpr ptrdiff_t DJP_CVAR_OFF_FLOAT  = 0x60;
static constexpr ptrdiff_t DJP_CVAR_OFF_INT    = 0x64;

static ConVar bridge_double_jump_power("bridge_double_jump_power", "1", FCVAR_RELEASE,
	"Double-jump power prediction (drain, no ground reset, timed refill) for the local player. Must match the dedi.");

static ConVar bridge_double_jump_power_diag("bridge_double_jump_power_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[DJ-POWER] log each predicted jump that changes the local double-jump power. Replays included.");

static char (*v_GameMovement_Jump)(void* ctx) = nullptr;
static void (*v_Player_TouchGround)(void* player) = nullptr;
static void (*v_Player_SuitPowerUpdate)(void* player) = nullptr;

static bool s_bEnabled = false;
static ConVar* s_pMinPowerUse = nullptr;
static ConVar* s_pPowerResetOnGround = nullptr;

static uint8_t* DoubleJumpPower_CvarParent(ConVar*& pVar, const char* const pszName)
{
	if (!pVar && g_pCVar)
		pVar = g_pCVar->FindVar(pszName);
	return pVar ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(pVar) + DJP_CVAR_OFF_PARENT) : nullptr;
}

static bool DoubleJumpPower_IsEnabled(const void* const player)
{
	return player && s_bEnabled && bridge_double_jump_power.GetBool() && player == ClassVar_LocalPlayer();
}

static char __fastcall Hook_GameMovement_Jump(void* ctx)
{
	uint8_t* const player = ctx ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctx) + DJP_CTX_OFF_PLAYER) : nullptr;
	uint8_t* const pParent = DoubleJumpPower_CvarParent(s_pMinPowerUse, "superjump_min_power_use");
	if (!pParent || !DoubleJumpPower_IsEnabled(player))
		return v_GameMovement_Jump(ctx);

	// Jump gates on, and drains, superjump_min_power_use.
	float* const pMin = reinterpret_cast<float*>(pParent + DJP_CVAR_OFF_FLOAT);
	const float flSaved = *pMin;
	const float flBefore = *reinterpret_cast<const float*>(player + DJP_OFF_JUMPPOWER);
	*pMin = DOUBLE_JUMP_POWER_DRAIN;
	const char result = v_GameMovement_Jump(ctx);
	*pMin = flSaved;

	const float flAfter = *reinterpret_cast<const float*>(player + DJP_OFF_JUMPPOWER);
	if (bridge_double_jump_power_diag.GetBool() && flBefore != flAfter)
		Msg(eDLL_T::CLIENT, "[DJ-POWER] jump power %.1f -> %.1f\n", flBefore, flAfter);
	return result;
}

static void __fastcall Hook_Player_TouchGround(void* player)
{
	uint8_t* const pParent = DoubleJumpPower_CvarParent(s_pPowerResetOnGround, "superjump_powerreset_onground");
	if (!pParent || !DoubleJumpPower_IsEnabled(player))
	{
		v_Player_TouchGround(player);
		return;
	}

	int* const pReset = reinterpret_cast<int*>(pParent + DJP_CVAR_OFF_INT);
	const int nSaved = *pReset;
	*pReset = 0;
	v_Player_TouchGround(player);
	*pReset = nSaved;

	float* const pPower = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + DJP_OFF_JUMPPOWER);
	*pPower = DoubleJumpPower_OnTouchGround(*pPower);
}

static void __fastcall Hook_Player_SuitPowerUpdate(void* player)
{
	v_Player_SuitPowerUpdate(player);
	if (!DoubleJumpPower_IsEnabled(player))
		return;

	uint8_t* const p = reinterpret_cast<uint8_t*>(player);
	const bool bOnGround = *reinterpret_cast<const int*>(p + DJP_OFF_GROUNDENT) != -1;
	const bool bOnWall = reinterpret_cast<const float*>(p + DJP_OFF_WALLNORMAL)[2] < 0.999f;
	float* const pPower = reinterpret_cast<float*>(p + DJP_OFF_JUMPPOWER);
	*pPower = DoubleJumpPower_Recharge(*pPower, bOnGround, bOnWall,
		*reinterpret_cast<const float*>(p + DJP_OFF_REGENSCALE), PredNative_FrameTime());
}

//-----------------------------------------------------------------------------
// Purpose: SetLocalDoubleJumpPowerEnabled( bool ) -- the local player's
// prediction of the dedi's player.SetDoubleJumpPowerEnabled.
//-----------------------------------------------------------------------------
static SQRESULT ClientScript_SetLocalDoubleJumpPowerEnabled(HSQUIRRELVM v)
{
	SQBool b = SQFalse;
	sq_getbool(v, 2, &b);
	s_bEnabled = b != SQFalse;

	if (bridge_double_jump_power_diag.GetBool())
		Msg(eDLL_T::CLIENT, "[DJ-POWER] enabled local %d\n", s_bEnabled ? 1 : 0);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void DoubleJumpPowerClient_RegisterClientFunctions(CSquirrelVM* s)
{
	if (!s)
		return;

	// A new CLIENT VM is a new session: nothing the old script enabled survives it.
	s_bEnabled = false;

	if (Script_RegisterFuncTC_S21(s, "SetLocalDoubleJumpPowerEnabled",
			reinterpret_cast<void*>(ClientScript_SetLocalDoubleJumpPowerEnabled), "void", "bool enabled") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[S21-REG] SetLocalDoubleJumpPowerEnabled registration FAILED\n");
}

void VDoubleJumpPowerClient::GetAdr(void) const
{
	LogFunAdr("GameMovement_Jump", v_GameMovement_Jump);
	LogFunAdr("Player_TouchGround", v_Player_TouchGround);
	LogFunAdr("Player_SuitPowerUpdate", v_Player_SuitPowerUpdate);
}

void VDoubleJumpPowerClient::GetFun(void) const
{
	// Jump (same resolve as the dodge rules).
	Module_FindPattern(g_GameDll,
		"40 55 53 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 4C 8B 41")
		.GetPtr(v_GameMovement_Jump);

	// Ground touch.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 33 FF C6 81 A8 36 00 00 00 89 B9 FC 2A 00 00 48 8B D9 C6 81 EC 2F 00 00 01 E8")
		.GetPtr(v_Player_TouchGround);

	// Per-tick suit power update.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 50 48 8B D9 0F 29 7C 24 30 8B 89 24 03 00 00 83 F9 FF 74 ?? 0F B7 C1 48 8D 15")
		.GetPtr(v_Player_SuitPowerUpdate);

	if (!v_GameMovement_Jump || !v_Player_TouchGround || !v_Player_SuitPowerUpdate)
		Warning(eDLL_T::CLIENT, "[DJ-POWER] pattern unresolved (jump=%p ground=%p power=%p); double-jump power prediction disabled\n",
			reinterpret_cast<void*>(v_GameMovement_Jump), reinterpret_cast<void*>(v_Player_TouchGround),
			reinterpret_cast<void*>(v_Player_SuitPowerUpdate));
}

void VDoubleJumpPowerClient::Detour(const bool bAttach) const
{
	if (!v_GameMovement_Jump || !v_Player_TouchGround || !v_Player_SuitPowerUpdate)
		return;

	DetourSetup(&v_GameMovement_Jump, &Hook_GameMovement_Jump, bAttach);
	DetourSetup(&v_Player_TouchGround, &Hook_Player_TouchGround, bAttach);
	DetourSetup(&v_Player_SuitPowerUpdate, &Hook_Player_SuitPowerUpdate, bAttach);
}
