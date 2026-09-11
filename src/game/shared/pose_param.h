//=============================================================================//
//
// Purpose: S21 client crash shield for Studio_LocalPoseParameter OOB pose-index remaps (virtualmodel masterPose).
//
//=============================================================================//
#ifndef POSE_PARAM_GUARD_S21_H
#define POSE_PARAM_GUARD_S21_H

#include "thirdparty/detours/include/idetour.h"

inline __int64(__fastcall* v_Studio_LocalPoseParameter)(
	__int64, const float*, const void*, unsigned __int16, unsigned __int16, float*) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VPoseParamGuardS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Studio_LocalPoseParameter", v_Studio_LocalPoseParameter);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // POSE_PARAM_GUARD_S21_H
