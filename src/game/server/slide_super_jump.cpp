//=============================================================================//
//
// Purpose: slide super-jump twin for the dedicated server.
//
// The S21 client's Jump grants a second jump when the player's settings carry
// player_slideSuperJumpEnabled (the "boosted_slide_jump" mod): after a jump
// taken from a slide, a jump press within 0.3s of leaving the ground adds
// up * sqrt(2 * gravity * slideSuperJumpHeight) once per slideSuperJumpCooldown.
// The S3 Jump returns 0 for any airborne press, so the twin evaluates the same
// gates around the native and applies the same impulse to the move velocity.
//
//=============================================================================//
#include "core/stdafx.h"

#include "slide_super_jump.h"
#include "skydive.h"
#include "player.h"
#include "game/shared/in_buttons.h"
#include "game/shared/sdk_entity_state.h"

// CGameMovement ctx / CMoveData (same layout mantle_boost.cpp established).
static constexpr ptrdiff_t SSJ_CTX_OFF_PLAYER         = 8;
static constexpr ptrdiff_t SSJ_CTX_OFF_MOVEDATA       = 16;
static constexpr ptrdiff_t SSJ_MV_OFF_BUTTONS_PRESSED = 44;   // m_nButtonsPressed
static constexpr ptrdiff_t SSJ_MV_OFF_VELOCITY        = 304;  // float[3]
static constexpr ptrdiff_t SSJ_PLAYER_OFF_GROUNDENT   = 964;  // EHANDLE, -1 = airborne

static constexpr const char* SSJ_MOD_NAME = "boosted_slide_jump";
static constexpr float SSJ_PRESS_WINDOW = 0.3f;   // seconds after leaving the ground

static char (*v_CGameMovement__Jump)(void* ctx) = nullptr;

struct SlideSuperJumpState
{
	float m_flTimeLeftGround = 0.0f;
	float m_flLastSuperJumpTime = -1000.0f;
	bool  m_bJumpedSinceGround = false;
	bool  m_bLastJumpFromSlide = false;
};
static SDKEntityMap<SlideSuperJumpState> s_states(ESide::Server, "ssj.srv");

static ConVar bridge_slide_super_jump("bridge_slide_super_jump", "1", FCVAR_RELEASE,
	"[SSJ] Server twin of the S21 slide super-jump (boosted_slide_jump mod). 0 = off.");
static ConVar bridge_slide_super_jump_height("bridge_slide_super_jump_height", "150", FCVAR_RELEASE,
	"[SSJ] Jump height (u) for the super-jump; the S21 boosted_slide_jump mod authors slideSuperJumpHeight 150.", true, 0.f, true, 1000.f);
static ConVar bridge_slide_super_jump_cooldown("bridge_slide_super_jump_cooldown", "2", FCVAR_RELEASE,
	"[SSJ] Seconds between super-jumps; the S21 base setfile authors slideSuperJumpCooldown 2.0.", true, 0.f, true, 30.f);
static ConVar bridge_slide_super_jump_trace("bridge_slide_super_jump_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[SSJ] Log every accepted super-jump and the first rejected airborne press per reason.");

static float SlideSuperJump_Gravity(void)
{
	static ConVar* s_pGravity = nullptr;
	if (!s_pGravity && g_pCVar)
		s_pGravity = g_pCVar->FindVar("sv_gravity");
	const float flGravity = s_pGravity ? s_pGravity->GetFloat() : 750.0f;
	return (isfinite(flGravity) && flGravity > 0.0f) ? flGravity : 750.0f;
}

static char __fastcall Hook_CGameMovement_Jump(void* ctx)
{
	CPlayer* const player = *reinterpret_cast<CPlayer**>(reinterpret_cast<uintptr_t>(ctx) + SSJ_CTX_OFF_PLAYER);
	const uintptr_t mv = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(ctx) + SSJ_CTX_OFF_MOVEDATA);
	if (!player || !mv)
		return v_CGameMovement__Jump(ctx);

	const bool bOnGround = *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(player) + SSJ_PLAYER_OFF_GROUNDENT) != -1;
	const bool bSliding = player->IsSliding();
	const float flCurTime = gpGlobals ? gpGlobals->curTime : 0.0f;

	const char result = v_CGameMovement__Jump(ctx);

	SlideSuperJumpState& st = s_states[player];
	if (result)
	{
		st.m_bJumpedSinceGround = true;
		st.m_bLastJumpFromSlide = bSliding;
		st.m_flTimeLeftGround = flCurTime;
		return result;
	}
	if (bOnGround)
	{
		st.m_bJumpedSinceGround = false;
		st.m_bLastJumpFromSlide = false;
		st.m_flTimeLeftGround = flCurTime;
		return result;
	}

	if (!bridge_slide_super_jump.GetBool())
		return result;
	const uint32_t nPressed = *reinterpret_cast<const uint32_t*>(mv + SSJ_MV_OFF_BUTTONS_PRESSED);
	if (!(nPressed & IN_JUMP))
		return result;

	const char* pszReject = nullptr;
	if (!st.m_bJumpedSinceGround)
		pszReject = "no jump since ground";
	else if (!st.m_bLastJumpFromSlide)
		pszReject = "last jump not from a slide";
	else if (flCurTime - st.m_flTimeLeftGround > SSJ_PRESS_WINDOW)
		pszReject = "press window expired";
	else if (flCurTime - st.m_flLastSuperJumpTime < bridge_slide_super_jump_cooldown.GetFloat())
		pszReject = "cooldown";
	else if (!SkydiveBridge_IsClassModActive(player, SSJ_MOD_NAME))
		pszReject = "mod inactive";
	if (pszReject)
	{
		if (bridge_slide_super_jump_trace.GetBool())
			DevMsg(eDLL_T::SERVER, "[SSJ] airborne jump press rejected: %s\n", pszReject);
		return result;
	}

	const float flImpulse = sqrtf(2.0f * SlideSuperJump_Gravity() * bridge_slide_super_jump_height.GetFloat());
	float* const vel = reinterpret_cast<float*>(mv + SSJ_MV_OFF_VELOCITY);
	vel[2] += flImpulse;

	st.m_flLastSuperJumpTime = flCurTime;
	st.m_bLastJumpFromSlide = false;   // the client clears its slide-jump latch on every jump

	static bool s_bFirst = true;
	if (s_bFirst || bridge_slide_super_jump_trace.GetBool())
	{
		s_bFirst = false;
		Msg(eDLL_T::SERVER, "[SSJ] super-jump impulse=%.1f vel=(%.1f %.1f %.1f) t=%.3f\n",
			flImpulse, vel[0], vel[1], vel[2], flCurTime);
	}
	return 1;
}

void VSlideSuperJumpBridge::GetAdr(void) const
{
	LogFunAdr("CGameMovement::Jump", v_CGameMovement__Jump);
}

void VSlideSuperJumpBridge::GetFun(void) const
{
	Module_FindPattern(g_GameDll, "40 55 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 51")
		.GetPtr(v_CGameMovement__Jump);
	if (!v_CGameMovement__Jump)
		Warning(eDLL_T::SERVER, "[SSJ] CGameMovement::Jump pattern unresolved -- slide super-jump twin disabled\n");
}

void VSlideSuperJumpBridge::Detour(const bool bAttach) const
{
	if (v_CGameMovement__Jump)
		DetourSetup(&v_CGameMovement__Jump, &Hook_CGameMovement_Jump, bAttach);
}
