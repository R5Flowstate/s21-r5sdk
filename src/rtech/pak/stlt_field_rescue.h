//=============================================================================//
//
// Purpose: Rescue settings-layout (stlt) fields whose finder resolution
// fails in CSettingsLayout::ResolveFieldFinders.
//
// On miss the engine logs "Couldn't find field ..." and caches offset -1
// (0xFFFFFFFF); consumers then fault on layoutBase + 4 GiB - 1. A
// post-process detour re-walks the node's sibling list after the original
// returns and aliases each -1 offset to a same-type sibling that resolved,
// so consumers degrade gracefully instead of crashing on newer stlt assets.
//
//=============================================================================//
#ifndef STLT_FIELD_RESCUE_H
#define STLT_FIELD_RESCUE_H

#include "thirdparty/detours/include/idetour.h"

class VStltFieldRescue : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // STLT_FIELD_RESCUE_H
