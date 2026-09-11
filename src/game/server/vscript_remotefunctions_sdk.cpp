//=============================================================================//
// Purpose: Shared state + server-side entry points for Remote_RegisterServerFunction.
//=============================================================================//

#include "core/stdafx.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
// NOTE: SCRIPT_REGISTER_FUNC must be in scope before vsquirrel.h parses the template.
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/scriptremotefunctions_shared.h"
#include "game/shared/vscript_remotefunctions_sdk.h"

// Inlined copy of DEFINE_SERVER_SCRIPTFUNC_NAMED from game/server/vscript_server.h.
// We reproduce it here so this translation unit (in game_shared_static) does not
// need to include the server header and pull in server-side transitive deps.
#define DEFINE_SERVER_SCRIPTFUNC_NAMED(s, functionName, helpString, returnType, parameters, isVariadic, ...) \
	Script_RegisterFuncNamed(s, MKSTRING(functionName), MKSTRING(Server_Script_##functionName),  \
	helpString, returnType, parameters, isVariadic, ServerScript_##functionName, __VA_ARGS__)

//-----------------------------------------------------------------------------
// Shared registration-table state (see vscript_remotefunctions_sdk.h).
//-----------------------------------------------------------------------------
RemoteFuncClientState_e g_eClientRemoteFuncState = RemoteFuncClientState_e::INACTIVE;
ClientRemoteFuncEntry_t g_clientRemoteFuncTable[SCRIPT_REMOTE_SERVER_MAX_FUNCTIONS];
int g_nClientRemoteFuncCount = 0;

//-----------------------------------------------------------------------------
// Report a script-facing error without assuming SQVM_RaiseError resolved.
// Where that pointer is null, calling it executes address 0 and takes the
// process down instead of reporting the bad argument.
//-----------------------------------------------------------------------------
static void RemoteFunc_RaiseError(HSQUIRRELVM v, const char* pszFormat, ...)
{
	char szMessage[512];

	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szMessage, sizeof(szMessage), pszFormat, args);
	va_end(args);

	szMessage[sizeof(szMessage) - 1] = '\0';

	if (v_SQVM_RaiseError)
		v_SQVM_RaiseError(v, "%s", szMessage);
	else
		Warning(eDLL_T::SERVER, "[SR-ERR] %s", szMessage);
}

//-----------------------------------------------------------------------------
// Linear search by name. Small table, called infrequently.
//-----------------------------------------------------------------------------
const ClientRemoteFuncEntry_t* Script_Remote_FindFunc(const char* pszName)
{
	for (int i = 0; i < g_nClientRemoteFuncCount; i++)
	{
		if (strcmp(g_clientRemoteFuncTable[i].szName, pszName) == 0)
			return &g_clientRemoteFuncTable[i];
	}
	return nullptr;
}

//-----------------------------------------------------------------------------
// Clear all client-side registrations. Called on disconnect / map change from
// CServerGameDLL::LevelShutdown (gameinterface.cpp) and from the
// Begin/Register handlers below.
//-----------------------------------------------------------------------------
void Script_ClearRemoteFunctionRegistrations()
{
	g_eClientRemoteFuncState = RemoteFuncClientState_e::INACTIVE;
	g_nClientRemoteFuncCount = 0;
	memset(g_clientRemoteFuncTable, 0, sizeof(g_clientRemoteFuncTable));
}

//=============================================================================//
// Shared SQRESULT implementations
//=============================================================================//

//-----------------------------------------------------------------------------
// void Remote_BeginRegisteringServerFunctions
//-----------------------------------------------------------------------------
SQRESULT Script_Remote_BeginRegistering_Impl(HSQUIRRELVM v)
{
	if (g_eClientRemoteFuncState != RemoteFuncClientState_e::INACTIVE)
	{
		RemoteFunc_RaiseError(v, "Remote_BeginRegisteringServerFunctions: already in state %d\n",
			static_cast<int>(g_eClientRemoteFuncState));
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	g_eClientRemoteFuncState = RemoteFuncClientState_e::REGISTERING;
	g_nClientRemoteFuncCount = 0;
	ScriptRemoteServer_ClearRegistrations();

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// void Remote_EndRegisteringServerFunctions
//-----------------------------------------------------------------------------
SQRESULT Script_Remote_EndRegistering_Impl(HSQUIRRELVM v)
{
	if (g_eClientRemoteFuncState != RemoteFuncClientState_e::REGISTERING)
	{
		RemoteFunc_RaiseError(v, "Remote_EndRegisteringServerFunctions: not in registering state\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	g_eClientRemoteFuncState = RemoteFuncClientState_e::ACTIVE;

	ScriptRemoteServer_LockRegistrations();

	DevMsg(eDLL_T::CLIENT, "ScriptRemoteClient: registered %d server functions\n", g_nClientRemoteFuncCount);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// void Remote_RegisterServerFunction(string functionName,...)
//-----------------------------------------------------------------------------
SQRESULT Script_Remote_RegisterServerFunction_Impl(HSQUIRRELVM v)
{
	// Scripts call engine Remote_BeginRegisteringFunctions which does NOT arm
	// this SDK state machine. Auto-enter REGISTERING from INACTIVE (S21-compat).
	// Refuse ACTIVE -- post-lock growth desyncs client/server indices.
	if (g_eClientRemoteFuncState == RemoteFuncClientState_e::INACTIVE)
	{
		g_eClientRemoteFuncState = RemoteFuncClientState_e::REGISTERING;
		g_nClientRemoteFuncCount = 0;
		ScriptRemoteServer_ClearRegistrations();
	}
	else if (g_eClientRemoteFuncState != RemoteFuncClientState_e::REGISTERING)
	{
		RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: not in registering state (state=%d)\n",
			static_cast<int>(g_eClientRemoteFuncState));
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	if (g_nClientRemoteFuncCount >= SCRIPT_REMOTE_SERVER_MAX_FUNCTIONS)
	{
		RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: too many functions registered\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	const SQChar* pszFuncName = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &pszFuncName)) || !pszFuncName || !*pszFuncName)
	{
		RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: invalid function name\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	// Skip if already registered
	for (int i = 0; i < g_nClientRemoteFuncCount; i++)
	{
		if (strcmp(g_clientRemoteFuncTable[i].szName, pszFuncName) == 0)
		{
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
	}

	ClientRemoteFuncEntry_t entry;
	memset(&entry, 0, sizeof(entry));
	V_strncpy(entry.szName, pszFuncName, sizeof(entry.szName));
	entry.nParamCount = 0;

	const SQInteger nTop = sq_gettop(v);
	SQInteger idx = 3; // Start after function name

	while (idx <= nTop)
	{
		if (entry.nParamCount >= SCRIPT_REMOTE_SERVER_MAX_PARAMS)
		{
			RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' too many params\n", pszFuncName);
			SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
		}

		const SQChar* pszType = nullptr;
		if (SQ_FAILED(sq_getstring(v, idx, &pszType)))
		{
			RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' expected type string at arg %d\n",
				pszFuncName, static_cast<int>(idx));
			SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
		}

		ScriptRemoteParamDesc_t& param = entry.params[entry.nParamCount];
		memset(&param, 0, sizeof(param));

		if (strcmp(pszType, "int") == 0)
		{
			// "int", min, max[, precision] — need 2+ more args
			param.type = ScriptRemoteParamType_e::SRP_INT;
			if (idx + 2 > nTop)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' int param missing min/max\n",
					pszFuncName);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			SQInteger intMin = 0, intMax = 0;
			sq_getinteger(v, idx + 1, &intMin);
			sq_getinteger(v, idx + 2, &intMax);

			if (intMax < intMin)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' int max < min (%d < %d)\n",
					pszFuncName, static_cast<int>(intMax), static_cast<int>(intMin));
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			param.intMin = static_cast<int>(intMin);
			param.intMax = static_cast<int>(intMax);
			idx += 3;
		}
		else if (strcmp(pszType, "float") == 0)
		{
			// "float", min, max, bits — need 3 more args
			param.type = ScriptRemoteParamType_e::SRP_FLOAT;
			if (idx + 3 > nTop)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' float param needs min, max, bits\n",
					pszFuncName);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			SQFloat fMin = 0.0f, fMax = 0.0f;
			sq_getfloat(v, idx + 1, &fMin);
			sq_getfloat(v, idx + 2, &fMax);

			SQInteger bits = 32;
			sq_getinteger(v, idx + 3, &bits);

			if (bits < 1 || bits > 32)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' float bit count must be 1-32\n",
					pszFuncName);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			if (fMin >= fMax)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' float min >= max [%f, %f]\n",
					pszFuncName, fMin, fMax);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			param.floatMin = fMin;
			param.floatMax = fMax;
			param.floatBits = static_cast<int>(bits);
			idx += 4;
		}
		else if (strcmp(pszType, "bool") == 0)
		{
			param.type = ScriptRemoteParamType_e::SRP_BOOL;
			idx += 1;
		}
		else if (strcmp(pszType, "vector") == 0)
		{
			// "vector", min, max, bits — same format as float
			param.type = ScriptRemoteParamType_e::SRP_VECTOR;
			if (idx + 3 > nTop)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' vector param needs min, max, bits\n",
					pszFuncName);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			SQFloat fMin = 0.0f, fMax = 0.0f;
			sq_getfloat(v, idx + 1, &fMin);
			sq_getfloat(v, idx + 2, &fMax);

			SQInteger bits = 32;
			sq_getinteger(v, idx + 3, &bits);

			if (bits < 1 || bits > 32)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' vector bit count must be 1-32\n",
					pszFuncName);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			if (fMin >= fMax)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' vector min >= max [%f, %f]\n",
					pszFuncName, fMin, fMax);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			param.floatMin = fMin;
			param.floatMax = fMax;
			param.floatBits = static_cast<int>(bits);
			idx += 4;
		}
		else if (strcmp(pszType, "entity") == 0)
		{
			param.type = ScriptRemoteParamType_e::SRP_ENTITY;
			idx += 1;
		}
		else if (strcmp(pszType, "typed_entity") == 0)
		{
			// "typed_entity", className -- resolve EHandle and verify classname.
			param.type = ScriptRemoteParamType_e::SRP_TYPED_ENTITY;
			if (idx + 1 > nTop)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' typed_entity needs a class name\n",
					pszFuncName);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			const SQChar* pszClass = nullptr;
			if (SQ_FAILED(sq_getstring(v, idx + 1, &pszClass)) || !pszClass || !*pszClass)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' typed_entity class name must be a non-empty string\n",
					pszFuncName);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}
			V_strncpy(param.szEntityClass, pszClass, sizeof(param.szEntityClass));
			idx += 2;
		}
		else if (strcmp(pszType, "itemflavor") == 0)
		{
			// "itemflavor", int [, int] — 2 or 3 args (single type or range)
			param.type = ScriptRemoteParamType_e::SRP_ITEMFLAVOR;
			if (idx + 1 > nTop)
			{
				RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' itemflavor needs at least 1 int\n",
					pszFuncName);
				SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
			}

			SQInteger itemType1 = 0;
			sq_getinteger(v, idx + 1, &itemType1);
			param.intMin = static_cast<int>(itemType1);
			idx += 2;

			// Optional second int (range max)
			if (idx <= nTop)
			{
				SQInteger testVal;
				if (SQ_SUCCEEDED(sq_getinteger(v, idx, &testVal)))
				{
					param.intMax = static_cast<int>(testVal);
					idx++;
				}
			}
		}
		else
		{
			RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' unknown type '%s'\n",
				pszFuncName, pszType);
			SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
		}

		entry.nParamCount++;
	}

	if (!ScriptRemoteServer_RegisterFunction(entry.szName, entry.nParamCount, entry.params))
	{
		RemoteFunc_RaiseError(v, "Remote_RegisterServerFunction: '%s' rejected by allowlist\n", pszFuncName);
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	entry.nIndex = static_cast<uint16_t>(g_nClientRemoteFuncCount);
	g_clientRemoteFuncTable[g_nClientRemoteFuncCount] = entry;
	g_nClientRemoteFuncCount++;

	DevMsg(eDLL_T::CLIENT, "ScriptRemoteClient: registered '%s' (index=%d, params=%d)\n",
		pszFuncName, entry.nIndex, entry.nParamCount);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// bool Remote_ServerCallFunctionAllowed
//-----------------------------------------------------------------------------
SQRESULT Script_Remote_ServerCallFunctionAllowed_Impl(HSQUIRRELVM v)
{
	// Engine End does not flip us to ACTIVE; any non-INACTIVE means table is live.
	sq_pushbool(v, g_eClientRemoteFuncState != RemoteFuncClientState_e::INACTIVE);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================//
// Server-VM aliases. DEFINE_SERVER_SCRIPTFUNC_NAMED expects ServerScript_<name>.
//=============================================================================//

static SQRESULT ServerScript_Remote_BeginRegisteringServerFunctions(HSQUIRRELVM v)
{
	return Script_Remote_BeginRegistering_Impl(v);
}

static SQRESULT ServerScript_Remote_EndRegisteringServerFunctions(HSQUIRRELVM v)
{
	return Script_Remote_EndRegistering_Impl(v);
}

static SQRESULT ServerScript_Remote_RegisterServerFunction(HSQUIRRELVM v)
{
	return Script_Remote_RegisterServerFunction_Impl(v);
}

//-----------------------------------------------------------------------------
// Server does not call server functions over the wire — this is a no-op when
// invoked from a server VM, matching the previous behavior from
// vscript_remotefunctions.cpp.
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_Remote_ServerCallFunction(HSQUIRRELVM v)
{
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_Remote_ServerCallFunctionAllowed(HSQUIRRELVM v)
{
	sq_pushbool(v, true);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================//
// Registration
//=============================================================================//

void Script_RegisterRemoteFunctionServerNatives(CSquirrelVM* s)
{
	DEFINE_SERVER_SCRIPTFUNC_NAMED(s, Remote_BeginRegisteringServerFunctions,
		"Begin remote server function registration phase",
		"void", "", false);

	DEFINE_SERVER_SCRIPTFUNC_NAMED(s, Remote_EndRegisteringServerFunctions,
		"End remote server function registration phase",
		"void", "", false);

	DEFINE_SERVER_SCRIPTFUNC_NAMED(s, Remote_RegisterServerFunction,
		"Register a function the client can invoke on the server. Args: functionName, [type, min, max, ...]",
		"void", "string functionName, ...", true);

	DEFINE_SERVER_SCRIPTFUNC_NAMED(s, Remote_ServerCallFunction,
		"Call a registered server function with arguments",
		"void", "string functionName, ...", true);

	DEFINE_SERVER_SCRIPTFUNC_NAMED(s, Remote_ServerCallFunctionAllowed,
		"Returns true if remote server function calls are allowed",
		"bool", "", false);
}
