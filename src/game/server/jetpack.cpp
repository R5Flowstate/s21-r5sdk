//=============================================================================//
//
// Purpose: S21 jetpack on the dedicated server -- the fuel natives the S3
// script VM lacks and the S21 flight model the S21 client predicts.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "jetpack.h"
#include "trigger_cannon.h"
#include "trigger_gravity.h"
#include "offhand_jump_toggle.h"
#include "baseentity.h"
#include "engine/server/snapshot_diag.h"
#include "public/gametrace.h"
#include "mathlib/vector.h"
#include "game/shared/usercmd.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cfloat>
#include <cmath>
#include <cstring>

// Server CPlayer layout.
static constexpr ptrdiff_t JP_PLAYER_OFF_ACTIVEWEAP = 0x16CC; // m_inventory.activeWeapons[0]
static constexpr ptrdiff_t JP_PLAYER_OFF_GROUNDENT  = 0x3C4;  // EHANDLE, -1 airborne
static constexpr ptrdiff_t JP_PLAYER_OFF_JETPACK    = 0x695C; // bool m_jetpack
static constexpr ptrdiff_t JP_PLAYER_OFF_GLIDEMETER = 0x6960; // float m_glideMeter (native SendProp)
static constexpr uint32_t  JP_INVALID_HANDLE        = 0xFFFFFFFFu;
static constexpr int       JP_IN_JUMP               = 0x00000002;
static constexpr float     JP_GLIDEMETER_MAX        = 1000.0f;

static void Jetpack_FlightLevelShutdown(void);

void Jetpack_LevelShutdown(void)
{
	Jetpack_FlightLevelShutdown();
}

//-----------------------------------------------------------------------------
// Purpose: player.SetGlideMeter( float ) -- the S3 server VM only has the getter
//-----------------------------------------------------------------------------
static SQRESULT Script_SetGlideMeter(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQFloat fl = 0.0;
	sq_getfloat(v, 2, &fl);
	const float flIn = static_cast<float>(fl);
	if (!std::isfinite(flIn))
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[JETS] SetGlideMeter rejected non-finite value player=%p\n", pPlayer);
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const float flMeter = fminf(fmaxf(flIn, 0.0f), JP_GLIDEMETER_MAX);
	*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pPlayer) + JP_PLAYER_OFF_GLIDEMETER) = flMeter;
	MarkEntityEdictDirty(pPlayer);

	static bool s_bLogged = false;
	if (!s_bLogged)
	{
		s_bLogged = true;
		Msg(eDLL_T::SERVER, "[JETS] SetGlideMeter first call player=%p value=%.2f\n", pPlayer, flMeter);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void Jetpack_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction(
		"SetGlideMeter",
		"Script_SetGlideMeter",
		"Sets this player's jetpack / glide fuel meter",
		"void",
		"float meter",
		false,
		Script_SetGlideMeter);
}

//=============================================================================
// Flight model. The S21 client predicts the jetpack with fields the S3 server
// half never reads -- thrust ramp on activation, vertical-speed scale on
// activation and release, post-effect gravity and drag, per-activation fuel,
// tactical fuel rate -- so the server runs the same math.
//=============================================================================
static constexpr ptrdiff_t JP_PLAYER_OFF_SETTINGS       = 0x5F08; // player settings block
static constexpr ptrdiff_t JP_PLAYER_OFF_GRAVITY        = 0x5B8;  // CBaseEntity::m_flGravity
static constexpr ptrdiff_t JP_PLAYER_OFF_FLAGS          = 0x234;  // m_fFlags
static constexpr ptrdiff_t JP_PLAYER_OFF_MOVETYPE       = 0x308;
static constexpr ptrdiff_t JP_PLAYER_OFF_ACTIVEWEAP_ALT = 0x16D0; // m_inventory.activeWeapons[1]
static constexpr ptrdiff_t JP_PLAYER_OFF_ZOOMING        = 0x5A61; // m_bZooming
static constexpr ptrdiff_t JP_PLAYER_OFF_JUMPPRESSTIME  = 0x5ABC; // m_Local.m_jumpPressTime
static constexpr ptrdiff_t JP_PLAYER_OFF_ACTIVATETIME   = 0x5AC4; // m_Local.m_jetpackActivateTime
static constexpr ptrdiff_t JP_PLAYER_OFF_BUTTONS        = 0x60DC; // m_nButtons
static constexpr ptrdiff_t JP_PLAYER_OFF_PRESSED        = 0x60E0; // m_afButtonPressed
static constexpr ptrdiff_t JP_PLAYER_OFF_BOOSTED        = 0x6955; // with +0x6957: a repeated boost is pending
static constexpr ptrdiff_t JP_PLAYER_OFF_FORWARDMOVE    = 0x60F4;
static constexpr ptrdiff_t JP_PLAYER_OFF_SIDEMOVE       = 0x60F8;
static constexpr ptrdiff_t JP_PLAYER_OFF_GRAPPLEATTACH  = 0x6780;
static constexpr ptrdiff_t JP_PLAYER_OFF_GRAPPLEACTIVE  = 0x67B8;
static constexpr ptrdiff_t JP_PLAYER_OFF_ACTIVATEJET    = 0x695D; // m_activateJetpack
static constexpr ptrdiff_t JP_PLAYER_OFF_AFTERBURNER    = 0x695E;
static constexpr ptrdiff_t JP_PLAYER_OFF_HOVERING       = 0x6968;
static constexpr ptrdiff_t JP_ENT_VTBL_STATECHANGED     = 0x6F8;  // NetworkStateChanged( void* pVar )

static constexpr int      JP_FL_ONGROUND           = 0x1;
static constexpr int      JP_MOVETYPE_WALK         = 2;
static constexpr int      JP_IN_OFFHAND1           = 0x00200000;
static constexpr int      JP_IN_AFTERBURN_DOWN     = 0x04000004;
static constexpr float    JP_LN2                   = 0.69314718f;
// The S21 client switches an axis to this tightness once its speed exceeds what strafe thrust holds.
static constexpr float    JP_TIGHTNESS_ABOVE_LIMIT = 0.4f;
static constexpr float    JP_POSTEFFECT_PROBE      = 50.0f;
static constexpr unsigned JP_MASK_PLAYERSOLID      = 0x420B;
static constexpr uint32_t JP_SETTINGS_OFF_CAP      = 0x10000u;
static constexpr uint32_t JP_FIELD_MISSING         = 0xFFFFFFFFu;

static ConVar bridge_jetpack("bridge_jetpack", "1", FCVAR_RELEASE,
	"Run the S21 jetpack flight model on the server (thrust ramp, release cut, post-effect "
	"gravity and drag, activation fuel, tactical fuel rate). 0 = stock S3 jetpack.");

static ConVar bridge_jetpack_diag("bridge_jetpack_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Print a [JETPACK] line every N jetpack ticks plus every activation and release (0 = off).");

enum JetpackField_e
{
	JPF_THRUST,
	JPF_HORIZONTAL_MOVE_THRUST,
	JPF_AFTERBURNER_THRUST,
	JPF_AFTERBURNER_BOUND_TO_JUMP,
	JPF_THRUST_SCALE_ON_ACTIVATION,
	JPF_THRUST_SCALE_TIME_ON_ACTIVATION,
	JPF_VVSCALE_ON_ACTIVATION,
	JPF_VVSCALE_ON_DEACTIVATION,
	JPF_TIGHTNESS_UP,
	JPF_TIGHTNESS_DOWN,
	JPF_TIGHTNESS_SIDEWAYS,
	JPF_TIGHTNESS_FORWARDBACK,
	JPF_STRAFE,
	JPF_STRAFE_TAPER_START,
	JPF_STRAFE_TAPER_FINISH,
	JPF_PITCH_MOVEMENT,
	JPF_SCRIPT_TO_ACTIVATE,
	JPF_TOGGLE_ON,
	JPF_TOGGLE_OFF,
	JPF_METER_RATE,
	JPF_METER_ON_ACTIVATION,
	JPF_TACTICAL_METER_RATE,
	JPF_POSTEFFECT_TIME,
	JPF_POSTEFFECT_GRAVITY,
	JPF_POSTEFFECT_DRAG,
	JPF_ENABLED,
	JPF_GLIDE_ENABLED,
	JPF_HOVER_ENABLED,
	JPF_GLIDE_DURATION,
	JPF_RECHARGE_IN_GROUNDED_STATE,
	JPF_RECHARGE_WHEN_FEET_ON_GROUND,
	JPF_JUMPS_TO_ACTIVATE,
	JPF_REPEATED_BOOSTS_SHORT,
	JPF_COUNT
};

struct JetpackFieldDesc_t
{
	const char* pszName;
	float flMissing; // the S3 behaviour when the layout does not carry the field
};

static const JetpackFieldDesc_t s_jetpackFields[JPF_COUNT] =
{
	{ "jetpackThrust",                               0.0f },
	{ "jetpackHorizontalMoveThrust",                -1.0f },
	{ "jetpackAfterburnerThrust",                    0.0f },
	{ "jetpackAfterburnerBoundToJump",               0.0f },
	{ "jetpackThrustScaleOnActivation",              1.0f },
	{ "jetpackThrustScaleTimeOnActivation",          0.0f },
	{ "jetpackVerticalVelocityScaleOnActivation",    1.0f },
	{ "jetpackVerticalVelocityScaleOnDeactivation",  1.0f },
	{ "jetpackTightnessUp",                          1.0f },
	{ "jetpackTightnessDown",                        1.0f },
	{ "jetpackTightnessSideways",                    1.0f },
	{ "jetpackTightnessForwardBack",                 1.0f },
	{ "jetpackStrafe",                               0.0f },
	{ "jetpackStrafeTaperStart",                     0.0f },
	{ "jetpackStrafeTaperFinish",                    0.0f },
	{ "jetpackPitchMovement",                        0.0f },
	{ "jetpackScriptToActivate",                     0.0f },
	{ "jetpackToggleOn",                             0.0f },
	{ "jetpackToggleOff",                            0.0f },
	{ "jetpackMeterRate",                            0.0f },
	{ "jetpackMeterOnActivation",                    0.0f },
	{ "jetpackTacticalMeterRate",                   -1.0f },
	{ "jetpackPostEffectTime",                       0.0f },
	{ "jetpackPostEffectGravity",                    1.0f },
	{ "jetpackPostEffectDrag",                       0.0f },
	{ "jetpackEnabled",                              0.0f },
	{ "glideEnabled",                                0.0f },
	{ "hoverEnabled",                                0.0f },
	{ "glideDuration",                               0.0f },
	{ "glideRechargeOnlyInGroundedState",            0.0f },
	{ "glideRechargeOnlyWhenFeetOnGround",           0.0f },
	{ "jetpackJumpsToActivate",                      0.0f },
	{ "boostRepeatedBoostsAreShort",                 0.0f },
};

static uint32_t s_nFieldOff[JPF_COUNT];
static bool s_bFieldsResolved = false;

static bool Jetpack_ResolveFields(void)
{
	if (s_bFieldsResolved)
		return true;
	if (!Bridge_HasPlayerSettingsLayout())
		return false;

	int nMissing = 0;
	for (int i = 0; i < JPF_COUNT; ++i)
	{
		const uint32_t nOff = Bridge_LookupPlayerSettingsFieldOffset(s_jetpackFields[i].pszName);
		s_nFieldOff[i] = (nOff < JP_SETTINGS_OFF_CAP) ? nOff : JP_FIELD_MISSING;
		if (s_nFieldOff[i] == JP_FIELD_MISSING)
		{
			++nMissing;
			Warning(eDLL_T::SERVER, "[JETPACK] settings field '%s' not in the player layout -- using %.3f\n",
				s_jetpackFields[i].pszName, s_jetpackFields[i].flMissing);
		}
	}

	s_bFieldsResolved = true;
	Msg(eDLL_T::SERVER, "[JETPACK] %d/%d settings fields resolved\n", JPF_COUNT - nMissing, JPF_COUNT);
	return true;
}

static inline const uint8_t* Jetpack_Settings(const uint8_t* pPlayer)
{
	return *reinterpret_cast<const uint8_t* const*>(pPlayer + JP_PLAYER_OFF_SETTINGS);
}

static inline float Jetpack_F(const uint8_t* pSettings, const JetpackField_e f)
{
	const uint32_t nOff = s_nFieldOff[f];
	if (!pSettings || nOff == JP_FIELD_MISSING)
		return s_jetpackFields[f].flMissing;
	return *reinterpret_cast<const float*>(pSettings + nOff);
}

static inline bool Jetpack_B(const uint8_t* pSettings, const JetpackField_e f)
{
	const uint32_t nOff = s_nFieldOff[f];
	if (!pSettings || nOff == JP_FIELD_MISSING)
		return s_jetpackFields[f].flMissing != 0.0f;
	return pSettings[nOff] != 0;
}

static inline int Jetpack_I(const uint8_t* pSettings, const JetpackField_e f)
{
	const uint32_t nOff = s_nFieldOff[f];
	if (!pSettings || nOff == JP_FIELD_MISSING)
		return static_cast<int>(s_jetpackFields[f].flMissing);
	return *reinterpret_cast<const int*>(pSettings + nOff);
}

struct JetpackState
{
	bool  m_bPostGravity = false;     // m_flGravity holds the post-effect value written here
	bool  m_bPostEffect = false;      // release post effect active this tick
	float m_flDeactivateTime = -1.0f;
};

static SDKEntityMap<JetpackState> s_jetpackMap(ESide::Server, "jetpack.state");
static uint32_t s_nApplyTick = 0;

static void  (*v_Jetpack_Check)(void* pPlayer, void* pUnused, float* vel) = nullptr;
static bool  (*v_Jetpack_CanUse)(void* pPlayer, const float* vel) = nullptr;
static void  (*v_Jetpack_Apply)(void* pPlayer, float* vel, const float* moveFwd, const float* moveRight,
	const float* moveUp, const float* lookFwd, const float* lookRight, const float* lookUp) = nullptr;
static int64_t (*v_Jetpack_UpdateMeter)(void* pPlayer, char bGrounded) = nullptr;
static int64_t (*v_Jetpack_ResetAux)(void* pPlayer) = nullptr;
static bool  (*v_Player_IsZiplining)(void* pPlayer) = nullptr;

// SettingsFieldFinder globals; their first dword is the layout offset the engine reads.
static uint32_t* s_pAirDragFinder = nullptr;
static uint32_t* s_pMeterRateFinder = nullptr;
static uint32_t* s_pRechargeGroundedFinder = nullptr;

// jz -> jmp over the two reads of fields the S21 layout does not have (rescued to offset 8).
static uint8_t* s_pZeroVelOnActivationJz = nullptr;
static uint8_t* s_pZeroVelOnDeactivationJz = nullptr;

static const void* s_pForceCanUse = nullptr;
static uint32_t s_nSavedAirDragOff = 0;
static bool s_bAirDragSwapped = false;

static ConVar* s_pGravityVar = nullptr;

static float Jetpack_Gravity(void)
{
	if (!s_pGravityVar && g_pCVar)
		s_pGravityVar = g_pCVar->FindVar("sv_gravity");
	const float fl = s_pGravityVar ? s_pGravityVar->GetFloat() : 750.0f;
	return std::isfinite(fl) ? fl : 750.0f;
}

static inline float Jetpack_Now(void) { return gpGlobals ? gpGlobals->curTime : 0.0f; }
static inline float Jetpack_Dt(void)  { return gpGlobals ? gpGlobals->frameTime : 0.0f; }
static inline float Jetpack_Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static inline bool Jetpack_HandleLive(const uint8_t* pPlayer, const ptrdiff_t off)
{
	const uint32_t eh = *reinterpret_cast<const uint32_t*>(pPlayer + off);
	return eh != JP_INVALID_HANDLE && SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server) != nullptr;
}

static inline void Jetpack_SetByte(uint8_t* pPlayer, const ptrdiff_t off, const uint8_t v)
{
	if (pPlayer[off] == v)
		return;
	pPlayer[off] = v;
	MarkEntityEdictDirty(pPlayer);
}

static inline void Jetpack_SetFloat(uint8_t* pPlayer, const ptrdiff_t off, const float v)
{
	float* const p = reinterpret_cast<float*>(pPlayer + off);
	if (*p == v)
		return;
	*p = v;
	MarkEntityEdictDirty(pPlayer);
}

// Speed at which an acceleration balances the exponential tightness pull.
static inline float Jetpack_Terminal(const float flAccel, const float flTightness, const float dt)
{
	return (flAccel * dt) / (1.0f - expf((-JP_LN2 / flTightness) * dt));
}

void Jetpack_ClearGround(uint8_t* pPlayer)
{
	TriggerPass_SetGroundEntityNull(pPlayer);

	uint32_t* const pFlags = reinterpret_cast<uint32_t*>(pPlayer + JP_PLAYER_OFF_FLAGS);
	const uint32_t nFlags = *pFlags & ~static_cast<uint32_t>(JP_FL_ONGROUND);
	if (*pFlags == nFlags)
		return;

	using StateChangedFn_t = void(__fastcall*)(void* pEnt, void* pVar);
	void** const pVtbl = *reinterpret_cast<void***>(pPlayer);
	reinterpret_cast<StateChangedFn_t>(pVtbl[JP_ENT_VTBL_STATECHANGED / sizeof(void*)])(pPlayer, pFlags);
	*pFlags = nFlags;
}

static void Jetpack_Stop(uint8_t* pPlayer, float* vel, const char* pszWhy)
{
	if (!pPlayer[JP_PLAYER_OFF_JETPACK])
		return;

	Jetpack_SetByte(pPlayer, JP_PLAYER_OFF_JETPACK, 0);

	const uint8_t* const pSet = Jetpack_Settings(pPlayer);
	const float flTerm = Jetpack_Terminal((Jetpack_F(pSet, JPF_THRUST) - 1.0f) * Jetpack_Gravity(),
		Jetpack_F(pSet, JPF_TIGHTNESS_UP), Jetpack_Dt());

	JetpackState& st = s_jetpackMap[pPlayer];
	const float flVzIn = vel[2];
	if (flTerm >= vel[2])
	{
		vel[2] *= fminf(1.0f, Jetpack_F(pSet, JPF_VVSCALE_ON_DEACTIVATION));
		Jetpack_SetFloat(pPlayer, JP_PLAYER_OFF_GRAVITY, Jetpack_F(pSet, JPF_POSTEFFECT_GRAVITY));
		st.m_bPostGravity = true;
	}

	st.m_flDeactivateTime = Jetpack_Now();
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_jetpackDeactivateTime), st.m_flDeactivateTime);
	MarkEntityEdictDirty(pPlayer);

	if (bridge_jetpack_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[JETPACK] stop (%s) player=%p vz %.1f -> %.1f term=%.1f grav=%.3f\n",
			pszWhy, pPlayer, flVzIn, vel[2], flTerm,
			*reinterpret_cast<const float*>(pPlayer + JP_PLAYER_OFF_GRAVITY));
}

// S3 CPlayer offsets, S21 ApplyJetpack math.
static void Jetpack_Apply(uint8_t* pPlayer, float* vel,
	const float* moveFwd, const float* moveRight, const float* moveUp,
	const float* lookFwd, const float* lookRight, const float* lookUp)
{
	if (!pPlayer[JP_PLAYER_OFF_JETPACK])
		return;

	const uint8_t* const pSet = Jetpack_Settings(pPlayer);
	CBaseEntity* const pEnt = reinterpret_cast<CBaseEntity*>(pPlayer);

	if (pEnt->Diag_LifeState() != 0)
		return Jetpack_Stop(pPlayer, vel, "dead");
	if (pEnt->Diag_HasMoveParent())
		return Jetpack_Stop(pPlayer, vel, "parented");
	if (pPlayer[JP_PLAYER_OFF_HOVERING])
		return Jetpack_Stop(pPlayer, vel, "hovering");
	if (pPlayer[JP_PLAYER_OFF_GRAPPLEACTIVE] && pPlayer[JP_PLAYER_OFF_GRAPPLEATTACH])
		return Jetpack_Stop(pPlayer, vel, "grapple");

	// S21 runs the jets' deactivate callback inside the releasing command's
	// offhand frame; S3 defers it past movement, so read the holster here.
	const bool bScript = Jetpack_B(pSet, JPF_SCRIPT_TO_ACTIVATE);
	if (bScript && !pPlayer[JP_PLAYER_OFF_ACTIVATEJET])
		return Jetpack_Stop(pPlayer, vel, "script");
	if (bScript && OffhandJumpToggle_IsReleasing(pPlayer))
		return Jetpack_Stop(pPlayer, vel, "release");

	const float now = Jetpack_Now();
	const int nButtons = *reinterpret_cast<const int*>(pPlayer + JP_PLAYER_OFF_BUTTONS);
	const bool bJumpHeld = (nButtons & JP_IN_JUMP) != 0;
	const bool bAltHand = Jetpack_HandleLive(pPlayer, JP_PLAYER_OFF_ACTIVEWEAP_ALT);
	const float flActivateTime = *reinterpret_cast<const float*>(pPlayer + JP_PLAYER_OFF_ACTIVATETIME);
	const float flJumpPressTime = *reinterpret_cast<const float*>(pPlayer + JP_PLAYER_OFF_JUMPPRESSTIME);

	const bool bToggleHeld = Jetpack_B(pSet, JPF_TOGGLE_ON)
		&& (!Jetpack_B(pSet, JPF_TOGGLE_OFF) || now - flActivateTime <= 0.25f || now - flJumpPressTime > 0.25f);
	if (!(bJumpHeld || bAltHand || bToggleHeld || bScript))
		return Jetpack_Stop(pPlayer, vel, "released");
	if (pPlayer[JP_PLAYER_OFF_MOVETYPE] != JP_MOVETYPE_WALK)
		return Jetpack_Stop(pPlayer, vel, "movetype");
	if (v_Player_IsZiplining && v_Player_IsZiplining(pPlayer))
		return Jetpack_Stop(pPlayer, vel, "zipline");

	const float flMeter = *reinterpret_cast<const float*>(pPlayer + JP_PLAYER_OFF_GLIDEMETER);
	if (flMeter <= 0.0f && Jetpack_F(pSet, JPF_METER_RATE) > 0.0f)
		return Jetpack_Stop(pPlayer, vel, "fuel");

	const float flBaseThrust = Jetpack_F(pSet, JPF_THRUST);
	const float flHorizThrust = Jetpack_F(pSet, JPF_HORIZONTAL_MOVE_THRUST);
	const bool bAfterburner = pPlayer[JP_PLAYER_OFF_AFTERBURNER] != 0;
	float flThrust = bAfterburner ? Jetpack_F(pSet, JPF_AFTERBURNER_THRUST) : flBaseThrust;
	float flStrafeThrust = flHorizThrust;

	const float fwdMove = *reinterpret_cast<const float*>(pPlayer + JP_PLAYER_OFF_FORWARDMOVE);
	const float sideMove = *reinterpret_cast<const float*>(pPlayer + JP_PLAYER_OFF_SIDEMOVE);
	{
		const float wx = fwdMove * moveFwd[0] + sideMove * moveRight[0];
		const float wy = fwdMove * moveFwd[1] + sideMove * moveRight[1];
		const float wz = fwdMove * moveFwd[2] + sideMove * moveRight[2];
		const float flWish = fminf(1.0f, sqrtf(wx * wx + wy * wy + wz * wz));
		if (flHorizThrust >= 0.0f && flWish > 0.0001f)
			flThrust = (flHorizThrust - flBaseThrust) * flWish + flBaseThrust;
	}

	const float flRampTime = Jetpack_F(pSet, JPF_THRUST_SCALE_TIME_ON_ACTIVATION);
	const float flSinceActivate = now - flActivateTime;
	if (flRampTime > 0.0f && flRampTime >= flSinceActivate)
	{
		const float flScaleOn = Jetpack_F(pSet, JPF_THRUST_SCALE_ON_ACTIVATION);
		const float flScale = (flSinceActivate / flRampTime) * (1.0f - flScaleOn) + flScaleOn;
		flThrust *= flScale;
		flStrafeThrust = flHorizThrust * flScale;
	}

	if (bAltHand)
	{
		flThrust = 1.0f;
		flStrafeThrust = 1.0f;
	}
	if (pPlayer[JP_PLAYER_OFF_ZOOMING])
		flThrust = 1.0f;

	const bool bPitch = Jetpack_B(pSet, JPF_PITCH_MOVEMENT);
	const float flTightUp = Jetpack_F(pSet, JPF_TIGHTNESS_UP);
	const float flTightDown = Jetpack_F(pSet, JPF_TIGHTNESS_DOWN);
	const float flTightSide = Jetpack_F(pSet, JPF_TIGHTNESS_SIDEWAYS);
	const float flTightFB = Jetpack_F(pSet, JPF_TIGHTNESS_FORWARDBACK);
	const float flStrafe = Jetpack_F(pSet, JPF_STRAFE);
	const float g = Jetpack_Gravity();
	const float dt = Jetpack_Dt();
	const float velIn[3] = { vel[0], vel[1], vel[2] };

	const bool bBoundToJump = Jetpack_B(pSet, JPF_AFTERBURNER_BOUND_TO_JUMP);
	if (!bBoundToJump && (bJumpHeld || bAltHand || bScript))
		vel[2] += g * flThrust * dt;
	if (bAfterburner && !bBoundToJump && (nButtons & JP_IN_AFTERBURN_DOWN) && !bJumpHeld)
		vel[2] -= g * flThrust * dt;

	const float* const up = bPitch ? lookUp : moveUp;
	{
		const float d = Jetpack_Dot(vel, up);
		const float k = expf((-JP_LN2 / ((d > 0.0f) ? flTightUp : flTightDown)) * dt) * d - d;
		vel[0] += up[0] * k;
		vel[1] += up[1] * k;
		vel[2] += up[2] * k;
	}

	// Entering faster upward than the thrust can hold decays the entry speed instead.
	{
		const float dIn = Jetpack_Dot(velIn, up);
		if (dIn > 0.0f && dIn - Jetpack_Terminal((flThrust - 1.0f) * g, flTightUp, dt) > 0.01f)
		{
			const float e = expf((-JP_LN2 / JP_TIGHTNESS_ABOVE_LIMIT) * dt) - 1.0f;
			vel[0] = velIn[0] + up[0] * dIn * e;
			vel[1] = velIn[1] + up[1] * dIn * e;
			vel[2] = velIn[2] + up[2] * dIn * e + g * dt;
		}
	}

	const bool bMoving = fwdMove != 0.0f || sideMove != 0.0f;

	const float* const right = bPitch ? lookRight : moveRight;
	bool bOverSide;
	{
		const float d = Jetpack_Dot(vel, right);
		const float flLimit = bMoving ? Jetpack_Terminal(flStrafeThrust * flStrafe, flTightSide, dt) : 0.0f;
		bOverSide = fabsf(d) > flLimit;
		const float k = expf((-JP_LN2 / (bOverSide ? JP_TIGHTNESS_ABOVE_LIMIT : flTightSide)) * dt) * d - d;
		vel[0] += right[0] * k;
		vel[1] += right[1] * k;
		vel[2] += right[2] * k;
	}

	const float* const fwd = bPitch ? lookFwd : moveFwd;
	bool bOverFwd;
	{
		const float d = Jetpack_Dot(vel, fwd);
		const float flLimit = bMoving ? Jetpack_Terminal(flStrafeThrust * flStrafe, flTightFB, dt) : 0.0f;
		bOverFwd = fabsf(d) > flLimit;
		const float k = expf((-JP_LN2 / (bOverFwd ? JP_TIGHTNESS_ABOVE_LIMIT : flTightFB)) * dt) * d - d;
		vel[0] += fwd[0] * k;
		vel[1] += fwd[1] * k;
		vel[2] += fwd[2] * k;
	}

	const float flFwdInput = bAfterburner ? 1.0f : fwdMove;
	float w[3];
	for (int i = 0; i < 3; ++i)
		w[i] = bPitch ? (flFwdInput * lookFwd[i] + sideMove * lookRight[i])
		              : (flFwdInput * moveFwd[i] + sideMove * moveRight[i]);

	const float flWishSqr = Jetpack_Dot(w, w);
	if (flWishSqr > 0.0000010000001f)
	{
		if (flWishSqr > 1.0f)
		{
			const float inv = 1.0f / fmaxf(sqrtf(flWishSqr), FLT_MIN);
			w[0] *= inv; w[1] *= inv; w[2] *= inv;
		}

		float acc[3] = { w[0] * flStrafe, w[1] * flStrafe, w[2] * flStrafe };
		const float flTaperStart = Jetpack_F(pSet, JPF_STRAFE_TAPER_START);
		const float flTaperFinish = Jetpack_F(pSet, JPF_STRAFE_TAPER_FINISH);
		if (flTaperFinish > flTaperStart)
		{
			const float s = Jetpack_Dot(vel, w);
			float flTaper = 1.0f;
			if (s > flTaperStart && s < flTaperFinish)
				flTaper = 1.0f - (s - flTaperStart) / (flTaperFinish - flTaperStart);
			else if (s >= flTaperFinish)
				flTaper = 0.0f;
			acc[0] *= flTaper; acc[1] *= flTaper; acc[2] *= flTaper;
		}

		// Over-limit axes lose their strafe push; these use the move basis even with pitch movement.
		if (bOverFwd)
		{
			const float p = Jetpack_Dot(moveFwd, acc);
			acc[0] -= p * moveFwd[0]; acc[1] -= p * moveFwd[1]; acc[2] -= p * moveFwd[2];
		}
		if (bOverSide)
		{
			const float p = Jetpack_Dot(moveRight, acc);
			acc[0] -= p * moveRight[0]; acc[1] -= p * moveRight[1]; acc[2] -= p * moveRight[2];
		}

		vel[0] += acc[0] * flStrafeThrust * dt;
		vel[1] += acc[1] * flStrafeThrust * dt;
		vel[2] += acc[2] * flStrafeThrust * dt;
	}

	Jetpack_ClearGround(pPlayer);

	const int nEvery = bridge_jetpack_diag.GetInt();
	if (nEvery > 0 && (++s_nApplyTick % static_cast<uint32_t>(nEvery)) == 0)
		Msg(eDLL_T::SERVER,
			"[JETPACK] apply player=%p t=%.3f thrust=%.3f strafe=%.3f vel=(%.1f %.1f %.1f) meter=%.3f alt=%d zoom=%d\n",
			pPlayer, flSinceActivate, flThrust, flStrafeThrust, vel[0], vel[1], vel[2], flMeter,
			bAltHand ? 1 : 0, pPlayer[JP_PLAYER_OFF_ZOOMING] ? 1 : 0);
}

// The release post effect: reduced gravity and air drag for jetpackPostEffectTime,
// cancelled by the ground or anything solid within 50 units below.
static bool Jetpack_PostEffectActive(uint8_t* pPlayer, const JetpackState& st)
{
	if (pPlayer[JP_PLAYER_OFF_JETPACK] || st.m_flDeactivateTime < 0.0f)
		return false;
	if (Jetpack_F(Jetpack_Settings(pPlayer), JPF_POSTEFFECT_TIME) <= Jetpack_Now() - st.m_flDeactivateTime)
		return false;
	if (*reinterpret_cast<const int*>(pPlayer + JP_PLAYER_OFF_GROUNDENT) != -1)
		return false;

	TriggerPass_EnsureAbsOrigin(pPlayer);
	const Vector3D& origin = reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_AbsOrigin();
	const Vector3D end(origin.x, origin.y, origin.z - JP_POSTEFFECT_PROBE);
	trace_t tr{};
	if (!TriggerGravity_TraceLine(origin, end, JP_MASK_PLAYERSOLID, pPlayer, 0, &tr, 0, 0))
		return true;
	return tr.fraction >= 1.0f && !tr.startsolid && !tr.allsolid;
}

static void Jetpack_PostEffectTick(uint8_t* pPlayer)
{
	JetpackState* const pState = s_jetpackMap.Find(pPlayer);
	if (!pState)
		return;

	pState->m_bPostEffect = Jetpack_PostEffectActive(pPlayer, *pState);
	if (pState->m_bPostGravity
		&& (!pState->m_bPostEffect || !Jetpack_B(Jetpack_Settings(pPlayer), JPF_ENABLED)))
	{
		Jetpack_SetFloat(pPlayer, JP_PLAYER_OFF_GRAVITY, 1.0f);
		pState->m_bPostGravity = false;
	}
}

bool Jetpack_IsZiplining(void* pPlayer)
{
	return v_Player_IsZiplining && v_Player_IsZiplining(pPlayer);
}

static bool Jetpack_Enabled(void)
{
	return bridge_jetpack.GetBool() && Jetpack_ResolveFields();
}

static bool Hook_Jetpack_CanUse(void* pPlayer, const float* vel)
{
	// A script-activated jetpack engages on m_activateJetpack alone; the script ran CanUseJetpack itself.
	if (pPlayer && pPlayer == s_pForceCanUse)
		return static_cast<const uint8_t*>(pPlayer)[JP_PLAYER_OFF_JETPACK] == 0
			&& !OffhandJumpToggle_IsReleasing(pPlayer);
	if (!v_Jetpack_CanUse(pPlayer, vel))
		return false;
	if (!pPlayer || !Jetpack_Enabled())
		return true;

	// S21 refuses within 0.1 s of the jump press before this one; the S3 player
	// has no such field, so read the value the wire carries.
	if (Jetpack_Now() - PlayerExtend_GetF32(pPlayer, offsetof(PlayerExtendWire, m_prevJumpPressTime)) <= 0.1f)
		return false;

	// S21 also wants the jump pressed on this command when the jetpack takes
	// more than one jump; S3 only waits out boostOnGroundSafety, so a held
	// first jump would pass.
	const uint8_t* const p = static_cast<const uint8_t*>(pPlayer);
	const uint8_t* const pSet = Jetpack_Settings(p);
	if (p[JP_PLAYER_OFF_BOOSTED] && p[JP_PLAYER_OFF_BOOSTED + 2] && Jetpack_B(pSet, JPF_REPEATED_BOOSTS_SHORT))
		return true;
	if (Jetpack_I(pSet, JPF_JUMPS_TO_ACTIVATE) <= 1)
		return true;
	return (*reinterpret_cast<const int*>(p + JP_PLAYER_OFF_PRESSED) & JP_IN_JUMP) != 0;
}

static void Hook_Jetpack_Check(void* pPlayer, void* pUnused, float* vel)
{
	if (!pPlayer || !vel || !Jetpack_Enabled())
		return v_Jetpack_Check(pPlayer, pUnused, vel);

	uint8_t* const p = static_cast<uint8_t*>(pPlayer);
	const uint8_t* const pSet = Jetpack_Settings(p);
	const bool bWas = p[JP_PLAYER_OFF_JETPACK] != 0;
	const float flVz = vel[2];

	s_pForceCanUse = Jetpack_B(pSet, JPF_SCRIPT_TO_ACTIVATE) ? pPlayer : nullptr;
	v_Jetpack_Check(pPlayer, pUnused, vel);
	s_pForceCanUse = nullptr;

	if (bWas || !p[JP_PLAYER_OFF_JETPACK])
		return;

	vel[2] = flVz * fminf(Jetpack_F(pSet, JPF_VVSCALE_ON_ACTIVATION), 1.0f);

	const float flMeter = *reinterpret_cast<const float*>(p + JP_PLAYER_OFF_GLIDEMETER);
	Jetpack_SetFloat(p, JP_PLAYER_OFF_GLIDEMETER, flMeter - Jetpack_F(pSet, JPF_METER_ON_ACTIVATION));

	JetpackState& st = s_jetpackMap[pPlayer];
	Jetpack_SetFloat(p, JP_PLAYER_OFF_GRAVITY, 1.0f);
	st.m_bPostGravity = false;
	st.m_bPostEffect = false;

	if (bridge_jetpack_diag.GetInt() > 0)
		Msg(eDLL_T::SERVER, "[JETPACK] start player=%p vz %.1f -> %.1f meter %.3f -> %.3f\n",
			pPlayer, flVz, vel[2], flMeter, *reinterpret_cast<const float*>(p + JP_PLAYER_OFF_GLIDEMETER));
}

static void Hook_Jetpack_Apply(void* pPlayer, float* vel, const float* moveFwd, const float* moveRight,
	const float* moveUp, const float* lookFwd, const float* lookRight, const float* lookUp)
{
	if (!pPlayer || !vel || !Jetpack_Enabled())
		return v_Jetpack_Apply(pPlayer, vel, moveFwd, moveRight, moveUp, lookFwd, lookRight, lookUp);

	uint8_t* const p = static_cast<uint8_t*>(pPlayer);
	Jetpack_Apply(p, vel, moveFwd, moveRight, moveUp, lookFwd, lookRight, lookUp);
	Jetpack_PostEffectTick(p);
}

// The S3 recharge gate reads glideRechargeOnlyWhenGrounded, which the S21 layout
// replaces with two flags (either one blocks airborne recharge); it is pointed
// at whichever of them is set for this player.
static uint32_t Jetpack_RechargeGroundedOffset(const uint8_t* pSettings)
{
	const uint32_t nFeet = s_nFieldOff[JPF_RECHARGE_WHEN_FEET_ON_GROUND];
	const uint32_t nState = s_nFieldOff[JPF_RECHARGE_IN_GROUNDED_STATE];
	if (nFeet != JP_FIELD_MISSING && Jetpack_B(pSettings, JPF_RECHARGE_WHEN_FEET_ON_GROUND))
		return nFeet;
	return nState;
}

static int64_t Hook_Jetpack_UpdateMeter(void* pPlayer, char bGrounded)
{
	if (!pPlayer || !Jetpack_Enabled())
		return v_Jetpack_UpdateMeter(pPlayer, bGrounded);

	const uint8_t* const p = static_cast<const uint8_t*>(pPlayer);
	const uint8_t* const pSet = Jetpack_Settings(p);

	// Holding the tactical in the alt hand drains at jetpackTacticalMeterRate.
	const bool bTactical = s_pMeterRateFinder
		&& p[JP_PLAYER_OFF_JETPACK] && !p[JP_PLAYER_OFF_AFTERBURNER]
		&& s_nFieldOff[JPF_TACTICAL_METER_RATE] != JP_FIELD_MISSING
		&& Jetpack_F(pSet, JPF_TACTICAL_METER_RATE) >= 0.0f
		&& (*reinterpret_cast<const int*>(p + JP_PLAYER_OFF_BUTTONS) & JP_IN_OFFHAND1)
		&& Jetpack_HandleLive(p, JP_PLAYER_OFF_ACTIVEWEAP_ALT);
	const uint32_t nGroundedOff = Jetpack_RechargeGroundedOffset(pSet);
	const bool bGroundedSwap = s_pRechargeGroundedFinder && pSet && nGroundedOff != JP_FIELD_MISSING;

	const uint32_t nSavedRate = s_pMeterRateFinder ? *s_pMeterRateFinder : 0;
	const uint32_t nSavedGrounded = s_pRechargeGroundedFinder ? *s_pRechargeGroundedFinder : 0;
	if (bTactical)
		*s_pMeterRateFinder = s_nFieldOff[JPF_TACTICAL_METER_RATE];
	if (bGroundedSwap)
		*s_pRechargeGroundedFinder = nGroundedOff;

	const int64_t ret = v_Jetpack_UpdateMeter(pPlayer, bGrounded);

	if (bTactical)
		*s_pMeterRateFinder = nSavedRate;
	if (bGroundedSwap)
		*s_pRechargeGroundedFinder = nSavedGrounded;
	return ret;
}

// The S3 aux reset refills the fuel meter for glide and hover only and zeroes
// it otherwise; the S21 meter serves the jetpack too, so a jetpack-only setfile
// spawned empty.
static int64_t Hook_Jetpack_ResetAux(void* pPlayer)
{
	const int64_t ret = v_Jetpack_ResetAux(pPlayer);
	if (!pPlayer || !Jetpack_Enabled())
		return ret;

	uint8_t* const p = static_cast<uint8_t*>(pPlayer);
	const uint8_t* const pSet = Jetpack_Settings(p);
	if (Jetpack_B(pSet, JPF_ENABLED) && !Jetpack_B(pSet, JPF_GLIDE_ENABLED) && !Jetpack_B(pSet, JPF_HOVER_ENABLED))
		Jetpack_SetFloat(p, JP_PLAYER_OFF_GLIDEMETER, Jetpack_F(pSet, JPF_GLIDE_DURATION));

	JetpackState* const pState = s_jetpackMap.Find(pPlayer);
	if (pState && pState->m_bPostGravity)
	{
		Jetpack_SetFloat(p, JP_PLAYER_OFF_GRAVITY, 1.0f);
		pState->m_bPostGravity = false;
	}
	return ret;
}

bool Jetpack_AirMoveBegin(void* pPlayer)
{
	if (!pPlayer || !s_pAirDragFinder || s_bAirDragSwapped || !Jetpack_Enabled())
		return false;
	if (s_nFieldOff[JPF_POSTEFFECT_DRAG] == JP_FIELD_MISSING)
		return false;

	JetpackState* const pState = s_jetpackMap.Find(pPlayer);
	if (!pState || !pState->m_bPostEffect)
		return false;

	s_nSavedAirDragOff = *s_pAirDragFinder;
	*s_pAirDragFinder = s_nFieldOff[JPF_POSTEFFECT_DRAG];
	s_bAirDragSwapped = true;
	return true;
}

void Jetpack_AirMoveEnd(void)
{
	if (!s_bAirDragSwapped)
		return;
	*s_pAirDragFinder = s_nSavedAirDragOff;
	s_bAirDragSwapped = false;
}

static void Jetpack_FlightLevelShutdown(void)
{
	Jetpack_AirMoveEnd();
	s_jetpackMap.Clear();
}

static void Jetpack_PatchJz(uint8_t* pJz, const bool bAttach, const char* pszWhat)
{
	if (!pJz)
		return;
	const uint8_t nWant = bAttach ? 0xEB : 0x74;
	if (*pJz == nWant)
		return;
	if (*pJz != (bAttach ? 0x74 : 0xEB))
	{
		Warning(eDLL_T::SERVER, "[JETPACK] %s patch site holds 0x%02X -- left alone\n", pszWhat, *pJz);
		return;
	}
	DWORD oldProt = 0;
	if (!VirtualProtect(pJz, 1, PAGE_EXECUTE_READWRITE, &oldProt))
		return;
	*pJz = nWant;
	VirtualProtect(pJz, 1, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), pJz, 1);
}

void VJetpackBridge::GetAdr(void) const
{
	LogFunAdr("Jetpack_Check", v_Jetpack_Check);
	LogFunAdr("Jetpack_CanUse", v_Jetpack_CanUse);
	LogFunAdr("Jetpack_Apply", v_Jetpack_Apply);
	LogFunAdr("Jetpack_UpdateMeter", v_Jetpack_UpdateMeter);
	LogFunAdr("Jetpack_ResetAux", v_Jetpack_ResetAux);
	LogFunAdr("Player_IsZiplining", v_Player_IsZiplining);
	LogVarAdr("Jetpack_AirDragFinder", s_pAirDragFinder);
	LogVarAdr("Jetpack_MeterRateFinder", s_pMeterRateFinder);
	LogVarAdr("Jetpack_RechargeGroundedFinder", s_pRechargeGroundedFinder);
}

void VJetpackBridge::GetFun(void) const
{
	// Server-half bodies: each pattern carries the literal settings-block
	// displacement 0x5F08, which is what separates them from the client twins.
	Module_FindPattern(g_GameDll,
		"40 53 55 57 48 83 EC 40 48 8B 99 08 5F 00 00 49 8B D0 49 8B E8 48 8B F9 E8")
		.GetPtr(v_Jetpack_Check);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 48 8B 99 08 5F 00 00 48 8B F9 8B 05 ?? ?? ?? ?? 80 3C 18 00 0F 84")
		.GetPtr(v_Jetpack_CanUse);
	CMemory apply = Module_FindPattern(g_GameDll,
		"40 53 56 57 41 54 41 55 48 81 EC E0 00 00 00 80 B9 5C 69 00 00 00 4D 8B E9 48 8B 99 08 5F 00 00");
	apply.GetPtr(v_Jetpack_Apply);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 57 48 83 EC 50 48 8B 99 08 5F 00 00 44 0F B6 C2 8B 05")
		.GetPtr(v_Jetpack_UpdateMeter);
	Module_FindPattern(g_GameDll,
		"80 B9 55 69 00 00 00 4C 8B C9 48 8B 81 08 5F 00 00 41 BA 00 02 00 00")
		.GetPtr(v_Jetpack_ResetAux);

	// The apply body's first call is the zipline test that cancels the jetpack.
	if (apply)
	{
		const CMemory call = apply.Offset(0x164);
		if (*call.RCast<const uint8_t*>() == 0xE8)
			call.FollowNearCall().GetPtr(v_Player_IsZiplining);
	}

	// The meter update's drain read of jetpackMeterRate; the finder global it loads.
	const CMemory meterRate = Module_FindPattern(g_GameDll,
		"40 38 AF 5C 69 00 00 0F 84 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 0F 29 74 24");
	if (meterRate)
		s_pMeterRateFinder = meterRate.Offset(0xD).ResolveRelativeAddress(0x2, 0x6).RCast<uint32_t*>();

	// The meter update's glideRechargeOnlyWhenGrounded read; the client twin loads the same finder global.
	const CMemory rechargeGrounded = Module_FindPattern(g_GameDll,
		"45 84 C0 75 10 8B 05 ?? ?? ?? ?? 0F B6 D2 40 38 2C 18 0F 45 D5");
	if (rechargeGrounded)
		s_pRechargeGroundedFinder = rechargeGrounded.Offset(0x5).ResolveRelativeAddress(0x2, 0x6).RCast<uint32_t*>();

	// AirMove's airDrag read. The client twin matches the same bytes and loads the same finder global.
	const CMemory airDrag = Module_FindPattern(g_GameDll,
		"45 0F 28 D1 8B 05 ?? ?? ?? ?? 0F 28 D3 0F 28 C3 0F 28 CB");
	if (airDrag)
		s_pAirDragFinder = airDrag.Offset(0x4).ResolveRelativeAddress(0x2, 0x6).RCast<uint32_t*>();

	// jetpackZeroVerticalVelocityOnActivation / ...OnDeactivation are not in the
	// S21 layout; the rescued read lands on unrelated bytes, so skip both branches.
	const CMemory zeroOn = Module_FindPattern(g_GameDll,
		"44 88 A7 55 69 00 00 8B 05 ?? ?? ?? ?? 44 38 24 18 74 04 44 89 65 08");
	if (zeroOn)
		s_pZeroVelOnActivationJz = zeroOn.Offset(0x11).RCast<uint8_t*>();
	const CMemory zeroOff = Module_FindPattern(g_GameDll,
		"84 C0 75 57 8B 05 ?? ?? ?? ?? 33 FF 40 38 3C 18 74 07 49 89 7E 04");
	if (zeroOff)
		s_pZeroVelOnDeactivationJz = zeroOff.Offset(0x10).RCast<uint8_t*>();

	if (!v_Jetpack_Check || !v_Jetpack_CanUse || !v_Jetpack_Apply || !v_Jetpack_UpdateMeter)
		Warning(eDLL_T::SERVER, "[JETPACK] pattern unresolved (check=%d canuse=%d apply=%d meter=%d) -- S3 jetpack stays\n",
			v_Jetpack_Check ? 1 : 0, v_Jetpack_CanUse ? 1 : 0, v_Jetpack_Apply ? 1 : 0, v_Jetpack_UpdateMeter ? 1 : 0);
	if (!v_Player_IsZiplining || !v_Jetpack_ResetAux || !s_pRechargeGroundedFinder || !s_pMeterRateFinder || !s_pAirDragFinder
		|| !s_pZeroVelOnActivationJz || !s_pZeroVelOnDeactivationJz)
		Warning(eDLL_T::SERVER, "[JETPACK] partial resolve (zip=%d reset=%d grounded=%d meterRate=%d airDrag=%d zeroOn=%d zeroOff=%d)\n",
			v_Player_IsZiplining ? 1 : 0, v_Jetpack_ResetAux ? 1 : 0, s_pRechargeGroundedFinder ? 1 : 0, s_pMeterRateFinder ? 1 : 0, s_pAirDragFinder ? 1 : 0,
			s_pZeroVelOnActivationJz ? 1 : 0, s_pZeroVelOnDeactivationJz ? 1 : 0);
}

void VJetpackBridge::Detour(const bool bAttach) const
{
	if (!v_Jetpack_Check || !v_Jetpack_CanUse || !v_Jetpack_Apply || !v_Jetpack_UpdateMeter)
		return;

	DetourSetup(&v_Jetpack_Check, &Hook_Jetpack_Check, bAttach);
	DetourSetup(&v_Jetpack_CanUse, &Hook_Jetpack_CanUse, bAttach);
	DetourSetup(&v_Jetpack_Apply, &Hook_Jetpack_Apply, bAttach);
	DetourSetup(&v_Jetpack_UpdateMeter, &Hook_Jetpack_UpdateMeter, bAttach);
	if (v_Jetpack_ResetAux)
		DetourSetup(&v_Jetpack_ResetAux, &Hook_Jetpack_ResetAux, bAttach);

	Jetpack_PatchJz(s_pZeroVelOnActivationJz, bAttach, "zero-velocity-on-activation");
	Jetpack_PatchJz(s_pZeroVelOnDeactivationJz, bAttach, "zero-velocity-on-deactivation");
}
