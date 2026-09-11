#ifndef GLOBALNONREWIND_VARS_H
#define GLOBALNONREWIND_VARS_H

//=============================================================================//
//
// Purpose: Global non-rewind networked var declarations
//
//=============================================================================//
#include "vscript/languages/squirrel_re/include/sqvm.h"

// Global NonRewind net vars (Get/Set by name)
SQRESULT Script_SetGlobalNonRewindNetBool(HSQUIRRELVM v);
SQRESULT Script_SetGlobalNonRewindNetInt(HSQUIRRELVM v);
SQRESULT Script_SetGlobalNonRewindNetFloat(HSQUIRRELVM v);
SQRESULT Script_SetGlobalNonRewindNetTime(HSQUIRRELVM v);
SQRESULT Script_SetGlobalNonRewindNetEnt(HSQUIRRELVM v);
SQRESULT Script_GetGlobalNonRewindNetBool(HSQUIRRELVM v);
SQRESULT Script_GetGlobalNonRewindNetInt(HSQUIRRELVM v);
SQRESULT Script_GetGlobalNonRewindNetFloat(HSQUIRRELVM v);
SQRESULT Script_GetGlobalNonRewindNetTime(HSQUIRRELVM v);
SQRESULT Script_GetGlobalNonRewindNetEnt(HSQUIRRELVM v);

// Per-player NonRewind (player methods)
SQRESULT Script_GetNonRewindRespawnTime(HSQUIRRELVM v);
SQRESULT Script_SetNonRewindRespawnTime(HSQUIRRELVM v);
SQRESULT Script_GetNonRewindMusicPack(HSQUIRRELVM v);
SQRESULT Script_SetNonRewindMusicPack(HSQUIRRELVM v);

void GlobalNonRewind_LevelShutdown();

#ifndef CLIENT_DLL
// Slot-indexed NonRewind misc data for the DT_GlobalNonRewinding.m_playerMiscData
// wire array. Returns false for an out-of-range slot.
bool GlobalNonRewind_GetSlotMisc(int slot, float* outRespawnTime, int* outMusicPack);
#endif // !CLIENT_DLL

#endif // GLOBALNONREWIND_VARS_H
