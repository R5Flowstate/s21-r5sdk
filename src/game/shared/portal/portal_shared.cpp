//=============================================================================//
//
// Purpose: Portal pair transform + crossing test, compiled into both products
// so the server and prediction run identical arithmetic.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/convar.h"
#include "mathlib/mathlib.h"
#include "mathlib/vmatrix.h"
#include "game/shared/portal/portal_shared.h"

ConVar portal_teleport_enable("portal_teleport_enable", "1", FCVAR_RELEASE,
	"Master switch for portal teleport (server apply + client prediction). 0 = portals are inert.");
ConVar portal_tele_debug("portal_tele_debug", "0", FCVAR_DEVELOPMENTONLY,
	"Log [PORTAL-TELE] before/after origin+velocity on every teleport, both products.");
ConVar portal_teleport_min_speed("portal_teleport_min_speed", "300", FCVAR_RELEASE,
	"Min upward exit speed for players leaving a floor portal. Must match on server and client.", true, 0.f, true, 2000.f);
ConVar portal_teleport_max_speed("portal_teleport_max_speed", "1000", FCVAR_RELEASE,
	"Max exit speed after a portal teleport. Must match on server and client.", true, 0.f, true, 10000.f);

static constexpr float kPortalRuleHalfXY = 16.0f;
static constexpr float kPortalRuleHeight = 80.0f;
static constexpr float kPortalContactSlop = 2.0f;
static constexpr float kPortalHullMargin = 4.0f;
static constexpr float kPortalFloorZ = 0.7071f;
// Past the contact band, so the exit portal is not re-entered next command.
static constexpr float kPortalExitClear = 4.0f;
static constexpr float kPortalExitUp = 2.0f;
// Pilot hull, fixed so both products test the same box.
static constexpr float kPortalHullHalfXY = 16.0f;
static constexpr float kPortalHullStand = 72.0f;
static constexpr float kPortalHullDuck = 47.0f;
static constexpr float kPortalExitLiftStep = 8.0f;
static constexpr float kPortalExitLiftMax = 48.0f;

void Portal_BuildPairMatrix(const Vector3D& entryOrigin, const QAngle& entryAngles,
	const Vector3D& exitOrigin, const QAngle& exitAngles, VMatrix* pOut)
{
	if (!pOut)
		return;

	VMatrix matEntry, matExit, matEntryInv, matRot;
	matEntry.SetupMatrixOrgAngles(entryOrigin, entryAngles);
	matExit.SetupMatrixOrgAngles(exitOrigin, exitAngles);
	MatrixInverseTR(matEntry, matEntryInv);

	matRot.Identity();
	matRot.m[0][0] = -1.0f;
	matRot.m[1][1] = -1.0f;

	VMatrix matTmp;
	MatrixMultiply(matExit, matRot, matTmp);
	MatrixMultiply(matTmp, matEntryInv, *pOut);
}

Vector3D Portal_TransformPoint(const VMatrix& m, const Vector3D& pt)
{
	return m * pt;
}

Vector3D Portal_TransformVector(const VMatrix& m, const Vector3D& v)
{
	return m.ApplyRotation(v);
}

QAngle Portal_TransformAngles(const VMatrix& m, const QAngle& ang)
{
	QAngle out = TransformAnglesToWorldSpace(ang, m.As3x4());
	out.x = AngleNormalizePositive(out.x);
	out.y = AngleNormalizePositive(out.y);
	out.z = AngleNormalizePositive(out.z);
	return out;
}

bool Portal_FindExitSpace(const PortalPairState_t& pair, const Vector3D& exitOrigin,
	PortalHullClearFn fnClear, void* pCtx, Vector3D* pOutOrigin, bool* pDucked)
{
	if (!fnClear || !pOutOrigin || !pDucked)
		return false;
	Vector3D entryFwd, exitFwd;
	AngleVectors(pair.m_entryAngles, &entryFwd);
	AngleVectors(pair.m_exitAngles, &exitFwd);
	const bool bAllowDuck = fabsf(entryFwd.z) > 0.01f || fabsf(exitFwd.z) > 0.01f;
	const Vector3D mins(-kPortalHullHalfXY, -kPortalHullHalfXY, 0.0f);
	const Vector3D standMaxs(kPortalHullHalfXY, kPortalHullHalfXY, kPortalHullStand);
	const Vector3D duckMaxs(kPortalHullHalfXY, kPortalHullHalfXY, kPortalHullDuck);
	for (float flLift = 0.0f; flLift <= kPortalExitLiftMax; flLift += kPortalExitLiftStep)
	{
		const Vector3D origin(exitOrigin.x, exitOrigin.y, exitOrigin.z + flLift);
		if (fnClear(pCtx, origin, mins, standMaxs))
		{
			*pOutOrigin = origin;
			*pDucked = false;
			return true;
		}
		if (bAllowDuck && fnClear(pCtx, origin, mins, duckMaxs))
		{
			*pOutOrigin = origin;
			*pDucked = true;
			return true;
		}
	}
	return false;
}

QAngle Portal_TeleportViewAngles(const VMatrix& matPair, const QAngle& in)
{
	Vector3D fwd;
	AngleVectors(Portal_TransformAngles(matPair, in), &fwd);
	QAngle out;
	VectorAngles(fwd, out);
	out.x = clamp(AngleNormalize(out.x), -89.0f, 89.0f);
	out.y = AngleNormalize(out.y);
	out.z = 0.0f;
	return out;
}

bool Portal_SegmentCrossesRect(const Vector3D& prevCenter, const Vector3D& curCenter,
	const Vector3D& entryOrigin, const QAngle& entryAngles,
	float flHalfW, float flHalfH, float flHullMargin, float* pCrossT)
{
	VMatrix matEntry, matEntryInv;
	matEntry.SetupMatrixOrgAngles(entryOrigin, entryAngles);
	MatrixInverseTR(matEntry, matEntryInv);

	Vector3D prevLocal = matEntryInv * prevCenter;
	Vector3D curLocal = matEntryInv * curCenter;

	if (prevLocal.x <= 0.0f || curLocal.x > 0.0f)
		return false;
	if (curLocal.x >= prevLocal.x)
		return false;

	const float flDenom = prevLocal.x - curLocal.x;
	if (flDenom <= 0.0f)
		return false;
	const float flT = prevLocal.x / flDenom;
	if (flT < 0.0f || flT > 1.0f)
		return false;

	const float flY = prevLocal.y + (curLocal.y - prevLocal.y) * flT;
	const float flZ = prevLocal.z + (curLocal.z - prevLocal.z) * flT;
	if (fabsf(flY) > flHalfW - flHullMargin)
		return false;
	if (fabsf(flZ) > flHalfH - flHullMargin)
		return false;

	if (pCrossT)
		*pCrossT = flT;
	return true;
}

void Portal_ApplyExitTransform(const VMatrix& m,
	const Vector3D& inCenter, const Vector3D& inOrigin,
	const Vector3D& inVelocity, const QAngle& inAngles,
	Vector3D* pOutOrigin, Vector3D* pOutVelocity, QAngle* pOutAngles)
{
	const Vector3D outCenter = m * inCenter;
	const Vector3D centerDelta = inOrigin - inCenter;
	if (pOutOrigin)
		*pOutOrigin = outCenter + centerDelta;
	if (pOutVelocity)
		*pOutVelocity = m.ApplyRotation(inVelocity);
	if (pOutAngles)
		*pOutAngles = Portal_TransformAngles(m, inAngles);
}

bool Portal_PointInVolume(const Vector3D& center, const PortalPairState_t& pair, float flExpand)
{
	if (!pair.m_bValid)
		return false;

	VMatrix matEntry, matEntryInv;
	matEntry.SetupMatrixOrgAngles(pair.m_entryOrigin, pair.m_entryAngles);
	MatrixInverseTR(matEntry, matEntryInv);

	const Vector3D local = matEntryInv * center;
	if (fabsf(local.x) > flExpand)
		return false;
	if (fabsf(local.y) > pair.m_flEntryHalfW + flExpand)
		return false;
	if (fabsf(local.z) > pair.m_flEntryHalfH + flExpand)
		return false;
	return true;
}

bool Portal_ShouldPitchReorient(const Vector3D& entryFwd, const Vector3D& entryUp,
	const Vector3D& exitFwd, const QAngle& eyeAngles)
{
	Vector3D eyeFwd;
	AngleVectors(eyeAngles, &eyeFwd);

	Vector3D flatFwd(eyeFwd.x, eyeFwd.y, 0.0f);
	const float flFlatLen = flatFwd.Length();
	if (flFlatLen > 0.0f)
		flatFwd /= flFlatLen;

	const float flFaceDot = entryFwd.Dot(flatFwd);
	const float flUpDot = entryUp.Dot(flatFwd);

	if (entryUp.z > 0.99f &&
		(flFlatLen == 0.0f || flFaceDot > 0.5f || flFaceDot < -0.5f))
		return true;

	if ((entryFwd.z > 0.99f || entryFwd.z < -0.99f) &&
		(exitFwd.z > 0.99f || exitFwd.z < -0.99f) &&
		(eyeFwd.z < -0.5f || eyeFwd.z > 0.5f))
		return true;

	if (exitFwd.z > 0.75f && exitFwd.z <= 0.99f && flUpDot > 0.0f)
		return true;

	return false;
}

static Vector3D Portal_RuleCenter(const Vector3D& origin)
{
	return Vector3D(origin.x, origin.y, origin.z + kPortalRuleHeight * 0.5f);
}

// Distance from the rule hull's centre to its face along n.
static float Portal_RuleSupport(const Vector3D& n)
{
	return (fabsf(n.x) + fabsf(n.y)) * kPortalRuleHalfXY + fabsf(n.z) * kPortalRuleHeight * 0.5f;
}

Vector3D Portal_WishDir(float flYaw, float flForwardMove, float flSideMove)
{
	Vector3D fwd, right;
	AngleVectors(QAngle(0.0f, flYaw, 0.0f), &fwd, &right, nullptr);
	return fwd * flForwardMove + right * flSideMove;
}

// The surface behind a portal stays solid, so the hull never crosses the
// portal plane. A portal is entered on contact: the hull touches the plane
// inside the opening while moving or pushing into it, or rests on a floor
// portal.
bool Portal_EntersOnContact(const PortalPairState_t& pair, const Vector3D& prevOrigin,
	const Vector3D& origin, const Vector3D& wishDir)
{
	if (!pair.m_bValid)
		return false;
	VMatrix matEntry, matEntryInv;
	matEntry.SetupMatrixOrgAngles(pair.m_entryOrigin, pair.m_entryAngles);
	MatrixInverseTR(matEntry, matEntryInv);
	const Vector3D cur = matEntryInv * Portal_RuleCenter(origin);
	const Vector3D prev = matEntryInv * Portal_RuleCenter(prevOrigin);

	Vector3D fwd;
	AngleVectors(pair.m_entryAngles, &fwd);
	const float flSupport = Portal_RuleSupport(fwd);
	if (cur.x < -kPortalContactSlop || cur.x > flSupport + kPortalContactSlop)
		return false;
	if (fabsf(cur.y) > pair.m_flEntryHalfW - kPortalHullMargin ||
		fabsf(cur.z) > pair.m_flEntryHalfH - kPortalHullMargin)
		return false;

	if (fwd.z > kPortalFloorZ)
		return true;
	if (prev.x > cur.x + 0.01f)
		return true;
	return wishDir.Dot(fwd) < 0.0f;
}

void Portal_ComputeTeleport(const PortalPairState_t& pair, const Vector3D& origin,
	const Vector3D& velocity, VMatrix* pMatPair, Vector3D* pOutOrigin, Vector3D* pOutVelocity)
{
	VMatrix matPair;
	Portal_BuildPairMatrix(pair.m_entryOrigin, pair.m_entryAngles,
		pair.m_exitOrigin, pair.m_exitAngles, &matPair);
	if (pMatPair)
		*pMatPair = matPair;

	const Vector3D center = Portal_RuleCenter(origin);
	Vector3D outCenter = matPair * center;

	// Entry happens in front of the plane, so the mirrored centre lands
	// behind the exit surface; move it out past the contact band.
	Vector3D exitFwd;
	AngleVectors(pair.m_exitAngles, &exitFwd);
	const float flExitDist = (outCenter - pair.m_exitOrigin).Dot(exitFwd);
	outCenter += exitFwd * (Portal_RuleSupport(exitFwd) + kPortalExitClear - flExitDist);
	Vector3D outOrigin = outCenter - (center - origin);
	if (exitFwd.z < kPortalFloorZ)
		outOrigin.z += kPortalExitUp;

	Vector3D outVel = matPair.ApplyRotation(velocity);
	const float flMinSpeed = portal_teleport_min_speed.GetFloat();
	const float flMaxSpeed = portal_teleport_max_speed.GetFloat();
	if (exitFwd.z > kPortalFloorZ && flMinSpeed > 0.0f && outVel.z < flMinSpeed)
		outVel.z = flMinSpeed;
	const float flSpeed = outVel.Length();
	if (flMaxSpeed > 0.0f && flSpeed > flMaxSpeed)
		outVel *= (flMaxSpeed / flSpeed);

	if (pOutOrigin)
		*pOutOrigin = outOrigin;
	if (pOutVelocity)
		*pOutVelocity = outVel;
}
