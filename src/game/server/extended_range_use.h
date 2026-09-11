#ifndef EXTENDED_RANGE_USE_H
#define EXTENDED_RANGE_USE_H

//=============================================================================//
//
// Purpose: Inject script-supplied extended-range use entities into the
// native use-candidate list on the dedicated server.
//
//=============================================================================//
#ifndef CLIENT_DLL

#include "thirdparty/detours/include/idetour.h"

//-----------------------------------------------------------------------------
// The S3 dedi never calls CodeCallback_GetExtendedRangeUseEntitiesForPlayer
// and has no extended-range use search. Hooks the use-candidate sort seam so
// script entities (Alter deathbox, Void Nexus) reach EnumEntity under the
// extended cones before the list is ranked. Also NOPs the count<=0 early-out
// so pure extended-range targets still reach that seam, and honors
// USABLE_NO_LOS_REQUIREMENT on the post-trace accept path (S3 never reads it).
//-----------------------------------------------------------------------------
void ExtendedUse_LevelShutdown(void);

class VExtendedRangeUse : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // !CLIENT_DLL

#endif // EXTENDED_RANGE_USE_H
