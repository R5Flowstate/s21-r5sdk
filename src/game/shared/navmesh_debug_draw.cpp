//============================================================================//
// 
// Purpose: NavMesh debug draw implementation
// 
//============================================================================//
#include "tier2/renderutils.h"
#include "navmesh_debug_draw.h"

//------------------------------------------------------------------------------
// Constructors/destructors
//------------------------------------------------------------------------------
rdNavMeshDebugDraw::rdNavMeshDebugDraw()
{
	m_currentVertCount = 0;
	m_expectedVertCount = 0;
	m_primitiveSize = 0.0f;

	m_drawOffset.Init(0.f, 0.f, 0.f);

	for (int i = 0; i < V_ARRAYSIZE(m_primitiveVertices); i++)
		m_primitiveVertices[i].Init(0.f, 0.f, 0.f);

	m_currentPrimitive = duDebugDrawPrimitives::DU_DRAW_UNDEFINED;
	m_primitiveColor.SetColor(0, 0, 0, 0);
}

//------------------------------------------------------------------------------
// 
//------------------------------------------------------------------------------
void rdNavMeshDebugDraw::depthMask(bool state)
{
	// Unused and unimplemented
	rdIgnoreUnused(state);
}

//------------------------------------------------------------------------------
// 
//------------------------------------------------------------------------------
void rdNavMeshDebugDraw::texture(bool state)
{
	// Unused and unimplemented
	rdIgnoreUnused(state);
}

//------------------------------------------------------------------------------
// Begin collecting the primitives
//------------------------------------------------------------------------------
void rdNavMeshDebugDraw::begin(const duDebugDrawPrimitives prim, const float size, const rdVec3D* offset)
{
	switch (prim)
	{
	case duDebugDrawPrimitives::DU_DRAW_POINTS:
		m_expectedVertCount = 1;
		break;
	case duDebugDrawPrimitives::DU_DRAW_LINES:
		m_expectedVertCount = 2;
		break;
	case duDebugDrawPrimitives::DU_DRAW_TRIS:
		m_expectedVertCount = 3;
		break;
	case duDebugDrawPrimitives::DU_DRAW_QUADS:
		m_expectedVertCount = 4;
		break;

	default:
		rdAssert(0); // Unimplemented primitive.
		break;
	}

	m_currentPrimitive = prim;
	m_currentVertCount = 0;
	m_primitiveSize = size;

	m_drawOffset.Init(offset->x, offset->y, offset->z);
}

//------------------------------------------------------------------------------
// Submit a vertex
//------------------------------------------------------------------------------
void rdNavMeshDebugDraw::vertex(const rdVec3D* pos, unsigned int color)
{
	handleAppend(pos->x, pos->y, pos->z, color);
}

//------------------------------------------------------------------------------
// Submit a vertex
//------------------------------------------------------------------------------
void rdNavMeshDebugDraw::vertex(const float x, const float y, const float z, unsigned int color)
{
	handleAppend(x, y, z, color);
}

//------------------------------------------------------------------------------
// Submit a vertex
//------------------------------------------------------------------------------
void rdNavMeshDebugDraw::vertex(const rdVec3D* pos, unsigned int color, const rdVec2D* uv)
{
	rdIgnoreUnused(uv);
	handleAppend(pos->x, pos->y, pos->z, color);
}

//------------------------------------------------------------------------------
// Submit a vertex
//------------------------------------------------------------------------------
void rdNavMeshDebugDraw::vertex(const float x, const float y, const float z, unsigned int color, const float u, const float v)
{
	rdIgnoreUnused(u);
	rdIgnoreUnused(v);

	handleAppend(x, y, z, color);
}

//------------------------------------------------------------------------------
// Append the vertex and determine if we can render it out
//------------------------------------------------------------------------------
void rdNavMeshDebugDraw::handleAppend(const float x, const float y, const float z, unsigned int color)
{
	rdAssert(m_currentPrimitive != duDebugDrawPrimitives::DU_DRAW_UNDEFINED);

	if (m_currentVertCount == 0)
		m_primitiveColor.SetColor(color);

	m_primitiveVertices[m_currentVertCount++].Init(x + m_drawOffset.x, y + m_drawOffset.y, z + m_drawOffset.z);

	if (m_currentVertCount != m_expectedVertCount)
		return;

	switch (m_currentPrimitive)
	{
	case duDebugDrawPrimitives::DU_DRAW_POINTS:
		if (m_primitiveColor.a() == 0)
		{
			m_primitiveColor[3] = 255;
			RenderWireframeSphere(m_primitiveVertices[0], m_primitiveSize, 5, 5, m_primitiveColor, true);
		}
		else
			RenderSphere(m_primitiveVertices[0], m_primitiveSize, 5, 5, m_primitiveColor, true);
		break;
	case duDebugDrawPrimitives::DU_DRAW_LINES:
		RenderLine(m_primitiveVertices[0], m_primitiveVertices[1], m_primitiveColor, true);
		break;
	case duDebugDrawPrimitives::DU_DRAW_TRIS:
		RenderTriangle(m_primitiveVertices[0], m_primitiveVertices[1], m_primitiveVertices[2], m_primitiveColor, true);
		break;
	case duDebugDrawPrimitives::DU_DRAW_QUADS:
		RenderTriangle(m_primitiveVertices[0], m_primitiveVertices[1], m_primitiveVertices[2], m_primitiveColor, true);
		RenderTriangle(m_primitiveVertices[1], m_primitiveVertices[3], m_primitiveVertices[2], m_primitiveColor, true);
		break;

	default:
		rdAssert(0); // Unimplemented primitive.
		break;
	}

	m_currentVertCount = 0;
}

//------------------------------------------------------------------------------
// End collecting the primitives
//------------------------------------------------------------------------------
void rdNavMeshDebugDraw::end()
{
	// If hit, not all vertices were provided during begin and end.
	// This means there was a mismatch between the number of given
	// vertices and the number required for selected primitive type.
	rdAssert(m_currentVertCount == 0);

	m_currentPrimitive = duDebugDrawPrimitives::DU_DRAW_UNDEFINED;
	m_expectedVertCount = 0;
}

//------------------------------------------------------------------------------
// Get the polygon face color for given area ID
//------------------------------------------------------------------------------
unsigned int rdNavMeshDebugDraw::areaToFaceCol(const unsigned int area) const
{
	switch (area)
	{
		// Ground: light blue
	case DT_POLYAREA_GROUND: return duRGBA(0, 192, 215, 255);
		// Jump: blue
	case DT_POLYAREA_JUMP: return duRGBA(0, 0, 255, 255);
		// Trigger: light green
	case DT_POLYAREA_TRIGGER: return duRGBA(20, 245, 0, 255);
		// Unexpected: white
	default: return duRGBA(255, 255, 255, 255);
	}
}

//------------------------------------------------------------------------------
// Get the polygon edge color for given area ID
//------------------------------------------------------------------------------
unsigned int rdNavMeshDebugDraw::areaToEdgeCol(const unsigned int area) const
{
	switch (area)
	{
		// Ground: light blue
	case DT_POLYAREA_GROUND: return duRGBA(0, 24, 32, 255);
		// Jump: blue
	case DT_POLYAREA_JUMP: return duRGBA(0, 0, 48, 255);
		// Trigger: light green
	case DT_POLYAREA_TRIGGER: return duRGBA(0, 32, 24, 255);
		// Unexpected: white
	default: return duRGBA(28, 28, 28, 255);
	}
}
