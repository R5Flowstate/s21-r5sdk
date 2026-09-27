#pragma once

// True when the launcher that started this client listens for handoff requests.
bool LauncherHandoff_IsAvailable(void);

// Asks the launcher to install what the server at host[:port] requires, then
// restart this client into it. The launcher resolves the requirements from the
// master-server listing and asks the player before downloading anything.
bool LauncherHandoff_RequestModJoin(const char* pszTarget);
