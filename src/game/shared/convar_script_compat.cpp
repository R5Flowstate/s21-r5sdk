//=============================================================================//
//
// Purpose: Create-if-absent ConVars that game scripts read
//
// These were carried by the SDK.Core mod's ConVars block, which meant every
// install had to ship a mod folder before stock scripts would work. They are
// part of the product, so they live here.
//
// The mod system only created a name the engine did not already own. That
// guard is kept: another owner wins and the entry below is a fallback, so
// registration has to run after ConVar_Register when the engine's list is
// visible.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"

struct ScriptConVarDef_t
{
	const char* pszName;
	const char* pszDefault;
	const char* pszHelp;
	bool  bMin;
	float flMin;
	bool  bMax;
	float flMax;
};

// An empty bound in the source manifest is not a bound. Registering min=max=0
// on the two string-valued entries would clamp their text through the float
// path, so those carry no range.
static const ScriptConVarDef_t s_ScriptConVars[] =
{
	{ "sh_odsl_test_time_sec", "60", "", true , 0.f, true , 100.f },
	{ "sh_odsl_enabled_test_name", "", "", false, 0.f, false, 0.f },
	{ "mastery_unlock_all_trials", "0", "", true , 0.f, true , 1.f },
	{ "script_ranked_debug", "0", "", true , 0.f, true , 1.f },
	{ "script_bypass_bot_checks", "0", "S21 PrestigeBadgeIsAllowed: skip bot presence gate when true", true , 0.f, true , 1.f },
	{ "script_enable_twitch_drops_clawback", "0", "S21 entitlements twitch drops clawback", true , 0.f, true , 1.f },
	{ "script_ftue_skip_orientation_ab_test", "0", "S21 FTUE orientation A/B skip", true , 0.f, true , 1.f },
	{ "script_ftue_skip_training_ab_test", "0", "S21 FTUE training A/B skip", true , 0.f, true , 1.f },
	{ "script_mover_traversal_mover_support", "1", "S21 shield-throw / traversal mover support", true , 0.f, true , 1.f },
	{ "mtx_progression_modifier_dev_boosts_enabled", "0", "", true , 0.f, true , 1.f },
	{ "hover_vehicle_air_acceleration", "250.0", "", true , 0.0f, true , 2000.0f },
	{ "hover_vehicle_deceleration_powerbreaking", "1000.0", "", true , 0.0f, true , 2000.0f },
	{ "hover_vehicle_deceleration", "550.0", "", true , 0.0f, true , 1000.0f },
	{ "hover_vehicle_acceleration", "750.0", "", true , 0.0f, true , 1000.0f },
	{ "hover_vehicle_boost_cooldown", "25.0", "", true , 0.0f, true , 100.0f },
	{ "hover_vehicle_boost_speed_max", "2000", "", true , 0.f, true , 5000.f },
	{ "hover_vehicle_boost_speed_min", "1200", "", true , 0.f, true , 5000.f },
	{ "hover_vehicle_speed", "1000", "", true , 0.f, true , 5000.f },
	{ "match_trackMMR", "0", "", true , 0.f, true , 1.f },
	{ "net_traceroute", "0", "", true , 0.f, true , 1.f },
	{ "ranked_rumble_enabled", "0", "", true , 0.f, true , 1.f },
	{ "fast_intro", "0", "", true , 0.f, true , 1.f },
	{ "cups_enabled", "1", "", true , 0.f, true , 1.f },
	{ "kepler_isEnabled", "1", "", true , 0.f, true , 1.f },
	{ "cl_liveapi_enabled", "0", "Enable or disable liveapi", true , 0.f, true , 1.f },
	{ "liveapi_session", "0", "liveapi session", true , 0.f, true , 1.f },
	{ "cl_ezlaunch_button", "1", "", true , 0.f, true , 1.f },
	{ "sv_infinite_ammo", "0", "Infinite Ammo", true , 0.f, true , 1.f },
	{ "toggle_on_jump_to_deactivate", "0", "Displays healthbars", true , 0.f, true , 1.f },
	{ "toggle_on_jump_to_deactivate_changed", "0", "Displays healthbars", true , 0.f, true , 1.f },
	{ "match_forcePostMatchSurvey", "0", "Displays healthbars", true , 0.f, true , 1.f },
	{ "matchmake_from_match_enabled", "1", "Displays healthbars", true , 0.f, true , 1.f },
	{ "hud_setting_showHopUpPopUp", "1", "Displays healthbars", true , 0.f, true , 1.f },
	{ "serverbrowser_gameModeFilter", "0", "Displays healthbars", true , 0.f, true , 1.f },
	{ "serverbrowser_mapFilter", "0", "Displays healthbars", true , 0.f, true , 1.f },
	{ "serverbrowser_hideEmptyServers", "1", "Displays healthbars", true , 0.f, true , 1.f },
	{ "enable_healthbar", "1", "Displays healthbars", true , 0.f, true , 1.f },
	{ "customMatch_enabled", "0", "S21 custom match gate (S3 stub)", true , 0.f, true , 1.f },
	{ "customMatch_fastStart", "0", "S21 custom match fast start (S3 stub)", true , 0.f, true , 1.f },
	{ "matchSquadRequeue_enabled", "0", "S21 squad requeue (S3 stub)", true , 0.f, true , 1.f },
	{ "matchSquadRequeue_timeLimit", "0", "S21 squad requeue time limit seconds (S3 stub)", true , 0.f, true , 3600.f },
	{ "mtx_giftingEnabled", "0", "S21 gifting enabled (S3 stub)", true , 0.f, true , 1.f },
	{ "mtx_giftingForce2FA", "0", "S21 gifting 2FA force (S3 stub)", true , 0.f, true , 1.f },
	{ "mtx_giftingLimit", "0", "S21 gifting limit (S3 stub)", true , 0.f, true , 100.f },
	{ "mtx_giftingMinAccountLevel", "0", "S21 gifting min account level (S3 stub)", true , 0.f, true , 500.f },
	{ "mtx_giftingMinFriendshipInDays", "0", "S21 gifting min friendship days (S3 stub)", true , 0.f, true , 3650.f },
	{ "mtx_gifting_notifications_enabled", "0", "S21 gifting notifications (S3 stub)", true , 0.f, true , 1.f },
	{ "mtx_useClientContainers", "0", "S21 GRX client containers (S3 stub)", true , 0.f, true , 1.f },
	{ "persistence_development_mode", "0", "S21 persistence dev mode (S3 stub)", true , 0.f, true , 1.f },
	{ "persistence_run_pdef_autogen_on_connect", "0", "S21 pdef autogen on connect (S3 stub)", true , 0.f, true , 1.f },
	{ "loadouts_ignore_persistence", "0", "S21 loadouts ignore persistence (S3 stub)", true , 0.f, true , 1.f },
	{ "ranked_assert_on_invalid_client_data", "0", "S21 ranked assert invalid client data (S3 stub)", true , 0.f, true , 1.f },
	{ "ranked_disable_full_bonus_system", "0", "S21 ranked disable full bonus system (S3 stub)", true , 0.f, true , 1.f },
	{ "ranked_enable_old_kill_bonus", "0", "S21 ranked old kill bonus (S3 stub)", true , 0.f, true , 1.f },
	{ "ranked_rumble_skip_orientation", "0", "S21 ranked rumble skip orientation (S3 stub)", true , 0.f, true , 1.f },
	{ "ranked_rumble_skip_training", "0", "S21 ranked rumble skip training (S3 stub)", true , 0.f, true , 1.f },
	{ "kepler_forceNotReady", "0", "S21 kepler force not ready (S3 stub)", true , 0.f, true , 1.f },
	{ "kepler_forceVariant", "", "S21 kepler force variant string (S3 stub)", false, 0.f, false, 0.f },
	{ "cups_has_post_match", "0", "S21 cups post-match (S3 stub)", true , 0.f, true , 1.f },
	{ "spawnpoint_debug", "0", "S21 spawnpoint debug draw (S3 stub)", true , 0.f, true , 1.f },
	{ "sv_edict_cleanup_display_warnings", "0", "S21 edict cleanup warnings (S3 stub)", true , 0.f, true , 1.f },
	{ "sv_tournament_anonymous_mode", "0", "S21 tournament anonymous mode (S3 stub)", true , 0.f, true , 1.f },
	{ "autoplayer_gameplay_mode", "0", "S21 autoplayer gameplay mode (S3 stub)", true , 0.f, true , 1.f },
	{ "gladCards_debug", "0", "S21 gladiator cards debug (S3 stub)", true , 0.f, true , 1.f },
	{ "player_use_prompt_enabled", "1", "S21 use prompt enabled (S3 stub)", true , 0.f, true , 1.f },
	{ "test_fakeTimeStamp", "0", "S21 fake timestamp (S3 stub)", true , 0.f, true , 9999999999.f },
	{ "threat_detection_view_dist_aim", "0", "S21 threat detection aim dist (S3 stub)", true , 0.f, true , 10000.f },
	{ "threat_detection_view_dist_snipe", "0", "S21 threat detection snipe dist (S3 stub)", true , 0.f, true , 10000.f },
};

#if defined(CLIENT_DLL)
//-----------------------------------------------------------------------------
// The client injects before the S21 exe registers its static ConVars, so
// FindCommandBase cannot see them yet. They wait on the exe's own pending
// list; a stub made under one of those names becomes the registered parent
// and the native (with its flags, e.g. FCVAR_USERINFO) is linked beneath it.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t PENDING_CVAR_OFF_NEXT = 0x08;
static constexpr ptrdiff_t PENDING_CVAR_OFF_NAME = 0x18;

static bool ScriptConVar_ExePendingHas(const char* pszName)
{
	// Static ConVar initializer: mov rax,[head]; mov [cv+8],rax; ... mov [head],rsi.
	static const CMemory s_headLoad = Module_FindPattern(g_GameDll,
		"48 8B 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 48 C7 05 ?? ?? ?? ?? 00 00 00 00 C7 05 ?? ?? ?? ?? 00 02 00 01 48 89 35");
	if (!s_headLoad)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::CLIENT, "[CVAR-STUB] exe pending ConVar list unresolved -- script stubs may shadow natives\n");
		}
		return false;
	}

	const uintptr_t* const pHead = s_headLoad.ResolveRelativeAddress(3, 7).RCast<const uintptr_t*>();
	uintptr_t node = *pHead;
	for (int safety = 0; node && safety < 32768; ++safety)
	{
		const char* const pszNode = *reinterpret_cast<const char* const*>(node + PENDING_CVAR_OFF_NAME);
		if (pszNode && !V_strcmp(pszNode, pszName))
			return true;
		node = *reinterpret_cast<const uintptr_t*>(node + PENDING_CVAR_OFF_NEXT);
	}
	return false;
}
#endif // CLIENT_DLL

void ConVarStubs_InitScriptConVars()
{
	if (!g_pCVar)
		return;

	for (const ScriptConVarDef_t& def : s_ScriptConVars)
	{
		if (g_pCVar->FindCommandBase(def.pszName))
			continue;
#if defined(CLIENT_DLL)
		if (ScriptConVar_ExePendingHas(def.pszName))
			continue;
#endif // CLIENT_DLL

		// Owned by the engine's ConVar list for the life of the process.
		new ConVar(def.pszName, def.pszDefault, FCVAR_ARCHIVE | FCVAR_RELEASE,
			def.pszHelp, def.bMin, def.flMin, def.bMax, def.flMax, nullptr, nullptr);
	}
}
