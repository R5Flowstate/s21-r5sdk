//=============================================================================//
//
// Purpose: Attach the hosted client to the launcher loopback RCON session.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/cl_rcon_launcher.h"
#include "engine/client/cl_rcon.h"
#include "engine/shared/shared_rcon.h"
#include "engine/cmd.h"
#include "engine/client/bridge_join_auth.h"
#include "tier0/threadtools.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "protoc/netcon.pb.h"

static ConVar cl_rcon_host_sendlogs("cl_rcon_host_sendlogs", "0", FCVAR_DEVELOPMENTONLY,
	"If 1, Play Local host console ingests dedi logs over RCON. Default 0 -- changelevel spew stalls HostStateFrame in ImGui AddLog and the game connection dies.");

namespace
{
	constexpr int kPasswordMax = 128;
	constexpr int kDefaultPort = 37017;
	constexpr int kMaxAuthFails = 3;
	constexpr int kQueueCap = 8;
	constexpr int kLineMax = 511;
	constexpr char kEnvFromLauncher[] = "FROM_R5F_LAUNCHER";
	constexpr char kEnvPassword[] = "R5F_LOCAL_RCON";
	constexpr char kEnvPort[] = "R5F_LOCAL_RCON_PORT";

	bool s_bLatched = false;
	bool s_bActive = false;
	bool s_bAuthSent = false;
	bool s_bLogsPolicyApplied = false;
	int s_nPort = kDefaultPort;
	int s_nAuthFails = 0;
	double s_flNextConnect = 0.0;
	char s_szPassword[kPasswordMax] = {};
	CThreadFastMutex s_QueueMutex;
	char s_szQueue[kQueueCap][kLineMax + 1] = {};
	int s_nQueueHead = 0;
	int s_nQueueTail = 0;
	int s_nQueueCount = 0;

	static void ClearExecQueue(void)
	{
		CAutoLockT<CThreadFastMutex> lock(s_QueueMutex);
		s_nQueueHead = 0;
		s_nQueueTail = 0;
		s_nQueueCount = 0;
		for (int i = 0; i < kQueueCap; ++i)
			s_szQueue[i][0] = '\0';
	}

	static void WipeEnv(const char* pszName)
	{
		SetEnvironmentVariableA(pszName, nullptr);
	}

	static bool HasIPrefix(const char* psz, const char* pszPrefix)
	{
		return V_strnicmp(psz, pszPrefix, V_strlen(pszPrefix)) == 0;
	}

	static bool IsThisProcessCommand(const char* psz)
	{
		if (!psz || !psz[0])
			return false;

		static const char* const kKeep[] =
		{
			"quit", "_restart", "disconnect", "connect", "reconnect",
			"rcon", "bridge_rcon", "bridge_connect",
			"toggleconsole", "clear", "bind", "unbind",
		};

		for (size_t i = 0; i < SDK_ARRAYSIZE(kKeep); ++i)
		{
			if (V_stricmp(psz, kKeep[i]) == 0)
				return true;
		}

		return false;
	}

	// Engine client cvars are FCVAR_RELEASE with no CLIENTDLL bit, so a
	// "forward unless CLIENTDLL-only" rule ships fps_max / mat_* to the dedi.
	static bool IsClientLocalName(const char* psz)
	{
		if (IsThisProcessCommand(psz))
			return true;

		static const char* const kPrefix[] =
		{
			"fps_", "cl_", "mat_", "r_", "hud_", "rui_",
			"model_", "gamepad_", "joy_", "imgui_", "miles_", "snd_",
			"showfps", "net_graph",
		};

		for (size_t i = 0; i < SDK_ARRAYSIZE(kPrefix); ++i)
		{
			if (HasIPrefix(psz, kPrefix[i]))
				return true;
		}

		return false;
	}

	static bool IsServerOwnedConVar(ConCommandBase* const pBase, const char* pszArg0)
	{
		if (RCON_IsServerAuthorityCmd(pszArg0))
			return true;
		if (HasIPrefix(pszArg0, "sv_"))
			return true;
		if (pBase->IsFlagSet(FCVAR_REPLICATED))
			return true;
		if (pBase->IsFlagSet(FCVAR_GAMEDLL) && !pBase->IsFlagSet(FCVAR_CLIENTDLL))
			return true;
		if (pBase->IsFlagSet(FCVAR_NOTIFY))
			return true;
		return false;
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
		if (nLen < 8 || nLen >= sizeof(s_szPassword))
		{
			Warning(eDLL_T::CLIENT, "[RCON] launcher session ignored: password length %zu\n", nLen);
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
		Msg(eDLL_T::CLIENT, "[RCON] launcher session latched (loopback port %d)\n", s_nPort);
	}

	static bool GameHostIsLoopback(void)
	{
		const char* const pszHost = Bridge_LastConnectHost();
		if (!pszHost || !pszHost[0])
			return false;
		return Bridge_IsTrueLoopbackHost(pszHost);
	}

	static void SendAuth(void)
	{
		if (s_bAuthSent || s_nAuthFails >= kMaxAuthFails)
			return;

		CRConClient* const pClient = RCONClient();
		ConnectedNetConsoleData_s* const pData = pClient ? pClient->GetData() : nullptr;
		if (!pData)
			return;

		vector<byte> vecMsg;
		const size_t nLen = V_strlen(s_szPassword);
		if (!pClient->Serialize(*pData, vecMsg, s_szPassword, nLen, "", 0,
			netcon::request_e::SERVERDATA_REQUEST_AUTH))
		{
			Warning(eDLL_T::CLIENT, "[RCON] launcher session failed to serialize AUTH\n");
			return;
		}

		if (!pClient->Send(pData->socket, vecMsg.data(), static_cast<u32>(vecMsg.size())))
		{
			Warning(eDLL_T::CLIENT, "[RCON] launcher session AUTH send failed\n");
			++s_nAuthFails;
			return;
		}

		s_bAuthSent = true;
		Msg(eDLL_T::CLIENT, "[RCON] launcher session AUTH sent\n");
	}

	static bool TryConnect(const char* pszAddr)
	{
		CRConClient* const pClient = RCONClient();
		if (!pClient)
			return false;
		// 0s: poll only. A 1s select here runs on HostStateFrame and freezes
		// the game while the dedi is down (script error / host shutdown).
		return pClient->ConnectTimeout(pszAddr, SOCKET_ERROR, 0.0f);
	}

	static bool LastHostIsRemote(void)
	{
		const char* const pszHost = Bridge_LastConnectHost();
		return pszHost && pszHost[0] && !Bridge_IsTrueLoopbackHost(pszHost);
	}

	static void DrainExecQueue(void)
	{
		CRConClient* const pClient = RCONClient();
		if (!pClient || !pClient->IsConnected())
			return;

		ConnectedNetConsoleData_s* const pData = pClient->GetData();
		if (!pData || !pData->authorized)
			return;

		for (;;)
		{
			char szLine[kLineMax + 1];
			szLine[0] = '\0';
			{
				CAutoLockT<CThreadFastMutex> lock(s_QueueMutex);
				if (s_nQueueCount <= 0)
					return;
				V_strncpy(szLine, s_szQueue[s_nQueueHead], sizeof(szLine));
				s_szQueue[s_nQueueHead][0] = '\0';
				s_nQueueHead = (s_nQueueHead + 1) % kQueueCap;
				--s_nQueueCount;
			}

			CCommand cmd;
			if (!cmd.Tokenize(szLine, cmd_source_t::kCommandSrcCode) || cmd.ArgC() < 1)
				continue;

			const char* const pszName = cmd.Arg(0);
			const size_t nNameLen = V_strlen(pszName);
			const size_t nValLen = V_strlen(szLine);

			vector<byte> vecMsg;
			if (!pClient->Serialize(*pData, vecMsg, pszName, nNameLen, szLine, nValLen,
				netcon::request_e::SERVERDATA_REQUEST_EXECCOMMAND))
			{
				Warning(eDLL_T::CLIENT, "[RCON] host console failed to serialize '%s'\n", pszName);
				continue;
			}
			if (!pClient->Send(pData->socket, vecMsg.data(), static_cast<u32>(vecMsg.size())))
				Warning(eDLL_T::CLIENT, "[RCON] host console send failed '%s'\n", pszName);
		}
	}

	static void MaintainConnection(void)
	{
		if (!s_bActive || !g_pCVar)
			return;

		if (LastHostIsRemote() || !GameHostIsLoopback())
		{
			ClearExecQueue();
			s_bLogsPolicyApplied = false;
			if (RCONClient() && RCONClient()->IsConnected())
				RCONClient()->Disconnect("left loopback game");
			return;
		}

		if (ConVar* pEnc = g_pCVar->FindVar("rcon_encryptframes"))
		{
			if (pEnc->GetBool())
				pEnc->SetValue(0);
		}

		CRConClient* const pClient = RCONClient();
		if (!pClient)
			return;

		if (!pClient->IsInitialized())
			RCON_InitClientAndTrySyncKeys();
		if (!pClient->IsInitialized())
			return;

		if (pClient->IsConnected())
		{
			ConnectedNetConsoleData_s* const pData = pClient->GetData();
			if (pData && pData->authorized)
			{
				s_nAuthFails = 0;
				if (!s_bLogsPolicyApplied && GameHostIsLoopback())
				{
					s_bLogsPolicyApplied = true;
					const bool bWantLogs = cl_rcon_host_sendlogs.GetBool();
					pClient->RequestConsoleLog(bWantLogs);
					if (bWantLogs)
						Msg(eDLL_T::CLIENT, "[RCON] host console subscribed to dedi logs\n");
					else
						Msg(eDLL_T::CLIENT, "[RCON] host console command-only (dedi logs stay on the dedi)\n");
				}
				return;
			}

			SendAuth();
			if (pData && !pData->authorized && s_bAuthSent)
			{
				// AUTH reply is consumed in CRConClient::ProcessMessage.
			}
			return;
		}

		s_bAuthSent = false;
		s_bLogsPolicyApplied = false;
		// Clean disconnect re-arms AUTH retries; stale fails would wedge legit reconnect at kMaxAuthFails.
		s_nAuthFails = 0;

		const double flNow = Plat_FloatTime();
		if (flNow < s_flNextConnect)
			return;
		s_flNextConnect = flNow + 1.5;

		char szAddr[64];
		V_snprintf(szAddr, sizeof(szAddr), "[::1]:%d", s_nPort);
		if (TryConnect(szAddr))
			return;

		V_snprintf(szAddr, sizeof(szAddr), "127.0.0.1:%d", s_nPort);
		if (!TryConnect(szAddr))
		{
			DevMsg(eDLL_T::CLIENT, "[RCON] launcher session connect retry [::1]/%s:%d\n",
				"127.0.0.1", s_nPort);
		}
	}
}

void RCON_LauncherClient_Think(void)
{
	LatchFromEnv();
	if (s_bActive)
	{
		MaintainConnection();
		if (RCON_LauncherClient_Ready())
			DrainExecQueue();
	}
}

bool RCON_LauncherClient_Active(void)
{
	return s_bActive;
}

bool RCON_LauncherClient_Ready(void)
{
	if (!s_bActive)
		return false;
	if (!GameHostIsLoopback())
		return false;

	CRConClient* const pClient = RCONClient();
	if (!pClient || !pClient->IsConnected())
		return false;

	ConnectedNetConsoleData_s* const pData = pClient->GetData();
	return pData && pData->authorized;
}

bool RCON_LauncherClient_ShouldForward(const char* pszArg0)
{
	if (!pszArg0 || !pszArg0[0] || !g_pCVar)
		return false;
	// Button commands (+forward/-jump/...) are local player input, never
	// console verbs. Cmd_ExecuteString dispatches every key bind through
	// here; forwarding them eats all movement on the hosted client.
	if (pszArg0[0] == '+' || pszArg0[0] == '-')
		return false;
	// Not in the client registry (or the client copy is the wrong VM).
	if (V_stricmp(pszArg0, "script") == 0 || V_stricmp(pszArg0, "reload") == 0)
		return true;
	if (IsClientLocalName(pszArg0))
		return false;

	ConCommandBase* const pBase = g_pCVar->FindCommandBase(pszArg0);
	if (!pBase)
	{
		// Dedi-only verbs are absent from the client registry (sv_addbot).
		if (HasIPrefix(pszArg0, "sv_"))
			return true;
		return RCON_IsServerAuthorityCmd(pszArg0);
	}

	if (pBase->IsFlagSet(FCVAR_USERINFO))
		return false;

	if (!pBase->IsCommand())
		return IsServerOwnedConVar(pBase, pszArg0);

	if (pBase->IsFlagSet(FCVAR_GAMEDLL) && !pBase->IsFlagSet(FCVAR_CLIENTDLL))
		return true;

	return RCON_IsServerAuthorityCmd(pszArg0);
}

bool RCON_LauncherClient_WantSendLogs(void)
{
	return cl_rcon_host_sendlogs.GetBool();
}

bool RCON_LauncherClient_QueueExec(const char* pszLine)
{
	if (!RCON_LauncherClient_Ready())
		return false;
	if (!pszLine || !pszLine[0])
		return false;
	if (V_strlen(pszLine) >= kLineMax)
		return false;
	if (strpbrk(pszLine, "\n\r"))
		return false;

	CCommand cmd;
	if (!cmd.Tokenize(pszLine, cmd_source_t::kCommandSrcCode) || cmd.ArgC() < 1)
		return false;

	const char* const pszArg0 = cmd.Arg(0);
	if (V_stricmp(pszArg0, "script") != 0 && strchr(pszLine, ';'))
		return false;
	if (!RCON_LauncherClient_ShouldForward(pszArg0))
		return false;

	CAutoLockT<CThreadFastMutex> lock(s_QueueMutex);
	if (s_nQueueCount >= kQueueCap)
		return false;

	V_strncpy(s_szQueue[s_nQueueTail], pszLine, sizeof(s_szQueue[0]));
	s_nQueueTail = (s_nQueueTail + 1) % kQueueCap;
	++s_nQueueCount;
	return true;
}

