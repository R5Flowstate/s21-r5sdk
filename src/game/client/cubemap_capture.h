//=============================================================================//
//
// Purpose: In-engine cubemap capture. Renders the main view from every map
// cubemap origin, one face at a time, reads the HDR scene back and writes
// each face as a float image for the map compiler to build the cubemap array.
//
//=============================================================================//
#ifndef CLIENT_CUBEMAP_CAPTURE_H
#define CLIENT_CUBEMAP_CAPTURE_H

// Records the loaded map's cubemap origins (packed float3 origins).
void CubemapCapture_SetSamples(const char* pszLoadName, const void* pSamples, int nCount);

// Called from the local player's view calculation; while a capture runs it
// places the camera and returns true.
bool CubemapCapture_OverrideView(float* pOrigin, float* pAngles, float* pFov);


#endif // CLIENT_CUBEMAP_CAPTURE_H
