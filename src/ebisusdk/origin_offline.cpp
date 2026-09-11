//=============================================================================//
//
// Purpose: offline platform guard -- opt-in byte patches that keep the platform
// client from launching, plus an always-on CreateProcess watchdog.
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/commandline.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "origin_offline.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <cstdint>

void SDK_Log(const char* fmt, ...);

//--- state -------------------------------------------------------------------
static bool s_patchDApplied = false;
static bool s_patchEApplied = false;
static bool s_patchA1Applied = false;
static bool s_patchA2Applied = false;
static bool s_patchCApplied = false;
static void* s_patchDAddr = nullptr;
static void* s_patchEAddr = nullptr;
static void* s_patchA1Addr = nullptr;
static void* s_patchA2Addr = nullptr;
static void* s_patchCAddr = nullptr;
// Cross-check only (not a write): call-graph anchor for patch C.
static void* s_crossCheckAnchor = nullptr;
static void* s_crossCheckTarget = nullptr;
// 0=unavailable, 1=passed, -1=mismatch fail-closed
static int s_crossCheckResult = 0;

static volatile LONG s_spawnCount = 0;
static volatile LONG s_platformSpawnCount = 0;
static char s_firstPlatformName[512] = {};
static volatile LONG s_firstPlatformNameSet = 0;

//--- gating ------------------------------------------------------------------
static bool OfflineGuard_IsRequested(void)
{
	// -offline only, and on by default there: an -offline launch that still brings the
	// platform client up is not offline. Deliberately NOT keyed on IsOriginDisabled --
	// -noorigin keeps its narrower meaning (fixed identity, poll stubbed) and does not
	// pull in binary patches. -sdk_allow_platform_launch restores stock behaviour for
	// an A/B. Every site is byte-validated and writes all-or-nothing, so a build whose
	// bytes moved degrades to stock rather than misfiring.
	static const bool s_requested = []() -> bool
	{
		if (!CommandLine())
			return false;
		if (!CommandLine()->CheckParm("-offline"))
			return false;
		return CommandLine()->CheckParm("-sdk_allow_platform_launch") == nullptr;
	}();
	return s_requested;
}

//--- helpers -----------------------------------------------------------------
static bool OfflineGuard_WriteBytes(uint8_t* site, const uint8_t* bytes, size_t len)
{
	if (!site || !bytes || len == 0)
		return false;

	DWORD oldProt = 0;
	if (!VirtualProtect(site, static_cast<SIZE_T>(len), PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[OFFLINE-GUARD] VirtualProtect failed @ %p (len=%zu gle=%lu) -- write skipped\n",
			(void*)site, len, GetLastError());
		return false;
	}

	memcpy(site, bytes, len);
	VirtualProtect(site, static_cast<SIZE_T>(len), oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), site, static_cast<SIZE_T>(len));
	return true;
}

static void OfflineGuard_BuildDisplayA(LPCSTR lpApplicationName, LPCSTR lpCommandLine,
	char* out, size_t cap)
{
	if (!out || cap == 0)
		return;
	out[0] = '\0';

	const char* src = lpApplicationName ? lpApplicationName : lpCommandLine;
	if (!src)
		return;

	size_t i = 0;
	for (; i + 1 < cap && src[i]; ++i)
		out[i] = src[i];
	out[i] = '\0';
}

static void OfflineGuard_BuildDisplayW(LPCWSTR lpApplicationName, LPCWSTR lpCommandLine,
	char* out, size_t cap)
{
	if (!out || cap == 0)
		return;
	out[0] = '\0';

	const wchar_t* src = lpApplicationName ? lpApplicationName : lpCommandLine;
	if (!src)
		return;

	// Fixed buffer only -- never allocate. Truncate + force NUL.
	const int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, out, static_cast<int>(cap),
		nullptr, nullptr);
	if (n <= 0)
	{
		// Buffer too small or convert failed: best-effort partial fill.
		const int partial = WideCharToMultiByte(CP_UTF8, 0, src, -1, out,
			static_cast<int>(cap > 0 ? cap - 1 : 0), nullptr, nullptr);
		if (partial <= 0)
			out[0] = '\0';
		else
			out[cap - 1] = '\0';
	}
	else
	{
		out[cap - 1] = '\0';
	}
}

static bool OfflineGuard_IsPlatformClientName(const char* display)
{
	if (!display || !display[0])
		return false;

	// Substrings of platform client process names (case-insensitive).
	static const char* const kSubs[] = {
		"EADesktop",
		"EALaunchHelper",
		"EABackgroundService",
		"EACoreServer",
		"EALocalHostSvc",
		"Origin.exe",
		"OriginClient",
		"EAAppLauncher",
	};

	for (size_t i = 0; i < SDK_ARRAYSIZE(kSubs); ++i)
	{
		if (V_stristr(display, kSubs[i]))
			return true;
	}
	return false;
}

static void OfflineGuard_RecordPlatformSpawn(const char* display)
{
	InterlockedIncrement(&s_platformSpawnCount);

	if (InterlockedCompareExchange(&s_firstPlatformNameSet, 1, 0) == 0)
	{
		size_t i = 0;
		const char* src = display ? display : "";
		for (; i + 1 < sizeof(s_firstPlatformName) && src[i]; ++i)
			s_firstPlatformName[i] = src[i];
		s_firstPlatformName[i] = '\0';
	}
}

//--- CreateProcess watchdog --------------------------------------------------
typedef BOOL(WINAPI* PFN_CreateProcessA)(
	LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
	LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
typedef BOOL(WINAPI* PFN_CreateProcessW)(
	LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
	LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

static PFN_CreateProcessA v_CreateProcessA = nullptr;
static PFN_CreateProcessW v_CreateProcessW = nullptr;

static BOOL WINAPI Hook_CreateProcessA(
	LPCSTR lpApplicationName,
	LPSTR lpCommandLine,
	LPSECURITY_ATTRIBUTES lpProcessAttributes,
	LPSECURITY_ATTRIBUTES lpThreadAttributes,
	BOOL bInheritHandles,
	DWORD dwCreationFlags,
	LPVOID lpEnvironment,
	LPCSTR lpCurrentDirectory,
	LPSTARTUPINFOA lpStartupInfo,
	LPPROCESS_INFORMATION lpProcessInformation)
{
	char display[512];
	// lpCommandLine is caller-owned writable memory -- read only, never write.
	OfflineGuard_BuildDisplayA(lpApplicationName, lpCommandLine, display, sizeof(display));

	InterlockedIncrement(&s_spawnCount);

	if (OfflineGuard_IsPlatformClientName(display))
	{
		OfflineGuard_RecordPlatformSpawn(display);
		Warning(eDLL_T::ENGINE,
			"[OFFLINE-GUARD] platform client spawn observed (A): %s\n", display);

		if (OfflineGuard_IsRequested())
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] platform client launch BLOCKED (A): %s\n", display);
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}

	if (!v_CreateProcessA)
	{
		SetLastError(ERROR_PROC_NOT_FOUND);
		return FALSE;
	}

	return v_CreateProcessA(
		lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
		bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory,
		lpStartupInfo, lpProcessInformation);
}

static BOOL WINAPI Hook_CreateProcessW(
	LPCWSTR lpApplicationName,
	LPWSTR lpCommandLine,
	LPSECURITY_ATTRIBUTES lpProcessAttributes,
	LPSECURITY_ATTRIBUTES lpThreadAttributes,
	BOOL bInheritHandles,
	DWORD dwCreationFlags,
	LPVOID lpEnvironment,
	LPCWSTR lpCurrentDirectory,
	LPSTARTUPINFOW lpStartupInfo,
	LPPROCESS_INFORMATION lpProcessInformation)
{
	char display[512];
	OfflineGuard_BuildDisplayW(lpApplicationName, lpCommandLine, display, sizeof(display));

	InterlockedIncrement(&s_spawnCount);

	if (OfflineGuard_IsPlatformClientName(display))
	{
		OfflineGuard_RecordPlatformSpawn(display);
		Warning(eDLL_T::ENGINE,
			"[OFFLINE-GUARD] platform client spawn observed (W): %s\n", display);

		if (OfflineGuard_IsRequested())
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] platform client launch BLOCKED (W): %s\n", display);
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}

	if (!v_CreateProcessW)
	{
		SetLastError(ERROR_PROC_NOT_FOUND);
		return FALSE;
	}

	return v_CreateProcessW(
		lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
		bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory,
		lpStartupInfo, lpProcessInformation);
}

static void OfflineGuard_InstallWatchdog(void)
{
	v_CreateProcessA = reinterpret_cast<PFN_CreateProcessA>(
		DetourFindFunction("KERNEL32.dll", "CreateProcessA"));
	v_CreateProcessW = reinterpret_cast<PFN_CreateProcessW>(
		DetourFindFunction("KERNEL32.dll", "CreateProcessW"));

	if (!v_CreateProcessA || !v_CreateProcessW)
	{
		Warning(eDLL_T::ENGINE,
			"[OFFLINE-GUARD] CreateProcess resolve failed (A=%p W=%p) -- watchdog not installed\n",
			(void*)v_CreateProcessA, (void*)v_CreateProcessW);
		return;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(LPVOID&)v_CreateProcessA, (PBYTE)Hook_CreateProcessA);
	DetourAttach(&(LPVOID&)v_CreateProcessW, (PBYTE)Hook_CreateProcessW);
	const LONG err = DetourTransactionCommit();
	if (err != NO_ERROR)
	{
		Warning(eDLL_T::ENGINE,
			"[OFFLINE-GUARD] CreateProcess detour commit failed (err=%ld) -- watchdog inactive\n",
			(long)err);
		return;
	}

	SDK_Log("[OFFLINE-GUARD] CreateProcess watchdog installed (A+W)\n");
}

//--- byte patches (apply D/E/A1/A2/C or none) --------------------------------
static void OfflineGuard_ApplyPatches(void)
{
	// Patch D: IsOriginInstalled false returns ORIGIN_ERROR_CORE_NOT_INSTALLED
	// (0xA0020008). LauncherMain fatals on that. A1/A2 never run. Invert the
	// jnz so a missing HKLM Origin\ClientPath falls through to FindProcessOrigin.
	const CMemory memD = Module_FindPattern(g_GameDll,
		"E8 ?? ?? ?? ?? 84 C0 75 ?? B9 08 00 02 A0");
	uint8_t* siteD = nullptr;
	if (!memD.GetPtr())
	{
		Warning(eDLL_T::ENGINE,
			"[OFFLINE-GUARD] patch D pattern unresolved -- guard inactive, "
			"EA App install check still fatal\n");
	}
	else
	{
		siteD = reinterpret_cast<uint8_t*>(memD.GetPtr()) + 7;
		if (siteD[0] != 0x75)
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch D site byte mismatch @ %p (got 0x%02X, expected 0x75) -- "
				"guard inactive, EA App install check still fatal\n",
				(void*)siteD, siteD[0]);
			siteD = nullptr;
		}
	}

	// Patch A: one match yields E, A1, and A2.
	// E  = match+2  NOP the jnz that skips to LSX when EA App is already running.
	// A1 = match+14 NOP the jump that skips the no-launch branch.
	// A2 = match+35 rewrite the branch return from 0xA0020000 to 0x42010001
	//      (engine treats that code as platform-start success).
	const CMemory memA = Module_FindPattern(g_GameDll,
		"85 C0 75 ?? 8B 86 68 04 00 00 0F BA E0 09 73 ?? 0F BA E0 0C 73 ?? "
		"45 33 C0 B2 01 48 8B CE E8 ?? ?? ?? ?? 41 BC 00 00 02 A0");
	uint8_t* siteE = nullptr;
	uint8_t* siteA1 = nullptr;
	uint8_t* siteA2 = nullptr;
	if (!memA.GetPtr())
	{
		Warning(eDLL_T::ENGINE,
			"[OFFLINE-GUARD] patch A pattern unresolved -- guard inactive, "
			"platform client may still be launched\n");
	}
	else
	{
		uint8_t* const matchA = reinterpret_cast<uint8_t*>(memA.GetPtr());

		siteE = matchA + 2;
		if (siteE[0] != 0x75)
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch E site byte mismatch @ %p (got 0x%02X, expected 0x75) -- "
				"guard inactive, running EA App still handshakes\n",
				(void*)siteE, siteE[0]);
			siteE = nullptr;
		}

		siteA1 = matchA + 14;
		if (siteA1[0] != 0x73)
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch A1 site byte mismatch @ %p (got 0x%02X, expected 0x73) -- "
				"guard inactive, platform client may still be launched\n",
				(void*)siteA1, siteA1[0]);
			siteA1 = nullptr;
		}

		// Expected: mov r12d, 0A0020000h
		static const uint8_t kA2Expect[6] = { 0x41, 0xBC, 0x00, 0x00, 0x02, 0xA0 };
		siteA2 = matchA + 35;
		if (memcmp(siteA2, kA2Expect, sizeof(kA2Expect)) != 0)
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch A2 site byte mismatch @ %p "
				"(got %02X %02X %02X %02X %02X %02X, expected 41 BC 00 00 02 A0) -- "
				"guard inactive, platform client may still be launched\n",
				(void*)siteA2,
				siteA2[0], siteA2[1], siteA2[2], siteA2[3], siteA2[4], siteA2[5]);
			siteA2 = nullptr;
		}
	}

	// Patch C: force the platform SDK "singleton ready" predicate false so every
	// C-API entry takes its documented not-initialized path (no object deref).
	// Body is cmp qword [rip+disp],0 / setne al / ret -- leave the disp alone.
	// Pattern alone is a generic compiler idiom; bind via call-graph cross-check.
	const CMemory memC = Module_FindPattern(g_GameDll,
		"48 83 3D ?? ?? ?? ?? 00 0F 95 C0 C3");
	uint8_t* siteC = nullptr;
	if (!memC.GetPtr())
	{
		Warning(eDLL_T::ENGINE,
			"[OFFLINE-GUARD] patch C pattern unresolved -- guard inactive, "
			"platform client may still be launched\n");
	}
	else
	{
		siteC = reinterpret_cast<uint8_t*>(memC.GetPtr());
		if (siteC[0] != 0x48 || siteC[1] != 0x83 || siteC[2] != 0x3D
			|| siteC[7] != 0x00 || siteC[8] != 0x0F || siteC[9] != 0x95
			|| siteC[10] != 0xC0 || siteC[11] != 0xC3)
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch C site byte mismatch @ %p "
				"(got %02X %02X %02X .. %02X %02X %02X %02X %02X) -- "
				"guard inactive, platform client may still be launched\n",
				(void*)siteC,
				siteC[0], siteC[1], siteC[2],
				siteC[7], siteC[8], siteC[9], siteC[10], siteC[11]);
			siteC = nullptr;
		}
	}

	// Anchor: a C-API entry that calls the ready-predicate at fixed offset +0x21
	// (5-byte E8 rel32). Used only to confirm patch C hit that predicate, not a
	// sibling cmp/setne/ret elsewhere. Not a write site.
	const CMemory memAnchor = Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 30 48 8B DA 48 8B F9 48 8D 15 ?? ?? ?? ?? "
		"B9 00 00 00 03 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 1D E8 ?? ?? ?? ?? "
		"48 8B C8 4C 8B C3 48 8B D7");
	bool crossCheckFailClosed = false;
	s_crossCheckResult = 0;
	s_crossCheckAnchor = nullptr;
	s_crossCheckTarget = nullptr;

	if (siteC)
	{
		if (!memAnchor.GetPtr())
		{
			// Anchor missing: still allow pattern-only C, but make it visible.
			s_crossCheckResult = 0;
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] cross-check unavailable (anchor pattern unresolved) -- "
				"proceeding on patch C pattern alone\n");
		}
		else
		{
			uint8_t* const anchor = reinterpret_cast<uint8_t*>(memAnchor.GetPtr());
			uint8_t* const callsite = anchor + 0x21;
			s_crossCheckAnchor = anchor;

			if (callsite[0] != 0xE8)
			{
				crossCheckFailClosed = true;
				s_crossCheckResult = -1;
				Warning(eDLL_T::ENGINE,
					"[OFFLINE-GUARD] cross-check failed: expected E8 at anchor+0x21 @ %p "
					"(got 0x%02X) -- nothing written; guard inactive, "
					"platform client may still be launched\n",
					(void*)callsite, callsite[0]);
			}
			else
			{
				// target = callsite + 5 + signed rel32 (memcpy, no misaligned load)
				int32_t rel32 = 0;
				memcpy(&rel32, callsite + 1, sizeof(rel32));
				uint8_t* const target = callsite + 5 + rel32;
				s_crossCheckTarget = target;

				if (target != siteC)
				{
					// Idiom matched the wrong function -- fail closed.
					crossCheckFailClosed = true;
					s_crossCheckResult = -1;
					Warning(eDLL_T::ENGINE,
						"[OFFLINE-GUARD] cross-check failed: predicate call target %p "
						"!= patch C site %p -- wrong function; nothing written; "
						"guard inactive, platform client may still be launched\n",
						(void*)target, (void*)siteC);
				}
				else
				{
					s_crossCheckResult = 1;
					SDK_Log("[OFFLINE-GUARD] cross-check passed: anchor %p +0x21 -> "
						"predicate %p (matches patch C)\n",
						(void*)anchor, (void*)siteC);
				}
			}
		}
	}

	// Written sites: D, E, A1, A2, C. Cross-check mismatch aborts all writes.
	if (!siteD || !siteE || !siteA1 || !siteA2 || !siteC || crossCheckFailClosed)
	{
		if (!siteD)
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch D failed validation -- neither patch applied; "
				"guard inactive, EA App install check still fatal\n");
		}
		if (!siteA1 || !siteA2 || !siteE)
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch A/E failed validation (E=%p A1=%p A2=%p) -- neither patch applied; "
				"guard inactive, platform client may still be launched\n",
				(void*)siteE, (void*)siteA1, (void*)siteA2);
		}
		if (!siteC && !crossCheckFailClosed)
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch C failed validation -- neither patch applied; "
				"guard inactive, platform client may still be launched\n");
		}
		return;
	}

	// D: jnz -> jmp (keep rel8). Missing Origin ClientPath continues into FindProcessOrigin.
	static const uint8_t kPatchD[] = { 0xEB };
	// E: NOP the short jnz that skips to LSX when EA App is already running.
	static const uint8_t kPatchE[] = { 0x90, 0x90 };
	// A1: NOP the short conditional jump (2 bytes).
	static const uint8_t kPatchA1[] = { 0x90, 0x90 };
	// A2: mov r12d, 42010001h  (engine success code instead of 0xA0020000).
	static const uint8_t kPatchA2[] = { 0x41, 0xBC, 0x01, 0x00, 0x01, 0x42 };
	// C: xor eax,eax ; ret  (always "not initialized").
	static const uint8_t kPatchC[] = { 0x33, 0xC0, 0xC3 };

	const uint8_t origD[1] = { siteD[0] };
	const uint8_t origE[2] = { siteE[0], siteE[1] };
	const uint8_t origA1[2] = { siteA1[0], siteA1[1] };
	const uint8_t origA2[6] = {
		siteA2[0], siteA2[1], siteA2[2], siteA2[3], siteA2[4], siteA2[5]
	};

	auto restoreWritten = [&](int n) -> bool
	{
		bool ok = true;
		if (n >= 5)
			ok = OfflineGuard_WriteBytes(siteA2, origA2, sizeof(origA2)) && ok;
		if (n >= 4)
			ok = OfflineGuard_WriteBytes(siteA1, origA1, sizeof(origA1)) && ok;
		if (n >= 3)
			ok = OfflineGuard_WriteBytes(siteE, origE, sizeof(origE)) && ok;
		if (n >= 2)
			ok = OfflineGuard_WriteBytes(siteD, origD, sizeof(origD)) && ok;
		return ok;
	};

	if (!OfflineGuard_WriteBytes(siteD, kPatchD, sizeof(kPatchD)))
	{
		Warning(eDLL_T::ENGINE,
			"[OFFLINE-GUARD] patch D write failed -- neither patch applied; "
			"guard inactive, EA App install check still fatal\n");
		return;
	}

	if (!OfflineGuard_WriteBytes(siteE, kPatchE, sizeof(kPatchE)))
	{
		if (!restoreWritten(2))
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch E write failed and restore incomplete -- "
				"partial apply; guard inactive\n");
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch E write failed -- D restored; neither applied; "
				"guard inactive\n");
		}
		return;
	}

	if (!OfflineGuard_WriteBytes(siteA1, kPatchA1, sizeof(kPatchA1)))
	{
		if (!restoreWritten(3))
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch A1 write failed and restore incomplete -- "
				"partial apply; guard inactive\n");
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch A1 write failed -- D/E restored; neither applied; "
				"guard inactive\n");
		}
		return;
	}

	if (!OfflineGuard_WriteBytes(siteA2, kPatchA2, sizeof(kPatchA2)))
	{
		if (!restoreWritten(4))
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch A2 write failed and restore incomplete -- "
				"partial apply; guard inactive\n");
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch A2 write failed -- prior sites restored; neither applied; "
				"guard inactive\n");
		}
		return;
	}

	if (!OfflineGuard_WriteBytes(siteC, kPatchC, sizeof(kPatchC)))
	{
		if (!restoreWritten(5))
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch C write failed and restore incomplete -- "
				"partial apply; guard inactive\n");
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[OFFLINE-GUARD] patch C write failed -- prior sites restored; neither applied; "
				"guard inactive\n");
		}
		return;
	}

	s_patchDApplied = true;
	s_patchEApplied = true;
	s_patchA1Applied = true;
	s_patchA2Applied = true;
	s_patchCApplied = true;
	s_patchDAddr = siteD;
	s_patchEAddr = siteE;
	s_patchA1Addr = siteA1;
	s_patchA2Addr = siteA2;
	s_patchCAddr = siteC;

	SDK_Log("[OFFLINE-GUARD] patch D applied @ %p (skip ORIGIN_ERROR_CORE_NOT_INSTALLED)\n", (void*)siteD);
	SDK_Log("[OFFLINE-GUARD] patch E applied @ %p (NOP running-EA-App handshake skip)\n", (void*)siteE);
	SDK_Log("[OFFLINE-GUARD] patch A1 applied @ %p (NOP launch-permit branch)\n", (void*)siteA1);
	SDK_Log("[OFFLINE-GUARD] patch A2 applied @ %p (return 0x42010001)\n", (void*)siteA2);
	SDK_Log("[OFFLINE-GUARD] patch C applied @ %p (ready-predicate -> always false)\n", (void*)siteC);
}

//--- status command ----------------------------------------------------------
static void OriginOfflineGuardStatus_f(const CCommand& args)
{
	(void)args;

	const bool offline = CommandLine()
		&& CommandLine()->CheckParm("-offline") != nullptr;
	const bool optedOut = CommandLine()
		&& CommandLine()->CheckParm("-sdk_allow_platform_launch") != nullptr;
	const bool requested = OfflineGuard_IsRequested();

	const char* crossLabel = "unavailable";
	if (s_crossCheckResult > 0)
		crossLabel = "passed";
	else if (s_crossCheckResult < 0)
		crossLabel = "FAIL";

	Msg(eDLL_T::ENGINE,
		"[OFFLINE-GUARD] active=%d (-offline=%d -sdk_allow_platform_launch=%d)\n",
		requested ? 1 : 0, offline ? 1 : 0, optedOut ? 1 : 0);
	Msg(eDLL_T::ENGINE,
		"[OFFLINE-GUARD] patchD=%d @ %p  patchE=%d @ %p\n",
		s_patchDApplied ? 1 : 0, s_patchDAddr,
		s_patchEApplied ? 1 : 0, s_patchEAddr);
	Msg(eDLL_T::ENGINE,
		"[OFFLINE-GUARD] patchA1=%d @ %p  patchA2=%d @ %p  patchC=%d @ %p\n",
		s_patchA1Applied ? 1 : 0, s_patchA1Addr,
		s_patchA2Applied ? 1 : 0, s_patchA2Addr,
		s_patchCApplied ? 1 : 0, s_patchCAddr);
	Msg(eDLL_T::ENGINE,
		"[OFFLINE-GUARD] cross-check=%s anchor=%p target=%p\n",
		crossLabel, s_crossCheckAnchor, s_crossCheckTarget);
	Msg(eDLL_T::ENGINE,
		"[OFFLINE-GUARD] spawns=%ld platform_spawns=%ld first='%s'\n",
		(long)s_spawnCount, (long)s_platformSpawnCount,
		s_firstPlatformName[0] ? s_firstPlatformName : "(none)");
}

static ConCommand origin_offline_guard_status(
	"origin_offline_guard_status",
	OriginOfflineGuardStatus_f,
	"Print offline platform guard status (request, patches, spawn counters).",
	FCVAR_RELEASE);

//-----------------------------------------------------------------------------
// Purpose: install watchdog (always) and opt-in byte patches (when requested)
//-----------------------------------------------------------------------------
void Origin_InstallOfflineGuard(void)
{
	OfflineGuard_InstallWatchdog();

	if (!OfflineGuard_IsRequested())
	{
		SDK_Log("[OFFLINE-GUARD] inactive (%s) -- patches skipped\n",
			(CommandLine() && CommandLine()->CheckParm("-offline"))
				? "-sdk_allow_platform_launch given"
				: "not an -offline launch");
		return;
	}

	SDK_Log("[OFFLINE-GUARD] active -- resolving platform patches\n");
	OfflineGuard_ApplyPatches();
}
