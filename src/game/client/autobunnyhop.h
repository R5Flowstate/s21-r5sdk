//=============================================================================//
//
// Purpose: auto bunny hop, client half -- a held jump counts as a fresh press
// on every grounded command, as CS:GO's sv_autobunnyhopping does. The dedi
// runs the same rule (game/server/autobunnyhop.cpp), so prediction agrees.
//
//=============================================================================//
#ifndef CLIENT_AUTOBUNNYHOP_H
#define CLIENT_AUTOBUNNYHOP_H

// Sampled from VMoveSimTraceClient's FullWalkMove hook. That class owns the only attach.
void AutoBunnyHopClient_BeforeFullWalkMove(void* ctx);

#endif // CLIENT_AUTOBUNNYHOP_H
