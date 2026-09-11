//=============================================================================//
//
// Purpose: Load localization from disk language tables; skip localization_*.rpak.
//
//=============================================================================//

#include "core/stdafx.h"
#include "localize_disk.h"
#include "common/global.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/cmd.h"
#include "engine/cmd.h"
#include "filesystem/filesystem.h"
#include "localize/localize.h"
#include "pluginsystem/modsystem.h"
#include "tier1/keyvalues.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>

//-----------------------------------------------------------------------------
// Engine LocalizationAsset / LocalizationLookup_s (S21, size 0x38 / 0x10).
//-----------------------------------------------------------------------------
#pragma pack(push, 1)
struct LocalizationLookup_s21
{
	uint64_t hash64;
	uint32_t textOffset; // wchar index into textBuffer
	int32_t  nextEntry;  // -1 end of chain
};
#pragma pack(pop)
static_assert(sizeof(LocalizationLookup_s21) == 16, "LocalizationLookup size");

struct LocalizationAsset_s21
{
	const char* name;              // +0x00
	uint32_t    numEntries;        // +0x08
	uint32_t    hashTableLength;   // +0x0C power of 2
	uint32_t    lookupSize;        // +0x10
	uint32_t    pad;               // +0x14
	LocalizationLookup_s21* lookup;// +0x18
	wchar_t*    textBuffer;        // +0x20
	char*       keyBuffer;         // +0x28
	uint32_t*   keyOffset;         // +0x30
};
static_assert(sizeof(LocalizationAsset_s21) == 56, "LocalizationAsset size");

static constexpr int kLocPakSlots = 14;
static constexpr int kLocAssetMax = 32;

//-----------------------------------------------------------------------------
static ConVar localize_disk(
	"localize_disk", "1", FCVAR_RELEASE,
	"When 1, load localization from platform/localization/localization_<lang>.txt "
	"(legacy .locl also accepted) and skip localization_*.rpak. When 0, rpak only.");

static ConVar localize_disk_strict(
	"localize_disk_strict", "0", FCVAR_RELEASE,
	"When 1 with localize_disk 1, do not fall back to rpak if the language .txt/.locl is missing.");

// Hot language override. DetectLanguage caches once at boot; own a local
// command + buffer (string ConVar SetValue AVs on empty-default path).
static char s_langOverride[64] = {};
static bool s_langUiSyncGuard = false;

static bool SetLanguageOverride(const char* lang);
static void ReloadDiskLanguageNow(void);
static const char* ResolveLanguage(void);
// Settings HUD switch (DialogListButton). Values are g_LanguageNames indices.
// Settings UI shows a 10-slot subset only; -1 still means system DetectLanguage
// for console/cfg. ARCHIVE so the choice survives restart.
static void BridgeUILanguage_Changed(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t pUserData);

static ConVar bridge_ui_language(
	"bridge_ui_language", "0", FCVAR_ARCHIVE | FCVAR_RELEASE,
	"UI text language: -1=system DetectLanguage, else g_LanguageNames index "
	"(0=english .. 12=polish). Settings picker is a 10-lang subset.",
	true, -1.f, true, float(SDK_ARRAYSIZE(g_LanguageNames) - 1),
	BridgeUILanguage_Changed);

// uiscript_reset after a language swap is off by default; table swap alone is safe.
static ConVar localize_ui_reset(
	"localize_ui_reset", "0", FCVAR_RELEASE,
	"After hot language reload, queue uiscript_reset (0=off safe, 1=rebind UI).");

// Poison retired text buffers with 0xFE. Localize() returns interior
// pointers into textBuffer -- never free them on hot reload.
static ConVar localize_retire_poison(
	"localize_retire_poison", "1", FCVAR_RELEASE,
	"Poison retired loc text buffers (0xFE) instead of free on hot reload.");

static std::mutex s_locDiskMtx;
static LocalizationAsset_s21* s_diskAsset = nullptr;
static bool s_diskActive = false;
static unsigned s_locReloadGeneration = 0;
static char s_loadedLang[64] = {};
static bool s_reloadBusy = false;

// Owned buffers for the CURRENT disk asset.
static char* s_diskName = nullptr;
static LocalizationLookup_s21* s_diskLookup = nullptr;
static wchar_t* s_diskText = nullptr;
static size_t s_diskTextWchars = 0; // includes all packed strings + nulls
static char* s_diskKeys = nullptr;
static uint32_t* s_diskKeyOff = nullptr;

// Never free active disk text: Localize() returns interior pointers;
// free-while-held UAF on language switch. Retire/poison instead.
struct RetiredLocPack
{
	char* name;
	LocalizationLookup_s21* lookup;
	wchar_t* text;
	size_t textBytes;
	char* keys;
	uint32_t* keyOff;
	LocalizationAsset_s21* asset;
};
static std::vector<RetiredLocPack> s_retired;
static constexpr size_t kRetiredSoftCap = 16;

//-----------------------------------------------------------------------------
static unsigned NextPow2(unsigned n)
{
	if (n < 16)
		return 16;
	--n;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	return n + 1;
}

static void UnlinkDiskAssetFromEngineUnlocked(void)
{
	if (!g_pLocAssetCount_S21 || !g_ppLocAssets_S21 || !s_diskAsset)
		return;

	const unsigned count = *g_pLocAssetCount_S21;
	for (unsigned i = 0; i < count && i < (unsigned)kLocAssetMax; ++i)
	{
		if (g_ppLocAssets_S21[i] != s_diskAsset)
			continue;
		for (unsigned j = i; j + 1 < count && j + 1 < (unsigned)kLocAssetMax; ++j)
			g_ppLocAssets_S21[j] = g_ppLocAssets_S21[j + 1];
		if (count > 0)
		{
			g_ppLocAssets_S21[count - 1] = nullptr;
			*g_pLocAssetCount_S21 = count - 1;
		}
		break;
	}
}

static void ReallyFreePack(RetiredLocPack& p)
{
	free(p.name);
	free(p.lookup);
	free(p.text);
	free(p.keys);
	free(p.keyOff);
	free(p.asset);
	memset(&p, 0, sizeof(p));
}

// Move current disk asset out of engine view but KEEP allocations alive so
// any UI still holding Localize() pointers does not UAF.
static void RetireDiskAssetUnlocked(const char* reason)
{
	if (!s_diskAsset && !s_diskText)
	{
		s_diskActive = false;
		return;
	}

	UnlinkDiskAssetFromEngineUnlocked();

	RetiredLocPack pack = {};
	pack.name = s_diskName;
	pack.lookup = s_diskLookup;
	pack.text = s_diskText;
	pack.textBytes = s_diskTextWchars * sizeof(wchar_t);
	pack.keys = s_diskKeys;
	pack.keyOff = s_diskKeyOff;
	pack.asset = s_diskAsset;

	if (pack.text && s_diskTextWchars > 0 && localize_retire_poison.GetBool())
	{
		// U+FEFE fill: use-after-retire is obvious in a memory window / string view.
		wmemset(pack.text, static_cast<wchar_t>(0xFEFE), s_diskTextWchars);
		Msg(eDLL_T::ENGINE,
			"[LOC-DISK] RETIRE+POISON text=%p wchars=%zu asset=%p reason=%s retired=%zu\n",
			pack.text, s_diskTextWchars, pack.asset, reason ? reason : "?",
			s_retired.size() + 1);
	}
	else
	{
		Msg(eDLL_T::ENGINE,
			"[LOC-DISK] RETIRE (no free) text=%p wchars=%zu asset=%p reason=%s retired=%zu\n",
			pack.text, s_diskTextWchars, pack.asset, reason ? reason : "?",
			s_retired.size() + 1);
	}

	s_retired.push_back(pack);

	s_diskName = nullptr;
	s_diskLookup = nullptr;
	s_diskText = nullptr;
	s_diskTextWchars = 0;
	s_diskKeys = nullptr;
	s_diskKeyOff = nullptr;
	s_diskAsset = nullptr;
	s_diskActive = false;

	// Soft cap: only free the OLDEST retired packs once far past likely UI hold.
	// Never free the most recent kRetiredSoftCap/2 -- those may still be on-screen.
	while (s_retired.size() > kRetiredSoftCap)
	{
		ReallyFreePack(s_retired.front());
		s_retired.erase(s_retired.begin());
		Msg(eDLL_T::ENGINE, "[LOC-DISK] freed oldest retired pack (cap=%zu)\n", kRetiredSoftCap);
	}
}

static void InvalidateAllLocPaks(void)
{
	if (!g_pLocalizationPaks_S21)
		return;
	for (int i = 0; i < kLocPakSlots; ++i)
		g_pLocalizationPaks_S21[i] = -1;
}

//-----------------------------------------------------------------------------
// Read entire file into a byte buffer. Prefer IFileSystem GAME path, then
// absolute platform/ relative to the module directory.
//-----------------------------------------------------------------------------
static bool ReadLocFile(const char* relPath, std::vector<char>& out)
{
	out.clear();

	// 1) Engine filesystem (HEAD platform/ from VPlatformFSOverrideS21).
	CFileSystem_Stdio* fs = g_pFullFileSystem ? *g_pFullFileSystem : g_pFileSystem_Stdio;
	if (fs)
	{
		FileHandle_t fh = fs->Open(relPath, "rb", "GAME");
		if (fh != FILESYSTEM_INVALID_HANDLE)
		{
			const ssize_t sz = fs->Size(fh);
			if (sz > 0 && sz < 64 * 1024 * 1024)
			{
				out.resize(static_cast<size_t>(sz) + 1);
				const ssize_t n = fs->Read(out.data(), sz, fh);
				fs->Close(fh);
				if (n > 0)
				{
					out.resize(static_cast<size_t>(n));
					out.push_back('\0');
					return true;
				}
			}
			else
			{
				fs->Close(fh);
			}
		}
	}

	// 2) Absolute: <exe_dir>\platform\<relPath>
	char modPath[MAX_PATH] = {};
	if (!GetModuleFileNameA(nullptr, modPath, MAX_PATH))
		return false;
	char* slash = strrchr(modPath, '\\');
	if (!slash)
		slash = strrchr(modPath, '/');
	if (slash)
		*slash = '\0';

	char full[MAX_PATH * 2] = {};
	_snprintf_s(full, _TRUNCATE, "%s\\platform\\%s", modPath, relPath);

	FILE* f = nullptr;
	if (fopen_s(&f, full, "rb") != 0 || !f)
		return false;
	if (fseek(f, 0, SEEK_END) != 0)
	{
		fclose(f);
		return false;
	}
	const long sz = ftell(f);
	if (sz <= 0 || sz > 64 * 1024 * 1024)
	{
		fclose(f);
		return false;
	}
	fseek(f, 0, SEEK_SET);
	out.resize(static_cast<size_t>(sz) + 1);
	const size_t n = fread(out.data(), 1, static_cast<size_t>(sz), f);
	fclose(f);
	if (n == 0)
		return false;
	out.resize(n);
	out.push_back('\0');
	return true;
}

//-----------------------------------------------------------------------------
// Unescape RSX-style string body (\n \r \t \" \\ \xHH).
//-----------------------------------------------------------------------------
static std::string UnescapeLocValue(const char* s, size_t len)
{
	std::string out;
	out.reserve(len);
	for (size_t i = 0; i < len; ++i)
	{
		if (s[i] != '\\' || i + 1 >= len)
		{
			out.push_back(s[i]);
			continue;
		}
		const char c = s[++i];
		switch (c)
		{
		case 'n': out.push_back('\n'); break;
		case 'r': out.push_back('\r'); break;
		case 't': out.push_back('\t'); break;
		case '"': out.push_back('"'); break;
		case '\\': out.push_back('\\'); break;
		case 'x':
			if (i + 2 < len)
			{
				char hex[3] = { s[i + 1], s[i + 2], 0 };
				out.push_back(static_cast<char>(strtoul(hex, nullptr, 16)));
				i += 2;
			}
			break;
		default:
			out.push_back(c);
			break;
		}
	}
	return out;
}

struct LocEntry
{
	uint64_t hash;
	std::string valueUtf8;
	std::string keyName; // hex or human name for keyBuffer / debug
};

// Same HashName FindIndex uses (aligned/unaligned). Returns 0 if unresolved.
static uint64_t LocHashName64(const char* name)
{
	if (!name || !*name)
		return 0;
	if (!v_LocHashAligned_S21 || !v_LocHashUnaligned_S21)
		return 0;
	const bool aligned = (reinterpret_cast<uintptr_t>(name) & 3) == 0;
	return aligned ? v_LocHashAligned_S21(name) : v_LocHashUnaligned_S21(name);
}

static bool IsHexHashKey(const std::string& key)
{
	if (key.empty() || key.size() > 16)
		return false;
	// Pure hex >= 10 digits treated as RSX hash key (typical keys are 12-16).
	if (key.size() < 10)
		return false;
	for (char c : key)
	{
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// Parse language.txt (RSX dump + human edits + // # /* */ comments).
// Hex key = HashName64; else human token (strip leading #). Same hash last-wins.
//-----------------------------------------------------------------------------
static bool ParseLoclText(const char* text, std::vector<LocEntry>& entries, std::string& assetName,
	int* outHex = nullptr, int* outHuman = nullptr)
{
	entries.clear();
	assetName.clear();
	if (outHex) *outHex = 0;
	if (outHuman) *outHuman = 0;
	if (!text)
		return false;

	const char* p = text;
	// Skip UTF-8 BOM.
	if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
		p += 3;

	// Skip whitespace and comments (// # /* */). '#' only starts a comment
	// outside quotes -- quoted "#TOKEN" remains a localization key.
	auto skipNoise = [&]() {
		for (;;)
		{
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
				++p;
			if (p[0] == '/' && p[1] == '/')
			{
				p += 2;
				while (*p && *p != '\n')
					++p;
				continue;
			}
			if (*p == '#')
			{
				++p;
				while (*p && *p != '\n')
					++p;
				continue;
			}
			if (p[0] == '/' && p[1] == '*')
			{
				p += 2;
				while (*p && !(p[0] == '*' && p[1] == '/'))
					++p;
				if (*p)
					p += 2; // */
				continue;
			}
			break;
		}
	};
	auto parseQuoted = [&](std::string& out) -> bool {
		skipNoise();
		if (*p != '"')
			return false;
		++p;
		const char* start = p;
		while (*p && *p != '"')
		{
			if (*p == '\\' && p[1])
				p += 2;
			else
				++p;
		}
		if (*p != '"')
			return false;
		out.assign(start, p - start);
		++p;
		return true;
	};

	// Asset name line.
	std::string nameRaw;
	if (!parseQuoted(nameRaw))
		return false;
	assetName = UnescapeLocValue(nameRaw.c_str(), nameRaw.size());

	skipNoise();
	if (*p != '{')
		return false;
	++p;

	// Last-wins map so appending a human override after a hex line replaces it.
	std::unordered_map<uint64_t, LocEntry> byHash;
	byHash.reserve(32000);
	int nHex = 0, nHuman = 0;

	while (*p)
	{
		skipNoise();
		if (*p == '}')
		{
			++p;
			break;
		}
		if (!*p)
			break;

		std::string keyRaw, valRaw;
		if (!parseQuoted(keyRaw))
			return false;
		if (!parseQuoted(valRaw))
			return false;
		// Allow trailing // or # comment after the pair.
		skipNoise();

		std::string key = UnescapeLocValue(keyRaw.c_str(), keyRaw.size());
		const std::string val = UnescapeLocValue(valRaw.c_str(), valRaw.size());
		if (key.empty())
			continue;

		LocEntry e;
		e.valueUtf8 = val;

		if (IsHexHashKey(key))
		{
			e.hash = strtoull(key.c_str(), nullptr, 16);
			e.keyName = key;
			++nHex;
		}
		else
		{
			// Human token: strip leading # (Find("#X") hashes "X").
			if (key[0] == '#')
				key.erase(0, 1);
			if (key.empty())
				continue;
			e.hash = LocHashName64(key.c_str());
			if (e.hash == 0)
			{
				// HashName unresolved: skip human keys (hex still work).
				static int s_hashWarn = 0;
				if (s_hashWarn++ < 8)
					Warning(eDLL_T::ENGINE,
						"[LOC-DISK] HashName unresolved; cannot add human key '%s'\n",
						key.c_str());
				continue;
			}
			e.keyName = key;
			++nHuman;
		}

		if (e.hash == 0)
			continue;
		byHash[e.hash] = std::move(e);
	}

	entries.reserve(byHash.size());
	for (auto& kv : byHash)
		entries.push_back(std::move(kv.second));

	if (outHex) *outHex = nHex;
	if (outHuman) *outHuman = nHuman;
	return !entries.empty();
}

//-----------------------------------------------------------------------------
// Build LocalizationAsset from parsed entries and install into sLocAssets.
//-----------------------------------------------------------------------------
static bool BuildAndInstallAsset(const std::string& assetName, std::vector<LocEntry>& entries)
{
	if (!g_pLocAssetCount_S21 || !g_ppLocAssets_S21)
		return false;
	if (entries.empty())
		return false;

	const unsigned n = static_cast<unsigned>(entries.size());
	// Load factor ~0.5; engine wants lookupSize > hashTableLength.
	const unsigned hashTableLength = NextPow2(n * 2);
	const unsigned lookupSize = hashTableLength + n; // primary + overflow room

	// Pack UTF-16 text + key strings.
	size_t totalWchars = 0;
	size_t totalKeyBytes = 0;
	for (const LocEntry& e : entries)
	{
		// MultiByteToWideChar length.
		const int wlen = MultiByteToWideChar(CP_UTF8, 0, e.valueUtf8.c_str(), -1, nullptr, 0);
		if (wlen <= 0)
			return false;
		totalWchars += static_cast<size_t>(wlen); // includes null
		totalKeyBytes += e.keyName.size() + 1;
	}

	auto* lookup = static_cast<LocalizationLookup_s21*>(
		calloc(lookupSize, sizeof(LocalizationLookup_s21)));
	auto* text = static_cast<wchar_t*>(calloc(totalWchars, sizeof(wchar_t)));
	auto* keys = static_cast<char*>(malloc(totalKeyBytes ? totalKeyBytes : 1));
	auto* keyOff = static_cast<uint32_t*>(calloc(lookupSize, sizeof(uint32_t)));
	auto* asset = static_cast<LocalizationAsset_s21*>(calloc(1, sizeof(LocalizationAsset_s21)));
	char* nameCopy = _strdup(assetName.c_str());

	if (!lookup || !text || !keys || !keyOff || !asset || !nameCopy)
	{
		free(lookup); free(text); free(keys); free(keyOff); free(asset); free(nameCopy);
		return false;
	}

	// Init empty slots.
	for (unsigned i = 0; i < lookupSize; ++i)
	{
		lookup[i].hash64 = 0;
		lookup[i].textOffset = 0;
		lookup[i].nextEntry = -1;
		keyOff[i] = 0;
	}

	unsigned freeSlot = hashTableLength; // overflow starts after primary buckets
	size_t textCursor = 0;
	size_t keyCursor = 0;
	unsigned inserted = 0;

	for (const LocEntry& e : entries)
	{
		const int wlen = MultiByteToWideChar(
			CP_UTF8, 0, e.valueUtf8.c_str(), -1,
			text + textCursor, static_cast<int>(totalWchars - textCursor));
		if (wlen <= 0)
			continue;

		const uint32_t textOffset = static_cast<uint32_t>(textCursor);
		textCursor += static_cast<size_t>(wlen);

		const uint32_t thisKeyOff = static_cast<uint32_t>(keyCursor);
		memcpy(keys + keyCursor, e.keyName.c_str(), e.keyName.size() + 1);
		keyCursor += e.keyName.size() + 1;

		const unsigned bucket = static_cast<unsigned>(e.hash & (hashTableLength - 1));
		unsigned slot;
		if (lookup[bucket].hash64 == 0)
		{
			slot = bucket;
		}
		else
		{
			// Chain: walk to end, allocate overflow slot.
			unsigned cur = bucket;
			int safety = 0;
			while (lookup[cur].nextEntry != -1 && safety++ < 100000)
				cur = static_cast<unsigned>(lookup[cur].nextEntry);
			if (freeSlot >= lookupSize)
			{
				// Should not happen with lookupSize = hashTableLength + n.
				Warning(eDLL_T::ENGINE, "[LOC-DISK] lookup overflow, truncating at %u\n", inserted);
				break;
			}
			lookup[cur].nextEntry = static_cast<int32_t>(freeSlot);
			slot = freeSlot++;
		}

		lookup[slot].hash64 = e.hash;
		lookup[slot].textOffset = textOffset;
		lookup[slot].nextEntry = -1;
		keyOff[slot] = thisKeyOff;
		++inserted;
	}

	asset->name = nameCopy;
	asset->numEntries = inserted;
	asset->hashTableLength = hashTableLength;
	asset->lookupSize = lookupSize;
	asset->pad = 0;
	asset->lookup = lookup;
	asset->textBuffer = text;
	asset->keyBuffer = keys;
	asset->keyOffset = keyOff;

	// Retire previous ownership (do NOT free -- UI may still hold Localize ptrs).
	RetireDiskAssetUnlocked("install_replace");

	s_diskName = nameCopy;
	s_diskLookup = lookup;
	s_diskText = text;
	s_diskTextWchars = totalWchars;
	s_diskKeys = keys;
	s_diskKeyOff = keyOff;
	s_diskAsset = asset;

	// Install as sole asset (full replace). Clear any previous rpak-linked slots
	// so FindIndex only walks our table.
	const unsigned oldCount = *g_pLocAssetCount_S21;
	for (unsigned i = 0; i < oldCount && i < (unsigned)kLocAssetMax; ++i)
		g_ppLocAssets_S21[i] = nullptr;
	g_ppLocAssets_S21[0] = asset;
	*g_pLocAssetCount_S21 = 1;

	s_diskActive = true;
	InvalidateAllLocPaks();

	Msg(eDLL_T::ENGINE,
		"[LOC-DISK] installed asset '%s' entries=%u hashTable=%u lookupSize=%u "
		"(sLocAssets=1, paks invalidated)\n",
		assetName.c_str(), inserted, hashTableLength, lookupSize);
	return true;
}

#if defined(CLIENT_DLL)
static bool ModLoc_StoredPathOk(const CUtlString& stored, const CUtlString& base)
{
	char path[MAX_PATH];
	char prefix[MAX_PATH];
	if (stored.Length() >= MAX_PATH || base.Length() >= MAX_PATH || stored.Length() <= 0)
		return false;

	V_strncpy(path, stored.String(), sizeof(path));
	V_strncpy(prefix, base.String(), sizeof(prefix));
	for (char* q = path; *q; ++q)
	{
		if (*q == '\\')
			*q = '/';
	}
	for (char* q = prefix; *q; ++q)
	{
		if (*q == '\\')
			*q = '/';
	}
	if (!ModSystem_IsSafeRelativePath(path))
		return false;

	const size_t nPrefix = strlen(prefix);
	if (nPrefix == 0)
		return false;
	return V_strnicmp(path, prefix, static_cast<int>(nPrefix)) == 0;
}

static void Loc_MergeLastWins(std::vector<LocEntry>& dest, std::vector<LocEntry>& src)
{
	std::unordered_map<uint64_t, size_t> idx;
	idx.reserve(dest.size() + src.size());
	for (size_t i = 0; i < dest.size(); ++i)
		idx[dest[i].hash] = i;

	for (size_t i = 0; i < src.size(); ++i)
	{
		LocEntry& e = src[i];
		if (e.hash == 0)
			continue;
		const auto it = idx.find(e.hash);
		if (it != idx.end())
			dest[it->second] = std::move(e);
		else
		{
			idx[e.hash] = dest.size();
			dest.push_back(std::move(e));
		}
	}
}

static bool ParseSourceTokensLoc(const char* text, std::vector<LocEntry>& entries,
	int* nHex, int* nHuman)
{
	entries.clear();
	if (nHex)
		*nHex = 0;
	if (nHuman)
		*nHuman = 0;
	if (!text)
		return false;
	if (!FileSystem())
		return false;

	KeyValues root("lang");
	if (!root.LoadFromBuffer("mod_loc", text, FileSystem(), nullptr))
		return false;

	KeyValues* tokens = root.FindKey("Tokens");
	if (!tokens)
	{
		KeyValues* const lang = root.FindKey("lang");
		if (lang)
			tokens = lang->FindKey("Tokens");
	}
	if (!tokens)
		return false;

	std::unordered_map<uint64_t, LocEntry> byHash;
	int hx = 0;
	int hu = 0;
	int nSafety = 0;
	for (KeyValues* pTok = tokens->GetFirstSubKey(); pTok; pTok = pTok->GetNextKey())
	{
		if (++nSafety > 100000)
			break;

		std::string key = pTok->GetName();
		const char* const val = pTok->GetString();
		if (key.empty() || !val)
			continue;

		LocEntry e;
		e.valueUtf8 = val;
		if (IsHexHashKey(key))
		{
			e.hash = strtoull(key.c_str(), nullptr, 16);
			e.keyName = key;
			++hx;
		}
		else
		{
			if (key[0] == '#')
				key.erase(0, 1);
			if (key.empty())
				continue;
			e.hash = LocHashName64(key.c_str());
			if (e.hash == 0)
				continue;
			e.keyName = key;
			++hu;
		}
		if (e.hash == 0)
			continue;
		byHash[e.hash] = std::move(e);
	}

	entries.reserve(byHash.size());
	for (auto& kv : byHash)
		entries.push_back(std::move(kv.second));
	if (nHex)
		*nHex = hx;
	if (nHuman)
		*nHuman = hu;
	return !entries.empty();
}

static void AppendModLocEntries(std::vector<LocEntry>& entries)
{
	if (!ModSystem()->IsEnabled())
		return;

	ModSystem()->LockModList();
	FOR_EACH_VEC(ModSystem()->GetModList(), i)
	{
		const CModSystem::ModInstance_t* const mod = ModSystem()->GetModList()[i];
		if (!mod || !mod->IsEnabled())
			continue;

		FOR_EACH_VEC(mod->localizationFiles, j)
		{
			const CUtlString& stored = mod->localizationFiles.Element(j);
			if (!ModLoc_StoredPathOk(stored, mod->GetBasePath()))
			{
				Warning(eDLL_T::ENGINE,
					"[MOD-LOC] skipped unsafe localization path from '%s': '%s'\n",
					mod->id.String(), stored.String());
				continue;
			}

			std::vector<char> buf;
			if (!ReadLocFile(stored.String(), buf))
			{
				Warning(eDLL_T::ENGINE,
					"[MOD-LOC] failed to read '%s' from '%s'\n",
					stored.String(), mod->id.String());
				continue;
			}

			std::vector<LocEntry> modEntries;
			std::string ignoredAsset;
			int nHex = 0;
			int nHuman = 0;
			if (!ParseLoclText(buf.data(), modEntries, ignoredAsset, &nHex, &nHuman)
				&& !ParseSourceTokensLoc(buf.data(), modEntries, &nHex, &nHuman))
			{
				Warning(eDLL_T::ENGINE,
					"[MOD-LOC] parse failed for '%s' ('%s')\n",
					stored.String(), mod->id.String());
				continue;
			}

			Loc_MergeLastWins(entries, modEntries);
			Msg(eDLL_T::ENGINE,
				"[MOD-LOC] merged '%s' from '%s' (hex=%d human=%d, total=%zu)\n",
				stored.String(), mod->id.String(), nHex, nHuman, entries.size());
		}
	}
	ModSystem()->UnlockModList();
}
#endif // CLIENT_DLL

//-----------------------------------------------------------------------------
static bool TryLoadLanguageFromDisk(const char* lang)
{
	if (!lang || !*lang)
		return false;

	// Prefer.txt; keep.locl as legacy fallback for existing trees.
	char relTxt[260] = {};
	char relLocl[260] = {};
	_snprintf_s(relTxt, _TRUNCATE, "localization/localization_%s.txt", lang);
	_snprintf_s(relLocl, _TRUNCATE, "localization/localization_%s.locl", lang);

	std::vector<char> buf;
	const char* rel = relTxt;
	if (!ReadLocFile(relTxt, buf))
	{
		if (!ReadLocFile(relLocl, buf))
		{
			Msg(eDLL_T::ENGINE,
				"[LOC-DISK] missing '%s' (and legacy .locl) under GAME + platform/\n",
				relTxt);
			return false;
		}
		rel = relLocl;
		Msg(eDLL_T::ENGINE,
			"[LOC-DISK] using legacy '%s' -- rename to .txt when convenient\n", relLocl);
	}

	std::vector<LocEntry> entries;
	std::string assetName;
	int nHex = 0, nHuman = 0;
	if (!ParseLoclText(buf.data(), entries, assetName, &nHex, &nHuman))
	{
		Warning(eDLL_T::ENGINE, "[LOC-DISK] parse failed for '%s'\n", rel);
		return false;
	}

	Msg(eDLL_T::ENGINE,
		"[LOC-DISK] parsed '%s' -> %zu unique entries (hex=%d human=%d asset='%s')\n",
		rel, entries.size(), nHex, nHuman, assetName.c_str());

#if defined(CLIENT_DLL)
	AppendModLocEntries(entries);
#endif // CLIENT_DLL

	return BuildAndInstallAsset(assetName, entries);
}

static void PrintSupportedLanguages(void)
{
	char buf[512];
	buf[0] = '\0';
	for (size_t i = 0; i < SDK_ARRAYSIZE(g_LanguageNames); ++i)
	{
		if (i)
			V_strncat(buf, " ", sizeof(buf));
		V_strncat(buf, g_LanguageNames[i], sizeof(buf));
	}
	Msg(eDLL_T::ENGINE, "[LOC-DISK] supported: %s\n", buf);
}

// Do not call bridge_ui_language.SetValue from language_f -- SetValue AVs.
// Console `language <name>` only owns s_langOverride.

static bool SetLanguageOverride(const char* lang)
{
	if (!lang || !lang[0])
	{
		s_langOverride[0] = '\0';
		return true;
	}

	if (!Localize_IsLanguageSupported(lang))
	{
		Warning(eDLL_T::ENGINE, "[LOC-DISK] unsupported language '%s'\n", lang);
		PrintSupportedLanguages();
		return false;
	}

	V_strncpy(s_langOverride, lang, sizeof(s_langOverride));
	return true;
}

static void BridgeUILanguage_Changed(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t pUserData)
{
	(void)var;
	(void)pOldValue;
	(void)flOldValue;
	(void)pUserData;

	// Script/console sync of the switch index must not re-enter reload.
	if (s_langUiSyncGuard)
		return;

	const int idx = bridge_ui_language.GetInt();
	if (idx < 0)
	{
		if (!s_langOverride[0])
			return;
		SetLanguageOverride(nullptr);
		Msg(eDLL_T::ENGINE, "[LOC-DISK] bridge_ui_language -> system ('%s')\n", ResolveLanguage());
		ReloadDiskLanguageNow();
		return;
	}

	if (idx >= static_cast<int>(SDK_ARRAYSIZE(g_LanguageNames)))
	{
		Warning(eDLL_T::ENGINE, "[LOC-DISK] bridge_ui_language index %d out of range\n", idx);
		return;
	}

	const char* name = g_LanguageNames[idx];
	if (s_langOverride[0] && V_stricmp(s_langOverride, name) == 0)
		return;

	if (!SetLanguageOverride(name))
		return;

	Msg(eDLL_T::ENGINE, "[LOC-DISK] bridge_ui_language -> '%s', reloading tables\n", name);
	ReloadDiskLanguageNow();
}

static const char* ResolveLanguage(void)
{
	if (s_langOverride[0])
		return s_langOverride;

	// Call through trampoline (DetectLanguage -- once-cached at boot).
	if (v_DetectLanguage_S21)
	{
		const char* lang = v_DetectLanguage_S21();
		if (lang && *lang)
			return lang;
	}
	return "english";
}

const char* Localize_GetCurrentLanguage(void)
{
	const char* lang = ResolveLanguage();
	return (lang && lang[0]) ? lang : "english";
}

static void QueueUiRelocalizeAfterLangChange(void)
{
	// Default OFF -- uiscript_reset mid-menu silent-closed (abort on main.menu).
	if (!localize_ui_reset.GetBool() || !Cbuf_AddText)
		return;
	if (s_locReloadGeneration == 0)
	{
		Msg(eDLL_T::ENGINE, "[LOC-DISK] skip uiscript_reset (first reload / boot)\n");
		return;
	}
	Msg(eDLL_T::ENGINE, "[LOC-DISK] queue uiscript_reset (localize_ui_reset 1)\n");
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "uiscript_reset", cmd_source_t::kCommandSrcCode);
}

// Reload disk tables for the current ResolveLanguage() without going through
// Cbuf (nested command dispatch) or string-ConVar SetValue.
static void ReloadDiskLanguageNow(void)
{
	if (!localize_disk.GetBool())
	{
		if (Cbuf_AddText)
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reload_localization", cmd_source_t::kCommandSrcCode);
		else
			Warning(eDLL_T::ENGINE, "[LOC-DISK] localize_disk 0 and Cbuf unresolved; cannot reload\n");
		return;
	}

	if (s_reloadBusy)
	{
		Msg(eDLL_T::ENGINE, "[LOC-DISK] reload busy, coalescing to lang='%s'\n",
			ResolveLanguage());
		return;
	}

	s_reloadBusy = true;
	bool loaded = false;
	const char* lang = nullptr;
	{
		std::lock_guard<std::mutex> lock(s_locDiskMtx);
		lang = ResolveLanguage();

		// Skip no-op reloads of the same already-installed language.
		if (s_diskActive && s_loadedLang[0] && lang && V_stricmp(s_loadedLang, lang) == 0)
		{
			Msg(eDLL_T::ENGINE, "[LOC-DISK] Reload skip (already '%s')\n", lang);
			s_reloadBusy = false;
			return;
		}

		Msg(eDLL_T::ENGINE, "[LOC-DISK] Reload lang='%s' (gen=%u retired=%zu)\n",
			lang ? lang : "(null)", s_locReloadGeneration, s_retired.size());

		// TryLoad -> BuildAndInstall retires the previous pack (no free).
		InvalidateAllLocPaks();

		if (TryLoadLanguageFromDisk(lang))
		{
			loaded = true;
			if (lang)
				V_strncpy(s_loadedLang, lang, sizeof(s_loadedLang));
		}
		else if (localize_disk_strict.GetBool())
		{
			Warning(eDLL_T::ENGINE, "[LOC-DISK] strict reload failed for lang='%s'\n",
				lang ? lang : "(null)");
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[LOC-DISK] disk reload failed for lang='%s' (run reload_localization for rpak path)\n",
				lang ? lang : "(null)");
		}
	}

	if (loaded)
	{
		QueueUiRelocalizeAfterLangChange();
		++s_locReloadGeneration;
	}
	s_reloadBusy = false;

	// If UI spun the language wheel during our load, apply the latest once.
	const char* now = ResolveLanguage();
	if (loaded && now && s_loadedLang[0] && V_stricmp(s_loadedLang, now) != 0)
	{
		Msg(eDLL_T::ENGINE, "[LOC-DISK] pending lang change during reload -> '%s'\n", now);
		ReloadDiskLanguageNow();
	}
}

static void language_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::ENGINE,
			"[LOC-DISK] language override='%s' resolved='%s' bridge_ui_language=%d "
			"(usage: language <name> | language clear)\n",
			s_langOverride[0] ? s_langOverride : "(none)",
			ResolveLanguage(),
			bridge_ui_language.GetInt());
		PrintSupportedLanguages();
		return;
	}

	const char* arg = args.Arg(1);
	if (V_stricmp(arg, "clear") == 0 || V_stricmp(arg, "default") == 0 || V_stricmp(arg, "auto") == 0)
	{
		SetLanguageOverride(nullptr);
		Msg(eDLL_T::ENGINE, "[LOC-DISK] language override cleared -> '%s'\n", ResolveLanguage());
	}
	else if (!SetLanguageOverride(arg))
	{
		return;
	}
	else
	{
		Msg(eDLL_T::ENGINE, "[LOC-DISK] language -> '%s', reloading tables\n", s_langOverride);
	}

	ReloadDiskLanguageNow();
}

// Local registration so the bridge console does not forward 'language' to the dedi.
static ConCommand language_cmd(
	"language", language_f,
	"Hot-switch UI text language and reload disk localization tables. "
	"Usage: language <english|spanish|...> | language clear",
	FCVAR_RELEASE);

//-----------------------------------------------------------------------------
// Hooks
//-----------------------------------------------------------------------------
static const char* __fastcall Hook_DetectLanguage_S21(void)
{
	if (s_langOverride[0])
		return s_langOverride;

	return v_DetectLanguage_S21 ? v_DetectLanguage_S21() : "english";
}

static bool __fastcall Hook_QueueLocalizationPak_S21(void* thisptr)
{
	if (!localize_disk.GetBool())
		return v_QueueLocalizationPak_S21 ? v_QueueLocalizationPak_S21(thisptr) : false;

	std::lock_guard<std::mutex> lock(s_locDiskMtx);
	const char* lang = ResolveLanguage();

	if (TryLoadLanguageFromDisk(lang))
		return true;

	if (localize_disk_strict.GetBool())
	{
		Warning(eDLL_T::ENGINE,
			"[LOC-DISK] strict: disk load failed for lang='%s', not falling back to rpak\n",
			lang ? lang : "(null)");
		InvalidateAllLocPaks();
		return false;
	}

	Msg(eDLL_T::ENGINE, "[LOC-DISK] falling back to native rpak for lang='%s'\n", lang ? lang : "(null)");
	return v_QueueLocalizationPak_S21 ? v_QueueLocalizationPak_S21(thisptr) : false;
}

static void __fastcall Hook_WaitForLocalizationPak_S21(void* thisptr)
{
	if (s_diskActive && localize_disk.GetBool())
	{
		InvalidateAllLocPaks();
		Msg(eDLL_T::ENGINE, "[LOC-DISK] Wait: disk asset active, skip pak wait\n");
		return;
	}

	if (v_WaitForLocalizationPak_S21)
		v_WaitForLocalizationPak_S21(thisptr);
}

static void __fastcall Hook_ReloadLocalizationFiles_S21(void* thisptr)
{
	if (!localize_disk.GetBool())
	{
		if (v_ReloadLocalizationFiles_S21)
			v_ReloadLocalizationFiles_S21(thisptr);
		return;
	}

	std::lock_guard<std::mutex> lock(s_locDiskMtx);
	const char* lang = ResolveLanguage();
	Msg(eDLL_T::ENGINE, "[LOC-DISK] engine ReloadLocalizationFiles lang='%s'\n",
		lang ? lang : "(null)");

	// BuildAndInstall retires the previous pack (no free / no UAF).
	InvalidateAllLocPaks();

	if (TryLoadLanguageFromDisk(lang))
	{
		if (lang)
			V_strncpy(s_loadedLang, lang, sizeof(s_loadedLang));
		++s_locReloadGeneration;
		return;
	}

	if (localize_disk_strict.GetBool())
	{
		Warning(eDLL_T::ENGINE, "[LOC-DISK] strict reload failed for lang='%s'\n",
			lang ? lang : "(null)");
		return;
	}

	if (v_ReloadLocalizationFiles_S21)
		v_ReloadLocalizationFiles_S21(thisptr);
}

//-----------------------------------------------------------------------------
void VLocalizeDiskS21::GetFun(void) const
{
	// QueueLocalizationPak -- unique: push rbx; sub rsp,180h; lea rcx, critsec
	Module_FindPattern(g_GameDll,
		"40 53 48 81 EC ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? FF 15")
		.GetPtr(v_QueueLocalizationPak_S21);

	// WaitForLocalizationPakToLoad 
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ?? 33 ED")
		.GetPtr(v_WaitForLocalizationPak_S21);

	// ReloadLocalizationFiles 
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B F1 48 8D 1D")
		.GetPtr(v_ReloadLocalizationFiles_S21);

	// DetectLanguage 
	Module_FindPattern(g_GameDll,
		"40 53 48 81 EC ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 74")
		.GetPtr(v_DetectLanguage_S21);

	// rtech HashName aligned/unaligned (FindIndex uses these; same as weapon KV).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 45 33 C9")
		.GetPtr(v_LocHashAligned_S21);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 45 33 DB")
		.GetPtr(v_LocHashUnaligned_S21);

	if (!v_QueueLocalizationPak_S21)
		Warning(eDLL_T::ENGINE, "[LOC-DISK] QueueLocalizationPak pattern unresolved\n");
	if (!v_WaitForLocalizationPak_S21)
		Warning(eDLL_T::ENGINE, "[LOC-DISK] WaitForLocalizationPak pattern unresolved\n");
	if (!v_ReloadLocalizationFiles_S21)
		Warning(eDLL_T::ENGINE, "[LOC-DISK] ReloadLocalizationFiles pattern unresolved\n");
	if (!v_DetectLanguage_S21)
		Warning(eDLL_T::ENGINE, "[LOC-DISK] DetectLanguage pattern unresolved\n");
	if (!v_LocHashAligned_S21 || !v_LocHashUnaligned_S21)
		Warning(eDLL_T::ENGINE, "[LOC-DISK] HashName pattern(s) unresolved -- human keys disabled\n");
}

void VLocalizeDiskS21::GetVar(void) const
{
	// s_localizationPaks from Reload: lea rbx, [rip+s_localizationPaks] after mov rsi, rcx.
	// Pattern ends at the LEA; offset 0x1A into Reload to the 48 8D 1D.
	g_pLocalizationPaks_S21 = Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B F1 48 8D 1D")
		.Offset(0x1A)
		.ResolveRelativeAddressSelf(0x3, 0x7)
		.RCast<int*>();

	// sLocAssets.locAssets from GetValueByIndex: lea r8, [rip+].
	// Unique prologue of GetValueByIndex then interior lea at +0x41.
	g_ppLocAssets_S21 = Module_FindPattern(g_GameDll,
		"48 83 EC ?? 4C 8B C1 83 FA FF 75 07 33 C0 48 83 C4 28 C3")
		.Offset(0x41)
		.ResolveRelativeAddressSelf(0x3, 0x7)
		.RCast<void**>();

	// count is 8 bytes before the locAssets array (LocalizationAssetTracker_s).
	if (g_ppLocAssets_S21)
		g_pLocAssetCount_S21 = reinterpret_cast<unsigned int*>(
			reinterpret_cast<char*>(g_ppLocAssets_S21) - 8);

	if (!g_pLocalizationPaks_S21)
		Warning(eDLL_T::ENGINE, "[LOC-DISK] s_localizationPaks unresolved\n");
	if (!g_ppLocAssets_S21 || !g_pLocAssetCount_S21)
		Warning(eDLL_T::ENGINE, "[LOC-DISK] sLocAssets unresolved\n");
}

void VLocalizeDiskS21::Detour(const bool bAttach) const
{
	if (v_QueueLocalizationPak_S21)
		DetourSetup(&v_QueueLocalizationPak_S21, &Hook_QueueLocalizationPak_S21, bAttach);
	if (v_WaitForLocalizationPak_S21)
		DetourSetup(&v_WaitForLocalizationPak_S21, &Hook_WaitForLocalizationPak_S21, bAttach);
	if (v_ReloadLocalizationFiles_S21)
		DetourSetup(&v_ReloadLocalizationFiles_S21, &Hook_ReloadLocalizationFiles_S21, bAttach);
	if (v_DetectLanguage_S21)
		DetourSetup(&v_DetectLanguage_S21, &Hook_DetectLanguage_S21, bAttach);
}
