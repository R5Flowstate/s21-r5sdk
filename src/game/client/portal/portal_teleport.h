//=============================================================================//
//
// Purpose: Client prediction + EntityPortalled handler for portal teleports.
//
//=============================================================================//
#ifndef PORTAL_TELEPORT_CLIENT_H
#define PORTAL_TELEPORT_CLIENT_H

void PortalPredict_BeforeFullWalkMove(void* pMv);
void PortalPredict_AfterFullWalkMove(void* pPlayer, void* pMv, int nCmd);
void PortalClient_OnPortalled(int nEntWire, int nPortalWire,
	const float flOrigin[3], const float flAngles[3], int nTick);

#endif // PORTAL_TELEPORT_CLIENT_H
