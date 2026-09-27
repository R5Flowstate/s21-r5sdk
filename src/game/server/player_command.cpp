//====== Copyright � 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose
//
// $NoKeywords: $
//=============================================================================//
#include "core/stdafx.h"
#include <atomic>
#include <cmath>
#include "engine/server/server.h"
#include "engine/client/client.h"

#include "player_command.h"
#include "game/shared/in_buttons.h"
#include "skydive.h"
#include "game/server/energize.h"
#include "game/server/akimbo.h"
#include "game/shared/weapon_heat.h"
#include "game/shared/weapon_script_vars.h"
#include "mantle_boost.h"
#include "bridge_cmd_chain.h"
#include "game/shared/dt_extend.h"


// Last consumed slot-0 usercmd identity for the SendSnapshot ack-trace probe.
std::atomic<int> g_bridgeLastUcmdCmdNumber{ -1 };
std::atomic<int> g_bridgeLastUcmdTickCount{ -1 };

// Per-slot command number of the last usercmd actually executed, not last received.
std::atomic<int> g_bridgeLastExecCmdNumber[MAX_PLAYERS] = {};

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
CPlayerMove::CPlayerMove(void)
{
}

static ConVar sv_autobunnyhopping("sv_autobunnyhopping", "0", FCVAR_RELEASE | FCVAR_REPLICATED | FCVAR_CHEAT, "Players automatically re-jump while holding the jump button.");

//-----------------------------------------------------------------------------
// Purpose: Runs movement commands for the player
// Input: *player -
// *ucmd -
// *moveHelper -
//-----------------------------------------------------------------------------
// Wrapper skips and movement-budget drops both skip mantle/energize/exec stamp.
// g_PlayerMove data global; vtable slot 1 is RunCommand (RVA, no code to pattern).
static constexpr ptrdiff_t PMOVE_RVA_G_PLAYERMOVE = 0x23A4B38;

static ConVar bridge_pmove_diag("bridge_pmove_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[PMOVE] one-shot entry probe for CPlayerMove::RunCommand plus a rate-limited "
	"report when a command is dropped by the movement-time budget. 0 = silent.");

void CPlayerMove::StaticRunCommand(CPlayerMove* thisp, CPlayer* player, CUserCmd* ucmd, IMoveHelper* moveHelper)
{
	CmdChain_Bump(CMDCHAIN_WRAPPER_ENTERED);

	// Proves the detour actually reaches our body -- print before any early exit.
	if (bridge_pmove_diag.GetBool())
	{
		static bool s_bEntryLogged = false;
		if (!s_bEntryLogged)
		{
			s_bEntryLogged = true;
			Msg(eDLL_T::SERVER, "[PMOVE] StaticRunCommand entry (first call) edict=%d cmd=%d frametime=%.5f\n",
				player ? static_cast<int>(player->GetEdict()) : -1,
				ucmd ? ucmd->command_number : -1,
				ucmd ? ucmd->frametime : -1.0f);
		}
	}

	// Auto bunny hopping: strip the jump button from the usercmd while the
	// player is airborne. This way, when they land, the engine sees IN_JUMP
	// appear as a fresh press and triggers a new jump automatically.
	if (sv_autobunnyhopping.GetBool() && (ucmd->buttons & IN_JUMP))
	{
		if (!(player->GetFlags() & FL_ONGROUND))
			ucmd->buttons &= ~IN_JUMP;
	}

	CClientExtended* const cle = g_pServer->GetClientExtended(player->GetEdict() - 1);
	float playerFrameTime;

	// Non-finite wire frametime would poison the movement budget (NaN clears no
	// comparison, so the drop check never fires). Scrub to tick unconditionally.
	if (!isfinite(ucmd->frametime))
	{
		static uint32_t s_nNonFiniteFt = 0;
		if (++s_nNonFiniteFt <= 8)
			Warning(eDLL_T::SERVER, "[PMOVE] non-finite frametime -- clamped to tick\n");
		ucmd->frametime = TICK_INTERVAL;
	}
	
	// Always default to clamped UserCmd frame time if this cvar is set
	if (player_disallow_negative_frametime->GetBool())
		playerFrameTime = fmaxf(ucmd->frametime, 0.0f);
	else
	{
		if (player->m_bGamePaused)
			playerFrameTime = 0.0f;
		else
			playerFrameTime = TICK_INTERVAL;

		if (ucmd->frametime)
			playerFrameTime = ucmd->frametime;
	}

	if (sv_clampPlayerFrameTime->GetBool() && player->m_joinFrameTime > ((*g_pflServerFrameTimeBase) + playerframetimekick_margin->GetFloat()))
		playerFrameTime = 0.0f;

	const float timeAllowedForProcessing = cle->ConsumeMovementTimeForUserCmdProcessing(playerFrameTime);

	if (!player->IsBot() && (timeAllowedForProcessing < playerFrameTime))
	{
		CmdChain_Bump(CMDCHAIN_WRAPPER_BUDGET_DROP);

		// Dropping here skips the engine call. Counted always; printed at most once a second.
		if (bridge_pmove_diag.GetBool())
		{
			static uint32_t s_nDropped = 0;
			static float s_flLastDropLog = 0.0f;
			const float flNow = static_cast<float>(Plat_FloatTime());

			++s_nDropped;
			if ((flNow - s_flLastDropLog) > 1.0f)
			{
				s_flLastDropLog = flNow;
				Warning(eDLL_T::SERVER,
					"[PMOVE] cmd dropped by movement budget (#%u): allowed=%.5f want=%.5f edict=%d\n",
					s_nDropped, timeAllowedForProcessing, playerFrameTime,
					static_cast<int>(player->GetEdict()));
			}
		}
		return; // Don't process this command
	}

	// Author velocity before movement integrates it. After would be one command behind.
	SkydiveBridge_Think(player, ucmd, playerFrameTime);

	const float flFreezeScale = GameTimescale_WorldScale();
	if (flFreezeScale < 1.0f)
		ucmd->frametime *= flFreezeScale;

	CPlayerMove__RunCommand(thisp, player, ucmd, moveHelper);

	CmdChain_Bump(CMDCHAIN_ENGINE_RETURNED);

	// Energize FSM tick: per executed usercmd, not PlayerRunCommand (no callers).
	EnergizeBridge_Think(player, ucmd);
	AkimboBridge_Think(player, ucmd);

	// Sync laserSightColor userinfo into DT_Player.
	LaserSightColorBridge_Think(player);

	// Publish this consumed cmd's identity for the SendSnapshot ack-trace probe.
	if (player->GetEdict() == 1 && ucmd)
	{
		g_bridgeLastUcmdCmdNumber.store(static_cast<int>(ucmd->command_number), std::memory_order_relaxed);
		g_bridgeLastUcmdTickCount.store(static_cast<int>(ucmd->tick_count), std::memory_order_relaxed);
	}

	// [CMDTICK-EXEC] publish the executed command number for the wire stamp (all slots).
	if (ucmd)
	{
		const int nSlot = static_cast<int>(player->GetEdict()) - 1;
		if (nSlot >= 0 && nSlot < MAX_PLAYERS)
		{
			g_bridgeLastExecCmdNumber[nSlot].store(
				static_cast<int>(ucmd->command_number), std::memory_order_relaxed);
			CmdChain_Bump(CMDCHAIN_STAMP_EXEC);
		}
	}

}

void VPlayerMove::Detour(const bool bAttach) const
{
	// Print resolved target, attach result, and live vtable slot 1.
	const LONG nResult = DetourSetup(&CPlayerMove__RunCommand, &CPlayerMove::StaticRunCommand, bAttach);

	if (bAttach)
	{
		// g_PlayerMove object (S3 S21 layout): the vtable ptr IS the object's first
		// qword; slot 1 is CPlayerMove::RunCommand -- what engine helper dispatches.
		void** const ppPlayerMoveVtbl = *reinterpret_cast<void***>(
			g_GameDll.GetModuleBase() + PMOVE_RVA_G_PLAYERMOVE);

		Msg(eDLL_T::SERVER,
			"[PMOVE-ATTACH] target=%p detour=%p result=%ld vtbl=%p slot1=%p\n",
			reinterpret_cast<void*>(CPlayerMove__RunCommand),
			reinterpret_cast<void*>(&CPlayerMove::StaticRunCommand),
			static_cast<long>(nResult),
			reinterpret_cast<void*>(ppPlayerMoveVtbl),
			ppPlayerMoveVtbl ? ppPlayerMoveVtbl[1] : nullptr);
	}
}
