//=============================================================================//
//
// Purpose: S21 player-layout remap for wallrun/climb settings finders, plus
// the disable_wall_run status bind and [WALLCLIMB] FullWalkMove tap.
//
//=============================================================================//
#ifndef WALLCLIMB_H
#define WALLCLIMB_H

#include "thirdparty/detours/include/idetour.h"

void WallClimb_BeforeFullWalkMove(void* ctx);
void WallClimb_AfterFullWalkMove(void* ctx);

///////////////////////////////////////////////////////////////////////////////
class VWallClimb : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // WALLCLIMB_H
