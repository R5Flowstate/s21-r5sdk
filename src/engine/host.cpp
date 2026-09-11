#if defined(CLIENT_DLL)
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ========//
//
// Purpose
//
// $NoKeywords: $
//=============================================================================//

#include "core/stdafx.h"
#include "engine/cmd.h"
#include "engine/host.h"
#include "client/clientstate.h"

CCommonHostState* g_pCommonHostState = nullptr;

void CCommonHostState::SetWorldModel(model_t* pModel)
{
	if (worldmodel == pModel)
		return;

	worldmodel = pModel;
	if (pModel)
	{
		worldbrush = pModel->brush.pShared;
	}
	else
	{
		worldbrush = NULL;
	}
}

void Host_Error(const char* const error, ...)
{
	char buf[1024];
	{/////////////////////////////
		va_list args{};
		va_start(args, error);

		const int ret = V_vsnprintf(buf, sizeof(buf), error, args);

		if (ret < 0)
			buf[0] = '\0';

		va_end(args);
	}/////////////////////////////

	Error(eDLL_T::ENGINE, NO_ERROR, "Host_Error: %s", buf);
	if (v_Host_Error)
		v_Host_Error(buf);
}

void Host_ReparseAllScripts()
{
	// NOTE: the following are already called during "reload" or "reconnect".
	//"aisettings_reparse"
	//"aisettings_reparse_client"

	//"damagedefs_reparse"
	//"damagedefs_reparse_client"

	//"playerSettings_reparse"
	//"fx_impact_reparse"

	// Reparse banks.rson
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "miles_reboot", cmd_source_t::kCommandSrcCode);

	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "downloadPlaylists", cmd_source_t::kCommandSrcCode);
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "banlist_reload", cmd_source_t::kCommandSrcCode);

	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "ReloadAimAssistSettings", cmd_source_t::kCommandSrcCode);
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reload_localization", cmd_source_t::kCommandSrcCode);

	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "weapon_reparse", cmd_source_t::kCommandSrcCode);

	// Recompile all UI scripts
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "uiscript_reset", cmd_source_t::kCommandSrcCode);

	bool serverActive = false;

	if (!serverActive && g_pClientState->IsActive())
	{
		// If we hit this code path, we are connected to a remote server,
		// reconnect to it to recompile all client side scripts.
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reconnect", cmd_source_t::kCommandSrcCode);
	}

	Cbuf_Execute();
}
#else // !CLIENT_DLL
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ========//
//
// Purpose
//
// $NoKeywords: $
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/frametask.h"
#include "engine/cmd.h"
#include "engine/host.h"
#include "engine/debugoverlay.h"
#include "server/server.h"

CCommonHostState* g_pCommonHostState = nullptr;

void CCommonHostState::SetWorldModel(model_t* pModel)
{
	if (worldmodel == pModel)
		return;

	worldmodel = pModel;
	if (pModel)
	{
		worldbrush = pModel->brush.pShared;
	}
	else
	{
		worldbrush = NULL;
	}
}

/*
==================
_Host_RunFrame

Runs all active servers
==================
*/
#include "game/shared/weapon_enforce.h"
#include "engine/server/snapshot_diag.h"   // WeaponSelectMirror_TickServer
#include "game/shared/dt_extend.h"         // ConnQuality_TickServer
#include "game/shared/heap_canary.h"

void _Host_RunFrame(double realtime, const float deltaTime)
{
	// Per-tick canary poll: catches the moment any registered expansion
	// buffer's tail gets stomped, before the eventual mspace_malloc AV.
	// Cheap (one qword cmp per entry); latches silent on first detection.
	HeapCanary::PollTick();

	// Engine-driven weapon switches (V-key melee, slot keys, TAB cycle)
	// bypass Script_DisableWeaponTypes. Per-tick sweep catches them within
	// 1 tick.
	WeaponEnforce_TickAllServer();

	// [WEAP-SEL-MIRROR] expire mirrored m_selectedWeapons pending selections
	// back to -1 once the S21 client has had a snapshot to latch them.
	WeaponSelectMirror_TickServer();

	// Recompute DT_Player.connectionQualityIndex from each client's netchan.
	// Self-throttled to sdk_conn_quality_interval.
	ConnQuality_TickServer(deltaTime);

	// Freeze keeps CServer.m_flTimescale at 1.0 so remainder still produces
	// ticks; the GNR wire carries bridge_world_timescale instead.
	GameTimescale_TickServer();

	for (IFrameTask* const& task : g_TaskQueueList)
	{
		task->RunFrame();
	}

	g_TaskQueueList.erase(std::remove_if(g_TaskQueueList.begin(), g_TaskQueueList.end(), [](const IFrameTask* task)
		{
			return task->IsFinished();
		}), g_TaskQueueList.end());


	DebugOverlay_HandleDecayed();

	v_Host_RunFrame(realtime, deltaTime);
}

void Host_Error(const char* const error, ...)
{
	char buf[1024];
	{/////////////////////////////
		va_list args{};
		va_start(args, error);

		const int ret = V_vsnprintf(buf, sizeof(buf), error, args);

		if (ret < 0)
			buf[0] = '\0';

		va_end(args);
	}/////////////////////////////

	Error(eDLL_T::ENGINE, NO_ERROR, "Host_Error: %s", buf);
	v_Host_Error(buf);
}

void Host_ReparseAllScripts()
{
	// NOTE: the following are already called during "reload" or "reconnect".
	//"aisettings_reparse"
	//"aisettings_reparse_client"

	//"damagedefs_reparse"
	//"damagedefs_reparse_client"

	//"playerSettings_reparse"
	//"fx_impact_reparse"


	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "downloadPlaylists", cmd_source_t::kCommandSrcCode);
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "banlist_reload", cmd_source_t::kCommandSrcCode);

	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "ReloadAimAssistSettings", cmd_source_t::kCommandSrcCode);
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reload_localization", cmd_source_t::kCommandSrcCode);

	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "weapon_reparse", cmd_source_t::kCommandSrcCode);


	bool serverActive = false;

	if (g_pServer->IsActive())
	{
		// If we hit this code path, we are the server (or the listen server),
		// reload it to recompile all scripts.
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reload", cmd_source_t::kCommandSrcCode);
		serverActive = true;
	}

	Cbuf_Execute();
}

///////////////////////////////////////////////////////////////////////////////
void VHost::Detour(const bool bAttach) const
{
	DetourSetup(&v_Host_RunFrame, &_Host_RunFrame, bAttach);

}
#endif // CLIENT_DLL
