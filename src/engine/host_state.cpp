#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: Runs the state machine for the host & server.
//
//=============================================================================//
// host_state.cpp:.
//
/////////////////////////////////////////////////////////////////////////////////
#include "core/stdafx.h"
#include "tier0/jobthread.h"
#include "tier0/commandline.h"
#include "tier0/fasttimer.h"
#include "tier0/frametask.h"
#include "tier1/cvar.h"
#include "tier1/NetAdr.h"
#include "tier2/socketcreator.h"
#include "datacache/mdlcache.h"
#include "engine/client/cl_rcon.h"
#include "engine/client/cl_main.h"
#include "engine/client/clientstate.h"
#include "engine/cmd.h"
#include "engine/net.h"
#include "engine/gl_screen.h"
#include "engine/host.h"
#include "engine/host_cmd.h"
#include "engine/host_state.h"
#include "engine/sys_engine.h"
#include "engine/modelloader.h"
#include "engine/cmodel_bsp.h"
#include "pluginsystem/modsystem.h"
#include "rtech/stryder/stryder.h"
#include "rtech/playlists/playlists.h"
#include "vgui/vgui_baseui_interface.h"
#include "client/vengineclient_impl.h"
#include "client/cdll_engine_int.h"
#include "gameui/imgui_system.h"
#include "networksystem/spire.h"
#include "networksystem/listmanager.h"
#include "public/edict.h"
#include "game/shared/vscript_shared.h"
#include "game/shared/activity.h"
#include "game/shared/activitymodifier.h"
#include <tier2/fileutils.h>

static void SV_ServerPasswordChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData);
static string SV_HashPasswordTag(const char* const pszPassword);


static ConVar host_sessionId("host_sessionId", "", FCVAR_REPLICATED|FCVAR_DEVELOPMENTONLY, "Host session ID.");
ConVar hostdesc("hostdesc", "", FCVAR_RELEASE, "Host game server description.");
ConVar sv_modsProfile("sv_modsProfile", "", FCVAR_RELEASE, "Thunderstore mods profile identifier.");
static ConVar sv_password("sv_password", "", FCVAR_RELEASE, "Server password for entry.", false, 0.f, false, 0.f, &SV_ServerPasswordChanged_f, nullptr);

static void SV_ServerPasswordChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData)
{
	ConVar* const pPassword = g_pCVar->FindVar(pConVar->GetName());
	ConVar* const pFilter   = g_pCVar->FindVar("serverFilter");
	if (!pPassword || !pFilter)
		return;

	const char* const newPw = pPassword->GetString();
	// Skip if value hasn't actually changed.
	if (pOldString && strcmp(pOldString, newPw) == 0)
		return;

	const string tagged = SV_HashPasswordTag(newPw);
	pFilter->SetValue(tagged.c_str());
}

static string SV_HashPasswordTag(const char* const pszPassword)
{
	if (!pszPassword || !pszPassword[0])
		return string();
	uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszPassword); *p; ++p)
	{
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return Format("pw:%016llx", h);
}



bool HostState_IsTransitioningToLoad()
{
	if (g_pHostState->m_iNextState == HostStates_t::HS_NEW_GAME ||
		g_pHostState->m_iNextState == HostStates_t::HS_LOAD_GAME ||
		g_pHostState->m_iNextState == HostStates_t::HS_CHANGE_LEVEL_SP ||
		g_pHostState->m_iNextState == HostStates_t::HS_CHANGE_LEVEL_MP)
	{
		return true;
	}

	return false;
}

const char* Host_GetSessionID()
{
	return host_sessionId.GetString();
}

static void Host_UpdateSessionID()
{
	host_sessionId.SetValue(g_LogSessionUUID.c_str());
}

//-----------------------------------------------------------------------------
// Purpose: state machine's main processing loop
//-----------------------------------------------------------------------------
void CHostState::FrameUpdate(CHostState* pHostState, double flCurrentTime, float flFrameTime)
{
	static bool bInitialized = false;
	static bool bResetIdleName = false;
	if (!bInitialized)
	{
		g_pHostState->Setup();
		bInitialized = true;
	}

	g_pHostState->Think();
	RCONClient()->RunFrame();
	

	// Disable "warning C4611: interaction between '_setjmp' and C++ object destruction is non-portable"
#pragma warning(push)
#pragma warning(disable : 4611)
	if (setjmp(*host_abortserver))
	{
		g_pHostState->Init();
		return;
	}
#pragma warning(pop)
	else
	{
		while (true)
		{
			Cbuf_Execute();

			const HostStates_t oldState = g_pHostState->m_iCurrentState;
			switch (g_pHostState->m_iCurrentState)
			{
			case HostStates_t::HS_NEW_GAME:
			{
				g_pHostState->State_NewGame();
				break;
			}
			case HostStates_t::HS_CHANGE_LEVEL_SP:
			{
				g_pHostState->State_ChangeLevelSP();
				break;
			}
			case HostStates_t::HS_CHANGE_LEVEL_MP:
			{
				g_pHostState->State_ChangeLevelMP();
				break;
			}
			case HostStates_t::HS_RUN:
			{
				if (!g_pHostState->m_bActiveGame)
				{
					if (bResetIdleName)
					{
						g_pHostState->ResetLevelName();
						bResetIdleName = false;
					}
				}
				else // Reset idle name the next non-active frame.
				{
					bResetIdleName = true;
				}

				CHostState__State_Run(&g_pHostState->m_iCurrentState, flCurrentTime, flFrameTime);
				break;
			}
			case HostStates_t::HS_GAME_SHUTDOWN:
			{
				Msg(eDLL_T::ENGINE, "%s: Shutdown host game\n", __FUNCTION__);
				CHostState__State_GameShutDown(g_pHostState);
				break;
			}
			case HostStates_t::HS_RESTART:
			{
				Msg(eDLL_T::ENGINE, "%s: Restarting state machine\n", __FUNCTION__);
				v_CL_EndMovie();
				v_Stryder_SendOfflineRequest(); // We have hostnames nulled anyway.
				g_pEngine->SetNextState(IEngine::DLL_RESTART);
				break;
			}
			case HostStates_t::HS_SHUTDOWN:
			{
				Msg(eDLL_T::ENGINE, "%s: Shutdown state machine\n", __FUNCTION__);
				v_CL_EndMovie();
				v_Stryder_SendOfflineRequest(); // We have hostnames nulled anyway.
				g_pEngine->SetNextState(IEngine::DLL_CLOSE);
				break;
			}
			default:
			{
				break;
			}
			}

			// only do a single pass at HS_RUN per frame. All other states loop until they reach HS_RUN 
			if (oldState == HostStates_t::HS_RUN && (g_pHostState->m_iNextState != HostStates_t::HS_LOAD_GAME || !single_frame_shutdown_for_reload->GetBool()))
				break;

			// shutting down
			if (oldState == HostStates_t::HS_SHUTDOWN ||
				oldState == HostStates_t::HS_RESTART)
				break;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: state machine initialization
//-----------------------------------------------------------------------------
void CHostState::Init(void)
{
	if (m_iNextState != HostStates_t::HS_SHUTDOWN)
	{
		if (m_iNextState == HostStates_t::HS_GAME_SHUTDOWN)
		{
			CHostState__State_GameShutDown(this);
		}
		else
		{
			if (g_pHLClient)
				g_pHLClient->SetSoundState(0);

			m_iCurrentState = HostStates_t::HS_RUN;
			if (m_iNextState != HostStates_t::HS_SHUTDOWN || !single_frame_shutdown_for_reload->GetInt())
				m_iNextState = HostStates_t::HS_RUN;
		}
	}
	m_flShortFrameTime = 1.0f;
	m_bActiveGame = false;
	m_bRememberLocation = false;
	m_bBackgroundLevel = false;
	m_bWaitingForConnection = false;
	m_levelName[0] = 0;
	m_landMarkName[0] = 0;
	m_mapGroupName[0] = 0;
	m_bSplitScreenConnect = false;
	m_bGameHasShutDownAndFlushedMemory = true;
	m_vecLocation.Init();
	m_angLocation.Init();
	m_iServerState = HostStates_t::HS_NEW_GAME;
}

//-----------------------------------------------------------------------------
// Purpose: state machine setup
//-----------------------------------------------------------------------------
void CHostState::Setup(void) 
{
	g_pHostState->LoadConfig();
	LoadModConfigs();
	MergeModPlaylistsIntoFile(); // Merge mod playlists into base file
	ConVar_PurgeHostNames();


	// Past engine net init: a key installed before this point does not survive.
	NET_EnableKeyInstall();

	// Check if a custom net key was specified via ConVar (e.g., from command line)
	const char* customKey = sv_netkey.GetString();
	if (customKey && customKey[0] != '\0')
	{
		NET_SetKey(customKey);
	}
	else if (CommandLine()->CheckParm("-norandomkey") || !net_useRandomKey.GetBool())
	{
		// Honor +net_useRandomKey 0 / -norandomkey. Do not call
		// NET_GenerateKey -- that helper force-flips the convar to 1.
		if (net_useRandomKey.GetBool())
			net_useRandomKey.SetValue(0); // Change callback installs the default key.
		else
			NET_SetKey(DEFAULT_NET_ENCRYPTION_KEY);
	}
	else
	{
		NET_GenerateKey();
	}


	ResetLevelName();
}

//-----------------------------------------------------------------------------
// Purpose: think
//-----------------------------------------------------------------------------
void CHostState::Think(void) const
{
}

extern void Bridge_ApplyLaunchConVarTokens(void);

static void CC_ApplyLaunchConVars_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	Bridge_ApplyLaunchConVarTokens();
}

static ConCommand sdk_apply_launch_convars("_sdk_apply_launch_convars", CC_ApplyLaunchConVars_f,
	"Re-assert '+<convar> <value>' launch arguments over values set by config files.",
	FCVAR_RELEASE | FCVAR_HIDDEN | FCVAR_DONTRECORD);

//-----------------------------------------------------------------------------
// Purpose: load and execute configuration files
//-----------------------------------------------------------------------------
void CHostState::LoadConfig(void) const
{
	if (CommandLine()->ParmValue("-launcher", 0) < 1) // Launcher level 1 indicates everything is handled from the commandline/launcher.
	{
		if (!CommandLine()->CheckParm("-devsdk"))
		{
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/autoexec.cfg\n", cmd_source_t::kCommandSrcCode);
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/autoexec_client.cfg\n", cmd_source_t::kCommandSrcCode);
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec tools/rcon_client.cfg\n", cmd_source_t::kCommandSrcCode);
		}
		else // Development configs.
		{
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/autoexec_dev.cfg\n", cmd_source_t::kCommandSrcCode);
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/autoexec_client_dev.cfg\n", cmd_source_t::kCommandSrcCode);
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec tools/rcon_client_dev.cfg\n", cmd_source_t::kCommandSrcCode);
		}
		if (CommandLine()->CheckParm("-offline"))
		{
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/offline_client.cfg\n", cmd_source_t::kCommandSrcCode);
		}
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec bind.cfg\n", cmd_source_t::kCommandSrcCode);

		// Queued last so it drains after every exec above (exec inserts its file at
		// the front of the remaining buffer). A shipped cfg assigns values the
		// launcher also passes on the command line; the boot-time re-apply in
		// CVar_Connect runs long before this exec, so the cfg would otherwise win.
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "_sdk_apply_launch_convars\n", cmd_source_t::kCommandSrcCode);
	}
}

void CHostState::LoadModConfigs()
{
	if (!ModSystem()->IsEnabled())
		return;

	ModSystem()->LockModList();
	FOR_EACH_VEC(ModSystem()->GetResolvedModList(), i)
	{
		CModSystem::ModInstance_t* const mod = ModSystem()->GetResolvedModList()[i];
		if (!mod || !mod->IsEnabled())
			continue;

		char dirPath[MAX_PATH];
		Q_snprintf(dirPath, sizeof(dirPath), "%s%s", mod->basePath.String(), "cfg/autoload");
		CUtlVector<CUtlString> cfgFiles;
		AddFilesToList(cfgFiles, dirPath, "cfg", "GAME", '/');

		FOR_EACH_VEC(cfgFiles, fidx)
		{
			const char* filePath = cfgFiles[fidx].String();
			FileHandle_t file = FileSystem()->Open(filePath, "rt", "GAME");
			if (!file)
				continue;

			const ssize_t fileSize = FileSystem()->Size(file);
			if (fileSize <= 0)
			{
				FileSystem()->Close(file);
				continue;
			}

			static constexpr ssize_t kMaxAutoloadCfgBytes = 1 << 20;
			if (fileSize > kMaxAutoloadCfgBytes)
			{
				Warning(eDLL_T::MODSYSTEM,
					"[AUTOLOAD-CFG] skipping oversized autoload config '%s' (%zd bytes)\n",
					filePath, fileSize);
				FileSystem()->Close(file);
				continue;
			}

			std::string buffer;
			buffer.resize(size_t(fileSize));
			const ssize_t bytesRead = FileSystem()->Read(&buffer[0], fileSize, file);
			FileSystem()->Close(file);
			buffer.resize(bytesRead > 0 ? size_t(bytesRead) : 0);

			auto dispatchLine = [&](const char* lineBegin, const char* lineEnd)
			{
				while (lineBegin < lineEnd && (*lineBegin == ' ' || *lineBegin == '\t' || *lineBegin == '\r')) ++lineBegin;
				while (lineEnd > lineBegin && (lineEnd[-1] == '\r' || lineEnd[-1] == ' ' || lineEnd[-1] == '\t')) --lineEnd;
				if (lineBegin >= lineEnd) return;

				std::string cmd(lineBegin, size_t(lineEnd - lineBegin));

				const char* s = cmd.c_str();
				while (*s == ' ' || *s == '\t') ++s;
				const char* e = s;
				while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') ++e;
				std::string name(s, size_t(e - s));

				ConCommandBase* pBase = g_pCVar->FindCommandBase(name.c_str());
				if (!pBase) return;

				cmd.push_back('\n');
				// VCmd is a server-product detour, so v_Cmd_Dispatch is null here.
				// The command was resolved above, so only a real one is queued.
				Cbuf_AddText(Cbuf_GetCurrentPlayer(), cmd.c_str(), cmd_source_t::kCommandSrcCode);
			};

			const char* p = buffer.c_str();
			const char* end = p + buffer.size();
			while (p < end)
			{
				const char* lineStart = p;
				const void* newlinePtr = memchr(p, '\n', size_t(end - p));
				const char* lineEnd = newlinePtr ? static_cast<const char*>(newlinePtr) : end;
				dispatchLine(lineStart, lineEnd);
				p = newlinePtr ? lineEnd + 1 : end;
			}

			char relBuf[MAX_PATH];
			const char* relPtr = filePath;
			const char* basePathStr = mod->basePath.String();
			const size_t baseLen = Q_strlen(basePathStr);
			if (!Q_strnicmp(filePath, basePathStr, baseLen))
				relPtr = filePath + baseLen;
			V_strncpy(relBuf, relPtr, sizeof(relBuf));
			V_FixSlashes(relBuf, '\\');
			const char* autoloadPos = V_stristr(relBuf, "cfg\\autoload\\");
			const char* finalRel = autoloadPos ? autoloadPos : relBuf;
			Msg(eDLL_T::MODSYSTEM, "Autoloaded config: %s (%s)\n", mod->name.String(), finalRel);
		}
	}
	ModSystem()->UnlockModList();
}

//-----------------------------------------------------------------------------
// Purpose: set state machine
// Input: newState - 
// clearNext - 
//-----------------------------------------------------------------------------
void CHostState::SetState(const HostStates_t newState)
{
	m_iCurrentState = newState;

	// If our next state isn't a shutdown, or its a forced shutdown then set
	// next state to run.
	if (m_iNextState != HostStates_t::HS_SHUTDOWN ||
		!host_hasIrreversibleShutdown->GetBool())
	{
		m_iNextState = newState;
	}
}

//-----------------------------------------------------------------------------
// Purpose: shutdown active game
//-----------------------------------------------------------------------------
void CHostState::GameShutDown(void)
{
	if (m_bActiveGame)
	{
		m_bActiveGame = false;
		ResetLevelName();
	}
}

//-----------------------------------------------------------------------------
// Purpose: initialize new game
//-----------------------------------------------------------------------------
void CHostState::State_NewGame(void)
{
	Msg(eDLL_T::ENGINE, "%s: Loading level: '%s'\n", __FUNCTION__, g_pHostState->m_levelName);



	Host_UpdateSessionID();

	// Load custom activity modifiers from scripts/activity_modifier_types.txt (only once)
	static bool s_activityModifiersLoaded = false;
	if (!s_activityModifiersLoaded && IsActivityModifierSystemInitialized())
	{
		LoadCustomActivityModifiersFromFile();
		s_activityModifiersLoaded = true;
	}

	// Load custom activities from scripts/activity_types.txt (only once)
	static bool s_activitiesLoaded = false;
	if (!s_activitiesLoaded && IsActivitySystemInitialized())
	{
		LoadCustomActivitiesFromFile();
		s_activitiesLoaded = true;
	}

	SetState(HostStates_t::HS_RUN);
}

//-----------------------------------------------------------------------------
// Purpose: change singleplayer level
//-----------------------------------------------------------------------------
void CHostState::State_ChangeLevelSP(void)
{
	Msg(eDLL_T::ENGINE, "%s: Changing singleplayer level to: '%s'\n", __FUNCTION__, m_levelName);
	m_flShortFrameTime = 1.5f; // Set frame time.

	if (CModelLoader__Map_IsValid(g_pModelLoader, m_levelName)) // Check if map is valid and if we can start a new game.
	{
		v_Host_ChangeLevel(true, m_levelName, m_mapGroupName); // Call change level as singleplayer level.
		Host_UpdateSessionID();
		SetState(HostStates_t::HS_RUN);
		return;
	}

	// Error(NO_ERROR) returns, so leave a terminal state behind: the host state
	// is still CHANGE_LEVEL_SP here and FrameUpdate would re-enter every frame.
	Error(eDLL_T::ENGINE, NO_ERROR, "%s: Unable to find level: '%s'\n", __FUNCTION__, m_levelName);
	SetState(HostStates_t::HS_RUN);
}

//-----------------------------------------------------------------------------
// Purpose: change multiplayer level
//-----------------------------------------------------------------------------
void CHostState::State_ChangeLevelMP(void)
{
	Msg(eDLL_T::ENGINE, "%s: Changing multiplayer level to: '%s'\n", __FUNCTION__, m_levelName);
	m_flShortFrameTime = 0.5f; // Set frame time.

	if (CModelLoader__Map_IsValid(g_pModelLoader, m_levelName)) // Check if map is valid and if we can start a new game.
	{
		g_pEngineVGui->EnabledProgressBarForNextLoad();
		v_Host_ChangeLevel(false, m_levelName, m_mapGroupName); // Call change level as multiplayer level.
		Host_UpdateSessionID();
		SetState(HostStates_t::HS_RUN);
		return;
	}

	// Error(NO_ERROR) returns, so leave a terminal state behind: the host state
	// is still CHANGE_LEVEL_MP here and FrameUpdate would re-enter every frame.
	Error(eDLL_T::ENGINE, NO_ERROR, "%s: Unable to find level: '%s'\n", __FUNCTION__, m_levelName);
	SetState(HostStates_t::HS_RUN);
}

//-----------------------------------------------------------------------------
// Purpose: resets the level name
//-----------------------------------------------------------------------------
void CHostState::ResetLevelName(void)
{
	static const char* szNoMap = "no_map";
	Q_snprintf(const_cast<char*>(m_levelName), sizeof(m_levelName), "%s", szNoMap);
}

///////////////////////////////////////////////////////////////////////////////
CHostState* g_pHostState = nullptr;

#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: Runs the state machine for the host & server.
//
//=============================================================================//
// host_state.cpp:.
//
/////////////////////////////////////////////////////////////////////////////////
#include "core/stdafx.h"
#include "tier0/jobthread.h"
#include "tier0/commandline.h"
#include "tier0/fasttimer.h"
#include "tier0/frametask.h"
#include "tier1/cvar.h"
#include "tier1/NetAdr.h"
#include "tier2/socketcreator.h"
#include "datacache/mdlcache.h"
#include "engine/server/sv_rcon.h"
#include "engine/server/server.h"
#include "engine/cmd.h"
#include "engine/cmd_frame_queue.h"
#include "engine/net.h"
#include "engine/gl_screen.h"
#include "engine/host.h"
#include "engine/host_cmd.h"
#include "engine/host_state.h"
#include "engine/sys_engine.h"
#include "engine/modelloader.h"
#include "engine/cmodel_bsp.h"
#include "engine/server/server.h"
#include "engine/server/precache_natives.h"
#include "engine/server/snapshot_diag.h"
#include "engine/net_chan.h"
#include "engine/server/mod_policy_gate.h"
#include "pluginsystem/modsystem.h"
#include "rtech/stryder/stryder.h"
#include "rtech/playlists/playlists.h"
#include "networksystem/spire.h"
#include "networksystem/bansystem.h"
#include "networksystem/hostmanager.h"
#include "networksystem/listmanager.h"
#include "public/edict.h"
#include "game/server/gameinterface.h"
#include "game/shared/vscript_shared.h"
#include "game/shared/activity.h"
#include "game/shared/activity_s3_to_s21.h"  // S3->S21 activity ID map
#include "game/shared/activitymodifier.h"
#include <tier2/fileutils.h>

static void SV_ServerPasswordChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData);
static string SV_HashPasswordTag(const char* const pszPassword);

static ConVar host_statusRefreshRate("host_statusRefreshRate", "0.5", FCVAR_RELEASE, "Host status refresh rate (seconds).", true, 0.f, false, 0.f);

static ConVar host_autoReloadRate("host_autoReloadRate", "0", FCVAR_RELEASE, "Time in seconds between each auto-reload (disabled if null).");
static ConVar host_autoReloadRespectGameState("host_autoReloadRespectGameState", "0", FCVAR_RELEASE, "Check the game state before proceeding to auto-reload (don't reload in the middle of a match).");

static ConVar host_sessionId("host_sessionId", "", FCVAR_REPLICATED|FCVAR_DEVELOPMENTONLY, "Host session ID.");
ConVar hostdesc("hostdesc", "", FCVAR_RELEASE, "Host game server description.");
ConVar sv_modsProfile("sv_modsProfile", "", FCVAR_RELEASE, "Thunderstore mods profile identifier.");
ConVar sv_requiredMods("sv_requiredMods", "", FCVAR_RELEASE, "Comma separated mod ids that a client MUST have enabled.");
ConVar sv_allowedMods("sv_allowedMods", "", FCVAR_RELEASE, "Comma separated mod ids that a client MAY additionally have.");
ConVar sv_modPolicy("sv_modPolicy", "1", FCVAR_RELEASE, "0 = off, 1 = enforce required, 2 = enforce required + allowlist.", true, 0.f, true, 2.f);
static ConVar sv_password("sv_password", "", FCVAR_RELEASE, "Server password for entry.", false, 0.f, false, 0.f, &SV_ServerPasswordChanged_f, nullptr);

static ConVar sv_signonLadderPump("sv_signonLadderPump", "1", FCVAR_RELEASE,
	"Re-arm a connected client's stalled signon rung so a lost handshake echo cannot park it on the loading screen.");
static ConVar sv_signonLadderStallMs("sv_signonLadderStallMs", "6000", FCVAR_RELEASE,
	"Milliseconds without signon progress before the ladder pump re-arms CONNECTED. Must exceed the client's signon DataBlock processing time.");
static ConVar sv_signonLadderMaxNudges("sv_signonLadderMaxNudges", "2", FCVAR_RELEASE,
	"Give up after this many re-arms for one client. CONNECTED re-arms m_bSendServerInfo; rungs 3-7 re-send the parked NET_SignonState.");
static ConVar sv_signonParkReportMs("sv_signonParkReportMs", "10000", FCVAR_RELEASE,
	"Milliseconds between [SIGNON-PUMP] reports for a client parked above CONNECTED. Each report carries the inbound packet delta, so a deaf channel is distinguishable from a slow client.");

static constexpr int kModPolicyListMax = 64;

static bool ModPolicy_IdValid(const char* const pszId)
{
	if (!pszId || !pszId[0])
		return false;

	const size_t nLen = V_strlen(pszId);
	if (nLen < 4 || nLen > 32)
		return false;

	const char chFirst = pszId[0];
	if (!((chFirst >= 'A' && chFirst <= 'Z') || (chFirst >= 'a' && chFirst <= 'z')))
		return false;

	return strspn(pszId, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._") == nLen;
}

static void ModPolicy_ParseCsv(const char* const pszCsv, CUtlVector<CUtlString>& out)
{
	out.RemoveAll();
	if (!pszCsv || !pszCsv[0])
		return;

	const size_t nLen = V_strlen(pszCsv);
	size_t nStart = 0;
	for (size_t i = 0; i <= nLen; ++i)
	{
		if (i < nLen && pszCsv[i] != ',')
			continue;

		size_t nLo = nStart;
		size_t nHi = i;
		nStart = i + 1;

		while (nLo < nHi && (pszCsv[nLo] == ' ' || pszCsv[nLo] == '\t'))
			++nLo;
		while (nHi > nLo && (pszCsv[nHi - 1] == ' ' || pszCsv[nHi - 1] == '\t'))
			--nHi;
		if (nLo >= nHi)
			continue;

		if (out.Count() >= kModPolicyListMax)
		{
			Warning(eDLL_T::SERVER, "[MOD-POLICY] list capped at %d\n", kModPolicyListMax);
			return;
		}

		const size_t nTok = nHi - nLo;
		char szTok[33];
		if (nTok >= sizeof(szTok))
		{
			Warning(eDLL_T::SERVER, "[MOD-POLICY] dropped malformed id\n");
			continue;
		}

		memcpy(szTok, pszCsv + nLo, nTok);
		szTok[nTok] = '\0';
		if (!ModPolicy_IdValid(szTok))
		{
			Warning(eDLL_T::SERVER, "[MOD-POLICY] dropped malformed id '%s'\n", szTok);
			continue;
		}

		out.AddToTail(szTok);
	}
}

static void ModPolicy_CopyFileList(const CUtlVector<CUtlString>& src, CUtlVector<CUtlString>& out)
{
	out.RemoveAll();
	const int nCount = src.Count();
	const int nCap = (nCount < kModPolicyListMax) ? nCount : kModPolicyListMax;
	for (int i = 0; i < nCap; ++i)
		out.AddToTail(src[i]);

	if (nCount > kModPolicyListMax)
		Warning(eDLL_T::SERVER, "[MOD-POLICY] list capped at %d (had %d)\n", kModPolicyListMax, nCount);
}

void ModPolicy_GetEffectiveRequired(CUtlVector<CUtlString>& out)
{
	out.RemoveAll();
	const char* const pszCsv = sv_requiredMods.GetString();
	if (pszCsv && pszCsv[0])
	{
		ModPolicy_ParseCsv(pszCsv, out);
		return;
	}

	if (!ModSystem()->IsEnabled())
		return;

	ModSystem()->LockModList();
	ModPolicy_CopyFileList(ModSystem()->GetRequiredMods(), out);
	ModSystem()->UnlockModList();
}

void ModPolicy_GetEffectiveAllowed(CUtlVector<CUtlString>& out)
{
	out.RemoveAll();
	const char* const pszCsv = sv_allowedMods.GetString();
	if (pszCsv && pszCsv[0])
	{
		ModPolicy_ParseCsv(pszCsv, out);
		return;
	}

	if (!ModSystem()->IsEnabled())
		return;

	ModSystem()->LockModList();
	ModPolicy_CopyFileList(ModSystem()->GetAllowedMods(), out);
	ModSystem()->UnlockModList();
}


static void SV_ServerPasswordChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData)
{
	ConVar* const pPassword = g_pCVar->FindVar(pConVar->GetName());
	ConVar* const pFilter   = g_pCVar->FindVar("serverFilter");
	if (!pPassword || !pFilter)
		return;

	const char* const newPw = pPassword->GetString();
	// Skip if value hasn't actually changed.
	if (pOldString && strcmp(pOldString, newPw) == 0)
		return;

	const string tagged = SV_HashPasswordTag(newPw);
	pFilter->SetValue(tagged.c_str());
}

static string SV_HashPasswordTag(const char* const pszPassword)
{
	if (!pszPassword || !pszPassword[0])
		return string();
	uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszPassword); *p; ++p)
	{
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return Format("pw:%016llx", h);
}

//-----------------------------------------------------------------------------
// Purpose: Send keep alive request to Spire master server.
// Output: Returns true on success, false otherwise.
//-----------------------------------------------------------------------------
static void HostState_KeepAlive()
{
	// IsEnabled is checked here rather than left to PostServerHost: the request
	// would fail with "matchmaking disabled" and reach the Error path below,
	// which is noise on a server deliberately launched with -offline.
	if (!g_pServer->IsActive() || !spire_host_visibility.GetBool() || !g_Spire.IsEnabled())
	{
		return;
	}

	// The master server rejects a listing with an empty name, so without this
	// the only symptom is a server that never shows up in any browser.
	if (!hostname->GetString()[0])
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			Warning(eDLL_T::SERVER, "Not publishing to the master server: 'hostname' is empty. Set it in cfg/system/autoexec_server.cfg.\n");
		}

		return;
	}

	string password = sv_password.GetString();

	const NetGameServer_t gameServer
	{
		hostname->GetString(),
		hostdesc.GetString(),
		spire_host_visibility.GetInt() == ServerVisibility_e::HIDDEN,
		password.length() > 0,
		g_pHostState->m_levelName,
		v_Playlists_GetCurrent(),
		hostip->GetString(),
		hostport->GetInt(),
		g_pNetKey->GetBase64NetKey(),
		SV_HashPasswordTag(password.c_str()), // tag, not plaintext; the master server only checks non-emptiness
		*g_nServerRemoteChecksum,
		SDK_VERSION,
		g_pServer->GetNumClients(),
		gpGlobals->maxClients,
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()
			).count(),
		{},
		{},
		sv_modsProfile.GetString()
	};

	{
		CUtlVector<CUtlString> req;
		CUtlVector<CUtlString> allow;
		ModPolicy_GetEffectiveRequired(req);
		ModPolicy_GetEffectiveAllowed(allow);
		for (int i = 0; i < req.Count(); ++i)
			const_cast<NetGameServer_t&>(gameServer).requiredMods.emplace_back(req[i].String());
		for (int i = 0; i < allow.Count(); ++i)
			const_cast<NetGameServer_t&>(gameServer).allowedMods.emplace_back(allow[i].String());
	}

	std::thread request([&, gameServer]
		{
			string errorMsg;
			string hostToken;
			string hostIp;

			const bool result = g_Spire.PostServerHost(errorMsg, hostToken, hostIp, gameServer);

			// Apply the data the next frame
			g_TaskQueue.Dispatch([result, errorMsg, hostToken, hostIp]
				{
					if (!result)
					{
						if (!errorMsg.empty() && g_ServerHostManager.GetCurrentError().compare(errorMsg) != NULL)
						{
							g_ServerHostManager.SetCurrentError(errorMsg);
							Error(eDLL_T::SERVER, NO_ERROR, "%s\n", errorMsg.c_str());
						}
					}
					else // Attempt to log the token, if there is one.
					{
						if (!hostToken.empty() && g_ServerHostManager.GetCurrentToken().compare(hostToken) != NULL)
						{
							g_ServerHostManager.SetCurrentToken(hostToken);
							Msg(eDLL_T::SERVER, "Published server with token: %s'%s%s%s'\n",
								g_svReset.c_str(), g_svGreyB.c_str(),
								hostToken.c_str(), g_svReset.c_str());
						}
					}

					if (hostIp.length() != 0)
						g_ServerHostManager.SetHostIP(hostIp);

				}, 0);
		}
	);

	request.detach();
}

void HostState_HandleAutoReload()
{
	if (host_autoReloadRate.GetBool())
	{
		if (gpGlobals->curTime > host_autoReloadRate.GetFloat())
		{
			// We should respect the game state, and the game isn't finished yet so
			// don't reload the server now.
			if (host_autoReloadRespectGameState.GetBool() && !g_hostReloadState)
				return;

			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reload\n", cmd_source_t::kCommandSrcCode);
		}
	}
}

bool HostState_IsTransitioningToLoad()
{
	if (g_pHostState->m_iNextState == HostStates_t::HS_NEW_GAME ||
		g_pHostState->m_iNextState == HostStates_t::HS_LOAD_GAME ||
		g_pHostState->m_iNextState == HostStates_t::HS_CHANGE_LEVEL_SP ||
		g_pHostState->m_iNextState == HostStates_t::HS_CHANGE_LEVEL_MP)
	{
		return true;
	}

	return false;
}

const char* Host_GetSessionID()
{
	return host_sessionId.GetString();
}

static void Host_UpdateSessionID()
{
	host_sessionId.SetValue(g_LogSessionUUID.c_str());
}

static void Host_PumpSignonLadder(void);

namespace
{
	// CClient fields not exposed with stable offsets in the SDK header.
	constexpr ptrdiff_t kClientOff_SendServerInfo = 0x369;   // m_bSendServerInfo
	constexpr ptrdiff_t kClientOff_SendSignonData = 0x36A;   // m_bSendSignonData
	constexpr ptrdiff_t kClientOff_SignonState    = 0x3B0;   // m_nSignonState
	constexpr ptrdiff_t kClientOff_DbInFlight     = 0x48848; // DataBlock transfer-in-flight latch

	struct SignonPumpSlot_t
	{
		int       nSignon;
		uint8_t   bSendServerInfo;
		uint8_t   bSendSignonData;
		int       nNudges;
		bool      bGaveUpWarned;
		bool      bSeen;
		ULONGLONG ullStamp;
		ULONGLONG ullNextParkReport;
		int64_t   nParkReportRecvs;
	};

	static SignonPumpSlot_t s_pumpSlots[MAX_PLAYERS] = {};
	static bool             s_pumpDisabled = false;
	static bool             s_pumpOffsetsChecked = false;
} // namespace

static void Host_PumpSignonLadder(void)
{
	if (s_pumpDisabled || !sv_signonLadderPump.GetBool() || !g_pServer)
		return;

	const int nMax = g_pServer->GetMaxClients();
	const int nSlots = (nMax < MAX_PLAYERS) ? nMax : MAX_PLAYERS;
	const ULONGLONG ullNow = GetTickCount64();
	const ULONGLONG ullStallMs = (ULONGLONG)sv_signonLadderStallMs.GetInt();
	const int nMaxNudges = sv_signonLadderMaxNudges.GetInt();

	for (int i = 0; i < nSlots; ++i)
	{
		SignonPumpSlot_t& slot = s_pumpSlots[i];
		CClient* const pClient = g_pServer->GetClient(i);
		if (!pClient || !pClient->IsHumanPlayer() || !pClient->GetNetChan())
		{
			slot = SignonPumpSlot_t{};
			continue;
		}

		char* const pBase = reinterpret_cast<char*>(pClient);
		const int nSignon = *reinterpret_cast<const int*>(pBase + kClientOff_SignonState);

		if (!s_pumpOffsetsChecked)
		{
			s_pumpOffsetsChecked = true;
			const int nApiSignon = (int)pClient->GetSignonState();
			if (nSignon != nApiSignon)
			{
				Warning(eDLL_T::ENGINE,
					"[SIGNON-PUMP] CClient offset self-check FAILED (raw=%d api=%d) -- pump disabled\n",
					nSignon, nApiSignon);
				s_pumpDisabled = true;
				return;
			}
		}

		const uint8_t bSendInfo = *reinterpret_cast<const uint8_t*>(pBase + kClientOff_SendServerInfo);
		const uint8_t bSendData = *reinterpret_cast<const uint8_t*>(pBase + kClientOff_SendSignonData);
		const uint8_t bDbInFlight = *reinterpret_cast<const uint8_t*>(pBase + kClientOff_DbInFlight);

		if (nSignon >= (int)SIGNONSTATE::SIGNONSTATE_FULL)
		{
			slot = SignonPumpSlot_t{};
			continue;
		}

		if (nSignon != slot.nSignon
			|| bSendInfo != slot.bSendServerInfo
			|| bSendData != slot.bSendSignonData)
		{
			// Only CClient::Reconnect walks a connected client backwards. It
			// also rewinds the netchan subchannel base, which the bridge cannot
			// recover from, so name it the moment it happens.
			if (slot.bSeen && nSignon < slot.nSignon)
				Warning(eDLL_T::ENGINE,
					"[SIGNON-PUMP] client[%d] signon REGRESSED %d -> %d (server-side reconnect)\n",
					i, slot.nSignon, nSignon);
			else if (slot.bSeen && nSignon != slot.nSignon)
				Warning(eDLL_T::ENGINE,
					"[SIGNON-PUMP] client[%d] signon %d -> %d (sendInfo=%d sendData=%d)\n",
					i, slot.nSignon, nSignon, (int)bSendInfo, (int)bSendData);

			slot.nSignon = nSignon;
			slot.bSendServerInfo = bSendInfo;
			slot.bSendSignonData = bSendData;
			slot.bSeen = true;
			slot.bGaveUpWarned = false;
			slot.nNudges = 0;
			slot.ullStamp = ullNow;
			slot.ullNextParkReport = 0;
			slot.nParkReportRecvs = 0;
			continue;
		}

		if ((ullNow - slot.ullStamp) < ullStallMs)
			continue;

		if (bDbInFlight != 0)
		{
			slot.ullStamp = ullNow;
			continue;
		}

		if (slot.nNudges >= nMaxNudges)
		{
			if (!slot.bGaveUpWarned)
			{
				slot.bGaveUpWarned = true;
				Warning(eDLL_T::ENGINE,
					"[SIGNON-PUMP] client[%d] stuck at signon=%d after %d re-arms -- giving up\n",
					i, nSignon, slot.nNudges);
			}
			continue;
		}

		// CONNECTED: re-arm ServerInfo. Rungs 3-7: re-send the parked NET_SignonState
		// (CClient +0x369/+0x36A are write-only; SendSignonState is the emit).
		if (nSignon == (int)SIGNONSTATE::SIGNONSTATE_CONNECTED)
		{
			*reinterpret_cast<uint8_t*>(pBase + kClientOff_SendServerInfo) = 1;
			++slot.nNudges;
			const ULONGLONG ullElapsed = ullNow - slot.ullStamp;
			slot.ullStamp = ullNow;
			Warning(eDLL_T::ENGINE,
				"[SIGNON-PUMP] client[%d] parked at signon=%d for %llu ms -- re-armed m_bSendServerInfo (nudge %d)\n",
				i, nSignon, ullElapsed, slot.nNudges);
		}
		else if (ullNow >= slot.ullNextParkReport)
		{
			CNetChan* const pChan = pClient->GetNetChan();
			int64_t nRecvs = 0;
			double flLastRecv = 0.0;
			if (!NetChan_GetInboundLiveness(pChan, &nRecvs, &flLastRecv))
			{
				nRecvs = pChan->GetTotalPackets(FLOW_INCOMING);
				flLastRecv = pChan->GetLastReceivedTime();
			}
			const int64_t nDelta = slot.bGaveUpWarned
				? nRecvs - slot.nParkReportRecvs : -1;
			const double flRecvAge = Plat_FloatTime() - flLastRecv;

			const bool bResend = nSignon >= (int)SIGNONSTATE::SIGNONSTATE_NEW
				&& nSignon < (int)SIGNONSTATE::SIGNONSTATE_FULL
				&& v_CClient_SendSignonState != nullptr
				&& pChan != nullptr;

			if (bResend)
			{
				v_CClient_SendSignonState(pClient);
				const int nSent = pChan->SendDatagram(nullptr);
				++slot.nNudges;
				const ULONGLONG ullElapsed = ullNow - slot.ullStamp;
				slot.ullStamp = ullNow;
				Warning(eDLL_T::ENGINE,
					"[SIGNON-PUMP] client[%d] parked at signon=%d for %llu ms -- re-sent rung %d "
					"(nudge %d sent=%d c2s msgs=%lld delta=%lld lastRecv=%.1fs ago)\n",
					i, nSignon, ullElapsed, nSignon, slot.nNudges, nSent,
					nRecvs, nDelta, flRecvAge);
			}
			else
			{
				Warning(eDLL_T::ENGINE,
					"[SIGNON-PUMP] client[%d] parked at signon=%d for %llu ms -- waiting on the "
					"client, not re-arming (c2s msgs=%lld delta=%lld lastRecv=%.1fs ago)\n",
					i, nSignon, ullNow - slot.ullStamp, nRecvs, nDelta, flRecvAge);
			}

			if (nDelta == 0)
				Warning(eDLL_T::ENGINE,
					"[SIGNON-PUMP] client[%d] NO C2S messages parsed since the last report -- "
					"the client is silent (mid-load or gone), not merely slow to answer\n", i);

			slot.bGaveUpWarned = true;
			slot.nParkReportRecvs = nRecvs;
			slot.ullNextParkReport = ullNow + (ULONGLONG)sv_signonParkReportMs.GetInt();
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: state machine's main processing loop
//-----------------------------------------------------------------------------
void CHostState::FrameUpdate(CHostState* pHostState, double flCurrentTime, float flFrameTime)
{
	static bool bInitialized = false;
	static bool bResetIdleName = false;
	if (!bInitialized)
	{
		g_pHostState->Setup();
		bInitialized = true;
	}

	g_pHostState->Think();
	// A deferred command runs game code, so it dispatches only in HS_RUN --
	// which includes the idle no-map frame, the state 'map' is typed from.
	// A transitional state holds the backlog for the next HS_RUN; only a host
	// that is going away discards it, because nothing would drain it there.
	switch (g_pHostState->m_iCurrentState)
	{
	case HostStates_t::HS_RUN:
		Cmd_RunUnrestrictedQueue();
		break;

	case HostStates_t::HS_SHUTDOWN:
	case HostStates_t::HS_RESTART:
		Cmd_DropUnrestrictedQueue();
		break;

	default:
		break;
	}

	RCONServer()->RunFrame();

	// Disable "warning C4611: interaction between '_setjmp' and C++ object destruction is non-portable"
#pragma warning(push)
#pragma warning(disable : 4611)
	if (setjmp(*host_abortserver))
	{
		g_pHostState->Init();
		return;
	}
#pragma warning(pop)
	else
	{
		*g_bAbortServerSet = true;
		while (true)
		{
			Cbuf_Execute();

			const HostStates_t oldState = g_pHostState->m_iCurrentState;
			switch (g_pHostState->m_iCurrentState)
			{
			case HostStates_t::HS_NEW_GAME:
			{
				g_pHostState->State_NewGame();
				break;
			}
			case HostStates_t::HS_CHANGE_LEVEL_SP:
			{
				g_pHostState->State_ChangeLevelSP();
				break;
			}
			case HostStates_t::HS_CHANGE_LEVEL_MP:
			{
				g_pHostState->State_ChangeLevelMP();
				break;
			}
			case HostStates_t::HS_RUN:
			{
				if (!g_pHostState->m_bActiveGame)
				{
					if (bResetIdleName)
					{
						g_pHostState->ResetLevelName();
						bResetIdleName = false;
					}
				}
				else // Reset idle name the next non-active frame.
				{
					bResetIdleName = true;
				}

				Host_PumpSignonLadder();
				ModPolicyGate_OnFrame();
				CHostState__State_Run(&g_pHostState->m_iCurrentState, flCurrentTime, flFrameTime);
				break;
			}
			case HostStates_t::HS_GAME_SHUTDOWN:
			{
				Msg(eDLL_T::ENGINE, "%s: Shutdown host game\n", __FUNCTION__);
				CHostState__State_GameShutDown(g_pHostState);
				break;
			}
			case HostStates_t::HS_RESTART:
			{
				Msg(eDLL_T::ENGINE, "%s: Restarting state machine\n", __FUNCTION__);
				v_Stryder_SendOfflineRequest(); // We have hostnames nulled anyway.
				g_pEngine->SetNextState(IEngine::DLL_RESTART);
				break;
			}
			case HostStates_t::HS_SHUTDOWN:
			{
				Msg(eDLL_T::ENGINE, "%s: Shutdown state machine\n", __FUNCTION__);
				v_Stryder_SendOfflineRequest(); // We have hostnames nulled anyway.
				g_pEngine->SetNextState(IEngine::DLL_CLOSE);
				break;
			}
			default:
			{
				break;
			}
			}

			// only do a single pass at HS_RUN per frame. All other states loop until they reach HS_RUN 
			if (oldState == HostStates_t::HS_RUN && (g_pHostState->m_iNextState != HostStates_t::HS_LOAD_GAME || !single_frame_shutdown_for_reload->GetBool()))
				break;

			// shutting down
			if (oldState == HostStates_t::HS_SHUTDOWN ||
				oldState == HostStates_t::HS_RESTART)
				break;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: state machine initialization
//-----------------------------------------------------------------------------
void CHostState::Init(void)
{
	if (m_iNextState != HostStates_t::HS_SHUTDOWN)
	{
		if (m_iNextState == HostStates_t::HS_GAME_SHUTDOWN)
		{
			CHostState__State_GameShutDown(this);
		}
		else
		{

			m_iCurrentState = HostStates_t::HS_RUN;
			if (m_iNextState != HostStates_t::HS_SHUTDOWN || !single_frame_shutdown_for_reload->GetInt())
				m_iNextState = HostStates_t::HS_RUN;
		}
	}
	m_flShortFrameTime = 1.0f;
	m_bActiveGame = false;
	m_bRememberLocation = false;
	m_bBackgroundLevel = false;
	m_bWaitingForConnection = false;
	m_levelName[0] = 0;
	m_landMarkName[0] = 0;
	m_mapGroupName[0] = 0;
	m_bSplitScreenConnect = false;
	m_bGameHasShutDownAndFlushedMemory = true;
	m_vecLocation.Init();
	m_angLocation.Init();
	m_iServerState = HostStates_t::HS_NEW_GAME;
}

//-----------------------------------------------------------------------------
// Purpose: state machine setup
//-----------------------------------------------------------------------------
void CHostState::Setup(void) 
{
	g_pHostState->LoadConfig();
	LoadModConfigs();
	MergeModPlaylistsIntoFile(); // Merge mod playlists into base file
	g_BanSystem.LoadList();
	ConVar_PurgeHostNames();


	// Past engine net init: a key installed before this point does not survive.
	NET_EnableKeyInstall();

	// Check if a custom net key was specified via ConVar (e.g., from command line)
	const char* customKey = sv_netkey.GetString();
	if (customKey && customKey[0] != '\0')
	{
		NET_SetKey(customKey);
	}
	else if (CommandLine()->CheckParm("-norandomkey") || !net_useRandomKey.GetBool())
	{
		// Honor +net_useRandomKey 0 / -norandomkey. Do not call
		// NET_GenerateKey -- that helper force-flips the convar to 1.
		if (net_useRandomKey.GetBool())
			net_useRandomKey.SetValue(0); // Change callback installs the default key.
		else
			NET_SetKey(DEFAULT_NET_ENCRYPTION_KEY);
	}
	else
	{
		NET_GenerateKey();
	}


	ResetLevelName();
}

//-----------------------------------------------------------------------------
// Purpose: think
//-----------------------------------------------------------------------------
void CHostState::Think(void) const
{
	static bool bInitialized = false;
	static CFastTimer statsTimer;
	static CFastTimer banListTimer;
	static CFastTimer spireTimer;

	if (!bInitialized) // Initialize clocks.
	{
		statsTimer.Start();
		banListTimer.Start();
		spireTimer.Start();
		bInitialized = true;
	}

	HostState_HandleAutoReload();

	if (statsTimer.GetDurationInProgress().GetSeconds() > host_statusRefreshRate.GetFloat())
	{
		SetConsoleTitleA(Format("%s - %d/%d Players (%s on %s) - %d%% Server CPU (%.3f msec on frame %d)",
			hostname->GetString(), g_pServer->GetNumClients(),
			gpGlobals->maxClients, v_Playlists_GetCurrent(), m_levelName,
			static_cast<int>(g_pServer->GetCPUUsage() * 100.0f), (g_pEngine->GetFrameTime() * 1000.0f),
			g_pServer->GetTick()).c_str());

		statsTimer.Start();
	}
	if (sv_globalBanlist.GetBool() &&
		banListTimer.GetDurationInProgress().GetSeconds() > sv_banlistRefreshRate.GetFloat())
	{
		SV_CheckClientsForBan();
		banListTimer.Start();
	}
	if (spireTimer.GetDurationInProgress().GetSeconds() > spire_host_update_interval.GetFloat())
	{
		HostState_KeepAlive();
		spireTimer.Start();
	}
}

extern void Bridge_ApplyLaunchConVarTokens(void);

static void CC_ApplyLaunchConVars_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	Bridge_ApplyLaunchConVarTokens();
}

static ConCommand sdk_apply_launch_convars("_sdk_apply_launch_convars", CC_ApplyLaunchConVars_f,
	"Re-assert '+<convar> <value>' launch arguments over values set by config files.",
	FCVAR_RELEASE | FCVAR_HIDDEN | FCVAR_DONTRECORD);

//-----------------------------------------------------------------------------
// Purpose: load and execute configuration files
//-----------------------------------------------------------------------------
void CHostState::LoadConfig(void) const
{
	if (CommandLine()->ParmValue("-launcher", 0) < 1) // Launcher level 1 indicates everything is handled from the commandline/launcher.
	{
		if (!CommandLine()->CheckParm("-devsdk"))
		{
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/autoexec.cfg\n", cmd_source_t::kCommandSrcCode);
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/autoexec_server.cfg\n", cmd_source_t::kCommandSrcCode);
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec tools/rcon_server.cfg\n", cmd_source_t::kCommandSrcCode);
		}
		else // Development configs.
		{
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/autoexec_dev.cfg\n", cmd_source_t::kCommandSrcCode);
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/autoexec_server_dev.cfg\n", cmd_source_t::kCommandSrcCode);
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec tools/rcon_server_dev.cfg\n", cmd_source_t::kCommandSrcCode);
		}
		if (CommandLine()->CheckParm("-offline"))
		{
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "exec system/offline_server.cfg\n", cmd_source_t::kCommandSrcCode);
		}

		// Queued last so it drains after every exec above (exec inserts its file at
		// the front of the remaining buffer). A shipped cfg assigns values the
		// launcher also passes on the command line; the boot-time re-apply in
		// CVar_Connect runs long before this exec, so the cfg would otherwise win.
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "_sdk_apply_launch_convars\n", cmd_source_t::kCommandSrcCode);
	}
}

void CHostState::LoadModConfigs()
{
	if (!ModSystem()->IsEnabled())
		return;

	ModSystem()->LockModList();
	FOR_EACH_VEC(ModSystem()->GetResolvedModList(), i)
	{
		CModSystem::ModInstance_t* const mod = ModSystem()->GetResolvedModList()[i];
		if (!mod || !mod->IsEnabled())
			continue;

		char dirPath[MAX_PATH];
		Q_snprintf(dirPath, sizeof(dirPath), "%s%s", mod->basePath.String(), "cfg/autoload");
		CUtlVector<CUtlString> cfgFiles;
		AddFilesToList(cfgFiles, dirPath, "cfg", "GAME", '/');

		FOR_EACH_VEC(cfgFiles, fidx)
		{
			const char* filePath = cfgFiles[fidx].String();
			FileHandle_t file = FileSystem()->Open(filePath, "rt", "GAME");
			if (!file)
				continue;

			const ssize_t fileSize = FileSystem()->Size(file);
			if (fileSize <= 0)
			{
				FileSystem()->Close(file);
				continue;
			}

			static constexpr ssize_t kMaxAutoloadCfgBytes = 1 << 20;
			if (fileSize > kMaxAutoloadCfgBytes)
			{
				Warning(eDLL_T::MODSYSTEM,
					"[AUTOLOAD-CFG] skipping oversized autoload config '%s' (%zd bytes)\n",
					filePath, fileSize);
				FileSystem()->Close(file);
				continue;
			}

			std::string buffer;
			buffer.resize(size_t(fileSize));
			const ssize_t bytesRead = FileSystem()->Read(&buffer[0], fileSize, file);
			FileSystem()->Close(file);
			buffer.resize(bytesRead > 0 ? size_t(bytesRead) : 0);

			auto dispatchLine = [&](const char* lineBegin, const char* lineEnd)
			{
				while (lineBegin < lineEnd && (*lineBegin == ' ' || *lineBegin == '\t' || *lineBegin == '\r')) ++lineBegin;
				while (lineEnd > lineBegin && (lineEnd[-1] == '\r' || lineEnd[-1] == ' ' || lineEnd[-1] == '\t')) --lineEnd;
				if (lineBegin >= lineEnd) return;

				std::string cmd(lineBegin, size_t(lineEnd - lineBegin));

				const char* s = cmd.c_str();
				while (*s == ' ' || *s == '\t') ++s;
				const char* e = s;
				while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') ++e;
				std::string name(s, size_t(e - s));

				ConCommandBase* pBase = g_pCVar->FindCommandBase(name.c_str());
				if (!pBase) return;

				cmd.push_back('\n');
				CCommand args;
				args.Tokenize(cmd.c_str(), cmd_source_t::kCommandSrcCode);
				v_Cmd_Dispatch(Cbuf_GetCurrentPlayer(), pBase, &args, false);
			};

			const char* p = buffer.c_str();
			const char* end = p + buffer.size();
			while (p < end)
			{
				const char* lineStart = p;
				const void* newlinePtr = memchr(p, '\n', size_t(end - p));
				const char* lineEnd = newlinePtr ? static_cast<const char*>(newlinePtr) : end;
				dispatchLine(lineStart, lineEnd);
				p = newlinePtr ? lineEnd + 1 : end;
			}

			char relBuf[MAX_PATH];
			const char* relPtr = filePath;
			const char* basePathStr = mod->basePath.String();
			const size_t baseLen = Q_strlen(basePathStr);
			if (!Q_strnicmp(filePath, basePathStr, baseLen))
				relPtr = filePath + baseLen;
			V_strncpy(relBuf, relPtr, sizeof(relBuf));
			V_FixSlashes(relBuf, '\\');
			const char* autoloadPos = V_stristr(relBuf, "cfg\\autoload\\");
			const char* finalRel = autoloadPos ? autoloadPos : relBuf;
			Msg(eDLL_T::MODSYSTEM, "Autoloaded config: %s (%s)\n", mod->name.String(), finalRel);
		}
	}
	ModSystem()->UnlockModList();
}

//-----------------------------------------------------------------------------
// Purpose: set state machine
// Input: newState - 
// clearNext - 
//-----------------------------------------------------------------------------
void CHostState::SetState(const HostStates_t newState)
{
	m_iCurrentState = newState;

	// If our next state isn't a shutdown, or its a forced shutdown then set
	// next state to run.
	if (m_iNextState != HostStates_t::HS_SHUTDOWN ||
		!host_hasIrreversibleShutdown->GetBool())
	{
		m_iNextState = newState;
	}
}

//-----------------------------------------------------------------------------
// Purpose: shutdown active game
//-----------------------------------------------------------------------------
void CHostState::GameShutDown(void)
{
	if (m_bActiveGame)
	{
		g_pServerGameDLL->GameShutdown();
		ModPolicyGate_ResetAll();
		m_bActiveGame = false;
		ResetLevelName();
	}
}

//-----------------------------------------------------------------------------
// Purpose: initialize new game
//-----------------------------------------------------------------------------
void CHostState::State_NewGame(void)
{
	Msg(eDLL_T::ENGINE, "%s: Loading level: '%s'\n", __FUNCTION__, g_pHostState->m_levelName);

	const bool bSplitScreenConnect = m_bSplitScreenConnect;
	m_bSplitScreenConnect = false;

	if (!g_pServerGameClients) // Init Game if it ain't valid.
	{
		SV_InitGameDLL();
	}

	LARGE_INTEGER time{};

	if (!CModelLoader__Map_IsValid(g_pModelLoader, m_levelName) // Check if map is valid and if we can start a new game.
		|| !v_Host_NewGame(m_levelName, nullptr, m_bBackgroundLevel, bSplitScreenConnect, time) || !g_pServerGameClients)
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "%s: Level not valid\n", __FUNCTION__);
		// Don't leave pack freeze stuck if NewGame failed after a prior LevelShutdown.
		SnapshotDiag_SetPackFrozen(false);
		GameShutDown();
	}
	else
	{
		// Replay PODLM PrecacheModel now that the modelloader is alive for this map.
		PrecacheNatives_ReplayOdlOnMapSpawn();

		// Class-settings init; SpawnServer does not run on this path.
		Bridge_RunCanonicalClassSettingsInit("State_NewGame");

		// LevelInit complete for this map -- resume entity snapshot packing.
		SnapshotDiag_SetPackFrozen(false);
	}

	Host_UpdateSessionID();

	// Load custom activity modifiers from scripts/activity_modifier_types.txt (only once)
	static bool s_activityModifiersLoaded = false;
	if (!s_activityModifiersLoaded && IsActivityModifierSystemInitialized())
	{
		LoadCustomActivityModifiersFromFile();
		s_activityModifiersLoaded = true;
	}

	// Load scripts/activity_types.txt once. Gate IsActivitySystemReady, not
	// IsActivitySystemInitialized. Fallback only -- list-init hook is the primary.
	static bool s_activitiesLoaded = false;
	if (!s_activitiesLoaded && !ActivityList_CustomsRegisteredAtListInit()
		&& IsActivitySystemReady())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] list-init hook did not register customs -- "
			"loading late; activities may not bind to already-resolved models\n");
		LoadCustomActivitiesFromFile();
		s_activitiesLoaded = true;
	}

	// S3->S21 activity ID map. Server-side resolvers, not the client-side gate.
	Bridge_BuildS3ToS21ActivityMap();

	SetState(HostStates_t::HS_RUN);
}

//-----------------------------------------------------------------------------
// Purpose: change singleplayer level
//-----------------------------------------------------------------------------
void CHostState::State_ChangeLevelSP(void)
{
	Msg(eDLL_T::ENGINE, "%s: Changing singleplayer level to: '%s'\n", __FUNCTION__, m_levelName);
	m_flShortFrameTime = 1.5f; // Set frame time.

	if (CModelLoader__Map_IsValid(g_pModelLoader, m_levelName)) // Check if map is valid and if we can start a new game.
	{
		v_Host_ChangeLevel(true, m_levelName, m_mapGroupName); // Call change level as singleplayer level.
		Host_UpdateSessionID();
		SetState(HostStates_t::HS_RUN);
		return;
	}

	// Error(NO_ERROR) returns, so leave a terminal state behind: the host state
	// is still CHANGE_LEVEL_SP here and FrameUpdate would re-enter every frame.
	Error(eDLL_T::ENGINE, NO_ERROR, "%s: Unable to find level: '%s'\n", __FUNCTION__, m_levelName);
	SetState(HostStates_t::HS_RUN);
}

// Host_ChangeLevel notifies signon=9 itself, but only after the map load. This
// early copy paints the loading screen first; the client drops the second one.
static void Host_NotifyClientsChangeLevel(void)
{
	if (!g_pServer)
		return;

	const int nMax = g_pServer->GetMaxClients();
	for (int i = 0; i < nMax && i < MAX_PLAYERS; ++i)
	{
		CClient* const pClient = g_pServer->GetClient(i);
		if (!pClient || !pClient->IsHumanPlayer())
			continue;

		CNetChan* const pChan = pClient->GetNetChan();
		pClient->SetSignonState(SIGNONSTATE::SIGNONSTATE_CHANGELEVEL);
		if (v_CClient_SendSignonState)
			v_CClient_SendSignonState(pClient);
		if (pChan)
			pChan->SendDatagram(nullptr);

		Warning(eDLL_T::ENGINE,
			"[CHANGELEVEL] notified client[%d] signon=9 netchan=%p send=%d\n",
			i, (void*)pChan, v_CClient_SendSignonState ? 1 : 0);
	}
}

//-----------------------------------------------------------------------------
// Purpose: change multiplayer level
//-----------------------------------------------------------------------------
void CHostState::State_ChangeLevelMP(void)
{
	Msg(eDLL_T::ENGINE, "%s: Changing multiplayer level to: '%s'\n", __FUNCTION__, m_levelName);
	m_flShortFrameTime = 0.5f; // Set frame time.

	// Validate before teardown. Error(NO_ERROR) returns; fall back to HS_RUN.
	if (!CModelLoader__Map_IsValid(g_pModelLoader, m_levelName))
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "%s: Unable to find level: '%s'\n", __FUNCTION__, m_levelName);
		SetState(HostStates_t::HS_RUN);
		return;
	}

	if (g_pServer)
	{
		const int nMax = g_pServer->GetMaxClients();
		int nHuman = 0;
		int nFull = 0;
		for (int i = 0; i < nMax && i < MAX_PLAYERS; ++i)
		{
			CClient* const pClient = g_pServer->GetClient(i);
			if (!pClient || !pClient->IsHumanPlayer())
				continue;
			++nHuman;
			if (pClient->IsActive())
				++nFull;
			Warning(eDLL_T::ENGINE,
				"[CHANGELEVEL] client[%d] signon=%d netchan=%p\n",
				i, (int)pClient->GetSignonState(), (void*)pClient->GetNetChan());
		}
		Warning(eDLL_T::ENGINE,
			"[CHANGELEVEL] before LevelShutdown humans=%d full=%d map='%s'\n",
			nHuman, nFull, m_levelName);
	}
	Host_NotifyClientsChangeLevel();
	g_pServerGameDLL->LevelShutdown();
	if (g_pServer)
	{
		const int nMax = g_pServer->GetMaxClients();
		for (int i = 0; i < nMax && i < MAX_PLAYERS; ++i)
		{
			CClient* const pClient = g_pServer->GetClient(i);
			if (!pClient || !pClient->IsHumanPlayer())
				continue;
			Warning(eDLL_T::ENGINE,
				"[CHANGELEVEL] after LevelShutdown client[%d] signon=%d netchan=%p\n",
				i, (int)pClient->GetSignonState(), (void*)pClient->GetNetChan());
		}
	}
	// Playlists_Parse remounts the playlist map VPK. Safe now: the live world is down.
	HostManager_ParseDeferredPlaylist();
	if (CModelLoader__Map_IsValid(g_pModelLoader, m_levelName)) // Check if map is valid and if we can start a new game.
	{
		v_Host_ChangeLevel(false, m_levelName, m_mapGroupName); // Call change level as multiplayer level.

		// Re-apply ODL precache after LevelShutdown tore down modelprecache.
		PrecacheNatives_ReplayOdlOnMapSpawn();

		// Canonical class-settings init (gated) -- see State_NewGame.
		Bridge_RunCanonicalClassSettingsInit("State_ChangeLevelMP");

		// Changelevel LevelInit complete -- resume entity snapshot packing.
		// Must pair with SnapshotDiag_SetPackFrozen(true) at LevelShutdown.
		SnapshotDiag_SetPackFrozen(false);

		// Flush NET_SignonState(2,-1); SV_ActivateServer queues it but does not send.
		if (g_pServer)
		{
			const int nMax = g_pServer->GetMaxClients();
			for (int i = 0; i < nMax && i < MAX_PLAYERS; ++i)
			{
				CClient* const pClient = g_pServer->GetClient(i);
				if (!pClient || !pClient->IsHumanPlayer())
					continue;
				CNetChan* const pChan = pClient->GetNetChan();
				Warning(eDLL_T::ENGINE,
					"[CHANGELEVEL] after Host_ChangeLevel client[%d] signon=%d netchan=%p\n",
					i, (int)pClient->GetSignonState(), (void*)pChan);
				if (v_CClient_SendSignonState && pChan
					&& pClient->GetSignonState() >= SIGNONSTATE::SIGNONSTATE_CONNECTED)
				{
					v_CClient_SendSignonState(pClient);
					// Transmit returns 0 without sending anything when the
					// reliable stream has overflowed, so the byte count is the
					// only proof the signon actually left the box.
					const int nSent = pChan->SendDatagram(nullptr);
					Warning(eDLL_T::ENGINE,
						"[CHANGELEVEL] flushed SignonState client[%d] signon=%d sent=%d\n",
						i, (int)pClient->GetSignonState(), nSent);
				}
			}
		}
	}
	else
	{
		// Validated above, so only reachable if the teardown between the two
		// checks unmounted the map. The level is already down; HS_RUN leaves a
		// host with no level, but re-entering this state would loop forever.
		Error(eDLL_T::ENGINE, NO_ERROR, "%s: Level went missing mid-transition: '%s'\n", __FUNCTION__, m_levelName);
		SnapshotDiag_SetPackFrozen(false);
		GameShutDown();
		SetState(HostStates_t::HS_GAME_SHUTDOWN);
		return;
	}

	Host_UpdateSessionID();
	SetState(HostStates_t::HS_RUN);
}

//-----------------------------------------------------------------------------
// Purpose: resets the level name
//-----------------------------------------------------------------------------
void CHostState::ResetLevelName(void)
{
	static const char* szNoMap = "no_map";
	Q_snprintf(const_cast<char*>(m_levelName), sizeof(m_levelName), "%s", szNoMap);
}

void VHostState::Detour(const bool bAttach) const
{
	DetourSetup(&CHostState__FrameUpdate, &CHostState::FrameUpdate, bAttach);
}

///////////////////////////////////////////////////////////////////////////////
CHostState* g_pHostState = nullptr;

bool g_hostReloadState = false;
#endif // CLIENT_DLL
