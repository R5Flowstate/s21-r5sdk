//=============================================================================//
//
// Purpose: [TITAN] client diagnostics (bridge_titan_diag): titan soul and titan
// NPC creation, the fields the dedi replicates onto them, the local player's
// soul and cockpit links. Off by default.
//
//=============================================================================//
#ifndef CLIENT_TITAN_DIAG_H
#define CLIENT_TITAN_DIAG_H

// Once per host frame; does nothing while bridge_titan_diag is 0.
void TitanDiag_OnFrame(void);

#endif // CLIENT_TITAN_DIAG_H
