// Included from each console.cpp dual-body half. Not a standalone TU.
#include <io.h>
#include <fcntl.h>

// Launcher-owned pipe names. Sized from the literal so the compare can never
// run past it into the leaf and reject every real path.
static const char s_szHostedPipePrefix[] = "\\\\.\\pipe\\r5f-";
static const size_t s_nHostedPipePrefixLen = sizeof(s_szHostedPipePrefix) - 1;

bool Console_HostedWanted(void)
{
	char szHosted[8] = {};
	return GetEnvironmentVariableA("R5F_HOSTED_CONSOLE", szHosted, sizeof(szHosted)) != 0
		&& szHosted[0] == '1';
}

// Launcher names are r5f-{con|in}-{s|c}-<id>. Bind only this product's
// letter or a leftover env from the other role feeds it our stdin.
#if defined(CLIENT_DLL)
static const char s_chHostedRole = 'c';
static const char s_szHostedPipeTag[] = "-c-";
#else
static const char s_chHostedRole = 's';
static const char s_szHostedPipeTag[] = "-s-";
#endif

static bool Console_HostedRoleMatches(void)
{
	char szRole[8] = {};
	const DWORD n = GetEnvironmentVariableA("R5F_CONSOLE_ROLE", szRole, sizeof(szRole));
	if (n == 0 || szRole[0] == '\0')
		return true;
	return (szRole[0] | 32) == s_chHostedRole;
}

static bool Console_HostedPipeTagOk(const char* pszPath)
{
	return pszPath && pszPath[0] && strstr(pszPath, s_szHostedPipeTag) != nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: hide the console the moment it exists.
//
// The console still has to be allocated -- the engine's logging writes through
// console APIs and produces nothing without one. The launcher already starts us
// with a hidden show state so the window never appears; this is the fallback
// for any path that reaches here with a visible one.
//-----------------------------------------------------------------------------
void Console_HideIfHosted(void)
{
	if (!Console_HostedWanted())
		return;

	HWND hWnd = GetConsoleWindow();
	if (hWnd)
		ShowWindow(hWnd, SW_HIDE);
}

void Console_ApplyHostedSession(void)
{
	if (!Console_HostedWanted())
		return;

	Console_HideIfHosted();

	if (!Console_HostedRoleMatches())
	{
		OutputDebugStringA("R5F hosted console: role mismatch, pipes not bound\n");
		return;
	}

	char szOut[MAX_PATH] = {};
	if (GetEnvironmentVariableA("R5F_CONSOLE_PIPE", szOut, sizeof(szOut)) > 0
		&& strncmp(szOut, s_szHostedPipePrefix, s_nHostedPipePrefixLen) == 0
		&& Console_HostedPipeTagOk(szOut))
	{
		HANDLE hPipe = INVALID_HANDLE_VALUE;
		for (int i = 0; i < 40 && hPipe == INVALID_HANDLE_VALUE; ++i)
		{
			hPipe = CreateFileA(szOut, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hPipe == INVALID_HANDLE_VALUE)
				Sleep(50);
		}
		if (hPipe != INVALID_HANDLE_VALUE)
		{
			const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hPipe), _O_TEXT);
			if (fd != -1)
			{
				_dup2(fd, _fileno(stdout));
				_dup2(fd, _fileno(stderr));
				SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
				SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
				setvbuf(stdout, nullptr, _IONBF, 0);
				setvbuf(stderr, nullptr, _IONBF, 0);
			}
		}
		else
		{
			// The window is already hidden by now, so a failure here costs every
			// line the run would have printed.
			OutputDebugStringA("R5F hosted console: stdout pipe connect failed\n");
		}
	}

	char szIn[MAX_PATH] = {};
	if (GetEnvironmentVariableA("R5F_CONSOLE_IN", szIn, sizeof(szIn)) > 0
		&& strncmp(szIn, s_szHostedPipePrefix, s_nHostedPipePrefixLen) == 0
		&& Console_HostedPipeTagOk(szIn))
	{
		HANDLE hPipe = INVALID_HANDLE_VALUE;
		for (int i = 0; i < 40 && hPipe == INVALID_HANDLE_VALUE; ++i)
		{
			hPipe = CreateFileA(szIn, GENERIC_READ, 0, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hPipe == INVALID_HANDLE_VALUE)
				Sleep(50);
		}
		if (hPipe != INVALID_HANDLE_VALUE)
		{
			const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hPipe), _O_TEXT);
			if (fd != -1)
			{
				_dup2(fd, _fileno(stdin));
				SetStdHandle(STD_INPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdin)));
			}
		}
	}
}
