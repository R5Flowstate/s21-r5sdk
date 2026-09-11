//=============================================================================//
//
// Purpose
//
// $NoKeywords: $
//
//=============================================================================//
// server.cpp: implementation of the CServer class.
//
/////////////////////////////////////////////////////////////////////////////////
#include "core/stdafx.h"
#include "common/protocol.h"
#include "tier0/frametask.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "engine/server/sv_main.h"
#include "engine/server/server.h"
#include "engine/server/connect_password_gate.h"
#include "engine/shared/s21_bridge_compat.h"
#include "networksystem/spire.h"
#include "networksystem/bansystem.h"
#include "game/shared/scriptnetdata_limits.h"
#include "ebisusdk/EbisuSDK.h"
#include "public/edict.h"
#include "pluginsystem/pluginsystem.h"
#include "game/server/gameinterface.h"
#include "engine/server/datablock_oversized.h"
#include "filesystem/filesystem.h"
#include "tier0/platform.h"
#include <algorithm>
#include <locale>
#include <codecvt>
#include <thread>

//---------------------------------------------------------------------------------
// Console variables
//---------------------------------------------------------------------------------
ConVar sv_showconnecting("sv_showconnecting", "1", FCVAR_RELEASE, "Logs information about the connecting client to the console");

ConVar sv_globalBanlist("sv_globalBanlist", "1", FCVAR_RELEASE, "Determines whether or not to use the global banned list.", false, 0.f, false, 0.f, "0 = Disable, 1 = Enable.");
ConVar sv_banlistRefreshRate("sv_banlistRefreshRate", "30.0", FCVAR_DEVELOPMENTONLY, "Banned list refresh rate (seconds).", true, 1.f, false, 0.f);

static ConVar sv_validatePersonaName("sv_validatePersonaName", "1", FCVAR_RELEASE, "Validate the client's textual persona name on connect.");
static ConVar sv_minPersonaNameLength("sv_minPersonaNameLength", "1", FCVAR_RELEASE, "The minimum length of the client's textual persona name.", true, 0.f, false, 0.f);
static ConVar sv_maxPersonaNameLength("sv_maxPersonaNameLength", "32", FCVAR_RELEASE, "The maximum length of the client's textual persona name.", true, 0.f, false, 0.f);
static ConVar sv_allowIconsInNames("sv_allowIconsInNames", "0", FCVAR_RELEASE, "Allow game icon characters in player names. 0 = Block icons, 1 = Allow icons");
static ConVar sv_nameFilterEnabled("sv_nameFilterEnabled", "1", FCVAR_RELEASE, "Kick players whose names contain words from the bad word list asset");
static ConVar sv_nameFilterPath("sv_nameFilterPath", "chatfilters/badwords.txt", FCVAR_RELEASE, "Relative path (in VPK) to the bad word list file");

//---------------------------------------------------------------------------------
// Purpose: load bad word list from VPK (englishserver_mp_common / englishclient_mp_common)
//---------------------------------------------------------------------------------
static CUtlVector<string> g_NameFilterWords;
static bool g_NameFilterLoaded = false;

static volatile LONG s_nBanChecksInFlight = 0;
constexpr LONG kMaxBanChecksInFlight = 8;
constexpr int kBanPendCap = 16;

struct BanPend_t
{
	int nSlot;
	PlatformUserId_t nUserID;
	int nPort;
	double flQueued;
	string svIPAddr;
	string svPersonaName;
};

static BanPend_t s_banPend[kBanPendCap];
static int s_nBanPend = 0;
static SRWLOCK s_banPendLock = SRWLOCK_INIT;
static volatile LONG s_nBanBusyReject = 0;
static volatile LONG s_nBanPendStale = 0;

static void SV_BanCheckInFlightTrampoline(CClient* const pClient, const string& svIPAddr,
	const PlatformUserId_t nUserID, const string& svPersonaName, const int nPort);
static void SV_BanPend_DrainOne(void);

static void SV_BanCheckSpawn(CClient* const pClient, const string& svIPAddr,
	const PlatformUserId_t nUserID, const string& svPersonaName, const int nPort)
{
	std::thread th(SV_BanCheckInFlightTrampoline, pClient, svIPAddr, nUserID, svPersonaName, nPort);
	th.detach();
}

static bool SV_BanPend_Push(const int nSlot, const string& svIPAddr,
	const PlatformUserId_t nUserID, const string& svPersonaName, const int nPort)
{
	AcquireSRWLockExclusive(&s_banPendLock);
	if (s_nBanPend >= kBanPendCap)
	{
		ReleaseSRWLockExclusive(&s_banPendLock);
		return false;
	}
	BanPend_t& e = s_banPend[s_nBanPend++];
	e.nSlot = nSlot;
	e.nUserID = nUserID;
	e.nPort = nPort;
	e.flQueued = Plat_FloatTime();
	e.svIPAddr = svIPAddr;
	e.svPersonaName = svPersonaName;
	ReleaseSRWLockExclusive(&s_banPendLock);
	return true;
}

static bool SV_BanPend_Pop(BanPend_t& out)
{
	AcquireSRWLockExclusive(&s_banPendLock);
	if (s_nBanPend <= 0)
	{
		ReleaseSRWLockExclusive(&s_banPendLock);
		return false;
	}
	out = s_banPend[0];
	for (int i = 1; i < s_nBanPend; ++i)
		s_banPend[i - 1] = std::move(s_banPend[i]);
	--s_nBanPend;
	ReleaseSRWLockExclusive(&s_banPendLock);
	return true;
}

static void SV_BanPend_DrainOne(void)
{
	BanPend_t e;
	if (!SV_BanPend_Pop(e))
		return;

	if ((Plat_FloatTime() - e.flQueued) > 30.0)
	{
		InterlockedIncrement(&s_nBanPendStale);
		return;
	}
	if (!g_pServer || e.nSlot < 0 || e.nSlot >= MAX_PLAYERS)
	{
		InterlockedIncrement(&s_nBanPendStale);
		return;
	}

	CClient* const pClient = g_pServer->GetClient(e.nSlot);
	if (!pClient || pClient->GetPlatformUserId() != e.nUserID)
	{
		InterlockedIncrement(&s_nBanPendStale);
		return;
	}

	if (InterlockedIncrement(&s_nBanChecksInFlight) > kMaxBanChecksInFlight)
	{
		InterlockedDecrement(&s_nBanChecksInFlight);
		if (!SV_BanPend_Push(e.nSlot, e.svIPAddr, e.nUserID, e.svPersonaName, e.nPort))
		{
			if (InterlockedIncrement(&s_nBanBusyReject) <= 8)
				Warning(eDLL_T::SERVER, "[BAN] busy-reject slot=%d uid=%llu\n",
					e.nSlot, static_cast<unsigned long long>(e.nUserID));
			pClient->Disconnect(REP_MARK_BAD, "#Valve_Reject_Banned");
		}
		return;
	}

	SV_BanCheckSpawn(pClient, e.svIPAddr, e.nUserID, e.svPersonaName, e.nPort);
}

static void SV_BanCheckInFlightTrampoline(CClient* const pClient, const string& svIPAddr,
	const PlatformUserId_t nUserID, const string& svPersonaName, const int nPort)
{
	SV_CheckForBanAndDisconnect(pClient, svIPAddr, nUserID, svPersonaName, nPort);
	InterlockedDecrement(&s_nBanChecksInFlight);
	SV_BanPend_DrainOne();
}

// Unicode-aware case conversion for better international character support
static std::string SV_ToLowerUnicode(const std::string& input)
{
    std::string result = input;
    
    // First handle ASCII characters with standard tolower
    std::transform(result.begin(), result.end(), result.begin(), 
        [](unsigned char c) { 
            return (c <= 127) ? (char)tolower(c) : (char)c; 
        });
    
    // For better Unicode support, we could add more sophisticated conversion here
    // This basic version handles ASCII properly and leaves Unicode characters unchanged
    // which is safer than corrupting them with ASCII-only tolower
    
    return result;
}

// Function to check if a name contains blocked game icon characters
static bool SV_NameContainsBlockedIcons(const char* name)
{
	if (!name)
		return false;

	const unsigned char* p = reinterpret_cast<const unsigned char*>(name);
	int safety = 0;

	while (*p && safety++ < 2048)
	{
		if (p[0] == 0xF3 && p[1] == 0xB0)
			return true;
		if (p[0] == 0xEF && p[1] >= 0x80 && p[1] <= 0xBF)
			return true;

		const unsigned char c = p[0];
		if (c <= 0x7F)
		{
			++p;
			continue;
		}

		if (c <= 0xBF || c == 0xC0 || c == 0xC1 || c >= 0xF5)
		{
			++p;
			continue;
		}

		if (c <= 0xDF)
		{
			if (!p[1] || (p[1] & 0xC0) != 0x80)
			{
				++p;
				continue;
			}
			p += 2;
			continue;
		}

		if (c <= 0xEF)
		{
			unsigned char lo = 0x80;
			unsigned char hi = 0xBF;
			if (c == 0xE0)
				lo = 0xA0;
			else if (c == 0xED)
				hi = 0x9F;
			if (!p[1] || !p[2]
				|| p[1] < lo || p[1] > hi
				|| (p[2] & 0xC0) != 0x80)
			{
				++p;
				continue;
			}
			p += 3;
			continue;
		}

		{
			unsigned char lo = 0x80;
			unsigned char hi = 0xBF;
			if (c == 0xF0)
				lo = 0x90;
			else if (c == 0xF4)
				hi = 0x8F;
			if (!p[1] || !p[2] || !p[3]
				|| p[1] < lo || p[1] > hi
				|| (p[2] & 0xC0) != 0x80
				|| (p[3] & 0xC0) != 0x80)
			{
				++p;
				continue;
			}
			p += 4;
		}
	}

	return false;
}

static void SV_LoadNameFilter()
{
    g_NameFilterWords.RemoveAll();
    g_NameFilterLoaded = false;

    const char* const filePath = sv_nameFilterPath.GetString();
    if (!filePath || !*filePath)
        return;

    FileHandle_t hFile = FileSystem()->Open(filePath, "rb", "GAME");
    if (hFile == FILESYSTEM_INVALID_HANDLE)
        return;

    const ssize_t nFileSize = FileSystem()->Size(hFile);
    if (nFileSize <= 0)
    {
        FileSystem()->Close(hFile);
        return;
    }

    const u64 nBufSize = FileSystem()->GetOptimalReadSize(hFile, nFileSize + 2);
    char* const pBuf = (char*)FileSystem()->AllocOptimalReadBuffer(hFile, nBufSize, 0);
    if (!pBuf)
    {
        FileSystem()->Close(hFile);
        return;
    }

    const ssize_t nRead = FileSystem()->ReadEx(pBuf, nBufSize, nFileSize, hFile);
    FileSystem()->Close(hFile);
    if (nRead <= 0)
    {
        FileSystem()->FreeOptimalReadBuffer(pBuf);
        return;
    }

    pBuf[nRead] = '\0';

    // Parse lines
    const char* p = pBuf;
    while (*p)
    {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;

        const char* start = p;
        while (*p && *p != '\r' && *p != '\n') ++p;
        string word(start, p - start);

        while (!word.empty() && (word.back() == ' ' || word.back() == '\t')) word.pop_back();
        if (!word.empty())
        {
            // Use Unicode-aware case conversion
            std::string lowerWord = SV_ToLowerUnicode(word);
            g_NameFilterWords.AddToTail(lowerWord);
        }
    }

    FileSystem()->FreeOptimalReadBuffer(pBuf);
    g_NameFilterLoaded = true;
}

static bool SV_NameContainsBadWord(const char* pszName)
{
    if (!sv_nameFilterEnabled.GetBool() || !pszName || !*pszName)
        return false;

    if (!g_NameFilterLoaded)
        SV_LoadNameFilter();

    if (!g_NameFilterLoaded || g_NameFilterWords.IsEmpty())
        return false;

    std::string lowered = SV_ToLowerUnicode(std::string(pszName));

    FOR_EACH_VEC(g_NameFilterWords, i)
    {
        if (lowered.find(g_NameFilterWords[i]) != string::npos)
            return true;
    }
    return false;
}

//---------------------------------------------------------------------------------
// Purpose: Gets the number of human players on the server
// Output: int
//---------------------------------------------------------------------------------
int CServer::GetNumHumanPlayers(void) const
{
	int nHumans = 0;
	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		const CClient* const pClient = g_pServer->GetClient(i);

		if (pClient->IsHumanPlayer())
			nHumans++;
	}

	return nHumans;
}

//---------------------------------------------------------------------------------
// Purpose: Gets the number of fake clients on the server
// Output: int
//---------------------------------------------------------------------------------
int CServer::GetNumFakeClients(void) const
{
	int nBots = 0;
	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		const CClient* const pClient = g_pServer->GetClient(i);

		if (pClient->IsConnected() && pClient->IsFakeClient())
			nBots++;
	}

	return nBots;
}

//---------------------------------------------------------------------------------
// Purpose: Gets the number of clients on the server
// Output: int
//---------------------------------------------------------------------------------
int CServer::GetNumClients(void) const
{
	int nClients = 0;
	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		const CClient* const pClient = g_pServer->GetClient(i);

		if (pClient->IsConnected())
			nClients++;
	}

	return nClients;
}

//---------------------------------------------------------------------------------
// Purpose: Rejects connection request and sends back a message
// Input: iSocket - 
// *pChallenge - 
// *szMessage - 
//---------------------------------------------------------------------------------
void CServer::RejectConnection(int iSocket, netadr_t* pNetAdr, const char* szMessage)
{
	CServer__RejectConnection(this, iSocket, pNetAdr, szMessage);
}

//---------------------------------------------------------------------------------
// Purpose: Initializes a CSVClient for a new net connection. This will only be called
// once for a player each game, not once for each level change.
// Input: *pServer - 
// *pChallenge - 
// Output: pointer to client instance on success, nullptr on failure
//---------------------------------------------------------------------------------
CClient* CServer::ConnectClient(CServer* pServer, user_creds_s* pChallenge)
{
	// Challenge-bind the password tag before any validation reads it.
	ConnectPasswordGate_FilterTag(pChallenge);

	if (g_bS21BridgeVerbose)
	{
		Msg(eDLL_T::SERVER, "S21 Bridge: ConnectClient called! state=%d, personaName=%p, personaId=%llu\n",
			(int)pServer->m_State, pChallenge->personaName, pChallenge->personaId);
	}

	if (pServer->m_State < server_state_t::ss_active)
	{
		if (g_bS21BridgeVerbose)
		{
			Msg(eDLL_T::SERVER, "S21 Bridge: REJECTED - server state %d < ss_active (%d)\n",
				(int)pServer->m_State, (int)server_state_t::ss_active);
		}
		return nullptr;
	}

	char* pszPersonaName = pChallenge->personaName;
	PlatformUserId_t nUserID = pChallenge->personaId;

	const bool bEnableLogging = sv_showconnecting.GetBool();
	const int nPort = int(ntohs(pChallenge->netAdr.GetPort()));

	char szAddresBuffer[128];
	const char* pszAddresBuffer = nullptr;

	if (bEnableLogging)
	{
		// Render the client address once.
		pChallenge->netAdr.ToString(szAddresBuffer, sizeof(szAddresBuffer), true);
		pszAddresBuffer = szAddresBuffer;

		Msg(eDLL_T::SERVER, "Processing connectionless challenge for '[%s]:%i' ('%llu')\n",
			pszAddresBuffer, nPort, nUserID);
	}

	// Reject if persona name contains a filtered word from asset (VPK)
	if (SV_NameContainsBadWord(pszPersonaName))
	{
		//#Client_Reject_Banned_Name: Your name contains a banned word.
		pServer->RejectConnection(pServer->m_Socket, &pChallenge->netAdr, "#Client_Reject_Banned_Name");
		if (bEnableLogging)
		{
			if (!pszAddresBuffer)
			{
				pChallenge->netAdr.ToString(szAddresBuffer, sizeof(szAddresBuffer), true);
				pszAddresBuffer = szAddresBuffer;
			}
			Warning(eDLL_T::SERVER, "Connection rejected for '[%s]:%i' ('%llu' name contains banned word)\n",
				pszAddresBuffer, nPort, nUserID);
		}
		return nullptr;
	}

	// Reject if persona name contains blocked game icon characters (unless allowed)
	if (!sv_allowIconsInNames.GetBool() && SV_NameContainsBlockedIcons(pszPersonaName))
	{
		//#Client_Reject_Invalid_Name: Your name contains invalid characters.
		pServer->RejectConnection(pServer->m_Socket, &pChallenge->netAdr, "#Client_Reject_Invalid_Name");
		if (bEnableLogging)
		{
			if (!pszAddresBuffer)
			{
				pChallenge->netAdr.ToString(szAddresBuffer, sizeof(szAddresBuffer), true);
				pszAddresBuffer = szAddresBuffer;
			}
			Warning(eDLL_T::SERVER, "Connection rejected for '[%s]:%i' ('%llu' name contains blocked icon characters)\n",
				pszAddresBuffer, nPort, nUserID);
		}
		return nullptr;
	}

	bool bValidName = false;

	if (VALID_CHARSTAR(pszPersonaName) &&
		V_IsValidUTF8(pszPersonaName))
	{
		if (sv_validatePersonaName.GetBool() && 
			!IsValidPersonaName(pszPersonaName, sv_minPersonaNameLength.GetInt(), sv_maxPersonaNameLength.GetInt()))
		{
			bValidName = false;
		}
		else
		{
			bValidName = true;
		}
	}

	// Only proceed connection if the client's name is valid and UTF-8 encoded.
	if (!bValidName)
	{
		if (g_bS21BridgeVerbose)
		{
			Msg(eDLL_T::SERVER, "S21 Bridge: REJECTED - invalid name (VALID_CHARSTAR=%d, UTF8=%d)\n",
				VALID_CHARSTAR(pszPersonaName), V_IsValidUTF8(pszPersonaName));
		}
		pServer->RejectConnection(pServer->m_Socket, &pChallenge->netAdr, "#Valve_Reject_Invalid_Name");

		if (bEnableLogging)
		{
			Warning(eDLL_T::SERVER, "Connection rejected for '[%s]:%i' ('%llu' has an invalid name!)\n",
				pszAddresBuffer, nPort, nUserID);
		}

		return nullptr;
	}

	if (g_BanSystem.IsBanned(&pChallenge->netAdr, nUserID))
	{
		pServer->RejectConnection(pServer->m_Socket, &pChallenge->netAdr, "#Valve_Reject_Banned");

		if (bEnableLogging)
		{
			Warning(eDLL_T::SERVER, "Connection rejected for '[%s]:%i' ('%llu' is banned from this server!)\n",
				pszAddresBuffer, nPort, nUserID);
		}

		return nullptr;
	}

	if (g_bS21BridgeVerbose)
		Msg(eDLL_T::SERVER, "S21 Bridge: Calling engine ConnectClient...\n");
	CClient* const pClient = CServer__ConnectClient(pServer, pChallenge);
	if (g_bS21BridgeVerbose)
		Msg(eDLL_T::SERVER, "S21 Bridge: Engine ConnectClient returned %p\n", pClient);

	for (auto& callback : !PluginSystem()->GetConnectClientCallbacks())
	{
		if (!callback.Function()(pServer, pClient, pChallenge))
		{
			pClient->Disconnect(REP_MARK_BAD, "#Valve_Reject_Banned");
			return nullptr;
		}
	}

	if (pClient && sv_globalBanlist.GetBool())
	{
		if (!pClient->GetNetChan()->GetRemoteAddress().IsLoopback())
		{
			if (!pszAddresBuffer)
			{
				pChallenge->netAdr.ToString(szAddresBuffer, sizeof(szAddresBuffer), true);
				pszAddresBuffer = szAddresBuffer;
			}

			const string addressBufferCopy(pszAddresBuffer);
			const string personaNameCopy(pszPersonaName);

			if (InterlockedIncrement(&s_nBanChecksInFlight) > kMaxBanChecksInFlight)
			{
			InterlockedDecrement(&s_nBanChecksInFlight);
			// GetHandle() is the edict index, not the client slot: GetClient()
			// is slot-based (cf. GetClient(nEdict - 1)). Resolve the slot by
			// pointer scan so the drain re-resolves the same client.
			int nSlot = -1;
			for (int i = 0; i < MAX_PLAYERS; ++i)
			{
				if (pServer->GetClient(i) == pClient) { nSlot = i; break; }
			}
				if (nSlot < 0 || nSlot >= MAX_PLAYERS
					|| !SV_BanPend_Push(nSlot, addressBufferCopy, nUserID, personaNameCopy, nPort))
				{
					if (InterlockedIncrement(&s_nBanBusyReject) <= 8)
						Warning(eDLL_T::SERVER, "[BAN] busy-reject slot=%d uid=%llu\n",
							nSlot, static_cast<unsigned long long>(nUserID));
					pClient->Disconnect(REP_MARK_BAD, "#Valve_Reject_Banned");
					return nullptr;
				}
			}
			else
			{
				SV_BanCheckSpawn(pClient, addressBufferCopy, nUserID, personaNameCopy, nPort);
			}
		}
	}

	return pClient;
}

//---------------------------------------------------------------------------------
// Purpose: Sends netmessage to all active clients
// Input: *msg -
// onlyActive - 
// reliable - 
//---------------------------------------------------------------------------------
void CServer::BroadcastMessage(CNetMessage* const msg, const bool onlyActive, const bool reliable)
{
	CServer__BroadcastMessage(this, msg, onlyActive, reliable);
}

//---------------------------------------------------------------------------------
// Purpose: Runs the server frame
// Input: *pServer - 
//---------------------------------------------------------------------------------
void CServer::RunFrame(CServer* pServer)
{
	CServer__RunFrame(pServer);

	// Flush dirty SNDC extension vars to clients via NET_ScriptMessage
	SNDC_FlushDirtyVars(pServer);
	DataBlockOversized_Pump(pServer);
}

///////////////////////////////////////////////////////////////////////////////
void VServer::Detour(const bool bAttach) const
{
	DetourSetup(&CServer__RunFrame, &CServer::RunFrame, bAttach);
	DetourSetup(&CServer__ConnectClient, &CServer::ConnectClient, bAttach);
}

///////////////////////////////////////////////////////////////////////////////
CServer* g_pServer = nullptr;
CClientExtended CServer::sm_ClientsExtended[MAX_PLAYERS];
