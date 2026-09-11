//=============================================================================//
//
// Purpose: Mint the online join token once per connect, shared by the Cbuf
// "connect" chokepoint and bridge_connect.
//
//=============================================================================//

#include "core/stdafx.h"
#include "engine/client/bridge_join_auth.h"
#include "tier0/dbg.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "tier1/strtools.h"
#include "ebisusdk/EbisuSDK.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"

// Defined in engine/client/clientstate.cpp. Top-level const on the pointer
// parameters is part of the decorated name in MSVC.
extern bool Bridge_InstallOnlineAuthToken(const char* const netAdrStr, char* const reasonBuf, const size_t reasonBufLen);
extern bool Bridge_IsJoinAuthDeferred(void);

static ConVar bridge_join_token_ttl("bridge_join_token_ttl", "5", FCVAR_RELEASE,
	"Seconds a minted join token is reused for the same host within one connect attempt's re-entry (not a reconnect)",
	true, 0.f, true, 25.f);

// Server decides whether a token is required. Default 0: mint failure warns and
// connect still goes out (mode-2 dedi reachable when master is down).
static ConVar cl_joinRequireToken("cl_joinRequireToken", "0", FCVAR_RELEASE,
	"1 = block a connect whose join-token mint failed. 0 = warn and connect anyway "
	"(default; the server decides).");

static char s_lastMintHost[256];
static double s_lastMintTime = 0.0;

//-----------------------------------------------------------------------------
// Host classifiers for join-token policy (mirrors dedi Authenticate gates).
// Strip optional [brackets] and :port so "192.168.1.5:37015" classifies correctly.
//-----------------------------------------------------------------------------
const char* Bridge_HostBase(const char* host, char* const out, const size_t outLen)
{
	if (!host || !host[0] || !out || outLen < 2)
		return "";

	while (*host == '[' || *host == ' ' || *host == '\t')
		++host;

	V_strncpy(out, host, outLen);

	// Drop trailing ']' and anything after the first unbracketed ':port' or '%zone'.
	char* p = out;
	if (*p)
	{
		// IPv6 bracket form already stripped '[' above; cut at ']' then.
		char* br = strchr(p, ']');
		if (br)
			*br = '\0';
		else
		{
			// IPv4 host:port -- only one colon; IPv6 without brackets has many.
			char* colon = strchr(p, ':');
			if (colon && !strchr(colon + 1, ':'))
				*colon = '\0';
		}
	}

	return out;
}

bool Bridge_IsTrueLoopbackHost(const char* host)
{
	if (!host || !host[0])
		return true; // bridge_connect with no arg defaults to localhost

	char base[128];
	Bridge_HostBase(host, base, sizeof(base));

	if (!base[0])
		return true;

	if (!V_stricmp(base, "localhost") || !V_stricmp(base, "localhost."))
		return true;

	if (!V_stricmp(base, "::1") || !V_stricmp(base, "0:0:0:0:0:0:0:1"))
		return true;

	// 127.0.0.0/8 and IPv4-mapped forms
	if (V_strncmp(base, "127.", 4) == 0)
		return true;
	if (V_stristr(base, "::ffff:127.") == base || V_stristr(base, "::FFFF:127.") == base)
		return true;

	return false;
}

bool Bridge_IsOfflineMultiplayerBlocked(void)
{
	if (CommandLine()->CheckParm("-offline") || CommandLine()->CheckParm("-noorigin"))
		return true;

	ConVar* const pAuthEnable = g_pCVar ? g_pCVar->FindVar("cl_onlineAuthEnable") : nullptr;
	return pAuthEnable && !pAuthEnable->GetBool();
}

void Bridge_ShowClientError(void)
{
	static double s_flNext = 0.0;
	const double flNow = Plat_FloatTime();
	if (flNow < s_flNext)
		return;
	s_flNext = flNow + 2.0;

	if (!g_pUIScript)
	{
		Warning(eDLL_T::ENGINE,
			"[JOIN-AUTH] offline multiplayer refused (UI VM not up)\n");
		return;
	}

	HSCRIPT hFn = g_pUIScript->FindFunction(
		"Bridge_ShowOfflineJoinError", nullptr, nullptr);
	if (!hFn)
	{
		Warning(eDLL_T::ENGINE,
			"[JOIN-AUTH] Bridge_ShowOfflineJoinError missing\n");
		return;
	}

	const ScriptStatus_t st = g_pUIScript->ExecuteFunction(
		hFn, nullptr, 0, nullptr, 0);
	if (st == SCRIPT_ERROR)
		Warning(eDLL_T::ENGINE, "[JOIN-AUTH] Bridge_ShowOfflineJoinError SCRIPT_ERROR\n");
}

// Rate-limited: silent private-range skips were the audit hole.
static void Bridge_WarnAuthSkipped(const char* const pszReason, const char* const pszHost)
{
	static double s_flNextWarnTime = 0.0;
	static int s_nSuppressed = 0;
	const double flNow = Plat_FloatTime();

	if (flNow < s_flNextWarnTime)
	{
		++s_nSuppressed;
		return;
	}

	if (s_nSuppressed > 0)
	{
		Warning(eDLL_T::ENGINE, "[AUTH] join token skipped: %s (%s) (+%d similar)\n",
			pszReason, pszHost ? pszHost : "?", s_nSuppressed);
		s_nSuppressed = 0;
	}
	else
	{
		Warning(eDLL_T::ENGINE, "[AUTH] join token skipped: %s (%s)\n",
			pszReason, pszHost ? pszHost : "?");
	}

	s_flNextWarnTime = flNow + 2.0;
}

static void Bridge_JoinAuthCacheMark(const char* host)
{
	V_strncpy(s_lastMintHost, host ? host : "", sizeof(s_lastMintHost));
	s_lastMintTime = Plat_FloatTime();
}

static void Bridge_JoinAuthCacheClear(void)
{
	s_lastMintHost[0] = '\0';
	s_lastMintTime = 0.0;
}

//-----------------------------------------------------------------------------
// Purpose: ensure a join token is installed for this host (or policy allows skip)
//-----------------------------------------------------------------------------
bool Bridge_EnsureJoinToken(const char* host, char* reasonBuf, size_t reasonBufLen)
{
	if (reasonBuf && reasonBufLen)
		reasonBuf[0] = '\0';

	if (!host)
		host = "";

	// Same host inside the TTL window: no re-mint (browser -> Cbuf -> bridge_connect).
	if (s_lastMintHost[0] && !V_stricmp(s_lastMintHost, host))
	{
		const double flAge = Plat_FloatTime() - s_lastMintTime;
		if (flAge >= 0.0 && flAge < (double)bridge_join_token_ttl.GetFloat())
			return true;
	}

	const bool bOfflineBlocked = Bridge_IsOfflineMultiplayerBlocked();
	const bool bLoopback = Bridge_IsTrueLoopbackHost(host);

	if (bOfflineBlocked && !bLoopback)
	{
		if (reasonBuf && reasonBufLen)
			V_strncpy(reasonBuf, "offline launch cannot join multiplayer", reasonBufLen);
		Warning(eDLL_T::ENGINE,
			"[JOIN-AUTH] refused offline multiplayer connect to '%s'\n", host);
		Bridge_ShowClientError();
		Bridge_JoinAuthCacheClear();
		return false;
	}

	// Loopback skips the join token, not Origin identity. A connect before
	// g_NucleusID is real publishes the 9990000 sentinel as a second client.
	if (!bOfflineBlocked && !EbisuSDK_IsConnectIdentityReady())
	{
		Bridge_ParkConnect(host);
		return false;
	}

	if (bOfflineBlocked)
	{
		Msg(eDLL_T::ENGINE,
			"[JOIN-AUTH] join-token mint skipped (offline loopback)\n");
		Bridge_JoinAuthCacheMark(host);
		return true;
	}

	if (bLoopback)
	{
		Bridge_WarnAuthSkipped("loopback", host);
		char authFailReason[512];
		authFailReason[0] = '\0';
		if (!Bridge_InstallOnlineAuthToken(host, authFailReason, sizeof(authFailReason)))
			Bridge_WarnAuthSkipped("loopback install soft-fail", authFailReason[0] ? authFailReason : host);
		Bridge_JoinAuthCacheMark(host);
		return true;
	}

	// Non-loopback require: force the install body past any private-range skip.
	ConVar* const pForceLocal = g_pCVar ? g_pCVar->FindVar("cl_onlineAuthForceLocal") : nullptr;
	int nPrevForce = -1;
	if (pForceLocal)
	{
		nPrevForce = pForceLocal->GetInt();
		pForceLocal->SetValue(1);
	}

	char localReason[512];
	localReason[0] = '\0';
	char* const pReason = (reasonBuf && reasonBufLen) ? reasonBuf : localReason;
	const size_t nReasonLen = (reasonBuf && reasonBufLen) ? reasonBufLen : sizeof(localReason);

	const bool bOk = Bridge_InstallOnlineAuthToken(host, pReason, nReasonLen);

	if (pForceLocal && nPrevForce >= 0)
		pForceLocal->SetValue(nPrevForce);

	if (!bOk)
	{
		// Still false: deferral is reported via Bridge_JoinAuthWasDeferred.
		Bridge_JoinAuthCacheClear();
		return false;
	}

	Bridge_JoinAuthCacheMark(host);
	return true;
}

bool Bridge_JoinAuthWasDeferred(void)
{
	return Bridge_IsJoinAuthDeferred();
}

bool Bridge_JoinAuthBlocksOnFailure(void)
{
	return cl_joinRequireToken.GetBool();
}

//-----------------------------------------------------------------------------
// Purpose: one-line pre-connect join-auth summary (never the token value)
//-----------------------------------------------------------------------------
void Bridge_LogJoinAuthState(const char* host)
{
	const bool bLoopback = Bridge_IsTrueLoopbackHost(host);
	const bool bOfflineLaunch = CommandLine()->CheckParm("-offline")
		|| CommandLine()->CheckParm("-noorigin");

	int nAuthEnable = 1;
	bool bHasToken = false;
	if (g_pCVar)
	{
		ConVar* const pAuthEnable = g_pCVar->FindVar("cl_onlineAuthEnable");
		if (pAuthEnable)
			nAuthEnable = pAuthEnable->GetInt();

		ConVar* const pToken = g_pCVar->FindVar("cl_onlineAuthToken");
		if (pToken)
		{
			const char* const psz = pToken->GetString();
			bHasToken = (psz && psz[0] != '\0');
		}
	}

	Msg(eDLL_T::ENGINE,
		"[JOIN-AUTH] host='%s' loopback=%d cl_onlineAuthEnable=%d token_present=%d offline_arg=%d\n",
		host ? host : "", bLoopback ? 1 : 0, nAuthEnable, bHasToken ? 1 : 0, bOfflineLaunch ? 1 : 0);
}
