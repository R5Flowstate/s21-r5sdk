#pragma once
//=============================================================================//
//
// Purpose: Constant buffer helpers for the material system
//
//=============================================================================//
#include "tier0/threadtools.h"
#include "rtech/rstdlib.h"

// IMPORTANT: do not change these as these are used by GfxCbufCollection_s
// which maps directly to the instance in the data segment of the image,
// change the NEW ones instead!
#define CBUF_MAX_ITEMS_OLD 3072
#define CBUF_BUCKET_SIZE_OLD 4096

// IMPORTANT: if you reached this limit, and you can't optimize resource
// usage any further, increase these. Keep in mind that the bucket size
// must be a power of two because RHashMap uses mod masks!
#define CBUF_MAX_ITEMS_NEW 9920
#define CBUF_BUCKET_SIZE_NEW 16384

struct GfxCbufItem_s
{
	u32 handle;
	u32 pad;
	u32 refCount;
	u32 hash;
};

// nb(kawe): this struct must be left unchanged as it maps directly
// with the data in the executable.
struct GfxCbufCollection_s
{
	CThreadMutexRW mutex;
	RHashMap<GfxCbufItem_s, CBUF_MAX_ITEMS_OLD> insertMap;
	int insertBuckets[CBUF_BUCKET_SIZE_OLD];
	GfxCbufItem_s insertItems[CBUF_MAX_ITEMS_OLD];
	RHashMap<u32, CBUF_MAX_ITEMS_OLD> eraseMap;
	u32 eraseItems[CBUF_MAX_ITEMS_OLD];
	int eraseBuckets[CBUF_BUCKET_SIZE_OLD];
};

extern GfxCbufCollection_s* g_constBufferCollection;
inline void (*v_Gfx_InitShared)(void);

