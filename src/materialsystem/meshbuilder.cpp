#include "cmatrendercontext.h"
#include "materialsystem/meshbuilder.h"

bool CMeshVertexBuilder::Begin(CMatRenderContext* const ctx, const int vertexCount)
{
#ifdef _DEBUG
    m_vertexCount = 0;
#endif // _DEBUG

    m_vertexParams.vertexFormat.position3d = 1;
    m_vertexParams.vertexFormat.color = 1;
#if defined(CLIENT_DLL)
    // S21 input layout expects these extra format bits.
    m_vertexParams.vertexFormat.unknown3 = 1;
    m_vertexParams.vertexFormat.texCoordFlags = 2;
#endif // CLIENT_DLL
    m_vertexParams.vertexStructSize = sizeof(MeshVertex_s);
    m_vertexParams.vertexBlockIndex = 0;
    m_vertexParams.vertexBufferOffset = 0;
    m_vertexParams.vertexCount = 0;

    m_vertexBuffer = (MeshVertex_s*)ctx->GetDynamicVertexBuffer(vertexCount, &m_vertexParams, 13);
    return m_vertexBuffer != nullptr;
}

void CMeshVertexBuilder::End(CMatRenderContext* const ctx) const
{
#ifdef _DEBUG
    // If this gets fired, you processed more or less
    // vertices than allocated.
    Assert(m_vertexCount == m_vertexParams.vertexCount);
#endif // _DEBUG
    ctx->EndDynamicVertexBuffer(m_vertexParams.vertexCount);
}

bool CMeshIndexBuilder::Begin(CMatRenderContext* const ctx, const int indexCount)
{
#ifdef _DEBUG
    m_indexCount = 0;
#endif // _DEBUG

    m_indexBuffer = (u16*)ctx->GetDynamicIndexBuffer(indexCount, &m_indexParams);
    return m_indexBuffer != nullptr;
}

void CMeshIndexBuilder::End(CMatRenderContext* const ctx)
{
#ifdef _DEBUG
    // If this gets fired, you processed more or less
    // indices than allocated.
    Assert(m_indexCount == m_indexParams.indexCount);
#endif // _DEBUG
    ctx->EndDynamicIndexBuffer(m_indexParams.indexCount, m_indexBuffer);
}
