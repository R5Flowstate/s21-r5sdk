//=============================================================================//
//
// Purpose: Implementation of the spire client.
//
// $NoKeywords: $
//=============================================================================//

#include <core/stdafx.h>
#include <tier0/commandline.h>
#include <tier1/cvar.h>
#include <tier2/curlutils.h>
#include <tier2/jsonutils.h>
#include <networksystem/spire.h>
#include <engine/server/server.h>

//-----------------------------------------------------------------------------
// Console variables
//-----------------------------------------------------------------------------
ConVar spire_matchmaking_enabled("spire_matchmaking_enabled", "1", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Whether to use the Spire matchmaking server");
ConVar spire_matchmaking_hostname("spire_matchmaking_hostname", "play.r5flowstate.org", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Holds the Spire matchmaking hostname");
ConVar spire_host_update_interval("spire_host_update_interval", "5", FCVAR_RELEASE, "Time interval between status updates to the Spire master server", true, 5.f, false, 0.f, "seconds");
// A dedicated server has no host UI to raise this, so an OFFLINE default means
// it never reaches /spire/hosts/publish and never appears in any browser. The
// client half keeps OFFLINE: UIScript_CreateServer writes the value the player
// picked before launching a listen server.
#if defined(CLIENT_DLL)
#define SPIRE_HOST_VISIBILITY_DEFAULT "0"
#else
#define SPIRE_HOST_VISIBILITY_DEFAULT "2"
#endif
ConVar spire_host_visibility("spire_host_visibility", SPIRE_HOST_VISIBILITY_DEFAULT, FCVAR_RELEASE, "Determines the visibility to the Spire master server", true, 0.f, true, 2.f, "0 = Offline, 1 = Hidden, 2 = Public");
ConVar spire_showdebuginfo("spire_showdebuginfo", "0", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Shows debug output for Spire");
// The console treats '//' as a comment, so a hostname carrying a scheme is cut
// at "http:" before it ever reaches the ConVar, and quoting does not survive a
// launch argument. Set this instead and give the hostname without a scheme.
ConVar spire_matchmaking_insecure("spire_matchmaking_insecure", "0", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Reach the Spire matchmaking host over plain HTTP instead of HTTPS (local master servers)");

//-----------------------------------------------------------------------------
// Purpose: checks if server listing fields are valid, and sets outGameServer
// Input  : &value - 
//          &outGameServer - 
// Output : true on success, false on failure.
//-----------------------------------------------------------------------------
static bool GetServerListingFromJSON(const rapidjson::Value& value, NetGameServer_t& outGameServer)
{
    if (JSON_GetValue(value, "name",        outGameServer.name)        &&
        JSON_GetValue(value, "description", outGameServer.description) &&
        JSON_GetValue(value, "hidden",      outGameServer.hidden)      &&
        JSON_GetValue(value, "map",         outGameServer.map)         &&
        JSON_GetValue(value, "playlist",    outGameServer.playlist)    &&
        JSON_GetValue(value, "ip",          outGameServer.address)     &&
        JSON_GetValue(value, "port",        outGameServer.port)        &&
        JSON_GetValue(value, "key",         outGameServer.netKey)      &&
        JSON_GetValue(value, "checksum",    outGameServer.checksum)    &&
        JSON_GetValue(value, "numPlayers",  outGameServer.numPlayers)  &&
        JSON_GetValue(value, "maxPlayers",  outGameServer.maxPlayers))
    {
        // Optional fields
        JSON_GetValue(value, "hasPassword", outGameServer.hasPassword);
        JSON_GetValue(value, "password",    outGameServer.netPassword);
        JSON_GetValue(value, "modsProfile", outGameServer.modsProfile);
        JSON_GetValue(value, "region",      outGameServer.region);

        // requiredMods (optional): parse manually from array of strings
        rapidjson::Document::ConstMemberIterator itReq;
        if (JSON_GetIterator(value, "requiredMods", JSONFieldType_e::kArray, itReq))
        {
            outGameServer.requiredMods.clear();
            const rapidjson::Value& arr = itReq->value;
            for (const rapidjson::Value& item : arr.GetArray())
            {
                if (item.IsString())
                {
                    outGameServer.requiredMods.emplace_back(std::string(item.GetString(), item.GetStringLength()));
                }
            }
        }

        rapidjson::Document::ConstMemberIterator itAllow;
        if (JSON_GetIterator(value, "allowedMods", JSONFieldType_e::kArray, itAllow))
        {
            outGameServer.allowedMods.clear();
            const rapidjson::Value& arrAllow = itAllow->value;
            for (const rapidjson::Value& item : arrAllow.GetArray())
            {
                if (item.IsString())
                {
                    outGameServer.allowedMods.emplace_back(std::string(item.GetString(), item.GetStringLength()));
                }
            }
        }
        return true;
    }

    return false;
}

//-----------------------------------------------------------------------------
// Purpose: gets a vector of hosted servers.
// Input  : &outServerList - 
//          &outMessage - 
// Output : true on success, false on failure.
//-----------------------------------------------------------------------------
bool CSpire::GetServerList(vector<NetGameServer_t>& outServerList, string& outMessage) const
{
    if (!IsEnabled())
    {
        SetDisabledMessage(outMessage);
        return false;
    }

    rapidjson::Document requestJson;
    requestJson.SetObject();
    requestJson.AddMember("version", SDK_VERSION, requestJson.GetAllocator());

    rapidjson::StringBuffer stringBuffer;
    JSON_DocumentToBufferDeserialize(requestJson, stringBuffer);

    rapidjson::Document responseJson;
    CURLINFO status;

    if (!SendRequest("/spire/hosts", requestJson, responseJson,
        outMessage, status, "server list error"))
    {
        return false;
    }

    rapidjson::Document::ConstMemberIterator serversIt;

    if (!JSON_GetIterator(responseJson, "servers", JSONFieldType_e::kArray, serversIt))
    {
        outMessage = Format("Invalid response with status: %d", int(status));
        return false;
    }

    const rapidjson::Value::ConstArray serverArray = serversIt->value.GetArray();

    for (const rapidjson::Value& obj : serverArray)
    {
        NetGameServer_t gameServer;

        if (!GetServerListingFromJSON(obj, gameServer))
        {
            // Missing details; skip this server listing.
            continue;
        }

        outServerList.emplace_back(std::move(gameServer));
    }

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Gets the server by token string.
// Input  : &outGameServer - 
//			&outMessage - 
//			&token - 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CSpire::GetServerByToken(NetGameServer_t& outGameServer,
    string& outMessage, const string& token) const
{
    if (!IsEnabled())
    {
        SetDisabledMessage(outMessage);
        return false;
    }

    rapidjson::Document requestJson;
    requestJson.SetObject();

    rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();
    requestJson.AddMember("version", rapidjson::Value(SDK_VERSION, sizeof(SDK_VERSION)-1, requestJson.GetAllocator()), allocator);
    requestJson.AddMember("token", rapidjson::Value(token.c_str(), token.length(), requestJson.GetAllocator()), allocator);

    rapidjson::Document responseJson;
    CURLINFO status;

    if (!SendRequest("/spire/host/claim", requestJson, responseJson,
        outMessage, status, "server not found"))
    {
        return false;
    }

    rapidjson::Document::ConstMemberIterator serversIt;

    if (!JSON_GetIterator(responseJson, "server", JSONFieldType_e::kObject, serversIt))
    {
        outMessage = Format("Invalid response with status: %d", int(status));
        return false;
    }

    const rapidjson::Value& serverJson = serversIt->value;

    if (!GetServerListingFromJSON(serverJson, outGameServer))
    {
        outMessage = Format("Invalid server listing data!");
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Sends host server POST request.
// Input  : &outMessage - 
//			&outToken - 
//			&outHostIp - 
//			&netGameServer - 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CSpire::PostServerHost(string& outMessage, string& outToken, string& outHostIp, const NetGameServer_t& netGameServer) const
{
    if (!IsEnabled())
    {
        SetDisabledMessage(outMessage);
        return false;
    }

    rapidjson::Document requestJson;
    requestJson.SetObject();

    rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();

    requestJson.AddMember("name",        rapidjson::Value(netGameServer.name.c_str(),        netGameServer.name.length(),        allocator), allocator);
    requestJson.AddMember("description", rapidjson::Value(netGameServer.description.c_str(), netGameServer.description.length(), allocator), allocator);
    requestJson.AddMember("hidden",      netGameServer.hidden,                               allocator);
    requestJson.AddMember("map",         rapidjson::Value(netGameServer.map.c_str(),         netGameServer.map.length(),         allocator), allocator);
    requestJson.AddMember("playlist",    rapidjson::Value(netGameServer.playlist.c_str(),    netGameServer.playlist.length(),    allocator), allocator);
    requestJson.AddMember("ip",          rapidjson::Value(netGameServer.address.c_str(),     netGameServer.address.length(),     allocator), allocator);
    requestJson.AddMember("port",        netGameServer.port,                                 allocator);
    requestJson.AddMember("key",         rapidjson::Value(netGameServer.netKey.c_str(),      netGameServer.netKey.length(),      allocator), allocator);
    requestJson.AddMember("checksum",    netGameServer.checksum,                             allocator);
    requestJson.AddMember("version",     rapidjson::Value(netGameServer.versionId.c_str(),   netGameServer.versionId.length(),   allocator), allocator);
    requestJson.AddMember("numPlayers",  netGameServer.numPlayers,                           allocator);
    requestJson.AddMember("maxPlayers",  netGameServer.maxPlayers,                           allocator);
    requestJson.AddMember("timeStamp",   netGameServer.timeStamp,                            allocator);
    requestJson.AddMember("password", rapidjson::Value(netGameServer.netPassword.c_str(), netGameServer.netPassword.length(), allocator), allocator);

    // required mods array
    if (!netGameServer.requiredMods.empty())
    {
        rapidjson::Value mods(rapidjson::kArrayType);
        for (const string& m : netGameServer.requiredMods)
        {
            mods.PushBack(rapidjson::Value(m.c_str(), (rapidjson::SizeType)m.length(), allocator), allocator);
        }
        requestJson.AddMember("requiredMods", mods, allocator);
    }

    if (!netGameServer.allowedMods.empty())
    {
        rapidjson::Value mods(rapidjson::kArrayType);
        for (const string& m : netGameServer.allowedMods)
        {
            mods.PushBack(rapidjson::Value(m.c_str(), (rapidjson::SizeType)m.length(), allocator), allocator);
        }
        requestJson.AddMember("allowedMods", mods, allocator);
    }

    // mods profile
    if (!netGameServer.modsProfile.empty())
    {
        requestJson.AddMember("modsProfile", rapidjson::Value(netGameServer.modsProfile.c_str(), netGameServer.modsProfile.length(), allocator), allocator);
    }

    if (!netGameServer.region.empty())
    {
        requestJson.AddMember("region", rapidjson::Value(netGameServer.region.c_str(), netGameServer.region.length(), allocator), allocator);
    }

    rapidjson::Document responseJson;
    CURLINFO status;

    if (!SendRequest("/spire/hosts/publish", requestJson, responseJson, outMessage, status, "server host error"))
    {
        return false;
    }

    if (netGameServer.hidden)
    {
        const char* token = nullptr;

        if (!JSON_GetValue(responseJson, "token", token))
        {
            outMessage = Format("Invalid response with status: %d", int(status));
            outToken.clear();
            return false;
        }

        outToken = token;
    }

    const char* ip = nullptr;
    int port = 0;

    if (JSON_GetValue(responseJson, "ip", ip) &&
        JSON_GetValue(responseJson, "port", port))
    {
        outHostIp = Format("[%s]:%i", ip, port);
    }

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Checks a list of clients for their banned status.
// Input  : &inBannedVec - 
//			**outBannedVec  - allocated; caller is responsible for freeing it
// Output : True on success, false otherwise.
//-----------------------------------------------------------------------------
bool CSpire::GetBannedList(const CBanSystem::BannedList_t& inBannedVec, CBanSystem::BannedList_t** outBannedVec) const
{
    if (!IsEnabled())
        return false;

    rapidjson::Document requestJson;
    requestJson.SetObject();

    rapidjson::Value playersArray(rapidjson::kArrayType);

    rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();

    FOR_EACH_VEC(inBannedVec, i)
    {
        const CBanSystem::Banned_t& banned = inBannedVec[i];
        rapidjson::Value player(rapidjson::kObjectType);

        player.AddMember("id", banned.m_UserID, allocator);
        player.AddMember("ip", rapidjson::Value(banned.m_Address.String(), banned.m_Address.Length(), allocator), allocator);

        playersArray.PushBack(player, allocator);
    }

    requestJson.AddMember("players", playersArray, allocator);

    rapidjson::Document responseJson;

    string outMessage;
    CURLINFO status;

    if (!SendRequest("/spire/moderation/bulk", requestJson, responseJson, outMessage, status, "banned bulk check error"))
    {
        return false;
    }

    rapidjson::Value::ConstMemberIterator bannedPlayersIt;

    if (!JSON_GetIterator(responseJson, "bannedPlayers", JSONFieldType_e::kArray, bannedPlayersIt))
    {
        outMessage = Format("Invalid response with status: %d", int(status));
        return false;
    }

    const rapidjson::Value::ConstArray bannedPlayers = bannedPlayersIt->value.GetArray();

    if (bannedPlayers.Empty())
        return false;

    *outBannedVec = new CBanSystem::BannedList_t();
    Assert(*outBannedVec);

    for (const rapidjson::Value& obj : bannedPlayers)
    {
        const char* reason = nullptr;
        JSON_GetValue(obj, "reason", reason);

        PlatformUserId_t userId = NULL;
        JSON_GetValue(obj, "id", userId);

        //Default to a connection ban
        CBanSystem::Banned_t::BanType_e banType = CBanSystem::Banned_t::CONNECT;
        JSON_GetValue(obj, "banType", (uint32_t&)banType);

        const char* pszExpiryTimestamp = nullptr;
        JSON_GetValue(obj, "banExpires", pszExpiryTimestamp);

        CBanSystem::Banned_t banned(reason ? reason : "#DISCONNECT_BANNED", userId, banType, pszExpiryTimestamp);
        (*outBannedVec)->AddToTail(banned);
    }

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Checks if client is banned on the comp server.
// Input  : &ipAddress - 
//			userId  - 
//			&personaName - 
//			&outReason - <- contains banned reason if any.
// Output : True if banned, false if not banned.
//-----------------------------------------------------------------------------
bool CSpire::CheckForBan(const string& ipAddress, const uint64_t userId, const string& personaName, string& outReason, CBanSystem::Banned_t::BanType_e& outBanType, string& outExpiryTimestamp) const
{
    if (!IsEnabled())
    {
        SetDisabledMessage(outReason);
        return false;
    }

    rapidjson::Document requestJson;
    requestJson.SetObject();

    rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();

    requestJson.AddMember("name", rapidjson::Value(personaName.c_str(), allocator), allocator);
    requestJson.AddMember("id", userId, allocator);
    requestJson.AddMember("ip", rapidjson::Value(ipAddress.c_str(), allocator), allocator);

    rapidjson::Document responseJson;
    string outMessage;
    CURLINFO status;

    if (!SendRequest("/spire/moderation/one", requestJson, responseJson, outMessage, status, "banned check error"))
    {
        return false;
    }

    bool isBanned = false;

    if (JSON_GetValue(responseJson, "banned", isBanned))
    {
        if (isBanned)
        {
            const char* reason = nullptr;

            outReason = JSON_GetValue(responseJson, "reason", reason)
                ? reason
                : "#DISCONNECT_BANNED";

            // Default to a connection ban
            CBanSystem::Banned_t::BanType_e banType = CBanSystem::Banned_t::CONNECT;
            JSON_GetValue(responseJson, "banType", (uint32_t&)banType);

            const char* expiry = nullptr;
            if (JSON_GetValue(responseJson, "banExpires", expiry))
            {
                outExpiryTimestamp = expiry;
            }

            outBanType = banType;

            // Build a polished message for display/logging
            const char* typeLabel = "User";
            switch (banType)
            {
            case CBanSystem::Banned_t::CONNECT: typeLabel = "User"; break;
            case CBanSystem::Banned_t::COMMUNICATION:    typeLabel = "Chat";       break;
            default:                            typeLabel = "Restriction"; break;
            }

            if (!outExpiryTimestamp.empty())
            {
                std::string dateOnly = outExpiryTimestamp;
                size_t tPos = dateOnly.find('T');
                if (tPos != std::string::npos)
                {
                    dateOnly.erase(tPos);
                }
                else if (dateOnly.size() > 10)
                {
                    dateOnly = dateOnly.substr(0, 10);
                }

                outReason = Format("\n--> This account is banned <--\n\nReason: %s\n\nExpires: %s", outReason.c_str(), dateOnly.c_str());
            }
            else
            {
                outReason = Format("\n--> This account is banned <-- \n\nReason: %s\n\nExpires: Permanent", outReason.c_str());
            }

            return true;
        }
    }

    return false;
}

//-----------------------------------------------------------------------------
// Purpose: whether an endpoint's request and reply carry live credentials
// Input  : *endpoint -
// Output : true if neither body may be written to a log
//-----------------------------------------------------------------------------
static bool Spire_IsCredentialEndpoint(const char* const endpoint)
{
    return endpoint && strcmp(endpoint, "/spire/session/mint") == 0;
}

//-----------------------------------------------------------------------------
// Purpose: log the auth request without its two credential fields
// Input  : &requestJson -
//-----------------------------------------------------------------------------
static void Spire_LogAuthRequestRedacted(const rapidjson::Document& requestJson)
{
    const char* id = "";
    const char* ip = "";
    const char* persona = "";

    JSON_GetValue(requestJson, "id", id);
    JSON_GetValue(requestJson, "ip", ip);
    JSON_GetValue(requestJson, "personaName", persona);

    const char* value = nullptr;
    const size_t codeLen = (JSON_GetValue(requestJson, "code", value) && value)
        ? strlen(value) : 0;

    value = nullptr;
    const size_t tokenLen = (JSON_GetValue(requestJson, "platformToken", value) && value)
        ? strlen(value) : 0;

    Msg(eDLL_T::ENGINE,
        "Auth request: id='%s' ip='%s' personaName='%s' code=<%zu bytes> platformToken=<%zu bytes>\n",
        id, ip, persona, codeLen, tokenLen);
}

//-----------------------------------------------------------------------------
// Purpose: authenticate for 'this' particular connection.
// Input  : userId   -
//          *ipAddress  -
//          *authCode   -
//          &outToken   -
//          &outMessage -
// Output : true on success, false on failure.
//-----------------------------------------------------------------------------
bool CSpire::AuthForConnection(const uint64_t userId, const char* ipAddress,
    const char* authCode, const char* platformToken, string& outToken, string& outMessage,
    const char* personaName) const
{
    if (!IsEnabled())
    {
        SetDisabledMessage(outMessage);
        return false;
    }

    rapidjson::Document requestJson;
    requestJson.SetObject();

    rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();

    // Send as string to avoid JSON number precision loss on uint64 ids.
    char userIdStr[32];
    V_snprintf(userIdStr, sizeof(userIdStr), "%llu", userId);
    requestJson.AddMember("id", rapidjson::Value(userIdStr, allocator), allocator);

    if (spire_showdebuginfo.GetBool())
        Msg(eDLL_T::ENGINE, "Adding user id to auth request: %llu (as string: %s)\n", userId, userIdStr);
    requestJson.AddMember("ip", rapidjson::Value(ipAddress, allocator), allocator);
    requestJson.AddMember("personaName", rapidjson::Value(personaName ? personaName : "", allocator), allocator);
    // Platform authorization code. Empty unless the platform had one resident, and
    // the master server only consults it when configured to check identity upstream.
    requestJson.AddMember("code",
        rapidjson::Value(authCode ? authCode : "", allocator), allocator);

    // Signed platform token proving this account belongs to the player. The master
    // server presents it upstream to establish authenticity, then reads the account
    // and persona out of the same bytes. A live credential: it is never logged here
    // and must not cross a plaintext connection.
    requestJson.AddMember("platformToken",
        rapidjson::Value(platformToken ? platformToken : "", allocator), allocator);

    rapidjson::Document responseJson;

    CURLINFO status;

    if (!SendRequest("/spire/session/mint", requestJson, responseJson, outMessage, status, "platform auth error"))
    {
        return false;
    }

    const char* token = nullptr;

    if (JSON_GetValue(responseJson, "token", token))
    {
        outToken = token;
        return true;
    }

    return false;
}

//-----------------------------------------------------------------------------
// Purpose: Gets the EULA from master server.
// Input  : &outData    -
//          &outMessage - 
// Output : True on success, false on failure.
//-----------------------------------------------------------------------------
bool CSpire::GetEULA(MSEulaData_t& outData, string& outMessage) const
{
    if (!IsEnabled())
    {
        SetDisabledMessage(outMessage);
        return false;
    }

    rapidjson::Document requestJson;
    requestJson.SetObject();

    rapidjson::Document responseJson;
    CURLINFO status;

    if (!SendRequest("/spire/notice", requestJson, responseJson, outMessage, status, "eula fetch error"))
    {
        return false;
    }

    rapidjson::Document::ConstMemberIterator serversIt;

    if (!JSON_GetIterator(responseJson, "data", JSONFieldType_e::kObject, serversIt))
    {
        outMessage = "missing or invalid data";
        return false;
    }

    const rapidjson::Value& data = serversIt->value;

    // check if the EULA response fields are valid.
    if (!JSON_GetValue(data, "contents", outData.contents))
    {
        outMessage = "schema is invalid";
        return false;
    }

    // Version can be null in new schema, so make it optional
    if (!JSON_GetValue(data, "version", outData.version))
    {
        outData.version = 0; // Default version if not provided
    }

    // Language field changed from "language" to "lang" in new schema
    if (!JSON_GetValue(data, "lang", outData.language))
    {
        outData.language = "english"; // Default language if not provided
    }

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: Sends request to Spire master server.
// Input  : *endpoint -
//			&requestJson -
//			&responseJson -
//			&outMessage -
//			&status -
//			checkEula - 
// Output : True on success, false on failure.
//-----------------------------------------------------------------------------
bool CSpire::SendRequest(const char* endpoint, const rapidjson::Document& requestJson,
    rapidjson::Document& responseJson, string& outMessage, CURLINFO& status,
    const char* errorText) const
{

    rapidjson::StringBuffer stringBuffer;
    JSON_DocumentToBufferDeserialize(requestJson, stringBuffer);

    if (Spire_IsCredentialEndpoint(endpoint) && spire_showdebuginfo.GetBool())
    {
        Spire_LogAuthRequestRedacted(requestJson);
    }

    string responseBody;
    if (!QueryServer(endpoint, stringBuffer.GetString(), responseBody, outMessage, status))
    {
        return false;
    }

    if (status == 200) // STATUS_OK
    {
        responseJson.Parse(responseBody.c_str(), responseBody.length());

        if (responseJson.HasParseError())
        {
            Warning(eDLL_T::ENGINE, "%s: JSON parse error at position %zu: %s\n", __FUNCTION__,
                responseJson.GetErrorOffset(), rapidjson::GetParseError_En(responseJson.GetParseError()));

            return false;
        }

        if (!responseJson.IsObject())
        {
            Warning(eDLL_T::ENGINE, "%s: JSON root was not an object\n", __FUNCTION__);
            return false;
        }

        if (spire_showdebuginfo.GetBool())
        {
            if (Spire_IsCredentialEndpoint(endpoint))
            {
                // The reply carries the minted join token.
                const char* token = nullptr;
                const size_t tokenLen = (JSON_GetValue(responseJson, "token", token) && token)
                    ? strlen(token) : 0;

                Msg(eDLL_T::ENGINE, "Auth reply: token <%zu bytes>\n", tokenLen);
            }
            else
            {
                LogBody(responseJson);
            }
        }

        bool success = false;

        if (JSON_GetValue(responseJson, "success", success)
            && success)
        {
            return true;
        }
        else
        {
            ExtractError(responseJson, outMessage, status);
            return false;
        }
    }
    else
    {
        ExtractError(responseBody, outMessage, status, errorText);
        return false;
    }
}

//-----------------------------------------------------------------------------
// Purpose: Sends query to master server.
// Input  : *endpoint    - 
//          *request     - 
//          &outResponse - 
//          &outMessage  - <- contains an error message on failure.
//          &outStatus   - 
// Output : True on success, false on failure.
//-----------------------------------------------------------------------------
bool CSpire::QueryServer(const char* endpoint, const char* request,
	string& outResponse, string& outMessage, CURLINFO& outStatus) const
{
	const bool showDebug = spire_showdebuginfo.GetBool();
	const char* hostName = spire_matchmaking_hostname.GetString();

	if (showDebug)
	{
		if (Spire_IsCredentialEndpoint(endpoint))
		{
			Msg(eDLL_T::ENGINE, "Sending request to '%s' with endpoint '%s' (body withheld)\n",
				hostName, endpoint);
		}
		else
		{
			Msg(eDLL_T::ENGINE, "Sending request to '%s' with endpoint '%s':\n%s\n",
				hostName, endpoint, request);
		}
	}

	// A scheme the user could not type is supplied here instead; CURLFormatUrl
	// leaves an explicit one alone and defaults to https otherwise.
	string schemedHost;

	if (spire_matchmaking_insecure.GetBool() &&
		V_strnicmp(hostName, "http://", 7) != NULL &&
		V_strnicmp(hostName, "https://", 8) != NULL)
	{
		schemedHost = Format("http://%s", hostName);
		hostName = schemedHost.c_str();
	}

	string finalUrl;
	CURLFormatUrl(finalUrl, hostName, endpoint);
	finalUrl += Format("?language=%s", this->GetLanguage().c_str());

	CURLParams params;

	params.writeFunction = CURLWriteStringCallback;
	params.timeout = curl_timeout.GetInt();
	params.verifyPeer = ssl_verify_peer.GetBool();
	params.verbose = curl_debug.GetBool();

	curl_slist* sList = nullptr;
	CURL* curl = CURLInitRequest(finalUrl.c_str(), request, outResponse, sList, params);
	if (!curl)
	{
		return false;
	}

	// Force IPv4 resolution for this request
	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

	CURLcode res = CURLSubmitRequest(curl, sList);
#ifdef CLIENT_DLL
	// No dedicated-server error path on the client, so always surface it here.
	if (!CURLHandleError(curl, res, outMessage, true))
#else
	if (!CURLHandleError(curl, res, outMessage,
		!IsDedicated(/* Errors are already shown for dedicated! */)))
#endif // CLIENT_DLL
	{
		return false;
	}

	outStatus = CURLRetrieveInfo(curl);

	if (showDebug)
	{
		Msg(eDLL_T::ENGINE, "Host '%s' replied with status: '%d'\n",
			hostName, outStatus);
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Extracts the error from the result json.
// Input  : &resultJson - 
//          &outMessage - 
//          status      - 
//          *errorText  - 
//-----------------------------------------------------------------------------
void CSpire::ExtractError(const rapidjson::Document& resultJson, string& outMessage,
    CURLINFO status, const char* errorText) const
{
    const char* error = nullptr;

    if (resultJson.IsObject() && 
        JSON_GetValue(resultJson, "error", error))
    {
        outMessage = error;
    }
    else
    {
        if (!errorText)
        {
            errorText = "unknown error";
        }

        outMessage = Format("Failed with status: %d (%s)",
            int(status), errorText);
    }
}

//-----------------------------------------------------------------------------
// Purpose: Extracts the error from the response buffer.
// Input  : &response   - 
//          &outMessage - 
//          status      - 
//          *errorText  - 
//-----------------------------------------------------------------------------
void CSpire::ExtractError(const string& response, string& outMessage,
    CURLINFO status, const char* errorText) const
{
    if (!response.empty())
    {
        rapidjson::Document resultBody;
        resultBody.Parse(response.c_str(), response.length());

        ExtractError(resultBody, outMessage, status, errorText);
    }
    else if (status)
    {
        outMessage = Format("Failed server query: %d", int(status));
    }
    else
    {
        outMessage = Format("Failed to reach server: %s",
            "connection timed out");
    }
}

//-----------------------------------------------------------------------------
// Purpose: Logs the response body if debug is enabled.
// Input  : &responseJson -
//-----------------------------------------------------------------------------
void CSpire::LogBody(const rapidjson::Document& responseJson) const
{
    rapidjson::StringBuffer stringBuffer;

    JSON_DocumentToBufferDeserialize(responseJson, stringBuffer);
    Msg(eDLL_T::ENGINE, "\n%s\n", stringBuffer.GetString());
}

//-----------------------------------------------------------------------------
// Purpose: Returns whether the spire matchmaking system is enabled.
//-----------------------------------------------------------------------------
bool CSpire::IsEnabled() const
{
    // '-offline' has to hold without any config file having run: a '-launcher 1'
    // boot skips CHostState::LoadConfig entirely, so system/offline_server.cfg
    // never clears spire_matchmaking_enabled and the dedi keeps publishing.
    static const bool offlineLaunch = []() -> bool
    {
        if (!CommandLine()->CheckParm("-offline"))
            return false;

        Msg(eDLL_T::ENGINE, "Spire matchmaking disabled: launched with -offline\n");
        return true;
    }();

    return !offlineLaunch && spire_matchmaking_enabled.GetBool();
}

//-----------------------------------------------------------------------------
// Purpose: If the system is disabled, this will be the default reason message.
//-----------------------------------------------------------------------------
void CSpire::SetDisabledMessage(string& outMsg) const
{
    outMsg = "matchmaking disabled";
}

///////////////////////////////////////////////////////////////////////////////
CSpire g_Spire;
