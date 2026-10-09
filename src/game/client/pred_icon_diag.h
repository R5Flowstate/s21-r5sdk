//=============================================================================//
//
// Purpose: [PRED-ICON] -- log every visible prediction correction and whether
// it lit the HUD prediction-error icon.
//
//=============================================================================//
#ifndef CLIENT_PRED_ICON_DIAG_H
#define CLIENT_PRED_ICON_DIAG_H

#include "thirdparty/detours/include/idetour.h"

// C_Player::NotePredictionError (S21): eye position, fov, gravity and angle
// error a re-prediction moved the view by; called right after the icon decision.
inline void (*C_Player__NotePredictionError)(void* pPlayer, const float* pEyeErr, float flFovErr,
	const float* pGravErr, float flAngErrA, float flAngErrB, float* pPoseErr, int nPose, float flTime) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VPredIconDiag : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_Player::NotePredictionError", C_Player__NotePredictionError);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_PRED_ICON_DIAG_H
