//=============================================================================//
//
// Purpose: client half of the zipline-exit m_doingHalfDuck parity. See
// halfduck_zip_parity_client.cpp, and the dedi twin in
// src\game\server\halfduck_zip_parity.cpp.
//
//=============================================================================//
#ifndef HALFDUCK_ZIP_PARITY_CLIENT_H
#define HALFDUCK_ZIP_PARITY_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VHalfDuckZipParityClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // HALFDUCK_ZIP_PARITY_CLIENT_H
