#if defined(CLIENT_DLL)

#include "core/stdafx.h"
#include "const.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier1/cmd.h"
#include "tier1/NetAdr.h"
#include "tier2/curlutils.h"
#include "completion.h"
#include "callback.h"
#include "global.h"
#include "game/client/visual_clutter.h"

ConVar curl_debug("curl_debug", "0", FCVAR_DEVELOPMENTONLY, "Determines whether or not to enable curl debug logging.", "1 = curl logs; 0 (zero) = no logs");
ConVar curl_timeout("curl_timeout", "15", FCVAR_DEVELOPMENTONLY, "Maximum time in seconds a curl transfer operation could take.");
ConVar ssl_verify_peer("ssl_verify_peer", "1", FCVAR_DEVELOPMENTONLY, "Verify the authenticity of the peer's SSL certificate.", "1 = curl verifies; 0 (zero) = no verification");
// S21 vscripts call GetConVarInt("mastery_unlock_all_trials") on the mastery/trials UI path.
// Register so lookups succeed (0=no change; 1=debug short-circuit: all trials complete).
static ConVar mastery_unlock_all_trials("mastery_unlock_all_trials", "0", FCVAR_RELEASE | FCVAR_ARCHIVE, "Unlock all weapon mastery trials on the client.");

//-----------------------------------------------------------------------------
// ENGINE |
ConVar* single_frame_shutdown_for_reload   = nullptr;
ConVar* old_gather_props                   = nullptr;

ConVar* enable_debug_overlays              = nullptr;
ConVar* debug_draw_box_depth_test          = nullptr;

ConVar* developer                          = nullptr;
ConVar* fps_max                            = nullptr;
ConVar* fps_max_vsync                      = nullptr;

ConVar* in_syncRT                          = nullptr;

ConVar* base_tickinterval_sp               = nullptr;
ConVar* base_tickinterval_mp               = nullptr;

ConVar* staticProp_no_fade_scalar          = nullptr;
ConVar* staticProp_gather_size_weight      = nullptr;

ConVar* model_defaultFadeDistScale         = nullptr;
ConVar* model_defaultFadeDistMin           = nullptr;

ConVar* ip_cvar                            = nullptr;
ConVar* hostname                           = nullptr;
ConVar* hostip                             = nullptr;
ConVar* hostport                           = nullptr;

ConVar* host_hasIrreversibleShutdown       = nullptr;
ConVar* host_timescale                     = nullptr;

ConVar* mp_gamemode                        = nullptr;

ConVar* r_visualizetraces                  = nullptr;
ConVar* r_visualizetraces_duration         = nullptr;
ConVar* r_drawvgui                         = nullptr;
ConVar* r_drawalphasort                    = nullptr;

ConVar* stream_overlay                     = nullptr;
ConVar* stream_overlay_mode                = nullptr;
ConVar* gpu_driven_tex_stream              = nullptr;

//ConVar* eula_version = nullptr;
//ConVar* eula_version_accepted = nullptr;

ConVar* language_cvar                      = nullptr;

ConVar* voice_noxplat                      = nullptr;

ConVar* platform_user_id                   = nullptr;

static ConVar* fps_max_use_refresh = nullptr;
static ConVar* fps_absolute_max = nullptr;
static bool s_bFpsMaxApplying = false;
static bool s_bFpsMaxBound = false;

static void FpsMaxChanged_f(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t pUserData);
static void FpsMax_ApplyUnlocked(void);

static ConVar fps_max_unlimited("fps_max_unlimited", "0", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"When 1, force fps_max to 0 (unlocked).", FpsMaxChanged_f);

static void FpsMax_ApplyUnlocked(void)
{
	if (s_bFpsMaxApplying || !fps_max)
		return;

	s_bFpsMaxApplying = true;
	if (fps_max_unlimited.GetBool())
	{
		if (fps_max->GetFloat() != 0.f)
			fps_max->SetValue(0);
	}
	else if (fps_max->GetFloat() < 0.f
		&& (!fps_max_use_refresh || !fps_max_use_refresh->GetBool()))
	{
		fps_max->SetValue(0);
	}
	s_bFpsMaxApplying = false;
}

extern void Rui_BindDrawEnable(void);

static void UnhideClientRenderCvar(const char* const pszName, const bool bCheat)
{
	ConVar* const pVar = g_pCVar->FindVar(pszName);
	if (!pVar)
	{
		Warning(eDLL_T::CLIENT, "[CVAR] '%s' not registered -- console set would stringcmd to dedi\n", pszName);
		return;
	}

	pVar->RemoveFlags(FCVAR_DEVELOPMENTONLY | FCVAR_HIDDEN);
	if (bCheat)
		pVar->RemoveFlags(FCVAR_CHEAT);
	pVar->AddFlags(FCVAR_RELEASE);
}

static void ClientRenderCvars_BindShipped(void)
{
	if (!g_pCVar)
		return;

	UnhideClientRenderCvar("mat_autoexposure_speed", false);
	UnhideClientRenderCvar("r_particle_timescale", false);
	UnhideClientRenderCvar("model_fadeRangeFraction", true);
	UnhideClientRenderCvar("cl_drawhud", true);
	UnhideClientRenderCvar("rui_text_drawing_enabled", false);
	Rui_BindDrawEnable();
}

static void FpsMaxChanged_f(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t pUserData)
{
	NOTE_UNUSED(var);
	NOTE_UNUSED(pOldValue);
	NOTE_UNUSED(flOldValue);
	NOTE_UNUSED(pUserData);
	FpsMax_ApplyUnlocked();
}

static void FpsMax_BindShipped(void)
{
	if (!g_pCVar)
		return;

	fps_max = g_pCVar->FindVar("fps_max");
	fps_max_vsync = g_pCVar->FindVar("fps_max_vsync");
	fps_max_use_refresh = g_pCVar->FindVar("fps_max_use_refresh");
	fps_absolute_max = g_pCVar->FindVar("fps_absolute_max");

	if (fps_max)
	{
		fps_max->AddFlags(FCVAR_ARCHIVE);
		fps_max->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		if (!s_bFpsMaxBound)
		{
			fps_max->InstallChangeCallback(FpsMaxChanged_f, nullptr, false);
			s_bFpsMaxBound = true;
		}
	}
	if (fps_max_vsync)
	{
		fps_max_vsync->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		fps_max_vsync->AddFlags(FCVAR_RELEASE);
	}
	if (fps_max_use_refresh)
	{
		fps_max_use_refresh->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		fps_max_use_refresh->AddFlags(FCVAR_RELEASE);
	}
	if (fps_absolute_max)
	{
		fps_absolute_max->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		fps_absolute_max->AddFlags(FCVAR_RELEASE);
		// FilterTime substitutes this when fps_max<=0 and min-clamps to it.
		if (fps_absolute_max->GetFloat() <= 300.f)
			fps_absolute_max->SetValue(10000.f);
	}

	FpsMax_ApplyUnlocked();
}

//-----------------------------------------------------------------------------
// Purpose: Default cl_is_softened_locale on. SetDefault keeps resets at 1.
//-----------------------------------------------------------------------------
static void SoftenedLocale_DefaultOn(void)
{
	if (!g_pCVar)
		return;

	ConVar* const pVar = g_pCVar->FindVar("cl_is_softened_locale");
	if (!pVar)
	{
		Warning(eDLL_T::CLIENT, "[CVAR] 'cl_is_softened_locale' not registered -- softened locale stays off\n");
		return;
	}

	pVar->SetDefault("1");
	pVar->SetValue(1);
}

void PlatformUserId_SetFromPlatform(const char* value)
{
	if (!platform_user_id || !VALID_CHARSTAR(value))
		return;

	platform_user_id->SetValue(value);
}

ConVar* name_cvar                          = nullptr;

//-----------------------------------------------------------------------------
// SERVER |
ConVar* sv_cheats                          = nullptr;
ConVar* sv_visualizetraces                 = nullptr;
ConVar* sv_visualizetraces_duration        = nullptr;
ConVar* bhit_enable                        = nullptr;
//-----------------------------------------------------------------------------
// CLIENT |
ConVar* cl_updaterate_mp                   = nullptr;

ConVar* cl_threaded_bone_setup             = nullptr;

ConVar* pvs_start_early                    = nullptr;
ConVar* pvs_frustumCullOnly                = nullptr;

ConVar* origin_disconnectWhenOffline       = nullptr;
ConVar* discord_updatePresence = nullptr;

ConVar* match_playlist                     = nullptr;

ConVar* gamepad_custom_enabled             = nullptr;
ConVar* gamepad_custom_assist_on           = nullptr;
ConVar* gamepad_look_curve                 = nullptr;


ConVar* hudchat_visibility				   = nullptr;
ConVar* hudchat_new_message_fade_duration  = nullptr;
ConVar* hudchat_new_message_shown_duration = nullptr;
//-----------------------------------------------------------------------------
// FILESYSTEM |
ConVar* fs_showAllReads                    = nullptr;
//-----------------------------------------------------------------------------
// NETCHANNEL |
ConVar* net_usesocketsforloopback;
ConVar* net_data_block_enabled             = nullptr;
ConVar* net_datablock_networkLossForSlowSpeed = nullptr;
ConVar* net_compressDataBlock              = nullptr;

ConVar* net_showmsg                        = nullptr;
ConVar* net_blockmsg                       = nullptr;
ConVar* net_showpeaks                      = nullptr;
//-----------------------------------------------------------------------------
// RUI |
ConVar* rui_defaultDebugFontFace           = nullptr;
//-----------------------------------------------------------------------------
// MILES |
ConVar* miles_language                     = nullptr;

//-----------------------------------------------------------------------------
// Purpose: Write every "+<convar> <value>" launch argument onto its ConVar.
//-----------------------------------------------------------------------------
void Bridge_ApplyLaunchConVarTokens(void)
{
	if (!g_pCVar || !CommandLine())
		return;

	const int nParms = CommandLine()->ParmCount();
	for (int i = 0; i < nParms - 1; i++)
	{
		const char* const pszParm = CommandLine()->GetParm(i);
		if (!pszParm || pszParm[0] != '+')
			continue;

		ConVar* const pCVar = g_pCVar->FindVar(pszParm + 1);
		if (!pCVar)
			continue; // unknown name or a ConCommand -- skip

		const char* const pszValue = CommandLine()->GetParm(i + 1);
		if (!pszValue || pszValue[0] == '+')
			continue; // no value token follows this "+convar"

		pCVar->SetValue(pszValue);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Re-apply "+<convar> <value>" after SDK ConVars are registered.
//-----------------------------------------------------------------------------
void Bridge_ApplyLaunchConVarOverrides(void)
{
	SoftenedLocale_DefaultOn();
	VisualClutter_ApplyPin();

	Bridge_ApplyLaunchConVarTokens();

	ClientRenderCvars_BindShipped();
	FpsMax_BindShipped();
}

//-----------------------------------------------------------------------------
// Purpose: initialize shipped ConVar's
//-----------------------------------------------------------------------------
void ConVar_InitShipped(void)
{
	developer                        = g_pCVar->FindVar("developer");
	fps_max                          = g_pCVar->FindVar("fps_max");
	fps_max_vsync                    = g_pCVar->FindVar("fps_max_vsync");
	base_tickinterval_sp             = g_pCVar->FindVar("base_tickinterval_sp");
	base_tickinterval_mp             = g_pCVar->FindVar("base_tickinterval_mp");
	fs_showAllReads                  = g_pCVar->FindVar("fs_showAllReads");

	//eula_version = g_pCVar->FindVar("eula_version");
	//eula_version_accepted = g_pCVar->FindVar("eula_version_accepted");

	language_cvar                    = g_pCVar->FindVar("language");
	voice_noxplat                    = g_pCVar->FindVar("voice_noxplat");
	platform_user_id                 = g_pCVar->FindVar("platform_user_id");
	name_cvar                        = g_pCVar->FindVar("name");
	cl_updaterate_mp                 = g_pCVar->FindVar("cl_updaterate_mp");
	cl_threaded_bone_setup           = g_pCVar->FindVar("cl_threaded_bone_setup");
	pvs_start_early                  = g_pCVar->FindVar("pvs_start_early");
	pvs_frustumCullOnly              = g_pCVar->FindVar("pvs_frustumCullOnly");
	single_frame_shutdown_for_reload = g_pCVar->FindVar("single_frame_shutdown_for_reload");
	enable_debug_overlays            = g_pCVar->FindVar("enable_debug_overlays");
	debug_draw_box_depth_test        = g_pCVar->FindVar("debug_draw_box_depth_test");
	model_defaultFadeDistScale       = g_pCVar->FindVar("model_defaultFadeDistScale");
	model_defaultFadeDistMin         = g_pCVar->FindVar("model_defaultFadeDistMin");
	miles_language                   = g_pCVar->FindVar("miles_language");
	rui_defaultDebugFontFace         = g_pCVar->FindVar("rui_defaultDebugFontFace");
	in_syncRT                        = g_pCVar->FindVar("in_syncRT");
	r_visualizetraces                = g_pCVar->FindVar("r_visualizetraces");
	r_visualizetraces_duration       = g_pCVar->FindVar("r_visualizetraces_duration");
	r_drawvgui                       = g_pCVar->FindVar("r_drawvgui");
	r_drawalphasort                  = g_pCVar->FindVar("r_drawalphasort");
	staticProp_no_fade_scalar        = g_pCVar->FindVar("staticProp_no_fade_scalar");
	staticProp_gather_size_weight    = g_pCVar->FindVar("staticProp_gather_size_weight");
	stream_overlay                   = g_pCVar->FindVar("stream_overlay");
	stream_overlay_mode              = g_pCVar->FindVar("stream_overlay_mode");
	gpu_driven_tex_stream            = g_pCVar->FindVar("gpu_driven_tex_stream");
	sv_cheats                        = g_pCVar->FindVar("sv_cheats");
	sv_visualizetraces               = g_pCVar->FindVar("sv_visualizetraces");
	sv_visualizetraces_duration      = g_pCVar->FindVar("sv_visualizetraces_duration");
	old_gather_props                 = g_pCVar->FindVar("old_gather_props");
	origin_disconnectWhenOffline     = g_pCVar->FindVar("origin_disconnectWhenOffline");
	discord_updatePresence           = g_pCVar->FindVar("discord_updatePresence");
	match_playlist                   = g_pCVar->FindVar("match_playlist");

	gamepad_custom_enabled           = g_pCVar->FindVar("gamepad_custom_enabled");
	gamepad_custom_assist_on         = g_pCVar->FindVar("gamepad_custom_assist_on");
	gamepad_look_curve               = g_pCVar->FindVar("gamepad_look_curve");


	hudchat_visibility				 = g_pCVar->FindVar("hudchat_visibility");
	hudchat_new_message_fade_duration = g_pCVar->FindVar("hudchat_new_message_fade_duration");
	hudchat_new_message_shown_duration = g_pCVar->FindVar("hudchat_new_message_shown_duration");
	mp_gamemode                      = g_pCVar->FindVar("mp_gamemode");
	ip_cvar                          = g_pCVar->FindVar("ip");
	hostname                         = g_pCVar->FindVar("hostname");
	hostip                           = g_pCVar->FindVar("hostip");
	hostport                         = g_pCVar->FindVar("hostport");
	host_hasIrreversibleShutdown     = g_pCVar->FindVar("host_hasIrreversibleShutdown");
	host_timescale                   = g_pCVar->FindVar("host_timescale");

	net_data_block_enabled           = g_pCVar->FindVar("net_data_block_enabled");
	net_compressDataBlock            = g_pCVar->FindVar("net_compressDataBlock");
	net_datablock_networkLossForSlowSpeed = g_pCVar->FindVar("net_datablock_networkLossForSlowSpeed");

	net_usesocketsforloopback        = g_pCVar->FindVar("net_usesocketsforloopback");

	net_showmsg = g_pCVar->FindVar("net_showmsg");
	net_blockmsg = g_pCVar->FindVar("net_blockmsg");
	net_showpeaks = g_pCVar->FindVar("net_showpeaks");
	// Force 0 (full PVS). FindVar is null when DEVELOPMENTONLY cvars were never registered.
	if (pvs_frustumCullOnly)
		pvs_frustumCullOnly->SetValue(0);

	if (cl_updaterate_mp)
		cl_updaterate_mp->RemoveFlags(FCVAR_DEVELOPMENTONLY);

	if (platform_user_id)
	{
		// NOT_CONNECTED would refuse the connect-time Nucleus stamp. Keep
		// DEVELOPMENTONLY|HIDDEN so native S2C ConVar-set still rejects.
		platform_user_id->RemoveFlags(FCVAR_PLATFORM_SYSTEM | FCVAR_NOT_CONNECTED);
	}

	if (cl_threaded_bone_setup)
		cl_threaded_bone_setup->RemoveFlags(FCVAR_DEVELOPMENTONLY);
	if (rui_defaultDebugFontFace)
		rui_defaultDebugFontFace->RemoveFlags(FCVAR_DEVELOPMENTONLY);
	if (origin_disconnectWhenOffline)
		origin_disconnectWhenOffline->RemoveFlags(FCVAR_DEVELOPMENTONLY);
	if (discord_updatePresence)
	{
		discord_updatePresence->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		discord_updatePresence->AddFlags(FCVAR_HIDDEN);
	}
	if (fps_max)
		fps_max->AddFlags(FCVAR_ARCHIVE);
	if (fps_max_vsync)
		fps_max_vsync->RemoveFlags(FCVAR_DEVELOPMENTONLY);

	if (base_tickinterval_sp)
		base_tickinterval_sp->RemoveFlags(FCVAR_DEVELOPMENTONLY);
	if (base_tickinterval_mp)
		base_tickinterval_mp->RemoveFlags(FCVAR_DEVELOPMENTONLY);

	if (mp_gamemode)
	{
		mp_gamemode->RemoveFlags(FCVAR_DEVELOPMENTONLY);

		// Client callback goes through CEngineClient::SetupGamemode; dedi calls it directly.
		mp_gamemode->RemoveChangeCallback(mp_gamemode->GetChangeCallback(0), 0);
		mp_gamemode->InstallChangeCallback(MP_GameMode_Changed_f, nullptr, false);
	}

	if (net_usesocketsforloopback)
		net_usesocketsforloopback->RemoveFlags(FCVAR_DEVELOPMENTONLY);
	if (language_cvar)
		language_cvar->InstallChangeCallback(LanguageChanged_f, nullptr, false);

	// Launch-arg re-apply is after ConVar_Register -- SDK vars are not in g_pCVar yet.
}

//-----------------------------------------------------------------------------
// Purpose: unregister/disable extraneous ConVar's.
//-----------------------------------------------------------------------------
void ConVar_PurgeShipped(void)
{
}

//-----------------------------------------------------------------------------
// Purpose: clear all hostname ConVar's.
//-----------------------------------------------------------------------------
void ConVar_PurgeHostNames(void)
{
	// Blank by VALUE, not a name list -- hostname ConVars outnumber any hand list.
	const char* const pszUpstreamHosts[] =
	{
		".ea.com",
		".respawn.com",
		".easports.com",
	};

	// The signed identity token is obtained from this one, and it is the only
	// thing proving a connecting player owns the account they claim. Nothing else
	// upstream is needed to play.
	const char* const pszKeep[] =
	{
		"eadpAuth_hostname",
	};

	if (!g_pCVar)
		return;

	int nPurged = 0;
	int nSafety = 0;

	for (ConCommandBase* pBase = g_pCVar->GetCommandList();
		pBase && nSafety++ < 16384; pBase = pBase->GetNext())
	{
		if (pBase->IsCommand())
			continue;

		ConVar* const pCVar = static_cast<ConVar*>(pBase);
		const char* const pszValue = pCVar->GetString();

		if (!pszValue || !pszValue[0])
			continue;

		bool bUpstream = false;

		for (size_t i = 0; i < SDK_ARRAYSIZE(pszUpstreamHosts); i++)
		{
			if (strstr(pszValue, pszUpstreamHosts[i]))
			{
				bUpstream = true;
				break;
			}
		}

		if (!bUpstream)
			continue;

		const char* const pszName = pCVar->GetName();
		bool bKeep = false;

		for (size_t i = 0; i < SDK_ARRAYSIZE(pszKeep); i++)
		{
			if (pszName && V_stricmp(pszName, pszKeep[i]) == 0)
			{
				bKeep = true;
				break;
			}
		}

		if (bKeep)
			continue;

		pCVar->SetValue(NET_IPV4_UNSPEC);
		nPurged++;
	}

	Msg(eDLL_T::ENGINE, "[HOSTS] purged %d upstream endpoint ConVar(s)\n", nPurged);
}

static ConCommand bhit("bhit", BHit_f, "Bullet-hit trajectory debug", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL | FCVAR_CHEAT);

static ConCommand line("line", Line_f, "Draw a debug line", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT);
static ConCommand triangle("triangle", Triangle_f, "Draw a debug triangle", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT);
static ConCommand sphere("sphere", Sphere_f, "Draw a debug sphere", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT);
static ConCommand capsule("capsule", Capsule_f, "Draw a debug capsule", FCVAR_REPLICATED | FCVAR_CHEAT);
static ConCommand box("createbox", Box_f, "Draw a permanent solid box for map making", FCVAR_RELEASE | FCVAR_GAMEDLL);
static ConCommand clearboxes("clearboxes", ClearBoxes_f, "Clear all boxes and debug overlays", FCVAR_RELEASE | FCVAR_GAMEDLL);

// TODO: move VPK building code to separate file and place this in 'packedstore.cpp'
static ConCommand fs_vpk_mount("fs_vpk_mount", VPK_Mount_f, "Mount a VPK file for FileSystem usage", FCVAR_DEVELOPMENTONLY);
static ConCommand fs_vpk_unmount("fs_vpk_unmount", VPK_Unmount_f, "Unmount a VPK file and clear its cache", FCVAR_DEVELOPMENTONLY);
static ConCommand fs_vpk_pack("fs_vpk_pack", VPK_Pack_f, "Pack a VPK file from current workspace", FCVAR_DEVELOPMENTONLY);
static ConCommand fs_vpk_unpack("fs_vpk_unpack", VPK_Unpack_f, "Unpack all files from a VPK file", FCVAR_DEVELOPMENTONLY);

//-----------------------------------------------------------------------------
// Purpose: shipped ConCommand initialization
//-----------------------------------------------------------------------------
void ConCommand_InitShipped(void)
{
	///---------------------------- [ CALLBACK SWAP ]
	//-------------------------------------------------------------------------
	// ENGINE DLL |
	ConCommand* changelevel = g_pCVar->FindCommand("changelevel");
	ConCommand* map = g_pCVar->FindCommand("map");
	ConCommand* map_background = g_pCVar->FindCommand("map_background");
	ConCommand* ss_map = g_pCVar->FindCommand("ss_map");
	ConCommand* migrateme = g_pCVar->FindCommand("migrateme");
	ConCommand* help = g_pCVar->FindCommand("help");
	ConCommand* convar_list = g_pCVar->FindCommand("convar_list");
	ConCommand* convar_differences = g_pCVar->FindCommand("convar_differences");
	ConCommand* convar_findByFlags = g_pCVar->FindCommand("convar_findByFlags");


	//-------------------------------------------------------------------------
	// MATERIAL SYSTEM
	ConCommand* mat_crosshair = g_pCVar->FindCommand("mat_crosshair"); // Patch callback function to working callback.
	//-------------------------------------------------------------------------
	// CLIENT DLL |
	ConCommand* give = g_pCVar->FindCommand("give");
	ConCommand* set = g_pCVar->FindCommand("set");

	help->m_fnCommandCallback = CVHelp_f;
	convar_list->m_fnCommandCallback = CVList_f;
	convar_differences->m_fnCommandCallback = CVDiff_f;
	convar_findByFlags->m_fnCommandCallback = CVFlag_f;
	changelevel->m_fnCompletionCallback = Host_Changelevel_f_CompletionFunc;

	map->m_fnCompletionCallback = Host_Map_f_CompletionFunc;
	map_background->m_fnCompletionCallback = Host_Background_f_CompletionFunc;
	ss_map->m_fnCompletionCallback = Host_SSMap_f_CompletionFunc;


	mat_crosshair->m_fnCommandCallback = Mat_CrossHair_f;
	give->m_fnCompletionCallback = Game_Give_f_CompletionFunc;
	set->m_fnCommandCallback = Set_f;

	/// ---------------------------- [ FLAG REMOVAL ]
	//-------------------------------------------------------------------------
	if (!CommandLine()->CheckParm("-devsdk"))
	{
		const char* pszMaskedBases[] =
		{
			"connect",
			"connectAsSpectator",
			"connectWithKey",
			"silentconnect",
			"ping",
			// Native flags FCVAR_SPONLY only; Create adds DEVELOPMENTONLY unless unmasked.
			"mat_crosshair",
			"launchplaylist",
			"quit",
			"exit",
			"reload",
			"restart",
			"set",
			"status",
			"version",
		};

		for (size_t i = 0; i < SDK_ARRAYSIZE(pszMaskedBases); i++)
		{
			if (ConCommandBase* pCommandBase = g_pCVar->FindCommandBase(pszMaskedBases[i]))
			{
				pCommandBase->RemoveFlags(FCVAR_DEVELOPMENTONLY);
			}
		}

		convar_list->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		convar_differences->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		convar_findByFlags->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		help->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		migrateme->RemoveFlags(FCVAR_SERVER_CAN_EXECUTE);
		changelevel->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		map->RemoveFlags(FCVAR_DEVELOPMENTONLY | FCVAR_SERVER_CAN_EXECUTE);
		map_background->RemoveFlags(FCVAR_DEVELOPMENTONLY | FCVAR_SERVER_CAN_EXECUTE);
		ss_map->RemoveFlags(FCVAR_DEVELOPMENTONLY | FCVAR_SERVER_CAN_EXECUTE);
	}
}

//-----------------------------------------------------------------------------
// Purpose: unregister extraneous ConCommand's.
//-----------------------------------------------------------------------------
void ConCommand_PurgeShipped(void)
{
}
#else // !CLIENT_DLL

#include "core/stdafx.h"
#include "const.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier1/cmd.h"
#include "tier1/NetAdr.h"
#include "tier2/curlutils.h"
#include "completion.h"
#include "callback.h"
#include "global.h"


ConVar curl_debug("curl_debug", "0", FCVAR_DEVELOPMENTONLY, "Determines whether or not to enable curl debug logging.", "1 = curl logs; 0 (zero) = no logs");
ConVar curl_timeout("curl_timeout", "15", FCVAR_DEVELOPMENTONLY, "Maximum time in seconds a curl transfer operation could take.");
ConVar ssl_verify_peer("ssl_verify_peer", "1", FCVAR_DEVELOPMENTONLY, "Verify the authenticity of the peer's SSL certificate.", "1 = curl verifies; 0 (zero) = no verification");

//-----------------------------------------------------------------------------
// ENGINE |
ConVar* single_frame_shutdown_for_reload   = nullptr;
ConVar* old_gather_props                   = nullptr;

ConVar* enable_debug_overlays              = nullptr;
ConVar* debug_draw_box_depth_test          = nullptr;

ConVar* developer                          = nullptr;
ConVar* fps_max                            = nullptr;
ConVar* fps_max_vsync                      = nullptr;


ConVar* base_tickinterval_sp               = nullptr;
ConVar* base_tickinterval_mp               = nullptr;

ConVar* staticProp_no_fade_scalar          = nullptr;
ConVar* staticProp_gather_size_weight      = nullptr;

ConVar* model_defaultFadeDistScale         = nullptr;
ConVar* model_defaultFadeDistMin           = nullptr;

ConVar* ip_cvar                            = nullptr;
ConVar* hostname                           = nullptr;
ConVar* hostip                             = nullptr;
ConVar* hostport                           = nullptr;

ConVar* host_hasIrreversibleShutdown       = nullptr;
ConVar* host_timescale                     = nullptr;

ConVar* mp_gamemode                        = nullptr;


ConVar* stream_overlay                     = nullptr;
ConVar* stream_overlay_mode                = nullptr;
ConVar* gpu_driven_tex_stream              = nullptr;

//ConVar* eula_version = nullptr;
//ConVar* eula_version_accepted = nullptr;

ConVar* language_cvar                      = nullptr;

ConVar* voice_noxplat                      = nullptr;

ConVar* platform_user_id                   = nullptr;


//-----------------------------------------------------------------------------
// SERVER |
ConVar* ai_script_nodes_draw               = nullptr;
ConVar* navmesh_move_along_surface_asserts = nullptr;

ConVar* sv_forceChatToTeamOnly             = nullptr;

ConVar* sv_single_core_dedi                = nullptr;

ConVar* sv_maxunlag                        = nullptr;
ConVar* sv_lagpushticks                    = nullptr;
ConVar* sv_clockcorrection_msecs           = nullptr;

ConVar* sv_updaterate_sp                   = nullptr;
ConVar* sv_updaterate_mp                   = nullptr;

ConVar* sv_showhitboxes                    = nullptr;
ConVar* sv_stats                           = nullptr;

ConVar* sv_voiceEcho                       = nullptr;
ConVar* sv_voiceenable                     = nullptr;
ConVar* sv_alltalk                         = nullptr;

ConVar* sv_clampPlayerFrameTime            = nullptr;

ConVar* playerframetimekick_margin         = nullptr;
ConVar* playerframetimekick_decayrate      = nullptr;

ConVar* player_userCmdsQueueWarning        = nullptr;
ConVar* player_disallow_negative_frametime = nullptr;

ConVar* script_server_fps                  = nullptr;

ConVar* hudchat_dead_can_only_talk_to_other_dead = nullptr;
ConVar* sv_cheats                          = nullptr;
ConVar* sv_visualizetraces                 = nullptr;
ConVar* sv_visualizetraces_duration        = nullptr;
ConVar* bhit_enable                        = nullptr;
//-----------------------------------------------------------------------------
// CLIENT |
//-----------------------------------------------------------------------------
// FILESYSTEM |
ConVar* fs_showAllReads                    = nullptr;
//-----------------------------------------------------------------------------
// NETCHANNEL |
ConVar* net_usesocketsforloopback;
ConVar* net_data_block_enabled             = nullptr;
ConVar* net_datablock_networkLossForSlowSpeed = nullptr;
ConVar* net_compressDataBlock              = nullptr;

ConVar* net_showmsg                        = nullptr;
ConVar* net_blockmsg                       = nullptr;
ConVar* net_showpeaks                      = nullptr;
//-----------------------------------------------------------------------------
// RUI |
//-----------------------------------------------------------------------------
// MILES |

//-----------------------------------------------------------------------------
// Purpose: Write every "+<convar> <value>" launch argument onto its ConVar.
//-----------------------------------------------------------------------------
void Bridge_ApplyLaunchConVarTokens(void)
{
	if (!g_pCVar || !CommandLine())
		return;

	const int nParms = CommandLine()->ParmCount();
	for (int i = 0; i < nParms - 1; i++)
	{
		const char* const pszParm = CommandLine()->GetParm(i);
		if (!pszParm || pszParm[0] != '+')
			continue;

		ConVar* const pCVar = g_pCVar->FindVar(pszParm + 1);
		if (!pCVar)
			continue; // unknown name or a ConCommand -- skip

		const char* const pszValue = CommandLine()->GetParm(i + 1);
		if (!pszValue || pszValue[0] == '+')
			continue; // no value token follows this "+convar"

		pCVar->SetValue(pszValue);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Re-apply "+<convar> <value>" after SDK ConVars are registered.
//-----------------------------------------------------------------------------
void Bridge_ApplyLaunchConVarOverrides(void)
{
	Bridge_ApplyLaunchConVarTokens();
}

//-----------------------------------------------------------------------------
// Purpose: initialize shipped ConVar's
//-----------------------------------------------------------------------------
void ConVar_InitShipped(void)
{
	ai_script_nodes_draw             = g_pCVar->FindVar("ai_script_nodes_draw");
	navmesh_move_along_surface_asserts = g_pCVar->FindVar("navmesh_move_along_surface_asserts");
	bhit_enable                      = g_pCVar->FindVar("bhit_enable");
	developer                        = g_pCVar->FindVar("developer");
	fps_max                          = g_pCVar->FindVar("fps_max");
	fps_max_vsync                    = g_pCVar->FindVar("fps_max_vsync");
	base_tickinterval_sp             = g_pCVar->FindVar("base_tickinterval_sp");
	base_tickinterval_mp             = g_pCVar->FindVar("base_tickinterval_mp");
	fs_showAllReads                  = g_pCVar->FindVar("fs_showAllReads");

	//eula_version = g_pCVar->FindVar("eula_version");
	//eula_version_accepted = g_pCVar->FindVar("eula_version_accepted");

	language_cvar                    = g_pCVar->FindVar("language");
	voice_noxplat                    = g_pCVar->FindVar("voice_noxplat");
	platform_user_id                 = g_pCVar->FindVar("platform_user_id");
	single_frame_shutdown_for_reload = g_pCVar->FindVar("single_frame_shutdown_for_reload");
	enable_debug_overlays            = g_pCVar->FindVar("enable_debug_overlays");
	debug_draw_box_depth_test        = g_pCVar->FindVar("debug_draw_box_depth_test");
	model_defaultFadeDistScale       = g_pCVar->FindVar("model_defaultFadeDistScale");
	model_defaultFadeDistMin         = g_pCVar->FindVar("model_defaultFadeDistMin");
	staticProp_no_fade_scalar        = g_pCVar->FindVar("staticProp_no_fade_scalar");
	staticProp_gather_size_weight    = g_pCVar->FindVar("staticProp_gather_size_weight");
	sv_cheats                        = g_pCVar->FindVar("sv_cheats");
	sv_visualizetraces               = g_pCVar->FindVar("sv_visualizetraces");
	sv_visualizetraces_duration      = g_pCVar->FindVar("sv_visualizetraces_duration");
	old_gather_props                 = g_pCVar->FindVar("old_gather_props");
	mp_gamemode                      = g_pCVar->FindVar("mp_gamemode");
	ip_cvar                          = g_pCVar->FindVar("ip");
	hostname                         = g_pCVar->FindVar("hostname");
	hostip                           = g_pCVar->FindVar("hostip");
	hostport                         = g_pCVar->FindVar("hostport");
	host_hasIrreversibleShutdown     = g_pCVar->FindVar("host_hasIrreversibleShutdown");
	host_timescale                   = g_pCVar->FindVar("host_timescale");

	net_data_block_enabled           = g_pCVar->FindVar("net_data_block_enabled");
	net_compressDataBlock            = g_pCVar->FindVar("net_compressDataBlock");
	net_datablock_networkLossForSlowSpeed = g_pCVar->FindVar("net_datablock_networkLossForSlowSpeed");

	net_usesocketsforloopback        = g_pCVar->FindVar("net_usesocketsforloopback");

	net_showmsg = g_pCVar->FindVar("net_showmsg");
	net_blockmsg = g_pCVar->FindVar("net_blockmsg");
	net_showpeaks = g_pCVar->FindVar("net_showpeaks");
	sv_stats = g_pCVar->FindVar("sv_stats");

	sv_maxunlag = g_pCVar->FindVar("sv_maxunlag");
	sv_lagpushticks = g_pCVar->FindVar("sv_lagpushticks");
	sv_clockcorrection_msecs = g_pCVar->FindVar("sv_clockcorrection_msecs");

	sv_updaterate_sp = g_pCVar->FindVar("sv_updaterate_sp");
	sv_updaterate_mp = g_pCVar->FindVar("sv_updaterate_mp");

	sv_showhitboxes = g_pCVar->FindVar("sv_showhitboxes");
	sv_forceChatToTeamOnly = g_pCVar->FindVar("sv_forceChatToTeamOnly");

	sv_single_core_dedi = g_pCVar->FindVar("sv_single_core_dedi");

	sv_voiceenable = g_pCVar->FindVar("sv_voiceenable");
	sv_voiceEcho = g_pCVar->FindVar("sv_voiceEcho");
	sv_alltalk = g_pCVar->FindVar("sv_alltalk");

	sv_clampPlayerFrameTime = g_pCVar->FindVar("sv_clampPlayerFrameTime");

	playerframetimekick_margin = g_pCVar->FindVar("playerframetimekick_margin");
	playerframetimekick_decayrate = g_pCVar->FindVar("playerframetimekick_decayrate");

	player_userCmdsQueueWarning = g_pCVar->FindVar("player_userCmdsQueueWarning");
	player_disallow_negative_frametime = g_pCVar->FindVar("player_disallow_negative_frametime");

	script_server_fps = g_pCVar->FindVar("script_server_fps");
	hudchat_dead_can_only_talk_to_other_dead = g_pCVar->FindVar("hudchat_dead_can_only_talk_to_other_dead");

	sv_updaterate_sp->RemoveFlags(FCVAR_DEVELOPMENTONLY);
	sv_updaterate_mp->RemoveFlags(FCVAR_DEVELOPMENTONLY);

	sv_showhitboxes->SetMin(-1); // Allow user to go over each entity manually without going out of bounds.
	sv_showhitboxes->SetMax(NUM_ENT_ENTRIES - 1);

	sv_forceChatToTeamOnly->RemoveFlags(FCVAR_DEVELOPMENTONLY);
	sv_forceChatToTeamOnly->AddFlags(FCVAR_REPLICATED);
	// Default global chat: bridge SayText is team-flagged and restriction defaults ON.
	sv_forceChatToTeamOnly->SetValue(0);

	sv_single_core_dedi->RemoveFlags(FCVAR_DEVELOPMENTONLY);

	// This gets used for division in code, make sure its never zero
	// to prevent division by zero since this cvar doesn't have a min
	// and code doesn't check for it either.
	script_server_fps->SetMin(0.0001f);

	// This is debugging code and enabled by default.disabled here to
	// save on bandwidth.
	bhit_enable->SetValue(0);
	if (fps_max)
		fps_max->AddFlags(FCVAR_ARCHIVE);
	if (fps_max_vsync)
		fps_max_vsync->RemoveFlags(FCVAR_DEVELOPMENTONLY);

	if (base_tickinterval_sp)
		base_tickinterval_sp->RemoveFlags(FCVAR_DEVELOPMENTONLY);
	if (base_tickinterval_mp)
		base_tickinterval_mp->RemoveFlags(FCVAR_DEVELOPMENTONLY);

	if (mp_gamemode)
	{
		mp_gamemode->RemoveFlags(FCVAR_DEVELOPMENTONLY);

		// Client callback goes through CEngineClient::SetupGamemode; dedi calls it directly.
		mp_gamemode->RemoveChangeCallback(mp_gamemode->GetChangeCallback(0), 0);
		mp_gamemode->InstallChangeCallback(MP_GameMode_Changed_f, nullptr, false);
	}

	if (net_usesocketsforloopback)
		net_usesocketsforloopback->RemoveFlags(FCVAR_DEVELOPMENTONLY);

	// Launch-arg re-apply is after ConVar_Register -- SDK vars are not in g_pCVar yet.
}

//-----------------------------------------------------------------------------
// Purpose: unregister/disable extraneous ConVar's.
//-----------------------------------------------------------------------------
void ConVar_PurgeShipped(void)
{
	const char* pszToPurge[] =
	{
		"bink_materials_enabled",
		"communities_enabled",
		"community_frame_run",
		"ime_enabled",
		"origin_igo_mutes_sound_enabled",
		"twitch_shouldQuery",
		"voice_enabled",
	};

	for (size_t i = 0; i < SDK_ARRAYSIZE(pszToPurge); i++)
	{
		if (ConVar* pCVar = g_pCVar->FindVar(pszToPurge[i]))
		{
			pCVar->SetValue(0);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: clear all hostname ConVar's.
//-----------------------------------------------------------------------------
void ConVar_PurgeHostNames(void)
{
	// Blank by VALUE, not a name list -- hostname ConVars outnumber any hand list.
	const char* const pszUpstreamHosts[] =
	{
		".ea.com",
		".respawn.com",
		".easports.com",
	};

	// The signed identity token is obtained from this one, and it is the only
	// thing proving a connecting player owns the account they claim. Nothing else
	// upstream is needed to play.
	const char* const pszKeep[] =
	{
		"eadpAuth_hostname",
	};

	if (!g_pCVar)
		return;

	int nPurged = 0;
	int nSafety = 0;

	for (ConCommandBase* pBase = g_pCVar->GetCommandList();
		pBase && nSafety++ < 16384; pBase = pBase->GetNext())
	{
		if (pBase->IsCommand())
			continue;

		ConVar* const pCVar = static_cast<ConVar*>(pBase);
		const char* const pszValue = pCVar->GetString();

		if (!pszValue || !pszValue[0])
			continue;

		bool bUpstream = false;

		for (size_t i = 0; i < SDK_ARRAYSIZE(pszUpstreamHosts); i++)
		{
			if (strstr(pszValue, pszUpstreamHosts[i]))
			{
				bUpstream = true;
				break;
			}
		}

		if (!bUpstream)
			continue;

		const char* const pszName = pCVar->GetName();
		bool bKeep = false;

		for (size_t i = 0; i < SDK_ARRAYSIZE(pszKeep); i++)
		{
			if (pszName && V_stricmp(pszName, pszKeep[i]) == 0)
			{
				bKeep = true;
				break;
			}
		}

		if (bKeep)
			continue;

		pCVar->SetValue(NET_IPV4_UNSPEC);
		nPurged++;
	}

	Msg(eDLL_T::ENGINE, "[HOSTS] purged %d upstream endpoint ConVar(s)\n", nPurged);
}

static ConCommand bhit("bhit", BHit_f, "Bullet-hit trajectory debug", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL | FCVAR_CHEAT);


// TODO: move VPK building code to separate file and place this in 'packedstore.cpp'
static ConCommand fs_vpk_mount("fs_vpk_mount", VPK_Mount_f, "Mount a VPK file for FileSystem usage", FCVAR_DEVELOPMENTONLY);
static ConCommand fs_vpk_unmount("fs_vpk_unmount", VPK_Unmount_f, "Unmount a VPK file and clear its cache", FCVAR_DEVELOPMENTONLY);
static ConCommand fs_vpk_pack("fs_vpk_pack", VPK_Pack_f, "Pack a VPK file from current workspace", FCVAR_DEVELOPMENTONLY);
static ConCommand fs_vpk_unpack("fs_vpk_unpack", VPK_Unpack_f, "Unpack all files from a VPK file", FCVAR_DEVELOPMENTONLY);

//-----------------------------------------------------------------------------
// Purpose: shipped ConCommand initialization
//-----------------------------------------------------------------------------
void ConCommand_InitShipped(void)
{
	///---------------------------- [ CALLBACK SWAP ]
	//-------------------------------------------------------------------------
	// ENGINE DLL |
	ConCommand* changelevel = g_pCVar->FindCommand("changelevel");
	ConCommand* map = g_pCVar->FindCommand("map");
	ConCommand* map_background = g_pCVar->FindCommand("map_background");
	ConCommand* ss_map = g_pCVar->FindCommand("ss_map");
	ConCommand* migrateme = g_pCVar->FindCommand("migrateme");
	ConCommand* help = g_pCVar->FindCommand("help");
	ConCommand* convar_list = g_pCVar->FindCommand("convar_list");
	ConCommand* convar_differences = g_pCVar->FindCommand("convar_differences");
	ConCommand* convar_findByFlags = g_pCVar->FindCommand("convar_findByFlags");

	ConCommand* weapon_reparse = g_pCVar->FindCommand("weapon_reparse");


	help->m_fnCommandCallback = CVHelp_f;
	convar_list->m_fnCommandCallback = CVList_f;
	convar_differences->m_fnCommandCallback = CVDiff_f;
	convar_findByFlags->m_fnCommandCallback = CVFlag_f;
	changelevel->m_fnCommandCallback = Host_Changelevel_f;
	changelevel->m_fnCompletionCallback = Host_Changelevel_f_CompletionFunc;

	map->m_fnCompletionCallback = Host_Map_f_CompletionFunc;
	map_background->m_fnCompletionCallback = Host_Background_f_CompletionFunc;
	ss_map->m_fnCompletionCallback = Host_SSMap_f_CompletionFunc;

	// FCVAR_GAMEDLL so a client exec asks the dedi to reparse weapon scripts; FCVAR_CHEAT gates remote use on sv_cheats.
	weapon_reparse->RemoveFlags(FCVAR_CLIENTDLL);
	weapon_reparse->AddFlags(FCVAR_GAMEDLL | FCVAR_CHEAT);


	/// ---------------------------- [ FLAG REMOVAL ]
	//-------------------------------------------------------------------------
	if (!CommandLine()->CheckParm("-devsdk"))
	{
		const char* pszMaskedBases[] =
		{
			"launchplaylist",
			"quit",
			"exit",
			"reload",
			"restart",
			"set",
			"status",
			"version",
		};

		for (size_t i = 0; i < SDK_ARRAYSIZE(pszMaskedBases); i++)
		{
			if (ConCommandBase* pCommandBase = g_pCVar->FindCommandBase(pszMaskedBases[i]))
			{
				pCommandBase->RemoveFlags(FCVAR_DEVELOPMENTONLY);
			}
		}

		convar_list->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		convar_differences->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		convar_findByFlags->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		help->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		migrateme->RemoveFlags(FCVAR_SERVER_CAN_EXECUTE);
		changelevel->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		map->RemoveFlags(FCVAR_DEVELOPMENTONLY | FCVAR_SERVER_CAN_EXECUTE);
		map_background->RemoveFlags(FCVAR_DEVELOPMENTONLY | FCVAR_SERVER_CAN_EXECUTE);
		ss_map->RemoveFlags(FCVAR_DEVELOPMENTONLY | FCVAR_SERVER_CAN_EXECUTE);
	}
}

//-----------------------------------------------------------------------------
// Purpose: unregister extraneous ConCommand's.
//-----------------------------------------------------------------------------
void ConCommand_PurgeShipped(void)
{
	const char* pszCommandToRemove[] =
	{
		"bind",
		"bind_held",
		"bind_list",
		"bind_list_abilities",
		"bind_US_standard",
		"bind_held_US_standard",
		"unbind",
		"unbind_US_standard",
		"unbindall",
		"unbind_all_gamepad",
		"unbindall_ignoreGamepad",
		"unbind_batch",
		"unbind_held",
		"unbind_held_US_standard",
		"uiscript_reset",
		"getpos_bind",
		"connect",
		"silent_connect",
		"set",
		"ping",
		"gameui_activate",
		"gameui_hide",
		"weaponSelectOrdnance",
		"weaponSelectPrimary0",
		"weaponSelectPrimary1",
		"weaponSelectPrimary2",
		"+scriptCommand1",
		"-scriptCommand1",
		"+scriptCommand2",
		"-scriptCommand2",
		"+scriptCommand3",
		"-scriptCommand3",
		"+scriptCommand4",
		"-scriptCommand4",
		"+scriptCommand5",
		"-scriptCommand5",
		"+scriptCommand6",
		"-scriptCommand6",
		"+scriptCommand7",
		"-scriptCommand7",
		"+scriptCommand8",
		"-scriptCommand8",
		"+scriptCommand9",
		"-scriptCommand9",
	};

	for (size_t i = 0; i < SDK_ARRAYSIZE(pszCommandToRemove); i++)
	{
		ConCommandBase* pCommandBase = g_pCVar->FindCommandBase(pszCommandToRemove[i]);

		if (pCommandBase)
		{
			g_pCVar->UnregisterConCommand(pCommandBase);
		}
	}
}
#endif // CLIENT_DLL
