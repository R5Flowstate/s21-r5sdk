//=============================================================================//
//
// Purpose: client -> launcher requests over the per-launch handoff pipe.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/strtools.h"
#include "engine/client/launcher_handoff.h"

#define HANDOFF_PIPE_ENV "R5F_HANDOFF_PIPE"
#define HANDOFF_PIPE_PREFIX "\\\\.\\pipe\\r5f-handoff-"
#define HANDOFF_PIPE_ID_LEN 32
#define HANDOFF_TARGET_MAX 255
#define HANDOFF_COOLDOWN_MS 3000

static ULONGLONG s_nLastRequestMs = 0;

// Whoever serves the pipe may identify this process but never act as it.
static constexpr DWORD s_nPipeFlags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;

// The name comes from the environment the launcher gave us; anything but its
// exact shape could point the write at another process's pipe.
static bool LauncherHandoff_GetPipe(char* const pszOut, const size_t nSize)
{
	const DWORD nLen = GetEnvironmentVariableA(HANDOFF_PIPE_ENV, pszOut, static_cast<DWORD>(nSize));
	if (nLen == 0 || nLen >= nSize)
		return false;

	const size_t nPrefix = sizeof(HANDOFF_PIPE_PREFIX) - 1;
	if (nLen != nPrefix + HANDOFF_PIPE_ID_LEN || V_strncmp(pszOut, HANDOFF_PIPE_PREFIX, nPrefix) != 0)
		return false;

	for (size_t i = nPrefix; i < nLen; ++i)
	{
		const char c = pszOut[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}

	return true;
}

static bool LauncherHandoff_TargetCharsOk(const char* const pszTarget)
{
	if (!pszTarget || !pszTarget[0] || V_strlen(pszTarget) > HANDOFF_TARGET_MAX)
		return false;

	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszTarget); *p; ++p)
	{
		const unsigned char c = *p;
		const bool bOk = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| c == '.' || c == ':' || c == '[' || c == ']' || c == '-' || c == '_';
		if (!bOk)
			return false;
	}

	return true;
}

bool LauncherHandoff_IsAvailable(void)
{
	char szPipe[128];
	return LauncherHandoff_GetPipe(szPipe, sizeof(szPipe));
}

bool LauncherHandoff_RequestModJoin(const char* const pszTarget)
{
	if (!LauncherHandoff_TargetCharsOk(pszTarget))
	{
		Warning(eDLL_T::CLIENT, "[LAUNCHER-HANDOFF] refused malformed target\n");
		return false;
	}

	char szPipe[128];
	if (!LauncherHandoff_GetPipe(szPipe, sizeof(szPipe)))
	{
		Warning(eDLL_T::CLIENT, "[LAUNCHER-HANDOFF] not started by the launcher\n");
		return false;
	}

	const ULONGLONG nNow = GetTickCount64();
	if (s_nLastRequestMs && nNow - s_nLastRequestMs < HANDOFF_COOLDOWN_MS)
		return false;
	s_nLastRequestMs = nNow;

	HANDLE hPipe = CreateFileA(szPipe, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, s_nPipeFlags, nullptr);
	if (hPipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeA(szPipe, 500))
		hPipe = CreateFileA(szPipe, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, s_nPipeFlags, nullptr);

	if (hPipe == INVALID_HANDLE_VALUE)
	{
		Warning(eDLL_T::CLIENT, "[LAUNCHER-HANDOFF] launcher pipe unavailable (error %lu)\n", GetLastError());
		return false;
	}

	char szLine[HANDOFF_TARGET_MAX + 32];
	const int nLine = V_snprintf(szLine, sizeof(szLine), "join_mods %s\n", pszTarget);

	DWORD nWritten = 0;
	const bool bOk = nLine > 0 && WriteFile(hPipe, szLine, static_cast<DWORD>(nLine), &nWritten, nullptr)
		&& nWritten == static_cast<DWORD>(nLine);
	CloseHandle(hPipe);

	if (bOk)
		Msg(eDLL_T::CLIENT, "[LAUNCHER-HANDOFF] asked the launcher to install mods for '%s'\n", pszTarget);
	else
		Warning(eDLL_T::CLIENT, "[LAUNCHER-HANDOFF] write failed (error %lu)\n", GetLastError());

	return bOk;
}
