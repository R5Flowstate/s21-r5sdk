#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: server list manager
//
//=============================================================================//

#include "core/stdafx.h"
#include "engine/client/bridge_connect_password.h"
#include "engine/client/bridge_join_auth.h"
#include "tier0/threadtools.h"
#include "tier0/frametask.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "engine/cmd.h"
#include "engine/net.h"
#include "engine/host_state.h"
#include "engine/server/server.h"
#include "engine/client/cl_rcon.h"
#include "rtech/playlists/playlists.h"
#include "spire.h"
#include "listmanager.h"


//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
CServerListManager::CServerListManager(void)
{
}

//-----------------------------------------------------------------------------
// Purpose: get server list from spire
//-----------------------------------------------------------------------------
bool CServerListManager::RefreshServerList(string& outMessage, size_t& numServers)
{
	ClearServerList();

	vector<NetGameServer_t> serverList;
	const bool success = g_Spire.GetServerList(serverList, outMessage);

	if (!success)
		return false;

	AUTO_LOCK(m_Mutex);
	m_vServerList = std::move(serverList);

	numServers = m_vServerList.size();
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: clears the server list
//-----------------------------------------------------------------------------
void CServerListManager::ClearServerList(void)
{
	AUTO_LOCK(m_Mutex);
	m_vServerList.clear();
}

// Do not Cbuf the listing IP; a semicolon in that string is ACE.
//-----------------------------------------------------------------------------
// Purpose: connects to specified server
//-----------------------------------------------------------------------------
void CServerListManager::ConnectToServer(const string& svIp, const int nPort, const string& svNetKey, const string& svNetPassword) const
{
	// Call connect on this thread; the frame-queue hop is skipped in safe mode.
	if (!svNetKey.empty())
		NET_SetKey(svNetKey);

	if (!svNetPassword.empty())
		Bridge_SetConnectPassword(svNetPassword.c_str());

	Bridge_ConnectToHost(svIp.c_str(), nPort);
}

//-----------------------------------------------------------------------------
// Purpose: connects to specified server
//-----------------------------------------------------------------------------
void CServerListManager::ConnectToServer(const string& svServer, const string& svNetKey, const string& svNetPassword) const
{
	if (!svNetKey.empty())
		NET_SetKey(svNetKey);

	if (!svNetPassword.empty())
		Bridge_SetConnectPassword(svNetPassword.c_str());

	Bridge_ConnectToHost(svServer.c_str(), 0);
}

CServerListManager g_ServerListManager;
#else // !CLIENT_DLL
//=============================================================================//
// 
// Purpose: server list manager
// 
//-----------------------------------------------------------------------------
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/threadtools.h"
#include "tier0/frametask.h"
#include "tier1/cvar.h"
#include "engine/cmd.h"
#include "engine/net.h"
#include "engine/host_state.h"
#include "engine/server/server.h"
#include "rtech/playlists/playlists.h"
#include "spire.h"
#include "listmanager.h"

//-----------------------------------------------------------------------------
// Purpose
//-----------------------------------------------------------------------------
CServerListManager::CServerListManager(void)
{
}

//-----------------------------------------------------------------------------
// Purpose: get server list from spire
// Input: &outMessage - 
// &numServers - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool CServerListManager::RefreshServerList(string& outMessage, size_t& numServers)
{
    ClearServerList();

    vector<NetGameServer_t> serverList;
    const bool success = g_Spire.GetServerList(serverList, outMessage);

    if (!success)
        return false;

    AUTO_LOCK(m_Mutex);
    m_vServerList = std::move(serverList);

    numServers = m_vServerList.size();
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: clears the server list
//-----------------------------------------------------------------------------
void CServerListManager::ClearServerList(void)
{
    AUTO_LOCK(m_Mutex);
    m_vServerList.clear();
}

//-----------------------------------------------------------------------------
// Purpose: connects to specified server
// Input: &svIp - 
// nPort - 
// &svNetKey - 
//-----------------------------------------------------------------------------
void CServerListManager::ConnectToServer(const string& svIp, const int nPort, const string& svNetKey, const string& svNetPassword) const
{
    if (!ThreadInMainThread())
    {
        g_TaskQueue.Dispatch([this, svIp, nPort, svNetKey, svNetPassword]()
            {
                this->ConnectToServer(svIp, nPort, svNetKey, svNetPassword);
            }, 0);
        return;
    }

    if (!svNetKey.empty())
    {
        NET_SetKey(svNetKey);
    }

    // Set the engine's server filter to carry the password in the connect request.
    if (!svNetPassword.empty())
    {
        const string tagged = Format("pw:%016llx\n", [] (const string& s) {
            uint64_t h = 1469598103934665603ULL;
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
            return h;
        }(svNetPassword));
        const string setFilter = Format("serverFilter \"%s\"\n", tagged.c_str());
        Cbuf_AddText(Cbuf_GetCurrentPlayer(), setFilter.c_str(), cmd_source_t::kCommandSrcCode);
    }

    const string command = Format("%s \"[%s]:%i\"", "connect", svIp.c_str(), nPort);
    Cbuf_AddText(Cbuf_GetCurrentPlayer(), command.c_str(), cmd_source_t::kCommandSrcCode);
}

//-----------------------------------------------------------------------------
// Purpose: connects to specified server
// Input: &svServer - 
// &svNetKey - 
//-----------------------------------------------------------------------------
void CServerListManager::ConnectToServer(const string& svServer, const string& svNetKey, const string& svNetPassword) const
{
    if (!ThreadInMainThread())
    {
        g_TaskQueue.Dispatch([this, svServer, svNetKey, svNetPassword]()
            {
                this->ConnectToServer(svServer, svNetKey, svNetPassword);
            }, 0);
        return;
    }

    if (!svNetKey.empty())
    {
        NET_SetKey(svNetKey);
    }

    // Set the engine's server filter to carry the password in the connect request.
    if (!svNetPassword.empty())
    {
        const string tagged = Format("pw:%016llx\n", [] (const string& s) {
            uint64_t h = 1469598103934665603ULL;
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
            return h;
        }(svNetPassword));
        const string setFilter = Format("serverFilter \"%s\"\n", tagged.c_str());
        Cbuf_AddText(Cbuf_GetCurrentPlayer(), setFilter.c_str(), cmd_source_t::kCommandSrcCode);
    }

    const string command = Format("%s \"%s\"", "connect", svServer.c_str()).c_str();
    Cbuf_AddText(Cbuf_GetCurrentPlayer(), command.c_str(), cmd_source_t::kCommandSrcCode);
}

CServerListManager g_ServerListManager;
#endif // CLIENT_DLL
