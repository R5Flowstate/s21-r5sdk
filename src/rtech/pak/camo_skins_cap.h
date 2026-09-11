//=============================================================================//
//
// Purpose: Raise the hardcoded 512-entry camo-skins cap (imm32 in four sites).
//
//=============================================================================//
#ifndef CAMO_SKINS_CAP_H
#define CAMO_SKINS_CAP_H

#include "thirdparty/detours/include/idetour.h"

class VCamoSkinsCap : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // CAMO_SKINS_CAP_H
