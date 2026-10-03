//=============================================================================//
//
// Purpose: Server-authoritative portal teleport. Runs inside the existing
// PlayerRunCommand hook (no second detour): angle fixup before the move,
// crossing test + apply after the move.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "mathlib/mathlib.h"
#include "mathlib/vmatrix.h"
#include "game/shared/portal/portal_shared.h"
#include "game/server/portal/prop_portal.h"
#include "game/server/portal/portal_teleport.h"
#include "game/server/player.h"
#include "game/server/baseentity.h"
#include "game/shared/usercmd.h"
#include "game/server/translocation.h"
#include "game/server/trigger_cannon.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/server/vscript_server.h"
#include "engine/enginetrace.h"
#include "game/shared/util_shared.h"
#include "game/shared/collisionproperty.h"
#include "common/netmessages.h"
#include "engine/server/server.h"

extern CGlobalVars* gpGlobals;

static constexpr int kPortalPairCap = 16;
static constexpr float kPortalVolumeExpand = 16.0f;
static constexpr ptrdiff_t kPlayerFlagsOff = 0x234;
static constexpr int kFlDuckingBit = 2;
static constexpr int kInvalidCmd = -1;
// Commands after a teleport that may still carry the old view (about one
// round trip of input), padded.
static constexpr int kFixupWindowCmds = 64;

struct PortalTeleportRecord_t
{
	bool m_bHasPrev;
	Vector3D m_prevOrigin;
	bool m_bFixupArmed;
	bool m_bFixupSnapped;
	int m_nTeleportCmd;
	int m_nFixupCmds;
	VMatrix m_fixupMatrix;
	QAngle m_preAngles;
	QAngle m_postAngles;
	int32_t m_nEnvHandle;
};

static SDKEntityMap<PortalTeleportRecord_t> s_teleportRecords(ESide::Server, "portal.teleport");

void PortalTeleport_LevelShutdown(void)
{
	s_teleportRecords.Clear();
}

static PortalTeleportRecord_t* PortalTeleport_Record(void* pPlayer)
{
	if (!pPlayer)
		return nullptr;
	PortalTeleportRecord_t* pRec = s_teleportRecords.Find(pPlayer);
	if (!pRec)
	{
		PortalTeleportRecord_t rec = {};
		rec.m_nTeleportCmd = kInvalidCmd;
		rec.m_nEnvHandle = -1;
		s_teleportRecords[pPlayer] = rec;
		pRec = s_teleportRecords.Find(pPlayer);
	}
	return pRec;
}

static float PortalTeleport_AngleDist(const QAngle& a, const QAngle& b)
{
	return fabsf(AngleDistance(a.x, b.x)) + fabsf(AngleDistance(a.y, b.y)) + fabsf(AngleDistance(a.z, b.z));
}

static bool PortalTeleport_ReadCenter(void* pPlayer, Vector3D* pOrigin, Vector3D* pCenter)
{
	if (!pPlayer || !pOrigin || !pCenter)
		return false;
	const float* const pOrg = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(pPlayer) + 0x450);
	pOrigin->x = pOrg[0]; pOrigin->y = pOrg[1]; pOrigin->z = pOrg[2];
	if (!isfinite(pOrigin->x) || !isfinite(pOrigin->y) || !isfinite(pOrigin->z))
		return false;
	const int flags = *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(pPlayer) + kPlayerFlagsOff);
	float mins[3], maxs[3];
	Translocation_GetPlayerHull(pPlayer, (flags & kFlDuckingBit) != 0, mins, maxs);
	pCenter->x = pOrigin->x + (mins[0] + maxs[0]) * 0.5f;
	pCenter->y = pOrigin->y + (mins[1] + maxs[1]) * 0.5f;
	pCenter->z = pOrigin->z + (mins[2] + maxs[2]) * 0.5f;
	return true;
}

void PortalTeleport_PreRunCommand(void* pPlayer, CUserCmd* pCmd)
{
	if (!pPlayer || !pCmd)
		return;
	extern ConVar portal_teleport_enable;
	if (!portal_teleport_enable.GetBool())
		return;
	PortalTeleportRecord_t* pRec = PortalTeleport_Record(pPlayer);
	if (!pRec)
		return;

	// Client prediction measures the move from the origin the command starts at.
	Vector3D origin, center;
	pRec->m_bHasPrev = PortalTeleport_ReadCenter(pPlayer, &origin, &center);
	if (pRec->m_bHasPrev)
		pRec->m_prevOrigin = origin;

	if (!pRec->m_bFixupArmed)
		return;

	if (pCmd->command_number <= pRec->m_nTeleportCmd)
		return;
	if (++pRec->m_nFixupCmds > kFixupWindowCmds)
	{
		pRec->m_bFixupArmed = false;
		return;
	}

	// A predicting client turns its view on the teleport command, so its
	// next command already looks out of the exit portal.
	const float flToPre = PortalTeleport_AngleDist(pCmd->viewangles, pRec->m_preAngles);
	const float flToPost = PortalTeleport_AngleDist(pCmd->viewangles, pRec->m_postAngles);
	if (flToPost <= flToPre)
	{
		pRec->m_bFixupArmed = false;
		return;
	}

	pCmd->viewangles = Portal_TeleportViewAngles(pRec->m_fixupMatrix, pCmd->viewangles);

	// The client did not predict the teleport: turn its view once.
	const bool bSnap = !pRec->m_bFixupSnapped && CPlayer__SnapEyeAngles;
	if (bSnap)
	{
		pRec->m_bFixupSnapped = true;
		CPlayer__SnapEyeAngles(reinterpret_cast<CPlayer*>(pPlayer), &pCmd->viewangles);
	}

	extern ConVar portal_tele_debug;
	if (portal_tele_debug.GetBool())
	{
		Warning(eDLL_T::SERVER, "[PORTAL-TELE] fixup cmd=%d rotated stale angles (dPre=%.1f dPost=%.1f)%s\n",
			pCmd->command_number, flToPre, flToPost, bSnap ? " snap" : "");
	}
}

static void PortalTeleport_SendPortalled(void* pPlayer, unsigned int nExitS3Handle,
	const Vector3D& outOrigin, const QAngle& outAngles)
{
	if (!g_pServer || !pPlayer)
		return;
	const int32_t nEntWire = SDKEntityState_PackS21RecvEHandle(Portal_GetEntityHandle(pPlayer));
	const int32_t nPortalWire = SDKEntityState_PackS21RecvEHandle(static_cast<int32_t>(nExitS3Handle));
	const int nTick = (gpGlobals && gpGlobals->tickInterval > 0.0f)
		? static_cast<int>(gpGlobals->curTime / gpGlobals->tickInterval) : 0;

	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		CClient* const pClient = g_pServer->GetClient(i);
		if (!pClient || !pClient->IsActive() || pClient->IsFakeClient())
			continue;
		NET_ScriptMessage msg;
		msg.InitWrite();
		msg.m_bIsTyped = false;
		msg.m_DataOut.WriteLong(static_cast<int>(BRIDGE_S2C_PORTALLED_MAGIC));
		msg.m_DataOut.WriteLong(nEntWire);
		msg.m_DataOut.WriteLong(nPortalWire);
		msg.m_DataOut.WriteFloat(outOrigin.x);
		msg.m_DataOut.WriteFloat(outOrigin.y);
		msg.m_DataOut.WriteFloat(outOrigin.z);
		msg.m_DataOut.WriteFloat(outAngles.x);
		msg.m_DataOut.WriteFloat(outAngles.y);
		msg.m_DataOut.WriteFloat(outAngles.z);
		msg.m_DataOut.WriteLong(nTick);
		pClient->SendNetMsgEx(&msg, false, true, false);
	}
}

static bool PortalTeleport_HullClear(void* pPlayer, const Vector3D& exitOrigin,
	const Vector3D& minsV, const Vector3D& maxsV)
{
	if (!g_pEngineTraceServer || !v_Ray_t_InitStartEndMinsMaxsUp)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[PORTAL-TELE] hull trace infra missing -- exit validation fails closed\n");
		}
		return false;
	}
	alignas(16) Ray_t ray;
	memset(&ray, 0, sizeof(ray));
	const Vector3D start = exitOrigin;
	const Vector3D up(0.0f, 0.0f, 1.0f);
	v_Ray_t_InitStartEndMinsMaxsUp(&ray, &start, &start, &minsV, &maxsV, &up);
	ray.m_nSolidType = reinterpret_cast<CBaseEntity*>(pPlayer)->CollisionProp()->Diag_SolidType();
	ray.m_nDetailLevel = 0;
	trace_t tr;
	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1.0f;
	tr.endpos = start;
	CTraceFilterSimple filter(reinterpret_cast<const IHandleEntity*>(pPlayer), 9);
	g_pEngineTraceServer->TraceRayFiltered(ray, TRACE_MASK_PLAYERSOLID, &filter, &tr);
	return !tr.allsolid && !tr.startsolid;
}

static bool PortalTeleport_ExitClear(void* pCtx, const Vector3D& origin, const Vector3D& mins, const Vector3D& maxs)
{
	return PortalTeleport_HullClear(pCtx, origin, mins, maxs);
}

static void PortalTeleport_PublishEnv(void* pPlayer, PortalTeleportRecord_t* pRec, int32_t nS3Handle)
{
	if (pRec->m_nEnvHandle == nS3Handle)
		return;
	pRec->m_nEnvHandle = nS3Handle;
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_hPortalEnvironment), nS3Handle);
	MarkEntityEdictDirty(pPlayer);
}

void PortalTeleport_PostRunCommand(void* pPlayer, CUserCmd* pCmd)
{
	if (!pPlayer || !pCmd)
		return;
	extern ConVar portal_teleport_enable;
	if (!portal_teleport_enable.GetBool())
		return;

	Vector3D origin, center;
	if (!PortalTeleport_ReadCenter(pPlayer, &origin, &center))
		return;

	PortalTeleportRecord_t* pRec = PortalTeleport_Record(pPlayer);
	if (!pRec)
		return;

	PortalPairState_t pairs[kPortalPairCap];
	const int nPairs = Portal_GetTeleportPairs(pairs, kPortalPairCap);

	int32_t nEnvHandle = -1;
	for (int i = 0; i < nPairs; ++i)
	{
		if (!pairs[i].m_bValid)
			continue;
		if (Portal_PointInVolume(center, pairs[i], kPortalVolumeExpand))
		{
			nEnvHandle = static_cast<int32_t>(Portal_GetEntityHandle(pairs[i].m_pEntryEntity));
			break;
		}
		PortalPairState_t swapped = pairs[i];
		swapped.m_entryOrigin = pairs[i].m_exitOrigin;
		swapped.m_entryAngles = pairs[i].m_exitAngles;
		if (Portal_PointInVolume(center, swapped, kPortalVolumeExpand))
		{
			nEnvHandle = static_cast<int32_t>(pairs[i].m_nExitHandle);
			break;
		}
	}
	PortalTeleport_PublishEnv(pPlayer, pRec, nEnvHandle);

	if (!pRec->m_bHasPrev || nPairs <= 0 || !CPlayer__EyeAngles)
		return;

	const Vector3D wish = Portal_WishDir(pCmd->viewangles.y, pCmd->forwardmove, pCmd->sidemove);
	for (int i = 0; i < nPairs; ++i)
	{
		const PortalPairState_t& pair = pairs[i];
		if (!Portal_EntersOnContact(pair, pRec->m_prevOrigin, origin, wish))
			continue;

		const Vector3D inVel = reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_AbsVelocity();
		VMatrix matPair;
		Vector3D outOrigin, outVel;
		Portal_ComputeTeleport(pair, origin, inVel, &matPair, &outOrigin, &outVel);

		QAngle eyeAngles(0, 0, 0);
		CPlayer__EyeAngles(reinterpret_cast<CPlayer*>(pPlayer), &eyeAngles);
		const QAngle outAngles = Portal_TeleportViewAngles(matPair, eyeAngles);

		Vector3D exitFwd, entryFwd, entryUp, discard;
		AngleVectors(pair.m_exitAngles, &exitFwd);
		AngleVectors(pair.m_entryAngles, &entryFwd, &discard, &entryUp);

		bool bForceDuck = false;
		Vector3D freeOrigin;
		if (!Portal_FindExitSpace(pair, outOrigin, &PortalTeleport_ExitClear, pPlayer, &freeOrigin, &bForceDuck))
		{
			extern ConVar portal_tele_debug;
			if (portal_tele_debug.GetBool())
				Warning(eDLL_T::SERVER, "[PORTAL-TELE] cmd=%d exit blocked at (%.1f %.1f %.1f) -- no teleport\n",
					pCmd->command_number, outOrigin.x, outOrigin.y, outOrigin.z);
			continue;
		}
		outOrigin = freeOrigin;

		TriggerPass_SetGroundEntityNull(pPlayer);
		Translocation_AddNoInterpFlip(pPlayer);
		float flOutOrigin[3] = { outOrigin.x, outOrigin.y, outOrigin.z };
		float flOutVel[3] = { outVel.x, outVel.y, outVel.z };
		Translocation_SetAbsOrigin3(pPlayer, flOutOrigin);
		Translocation_SetAbsVelocity3(pPlayer, flOutVel);
		if (bForceDuck)
			Translocation_DuckImmediateNow(pPlayer);

		const bool bPitch = Portal_ShouldPitchReorient(entryFwd, entryUp, exitFwd, eyeAngles);
		PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_bPitchReorientation), bPitch ? 1 : 0);
		PortalTeleport_PublishEnv(pPlayer, pRec,
			static_cast<int32_t>(Portal_GetEntityHandle(pair.m_pExitEntity)));
		MarkEntityEdictDirty(pPlayer);

		pRec->m_bFixupArmed = true;
		pRec->m_bFixupSnapped = false;
		pRec->m_nTeleportCmd = pCmd->command_number;
		pRec->m_nFixupCmds = 0;
		pRec->m_fixupMatrix = matPair;
		pRec->m_preAngles = eyeAngles;
		pRec->m_postAngles = outAngles;

		PortalTeleport_SendPortalled(pPlayer, pair.m_nExitHandle, outOrigin, outAngles);

		extern ConVar portal_tele_debug;
		if (portal_tele_debug.GetBool())
		{
			Warning(eDLL_T::SERVER,
				"[PORTAL-TELE] cmd=%d o=(%.2f %.2f %.2f)->(%.2f %.2f %.2f) v=(%.1f %.1f %.1f)->(%.1f %.1f %.1f) a=(%.1f %.1f)->(%.1f %.1f)%s%s\n",
				pCmd->command_number,
				origin.x, origin.y, origin.z, outOrigin.x, outOrigin.y, outOrigin.z,
				inVel.x, inVel.y, inVel.z, outVel.x, outVel.y, outVel.z,
				eyeAngles.x, eyeAngles.y, outAngles.x, outAngles.y,
				bForceDuck ? " duck" : "", CPlayer__SnapEyeAngles ? "" : " NO-SNAP");
		}
		return;
	}
}
