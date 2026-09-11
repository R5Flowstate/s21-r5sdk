//
// Copyright (c) 2009-2010 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#include "Recast/Include/Recast.h"
#include "Shared/Include/SharedAssert.h"
#include "Shared/Include/SharedCommon.h"
#include "Detour/Include/DetourNavMesh.h"
#include "Detour/Include/DetourNavMeshQuery.h"
#include "Detour/Include/DetourNavMeshBuilder.h"
#include "DetourCrowd/Include/DetourCrowd.h"
#include "DebugUtils/Include/RecastDebugDraw.h"
#include "DebugUtils/Include/DetourDebugDraw.h"
#include "NavEditor/Include/InputGeom.h"
#include "NavEditor/Include/Editor.h"
#include "NavEditor/Include/PerfTimer.h"

#include "game/server/ai_navmesh.h"
#include "game/server/ai_hull.h"
#include "coordsize.h"

unsigned int EditorDebugDraw::areaToFaceCol(const unsigned int area) const
{
	switch(area)
	{
	// Ground : light blue
	case DT_POLYAREA_GROUND: return duRGBA(0, 192, 215, 255);
	// Jump : blue
	case DT_POLYAREA_JUMP: return duRGBA(0, 0, 255, 255);
	// Trigger : light green
	case DT_POLYAREA_TRIGGER: return duRGBA(20, 245, 0, 255);
	// Unexpected : white
	default: return duRGBA(255, 255, 255, 255);
	}
}

unsigned int EditorDebugDraw::areaToEdgeCol(const unsigned int area) const
{
	switch (area)
	{
		// Ground : light blue
	case DT_POLYAREA_GROUND: return duRGBA(0, 24, 32, 255);
		// Jump : blue
	case DT_POLYAREA_JUMP: return duRGBA(0, 0, 48, 255);
		// Trigger : light green
	case DT_POLYAREA_TRIGGER: return duRGBA(0, 32, 24, 255);
		// Unexpected : white
	default: return duRGBA(28, 28, 28, 255);
	}
}

// Standable surface z-normal threshold used by this engine family.
static constexpr float kAgentStandableNormalZ = 0.7f;
// degrees(acos(0.7)); agent max slope expresses the same quantity.
static const float kAgentMaxSlopeDegrees = rdRadToDeg(acosf(kAgentStandableNormalZ));

static unsigned int s_traverseAnimTraverseFlags[TraverseAnimType_e::ANIMTYPE_COUNT];

static void initTraverseMasks()
{
	s_traverseAnimTraverseFlags[ANIMTYPE_HUMAN] = 0x0000013F;
#if DT_NAVMESH_SET_VERSION == 5
	s_traverseAnimTraverseFlags[ANIMTYPE_SPECTRE] = 0x000BFF7E;
	s_traverseAnimTraverseFlags[ANIMTYPE_STALKER] = 0x001BDF7F;
	s_traverseAnimTraverseFlags[ANIMTYPE_FRAG_DRONE] = 0x001BFFFF;
#else
	s_traverseAnimTraverseFlags[ANIMTYPE_SPECTRE] = 0x0013FF7E;
	s_traverseAnimTraverseFlags[ANIMTYPE_STALKER] = 0x0033DF7F;
	s_traverseAnimTraverseFlags[ANIMTYPE_FRAG_DRONE] = 0x0033FFFF;
#endif
	s_traverseAnimTraverseFlags[ANIMTYPE_PILOT] = 0x0008013F;
	s_traverseAnimTraverseFlags[ANIMTYPE_PROWLER] = 0x00033FB7;
	s_traverseAnimTraverseFlags[ANIMTYPE_SUPER_SPECTRE] = 0x00033FB2;
	s_traverseAnimTraverseFlags[ANIMTYPE_TITAN] = 0x00000030;
	s_traverseAnimTraverseFlags[ANIMTYPE_GOLIATH] = 0x00000030; // TODO: figure out all the activities GOLIATH has.
}

TraverseType_s s_traverseTable[NUM_TRAVERSE_TYPES];

static void initTraverseTableParams()
{
	s_traverseTable[0] =  { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused

	s_traverseTable[1]  = { 0.f,  120.f, 0.f,  48.f,  0.f, 67.f, 0.f, false };
	s_traverseTable[2]  = { 120.f, 160.f, 48.f, 96.f,  5.f, 78.f, 0.f, false };
	s_traverseTable[3]  = { 160.f, 220.f, 0.f,  128.f, 0.f, 38.f, 0.f, false };

	s_traverseTable[4]  = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[5]  = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[6]  = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused

	s_traverseTable[7]  = { 800.f, 1220.f, 0.f,   96.f,  0.0f, 6.5f,  0.f, true };
	s_traverseTable[8]  = { 70.f,  220.f,  48.f,  220.f, 19.f, 84.f,  0.f,  false };
	s_traverseTable[9]  = { 210.f, 450.f,  168.f, 384.f, 27.f, 87.5f, 0.f,  false };
	s_traverseTable[10] = { 450.f, 950.f,  384.f, 950.f, 44.f, 89.5f, 0.f,  false };
	s_traverseTable[11] = { 410.f, 800.f,  0.f,   56.f,  0.f , 7.f,   0.f,  true };
	s_traverseTable[12] = { 640.f, 930.f,  348.f, 640.f, 2.2f, 47.f,  0.f,  true };
	s_traverseTable[13] = { 810.f, 1220.f, 256.f, 640.f, 5.7f, 58.5f, 0.f,  true };

	s_traverseTable[14] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1, false }; // Unused
	s_traverseTable[15] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1, false }; // Off-mesh links only, see 'level_script.ent'

	s_traverseTable[16] = { 220.f, 410.f, 0.f,   104.f, 0.f,  12.5f, 0.f, false };
	s_traverseTable[17] = { 210.f, 580.f, 104.f, 416.f, 4.6f, 53.f,  0.f, true };

	s_traverseTable[18] = { 0.0f, 0.0f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Off-mesh links only, see 'level_script.ent'

#if DT_NAVMESH_SET_VERSION > 5
	s_traverseTable[19] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false };    // Unused
	s_traverseTable[20] = { 210.f, 450.f, 168.f, 384.f, 34.f, 89.f, 0.f, false }; // Maps to type 19 in MSET < 7
	s_traverseTable[21] = { 450.f, 860.f, 340.f, 850.f, 46.f, 89.f, 0.f, false }; // Maps to type 20 in MSET < 7
#else
	s_traverseTable[19] = { 210.f, 450.f, 168.f, 384.f, 34.f, 89.f, 0.f, false }; // Maps to type 20 in MSET >= 7
	s_traverseTable[20] = { 450.f, 860.f, 340.f, 850.f, 46.f, 89.f, 0.f, false }; // Maps to type 21 in MSET >= 7
	s_traverseTable[21] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false };    // Unused
#endif
	s_traverseTable[22] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[23] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[24] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Does not exist in MSET 5 ~ 8.
	s_traverseTable[25] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[26] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[27] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[28] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[29] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[30] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
	s_traverseTable[31] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, false }; // Unused
}

Editor::Editor() :
	m_geom(0),
	m_navMesh(0),
	m_navQuery(0),
	m_crowd(0),
	m_navMeshDrawFlags(
		DU_DRAW_DETOURMESH_OFFMESHCONS|DU_DRAW_DETOURMESH_WITH_CLOSED_LIST|DU_DRAW_DETOURMESH_POLY_FACES|
		DU_DRAW_DETOURMESH_POLY_BOUNDS_INNER|DU_DRAW_DETOURMESH_POLY_BOUNDS_OUTER|DU_DRAW_DETOURMESH_ALPHA),
	m_ignoreWindingOrder(false),
	m_filterLowHangingObstacles(true),
	m_filterLedgeSpans(true),
	m_filterNeighborSlopes(false),
	m_filterWalkableLowHeightSpans(true),
	m_buildTraversePortals(true),
	m_traverseRayDynamicOffset(true),
	m_traverseLinkSinglePortalPerPolyPair(false),
	m_collapseLinkedPolyGroups(false),
	m_buildBvTree(true),
	m_selectedNavMeshType(NAVMESH_SMALL),
	m_loadedNavMeshType(NAVMESH_SMALL),
	m_navmeshName(NavMesh_GetNameForType(NAVMESH_SMALL)),
	m_inputMeshCacheDirty(true),
	m_navMeshCacheDirty(true),
	m_tool(0),
	m_ctx(0)
{
	resetCommonSettings();
	m_navQuery = dtAllocNavMeshQuery();
	m_crowd = dtAllocCrowd();

	for (int i = 0; i < MAX_TOOLS; i++)
		m_toolStates[i] = 0;

	m_recastDrawOffset.init(0.0f,0.0f,0.0f);
	m_detourDrawOffset.init(0.0f,0.0f,4.0f);
}

Editor::~Editor()
{
	dtFreeNavMeshQuery(m_navQuery);
	dtFreeNavMesh(m_navMesh);
	dtFreeCrowd(m_crowd);
	delete m_tool;
	for (int i = 0; i < MAX_TOOLS; i++)
		delete m_toolStates[i];
	if (m_inputMeshCache.vboPos) glDeleteBuffers(1, &m_inputMeshCache.vboPos);
	if (m_inputMeshCache.vboColor) glDeleteBuffers(1, &m_inputMeshCache.vboColor);
	if (m_inputMeshCache.vboUV) glDeleteBuffers(1, &m_inputMeshCache.vboUV);
	if (m_navMeshCache.vboPos) glDeleteBuffers(1, &m_navMeshCache.vboPos);
	if (m_navMeshCache.vboColor) glDeleteBuffers(1, &m_navMeshCache.vboColor);
	if (m_navMeshCache.vboUV) glDeleteBuffers(1, &m_navMeshCache.vboUV);
}

void Editor::setTool(EditorTool* tool)
{
	delete m_tool;
	m_tool = tool;
	if (tool)
		m_tool->init(this);
}

void Editor::handleSettings()
{
}

void Editor::handleTools()
{
}

void Editor::handleDebugMode()
{
}

void Editor::handleRender()
{
	if (!m_geom)
		return;
	
	// Draw mesh
	duDebugDrawTriMesh(&m_dd, m_geom->getMesh()->getVerts(), m_geom->getMesh()->getVertCount(),
					   m_geom->getMesh()->getTris(), m_geom->getMesh()->getNormals(), m_geom->getMesh()->getTriCount(), 0, 1.0f, nullptr);
	// Draw bounds
	const rdVec3D* bmin = m_geom->getMeshBoundsMin();
	const rdVec3D* bmax = m_geom->getMeshBoundsMax();
	duDebugDrawBoxWire(&m_dd, bmin->x,bmin->y,bmin->z, bmax->x,bmax->y,bmax->z, duRGBA(255,255,255,128), 1.0f, nullptr);
}

void Editor::handleRenderOverlay(double* /*model*/, double* /*proj*/, int* /*view*/)
{
}

void Editor::handleMeshChanged(InputGeom* geom)
{
	m_geom = geom;
	m_inputMeshCacheDirty = true;

	const BuildSettings* buildSettings = geom->getBuildSettings();
	if (buildSettings)
	{
		m_cellSize = buildSettings->cellSize;
		m_cellHeight = buildSettings->cellHeight;
		m_agentHeight = buildSettings->agentHeight;
		m_agentRadius = buildSettings->agentRadius;
		m_agentMaxClimb = buildSettings->agentMaxClimb;
		m_agentMaxSlope = buildSettings->agentMaxSlope;
		m_regionMinSize = buildSettings->regionMinSize;
		m_regionMergeSize = buildSettings->regionMergeSize;
		m_edgeMaxLen = buildSettings->edgeMaxLen;
		m_edgeMaxError = buildSettings->edgeMaxError;
		m_vertsPerPoly = buildSettings->vertsPerPoly;
		m_polyCellRes = buildSettings->polyCellRes;
		m_detailSampleDist = buildSettings->detailSampleDist;
		m_detailSampleMaxError = buildSettings->detailSampleMaxError;
		m_partitionType = buildSettings->partitionType;
	}
}

void Editor::collectSettings(BuildSettings& settings)
{
	settings.cellSize = m_cellSize;
	settings.cellHeight = m_cellHeight;
	settings.agentHeight = m_agentHeight;
	settings.agentRadius = m_agentRadius;
	settings.agentMaxClimb = m_agentMaxClimb;
	settings.agentMaxSlope = m_agentMaxSlope;
	settings.regionMinSize = m_regionMinSize;
	settings.regionMergeSize = m_regionMergeSize;
	settings.edgeMaxLen = m_edgeMaxLen;
	settings.edgeMaxError = m_edgeMaxError;
	settings.vertsPerPoly = m_vertsPerPoly;
	settings.polyCellRes = m_polyCellRes;
	settings.detailSampleDist = m_detailSampleDist;
	settings.detailSampleMaxError = m_detailSampleMaxError;
	settings.partitionType = m_partitionType;
}

void Editor::resetCommonSettings()
{
	selectNavMeshType(NAVMESH_SMALL);

#if DT_NAVMESH_SET_VERSION == 5
	m_minTileBits = 14;
	m_maxTileBits = 22;
#else
	m_minTileBits = 16;
	m_maxTileBits = 28;
#endif
	m_tileSize = 128;

	// Max slope = degrees(acos(standable z-normal 0.7)).
	m_agentMaxSlope = kAgentMaxSlopeDegrees;

	// note(amos): even though the slope-based ledge offset calculation yields
	// very accurate results, in practice the ledge spans are sporadic causing the
	// ray to intersect with geometry while in-game the NPC would've been able to
	// traverse the portal perfectly without clipping into anything. To accommodate
	// for irregularities around more complex geometry, we enforce a default offset.
	// This extra offset is added on top of the ledge span amount which is used to
	// calculate the total ray offset. Therefore, this won't cause links with a
	// lower slope to clip into geometry unless this is being set very high (>40.0).
	m_traverseRayExtraOffset = 4.0f;
	m_traverseEdgeMinOverlap = RD_EPS;
	m_traversePortalMaxAlign = 0.5f;

	m_regionMinSize = 4;
	m_regionMergeSize = 8;

	// note(amos): according to Recast documentation, the best value should be
	// around walkableRadius * 8, however with titanfall and apex legends maps
	// the tessellation results are way better with this feature disabled.
	m_edgeMaxLen = 0;
	m_edgeMaxError = 1.3f;
	m_vertsPerPoly = RD_VERTS_PER_POLYGON;
	m_detailSampleDist = 6.0f;
	m_detailSampleMaxError = 3.0f;
	m_partitionType = EDITOR_PARTITION_WATERSHED;

	updateTraverseLinkRenderParams();

	initTraverseMasks();
	initTraverseTableParams();
}

void Editor::updateTraverseLinkRenderParams()
{
	m_traverseLinkDrawParams.cellHeight = m_cellHeight;
	m_traverseLinkDrawParams.extraOffset = (m_agentRadius*2) + m_traverseRayExtraOffset;
	m_traverseLinkDrawParams.dynamicOffset = m_traverseRayDynamicOffset;
}

void Editor::drawDisplayListFast(DisplayListCache& cache, duDebugDraw* dd)
{
	duDisplayList& dl = cache.list;
	const int numSegs = dl.segmentCount();
	if (!numSegs) return;

	// Upload to VBOs if data has changed.
	if (cache.vboDirty)
	{
		if (!cache.vboPos)
		{
			glGenBuffers(1, &cache.vboPos);
			glGenBuffers(1, &cache.vboColor);
			glGenBuffers(1, &cache.vboUV);
		}

		glBindBuffer(GL_ARRAY_BUFFER, cache.vboPos);
		glBufferData(GL_ARRAY_BUFFER, dl.size() * sizeof(rdVec3D), dl.getPositions(), GL_DYNAMIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, cache.vboColor);
		glBufferData(GL_ARRAY_BUFFER, dl.size() * sizeof(unsigned int), dl.getColors(), GL_DYNAMIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, cache.vboUV);
		glBufferData(GL_ARRAY_BUFFER, dl.size() * sizeof(rdVec2D), dl.getUVs(), GL_DYNAMIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		cache.vboDirty = false;
	}

	// DU_DRAW_UNDEFINED=0, DU_DRAW_POINTS=1, DU_DRAW_LINES=2, DU_DRAW_TRIS=3, DU_DRAW_QUADS=4
	static const GLenum s_glPrims[] = { 0, GL_POINTS, GL_LINES, GL_TRIANGLES, GL_QUADS };

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	glBindBuffer(GL_ARRAY_BUFFER, cache.vboPos);
	glVertexPointer(3, GL_FLOAT, sizeof(rdVec3D), 0);

	glBindBuffer(GL_ARRAY_BUFFER, cache.vboColor);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(unsigned int), 0);

	for (int s = 0; s < numSegs; ++s)
	{
		const duDisplayList::Segment& seg = dl.getSegment(s);

		if (seg.textured)
		{
			dd->texture(true);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			glBindBuffer(GL_ARRAY_BUFFER, cache.vboUV);
			glTexCoordPointer(2, GL_FLOAT, sizeof(rdVec2D), 0);
		}

		if (seg.prim == DU_DRAW_LINES)
			glLineWidth(seg.primSize);
		else if (seg.prim == DU_DRAW_POINTS)
			glPointSize(seg.primSize);

		glDrawArrays(s_glPrims[seg.prim], seg.startIndex, seg.count);

		if (seg.prim == DU_DRAW_LINES)
			glLineWidth(1.0f);
		else if (seg.prim == DU_DRAW_POINTS)
			glPointSize(1.0f);

		if (seg.textured)
		{
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
			dd->texture(false);
		}
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
}

void Editor::drawInputMeshCached(float maxSlope, float texScale)
{
	if (!m_geom || !m_geom->getMesh())
		return;

	if (m_inputMeshCacheDirty)
	{
		m_inputMeshCache.list.clear();
		duDebugDrawTriMeshSlope(&m_inputMeshCache.list,
			m_geom->getMesh()->getVerts(), m_geom->getMesh()->getVertCount(),
			m_geom->getMesh()->getTris(), m_geom->getMesh()->getNormals(),
			m_geom->getMesh()->getTriCount(),
			maxSlope, texScale, nullptr);
		m_inputMeshCacheDirty = false;
		m_inputMeshCache.vboDirty = true;
	}

	drawDisplayListFast(m_inputMeshCache, &m_dd);
}

void Editor::drawNavMeshCached(unsigned int flags)
{
	if (!m_navMesh || !m_navQuery)
		return;

	if (m_navMeshCacheDirty)
	{
		m_navMeshCache.list.clear();
		duDebugDrawNavMeshWithClosedList(&m_navMeshCache.list, *m_navMesh, *m_navQuery,
			&m_detourDrawOffset, flags, m_traverseLinkDrawParams);
		m_navMeshCacheDirty = false;
		m_navMeshCache.vboDirty = true;
	}

	drawDisplayListFast(m_navMeshCache, &m_dd);
}

void Editor::handleCommonSettings()
{
	ImGui::Text("NavMesh Type");
	for (int i = 0; i < NAVMESH_COUNT; i++)
	{
		const NavMeshType_e navMeshType = NavMeshType_e(i);

		if (ImGui::Button(NavMesh_GetNameForType(navMeshType), ImVec2(120, 0)))
		{
			selectNavMeshType(navMeshType);
		}
	}

	ImGui::Separator();

	ImGui::PushItemWidth(180.f);
	ImGui::Text("Rasterization");

	ImGui::SliderFloat("Cell Size", &m_cellSize, 0.4f, 16.0f);
	
	if (ImGui::SliderFloat("Cell Height", &m_cellHeight, 0.4f, 16.0f))
		m_traverseLinkDrawParams.cellHeight = m_cellHeight;
	
	if (m_geom)
	{
		const rdVec3D* bmin = m_geom->getNavMeshBoundsMin();
		const rdVec3D* bmax = m_geom->getNavMeshBoundsMax();
		int gw = 0, gh = 0;
		rcCalcGridSize(bmin, bmax, m_cellSize, &gw, &gh);
		char text[64];
		snprintf(text, 64, "Voxels: %d x %d", gw, gh);
		ImGui::Text(text);
	}

	ImGui::Checkbox("Ignore Winding Order", &m_ignoreWindingOrder);
	
	ImGui::Separator();
	ImGui::Text("Agent");
	ImGui::SliderFloat("Height", &m_agentHeight, 0.1f, 500.0f);
	if (ImGui::SliderFloat("Radius", &m_agentRadius, 0.0f, 500.0f))
		m_traverseLinkDrawParams.extraOffset = (m_agentRadius*2) + m_traverseRayExtraOffset;
	ImGui::SliderFloat("Max Climb", &m_agentMaxClimb, 0.1f, 250.0f);
	ImGui::SliderFloat("Max Slope", &m_agentMaxSlope, 0.0f, 90.0f);

	ImGui::PopItemWidth();
	ImGui::PushItemWidth(140.f);
	
	ImGui::Separator();
	ImGui::Text("Region");
	ImGui::SliderInt("Min Region Size", &m_regionMinSize, 0, 150);
	ImGui::SliderInt("Merged Region Size", &m_regionMergeSize, 0, 150);

	ImGui::PopItemWidth();

	if (m_geom)
	{
		ImGui::Separator();
		ImGui::Text("Bounding");

		rdVec3D* navMeshBMin = m_geom->getNavMeshBoundsMin();
		rdVec3D* navMeshBMax = m_geom->getNavMeshBoundsMax();

		const rdVec3D* meshBMin = m_geom->getMeshBoundsMin();
		const rdVec3D* meshBMax = m_geom->getMeshBoundsMax();

		ImGui::PushItemWidth(75);
		ImGui::SliderFloat("##BoundingMinsX", &navMeshBMin->x, meshBMin->x, rdMin(meshBMax->x, navMeshBMax->x));
		ImGui::SameLine();
		ImGui::SliderFloat("##BoundingMinsY", &navMeshBMin->y, meshBMin->y, rdMin(meshBMax->y, navMeshBMax->y));
		ImGui::SameLine();
		ImGui::SliderFloat("##BoundingMinsZ", &navMeshBMin->z, meshBMin->z, rdMin(meshBMax->z, navMeshBMax->z));
		ImGui::SameLine();
		ImGui::Text("Mins");

		ImGui::SliderFloat("##BoundingMaxsX", &navMeshBMax->x, rdMax(meshBMin->x, navMeshBMin->x), meshBMax->x);
		ImGui::SameLine();
		ImGui::SliderFloat("##BoundingMaxsY", &navMeshBMax->y, rdMax(meshBMin->y, navMeshBMin->y), meshBMax->y);
		ImGui::SameLine();
		ImGui::SliderFloat("##BoundingMaxsZ", &navMeshBMax->z, rdMax(meshBMin->z, navMeshBMin->z), meshBMax->z);
		ImGui::SameLine();
		ImGui::Text("Maxs");
		ImGui::PopItemWidth();

		if (ImGui::Button("Reset##BoundingSettings", ImVec2(120, 0)))
		{
			rdVcopy(navMeshBMin, m_geom->getOriginalNavMeshBoundsMin());
			rdVcopy(navMeshBMax, m_geom->getOriginalNavMeshBoundsMax());
		}

		ImGui::Checkbox("Build BVTree", &m_buildBvTree);
	}

	ImGui::Separator();
	ImGui::Text("Partitioning");

	bool isEnabled = m_partitionType == EDITOR_PARTITION_WATERSHED;

	if (ImGui::Checkbox("Watershed", &isEnabled))
		m_partitionType = EDITOR_PARTITION_WATERSHED;

	isEnabled = m_partitionType == EDITOR_PARTITION_MONOTONE;

	if (ImGui::Checkbox("Monotone", &isEnabled))
		m_partitionType = EDITOR_PARTITION_MONOTONE;

	isEnabled = m_partitionType == EDITOR_PARTITION_LAYERS;

	if (ImGui::Checkbox("Layers", &isEnabled))
		m_partitionType = EDITOR_PARTITION_LAYERS;
	
	ImGui::Separator();
	ImGui::Text("Filtering");
	ImGui::Checkbox("Low Hanging Obstacles##FilterSettings", &m_filterLowHangingObstacles);
	ImGui::Checkbox("Walkable Low Height Spans##FilterSettings", &m_filterWalkableLowHeightSpans);

	ImGui::Checkbox("Ledge Spans##FilterSettings", &m_filterLedgeSpans);
	if (m_filterLedgeSpans)
	{
		ImGui::Indent();
		ImGui::Checkbox("Neighbor Slopes##FilterSettings", &m_filterNeighborSlopes);
		ImGui::Unindent();
	}

	ImGui::PushItemWidth(145.f);
	ImGui::Separator();

	ImGui::Text("Polygonization");
	ImGui::SliderInt("Max Edge Length", &m_edgeMaxLen, 0, 1024);
	ImGui::SliderFloat("Max Edge Error", &m_edgeMaxError, 0.1f, 3.0f);
	ImGui::SliderInt("Verts Per Poly", &m_vertsPerPoly, 3, 6);

#if DT_NAVMESH_SET_VERSION >= 8
	ImGui::SliderInt("Poly Cell Resolution", &m_polyCellRes, 0, 128);
#endif

	ImGui::Separator();
	ImGui::Text("Detail Mesh");
	ImGui::SliderFloat("Sample Distance", &m_detailSampleDist, 1.0f, 128.0f);
	ImGui::SliderFloat("Max Sample Error", &m_detailSampleMaxError, 0.0f, 128.0f);

	ImGui::PopItemWidth();
	
	ImGui::Separator();
	ImGui::Text("Traversability");

	if (ImGui::CollapsingHeader("Traverse Table Fine Tuner"))
		renderTraverseTableFineTuners();

	ImGui::Checkbox("Build Traverse Portals", &m_buildTraversePortals);

	ImGui::Checkbox("Single Portal Per Poly Pair", &m_traverseLinkSinglePortalPerPolyPair);

	ImGui::Checkbox("Collapse Linked Poly Groups", &m_collapseLinkedPolyGroups);

	if (ImGui::Checkbox("Dynamic Traverse Ray Offset", &m_traverseRayDynamicOffset))
		m_traverseLinkDrawParams.dynamicOffset = m_traverseRayDynamicOffset;

	if (ImGui::SliderFloat("Extra Offset", &m_traverseRayExtraOffset, 0, 128))
		m_traverseLinkDrawParams.extraOffset = (m_agentRadius*2) + m_traverseRayExtraOffset;

	ImGui::SliderFloat("Min Overlap", &m_traverseEdgeMinOverlap, 0.0f, m_tileSize*m_cellSize, "%g");

	ImGui::SliderFloat("Max Align", &m_traversePortalMaxAlign, 0.0f, 0.5f, "%g", ImGuiSliderFlags_AlwaysClamp);

	if (ImGui::Button("Rebuild Static Pathing Data"))
	{
		createStaticPathingData();
		invalidateNavMeshCache();
	}

	ImGui::Separator();
}

void Editor::handleClick(const rdVec3D* s, const rdVec3D* p, const int v, bool shift)
{
	if (m_tool)
		m_tool->handleClick(s, p, v, shift);
}

void Editor::handleToggle()
{
	if (m_tool)
		m_tool->handleToggle();
}

void Editor::handleStep()
{
	if (m_tool)
		m_tool->handleStep();
}

bool Editor::handleBuild()
{
	return true;
}

void Editor::handleUpdate(const float dt)
{
	if (m_tool)
		m_tool->handleUpdate(dt);
	updateToolStates(dt);
}

bool traverseTypeSupported(void* userData, const unsigned char traverseType)
{
	const Editor* editor = ((const TraverseLinkBuildContext*)userData)->editor;
	const NavMeshType_e navMeshType = editor->getSelectedNavMeshType();

	if (navMeshType == NavMeshType_e::NAVMESH_SMALL)
	{
		const int tableCount = NavMesh_GetTraverseTableCountForNavMeshType(navMeshType);

		for (int t = 0; t < tableCount; t++)
		{
			if (rdBitCellBit(traverseType) & s_traverseAnimTraverseFlags[t])
				return true;
		}

		return false;
	}

	const int traverseTableIndex = NavMesh_GetFirstTraverseAnimTypeForType(navMeshType);
	return rdBitCellBit(traverseType) & s_traverseAnimTraverseFlags[traverseTableIndex];
}

unsigned char GetBestTraverseType(void* userData, const float traverseDist, const float elevation, const float slope, const bool baseOverlaps, const bool landOverlaps)
{
	TraverseType_e bestTraverseType = INVALID_TRAVERSE_TYPE;
	float smallestDiff = FLT_MAX;

	for (int i = NUM_TRAVERSE_TYPES-1; i >= 0; --i)
	{
		const TraverseType_s& traverseType = s_traverseTable[i];

		// Skip unused types...
		if (traverseType.minDist == 0.0f && traverseType.maxDist == 0.0f &&
			traverseType.minElev == 0.0f && traverseType.maxElev == 0.0f)
		{
			continue;
		}

		if (!traverseTypeSupported(userData, (unsigned char)i))
			continue;

		if (traverseDist < traverseType.minDist ||
			traverseDist > traverseType.maxDist)
		{
			continue;
		}

		if (elevation < traverseType.minElev ||
			elevation > traverseType.maxElev)
		{
			continue;
		}

		if (slope < traverseType.minSlope ||
			slope > traverseType.maxSlope)
		{
			continue;
		}

		if (traverseType.ovlpTrig > -1 && elevation >= traverseType.ovlpTrig)
		{
			const bool overlaps = traverseType.ovlpExcl 
				? (baseOverlaps || landOverlaps) 
				: (baseOverlaps && landOverlaps);

			if (!overlaps)
				continue;
		}

		const float midDist = (traverseType.minDist+traverseType.maxDist) / 2.0f;
		const float midElev = (traverseType.minElev+traverseType.maxElev) / 2.0f;
		const float midSlope = (traverseType.minSlope+traverseType.maxSlope) / 2.0f;

		const float distDiff = rdMathFabsf(traverseDist-midDist);
		const float elevDiff = rdMathFabsf(elevation-midElev);
		const float slopeDiff = rdMathFabsf(slope-midSlope);

		const float totalDiff = elevDiff+distDiff+slopeDiff;

		if (totalDiff < smallestDiff)
		{
			smallestDiff = totalDiff;
			bestTraverseType = (TraverseType_e)i;
		}
	}

	return (unsigned char)bestTraverseType;
}

static bool polyEdgeFaceAgainst(const rdVec2D* v1, const rdVec2D* v2, const rdVec2D* n1, const rdVec2D* n2)
{
	rdVec2D delta;
	rdVsub2D(&delta, v2, v1);

	return (rdVdot2D(&delta, n1) >= 0 && rdVdot2D(&delta, n2) < 0);
}

// NOTE: we don't want to collide with trigger area's as this would otherwise
// prevent a link from happening between 2 valid slabs that intersect with
// something like a door or action trigger.
// 
// UPDATE: currently we only use door area's to mark polygons under doors, and
// we don't want traverse links to establish between doors. The trigger mask
// has therefore been added. The mask system needs to be reworked to take trigger
// area flags into account and use those too, ideally with the content masks
// provided by the engine itself.
static const int TRAVERSE_LINK_TRACE_MASK = TRACE_WORLD|TRACE_CLIP|TRACE_TRIGGER;

// Links smaller than this do not need 3 rays casted upwards over the span of
// the link to detect overhanging objects. Since there are many small links,
// avoiding 2 additional ray casts will save a lot on build times.
static const float TRAVERSE_LINK_TRIPPLE_TRACE_THRESH = 100.f;

static bool raycastMesh(const InputGeom* geom, const rdVec3D* src, const rdVec3D* dst)
{
	return geom->raycastMesh(src, dst, TRAVERSE_LINK_TRACE_MASK);
}

static bool traverseLinkOffsetIntersectsGeom(const InputGeom* geom, const rdVec3D* basePos, const rdVec3D* offsetPos)
{
	// We need to fire a raycast from out initial
	// high pos to our offset position to make
	// sure we didn't clip into geometry:
	// 
	//                        object geom
	//                        ^
	//     outer navmesh      |
	//     ^                  |
	//     |          !-----------------------!
	//     |      gap !     / potential clip  !
	//     |      ^   !    /                  !
	//     |      |   !   /  inner navmesh    !
	//     |      |   !  /   ^                !
	//     |      |   ! /    |                !
	//   ++++++ <---> !/   +++++++++++++++    !
	// ========================================...
	// 
	// Otherwise we create links between a mesh
	// inside and outside an object, causing the
	// ai to traverse inside of it.
	if (raycastMesh(geom, basePos, offsetPos))
		return true;

	return false;
}

static bool traverseLinkIntersectsPlaneOverPlane(const InputGeom* geom, const rdVec3D* lowPos, const rdVec3D* highPos, const float walkableHeight)
{
	// Make sure we have at least a clearance of
	// the tile's walkable height in order to
	// prevent links like these:
	// 
	//                  object geom
	//                  ^
	//   navmesh        |
	//   ^              |
	//   |   !-------------------------!
	//   |   !    clearance trace      !    traverse link
	//   |   !    ^       ^       ^    !   /
	//   |   !    |       |       |    !  /
	//   |   -----|-------|-------|----- /
	// ++++ *---------------------------* ++++
	// ========================================...
	// 
	// Otherwise the NPC will traverse through
	// objects or geometry which have a very
	// slight elevation creating a gap. The
	// outer clearance traces are only performed
	// if the link is larger than
	// TRAVERSE_LINK_TRIPPLE_TRACE_MIN_DIST.
	// A classic example in which such link
	// could establish is if we have a box on a
	// forklift that is slightly elevated; too
	// low for the hull's walkable height but
	// high enough for a ray to reach the other
	// side of the navmesh unobstructed.

	rdVec3D rayMidStart;
	rdVsad(&rayMidStart, lowPos, highPos, 0.5f);

	rdVec3D rayMidEnd;
	rdVset(&rayMidEnd, rayMidStart.x, rayMidStart.y, rayMidStart.z+walkableHeight);

	if (raycastMesh(geom, &rayMidStart, &rayMidEnd))
		return true;

	const float distance = rdVdist(lowPos, highPos);

	if (distance < TRAVERSE_LINK_TRIPPLE_TRACE_THRESH)
		return false;

	rdVec3D rayMidMidStart;

	rdVsad(&rayMidMidStart, &rayMidStart, lowPos, 0.5f);
	rdVset(&rayMidEnd, rayMidMidStart.x, rayMidMidStart.y, rayMidMidStart.z+walkableHeight);

	if (raycastMesh(geom, &rayMidStart, &rayMidEnd))
		return true;

	rdVsad(&rayMidMidStart, &rayMidStart, highPos, 0.5f);
	rdVset(&rayMidEnd, rayMidMidStart.x, rayMidMidStart.y, rayMidMidStart.z+walkableHeight);

	if (raycastMesh(geom, &rayMidStart, &rayMidEnd))
		return true;

	return false;
}

static bool traverseLinkIntersectsOverhangOverPoint(const InputGeom* geom, const rdVec3D* startPos, const float walkableHeight)
{
	// Make sure we have enough clearance over
	// the kink of the link. The kink is formed
	// at a distance of walkableRadius from the
	// ledge, which is ultimately where the NPC
	// will be during the traversal of the link.
	// We have to make sure that when the NPC is
	// at this kink, that it does not clip into
	// overhanging geometry as shown below:
	// 
	// -----------------!
	// clearance ray    ! --> overhanging object
	//             ^    !
	//             |    !          upper navmesh
	// ------------|----!          ^
	//             |               |
	//             |               |
	// kink <----- *--------* +++++++++++++++++
	//             |      !--------------------
	//             |      !
	// link <----- |      !
	//             |      !
	//             |      !
	//             |      !
	//  ++++++++++ *      !
	// ========================================...

	// note(amos): walkableHeight*2 because some traverse animations
	// will make the NPC extend well beyond their walkable height on
	// the kind point of the traverse portal. We need to accommodate
	// for these to avoid them clipping into geometry.
	const rdVec3D minClearanceHeight = {
		startPos->x,
		startPos->y,
		startPos->z + (walkableHeight*2)
	};

	return raycastMesh(geom, &minClearanceHeight, startPos);
}

static bool traverseLinkInLOS(void* userData, const rdVec3D* lowPos, const rdVec3D* highPos, const rdVec2D* lowNorm,
	const rdVec2D* highNorm, const float walkableHeight, const float walkableRadius, const float slopeAngle)
{
	Editor* editor = ((TraverseLinkBuildContext*)userData)->editor;
	InputGeom* geom = editor->getInputGeom();

	const float extraOffset = editor->getTraverseRayExtraOffset();
	const float cellHeight = editor->getCellHeight();
	float offsetAmount;

	if (editor->useDynamicTraverseRayOffset())
	{
		// note(amos): walkableRadius*2 because the theoretical
		// distance between the poly edge and the center point
		// of the hull is twice the walkable radius, we have to
		// take this into account so our kink point moves over
		// the geometry ledge. The extra offset will account for
		// small irregularities in ledge spans.
		const float totLedgeSpan = (walkableRadius*2) + extraOffset;
		const float maxAngle = rdCalcMaxLOSAngle(totLedgeSpan, cellHeight);

		offsetAmount = rdCalcLedgeSpanOffsetAmount(totLedgeSpan, slopeAngle, maxAngle);
	}
	else
		offsetAmount = walkableRadius + extraOffset;

	// Detect overhangs to avoid links like these:
	// 
	//         geom             upper navmesh
	//     gap ^                ^
	//     ^   |                |
	//     |   |                |
	// * <---> | ++++++++++++++++++++++++++++++...
	//  \======================================...
	//   \        |
	//    \       |
	//     \      |---> overhang
	//      \
	//       \
	//        \-----> link
	//     gap \               lower navmesh
	//       ^  \              ^
	//       |   *             |
	//     <----> +++++++++++++++++++++++++++++...
	//     ====================================...
	// 
	// The AI would otherwise attempt to initiate
	// the jump from the lower navmesh, causing it
	// to clip through geometry.
	if (!polyEdgeFaceAgainst(lowPos, highPos, lowNorm, highNorm))
		return false;

	const rdVec3D* targetRayPos = highPos;

	// We offset the highest point with at least the
	// walkable radius, and perform a raycast test
	// from the highest point to the lowest. The
	// offsetting is necessary to account for the
	// gap between the edge of the navmesh and the
	// edge of the geometry shown below:
	// 
	//                          geom    upper navmesh
	//                          ^       ^
	//                     gap  |       |
	//                     ^    |       |
	//                     |    |       |
	//           offset <-----> | +++++++++++++...
	//                * =======================...
	//               /
	//     ray <----/     lower navmesh
	//             /      ^
	//     geom   /       |
	//     ^     *        |
	//     |    +++++++++++++++++++++++++++++++...
	// ========================================...
	// 
	// We only want the raycast test to fail if the
	// ledge is larger than usual, when the low and
	// high positions are angled in such way no LOS
	// is possible, or when there's an actual object
	// between the 2 positions.
	rdVec3D offsetRayPos;

	if (offsetAmount > 0)
	{
		offsetRayPos.x = highPos->x + highNorm->x * offsetAmount;
		offsetRayPos.y = highPos->y + highNorm->y * offsetAmount;
		offsetRayPos.z = highPos->z;

		if (traverseLinkOffsetIntersectsGeom(geom, highPos, &offsetRayPos))
			return false;

		targetRayPos = &offsetRayPos;
	}

	// Check if the path between the ground position and the kink point
	// is clear.
	if (raycastMesh(geom, targetRayPos, lowPos))
		return false;

	const float angle = rdCalcSlopeAngle(lowPos, highPos);
	const float maxAngle = editor->getAgentSlope();

	// Only perform this test if the link runs over a possible walkable surface.
	if (angle < maxAngle)
	{
		// Check if we don't traverse through a hole in geometry that has an
		// overhanging object.
		if (traverseLinkIntersectsPlaneOverPlane(geom, lowPos, targetRayPos, walkableHeight))
			return false;
	}

	// Check if the clearance between our portal points and the potential
	// ceiling is large enough. Check highPos first as we are more likely
	// to intersect from here.
	if (traverseLinkIntersectsOverhangOverPoint(geom, targetRayPos, walkableHeight) ||
		traverseLinkIntersectsOverhangOverPoint(geom, lowPos, walkableHeight))
		return false;

	return true;
}

static unsigned int* findFromPolyMap(void* userData, const dtPolyRef basePolyRef, const dtPolyRef landPolyRef)
{
	TraverseLinkBuildContext* ctx = (TraverseLinkBuildContext*)userData;
	std::shared_lock<std::shared_mutex> lock(*ctx->polyMapMutex);

	auto it = ctx->editor->getTraverseLinkPolyMap().find(TraverseLinkPolyPair(basePolyRef, landPolyRef));

	if (it == ctx->editor->getTraverseLinkPolyMap().end())
		return nullptr;

	return &it->second;
}

static int addToPolyMap(void* userData, const dtPolyRef basePolyRef, const dtPolyRef landPolyRef, const unsigned int traverseTypeBit)
{
	TraverseLinkBuildContext* ctx = (TraverseLinkBuildContext*)userData;
	std::unique_lock<std::shared_mutex> lock(*ctx->polyMapMutex);

	try
	{
		const auto ret = ctx->editor->getTraverseLinkPolyMap().emplace(TraverseLinkPolyPair(basePolyRef, landPolyRef), traverseTypeBit);
		if (!ret.second)
		{
			rdAssert(ret.second); // Called 'addToPolyMap' while poly link already exists.
			return 1;
		}
	}
	catch (const std::bad_alloc& /*e*/)
	{
		return -1;
	}

	return 0;
}

void Editor::createTraverseLinkParams(dtTraverseLinkConnectParams& params)
{
	params.getTraverseType = &GetBestTraverseType;
	params.traverseLinkInLOS = &traverseLinkInLOS;
	params.findPolyLink = &findFromPolyMap;
	params.addPolyLink = &addToPolyMap;

	params.userData = nullptr; // Set by caller with appropriate context.
	params.minEdgeOverlap = m_traverseEdgeMinOverlap;
	params.maxPortalAlign = m_traversePortalMaxAlign;
	params.singlePortalPerPair = m_traverseLinkSinglePortalPerPolyPair;
}

bool Editor::createTraverseLinks()
{
	rdAssert(m_navMesh);
	m_traverseLinkPolyMap.clear();

	std::shared_mutex polyMapMutex;
	TraverseLinkBuildContext buildCtx;
	buildCtx.editor = this;
	buildCtx.polyMapMutex = &polyMapMutex;

	dtTraverseLinkConnectParams params;
	createTraverseLinkParams(params);
	params.userData = &buildCtx;

	const int maxTiles = m_navMesh->getMaxTiles();
	const int numWorkers = rdMax(1, (int)std::thread::hardware_concurrency() - 1);

	// Collect active tiles grouped by position.
	struct TileInfo { int tileIndex; dtTileRef ref; int tx; int ty; };
	std::vector<TileInfo> activeTiles;

	for (int i = 0; i < maxTiles; i++)
	{
		dtMeshTile* tile = m_navMesh->getTile(i);
		if (!tile || !tile->header)
			continue;

		TileInfo info;
		info.tileIndex = i;
		info.ref = m_navMesh->getTileRef(tile);
		info.tx = tile->header->x;
		info.ty = tile->header->y;
		activeTiles.push_back(info);
	}

	// 3x3 tile grouping: tiles in the same group are spaced 3 apart in both
	// axes, so no two tiles in a group share a neighbor (even diagonally).
	// Each tile does both within-tile and neighbor-tile passes back-to-back,
	// preserving the original per-tile ordering that the poly map depends on.
	for (int gy = 0; gy < 3; gy++)
	{
		for (int gx = 0; gx < 3; gx++)
		{
			std::vector<int> groupIndices;
			for (int i = 0; i < (int)activeTiles.size(); i++)
			{
				int mx = ((activeTiles[i].tx % 3) + 3) % 3;
				int my = ((activeTiles[i].ty % 3) + 3) % 3;
				if (mx == gx && my == gy)
					groupIndices.push_back(i);
			}

			if (groupIndices.empty())
				continue;

			std::atomic<int> nextIdx(0);
			const int groupSize = (int)groupIndices.size();

			auto worker = [&]()
			{
				dtTraverseLinkConnectParams threadParams = params;

				for (;;)
				{
					const int idx = nextIdx.fetch_add(1);
					if (idx >= groupSize)
						break;

					const dtTileRef ref = activeTiles[groupIndices[idx]].ref;

					// Within-tile pass first, then neighbor pass — same order as sequential.
					threadParams.linkToNeighbor = false;
					m_navMesh->connectTraverseLinks(ref, threadParams);
					threadParams.linkToNeighbor = true;
					m_navMesh->connectTraverseLinks(ref, threadParams);
				}
			};

			const int groupWorkers = rdMin(numWorkers, groupSize);
			std::vector<std::thread> workers;
			for (int i = 0; i < groupWorkers; i++)
				workers.emplace_back(worker);
			for (auto& w : workers)
				w.join();
		}
	}

	return true;
}

static bool animTypeSupportsTraverseLink(const dtTraverseTableCreateParams* params, const dtLink* link, const int tableIndex)
{
	const NavMeshType_e navMeshType = (NavMeshType_e)params->navMeshType;

	// Only the _small NavMesh has more than 1 table.
	const int traverseAnimType = navMeshType == NAVMESH_SMALL
		? tableIndex
		: NavMesh_GetFirstTraverseAnimTypeForType(navMeshType);

	return rdBitCellBit(link->traverseType) & s_traverseAnimTraverseFlags[traverseAnimType];
}

bool Editor::createStaticPathingData()
{
	if (!m_navMesh)
		return false;

	dtTraverseTableCreateParams params;
	params.nav = m_navMesh;
	params.sets = m_djs;
	params.tableCount = NavMesh_GetTraverseTableCountForNavMeshType(m_selectedNavMeshType);
	params.navMeshType = m_selectedNavMeshType;
	params.canTraverse = animTypeSupportsTraverseLink;
	params.collapseGroups = m_collapseLinkedPolyGroups;

	// Phase 1: Build disjoint poly groups (sequential — flood fill).
	if (!dtCreateDisjointPolyGroups(&params))
	{
		m_ctx->log(RC_LOG_ERROR, "createStaticPathingData: Failed to build disjoint poly groups.");
		return false;
	}

	// Phase 2: Build traverse table data with per-table parallelism.
	const int tableCount = params.tableCount;

	m_navMesh->freeTraverseTables();
	if (!m_navMesh->allocTraverseTables(tableCount))
	{
		m_ctx->log(RC_LOG_ERROR, "createStaticPathingData: Failed to allocate traverse tables.");
		return false;
	}
	m_navMesh->setTraverseTableCount(tableCount);

	// Copy base disjoint set and union traverse-linked poly groups per table.
	// Each table has its own disjoint set — independent work.
	{
		dtDisjointSet& baseSet = params.sets[0];

		for (int i = 0; i < tableCount; i++)
		{
			dtDisjointSet& targetSet = params.sets[i];
			if (i > 0)
				baseSet.copy(targetSet);
		}

		// Union traverse-linked poly groups in parallel (each table uses its own set).
		std::atomic<int> nextTable(0);
		const int numWorkers = rdMin(tableCount, rdMax(1, (int)std::thread::hardware_concurrency() - 1));

		auto unionWorker = [&]()
		{
			for (;;)
			{
				const int t = nextTable.fetch_add(1);
				if (t >= tableCount)
					break;

				dtDisjointSet& set = params.sets[t];
				if (!set.getSetCount())
					continue;

				const int maxTiles = m_navMesh->getMaxTiles();
				for (int i = 0; i < maxTiles; ++i)
				{
					dtMeshTile* tile = m_navMesh->getTile(i);
					if (!tile->header)
						continue;

					const int pcount = tile->header->polyCount;
					for (int j = 0; j < pcount; j++)
					{
						dtPoly& poly = tile->polys[j];
						for (unsigned int k = poly.firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
						{
							const dtLink* link = &tile->links[k];
							const dtMeshTile* landTile;
							const dtPoly* landPoly;
							m_navMesh->getTileAndPolyByRefUnsafe(link->ref, &landTile, &landPoly);

							if (poly.groupId != landPoly->groupId && params.canTraverse(&params, link, t))
								set.setUnion(poly.groupId, landPoly->groupId);
						}
					}
				}
			}
		};

		std::vector<std::thread> workers;
		for (int i = 0; i < numWorkers; i++)
			workers.emplace_back(unionWorker);
		for (auto& w : workers)
			w.join();
	}

	const int polyGroupCount = params.sets[0].getSetCount();
	rdAssert(polyGroupCount <= DT_MAX_POLY_GROUP_COUNT);

	const int tableSize = dtCalcTraverseTableSize(polyGroupCount);
	m_navMesh->setTraverseTableSize(tableSize);

	// Fill traverse tables in parallel (each table is independent).
	{
		// Pre-allocate all tables sequentially (rdAlloc may not be thread-safe).
		std::vector<int*> tables(tableCount);
		for (int i = 0; i < tableCount; i++)
		{
			const rdSizeType bufferSize = sizeof(int) * tableSize;
			tables[i] = (int*)rdAlloc(bufferSize, RD_ALLOC_PERM);
			if (!tables[i])
			{
				m_ctx->log(RC_LOG_ERROR, "createStaticPathingData: Failed to allocate traverse table %d.", i);
				return false;
			}
			memset(tables[i], 0, bufferSize);
			m_navMesh->setTraverseTable(i, tables[i]);
		}

		std::atomic<int> nextTable(0);
		const int numWorkers = rdMin(tableCount, rdMax(1, (int)std::thread::hardware_concurrency() - 1));

		auto fillWorker = [&]()
		{
			for (;;)
			{
				const int t = nextTable.fetch_add(1);
				if (t >= tableCount)
					break;

				int* const traverseTable = tables[t];
				const dtDisjointSet& set = params.sets[t];

				for (unsigned short j = 0; j < polyGroupCount; j++)
				{
					for (unsigned short k = 0; k < polyGroupCount; k++)
					{
						const bool isReachable = j == k || set.find(j) == set.find(k);
						if (isReachable)
						{
							const int index = dtCalcTraverseTableCellIndex(polyGroupCount, j, k);
							traverseTable[index] |= 1 << (k & 31);
						}
					}
				}
			}
		};

		std::vector<std::thread> workers;
		for (int i = 0; i < numWorkers; i++)
			workers.emplace_back(fillWorker);
		for (auto& w : workers)
			w.join();
	}

	m_navMesh->setPolyGroupCount(params.sets[0].getSetCount());
	return true;
}

void Editor::connectOffMeshLinks()
{
	for (int i = 0; i < m_navMesh->getMaxTiles(); i++)
	{
		dtMeshTile* target = m_navMesh->getTile(i);
		const dtMeshHeader* header = target->header;

		if (!header)
			continue;

		const int offMeshConCount = header->offMeshConCount;

		if (!offMeshConCount)
			continue;

		const dtTileRef targetRef = m_navMesh->getTileRef(target);

		// Connect off-mesh polygons to inner and outer tiles.
		m_navMesh->connectOffMeshLinks(targetRef);
	}
}

unsigned int Editor::getTraverseFlags(const TraverseAnimType_e type)
{
	return s_traverseAnimTraverseFlags[type];
}

void Editor::updateToolStates(const float dt)
{
	for (int i = 0; i < MAX_TOOLS; i++)
	{
		if (m_toolStates[i])
			m_toolStates[i]->handleUpdate(dt);
	}
}

void Editor::initToolStates(Editor* editor)
{
	for (int i = 0; i < MAX_TOOLS; i++)
	{
		if (m_toolStates[i])
			m_toolStates[i]->init(editor);
	}
}

void Editor::resetToolStates()
{
	for (int i = 0; i < MAX_TOOLS; i++)
	{
		if (m_toolStates[i])
			m_toolStates[i]->reset();
	}
}

void Editor::renderToolStates()
{
	for (int i = 0; i < MAX_TOOLS; i++)
	{
		if (m_toolStates[i])
			m_toolStates[i]->handleRender();
	}
}

void Editor::renderOverlayToolStates(double* model, double* proj, int* view)
{
	for (int i = 0; i < MAX_TOOLS; i++)
	{
		if (m_toolStates[i])
			m_toolStates[i]->handleRenderOverlay(model, proj, view);
	}
}

void Editor::renderMeshOffsetOptions()
{
	ImGui::Text("Render Offsets");

	ImGui::PushItemWidth(230);

	ImGui::SliderFloat3("Recast##RenderOffset", (float*)&m_recastDrawOffset, -500, 500);
	ImGui::SliderFloat3("Detour##RenderOffset", (float*)&m_detourDrawOffset, -500, 500);

	ImGui::PopItemWidth();
}

void Editor::renderDetourDebugMenu()
{
	ImGui::Text("Detour Render Options");

	bool isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_OFFMESHCONS);

	if (ImGui::Checkbox("Off-Mesh Connections", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_OFFMESHCONS);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_QUERY_NODES);

	if (ImGui::Checkbox("Query Nodes", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_QUERY_NODES);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_BVTREE);

	if (ImGui::Checkbox("BVTree", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_BVTREE);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_PORTALS);

	if (ImGui::Checkbox("Portals", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_PORTALS);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_WITH_CLOSED_LIST);

	if (ImGui::Checkbox("Closed List", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_WITH_CLOSED_LIST);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_TILE_COLORS);

	if (ImGui::Checkbox("Tile ID Colors", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_TILE_COLORS);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_TILE_BOUNDS);

	if (ImGui::Checkbox("Tile Bounds", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_TILE_BOUNDS);

#if DT_NAVMESH_SET_VERSION >= 8
	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_TILE_CELLS);

	if (ImGui::Checkbox("Tile Cells", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_TILE_CELLS);
#endif

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_POLY_FACES);

	if (ImGui::Checkbox("Poly Faces", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_POLY_FACES);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_POLY_EDGES);

	if (ImGui::Checkbox("Poly Edges", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_POLY_EDGES);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_POLY_VERTS);

	if (ImGui::Checkbox("Poly Verts", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_POLY_VERTS);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_POLY_BOUNDS_INNER);

	if (ImGui::Checkbox("Inner Poly Boundaries", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_POLY_BOUNDS_INNER);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_POLY_BOUNDS_OUTER);

	if (ImGui::Checkbox("Outer Poly Boundaries", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_POLY_BOUNDS_OUTER);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_POLY_CENTERS);

	if (ImGui::Checkbox("Poly Centers", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_POLY_CENTERS);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_POLY_GROUPS);

	if (ImGui::Checkbox("Poly Group Colors", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_POLY_GROUPS);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_LEDGE_SPANS);

	if (ImGui::Checkbox("Ledge Spans", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_LEDGE_SPANS);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_DEPTH_MASK);

	if (ImGui::Checkbox("Depth Mask", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_DEPTH_MASK);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_ALPHA);

	if (ImGui::Checkbox("Transparency", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_ALPHA);

	isEnabled = (getNavMeshDrawFlags() & DU_DRAW_DETOURMESH_TRAVERSE_LINKS);

	if (ImGui::Checkbox("Traverse Links", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_TRAVERSE_LINKS);

	if (isEnabled && m_navMesh) // Supplemental options only available with a valid navmesh!
	{
		ImGui::PushItemWidth(190);
		ImGui::SliderInt("Traverse Type", &m_traverseLinkDrawParams.traverseLinkType, -1, DT_MAX_TRAVERSE_TYPES-1);
		ImGui::SliderInt("Traverse Dist", &m_traverseLinkDrawParams.traverseLinkDistance, -1, dtQuantLinkDistance(DT_TRAVERSE_DIST_MAX));
		ImGui::SliderInt("Traverse Anim", &m_traverseLinkDrawParams.traverseAnimType, -2, m_navMesh->getParams()->traverseTableCount-1);
		ImGui::PopItemWidth();
	}
}

void Editor::renderTraverseTableFineTuners()
{
	static ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit |
		/*ImGuiTableFlags_ScrollX |*/
		ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_BordersInner |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_Hideable |
		/*ImGuiTableFlags_Resizable |*/
		/*ImGuiTableFlags_Reorderable |*/
		ImGuiTableFlags_HighlightHoveredColumn;

	static ImGuiTableColumnFlags columnFlags = ImGuiTableColumnFlags_AngledHeader |
		ImGuiTableColumnFlags_WidthStretch;

	static int frozenCols = 1;
	static int frozenRows = 2;
	const int rowsCount = NUM_TRAVERSE_TYPES;
	const float textBaseHeight = ImGui::GetTextLineHeightWithSpacing();

	const char* linearColumnNames[] = { "Type", "minDist", "maxDist", "minElev", "maxElev" };
	const int linearColumnsCount = IM_ARRAYSIZE(linearColumnNames);

	if (ImGui::BeginTable("TraverseTableLinearFineTuner", linearColumnsCount, tableFlags, ImVec2(0.0f, (textBaseHeight * 12) + 10.f)))
	{
		ImGui::TableSetupColumn(linearColumnNames[0], ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder);
		for (int n = 1; n < linearColumnsCount; n++)
			ImGui::TableSetupColumn(linearColumnNames[n], columnFlags, 100);
		ImGui::TableSetupScrollFreeze(frozenCols, frozenRows);

		ImGui::TableAngledHeadersRow();
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(rowsCount);

		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				ImGui::PushID(row);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::Text("%d", row);

				for (int column = 1; column < linearColumnsCount; column++)
				{
					if (!ImGui::TableSetColumnIndex(column))
						continue;

					ImGui::PushID(column);
					ImGui::PushItemWidth(-FLT_MIN); // Right align cells.
					TraverseType_s& trav = s_traverseTable[row];

					switch (column)
					{
					case 1:
						ImGui::SliderFloat("", &trav.minDist, 0, trav.maxDist, "%g");
						break;
					case 2:
						ImGui::SliderFloat("", &trav.maxDist, 0, DT_TRAVERSE_DIST_MAX, "%g");
						break;
					case 3:
						ImGui::SliderFloat("", &trav.minElev, 0, trav.maxElev, "%g");
						break;
					case 4:
						ImGui::SliderFloat("", &trav.maxElev, 0, DT_TRAVERSE_DIST_MAX, "%g");
						break;
					}

					ImGui::PopItemWidth();
					ImGui::PopID();
				}
				ImGui::PopID();
			}
		}

		ImGui::EndTable();
	}

	const char* angularColumnNames[] = { "Type", "minSlope", "maxSlope", "ovlpTrig", "ovlpExcl" };
	const int angularColumnsCount = IM_ARRAYSIZE(angularColumnNames);

	if (ImGui::BeginTable("TraverseTableAngularFineTuner", angularColumnsCount, tableFlags, ImVec2(0.0f, (textBaseHeight * 12) + 10.f)))
	{
		ImGui::TableSetupColumn(angularColumnNames[0], ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder);
		for (int n = 1; n < angularColumnsCount; n++)
			ImGui::TableSetupColumn(angularColumnNames[n], columnFlags, 100);
		ImGui::TableSetupScrollFreeze(frozenCols, frozenRows);

		ImGui::TableAngledHeadersRow();
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(rowsCount);

		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				ImGui::PushID(row);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::Text("%d", row);

				for (int column = 1; column < angularColumnsCount; column++)
				{
					if (!ImGui::TableSetColumnIndex(column))
						continue;

					ImGui::PushID(column);
					ImGui::PushItemWidth(-FLT_MIN); // Right align cells.
					TraverseType_s& trav = s_traverseTable[row];

					switch (column)
					{
					case 1:
						ImGui::SliderFloat("", &trav.minSlope, 0, trav.maxSlope, "%g");
						break;
					case 2:
						ImGui::SliderFloat("", &trav.maxSlope, 0, 360, "%g");
						break;
					case 3:
						ImGui::SliderFloat("", &trav.ovlpTrig, 0, trav.maxElev, "%g");
						break;
					case 4:
						ImGui::Checkbox("", &trav.ovlpExcl);
						break;
					}

					ImGui::PopItemWidth();
					ImGui::PopID();
				}
				ImGui::PopID();
			}
		}

		ImGui::EndTable();
	}

	if (ImGui::Button("Reset Traverse Table Parameters"))
		initTraverseTableParams();

	const int numTraverseTables = NavMesh_GetTraverseTableCountForNavMeshType(m_selectedNavMeshType);
	const int numColumns = numTraverseTables + 1;

	if (ImGui::BeginTable("TraverseTableMaskSelector", numColumns, tableFlags, ImVec2(0.0f, (textBaseHeight * 12) + 20.f)))
	{
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder);
		const bool smallNavMesh = m_selectedNavMeshType == NAVMESH_SMALL;

		for (int n = 0; n < numTraverseTables; n++)
		{
			const int i = smallNavMesh
				? NavMesh_GetTraverseTableIndexForAnimType(TraverseAnimType_e(n))
				: NavMesh_GetFirstTraverseAnimTypeForType(m_selectedNavMeshType);

			ImGui::TableSetupColumn(g_traverseAnimTypeNames[i], columnFlags);
		}

		ImGui::TableSetupScrollFreeze(frozenCols, frozenRows);

		ImGui::TableAngledHeadersRow();
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(rowsCount);

		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				ImGui::PushID(row);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::Text("%d", row);

				for (int column = 0; column < numTraverseTables; column++)
				{
					if (!ImGui::TableSetColumnIndex(column + 1))
						continue;

					ImGui::PushID(column + 1);
					const int j = smallNavMesh
						? column
						: NavMesh_GetFirstTraverseAnimTypeForType(m_selectedNavMeshType);

					int* const flags = reinterpret_cast<int*>(&s_traverseAnimTraverseFlags[j]);

					ImGui::CheckboxFlags("", flags, 1 << row);
					ImGui::PopID();
				}
				ImGui::PopID();
			}
		}

		ImGui::EndTable();
	}
	if (ImGui::Button("Reset Traverse Table Masks"))
		initTraverseMasks();
}

// NOTE: the climb height should never equal or exceed the agent's height, see https://groups.google.com/g/recastnavigation/c/L5rBamxcOBk/m/5xGLj6YP25kJ
// Quote: "you will get into trouble in cases where there is an overhand which is low enough to step over and high enough for the agent to walk under."
const NavMeshDefaults_s g_navMeshDefaults[NAVMESH_COUNT] = {
	{ g_navMeshNames[NAVMESH_SMALL]      , NAI_Hull::Radius(HULL_HUMAN)  , NAI_Hull::Height(HULL_HUMAN)  , NAI_Hull::StepHeight(HULL_HUMAN)  , 8.f, 4.f, 16 },
	{ g_navMeshNames[NAVMESH_MED_SHORT]  , NAI_Hull::Radius(HULL_PROWLER), NAI_Hull::Height(HULL_PROWLER), NAI_Hull::StepHeight(HULL_PROWLER), 8.f, 4.f, 8 },
	{ g_navMeshNames[NAVMESH_MEDIUM]     , NAI_Hull::Radius(HULL_MEDIUM) , NAI_Hull::Height(HULL_MEDIUM) , NAI_Hull::StepHeight(HULL_MEDIUM) , 8.f, 4.f, 8 },
	{ g_navMeshNames[NAVMESH_LARGE]      , NAI_Hull::Radius(HULL_TITAN)  , NAI_Hull::Height(HULL_TITAN)  , NAI_Hull::StepHeight(HULL_TITAN)  , 15.f, 7.5f, 4 },
	{ g_navMeshNames[NAVMESH_EXTRA_LARGE], NAI_Hull::Radius(HULL_GOLIATH), NAI_Hull::Height(HULL_GOLIATH), NAI_Hull::StepHeight(HULL_GOLIATH), 15.f, 7.5f, 4 },
};

void Editor::selectNavMeshType(const NavMeshType_e navMeshType)
{
	const NavMeshDefaults_s& h = g_navMeshDefaults[navMeshType];

	m_navmeshName = h.name;

	m_agentRadius = h.radius;
	m_agentHeight = h.height;
	m_agentMaxClimb = h.climbHeight;

	m_cellSize = h.cellSize;
	m_cellHeight = h.cellHeight;

	m_polyCellRes = h.polyCellResolution;
	m_selectedNavMeshType = navMeshType;

	updateTraverseLinkRenderParams();
}

bool Editor::loadAll(std::string path, const bool fullPath)
{
	dtFreeNavMesh(m_navMesh);
	m_navMesh = nullptr;

	const char* navMeshPath = nullptr;
	char buffer[256];

	if (!fullPath) // Load from model name (e.g. "mp_rr_box").
	{
		fs::path p = "..\\platform\\maps\\navmesh\\";
		if (fs::is_directory(p))
		{
			path.insert(0, p.string());
		}

		sprintf(buffer, "%s_%s.nm", path.c_str(), m_navmeshName);
		navMeshPath = buffer;
	}
	else
		navMeshPath = path.c_str();

	FILE* fp = fopen(navMeshPath, "rb");
	if (!fp)
		return false;

	// Read header.
	dtNavMeshSetHeader header;
	size_t readLen = fread(&header, sizeof(dtNavMeshSetHeader), 1, fp);
	if (readLen != 1)
	{
		fclose(fp);
		return false;
	}
	// This path loads dtNavMeshSetHeader only (DT_NAVMESH_SET_*). Tilecache
	// set files use a different header (TILECACHESET_* in Editor_TempObstacles)
	// and a separate loadAll; no active-tool-mode flag is reachable here.
	if (header.magic != DT_NAVMESH_SET_MAGIC)
	{
		if (m_ctx)
			m_ctx->log(RC_LOG_ERROR,
				"loadAll: bad magic (expected 0x%08x, got 0x%08x)",
				DT_NAVMESH_SET_MAGIC, header.magic);
		fclose(fp);
		return false;
	}
	if (header.version != DT_NAVMESH_SET_VERSION)
	{
		if (m_ctx)
			m_ctx->log(RC_LOG_ERROR,
				"loadAll: bad version (expected %d, got %d)",
				DT_NAVMESH_SET_VERSION, header.version);
		fclose(fp);
		return false;
	}

	dtNavMesh* mesh = dtAllocNavMesh();
	if (!mesh)
	{
		fclose(fp);
		return false;
	}


	dtStatus status = mesh->init(&header.params);
	if (dtStatusFailed(status))
	{
		fclose(fp);
		return false;
	}
	
	// Read tiles.
	for (int i = 0; i < header.numTiles; ++i)
	{
		dtNavMeshTileHeader tileHeader;
		readLen = fread(&tileHeader, sizeof(tileHeader), 1, fp);
		if (readLen != 1)
		{
			fclose(fp);
			return false;
		}

		if (!tileHeader.tileRef || !tileHeader.dataSize)
			break;

		unsigned char* data = (unsigned char*)rdAlloc(tileHeader.dataSize, RD_ALLOC_PERM);
		if (!data)
			break;

		memset(data, 0, tileHeader.dataSize);
		readLen = fread(data, tileHeader.dataSize, 1, fp);

		if (readLen != 1)
		{
			rdFree(data);
			fclose(fp);
			return false;
		}

		mesh->addTile(data, tileHeader.dataSize, DT_TILE_FREE_DATA, tileHeader.tileRef, NULL);
	}

	// Read static pathing data.
	if (header.params.polyGroupCount >= DT_MIN_POLY_GROUP_COUNT)
	{
		for (int i = 0; i < header.params.traverseTableCount; i++)
		{
			int* traverseTable = (int*)rdAlloc(header.params.traverseTableSize, RD_ALLOC_PERM);
			if (!traverseTable)
				break;

			memset(traverseTable, 0, header.params.traverseTableSize);
			readLen = fread(traverseTable, header.params.traverseTableSize, 1, fp);

			if (readLen != 1)
			{
				rdFree(traverseTable);
				fclose(fp);
				return 0;
			}

			mesh->setTraverseTable(i, traverseTable);
		}
	}

	fclose(fp);
	m_navMesh = mesh;

	return true;
}

void Editor::saveAll(std::string path, const dtNavMesh* mesh)
{
	if (!mesh)
		return;

	fs::path p = "..\\platform\\maps\\navmesh\\";
	if (fs::is_directory(p))
	{
		path.insert(0, p.string());
	}

	char buffer[256];
	sprintf(buffer, "%s_%s.nm", path.c_str(), m_navmeshName);

	FILE* fp = fopen(buffer, "wb");
	if (!fp)
		return;

	// Store header.
	dtNavMeshSetHeader header;
	header.magic = DT_NAVMESH_SET_MAGIC;
	header.version = DT_NAVMESH_SET_VERSION;
	header.numTiles = 0;

	for (int i = 0; i < mesh->getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = mesh->getTile(i);
		if (!tile || !tile->header || !tile->dataSize)
			continue;

		header.numTiles++;
	}

	const dtNavMeshParams* params = mesh->getParams();

	memcpy(&header.params, params, sizeof(dtNavMeshParams));
	fwrite(&header, sizeof(dtNavMeshSetHeader), 1, fp);

	// Store tiles.
	for (int i = 0; i < mesh->getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = mesh->getTile(i);
		if (!tile || !tile->header || !tile->dataSize)
			continue;

		dtNavMeshTileHeader tileHeader;
		tileHeader.tileRef = mesh->getTileRef(tile);
		tileHeader.dataSize = tile->dataSize;

		fwrite(&tileHeader, sizeof(tileHeader), 1, fp);
		fwrite(tile->data, tile->dataSize, 1, fp);
	}

#if DT_NAVMESH_SET_VERSION == 5
	int mset5Unkown = 0;
	for (int i = 0; i < params->polyGroupCount; i++)
		fwrite(&mset5Unkown, sizeof(int), 1, fp);
#endif

	// Only store if we have 3 or more poly groups.
	if (params->polyGroupCount >= DT_MIN_POLY_GROUP_COUNT)
	{
		int** traverseTables = mesh->getTraverseTables();

		rdAssert(traverseTables);

		for (int i = 0; i < header.params.traverseTableCount; i++)
		{
			const int* const tableData = traverseTables[i];
			rdAssert(tableData);

			fwrite(tableData, sizeof(int), (header.params.traverseTableSize/4), fp);
		}
	}

	fclose(fp);
}

bool Editor::loadNavMesh(const char* path, const bool fullPath)
{
	const bool result = Editor::loadAll(path, fullPath);
	m_navQuery->init(m_navMesh, 2048);

	m_loadedNavMeshType = m_selectedNavMeshType;
	m_traverseLinkDrawParams.traverseAnimType = -2;

	if (m_tool)
	{
		m_tool->reset();
		m_tool->init(this);
	}

	resetToolStates();
	initToolStates(this);
	invalidateNavMeshCache();

	return result;
}
