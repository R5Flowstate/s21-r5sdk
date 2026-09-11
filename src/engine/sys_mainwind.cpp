//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
//===========================================================================//
#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "windows/id3dx.h"
#include "windows/input.h"
#include "engine/sys_mainwind.h"
#include "engine/sys_engine.h"
#include "gameui/IConsole.h"
#include "gameui/IBrowser.h"
#include "gameui/imgui_system.h"
#include <gameui/ITopBar.h>
#include <gameui/IDevMenu.h>
#include "gameui/IDlssNrMenu.h"

// Not pulled in by every WINVER/_WIN32_WINNT configuration; guarded the same
// way imgui_impl_win32.cpp/SDL_windowsevents.c do it in this tree.
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

// Diagnostic log sink from dllmain.cpp -- we use it to prove whether the
// WindowProc detour fires at all in S21. See the one-shot log at the top
// of CGame::WindowProc below.
extern void SDK_Log(const char* fmt, ...);

volatile LONG g_imguiWndProcToggleSerial = 0;

//-----------------------------------------------------------------------------
// Purpose: plays the startup video's
//-----------------------------------------------------------------------------
void CGame::PlayStartupVideos(void)
{
	if (!CommandLine()->CheckParm("-novid"))
	{
		CGame__PlayStartupVideos();
	}
}

//-----------------------------------------------------------------------------
// Purpose: main windows procedure
//-----------------------------------------------------------------------------
LRESULT CGame::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{


	if (ImguiSystem()->IsInitialized())
		ImguiWindowProc(hWnd, uMsg, wParam, lParam);

	return CGame__WindowProc(hWnd, uMsg, wParam, lParam);
}

ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

//-----------------------------------------------------------------------------
// Purpose: imgui windows procedure
//-----------------------------------------------------------------------------
LRESULT CGame::ImguiWindowProc(HWND hWnd, UINT& uMsg, WPARAM wParam, LPARAM lParam)
{
	LRESULT hr = NULL;

	Input_NoteGameWindow(hWnd);
	if (uMsg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE)
		Input_MaintainCursorClip(hWnd);
	else if (uMsg == WM_SETFOCUS || uMsg == WM_MOUSEMOVE || uMsg == WM_INPUT)
		Input_MaintainCursorClip(hWnd);

	if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN)
	{
		const ImGuiKey imParam = ImGui_ImplWin32_KeyEventToImGuiKey(wParam, lParam);

		// (Per-keydown imgui-key-mapping trace removed -- same synchronous
		// main-thread flush cost as the WND-HOOK keydown trace above.)

		if (imParam == g_ImGuiConfig.m_ConsoleConfig.m_nBind0 ||
			imParam == g_ImGuiConfig.m_ConsoleConfig.m_nBind1)
		{
			// Serial marks WndProc as the owner so toggleconsole ConCommand /
			// DX12 fallback do not XOR cancel.
			g_Console.ToggleTab(CConsole::kTabConsole);
			InterlockedIncrement(&g_imguiWndProcToggleSerial);
		}

		if (imParam == g_ImGuiConfig.m_BrowserConfig.m_nBind0 ||
			imParam == g_ImGuiConfig.m_BrowserConfig.m_nBind1)
		{
			g_Console.ToggleTab(CConsole::kTabBrowser);
			InterlockedIncrement(&g_imguiWndProcToggleSerial);
		}

		if (imParam == g_ImGuiConfig.m_LocalConfig.m_nBind0 ||
			imParam == g_ImGuiConfig.m_LocalConfig.m_nBind1)
		{
			g_Console.ToggleTab(CConsole::kTabManageLocal);
			InterlockedIncrement(&g_imguiWndProcToggleSerial);
		}

		if (imParam == g_ImGuiConfig.m_DevMenuConfig.m_nBind0 ||
			imParam == g_ImGuiConfig.m_DevMenuConfig.m_nBind1)
		{
			// Repurpose old TopBar binds to toggle the Dev Menu instead
			ConVar* const cvDev = g_pCVar->FindVar("ui_devmenu_enable");
			if (cvDev)
			{
				const bool newEnable = !cvDev->GetBool();
				cvDev->SetValue(newEnable);
				// Ensure surface activation matches visibility for proper input handling
				g_DevMenu.SetActive(newEnable);
			}
			InterlockedIncrement(&g_imguiWndProcToggleSerial);
			ResetInput();
		}

		if (imParam == ImGuiKey_F9)
		{
			g_DlssNrMenu.ToggleActive();
			ResetInput();
			InterlockedIncrement(&g_imguiWndProcToggleSerial);
		}
	}

	if (ImguiSystem()->IsSurfaceActive())
	{//////////////////////////////////////////////////////////////////////////////
		hr = ImguiSystem()->MessageHandler(hWnd, uMsg, wParam, lParam);

		switch (uMsg)
		{
		// This is required as the game calls CInputStackSystem::SetCursorPosition,
		// which hides the cursor. It keeps calling it as the game window is the top
		// most window, even when the ImGui window is enabled. We could in the future
		// create a new input context for the imgui system, then push it to the stack
		// after the game's context and call CInputStackSystem::EnableInputContext
		// on the new imgui context.
		case WM_SETCURSOR:
			uMsg = WM_NULL;
			break;

		// Rewrite to WM_NULL so CGame__WindowProc does not also consume the key/click.
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
		case WM_CHAR:
		case WM_SYSCHAR:
		case WM_DEADCHAR:
		case WM_SYSDEADCHAR:
		case WM_MOUSEMOVE:
		case WM_MOUSEWHEEL:
		case WM_MOUSEHWHEEL:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_MBUTTONDBLCLK:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
		case WM_XBUTTONDBLCLK:
			uMsg = WM_NULL;
			break;

		// WM_INPUT is deliberately NOT swallowed here -- the raw mouse-look
		// accumulator it feeds is gated upstream instead, see VRawInputAccum
		// (inputsystem.h/.cpp), which hooks the accumulate step directly and
		// skips it while a modal ImGui surface has focus.

		default:
			break;
		}

		// Latch block + confine to the game window (not ClipCursor(nullptr)).
		if (!g_bBlockInput.exchange(true))
			Input_ReleaseCursorClip(hWnd);
	}//////////////////////////////////////////////////////////////////////////////
	else
	{
		if (g_bBlockInput.exchange(false))
		{
			// Dry run with kill focus msg to clear the keydown state, we have to do
			// this as the menu's can be closed while still holding down a key. That
			// key will remain pressed down so the next time a window is opened that
			// key will be spammed, until that particular key msg is sent here again.
			hr = ImguiSystem()->MessageHandler(hWnd, WM_KILLFOCUS, wParam, lParam);

			ResetInput();
		}
	}

	return hr;
}

//-----------------------------------------------------------------------------
// Purpose: gets the window rect
//-----------------------------------------------------------------------------
void CGame::GetWindowRect(int* const x, int* const y, int* const w, int* const h) const
{
	if (x)
	{
		*x = m_x;
	}
	if (y)
	{
		*y = m_y;
	}
	if (w)
	{
		*w = m_width;
	}
	if (h)
	{
		*h = m_height;
	}
}

//-----------------------------------------------------------------------------
// Purpose: sets the window position
//-----------------------------------------------------------------------------
void CGame::SetWindowPosition(const int x, const int y)
{
	m_x = x;
	m_y = y;
}

//-----------------------------------------------------------------------------
// Purpose: sets the window size
//-----------------------------------------------------------------------------
void CGame::SetWindowSize(const int w, const int h)
{
	m_width = w;
	m_height = h;
}

///////////////////////////////////////////////////////////////////////////////
void VGame::Detour(const bool bAttach) const
{
	// S21: Guard against null function pointers from unmatched patterns
	if (CGame__PlayStartupVideos)
		DetourSetup(&CGame__PlayStartupVideos, &CGame::PlayStartupVideos, bAttach);
	if (CGame__WindowProc)
		DetourSetup(&CGame__WindowProc, &CGame::WindowProc, bAttach);
}
