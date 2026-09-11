//=============================================================================//
// 
// Purpose: server host manager
// 
//-----------------------------------------------------------------------------
//
//=============================================================================//
#include "tier0/frametask.h"
#include "common/callback.h"
#include "rtech/playlists/playlists.h"
#include "engine/cmd.h"
#include "engine/cmodel_bsp.h"
#include "engine/host_state.h"
#include "hostmanager.h"

#ifndef CLIENT_DLL
static char s_szDeferredPlaylist[32];
#endif // !CLIENT_DLL

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
CServerHostManager::CServerHostManager(void)
    : m_HostingStatus(HostStatus_e::NOT_HOSTING)
{
}

//-----------------------------------------------------------------------------
// Purpose: internal server launch handler
//-----------------------------------------------------------------------------
static void HostManager_HandleCommandInternal(const char* const map, const char* const mode, const bool changeLevel)
{
    Assert(!ThreadInServerFrameThread(), "Use server script GameRules_ChangeMap() instead!");

    Msg(eDLL_T::ENGINE, "Starting server with name: \"%s\" map: \"%s\" mode: \"%s\"\n",
        hostname->GetString(), map, mode);

    bool hasPendingMap = *g_pPlaylistMapToLoad != '\0';

    // NOTE: when the provided playlist is the same as the one we're currently
    // on, and there's already a pending map load request, the game will run 
    // "map <mapName>" in Playlists_Parse, where the map name is dictated by
    // g_pPlaylistMapToLoad. If changelevel was specified, we have to null the
    // requested map here as to prevent Playlists_Parse from running the map
    // command on it, as we are going to run the changelevel command anyways.
    // Not doing this will result in running both map and changelevel commands.
    if (changeLevel && hasPendingMap)
    {
        *g_pPlaylistMapToLoad = '\0';
        hasPendingMap = false;
    }

    char commandBuf[512];

#ifndef CLIENT_DLL
    // Playlists_Parse remounts vpk/server_<playlistmap>.bsp. On a live world
    // that unloads the resident map rpak while HS_RUN still traces it.
    if (changeLevel && g_pHostState && g_pHostState->IsActiveGame())
    {
        snprintf(s_szDeferredPlaylist, sizeof(s_szDeferredPlaylist), "%s", mode);
        mp_gamemode->SetValue(mode);
        snprintf(commandBuf, sizeof(commandBuf), "changelevel %s\n", map);
        Cbuf_AddText(Cbuf_GetCurrentPlayer(), commandBuf, cmd_source_t::kCommandSrcCode);
        return;
    }
#endif // !CLIENT_DLL

    const bool samePlaylist = v_Playlists_Parse(mode);

    mp_gamemode->SetValue(mode);

    if (!samePlaylist || !hasPendingMap)
    {
        snprintf(commandBuf, sizeof(commandBuf), "%s %s\n", changeLevel ? "changelevel" : "map", map);
        Cbuf_AddText(Cbuf_GetCurrentPlayer(), commandBuf, cmd_source_t::kCommandSrcCode);
    }
}

#ifndef CLIENT_DLL
void HostManager_ParseDeferredPlaylist(void)
{
    if (!s_szDeferredPlaylist[0] || !v_Playlists_Parse)
        return;

    if (g_pPlaylistMapToLoad)
        *g_pPlaylistMapToLoad = '\0';

    Msg(eDLL_T::SERVER, "[BRIDGE-MODE] Playlists_Parse('%s') after LevelShutdown\n",
        s_szDeferredPlaylist);
    Playlists_Parse(s_szDeferredPlaylist);

    // mp_gamemode carries a PLAYLIST name; SetupGamemode only turns that into the
    // playlist's gamemode by looking the name up against the CURRENT playlist, which
    // was still the outgoing one when the value was set above the changelevel.
    // Re-apply it now the new playlist is resident, or GameRules keeps the previous
    // gamemode whenever the two names differ (survival_dev -> survival).
    if (mp_gamemode && v_SetupGamemode)
    {
        mp_gamemode->SetValue(s_szDeferredPlaylist);
        v_SetupGamemode(mp_gamemode->GetString());

        Msg(eDLL_T::SERVER, "[BRIDGE-MODE] gamemode '%s' for playlist '%s'\n",
            mp_gamemode->GetString(), s_szDeferredPlaylist);
    }

    s_szDeferredPlaylist[0] = '\0';
}
#endif // !CLIENT_DLL

//-----------------------------------------------------------------------------
// Purpose: Launch server with given parameters
//-----------------------------------------------------------------------------
void CServerHostManager::LaunchServer(const char* const map, const char* const mode) const
{
    HostManager_HandleCommandInternal(map, mode, false);
}

//-----------------------------------------------------------------------------
// Purpose: Change level with given parameters
//-----------------------------------------------------------------------------
void CServerHostManager::ChangeLevel(const char* const map, const char* const mode) const
{
    HostManager_HandleCommandInternal(map, mode, true);
}

CServerHostManager g_ServerHostManager;

#ifndef CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: playlist + map identifiers may only contain these characters. The
// value reaches Cbuf_AddText through HandleCommandInternal, so a name carrying
// ';' or whitespace would append a second console command.
//-----------------------------------------------------------------------------
static bool HostManager_IsSafeIdentifier(const char* const pszName)
{
    if (!pszName || !pszName[0])
        return false;

    for (const char* p = pszName; *p; p++)
    {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            continue;

        return false;
    }

    return true;
}

static bool HostManager_PlaylistExists(const char* const pszPlaylist)
{
    for (const CUtlString& it : g_vecAllPlaylists)
    {
        if (it.IsEqual_CaseSensitive(pszPlaylist))
            return true;
    }

    return false;
}

static bool HostManager_MapInstalled(const char* const pszMap)
{
    AUTO_LOCK(g_InstalledMapsMutex);

    FOR_EACH_VEC(g_InstalledMaps, i)
    {
        if (g_InstalledMaps[i].IsEqual_CaseSensitive(pszMap))
            return true;
    }

    return false;
}

/*
=====================
Host_BridgeSetMode_f

  Applies a playlist and changes level in one step, the ordering
  HandleCommandInternal requires. Reachable from the S21 client's lobby through
  "bridge_rcon bridge_setmode <playlist> <map>", so both arguments are validated
  here regardless of what the sender promises.
=====================
*/
static void Host_BridgeSetMode_f(const CCommand& args)
{
    if (args.ArgC() != 3)
    {
        Warning(eDLL_T::SERVER, "[BRIDGE-MODE] usage: bridge_setmode <playlist> <map>\n");
        return;
    }

    const char* const pszPlaylist = args.Arg(1);
    const char* const pszMap = args.Arg(2);

    if (!HostManager_IsSafeIdentifier(pszPlaylist) || !HostManager_IsSafeIdentifier(pszMap))
    {
        Warning(eDLL_T::SERVER, "[BRIDGE-MODE] rejected: identifier outside [a-z0-9_]\n");
        return;
    }

    if (!HostManager_PlaylistExists(pszPlaylist))
    {
        Warning(eDLL_T::SERVER, "[BRIDGE-MODE] rejected: playlist '%s' not in the loaded playlist file\n",
            pszPlaylist);
        return;
    }

    if (!HostManager_MapInstalled(pszMap))
    {
        Warning(eDLL_T::SERVER, "[BRIDGE-MODE] rejected: map '%s' is not installed\n", pszMap);
        return;
    }

    Msg(eDLL_T::SERVER, "[BRIDGE-MODE] playlist '%s' map '%s'\n", pszPlaylist, pszMap);
    g_ServerHostManager.ChangeLevel(pszMap, pszPlaylist);
}

static ConCommand bridge_setmode("bridge_setmode", Host_BridgeSetMode_f,
    "Apply a playlist and change level in one step. Usage: bridge_setmode <playlist> <map>",
    FCVAR_RELEASE, nullptr, "bridge_setmode <playlist> <map>");
#endif // !CLIENT_DLL
