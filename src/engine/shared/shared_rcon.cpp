//===========================================================================//
// 
// Purpose: Shared rcon utilities.
// 
//===========================================================================//
#include "core/stdafx.h"
#include "base_rcon.h"
#include "shared_rcon.h"
#include "protoc/netcon.pb.h"

enum NetconDirection_e : u8
{
	NETCON_DIR_TO_CLIENT, // Frame travelling from the RCON server to a console.
	NETCON_DIR_TO_SERVER  // Frame travelling from a console to the RCON server.
};

// Bound into the AEAD tag so a frame cannot be replayed, reordered, or reflected
// back down the opposite direction of the connection it was captured on.
struct NetconAadBlock_s
{
	NetconAadBlock_s(const u64 seqNr, const u64 sessionId, const NetconDirection_e direction)
	{
		// Fixed byte order: both ends must build a bit-identical block.
		m_SeqNr = _byteswap_uint64(seqNr);
		m_SessionId = _byteswap_uint64(sessionId);
		m_Direction = direction;

		memset(m_Padding, 0, sizeof(m_Padding));
	}

	u64 m_SeqNr;
	u64 m_SessionId;
	NetconDirection_e m_Direction;
	u8 m_Padding[7];
};

//-----------------------------------------------------------------------------
// Purpose: serialize message to vector
// Input: *pBase - 
// &data - 
// &vecBuf - 
// *pResponseMsg - 
// nResponseMsgLen - 
// *pResponseVal - 
// nResponseValLen - 
// responseType - 
// nMessageId - 
// nMessageType - 
// bEncrypt - 
// bDebug - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NetconServer_Serialize(const CNetConBase* pBase, ConnectedNetConsoleData_s& data, vector<byte>& vecBuf,
	const char* pResponseMsg, const size_t nResponseMsgLen, const char* pResponseVal, const size_t nResponseValLen,
	const netcon::response_e responseType, const int nMessageId, const int nMessageType, const bool bEncrypt, const bool bDebug)
{
	netcon::response response;

	response.set_messageid(nMessageId);
	response.set_messagetype(nMessageType);
	response.set_responsetype(responseType);
	response.set_responsemsg(pResponseMsg, nResponseMsgLen);
	response.set_responseval(pResponseVal, nResponseValLen);

	if (!NetconShared_PackEnvelope(pBase, data, vecBuf, (u32)response.ByteSizeLong(), &response, bEncrypt, bDebug))
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: serialize message to vector
// Input: *pBase - 
// &data - 
// &vecBuf - 
// *szReqBuf - 
// nReqMsgLen - 
// *szReqVal - 
// nReqValLen - 
// *requestType - 
// bEncrypt - 
// bDebug - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NetconClient_Serialize(const CNetConBase* pBase, ConnectedNetConsoleData_s& data, vector<byte>& vecBuf, const char* szReqBuf, const size_t nReqMsgLen,
	const char* szReqVal, const size_t nReqValLen, const netcon::request_e requestType, const bool bEncrypt, const bool bDebug)
{
	netcon::request request;

	request.set_messageid(-1);
	request.set_requesttype(requestType);
	request.set_requestmsg(szReqBuf, nReqMsgLen);
	request.set_requestval(szReqVal, nReqValLen);

	if (!NetconShared_PackEnvelope(pBase, data, vecBuf, (u32)request.ByteSizeLong(), &request, bEncrypt, bDebug))
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: attempt to connect to remote
// Input: *pBase - 
// *pHostAdr - 
// nHostPort - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NetconClient_Connect(CNetConBase* pBase, const char* pHostAdr, const int nHostPort, float flTimeoutSec)
{
	string svLocalHost;
	const bool bValidSocket = nHostPort != SOCKET_ERROR;

	if (bValidSocket && (strcmp(pHostAdr, "localhost") == 0))
	{
		char szHostName[512];
		if (!gethostname(szHostName, sizeof(szHostName)))
		{
			svLocalHost = Format("[%s]:%i", szHostName, nHostPort);
			pHostAdr = svLocalHost.c_str();
		}
	}

	CNetAdr* pNetAdr = pBase->GetNetAddress();
	if (!pNetAdr->SetFromString(pHostAdr, true))
	{
		Error(eDLL_T::CLIENT, NO_ERROR, "Failed to set RCON address: %s\n", pHostAdr);
		return false;
	}

	// Pass 'SOCKET_ERROR' if you want to set port from address string instead.
	if (bValidSocket)
	{
		pNetAdr->SetPort(htons(u_short(nHostPort)));
	}

	CSocketCreator* pCreator = pBase->GetSocketCreator();
	if (pCreator->ConnectSocket(*pNetAdr, true, flTimeoutSec) == SOCKET_ERROR)
	{
		return false;
	}

	Msg(eDLL_T::CLIENT, "Connected to: %s\n", pNetAdr->ToString());
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: packs a message envelope
// Input: *pBase - 
// &data - 
// &outMsgBuf - 
// nMsgLen - 
// *inMsg - 
// bEncrypt - 
// bDebug - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NetconShared_PackEnvelope(const CNetConBase* pBase, ConnectedNetConsoleData_s& data,
	vector<byte>& outMsgBuf, const u32 nMsgLen,
	google::protobuf::MessageLite* const inMsg, const bool bEncrypt, const bool bDebug)
{
	byte* const encodeBuf = new byte[nMsgLen];
	std::unique_ptr<byte[]> encodedContainer(encodeBuf);

	if (!pBase->Encode(inMsg, encodeBuf, nMsgLen))
	{
		if (bDebug)
		{
			Error(eDLL_T::ENGINE, NO_ERROR, "Failed to encode RCON message data\n");
		}

		return false;
	}

	const NetconDirection_e sendDir = data.peerIsServer
		? NETCON_DIR_TO_SERVER
		: NETCON_DIR_TO_CLIENT;

	NetconAadBlock_s aad(data.sendSeqNr, data.sendSessionId, sendDir);

	netcon::envelope envelope;

	netcon::header* const header = envelope.mutable_header();
	header->set_seqnr(data.sendSeqNr);
	header->set_sessionid(data.sendSessionId);

	const byte* dataBuf = encodeBuf;
	std::unique_ptr<byte[]> container;

	if (bEncrypt)
	{
		// Fail closed: the public demo key is not an operator key.
		if (pBase->IsUsingDefaultEncryptionKey())
		{
			static bool s_bLoggedDemoKeyRefuse = false;
			if (!s_bLoggedDemoKeyRefuse)
			{
				s_bLoggedDemoKeyRefuse = true;
				Warning(eDLL_T::ENGINE, "[RCON] refuse encrypted frame on public demo key; set rcon_key to an operator key\n");
			}

			return false;
		}

		byte* encryptBuf = new byte[nMsgLen];
		container.reset(encryptBuf);

		CryptoContext_s ctx;
		if (!pBase->Encrypt(ctx, encodeBuf, encryptBuf, nMsgLen, (const u8*)&aad, sizeof(aad)))
		{
			if (bDebug)
			{
				Error(eDLL_T::ENGINE, NO_ERROR, "Failed to encrypt RCON message data\n");
			}

			return false;
		}

		envelope.set_iv(ctx.iv, sizeof(ctx.iv));
		envelope.set_tag(ctx.authTag, sizeof(ctx.authTag));
		dataBuf = encryptBuf;
	}

	envelope.set_data(dataBuf, nMsgLen);
	const u32 envelopeSize = (u32)envelope.ByteSizeLong();

	outMsgBuf.resize(sizeof(NetConFrameHeader_s) + envelopeSize);
	byte* const scratch = outMsgBuf.data();

	if (!pBase->Encode(&envelope, &scratch[sizeof(NetConFrameHeader_s)], envelopeSize))
	{
		if (bDebug)
		{
			Error(eDLL_T::ENGINE, NO_ERROR, "Failed to encode RCON message envelope\n");
		}

		return false;
	}

	NetConFrameHeader_s* const frameHeader = reinterpret_cast<NetConFrameHeader_s*>(scratch);

	// Write out magic and frame size in network byte order.
	frameHeader->magic = htonl(RCON_FRAME_MAGIC);
	frameHeader->length = htonl(u32(envelopeSize));

	data.sendSeqNr++;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: unpacks a message envelope
// Input: *pBase - 
// &data - 
// nMaxLen - 
// *outMsg - 
// bDecrypt - 
// bDebug - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NetconShared_UnpackEnvelope(const CNetConBase* pBase, ConnectedNetConsoleData_s& data,
	const u32 nMaxLen, google::protobuf::MessageLite* const outMsg,
	const bool bDecrypt, const bool bDebug)
{
	netcon::envelope envelope;

	if (!pBase->Decode(&envelope, data.recvBuffer.data(), data.payloadLen))
	{
		if (bDebug)
		{
			Error(eDLL_T::ENGINE, NO_ERROR, "Failed to decode RCON message envelope\n");
		}

		return false;
	}

	const u32 msgLen = (u32)envelope.data().size();

	if (msgLen > nMaxLen)
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Data in RCON message envelope is too large (%u > %u)\n",
			msgLen, nMaxLen);

		return false;
	}

	const netcon::header& header = envelope.header();

	const NetconDirection_e recvDir = data.peerIsServer
		? NETCON_DIR_TO_CLIENT
		: NETCON_DIR_TO_SERVER;

	NetconAadBlock_s aad(header.seqnr(), header.sessionid(), recvDir);

	const byte* netMsg = reinterpret_cast<const byte*>(envelope.data().c_str());
	const byte* dataBuf = netMsg;

	std::unique_ptr<byte[]> container;

	if (bDecrypt)
	{
		// Fail closed: the public demo key is not an operator key.
		if (pBase->IsUsingDefaultEncryptionKey())
		{
			static bool s_bLoggedDemoKeyDecryptRefuse = false;
			if (!s_bLoggedDemoKeyDecryptRefuse)
			{
				s_bLoggedDemoKeyDecryptRefuse = true;
				Warning(eDLL_T::ENGINE, "[RCON] refuse encrypted frame decrypt on public demo key; set rcon_key to an operator key\n");
			}

			return false;
		}

		const u32 ivLen = (u32)envelope.iv().size();
		const u32 tagLen = (u32)envelope.tag().size();

		if (ivLen != sizeof(CryptoIV_t))
		{
			if (bDebug)
			{
				Error(eDLL_T::ENGINE, NO_ERROR, "IV in RCON message envelope is invalid (%u != %u)\n",
					ivLen, sizeof(CryptoIV_t));
			}

			return false;
		}

		if (tagLen != sizeof(CryptoAuthTag_t))
		{
			if (bDebug)
			{
				Error(eDLL_T::ENGINE, NO_ERROR, "Tag in RCON message envelope is invalid (%u != %u)\n",
					tagLen, sizeof(CryptoAuthTag_t));
			}

			return false;
		}

		byte* decryptBuf = new byte[msgLen];
		container.reset(decryptBuf);

		CryptoContext_s ctx;
		memcpy(ctx.iv, envelope.iv().data(), ivLen);
		memcpy(ctx.authTag, envelope.tag().data(), tagLen);

		if (!pBase->Decrypt(ctx, netMsg, decryptBuf, msgLen, (const u8*)&aad, sizeof(aad)))
		{
			if (bDebug)
			{
				Error(eDLL_T::ENGINE, NO_ERROR, "Failed to decrypt RCON message data\n");
			}

			return false;
		}

		dataBuf = decryptBuf;
	}

	// Sequence/session are connection state: only advance after auth decrypt.
	if (header.seqnr() != data.recvSeqNr + 1)
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Out-of-sequence RCON frame #%llu; expected #%llu\n",
			header.seqnr(), data.recvSeqNr + 1);

		return false;
	}

	if (data.recvSessionKnown)
	{
		if (header.sessionid() != data.recvSessionId)
		{
			Error(eDLL_T::ENGINE, NO_ERROR, "RCON frame carries a foreign session id\n");
			return false;
		}
	}
	else
	{
		data.recvSessionId = header.sessionid();
		data.recvSessionKnown = true;
	}

	data.recvSeqNr = header.seqnr();

	Assert(dataBuf);

	if (!pBase->Decode(outMsg, dataBuf, msgLen))
	{
		if (bDebug)
		{
			Error(eDLL_T::ENGINE, NO_ERROR, "Failed to decode RCON message data\n");
		}

		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: gets the netconsole data
// Input: *pBase - 
// iSocket - 
// Output: nullptr on failure
//-----------------------------------------------------------------------------
ConnectedNetConsoleData_s* NetconShared_GetConnData(CNetConBase* pBase, const int iSocket)
{
	CSocketCreator* pCreator = pBase->GetSocketCreator();
	Assert(iSocket >= 0 && (pCreator->GetAcceptedSocketCount() == 0
		|| iSocket < pCreator->GetAcceptedSocketCount()));

	if (!pCreator->GetAcceptedSocketCount())
	{
		return nullptr;
	}

	return &pCreator->GetAcceptedSocketData(iSocket);
}

//-----------------------------------------------------------------------------
// Purpose: gets the netconsole socket
// Input: *pBase - 
// iSocket - 
// Output: SOCKET_ERROR (-1) on failure
//-----------------------------------------------------------------------------
SocketHandle_t NetconShared_GetSocketHandle(CNetConBase* pBase, const int iSocket)
{
	const ConnectedNetConsoleData_s* pData = NetconShared_GetConnData(pBase, iSocket);
	if (!pData)
	{
		return SOCKET_ERROR;
	}

	return pData->socket;
}

#ifndef _TOOLS

#ifndef CLIENT_DLL
#include "engine/server/sv_rcon.h"
#endif // !CLIENT_DLL
#ifndef DEDICATED
#include "engine/client/cl_rcon.h"
#endif // !DEDICATED

void RCON_KeyChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData);
void RCON_PasswordChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData);

ConVar rcon_debug("rcon_debug", "0", FCVAR_RELEASE, "Show RCON debug information ( !slower! )");
ConVar rcon_encryptframes("rcon_encryptframes", "1", FCVAR_RELEASE, "Whether to encrypt RCON messages");
ConVar rcon_key("rcon_key", "", FCVAR_RELEASE | FCVAR_PROTECTED | FCVAR_DONTRECORD | FCVAR_SERVER_CANNOT_QUERY, "Base64 remote server access encryption key (random if empty or invalid)", &RCON_KeyChanged_f);
ConVar rcon_maxframesize("rcon_maxframesize", "2048", FCVAR_RELEASE, "Max number of bytes allowed in an RCON message", true, 128.f, true, 4096.f);

//-----------------------------------------------------------------------------
// Purpose: change RCON key on server and client
//-----------------------------------------------------------------------------
void RCON_KeyChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData)
{
	if (ConVar* pConVarRef = g_pCVar->FindVar(pConVar->GetName()))
	{
		const char* pNewString = pConVarRef->GetString();

		if (strcmp(pOldString, pNewString) == NULL)
			return; // Same key.


#if !defined(DEDICATED) && !defined(CLIENT_DLL)
		RCONServer()->SetKey(pNewString);
		RCONClient()->SetKey(RCONServer()->GetKey()); // Sync server & client keys

		Msg(eDLL_T::ENGINE, "Installed RCON Key: %s'%s%s%s'\n",
			g_svReset.c_str(), g_svGreyB.c_str(), RCONClient()->GetKey(), g_svReset.c_str());
#else
#ifdef DEDICATED
		RCONServer()->SetKey(pNewString);

		Msg(eDLL_T::SERVER, "Installed RCON Key: %s'%s%s%s'\n",
			g_svReset.c_str(), g_svGreyB.c_str(), RCONServer()->GetKey(), g_svReset.c_str());
#endif // DEDICATED
#ifdef CLIENT_DLL
		RCONClient()->SetKey(pNewString);

		Msg(eDLL_T::CLIENT, "Installed RCON Key: %s'%s%s%s'\n",
			g_svReset.c_str(), g_svGreyB.c_str(), RCONClient()->GetKey(), g_svReset.c_str());
#endif // CLIENT_DLL

#endif // !DEDICATED && !CLIENT_DLL
	}
}

#ifndef CLIENT_DLL
void RCON_InitServerAndTrySyncKeys(const char* pPassword)
{
#ifndef DEDICATED
	RCONServer()->Init(pPassword, rcon_key.GetString());

	if (RCONServer()->IsInitialized())
	{
		// Sync server & client keys
		RCONClient()->SetKey(RCONServer()->GetKey());
	}
#else
	RCONServer()->Init(pPassword, rcon_key.GetString());
#endif // !DEDICATED
}
#endif // !CLIENT_DLL

#ifndef DEDICATED
void RCON_InitClientAndTrySyncKeys()
{
#ifndef CLIENT_DLL
	if (RCONServer()->IsInitialized())
	{
		// Sync server & client keys
		RCONClient()->Init(RCONServer()->GetKey());
	}
	else
#endif // !CLIENT_DLL
	{
		RCONClient()->Init(rcon_key.GetString());
	}
}
#endif // !DEDICATED

#endif // !_TOOLS
