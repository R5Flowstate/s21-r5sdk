#if defined(CLIENT_DLL)
//=====================================================================================//
//
// Purpose: Implementation of the CBanSystem class.
//
// $NoKeywords: $
//=====================================================================================//

#include "core/stdafx.h"
#include "tier1/strtools.h"
#include "tier2/jsonutils.h"
#include "engine/net.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "filesystem/filesystem.h"
#include "networksystem/bansystem.h"
#include "game/server/gameinterface.h"

//-----------------------------------------------------------------------------
// Purpose: loads and parses the banned list
//-----------------------------------------------------------------------------
void CBanSystem::LoadList(void)
{
	// IFileSystem optimal-I/O vtable is unsafe on S21; FileSystem_ReadAll uses IBaseFileSystem::Read.
	ssize_t nFileSize = 0;
	char* const pBuf = FileSystem_ReadAll("banlist.json", "PLATFORM", &nFileSize);
	if (!pBuf)
	{
		// Missing or empty banlist is fine on a fresh install -- nothing to do.
		return;
	}

	rapidjson::Document document;
	if (document.Parse(pBuf, nFileSize).HasParseError())
	{
		Error(eDLL_T::SERVER, 0, "%s: JSON parse error at position %zu: %s\n",
			__FUNCTION__, document.GetErrorOffset(), rapidjson::GetParseError_En(document.GetParseError()));
		delete[] pBuf;

		return;
	}

	// Buffer is no longer needed.
	delete[] pBuf;

	if (!document.IsArray())
	{
		Error(eDLL_T::SERVER, 0, "%s: JSON root was not an array\n", __FUNCTION__);
		return;
	}

	ssize_t currIdx = -1;

	for (const rapidjson::Value& entry : document.GetArray())
	{
		currIdx++;

		if (entry.IsUint64())
		{
			const PlatformUserId_t userId = entry.GetUint64();

			if (userId == 0)
			{
				Warning(eDLL_T::SERVER, "%s: user id (%llu) at index #%zd is zero!\n", __FUNCTION__, userId, currIdx);
				continue;
			}

			m_bannedIdList.insert(userId);
			continue;
		}

		if (entry.IsString())
		{
			netadr_t adr;
			const char* const adrStr = entry.GetString();

			if (!adr.SetFromString(adrStr, true))
			{
				Warning(eDLL_T::SERVER, "%s: IP Address (%s) at index #%zd is invalid!\n", __FUNCTION__, currIdx, adrStr);
				continue;
			}

			m_bannedIpList.insert(adr.GetIP());
			continue;
		}

		Error(eDLL_T::SERVER, 0, "%s: Entry #%zd is of type %s, but code expects type %s or %s\n", __FUNCTION__, currIdx,
			JSON_TypeToString(JSON_ExtractType(entry)), JSON_TypeToString(JSONFieldType_e::kUint64), JSON_TypeToString(JSONFieldType_e::kString));
	}
}

//-----------------------------------------------------------------------------
// Purpose: saves the banned list
//-----------------------------------------------------------------------------
void CBanSystem::SaveList(void) const
{
	FileHandle_t pFile = FileSystem()->Open("banlist.json", "wb", "PLATFORM");
	if (!pFile)
	{
		Error(eDLL_T::SERVER, NO_ERROR, "%s - Unable to write to '%s' (read-only?)\n", __FUNCTION__, "banlist.json");
		return;
	}

	rapidjson::Document document;
	document.SetArray();

	rapidjson::Document::AllocatorType& allocator = document.GetAllocator();

	for (const PlatformUserId_t id : m_bannedIdList)
	{
		document.PushBack(id, allocator);
	}

	ssize_t idx = -1;

	for (const IPv6Wrapper_s& ip : m_bannedIpList)
	{
		idx++;
		char adrBuf[INET6_ADDRSTRLEN];
		
		if (!inet_ntop(AF_INET6, &ip.adr, adrBuf, sizeof(adrBuf)))
		{
			Error(eDLL_T::SERVER, NO_ERROR, "%s - Unable to convert listed network address #%zd for write -- skipping...\n", __FUNCTION__, idx);
			continue; // Should never happen.
		}

		document.PushBack(rapidjson::Value(adrBuf, strlen(adrBuf), allocator), allocator);
	}

	rapidjson::StringBuffer buffer;
	JSON_DocumentToBufferDeserialize(document, buffer);

	FileSystem()->Write(buffer.GetString(), buffer.GetSize(), pFile);
	FileSystem()->Close(pFile);
}

void CBanSystem::Clear()
{
	m_bannedIdList.clear();
	m_bannedIpList.clear();
}

//-----------------------------------------------------------------------------
// Purpose: adds a banned player entry to the banned list
// Input: *ipAddress - 
// userId - 
//-----------------------------------------------------------------------------
bool CBanSystem::AddEntry(const netadr_t* const adr, const PlatformUserId_t userId)
{
	return AddEntry(adr->GetIP(), userId);
}

bool CBanSystem::AddEntry(const in6_addr* const adr, const PlatformUserId_t userId)
{
	bool userIdAdded = false;

	if (userId)
		userIdAdded = m_bannedIdList.insert(userId).second;

	bool adrAdded = false;

	if (adr)
		adrAdded = m_bannedIpList.insert(adr).second;

	return userIdAdded || adrAdded;
}

//-----------------------------------------------------------------------------
// Purpose: deletes an entry in the banned list
// Input: *ipAddress - 
// userId - 
//-----------------------------------------------------------------------------
bool CBanSystem::DeleteEntry(const netadr_t* const adr, const PlatformUserId_t userId)
{
	return DeleteEntry(adr->GetIP(), userId);
}

bool CBanSystem::DeleteEntry(const in6_addr* const adr, const PlatformUserId_t userId)
{
	bool userIdRemoved = false;

	if (userId)
		userIdRemoved = m_bannedIdList.erase(userId) != 0;

	bool adrRemoved = false;

	if (adr)
		adrRemoved = m_bannedIpList.erase(adr) != 0;

	return userIdRemoved || adrRemoved;
}

//-----------------------------------------------------------------------------
// Purpose: checks if specified ip address or user id is banned
// Input: *ipAddress - 
// userId - 
// Output: true if banned, false if not banned
//-----------------------------------------------------------------------------
bool CBanSystem::IsBanned(const netadr_t* const adr, const PlatformUserId_t userId) const
{
	return IsBanned(adr->GetIP(), userId);
}

bool CBanSystem::IsBanned(const in6_addr* const adr, const PlatformUserId_t userId) const
{
	if (userId && m_bannedIdList.find(userId) != m_bannedIdList.end())
		return true;

	if (adr && m_bannedIpList.find(adr) != m_bannedIpList.end())
		return true;

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given name
// Input: *playerName - 
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::KickPlayerByName(const char* playerName, const char* reason)
{
	if (!VALID_CHARSTAR(playerName))
		return;

	AuthorPlayerByName(playerName, false, reason);
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given handle or id
// Input: *playerHandle - 
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::KickPlayerById(const char* playerHandle, const char* reason)
{
	if (!VALID_CHARSTAR(playerHandle))
		return;

	AuthorPlayerById(playerHandle, false, reason);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given name
// Input: *playerName - 
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::BanPlayerByName(const char* playerName, const char* reason)
{
	if (!VALID_CHARSTAR(playerName))
		return;

	AuthorPlayerByName(playerName, true, reason);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given handle or id
// Input: *playerHandle - 
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::BanPlayerById(const char* playerHandle, const char* reason)
{
	if (!VALID_CHARSTAR(playerHandle))
		return;

	AuthorPlayerById(playerHandle, true, reason);
}

static bool BanSystem_ConvertAddress(const char* const address, in6_addr* const addr)
{
	const int ret = inet_pton(AF_INET6, address, addr);

	if (ret != 1)
	{
		Warning(eDLL_T::SERVER, "%s: Failed to convert provided network address \"%s\" (%s)\n",
			__FUNCTION__, address, ret == -1 ? NET_ErrorString(WSAGetLastError()) : "invalid format");
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: unbans a player by given user id or ip address
// Input: *criteria - 
//-----------------------------------------------------------------------------
void CBanSystem::UnbanPlayer(const char* criteria)
{
	bool bSave = false;

	if (V_IsAllDigit(criteria)) // Check if we have an ip address or user id.
	{
		char* pEnd = nullptr;
		const uint64_t nTargetID = strtoull(criteria, &pEnd, 10);

		if (DeleteEntry((in6_addr*)nullptr, nTargetID)) // Delete ban entry.
		{
			bSave = true;
		}
	}
	else
	{
		in6_addr address;

		if (BanSystem_ConvertAddress(criteria, &address))
		{
			if (DeleteEntry(&address, 0)) // Delete ban entry.
			{
				bSave = true;
			}
		}
	}

	if (bSave)
	{
		SaveList(); // Save modified vector to file.
		Msg(eDLL_T::SERVER, "Removed '%s' from banned list\n", criteria);
	}
}

//-----------------------------------------------------------------------------
// Purpose: authors player by given name
// Input: *playerName - 
// shouldBan - (only kicks if false)
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::AuthorPlayerByName(const char* playerName, const bool shouldBan, const char* reason)
{
	// The client has no CServer and therefore no clients to author. Loud so a
	// stray call from a shared caller shows up instead of silently doing nothing.
	NOTE_UNUSED(shouldBan);
	NOTE_UNUSED(reason);
	Warning(eDLL_T::CLIENT, "[BANSYS] %s('%s') ignored: no server on the client\n",
		__FUNCTION__, playerName ? playerName : "");
}

static bool BanSystem_CompareAddress(const in6_addr* const a, const in6_addr* const b)
{
	return IN6_ADDR_EQUAL(a, b);
}

//-----------------------------------------------------------------------------
// Purpose: authors player by given user id or ip address
// Input: *playerHandle - 
// shouldBan - (only kicks if false)
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::AuthorPlayerById(const char* playerHandle, const bool shouldBan, const char* reason)
{
	// The client has no CServer and therefore no clients to author. Loud so a
	// stray call from a shared caller shows up instead of silently doing nothing.
	NOTE_UNUSED(shouldBan);
	NOTE_UNUSED(reason);
	Warning(eDLL_T::CLIENT, "[BANSYS] %s('%s') ignored: no server on the client\n",
		__FUNCTION__, playerHandle ? playerHandle : "");
}

///////////////////////////////////////////////////////////////////////////////
// Console command handlers
///////////////////////////////////////////////////////////////////////////////

enum KickType_e
{
	KICK_NAME = 0,
	KICK_ID,
	BAN_NAME,
	BAN_ID
};

static void _Author_Client_f(const CCommand& args, const KickType_e type)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const char* szReason = args.ArgC() > 2 ? args.Arg(2) : nullptr;

	switch (type)
	{
	case KICK_NAME:
	{
		g_BanSystem.KickPlayerByName(args.Arg(1), szReason);
		break;
	}
	case KICK_ID:
	{
		g_BanSystem.KickPlayerById(args.Arg(1), szReason);
		break;
	}
	case BAN_NAME:
	{
		g_BanSystem.BanPlayerByName(args.Arg(1), szReason);
		break;
	}
	case BAN_ID:
	{
		g_BanSystem.BanPlayerById(args.Arg(1), szReason);
		break;
	}
	default:
	{
		// Code bug.
		Assert(0);
	}
	}
}
static void Host_Kick_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::KICK_NAME);
}
static void Host_KickID_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::KICK_ID);
}
static void Host_Ban_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::BAN_NAME);
}
static void Host_BanID_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::BAN_ID);
}
static void Host_Unban_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	g_BanSystem.UnbanPlayer(args.Arg(1));
}
static void Host_ReloadBanList_f()
{
	g_BanSystem.Clear();
	g_BanSystem.LoadList(); // Reload banned list.
}

static ConCommand kick("kick", Host_Kick_f, "Kick a client from the server by user name", FCVAR_RELEASE, nullptr, "kick \"<userId>\"");
static ConCommand kickid("kickid", Host_KickID_f, "Kick a client from the server by handle, user id or ip address", FCVAR_RELEASE, nullptr, "kickid \"<handle>\"/\"<userId>/<ipAddress>\"");
static ConCommand ban("ban", Host_Ban_f, "Bans a client from the server by user name", FCVAR_RELEASE, nullptr, "ban <userId>");
static ConCommand banid("banid", Host_BanID_f, "Bans a client from the server by handle, user id or ip address", FCVAR_RELEASE, nullptr, "banid \"<handle>\"/\"<userId>/<ipAddress>\"");
static ConCommand unban("unban", Host_Unban_f, "Unbans a client from the server by user id or ip address", FCVAR_RELEASE, nullptr, "unban \"<userId>\"/\"<ipAddress>\"");
static ConCommand reload_banlist("banlist_reload", Host_ReloadBanList_f, "Reloads the banned list", FCVAR_RELEASE);

///////////////////////////////////////////////////////////////////////////////
CBanSystem g_BanSystem;
#else // !CLIENT_DLL
//=====================================================================================//
//
// Purpose: Implementation of the CBanSystem class.
//
// $NoKeywords: $
//=====================================================================================//

#include "core/stdafx.h"
#include "tier1/strtools.h"
#include "tier2/jsonutils.h"
#include "engine/net.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "filesystem/filesystem.h"
#include "networksystem/bansystem.h"
#include "game/server/gameinterface.h"

//-----------------------------------------------------------------------------
// Purpose: loads and parses the banned list
//-----------------------------------------------------------------------------
void CBanSystem::LoadList(void)
{
	FileHandle_t pFile = FileSystem()->Open("banlist.json", "rb", "PLATFORM");
	if (!pFile)
		return;

	const ssize_t nFileSize = FileSystem()->Size(pFile);

	if (nFileSize <= 0)
	{
		Error(eDLL_T::SERVER, 0, "%s: Banned list file is empty\n", __FUNCTION__);
		FileSystem()->Close(pFile);

		return;
	}

	const u64 nBufSize = FileSystem()->GetOptimalReadSize(pFile, nFileSize+2);
	char* const pBuf = (char*)FileSystem()->AllocOptimalReadBuffer(pFile, nBufSize, 0);

	const ssize_t nRead = FileSystem()->ReadEx(pBuf, nBufSize, nFileSize, pFile);
	FileSystem()->Close(pFile);

	if (nRead == 0)
	{
		Error(eDLL_T::SERVER, 0, "%s: Banned list file read failure\n", __FUNCTION__);
		FileSystem()->FreeOptimalReadBuffer(pBuf);

		return;
	}

	pBuf[nFileSize] = '\0'; // Null terminate the string buffer containing our banned list.
	pBuf[nFileSize+1] = '\0'; // Double null terminating in case this is an unicode file.

	rapidjson::Document document;
	if (document.Parse(pBuf, nRead).HasParseError())
	{
		Error(eDLL_T::SERVER, 0, "%s: JSON parse error at position %zu: %s\n",
			__FUNCTION__, document.GetErrorOffset(), rapidjson::GetParseError_En(document.GetParseError()));
		FileSystem()->FreeOptimalReadBuffer(pBuf);

		return;
	}

	// Buffer is no longer needed.
	FileSystem()->FreeOptimalReadBuffer(pBuf);

	if (!document.IsArray())
	{
		Error(eDLL_T::SERVER, 0, "%s: JSON root was not an array\n", __FUNCTION__);
		return;
	}

	ssize_t currIdx = -1;

	for (const rapidjson::Value& entry : document.GetArray())
	{
		currIdx++;

		if (entry.IsUint64())
		{
			const PlatformUserId_t userId = entry.GetUint64();

			if (userId == 0)
			{
				Warning(eDLL_T::SERVER, "%s: user id (%llu) at index #%zd is zero!\n", __FUNCTION__, userId, currIdx);
				continue;
			}

			m_bannedIdList.insert(userId);
			continue;
		}

		if (entry.IsString())
		{
			netadr_t adr;
			const char* const adrStr = entry.GetString();

			if (!adr.SetFromString(adrStr, true))
			{
				Warning(eDLL_T::SERVER, "%s: IP Address (%s) at index #%zd is invalid!\n", __FUNCTION__, currIdx, adrStr);
				continue;
			}

			m_bannedIpList.insert(adr.GetIP());
			continue;
		}

		Error(eDLL_T::SERVER, 0, "%s: Entry #%zd is of type %s, but code expects type %s or %s\n", __FUNCTION__, currIdx,
			JSON_TypeToString(JSON_ExtractType(entry)), JSON_TypeToString(JSONFieldType_e::kUint64), JSON_TypeToString(JSONFieldType_e::kString));
	}
}

//-----------------------------------------------------------------------------
// Purpose: saves the banned list
//-----------------------------------------------------------------------------
void CBanSystem::SaveList(void) const
{
	FileHandle_t pFile = FileSystem()->Open("banlist.json", "wb", "PLATFORM");
	if (!pFile)
	{
		Error(eDLL_T::SERVER, NO_ERROR, "%s - Unable to write to '%s' (read-only?)\n", __FUNCTION__, "banlist.json");
		return;
	}

	rapidjson::Document document;
	document.SetArray();

	rapidjson::Document::AllocatorType& allocator = document.GetAllocator();

	for (const PlatformUserId_t id : m_bannedIdList)
	{
		document.PushBack(id, allocator);
	}

	ssize_t idx = -1;

	for (const IPv6Wrapper_s& ip : m_bannedIpList)
	{
		idx++;
		char adrBuf[INET6_ADDRSTRLEN];
		
		if (!inet_ntop(AF_INET6, &ip.adr, adrBuf, sizeof(adrBuf)))
		{
			Error(eDLL_T::SERVER, NO_ERROR, "%s - Unable to convert listed network address #%zd for write -- skipping...\n", __FUNCTION__, idx);
			continue; // Should never happen.
		}

		document.PushBack(rapidjson::Value(adrBuf, strlen(adrBuf), allocator), allocator);
	}

	rapidjson::StringBuffer buffer;
	JSON_DocumentToBufferDeserialize(document, buffer);

	FileSystem()->Write(buffer.GetString(), buffer.GetSize(), pFile);
	FileSystem()->Close(pFile);
}

void CBanSystem::Clear()
{
	m_bannedIdList.clear();
	m_bannedIpList.clear();
}

//-----------------------------------------------------------------------------
// Purpose: adds a banned player entry to the banned list
// Input: *ipAddress - 
// userId - 
//-----------------------------------------------------------------------------
bool CBanSystem::AddEntry(const netadr_t* const adr, const PlatformUserId_t userId)
{
	return AddEntry(adr->GetIP(), userId);
}

bool CBanSystem::AddEntry(const in6_addr* const adr, const PlatformUserId_t userId)
{
	bool userIdAdded = false;

	if (userId)
		userIdAdded = m_bannedIdList.insert(userId).second;

	bool adrAdded = false;

	if (adr)
		adrAdded = m_bannedIpList.insert(adr).second;

	return userIdAdded || adrAdded;
}

//-----------------------------------------------------------------------------
// Purpose: deletes an entry in the banned list
// Input: *ipAddress - 
// userId - 
//-----------------------------------------------------------------------------
bool CBanSystem::DeleteEntry(const netadr_t* const adr, const PlatformUserId_t userId)
{
	return DeleteEntry(adr->GetIP(), userId);
}

bool CBanSystem::DeleteEntry(const in6_addr* const adr, const PlatformUserId_t userId)
{
	bool userIdRemoved = false;

	if (userId)
		userIdRemoved = m_bannedIdList.erase(userId) != 0;

	bool adrRemoved = false;

	if (adr)
		adrRemoved = m_bannedIpList.erase(adr) != 0;

	return userIdRemoved || adrRemoved;
}

//-----------------------------------------------------------------------------
// Purpose: checks if specified ip address or user id is banned
// Input: *ipAddress - 
// userId - 
// Output: true if banned, false if not banned
//-----------------------------------------------------------------------------
bool CBanSystem::IsBanned(const netadr_t* const adr, const PlatformUserId_t userId) const
{
	return IsBanned(adr->GetIP(), userId);
}

bool CBanSystem::IsBanned(const in6_addr* const adr, const PlatformUserId_t userId) const
{
	if (userId && m_bannedIdList.find(userId) != m_bannedIdList.end())
		return true;

	if (adr && m_bannedIpList.find(adr) != m_bannedIpList.end())
		return true;

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given name
// Input: *playerName - 
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::KickPlayerByName(const char* playerName, const char* reason)
{
	if (!VALID_CHARSTAR(playerName))
		return;

	AuthorPlayerByName(playerName, false, reason);
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given handle or id
// Input: *playerHandle - 
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::KickPlayerById(const char* playerHandle, const char* reason)
{
	if (!VALID_CHARSTAR(playerHandle))
		return;

	AuthorPlayerById(playerHandle, false, reason);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given name
// Input: *playerName - 
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::BanPlayerByName(const char* playerName, const char* reason)
{
	if (!VALID_CHARSTAR(playerName))
		return;

	AuthorPlayerByName(playerName, true, reason);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given handle or id
// Input: *playerHandle - 
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::BanPlayerById(const char* playerHandle, const char* reason)
{
	if (!VALID_CHARSTAR(playerHandle))
		return;

	AuthorPlayerById(playerHandle, true, reason);
}

static bool BanSystem_ConvertAddress(const char* const address, in6_addr* const addr)
{
	const int ret = inet_pton(AF_INET6, address, addr);

	if (ret != 1)
	{
		Warning(eDLL_T::SERVER, "%s: Failed to convert provided network address \"%s\" (%s)\n",
			__FUNCTION__, address, ret == -1 ? NET_ErrorString(WSAGetLastError()) : "invalid format");
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: unbans a player by given user id or ip address
// Input: *criteria - 
//-----------------------------------------------------------------------------
void CBanSystem::UnbanPlayer(const char* criteria)
{
	bool bSave = false;

	if (V_IsAllDigit(criteria)) // Check if we have an ip address or user id.
	{
		char* pEnd = nullptr;
		const uint64_t nTargetID = strtoull(criteria, &pEnd, 10);

		if (DeleteEntry((in6_addr*)nullptr, nTargetID)) // Delete ban entry.
		{
			bSave = true;
		}
	}
	else
	{
		in6_addr address;

		if (BanSystem_ConvertAddress(criteria, &address))
		{
			if (DeleteEntry(&address, 0)) // Delete ban entry.
			{
				bSave = true;
			}
		}
	}

	if (bSave)
	{
		SaveList(); // Save modified vector to file.
		Msg(eDLL_T::SERVER, "Removed '%s' from banned list\n", criteria);
	}
}

//-----------------------------------------------------------------------------
// Purpose: authors player by given name
// Input: *playerName - 
// shouldBan - (only kicks if false)
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::AuthorPlayerByName(const char* playerName, const bool shouldBan, const char* reason)
{
	Assert(VALID_CHARSTAR(playerName));
	bool bDisconnect = false;
	bool bSave = false;

	if (!reason)
		reason = shouldBan ? "Banned from server" : "Kicked from server";

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CClient* const pClient = g_pServer->GetClient(i);
		const CNetChan* const pNetChan = pClient->GetNetChan();

		if (!pNetChan)
			continue;

		if (strlen(pNetChan->GetName()) > 0)
		{
			if (strcmp(playerName, pNetChan->GetName()) == NULL) // Our wanted name?
			{
				if (shouldBan && AddEntry(&pNetChan->GetRemoteAddress(), pClient->GetPlatformUserId()) && !bSave)
					bSave = true;

				pClient->Disconnect(REP_MARK_BAD, reason);
				bDisconnect = true;
			}
		}
	}

	if (bSave)
	{
		SaveList();
		Msg(eDLL_T::SERVER, "Added '%s' to banned list\n", playerName);
	}
	else if (bDisconnect)
	{
		Msg(eDLL_T::SERVER, "Kicked '%s' from server\n", playerName);
	}
}

static bool BanSystem_CompareAddress(const in6_addr* const a, const in6_addr* const b)
{
	return IN6_ADDR_EQUAL(a, b);
}

//-----------------------------------------------------------------------------
// Purpose: authors player by given user id or ip address
// Input: *playerHandle - 
// shouldBan - (only kicks if false)
// *reason - 
//-----------------------------------------------------------------------------
void CBanSystem::AuthorPlayerById(const char* playerHandle, const bool shouldBan, const char* reason)
{
	Assert(VALID_CHARSTAR(playerHandle));

	bool bOnlyDigits = V_IsAllDigit(playerHandle);
	bool bDisconnect = false;
	bool bSave = false;

	in6_addr playerAdr;

	if (!bOnlyDigits)
	{
		if (!BanSystem_ConvertAddress(playerHandle, &playerAdr))
			return;
	}

	if (!reason)
		reason = shouldBan ? "Banned from server" : "Kicked from server";

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CClient* const pClient = g_pServer->GetClient(i);
		const CNetChan* const pNetChan = pClient->GetNetChan();

		if (!pNetChan)
			continue;

		if (bOnlyDigits)
		{
			char* pEnd = nullptr;
			const uint64_t nTargetID = strtoull(playerHandle, &pEnd, 10);

		if (nTargetID >= MAX_PLAYERS) // Is it a possible user id?
		{
			const PlatformUserId_t nUserID = pClient->GetPlatformUserId();

			if (nUserID != nTargetID)
				continue;
			}
			else // If its not try by handle.
			{
				const edict_t nClientID = pClient->GetHandle();

				if (nClientID != nTargetID)
					continue;
			}

			if (shouldBan && AddEntry(&pNetChan->GetRemoteAddress(), pClient->GetPlatformUserId()) && !bSave)
				bSave = true;

			pClient->Disconnect(REP_MARK_BAD, reason);
			bDisconnect = true;
		}
		else
		{
			if (!BanSystem_CompareAddress(pNetChan->GetRemoteAddress().GetIP(), &playerAdr))
				continue;

			if (shouldBan && AddEntry(&pNetChan->GetRemoteAddress(), pClient->GetPlatformUserId()) && !bSave)
				bSave = true;

			pClient->Disconnect(REP_MARK_BAD, reason);
			bDisconnect = true;
		}
	}

	if (bSave)
	{
		SaveList();
		Msg(eDLL_T::SERVER, "Added '%s' to banned list\n", playerHandle);
	}
	else if (bDisconnect)
	{
		Msg(eDLL_T::SERVER, "Kicked '%s' from server\n", playerHandle);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Console command handlers
///////////////////////////////////////////////////////////////////////////////

enum KickType_e
{
	KICK_NAME = 0,
	KICK_ID,
	BAN_NAME,
	BAN_ID
};

static void _Author_Client_f(const CCommand& args, const KickType_e type)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const char* szReason = args.ArgC() > 2 ? args.Arg(2) : nullptr;

	switch (type)
	{
	case KICK_NAME:
	{
		g_BanSystem.KickPlayerByName(args.Arg(1), szReason);
		break;
	}
	case KICK_ID:
	{
		g_BanSystem.KickPlayerById(args.Arg(1), szReason);
		break;
	}
	case BAN_NAME:
	{
		g_BanSystem.BanPlayerByName(args.Arg(1), szReason);
		break;
	}
	case BAN_ID:
	{
		g_BanSystem.BanPlayerById(args.Arg(1), szReason);
		break;
	}
	default:
	{
		// Code bug.
		Assert(0);
	}
	}
}
static void Host_Kick_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::KICK_NAME);
}
static void Host_KickID_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::KICK_ID);
}
static void Host_Ban_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::BAN_NAME);
}
static void Host_BanID_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::BAN_ID);
}
static void Host_Unban_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	g_BanSystem.UnbanPlayer(args.Arg(1));
}
static void Host_ReloadBanList_f()
{
	g_BanSystem.Clear();
	g_BanSystem.LoadList(); // Reload banned list.
}

static ConCommand kick("kick", Host_Kick_f, "Kick a client from the server by user name", FCVAR_RELEASE, nullptr, "kick \"<userId>\"");
static ConCommand kickid("kickid", Host_KickID_f, "Kick a client from the server by handle, user id or ip address", FCVAR_RELEASE, nullptr, "kickid \"<handle>\"/\"<userId>/<ipAddress>\"");
static ConCommand ban("ban", Host_Ban_f, "Bans a client from the server by user name", FCVAR_RELEASE, nullptr, "ban <userId>");
static ConCommand banid("banid", Host_BanID_f, "Bans a client from the server by handle, user id or ip address", FCVAR_RELEASE, nullptr, "banid \"<handle>\"/\"<userId>/<ipAddress>\"");
static ConCommand unban("unban", Host_Unban_f, "Unbans a client from the server by user id or ip address", FCVAR_RELEASE, nullptr, "unban \"<userId>\"/\"<ipAddress>\"");
static ConCommand reload_banlist("banlist_reload", Host_ReloadBanList_f, "Reloads the banned list", FCVAR_RELEASE);

///////////////////////////////////////////////////////////////////////////////
CBanSystem g_BanSystem;
#endif // CLIENT_DLL
