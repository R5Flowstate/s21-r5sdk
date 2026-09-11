//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
//===========================================================================//
#ifndef UTIL_SERVER_H
#define UTIL_SERVER_H
#include "game/server/player.h"

CPlayer* UTIL_PlayerByIndex(const int nIndex);
inline void(*v_UTIL_SetDebugBits)(CPlayer* const pPlayer, const char* const name, const int bit);

///////////////////////////////////////////////////////////////////////////////
class V_UTIL_Server : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("UTIL_SetDebugBits", v_UTIL_SetDebugBits);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 41 8B F8").GetPtr(v_UTIL_SetDebugBits);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { };
};
///////////////////////////////////////////////////////////////////////////////

#endif // UTIL_SERVER_H
