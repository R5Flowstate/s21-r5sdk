//=============================================================================//
//
// Purpose: Client prediction teleport (same test as the server, applied to
// the post-FullWalkMove move data) + EntityPortalled S2C handler for remote
// players. Runs inside the existing FullWalkMove hook (no second detour).
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "mathlib/mathlib.h"
#include "mathlib/vmatrix.h"
#include "game/shared/portal/portal_shared.h"
#include "game/client/portal/c_prop_portal.h"
#include "game/client/portal/portal_teleport.h"
#include "game/client/cliententitylist.h"
#include "engine/client/net_bridge_internal.h"
#include "engine/client/demo_bridge.h"
#include "engine/enginetrace.h"
#include "public/bspflags.h"

extern ConVar portal_teleport_enable;
extern ConVar portal_tele_debug;

static ConVar portal_teleport_predict("portal_teleport_predict", "1", FCVAR_RELEASE,
	"Predict portal teleports on the client with the server's rule (0 = server only).");

static constexpr ptrdiff_t kMvOffForwardMove = 48;
static constexpr ptrdiff_t kMvOffSideMove = 52;
static constexpr ptrdiff_t kMvOffOrigin = 280;
static constexpr ptrdiff_t kMvOffVelocity = 292;
static constexpr ptrdiff_t kPlayerOffGroundEnt = 804;
static constexpr ptrdiff_t kPlayerOffCurrentCommand = 0x34B8;
static constexpr ptrdiff_t kCmdOffViewAngles = 0x0C;
static constexpr int kPortalPairCap = 16;

static Vector3D s_preMoveOrigin(0, 0, 0);
static const void* s_pPreMoveMv = nullptr;
static int s_nLastTurnCmd = -1;
// Re-prediction never reaches this far back; a larger drop is a new connection.
static constexpr int kTurnCmdResetGap = 512;

static bool PortalPredict_PtrReadable(const void* p, size_t n)
{
	if (!p || n == 0 || n > 0x10000)
		return false;
	__try
	{
		volatile uint8_t v = 0;
		const volatile uint8_t* b = reinterpret_cast<const volatile uint8_t*>(p);
		v |= b[0];
		v |= b[n - 1];
		(void)v;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	return true;
}

// Hits everything but the predicting player, like the server's exit test.
class CPortalExitFilter : public CTraceFilter
{
public:
	explicit CPortalExitFilter(const void* pSkip) : m_pSkip(pSkip) {}
	virtual bool ShouldHitEntity(IHandleEntity* const pEntity, const int) { return pEntity != m_pSkip; }
	virtual bool ShouldBlockTrace(trace_t* const) { return false; }
private:
	const void* m_pSkip;
};

static bool PortalPredict_ExitClear(void* pCtx, const Vector3D& origin, const Vector3D& mins, const Vector3D& maxs)
{
	CEngineTraceClient* const pTrace = EngineTrace_GetClient();
	if (!pTrace)
		return false;
	alignas(16) Ray_t ray(origin, origin);
	ray.m_IsRay = false;
	ray.m_Extents.x = (maxs.x - mins.x) * 0.5f;
	ray.m_Extents.y = (maxs.y - mins.y) * 0.5f;
	ray.m_Extents.z = (maxs.z - mins.z) * 0.5f;
	ray.m_Extents.w = ray.m_Extents.z - ray.m_Extents.x;
	const Vector3D offset((mins.x + maxs.x) * 0.5f, (mins.y + maxs.y) * 0.5f, (mins.z + maxs.z) * 0.5f);
	ray.m_Start.x = origin.x + offset.x;
	ray.m_Start.y = origin.y + offset.y;
	ray.m_Start.z = origin.z + offset.z;
	ray.m_StartOffset.x = -offset.x;
	ray.m_StartOffset.y = -offset.y;
	ray.m_StartOffset.z = -offset.z;
	trace_t tr;
	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1.0f;
	CPortalExitFilter filter(pCtx);
	pTrace->TraceRayFiltered(ray, TRACE_MASK_PLAYERSOLID, &filter, &tr);
	return !tr.allsolid && !tr.startsolid;
}

void PortalPredict_BeforeFullWalkMove(void* pMv)
{
	s_pPreMoveMv = nullptr;
	if (!pMv)
		return;
	const float* const pOrg = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(pMv) + kMvOffOrigin);
	s_preMoveOrigin = Vector3D(pOrg[0], pOrg[1], pOrg[2]);
	s_pPreMoveMv = pMv;
}

// Same command, same rule as the server's post-command test, so the acked
// origin matches the predicted one and no correction is needed.
void PortalPredict_AfterFullWalkMove(void* pPlayer, void* pMv, int nCmd)
{
	if (!pPlayer || !pMv || nCmd < 0 || pMv != s_pPreMoveMv)
		return;
	s_pPreMoveMv = nullptr;
	if (!portal_teleport_enable.GetBool() || !portal_teleport_predict.GetBool())
		return;

	uint8_t* const mv = reinterpret_cast<uint8_t*>(pMv);
	uint8_t* const player = reinterpret_cast<uint8_t*>(pPlayer);
	const uint8_t* const pCmd = *reinterpret_cast<uint8_t* const*>(player + kPlayerOffCurrentCommand);
	if (!pCmd)
		return;

	PortalPairState_t pairs[kPortalPairCap];
	const int nPairs = PortalClient_GetTeleportPairs(pairs, kPortalPairCap);
	if (nPairs <= 0)
		return;

	float* const pOrg = reinterpret_cast<float*>(mv + kMvOffOrigin);
	float* const pVel = reinterpret_cast<float*>(mv + kMvOffVelocity);
	const Vector3D origin(pOrg[0], pOrg[1], pOrg[2]);
	const Vector3D inVel(pVel[0], pVel[1], pVel[2]);
	const float flYaw = reinterpret_cast<const float*>(pCmd + kCmdOffViewAngles)[1];
	const Vector3D wish = Portal_WishDir(flYaw,
		*reinterpret_cast<const float*>(mv + kMvOffForwardMove),
		*reinterpret_cast<const float*>(mv + kMvOffSideMove));

	for (int i = 0; i < nPairs; ++i)
	{
		if (!Portal_EntersOnContact(pairs[i], s_preMoveOrigin, origin, wish))
			continue;

		VMatrix matPair;
		Vector3D outOrigin, outVel;
		Portal_ComputeTeleport(pairs[i], origin, inVel, &matPair, &outOrigin, &outVel);
		Vector3D freeOrigin;
		bool bDucked = false;
		if (!Portal_FindExitSpace(pairs[i], outOrigin, &PortalPredict_ExitClear, pPlayer, &freeOrigin, &bDucked))
			continue;
		outOrigin = freeOrigin;
		pOrg[0] = outOrigin.x; pOrg[1] = outOrigin.y; pOrg[2] = outOrigin.z;
		pVel[0] = outVel.x; pVel[1] = outVel.y; pVel[2] = outVel.z;
		*reinterpret_cast<int*>(player + kPlayerOffGroundEnt) = -1;

		// Turn the view on the first prediction only; the next command then
		// carries exit-side angles, same frame as the move.
		const bool bTurn = nCmd > s_nLastTurnCmd || s_nLastTurnCmd - nCmd > kTurnCmdResetGap;
		QAngle view(0, 0, 0);
		if (bTurn && DemoPlay_GetEngineViewAngles(&view.x))
		{
			s_nLastTurnCmd = nCmd;
			const QAngle outView = Portal_TeleportViewAngles(matPair, view);
			DemoPlay_SetEngineViewAngles(&outView.x);
			if (portal_tele_debug.GetBool())
			{
				Msg(eDLL_T::CLIENT, "[PORTAL-TELE] predict cmd=%d view (%.1f %.1f)->(%.1f %.1f)\n",
					nCmd, view.x, view.y, outView.x, outView.y);
			}
		}

		if (portal_tele_debug.GetBool())
		{
			Msg(eDLL_T::CLIENT,
				"[PORTAL-TELE] predict cmd=%d o=(%.2f %.2f %.2f)->(%.2f %.2f %.2f) v=(%.1f %.1f %.1f)->(%.1f %.1f %.1f)\n",
				nCmd, origin.x, origin.y, origin.z, outOrigin.x, outOrigin.y, outOrigin.z,
				inVel.x, inVel.y, inVel.z, outVel.x, outVel.y, outVel.z);
		}
		return;
	}
}

static int s_nParityOff = -2;
static int s_nEffectsOff = -2;
static int s_nOriginOff = -2;
static bool s_bOffsetsWarned = false;

static int PortalClient_FindRecvOffset(const char* pszClass, const char* pszProp)
{
	uintptr_t* pHead = (uintptr_t*)NetObs_NonRewindClientClassHeadAddr();
	if (!pHead)
		return -1;
	for (int guard = 0; *pHead && guard < 4096; ++guard)
	{
		uintptr_t cc = *pHead;
		if (!PortalPredict_PtrReadable(reinterpret_cast<const void*>(cc), 48))
			break;
		const char* nm = *(const char**)(cc + 0x10);
		if (nm && PortalPredict_PtrReadable(nm, 16) &&
			(strcmp(nm, pszClass) == 0))
		{
			uintptr_t rt = *(uintptr_t*)(cc + 0x18);
			if (!PortalPredict_PtrReadable(reinterpret_cast<const void*>(rt), 0x500))
				return -1;
			uint8_t** props = *(uint8_t***)(rt + 0x008);
			const int n = *(int*)(rt + 0x010);
			if (!props || n <= 0 || n > 4096)
				return -1;
			for (int j = 0; j < n; ++j)
			{
				uint8_t* p = nullptr;
				__try { p = props[j]; }
				__except (EXCEPTION_EXECUTE_HANDLER) { p = nullptr; }
				if (!p || !PortalPredict_PtrReadable(p, 0x68))
					continue;
				const char* pn = *(const char**)(p + 0x28);
				if (!pn || !PortalPredict_PtrReadable(pn, 32))
					continue;
				if (strcmp(pn, pszProp) == 0)
					return *(int*)(p + 0x04);
			}
			return -1;
		}
		pHead = (uintptr_t*)(cc + 0x20);
		if (!PortalPredict_PtrReadable(pHead, sizeof(uintptr_t)))
			break;
	}
	return -1;
}

static void PortalClient_ResolveOffsets(void)
{
	if (s_nParityOff != -2)
		return;
	s_nParityOff = PortalClient_FindRecvOffset("C_Player", "m_ubEFNoInterpParity");
	if (s_nParityOff < 0)
		s_nParityOff = PortalClient_FindRecvOffset("CPlayer", "m_ubEFNoInterpParity");
	s_nEffectsOff = PortalClient_FindRecvOffset("C_Player", "m_fEffects");
	if (s_nEffectsOff < 0)
		s_nEffectsOff = PortalClient_FindRecvOffset("CPlayer", "m_fEffects");
	s_nOriginOff = PortalClient_FindRecvOffset("C_Player", "m_vecAbsOrigin");
	if (s_nOriginOff < 0)
		s_nOriginOff = PortalClient_FindRecvOffset("CPlayer", "m_vecAbsOrigin");
	if ((s_nParityOff < 0 || s_nOriginOff < 0) && !s_bOffsetsWarned)
	{
		s_bOffsetsWarned = true;
		Warning(eDLL_T::CLIENT,
			"[PORTAL-TELE] S2C offsets unresolved (parity=%d effects=%d origin=%d) -- remote snap off\n",
			s_nParityOff, s_nEffectsOff, s_nOriginOff);
	}
}

void PortalClient_OnPortalled(int nEntWire, int nPortalWire,
	const float flOrigin[3], const float flAngles[3], int nTick)
{
	(void)nPortalWire;
	(void)flAngles;
	(void)nTick;
	if (!flOrigin)
		return;
	if (!portal_teleport_enable.GetBool())
		return;

	const int nEntNum = nEntWire & 0x3FFF;
	if (nEntNum == 1)
		return;

	void* const pEntity = ClientEntityList_EntityAt(nEntNum, -1);
	if (!pEntity)
		return;

	PortalClient_ResolveOffsets();
	if (s_nParityOff < 0 || s_nOriginOff < 0)
		return;

	__try
	{
		uint8_t* base = reinterpret_cast<uint8_t*>(pEntity);
		if (!PortalPredict_PtrReadable(base + s_nOriginOff, sizeof(Vector3D)))
			return;
		*reinterpret_cast<Vector3D*>(base + s_nOriginOff) = Vector3D(flOrigin[0], flOrigin[1], flOrigin[2]);
		if (s_nEffectsOff >= 0 && PortalPredict_PtrReadable(base + s_nEffectsOff, sizeof(int)))
			*reinterpret_cast<int*>(base + s_nEffectsOff) |= 8;
		if (PortalPredict_PtrReadable(base + s_nParityOff, 1))
			*reinterpret_cast<uint8_t*>(base + s_nParityOff) ^= 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }

	if (portal_tele_debug.GetBool())
	{
		Msg(eDLL_T::CLIENT, "[PORTAL-TELE] S2C snap ent=%d o=(%.1f %.1f %.1f) tick=%d\n",
			nEntNum, flOrigin[0], flOrigin[1], flOrigin[2], nTick);
	}
}
