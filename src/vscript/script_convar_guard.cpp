//=============================================================================//
//
// Purpose: script convar guard (see header). Natives are resolved by their
//          registration name string: the typed implementations are clones
//          that differ only in rip displacements, so no byte signature can
//          tell them apart.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "tier1/strtools.h"
#include "script_convar_guard.h"

// Readable by nothing in script; writable only where listed in s_secretWritable.
static const char* const s_secretNames[] =
{
	"password",
	"rcon_password",
	"rcon_key",
	"sv_rcon_password",
	"sv_password",
	"sv_netkey",
	"bridge_connect_password",
	"cl_onlineAuthToken",
	"cl_onlineAuthTokenSignature1",
	"cl_onlineAuthTokenSignature2",
	"fs_stats_host_key",
	"backtrace_token",
	"customMatch_playerToken",
	"match_roleToken",
};

// The server-password prompt hands the typed password to the connect path.
static const char* const s_secretWritable[] =
{
	"bridge_connect_password",
};

static const char* const s_writeLockedNames[] =
{
	"sv_cheats",
	"modsystem_enable",
	"sv_modPolicy",
	"sv_allowedMods",
	"sv_requiredMods",
	"sv_modsProfile",
	"sdk_rpak_sig_bypass",
	"sdk_pak_load_mod_paks",
	"sdk_pak_override_retail",
	"fs_stats_url",
	"cafe_prefs_trust_unverified",
	"sv_local_host_admin",
	"cafe_prefs_allow_anon",
	"cl_joinRequireToken",
	"bridge_playlist_override_allow",
	"sv_bridge_admin_passthrough",
	"sv_demo_dir",
	"demo_fuzz_dir",
	"sdk_pred_diff_file",
	"cl_nameFilterPath",
	"sv_nameFilterPath",
	"net_encryptionEnable",
	"net_useRandomKey",
	"rcon_encryptframes",
	"ssl_verify_peer",
	"sv_connect_challenge_bind",
	"bridge_join_token_ttl",
};

static const char* const s_writeLockedPrefixes[] =
{
	"rcon_",
	"sv_rcon_",
	"cl_rcon_",
	"sv_onlineAuth",
	"cl_onlineAuth",
	"sv_scriptremote_c2s_",
	"bridge_s2c_scriptremote",
	"spire_",
	"curl_",
};

// Console commands a script-issued ServerCommand line may not start with.
static const char* const s_serverCommandDenied[] =
{
	"exec",
	"execifexists",
	"alias",
	"bind",
	"unbind",
	"unbindall",
	"host_writeconfig",
	"writeid",
	"writeip",
	"con_logfile",
	"script",
	"script_client",
	"script_ui",
	"remap_build",
	"rcon",
	"bridge_rcon",
	"banid",
	"addip",
	"removeid",
	"removeip",
	"plugin_load",
	// Reloads re-parse whole files and keep the old copies alive; scripts may not loop them.
	"datatable_reload",
	"playlist_reload",
	"modsystem_reload",
	"modsystem_enable",
	"banlist_reload",
};

static bool NameInList(const char* const pszName, const char* const* const ppList, const size_t nCount)
{
	for (size_t i = 0; i < nCount; ++i)
	{
		if (V_stricmp(pszName, ppList[i]) == 0)
			return true;
	}
	return false;
}

static ConVar* FindScriptConVar(const char* const pszName)
{
	if (!pszName || !*pszName || !g_pCVar)
		return nullptr;
	return g_pCVar->FindVar(pszName);
}

bool ScriptConVarGuard_IsSecret(const char* const pszName)
{
	if (!pszName || !*pszName)
		return false;
	if (NameInList(pszName, s_secretNames, SDK_ARRAYSIZE(s_secretNames)))
		return true;

	const ConVar* const pVar = FindScriptConVar(pszName);
	return pVar && pVar->IsFlagSet(FCVAR_PROTECTED);
}

bool ScriptConVarGuard_IsWriteLocked(const char* const pszName)
{
	if (!pszName || !*pszName)
		return false;

	if (ScriptConVarGuard_IsSecret(pszName))
		return !NameInList(pszName, s_secretWritable, SDK_ARRAYSIZE(s_secretWritable));

	if (NameInList(pszName, s_writeLockedNames, SDK_ARRAYSIZE(s_writeLockedNames)))
		return true;

	for (size_t i = 0; i < SDK_ARRAYSIZE(s_writeLockedPrefixes); ++i)
	{
		if (V_strnicmp(pszName, s_writeLockedPrefixes[i], V_strlen(s_writeLockedPrefixes[i])) == 0)
			return true;
	}
	return false;
}

static void ScriptConVarGuard_Refuse(const char* const pszWhat, const char* const pszName)
{
	Warning(eDLL_T::ENGINE, "[SCRIPT-CVAR] refused %s '%s' from script\n",
		pszWhat, pszName ? pszName : "(null)");
}

//-----------------------------------------------------------------------------
// Typed native implementations (arg 1 is always the convar name)
//-----------------------------------------------------------------------------
static __int64 (*v_Script_SetConVarString)(const char*, const char*) = nullptr;
static __int64 (*v_Script_SetConVarInt)(const char*, int) = nullptr;
static __int64 (*v_Script_SetConVarFloat)(const char*, float) = nullptr;
static __int64 (*v_Script_SetConVarBool)(const char*, bool) = nullptr;
static __int64 (*v_Script_SetConVarColor)(const char*, const float*) = nullptr;
static __int64 (*v_Script_SetConVarToDefault)(const char*) = nullptr;
static const char* (*v_Script_GetConVarString)(const char*) = nullptr;
static int (*v_Script_GetConVarInt)(const char*) = nullptr;
static float (*v_Script_GetConVarFloat)(const char*) = nullptr;
static bool (*v_Script_GetConVarBool)(const char*) = nullptr;
static bool (*v_Script_DevTextBufferDumpToFile)(const char*) = nullptr;
#if !defined(CLIENT_DLL)
static __int64 (*v_Script_ServerCommand)(void*) = nullptr;
#endif // !CLIENT_DLL

static __int64 Script_SetConVarString(const char* pszName, const char* pszValue)
{
	if (ScriptConVarGuard_IsWriteLocked(pszName))
	{
		ScriptConVarGuard_Refuse("SetConVarString", pszName);
		return 0;
	}
	// Archived values are written back into cfg files verbatim inside quotes.
	if (pszValue && strpbrk(pszValue, "\"\r\n"))
	{
		ScriptConVarGuard_Refuse("SetConVarString (quote or line break in value)", pszName);
		return 0;
	}
	return v_Script_SetConVarString(pszName, pszValue);
}

static __int64 Script_SetConVarInt(const char* pszName, int nValue)
{
	if (ScriptConVarGuard_IsWriteLocked(pszName))
	{
		ScriptConVarGuard_Refuse("SetConVarInt", pszName);
		return 0;
	}
	return v_Script_SetConVarInt(pszName, nValue);
}

static __int64 Script_SetConVarFloat(const char* pszName, float flValue)
{
	if (ScriptConVarGuard_IsWriteLocked(pszName))
	{
		ScriptConVarGuard_Refuse("SetConVarFloat", pszName);
		return 0;
	}
	return v_Script_SetConVarFloat(pszName, flValue);
}

static __int64 Script_SetConVarBool(const char* pszName, bool bValue)
{
	if (ScriptConVarGuard_IsWriteLocked(pszName))
	{
		ScriptConVarGuard_Refuse("SetConVarBool", pszName);
		return 0;
	}
	return v_Script_SetConVarBool(pszName, bValue);
}

static __int64 Script_SetConVarColor(const char* pszName, const float* pColor)
{
	if (ScriptConVarGuard_IsWriteLocked(pszName))
	{
		ScriptConVarGuard_Refuse("SetConVarColor", pszName);
		return 0;
	}
	return v_Script_SetConVarColor(pszName, pColor);
}

static __int64 Script_SetConVarToDefault(const char* pszName)
{
	if (ScriptConVarGuard_IsWriteLocked(pszName))
	{
		ScriptConVarGuard_Refuse("SetConVarToDefault", pszName);
		return 0;
	}
	return v_Script_SetConVarToDefault(pszName);
}

static const char* Script_GetConVarString(const char* pszName)
{
	if (ScriptConVarGuard_IsSecret(pszName))
	{
		ScriptConVarGuard_Refuse("GetConVarString", pszName);
		return "";
	}
	return v_Script_GetConVarString(pszName);
}

// The numeric readers parse the same string value, so a digit-only secret leaks through them too.
static int Script_GetConVarInt(const char* pszName)
{
	if (ScriptConVarGuard_IsSecret(pszName))
	{
		ScriptConVarGuard_Refuse("GetConVarInt", pszName);
		return 0;
	}
	return v_Script_GetConVarInt(pszName);
}

static float Script_GetConVarFloat(const char* pszName)
{
	if (ScriptConVarGuard_IsSecret(pszName))
	{
		ScriptConVarGuard_Refuse("GetConVarFloat", pszName);
		return 0.0f;
	}
	return v_Script_GetConVarFloat(pszName);
}

static bool Script_GetConVarBool(const char* pszName)
{
	if (ScriptConVarGuard_IsSecret(pszName))
	{
		ScriptConVarGuard_Refuse("GetConVarBool", pszName);
		return false;
	}
	return v_Script_GetConVarBool(pszName);
}

// The stock native writes the dev text buffer to any path the script names,
// with no -dev gate on the dedi. Only a bare text file name is passed through.
static bool DumpToFile_IsSafeName(const char* const pszBase)
{
	const size_t nLen = strlen(pszBase);
	if (nLen == 0 || nLen > 64 || strstr(pszBase, ".."))
		return false;

	for (const char* p = pszBase; *p; ++p)
	{
		const char c = *p;
		if (!V_isalnum(c) && c != '_' && c != '-' && c != '.')
			return false;
	}

	const char* const pszExt = strrchr(pszBase, '.');
	return pszExt && (!V_stricmp(pszExt, ".txt") || !V_stricmp(pszExt, ".csv")
		|| !V_stricmp(pszExt, ".log") || !V_stricmp(pszExt, ".json"));
}

static bool Script_DevTextBufferDumpToFile(const char* pszPath)
{
	if (!pszPath)
		return false;

	const char* pszBase = pszPath;
	for (const char* p = pszPath; *p; ++p)
	{
		if (*p == '/' || *p == '\\' || *p == ':')
			pszBase = p + 1;
	}

	if (!DumpToFile_IsSafeName(pszBase))
	{
		ScriptConVarGuard_Refuse("DevTextBufferDumpToFile", pszPath);
		return false;
	}

	// The native opens the name in the GAME search path, so a bare name would still
	// truncate an existing root file; the prefix keeps dumps in their own namespace.
	char szDumpName[96];
	V_snprintf(szDumpName, sizeof(szDumpName), "scriptdump_%s", pszBase);
	return v_Script_DevTextBufferDumpToFile(szDumpName);
}

#if !defined(CLIENT_DLL)
static bool ServerCommand_TokenDenied(const char* const pszToken)
{
	if (NameInList(pszToken, s_serverCommandDenied, SDK_ARRAYSIZE(s_serverCommandDenied)))
		return true;
	return ScriptConVarGuard_IsWriteLocked(pszToken);
}

// Tokenized with the engine's own CCommand rules (break set and quoting), so the guard
// sees exactly the argv the command buffer will run. Every argument is checked, not just
// the verb, so "toggle sv_cheats" is caught; a command too long to tokenize is refused.
static bool ServerCommand_CommandDenied(const char* const pszCommand, const size_t nLen)
{
	if (nLen == 0)
		return false;

	char szCmd[512];
	if (nLen >= sizeof(szCmd) || nLen > static_cast<size_t>(CCommand::MaxCommandLength()))
		return true;

	memcpy(szCmd, pszCommand, nLen);
	szCmd[nLen] = '\0';

	CCommand args;
	if (!args.Tokenize(szCmd))
		return true;

	for (int i = 0; i < args.ArgC(); ++i)
	{
		if (ServerCommand_TokenDenied(args[i]))
			return true;
	}
	return false;
}

// Splits the line exactly like CCommandBuffer::GetNextCommandLength (quote and
// comment state reset per command, "//" comments, '\n' always breaks), so the
// guard judges the same commands the engine will run.
static bool ServerCommand_IsDenied(const char* const pszLine)
{
	const char* pText = pszLine;
	while (*pText)
	{
		const size_t nMaxLen = strlen(pText);
		size_t nCommandLength = 0;
		size_t nNext = 0;
		bool bIsQuoted = false;
		bool bIsCommented = false;

		for (; nNext < nMaxLen; ++nNext, nCommandLength += bIsCommented ? 0 : 1)
		{
			const char c = pText[nNext];
			if (!bIsCommented)
			{
				if (c == '"')
				{
					bIsQuoted = !bIsQuoted;
					continue;
				}

				if (!bIsQuoted && c == '/')
				{
					bIsCommented = (nNext < nMaxLen - 1) && pText[nNext + 1] == '/';
					if (bIsCommented)
					{
						++nNext;
						continue;
					}
				}

				if (!bIsQuoted && c == ';')
					break;
			}

			if (c == '\n')
				break;
		}

		if (ServerCommand_CommandDenied(pText, nCommandLength))
			return true;

		pText += nNext;
		if (*pText)
			++pText;
	}
	return false;
}

// dedi inlines the whole native into its Squirrel entry: the command string is the
// SQString at [[vm+0x58]+0x18], text at +0x40 (the same reads the entry itself makes).
static const char* ServerCommand_ArgFromVM(void* const pVM)
{
	const uint8_t* const pStackTop = pVM ? *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(pVM) + 0x58) : nullptr;
	const uint8_t* const pString = pStackTop ? *reinterpret_cast<uint8_t* const*>(pStackTop + 0x18) : nullptr;
	return pString ? reinterpret_cast<const char*>(pString + 0x40) : nullptr;
}

static __int64 Script_ServerCommand(void* pVM)
{
	const char* const pszLine = ServerCommand_ArgFromVM(pVM);
	if (pszLine && ServerCommand_IsDenied(pszLine))
	{
		ScriptConVarGuard_Refuse("ServerCommand", pszLine);
		return 0;
	}
	return v_Script_ServerCommand(pVM);
}
#endif // !CLIENT_DLL

//-----------------------------------------------------------------------------
// Resolution
//-----------------------------------------------------------------------------

// NUL-bounded exact match, so "SetConVarString" never lands inside
// "Client_Script_SetConVarString".
static QWORD FindExactString(const char* const pszString)
{
	const CModule::ModuleSections_t* const pRData = g_GameDll.FindSectionByName(".rdata");
	if (!pRData || !pRData->IsSectionValid())
		return 0;

	const size_t nLen = strlen(pszString) + 1;
	const uint8_t* const pBase = reinterpret_cast<const uint8_t*>(pRData->m_pSectionBase);

	for (size_t i = 1; i + nLen <= pRData->m_nSectionSize; ++i)
	{
		if (pBase[i - 1] == '\0' && memcmp(&pBase[i], pszString, nLen) == 0)
			return reinterpret_cast<QWORD>(&pBase[i]);
	}
	return 0;
}

// First REX.W lea reg,[rip+disp32] in .text whose target is nTarget.
static QWORD FindLeaReference(const QWORD nTarget)
{
	const CModule::ModuleSections_t* const pText = g_GameDll.FindSectionByName(".text");
	if (!pText || !pText->IsSectionValid() || !nTarget)
		return 0;

	const uint8_t* const pBase = reinterpret_cast<const uint8_t*>(pText->m_pSectionBase);
	for (size_t i = 0; i + 7 <= pText->m_nSectionSize; ++i)
	{
		if ((pBase[i] != 0x48 && pBase[i] != 0x4C) || pBase[i + 1] != 0x8D || (pBase[i + 2] & 0xC7) != 0x05)
			continue;

		const int32_t disp = *reinterpret_cast<const int32_t*>(&pBase[i + 3]);
		if (reinterpret_cast<QWORD>(&pBase[i + 7]) + disp == nTarget)
			return reinterpret_cast<QWORD>(&pBase[i]);
	}
	return 0;
}

static QWORD ResolveLeaRdx(const QWORD nAt)
{
	const uint8_t* const p = reinterpret_cast<const uint8_t*>(nAt);
	if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15)
		return 0;
	return nAt + 7 + *reinterpret_cast<const int32_t*>(&p[3]);
}

template <typename T>
static void ResolveScriptNative(const char* const pszName, T& pOut)
{
	pOut = nullptr;
	const QWORD nRef = FindLeaReference(FindExactString(pszName));
	if (!nRef)
		return;

#if defined(CLIENT_DLL)
	// S21: lea rdx,"<Name>" ... 0x40 bytes of binding setup ... lea rdx,<impl>.
	const QWORD nImpl = ResolveLeaRdx(nRef + 0x40);
#else
	// dedi: lea rdx,<impl> immediately precedes lea rax,"Script_<Name>".
	const QWORD nImpl = ResolveLeaRdx(nRef - 7);
#endif // CLIENT_DLL

	pOut = reinterpret_cast<T>(nImpl);
}

#if !defined(CLIENT_DLL)
// ServerCommand has no separate implementation on dedi: the lea rdx before its name is a
// console callback nothing calls. The Squirrel entry is the first lea rax after the name,
// accepted only if it makes the inlined call (lea rdx,[rdi+40h]; call [rax+0C0h]).
static void ResolveServerCommandEntry(void*& pOut)
{
	pOut = nullptr;
	const QWORD nRef = FindLeaReference(FindExactString("Script_ServerCommand"));
	if (!nRef)
		return;

	for (QWORD at = nRef + 7; at < nRef + 0x60; ++at)
	{
		const uint8_t* const p = reinterpret_cast<const uint8_t*>(at);
		if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x05)
			continue;

		const QWORD nEntry = at + 7 + *reinterpret_cast<const int32_t*>(&p[3]);
		static const uint8_t kInlinedCall[] = { 0x48, 0x8D, 0x57, 0x40, 0xFF, 0x90, 0xC0, 0x00, 0x00, 0x00 };
		const uint8_t* const pEntry = reinterpret_cast<const uint8_t*>(nEntry);
		for (size_t i = 0; i + sizeof(kInlinedCall) <= 0x40; ++i)
		{
			if (!memcmp(pEntry + i, kInlinedCall, sizeof(kInlinedCall)))
			{
				pOut = reinterpret_cast<void*>(nEntry);
				return;
			}
		}
		return;
	}
}
#endif // !CLIENT_DLL

#if defined(CLIENT_DLL)
#define SCRIPT_NATIVE_NAME(x) x
#else
#define SCRIPT_NATIVE_NAME(x) "Script_" x
#endif // CLIENT_DLL

void VScriptConVarGuard::GetAdr(void) const
{
	LogFunAdr("Script_SetConVarString", v_Script_SetConVarString);
	LogFunAdr("Script_SetConVarInt", v_Script_SetConVarInt);
	LogFunAdr("Script_SetConVarFloat", v_Script_SetConVarFloat);
	LogFunAdr("Script_SetConVarBool", v_Script_SetConVarBool);
	LogFunAdr("Script_SetConVarColor", v_Script_SetConVarColor);
	LogFunAdr("Script_SetConVarToDefault", v_Script_SetConVarToDefault);
	LogFunAdr("Script_GetConVarString", v_Script_GetConVarString);
	LogFunAdr("Script_GetConVarInt", v_Script_GetConVarInt);
	LogFunAdr("Script_GetConVarFloat", v_Script_GetConVarFloat);
	LogFunAdr("Script_GetConVarBool", v_Script_GetConVarBool);
	LogFunAdr("Script_DevTextBufferDumpToFile", v_Script_DevTextBufferDumpToFile);
#if !defined(CLIENT_DLL)
	LogFunAdr("Script_ServerCommand", v_Script_ServerCommand);
#endif // !CLIENT_DLL
}

void VScriptConVarGuard::GetFun(void) const
{
	ResolveScriptNative(SCRIPT_NATIVE_NAME("SetConVarString"), v_Script_SetConVarString);
	ResolveScriptNative(SCRIPT_NATIVE_NAME("SetConVarInt"), v_Script_SetConVarInt);
	ResolveScriptNative(SCRIPT_NATIVE_NAME("SetConVarFloat"), v_Script_SetConVarFloat);
	ResolveScriptNative(SCRIPT_NATIVE_NAME("SetConVarBool"), v_Script_SetConVarBool);
#if defined(CLIENT_DLL)
	ResolveScriptNative("SetConVarColor", v_Script_SetConVarColor);
#endif // CLIENT_DLL
	ResolveScriptNative(SCRIPT_NATIVE_NAME("SetConVarToDefault"), v_Script_SetConVarToDefault);
	ResolveScriptNative(SCRIPT_NATIVE_NAME("GetConVarString"), v_Script_GetConVarString);
	ResolveScriptNative(SCRIPT_NATIVE_NAME("GetConVarInt"), v_Script_GetConVarInt);
	ResolveScriptNative(SCRIPT_NATIVE_NAME("GetConVarFloat"), v_Script_GetConVarFloat);
	ResolveScriptNative(SCRIPT_NATIVE_NAME("GetConVarBool"), v_Script_GetConVarBool);
#if defined(CLIENT_DLL)
	ResolveScriptNative("DevTextBufferDumpToFile", v_Script_DevTextBufferDumpToFile);
#else
	ResolveScriptNative("ScriptDevTextBufferDumpToFile", v_Script_DevTextBufferDumpToFile);
	void* pServerCommand = nullptr;
	ResolveServerCommandEntry(pServerCommand);
	v_Script_ServerCommand = reinterpret_cast<decltype(v_Script_ServerCommand)>(pServerCommand);
#endif // CLIENT_DLL
}

#define GUARD_DETOUR(orig, hook, name) \
	if (orig) { DetourSetup(&orig, &hook, bAttach); ++nGuarded; } \
	else if (bAttach) { Warning(eDLL_T::ENGINE, "[SCRIPT-CVAR] %s unresolved -- not guarded\n", name); }

void VScriptConVarGuard::Detour(const bool bAttach) const
{
	int nGuarded = 0;
	GUARD_DETOUR(v_Script_SetConVarString, Script_SetConVarString, "SetConVarString");
	GUARD_DETOUR(v_Script_SetConVarInt, Script_SetConVarInt, "SetConVarInt");
	GUARD_DETOUR(v_Script_SetConVarFloat, Script_SetConVarFloat, "SetConVarFloat");
	GUARD_DETOUR(v_Script_SetConVarBool, Script_SetConVarBool, "SetConVarBool");
#if defined(CLIENT_DLL)
	GUARD_DETOUR(v_Script_SetConVarColor, Script_SetConVarColor, "SetConVarColor");
#endif // CLIENT_DLL
	GUARD_DETOUR(v_Script_SetConVarToDefault, Script_SetConVarToDefault, "SetConVarToDefault");
	GUARD_DETOUR(v_Script_GetConVarString, Script_GetConVarString, "GetConVarString");
	GUARD_DETOUR(v_Script_GetConVarInt, Script_GetConVarInt, "GetConVarInt");
	GUARD_DETOUR(v_Script_GetConVarFloat, Script_GetConVarFloat, "GetConVarFloat");
	GUARD_DETOUR(v_Script_GetConVarBool, Script_GetConVarBool, "GetConVarBool");
	GUARD_DETOUR(v_Script_DevTextBufferDumpToFile, Script_DevTextBufferDumpToFile, "DevTextBufferDumpToFile");
#if !defined(CLIENT_DLL)
	GUARD_DETOUR(v_Script_ServerCommand, Script_ServerCommand, "ServerCommand");
#endif // !CLIENT_DLL

	if (bAttach)
		Msg(eDLL_T::ENGINE, "[SCRIPT-CVAR] %d script convar natives guarded\n", nGuarded);
}

#undef GUARD_DETOUR
