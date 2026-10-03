//=============================================================================//
//
// Purpose: pilot-only movement overrides step aside for titans so titan
// movement runs the engine's own code. Each product resolves the settings
// field that carries the general class (game/client/titan_gate.cpp,
// game/server/titan_gate.cpp).
//
//=============================================================================//
#ifndef SHARED_TITAN_GATE_H
#define SHARED_TITAN_GATE_H

// True while the player's class settings carry a titan general class. False for
// a null player, a player without class settings, before the settings layout
// loads, and while bridge_titan_gate is 0.
bool TitanGate_IsTitanPlayer(const void* pPlayer);

// Reads the general class without the bridge_titan_gate lever.
bool TitanGate_ReadIsTitanPlayer(const void* pPlayer);

// General classes 1 and 3 are the titan classes.
inline bool TitanGate_IsTitanClass(const uint32_t nGeneralClass)
{
	return ((nGeneralClass - 1u) & ~2u) == 0u;
}

#endif // SHARED_TITAN_GATE_H
