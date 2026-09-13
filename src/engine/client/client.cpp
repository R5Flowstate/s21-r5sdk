#if defined(CLIENT_DLL)
//===============================================================================//
//
// Purpose
//
// $NoKeywords: $
//
//===============================================================================//
// client.cpp: implementation of the CClient class.
//
///////////////////////////////////////////////////////////////////////////////////
#include "core/stdafx.h"
#include "mathlib/bitvec.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "common/netmessages.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "networksystem/spire.h"
#include "game/server/gameinterface.h"

// Absolute max string cmd length, any character past this will be NULLED.
#define STRINGCMD_MAX_LEN 512

//---------------------------------------------------------------------------------
// Purpose: throw away any residual garbage in the channel
//---------------------------------------------------------------------------------
void CClient::Clear(void)
{
	CClient__Clear(this);

	// CClient::Clear does not null this; bots would reuse the last platform
	// user id because CClient instances are a static CServer array.
	m_nPlatformUserId = 0;
}

//---------------------------------------------------------------------------------
// Purpose: throw away any residual garbage in the channel
// Input: *pClient - 
//---------------------------------------------------------------------------------
void CClient::VClear(CClient* pClient)
{
	pClient->Clear();
}



static const char JWT_PUBLIC_KEY[] =
"-----BEGIN PUBLIC KEY-----\n"
"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAlJvwaN7R/2vJncExlh+M\n"
"eoiiBSsFFZutYOSxr3jWSpS2aalTUH1SXyNoelmqXoKT+csTRcahneXRKvhpOcK3\n"
"X97TnVFJ8gu7Okq1Flv58NbmDUGudJkd2/12hfvGD2vDR1nmIKPegHaitSfqGOGY\n"
"cuRlEpBcdRA8OseYR1B0TcmJwlR18MsVsErPj0p6csba4S8JmHv4PfjUyaErFmCC\n"
"57nbB6n0wQL2gPx3yKjUAE2qNFqYVPNg4zAed/X9k85BHfp/pH6zW1UVv2vkXFLq\n"
"KSuKd+yUw+0lT0vseTWsoBqFeCdYWZnTv3wAWlky1zz/M81zuORNsX72dSLK8OsR\n"
"MQIDAQAB\n"
"-----END PUBLIC KEY-----\n";

// Rotation: add the new key here, roll servers, then switch the master private
// key, then clear this. Empty means only JWT_PUBLIC_KEY is trusted.
static const char JWT_PUBLIC_KEY_ROTATING[] = "";

static ConVar sv_onlineAuthEnable("sv_onlineAuthEnable", "1", FCVAR_RELEASE, "Enables the server-side online authentication system");
// Default 1: RFC1918 clients must present a join token. Set 0 only for private-lab LAN joins.
// True loopback (127.0.0.1/::1) is always allowed without a token regardless of this lever.
static ConVar sv_onlineAuthForceLocal("sv_onlineAuthForceLocal", "1", FCVAR_RELEASE, "Require join-token verification for RFC1918/private clients (loopback always exempt)");
// 0 = off (admit all). 1 = token required. 2 = verify-if-present.
static ConVar sv_onlineAuthMode("sv_onlineAuthMode", "1", FCVAR_RELEASE,
	"Join-token policy: 0 = off, 1 = required, 2 = verify-if-present.");

static ConVar sv_onlineAuthValidateExpiry("sv_onlineAuthValidateExpiry", "1", FCVAR_RELEASE, "Validate the online authentication token 'expiry' claim");
static ConVar sv_onlineAuthValidateIssuedAt("sv_onlineAuthValidateIssuedAt", "1", FCVAR_RELEASE, "Validate the online authentication token 'issued at' claim");

static ConVar sv_onlineAuthExpiryTolerance("sv_onlineAuthExpiryTolerance", "1", FCVAR_DEVELOPMENTONLY, "The online authentication token 'expiry' claim tolerance in seconds", true, 0.f, true, float(UINT8_MAX), "Must range between [0,255]");
static ConVar sv_onlineAuthIssuedAtTolerance("sv_onlineAuthIssuedAtTolerance", "30", FCVAR_DEVELOPMENTONLY, "The online authentication token 'issued at' claim tolerance in seconds", true, 0.f, true, float(UINT8_MAX), "Must range between [0,255]");

static ConVar sv_onlineAuthReplayWindow("sv_onlineAuthReplayWindow", "60", FCVAR_RELEASE,
	"Seconds an accepted join token is remembered so the same bytes cannot be presented twice", true, 0.f, true, 300.f);
static ConVar sv_onlineAuthReplayGuard("sv_onlineAuthReplayGuard", "1", FCVAR_RELEASE,
	"Reject a join token whose exact stitched bytes were already accepted within the replay window");

static ConVar sv_quota_stringCmdsPerSecond("sv_quota_stringCmdsPerSecond", "32", FCVAR_RELEASE, "How many string commands per second clients are allowed to submit, 0 to disallow all string commands", true, 0.f, false, 0.f);


//---------------------------------------------------------------------------------
// Purpose: check whether this client is authorized to join this server
// Input: *playerName - 
// *reasonBuf - 
// reasonBufLen - 
// Output: true if authorized, false otherwise
//---------------------------------------------------------------------------------
bool CClient::Authenticate(const char* const playerName, char* const reasonBuf, const size_t reasonBufLen)
{

	return true;
}

//---------------------------------------------------------------------------------
// Purpose: connect new client
// Input: *szName - 
// *pNetChan - 
// bFakePlayer - 
// *conVars - 
// *szMessage -
// nMessageSize - 
// Output: true if connection was successful, false otherwise
//---------------------------------------------------------------------------------
bool CClient::Connect(const char* szName, CNetChan* pNetChan, bool bFakePlayer,
	CUtlVector<NET_SetConVar::cvar_t>* conVars, char* szMessage, int nMessageSize)
{

	if (!CClient__Connect(this, szName, pNetChan, bFakePlayer, conVars, szMessage, nMessageSize))
		return false;


	return true;
}

//---------------------------------------------------------------------------------
// Purpose: connect new client
// Input: *pClient - 
// *szName - 
// *pNetChan - 
// bFakePlayer - 
// *a5 - 
// *szMessage -
// nMessageSize - 
// Output: true if connection was successful, false otherwise
//---------------------------------------------------------------------------------
bool CClient::VConnect(CClient* pClient, const char* szName, CNetChan* pNetChan, bool bFakePlayer,
	CUtlVector<NET_SetConVar::cvar_t>* conVars, char* szMessage, int nMessageSize)
{
	return pClient->Connect(szName, pNetChan, bFakePlayer, conVars, szMessage, nMessageSize);
}

//---------------------------------------------------------------------------------
// Purpose: registers net messages
// Input: *pClient - 
// *pChan - 
// Output: true if setup was successful, false otherwise
//---------------------------------------------------------------------------------
bool CClient::VConnectionStart(CClient* pClient, CNetChan* pChan)
{
	pClient->RegisterNetMsgs(pChan);
	return CClient__ConnectionStart(pClient, pChan);
}

//---------------------------------------------------------------------------------
// Purpose: disconnect client
// Input: nRepLvl - 
// *szReason - 
//			... - 
//---------------------------------------------------------------------------------
void CClient::Disconnect(const Reputation_t nRepLvl, const char* szReason, ...)
{
	if (m_nSignonState != SIGNONSTATE::SIGNONSTATE_NONE)
	{
		char szBuf[1024];
		{/////////////////////////////
			va_list vArgs;
			va_start(vArgs, szReason);

			const int ret = V_vsnprintf(szBuf, sizeof(szBuf), szReason, vArgs);

			if (ret < 0)
				szBuf[0] = '\0';

			va_end(vArgs);
		}/////////////////////////////
		CClient__Disconnect(this, nRepLvl, szBuf);
	}
}

//---------------------------------------------------------------------------------
// Purpose: activate player
// Input: *pClient - 
//---------------------------------------------------------------------------------
void CClient::VActivatePlayer(CClient* pClient)
{
	// Set the client instance to 'ready' before calling ActivatePlayer.
	pClient->SetPersistenceState(PERSISTENCE::PERSISTENCE_READY);
	CClient__ActivatePlayer(pClient);

}

//---------------------------------------------------------------------------------
// Purpose: registers net messages
// Input: *chan
//---------------------------------------------------------------------------------
void CClient::RegisterNetMsgs(CNetChan* chan)
{
}

//---------------------------------------------------------------------------------
// Purpose: send a net message with replay.
// set 'CNetMessage::m_nGroup' to 'NoReplay' to disable replay.
// Input: *pMsg - 
// bLocal - 
// bForceReliable - 
// bVoice - 
//---------------------------------------------------------------------------------
bool CClient::SendNetMsgEx(CNetMessage* pMsg, bool bLocal, bool bForceReliable, bool bVoice)
{
	if (!CanReplayMessage(pMsg))
	{
		// Don't copy the message into the replay buffer.
		pMsg->m_nGroup = NetMessageGroup::NoReplay;
	}

	return CClient__SendNetMsgEx(this, pMsg, bLocal, bForceReliable, bVoice);
}

//---------------------------------------------------------------------------------
// Purpose: send a snapshot
// Input: *pClient - 
// *pFrame - 
// nTick - 
// nTickAck - 
//---------------------------------------------------------------------------------
void* CClient::VSendSnapshot(CClient* pClient, CClientFrame* pFrame, int nTick, int nTickAck)
{
	return CClient__SendSnapshot(pClient, pFrame, nTick, nTickAck);
}

//---------------------------------------------------------------------------------
// Purpose: internal hook to 'CClient::SendNetMsgEx'
// Input: *pClient - 
// *pMsg - 
// bLocal - 
// bForceReliable - 
// bVoice - 
//---------------------------------------------------------------------------------
bool CClient::VSendNetMsgEx(CClient* pClient, CNetMessage* pMsg, bool bLocal, bool bForceReliable, bool bVoice)
{
	return pClient->SendNetMsgEx(pMsg, bLocal, bForceReliable, bVoice);
}

//---------------------------------------------------------------------------------
// Purpose: write data into data blocks to send to the client
// Input: &buf
//---------------------------------------------------------------------------------
void CClient::WriteDataBlock(CClient* pClient, bf_write& buf)
{
}

//---------------------------------------------------------------------------------
// Purpose: some versions of the binary have an optimization that shifts the 'this'
// pointer of the CClient structure by 8 bytes to avoid having to cache the vftable
// pointer if it never get used. Here we shift it back so it aligns again.
//---------------------------------------------------------------------------------
CClient* AdjustShiftedThisPointer(CClient* shiftedPointer)
{
	/* ProcessStringCmd inlines ExecuteStringCommand; callers pass this+8. */
	char* pShifted = reinterpret_cast<char*>(shiftedPointer) - 8;
	return reinterpret_cast<CClient*>(pShifted);
}

//---------------------------------------------------------------------------------
// Purpose: process string commands (kicking anyone attempting to DOS)
// Input: *pClient - (ADJ)
// *pMsg - 
// Output: false if cmd should be passed to CServerGameClients
//---------------------------------------------------------------------------------
bool CClient::VProcessStringCmd(CClient* pClient, NET_StringCmd* pMsg)
{

	return CClient__ProcessStringCmd(pClient, pMsg);
}

//---------------------------------------------------------------------------------
// Purpose: process set convar
// Input: *pClient - (ADJ)
// *pMsg - 
// Output 
//---------------------------------------------------------------------------------
bool CClient::VProcessSetConVar(CClient* pClient, NET_SetConVar* pMsg)
{

	return true;
}


//---------------------------------------------------------------------------------
// Purpose: process voice data
// Input: *pClient - (ADJ)
// *pMsg - 
// Output 
//---------------------------------------------------------------------------------
bool CClient::VProcessVoiceData(CClient* pClient, CLC_VoiceData* pMsg)
{

	return true;
}

//---------------------------------------------------------------------------------
// Purpose: process durango voice data
// Input: *pClient - (ADJ)
// *pMsg - 
// Output 
//---------------------------------------------------------------------------------
bool CClient::VProcessDurangoVoiceData(CClient* pClient, CLC_DurangoVoiceData* pMsg)
{

	return true;
}

//---------------------------------------------------------------------------------
// Purpose: set UserCmd time buffer
// Input: numUserCmdProcessTicksMax - 
// tickInterval - 
//---------------------------------------------------------------------------------
void CClientExtended::InitializeMovementTimeForUserCmdProcessing(const int numUserCmdProcessTicksMax, const float tickInterval)
{
	// Grant the client some time buffer to execute user commands
	m_flMovementTimeForUserCmdProcessingRemaining += tickInterval;

	// but never accumulate more than N ticks
	if (m_flMovementTimeForUserCmdProcessingRemaining > numUserCmdProcessTicksMax * tickInterval)
		m_flMovementTimeForUserCmdProcessingRemaining = numUserCmdProcessTicksMax * tickInterval;
}

//---------------------------------------------------------------------------------
// Purpose: consume UserCmd time buffer
// Input: flTimeNeeded -
// Output: max time allowed for processing
//---------------------------------------------------------------------------------
float CClientExtended::ConsumeMovementTimeForUserCmdProcessing(const float flTimeNeeded)
{
	if (m_flMovementTimeForUserCmdProcessingRemaining <= 0.0f)
		return 0.0f;
	else if (flTimeNeeded > m_flMovementTimeForUserCmdProcessingRemaining + FLT_EPSILON)
	{
		const float flResult = m_flMovementTimeForUserCmdProcessingRemaining;
		m_flMovementTimeForUserCmdProcessingRemaining = 0.0f;

		return flResult;
	}
	else
	{
		m_flMovementTimeForUserCmdProcessingRemaining -= flTimeNeeded;

		if (m_flMovementTimeForUserCmdProcessingRemaining < 0.0f)
			m_flMovementTimeForUserCmdProcessingRemaining = 0.0f;

		return flTimeNeeded;
	}
}

void VClient::Detour(const bool bAttach) const
{
}
#else // !CLIENT_DLL
//===============================================================================//
//
// Purpose
//
// $NoKeywords: $
//
//===============================================================================//
// client.cpp: implementation of the CClient class.
//
///////////////////////////////////////////////////////////////////////////////////
#include "core/stdafx.h"
#include "mathlib/bitvec.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "common/netmessages.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "networksystem/spire.h"
#include "networksystem/hostmanager.h"
#include "jwt/include/decode.h"
#include "mbedtls/include/mbedtls/sha256.h"
#include "game/server/recipientfilter.h"
#include "game/server/util_server.h"
#include "game/shared/scriptnetdata_limits.h"
#include "game/server/util_server.h"
#include "game/shared/usermessages.h"
#include "game/client/vscript_player.h"
#include "tier1/fmtstr.h"
// Admin elevation lane (VProcessStringCmd) -- console command tokenizer,
// Cmd_ExecuteUnrestricted, and the RCON server accessors.
#include "tier1/cmd.h"
#include "engine/cmd.h"
#include "engine/server/sv_rcon.h"
#include "game/server/gameinterface.h"
#include <cstdlib>
#include <cstring>

// Absolute max string cmd length, any character past this will be NULLED.
#define STRINGCMD_MAX_LEN 512
static uint32_t s_nBridgeStrCmdDrops = 0;
static uint32_t s_nBridgeRelSeqDrops = 0;

//---------------------------------------------------------------------------------
// Purpose: throw away any residual garbage in the channel
//---------------------------------------------------------------------------------
void CClient::Clear(void)
{
	GetClientExtended()->Reset(); // Reset extended data.
	CClient__Clear(this);

	// CClient::Clear does not null this; bots would reuse the last platform
	// user id because CClient instances are a static CServer array.
	m_nPlatformUserId = 0;
}

//---------------------------------------------------------------------------------
// Purpose: throw away any residual garbage in the channel
// Input: *pClient - 
//---------------------------------------------------------------------------------
void CClient::VClear(CClient* pClient)
{
	pClient->Clear();
}

//---------------------------------------------------------------------------------
// Purpose: gets the extended client data
// Output: CClientExtended* - 
//---------------------------------------------------------------------------------
CClientExtended* CClient::GetClientExtended(void) const
{
	return m_pServer->GetClientExtended(m_nUserID);
}

bool CClientExtended::AcceptBridgeRelSeq(uint32_t nSeq)
{
	if (nSeq == 0)
		return true;

	uint32_t& nHigh = m_nBridgeRelSeqHigh;
	uint64_t& nMask = m_nBridgeRelSeenMask;
	if (nHigh == 0 && nMask == 0)
	{
		nHigh = nSeq;
		nMask = 1;
		return true;
	}
	if (nSeq > nHigh)
	{
		const uint32_t nShift = nSeq - nHigh;
		nMask = (nShift >= 64) ? 0ull : (nMask << nShift);
		nMask |= 1ull;
		nHigh = nSeq;
		return true;
	}
	const uint32_t nDelta = nHigh - nSeq;
	if (nDelta < 64 && ((nMask >> nDelta) & 1ull) == 0)
	{
		nMask |= (1ull << nDelta);
		return true;
	}
	++s_nBridgeRelSeqDrops;
	return false;
}


static const char JWT_PUBLIC_KEY[] =
"-----BEGIN PUBLIC KEY-----\n"
"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAlJvwaN7R/2vJncExlh+M\n"
"eoiiBSsFFZutYOSxr3jWSpS2aalTUH1SXyNoelmqXoKT+csTRcahneXRKvhpOcK3\n"
"X97TnVFJ8gu7Okq1Flv58NbmDUGudJkd2/12hfvGD2vDR1nmIKPegHaitSfqGOGY\n"
"cuRlEpBcdRA8OseYR1B0TcmJwlR18MsVsErPj0p6csba4S8JmHv4PfjUyaErFmCC\n"
"57nbB6n0wQL2gPx3yKjUAE2qNFqYVPNg4zAed/X9k85BHfp/pH6zW1UVv2vkXFLq\n"
"KSuKd+yUw+0lT0vseTWsoBqFeCdYWZnTv3wAWlky1zz/M81zuORNsX72dSLK8OsR\n"
"MQIDAQAB\n"
"-----END PUBLIC KEY-----\n";

// Rotation: add the new key here, roll servers, then switch the master private
// key, then clear this. Empty means only JWT_PUBLIC_KEY is trusted.
static const char JWT_PUBLIC_KEY_ROTATING[] = "";

static ConVar sv_onlineAuthEnable("sv_onlineAuthEnable", "1", FCVAR_RELEASE, "Enables the server-side online authentication system");
// Default 1: RFC1918 clients must present a join token. Set 0 only for private-lab LAN joins.
// True loopback (127.0.0.1/::1) is always allowed without a token regardless of this lever.
static ConVar sv_onlineAuthForceLocal("sv_onlineAuthForceLocal", "1", FCVAR_RELEASE, "Require join-token verification for RFC1918/private clients (loopback always exempt)");
// 0 = off (admit all). 1 = token required. 2 = verify-if-present.
static ConVar sv_onlineAuthMode("sv_onlineAuthMode", "1", FCVAR_RELEASE,
	"Join-token policy: 0 = off, 1 = required, 2 = verify-if-present.");

static ConVar sv_onlineAuthValidateExpiry("sv_onlineAuthValidateExpiry", "1", FCVAR_RELEASE, "Validate the online authentication token 'expiry' claim");
static ConVar sv_onlineAuthValidateIssuedAt("sv_onlineAuthValidateIssuedAt", "1", FCVAR_RELEASE, "Validate the online authentication token 'issued at' claim");

static ConVar sv_onlineAuthExpiryTolerance("sv_onlineAuthExpiryTolerance", "1", FCVAR_DEVELOPMENTONLY, "The online authentication token 'expiry' claim tolerance in seconds", true, 0.f, true, float(UINT8_MAX), "Must range between [0,255]");
static ConVar sv_onlineAuthIssuedAtTolerance("sv_onlineAuthIssuedAtTolerance", "30", FCVAR_DEVELOPMENTONLY, "The online authentication token 'issued at' claim tolerance in seconds", true, 0.f, true, float(UINT8_MAX), "Must range between [0,255]");

static ConVar sv_onlineAuthReplayWindow("sv_onlineAuthReplayWindow", "60", FCVAR_RELEASE,
	"Seconds an accepted join token is remembered so the same bytes cannot be presented twice", true, 0.f, true, 300.f);
static ConVar sv_onlineAuthReplayGuard("sv_onlineAuthReplayGuard", "1", FCVAR_RELEASE,
	"Reject a join token whose exact stitched bytes were already accepted within the replay window");

static ConVar sv_quota_stringCmdsPerSecond("sv_quota_stringCmdsPerSecond", "32", FCVAR_RELEASE, "How many string commands per second clients are allowed to submit, 0 to disallow all string commands", true, 0.f, false, 0.f);
static ConVar sv_quota_scriptExecsPerSecond("sv_quota_scriptExecsPerSecond", "3", FCVAR_REPLICATED | FCVAR_RELEASE,
	"How many script executions per second clients are allowed to submit, 0 to disable the limitation thereof.", true, 0.f, false, 0.f);

// Verbs that run a cfg file through Cmd_Exec_f.
static bool IsScriptExecVerb(const char* const pszVerb)
{
	return !_stricmp(pszVerb, "exec")
		|| !_stricmp(pszVerb, "execifexists")
		|| !_stricmp(pszVerb, "execPlayerConfig");
}

// See VProcessStringCmd's admin elevation block and internal notes.
static ConVar sv_bridge_admin_elevation("sv_bridge_admin_elevation", "0", FCVAR_RELEASE, "Execute console commands from an RCON-authenticated loopback client with full console authority (loopback-only, see CONSOLE_SERVER_AUTHORITY_DESIGN)");
static ConVar sv_bridge_admin_passthrough("sv_bridge_admin_passthrough", "", FCVAR_RELEASE, "Space-separated command names that always take the native player path even for the elevated admin");

//---------------------------------------------------------------------------------
// Purpose: check if a peer address is private/local (RFC1918, link-local, ULA)
// Input: adr - peer address from the net channel
// Output: true if private/local, false otherwise
//---------------------------------------------------------------------------------
static bool IsPrivateNetworkAddress(const CNetAdr& adr)
{
	const in6_addr* const pIP = adr.GetIP();
	if (!pIP)
		return false;

	// IPv4 (and IPv4-mapped IPv6): classify from the four host octets.
	if (IN6_IS_ADDR_V4MAPPED(pIP))
	{
		const unsigned a = pIP->s6_addr[12];
		const unsigned b = pIP->s6_addr[13];

		if (a == 10)
			return true;
		if (a == 192 && b == 168)
			return true;
		if (a == 172 && b >= 16 && b <= 31)
			return true;
		if (a == 169 && b == 254)
			return true;
		if (a == 127)
			return true;

		return false;
	}

	// IPv6 unique local (fc00::/7) and link-local (fe80::/10).
	if ((pIP->s6_addr[0] & 0xfe) == 0xfc)
		return true;
	if (pIP->s6_addr[0] == 0xfe && (pIP->s6_addr[1] & 0xc0) == 0x80)
		return true;

	return false;
}

// Rate-limited skip notice: every join that bypasses the token must be audible.
static void Auth_WarnTokenSkipped(const char* const pszReason, const char* const pszAddr)
{
	static double s_flNextWarnTime = 0.0;
	static int s_nSuppressed = 0;
	const double flNow = Plat_FloatTime();

	if (flNow < s_flNextWarnTime)
	{
		++s_nSuppressed;
		return;
	}

	if (s_nSuppressed > 0)
	{
		Warning(eDLL_T::SERVER, "[AUTH] join token skipped: %s (%s) (+%d similar)\n",
			pszReason, pszAddr ? pszAddr : "?", s_nSuppressed);
		s_nSuppressed = 0;
	}
	else
	{
		Warning(eDLL_T::SERVER, "[AUTH] join token skipped: %s (%s)\n",
			pszReason, pszAddr ? pszAddr : "?");
	}

	s_flNextWarnTime = flNow + 2.0;
}

// Connect path is single-threaded on the dedi -- no lock around this table.
enum { AUTH_REPLAY_SLOTS = 128 };

struct AuthReplayEntry_t
{
	uint8_t hash[32];
	uint8_t addr[16]; // in6_addr bytes; port ignored (NAT retransmit)
	double acceptTime;
};

static AuthReplayEntry_t s_authReplayTable[AUTH_REPLAY_SLOTS];

// True only when this token was already accepted from a different peer address.
// Same peer again is a UDP retransmit, not theft.
static bool Auth_TokenReplayFromOtherPeer(const char* const token, const int tokenLen, const CNetAdr& peerAdr)
{
	if (!sv_onlineAuthReplayGuard.GetBool())
		return false;
	if (!token || tokenLen <= 0)
		return false;

	uint8_t tokenHash[32];
	if (mbedtls_sha256(reinterpret_cast<const uint8_t*>(token), tokenLen, tokenHash, NULL) != 0)
		return false;

	const in6_addr* const pPeerIP = peerAdr.GetIP();
	const double flNow = Plat_FloatTime();
	const double flWindow = static_cast<double>(sv_onlineAuthReplayWindow.GetFloat());

	int freeSlot = -1;
	int oldestSlot = 0;
	double oldestTime = s_authReplayTable[0].acceptTime;

	for (int i = 0; i < AUTH_REPLAY_SLOTS; ++i)
	{
		AuthReplayEntry_t& entry = s_authReplayTable[i];
		const bool bExpired = (entry.acceptTime <= 0.0)
			|| (flWindow <= 0.0)
			|| ((flNow - entry.acceptTime) > flWindow);

		if (entry.acceptTime > 0.0
			&& memcmp(entry.hash, tokenHash, sizeof(tokenHash)) == 0)
		{
			if (memcmp(entry.addr, pPeerIP->s6_addr, sizeof(entry.addr)) == 0)
			{
				entry.acceptTime = flNow;
				return false;
			}
			return true;
		}

		if (bExpired)
		{
			if (freeSlot < 0)
				freeSlot = i;
		}

		if (entry.acceptTime < oldestTime)
		{
			oldestTime = entry.acceptTime;
			oldestSlot = i;
		}
	}

	const int slot = (freeSlot >= 0) ? freeSlot : oldestSlot;
	memcpy(s_authReplayTable[slot].hash, tokenHash, sizeof(tokenHash));
	memcpy(s_authReplayTable[slot].addr, pPeerIP->s6_addr, sizeof(s_authReplayTable[slot].addr));
	s_authReplayTable[slot].acceptTime = flNow;
	return false;
}

//---------------------------------------------------------------------------------
// Purpose: check whether this client is authorized to join this server
// Input: *playerName - 
// *reasonBuf - 
// reasonBufLen - 
// Output: true if authorized, false otherwise
//---------------------------------------------------------------------------------
bool CClient::Authenticate(const char* const playerName, char* const reasonBuf, const size_t reasonBufLen)
{
	// Bots have no token and never will.
	if (IsFakeClient())
		return true;

	const CNetAdr& remoteAdr = GetNetChan()->GetRemoteAddress();
	const char* const clientIP = GetNetChan()->GetAddress(true);

	const int nAuthMode = sv_onlineAuthMode.GetInt();
	if (nAuthMode == 0)
	{
		Auth_WarnTokenSkipped("sv_onlineAuthMode 0", clientIP);
		return true;
	}

	// True loopback (engine NA_LOOPBACK or 127.0.0.0/8 / ::1) stays free for local testing.
	if (remoteAdr.IsLoopback() || NET_IsAddressLoopback(remoteAdr))
	{
		Auth_WarnTokenSkipped("loopback", clientIP);
		return true;
	}

	// RFC1918 skip is lab-only: default sv_onlineAuthForceLocal 1 requires a token.
	if (!sv_onlineAuthForceLocal.GetBool() && IsPrivateNetworkAddress(remoteAdr))
	{
		Auth_WarnTokenSkipped("private (sv_onlineAuthForceLocal 0)", clientIP);
		return true;
	}

	l8w8jwt_claim* claims = nullptr;
	size_t numClaims = 0;

	// formats the error reason, and frees the claims and returns
#define ERROR_AND_RETURN(fmt, ...) \
		do {\
			V_snprintf(reasonBuf, reasonBufLen, fmt, ##__VA_ARGS__); \
			if (claims) {\
				l8w8jwt_free_claims(claims, numClaims); \
			}\
			return false; \
		} while(0)\

	KeyValues* const cl_onlineAuthTokenKv = this->m_ConVars->FindKey("cl_onlineAuthToken");
	KeyValues* const cl_onlineAuthTokenSignature1Kv = this->m_ConVars->FindKey("cl_onlineAuthTokenSignature1");
	KeyValues* const cl_onlineAuthTokenSignature2Kv = this->m_ConVars->FindKey("cl_onlineAuthTokenSignature2");

	if (!cl_onlineAuthTokenKv)
	{
		if (nAuthMode == 2)
		{
			Auth_WarnTokenSkipped("no token (sv_onlineAuthMode 2)", clientIP);
			if (claims)
				l8w8jwt_free_claims(claims, numClaims);
			return true;
		}
		ERROR_AND_RETURN("Missing token");
	}

	if (!cl_onlineAuthTokenSignature1Kv)
	{
		if (nAuthMode == 2)
		{
			Auth_WarnTokenSkipped("no token (sv_onlineAuthMode 2)", clientIP);
			if (claims)
				l8w8jwt_free_claims(claims, numClaims);
			return true;
		}
		ERROR_AND_RETURN("Missing signature");
	}

	const char* const onlineAuthToken = cl_onlineAuthTokenKv->GetString();
	const char* const onlineAuthTokenSignature1 = cl_onlineAuthTokenSignature1Kv->GetString();

	if (!*onlineAuthToken)
	{
		if (nAuthMode == 2)
		{
			Auth_WarnTokenSkipped("no token (sv_onlineAuthMode 2)", clientIP);
			if (claims)
				l8w8jwt_free_claims(claims, numClaims);
			return true;
		}
		ERROR_AND_RETURN("Empty token");
	}

	if (!*onlineAuthTokenSignature1)
	{
		if (nAuthMode == 2)
		{
			Auth_WarnTokenSkipped("no token (sv_onlineAuthMode 2)", clientIP);
			if (claims)
				l8w8jwt_free_claims(claims, numClaims);
			return true;
		}
		ERROR_AND_RETURN("Empty signature");
	}

	// Optional second signature half (sig > 255 chars); FindKey is null when absent.
	const char* const onlineAuthTokenSignature2 = cl_onlineAuthTokenSignature2Kv
		? cl_onlineAuthTokenSignature2Kv->GetString()
		: "";

	char fullToken[1024]; // enough buffer for 3x255, which is cvar count * userinfo str limit.
	const int tokenLen = snprintf(fullToken, sizeof(fullToken), "%s.%s%s",
		onlineAuthToken, onlineAuthTokenSignature1, onlineAuthTokenSignature2);

	if (tokenLen < 0)
		ERROR_AND_RETURN("Token stitching failed");
	if (tokenLen >= (int)sizeof(fullToken))
		ERROR_AND_RETURN("Token too long");

	struct l8w8jwt_decoding_params params;
	l8w8jwt_decoding_params_init(&params);

	params.alg = L8W8JWT_ALG_RS256;

	params.jwt = (char*)fullToken;
	params.jwt_length = tokenLen;

	params.validate_exp = sv_onlineAuthValidateExpiry.GetBool();
	params.exp_tolerance_seconds = (uint8_t)sv_onlineAuthExpiryTolerance.GetInt();

	params.validate_iat = sv_onlineAuthValidateIssuedAt.GetBool();
	params.iat_tolerance_seconds = (uint8_t)sv_onlineAuthIssuedAtTolerance.GetInt();

	// Try each trusted key until one verifies the signature. Only a signature
	// failure moves on -- an expired or malformed token fails the same way under
	// every key, and retrying it would just report the last key's verdict.
	static const char* const verificationKeys[] = { JWT_PUBLIC_KEY, JWT_PUBLIC_KEY_ROTATING };

	enum l8w8jwt_validation_result validation_result = L8W8JWT_VALID;
	int r = L8W8JWT_INVALID_ARG; // stands if every key slot is empty

	for (size_t keyIdx = 0; keyIdx < V_ARRAYSIZE(verificationKeys); keyIdx++)
	{
		const char* const verificationKey = verificationKeys[keyIdx];

		if (!verificationKey[0])
			continue;

		// Length counts the terminator, matching what the key literal carries.
		params.verification_key = (unsigned char*)verificationKey;
		params.verification_key_length = strlen(verificationKey) + 1;

		if (claims)
		{
			l8w8jwt_free_claims(claims, numClaims);
			claims = nullptr;
			numClaims = 0;
		}

		r = l8w8jwt_decode(&params, &validation_result, &claims, &numClaims);

		if (r == L8W8JWT_SUCCESS && !(validation_result & L8W8JWT_SIGNATURE_VERIFICATION_FAILURE))
			break;
	}

	if (r != L8W8JWT_SUCCESS)
		ERROR_AND_RETURN("Code %i", r);

	if (validation_result != L8W8JWT_VALID)
	{
		char reasonBuffer[64];
		l8w8jwt_get_validation_result_desc(validation_result, reasonBuffer, sizeof(reasonBuffer));

		ERROR_AND_RETURN("%s", reasonBuffer);
	}

	bool foundSessionId = false;
	for (size_t i = 0; i < numClaims; ++i)
	{
		const l8w8jwt_claim& claim = claims[i];

		// session id
		if (!strcmp(claim.key, "sessionId"))
		{
			const char* const sessionId = claim.value;

			if (g_ServerHostManager.GetHostIP().empty())
			{
				const char* pszIp = (hostip && hostip->GetString()[0])
					? hostip->GetString() : "";
				const int nPort = hostport ? hostport->GetInt() : 0;
				if (pszIp[0] && nPort > 0)
				{
					char szSeed[128];
					V_snprintf(szSeed, sizeof(szSeed), "[%s]:%d", pszIp, nPort);
					g_ServerHostManager.SetHostIP(szSeed);
				}
				else
					ERROR_AND_RETURN("Host IP not published yet");
			}

			char newId[256];
			const int idLen = snprintf(newId, sizeof(newId), "%llu-%s-%s",
				(PlatformUserId_t)this->m_DataBlock.userData,
				playerName,
				g_ServerHostManager.GetHostIP().c_str());

			if (idLen < 0)
				ERROR_AND_RETURN("Session ID stitching failed");
			if (idLen >= (int)sizeof(newId))
				ERROR_AND_RETURN("Session ID too long");

			// Shorter claim leaves sessionHash partially uninitialised for memcmp.
			if (claim.value_length != 64)
				ERROR_AND_RETURN("Malformed session ID");

			uint8_t sessionHash[32]; // hash decoded from JWT token
			V_hextobinary(sessionId, claim.value_length, sessionHash, sizeof(sessionHash));

			uint8_t oobHash[32]; // hash of data collected from out of band packet
			const int shRet = mbedtls_sha256((const uint8_t*)newId, idLen, oobHash, NULL);

			if (shRet != NULL)
				ERROR_AND_RETURN("Session ID hashing failed");

			if (memcmp(oobHash, sessionHash, sizeof(sessionHash)) != 0)
			{
				// Signature ok, session mismatch: the three inputs disagreed.
				// Nothing here is secret -- the server observed all three itself.
				char computedHashHex[65];
				V_binarytohex(oobHash, sizeof(oobHash), computedHashHex, sizeof(computedHashHex));

				Warning(eDLL_T::SERVER, "[AUTH] session mismatch: server built '%s' (%s), token carried %s\n",
					newId, computedHashHex, sessionId);

				ERROR_AND_RETURN("Token is not authorized for the connecting client");
			}

			foundSessionId = true;
			break;
		}
	}

	if (!foundSessionId)
		ERROR_AND_RETURN("No session ID");

	// After session check only: rejected tokens must not burn a replay slot.
	if (Auth_TokenReplayFromOtherPeer(fullToken, tokenLen, remoteAdr))
	{
		Warning(eDLL_T::SERVER, "[AUTH] join token already accepted from a different address; rejected %s\n",
			clientIP ? clientIP : "?");
		ERROR_AND_RETURN("Token already used");
	}

	l8w8jwt_free_claims(claims, numClaims);

#undef ERROR_AND_RETURN

	return true;
}

//---------------------------------------------------------------------------------
// Purpose: connect new client
// Input: *szName - 
// *pNetChan - 
// bFakePlayer - 
// *conVars - 
// *szMessage -
// nMessageSize - 
// Output: true if connection was successful, false otherwise
//---------------------------------------------------------------------------------
bool CClient::Connect(const char* szName, CNetChan* pNetChan, bool bFakePlayer,
	CUtlVector<NET_SetConVar::cvar_t>* conVars, char* szMessage, int nMessageSize)
{
	GetClientExtended()->Reset(); // Reset extended data.

	if (!CClient__Connect(this, szName, pNetChan, bFakePlayer, conVars, szMessage, nMessageSize))
		return false;


#define REJECT_CONNECTION(fmt, ...) V_snprintf(szMessage, nMessageSize, fmt, ##__VA_ARGS__);

	if (sv_onlineAuthEnable.GetBool())
	{
		char authFailReason[512];
		if (!Authenticate(szName, authFailReason, sizeof(authFailReason)))
		{
			REJECT_CONNECTION("Failed to verify authentication token! [%s]", authFailReason);

			const bool bEnableLogging = sv_showconnecting.GetBool();
			if (bEnableLogging)
			{
				const char* const netAdr = pNetChan ? pNetChan->GetAddress() : "<unknown>";

				Warning(eDLL_T::SERVER, "Client '%s' ('%llu') failed online authentication! [%s]\n",
					netAdr, (PlatformUserId_t)m_DataBlock.userData, authFailReason);
			}

			return false;
		}
	}

#undef REJECT_CONNECTION

	return true;
}

//---------------------------------------------------------------------------------
// Purpose: connect new client
// Input: *pClient - 
// *szName - 
// *pNetChan - 
// bFakePlayer - 
// *a5 - 
// *szMessage -
// nMessageSize - 
// Output: true if connection was successful, false otherwise
//---------------------------------------------------------------------------------
bool CClient::VConnect(CClient* pClient, const char* szName, CNetChan* pNetChan, bool bFakePlayer,
	CUtlVector<NET_SetConVar::cvar_t>* conVars, char* szMessage, int nMessageSize)
{
	return pClient->Connect(szName, pNetChan, bFakePlayer, conVars, szMessage, nMessageSize);
}

//---------------------------------------------------------------------------------
// Purpose: registers net messages
// Input: *pClient - 
// *pChan - 
// Output: true if setup was successful, false otherwise
//---------------------------------------------------------------------------------
bool CClient::VConnectionStart(CClient* pClient, CNetChan* pChan)
{
	pClient->RegisterNetMsgs(pChan);
	return CClient__ConnectionStart(pClient, pChan);
}

//---------------------------------------------------------------------------------
// Purpose: disconnect client
// Input: nRepLvl - 
// *szReason - 
//			... - 
//---------------------------------------------------------------------------------
void CClient::Disconnect(const Reputation_t nRepLvl, const char* szReason, ...)
{
	if (m_nSignonState != SIGNONSTATE::SIGNONSTATE_NONE)
	{
		char szBuf[1024];
		{/////////////////////////////
			va_list vArgs;
			va_start(vArgs, szReason);

			const int ret = V_vsnprintf(szBuf, sizeof(szBuf), szReason, vArgs);

			if (ret < 0)
				szBuf[0] = '\0';

			va_end(vArgs);
		}/////////////////////////////
		CClient__Disconnect(this, nRepLvl, szBuf);
	}
}

//---------------------------------------------------------------------------------
// Forge svc_UserMessage type 54 at SIGNONSTATE_FULL (S21 ClientInitComplete).
// S3 slot 54 is SetMixLayerTriggerFactor / RemoteFunctionCallsChecksum by name.
//---------------------------------------------------------------------------------
static void Bridge_SendClientInitComplete(const CPlayer* pTarget, int nUserID)
{
	if (!pTarget || !pTarget->IsConnected())
		return;

	CSingleUserRecipientFilter filter(pTarget);
	filter.MakeReliable();

	// Name must match S3 enum slot 54 or UserMessageBegin rejects; wire carries the int.
	v_UserMessageBegin(&filter, "RemoteFunctionCallsChecksum",
		static_cast<int>(UserMessages_t::RemoteFunctionCallsChecksum));

	if (!*g_ppUsrMessageBuffer)
	{
		Warning(eDLL_T::SERVER,
			"Bridge_SendClientInitComplete: no usermessage buffer (uid #%d)\n",
			nUserID);
		return;
	}

	// Write 1 dummy byte to ensure non-zero payload length on the wire.
	// S21 ClientInitComplete handler reads 0 args, so the byte sits unread.
	(*g_ppUsrMessageBuffer)->WriteByte(0);

	MessageEnd();

	DevMsg(eDLL_T::SERVER,
		"Bridge: sent svc_UserMessage type=54 (ClientInitComplete) to uid #%d\n",
		nUserID);
}

//---------------------------------------------------------------------------------
// Purpose: activate player
// Input: *pClient -
//---------------------------------------------------------------------------------
void CClient::VActivatePlayer(CClient* pClient)
{
	// Set the client instance to 'ready' before calling ActivatePlayer.
	pClient->SetPersistenceState(PERSISTENCE::PERSISTENCE_READY);

	// [SIGNON-PHASE] charge the native activate (serverGameClients->ClientActive
	// -> DecideRespawnPlayer chain) to its accumulator when this runs inside a
	// net_SignonState handler.
	const double flActivateStart = Plat_FloatTime();
	CClient__ActivatePlayer(pClient);
	g_flSignonPhaseActivateMs += (Plat_FloatTime() - flActivateStart) * 1000.0;

	const CNetChan* pNetChan = pClient->GetNetChan();

	if (pNetChan && sv_showconnecting.GetBool())
	{
		DevMsg(eDLL_T::SERVER, "Activated player #%d; channel %s(%s) ('%llu')\n",
			pClient->GetUserID(), pNetChan->GetName(), pNetChan->GetAddress(), pClient->GetPlatformUserId());
	}

	// [SIGNON-PHASE] everything below is bridge post-activate work; time it as
	// one phase so it cannot hide inside the native residual.
	const double flPostStart = Plat_FloatTime();

	// Send full SNDC extension state to the newly activated player
	SNDC_SendFullSync(pClient);

	// SNDC full sync, extra-shield burst, then ClientInitComplete (type 54).
	if (CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle()))
	{
		VScriptPlayer_SendExtraShieldInitialState(pPlayer);
		Bridge_SendClientInitComplete(pPlayer, pClient->GetUserID());
	}

	g_flSignonPhasePostMs += (Plat_FloatTime() - flPostStart) * 1000.0;
}

//---------------------------------------------------------------------------------
// Purpose: registers net messages
// Input: *chan
//---------------------------------------------------------------------------------
void CClient::RegisterNetMsgs(CNetChan* chan)
{
	REGISTER_NET_MSG(ScriptMessage);
}

//---------------------------------------------------------------------------------
// Purpose: send a net message with replay.
// set 'CNetMessage::m_nGroup' to 'NoReplay' to disable replay.
// Input: *pMsg - 
// bLocal - 
// bForceReliable - 
// bVoice - 
//---------------------------------------------------------------------------------
bool CClient::SendNetMsgEx(CNetMessage* pMsg, bool bLocal, bool bForceReliable, bool bVoice)
{
	if (!CanReplayMessage(pMsg))
	{
		// Don't copy the message into the replay buffer.
		pMsg->m_nGroup = NetMessageGroup::NoReplay;
	}

	return CClient__SendNetMsgEx(this, pMsg, bLocal, bForceReliable, bVoice);
}

//---------------------------------------------------------------------------------
// Purpose: send a snapshot
// Input: *pClient - 
// *pFrame - 
// nTick - 
// nTickAck - 
//---------------------------------------------------------------------------------
void* CClient::VSendSnapshot(CClient* pClient, CClientFrame* pFrame, int nTick, int nTickAck)
{
	return CClient__SendSnapshot(pClient, pFrame, nTick, nTickAck);
}

//---------------------------------------------------------------------------------
// Purpose: internal hook to 'CClient::SendNetMsgEx'
// Input: *pClient - 
// *pMsg - 
// bLocal - 
// bForceReliable - 
// bVoice - 
//---------------------------------------------------------------------------------
bool CClient::VSendNetMsgEx(CClient* pClient, CNetMessage* pMsg, bool bLocal, bool bForceReliable, bool bVoice)
{
	return pClient->SendNetMsgEx(pMsg, bLocal, bForceReliable, bVoice);
}

//---------------------------------------------------------------------------------
// Purpose: write data into data blocks to send to the client
// Input: &buf
//---------------------------------------------------------------------------------
void CClient::WriteDataBlock(CClient* pClient, bf_write& buf)
{
	if (net_data_block_enabled->GetBool())
	{
		buf.WriteUBitLong(net_NOP, NETMSG_TYPE_BITS);

		const int remainingBits = buf.GetNumBitsWritten() % 8;

		if (remainingBits && (8 - remainingBits) > 0)
		{
			// fill the last bits in the last byte with NOP
			buf.WriteUBitLong(net_NOP, 8 - remainingBits);
		}

		const bool isMultiplayer = gpGlobals->gameMode < GameMode_t::PVE_MODE;

		// [SIGNON-PHASE] the sender compresses the whole signon blob (LZ4) into
		// its scratch buffer synchronously on this thread; charge it so a slow
		// join cannot masquerade as native handler time.
		const double flDbStart = Plat_FloatTime();
		pClient->m_DataBlock.sender.WriteDataBlock(buf.GetData(), buf.GetNumBytesWritten(), isMultiplayer, buf.GetDebugName());
		g_flSignonPhaseDbBlockMs += (Plat_FloatTime() - flDbStart) * 1000.0;
	}
	else
	{
		pClient->m_NetChannel->SendData(buf, true);
	}
}

//---------------------------------------------------------------------------------
// Purpose: some versions of the binary have an optimization that shifts the 'this'
// pointer of the CClient structure by 8 bytes to avoid having to cache the vftable
// pointer if it never get used. Here we shift it back so it aligns again.
//---------------------------------------------------------------------------------
CClient* AdjustShiftedThisPointer(CClient* shiftedPointer)
{
	/* ProcessStringCmd inlines ExecuteStringCommand; callers pass this+8. */
	char* pShifted = reinterpret_cast<char*>(shiftedPointer) - 8;
	return reinterpret_cast<CClient*>(pShifted);
}

//---------------------------------------------------------------------------------
// Purpose: whole-token (space-delimited, case-insensitive) membership test against
// sv_bridge_admin_passthrough, so "sv_cheats" does NOT match "sv_cheats_extra".
// Input: *pszCmdName -
// Output: true if present in the list, false otherwise
//---------------------------------------------------------------------------------
static bool AdminPassthroughList_Contains(const char* const pszCmdName)
{
	if (!pszCmdName || !*pszCmdName)
		return false;

	const char* pList = sv_bridge_admin_passthrough.GetString();
	if (!pList || !*pList)
		return false;

	const size_t nCmdLen = strlen(pszCmdName);

	for (const char* p = pList; *p; )
	{
		while (*p == ' ') ++p;               // skip separators
		const char* const pTokenStart = p;
		while (*p && *p != ' ') ++p;         // token end
		const size_t nTokenLen = size_t(p - pTokenStart);

		if (nTokenLen == nCmdLen && _strnicmp(pTokenStart, pszCmdName, nTokenLen) == 0)
			return true;
	}

	return false;
}

//---------------------------------------------------------------------------------
// Purpose: process string commands (kicking anyone attempting to DOS)
// Input: *pClient - (ADJ)
// *pMsg -
// Output: false if cmd should be passed to CServerGameClients
//---------------------------------------------------------------------------------
bool CClient::VProcessStringCmd(CClient* pClient, NET_StringCmd* pMsg)
{
	CClient* const pClient_Adj = AdjustShiftedThisPointer(pClient);

	// Jettison the cmd if the client isn't active.
	if (!pClient_Adj->IsActive())
		return true;

	CClientExtended* const pSlot = pClient_Adj->GetClientExtended();

	const double flStartTime = Plat_FloatTime();
	const int nCmdQuotaLimit = sv_quota_stringCmdsPerSecond.GetInt();

	if (!nCmdQuotaLimit)
		return true;

	const char* pCmd = pMsg->cmd;
	// Just skip if the cmd pointer is null, we still check if the
	// client sent too many commands and take appropriate actions.
	// The internal function discards the command if it's null.
	if (pCmd)
	{
		// There is an issue in CUtlBuffer::ParseToken that causes it to read
		// past its buffer; mostly seems to happen on 32bit, but a carefully
		// crafted string should work on 64bit too). The fix is to just null 
		// everything past the maximum allowed length. The second 'theoretical'
		// fix would be to properly fix CUtlBuffer::ParseToken by computing
		// the UTF8 character size each iteration and check if it still doesn't
		// exceed bounds.
		memset(&pMsg->buffer[STRINGCMD_MAX_LEN],
			'\0', sizeof(pMsg->buffer) - (STRINGCMD_MAX_LEN));

		if (!V_IsValidUTF8(pCmd))
		{
			Warning(eDLL_T::SERVER, "Removing client '%s' from slot #%i ('%llu' sent invalid string command!)\n",
				pClient_Adj->GetNetChan()->GetAddress(), pClient_Adj->GetUserID(), pClient_Adj->GetPlatformUserId());

			pClient_Adj->Disconnect(Reputation_t::REP_MARK_BAD, "#DISCONNECT_INVALID_STRINGCMD");
			return true;
		}

		// Dedupe before quota so redundant copies do not multiply quota usage.
		if (pCmd[0] == 'b' && pCmd[1] == 'r' && pCmd[2] == 'q' && pCmd[3] == ' ')
		{
			const char* const pszNum = pCmd + 4;
			if (pszNum[0] >= '0' && pszNum[0] <= '9')
			{
				char* pszEnd = nullptr;
				const unsigned long nSeqUL = strtoul(pszNum, &pszEnd, 10);
				const ptrdiff_t nDigits = pszEnd ? (pszEnd - pszNum) : 0;
				if (pszEnd && nDigits > 0 && nDigits <= 10
					&& !(nDigits == 10 && strncmp(pszNum, "4294967295", 10) > 0)
					&& *pszEnd == ' ')
				{
					const uint32_t nSeq = static_cast<uint32_t>(nSeqUL);
					const char* const pszRest = pszEnd + 1;
					uint32_t& nHigh = pSlot->m_nBridgeStrCmdSeqHigh;
					uint64_t& nMask = pSlot->m_nBridgeStrCmdSeenMask;
					bool bAccept = false;
					if (nHigh == 0 && nMask == 0)
					{
						nHigh = nSeq;
						nMask = 1;
						bAccept = true;
					}
					else if (nSeq > nHigh)
					{
						const uint32_t nShift = nSeq - nHigh;
						nMask = (nShift >= 64) ? 0ull : (nMask << nShift);
						nMask |= 1ull;
						nHigh = nSeq;
						bAccept = true;
					}
					else
					{
						const uint32_t nDelta = nHigh - nSeq;
						if (nDelta < 64 && ((nMask >> nDelta) & 1ull) == 0)
						{
							nMask |= (1ull << nDelta);
							bAccept = true;
						}
					}

					if (!bAccept)
					{
						++s_nBridgeStrCmdDrops;
						if (s_nBridgeStrCmdDrops <= 8 || (s_nBridgeStrCmdDrops % 256) == 0)
							Warning(eDLL_T::SERVER,
								"[BRIDGE-SCMD] drop dup seq=%u slot=%d\n",
								nSeq, pClient_Adj->GetUserID());
						return true;
					}

					// rest may already point into pMsg->buffer.
					memmove(pMsg->buffer, pszRest, strlen(pszRest) + 1);
					pMsg->cmd = pMsg->buffer;
					pCmd = pMsg->cmd;
				}
			}
		}
	}

	if (flStartTime - pSlot->m_flStringCommandQuotaTimeStart >= 1.0)
	{
		pSlot->m_flStringCommandQuotaTimeStart = flStartTime;
		pSlot->m_nStringCommandQuotaCount = 0;
	}
	++pSlot->m_nStringCommandQuotaCount;

	if (pSlot->m_nStringCommandQuotaCount > nCmdQuotaLimit)
	{
		Warning(eDLL_T::SERVER, "Removing client '%s' from slot #%i ('%llu' exceeded string command quota!)\n",
			pClient_Adj->GetNetChan()->GetAddress(), pClient_Adj->GetUserID(), pClient_Adj->GetPlatformUserId());

		pClient_Adj->Disconnect(Reputation_t::REP_MARK_BAD, "#DISCONNECT_STRINGCMD_OVERFLOW");
		return true;
	}

	// Loopback client-console admin elevation. Game netchan and an authenticated
	// RCON session must both be loopback; remote rcon.exe is a separate EXEC path.
	if (sv_bridge_admin_elevation.GetBool() && pCmd &&
		NET_IsAddressLoopback(pClient_Adj->GetNetChan()->GetRemoteAddress()) &&
		RCONServer()->IsInitialized() &&
		RCONServer()->HasAuthenticatedLoopbackSession())
	{
		CCommand args;
		if (args.Tokenize(pCmd, cmd_source_t::kCommandSrcNetClient) && args.ArgC() > 0)
		{
			ConCommandBase* const pElevBase = g_pCVar->FindCommandBase(args.Arg(0));
			if (pElevBase && !AdminPassthroughList_Contains(args.Arg(0)))
			{
				// give_server/noclip need UTIL_GetCommandClient; unrestricted never sets it.
				if (!(pElevBase->IsCommand() && pElevBase->IsFlagSet(FCVAR_CHEAT)))
				{
					Warning(eDLL_T::SERVER, "[ADMIN-CMD] slot=%i id64=%llu '%s'\n",
						pClient_Adj->GetUserID(), pClient_Adj->GetPlatformUserId(), pMsg->cmd);

					Cmd_ExecuteUnrestricted(args.Arg(0), pMsg->cmd);
					return true; // Consumed -- do not also run the native path.
				}
			}
		}
	}

	if (pCmd && g_pCVar)
	{
		CCommand verbArgs;
		if (!verbArgs.Tokenize(pCmd, cmd_source_t::kCommandSrcNetClient))
		{
			Warning(eDLL_T::SERVER,
				"[BRIDGE-SCMD] drop from slot=%i (tokenize overflow)\n",
				pClient_Adj->GetUserID());
			return true;
		}
		if (verbArgs.ArgC() > 0)
		{
			ConCommandBase* const pBase = g_pCVar->FindCommandBase(verbArgs.Arg(0));
			// FCVAR_CHEAT is native sv_cheats (give_server is CHEAT|GAMEDLL|HIDDEN).
			if (pBase && pBase->IsCommand() && pBase->IsFlagSet(FCVAR_DEVELOPMENTONLY))
			{
				static int s_scmdDeny = 0;
				if (++s_scmdDeny <= 8)
					Warning(eDLL_T::SERVER,
						"[BRIDGE-SCMD] drop '%s' from slot=%i (devonly ConCommand)\n",
						verbArgs.Arg(0), pClient_Adj->GetUserID());
				return true;
			}
			const char* pszVerb = verbArgs.Arg(0);
			const int nExecQuota = sv_quota_scriptExecsPerSecond.GetInt();
			if (nExecQuota > 0 && pszVerb && IsScriptExecVerb(pszVerb))
			{
				if (flStartTime - pSlot->m_flScriptExecQuotaTimeStart >= 1.0)
				{
					pSlot->m_flScriptExecQuotaTimeStart = flStartTime;
					pSlot->m_nScriptExecQuotaCount = 0;
				}

				if (++pSlot->m_nScriptExecQuotaCount > nExecQuota)
				{
					static int s_execDeny = 0;
					if (++s_execDeny <= 8)
						Warning(eDLL_T::SERVER,
							"[BRIDGE-SCMD] drop '%s' from slot=%i (exec quota %d/s)\n",
							pszVerb, pClient_Adj->GetUserID(), nExecQuota);
					return true;
				}
			}
			if (pszVerb && (!_stricmp(pszVerb, "net_setkey")
				|| !_stricmp(pszVerb, "net_generatekey")
				|| !_stricmp(pszVerb, "net_getkey")
				|| !_stricmp(pszVerb, "sv_netkey")
				|| !_stricmp(pszVerb, "net_tracePayload")
				|| !_stricmp(pszVerb, "net_dumpWire")
				|| !_stricmp(pszVerb, "net_useRandomKey")))
			{
				Warning(eDLL_T::SERVER,
					"[BRIDGE-SCMD] drop '%s' from slot=%i (net-key)\n",
					pszVerb, pClient_Adj->GetUserID());
				return true;
			}
			if (pszVerb && (!_stricmp(pszVerb, "spawnbots")
				|| !_stricmp(pszVerb, "playlist_override_set")
				|| !_stricmp(pszVerb, "playlist_override_clear")
				|| !_stricmp(pszVerb, "launchplaylist")
				|| !_stricmp(pszVerb, "fs_guardLiveMapUnmount")
				|| !_stricmp(pszVerb, "sdk_splitpacket_recv_clamp")
				|| !_stricmp(pszVerb, "language")
				|| !_stricmp(pszVerb, "bridge_akimbo")
				|| !_stricmp(pszVerb, "bridge_akimbo_deploy_partner")
				|| !_stricmp(pszVerb, "bridge_pose_param_ext")
				|| !_stricmp(pszVerb, "bridge_pose_moveyaw")))
			{
				Warning(eDLL_T::SERVER,
					"[BRIDGE-SCMD] drop '%s' from slot=%i (cheat)\n",
					pszVerb, pClient_Adj->GetUserID());
				return true;
			}
		}
	}


	return CClient__ProcessStringCmd(pClient, pMsg);
}

//---------------------------------------------------------------------------------
// Purpose: process set convar
// Input: *pClient - (ADJ)
// *pMsg - 
// Output 
//---------------------------------------------------------------------------------
// Bridge-only USERINFO keys the native dump omits; reconnect can latch without them.
static bool UserInfoKeyAlwaysAllowed(const char* const name)
{
	static const char* const kBridgeOnly[] =
	{
		"mantle_boost_input_setting",
		"bridge_mantle_boost_button_mask",
		"net_maxroutable",
		"sdk_mods",
	};
	if (!name)
		return false;
	for (size_t i = 0; i < SDK_ARRAYSIZE(kBridgeOnly); ++i)
	{
		if (!V_stricmp(name, kBridgeOnly[i]))
			return true;
	}
	return false;
}

bool CClient::VProcessSetConVar(CClient* pClient, NET_SetConVar* pMsg)
{
	CClient* const pAdj = AdjustShiftedThisPointer(pClient);
	CClientExtended* const pSlot = pAdj->GetClientExtended();
	const bool bFirstPacket = !pSlot->m_bInitialConVarsSet;
	bool bChanged = false;

	// This loop never exceeds 255 iterations, NET_SetConVar::ReadFromBuffer(...)
	// reads and inserts up to 255 entries in the vector (reads a byte for size).
	FOR_EACH_VEC(pMsg->m_ConVars, i)
	{
		const NET_SetConVar::cvar_t& entry = pMsg->m_ConVars[i];
		const char* const name = entry.name;
		const char* const value = entry.value;

		// Discard any ConVar change request if it contains funky characters.
		bool bFunky = false;
		for (const char* s = name; *s != '\0'; ++s)
		{
			if (!V_isalnum(*s) && *s != '_')
			{
				bFunky = true;
				break;
			}
		}
		if (bFunky)
		{
			DevWarning(eDLL_T::SERVER, "Ignoring ConVar change request for variable '%s' from client '%s'; invalid characters in the variable name\n",
				name, pAdj->GetClientName());
			continue;
		}

		// The initial set of ConVars must contain all client ConVars that are
		// flagged UserInfo. This is a simple fix to exploits that send bogus
		// data later, and catches bugs, such as new UserInfo ConVars appearing
		// later, which shouldn't happen.
		// Exception: bridge-only keys (see UserInfoKeyAlwaysAllowed) -- they
		// never ride the S21 native dump that often wins the reconnect race.
		KeyValues* const pExisting = pAdj->m_ConVars->FindKey(name);
		if (pSlot->m_bInitialConVarsSet && !pExisting
			&& !UserInfoKeyAlwaysAllowed(name))
		{
			continue;
		}

		if (pExisting)
		{
			const char* const pszOld = pExisting->GetString();
			if (pszOld && value && !V_strcmp(pszOld, value))
				continue;
		}

		pAdj->m_ConVars->SetString(name, value);
		bChanged = true;
	}

	pSlot->m_bInitialConVarsSet = true;
	if (bFirstPacket || bChanged)
		pAdj->m_bConVarsChanged = true;

	return true;
}

//---------------------------------------------------------------------------------
// Purpose: When the client is muted and they speak send a text chat message to alert them they are banned
// Input: *pClient
//---------------------------------------------------------------------------------
static void InformClientAboutCommsBanTriggeredByVoice(CClient* const pClient)
{
	CClientExtended* const pClientExtended = pClient->GetClientExtended();

	if (!pClientExtended->HasBeenPromptedFromVoiceAboutBan())
	{
		CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());

		if (!pPlayer || !pPlayer->IsConnected())
			return;

		CSingleUserRecipientFilter filter(pPlayer);
		v_UserMessageBegin(&filter, "SayText", 2);

		MessageWriteByte(pPlayer->GetEdict());
		MessageWriteString(pClient->GetClientExtended()->GetCommsMuteDisplayMessage());
		MessageWriteBool(true);

		MessageEnd();

		pClientExtended->SetHasBeenPromptedFromVoiceAboutBan(true);
	}
}

//---------------------------------------------------------------------------------
// Purpose: This builds and stores the message we send to a client when they are comms banned
// Input: *pszReason -
// *pszExpiryTimestamp - 
// Output 
//---------------------------------------------------------------------------------
void CClientExtended::BuildCommsBanDisplayMessage(const char* pszReasonStr, const char* pszExpiryTimestamp)
{
	CFmtStr fmt;

	//The default bansystem reason for when there is no reason from the MS is this
	//TODO: Localize this string into what it should be, for now we just give a descriptive default
	if (pszReasonStr)
	{
		if (V_strcmp("#DISCONNECT_BANNED", pszReasonStr) == 0)
		{
			pszReasonStr = "Communication Banned";
		}
	}

	fmt.Format("You have an active %s communications ban.\nReason: %s\n",
		pszExpiryTimestamp ? "temporary" : "permanent",
		pszReasonStr ? pszReasonStr : "None"
	);

	if (pszExpiryTimestamp)
	{
		fmt.AppendFormat("Expiry: %s", pszExpiryTimestamp);
	}

	m_MuteDisplayPrompt = fmt;
}

//---------------------------------------------------------------------------------
// Purpose: process voice data
// Input: *pClient - (ADJ)
// *pMsg - 
// Output 
//---------------------------------------------------------------------------------
bool CClient::VProcessVoiceData(CClient* pClient, CLC_VoiceData* pMsg)
{
	char voiceDataBuffer[4096];
	const int bitsRead = pMsg->m_DataIn.ReadBitsClamped(voiceDataBuffer, pMsg->m_nLength);

	if (pMsg->m_DataIn.IsOverflowed())
		return false;

	CClient* const pAdj = AdjustShiftedThisPointer(pClient);

	//Is our client communication banned
	if (pAdj->GetClientExtended()->IsClientCommsBanned())
	{
		//Should we apply the communication ban based on what the host has decided
		if (SV_ShouldApplyVoiceChatGlobalMutes())
		{
			InformClientAboutCommsBanTriggeredByVoice(pAdj);
			return true;
		}
	}

	SV_BroadcastVoiceData(pAdj, Bits2Bytes(bitsRead), voiceDataBuffer);

	return true;
}

//---------------------------------------------------------------------------------
// Purpose: process durango voice data
// Input: *pClient - (ADJ)
// *pMsg - 
// Output 
//---------------------------------------------------------------------------------
bool CClient::VProcessDurangoVoiceData(CClient* pClient, CLC_DurangoVoiceData* pMsg)
{
	char voiceDataBuffer[4096];
	const int bitsRead = pMsg->m_DataIn.ReadBitsClamped(voiceDataBuffer, pMsg->m_nLength);

	if (pMsg->m_DataIn.IsOverflowed())
		return false;

	CClient* const pAdj = AdjustShiftedThisPointer(pClient);

	//Is our client communication banned
	if (pAdj->GetClientExtended()->IsClientCommsBanned())
	{
		//Should we apply the communication ban based on what the host has decided
		if (SV_ShouldApplyVoiceChatGlobalMutes())
		{
			InformClientAboutCommsBanTriggeredByVoice(pAdj);
			return true;
		}
	}

	SV_BroadcastDurangoVoiceData(pAdj, Bits2Bytes(bitsRead), voiceDataBuffer,
		pMsg->m_xid, pMsg->m_unknown, pMsg->m_useVoiceStream, pMsg->m_skipXidCheck);

	return true;
}

//---------------------------------------------------------------------------------
// Purpose: set UserCmd time buffer
// Input: numUserCmdProcessTicksMax - 
// tickInterval - 
//---------------------------------------------------------------------------------
void CClientExtended::InitializeMovementTimeForUserCmdProcessing(const int numUserCmdProcessTicksMax, const float tickInterval)
{
	// Grant the client some time buffer to execute user commands
	m_flMovementTimeForUserCmdProcessingRemaining += tickInterval;

	// but never accumulate more than N ticks
	if (m_flMovementTimeForUserCmdProcessingRemaining > numUserCmdProcessTicksMax * tickInterval)
		m_flMovementTimeForUserCmdProcessingRemaining = numUserCmdProcessTicksMax * tickInterval;
}

//---------------------------------------------------------------------------------
// Purpose: consume UserCmd time buffer
// Input: flTimeNeeded -
// Output: max time allowed for processing
//---------------------------------------------------------------------------------
float CClientExtended::ConsumeMovementTimeForUserCmdProcessing(const float flTimeNeeded)
{
	if (m_flMovementTimeForUserCmdProcessingRemaining <= 0.0f)
		return 0.0f;
	else if (flTimeNeeded > m_flMovementTimeForUserCmdProcessingRemaining + FLT_EPSILON)
	{
		const float flResult = m_flMovementTimeForUserCmdProcessingRemaining;
		m_flMovementTimeForUserCmdProcessingRemaining = 0.0f;

		return flResult;
	}
	else
	{
		m_flMovementTimeForUserCmdProcessingRemaining -= flTimeNeeded;

		if (m_flMovementTimeForUserCmdProcessingRemaining < 0.0f)
			m_flMovementTimeForUserCmdProcessingRemaining = 0.0f;

		return flTimeNeeded;
	}
}

void VClient::Detour(const bool bAttach) const
{
	DetourSetup(&CClient__Clear, &CClient::VClear, bAttach);
	DetourSetup(&CClient__Connect, &CClient::VConnect, bAttach);
	DetourSetup(&CClient__ConnectionStart, &CClient::VConnectionStart, bAttach);
	DetourSetup(&CClient__ActivatePlayer, &CClient::VActivatePlayer, bAttach);
	DetourSetup(&CClient__SendNetMsgEx, &CClient::VSendNetMsgEx, bAttach);
	//DetourSetup(&CClient__SendSnapshot, &CClient::VSendSnapshot, bAttach);
	DetourSetup(&CClient__WriteDataBlock, &CClient::WriteDataBlock, bAttach);

	DetourSetup(&CClient__ProcessStringCmd, &CClient::VProcessStringCmd, bAttach);
	DetourSetup(&CClient__ProcessSetConVar, &CClient::VProcessSetConVar, bAttach);
	DetourSetup(&CClient__ProcessVoiceData, &CClient::VProcessVoiceData, bAttach);
	DetourSetup(&CClient__ProcessDurangoVoiceData, &CClient::VProcessDurangoVoiceData, bAttach);
}
#endif // CLIENT_DLL
