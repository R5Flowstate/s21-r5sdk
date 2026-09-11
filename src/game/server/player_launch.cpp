//=============================================================================//
//
// Purpose: S3 port of PlayerLaunch. See player_launch.h.
//
//=============================================================================//
#include "core/stdafx.h"


#include "player_launch.h"
#include <cstring>
#include "translocation.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/dt_extend.h"
#include "mathlib/mathlib.h"
#include "public/const.h"
#include "public/edict.h"

extern CGlobalVars* gpGlobals;

static constexpr ptrdiff_t PL_CTX_OFF_PLAYER   = 8;
static constexpr ptrdiff_t PL_CTX_OFF_MOVEDATA = 16;
static constexpr ptrdiff_t PL_MV_OFF_VELOCITY  = 304; // CMoveData velocity float[3]

static constexpr ptrdiff_t PL_OFF_GROUND_ENTITY = 0x3C4; // m_hGroundEntity
static constexpr ptrdiff_t PL_OFF_ABS_ORIGIN   = 1104; // float[3] abs origin
static constexpr ptrdiff_t PL_OFF_HAS_JUMPED   = 0x6230; // m_bHasJumpedSinceTouchedGround
static constexpr ptrdiff_t PL_OFF_SLIDING      = 26565;  // m_sliding

static constexpr float PL_LAND_MIN_TIME = 0.2f;
static constexpr LONG  PL_ACTIVATE_MIN_SAMPLES = 1;
static constexpr float PL_ACTIVATE_MAX_HOLD    = 0.50f;
static constexpr float PL_LOCK_MAX_TIME        = 8.0f;

struct PlayerLaunchState
{
	bool  m_activate = false;
	bool  m_lock3pRotation = false;
	bool  m_applied = false;
	bool  m_leftGround = false;
	float m_velocity[3] = {};
	float m_startTime = 0.0f;
};

static SDKEntityMap<PlayerLaunchState> s_launchMap(ESide::Server, "playerLaunch.srv");

struct PlayerLaunchWireSlot
{
	volatile uint64_t handleKey;
	volatile LONG     seq;
	volatile LONG     activateSamples;
	PlayerLaunchWire  wire;
};

static PlayerLaunchWireSlot s_launchWireSlots[64];
static LONG s_launchWireCursor = 0;
static volatile LONG s_launchWireUsed = 0;

static ConVar sdk_player_launch_diag("sdk_player_launch_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log PlayerLaunch latch/apply/land. 0=off.");

static thread_local void* s_fullWalkCtx = nullptr;

static int  s_offDuckToggleOn = -1;
static int  s_offWallrunFloorHeight = -1;
static bool s_launchOffsetsResolved = false;

static __int64 (__fastcall *v_CGameMovement__CheckJumpButton)(void* ctx) = nullptr;
static __int64 (__fastcall *v_CGameMovement__SetGroundEntity)(void* ctx, void* pGroundEnt) = nullptr;
static __int64 (__fastcall *v_CPlayer__ClearWallRun)(void* pPlayer) = nullptr;
static __int64 (__fastcall *v_CBaseEntity__SetAbsAngles)(void* pEntity, const float* pAngles) = nullptr;

static inline float PlayerLaunch_CurTime(void)
{
	return gpGlobals ? gpGlobals->curTime : 0.0f;
}

static inline uint64_t PlayerLaunchWire_PackHandle(const SDKEntityHandle& h)
{
	return h.IsValid() ? static_cast<uint64_t>(h.Raw()) : 0;
}

static PlayerLaunchWireSlot* PlayerLaunchWire_FindSlot(uint64_t key)
{
	if (!key || !s_launchWireUsed)
		return nullptr;
	for (PlayerLaunchWireSlot& slot : s_launchWireSlots)
	{
		if (slot.handleKey == key)
			return &slot;
	}
	return nullptr;
}

static void PlayerLaunchWire_Flatten(const PlayerLaunchState& s, PlayerLaunchWire& w)
{
	w.m_activate = s.m_activate ? 1 : 0;
	w.m_avoidedMantle = 0;
	w.m_lock3pRotation = s.m_lock3pRotation ? 1 : 0;
	w.m_velocity[0] = s.m_velocity[0];
	w.m_velocity[1] = s.m_velocity[1];
	w.m_velocity[2] = s.m_velocity[2];
	// Client compares this against its own clock -- publish newest server time while arming.
	w.m_startTime = s.m_activate ? PlayerLaunch_CurTime() : s.m_startTime;
}

static void PlayerLaunch_Publish(void* pPlayer, const PlayerLaunchState& s)
{
	const uint64_t key = PlayerLaunchWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	if (!key)
		return;

	PlayerLaunchWire w;
	PlayerLaunchWire_Flatten(s, w);

	PlayerLaunchWireSlot* slot = PlayerLaunchWire_FindSlot(key);
	if (!slot)
	{
		for (PlayerLaunchWireSlot& cand : s_launchWireSlots)
		{
			if (cand.handleKey == 0)
			{
				slot = &cand;
				break;
			}
		}
		if (slot)
			InterlockedIncrement(&s_launchWireUsed);
		else
		{
			for (PlayerLaunchWireSlot& cand : s_launchWireSlots)
			{
				if (cand.handleKey != key
					&& cand.wire.m_activate == 0
					&& cand.wire.m_lock3pRotation == 0)
				{
					cand.handleKey = 0;
					cand.activateSamples = 0;
					slot = &cand;
					break;
				}
			}
			if (!slot)
			{
				const LONG idx = (InterlockedIncrement(&s_launchWireCursor) - 1) & 63;
				slot = &s_launchWireSlots[idx];
			}
		}
		slot->handleKey = 0;
		slot->activateSamples = 0;
	}

	if (w.m_activate && !s.m_applied)
		slot->activateSamples = 0;

	InterlockedIncrement(&slot->seq);
	slot->wire = w;
	InterlockedIncrement(&slot->seq);
	slot->handleKey = key;

	MarkEntityEdictDirty(pPlayer);

	if (sdk_player_launch_diag.GetBool())
	{
		static volatile LONG s_pubN = 0;
		const LONG n = InterlockedIncrement(&s_pubN);
		if (n <= 16)
			Warning(eDLL_T::SERVER,
				"[PLAYER-LAUNCH] publish #%d player=%p act=%d lock3p=%d vel=(%.1f %.1f %.1f) t=%.3f\n",
				static_cast<int>(n), pPlayer, w.m_activate, w.m_lock3pRotation,
				w.m_velocity[0], w.m_velocity[1], w.m_velocity[2], w.m_startTime);
	}
}

// The engine's own definition of on-ground: FullWalkMove and the landing
// handler at its tail both branch on whether this handle still resolves.
static bool PlayerLaunch_IsOnGround(const void* pPlayer)
{
	if (!pPlayer)
		return false;
	const uint32_t raw = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<uintptr_t>(pPlayer) + PL_OFF_GROUND_ENTITY);
	return SDKEntityState_Resolve(SDKEntityHandle(raw), ESide::Server) != nullptr;
}

static void PlayerLaunch_ResolveOffsetsOnce(void* pPlayer)
{
	if (s_launchOffsetsResolved)
		return;
	s_launchOffsetsResolved = true;

	const int sliding = DTExtend_FindNativePropOffset(pPlayer, "m_sliding");
	const int duck = DTExtend_FindNativePropOffset(pPlayer, "m_duckToggleOn");
	const int floor = DTExtend_FindNativePropOffset(pPlayer, "m_wallrunLatestFloorHeight");

	if (sliding == static_cast<int>(PL_OFF_SLIDING)
		&& duck > 0 && duck < 32000
		&& floor > 0 && floor < 32000)
	{
		s_offDuckToggleOn = duck;
		s_offWallrunFloorHeight = floor;
		return;
	}

	Warning(eDLL_T::SERVER,
		"[PLAYER-LAUNCH] prop offsets unresolved (m_sliding=%d) -- duck/floor-height parity skipped\n",
		sliding);
}

bool PlayerLaunch_GetWire(const void* pPlayer, PlayerLaunchWire* pOut)
{
	if (!pOut)
		return false;
	memset(pOut, 0, sizeof(*pOut));
	if (!pPlayer)
		return true;

	const uint64_t key = PlayerLaunchWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	PlayerLaunchWireSlot* const slot = PlayerLaunchWire_FindSlot(key);
	if (!slot)
		return true;

	for (int attempt = 0; attempt < 8; ++attempt)
	{
		const LONG before = slot->seq;
		if (before & 1)
			continue;
		const PlayerLaunchWire copy = slot->wire;
		MemoryBarrier();
		if (slot->seq == before && slot->handleKey == key)
		{
			*pOut = copy;
			return true;
		}
	}
	return true;
}

static LONG PlayerLaunch_ActivateSamples(const void* pPlayer)
{
	if (!pPlayer)
		return 0;
	const uint64_t key = PlayerLaunchWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	PlayerLaunchWireSlot* const slot = PlayerLaunchWire_FindSlot(key);
	if (!slot)
		return 0;
	return slot->activateSamples;
}

void PlayerLaunch_NoteActivateSampled(const void* pPlayer)
{
	if (!pPlayer)
		return;
	const uint64_t key = PlayerLaunchWire_PackHandle(SDKEntityState_GetHandle(pPlayer));
	PlayerLaunchWireSlot* const slot = PlayerLaunchWire_FindSlot(key);
	if (!slot || slot->wire.m_activate == 0)
		return;
	InterlockedIncrement(&slot->activateSamples);
}

void PlayerLaunch_LevelShutdown(void)
{
	InterlockedExchange(&s_launchWireUsed, 0);
	memset(s_launchWireSlots, 0, sizeof(s_launchWireSlots));
	InterlockedExchange(&s_launchWireCursor, 0);
}

bool PlayerLaunch_KeepsToss(const void* pPlayer)
{
	PlayerLaunchWire wire;
	if (!PlayerLaunch_GetWire(pPlayer, &wire))
		return false;
	return wire.m_activate != 0 || wire.m_lock3pRotation != 0;
}

void PlayerLaunch_BeginFullWalkMove(void* ctx)
{
	s_fullWalkCtx = ctx;
}

void PlayerLaunch_EndFullWalkMove(void)
{
	s_fullWalkCtx = nullptr;
}

void PlayerLaunch_Latch(void* pPlayer, float velX, float velY, float velZ, bool lock3pRotation)
{
	if (!pPlayer)
		return;

	PlayerLaunchState& s = s_launchMap[pPlayer];
	s.m_activate = true;
	s.m_lock3pRotation = lock3pRotation;
	s.m_applied = false;
	s.m_leftGround = false;
	s.m_velocity[0] = velX;
	s.m_velocity[1] = velY;
	s.m_velocity[2] = velZ;
	s.m_startTime = PlayerLaunch_CurTime();

	PlayerLaunch_Publish(pPlayer, s);

	if (sdk_player_launch_diag.GetBool())
		Warning(eDLL_T::SERVER,
			"[PLAYER-LAUNCH] latch player=%p vel=(%.1f %.1f %.1f) lock3p=%d t=%.3f\n",
			pPlayer, velX, velY, velZ, lock3pRotation ? 1 : 0, s.m_startTime);

	Translocation_BeginNoProjTossForPlayer(pPlayer);
}

void PlayerLaunch_ApplyFromMoveCtx(void* ctx)
{
	if (!ctx)
		return;

	void* const pPlayer = *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(ctx) + PL_CTX_OFF_PLAYER);
	void* const mv = *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(ctx) + PL_CTX_OFF_MOVEDATA);
	if (!pPlayer)
		return;

	PlayerLaunchState* const pState = s_launchMap.Find(pPlayer);
	if (!pState)
		return;
	PlayerLaunchState& s = *pState;

	if (s.m_activate)
	{
		if (!s.m_applied)
		{
			const float velX = s.m_velocity[0];
			const float velY = s.m_velocity[1];
			const float velZ = s.m_velocity[2];
			const uintptr_t base = reinterpret_cast<uintptr_t>(pPlayer);

			if (mv)
			{
				float* const pMvVel = reinterpret_cast<float*>(
					reinterpret_cast<uintptr_t>(mv) + PL_MV_OFF_VELOCITY);
				pMvVel[0] = velX;
				pMvVel[1] = velY;
				pMvVel[2] = velZ;
			}

			if (fmaxf(fabsf(velX), fabsf(velY)) > 0.01f && v_CBaseEntity__SetAbsAngles)
			{
				const float yaw = RAD2DEG(atan2f(velY, velX));
				const float angles[3] = { 0.0f, yaw, 0.0f };
				v_CBaseEntity__SetAbsAngles(pPlayer, angles);
			}

			if (v_CGameMovement__SetGroundEntity)
				v_CGameMovement__SetGroundEntity(ctx, nullptr);

			// Native SetGroundEntity clears the adjacent flag -- write after it.
			*reinterpret_cast<uint8_t*>(base + PL_OFF_HAS_JUMPED) = 1;

			// The client's m_landingType = 2 has no counterpart here: no dedi site
			// compares that field against 2, and writing it masks the live 1 a
			// jumppad or cannon launch leaves for the landing handler.

			PlayerLaunch_ResolveOffsetsOnce(pPlayer);

			if (s_offWallrunFloorHeight > 0)
			{
				const float originZ = *reinterpret_cast<const float*>(base + PL_OFF_ABS_ORIGIN + 8);
				*reinterpret_cast<float*>(base + s_offWallrunFloorHeight) = originZ;
			}

			if (v_CPlayer__ClearWallRun)
				v_CPlayer__ClearWallRun(pPlayer);

			*reinterpret_cast<uint8_t*>(base + PL_OFF_SLIDING) = 0;

			if (s_offDuckToggleOn > 0)
				*reinterpret_cast<uint8_t*>(base + s_offDuckToggleOn) = 0;

			s.m_applied = true;
			PlayerLaunch_Publish(pPlayer, s);

			if (sdk_player_launch_diag.GetBool())
				Warning(eDLL_T::SERVER, "[PLAYER-LAUNCH] apply player=%p landingType=2 pulse=1\n", pPlayer);
			return;
		}

		// Client only reaches launch state by decoding this bit from a snapshot.
		const float held = PlayerLaunch_CurTime() - s.m_startTime;
		if (PlayerLaunch_ActivateSamples(pPlayer) >= PL_ACTIVATE_MIN_SAMPLES
			|| held >= PL_ACTIVATE_MAX_HOLD)
		{
			s.m_activate = false;
			PlayerLaunch_Publish(pPlayer, s);

			if (sdk_player_launch_diag.GetBool())
				Warning(eDLL_T::SERVER,
					"[PLAYER-LAUNCH] activate cleared player=%p samples=%d held=%.3f\n",
					pPlayer, static_cast<int>(PlayerLaunch_ActivateSamples(pPlayer)), held);
		}
	}

	// The land path also tears down the no-projectile toss loop, so it must run
	// for a launch that asked for no rotation lock as well.
	if (!s.m_applied)
		return;

	const float elapsed = PlayerLaunch_CurTime() - s.m_startTime;
	if (elapsed < PL_LOCK_MAX_TIME)
	{
		if (!PlayerLaunch_IsOnGround(pPlayer))
		{
			s.m_leftGround = true;
			return;
		}
		if (!s.m_leftGround || elapsed < PL_LAND_MIN_TIME)
			return;
	}

	s.m_lock3pRotation = false;
	s.m_applied = false;
	PlayerLaunch_Publish(pPlayer, s);
	Translocation_EndNoProjTossForPlayer(pPlayer);

	if (sdk_player_launch_diag.GetBool())
		Warning(eDLL_T::SERVER, "[PLAYER-LAUNCH] land player=%p elapsed=%.3f\n", pPlayer, elapsed);
}

static __int64 __fastcall Hook_CGameMovement_CheckJumpButton(void* ctx)
{
	const __int64 ret = v_CGameMovement__CheckJumpButton
		? v_CGameMovement__CheckJumpButton(ctx)
		: 0;

	if (ctx && ctx == s_fullWalkCtx)
		PlayerLaunch_ApplyFromMoveCtx(ctx);

	return ret;
}

void VPlayerLaunch::GetAdr(void) const
{
	LogFunAdr("CGameMovement::CheckJumpButton", v_CGameMovement__CheckJumpButton);
	LogFunAdr("CGameMovement::SetGroundEntity", v_CGameMovement__SetGroundEntity);
	LogFunAdr("CPlayer::ClearWallRun", v_CPlayer__ClearWallRun);
	LogFunAdr("CBaseEntity::SetAbsAngles", v_CBaseEntity__SetAbsAngles);
}

void VPlayerLaunch::GetFun(void) const
{
	// CGameMovement::CheckJumpButton(ctx). Unique on this dedi (1 hit).
	Module_FindPattern(g_GameDll,
		"40 55 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 51")
		.GetPtr(v_CGameMovement__CheckJumpButton);

	if (!v_CGameMovement__CheckJumpButton)
		Warning(eDLL_T::SERVER,
			"[PLAYER-LAUNCH] CGameMovement::CheckJumpButton pattern unresolved -- apply stays parked\n");

	// CGameMovement::SetGroundEntity(ctx, pEnt). Unique on this dedi (1 hit).
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 56 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 44 0F 29 A0")
		.GetPtr(v_CGameMovement__SetGroundEntity);

	if (!v_CGameMovement__SetGroundEntity)
		Warning(eDLL_T::SERVER,
			"[PLAYER-LAUNCH] CGameMovement::SetGroundEntity pattern unresolved\n");

	// CPlayer::ClearWallRun(player). Unique on this dedi (1 hit).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 81 EC ?? ?? ?? ?? 0F 29 74 24 ?? 33 FF")
		.GetPtr(v_CPlayer__ClearWallRun);

	if (!v_CPlayer__ClearWallRun)
		Warning(eDLL_T::SERVER,
			"[PLAYER-LAUNCH] CPlayer::ClearWallRun pattern unresolved\n");

	// CBaseEntity::SetAbsAngles(entity, const float ang[3]). Unique on this dedi
	// (1 hit). Interior anchors are the m_angAbsRotation compares at +0x45C/+0x460
	// that follow CalcAbsolutePosition. NOT the script SetAbsAngles binding: that
	// one range-checks and then dispatches Teleport through vtable +0x438, which
	// relinks the entity and walks its children -- unusable mid-move.
	Module_FindPattern(g_GameDll,
		"40 55 53 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? "
		"F3 0F 10 07 0F 2E 83 5C 04 00 00 7A ?? 75 ?? F3 0F 10 47 04 0F 2E 83 60 04 00 00")
		.GetPtr(v_CBaseEntity__SetAbsAngles);

	if (!v_CBaseEntity__SetAbsAngles)
		Warning(eDLL_T::SERVER,
			"[PLAYER-LAUNCH] CBaseEntity::SetAbsAngles pattern unresolved\n");
}

void VPlayerLaunch::Detour(const bool bAttach) const
{
	if (v_CGameMovement__CheckJumpButton)
		DetourSetup(&v_CGameMovement__CheckJumpButton, &Hook_CGameMovement_CheckJumpButton, bAttach);
}

