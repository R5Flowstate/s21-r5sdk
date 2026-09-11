//=============================================================================//
//
// Purpose: Heap-backed overflow when pdef baseStruct exceeds the engine's
// 180-item (8640-byte) stack Src; needed for S21-capacity pdefs.
//
//=============================================================================//
#ifndef PERSISTENCE_BASEVAR_OVERFLOW_H
#define PERSISTENCE_BASEVAR_OVERFLOW_H

#include "thirdparty/detours/include/idetour.h"

bool PersistenceBvo_IsArmed(void);

class VPersistenceBaseVarOverflow : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // PERSISTENCE_BASEVAR_OVERFLOW_H
