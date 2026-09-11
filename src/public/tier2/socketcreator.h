#pragma once
#include "tier0/threadtools.h"
#include "tier1/NetAdr.h"
#include "common/igameserverdata.h"

//-----------------------------------------------------------------------------
// Purpose: container class to handle network streams
//-----------------------------------------------------------------------------
class CSocketCreator
{
public:
	CSocketCreator(const bool isServer);
	~CSocketCreator(void);

	// The accepted-socket list is walked from logger worker threads (rcon
	// console-log spew) while the main thread adds and compacts entries.
	// Structural mutation happens under m_AcceptedSocketsLock; element
	// references must be used on the main thread or under this lock.
	void LockAcceptedSockets(void) const { m_AcceptedSocketsLock.Lock(); }
	void UnlockAcceptedSockets(void) const { m_AcceptedSocketsLock.Unlock(); }

	void RunFrame(void);
	void ProcessAccept(void);

	bool CreateListenSocket(const netadr_t& netAdr, bool bDualStack = true, bool bReuse = false);
	void CloseListenSocket(void);

	// flTimeoutSec <= 0 polls once (no frame stall). Default 1s for netconsole.
	int ConnectSocket(const netadr_t& netAdr, bool bSingleSocket, float flTimeoutSec = 1.0f);
	void DisconnectSocket(SocketHandle_t hSocket);
	void DisconnectSockets(void);

	bool ConfigureSocket(SocketHandle_t hSocket, bool bDualStack = true, bool bReuse = false);
	int OnSocketAccepted(SocketHandle_t hSocket, const netadr_t& netAdr);

	void CloseAcceptedSocket(int nIndex);
	void CloseAllAcceptedSockets(void);

	bool IsListening(void) const;
	bool IsSocketBlocking(void) const;

	int GetAuthorizedSocketCount(void) const;
	int GetAcceptedSocketCount(void) const;

	SocketHandle_t GetAcceptedSocketHandle(int nIndex) const;
	const netadr_t& GetAcceptedSocketAddress(int nIndex) const;
	ConnectedNetConsoleData_s& GetAcceptedSocketData(int nIndex);
	const ConnectedNetConsoleData_s& GetAcceptedSocketData(int nIndex) const;

public:
	struct AcceptedSocket_t
	{
		netadr_t                  m_Address;
		ConnectedNetConsoleData_s m_Data;
	};

private:
	CUtlVector<AcceptedSocket_t>  m_AcceptedSockets;
	CThreadMutex                  m_AcceptedSocketsLock;
	SocketHandle_t                m_hListenSocket; // Used to accept connections.
	bool                          m_bIsServer;

	enum
	{
		SOCKET_TCP_MAX_ACCEPTS = 2
	};
};
