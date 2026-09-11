//=============================================================================//
//
// Purpose: ScriptNetData extension shared declarations
//
//=============================================================================//
#if defined(CLIENT_DLL)
#ifndef SCRIPTNETDATA_EXT_H
#define SCRIPTNETDATA_EXT_H

//------------------------------------------------------------------------------
// NonRewind entity pointer, var-slot lookup, and level-shutdown helper.
//------------------------------------------------------------------------------
inline void* g_pScriptNetDataNonRewindEnt = nullptr;

// Var slot lookup for NonRewind natives -- searches engine's scriptNetVars
// hash table for a variable name with the given category.
// Returns slot index, or -1 on failure.
int ScriptNetData_FindVarSlot(const char* name, int expectedCategory);

// Lifecycle -- clear entity pointer on map shutdown
void ScriptNetDataExt_LevelShutdown();

#endif // SCRIPTNETDATA_EXT_H
#else // !CLIENT_DLL
#ifndef SCRIPTNETDATA_EXT_H
#define SCRIPTNETDATA_EXT_H

// SNDC_GLOBAL_NON_REWIND is category 5. Engine bounds patched to allow <=5; 6-entry copy, struct 0x20C.

#include "core/init.h"
#include "game/client/scriptnetdata_client.h"

// SDK-owned global pointer for the NonRewind entity
inline void* g_pScriptNetDataNonRewindEnt = nullptr;

// Var slot lookup for NonRewind natives — searches engine's scriptNetVars
// hash table for a variable name with the given category.
// Returns slot index, or -1 on failure.
int ScriptNetData_FindVarSlot(const char* name, int expectedCategory);

// FindVarSlot returns an index into a type-specific array; callers must use +0x0C SNVT.
int ScriptNetData_FindVarSlotAndType(const char* name, int expectedCategory, int* outType);

// Lifecycle — clear entity pointer on map shutdown
void ScriptNetDataExt_LevelShutdown();

// Zero changeCallback SQObjects in engine storage so a destroyed VM cannot crash the next dispatch.
void ScriptNetDataExt_ClearEngineCallbacks();

// TriggerGlobalChangeCallbacks — fires all registered netvar change callbacks
// for the GLOBAL entity. Called once from cl_mapspawn.gnut after init.
// S3 doesn't have this native; S21+ scripts expect it.
void ScriptNetDataExt_TriggerGlobalChangeCallbacks();

class VScriptNetDataExt : public IDetour
{
	virtual void GetAdr(void) const {}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};

#endif // SCRIPTNETDATA_EXT_H
#endif // CLIENT_DLL
