//=============================================================================//
//
// Purpose: shared state and SQRESULT entry points for the
// Remote_RegisterServerFunction system. Lives in game_shared_static so the
// client-side (game/client) and server-side (game/server) code share one
// registration table.
//
// The send path that touches g_pClientState->m_NetChannel stays in
// game/client/vscript_remotefunctions.cpp -- dedicated builds exclude
// clientstate.cpp, so it cannot link here.
//
//=============================================================================//
#ifndef VSCRIPT_REMOTEFUNCTIONS_SDK_H
#define VSCRIPT_REMOTEFUNCTIONS_SDK_H

#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "scriptremotefunctions_shared.h"

enum class RemoteFuncClientState_e : int
{
	INACTIVE    = 0, // Not initialized
	REGISTERING = 1, // Between BeginRegistering / EndRegistering
	ACTIVE      = 2, // Registration complete, calls allowed
};

struct ClientRemoteFuncEntry_t
{
	char szName[128];
	uint16_t nIndex;  // Dictionary index for binary protocol
	int nParamCount;
	ScriptRemoteParamDesc_t params[SCRIPT_REMOTE_SERVER_MAX_PARAMS];
};

//-----------------------------------------------------------------------------
// Shared registration-table state. Exposed so the client-only send path
// (ClientScript_Remote_ServerCallFunction) can read entries from the same
// table that was populated by the shared registration handlers.
//-----------------------------------------------------------------------------
extern RemoteFuncClientState_e g_eClientRemoteFuncState;
extern ClientRemoteFuncEntry_t g_clientRemoteFuncTable[SCRIPT_REMOTE_SERVER_MAX_FUNCTIONS];
extern int g_nClientRemoteFuncCount;

const ClientRemoteFuncEntry_t* Script_Remote_FindFunc(const char* pszName);
void Script_ClearRemoteFunctionRegistrations();

//-----------------------------------------------------------------------------
// Shared SQRESULT handler implementations. The client-prefixed and UI-prefixed
// SQRESULT wrappers in game/client/vscript_remotefunctions.cpp delegate to these
// so there is one authoritative implementation.
//-----------------------------------------------------------------------------
SQRESULT Script_Remote_BeginRegistering_Impl(HSQUIRRELVM v);
SQRESULT Script_Remote_EndRegistering_Impl(HSQUIRRELVM v);
SQRESULT Script_Remote_RegisterServerFunction_Impl(HSQUIRRELVM v);
SQRESULT Script_Remote_ServerCallFunctionAllowed_Impl(HSQUIRRELVM v);

class CSquirrelVM;
void Script_RegisterRemoteFunctionServerNatives(CSquirrelVM* s);

#endif // VSCRIPT_REMOTEFUNCTIONS_SDK_H
