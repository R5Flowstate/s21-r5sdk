//=============================================================================//
//
// Purpose: +dodge settings row + jump coupling. GetButtonBits already packs
// bit 28; JumpDown/Up on this build never copy that press onto in_dodge.
//
//=============================================================================//
#ifndef CLIENT_DODGE_BIND_H
#define CLIENT_DODGE_BIND_H

#include "thirdparty/detours/include/idetour.h"

inline void (*IN_JumpDown)(void* args) = nullptr;
inline void (*IN_JumpUp)(void* args) = nullptr;
inline void (*IN_DodgeDown)(void* args) = nullptr;
inline void (*IN_DodgeUp)(void* args) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VDodgeBind : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_DODGE_BIND_H
