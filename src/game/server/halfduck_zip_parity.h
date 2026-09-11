//=============================================================================//
//
// Purpose: make m_doingHalfDuck agree with the client's at a zipline exit.
// See halfduck_zip_parity.cpp for the mechanism and the measurement.
//
//=============================================================================//
#ifndef HALFDUCK_ZIP_PARITY_H
#define HALFDUCK_ZIP_PARITY_H

#include "thirdparty/detours/include/idetour.h"

// CPlayer::Zipline_IsZiplining. False if the pattern missed.
bool HalfDuck_PlayerIsZiplining(void* player);

///////////////////////////////////////////////////////////////////////////////
class VHalfDuckZipParity : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // HALFDUCK_ZIP_PARITY_H
