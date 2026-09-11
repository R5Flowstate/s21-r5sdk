#pragma once
#include "engine/cdll_int.h"

class CEngineClient : public IVEngineClient
{
public:
	void SetRestrictServerCommands(bool bRestrict);
	void SetRestrictClientCommands(bool bRestrict);
	int GetLocalPlayer(); // Local player index.

	// Hook statics
	static void _ClientCmd(CEngineClient* thisptr, const char* const szCmdString);
};

/* ==== CVENGINECLIENT ================================================================================================================================================== */
///////////////////////////////////////////////////////////////////////////////
inline void(*CEngineClient__ClientCmd)(CEngineClient* thisptr, const char* const szCmdString);

inline CMemory g_pEngineClientVFTable = nullptr;
inline CEngineClient* g_pEngineClient = nullptr;

///////////////////////////////////////////////////////////////////////////////
class HVEngineClient : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogConAdr("CEngineClient::`vftable'", (void*)g_pEngineClientVFTable.GetPtr());
		LogFunAdr("CEngineClient::ClientCmd", CEngineClient__ClientCmd);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "40 53 48 83 EC 20 80 3D ?? ?? ?? ?? ?? 48 8B DA 74 0C").GetPtr(CEngineClient__ClientCmd);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const 
	{
		g_pEngineClientVFTable = g_GameDll.GetVirtualMethodTable(".?AVCEngineClient@@");
		g_pEngineClient = (CEngineClient*)&g_pEngineClientVFTable; // CEngineClient is iface only (doesn't have members).
	}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
