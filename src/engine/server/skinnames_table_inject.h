//=============================================================================//
//
// Purpose: Inject an empty SkinNames table on the dedi; the slot must exist.
//
//=============================================================================//
#ifndef SKINNAMES_TABLE_INJECT_H
#define SKINNAMES_TABLE_INJECT_H

#include "thirdparty/detours/include/idetour.h"

class VSkinNamesTableInject : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

// Clear inject state on LevelShutdown so map 2+ still gets a SkinNames table.
void SkinNamesInject_LevelShutdown();

#endif // SKINNAMES_TABLE_INJECT_H
