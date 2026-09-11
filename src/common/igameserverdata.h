//===========================================================================//
//
// Purpose: Enumerations for writing out the requests.
//
//===========================================================================//
#pragma once
#include "netcon/INetCon.h"

typedef int SocketHandle_t;

enum class ServerDataRequestType_e : int
{
	SERVERDATA_REQUEST_VALUE = 0,
	SERVERDATA_REQUEST_SETVALUE,
	SERVERDATA_REQUEST_EXECCOMMAND,
	SERVERDATA_REQUEST_AUTH,
	SERVERDATA_REQUEST_SEND_CONSOLE_LOG,
	SERVERDATA_REQUEST_SEND_REMOTEBUG,
};

enum class ServerDataResponseType_e : int
{
	SERVERDATA_RESPONSE_VALUE = 0,
	SERVERDATA_RESPONSE_UPDATE,
	SERVERDATA_RESPONSE_AUTH,
	SERVERDATA_RESPONSE_CONSOLE_LOG,
	SERVERDATA_RESPONSE_STRING,
	SERVERDATA_RESPONSE_REMOTEBUG,
};

struct ConnectedNetConsoleData_s
{
	SocketHandle_t socket;
	u32 payloadLen;        // Num bytes for this message.
	u32 payloadRead;       // Num read bytes from input buffer.
	s32 numFailedAttempts; // Num failed authentication attempts.
	s32 numIgnoredMessage; // Count how many times client ignored the no-auth message.
	bool validated;        // Revalidates netconsole if false.
	bool authorized;       // Set to true after successful netconsole auth.
	bool inputOnly;        // If set, don't send spew to this netconsole.
	bool peerIsServer;     // True when the remote end of this connection is the RCON server.
	u64 sendSeqNr;         // Sequence number stamped on the next frame we send.
	u64 recvSeqNr;         // Sequence number of the last frame accepted from the peer.
	u64 sendSessionId;     // Random id identifying our half of this connection.
	u64 recvSessionId;     // Peer's session id, latched from its first accepted frame.
	bool recvSessionKnown; // False until recvSessionId has been latched.
	NetConFrameHeader_s frameHeader; // Current frame header.
	vector<byte> recvBuffer;

	ConnectedNetConsoleData_s(SocketHandle_t hSocket = -1)
	{
		socket = hSocket;
		payloadLen = 0;
		payloadRead = 0;
		numFailedAttempts = 0;
		numIgnoredMessage = 0;
		validated = false;
		authorized = false;
		inputOnly = true;
		peerIsServer = false;
		sendSeqNr = 1;
		recvSeqNr = 0;
		sendSessionId = 0;
		recvSessionId = 0;
		recvSessionKnown = false;
		frameHeader.magic = 0;
		frameHeader.length = 0;
	}
};

/* PACKET FORMAT **********************************

REQUEST
  NetConFrameHeader_s header;
  byte* data;

RESPONSE
  NetConFrameHeader_s header;
  byte* data;

***************************************************/
