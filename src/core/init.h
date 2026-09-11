// Client boot: Systems_Init_S21(). Dedi boot: Systems_Init() + DetourInit().
#pragma once

// Client-only static cvar helpers (NOT dual convar.h -- see that file's PCH note).
#if defined(CLIENT_DLL)
#include "tier1/sdk_static_convar.h"
#endif // CLIENT_DLL

DLL_EXPORT void SDK_Init();
DLL_EXPORT void SDK_Shutdown();

void Systems_Init();       // dedi / hybrid: classic DetourInit + attach path
void Systems_Init_S21();   // client inject: crash-safe three-phase detour init
void Systems_Shutdown();

void Winsock_Startup();
void Winsock_Shutdown();
void DirtySDK_Startup();
void DirtySDK_Shutdown();

void DetourInit();
void DetourAddress();
void DetourRegister();

extern bool g_bSdkInitialized;
extern bool g_bSdkShutdownInitiatedFromConsoleHandler;
