//=============================================================================//
//
// Purpose: Nitro Gate launch. See slide_gate_launch.h.
//
//=============================================================================//
#include "core/stdafx.h"

#include "slide_gate_launch.h"
#include "player.h"
#include "vscript_server.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/edict_dirty.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "mathlib/mathlib.h"
#include <cmath>

extern CGlobalVars* gpGlobals;

// CGameMovement ctx / CMoveData (same layout mantle_boost.cpp established).
static constexpr ptrdiff_t SGL_CTX_OFF_PLAYER    = 8;
static constexpr ptrdiff_t SGL_CTX_OFF_MOVEDATA  = 16;
static constexpr ptrdiff_t SGL_MV_OFF_FORWARD2D  = 96;   // float[3], view forward flattened
static constexpr ptrdiff_t SGL_MV_OFF_VELOCITY   = 304;  // float[3]
static constexpr ptrdiff_t SGL_PLAYER_OFF_PREV_GROUND_NORMAL = 0x724C; // float[3]
static constexpr ptrdiff_t SGL_PLAYER_OFF_DUCK_TOGGLE = 0x5AA8;  // bool m_Local.m_duckToggleOn
static constexpr ptrdiff_t SGL_PLAYER_OFF_FORCE_STANCE = 0x5AAC; // int m_Local.m_forceStance

// The server build's ForceSlide stance. The client bridge decodes it into S21
// m_forceSlide, so both movement twins hold the slide while the boost runs.
static constexpr int SGL_FORCE_STANCE_SLIDE = 3;
static constexpr float SGL_FORCED_SLIDE_MAX = 8.0f;

static constexpr float SGL_PENDING_TTL = 0.5f;
static constexpr float SGL_MAX_LAUNCH_POWER = 4000.0f;

struct SlideGateLaunchState
{
	bool  m_pending = false;
	float m_power = 0.0f;
	float m_requestTime = 0.0f;
	float m_debounceExpire = 0.0f;
	bool  m_forcedSlide = false;
	float m_forcedSlideExpire = 0.0f;
};
static SDKEntityMap<SlideGateLaunchState> s_states(ESide::Server, "slideGateLaunch.srv");

static ConVar bridge_slide_gate_debounce("bridge_slide_gate_debounce", "1.0", FCVAR_RELEASE,
	"[SLIDEGATE] Seconds after a Nitro Gate launch before the same player can be launched again.", true, 0.f, true, 30.f);
static ConVar bridge_slide_gate_trace("bridge_slide_gate_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[SLIDEGATE] Log every launch request and whether the slide began.");

static void (__fastcall* v_CGameMovement__TryBeginSlide)(void* ctx, const float* pGroundNormal, char bFromAir) = nullptr;

static inline float SGL_CurTime(void)
{
	return gpGlobals ? gpGlobals->curTime : 0.0f;
}

static void SGL_BeginForcedSlide(void* pPlayer, SlideGateLaunchState& s, const float flNow)
{
	int* const pForceStance = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pPlayer) + SGL_PLAYER_OFF_FORCE_STANCE);
	if (*pForceStance != 0 && *pForceStance != SGL_FORCE_STANCE_SLIDE)
		return;   // a forced stand or crouch owns the stance

	*pForceStance = SGL_FORCE_STANCE_SLIDE;
	s.m_forcedSlide = true;
	s.m_forcedSlideExpire = flNow + SGL_FORCED_SLIDE_MAX;
	MarkEntityEdictDirty(pPlayer);
}

// The forced slide latches the duck toggle on both twins; clearing it here is
// what lets the player stand when the boost ends.
static void SGL_EndForcedSlide(void* pPlayer, SlideGateLaunchState& s)
{
	if (!s.m_forcedSlide)
		return;
	s.m_forcedSlide = false;

	const uintptr_t p = reinterpret_cast<uintptr_t>(pPlayer);
	int* const pForceStance = reinterpret_cast<int*>(p + SGL_PLAYER_OFF_FORCE_STANCE);
	if (*pForceStance == SGL_FORCE_STANCE_SLIDE)
		*pForceStance = 0;
	*reinterpret_cast<uint8_t*>(p + SGL_PLAYER_OFF_DUCK_TOGGLE) = 0;
	MarkEntityEdictDirty(pPlayer);

	if (bridge_slide_gate_trace.GetBool())
		Msg(eDLL_T::SERVER, "[SLIDEGATE] forced slide end player=%p t=%.3f\n", pPlayer, SGL_CurTime());
}

//-----------------------------------------------------------------------------
// Script: void Player.EndSlideGateLaunch()
// Releases the slide the launch forced. Safe to call when no launch is active.
//-----------------------------------------------------------------------------
static SQRESULT Script_EndSlideGateLaunch(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SlideGateLaunchState* const s = s_states.Find(pPlayer);
	if (s)
		SGL_EndForcedSlide(pPlayer, *s);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Script: bool Player.ApplySlideGateLaunch( float launchPower )
// Queues the launch for the player's next movement pass. False when the
// previous launch is still inside the debounce window.
//-----------------------------------------------------------------------------
static SQRESULT Script_ApplySlideGateLaunch(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQFloat flPower = 0.0f;
	if (SQ_FAILED(sq_getfloat(v, 2, &flPower)) || !std::isfinite(flPower) || flPower <= 0.0f || flPower > SGL_MAX_LAUNCH_POWER)
	{
		Warning(eDLL_T::SERVER, "[SLIDEGATE] ApplySlideGateLaunch rejected launch power %f\n", static_cast<float>(flPower));
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const float flNow = SGL_CurTime();
	SlideGateLaunchState& s = s_states[pPlayer];
	const bool bAllowed = flNow >= s.m_debounceExpire;
	if (bAllowed)
	{
		s.m_pending = true;
		s.m_power = static_cast<float>(flPower);
		s.m_requestTime = flNow;
	}

	if (bridge_slide_gate_trace.GetBool())
		Msg(eDLL_T::SERVER, "[SLIDEGATE] request player=%p power=%.1f accepted=%d t=%.3f\n", pPlayer, static_cast<float>(flPower), bAllowed ? 1 : 0, flNow);

	sq_pushbool(v, bAllowed);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void SlideGateLaunch_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction("ApplySlideGateLaunch", "Script_ApplySlideGateLaunch",
		"Launches the player along their travel direction and begins a slide on the next movement pass; false while debounced",
		"bool", "float launchPower", false, Script_ApplySlideGateLaunch);

	playerStruct->AddFunction("EndSlideGateLaunch", "Script_EndSlideGateLaunch",
		"Releases the slide a Nitro Gate launch forced, so the player can stand again",
		"void", "", false, Script_EndSlideGateLaunch);
}

void SlideGateLaunch_BeginFullWalkMove(void* ctx)
{
	if (!ctx)
		return;

	void* const pPlayer = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ctx) + SGL_CTX_OFF_PLAYER);
	const uintptr_t mv = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(ctx) + SGL_CTX_OFF_MOVEDATA);
	if (!pPlayer || !mv)
		return;

	SlideGateLaunchState* const s = s_states.Find(pPlayer);
	if (!s)
		return;

	const float flNow = SGL_CurTime();
	if (s->m_forcedSlide && flNow >= s->m_forcedSlideExpire)
		SGL_EndForcedSlide(pPlayer, *s);

	if (!s->m_pending)
		return;

	s->m_pending = false;
	if (flNow - s->m_requestTime > SGL_PENDING_TTL)
		return;

	float* const vel = reinterpret_cast<float*>(mv + SGL_MV_OFF_VELOCITY);
	float dirX = vel[0];
	float dirY = vel[1];
	float len = sqrtf(dirX * dirX + dirY * dirY);
	if (len < 1.0f)
	{
		const float* const fwd = reinterpret_cast<const float*>(mv + SGL_MV_OFF_FORWARD2D);
		dirX = fwd[0];
		dirY = fwd[1];
		len = sqrtf(dirX * dirX + dirY * dirY);
	}
	if (len < 0.001f)
		return;

	vel[0] = dirX / len * s->m_power;
	vel[1] = dirY / len * s->m_power;
	vel[2] = 0.0f;

	s->m_debounceExpire = flNow + bridge_slide_gate_debounce.GetFloat();

	if (v_CGameMovement__TryBeginSlide)
	{
		const float* const pGroundNormal = reinterpret_cast<const float*>(
			reinterpret_cast<uintptr_t>(pPlayer) + SGL_PLAYER_OFF_PREV_GROUND_NORMAL);
		v_CGameMovement__TryBeginSlide(ctx, pGroundNormal, 0);
	}
	SGL_BeginForcedSlide(pPlayer, *s, flNow);

	static bool s_bFirst = true;
	if (s_bFirst || bridge_slide_gate_trace.GetBool())
	{
		s_bFirst = false;
		Msg(eDLL_T::SERVER, "[SLIDEGATE] launch player=%p power=%.1f sliding=%d t=%.3f\n",
			pPlayer, s->m_power, static_cast<CPlayer*>(pPlayer)->IsSliding() ? 1 : 0, flNow);
	}
}

void VSlideGateLaunch::GetAdr(void) const
{
	LogFunAdr("CGameMovement::TryBeginSlide", v_CGameMovement__TryBeginSlide);
}

void VSlideGateLaunch::GetFun(void) const
{
	// Duck's slide-start call: add rdx, m_vPrevGroundNormal; xor r8d, r8d; mov rcx, rdi; call TryBeginSlide.
	// The movement class has a byte-identical client half, so the pattern anchors on the server-only
	// player offset in the add and follows the call.
	Module_FindPattern(g_GameDll, "48 81 C2 4C 72 00 00 45 33 C0 48 8B CF E8 ?? ?? ?? ??")
		.OffsetSelf(0xD)
		.FollowNearCallSelf()
		.GetPtr(v_CGameMovement__TryBeginSlide);

	if (!v_CGameMovement__TryBeginSlide)
		Warning(eDLL_T::SERVER, "[SLIDEGATE] CGameMovement::TryBeginSlide unresolved -- Nitro Gate launches without a slide\n");
}
