//=============================================================================//
//
// Purpose: C_Prop_Portal client class link + decode (client half).
//
//=============================================================================//
#ifndef C_PROP_PORTAL_H
#define C_PROP_PORTAL_H

class CSquirrelVM;
struct PortalPairState_t;

void Portal_LinkClientClass(void);
void Portal_ClientFrameTick(void);
void Portal_RegisterClientNatives(CSquirrelVM* s);
int PortalClient_GetTeleportPairs(PortalPairState_t* pOut, int nCap);
// The synthesized DT_Prop_Portal RecvTable, or nullptr before the link.
const void* Portal_ClientRecvTable(void);

// Read accessor for the portal surface renderer: snapshot of live portals.
struct PortalRenderInfo_t
{
	const void* pEntity;
	int entNum;
	float origin[3];
	float angles[3];
	unsigned int link;
	bool bActive;
	bool bIsPortal2;
	float halfW;
	float halfH;
};
int Portal_GetRenderList(PortalRenderInfo_t* pOut, int maxN);

#endif // C_PROP_PORTAL_H
