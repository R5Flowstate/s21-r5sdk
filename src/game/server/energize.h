#ifndef ENERGIZE_BRIDGE_H
#define ENERGIZE_BRIDGE_H

//=============================================================================//
//
// Purpose: Energize weapon mechanic bridge (fed-ammo damage/fire-rate buff)
//
//=============================================================================//
struct ScriptClassDescriptor_t;

// Energize FSM (NONE/ENERGIZING/ENERGIZED). m_lastEnergizeState is an edge detector:
// leaving last stuck at ENERGIZING re-fires the client's CheckForEnergize every snapshot.
// Networked: m_energizeState / m_lastEnergizeState / m_energizedEndTime / m_startEnergizingTime.
void EnergizeBridge_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);

#ifndef CLIENT_DLL
#include "vscript/ivscript.h"

bool WeaponBridge_InvokeChangeMod(HSCRIPT hWeaponScript, const char* modName, bool isAdding);

// Per-tick FSM on activeWeapons[0..2] that are hasEnergized or mid-FSM.
void EnergizeBridge_Think(void* pPlayer, void* pUserCmd);

// Wire accessor: appended slots alias live m_modVars. False if this weapon has no FSM entry.
bool EnergizeBridge_WireGet(void* pWeapon, int* pState, float* pStartTime, float* pEndTime);
#endif

// Clears all per-instance/per-class caches. Call on level shutdown (mirrors
// WeaponHeat_LevelShutdown / OffhandSlotsExt_LevelShutdown).
void EnergizeBridge_LevelShutdown();

#endif // ENERGIZE_BRIDGE_H
