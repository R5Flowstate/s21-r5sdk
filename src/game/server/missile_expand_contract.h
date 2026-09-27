//=============================================================================//
//
// Purpose: S21 expand-contract missile path on the dedicated server -- the
// three-phase flight, wiggle, grace period and the multi-target grid finder
// the S3 script VM lacks.
//
//=============================================================================//
#ifndef MISSILE_EXPAND_CONTRACT_H
#define MISSILE_EXPAND_CONTRACT_H

#include "thirdparty/detours/include/idetour.h"

struct ScriptClassDescriptor_t;

void MissileExpandContract_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct);

///////////////////////////////////////////////////////////////////////////////
class VMissileExpandContract : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MISSILE_EXPAND_CONTRACT_H
