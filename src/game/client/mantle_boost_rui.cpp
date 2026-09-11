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
#include "game/shared/mantle_boost_curves.h"
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
static constexpr ptrdiff_t CPLAYER_OFF_PROXY_HANDLE        = 0x3704;

// C_PredictedFirstPersonProxy. The view-correction block is always written by
// GetTraversalViewPosition, so every sampler snapshots and restores it.
static constexpr ptrdiff_t PROXY_OFF_ATTACH_REF   = 5536;
static constexpr ptrdiff_t PROXY_OFF_ATTACH_CAM   = 5537;
static constexpr ptrdiff_t PROXY_OFF_ATTACH_BASE  = 5538;
static constexpr ptrdiff_t PROXY_OFF_VIEWCORR     = 5568;
static constexpr ptrdiff_t PROXY_OFF_PLAYBACKRATE = 0xE34;  // Anim_SetPlaybackRate is inline

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
// Purpose: the player's first-person proxy, with its cached attachment ids
// already populated. Zero ids mean the engine has not cached them yet.
//-----------------------------------------------------------------------------
static uintptr_t MantleBoostRui_Proxy(uintptr_t pPlayer)
{
	if (!pPlayer)
		return 0;

	const uint32_t hProxy = *reinterpret_cast<const uint32_t*>(pPlayer + CPLAYER_OFF_PROXY_HANDLE);
	if (hProxy == 0xFFFFFFFFu)
		return 0;

	const uintptr_t pList = NetObs_EntityHandleTableAddr();
	if (!pList)
		return 0;

	const uintptr_t pEntry = pList + 32ull * static_cast<uint16_t>(hProxy);
	const uintptr_t pProxy = *reinterpret_cast<const uintptr_t*>(pEntry);
	if (!pProxy || *reinterpret_cast<const uint32_t*>(pEntry + 8) != (hProxy >> 16))
		return 0;

	if (*reinterpret_cast<const uint8_t*>(pProxy + PROXY_OFF_ATTACH_REF) <= 0
		|| *reinterpret_cast<const uint8_t*>(pProxy + PROXY_OFF_ATTACH_CAM) <= 0
		|| *reinterpret_cast<const uint8_t*>(pProxy + PROXY_OFF_ATTACH_BASE) <= 0)
		return 0;

	return pProxy;
}

//-----------------------------------------------------------------------------
// Purpose: tanHalfFovX * aspectRatioYOverX, inverted.
//
// Not read off CViewSetup: the SDK's declaration of that struct does not match
// what this build lays out, so its field offsets are not trustworthy here.
// Projecting a probe point one unit forward and one unit up from the eye gives
// an NDC y of exactly 1/(tanHalfFovX * aspectRatioYOverX), which is the whole
// term both getters divide by.
//-----------------------------------------------------------------------------
static bool MantleBoostRui_InvProjectionScale(float* pflInvScale)
{
	if (!g_pViewRender)
		return false;

	const VMatrix* const pWorldToScreen =
		g_pViewRender->GetViewProjectionMatrix(VMATRIX_TYPE_VIEW);
	if (!pWorldToScreen)
		return false;

	Vector3D vecForward, vecRight, vecUp;
	AngleVectors(MainViewAngles(), &vecForward, &vecRight, &vecUp);

	const Vector3D vecProbe = MainViewOrigin() + vecForward + vecUp;

	Vector2D ndc;
	if (ClipTransform(*pWorldToScreen, vecProbe, &ndc))
		return false;   // behind the eye: the clamp path, not a usable ratio

	if (!isfinite(ndc.y) || ndc.y <= 0.0f)
		return false;

	*pflInvScale = ndc.y;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: where the boost window OPENS, as a traversal-progress (cycle)
// fraction -- the moment the ring should meet the brackets.
//
// Evaluated off the baked curve: scan the cycle upward from 0.5 and cache
// the first sample whose |delta| drops below the angle threshold.
//-----------------------------------------------------------------------------
static float s_flTraversalSweetSpotFrac[TRAVERSAL_COUNT] = { 0.0f };
static float s_flSweetSpotRetryAt[TRAVERSAL_COUNT] = { 0.0f };

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

	float vecFwd[3];
	if (!MBCurves_Has(nTraversalState)
		|| !MantleBoostClient_GetTraversalFwd(pPlayer, vecFwd))
		return 0.0f;

	float flEye[3] = { 0.0f, 0.0f, 0.0f };
	if (v_C_Player_EyeAngles)
		v_C_Player_EyeAngles(reinterpret_cast<void*>(pPlayer), flEye);

	const Vector3D fwd(vecFwd[0], vecFwd[1], vecFwd[2]);
	const QAngle eye(flEye[0], flEye[1], flEye[2]);
	const float flThreshold = MantleBoostClient_GetSweetSpotAngle(nTraversalState, fwd, eye);

	float flFound = 0.0f;
	for (int i = MB_CURVE_SAMPLES / 2; i < MB_CURVE_SAMPLES; ++i)
	{
		const float flCycle = float(i) / float(MB_CURVE_SAMPLES - 1);
		float flDelta = 0.0f;
		if (!MBCurves_Eval(nTraversalState, flCycle, fwd, eye, &flDelta))
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
// Purpose: clear baked curve state so the next climb re-bakes it. Also resets
// the memoized ring windows, which derive from the same table.
//-----------------------------------------------------------------------------
static void CC_MantleBoostCurveRebake(const CCommand& args)
{
	MBCurveSet_t& set = MBCurves();
	MBCurves_Load();
	int nCleared = 0;
	for (int t = 0; t < MB_CURVE_TRAVERSAL_COUNT; ++t)
	{
		if (args.ArgC() > 1 && atoi(args.Arg(1)) != t)
			continue;
		if (set.m_bLoaded[t])
			++nCleared;
		set.m_bLoaded[t] = false;
		s_flTraversalSweetSpotFrac[t] = 0.0f;
		s_flSweetSpotRetryAt[t] = 0.0f;
	}
	Msg(eDLL_T::CLIENT, "[MB-CURVE] cleared %d baked traversal states -- "
		"climb each type again to re-bake\n", nCleared);
}
static ConCommand mantle_boost_curve_rebake("mantle_boost_curve_rebake",
	CC_MantleBoostCurveRebake,
	"Clear baked mantle-boost curves (optional travState arg) so the next climb re-bakes.",
	FCVAR_DEVELOPMENTONLY);

//-----------------------------------------------------------------------------
// Curve bake (game/shared/mantle_boost_curves.h). GetTraversalViewPosition is
// linear in the eye matrix and factors as SA * P(cycle) * inv(SA) * eyeM, so
// sampling the NATIVE function with a zero eye across the cycle range recovers
// P exactly -- the dedi then evaluates the identical delta from its own ledge
// basis with no proxy animation or attachments involved. Each bake validates
// itself against the native function at probe eyes before it is kept.
//-----------------------------------------------------------------------------
static ConVar mantle_boost_curve_autodump("mantle_boost_curve_autodump", "1", FCVAR_RELEASE,
	"Bake the S21 traversal camera curve for each mantle type on first climb "
	"(writes platform/cfg/mantle_boost_curves.txt -- the dedi's sweet-spot input).");

static constexpr ptrdiff_t CPLAYER_OFF_TRAVERSAL_START = 0x2B80; // m_traversalStartTime

static float MantleBoostCurve_WrapDeg(float a)
{
	return fmodf(a + 540.0f, 360.0f) - 180.0f;
}

// delta composed from a P sample: MatrixAngles(SA * P * SA^T * eyeM).x - eye.x.
// Mirrors MBCurves_Eval on an exact sample row (no lerp), for validation.
static float MantleBoostCurve_ComposePitch(const matrix3x4_t& sa, const matrix3x4_t& saT,
	const float* pRow, const QAngle& eye)
{
	matrix3x4_t P;
	for (int r = 0; r < 3; ++r)
	{
		for (int c = 0; c < 3; ++c)
			P[r][c] = pRow[r * 3 + c];
		P[r][3] = 0.0f;
	}
	matrix3x4_t eyeM, a, b, m;
	AngleMatrix(eye, eyeM);
	ConcatTransforms(saT, eyeM, a);
	ConcatTransforms(P, a, b);
	ConcatTransforms(sa, b, m);
	QAngle out;
	MatrixAngles(m, out);
	return out.x;
}

void MantleBoostCurveDump_Think(uintptr_t pPlayer)
{
	if (!mantle_boost_curve_autodump.GetBool() || !pPlayer
		|| !v_C_BaseAnimating_SetCycle || !C_PredictedFirstPersonProxy__GetTraversalViewPosition)
		return;

	const int nTravState = *reinterpret_cast<const int*>(pPlayer + CPLAYER_OFF_TRAVERSAL_STATE);
	if (nTravState < 0 || nTravState >= MB_CURVE_TRAVERSAL_COUNT)
		return;
	if (MBCurves_Has(nTravState))
		return;

	// One attempt per traversal: a failed bake (proxy not ready, native bail,
	// validation miss) retries on the NEXT climb of this type, not per tick.
	static float s_flLastTryStart[MB_CURVE_TRAVERSAL_COUNT] = {};
	const float flStartTime = *reinterpret_cast<const float*>(pPlayer + CPLAYER_OFF_TRAVERSAL_START);
	if (s_flLastTryStart[nTravState] == flStartTime)
		return;

	const uintptr_t pProxy = MantleBoostRui_Proxy(pPlayer);
	if (!pProxy)
		return;   // attachments not cached yet -- retry this same traversal
	s_flLastTryStart[nTravState] = flStartTime;

	float vecFwd[3];
	if (!MantleBoostClient_GetTraversalFwd(pPlayer, vecFwd))
		return;

	matrix3x4_t sa, saT;
	VectorMatrix(Vector3D(vecFwd[0], vecFwd[1], vecFwd[2]), sa);
	MatrixSetColumn(Vector3D(0.0f, 0.0f, 0.0f), 3, sa);
	MatrixInvert(sa, saT);

	float* const pViewCorr = reinterpret_cast<float*>(pProxy + PROXY_OFF_VIEWCORR);
	float flSavedCorr[8];
	for (int i = 0; i < 8; ++i)
		flSavedCorr[i] = pViewCorr[i];
	float* const pPlaybackRate = reinterpret_cast<float*>(pProxy + PROXY_OFF_PLAYBACKRATE);
	const float flSavedRate = *pPlaybackRate;

	static float s_rows[MB_CURVE_SAMPLES][9];
	float flLiveEye[3] = { 0.0f, 0.0f, 0.0f };
	if (v_C_Player_EyeAngles)
		v_C_Player_EyeAngles(reinterpret_cast<void*>(pPlayer), flLiveEye);

	bool bOk = true;
	float flMaxResid = 0.0f;
	for (int i = 0; i < MB_CURVE_SAMPLES && bOk; ++i)
	{
		const float flCycle = float(i) / float(MB_CURVE_SAMPLES - 1);
		v_C_BaseAnimating_SetCycle(reinterpret_cast<void*>(pProxy), flCycle);
		*pPlaybackRate = 0.0f;

		// Zero eye: the native output IS M(cycle); P = SA^T * M * SA.
		float flOrg[3] = { 0.0f, 0.0f, 0.0f };
		float flAng[3] = { 0.0f, 0.0f, 0.0f };
		C_PredictedFirstPersonProxy__GetTraversalViewPosition(pProxy, flOrg, flAng);
		if (flOrg[0] == 0.0f && flOrg[1] == 0.0f && flOrg[2] == 0.0f)
		{
			bOk = false;   // native bailed -- not a usable pose this tick
			break;
		}

		matrix3x4_t M, t, P;
		AngleMatrix(QAngle(flAng[0], flAng[1], flAng[2]), M);
		ConcatTransforms(M, sa, t);
		ConcatTransforms(saT, t, P);   // SA^T * M * SA
		for (int r = 0; r < 3; ++r)
		{
			for (int c = 0; c < 3; ++c)
				s_rows[i][r * 3 + c] = P[r][c];
		}

		// Every 10th sample: prove the conjugation reproduces the native output
		// at non-zero eyes (linearity + factorization, end to end).
		if ((i % 10) == 0)
		{
			const QAngle probes[2] = {
				QAngle(flLiveEye[0], flLiveEye[1], flLiveEye[2]),
				QAngle(10.0f, flLiveEye[1] + 30.0f, 0.0f),
			};
			for (const QAngle& probe : probes)
			{
				float flOrgP[3] = { 0.0f, 0.0f, 0.0f };
				float flAngP[3] = { probe.x, probe.y, probe.z };
				C_PredictedFirstPersonProxy__GetTraversalViewPosition(pProxy, flOrgP, flAngP);
				const float flComposed = MantleBoostCurve_ComposePitch(sa, saT, s_rows[i], probe);
				const float flResid = fabsf(MantleBoostCurve_WrapDeg(flComposed - flAngP[0]));
				if (flResid > flMaxResid)
					flMaxResid = flResid;
			}
		}
	}

	// Always put the proxy back on the live traversal frame.
	v_C_BaseAnimating_SetCycle(reinterpret_cast<void*>(pProxy),
		*reinterpret_cast<const float*>(pPlayer + CPLAYER_OFF_TRAVERSAL_PROGRESS));
	*pPlaybackRate = flSavedRate;
	for (int i = 0; i < 8; ++i)
		pViewCorr[i] = flSavedCorr[i];

	if (!bOk)
		return;

	if (flMaxResid > 0.25f)
	{
		static uint16_t s_nWarnedStates = 0;
		if (!(s_nWarnedStates & (1u << nTravState)))
		{
			s_nWarnedStates |= uint16_t(1u << nTravState);
			Warning(eDLL_T::CLIENT, "[MB-CURVE] travState=%d bake REJECTED: probe residual "
				"%.3f deg -- the conjugation model does not hold for this traversal\n",
				nTravState, flMaxResid);
		}
		return;
	}

	MBCurves_Store(nTravState, s_rows);
	const bool bSaved = MBCurves_Save();
	Warning(eDLL_T::CLIENT, "[MB-CURVE] baked travState=%d maxResid=%.4f deg -> %s%s\n",
		nTravState, flMaxResid, MBCurves_FilePath(), bSaved ? "" : " (WRITE FAILED)");
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

// Raw traversal cycle -- the domain the sweet-spot window lives in. The ring
// must be driven by this, not the anim fraction, or it meets the brackets at a
// different moment than the gate opens.
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

// 0.5 * cos(threshold) / (sin(threshold) * tanHalfFovX * aspectRatioYOverX)
static SQRESULT ClientScript_MantleBoostGetViewAngleRuiOffset(HSQUIRRELVM v)
{
	float flInvScale = 0.0f;
	if (!MantleBoostRui_InvProjectionScale(&flInvScale))
	{
		sq_pushfloat(v, 0.0f);
		return SQ_OK;
	}

	const float flRad = MantleBoostClient_GetSweetSpotAngle() * float(M_PI_F / 180.0f);
	const float flSin = fmaxf(0.001f, sinf(flRad));

	sq_pushfloat(v, 0.5f * cosf(flRad) * flInvScale / flSin);
	return SQ_OK;
}

// 0.5 * -dot(up, aim) / (dot(forward, aim) * tanHalfFovX * aspectRatioYOverX):
// where the crosshair sits once the traversal camera has been corrected away
// from the aim direction.
static SQRESULT ClientScript_MantleBoostGetCrosshairRuiOffset(HSQUIRRELVM v)
{
	const uintptr_t pPlayer = MantleBoostRui_LocalPlayer();
	float flInvScale = 0.0f;

	if (!pPlayer || !v_C_Player_GetViewVector || !MantleBoostRui_InvProjectionScale(&flInvScale))
	{
		sq_pushfloat(v, 0.0f);
		return SQ_OK;
	}

	Vector3D vecAim;
	v_C_Player_GetViewVector(reinterpret_cast<void*>(pPlayer), &vecAim);

	Vector3D vecForward, vecRight, vecUp;
	AngleVectors(MainViewAngles(), &vecForward, &vecRight, &vecUp);

	const float flDenom = fmaxf(0.001f, DotProduct(vecForward, vecAim));

	sq_pushfloat(v, 0.5f * -DotProduct(vecUp, vecAim) * flInvScale / flDenom);
	return SQ_OK;
}

void MantleBoostRui_RegisterClientFunctions(CSquirrelVM* s)
{
	if (!s)
	{
		Warning(eDLL_T::CLIENT, "[MB-RUI] null CLIENT VM\n");
		return;
	}

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
