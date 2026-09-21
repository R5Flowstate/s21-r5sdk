//=============================================================================//
//
// Purpose: Expose native code to VScript API
//
//-----------------------------------------------------------------------------
//
// Read the documentation in 'game/shared/vscript_shared.cpp' before modifying
// existing code or adding new code!
//
// To create server script bindings
// - use the DEFINE_SERVER_SCRIPTFUNC_NAMED macro.
// - prefix your function with "ServerScript_" i.e.: "ServerScript_GetVersion".
//
//=============================================================================//

#include "core/stdafx.h"
#include "common/callback.h"
#include "game/shared/scriptnetdata_limits.h"
#include "engine/server/server.h"
#include "engine/server/sv_main.h"
#include "engine/host_state.h"
#include "engine/debugoverlay.h"
#include "pluginsystem/pluginsystem.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"

#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/globalnonrewind_vars.h"
#include "game/shared/weapon_heat.h"
#include "game/server/energize.h"
#include "game/server/akimbo.h"
#include "game/shared/deathfield_system.h"
#include "game/shared/alliance_compat.h"
#include "game/shared/highlight_context.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"

#include "game/shared/scriptremotefunctions_server.h"
#include "game/shared/vscript_shared.h"
#include "game/shared/vscript_debug_overlay_shared.h"

#include "vscript_server.h"
#include "vscript_server_natives.h"
#include "vscript_server_placement.h"
#include "player.h"
#include "util_server.h"
#include "entitylist.h"
#include "detour_impl.h"
#include "game/shared/weapon_script_vars.h"
#include "game/server/chatbuilder.h"
#include "game/server/jetdrive.h"
#include "game/server/trigger_updraft.h"
#include "game/server/skydive.h"
#include "game/server/cmd_recorder.h"
#include "game/server/mapedit_paks.h"
#include "game/server/bot_cmd.h"
#include "game/server/player_overheat.h"
#include "game/server/context_action.h"
#include "game/server/translocation.h"
#include "game/shared/status_effects_sdk.h"
#include "game/shared/util_shared.h"
#include "game/client/vscript_player.h"
#include "game/shared/vscript_remotefunctions_sdk.h"
#include "engine/enginetrace.h"
#include "engine/modelloader.h"
#include "engine/server/precache_natives.h"
#include "public/bspflags.h"
#include "tier1/keyvalues.h"
#include "tier1/convar.h"
#include "tier1/cvar.h"
#include "tier2/curlutils.h"
#include "ebisusdk/EbisuSDK.h"
#include "game/server/sound.h"
#include "vscript/languages/squirrel_re/include/sqarray.h"

#include <atomic>
#include <cfloat>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

/*
=====================
SQVM_ServerScript_f

 Executes input on the
 VM in SERVER context.
=====================
*/
static void SQVM_ServerScript_f(const CCommand& args)
{
    if (args.ArgC() < 2)
    {
        Warning(eDLL_T::SERVER, "script: missing code (example: script SpawnBots(60))\n");
        return;
    }
    Script_Execute(args.ArgS(), SQCONTEXT::SERVER);
    ScriptRemoteC2S_DropFnCache();
}
static ConCommand script("script", SQVM_ServerScript_f, "Run input code as SERVER script on the VM", FCVAR_GAMEDLL | FCVAR_CHEAT | FCVAR_DEVELOPMENTONLY | FCVAR_SERVER_FRAME_THREAD);

// Runs every 'script ...' line of cfg/<name>.cfg straight on the server VM.
// The console command buffer drops most of a long cfg, so map builds
// exported as script lines go through here instead of 'exec'.
static void SQVM_RemapBuild_f(const CCommand& args)
{
    if (args.ArgC() < 2)
    {
        Warning(eDLL_T::SERVER, "remap_build: missing name (cfg/<name>.cfg)\n");
        return;
    }
    const char* const pszName = args.Arg(1);
    for (const char* c = pszName; *c; ++c)
    {
        if (!V_isalnum(*c) && *c != '_' && *c != '-')
        {
            Warning(eDLL_T::SERVER, "remap_build: refused name '%s'\n", pszName);
            return;
        }
    }
    char szPath[MAX_OSPATH];
    V_snprintf(szPath, sizeof(szPath), "cfg/%s.cfg", pszName);
    FileHandle_t hFile = FileSystem()->Open(szPath, "rb", "PLATFORM");
    if (hFile == FILESYSTEM_INVALID_HANDLE)
    {
        Warning(eDLL_T::SERVER, "remap_build: cannot open %s\n", szPath);
        return;
    }
    const ssize_t nLen = FileSystem()->Size(hFile);
    if (nLen <= 0 || nLen > (4 << 20))
    {
        Warning(eDLL_T::SERVER, "remap_build: %s has bad size %zd\n", szPath, nLen);
        FileSystem()->Close(hFile);
        return;
    }
    std::unique_ptr<char[]> pBuf(new char[nLen + 1]);
    FileSystem()->Read(pBuf.get(), nLen, hFile);
    FileSystem()->Close(hFile);
    pBuf[nLen] = '\0';

    int nRun = 0, nSkipped = 0;
    char* pLine = pBuf.get();
    while (pLine && *pLine)
    {
        char* pNext = strchr(pLine, '\n');
        if (pNext)
            *pNext++ = '\0';
        size_t nLine = strlen(pLine);
        while (nLine && (pLine[nLine - 1] == '\r' || pLine[nLine - 1] == ' '))
            pLine[--nLine] = '\0';
        if (strncmp(pLine, "script ", 7) == 0 && pLine[7])
        {
            Script_Execute(pLine + 7, SQCONTEXT::SERVER);
            nRun++;
        }
        else if (nLine)
            nSkipped++;
        pLine = pNext;
    }
    ScriptRemoteC2S_DropFnCache();
    Msg(eDLL_T::SERVER, "remap_build: %s ran %d script line(s), skipped %d\n", szPath, nRun, nSkipped);
}
static ConCommand remap_build("remap_build", SQVM_RemapBuild_f, "Run every 'script' line of cfg/<name>.cfg on the SERVER VM, bypassing the command buffer", FCVAR_GAMEDLL | FCVAR_CHEAT | FCVAR_DEVELOPMENTONLY | FCVAR_SERVER_FRAME_THREAD);

static void SQVM_SpawnBots_f(const CCommand& args)
{
    int n = 1;
    if (args.ArgC() >= 2)
        n = atoi(args.Arg(1));
    if (n < 1)
        n = 1;
    char code[64];
    V_snprintf(code, sizeof(code), "SpawnBots(%d)", n);
    Msg(eDLL_T::SERVER, "[SpawnBots] console %s\n", code);
    Script_Execute(code, SQCONTEXT::SERVER);
}
static ConCommand spawnbots("spawnbots", SQVM_SpawnBots_f, "Spawn fake players. Usage: spawnbots <count>", FCVAR_GAMEDLL | FCVAR_CHEAT | FCVAR_SERVER_FRAME_THREAD);

//-----------------------------------------------------------------------------
// Purpose: server NDebugOverlay proxies
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_DebugDrawSolidBox(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawSolidBox(v);
}
static SQRESULT ServerScript_DebugDrawSweptBox(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawSweptBox(v);
}
static SQRESULT ServerScript_DebugDrawTriangle(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawTriangle(v);
}
static SQRESULT ServerScript_DebugDrawSolidSphere(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawSolidSphere(v);
}
static SQRESULT ServerScript_DebugDrawCapsule(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawCapsule(v);
}
static SQRESULT ServerScript_CreateBox(HSQUIRRELVM v)
{
    return SharedScript_CreateBox(v);
}
static SQRESULT ServerScript_ClearBoxes(HSQUIRRELVM v)
{
    return SharedScript_ClearBoxes(v);
}

//-----------------------------------------------------------------------------
// Purpose: calculates the duration for the debug text overlay
//-----------------------------------------------------------------------------
static float ServerScript_DebugScreenText_DetermineDuration(HSQUIRRELVM v)
{
    const float serverFPS = script_server_fps->GetFloat();
    // Make sure the overlay exists as long as the entire server
    // script frame, as it must last until the next call from the
    // server is initiated. Otherwise the following happens
    //
    // - 1 / script_server_fps < NDEBUG_PERSIST_TILL_NEXT_SERVER =
    // text will flicker as they decay
    // before the next frame is fired.
    // - 1 / script_server_fps > NDEBUG_PERSIST_TILL_NEXT_SERVER =
    // text will overlap with previous
    // as the prev hasn't decayed yet.
    return 1.0f / serverFPS;
}

//-----------------------------------------------------------------------------
// Purpose: internal handler for adding debug texts on screen through scripts
//-----------------------------------------------------------------------------
static void ServerScript_Internal_DebugScreenTextWithColor(HSQUIRRELVM v, const float posX, const float posY, const Color color, const char* const text)
{
    const float duration = ServerScript_DebugScreenText_DetermineDuration(v);
    g_pDebugOverlay->AddScreenTextOverlay(posX, posY, duration, color.r(), color.g(), color.b(), color.a(), text);
}

//-----------------------------------------------------------------------------
// Purpose: adds a debug text on the screen at given position
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_DebugScreenText(HSQUIRRELVM v)
{
    if (g_pDebugOverlay)
    {
        SQFloat posX;
        SQFloat posY;
        const SQChar* text = nullptr;

        sq_getfloat(v, 2, &posX);
        sq_getfloat(v, 3, &posY);
        if (SQ_FAILED(sq_getstring(v, 4, &text)) || !text)
        {
            v_SQVM_ScriptError("DebugScreenText: argument 'text' must be a string");
            SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
        }

        const Color color(255, 255, 255, 255);
        ServerScript_Internal_DebugScreenTextWithColor(v, posX, posY, color, text);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: adds a debug text on the screen at given position with color
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_DebugScreenTextWithColor(HSQUIRRELVM v)
{
    if (g_pDebugOverlay)
    {
        SQFloat posX;
        SQFloat posY;
        const SQChar* text = nullptr;
        const SQVector3D* colorVec = nullptr;

        sq_getfloat(v, 2, &posX);
        sq_getfloat(v, 3, &posY);
        if (SQ_FAILED(sq_getstring(v, 4, &text)) || !text)
        {
            v_SQVM_ScriptError("DebugScreenTextWithColor: argument 'text' must be a string");
            SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
        }
        if (SQ_FAILED(sq_getvector(v, 5, &colorVec)) || !colorVec)
        {
            v_SQVM_ScriptError("DebugScreenTextWithColor: argument 'color' must be a vector");
            SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
        }

        const Color color = Script_VectorToColor(colorVec, 1.0f);
        ServerScript_Internal_DebugScreenTextWithColor(v, posX, posY, color, text);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: sets whether the server could auto reload at this time (e.g. if
// server admin has host_autoReloadRate AND host_autoReloadRespectGameState
// set, and its time to auto reload, but the match hasn't finished yet, wait
// until this is set to proceed the reload of the server
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_SetAutoReloadState(HSQUIRRELVM v)
{
    SQBool state = false;
    sq_getbool(v, 2, &state);

    g_hostReloadState = state;
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given name
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_KickPlayerByName(HSQUIRRELVM v)
{
    const SQChar* playerName = nullptr;
    const SQChar* reason = nullptr;

    sq_getstring(v, 2, &playerName);
    sq_getstring(v, 3, &reason);

    if (!VALID_CHARSTAR(playerName))
    {
        v_SQVM_ScriptError("Empty or null player name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    // Discard empty strings, this will use the default message instead.
    if (!VALID_CHARSTAR(reason))
        reason = nullptr;

    g_BanSystem.KickPlayerByName(playerName, reason);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given handle or id
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_KickPlayerById(HSQUIRRELVM v)
{
    const SQChar* playerHandle = nullptr;
    const SQChar* reason = nullptr;

    sq_getstring(v, 2, &playerHandle);
    sq_getstring(v, 3, &reason);

    if (!VALID_CHARSTAR(playerHandle))
    {
        v_SQVM_ScriptError("Empty or null player handle");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    // Discard empty strings, this will use the default message instead.
    if (!VALID_CHARSTAR(reason))
        reason = nullptr;

    g_BanSystem.KickPlayerById(playerHandle, reason);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given name
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_BanPlayerByName(HSQUIRRELVM v)
{
    const SQChar* playerName = nullptr;
    const SQChar* reason = nullptr;

    sq_getstring(v, 2, &playerName);
    sq_getstring(v, 3, &reason);

    if (!VALID_CHARSTAR(playerName))
    {
        v_SQVM_ScriptError("Empty or null player name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    // Discard empty strings, this will use the default message instead.
    if (!VALID_CHARSTAR(reason))
        reason = nullptr;

    g_BanSystem.BanPlayerByName(playerName, reason);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given handle or id
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_BanPlayerById(HSQUIRRELVM v)
{
    const SQChar* playerHandle = nullptr;
    const SQChar* reason = nullptr;

    sq_getstring(v, 2, &playerHandle);
    sq_getstring(v, 3, &reason);

    if (!VALID_CHARSTAR(playerHandle))
    {
        v_SQVM_ScriptError("Empty or null player handle");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    // Discard empty strings, this will use the default message instead.
    if (!VALID_CHARSTAR(reason))
        reason = nullptr;

    g_BanSystem.BanPlayerById(playerHandle, reason);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: unbans a player by given user id or ip address
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_UnbanPlayer(HSQUIRRELVM v)
{
    const SQChar* szCriteria = nullptr;
    sq_getstring(v, 2, &szCriteria);

    if (!VALID_CHARSTAR(szCriteria))
    {
        v_SQVM_ScriptError("Empty or null player criteria");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    g_BanSystem.UnbanPlayer(szCriteria);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_BroadcastServerTextMessage(HSQUIRRELVM v)
{
    const SQChar* pszPrefix = nullptr;
    const SQChar* pszMessage = nullptr;
    SQBool bAdminMsg = false;

    sq_getstring(v, 2, &pszPrefix);
    sq_getstring(v, 3, &pszMessage);
    sq_getbool(v, 4, &bAdminMsg);

    if (!VALID_CHARSTAR(pszPrefix))
    {
        v_SQVM_ScriptError("Null prefix string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!VALID_CHARSTAR(pszMessage))
    {
        v_SQVM_ScriptError("Null message string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    SVC_SystemSayText message(pszPrefix, pszMessage, bAdminMsg);

    g_pServer->BroadcastMessage(&message, true, false);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static CClient* ServerScript_ClientForPlayer(CPlayer* pPlayer)
{
    if (!pPlayer || !g_pServer)
        return nullptr;

    const int nSlot = pPlayer->GetEdict() - 1;
    if (nSlot < 0 || nSlot >= MAX_PLAYERS)
        return nullptr;

    return g_pServer->GetClient(nSlot);
}

static SQRESULT ServerScript_SendServerTextMessage(HSQUIRRELVM v)
{
    CPlayer* pPlayer = nullptr;
    const SQChar* pszPrefix = nullptr;
    const SQChar* pszMessage = nullptr;
    SQBool bAdminMsg = false;

    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
        return SQ_ERROR;

    sq_getstring(v, 2, &pszPrefix);
    sq_getstring(v, 3, &pszMessage);
    sq_getbool(v, 4, &bAdminMsg);

    if (!VALID_CHARSTAR(pszPrefix))
    {
        v_SQVM_ScriptError("Null prefix string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!VALID_CHARSTAR(pszMessage))
    {
        v_SQVM_ScriptError("Null message string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    CClient* const pClient = ServerScript_ClientForPlayer(pPlayer);

    if (!pClient)
        return SQ_ERROR;

    SVC_SystemSayText message(pszPrefix, pszMessage, bAdminMsg);

    sq_pushbool(v, pClient->SendNetMsgEx(&message, false, false, false));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: chat builder - renders an array of styled segments in the recipient's
//          chat box. Each segment is a table:
//            { text = "hi", r = 255, g = 0, b = 0, rainbow = false,
//              newline = true, sustain = 10.0, fade = 1.0 }
//          Only "text" is required. sustain/fade are seconds; 0 means the
//          recipient's own chat defaults.
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_ChatBuilder(HSQUIRRELVM v)
{
    CPlayer* pPlayer = nullptr;

    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
        return SQ_ERROR;

    ChatBuilderSeg_t segs[kChatBuilderMaxSegments] = {};
    const int nCount = ChatBuilder_ParseSegArray(v, 2, segs, kChatBuilderMaxSegments);

    if (nCount < 0)
    {
        v_SQVM_ScriptError("Expected an array of segment tables");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!g_pServer)
        return SQ_ERROR;

    const int nEdict = pPlayer->GetEdict();

    if (nEdict < 1 || nEdict > MAX_PLAYERS)
        return SQ_ERROR;

    CClient* const pClient = g_pServer->GetClient(nEdict - 1);

    if (!pClient)
        return SQ_ERROR;

    const bool bAdminMsg = ChatBuilder_ReadOptionalBool(v, 3, false);
    sq_pushbool(v, ChatBuilder_SendToClient(pClient, segs, nCount, bAdminMsg));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: chat builder - broadcasts the same segment array to every client
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_BroadcastChatBuilder(HSQUIRRELVM v)
{
    ChatBuilderSeg_t segs[kChatBuilderMaxSegments] = {};
    const int nCount = ChatBuilder_ParseSegArray(v, 2, segs, kChatBuilderMaxSegments);

    if (nCount < 0)
    {
        v_SQVM_ScriptError("Expected an array of segment tables");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const bool bAdminMsg = ChatBuilder_ReadOptionalBool(v, 3, false);
    ChatBuilder_Broadcast(segs, nCount, bAdminMsg);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: chat builder - one rainbow-coloured line
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_ChatBuilderRainbow(HSQUIRRELVM v)
{
    CPlayer* pPlayer = nullptr;
    const SQChar* pszText = nullptr;
    SQFloat flSustain = 0.0f;
    SQFloat flFade = 0.0f;

    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
        return SQ_ERROR;

    sq_getstring(v, 2, &pszText);
    sq_getfloat(v, 3, &flSustain);
    sq_getfloat(v, 4, &flFade);

    ChatBuilderSeg_t seg = {};
    seg.op = CHATBUILDER_OP_RAINBOW;
    seg.flags = CHATBUILDER_F_NEWLINE;
    seg.r = seg.g = seg.b = 255;
    seg.sustainMs = ChatBuilder_ClampMs(flSustain, kChatBuilderMaxSustainMs);
    seg.fadeMs = ChatBuilder_ClampMs(flFade, kChatBuilderMaxFadeMs);
    seg.textLen = static_cast<uint8_t>(
        ChatBuilder_SanitizeText(pszText, seg.text, kChatBuilderMaxSegText));

    if (seg.textLen == 0)
    {
        sq_pushbool(v, false);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    if (!g_pServer)
        return SQ_ERROR;

    const int nEdict = pPlayer->GetEdict();

    if (nEdict < 1 || nEdict > MAX_PLAYERS)
        return SQ_ERROR;

    CClient* const pClient = g_pServer->GetClient(nEdict - 1);

    if (!pClient)
        return SQ_ERROR;

    const bool bAdminMsg = ChatBuilder_ReadOptionalBool(v, 5, false);
    sq_pushbool(v, ChatBuilder_SendToClient(pClient, &seg, 1, bAdminMsg));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}
//-----------------------------------------------------------------------------
// Purpose: gets the number of real players on this server
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_GetNumHumanPlayers(HSQUIRRELVM v)
{
    sq_pushinteger(v, g_pServer->GetNumHumanPlayers());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: gets the number of fake players on this server
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_GetNumFakeClients(HSQUIRRELVM v)
{
    sq_pushinteger(v, g_pServer->GetNumFakeClients());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: gets our current session id
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_GetSessionID(HSQUIRRELVM v)
{
    sq_pushstring(v, g_LogSessionUUID.c_str(), (SQInteger)g_LogSessionUUID.length());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: creates a fake player and returns the edict index
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_CreateFakePlayer(HSQUIRRELVM v)
{
    if (!g_pServer->IsActive())
    {
        sq_pushinteger(v, -1);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    const int numPlayers = g_pServer->GetNumClients();

    // Already at max, don't create
    if (numPlayers >= g_ServerGlobalVariables->maxClients)
    {
        sq_pushinteger(v, -1);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    const SQChar* playerName = nullptr;
    SQInteger teamNum;

    // Get parameters
    if (SQ_FAILED(sq_getstring(v, 2, &playerName)) || !playerName)
    {
        v_SQVM_ScriptError("CreateFakePlayer: argument 'playerName' must be a string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    sq_getinteger(v, 3, &teamNum);

    if (!VALID_CHARSTAR(playerName))
    {
        v_SQVM_ScriptError("Empty or null player name");
        sq_pushinteger(v, -1);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    // Thread synchronization required
    ThreadJoinServerJob();

    // Create the fake client
    const edict_t nHandle = g_pEngineServer->CreateFakeClient(playerName, static_cast<int>(teamNum));

    if (nHandle < 0)
    {
        sq_pushinteger(v, -1);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    // Fully connect the client
    g_pServerGameClients->ClientFullyConnect(nHandle, false);

    // Return the edict index - scripts can use GetPlayerArray or GetPlayerByIndex to get the entity
    sq_pushinteger(v, static_cast<SQInteger>(nHandle));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: sets a class var on the server and each client
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_ScriptSetClassVar(HSQUIRRELVM v)
{
    CPlayer* player = nullptr;

    if (!v_sq_getentity(v, (SQEntity*)&player))
        return SQ_ERROR;

    const SQChar* key = nullptr;
    sq_getstring(v, 2, &key);

    if (!VALID_CHARSTAR(key))
    {
        v_SQVM_ScriptError("Empty or null class key");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const SQChar* val = nullptr;
    sq_getstring(v, 3, &val);

    if (!VALID_CHARSTAR(val))
    {
        v_SQVM_ScriptError("Empty or null class value");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    CClient* const client = ServerScript_ClientForPlayer(player);
    if (!client)
        return SQ_ERROR;

    SVC_SetClassVar msg(key, val);

    const bool success = client->SendNetMsgEx(&msg, false, true, false);

    if (success)
    {
        const char* pArgs[3] = {
            "_setClassVarServer",
            key,
            val
        };

        const CCommand cmd((int)V_ARRAYSIZE(pArgs), pArgs, cmd_source_t::kCommandSrcCode);
        if (cmd.ArgC() >= 3)
        {
            const int oldIdx = *g_nCommandClientIndex;

            *g_nCommandClientIndex = client->GetUserID();
            v__setClassVarServer_f(cmd);

            *g_nCommandClientIndex = oldIdx;
        }
    }

    sq_pushbool(v, success);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: checks if the provided hull type is valid
//-----------------------------------------------------------------------------
static bool Internal_ServerScript_ValidateHull(const SQInteger hull)
{
    if (hull < 0 || hull >= Hull_e::NUM_HULLS)
    {
        v_SQVM_ScriptError("Hull type with value %d does not index the maximum number of hulls(%d)", hull, Hull_e::NUM_HULLS);
        return false;
    }

    return true;
}

// Engine map-coord ceiling used by stock NavMesh natives before querying.
static constexpr float s_navMeshMapCoordLimit = 131072.0f;

//-----------------------------------------------------------------------------
// Purpose: reject non-finite coords and values outside engine map bounds
//-----------------------------------------------------------------------------
static bool Internal_ServerScript_ValidateMapPoint(HSQUIRRELVM v, const SQVector3D* point)
{
    if (!point)
    {
        v_SQVM_ScriptError("Point is null");
        return false;
    }

    if (!isfinite(point->x) || !isfinite(point->y) || !isfinite(point->z))
    {
        v_SQVM_ScriptError("Point (%f, %f, %f) has non-finite components",
            point->x, point->y, point->z);
        return false;
    }

    if (fabsf(point->x) > s_navMeshMapCoordLimit
        || fabsf(point->y) > s_navMeshMapCoordLimit
        || fabsf(point->z) > s_navMeshMapCoordLimit)
    {
        v_SQVM_ScriptError("Point (%f, %f, %f) is outside map bounds (+/-%f)",
            point->x, point->y, point->z, s_navMeshMapCoordLimit);
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: resolve loaded navmesh for hull; raises the standard script error if absent
//-----------------------------------------------------------------------------
static const dtNavMesh* Internal_ServerScript_GetNavMeshForHull(HSQUIRRELVM v, const Hull_e hullType)
{
    const NavMeshType_e navType = NAI_Hull::NavMeshType(hullType);
    const dtNavMesh* const nav = Detour_GetNavMeshByType(navType);

    if (!nav)
    {
        v_SQVM_ScriptError("NavMesh \"%s\" for hull \"%s\" hasn't been loaded!",
            NavMesh_GetNameForType(navType), g_aiHullNames[hullType]);
        return nullptr;
    }

    return nav;
}

//-----------------------------------------------------------------------------
// Purpose: checks if the provided half-extents is valid
//-----------------------------------------------------------------------------
static bool Internal_ServerScript_NavMesh_GetExtents(HSQUIRRELVM v, const SQInteger stackIdx, rdVec3D* const out)
{
    const SQVector3D* extents = nullptr;
    if (SQ_FAILED(sq_getvector(v, stackIdx, &extents)) || !extents)
    {
        v_SQVM_ScriptError("Argument 'halfExtents' must be a vector");
        return false;
    }

    const SQFloat maxMagnitudeSqr = 9000000.0f;
    const SQFloat magnitudeSqr = extents->Dot();

    if (magnitudeSqr > maxMagnitudeSqr)
    {
        v_SQVM_ScriptError("Extents magnitude (%f) is too big. Max magnitude is %f", sqrtf(magnitudeSqr), sqrtf(maxMagnitudeSqr));
        return false;
    }

    if (extents->x <= 0.f || extents->y <= 0.f || extents->z <= 0.f)
    {
        v_SQVM_ScriptError("Extents elements (%f, %f, %f) must all be greater than zero", extents->x, extents->y, extents->z);
        return false;
    }

    out->init(extents->x, extents->y, extents->z);
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: findNearestPoly query only; no stack reads, no errors.
// Returns false if nav is null or no poly is found.
//-----------------------------------------------------------------------------
static bool Internal_ServerScript_NavMesh_QueryNearestPos(
    const dtNavMesh* const nav,
    const rdVec3D& searchPoint,
    const rdVec3D& halfExtents,
    rdVec3D* const outResult)
{
    if (!nav || !outResult)
        return false;

    dtNavMeshQuery query;
    query.attachNavMeshUnsafe(nav);

    dtQueryFilter filter;
    filter.setIncludeFlags(DT_POLYFLAGS_ALL);
    filter.setExcludeFlags(DT_POLYFLAGS_DISABLED);

    dtPolyRef nearestRef = 0;
    rdVec3D nearestPt;

    const dtStatus status = query.findNearestPoly(&searchPoint, &halfExtents, &filter, &nearestRef, &nearestPt);

    if (dtStatusFailed(status) || !nearestRef)
        return false;

    outResult->init(nearestPt.x, nearestPt.y, nearestPt.z);
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: finds the nearest poly to given point, with an optional bounds filter.
// On miss pushes null (GetNearestPos contract). Raises on bad args / missing mesh.
//-----------------------------------------------------------------------------
static bool Internal_ServerScript_NavMesh_FindNearestPos(HSQUIRRELVM v, const bool useBounds)
{
    SQInteger hullIdx = 0;
    if (SQ_FAILED(sq_getinteger(v, useBounds ? 4 : 3, &hullIdx)))
    {
        v_SQVM_ScriptError("Argument 'hullType' must be an integer");
        return false;
    }

    if (!Internal_ServerScript_ValidateHull(hullIdx))
        return false;

    const Hull_e hullType = Hull_e(hullIdx);

    const dtNavMesh* const nav = Internal_ServerScript_GetNavMeshForHull(v, hullType);
    if (!nav)
        return false;

    rdVec3D halfExtents;

    if (useBounds)
    {
        if (!Internal_ServerScript_NavMesh_GetExtents(v, 3, &halfExtents))
            return false;
    }
    else
    {
        const Vector3D& maxs = NAI_Hull::Maxs(hullType);
        halfExtents.init(maxs.x, maxs.y, maxs.z);
    }

    const SQVector3D* point = nullptr;
    if (SQ_FAILED(sq_getvector(v, 2, &point)) || !point)
    {
        v_SQVM_ScriptError("Argument 'searchPoint' must be a vector");
        return false;
    }

    const rdVec3D searchPoint(point->x, point->y, point->z);

    rdVec3D nearestPt;
    if (!Internal_ServerScript_NavMesh_QueryNearestPos(nav, searchPoint, halfExtents, &nearestPt))
    {
        v->PushNull();
        return true;
    }

    const SQVector3D result(nearestPt.x, nearestPt.y, nearestPt.z);
    sq_pushvector(v, &result);

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: finds the nearest polygon to provided point
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_GetNearestPos(HSQUIRRELVM v)
{
    const bool ret = Internal_ServerScript_NavMesh_FindNearestPos(v, false);

    if (!ret)
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: finds the nearest polygon to provided point within extents
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_GetNearestPosInBounds(HSQUIRRELVM v)
{
    const bool ret = Internal_ServerScript_NavMesh_FindNearestPos(v, true);

    if (!ret)
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: nearest navmesh position to point (HULL_HUMAN); echoes input on miss
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_GetClosestPoint(HSQUIRRELVM v)
{
    const SQVector3D* point = nullptr;
    if (SQ_FAILED(sq_getvector(v, 2, &point)) || !point)
    {
        v_SQVM_ScriptError("Argument 'point' must be a vector");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!Internal_ServerScript_ValidateMapPoint(v, point))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQVector3D pointCopy(point->x, point->y, point->z);

    // Absent mesh / no poly: return caller input (never world origin).
    const dtNavMesh* const nav = Detour_GetNavMeshByType(NAI_Hull::NavMeshType(HULL_HUMAN));
    if (!nav)
    {
        sq_pushvector(v, &pointCopy);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    const Vector3D& maxs = NAI_Hull::Maxs(HULL_HUMAN);
    const rdVec3D halfExtents(maxs.x, maxs.y, maxs.z);
    const rdVec3D searchPoint(point->x, point->y, point->z);

    rdVec3D nearestPt;
    if (!Internal_ServerScript_NavMesh_QueryNearestPos(nav, searchPoint, halfExtents, &nearestPt))
    {
        sq_pushvector(v, &pointCopy);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    const SQVector3D result(nearestPt.x, nearestPt.y, nearestPt.z);
    sq_pushvector(v, &result);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: up to N nearest navmesh positions around origin (HULL_HUMAN)
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_GetClosestPoints(HSQUIRRELVM v)
{
    const SQVector3D* origin = nullptr;
    if (SQ_FAILED(sq_getvector(v, 2, &origin)) || !origin)
    {
        v_SQVM_ScriptError("Argument 'origin' must be a vector");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!Internal_ServerScript_ValidateMapPoint(v, origin))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    SQInteger numPointsArg = 0;
    if (SQ_FAILED(sq_getinteger(v, 3, &numPointsArg)))
    {
        v_SQVM_ScriptError("Argument 'numPoints' must be an integer");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    int numPoints = static_cast<int>(numPointsArg);
    if (numPoints < 1)
        numPoints = 1;
    if (numPoints > 64)
        numPoints = 64;

    const SQVector3D originCopy(origin->x, origin->y, origin->z);

    // Mesh absent or query failure: echo caller's origin (never world 0,0,0).
    auto pushOriginFill = [&]() -> SQRESULT
    {
        sq_newarray(v, 0);
        for (int i = 0; i < numPoints; ++i)
        {
            sq_pushvector(v, &originCopy);
            sq_arrayappend(v, -2);
        }
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    };

    const dtNavMesh* const nav = Detour_GetNavMeshByType(NAI_Hull::NavMeshType(HULL_HUMAN));
    if (!nav)
        return pushOriginFill();

    const Vector3D& maxs = NAI_Hull::Maxs(HULL_HUMAN);
    const rdVec3D halfExtents(maxs.x * 4.0f, maxs.y * 4.0f, maxs.z * 4.0f);
    const rdVec3D searchPoint(origin->x, origin->y, origin->z);

    dtNavMeshQuery query;
    query.attachNavMeshUnsafe(nav);

    dtQueryFilter filter;
    filter.setIncludeFlags(DT_POLYFLAGS_ALL);
    filter.setExcludeFlags(DT_POLYFLAGS_DISABLED);

    constexpr int kMaxPolys = 256;
    dtPolyRef polys[kMaxPolys];
    int polyCount = 0;

    const dtStatus status = query.queryPolygons(&searchPoint, &halfExtents, &filter, polys, &polyCount, kMaxPolys);
    if (dtStatusFailed(status) || polyCount <= 0)
        return pushOriginFill();

    if (polyCount > kMaxPolys)
        polyCount = kMaxPolys;

    struct ClosestEntry_t
    {
        float distSqr;
        rdVec3D pt;
    };

    ClosestEntry_t entries[kMaxPolys];
    int entryCount = 0;

    for (int i = 0; i < polyCount; ++i)
    {
        bool posOverPoly = false;
        rdVec3D closest;
        const dtStatus cstatus = query.closestPointOnPoly(polys[i], &searchPoint, &closest, &posOverPoly);
        if (dtStatusFailed(cstatus))
            continue;

        if (entryCount >= kMaxPolys)
            break;

        entries[entryCount].distSqr = rdVdistSqr(&searchPoint, &closest);
        entries[entryCount].pt = closest;
        ++entryCount;
    }

    if (entryCount <= 0)
        return pushOriginFill();

    for (int i = 1; i < entryCount; ++i)
    {
        ClosestEntry_t key = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].distSqr > key.distSqr)
        {
            entries[j + 1] = entries[j];
            --j;
        }
        entries[j + 1] = key;
    }

    const int emitCount = (entryCount < numPoints) ? entryCount : numPoints;

    sq_newarray(v, 0);
    for (int i = 0; i < emitCount; ++i)
    {
        const SQVector3D result(entries[i].pt.x, entries[i].pt.y, entries[i].pt.z);
        sq_pushvector(v, &result);
        sq_arrayappend(v, -2);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: poly count in a tall vertical column through point (HULL_HUMAN)
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_TraceVerticalLine_PolyCount(HSQUIRRELVM v)
{
    const SQVector3D* point = nullptr;
    if (SQ_FAILED(sq_getvector(v, 2, &point)) || !point)
    {
        v_SQVM_ScriptError("Argument 'point' must be a vector");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!Internal_ServerScript_ValidateMapPoint(v, point))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const dtNavMesh* const nav = Detour_GetNavMeshByType(NAI_Hull::NavMeshType(HULL_HUMAN));
    if (!nav)
    {
        sq_pushinteger(v, 0);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    const rdVec3D searchPoint(point->x, point->y, point->z);
    const rdVec3D halfExtents(8.0f, 8.0f, 32768.0f);

    dtNavMeshQuery query;
    query.attachNavMeshUnsafe(nav);

    dtQueryFilter filter;
    filter.setIncludeFlags(DT_POLYFLAGS_ALL);
    filter.setExcludeFlags(DT_POLYFLAGS_DISABLED);

    constexpr int kMaxPolys = 256;
    dtPolyRef polys[kMaxPolys];
    int polyCount = 0;

    const dtStatus status = query.queryPolygons(&searchPoint, &halfExtents, &filter, polys, &polyCount, kMaxPolys);
    if (dtStatusFailed(status) || polyCount < 0)
        polyCount = 0;
    if (polyCount > kMaxPolys)
        polyCount = kMaxPolys;

    sq_pushinteger(v, polyCount);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: one representative point per distinct elevation in a vertical column
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_GetPointsInDifferentElevations(HSQUIRRELVM v)
{
    const SQVector3D* point = nullptr;
    if (SQ_FAILED(sq_getvector(v, 2, &point)) || !point)
    {
        v_SQVM_ScriptError("Argument 'point' must be a vector");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!Internal_ServerScript_ValidateMapPoint(v, point))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    sq_newarray(v, 0);

    const dtNavMesh* const nav = Detour_GetNavMeshByType(NAI_Hull::NavMeshType(HULL_HUMAN));
    if (!nav)
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    const rdVec3D searchPoint(point->x, point->y, point->z);
    const rdVec3D halfExtents(8.0f, 8.0f, 32768.0f);

    dtNavMeshQuery query;
    query.attachNavMeshUnsafe(nav);

    dtQueryFilter filter;
    filter.setIncludeFlags(DT_POLYFLAGS_ALL);
    filter.setExcludeFlags(DT_POLYFLAGS_DISABLED);

    constexpr int kMaxPolys = 256;
    dtPolyRef polys[kMaxPolys];
    int polyCount = 0;

    const dtStatus status = query.queryPolygons(&searchPoint, &halfExtents, &filter, polys, &polyCount, kMaxPolys);
    if (dtStatusFailed(status) || polyCount <= 0)
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    if (polyCount > kMaxPolys)
        polyCount = kMaxPolys;

    constexpr int kMaxResults = 32;
    constexpr float kElevEps = 32.0f;
    rdVec3D kept[kMaxResults];
    int keptCount = 0;

    for (int i = 0; i < polyCount; ++i)
    {
        bool posOverPoly = false;
        rdVec3D closest;
        const dtStatus cstatus = query.closestPointOnPoly(polys[i], &searchPoint, &closest, &posOverPoly);
        if (dtStatusFailed(cstatus))
            continue;

        bool tooClose = false;
        for (int k = 0; k < keptCount; ++k)
        {
            if (fabsf(closest.z - kept[k].z) < kElevEps)
            {
                tooClose = true;
                break;
            }
        }

        if (tooClose)
            continue;

        if (keptCount >= kMaxResults)
            break;

        kept[keptCount++] = closest;
    }

    for (int i = 1; i < keptCount; ++i)
    {
        rdVec3D key = kept[i];
        int j = i - 1;
        while (j >= 0 && kept[j].z > key.z)
        {
            kept[j + 1] = kept[j];
            --j;
        }
        kept[j + 1] = key;
    }

    for (int i = 0; i < keptCount; ++i)
    {
        const SQVector3D result(kept[i].x, kept[i].y, kept[i].z);
        sq_pushvector(v, &result);
        sq_arrayappend(v, -2);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: true if any hazard-flagged poly lies within distance of point
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_HasHarmfulAreaWithinDistance(HSQUIRRELVM v)
{
    const SQVector3D* point = nullptr;
    if (SQ_FAILED(sq_getvector(v, 2, &point)) || !point)
    {
        v_SQVM_ScriptError("Argument 'point' must be a vector");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!Internal_ServerScript_ValidateMapPoint(v, point))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    SQFloat distance = 0.0f;
    if (SQ_FAILED(sq_getfloat(v, 3, &distance)))
    {
        v_SQVM_ScriptError("Argument 'distance' must be a float");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!isfinite(distance) || distance <= 0.0f)
    {
        v_SQVM_ScriptError("Distance (%f) must be greater than zero and finite", distance);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const dtNavMesh* const nav = Detour_GetNavMeshByType(NAI_Hull::NavMeshType(HULL_HUMAN));
    if (!nav)
    {
        sq_pushbool(v, false);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    const rdVec3D searchPoint(point->x, point->y, point->z);
    const float distF = static_cast<float>(distance);
    const rdVec3D halfExtents(distF, distF, distF);

    dtNavMeshQuery query;
    query.attachNavMeshUnsafe(nav);

    dtQueryFilter filter;
    filter.setIncludeFlags(DT_POLYFLAGS_HAZARD);
    filter.setExcludeFlags(0);

    constexpr int kMaxPolys = 32;
    dtPolyRef polys[kMaxPolys];
    int polyCount = 0;

    const dtStatus status = query.queryPolygons(&searchPoint, &halfExtents, &filter, polys, &polyCount, kMaxPolys);
    const bool found = !dtStatusFailed(status) && polyCount > 0;

    sq_pushbool(v, found);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: positions on open navmesh polys matching area/adjacency criteria
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_SearchForOpenAreas(HSQUIRRELVM v)
{
    SQInteger hullIdx = 0;
    if (SQ_FAILED(sq_getinteger(v, 2, &hullIdx)))
    {
        v_SQVM_ScriptError("Argument 'hullType' must be an integer");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!Internal_ServerScript_ValidateHull(hullIdx))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const Hull_e hullType = Hull_e(hullIdx);

    SQFloat idealTileArea = 0.0f;
    SQFloat minTileSize = 0.0f;
    SQFloat minSurroundingArea = 0.0f;
    SQFloat maxSurroundingArea = 0.0f;
    SQFloat maxPathCost = 0.0f;
    SQInteger minNumAdjacentTilesArg = 0;

    if (SQ_FAILED(sq_getfloat(v, 3, &idealTileArea)))
    {
        v_SQVM_ScriptError("Argument 'idealTileArea' must be a float");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (SQ_FAILED(sq_getfloat(v, 4, &minTileSize)))
    {
        v_SQVM_ScriptError("Argument 'minTileSize' must be a float");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (SQ_FAILED(sq_getfloat(v, 5, &minSurroundingArea)))
    {
        v_SQVM_ScriptError("Argument 'minSurroundingArea' must be a float");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (SQ_FAILED(sq_getfloat(v, 6, &maxSurroundingArea)))
    {
        v_SQVM_ScriptError("Argument 'maxSurroundingArea' must be a float");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (SQ_FAILED(sq_getfloat(v, 7, &maxPathCost)))
    {
        v_SQVM_ScriptError("Argument 'maxPathCost' must be a float");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (SQ_FAILED(sq_getinteger(v, 8, &minNumAdjacentTilesArg)))
    {
        v_SQVM_ScriptError("Argument 'minNumAdjacentTiles' must be an integer");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    static bool s_warnedMaxPathCost = false;
    if (!s_warnedMaxPathCost)
    {
        s_warnedMaxPathCost = true;
        DevWarning(eDLL_T::SERVER,
            "[NAVMESH] NavMesh_SearchForOpenAreas: maxPathCost is not implemented (no path origin in signature)\n");
    }

    (void)maxPathCost;

    sq_newarray(v, 0);

    // Silent degrade: do not raise (callers treat empty as "no spots").
    const dtNavMesh* const nav = Detour_GetNavMeshByType(NAI_Hull::NavMeshType(hullType));
    if (!nav)
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    const int minNumAdjacentTiles = static_cast<int>(minNumAdjacentTilesArg);
    const float minPolyArea = static_cast<float>(minTileSize) * static_cast<float>(minTileSize);
    const float minSurr = static_cast<float>(minSurroundingArea);
    const float maxSurr = static_cast<float>(maxSurroundingArea);
    const float idealArea = static_cast<float>(idealTileArea);

    constexpr int kMaxKeepers = 64;
    constexpr int kMaxPolysExamined = 200000;

    struct OpenAreaEntry_t
    {
        float score;
        rdVec3D center;
    };

    OpenAreaEntry_t keepers[kMaxKeepers];
    int keeperCount = 0;
    int polysExamined = 0;
    bool hitPolyCap = false;
    bool hitKeeperCap = false;

    CFastTimer timer;
    timer.Start();

    const int maxTiles = nav->getMaxTiles();
    for (int ti = 0; ti < maxTiles; ++ti)
    {
        if (hitPolyCap || hitKeeperCap)
            break;

        const dtMeshTile* const tile = nav->getTile(ti);
        if (!tile || !tile->header)
            continue;

        const dtMeshHeader* const header = tile->header;
        const int polyCount = header->polyCount;
        const int maxLinkCount = header->maxLinkCount;

        for (int pi = 0; pi < polyCount; ++pi)
        {
            if (polysExamined >= kMaxPolysExamined)
            {
                hitPolyCap = true;
                break;
            }
            ++polysExamined;

            if (keeperCount >= kMaxKeepers)
            {
                hitKeeperCap = true;
                break;
            }

            const dtPoly* const poly = &tile->polys[pi];
            const float area = dtCalcPolySurfaceArea(poly, tile->verts);
            if (area < minPolyArea)
                continue;

            int neighbourCount = 0;
            float surroundingArea = 0.0f;
            int linkSafety = 0;

            for (unsigned int li = poly->firstLink; li != DT_NULL_LINK; li = tile->links[li].next)
            {
                if (++linkSafety > maxLinkCount)
                    break;

                if (li >= static_cast<unsigned int>(maxLinkCount))
                    break;

                const dtLink& link = tile->links[li];
                if (!link.ref)
                    continue;

                const dtMeshTile* ntile = nullptr;
                const dtPoly* npoly = nullptr;
                if (dtStatusFailed(nav->getTileAndPolyByRef(link.ref, &ntile, &npoly)))
                    continue;

                ++neighbourCount;
                surroundingArea += dtCalcPolySurfaceArea(npoly, ntile->verts);
            }

            if (neighbourCount < minNumAdjacentTiles)
                continue;

            if (surroundingArea < minSurr || surroundingArea > maxSurr)
                continue;

            if (keeperCount >= kMaxKeepers)
            {
                hitKeeperCap = true;
                break;
            }

            keepers[keeperCount].score = fabsf(area - idealArea);
            keepers[keeperCount].center = poly->center;
            ++keeperCount;
        }
    }

    timer.End();

    if (hitPolyCap)
    {
        DevMsg(eDLL_T::SERVER,
            "[NAVMESH] NavMesh_SearchForOpenAreas: stopped after examining %d polys\n",
            kMaxPolysExamined);
    }
    else if (hitKeeperCap)
    {
        DevMsg(eDLL_T::SERVER,
            "[NAVMESH] NavMesh_SearchForOpenAreas: stopped after collecting %d keepers\n",
            kMaxKeepers);
    }

    DevMsg(eDLL_T::SERVER,
        "[NAVMESH] NavMesh_SearchForOpenAreas: examined %d polys, kept %d in %lf seconds\n",
        polysExamined, keeperCount, timer.GetDuration().GetSeconds());

    for (int i = 1; i < keeperCount; ++i)
    {
        OpenAreaEntry_t key = keepers[i];
        int j = i - 1;
        while (j >= 0 && keepers[j].score > key.score)
        {
            keepers[j + 1] = keepers[j];
            --j;
        }
        keepers[j + 1] = key;
    }

    for (int i = 0; i < keeperCount; ++i)
    {
        const SQVector3D result(keepers[i].center.x, keepers[i].center.y, keepers[i].center.z);
        sq_pushvector(v, &result);
        sq_arrayappend(v, -2);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: builds a waypoint list between two world positions over the human
// hull's NavMesh. Returns an empty array when no route exists; callers treat
// that as "fly straight at the target".
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_FindUnboundPath(HSQUIRRELVM v)
{
    const SQVector3D* startArg = nullptr;
    if (SQ_FAILED(sq_getvector(v, 2, &startArg)) || !startArg)
    {
        v_SQVM_ScriptError("Argument 'startPos' must be a vector");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!Internal_ServerScript_ValidateMapPoint(v, startArg))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQVector3D* endArg = nullptr;
    if (SQ_FAILED(sq_getvector(v, 3, &endArg)) || !endArg)
    {
        v_SQVM_ScriptError("Argument 'endPos' must be a vector");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!Internal_ServerScript_ValidateMapPoint(v, endArg))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    // Every failure below degrades to an empty waypoint list, never an error.
    sq_newarray(v, 0);

    const dtNavMesh* const nav = Detour_GetNavMeshByType(NAI_Hull::NavMeshType(HULL_HUMAN));
    if (!nav)
    {
        DevWarning(eDLL_T::SERVER, "[NAVMESH] NavMesh_FindUnboundPath: no mesh loaded for hull \"%s\"\n",
            g_aiHullNames[HULL_HUMAN]);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    dtNavMeshQuery query;
    query.attachNavMeshUnsafe(nav);

    dtQueryFilter filter;
    filter.setIncludeFlags(DT_POLYFLAGS_ALL);
    filter.setExcludeFlags(DT_POLYFLAGS_DISABLED);

    const Vector3D& maxs = NAI_Hull::Maxs(HULL_HUMAN);
    const rdVec3D halfExtents(maxs.x, maxs.y, maxs.z);

    const rdVec3D startPos(startArg->x, startArg->y, startArg->z);
    const rdVec3D endPos(endArg->x, endArg->y, endArg->z);

    dtPolyRef startRef = 0;
    dtPolyRef endRef = 0;
    rdVec3D startPt;
    rdVec3D endPt;

    if (dtStatusFailed(query.findNearestPoly(&startPos, &halfExtents, &filter, &startRef, &startPt)) || !startRef)
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    if (dtStatusFailed(query.findNearestPoly(&endPos, &halfExtents, &filter, &endRef, &endPt)) || !endRef)
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    constexpr int kMaxPolys = 256;
    dtPolyRef polys[kMaxPolys];
    unsigned char jumpTypes[kMaxPolys];
    int polyCount = 0;

    if (dtStatusFailed(query.findPath(startRef, endRef, &startPt, &endPt, &filter,
        polys, jumpTypes, &polyCount, kMaxPolys)) || polyCount <= 0)
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    // findPath returns a partial corridor when the goal is unreachable; clamp
    // the end to the last poly we did reach so the string-pull stays on mesh.
    rdVec3D targetPt = endPt;
    if (polys[polyCount - 1] != endRef)
        query.closestPointOnPoly(polys[polyCount - 1], &endPt, &targetPt, nullptr);

    constexpr int kMaxWaypoints = 128;
    rdVec3D straightPath[kMaxWaypoints];
    unsigned char straightFlags[kMaxWaypoints];
    dtPolyRef straightRefs[kMaxWaypoints];
    unsigned char straightJumps[kMaxWaypoints];
    int waypointCount = 0;

    // 0xffffffff admits every traverse type; a zero filter rejects all portals.
    if (dtStatusFailed(query.findStraightPath(&startPt, &targetPt, polys, jumpTypes, polyCount,
        straightPath, straightFlags, straightRefs, straightJumps, &waypointCount,
        kMaxWaypoints, 0xffffffff)) || waypointCount <= 0)
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    if (waypointCount > kMaxWaypoints)
        waypointCount = kMaxWaypoints;

    // Skip the first vertex: it is the caller's own position.
    for (int i = 1; i < waypointCount; ++i)
    {
        const SQVector3D result(straightPath[i].x, straightPath[i].y, straightPath[i].z);
        sq_pushvector(v, &result);
        sq_arrayappend(v, -2);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: saves a recorded animation on the disk to be used by bakery
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_SaveRecordedAnimation(HSQUIRRELVM v)
{
    if (!developer->GetBool())
    {
        v_SQVM_ScriptError("SaveRecordedAnimation() is dev only!");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    AnimRecordingAssetHeader_s* const animRecording = v_ServerScript_GetRecordedAnimationFromCurrentStack(v);

    if (!animRecording)
    {
        v_SQVM_ScriptError("Parameter must be a recorded animation");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (animRecording->numRecordedFrames == 0)
    {
        v_SQVM_ScriptError("Recorded animation has 0 frames");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const SQChar* fileName = nullptr;
    if (SQ_FAILED(sq_getstring(v, 3, &fileName)) || !fileName)
    {
        v_SQVM_ScriptError("SaveRecordedAnimation: argument 'fileName' must be a string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!V_IsValidPath(fileName) || V_stristr(fileName, ":") || V_stristr(fileName, "\\") || V_stristr(fileName, "/"))
    {
        v_SQVM_ScriptError("SaveRecordedAnimation: argument 'fileName' is not a simple name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    char fileNameBuf[MAX_OSPATH];
    const int fmtResult = snprintf(fileNameBuf, sizeof(fileNameBuf), "anim_recording/%s.anir", fileName);

    if (fmtResult < 0)
    {
        v_SQVM_ScriptError("Failed to format recorded animation file name; provided name \"%s\" is invalid", fileName);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    FileSystem()->CreateDirHierarchy("anim_recording/", "MOD");
    FileHandle_t animRecordingFile = FileSystem()->Open(fileNameBuf, "wb", "MOD");

    if (animRecordingFile == FILESYSTEM_INVALID_HANDLE)
    {
        v_SQVM_ScriptError("Failed to open recorded animation file \"%s\" for write", fileNameBuf);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    AnimRecordingFileHeader_s fileHdr;

    fileHdr.magic = ANIR_FILE_MAGIC;
    fileHdr.fileVersion = ANIR_FILE_VERSION;
    fileHdr.assetVersion = ANIR_ASSET_VERSION;

    fileHdr.startPos = animRecording->startPos;
    fileHdr.startAngles = animRecording->startAngles;

    fileHdr.stringBufSize = 0;

    fileHdr.numElements = 0;
    fileHdr.numSequences = 0;

    fileHdr.numRecordedFrames = animRecording->numRecordedFrames;
    fileHdr.numRecordedOverlays = animRecording->numRecordedOverlays;

    fileHdr.animRecordingId = animRecording->animRecordingId;
    FileSystem()->Write(&fileHdr, sizeof(AnimRecordingFileHeader_s), animRecordingFile);

    // This information can only be retrieved by counting the number
    // of valid pose parameter names.
    int numElems = 0;
    int stringBufLen = 0;

    // Write out the pose parameters.
    for (int i = 0; i < ANIR_MAX_ELEMENTS; i++)
    {
        const char* const poseParamName = animRecording->poseParamNames[i];

        if (poseParamName)
            numElems++;
        else
            break;

        const ssize_t strLen = (ssize_t)strlen(poseParamName) + 1; // Include the null too.
        FileSystem()->Write(poseParamName, strLen, animRecordingFile);

        stringBufLen += (int)strLen;
    }

    // Write out the pose values.
    for (int i = 0; i < numElems; i++)
    {
        const Vector2D* poseParamValue = &animRecording->poseParamValues[i];
        FileSystem()->Write(poseParamValue, sizeof(Vector2D), animRecordingFile);
    }

    int numSeqs = 0;

    // Write out the animation sequence names.
    for (int i = 0; i < ANIR_MAX_SEQUENCES; i++)
    {
        const char* const animSequenceName = animRecording->animSequences[i];

        if (animSequenceName)
            numSeqs++;
        else
            break;

        const ssize_t strLen = (ssize_t)strlen(animSequenceName) + 1; // Include the null too.
        FileSystem()->Write(animSequenceName, strLen, animRecordingFile);

        stringBufLen += (int)strLen;
    }

    // Write out the recorded frames.
    for (int i = 0; i < animRecording->numRecordedFrames; i++)
    {
        assert(animRecording->recordedFrames);

        const AnimRecordingFrame_s* const frame = &animRecording->recordedFrames[i];
        FileSystem()->Write(frame, sizeof(AnimRecordingFrame_s), animRecordingFile);
    }

    // Write out the recorded overlays.
    for (int i = 0; i < animRecording->numRecordedOverlays; i++)
    {
        assert(animRecording->recordedOverlays);

        const AnimRecordingOverlay_s* const overlay = &animRecording->recordedOverlays[i];
        FileSystem()->Write(overlay, sizeof(AnimRecordingOverlay_s), animRecordingFile);
    }

    // Update the data in the header if we ended up writing
    // elements and sequences.
    if (numElems > 0 || numSeqs > 0)
    {
        FileSystem()->Seek(animRecordingFile, offsetof(AnimRecordingFileHeader_s, stringBufSize), FILESYSTEM_SEEK_HEAD);

        FileSystem()->Write(&stringBufLen, sizeof(int), animRecordingFile);
        FileSystem()->Write(&numElems, sizeof(int), animRecordingFile);
        FileSystem()->Write(&numSeqs, sizeof(int), animRecordingFile);
    }

    FileSystem()->Close(animRecordingFile);

    Msg(eDLL_T::SERVER, "Recorded animation saved to \"%s\"\n", fileNameBuf);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


//---------------------------------------------------------------------------------
// Purpose: registers script functions in SERVER context
// Input: *s -
//---------------------------------------------------------------------------------
void Script_RegisterServerFunctions(CSquirrelVM* s)
{
    Script_RegisterCommonAbstractions(s);
    Script_RegisterCoreServerFunctions(s);
    Script_RegisterAdminServerFunctions(s);
    Script_RegisterDedicatedS21ServerNatives(s);

    // NOTE: plugin functions must always come after SDK functions!
    for (auto& callback : !PluginSystem()->GetRegisterServerScriptFuncsCallbacks())
    {
        // Register script functions inside plugins.
        callback.Function()(s);
    }
}

static void Script_RegisterLiveAPIEventTypes(CSquirrelVM* const s)
{
	Script_RegisterEnumTable(s, "eLiveAPI_EventTypes", 0,
		"ammoUsed",
		"arenasItemDeselected",
		"arenasItemSelected",
		"bannerCollected",
		"blackMarketAction",
		"characterSelected",
		"customEvent",
		"datacenter",
		"gameStateChanged",
		"gibraltarShieldAbsorbed",
		"grenadeThrown",
		"init",
		"inventoryDrop",
		"inventoryItem",
		"inventoryPickUp",
		"inventoryUse",
		"legendUpgradeSelected",
		"liveAPIEvent",
		"loadoutConfiguration",
		"matchSetup",
		"matchStateEnd",
		"observerAnnotation",
		"observerSwitched",
		"player",
		"playerAbilityUsed",
		"playerAssist",
		"playerConnected",
		"playerDamaged",
		"playerDisconnected",
		"playerDowned",
		"playerKilled",
		"playerRespawnTeam",
		"playerRevive",
		"playerStatChanged",
		"playerUpgradeTierChanged",
		"revenantForgedShadowDamaged",
		"ringFinishedClosing",
		"ringStartClosing",
		"squadEliminated",
		"vector3",
		"version",
		"warpGateUsed",
		"weaponSwitched",
		"wraithPortal",
		"ziplineUsed",
		"MAX"
	);
}

void Script_RegisterServerEnums(CSquirrelVM* const s)
{
	WeaponScriptVars_RegisterS21EWeaponVarAliases(s);
	Script_RegisterLiveAPIEventTypes(s);
}

//---------------------------------------------------------------------------------
// Purpose: core server script functions
// Input: *s -
//---------------------------------------------------------------------------------
void Script_RegisterCoreServerFunctions(CSquirrelVM* s)
{
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawSolidBox, "Draw a debug overlay solid box", "void", "vector origin, vector mins, vector maxs, vector color, float alpha, bool drawThroughWorld, float duration", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawSweptBox, "Draw a debug overlay swept box", "void", "vector start, vector end, vector mins, vector maxs, vector angles, vector color, float alpha, bool drawThroughWorld, float duration", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawTriangle, "Draw a debug overlay triangle", "void", "vector p1, vector p2, vector p3, vector color, float alpha, bool drawThroughWorld, float duration", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawSolidSphere, "Draw a debug overlay solid sphere", "void", "vector origin, float radius, int theta, int phi, vector color, float alpha, bool drawThroughWorld, float duration", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawCapsule, "Draw a debug overlay capsule", "void", "vector start, vector end, float radius, vector color, float alpha, bool drawThroughWorld, float duration", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, CreateBox, "Create a permanent box for map making", "void", "vector origin, vector angles, vector mins, vector maxs, vector color, float alpha", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, ClearBoxes, "Clear all debug overlays and boxes", "void", "", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, SetAutoReloadState, "Set whether we can auto-reload the server", "void", "bool canAutoReload", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, GetSessionID, "Gets our current session ID", "string", "", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_GetNearestPos, "Finds the nearest position to the provided point on the hull's NavMesh using the hull's bounds as extents", "vector ornull", "vector searchPoint, int hullType", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_GetNearestPosInBounds, "Finds the nearest position to the provided point on the hull's NavMesh using provided bounds as extents", "vector ornull", "vector searchPoint, vector halfExtents, int hullType", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_GetClosestPoint, "Finds the closest NavMesh position to a point, or returns the point if none is found", "vector", "vector point", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_GetClosestPoints, "Gets the closest NavMesh positions near a point", "array<vector>", "vector origin, int numPoints", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_TraceVerticalLine_PolyCount, "Counts NavMesh polygons stacked under a point", "int", "vector point", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_GetPointsInDifferentElevations, "Gets one NavMesh point per distinct elevation under a point", "array<vector>", "vector point", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_HasHarmfulAreaWithinDistance, "Returns whether a harmful NavMesh area is within distance of a point", "bool", "vector point, float distance", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_SearchForOpenAreas, "Finds open NavMesh positions matching area and adjacency criteria", "array<vector>", "int hullType, float idealTileArea, float minTileSize, float minSurroundingArea, float maxSurroundingArea, float maxPathCost, int minNumAdjacentTiles", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_FindUnboundPath, "Finds a NavMesh waypoint path between two positions; empty when unreachable", "array<vector>", "vector startPos, vector endPos", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, FS_StatsIngest,
        "Queue a 1v1 stats ingest POST. URL and host key come from convars. Returns true if queued",
        "bool",
        "string body",
        false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, SaveRecordedAnimation, "Saves an anim_recording asset to be used by bakery. (dev only)", "void", "var recordedAnim, string fileName", false);

    Script_RegisterRemoteFunctionServerNatives(s);

    // S21 bridge: server-VM bindings for the two precache natives the
    // S21 client expects. See engine/server/precache_natives.cpp

    Script_RegisterPrecacheServerNatives(s);

    s->RegisterConstant("SNDC_GLOBAL_NON_REWIND", 5);

    s->RegisterConstant("AKIMBO_STATE_NONE", 0);
    s->RegisterConstant("AKIMBO_STATE_SINGLE", 1);
    s->RegisterConstant("AKIMBO_STATE_OFFHAND", 2);
    s->RegisterConstant("AKIMBO_STATE_ACTIVE", 3);
    s->RegisterConstant("GRX_CURRENCY_PREMIUM", 0);
    s->RegisterConstant("GRX_CURRENCY_CREDITS", 1);
    s->RegisterConstant("GRX_CURRENCY_CRAFTING", 2);
    s->RegisterConstant("GRX_CURRENCY_HEIRLOOM", 3);
    s->RegisterConstant("GRX_CURRENCY_EXOTIC", 4);
    s->RegisterConstant("GRX_CURRENCY_ESCROW", 5);
    s->RegisterConstant("GRX_CURRENCY_EVENT", 6);
    s->RegisterConstant("GRX_CURRENCY_COUNT", 7);

    s->RegisterConstant("SHIELD_CHANGE_SOURCE_DIRECT", 0);
    s->RegisterConstant("SHIELD_CHANGE_SOURCE_REGEN", 1);
    s->RegisterConstant("FX_PATTACH_WEAPON_CHARGE_FRACTION_CURVED", 0x18);
    s->RegisterConstant("FORCE_STANCE_STAND", 0);
    s->RegisterConstant("FORCE_STANCE_CROUCH", 1);
    s->RegisterConstant("WT_GADGET", 9);
    s->RegisterConstant("TRACE_COLLISION_GROUP_NPC_MOVEMENT", 10); // S3 engine index
    // Newer script alias of the restrict-who-targets bit (same value as AI_AP_FLAG_TITAN_ONLY).
    s->RegisterConstant("AI_AP_FLAG_SMART_AI_ONLY", 1);

    // WPT_* weapon-type bitmask constants (shared across all three VMs)
    WeaponScriptVars_RegisterWPTConstants(s);

    s->RegisterConstant("PHASETYPE_DEFAULT", 0);
    s->RegisterConstant("PHASETYPE_BALANCE", 1);
    s->RegisterConstant("PHASETYPE_TUNNEL", 2);
    s->RegisterConstant("PHASETYPE_DASH", 3);
    s->RegisterConstant("PHASETYPE_GATE", 4);
    s->RegisterConstant("PHASETYPE_BREACH", 5);
    s->RegisterConstant("PHASETYPE_TRANSPORT", 6);
    s->RegisterConstant("PHASETYPE_DOOR", 7);
    s->RegisterConstant("PHASETYPE_TELEPORTER", 8);
    s->RegisterConstant("PHASETYPE_REWIND", 9);

    s->RegisterConstant("INFINITEAMMO_NONE", 0);
    s->RegisterConstant("INFINITEAMMO_CLIPS", 1);

    s->RegisterConstant("HIGHLIGHT_FLAG_REQUIRE_SAME_TEAM", 0x80);
    s->RegisterConstant("HIGHLIGHT_FLAG_REQUIRE_DIFFERENT_TEAM", 0x100);
    s->RegisterConstant("HIGHLIGHT_FLAG_REQUIRE_FRIENDLY_TEAM", 0x200);
    s->RegisterConstant("HIGHLIGHT_FLAG_REQUIRE_ENEMY_TEAM", 0x400);
    s->RegisterConstant("HIGHLIGHT_FLAG_REQUIRE_LOW_MOVEMENT", 0x1000);
    s->RegisterConstant("HIGHLIGHT_FLAG_REQUIRE_HIGH_MOVEMENT", 0x2000);
    s->RegisterConstant("HIGHLIGHT_FLAG_CHECK_OFTEN", 0x4000);
    s->RegisterConstant("HIGHLIGHT_FLAG_CHECK_NEXT_FRAME", 0x8000);
    s->RegisterConstant("HIGHLIGHT_FLAG_DISABLE_DEATH_FADE", 0x10000);
    s->RegisterConstant("HIGHLIGHT_FLAG_TEAM_AGNOSTIC", 0x20000);
    s->RegisterConstant("HIGHLIGHT_FLAG_ADDITIONAL_LOS_CHECKS", 0x80000);
    s->RegisterConstant("HIGHLIGHT_VIS_LOS_ENTSONLY_BLOCKSCAN", 7);

    HighlightContext_RegisterDrawFuncEnum(s->GetVM());

    Script_RegisterFuncNamed(s, "HighlightContext_GetId", "Script_HighlightContext_GetId", "Get highlight context id by name", "int", "string name", false, Script_HighlightContext_GetId);
    Script_RegisterFuncNamed(s, "HighlightContext_SetParam", "Script_HighlightContext_SetParam", "Set highlight param", "void", "int contextId, int paramIndex, vector value", false, Script_HighlightContext_SetParam);
    Script_RegisterFuncNamed(s, "HighlightContext_GetParam", "Script_HighlightContext_GetParam", "Get highlight param", "vector", "int contextId, int paramIndex", false, Script_HighlightContext_GetParam);
    Script_RegisterFuncNamed(s, "HighlightContext_SetDrawFunc", "Script_HighlightContext_SetDrawFunc", "Set draw function", "void", "int contextId, int drawFuncId", false, Script_HighlightContext_SetDrawFunc);
    Script_RegisterFuncNamed(s, "HighlightContext_GetDrawFunc", "Script_HighlightContext_GetDrawFunc", "Get draw function", "int", "int contextId", false, Script_HighlightContext_GetDrawFunc);
    Script_RegisterFuncNamed(s, "HighlightContext_SetRadius", "Script_HighlightContext_SetRadius", "Set outline radius", "void", "int contextId, float radius", false, Script_HighlightContext_SetRadius);
    Script_RegisterFuncNamed(s, "HighlightContext_GetOutlineRadius", "Script_HighlightContext_GetOutlineRadius", "Get outline radius", "float", "int contextId", false, Script_HighlightContext_GetOutlineRadius);
    Script_RegisterFuncNamed(s, "HighlightContext_GetInsideFunction", "Script_HighlightContext_GetInsideFunction", "Get inside function", "int", "int contextId", false, Script_HighlightContext_GetInsideFunction);
    Script_RegisterFuncNamed(s, "HighlightContext_GetOutlineFunction", "Script_HighlightContext_GetOutlineFunction", "Get outline function", "int", "int contextId", false, Script_HighlightContext_GetOutlineFunction);
    Script_RegisterFuncNamed(s, "HighlightContext_SetFlags", "Script_HighlightContext_SetFlags", "Set flags", "void", "int contextId, int flags", false, Script_HighlightContext_SetFlags);
    Script_RegisterFuncNamed(s, "HighlightContext_SetNearFadeDistance", "Script_HighlightContext_SetNearFadeDistance", "Set near fade distance", "void", "int contextId, float distance", false, Script_HighlightContext_SetNearFadeDistance);
    Script_RegisterFuncNamed(s, "HighlightContext_SetFarFadeDistance", "Script_HighlightContext_SetFarFadeDistance", "Set far fade distance", "void", "int contextId, float distance", false, Script_HighlightContext_SetFarFadeDistance);
    Script_RegisterFuncNamed(s, "HighlightContext_SetFocusedColor", "Script_HighlightContext_SetFocusedColor", "Set focused color", "void", "int contextId, vector color", false, Script_HighlightContext_SetFocusedColor);
    Script_RegisterFuncNamed(s, "HighlightContext_IsEntityVisible", "Script_HighlightContext_IsEntityVisible", "Is entity visible", "bool", "int contextId", false, Script_HighlightContext_IsEntityVisible);
    Script_RegisterFuncNamed(s, "HighlightContext_IsAfterPostProcess", "Script_HighlightContext_IsAfterPostProcess", "Is after post process", "bool", "int contextId", false, Script_HighlightContext_IsAfterPostProcess);

    Script_RegisterFuncNamed(s, "Weapon_GetBaseClassName", "Script_Global_Weapon_GetBaseClassName", "Returns baseclass or the weapon's classname", "string", "string weaponClassName", false, Script_Global_Weapon_GetBaseClassName);
    Script_RegisterFuncNamed(s, "Weapon_GetBaseClassNameOrEmpty", "Script_Global_Weapon_GetBaseClassNameOrEmpty", "Returns baseclass or empty string", "string", "string weaponClassName", false, Script_Global_Weapon_GetBaseClassNameOrEmpty);

    StatusEffects_SDK_RegisterServerFunctions(s);

    // GlobalNonRewind variable system
    Script_RegisterFuncNamed(s, "SetGlobalNonRewindNetBool", "Script_SetGlobalNonRewindNetBool", "Sets a global non-rewind bool", "void", "string name, bool value", false, Script_SetGlobalNonRewindNetBool);
    Script_RegisterFuncNamed(s, "SetGlobalNonRewindNetInt", "Script_SetGlobalNonRewindNetInt", "Sets a global non-rewind int", "void", "string name, int value", false, Script_SetGlobalNonRewindNetInt);
    Script_RegisterFuncNamed(s, "SetGlobalNonRewindNetFloat", "Script_SetGlobalNonRewindNetFloat", "Sets a global non-rewind float", "void", "string name, float value", false, Script_SetGlobalNonRewindNetFloat);
    Script_RegisterFuncNamed(s, "SetGlobalNonRewindNetTime", "Script_SetGlobalNonRewindNetTime", "Sets a global non-rewind time", "void", "string name, float value", false, Script_SetGlobalNonRewindNetTime);
    Script_RegisterFuncNamed(s, "GetGlobalNonRewindNetBool", "Script_GetGlobalNonRewindNetBool", "Gets a global non-rewind bool", "bool", "string name", false, Script_GetGlobalNonRewindNetBool);
    Script_RegisterFuncNamed(s, "GetGlobalNonRewindNetInt", "Script_GetGlobalNonRewindNetInt", "Gets a global non-rewind int", "int", "string name", false, Script_GetGlobalNonRewindNetInt);
    Script_RegisterFuncNamed(s, "GetGlobalNonRewindNetFloat", "Script_GetGlobalNonRewindNetFloat", "Gets a global non-rewind float", "float", "string name", false, Script_GetGlobalNonRewindNetFloat);
    Script_RegisterFuncNamed(s, "GetGlobalNonRewindNetTime", "Script_GetGlobalNonRewindNetTime", "Gets a global non-rewind time", "float", "string name", false, Script_GetGlobalNonRewindNetTime);
    Script_RegisterFuncNamed(s, "SetGlobalNonRewindNetEnt", "Script_SetGlobalNonRewindNetEnt", "Sets a global non-rewind entity", "void", "string name, entity ent", false, Script_SetGlobalNonRewindNetEnt);
    Script_RegisterFuncNamed(s, "GetGlobalNonRewindNetEnt", "Script_GetGlobalNonRewindNetEnt", "Gets a global non-rewind entity", "entity ornull", "string name", false, Script_GetGlobalNonRewindNetEnt);


    // Indexed deathfield natives (S21 signatures overwrite S3 no-index ones).
    DeathField_RegisterOnVM(s);

    // FreeDM/Control alliance natives (S3 missing SetTeamIsInAlliance) -- same RegisterOnVM pattern as DeathField
    AllianceCompat_RegisterOnVM(s);
}

//---------------------------------------------------------------------------------
// Purpose: admin server script functions
// Input: *s -
//---------------------------------------------------------------------------------
void Script_RegisterAdminServerFunctions(CSquirrelVM* s)
{
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, GetNumHumanPlayers, "Gets the number of human players on the server", "int", "", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, GetNumFakeClients, "Gets the number of bot players on the server", "int", "", false);
    CmdRecorder_RegisterGlobalFuncs(s);
    MapEditPaks_RegisterServerFunctions(s);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, CreateFakePlayer, "Creates a fake player and returns the edict index (-1 on failure). Use GetPlayerArray() to get entity.", "int", "string name, int team", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, KickPlayerByName, "Kicks a player from the server by name", "void", "string name, string reason", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, KickPlayerById, "Kicks a player from the server by handle or user id", "void", "string id, string reason", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, BanPlayerByName, "Bans a player from the server by name", "void", "string name, string reason", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, BanPlayerById, "Bans a player from the server by handle or user id", "void", "string id, string reason", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, UnbanPlayer, "Unbans a player from the server by user id or ip address", "void", "string handle", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, BroadcastServerTextMessage, "Broadcasts a chatmessage to all clients", "void", "string prefix, string message, bool adminMsg", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, BroadcastChatBuilder, "Broadcasts a styled chat message to all clients. Optional trailing bool adminMsg (default false) bypasses the recipient's chat filters.", "void", "array segments", true);
}

//---------------------------------------------------------------------------------
// Purpose: script code class function registration
//---------------------------------------------------------------------------------
static void Script_RegisterServerEntityClassFuncs()
{
    v_Script_RegisterServerEntityClassFuncs();

    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;

    WeaponScriptVars_RegisterEntityFuncs(g_serverScriptEntityStruct);
    WeaponScriptVars_RegisterWeaponTypeDisableFuncs(g_serverScriptEntityStruct);
    Translocation_RegisterProjectileFuncs(g_serverScriptEntityStruct);
    Script_RegisterDedicatedEntityNatives(g_serverScriptEntityStruct);
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerPlayerClassFuncs()
{
    v_Script_RegisterServerPlayerClassFuncs();
    Script_UpdateDedicatedPlayerDecoySignature();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;

    Script_RegisterDedicatedPlayerNatives(g_serverScriptPlayerStruct);

    g_serverScriptPlayerStruct->AddFunction("SetClassVar",
        "ScriptSetClassVar",
        "Change a variable in the player's class settings",
        "bool",
        "string key, string value",
        false,
        ServerScript_ScriptSetClassVar);

    g_serverScriptPlayerStruct->AddFunction("SendServerTextMessage",
        "ScriptSendServerTextMessage",
        "Sends a chat message to a player",
        "bool",
        "string prefix, string message, bool adminMsg",
        false,
        ServerScript_SendServerTextMessage);

    g_serverScriptPlayerStruct->AddFunction("ChatBuilder",
        "ScriptChatBuilder",
        "Sends a styled chat message. Pass an array of tables: { text = \"hi\", r = 255, g = 0, b = 0, rainbow = false, newline = true, sustain = 10.0, fade = 1.0 }. Only text is required. Optional trailing bool adminMsg (default false) bypasses the recipient's chat filters.",
        "bool",
        "array segments",
        true,
        ServerScript_ChatBuilder);

    g_serverScriptPlayerStruct->AddFunction("ChatBuilderRainbow",
        "ScriptChatBuilderRainbow",
        "Sends a rainbow-colored line (cycles through colors per character). Text over 64 characters is truncated. Optional trailing bool adminMsg (default false) bypasses the recipient's chat filters.",
        "bool",
        "string text, float sustain, float fadeTime",
        true,
        ServerScript_ChatBuilderRainbow);

    // Register shared player functions (PushForcedStance, GetLastTimeDamaged, skydive, etc.)
    Script_RegisterPlayerScriptFunctions(g_serverScriptPlayerStruct);
    if (ServerScript_IsDedicatedRuntime())
        Script_RegisterDedicatedPlayerScriptFunctions(g_serverScriptPlayerStruct);
    WeaponScriptVars_RegisterLaserSightOverride(g_serverScriptPlayerStruct);
    AkimboBridge_RegisterPlayerFuncs(g_serverScriptPlayerStruct);
    WeaponScriptVars_RegisterOffhandPlayerOverrides(g_serverScriptPlayerStruct, /*isServerStruct=*/true);
    JetDrive_RegisterScriptFunctions(g_serverScriptPlayerStruct);
    UpdraftBridge_RegisterScriptFunctions(g_serverScriptPlayerStruct);
    SkydiveBridge_RegisterScriptFunctions(g_serverScriptPlayerStruct);
    CmdRecorder_RegisterPlayerFuncs(g_serverScriptPlayerStruct);
    BotCmd_RegisterPlayerFuncs(g_serverScriptPlayerStruct);
    Translocation_RegisterPlayerFuncs(g_serverScriptPlayerStruct);

    // Register SERVER-ONLY player setters (NonRewind setters must not be on CLIENT)
    Script_RegisterPlayerScriptSetters(g_serverScriptPlayerStruct);
}
//---------------------------------------------------------------------------------
// Offhand natives, PhaseShiftBegin, and player-overheat bind on the combat
// character. Registering them on the player struct rebinds the name and NPC
// call sites throw.
static void Script_RegisterServerCombatCharacterClassFuncs()
{
    v_Script_RegisterServerCombatCharacterClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;

    WeaponScriptVars_RegisterPhaseShiftOverride(g_serverScriptCombatCharacterStruct);
    WeaponScriptVars_RegisterOffhandOverrides(g_serverScriptCombatCharacterStruct, /*isServerStruct=*/true);
    ContextAction_RegisterScriptFunctions(g_serverScriptCombatCharacterStruct);
    PlayerOverheat_RegisterCombatCharacterFuncs(g_serverScriptCombatCharacterStruct);
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerAIClassFuncs()
{
    v_Script_RegisterServerAIClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}

//---------------------------------------------------------------------------------
static void Script_RegisterServerWeaponClassFuncs()
{
    v_Script_RegisterServerWeaponClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;

    Script_RegisterDedicatedWeaponNatives();

    WeaponScriptVars_RegisterWeaponFuncs(g_serverScriptWeaponStruct);
    WeaponScriptVars_RegisterWeaponLockedSetSetter(g_serverScriptWeaponStruct);
    WeaponScriptVars_RegisterInfiniteAmmoFuncs(g_serverScriptWeaponStruct);
    WeaponScriptVars_RegisterInfiniteAmmoSetter(g_serverScriptWeaponStruct);
    WeaponHeat_RegisterWeaponFuncs(g_serverScriptWeaponStruct);
    EnergizeBridge_RegisterWeaponFuncs(g_serverScriptWeaponStruct);
    AkimboBridge_RegisterWeaponFuncs(g_serverScriptWeaponStruct);
    PlayerOverheat_RegisterWeaponFuncs(g_serverScriptWeaponStruct);
    Translocation_RegisterWeaponFuncs(g_serverScriptWeaponStruct);
}

//---------------------------------------------------------------------------------
static void Script_RegisterServerProjectileClassFuncs()
{
    v_Script_RegisterServerProjectileClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerTitanSoulClassFuncs()
{
    v_Script_RegisterServerTitanSoulClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerPlayerDecoyClassFuncs()
{
    v_Script_RegisterServerPlayerDecoyClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerSpawnpointClassFuncs()
{
    v_Script_RegisterServerSpawnpointClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerFirstPersonProxyClassFuncs()
{
    v_Script_RegisterServerFirstPersonProxyClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}

//---------------------------------------------------------------------------------
// Purpose: run after the engine's GRX block so COUNT=7 overrides the engine's 4.
//---------------------------------------------------------------------------------
static void Hook_Script_RegisterServerCodeConstants(CSquirrelVM* s)
{
    v_Script_RegisterServerCodeConstants(s); // run engine codeconsts (writes S3 schema)

    // S21 GRX_CURRENCY: keep PREMIUM/CREDITS/CRAFTING; COUNT is 7 with four new keys.
    s->RegisterConstant("GRX_CURRENCY_HEIRLOOM", 3);
    s->RegisterConstant("GRX_CURRENCY_EXOTIC",   4);
    s->RegisterConstant("GRX_CURRENCY_ESCROW",   5);
    s->RegisterConstant("GRX_CURRENCY_EVENT",    6);
    s->RegisterConstant("GRX_CURRENCY_COUNT",    7);
}

//---------------------------------------------------------------------------------
// Purpose: S21 has 12 inventory slots; dual-wield partner is main+7, not main+5.
//---------------------------------------------------------------------------------
static void Hook_Script_RegisterServerWeaponSlotConstants(CSquirrelVM* s)
{
    v_Script_RegisterServerWeaponSlotConstants(s); // run engine codeconsts (writes S3 schema)

    s->RegisterConstant("WEAPON_INVENTORY_SLOT_ANTI_TITAN",     5);
    s->RegisterConstant("WEAPON_INVENTORY_SLOT_DUALPRIMARY_0",  7);
    s->RegisterConstant("WEAPON_INVENTORY_SLOT_DUALPRIMARY_1",  8);
    s->RegisterConstant("WEAPON_INVENTORY_SLOT_DUALPRIMARY_2",  9);
    s->RegisterConstant("WEAPON_INVENTORY_SLOT_DUALPRIMARY_3", 10);

    // Engine stow marker (0xFD); the engine schema predates it, so name it
    // here for scripts testing the stow edge. INVALID/ANY stay engine-owned.
    s->RegisterConstant("WEAPON_INVENTORY_SLOT_HOLSTERED", 0xFD);

    Msg(eDLL_T::SERVER, "[WEAP-SLOT] S21 inventory slot schema applied "
        "(sling=4, anti_titan=5, gadget=6)\n");
}

void VScriptServer::Detour(const bool bAttach) const
{
    DetourSetup(&v_ServerScript_DebugScreenText, &ServerScript_DebugScreenText, bAttach);
    DetourSetup(&v_ServerScript_DebugScreenTextWithColor, &ServerScript_DebugScreenTextWithColor, bAttach);

    DetourSetup(&v_Script_RegisterServerCodeConstants, &Hook_Script_RegisterServerCodeConstants, bAttach);

    if (v_Script_RegisterServerWeaponSlotConstants)
        DetourSetup(&v_Script_RegisterServerWeaponSlotConstants, &Hook_Script_RegisterServerWeaponSlotConstants, bAttach);

	Script_DedicatedTraceDetour(bAttach);

	if (v_ScriptSetAimAssistAllowed)
		DetourSetup(&v_ScriptSetAimAssistAllowed, &Script_SetAimAssistAllowed, bAttach);

    DetourSetup(&v_Script_RegisterServerEntityClassFuncs, &Script_RegisterServerEntityClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerPlayerClassFuncs, &Script_RegisterServerPlayerClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerCombatCharacterClassFuncs, &Script_RegisterServerCombatCharacterClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerAIClassFuncs, &Script_RegisterServerAIClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerWeaponClassFuncs, &Script_RegisterServerWeaponClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerProjectileClassFuncs, &Script_RegisterServerProjectileClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerTitanSoulClassFuncs, &Script_RegisterServerTitanSoulClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerPlayerDecoyClassFuncs, &Script_RegisterServerPlayerDecoyClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerSpawnpointClassFuncs, &Script_RegisterServerSpawnpointClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerFirstPersonProxyClassFuncs, &Script_RegisterServerFirstPersonProxyClassFuncs, bAttach);
}
