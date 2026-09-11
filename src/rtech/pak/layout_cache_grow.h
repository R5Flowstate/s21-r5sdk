//=============================================================================//
//
// Purpose: Grow the s_settingsLayoutRuntimeData hash cache from 64 to 128
// slots (40 bytes per slot) so more than 64 unique stlt assets avoid the
// engine fatal "Out of room for settings layout runtime data".
//
// Relocates the table onto a 128-slot DLL-resident array via in-place byte
// patches: 8 disp32 sites retargeted to s_newLayoutCache, 16 imm8 sites
// (mask 0x3F -> 0x7F, probe limit 0x40 -> 0x7F). The imm8 probe limit cannot
// exceed 0x7F (sign extension), so 128 slots / 127 probes is the ceiling
// without rewriting instruction widths.
//
//=============================================================================//
#ifndef LAYOUT_CACHE_GROW_H
#define LAYOUT_CACHE_GROW_H

#include "thirdparty/detours/include/idetour.h"

class VLayoutCacheGrow : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // LAYOUT_CACHE_GROW_H
