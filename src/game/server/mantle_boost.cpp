//=============================================================================//
//
// Purpose: mantle-exit boost on TraversalMove. Hook TraversalMove, not
// TraversalFinished -- its caller overwrites origin and exit velocity after
// return. Client twin must match ConVar defaults by hand.
//
//=============================================================================//
#include "core/stdafx.h"


#include "mantle_boost.h"
#include "player.h"
#include "baseentity.h"
#include "game/shared/in_buttons.h"
#include "mathlib/mathlib.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "public/edict.h"
#include "engine/server/vengineserver_impl.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/mantle_boost_curves.h"
#include "game/shared/scriptremotefunctions_shared.h"
#include "common/netmessages.h"
#include "engine/server/server.h"
#include "vscript_server.h"
#include "tier1/cmd.h"
#include <cmath>
#include <cstdlib>

// gpGlobals is defined in the game module; declare locally.
extern CGlobalVars* gpGlobals;

//-----------------------------------------------------------------------------
// TraversalMove/Jump layout offsets for S3 CGameMovement ctx, CMoveData, player.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t MB_CTX_OFF_PLAYER   = 8;    // CPlayer*
static constexpr ptrdiff_t MB_CTX_OFF_MOVEDATA = 16;   // CMoveData*

// m_vecAbsViewAngles at mv+0x0C on both engines; m_nButtons at 0x24.
static constexpr ptrdiff_t MB_MV_OFF_VIEWANGLES      = 12;   // QAngle m_vecAbsViewAngles
static constexpr ptrdiff_t MB_MV_OFF_BUTTONS_PRESSED = 40;   // m_nOldButtons
static constexpr ptrdiff_t MB_MV_OFF_BUTTONS_HELD    = 44;   // m_nButtonsPressed; Jump inject uses this
static constexpr ptrdiff_t MB_MV_OFF_BUTTONS_HELD_TRUE = 36; // m_nButtons held (mv+0x24); Duck uses this
static constexpr ptrdiff_t MB_MV_OFF_MOVEDIR_GRAVITY = 180;  // float[3] m_moveDirGravity
static constexpr ptrdiff_t MB_MV_OFF_VELOCITY        = 304;  // float[3] velocity (+304/308/312)

// CPlayer raw fields (no SDK mirror/accessor exists for these)
static constexpr ptrdiff_t MB_PLAYER_OFF_SKIP_TIMESTAMP   = 25116;  // anti-bhop skip-window timestamp (native Jump)
static constexpr ptrdiff_t MB_PLAYER_OFF_DANGLE_DISMOUNT  = 26388;  // dangle-dismount timestamp; ==0.0 for trigger
static constexpr ptrdiff_t MB_PLAYER_OFF_DANGLE_FORCEFALL = 26393;  // m_wallDangleForceFallOff analog; ==0 for trigger
static constexpr ptrdiff_t MB_PLAYER_OFF_TRAVERSAL_TYPE   = 26276;  // m_traversalType (0=mantle, nonzero=window vault)
static constexpr ptrdiff_t MB_PLAYER_OFF_TRAVERSAL_STATE  = 26280;  // m_traversalState (11=idle/hang, 12=jump-off)
static constexpr ptrdiff_t MB_PLAYER_OFF_MOVE_SCALE       = 24884;  // m_cachedMoveScale -- GetPoseSpeed_Sprint multiplier
static constexpr ptrdiff_t MB_PLAYER_OFF_MOVE_SPEED_SCALE = 24880;  // m_playerMoveSpeedScale
static constexpr ptrdiff_t MB_PLAYER_OFF_TIMELASTLANDED   = 25116;  // last-landing timestamp (same field as SKIP_TIMESTAMP)

//-----------------------------------------------------------------------------
// Engine function pointers.
//-----------------------------------------------------------------------------
static char (*v_CGameMovement__TraversalMove)(void* ctx, char justStarted) = nullptr;
static char (*v_CGameMovement__Jump)(void* ctx) = nullptr;
static char (*v_CGameMovement__Duck)(void* ctx) = nullptr;
static float (*v_CPlayer__GetPoseSpeed_Sprint)(CPlayer* player, int nPose) = nullptr;

//-----------------------------------------------------------------------------
// Tunables. Client twin must register the same names and defaults.
//-----------------------------------------------------------------------------
static ConVar mantle_boost_enabled("mantle_boost_enabled", "1", FCVAR_RELEASE,
	"[MANTLE-BOOST] Master enable for the mantle-exit boost bridge (S21 mantle_boost enable). "
	"Must match the client-side default.");
static ConVar bridge_mantle_boost_button_mask("bridge_mantle_boost_button_mask", "268435456", FCVAR_RELEASE,
	"[MANTLE-BOOST] Held-button bitmask when mantle_boost_input_setting=3 (Movement Ability). "
	"Default 268435456 (bit 28 / +dodge). 4 is remapped to bit 28.");
// Per-client preference; client twin is FCVAR_USERINFO. This ConVar is the fallback.
static ConVar mantle_boost_input_setting("mantle_boost_input_setting", "1", FCVAR_RELEASE,
	"[MANTLE-BOOST] Activation input FALLBACK default (per-client value comes from the client's "
	"FCVAR_USERINFO copy): 0=Off, 1=Jump (S21 default), 2=Crouch, 3=Movement Ability/custom "
	"(bridge_mantle_boost_button_mask). Must match the client-side default for lockstep prediction.");
static ConVar bridge_mantle_boost_sweet_spot_auto("bridge_mantle_boost_sweet_spot_auto", "1", FCVAR_RELEASE,
	"[MANTLE-BOOST] derive the sweet-spot threshold per traversal state from the baked curve so "
	"the window is the last min_valid_traversal_frac of the climb, as S21's is. 0 uses the "
	"authored bridge_mantle_boost_sweet_spot_angle. Must match the client-side default.");
static ConVar bridge_mantle_boost_sweet_spot_angle("bridge_mantle_boost_sweet_spot_angle", "2", FCVAR_RELEASE,
	"[MANTLE-BOOST] trigger: max |animViewPitch - eyePitch| (degrees) that still counts as the sweet spot. "
	"Must match the client-side default.");
static ConVar mantle_boost_require_increasing_view_angle("mantle_boost_require_increasing_view_angle", "0", FCVAR_RELEASE,
	"[MANTLE-BOOST] tweak (default 0): require |delta| still opening (moving away from zero). Must match client.");
static ConVar mantle_boost_require_decreasing_view_angle("mantle_boost_require_decreasing_view_angle", "1", FCVAR_RELEASE,
	"[MANTLE-BOOST] tweak (default 1): require |delta| past apex and settling toward zero, either sign. Must match client.");
static ConVar mantle_boost_min_valid_traversal_frac("mantle_boost_min_valid_traversal_frac", "0.5", FCVAR_RELEASE,
	"[MANTLE-BOOST] tweak (default 0.5): the traversal must have progressed past this fraction at the"
	"press for the superglide to be valid -- final decision-cascade step (at/below -> FAILED (3), above -> BOOST (4)). "
	"Must match the client-side default.");
static ConVar bridge_mantle_boost_exit_speed("bridge_mantle_boost_exit_speed", "200", FCVAR_RELEASE,
	"[MANTLE-BOOST] Base horizontal exit speed (u/s). State 4 multiplies by sprint_mult.");
static ConVar bridge_mantle_boost_sprint_mult("bridge_mantle_boost_sprint_mult", "1.5", FCVAR_RELEASE,
	"[MANTLE-BOOST] Exit-speed multiplier on full boost (state 4). S21 "
	"player_mantleBoostSprintSpeedMultiplier = 1.5 (29/30 legends; Sparrow is 1.0).");
static ConVar bridge_mantle_boost_jump_height("bridge_mantle_boost_jump_height", "90", FCVAR_RELEASE,
	"[MANTLE-BOOST] Jump height (u) for state-4 forced Jump (sqrt(2*gravity*height)). "
	"State 3 keeps player_jumpHeight (56).");
static ConVar bridge_mantle_boost_gravity("bridge_mantle_boost_gravity", "750", FCVAR_RELEASE,
	"[MANTLE-BOOST] Fallback gravity for the state-4 vz solve, used only if sv_gravity fails to resolve. "
	"Real Jump reads sv_gravity directly ( JumpHeightToVelocity) -- this is a safety net, not the primary source.");
static ConVar bridge_mantle_boost_apply_log("bridge_mantle_boost_apply_log", "0", FCVAR_DEVELOPMENTONLY,
	"[MB] log every boost finish apply: state, dir, pose, scale, speed, final velocity. Same name on the client.");
static ConVar bridge_mantle_boost_trig_log("bridge_mantle_boost_trig_log", "0", FCVAR_DEVELOPMENTONLY,
	"[MB] log each activation press-edge: travState, raw/anim frac, delta, latch. Same name on the client.");

//-----------------------------------------------------------------------------
// After a state-4 boost, suppress tap-strafe until the next traversal or landing.
//-----------------------------------------------------------------------------
static ConVar mantle_boost_disables_tap_strafes("mantle_boost_disables_tap_strafes", "1", FCVAR_RELEASE,
	"[MANTLE-BOOST] ship-named master enable (S21 default \"1\") for the post-boost lurch/tap-strafe "
	"restriction. Must match the client-side default for lockstep prediction.");

static int MantleBoost_MaskFromSetting(const int setting)
{
	switch (setting)
	{
	case 1:  return IN_JUMP;
	case 2:  return 0x04000004;   // IN_DUCK | controller duck-toggle
	case 3:
	{
		const int mask = bridge_mantle_boost_button_mask.GetInt();
		return mask == 4 ? 0x10000000 : mask;
	}
	default: return 0;
	}
}

// Protected CBaseEntity::m_entIndex; player entindex == client index.
class MB_PlayerEntIndexAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_entIndex;
};

//-----------------------------------------------------------------------------
// Per-player activation mask. Input setting is FCVAR_USERINFO; this ConVar is the fallback.
//-----------------------------------------------------------------------------
static int MantleBoost_GetClientConVarInt(const CPlayer* const player, const char* const pszName, const int nFallback)
{
	if (player && g_pEngineServer)
	{
		const int clientIndex = static_cast<const MB_PlayerEntIndexAccess*>(
			reinterpret_cast<const CBaseEntity*>(player))->m_entIndex;
		const char* const v = g_pEngineServer->GetClientConVarValue(clientIndex, pszName);
		if (v && v[0])
			return static_cast<int>(strtol(v, nullptr, 10));
	}
	return nFallback;
}

static int MantleBoost_GetInputSetting(const CPlayer* const player)
{
	return MantleBoost_GetClientConVarInt(player, "mantle_boost_input_setting",
		mantle_boost_input_setting.GetInt());
}

static int MantleBoost_ActivationButtonMask(const CPlayer* const player)
{
	const int setting = MantleBoost_GetInputSetting(player);
	if (setting == 3)
		return MantleBoost_GetClientConVarInt(player, "bridge_mantle_boost_button_mask",
			bridge_mantle_boost_button_mask.GetInt());
	return MantleBoost_MaskFromSetting(setting);
}

//-----------------------------------------------------------------------------
// Per-player FSM state, indexed by edict-1 (player slot = GetEdict-1).
//-----------------------------------------------------------------------------
struct MantleBoostSlot_t
{
	int             m_nState;                   // 0 idle, 1 INVALID (hang), 3 FAILED-armed, 4 BOOST-armed
	float           m_flBoostAppliedTime;       // curtime of the last state-4 finish apply (landing-reset anchor)
	float           m_flPrevDelta;              // previous evaluated tick's delta (apex bookkeeping)
	int             m_nTraversalSeq;            // climb counter, wraps at 16
	const char*     m_pszDecisionGate;
	float           m_flDecisionDelta;
	float           m_flDecisionAnim;
	int             m_nPublishedState;          // last ENCODED value written to the replicated prop (change gate)
};
static MantleBoostSlot_t s_mantleBoost[MAX_PLAYERS];

//-----------------------------------------------------------------------------
// Packed wire value: bits 0..2 state, 3..6 traversal seq, 7..13 edict. Change-gated.
//-----------------------------------------------------------------------------
static int MantleBoost_EncodeState(const CPlayer* const player, const MantleBoostSlot_t& s)
{
	const int nEdict = static_cast<int>(player->GetEdict());
	if (nEdict <= 0 || nEdict > 0x7F)
		return s.m_nState & 7;

	return (s.m_nState & 7) | ((s.m_nTraversalSeq & 15) << 3) | (nEdict << 7);
}

// The appended DT_Player prop reaches the client only in the instance baseline,
// so the verdict also rides the reliable S2C script-message lane to its owner.
static void MantleBoost_SendVerdict(CPlayer* const player, const int nEncoded)
{
	const int nSlot = static_cast<int>(player->GetEdict()) - 1;
	if (nSlot < 0 || nSlot >= MAX_PLAYERS || !g_pServer)
		return;

	CClient* const pClient = g_pServer->GetClient(nSlot);
	if (!pClient || !pClient->IsActive() || pClient->IsFakeClient())
		return;

	NET_ScriptMessage msg;
	msg.InitWrite();
	msg.m_bIsTyped = false;
	msg.m_DataOut.WriteLong(static_cast<int>(BRIDGE_S2C_MANTLEBOOST_MAGIC));
	msg.m_DataOut.WriteLong(nEncoded);
	pClient->SendNetMsgEx(&msg, false, true, false);

	static int s_nSent = 0;
	if (++s_nSent <= 8)
		Msg(eDLL_T::SERVER, "[MANTLE-BOOST] verdict sent slot=%d value=0x%04X (#%d)\n",
			nSlot, nEncoded, s_nSent);
}

static void MantleBoost_PublishState(CPlayer* const player, MantleBoostSlot_t& s)
{
	const int nEncoded = MantleBoost_EncodeState(player, s);
	if (nEncoded == s.m_nPublishedState)
		return;

	PlayerExtend_SetI32(player, offsetof(PlayerExtendWire, m_mantleBoostState), nEncoded);
	MarkEntityEdictDirty(player);
	s.m_nPublishedState = nEncoded;
	MantleBoost_SendVerdict(player, nEncoded);

	static int s_nPackStatLogs = 0;
	if (++s_nPackStatLogs <= 12)
	{
		uint32_t nReads = 0, nNoSlot = 0, nSeqFail = 0;
		PlayerExtend_GetPackReadStats(&nReads, &nNoSlot, &nSeqFail);
		Msg(eDLL_T::SERVER, "[MANTLE-BOOST] sidecar pack reads=%u noslot=%u seqfail=%u\n",
			nReads, nNoSlot, nSeqFail);
	}

	const int32_t nRead = PlayerExtend_GetI32(player, offsetof(PlayerExtendWire, m_mantleBoostState));
	if (nRead != nEncoded)
	{
		static bool s_bWarnedSidecar = false;
		if (!s_bWarnedSidecar)
		{
			s_bWarnedSidecar = true;
			const uint32_t nHandle = SDKEntityState_GetHandle(player).Raw();
			Warning(eDLL_T::SERVER, "[MANTLE-BOOST] sidecar write did not round-trip "
				"(wrote %d read %d handle=0x%08X)\n", nEncoded, nRead, nHandle);
		}
	}
}

static void MantleBoost_FireCallback(CPlayer* const player)
{
	if (!g_pServerScript)
		return;

	const HSCRIPT hPlayerScript = player->GetScriptInstance();
	if (!hPlayerScript)
		return;

	const HSCRIPT hFunc = g_pServerScript->FindFunction("CodeCallback_OnPlayerMantleBoosted", nullptr, nullptr);
	if (!hFunc)
	{
		static bool s_bWarnedMissingCallback = false;
		if (!s_bWarnedMissingCallback)
		{
			s_bWarnedMissingCallback = true;
			Warning(eDLL_T::SERVER,
				"[MANTLE-BOOST] CodeCallback_OnPlayerMantleBoosted not found in server VM -- "
				"boost still applies, script callback skipped\n");
		}
		return;
	}

	ScriptVariant_t args[1];
	args[0] = hPlayerScript;
	g_pServerScript->ExecuteFunction(hFunc, args, 1, nullptr, nullptr);
}

//-----------------------------------------------------------------------------
// Anim-camera pitch delta from the baked curve vs eye angles. False if degenerate or unbaked.
//-----------------------------------------------------------------------------
static bool MantleBoost_ComputeAnimCameraDelta(CPlayer* const player, const QAngle& eyeAngles,
	float* const pflDelta)
{
	const Vector3D& vecFwd = player->Diag_TraversalForwardDir();
	if (vecFwd.LengthSqr() < 1e-6f)
		return false;

	const int nTravState = *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_TRAVERSAL_STATE);
	const float flCycle = player->Diag_TraversalProgress();

	if (!MBCurves_Has(nTravState))
	{
		static uint16_t s_nWarnedStates = 0;
		if (nTravState >= 0 && nTravState < MB_CURVE_TRAVERSAL_COUNT
			&& !(s_nWarnedStates & (1u << nTravState)))
		{
			s_nWarnedStates |= uint16_t(1u << nTravState);
			Warning(eDLL_T::SERVER, "[MANTLE-BOOST] no baked curve for travState=%d -- "
				"presses latch the weak tier until %s carries it\n",
				nTravState, MBCurves_FilePath());
		}
		return false;
	}

	return MBCurves_Eval(nTravState, flCycle, vecFwd, eyeAngles, pflDelta);
}

static float MantleBoost_SweetSpotThreshold(CPlayer* const player, const QAngle& eyeAngles)
{
	float flThreshold = bridge_mantle_boost_sweet_spot_angle.GetFloat();
	if (!bridge_mantle_boost_sweet_spot_auto.GetBool())
		return flThreshold;

	const int nTravState = *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_TRAVERSAL_STATE);

	float flDerived = 0.0f;
	if (MBCurves_AutoThreshold(nTravState, mantle_boost_min_valid_traversal_frac.GetFloat(),
			player->Diag_TraversalForwardDir(), eyeAngles, &flDerived)
		&& flDerived > flThreshold)
	{
		flThreshold = flDerived;
	}
	return flThreshold;
}

//-----------------------------------------------------------------------------
// Dev iteration on a fresh bake without a dedi restart.
//-----------------------------------------------------------------------------
static void CC_MantleBoostCurvesReload(const CCommand& args)
{
	MBCurves_Reload();
	int nBaked = 0;
	for (int t = 0; t < MB_CURVE_TRAVERSAL_COUNT; ++t)
	{
		if (MBCurves_Has(t))
			++nBaked;
	}
	Msg(eDLL_T::SERVER, "[MANTLE-BOOST] curves reloaded: %d traversal states baked\n", nBaked);
}
static ConCommand mantle_boost_curves_reload("mantle_boost_curves_reload",
	CC_MantleBoostCurvesReload, "Reload platform/cfg/mantle_boost_curves.txt.",
	FCVAR_DEVELOPMENTONLY);

//-----------------------------------------------------------------------------
// Latch 3 or 4 on the first activation press while dangle gates are clear.
//-----------------------------------------------------------------------------
static void MantleBoost_EvaluateTrigger(CPlayer* const player, const uint8_t* const mv,
	MantleBoostSlot_t& s, const int slot)
{
	// Eye angles from mv, not the usercmd -- same field and offset as the client twin.
	const float* const pflEyeAngles = reinterpret_cast<const float*>(mv + MB_MV_OFF_VIEWANGLES);
	if (!isfinite(pflEyeAngles[0]) || !isfinite(pflEyeAngles[1]))
		return;
	if (pflEyeAngles[0] < -90.1f || pflEyeAngles[0] > 90.1f)
		return;

	const QAngle eyeAngles(pflEyeAngles[0], pflEyeAngles[1], pflEyeAngles[2]);

	// Press edge, mantle type 0, dangle clear. mv+40 is m_nButtonsPressed despite MB_MV_OFF_BUTTONS_HELD.
	const bool bButtonPressed = (*reinterpret_cast<const uint32_t*>(mv + MB_MV_OFF_BUTTONS_HELD)
		& MantleBoost_ActivationButtonMask(player)) != 0;
	const bool bMantleType = (*reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_TRAVERSAL_TYPE) == 0);
	const bool bDangleClear =
		(*reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_DANGLE_FORCEFALL) == 0) &&
		(*reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_DANGLE_DISMOUNT) == 0.0f);

	// Unbaked/degenerate delta still latches the weak tier -- never left pending.
	float flLiveDelta = 0.0f;
	const bool bHaveDelta = MantleBoost_ComputeAnimCameraDelta(player, eyeAngles, &flLiveDelta);

	const float flAnim = player->Diag_TraversalAnimProgress();
	if (bButtonPressed && bMantleType && bDangleClear)
	{
		// Decreasing = negative lobe rising toward zero; increasing = positive falling. No epsilon.
		const bool bDecreasingOk = flLiveDelta <= 0.0f && flLiveDelta >= s.m_flPrevDelta;
		const bool bIncreasingOk = flLiveDelta >= 0.0f && s.m_flPrevDelta >= flLiveDelta;
		const char* pszGate = "ok";

		if (!bHaveDelta)
		{
			s.m_nState = 3;
			pszGate = "nocurve";
		}
		else if (!bDecreasingOk && mantle_boost_require_increasing_view_angle.GetBool())
		{
			s.m_nState = 3;
			pszGate = "open";
		}
		else if (!bIncreasingOk && mantle_boost_require_decreasing_view_angle.GetBool())
		{
			s.m_nState = 3;
			pszGate = "dec";
		}
		else if (fabsf(flLiveDelta) >= MantleBoost_SweetSpotThreshold(player, eyeAngles))
		{
			s.m_nState = 3;
			pszGate = "angle";
		}
		else if (flAnim <= mantle_boost_min_valid_traversal_frac.GetFloat())
		{
			s.m_nState = 3;
			pszGate = "frac";
		}
		else
			s.m_nState = 4;

		s.m_pszDecisionGate = pszGate;
		s.m_flDecisionDelta = flLiveDelta;
		s.m_flDecisionAnim  = flAnim;

		if (bridge_mantle_boost_trig_log.GetBool())
		{
			const float flRaw = player->Diag_TraversalProgress();
			const int nTravState = *reinterpret_cast<const int*>(
				reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_TRAVERSAL_STATE);
			Msg(eDLL_T::SERVER, "[MB-TRIG] t=%.3f travState=%d raw=%.3f anim=%.3f delta=%.2f prev=%.2f thr=%.2f gate=%s -> %d slot=%d\n",
				gpGlobals ? gpGlobals->curTime : 0.0f, nTravState, flRaw, flAnim,
				flLiveDelta, s.m_flPrevDelta, MantleBoost_SweetSpotThreshold(player, eyeAngles),
				pszGate, s.m_nState, slot);
		}
	}

	if (bHaveDelta)
		s.m_flPrevDelta = flLiveDelta;
}

//-----------------------------------------------------------------------------
// Finish apply: overwrite exit velocity, then one native Jump or Duck, never both.
//-----------------------------------------------------------------------------
static float MantleBoost_Gravity(void)
{
	static ConVar* s_pGravity = nullptr;
	if (!s_pGravity && g_pCVar)
		s_pGravity = g_pCVar->FindVar("sv_gravity");
	const float flGravity = s_pGravity ? s_pGravity->GetFloat() : bridge_mantle_boost_gravity.GetFloat();
	return (isfinite(flGravity) && flGravity > 0.0f) ? flGravity : bridge_mantle_boost_gravity.GetFloat();
}

static void MantleBoost_ApplyBoost(void* const ctx, CPlayer* const player,
	MantleBoostSlot_t& s, const int slot)
{
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(ctx) + MB_CTX_OFF_MOVEDATA);
	if (!mv)
		return;

	// Direction: mv gravity-space move dir, else traversal forward, then reflect over the ledge.
	const float* const pMoveDirGravity = reinterpret_cast<const float*>(mv + MB_MV_OFF_MOVEDIR_GRAVITY);
	Vector3D vecDir(pMoveDirGravity[0], pMoveDirGravity[1], pMoveDirGravity[2]);
	const Vector3D vecFwd = player->Diag_TraversalForwardDir();
	if (fmaxf(fmaxf(fabsf(vecDir.x), fabsf(vecDir.y)), fabsf(vecDir.z)) <= 0.0099999998f)
		vecDir = vecFwd;
	const float flDot = vecDir.x * vecFwd.x + vecDir.y * vecFwd.y + vecDir.z * vecFwd.z;
	if (flDot < 0.0f && isfinite(flDot))
	{
		vecDir.x -= 2.0f * flDot * vecFwd.x;
		vecDir.y -= 2.0f * flDot * vecFwd.y;
		vecDir.z -= 2.0f * flDot * vecFwd.z;
	}

	const float flDirAbs = fmaxf(fmaxf(fabsf(vecDir.x), fabsf(vecDir.y)), fabsf(vecDir.z));
	const bool bHaveDir = (flDirAbs > 0.0099999998f) && isfinite(flDirAbs);

	float flPose = 0.0f;
	float flScale = 1.0f;
	float flSpeed = bridge_mantle_boost_exit_speed.GetFloat();
	if (v_CPlayer__GetPoseSpeed_Sprint)
	{
		flPose = v_CPlayer__GetPoseSpeed_Sprint(player, 0);   // PLAYERPOSE_STANDING
		if (isfinite(flPose) && flPose > 0.0f && flPose < 2000.0f)
		{
			flScale = *reinterpret_cast<const float*>(
				reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_MOVE_SCALE);
			flSpeed = flPose;
		}
	}
	if (s.m_nState == 4)
		flSpeed *= bridge_mantle_boost_sprint_mult.GetFloat();

	if (bHaveDir)
	{
		// Overwrite the native plain exit-velocity write.
		float* const pVelocity = reinterpret_cast<float*>(mv + MB_MV_OFF_VELOCITY);
		pVelocity[0] = vecDir.x * flSpeed;
		pVelocity[1] = vecDir.y * flSpeed;
		pVelocity[2] = vecDir.z * flSpeed;
	}
	else
	{
		// Both moveDir and traversalForwardDir degenerate -- keep the native
		// exit velocity; the forced jump/slide grant below still applies.
		Warning(eDLL_T::SERVER, "[MB] no usable boost direction at finish (slot %d) -- native exit velocity kept\n", slot);
	}

	if (s.m_nState == 4)
		player->MantleBoost_StampSlideTime(gpGlobals ? gpGlobals->curTime : 0.0f);

	if (s.m_nState == 4)
	{
		MantleBoost_FireCallback(player);

		s.m_flBoostAppliedTime = gpGlobals ? gpGlobals->curTime : 0.0f;
	}

	const int  nSetting    = MantleBoost_GetInputSetting(player);
	const bool bCrouchMode = (nSetting == 2);

	bool bJumped = false;
	if ((s.m_nState == 4 || !bCrouchMode) && v_CGameMovement__Jump)
	{
		uint32_t* const pHeld    = reinterpret_cast<uint32_t*>(mv + MB_MV_OFF_BUTTONS_HELD);
		uint32_t* const pPressed = reinterpret_cast<uint32_t*>(mv + MB_MV_OFF_BUTTONS_PRESSED);
		const uint32_t nHeldSave    = *pHeld;
		const uint32_t nPressedSave = *pPressed;
		*pHeld    |= IN_JUMP;
		*pPressed |= IN_JUMP;

		// Never write the ctx settings block -- per-class shared data, heap corruption.
		// Push skip-window timestamp far into the past so Jump's anti-bhop gate passes.
		float* const pflSkipStamp = reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_SKIP_TIMESTAMP);
		const float flSkipSave = *pflSkipStamp;
		*pflSkipStamp = -100000.0f;

		float* const vel = reinterpret_cast<float*>(mv + MB_MV_OFF_VELOCITY);
		const float velBefore[3] = { vel[0], vel[1], vel[2] };

		v_CGameMovement__Jump(ctx);
		bJumped = true;

		// Recompose vz as sqrt(2*g*h) so both engines add the same number.
		const float flHeight = (s.m_nState == 4)
			? bridge_mantle_boost_jump_height.GetFloat()
			: 56.0f;   // player_jumpHeight authored default (state 3)
		vel[0] = velBefore[0];
		vel[1] = velBefore[1];
		vel[2] = velBefore[2] + sqrtf(2.0f * MantleBoost_Gravity() * flHeight);

		*pflSkipStamp = flSkipSave;
		*pPressed = nPressedSave;
		*pHeld    = nHeldSave;
	}
	else if (s.m_nState == 3 && bCrouchMode)
	{
		if (v_CGameMovement__Duck)
		{
			uint32_t* const pHeldTrue = reinterpret_cast<uint32_t*>(mv + MB_MV_OFF_BUTTONS_HELD_TRUE);
			const uint32_t nHeldTrueSave = *pHeldTrue;
			__try
			{
				*pHeldTrue |= IN_DUCK;
				v_CGameMovement__Duck(ctx);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				Warning(eDLL_T::SERVER, "[MANTLE-BOOST] CGameMovement::Duck faulted -- crouch-mode failed-attempt duck skipped\n");
			}
			*pHeldTrue = nHeldTrueSave;
		}
		else
		{
			static bool s_bWarnedNoDuck = false;
			if (!s_bWarnedNoDuck)
			{
				s_bWarnedNoDuck = true;
				Warning(eDLL_T::SERVER, "[MANTLE-BOOST] CGameMovement::Duck unavailable -- "
					"crouch-mode failed attempts get no native reaction (neither jump nor duck)\n");
			}
		}
	}

	if (s.m_nState == 4)
		player->MantleBoost_GrantSlide();

	if (bridge_mantle_boost_trig_log.GetBool())
		Msg(eDLL_T::SERVER, "[MB-CLIMB] side=ds climb=%d pred=%d auth=%d apply=%d "
			"gate=%s delta=%.2f src=recon anim=%.3f speed=%.1f jump=%d t=%.3f slot=%d\n",
			s.m_nTraversalSeq, s.m_nState, s.m_nState, s.m_nState,
			s.m_pszDecisionGate ? s.m_pszDecisionGate : "none",
			s.m_flDecisionDelta, s.m_flDecisionAnim, flSpeed, bJumped ? 1 : 0,
			gpGlobals ? gpGlobals->curTime : 0.0f, slot);

	if (bridge_mantle_boost_apply_log.GetBool())
	{
		const float* const v = reinterpret_cast<const float*>(mv + MB_MV_OFF_VELOCITY);
		const float flMoveSpeedScale = *reinterpret_cast<const float*>(
			reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_MOVE_SPEED_SCALE);
		Msg(eDLL_T::SERVER, "[MB-APPLY] t=%.3f state=%d dir=(%.3f %.3f %.3f) pose=%.1f scale=%.3f mss=%.3f speed=%.1f vel=(%.1f %.1f %.1f) slot=%d\n",
			gpGlobals ? gpGlobals->curTime : 0.0f, s.m_nState,
			vecDir.x, vecDir.y, vecDir.z, flPose, flScale, flMoveSpeedScale, flSpeed, v[0], v[1], v[2], slot);
	}

	// Loud first fire, once per process.
	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		Warning(eDLL_T::SERVER, "[MANTLE-BOOST] FIRST FIRE -- state=%d dir=(%.2f %.2f %.2f) pose=%.1f scale=%.3f mss=%.3f speed=%.1f jump=%d slot=%d\n",
			s.m_nState, vecDir.x, vecDir.y, vecDir.z, flPose, flScale,
			*reinterpret_cast<const float*>(
				reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_MOVE_SPEED_SCALE),
			flSpeed, bJumped ? 1 : 0, slot);
	}
}

static char Hook_CGameMovement_TraversalMove(void* ctx, char justStarted)
{
	if (!mantle_boost_enabled.GetBool() || !ctx)
		return v_CGameMovement__TraversalMove(ctx, justStarted);

	CPlayer* const player = *reinterpret_cast<CPlayer**>(
		reinterpret_cast<uintptr_t>(ctx) + MB_CTX_OFF_PLAYER);
	if (!player)
		return v_CGameMovement__TraversalMove(ctx, justStarted);

	const int slot = static_cast<int>(player->GetEdict()) - 1;
	if (slot < 0 || slot >= MAX_PLAYERS)
		return v_CGameMovement__TraversalMove(ctx, justStarted);

	MantleBoostSlot_t& s = s_mantleBoost[slot];

	// The engine's entry call is the climb boundary. m_traversalStartTime is not:
	// the clock correction re-stamps it mid-climb, which would open a new sequence
	// and wipe a latched decision.
	if (justStarted)
	{
		// Keep m_nPublishedState: wiping it would make a reset-to-0 look unchanged.
		const int nKeepPublished = s.m_nPublishedState;
		const int nNextSeq = (s.m_nTraversalSeq + 1) & 15;
		s = MantleBoostSlot_t{};
		s.m_nPublishedState = nKeepPublished;
		s.m_nTraversalSeq = nNextSeq;
	}

	const float flProgressBefore = player->Diag_TraversalProgress();
	const float flJumpOffBefore = *reinterpret_cast<const float*>(
		reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_DANGLE_DISMOUNT);   // m_wallDangleJumpOffTime analog, pre-orig

	uint8_t* const mvTrig = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(ctx) + MB_CTX_OFF_MOVEDATA);
	if (!justStarted && mvTrig && s.m_nState == 0)
		MantleBoost_EvaluateTrigger(player, mvTrig, s, slot);

	const char ret = v_CGameMovement__TraversalMove(ctx, justStarted);

	// Armed 3/4: clear a native jump-off stamp so auto-dismount cannot abort the pull-through.
	if (s.m_nState >= 3 && flJumpOffBefore == 0.0f)
	{
		float* const pJumpOff = reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_DANGLE_DISMOUNT);
		if (*pJumpOff != 0.0f)
			*pJumpOff = 0.0f;
	}

	if (s.m_nState >= 3 && flProgressBefore < 1.0f && player->Diag_TraversalProgress() == 1.0f)
		MantleBoost_ApplyBoost(ctx, player, s, slot);

	// One publish point per tick, after every transition this tick has settled.
	MantleBoost_PublishState(player, s);

	return ret;
}

bool MantleBoost_ShouldSuppressTapStrafe(const CPlayer* const player)
{
	if (!mantle_boost_enabled.GetBool() || !mantle_boost_disables_tap_strafes.GetBool() || !player)
		return false;

	const int slot = static_cast<int>(player->GetEdict()) - 1;
	if (slot < 0 || slot >= MAX_PLAYERS)
		return false;

	MantleBoostSlot_t& s = s_mantleBoost[slot];

	// Clear state 4 when last-landing time advances past the boost-apply stamp (`>` so finish-tick equal stays restricted).
	if (s.m_nState == 4)
	{
		const float flLanded = *reinterpret_cast<const float*>(
			reinterpret_cast<uintptr_t>(player) + MB_PLAYER_OFF_TIMELASTLANDED);
		if (flLanded > s.m_flBoostAppliedTime)
		{
			s.m_nState = 0;   // S21 TouchGround clear
			MantleBoost_PublishState(const_cast<CPlayer*>(player), s);
		}
	}

	return s.m_nState == 4;
}

void VMantleBoostBridge::GetAdr(void) const
{
	LogFunAdr("CGameMovement::TraversalMove", v_CGameMovement__TraversalMove);
	LogFunAdr("CGameMovement::Jump", v_CGameMovement__Jump);
	LogFunAdr("CGameMovement::Duck", v_CGameMovement__Duck);
	LogFunAdr("CPlayer::GetPoseSpeed_Sprint", v_CPlayer__GetPoseSpeed_Sprint);
}

void VMantleBoostBridge::GetFun(void) const
{
	Module_FindPattern(g_GameDll, "48 8B C4 88 50 ?? 55")
		.GetPtr(v_CGameMovement__TraversalMove);

	Module_FindPattern(g_GameDll, "40 55 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 51")
		.GetPtr(v_CGameMovement__Jump);

	Module_FindPattern(g_GameDll, "48 8B C4 57 48 81 EC ? ? ? ? 4C 8B 41")
		.GetPtr(v_CGameMovement__Duck);

	// Whole-body pattern; sprintspeed field offset separates it from the Normal twin.
	Module_FindPattern(g_GameDll,
		"F3 0F 10 81 34 61 00 00 4C 8B C1 8B 0D 9B 2B 79 01 85 C9 79 2D "
		"4D 8B 80 08 5F 00 00 8B C1 0F BA F0 1F 42 8B 4C 00 04 8B 05 87 2B 79 01 "
		"49 03 C8 0F AF C2 48 03 C1 8B 0D 24 0E 79 01 F3 0F 59 04 01 C3")
		.GetPtr(v_CPlayer__GetPoseSpeed_Sprint);
	if (!v_CPlayer__GetPoseSpeed_Sprint)
		Warning(eDLL_T::SERVER, "[MANTLE-BOOST] GetPoseSpeed_Sprint pattern unresolved -- "
			"boost exit speed falls back to bridge_mantle_boost_exit_speed\n");

	if (!v_CGameMovement__TraversalMove)
		Warning(eDLL_T::SERVER, "[MANTLE-BOOST] CGameMovement::TraversalMove pattern unresolved -- mantle boost disabled\n");
	if (!v_CGameMovement__Jump)
		Warning(eDLL_T::SERVER, "[MANTLE-BOOST] CGameMovement::Jump pattern unresolved -- forced boost jump disabled\n");
	if (!v_CGameMovement__Duck)
		Warning(eDLL_T::SERVER, "[MANTLE-BOOST] CGameMovement::Duck pattern unresolved -- crouch-mode failed-attempt duck disabled\n");
}

void VMantleBoostBridge::Detour(const bool bAttach) const
{
	if (v_CGameMovement__TraversalMove)
		DetourSetup(&v_CGameMovement__TraversalMove, &Hook_CGameMovement_TraversalMove, bAttach);
}

