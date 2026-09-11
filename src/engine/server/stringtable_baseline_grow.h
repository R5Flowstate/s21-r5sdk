//=============================================================================//
//
// Purpose: Grow the string-table baseline scratch used by
// CClient::SendServerInfo, and report per-table fill so an overflow is named
// instead of inferred. See stringtable_baseline_grow.cpp.
//
//=============================================================================//
#ifndef ENGINE_SERVER_STRINGTABLE_BASELINE_GROW_H
#define ENGINE_SERVER_STRINGTABLE_BASELINE_GROW_H

#include "thirdparty/detours/include/idetour.h"

// CNetworkStringTable::WriteBaselines(this, SVC_CreateStringTable* msg,
// void* buf, int nBytes) -- builds msg's bf_write over buf and serialises
// every entry into it.
inline bool (*v_CNetworkStringTable_WriteBaselines)(void* pTable, void* pMsg,
	void* pBuf, int nBytes) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VStringTableBaselineGrow : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CNetworkStringTable::WriteBaselines",
			v_CNetworkStringTable_WriteBaselines);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ENGINE_SERVER_STRINGTABLE_BASELINE_GROW_H
