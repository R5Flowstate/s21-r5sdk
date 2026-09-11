#ifndef ENGINE_STATICPROP_BOUNDS_DEBUG_H
#define ENGINE_STATICPROP_BOUNDS_DEBUG_H
//=============================================================================//
//
// Purpose: Static prop bounds debug draw / dump helpers
//
//=============================================================================//
#pragma once
#include "tier0/basetypes.h"
#include "thirdparty/detours/include/idetour.h"

//-----------------------------------------------------------------------------
// Debug hook for StaticPropBoundsCheck during vis traversal.
// Crashes on bad static-prop indices; resolves data/bounds/threshold globals
// (data 32-byte stride, bounds 24-byte stride).
// bool StaticPropBoundsCheck(unsigned int staticPropIndex, float* position, float radiusSq)
//-----------------------------------------------------------------------------

// Original function pointer
inline bool(*v_StaticPropBoundsCheck)(unsigned int staticPropIndex, float* position, float radiusSq);

// Hook function declaration
bool StaticPropBoundsCheck_Hook(unsigned int staticPropIndex, float* position, float radiusSq);

// Globals for debugging
inline void** g_pStaticPropData = nullptr;
inline void** g_pStaticPropBounds = nullptr;
inline int* g_pStaticPropThreshold = nullptr;

// BSP version global - declared as extern in cpp
extern int* g_pBspVersion;

///////////////////////////////////////////////////////////////////////////////
class VStaticPropBoundsDebug : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("StaticPropBoundsCheck", v_StaticPropBoundsCheck);
		LogVarAdr("g_pStaticPropData", g_pStaticPropData);
		LogVarAdr("g_pStaticPropBounds", g_pStaticPropBounds);
		LogVarAdr("g_pStaticPropThreshold", g_pStaticPropThreshold);
		LogVarAdr("g_pBspVersion", g_pBspVersion);
	}
	
	virtual void GetFun(void) const
	{
		// Match StaticPropBoundsCheck prologue (first RIP-relative global load).
		Module_FindPattern(g_GameDll, "48 83 EC 48 F3 0F 10 62 04 0F 57 C0 48 8B 05").GetPtr(v_StaticPropBoundsCheck);
	}
	
	virtual void GetVar(void) const
	{
		// Resolve RIP-relative globals from StaticPropBoundsCheck and related patterns.
		if (v_StaticPropBoundsCheck)
		{
			// Static prop data base at function+0xC (7-byte RIP-relative mov).
			CMemory funcMem((uintptr_t)v_StaticPropBoundsCheck);
			funcMem.Offset(0xC).ResolveRelativeAddressSelf(3, 7).GetPtr(g_pStaticPropData);

			// Bounds table around +0x48 (same RIP-relative form).
			funcMem.Offset(0x48).ResolveRelativeAddressSelf(3, 7).GetPtr(g_pStaticPropBounds);

			// Threshold set during BSP load: sub r9d, [rip+disp] then shifts.
			Module_FindPattern(g_GameDll, "44 2B 0D ?? ?? ?? ?? 41 8B D9 C1 EB 1F").Offset(3).ResolveRelativeAddressSelf(0, 4).GetPtr(g_pStaticPropThreshold);

			// BSP version: cmp [rip+disp], 0x2E.
			Module_FindPattern(g_GameDll, "83 3D ?? ?? ?? ?? 2E 48 8B 05").Offset(2).ResolveRelativeAddressSelf(0, 5).GetPtr(g_pBspVersion);
		}
	}
	
	virtual void GetCon(void) const { }
	
	virtual void Detour(const bool bAttach) const
	{
		//DetourSetup(&v_StaticPropBoundsCheck, &StaticPropBoundsCheck_Hook, bAttach);
	}
};
///////////////////////////////////////////////////////////////////////////////

#endif // ENGINE_STATICPROP_BOUNDS_DEBUG_H
