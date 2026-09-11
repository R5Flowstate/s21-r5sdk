#ifndef MATRENDERCONTEXT_H
#define MATRENDERCONTEXT_H
#include "materialsystem/imaterial.h"
#include "materialsystem/imatrendercontext.h"

#if defined(CLIENT_DLL)
// r5apex_dx12.exe inserted 3 methods between EndRenderer (slot 1) and Bind
// (slot 27). client.dll is injected into both exes, so the shift is runtime.
// Verified against native DrawAllOverlays / RenderLine: Bind 27/30,
// PushDynamicGeoPool 92/95, MapDynamicVertices 94/97, DrawLineList 113/116.
extern bool SDK_IsDx12Exe();
inline int MatRenderContext_Slot(const int dx11Slot)
{
    if (dx11Slot >= 27 && SDK_IsDx12Exe())
        return dx11Slot + 3;
    return dx11Slot;
}
#endif // CLIENT_DLL

class CMatRenderContext
{
public:

    inline void EndRenderer()
    {
        const static int index = 1;
        CallVFunc<void>(index, this);
    }

    inline void Bind(IMaterial* const material)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(27);
#else
        const static int index = 19;
#endif // CLIENT_DLL
        CallVFunc<void>(index, this, material);
    }

    inline void* GetDynamicVertexBuffer(const int vertexCount, DirectDrawVertexParams_s* const drawParams, const int unknown)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(94);
#else
        const static int index = 83;
#endif // CLIENT_DLL
        return CallVFunc<void*>(index, this, vertexCount, drawParams, unknown);
    }

    inline void EndDynamicVertexBuffer(const int vertexCount)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(96);
#else
        const static int index = 85;
#endif // CLIENT_DLL
        return CallVFunc<void>(index, this, vertexCount);
    }

    inline void* GetDynamicIndexBuffer(const int indexCount, DirectDrawIndexParams_s* const indexParams)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(86);
#else
        const static int index = 86;
#endif // CLIENT_DLL
        return CallVFunc<void*>(index, this, indexCount, indexParams);
    }

    inline void EndDynamicIndexBuffer(const int indexCount, u16* const indexBuffer)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(88);
#else
        const static int index = 88;
#endif // CLIENT_DLL
        return CallVFunc<void>(index, this, indexCount, indexBuffer);
    }

    inline void DrawTriangleListIndexed(const DirectDrawVertexParams_s* const vertexParams, const DirectDrawIndexParams_s* const indexParams, const int unknown)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(89);
#else
        const static int index = 89;
#endif // CLIENT_DLL
        return CallVFunc<void>(index, this, vertexParams, indexParams, unknown);
    }

    inline void DrawTriangleList(const DirectDrawVertexParams_s* const drawParams, ID3D11SamplerState* const samplerState, const int unknown)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(98);
#else
        const static int index = 98;
#endif // CLIENT_DLL
        return CallVFunc<void>(index, this, drawParams, samplerState, unknown);
    }

    inline void DrawTriangleStrip(const DirectDrawVertexParams_s* const drawParams, ID3D11SamplerState* const samplerState, const int unknown)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(99);
#else
        const static int index = 99;
#endif // CLIENT_DLL
        return CallVFunc<void>(index, this, drawParams, samplerState, unknown);
    }

    inline void DrawLineList(const DirectDrawVertexParams_s* const drawParams, ID3D11SamplerState* const samplerState, const int unknown)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(113);
#else
        const static int index = 102;
#endif // CLIENT_DLL
        return CallVFunc<void>(index, this, drawParams, samplerState, unknown);
    }

#if defined(CLIENT_DLL)
    // Without the push, MapDynamicVertices returns null and RenderLine writes
    // nothing. Pool 1 is the only pool this build provisions.
    inline void PushDynamicGeoPool(const int pool)
    {
        const static int index = MatRenderContext_Slot(92);
        CallVFunc<void>(index, this, pool);
    }
    inline void PopDynamicGeoPool(void)
    {
        const static int index = MatRenderContext_Slot(93);
        CallVFunc<void>(index, this);
    }
#endif // CLIENT_DLL

    inline void DrawPointList(const DirectDrawVertexParams_s* const drawParams, ID3D11SamplerState* const samplerState, const int unknown)
    {
#if defined(CLIENT_DLL)
        const static int index = MatRenderContext_Slot(103);
#else
        const static int index = 103;
#endif // CLIENT_DLL
        return CallVFunc<void>(index, this, drawParams, samplerState, unknown);
    }
};

#endif // MATRENDERCONTEXT_H
