//=============================================================================//
//
// Purpose: Server-side portal placement, structured like the reference
// VerifyPortalPlacement path: trace, surface gate, orient, fit/bump,
// hull clearance, overlap rejection, bounds check.
//
//=============================================================================//
#ifndef PORTAL_PLACEMENT_H
#define PORTAL_PLACEMENT_H

class Vector3D;
class QAngle;
class CPlayer;

struct PortalShot_t
{
	CPlayer* m_pPlayer;
	Vector3D m_eye;
	Vector3D m_dir;
};

struct PortalPlaceOut_t
{
	Vector3D m_pos;
	QAngle m_ang;
	bool m_bumped;
	float m_analog;
};

// Full placement from a shot the caller already validated against the
// owner. Returns ePortalPlaceResult_t as int.
int PortalPlacement_Place(const PortalShot_t& shot, int slot,
	const void* pIgnorePortal, PortalPlaceOut_t* out);

#endif // PORTAL_PLACEMENT_H
