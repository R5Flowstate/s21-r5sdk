//=============================================================================//
//
// Purpose: S21 virtual-model seq bind: a counted aseq wrapper whose first
//          pointer is NULL still has its numautolayers word read at +0x4E.
//
//=============================================================================//
#ifndef STUDIO_SEQDESC_GUARD_S21_H
#define STUDIO_SEQDESC_GUARD_S21_H

#include "thirdparty/detours/include/idetour.h"

class VStudioSeqdescGuardS21 : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // STUDIO_SEQDESC_GUARD_S21_H
