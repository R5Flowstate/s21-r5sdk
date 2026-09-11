//=============================================================================//
//
// Purpose: Place the reserved non-rewinding edict where the S21 client reads it.
//
//=============================================================================//
#ifndef ENGINE_SERVER_EDICT_RESERVE_H
#define ENGINE_SERVER_EDICT_RESERVE_H

#include "thirdparty/detours/include/idetour.h"

class VEdictReserve : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_SERVER_EDICT_RESERVE_H
