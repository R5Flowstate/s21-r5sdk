#if defined(CLIENT_DLL)
#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "ebisusdk/EbisuSDK.h"
#include "engine/server/sv_main.h"
#include "common/global.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static ConVar cl_platformIdentityEnable("cl_platformIdentityEnable", "1", FCVAR_RELEASE,
	"Obtain the signed platform token that proves this account to the master server");

static ConVar cl_platformIdentityWait("cl_platformIdentityWait", "8", FCVAR_RELEASE,
	"Seconds a connect may wait for the platform token to arrive", true, 0.f, true, 30.f);

static ConVar cl_platformPurgeUpstream("cl_platformPurgeUpstream", "1", FCVAR_RELEASE,
	"Blank every upstream endpoint once platform identity is settled");

static ConVar cl_platformIdentityDiag("cl_platformIdentityDiag", "0", FCVAR_DEVELOPMENTONLY,
	"Announce once whether the signed platform token arrived, and how long it took");

// When the exchange was switched on, so "still arriving" can be told apart from
// "arriving slower than it should".
static double s_identityEnabledTime = 0.0;

//-----------------------------------------------------------------------------
// Purpose: blank upstream endpoints once they are no longer needed
//-----------------------------------------------------------------------------
static void EbisuSDK_PurgeUpstreamOnce()
{
	static bool purged = false;
	static double firstFrameTime = 0.0;

	if (purged || !cl_platformPurgeUpstream.GetBool())
		return;

	// Purge after identity settles (or 60s). Blanking earlier drops launch-arg processing.
	if (firstFrameTime == 0.0)
		firstFrameTime = Plat_FloatTime();

	const bool identitySettled = !EbisuSDK_IsPlatformIdentityExpected()
		|| g_NucleusTokenClient[0];

	// Something upstream failing must not leave every endpoint reachable for the
	// rest of the session.
	if (!identitySettled && Plat_FloatTime() - firstFrameTime < 60.0)
		return;

	purged = true;
	ConVar_PurgeHostNames();
}

//-----------------------------------------------------------------------------
// Purpose: request the signed platform token the master server verifies
//-----------------------------------------------------------------------------
static void EbisuSDK_EnablePlatformIdentity()
{
	// Enable crossPlay_Enabled and origin_use_jwt before the Origin state machine runs.
	static bool applied = false;

	if (applied || !g_pCVar || !cl_platformIdentityEnable.GetBool())
		return;

	ConVar* const pCrossPlay = g_pCVar->FindVar("crossPlay_Enabled");
	ConVar* const pUseJwt = g_pCVar->FindVar("origin_use_jwt");

	if (!pCrossPlay || !pUseJwt)
		return;

	pCrossPlay->SetValue(1);
	pUseJwt->SetValue(1);
	applied = true;
	s_identityEnabledTime = Plat_FloatTime();

	Msg(eDLL_T::ENGINE, "[EbisuSDK] platform identity enabled; signed token will be requested\n");
}

//-----------------------------------------------------------------------------
// Purpose: report once whether the signed platform token ever arrived
//-----------------------------------------------------------------------------
static void EbisuSDK_ReportPlatformIdentityOnce()
{
	static bool reported = false;

	if (reported || !cl_platformIdentityDiag.GetBool())
		return;

	if (!EbisuSDK_IsPlatformIdentityExpected())
	{
		reported = true;
		Msg(eDLL_T::ENGINE, "[EbisuSDK] no platform token expected on this client\n");
		return;
	}

	if (s_identityEnabledTime == 0.0)
		return;

	const double flElapsed = Plat_FloatTime() - s_identityEnabledTime;

	if (g_NucleusTokenClient[0])
	{
		reported = true;

		// Length only: the token is a live credential and never goes in a log.
		Msg(eDLL_T::ENGINE, "[EbisuSDK] platform token arrived after %.1fs (%zu bytes)\n",
			flElapsed, strlen(g_NucleusTokenClient));
		return;
	}

	if (flElapsed > 120.0)
	{
		reported = true;
		Warning(eDLL_T::ENGINE, "[EbisuSDK] platform token never arrived (%.0fs) -- "
			"this client cannot prove its account to the master server\n", flElapsed);
	}
}

//-----------------------------------------------------------------------------
// Purpose: whether a signed platform token should be available on this client
//-----------------------------------------------------------------------------
bool EbisuSDK_IsPlatformIdentityExpected()
{
	return cl_platformIdentityEnable.GetBool() && !IsOriginDisabled() && g_NucleusTokenClient != nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: the signed platform token, waiting for it if it has not landed yet
// Output: the token, or "" if none is expected or none arrived in time
//-----------------------------------------------------------------------------
const char* EbisuSDK_GetPlatformToken()
{
	if (!EbisuSDK_IsPlatformIdentityExpected())
		return "";

	if (g_NucleusTokenClient[0])
		return g_NucleusTokenClient;

	// Never waits. The platform mints this asynchronously on this same thread, so
	// spinning here freezes the game and stalls the arrival at once -- a caller
	// that finds it missing holds its work and retries from the frame loop.
	return g_NucleusTokenClient;
}

//-----------------------------------------------------------------------------
// Purpose: how long a held connect may wait for identity
//-----------------------------------------------------------------------------
float EbisuSDK_PlatformIdentityWaitSeconds()
{
	return cl_platformIdentityWait.GetFloat();
}

//-----------------------------------------------------------------------------
// Purpose: whether C2S_CONNECT may use this client's account id
//-----------------------------------------------------------------------------
bool EbisuSDK_IsConnectIdentityReady()
{
	if (IsOriginDisabled())
		return true;

	if (!g_NucleusID || *g_NucleusID == 0
		|| *g_NucleusID == static_cast<uint64_t>(FAKE_BASE_NUCLEUD_ID))
		return false;

	if (!g_PersonaName || !g_PersonaName[0])
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: identity progress, for a UI that must not offer an action that cannot
//          yet succeed
// Output: one of PLATFORM_IDENTITY_*
//-----------------------------------------------------------------------------
int EbisuSDK_GetPlatformIdentityState()
{
	if (!EbisuSDK_IsPlatformIdentityExpected())
		return PLATFORM_IDENTITY_NOT_REQUIRED;

	if (g_NucleusTokenClient[0])
		return PLATFORM_IDENTITY_READY;

	// Distinguished only so the player can be told it is taking longer than it
	// should. Still not ready, and still nothing to offer them -- the platform
	// retries on its own backoff, so this can return to READY by itself.
	if (s_identityEnabledTime != 0.0
		&& Plat_FloatTime() - s_identityEnabledTime > cl_platformIdentityWait.GetFloat())
	{
		return PLATFORM_IDENTITY_SLOW;
	}

	return PLATFORM_IDENTITY_PENDING;
}

//-----------------------------------------------------------------------------
// Purpose: initialize the EbisuSDK
//-----------------------------------------------------------------------------
void HEbisuSDK_Init()
{
	const bool isDedicated = IsDedicated();

	// Online: let native Origin fill Nucleus id + persona. Offline/dedi: fixed identity.
	const bool useFixedIdentity = isDedicated || IsOriginDisabled();
	//
	// Offline still sets profile+Nucleus so map works. Do not set g_EbisuSDKInit or MP presence flags.
	if (useFixedIdentity)
	{
		// On DX12 (or any build where VEbisuSDK did not resolve) these are null;
		// defer rather than dereferencing null.
		if (!g_EbisuSDKInit || !g_EbisuProfileInit || !g_NucleusID)
		{
			Warning(eDLL_T::ENGINE, "[EbisuSDK] HEbisuSDK_Init: platform globals unresolved "
				"(VEbisuSDK inactive on this build); skipping fixed-identity fill.\n");
			return;
		}

		// Do not set *g_EbisuSDKInit: native Origin init is `if (!g_EbisuSDKInit)` and registers ammo_pool_types.
		*g_EbisuProfileInit = true;
		*g_NucleusID = FAKE_BASE_NUCLEUD_ID;

		if (g_OriginAuthCode)
			Q_snprintf(g_OriginAuthCode, 256, "%s", "INVALID_OAUTH_CODE");
		if (g_NucleusToken)
			Q_snprintf(g_NucleusToken, 1024, "%s", "INVALID_NUCLEUS_TOKEN");

		// Offline players get a fixed "unnamed" persona (they cannot reach MP anyway).
		if (!isDedicated && g_PersonaName)
		{
			strncpy(g_PersonaName, "unnamed", MAX_PERSONA_NAME_LEN - 1);
			g_PersonaName[MAX_PERSONA_NAME_LEN - 1] = '\0';
		}

		if (!isDedicated && platform_user_id)
		{
			platform_user_id->SetValue(FAKE_BASE_NUCLEUD_ID);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: runs the EbisuSDK state machine
//-----------------------------------------------------------------------------
void HEbisuSDK_RunFrame()
{
	// Offline: skip native Origin. Purge upstream first.
	EbisuSDK_PurgeUpstreamOnce();

	if (IsOriginDisabled())
	{
		return;
	}

	EbisuSDK_EnablePlatformIdentity();
	EbisuSDK_RunFrame();
	EbisuSDK_ReportPlatformIdentityOnce();

	// Defined in engine/client/clientstate.cpp. A connect issued before identity
	// arrived is held rather than refused, and this is the client's reliable
	// per-frame hook to start it once it has.
	extern void Bridge_TickPendingConnect(void);
	Bridge_TickPendingConnect();

	// Mirror g_PersonaName onto the name userinfo ConVar; nothing else on the online path does.
	static bool s_nameSynced = false;
	if (!s_nameSynced && g_PersonaName && g_PersonaName[0] != '\0' && name_cvar)
	{
		name_cvar->SetValue(g_PersonaName);
		s_nameSynced = true;
		Msg(eDLL_T::ENGINE, "[EbisuSDK] name convar synced to real persona '%s'\n", g_PersonaName);
	}
}

//-----------------------------------------------------------------------------
// Purpose: returns the currently set language
//-----------------------------------------------------------------------------
const char* HEbisuSDK_GetLanguage()
{
	static bool initialized = false;
	static char languageName[32];

	if (initialized)
	{
		return languageName;
	}

	const char* value = nullptr;
	bool useDefault = true;

	if (CommandLine()->CheckParm("-language", &value))
	{
		if (V_LocaleNameExists(value))
		{
			strncpy(languageName, value, sizeof(languageName));
			useDefault = false;
		}
	}

	if (useDefault)
	{
		strncpy(languageName, g_LanguageNames[0], sizeof(languageName));
	}

	languageName[sizeof(languageName) - 1] = '\0';
	initialized = true;

	return languageName;
}

//-----------------------------------------------------------------------------
// Purpose: checks if the EbisuSDK is disabled
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool IsOriginDisabled()
{
	// 1:1 with legacy r5sdk (-noorigin). We also honour -offline as an alias so the
	// player can opt into the fixed "unnamed" identity with either flag.
	const static bool isDisabled = CommandLine()->CheckParm("-noorigin")
		|| CommandLine()->CheckParm("-offline");
	return isDisabled;
}

//-----------------------------------------------------------------------------
// Purpose: checks if the EbisuSDK is initialized
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool IsOriginInitialized()
{
	if (IsDedicated())
	{
		return true;
	}

	// Null-safe: on a build where VEbisuSDK did not resolve (e.g. DX12) treat the
	// platform as initialized (the native bootstrap in dllmain owns real state)
	// rather than dereferencing null pointers.
	if (!g_OriginErrorLevel || !g_EbisuSDKInit || !g_NucleusID || !g_EbisuProfileInit)
	{
		return true;
	}

	if ((!(*g_OriginErrorLevel)
		&& (*g_EbisuSDKInit)
		&& (*g_NucleusID)
		&& (*g_EbisuProfileInit)))
	//	&& (*g_OriginAuthCode)
	// && (g_NucleusToken[0])))
	{
		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: validates if client's persona name meets EA's criteria
// Input: *pszName -
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool IsValidPersonaName(const char* pszName, int nMinLen, int nMaxLen)
{
	size_t len = strlen(pszName);

	if (len < nMinLen ||
		len > nMaxLen)
	{
		return false;
	}

	// Check if the name contains any special characters.
	size_t pos = strspn(pszName, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
	return pszName[pos] == '\0';
}

void EbisuSDK_StubNativePollIfUnhooked(void)
{
	if (!IsOriginDisabled())
		return;

	const uintptr_t base = g_GameDll.GetModuleBase();
	if (!base)
		return;

	uint8_t* const pPoll = reinterpret_cast<uint8_t*>(
		base + (SDK_IsDx12Exe() ? 0x3F7530 : 0x3DBF10));

	// E9 = detour jmp (hook attached). C3 = already stubbed.
	if (pPoll[0] == 0xE9 || pPoll[0] == 0xC3)
	{
		Msg(eDLL_T::ENGINE, "[EbisuSDK] offline poll %s -- native stub skipped\n",
			pPoll[0] == 0xE9 ? "hooked" : "already stubbed");
		return;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(pPoll, 1, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE, "[EbisuSDK] VirtualProtect failed on offline poll @ %p\n",
			(void*)pPoll);
		return;
	}

	pPoll[0] = 0xC3;
	VirtualProtect(pPoll, 1, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), pPoll, 1);
	Warning(eDLL_T::ENGINE,
		"[EbisuSDK] VEbisuSDK hook missed; offline poll stubbed @ %p\n", (void*)pPoll);
}

void VEbisuSDK::Detour(const bool bAttach) const
{
	// Guard null: EbisuSDK_RunFrame is unresolved on DX12; GetLanguage is not
	// resolved on S21. Only attach what we resolved.
	if (EbisuSDK_RunFrame)
		DetourSetup(&EbisuSDK_RunFrame, &HEbisuSDK_RunFrame, bAttach);
	if (EbisuSDK_GetLanguage)
		DetourSetup(&EbisuSDK_GetLanguage, &HEbisuSDK_GetLanguage, bAttach);
}
#else // !CLIENT_DLL
#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "ebisusdk/EbisuSDK.h"
#include "engine/server/sv_main.h"

//-----------------------------------------------------------------------------
// Purpose: initialize the EbisuSDK
//-----------------------------------------------------------------------------
void HEbisuSDK_Init()
{
	const bool isDedicated = IsDedicated();
	const bool noOrigin = IsOriginDisabled();

	// Fill with default data if this is a dedicated server, or if the game was
	// launched with the platform system disabled. Engine code requires these
	// to be set for the game to function, else stuff like the "map" command
	// won't run as 'IsOriginInitialized' returns false (which got inlined in
	// every place this was called in the game's executable).
	if (isDedicated || noOrigin)
	{
		if (!g_EbisuSDKInit || !g_EbisuProfileInit || !g_NucleusID
			|| !g_OriginAuthCode || !g_NucleusToken)
		{
			Warning(eDLL_T::ENGINE, "[EbisuSDK] HEbisuSDK_Init: platform globals unresolved\n");
			return;
		}

		*g_EbisuSDKInit = true;
		*g_EbisuProfileInit = true;
		*g_NucleusID = FAKE_BASE_NUCLEUD_ID;

		Q_snprintf(g_OriginAuthCode, 256, "%s", "INVALID_OAUTH_CODE");
		Q_snprintf(g_NucleusToken, 1024, "%s", "INVALID_NUCLEUS_TOKEN");

		if (!isDedicated)
		{
			platform_user_id->SetValue(FAKE_BASE_NUCLEUD_ID);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: runs the EbisuSDK state machine
//-----------------------------------------------------------------------------
void HEbisuSDK_RunFrame()
{
	if (IsOriginDisabled())
	{
		return;
	}

	EbisuSDK_RunFrame();
}

//-----------------------------------------------------------------------------
// Purpose: returns the currently set language
//-----------------------------------------------------------------------------
const char* HEbisuSDK_GetLanguage()
{
	static bool initialized = false;
	static char languageName[32];

	if (initialized)
	{
		return languageName;
	}

	const char* value = nullptr;
	bool useDefault = true;

	if (CommandLine()->CheckParm("-language", &value))
	{
		if (V_LocaleNameExists(value))
		{
			strncpy(languageName, value, sizeof(languageName));
			useDefault = false;
		}
	}

	if (useDefault)
	{
		strncpy(languageName, g_LanguageNames[0], sizeof(languageName));
	}

	languageName[sizeof(languageName) - 1] = '\0';
	initialized = true;

	return languageName;
}

//-----------------------------------------------------------------------------
// Purpose: checks if the EbisuSDK is disabled
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool IsOriginDisabled()
{
	const static bool isDisabled = CommandLine()->CheckParm("-noorigin");
	return isDisabled;
}

//-----------------------------------------------------------------------------
// Purpose: checks if the EbisuSDK is initialized
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool IsOriginInitialized()
{
	if (IsDedicated())
	{
		return true;
	}
	else if ((!(*g_OriginErrorLevel)
		&& (*g_EbisuSDKInit)
		&& (*g_NucleusID)
		&& (*g_EbisuProfileInit)))
	//	&& (*g_OriginAuthCode)
	// && (g_NucleusToken[0])))
	{
		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: validates if client's persona name meets EA's criteria
// Input: *pszName -
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool IsValidPersonaName(const char* pszName, int nMinLen, int nMaxLen)
{
	size_t len = strlen(pszName);

	if (len < nMinLen ||
		len > nMaxLen)
	{
		return false;
	}

	// Check if the name contains any special characters.
	size_t pos = strspn(pszName, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
	return pszName[pos] == '\0';
}

void VEbisuSDK::Detour(const bool bAttach) const
{
	DetourSetup(&EbisuSDK_RunFrame, &HEbisuSDK_RunFrame, bAttach);
	DetourSetup(&EbisuSDK_GetLanguage, &HEbisuSDK_GetLanguage, bAttach);
}
#endif // CLIENT_DLL
