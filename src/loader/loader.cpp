//===========================================================================//
//
// Purpose: command-line staging loader for the S21 SDK worker DLL.
//
//===========================================================================//
#include "loader.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>

#include "thirdparty/detours/include/detours.h"

#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

//-----------------------------------------------------------------------------
static const char* const kTracePath = "platform/logs/loader_trace.txt";

// Worker floor: real client/server DLLs are multi-MB; reject stubs / empties.
static const ULONGLONG kWorkerDllMinBytes = 64ull * 1024ull;

static void Loader_Trace(const char* fmt, ...)
{
	FILE* fp = nullptr;
	fopen_s(&fp, kTracePath, "a");
	if (!fp)
		return;

	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	const double t = static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
	fprintf(fp, "[%12.6f] ", t);

	va_list args;
	va_start(args, fmt);
	vfprintf(fp, fmt, args);
	va_end(args);

	fflush(fp);
	fclose(fp);
}

//-----------------------------------------------------------------------------
static const char* const kClientSdkModule = "client.dll";
static const char* const kServerSdkModule = "server.dll";
static const wchar_t* const kClientSdkModuleW = L"client.dll";
static const wchar_t* const kServerSdkModuleW = L"server.dll";

// Already-staged predicates: a real product flag, not a dummy handshake token.
// Cold double-click lacks these; CreateProcessW appends extras and the child
// sees the flag, so the relaunch cannot loop.
static const wchar_t* const kClientStagedFlag = L"-nodiscord";
static const wchar_t* const kDediStagedFlag = L"-dedicated";
static const wchar_t* const kWorkerShaArg = L"-r5sdk_worker_sha256";

// Live-client default flags (double-click / cold start).
// -fnf selects /Respawn/Apex_fnf (retargeted to Apex_r5f below). Without it
// CheckParm misses and videoconfig/profile land in live /Respawn/Apex.
static const wchar_t* const kClientExtraArgs =
	L" -nodiscord -forceSendTable -novid -windowed -allowmultiple -fnf";
static const wchar_t* const kDediExtraArgs =
	L" -dedicated +sv_allowSendTableTransmitToClients 1 +stringtable_compress 1";
static const wchar_t* const kDediSignonArgsW =
	L" +sv_allowSendTableTransmitToClients 1 +stringtable_compress 1";
static const char* const kDediSignonArgsA =
	" +sv_allowSendTableTransmitToClients 1 +stringtable_compress 1";
static const wchar_t* const kFnfArg = L"-fnf";
static const wchar_t* const kFnfExtraOnly = L" -fnf";

static char s_fnfCmdA[4096];
static wchar_t s_fnfCmdW[4096];
static LPSTR (WINAPI* v_GetCommandLineA)(void) = nullptr;
static LPWSTR (WINAPI* v_GetCommandLineW)(void) = nullptr;

static const uintptr_t kClientLauncherMainRvaDx11 = 0x501C90;
static const uintptr_t kClientLauncherMainRvaDx12 = 0x532180;
static const uintptr_t kDediLauncherMainRva = 0x44C240;

static int (*v_LauncherMain)(HINSTANCE, HINSTANCE, LPSTR, int) = nullptr;

//-----------------------------------------------------------------------------
static void Loader_FatalError(const char* const pszMessage)
{
	Loader_Trace("FATAL: %s\n", pszMessage);
	MessageBoxA(NULL, pszMessage, "R5Flowstate loader", MB_ICONERROR | MB_OK);
	ExitProcess(1);
}

static bool Loader_WideStrContains(const wchar_t* haystack, const wchar_t* needle)
{
	if (!haystack || !needle)
		return false;
	return wcsstr(haystack, needle) != nullptr;
}

static bool Loader_IsDedicatedExe(void)
{
	wchar_t path[MAX_PATH];
	if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0)
		return false;
	const wchar_t* leaf = path;
	for (const wchar_t* p = path; *p; ++p)
	{
		if (*p == L'\\' || *p == L'/')
			leaf = p + 1;
	}
	return _wcsicmp(leaf, L"r5apex_ds.exe") == 0;
}

static bool Loader_IsDx12Exe(void)
{
	wchar_t path[MAX_PATH];
	if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0)
		return false;
	return wcsstr(path, L"dx12") != nullptr;
}

//-----------------------------------------------------------------------------
// Host directory (trailing slash), fully canonicalized. Role selection still
// keys off the host exe leaf (r5apex_ds.exe vs r5apex.exe), not this path.
//-----------------------------------------------------------------------------
static bool Loader_GetHostDirW(wchar_t* pszOut, const DWORD cchOut)
{
	if (!pszOut || cchOut < 4)
		return false;

	wchar_t exePath[MAX_PATH];
	const DWORD pathLen = GetModuleFileNameW(NULL, exePath, MAX_PATH);
	if (pathLen == 0 || pathLen >= MAX_PATH)
		return false;

	wchar_t* leaf = exePath;
	for (wchar_t* p = exePath; *p; ++p)
	{
		if (*p == L'\\' || *p == L'/')
			leaf = p + 1;
	}
	*leaf = L'\0';

	wchar_t full[MAX_PATH];
	const DWORD fullLen = GetFullPathNameW(exePath, MAX_PATH, full, nullptr);
	if (fullLen == 0 || fullLen >= MAX_PATH)
		return false;

	const size_t n = wcslen(full);
	const bool bHasSlash = (n > 0 && (full[n - 1] == L'\\' || full[n - 1] == L'/'));
	if (n + (bHasSlash ? 0u : 1u) + 1u > cchOut)
		return false;

	wmemcpy(pszOut, full, n);
	if (!bHasSlash)
	{
		pszOut[n] = L'\\';
		pszOut[n + 1] = L'\0';
	}
	else
	{
		pszOut[n] = L'\0';
	}
	return true;
}

static bool Loader_PathIsUnderHostDir(const wchar_t* pszFullPath, const wchar_t* pszHostDir)
{
	if (!pszFullPath || !pszHostDir || !*pszFullPath || !*pszHostDir)
		return false;

	const size_t hostLen = wcslen(pszHostDir);
	if (_wcsnicmp(pszFullPath, pszHostDir, hostLen) != 0)
		return false;

	// Require a real file under host, not a sibling that only shares a prefix
	// (hostDir always ends with slash).
	return pszFullPath[hostLen] != L'\0';
}

// Absolute worker path next to host. Rejects path escape after canonicalize.
static bool Loader_ResolveWorkerPathW(
	const bool bDedicated,
	wchar_t* pszOut,
	const DWORD cchOut,
	wchar_t* pszHostDirOut,
	const DWORD cchHostDir)
{
	wchar_t hostDir[MAX_PATH];
	if (!Loader_GetHostDirW(hostDir, MAX_PATH))
		return false;

	const wchar_t* const leaf = bDedicated ? kServerSdkModuleW : kClientSdkModuleW;
	wchar_t joined[MAX_PATH];
	if (_snwprintf_s(joined, _TRUNCATE, L"%s%s", hostDir, leaf) < 0)
		return false;

	wchar_t full[MAX_PATH];
	const DWORD fullLen = GetFullPathNameW(joined, MAX_PATH, full, nullptr);
	if (fullLen == 0 || fullLen >= MAX_PATH)
		return false;

	if (!Loader_PathIsUnderHostDir(full, hostDir))
	{
		Loader_Trace("[SEC-LOADER] REJECT path escapes host dir: %ls (host=%ls)\n",
			full, hostDir);
		return false;
	}

	if (wcslen(full) + 1 > cchOut)
		return false;
	wmemcpy(pszOut, full, wcslen(full) + 1);

	if (pszHostDirOut && cchHostDir)
	{
		if (wcslen(hostDir) + 1 > cchHostDir)
			return false;
		wmemcpy(pszHostDirOut, hostDir, wcslen(hostDir) + 1);
	}
	return true;
}

static bool Loader_FileExistsSize(const wchar_t* pszPath, ULONGLONG* pSizeOut)
{
	if (pSizeOut)
		*pSizeOut = 0;

	const DWORD attrs = GetFileAttributesW(pszPath);
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
		return false;

	WIN32_FILE_ATTRIBUTE_DATA fad = {};
	if (!GetFileAttributesExW(pszPath, GetFileExInfoStandard, &fad))
		return false;

	ULARGE_INTEGER sz;
	sz.HighPart = fad.nFileSizeHigh;
	sz.LowPart = fad.nFileSizeLow;
	if (pSizeOut)
		*pSizeOut = sz.QuadPart;
	return true;
}

// Basic PE sanity: MZ + PE + AMD64. Fail-closed on short / corrupt image.
static bool Loader_CheckPeSanity(const wchar_t* pszPath)
{
	HANDLE hFile = CreateFileW(
		pszPath,
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	IMAGE_DOS_HEADER dos = {};
	DWORD nRead = 0;
	if (!ReadFile(hFile, &dos, sizeof(dos), &nRead, nullptr) || nRead != sizeof(dos))
	{
		CloseHandle(hFile);
		return false;
	}
	if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < (LONG)sizeof(IMAGE_DOS_HEADER))
	{
		CloseHandle(hFile);
		return false;
	}

	if (SetFilePointer(hFile, dos.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
	{
		CloseHandle(hFile);
		return false;
	}

	IMAGE_NT_HEADERS64 nt = {};
	if (!ReadFile(hFile, &nt, sizeof(nt), &nRead, nullptr) || nRead != sizeof(nt))
	{
		CloseHandle(hFile);
		return false;
	}
	CloseHandle(hFile);

	if (nt.Signature != IMAGE_NT_SIGNATURE)
		return false;
	if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
		return false;
	if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		return false;

	return true;
}

static int Loader_HexNibble(const char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

// Accept 64 hex chars; ignore spaces. Returns false if non-empty but malformed.
static bool Loader_ParseSha256Hex(const char* pszHex, BYTE out[32], bool* pbEmpty)
{
	if (pbEmpty)
		*pbEmpty = true;
	if (!pszHex)
		return true;

	BYTE tmp[32];
	int nNibbles = 0;
	for (const char* p = pszHex; *p; ++p)
	{
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			continue;
		const int nib = Loader_HexNibble(*p);
		if (nib < 0)
			return false;
		if (nNibbles >= 64)
			return false;
		if ((nNibbles & 1) == 0)
			tmp[nNibbles / 2] = static_cast<BYTE>(nib << 4);
		else
			tmp[nNibbles / 2] |= static_cast<BYTE>(nib);
		++nNibbles;
	}

	if (nNibbles == 0)
		return true;
	if (nNibbles != 64)
		return false;

	if (pbEmpty)
		*pbEmpty = false;
	memcpy(out, tmp, 32);
	return true;
}

static bool Loader_Sha256File(const wchar_t* pszPath, BYTE outHash[32])
{
	memset(outHash, 0, 32);

	HANDLE hFile = CreateFileW(
		pszPath,
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
		nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	BCRYPT_ALG_HANDLE hAlg = nullptr;
	BCRYPT_HASH_HANDLE hHash = nullptr;
	DWORD cbHashObject = 0;
	DWORD cbData = 0;
	PBYTE pHashObject = nullptr;
	bool bOk = false;

	if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
		goto done;
	if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&cbHashObject), sizeof(cbHashObject), &cbData, 0)))
		goto done;

	pHashObject = static_cast<PBYTE>(HeapAlloc(GetProcessHeap(), 0, cbHashObject));
	if (!pHashObject)
		goto done;

	if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, pHashObject, cbHashObject, nullptr, 0, 0)))
		goto done;

	{
		BYTE buf[64 * 1024];
		for (;;)
		{
			DWORD nRead = 0;
			if (!ReadFile(hFile, buf, sizeof(buf), &nRead, nullptr))
				goto done;
			if (nRead == 0)
				break;
			if (!NT_SUCCESS(BCryptHashData(hHash, buf, nRead, 0)))
				goto done;
		}
	}

	if (!NT_SUCCESS(BCryptFinishHash(hHash, outHash, 32, 0)))
		goto done;

	bOk = true;

done:
	if (hHash)
		BCryptDestroyHash(hHash);
	if (hAlg)
		BCryptCloseAlgorithmProvider(hAlg, 0);
	if (pHashObject)
		HeapFree(GetProcessHeap(), 0, pHashObject);
	CloseHandle(hFile);
	return bOk;
}

static void Loader_HashToHex(const BYTE hash[32], char* pszOut, const size_t cchOut)
{
	static const char* const kHex = "0123456789abcdef";
	if (!pszOut || cchOut < 65)
		return;
	for (int i = 0; i < 32; ++i)
	{
		pszOut[i * 2] = kHex[(hash[i] >> 4) & 0xF];
		pszOut[i * 2 + 1] = kHex[hash[i] & 0xF];
	}
	pszOut[64] = '\0';
}

// Optional pin: cmdline -r5sdk_worker_sha256 <hex> wins; else role env.
// Empty pin => lab builds load without hash gate.
static bool Loader_GetOptionalShaPin(const bool bDedicated, BYTE outPin[32], bool* pbHavePin)
{
	if (pbHavePin)
		*pbHavePin = false;

	const wchar_t* const cmd = GetCommandLineW();
	if (cmd)
	{
		const wchar_t* p = wcsstr(cmd, kWorkerShaArg);
		if (p)
		{
			p += wcslen(kWorkerShaArg);
			while (*p == L' ' || *p == L'\t')
				++p;
			char hex[128] = {};
			size_t n = 0;
			while (*p && *p != L' ' && *p != L'\t' && n + 1 < sizeof(hex))
			{
				if (*p > 127)
					break;
				hex[n++] = static_cast<char>(*p++);
			}
			hex[n] = '\0';

			bool bEmpty = true;
			if (!Loader_ParseSha256Hex(hex, outPin, &bEmpty))
			{
				Loader_Trace("[SEC-LOADER] REJECT malformed -r5sdk_worker_sha256 pin\n");
				return false;
			}
			if (!bEmpty)
			{
				if (pbHavePin)
					*pbHavePin = true;
				return true;
			}
		}
	}

	char envBuf[128] = {};
	const char* const envName = bDedicated
		? "R5SDK_SERVER_DLL_SHA256"
		: "R5SDK_CLIENT_DLL_SHA256";
	const DWORD nEnv = GetEnvironmentVariableA(envName, envBuf, sizeof(envBuf));
	if (nEnv == 0 || nEnv >= sizeof(envBuf))
		return true; // unset or empty => no pin

	bool bEmpty = true;
	if (!Loader_ParseSha256Hex(envBuf, outPin, &bEmpty))
	{
		Loader_Trace("[SEC-LOADER] REJECT malformed env %s pin\n", envName);
		return false;
	}
	if (!bEmpty && pbHavePin)
		*pbHavePin = true;
	return true;
}

// Authenticate worker file on disk. Fail-closed; never loads on reject.
static bool Loader_AuthenticateWorker(
	const wchar_t* pszPath,
	const bool bDedicated,
	const char* pszModuleNameA)
{
	ULONGLONG size = 0;
	if (!Loader_FileExistsSize(pszPath, &size))
	{
		Loader_Trace("[SEC-LOADER] REJECT %s missing or not a file: %ls\n",
			pszModuleNameA, pszPath);
		return false;
	}
	if (size < kWorkerDllMinBytes)
	{
		Loader_Trace("[SEC-LOADER] REJECT %s size %llu < floor %llu: %ls\n",
			pszModuleNameA,
			static_cast<unsigned long long>(size),
			static_cast<unsigned long long>(kWorkerDllMinBytes),
			pszPath);
		return false;
	}
	if (!Loader_CheckPeSanity(pszPath))
	{
		Loader_Trace("[SEC-LOADER] REJECT %s PE sanity failed: %ls\n",
			pszModuleNameA, pszPath);
		return false;
	}

	BYTE pin[32] = {};
	bool bHavePin = false;
	if (!Loader_GetOptionalShaPin(bDedicated, pin, &bHavePin))
		return false;

	if (bHavePin)
	{
		BYTE actual[32] = {};
		if (!Loader_Sha256File(pszPath, actual))
		{
			Loader_Trace("[SEC-LOADER] REJECT %s SHA-256 read failed: %ls\n",
				pszModuleNameA, pszPath);
			return false;
		}
		if (memcmp(pin, actual, 32) != 0)
		{
			char expectHex[65] = {};
			char actualHex[65] = {};
			Loader_HashToHex(pin, expectHex, sizeof(expectHex));
			Loader_HashToHex(actual, actualHex, sizeof(actualHex));
			Loader_Trace(
				"[SEC-LOADER] REJECT %s SHA-256 pin mismatch\n"
				"  path=%ls\n"
				"  expect=%s\n"
				"  actual=%s\n",
				pszModuleNameA, pszPath, expectHex, actualHex);
			return false;
		}
		Loader_Trace("[SEC-LOADER] %s SHA-256 pin OK\n", pszModuleNameA);
	}
	else
	{
		Loader_Trace("[SEC-LOADER] %s pin unset (lab); size=%llu PE OK path=%ls\n",
			pszModuleNameA,
			static_cast<unsigned long long>(size),
			pszPath);
	}

	return true;
}

//-----------------------------------------------------------------------------
// /Respawn/Apex_fnf and /Respawn/Apex_r5f are the same length. One rdata
// copy feeds profile, videoconfig, savegames, previousgamestate. Keep -fnf
// so the engine takes this string instead of live /Respawn/Apex.
//-----------------------------------------------------------------------------
static void Loader_RetargetSavedGamesFolder(void)
{
	static const char kNeedle[] = "/Respawn/Apex_fnf";
	static const char kNewTail[] = "r5f";
	const size_t needleLen = sizeof(kNeedle);
	const size_t tailOff = sizeof(kNeedle) - 1u - 3u;

	if (Loader_IsDedicatedExe())
	{
		Loader_Trace("[FNF] skip retarget on dedicated\n");
		return;
	}

	const HMODULE hExe = GetModuleHandleW(NULL);
	if (!hExe)
	{
		Loader_Trace("[FNF] GetModuleHandleW null, skip retarget\n");
		return;
	}

	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hExe);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return;

	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		reinterpret_cast<const BYTE*>(hExe) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return;

	const BYTE* const base = reinterpret_cast<const BYTE*>(hExe);
	const size_t imageSize = nt->OptionalHeader.SizeOfImage;
	const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
	{
		const DWORD chars = sec->Characteristics;
		if (!(chars & IMAGE_SCN_CNT_INITIALIZED_DATA))
			continue;
		if (!(chars & IMAGE_SCN_MEM_READ))
			continue;
		if (chars & IMAGE_SCN_MEM_EXECUTE)
			continue;
		if (chars & IMAGE_SCN_MEM_DISCARDABLE)
			continue;
		if (sec->SizeOfRawData == 0)
			continue;

		size_t n = sec->Misc.VirtualSize;
		if (n == 0 || n > sec->SizeOfRawData)
			n = sec->SizeOfRawData;
		if (sec->VirtualAddress >= imageSize)
			continue;
		if (sec->VirtualAddress + n > imageSize)
			n = imageSize - sec->VirtualAddress;
		if (n < needleLen)
			continue;

		BYTE* const start = const_cast<BYTE*>(base + sec->VirtualAddress);
		MEMORY_BASIC_INFORMATION mbi = {};
		if (!VirtualQuery(start, &mbi, sizeof(mbi)))
			continue;
		if (mbi.State != MEM_COMMIT)
			continue;
		if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
			PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
			continue;
		if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
			continue;

		const size_t committed = static_cast<size_t>(
			reinterpret_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize - start);
		if (committed < n)
			n = committed;
		if (n < needleLen)
			continue;

		for (size_t off = 0; off + needleLen <= n; ++off)
		{
			if (memcmp(start + off, kNeedle, needleLen) != 0)
				continue;

			BYTE* const dest = start + off + tailOff;
			DWORD oldProt = 0;
			if (!VirtualProtect(dest, 3, PAGE_READWRITE, &oldProt))
			{
				Loader_Trace("[FNF] VirtualProtect failed @ %p gle=%lu\n",
					dest, GetLastError());
				return;
			}
			memcpy(dest, kNewTail, 3);
			DWORD tmp = 0;
			VirtualProtect(dest, 3, oldProt, &tmp);
			Loader_Trace("[FNF] retargeted Saved Games @ %p -> Apex_r5f\n", dest);
			return;
		}
	}

	Loader_Trace("[FNF] '/Respawn/Apex_fnf' not in host image; path unchanged\n");
}

static LPSTR WINAPI hkGetCommandLineA(void)
{
	return s_fnfCmdA;
}

static LPWSTR WINAPI hkGetCommandLineW(void)
{
	return s_fnfCmdW;
}

//-----------------------------------------------------------------------------
// Hook GetCommandLine so CreateCmdLine sees a suffix the process was not started with.
//-----------------------------------------------------------------------------
static bool Loader_HookCommandLine(const wchar_t* suffixW, const char* suffixA, const char* tag)
{
	const wchar_t* const curW = GetCommandLineW();
	const char* const curA = GetCommandLineA();
	if (!curW || !curA)
	{
		Loader_Trace("[%s] GetCommandLine null, cannot inject\n", tag);
		return false;
	}

	if (_snwprintf_s(s_fnfCmdW, sizeof(s_fnfCmdW) / sizeof(s_fnfCmdW[0]),
		_TRUNCATE, L"%s%s", curW, suffixW) < 0)
	{
		Loader_Trace("[%s] wide cmdline overflow\n", tag);
		return false;
	}
	if (_snprintf_s(s_fnfCmdA, sizeof(s_fnfCmdA), _TRUNCATE, "%s%s", curA, suffixA) < 0)
	{
		Loader_Trace("[%s] ansi cmdline overflow\n", tag);
		return false;
	}

	const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (!k32)
		return false;

	v_GetCommandLineA = reinterpret_cast<LPSTR (WINAPI*)(void)>(
		GetProcAddress(k32, "GetCommandLineA"));
	v_GetCommandLineW = reinterpret_cast<LPWSTR (WINAPI*)(void)>(
		GetProcAddress(k32, "GetCommandLineW"));
	if (!v_GetCommandLineA || !v_GetCommandLineW)
	{
		Loader_Trace("[%s] GetCommandLine proc missing\n", tag);
		return false;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_GetCommandLineA),
		reinterpret_cast<PVOID>(&hkGetCommandLineA));
	DetourAttach(&reinterpret_cast<PVOID&>(v_GetCommandLineW),
		reinterpret_cast<PVOID>(&hkGetCommandLineW));
	const LONG hr = DetourTransactionCommit();
	if (hr != NO_ERROR)
	{
		Loader_Trace("[%s] GetCommandLine detour FAILED hr=%ld\n", tag, (long)hr);
		s_fnfCmdA[0] = '\0';
		s_fnfCmdW[0] = L'\0';
		return false;
	}

	Loader_Trace("[%s] injected via GetCommandLine hook\n", tag);
	return true;
}

//-----------------------------------------------------------------------------
// Launcher-owned processes already have -dedicated; append extra flags in-process.
// Dedi needs sendtable transmit + compressed stringtables or the client hangs.
//-----------------------------------------------------------------------------
static void Loader_InstallFnfCommandLine(void)
{
	if (Loader_IsDedicatedExe())
	{
		const wchar_t* const curW = GetCommandLineW();
		if (curW
			&& Loader_WideStrContains(curW, L"+sv_allowSendTableTransmitToClients 1")
			&& Loader_WideStrContains(curW, L"+stringtable_compress 1"))
		{
			Loader_Trace("[SIGNON] cmdline already has sendtable+stringtable compress\n");
			return;
		}
		Loader_HookCommandLine(kDediSignonArgsW, kDediSignonArgsA, "SIGNON");
		return;
	}

	const wchar_t* const curW = GetCommandLineW();
	if (curW && Loader_WideStrContains(curW, kFnfArg))
	{
		Loader_Trace("[FNF] cmdline already has -fnf\n");
		return;
	}

	Loader_HookCommandLine(kFnfExtraOnly, " -fnf", "FNF");
}

//-----------------------------------------------------------------------------
// Returns true if this process should stop (child spawned / parent exiting).
//-----------------------------------------------------------------------------
static bool Loader_RelaunchIfMissingFlags(void)
{
	const wchar_t* const currentCmdLine = GetCommandLineW();
	if (!currentCmdLine)
	{
		Loader_Trace("  GetCommandLineW null, skip relaunch\n");
		return false;
	}

	Loader_Trace("  current cmdline: %ls\n", currentCmdLine);

	const bool bDedicated = Loader_IsDedicatedExe();
	const wchar_t* const staged = bDedicated ? kDediStagedFlag : kClientStagedFlag;
	if (Loader_WideStrContains(currentCmdLine, staged))
	{
		Loader_Trace("  product flag %ls present, no relaunch\n", staged);
		return false;
	}

	wchar_t exePath[MAX_PATH];
	const DWORD pathLen = GetModuleFileNameW(NULL, exePath, MAX_PATH);
	if (pathLen == 0 || pathLen >= MAX_PATH)
	{
		Loader_Trace("  GetModuleFileNameW failed\n");
		return false;
	}

	const wchar_t* const extra = bDedicated ? kDediExtraArgs : kClientExtraArgs;
	const size_t currentLen = wcslen(currentCmdLine);
	const size_t extraLen = wcslen(extra);
	const size_t needed = currentLen + extraLen + 1;

	wchar_t* const newCmdLine = static_cast<wchar_t*>(
		HeapAlloc(GetProcessHeap(), 0, needed * sizeof(wchar_t)));
	if (!newCmdLine)
	{
		Loader_Trace("  HeapAlloc failed\n");
		return false;
	}

	wmemcpy(newCmdLine, currentCmdLine, currentLen);
	wmemcpy(newCmdLine + currentLen, extra, extraLen);
	newCmdLine[currentLen + extraLen] = L'\0';
	Loader_Trace("  relaunching: %ls\n", newCmdLine);

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};

	const BOOL ok = CreateProcessW(
		exePath,
		newCmdLine,
		nullptr,
		nullptr,
		FALSE,
		0,
		nullptr,
		nullptr,
		&si,
		&pi);

	if (!ok)
	{
		Loader_Trace("  CreateProcessW FAILED err=0x%08X\n", (unsigned)GetLastError());
		HeapFree(GetProcessHeap(), 0, newCmdLine);
		return false;
	}

	Loader_Trace("  child pid=%u -- parent exiting\n", (unsigned)pi.dwProcessId);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	HeapFree(GetProcessHeap(), 0, newCmdLine);

	// ExitProcess (not TerminateProcess): same outcome, less "injector kill" shape.
	ExitProcess(0);
}

//-----------------------------------------------------------------------------
static void Loader_GatedInitSdk(void)
{
	const bool bDedicated = Loader_IsDedicatedExe();
	const char* const moduleName = bDedicated ? kServerSdkModule : kClientSdkModule;

	wchar_t hostDir[MAX_PATH] = {};
	wchar_t workerPath[MAX_PATH] = {};
	if (!Loader_ResolveWorkerPathW(bDedicated, workerPath, MAX_PATH, hostDir, MAX_PATH))
	{
		char buf[512];
		wsprintfA(buf,
			"[SEC-LOADER] Failed to resolve absolute path for %s next to host.",
			moduleName);
		Loader_FatalError(buf);
		return;
	}

	Loader_Trace("[SEC-LOADER] worker resolve dedicated=%d host=%ls path=%ls\n",
		(int)bDedicated, hostDir, workerPath);

	if (!Loader_AuthenticateWorker(workerPath, bDedicated, moduleName))
	{
		char buf[512];
		wsprintfA(buf,
			"[SEC-LOADER] Authenticity check failed for %s. Refusing SDK_Init.",
			moduleName);
		Loader_FatalError(buf);
		return;
	}

	Loader_Trace("[SEC-LOADER] LoadLibraryExW(%ls) + SDK_Init\n", workerPath);

	// Absolute worker path; dependents from System32 only (worker IAT is OS-only).
	const DWORD loadFlags = LOAD_LIBRARY_SEARCH_SYSTEM32;
	HMODULE sdk = LoadLibraryExW(workerPath, nullptr, loadFlags);
	if (!sdk)
	{
		const DWORD errEx = GetLastError();
		Loader_Trace("[SEC-LOADER] LoadLibraryExW flags=0x%X failed err=0x%08X\n",
			(unsigned)loadFlags, (unsigned)errEx);
	}
	if (!sdk)
	{
		char buf[512];
		wsprintfA(buf, "[SEC-LOADER] Failed to load %s (error 0x%08X).",
			moduleName, (unsigned)GetLastError());
		Loader_FatalError(buf);
		return;
	}

	typedef void (*PFN_SDK_Init)(void);
	const PFN_SDK_Init pfnSDKInit =
		reinterpret_cast<PFN_SDK_Init>(GetProcAddress(sdk, "SDK_Init"));
	if (!pfnSDKInit)
	{
		char buf[256];
		wsprintfA(buf, "[SEC-LOADER] %s is missing the SDK_Init export.", moduleName);
		Loader_FatalError(buf);
		return;
	}

	pfnSDKInit();
	Loader_Trace("  SDK_Init returned; calling real LauncherMain\n");
}

static int hkLauncherMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
	Loader_Trace("hkLauncherMain enter\n");
	Loader_GatedInitSdk();
	return v_LauncherMain(hInstance, hPrevInstance, lpCmdLine, nShowCmd);
}

// Weak entry check: catch RVA->padding after a host update. Do not police
// prologues -- a hard reject once unhooked a critical path silently.
static bool Loader_IsPlausibleCodeEntry(HMODULE hMod, void* pTarget)
{
	if (!pTarget)
	{
		Loader_Trace("  LauncherMain reject: null target\n");
		return false;
	}
	if (!hMod)
	{
		Loader_Trace("  LauncherMain reject: null module\n");
		return false;
	}

	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hMod);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		Loader_Trace("  LauncherMain reject: bad DOS header\n");
		return false;
	}

	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		reinterpret_cast<const BYTE*>(hMod) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
	{
		Loader_Trace("  LauncherMain reject: bad NT headers\n");
		return false;
	}

	const uintptr_t base = reinterpret_cast<uintptr_t>(hMod);
	const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
	const uintptr_t addr = reinterpret_cast<uintptr_t>(pTarget);
	if (addr < base || addr >= end)
	{
		Loader_Trace("  LauncherMain reject: %p outside image [%p, %p)\n",
			pTarget, (void*)base, (void*)end);
		return false;
	}

	const unsigned char first = *reinterpret_cast<const unsigned char*>(pTarget);
	if (first == 0x00 || first == 0xCC)
	{
		Loader_Trace("  LauncherMain reject: %p first-byte 0x%02X (padding/int3)\n",
			pTarget, (unsigned)first);
		return false;
	}

	return true;
}

static bool Loader_ResolveLauncherMain(void)
{
	const HMODULE hExe = GetModuleHandleW(NULL);
	const uintptr_t exeBase = reinterpret_cast<uintptr_t>(hExe);
	if (!exeBase)
		return false;

	wchar_t exePath[MAX_PATH] = {};
	GetModuleFileNameW(hExe, exePath, MAX_PATH);
	const bool bDedicated = Loader_IsDedicatedExe();
	Loader_Trace("  image: %ls base=%p dedicated=%d\n",
		exePath, (void*)exeBase, (int)bDedicated);

	if (bDedicated)
	{
		FARPROC exp = GetProcAddress(hExe, "LauncherMain");
		if (exp)
		{
			v_LauncherMain = reinterpret_cast<int (*)(HINSTANCE, HINSTANCE, LPSTR, int)>(exp);
			Loader_Trace("  LauncherMain @ %p (export)\n", (void*)exp);
		}
		else
		{
			v_LauncherMain = reinterpret_cast<int (*)(HINSTANCE, HINSTANCE, LPSTR, int)>(
				exeBase + kDediLauncherMainRva);
			Loader_Trace("  LauncherMain @ %p (dedi RVA 0x%zX)\n",
				(void*)v_LauncherMain, (size_t)kDediLauncherMainRva);
		}
	}
	else
	{
		FARPROC exp = GetProcAddress(hExe, "LauncherMain");
		if (exp)
		{
			v_LauncherMain = reinterpret_cast<int (*)(HINSTANCE, HINSTANCE, LPSTR, int)>(exp);
			Loader_Trace("  LauncherMain @ %p (export)\n", (void*)exp);
		}
		else
		{
			const uintptr_t rva = Loader_IsDx12Exe()
				? kClientLauncherMainRvaDx12
				: kClientLauncherMainRvaDx11;
			v_LauncherMain = reinterpret_cast<int (*)(HINSTANCE, HINSTANCE, LPSTR, int)>(
				exeBase + rva);
			Loader_Trace("  LauncherMain @ %p (client RVA 0x%zX dx12=%d)\n",
				(void*)v_LauncherMain, (size_t)rva, (int)Loader_IsDx12Exe());
		}
	}

	if (!Loader_IsPlausibleCodeEntry(hExe, reinterpret_cast<void*>(v_LauncherMain)))
	{
		v_LauncherMain = nullptr;
		return false;
	}

	return v_LauncherMain != nullptr;
}

static bool Loader_AttachLauncherMain(void)
{
	if (!Loader_ResolveLauncherMain())
		return false;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_LauncherMain),
		reinterpret_cast<PVOID>(&hkLauncherMain));
	const LONG hr = DetourTransactionCommit();
	if (hr != NO_ERROR)
	{
		Loader_Trace("  DetourAttach(LauncherMain) FAILED hr=%ld\n", (long)hr);
		return false;
	}

	Loader_Trace("  LauncherMain detour armed\n");
	return true;
}

static void Loader_SecurityBootBanner(void)
{
	static const char* const kBanner =
		"[SEC-LOADER] WARNING: loader.dll is PE-import-mapped by the host. "
		"Windows maps it before any SDK authenticity check can run. "
		"Ship only a known-good / signed loader.dll next to the host exe. "
		"Worker client.dll/server.dll is authenticated at LauncherMain gate "
		"(exists, size floor, PE, optional SHA-256 pin).\n";

	Loader_Trace("%s", kBanner);
	OutputDebugStringA(kBanner);
}

//-----------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID /*lpReserved*/)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
	{
		{
			FILE* fp = nullptr;
			fopen_s(&fp, kTracePath, "w");
			if (fp)
				fclose(fp);
		}
		Loader_Trace("DllMain PROCESS_ATTACH hModule=%p\n", (void*)hModule);
		Loader_SecurityBootBanner();
		DisableThreadLibraryCalls(hModule);

		// -allowmultiple needs non-empty VPROJECT (child inherits env).
		SetEnvironmentVariableW(L"VPROJECT", L"1");
		Loader_Trace("set VPROJECT=1\n");

		// Stage 1: process-level cmdline (CreateProcess child). ExitProcess
		// never returns on success.
		if (Loader_RelaunchIfMissingFlags())
			return FALSE;

		Loader_InstallFnfCommandLine();
		Loader_RetargetSavedGamesFolder();

		// Stage 2: SDK gate.
		if (!Loader_AttachLauncherMain())
			Loader_FatalError("Failed to install the LauncherMain SDK-init gate.");

		Loader_Trace("DllMain: gate armed, return TRUE\n");
		break;
	}

	case DLL_PROCESS_DETACH:
		break;

	default:
		break;
	}

	return TRUE;
}
