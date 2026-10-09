//=============================================================================//
//
// Purpose: script-driven base velocity that also carries a grounded player.
//
// The movement step that applies m_vecBaseVelocity only moves an airborne
// player; on the ground it replaces the value with the ground entity's
// velocity. While a script push is set, that step runs as if the player were
// airborne, so the push moves them without friction whether they walk, stand
// or fly, as a Source trigger_push does.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "base_push.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cmath>
#include <cstring>

static constexpr ptrdiff_t BP_CTX_OFF_PLAYER        = 8;     // CPlayer*
static constexpr ptrdiff_t BP_CTX_OFF_MOVEDATA      = 16;    // CMoveData*
static constexpr ptrdiff_t BP_MV_FLOAT_ORIGIN       = 73;    // float index: origin at +292, velocity follows at +304
static constexpr ptrdiff_t BP_PLAYER_OFF_GROUND     = 0x3C4; // m_hGroundEntity
static constexpr ptrdiff_t BP_PLAYER_OFF_BASEVEL    = 0x3CC; // m_vecBaseVelocity
static constexpr ptrdiff_t BP_PLAYER_OFF_BASEVEL_SRC = 0x3D8; // entity the base velocity follows; -1 = none
static constexpr uint32_t  BP_INVALID_EHANDLE       = 0xFFFFFFFFu;
static constexpr float     BP_MAX_SPEED             = 10000.0f;

static SDKEntityMap<uint8_t> s_pushActive(ESide::Server, "basePush.active");

static void (*v_Player_MoveBaseVelocity)(void* ctx, float* pGroundNormal, float* pGroundDot) = nullptr;
static void (*v_CBaseEntity__SetBaseVelocity)(void* pEntity, const float* pVel) = nullptr;

static ConVar bridge_base_push_diag("bridge_base_push_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[BASE-PUSH] log each grounded push step");

static void Hook_Player_MoveBaseVelocity(void* ctx, float* pGroundNormal, float* pGroundDot)
{
	uint8_t* const pPlayer = ctx ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + BP_CTX_OFF_PLAYER) : nullptr;
	if (!pPlayer || !s_pushActive.Find(pPlayer))
	{
		v_Player_MoveBaseVelocity(ctx, pGroundNormal, pGroundDot);
		return;
	}

	uint32_t* const pGround = reinterpret_cast<uint32_t*>(pPlayer + BP_PLAYER_OFF_GROUND);
	uint32_t* const pSource = reinterpret_cast<uint32_t*>(pPlayer + BP_PLAYER_OFF_BASEVEL_SRC);
	const uint32_t hGround = *pGround;
	const uint32_t hSource = *pSource;

	const float* const pMv = *reinterpret_cast<float* const*>(static_cast<uint8_t*>(ctx) + BP_CTX_OFF_MOVEDATA);
	float before[6] = {};
	if (pMv)
		memcpy(before, pMv + BP_MV_FLOAT_ORIGIN, sizeof(before));

	// With no ground and no source entity the step moves the player by the base
	// velocity and keeps it. Only this step reads the two handles; both are back
	// before anything else runs.
	*pGround = BP_INVALID_EHANDLE;
	*pSource = BP_INVALID_EHANDLE;
	v_Player_MoveBaseVelocity(ctx, pGroundNormal, pGroundDot);
	*pGround = hGround;
	*pSource = hSource;

	if (bridge_base_push_diag.GetBool() && pMv)
	{
		const float* const pBase = reinterpret_cast<const float*>(pPlayer + BP_PLAYER_OFF_BASEVEL);
		const float* const pAfter = pMv + BP_MV_FLOAT_ORIGIN;
		Msg(eDLL_T::SERVER, "[BASE-PUSH] ground=%08X source=%08X base=(%.0f %.0f %.0f) move=(%.1f %.1f %.1f) "
			"vel=(%.0f %.0f %.0f)->(%.0f %.0f %.0f)\n",
			hGround, hSource, pBase[0], pBase[1], pBase[2],
			pAfter[0] - before[0], pAfter[1] - before[1], pAfter[2] - before[2],
			before[3], before[4], before[5], pAfter[3], pAfter[4], pAfter[5]);
	}
}

//-----------------------------------------------------------------------------
// player.SetBasePush( vector push ): sets m_vecBaseVelocity and keeps it
// applied on the ground; a zero vector ends the push.
//-----------------------------------------------------------------------------
static SQRESULT Script_SetBasePush(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	const SQVector3D* pPush = nullptr;
	if (SQ_FAILED(sq_getvector(v, 2, &pPush)) || !pPush)
		return SQ_ERROR;

	float push[3] = { pPush->x, pPush->y, pPush->z };
	for (float& f : push)
	{
		if (!std::isfinite(f))
			f = 0.0f;
		f = fminf(fmaxf(f, -BP_MAX_SPEED), BP_MAX_SPEED);
	}

	if (!v_CBaseEntity__SetBaseVelocity)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[BASE-PUSH] SetBasePush called but the base velocity setter is unresolved\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	v_CBaseEntity__SetBaseVelocity(pPlayer, push);
	if (push[0] != 0.0f || push[1] != 0.0f || push[2] != 0.0f)
		s_pushActive[pPlayer] = 1;
	else
		s_pushActive.Erase(pPlayer);

	static bool s_bLogged = false;
	if (!s_bLogged)
	{
		s_bLogged = true;
		Msg(eDLL_T::SERVER, "[BASE-PUSH] SetBasePush first call player=%p push=(%.0f %.0f %.0f)\n",
			pPlayer, push[0], push[1], push[2]);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void BasePush_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction("SetBasePush", "Script_SetBasePush",
		"Sets the base velocity and keeps it moving the player on the ground; zero ends it",
		"void", "vector push", false, Script_SetBasePush);
}

void VBasePush::GetAdr(void) const
{
	LogFunAdr("Player_MoveBaseVelocity", v_Player_MoveBaseVelocity);
	LogFunAdr("CBaseEntity::SetBaseVelocity", v_CBaseEntity__SetBaseVelocity);
}

void VBasePush::GetFun(void) const
{
	// Server half: the movement ctx reads the player at +8 and its ring at +0x77A0.
	Module_FindPattern(g_GameDll,
		"48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D AC 24 70 FE FF FF 48 81 EC 90 02 00 00 4C 8B 49 08 4C 8D 1D")
		.GetPtr(v_Player_MoveBaseVelocity);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 F3 0F 10 02 48 8D 99 CC 03 00 00 0F 2E 03 48 8B FA")
		.GetPtr(v_CBaseEntity__SetBaseVelocity);

	if (!v_Player_MoveBaseVelocity || !v_CBaseEntity__SetBaseVelocity)
		Warning(eDLL_T::SERVER, "[BASE-PUSH] patterns unresolved (move=%d set=%d) -- grounded pushes disabled\n",
			v_Player_MoveBaseVelocity ? 1 : 0, v_CBaseEntity__SetBaseVelocity ? 1 : 0);
}

void VBasePush::Detour(const bool bAttach) const
{
	if (v_Player_MoveBaseVelocity)
		DetourSetup(&v_Player_MoveBaseVelocity, &Hook_Player_MoveBaseVelocity, bAttach);
}
