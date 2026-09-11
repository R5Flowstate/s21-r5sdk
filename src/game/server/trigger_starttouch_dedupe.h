//=============================================================================//
//
// Purpose: restore the missing new-toucher guard on CBaseTrigger::StartTouch.
// OnStartTouch and m_enterCallback fire on re-entry; EndTouch already gates.
//
//=============================================================================//
#ifndef TRIGGER_STARTTOUCH_DEDUPE_H
#define TRIGGER_STARTTOUCH_DEDUPE_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VTriggerStartTouchDedupe : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TRIGGER_STARTTOUCH_DEDUPE_H
