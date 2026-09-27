//=============================================================================//
//
// Purpose: S21 script-activated glide on the dedicated server -- engage,
// flight model, script natives and code callbacks the S21 client predicts.
//
// The stock server half carries an older glide that engages on a held jump for
// every glideEnabled setfile and reads fields the S21 layout does not have.
// Its check and flight functions are replaced; the meter update is shared
// with the jetpack (jetpack.cpp).
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "glide.h"
#include "jetpack.h"
#include "baseentity.h"
#include "engine/server/snapshot_diag.h"
#include "engine/host_state.h"
#include "game/shared/usercmd.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/glide_tuning.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "public/tier0/memory_patch.h"
#include <cfloat>
#include <cmath>
#include <cstring>
#include <initializer_list>

extern CGlobalVars* gpGlobals;

// Server CPlayer layout.
static constexpr ptrdiff_t GL_OFF_FLAGS           = 0x234;  // m_fFlags
static constexpr ptrdiff_t GL_OFF_MOVETYPE        = 0x308;
static constexpr ptrdiff_t GL_OFF_LIFESTATE       = 0x499;
static constexpr ptrdiff_t GL_OFF_ACTIVEWEAP_ALT  = 0x16D0; // m_inventory.activeWeapons[1]
static constexpr ptrdiff_t GL_OFF_SUPERJUMPSUSED  = 0x5AB4; // m_Local.m_superJumpsUsed
static constexpr ptrdiff_t GL_OFF_JUMPPRESSTIME   = 0x5ABC; // m_Local.m_jumpPressTime
static constexpr ptrdiff_t GL_OFF_SETTINGS        = 0x5F08;
static constexpr ptrdiff_t GL_OFF_BUTTONS         = 0x60DC; // m_nButtons
static constexpr ptrdiff_t GL_OFF_BUTTONSPRESSED  = 0x60E0; // m_afButtonPressed
static constexpr ptrdiff_t GL_OFF_FORWARDMOVE     = 0x60F4;
static constexpr ptrdiff_t GL_OFF_SIDEMOVE        = 0x60F8;
static constexpr ptrdiff_t GL_OFF_TOUCHEDGROUND   = 0x6214; // m_flTimeLastTouchedGround
static constexpr ptrdiff_t GL_OFF_LASTJUMPED      = 0x6218; // m_flTimeLastJumped
static constexpr ptrdiff_t GL_OFF_GRAPPLEATTACH   = 0x6780;
static constexpr ptrdiff_t GL_OFF_GRAPPLEACTIVE   = 0x67B8;
static constexpr ptrdiff_t GL_OFF_ZIPDETACHTIME   = 0x67DC; // m_lastZiplineDetachTime
static constexpr ptrdiff_t GL_OFF_BOOSTING        = 0x6955;
static constexpr ptrdiff_t GL_OFF_REPEATEDBOOST   = 0x6957;
static constexpr ptrdiff_t GL_OFF_JETPACK         = 0x695C;
static constexpr ptrdiff_t GL_OFF_GLIDING         = 0x695F;
static constexpr ptrdiff_t GL_OFF_GLIDEMETER      = 0x6960;
static constexpr ptrdiff_t GL_OFF_RECHARGEACCUM   = 0x6964;
static constexpr ptrdiff_t GL_OFF_HOVERING        = 0x6968;

static constexpr int      GL_FL_ONGROUND     = 0x1;
static constexpr int      GL_MOVETYPE_WALK   = 2;
static constexpr int      GL_IN_JUMP         = 0x00000002;
static constexpr uint32_t GL_INVALID_HANDLE  = 0xFFFFFFFFu;
static constexpr float    GL_LN2             = 0.69314718f;
// Minimum spacing the S21 client enforces between two jump presses and after a zipline detach.
static constexpr float    GL_MIN_INTERVAL    = 0.1f;
static constexpr uint32_t GL_SETTINGS_OFF_CAP = 0x10000u;
static constexpr uint32_t GL_FIELD_MISSING    = 0xFFFFFFFFu;

// Returned by CanUseGlide, same values as the S21 client.
enum GlideEngage_e
{
	GLIDE_ENGAGE_SUCCEED        = 0,
	GLIDE_ENGAGE_OUT_OF_METER   = 1,
	GLIDE_ENGAGE_FAILED_OTHER   = 2
};

static ConVar bridge_glide("bridge_glide", "1", FCVAR_RELEASE,
	"Run the S21 glide (script activation, flight model, CanUseGlide / SetActivateGlide, "
	"glide start/stop callbacks) on the server. 0 = stock glide.");

static ConVar bridge_glide_diag("bridge_glide_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[GLIDE] start/stop/verdict lines; 2+ also prints the flight state every N glide ticks (0 = off).");

enum GlideField_e
{
	GLF_ENABLED,
	GLF_SCRIPT_TO_ACTIVATE,
	GLF_HOLD_INPUT_ACTIVATION,
	GLF_TIME_TO_HOLD_INPUT,
	GLF_JETPACK_ENABLED,
	GLF_BOOST_ON_GROUND_SAFETY,
	GLF_BOOST_REPEATED_SHORT,
	GLF_DOUBLE_JUMP,
	GLF_SUPERJUMP_LIMIT,
	GLF_THRUST,
	GLF_TIGHTNESS_UP,
	GLF_TIGHTNESS_DOWN,
	GLF_UPBOOST_ENABLED,
	GLF_UPBOOST_DURATION,
	GLF_UPBOOST_FINISH_VELOCITY,
	GLF_UPBOOST_FALL_SPEED,
	GLF_UPBOOST_ONCE_PER_GLIDE,
	GLF_STRAFE_FORWARD,
	GLF_STRAFE_SIDEWAYS,
	GLF_STRAFE_FORWARD_TAPER_START,
	GLF_STRAFE_FORWARD_TAPER_FINISH,
	GLF_STRAFE_SIDEWAYS_TAPER_START,
	GLF_STRAFE_SIDEWAYS_TAPER_FINISH,
	GLF_MAX_SPEED,
	GLF_DECEL_TO_MAX,
	GLF_JETPACK_SCRIPT_TO_ACTIVATE,
	GLF_DURATION,
	GLF_COUNT
};

static const char* const s_glideFieldNames[GLF_COUNT] =
{
	"glideEnabled",
	"glideScriptToActivate",
	"glideHoldInputActivation",
	"glideTimeToHoldInputForActivation",
	"jetpackEnabled",
	"boostOnGroundSafety",
	"boostRepeatedBoostsAreShort",
	"doubleJump",
	"superjumpLimit",
	"glideThrust",
	"glideTightnessUp",
	"glideTightnessDown",
	"glideUpwardsBoostEnabled",
	"glideUpwardsBoostDuration",
	"glideUpwardsBoostFinishVelocity",
	"glideUpwardsBoostFallActivationSpeed",
	"glideUpwardsBoostActivateOncePerGlide",
	"glideStrafeForward",
	"glideStrafeSideways",
	"glideStrafeForwardTaperStart",
	"glideStrafeForwardTaperFinish",
	"glideStrafeSidewaysTaperStart",
	"glideStrafeSidewaysTaperFinish",
	"glideMaxSpeed",
	"glideSpeedDecelerationToMax",
	"jetpackScriptToActivate",
	"glideDuration",
};

static uint32_t s_nFieldOff[GLF_COUNT];
static bool s_bFieldsResolved = false;
static bool s_bFieldsUsable = false;

static bool Glide_ResolveFields(void)
{
	if (s_bFieldsResolved)
		return s_bFieldsUsable;
	if (!Bridge_HasPlayerSettingsLayout())
		return false;

	int nMissing = 0;
	for (int i = 0; i < GLF_COUNT; ++i)
	{
		const uint32_t nOff = Bridge_LookupPlayerSettingsFieldOffset(s_glideFieldNames[i]);
		s_nFieldOff[i] = (nOff < GL_SETTINGS_OFF_CAP) ? nOff : GL_FIELD_MISSING;
		if (s_nFieldOff[i] == GL_FIELD_MISSING)
		{
			++nMissing;
			Warning(eDLL_T::SERVER, "[GLIDE] settings field '%s' not in the player layout\n", s_glideFieldNames[i]);
		}
	}

	s_bFieldsResolved = true;
	// Without the activation fields the S21 rules cannot be evaluated; leave the stock glide alone.
	s_bFieldsUsable = s_nFieldOff[GLF_ENABLED] != GL_FIELD_MISSING
		&& s_nFieldOff[GLF_SCRIPT_TO_ACTIVATE] != GL_FIELD_MISSING;
	Msg(eDLL_T::SERVER, "[GLIDE] %d/%d settings fields resolved%s\n", GLF_COUNT - nMissing, GLF_COUNT,
		s_bFieldsUsable ? "" : " -- activation fields missing, stock glide stays");
	return s_bFieldsUsable;
}

static inline const uint8_t* Glide_Settings(const uint8_t* pPlayer)
{
	return *reinterpret_cast<const uint8_t* const*>(pPlayer + GL_OFF_SETTINGS);
}

static inline float Glide_F(const uint8_t* pSettings, const GlideField_e f)
{
	const uint32_t nOff = s_nFieldOff[f];
	if (!pSettings || nOff == GL_FIELD_MISSING)
		return 0.0f;
	return *reinterpret_cast<const float*>(pSettings + nOff);
}

static inline bool Glide_B(const uint8_t* pSettings, const GlideField_e f)
{
	const uint32_t nOff = s_nFieldOff[f];
	if (!pSettings || nOff == GL_FIELD_MISSING)
		return false;
	return pSettings[nOff] != 0;
}

static inline float Glide_PF(const uint8_t* pPlayer, const ptrdiff_t off)
{
	return *reinterpret_cast<const float*>(pPlayer + off);
}

static inline float Glide_Now(void) { return gpGlobals ? gpGlobals->curTime : 0.0f; }
static inline float Glide_Dt(void)  { return gpGlobals ? gpGlobals->frameTime : 0.0f; }
static inline float Glide_Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static ConVar* s_pGravityVar = nullptr;

static float Glide_Gravity(void)
{
	if (!s_pGravityVar && g_pCVar)
		s_pGravityVar = g_pCVar->FindVar("sv_gravity");
	const float fl = s_pGravityVar ? s_pGravityVar->GetFloat() : 750.0f;
	return std::isfinite(fl) ? fl : 750.0f;
}

struct GlideState
{
	bool  m_bActivate = false;
	bool  m_bTouchedGround = false;   // m_touchedGroundSinceLastGlide
	float m_flBoostEndTime = -1.0f;   // m_glideUpwardsBoostEndTime
	float m_flBoostFallRatio = 0.0f;
	float m_flPrevJumpPressTime = 0.0f;
};

static SDKEntityMap<GlideState> s_glideMap(ESide::Server, "glide.state");
static uint32_t s_nDiagTick = 0;
static int32_t s_nDiagCmd = 0;
static float s_flDiagCmdFrameTime = 0.0f;
static bool s_bInCallback = false;

static void (*v_Glide_Check)(void* pPlayer, void* pUnused, float* vel) = nullptr;
static void (*v_Glide_Apply)(void* pPlayer, float* vel, const float* moveFwd, const float* moveRight) = nullptr;
static int64_t (*v_Player_AirAbilityEvent)(void* pPlayer, unsigned int nEvent, int nUnused) = nullptr;
static void (*v_Player_OnTouchSurface)(void* pPlayer) = nullptr;
static const uint32_t* s_pStfJetpackToggleOn = nullptr; // settings-block offset of jetpackToggleOn

static bool Glide_Enabled(void)
{
	return bridge_glide.GetBool() && Glide_ResolveFields();
}

static void Glide_MirrorWire(void* pPlayer, const GlideState& st)
{
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_activateGlide), st.m_bActivate ? 1 : 0);
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_touchedGroundSinceLastGlide), st.m_bTouchedGround ? 1 : 0);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_glideUpwardsBoostEndTime), st.m_flBoostEndTime);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_prevJumpPressTime), st.m_flPrevJumpPressTime);
	MarkEntityEdictDirty(pPlayer);
}

static inline void Glide_SetByte(uint8_t* pPlayer, const ptrdiff_t off, const uint8_t v)
{
	if (pPlayer[off] == v)
		return;
	pPlayer[off] = v;
	MarkEntityEdictDirty(pPlayer);
}

static inline void Glide_SetFloat(uint8_t* pPlayer, const ptrdiff_t off, const float v)
{
	float* const p = reinterpret_cast<float*>(pPlayer + off);
	if (*p == v)
		return;
	*p = v;
	MarkEntityEdictDirty(pPlayer);
}

static void Glide_CancelHostShutdown(const char* pszName,
	const HostStates_t iStateBefore, const HostStates_t iNextBefore)
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
	Warning(eDLL_T::SERVER, "[GLIDE] cancelled host shutdown scheduled by '%s'\n", pszName);
}

// Absent callback = no call; a script error is logged and contained.
static void Glide_FireCallback(const char* pszName, void* pPlayer, bool* pbWarned)
{
	if (!g_pServerScript || !pPlayer || s_bInCallback)
		return;

	const HSCRIPT hFunc = g_pServerScript->FindFunction(pszName, nullptr, nullptr);
	if (!hFunc)
	{
		if (!*pbWarned)
		{
			*pbWarned = true;
			Warning(eDLL_T::SERVER, "[GLIDE] %s not found in server VM -- skipped\n", pszName);
		}
		return;
	}

	const HSCRIPT hPlayer = reinterpret_cast<CBaseEntity*>(pPlayer)->GetScriptInstance();
	if (!hPlayer)
		return;

	s_bInCallback = true;
	const HostStates_t iStateBefore = g_pHostState ? g_pHostState->m_iCurrentState : HostStates_t::HS_RUN;
	const HostStates_t iNextBefore = g_pHostState ? g_pHostState->m_iNextState : HostStates_t::HS_RUN;

	ScriptVariant_t args[1];
	args[0] = hPlayer;
	if (g_pServerScript->ExecuteFunction(hFunc, args, 1, nullptr, nullptr) == SCRIPT_ERROR)
	{
		Warning(eDLL_T::SERVER, "[GLIDE] %s SCRIPT_ERROR\n", pszName);
		Glide_CancelHostShutdown(pszName, iStateBefore, iNextBefore);
	}
	s_bInCallback = false;
}

static bool Glide_HandleLive(const uint8_t* pPlayer, const ptrdiff_t off)
{
	const uint32_t eh = *reinterpret_cast<const uint32_t*>(pPlayer + off);
	return eh != GL_INVALID_HANDLE && SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server) != nullptr;
}

static bool Glide_SuperJumpAllowedNow(const uint8_t* pPlayer, const uint8_t* pSet)
{
	if (!Glide_B(pSet, GLF_DOUBLE_JUMP))
		return false;
	const float flLimit = Glide_F(pSet, GLF_SUPERJUMP_LIMIT);
	return flLimit < 0.0f || flLimit > static_cast<float>(*reinterpret_cast<const int*>(pPlayer + GL_OFF_SUPERJUMPSUSED));
}

// S21 CanUseGlide.
static int Glide_CanUse(uint8_t* pPlayer)
{
	const uint8_t* const pSet = Glide_Settings(pPlayer);
	CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
	const float now = Glide_Now();

	if (!Glide_B(pSet, GLF_ENABLED) || pPlayer[GL_OFF_LIFESTATE]
		|| (pPlayer[GL_OFF_GRAPPLEACTIVE] && pPlayer[GL_OFF_GRAPPLEATTACH]))
		return GLIDE_ENGAGE_FAILED_OTHER;
	if (Glide_B(pSet, GLF_JETPACK_ENABLED))
		return GLIDE_ENGAGE_FAILED_OTHER;
	if (Glide_F(pSet, GLF_BOOST_ON_GROUND_SAFETY) >= now - Glide_PF(pPlayer, GL_OFF_TOUCHEDGROUND))
		return GLIDE_ENGAGE_FAILED_OTHER;
	if (pPlayer[GL_OFF_HOVERING] || pPlayer[GL_OFF_GLIDING] || pPlayer[GL_OFF_JETPACK])
		return GLIDE_ENGAGE_FAILED_OTHER;
	if (pPlayer[GL_OFF_MOVETYPE] != GL_MOVETYPE_WALK || Jetpack_IsZiplining(pEnt))
		return GLIDE_ENGAGE_FAILED_OTHER;
	if (Glide_SuperJumpAllowedNow(pPlayer, pSet))
		return GLIDE_ENGAGE_FAILED_OTHER;

	const float flLastJumped = Glide_PF(pPlayer, GL_OFF_LASTJUMPED);
	if (flLastJumped == now)
		return GLIDE_ENGAGE_FAILED_OTHER;
	const float flSinceZipDetach = now - Glide_PF(pPlayer, GL_OFF_ZIPDETACHTIME);
	if (flSinceZipDetach <= GL_MIN_INTERVAL)
		return GLIDE_ENGAGE_FAILED_OTHER;

	if (!(*reinterpret_cast<const int*>(pPlayer + GL_OFF_BUTTONSPRESSED) & GL_IN_JUMP))
	{
		if (!Glide_B(pSet, GLF_HOLD_INPUT_ACTIVATION))
			return GLIDE_ENGAGE_FAILED_OTHER;
		const float flHold = Glide_F(pSet, GLF_TIME_TO_HOLD_INPUT);
		if (flHold >= now - flLastJumped || flHold >= flSinceZipDetach)
			return GLIDE_ENGAGE_FAILED_OTHER;
	}

	const GlideState* const pState = s_glideMap.Find(pPlayer);
	if (pState && now - pState->m_flPrevJumpPressTime <= GL_MIN_INTERVAL)
		return GLIDE_ENGAGE_FAILED_OTHER;

	return Glide_PF(pPlayer, GL_OFF_GLIDEMETER) <= 0.0f ? GLIDE_ENGAGE_OUT_OF_METER : GLIDE_ENGAGE_SUCCEED;
}

static void Glide_Stop(uint8_t* pPlayer, const char* pszWhy)
{
	if (!pPlayer[GL_OFF_GLIDING])
		return;

	Glide_SetByte(pPlayer, GL_OFF_GLIDING, 0);

	if (bridge_glide_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[GLIDE] stop (%s) player=%p meter=%.3f\n",
			pszWhy, pPlayer, Glide_PF(pPlayer, GL_OFF_GLIDEMETER));

	static bool s_bWarned = false;
	Glide_FireCallback("CodeCallback_OnPlayerGlideStop", pPlayer, &s_bWarned);
}

static void Glide_Start(uint8_t* pPlayer, const float* vel)
{
	const uint8_t* const pSet = Glide_Settings(pPlayer);

	static bool s_bWarned = false;
	Glide_FireCallback("CodeCallback_OnPlayerGlideStart", pPlayer, &s_bWarned);

	if (v_Player_AirAbilityEvent)
		v_Player_AirAbilityEvent(pPlayer, 7, 0);

	if (pPlayer[GL_OFF_BOOSTING] && Glide_B(pSet, GLF_BOOST_REPEATED_SHORT))
		Glide_SetByte(pPlayer, GL_OFF_REPEATEDBOOST, 1);
	Glide_SetByte(pPlayer, GL_OFF_BOOSTING, 0);
	Glide_SetFloat(pPlayer, GL_OFF_RECHARGEACCUM, 0.0f);
	Glide_SetByte(pPlayer, GL_OFF_GLIDING, 1);

	GlideState& st = s_glideMap[pPlayer];
	if (Glide_B(pSet, GLF_UPBOOST_ENABLED))
	{
		const GlideBoost_t boost = Glide_ArmBoost(vel[2], Glide_Gravity(), Glide_Now());
		st.m_flBoostEndTime = boost.flEndTime;
		st.m_flBoostFallRatio = boost.flFallRatio;
	}
	Glide_MirrorWire(pPlayer, st);

	if (bridge_glide_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[GLIDE] start player=%p vz=%.1f meter=%.3f boostEnd=%.3f\n",
			pPlayer, vel[2], Glide_PF(pPlayer, GL_OFF_GLIDEMETER), st.m_flBoostEndTime);
}

static void Hook_Glide_Check(void* pPlayer, void* pUnused, float* vel)
{
	if (!pPlayer || !vel || !Glide_Enabled())
		return v_Glide_Check(pPlayer, pUnused, vel);

	uint8_t* const p = static_cast<uint8_t*>(pPlayer);
	const uint8_t* const pSet = Glide_Settings(p);

	if (Glide_B(pSet, GLF_SCRIPT_TO_ACTIVATE))
	{
		if (p[GL_OFF_BOOSTING] || p[GL_OFF_GLIDING])
			return;
		const GlideState* const pState = s_glideMap.Find(pPlayer);
		if (!pState || !pState->m_bActivate)
			return;
	}
	else
	{
		if (Glide_CanUse(p) != GLIDE_ENGAGE_SUCCEED || p[GL_OFF_GLIDING] || p[GL_OFF_BOOSTING])
			return;
		if (!(*reinterpret_cast<const int*>(p + GL_OFF_BUTTONSPRESSED) & GL_IN_JUMP)
			&& !(*reinterpret_cast<const int*>(p + GL_OFF_BUTTONS) & GL_IN_JUMP))
			return;
	}

	if (Glide_PF(p, GL_OFF_GLIDEMETER) <= 0.0f)
		return;

	Glide_Start(p, vel);
}

// Server CPlayer offsets, S21 ApplyGlide math.
static void Glide_Apply(uint8_t* pPlayer, float* vel, const float* moveFwd, const float* moveRight)
{
	if (!pPlayer[GL_OFF_GLIDING])
		return;

	const uint8_t* const pSet = Glide_Settings(pPlayer);
	CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
	const bool bScript = Glide_B(pSet, GLF_SCRIPT_TO_ACTIVATE);
	const GlideState* const pState = s_glideMap.Find(pPlayer);

	if (pPlayer[GL_OFF_LIFESTATE])
		return Glide_Stop(pPlayer, "dead");
	if (pEnt->Diag_HasMoveParent())
		return Glide_Stop(pPlayer, "parented");
	if (bScript && (!pState || !pState->m_bActivate))
		return Glide_Stop(pPlayer, "script");
	if (pPlayer[GL_OFF_GRAPPLEACTIVE] && pPlayer[GL_OFF_GRAPPLEATTACH])
		return Glide_Stop(pPlayer, "grapple");
	if (pPlayer[GL_OFF_JETPACK] || pPlayer[GL_OFF_HOVERING])
		return Glide_Stop(pPlayer, "jetpack");
	if (*reinterpret_cast<const uint32_t*>(pPlayer + GL_OFF_FLAGS) & GL_FL_ONGROUND)
		return Glide_Stop(pPlayer, "ground");
	if (pPlayer[GL_OFF_MOVETYPE] != GL_MOVETYPE_WALK)
		return Glide_Stop(pPlayer, "movetype");
	if (Jetpack_IsZiplining(pEnt))
		return Glide_Stop(pPlayer, "zipline");
	if (Glide_PF(pPlayer, GL_OFF_GLIDEMETER) <= 0.0f)
		return Glide_Stop(pPlayer, "fuel");
	if (!(*reinterpret_cast<const int*>(pPlayer + GL_OFF_BUTTONS) & GL_IN_JUMP)
		&& !Glide_HandleLive(pPlayer, GL_OFF_ACTIVEWEAP_ALT) && !bScript)
		return Glide_Stop(pPlayer, "released");

	const float dt = Glide_Dt();
	const float now = Glide_Now();

	const float flGravity = Glide_Gravity();
	// Taper tests use the horizontal velocity before this tick's changes.
	const float flFwdVel = vel[0] * moveFwd[0] + vel[1] * moveFwd[1];
	const float flSideVel = vel[0] * moveRight[0] + vel[1] * moveRight[1];

	const float flVzIn = vel[2];
	const float flBoostEnd = pState ? pState->m_flBoostEndTime : -1.0f;
	float flBoostG = 0.0f;
	if (Glide_B(pSet, GLF_UPBOOST_ENABLED) && pState)
	{
		flBoostG = Glide_BoostThrust(flBoostEnd, pState->m_flBoostFallRatio, now);
		vel[2] += flBoostG * flGravity * dt;
	}

	const float flThrust = Glide_DecayedThrust(Glide_F(pSet, GLF_THRUST),
		Glide_PF(pPlayer, GL_OFF_GLIDEMETER), Glide_F(pSet, GLF_DURATION));
	vel[2] += flGravity * flThrust * dt;
	const float flTight = Glide_F(pSet, vel[2] <= 0.0f ? GLF_TIGHTNESS_DOWN : GLF_TIGHTNESS_UP);
	vel[2] *= expf((-GL_LN2 / flTight) * dt);

	const float fwdMove = Glide_PF(pPlayer, GL_OFF_FORWARDMOVE);
	const float sideMove = Glide_PF(pPlayer, GL_OFF_SIDEMOVE);
	float f[3] = { moveFwd[0] * fwdMove, moveFwd[1] * fwdMove, moveFwd[2] * fwdMove };
	float s[3] = { moveRight[0] * sideMove, moveRight[1] * sideMove, moveRight[2] * sideMove };

	const float hx = f[0] + s[0];
	const float hy = f[1] + s[1];
	const float flWishSqr = hx * hx + hy * hy;
	if (flWishSqr > 0.0000010000001f)
	{
		// Each axis is normalised on its own once the combined wish exceeds 1.
		if (flWishSqr > 1.0f)
		{
			const float invF = 1.0f / fmaxf(sqrtf(Glide_Dot(f, f)), FLT_MIN);
			const float invS = 1.0f / fmaxf(sqrtf(Glide_Dot(s, s)), FLT_MIN);
			for (int i = 0; i < 3; ++i)
			{
				f[i] *= invF;
				s[i] *= invS;
			}
		}

		const float flStrafeF = Glide_F(pSet, GLF_STRAFE_FORWARD);
		const float flStrafeS = Glide_F(pSet, GLF_STRAFE_SIDEWAYS);
		float fa[3] = { f[0] * flStrafeF, f[1] * flStrafeF, f[2] * flStrafeF };
		float sa[3] = { s[0] * flStrafeS, s[1] * flStrafeS, s[2] * flStrafeS };

		// A taper only limits input that pushes along the current velocity; braking is never tapered.
		const float flFwdStart = Glide_F(pSet, GLF_STRAFE_FORWARD_TAPER_START);
		const float flFwdFinish = Glide_F(pSet, GLF_STRAFE_FORWARD_TAPER_FINISH);
		if (flFwdFinish > flFwdStart && flFwdVel * fwdMove > 0.0f)
		{
			const float d = fabsf(flFwdVel);
			float k = 1.0f;
			if (d > flFwdStart && d < flFwdFinish)
				k = 1.0f - (d - flFwdStart) / (flFwdFinish - flFwdStart);
			else if (d >= flFwdFinish)
				k = 0.0f;
			fa[0] *= k; fa[1] *= k; fa[2] *= k;
		}

		const float flSideStart = Glide_F(pSet, GLF_STRAFE_SIDEWAYS_TAPER_START);
		const float flSideFinish = Glide_F(pSet, GLF_STRAFE_SIDEWAYS_TAPER_FINISH);
		if (flSideFinish > flSideStart && flSideVel * sideMove > 0.0f)
		{
			const float d = fabsf(flSideVel);
			float k = 1.0f;
			if (d > flSideStart && d < flSideFinish)
				k = 1.0f - (d - flSideStart) / (flSideFinish - flSideStart);
			else if (d >= flSideFinish)
				k = 0.0f;
			sa[0] *= k; sa[1] *= k; sa[2] *= k;
		}

		for (int i = 0; i < 3; ++i)
			vel[i] += (fa[i] + sa[i]) * dt;
	}

	Glide_DecayHorizontal(vel, Glide_F(pSet, GLF_MAX_SPEED), dt);

	Jetpack_ClearGround(pPlayer);

	const int nDiag = bridge_glide_diag.GetInt();
	if (nDiag >= 2 && (++s_nDiagTick % static_cast<uint32_t>(nDiag)) == 0)
		Msg(eDLL_T::SERVER, "[GLIDE] apply cmd=%d now=%.4f dt=%.5f cmdFt=%.5f vz=%.2f->%.2f vel=(%.1f %.1f) "
			"meter=%.3f thrust=%.3f boostG=%.3f boostEnd=%.3f ratio=%.3f\n",
			s_nDiagCmd, now, dt, s_flDiagCmdFrameTime, flVzIn, vel[2], vel[0], vel[1],
			Glide_PF(pPlayer, GL_OFF_GLIDEMETER), flThrust, flBoostG, flBoostEnd,
			pState ? pState->m_flBoostFallRatio : 0.0f);
}

static void Hook_Glide_Apply(void* pPlayer, float* vel, const float* moveFwd, const float* moveRight)
{
	if (!pPlayer || !vel || !moveFwd || !moveRight || !Glide_Enabled())
		return v_Glide_Apply(pPlayer, vel, moveFwd, moveRight);

	Glide_Apply(static_cast<uint8_t*>(pPlayer), vel, moveFwd, moveRight);
}

void Glide_PreRunCommand(CPlayer* pPlayer, CUserCmd* pUserCmd)
{
	if (!pPlayer || !pUserCmd || !Glide_Enabled())
		return;

	const uint8_t* const p = reinterpret_cast<const uint8_t*>(pPlayer);
	s_nDiagCmd = pUserCmd->command_number;
	s_flDiagCmdFrameTime = pUserCmd->frametime;

	// The engine stamps the new press time during this command; the one it replaces is the previous press.
	const bool bHeldBefore = (*reinterpret_cast<const int*>(p + GL_OFF_BUTTONS) & GL_IN_JUMP) != 0;
	if (!(pUserCmd->buttons & GL_IN_JUMP) || bHeldBefore)
		return;

	GlideState& st = s_glideMap[pPlayer];
	st.m_flPrevJumpPressTime = Glide_PF(p, GL_OFF_JUMPPRESSTIME);
	Glide_MirrorWire(pPlayer, st);
}

// Landing and wall contact: the S21 client re-arms the once-per-glide upward boost here.
static void Hook_Player_OnTouchSurface(void* pPlayer)
{
	v_Player_OnTouchSurface(pPlayer);

	if (!pPlayer || !Glide_Enabled())
		return;

	GlideState& st = s_glideMap[pPlayer];
	if (st.m_bTouchedGround)
		return;
	st.m_bTouchedGround = true;
	Glide_MirrorWire(pPlayer, st);
}

// Replaces the stock duck block 'm_jetpack && jetpackToggleOn' with the S21 rule.
static bool __fastcall Glide_DuckInputBlocked(const uint8_t* pPlayer)
{
	if (!pPlayer)
		return false;

	const uint8_t* const pSet = Glide_Settings(pPlayer);
	if (!pSet)
		return false;

	if (pPlayer[GL_OFF_JETPACK] && s_pStfJetpackToggleOn && pSet[*s_pStfJetpackToggleOn])
		return true;
	if (!Glide_Enabled())
		return false;

	if (pPlayer[GL_OFF_JETPACK] && Glide_B(pSet, GLF_JETPACK_SCRIPT_TO_ACTIVATE))
		return true;
	return pPlayer[GL_OFF_GLIDING] && Glide_B(pSet, GLF_SCRIPT_TO_ACTIVATE);
}

// 'cmp [rbx+695Ch], r14b ; jz ; mov ecx, stf_jetpackToggleOn ; mov rax, [rbx+5F08h] ;
// cmp [rcx+rax], r14b ; jz ; xor r9b, r9b' -- rbx = player, r9b = wants duck.
static constexpr size_t kDuckSiteBytes = 31;
static constexpr size_t kDuckCaveBytes = 160;
static uint8_t* s_pDuckSite = nullptr;
static uint8_t* s_pDuckCave = nullptr;

static bool Glide_BuildDuckCave(void)
{
	s_pDuckCave = Mem_AllocNearModule(g_GameDll, kDuckCaveBytes);
	if (!s_pDuckCave)
		return false;

	DWORD old = 0;
	VirtualProtect(s_pDuckCave, kDuckCaveBytes, PAGE_EXECUTE_READWRITE, &old);
	memset(s_pDuckCave, 0xCC, kDuckCaveBytes);

	uint8_t* p = s_pDuckCave;
	auto emit = [&p](std::initializer_list<uint8_t> bytes)
	{
		for (const uint8_t b : bytes)
			*p++ = b;
	};

	// The call clobbers every volatile register; r8-r10 and rdx are live at the site, xmm0-5 may be.
	emit({ 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 });   // push rdx, r8, r9, r10, r11
	emit({ 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 });                // sub rsp, 0x88
	emit({ 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x28 });                      // movdqu [rsp+28h], xmm0
	emit({ 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x38 });                      // movdqu [rsp+38h], xmm1
	emit({ 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x48 });                      // movdqu [rsp+48h], xmm2
	emit({ 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x58 });                      // movdqu [rsp+58h], xmm3
	emit({ 0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x68 });                      // movdqu [rsp+68h], xmm4
	emit({ 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x78 });                      // movdqu [rsp+78h], xmm5
	emit({ 0x48, 0x8B, 0xCB });                                        // mov rcx, rbx
	emit({ 0x48, 0xB8 });                                              // mov rax, imm64
	const uintptr_t fn = reinterpret_cast<uintptr_t>(&Glide_DuckInputBlocked);
	memcpy(p, &fn, sizeof(fn));
	p += sizeof(fn);
	emit({ 0xFF, 0xD0 });                                              // call rax
	emit({ 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x28 });                      // movdqu xmm0, [rsp+28h]
	emit({ 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x38 });                      // movdqu xmm1, [rsp+38h]
	emit({ 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x48 });                      // movdqu xmm2, [rsp+48h]
	emit({ 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x58 });                      // movdqu xmm3, [rsp+58h]
	emit({ 0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x68 });                      // movdqu xmm4, [rsp+68h]
	emit({ 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x78 });                      // movdqu xmm5, [rsp+78h]
	emit({ 0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00 });                // add rsp, 0x88
	emit({ 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A });   // pop r11, r10, r9, r8, rdx
	emit({ 0x84, 0xC0 });                                              // test al, al
	emit({ 0x74, 0x03 });                                              // jz back
	emit({ 0x45, 0x32, 0xC9 });                                        // xor r9b, r9b
	emit({ 0xE9 });                                                    // back: jmp site end
	const int32_t rel = static_cast<int32_t>((s_pDuckSite + kDuckSiteBytes) - (p + 4));
	memcpy(p, &rel, sizeof(rel));
	p += sizeof(rel);

	return static_cast<size_t>(p - s_pDuckCave) <= kDuckCaveBytes;
}

static void Glide_InstallDuckRule(void)
{
	if (!s_pDuckSite || s_pDuckCave)
		return;

	if (!Glide_BuildDuckCave())
	{
		Warning(eDLL_T::SERVER, "[GLIDE] duck cave alloc failed -- the server ducks during a script glide\n");
		return;
	}

	uint8_t jmp[kDuckSiteBytes];
	memset(jmp, 0x90, sizeof(jmp));
	jmp[0] = 0xE9;
	const int32_t rel = static_cast<int32_t>(s_pDuckCave - (s_pDuckSite + 5));
	memcpy(jmp + 1, &rel, sizeof(rel));

	if (!Mem_PatchCode(s_pDuckSite, jmp, sizeof(jmp)))
	{
		Warning(eDLL_T::SERVER, "[GLIDE] duck site patch failed -- the server ducks during a script glide\n");
		return;
	}
	Msg(eDLL_T::SERVER, "[GLIDE] duck rule installed\n");
}

void Glide_LevelShutdown(void)
{
	s_glideMap.Clear();
	s_bInCallback = false;
}

//-----------------------------------------------------------------------------
// Purpose: player.CanUseGlide( vector velocity ) -> GLIDE_ENGAGE_*
//-----------------------------------------------------------------------------
static SQRESULT Script_CanUseGlide(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	const int nResult = Glide_Enabled()
		? Glide_CanUse(static_cast<uint8_t*>(pPlayer))
		: GLIDE_ENGAGE_FAILED_OTHER;

	if (bridge_glide_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[GLIDE] CanUseGlide player=%p -> %d\n", pPlayer, nResult);

	sq_pushinteger(v, nResult);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: player.SetActivateGlide( bool )
//-----------------------------------------------------------------------------
static SQRESULT Script_SetActivateGlide(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQBool bActivate = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bActivate)))
		return SQ_ERROR;

	GlideState& st = s_glideMap[pPlayer];
	st.m_bActivate = bActivate != SQFalse;
	Glide_MirrorWire(pPlayer, st);

	static bool s_bLogged = false;
	if (!s_bLogged)
	{
		s_bLogged = true;
		Msg(eDLL_T::SERVER, "[GLIDE] SetActivateGlide first call player=%p value=%d\n", pPlayer, st.m_bActivate ? 1 : 0);
	}
	if (bridge_glide_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[GLIDE] SetActivateGlide player=%p %d\n", pPlayer, st.m_bActivate ? 1 : 0);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void Glide_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"CanUseGlide",
		"Script_CanUseGlide",
		"Returns GLIDE_ENGAGE_SUCCEED, GLIDE_ENGAGE_OUT_OF_METER or GLIDE_ENGAGE_FAILED_OTHER",
		"int",
		"vector velocity",
		false,
		Script_CanUseGlide);

	playerStruct->AddFunction(
		"SetActivateGlide",
		"Script_SetActivateGlide",
		"Starts (true) or ends (false) this player's script-activated glide",
		"void",
		"bool activate",
		false,
		Script_SetActivateGlide);
}

void VGlideBridge::GetAdr(void) const
{
	LogFunAdr("Glide_Check", v_Glide_Check);
	LogFunAdr("Glide_Apply", v_Glide_Apply);
	LogFunAdr("Player_AirAbilityEvent", v_Player_AirAbilityEvent);
	LogFunAdr("Player_OnTouchSurface", v_Player_OnTouchSurface);
	LogVarAdr("Glide_DuckSite", s_pDuckSite);
}

void VGlideBridge::GetFun(void) const
{
	// Server-half bodies: the settings-block displacement 0x5F08 separates them from the client twins.
	CMemory check = Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 30 48 8B 99 08 5F 00 00 48 8B F9 8B 05 ?? ?? ?? ?? 80 3C 18 00 0F 84 ?? ?? ?? ?? 80 B9 99 04 00 00 00");
	check.GetPtr(v_Glide_Check);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 48 89 6C 24 20 56 57 41 57 48 81 EC C0 00 00 00 80 B9 5F 69 00 00 00 49 8B E9 48 8B 99 08 5F 00 00")
		.GetPtr(v_Glide_Apply);

	// The stock check's start path notifies the player's view models of the air-ability event 7.
	if (check)
	{
		const CMemory call = check.Offset(0x11B);
		if (*call.RCast<const uint8_t*>() == 0xE8)
			call.FollowNearCall().GetPtr(v_Player_AirAbilityEvent);
	}

	if (!v_Glide_Check || !v_Glide_Apply)
		Warning(eDLL_T::SERVER, "[GLIDE] pattern unresolved (check=%d apply=%d) -- stock glide stays\n",
			v_Glide_Check ? 1 : 0, v_Glide_Apply ? 1 : 0);
	if (!v_Player_AirAbilityEvent)
		Warning(eDLL_T::SERVER, "[GLIDE] air-ability event call not found -- glide start skips it\n");

	// Server half of the land / wall-touch reset: superjumps from the settings block at +0x5F08.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 F2 0F 10 89 2C 66 00 00 0F 57 DB 8B 81 34 66 00 00")
		.GetPtr(v_Player_OnTouchSurface);
	if (!v_Player_OnTouchSurface)
		Warning(eDLL_T::SERVER, "[GLIDE] land reset pattern unresolved -- once-per-glide boost never re-arms\n");

	// CGameMovement::Duck, server half: the jetpack duck block.
	const CMemory duck = Module_FindPattern(g_GameDll,
		"44 38 B3 5C 69 00 00 74 16 8B 0D ?? ?? ?? ?? 48 8B 83 08 5F 00 00 44 38 34 01 74 03 45 32 C9");
	if (duck)
	{
		s_pDuckSite = duck.RCast<uint8_t*>();
		s_pStfJetpackToggleOn = duck.Offset(9).ResolveRelativeAddress(2, 6).RCast<const uint32_t*>();
	}
	else
		Warning(eDLL_T::SERVER, "[GLIDE] duck block pattern unresolved -- the server ducks during a script glide\n");
}

void VGlideBridge::Detour(const bool bAttach) const
{
	if (!v_Glide_Check || !v_Glide_Apply)
		return;

	DetourSetup(&v_Glide_Check, &Hook_Glide_Check, bAttach);
	DetourSetup(&v_Glide_Apply, &Hook_Glide_Apply, bAttach);
	if (v_Player_OnTouchSurface)
		DetourSetup(&v_Player_OnTouchSurface, &Hook_Player_OnTouchSurface, bAttach);
	if (bAttach)
		Glide_InstallDuckRule();
}
