#ifndef ZIPLINE_DISCONNECT_CLIENT_H
#define ZIPLINE_DISCONNECT_CLIENT_H

#include "thirdparty/detours/include/idetour.h"

bool ZipDisc_ShouldRefuseMount(void* player);
void ZipDisc_OnMountGranted(void* player);
void ZipDisc_OnCommand(void* player, const bool bOnGround, const bool bZiplining);
void ZipDisc_OnMantle(void* player);

class VZipDiscClient : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif
