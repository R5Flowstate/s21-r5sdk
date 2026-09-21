//=============================================================================//
//
// Purpose: S21 RPAK observability and load gating. Slot = pakHnd & 0x1FF, name at +0x18.
//
//=============================================================================//

#include "core/stdafx.h"
#include "rpak_observe.h"
#include "pakparse.h"
#include "common/global.h"
#include "tier0/dbg.h"
#include "tier0/memvalidate.h"
#include "tier1/convar.h"
#include "pluginsystem/modsystem.h"
#include <intrin.h>
#include <windows.h>
#pragma intrinsic(_ReturnAddress)

// File-only bridge_trace sink; flushed before a possible TerminateProcess.
extern void BridgeTrace_Log(const char* fmt, ...);
extern void BridgeTrace_Flush();

static ConVar sdk_pak_load_sdk_paks("sdk_pak_load_sdk_paks", "1", FCVAR_RELEASE,
	"Load SDK UI rpak during boot (ui_sdk). common_flowstate loads later (after common).");

static ConVar sdk_pak_load_flowstate("sdk_pak_load_flowstate", "1", FCVAR_RELEASE,
	"Enqueue common_flowstate.rpak after common_mp. 0 = skip (launch-arg A/B).");

static ConVar sdk_pak_load_s30("sdk_pak_load_s30", "1", FCVAR_RELEASE,
	"Enqueue sdk_s30.rpak (S30 effects, Axle, akimbo, Ash, mantle air) after common_mp. 0 = skip (launch-arg A/B).");

static ConVar sdk_pak_load_mod_paks("sdk_pak_load_mod_paks", "1", FCVAR_RELEASE,
	"Enqueue each enabled mod's paks/Win64/preload.rson list after common_mp.");

static ConVar sdk_pak_unload_sdk_paks_on_shutdown("sdk_pak_unload_sdk_paks_on_shutdown", "1", FCVAR_RELEASE,
	"Unload SDK-enqueued paks before the engine unloads its precache groups.");

struct SdkPakRecord_S21
{
	int  handle;
	char name[64];
};
static SdkPakRecord_S21 s_sdkPakRecords[64];
static int s_sdkPakRecordCount = 0;

static void Pak_RecordSdkHandle_S21(const char* name, int handle)
{
	if (handle == -1 || s_sdkPakRecordCount >= 64) return;
	SdkPakRecord_S21& r = s_sdkPakRecords[s_sdkPakRecordCount++];
	r.handle = handle;
	strncpy_s(r.name, name ? name : "", _TRUNCATE);
}

// Script RPAKs always blocked; disk scripts load via VPlatformFSOverrideS21.

// Fixed r5apex.data slot table (512 * 0x160). Preflight once; plain reads after.
static bool s_pakSlotTableChecked = false;
static bool s_pakSlotTableOk = false;

static bool EnsurePakSlotTableOk()
{
	if (s_pakSlotTableChecked)
		return s_pakSlotTableOk;
	s_pakSlotTableChecked = true;
	const uintptr_t slotBase = Pak_GetSlotBase_S21();
	if (!slotBase)
	{
		s_pakSlotTableOk = false;
		return false;
	}
	s_pakSlotTableOk = Mem_InModule(g_GameDll,
		reinterpret_cast<const void*>(slotBase),
		kS21_PakSlotCount * kS21_PakSlotStride);
	return s_pakSlotTableOk;
}

static const char* GetPakNameForSlot(int slot)
{
	if (slot < 0 || slot >= static_cast<int>(kS21_PakSlotCount))
		return nullptr;
	if (!EnsurePakSlotTableOk())
		return nullptr;
	const uintptr_t slotBase = Pak_GetSlotBase_S21();
	const char** pName = reinterpret_cast<const char**>(
		slotBase + static_cast<size_t>(slot) * kS21_PakSlotStride +
		kS21_PakSlot_Name);
	return *pName;
}

// Exact basename match for script-asset rpaks (no prefix).
static bool IsScriptAssetRpak(const char* name)
{
	if (!name) return false;
	return (strcmp(name, "script_ui.rpak")     == 0) ||
		   (strcmp(name, "script_client.rpak") == 0);
}

// settings/rpak_roots/X -> root_<X>.rpak (slash to underscore). ODL stores the script-form path.
static bool TranslateRpakPath(const char* name, char* outBuf, size_t outCap)
{
	if (!name || !outBuf || outCap < 16) return false;
	static const char kPrefix[] = "settings/rpak_roots/";
	constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;

	// ASCII case-insensitive prefix.
	for (size_t i = 0; i < kPrefixLen; ++i)
	{
		const unsigned char a = static_cast<unsigned char>(name[i]);
		const unsigned char b = static_cast<unsigned char>(kPrefix[i]);
		if (!a) return false;
		const unsigned char ax = (a >= 'A' && a <= 'Z') ? a + 32 : a;
		const unsigned char bx = (b >= 'A' && b <= 'Z') ? b + 32 : b;
		if (ax != bx) return false;
	}

	const char* suffix = name + kPrefixLen;
	const size_t suffixLen = strlen(suffix);
	constexpr const char kHead[] = "root_";
	constexpr size_t kHeadLen = sizeof(kHead) - 1;
	if (kHeadLen + suffixLen + 6 > outCap) return false;

	memcpy(outBuf, kHead, kHeadLen);
	memcpy(outBuf + kHeadLen, suffix, suffixLen);
	outBuf[kHeadLen + suffixLen] = 0;
	for (size_t j = kHeadLen; j < kHeadLen + suffixLen; ++j)
		if (outBuf[j] == '/') outBuf[j] = '_';

	// Append .rpak if missing.
	const size_t total = kHeadLen + suffixLen;
	const bool hasRpak =
		total >= 5 &&
		(outBuf[total - 5] == '.') && (outBuf[total - 4] == 'r' || outBuf[total - 4] == 'R') &&
		(outBuf[total - 3] == 'p' || outBuf[total - 3] == 'P') &&
		(outBuf[total - 2] == 'a' || outBuf[total - 2] == 'A') &&
		(outBuf[total - 1] == 'k' || outBuf[total - 1] == 'K');
	if (!hasRpak)
	{
		if (total + 5 + 1 > outCap) return false;
		memcpy(outBuf + total, ".rpak", 5);
		outBuf[total + 5] = 0;
	}
	return true;
}

// Flat basename only: [A-Za-z0-9_().]. Rejects UNC, drive, slash, and "..".
bool Pak_IsAllowedLoadName_S21(const char* name)
{
	if (!name || !*name)
		return false;
	if (name[0] == '/' || name[0] == '\\')
		return false;

	size_t n = 0;
	for (const char* p = name; *p; ++p, ++n)
	{
		if (n >= 128)
			return false;
		const unsigned char c = static_cast<unsigned char>(*p);
		if (c == '\\' || c == '/' || c == ':')
			return false;
		if (c == '.' && p[1] == '.')
			return false;
		const bool ok =
			(c >= 'A' && c <= 'Z') ||
			(c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') ||
			c == '_' || c == '.' || c == '(' || c == ')';
		if (!ok)
			return false;
	}
	return n > 0;
}

// After common_early, before common. common_flowstate waits for common_mp (its shds live there).
static const char* const s_sdkPaksToLoad[] =
{
	"ui_sdk.rpak",
	"ui_fs.rpak",
};

// After common_mp AND mp_lobby GuidLive. Hash-only is not settled; lobby shds bind CF materials.
static const char* const s_sdkPaksToLoadAfterCommonMp[] =
{
	"common_flowstate.rpak",
	"sdk_s30.rpak",
};
static const size_t s_sdkPaksToLoadAfterCommonMpCount =
	V_ARRAYSIZE(s_sdkPaksToLoadAfterCommonMp);

// Late common_mp GUIDs that miss the hash immediately after LOADED.
static const unsigned __int64 s_commonMpCanaryGuids[] =
{
	0x3B7A97811E15614AULL,
	0x41C205864FE882CCULL,
	0xFA9D2BC326DF5B69ULL,
	0xB562BDFF4C9837EDULL,
	0xE59B8C6C62951190ULL,
	0xFA1B2B45C5F6D5B6ULL,
	0xE0E2EC25A70317BBULL,
	0xF8C0F59DDB3A28E7ULL,
};
// mp_lobby shds CF binds (PS@5 / VS@13).
static const unsigned __int64 s_lobbyCanaryGuids[] =
{
	0x67288D906E14D1B7ULL,
};
static const int kAfterCommonCanaryTries = 8;
static const int kLobbyLivePollMs = 5;
static const int kLobbyLivePollMax = 100;
static constexpr uintptr_t kS21LoadedAssetsFromHash = 0x400000;
static constexpr size_t kS21LoadedAssetStride = 0x10;
static bool s_sawCommonMpLoaded = false;
static bool s_sawLobbyLoaded = false;
static volatile long s_afterCommonRequested = 0;
static volatile long s_canaryTries = 0;

static bool ConsistencyObs_GuidResolves(uintptr_t hashBase, unsigned __int64 guid);
static bool ConsistencyObs_GuidLive(uintptr_t hashBase, unsigned __int64 guid);

// HAS_MODULE LoadLibraryExA uses a bare name. Preload the sibling by absolute path so it is already resident.
static void Pak_PreloadSiblingModule_S21(const char* const pakName)
{
	if (!pakName || !pakName[0])
		return;

	char stem[MAX_PATH];
	V_strncpy(stem, pakName, sizeof(stem));

	char* slash = strrchr(stem, '\\');
	if (!slash)
		slash = strrchr(stem, '/');
	if (slash)
		memmove(stem, slash + 1, V_strlen(slash + 1) + 1);

	char* const pExt = strrchr(stem, '.');
	if (pExt)
		*pExt = '\0';

	bool bAllowed = false;
	for (size_t i = 0; i < Pak_AllowedModuleStemCount; ++i)
	{
		if (_stricmp(stem, Pak_AllowedModuleStems[i]) == 0)
		{
			bAllowed = true;
			break;
		}
	}
	if (!bAllowed)
	{
		static volatile LONG s_notListed = 0;
		if (InterlockedIncrement(&s_notListed) == 1)
			Warning(eDLL_T::RTECH,
				"[PAK-MODULE] sibling for '%s' not allowlisted\n", pakName);
		return;
	}

	char szExeDir[MAX_PATH];
	if (!GetModuleFileNameA(nullptr, szExeDir, sizeof(szExeDir)))
		return;

	char* const pLastSlash = strrchr(szExeDir, '\\');
	if (pLastSlash)
		*pLastSlash = '\0';
	else
		szExeDir[0] = '\0';

	char absDllPath[MAX_PATH];
	snprintf(absDllPath, sizeof(absDllPath), "%s\\paks\\Win64\\%s.dll", szExeDir, stem);

	if (strstr(absDllPath, ".."))
	{
		static volatile LONG s_dotdot = 0;
		if (InterlockedIncrement(&s_dotdot) == 1)
			Warning(eDLL_T::RTECH,
				"[PAK-MODULE] sibling path refused for '%s'\n", pakName);
		return;
	}

	const DWORD attrs = GetFileAttributesA(absDllPath);
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
		return;
	if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
	{
		static volatile LONG s_reparse = 0;
		if (InterlockedIncrement(&s_reparse) == 1)
			Warning(eDLL_T::RTECH,
				"[PAK-MODULE] sibling path refused for '%s'\n", pakName);
		return;
	}

	char dllName[MAX_PATH];
	snprintf(dllName, sizeof(dllName), "%s.dll", stem);

	if (GetModuleHandleA(dllName))
		return;

	if (!LoadLibraryA(absDllPath))
	{
		Warning(eDLL_T::RTECH,
			"[PAK-MODULE] LoadLibrary failed for '%s' (err=%lu)\n",
			absDllPath, GetLastError());
		return;
	}

	Msg(eDLL_T::RTECH, "[PAK-MODULE] preloaded '%s'\n", absDllPath);
}

static void Pak_EnqueueNamedList_S21(const char* const* names, size_t count, const char* tag,
	const bool bAllowSiblingModule)
{
	if (!Pak_RequestLoadByName_S21Resolve())
	{
 Warning(eDLL_T::RTECH,
 "[%s] Pak_RequestLoadByName_S21 not resolved -- SDK paks not loaded\n", tag);
 return;
	}

	const uintptr_t allocSlot = Pak_GetGlobalAllocatorSlot_S21();
	if (!allocSlot)
	{
 Warning(eDLL_T::RTECH,
 "[%s] Pak_GetGlobalAllocatorSlot_S21 not resolved -- SDK paks not loaded\n", tag);
 return;
	}

	auto pfnFull = reinterpret_cast<PFN_Pak_RequestLoadByName_Full_S21>(v_Pak_RequestLoadByName_S21);

	for (size_t i = 0; i < count; ++i)
	{
 const char* const name = names[i];
 if (name && !_stricmp(name, "common_flowstate.rpak") &&
	 !sdk_pak_load_flowstate.GetBool())
 {
	 Warning(eDLL_T::RTECH,
		 "[%s] skip '%s' (sdk_pak_load_flowstate 0)\n", tag, name);
	 continue;
 }
 if (name && !_stricmp(name, "sdk_s30.rpak") &&
	 !sdk_pak_load_s30.GetBool())
 {
	 Warning(eDLL_T::RTECH,
		 "[%s] skip '%s' (sdk_pak_load_s30 0)\n", tag, name);
	 continue;
 }
 char diskPath[MAX_PATH];
 snprintf(diskPath, sizeof(diskPath), "paks\\Win64\\%s", name);

 const DWORD attrs = GetFileAttributesA(diskPath);
 if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
 {
 Warning(eDLL_T::RTECH,
 "[%s] '%s' not on disk (%s) -- skipping\n", tag, name, diskPath);
 continue;
 }

 if (bAllowSiblingModule)
	 Pak_PreloadSiblingModule_S21(name);

 const int handle = pfnFull(name, 1 /*priority*/, allocSlot, 8 /*c4*/, 0 /*trackFeature*/);
		if (handle != -1)
			Pak_RecordSdkHandle_S21(name, handle);
		if (handle == -1)
		{
			Warning(eDLL_T::RTECH,
				"[%s] '%s' enqueue failed (engine returned -1)\n", tag, name);
		}
		else
		{
			Msg(eDLL_T::RTECH,
				"[%s] loading '%s' (handle 0x%X)\n", tag, name, handle & 0xFFFFFF);
		}
	}
}

static void Pak_LoadSdkPaks_S21()
{
	if (!sdk_pak_load_sdk_paks.GetBool())
		return;

	static bool s_sdkPaksRequested = false;
	if (s_sdkPaksRequested)
		return;
	s_sdkPaksRequested = true;

	Pak_EnqueueNamedList_S21(s_sdkPaksToLoad, V_ARRAYSIZE(s_sdkPaksToLoad), "UI-SDK", true);
}

static uintptr_t Pak_GuidHashBase_S21(void)
{
	return S21Pak_AssetGuidHashBase();
}

static bool Pak_CommonMpCanaryResolves_S21(unsigned int* pHit, unsigned int* pNeed)
{
	const unsigned int need = static_cast<unsigned int>(V_ARRAYSIZE(s_commonMpCanaryGuids));
	if (pNeed)
		*pNeed = need;

	const uintptr_t hashBase = Pak_GuidHashBase_S21();
	if (!hashBase)
	{
		if (pHit)
			*pHit = 0;
		return false;
	}

	unsigned int hit = 0;
	for (unsigned int i = 0; i < need; ++i)
	{
		if (ConsistencyObs_GuidLive(hashBase, s_commonMpCanaryGuids[i]))
			++hit;
	}

	if (pHit)
		*pHit = hit;
	return hit == need;
}

static bool Pak_LobbyCanaryResolves_S21(unsigned int* pHit, unsigned int* pNeed)
{
	const unsigned int need = static_cast<unsigned int>(V_ARRAYSIZE(s_lobbyCanaryGuids));
	if (pNeed)
		*pNeed = need;

	const uintptr_t hashBase = Pak_GuidHashBase_S21();
	if (!hashBase)
	{
		if (pHit)
			*pHit = 0;
		return false;
	}

	unsigned int hit = 0;
	for (unsigned int i = 0; i < need; ++i)
	{
		if (ConsistencyObs_GuidLive(hashBase, s_lobbyCanaryGuids[i]))
			++hit;
	}

	if (pHit)
		*pHit = hit;
	return hit == need;
}

static constexpr int kModPakNamesMax = 256;
static constexpr size_t kModPakNameMax = 128;
static constexpr size_t kModPreloadRsonCap = 256u * 1024u;

static const char* Pak_SkipWsAndComments_S21(const char* p, const char* const end, int* pSafety)
{
	const int kSafety = 1 << 20;
	for (;;)
	{
		if (++(*pSafety) > kSafety)
			return end;
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ','))
			++p;
		if (p + 1 < end && p[0] == '/' && p[1] == '/')
		{
			p += 2;
			while (p < end && *p != '\n')
				++p;
			continue;
		}
		if (p + 1 < end && p[0] == '/' && p[1] == '*')
		{
			p += 2;
			while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
				++p;
			if (p + 1 < end)
				p += 2;
			continue;
		}
		break;
	}
	return p;
}

static bool Pak_IsPreloadNameDelim_S21(const unsigned char c)
{
	return c == ']' || c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int Pak_ParsePreloadPaksText_S21(const char* text, const size_t len,
	char names[][kModPakNameMax], const int maxNames, const char* pszModId)
{
	if (!text || !len || !names || maxNames <= 0)
		return -1;

	const char* const who = (pszModId && pszModId[0]) ? pszModId : "unknown";
	const char* const end = text + len;
	const char* p = text;
	int nSafety = 0;
	bool foundKey = false;

	while (p < end)
	{
		p = Pak_SkipWsAndComments_S21(p, end, &nSafety);
		if (p >= end)
			break;
		if (*p == '"')
		{
			++p;
			const char* const s = p;
			while (p < end && *p != '"')
				++p;
			if (p < end && (p - s) == 4 && !_strnicmp(s, "Paks", 4))
			{
				++p;
				foundKey = true;
				break;
			}
			if (p < end)
				++p;
			continue;
		}
		if (p + 4 <= end && !_strnicmp(p, "Paks", 4))
		{
			const char next = (p + 4 < end) ? p[4] : '\0';
			const bool identCont = (next >= 'A' && next <= 'Z')
				|| (next >= 'a' && next <= 'z')
				|| (next >= '0' && next <= '9')
				|| next == '_';
			if (!identCont)
			{
				p += 4;
				foundKey = true;
				break;
			}
		}
		++p;
	}

	if (!foundKey)
		return 0;

	p = Pak_SkipWsAndComments_S21(p, end, &nSafety);
	if (p < end && *p == ':')
	{
		++p;
		p = Pak_SkipWsAndComments_S21(p, end, &nSafety);
	}
	if (p >= end || *p != '[')
		return -1;
	++p;

	int nOut = 0;
	while (p < end && nOut < maxNames)
	{
		p = Pak_SkipWsAndComments_S21(p, end, &nSafety);
		if (p >= end)
			return -1;
		if (*p == ']')
			return nOut;

		char tok[kModPakNameMax];
		int nTok = 0;
		bool bOversize = false;
		if (*p == '"')
		{
			++p;
			while (p < end && *p != '"')
			{
				if (nTok < static_cast<int>(kModPakNameMax) - 1)
					tok[nTok++] = *p;
				else
					bOversize = true;
				++p;
				if (++nSafety > (1 << 20))
					return -1;
			}
			if (p >= end || *p != '"')
				return -1;
			++p;
		}
		else
		{
			while (p < end)
			{
				const unsigned char c = static_cast<unsigned char>(*p);
				if (Pak_IsPreloadNameDelim_S21(c))
					break;
				if (nTok < static_cast<int>(kModPakNameMax) - 1)
					tok[nTok++] = *p;
				else
					bOversize = true;
				++p;
				if (++nSafety > (1 << 20))
					return -1;
			}
		}
		tok[nTok] = '\0';
		if (bOversize)
		{
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-PAK] '%s' rejected oversized pak name '%s'\n", who, tok);
			nTok = 0;
		}
		if (nTok > 0)
		{
			if (!Pak_IsAllowedLoadName_S21(tok))
			{
				Warning(eDLL_T::MODSYSTEM,
					"[MOD-PAK] '%s' rejected name '%s'\n", who, tok);
			}
			else
			{
				V_strncpy(names[nOut], tok, static_cast<int>(kModPakNameMax));
				++nOut;
			}
		}
		if (++nSafety > (1 << 20))
			return -1;
	}
	return nOut;
}

static void Pak_EnqueueModPaks_S21(void)
{
	if (!sdk_pak_load_mod_paks.GetBool())
		return;
	if (!ModSystem()->IsEnabled())
		return;
	if (!Pak_RequestLoadByName_S21Resolve())
	{
		Warning(eDLL_T::RTECH,
			"[MOD-PAK] Pak_RequestLoadByName_S21 not resolved -- mod paks not loaded\n");
		return;
	}

	const uintptr_t allocSlot = Pak_GetGlobalAllocatorSlot_S21();
	if (!allocSlot)
	{
		Warning(eDLL_T::RTECH,
			"[MOD-PAK] allocator slot unresolved -- mod paks not loaded\n");
		return;
	}

	if (!ModSystem_IsSafeRelativePath("paks/Win64/preload.rson"))
		return;

	auto pfnFull = reinterpret_cast<PFN_Pak_RequestLoadByName_Full_S21>(v_Pak_RequestLoadByName_S21);

	ModSystem()->LockModList();
	FOR_EACH_VEC(ModSystem()->GetResolvedModList(), i)
	{
		const CModSystem::ModInstance_t* const mod = ModSystem()->GetResolvedModList()[i];
		if (!mod || !mod->IsEnabled())
			continue;

		char preloadPath[MAX_PATH];
		if (_snprintf_s(preloadPath, sizeof(preloadPath), _TRUNCATE, "%spaks/Win64/preload.rson",
			mod->GetBasePath().String()) < 0)
			continue;
		V_FixSlashes(preloadPath);

		HANDLE h = CreateFileA(preloadPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			continue;

		LARGE_INTEGER li;
		if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0)
		{
			CloseHandle(h);
			continue;
		}
		if (li.QuadPart > static_cast<LONGLONG>(kModPreloadRsonCap))
		{
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-PAK] '%s' preload.rson exceeds cap -- skipped\n",
				mod->id.String());
			CloseHandle(h);
			continue;
		}

		const DWORD size = static_cast<DWORD>(li.QuadPart);
		char* const text = static_cast<char*>(malloc(size + 1));
		if (!text)
		{
			CloseHandle(h);
			continue;
		}

		DWORD readBytes = 0;
		const BOOL readOk = ReadFile(h, text, size, &readBytes, nullptr);
		CloseHandle(h);
		if (!readOk || readBytes != size)
		{
			free(text);
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-PAK] '%s' failed to read preload.rson -- skipped\n",
				mod->id.String());
			continue;
		}
		text[size] = '\0';

		char names[kModPakNamesMax][kModPakNameMax];
		const int nNames = Pak_ParsePreloadPaksText_S21(text, size, names, kModPakNamesMax,
			mod->id.String());
		free(text);
		if (nNames < 0)
		{
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-PAK] '%s' preload.rson parse failed -- skipped\n",
				mod->id.String());
			continue;
		}

		for (int n = 0; n < nNames; ++n)
		{
			const char* const pakName = names[n];
			if (!Pak_IsAllowedLoadName_S21(pakName))
			{
				Warning(eDLL_T::MODSYSTEM,
					"[MOD-PAK] '%s' rejected name '%s'\n",
					mod->id.String(), pakName);
				continue;
			}

			char diskPath[MAX_PATH];
			if (_snprintf_s(diskPath, sizeof(diskPath), _TRUNCATE, "%spaks/Win64/%s",
				mod->GetBasePath().String(), pakName) < 0)
				continue;
			V_FixSlashes(diskPath);

			const DWORD attrs = GetFileAttributesA(diskPath);
			if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
			{
				Warning(eDLL_T::MODSYSTEM,
					"[MOD-PAK] '%s' missing '%s' (%s) -- skipped\n",
					mod->id.String(), pakName, diskPath);
				continue;
			}

			const int handle = pfnFull(pakName, 1, allocSlot, 8, 0);
			if (handle != -1)
				Pak_RecordSdkHandle_S21(pakName, handle);
			if (handle == -1)
			{
				Warning(eDLL_T::MODSYSTEM,
					"[MOD-PAK] '%s' enqueue failed for '%s'\n",
					mod->id.String(), pakName);
			}
			else
			{
				Msg(eDLL_T::MODSYSTEM, "[MOD-PAK] %s -> %s\n",
					mod->id.String(), pakName);
			}
		}
	}
	ModSystem()->UnlockModList();
}

static void Pak_LoadSdkPaksAfterCommon_S21()
{
	if (s_afterCommonRequested)
		return;

	if (!s_sawLobbyLoaded)
		return;

	unsigned int hit = 0;
	unsigned int need = 0;
	unsigned int lobbyHit = 0;
	unsigned int lobbyNeed = 0;
	const bool mpOk = Pak_CommonMpCanaryResolves_S21(&hit, &need);
	const bool lobbyOk = Pak_LobbyCanaryResolves_S21(&lobbyHit, &lobbyNeed);
	if (!mpOk || !lobbyOk)
	{
		const long tries = InterlockedIncrement(&s_canaryTries);
		if (tries <= kAfterCommonCanaryTries || (tries % 16) == 0)
		{
			Warning(eDLL_T::RTECH,
				"[FS-PAK] GuidLive canary miss mp=%u/%u lobby=%u/%u -- defer enqueue (try %ld)\n",
				hit, need, lobbyHit, lobbyNeed, tries);
		}
		return;
	}
	else if (s_canaryTries > 0)
	{
		Msg(eDLL_T::RTECH,
			"[FS-PAK] GuidLive canary ok mp=%u/%u lobby=%u/%u after %ld defer(s) -- enqueue\n",
			hit, need, lobbyHit, lobbyNeed, s_canaryTries);
	}

	if (InterlockedCompareExchange(&s_afterCommonRequested, 1, 0) != 0)
		return;

	if (sdk_pak_load_sdk_paks.GetBool()
		&& s_sdkPaksToLoadAfterCommonMpCount != 0
		&& s_sdkPaksToLoadAfterCommonMp)
	{
		Pak_EnqueueNamedList_S21(s_sdkPaksToLoadAfterCommonMp,
			s_sdkPaksToLoadAfterCommonMpCount, "FS-PAK", true);
	}
	else if (!sdk_pak_load_sdk_paks.GetBool())
	{
		Msg(eDLL_T::RTECH,
			"[FS-PAK] after common_mp: sdk_pak_load_sdk_paks 0 -- SDK list skipped\n");
	}
	else
	{
		Msg(eDLL_T::RTECH,
			"[FS-PAK] after common_mp: no content paks queued (parked)\n");
	}

	Pak_EnqueueModPaks_S21();
}

// Translate ODL script-form paths; block script_ui/script_client. Full 5-arg so r8/r9/stack stay intact.
static int __fastcall Hook_Pak_RequestLoadByName_S21(
	const char* name,
	char        priority,
	uintptr_t   allocatorSlot,
	char        c4,
	char        trackFeature)
{
	char xlatBuf[300];
	const char* effective = name;
	if (TranslateRpakPath(name, xlatBuf, sizeof(xlatBuf)))
	{
		effective = xlatBuf;
		static int s_xlatLog = 0;
		if (++s_xlatLog <= 32 || (s_xlatLog % 64) == 0)
		{
			Msg(eDLL_T::RTECH,
				"[ODL-XLAT] '%s' -> '%s'\n", name, xlatBuf);
		}
	}

	if (!Pak_IsAllowedLoadName_S21(effective))
	{
		static int s_rejectLog = 0;
		if (++s_rejectLog <= 32 || (s_rejectLog % 64) == 0)
		{
			Warning(eDLL_T::RTECH,
				"[PAK-NAME] reject '%s' (effective '%s')\n",
				name ? name : "(null)", effective ? effective : "(null)");
		}
		return -1;
	}

	if (IsScriptAssetRpak(effective))
		return -1;

	// Missing-file fake-success lives in InitAsyncLoad. Returning -1 here stored slot 511 and fatals.

	if (!v_Pak_RequestLoadByName_S21) return -1;
	const int result = v_Pak_RequestLoadByName_S21(effective, priority,
												   allocatorSlot, c4, trackFeature);

	// ui.rpak is after common_early and before common; queue UI SDK paks here.
	if (effective && _stricmp(effective, "ui.rpak") == 0)
		Pak_LoadSdkPaks_S21();

	// Do not enqueue CF on RequestLoadByName of common_mp (LOAD_PENDING race). Retry after lobby LOADED.
	if (s_sawCommonMpLoaded && s_sawLobbyLoaded)
		Pak_LoadSdkPaksAfterCommon_S21();

	return result;
}

// File-open gate: missing file -> status 14 ERROR, not severity-5 TerminateProcess.
static bool VerifyPakOnDisk(const char* slotName)
{
	if (!slotName || !*slotName) return false;

	char xlatBuf[300];
	const char* checkName = slotName;
	if (TranslateRpakPath(slotName, xlatBuf, sizeof(xlatBuf)))
		checkName = xlatBuf;

	// Never GetFileAttributesA a UNC/traversal/absolute name.
	if (!Pak_IsAllowedLoadName_S21(checkName))
		return false;

	char engineLookup[420];
	snprintf(engineLookup, sizeof(engineLookup),
			 "paks\\Win64\\%s", checkName);
	const DWORD attrs = GetFileAttributesA(engineLookup);
	return attrs != INVALID_FILE_ATTRIBUTES &&
		(attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// ODL/skin paks are polled (fake LOADED on miss). Map/engine paks use the pump (do not fake).
static bool PakNameEndsWithI(const char* name, const char* suffix)
{
	const size_t n = strlen(name);
	const size_t s = strlen(suffix);
	if (n < s)
		return false;
	return _strnicmp(name + n - s, suffix, s) == 0;
}

static bool IsOdlSkinPak(const char* name)
{
	if (!name || !*name)
		return false;
	for (const char* p = name; *p; ++p)
		if (*p == '/' || *p == '\\')
			return true;
	if (strncmp(name, "root_", 5) != 0)
		return false;
	if (PakNameEndsWithI(name, "_client_temp.rpak") ||
		PakNameEndsWithI(name, "_loadscreen.rpak"))
		return false;
	return true;
}

static char __fastcall Hook_Pak_InitAsyncLoad_S21(__int64 pakHnd)
{
	if (!v_Pak_InitAsyncLoad_S21)
		return 0;

	const int slot = static_cast<int>(pakHnd) & 0x1FF;
	const char* name = GetPakNameForSlot(slot);

	// Missing ODL pak: report status=10 LOADED so the pool worker does not
	// TerminateProcess on ERROR, but return 0 so the pump skips the header
	// parse (it dereferences the parsed-pak block this slot never allocated)
	// and finalizes the slot on its next pass.
	if (name && *name && !VerifyPakOnDisk(name))
	{
		if (IsOdlSkinPak(name))
		{
			const uintptr_t slotTable = Pak_GetSlotBase_S21();
			if (slotTable && EnsurePakSlotTableOk())
			{
				const uintptr_t slotBase = slotTable
					+ static_cast<size_t>(slot) * kS21_PakSlotStride;
				*reinterpret_cast<uint32_t*>(slotBase + kS21_PakSlot_Status) = 10;
			}
			static int s_skipLog = 0;
			if (++s_skipLog <= 32 || (s_skipLog % 128) == 0)
			{
				Warning(eDLL_T::RTECH,
					"[ODL-XLAT] missing ODL/skin pak slot=%d '%s' -- faking "
					"PAK_STATUS_LOADED (incomplete skin: ItemFlavor "
					"references rpak not shipped in this distribution; "
					"model will render as error.rmdl on use)\n",
					slot, name);
			}
			return 0;
		}

		// Map/engine miss: do not fake; original returns 0 and the pump skips the slot.
		static int s_mapSkipLog = 0;
		if (++s_mapSkipLog <= 32 || (s_mapSkipLog % 128) == 0)
		{
			Warning(eDLL_T::RTECH,
				"missing map pak slot=%d '%s' -- not on disk; letting the "
				"loader skip it (BSP loads from disk; perm/temp not required)\n",
				slot, name);
		}

	}

	char result = 0;
	__try
	{
		result = v_Pak_InitAsyncLoad_S21(pakHnd);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::RTECH,
			"pak load exception. '%s'\n",
			name ? name : "(null)");
		return 0;
	}

	if (result == 0)
	{
		Warning(eDLL_T::RTECH, "pak load failed. '%s'\n",
			name ? name : "(unknown)");
	}
	else
	{
		Msg(eDLL_T::RTECH, "pak loaded. '%s'\n",
			name ? name : "(unknown)");

		// After common_mp LOADED, probe the canary. A miss does not latch.
		if (name && _stricmp(name, "common_mp.rpak") == 0)
			s_sawCommonMpLoaded = true;
		if (name && _stricmp(name, "mp_lobby.rpak") == 0)
		{
			s_sawLobbyLoaded = true;
			// "pak loaded" is ahead of guid-hash publish; spin until lobby GuidLive.
			unsigned int lobbyHit = 0;
			unsigned int lobbyNeed = 0;
			for (int i = 0; i < kLobbyLivePollMax; ++i)
			{
				if (Pak_LobbyCanaryResolves_S21(&lobbyHit, &lobbyNeed))
					break;
				Sleep(kLobbyLivePollMs);
			}
			if (lobbyHit != lobbyNeed)
			{
				Warning(eDLL_T::RTECH,
					"[FS-PAK] lobby GuidLive still %u/%u after %d ms\n",
					lobbyHit, lobbyNeed,
					kLobbyLivePollMs * kLobbyLivePollMax);
			}
		}
		if (s_sawCommonMpLoaded)
			Pak_LoadSdkPaksAfterCommon_S21();
	}

	return result;
}

static void __fastcall Hook_Pak_UnloadAsyncByHandle_S21(unsigned int handle, int wait)
{
	const char* const name = GetPakNameForSlot(static_cast<int>(handle) & 0x1FF);
	Msg(eDLL_T::RTECH, "pak unload requested. '%s' (wait=%d)\n",
		name ? name : "(unknown)", wait);
	v_Pak_UnloadAsyncByHandle_S21(handle, wait);
}

static void Pak_UnloadSdkPaks_S21()
{
	const uintptr_t base = Pak_GetSlotBase_S21();
	if (!base || !v_Pak_UnloadAsyncByHandle_S21) return;

	for (int i = s_sdkPakRecordCount - 1; i >= 0; --i)
	{
		const SdkPakRecord_S21& r = s_sdkPakRecords[i];
		const uintptr_t slot = base + (uintptr_t)(r.handle & 0x1FF) * kS21_PakSlotStride;
		const int slotHandle = *reinterpret_cast<int*>(slot + kS21_PakSlot_Handle);
		const int status     = *reinterpret_cast<int*>(slot + kS21_PakSlot_Status);

		if (slotHandle != r.handle || status == 0)
		{
			Msg(eDLL_T::RTECH, "[SDK-PAK-UNLOAD] skip '%s' (handle 0x%X, slot handle 0x%X, status %s)\n",
				r.name, r.handle & 0xFFFFFF, slotHandle & 0xFFFFFF, Pak_StatusToString_S21(status));
			continue;
		}

		Msg(eDLL_T::RTECH, "[SDK-PAK-UNLOAD] unloading '%s' (handle 0x%X, status %s)\n",
			r.name, r.handle & 0xFFFFFF, Pak_StatusToString_S21(status));
		v_Pak_UnloadAsyncByHandle_S21((unsigned int)r.handle, 1);
	}
	s_sdkPakRecordCount = 0;
}

static void __fastcall Hook_Pak_PreCache_UnloadAll_S21()
{
	Msg(eDLL_T::RTECH, "[SDK-PAK-UNLOAD] Pak_PreCache_UnloadAll: %d SDK pak(s) recorded, gate %d\n",
		s_sdkPakRecordCount, sdk_pak_unload_sdk_paks_on_shutdown.GetBool() ? 1 : 0);
	if (sdk_pak_unload_sdk_paks_on_shutdown.GetBool())
		Pak_UnloadSdkPaks_S21();
	v_Pak_PreCache_UnloadAll_S21();
}

// severity==5 is dumpless TerminateProcess. Neuter known-safe pak fatals; others pass through.
static void FatalObs_Render(char* out, size_t n, const char* fmt, __int64 vargs)
{
	if (!out || n == 0) return;
	out[0] = '\0';
	__try
	{
		if (fmt)
			vsnprintf(out, n, fmt, reinterpret_cast<va_list>(vargs));
		out[n - 1] = '\0';
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		const char* k = "<unrenderable fatal msg>";
		size_t i = 0;
		for (; i < n - 1 && k[i]; ++i) out[i] = k[i];
		out[i] = '\0';
	}
}

static void __fastcall Hook_FatalErrorLogger_S21(char severity, const char* fmt, __int64 vargs)
{
	// Log every engine fatal before the pak-only neuter.
	char msg[1024];
	FatalObs_Render(msg, sizeof(msg), fmt, vargs);

	// Caller VA for [FATAL-OBS] and the allowlist.
	void* const ret = _ReturnAddress();
	const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	const unsigned long long callerVa =
		base ? (0x140000000ULL + (reinterpret_cast<uintptr_t>(ret) - base)) : 0;

	// Capture a real stack; _ReturnAddress is the varargs wrapper for many fatals.
	void* stackFrames[24] = {};
	const USHORT frameCount = RtlCaptureStackBackTrace(0, 24, stackFrames, nullptr);
	char stackBuf[24 * 20 + 1] = {};
	size_t stackBufLen = 0;
	for (USHORT i = 0; i < frameCount && stackBufLen + 20 < sizeof(stackBuf); ++i)
	{
		const uintptr_t frameAddr = reinterpret_cast<uintptr_t>(stackFrames[i]);
		const unsigned long long frameVa =
			base ? (0x140000000ULL + (frameAddr - base)) : 0;
		stackBufLen += static_cast<size_t>(snprintf(stackBuf + stackBufLen,
			sizeof(stackBuf) - stackBufLen, "0x%llX ", frameVa));
	}

	Warning(eDLL_T::RTECH,
		"[FATAL-OBS] sev=%d caller_va=0x%llX stack=[ %s] msg=\"%s\"\n",
		(int)severity, callerVa, stackBuf, msg);
	BridgeTrace_Log(
		"[FATAL-OBS] sev=%d caller_va=0x%llX stack=[ %s] msg=\"%s\"\n",
		(int)severity, callerVa, stackBuf, msg);
	BridgeTrace_Flush();

	// Sev=5: neuter missing .opt.starpak HD streams and allowlisted caller VAs only.
	if (severity == 5)
	{
		const bool missingOptStarpak =
			msg[0] &&
			strstr(msg, "Error opening streaming file") != nullptr &&
			strstr(msg, ".opt.starpak") != nullptr;

		// Streaming-file open fatal caller.
		const uintptr_t fatalCaller = NetObs_Sym(NetObsSym_t::PakStreamFatalCaller);
		const unsigned long long kPakFatalAllowlist[] = {
			(base && fatalCaller) ? (0x140000000ULL + (fatalCaller - base)) : 0ULL,
		};
		constexpr size_t kPakFatalAllowlistCount =
			sizeof(kPakFatalAllowlist) / sizeof(kPakFatalAllowlist[0]);

		bool allow = missingOptStarpak;
		unsigned long long matchedVa = 0;
		if (!allow)
		{
			for (size_t ai = 0; ai < kPakFatalAllowlistCount; ++ai)
			{
				if (callerVa == kPakFatalAllowlist[ai])
				{
					allow = true;
					matchedVa = kPakFatalAllowlist[ai];
					break;
				}
			}
		}

		if (allow)
		{
			static volatile long s_neuterN = 0;
			const long n = InterlockedIncrement(&s_neuterN);
			if (n <= 64 || (n % 512) == 0)
				Warning(eDLL_T::RTECH,
					"[PAK-FATAL-NEUTER] #%ld suppressed engine TerminateProcess(sev=5) "
					"caller_va=0x%llX matched_va=0x%llX opt_starpak=%d -- msg=\"%.160s\"\n",
					n, callerVa, matchedVa, missingOptStarpak ? 1 : 0, msg);
			return;
		}

		// Not allowlisted.
		static volatile long s_missN = 0;
		const long mn = InterlockedIncrement(&s_missN);
		if (mn <= 32 || (mn % 256) == 0)
			Warning(eDLL_T::RTECH,
				"[FATAL-NEUTER-ALLOWLIST] #%ld sev=5 caller_va=0x%llX NOT in allowlist "
				"-- passing through; add to kPakFatalAllowlist to neuter. msg=\"%.128s\"\n",
				mn, callerVa, msg);
	}
	v_FatalErrorLogger_S21(severity, fmt, vargs);
}

// Loaded-asset hash: 64 B buckets of 8 u64s, quadratic probe; empty slot before a match = miss.
static bool ConsistencyObs_GuidResolves(uintptr_t hashBase, unsigned __int64 guid)
{
	for (unsigned int probe = 0; probe < 512; ++probe)
	{
		const unsigned __int16 bucketIdx =
			(unsigned __int16)(guid + (unsigned __int64)probe * probe);
		const unsigned __int64* const bucket =
			reinterpret_cast<const unsigned __int64*>(hashBase + 64ull * bucketIdx);
		bool bucketFull = true;
		for (int s = 0; s < 8; ++s)
		{
			const unsigned __int64 slot = bucket[s];
			if (slot == 0)    { bucketFull = false; break; }
			if (slot == guid) { return true; }
		}
		if (!bucketFull)
			return false;
	}
	return false;
}

// Installed asset header of a guid (NULL when absent). The head is what a rig's
// sequence array and a model's rig array point at.
void* Pak_FindInstalledHead_S21(unsigned __int64 guid)
{
	const uintptr_t hashBase = S21Pak_AssetGuidHashBase();
	if (!hashBase)
		return nullptr;
	for (unsigned int probe = 0; probe < 512; ++probe)
	{
		const unsigned __int16 bucketIdx = (unsigned __int16)(guid + (unsigned __int64)probe * probe);
		const unsigned __int64* const bucket = reinterpret_cast<const unsigned __int64*>(hashBase + 64ull * bucketIdx);
		for (int sl = 0; sl < 8; ++sl)
		{
			if (bucket[sl] == 0)
				return nullptr;
			if (bucket[sl] != guid)
				continue;
			const unsigned int h = (static_cast<unsigned int>(bucketIdx) << 3) | (sl & 7);
			return *reinterpret_cast<void* const*>(hashBase + 0x400000ull + 16ull * h);
		}
	}
	return nullptr;
}

// Owner chain of one guid: the hash entry (installed header, head tracked index)
// and every tracked copy with its pak slot and slot priority. [PAK-CHAIN]
void Pak_DumpGuidChain_S21(unsigned __int64 guid, const char* tag)
{
	const uintptr_t hashBase = S21Pak_AssetGuidHashBase();
	const uintptr_t slotBase = Pak_GetSlotBase_S21();
	if (!hashBase || !slotBase)
		return;
	for (unsigned int probe = 0; probe < 512; ++probe)
	{
		const unsigned __int16 bucketIdx = (unsigned __int16)(guid + (unsigned __int64)probe * probe);
		const unsigned __int64* const bucket = reinterpret_cast<const unsigned __int64*>(hashBase + 64ull * bucketIdx);
		for (int sl = 0; sl < 8; ++sl)
		{
			if (bucket[sl] == 0)
			{
				Msg(eDLL_T::RTECH, "[PAK-CHAIN] %s 0x%016llX: not in hash\n", tag, guid);
				return;
			}
			if (bucket[sl] != guid)
				continue;
			const unsigned int h = (static_cast<unsigned int>(bucketIdx) << 3) | (sl & 7);
			const uintptr_t entry = hashBase + 0x400000ull + 16ull * h;
			const uintptr_t installed = *reinterpret_cast<const uintptr_t*>(entry);
			unsigned int idx = *reinterpret_cast<const unsigned int*>(entry + 8);
			const unsigned int flags = *reinterpret_cast<const unsigned int*>(entry + 12);
			Msg(eDLL_T::RTECH, "[PAK-CHAIN] %s 0x%016llX: h=%u installed=%p flags=0x%X head=%u\n",
				tag, guid, h, reinterpret_cast<void*>(installed), flags, idx);
			for (int n = 0; n < 8 && idx != 0xFFFFFFFFu; ++n)
			{
				const uintptr_t tracked = hashBase + 0x1482800ull + 16ull * idx;
				const uintptr_t pakHdr = *reinterpret_cast<const uintptr_t*>(tracked);
				const unsigned int next = *reinterpret_cast<const unsigned int*>(tracked + 8);
				const unsigned int pakSlot = *reinterpret_cast<const unsigned __int16*>(tracked + 12) & 0x1FF;
				const uintptr_t slot = slotBase + static_cast<size_t>(pakSlot) * kS21_PakSlotStride;
				const char* const name = *reinterpret_cast<const char* const*>(slot + kS21_PakSlot_Name);
				Msg(eDLL_T::RTECH, "[PAK-CHAIN]   tracked=%u pakHdr=%p slot=%u '%s' status=%d priority=%llu\n",
					idx, reinterpret_cast<void*>(pakHdr), pakSlot, name ? name : "?",
					*reinterpret_cast<const int*>(slot + kS21_PakSlot_Status),
					*reinterpret_cast<const unsigned __int64*>(slot + kS21_PakSlot_Priority));
				idx = next;
			}
			return;
		}
	}
}

static void PakGuidChain_f(const CCommand& args)
{
	if (args.ArgC() < 2)
		return;
	Pak_DumpGuidChain_S21(_strtoui64(args.Arg(1), nullptr, 16), "cmd");
}

static ConCommand sdk_pak_guid_chain("sdk_pak_guid_chain", PakGuidChain_f,
	"Dump the owner chain of a pak asset guid (hex). [PAK-CHAIN]", FCVAR_DEVELOPMENTONLY);

// Hash hit is not enough: live data pointer at hashBase+0x400000+16*slot can still be NULL.
static bool ConsistencyObs_GuidLive(uintptr_t hashBase, unsigned __int64 guid)
{
	for (unsigned int probe = 0; probe < 512; ++probe)
	{
		const unsigned __int16 bucketIdx =
			(unsigned __int16)(guid + (unsigned __int64)probe * probe);
		const unsigned __int64* const bucket =
			reinterpret_cast<const unsigned __int64*>(hashBase + 64ull * bucketIdx);
		bool bucketFull = true;
		for (int s = 0; s < 8; ++s)
		{
			const unsigned __int64 slot = bucket[s];
			if (slot == 0)    { bucketFull = false; break; }
			if (slot != guid)
				continue;

			const unsigned int loadedIndex =
				(static_cast<unsigned int>(bucketIdx) << 3) | (static_cast<unsigned int>(s) & 7);
			const unsigned __int64 live =
				*reinterpret_cast<const unsigned __int64*>(
					hashBase + kS21LoadedAssetsFromHash +
					static_cast<uintptr_t>(loadedIndex) * kS21LoadedAssetStride);
			return live != 0;
		}
		if (!bucketFull)
			return false;
	}
	return false;
}

// Name the referencing asset and each missing guidDesc before the anonymous consistency fatal.
static __int64 __fastcall Hook_PakConsistencyCheck_S21(__int64 a1, __int64 a2)
{
	if (a1 && a2)
	{
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		if (base)
		{
			const uintptr_t hashBase = S21Pak_AssetGuidHashBase();
			const unsigned __int64 assetGuid = *reinterpret_cast<const unsigned __int64*>(a2);
			const unsigned __int16 refCount  = *reinterpret_cast<const unsigned __int16*>(a2 + 64);
			const unsigned int     startIdx  = *reinterpret_cast<const unsigned int*>(a2 + 56);
			const uint32_t pageCount = Pak_S21GetPageCount(reinterpret_cast<const void*>(a1));
			const uint32_t guidDescCount = Pak_S21GetGuidDescCount(reinterpret_cast<const void*>(a1));
			const uintptr_t pageArray = *reinterpret_cast<const uintptr_t*>(a1 + kS21Pak_MemPageBuffersOffset);

			// Keyed on the pak slot, not the struct pointer: slots are reused, so a
			// pointer key silences every later pak in the same slot.
			const int pakSlot = *reinterpret_cast<const int*>(a1 + 0x582C) & 0x1FF;
			const char* const pakName = GetPakNameForSlot(pakSlot);
			auto logSkip = [&](const char* reason)
			{
				static int s_lastSlot = -1;
				static unsigned __int64 s_lastAsset = 0;
				if (pakSlot == s_lastSlot && assetGuid == s_lastAsset)
					return;
				s_lastSlot = pakSlot;
				s_lastAsset = assetGuid;
				Warning(eDLL_T::RTECH, "[CONSISTENCY-OBS] pak '%s' asset 0x%016llX: skipped walk: %s (refs %u start %u descs %u pages %u)\n",
					pakName ? pakName : "?", assetGuid, reason, (unsigned)refCount, startIdx, guidDescCount, pageCount);
			};

			unsigned int walkCount = refCount;
			if (walkCount > 4096)
			{
				logSkip("refCount cap");
				walkCount = 4096;
			}

			const uint64_t rangeEnd = static_cast<uint64_t>(startIdx) + refCount;
			if (!pageCount || !guidDescCount || !pageArray
				|| rangeEnd > guidDescCount)
			{
				logSkip(!pageCount ? "pageCount"
					: !guidDescCount ? "guidDesc count"
					: !pageArray ? "page buffers"
					: "guidDesc range");
			}
			else
			{
				const uintptr_t gdBase = *reinterpret_cast<const uintptr_t*>(a1 + kS21Pak_PageDescriptorsOffset);
				if (!gdBase)
				{
					logSkip("guidDesc ptr");
				}
				else
				{
					static int s_walkedSlot = -1;
					if (pakSlot != s_walkedSlot)
					{
						s_walkedSlot = pakSlot;
						Warning(eDLL_T::RTECH, "[CONSISTENCY-OBS] walking refs for pak '%s' (first asset 0x%016llX, %u refs)\n",
							pakName ? pakName : "?", assetGuid, (unsigned)refCount);
					}
					const uintptr_t gdArray = gdBase + 8ull * startIdx;
					for (unsigned int i = 0; i < walkCount; ++i)
					{
						const uintptr_t entry = gdArray + 8ull * i;
						const unsigned int pageIdx = *reinterpret_cast<const unsigned int*>(entry);
						const unsigned int pageOff = *reinterpret_cast<const unsigned int*>(entry + 4);
						if (pageIdx >= pageCount)
						{
							logSkip("pageIdx");
							continue;
						}
						const uint32_t pageSize = Pak_S21GetPageDataSize(reinterpret_cast<const void*>(a1), pageIdx);
						if (static_cast<uint64_t>(pageOff) + 8ull > pageSize)
						{
							logSkip("pageOff");
							continue;
						}
						const uintptr_t pagePtr = *reinterpret_cast<const uintptr_t*>(pageArray + 8ull * pageIdx);
						if (!pagePtr) continue;
						const unsigned __int64 refGuid =
							*reinterpret_cast<const unsigned __int64*>(pagePtr + pageOff);
						// The engine also needs the tracked asset's live pointer: an on-demand
						// model sits in the hash with NULL data until its root pak loads.
						const bool inHash = ConsistencyObs_GuidResolves(hashBase, refGuid);
						if (!inHash || !ConsistencyObs_GuidLive(hashBase, refGuid))
						{
							Warning(eDLL_T::RTECH,
								"[CONSISTENCY-OBS] asset 0x%016llX references %s guid 0x%016llX (ref %u/%u)\n",
								assetGuid, inHash ? "NOT-LIVE" : "MISSING", refGuid, i, (unsigned)refCount);
							BridgeTrace_Log(
								"[CONSISTENCY-OBS] asset 0x%016llX references %s guid 0x%016llX (ref %u/%u)\n",
								assetGuid, inHash ? "NOT-LIVE" : "MISSING", refGuid, i, (unsigned)refCount);
							BridgeTrace_Flush();
						}
					}
				}
			}
		}
	}
	return v_PakConsistencyCheck_S21(a1, a2);
}

void VRPakObserveS21::Detour(const bool bAttach) const
{
	DetourSetup(&v_Pak_InitAsyncLoad_S21,
				&Hook_Pak_InitAsyncLoad_S21, bAttach);
	DetourSetup(&v_Pak_RequestLoadByName_S21,
				&Hook_Pak_RequestLoadByName_S21, bAttach);
	DetourSetup(&v_FatalErrorLogger_S21,
				&Hook_FatalErrorLogger_S21, bAttach);
	DetourSetup(&v_PakConsistencyCheck_S21,
				&Hook_PakConsistencyCheck_S21, bAttach);
	if (v_Pak_UnloadAsyncByHandle_S21)
		DetourSetup(&v_Pak_UnloadAsyncByHandle_S21,
					&Hook_Pak_UnloadAsyncByHandle_S21, bAttach);
	if (v_Pak_PreCache_UnloadAll_S21)
		DetourSetup(&v_Pak_PreCache_UnloadAll_S21,
					&Hook_Pak_PreCache_UnloadAll_S21, bAttach);
}

const char* Pak_StatusToString_S21(int status)
{
	switch (status)
	{
	case 0:  return "PAK_STATUS_FREED";
	case 1:  return "PAK_STATUS_LOAD_PENDING";
	case 2:  return "PAK_STATUS_LOAD_REPAK_RUNNING";
	case 3:  return "PAK_STATUS_LOAD_REPAK_DONE";
	case 4:  return "PAK_STATUS_LOAD_STARTING";
	case 5:  return "PAK_STATUS_LOAD_PAKHDR";
	case 6:  return "PAK_STATUS_LOAD_PATCH_INIT";
	case 7:  return "PAK_STATUS_LOAD_PATCH_EDIT_STREAM";
	case 8:  return "PAK_STATUS_LOAD_ASSETS";
	case 9:  return "PAK_STATUS_LOAD_DO_HOTSWAP";
	case 10: return "PAK_STATUS_LOADED";
	case 11: return "PAK_STATUS_UNLOAD_PENDING";
	case 12: return "PAK_STATUS_FREE_PENDING";
	case 13: return "PAK_STATUS_CANCELING";
	case 14: return "PAK_STATUS_ERROR";
	case 15: return "PAK_STATUS_INVALID_PAKHANDLE";
	default: return "PAK_STATUS_UNKNOWN";
	}
}

uintptr_t Pak_GetSlotBase_S21()
{
	return S21Pak_SlotBase();
}

bool Pak_IsSlotNameLive_S21(const char* pszName)
{
	if (!pszName || !pszName[0])
		return false;
	const uintptr_t slotBase = S21Pak_SlotBase();
	if (!slotBase || !EnsurePakSlotTableOk())
		return true;
	for (size_t slot = 0; slot < kS21_PakSlotCount; ++slot)
	{
		const uintptr_t entry = slotBase + slot * kS21_PakSlotStride;
		bool bMatch = false;
		__try
		{
			if (*reinterpret_cast<const int*>(entry + kS21_PakSlot_Status) != 0)
			{
				const char* const pszSlotName = *reinterpret_cast<const char* const*>(entry + kS21_PakSlot_Name);
				bMatch = (pszSlotName && _stricmp(pszSlotName, pszName) == 0);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return true;
		}
		if (bMatch)
			return true;
	}
	return false;
}

// Pattern pointer first, then RVA. Silent on success.
int Pak_RequestLoadByName_S21Resolve()
{
	if (v_Pak_RequestLoadByName_S21) return 1;
	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe) return 0;
	v_Pak_RequestLoadByName_S21 =
		reinterpret_cast<decltype(v_Pak_RequestLoadByName_S21)>(
			NetObs_Sym(NetObsSym_t::PakRequestLoadByName));
	return v_Pak_RequestLoadByName_S21 ? 1 : 0;
}

int Pak_UnloadAsyncByHandle_S21Resolve()
{
	if (v_Pak_UnloadAsyncByHandle_S21) return 1;
	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe) return 0;
	v_Pak_UnloadAsyncByHandle_S21 = reinterpret_cast<void(__fastcall*)(unsigned int, int)>(
		NetObs_Sym(NetObsSym_t::PakUnloadAsyncByHandle));
	return v_Pak_UnloadAsyncByHandle_S21 ? 1 : 0;
}

int ClientPakFile_RequestAsyncLoad_S21Resolve()
{
	if (v_ClientPakFile_RequestAsyncLoad_S21) return 1;
	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe) return 0;
	v_ClientPakFile_RequestAsyncLoad_S21 = reinterpret_cast<int(__fastcall*)(const char*, unsigned int)>(
		NetObs_Sym(NetObsSym_t::ClientPakFileRequestAsyncLoad));
	return v_ClientPakFile_RequestAsyncLoad_S21 ? 1 : 0;
}

int ClientPakFile_Unload_S21Resolve()
{
	if (v_ClientPakFile_Unload_S21) return 1;
	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe) return 0;
	v_ClientPakFile_Unload_S21 = reinterpret_cast<int(__fastcall*)(unsigned int)>(
		NetObs_Sym(NetObsSym_t::ClientPakFileUnload));
	return v_ClientPakFile_Unload_S21 ? 1 : 0;
}

uintptr_t Pak_GetGlobalAllocatorSlot_S21()
{
	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe) return 0;
	// Engine passes the VALUE at the allocator slot, not the slot address.
	const uintptr_t slotAddr = NetObs_Sym(NetObsSym_t::PakGlobalAllocatorSlot);
	return *reinterpret_cast<uintptr_t*>(slotAddr);
}
