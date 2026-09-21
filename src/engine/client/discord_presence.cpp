//=============================================================================//
//
// Purpose: Discord rich presence state for the client
//
//=============================================================================//
#include "core/stdafx.h"

#ifndef DEDICATED

#include "discord_presence.h"
#include "discord_rpc_wrapper.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "rtech/playlists/playlists.h"
#include "engine/client/net_bridge_internal.h"
#include <chrono>

extern bool S21Bridge_IsActive();
extern const char* S21Bridge_GetCurrentPlaylistName(void);

#define DISCORD_APP_ID "1550576003808632902"
#define DISCORD_LARGE_IMAGE_KEY "r5f_logo"
#define DISCORD_LARGE_IMAGE_TEXT "R5Flowstate"
#define DISCORD_BUTTON_SITE_LABEL "Play R5Flowstate"
#define DISCORD_BUTTON_SITE_URL "https://play.r5flowstate.org"

ConVar discord_enable("discord_enable", "1", FCVAR_RELEASE, "Enable Discord Rich Presence updates.");
static ConVar discord_button_invite("discord_button_invite", "", FCVAR_RELEASE, "Discord invite URL shown as the second presence button (empty = no button).");
static ConVar discord_debug("discord_debug", "0", FCVAR_DEVELOPMENTONLY, "Log Discord Rich Presence pipe and payload activity.");

bool CDiscordPresence::s_bInitialized = false;
bool CDiscordPresence::s_bConnected = false;
char CDiscordPresence::s_szCurrentState[128] = { 0 };
char CDiscordPresence::s_szCurrentDetails[128] = { 0 };
char CDiscordPresence::s_szCurrentMap[64] = { 0 };
char CDiscordPresence::s_szCurrentPlaylist[64] = { 0 };
int64_t CDiscordPresence::s_nStartTime = 0;
bool CDiscordPresence::s_bNeedsUpdate = false;

//-----------------------------------------------------------------------------
// Purpose: Initialize Discord Rich Presence
//-----------------------------------------------------------------------------
void CDiscordPresence::Initialize(void)
{
	if (s_bInitialized)
		return;

	if (!IsEnabled())
	{
		DevMsg(eDLL_T::CLIENT, "[DISCORD] Rich Presence disabled\n");
		return;
	}

	DiscordEventHandlers handlers = {};
	handlers.ready = OnDiscordReady;
	handlers.disconnected = OnDiscordDisconnected;
	handlers.errored = OnDiscordError;

	DevMsg(eDLL_T::CLIENT, "[DISCORD] Initializing Rich Presence (app %s)\n", DISCORD_APP_ID);

	Discord_Initialize(DISCORD_APP_ID, &handlers, 1, nullptr);

	s_bInitialized = true;
	s_nStartTime = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();

	DevMsg(eDLL_T::CLIENT, "[DISCORD] Rich Presence initialized\n");
}

//-----------------------------------------------------------------------------
// Purpose: Shutdown Discord Rich Presence
//-----------------------------------------------------------------------------
void CDiscordPresence::Shutdown(void)
{
	if (!s_bInitialized)
		return;

	Discord_ClearPresence();
	Discord_Shutdown();
	s_bInitialized = false;
	s_bConnected = false;

	DevMsg(eDLL_T::CLIENT, "[DISCORD] Rich Presence shutdown\n");
}

//-----------------------------------------------------------------------------
// Purpose: Update Discord Rich Presence (called every frame)
//-----------------------------------------------------------------------------
void CDiscordPresence::Update(void)
{
	static volatile LONG s_nBusy = 0;
	static double s_flNextTick = 0.0;

	if (InterlockedCompareExchange(&s_nBusy, 1, 0) != 0)
		return;

	const double flNow = Plat_FloatTime();
	if (flNow < s_flNextTick)
	{
		InterlockedExchange(&s_nBusy, 0);
		return;
	}
	s_flNextTick = flNow + 0.25;

	UpdateLocked();
	InterlockedExchange(&s_nBusy, 0);
}

void CDiscordPresence::UpdateLocked(void)
{
	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		Msg(eDLL_T::CLIENT, "[DISCORD] pump alive: enable=%d nopresence=%d\n",
			discord_enable.GetBool() ? 1 : 0, CommandLine()->CheckParm("-nopresence") ? 1 : 0);
	}

	if (!s_bInitialized)
	{
		// Lazy init: a convar flipped on after boot still takes effect.
		if (!IsEnabled())
			return;

		Initialize();
		if (!s_bInitialized)
			return;
	}
	else if (!IsEnabled())
	{
		ClearPresence();
		Shutdown();
		return;
	}

	Discord_RunCallbacks();

	DeriveGameState();

	if (s_bNeedsUpdate)
	{
		PushPresence();
		s_bNeedsUpdate = false;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Clear Discord presence and drop cached state
//-----------------------------------------------------------------------------
void CDiscordPresence::ClearPresence(void)
{
	if (!s_bInitialized)
		return;

	Discord_ClearPresence();

	s_szCurrentState[0] = '\0';
	s_szCurrentDetails[0] = '\0';
	s_szCurrentMap[0] = '\0';
	s_szCurrentPlaylist[0] = '\0';
	s_bNeedsUpdate = false;
}

//-----------------------------------------------------------------------------
// Purpose: Check if Discord Rich Presence is enabled
//-----------------------------------------------------------------------------
bool CDiscordPresence::IsEnabled(void)
{
	return discord_enable.GetBool() && !CommandLine()->CheckParm("-nopresence");
}

//-----------------------------------------------------------------------------
// Purpose: Check if Discord is connected
//-----------------------------------------------------------------------------
bool CDiscordPresence::IsConnected(void)
{
	return s_bConnected;
}

//-----------------------------------------------------------------------------
// Purpose: Push cached state to Discord
//-----------------------------------------------------------------------------
void CDiscordPresence::PushPresence(void)
{
	if (!s_bConnected || !IsEnabled())
		return;

	DiscordRichPresence presence = {};

	if (s_szCurrentState[0])
		presence.state = s_szCurrentState;

	if (s_szCurrentDetails[0])
		presence.details = s_szCurrentDetails;

	presence.startTimestamp = s_nStartTime;
	presence.largeImageKey = DISCORD_LARGE_IMAGE_KEY;
	presence.largeImageText = DISCORD_LARGE_IMAGE_TEXT;
	presence.button0Label = DISCORD_BUTTON_SITE_LABEL;
	presence.button0Url = DISCORD_BUTTON_SITE_URL;

	const char* const pszInvite = discord_button_invite.GetString();
	if (pszInvite && V_strnicmp(pszInvite, "https://", 8) == 0)
	{
		presence.button1Label = "Join the Discord";
		presence.button1Url = pszInvite;
	}

	if (discord_debug.GetBool())
	{
		DevMsg(eDLL_T::CLIENT, "[DISCORD] push state='%s' details='%s'\n",
			s_szCurrentState, s_szCurrentDetails);
	}

	Discord_UpdatePresence(&presence);
}

//-----------------------------------------------------------------------------
// Purpose: playlist var lookup for display labels (r5f_mode_title, r5f_mode_map_title)
//-----------------------------------------------------------------------------
static const char* Presence_PlaylistVar(const char* pszPlaylist, const char* pszVar)
{
	if (!pszPlaylist || !pszPlaylist[0])
		return nullptr;

	KeyValues* const pRoot = Playlists_GetRootKV();
	KeyValues* const pPlaylists = pRoot ? pRoot->FindKey("Playlists") : nullptr;
	KeyValues* const pPl = pPlaylists ? pPlaylists->FindKey(pszPlaylist) : nullptr;
	KeyValues* const pVars = pPl ? pPl->FindKey("vars") : nullptr;
	const char* const pszValue = pVars ? pVars->GetString(pszVar, "") : "";
	if (!pszValue || !pszValue[0] || pszValue[0] == '#')
		return nullptr;

	return pszValue;
}

//-----------------------------------------------------------------------------
// Purpose: Derive display state from the host state machine
//-----------------------------------------------------------------------------
void CDiscordPresence::DeriveGameState(void)
{
	if (!s_bInitialized)
		return;

	char szState[128] = { 0 };
	char szDetails[128] = { 0 };
	char szMap[64] = { 0 };
	char szPlaylist[64] = { 0 };

	const bool bBridge = S21Bridge_IsActive();
	bool bInGame = false;
	if (bBridge)
	{
		const uintptr_t nFlag = NetObs_InGameFlagAddr();
		if (nFlag)
			bInGame = *reinterpret_cast<const unsigned char*>(nFlag) != 0;
	}

	const char* pszLevel = bBridge ? Bridge_GetLevelBaseName() : "";
	if (!pszLevel)
		pszLevel = "";
	const bool bHasLevel = pszLevel[0] && _strnicmp(pszLevel, "mp_lobby", 8) != 0;

	if (bBridge && bHasLevel)
	{
		V_strncpy(szMap, pszLevel, sizeof(szMap));

		const char* pszPlaylist = S21Bridge_GetCurrentPlaylistName();
		if (!pszPlaylist)
			pszPlaylist = "";
		V_strncpy(szPlaylist, pszPlaylist, sizeof(szPlaylist));

		const char* pszMapTitle = Presence_PlaylistVar(szPlaylist, "r5f_mode_map_title");
		V_strncpy(szState, pszMapTitle ? pszMapTitle : pszLevel, sizeof(szState));

		if (!bInGame)
		{
			V_strncpy(szDetails, "Loading", sizeof(szDetails));
		}
		else
		{
			const char* pszMode = Presence_PlaylistVar(szPlaylist, "r5f_mode_title");
			if (!pszMode)
				pszMode = Presence_PlaylistVar(szPlaylist, "name");
			if (!pszMode)
				pszMode = szPlaylist[0] ? szPlaylist : "In game";
			V_strncpy(szDetails, pszMode, sizeof(szDetails));
		}
	}
	else if (bBridge)
	{
		V_strncpy(szState, "Connecting", sizeof(szState));
		V_strncpy(szDetails, "R5Flowstate", sizeof(szDetails));
	}
	else
	{
		V_strncpy(szState, "In menu", sizeof(szState));
		V_strncpy(szDetails, "R5Flowstate", sizeof(szDetails));
	}

	bool changed = false;

	if (strcmp(s_szCurrentState, szState) != 0)
	{
		V_strncpy(s_szCurrentState, szState, sizeof(s_szCurrentState));
		changed = true;
	}
	if (strcmp(s_szCurrentDetails, szDetails) != 0)
	{
		V_strncpy(s_szCurrentDetails, szDetails, sizeof(s_szCurrentDetails));
		changed = true;
	}
	if (strcmp(s_szCurrentMap, szMap) != 0)
	{
		V_strncpy(s_szCurrentMap, szMap, sizeof(s_szCurrentMap));
		s_nStartTime = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		changed = true;
	}
	if (strcmp(s_szCurrentPlaylist, szPlaylist) != 0)
	{
		V_strncpy(s_szCurrentPlaylist, szPlaylist, sizeof(s_szCurrentPlaylist));
		changed = true;
	}

	if (changed)
		s_bNeedsUpdate = true;
}

//-----------------------------------------------------------------------------
// Purpose: Discord ready callback
//-----------------------------------------------------------------------------
void CDiscordPresence::OnDiscordReady(const DiscordRPCUser* user)
{
	s_bConnected = true;
	s_bNeedsUpdate = true;

	if (user && user->username)
	{
		char safeUsername[64] = { 0 };
		V_strncpy(safeUsername, user->username, sizeof(safeUsername));
		safeUsername[sizeof(safeUsername) - 1] = '\0';

		DevMsg(eDLL_T::CLIENT, "[DISCORD] Rich Presence connected (user: %s)\n", safeUsername);
	}
	else
	{
		DevMsg(eDLL_T::CLIENT, "[DISCORD] Rich Presence connected\n");
	}
}

//-----------------------------------------------------------------------------
// Purpose: Discord disconnected callback
//-----------------------------------------------------------------------------
void CDiscordPresence::OnDiscordDisconnected(int errorCode, const char* message)
{
	s_bConnected = false;

	if (message)
	{
		char safeMessage[256] = { 0 };
		V_strncpy(safeMessage, message, sizeof(safeMessage));
		safeMessage[sizeof(safeMessage) - 1] = '\0';

		DevMsg(eDLL_T::CLIENT, "[DISCORD] Rich Presence disconnected (%d: %s)\n", errorCode, safeMessage);
	}
	else
	{
		DevMsg(eDLL_T::CLIENT, "[DISCORD] Rich Presence disconnected (%d)\n", errorCode);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Discord error callback
//-----------------------------------------------------------------------------
void CDiscordPresence::OnDiscordError(int errorCode, const char* message)
{
	if (message)
	{
		char safeMessage[256] = { 0 };
		V_strncpy(safeMessage, message, sizeof(safeMessage));
		safeMessage[sizeof(safeMessage) - 1] = '\0';

		DevWarning(eDLL_T::CLIENT, "[DISCORD] Rich Presence error (%d: %s)\n", errorCode, safeMessage);
	}
	else
	{
		DevWarning(eDLL_T::CLIENT, "[DISCORD] Rich Presence error (%d)\n", errorCode);
	}
}

#endif // !DEDICATED
