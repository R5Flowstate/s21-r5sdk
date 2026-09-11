//=============================================================================//
//
// Purpose: Guard GetEntryUserData; empty table AVs at storage+72*idx+56.
//
//=============================================================================//

#ifndef STRINGTABLE_DIAG_S21_H
#define STRINGTABLE_DIAG_S21_H

#include "thirdparty/detours/include/idetour.h"

// GetEntryUserData (vtable 16): empty table AVs at storage+72*idx+56.
inline const void* (__fastcall *v_CNetworkStringTable_GetEntryUserData_S21)(
	void* thisp, int stringNumber, int* length) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VStringTableDiagS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CNetworkStringTable::GetEntryUserData_S21",
				  v_CNetworkStringTable_GetEntryUserData_S21);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll,
			"40 53 48 83 EC 20 48 8B 41 50 49 8B D8 4C 8B 49 48 "
			"48 85 C0 74 0A 83 FA FF")
			.GetPtr(v_CNetworkStringTable_GetEntryUserData_S21);
	}
	virtual void GetVar(void) const {}
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // STRINGTABLE_DIAG_S21_H
