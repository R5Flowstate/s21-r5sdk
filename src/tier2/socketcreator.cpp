//===========================================================================//
//
// Purpose: Server/Client dual-stack socket utility class
//
//===========================================================================//

#include <tier1/NetAdr.h>
#include <tier2/socketcreator.h>
#include <tier2/cryptutils.h>
#ifndef _TOOLS
#include <engine/sys_utils.h>
#endif // !_TOOLS
#include <engine/net.h>

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
CSocketCreator::CSocketCreator(const bool isServer)
{
	m_hListenSocket = SOCKET_ERROR;
	m_bIsServer = isServer;
}

//-----------------------------------------------------------------------------
// Purpose: Destructor
//-----------------------------------------------------------------------------
CSocketCreator::~CSocketCreator(void)
{
	DisconnectSockets();
}

//-----------------------------------------------------------------------------
// Purpose: accept new connections and walk open sockets and handle any incoming data
//-----------------------------------------------------------------------------
void CSocketCreator::RunFrame(void)
{
	if (IsListening())
	{
		ProcessAccept(); // handle any new connection requests.
	}
}

//-----------------------------------------------------------------------------
// Purpose: handle a new connection
//-----------------------------------------------------------------------------
void CSocketCreator::ProcessAccept(void)
{
	sockaddr_in6 inClient{};
	int nLengthAddr = sizeof(inClient);
	const SocketHandle_t newSocket = SocketHandle_t(::accept(SOCKET(m_hListenSocket), reinterpret_cast<sockaddr*>(&inClient), &nLengthAddr));

	if (newSocket == SOCKET_ERROR)
	{
		if (!IsSocketBlocking())
		{
			Error(eDLL_T::COMMON, 0, "%s - Error: %s\n", __FUNCTION__, NET_ErrorString(WSAGetLastError()));
		}
		return;
	}

	if (!ConfigureSocket(newSocket, false))
	{
		DisconnectSocket(newSocket);
		return;
	}

	netadr_t netAdr;

	if (!netAdr.SetFromSockadr(&inClient))
	{
		Error(eDLL_T::COMMON, 0, "%s - Failed to set from socket address\n", __FUNCTION__);
		DisconnectSocket(newSocket);

		return;
	}

	OnSocketAccepted(newSocket, netAdr);
}

//-----------------------------------------------------------------------------
// Purpose: bind to a TCP port and accept incoming connections
// Input: *netAdr - 
// bDualStack - 
// bReuse - 
// Output: true on success, failed otherwise
//-----------------------------------------------------------------------------
bool CSocketCreator::CreateListenSocket(const netadr_t& netAdr, bool bDualStack, bool bReuse)
{
	CloseListenSocket();
	m_hListenSocket = SocketHandle_t(::socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP));

	if (m_hListenSocket != INVALID_SOCKET)
	{
		if (!ConfigureSocket(m_hListenSocket, bDualStack, bReuse))
		{
			CloseListenSocket();
			return false;
		}

		sockaddr_in6 sadr{};
		netAdr.ToSockadr(&sadr);

		int results = ::bind(m_hListenSocket, reinterpret_cast<sockaddr*>(&sadr), sizeof(sockaddr_in6));
		if (results == SOCKET_ERROR)
		{
			// Port scan callers treat intermediate bind failures as expected.
			Warning(eDLL_T::COMMON, "Socket bind failed (%s)\n", NET_ErrorString(WSAGetLastError()));
			CloseListenSocket();

			return false;
		}

		results = ::listen(m_hListenSocket, SOCKET_TCP_MAX_ACCEPTS);
		if (results == SOCKET_ERROR)
		{
			Error(eDLL_T::COMMON, 0, "Socket listen failed (%s)\n", NET_ErrorString(WSAGetLastError()));
			CloseListenSocket();

			return false;
		}
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: close an open rcon connection
//-----------------------------------------------------------------------------
void CSocketCreator::CloseListenSocket(void)
{
	if (m_hListenSocket != SOCKET_ERROR)
	{
		DisconnectSocket(m_hListenSocket);
		m_hListenSocket = SOCKET_ERROR;
	}
}

//-----------------------------------------------------------------------------
// Purpose: connect to the remote server
// Input: *netAdr - 
// bSingleSocket - 
// Output: accepted socket index, SOCKET_ERROR (-1) if failed
//-----------------------------------------------------------------------------
int CSocketCreator::ConnectSocket(const netadr_t& netAdr, bool bSingleSocket, float flTimeoutSec)
{
	if (bSingleSocket)
	{ // NOTE: Closing an accepted socket will re-index all the sockets with higher indices.
		CloseAllAcceptedSockets();
	}

	SocketHandle_t hSocket = SocketHandle_t(::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
	if (hSocket == SOCKET_ERROR)
	{
		Error(eDLL_T::COMMON, 0, "Unable to create socket (%s)\n", NET_ErrorString(WSAGetLastError()));
		return SOCKET_ERROR;
	}

	if (!ConfigureSocket(hSocket))
	{
		DisconnectSocket(hSocket);
		return SOCKET_ERROR;
	}

	struct sockaddr_in6 s{};
	netAdr.ToSockadr(&s);

	const int results = ::connect(hSocket, reinterpret_cast<sockaddr*>(&s), sizeof(sockaddr_in6));
	if (results == SOCKET_ERROR)
	{
		if (!IsSocketBlocking())
		{
			Error(eDLL_T::COMMON, 0, "Socket connection failed (%s)\n", NET_ErrorString(WSAGetLastError()));

			DisconnectSocket(hSocket);
			return SOCKET_ERROR;
		}

		fd_set writefds;

		FD_ZERO(&writefds);
		FD_SET(static_cast<u_int>(hSocket), &writefds);

		timeval tv;
		if (flTimeoutSec <= 0.0f)
		{
			tv.tv_sec = 0;
			tv.tv_usec = 0;
		}
		else
		{
			tv.tv_sec = static_cast<long>(flTimeoutSec);
			tv.tv_usec = static_cast<long>((flTimeoutSec - static_cast<float>(tv.tv_sec)) * 1000000.0f);
		}

		if (::select(hSocket + 1, NULL, &writefds, NULL, &tv) < 1)
		{
			if (flTimeoutSec > 0.0f)
				Error(eDLL_T::COMMON, 0, "Socket connection timed out\n");
			DisconnectSocket(hSocket);

			return SOCKET_ERROR;
		}
	}

	return OnSocketAccepted(hSocket, netAdr);
}

//-----------------------------------------------------------------------------
// Purpose: closes specific open sockets (listen + accepted)
//-----------------------------------------------------------------------------
void CSocketCreator::DisconnectSocket(SocketHandle_t hSocket)
{
	Assert(hSocket != SOCKET_ERROR);
	if (hSocket == SOCKET_ERROR)
		return;

	// Best-effort close. Process-exit static dtor runs after WSACleanup /
	// while CRT tears down statics -- Error() then AV'd into dead spdlog.
	if (::closesocket(hSocket) == SOCKET_ERROR)
	{
		const int err = WSAGetLastError();
		if (err != WSANOTINITIALISED && err != WSAENOTSOCK && err != WSAENOTCONN)
		{
			Error(eDLL_T::COMMON, 0, "Unable to close socket (%s)\n",
				NET_ErrorString(err));
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: closes all open sockets (listen + accepted)
//-----------------------------------------------------------------------------
void CSocketCreator::DisconnectSockets(void)
{
	CloseListenSocket();
	CloseAllAcceptedSockets();
}

//-----------------------------------------------------------------------------
// Purpose: Configures a socket for use
// Input: iSocket - 
// bDualStack - 
// bReuse - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool CSocketCreator::ConfigureSocket(SocketHandle_t hSocket, bool bDualStack /*= true*/, bool bReuse /*= false*/)
{
	// Disable NAGLE as RCON cmds are small in size.
	int opt = 1;
	int ret = ::setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&opt), sizeof(opt));
	if (ret == SOCKET_ERROR)
	{
		Error(eDLL_T::COMMON, 0, "Socket 'sockopt(%s)' failed (%s)\n", "TCP_NODELAY", NET_ErrorString(WSAGetLastError()));
		return false;
	}

	// Opt-in only: unconditional SO_REUSEADDR lets another local process steal the port.
	if (bReuse)
	{
		opt = 1;
		ret = ::setsockopt(hSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&opt), sizeof(opt));
		if (ret == SOCKET_ERROR)
		{
			Error(eDLL_T::COMMON, 0, "Socket 'sockopt(%s)' failed (%s)\n", "SO_REUSEADDR", NET_ErrorString(WSAGetLastError()));
			return false;
		}
	}

	if (bDualStack)
	{
		// Disable IPv6 only mode to enable dual stack.
		opt = 0;
		ret = ::setsockopt(hSocket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&opt), sizeof(opt));
		if (ret == SOCKET_ERROR)
		{
			Error(eDLL_T::COMMON, 0, "Socket 'sockopt(%s)' failed (%s)\n", "IPV6_V6ONLY", NET_ErrorString(WSAGetLastError()));
			return false;
		}
	}

	// Mark socket as non-blocking.
	opt = 1;
	ret = ::ioctlsocket(hSocket, FIONBIO, reinterpret_cast<u_long*>(&opt));
	if (ret == SOCKET_ERROR)
	{
		Error(eDLL_T::COMMON, 0, "Socket 'ioctl(%s)' failed (%s)\n", "FIONBIO", NET_ErrorString(WSAGetLastError()));
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: handles new TCP requests and puts them in accepted queue
// Input: hSocket - 
// *netAdr - 
// Output: accepted socket index
//-----------------------------------------------------------------------------
int CSocketCreator::OnSocketAccepted(SocketHandle_t hSocket, const netadr_t& netAdr)
{
	// Generate outside the lock: failure logs, and loggers hold s_LogMutex
	// across their own rcon calls -- logging under the lock would ABBA.
	u64 nSessionId = 0;
	const char* errorMsg;

	if (!Plat_GenerateRandom((u8*)&nSessionId, sizeof(nSessionId), errorMsg))
	{
		Error(eDLL_T::COMMON, 0, "Failed to generate RCON session id: [%s]\n", errorMsg);
		DisconnectSocket(hSocket);

		return m_AcceptedSockets.InvalidIndex();
	}

	m_AcceptedSocketsLock.Lock();

	const int hnd = m_AcceptedSockets.AddToTail();
	AcceptedSocket_t& newEntry = m_AcceptedSockets[hnd];

	newEntry.m_Address = netAdr;
	newEntry.m_Data.socket = hSocket;
	newEntry.m_Data.sendSessionId = nSessionId;

	// If we are the server, the far end of this connection is a client.
	newEntry.m_Data.peerIsServer = !m_bIsServer;

	m_AcceptedSocketsLock.Unlock();
	return hnd;
}

//-----------------------------------------------------------------------------
// Purpose: close an accepted socket
// Input: nIndex - 
//-----------------------------------------------------------------------------
void CSocketCreator::CloseAcceptedSocket(int nIndex)
{
	m_AcceptedSocketsLock.Lock();

	if (nIndex >= m_AcceptedSockets.Count())
	{
		Assert(0);
		m_AcceptedSocketsLock.Unlock();
		return;
	}

	{
		AcceptedSocket_t& connected = m_AcceptedSockets[nIndex];
		DisconnectSocket(connected.m_Data.socket);
	}

	m_AcceptedSockets.Remove(nIndex);

	m_AcceptedSocketsLock.Unlock();
}

//-----------------------------------------------------------------------------
// Purpose: close all accepted sockets
//-----------------------------------------------------------------------------
void CSocketCreator::CloseAllAcceptedSockets(void)
{
	m_AcceptedSocketsLock.Lock();

	for (int i = 0; i < m_AcceptedSockets.Count(); ++i)
	{
		AcceptedSocket_t& connected = m_AcceptedSockets[i];
		DisconnectSocket(connected.m_Data.socket);
	}
	m_AcceptedSockets.Purge();

	m_AcceptedSocketsLock.Unlock();
}

//-----------------------------------------------------------------------------
// Purpose: returns true if the listening socket is created and listening
// Output: bool
//-----------------------------------------------------------------------------
bool CSocketCreator::IsListening(void) const
{
	return m_hListenSocket != SOCKET_ERROR;
}

//-----------------------------------------------------------------------------
// Purpose: returns true if the socket would block because of the last socket command
// Output: bool
//-----------------------------------------------------------------------------
bool CSocketCreator::IsSocketBlocking(void) const
{
	return (WSAGetLastError() == WSAEWOULDBLOCK);
}

//-----------------------------------------------------------------------------
// Purpose: returns authorized socket count
// Output: int
//-----------------------------------------------------------------------------
int CSocketCreator::GetAuthorizedSocketCount(void) const
{
	// Worker-reachable via the rcon log spew gate; the CRITICAL_SECTION is
	// recursive, so the logger's own SendEncoded walk re-entering here is safe.
	m_AcceptedSocketsLock.Lock();

	int ret = 0;

	for (int i = 0; i < m_AcceptedSockets.Count(); ++i)
	{
		if (m_AcceptedSockets[i].m_Data.authorized)
		{
			ret++;
		}
	}

	m_AcceptedSocketsLock.Unlock();
	return ret;
}

//-----------------------------------------------------------------------------
// Purpose: returns accepted socket count
// Output: int
//-----------------------------------------------------------------------------
int CSocketCreator::GetAcceptedSocketCount(void) const
{
	m_AcceptedSocketsLock.Lock();

	const int ret = m_AcceptedSockets.Count();

	m_AcceptedSocketsLock.Unlock();
	return ret;
}

//-----------------------------------------------------------------------------
// Purpose: returns accepted socket handle
// Input: nIndex - 
// Output: SocketHandle_t
//-----------------------------------------------------------------------------
SocketHandle_t CSocketCreator::GetAcceptedSocketHandle(int nIndex) const
{
	Assert(nIndex >= 0 && nIndex < m_AcceptedSockets.Count());
	return m_AcceptedSockets[nIndex].m_Data.socket;
}

//-----------------------------------------------------------------------------
// Purpose: returns accepted socket address
// Input: nIndex - 
// Output: const netadr_t&
//-----------------------------------------------------------------------------
const netadr_t& CSocketCreator::GetAcceptedSocketAddress(int nIndex) const
{
	Assert(nIndex >= 0 && nIndex < m_AcceptedSockets.Count());
	return m_AcceptedSockets[nIndex].m_Address;
}

//-----------------------------------------------------------------------------
// Purpose: returns accepted socket data
// Input: nIndex - 
// Output: CConnectedNetConsoleData*
//-----------------------------------------------------------------------------
ConnectedNetConsoleData_s& CSocketCreator::GetAcceptedSocketData(int nIndex)
{
	Assert(nIndex >= 0 && nIndex < m_AcceptedSockets.Count());
	return m_AcceptedSockets[nIndex].m_Data;
}

//-----------------------------------------------------------------------------
// Purpose: returns accepted socket data
// Input: nIndex - 
// Output: CConnectedNetConsoleData*
//-----------------------------------------------------------------------------
const ConnectedNetConsoleData_s& CSocketCreator::GetAcceptedSocketData(int nIndex) const
{
	Assert(nIndex >= 0 && nIndex < m_AcceptedSockets.Count());
	return m_AcceptedSockets[nIndex].m_Data;
}
