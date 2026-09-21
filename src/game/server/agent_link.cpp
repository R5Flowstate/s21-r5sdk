//=============================================================================//
//
// Purpose: loopback line socket for a local build agent (headless ReMap).
//          SCRIPT <code> runs on the server VM from the game frame, so the
//          agent's props spawn as it places them. Dev only: on with -devsdk
//          (-noagentlink turns it off, -agentlink <port> moves it), binds
//          127.0.0.1, one client at a time, per-launch token written to
//          platform/agentlink.token.
//
//=============================================================================//
#include "core/stdafx.h"
#include "game/server/agent_link.h"
#include "vscript/vscript.h"
#include "tier0/threadtools.h"
#include "tier0/commandline.h"
#include "filesystem/filesystem.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>

#include "game/shared/scriptremotefunctions_server.h"

namespace
{
	constexpr int kDefaultPort = 37019;
	constexpr int kLineMax = 1000;
	constexpr int kQueueCap = 256;
	constexpr int kQueueWaitMs = 2000;

	bool s_bEnabled = false;
	SOCKET s_hListen = INVALID_SOCKET;
	HANDLE s_hThread = nullptr;
	volatile LONG s_nStop = 0;
	char s_szToken[65] = {};

	CThreadFastMutex s_QueueMutex;
	char s_szQueue[kQueueCap][kLineMax + 1];
	int s_nHead = 0, s_nTail = 0, s_nCount = 0;
	volatile LONG s_nDrained = 0;

	static bool MakeToken(void)
	{
		unsigned char raw[32];
		HCRYPTPROV hProv = 0;
		if (!CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
			return false;
		const BOOL ok = CryptGenRandom(hProv, sizeof(raw), raw);
		CryptReleaseContext(hProv, 0);
		if (!ok)
			return false;
		for (int i = 0; i < 32; ++i)
			V_snprintf(s_szToken + i * 2, 3, "%02x", raw[i]);
		FileHandle_t h = FileSystem()->Open("agentlink.token", "wb", "PLATFORM");
		if (h == FILESYSTEM_INVALID_HANDLE)
			return false;
		FileSystem()->Write(s_szToken, 64, h);
		FileSystem()->Close(h);
		return true;
	}

	static void Send(SOCKET s, const char* psz)
	{
		send(s, psz, (int)strlen(psz), 0);
	}

	static bool Enqueue(const char* pszCode)
	{
		CAutoLockT<CThreadFastMutex> lock(s_QueueMutex);
		if (s_nCount >= kQueueCap)
			return false;
		V_strncpy(s_szQueue[s_nTail], pszCode, sizeof(s_szQueue[0]));
		s_nTail = (s_nTail + 1) % kQueueCap;
		++s_nCount;
		return true;
	}

	static int QueueCount(void)
	{
		CAutoLockT<CThreadFastMutex> lock(s_QueueMutex);
		return s_nCount;
	}

	static void HandleLine(SOCKET s, char* pszLine, bool& bAuthed)
	{
		size_t n = strlen(pszLine);
		while (n && (pszLine[n - 1] == '\r' || pszLine[n - 1] == ' '))
			pszLine[--n] = '\0';
		if (!n)
			return;
		if (strncmp(pszLine, "AUTH ", 5) == 0)
		{
			bAuthed = strcmp(pszLine + 5, s_szToken) == 0;
			Send(s, bAuthed ? "OK\n" : "ERR bad token\n");
			return;
		}
		if (!bAuthed)
		{
			Send(s, "ERR auth first\n");
			return;
		}
		if (strcmp(pszLine, "PING") == 0)
		{
			char szReply[64];
			V_snprintf(szReply, sizeof(szReply), "OK queued %d drained %ld\n", QueueCount(), s_nDrained);
			Send(s, szReply);
			return;
		}
		if (strcmp(pszLine, "FLUSH") == 0)
		{
			const DWORD nStart = GetTickCount();
			while (QueueCount() > 0 && GetTickCount() - nStart < (DWORD)kQueueWaitMs)
				Sleep(5);
			Send(s, QueueCount() == 0 ? "OK\n" : "BUSY\n");
			return;
		}
		if (strncmp(pszLine, "SCRIPT ", 7) == 0 && pszLine[7])
		{
			const DWORD nStart = GetTickCount();
			while (!Enqueue(pszLine + 7))
			{
				if (GetTickCount() - nStart > (DWORD)kQueueWaitMs)
				{
					Send(s, "BUSY\n");
					return;
				}
				Sleep(5);
			}
			Send(s, "OK\n");
			return;
		}
		Send(s, "ERR unknown op\n");
	}

	static void ServeClient(SOCKET s)
	{
		char buf[8192];
		int nUsed = 0;
		bool bAuthed = false;
		Send(s, "AGENTLINK 1\n");
		while (!s_nStop)
		{
			const int r = recv(s, buf + nUsed, (int)sizeof(buf) - 1 - nUsed, 0);
			if (r <= 0)
				return;
			nUsed += r;
			buf[nUsed] = '\0';
			char* pStart = buf;
			for (;;)
			{
				char* pNl = strchr(pStart, '\n');
				if (!pNl)
					break;
				*pNl = '\0';
				if (pNl - pStart <= kLineMax)
					HandleLine(s, pStart, bAuthed);
				else
					Send(s, "ERR line too long\n");
				pStart = pNl + 1;
			}
			const int nRest = (int)(buf + nUsed - pStart);
			memmove(buf, pStart, nRest);
			nUsed = nRest;
			if (nUsed >= (int)sizeof(buf) - 1)
				nUsed = 0;
		}
	}

	static DWORD WINAPI ListenThread(LPVOID)
	{
		while (!s_nStop)
		{
			sockaddr_in peer{};
			int nLen = sizeof(peer);
			const SOCKET s = accept(s_hListen, (sockaddr*)&peer, &nLen);
			if (s == INVALID_SOCKET)
				break;
			if (peer.sin_addr.S_un.S_addr != htonl(INADDR_LOOPBACK))
			{
				closesocket(s);
				continue;
			}
			ServeClient(s);
			closesocket(s);
		}
		return 0;
	}

	static void Init(void)
	{
		if (!CommandLine()->CheckParm("-devsdk") || CommandLine()->CheckParm("-noagentlink"))
			return;
		int nPort = CommandLine()->ParmValue("-agentlink", kDefaultPort);
		if (nPort <= 0 || nPort > 65535)
			nPort = kDefaultPort;

		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return;
		if (!MakeToken())
		{
			Warning(eDLL_T::SERVER, "[AGENT-LINK] token write failed -- link disabled\n");
			return;
		}
		s_hListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s_hListen == INVALID_SOCKET)
			return;
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons((u_short)nPort);
		addr.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
		if (bind(s_hListen, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s_hListen, 1) != 0)
		{
			Warning(eDLL_T::SERVER, "[AGENT-LINK] bind 127.0.0.1:%d failed (%d)\n", nPort, WSAGetLastError());
			closesocket(s_hListen);
			s_hListen = INVALID_SOCKET;
			return;
		}
		s_hThread = CreateThread(nullptr, 0, ListenThread, nullptr, 0, nullptr);
		s_bEnabled = s_hThread != nullptr;
		Warning(eDLL_T::SERVER, "[AGENT-LINK] DEV ONLY -- local script execution enabled on 127.0.0.1:%d (token in platform/agentlink.token)\n", nPort);
	}
}

void AgentLink_Think(void)
{
	static bool s_bInitTried = false;
	if (!s_bInitTried)
	{
		s_bInitTried = true;
		Init();
	}
	if (!s_bEnabled)
		return;
	int nRan = 0;
	for (;;)
	{
		char szCode[kLineMax + 1];
		{
			CAutoLockT<CThreadFastMutex> lock(s_QueueMutex);
			if (s_nCount <= 0)
				break;
			V_strncpy(szCode, s_szQueue[s_nHead], sizeof(szCode));
			s_nHead = (s_nHead + 1) % kQueueCap;
			--s_nCount;
		}
		Script_Execute(szCode, SQCONTEXT::SERVER);
		InterlockedIncrement(&s_nDrained);
		++nRan;
	}
	if (nRan)
		ScriptRemoteC2S_DropFnCache();
}
