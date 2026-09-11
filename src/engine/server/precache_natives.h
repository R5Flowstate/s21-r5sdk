//=============================================================================//
//
// Purpose: PrecacheSkinName / PrecacheOnDemandLoadModel natives.
// SkinNames table: maxEntries=1024, userDataMaxSize=0, allowClientSideAddString=1.
//
//=============================================================================//
#ifndef PRECACHE_NATIVES_DEDI_H
#define PRECACHE_NATIVES_DEDI_H

#include "vscript/languages/squirrel_re/include/squirrel.h"

class CSquirrelVM;

void PrecacheSkinName_Add(const char* skinName);

void PrecacheSkinName_PopulateAll();

void Script_RegisterPrecacheServerNatives(CSquirrelVM* s);

SQRESULT Script_Zipline_IsCurvedZipline(HSQUIRRELVM v);

// Rope colour modulation via dt_extend DT_Zipline.m_ropeColorModulation.
// Class-gated to "zipline"; unavailable offset is a loud no-op / zero vector.
SQRESULT Script_Zipline_SetRopeColorModulation(HSQUIRRELVM v);
SQRESULT Script_Zipline_GetRopeColorModulation(HSQUIRRELVM v);

// Class-gated to info_loot_ceremony_harvester; slot [0, 128); field +0x15E0.
SQRESULT Script_GetUseStateByIndex(HSQUIRRELVM v);
SQRESULT Script_SetUseStateByIndex(HSQUIRRELVM v);

SQRESULT Script_GetTriggersByClassesInRealms(HSQUIRRELVM v);
SQRESULT Script_GetTriggersByClassesInRealms_HullSize(HSQUIRRELVM v);

// Drop cached SkinNames / modelprecache pointers; they UAF across changelevel.
void PrecacheNativesDedi_LevelShutdown();

// Replicated modelprecache AddString (no 0xB0BA user data). Appends ".rmdl".
void PrecacheModel_RegisterReplicated(const char* modelName);

// True once modelprecache exists; bulk placement register must wait.
bool PrecacheModel_IsModelPrecacheReady();

// Re-PrecacheModel every PODLM-tracked name once model_t binding sticks.
int PrecacheNatives_ReplayOdlOnMapSpawn();

#endif // PRECACHE_NATIVES_DEDI_H
