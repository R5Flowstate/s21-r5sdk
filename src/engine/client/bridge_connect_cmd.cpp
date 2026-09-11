//=============================================================================//
//
// Purpose: bridge_connect / bridge_rcon ConCommands for the S21 client inject.
//
//=============================================================================//

#include "core/stdafx.h"
#include "engine/client/bridge_join_auth.h"
#include "engine/client/bridge_connect_password.h"
#include "engine/shared/connect_password_tag.h"
#include "tier0/dbg.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "tier1/strtools.h"
#include "engine/cmd.h"
#include "engine/client/cl_rcon_launcher.h"

static char s_deferredConnect[320];
static bool s_insideDispatch = false;

static unsigned char s_connectPwKey[32];
static bool s_connectPwHaveKey = false;

static bool Bridge_PasswordCharsOk(const char* pszPassword)
{
	size_t n = 0;
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszPassword); *p; ++p)
	{
		if (++n > 128)
			return false;
		if (*p < 0x20 || *p == 0x7F)
			return false;
		if (*p == '"' || *p == ';' || *p == '\\')
			return false;
	}
	return n > 0;
}

static bool Bridge_ConnectHostCharsOk(const char* host)
{
	if (!host || !host[0] || strpbrk(host, "\";\n\r"))
		return false;

	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(host); *p; ++p)
	{
		const unsigned char c = *p;
		const bool ok = (c >= '0' && c <= '9')
			|| (c >= 'A' && c <= 'Z')
			|| (c >= 'a' && c <= 'z')
			|| c == '.' || c == ':' || c == '[' || c == ']'
			|| c == '-' || c == '_';
		if (!ok)
			return false;
	}
	return true;
}

void Bridge_SetConnectPassword(const char* pszPassword)
{
	memset(s_connectPwKey, 0, sizeof(s_connectPwKey));
	s_connectPwHaveKey = false;
	if (!pszPassword || !pszPassword[0])
		return;

	if (!Bridge_PasswordCharsOk(pszPassword))
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-PW] password rejected\n");
		return;
	}

	if (!ConnectPw_DeriveKey(pszPassword, s_connectPwKey))
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-PW] password rejected\n");
		memset(s_connectPwKey, 0, sizeof(s_connectPwKey));
		return;
	}

	s_connectPwHaveKey = true;
	Msg(eDLL_T::ENGINE, "[BRIDGE-PW] connect password set\n");
}

static void Bridge_ConnectPasswordChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData)
{
	(void)pOldString;
	(void)flOldValue;
	(void)pUserData;

	if (!g_pCVar)
		return;

	ConVar* const pPw = g_pCVar->FindVar(pConVar->GetName());
	if (!pPw)
		return;

	const char* const v = pPw->GetString();
	if (!v || !v[0])
		return;

	Bridge_SetConnectPassword(v);
	pPw->SetValue("");
}

const char* Bridge_GetConnectPasswordTag(void)
{
	return s_connectPwHaveKey ? "1" : "";
}

void Bridge_WirePasswordTag(const uint32_t nChallenge, char* const out, const size_t outLen)
{
	out[0] = '\0';
	if (!s_connectPwHaveKey)
		return;

	uint64_t v = 0;
	if (!ConnectPw_WireU64(s_connectPwKey, nChallenge, &v))
		return;

	V_snprintf(out, outLen, "pw:%016llx", v);
}

// Script sets this via SetConVarString (S21 type compiler cannot register a
// string-arg native). Change callback hashes immediately and clears the value.
// Not FCVAR_PROTECTED: SetConVarString refuses that flag.
static ConVar bridge_connect_password("bridge_connect_password", "",
	FCVAR_RELEASE | FCVAR_UNLOGGED | FCVAR_DONTRECORD,
	"Join password for the next connect.",
	false, 0.f, false, 0.f, &Bridge_ConnectPasswordChanged_f, nullptr);

void Bridge_PumpDeferredConnect(void)
{
	if (s_insideDispatch || !s_deferredConnect[0])
		return;

	if (!Cbuf_AddText)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-CONNECT] Cbuf_AddText unresolved -- deferred connect dropped\n");
		s_deferredConnect[0] = '\0';
		return;
	}

	char line[320];
	V_strncpy(line, s_deferredConnect, sizeof(line));
	s_deferredConnect[0] = '\0';

	char shown[320];
	V_strncpy(shown, line, sizeof(shown));
	size_t n = strlen(shown);
	while (n > 0 && (shown[n - 1] == '\n' || shown[n - 1] == '\r'))
		shown[--n] = '\0';

	Msg(eDLL_T::ENGINE, "[BRIDGE-CONNECT] dispatching '%s'\n", shown);
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), line, cmd_source_t::kCommandSrcCode);
}

//-----------------------------------------------------------------------------
// CCommand::Tokenize breaks on ':'. Quote the address and stamp hostport from :port.
//-----------------------------------------------------------------------------
static int Bridge_ExtractHostPort(const char* host, char* hostOnly, size_t hostOnlyLen)
{
	if (!host || !host[0] || !hostOnly || hostOnlyLen < 2)
		return -1;

	// Bracketed IPv6: [fe80::1]:37122
	if (host[0] == '[')
	{
		const char* br = strchr(host, ']');
		if (!br)
		{
			V_strncpy(hostOnly, host, hostOnlyLen);
			return -1;
		}
		const size_t n = static_cast<size_t>(br - host - 1);
		if (n + 1 > hostOnlyLen)
			return -1;
		memcpy(hostOnly, host + 1, n);
		hostOnly[n] = '\0';
		if (br[1] == ':')
		{
			const int port = atoi(br + 2);
			if (port > 0 && port <= 65535)
				return port;
		}
		return -1;
	}

	// IPv4 host:port -- single colon. Bare IPv6 has multiple colons; leave intact.
	const char* colon = strchr(host, ':');
	if (colon && !strchr(colon + 1, ':'))
	{
		const size_t n = static_cast<size_t>(colon - host);
		if (n + 1 > hostOnlyLen)
			return -1;
		memcpy(hostOnly, host, n);
		hostOnly[n] = '\0';
		const int port = atoi(colon + 1);
		if (port > 0 && port <= 65535)
			return port;
		return -1;
	}

	V_strncpy(hostOnly, host, hostOnlyLen);
	return -1;
}

//-----------------------------------------------------------------------------
// Stamp hostport + join token, then hold connect for the next Cbuf_Execute.
//-----------------------------------------------------------------------------
static void Bridge_DispatchConnect(const char* host)
{
	if (!host || !host[0])
		host = "localhost";

	if (!g_pCVar)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-CONNECT] g_pCVar null\n");
		return;
	}

	ConCommandBase* pBase = g_pCVar->FindCommandBase("connect");
	if (!pBase || !pBase->IsCommand())
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-CONNECT] ConCommand 'connect' not found\n");
		return;
	}

	char hostOnly[256];
	const int port = Bridge_ExtractHostPort(host, hostOnly, sizeof(hostOnly));
	if (port > 0)
	{
		// Connect_Worker / NET path often read hostport when the address
		// token has no port (or after a bad tokenize). Keep them in sync.
		ConVar* const pHostPort = g_pCVar->FindVar("hostport");
		if (pHostPort)
		{
			pHostPort->SetValue(port);
			Msg(eDLL_T::ENGINE, "[BRIDGE-CONNECT] hostport -> %d (from %s)\n", port, host);
		}
	}

	char authFailReason[512];
	authFailReason[0] = '\0';
	if (!Bridge_EnsureJoinToken(host, authFailReason, sizeof(authFailReason)))
	{
		if (Bridge_JoinAuthWasDeferred())
		{
			Msg(eDLL_T::ENGINE,
				"[JOIN-AUTH] connect to '%s' waiting on platform sign-in; will start on its own\n",
				host);
			return;
		}
		Warning(eDLL_T::ENGINE,
			"[BRIDGE-CONNECT] online authentication failed: %s\n",
			authFailReason);
		if (Bridge_IsTrueLoopbackHost(host) && !Bridge_JoinAuthBlocksOnFailure())
		{
			Warning(eDLL_T::ENGINE,
				"[JOIN-AUTH] loopback connect proceeding without token\n");
		}
		else
		{
			return;
		}
	}

	// `connect` is not CLIENTCMD_CAN_EXECUTE; a queue from this ClientCommand
	// window is dropped. Hold until the next Cbuf_Execute entry.
	if (strpbrk(host, "\";\n\r"))
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-CONNECT] refused host with Cbuf metacharacters\n");
		return;
	}

	s_insideDispatch = true;
	if (Bridge_IsTrueLoopbackHost(host))
		V_snprintf(s_deferredConnect, sizeof(s_deferredConnect), "connect localhost\n");
	else
		V_snprintf(s_deferredConnect, sizeof(s_deferredConnect), "connect \"%s\"\n", host);
	s_insideDispatch = false;
	Msg(eDLL_T::ENGINE, "[BRIDGE-CONNECT] deferred host='%s' port=%d loopback=%d\n",
		host, port, Bridge_IsTrueLoopbackHost(host) ? 1 : 0);
}

bool Bridge_ConnectToHost(const char* host, int port)
{
	if (!Bridge_ConnectHostCharsOk(host))
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-CONNECT] refused host with illegal characters\n");
		return false;
	}

	char buf[288];
	const char* dispatch = host;
	if (port > 0 && port <= 65535)
	{
		if (host[0] != '[' && strchr(host, ':'))
			V_snprintf(buf, sizeof(buf), "[%s]:%d", host, port);
		else
			V_snprintf(buf, sizeof(buf), "%s:%d", host, port);
		dispatch = buf;
	}

	Bridge_DispatchConnect(dispatch);
	return true;
}

//-----------------------------------------------------------------------------
// Re-join the last accepted connect host. g_pClientState is null on this product.
//-----------------------------------------------------------------------------
void Bridge_Reconnect(void)
{
	const char* const pszLast = Bridge_LastConnectHost();
	if (!pszLast || !pszLast[0])
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-RECONNECT] no server to reconnect to\n");
		return;
	}

	if (!Cbuf_AddText)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-RECONNECT] Cbuf_AddText unresolved\n");
		return;
	}

	char host[256];
	V_strncpy(host, pszLast, sizeof(host));

	Msg(eDLL_T::ENGINE, "[BRIDGE-RECONNECT] rejoining '%s'\n", host);

	// Clear the dispatch stamp first: the rejoin targets the host we are
	// already on, and the duplicate-connect dedupe would drop it after the
	// disconnect below had run, leaving the client at the menu.
	Bridge_NoteConnectDispatched("");

	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "disconnect\n", cmd_source_t::kCommandSrcCode);
	Bridge_DispatchConnect(host);
}

// UI / console: bridge_connect host[:port]
// Apex Tokenize splits "host:port" into ["host", ":", "port"] before we run --
// reassemble those fragments. Also accept "bridge_connect host port".
static void BridgeConnect_f(const CCommand& args)
{
	char hostBuf[288];
	const char* host = "localhost";
	const char* password = "";

	// bridge_connect 1.2.3.4 : 37122 [password]  (argc 4 after colon split)
	if (args.ArgC() >= 4 && args.Arg(1)[0]
		&& args.Arg(2)[0] == ':' && args.Arg(2)[1] == '\0'
		&& atoi(args.Arg(3)) > 0)
	{
		V_snprintf(hostBuf, sizeof(hostBuf), "%s:%s", args.Arg(1), args.Arg(3));
		host = hostBuf;
		if (args.ArgC() >= 5)
			password = args.Arg(4);
	}
	// bridge_connect 1.2.3.4 37122 [password]
	else if (args.ArgC() >= 3 && args.Arg(1)[0] && args.Arg(2)[0]
		&& !strchr(args.Arg(1), ':') && atoi(args.Arg(2)) > 0)
	{
		V_snprintf(hostBuf, sizeof(hostBuf), "%s:%s", args.Arg(1), args.Arg(2));
		host = hostBuf;
		if (args.ArgC() >= 4)
			password = args.Arg(3);
	}
	else if (args.ArgC() > 1 && args.Arg(1)[0])
	{
		host = args.Arg(1);
		if (args.ArgC() >= 3)
			password = args.Arg(2);
	}

	// Only apply an explicit trailing password. No-arg must not wipe a tag
	// ConnectToServer just stashed -- that path Cbufs `bridge_connect host`.
	if (password[0])
		Bridge_SetConnectPassword(password);

	Msg(eDLL_T::ENGINE, "[BRIDGE-CONNECT] argv argc=%d -> host='%s' password=%d\n",
		args.ArgC(), host, password[0] ? 1 : 0);
	Bridge_DispatchConnect(host);
}

static ConCommand bridge_connect(
	"bridge_connect",
	BridgeConnect_f,
	"Connect to a game server (UI-safe). Usage: bridge_connect host[:port] [password]",
	FCVAR_CLIENTCMD_CAN_EXECUTE | FCVAR_RELEASE);

// UI: ClientCommand("bridge_rcon changelevel mp_lobby")
// Kept registered (FCVAR_CLIENTCMD_CAN_EXECUTE) so scripts get a clear refusal
// instead of a missing-command error.
static void BridgeRcon_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-RCON] usage: bridge_rcon <server command...>\n");
		return;
	}

	char assembled[480];
	assembled[0] = '\0';
	for (int i = 1; i < args.ArgC(); ++i)
	{
		if (i > 1)
			V_strncat(assembled, " ", sizeof(assembled));
		V_strncat(assembled, args.Arg(i), sizeof(assembled));
	}

	if (strpbrk(assembled, "\n\r"))
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-RCON] refused command with line breaks\n");
		return;
	}

	CCommand inner;
	if (!inner.Tokenize(assembled, cmd_source_t::kCommandSrcCode) || inner.ArgC() < 1)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-RCON] refused empty command\n");
		return;
	}

	// UI ClientCommand path -- do not accept squirrel. Type script in the console.
	if (V_stricmp(inner.Arg(0), "script") == 0)
	{
		Warning(eDLL_T::ENGINE, "[BRIDGE-RCON] 'script' refused on the UI path\n");
		return;
	}

	if (RCON_LauncherClient_QueueExec(assembled))
	{
		Msg(eDLL_T::ENGINE, "[BRIDGE-RCON] '%s' -> dedi via host RCON\n", assembled);
		return;
	}

	Warning(eDLL_T::ENGINE,
		"[BRIDGE-RCON] '%s' refused: host console is only on Play Local -- use netconsole.exe\n",
		assembled);
}

static ConCommand bridge_rcon(
	"bridge_rcon",
	BridgeRcon_f,
	"Forward a server command on a hosted Play Local session",
	FCVAR_CLIENTCMD_CAN_EXECUTE | FCVAR_RELEASE);
