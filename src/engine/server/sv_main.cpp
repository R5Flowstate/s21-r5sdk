#if defined(CLIENT_DLL)
#else // !CLIENT_DLL
//===========================================================================//
//
// Purpose
//
//===========================================================================//
#include "core/stdafx.h"
#include "tier0/threadtools.h"
#include "tier0/frametask.h"
#include "tier1/cvar.h"
#include "engine/server/sv_main.h"
#include "engine/server/zipline_validation.h"
#include "engine/client/client.h"
#include "networksystem/spire.h"
#include "networksystem/bansystem.h"
#include "server.h"
#include "game/server/gameinterface.h"
#include "game/server/util_server.h"


static ConVar sv_applyGlobalCommsBans("sv_applyGlobalCommsBans", "3", FCVAR_RELEASE, "Determines whether or not to use the global chat ban list, 0 = None, 1 = Text, 2 = Voice, 3 = Both.", false, 0.f, true, 3.f);
static ConVar sv_commsBansAreGameBans("sv_commsBansAreGameBans", "0", FCVAR_RELEASE, "If set chat bans will be applied as game bans", false, 0.f, true, 1.f);

bool SV_ShouldApplyTextChatGlobalMutes()
{
	const int globalBanSetting = sv_applyGlobalCommsBans.GetInt();
	if (globalBanSetting == 1 || globalBanSetting == 3)
		return true;
	return false;
}

bool SV_ShouldApplyVoiceChatGlobalMutes()
{
	const int globalBanSetting = sv_applyGlobalCommsBans.GetInt();
	if (globalBanSetting >= 2)
		return true;
	return false;
}

static bool SV_GlobalCommsBansEnabled()
{
	if (sv_applyGlobalCommsBans.GetInt() != 0)
		return true;
	return false;
}

static void SV_HandleConnectBan(CClient* const pClient, const char* const pszReason, const char* const pszIpStr, const int nPort, const PlatformUserId_t nUserID)
{
	pClient->Disconnect(Reputation_t::REP_MARK_BAD, "%s", pszReason);
	Warning(eDLL_T::SERVER, "Removed client '[%s]:%i' from slot #%i ('%llu' is banned globally!)\n",
		pszIpStr, nPort, pClient->GetUserID(), nUserID);
}

static void SV_HandleCommunicationBan(CClient* const pClient, const char* const pszReason, const char* const pszExpiry, const char* const pszIpStr, const int nPort, const PlatformUserId_t nUserID)
{
	const int nUserId = pClient->GetUserID();
	CClientExtended* const pClientExtended = pClient->GetClientExtended();

	if (sv_commsBansAreGameBans.GetBool())
	{
		pClient->Disconnect(Reputation_t::REP_MARK_BAD, "%s", pszReason);
		Warning(eDLL_T::SERVER, "Removed client '[%s]:%i' from slot #%i ('%llu' is communication banned and communication bans are treated as game bans!)\n",
			pszIpStr, nPort, nUserId, nUserID);
	}
	else
	{
		DevMsg(eDLL_T::SERVER, "Muting client '[%s]:%i' from slot #%i ('%llu' is communication banned!)\n",
			pszIpStr, nPort, nUserId, nUserID);
	}

	pClientExtended->SetClientIsCommsBanned(true);
	pClientExtended->SetCommsBanInfo(pszReason, pszExpiry);
}

//-----------------------------------------------------------------------------
// Purpose: checks if particular client is banned on the comp server
//-----------------------------------------------------------------------------
void SV_CheckForBanAndDisconnect(CClient* const pClient, const string& svIPAddr,
	const PlatformUserId_t nUserID, const string& svPersonaName, const int nPort)
{
	Assert(pClient != nullptr);

	string svError;
	string expiry;
	CBanSystem::Banned_t::BanType_e banType = CBanSystem::Banned_t::CONNECT;
	
	const bool bCompBanned = g_Spire.CheckForBan(svIPAddr, nUserID, svPersonaName, svError, banType, expiry);

	if (bCompBanned)
	{
		g_TaskQueue.Dispatch([pClient, svError, svIPAddr, nUserID, nPort, banType, expiry]
			{
				// Make sure client isn't already disconnected,
				// and that if there is a valid netchannel, that
				// it hasn't been taken by a different client by
				// the time this task is getting executed.
				const CNetChan* const pChan = pClient->GetNetChan();
				if (pChan && pClient->GetPlatformUserId() == nUserID)
				{
					switch (banType)
					{
					case CBanSystem::Banned_t::CONNECT:
					{
						SV_HandleConnectBan(pClient, svError.c_str(), svIPAddr.c_str(), nPort, nUserID);
						break;
					}
					case CBanSystem::Banned_t::COMMUNICATION:
					{
						if(SV_GlobalCommsBansEnabled())
							SV_HandleCommunicationBan(pClient, svError.c_str(), expiry.c_str(), svIPAddr.c_str(), nPort, nUserID);
						break;
					default:
						break;
					}
					}
				}
			}, 0);
	}
}

//-----------------------------------------------------------------------------
// Purpose: checks if particular client is banned on the master server
//-----------------------------------------------------------------------------
void SV_ProcessBulkCheck(const CBanSystem::BannedList_t* const pBannedVec)
{
	CBanSystem::BannedList_t* outBannedVec = nullptr;

	if (!g_Spire.GetBannedList(*pBannedVec, &outBannedVec))
		return;

	g_TaskQueue.Dispatch([outBannedVec]
		{
			SV_CheckClientsForBan(outBannedVec);
			delete outBannedVec;
		}, 0);
}

//-----------------------------------------------------------------------------
// Purpose: creates a snapshot of the currently connected clients
// Input: *pBannedVec - if passed, will check for bans and kick the clients
//-----------------------------------------------------------------------------
void SV_CheckClientsForBan(const CBanSystem::BannedList_t* const pBannedVec /*= nullptr*/)
{
	Assert(ThreadInMainThread());

	CBanSystem::BannedList_t* bannedVec = !pBannedVec 
		? new CBanSystem::BannedList_t 
		: nullptr;

	for (int c = 0; c < gpGlobals->maxClients; c++) // Loop through all possible client instances.
	{
		CClient* const pClient = g_pServer->GetClient(c);
		const CNetChan* const pNetChan = pClient->GetNetChan();

		if (!pNetChan)
			continue;

		if (!pClient->IsConnected())
			continue;

		if (pNetChan->GetRemoteAddress().IsLoopback())
			continue;

		CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());

		// Bots shouldn't be checked for bans because these are added by the
		// server host.
		if (pPlayer && pPlayer->IsBot())
			continue;

		const char* const szIPAddr = pNetChan->GetAddress(true);
		const PlatformUserId_t nUserID = pClient->GetPlatformUserId();

		// If no banned list was provided, build one with all clients
		// on the server. This will be used for bulk checking so live
		// bans could be performed, as this function is called periodically.
		if (bannedVec)
			bannedVec->AddToTail(CBanSystem::Banned_t(szIPAddr, nUserID));
		else
		{
			// Check if current client is within provided banned list, and
			// prune if so...
			FOR_EACH_VEC(*pBannedVec, i)
			{
				const CBanSystem::Banned_t& banned = (*pBannedVec)[i];

				//If this ban isnt for this client then we check the next
				if (banned.m_UserID != pClient->GetPlatformUserId())
					continue;

				const int nPort = pNetChan->GetPort();

				//What ban type do we have for this client
				switch (banned.m_BanType)
				{
				case CBanSystem::Banned_t::CONNECT:
				{
					SV_HandleConnectBan(pClient, banned.m_Address.String(), szIPAddr, nPort, nUserID);
					break;
				}
				case CBanSystem::Banned_t::COMMUNICATION:
				{
					//Does the host have the comms ban system on and is our client already banned, no point rebanning them if they are
					if (SV_GlobalCommsBansEnabled() && (!pClient->GetClientExtended()->IsClientCommsBanned() || sv_commsBansAreGameBans.GetBool()))
						SV_HandleCommunicationBan(pClient, banned.m_Address.String(), banned.m_BanExpiry.Get(), szIPAddr, nPort, nUserID);
					break;
				}
				//Unknown ban type
				default:
					break;
				}

				//Since we have handled this client we can move onto the next one
				break;
			}
		}
	}

	if (bannedVec && !bannedVec->IsEmpty())
	{
		std::thread bulkCheck([bannedVec]()
			{
				SV_ProcessBulkCheck(bannedVec);
				delete bannedVec;
			});

		bulkCheck.detach();
	}
	else if (bannedVec)
	{
		delete bannedVec;
		bannedVec = nullptr;
	}
}

static string SV_HashPasswordTag(const char* const pszPassword)
{
	if (!pszPassword || !pszPassword[0])
		return string();
	uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszPassword); *p; ++p)
	{
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return Format("pw:%016llx", h);
}

//-----------------------------------------------------------------------------
// Purpose: loads the game.dll
//-----------------------------------------------------------------------------
void SV_InitGameDLL()
{
	// Mirror current password into the server filter used by the engine's connect validation (hashed tag).
	if (ConVar* const pFilter = g_pCVar->FindVar("serverFilter"))
	{
		const char* pw = "";
		if (ConVar* const pPw = g_pCVar->FindVar("sv_password"))
			pw = pPw->GetString();
		const string tagged = SV_HashPasswordTag(pw);
		pFilter->SetValue(tagged.c_str());
	}
	v_SV_InitGameDLL();
}

//-----------------------------------------------------------------------------
// Purpose: release resources associated with extension DLLs.
//-----------------------------------------------------------------------------
void SV_ShutdownGameDLL()
{
	v_SV_ShutdownGameDLL();
}

//-----------------------------------------------------------------------------
// Purpose: activates the server
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool SV_ActivateServer()
{
	// Materialize ziprail chains before the original builds baselines.
	ZiprailDedi_MaterializeAllPending("SV_ActivateServer");

	const bool ret = v_SV_ActivateServer();

	// Dump stored instance-baseline blobs after SV_CreateBaseline.
	static bool s_ibDumpedDedi = false;
	if (!s_ibDumpedDedi && g_pServer)
	{
		s_ibDumpedDedi = true;
		__try
		{
			INetworkStringTable* const ib = g_pServer->GetInstanceBaselineTable();
			if (ib)
			{
				const int nStrings = ib->GetNumStrings();
				Warning(eDLL_T::SERVER, "[IB-DUMP-DEDI] instancebaseline numStrings=%d\n", nStrings);
				for (int i = 0; i < nStrings && i < 256; ++i)
				{
					const char* const key = ib->GetString(i);
					int len = 0;
					const uint8_t* const ud = reinterpret_cast<const uint8_t*>(ib->GetStringUserData(i, &len));
					// len units (bits vs bytes) unknown across engines; clamp the
					// hex byte count to (len+7)/8 so a small blob is never over-read.
					int dn = (len > 0) ? (len + 7) / 8 : 0;
					if (dn > 32) dn = 32;
					char hex[3 * 32 + 1] = {};
					if (ud)
					{
						for (int b = 0; b < dn; ++b)
						{
							char t[4];
							snprintf(t, sizeof(t), "%02X ", ud[b]);
							strcat_s(hex, sizeof(hex), t);
						}
					}
					Warning(eDLL_T::SERVER, "[IB-DUMP-DEDI] [%3d] key='%s' len=%d hex: %s\n",
						i, key ? key : "?", len, hex);
				}
			}
			else
			{
				Warning(eDLL_T::SERVER, "[IB-DUMP-DEDI] instancebaseline table NULL\n");
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Warning(eDLL_T::SERVER, "[IB-DUMP-DEDI] exception dumping baselines\n");
		}
	}

	return ret;
}

//-----------------------------------------------------------------------------
// Purpose: returns whether voice data can be broadcasted from the server
//-----------------------------------------------------------------------------
bool SV_CanBroadcastVoice()
{
	if (IsPartyDedi())
		return false;

	if (IsTrainingDedi())
		return false;

	if (!sv_voiceenable->GetBool())
		return false;

	if (gpGlobals->maxClients <= 0)
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: relays voice data to other clients
//-----------------------------------------------------------------------------
void SV_BroadcastVoiceData(CClient* const cl, const int nBytes, char* const data)
{
	if (!SV_CanBroadcastVoice())
		return;

	const bool bShouldApplyGlobalMutes = SV_ShouldApplyVoiceChatGlobalMutes();
	const bool bBannedClientsCanHearOtherClients = sv_commsBannedClientsCanReceiveComms.GetBool();

	SVC_VoiceData voiceData(cl->GetUserID(), nBytes, data);

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CClient* const pClient = g_pServer->GetClient(i);

		// is this client fully connected
		if (pClient->GetSignonState() != SIGNONSTATE::SIGNONSTATE_FULL)
			continue;

		//If the client is communication banned and the server has decidecd that players who are comms banned cant hear other players, skip broadcasting to them
		if (!bBannedClientsCanHearOtherClients && bShouldApplyGlobalMutes &&  pClient->GetClientExtended()->IsClientCommsBanned())
			continue;

		// is this client the sender
		if (pClient == cl && !sv_voiceEcho->GetBool())
			continue;

		// is this client on the sender's team
		if (pClient->GetTeamNum() != cl->GetTeamNum() && !sv_alltalk->GetBool())
			continue;

		//if (voice_noxplat->GetBool && cl->GetXPlatID != pClient->GetXPlatID)
		//	if ((cl->GetXPlatID -1) > 1 || (pClient->GetXPlatID -1) > 1)
		// continue;

		CNetChan* const pNetChan = pClient->GetNetChan();

		if (!pNetChan)
			continue;

		// if voice stream has enough space for new data
		if (pNetChan->GetStreamVoice().GetNumBitsLeft() >= 8 * nBytes + 96)
			pClient->SendNetMsgEx(&voiceData, false, false, true);
	}
}

//-----------------------------------------------------------------------------
// Purpose: relays durango voice data to other clients
//-----------------------------------------------------------------------------
void SV_BroadcastDurangoVoiceData(CClient* const cl, const int nBytes, char* const data,
	const int nXid, const int unknown, const bool useVoiceStream, const bool skipXidCheck)
{
	if (!SV_CanBroadcastVoice())
		return;

	const bool bShouldApplyGlobalMutes = SV_ShouldApplyVoiceChatGlobalMutes();
	const bool bBannedClientsCanHearOtherClients = sv_commsBannedClientsCanReceiveComms.GetBool();

	(void)useVoiceStream;
	SVC_DurangoVoiceData voiceData(cl->GetUserID(), nBytes, data, unknown, true);

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CClient* const pClient = g_pServer->GetClient(i);

		// is this client fully connected
		if (pClient->GetSignonState() != SIGNONSTATE::SIGNONSTATE_FULL)
			continue;

		//If the client is communication banned and the server has decidecd that players who are comms banned cant other players, skip broadcasting to them
		if (!bBannedClientsCanHearOtherClients && bShouldApplyGlobalMutes && pClient->GetClientExtended()->IsClientCommsBanned())
			continue;

		// is this client the sender
		if (pClient == cl && !sv_voiceEcho->GetBool())
			continue;

		if (!skipXidCheck && i != nXid)
			continue;

		// is this client on the sender's team
		if (pClient->GetTeamNum() != cl->GetTeamNum() && !sv_alltalk->GetBool())
		{
			// NOTE: on Durango packets, the game appears to bypass the team
			// check if 'useVoiceStream' is false, thus forcing the usage
			// of the reliable stream. Omitted the check as it appears that
			// could be exploited to transmit voice to other teams while cvar
			// 'sv_alltalk' is unset.
			continue;
		}

		// NOTE: xplat code checks disabled; CClient::GetXPlatID seems to be
		// an enumeration of platforms, but the enum hasn't been reversed yet.
		//if (voice_noxplat->GetBool && cl->GetXPlatID != pClient->GetXPlatID)
		//	if ((cl->GetXPlatID - 1) > 1 || (pClient->GetXPlatID - 1) > 1)
		// continue;

		CNetChan* const pNetChan = pClient->GetNetChan();

		if (!pNetChan)
			continue;

		const bf_write& stream = pNetChan->GetStreamVoice();

		// if stream has enough space for new data
		if (stream.GetNumBitsLeft() >= 8 * nBytes + 34)
			pClient->SendNetMsgEx(&voiceData, false, false, true);
	}
}
#endif // CLIENT_DLL
