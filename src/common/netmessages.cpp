#if defined(CLIENT_DLL)
//===============================================================================//
//
// Purpose
//
// $NoKeywords: $
//
//===============================================================================//
// netmessages.cpp: implementation of the CNetMessage types.
//
///////////////////////////////////////////////////////////////////////////////////
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "engine/net.h"
#include "common/netmessages.h"
#include "common/callback.h"
#include "game/shared/usermessages.h"


#include "game/client/hud_basechat.h"
#include "game/shared/scriptnetdata_limits.h"

///////////////////////////////////////////////////////////////////////////////////
// re-implementation of 'SVC_Print::Process'
///////////////////////////////////////////////////////////////////////////////////
bool SVC_Print::ProcessImpl()
{
	if (this->m_szText)
	{
		Assert(m_szText == m_szTextBuffer); // Should always point to 'm_szTextBuffer'.

		size_t len = strnlen_s(m_szText, sizeof(m_szTextBuffer));
		Assert(len < sizeof(m_szTextBuffer));

		if (len && len < sizeof(m_szTextBuffer))
		{
			Msg(eDLL_T::SERVER, m_szText[len-1] == '\n' ? "%s" : "%s\n", m_szText);
		}
	}

	return true; // Original just return true also.
}

///////////////////////////////////////////////////////////////////////////////////
// re-implementation of 'SVC_UserMessage::Process'
///////////////////////////////////////////////////////////////////////////////////
bool SVC_UserMessage::ProcessImpl()
{
	if (m_nMsgType == UserMessages_t::TextMsg)
	{
		bf_read buf = m_DataIn;
		byte type = byte(buf.ReadByte());

		if (type == HUD_PRINTCONSOLE ||
			type == HUD_PRINTCENTER)
		{
			char text[MAX_USER_MSG_DATA];
			int len;

			buf.ReadString(text, sizeof(text), false, &len);
			Assert(len < sizeof(text));

			if (len && len < sizeof(text))
			{
				Msg(eDLL_T::SERVER, text[len - 1] == '\n' ? "%s" : "%s\n", text);
			}
		}
	}

	// No RTTI string on the S21 client (class renamed); trace lives in bridge_flag_set.h.

	return SVC_UserMessage_Process(this); // Need to return original.
}

///////////////////////////////////////////////////////////////////////////////////
// Net message to change class settings vars on the client
///////////////////////////////////////////////////////////////////////////////////
bool SVC_SetClassVar::ReadFromBuffer(bf_read* buffer)
{
	const bool key = buffer->ReadString(m_szKey, sizeof(m_szKey));
	const bool val = buffer->ReadString(m_szValue, sizeof(m_szValue));

	return key && val;
}
bool SVC_SetClassVar::WriteToBuffer(bf_write* buffer)
{
	const bool key = buffer->WriteString(m_szKey);
	const bool val = buffer->WriteString(m_szValue);

	return key && val;
}
bool SVC_SetClassVar::Process(void)
{
	const char* pArgs[3] = {
		"_setClassVarClient",
		m_szKey,
		m_szValue
	};

	CCommand command((int)V_ARRAYSIZE(pArgs), pArgs, cmd_source_t::kCommandSrcCode);
	if (command.ArgC() >= 3)
		v__setClassVarClient_f(command);

	return true;
}

bool SVC_SystemSayText::ReadFromBuffer(bf_read* buffer)
{
	buffer->ReadString(m_szPrefix, sizeof(m_szPrefix));
	buffer->ReadString(m_szMessage, sizeof(m_szMessage));
	m_bAdminMsg = buffer->ReadOneBit();
	return !buffer->IsOverflowed();
}

bool SVC_SystemSayText::WriteToBuffer(bf_write* buffer)
{
	buffer->WriteString(m_szPrefix);
	buffer->WriteString(m_szMessage);
	buffer->WriteOneBit(m_bAdminMsg);
	return !buffer->IsOverflowed();
}

bool SVC_SystemSayText::Process(void)
{
	if (*g_ppHudChat)
		(*g_ppHudChat)->PrintSystemMsg(m_szPrefix, m_szMessage, m_bAdminMsg);
	return true;
}

///////////////////////////////////////////////////////////////////////////////////
// NET_ScriptMessage
///////////////////////////////////////////////////////////////////////////////////
bool NET_ScriptMessage::ReadFromBuffer(bf_read* buffer)
{
	m_bIsTyped = buffer->ReadOneBit() != 0;

	const int nPayloadBytes = buffer->ReadShort();
	if (nPayloadBytes < 0 || nPayloadBytes > SCRIPT_MESSAGE_BUFFER_SIZE)
	{
		Warning(eDLL_T::ENGINE, "NET_ScriptMessage: invalid payload size %d\n", nPayloadBytes);
		return false;
	}

	if (!buffer->ReadBytes(m_Buffer, nPayloadBytes))
	{
		Warning(eDLL_T::ENGINE, "NET_ScriptMessage: failed to read %d payload bytes\n", nPayloadBytes);
		return false;
	}

	m_DataIn.StartReading(m_Buffer, nPayloadBytes, 0, nPayloadBytes * 8);
	return !buffer->IsOverflowed();
}

bool NET_ScriptMessage::WriteToBuffer(bf_write* buffer)
{
	buffer->WriteOneBit(m_bIsTyped ? 1 : 0);

	const int nPayloadBytes = (m_DataOut.GetNumBitsWritten() + 7) >> 3;
	buffer->WriteShort(nPayloadBytes);
	buffer->WriteBytes(m_DataOut.GetData(), nPayloadBytes);

	return !buffer->IsOverflowed();
}

bool NET_ScriptMessage::Process(void)
{
	if (m_bIsTyped)
	{
		// Typed = client->server remote function call
	}
	else
	{
		// Untyped script messages were the removed SNDC overflow extension
		// channel (NET_ScriptMessage) -- no longer used, ignore.
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////////
// Below functions are hooked as playlist overrides can be abused from the client.
// The client could basically manage the server's playlists. Only allow read/write
// when cheats are enabled.
///////////////////////////////////////////////////////////////////////////////////
bool CLC_SetPlaylistVarOverride::ReadFromBufferImpl(CLC_SetPlaylistVarOverride* thisptr, bf_read* buffer)
{
	// Abusable netmsg; only allow if cheats are enabled.
	if (!sv_cheats->GetBool())
	{
		return false;
	}

	return CLC_SetPlaylistVarOverride_ReadFromBuffer(thisptr, buffer);
}
bool CLC_SetPlaylistVarOverride::WriteToBufferImpl(CLC_SetPlaylistVarOverride* thisptr, bf_write* buffer)
{
	// Abusable netmsg; only allow if cheats are enabled.
	if (!sv_cheats->GetBool())
	{
		return false;
	}

	return CLC_SetPlaylistVarOverride_WriteToBuffer(thisptr, buffer);
}

static ConVar enable_CmdKeyValues("enable_CmdKeyValues", "0", FCVAR_DEVELOPMENTONLY, "Toggle CmdKeyValues transmit and receive.");

static bool CmdKeyValues_ReadGated(Base_CmdKeyValues* thisptr, bf_read* buffer,
	bool (*orig)(Base_CmdKeyValues*, bf_read*))
{
	if (!buffer)
		return false;

	const ssize_t left = buffer->GetNumBitsLeft();
	if (left < 32)
	{
		if (left > 0)
			buffer->SeekRelative(left);
		return false;
	}

	const unsigned nBits = buffer->PeekUBitLong(32);
	unsigned nBytes = 0xFFFFFFFFu;
	if (nBits <= (0xFFFFFFFFu - 7u))
		nBytes = (nBits + 7u) / 8u;

	const bool tooBig = nBytes > 65536u || static_cast<ssize_t>(nBits) > (left - 32);
	if (!enable_CmdKeyValues.GetBool() || tooBig || !orig)
	{
		const ssize_t payload = (static_cast<ssize_t>(nBits) <= (left - 32))
			? static_cast<ssize_t>(nBits) : (left - 32);
		buffer->SeekRelative(32 + payload);
		return false;
	}

	return orig(thisptr, buffer);
}

static bool CLC_CmdKeyValues_ReadFromBufferImpl(Base_CmdKeyValues* thisptr, bf_read* buffer)
{
	return CmdKeyValues_ReadGated(thisptr, buffer, CLC_CmdKeyValues_ReadFromBuffer);
}
static bool CLC_CmdKeyValues_WriteToBufferImpl(Base_CmdKeyValues* thisptr, bf_write* buffer)
{
	if (!enable_CmdKeyValues.GetBool() || !CLC_CmdKeyValues_WriteToBuffer)
		return false;
	return CLC_CmdKeyValues_WriteToBuffer(thisptr, buffer);
}
static bool SVC_CmdKeyValues_ReadFromBufferImpl(Base_CmdKeyValues* thisptr, bf_read* buffer)
{
	return CmdKeyValues_ReadGated(thisptr, buffer, SVC_CmdKeyValues_ReadFromBuffer);
}
static bool SVC_CmdKeyValues_WriteToBufferImpl(Base_CmdKeyValues* thisptr, bf_write* buffer)
{
	if (!enable_CmdKeyValues.GetBool() || !SVC_CmdKeyValues_WriteToBuffer)
		return false;
	return SVC_CmdKeyValues_WriteToBuffer(thisptr, buffer);
}

///////////////////////////////////////////////////////////////////////////////////
// below functions are hooked as 'CmdKeyValues' isn't really used in this game, but
// still exploitable on the server. the 'OnPlayerAward' command calls the function
// 'UTIL_SendClientCommandKVToPlayer' which forwards the keyvalues to all connected clients.
///////////////////////////////////////////////////////////////////////////////////
bool Base_CmdKeyValues::ReadFromBufferImpl(Base_CmdKeyValues* thisptr, bf_read* buffer)
{
	return CmdKeyValues_ReadGated(thisptr, buffer, Base_CmdKeyValues_ReadFromBuffer);
}
bool Base_CmdKeyValues::WriteToBufferImpl(Base_CmdKeyValues* thisptr, bf_write* buffer)
{
	if (!enable_CmdKeyValues.GetBool())
		return false;

	return Base_CmdKeyValues_WriteToBuffer(thisptr, buffer);
}

///////////////////////////////////////////////////////////////////////////////////
// determine whether or not the message can be copied into the replay buffer,
// regardless of the 'CNetMessage::m_Group' type.
///////////////////////////////////////////////////////////////////////////////////
bool CanReplayMessage(const CNetMessage* msg)
{
	switch (msg->GetType())
	{
	// String commands can be abused in a way they get executed
	// on the client that is watching a replay. This happens as
	// the server copies the message into the replay buffer from
	// the client that initially submitted it. Its group type is
	// 'None', so call this to determine whether or not to set
	// the group type to 'NoReplay'. This exploit has been used
	// to connect clients to an arbitrary server during replay.
	case NetMessageType::net_StringCmd:
	// Print and user messages sometimes make their way to the
	// client that is watching a replay, while it should only
	// be broadcasted to the target client. This happens for the 
	// same reason as the 'net_StringCmd' above.
	case NetMessageType::svc_Print:
	{
		return false;
	}
	case NetMessageType::svc_UserMessage:
	{
		SVC_UserMessage* userMsg = (SVC_UserMessage*)msg;

		// Just don't replay console prints.
		if (userMsg->m_nMsgType == UserMessages_t::TextMsg)
		{
			return false;
		}

		return true;
	}
	default:
	{
		return true;
	}
	}
}

void V_NetMessages::Detour(const bool bAttach) const
{
	if (bAttach)
	{
		auto hk_SVCPrint_Process = &SVC_Print::ProcessImpl;
		auto hk_SVCUserMessage_Process = &SVC_UserMessage::ProcessImpl;

		CMemory::HookVirtualMethod((uintptr_t)g_pSVC_Print_VFTable, (LPVOID&)hk_SVCPrint_Process, NetMessageVtbl::Process, (LPVOID*)&SVC_Print_Process);
		CMemory::HookVirtualMethod((uintptr_t)g_pSVC_UserMessage_VFTable, (LPVOID&)hk_SVCUserMessage_Process, NetMessageVtbl::Process, (LPVOID*)&SVC_UserMessage_Process);
		CMemory::HookVirtualMethod((uintptr_t)g_pBase_CmdKeyValues_VFTable, (LPVOID*)&Base_CmdKeyValues::ReadFromBufferImpl, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&Base_CmdKeyValues_ReadFromBuffer);
		CMemory::HookVirtualMethod((uintptr_t)g_pBase_CmdKeyValues_VFTable, (LPVOID*)&Base_CmdKeyValues::WriteToBufferImpl, NetMessageVtbl::WriteToBuffer, (LPVOID*)&Base_CmdKeyValues_WriteToBuffer);
		if (g_pCLC_CmdKeyValues_VFTable)
		{
			CMemory::HookVirtualMethod((uintptr_t)g_pCLC_CmdKeyValues_VFTable, (LPVOID)CLC_CmdKeyValues_ReadFromBufferImpl, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&CLC_CmdKeyValues_ReadFromBuffer);
			CMemory::HookVirtualMethod((uintptr_t)g_pCLC_CmdKeyValues_VFTable, (LPVOID)CLC_CmdKeyValues_WriteToBufferImpl, NetMessageVtbl::WriteToBuffer, (LPVOID*)&CLC_CmdKeyValues_WriteToBuffer);
		}
		else
		{
			Warning(eDLL_T::ENGINE, "[NET] CLC_CmdKeyValues vtable unresolved -- type 59 RFB unhooked\n");
		}
		if (g_pSVC_CmdKeyValues_VFTable)
		{
			CMemory::HookVirtualMethod((uintptr_t)g_pSVC_CmdKeyValues_VFTable, (LPVOID)SVC_CmdKeyValues_ReadFromBufferImpl, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&SVC_CmdKeyValues_ReadFromBuffer);
			CMemory::HookVirtualMethod((uintptr_t)g_pSVC_CmdKeyValues_VFTable, (LPVOID)SVC_CmdKeyValues_WriteToBufferImpl, NetMessageVtbl::WriteToBuffer, (LPVOID*)&SVC_CmdKeyValues_WriteToBuffer);
		}
		CMemory::HookVirtualMethod((uintptr_t)g_pCLC_SetPlaylistVarOverride_VFTable, (LPVOID*)&CLC_SetPlaylistVarOverride::ReadFromBufferImpl, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&CLC_SetPlaylistVarOverride_ReadFromBuffer);
		CMemory::HookVirtualMethod((uintptr_t)g_pCLC_SetPlaylistVarOverride_VFTable, (LPVOID*)&CLC_SetPlaylistVarOverride::WriteToBufferImpl, NetMessageVtbl::WriteToBuffer, (LPVOID*)&CLC_SetPlaylistVarOverride_WriteToBuffer);
	}
	else
	{
		void* hkRestore = nullptr;
		CMemory::HookVirtualMethod((uintptr_t)g_pSVC_Print_VFTable, (LPVOID)SVC_Print_Process, NetMessageVtbl::Process, (LPVOID*)&hkRestore);
		CMemory::HookVirtualMethod((uintptr_t)g_pSVC_UserMessage_VFTable, (LPVOID)SVC_UserMessage_Process, NetMessageVtbl::Process, (LPVOID*)&hkRestore);
		CMemory::HookVirtualMethod((uintptr_t)g_pBase_CmdKeyValues_VFTable, (LPVOID)Base_CmdKeyValues_ReadFromBuffer, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&hkRestore);
		CMemory::HookVirtualMethod((uintptr_t)g_pBase_CmdKeyValues_VFTable, (LPVOID)Base_CmdKeyValues_WriteToBuffer, NetMessageVtbl::WriteToBuffer, (LPVOID*)&hkRestore);
		if (g_pCLC_CmdKeyValues_VFTable && CLC_CmdKeyValues_ReadFromBuffer)
			CMemory::HookVirtualMethod((uintptr_t)g_pCLC_CmdKeyValues_VFTable, (LPVOID)CLC_CmdKeyValues_ReadFromBuffer, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&hkRestore);
		if (g_pCLC_CmdKeyValues_VFTable && CLC_CmdKeyValues_WriteToBuffer)
			CMemory::HookVirtualMethod((uintptr_t)g_pCLC_CmdKeyValues_VFTable, (LPVOID)CLC_CmdKeyValues_WriteToBuffer, NetMessageVtbl::WriteToBuffer, (LPVOID*)&hkRestore);
		if (g_pSVC_CmdKeyValues_VFTable && SVC_CmdKeyValues_ReadFromBuffer)
			CMemory::HookVirtualMethod((uintptr_t)g_pSVC_CmdKeyValues_VFTable, (LPVOID)SVC_CmdKeyValues_ReadFromBuffer, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&hkRestore);
		if (g_pSVC_CmdKeyValues_VFTable && SVC_CmdKeyValues_WriteToBuffer)
			CMemory::HookVirtualMethod((uintptr_t)g_pSVC_CmdKeyValues_VFTable, (LPVOID)SVC_CmdKeyValues_WriteToBuffer, NetMessageVtbl::WriteToBuffer, (LPVOID*)&hkRestore);
		CMemory::HookVirtualMethod((uintptr_t)g_pCLC_SetPlaylistVarOverride_VFTable, (LPVOID)CLC_SetPlaylistVarOverride_ReadFromBuffer, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&hkRestore);
		CMemory::HookVirtualMethod((uintptr_t)g_pCLC_SetPlaylistVarOverride_VFTable, (LPVOID)CLC_SetPlaylistVarOverride_WriteToBuffer, NetMessageVtbl::WriteToBuffer, (LPVOID*)&hkRestore);
	}
}
#else // !CLIENT_DLL
//===============================================================================//
//
// Purpose
//
// $NoKeywords: $
//
//===============================================================================//
// netmessages.cpp: implementation of the CNetMessage types.
//
///////////////////////////////////////////////////////////////////////////////////
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "engine/net.h"
#include "common/netmessages.h"
#include "common/callback.h"
#include "game/shared/usermessages.h"

#include "engine/client/client.h"
#include "game/shared/scriptremotefunctions_server.h"


///////////////////////////////////////////////////////////////////////////////////
// re-implementation of 'SVC_Print::Process'
///////////////////////////////////////////////////////////////////////////////////
bool SVC_Print::ProcessImpl()
{
	if (this->m_szText)
	{
		Assert(m_szText == m_szTextBuffer); // Should always point to 'm_szTextBuffer'.

		size_t len = strnlen_s(m_szText, sizeof(m_szTextBuffer));
		Assert(len < sizeof(m_szTextBuffer));

		if (len && len < sizeof(m_szTextBuffer))
		{
			Msg(eDLL_T::SERVER, m_szText[len-1] == '\n' ? "%s" : "%s\n", m_szText);
		}
	}

	return true; // Original just return true also.
}

///////////////////////////////////////////////////////////////////////////////////
// re-implementation of 'SVC_UserMessage::Process'
///////////////////////////////////////////////////////////////////////////////////
bool SVC_UserMessage::ProcessImpl()
{
	if (m_nMsgType == UserMessages_t::TextMsg)
	{
		bf_read buf = m_DataIn;
		byte type = byte(buf.ReadByte());

		if (type == HUD_PRINTCONSOLE ||
			type == HUD_PRINTCENTER)
		{
			char text[MAX_USER_MSG_DATA];
			int len;

			buf.ReadString(text, sizeof(text), false, &len);
			Assert(len < sizeof(text));

			if (len && len < sizeof(text))
			{
				Msg(eDLL_T::SERVER, text[len - 1] == '\n' ? "%s" : "%s\n", text);
			}
		}
	}

	return SVC_UserMessage_Process(this); // Need to return original.
}

///////////////////////////////////////////////////////////////////////////////////
// Net message to change class settings vars on the client
///////////////////////////////////////////////////////////////////////////////////
bool SVC_SetClassVar::ReadFromBuffer(bf_read* buffer)
{
	const bool key = buffer->ReadString(m_szKey, sizeof(m_szKey));
	const bool val = buffer->ReadString(m_szValue, sizeof(m_szValue));

	return key && val;
}
bool SVC_SetClassVar::WriteToBuffer(bf_write* buffer)
{
	const bool key = buffer->WriteString(m_szKey);
	const bool val = buffer->WriteString(m_szValue);

	return key && val;
}
bool SVC_SetClassVar::Process(void)
{
	const char* pArgs[3] = {
		"_setClassVarClient",
		m_szKey,
		m_szValue
	};

	CCommand command((int)V_ARRAYSIZE(pArgs), pArgs, cmd_source_t::kCommandSrcCode);
	if (command.ArgC() >= 3)
		v__setClassVarClient_f(command);

	return true;
}

bool SVC_SystemSayText::ReadFromBuffer(bf_read* buffer)
{
	buffer->ReadString(m_szPrefix, sizeof(m_szPrefix));
	buffer->ReadString(m_szMessage, sizeof(m_szMessage));
	m_bAdminMsg = buffer->ReadOneBit();
	return !buffer->IsOverflowed();
}

bool SVC_SystemSayText::WriteToBuffer(bf_write* buffer)
{
	buffer->WriteString(m_szPrefix);
	buffer->WriteString(m_szMessage);
	buffer->WriteOneBit(m_bAdminMsg);
	return !buffer->IsOverflowed();
}

bool SVC_SystemSayText::Process(void)
{
	return true;
}

///////////////////////////////////////////////////////////////////////////////////
// SVC_Sounds wire encode/decode; see netmessages.h and sound.h.
///////////////////////////////////////////////////////////////////////////////////
bool SVC_Sounds::WriteToBuffer(bf_write* buffer)
{
	buffer->WriteLong(m_ticks);

	// SVC_Sounds entry shape (ProcessNetworkSound / S21):
	// isStart(1) [hasSeek(1) [seek float(32)]] soundId(64)
	// isOnEntity(1) [entIndex(14)]. No-seek start = 81 payload bits.
	uint8_t payloadBuf[16] = {};
	bf_write payload;
	payload.StartWriting(payloadBuf, sizeof(payloadBuf));

	payload.WriteOneBit(1); // isStart
	payload.WriteOneBit(0); // hasSeek = false (no seek float follows)
	payload.WriteLongLong(static_cast<int64>(m_soundHash));
	payload.WriteOneBit(1); // isOnEntity
	payload.WriteUBitLong(static_cast<unsigned int>(m_entityIndex), 14);

	const int nBits = payload.GetNumBitsWritten();
	buffer->WriteUBitLong(static_cast<unsigned int>(nBits), 8);
	return buffer->WriteBits(payloadBuf, nBits) && !buffer->IsOverflowed();
}

bool SVC_Sounds::ReadFromBuffer(bf_read* buffer)
{
	m_ticks = buffer->ReadLong();
	buffer->ReadUBitLong(8); // payload bit-length, unused -- variable shape below

	const bool isStart = buffer->ReadOneBit() != 0;
	if (isStart)
	{
		const bool hasSeek = buffer->ReadOneBit() != 0;
		if (hasSeek)
			buffer->ReadFloat(); // seek, unused on dedi receive path
	}

	m_soundHash = static_cast<uint64_t>(buffer->ReadLongLong());

	const bool isOnEntity = buffer->ReadOneBit() != 0;
	m_entityIndex = isOnEntity ? static_cast<int>(buffer->ReadUBitLong(14)) : -1;

	return !buffer->IsOverflowed();
}

///////////////////////////////////////////////////////////////////////////////////
// NET_ScriptMessage
///////////////////////////////////////////////////////////////////////////////////
bool NET_ScriptMessage::ReadFromBuffer(bf_read* buffer)
{
	m_bIsTyped = buffer->ReadOneBit() != 0;

	// S21: [1b flag][15b payloadByteLen][1b sign]; ReadShort consumes length+sign.

	const int nPayloadBytes = buffer->ReadShort();
	if (nPayloadBytes < 0 || nPayloadBytes > SCRIPT_MESSAGE_BUFFER_SIZE)
	{
		Warning(eDLL_T::ENGINE, "NET_ScriptMessage: invalid payload size %d\n", nPayloadBytes);
		return false;
	}

	if (!buffer->ReadBytes(m_Buffer, nPayloadBytes))
	{
		Warning(eDLL_T::ENGINE, "NET_ScriptMessage: failed to read %d payload bytes\n", nPayloadBytes);
		return false;
	}

	m_DataIn.StartReading(m_Buffer, nPayloadBytes, 0, nPayloadBytes * 8);
	return !buffer->IsOverflowed();
}

bool NET_ScriptMessage::WriteToBuffer(bf_write* buffer)
{
	buffer->WriteOneBit(m_bIsTyped ? 1 : 0);

	const int nPayloadBytes = (m_DataOut.GetNumBitsWritten() + 7) >> 3;
	buffer->WriteShort(nPayloadBytes);
	buffer->WriteBytes(m_DataOut.GetData(), nPayloadBytes);

	return !buffer->IsOverflowed();
}

bool NET_ScriptMessage::Process(void)
{
	if (m_bIsTyped)
	{
		// Typed = client→server remote function call
		if (m_pMessageHandler)
		{
			CClient* pClient = static_cast<CClient*>(m_pMessageHandler);
			if (!ScriptRemoteServer_ProcessMessage(pClient, this))
				Warning(eDLL_T::SERVER, "NET_ScriptMessage: failed to process message\n");
		}
	}
	else
	{
		if (m_DataIn.GetNumBitsLeft() >= 48)
		{
			const uint32_t magic = m_DataIn.ReadUBitLong(32);
			const unsigned nRttMs = m_DataIn.ReadUBitLong(16);
			if (magic == 0x31545242u && nRttMs <= 2000 && m_pMessageHandler)
				BridgeLatency_OnClientReport(static_cast<CClient*>(m_pMessageHandler), nRttMs);
		}
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////////
// Below functions are hooked as playlist overrides can be abused from the client.
// The client could basically manage the server's playlists. Only allow read/write
// when cheats are enabled.
///////////////////////////////////////////////////////////////////////////////////
bool CLC_SetPlaylistVarOverride::ReadFromBufferImpl(CLC_SetPlaylistVarOverride* thisptr, bf_read* buffer)
{
	// Abusable netmsg; only allow if cheats are enabled.
	if (!sv_cheats->GetBool())
	{
		return false;
	}

	return CLC_SetPlaylistVarOverride_ReadFromBuffer(thisptr, buffer);
}
bool CLC_SetPlaylistVarOverride::WriteToBufferImpl(CLC_SetPlaylistVarOverride* thisptr, bf_write* buffer)
{
	// Abusable netmsg; only allow if cheats are enabled.
	if (!sv_cheats->GetBool())
	{
		return false;
	}

	return CLC_SetPlaylistVarOverride_WriteToBuffer(thisptr, buffer);
}

static ConVar enable_CmdKeyValues("enable_CmdKeyValues", "0", FCVAR_DEVELOPMENTONLY, "Toggle CmdKeyValues transmit and receive.");

static bool CmdKeyValues_ReadGated(Base_CmdKeyValues* thisptr, bf_read* buffer,
	bool (*orig)(Base_CmdKeyValues*, bf_read*))
{
	if (!buffer)
		return false;

	const ssize_t left = buffer->GetNumBitsLeft();
	if (left < 32)
	{
		if (left > 0)
			buffer->SeekRelative(left);
		return false;
	}

	const unsigned nBits = buffer->PeekUBitLong(32);
	unsigned nBytes = 0xFFFFFFFFu;
	if (nBits <= (0xFFFFFFFFu - 7u))
		nBytes = (nBits + 7u) / 8u;

	const bool tooBig = nBytes > 65536u || static_cast<ssize_t>(nBits) > (left - 32);
	if (!enable_CmdKeyValues.GetBool() || tooBig || !orig)
	{
		const ssize_t payload = (static_cast<ssize_t>(nBits) <= (left - 32))
			? static_cast<ssize_t>(nBits) : (left - 32);
		buffer->SeekRelative(32 + payload);
		return false;
	}

	return orig(thisptr, buffer);
}

static bool CLC_CmdKeyValues_ReadFromBufferImpl(Base_CmdKeyValues* thisptr, bf_read* buffer)
{
	return CmdKeyValues_ReadGated(thisptr, buffer, CLC_CmdKeyValues_ReadFromBuffer);
}
static bool CLC_CmdKeyValues_WriteToBufferImpl(Base_CmdKeyValues* thisptr, bf_write* buffer)
{
	if (!enable_CmdKeyValues.GetBool() || !CLC_CmdKeyValues_WriteToBuffer)
		return false;
	return CLC_CmdKeyValues_WriteToBuffer(thisptr, buffer);
}
static bool SVC_CmdKeyValues_ReadFromBufferImpl(Base_CmdKeyValues* thisptr, bf_read* buffer)
{
	return CmdKeyValues_ReadGated(thisptr, buffer, SVC_CmdKeyValues_ReadFromBuffer);
}
static bool SVC_CmdKeyValues_WriteToBufferImpl(Base_CmdKeyValues* thisptr, bf_write* buffer)
{
	if (!enable_CmdKeyValues.GetBool() || !SVC_CmdKeyValues_WriteToBuffer)
		return false;
	return SVC_CmdKeyValues_WriteToBuffer(thisptr, buffer);
}

///////////////////////////////////////////////////////////////////////////////////
// below functions are hooked as 'CmdKeyValues' isn't really used in this game, but
// still exploitable on the server. the 'OnPlayerAward' command calls the function
// 'UTIL_SendClientCommandKVToPlayer' which forwards the keyvalues to all connected clients.
///////////////////////////////////////////////////////////////////////////////////
bool Base_CmdKeyValues::ReadFromBufferImpl(Base_CmdKeyValues* thisptr, bf_read* buffer)
{
	return CmdKeyValues_ReadGated(thisptr, buffer, Base_CmdKeyValues_ReadFromBuffer);
}
bool Base_CmdKeyValues::WriteToBufferImpl(Base_CmdKeyValues* thisptr, bf_write* buffer)
{
	if (!enable_CmdKeyValues.GetBool())
		return false;

	return Base_CmdKeyValues_WriteToBuffer(thisptr, buffer);
}

///////////////////////////////////////////////////////////////////////////////////
// determine whether or not the message can be copied into the replay buffer,
// regardless of the 'CNetMessage::m_Group' type.
///////////////////////////////////////////////////////////////////////////////////
bool CanReplayMessage(const CNetMessage* msg)
{
	switch (msg->GetType())
	{
	// String commands can be abused in a way they get executed
	// on the client that is watching a replay. This happens as
	// the server copies the message into the replay buffer from
	// the client that initially submitted it. Its group type is
	// 'None', so call this to determine whether or not to set
	// the group type to 'NoReplay'. This exploit has been used
	// to connect clients to an arbitrary server during replay.
	case NetMessageType::net_StringCmd:
	// Print and user messages sometimes make their way to the
	// client that is watching a replay, while it should only
	// be broadcasted to the target client. This happens for the 
	// same reason as the 'net_StringCmd' above.
	case NetMessageType::svc_Print:
	{
		return false;
	}
	case NetMessageType::svc_UserMessage:
	{
		SVC_UserMessage* userMsg = (SVC_UserMessage*)msg;

		// Just don't replay console prints.
		if (userMsg->m_nMsgType == UserMessages_t::TextMsg)
		{
			return false;
		}

		// ScriptRemote and connect checksum must not land in the replay buffer.
		if (userMsg->m_nMsgType == UserMessages_t::RemoteFunctionCall ||
			userMsg->m_nMsgType == UserMessages_t::RemoteUntypedFunctionCall ||
			userMsg->m_nMsgType == UserMessages_t::RemoteFunctionCallsChecksum)
		{
			return false;
		}

		return true;
	}
	default:
	{
		return true;
	}
	}
}

void V_NetMessages::Detour(const bool bAttach) const
{
	if (bAttach)
	{
		CMemory::HookVirtualMethod((uintptr_t)g_pBase_CmdKeyValues_VFTable, (LPVOID*)&Base_CmdKeyValues::ReadFromBufferImpl, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&Base_CmdKeyValues_ReadFromBuffer);
		CMemory::HookVirtualMethod((uintptr_t)g_pBase_CmdKeyValues_VFTable, (LPVOID*)&Base_CmdKeyValues::WriteToBufferImpl, NetMessageVtbl::WriteToBuffer, (LPVOID*)&Base_CmdKeyValues_WriteToBuffer);
		if (g_pCLC_CmdKeyValues_VFTable)
		{
			CMemory::HookVirtualMethod((uintptr_t)g_pCLC_CmdKeyValues_VFTable, (LPVOID)CLC_CmdKeyValues_ReadFromBufferImpl, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&CLC_CmdKeyValues_ReadFromBuffer);
			CMemory::HookVirtualMethod((uintptr_t)g_pCLC_CmdKeyValues_VFTable, (LPVOID)CLC_CmdKeyValues_WriteToBufferImpl, NetMessageVtbl::WriteToBuffer, (LPVOID*)&CLC_CmdKeyValues_WriteToBuffer);
		}
		else
		{
			Warning(eDLL_T::ENGINE, "[NET] CLC_CmdKeyValues vtable unresolved -- type 59 RFB unhooked\n");
		}
		if (g_pSVC_CmdKeyValues_VFTable)
		{
			CMemory::HookVirtualMethod((uintptr_t)g_pSVC_CmdKeyValues_VFTable, (LPVOID)SVC_CmdKeyValues_ReadFromBufferImpl, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&SVC_CmdKeyValues_ReadFromBuffer);
			CMemory::HookVirtualMethod((uintptr_t)g_pSVC_CmdKeyValues_VFTable, (LPVOID)SVC_CmdKeyValues_WriteToBufferImpl, NetMessageVtbl::WriteToBuffer, (LPVOID*)&SVC_CmdKeyValues_WriteToBuffer);
		}
		CMemory::HookVirtualMethod((uintptr_t)g_pCLC_SetPlaylistVarOverride_VFTable, (LPVOID*)&CLC_SetPlaylistVarOverride::ReadFromBufferImpl, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&CLC_SetPlaylistVarOverride_ReadFromBuffer);
		CMemory::HookVirtualMethod((uintptr_t)g_pCLC_SetPlaylistVarOverride_VFTable, (LPVOID*)&CLC_SetPlaylistVarOverride::WriteToBufferImpl, NetMessageVtbl::WriteToBuffer, (LPVOID*)&CLC_SetPlaylistVarOverride_WriteToBuffer);
	}
	else
	{
		void* hkRestore = nullptr;
		CMemory::HookVirtualMethod((uintptr_t)g_pBase_CmdKeyValues_VFTable, (LPVOID)Base_CmdKeyValues_ReadFromBuffer, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&hkRestore);
		CMemory::HookVirtualMethod((uintptr_t)g_pBase_CmdKeyValues_VFTable, (LPVOID)Base_CmdKeyValues_WriteToBuffer, NetMessageVtbl::WriteToBuffer, (LPVOID*)&hkRestore);
		if (g_pCLC_CmdKeyValues_VFTable && CLC_CmdKeyValues_ReadFromBuffer)
			CMemory::HookVirtualMethod((uintptr_t)g_pCLC_CmdKeyValues_VFTable, (LPVOID)CLC_CmdKeyValues_ReadFromBuffer, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&hkRestore);
		if (g_pCLC_CmdKeyValues_VFTable && CLC_CmdKeyValues_WriteToBuffer)
			CMemory::HookVirtualMethod((uintptr_t)g_pCLC_CmdKeyValues_VFTable, (LPVOID)CLC_CmdKeyValues_WriteToBuffer, NetMessageVtbl::WriteToBuffer, (LPVOID*)&hkRestore);
		if (g_pSVC_CmdKeyValues_VFTable && SVC_CmdKeyValues_ReadFromBuffer)
			CMemory::HookVirtualMethod((uintptr_t)g_pSVC_CmdKeyValues_VFTable, (LPVOID)SVC_CmdKeyValues_ReadFromBuffer, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&hkRestore);
		if (g_pSVC_CmdKeyValues_VFTable && SVC_CmdKeyValues_WriteToBuffer)
			CMemory::HookVirtualMethod((uintptr_t)g_pSVC_CmdKeyValues_VFTable, (LPVOID)SVC_CmdKeyValues_WriteToBuffer, NetMessageVtbl::WriteToBuffer, (LPVOID*)&hkRestore);
		CMemory::HookVirtualMethod((uintptr_t)g_pCLC_SetPlaylistVarOverride_VFTable, (LPVOID)CLC_SetPlaylistVarOverride_ReadFromBuffer, NetMessageVtbl::ReadFromBuffer, (LPVOID*)&hkRestore);
		CMemory::HookVirtualMethod((uintptr_t)g_pCLC_SetPlaylistVarOverride_VFTable, (LPVOID)CLC_SetPlaylistVarOverride_WriteToBuffer, NetMessageVtbl::WriteToBuffer, (LPVOID*)&hkRestore);
	}
}
#endif // CLIENT_DLL
