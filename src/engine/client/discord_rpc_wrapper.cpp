//=============================================================================//
//
// Purpose: Thin wrapper over Discord RPC integration
//
//=============================================================================//
#include "core/stdafx.h"

#ifndef DEDICATED

#include "discord_rpc_wrapper.h"
#include "discord_ipc.h"

//-----------------------------------------------------------------------------
// Discord RPC Wrapper - native IPC, no third-party DLL.
// autoRegister is ignored: this product never writes protocol-handler or
// Steam registry keys on the user's machine.
//-----------------------------------------------------------------------------
extern "C" {

void Discord_Initialize(const char* applicationId, DiscordEventHandlers* handlers, int autoRegister, const char* optionalPlatformId)
{
	(void)autoRegister;
	(void)optionalPlatformId;
	CDiscordIpc::Initialize(applicationId, handlers);
}

void Discord_Shutdown(void)
{
	CDiscordIpc::Shutdown();
}

void Discord_RunCallbacks(void)
{
	CDiscordIpc::RunCallbacks();
}

void Discord_UpdatePresence(const DiscordRichPresence* presence)
{
	CDiscordIpc::UpdatePresence(presence);
}

void Discord_ClearPresence(void)
{
	CDiscordIpc::ClearPresence();
}

void Discord_Respond(const char* userid, int reply)
{
	// Join requests are never solicited (no joinSecret is published).
	(void)userid;
	(void)reply;
}

void Discord_UpdateHandlers(DiscordEventHandlers* handlers)
{
	CDiscordIpc::UpdateHandlers(handlers);
}

} // extern "C"

#endif // !DEDICATED
