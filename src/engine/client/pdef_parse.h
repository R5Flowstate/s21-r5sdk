//=============================================================================//
//
// Purpose: Fail-closed PersistenceDef struct items / flush past 4096
//          and enum name-pool / enum-count past 4608 / 192.
//
//=============================================================================//
#ifndef ENGINE_CLIENT_PDEF_PARSE_H
#define ENGINE_CLIENT_PDEF_PARSE_H

#include "tier0/dbg.h"
#include "tier0/module.h"
#include "thirdparty/detours/include/idetour.h"

inline __int64(__fastcall* v_PdefStartNewStruct)(char* name, __int64 pdef, __int64 nAdd) = nullptr;
inline __int64(__fastcall* v_PdefStartNewEnum)(__int64 pdef, char* name, __int64 nAdd) = nullptr;
inline void* v_PdefParse = nullptr;

class VPdefParseBound : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("PdefStartNewStruct", v_PdefStartNewStruct);
		LogFunAdr("PdefStartNewEnum", v_PdefStartNewEnum);
		LogFunAdr("PdefParse", v_PdefParse);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 83 EC ?? 49 8B E8 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B D3 48 8D 0D")
			.GetPtr(v_PdefStartNewStruct);
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 83 EC ?? 48 8B FA 48 8B D9 48 8B D1 49 8B E8")
			.GetPtr(v_PdefStartNewEnum);
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 4C 89 4C 24 ?? 4C 89 44 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24")
			.GetPtr(v_PdefParse);
		if (!v_PdefStartNewStruct)
			Warning(eDLL_T::CLIENT, "[PDEF] StartNewStruct pattern unresolved\n");
		if (!v_PdefStartNewEnum)
			Warning(eDLL_T::CLIENT, "[PDEF] StartNewEnum pattern unresolved\n");
		if (!v_PdefParse)
			Warning(eDLL_T::CLIENT, "[PDEF] PdefParse pattern unresolved\n");
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_CLIENT_PDEF_PARSE_H
