//=============================================================================//
//
// Purpose: sidecar + value proxies for the S21-only DT_Zipline props.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier0/dbg.h"
#include "game/shared/sdk_entity_state.h"
#include "zipline_extend_state.h"

#include <cstring>

struct ZiplineExtendState
{
	float ropeColor[3] = { kZiplineRopeColorDefault,
	                       kZiplineRopeColorDefault,
	                       kZiplineRopeColorDefault };
};

static SDKEntityMap<ZiplineExtendState> s_ziplineStates(ESide::Server, "zipline.extend");

void Zipline_SetRopeColorModulation(const void* pZipline, const float* pRGB)
{
	if (!pZipline || !pRGB)
		return;

	ZiplineExtendState& st = s_ziplineStates[pZipline];
	st.ropeColor[0] = pRGB[0];
	st.ropeColor[1] = pRGB[1];
	st.ropeColor[2] = pRGB[2];
}

void Zipline_GetRopeColorModulation(const void* pZipline, float* pOutRGB)
{
	if (!pOutRGB)
		return;

	const ZiplineExtendState* const st = pZipline ? s_ziplineStates.Find(pZipline) : nullptr;
	if (st)
		memcpy(pOutRGB, st->ropeColor, sizeof(st->ropeColor));
	else
		pOutRGB[0] = pOutRGB[1] = pOutRGB[2] = kZiplineRopeColorDefault;
}

static void __fastcall ZiplineRopeColor_ValueProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	Zipline_GetRopeColorModulation(pStruct, (float*)pOut);
}

// S3 CZipline carries neither field. Idle values, not aliased entity memory:
// 0 reverse-mount distance and manual detach allowed.
static void __fastcall ZiplineZero_ValueProxy(void* /*pProp*/, void* /*pStruct*/,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
}

DTExtendProxyFn Zipline_ValueProxyForProp(const char* propName)
{
	if (!propName)
		return nullptr;
	if (strcmp(propName, "m_ropeColorModulation") == 0)
		return &ZiplineRopeColor_ValueProxy;
	if (strcmp(propName, "m_ziplineMountReverseDistance") == 0 ||
		strcmp(propName, "m_ziplinePreventManualDetach") == 0)
		return &ZiplineZero_ValueProxy;
	return nullptr;
}

