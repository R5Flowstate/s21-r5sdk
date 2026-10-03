//=============================================================================//
//
// Purpose: Server-authoritative portal teleport (contact scaffolding: swept
// bbox-center segment vs portal rect; the center-behind-plane rule
// drops in when collision passes through).
//
//=============================================================================//
#ifndef PORTAL_TELEPORT_H
#define PORTAL_TELEPORT_H

class CUserCmd;

void PortalTeleport_PreRunCommand(void* pPlayer, CUserCmd* pCmd);
void PortalTeleport_PostRunCommand(void* pPlayer, CUserCmd* pCmd);
void PortalTeleport_LevelShutdown(void);

#endif // PORTAL_TELEPORT_H
