//=============================================================================//
//
// Purpose: live diagnostics for S3 CTriggerSlip touch + force application.
//
//=============================================================================//
#ifndef TRIGGER_SLIP_DIAG_H
#define TRIGGER_SLIP_DIAG_H

#include "thirdparty/detours/include/idetour.h"

// Called from VJetDrive's FullWalkMove hook. That class owns the only attach.
void SlipDiag_BeforeFullWalkMove(void* ctx);
void SlipDiag_AfterFullWalkMove(void* ctx);

///////////////////////////////////////////////////////////////////////////////
class VTriggerSlipDiag : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // TRIGGER_SLIP_DIAG_H
