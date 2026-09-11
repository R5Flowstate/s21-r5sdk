#pragma once

/////////////////////////////////////////////////////////////////////////////
// Internals
void Input_Init();
void Input_Shutdown();

// Remember the game HWND so clip helpers can run from Present / user32
// hooks without a caller-supplied window.
void Input_NoteGameWindow(HWND hWnd);

// Modal ImGui taking the cursor: confine to the game window (never
// ClipCursor(nullptr) -- that is the whole virtual desktop, i.e. every
// monitor) and show the OS cursor. Latches whether look had the cursor
// hidden so leave can restore it.
void Input_ReleaseCursorClip(HWND hWnd);

// Re-apply ClipCursor to the game window rect. Does NOT change
// ShowCursor visibility -- caller chooses hide vs show.
void Input_RestoreCursorClip(HWND hWnd);

// Clip to the game window. No-op on a bad HWND.
void Input_ConfineCursorToWindow(HWND hWnd);

// If the game window is foreground and the OS clip is the full virtual
// desktop (no clip), confine. Heals alt-tab and any leftover unclip.
void Input_MaintainCursorClip(HWND hWnd);

// True if the OS cursor was hidden when the last modal took ownership.
bool Input_WasLookCursorHidden(void);

// Force OS cursor visible (ImGui / GameUI) via ShowCursor. Hide is
// SetCursor(NULL) only -- never a negative display counter.
// Never touch CInputSystem::m_bCursorVisible -- SetCursorIcon gates on it.
void Input_EnsureCursorVisible(void);
void Input_EnsureCursorHidden(void);

/////////////////////////////////////////////////////////////////////////////
// Globals
extern std::atomic_bool g_bBlockInput;

/////////////////////////////////////////////////////////////////////////////
