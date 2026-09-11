#pragma once

// Installs the offline platform guard: process-creation watchdog (always) and, when
// the client was launched with -offline, the byte patches that skip EA App install,
// spawn, and LSX handshake (LauncherMain still sees OriginStartup success).
void Origin_InstallOfflineGuard();
