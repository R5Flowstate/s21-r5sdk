//=============================================================================//
//
// Purpose: Grow the SignonInfo bf_write static buffer past the engine's stock
// 0xC0000 (786432-byte) cap so oversized SettingsAssets baselines (and the
// rest of the signon payload) fit on client connect.
//
// Without it, the connecting client dies with Host_Error: Overflow error
// writing string table baseline SettingsAssets when the outer bf_write
// target overflows.
//
//=============================================================================//
#ifndef RTECH_PAK_SIGNON_BUFFER_GROW_H
#define RTECH_PAK_SIGNON_BUFFER_GROW_H

#include "thirdparty/detours/include/idetour.h"

class VSignonBufferGrow : public IDetour
{
    virtual void GetAdr(void) const { }
    virtual void GetFun(void) const { }
    virtual void GetVar(void) const { }
    virtual void GetCon(void) const { }
    virtual void Detour(const bool bAttach) const;
};

#endif // RTECH_PAK_SIGNON_BUFFER_GROW_H
