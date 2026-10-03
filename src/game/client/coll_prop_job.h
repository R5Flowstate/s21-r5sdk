//=============================================================================//
//
// Purpose: Let collision-prop update jobs read under the entity transform lock
//          that their dispatcher already holds exclusively.
//
//=============================================================================//
#ifndef CLIENT_COLL_PROP_JOB_H
#define CLIENT_COLL_PROP_JOB_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VCollPropJobLock : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_COLL_PROP_JOB_H
