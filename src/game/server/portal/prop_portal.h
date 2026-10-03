//=============================================================================//
//
// Purpose: CProp_Portal networked entity (server half).
//
//=============================================================================//
#ifndef PROP_PORTAL_H
#define PROP_PORTAL_H

class CSquirrelVM;
class Vector3D;
class QAngle;
struct PortalPairState_t;

void Portal_RegisterServerClass(void);
int Portal_RequiredAllocSize(void);
void Portal_LevelShutdown(void);
// Steps the opening scale of newly placed portals; once per server frame.
void PortalEntity_Frame(void);
void Portal_RegisterServerNatives(CSquirrelVM* s);
bool Portal_IsPortalEntity(const void* pEntity);
// True for the send proxies that serve the portal fields from the registry.
bool Portal_IsTailProxy(const void* fn);
int Portal_GetTeleportPairs(PortalPairState_t* pOut, int nCap);
unsigned int Portal_GetEntityHandle(const void* pEntity);
void* Portal_ResolveServerEntity(unsigned int nS3Handle);

struct PortalPlacementInfo_t
{
	const void* m_portalPtr;
	unsigned int m_ownerHandle;
	int m_slot;
	Vector3D m_pos;
	QAngle m_ang;
	float m_halfW;
	float m_halfH;
};

// Reuse the owner's pair slot when live, else create. Null when the
// server-wide cap is reached. outReused reports which path was taken.
void* PortalEntity_Acquire(unsigned int ownerHandle, int slot, bool* outReused);
// PortalEntity_NewLocation-style entry: move, activate, relink, parity flip.
void PortalEntity_NewLocation(void* pEntity, const Vector3D& pos, const QAngle& ang);
bool PortalEntity_GetInfo(const void* pEntity, PortalPlacementInfo_t* out);
typedef bool (*PortalVisitFn)(const PortalPlacementInfo_t* info, void* ctx);
int PortalEntity_VisitActive(PortalVisitFn fn, void* ctx);
int PortalEntity_CloseOwnerPair(unsigned int ownerHandle);
int PortalEntity_ActiveCount(void);
// Destroy/shutdown routing: closes the pair when pEntity is a portal, its
// owner, or a weapon holding the pair. Returns portals closed.
int PortalEntity_OnEntityDestroyed(const void* pEntity);

#endif // PROP_PORTAL_H
