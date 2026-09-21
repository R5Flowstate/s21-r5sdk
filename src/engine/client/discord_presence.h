#pragma once

//=============================================================================//
//
// Purpose: Discord rich presence state for the client
//
//=============================================================================//
#ifndef DEDICATED

#include "core/stdafx.h"

// Presence is derived per frame from the host state machine (menu, loading,
// in game). No server name or player counts are published: the client never
// holds honest counts, and a remote hostname on a public profile is attacker
// text. Party and secrets are never sent.
class CDiscordPresence
{
public:
	static void Initialize(void);
	static void Shutdown(void);
	static void Update(void);
	static void ClearPresence(void);
	static bool IsEnabled(void);
	static bool IsConnected(void);

private:
	static void UpdateLocked(void);
	static void DeriveGameState(void);
	static void PushPresence(void);
	static void OnDiscordReady(const struct DiscordRPCUser* user);
	static void OnDiscordDisconnected(int errorCode, const char* message);
	static void OnDiscordError(int errorCode, const char* message);

	static bool s_bInitialized;
	static bool s_bConnected;
	static char s_szCurrentState[128];
	static char s_szCurrentDetails[128];
	static char s_szCurrentMap[64];
	static char s_szCurrentPlaylist[64];
	static int64_t s_nStartTime;
	static bool s_bNeedsUpdate;
};

#endif // !DEDICATED
