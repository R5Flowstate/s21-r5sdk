//=============================================================================//
//
// Purpose: realm hygiene for script-built entities and world effects.
// See realm_adopt.cpp.
//
//=============================================================================//
#ifndef REALM_ADOPT_H
#define REALM_ADOPT_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VRealmAdopt : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

// Restrict the next temp entity dispatched on this thread to players sharing
// a realm with mask. Consumed by that dispatch; 0 disarms.
void RealmAdopt_ArmTempEntRealms(const uint64_t mask);

// Live m_realmsBitMask of a server entity, 0 for null.
uint64_t RealmAdopt_GetRealmsBitMask(const void* const pEntity);

#endif // REALM_ADOPT_H
