//=============================================================================//
//
// Purpose: Latch the launcher loopback RCON session (hosted dedi console).
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/server/sv_rcon_launcher.h"
#include "engine/server/sv_rcon.h"
#include "engine/shared/shared_rcon.h"
#include "tier1/cvar.h"
#include "engine/cmd.h"

namespace
{
	constexpr int kPasswordMax = 128;
	constexpr int kDefaultPort = 37017;
	constexpr int kHostMaxConnections = 2; // launcher setmode + hosted client console
	constexpr int kHostExecPerSecond = 20;
	constexpr char kEnvFromLauncher[] = "FROM_R5F_LAUNCHER";
	constexpr char kEnvPassword[] = "R5F_LOCAL_RCON";
	constexpr char kEnvPort[] = "R5F_LOCAL_RCON_PORT";
	constexpr char kEnvHostReady[] = "R5F_HOST_READY";
	constexpr char kSetModeCmd[] = "bridge_setmode";
	// Must match HostReadyGate.EventPrefix in the launcher.
	constexpr char kHostReadyPrefix[] = "Local\\r5f-host-";

	bool s_bLatched = false;
	bool s_bActive = false;
	bool s_bApplied = false;
	int s_nPort = kDefaultPort;
	char s_szPassword[kPasswordMax] = {};

	static void WipeEnv(const char* pszName)
	{
		SetEnvironmentVariableA(pszName, nullptr);
	}

	static bool IsSafeToken(const char* psz)
	{
		if (!psz || !psz[0])
			return false;

		for (const char* p = psz; *p; ++p)
		{
			const char c = *p;
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
				continue;
			return false;
		}

		return true;
	}

	static bool ValueLooksLikeSetMode(const char* pszValue)
	{
		if (!pszValue || !pszValue[0])
			return false;

		for (const char* p = pszValue; *p; ++p)
		{
			const char c = *p;
			if (c == ';' || c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '\\')
				return false;
		}

		CCommand cmd;
		cmd.Tokenize(pszValue, cmd_source_t::kCommandSrcCode);
		if (cmd.ArgC() != 3)
			return false;
		if (V_strcmp(cmd.Arg(0), kSetModeCmd) != 0)
			return false;
		return IsSafeToken(cmd.Arg(1)) && IsSafeToken(cmd.Arg(2));
	}

	static bool ValueHasLineBreak(const char* psz)
	{
		if (!psz)
			return false;
		for (const char* p = psz; *p; ++p)
		{
			if (*p == '\n' || *p == '\r')
				return true;
		}
		return false;
	}

	static bool IsRconControlPlane(const char* psz)
	{
		if (!psz || !psz[0])
			return false;
		if (V_strnicmp(psz, "sv_rcon_", 8) == 0)
			return true;
		if (V_strnicmp(psz, "rcon_", 5) == 0)
			return true;
		return V_stricmp(psz, "sv_bridge_admin_elevation") == 0;
	}

	static bool AllowHostValue(const char* pszCommand, const char* pszValue)
	{
		if (!pszCommand || !pszCommand[0])
			return false;

		const char* const pszVal = pszValue ? pszValue : "";
		if (V_strlen(pszVal) >= 512)
			return false;
		if (ValueHasLineBreak(pszVal))
			return false;

		const bool bScript = V_stricmp(pszCommand, "script") == 0;
		if (!bScript)
		{
			for (const char* p = pszVal; *p; ++p)
			{
				if (*p == ';')
					return false;
			}
		}

		CCommand cmd;
		if (pszVal[0])
		{
			cmd.Tokenize(pszVal, cmd_source_t::kCommandSrcCode);
			if (cmd.ArgC() < 1 || V_strcmp(cmd.Arg(0), pszCommand) != 0)
				return false;
		}

		if (V_strcmp(pszCommand, kSetModeCmd) == 0)
			return ValueLooksLikeSetMode(pszVal);

		if (IsRconControlPlane(pszCommand))
			return false;

		if (!g_pCVar || !g_pCVar->FindCommandBase(pszCommand))
			return false;

		return true;
	}

	static void PinSessionConVars(void)
	{
		if (!s_bActive || !g_pCVar)
			return;

		if (ConVar* pLoop = g_pCVar->FindVar("sv_rcon_useloopbacksocket"))
		{
			if (!pLoop->GetBool())
				pLoop->SetValue(1);
		}
		if (ConVar* pEnc = g_pCVar->FindVar("rcon_encryptframes"))
		{
			if (pEnc->GetBool())
				pEnc->SetValue(0);
		}
		if (ConVar* pPort = g_pCVar->FindVar("sv_rcon_port"))
		{
			if (pPort->GetInt() != s_nPort)
				pPort->SetValue(s_nPort);
		}
		if (ConVar* pMax = g_pCVar->FindVar("sv_rcon_maxconnections"))
		{
			if (pMax->GetInt() < kHostMaxConnections)
				pMax->SetValue(kHostMaxConnections);
		}
		if (ConVar* pElev = g_pCVar->FindVar("sv_bridge_admin_elevation"))
		{
			if (pElev->GetBool())
				pElev->SetValue(0);
		}
	}

	static void LatchFromEnv(void)
	{
		if (s_bLatched)
			return;
		s_bLatched = true;

		char szFrom[8] = {};
		GetEnvironmentVariableA(kEnvFromLauncher, szFrom, sizeof(szFrom));

		char szPassword[kPasswordMax] = {};
		GetEnvironmentVariableA(kEnvPassword, szPassword, sizeof(szPassword));

		char szPort[16] = {};
		GetEnvironmentVariableA(kEnvPort, szPort, sizeof(szPort));

		WipeEnv(kEnvPassword);
		WipeEnv(kEnvPort);

		if (V_strcmp(szFrom, "1") != 0)
		{
			SecureZeroMemory(szPassword, sizeof(szPassword));
			return;
		}

		const size_t nLen = V_strlen(szPassword);
		if (nLen < RCON_MIN_PASSWORD_LEN || nLen >= sizeof(s_szPassword))
		{
			Warning(eDLL_T::SERVER, "[RCON] launcher session ignored: password length %zu\n", nLen);
			SecureZeroMemory(szPassword, sizeof(szPassword));
			return;
		}

		int nPort = kDefaultPort;
		if (szPort[0])
		{
			const int parsed = atoi(szPort);
			if (parsed >= 1 && parsed <= 65535)
				nPort = parsed;
		}

		V_strncpy(s_szPassword, szPassword, sizeof(s_szPassword));
		SecureZeroMemory(szPassword, sizeof(szPassword));
		s_nPort = nPort;
		s_bActive = true;
		Msg(eDLL_T::SERVER, "[RCON] launcher session latched (loopback port %d, host console)\n", s_nPort);
	}

	static bool ProbeAddrOccupied(const sockaddr* pAddr, int nAddrLen)
	{
		const SOCKET hSocket = ::socket(pAddr->sa_family, SOCK_STREAM, IPPROTO_TCP);
		if (hSocket == INVALID_SOCKET)
			return false; // Fail open: the bind attempt below reports the real error.

		u_long nNonBlocking = 1;
		if (::ioctlsocket(hSocket, FIONBIO, &nNonBlocking) == SOCKET_ERROR)
		{
			::closesocket(hSocket);
			return false;
		}

		if (::connect(hSocket, pAddr, nAddrLen) == 0)
		{
			::closesocket(hSocket);
			return true;
		}
		if (WSAGetLastError() != WSAEWOULDBLOCK)
		{
			::closesocket(hSocket);
			return false; // Refused/unreachable: the port is free.
		}

		fd_set writeFds;
		FD_ZERO(&writeFds);
		FD_SET(static_cast<u_int>(hSocket), &writeFds);

		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 0;

		bool bAccepted = false;
		if (::select(0, nullptr, &writeFds, nullptr, &tv) > 0 && FD_ISSET(hSocket, &writeFds))
		{
			// Writable alone is not proof: a refused loopback connect also
			// wakes select. Confirm the handshake actually completed.
			int nErr = 0;
			int nErrLen = sizeof(nErr);
			if (::getsockopt(hSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&nErr), &nErrLen) == 0 && nErr == 0)
				bAccepted = true;
		}

		::closesocket(hSocket);
		return bAccepted; // Zero bytes sent either way.
	}

	static bool ProbeLauncherPortOccupied(int nPort)
	{
		if (nPort < 1 || nPort > 65535)
			return false;

		sockaddr_in6 addr6{};
		addr6.sin6_family = AF_INET6;
		addr6.sin6_addr = in6addr_loopback;
		addr6.sin6_port = htons(static_cast<u_short>(nPort));
		if (ProbeAddrOccupied(reinterpret_cast<const sockaddr*>(&addr6), sizeof(addr6)))
			return true;

		sockaddr_in addr4{};
		addr4.sin_family = AF_INET;
		addr4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr4.sin_port = htons(static_cast<u_short>(nPort));
		return ProbeAddrOccupied(reinterpret_cast<const sockaddr*>(&addr4), sizeof(addr4));
	}

	static void ApplyIfNeeded(void)
	{
		if (!s_bActive || s_bApplied)
			return;
		if (!g_pCVar)
			return;

		static double s_flNextTry = 0.0;
		const double flNow = Plat_FloatTime();
		if (flNow < s_flNextTry)
			return;

		if (ProbeLauncherPortOccupied(s_nPort))
		{
			static volatile LONG s_nOccupiedWarns = 0;
			if (InterlockedIncrement(&s_nOccupiedWarns) <= 8)
			{
				Warning(eDLL_T::SERVER, "[RCON] launcher port %d occupied -- refusing bind, retrying\n", s_nPort);
			}
			s_flNextTry = flNow + 2.0;
			return;
		}

		if (ConVar* pLoop = g_pCVar->FindVar("sv_rcon_useloopbacksocket"))
			pLoop->SetValue(1);
		if (ConVar* pEnc = g_pCVar->FindVar("rcon_encryptframes"))
			pEnc->SetValue(0);
		if (ConVar* pPort = g_pCVar->FindVar("sv_rcon_port"))
			pPort->SetValue(s_nPort);
		if (ConVar* pMax = g_pCVar->FindVar("sv_rcon_maxconnections"))
			pMax->SetValue(kHostMaxConnections);
		if (ConVar* pElev = g_pCVar->FindVar("sv_bridge_admin_elevation"))
			pElev->SetValue(0);

		if (RCONServer() && RCONServer()->IsInitialized())
			RCONServer()->Shutdown();

		RCON_InitServerAndTrySyncKeys(s_szPassword);
		if (RCONServer() && RCONServer()->IsInitialized())
		{
			s_bApplied = true;
			Msg(eDLL_T::SERVER, "[RCON] launcher session listening on [::1]:%d\n", s_nPort);
			return;
		}

		s_flNextTry = flNow + 2.0;
	}

	static bool HostReadyNameOk(const char* psz)
	{
		if (!psz || !psz[0])
			return false;

		const size_t nPrefix = V_strlen(kHostReadyPrefix);
		if (V_strnicmp(psz, kHostReadyPrefix, static_cast<int>(nPrefix)) != 0)
			return false;

		const char* p = psz + nPrefix;
		if (!*p)
			return false;

		int n = 0;
		for (; *p; ++p, ++n)
		{
			const char c = *p;
			const bool ok = (c >= 'a' && c <= 'z')
				|| (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9')
				|| c == '_' || c == '-';
			if (!ok || n >= 64)
				return false;
		}
		return true;
	}
}

void RCON_LauncherSession_NotifyHostReady(const char* pszMap)
{
	const char* psz = (pszMap && pszMap[0]) ? pszMap : "?";
	Msg(eDLL_T::ENGINE, "[R5F-HOST] ready %s\n", psz);

	char szName[128] = {};
	const DWORD n = GetEnvironmentVariableA(kEnvHostReady, szName, sizeof(szName));
	if (n == 0 || n >= sizeof(szName))
		return;
	if (!HostReadyNameOk(szName))
	{
		Warning(eDLL_T::ENGINE, "[R5F-HOST] ignoring bad event name\n");
		return;
	}

	HANDLE h = CreateEventA(nullptr, TRUE, FALSE, szName);
	if (!h)
	{
		Warning(eDLL_T::ENGINE, "[R5F-HOST] CreateEvent failed (%lu)\n", GetLastError());
		return;
	}
	SetEvent(h);
	CloseHandle(h);
}

void RCON_LauncherSession_Think(void)
{
	LatchFromEnv();
	ApplyIfNeeded();
	if (s_bApplied)
		PinSessionConVars();
}

bool RCON_LauncherSession_Active(void)
{
	return s_bActive;
}

bool RCON_LauncherSession_IgnorePasswordClear(void)
{
	return s_bActive;
}

int RCON_LauncherSession_Port(void)
{
	return s_bActive ? s_nPort : 0;
}

bool RCON_LauncherSession_AllowExec(const char* pszCommand, const char* pszValue)
{
	if (!s_bActive)
		return true;

	if (!pszCommand || !pszCommand[0])
	{
		Warning(eDLL_T::SERVER, "[RCON] launcher session refused empty command\n");
		return false;
	}

	static double s_flExecWindowStart = 0.0;
	static int s_nExecWindowCount = 0;
	const double flNow = Plat_FloatTime();
	if (flNow - s_flExecWindowStart >= 1.0)
	{
		s_flExecWindowStart = flNow;
		s_nExecWindowCount = 0;
	}
	if (++s_nExecWindowCount > kHostExecPerSecond)
	{
		Warning(eDLL_T::SERVER, "[RCON] launcher session rate-limited '%s'\n", pszCommand);
		return false;
	}

	if (!AllowHostValue(pszCommand, pszValue))
	{
		Warning(eDLL_T::SERVER, "[RCON] launcher session refused malformed '%s'\n", pszCommand);
		return false;
	}

	return true;
}

