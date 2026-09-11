#if defined(CLIENT_DLL)
//=============================================================================//
// Purpose: connection-ping accessor for the performance HUD
//=============================================================================//

#ifndef RUITRACKS_H
#define RUITRACKS_H

class CSquirrelVM;

// No-op; S21 declares every RUI track natively.
void RuiTracks_RegisterMissingEnums(CSquirrelVM* const s);

// Performance HUD SPING (ms). 0 offline / not FULL.
int RuiTracks_GetConnectionPingMs();

#endif // RUITRACKS_H
#else // !CLIENT_DLL
#ifndef RUITRACKS_H
#define RUITRACKS_H

class CSquirrelVM;

void RuiTracks_RegisterMissingEnums(CSquirrelVM* const s);
int RuiTracks_GetConnectionPingMs();

#endif // RUITRACKS_H
#endif // CLIENT_DLL
