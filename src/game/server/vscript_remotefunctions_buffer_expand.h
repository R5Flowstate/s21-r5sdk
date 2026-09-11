//=============================================================================//
//
// Purpose: expand Remote_RegisterClientFunction's 16 KB / 256-entry BSS
// buffer. Heap-expand to 256 KB / 2048 entries; cursor and count stay in BSS.
//
//=============================================================================//
#ifndef VSCRIPT_REMOTEFUNCTIONS_BUFFER_EXPAND_H
#define VSCRIPT_REMOTEFUNCTIONS_BUFFER_EXPAND_H

#include "thirdparty/detours/include/idetour.h"
#include <cstdint>

// Pointer to the heap-backed expanded buffer. nullptr if expansion was
// disabled at startup or allocation failed.
extern uint8_t* g_pSdkRemoteFuncBuffer;

// Pointer to the original BSS buffer (module_base + 0x027B28E0). Set when the
// expanded buffer is allocated; used as the per-site validation target.
extern uint8_t* g_pOrigRemoteFuncBuffer;

class VRemoteFuncBufferExpand : public IDetour
{
    virtual void GetAdr(void) const;
    virtual void GetFun(void) const { }
    virtual void GetVar(void) const { }
    virtual void GetCon(void) const { }
    virtual void Detour(const bool bAttach) const;
};

#endif // VSCRIPT_REMOTEFUNCTIONS_BUFFER_EXPAND_H
