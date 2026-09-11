//=============================================================================//
//
// Purpose: Raise SettingsAssets max-entries past stock 8192.
//
//=============================================================================//
#ifndef STRING_TABLE_GROW_H
#define STRING_TABLE_GROW_H

#include "thirdparty/detours/include/idetour.h"

class VStringTableGrow : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // STRING_TABLE_GROW_H
