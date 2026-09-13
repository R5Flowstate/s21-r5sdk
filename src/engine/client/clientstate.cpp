//=============================================================================//
//
// Purpose
//
// $NoKeywords: $
//
//=============================================================================//
// clientstate.cpp: implementation of the CClientState class.
//
/////////////////////////////////////////////////////////////////////////////////
#include "core/stdafx.h"
#include "mathlib/bitvec.h"
#include "tier0/frametask.h"
#include "engine/common.h"
#include "engine/host.h"
#include "engine/host_cmd.h"
#include "clientstate.h"
#include "common/callback.h"
#include "cdll_engine_int.h"
#include "vgui/vgui_baseui_interface.h"
#include "rtech/playlists/playlists.h"
#include <ebisusdk/EbisuSDK.h>
#include <engine/cmd.h>
#include "filesystem/filesystem.h"
#include <new>
#include "networksystem/spire.h"
#include "common/proto_oob.h"
#include "engine/client/bridge_join_auth.h"
#include "engine/client/net_bridge_skip.h"
#include "tier1/strtools.h"

//------------------------------------------------------------------------------
// Purpose: console command callbacks
//------------------------------------------------------------------------------
static std::string SanitizePersonaName(const std::string& name); // forward
static void CL_MaskBadWords(std::string& name); // forward

static void SetName_f(const CCommand& args)
{
    if (args.ArgC() < 2)
        return;

    if (!IsOriginDisabled())
        return;

    const char* pszName = args.Arg(1);

    if (!pszName[0])
        pszName = "unnamed";

    // Sanitize to allowed ASCII set before applying
    std::string sanitized = SanitizePersonaName(pszName);
    // Mask bad words locally
    CL_MaskBadWords(sanitized);
    if (sanitized.empty())
        sanitized = "_";

    // Update offline persona name.
    strncpy(g_PersonaName, sanitized.c_str(), MAX_PERSONA_NAME_LEN - 1);
    g_PersonaName[MAX_PERSONA_NAME_LEN - 1] = '\0';
    name_cvar->SetValue(g_PersonaName);
}
static void Reconnect_f(const CCommand& args)
{
    // This product never resolves g_pClientState (VClientState is not
    // registered), so the rejoin runs off the bridge's connect chokepoint.
    Bridge_Reconnect();
}

//------------------------------------------------------------------------------
// Purpose: console commands
//------------------------------------------------------------------------------
static ConCommand cl_setname("cl_setname", SetName_f, "Sets the client's persona name", FCVAR_RELEASE);
static ConCommand reconnect("reconnect", Reconnect_f, "Reconnect to current server.", FCVAR_DONTRECORD|FCVAR_RELEASE);

//------------------------------------------------------------------------------
// Purpose: returns true if client simulation is paused
//------------------------------------------------------------------------------
bool CClientState::IsPaused() const
{
	return m_bPaused || !*host_initialized || g_pEngineVGui->ShouldPause();
}

//------------------------------------------------------------------------------
// Purpose: returns true if client is fully connected and active
//------------------------------------------------------------------------------
bool CClientState::IsActive(void) const
{
    return m_nSignonState == SIGNONSTATE::SIGNONSTATE_FULL;
};

//------------------------------------------------------------------------------
// Purpose: returns true if client connected but not active
//------------------------------------------------------------------------------
bool CClientState::IsConnected(void) const
{
    return m_nSignonState >= SIGNONSTATE::SIGNONSTATE_CONNECTED;
};

//------------------------------------------------------------------------------
// Purpose: returns true if client is still connecting
//------------------------------------------------------------------------------
bool CClientState::IsConnecting(void) const
{
    return m_nSignonState >= SIGNONSTATE::SIGNONSTATE_NONE;
}

//------------------------------------------------------------------------------
// Purpose: gets the client time
// Technically doesn't belong here
//------------------------------------------------------------------------------
float CClientState::GetClientTime() const
{
    if (m_bClockCorrectionEnabled)
    {
        return (float)m_ClockDriftMgr.m_nClientTick * g_pCommonHostState->interval_per_tick;
    }
    else
    {
        return m_flClockDriftFrameTime;
    }
}

//------------------------------------------------------------------------------
// Purpose: gets the simulation tick count
//------------------------------------------------------------------------------
int CClientState::GetTick() const
{
    return m_ClockDriftMgr.m_nClientTick;
}

//------------------------------------------------------------------------------
// Purpose: gets the last-received server tick count
//------------------------------------------------------------------------------
int CClientState::GetServerTickCount() const
{
    return m_ClockDriftMgr.m_nServerTick;
}

//------------------------------------------------------------------------------
// Purpose: sets the server tick count
//------------------------------------------------------------------------------
void CClientState::SetServerTickCount(int tick)
{
    m_ClockDriftMgr.m_nServerTick = tick;
}

//------------------------------------------------------------------------------
// Purpose: gets the client tick count
//------------------------------------------------------------------------------
int CClientState::GetClientTickCount() const
{
    return m_ClockDriftMgr.m_nClientTick;
}

//------------------------------------------------------------------------------
// Purpose: sets the client tick count
//------------------------------------------------------------------------------
void CClientState::SetClientTickCount(int tick)
{
    m_ClockDriftMgr.m_nClientTick = tick;
}

//------------------------------------------------------------------------------
// Purpose: gets the client frame time
//------------------------------------------------------------------------------
float CClientState::GetFrameTime() const
{
    if (IsPaused())
    {
        return 0.0f;
    }

    return m_flFrameTime;
}

//---------------------------------------------------------------------------------
// Purpose: registers net messages
// Input: *pClient - 
// *pChan - 
// Output: true if setup was successful, false otherwise
//---------------------------------------------------------------------------------
bool CClientState::VConnectionStart(CClientState* pClient, CNetChan* pChan)
{
    pClient->RegisterNetMsgs(pChan);
    bool result = CClientState__ConnectionStart(pClient, pChan);
    
    
    return result;
}

//------------------------------------------------------------------------------
// Purpose: called when connection to the server has been closed
//------------------------------------------------------------------------------
void CClientState::VConnectionClosing(CClientState* thisptr, const char* szReason)
{
    
    CClientState__ConnectionClosing(thisptr, szReason);


    // Delay execution to the next frame; this is required to avoid a rare crash.
    // Cannot reload playlists while still disconnecting.
    g_TaskQueue.Dispatch([]()
        {
            // VPlaylists is server-only; this resolver is null in client.dll.
            if (!v_Playlists_Download_f)
            {
                Warning(eDLL_T::ENGINE, "[PLAYLIST] Playlists_Download_f unresolved -- "
                    "skipping post-disconnect playlist reload\n");
                return;
            }

            v_Playlists_Download_f();
            Playlists_SDKInit();
        }, 0);
}

//------------------------------------------------------------------------------
// Purpose: SVC_ServerTick. Command tick -1 updates statistics only.
//------------------------------------------------------------------------------
bool CClientState::VProcessServerTick(CClientState* thisptr, SVC_ServerTick* msg)
{
    if (msg->m_NetTick.m_nCommandTick != -1)
    {
        // Updates statistics and updates clockdrift.
        return CClientState__ProcessServerTick(thisptr, msg);
    }
    else // Statistics only.
    {
        CClientState* const thisptr_ADJ = thisptr->GetShiftedBasePointer();

        if (thisptr_ADJ->IsConnected())
        {
            CNetChan* const pChan = thisptr_ADJ->m_NetChannel;

            pChan->SetRemoteFramerate(msg->m_NetTick.m_flHostFrameTime, msg->m_NetTick.m_flHostFrameTimeStdDeviation);
            pChan->SetRemoteCPUStatistics(msg->m_NetTick.m_nServerCPU);
        }

        return true;
    }
}

//------------------------------------------------------------------------------
// Purpose: processes string commands sent from server
// Input: *thisptr - 
// *msg - 
// Output: true on success, false otherwise
//------------------------------------------------------------------------------
bool CClientState::_ProcessStringCmd(CClientState* thisptr, NET_StringCmd* msg)
{
    CClientState* const thisptr_ADJ = thisptr->GetShiftedBasePointer();

    if (thisptr_ADJ->m_bRestrictServerCommands
        )
    {
        CCommand args;
        args.Tokenize(msg->cmd, cmd_source_t::kCommandSrcInvalid);

        if (args.ArgC() > 0)
        {
            if (!Cbuf_AddTextWithMarkers(msg->cmd,
                eCmdExecutionMarker_Enable_FCVAR_SERVER_CAN_EXECUTE,
                eCmdExecutionMarker_Disable_FCVAR_SERVER_CAN_EXECUTE))
            {
                DevWarning(eDLL_T::CLIENT, "%s: No room for %i execution markers; command \"%s\" ignored\n",
                    __FUNCTION__, 2, msg->cmd);
            }

            return true;
        }
    }
    else
    {
        Cbuf_AddText(Cbuf_GetCurrentPlayer(), msg->cmd, cmd_source_t::kCommandSrcCode);
    }

    return true;
}

//------------------------------------------------------------------------------
// Purpose: create's string tables from string table data sent from server
// Input: *thisptr - 
// *msg - 
// Output: true on success, false otherwise
//------------------------------------------------------------------------------
bool CClientState::_ProcessCreateStringTable(CClientState* thisptr, SVC_CreateStringTable* msg)
{
    CClientState* const cl = thisptr->GetShiftedBasePointer();

    if (!cl->IsConnected())
        return false;

    CNetworkStringTableContainer* const container = cl->m_StringTableContainer;

    // Must have a string table container at this point!
    if (!container)
    {
        Assert(0);

        COM_ExplainDisconnection(true, "String table container missing.\n");
        v_Host_Disconnect(true);

        return false;
    }

    container->AllowCreation(true);
    const ssize_t startbit = msg->m_DataIn.GetNumBitsRead();

    static constexpr unsigned int kMaxStringTableUncompressed = 16u * 1024u * 1024u;
    static constexpr unsigned int kMaxStringTableCompressed = 16u * 1024u * 1024u;
    static constexpr int kMaxStringTableEntries = 65536;
    static constexpr int kMaxUserDataSize = 4096;

    if (msg->m_nMaxEntries <= 0 || msg->m_nMaxEntries > kMaxStringTableEntries
        || msg->m_nUserDataSize < 0 || msg->m_nUserDataSize > kMaxUserDataSize)
    {
        Warning(eDLL_T::CLIENT, "%s: string table '%s' rejected (maxEntries=%d userData=%d)\n",
            __FUNCTION__, msg->m_szTableName ? msg->m_szTableName : "?",
            msg->m_nMaxEntries, msg->m_nUserDataSize);
        container->AllowCreation(false);
        COM_ExplainDisconnection(true, "String table bounds rejected.\n");
        v_Host_Disconnect(true);
        return false;
    }

    if (S21Bridge_CstCountInvalid(msg->m_nNumEntries, msg->m_nMaxEntries))
    {
        static volatile LONG s_cstNumEntriesWarn = 0;
        const LONG n = InterlockedIncrement(&s_cstNumEntriesWarn);
        if (n <= 8)
            Warning(eDLL_T::CLIENT, "%s: string table '%s' rejected (numEntries=%d maxEntries=%d)\n",
                __FUNCTION__, msg->m_szTableName ? msg->m_szTableName : "?",
                msg->m_nNumEntries, msg->m_nMaxEntries);
        container->AllowCreation(false);
        return false;
    }

    CNetworkStringTable* const table = (CNetworkStringTable*)container->CreateStringTable(false, msg->m_szTableName,
        msg->m_nMaxEntries, msg->m_nUserDataSize, msg->m_nUserDataSizeBits, msg->m_nDictFlags);
    if (!table)
    {
        Warning(eDLL_T::CLIENT, "%s: CreateStringTable failed for '%s'\n",
            __FUNCTION__, msg->m_szTableName ? msg->m_szTableName : "?");
        container->AllowCreation(false);
        COM_ExplainDisconnection(true, "String table container missing.\n");
        v_Host_Disconnect(true);
        return false;
    }

    table->SetTick(cl->GetServerTickCount());
    CClientState__HookClientStringTable(cl, msg->m_szTableName);

    if (msg->m_bDataCompressed)
    {
        unsigned int msgUncompressedSize = msg->m_DataIn.ReadLong();
        unsigned int msgCompressedSize = msg->m_DataIn.ReadLong();

        size_t uncompressedSize = msgUncompressedSize;
        size_t compressedSize = msgCompressedSize;

        bool bSuccess = false;

        if (msg->m_DataIn.TotalBytesAvailable() > 0 &&
            msgCompressedSize > 0 && msgUncompressedSize > 0 &&
            msgCompressedSize <= (unsigned int)msg->m_DataIn.TotalBytesAvailable() &&
            msgCompressedSize <= kMaxStringTableCompressed &&
            msgUncompressedSize <= kMaxStringTableUncompressed)
        {
            uint8_t* const uncompressedBuffer = new (std::nothrow) uint8_t[PAD_NUMBER(msgUncompressedSize, 4)];
            uint8_t* const compressedBuffer = new (std::nothrow) uint8_t[PAD_NUMBER(msgCompressedSize, 4)];

            if (!uncompressedBuffer || !compressedBuffer)
            {
                delete[] uncompressedBuffer;
                delete[] compressedBuffer;
                container->AllowCreation(false);
                Warning(eDLL_T::CLIENT, "%s: string table alloc failed (uncomp=%u comp=%u)\n",
                    __FUNCTION__, msgUncompressedSize, msgCompressedSize);
                COM_ExplainDisconnection(true, "String table allocation failed.\n");
                v_Host_Disconnect(true);
                return false;
            }

            msg->m_DataIn.ReadBytes(compressedBuffer, msgCompressedSize);

            bSuccess = NET_BufferToBufferDecompress(compressedBuffer, compressedSize, uncompressedBuffer, uncompressedSize);
            bSuccess &= (uncompressedSize == msgUncompressedSize);

            if (bSuccess)
            {
                bf_read data(uncompressedBuffer, (int)uncompressedSize);
                table->ParseUpdate(data, msg->m_nNumEntries);
            }

            delete[] uncompressedBuffer;
            delete[] compressedBuffer;
        }

        if (!bSuccess)
        {
            Assert(false);
            DevWarning(eDLL_T::CLIENT, "%s: Received malformed string table message!\n", __FUNCTION__);
        }
    }
    else
    {
        table->ParseUpdate(msg->m_DataIn, msg->m_nNumEntries);
    }

    container->AllowCreation(false);
    const ssize_t endbit = msg->m_DataIn.GetNumBitsRead();

    return (endbit - startbit) == msg->m_nLength;
}

//------------------------------------------------------------------------------
// Purpose: processes user message data
// Input: *thisptr - 
// *msg - 
// Output: true on success, false otherwise
//------------------------------------------------------------------------------
bool CClientState::_ProcessUserMessage(CClientState* thisptr, SVC_UserMessage* msg)
{
    CClientState* const cl = thisptr->GetShiftedBasePointer();

    if (!cl->IsConnected())
        return false;

    // buffer for incoming user message
    ALIGN4 byte userdata[MAX_USER_MSG_DATA] ALIGN4_POST = { 0 };
    bf_read userMsg("UserMessage(read)", userdata, sizeof(userdata));

    int bitsRead = msg->m_DataIn.ReadBitsClamped(userdata, msg->m_nLength);
    userMsg.StartReading(userdata, Bits2Bytes(bitsRead));

    // dispatch message to client.dll
    if (!g_pHLClient->DispatchUserMessage(msg->m_nMsgType, &userMsg))
    {
        Warning(eDLL_T::CLIENT, "Couldn't dispatch user message (%i)\n", msg->m_nMsgType);
        return false;
    }

    return true;
}

static ConVar cl_onlineAuthEnable("cl_onlineAuthEnable", "1", FCVAR_RELEASE, "Enables the client-side online authentication system");
static ConVar cl_onlineAuthForceLocal("cl_onlineAuthForceLocal", "0", FCVAR_RELEASE, "Run online authentication even when connecting to a local or private address");

static ConVar cl_onlineAuthToken("cl_onlineAuthToken", "", FCVAR_HIDDEN | FCVAR_USERINFO | FCVAR_DONTRECORD | FCVAR_SERVER_CANNOT_QUERY | FCVAR_PLATFORM_SYSTEM, "The client's online authentication token");
static ConVar cl_onlineAuthTokenSignature1("cl_onlineAuthTokenSignature1", "", FCVAR_HIDDEN | FCVAR_USERINFO | FCVAR_DONTRECORD | FCVAR_SERVER_CANNOT_QUERY | FCVAR_PLATFORM_SYSTEM, "The client's online authentication token signature", false, 0.f, false, 0.f, "Primary");
static ConVar cl_onlineAuthTokenSignature2("cl_onlineAuthTokenSignature2", "", FCVAR_HIDDEN | FCVAR_USERINFO | FCVAR_DONTRECORD | FCVAR_SERVER_CANNOT_QUERY | FCVAR_PLATFORM_SYSTEM, "The client's online authentication token signature", false, 0.f, false, 0.f, "Secondary");

static ConVar cl_sanitizePersonaName("cl_sanitizePersonaName", "1", FCVAR_RELEASE, "Sanitize persona name to printable ASCII (32-126); non-printable dropped");
static ConVar cl_nameFilterEnabled("cl_nameFilterEnabled", "1", FCVAR_RELEASE, "Mask bad words in client names with '*' using chatfilters/badwords.txt from VPKs");
static ConVar cl_nameFilterPath("cl_nameFilterPath", "chatfilters/badwords.txt", FCVAR_RELEASE, "Relative VPK path to bad word list");
static ConVar cl_allowIconsInNames("cl_allowIconsInNames", "0", FCVAR_RELEASE, "Allow game icon characters in displayed player names. 0 = Remove icons, 1 = Show icons");

static CUtlVector<string> g_ClientBadWords;
static bool g_ClientBadWordsLoaded = false;

// Unicode-aware case conversion for better international character support
static std::string CL_ToLowerUnicode(const std::string& input)
{
    std::string result = input;
    
    // Handle ASCII characters with standard tolower, preserve Unicode characters
    std::transform(result.begin(), result.end(), result.begin(), 
        [](unsigned char c) { 
            return (c <= 127) ? (char)tolower(c) : (char)c; 
        });
    
    return result;
}

// Function to remove blocked game icon characters from names
static void CL_RemoveBlockedIcons(std::string& name)
{
    if (name.empty()) return;
    
    std::string result;
    result.reserve(name.size());
    
    const unsigned char* p = reinterpret_cast<const unsigned char*>(name.c_str());
    const unsigned char* end = p + name.size();
    
    while (p < end && *p)
    {
        bool isBlockedIcon = false;
        
        // Check for UTF-8 sequences that represent the blocked icon characters
        // These characters are in the Private Use Area around U+F0000-U+F0FFF
        
        // UTF-8 encoding for U+F0000-U+F0FFF
        // 4-byte sequence: 0xF3 0xB0 0x80-0xBF 0x80-0xBF
        if (p + 3 < end && p[0] == 0xF3 && p[1] == 0xB0)
        {
            // This is likely one of the blocked icon characters, skip it
            p += 4; // Skip the 4-byte UTF-8 sequence
            isBlockedIcon = true;
        }
        // Also check for some other common Private Use Area ranges that might contain icons
        // U+E000-U+F8FF (3-byte UTF-8: 0xEE-0xEF)
        else if (p + 2 < end && p[0] >= 0xEE && p[0] <= 0xEF)
        {
            // Check if this matches the specific icon pattern
            if (p[0] == 0xEF && p[1] >= 0x80 && p[1] <= 0xBF)
            {
                // Block these specific Private Use Area characters, skip them
                p += 3; // Skip the 3-byte UTF-8 sequence
                isBlockedIcon = true;
            }
        }
        
        if (!isBlockedIcon)
        {
            // Copy the character to result
            if (*p < 0x80)
            {
                // ASCII character
                result += *p;
                p++;
            }
            else if ((*p & 0xE0) == 0xC0 && p + 1 < end)
            {
                // 2-byte UTF-8
                result += *p++;
                result += *p++;
            }
            else if ((*p & 0xF0) == 0xE0 && p + 2 < end)
            {
                // 3-byte UTF-8
                result += *p++;
                result += *p++;
                result += *p++;
            }
            else if ((*p & 0xF8) == 0xF0 && p + 3 < end)
            {
                // 4-byte UTF-8
                result += *p++;
                result += *p++;
                result += *p++;
                result += *p++;
            }
            else
            {
                // Invalid UTF-8, skip
                p++;
            }
        }
    }
    
    name = result;
}

static void CL_LoadNameFilter()
{
    g_ClientBadWords.RemoveAll();
    g_ClientBadWordsLoaded = false;

    const char* filePath = cl_nameFilterPath.GetString();
    if (!filePath || !*filePath)
        return;

    // S21: go through FileSystem_ReadAll (plain Open+Read+Close via
    // IBaseFileSystem) rather than the IFileSystem "optimal I/O" path.
    // See filesystem.h for the full rationale.
    ssize_t fs = 0;
    char* const buf = FileSystem_ReadAll(filePath, "GAME", &fs);
    if (!buf)
        return;

    const char* p = buf;
    while (*p)
    {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != '\r' && *p != '\n') ++p;
        string w(start, p - start);
        while (!w.empty() && (w.back() == ' ' || w.back() == '\t')) w.pop_back();
        if (!w.empty())
        {
            // Use Unicode-aware case conversion
            std::string lowerWord = CL_ToLowerUnicode(w);
            g_ClientBadWords.AddToTail(lowerWord);
        }
    }
    delete[] buf;
    g_ClientBadWordsLoaded = true;
}

static void CL_MaskBadWords(std::string& name)
{
    if (name.empty())
        return;

    // Remove blocked icon characters (unless allowed)
    if (!cl_allowIconsInNames.GetBool())
        CL_RemoveBlockedIcons(name);
    
    if (!cl_nameFilterEnabled.GetBool())
        return;

    if (!g_ClientBadWordsLoaded)
        CL_LoadNameFilter();
    if (!g_ClientBadWordsLoaded || g_ClientBadWords.IsEmpty())
        return;

    std::string lower = CL_ToLowerUnicode(name);

    FOR_EACH_VEC(g_ClientBadWords, i)
    {
        const std::string& bad = g_ClientBadWords[i];
        size_t pos = 0;
        while ((pos = lower.find(bad, pos)) != std::string::npos)
        {
            for (size_t k = 0; k < bad.size() && (pos + k) < name.size(); ++k)
            {
                name[pos + k] = '*';
                lower[pos + k] = '*';
            }
            pos += bad.size();
        }
    }
}

//------------------------------------------------------------------------------
// Purpose: Sanitize persona name to printable ASCII
//------------------------------------------------------------------------------
std::string SanitizePersonaName(const std::string& name)
{
    std::string sanitized;
    sanitized.reserve(name.length());

    auto isAllowed = [](unsigned char ch) -> bool { return ch >= 32 && ch <= 126; };

    for (unsigned char c : name)
    {
        if (isAllowed(c))
        {
            sanitized += static_cast<char>(c);
        }
        // else: drop disallowed char
    }

    // Trim to engine max (leave room for terminator in caller)
    if (sanitized.length() >= (size_t)MAX_PERSONA_NAME_LEN)
    {
        sanitized.resize(MAX_PERSONA_NAME_LEN - 1);
    }

    return sanitized;
}

//------------------------------------------------------------------------------
// Purpose: Origin identity dump.
//------------------------------------------------------------------------------
static void OriginInfo_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	Msg(eDLL_T::ENGINE, "=== Origin / offline identity ===\n");
	if (g_NucleusID)
		Msg(eDLL_T::ENGINE, "g_NucleusID: %llu\n", (unsigned long long)*g_NucleusID);
	if (g_PersonaName && g_PersonaName[0])
		Msg(eDLL_T::ENGINE, "g_PersonaName: '%s'\n", g_PersonaName);
	if (platform_user_id)
		Msg(eDLL_T::ENGINE, "platform_user_id: %s\n", platform_user_id->GetString());
	if (name_cvar)
		Msg(eDLL_T::ENGINE, "name: %s\n", name_cvar->GetString());
	Msg(eDLL_T::ENGINE, "Origin disabled: %s\n", IsOriginDisabled() ? "yes (-noorigin/-offline)" : "no (live Origin poll)");
}

static ConCommand origin_info("origin_info", OriginInfo_f,
	"Shows Origin/offline Nucleus id + persona (identity source for the bridge)", FCVAR_RELEASE);

//------------------------------------------------------------------------------
// Purpose: normalize connect address to "[ip]:port" (CNetAdr::ToString is unusable).
//------------------------------------------------------------------------------
static bool NormalizeAuthServerAddress(const char* const netAdr, char* const outBuf, const size_t outBufLen,
    char* const reasonBuf, const size_t reasonBufLen)
{
#define FORMAT_ERROR_REASON(fmt, ...) V_snprintf(reasonBuf, reasonBufLen, fmt, ##__VA_ARGS__);
    if (!netAdr || !netAdr[0])
    {
        FORMAT_ERROR_REASON("Could not parse server address: empty");
        return false;
    }

    CNetAdr adr;
    if (!adr.SetFromString(netAdr, true))
    {
        FORMAT_ERROR_REASON("Could not parse server address: '%s'", netAdr);
        return false;
    }

    const in6_addr* const pIP = adr.GetIP();
    char ipStr[INET6_ADDRSTRLEN];

    if (IN6_IS_ADDR_V4MAPPED(pIP))
    {
        // Dotted quad; session claim must match spire's Format("[%s]:%i", ...)
        V_snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u",
            (unsigned)pIP->s6_addr[12], (unsigned)pIP->s6_addr[13],
            (unsigned)pIP->s6_addr[14], (unsigned)pIP->s6_addr[15]);
    }
    else if (IN6_ADDR_EQUAL(pIP, &in6addr_loopback))
    {
        V_strncpy(ipStr, "127.0.0.1", sizeof(ipStr));
    }
    else if (!inet_ntop(AF_INET6, pIP, ipStr, sizeof(ipStr)))
    {
        FORMAT_ERROR_REASON("Could not format server IP from address: '%s'", netAdr);
        return false;
    }

    int port = (int)ntohs(adr.GetPort());
    if (port == 0)
        port = PORT_SERVER;

    // Bracketed so the master-server session claim matches the dedi host IP form.
    V_snprintf(outBuf, outBufLen, "[%s]:%i", ipStr, port);
    return true;
#undef FORMAT_ERROR_REASON
}

// A connect issued before platform identity arrived is held here rather than
// refused, and started from the frame loop once it lands.
static char s_pendingConnectAdr[128];
static double s_pendingConnectDeadline = 0.0;
static char s_dispatchedConnectAdr[128];
static double s_dispatchedConnectTime = 0.0;

static const float kConnectIdentityWaitSeconds = 60.0f;
// Same-host +connect is re-issued on each level-load +arg pass.
static const float kConnectDispatchDedupeSeconds = 180.0f;

//------------------------------------------------------------------------------
// Purpose: hold a connect until Origin identity (and token, if required) is ready
//------------------------------------------------------------------------------
void Bridge_ParkConnect(const char* host)
{
    const char* const adr = (host && host[0]) ? host : "localhost";
    const bool already = (s_pendingConnectAdr[0] != '\0'
        && !V_stricmp(s_pendingConnectAdr, adr));

    V_strncpy(s_pendingConnectAdr, adr, sizeof(s_pendingConnectAdr));
    s_pendingConnectDeadline = Plat_FloatTime() + kConnectIdentityWaitSeconds;

    if (!already)
    {
        Msg(eDLL_T::ENGINE,
            "[JOIN-AUTH] holding connect to '%s' until Origin identity is ready\n",
            adr);
    }
}

bool Bridge_ShouldSuppressConnect(const char* host)
{
    if (!host || !host[0])
        return false;

    if (s_pendingConnectAdr[0] && !V_stricmp(s_pendingConnectAdr, host))
        return true;

    if (s_dispatchedConnectAdr[0] && !V_stricmp(s_dispatchedConnectAdr, host)
        && (Plat_FloatTime() - s_dispatchedConnectTime) < kConnectDispatchDedupeSeconds)
        return true;

    return false;
}

void Bridge_NoteConnectDispatched(const char* host)
{
    V_strncpy(s_dispatchedConnectAdr, (host && host[0]) ? host : "", sizeof(s_dispatchedConnectAdr));
    s_dispatchedConnectTime = Plat_FloatTime();
}

void Bridge_NotifyConnectSessionEnded(void)
{
    s_dispatchedConnectAdr[0] = '\0';
    s_dispatchedConnectTime = 0.0;
}

const char* Bridge_LastConnectHost(void)
{
    return s_dispatchedConnectAdr;
}

//------------------------------------------------------------------------------
// Purpose: start a connect that was held back waiting for platform identity
//------------------------------------------------------------------------------
void Bridge_TickPendingConnect(void)
{
    if (!s_pendingConnectAdr[0])
        return;

    if (!EbisuSDK_IsConnectIdentityReady())
    {
        if (Plat_FloatTime() < s_pendingConnectDeadline)
            return;

        Warning(eDLL_T::ENGINE, "[AUTH] Origin identity never arrived; connect to '%s' dropped\n",
            s_pendingConnectAdr);

        s_pendingConnectAdr[0] = '\0';
        return;
    }

    if (!Bridge_IsTrueLoopbackHost(s_pendingConnectAdr)
        && EbisuSDK_IsPlatformIdentityExpected()
        && !EbisuSDK_GetPlatformToken()[0])
    {
        if (Plat_FloatTime() < s_pendingConnectDeadline)
            return;

        Warning(eDLL_T::ENGINE, "[AUTH] platform token never arrived; connect to '%s' dropped\n",
            s_pendingConnectAdr);

        s_pendingConnectAdr[0] = '\0';
        return;
    }

    char held[128];
    V_strncpy(held, s_pendingConnectAdr, sizeof(held));

    char buf[192];
    V_snprintf(buf, sizeof(buf), "connect \"%s\"", held);

    // Cleared before dispatch: the command runs straight back through the auth
    // path, and by now identity is ready, so nothing can re-enter this.
    s_pendingConnectAdr[0] = '\0';

    Msg(eDLL_T::ENGINE, "[JOIN-AUTH] Origin identity ready; starting held connect to '%s'\n", held);

    Cbuf_AddText(ECommandTarget_t::CBUF_FIRST_PLAYER, buf, cmd_source_t::kCommandSrcCode);
}

//------------------------------------------------------------------------------
// Purpose: get authentication token for current connection context
// Input: *connectParams -
// *reasonBuf -
// reasonBufLen -
// Output: true on success, false otherwise
//------------------------------------------------------------------------------
static bool AuthFetchAndInstallToken(const char* const netAdrStr, char* const reasonBuf, const size_t reasonBufLen)
{
#define FORMAT_ERROR_REASON(fmt, ...) V_snprintf(reasonBuf, reasonBufLen, fmt, ##__VA_ARGS__);

    // Fetch master-server JWT for this connection and split it into the three
    // cl_onlineAuthToken* userinfo ConVars the dedi reconstructs and verifies.
    if (!g_NucleusID || *g_NucleusID == 0)
    {
        FORMAT_ERROR_REASON("Origin authentication failed: no Nucleus id available yet");
        return false;
    }

    if (platform_user_id)
        PlatformUserId_SetFromPlatform(Format("%llu", *g_NucleusID).c_str());

    // Always start empty so a failed attempt never leaves a partial token installed.
    cl_onlineAuthToken.SetValue("");
    cl_onlineAuthTokenSignature1.SetValue("");
    cl_onlineAuthTokenSignature2.SetValue("");

    string msToken;
    string message;
    const char* const personaName = (g_PersonaName && g_PersonaName[0]) ? g_PersonaName : "";

    char sessionAdr[128];
    if (!NormalizeAuthServerAddress(netAdrStr, sessionAdr, sizeof(sessionAdr), reasonBuf, reasonBufLen))
        return false;

    if (spire_showdebuginfo.GetBool())
        Msg(eDLL_T::ENGINE, "AuthForConnection server address: %s (from '%s')\n", sessionAdr, netAdrStr);

    // Only the upstream verifier consults this, and only when configured to; the
    // platform token below is what proves identity. Left uncleared: the platform's
    // own token exchange needs the code to still be present.
    const char* const authCode =
        (g_OriginAuthCode && g_OriginAuthCode[0]) ? g_OriginAuthCode : "";

    // Whether a server actually requires this is the server's decision, so an
    // empty token still goes out when none was expected.
    const char* const platformToken = EbisuSDK_GetPlatformToken();

    if (!platformToken[0] && EbisuSDK_IsPlatformIdentityExpected())
    {
        // Hold rather than wait: this thread completes the platform request,
        // so waiting freezes the game and stalls the token at the same time.
        V_strncpy(s_pendingConnectAdr, netAdrStr, sizeof(s_pendingConnectAdr));
        s_pendingConnectDeadline = Plat_FloatTime() + EbisuSDK_PlatformIdentityWaitSeconds();

        FORMAT_ERROR_REASON("Signing in; the connection will start on its own");
        return false;
    }

    const bool ret = g_Spire.AuthForConnection(
        *g_NucleusID, sessionAdr, authCode, platformToken, msToken, message, personaName);

    // Do not clear: the platform token exchange still needs the code present.

    if (!ret)
    {
        if (!message.empty())
        {
            FORMAT_ERROR_REASON("%s", message.c_str());
        }
        else
        {
            FORMAT_ERROR_REASON("Master server authentication request failed");
        }
        return false;
    }

    if (msToken.empty())
    {
        FORMAT_ERROR_REASON("Master server returned no authentication token");
        return false;
    }

    // Split at last '.' so reconstruction is "%s.%s%s" (header.payload + sig1 + sig2).
    const size_t lastDot = msToken.rfind('.');
    if (lastDot == string::npos || lastDot == 0)
    {
        FORMAT_ERROR_REASON("Malformed authentication token: missing signature delimiter");
        return false;
    }

    const string headerPayload = msToken.substr(0, lastDot);
    const string signature = msToken.substr(lastDot + 1);

    if (signature.empty())
    {
        FORMAT_ERROR_REASON("Malformed authentication token: empty signature");
        return false;
    }

    // Userinfo cap is 255 per ConVar; two signature slots -> 510 max.
    if (signature.length() > 510)
    {
        FORMAT_ERROR_REASON("Authentication token signature too long to encode in userinfo (%zu > 510)", signature.length());
        return false;
    }

    cl_onlineAuthToken.SetValue(headerPayload.c_str());

    if (signature.length() <= 255)
    {
        cl_onlineAuthTokenSignature1.SetValue(signature.c_str());
    }
    else
    {
        cl_onlineAuthTokenSignature1.SetValue(signature.substr(0, 255).c_str());
        cl_onlineAuthTokenSignature2.SetValue(signature.substr(255).c_str());
    }

    return true;
#undef FORMAT_ERROR_REASON
}

bool CClientState::Authenticate(connectparams_t* connectParams, char* const reasonBuf, const size_t reasonBufLen) const
{
    return AuthFetchAndInstallToken(connectParams->netAdr, reasonBuf, reasonBufLen);
}

bool IsLocalHost(connectparams_t* connectParams)
{
    if (Bridge_IsTrueLoopbackHost(connectParams->netAdr))
        return true;

    // RFC1918 / link-local on the host base only (port and brackets stripped).
    char base[128];
    Bridge_HostBase(connectParams->netAdr, base, sizeof(base));

    if (strstr(base, "192.168.") == base)
        return true;

    if (strstr(base, "10.") == base)
        return true;

    if (strncmp(base, "172.", 4) == 0) {
        const char* secondOctet = base + 4;
        int octet = atoi(secondOctet);
        if (octet >= 16 && octet <= 31)
            return true;
    }

    if (strstr(base, "169.254.") == base)
        return true;

    return false;
}

//------------------------------------------------------------------------------
// Purpose: install the join token for an imminent bridge connect
//------------------------------------------------------------------------------
bool Bridge_InstallOnlineAuthToken(const char* const netAdrStr, char* const reasonBuf, const size_t reasonBufLen)
{
    // VClientState is dedi-only; the token must be in the ConVars before C2S_CONNECT.
    if (!cl_onlineAuthEnable.GetBool())
        return true;

    connectparams_t params{};
    params.netAdr = netAdrStr;

    if (IsLocalHost(&params) && !cl_onlineAuthForceLocal.GetBool())
        return true;

    return AuthFetchAndInstallToken(netAdrStr, reasonBuf, reasonBufLen);
}

// True while a connect is parked waiting on platform identity; the frame loop
// re-dispatches it, so callers must not treat the failed install as a refusal.
bool Bridge_IsJoinAuthDeferred(void)
{
    return s_pendingConnectAdr[0] != '\0';
}

void CClientState::VConnect(CClientState* thisptr, connectparams_t* connectParams)
{
    // Identity is Origin-sourced by boot. cl_onlineAuthEnable gates this check;
    // IsLocalHost skips it unless force-local is set.
    if (cl_onlineAuthEnable.GetBool() && (!IsLocalHost(connectParams) || cl_onlineAuthForceLocal.GetBool()))
    {
        char authFailReason[512];

        if (!thisptr->Authenticate(connectParams, authFailReason, sizeof(authFailReason)))
        {
            COM_ExplainDisconnection(true, "Failed to authenticate for online play: %s", authFailReason);
            return;
        }
    }

    CClientState__Connect(thisptr, connectParams);
}

//------------------------------------------------------------------------------
// Purpose: reconnects to currently connected server
//------------------------------------------------------------------------------
void CClientState::Reconnect()
{
    if (!IsConnected())
    {
        Warning(eDLL_T::CLIENT, "Attempted to reconnect while unconnected, please defer it.\n");
        return;
    }

    const netadr_t& remoteAdr = m_NetChannel->GetRemoteAddress();

    // NOTE: technically the engine supports running "connect localhost" to
    // reconnect to the listen server without killing it, however when running
    // this, an engine error occurs "Couldn't create a world static vertex buffer\n"
    // Needs investigation.
    if (remoteAdr.IsLoopback() || NET_IsRemoteLocal(remoteAdr))
    {
        Warning(eDLL_T::CLIENT, "Reconnecting to a listen server isn't supported, use \"reload\" instead.\n");
        return;
    }

    char buf[1024];
    V_snprintf(buf, sizeof(buf), "connect \"%s\"", remoteAdr.ToString());

    Cbuf_AddText(ECommandTarget_t::CBUF_FIRST_PLAYER, buf, cmd_source_t::kCommandSrcCode);
}

//---------------------------------------------------------------------------------
// Purpose: registers net messages
// Input: *chan
//---------------------------------------------------------------------------------
void CClientState::RegisterNetMsgs(CNetChan* chan)
{
    REGISTER_SVC_MSG(SetClassVar);
    REGISTER_SVC_MSG(SystemSayText);
    REGISTER_NET_MSG(ScriptMessage);
}


static bool (*v_S21ProcessStringCmd)(void* pCl, void* pMsg) = nullptr;

// Cbuf splits on unquoted ';'; argv0 is CCommand::Tokenize ("{}()':").
static void S21_CbufNextCommand(const char* pText, ssize_t nMaxLen,
	ssize_t* pCommandLength, ssize_t* pNextCommandOffset)
{
	ssize_t nCommandLength = 0;
	ssize_t nNext = 0;
	bool bQuoted = false;
	bool bCommented = false;

	for (; nNext < nMaxLen; ++nNext, nCommandLength += bCommented ? 0 : 1)
	{
		const char c = pText[nNext];
		if (!bCommented)
		{
			if (c == '"')
			{
				bQuoted = !bQuoted;
				continue;
			}
			if (!bQuoted && c == '/')
			{
				bCommented = (nNext < nMaxLen - 1) && pText[nNext + 1] == '/';
				if (bCommented)
				{
					++nNext;
					continue;
				}
			}
			if (!bQuoted && c == ';')
				break;
		}
		if (c == '\n')
			break;
	}

	*pCommandLength = nCommandLength;
	*pNextCommandOffset = nNext;
}

static bool S21_StringCmdNameDenied(const char* pszName)
{
	static const char* const kDeny[] = {
		"script", "script_client", "script_ui",
		"bind", "unbind", "unbindall",
		"exec", "alias", "quit",
		"_setClassVarClient",
		"rcon", "cl_rcon_address", "cl_rcon_inputonly",
		"platform_user_id",
		"connect",
		"pak_requestload", "pak_requestswap", "pak_requestunload",
		"sdk_splitpacket_recv_clamp",
		"language", "bridge_ui_language", "localize_ui_reset",
		"localize_disk", "localize_disk_strict", "localize_retire_poison",
		"bridge_pose_param_ext", "bridge_pose_moveyaw",
	};

	if (!pszName || !pszName[0])
		return false;

	for (size_t i = 0; i < ARRAYSIZE(kDeny); ++i)
	{
		if (V_stricmp(pszName, kDeny[i]) == 0)
			return true;
	}
	return false;
}

static bool S21_StringCmdDenied(const char* pszCmd)
{
	if (!pszCmd)
		return false;

	const char* p = pszCmd;
	ssize_t nLen = static_cast<ssize_t>(V_strlen(pszCmd));
	int safety = 0;

	for (; nLen > 0 && safety++ < 4096; )
	{
		ssize_t nCommandLength = 0;
		ssize_t nOffset = 0;
		S21_CbufNextCommand(p, nLen, &nCommandLength, &nOffset);

		if (nCommandLength > 0)
		{
			if (nCommandLength >= CCommand::MaxCommandLength())
				return true;

			char szCmd[512];
			memcpy(szCmd, p, static_cast<size_t>(nCommandLength));
			szCmd[nCommandLength] = '\0';

			CCommand args;
			if (!args.Tokenize(szCmd))
				return true;
			if (args.ArgC() > 0 && S21_StringCmdNameDenied(args[0]))
				return true;
		}

		const ssize_t nStep = nOffset + 1;
		if (nStep > nLen)
			break;
		p += nStep;
		nLen -= nStep;
	}
	return false;
}

static bool Hook_S21ProcessStringCmd(void* pCl, void* pMsg)
{
	const char* pszCmd = nullptr;
	if (pMsg)
		pszCmd = *reinterpret_cast<const char**>(static_cast<char*>(pMsg) + 0x20);
	if (S21_StringCmdDenied(pszCmd))
	{
		Warning(eDLL_T::CLIENT, "[SEC][STRCMD] drop S2C '%s'\n", pszCmd);
		return true;
	}
	return v_S21ProcessStringCmd(pCl, pMsg);
}

void VClientStringCmdRestrict::GetAdr(void) const
{
	LogFunAdr("CClientState::ProcessStringCmd", v_S21ProcessStringCmd);
}

void VClientStringCmdRestrict::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"40 53 48 81 EC 30 06 00 00 80 3D ?? ?? ?? ?? 00 48 8B DA")
		.GetPtr(v_S21ProcessStringCmd);
	if (!v_S21ProcessStringCmd)
		Warning(eDLL_T::CLIENT, "[SEC][STRCMD] ProcessStringCmd pattern unresolved\n");
}

void VClientStringCmdRestrict::Detour(const bool bAttach) const
{
	if (v_S21ProcessStringCmd)
		DetourSetup(&v_S21ProcessStringCmd, &Hook_S21ProcessStringCmd, bAttach);
}

/////////////////////////////////////////////////////////////////////////////////
CClientState* g_pClientState = nullptr;
CClientState** g_pClientState_Shifted = nullptr;
