#include "core/stdafx.h"
#include "windows/input.h"

/*-----------------------------------------------------------------------------
 * _input.cpp
 *
 * user32 cursor detours for modal ImGui (console / browser / dev menu).
 *
 * ClipCursor(nullptr) is the full virtual desktop. One call lets the
 * cursor walk onto every monitor; do not issue it while our window is
 * foreground. Modal UI only needs the clip expanded to the window rect.
 *
 * While g_bBlockInput is true:
 * - SetCursorPos is swallowed so the engine cannot recenter under the UI
 * - ClipCursor from the engine is ignored (keeps the window confine;
 *   a 1x1 look-clip or a nullptr would either trap or free the cursor)
 * - ShowCursor from the engine is swallowed (visibility is owned by
 *   Input_EnsureCursorVisible/Hidden on enter/leave)
 *-----------------------------------------------------------------------------*/

///////////////////////////////////////////////////////////////////////////////
typedef BOOL(WINAPI* IGetCursorPos)(LPPOINT lpPoint);
typedef BOOL(WINAPI* ISetCursorPos)(int nX, int nY);
typedef BOOL(WINAPI* IClipCursor)(const RECT* lpRect);
// ShowCursor returns the display counter (int), not BOOL.
typedef int (WINAPI* IShowCursor)(BOOL bShow);

///////////////////////////////////////////////////////////////////////////////
static IGetCursorPos            g_oGetCursorPos             = nullptr;
static ISetCursorPos            g_oSetCursorPos             = nullptr;
static IClipCursor              g_oClipCursor               = nullptr;
static IShowCursor              g_oShowCursor               = nullptr;

///////////////////////////////////////////////////////////////////////////////
static POINT                    g_pLastCursorPos              { 0 };
// Last known OS display counter from a real ShowCursor call (0 = just visible).
static int                      g_nShowCursorCount            = 0;
static HWND                     s_hClipWnd                    = nullptr;
static bool                     s_bLookCursorHiddenAtBlock    = false;
extern std::atomic_bool         g_bBlockInput               = false;

//#############################################################################
// INITIALIZATION
//#############################################################################

void Input_Setup()
{
	g_oSetCursorPos = (ISetCursorPos)DetourFindFunction("user32.dll", "SetCursorPos");
	g_oClipCursor   = (IClipCursor  )DetourFindFunction("user32.dll", "ClipCursor"  );
	g_oGetCursorPos = (IGetCursorPos)DetourFindFunction("user32.dll", "GetCursorPos");
	g_oShowCursor   = (IShowCursor  )DetourFindFunction("user32.dll", "ShowCursor"  );
}

//#############################################################################
// INPUT HOOKS
//#############################################################################

BOOL WINAPI HGetCursorPos(LPPOINT lpPoint)
{
	const BOOL ok = g_oGetCursorPos(lpPoint);
	if (ok && lpPoint)
	{
		g_pLastCursorPos.x = lpPoint->x;
		g_pLastCursorPos.y = lpPoint->y;
	}
	return ok;
}

BOOL WINAPI HSetCursorPos(int X, int Y)
{
	g_pLastCursorPos.x = X;
	g_pLastCursorPos.y = Y;

	if (g_bBlockInput)
	{
		// Engine recenter while a modal owns the cursor -- swallow.
		return TRUE;
	}

	return g_oSetCursorPos(X, Y);
}

BOOL WINAPI HClipCursor(const RECT* lpRect)
{
	if (g_bBlockInput)
	{
		// Keep the window confine. Engine look-clip is often a 1x1 at
		// center; nullptr is the full virtual desktop. Neither is usable
		// for in-window ImGui, and nullptr is the multi-monitor leak.
		(void)lpRect;
		return TRUE;
	}

	if (!lpRect)
	{
		const HWND hWnd = (s_hClipWnd && IsWindow(s_hClipWnd)) ? s_hClipWnd : nullptr;
		if (hWnd && GetForegroundWindow() == hWnd)
		{
			Input_ConfineCursorToWindow(hWnd);
			return TRUE;
		}
	}

	return g_oClipCursor(lpRect);
}

int WINAPI HShowCursor(BOOL bShow)
{
	if (g_bBlockInput)
	{
		// Swallow. Old code forced bShow=TRUE on every call, so every engine
		// ShowCursor(FALSE) (gameplay hide while modal still latched, or hide
		// on focus paths) became an increment -- Win32 display count stayed
		// positive after UI close and the cursor never hid again.
		// Visibility is owned by Input_EnsureCursorVisible/Hidden on enter/leave.
		return g_nShowCursorCount >= 0 ? g_nShowCursorCount : 0;
	}

	g_nShowCursorCount = g_oShowCursor(bShow);
	return g_nShowCursorCount;
}

//#############################################################################
// MANAGEMENT
//#############################################################################

void Input_EnsureCursorVisible(void)
{
	if (!g_oShowCursor)
		return;

	// Already drawn -- do not increment the display counter again.
	CURSORINFO ci{};
	ci.cbSize = sizeof(ci);
	if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))
		return;

	int safety = 0;
	int n = g_nShowCursorCount;
	do
	{
		n = g_oShowCursor(TRUE);
	} while (n < 0 && ++safety < 64);

	g_nShowCursorCount = n;
}

void Input_EnsureCursorHidden(void)
{
	// Shape-only. GameUI Esc applies the arrow with SetCursor and never
	// increments the Win32 display counter -- ShowCursor(FALSE) here makes
	// that arrow invisible.
	::SetCursor(NULL);
}

void Input_NoteGameWindow(HWND hWnd)
{
	if (hWnd && IsWindow(hWnd))
		s_hClipWnd = hWnd;
}

static bool Input_OsCursorHidden(void)
{
	CURSORINFO ci{};
	ci.cbSize = sizeof(ci);
	if (!GetCursorInfo(&ci))
		return g_nShowCursorCount < 0;
	return (ci.flags & CURSOR_SHOWING) == 0;
}

bool Input_WasLookCursorHidden(void)
{
	return s_bLookCursorHiddenAtBlock;
}

void Input_ConfineCursorToWindow(HWND hWnd)
{
	if (!hWnd || !IsWindow(hWnd))
		hWnd = s_hClipWnd;
	if (!g_oClipCursor || !hWnd || !IsWindow(hWnd))
		return;

	s_hClipWnd = hWnd;

	RECT rc = {};
	if (!GetWindowRect(hWnd, &rc))
		return;
	if (rc.right <= rc.left || rc.bottom <= rc.top)
		return;

	g_oClipCursor(&rc);
}

static bool Input_ClipIsUnconstrained(void)
{
	RECT clip = {};
	if (!GetClipCursor(&clip))
		return false;

	const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	if (vw <= 0 || vh <= 0)
		return false;

	return clip.left <= vx && clip.top <= vy &&
		clip.right >= vx + vw && clip.bottom >= vy + vh;
}

void Input_MaintainCursorClip(HWND hWnd)
{
	if (!hWnd || !IsWindow(hWnd))
		hWnd = s_hClipWnd;
	if (!hWnd || !IsWindow(hWnd))
		return;
	if (GetForegroundWindow() != hWnd)
		return;

	s_hClipWnd = hWnd;

	if (g_bBlockInput || Input_ClipIsUnconstrained())
	{
		static bool s_bLoggedUnconstrained = false;
		if (!g_bBlockInput && !s_bLoggedUnconstrained)
		{
			s_bLoggedUnconstrained = true;
			Warning(eDLL_T::COMMON, "[CURSOR] clip was unconstrained, confined to hwnd=%p\n",
				reinterpret_cast<void*>(hWnd));
		}
		Input_ConfineCursorToWindow(hWnd);
	}

	// Esc menu installs an icon via SetCursor; a leftover negative display
	// counter leaves that icon installed but not shown.
	if (!g_bBlockInput)
	{
		CURSORINFO ci{};
		ci.cbSize = sizeof(ci);
		if (GetCursorInfo(&ci) && ci.hCursor && !(ci.flags & CURSOR_SHOWING))
			Input_EnsureCursorVisible();
	}
}

void Input_ReleaseCursorClip(HWND hWnd)
{
	s_bLookCursorHiddenAtBlock = Input_OsCursorHidden();
	Input_ConfineCursorToWindow(hWnd);
	Input_EnsureCursorVisible();
}

void Input_RestoreCursorClip(HWND hWnd)
{
	Input_ConfineCursorToWindow(hWnd);
}

void Input_Init()
{
	Input_Setup();
	///////////////////////////////////////////////////////////////////////////
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

	///////////////////////////////////////////////////////////////////////////
	DetourAttach(&(LPVOID&)g_oGetCursorPos, (PBYTE)HGetCursorPos);
	DetourAttach(&(LPVOID&)g_oSetCursorPos, (PBYTE)HSetCursorPos);
	DetourAttach(&(LPVOID&)g_oClipCursor, (PBYTE)HClipCursor);
	DetourAttach(&(LPVOID&)g_oShowCursor, (PBYTE)HShowCursor);

	///////////////////////////////////////////////////////////////////////////
	HRESULT hr = DetourTransactionCommit();
	if (hr != NO_ERROR)
	{
		// Failed to hook into the process, terminate
		Assert(0);
		Error(eDLL_T::COMMON, 0xBAD0C0DE, "Failed to detour process: error code = %08x\n", hr);
	}
}

void Input_Shutdown()
{
	///////////////////////////////////////////////////////////////////////////
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

	///////////////////////////////////////////////////////////////////////////
	DetourDetach(&(LPVOID&)g_oGetCursorPos, (PBYTE)HGetCursorPos);
	DetourDetach(&(LPVOID&)g_oSetCursorPos, (PBYTE)HSetCursorPos);
	DetourDetach(&(LPVOID&)g_oClipCursor, (PBYTE)HClipCursor);
	DetourDetach(&(LPVOID&)g_oShowCursor, (PBYTE)HShowCursor);

	///////////////////////////////////////////////////////////////////////////
	DetourTransactionCommit();
}
