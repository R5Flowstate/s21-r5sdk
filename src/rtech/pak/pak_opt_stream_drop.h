//=============================================================================//
//
// Purpose: S21 .opt.starpak policy: auto-drop when core HD packs are missing.
//
//=============================================================================//
#ifndef PAK_OPT_STREAM_DROP_S21_H
#define PAK_OPT_STREAM_DROP_S21_H

#include "thirdparty/detours/include/idetour.h"
#include "tier0/dbg.h"

using FsAsyncOpenPath_fn = int(__fastcall*)(const char* path, int logChannel,
	size_t* outSize, unsigned char flags);

// Pre-open policy for the async file open hook. True when the open was
// handled (result written); false to proceed with the stock open.
bool PakOptStreamDrop_GuardOpen(const char* path, int logChannel, size_t* outSize,
	unsigned char flags, FsAsyncOpenPath_fn origOpen, int* pResult);

///////////////////////////////////////////////////////////////////////////////
class VPakOptStreamDropS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogVarAdr("PakOptStreamDrop_DecisionSite_S21",
			reinterpret_cast<const void*>(s_decisionSite));
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;

	static inline uintptr_t s_decisionSite = 0;
	static inline uint8_t s_origJnz[6] = {};
	static inline bool s_patched = false;
};
///////////////////////////////////////////////////////////////////////////////

#endif // PAK_OPT_STREAM_DROP_S21_H
