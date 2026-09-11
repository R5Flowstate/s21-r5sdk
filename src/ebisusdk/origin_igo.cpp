//=============================================================================//
//
// Purpose: keep IGO64.dll out of the process. The overlay's Present/raw-input
//          hooks fight the client's swapchain and input. LSX identity is
//          untouched -- StartIGO runs after the handshake and its return is
//          discarded.
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/commandline.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "origin_igo.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <cstdint>

void SDK_Log(const char* fmt, ...);

static bool s_loadHookInstalled = false;
static bool s_startIgoHooked = false;
static void* s_startIgoAddr = nullptr;
static volatile LONG s_blockedLoads = 0;
static volatile LONG s_startIgoHits = 0;
static char s_firstBlocked[512] = {};
static volatile LONG s_firstBlockedSet = 0;

static bool IgoGuard_IsRequested(void)
{
	static const bool s_requested = []() -> bool
	{
		if (!CommandLine())
			return true;
		return CommandLine()->CheckParm("-sdk_allow_igo") == nullptr;
	}();
	return s_requested;
}

static const char* IgoGuard_BasenameA(const char* p)
{
	if (!p || !p[0])
		return "";
	const char* b = p;
	for (const char* s = p; *s; ++s)
	{
		if (*s == '\\' || *s == '/')
			b = s + 1;
	}
	return b;
}

static const wchar_t* IgoGuard_BasenameW(const wchar_t* p)
{
	if (!p || !p[0])
		return L"";
	const wchar_t* b = p;
	for (const wchar_t* s = p; *s; ++s)
	{
		if (*s == L'\\' || *s == L'/')
			b = s + 1;
	}
	return b;
}

static bool IgoGuard_IsIgoNameA(const char* base)
{
	if (!base || !base[0])
		return false;
	return !V_stricmp(base, "IGO64.dll")
		|| !V_stricmp(base, "IGO64d.dll")
		|| !V_stricmp(base, "IGO32.dll")
		|| !V_stricmp(base, "IGO32d.dll");
}

static bool IgoGuard_IsIgoNameW(const wchar_t* base)
{
	if (!base || !base[0])
		return false;
	return _wcsicmp(base, L"IGO64.dll") == 0
		|| _wcsicmp(base, L"IGO64d.dll") == 0
		|| _wcsicmp(base, L"IGO32.dll") == 0
		|| _wcsicmp(base, L"IGO32d.dll") == 0;
}

static void IgoGuard_RecordBlocked(const char* display)
{
	InterlockedIncrement(&s_blockedLoads);
	if (InterlockedCompareExchange(&s_firstBlockedSet, 1, 0) == 0)
	{
		size_t i = 0;
		const char* src = display ? display : "";
		for (; i + 1 < sizeof(s_firstBlocked) && src[i]; ++i)
			s_firstBlocked[i] = src[i];
		s_firstBlocked[i] = '\0';
	}
}

static void IgoGuard_WideToUtf8(const wchar_t* src, char* out, size_t cap)
{
	if (!out || cap == 0)
		return;
	out[0] = '\0';
	if (!src)
		return;
	const int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, out, static_cast<int>(cap),
		nullptr, nullptr);
	if (n <= 0)
		out[0] = '\0';
	else
		out[cap - 1] = '\0';
}

static void IgoGuard_LogBlocked(const char* display)
{
	const LONG n = s_blockedLoads;
	if (n <= 4 || (n % 16) == 0)
	{
		Warning(eDLL_T::ENGINE, "[IGO-GUARD] blocked load of '%s' (n=%ld)\n",
			display && display[0] ? display : "(null)", (long)n);
	}
}

typedef HMODULE(WINAPI* PFN_LoadLibraryA)(LPCSTR);
typedef HMODULE(WINAPI* PFN_LoadLibraryW)(LPCWSTR);
typedef HMODULE(WINAPI* PFN_LoadLibraryExA)(LPCSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI* PFN_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);

static PFN_LoadLibraryA v_LoadLibraryA = nullptr;
static PFN_LoadLibraryW v_LoadLibraryW = nullptr;
static PFN_LoadLibraryExA v_LoadLibraryExA = nullptr;
static PFN_LoadLibraryExW v_LoadLibraryExW = nullptr;

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName)
{
	if (IgoGuard_IsRequested() && IgoGuard_IsIgoNameA(IgoGuard_BasenameA(lpLibFileName)))
	{
		IgoGuard_RecordBlocked(lpLibFileName);
		IgoGuard_LogBlocked(lpLibFileName);
		SetLastError(ERROR_MOD_NOT_FOUND);
		return nullptr;
	}
	if (!v_LoadLibraryA)
	{
		SetLastError(ERROR_PROC_NOT_FOUND);
		return nullptr;
	}
	return v_LoadLibraryA(lpLibFileName);
}

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName)
{
	if (IgoGuard_IsRequested() && IgoGuard_IsIgoNameW(IgoGuard_BasenameW(lpLibFileName)))
	{
		char display[512];
		IgoGuard_WideToUtf8(lpLibFileName, display, sizeof(display));
		IgoGuard_RecordBlocked(display);
		IgoGuard_LogBlocked(display);
		SetLastError(ERROR_MOD_NOT_FOUND);
		return nullptr;
	}
	if (!v_LoadLibraryW)
	{
		SetLastError(ERROR_PROC_NOT_FOUND);
		return nullptr;
	}
	return v_LoadLibraryW(lpLibFileName);
}

static HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
	if (IgoGuard_IsRequested() && IgoGuard_IsIgoNameA(IgoGuard_BasenameA(lpLibFileName)))
	{
		IgoGuard_RecordBlocked(lpLibFileName);
		IgoGuard_LogBlocked(lpLibFileName);
		SetLastError(ERROR_MOD_NOT_FOUND);
		return nullptr;
	}
	if (!v_LoadLibraryExA)
	{
		SetLastError(ERROR_PROC_NOT_FOUND);
		return nullptr;
	}
	return v_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
}

static HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
	if (IgoGuard_IsRequested() && IgoGuard_IsIgoNameW(IgoGuard_BasenameW(lpLibFileName)))
	{
		char display[512];
		IgoGuard_WideToUtf8(lpLibFileName, display, sizeof(display));
		IgoGuard_RecordBlocked(display);
		IgoGuard_LogBlocked(display);
		SetLastError(ERROR_MOD_NOT_FOUND);
		return nullptr;
	}
	if (!v_LoadLibraryExW)
	{
		SetLastError(ERROR_PROC_NOT_FOUND);
		return nullptr;
	}
	return v_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
}

// Return 0 (ORIGIN_SUCCESS), same as the vendor flags&0x100 skip. Do not OR
// 0x100 into the SDK flags -- StartOrigin reads that bit too.
typedef int64_t(__fastcall* PFN_StartIGO)(void* pThis, uint8_t bUseApi, char bDebug);
static PFN_StartIGO v_OriginSDK_StartIGO = nullptr;

static int64_t __fastcall Hook_OriginSDK_StartIGO(void* pThis, uint8_t bUseApi, char bDebug)
{
	(void)pThis;
	const LONG n = InterlockedIncrement(&s_startIgoHits);
	if (n <= 3 || (n % 16) == 0)
	{
		Warning(eDLL_T::ENGINE,
			"[IGO-GUARD] StartIGO no-op (useApi=%u debug=%d n=%ld)\n",
			(unsigned)bUseApi, (int)bDebug, (long)n);
	}
	return 0;
}

static void IgoGuard_InstallLoadHooks(void)
{
	v_LoadLibraryA = reinterpret_cast<PFN_LoadLibraryA>(
		DetourFindFunction("KERNEL32.dll", "LoadLibraryA"));
	v_LoadLibraryW = reinterpret_cast<PFN_LoadLibraryW>(
		DetourFindFunction("KERNEL32.dll", "LoadLibraryW"));
	v_LoadLibraryExA = reinterpret_cast<PFN_LoadLibraryExA>(
		DetourFindFunction("KERNEL32.dll", "LoadLibraryExA"));
	v_LoadLibraryExW = reinterpret_cast<PFN_LoadLibraryExW>(
		DetourFindFunction("KERNEL32.dll", "LoadLibraryExW"));

	if (!v_LoadLibraryA || !v_LoadLibraryW || !v_LoadLibraryExA || !v_LoadLibraryExW)
	{
		Warning(eDLL_T::ENGINE,
			"[IGO-GUARD] LoadLibrary resolve failed (A=%p W=%p ExA=%p ExW=%p)\n",
			(void*)v_LoadLibraryA, (void*)v_LoadLibraryW,
			(void*)v_LoadLibraryExA, (void*)v_LoadLibraryExW);
		return;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(LPVOID&)v_LoadLibraryA, (PBYTE)Hook_LoadLibraryA);
	DetourAttach(&(LPVOID&)v_LoadLibraryW, (PBYTE)Hook_LoadLibraryW);
	DetourAttach(&(LPVOID&)v_LoadLibraryExA, (PBYTE)Hook_LoadLibraryExA);
	DetourAttach(&(LPVOID&)v_LoadLibraryExW, (PBYTE)Hook_LoadLibraryExW);
	const LONG err = DetourTransactionCommit();
	if (err != NO_ERROR)
	{
		Warning(eDLL_T::ENGINE,
			"[IGO-GUARD] LoadLibrary detour commit failed (err=%ld)\n", (long)err);
		return;
	}

	s_loadHookInstalled = true;
	SDK_Log("[IGO-GUARD] LoadLibrary A/W/ExA/ExW hooked\n");
}

static void IgoGuard_InstallStartIgoHook(void)
{
	const CMemory mem = Module_FindPattern(g_GameDll,
		"40 55 56 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 C7 44 24");
	if (!mem.GetPtr())
	{
		Warning(eDLL_T::ENGINE,
			"[IGO-GUARD] StartIGO pattern unresolved -- LoadLibrary block still active\n");
		return;
	}

	v_OriginSDK_StartIGO = mem.RCast<PFN_StartIGO>();
	s_startIgoAddr = reinterpret_cast<void*>(mem.GetPtr());

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(LPVOID&)v_OriginSDK_StartIGO, (PBYTE)Hook_OriginSDK_StartIGO);
	const LONG err = DetourTransactionCommit();
	if (err != NO_ERROR)
	{
		Warning(eDLL_T::ENGINE,
			"[IGO-GUARD] StartIGO detour commit failed (err=%ld) @ %p\n",
			(long)err, s_startIgoAddr);
		v_OriginSDK_StartIGO = nullptr;
		s_startIgoAddr = nullptr;
		return;
	}

	s_startIgoHooked = true;
	SDK_Log("[IGO-GUARD] StartIGO hooked @ %p\n", s_startIgoAddr);
}

static void OriginIgoGuardStatus_f(const CCommand& args)
{
	(void)args;
	const bool optedOut = CommandLine()
		&& CommandLine()->CheckParm("-sdk_allow_igo") != nullptr;
	Msg(eDLL_T::ENGINE,
		"[IGO-GUARD] active=%d (-sdk_allow_igo=%d) loadHook=%d startIgo=%d @ %p\n",
		IgoGuard_IsRequested() ? 1 : 0, optedOut ? 1 : 0,
		s_loadHookInstalled ? 1 : 0, s_startIgoHooked ? 1 : 0, s_startIgoAddr);
	Msg(eDLL_T::ENGINE,
		"[IGO-GUARD] startIgo_hits=%ld blocked_loads=%ld first='%s'\n",
		(long)s_startIgoHits, (long)s_blockedLoads,
		s_firstBlocked[0] ? s_firstBlocked : "(none)");
}

static ConCommand origin_igo_guard_status(
	"origin_igo_guard_status",
	OriginIgoGuardStatus_f,
	"Print EA overlay guard status (StartIGO hook, blocked IGO64 loads).",
	FCVAR_RELEASE);

void Origin_InstallIgoGuard(void)
{
	if (!IgoGuard_IsRequested())
	{
		SDK_Log("[IGO-GUARD] inactive (-sdk_allow_igo) -- overlay left stock\n");
		return;
	}

	SDK_Log("[IGO-GUARD] active -- blocking IGO64 and no-op StartIGO\n");
	IgoGuard_InstallLoadHooks();
	IgoGuard_InstallStartIgoHook();
}
