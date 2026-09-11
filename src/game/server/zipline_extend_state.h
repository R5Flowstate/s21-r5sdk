//=============================================================================//
//
// Purpose: EHandle-keyed sidecar backing the three S21-only DT_Zipline props.
// CZipline's own fields start at entity+2832, so an appended prop has no
// entity-memory slot on this dedi -- these are served by value proxy.
//
//=============================================================================//
#ifndef ZIPLINE_EXTEND_STATE_H
#define ZIPLINE_EXTEND_STATE_H

#include "game/shared/dt_extend.h"

#ifndef CLIENT_DLL

// S21 CZipline constructor default.
constexpr float kZiplineRopeColorDefault = 1.0f;

void Zipline_SetRopeColorModulation(const void* pZipline, const float* pRGB);
void Zipline_GetRopeColorModulation(const void* pZipline, float* pOutRGB);

// nullptr for any prop this sidecar does not serve.
DTExtendProxyFn Zipline_ValueProxyForProp(const char* propName);

#endif // !CLIENT_DLL

#endif // ZIPLINE_EXTEND_STATE_H
