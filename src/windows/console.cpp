#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: Windows terminal utilities
//
//=============================================================================//

#include "core/stdafx.h"
#ifndef _TOOLS
#include "core/init.h"
#include "core/logdef.h"
#include "tier0/frametask.h"
#include "engine/cmd.h"
#include "windows/id3dx.h"
#endif // !_TOOLS
#include "windows/system.h"
#include "windows/console.h"

static std::string s_ConsoleInput;

//-----------------------------------------------------------------------------
// Purpose: sets the windows terminal background color
// Input: color - 
//-----------------------------------------------------------------------------
void SetConsoleBackgroundColor(COLORREF color)
{
	CONSOLE_SCREEN_BUFFER_INFOEX sbInfoEx{0};
	sbInfoEx.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);

	HANDLE consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
	GetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);

	// The +='' 1 is required, else the window will shrink
	// by '1' column and row each time this function is
	// getting called on the same console window. The
	// lower right bounds are detected inclusively on the
	// 'GetConsoleScreenBufferEx' call and exclusively
	// on the 'SetConsoleScreenBufferEx' call.
	sbInfoEx.srWindow.Right += 1;
	sbInfoEx.srWindow.Bottom += 1;

	sbInfoEx.ColorTable[0] = color;
	SetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);
}

//-----------------------------------------------------------------------------
// Purpose: flashes the windows terminal background color
// Input: nFlashCount - 
// nFlashInterval -
// color -
//-----------------------------------------------------------------------------
void FlashConsoleBackground(int nFlashCount, int nFlashInterval, COLORREF color)
{
	CONSOLE_SCREEN_BUFFER_INFOEX sbInfoEx{0};
	sbInfoEx.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);

	HANDLE consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
	GetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);

	COLORREF storedBG = sbInfoEx.ColorTable[0];

	for (int i = 0; i < nFlashCount; ++i)
	{
		//-- set BG color
		Sleep(nFlashInterval);
		sbInfoEx.ColorTable[0] = color;
		SetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);

		//-- restore previous color
		Sleep(nFlashInterval);
		sbInfoEx.ColorTable[0] = storedBG;
		SetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);
	}
}

//-----------------------------------------------------------------------------
// Purpose: terminal window setup
// Input: bAnsiColor - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool Console_Init(const bool bAnsiColor)
{
	char msgBuf[2048];

	///////////////////////////////////////////////////////////////////////////
	// Create the console window.
	// When injected, the process may already have a console inherited from
	// the injector. Free it first so we get our own dedicated SDK console.
	FreeConsole();

	if (AllocConsole() == FALSE)
	{
		snprintf(msgBuf, sizeof(msgBuf), "Failed to create console window! [%s]\n",
			std::system_category().message(static_cast<int>(::GetLastError())).c_str());

		OutputDebugStringA(msgBuf);
		return false;
	}

	//-- Hosted: hide before anything can draw it.
	Console_HideIfHosted();

#ifndef _TOOLS
	//-- Set the window title
	SetConsoleTitleA("R5F Client");
#endif // !_TOOLS

	//-- Open input/output streams
	FILE* fDummy;
	freopen_s(&fDummy, "CONIN$", "r", stdin);
	freopen_s(&fDummy, "CONOUT$", "w", stdout);
	freopen_s(&fDummy, "CONOUT$", "w", stderr);

	if (bAnsiColor)
	{
		if (!Console_ColorInit())
		{
			Assert(0);
			//snprintf(msgBuf, sizeof(msgBuf), "Failed to set color console mode! [%s]\n",
			//	std::system_category.message(static_cast<int>(::GetLastError)).c_str);

			//MessageBoxA(NULL, msgBuf, "SDK Warning", MB_ICONEXCLAMATION | MB_OK);
		}
	}

	Console_ApplyHostedSession();

#ifndef _TOOLS
	//-- Create a worker thread to process console commands
	DWORD dwThreadId = NULL;
	DWORD __stdcall ProcessConsoleWorker(LPVOID);
	HANDLE hThread = CreateThread(NULL, 0, ProcessConsoleWorker, NULL, 0, &dwThreadId);
	
	if (hThread)
	{
		CloseHandle(hThread);
	}
#endif // !_TOOLS

#ifndef _TOOLS
	SetConsoleCtrlHandler(ConsoleHandlerRoutine, true);
#endif // !_TOOLS

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: terminal color setup
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool Console_ColorInit()
{
	HANDLE hOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD dwMode = NULL;

	GetConsoleMode(hOutput, &dwMode); // Some editions of Windows have 'VirtualTerminalLevel' disabled by default.
	dwMode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;

	if (!SetConsoleMode(hOutput, dwMode))
		return false; // Failure.

	SetConsoleBackgroundColor(0x00000000);
	AnsiColors_Init();

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: terminal window shutdown
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool Console_Shutdown()
{
	///////////////////////////////////////////////////////////////////////////
	// Destroy the console window
	if (FreeConsole() == FALSE)
	{
		char szBuf[2048];
		snprintf(szBuf, sizeof(szBuf), "Failed to destroy console window! [%s]\n", 
			std::system_category().message(static_cast<int>(::GetLastError())).c_str());

		OutputDebugStringA(szBuf);
		return false;
	}

	return true;
}

#ifndef _TOOLS
//#############################################################################
// CONSOLE WORKER
//#############################################################################
DWORD __stdcall ProcessConsoleWorker(LPVOID)
{
	while (true)
	{
		//printf("] ");
		//-- Get the user input on the debug console
		std::getline(std::cin, s_ConsoleInput);

		if (!s_ConsoleInput.empty())
		{
			// Execute the command.
			Cbuf_AddText(Cbuf_GetCurrentPlayer(), s_ConsoleInput.c_str(), cmd_source_t::kCommandSrcCode);
			s_ConsoleInput.clear();
		}

		Sleep(50);
	}
	return NULL;
}
#endif // !_TOOLS

#include "windows/console_hosted.inl"
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: Windows terminal utilities
//
//=============================================================================//

#include "core/stdafx.h"
#ifndef _TOOLS
#include "core/init.h"
#include "core/logdef.h"
#include "tier0/frametask.h"
#include "engine/cmd.h"
#include "tier1/cvar.h"
#ifndef DEDICATED
#include "windows/id3dx.h"
#endif // !DEDICATED
#endif // !_TOOLS
#include "windows/system.h"
#include "windows/console.h"


//-----------------------------------------------------------------------------
// Purpose: sets the windows terminal background color
// Input: color - 
//-----------------------------------------------------------------------------
void SetConsoleBackgroundColor(COLORREF color)
{
	CONSOLE_SCREEN_BUFFER_INFOEX sbInfoEx{0};
	sbInfoEx.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);

	HANDLE consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
	GetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);

	// The +='' 1 is required, else the window will shrink
	// by '1' column and row each time this function is
	// getting called on the same console window. The
	// lower right bounds are detected inclusively on the
	// 'GetConsoleScreenBufferEx' call and exclusively
	// on the 'SetConsoleScreenBufferEx' call.
	sbInfoEx.srWindow.Right += 1;
	sbInfoEx.srWindow.Bottom += 1;

	sbInfoEx.ColorTable[0] = color;
	SetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);
}

//-----------------------------------------------------------------------------
// Purpose: flashes the windows terminal background color
// Input: nFlashCount - 
// nFlashInterval -
// color -
//-----------------------------------------------------------------------------
void FlashConsoleBackground(int nFlashCount, int nFlashInterval, COLORREF color)
{
	CONSOLE_SCREEN_BUFFER_INFOEX sbInfoEx{0};
	sbInfoEx.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);

	HANDLE consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
	GetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);

	COLORREF storedBG = sbInfoEx.ColorTable[0];

	for (int i = 0; i < nFlashCount; ++i)
	{
		//-- set BG color
		Sleep(nFlashInterval);
		sbInfoEx.ColorTable[0] = color;
		SetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);

		//-- restore previous color
		Sleep(nFlashInterval);
		sbInfoEx.ColorTable[0] = storedBG;
		SetConsoleScreenBufferInfoEx(consoleOut, &sbInfoEx);
	}
}

//-----------------------------------------------------------------------------
// Purpose: terminal window setup
// Input: bAnsiColor - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool Console_Init(const bool bAnsiColor)
{
	char msgBuf[2048];

	///////////////////////////////////////////////////////////////////////////
	// Create the console window. FreeConsole first: an inherited console makes
	// AllocConsole fail, and bailing there would skip the hosted-pipe bind that
	// carries every line to the launcher.
	FreeConsole();

	if (AllocConsole() == FALSE)
	{
		snprintf(msgBuf, sizeof(msgBuf), "Failed to create console window! [%s]\n", 
			std::system_category().message(static_cast<int>(::GetLastError())).c_str());

		OutputDebugStringA(msgBuf);
		Console_ApplyHostedSession();
		return false;
	}

	//-- Hosted: hide before anything can draw it.
	Console_HideIfHosted();

#ifndef _TOOLS
	//-- Set the window title
	SetConsoleTitleA("R5F Server");
#endif // !_TOOLS

	//-- Open input/output streams
	FILE* fDummy;
	freopen_s(&fDummy, "CONIN$", "r", stdin);
	freopen_s(&fDummy, "CONOUT$", "w", stdout);
	freopen_s(&fDummy, "CONOUT$", "w", stderr);

	if (bAnsiColor)
	{
		if (!Console_ColorInit())
		{
			Assert(0);
			//snprintf(msgBuf, sizeof(msgBuf), "Failed to set color console mode! [%s]\n",
			//	std::system_category.message(static_cast<int>(::GetLastError)).c_str);

			//MessageBoxA(NULL, msgBuf, "SDK Warning", MB_ICONEXCLAMATION | MB_OK);
		}
	}

	Console_ApplyHostedSession();

#ifndef _TOOLS
	//-- Create a worker thread to process console commands
	DWORD dwThreadId = NULL;
	DWORD __stdcall ProcessConsoleWorker(LPVOID);
	HANDLE hThread = CreateThread(NULL, 0, ProcessConsoleWorker, NULL, 0, &dwThreadId);
	
	if (hThread)
	{
		CloseHandle(hThread);
	}
#endif // !_TOOLS

#ifndef _TOOLS
	SetConsoleCtrlHandler(ConsoleHandlerRoutine, true);
#endif // !_TOOLS

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: terminal color setup
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool Console_ColorInit()
{
	HANDLE hOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD dwMode = NULL;

	GetConsoleMode(hOutput, &dwMode); // Some editions of Windows have 'VirtualTerminalLevel' disabled by default.
	dwMode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;

	if (!SetConsoleMode(hOutput, dwMode))
		return false; // Failure.

	SetConsoleBackgroundColor(0x00000000);
	AnsiColors_Init();

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: terminal window shutdown
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool Console_Shutdown()
{
	///////////////////////////////////////////////////////////////////////////
	// Destroy the console window
	if (FreeConsole() == FALSE)
	{
		char szBuf[2048];
		snprintf(szBuf, sizeof(szBuf), "Failed to destroy console window! [%s]\n", 
			std::system_category().message(static_cast<int>(::GetLastError())).c_str());

		OutputDebugStringA(szBuf);
		return false;
	}

	return true;
}

#ifndef _TOOLS
//#############################################################################
// CONSOLE WORKER
//#############################################################################
//-----------------------------------------------------------------------------
// Purpose: dedicated console command input.
//
// Reads its own stdin, so the server executes what is typed at it -- native,
// with no client process and no RCON hop in between. A hosted run replaces
// stdin with the launcher pipe through _dup2, which std::cin does not reliably
// follow, so this reads the live STD_INPUT_HANDLE instead.
//
// Dispatch mirrors the RCON entry point rather than the command buffer. Cbuf
// hands the callback an argv-rebuilt CCommand, and CCommand's break set splits
// on "{}()':" -- so `script printt(x)` reaches the VM as re-joined fragments.
// Cmd_ExecuteUnrestricted tokenizes the raw line, leaving ArgS() byte-exact.
// Anything the cvar system does not own, and any ';' chain, still goes to Cbuf
// -- CBUF_SERVER, never CBUF_FIRST_PLAYER. This binary still has a listen-server
// client buffer at slot 0; unknown text there runs as a client command.
//-----------------------------------------------------------------------------
DWORD __stdcall ProcessConsoleWorker(LPVOID)
{
	char buf[1024];

	while (true)
	{
		const HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
		if (hIn == INVALID_HANDLE_VALUE || hIn == NULL)
			break;

		DWORD bytesRead = 0;
		const BOOL ok = (GetFileType(hIn) == FILE_TYPE_PIPE)
			? ReadFile(hIn, buf, sizeof(buf) - 2, &bytesRead, NULL)
			: ReadConsoleA(hIn, buf, sizeof(buf) - 2, &bytesRead, NULL);

		if (!ok)
			break;

		while (bytesRead > 0 && (buf[bytesRead - 1] == '\n' || buf[bytesRead - 1] == '\r'))
			bytesRead--;

		if (bytesRead == 0)
			continue;

		buf[bytesRead] = '\0';

		// The console is built before Systems_Init resolves either of these.
		if (!Cbuf_AddText || !g_pCVar)
		{
			OutputDebugStringA("R5F server console: command dropped, command system not resolved\n");
			continue;
		}

		char szCommand[128];
		size_t nName = 0;
		while (nName < bytesRead && nName < sizeof(szCommand) - 1
			&& buf[nName] != ' ' && buf[nName] != '\t')
		{
			szCommand[nName] = buf[nName];
			++nName;
		}
		szCommand[nName] = '\0';

		Msg(eDLL_T::SERVER, "[CON-STDIN] %s\n", buf);

		const bool bChained = V_stricmp(szCommand, "script") != 0 && strchr(buf, ';') != NULL;

		ConCommandBase* const pBase = (nName > 0) ? g_pCVar->FindCommandBase(szCommand) : nullptr;
		// This binary still carries the listen-server client registry. A
		// client-only name must not run from the dedicated console --
		// Cbuf_GetCurrentPlayer is CBUF_FIRST_PLAYER (the client buffer).
		if (pBase && pBase->IsFlagSet(FCVAR_CLIENTDLL) && !pBase->IsFlagSet(FCVAR_GAMEDLL))
		{
			Warning(eDLL_T::SERVER,
				"Command '%s' is client-only; not executed on the dedicated server\n",
				szCommand);
			continue;
		}

		if (!bChained && nName > 0 && Cmd_ExecuteUnrestricted(szCommand, buf))
			continue;

		buf[bytesRead] = '\n';
		buf[bytesRead + 1] = '\0';

		Cbuf_AddText(ECommandTarget_t::CBUF_SERVER, buf, cmd_source_t::kCommandSrcCode);
	}

	return NULL;
}
#endif // !_TOOLS

#include "windows/console_hosted.inl"
#endif // CLIENT_DLL
