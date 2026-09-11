//=============================================================================//
//
// Purpose: Hosted-client latch of the launcher loopback RCON session.
//
//=============================================================================//
#pragma once

#ifndef DEDICATED

// Multi-instance invariant: one env per launcher tree (R5F_LOCAL_RCON /
// R5F_LOCAL_RCON_PORT), one TCP port per dedi instance; this client only
// connects while its last game host is loopback, plaintext AUTH only.
// Cookie rotation: the launcher mints R5F_LOCAL_RCON fresh per boot; both
// sides latch once, WipeEnv immediately, and keep it for process lifetime.
void RCON_LauncherClient_Think(void);
bool RCON_LauncherClient_Active(void);
bool RCON_LauncherClient_Ready(void);
bool RCON_LauncherClient_ShouldForward(const char* pszArg0);
bool RCON_LauncherClient_QueueExec(const char* pszLine);
bool RCON_LauncherClient_WantSendLogs(void);

#endif // !DEDICATED
