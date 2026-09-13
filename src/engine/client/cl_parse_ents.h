//=============================================================================//
//
// Purpose: Bound CL_ParsePacketEntities from-cursor writes to MAX_EDICTS.
//
//=============================================================================//
#ifndef ENGINE_CLIENT_CL_PARSE_ENTS_H
#define ENGINE_CLIENT_CL_PARSE_ENTS_H

#include "tier0/dbg.h"
#include "tier0/module.h"
#include "thirdparty/detours/include/idetour.h"

inline void* v_CL_ParsePacketEntities = nullptr;

class VCLParsePacketEntitiesBound : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CL_ParsePacketEntities", v_CL_ParsePacketEntities);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 8B C4 48 89 48 ?? 55 53 57")
			.GetPtr(v_CL_ParsePacketEntities);
		if (!v_CL_ParsePacketEntities)
			Warning(eDLL_T::CLIENT, "[PARSE-ENTS] CL_ParsePacketEntities pattern unresolved\n");
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_CLIENT_CL_PARSE_ENTS_H
