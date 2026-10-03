//=============================================================================//
//
// Purpose: [TITAN] dedi diagnostics (bridge_titan_diag): titan soul, titan NPC
// and titan-class player state as the dedi publishes it. Off by default.
//
//=============================================================================//
#ifndef SERVER_TITAN_DIAG_H
#define SERVER_TITAN_DIAG_H

// Once per simulated server frame; does nothing while bridge_titan_diag is 0.
void TitanDiag_Frame(void);

#endif // SERVER_TITAN_DIAG_H
