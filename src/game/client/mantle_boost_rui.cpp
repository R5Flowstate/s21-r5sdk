//=============================================================================//
//
// Purpose: client getters for the mantle-boost timing RUI.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/memaddr.h"
#include "tier1/cvar.h"
#include "tier1/cmd.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/vsquirrel_s21.h"
#include "vscript_client.h"
#include "engine/client/net_bridge_internal.h"
#include "engine/gl_rmain.h"
#include "game/client/viewrender.h"
#include "game/client/mantle_boost.h"
#include "game/client/mantle_boost_rui.h"
#include "mathlib/mathlib.h"
#include <cmath>

static ConVar mantle_boost_ui_setting("mantle_boost_ui_setting", "3", FCVAR_ARCHIVE,
	"Mantle-boost timing indicator: 0 off, 1 minimal, 2 markers, 3 full.");

static ConVar bridge_mantle_boost_rui_log("bridge_mantle_boost_rui_log", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log the sweet-spot fraction the timing ring converges on, per traversal type.");

//-----------------------------------------------------------------------------
// C_Player fields.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t CPLAYER_OFF_TRAVERSAL_ANIM_FRAC = 0x2234; // m_traversalAnimProgress
static constexpr ptrdiff_t CPLAYER_OFF_TRAVERSAL_PROGRESS  = 0x2B7C; // m_traversalProgress
static constexpr ptrdiff_t CPLAYER_OFF_TRAVERSAL_STATE     = 0x2B34; // m_traversalState

static constexpr int TRAVERSAL_COUNT = 13;

// Local-player slot, off the same setClassVar handler the class-var natives
// anchor on. Resolved here so this file owns no cross-module state.
static uint32_t* g_pLocalPlayerSlot = nullptr;
static uintptr_t g_nPlayerArray = 0;
static bool s_bLocalPlayerResolved = false;

static constexpr size_t PLAYER_ARRAY_STRIDE = 0x20;
static constexpr ptrdiff_t PLAYER_ENTRY_SERIAL = 0x8;

static void MantleBoostRui_ResolveLocalPlayer(void)
{
	if (s_bLocalPlayerResolved)
		return;
	s_bLocalPlayerResolved = true;

	const CMemory handler = Module_FindPattern(g_GameDll, "4C 8B DC 55 56 49 8D AB");
	if (!handler.GetPtr())
	{
		Warning(eDLL_T::CLIENT, "[MB-RUI] local-player slot unresolved -- getters are inert\n");
		return;
	}

	g_pLocalPlayerSlot = handler.Offset(0x13).ResolveRelativeAddress(2, 6).RCast<uint32_t*>();
	g_nPlayerArray = handler.Offset(0x28).ResolveRelativeAddress(3, 7).GetPtr();
}

static uintptr_t MantleBoostRui_LocalPlayer(void)
{
	MantleBoostRui_ResolveLocalPlayer();
	if (!g_pLocalPlayerSlot || !g_nPlayerArray)
		return 0;

	const uint32_t nSlot = *g_pLocalPlayerSlot;
	if (nSlot == UINT32_MAX)
		return 0;

	const uintptr_t nEntry = g_nPlayerArray + (static_cast<uint16_t>(nSlot) * PLAYER_ARRAY_STRIDE);
	if (*reinterpret_cast<uint32_t*>(nEntry + PLAYER_ENTRY_SERIAL) != (nSlot >> 16))
		return 0;

	return *reinterpret_cast<uintptr_t*>(nEntry);
}

//-----------------------------------------------------------------------------
// The rendered main view: the S21 CViewRender's main logical view setup.
//-----------------------------------------------------------------------------
static uintptr_t s_pViewRenderS21 = 0;

static constexpr ptrdiff_t VIEWRENDER_MAIN_SETUP = 0x81700;
static constexpr ptrdiff_t SETUP_FORWARD         = 0x10;    // float[3]
static constexpr ptrdiff_t SETUP_UP              = 0x30;    // float[3]
static constexpr ptrdiff_t SETUP_TAN_HALF_FOV_Y  = 0x194;   // tanHalfFovX * aspectRatioYOverX

static ConVar bridge_mantle_boost_rui_offsets("bridge_mantle_boost_rui_offsets", "1",
	FCVAR_RELEASE,
	"Feed the timing ring its view-angle and crosshair offsets (0 = both stay 0).");

// pForward and pUp may be null.
static bool MantleBoostRui_MainView(Vector3D* pForward, Vector3D* pUp, float* pflTanHalfFovY)
{
	if (!s_pViewRenderS21 || !bridge_mantle_boost_rui_offsets.GetBool())
		return false;

	const uint8_t* const pSetup = reinterpret_cast<const uint8_t*>(s_pViewRenderS21 + VIEWRENDER_MAIN_SETUP);
	const float* const pFwd = reinterpret_cast<const float*>(pSetup + SETUP_FORWARD);
	const float* const pUpv = reinterpret_cast<const float*>(pSetup + SETUP_UP);
	const float flTan = *reinterpret_cast<const float*>(pSetup + SETUP_TAN_HALF_FOV_Y);
	if (!isfinite(flTan) || flTan <= 0.0f || flTan > 16.0f)
		return false;

	if (pForward)
		*pForward = Vector3D(pFwd[0], pFwd[1], pFwd[2]);
	if (pUp)
		*pUp = Vector3D(pUpv[0], pUpv[1], pUpv[2]);
	*pflTanHalfFovY = flTan;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: where the boost window OPENS, as a fraction of the proxy's traversal
// cycle -- the moment the ring should meet the brackets.
//
// Scrubs the first-person proxy's own traversal camera upward from 0.5 and
// caches the first cycle whose |delta| drops below the angle threshold.
//-----------------------------------------------------------------------------
static float s_flTraversalSweetSpotFrac[TRAVERSAL_COUNT] = { 0.0f };
static float s_flSweetSpotRetryAt[TRAVERSAL_COUNT] = { 0.0f };

static constexpr int SWEET_SPOT_SCAN_STEPS = 50;   // cycle 0.5..1.0 in 0.01 steps

static float MantleBoostRui_SweetSpotFrac(uintptr_t pPlayer)
{
	const int nTraversalState =
		*reinterpret_cast<const int*>(pPlayer + CPLAYER_OFF_TRAVERSAL_STATE);

	if (nTraversalState < 0 || nTraversalState >= TRAVERSAL_COUNT)
		return 0.0f;

	if (s_flTraversalSweetSpotFrac[nTraversalState] != 0.0f)
		return s_flTraversalSweetSpotFrac[nTraversalState];

	const float flNow = static_cast<float>(Plat_FloatTime());
	if (flNow < s_flSweetSpotRetryAt[nTraversalState])
		return 0.0f;
	s_flSweetSpotRetryAt[nTraversalState] = flNow + 1.0f;

	float flEye[3] = { 0.0f, 0.0f, 0.0f };
	if (v_C_Player_EyeAngles)
		v_C_Player_EyeAngles(reinterpret_cast<void*>(pPlayer), flEye);

	const QAngle eye(flEye[0], flEye[1], flEye[2]);
	const float flThreshold = MantleBoostClient_GetSweetSpotAngle();

	float flFound = 0.0f;
	for (int i = 0; i <= SWEET_SPOT_SCAN_STEPS; ++i)
	{
		const float flCycle = 0.5f + 0.5f * float(i) / float(SWEET_SPOT_SCAN_STEPS);
		float flDelta = 0.0f;
		if (!MantleBoostClient_SampleCameraDelta(pPlayer, flCycle, eye, &flDelta))
			break;
		if (fabsf(flDelta) < flThreshold)
		{
			flFound = flCycle;
			break;
		}
	}

	if (bridge_mantle_boost_rui_log.GetBool())
	{
		static uint16_t s_nLoggedFail = 0;
		const bool bFail = flFound <= 0.0f;
		if (!bFail || !(s_nLoggedFail & (1u << nTraversalState)))
		{
			if (bFail)
				s_nLoggedFail |= uint16_t(1u << nTraversalState);
			Msg(eDLL_T::CLIENT,
				"[MB-RUI] sweetSpotFrac travState=%d thresh=%.2f eyePitch=%.2f -> %.3f%s\n",
				nTraversalState, flThreshold, flEye[0], flFound,
				bFail ? " (no window -- ring unset; retrying at 1/s)" : "");
		}
	}

	if (flFound > 0.0f)
		s_flTraversalSweetSpotFrac[nTraversalState] = flFound;

	return flFound;
}

//-----------------------------------------------------------------------------
// Script surface
//-----------------------------------------------------------------------------
static SQRESULT ClientScript_MantleBoostGetState(HSQUIRRELVM v)
{
	sq_pushinteger(v, MantleBoostClient_GetState());
	return SQ_OK;
}

static SQRESULT ClientScript_MantleBoostGetUiSetting(HSQUIRRELVM v)
{
	sq_pushinteger(v, mantle_boost_ui_setting.GetInt());
	return SQ_OK;
}

static SQRESULT ClientScript_MantleBoostGetTraversalAnimFrac(HSQUIRRELVM v)
{
	const uintptr_t pPlayer = MantleBoostRui_LocalPlayer();
	sq_pushfloat(v, pPlayer
		? *reinterpret_cast<const float*>(pPlayer + CPLAYER_OFF_TRAVERSAL_ANIM_FRAC)
		: 0.0f);
	return SQ_OK;
}

static SQRESULT ClientScript_MantleBoostGetTraversalProgress(HSQUIRRELVM v)
{
	const uintptr_t pPlayer = MantleBoostRui_LocalPlayer();
	sq_pushfloat(v, pPlayer
		? *reinterpret_cast<const float*>(pPlayer + CPLAYER_OFF_TRAVERSAL_PROGRESS)
		: 0.0f);
	return SQ_OK;
}

static SQRESULT ClientScript_MantleBoostGetSweetSpotFrac(HSQUIRRELVM v)
{
	const uintptr_t pPlayer = MantleBoostRui_LocalPlayer();
	sq_pushfloat(v, pPlayer ? MantleBoostRui_SweetSpotFrac(pPlayer) : 0.0f);
	return SQ_OK;
}

// The threshold angle above the view centre as a fraction of screen height:
// -0.5 * tan(threshold) / tanHalfFovY. The RUI spans its sweet-spot band from it.
static SQRESULT ClientScript_MantleBoostGetViewAngleRuiOffset(HSQUIRRELVM v)
{
	float flTanHalfFovY = 0.0f;
	if (!MantleBoostRui_MainView(nullptr, nullptr, &flTanHalfFovY))
	{
		sq_pushfloat(v, 0.0f);
		return SQ_OK;
	}

	const float flRad = MantleBoostClient_GetSweetSpotAngle() * float(M_PI_F / 180.0f);
	const float flCos = fmaxf(0.001f, cosf(flRad));

	sq_pushfloat(v, -0.5f * sinf(flRad) / (flCos * flTanHalfFovY));
	return SQ_OK;
}

// 0.5 * -dot(up, aim) / (dot(forward, aim) * tanHalfFovX * aspectRatioYOverX):
// where the crosshair sits once the traversal camera has been corrected away
// from the aim direction.
static SQRESULT ClientScript_MantleBoostGetCrosshairRuiOffset(HSQUIRRELVM v)
{
	const uintptr_t pPlayer = MantleBoostRui_LocalPlayer();
	Vector3D vecForward, vecUp;
	float flTanHalfFovY = 0.0f;

	if (!pPlayer || !v_C_Player_GetViewVector || !MantleBoostRui_MainView(&vecForward, &vecUp, &flTanHalfFovY))
	{
		sq_pushfloat(v, 0.0f);
		return SQ_OK;
	}

	Vector3D vecAim;
	v_C_Player_GetViewVector(reinterpret_cast<void*>(pPlayer), &vecAim);

	const float flDenom = fmaxf(0.001f, DotProduct(vecForward, vecAim));
	const float flOffset = 0.5f * -DotProduct(vecUp, vecAim) / (flDenom * flTanHalfFovY);

	if (bridge_mantle_boost_rui_log.GetBool())
	{
		static float s_flNextLog = 0.0f;
		const float flNow = static_cast<float>(Plat_FloatTime());
		if (flNow >= s_flNextLog)
		{
			s_flNextLog = flNow + 0.1f;
			Msg(eDLL_T::CLIENT, "[MB-RUI] crosshair fwd=(%.3f %.3f %.3f) aim=(%.3f %.3f %.3f) tanY=%.3f offset=%.4f\n",
				vecForward.x, vecForward.y, vecForward.z, vecAim.x, vecAim.y, vecAim.z, flTanHalfFovY, flOffset);
		}
	}

	sq_pushfloat(v, flOffset);
	return SQ_OK;
}

static SQRESULT ClientScript_SetLocalMantleBoostProfile(HSQUIRRELVM v)
{
	SQFloat flHeight = 1.0f, flSprint = 1.0f;
	sq_getfloat(v, 2, &flHeight);
	sq_getfloat(v, 3, &flSprint);
	MantleBoostClient_SetProfile(flHeight, flSprint);
	return SQ_OK;
}

void MantleBoostRui_RegisterClientFunctions(CSquirrelVM* s)
{
	if (!s)
	{
		Warning(eDLL_T::CLIENT, "[MB-RUI] null CLIENT VM\n");
		return;
	}

	// A new CLIENT VM is a new session: no class profile survives it.
	MantleBoostClient_SetProfile(1.0f, 1.0f);
	if (Script_RegisterFuncTC_S21(s, "SetLocalMantleBoostProfile", reinterpret_cast<void*>(ClientScript_SetLocalMantleBoostProfile),
			"void", "float heightScale, float sprintScale") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[MB-RUI] SetLocalMantleBoostProfile registration FAILED\n");

	struct Binding_s
	{
		const char* pszName;
		void* pFunc;
		const char* pszReturn;
	};

	const Binding_s bindings[] = {
		{ "MantleBoost_GetState",            reinterpret_cast<void*>(ClientScript_MantleBoostGetState),            "int" },
		{ "MantleBoost_GetUiSetting",        reinterpret_cast<void*>(ClientScript_MantleBoostGetUiSetting),        "int" },
		{ "MantleBoost_GetTraversalAnimFrac",reinterpret_cast<void*>(ClientScript_MantleBoostGetTraversalAnimFrac),"float" },
		{ "MantleBoost_GetTraversalProgress",reinterpret_cast<void*>(ClientScript_MantleBoostGetTraversalProgress),"float" },
		{ "MantleBoost_GetSweetSpotFrac",    reinterpret_cast<void*>(ClientScript_MantleBoostGetSweetSpotFrac),    "float" },
		{ "MantleBoost_GetViewAngleRuiOffset",reinterpret_cast<void*>(ClientScript_MantleBoostGetViewAngleRuiOffset),"float" },
		{ "MantleBoost_GetCrosshairRuiOffset",reinterpret_cast<void*>(ClientScript_MantleBoostGetCrosshairRuiOffset),"float" },
	};

	for (const Binding_s& b : bindings)
	{
		if (Script_RegisterFuncTC_S21(s, b.pszName, b.pFunc, b.pszReturn, "") == SQ_ERROR)
			Warning(eDLL_T::CLIENT, "[MB-RUI] %s registration FAILED\n", b.pszName);
	}
}

void VMantleBoostRuiCl::GetFun(void) const
{
	// CViewRender global initializer: the store after the vtable lea is the instance.
	s_pViewRenderS21 = Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 F3 0F 10 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 80 25 ?? ?? ?? ?? FC "
		"33 DB 0F 28 05 ?? ?? ?? ?? 48 89 05")
		.Offset(0x29).ResolveRelativeAddressSelf(0x3, 0x7).GetPtr();
	if (!s_pViewRenderS21)
		Warning(eDLL_T::CLIENT, "[MB-RUI] CViewRender unresolved -- the timing ring stays on the crosshair\n");

	// C_Player::GetViewVector -- AngleVectors(GetAimAngles(this)).
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 8B DA 48 8D 54 24 ?? E8 ?? ?? ?? ?? 48 8B C8 48 8B D3 "
		"E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 ?? 5B C3")
		.GetPtr(v_C_Player_GetViewVector);

	// C_BaseAnimating::SetCycle -- verified unique.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 83 B9 ?? ?? ?? ?? ?? 48 8B D9 0F 29 74 24 ?? 0F 28 F1 "
		"75 ?? 48 8B 41 ?? 48 83 C1 ?? FF 50 ?? 48 85 C0 74 ?? 48 8B CB E8")
		.GetPtr(v_C_BaseAnimating_SetCycle);

	// C_Player::EyeAngles -- verified unique.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 8B 01 48 8B DA 48 8D 54 24 ?? FF 90 ?? ?? ?? ?? "
		"F3 0F 10 44 24 ?? 48 8B C3 F3 0F 10 4C 24 ?? F3 0F 11 03")
		.GetPtr(v_C_Player_EyeAngles);
}
