//=============================================================================//
//
// Purpose: connection-ping accessor for the performance HUD.
// S21 declares every RUI track natively; this layer adds none.
//
//=============================================================================//

#include "core/stdafx.h"
#include "ruitracks.h"
#include "engine/client/net_observer.h"

//-----------------------------------------------------------------------------
// Performance HUD SPING (ms). 0 offline / not FULL.
//-----------------------------------------------------------------------------
int RuiTracks_GetConnectionPingMs()
{
    // Engine NET_GetSPing -- same value as performance HUD SPING.
    return S21Bridge_GetConnectionPingMs();
}

//-----------------------------------------------------------------------------
// No-op: init still routes enum-registration here; S21 already declares every track.
//-----------------------------------------------------------------------------
void RuiTracks_RegisterMissingEnums(CSquirrelVM* const s)
{
    (void)s;
}
