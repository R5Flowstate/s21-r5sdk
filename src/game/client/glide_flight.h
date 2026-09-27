//=============================================================================//
//
// Purpose: client prediction twin of the server glide flight model
// (game/shared/glide_tuning.h terms on top of the S21 glide functions).
//
//=============================================================================//
#ifndef GLIDE_FLIGHT_CLIENT_H
#define GLIDE_FLIGHT_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VGlideFlightClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // GLIDE_FLIGHT_CLIENT_H
