//=============================================================================//
//
// Purpose: Discord IPC transport for rich presence
//
//=============================================================================//
#include "core/stdafx.h"

#ifndef DEDICATED

#include "discord_ipc.h"
#include "discord_rpc_wrapper.h"
#include "tier0/commandline.h"
#include "tier0/platform.h"

//-----------------------------------------------------------------------------
// Discord IPC Protocol Constants
//-----------------------------------------------------------------------------
#define DISCORD_IPC_VERSION 1
#define DISCORD_RPC_PIPE_NAME "\\\\.\\pipe\\discord-ipc-0"
#define DISCORD_MAX_MESSAGE_SIZE 65536

#pragma pack(push, 1)
struct DiscordIpcHeader
{
	uint32_t opcode;
	uint32_t length;
};
#pragma pack(pop)

//-----------------------------------------------------------------------------
// Static member definitions
//-----------------------------------------------------------------------------
HANDLE CDiscordIpc::s_hPipe = INVALID_HANDLE_VALUE;
bool CDiscordIpc::s_bConnected = false;
bool CDiscordIpc::s_bInitialized = false;
char CDiscordIpc::s_szApplicationId[32] = { 0 };
uint32_t CDiscordIpc::s_nNonce = 0;
DiscordEventHandlers CDiscordIpc::s_Handlers = {};
char CDiscordIpc::s_szReadBuffer[16384] = { 0 };
size_t CDiscordIpc::s_nReadBufferPos = 0;
static DiscordIpcHeader s_pendingHeader = {};
static bool s_bPendingHeader = false;

//-----------------------------------------------------------------------------
// Purpose: Initialize Discord IPC connection
//-----------------------------------------------------------------------------
bool CDiscordIpc::Initialize(const char* applicationId, DiscordEventHandlers* handlers)
{
	if (s_bInitialized)
		return true;

	if (!applicationId || applicationId[0] == '\0' || strlen(applicationId) >= sizeof(s_szApplicationId))
	{
		DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: invalid application ID\n");
		return false;
	}

	for (const char* p = applicationId; *p; ++p)
	{
		if (*p < '0' || *p > '9')
		{
			DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: invalid application ID format\n");
			return false;
		}
	}

	V_strncpy(s_szApplicationId, applicationId, sizeof(s_szApplicationId));

	if (handlers)
		s_Handlers = *handlers;

	s_nNonce = 0;
	s_nReadBufferPos = 0;
	s_bInitialized = true;

	DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: initialized (app %s)\n", s_szApplicationId);

	Connect();

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Shutdown Discord IPC
//-----------------------------------------------------------------------------
void CDiscordIpc::Shutdown(void)
{
	if (!s_bInitialized)
		return;

	Disconnect();

	s_bInitialized = false;
	memset(s_szApplicationId, 0, sizeof(s_szApplicationId));
	memset(&s_Handlers, 0, sizeof(s_Handlers));

	DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: shutdown\n");
}

//-----------------------------------------------------------------------------
// Purpose: Process callbacks and reconnection
//-----------------------------------------------------------------------------
void CDiscordIpc::RunCallbacks(void)
{
	if (!s_bInitialized)
		return;

	if (!s_bConnected)
	{
		static double s_flNextConnectAttempt = 0.0;
		double flCurTime = Plat_FloatTime();

		if (flCurTime >= s_flNextConnectAttempt)
		{
			if (Connect())
				DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: connected\n");
			else
				s_flNextConnectAttempt = flCurTime + 5.0;
		}
		return;
	}

	ReadMessage();
}

//-----------------------------------------------------------------------------
// Purpose: Connect to Discord IPC pipe
//-----------------------------------------------------------------------------
bool CDiscordIpc::Connect(void)
{
	if (s_bConnected)
		return true;

	for (int i = 0; i < 10; i++)
	{
		char pipeName[64];
		V_snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\discord-ipc-%d", i);

		s_hPipe = CreateFileA(
			pipeName,
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			0,
			nullptr
		);

		if (s_hPipe != INVALID_HANDLE_VALUE)
		{
			DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: pipe %s\n", pipeName);

			DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
			SetNamedPipeHandleState(s_hPipe, &mode, nullptr, nullptr);

			if (SendHandshake())
			{
				s_bConnected = true;
				return true;
			}

			CloseHandle(s_hPipe);
			s_hPipe = INVALID_HANDLE_VALUE;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Disconnect from Discord IPC
//-----------------------------------------------------------------------------
void CDiscordIpc::Disconnect(void)
{
	if (s_hPipe != INVALID_HANDLE_VALUE)
	{
		CloseHandle(s_hPipe);
		s_hPipe = INVALID_HANDLE_VALUE;
	}

	const bool bWasConnected = s_bConnected;
	s_bConnected = false;
	s_nReadBufferPos = 0;
	s_bPendingHeader = false;

	if (bWasConnected)
		OnDisconnected(0, "pipe closed");
}

//-----------------------------------------------------------------------------
// Purpose: Send handshake to Discord
//-----------------------------------------------------------------------------
bool CDiscordIpc::SendHandshake(void)
{
	char payload[256];
	int len = V_snprintf(payload, sizeof(payload),
		"{\"v\":%d,\"client_id\":\"%s\"}",
		DISCORD_IPC_VERSION, s_szApplicationId);

	DiscordIpcHeader header;
	header.opcode = static_cast<uint32_t>(DiscordIpcOpcode::HANDSHAKE);
	header.length = static_cast<uint32_t>(len);

	DWORD bytesWritten;

	if (!WriteFile(s_hPipe, &header, sizeof(header), &bytesWritten, nullptr) || bytesWritten != sizeof(header))
	{
		DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: handshake header write failed\n");
		return false;
	}

	if (!WriteFile(s_hPipe, payload, len, &bytesWritten, nullptr) || bytesWritten != static_cast<DWORD>(len))
	{
		DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: handshake payload write failed\n");
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Send a frame to Discord
//-----------------------------------------------------------------------------
bool CDiscordIpc::SendFrame(const char* jsonPayload)
{
	if (!s_bConnected || s_hPipe == INVALID_HANDLE_VALUE)
		return false;

	size_t len = strlen(jsonPayload);
	if (len > DISCORD_MAX_MESSAGE_SIZE)
		return false;

	DiscordIpcHeader header;
	header.opcode = static_cast<uint32_t>(DiscordIpcOpcode::FRAME);
	header.length = static_cast<uint32_t>(len);

	DWORD bytesWritten;

	if (!WriteFile(s_hPipe, &header, sizeof(header), &bytesWritten, nullptr) || bytesWritten != sizeof(header))
	{
		Disconnect();
		return false;
	}

	if (!WriteFile(s_hPipe, jsonPayload, static_cast<DWORD>(len), &bytesWritten, nullptr) || bytesWritten != static_cast<DWORD>(len))
	{
		Disconnect();
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Read messages from Discord
//-----------------------------------------------------------------------------
bool CDiscordIpc::ReadMessage(void)
{
	if (!s_bConnected || s_hPipe == INVALID_HANDLE_VALUE)
		return false;

	DWORD bytesAvailable = 0;
	if (!PeekNamedPipe(s_hPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr))
	{
		Disconnect();
		return false;
	}

	if (!s_bPendingHeader)
	{
		if (bytesAvailable < sizeof(DiscordIpcHeader))
			return true;

		DiscordIpcHeader header;
		DWORD bytesRead = 0;
		if (!ReadFile(s_hPipe, &header, sizeof(header), &bytesRead, nullptr) || bytesRead != sizeof(header))
		{
			Disconnect();
			return false;
		}

		if (header.length > sizeof(s_szReadBuffer) - 1)
		{
			DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: message too large (%u bytes)\n", header.length);
			Disconnect();
			return false;
		}

		s_pendingHeader = header;
		s_bPendingHeader = true;
		s_nReadBufferPos = 0;
		bytesAvailable -= static_cast<DWORD>(sizeof(DiscordIpcHeader));
	}

	if (s_bPendingHeader)
	{
		const DWORD nNeed = s_pendingHeader.length - static_cast<DWORD>(s_nReadBufferPos);
		const DWORD toRead = min(bytesAvailable, nNeed);
		if (toRead > 0)
		{
			DWORD bytesRead = 0;
			if (!ReadFile(s_hPipe, s_szReadBuffer + s_nReadBufferPos, toRead, &bytesRead, nullptr) || bytesRead != toRead)
			{
				Disconnect();
				return false;
			}
			s_nReadBufferPos += bytesRead;
		}

		if (s_nReadBufferPos != s_pendingHeader.length)
			return true;

		s_szReadBuffer[s_pendingHeader.length] = '\0';

		switch (static_cast<DiscordIpcOpcode>(s_pendingHeader.opcode))
		{
		case DiscordIpcOpcode::FRAME:
			ProcessMessage(s_szReadBuffer, s_pendingHeader.length);
			break;

		case DiscordIpcOpcode::CLOSE:
		{
			static bool s_bCloseAnnounced = false;
			if (!s_bCloseAnnounced)
			{
				s_bCloseAnnounced = true;
				Warning(eDLL_T::CLIENT, "[DISCORD] IPC: closed by Discord: %s\n", s_szReadBuffer);
			}
			Disconnect();
			break;
		}

		case DiscordIpcOpcode::PING:
		{
			DiscordIpcHeader pongHeader;
			pongHeader.opcode = static_cast<uint32_t>(DiscordIpcOpcode::PONG);
			pongHeader.length = s_pendingHeader.length;

			DWORD bytesWritten = 0;
			if (!WriteFile(s_hPipe, &pongHeader, sizeof(pongHeader), &bytesWritten, nullptr) || bytesWritten != sizeof(pongHeader) ||
				!WriteFile(s_hPipe, s_szReadBuffer, s_pendingHeader.length, &bytesWritten, nullptr) || bytesWritten != s_pendingHeader.length)
			{
				Disconnect();
				return false;
			}
			break;
		}

		default:
			break;
		}

		s_bPendingHeader = false;
		s_nReadBufferPos = 0;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Process incoming JSON message
//-----------------------------------------------------------------------------
void CDiscordIpc::ProcessMessage(const char* json, size_t length)
{
	(void)length;

	if (strstr(json, "\"evt\":\"READY\"") || strstr(json, "\"cmd\":\"DISPATCH\""))
	{
		const char* userIdStart = strstr(json, "\"id\":\"");
		const char* usernameStart = strstr(json, "\"username\":\"");
		const char* discriminatorStart = strstr(json, "\"discriminator\":\"");

		char userId[32] = { 0 };
		char username[64] = { 0 };
		char discriminator[8] = { 0 };

		if (userIdStart)
		{
			userIdStart += 6;
			const char* userIdEnd = strchr(userIdStart, '\"');
			if (userIdEnd && userIdEnd >= userIdStart)
			{
				const size_t len = static_cast<size_t>(userIdEnd - userIdStart);
				const size_t toCopy = min(len, sizeof(userId) - 1);
				memcpy(userId, userIdStart, toCopy);
				userId[toCopy] = '\0';
			}
		}

		if (usernameStart)
		{
			usernameStart += 12;
			const char* usernameEnd = strchr(usernameStart, '\"');
			if (usernameEnd && usernameEnd >= usernameStart)
			{
				const size_t len = static_cast<size_t>(usernameEnd - usernameStart);
				const size_t toCopy = min(len, sizeof(username) - 1);
				memcpy(username, usernameStart, toCopy);
				username[toCopy] = '\0';
			}
		}

		if (discriminatorStart)
		{
			discriminatorStart += 17;
			const char* discriminatorEnd = strchr(discriminatorStart, '\"');
			if (discriminatorEnd && discriminatorEnd >= discriminatorStart)
			{
				const size_t len = static_cast<size_t>(discriminatorEnd - discriminatorStart);
				const size_t toCopy = min(len, sizeof(discriminator) - 1);
				memcpy(discriminator, discriminatorStart, toCopy);
				discriminator[toCopy] = '\0';
			}
		}

		OnReady(userId, username, discriminator, "");
	}
	else if (strstr(json, "\"evt\":\"ERROR\""))
	{
		OnError(-1, "Discord RPC error");
	}
}

//-----------------------------------------------------------------------------
// Purpose: Called when Discord is ready
//-----------------------------------------------------------------------------
void CDiscordIpc::OnReady(const char* userId, const char* username, const char* discriminator, const char* avatar)
{
	DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: ready (user: %s#%s)\n", username, discriminator);

	if (s_Handlers.ready)
	{
		DiscordRPCUser user;
		user.userId = userId;
		user.username = username;
		user.discriminator = discriminator;
		user.avatar = avatar;
		s_Handlers.ready(&user);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Called when Discord disconnects
//-----------------------------------------------------------------------------
void CDiscordIpc::OnDisconnected(int errorCode, const char* message)
{
	DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: disconnected (%d: %s)\n", errorCode, message ? message : "");

	if (s_Handlers.disconnected)
		s_Handlers.disconnected(errorCode, message);
}

//-----------------------------------------------------------------------------
// Purpose: Called on Discord error
//-----------------------------------------------------------------------------
void CDiscordIpc::OnError(int errorCode, const char* message)
{
	DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: error (%d: %s)\n", errorCode, message ? message : "");

	if (s_Handlers.errored)
		s_Handlers.errored(errorCode, message);
}

//-----------------------------------------------------------------------------
// Purpose: Update Discord presence
//-----------------------------------------------------------------------------
bool CDiscordIpc::UpdatePresence(const DiscordRichPresence* presence)
{
	if (!s_bConnected || !presence)
		return false;

	char buffer[8192];
	size_t len = BuildSetActivityPayload(buffer, sizeof(buffer), presence);
	if (len == 0)
		return false;

	return SendFrame(buffer);
}

//-----------------------------------------------------------------------------
// Purpose: Clear Discord presence
//-----------------------------------------------------------------------------
void CDiscordIpc::ClearPresence(void)
{
	if (!s_bConnected)
		return;

	char buffer[512];
	size_t len = BuildClearActivityPayload(buffer, sizeof(buffer));
	if (len > 0)
		SendFrame(buffer);
}

//-----------------------------------------------------------------------------
// Purpose: Replace event handlers
//-----------------------------------------------------------------------------
void CDiscordIpc::UpdateHandlers(DiscordEventHandlers* handlers)
{
	if (!s_bInitialized || !handlers)
		return;

	s_Handlers = *handlers;
}

//-----------------------------------------------------------------------------
// Purpose: Check if connected
//-----------------------------------------------------------------------------
bool CDiscordIpc::IsConnected(void)
{
	return s_bConnected;
}

//-----------------------------------------------------------------------------
// Helper: Escape JSON string
//-----------------------------------------------------------------------------
static size_t EscapeJsonString(char* dest, size_t destSize, const char* src)
{
	if (!src || !dest || destSize == 0)
		return 0;

	size_t pos = 0;
	while (*src && pos < destSize - 1)
	{
		char c = *src++;
		if (c == '\"' || c == '\\')
		{
			if (pos + 2 >= destSize)
				break;
			dest[pos++] = '\\';
			dest[pos++] = c;
		}
		else if (c == '\n')
		{
			if (pos + 2 >= destSize)
				break;
			dest[pos++] = '\\';
			dest[pos++] = 'n';
		}
		else if (c == '\r')
		{
			if (pos + 2 >= destSize)
				break;
			dest[pos++] = '\\';
			dest[pos++] = 'r';
		}
		else if (c == '\t')
		{
			if (pos + 2 >= destSize)
				break;
			dest[pos++] = '\\';
			dest[pos++] = 't';
		}
		else if (c == '\b')
		{
			if (pos + 2 >= destSize)
				break;
			dest[pos++] = '\\';
			dest[pos++] = 'b';
		}
		else if (c == '\f')
		{
			if (pos + 2 >= destSize)
				break;
			dest[pos++] = '\\';
			dest[pos++] = 'f';
		}
		else if (c >= 0x20)
		{
			dest[pos++] = c;
		}
	}
	dest[pos] = '\0';
	return pos;
}

// Bail the payload build instead of overrunning the frame buffer. All fields
// are small and escaped up front, so this never fires on real input.
#define DISCORD_JSON_GUARD() do { if (pos >= bufferSize - 128) return 0; } while (0)

//-----------------------------------------------------------------------------
// Purpose: Build SET_ACTIVITY payload
//-----------------------------------------------------------------------------
size_t CDiscordIpc::BuildSetActivityPayload(char* buffer, size_t bufferSize, const DiscordRichPresence* presence)
{
	char escapedState[256];
	char escapedDetails[256];
	char escapedLargeImageKey[64];
	char escapedLargeImageText[256];
	char escapedSmallImageKey[64];
	char escapedSmallImageText[256];
	char escapedPartyId[256];

	size_t pos = 0;
	uint32_t nonce = ++s_nNonce;

	DISCORD_JSON_GUARD();
	pos += V_snprintf(buffer + pos, bufferSize - pos,
		"{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{",
		GetCurrentProcessId());

	bool needsComma = false;

	if (presence->state && presence->state[0])
	{
		EscapeJsonString(escapedState, sizeof(escapedState), presence->state);
		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos,
			"\"state\":\"%s\"", escapedState);
		needsComma = true;
	}

	if (presence->details && presence->details[0])
	{
		EscapeJsonString(escapedDetails, sizeof(escapedDetails), presence->details);
		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos,
			"%s\"details\":\"%s\"",
			needsComma ? "," : "", escapedDetails);
		needsComma = true;
	}

	if (presence->startTimestamp > 0 || presence->endTimestamp > 0)
	{
		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos,
			"%s\"timestamps\":{", needsComma ? "," : "");

		bool timestampComma = false;
		if (presence->startTimestamp > 0)
		{
			DISCORD_JSON_GUARD();
			pos += V_snprintf(buffer + pos, bufferSize - pos,
				"\"start\":%lld", presence->startTimestamp);
			timestampComma = true;
		}
		if (presence->endTimestamp > 0)
		{
			DISCORD_JSON_GUARD();
			pos += V_snprintf(buffer + pos, bufferSize - pos,
				"%s\"end\":%lld",
				timestampComma ? "," : "", presence->endTimestamp);
		}
		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos, "}");
		needsComma = true;
	}

	bool hasAssets = (presence->largeImageKey && presence->largeImageKey[0]) ||
					 (presence->smallImageKey && presence->smallImageKey[0]);
	if (hasAssets)
	{
		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos,
			"%s\"assets\":{", needsComma ? "," : "");

		bool assetComma = false;
		if (presence->largeImageKey && presence->largeImageKey[0])
		{
			EscapeJsonString(escapedLargeImageKey, sizeof(escapedLargeImageKey), presence->largeImageKey);
			DISCORD_JSON_GUARD();
			pos += V_snprintf(buffer + pos, bufferSize - pos,
				"\"large_image\":\"%s\"", escapedLargeImageKey);
			assetComma = true;

			if (presence->largeImageText && presence->largeImageText[0])
			{
				EscapeJsonString(escapedLargeImageText, sizeof(escapedLargeImageText), presence->largeImageText);
				DISCORD_JSON_GUARD();
				pos += V_snprintf(buffer + pos, bufferSize - pos,
					",\"large_text\":\"%s\"", escapedLargeImageText);
			}
		}

		if (presence->smallImageKey && presence->smallImageKey[0])
		{
			EscapeJsonString(escapedSmallImageKey, sizeof(escapedSmallImageKey), presence->smallImageKey);
			DISCORD_JSON_GUARD();
			pos += V_snprintf(buffer + pos, bufferSize - pos,
				"%s\"small_image\":\"%s\"",
				assetComma ? "," : "", escapedSmallImageKey);

			if (presence->smallImageText && presence->smallImageText[0])
			{
				EscapeJsonString(escapedSmallImageText, sizeof(escapedSmallImageText), presence->smallImageText);
				DISCORD_JSON_GUARD();
				pos += V_snprintf(buffer + pos, bufferSize - pos,
					",\"small_text\":\"%s\"", escapedSmallImageText);
			}
		}

		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos, "}");
		needsComma = true;
	}

	bool hasParty = (presence->partyId && presence->partyId[0]) ||
					(presence->partySize > 0 && presence->partyMax > 0);
	if (hasParty)
	{
		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos,
			"%s\"party\":{", needsComma ? "," : "");

		bool partyComma = false;
		if (presence->partyId && presence->partyId[0])
		{
			EscapeJsonString(escapedPartyId, sizeof(escapedPartyId), presence->partyId);
			DISCORD_JSON_GUARD();
			pos += V_snprintf(buffer + pos, bufferSize - pos,
				"\"id\":\"%s\"", escapedPartyId);
			partyComma = true;
		}

		if (presence->partySize > 0 && presence->partyMax > 0)
		{
			DISCORD_JSON_GUARD();
			pos += V_snprintf(buffer + pos, bufferSize - pos,
				"%s\"size\":[%d,%d]",
				partyComma ? "," : "", presence->partySize, presence->partyMax);
		}

		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos, "}");
		needsComma = true;
	}

	// Secrets are never published by this product; the block stays so a
	// future caller cannot silently start sending them without noticing it.
	bool hasSecrets = (presence->matchSecret && presence->matchSecret[0]) ||
					  (presence->joinSecret && presence->joinSecret[0]) ||
					  (presence->spectateSecret && presence->spectateSecret[0]);
	if (hasSecrets)
	{
		DevMsg(eDLL_T::CLIENT, "[DISCORD] IPC: secrets in presence are dropped\n");
	}

	const bool hasButton0 = presence->button0Label && presence->button0Label[0] && presence->button0Url && presence->button0Url[0];
	const bool hasButton1 = presence->button1Label && presence->button1Label[0] && presence->button1Url && presence->button1Url[0];
	if (hasButton0 || hasButton1)
	{
		char escapedLabel[64];
		char escapedUrl[512];
		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos, "%s\"buttons\":[", needsComma ? "," : "");
		bool buttonComma = false;
		if (hasButton0)
		{
			EscapeJsonString(escapedLabel, sizeof(escapedLabel), presence->button0Label);
			EscapeJsonString(escapedUrl, sizeof(escapedUrl), presence->button0Url);
			DISCORD_JSON_GUARD();
			pos += V_snprintf(buffer + pos, bufferSize - pos, "{\"label\":\"%s\",\"url\":\"%s\"}", escapedLabel, escapedUrl);
			buttonComma = true;
		}
		if (hasButton1)
		{
			EscapeJsonString(escapedLabel, sizeof(escapedLabel), presence->button1Label);
			EscapeJsonString(escapedUrl, sizeof(escapedUrl), presence->button1Url);
			DISCORD_JSON_GUARD();
			pos += V_snprintf(buffer + pos, bufferSize - pos, "%s{\"label\":\"%s\",\"url\":\"%s\"}", buttonComma ? "," : "", escapedLabel, escapedUrl);
		}
		DISCORD_JSON_GUARD();
		pos += V_snprintf(buffer + pos, bufferSize - pos, "]");
		needsComma = true;
	}

	DISCORD_JSON_GUARD();
	pos += V_snprintf(buffer + pos, bufferSize - pos,
		"%s\"instance\":%s",
		needsComma ? "," : "", presence->instance ? "true" : "false");

	DISCORD_JSON_GUARD();
	pos += V_snprintf(buffer + pos, bufferSize - pos,
		"}},\"nonce\":\"%u\"}", nonce);

	if (pos >= bufferSize)
		return 0;

	return pos;
}

#undef DISCORD_JSON_GUARD

//-----------------------------------------------------------------------------
// Purpose: Build CLEAR_ACTIVITY payload
//-----------------------------------------------------------------------------
size_t CDiscordIpc::BuildClearActivityPayload(char* buffer, size_t bufferSize)
{
	uint32_t nonce = ++s_nNonce;
	return V_snprintf(buffer, bufferSize,
		"{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu},\"nonce\":\"%u\"}",
		GetCurrentProcessId(), nonce);
}

#endif // !DEDICATED
