//=============================================================================//
// 
// Purpose: Expose native code to VScript API
// 
//-----------------------------------------------------------------------------
// 
// Read the documentation in 'game/shared/vscript_shared.cpp' before modifying
// existing code or adding new code!
// 
// To create client script bindings
// - use the DEFINE_CLIENT_SCRIPTFUNC_NAMED macro.
// - prefix your function with "ClientScript_" i.e.: "ClientScript_GetVersion".
// 
// To create ui script bindings
// - use the DEFINE_UI_SCRIPTFUNC_NAMED macro.
// - prefix your function with "UIScript_" i.e.: "UIScript_GetVersion".
// 
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/frametask.h"
#include "tier1/keyvalues.h"
#include "engine/cmodel_bsp.h"
#include "engine/host_state.h"
#include "engine/debugoverlay.h"
#include "pluginsystem/pluginsystem.h"
#include "networksystem/spire.h"
#include "localize/localize_disk.h"
#include "ebisusdk/EbisuSDK.h"
#include "networksystem/listmanager.h"
#include "networksystem/hostmanager.h"
#include "pluginsystem/modsystem.h"
#include "engine/client/bridge_connect_password.h"
#include "game/client/hud_basechat.h"

#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"

#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/scriptnetdata_limits.h"

#include "game/shared/vscript_shared.h"

#include "vscript_client.h"
#include "vscript_player.h"
#include "scriptnetdata_client.h"
#include "ruitracks.h"
#include "vscript/vsquirrel_s21.h"
#include "game/client/clientleafsystem.h"
#include "game/client/c_baseentity.h"
#include "game/shared/weapon_script_vars.h"
#include "game/shared/globalnonrewind_vars.h"
#include "game/shared/deathfield_system.h"
#include "game/shared/highlight_context.h"
#include "game/shared/status_effects_sdk.h"
#include "classvar_natives.h"
#include "vscript/languages/squirrel_re/include/sqarray.h"
#include "public/globalvars_base.h"
#include "inputsystem/inputsystem.h"

extern CGlobalVarsBase* gpGlobals;

// NOTE: script_client / script_ui ConCommands live in vsquirrel_s21.cpp.
// Defining them here in client_static.lib made the linker drop this TU.

//-----------------------------------------------------------------------------
// Purpose: performance-HUD SPING for system menu (not EA MyPing). Late-reg only.
//-----------------------------------------------------------------------------
SQRESULT UIScript_GetConnectionPingMs(HSQUIRRELVM v)
{
    if (!v || !v_sq_pushinteger)
    {
        Warning(eDLL_T::CLIENT,
            "GetConnectionPingMs: v_sq_pushinteger unresolved (v=%p)\n", (void*)v);
        return SQ_ERROR;
    }
    v_sq_pushinteger(v, static_cast<SQInteger>(RuiTracks_GetConnectionPingMs()));
    return SQ_OK;
}

//-----------------------------------------------------------------------------
// Purpose: report platform identity progress to the menu
// Output: one of PLATFORM_IDENTITY_*, mirrored by ePlatformIdentity in script
//-----------------------------------------------------------------------------
SQRESULT UIScript_GetPlatformIdentityState(HSQUIRRELVM v)
{
    if (!v || !v_sq_pushinteger)
    {
        Warning(eDLL_T::CLIENT,
            "GetPlatformIdentityState: v_sq_pushinteger unresolved (v=%p)\n", (void*)v);
        return SQ_ERROR;
    }
    v_sq_pushinteger(v, static_cast<SQInteger>(EbisuSDK_GetPlatformIdentityState()));
    return SQ_OK;
}

//-----------------------------------------------------------------------------
// Purpose: register the identity state accessor on the UI VM
//-----------------------------------------------------------------------------
void Script_RegisterPlatformIdentityUI(CSquirrelVM* s)
{
    if (!s)
    {
        Warning(eDLL_T::CLIENT,
            "[S21-REG] Script_RegisterPlatformIdentityUI: null UI VM\n");
        return;
    }

    // Unrecognised return type still registers as 'var'; the error only shows at a call site.
    const SQRESULT r = Script_RegisterFuncTC_S21(s, "GetPlatformIdentityState",
        reinterpret_cast<void*>(UIScript_GetPlatformIdentityState),
        "int", "");

    if (r != SQ_OK)
    {
        Warning(eDLL_T::CLIENT,
            "[S21-REG] GetPlatformIdentityState registration FAILED (r=%d)\n", (int)r);
        return;
    }
}

void Script_RegisterConnectionPingUI(CSquirrelVM* s)
{
    if (!s)
    {
        Warning(eDLL_T::CLIENT,
            "[S21-REG] Script_RegisterConnectionPingUI: null UI VM\n");
        return;
    }
    // Return type must be "integer", not "int".
    const SQRESULT r = Script_RegisterFuncTC_S21(s, "GetConnectionPingMs",
        reinterpret_cast<void*>(UIScript_GetConnectionPingMs),
        "integer", "");
    if (r != SQ_OK)
    {
        Warning(eDLL_T::CLIENT,
            "[S21-REG] GetConnectionPingMs registration FAILED (r=%d)\n", (int)r);
        return;
    }
}

// Set while a browser refresh is outstanding so the panel can show a
// spinner instead of an empty list.
static std::atomic<bool> s_serverListRequestInFlight{ false };
// Worker publishes completion here; main thread drains (g_TaskQueue is
// unpumped -- VHost is not registered on the client).
static std::atomic<bool> s_serverListComplete{ false };
static bool s_serverListPendingSuccess = false;
static size_t s_serverListPendingCount = 0;
static string s_serverListPendingMessage;
// Last message from the master server, surfaced to the panel verbatim.
// Written only on the main thread (drain + natives); no extra lock.
static string s_serverListMessage;

static void UIScript_FireServerListCompleted(bool success, const string& errorMsg, int serverCount)
{
	if (!g_pUIScript)
		return;

	HSCRIPT onRequestComplete = g_pUIScript->FindFunction(
		"UICodeCallback_OnServerListRequestCompleted",
		"void functionref( bool success, string errorMsg, int serverCount )",
		nullptr);

	if (!onRequestComplete)
	{
		Warning(eDLL_T::CLIENT,
			"[SERVERBROWSER] UICodeCallback_OnServerListRequestCompleted missing\n");
		return;
	}

	ScriptVariant_t args[3] = { success, errorMsg.c_str(), serverCount };
	g_pUIScript->ExecuteFunction(onRequestComplete, args, SDK_ARRAYSIZE(args), nullptr, 0);

	// Not freed on purpose. FindFunction returns engine-allocated memory; free()
	// through the wrong CRT corrupted the heap (see prior note).
}

// Main-thread only. Called from list natives so UI scripts that poll in-flight
// or read the list still get a completion even when VHost/TaskQueue is skipped.
static void UIScript_DrainServerListCompletion(void)
{
	if (!s_serverListComplete.exchange(false, std::memory_order_acq_rel))
		return;

	s_serverListMessage = s_serverListPendingMessage;
	const bool success = s_serverListPendingSuccess;
	const int count = static_cast<int>(s_serverListPendingCount);

	UIScript_FireServerListCompleted(success, s_serverListMessage, count);
}

//-----------------------------------------------------------------------------
// Purpose: checks if the server index is valid, raises an error if not
//-----------------------------------------------------------------------------
static SQBool Script_CheckServerIndexAndFailure(HSQUIRRELVM v, SQInteger iServer)
{
    const SQInteger iCount = static_cast<SQInteger>(g_ServerListManager.m_vServerList.size());
    const char* reason = nullptr;

    if (iServer == -1) // If its still -1, then 'sq_getinteger' failed
        reason = "Invalid argument type provided.";
    else if (iServer < 0)
        reason = "Index must not be negative.";
    else if (iServer >= iCount)
        reason = "Index out of range.";

    if (!reason)
        return true;

    // v_SQVM_RaiseError is null on this client (safe-mode skips GetFun).
    if (v_SQVM_RaiseError)
        v_SQVM_RaiseError(v, "%s (index %i, count %i)\n", reason, (int)iServer, (int)iCount);
    else
        Warning(eDLL_T::CLIENT, "[SERVERBROWSER] %s (index %i, count %i)\n",
            reason, (int)iServer, (int)iCount);

    // One-shot stack dump: S3 this=1, first arg=2.
    static bool bDumpedStack = false;
    if (!bDumpedStack)
    {
        bDumpedStack = true;

        const SQInteger top = sq_gettop(v);
        Warning(eDLL_T::CLIENT, "[SERVERBROWSER] stack diag: gettop=%i\n", (int)top);

        for (SQInteger i = 1; i <= 4 && i <= top; ++i)
        {
            const SQObjectPtr& o = stack_get(v, i);
            SQInteger asInt = 0;
            const bool numeric = sq_isnumeric(o) != 0;

            if (numeric)
                asInt = tointeger(o);

            Warning(eDLL_T::CLIENT, "[SERVERBROWSER]   slot %i: type=0x%08X numeric=%d value=%i\n",
                (int)i, (unsigned int)sq_type(o), numeric ? 1 : 0, (int)asInt);
        }
    }

    return false;
}

// Same bounds as Script_CheckServerIndexAndFailure, but silent -- stale panel index yields a blank row.
static bool Script_ServerIndexValid(SQInteger iServer)
{
    if (iServer < 0)
        return false;
    if (iServer >= static_cast<SQInteger>(g_ServerListManager.m_vServerList.size()))
        return false;
    return true;
}

static void Internal_UIScript_RequestForServerBrowserListThreaded()
{
    string responseMsg;
    size_t serverCount;

    const bool success = g_ServerListManager.RefreshServerList(responseMsg, serverCount);

    // Do not use g_TaskQueue: VHost is not in the safe-mode allowlist, so Dispatch never runs.
    s_serverListPendingSuccess = success;
    s_serverListPendingCount = serverCount;
    s_serverListPendingMessage = std::move(responseMsg);
    s_serverListRequestInFlight.store(false, std::memory_order_release);
    s_serverListComplete.store(true, std::memory_order_release);
}

//-----------------------------------------------------------------------------
// Purpose: refreshes the server list
//-----------------------------------------------------------------------------
static SQRESULT UIScript_RequestServerList(HSQUIRRELVM v)
{
    if (s_serverListRequestInFlight.exchange(true))
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    g_Spire.SetLanguage(Localize_GetCurrentLanguage());
    std::thread(Internal_UIScript_RequestForServerBrowserListThreaded).detach();
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: get current server count from spire
//-----------------------------------------------------------------------------
static SQRESULT UIScript_GetServerCount(HSQUIRRELVM v)
{
    UIScript_DrainServerListCompletion();

    AUTO_LOCK(g_ServerListManager.m_Mutex);

    size_t iCount = g_ServerListManager.m_vServerList.size();
    sq_pushinteger(v, static_cast<SQInteger>(iCount));

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}
//-----------------------------------------------------------------------------
// Purpose: get server's current name from server list index
//-----------------------------------------------------------------------------
static SQRESULT UIScript_GetServerName(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_CheckServerIndexAndFailure(v, iServer))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const string& serverName = g_ServerListManager.m_vServerList[iServer].name;
    sq_pushstring(v, serverName.c_str(), (SQInteger)serverName.length());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: get server's current description from server list index
//-----------------------------------------------------------------------------
static SQRESULT UIScript_GetServerDescription(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_CheckServerIndexAndFailure(v, iServer))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const string& serverDescription = g_ServerListManager.m_vServerList[iServer].description;
    sq_pushstring(v, serverDescription.c_str(), (SQInteger)serverDescription.length());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: get server's current map via server list index
//-----------------------------------------------------------------------------
static SQRESULT UIScript_GetServerMap(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_CheckServerIndexAndFailure(v, iServer))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const string& serverMapName = g_ServerListManager.m_vServerList[iServer].map;
    sq_pushstring(v, serverMapName.c_str(), (SQInteger)serverMapName.length());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: get server's current playlist via server list index
//-----------------------------------------------------------------------------
static SQRESULT UIScript_GetServerPlaylist(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_CheckServerIndexAndFailure(v, iServer))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const string& serverPlaylist = g_ServerListManager.m_vServerList[iServer].playlist;
    sq_pushstring(v, serverPlaylist.c_str(), (SQInteger)serverPlaylist.length());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: get server's current player count via server list index
//-----------------------------------------------------------------------------
static SQRESULT UIScript_GetServerCurrentPlayers(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_CheckServerIndexAndFailure(v, iServer))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQInteger playerCount = g_ServerListManager.m_vServerList[iServer].numPlayers;
    sq_pushinteger(v, playerCount);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: get server's current player count via server list index
//-----------------------------------------------------------------------------
static SQRESULT UIScript_GetServerMaxPlayers(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_CheckServerIndexAndFailure(v, iServer))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQInteger maxPlayers = g_ServerListManager.m_vServerList[iServer].maxPlayers;
    sq_pushinteger(v, maxPlayers);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetServerHasPassword(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_CheckServerIndexAndFailure(v, iServer))
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    const SQBool hasPassword = g_ServerListManager.m_vServerList[iServer].hasPassword;
    sq_pushbool(v, hasPassword);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetServerRequiredMods(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    sq_newarray(v, 0);

    if (!Script_ServerIndexValid(iServer))
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    for (const string& s : g_ServerListManager.m_vServerList[iServer].requiredMods)
    {
        sq_pushstring(v, s.c_str(), (SQInteger)s.length());
        sq_arrayappend(v, -2);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetServerAllowedMods(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    sq_newarray(v, 0);

    if (!Script_ServerIndexValid(iServer))
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    for (const string& s : g_ServerListManager.m_vServerList[iServer].allowedMods)
    {
        sq_pushstring(v, s.c_str(), (SQInteger)s.length());
        sq_arrayappend(v, -2);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetServerModsProfile(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_ServerIndexValid(iServer))
    {
        sq_pushstring(v, "", 0);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    const string& modsProfile = g_ServerListManager.m_vServerList[iServer].modsProfile;
    sq_pushstring(v, modsProfile.c_str(), (SQInteger)modsProfile.length());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetServerRegion(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_ServerIndexValid(iServer))
    {
        sq_pushstring(v, "", 0);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    const string& region = g_ServerListManager.m_vServerList[iServer].region;
    sq_pushstring(v, region.c_str(), (SQInteger)region.length());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetServerMissingMods(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_ServerIndexValid(iServer))
    {
        sq_pushstring(v, "", 0);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    CUtlVector<CUtlString> required;
    for (const string& s : g_ServerListManager.m_vServerList[iServer].requiredMods)
        required.AddToTail(s.c_str());

    CUtlVector<CUtlString> missing;
    ModSystem_ComputeMissing(required, missing);

    string joined;
    FOR_EACH_VEC(missing, i)
    {
        if (i)
            joined.append(", ");
        joined.append(missing[i].String());
    }

    sq_pushstring(v, joined.c_str(), (SQInteger)joined.length());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_ServerListHasRequiredMods(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_ServerIndexValid(iServer))
    {
        sq_pushbool(v, false);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    CUtlVector<CUtlString> required;
    for (const string& s : g_ServerListManager.m_vServerList[iServer].requiredMods)
        required.AddToTail(s.c_str());

    sq_pushbool(v, ModSystem_HasRequiredMods(required) ? true : false);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetServerListMessage(HSQUIRRELVM v)
{
    UIScript_DrainServerListCompletion();
    sq_pushstring(v, s_serverListMessage.c_str(), (SQInteger)s_serverListMessage.length());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_IsServerListRequestInFlight(HSQUIRRELVM v)
{
    UIScript_DrainServerListCompletion();
    sq_pushbool(v, s_serverListRequestInFlight.load(std::memory_order_acquire));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_ClearConnectPassword(HSQUIRRELVM v)
{
    Bridge_SetConnectPassword("");
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: set netchannel encryption key and connect to server
//-----------------------------------------------------------------------------
static SQRESULT UIScript_ConnectToListedServer(HSQUIRRELVM v)
{
    AUTO_LOCK(g_ServerListManager.m_Mutex);

    SQInteger iServer = -1;
    sq_getinteger(v, 2, &iServer);

    if (!Script_CheckServerIndexAndFailure(v, iServer))
    {
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const NetGameServer_t& gameServer = g_ServerListManager.m_vServerList[iServer];

    g_ServerListManager.ConnectToServer(gameServer.address, gameServer.port, gameServer.netKey, string());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// EULA text from the master server. Pull-based: no main-thread tick to dispatch a callback from here.
static std::atomic<bool> s_eulaRequestInFlight{ false };
static std::atomic<bool> s_eulaComplete{ false };
static string s_eulaPendingContents;
static int s_eulaPendingVersion = 0;
// Main-thread only, published by the drain below.
static string s_eulaContents;
static int s_eulaVersion = 0;

static void Internal_UIScript_RequestEULAThreaded_S21()
{
    MSEulaData_t eulaData;
    string responseMsg;

    if (g_Spire.GetEULA(eulaData, responseMsg))
    {
        s_eulaPendingContents = std::move(eulaData.contents);
        s_eulaPendingVersion = eulaData.version;
    }
    else
    {
        Warning(eDLL_T::CLIENT, "[EULA] fetch failed: %s\n", responseMsg.c_str());
        s_eulaPendingContents.clear();
        s_eulaPendingVersion = 0;
    }

    s_eulaRequestInFlight.store(false, std::memory_order_release);
    s_eulaComplete.store(true, std::memory_order_release);
}

static void UIScript_DrainEULACompletion()
{
    if (!s_eulaComplete.exchange(false, std::memory_order_acquire))
        return;

    s_eulaContents = std::move(s_eulaPendingContents);
    s_eulaVersion = s_eulaPendingVersion;
}

static SQRESULT UIScript_RequestEULAContents(HSQUIRRELVM v)
{
    if (s_eulaRequestInFlight.exchange(true))
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

    const char* lang = Localize_GetCurrentLanguage();
    g_Spire.SetLanguage(lang);
    DevMsg(eDLL_T::CLIENT, "[EULA] requesting lang='%s'\n", lang);
    std::thread(Internal_UIScript_RequestEULAThreaded_S21).detach();
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Empty until the fetch lands, and after a failure: the script keeps its built-in
// text in that case rather than showing a blank agreement.
static SQRESULT UIScript_GetEULAContents(HSQUIRRELVM v)
{
    UIScript_DrainEULACompletion();
    sq_pushstring(v, s_eulaContents.c_str(), -1);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// 0 means the master server served no version; the script keeps its local one so
// players are not re-prompted on every launch.
static SQRESULT UIScript_GetEULAVersion(HSQUIRRELVM v)
{
    UIScript_DrainEULACompletion();
    sq_pushinteger(v, static_cast<SQInteger>(s_eulaVersion));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//---------------------------------------------------------------------------------
// Purpose: binds the server-browser natives to the UI VM. Late-register; bulk UI register is not fired.
//---------------------------------------------------------------------------------
void Script_RegisterServerBrowserUI(CSquirrelVM* s)
{
    if (!s)
    {
        Warning(eDLL_T::CLIENT, "[S21-REG] Script_RegisterServerBrowserUI: null UI VM\n");
        return;
    }

    struct Reg_t
    {
        const char* name;
        void* func;
        const char* ret;
        const char* params;
    };

    // Type compiler path only. Script_RegisterFunc_S21 stores param memory
    // the engine frees across the CRT boundary.
    const Reg_t natives[] =
    {
        // Engine does not bind these on S21; lobby RequestServerList needs them here.
        { "RequestServerList",                (void*)UIScript_RequestServerList,                 "void",           "" },
        { "GetServerCount",                   (void*)UIScript_GetServerCount,                    "int",            "" },
        { "GetServerName",                    (void*)UIScript_GetServerName,                     "string",         "int index" },
        { "GetServerDescription",             (void*)UIScript_GetServerDescription,              "string",         "int index" },
        { "GetServerMap",                     (void*)UIScript_GetServerMap,                      "string",         "int index" },
        { "GetServerPlaylist",                (void*)UIScript_GetServerPlaylist,                 "string",         "int index" },
        { "GetServerCurrentPlayers",          (void*)UIScript_GetServerCurrentPlayers,           "int",            "int index" },
        { "GetServerMaxPlayers",              (void*)UIScript_GetServerMaxPlayers,               "int",            "int index" },
        { "GetServerHasPassword",             (void*)UIScript_GetServerHasPassword,              "bool",           "int index" },
        { "ConnectToListedServer",            (void*)UIScript_ConnectToListedServer,             "void",           "int index" },

        { "GetServerModsProfile",             (void*)UIScript_GetServerModsProfile,              "string",         "int index" },
        { "GetServerRegion",                  (void*)UIScript_GetServerRegion,                   "string",         "int index" },
        { "GetServerMissingMods",             (void*)UIScript_GetServerMissingMods,              "string",         "int index" },
        { "ServerListHasRequiredMods",        (void*)UIScript_ServerListHasRequiredMods,         "bool",           "int index" },
        { "GetServerListMessage",             (void*)UIScript_GetServerListMessage,              "string",         "" },
        { "IsServerListRequestInFlight",      (void*)UIScript_IsServerListRequestInFlight,       "bool",           "" },
        { "GetServerRequiredMods",            (void*)UIScript_GetServerRequiredMods,             "array< string >", "int index" },
        { "GetServerAllowedMods",             (void*)UIScript_GetServerAllowedMods,              "array< string >", "int index" },

        { "RequestEULAContents",              (void*)UIScript_RequestEULAContents,                "void",           "" },
        { "GetEULAContents",                  (void*)UIScript_GetEULAContents,                    "string",         "" },
        { "GetEULAVersion",                   (void*)UIScript_GetEULAVersion,                     "int",            "" },
        { "ClearConnectPassword",             (void*)UIScript_ClearConnectPassword,              "void",           "" },
    };

    for (const Reg_t& n : natives)
    {
        const SQRESULT r = Script_RegisterFuncTC_S21(s, n.name, n.func, n.ret, n.params);

        // S21 returns the native-closure type tag on success, not SQ_OK; only
        // SQ_ERROR is a hard failure.
        if (r == SQ_ERROR)
            Warning(eDLL_T::CLIENT, "[S21-REG] %s registration FAILED\n", n.name);
    }
}

//---------------------------------------------------------------------------------
// Purpose: per-sender chat mute natives, keyed on the SayText sender slot.
//---------------------------------------------------------------------------------
static SQRESULT ClientScript_SetChatMutedSlot(HSQUIRRELVM v)
{
    SQInteger nSlot = 0;
    SQBool bMuted = false;

    sq_getinteger(v, 2, &nSlot);
    sq_getbool(v, 3, &bMuted);

    Chat_SetSlotMuted(static_cast<int>(nSlot), bMuted != 0);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ClientScript_IsChatMutedSlot(HSQUIRRELVM v)
{
    SQInteger nSlot = 0;
    sq_getinteger(v, 2, &nSlot);

    sq_pushbool(v, Chat_IsSlotMuted(static_cast<int>(nSlot)));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ClientScript_ClearChatMutes(HSQUIRRELVM v)
{
    Chat_ClearMutedSlots();
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//---------------------------------------------------------------------------------
// Purpose: binds the chat-mute natives to the CLIENT VM. Late-register; bulk client register is not fired.
//---------------------------------------------------------------------------------
void Script_RegisterChatMuteClient(CSquirrelVM* s)
{
    if (!s)
    {
        Warning(eDLL_T::CLIENT, "[S21-REG] Script_RegisterChatMuteClient: null CLIENT VM\n");
        return;
    }

    struct Reg_t
    {
        const char* name;
        void* func;
        const char* ret;
        const char* params;
    };

    const Reg_t natives[] =
    {
        { "SetChatMutedSlot", (void*)ClientScript_SetChatMutedSlot, "void", "int slot, bool muted" },
        { "IsChatMutedSlot",  (void*)ClientScript_IsChatMutedSlot,  "bool", "int slot" },
        { "ClearChatMutes",   (void*)ClientScript_ClearChatMutes,   "void", "" },
    };

    for (const Reg_t& n : natives)
    {
        const SQRESULT r = Script_RegisterFuncTC_S21(s, n.name, n.func, n.ret, n.params);
        if (r == SQ_ERROR)
            Warning(eDLL_T::CLIENT, "[S21-REG] %s registration FAILED\n", n.name);
    }
}

//---------------------------------------------------------------------------------
// Purpose: console variables for scripts, these should not be used in engine/sdk code !!!
//---------------------------------------------------------------------------------
static ConVar settings_reflex("settings_reflex", "1", FCVAR_RELEASE, "Selected NVIDIA Reflex mode.", "0 = Off. 1 = On. 2 = On + Boost.");
static ConVar settings_antilag("settings_antilag", "1", FCVAR_RELEASE, "Selected AMD Anti-Lag mode.", "0 = Off. 1 = On.");

// NOTE: if we want to make a certain promo only show once, add the playerprofile flag to the cvar below. Current behavior = always show after game restart.
static ConVar promo_version_accepted("promo_version_accepted", "0", FCVAR_RELEASE, "The accepted promo version.");

static ConVar customMatch_enabled("customMatch_enabled", "0", FCVAR_RELEASE, "Enable custom match features.");
static ConVar gladCards_debug("gladCards_debug", "0", FCVAR_DEVELOPMENTONLY, "Enable gladiator card debug logging.");

