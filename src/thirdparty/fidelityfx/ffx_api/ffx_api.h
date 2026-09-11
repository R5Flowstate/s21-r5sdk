// FidelityFX SDK 1.1.4 API (MIT). Load via GetProcAddress; do not link import lib.
#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef FFX_API_ENTRY
#define FFX_API_ENTRY
#endif
#include <stdint.h>

enum FfxApiReturnCodes
{
	FFX_API_RETURN_OK                     = 0,
	FFX_API_RETURN_ERROR                  = 1,
	FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2,
	FFX_API_RETURN_ERROR_RUNTIME_ERROR    = 3,
	FFX_API_RETURN_NO_PROVIDER            = 4,
	FFX_API_RETURN_ERROR_MEMORY           = 5,
	FFX_API_RETURN_ERROR_PARAMETER        = 6,
};

typedef void* ffxContext;
typedef uint32_t ffxReturnCode_t;

#define FFX_API_EFFECT_MASK 0xffff0000u
#define FFX_API_EFFECT_ID_GENERAL 0x00000000u

typedef uint64_t ffxStructType_t;
typedef struct ffxApiHeader
{
	ffxStructType_t      type;
	struct ffxApiHeader* pNext;
} ffxApiHeader;

typedef ffxApiHeader ffxCreateContextDescHeader;
typedef ffxApiHeader ffxConfigureDescHeader;
typedef ffxApiHeader ffxQueryDescHeader;
typedef ffxApiHeader ffxDispatchDescHeader;

#define FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_SILENCE  0x0000000u
#define FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS   0x0000001u
#define FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS 0x0000002u
#define FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE  0xfffffffu

enum FfxApiMsgType
{
	FFX_API_MESSAGE_TYPE_ERROR   = 0,
	FFX_API_MESSAGE_TYPE_WARNING = 1,
	FFX_API_MESSAGE_TYPE_COUNT
};

typedef void (*ffxApiMessage)(uint32_t type, const wchar_t* message);

#define FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1 0x0000001u
struct ffxConfigureDescGlobalDebug1
{
	ffxConfigureDescHeader header;
	ffxApiMessage          fpMessage;
	uint32_t               debugLevel;
};

#define FFX_API_QUERY_DESC_TYPE_GET_VERSIONS 4u
struct ffxQueryDescGetVersions
{
	ffxQueryDescHeader header;
	uint64_t createDescType;
	void* device;
	uint64_t *outputCount;
	uint64_t *versionIds;
	const char** versionNames;
};

#define FFX_API_DESC_TYPE_OVERRIDE_VERSION 5u
struct ffxOverrideVersion
{
	ffxApiHeader header;
	uint64_t versionId;
};

#define FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION 6u
struct ffxQueryGetProviderVersion
{
	ffxQueryDescHeader header;
	uint64_t versionId;
	const char* versionName;
};

typedef void* (*ffxAlloc)(void* pUserData, uint64_t size);
typedef void (*ffxDealloc)(void* pUserData, void* pMem);

typedef struct ffxAllocationCallbacks
{
	void* pUserData;
	ffxAlloc alloc;
	ffxDealloc dealloc;
} ffxAllocationCallbacks;

FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb);

FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext* context, const ffxAllocationCallbacks* memCb);

FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc);
typedef ffxReturnCode_t (*PfnFfxConfigure)(ffxContext* context, const ffxConfigureDescHeader* desc);

FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc);
typedef ffxReturnCode_t (*PfnFfxQuery)(ffxContext* context, ffxQueryDescHeader* desc);

FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc);
typedef ffxReturnCode_t (*PfnFfxDispatch)(ffxContext* context, const ffxDispatchDescHeader* desc);

#if defined(__cplusplus)
}
#endif
