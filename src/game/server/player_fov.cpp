//=============================================================================//
//
// Purpose: CPlayer.GetDefaultFOV() server native. See player_fov.h.
//
//=============================================================================//
#include "core/stdafx.h"

#include "player_fov.h"
#include "engine/server/snapshot_diag.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript_server.h"
#include "tier0/dbg.h"
#include "public/tier0/memaddr.h"
#include <cmath>

static constexpr ptrdiff_t kPlayerSettingsBlock = 0x5F08;
static constexpr uint32_t  kSettingsOffCap      = 0x10000;

// Reads the player's cl_fovScale userinfo and clamps it to 1.0..1.7.
static float (__fastcall* v_CPlayer_GetConvarFoVScale)(void* pPlayer) = nullptr;

static uint32_t s_nPlayerFovOff = 0xFFFFFFFFu;
static bool s_bPlayerFovLooked = false;

static float PlayerFov_SettingFov(void* pPlayer)
{
	if (!s_bPlayerFovLooked)
	{
		s_bPlayerFovLooked = true;
		s_nPlayerFovOff = Bridge_LookupPlayerSettingsFieldOffset("player_fov");
		if (s_nPlayerFovOff == 0xFFFFFFFFu || s_nPlayerFovOff >= kSettingsOffCap)
		{
			Warning(eDLL_T::SERVER, "[PLAYER-FOV] player_fov settings field unresolved -- GetDefaultFOV falls back to 70\n");
			s_nPlayerFovOff = 0xFFFFFFFFu;
		}
	}
	if (s_nPlayerFovOff == 0xFFFFFFFFu)
		return 70.0f;

	const uint8_t* const pBlock = *reinterpret_cast<const uint8_t* const*>(
		reinterpret_cast<const uint8_t*>(pPlayer) + kPlayerSettingsBlock);
	if (!pBlock)
		return 70.0f;
	return *reinterpret_cast<const float*>(pBlock + s_nPlayerFovOff);
}

static int PlayerFov_GetDefaultFOV(void* pPlayer)
{
	const float scale = v_CPlayer_GetConvarFoVScale ? v_CPlayer_GetConvarFoVScale(pPlayer) : 1.0f;
	const float fov = floorf(PlayerFov_SettingFov(pPlayer) * scale + 0.5f);
	int result = static_cast<int>(fov);
	if (result > 179)
		result = 179;
	if (result < 1)
		result = 1;

	static LONG s_logged = 0;
	if (InterlockedIncrement(&s_logged) == 1)
		Msg(eDLL_T::SERVER, "[PLAYER-FOV] GetDefaultFOV first call player=%p scale=%.4f fov=%d\n", pPlayer, scale, result);
	return result;
}

static SQRESULT Script_GetDefaultFOV(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	sq_pushinteger(v, PlayerFov_GetDefaultFOV(pPlayer));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void PlayerFov_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"GetDefaultFOV",
		"Script_GetDefaultFOV",
		"Get FOV of the player",
		"int",
		"",
		false,
		Script_GetDefaultFOV);
}

void VPlayerFov::GetAdr(void) const
{
	LogFunAdr("CPlayer::GetConvarFoVScale", v_CPlayer_GetConvarFoVScale);
}

void VPlayerFov::GetFun(void) const
{
	// Server half: reads the edict index at +0x58 and asks the engine for the
	// client's "cl_fovScale" userinfo value.
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 0F BF 41 58 45 33 C0 48 8B 0D ?? ?? ?? ?? 66 83 F8 FF "
		"8B D0 41 0F 44 D0 4C 8D 05 ?? ?? ?? ?? 4C 8B 09 41 FF 91 80 01 00 00")
		.GetPtr(v_CPlayer_GetConvarFoVScale);

	if (!v_CPlayer_GetConvarFoVScale)
		Warning(eDLL_T::SERVER, "[PLAYER-FOV] GetConvarFoVScale pattern unresolved -- GetDefaultFOV ignores cl_fovScale\n");
}
