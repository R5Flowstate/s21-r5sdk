//=============================================================================//
//
// Purpose: Client ScriptNetData category helpers
//
//=============================================================================//
#if defined(CLIENT_DLL)
#ifndef SCRIPTNETDATA_CLIENT_H
#define SCRIPTNETDATA_CLIENT_H

//------------------------------------------------------------------------------
// ScriptNetData - Networked Variable Change Callbacks for CLIENT/UI
// Engine only registers these for CLIENT VM; SDK provides them for UI VM too
//------------------------------------------------------------------------------

#include "vscript/vscript.h"

// SNDC - Script Net Data Category
// Engine has 5 categories (0-4). SDK adds GLOBAL_NON_REWIND as category 5.
enum ScriptNetDataCategory_e
{
	SNDC_GLOBAL = 0,
	SNDC_PLAYER_GLOBAL = 1,
	SNDC_PLAYER_EXCLUSIVE = 2,
	SNDC_TITAN_SOUL = 3,
	SNDC_DEATH_BOX = 4,
	SNDC_GLOBAL_NON_REWIND = 5,  // SDK extension -- appended to S3's 5 categories

	SNDC_COUNT = 6
};

// SNVT - Script Net Variable Type
enum ScriptNetVarType_e
{
	SNVT_BOOL = 0,
	SNVT_INT = 1,
	SNVT_UNSIGNED_INT = 2,
	SNVT_BIG_INT = 3,
	SNVT_FLOAT_RANGE = 4,
	SNVT_FLOAT_RANGE_OVER_TIME = 5,  // Cannot have callbacks
	SNVT_TIME = 6,
	SNVT_ENTITY = 7,

	SNVT_COUNT = 8
};

//------------------------------------------------------------------------------
// Script Function Registration
//------------------------------------------------------------------------------
void ScriptNetData_RegisterClientFunctions(CSquirrelVM* vm);
void ScriptNetData_RegisterUIFunctions(CSquirrelVM* vm);
void ScriptNetData_RegisterLimitsOnClient(CSquirrelVM* vm);
void ScriptNetData_RegisterLimitsOnUI(CSquirrelVM* vm);

//------------------------------------------------------------------------------
// Cleanup - called when VM is destroyed
//------------------------------------------------------------------------------
void ScriptNetData_OnVMDestroyed(CSquirrelVM* vm);

#endif // SCRIPTNETDATA_CLIENT_H
#else // !CLIENT_DLL
#ifndef SCRIPTNETDATA_CLIENT_H
#define SCRIPTNETDATA_CLIENT_H

//------------------------------------------------------------------------------
// ScriptNetData - Networked Variable Change Callbacks for CLIENT/UI
// Engine only registers these for CLIENT VM; SDK provides them for UI VM too
//------------------------------------------------------------------------------

#include "vscript/vscript.h"

// SNDC - Script Net Data Category
// Engine has 5 categories (0-4). SDK adds GLOBAL_NON_REWIND as category 5.
enum ScriptNetDataCategory_e
{
	SNDC_GLOBAL = 0,
	SNDC_PLAYER_GLOBAL = 1,
	SNDC_PLAYER_EXCLUSIVE = 2,
	SNDC_TITAN_SOUL = 3,
	SNDC_DEATH_BOX = 4,
	SNDC_GLOBAL_NON_REWIND = 5,  // SDK extension — appended to S3's 5 categories

	SNDC_COUNT = 6
};

// SNVT - Script Net Variable Type
enum ScriptNetVarType_e
{
	SNVT_BOOL = 0,
	SNVT_INT = 1,
	SNVT_UNSIGNED_INT = 2,
	SNVT_BIG_INT = 3,
	SNVT_FLOAT_RANGE = 4,
	SNVT_FLOAT_RANGE_OVER_TIME = 5,  // Cannot have callbacks
	SNVT_TIME = 6,
	SNVT_ENTITY = 7,

	SNVT_COUNT = 8
};

//------------------------------------------------------------------------------
// Script Function Registration
//------------------------------------------------------------------------------
void ScriptNetData_RegisterClientFunctions(CSquirrelVM* vm);
void ScriptNetData_RegisterUIFunctions(CSquirrelVM* vm);

//------------------------------------------------------------------------------
// Cleanup
//------------------------------------------------------------------------------
void ScriptNetData_OnVMDestroyed(CSquirrelVM* vm);
void ScriptNetData_LevelShutdown();

//------------------------------------------------------------------------------
// Overflow callback firing — called from SNDC_ProcessClientMessage when a
// NET_ScriptMessage updates an overflow var. Event-driven, no polling.
//------------------------------------------------------------------------------
void SNDC_FireOverflowCallbacks(const char* varName, int32_t oldValue, int32_t newValue);

//------------------------------------------------------------------------------
// Shared RegisterNetworkedVariableChangeCallback_* handler. type is ScriptNetVarType_e.
//------------------------------------------------------------------------------
SQRESULT ScriptNetData_HandleCallbackRegistration(HSQUIRRELVM v, int type);

#endif // SCRIPTNETDATA_CLIENT_H
#endif // CLIENT_DLL
