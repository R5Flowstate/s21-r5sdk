//=============================================================================//
//
// Purpose: Server-authoritative skyward launch state, movement, and natives.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "skyward.h"
#include "skydive.h"
#include "translocation.h"
#include "trigger_cannon.h"
#include "baseentity.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/weapon_script_vars.h"
#include "public/edict.h"
#include "mathlib/vector.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript_server_natives.h"
#include "engine/host_state.h"
#include <cmath>
#include <cfloat>
#include <cstring>
#include <intrin.h>

extern CGlobalVars* gpGlobals;

enum PlayerSkywardLaunchState_t
{
	PLAYER_SKYWARD_LAUNCH_STATE_NONE       = 0,
	PLAYER_SKYWARD_LAUNCH_STATE_DEPLOY     = 1,
	PLAYER_SKYWARD_LAUNCH_STATE_HOVER      = 2,
	PLAYER_SKYWARD_LAUNCH_STATE_LAUNCH     = 3,
	PLAYER_SKYWARD_LAUNCH_STATE_TRANSITION = 4
};

static constexpr ptrdiff_t SW_OFF_FFLAGS    = 564;    // m_fFlags
static constexpr ptrdiff_t SW_OFF_MOVETYPE  = 0x308;  // m_MoveType
static constexpr ptrdiff_t SW_OFF_DUCKSTATE = 26096;  // m_duckState
static constexpr ptrdiff_t SW_OFF_SLIDING   = 26565;  // m_sliding
static constexpr int SW_FL_DUCKING = 0x2;
static constexpr int SW_MOVETYPE_WALK = 2;
static constexpr int SW_MOVETYPE_FLY  = 4;

static constexpr ptrdiff_t SW_CTX_OFF_PLAYER   = 8;
static constexpr ptrdiff_t SW_CTX_OFF_MOVEDATA = 16;
static constexpr ptrdiff_t SW_MV_OFF_ORIGIN    = 292;
static constexpr ptrdiff_t SW_MV_OFF_VELOCITY  = 304;

static constexpr float SW_TRANSITION_TIME        = 1.2f;
static constexpr float SW_FOLLOWER_OFFSET_SPEED  = 100.0f;
static constexpr float SW_DISATTACH_MIN          = 800.0f;
static constexpr float SW_TIME_MAX               = 60.0f;
static constexpr float SW_SPEED_MAX              = 20000.0f;
static constexpr float SW_OFFSET_MAX             = 4096.0f;
// The use-prompt that joins a launch is a few hundred units; anything farther is not a join.
static constexpr float SW_JOIN_MAX_DIST          = 2048.0f;
static constexpr int   SW_FOLLOWER_CAP           = 64;

struct SkywardState
{
	int   m_state = PLAYER_SKYWARD_LAUNCH_STATE_NONE;
	int   m_following = 0;
	int   m_interrupted = 0;
	float m_deploySpeed = 0.0f;
	float m_deployEndTime = 0.0f;
	float m_deployStartPos[3] = {};
	float m_slowStartTime = 0.0f;
	float m_slowEndTime = 0.0f;
	float m_slowSpeed = 0.0f;
	float m_fastEndTime = 0.0f;
	float m_fastSpeed = 0.0f;
	float m_launchEndTime = 0.0f;
	float m_offset[3] = {};
	float m_offsetSpeed = 0.0f;
	float m_obstacleAvoidanceEndPos[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	SDKEntityHandle m_hLeader;
};

static SDKEntityMap<SkywardState> s_skywardMap(ESide::Server, "skyward.srv");

static ConVar bridge_skyward(
	"bridge_skyward", "1", FCVAR_RELEASE,
	"Master gate for server skyward-launch natives and movement.");

static ConVar bridge_skyward_diag(
	"bridge_skyward_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Per-tick [SKYWARD-MOVE] every N ticks (0 = off) and [SKYWARD] state lines.");

static ConVar bridge_skyward_obstacle_avoidance(
	"bridge_skyward_obstacle_avoidance", "0", FCVAR_RELEASE,
	"Leader obstacle-avoidance steer. Unimplemented in this pass; leave 0.");

static bool s_bInCallback = false;
static uint32_t s_nMoveTick = 0;

using SetParentFn_t = void(__fastcall*)(void* pEnt, void* pParent, int attachment);

static inline void Entity_SetParent(void* pEnt, void* pParent)
{
	if (!pEnt)
		return;
	SetParentFn_t* const pVtbl = *reinterpret_cast<SetParentFn_t**>(pEnt);
	if (!pVtbl)
		return;
	pVtbl[31](pEnt, pParent, -1);
}

static inline float Skyward_CurTime(void)
{
	return gpGlobals ? gpGlobals->curTime : 0.0f;
}

static inline float Skyward_Clamp(const float flValue, const float flMin, const float flMax)
{
	return fminf(fmaxf(flValue, flMin), flMax);
}

static inline float Skyward_Len(const float x, const float y, const float z)
{
	return sqrtf(x * x + y * y + z * z);
}

static void Skyward_RotateYaw(float out[3], const float in[3], const float yawDeg)
{
	const float rad = yawDeg * (3.14159265358979323846f / 180.0f);
	const float c = cosf(rad);
	const float s = sinf(rad);
	out[0] = in[0] * c - in[1] * s;
	out[1] = in[0] * s + in[1] * c;
	out[2] = in[2];
}

static void Skyward_VecInvalid(float xyz[3])
{
	xyz[0] = FLT_MAX;
	xyz[1] = FLT_MAX;
	xyz[2] = FLT_MAX;
}

static void Skyward_VecZero(float xyz[3])
{
	xyz[0] = 0.0f;
	xyz[1] = 0.0f;
	xyz[2] = 0.0f;
}

static bool Skyward_GatedOff(bool& bLogged, const char* pszNative)
{
	if (bridge_skyward.GetBool())
		return false;
	if (!bLogged)
	{
		bLogged = true;
		Warning(eDLL_T::SERVER,
			"[SKYWARD] %s ignored -- bridge_skyward 0\n", pszNative);
	}
	return true;
}

static void Skyward_LogFirst(bool& bLogged, const char* pszNative, void* pPlayer)
{
	if (bLogged)
		return;
	bLogged = true;
	Msg(eDLL_T::SERVER, "[SKYWARD] %s first call player=%p\n", pszNative, pPlayer);
}

static bool Skyward_FiniteOrWarn(const char* pszNative, const float fl)
{
	if (std::isfinite(fl))
		return true;
	static bool s_bWarned = false;
	if (!s_bWarned)
	{
		s_bWarned = true;
		Warning(eDLL_T::SERVER,
			"[SKYWARD] %s rejected non-finite float\n", pszNative);
	}
	return false;
}

// S21 WPT masks the launch holds disabled: everything but viewhands, ultimate and
// incap shield; a follower also loses its ultimate.
static constexpr uint32_t SW_WPT_DISABLED_S21          = 0x3AE;
static constexpr uint32_t SW_WPT_DISABLED_FOLLOWER_S21 = 0x3BE;

static uint32_t Skyward_DisabledWeaponTypes(const bool bFollowing)
{
	return WeaponScriptVars_WeaponTypesFromS21(bFollowing ? SW_WPT_DISABLED_FOLLOWER_S21 : SW_WPT_DISABLED_S21);
}

static void Skyward_UnduckAndFly(void* pPlayer)
{
	uint8_t* const pBytes = reinterpret_cast<uint8_t*>(pPlayer);
	*reinterpret_cast<int*>(pBytes + SW_OFF_FFLAGS) &= ~SW_FL_DUCKING;
	*reinterpret_cast<int*>(pBytes + SW_OFF_DUCKSTATE) = 0;
	*(pBytes + SW_OFF_SLIDING) = 0;
	Translocation_SetMoveType(pPlayer, SW_MOVETYPE_FLY);
	TriggerPass_SetGroundEntityNull(pPlayer);
}

static void SkywardBridge_Mirror(void* pPlayer, const SkywardState& s)
{
	if (!pPlayer)
		return;

	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_skywardLaunchState), s.m_state);
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_skywardLaunchFollowing), s.m_following);
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_skywardLaunchInterrupted), s.m_interrupted);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_skywardLaunchSlowStartTime), s.m_slowStartTime);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_skywardLaunchSlowEndTime), s.m_slowEndTime);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_skywardLaunchSlowSpeed), s.m_slowSpeed);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_skywardLaunchFastEndTime), s.m_fastEndTime);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_skywardLaunchFastSpeed), s.m_fastSpeed);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_skywardLaunchEndTime), s.m_launchEndTime);
	PlayerExtend_SetVec(pPlayer, offsetof(PlayerExtendWire, m_skywardOffset), s.m_offset);
	PlayerExtend_SetVec(pPlayer, offsetof(PlayerExtendWire, m_skywardObstacleAvoidanceEndPos),
		s.m_obstacleAvoidanceEndPos);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_skywardDeployEndTime), s.m_deployEndTime);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_skywardDeploySpeed), s.m_deploySpeed);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_skywardOffsetSpeed), s.m_offsetSpeed);
	PlayerExtend_SetVec(pPlayer, offsetof(PlayerExtendWire, m_skywardDeployStartPos), s.m_deployStartPos);
	MarkEntityEdictDirty(pPlayer);
}

static void Skyward_CancelHostShutdown(const char* pszName,
	const HostStates_t iStateBefore, const HostStates_t iNextBefore)
{
	if (!g_pHostState)
		return;
	if (iStateBefore == HostStates_t::HS_GAME_SHUTDOWN
		|| iNextBefore == HostStates_t::HS_GAME_SHUTDOWN)
		return;
	if (g_pHostState->m_iCurrentState != HostStates_t::HS_GAME_SHUTDOWN
		&& g_pHostState->m_iNextState != HostStates_t::HS_GAME_SHUTDOWN)
		return;

	g_pHostState->m_iCurrentState = iStateBefore;
	g_pHostState->m_iNextState = iNextBefore;
	Warning(eDLL_T::SERVER,
		"[SKYWARD] cancelled host shutdown scheduled by '%s'\n",
		pszName ? pszName : "?");
}

static void Skyward_FireCallback(const char* pszName, void* pPlayer, const bool* pInterrupt)
{
	if (!g_pServerScript || !pPlayer)
		return;

	if (s_bInCallback)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYWARD] %s re-entered -- refused\n", pszName);
		}
		return;
	}

	const HSCRIPT hFunc = g_pServerScript->FindFunction(pszName, nullptr, nullptr);
	if (!hFunc)
	{
		static bool s_bWarnedDeploy = false;
		static bool s_bWarnedBegin = false;
		static bool s_bWarnedEnd = false;
		bool* pFlag = &s_bWarnedEnd;
		if (pszName && strstr(pszName, "Deploy"))
			pFlag = &s_bWarnedDeploy;
		else if (pszName && strstr(pszName, "LaunchBegin"))
			pFlag = &s_bWarnedBegin;
		if (!*pFlag)
		{
			*pFlag = true;
			Warning(eDLL_T::SERVER,
				"[SKYWARD] %s not found in server VM -- skipped\n", pszName);
		}
		return;
	}

	CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
	const HSCRIPT hPlayer = pEnt->GetScriptInstance();
	if (!hPlayer)
		return;

	s_bInCallback = true;

	const HostStates_t iStateBefore = g_pHostState
		? g_pHostState->m_iCurrentState : HostStates_t::HS_RUN;
	const HostStates_t iNextBefore = g_pHostState
		? g_pHostState->m_iNextState : HostStates_t::HS_RUN;

	ScriptVariant_t args[2];
	args[0] = hPlayer;
	unsigned int nArgs = 1;
	if (pInterrupt)
	{
		args[1] = *pInterrupt;
		nArgs = 2;
	}

	const ScriptStatus_t status = g_pServerScript->ExecuteFunction(
		hFunc, args, nArgs, nullptr, nullptr);
	if (status == SCRIPT_ERROR)
	{
		Warning(eDLL_T::SERVER, "[SKYWARD] %s SCRIPT_ERROR\n", pszName);
		Skyward_CancelHostShutdown(pszName, iStateBefore, iNextBefore);
	}

	s_bInCallback = false;
}

static void Skyward_EndFollowersOf(void* pLeader, const bool bInterrupt);

static void Skyward_End(void* pPlayer, const bool bInterrupt, const bool bWarnIfIdle)
{
	if (!pPlayer)
		return;

	SkywardState* const pState = s_skywardMap.Find(pPlayer);
	if (!pState || pState->m_state == PLAYER_SKYWARD_LAUNCH_STATE_NONE)
	{
		if (bWarnIfIdle)
		{
			static bool s_bWarned = false;
			if (!s_bWarned)
			{
				s_bWarned = true;
				Warning(eDLL_T::SERVER,
					"[SKYWARD] Player_EndSkywardLaunch -- not skyward launching\n");
			}
		}
		return;
	}

	const bool bWasFollowing = pState->m_following != 0;
	if (bWasFollowing)
		Entity_SetParent(pPlayer, nullptr);

	// The dive starts from rest: a seeded climb velocity carries the player up past the apex.
	if (!bInterrupt)
		SkydiveBridge_BeginFreefall(pPlayer, Vector3D(0.0f, 0.0f, 0.0f));

	WeaponScriptVars_EnableWeaponTypes(pPlayer, Skyward_DisabledWeaponTypes(bWasFollowing));

	if (bInterrupt)
		Translocation_SetMoveType(pPlayer, SW_MOVETYPE_WALK);

	const int nFall = SkydiveBridge_GetFreefallState(pPlayer);
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_skydiveFromSkywardLaunch),
		(nFall != 0) ? 1 : 0);

	pState->m_state = PLAYER_SKYWARD_LAUNCH_STATE_NONE;
	pState->m_launchEndTime = Skyward_CurTime();
	pState->m_interrupted = bInterrupt ? 1 : 0;
	pState->m_following = 0;
	pState->m_hLeader = SDKEntityHandle();
	Skyward_VecInvalid(pState->m_obstacleAvoidanceEndPos);
	Skyward_VecZero(pState->m_offset);
	SkywardBridge_Mirror(pPlayer, *pState);

	if (bridge_skyward_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[SKYWARD] end interrupted=%d player=%p\n",
			bInterrupt ? 1 : 0, pPlayer);

	// A leader that leaves the ride stops toss-moving, so its followers can no
	// longer be ticked from its movement step -- release them here.
	if (!bWasFollowing)
		Skyward_EndFollowersOf(pPlayer, bInterrupt);

	const bool bInt = bInterrupt;
	Skyward_FireCallback("CodeCallback_PlayerSkywardLaunchEnd", pPlayer, &bInt);
}

static void Skyward_EndFollowersOf(void* pLeader, const bool bInterrupt)
{
	if (!pLeader)
		return;

	SDKEntityHandle followers[SW_FOLLOWER_CAP];
	int nFollowers = 0;
	for (auto it = s_skywardMap.begin();
		it != s_skywardMap.end() && nFollowers < SW_FOLLOWER_CAP; ++it)
	{
		const SkywardState& s = it->second;
		if (!s.m_following || s.m_state == PLAYER_SKYWARD_LAUNCH_STATE_NONE)
			continue;
		if (SDKEntityState_Resolve(s.m_hLeader, ESide::Server) == pLeader)
			followers[nFollowers++] = it->first;
	}

	for (int i = 0; i < nFollowers; ++i)
	{
		void* const pAlly = SDKEntityState_Resolve(followers[i], ESide::Server);
		if (pAlly)
			Skyward_End(pAlly, bInterrupt, false);
	}
}

static void Skyward_DeployBody(void* pPlayer, SkywardState& s, const float flUpSpeed,
	const float flUpTime, const bool bFollowing)
{
	CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
	const Vector3D& origin = pEnt->Diag_AbsOrigin();

	if (s.m_state == PLAYER_SKYWARD_LAUNCH_STATE_NONE)
		WeaponScriptVars_DisableWeaponTypes(pPlayer, Skyward_DisabledWeaponTypes(bFollowing));

	s.m_state = PLAYER_SKYWARD_LAUNCH_STATE_DEPLOY;
	s.m_following = bFollowing ? 1 : 0;
	s.m_interrupted = 0;
	s.m_deploySpeed = bFollowing ? 0.0f : flUpSpeed;
	s.m_deployEndTime = Skyward_CurTime() + (bFollowing ? 0.0f : flUpTime);
	s.m_deployStartPos[0] = origin.x;
	s.m_deployStartPos[1] = origin.y;
	s.m_deployStartPos[2] = origin.z;
	s.m_slowEndTime = 0.0f;
	s.m_fastEndTime = 0.0f;
	s.m_slowSpeed = 0.0f;
	s.m_fastSpeed = 0.0f;
	s.m_slowStartTime = 0.0f;
	Skyward_VecInvalid(s.m_obstacleAvoidanceEndPos);
	Skyward_UnduckAndFly(pPlayer);
	SkywardBridge_Mirror(pPlayer, s);
	Skyward_FireCallback("CodeCallback_PlayerSkywardDeployBegin", pPlayer, nullptr);
}

static void Skyward_TickFollowers(void* pLeader)
{
	if (!pLeader)
		return;

	SDKEntityHandle followers[SW_FOLLOWER_CAP];
	int nFollowers = 0;
	for (auto it = s_skywardMap.begin();
		it != s_skywardMap.end() && nFollowers < SW_FOLLOWER_CAP; ++it)
	{
		const SkywardState& s = it->second;
		if (!s.m_following || s.m_state == PLAYER_SKYWARD_LAUNCH_STATE_NONE)
			continue;
		void* const pResolved = SDKEntityState_Resolve(s.m_hLeader, ESide::Server);
		if (pResolved == pLeader)
			followers[nFollowers++] = it->first;
	}

	CBaseEntity* const pLeadEnt = reinterpret_cast<CBaseEntity*>(pLeader);
	const Vector3D& leadOrigin = pLeadEnt->Diag_AbsOrigin();
	const float yaw = pLeadEnt->Diag_AbsRotation().y;
	const float dt = TriggerPass_FrameTime();
	SkywardState* const pLeadState = s_skywardMap.Find(pLeader);

	for (int i = 0; i < nFollowers; ++i)
	{
		void* const pAlly = SDKEntityState_Resolve(followers[i], ESide::Server);
		if (!pAlly)
			continue;
		SkywardState* const pAllySt = s_skywardMap.Find(pAlly);
		if (!pAllySt || !pAllySt->m_following)
			continue;

		CBaseEntity* const pAllyEnt = reinterpret_cast<CBaseEntity*>(pAlly);
		const Vector3D& allyOrigin = pAllyEnt->Diag_AbsOrigin();
		const float dx = allyOrigin.x - leadOrigin.x;
		const float dy = allyOrigin.y - leadOrigin.y;
		const float dz = allyOrigin.z - leadOrigin.z;
		if (Skyward_Len(dx, dy, dz) >= SW_DISATTACH_MIN)
		{
			Skyward_End(pAlly, true, false);
			continue;
		}

		if (!pLeadState || pLeadState->m_state == PLAYER_SKYWARD_LAUNCH_STATE_NONE)
		{
			const bool bInt = pLeadState && pLeadState->m_interrupted != 0;
			Skyward_End(pAlly, bInt, false);
			continue;
		}

		const float flSpeed = (pLeadState->m_state == PLAYER_SKYWARD_LAUNCH_STATE_LAUNCH)
			? SW_FOLLOWER_OFFSET_SPEED : pAllySt->m_offsetSpeed;

		float rotated[3];
		Skyward_RotateYaw(rotated, pAllySt->m_offset, yaw);
		const float target[3] = {
			leadOrigin.x + rotated[0],
			leadOrigin.y + rotated[1],
			leadOrigin.z + rotated[2]
		};
		const float ddx = target[0] - allyOrigin.x;
		const float ddy = target[1] - allyOrigin.y;
		const float ddz = target[2] - allyOrigin.z;
		const float dist = Skyward_Len(ddx, ddy, ddz);
		const float step = flSpeed * dt;
		float dest[3];
		if (dist <= step || dist <= 0.001f)
		{
			dest[0] = target[0];
			dest[1] = target[1];
			dest[2] = target[2];
			if (pAllySt->m_state == PLAYER_SKYWARD_LAUNCH_STATE_DEPLOY)
			{
				pAllySt->m_state = PLAYER_SKYWARD_LAUNCH_STATE_HOVER;
				SkywardBridge_Mirror(pAlly, *pAllySt);
			}
		}
		else
		{
			const float scale = step / dist;
			dest[0] = allyOrigin.x + ddx * scale;
			dest[1] = allyOrigin.y + ddy * scale;
			dest[2] = allyOrigin.z + ddz * scale;
		}
		ServerNatives_SetAbsOrigin(pAllyEnt, dest);
	}
}

bool SkywardBridge_WireEnabled(void)
{
	return bridge_skyward.GetBool();
}

void SkywardBridge_LevelShutdown(void)
{
	s_skywardMap.Clear();
}

bool SkywardBridge_IsActive(const void* pPlayer)
{
	if (!pPlayer)
		return false;
	const SkywardState* const pState = s_skywardMap.Find(pPlayer);
	return pState && pState->m_state != PLAYER_SKYWARD_LAUNCH_STATE_NONE;
}

// Movement only steps a rider that is still toss-moving; a death or another
// system taking the movetype away would otherwise leave the ride latched.
void SkywardBridge_PreRunCommand(void* pPlayer)
{
	if (!pPlayer)
		return;

	SkywardState* const pState = s_skywardMap.Find(pPlayer);
	if (!pState || pState->m_state == PLAYER_SKYWARD_LAUNCH_STATE_NONE)
		return;

	const uint8_t* const pBytes = static_cast<const uint8_t*>(pPlayer);
	const bool bDead = reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_LifeState() != 0;
	if (bDead || pBytes[SW_OFF_MOVETYPE] != SW_MOVETYPE_FLY)
	{
		if (bridge_skyward_diag.GetInt() > 0)
			Msg(eDLL_T::SERVER, "[SKYWARD] ride interrupted (%s) player=%p\n",
				bDead ? "dead" : "movetype", pPlayer);
		Skyward_End(pPlayer, true, false);
	}
}

void SkywardBridge_PreTossMove(int64_t movement)
{
	if (!movement || !bridge_skyward.GetBool())
		return;

	void* const pPlayer = *reinterpret_cast<void**>(movement + SW_CTX_OFF_PLAYER);
	void* const pMoveData = *reinterpret_cast<void**>(movement + SW_CTX_OFF_MOVEDATA);
	if (!pPlayer || !pMoveData)
		return;

	SkywardState* const pState = s_skywardMap.Find(pPlayer);
	if (!pState || pState->m_state == PLAYER_SKYWARD_LAUNCH_STATE_NONE)
		return;

	float* const pVel = reinterpret_cast<float*>(
		static_cast<uint8_t*>(pMoveData) + SW_MV_OFF_VELOCITY);

	// A follower is placed by its leader's tick; its own step must not drift it.
	if (pState->m_following)
	{
		pVel[0] = 0.0f;
		pVel[1] = 0.0f;
		pVel[2] = 0.0f;
		return;
	}

	const float* const pOrigin = reinterpret_cast<const float*>(
		static_cast<const uint8_t*>(pMoveData) + SW_MV_OFF_ORIGIN);
	const float flVzIn = pVel[2];

	if (bridge_skyward_obstacle_avoidance.GetBool())
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYWARD] obstacle avoidance is unimplemented -- steer stays 0\n");
		}
	}

	float vx = 0.0f;
	float vy = 0.0f;
	float vz = 0.0f;
	switch (pState->m_state)
	{
	case PLAYER_SKYWARD_LAUNCH_STATE_DEPLOY:
		vz = pState->m_deploySpeed;
		break;
	case PLAYER_SKYWARD_LAUNCH_STATE_HOVER:
		break;
	case PLAYER_SKYWARD_LAUNCH_STATE_LAUNCH:
		{
			const float flNow = Skyward_CurTime();
			if (flNow <= pState->m_slowEndTime)
				vz = pState->m_slowSpeed;
			else if (flNow <= pState->m_fastEndTime)
				vz = pState->m_fastSpeed;
			else
			{
				const float t = Skyward_Clamp(
					(flNow - pState->m_fastEndTime) / SW_TRANSITION_TIME, 0.0f, 1.0f);
				vz = pState->m_fastSpeed + (pState->m_fastSpeed * 0.5f - pState->m_fastSpeed) * t;
			}
		}
		break;
	case PLAYER_SKYWARD_LAUNCH_STATE_TRANSITION:
		{
			const float flNow = Skyward_CurTime();
			const float t = Skyward_Clamp(
				(flNow - pState->m_fastEndTime) / SW_TRANSITION_TIME, 0.0f, 1.0f);
			vz = pState->m_fastSpeed + (pState->m_fastSpeed * 0.5f - pState->m_fastSpeed) * t;
		}
		break;
	default:
		break;
	}

	if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz))
		return;

	pVel[0] = vx;
	pVel[1] = vy;
	pVel[2] = vz;
	TriggerPass_SetGroundEntityNull(pPlayer);

	const int nEvery = bridge_skyward_diag.GetInt();
	if (nEvery > 0)
	{
		++s_nMoveTick;
		if ((s_nMoveTick % static_cast<uint32_t>(nEvery)) == 0)
		{
			const float dt = TriggerPass_FrameTime();
			Msg(eDLL_T::SERVER,
				"[SKYWARD-MOVE] state=%d dt=%.4f vzIn=%.1f vzOut=%.1f z=%.1f dz=%.2f follow=%d\n",
				pState->m_state, dt, flVzIn, pVel[2], pOrigin[2], pVel[2] * dt,
				pState->m_following);
		}
	}
}

void SkywardBridge_PostTossMove(int64_t movement)
{
	if (!movement || !bridge_skyward.GetBool())
		return;

	void* const pPlayer = *reinterpret_cast<void**>(movement + SW_CTX_OFF_PLAYER);
	if (!pPlayer)
		return;

	SkywardState* const pState = s_skywardMap.Find(pPlayer);
	if (pState && pState->m_state != PLAYER_SKYWARD_LAUNCH_STATE_NONE)
	{
		CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
		if (pEnt->Diag_LifeState() != 0)
			Skyward_End(pPlayer, true, false);
		else if (!pState->m_following)
		{
			const float flNow = Skyward_CurTime();
			bool bChanged = false;
			if (pState->m_state == PLAYER_SKYWARD_LAUNCH_STATE_DEPLOY
				&& flNow >= pState->m_deployEndTime)
			{
				pState->m_state = PLAYER_SKYWARD_LAUNCH_STATE_HOVER;
				bChanged = true;
			}
			else if (pState->m_state == PLAYER_SKYWARD_LAUNCH_STATE_LAUNCH
				&& flNow >= pState->m_fastEndTime)
			{
				pState->m_state = PLAYER_SKYWARD_LAUNCH_STATE_TRANSITION;
				bChanged = true;
			}
			else if (pState->m_state == PLAYER_SKYWARD_LAUNCH_STATE_TRANSITION
				&& flNow >= pState->m_fastEndTime + SW_TRANSITION_TIME)
			{
				Skyward_End(pPlayer, false, false);
			}

			if (bChanged && s_skywardMap.Find(pPlayer)
				&& s_skywardMap.Find(pPlayer)->m_state != PLAYER_SKYWARD_LAUNCH_STATE_NONE)
			{
				SkywardBridge_Mirror(pPlayer, *pState);
				if (bridge_skyward_diag.GetInt() > 0)
					Msg(eDLL_T::SERVER, "[SKYWARD] state=%d player=%p\n",
						pState->m_state, pPlayer);
			}
		}
	}

	Skyward_TickFollowers(pPlayer);
}

static void* Skyward_This(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return nullptr;
	return pPlayer;
}

static SQRESULT Script_Player_DeploySkywardLaunch(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "Player_DeploySkywardLaunch", pPlayer);
	if (Skyward_GatedOff(s_bGate, "Player_DeploySkywardLaunch"))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SQFloat flSpeed = 0.0;
	SQFloat flTime = 0.0;
	sq_getfloat(v, 2, &flSpeed);
	sq_getfloat(v, 3, &flTime);
	const float upSpeed = static_cast<float>(flSpeed);
	const float upTime = static_cast<float>(flTime);
	if (!Skyward_FiniteOrWarn("Player_DeploySkywardLaunch", upSpeed)
		|| !Skyward_FiniteOrWarn("Player_DeploySkywardLaunch", upTime))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SkywardState& s = s_skywardMap[pPlayer];
	Skyward_DeployBody(pPlayer, s,
		Skyward_Clamp(upSpeed, -SW_SPEED_MAX, SW_SPEED_MAX),
		Skyward_Clamp(upTime, 0.0f, SW_TIME_MAX),
		false);
	if (bridge_skyward_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[SKYWARD] deploy player=%p speed=%.1f time=%.2f\n",
			pPlayer, s.m_deploySpeed, upTime);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_BeginSkywardLaunch(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "Player_BeginSkywardLaunch", pPlayer);
	if (Skyward_GatedOff(s_bGate, "Player_BeginSkywardLaunch"))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SQFloat a = 0.0, b = 0.0, c = 0.0, d = 0.0;
	sq_getfloat(v, 2, &a);
	sq_getfloat(v, 3, &b);
	sq_getfloat(v, 4, &c);
	sq_getfloat(v, 5, &d);
	const float slowSpeed = static_cast<float>(a);
	const float slowTime = static_cast<float>(b);
	const float fastSpeed = static_cast<float>(c);
	const float fastTime = static_cast<float>(d);
	if (!Skyward_FiniteOrWarn("Player_BeginSkywardLaunch", slowSpeed)
		|| !Skyward_FiniteOrWarn("Player_BeginSkywardLaunch", slowTime)
		|| !Skyward_FiniteOrWarn("Player_BeginSkywardLaunch", fastSpeed)
		|| !Skyward_FiniteOrWarn("Player_BeginSkywardLaunch", fastTime))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SkywardState* const pState = s_skywardMap.Find(pPlayer);
	if (!pState || pState->m_state == PLAYER_SKYWARD_LAUNCH_STATE_NONE)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYWARD] Player_BeginSkywardLaunch refused -- Deploy has not run\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const float flNow = Skyward_CurTime();
	pState->m_state = PLAYER_SKYWARD_LAUNCH_STATE_LAUNCH;
	pState->m_slowStartTime = flNow;
	pState->m_slowSpeed = Skyward_Clamp(slowSpeed, -SW_SPEED_MAX, SW_SPEED_MAX);
	pState->m_fastSpeed = Skyward_Clamp(fastSpeed, -SW_SPEED_MAX, SW_SPEED_MAX);
	pState->m_slowEndTime = flNow + Skyward_Clamp(slowTime, 0.0f, SW_TIME_MAX);
	pState->m_fastEndTime = pState->m_slowEndTime + Skyward_Clamp(fastTime, 0.0f, SW_TIME_MAX);
	Skyward_VecInvalid(pState->m_obstacleAvoidanceEndPos);
	SkywardBridge_Mirror(pPlayer, *pState);
	if (bridge_skyward_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[SKYWARD] begin player=%p slow=%.1f fast=%.1f\n",
			pPlayer, pState->m_slowSpeed, pState->m_fastSpeed);
	Skyward_FireCallback("CodeCallback_PlayerSkywardLaunchBegin", pPlayer, nullptr);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_EndSkywardLaunch(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "Player_EndSkywardLaunch", pPlayer);
	if (Skyward_GatedOff(s_bGate, "Player_EndSkywardLaunch"))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	Skyward_End(pPlayer, true, true);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_JoinSkywardLaunch(HSQUIRRELVM v)
{
	void* const pAlly = Skyward_This(v);
	if (!pAlly)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "Player_JoinSkywardLaunch", pAlly);
	if (Skyward_GatedOff(s_bGate, "Player_JoinSkywardLaunch"))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	void* const pLeader = ServerScript_EntityPtrFromStackIdx(v, 2);
	if (!pLeader || !ServerScript_EntityIsPlayer(pLeader) || pLeader == pAlly)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYWARD] Player_JoinSkywardLaunch rejected leader player=%p leader=%p\n",
				pAlly, pLeader);
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	CBaseEntity* const pLeadEnt = reinterpret_cast<CBaseEntity*>(pLeader);
	if (pLeadEnt->Diag_LifeState() != 0)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYWARD] Player_JoinSkywardLaunch rejected dead leader\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const Vector3D& leadOrigin = pLeadEnt->Diag_AbsOrigin();
	const Vector3D& allyOrigin = reinterpret_cast<CBaseEntity*>(pAlly)->Diag_AbsOrigin();
	const float flJoinDist = Skyward_Len(leadOrigin.x - allyOrigin.x,
		leadOrigin.y - allyOrigin.y, leadOrigin.z - allyOrigin.z);
	if (pLeadEnt->Diag_TeamNum() != reinterpret_cast<CBaseEntity*>(pAlly)->Diag_TeamNum()
		|| !(flJoinDist <= SW_JOIN_MAX_DIST))
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYWARD] Player_JoinSkywardLaunch rejected -- leader on another team or %.0f units away\n",
				flJoinDist);
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	SkywardState* const pLeadSt = s_skywardMap.Find(pLeader);
	if (!pLeadSt || pLeadSt->m_state == PLAYER_SKYWARD_LAUNCH_STATE_NONE)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYWARD] Player_JoinSkywardLaunch rejected -- leader is not launching\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	SkywardState* const pAllySt = s_skywardMap.Find(pAlly);
	if (pAllySt && pAllySt->m_state != PLAYER_SKYWARD_LAUNCH_STATE_NONE)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER,
				"[SKYWARD] Player_JoinSkywardLaunch rejected -- already launching\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const SQVector3D* pOff = nullptr;
	sq_getvector(v, 3, &pOff);
	if (!pOff)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SQFloat flSpeed = 0.0;
	sq_getfloat(v, 4, &flSpeed);
	const float offSpeed = static_cast<float>(flSpeed);
	if (!Skyward_FiniteOrWarn("Player_JoinSkywardLaunch", pOff->x)
		|| !Skyward_FiniteOrWarn("Player_JoinSkywardLaunch", pOff->y)
		|| !Skyward_FiniteOrWarn("Player_JoinSkywardLaunch", pOff->z)
		|| !Skyward_FiniteOrWarn("Player_JoinSkywardLaunch", offSpeed))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	Entity_SetParent(pAlly, pLeader);

	SkywardState& s = s_skywardMap[pAlly];
	s.m_hLeader = SDKEntityState_GetHandle(pLeader);
	s.m_offset[0] = Skyward_Clamp(pOff->x, -SW_OFFSET_MAX, SW_OFFSET_MAX);
	s.m_offset[1] = Skyward_Clamp(pOff->y, -SW_OFFSET_MAX, SW_OFFSET_MAX);
	s.m_offset[2] = Skyward_Clamp(pOff->z, -SW_OFFSET_MAX, SW_OFFSET_MAX);
	s.m_offsetSpeed = Skyward_Clamp(offSpeed, -SW_SPEED_MAX, SW_SPEED_MAX);
	Skyward_DeployBody(pAlly, s, 0.0f, 0.0f, true);
	if (bridge_skyward_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[SKYWARD] join ally=%p leader=%p\n", pAlly, pLeader);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_StopFollowSkywardLaunch(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "Player_StopFollowSkywardLaunch", pPlayer);
	if (Skyward_GatedOff(s_bGate, "Player_StopFollowSkywardLaunch"))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SQBool bInterrupt = SQTrue;
	sq_getbool(v, 2, &bInterrupt);
	(void)bInterrupt;

	Entity_SetParent(pPlayer, nullptr);
	Skyward_End(pPlayer, true, false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_IsSkywardLaunching(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "Player_IsSkywardLaunching", pPlayer);
	Skyward_GatedOff(s_bGate, "Player_IsSkywardLaunching");

	sq_pushbool(v, SkywardBridge_IsActive(pPlayer) ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_IsSkywardFollowing(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "Player_IsSkywardFollowing", pPlayer);
	Skyward_GatedOff(s_bGate, "Player_IsSkywardFollowing");

	const SkywardState* const pState = s_skywardMap.Find(pPlayer);
	const bool bFollow = pState
		&& pState->m_state != PLAYER_SKYWARD_LAUNCH_STATE_NONE
		&& pState->m_following;
	sq_pushbool(v, bFollow ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_IsSkywardDiving(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "Player_IsSkywardDiving", pPlayer);
	Skyward_GatedOff(s_bGate, "Player_IsSkywardDiving");

	const int nFrom = PlayerExtend_GetI32(pPlayer,
		offsetof(PlayerExtendWire, m_skydiveFromSkywardLaunch));
	const bool bDive = nFrom != 0 && SkydiveBridge_IsFreefalling(pPlayer);
	sq_pushbool(v, bDive ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetSkywardLaunchState(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "GetSkywardLaunchState", pPlayer);
	Skyward_GatedOff(s_bGate, "GetSkywardLaunchState");

	const SkywardState* const pState = s_skywardMap.Find(pPlayer);
	sq_pushinteger(v, pState ? pState->m_state : PLAYER_SKYWARD_LAUNCH_STATE_NONE);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SkyDive_SetIsFromSkywardLaunch(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "SkyDive_SetIsFromSkywardLaunch", pPlayer);
	if (Skyward_GatedOff(s_bGate, "SkyDive_SetIsFromSkywardLaunch"))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SQBool bVal = SQFalse;
	sq_getbool(v, 2, &bVal);
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_skydiveFromSkywardLaunch),
		bVal ? 1 : 0);
	MarkEntityEdictDirty(pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Skydive_IsFromSkywardLaunch(HSQUIRRELVM v)
{
	void* const pPlayer = Skyward_This(v);
	if (!pPlayer)
		return SQ_ERROR;

	static bool s_bGate = false;
	static bool s_bFirst = false;
	Skyward_LogFirst(s_bFirst, "Skydive_IsFromSkywardLaunch", pPlayer);
	Skyward_GatedOff(s_bGate, "Skydive_IsFromSkywardLaunch");

	const int nFrom = PlayerExtend_GetI32(pPlayer,
		offsetof(PlayerExtendWire, m_skydiveFromSkywardLaunch));
	sq_pushbool(v, nFrom != 0 ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void SkywardBridge_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"Player_DeploySkywardLaunch",
		"Script_Player_DeploySkywardLaunch",
		"Begin skyward deploy with upward speed and duration",
		"void",
		"float upSpeed, float upTime",
		false,
		Script_Player_DeploySkywardLaunch);

	playerStruct->AddFunction(
		"Player_BeginSkywardLaunch",
		"Script_Player_BeginSkywardLaunch",
		"Begin the skyward climb envelope",
		"void",
		"float slowSpeed, float slowTime, float fastSpeed, float fastTime",
		false,
		Script_Player_BeginSkywardLaunch);

	playerStruct->AddFunction(
		"Player_EndSkywardLaunch",
		"Script_Player_EndSkywardLaunch",
		"End skyward launch (interrupt)",
		"void",
		"",
		false,
		Script_Player_EndSkywardLaunch);

	playerStruct->AddFunction(
		"Player_JoinSkywardLaunch",
		"Script_Player_JoinSkywardLaunch",
		"Follow a leader's skyward launch",
		"void",
		"entity leader, vector offset, float offsetSpeed",
		false,
		Script_Player_JoinSkywardLaunch);

	playerStruct->AddFunction(
		"Player_StopFollowSkywardLaunch",
		"Script_Player_StopFollowSkywardLaunch",
		"Stop following a skyward launch",
		"void",
		"bool interrupt",
		false,
		Script_Player_StopFollowSkywardLaunch);

	playerStruct->AddFunction(
		"Player_IsSkywardLaunching",
		"Script_Player_IsSkywardLaunching",
		"True while skyward state is not idle",
		"bool",
		"",
		false,
		Script_Player_IsSkywardLaunching);

	playerStruct->AddFunction(
		"Player_IsSkywardFollowing",
		"Script_Player_IsSkywardFollowing",
		"True while following a skyward leader",
		"bool",
		"",
		false,
		Script_Player_IsSkywardFollowing);

	playerStruct->AddFunction(
		"Player_IsSkywardDiving",
		"Script_Player_IsSkywardDiving",
		"True while freefalling from a skyward launch",
		"bool",
		"",
		false,
		Script_Player_IsSkywardDiving);

	playerStruct->AddFunction(
		"GetSkywardLaunchState",
		"Script_GetSkywardLaunchState",
		"Returns PLAYER_SKYWARD_LAUNCH_STATE_*",
		"int",
		"",
		false,
		Script_GetSkywardLaunchState);

	playerStruct->AddFunction(
		"SkyDive_SetIsFromSkywardLaunch",
		"Script_SkyDive_SetIsFromSkywardLaunch",
		"Mark the next freefall as originating from skyward",
		"void",
		"bool fromSkyward",
		false,
		Script_SkyDive_SetIsFromSkywardLaunch);

	playerStruct->AddFunction(
		"Skydive_IsFromSkywardLaunch",
		"Script_Skydive_IsFromSkywardLaunch",
		"True if the current dive originated from skyward",
		"bool",
		"",
		false,
		Script_Skydive_IsFromSkywardLaunch);
}

//-----------------------------------------------------------------------------
// The dedi PlayerMove only knows MOVETYPE_FLY from skydive: FLY with no freefall
// is reset to WALK before the move dispatch, which would drop every ride back
// to walking. That one reset is skipped while a ride is active.
//-----------------------------------------------------------------------------
static char (*v_Skyward_SetMoveType)(void* pEnt, int moveType, char moveCollide) = nullptr;
static const void* s_pFlyResetReturn = nullptr;

static char Hook_Skyward_SetMoveType(void* pEnt, int moveType, char moveCollide)
{
	if (moveType == SW_MOVETYPE_WALK && _ReturnAddress() == s_pFlyResetReturn
		&& bridge_skyward.GetBool() && SkywardBridge_IsActive(pEnt))
	{
		static bool s_bLogged = false;
		if (!s_bLogged)
		{
			s_bLogged = true;
			Msg(eDLL_T::SERVER, "[SKYWARD] kept MOVETYPE_FLY through the PlayerMove reset player=%p\n", pEnt);
		}
		return 0;
	}
	return v_Skyward_SetMoveType(pEnt, moveType, moveCollide);
}

void VSkywardBridge::GetAdr(void) const
{
	LogFunAdr("Skyward_SetMoveType", v_Skyward_SetMoveType);
	LogVarAdr("Skyward_FlyResetReturn", s_pFlyResetReturn);
}

void VSkywardBridge::GetFun(void) const
{
	Module_FindPattern(g_GameDll, "40 55 56 57 48 83 EC ?? 0F B6 81")
		.GetPtr(v_Skyward_SetMoveType);

	// PlayerMove: cmp movetype, FLY / cmp m_freefallState, 0 / SetMoveType( WALK ) -- return address after the call.
	const CMemory flyReset = Module_FindPattern(g_GameDll,
		"40 80 FE 04 75 1A 48 8B 4F 08 44 39 A9 60 7B 00 00 75 0D BA 02 00 00 00 45 33 C0 E8");
	if (flyReset)
		s_pFlyResetReturn = flyReset.Offset(0x20).RCast<const void*>();

	if (!v_Skyward_SetMoveType || !s_pFlyResetReturn)
		Warning(eDLL_T::SERVER,
			"[SKYWARD] PlayerMove FLY reset unresolved (setmovetype=%d site=%d) -- rides drop to walking\n",
			v_Skyward_SetMoveType ? 1 : 0, s_pFlyResetReturn ? 1 : 0);
}

void VSkywardBridge::Detour(const bool bAttach) const
{
	if (v_Skyward_SetMoveType && s_pFlyResetReturn)
		DetourSetup(&v_Skyward_SetMoveType, &Hook_Skyward_SetMoveType, bAttach);
}
