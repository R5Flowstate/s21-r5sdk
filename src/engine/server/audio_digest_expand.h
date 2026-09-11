//=============================================================================//
//
// Purpose: Expand the audio bank digest to 200000 entries; ingest FDMR 1.1.
// Static region is 100000 x 24-byte entries at +0x410; buckets at +0x24A310.
// Heap buffer keeps entries at +0x410; buckets move to +0x493C10.
//
//=============================================================================//
#ifndef AUDIO_DIGEST_EXPAND_H
#define AUDIO_DIGEST_EXPAND_H

#include "thirdparty/detours/include/idetour.h"
#include <cstdint>

// Heap buffer used as the runtime mbnk_digest storage. nullptr if expansion
// was disabled at startup, allocation failed, or every patch failed.
extern uint8_t* g_pSdkAudioDigest;

// Original digest base address (module_base + 0x25E58260). Set when the
// buffer is allocated; used as the validation target for per-site xref
// resolution.
extern uint8_t* g_pOrigAudioDigest;

class VAudioDigestExpand : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // AUDIO_DIGEST_EXPAND_H
