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

#include "Shared/Include/SharedAssert.h"
#include "Include/Editor.h"
#include "Include/Editor_Common.h"
#include "DebugUtils/Include/RecastDebugDraw.h"
#include "DebugUtils/Include/DetourDebugDraw.h"
#include "Include/InputGeom.h"
#include "DetourTileCache/Include/DetourTileCache.h"
#include "include/ShapeVolumeTool.h"

static void EditorCommon_DrawBoundingBox(duDebugDraw* const dd, const InputGeom* const geom)
{
	const rdVec3D* const origBmin = geom->getMeshBoundsMin();
	const rdVec3D* const origBmax = geom->getMeshBoundsMax();

	// Draw Mesh bounds
	duDebugDrawBoxWire(dd, origBmin->x, origBmin->y, origBmin->z, origBmax->x, origBmax->y, origBmax->z, duRGBA(255, 255, 255, 170), 1.0f, nullptr);

	const rdVec3D* const origNavBmin = geom->getOriginalNavMeshBoundsMin();
	const rdVec3D* const origNavBmax = geom->getOriginalNavMeshBoundsMax();

	const rdVec3D* const navBmin = geom->getNavMeshBoundsMin();
	const rdVec3D* const navBmax = geom->getNavMeshBoundsMax();

	// Draw Original NavMesh bounds (e.g. when loading from a .gset with a predetermined NavMesh bound that differs from the mesh bound)
	if ((!rdVequal(origBmin, origNavBmin) || !rdVequal(origBmax, origNavBmax)) && (!rdVequal(navBmin, origNavBmin) || !rdVequal(navBmax, origNavBmax)))
		duDebugDrawBoxWire(dd, origNavBmin->x, origNavBmin->y, origNavBmin->z, origNavBmax->x, origNavBmax->y, origNavBmax->z, duRGBA(0, 80, 255, 215), 1.0f, nullptr);

	// Draw NavMesh bounds
	if (!rdVequal(origBmin, navBmin) || !rdVequal(origBmax, navBmax))
		duDebugDrawBoxWire(dd, navBmin->x, navBmin->y, navBmin->z, navBmax->x, navBmax->y, navBmax->z, duRGBA(0, 255, 0, 215), 1.0f, nullptr);
}

static void EditorCommon_DrawTilingGrid(duDebugDraw* const dd, const InputGeom* const geom, const int tileSize, const float cellSize)
{
	const rdVec3D* const bmin = geom->getNavMeshBoundsMin();
	const rdVec3D* const bmax = geom->getNavMeshBoundsMax();

	int gw = 0, gh = 0;
	rcCalcGridSize(bmin, bmax, cellSize, &gw, &gh);

	const int tw = (gw + tileSize - 1) / tileSize;
	const int th = (gh + tileSize - 1) / tileSize;
	const float s = tileSize * cellSize;

	duDebugDrawGridXY(dd, bmax->x, bmin->y, bmin->z, tw, th, s, duRGBA(0, 0, 0, 64), 1.0f, nullptr);
}

int EditorCommon_CalcTileProperties(const InputGeom* const geom,
	const int minTilebits, const int maxTileBits, const int tileSize,
	const float cellSize, int& maxTiles, int& maxPolysPerTile,
	int& tileCountX, int& tileCountY)
{
	int gridSize = 1;

	if (geom)
	{
		int gw = 0, gh = 0;
		const rdVec3D* bmin = geom->getNavMeshBoundsMin();
		const rdVec3D* bmax = geom->getNavMeshBoundsMax();
		rcCalcGridSize(bmin, bmax, cellSize, &gw, &gh);
		const int ts = tileSize;
		const int tw = (gw + ts-1) / ts;
		const int th = (gh + ts-1) / ts;

		// Max tiles and max polys affect how the tile IDs are calculated.
		// There are MAX_TILE_BITS bits available for identifying a tile and a polygon.
		const int tileBits = rdMin((int)rdIlog2(rdNextPow2(tw*th)), minTilebits);
		const int polyBits = maxTileBits - tileBits;

		maxTiles = 1 << tileBits;
		maxPolysPerTile = 1 << polyBits;

		gridSize = tw*th;
		tileCountX = tw;
		tileCountY = th;
	}
	else
	{
		maxTiles = 0;
		maxPolysPerTile = 0;
		gridSize = 1;
		tileCountX = 0;
		tileCountY = 0;
	}

	return gridSize;
}

int EditorCommon_SetAndRenderTileProperties(const InputGeom* const geom, 
	const int minTilebits, const int maxTileBits, const int tileSize,
	const float cellSize, int& maxTiles, int& maxPolysPerTile)
{
	int tileCountX = 0, tileCountY = 0;
	const int gridSize = EditorCommon_CalcTileProperties(geom, minTilebits, maxTileBits,
		tileSize, cellSize, maxTiles, maxPolysPerTile, tileCountX, tileCountY);

	if (geom)
	{
		ImGui::Text("Tiles: %d x %d", tileCountX, tileCountY);
		ImGui::Text("Tile Sizes: %g x %g (%g)", tileCountX*cellSize, tileCountY*cellSize, tileSize*cellSize);
		ImGui::Text("Max Tiles: %d", maxTiles);
		ImGui::Text("Max Polys: %d", maxPolysPerTile);
	}

	return gridSize;
}

Editor_StaticTileMeshCommon::Editor_StaticTileMeshCommon()
	: m_triareas(nullptr)
	, m_solid(nullptr)
	, m_chf(nullptr)
	, m_cset(nullptr)
	, m_pmesh(nullptr)
	, m_dmesh(nullptr)
	, m_tileMeshDrawFlags(DU_DRAW_RECASTMESH_INPUT_MESH|DU_DRAW_RECASTMESH_NAVMESH|DU_DRAW_RECASTMESH_SHAPE_VOLUMES)
	, m_tileCol(duRGBA(0, 0, 0, 64))
	, m_totalBuildTimeMs(0.0f)
	, m_drawActiveTile(false)
	, m_keepInterResults(false)
{
	m_lastBuiltTileBmin.init(0.0f,0.0f,0.0f);
	m_lastBuiltTileBmax.init(0.0f,0.0f,0.0f);

	memset(&m_cfg, 0, sizeof(rcConfig));
}

void Editor_StaticTileMeshCommon::cleanup()
{
	delete[] m_triareas;
	m_triareas = 0;
	rcFreeHeightField(m_solid);
	m_solid = 0;
	rcFreeCompactHeightfield(m_chf);
	m_chf = 0;
	rcFreeContourSet(m_cset);
	m_cset = 0;
	rcFreePolyMesh(m_pmesh);
	m_pmesh = 0;
	rcFreePolyMeshDetail(m_dmesh);
	m_dmesh = 0;
}

void Editor_StaticTileMeshCommon::renderRecastDebugMenu()
{
	ImGui::Text("Recast Render Options");

	bool isEnabled = m_tileMeshDrawFlags & DU_DRAW_RECASTMESH_INPUT_MESH;

	// This should always be available, since if we load a large mesh we want to
	// be able to toggle this off to save on performance. The renderer has to be
	// moved to its own thread to solve this issue.
	if (ImGui::Checkbox("Input Mesh", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_INPUT_MESH);

	// Check which modes are valid.
	//const bool hasNavMesh =m_navMesh != 0;
	const bool hasChf = m_chf != 0;
	const bool hasCset = m_cset != 0;
	const bool hasSolid = m_solid != 0;
	const bool hasDMesh = m_dmesh != 0;

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_NAVMESH;
	//ImGui::BeginDisabled(!hasNavMesh);

	if (ImGui::Checkbox("NavMesh", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_NAVMESH);

	//ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_VOXELS;
	ImGui::BeginDisabled(!hasSolid);

	if (ImGui::Checkbox("Voxels", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_VOXELS);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_VOXELS_WALKABLE;
	ImGui::BeginDisabled(!hasSolid);

	if (ImGui::Checkbox("Walkable Voxels", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_VOXELS_WALKABLE);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_COMPACT;
	ImGui::BeginDisabled(!hasChf);

	if (ImGui::Checkbox("Compact", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_COMPACT);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_COMPACT_DISTANCE;
	ImGui::BeginDisabled(!hasChf);

	if (ImGui::Checkbox("Compact Distance", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_COMPACT_DISTANCE);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_COMPACT_REGIONS;
	ImGui::BeginDisabled(!hasChf);

	if (ImGui::Checkbox("Compact Regions", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_COMPACT_REGIONS);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_REGION_CONNECTIONS;
	ImGui::BeginDisabled(!hasCset);

	if (ImGui::Checkbox("Region Connections", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_REGION_CONNECTIONS);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_RAW_CONTOURS;
	ImGui::BeginDisabled(!hasCset);

	if (ImGui::Checkbox("Raw Contours", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_RAW_CONTOURS);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_CONTOURS;
	ImGui::BeginDisabled(!hasCset);

	if (ImGui::Checkbox("Contours", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_CONTOURS);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_POLYMESH;
	ImGui::BeginDisabled(!hasDMesh);

	if (ImGui::Checkbox("Poly Mesh", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_POLYMESH);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_POLYMESH_DETAIL;
	ImGui::BeginDisabled(!hasDMesh);

	if (ImGui::Checkbox("Poly Mesh Detail", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_POLYMESH_DETAIL);

	ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_SHAPE_VOLUMES;

	if (ImGui::Checkbox("Shape Volumes", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_SHAPE_VOLUMES);

	//if (intermediateDataUnavailable) // todo(amos): tool tip
	//{
	//	ImGui::Separator();

	//	ImGui::Text("Tick 'Keep Intermediate Results'");
	//	ImGui::Text("rebuild some tiles to see");
	//	ImGui::Text("more debug mode options.");
	//}
}

void Editor_StaticTileMeshCommon::renderTileMeshData()
{
	if (!m_geom || !m_geom->getMesh())
		return;

	const float texScale = 1.0f / (m_cellSize * 10.0f);

	// Draw input mesh (cached — only recomputed when mesh changes)
	if (getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_INPUT_MESH)
		drawInputMeshCached(m_agentMaxSlope, texScale);

	glDepthMask(GL_FALSE);

	// Draw bounds
	EditorCommon_DrawBoundingBox(&m_dd, m_geom);

	// Tiling grid.
	EditorCommon_DrawTilingGrid(&m_dd, m_geom, m_tileSize, m_cellSize);

	const rdVec3D* recastDrawOffset = getRecastDrawOffset();
	const rdVec3D* detourDrawOffset = getDetourDrawOffset();

	const unsigned int recastDrawFlags = getTileMeshDrawFlags();
	const unsigned int detourDrawFlags = getNavMeshDrawFlags();

	if (m_drawActiveTile)
	{
		// Draw active tile
		// NOTE: only perform offset in x-y
		duDebugDrawBoxWire(&m_dd, m_lastBuiltTileBmin.x+detourDrawOffset->x, m_lastBuiltTileBmin.y+detourDrawOffset->y, m_lastBuiltTileBmin.z,
			m_lastBuiltTileBmax.x+detourDrawOffset->x, m_lastBuiltTileBmax.y+detourDrawOffset->y, m_lastBuiltTileBmax.z, m_tileCol, 1.0f, nullptr);
	}

	if (m_navMesh && m_navQuery)
	{
		if (m_tileMeshDrawFlags & DU_DRAW_RECASTMESH_NAVMESH)
			drawNavMeshCached(detourDrawFlags);
	}

	glDepthMask(GL_TRUE);

	if (m_chf)
	{
		if (recastDrawFlags & DU_DRAW_RECASTMESH_COMPACT)
			duDebugDrawCompactHeightfieldSolid(&m_dd, *m_chf, recastDrawOffset);

		if (recastDrawFlags & DU_DRAW_RECASTMESH_COMPACT_DISTANCE)
			duDebugDrawCompactHeightfieldDistance(&m_dd, *m_chf, recastDrawOffset);
		if (recastDrawFlags & DU_DRAW_RECASTMESH_COMPACT_REGIONS)
			duDebugDrawCompactHeightfieldRegions(&m_dd, *m_chf, recastDrawOffset);
	}

	if (m_solid)
	{
		if (recastDrawFlags & DU_DRAW_RECASTMESH_VOXELS)
		{
			glEnable(GL_FOG);
			duDebugDrawHeightfieldSolid(&m_dd, *m_solid, recastDrawOffset);
			glDisable(GL_FOG);
		}
		if (recastDrawFlags & DU_DRAW_RECASTMESH_VOXELS_WALKABLE)
		{
			glEnable(GL_FOG);
			duDebugDrawHeightfieldWalkable(&m_dd, *m_solid, recastDrawOffset);
			glDisable(GL_FOG);
		}
	}

	if (m_cset)
	{
		if (recastDrawFlags & DU_DRAW_RECASTMESH_RAW_CONTOURS)
		{
			glDepthMask(GL_FALSE);
			duDebugDrawRawContours(&m_dd, *m_cset, recastDrawOffset);
			glDepthMask(GL_TRUE);
		}
		if (recastDrawFlags & DU_DRAW_RECASTMESH_CONTOURS)
		{
			glDepthMask(GL_FALSE);
			duDebugDrawContours(&m_dd, *m_cset, recastDrawOffset);
			glDepthMask(GL_TRUE);
		}
	}

	if ((m_chf &&m_cset) && 
		(recastDrawFlags & DU_DRAW_RECASTMESH_REGION_CONNECTIONS))
	{
		duDebugDrawCompactHeightfieldRegions(&m_dd, *m_chf, recastDrawOffset);

		glDepthMask(GL_FALSE);
		duDebugDrawRegionConnections(&m_dd, *m_cset, recastDrawOffset);
		glDepthMask(GL_TRUE);
	}

	if (m_pmesh && (recastDrawFlags & DU_DRAW_RECASTMESH_POLYMESH))
	{
		glDepthMask(GL_FALSE);
		duDebugDrawPolyMesh(&m_dd, *m_pmesh, recastDrawOffset);
		glDepthMask(GL_TRUE);
	}

	if (m_dmesh && (recastDrawFlags & DU_DRAW_RECASTMESH_POLYMESH_DETAIL))
	{
		glDepthMask(GL_FALSE);
		duDebugDrawPolyMeshDetail(&m_dd, *m_dmesh, recastDrawOffset);
		glDepthMask(GL_TRUE);
	}

	int selectedVolumeIndex = -1;
	const bool isShapeVolumeTool = (m_tool->type() == TOOL_SHAPE_VOLUME);

	if (isShapeVolumeTool)
	{
		const ShapeVolumeTool* volTool = (const ShapeVolumeTool*)m_tool;
		selectedVolumeIndex = volTool->getSelectedVolumeIndex();
	}

	if ((recastDrawFlags & DU_DRAW_RECASTMESH_SHAPE_VOLUMES) || isShapeVolumeTool)
	{
		m_geom->drawBoxVolumes(&m_dd, recastDrawOffset, selectedVolumeIndex);
		m_geom->drawCylinderVolumes(&m_dd, recastDrawOffset, selectedVolumeIndex);
		m_geom->drawConvexVolumes(&m_dd, recastDrawOffset, selectedVolumeIndex);
	}

	// NOTE: commented out because this already gets rendered when the off-mesh
	// connection tool is activated. And if we generated an off-mesh link, this
	// would overlap with that as well.
	//m_geom->drawOffMeshConnections(&m_dd, recastDrawOffset);

	if (m_tool)
		m_tool->handleRender();

	renderToolStates();

	glDepthMask(GL_TRUE);
}

void Editor_StaticTileMeshCommon::renderIntermediateTileMeshOptions()
{
	ImGui::Indent();
	ImGui::Indent();

	if (ImGui::Button("Load", ImVec2(123, 0)))
	{
		Editor::loadNavMesh(m_modelName.c_str());
	}

	if (ImGui::Button("Save", ImVec2(123, 0)))
	{
		Editor::saveAll(m_modelName, m_navMesh);
	}

	ImGui::Unindent();
	ImGui::Unindent();

	ImGui::Text("Build Time: %.1fms", m_totalBuildTimeMs);

	if (m_navMesh)
	{
		const dtNavMeshParams& params = *m_navMesh->getParams();
		//const float* origin = m_navMesh->m_orig;

		//ImGui::Text("Mesh Origin: \n\tX: %g \n\tY: %g \n\tZ: %g", origin[0], origin[1], origin[2]);
		ImGui::Text("Tile Dimensions: %g x %g", params.tileWidth, params.tileHeight);
		ImGui::Text("Poly Group Count: %d", params.polyGroupCount);
		ImGui::Text("Traversal Table Size: %d", params.traverseTableSize);
		ImGui::Text("Traversal Table Count: %d", params.traverseTableCount);
		ImGui::Text("Max Tiles: %d", params.maxTiles);
		ImGui::Text("Max Polys: %d", params.maxPolys);

		ImGui::Separator();
	}
	else
		ImGui::Separator();
}

static void drawTiles(duDebugDraw* dd, dtTileCache* tc, const rdVec3D* offset)
{
	unsigned int fcol[6];
	rdVec3D bmin, bmax;

	for (int i = 0; i < tc->getTileCount(); ++i)
	{
		const dtCompressedTile* tile = tc->getTile(i);
		if (!tile->header) continue;

		tc->calcTightTileBounds(tile->header, &bmin, &bmax);

		const unsigned int col = duIntToCol(i, 64);
		duCalcBoxColors(fcol, col, col);
		duDebugDrawBox(dd, bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z, fcol, offset);
	}

	for (int i = 0; i < tc->getTileCount(); ++i)
	{
		const dtCompressedTile* tile = tc->getTile(i);
		if (!tile->header) continue;

		tc->calcTightTileBounds(tile->header, &bmin, &bmax);

		const unsigned int col = duIntToCol(i, 255);
		const float pad = tc->getParams()->cs * 0.1f;
		duDebugDrawBoxWire(dd, bmin.x - pad, bmin.y - pad, bmin.z - pad,
			bmax.x + pad, bmax.y + pad, bmax.z + pad, col, 2.0f, offset);
	}
}

static void drawObstacles(duDebugDraw* dd, const dtTileCache* tc, const rdVec3D* offset)
{
	// Draw obstacles
	for (int i = 0; i < tc->getObstacleCount(); ++i)
	{
		const dtTileCacheObstacle* ob = tc->getObstacle(i);
		if (ob->state == DT_OBSTACLE_EMPTY) continue;
		rdVec3D bmin, bmax;
		tc->getObstacleBounds(ob, &bmin, &bmax);

		unsigned int col = 0;
		if (ob->state == DT_OBSTACLE_PROCESSING)
			col = duRGBA(255, 255, 0, 128);
		else if (ob->state == DT_OBSTACLE_PROCESSED)
			col = duRGBA(255, 192, 0, 192);
		else if (ob->state == DT_OBSTACLE_REMOVING)
			col = duRGBA(220, 0, 0, 128);

		duDebugDrawCylinder(dd, bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z, col, offset);
		duDebugDrawCylinderWire(dd, bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z, duDarkenCol(col), 2.0f, offset);
	}
}

Editor_DynamicTileMeshCommon::Editor_DynamicTileMeshCommon()
	: m_tileCache(nullptr)
	, m_talloc(nullptr)
	, m_tcomp(nullptr)
	, m_tmproc(nullptr)
	, m_cacheBuildTimeMs(0.0f)
	, m_cacheCompressedSize(0)
	, m_cacheRawSize(0)
	, m_cacheLayerCount(0)
	, m_cacheBuildMemUsage(0)
	, m_tileMeshDrawFlags(DU_DRAW_RECASTMESH_INPUT_MESH|DU_DRAW_RECASTMESH_NAVMESH|DU_DRAW_RECASTMESH_SHAPE_VOLUMES)
	, m_keepInterResults(false)
{
}

void Editor_DynamicTileMeshCommon::renderRecastRenderOptions()
{
	ImGui::Text("Recast Render Options");

	bool isEnabled = m_tileMeshDrawFlags & DU_DRAW_RECASTMESH_INPUT_MESH;

	// This should always be available, since if we load a large mesh we want to
	// be able to toggle this off to save on performance. The renderer has to be
	// moved to its own thread to solve this issue.
	if (ImGui::Checkbox("Input Mesh", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_INPUT_MESH);

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_RECASTMESH_NAVMESH;
	//ImGui::BeginDisabled(!hasNavMesh);

	if (ImGui::Checkbox("NavMesh", &isEnabled))
		toggleTileMeshDrawFlag(DU_DRAW_RECASTMESH_NAVMESH);

	//ImGui::EndDisabled();

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_DETOURMESH_TILE_CACHE_BOUNDS;

	ImGui::BeginDisabled(!m_tileCache);

	if (ImGui::Checkbox("Cache Bounds", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_TILE_CACHE_BOUNDS);

	isEnabled = getTileMeshDrawFlags() & DU_DRAW_DETOURMESH_TILE_CACHE_OBSTACLES;

	if (ImGui::Checkbox("Temp Obstacles", &isEnabled))
		toggleNavMeshDrawFlag(DU_DRAW_DETOURMESH_TILE_CACHE_OBSTACLES);

	ImGui::EndDisabled();
}

void Editor_DynamicTileMeshCommon::renderTileMeshData()
{
	if (!m_geom || !m_geom->getMesh())
		return;

	const float texScale = 1.0f / (m_cellSize * 10.0f);
	const unsigned int recastDrawFlags = getTileMeshDrawFlags();
	const unsigned int detourDrawFlags = getNavMeshDrawFlags();

	const rdVec3D* recastDrawOffset = getRecastDrawOffset();
	const rdVec3D* detourDrawOffset = getDetourDrawOffset();

	// Draw input mesh (cached — only recomputed when mesh changes)
	if (recastDrawFlags & DU_DRAW_RECASTMESH_INPUT_MESH)
		drawInputMeshCached(m_agentMaxSlope, texScale);

	// Draw bounds
	EditorCommon_DrawBoundingBox(&m_dd, m_geom);

	// Tiling grid.
	EditorCommon_DrawTilingGrid(&m_dd, m_geom, m_tileSize, m_cellSize);

	if (m_tileCache && detourDrawFlags & DU_DRAW_DETOURMESH_TILE_CACHE_BOUNDS)
		drawTiles(&m_dd, m_tileCache, detourDrawOffset);

	if (m_tileCache && detourDrawFlags & DU_DRAW_DETOURMESH_TILE_CACHE_OBSTACLES)
		drawObstacles(&m_dd, m_tileCache, detourDrawOffset);

	if (m_navMesh && m_navQuery)
	{
		if (recastDrawFlags & DU_DRAW_RECASTMESH_NAVMESH)
			drawNavMeshCached(detourDrawFlags);
	}

	int selectedVolumeIndex = -1;
	const bool isShapeVolumeTool = (m_tool->type() == TOOL_SHAPE_VOLUME);

	if (isShapeVolumeTool)
	{
		const ShapeVolumeTool* volTool = (const ShapeVolumeTool*)m_tool;
		selectedVolumeIndex = volTool->getSelectedVolumeIndex();
	}
	
	if ((recastDrawFlags & DU_DRAW_RECASTMESH_SHAPE_VOLUMES) || isShapeVolumeTool)
	{
		m_geom->drawBoxVolumes(&m_dd, recastDrawOffset, selectedVolumeIndex);
		m_geom->drawCylinderVolumes(&m_dd, recastDrawOffset, selectedVolumeIndex);
		m_geom->drawConvexVolumes(&m_dd, recastDrawOffset, selectedVolumeIndex);
	}

	// NOTE: commented out because this already gets rendered when the off-mesh
	// connection tool is activated. And if we generated an off-mesh link, this
	// would overlap with that as well.
	//m_geom->drawOffMeshConnections(&m_dd, recastDrawOffset);

	if (m_tool)
		m_tool->handleRender();

	renderToolStates();

	glDepthMask(GL_TRUE);
}
