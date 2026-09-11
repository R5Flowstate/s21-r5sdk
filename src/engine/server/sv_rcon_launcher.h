//=============================================================================//
//
// Purpose: Loopback RCON session for a launcher-spawned dedicated server.
//
//=============================================================================//
#pragma once

#ifndef CLIENT_DLL

// Password is taken from env R5F_LOCAL_RCON when FROM_R5F_LAUNCHER=1.
// Never from argv -- command lines are logged.
// Authenticated loopback exec is the dedi console (commands + ConVars).
//
// Multi-instance invariant: one env per launcher tree (R5F_LOCAL_RCON /
// R5F_LOCAL_RCON_PORT), one TCP port per dedi instance; the client only
// talks to its last loopback host (cl_rcon_launcher gate). The dedi never
// falls through to another port while its latched port is occupied.
// Cookie rotation: the launcher mints R5F_LOCAL_RCON fresh per boot; the
// dedi latches it once and keeps it for its process lifetime (a dedi
// restart re-latches the same launcher boot's cookie). Both sides WipeEnv
// immediately after latching so the secret never lingers in the environment.
void RCON_LauncherSession_Think(void);
bool RCON_LauncherSession_Active(void);
bool RCON_LauncherSession_IgnorePasswordClear(void);
bool RCON_LauncherSession_AllowExec(const char* pszCommand, const char* pszValue);
int RCON_LauncherSession_Port(void);

// After Host_NewGame / changelevel succeeds. Named event R5F_HOST_READY if set.
void RCON_LauncherSession_NotifyHostReady(const char* pszMap);

#endif // !CLIENT_DLL
