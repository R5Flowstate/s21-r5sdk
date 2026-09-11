#pragma once

void SetConsoleBackgroundColor(COLORREF color);
void FlashConsoleBackground(int nFlashCount, int nFlashInterval, COLORREF color);

bool Console_Init(const bool bAnsiColor);
bool Console_ColorInit();
bool Console_Shutdown();

// R5F_HOSTED_CONSOLE=1 routes stdio to the launcher. R5F_CONSOLE_PIPE /
// R5F_CONSOLE_IN are \\.\pipe\r5f-* stdout and stdin.
//
// The console is still allocated: engine logging writes through console APIs
// and goes silent without one. Call Console_HideIfHosted the moment it exists.
// The launcher also starts the process with a hidden show state, so the window
// AllocConsole creates never appears in the first place.
bool Console_HostedWanted(void);
void Console_HideIfHosted(void);
void Console_ApplyHostedSession(void);
