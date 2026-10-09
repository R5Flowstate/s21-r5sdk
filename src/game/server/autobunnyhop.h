//=============================================================================//
//
// Purpose: auto bunny hop on the dedicated server; the client twin is
// game/client/autobunnyhop.cpp.
//
//=============================================================================//
#ifndef AUTOBUNNYHOP_H
#define AUTOBUNNYHOP_H

// Sampled from VJetDrive's FullWalkMove hook. That class owns the only attach.
void AutoBunnyHop_BeforeFullWalkMove(void* ctx);

#endif // AUTOBUNNYHOP_H
