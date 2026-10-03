//=============================================================================//
//
// Purpose: Portal pair transform + crossing test, compiled into both products
// so the server and prediction run identical arithmetic.
//
//=============================================================================//
#ifndef PORTAL_SHARED_H
#define PORTAL_SHARED_H
#ifdef _WIN32
#pragma once
#endif

#include "mathlib/vector.h"
#include "mathlib/vmatrix.h"

constexpr unsigned int BRIDGE_S2C_PORTALLED_MAGIC = 0x504F5254u;

struct PortalPairState_t
{
	Vector3D m_entryOrigin;
	QAngle m_entryAngles;
	float m_flEntryHalfW;
	float m_flEntryHalfH;
	Vector3D m_exitOrigin;
	QAngle m_exitAngles;
	void* m_pEntryEntity;
	void* m_pExitEntity;
	unsigned int m_nExitHandle;
	bool m_bValid;
};

void Portal_BuildPairMatrix(const Vector3D& entryOrigin, const QAngle& entryAngles,
	const Vector3D& exitOrigin, const QAngle& exitAngles, VMatrix* pOut);
Vector3D Portal_TransformPoint(const VMatrix& m, const Vector3D& pt);
Vector3D Portal_TransformVector(const VMatrix& m, const Vector3D& v);
QAngle Portal_TransformAngles(const VMatrix& m, const QAngle& ang);

bool Portal_SegmentCrossesRect(const Vector3D& prevCenter, const Vector3D& curCenter,
	const Vector3D& entryOrigin, const QAngle& entryAngles,
	float flHalfW, float flHalfH, float flHullMargin, float* pCrossT);

void Portal_ApplyExitTransform(const VMatrix& m,
	const Vector3D& inCenter, const Vector3D& inOrigin,
	const Vector3D& inVelocity, const QAngle& inAngles,
	Vector3D* pOutOrigin, Vector3D* pOutVelocity, QAngle* pOutAngles);

bool Portal_PointInVolume(const Vector3D& center, const PortalPairState_t& pair, float flExpand);

bool Portal_ShouldPitchReorient(const Vector3D& entryFwd, const Vector3D& entryUp,
	const Vector3D& exitFwd, const QAngle& eyeAngles);

// Teleport rule shared by the server and client prediction. It measures from
// the feet origin with a fixed standing hull so both sides decide the same
// entry on the same command.
Vector3D Portal_WishDir(float flYaw, float flForwardMove, float flSideMove);
bool Portal_EntersOnContact(const PortalPairState_t& pair, const Vector3D& prevOrigin,
	const Vector3D& origin, const Vector3D& wishDir);
void Portal_ComputeTeleport(const PortalPairState_t& pair, const Vector3D& origin,
	const Vector3D& velocity, VMatrix* pMatPair, Vector3D* pOutOrigin, Vector3D* pOutVelocity);
// Free-space probe each product runs with its own engine trace.
typedef bool (*PortalHullClearFn)(void* pCtx, const Vector3D& origin, const Vector3D& mins, const Vector3D& maxs);
// Where the player stands after the teleport: the computed exit, ducked when a
// floor or ceiling portal is involved, else lifted in small steps. False when
// nothing is free; both products then skip the teleport.
bool Portal_FindExitSpace(const PortalPairState_t& pair, const Vector3D& exitOrigin,
	PortalHullClearFn fnClear, void* pCtx, Vector3D* pOutOrigin, bool* pDucked);

// Exit view angles: the look direction through the pair, roll dropped
// (the view has no roll recovery).
QAngle Portal_TeleportViewAngles(const VMatrix& matPair, const QAngle& in);

#endif // PORTAL_SHARED_H
