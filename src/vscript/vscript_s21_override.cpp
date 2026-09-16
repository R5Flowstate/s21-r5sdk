//=============================================================================//
// S21 script override + filesystem HEAD-priority override.
//=============================================================================//

#include "core/stdafx.h"
#include "core/sdk_stage.h"
#include "vscript_s21_override.h"
#include "common/global.h"
#include "tier0/dbg.h"
#include "tier0/memvalidate.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "filesystem/filesystem.h"
#include "game/client/scriptnetdata_client.h"
#include "game/client/vscript_client.h"
#include "game/client/classvar_natives.h"
#include "game/client/mantle_boost_rui.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/ivscript.h"
#include "pluginsystem/modsystem.h"
#include "game/shared/pluginsystem/modsystem.h"
#include "engine/host_state.h"
#include "engine/cmd.h"
#include <mutex>
#include <string>
#include <unordered_map>

extern void Cvar_ForceCommandLineDevModeForScripts();

static void ModSystem_EnsureClientInit(void);
static void ModSystem_TryLoadModConfigs(const bool bFinal);
static char __fastcall Hook_RSON_LoadFileFromPath_S21(
	void* out, const char* path, __int64 a3, __int64 a4, void* alloc, __int64* a6);

static bool IsDx12Executable_S21()
{
	char path[MAX_PATH] = {};
	const DWORD len = GetModuleFileNameA(NULL, path, SDK_ARRAYSIZE(path));
	return len > 0 && len < SDK_ARRAYSIZE(path) &&
		V_stristr(path, "r5apex_dx12.exe") != nullptr;
}

extern void EnsureScriptDiskRedirect_S21(void);
extern void Script_RegisterCommandLineDevConstants_S21(CSquirrelVM* s, SQCONTEXT ctx);

// Late SNDC registration: engine bulk-register overwrites Init-time natives.
static volatile LONG s_lateRegClient = 0;
static volatile LONG s_lateRegUI     = 0;

void ResetLateNativeRegistration_S21(uint8_t vmType)
{
	switch (vmType)
	{
	case 1: InterlockedExchange(&s_lateRegClient, 0); break;
	case 2: InterlockedExchange(&s_lateRegUI, 0);     break;
	}
}

static void EnsureLateNativeRegistration(uint8_t vmType, CSquirrelVM* s)
{
	switch (vmType)
	{
	case 1: // CLIENT
		if (InterlockedCompareExchange(&s_lateRegClient, 1, 0) == 0)
		{
			CSquirrelVM* const vm = s ? s : g_pClientScript;
			if (vm)
			{
				ScriptNetData_RegisterClientFunctions(vm);
				Script_RegisterModSystemFunctions(vm);
				Script_RegisterChatMuteClient(vm);
				ClassVar_RegisterClientFunctions(vm);
				MantleBoostRui_RegisterClientFunctions(vm);
			}
		}
		break;
	case 2: // UI
		if (InterlockedCompareExchange(&s_lateRegUI, 1, 0) == 0)
		{
			CSquirrelVM* const vm = s ? s : g_pUIScript;
			if (vm)
			{
				ScriptNetData_RegisterUIFunctions(vm);
				// Bulk Script_RegisterUIFunctions never fires on S21, so the mod
				// natives the Mods list and DevMenu call must land here.
				Script_RegisterModSystemFunctions(vm);
				// SDK-only native: live netchannel ping (not EA datacenter).
				// Must late-reg; bulk Script_RegisterUIFunctions is not fired on S21.
				Script_RegisterConnectionPingUI(vm);
				Script_RegisterServerBrowserUI(vm);
				// Lets the menu withhold actions that cannot succeed until the
				// platform has issued the identity a connect will be verified on.
				Script_RegisterPlatformIdentityUI(vm);
			}
		}
		break;
	}
}

static __int64 __fastcall Hook_CScriptVM_PreCompileScriptFile(
	void* scriptvm, const char* path, const char* pszId, int isCompile)
{
	Cvar_ForceCommandLineDevModeForScripts();

	// Belt-and-suspenders: primary install is Hook_COM_InitFilesystem_S21.
	// No-ops if already installed; retries if COM-time FS was still null.
	EnsureScriptDiskRedirect_S21();
	ModSystem_EnsureClientInit();
	ModSystem_TryLoadModConfigs(true);

	if (!scriptvm || !path || !v_CScriptVM_PreCompileScriptFile_S21)
	{
		return v_CScriptVM_PreCompileScriptFile_S21
			? v_CScriptVM_PreCompileScriptFile_S21(scriptvm, path, pszId, isCompile)
			: 0;
	}

	CSquirrelVM* const s = reinterpret_cast<CSquirrelVM*>(scriptvm);
	const uint8_t vmType = *reinterpret_cast<uint8_t*>(
		reinterpret_cast<uintptr_t>(scriptvm) + 96);
	if (vmType <= 2)
		Script_RegisterCommandLineDevConstants_S21(
			s, static_cast<SQCONTEXT>(vmType));

	// Late registration: after engine bulk pass, before script compile.
	// One-shot per VM context. Alliance + SNDC + UI-only natives land here.
	EnsureLateNativeRegistration(vmType, s);

	return v_CScriptVM_PreCompileScriptFile_S21(scriptvm, path, pszId, isCompile);
}

void VScriptS21Override::Detour(const bool bAttach) const
{
	DetourSetup(&v_CScriptVM_PreCompileScriptFile_S21,
				&Hook_CScriptVM_PreCompileScriptFile, bAttach);
}

FS_AsyncReadScript_fn v_FS_AsyncReadScript_S21 = nullptr;

// --- Loose menu-layout (.res/.menu/.lst) DISK-OVERRIDE machinery ----------
// Forward decl: true when `path` is a .res/.menu/.lst with a loose file at platform\<path>.
static bool FSRes_HasLooseOverride_S21(const char* path);
static bool ModPath_IsSafeRel(const char* path);

// Native CUtlBuffer EnsureCapacity/Put (same shape as slot-15).
typedef void    (__fastcall* Buf_EnsureCapacity_fn)(void* buf, __int64 cap);
typedef __int64 (__fastcall* Buf_Put_fn)(void* buf, const void* src, size_t size);
static Buf_EnsureCapacity_fn v_Buf_EnsureCapacity_S21 = nullptr;
static Buf_Put_fn            v_Buf_Put_S21            = nullptr;

// Packed .res/.menu/.lst ignore search paths; fill slot-15's CUtlBuffer from disk.
static bool FSRes_FillBufferFromMemory_S21(void* buf, const char* data, const size_t size)
{
	if (!buf || !data || !size || !v_Buf_EnsureCapacity_S21 || !v_Buf_Put_S21)
		return false;

	bool ok = false;
	__try
	{
		v_Buf_EnsureCapacity_S21(buf, static_cast<__int64>(size) + 1);
		const __int64 wp = v_Buf_Put_S21(buf, data, size);
		if (wp)
		{
			reinterpret_cast<char*>(wp)[size] = 0;
			ok = true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	return ok;
}

static bool FSRes_FillBufferFromDiskPath_S21(const char* disk, void* buf)
{
	if (!disk || !buf || !v_Buf_EnsureCapacity_S21 || !v_Buf_Put_S21)
		return false;

	HANDLE h = CreateFileA(disk, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	bool ok = false;
	LARGE_INTEGER li;
	if (GetFileSizeEx(h, &li) && li.QuadPart > 0 && li.QuadPart < (64 * 1024 * 1024))
	{
		const DWORD size = static_cast<DWORD>(li.QuadPart);
		char* const tmp = static_cast<char*>(malloc(size));
		if (tmp)
		{
			DWORD readBytes = 0;
			if (ReadFile(h, tmp, size, &readBytes, nullptr) && readBytes == size)
				ok = FSRes_FillBufferFromMemory_S21(buf, tmp, size);
			free(tmp);
		}
	}
	CloseHandle(h);
	return ok;
}

static bool FSRes_FillBufferFromDisk_S21(const char* path, void* buf)
{
	if (!path || !buf || !ModPath_IsSafeRel(path))
		return false;

	char disk[1024];
	if (_snprintf_s(disk, sizeof(disk), _TRUNCATE, "platform\\%s", path) < 0)
		return false;

	return FSRes_FillBufferFromDiskPath_S21(disk, buf);
}

static bool FSRes_PlatformFileExists_S21(const char* path)
{
	if (!path || !ModPath_IsSafeRel(path))
		return false;

	char disk[1024];
	if (_snprintf_s(disk, sizeof(disk), _TRUNCATE, "platform\\%s", path) < 0)
		return false;

	const DWORD attr = GetFileAttributesA(disk);
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ModPath_IsSafeRel(const char* path)
{
	if (!path || !*path)
		return false;

	char tmp[MAX_PATH];
	const size_t n = strlen(path);
	if (n >= sizeof(tmp))
		return false;

	memcpy(tmp, path, n + 1);
	for (char* q = tmp; *q; ++q)
	{
		if (*q == '\\')
			*q = '/';
	}
	return ModSystem_IsSafeRelativePath(tmp);
}

static bool ModScript_IsCompileListPath(const char* path)
{
	if (!path || !*path)
		return false;

	const char* base = path;
	for (const char* p = path; *p; ++p)
	{
		if (*p == '/' || *p == '\\')
			base = p + 1;
	}

	return !_stricmp(base, "scripts.rson")
		|| !_stricmp(base, "scripts.rson.client")
		|| !_stricmp(base, "scripts.rson.ui")
		|| !_stricmp(base, "scripts.rson.server");
}

static bool FSRes_IsScriptSourcePath_S21(const char* path)
{
	if (!path)
		return false;

	const size_t len = strlen(path);
	if (len > 4 && !_stricmp(path + len - 4, ".nut"))
		return true;
	if (len > 5 && !_stricmp(path + len - 5, ".gnut"))
		return true;
	if (len > 5 && !_stricmp(path + len - 5, ".rson"))
		return true;
	return false;
}

static constexpr size_t kModScriptSpliceCap = 4u * 1024u * 1024u;

static bool ModScript_IsWhitespaceOnly(const char* p, size_t n)
{
	int nSafety = 0;
	for (size_t i = 0; i < n; ++i)
	{
		if (++nSafety > static_cast<int>(kModScriptSpliceCap))
			return false;
		const unsigned char c = static_cast<unsigned char>(p[i]);
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			return false;
	}
	return true;
}

static bool ModScript_AppendDiskFile(char* dst, size_t* pUsed, const size_t cap,
	const char* diskPath, const char* pszWho, size_t* pAdded)
{
	*pAdded = 0;
	if (!dst || !pUsed || !diskPath)
		return false;

	const char* const who = (pszWho && pszWho[0]) ? pszWho : "unknown";

	HANDLE h = CreateFileA(diskPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return true;

	LARGE_INTEGER li;
	if (!GetFileSizeEx(h, &li))
	{
		CloseHandle(h);
		return true;
	}
	if (li.QuadPart <= 0)
	{
		CloseHandle(h);
		if (V_stricmp(who, "platform") != 0)
		{
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-SCRIPT] '%s' compile list empty -- skipped\n", who);
		}
		return true;
	}
	if (li.QuadPart > static_cast<LONGLONG>(cap))
	{
		CloseHandle(h);
		Warning(eDLL_T::MODSYSTEM,
			"[MOD-SCRIPT] '%s' compile list exceeds 4 MiB cap\n", who);
		return false;
	}

	const size_t size = static_cast<size_t>(li.QuadPart);
	const size_t used0 = *pUsed;
	if (used0 > cap || size > cap - used0)
	{
		CloseHandle(h);
		Warning(eDLL_T::MODSYSTEM,
			"[MOD-SCRIPT] '%s' compile list would exceed 4 MiB splice cap\n", who);
		return false;
	}

	DWORD readBytes = 0;
	const BOOL ok = ReadFile(h, dst + used0, static_cast<DWORD>(size), &readBytes, nullptr);
	CloseHandle(h);
	if (!ok || readBytes != size)
	{
		Warning(eDLL_T::MODSYSTEM,
			"[MOD-SCRIPT] '%s' failed to read compile list -- skipped\n", who);
		return true;
	}

	char* body = dst + used0;
	size_t n = size;
	if (n >= 3
		&& static_cast<unsigned char>(body[0]) == 0xEF
		&& static_cast<unsigned char>(body[1]) == 0xBB
		&& static_cast<unsigned char>(body[2]) == 0xBF)
	{
		n -= 3;
		if (n)
			memmove(body, body + 3, n);
	}

	if (n == 0 || ModScript_IsWhitespaceOnly(body, n))
	{
		if (V_stricmp(who, "platform") != 0)
		{
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-SCRIPT] '%s' compile list empty -- skipped\n", who);
		}
		return true;
	}

	size_t insert = 0;
	if (used0 > 0 && dst[used0 - 1] != '\n')
		insert = 1;

	if (used0 + insert + n > cap)
	{
		Warning(eDLL_T::MODSYSTEM,
			"[MOD-SCRIPT] '%s' compile list would exceed 4 MiB splice cap\n", who);
		return false;
	}

	if (insert)
	{
		memmove(body + 1, body, n);
		body[0] = '\n';
	}

	*pUsed = used0 + insert + n;
	dst[*pUsed] = '\0';
	*pAdded = n;
	return true;
}

static constexpr size_t kModScriptNameCap = 128;

static bool ModScript_TokenLooksLikeScript(const char* tok, const size_t n)
{
	if (!tok || n < 4)
		return false;
	if (n >= 4 && _strnicmp(tok + n - 4, ".nut", 4) == 0)
		return true;
	if (n >= 5 && _strnicmp(tok + n - 5, ".gnut", 5) == 0)
		return true;
	if (n >= 5 && _strnicmp(tok + n - 5, ".rson", 5) == 0)
		return true;
	return false;
}

static bool ModScript_SplicedHasOverlongName(const char* buf, const size_t used)
{
	if (!buf)
		return false;

	const char* p = buf;
	const char* const end = buf + used;
	while (p < end)
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'
			|| *p == ',' || *p == '[' || *p == ']' || *p == '{' || *p == '}'))
			++p;
		if (p >= end)
			break;

		const char* tok = p;
		size_t n = 0;
		if (*p == '"' || *p == '\'')
		{
			const char q = *p++;
			tok = p;
			while (p < end && *p != q && *p != '\n')
				++p;
			n = static_cast<size_t>(p - tok);
			if (p < end && *p == q)
				++p;
		}
		else
		{
			while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'
				&& *p != ',' && *p != '[' && *p != ']' && *p != '{' && *p != '}')
				++p;
			n = static_cast<size_t>(p - tok);
		}

		if (ModScript_TokenLooksLikeScript(tok, n) && n > kModScriptNameCap)
			return true;
	}

	return false;
}

static char* ModScript_BuildSpliced(const char* requestPath, size_t* outSize)
{
	if (outSize)
		*outSize = 0;

	char* const buf = static_cast<char*>(malloc(kModScriptSpliceCap + 1));
	if (!buf)
		return nullptr;
	buf[0] = '\0';
	size_t used = 0;

	char platA[1024];
	if (requestPath
		&& _snprintf_s(platA, sizeof(platA), _TRUNCATE, "platform\\%s", requestPath) >= 0)
	{
		size_t added = 0;
		if (!ModScript_AppendDiskFile(buf, &used, kModScriptSpliceCap, platA, "platform", &added))
		{
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-SCRIPT] 'platform' splice exceeded 4 MiB cap; using unspliced compile list\n");
			free(buf);
			return nullptr;
		}
	}

	const bool sameUnsuffixed = (requestPath
		&& !_stricmp(requestPath, GAME_SCRIPT_COMPILELIST));
	if (!sameUnsuffixed)
	{
		char platB[1024];
		if (_snprintf_s(platB, sizeof(platB), _TRUNCATE, "platform\\%s", GAME_SCRIPT_COMPILELIST) >= 0)
		{
			size_t added = 0;
			if (!ModScript_AppendDiskFile(buf, &used, kModScriptSpliceCap, platB, "platform", &added))
			{
				Warning(eDLL_T::MODSYSTEM,
					"[MOD-SCRIPT] 'platform' splice exceeded 4 MiB cap; using unspliced compile list\n");
				free(buf);
				return nullptr;
			}
		}
	}

	int nSpliced = 0;
	if (ModSystem()->IsEnabled())
	{
		if (!ModPath_IsSafeRel(GAME_SCRIPT_COMPILELIST))
		{
			free(buf);
			return nullptr;
		}

		ModSystem()->LockModList();
		FOR_EACH_VEC(ModSystem()->GetResolvedModList(), i)
		{
			CModSystem::ModInstance_t* const mod = ModSystem()->GetResolvedModList()[i];
			if (!mod || !mod->IsEnabled())
				continue;

			const CUtlString compilePath = mod->GetScriptCompileListPath();
			size_t added = 0;
			if (!ModScript_AppendDiskFile(buf, &used, kModScriptSpliceCap,
				compilePath.String(), mod->id.String(), &added))
			{
				ModSystem()->UnlockModList();
				Warning(eDLL_T::MODSYSTEM,
					"[MOD-SCRIPT] '%s' splice exceeded 4 MiB cap; using unspliced compile list\n",
					mod->id.String());
				free(buf);
				return nullptr;
			}
			if (added > 0)
			{
				mod->hasPrecompiledScripts = true;
				Msg(eDLL_T::MODSYSTEM, "[MOD-SCRIPT] spliced '%s' (%zu bytes)\n",
					mod->id.String(), added);
				++nSpliced;
			}
		}
		ModSystem()->UnlockModList();
	}

	if (nSpliced == 0)
	{
		free(buf);
		return nullptr;
	}

	if (ModScript_SplicedHasOverlongName(buf, used))
	{
		Warning(eDLL_T::MODSYSTEM,
			"[MOD-SCRIPT] Scripts name longer than %zu -- using unspliced compile list\n",
			kModScriptNameCap);
		free(buf);
		return nullptr;
	}

	if (outSize)
		*outSize = used;
	return buf;
}

static volatile LONG s_spliceTmpSeq = 0;

static bool ModScript_WriteSplicedTemp(const char* requestPath, char* tmpPath, const size_t tmpCap)
{
	if (!tmpPath || tmpCap < 8)
		return false;

	size_t sz = 0;
	char* const data = ModScript_BuildSpliced(requestPath, &sz);
	if (!data || !sz)
		return false;

	char tmpDir[MAX_PATH];
	const DWORD nDir = GetTempPathA(sizeof(tmpDir), tmpDir);
	if (nDir == 0 || nDir >= sizeof(tmpDir))
	{
		free(data);
		return false;
	}

	HANDLE h = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 8; ++attempt)
	{
		const LONG seq = InterlockedIncrement(&s_spliceTmpSeq);
		if (_snprintf_s(tmpPath, tmpCap, _TRUNCATE, "%ssdk_mod_rson_%lu_%ld.tmp",
			tmpDir, GetCurrentProcessId(), seq) < 0)
		{
			free(data);
			return false;
		}

		h = CreateFileA(tmpPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY, nullptr);
		if (h != INVALID_HANDLE_VALUE)
			break;

		const DWORD err = GetLastError();
		if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
		{
			free(data);
			return false;
		}
	}
	if (h == INVALID_HANDLE_VALUE)
	{
		free(data);
		return false;
	}

	DWORD written = 0;
	const BOOL ok = WriteFile(h, data, static_cast<DWORD>(sz), &written, nullptr);
	CloseHandle(h);
	free(data);
	if (!ok || written != sz)
	{
		DeleteFileA(tmpPath);
		return false;
	}
	return true;
}

static bool FSRes_FillBufferFromModScripts_S21(const char* path, void* buf)
{
	if (!path || !buf || !ModSystem()->IsEnabled())
		return false;
	if (!ModPath_IsSafeRel(path))
		return false;

	char disk[1024];
	bool ok = false;
	ModSystem()->LockModList();
	FOR_EACH_VEC(ModSystem()->GetResolvedModList(), i)
	{
		const CModSystem::ModInstance_t* const mod = ModSystem()->GetResolvedModList()[i];
		if (!mod || !mod->IsEnabled())
			continue;

		if (_snprintf_s(disk, sizeof(disk), _TRUNCATE, "%s%s",
			mod->GetBasePath().String(), path) < 0)
			continue;
		if (FSRes_FillBufferFromDiskPath_S21(disk, buf))
		{
			ok = true;
			break;
		}

		const size_t prefixLen = strlen(GAME_SCRIPT_PATH);
		if (!_strnicmp(path, GAME_SCRIPT_PATH, static_cast<int>(prefixLen)))
			continue;
		if (!ModPath_IsSafeRel(GAME_SCRIPT_PATH))
			continue;

		char rel[512];
		if (_snprintf_s(rel, sizeof(rel), _TRUNCATE, "%s%s", GAME_SCRIPT_PATH, path) < 0)
			continue;
		if (!ModPath_IsSafeRel(rel))
			continue;
		if (_snprintf_s(disk, sizeof(disk), _TRUNCATE, "%s%s",
			mod->GetBasePath().String(), rel) < 0)
			continue;
		if (FSRes_FillBufferFromDiskPath_S21(disk, buf))
		{
			ok = true;
			break;
		}
	}
	ModSystem()->UnlockModList();
	return ok;
}

struct ScriptCacheEntry_t
{
	char* data;
	size_t size;
};

static std::unordered_map<std::string, ScriptCacheEntry_t> s_scriptCache;
static std::mutex s_scriptCacheMtx;
static HANDLE s_prefetchDone = nullptr;
static volatile LONG s_prefetchStarted = 0;
static volatile LONG s_prefetchWalkCount = 0;
static size_t s_prefetchBytes = 0;

static constexpr int kScriptCacheMaxFiles = 8000;
static constexpr size_t kScriptCacheMaxFile = 8u * 1024u * 1024u;
static constexpr size_t kScriptCacheMaxTotal = 96u * 1024u * 1024u;

static void ScriptCache_MakeKey(const char* path, char* out, size_t outCap)
{
	size_t n = 0;
	if (!path)
	{
		if (outCap)
			out[0] = '\0';
		return;
	}
	for (; path[n] && n + 1 < outCap; ++n)
	{
		char c = path[n];
		if (c == '\\')
			c = '/';
		else if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c + 32);
		out[n] = c;
	}
	out[n] = '\0';
}

static bool ScriptCache_IsSourceName(const char* name)
{
	if (!name)
		return false;
	const size_t len = strlen(name);
	if (len > 4 && !_stricmp(name + len - 4, ".nut"))
		return true;
	if (len > 5 && !_stricmp(name + len - 5, ".gnut"))
		return true;
	if (len > 5 && !_stricmp(name + len - 5, ".rson"))
		return true;
	return false;
}

static void ScriptCache_Store(const char* key, char* data, size_t size)
{
	if (!key || !data || !size)
	{
		free(data);
		return;
	}

	std::lock_guard<std::mutex> lock(s_scriptCacheMtx);
	const auto it = s_scriptCache.find(key);
	if (it != s_scriptCache.end())
	{
		free(data);
		return;
	}
	if (s_prefetchBytes + size > kScriptCacheMaxTotal)
	{
		free(data);
		return;
	}

	ScriptCacheEntry_t e;
	e.data = data;
	e.size = size;
	s_scriptCache.emplace(key, e);
	s_prefetchBytes += size;
}

static bool ScriptCache_FindUnlocked(const char* key, const char** outData, size_t* outSize)
{
	const auto it = s_scriptCache.find(key);
	if (it == s_scriptCache.end() || !it->second.data || !it->second.size)
		return false;
	*outData = it->second.data;
	*outSize = it->second.size;
	return true;
}

static bool ScriptCache_ReadFile(const char* diskPath, char** outData, size_t* outSize)
{
	*outData = nullptr;
	*outSize = 0;
	if (!diskPath)
		return false;

	HANDLE h = CreateFileA(diskPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER li;
	if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0
		|| li.QuadPart > static_cast<LONGLONG>(kScriptCacheMaxFile))
	{
		CloseHandle(h);
		return false;
	}

	const size_t size = static_cast<size_t>(li.QuadPart);
	char* const buf = static_cast<char*>(malloc(size));
	if (!buf)
	{
		CloseHandle(h);
		return false;
	}

	DWORD readBytes = 0;
	const BOOL ok = ReadFile(h, buf, static_cast<DWORD>(size), &readBytes, nullptr);
	CloseHandle(h);
	if (!ok || readBytes != size)
	{
		free(buf);
		return false;
	}

	*outData = buf;
	*outSize = size;
	return true;
}

static void ScriptCache_LoadDisk(const char* relKey, const char* diskPath)
{
	if (s_prefetchWalkCount >= kScriptCacheMaxFiles)
		return;

	{
		std::lock_guard<std::mutex> lock(s_scriptCacheMtx);
		if (s_scriptCache.find(relKey) != s_scriptCache.end())
			return;
		if (s_prefetchBytes >= kScriptCacheMaxTotal)
			return;
	}

	char* data = nullptr;
	size_t size = 0;
	if (!ScriptCache_ReadFile(diskPath, &data, &size))
		return;

	InterlockedIncrement(&s_prefetchWalkCount);
	ScriptCache_Store(relKey, data, size);
}

static void ScriptCache_Walk(char* diskBuf, const size_t diskCap,
	char* relBuf, const size_t relCap)
{
	if (s_prefetchWalkCount >= kScriptCacheMaxFiles)
		return;

	const size_t diskLen = strlen(diskBuf);
	const size_t relLen = strlen(relBuf);
	if (diskLen + 4 >= diskCap || relLen + 2 >= relCap)
		return;

	memcpy(diskBuf + diskLen, "\\*", 3);

	WIN32_FIND_DATAA fd;
	const HANDLE h = FindFirstFileA(diskBuf, &fd);
	diskBuf[diskLen] = '\0';
	if (h == INVALID_HANDLE_VALUE)
		return;

	int nSafety = 0;
	do
	{
		if (++nSafety > kScriptCacheMaxFiles)
			break;
		if (fd.cFileName[0] == '.' &&
			(fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
			continue;

		const size_t nameLen = strlen(fd.cFileName);
		if (diskLen + 1 + nameLen + 1 >= diskCap)
			continue;
		if (relLen + 1 + nameLen + 1 >= relCap)
			continue;

		_snprintf_s(diskBuf + diskLen, diskCap - diskLen, _TRUNCATE, "\\%s", fd.cFileName);
		_snprintf_s(relBuf + relLen, relCap - relLen, _TRUNCATE, "/%s", fd.cFileName);

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			ScriptCache_Walk(diskBuf, diskCap, relBuf, relCap);
		}
		else if (ScriptCache_IsSourceName(fd.cFileName))
		{
			char key[1024];
			ScriptCache_MakeKey(relBuf, key, sizeof(key));
			ScriptCache_LoadDisk(key, diskBuf);
		}

		diskBuf[diskLen] = '\0';
		relBuf[relLen] = '\0';
	}
	while (FindNextFileA(h, &fd));

	FindClose(h);
}

static bool ScriptCache_ExeRoot(char* out, size_t cap)
{
	if (!out || cap < 8)
		return false;
	const DWORD n = GetModuleFileNameA(NULL, out, static_cast<DWORD>(cap));
	if (n == 0 || n >= cap)
		return false;
	char* slash = strrchr(out, '\\');
	if (!slash)
		return false;
	*slash = '\0';
	return true;
}

static DWORD WINAPI ScriptCache_PrefetchThread(LPVOID)
{
	const DWORD t0 = GetTickCount();

	char disk[1024];
	char rel[1024];
	char root[MAX_PATH];
	if (ScriptCache_ExeRoot(root, sizeof(root)))
		_snprintf_s(disk, sizeof(disk), _TRUNCATE, "%s\\platform\\scripts", root);
	else
		_snprintf_s(disk, sizeof(disk), _TRUNCATE, "platform\\scripts");
	_snprintf_s(rel, sizeof(rel), _TRUNCATE, "scripts");
	ScriptCache_Walk(disk, sizeof(disk), rel, sizeof(rel));

	const DWORD ms = GetTickCount() - t0;
	int nFiles = 0;
	{
		std::lock_guard<std::mutex> lock(s_scriptCacheMtx);
		nFiles = static_cast<int>(s_scriptCache.size());
	}

	if (nFiles <= 0)
	{
		Warning(eDLL_T::FS,
			"[FS-SCRIPT] prefetch found 0 files under '%s'\n", disk);
	}
	else
	{
		Msg(eDLL_T::FS,
			"[FS-SCRIPT] prefetched %d files (%zu KB) in %u ms\n",
			nFiles, s_prefetchBytes / 1024, ms);
	}

	if (s_prefetchDone)
		SetEvent(s_prefetchDone);
	return 0;
}

static void ScriptCache_StartPrefetch(void)
{
	if (InterlockedCompareExchange(&s_prefetchStarted, 1, 0) != 0)
		return;

	s_prefetchDone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!s_prefetchDone)
	{
		ScriptCache_PrefetchThread(nullptr);
		return;
	}

	const HANDLE h = CreateThread(nullptr, 0, ScriptCache_PrefetchThread, nullptr, 0, nullptr);
	if (!h)
	{
		ScriptCache_PrefetchThread(nullptr);
		return;
	}
	CloseHandle(h);
}

static void ScriptCache_WaitReady(void)
{
	ScriptCache_StartPrefetch();
	if (s_prefetchDone)
		WaitForSingleObject(s_prefetchDone, 60000);
}

static bool ScriptCache_Lookup(const char* path, const char** outData, size_t* outSize)
{
	char key[1024];
	ScriptCache_MakeKey(path, key, sizeof(key));

	std::lock_guard<std::mutex> lock(s_scriptCacheMtx);
	if (ScriptCache_FindUnlocked(key, outData, outSize))
		return true;

	const size_t len = strlen(key);
	if (len > 7 && !strcmp(key + len - 7, ".client"))
	{
		key[len - 7] = '\0';
		if (ScriptCache_FindUnlocked(key, outData, outSize))
			return true;
	}
	else if (len > 3 && !strcmp(key + len - 3, ".ui"))
	{
		key[len - 3] = '\0';
		if (ScriptCache_FindUnlocked(key, outData, outSize))
			return true;
	}
	else if (len > 7 && !strcmp(key + len - 7, ".server"))
	{
		key[len - 7] = '\0';
		if (ScriptCache_FindUnlocked(key, outData, outSize))
			return true;
	}
	return false;
}

static bool ScriptCache_TryFill(const char* path, void* buf)
{
	if (!path || !buf)
		return false;
	if (!ModPath_IsSafeRel(path))
		return false;

	ScriptCache_WaitReady();

	const char* data = nullptr;
	size_t size = 0;
	if (ScriptCache_Lookup(path, &data, &size))
		return FSRes_FillBufferFromMemory_S21(buf, data, size);

	char disk[1024];
	char root[MAX_PATH];
	if (ScriptCache_ExeRoot(root, sizeof(root)))
	{
		if (_snprintf_s(disk, sizeof(disk), _TRUNCATE, "%s\\platform\\%s", root, path) < 0)
			return false;
	}
	else if (_snprintf_s(disk, sizeof(disk), _TRUNCATE, "platform\\%s", path) < 0)
	{
		return false;
	}
	for (char* p = disk; *p; ++p)
	{
		if (*p == '/')
			*p = '\\';
	}

	char* loaded = nullptr;
	size_t loadedSize = 0;
	if (!ScriptCache_ReadFile(disk, &loaded, &loadedSize))
		return false;

	char key[1024];
	ScriptCache_MakeKey(path, key, sizeof(key));
	const bool ok = FSRes_FillBufferFromMemory_S21(buf, loaded, loadedSize);
	ScriptCache_Store(key, loaded, loadedSize);
	return ok;
}

static bool __fastcall Hook_FS_AsyncReadScript_S21(
	void* iface, const char* path, void* flags, void* outBuf)
{
	if (path && ModPath_IsSafeRel(path) && ModScript_IsCompileListPath(path))
	{
		size_t sz = 0;
		char* const spliced = ModScript_BuildSpliced(path, &sz);
		if (spliced)
		{
			const bool ok = FSRes_FillBufferFromMemory_S21(outBuf, spliced, sz);
			free(spliced);
			if (ok)
				return true;
		}
	}

	if (path && FSRes_IsScriptSourcePath_S21(path)
		&& ScriptCache_TryFill(path, outBuf))
	{
		return true;
	}

	if (path && FSRes_HasLooseOverride_S21(path)
		&& FSRes_FillBufferFromDisk_S21(path, outBuf))
	{
		static volatile LONG s_lstFillLogged = 0;
		const size_t len = strlen(path);
		if (len > 4 && _stricmp(path + len - 4, ".lst") == 0
			&& InterlockedCompareExchange(&s_lstFillLogged, 1, 0) == 0)
		{
			Msg(eDLL_T::FS, "[FS-RES] disk-fill lst '%s'\n", path);
		}
		return true;
	}

	if (path && FSRes_IsScriptSourcePath_S21(path)
		&& !FSRes_PlatformFileExists_S21(path)
		&& FSRes_FillBufferFromModScripts_S21(path, outBuf))
	{
		return true;
	}

	if (path && FSRes_IsScriptSourcePath_S21(path) && !ModPath_IsSafeRel(path))
	{
		static volatile LONG s_nUnsafeScript;
		const LONG n = InterlockedIncrement(&s_nUnsafeScript);
		if (n <= 4 || (n % 128) == 0)
		{
			Warning(eDLL_T::MODSYSTEM, "[MOD-SCRIPT] refused unsafe script path '%s'\n",
				path);
		}
		return false;
	}

	return v_FS_AsyncReadScript_S21(iface, path, flags, outBuf);
}

static volatile LONG s_diskRedirectInitialized = 0;

// NOP slot-15 .nut/.gnut jnz so fopen consults HEAD platform/.
static bool s_nutRejectPatched = false;

static void PatchNutRejects_S21(uintptr_t exeBase)
{
	if (s_nutRejectPatched) return;

	const bool dx12 = IsDx12Executable_S21();
	const uintptr_t site1 = exeBase + (dx12 ? 0x46A580 : 0x44EB00);
	const uintptr_t site2 = exeBase + (dx12 ? 0x46A598 : 0x44EB18);
	const SIZE_T patchLen = 6;
	const BYTE nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

	auto writeNops = [&](uintptr_t site, const char* tag) -> bool
	{
		DWORD oldProt = 0;
		if (!VirtualProtect(reinterpret_cast<LPVOID>(site), patchLen,
							PAGE_EXECUTE_READWRITE, &oldProt))
		{
			Warning(eDLL_T::FS,
				"nut-reject patch: VirtualProtect failed at %s (0x%llX)\n",
				tag, (unsigned long long)site);
			return false;
		}
		// Sanity: expected 0x0F 0x85 (jnz near) at start.
		const BYTE* p = reinterpret_cast<const BYTE*>(site);
		if (p[0] == 0x90 && p[1] == 0x90 && p[2] == 0x90
			&& p[3] == 0x90 && p[4] == 0x90 && p[5] == 0x90)
		{
			VirtualProtect(reinterpret_cast<LPVOID>(site), patchLen,
						   oldProt, &oldProt);
			return true;
		}
		if (p[0] != 0x0F || p[1] != 0x85)
		{
			Warning(eDLL_T::FS,
				"nut-reject patch: unexpected opcode at %s (%02X %02X, "
				"want 0F 85); aborting patch\n", tag, p[0], p[1]);
			VirtualProtect(reinterpret_cast<LPVOID>(site), patchLen,
						   oldProt, &oldProt);
			return false;
		}
		memcpy(reinterpret_cast<LPVOID>(site), nops, patchLen);
		VirtualProtect(reinterpret_cast<LPVOID>(site), patchLen,
					   oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(),
							  reinterpret_cast<LPCVOID>(site), patchLen);
		return true;
	};

	const bool ok1 = writeNops(site1, dx12 ? ".nut@0x46A580" : ".nut@0x44EB00");
	const bool ok2 = writeNops(site2, dx12 ? ".gnut@0x46A598" : ".gnut@0x44EB18");

	if (ok1 && ok2)
	{
		s_nutRejectPatched = true;
	}
	else
	{
		Warning(eDLL_T::FS,
			"nut-reject patch: one or both patches failed; "
			"scripts will remain asset-catalog-only\n");
	}
}

// Loose .res/.menu/.lst: fill slot-15 from disk; null preloaded KV / skip cache so the file is re-read.

// True if `path` is a menu-layout file (.res/.menu), a bind list (.lst), or the
// VGUI screen class table -- with a loose override present on disk at platform\<path>.
// vgui_screens.txt is the PanelMetaClass registry CreateClientsideVGuiScreen looks up.
static bool FSRes_HasLooseOverride_S21(const char* path)
{
	if (!path || !ModPath_IsSafeRel(path))
		return false;
	const size_t len = strlen(path);
	const bool isRes  = len > 4 && _stricmp(path + len - 4, ".res")  == 0;
	const bool isMenu = len > 5 && _stricmp(path + len - 5, ".menu") == 0;
	const bool isLst  = len > 4 && _stricmp(path + len - 4, ".lst")  == 0;
	const bool isVguiScreens = _stricmp(path, "scripts/vgui_screens.txt") == 0;
	if (!isRes && !isMenu && !isLst && !isVguiScreens)
		return false;

	char disk[1024];
	if (_snprintf_s(disk, sizeof(disk), _TRUNCATE, "platform\\%s", path) < 0)
		return false;
	const DWORD attr = GetFileAttributesA(disk);
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Null preloaded KV so LoadFromFile hits slot-15 disk-fill.
typedef __int64 (__fastcall* BuildGroupLoadCtrl_fn)(
	void* bg, const char* resName, const char* pathID, __int64 preKV, __int64 cond);
static BuildGroupLoadCtrl_fn v_BuildGroup_LoadControlSettings_S21 = nullptr;

static __int64 __fastcall Hook_BuildGroup_LoadControlSettings_S21(
	void* bg, const char* resName, const char* pathID, __int64 preKV, __int64 cond)
{
	if (resName && FSRes_HasLooseOverride_S21(resName))
		preKV = 0;   // force the LoadFromFile path so slot-15 disk-fill loads the loose copy
	return v_BuildGroup_LoadControlSettings_S21(bg, resName, pathID, preKV, cond);
}

// Uncached LoadFromFile for loose .res so a pak-cached KV cannot win.
typedef __int64 (__fastcall* CtrlSettingsLoad_fn)(const char* path);
static CtrlSettingsLoad_fn v_CtrlSettingsLoad_S21 = nullptr;
typedef __int64 (__fastcall* KVCtorName_fn)(__int64 kvMem, const char* name);
typedef char    (__fastcall* KVLoadFromFile_fn)(
	__int64 kv, __int64* fs, __int64 path, __int64 a4, char a5, __int64 pathID, __int64 a7);
static KVCtorName_fn     v_KV_CtorName_S21     = nullptr;
static KVLoadFromFile_fn v_KV_LoadFromFile_S21 = nullptr;
static uintptr_t         g_KVAllocIface_S21    = 0;   // & (KeyValues mempool interface)
static uintptr_t         g_HookedFsGlobal_S21  = 0;   // & (the FS singleton the bridge hooks)

static __int64 __fastcall Hook_CtrlSettingsLoad_S21(const char* path)
{
	if (path && v_KV_CtorName_S21 && v_KV_LoadFromFile_S21 && g_KVAllocIface_S21 && g_HookedFsGlobal_S21
		&& FSRes_HasLooseOverride_S21(path))
	{
		// Symbols preflighted once at EnsureScriptDiskRedirect install.
		__int64 kv = 0;
		void** const ifaceVtbl = *reinterpret_cast<void***>(g_KVAllocIface_S21);
		typedef __int64 (__fastcall* KVAlloc_fn)(void* iface, __int64 size);
		const KVAlloc_fn allocFn = ifaceVtbl
			? reinterpret_cast<KVAlloc_fn>(ifaceVtbl[1])
			: nullptr;
		if (allocFn)
		{
			const __int64 kvMem = allocFn(reinterpret_cast<void*>(g_KVAllocIface_S21), 64);
			if (kvMem)
			{
				kv = v_KV_CtorName_S21(kvMem, path);
				// fs sub-object = * + 8; its slot-15 disk-fills the loose copy.
				const uintptr_t fsSub = *reinterpret_cast<uintptr_t*>(g_HookedFsGlobal_S21) + 8;
				const char ok = v_KV_LoadFromFile_S21(
					kv, reinterpret_cast<__int64*>(fsSub), reinterpret_cast<__int64>(path), 0, 0, 0, 0);
				if (!ok)
					kv = 0;
			}
		}

		if (kv)
			return kv;
	}
	return v_CtrlSettingsLoad_S21(path);
}

typedef void (__fastcall* AddSearchPath_fn)(void*, const char*, const char*, int);
static void* s_headFs = nullptr;
static AddSearchPath_fn s_headAddSearchPath = nullptr;

static void Stage_FS_HeadSearchPath(void)
{
	if (!s_headFs || !s_headAddSearchPath)
		return;
	s_headAddSearchPath(s_headFs, "platform", "GAME", 0);
	Msg(eDLL_T::FS,
		"[FS-HEAD] platform/ HEAD GAME search path installed "
		"(fs=%p full=%p -> %p stdio=%p)\n",
		s_headFs, (void*)g_pFullFileSystem,
		g_pFullFileSystem ? (void*)*g_pFullFileSystem : nullptr,
		(void*)g_pFileSystem_Stdio);
}

void EnsureScriptDiskRedirect_S21(void)
{
	ScriptCache_StartPrefetch();

	if (InterlockedCompareExchange(&s_diskRedirectInitialized, 1, 0) != 0)
		return;  // already done (or in flight on another thread)

	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe)
	{
		Warning(eDLL_T::FS,
			"script disk-redirect: GetModuleHandleA failed\n");
		return;
	}
	const uintptr_t exeBase = reinterpret_cast<uintptr_t>(hExe);

	const bool bDx12 = IsDx12Executable_S21();
	v_Buf_EnsureCapacity_S21 = reinterpret_cast<Buf_EnsureCapacity_fn>(exeBase + (bDx12 ? 0x1F80E0 : 0x1F5340));
	v_Buf_Put_S21            = reinterpret_cast<Buf_Put_fn>(exeBase + (bDx12 ? 0x586950 : 0x555BD0));

	PatchNutRejects_S21(exeBase);

	void* fs = nullptr;
	if (g_pFullFileSystem && *g_pFullFileSystem)
		fs = *g_pFullFileSystem;
	else
		fs = g_pFileSystem_Stdio;

	if (!fs)
	{
		Warning(eDLL_T::FS,
			"[FS-HEAD] filesystem singleton NULL; will retry "
			"(COM post-native or next PreCompile)\n");
		InterlockedExchange(&s_diskRedirectInitialized, 0);
		return;
	}

	// GAME HEAD (addType=0) so loose platform/* shadows RPAK/VPK.
	void** vtbl = *reinterpret_cast<void***>(fs);
	if (vtbl)
	{
 s_headFs = fs;
 s_headAddSearchPath = reinterpret_cast<AddSearchPath_fn>(vtbl[16]);
 // T1 one-shot; slot-15 install continues whether this stage faults.
 SdkStage_Run("FS_HeadSearchPath", Stage_FS_HeadSearchPath);
	}
	else
	{
 Warning(eDLL_T::FS, "[FS-HEAD] filesystem vtable NULL; HEAD skipped\n");
	}

	// Slot 15 is on the +8 secondary iface (no deref of fs for this-pointer).
	void* const subIface = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(fs) + 8);

	void** vtbl2 = *reinterpret_cast<void***>(subIface);
	if (!vtbl2)
	{
 Warning(eDLL_T::FS,
 "script disk-redirect: secondary vtable NULL\n");
 return;
	}
	v_FS_AsyncReadScript_S21 =
 reinterpret_cast<FS_AsyncReadScript_fn>(vtbl2[15]);
	if (!v_FS_AsyncReadScript_S21)
	{
 Warning(eDLL_T::FS,
 "script disk-redirect: vtbl[15] is NULL\n");
 return;
	}

	// Own transaction; skipping it yields ERROR_INVALID_TRANSACTION_STATE.
	LONG rc;
	if ((rc = DetourTransactionBegin()) != NO_ERROR ||
 (rc = DetourUpdateThread(GetCurrentThread())) != NO_ERROR ||
 (rc = DetourAttach(reinterpret_cast<void**>(&v_FS_AsyncReadScript_S21),
 reinterpret_cast<void*>(&Hook_FS_AsyncReadScript_S21))) != NO_ERROR ||
 (rc = DetourTransactionCommit()) != NO_ERROR)
	{
 Warning(eDLL_T::FS,
 "script disk-redirect: slot-15 detour transaction failed "
 "rc=%ld\n", rc);
 // DetourTransactionAbort in case Begin succeeded but a later
 // step failed. Safe to call even if no transaction is open --
 // it just returns an error.
 DetourTransactionAbort();
 v_FS_AsyncReadScript_S21 = nullptr;
 return;
	}

	Msg(eDLL_T::FS,
 "[FS-HEAD] slot-15 hook installed "
 "(fs=%p subIface=%p vtbl2=%p target=%p)\n",
 fs, subIface, (void*)vtbl2, (void*)v_FS_AsyncReadScript_S21);

	v_BuildGroup_LoadControlSettings_S21 =
 reinterpret_cast<BuildGroupLoadCtrl_fn>(exeBase + (bDx12 ? 0xD71EC0: 0xD402D0));
	LONG rc3;
	if ((rc3 = DetourTransactionBegin()) != NO_ERROR ||
 (rc3 = DetourUpdateThread(GetCurrentThread())) != NO_ERROR ||
 (rc3 = DetourAttach(reinterpret_cast<void**>(&v_BuildGroup_LoadControlSettings_S21),
 reinterpret_cast<void*>(&Hook_BuildGroup_LoadControlSettings_S21))) != NO_ERROR ||
 (rc3 = DetourTransactionCommit()) != NO_ERROR)
	{
 Warning(eDLL_T::FS,
 "menu-res disk override: LoadControlSettings detour failed rc=%ld\n", rc3);
 DetourTransactionAbort();
 v_BuildGroup_LoadControlSettings_S21 = nullptr;
	}

	v_KV_CtorName_S21 = reinterpret_cast<KVCtorName_fn>(exeBase + (bDx12 ? 0x579710: 0x548990));
	v_KV_LoadFromFile_S21 = reinterpret_cast<KVLoadFromFile_fn>(exeBase + (bDx12 ? 0x57A200: 0x549480));
	g_KVAllocIface_S21 = exeBase + (bDx12 ? 0x6E27D80: 0xAA5DE90);
	g_HookedFsGlobal_S21 = exeBase + (bDx12 ? 0x3403C48: 0x74F4DD0);
	v_CtrlSettingsLoad_S21 = reinterpret_cast<CtrlSettingsLoad_fn>(exeBase + (bDx12 ? 0x8DCA00: 0x8AB9E0));

	// Boot Mem_InModule once; hook body then plain-reads (no per-call SEH).
	bool kvSymOk =
 Mem_InModule(g_GameDll, reinterpret_cast<const void*>(v_KV_CtorName_S21), 16) &&
 Mem_InModule(g_GameDll, reinterpret_cast<const void*>(v_KV_LoadFromFile_S21), 16) &&
 Mem_InModule(g_GameDll, reinterpret_cast<const void*>(g_KVAllocIface_S21), sizeof(void*)) &&
 Mem_InModule(g_GameDll, reinterpret_cast<const void*>(g_HookedFsGlobal_S21), sizeof(void*)) &&
 Mem_InModule(g_GameDll, reinterpret_cast<const void*>(v_CtrlSettingsLoad_S21), 16);
	if (kvSymOk)
	{
 void** const ifaceVtbl = *reinterpret_cast<void***>(g_KVAllocIface_S21);
 if (!ifaceVtbl ||
 !Mem_InModule(g_GameDll, ifaceVtbl, 2 * sizeof(void*)) ||
 !ifaceVtbl[1] ||
 !Mem_InModule(g_GameDll, ifaceVtbl[1], 16))
 {
 kvSymOk = false;
 }
	}
	if (!kvSymOk)
	{
 Warning(eDLL_T::FS,
 "menu-res disk override: control-settings KV symbols outside module "
 "-- cache-bypass hook skipped\n");
 v_KV_CtorName_S21 = nullptr;
 v_KV_LoadFromFile_S21 = nullptr;
 g_KVAllocIface_S21 = 0;
 g_HookedFsGlobal_S21 = 0;
 v_CtrlSettingsLoad_S21 = nullptr;
	}
	else
	{
 LONG rc4;
 if ((rc4 = DetourTransactionBegin()) != NO_ERROR ||
 (rc4 = DetourUpdateThread(GetCurrentThread())) != NO_ERROR ||
 (rc4 = DetourAttach(reinterpret_cast<void**>(&v_CtrlSettingsLoad_S21),
 reinterpret_cast<void*>(&Hook_CtrlSettingsLoad_S21))) != NO_ERROR ||
 (rc4 = DetourTransactionCommit()) != NO_ERROR)
 {
 Warning(eDLL_T::FS,
 "menu-res disk override: control-settings loader detour failed rc=%ld\n", rc4);
 DetourTransactionAbort();
 v_CtrlSettingsLoad_S21 = nullptr;
 }
	}
}

// Legacy export -- kept so the linker is happy for the extern declared
// in vscript_s21_override.h. Forwards to the one-shot function.
void InstallScriptDiskRedirectHooks_S21(void)
{
	EnsureScriptDiskRedirect_S21();
}

void RemoveScriptDiskRedirectHooks_S21(void)
{
	if (v_FS_AsyncReadScript_S21)
	{
 DetourDetach(reinterpret_cast<void**>(&v_FS_AsyncReadScript_S21),
 reinterpret_cast<void*>(&Hook_FS_AsyncReadScript_S21));
 v_FS_AsyncReadScript_S21 = nullptr;
	}
	if (v_BuildGroup_LoadControlSettings_S21)
	{
 DetourDetach(reinterpret_cast<void**>(&v_BuildGroup_LoadControlSettings_S21),
 reinterpret_cast<void*>(&Hook_BuildGroup_LoadControlSettings_S21));
 v_BuildGroup_LoadControlSettings_S21 = nullptr;
	}
	if (v_CtrlSettingsLoad_S21)
	{
 DetourDetach(reinterpret_cast<void**>(&v_CtrlSettingsLoad_S21),
 reinterpret_cast<void*>(&Hook_CtrlSettingsLoad_S21));
 v_CtrlSettingsLoad_S21 = nullptr;
	}
}

static volatile LONG s_modSystemInited = 0;
static bool s_bModConfigsLoaded = false;
static bool s_bModConfigsWarned = false;

static void ModSystem_TryLoadModConfigs(const bool bFinal)
{
	if (s_bModConfigsLoaded)
		return;

	if (!Cbuf_AddText)
	{
		if (bFinal)
		{
			if (!s_bModConfigsWarned)
			{
				s_bModConfigsWarned = true;
				Warning(eDLL_T::MODSYSTEM,
					"[MOD-LOAD] LoadModConfigs skipped: Cbuf_AddText unresolved\n");
			}
			s_bModConfigsLoaded = true;
		}
		return;
	}

	if (!g_pHostState)
	{
		if (bFinal)
		{
			s_bModConfigsLoaded = true;
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-LOAD] LoadModConfigs skipped: host state unavailable\n");
		}
		return;
	}

	s_bModConfigsLoaded = true;
	g_pHostState->LoadModConfigs();
}

static void ModSystem_EnsureClientInit(void)
{
	if (!FileSystem())
		return;
	if (InterlockedCompareExchange(&s_modSystemInited, 1, 0) != 0)
		return;

	ModSystem()->Init();

	int nMods = 0;
	int nEnabled = 0;
	ModSystem()->LockModList();
	FOR_EACH_VEC(ModSystem()->GetResolvedModList(), i)
	{
		++nMods;
		if (ModSystem()->GetResolvedModList()[i]->IsEnabled())
			++nEnabled;
	}
	ModSystem()->UnlockModList();

	Msg(eDLL_T::MODSYSTEM, "[MOD-LOAD] client modsystem init: %d mods (%d enabled)\n",
		nMods, nEnabled);

	ModSystem_TryLoadModConfigs(false);
}

static void __fastcall Hook_COM_InitFilesystem_S21(const char* pFullModPath)
{
	if (v_COM_InitFilesystem_S21)
		v_COM_InitFilesystem_S21(pFullModPath);

	Msg(eDLL_T::FS,
		"[FS-HEAD] COM_InitFilesystem native done (mod=\"%.160s\"); "
		"installing platform HEAD + disk redirect\n",
		pFullModPath ? pFullModPath : "(null)");

	// Shared install: HEAD AddSearchPath + slot-15 + nut-reject NOPs +
	// menu-res complementary hooks. One-shot; PreCompile retry if FS null.
	InstallScriptDiskRedirectHooks_S21();
	ModSystem_EnsureClientInit();
}

void VPlatformFSOverrideS21::Detour(const bool bAttach) const
{
	if (v_COM_InitFilesystem_S21)
 DetourSetup(&v_COM_InitFilesystem_S21,
 &Hook_COM_InitFilesystem_S21, bAttach);
	else
 Warning(eDLL_T::FS,
 "[FS-HEAD] COM_InitFilesystem_S21 pattern unresolved -- "
 "platform/ HEAD falls back to PreCompileScript lazy path\n");
}

// Free the tree, then the DynPage list (native did pages first).
static void (__fastcall *v_RSON_FreeValuePayload_S21)(void* value, void* alloc) = nullptr;
static uint8_t* s_rsonTeardownSite = nullptr;
static uint8_t s_rsonTeardownSaved[14] = {};
static uint8_t* s_rsonTeardownStub = nullptr;
static bool s_rsonTeardownPatched = false;

static constexpr uint8_t s_rsonTeardownExpect[4] = { 0x48, 0x8B, 0x55, 0x88 };

// Opcode 0x88/0x80/0xA8/0xB0 are signed rbp: -0x78/-0x80/-0x58/-0x50.
static constexpr ptrdiff_t RSON_FRAME_VALUE = -0x58;
static constexpr ptrdiff_t RSON_FRAME_ALLOC = -0x50;
static constexpr ptrdiff_t RSON_FRAME_PAGE  = -0x78;
static constexpr ptrdiff_t RSON_FRAME_HEAP  = -0x80;

// Both allocators here are called the way the native epilogue calls them:
// the free function sits at +8 of the allocator itself, receiving it as 'this'.
typedef void (__fastcall* RsonFree_t)(void* pAllocator, void* pBlock);

static bool ScriptRson_IsUserPtr(const void* const p)
{
	const uintptr_t u = reinterpret_cast<uintptr_t>(p);
	return u > 0x10000ull && u < 0x00007FFFFFFFFFFFull && (u & 7) == 0;
}

static bool ScriptRson_IsCallerFrame(const uint8_t* const frame)
{
	const NT_TIB* const pTib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
	const uint8_t* const pLow = static_cast<const uint8_t*>(pTib->StackLimit);
	const uint8_t* const pHigh = static_cast<const uint8_t*>(pTib->StackBase);

	return frame >= pLow + 0x100 && frame + 0x100 < pHigh;
}

static bool ScriptRson_IsCallable(const void* const pFn)
{
	MEMORY_BASIC_INFORMATION mbi;

	if (!VirtualQuery(pFn, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return false;

	if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		return false;

	return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
		PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static bool ScriptRson_GetFree(void* const pAllocator, RsonFree_t* const pOut)
{
	if (!ScriptRson_IsUserPtr(pAllocator))
		return false;

	void* const pFn = *reinterpret_cast<void* const*>(
		static_cast<const uint8_t*>(pAllocator) + 8);

	if (!ScriptRson_IsCallable(pFn))
		return false;

	*pOut = reinterpret_cast<RsonFree_t>(pFn);
	return true;
}

// Anything that fails a check is left to the native epilogue, which the stub
// re-enters: stock order and a leaked page list beat a free of the wrong bytes.
static void __fastcall ScriptRson_FreeTreeBeforePages(uint8_t* const frame)
{
	if (!frame || !ScriptRson_IsCallerFrame(frame))
		return;

	void** const ppValue = reinterpret_cast<void**>(frame + RSON_FRAME_VALUE);
	void** const ppAlloc = reinterpret_cast<void**>(frame + RSON_FRAME_ALLOC);
	void* const pValue = *ppValue;
	void* const pAlloc = *ppAlloc;
	RsonFree_t freeTree = nullptr;

	if (v_RSON_FreeValuePayload_S21 && ScriptRson_IsUserPtr(pValue) &&
		ScriptRson_GetFree(pAlloc, &freeTree))
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			Msg(eDLL_T::ENGINE,
				"[RSON-TEAR] scripts.rson tree freed before DynPage walk\n");
		}

		v_RSON_FreeValuePayload_S21(pValue, pAlloc);
		freeTree(pAlloc, pValue);

		// Zeroed so the native epilogue skips the free it would do next.
		*ppValue = nullptr;
		*ppAlloc = nullptr;
	}

	void* pPage = *reinterpret_cast<void**>(frame + RSON_FRAME_PAGE);
	void* const pHeap = *reinterpret_cast<void**>(frame + RSON_FRAME_HEAP);
	RsonFree_t freePage = nullptr;

	if (!ScriptRson_GetFree(pHeap, &freePage))
		return;

	for (int i = 0; i < 4096 && ScriptRson_IsUserPtr(pPage); i++)
	{
		void* const pNext = *reinterpret_cast<void**>(pPage);

		if (pNext == pPage)
			break;

		freePage(pHeap, pPage);
		pPage = pNext;
	}
}

void ScriptRson_ResolveTeardownPatch_S21(void)
{
	if (!v_RSON_FreeValuePayload_S21)
	{
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? "
			"4C 89 74 24 ?? 41 57 48 83 EC ?? 8B 01")
			.GetPtr(v_RSON_FreeValuePayload_S21);
		if (!v_RSON_FreeValuePayload_S21)
			Warning(eDLL_T::ENGINE,
				"[RSON-TEAR] RSON_FreeValuePayload pattern unresolved\n");
	}

	if (!s_rsonTeardownSite)
	{
		// Full epilogue: page walk at [rbp+88] then FreeValue of [rbp+A8]/[rbp+B0].
		// The walk prefix alone is not unique on the DX12 exe (hits a path buffer).
		const CMemory site = Module_FindPattern(g_GameDll,
			"48 8B 55 88 48 85 D2 74 1B 66 0F 1F 44 00 00 "
			"48 8B 45 80 48 8B 1A 48 8B C8 FF 50 08 "
			"48 8B D3 48 85 DB 75 EB "
			"48 8B 5D B0 4C 89 65 88 4C 89 65 A0 48 85 DB 74 1D "
			"48 8B 7D A8 48 8B D3 48 8B CF E8");
		s_rsonTeardownSite = site.RCast<uint8_t*>();
		if (!s_rsonTeardownSite)
			Warning(eDLL_T::ENGINE,
				"[RSON-TEAR] DynPage walk site unresolved\n");
		else
			LogFunAdr("VScriptPrecompile_Teardown", s_rsonTeardownSite);
	}
}

void ScriptRson_InstallTeardownPatch_S21(const bool bAttach)
{
	ScriptRson_ResolveTeardownPatch_S21();

	if (!bAttach)
	{
		if (s_rsonTeardownPatched && s_rsonTeardownSite)
		{
			DWORD oldProt = 0;
			if (VirtualProtect(s_rsonTeardownSite, sizeof(s_rsonTeardownSaved),
				PAGE_EXECUTE_READWRITE, &oldProt))
			{
				memcpy(s_rsonTeardownSite, s_rsonTeardownSaved, sizeof(s_rsonTeardownSaved));
				VirtualProtect(s_rsonTeardownSite, sizeof(s_rsonTeardownSaved),
					oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(),
					s_rsonTeardownSite, sizeof(s_rsonTeardownSaved));
			}
			s_rsonTeardownPatched = false;
		}
		if (s_rsonTeardownStub)
		{
			VirtualFree(s_rsonTeardownStub, 0, MEM_RELEASE);
			s_rsonTeardownStub = nullptr;
		}
		return;
	}

	if (s_rsonTeardownPatched || !s_rsonTeardownSite || !v_RSON_FreeValuePayload_S21)
		return;

	if (memcmp(s_rsonTeardownSite, s_rsonTeardownExpect, sizeof(s_rsonTeardownExpect)) != 0)
	{
		Warning(eDLL_T::ENGINE,
			"[RSON-TEAR] unexpected opcode at walk site (%02X %02X %02X %02X); abort\n",
			s_rsonTeardownSite[0], s_rsonTeardownSite[1],
			s_rsonTeardownSite[2], s_rsonTeardownSite[3]);
		return;
	}

	s_rsonTeardownStub = static_cast<uint8_t*>(VirtualAlloc(
		nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (!s_rsonTeardownStub)
	{
		Warning(eDLL_T::ENGINE, "[RSON-TEAR] VirtualAlloc stub failed\n");
		return;
	}

	// Pad 0x20: site rsp is 16-aligned; 0x28 misaligns SSE spill in EngineLoggerSink.
	uint8_t* p = s_rsonTeardownStub;
	*p++ = 0x48; *p++ = 0x89; *p++ = 0xE9;
	*p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x20;
	*p++ = 0x48; *p++ = 0xB8;
	const uint64_t helper = reinterpret_cast<uint64_t>(&ScriptRson_FreeTreeBeforePages);
	memcpy(p, &helper, 8); p += 8;
	*p++ = 0xFF; *p++ = 0xD0;
	*p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x20;
	*p++ = 0xFF; *p++ = 0x25; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
	const uint64_t cont = reinterpret_cast<uint64_t>(s_rsonTeardownSite + 0x24);
	memcpy(p, &cont, 8);

	DWORD stubProt = 0;
	if (!VirtualProtect(s_rsonTeardownStub, 64, PAGE_EXECUTE_READ, &stubProt))
	{
		Warning(eDLL_T::ENGINE, "[RSON-TEAR] stub could not be sealed\n");
		VirtualFree(s_rsonTeardownStub, 0, MEM_RELEASE);
		s_rsonTeardownStub = nullptr;
		return;
	}
	FlushInstructionCache(GetCurrentProcess(), s_rsonTeardownStub, 64);

	memcpy(s_rsonTeardownSaved, s_rsonTeardownSite, sizeof(s_rsonTeardownSaved));

	uint8_t jmpAbs[14] = {
		0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
		0, 0, 0, 0, 0, 0, 0, 0
	};
	const uint64_t stubAddr = reinterpret_cast<uint64_t>(s_rsonTeardownStub);
	memcpy(jmpAbs + 6, &stubAddr, 8);

	DWORD oldProt = 0;
	if (!VirtualProtect(s_rsonTeardownSite, sizeof(jmpAbs),
		PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE, "[RSON-TEAR] VirtualProtect site failed\n");
		VirtualFree(s_rsonTeardownStub, 0, MEM_RELEASE);
		s_rsonTeardownStub = nullptr;
		return;
	}
	memcpy(s_rsonTeardownSite, jmpAbs, sizeof(jmpAbs));
	VirtualProtect(s_rsonTeardownSite, sizeof(jmpAbs), oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), s_rsonTeardownSite, sizeof(jmpAbs));
	s_rsonTeardownPatched = true;
	Msg(eDLL_T::ENGINE,
		"[RSON-TEAR] DynPage walk redirected (site=%p stub=%p)\n",
		s_rsonTeardownSite, s_rsonTeardownStub);
}

static char __fastcall Hook_RSON_LoadFileFromPath_S21(
	void* out, const char* path, __int64 a3, __int64 a4, void* alloc, __int64* a6)
{
	if (!v_RSON_LoadFileFromPath_S21)
		return 0;

	char tmpPath[MAX_PATH] = {};
	const char* usePath = path;
	bool bTmp = false;

	if (path && ModPath_IsSafeRel(path) && ModScript_IsCompileListPath(path)
		&& ModScript_WriteSplicedTemp(path, tmpPath, sizeof(tmpPath)))
	{
		usePath = tmpPath;
		bTmp = true;
	}

	const char ok = v_RSON_LoadFileFromPath_S21(out, usePath, a3, a4, alloc, a6);
	if (bTmp)
	{
		if (!ok)
		{
			const char retry = v_RSON_LoadFileFromPath_S21(out, path, a3, a4, alloc, a6);
			DeleteFileA(tmpPath);
			return retry;
		}
		DeleteFileA(tmpPath);
	}
	return ok;
}

void VFSScriptRedirectS21::Detour(const bool bAttach) const
{
	if (!v_RSON_LoadFileFromPath_S21)
	{
		if (bAttach)
		{
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-SCRIPT] RSON load-from-file pattern unresolved -- "
				"scripts.rson splice disabled\n");
		}
	}
	else
	{
		DetourSetup(&v_RSON_LoadFileFromPath_S21, &Hook_RSON_LoadFileFromPath_S21, bAttach);
	}

	ScriptRson_InstallTeardownPatch_S21(bAttach);
	// Slot 15 is installed / removed lazily from
	// InstallScriptDiskRedirectHooks_S21 / RemoveScriptDiskRedirectHooks_S21
	// because it depends on a runtime singleton. On detach, clean up.
	if (!bAttach)
		RemoveScriptDiskRedirectHooks_S21();
}
