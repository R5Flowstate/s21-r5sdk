//=============================================================================//
//
// Purpose: prediction twin of the dedi's jump pad launch state. See header.
//
// The pad and cannon scripts set m_slowMoEnabled in a server-only enter
// callback and clear it a frame after landing, and an air relaunch runs a
// server-only TouchGround; the client never predicted either. The dedi moves
// both onto the launch and landing commands, and this side applies the same
// rule from the same predicted fields, so a replayed command re-derives it.
//
//=============================================================================//
#include "core/stdafx.h"

#include "game/client/pred_authority.h"
#include "game/client/jumppad_predict.h"

// C_GameMovement ctx.
static constexpr ptrdiff_t JPP_CTX_OFF_PLAYER = 0x08;

// C_Player.
static constexpr ptrdiff_t JPP_OFF_FFLAGS           = 0xC8;   // int m_fFlags, bit 0 = on ground
static constexpr ptrdiff_t JPP_OFF_DEBOUNCE         = 0x350;  // float m_jumpPadDebounceExpireTime
static constexpr ptrdiff_t JPP_OFF_LANDINGTYPE      = 0x1D3C; // int m_landingType
static constexpr ptrdiff_t JPP_OFF_SLOWMO           = 0x2D74; // bool m_slowMoEnabled
static constexpr ptrdiff_t JPP_OFF_ZIP_COOLDOWN     = 0x2FA0; // float m_ziplineReattachCooldownTime
static constexpr ptrdiff_t JPP_OFF_ZIP_COOLDOWN_IDX = 0x2FA4; // int m_ziplineCooldownIndex
static constexpr ptrdiff_t JPP_OFF_GLIDE_GROUNDED   = 0x2FEC; // bool m_touchedGroundSinceLastGlide
static constexpr ptrdiff_t JPP_OFF_CURRENTCOMMAND   = 0x34B8; // C_UserCmd* m_pCurrentCommand
static constexpr ptrdiff_t JPP_OFF_LAST_GROUNDED    = 0x366C; // float m_flTimeLastTouchedGround, stamped before the move
static constexpr ptrdiff_t JPP_OFF_HAS_JUMPED       = 0x36A8; // bool m_bHasJumpedSinceTouchedGround

// C_UserCmd.
static constexpr ptrdiff_t JPP_CMD_OFF_BASE_TICK = 0x20C; // int baseSnapshotTickCount

// C_TriggerCylinderHeavy: m_nextLaunchTime, which the dedi sets on a pad to the
// time of the first snapshot that carried it.
static constexpr ptrdiff_t JPP_TRIG_OFF_LIVE_TIME = 0xA18;

static constexpr int JPP_FL_ONGROUND = 1;

// Same constants as the dedi: the launch's debounce window, the float error of
// (now + window) - window, and half a tick of slack on the whole-tick live time.
static constexpr float JPP_LAUNCH_DEBOUNCE_WINDOW = 1.0f;
static constexpr float JPP_RELAUNCH_GROUND_EPS = 0.01f;
static constexpr float JPP_LIVE_TICK_MARGIN = 0.5f;

// The entry, not a trampoline: a call passes through its detours (double-jump power).
static void (*C_Player__TouchGround)(void* pPlayer) = nullptr;

static void* s_pMovePlayer = nullptr;
static bool s_bMoveStartedAirborne = false;
static bool s_bMoveHookSeen = false;

static inline bool JumpPadPredict_OnGround(const uint8_t* pPlayer)
{
	return (*reinterpret_cast<const int*>(pPlayer + JPP_OFF_FFLAGS) & JPP_FL_ONGROUND) != 0;
}

void JumpPadPredict_OnLauncherLaunched(void* pPlayer)
{
	if (!pPlayer)
		return;

	static bool s_bWarnedNoMoveHook = false;
	if (!s_bMoveHookSeen && !s_bWarnedNoMoveHook)
	{
		s_bWarnedNoMoveHook = true;
		Warning(eDLL_T::CLIENT, "[JP-PRED] PlayerMove hook never ran -- landing cannot clear launch "
			"slow-mo, so every pad landing mispredicts\n");
	}

	*(static_cast<uint8_t*>(pPlayer) + JPP_OFF_SLOWMO) = 1;
}

void JumpPadPredict_OnPlayerMoveBegin(void* ctx)
{
	s_bMoveHookSeen = true;
	s_pMovePlayer = ctx ? *reinterpret_cast<void**>(static_cast<uint8_t*>(ctx) + JPP_CTX_OFF_PLAYER) : nullptr;
	s_bMoveStartedAirborne = s_pMovePlayer && !JumpPadPredict_OnGround(static_cast<uint8_t*>(s_pMovePlayer));
}

void JumpPadPredict_OnPlayerMoveEnd(void* ctx)
{
	uint8_t* const pPlayer = ctx ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + JPP_CTX_OFF_PLAYER) : nullptr;
	const bool bLanded = pPlayer && pPlayer == s_pMovePlayer && s_bMoveStartedAirborne && JumpPadPredict_OnGround(pPlayer);
	s_pMovePlayer = nullptr;

	if (bLanded)
		pPlayer[JPP_OFF_SLOWMO] = 0;
}

//-----------------------------------------------------------------------------
// Purpose: the dedi runs its TouchGround on a pad launch that follows the
// previous one without a ground touch in between. S21's TouchGround also writes
// fields the dedi one leaves alone; those keep their post-launch values.
//-----------------------------------------------------------------------------
static void JumpPadPredict_ResetAirStateOnRelaunch(uint8_t* pPlayer, const float flDebounceBefore, const float flLastGrounded)
{
	if (!C_Player__TouchGround || !(flDebounceBefore > 0.0f))
		return;

	if (flLastGrounded > flDebounceBefore - JPP_LAUNCH_DEBOUNCE_WINDOW + JPP_RELAUNCH_GROUND_EPS)
		return;

	const uint8_t bHasJumped = pPlayer[JPP_OFF_HAS_JUMPED];
	const int nLandingType = *reinterpret_cast<const int*>(pPlayer + JPP_OFF_LANDINGTYPE);
	const uint8_t bGlideGrounded = pPlayer[JPP_OFF_GLIDE_GROUNDED];
	const float flZipCooldown = *reinterpret_cast<const float*>(pPlayer + JPP_OFF_ZIP_COOLDOWN);
	const int nZipCooldownIdx = *reinterpret_cast<const int*>(pPlayer + JPP_OFF_ZIP_COOLDOWN_IDX);

	C_Player__TouchGround(pPlayer);

	pPlayer[JPP_OFF_HAS_JUMPED] = bHasJumped;
	*reinterpret_cast<int*>(pPlayer + JPP_OFF_LANDINGTYPE) = nLandingType;
	pPlayer[JPP_OFF_GLIDE_GROUNDED] = bGlideGrounded;
	*reinterpret_cast<float*>(pPlayer + JPP_OFF_ZIP_COOLDOWN) = flZipCooldown;
	*reinterpret_cast<int*>(pPlayer + JPP_OFF_ZIP_COOLDOWN_IDX) = nZipCooldownIdx;
}

//-----------------------------------------------------------------------------
// Purpose: a pad is live for a command whose base snapshot (the one its
// prediction interpolates from) is newer than the pad's first snapshot, so this
// side has had the pad, and touched it, for at least a snapshot interval.
//-----------------------------------------------------------------------------
static bool JumpPadPredict_IsLiveForCommand(const uint8_t* pPlayer, const void* pTrigger)
{
	if (!pTrigger)
		return true;

	const float flLiveTime = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(pTrigger) + JPP_TRIG_OFF_LIVE_TIME);
	const uint8_t* const pCmd = *reinterpret_cast<const uint8_t* const*>(pPlayer + JPP_OFF_CURRENTCOMMAND);
	const float flInterval = PredNative_TickInterval();
	if (!(flLiveTime > 0.0f) || !pCmd || !(flInterval > 0.0f))
		return true;

	return static_cast<float>(*reinterpret_cast<const int*>(pCmd + JPP_CMD_OFF_BASE_TICK)) > flLiveTime / flInterval + JPP_LIVE_TICK_MARGIN;
}

static char __fastcall Hook_C_GameMovement_JumpPadLaunch(void* ctx, void* pTrigger)
{
	uint8_t* const pPlayer = ctx ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + JPP_CTX_OFF_PLAYER) : nullptr;
	if (!pPlayer)
		return C_GameMovement__JumpPadLaunch(ctx, pTrigger);

	if (!JumpPadPredict_IsLiveForCommand(pPlayer, pTrigger))
		return 0;

	const float flDebounceBefore = *reinterpret_cast<const float*>(pPlayer + JPP_OFF_DEBOUNCE);
	const float flLastGrounded = *reinterpret_cast<const float*>(pPlayer + JPP_OFF_LAST_GROUNDED);

	const char result = C_GameMovement__JumpPadLaunch(ctx, pTrigger);

	if (*reinterpret_cast<const float*>(pPlayer + JPP_OFF_DEBOUNCE) == flDebounceBefore)
		return result;

	JumpPadPredict_ResetAirStateOnRelaunch(pPlayer, flDebounceBefore, flLastGrounded);
	JumpPadPredict_OnLauncherLaunched(pPlayer);
	return result;
}

void VJumpPadPredict::GetFun(void) const
{
	// Launch off one touched pad (trigger type 1 of the predicted trigger pass);
	// unique on its 0x2E0 frame and the ctx+8 player load.
	Module_FindPattern(g_GameDll,
		"48 89 74 24 18 4C 89 74 24 20 55 48 8D AC 24 20 FE FF FF 48 81 EC E0 02 00 00 48 8B 41 08 4C 8B F1")
		.GetPtr(C_GameMovement__JumpPadLaunch);

	// Ground touch (same resolve as the double-jump power twin).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 33 FF C6 81 A8 36 00 00 00 89 B9 FC 2A 00 00 48 8B D9 C6 81 EC 2F 00 00 01 E8")
		.GetPtr(C_Player__TouchGround);

	if (!C_GameMovement__JumpPadLaunch || !C_Player__TouchGround)
		Warning(eDLL_T::CLIENT,
			"[JP-PRED] pattern unresolved (launch=%p touchground=%p) -- pad launches mispredict\n",
			reinterpret_cast<void*>(C_GameMovement__JumpPadLaunch), reinterpret_cast<void*>(C_Player__TouchGround));
}

void VJumpPadPredict::Detour(const bool bAttach) const
{
	if (C_GameMovement__JumpPadLaunch)
		DetourSetup(&C_GameMovement__JumpPadLaunch, &Hook_C_GameMovement_JumpPadLaunch, bAttach);
}
