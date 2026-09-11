#ifndef MESHBUILDER_H
#define MESHBUILDER_H
#include "imatrendercontext.h"

class CMeshVertexBuilder
{
public:
	bool Begin(CMatRenderContext* const ctx, const int vertexCount);

	inline void AppendVertex(const Vector3D& vec, const Color color)
	{
		m_vertexBuffer->pos = vec;
		m_vertexBuffer->col = color;
#if defined(CLIENT_DLL)
		m_vertexBuffer->pad[0] = 0;
		m_vertexBuffer->pad[1] = 0;
#endif // CLIENT_DLL
		m_vertexBuffer++;

#ifdef _DEBUG
		m_vertexCount++;
#endif // DEBUG
	}

	void End(CMatRenderContext* const ctx) const;
	inline const DirectDrawVertexParams_s* GetParams() const { return &m_vertexParams; }

private:
	struct MeshVertex_s
	{
		Vector3D pos;
		Color col;
#if defined(CLIENT_DLL)
		uint32_t pad[2]; // S21 dynamic vertex stride is 24 bytes.
#endif // CLIENT_DLL
	};

	MeshVertex_s* m_vertexBuffer;
	DirectDrawVertexParams_s m_vertexParams;

#ifdef _DEBUG
	int m_vertexCount;
#endif // DEBUG
};

class CMeshIndexBuilder
{
public:
	bool Begin(CMatRenderContext* const ctx, const int indexCount);

	void AppendIndex(const u16 index)
	{
		*m_indexBuffer = index;
		m_indexBuffer++;

#ifdef _DEBUG
		m_indexCount++;
#endif // DEBUG
	}

	void End(CMatRenderContext* const ctx);
	inline const DirectDrawIndexParams_s* GetParams() const { return &m_indexParams; }

private:
	u16* m_indexBuffer;
	DirectDrawIndexParams_s m_indexParams;

#ifdef _DEBUG
	int m_indexCount;
#endif // DEBUG
};

#endif // MESHBUILDER_H
