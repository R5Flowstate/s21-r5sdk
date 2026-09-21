//=============================================================================//
//
// Purpose: Bolt hit-size grow schedule parity (dedi side).
//
//=============================================================================//
#ifndef BOLT_HITSIZE_PARITY_H
#define BOLT_HITSIZE_PARITY_H
#ifdef _WIN32
#pragma once
#endif

#include "thirdparty/detours/include/idetour.h"

typedef __int64 (__fastcall* PFN_CrossbowBolt_Create)(__int64 pOrigin, float* pDir, __int64 damage,
	float speed, __int64 owner, int impactTable, unsigned int modelIndex, float hitSize,
	unsigned char usesGravity, int projectileIndex, __int64 weapon);
inline PFN_CrossbowBolt_Create v_CrossbowBolt_Create = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VBoltHitsizeParity : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CrossbowBolt_Create", v_CrossbowBolt_Create);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // BOLT_HITSIZE_PARITY_H
