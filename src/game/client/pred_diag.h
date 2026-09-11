//=============================================================================//
//
// Purpose: Per-field client/server prediction-error spew (S21 client).
//
//=============================================================================//
#ifndef CLIENT_PRED_DIAG_H
#define CLIENT_PRED_DIAG_H

#include "thirdparty/detours/include/idetour.h"

// C_BaseEntity::PostNetworkDataReceived(latestCommandRunOnServer, commands_acknowledged)
// Returns true if any networked predicted field diverged from the received server data.
inline bool(*C_BaseEntity__PostNetworkDataReceived)(void* pEntity, __int64 nLatestCmd, int nAcked) = nullptr;

// C_BaseEntity::GetPredictedEntityState(commandNum) -> PredictedEntityState*
// The predicted serializedData buffer lives at +0x08 of the returned state.
inline void* (*C_BaseEntity__GetPredictedEntityState)(void* pEntity, unsigned int nCmd) = nullptr;

// CPrediction dispatch. a2 = nLatestCmd, a3 = commands_acknowledged, a4 = flag.
inline __int64(*Prediction_Dispatch)(__int64 a1, unsigned int a2, int a3, char a4) = nullptr;

// [PREDERR] vtbl[29] origin-error check. If |delta| in (0.5, 512], frees the
// smoothing record at ent+3472 and the position snaps.
inline __int64(*PredErrorCheck)(__int64 a1, unsigned int a2, __int64 a3, __int64 a4) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VPredDiag : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_BaseEntity::PostNetworkDataReceived", C_BaseEntity__PostNetworkDataReceived);
		LogFunAdr("C_BaseEntity::GetPredictedEntityState", C_BaseEntity__GetPredictedEntityState);
		LogFunAdr("CPrediction::Dispatch", Prediction_Dispatch);
		LogFunAdr("CPrediction::PredErrorCheck", PredErrorCheck);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_PRED_DIAG_H
