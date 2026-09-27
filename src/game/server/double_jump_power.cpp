//=============================================================================//
//
// Purpose: Sparrow's double-jump power on the dedicated server
// (game/shared/double_jump_power.h). The client twin is
// game/client/double_jump_power.cpp.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "double_jump_power.h"
#include "game/server/gameinterface.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/double_jump_power.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"

// CGameMovement ctx -- server half.
static constexpr ptrdiff_t DJP_CTX_OFF_PLAYER = 0x08;

// CPlayer -- server half.
static constexpr ptrdiff_t DJP_OFF_GROUNDENT   = 0x3C4;
static constexpr ptrdiff_t DJP_OFF_LOCAL       = 0x5A90; // m_Local; its first slot is the state-changed notifier
static constexpr ptrdiff_t DJP_OFF_JUMPPOWER   = 0x5ACC; // m_Local.m_flSuitJumpPower
static constexpr ptrdiff_t DJP_OFF_REGENSCALE  = 0x5AF4; // m_Local.m_powerRegenRateScale
static constexpr ptrdiff_t DJP_OFF_WALLNORMAL  = 0x662C; // wall normal while on a wall, else +Z

// S3 ConVar: m_pParent at +0x48, value float/int at parent +0x68/+0x6C.
static constexpr ptrdiff_t DJP_CVAR_OFF_PARENT = 0x48;
static constexpr ptrdiff_t DJP_CVAR_OFF_FLOAT  = 0x68;
static constexpr ptrdiff_t DJP_CVAR_OFF_INT    = 0x6C;

static ConVar bridge_double_jump_power("bridge_double_jump_power", "1", FCVAR_RELEASE,
	"Double-jump power (drain, no ground reset, timed refill) for players whose script enabled it. Must match the client.");

static ConVar bridge_double_jump_power_diag("bridge_double_jump_power_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[DJ-POWER] log each jump that changes an enabled player's double-jump power.");

static char (*v_GameMovement_Jump)(void* ctx) = nullptr;
static void (*v_Player_TouchGround)(void* player) = nullptr;
static void (*v_Player_SuitPowerUpdate)(void* player) = nullptr;

static SDKEntityMap<bool> s_enabledMap(ESide::Server, "doubleJumpPower.srv");
static ConVar* s_pMinPowerUse = nullptr;
static ConVar* s_pPowerResetOnGround = nullptr;

static uint8_t* DoubleJumpPower_CvarParent(ConVar*& pVar, const char* const pszName)
{
	if (!pVar && g_pCVar)
		pVar = g_pCVar->FindVar(pszName);
	return pVar ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(pVar) + DJP_CVAR_OFF_PARENT) : nullptr;
}

static bool DoubleJumpPower_IsEnabled(void* const player)
{
	if (!player || !bridge_double_jump_power.GetBool())
		return false;
	const bool* const pEnabled = s_enabledMap.Find(player);
	return pEnabled && *pEnabled;
}

static void DoubleJumpPower_SetPower(uint8_t* const player, const float flPower)
{
	float* const pPower = reinterpret_cast<float*>(player + DJP_OFF_JUMPPOWER);
	if (*pPower == flPower)
		return;

	// Same notify the engine issues before every m_Local write.
	void* const pLocal = player + DJP_OFF_LOCAL;
	(**reinterpret_cast<void(__fastcall***)(void*, void*)>(pLocal))(pLocal, pPower);
	*pPower = flPower;
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
		Msg(eDLL_T::SERVER, "[DJ-POWER] jump player=%p power %.1f -> %.1f\n", player, flBefore, flAfter);
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

	uint8_t* const p = reinterpret_cast<uint8_t*>(player);
	DoubleJumpPower_SetPower(p, DoubleJumpPower_OnTouchGround(*reinterpret_cast<const float*>(p + DJP_OFF_JUMPPOWER)));
}

static void __fastcall Hook_Player_SuitPowerUpdate(void* player)
{
	v_Player_SuitPowerUpdate(player);
	if (!DoubleJumpPower_IsEnabled(player) || !gpGlobals)
		return;

	uint8_t* const p = reinterpret_cast<uint8_t*>(player);
	const bool bOnGround = *reinterpret_cast<const int*>(p + DJP_OFF_GROUNDENT) != -1;
	const bool bOnWall = reinterpret_cast<const float*>(p + DJP_OFF_WALLNORMAL)[2] < 0.999f;
	const float flPower = *reinterpret_cast<const float*>(p + DJP_OFF_JUMPPOWER);
	DoubleJumpPower_SetPower(p, DoubleJumpPower_Recharge(flPower, bOnGround, bOnWall,
		*reinterpret_cast<const float*>(p + DJP_OFF_REGENSCALE), gpGlobals->frameTime));
}

//-----------------------------------------------------------------------------
// Purpose: player.SetDoubleJumpPowerEnabled( bool )
//-----------------------------------------------------------------------------
static SQRESULT Script_SetDoubleJumpPowerEnabled(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQBool bEnabled = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bEnabled)))
		return SQ_ERROR;

	s_enabledMap[pPlayer] = bEnabled != SQFalse;
	if (bEnabled)
		DoubleJumpPower_SetPower(reinterpret_cast<uint8_t*>(pPlayer), DOUBLE_JUMP_POWER_MAX);

	if (bridge_double_jump_power_diag.GetBool())
		Msg(eDLL_T::SERVER, "[DJ-POWER] enabled player=%p %d\n", pPlayer, bEnabled ? 1 : 0);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void DoubleJumpPower_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"SetDoubleJumpPowerEnabled",
		"Script_SetDoubleJumpPowerEnabled",
		"Double jumps drain a full meter that refills over time instead of on landing",
		"void",
		"bool enabled",
		false,
		Script_SetDoubleJumpPowerEnabled);
}

void VDoubleJumpPower::GetAdr(void) const
{
	LogFunAdr("GameMovement_Jump", v_GameMovement_Jump);
	LogFunAdr("Player_TouchGround", v_Player_TouchGround);
	LogFunAdr("Player_SuitPowerUpdate", v_Player_SuitPowerUpdate);
}

void VDoubleJumpPower::GetFun(void) const
{
	// Server-half Jump (same resolve as the dodge rules).
	Module_FindPattern(g_GameDll, "40 55 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 51")
		.GetPtr(v_GameMovement_Jump);

	// Ground touch; wall normal at +0x662C keeps it off the client twin.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 F2 0F 10 89 2C 66 00 00 0F 57 DB 8B 81 34 66 00 00")
		.GetPtr(v_Player_TouchGround);

	// Per-tick suit power update.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 57 48 81 EC 80 00 00 00 8B 91 C4 03 00 00 48 8B D9 0F 29 74 24 70 0F 57 F6")
		.GetPtr(v_Player_SuitPowerUpdate);

	if (!v_GameMovement_Jump || !v_Player_TouchGround || !v_Player_SuitPowerUpdate)
		Warning(eDLL_T::SERVER, "[DJ-POWER] pattern unresolved (jump=%p ground=%p power=%p); double-jump power disabled\n",
			reinterpret_cast<void*>(v_GameMovement_Jump), reinterpret_cast<void*>(v_Player_TouchGround),
			reinterpret_cast<void*>(v_Player_SuitPowerUpdate));
}

void VDoubleJumpPower::Detour(const bool bAttach) const
{
	if (!v_GameMovement_Jump || !v_Player_TouchGround || !v_Player_SuitPowerUpdate)
		return;

	DetourSetup(&v_GameMovement_Jump, &Hook_GameMovement_Jump, bAttach);
	DetourSetup(&v_Player_TouchGround, &Hook_Player_TouchGround, bAttach);
	DetourSetup(&v_Player_SuitPowerUpdate, &Hook_Player_SuitPowerUpdate, bAttach);
}
