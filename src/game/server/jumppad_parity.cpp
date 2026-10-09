//=============================================================================//
//
// Purpose: Jump-pad parity on the dedi. The dedi launch pass is reshaped into
// the S21 launch the client predicts: per-command swept touch, air launch, one
// launch per pass, relaunch debounce, ducked vertical scale, the pad's live
// tick and launch line, and the launch state the pad scripts set (slow-mo,
// relaunch ground reset, double-jump grant) moved onto the command it happens
// on. The client half is game/client/jumppad_predict.cpp.
//
//=============================================================================//
#include "core/stdafx.h"


#include "jumppad_parity.h"
#include "trigger_cannon.h"
#include "trigger_gravity.h"
#include "trigger_updraft.h"
#include "glide.h"
#include "entitylist.h" // g_serverEntityList
#include "util_server.h"
#include "player.h"
#include "engine/enginetrace.h"
#include "engine/host_state.h"
#include "engine/server/snapshot_send.h"
#include "public/cmodel.h"
#include "public/engine/IEngineTrace.h"
#include "baseentity.h"
#include "vscript_server_natives.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "public/tier0/memory_patch.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cfloat>
#include <cmath>
#include <vector>

//-----------------------------------------------------------------------------
// Raw layout -- movement ctx, player, trigger.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t JP_CTX_OFF_PLAYER = 8; // CPlayer*

static constexpr ptrdiff_t JP_PLAYER_OFF_DUCKSTATE     = 26096; // int m_duckState
static constexpr ptrdiff_t JP_PLAYER_OFF_TOUCHED_TRIG  = 27096; // EHANDLE array start
static constexpr ptrdiff_t JP_PLAYER_OFF_TOUCHED_COUNT = 27160; // int64 live entry count
static constexpr ptrdiff_t JP_PLAYER_OFF_LAUNCHCOUNT   = 27172; // int m_launchCount
static constexpr ptrdiff_t JP_PLAYER_OFF_SLOWMO        = 0x67C4; // bool m_slowMoEnabled
static constexpr ptrdiff_t JP_PLAYER_OFF_HAS_JUMPED    = 0x6230; // bool m_bHasJumpedSinceTouchedGround
static constexpr ptrdiff_t JP_PLAYER_OFF_LAST_GROUNDED = 0x6214; // float m_flTimeLastTouchedGround, stamped before the move

static constexpr ptrdiff_t JP_TRIG_OFF_VERTOVERRIDE = 3364; // float m_vertOverride
static constexpr ptrdiff_t JP_TRIG_OFF_TRIGGERTYPE  = 3384; // int m_triggerType; 1 = jump pad
static constexpr ptrdiff_t JP_TRIG_OFF_LAUNCHDIR    = 3416; // float[3] m_launchDir

static constexpr int JP_TOUCHED_CAP = 16;
static constexpr int JP_TRIGGER_TYPE_JUMPPAD = 1;

// The client multiplies a ducking or ducked player's pad m_vertOverride by its
// jumppad_vert_override_ducked_scalar, which its boot cfg sets to 0.5.
static ConVar bridge_jumppad_ducked_vert_scalar(
	"bridge_jumppad_ducked_vert_scalar", "0.5", FCVAR_RELEASE,
	"Scalar applied to jump-pad m_vertOverride when the launching player is ducked. "
	"Must match the client's jumppad_vert_override_ducked_scalar.");

// Same name and default as the S21 engine's: delay from a pad launch to the double-jump grant.
static ConVar superjump_jumppad_enable_time(
	"superjump_jumppad_enable_time", "0.1", FCVAR_RELEASE,
	"Seconds after a jump pad launch before the pad's double jump is granted.");

// One restore loop for both mutation kinds.
struct JumpPadMutation_t
{
	void* pEnt;
	float flVertOriginal;
	int   nTypeOriginal;
	bool  bScaledVert;
	bool  bSuppressedType;
};

static constexpr ptrdiff_t JP_ENT_OFF_EDICT            = 0x58; // uint16 edict index
static constexpr ptrdiff_t JP_ENT_OFF_ABS_ORIGIN       = 1104; // float[3]
static constexpr int       JP_VIDX_STARTTOUCH          = 129;
static constexpr int       JP_VIDX_WORLDSPACECENTER    = 168; // Vector WorldSpaceCenter() const, returned via out pointer
// Past either limit the previous command's origin is not a sweep start (teleport, idle).
static constexpr float     JP_SWEEP_MAX_DIST           = 256.0f;
static constexpr float     JP_SWEEP_MAX_AGE            = 0.5f;
// Well past any pad cylinder plus one command's sweep; farther players skip the sweep.
static constexpr float     JP_PAD_NEAR_DIST            = 1024.0f;

static void* s_pfnTriggerHeavyStartTouch = nullptr;
static void* s_pfnEntityWorldSpaceCenter = nullptr; // CBaseEntity's; the pad trigger does not override it

// The launch pass's 'call <on ground or grappled>; test al, al; jz skip'. S21 launches
// a touching player in the air too, so the call is replaced with 'mov al, 1'.
static constexpr ptrdiff_t JP_PASS_OFF_GROUNDGATE = 0x45;
static uint8_t* s_pGroundGate = nullptr;
static uint8_t s_groundGateOrig[5];
static bool s_bGroundGateLifted = false;
// Rebuilt before every frame's snapshots (JumpPad_RegisterPads).
static std::vector<uint32_t> s_padHandles;

// Engine solid-moved pieces: the touch-link enumerator builds the mover's swept capsule
// and clips it against each trigger in the partition, exactly as the client's does.
static constexpr size_t    JP_TOUCHLINKS_SIZE      = 0x76E0;
static constexpr ptrdiff_t JP_TOUCHLINKS_OFF_RAY   = 0x1C70;
static constexpr ptrdiff_t JP_TOUCHLINKS_OFF_COUNT = 0x36CC; // uint32 hits
static constexpr ptrdiff_t JP_TOUCHLINKS_OFF_HITS  = 0x36D0; // uint16 edict[]
static constexpr uint32_t  JP_TOUCHLINKS_MAX_HITS  = 0x2000;
static constexpr uint16_t  JP_PARTITION_TRIGGERS   = 2;

static void* (*v_TouchLinks_Init)(void* pLinks, int nMoverEdict, const float* pPrevAbsOrigin) = nullptr;
static void (*v_Partition_EnumerateAlongRay)(void* pPartition, uint16_t nListMask, void** ppRay, int nFlags, void* pEnum) = nullptr;
static void* s_pServerPartition = nullptr;
static void** s_ppServerGameEnts = nullptr; // vtable +0x20 = MarkEntitiesAsTouching(edict, edict)

struct JumpPadSweep_t
{
	float vStart[3];
	float flTime;
	bool bValid;
};
static SDKEntityMap<JumpPadSweep_t> s_sweepMap(ESide::Server, "jumppad.sweep");

//-----------------------------------------------------------------------------
// Purpose: a launch off a pad with m_enableDoubleJump schedules the double-jump
// grant; the client predicts the same time from the same networked field.
//-----------------------------------------------------------------------------
static void JumpPad_ArmDoubleJump(void* pPlayer, const void* pPad)
{
	if (!gpGlobals || !TriggerCannon_GetEnableDoubleJump(pPad))
		return;

	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_timeShouldTryGivePlayerDoubleJump),
		gpGlobals->curTime + superjump_jumppad_enable_time.GetFloat());
	MarkEntityEdictDirty(pPlayer);
}

static void JumpPad_ArmLimitedAirLocks(void* pPlayer)
{
	if (!pPlayer || !g_serverEntityList)
		return;

	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);
	int64_t nCount = *reinterpret_cast<int64_t*>(pPlayerBytes + JP_PLAYER_OFF_TOUCHED_COUNT);
	if (nCount < 0)
		nCount = 0;
	if (nCount > JP_TOUCHED_CAP)
		nCount = JP_TOUCHED_CAP;

	for (int i = 0; i < static_cast<int>(nCount); ++i)
	{
		const uint32_t rawHandle = *reinterpret_cast<uint32_t*>(
			pPlayerBytes + JP_PLAYER_OFF_TOUCHED_TRIG + 4 * i);
		if (rawHandle == 0xFFFFFFFFu)
			continue;

		const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
		void* const pEnt = g_serverEntityList->LookupEntity(handle);
		if (!pEnt)
			continue;

		const int nType = *reinterpret_cast<const int*>(
			static_cast<uint8_t*>(pEnt) + JP_TRIG_OFF_TRIGGERTYPE);
		if (nType != JP_TRIGGER_TYPE_JUMPPAD)
			continue;

		TriggerCannon_OnJumpPadLaunched(pPlayer, pEnt);
		JumpPad_ArmDoubleJump(pPlayer, pEnt);
	}
}

//-----------------------------------------------------------------------------
// Purpose: launcher flight slow-mo. The scripts set it from a server-only enter
// callback and clear it a frame after landing, neither of which the client can
// predict; both engines instead set it on the launch command and clear it on the
// landing command, and the scripts' calls find it already in that state.
//-----------------------------------------------------------------------------
static void JumpPad_SetSlowMo(void* pPlayer, const bool bEnabled)
{
	uint8_t* const pField = static_cast<uint8_t*>(pPlayer) + JP_PLAYER_OFF_SLOWMO;
	if ((*pField != 0) == bEnabled)
		return;

	*pField = bEnabled ? 1 : 0;
	MarkEntityEdictDirty(pPlayer);
}

void JumpPad_OnLauncherLaunched(void* pPlayer)
{
	if (pPlayer)
		JumpPad_SetSlowMo(pPlayer, true);
}

// The S21 launch writes m_jumpPadDebounceExpireTime = now + this, so the previous
// launch's time is the old value minus it.
static constexpr float JP_LAUNCH_DEBOUNCE_WINDOW = 1.0f;
// Covers the float error of (now + window) - window; a pad flight cannot land this soon.
static constexpr float JP_RELAUNCH_GROUND_EPS = 0.01f;

// The entry, not a trampoline: a call passes through its detours as the script's does.
static void (*v_Player_TouchGround)(void* pPlayer) = nullptr;

//-----------------------------------------------------------------------------
// Purpose: a pad launch ends the previous flight's double-jump thread, whose end
// calls the server-only player.TouchGround() in mid-air. Run that reset on every
// launch that follows the previous one without a ground touch in between, the
// test the client makes from the same two predicted fields.
//-----------------------------------------------------------------------------
static void JumpPad_ResetAirStateOnRelaunch(void* pPlayer, const float flDebounceBefore, const float flLastGrounded)
{
	if (!v_Player_TouchGround || !(flDebounceBefore > 0.0f))
		return;

	if (flLastGrounded > flDebounceBefore - JP_LAUNCH_DEBOUNCE_WINDOW + JP_RELAUNCH_GROUND_EPS)
		return;

	v_Player_TouchGround(pPlayer);
	MarkEntityEdictDirty(pPlayer);
}

static int64_t JumpPad_OrigAndArm(void* pCtx, void* pPlayer)
{
	if (!pPlayer)
		return JumpPad__ApplyLaunchPass(pCtx);

	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);
	const int nBefore = *reinterpret_cast<int*>(pPlayerBytes + JP_PLAYER_OFF_LAUNCHCOUNT);
	const float flDebounceBefore = PlayerExtend_GetF32(pPlayer, offsetof(PlayerExtendWire, m_jumpPadDebounceExpireTime));
	const float flLastGrounded = *reinterpret_cast<const float*>(pPlayerBytes + JP_PLAYER_OFF_LAST_GROUNDED);

	const int64_t nResult = JumpPad__ApplyLaunchPass(pCtx);

	if (*reinterpret_cast<int*>(pPlayerBytes + JP_PLAYER_OFF_LAUNCHCOUNT) > nBefore)
	{
		JumpPad_ResetAirStateOnRelaunch(pPlayer, flDebounceBefore, flLastGrounded);
		// The S21 launch counts as a jump, so the air jump that follows is a
		// double jump rather than a grace-period ground jump.
		*reinterpret_cast<uint8_t*>(pPlayerBytes + JP_PLAYER_OFF_HAS_JUMPED) = 1;
		Glide_StopOnLaunch(pPlayer);
		JumpPad_OnLauncherLaunched(pPlayer);
		JumpPad_ArmLimitedAirLocks(pPlayer);
	}

	return nResult;
}

static void* JumpPad_Resolve(const uint32_t rawHandle)
{
	if (rawHandle == 0xFFFFFFFFu || !g_serverEntityList)
		return nullptr;
	return g_serverEntityList->LookupEntity(CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle)));
}

static bool JumpPad_IsPad(const void* pEnt)
{
	const void* const* const vtbl = *reinterpret_cast<const void* const* const*>(pEnt);
	return vtbl && vtbl[JP_VIDX_STARTTOUCH] == s_pfnTriggerHeavyStartTouch
		&& *reinterpret_cast<const int*>(static_cast<const uint8_t*>(pEnt) + JP_TRIG_OFF_TRIGGERTYPE) == JP_TRIGGER_TYPE_JUMPPAD;
}

//-----------------------------------------------------------------------------
// Purpose: the client places a pad from wire origins, which carry 1/32-unit
// precision; a pad between grid points sits up to 1/32 unit apart on the two
// sides, enough for a slow (crouched) approach to touch it a command apart.
// Put the pad prop and its trigger on the grid so both test the same cylinder.
//-----------------------------------------------------------------------------
class CJumpPadAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_hMoveParent;
	using CBaseEntity::m_hOwnerEntity;
};

static constexpr float JP_WIRE_COORD_STEPS = 32.0f; // COORD_MP fractional resolution

static bool JumpPad_OnWireGrid(const Vector3D& v)
{
	for (int i = 0; i < 3; ++i)
	{
		const float flScaled = v[i] * JP_WIRE_COORD_STEPS;
		if (flScaled != std::round(flScaled))
			return false;
	}
	return true;
}

static void JumpPad_SnapEntityToWireGrid(CBaseEntity* pEnt)
{
	const Vector3D& v = pEnt->Diag_AbsOrigin();
	if (JumpPad_OnWireGrid(v))
		return;
	const float snapped[3] =
	{
		std::round(v[0] * JP_WIRE_COORD_STEPS) / JP_WIRE_COORD_STEPS,
		std::round(v[1] * JP_WIRE_COORD_STEPS) / JP_WIRE_COORD_STEPS,
		std::round(v[2] * JP_WIRE_COORD_STEPS) / JP_WIRE_COORD_STEPS,
	};
	ServerNatives_SetAbsOrigin(pEnt, snapped);
}

static void JumpPad_SnapPadToWireGrid(void* pTrigger)
{
	if (!ServerNatives_SetAbsOriginResolved())
		return;

	CBaseEntity* const pTrig = static_cast<CBaseEntity*>(pTrigger);
	CBaseEntity* pPad = nullptr;
	if (pTrig->Diag_HasMoveParent())
	{
		const uint32_t hParent = static_cast<uint32_t>(static_cast<CJumpPadAccess*>(pTrig)->m_hMoveParent.ToInt());
		pPad = static_cast<CBaseEntity*>(JumpPad_Resolve(hParent));
		// A pad riding a mover keeps its own origin; snapping it would fight the mover.
		if (!pPad || pPad->Diag_HasMoveParent())
			return;
	}

	if (pPad)
		JumpPad_SnapEntityToWireGrid(pPad);
	JumpPad_SnapEntityToWireGrid(pTrig);
}

// m_nextLaunchTime, unused by the pad launch on either engine: for a pad it holds
// the time of the first snapshot that carries it.
static int s_nPadLiveTimeOff = 0;

static float* JumpPad_LiveTimeField(void* pPad)
{
	if (s_nPadLiveTimeOff == 0)
	{
		const int nOff = DTExtend_GetOffset("DT_TriggerCylinderHeavy", "m_nextLaunchTime");
		s_nPadLiveTimeOff = nOff > 0 ? nOff : -1;
		if (s_nPadLiveTimeOff < 0)
			Warning(eDLL_T::SERVER, "[JP-PARITY] m_nextLaunchTime offset unresolved -- a new jump pad can "
				"launch a player before the client has it\n");
	}
	return s_nPadLiveTimeOff > 0 ? reinterpret_cast<float*>(static_cast<uint8_t*>(pPad) + s_nPadLiveTimeOff) : nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: runs just before each frame's snapshots, so a pad created this frame
// leaves in its first snapshot already on the wire grid and carrying the time
// that snapshot was built.
//-----------------------------------------------------------------------------
static bool s_bPadRegistryRunning = false;

static void JumpPad_RegisterPads(void)
{
	if (!g_serverEntityList || !gpGlobals || !s_pfnTriggerHeavyStartTouch)
		return;

	s_bPadRegistryRunning = true;
	s_padHandles.clear();
	const float flSnapshotTime = static_cast<float>(gpGlobals->tickCount) * gpGlobals->tickInterval;

	for (const CEntInfo* pInfo = g_serverEntityList->FirstEntInfo(); pInfo; pInfo = pInfo->m_pNext)
	{
		void* const pEnt = pInfo->m_pEntity;
		if (!pEnt || !JumpPad_IsPad(pEnt))
			continue;

		s_padHandles.push_back(SDKEntityState_GetHandle(pEnt).Raw());
		JumpPad_SnapPadToWireGrid(pEnt);

		float* const pLiveTime = JumpPad_LiveTimeField(pEnt);
		if (pLiveTime && !(*pLiveTime > 0.0f))
		{
			*pLiveTime = flSnapshotTime;
			MarkEntityEdictDirty(pEnt);
		}
	}
}

// The live time is a whole tick; half a tick absorbs float error in the networked copy.
static constexpr float JP_LIVE_TICK_MARGIN = 0.5f;

//-----------------------------------------------------------------------------
// Purpose: the client launches off a pad only once it has it, and a usercmd
// carries the tick of the snapshot its prediction interpolated from. A pad is
// live for a command whose base snapshot is newer than the pad's first one; the
// client gates on the same command field, so both open it on the same command.
//-----------------------------------------------------------------------------
static bool JumpPad_IsLiveForCommand(void* pPlayer, void* pPad)
{
	if (!gpGlobals || !s_bPadRegistryRunning || !(gpGlobals->tickInterval > 0.0f))
		return true;

	const float* const pLiveTime = JumpPad_LiveTimeField(pPad);
	if (!pLiveTime)
		return true;
	if (!(*pLiveTime > 0.0f))
		return false; // created this frame: no snapshot has carried it yet

	const CUserCmd* const pCmd = static_cast<CPlayer*>(pPlayer)->GetCurrentUserCommand();
	if (!pCmd)
		return true;

	return static_cast<float>(pCmd->tick_count) > *pLiveTime / gpGlobals->tickInterval + JP_LIVE_TICK_MARGIN;
}

// Solid, window, grate, physics clip, moveable, monster.
static constexpr unsigned int JP_LAUNCH_LINE_MASK = 0x200420B;
static constexpr int JP_TRACE_DETAIL_HIGH = 2;

static Vector3D JumpPad_WorldSpaceCenter(void* pEnt)
{
	using WorldSpaceCenterFn_t = Vector3D* (__fastcall*)(void*, Vector3D*);
	Vector3D vCenter;
	(*reinterpret_cast<WorldSpaceCenterFn_t* const*>(pEnt))[JP_VIDX_WORLDSPACECENTER](pEnt, &vCenter);
	return vCenter;
}

//-----------------------------------------------------------------------------
// Purpose: the client's launch first traces world and static props from the
// pad (its owner prop's center, else the trigger origin) to the player's center
// and launches only on a clear line. Run the same trace before the dedi's.
//-----------------------------------------------------------------------------
static bool JumpPad_LaunchLineClear(void* pPlayer, void* pPad)
{
	if (!g_pEngineTraceServer || !s_pfnEntityWorldSpaceCenter)
		return true;

	if ((*reinterpret_cast<void* const* const*>(pPad))[JP_VIDX_WORLDSPACECENTER] != s_pfnEntityWorldSpaceCenter)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[JP-PARITY] WorldSpaceCenter vtable slot mismatch -- pad launch line test "
				"disabled; launches through walls the client refuses mispredict\n");
		}
		return true;
	}

	Vector3D vStart;
	void* const pOwner = JumpPad_Resolve(static_cast<uint32_t>(static_cast<CJumpPadAccess*>(pPad)->m_hOwnerEntity.ToInt()));
	if (pOwner)
		vStart = JumpPad_WorldSpaceCenter(pOwner);
	else
	{
		TriggerPass_EnsureAbsOrigin(pPad);
		vStart = static_cast<CBaseEntity*>(pPad)->Diag_AbsOrigin();
	}
	const Vector3D vEnd = JumpPad_WorldSpaceCenter(pPlayer);

	Ray_t ray(vStart, vEnd);
	ray.m_nDetailLevel = JP_TRACE_DETAIL_HIGH;
	trace_t tr;
	g_pEngineTraceServer->TraceRay(ray, JP_LAUNCH_LINE_MASK, &tr);
	return tr.fraction >= 1.0f;
}

static inline uint16_t JumpPad_Edict(const void* pEnt)
{
	return *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(pEnt) + JP_ENT_OFF_EDICT);
}

//-----------------------------------------------------------------------------
// Purpose: the client touches triggers after every command by sweeping the
// player's capsule from the command's start origin to its end. The dedi only
// touches from a once-per-frame player think with a static capsule, so a pad
// was entered up to a frame late, or missed when the sweep crossed it. Run the
// engine's own sweep for the previous command here, ahead of this command's
// launch pass (the client runs its pass right after the same touch), and link
// only the jump pads it hits; every other trigger keeps its frame touch.
//-----------------------------------------------------------------------------
static void JumpPad_CmdTouch(void* pPlayer)
{
	if (!v_TouchLinks_Init || !g_serverEntityList || !gpGlobals)
		return;

	TriggerPass_EnsureAbsOrigin(pPlayer);
	const float* const pOrigin = reinterpret_cast<const float*>(static_cast<uint8_t*>(pPlayer) + JP_ENT_OFF_ABS_ORIGIN);
	const float flNow = gpGlobals->curTime;

	JumpPadSweep_t& st = s_sweepMap[pPlayer];
	const float* pStart = nullptr;
	if (st.bValid && flNow >= st.flTime && flNow - st.flTime <= JP_SWEEP_MAX_AGE)
	{
		const float dx = pOrigin[0] - st.vStart[0];
		const float dy = pOrigin[1] - st.vStart[1];
		const float dz = pOrigin[2] - st.vStart[2];
		if (dx * dx + dy * dy + dz * dz <= JP_SWEEP_MAX_DIST * JP_SWEEP_MAX_DIST)
			pStart = st.vStart;
	}

	float vSweepStart[3];
	if (pStart)
		memcpy(vSweepStart, pStart, sizeof(vSweepStart));

	memcpy(st.vStart, pOrigin, sizeof(st.vStart));
	st.flTime = flNow;
	st.bValid = true;

	bool bNearPad = false;
	for (const uint32_t hPad : s_padHandles)
	{
		void* const pPad = JumpPad_Resolve(hPad);
		if (!pPad)
			continue;
		const float* const pPadOrigin = reinterpret_cast<const float*>(static_cast<uint8_t*>(pPad) + JP_ENT_OFF_ABS_ORIGIN);
		const float dx = pOrigin[0] - pPadOrigin[0];
		const float dy = pOrigin[1] - pPadOrigin[1];
		const float dz = pOrigin[2] - pPadOrigin[2];
		if (dx * dx + dy * dy + dz * dz <= JP_PAD_NEAR_DIST * JP_PAD_NEAR_DIST)
		{
			bNearPad = true;
			break;
		}
	}
	if (!bNearPad)
		return;

	const uint16_t nPlayerEdict = JumpPad_Edict(pPlayer);
	if (nPlayerEdict == 0xFFFF)
		return;

	alignas(16) uint8_t links[JP_TOUCHLINKS_SIZE];
	v_TouchLinks_Init(links, nPlayerEdict, pStart ? vSweepStart : nullptr);
	void* pRay = links + JP_TOUCHLINKS_OFF_RAY;
	v_Partition_EnumerateAlongRay(s_pServerPartition, JP_PARTITION_TRIGGERS, &pRay, 0, links);

	uint32_t nHits = *reinterpret_cast<const uint32_t*>(links + JP_TOUCHLINKS_OFF_COUNT);
	if (nHits > JP_TOUCHLINKS_MAX_HITS)
		nHits = JP_TOUCHLINKS_MAX_HITS;
	const uint16_t* const pHits = reinterpret_cast<const uint16_t*>(links + JP_TOUCHLINKS_OFF_HITS);

	void* const pEnts = *s_ppServerGameEnts;
	if (!pEnts)
		return;
	using MarkTouchingFn_t = void(__fastcall*)(void*, int, int);
	const MarkTouchingFn_t fnMarkTouching = reinterpret_cast<MarkTouchingFn_t>((*reinterpret_cast<void***>(pEnts))[4]);

	for (const uint32_t hPad : s_padHandles)
	{
		void* const pPad = JumpPad_Resolve(hPad);
		if (!pPad || !JumpPad_IsPad(pPad))
			continue;

		const uint16_t nPadEdict = JumpPad_Edict(pPad);
		bool bHit = false;
		for (uint32_t i = 0; i < nHits && !bHit; ++i)
			bHit = pHits[i] == nPadEdict;
		if (bHit)
			fnMarkTouching(pEnts, nPadEdict, nPlayerEdict);
	}
}

static void JumpPad_LiftGroundGate(const bool bLift)
{
	if (!s_pGroundGate || bLift == s_bGroundGateLifted)
		return;

	static const uint8_t kAlwaysOn[5] = { 0xB0, 0x01, 0x90, 0x90, 0x90 }; // mov al, 1
	if (Mem_PatchCode(s_pGroundGate, bLift ? kAlwaysOn : s_groundGateOrig, sizeof(kAlwaysOn)))
		s_bGroundGateLifted = bLift;
}

//-----------------------------------------------------------------------------
// Purpose: JumpPad launch pass -- relaunch debounce, one launch per pass, the
// pad's live tick and launch line, and the ducked vert scale. The S21 launch
// arms the debounce, which turns away every later pad in the same pass; the dedi
// pass launches off each touched pad in turn.
//-----------------------------------------------------------------------------
static int64_t JumpPad_ApplyLaunchPassInner(void* pCtx, void* pPlayer)
{
	if (!pPlayer || !g_serverEntityList || !gpGlobals)
		return JumpPad_OrigAndArm(pCtx, pPlayer);

	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);
	const bool bDebounced = gpGlobals->curTime <= PlayerExtend_GetF32(pPlayer,
		offsetof(PlayerExtendWire, m_jumpPadDebounceExpireTime));

	// The client scales the vertical override at m_duckState 1 and 2.
	const int nDuckState = *reinterpret_cast<const int*>(pPlayerBytes + JP_PLAYER_OFF_DUCKSTATE);
	const float flDuckScalar = bridge_jumppad_ducked_vert_scalar.GetFloat();
	const bool bDuckScale = static_cast<unsigned>(nDuckState - 1) <= 1u
		&& std::isfinite(flDuckScalar) && flDuckScalar > 0.0f && flDuckScalar != 1.0f;

	JumpPadMutation_t mut[JP_TOUCHED_CAP];
	int nMut = 0;
	bool bPadTaken = false;

	int64_t nCount = *reinterpret_cast<int64_t*>(pPlayerBytes + JP_PLAYER_OFF_TOUCHED_COUNT);
	if (nCount < 0)
		nCount = 0;
	if (nCount > JP_TOUCHED_CAP)
		nCount = JP_TOUCHED_CAP;

	for (int i = 0; i < static_cast<int>(nCount); ++i)
	{
		const uint32_t rawHandle = *reinterpret_cast<uint32_t*>(
			pPlayerBytes + JP_PLAYER_OFF_TOUCHED_TRIG + 4 * i);
		if (rawHandle == 0xFFFFFFFFu)
			continue;

		const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
		void* const pEnt = g_serverEntityList->LookupEntity(handle);
		if (!pEnt)
			continue;

		uint8_t* const pEntBytes = static_cast<uint8_t*>(pEnt);
		int* const pType = reinterpret_cast<int*>(pEntBytes + JP_TRIG_OFF_TRIGGERTYPE);
		if (*pType != JP_TRIGGER_TYPE_JUMPPAD)
			continue;

		// Poke the type so the engine skips this pad; restored after the pass.
		if (bDebounced || bPadTaken || !JumpPad_IsLiveForCommand(pPlayer, pEnt)
			|| !JumpPad_LaunchLineClear(pPlayer, pEnt))
		{
			mut[nMut].pEnt = pEnt;
			mut[nMut].flVertOriginal = 0.0f;
			mut[nMut].nTypeOriginal = *pType;
			mut[nMut].bScaledVert = false;
			mut[nMut].bSuppressedType = true;
			++nMut;

			*pType = 0;
			continue;
		}

		bPadTaken = true;
		if (!bDuckScale)
			continue;

		// Engine only takes the vert-override path when launch dir is unset.
		const float* const pLaunchDir = reinterpret_cast<const float*>(
			pEntBytes + JP_TRIG_OFF_LAUNCHDIR);
		if (pLaunchDir[0] != 0.0f || pLaunchDir[1] != 0.0f || pLaunchDir[2] != 0.0f)
			continue;

		float* const pVert = reinterpret_cast<float*>(pEntBytes + JP_TRIG_OFF_VERTOVERRIDE);

		mut[nMut].pEnt = pEnt;
		mut[nMut].flVertOriginal = *pVert;
		mut[nMut].nTypeOriginal = 0;
		mut[nMut].bScaledVert = true;
		mut[nMut].bSuppressedType = false;
		++nMut;

		*pVert *= flDuckScalar;
	}

	const int nLaunchBefore = *reinterpret_cast<int*>(pPlayerBytes + JP_PLAYER_OFF_LAUNCHCOUNT);

	const int64_t nResult = JumpPad_OrigAndArm(pCtx, pPlayer);

	// Restore before any further work -- these fields are replicated.
	for (int i = 0; i < nMut; ++i)
	{
		uint8_t* const pEntBytes = static_cast<uint8_t*>(mut[i].pEnt);
		if (mut[i].bScaledVert)
			*reinterpret_cast<float*>(pEntBytes + JP_TRIG_OFF_VERTOVERRIDE) = mut[i].flVertOriginal;
		if (mut[i].bSuppressedType)
			*reinterpret_cast<int*>(pEntBytes + JP_TRIG_OFF_TRIGGERTYPE) = mut[i].nTypeOriginal;
	}

	if (*reinterpret_cast<int*>(pPlayerBytes + JP_PLAYER_OFF_LAUNCHCOUNT) > nLaunchBefore)
	{
		PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_jumpPadDebounceExpireTime),
			gpGlobals->curTime + JP_LAUNCH_DEBOUNCE_WINDOW);
		MarkEntityEdictDirty(pPlayer);
	}

	return nResult;
}

// The player in PlayerMove right now and whether it began the command airborne.
static void* s_pMovePlayer = nullptr;
static bool s_bMoveStartedAirborne = false;

void JumpPad_OnPlayerMoveBegin(void* pCtx)
{
	s_pMovePlayer = pCtx ? *reinterpret_cast<void**>(static_cast<uint8_t*>(pCtx) + JP_CTX_OFF_PLAYER) : nullptr;
	s_bMoveStartedAirborne = s_pMovePlayer
		&& !(reinterpret_cast<CBaseEntity*>(s_pMovePlayer)->GetFlags() & FL_ONGROUND);
}

//-----------------------------------------------------------------------------
// Purpose: this detour owns the only entry into the predicted-trigger pass, so
// the trigger types the engine has no case for are applied here rather than
// through a second hook on the same function.
//-----------------------------------------------------------------------------
static int64_t Hook_JumpPad_ApplyLaunchPass(void* pCtx)
{
	void* const pPlayer = pCtx ? *reinterpret_cast<void**>(static_cast<uint8_t*>(pCtx) + JP_CTX_OFF_PLAYER) : nullptr;
	if (pPlayer)
		JumpPad_CmdTouch(pPlayer);

	const int64_t nResult = JumpPad_ApplyLaunchPassInner(pCtx, pPlayer);
	TriggerCannon_ApplyPass(pCtx);
	TriggerGravity_ApplyPass(pCtx);
	UpdraftBridge_ApplyPass(pCtx);
	return nResult;
}

// An uncaught error in the grant callback schedules a host shutdown; undo it.
static void JumpPad_CancelHostShutdown(const HostStates_t iStateBefore, const HostStates_t iNextBefore)
{
	if (!g_pHostState)
		return;
	if (iStateBefore == HostStates_t::HS_GAME_SHUTDOWN || iNextBefore == HostStates_t::HS_GAME_SHUTDOWN)
		return;
	if (g_pHostState->m_iCurrentState != HostStates_t::HS_GAME_SHUTDOWN
		&& g_pHostState->m_iNextState != HostStates_t::HS_GAME_SHUTDOWN)
		return;

	g_pHostState->m_iCurrentState = iStateBefore;
	g_pHostState->m_iNextState = iNextBefore;
	Warning(eDLL_T::SERVER, "[JP-DJUMP] cancelled host shutdown scheduled by CodeCallback_GivePlayerDoubleJump\n");
}

//-----------------------------------------------------------------------------
// Purpose: end of the player's movement for a command, where the client makes
// the same two checks: landing ends launcher slow-mo, and once the double-jump
// grant time passes it is cleared and the script code callback hands it out.
//-----------------------------------------------------------------------------
void JumpPad_OnPlayerMoveEnd(void* pCtx)
{
	if (!pCtx)
		return;

	void* const pPlayer = *reinterpret_cast<void**>(static_cast<uint8_t*>(pCtx) + JP_CTX_OFF_PLAYER);
	if (!pPlayer)
		return;

	const bool bLanded = pPlayer == s_pMovePlayer && s_bMoveStartedAirborne
		&& (reinterpret_cast<CBaseEntity*>(pPlayer)->GetFlags() & FL_ONGROUND);
	s_pMovePlayer = nullptr;
	if (bLanded)
		JumpPad_SetSlowMo(pPlayer, false);

	if (!gpGlobals || !g_pServerScript)
		return;

	// A player without a sidecar slot reads every field as 0, which would grant every command.
	PlayerExtendBundle bundle;
	if (!PlayerExtend_GetBundle(pPlayer, &bundle))
		return;
	if (!(gpGlobals->curTime >= bundle.player.m_timeShouldTryGivePlayerDoubleJump))
		return;

	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_timeShouldTryGivePlayerDoubleJump), FLT_MAX);
	MarkEntityEdictDirty(pPlayer);

	const HSCRIPT hPlayer = reinterpret_cast<CBaseEntity*>(pPlayer)->GetScriptInstance();
	const HSCRIPT hFunc = g_pServerScript->FindFunction("CodeCallback_GivePlayerDoubleJump", nullptr, nullptr);
	if (!hPlayer || !hFunc)
		return;

	const HostStates_t iStateBefore = g_pHostState ? g_pHostState->m_iCurrentState : HostStates_t::HS_RUN;
	const HostStates_t iNextBefore = g_pHostState ? g_pHostState->m_iNextState : HostStates_t::HS_RUN;

	ScriptVariant_t arg;
	arg = hPlayer;
	if (g_pServerScript->ExecuteFunction(hFunc, &arg, 1, nullptr, nullptr) == SCRIPT_ERROR)
	{
		Warning(eDLL_T::SERVER, "[JP-DJUMP] CodeCallback_GivePlayerDoubleJump SCRIPT_ERROR player=%p\n", pPlayer);
		JumpPad_CancelHostShutdown(iStateBefore, iNextBefore);
	}
}

void VJumpPadParity::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"40 55 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 48 8B F9 48 8B 49 08 80 B9 99 04 00 00 00 75 ?? 48 8B 05 ?? ?? ?? ?? F3 0F 10 48 28 0F 2F 89 B4 15 00 00")
		.GetPtr(JumpPad__ApplyLaunchPass);

	if (!JumpPad__ApplyLaunchPass)
		Warning(eDLL_T::SERVER,
			"[JP-PARITY] JumpPad::ApplyLaunchPass pattern unresolved -- "
			"jump pads keep the stock launch and mispredict\n");

	if (JumpPad__ApplyLaunchPass)
	{
		uint8_t* const pSite = reinterpret_cast<uint8_t*>(JumpPad__ApplyLaunchPass) + JP_PASS_OFF_GROUNDGATE;
		static const uint8_t kAfter[] = { 0x84, 0xC0, 0x0F, 0x84 };
		if (pSite[0] == 0xE8 && !memcmp(pSite + 5, kAfter, sizeof(kAfter)))
		{
			s_pGroundGate = pSite;
			memcpy(s_groundGateOrig, pSite, sizeof(s_groundGateOrig));
		}
		else
			Warning(eDLL_T::SERVER, "[JP-PARITY] launch pass ground gate not at +0x45 -- pads launch only from the ground\n");
	}

	// Server half: the 0xD38 trigger-type and 0x5A4 player mask immediates are server-only.
	Module_FindPattern(g_GameDll,
		"40 56 57 48 83 EC 28 8B 82 A4 05 00 00 48 8B F2 48 8B F9 85 81 38 0D 00 00 0F 85")
		.GetPtr(s_pfnTriggerHeavyStartTouch);
	if (!s_pfnTriggerHeavyStartTouch)
		Warning(eDLL_T::SERVER,
			"[JP-PARITY] CTriggerCylinderHeavy::StartTouch pattern unresolved -- "
			"jump pads keep the once-per-frame touch and launch late\n");

	// Engine solid-moved (server): builds the touch-link sweep at +0x1B, enumerates the
	// partition at +0x45/+0x51, marks touching through the server game-ents at +0x70.
	const CMemory solidMoved = Module_FindPattern(g_GameDll,
		"48 89 54 24 10 53 B8 00 77 00 00 E8 ?? ?? ?? ?? 48 2B E0 0F B7 D1 48 8D 4C 24 30 "
		"E8 ?? ?? ?? ?? 48 8D 84 24 A0 1C 00 00 BA 02 00 00 00");
	if (solidMoved)
	{
		solidMoved.Offset(0x1B).FollowNearCall().GetPtr(v_TouchLinks_Init);
		solidMoved.Offset(0x51).FollowNearCall().GetPtr(v_Partition_EnumerateAlongRay);
		s_pServerPartition = solidMoved.Offset(0x45).ResolveRelativeAddress(3, 7).RCast<void*>();
		s_ppServerGameEnts = solidMoved.Offset(0x70).ResolveRelativeAddress(3, 7).RCast<void**>();
	}
	if (!v_TouchLinks_Init || !v_Partition_EnumerateAlongRay || !s_pServerPartition || !s_ppServerGameEnts)
	{
		v_TouchLinks_Init = nullptr;
		Warning(eDLL_T::SERVER,
			"[JP-PARITY] engine solid-moved pattern unresolved -- "
			"jump pads keep the once-per-frame touch and launch late\n");
	}

	// CBaseEntity WorldSpaceCenter, server half: the collision property at +0x328 is a server-only offset.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 81 C1 28 03 00 00 48 8B DA E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3")
		.GetPtr(s_pfnEntityWorldSpaceCenter);
	if (!s_pfnEntityWorldSpaceCenter)
		Warning(eDLL_T::SERVER,
			"[JP-PARITY] WorldSpaceCenter pattern unresolved -- pad launch line test disabled; launches "
			"through walls the client refuses mispredict\n");

	// Player TouchGround, server half: the +0x662C wall normal is a server-only offset.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 F2 0F 10 89 2C 66 00 00 0F 57 DB 8B 81 34 66 00 00")
		.GetPtr(v_Player_TouchGround);
	if (!v_Player_TouchGround)
		Warning(eDLL_T::SERVER,
			"[JP-PARITY] Player TouchGround pattern unresolved -- an air relaunch keeps the "
			"previous flight's double jump spent while the client resets it\n");
}

void VJumpPadParity::Detour(const bool bAttach) const
{
	g_pfnSnapshotSend_BeforeClientSnapshots = bAttach ? &JumpPad_RegisterPads : nullptr;
	JumpPad_LiftGroundGate(bAttach);

	if (JumpPad__ApplyLaunchPass)
		DetourSetup(&JumpPad__ApplyLaunchPass, &Hook_JumpPad_ApplyLaunchPass, bAttach);
}

