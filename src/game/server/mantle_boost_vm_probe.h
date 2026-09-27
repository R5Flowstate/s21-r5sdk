//=============================================================================//
//
// Purpose: [MB-VM] trace of the sprint / weapon-state chain around a mantle
// climb and boost -- the inputs that pick the 1p raise-from-sprint sequence.
//
//=============================================================================//
#ifndef MANTLE_BOOST_VM_PROBE_H
#define MANTLE_BOOST_VM_PROBE_H

#include "thirdparty/detours/include/idetour.h"

class CPlayer;

void MantleBoostVmProbe_OnClimbStart(CPlayer* const player);
void MantleBoostVmProbe_OnBoostStep(CPlayer* const player, const char* const pszStep);
void MantleBoostVmProbe_PostRunCommand(CPlayer* const player);

///////////////////////////////////////////////////////////////////////////////
class VMantleBoostVmProbe : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // MANTLE_BOOST_VM_PROBE_H
