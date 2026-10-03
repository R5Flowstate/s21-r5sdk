//=============================================================================//
//
// Purpose: Server-side portal placement, structured like the reference
// VerifyPortalPlacement path: trace, surface gate, orient, fit/bump,
// hull clearance, overlap rejection, bounds check.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "mathlib/mathlib.h"
#include "public/bspflags.h"
#include "engine/enginetrace.h"
#include "game/shared/util_shared.h"
#include "game/server/player.h"
#include "game/server/portal/portal_placement.h"
#include "game/server/portal/prop_portal.h"
#include "game/shared/portal/portal_placement_shared.h"
#include <cstring>

static ConVar portal_half_width("portal_half_width", "36",
	FCVAR_RELEASE | FCVAR_REPLICATED,
	"Portal half width in Apex units, networked per portal at spawn.");
static ConVar portal_half_height("portal_half_height", "64",
	FCVAR_RELEASE | FCVAR_REPLICATED,
	"Portal half height in Apex units, networked per portal at spawn.");
static ConVar portal_placement_debug("portal_placement_debug", "0", FCVAR_DEVELOPMENTONLY,
	"Log portal placement rejects with reason (0=off, 1=on).");

static float PortalPlacement_HalfW(void)
{
	const float v = portal_half_width.GetFloat();
	return (v >= 8.0f && v <= 96.0f) ? v : kPortalHalfWidthDefault;
}

static float PortalPlacement_HalfH(void)
{
	const float v = portal_half_height.GetFloat();
	return (v >= 8.0f && v <= 160.0f) ? v : kPortalHalfHeightDefault;
}

static void PortalPlacement_Reject(const char* reason, const Vector3D& pos)
{
	if (portal_placement_debug.GetBool())
		Warning(eDLL_T::SERVER, "[PORTAL-PLACE] reject %s at (%.1f %.1f %.1f)\n",
			reason, pos.x, pos.y, pos.z);
}

static bool PortalPlacement_IsGlassName(const char* name)
{
	if (!name || !name[0])
		return false;
	for (const char* p = name; *p; ++p)
	{
		if ((p[0] == 'g' || p[0] == 'G') &&
			(p[1] == 'l' || p[1] == 'L') &&
			(p[2] == 'a' || p[2] == 'A') &&
			(p[3] == 's' || p[3] == 'S') &&
			(p[4] == 's' || p[4] == 'S'))
			return true;
	}
	return false;
}

// World or static prop only. A hit entity means a dynamic object: mover,
// door, trigger volume, or prop with its own collision, all rejected in v1.
static int PortalPlacement_SurfaceGate(const trace_t& tr, const Vector3D& pos)
{
	if (tr.surface.flags & (SURF_SKY | SURF_SKY2D))
	{
		PortalPlacement_Reject("sky", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;
	}
	if (tr.surface.flags & SURF_NODRAW)
	{
		PortalPlacement_Reject("nodraw", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;
	}
	if (tr.surface.flags & SURF_NOPORTAL)
	{
		PortalPlacement_Reject("noportal-surface", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;
	}
	if (tr.surface.flags & SURF_TRIGGER)
	{
		PortalPlacement_Reject("trigger-surface", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;
	}
	if (tr.contents & (CONTENTS_WATER | CONTENTS_SLIME))
	{
		PortalPlacement_Reject("water", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;
	}
	if (tr.contents & CONTENTS_WINDOW)
	{
		PortalPlacement_Reject("glass-contents", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;
	}
	if (PortalPlacement_IsGlassName(tr.surface.name))
	{
		PortalPlacement_Reject("glass-name", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;
	}
	// Traces against the world return the world entity (index 0), never null;
	// only a real entity (mover, prop_dynamic, player) is off limits.
	if (tr.hit_entity != nullptr
		&& *reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(tr.hit_entity) + 88) != 0)
	{
		PortalPlacement_Reject("hit-entity", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;
	}
	return (int)ePortalPlaceResult_t::PORTAL_PLACE_SUCCESS;
}

struct PortalOverlapCtx_t
{
	const void* m_ignore;
	Vector3D m_pos;
	Vector3D m_fwd;
	Vector3D m_right;
	Vector3D m_up;
	float m_halfW;
	float m_halfH;
	bool m_hit;
};

static bool PortalPlacement_OverlapVisit(const PortalPlacementInfo_t* info, void* ctx)
{
	PortalOverlapCtx_t* c = reinterpret_cast<PortalOverlapCtx_t*>(ctx);
	if (info->m_portalPtr == c->m_ignore)
		return true;
	Vector3D otherFwd(0, 0, 0);
	AngleVectors(info->m_ang, &otherFwd, nullptr, nullptr);
	if (c->m_fwd.Dot(otherFwd) < 0.95f)
		return true;
	Vector3D otherRight(0, 0, 0), otherUp(0, 0, 0);
	AngleVectors(info->m_ang, nullptr, &otherRight, &otherUp);
	Vector3D d = c->m_pos - info->m_pos;
	const float dR = fabsf(d.Dot(otherRight));
	const float dU = fabsf(d.Dot(otherUp));
	const float dF = fabsf(d.Dot(otherFwd));
	if (dR < c->m_halfW + info->m_halfW &&
		dU < c->m_halfH + info->m_halfH &&
		dF < 66.0f)
	{
		c->m_hit = true;
		return false;
	}
	return true;
}

static bool PortalPlacement_CheckBounds(const Vector3D& pos)
{
	static bool s_warned = false;
	if (!s_warned)
	{
		s_warned = true;
		Warning(eDLL_T::SERVER,
			"[PORTAL-PLACE] deathfield/out-of-bounds check not wired to a native query -- always passes\n");
	}
	(void)pos;
	return true;
}

int PortalPlacement_Place(const PortalShot_t& shot, int slot,
	const void* pIgnorePortal, PortalPlaceOut_t* out)
{
	if (!out || !shot.m_pPlayer || slot < 0 || slot > 1)
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;

	const float halfW = PortalPlacement_HalfW();
	const float halfH = PortalPlacement_HalfH();
	const float maxBump2 = ((halfW * 2.0f) * (halfW * 2.0f) +
		(halfH * 2.0f) * (halfH * 2.0f)) * 0.5f;

	if (!g_pEngineTraceServer)
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;

	Vector3D dir = shot.m_dir;
	dir.NormalizeInPlace();
	Vector3D end(shot.m_eye.x + dir.x * kPortalTraceLength,
		shot.m_eye.y + dir.y * kPortalTraceLength,
		shot.m_eye.z + dir.z * kPortalTraceLength);

	Ray_t ray;
	ray.Init(shot.m_eye, end, 0x3f800000, 0);
	trace_t tr;
	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1.0f;
	tr.endpos = end;
	CTraceFilterSimple filter(reinterpret_cast<const IHandleEntity*>(shot.m_pPlayer), 9);
	g_pEngineTraceServer->TraceRayFiltered(ray, TRACE_MASK_SHOT, &filter, &tr);

	if (portal_placement_debug.GetBool())
		Warning(eDLL_T::SERVER, "[PORTAL-PLACE] slot=%d eye=(%.1f %.1f %.1f) dir=(%.2f %.2f %.2f) frac=%.3f hit=(%.1f %.1f %.1f) n=(%.2f %.2f %.2f) surf='%s' flags=0x%X contents=0x%X\n",
			slot, shot.m_eye.x, shot.m_eye.y, shot.m_eye.z, dir.x, dir.y, dir.z, tr.fraction,
			tr.endpos.x, tr.endpos.y, tr.endpos.z, tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z,
			tr.surface.name ? tr.surface.name : "", (unsigned)tr.surface.flags, (unsigned)tr.contents);

	if (tr.fraction >= 1.0f || tr.startsolid)
	{
		PortalPlacement_Reject("passthrough", tr.endpos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_PASSTHROUGH_SURFACE;
	}

	int gate = PortalPlacement_SurfaceGate(tr, tr.endpos);
	if (gate != (int)ePortalPlaceResult_t::PORTAL_PLACE_SUCCESS)
		return gate;

	Vector3D n = tr.plane.normal;
	n.NormalizeInPlace();
	const Vector3D centerN = n;

	QAngle ang(0, 0, 0);
	if (fabsf(n.z) > 0.7f)
	{
		Vector3D yawDir(dir.x, dir.y, 0.0f);
		if (yawDir.LengthSqr() < 1e-6f)
			yawDir = Vector3D(1.0f, 0.0f, 0.0f);
		yawDir.NormalizeInPlace();
		VectorAngles(n, yawDir, ang);
	}
	else
	{
		VectorAngles(n, Vector3D(0.0f, 0.0f, 1.0f), ang);
	}

	Vector3D pos(tr.endpos.x + n.x * kPortalHalfDepth,
		tr.endpos.y + n.y * kPortalHalfDepth,
		tr.endpos.z + n.z * kPortalHalfDepth);
	const Vector3D startPos = pos;

	Vector3D fwd(0, 0, 0), right(0, 0, 0), up(0, 0, 0);
	AngleVectors(ang, &fwd, &right, &up);

	const float inW = halfW - kPortalBumpForgiveness;
	const float inH = halfH - kPortalBumpForgiveness;
	const float bumpStep = 4.0f;
	const int maxIter = static_cast<int>(sqrtf(maxBump2) / bumpStep) + 2;
	for (int iter = 0; iter < maxIter; ++iter)
	{
		Vector3D corners[4] = {
			pos + up * inH - right * inW,
			pos + up * inH + right * inW,
			pos - up * inH - right * inW,
			pos - up * inH + right * inW,
		};
		Vector3D push(0, 0, 0);
		bool anyMiss = false;
		bool bad = false;
		for (int c = 0; c < 4 && !bad; ++c)
		{
			Ray_t cray;
			Vector3D cs(corners[c].x + fwd.x * 2.0f,
				corners[c].y + fwd.y * 2.0f,
				corners[c].z + fwd.z * 2.0f);
			Vector3D ce(corners[c].x - fwd.x * 4.0f,
				corners[c].y - fwd.y * 4.0f,
				corners[c].z - fwd.z * 4.0f);
			cray.Init(cs, ce, 0x3f800000, 0);
			trace_t ctr;
			memset(&ctr, 0, sizeof(ctr));
			ctr.fraction = 1.0f;
			ctr.endpos = ce;
			g_pEngineTraceServer->TraceRayFiltered(cray, TRACE_MASK_SOLID_BRUSHONLY, &filter, &ctr);
			// A corner buried in adjacent geometry (floor under a wall portal) bumps
			// the same way as a corner hanging off the surface.
			if (ctr.startsolid || ctr.fraction >= 1.0f)
			{
				anyMiss = true;
				push = push + (pos - corners[c]);
				continue;
			}
			Vector3D cn = ctr.plane.normal;
			cn.NormalizeInPlace();
			if (cn.Dot(centerN) < 0.99f)
			{
				anyMiss = true;
				push = push + (pos - corners[c]);
				continue;
			}
			int cg = PortalPlacement_SurfaceGate(ctr, corners[c]);
			if (cg != (int)ePortalPlaceResult_t::PORTAL_PLACE_SUCCESS)
				return cg;
		}
		if (bad)
			return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_SURFACE;
		if (!anyMiss)
			break;
		// Opposite corners missing cancel out: the surface is smaller than the portal.
		if (push.LengthSqr() < 1.0f)
		{
			PortalPlacement_Reject("surface-too-small", pos);
			return (int)ePortalPlaceResult_t::PORTAL_PLACE_CANT_FIT;
		}
		push.NormalizeInPlace();
		pos = pos + push * bumpStep;
		Vector3D moved = pos - startPos;
		if (moved.LengthSqr() > maxBump2)
		{
			PortalPlacement_Reject("bump-too-far", pos);
			return (int)ePortalPlaceResult_t::PORTAL_PLACE_CANT_FIT;
		}
		if (iter == maxIter - 1)
		{
			PortalPlacement_Reject("bump-no-converge", pos);
			return (int)ePortalPlaceResult_t::PORTAL_PLACE_CANT_FIT;
		}
	}

	Vector3D moved = pos - startPos;
	const float bump2 = moved.LengthSqr();

	const float clearDepth = 40.0f;
	const float hullMargin = 18.0f;
	Vector3D probes[5] = {
		pos,
		pos + up * (inH - hullMargin) - right * (inW - hullMargin),
		pos + up * (inH - hullMargin) + right * (inW - hullMargin),
		pos - up * (inH - hullMargin) - right * (inW - hullMargin),
		pos - up * (inH - hullMargin) + right * (inW - hullMargin),
	};
	for (int i = 0; i < 5; ++i)
	{
		Ray_t hray;
		Vector3D he(probes[i].x + fwd.x * clearDepth,
			probes[i].y + fwd.y * clearDepth,
			probes[i].z + fwd.z * clearDepth);
		hray.Init(probes[i], he, 0x3f800000, 0);
		trace_t htr;
		memset(&htr, 0, sizeof(htr));
		htr.fraction = 1.0f;
		htr.endpos = he;
		g_pEngineTraceServer->TraceRayFiltered(hray, TRACE_MASK_PLAYERSOLID, &filter, &htr);
		if (htr.fraction < 1.0f && !htr.startsolid)
		{
			PortalPlacement_Reject("hull-blocked", probes[i]);
			return (int)ePortalPlaceResult_t::PORTAL_PLACE_CANT_FIT;
		}
	}

	PortalOverlapCtx_t octx;
	octx.m_ignore = pIgnorePortal;
	octx.m_pos = pos;
	octx.m_fwd = fwd;
	octx.m_right = right;
	octx.m_up = up;
	octx.m_halfW = halfW;
	octx.m_halfH = halfH;
	octx.m_hit = false;
	PortalEntity_VisitActive(PortalPlacement_OverlapVisit, &octx);
	if (octx.m_hit)
	{
		PortalPlacement_Reject("overlap", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_OVERLAP_LINKED;
	}

	if (!PortalPlacement_CheckBounds(pos))
	{
		PortalPlacement_Reject("invalid-volume", pos);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_INVALID_VOLUME;
	}

	out->m_pos = pos;
	out->m_ang = ang;
	out->m_bumped = (bump2 >= 1.0f);
	float mult = 1.0f - (bump2 / maxBump2);
	mult *= mult;
	mult *= mult;
	out->m_analog = mult * (1.0f - 0.3f) + 0.3f;
	return out->m_bumped ? (int)ePortalPlaceResult_t::PORTAL_PLACE_BUMPED : (int)ePortalPlaceResult_t::PORTAL_PLACE_SUCCESS;
}
