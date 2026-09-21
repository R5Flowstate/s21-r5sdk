//=============================================================================//
//
// Purpose: slide super-jump (the "boosted_slide_jump" settings mod) for the
// dedicated server. The S21 client jumps a second time within 0.3s of a
// slide-jump when the mod is active; S3 has no such path, so the server twin
// accepts that press and adds the same vertical impulse.
//
//=============================================================================//
#ifndef SLIDE_SUPER_JUMP_H
#define SLIDE_SUPER_JUMP_H

#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VSlideSuperJumpBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SLIDE_SUPER_JUMP_H
