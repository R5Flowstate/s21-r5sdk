//=============================================================================//
//
// Purpose: [PRED-ICON] -- log every visible prediction correction and whether
// it lit the HUD prediction-error icon. See header.
//
// The icon is not the error-check verdict: CPrediction stamps its last error
// time only when a re-prediction moves the eye more than
// cl_predict_error_icon_threshold_dist or an angle (punch included) more than
// cl_predict_error_icon_threshold_angle. A field-only error never lights it.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "game/client/pred_authority.h"
#include "game/client/pred_icon_diag.h"

#include <cmath>

// C_Player.
static constexpr ptrdiff_t PID_OFF_FFLAGS    = 0xC8;   // int m_fFlags
static constexpr ptrdiff_t PID_OFF_ABSORIGIN = 0x17C;  // float[3]
static constexpr ptrdiff_t PID_OFF_DUCKSTATE = 0x2A70; // int m_duckState
static constexpr ptrdiff_t PID_OFF_SLOWMO    = 0x2D74; // bool m_slowMoEnabled

static ConVar sdk_pred_icon("sdk_pred_icon", "0", FCVAR_DEVELOPMENTONLY,
	"[PRED-ICON] log each visible prediction correction and whether it lit the prediction-error "
	"icon. 1 = icon events + corrections over 0.1 units, 2 = every correction.");

// CPrediction::m_lastPredictionErrorTime: set to curtime just before this call when
// the correction lights the icon.
static const float* s_pIconStamp = nullptr;
static uint32_t s_nCorrections = 0;

static void __fastcall Hook_C_Player_NotePredictionError(void* pPlayer, const float* pEyeErr, float flFovErr,
	const float* pGravErr, float flAngErrA, float flAngErrB, float* pPoseErr, int nPose, float flTime)
{
	const int nMode = sdk_pred_icon.GetInt();
	if (nMode > 0 && pPlayer && pEyeErr)
	{
		const bool bIcon = s_pIconStamp && *s_pIconStamp == PredNative_CurTime();

		const float flEye = std::sqrt(pEyeErr[0] * pEyeErr[0] + pEyeErr[1] * pEyeErr[1] + pEyeErr[2] * pEyeErr[2]);
		++s_nCorrections;
		if (bIcon || nMode > 1 || (flEye > 0.1f && (s_nCorrections <= 64 || (s_nCorrections % 64) == 0)))
		{
			const uint8_t* const p = static_cast<const uint8_t*>(pPlayer);
			const float* const o = reinterpret_cast<const float*>(p + PID_OFF_ABSORIGIN);
			Msg(eDLL_T::CLIENT, "[PRED-ICON] %s t=%.4f eye=(%.2f %.2f %.2f) |%.2f| fov=%.3f ang=%.3f/%.3f "
				"org=(%.1f %.1f %.1f) flags=%X duck=%d slowmo=%d\n",
				bIcon ? "ICON" : "corr", PredNative_CurTime(), pEyeErr[0], pEyeErr[1], pEyeErr[2], flEye,
				flFovErr, flAngErrA, flAngErrB, o[0], o[1], o[2],
				*reinterpret_cast<const int*>(p + PID_OFF_FFLAGS),
				*reinterpret_cast<const int*>(p + PID_OFF_DUCKSTATE), p[PID_OFF_SLOWMO]);
		}
	}

	C_Player__NotePredictionError(pPlayer, pEyeErr, flFovErr, pGravErr, flAngErrA, flAngErrB, pPoseErr, nPose, flTime);
}

void VPredIconDiag::GetFun(void) const
{
	// Prologue down to the m_lifeState early-out at +0x690.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 48 89 6C 24 20 56 48 81 EC 90 00 00 00 80 B9 90 06 00 00 00 49 8B E9 "
		"0F 29 7C 24 70 48 8B F2 0F 28 FA 48 8B D9 0F 85")
		.GetPtr(C_Player__NotePredictionError);
	if (!C_Player__NotePredictionError)
		Warning(eDLL_T::CLIENT, "[PRED-ICON] NotePredictionError pattern unresolved -- probe disabled\n");
}

void VPredIconDiag::GetVar(void) const
{
	// CPrediction::PredictionErrorDetected: curtime - m_lastPredictionErrorTime < icon duration.
	// The subss at +0x13 reads the stamp.
	const CMemory detected = Module_FindPattern(g_GameDll,
		"48 8B 05 ?? ?? ?? ?? F3 0F 10 48 10 48 8B 05 ?? ?? ?? ?? F3 0F 5C 0D ?? ?? ?? ?? "
		"F3 0F 10 40 60 0F 2F C1 0F 97 C0 C3");
	if (detected)
		s_pIconStamp = detected.Offset(0x13).ResolveRelativeAddress(4, 8).RCast<const float*>();
	if (!s_pIconStamp)
		Warning(eDLL_T::CLIENT, "[PRED-ICON] icon timestamp unresolved -- corrections log without the icon flag\n");
}

void VPredIconDiag::Detour(const bool bAttach) const
{
	if (C_Player__NotePredictionError)
		DetourSetup(&C_Player__NotePredictionError, &Hook_C_Player_NotePredictionError, bAttach);
}
