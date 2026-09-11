//=============================================================================//
//
// Purpose: S21 client crash shield for the C_BaseAnimating skin/body change
// path. See body_skin.cpp.
//
//=============================================================================//
#ifndef BODY_SKIN_GUARD_S21_H
#define BODY_SKIN_GUARD_S21_H

#include "thirdparty/detours/include/idetour.h"

inline double(__fastcall* v_AnimBodySkinChange)(__int64, unsigned __int16) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VBodySkinGuardS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("AnimBodySkinChange", v_AnimBodySkinChange);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BODY_SKIN_GUARD_S21_H
