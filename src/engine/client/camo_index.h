//=============================================================================//
//
// Purpose: Clamp S21 camo table index. Hostile dedi (or S3 -1) must not
//          index camo_skins as table[8*eax] for a negative or oversized value.
//
//=============================================================================//
#ifndef ENGINE_CLIENT_CAMO_INDEX_H
#define ENGINE_CLIENT_CAMO_INDEX_H

#include "tier0/dbg.h"
#include "tier0/module.h"
#include "thirdparty/detours/include/idetour.h"

inline int32_t(__fastcall* v_CamoIndexGetter)(void* pEnt) = nullptr;
inline __int64(__fastcall* v_CamoDrawSubmit)(void* a1, void* a2, uint8_t* drawInfo) = nullptr;
inline uintptr_t* s_pCamoSkins = nullptr;

class VCamoIndexClamp : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CamoIndexGetter", v_CamoIndexGetter);
		LogFunAdr("CamoDrawSubmit", v_CamoDrawSubmit);
		LogVarAdr("g_camoSkins", s_pCamoSkins);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "8B 81 64 0D 00 00 C3")
			.GetPtr(v_CamoIndexGetter);
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC 00 01 00 00 48 8B 0D")
			.GetPtr(v_CamoDrawSubmit);
		CMemory look = Module_FindPattern(g_GameDll,
			"8B D0 48 8B 05 ?? ?? ?? ?? 48 8B 08 48 8B 04 D1");
		if (look)
			s_pCamoSkins = look.Offset(2).ResolveRelativeAddress(3, 7).RCast<uintptr_t*>();
		if (!v_CamoIndexGetter)
			Warning(eDLL_T::CLIENT, "[CAMO-RECV] getter pattern unresolved\n");
		if (!v_CamoDrawSubmit)
			Warning(eDLL_T::CLIENT, "[CAMO-RECV] draw-submit pattern unresolved\n");
		if (!s_pCamoSkins)
			Warning(eDLL_T::CLIENT, "[CAMO-RECV] camo_skins object unresolved -- clamp fail-closed to 0\n");
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_CLIENT_CAMO_INDEX_H
