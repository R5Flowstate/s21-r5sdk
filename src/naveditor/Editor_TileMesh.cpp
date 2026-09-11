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
#include "Shared/Include/SharedCommon.h"
#include "Recast/Include/Recast.h"
#include "Detour/Include/DetourNavMesh.h"
#include "Detour/Include/DetourNavMeshBuilder.h"
#include "DebugUtils/Include/RecastDebugDraw.h"
#include "DebugUtils/Include/DetourDebugDraw.h"
#include "DebugUtils/Include/DetourDump.h"
#include "NavEditor/Include/NavMeshTesterTool.h"
#include "NavEditor/Include/NavMeshPruneTool.h"
#include "NavEditor/Include/OffMeshConnectionTool.h"
#include "NavEditor/Include/ShapeVolumeTool.h"
#include "NavEditor/Include/CrowdTool.h"
#include "NavEditor/Include/InputGeom.h"
#include "NavEditor/Include/Editor.h"
#include "NavEditor/Include/Editor_TileMesh.h"
#include "NavEditor/Include/CameraUtils.h"
#include "NavEditor/Include/PerfTimer.h"

#include "game/server/ai_navmesh.h"
#include "game/server/ai_hull.h"
#include "coordsize.h"


#ifdef DT_POLYREF64
const static int MAX_POLYREF_CHARS = 22;
#define STR_TO_ID strtoull
#else
const static int MAX_POLYREF_CHARS = 11;
#define STR_TO_ID strtoul
#endif // DT_POLYREF64

class NavMeshTileTool : public EditorTool
{
	Editor_TileMesh* m_editor;
	dtNavMesh* m_navMesh;
	dtNavMeshQuery* m_navQuery;
	rdVec3D m_hitPos;
	rdVec3D m_nearestPos;
	int m_selectedSide;
	int m_selectedTraverseType;

	dtTileRef m_markedTileRef;
	dtTileRef m_lastMarkedTileRef;

	dtPolyRef m_markedPolyRef;
	dtPolyRef m_lastMarkedPolyRef;

	enum TileToolCursorMode
	{
		TT_CURSOR_MODE_DEBUG = 0,
		TT_CURSOR_MODE_BUILD
	};

	TileToolCursorMode m_cursorMode;

	enum TextOverlayDrawMode
	{
		TO_DRAW_MODE_DISABLED = -1,
		TO_DRAW_MODE_POLY_FLAGS,
		TO_DRAW_MODE_POLY_GROUPS,
		TO_DRAW_MODE_POLY_SURF_AREAS
	};

	TextOverlayDrawMode m_textOverlayDrawMode;

	enum TextOverlayDrawFlags
	{
		TO_DRAW_FLAGS_NONE = 1 << 0,
		TO_DRAW_FLAGS_INDICES = 1 << 1
	};

	int m_textOverlayDrawFlags;

	char m_tileRefTextInput[MAX_POLYREF_CHARS];
	char m_polyRefTextInput[MAX_POLYREF_CHARS];
	bool m_hitPosSet;
	
public:

	NavMeshTileTool() :
		m_editor(0),
		m_navMesh(0),
		m_navQuery(0),
		m_selectedSide(-2),
		m_selectedTraverseType(-2),
		m_markedTileRef(0),
		m_lastMarkedTileRef(0),
		m_markedPolyRef(0),
		m_lastMarkedPolyRef(0),
		m_cursorMode(TT_CURSOR_MODE_DEBUG),
		m_textOverlayDrawMode(TO_DRAW_MODE_DISABLED),
		m_textOverlayDrawFlags(TO_DRAW_FLAGS_NONE),
		m_hitPosSet(false)
	{
		m_hitPos.init(0.0f,0.0f,0.0f);
		m_nearestPos.init(0.0f,0.0f,0.0f);
		memset(m_tileRefTextInput, '\0', sizeof(m_tileRefTextInput));
		memset(m_polyRefTextInput, '\0', sizeof(m_polyRefTextInput));
	}

	virtual ~NavMeshTileTool()
	{
	}

	virtual int type() { return TOOL_TILE_EDIT; }

	virtual void init(Editor* editor)
	{
		m_editor = (Editor_TileMesh*)editor;
		m_navMesh = editor->getNavMesh();
		m_navQuery = editor->getNavMeshQuery();
	}
	
	virtual void reset() {}

	virtual void handleMenu()
	{
		ImGui::Text("Cursor Mode");
		if (ImGui::RadioButton("Debug##TileTool", m_cursorMode == TT_CURSOR_MODE_DEBUG))
			m_cursorMode = TT_CURSOR_MODE_DEBUG;

		if (ImGui::RadioButton("Build##TileTool", m_cursorMode == TT_CURSOR_MODE_BUILD))
			m_cursorMode = TT_CURSOR_MODE_BUILD;

		ImGui::Separator();
		ImGui::Text("Create Tiles");

		if (ImGui::Button("Create All"))
		{
			if (m_editor)
				m_editor->buildAllTiles();
		}
		if (ImGui::Button("Remove All"))
		{
			if (m_editor)
				m_editor->removeAllTiles();
		}

		ImGui::Separator();
		ImGui::Text("Debug Options");

		if (ImGui::RadioButton("Show Poly Flags", m_textOverlayDrawMode == TO_DRAW_MODE_POLY_FLAGS))
			toggleTextOverlayDrawMode(TO_DRAW_MODE_POLY_FLAGS);

		if (ImGui::RadioButton("Show Poly Groups", m_textOverlayDrawMode == TO_DRAW_MODE_POLY_GROUPS))
			toggleTextOverlayDrawMode(TO_DRAW_MODE_POLY_GROUPS);

		if (ImGui::RadioButton("Show Poly Surface Areas", m_textOverlayDrawMode == TO_DRAW_MODE_POLY_SURF_AREAS))
			toggleTextOverlayDrawMode(TO_DRAW_MODE_POLY_SURF_AREAS);

		if (ImGui::RadioButton("Show Tile And Poly Indices", m_textOverlayDrawFlags & TO_DRAW_FLAGS_INDICES))
			toggleTextOverlayDrawFlags(TO_DRAW_FLAGS_INDICES);


		const bool hasMarker = m_markedTileRef || m_markedPolyRef;

		if (m_navMesh || hasMarker)
		{
			ImGui::Separator();
			ImGui::Text("Markers");
		}

		if (m_navMesh)
		{
			ImGui::PushItemWidth(83);
			if (m_navMesh && ImGui::InputText("Mark Tile By Ref", m_tileRefTextInput, sizeof(m_tileRefTextInput), ImGuiInputTextFlags_EnterReturnsTrue))
			{
				char* pEnd = nullptr;
				m_markedTileRef = (dtPolyRef)STR_TO_ID(m_tileRefTextInput, &pEnd, 10);
			}
			if (m_navMesh && ImGui::InputText("Mark Poly By Ref", m_polyRefTextInput, sizeof(m_polyRefTextInput), ImGuiInputTextFlags_EnterReturnsTrue))
			{
				char* pEnd = nullptr;
				m_markedPolyRef = (dtPolyRef)STR_TO_ID(m_polyRefTextInput, &pEnd, 10);
			}
			ImGui::SliderInt("Tile Side", &m_selectedSide, -2, 7, "%d", ImGuiSliderFlags_NoInput);
			ImGui::PopItemWidth();
		}

		ImGui::PushItemWidth(185);
		ImGui::SliderFloat3("Cursor", (float*)&m_hitPos, MIN_COORD_FLOAT, MAX_COORD_FLOAT);
		ImGui::PopItemWidth();

		if (hasMarker && ImGui::Button("Clear Markers"))
		{
			m_markedTileRef = 0;
			m_markedPolyRef = 0;
			m_nearestPos.init(0.0f, 0.0f, 0.0f);
		}

		if (m_navQuery && m_navMesh)
		{
			ImGui::Separator();
			ImGui::Text("Dumpers");

			ImGui::PushItemWidth(83);
			ImGui::SliderInt("Selected Traverse Type", &m_selectedTraverseType, -2, 31);
			ImGui::PopItemWidth();

			if (ImGui::Button("Dump Traverse Links"))
			{
				const char* modelName = m_editor->getModelName();
				char buf[512];

				if (m_selectedTraverseType == -2)
				{
					for (int i = -1; i < 32; i++)
					{
						snprintf(buf, sizeof(buf), "%s_%d.txt", modelName, i);
						FileIO io;

						if (io.openForWrite(buf))
							duDumpTraverseLinkDetail(*m_navMesh, m_navQuery, i, &io);
					}
				}
				else
				{
					snprintf(buf, sizeof(buf), "%s_%d.txt", modelName, m_selectedTraverseType);
					FileIO io;

					if (io.openForWrite(buf))
						duDumpTraverseLinkDetail(*m_navMesh, m_navQuery, m_selectedTraverseType, &io);
				}
			}
		}
	}

	virtual void handleClick(const rdVec3D* /*s*/, const rdVec3D* p, const int /*v*/, bool shift)
	{
		m_hitPosSet = true;
		m_hitPos=*p;
		if (m_editor)
		{
			if (m_cursorMode == TT_CURSOR_MODE_BUILD)
			{
				if (shift)
					m_editor->removeTile(&m_hitPos);
				else
					m_editor->buildTile(&m_hitPos);
			}
			else if (m_cursorMode == TT_CURSOR_MODE_DEBUG && m_navMesh)
			{
				const rdVec3D halfExtents(2, 2, 4); // math_refactor(kawe): enlarge?
				dtQueryFilter filter;

				if (shift)
				{
					if (dtStatusFailed(m_navQuery->findNearestPoly(&m_hitPos, &halfExtents, &filter, &m_markedPolyRef, &m_nearestPos)))
					{
						m_markedPolyRef = 0;
						m_nearestPos.init(0.0f, 0.0f, 0.0f);
					}
				}
				else
				{
					int tx, ty;
					m_editor->getTilePos(&m_hitPos, tx, ty);
					m_markedTileRef = m_navMesh->getTileRefAt(tx, ty, 0);
				}
			}
		}
	}

	virtual void handleToggle() {}

	virtual void handleStep() {}

	virtual void handleUpdate(const float /*dt*/) {}
	
	virtual void handleRender()
	{
		if (m_hitPosSet)
		{
			const float s = m_editor->getAgentRadius();
			glColor4ub(0,0,0,128);
			glLineWidth(2.0f);
			glBegin(GL_LINES);
			glVertex3f(m_hitPos.x-s,m_hitPos.y,m_hitPos.z+0.1f);
			glVertex3f(m_hitPos.x+s,m_hitPos.y,m_hitPos.z+0.1f);
			glVertex3f(m_hitPos.x,m_hitPos.y-s,m_hitPos.z+0.1f);
			glVertex3f(m_hitPos.x,m_hitPos.y+s,m_hitPos.z+0.1f);
			glVertex3f(m_hitPos.x,m_hitPos.y,m_hitPos.z-s+0.1f);
			glVertex3f(m_hitPos.x,m_hitPos.y,m_hitPos.z+s+0.1f);
			glEnd();
			glLineWidth(1.0f);
		}

		const rdVec3D* debugDrawOffset = m_editor->getDetourDrawOffset();

		if (m_markedTileRef && m_editor && m_navMesh)
		{
			const dtMeshTile* tile = m_navMesh->getTileByRef(m_markedTileRef);

			if (tile && tile->header)
			{
				const dtMeshHeader* header = tile->header;

				if (m_markedTileRef != m_lastMarkedTileRef)
				{
					m_lastMarkedTileRef = m_markedTileRef;

					rdVec3D bmin;
					rdVec3D bmax;
					tile->getTightBounds(&bmin, &bmax);

					rdVsad(&m_nearestPos, &bmin, &bmax, 0.5f);
				}

				duDebugDrawMeshTile(&m_editor->getDebugDraw(), *m_navMesh, m_navQuery, tile, debugDrawOffset, m_editor->getNavMeshDrawFlags(), m_editor->getTraverseLinkDrawParams());

				if (m_selectedSide > -2)
				{
					const int side = (m_selectedSide != -1)
						? m_selectedSide
						: rdClassifyPointOutsideBounds(&m_hitPos, &header->bmin, &header->bmax);

					if (side != 0xff)
					{
						const int MAX_NEIS = 32; // Max neighbors
						dtMeshTile* neis[MAX_NEIS];

						const int nneis = m_navMesh->getNeighbourTilesAt(header->x, header->y, side, neis, MAX_NEIS);

						for (int i = 0; i < nneis; i++)
						{
							const dtMeshTile* neiTile = neis[i];
							duDebugDrawMeshTile(&m_editor->getDebugDraw(), *m_navMesh, m_navQuery, neiTile, debugDrawOffset, m_editor->getNavMeshDrawFlags(), m_editor->getTraverseLinkDrawParams());
						}
					}
				}
			}
		}

		if (m_markedPolyRef && m_editor && m_navMesh)
		{
			const dtMeshTile* tile;
			const dtPoly* poly;
			if (dtStatusSucceed(m_navMesh->getTileAndPolyByRef(m_markedPolyRef, &tile, &poly)))
			{
				if (m_markedPolyRef != m_lastMarkedPolyRef)
				{
					m_lastMarkedPolyRef = m_markedPolyRef;
					m_nearestPos = poly->center;
				}
			}

			duDebugDrawNavMeshPoly(&m_editor->getDebugDraw(), *m_navMesh, m_markedPolyRef,
				debugDrawOffset, m_editor->getNavMeshDrawFlags(), duRGBA(255, 0, 170, 190), false);
		}

		if (m_markedTileRef || m_markedPolyRef)
			duDebugDrawCross(&m_editor->getDebugDraw(), m_nearestPos[0], m_nearestPos[1], m_nearestPos[2], 20.f, duRGBA(0, 0, 255, 255), 2, debugDrawOffset);
	}
	
	virtual void handleRenderOverlay(double* model, double* proj, int* view)
	{
		rdVec2D screenPos;
		const int h = view[3];
		const rdVec3D* drawOffset = m_editor->getDetourDrawOffset();

		// NOTE: don't add the render offset here as we want to keep the overlay at the hit position, this
		// way we can have the navmesh on the side and hit a specific location on the input geometry, and
		// see which tile we build as this will be drawn on the hit position, while we can enumerate all
		// the tiles using the debug options in the NavMeshTileTool which will always be aligned with the
		// navmesh.
		if (m_hitPosSet && worldToScreen(model, proj, view, m_hitPos, screenPos))
		{
			int tx=0, ty=0;
			m_editor->getTilePos(&m_hitPos, tx, ty);

			ImGui_RenderText(ImGuiTextAlign_e::kAlignCenter, ImVec2(screenPos.x, h-(screenPos.y-25)), ImVec4(0,0,0,0.8f), "(%d,%d)", tx,ty);
		}

		if (m_navMesh && m_textOverlayDrawMode != TO_DRAW_MODE_DISABLED)
		{
			for (int i = 0; i < m_navMesh->getMaxTiles(); i++)
			{
				const dtMeshTile* tile = m_navMesh->getTile(i);
				if (!tile->header) continue;

				for (int j = 0; j < tile->header->polyCount; j++)
				{
					const dtPoly* poly = &tile->polys[j];
					unsigned short value = 0;

					const rdVec3D* pos;
					if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
					{
						const unsigned int ip = (unsigned int)(poly - tile->polys);
						dtOffMeshConnection* con = &tile->offMeshCons[ip - tile->header->offMeshBase];

						// Render on end position to prevent clutter, because
						// we already render ref positions on the start pos.
						pos = &con->posb;
					}
					else
					{
						pos = &poly->center;
					}

					switch (m_textOverlayDrawMode)
					{
					case TO_DRAW_MODE_POLY_FLAGS:
						value = poly->flags;
						break;
					case TO_DRAW_MODE_POLY_GROUPS:
						value = poly->groupId;
						break;
					case TO_DRAW_MODE_POLY_SURF_AREAS:
						value = poly->surfaceArea;
						break;
					default:
						// Unhandled text overlay mode.
						rdAssert(0);
					}

					if (worldToScreen(model, proj, view, pos->x+drawOffset->x, pos->y+drawOffset->y, pos->z+drawOffset->z+30, screenPos))
					{
						const char* format = (m_textOverlayDrawFlags & TO_DRAW_FLAGS_INDICES)
							? "%hu (%d,%d)"
							: "%hu";

						ImGui_RenderText(ImGuiTextAlign_e::kAlignCenter,
							ImVec2(screenPos.x, h - screenPos.y), ImVec4(0, 0, 0, 0.8f), format, value, i, j);
					}
				}

				//for (int j = 0; j < tile->header->maxCellCount; j++)
				//{
				//	const dtCell* cell = &tile->cells[j];

				//	if (worldToScreen(model, proj, view, cell->pos.x+drawOffset->x, cell->pos.y+drawOffset->y, cell->pos.z+drawOffset->z+30, screenPos))
				//	{
				//		ImGui_RenderText(ImGuiTextAlign_e::kAlignCenter,
				//			ImVec2(screenPos.x, h - screenPos.y), ImVec4(0, 0.4, 0, 0.8f), "(%d,%d)", j, cell->flags);
				//	}
				//}
			}
		}
		
		// Tool help
		ImGui_RenderText(ImGuiTextAlign_e::kAlignLeft, ImVec2(300, 40),
			ImVec4(1.0f,1.0f,1.0f,0.75f), "LMB: Rebuild hit tile.  Shift+LMB: Clear hit tile.");
	}

	void toggleTextOverlayDrawMode(const TextOverlayDrawMode drawMode)
	{
		m_textOverlayDrawMode == drawMode
			? m_textOverlayDrawMode = TO_DRAW_MODE_DISABLED
			: m_textOverlayDrawMode = drawMode;
	}

	void toggleTextOverlayDrawFlags(unsigned int flag) { m_textOverlayDrawFlags ^= flag; }
};
#undef STR_TO_ID



Editor_TileMesh::Editor_TileMesh() :
	m_buildAll(true),
	m_maxTiles(0),
	m_maxPolysPerTile(0),
	m_tileBuildTime(0),
	m_tileMemUsage(0),
	m_tileTriCount(0)
{
	m_lastBuiltTileBmin.init(0,0,0);
	m_lastBuiltTileBmax.init(0,0,0);
	
	setTool(new NavMeshTileTool);
	m_drawActiveTile = true;
}

Editor_TileMesh::~Editor_TileMesh()
{
	cleanup();
}

void Editor_TileMesh::handleSettings()
{
	Editor::handleCommonSettings();
	
	ImGui::Text("Tiling");
	ImGui::SliderInt("Min Tile Bits", &m_minTileBits, 14, 32);
	ImGui::SliderInt("Max Tile Bits", &m_maxTileBits, 22, 32);
	ImGui::SliderInt("Tile Size", &m_tileSize, 8, 2048);

	ImGui::Checkbox("Build All Tiles", &m_buildAll);
	ImGui::Checkbox("Keep Intermediate Results", &m_keepInterResults);
	
	EditorCommon_SetAndRenderTileProperties(m_geom, m_minTileBits, m_maxTileBits, m_tileSize, m_cellSize, m_maxTiles, m_maxPolysPerTile);
	
	ImGui::Separator();
	Editor_StaticTileMeshCommon::renderIntermediateTileMeshOptions();
}

void Editor_TileMesh::handleTools()
{
	int type = !m_tool ? TOOL_NONE : m_tool->type();
	bool isEnabled = type == TOOL_NAVMESH_TESTER;

	if (ImGui::Checkbox("Test NavMesh", &isEnabled))
	{
		setTool(new NavMeshTesterTool);
	}

	isEnabled = type == TOOL_NAVMESH_PRUNE;
	if (ImGui::Checkbox("Prune NavMesh", &isEnabled))
	{
		setTool(new NavMeshPruneTool);
	}

	isEnabled = type == TOOL_TILE_EDIT;
	if (ImGui::Checkbox("Create Tiles", &isEnabled))
	{
		setTool(new NavMeshTileTool);
	}

	isEnabled = type == TOOL_OFFMESH_CONNECTION;
	if (ImGui::Checkbox("Create Off-Mesh Links", &isEnabled))
	{
		setTool(new OffMeshConnectionTool);
	}

	isEnabled = type == TOOL_SHAPE_VOLUME;
	if (ImGui::Checkbox("Create Shape Volumes", &isEnabled))
	{
		setTool(new ShapeVolumeTool);
	}

	isEnabled = type == TOOL_CROWD;
	if (ImGui::Checkbox("Create Crowds", &isEnabled))
	{
		setTool(new CrowdTool);
	}
	
	ImGui::Separator();

	ImGui::Indent();

	if (m_tool)
		m_tool->handleMenu();

	ImGui::Unindent();
}

void Editor_TileMesh::handleDebugMode()
{
	Editor::renderMeshOffsetOptions();
	ImGui::Separator();
	Editor_StaticTileMeshCommon::renderRecastDebugMenu();
	ImGui::Separator();
	Editor::renderDetourDebugMenu();
}

void Editor_TileMesh::handleRender()
{
	Editor_StaticTileMeshCommon::renderTileMeshData();
}

void Editor_TileMesh::handleRenderOverlay(double* model, double* proj, int* view)
{
	const int h = view[3];
	const rdVec3D* drawOffset = getDetourDrawOffset();

	rdVec3D projectPos(
		((m_lastBuiltTileBmin.x + m_lastBuiltTileBmax.x)/2)+drawOffset->x,
		((m_lastBuiltTileBmin.y + m_lastBuiltTileBmax.y)/2)+drawOffset->y,
		((m_lastBuiltTileBmin.z + m_lastBuiltTileBmax.z)/2)+drawOffset->z);
	
	rdVec2D screenPos;

	// Draw start and end point labels
	if (m_tileBuildTime > 0.0f && worldToScreen(model, proj, view, projectPos, screenPos))
	{
		ImGui_RenderText(ImGuiTextAlign_e::kAlignCenter, ImVec2(screenPos.x, h-(float)(screenPos.y-25)),
			ImVec4(0,0,0,0.8f), "%.3fms / %dTris / %.1fkB", m_tileBuildTime, m_tileTriCount, m_tileMemUsage);
	}
	
	if (m_tool)
		m_tool->handleRenderOverlay(model, proj, view);
	renderOverlayToolStates(model, proj, view);
}

void Editor_TileMesh::handleMeshChanged(InputGeom* geom)
{
	Editor::handleMeshChanged(geom);

	const BuildSettings* buildSettings = geom->getBuildSettings();
	if (buildSettings && buildSettings->tileSize > 0)
		m_tileSize = buildSettings->tileSize;

	cleanup();

	dtFreeNavMesh(m_navMesh);
	m_navMesh = 0;

	if (m_tool)
	{
		m_tool->reset();
		m_tool->init(this);
	}
	resetToolStates();
	initToolStates(this);
}

bool Editor_TileMesh::handleBuild()
{
	if (!m_geom || !m_geom->getMesh())
	{
		m_ctx->log(RC_LOG_ERROR, "buildTiledNavigation: No vertices and triangles.");
		return false;
	}
	
	dtFreeNavMesh(m_navMesh);
	
	m_navMesh = dtAllocNavMesh();
	if (!m_navMesh)
	{
		m_ctx->log(RC_LOG_ERROR, "buildTiledNavigation: Could not allocate navmesh.");
		return false;
	}

	m_loadedNavMeshType = m_selectedNavMeshType;
	m_traverseLinkDrawParams.traverseAnimType = -2;

	dtNavMeshParams params;
	params.orig.x = m_geom->getNavMeshBoundsMax()->x;
	params.orig.y = m_geom->getNavMeshBoundsMin()->y;
	params.orig.z = m_geom->getNavMeshBoundsMin()->z;

	params.tileWidth = m_tileSize*m_cellSize;
	params.tileHeight = m_tileSize*m_cellSize;
	params.maxTiles = m_maxTiles;
	params.maxPolys = m_maxPolysPerTile;
	params.polyGroupCount = 0;
	params.traverseTableSize = 0;
	params.traverseTableCount = 0;
#if DT_NAVMESH_SET_VERSION >= 8
	params.hintCount = 0;
#endif
	
	dtStatus status;
	
	status = m_navMesh->init(&params);
	if (dtStatusFailed(status))
	{
		m_ctx->log(RC_LOG_ERROR, "buildTiledNavigation: Could not init Detour navmesh.");
		return false;
	}
	
	status = m_navQuery->init(m_navMesh, 2048);
	if (dtStatusFailed(status))
	{
		m_ctx->log(RC_LOG_ERROR, "buildTiledNavigation: Could not init Detour navmesh query");
		return false;
	}
	
	if (m_buildAll)
		buildAllTiles();
	
	if (m_tool)
		m_tool->init(this);
	initToolStates(this);

	return true;
}

void Editor_TileMesh::collectSettings(BuildSettings& settings)
{
	Editor::collectSettings(settings);

	settings.tileSize = m_tileSize;
}

void Editor_TileMesh::buildTile(const rdVec3D* pos)
{
	if (!m_geom) return;
	if (!m_navMesh) return;
			
	int tx, ty;
	getTilePos(pos, tx, ty);
	getTileExtents(tx, ty, &m_lastBuiltTileBmin, &m_lastBuiltTileBmax);
	
	m_tileCol = duRGBA(255,255,255,64);
	
	m_ctx->resetLog();
	
	int dataSize = 0;
	unsigned char* data = buildTileMesh(tx, ty, &m_lastBuiltTileBmin, &m_lastBuiltTileBmax, dataSize);

	// Remove any previous data (navmesh owns and deletes the data).
	m_navMesh->removeTile(m_navMesh->getTileRefAt(tx,ty,0),0,0);

	// Add tile, or leave the location empty.
	if (data)
	{
		const dtMeshHeader* header = (const dtMeshHeader*)data;

		// Let the navmesh own the data.
		dtTileRef tileRef = 0;
		dtStatus status = m_navMesh->addTile(data,dataSize,DT_TILE_FREE_DATA,0,&tileRef);
		bool failure = false;

		if (dtStatusFailed(status) || dtStatusFailed(m_navMesh->connectTile(tileRef)))
		{
			rdFree(data);
			failure = true;
		}
		else if (header->offMeshConCount)
			m_navMesh->connectOffMeshLinks(tileRef);

		if (!failure)
		{
			// If there are external off-mesh links landing on
			// this tile, connect them.
			for (int i = 0; i < m_navMesh->getMaxTiles(); i++)
			{
				dtMeshTile* target = m_navMesh->getTile(i);
				const dtMeshHeader* targetHeader = target->header;

				if (!targetHeader)
					continue;

				const dtTileRef targetRef = m_navMesh->getTileRef(target);

				// Connection to self has already been done above.
				if (targetRef == tileRef)
					continue;

				for (int j = 0; j < targetHeader->offMeshConCount; j++)
				{
					const dtOffMeshConnection* con = &target->offMeshCons[j];

					int landTx, landTy;
					getTilePos(&con->posb, landTx, landTy);

					if (landTx == tx && landTy == ty)
						m_navMesh->connectOffMeshLinks(targetRef);
				}
			}

			if (m_buildTraversePortals)
			{
				// Reconnect the traverse links.
				std::shared_mutex polyMapMutex;
				TraverseLinkBuildContext buildCtx;
				buildCtx.editor = this;
				buildCtx.polyMapMutex = &polyMapMutex;

				dtTraverseLinkConnectParams params;
				createTraverseLinkParams(params);
				params.userData = &buildCtx;

				params.linkToNeighbor = false;
				m_navMesh->connectTraverseLinks(tileRef, params);
				params.linkToNeighbor = true;
				m_navMesh->connectTraverseLinks(tileRef, params);
			}

			createStaticPathingData();
		}
	}
	
	m_ctx->dumpLog("Build Tile (%d,%d):", tx,ty);
	invalidateNavMeshCache();
}

void Editor_TileMesh::getTileExtents(int tx, int ty, rdVec3D* tmin, rdVec3D* tmax)
{
	const float ts = m_tileSize * m_cellSize;
	const rdVec3D* bmin = m_geom->getNavMeshBoundsMin();
	const rdVec3D* bmax = m_geom->getNavMeshBoundsMax();
	tmin->x = bmax->x - (tx+1)*ts;
	tmin->y = bmin->y + (ty)*ts;
	tmin->z = bmin->z;

	tmax->x = bmax->x - (tx)*ts;
	tmax->y = bmin->y + (ty+1)*ts;
	tmax->z = bmax->z;
}
void Editor_TileMesh::getTilePos(const rdVec3D* pos, int& tx, int& ty)
{
	if (!m_geom) return;
	
	const rdVec3D* bmin = m_geom->getNavMeshBoundsMin();
	const rdVec3D* bmax = m_geom->getNavMeshBoundsMax();

	const float ts = m_tileSize*m_cellSize;
	tx = (int)((bmax->x - pos->x) / ts);
	ty = (int)((pos->y - bmin->y) / ts);
}

void Editor_TileMesh::removeTile(const rdVec3D* pos)
{
	if (!m_geom) return;
	if (!m_navMesh) return;
	
	int tx, ty;
	getTilePos(pos, tx, ty);
	getTileExtents(tx, ty, &m_lastBuiltTileBmin, &m_lastBuiltTileBmax);
	
	m_tileCol = duRGBA(255,0,0,180);
	const dtTileRef tileRef = m_navMesh->getTileRefAt(tx,ty, 0);

	if (dtStatusSucceed(m_navMesh->removeTile(tileRef, 0, 0)))
	{
		// Update traverse link map so the next time we rebuild this
		// tile, the polygon pairs will be marked as available.
		const unsigned int tileId = m_navMesh->decodePolyIdTile(tileRef);

		for (auto it = m_traverseLinkPolyMap.cbegin(); it != m_traverseLinkPolyMap.cend();)
		{
			const TraverseLinkPolyPair& pair = it->first;

			if (m_navMesh->decodePolyIdTile(pair.poly1) == tileId || 
				m_navMesh->decodePolyIdTile(pair.poly2) == tileId)
			{
				it = m_traverseLinkPolyMap.erase(it);
				continue;
			}

			++it;
		}

		createStaticPathingData();
		invalidateNavMeshCache();
	}
}

struct TileBuildParams
{
	const rcChunkyTriMesh* chunkyMesh;
	const rdVec3D* verts;
	int nverts;
	const ShapeVolume* volumes;
	int volumeCount;
	rcConfig cfg;
	int polyCellRes;
	bool buildBvTree;
	bool filterLowHangingObstacles;
	bool filterLedgeSpans;
	bool filterNeighborSlopes;
	bool filterWalkableLowHeightSpans;
	int partitionType;
	float agentHeight;
	float agentRadius;
	float agentMaxClimb;

	// Off-mesh connection data (copied from InputGeom to avoid const issues).
	rdVec3D* offMeshConVerts;
	rdVec3D* offMeshConRefPos;
	float* offMeshConRads;
	float* offMeshConRefYaws;
	unsigned char* offMeshConDirs;
	unsigned char* offMeshConJumps;
	unsigned char* offMeshConOrders;
	unsigned char* offMeshConAreas;
	unsigned short* offMeshConFlags;
	unsigned short* offMeshConId;
	int offMeshConCount;
};

struct TileBuildResult
{
	unsigned char* data;
	int dataSize;
	int tx;
	int ty;
};

// Standalone tile build function — uses only local variables, safe to call from any thread.
static unsigned char* buildTileMeshStandalone(const TileBuildParams& params, rcContext* ctx,
	const int tx, const int ty, const rdVec3D* bmin, const rdVec3D* bmax, int& dataSize)
{
	const rdVec3D* verts = params.verts;
	const int nverts = params.nverts;
	const rcChunkyTriMesh* chunkyMesh = params.chunkyMesh;

	rcConfig cfg = params.cfg;
	cfg.bmin = *bmin;
	cfg.bmax = *bmax;
	cfg.bmin.x -= cfg.borderSize*cfg.cs;
	cfg.bmin.y -= cfg.borderSize*cfg.cs;
	cfg.bmax.x += cfg.borderSize*cfg.cs;
	cfg.bmax.y += cfg.borderSize*cfg.cs;

	rcHeightfield* solid = rcAllocHeightfield();
	if (!solid)
		return 0;
	if (!rcCreateHeightfield(ctx, *solid, cfg.width, cfg.height, &cfg.bmin, &cfg.bmax, cfg.cs, cfg.ch))
	{
		rcFreeHeightField(solid);
		return 0;
	}

	unsigned char* triareas = new unsigned char[chunkyMesh->maxTrisPerChunk];
	if (!triareas)
	{
		rcFreeHeightField(solid);
		return 0;
	}

	rdVec2D tbmin(cfg.bmin);
	rdVec2D tbmax(cfg.bmax);

	int cid[1024];
	int currentNode = 0;
	bool done = false;
	int tileTriCount = 0;

	do {
		int currentCount = 0;
		done = rcGetChunksOverlappingRect(chunkyMesh, &tbmin, &tbmax, cid, 1024, currentCount, currentNode);
		for (int i = 0; i < currentCount; ++i)
		{
			const rcChunkyTriMeshNode& node = chunkyMesh->nodes[cid[i]];
			const int* ctris = &chunkyMesh->tris[node.i*3];
			const int nctris = node.n;

			tileTriCount += nctris;

			memset(triareas, 0, nctris * sizeof(unsigned char));
			rcMarkWalkableTriangles(ctx, cfg.walkableSlopeAngle,
				verts, nverts, ctris, nctris, triareas, cfg.ignoreWindingOrder);

			if (!rcRasterizeTriangles(ctx, verts, nverts, ctris, triareas, nctris, *solid, cfg.walkableClimb))
			{
				delete[] triareas;
				rcFreeHeightField(solid);
				return 0;
			}
		}
	} while (!done);

	delete[] triareas;

	if (tileTriCount == 0)
	{
		rcFreeHeightField(solid);
		return 0;
	}

	if (params.filterLowHangingObstacles)
		rcFilterLowHangingWalkableObstacles(ctx, cfg.walkableClimb, *solid);
	if (params.filterLedgeSpans)
		rcFilterLedgeSpans(ctx, cfg.walkableHeight, cfg.walkableClimb, params.filterNeighborSlopes, *solid);
	if (params.filterWalkableLowHeightSpans)
		rcFilterWalkableLowHeightSpans(ctx, cfg.walkableHeight, *solid);

	rcCompactHeightfield* chf = rcAllocCompactHeightfield();
	if (!chf)
	{
		rcFreeHeightField(solid);
		return 0;
	}
	if (!rcBuildCompactHeightfield(ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
	{
		rcFreeHeightField(solid);
		rcFreeCompactHeightfield(chf);
		return 0;
	}
	rcFreeHeightField(solid);

	if (!rcErodeWalkableArea(ctx, cfg.walkableRadius, *chf))
	{
		rcFreeCompactHeightfield(chf);
		return 0;
	}

	// Mark areas from shape volumes.
	for (int i = 0; i < params.volumeCount; ++i)
	{
		const ShapeVolume& vol = params.volumes[i];
		switch (vol.type)
		{
		case VOLUME_BOX:
			rcMarkBoxArea(ctx, &vol.verts[0], &vol.verts[1], vol.flags, vol.area, *chf);
			break;
		case VOLUME_CYLINDER:
			rcMarkCylinderArea(ctx, &vol.verts[0], vol.verts[1].x, vol.verts[1].y, vol.flags, vol.area, *chf);
			break;
		case VOLUME_CONVEX:
			rcMarkConvexPolyArea(ctx, vol.verts, vol.nverts, vol.hmin, vol.hmax, vol.flags, vol.area, *chf);
			break;
		}
	}

	rcContourSet* cset = 0;
	rcPolyMesh* pmesh = 0;
	rcPolyMeshDetail* dmesh = 0;
	unsigned char* navData = 0;
	int navDataSize = 0;

	if (params.partitionType == EDITOR_PARTITION_WATERSHED)
	{
		if (!rcBuildDistanceField(ctx, *chf))
			goto cleanup;
		if (!rcBuildRegions(ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
			goto cleanup;
	}
	else if (params.partitionType == EDITOR_PARTITION_MONOTONE)
	{
		if (!rcBuildRegionsMonotone(ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
			goto cleanup;
	}
	else
	{
		if (!rcBuildLayerRegions(ctx, *chf, cfg.borderSize, cfg.minRegionArea))
			goto cleanup;
	}

	cset = rcAllocContourSet();
	if (!cset)
		goto cleanup;
	if (!rcBuildContours(ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset))
		goto cleanup;
	if (cset->nconts == 0)
		goto cleanup;

	pmesh = rcAllocPolyMesh();
	if (!pmesh)
		goto cleanup;
	if (!rcBuildPolyMesh(ctx, *cset, cfg.maxVertsPerPoly, *pmesh))
		goto cleanup;

	dmesh = rcAllocPolyMeshDetail();
	if (!dmesh)
		goto cleanup;
	if (!rcBuildPolyMeshDetail(ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
		goto cleanup;

	rcFreeCompactHeightfield(chf);
	chf = 0;
	rcFreeContourSet(cset);
	cset = 0;

	if (cfg.maxVertsPerPoly <= RD_VERTS_PER_POLYGON)
	{
		if (pmesh->nverts >= 0xffff)
			goto cleanup;

		for (int i = 0; i < pmesh->npolys; ++i)
		{
			if (pmesh->areas[i] == RC_WALKABLE_AREA)
				pmesh->areas[i] = DT_POLYAREA_GROUND;

			if (pmesh->areas[i] == DT_POLYAREA_GROUND ||
				pmesh->areas[i] == DT_POLYAREA_TRIGGER)
				pmesh->flags[i] |= DT_POLYFLAGS_WALK;

			if (pmesh->surfa[i] <= RC_POLY_SURFAREA_TOO_SMALL_THRESHOLD)
				pmesh->flags[i] |= DT_POLYFLAGS_TOO_SMALL;

			const int nvp = pmesh->nvp;
			const unsigned short* p = &pmesh->polys[i*nvp*2];

			for (int j = 0; j < nvp; ++j)
			{
				if (p[j] == RD_MESH_NULL_IDX)
					break;
				if ((p[nvp+j] & 0x8000) == 0)
					continue;
				if ((p[nvp+j] & 0xf) == 0xf)
					continue;

				pmesh->flags[i] |= DT_POLYFLAGS_HAS_NEIGHBOUR;
			}
		}

		dtNavMeshCreateParams createParams;
		memset(&createParams, 0, sizeof(createParams));
		createParams.verts = pmesh->verts;
		createParams.vertCount = pmesh->nverts;
		createParams.polys = pmesh->polys;
		createParams.polyFlags = pmesh->flags;
		createParams.polyAreas = pmesh->areas;
		createParams.surfAreas = pmesh->surfa;
		createParams.polyCount = pmesh->npolys;
		createParams.nvp = pmesh->nvp;
		createParams.cellResolution = params.polyCellRes;
		createParams.detailMeshes = dmesh->meshes;
		createParams.detailVerts = dmesh->verts;
		createParams.detailVertsCount = dmesh->nverts;
		createParams.detailTris = dmesh->tris;
		createParams.detailTriCount = dmesh->ntris;
		createParams.offMeshConVerts = params.offMeshConVerts;
		createParams.offMeshConRefPos = params.offMeshConRefPos;
		createParams.offMeshConRad = params.offMeshConRads;
		createParams.offMeshConRefYaw = params.offMeshConRefYaws;
		createParams.offMeshConDir = params.offMeshConDirs;
		createParams.offMeshConJumps = params.offMeshConJumps;
		createParams.offMeshConOrders = params.offMeshConOrders;
		createParams.offMeshConAreas = params.offMeshConAreas;
		createParams.offMeshConFlags = params.offMeshConFlags;
		createParams.offMeshConUserID = params.offMeshConId;
		createParams.offMeshConCount = params.offMeshConCount;
		createParams.walkableHeight = params.agentHeight;
		createParams.walkableRadius = params.agentRadius;
		createParams.walkableClimb = params.agentMaxClimb;
		createParams.tileX = tx;
		createParams.tileY = ty;
		createParams.tileLayer = 0;
		createParams.bmin = pmesh->bmin;
		createParams.bmax = pmesh->bmax;
		createParams.cs = cfg.cs;
		createParams.ch = cfg.ch;
		createParams.buildBvTree = params.buildBvTree;

		if (!dtCreateNavMeshData(&createParams, &navData, &navDataSize))
			navData = 0;
	}

cleanup:
	rcFreeCompactHeightfield(chf);
	rcFreeContourSet(cset);
	rcFreePolyMesh(pmesh);
	rcFreePolyMeshDetail(dmesh);

	dataSize = navDataSize;
	return navData;
}

void Editor_TileMesh::buildAllTiles()
{
	if (!m_geom) return;
	if (!m_navMesh) return;

	const rdVec3D* bmin = m_geom->getNavMeshBoundsMin();
	const rdVec3D* bmax = m_geom->getNavMeshBoundsMax();
	int gw = 0, gh = 0;
	rcCalcGridSize(bmin, bmax, m_cellSize, &gw, &gh);
	const int ts = m_tileSize;
	const int tw = (gw + ts-1) / ts;
	const int th = (gh + ts-1) / ts;
	const int totalTiles = tw * th;

	// Prepare shared build parameters (read-only during build).
	TileBuildParams params;
	params.chunkyMesh = m_geom->getChunkyMesh();
	params.verts = m_geom->getMesh()->getVerts();
	params.nverts = m_geom->getMesh()->getVertCount();
	params.volumes = m_geom->getShapeVolumes();
	params.volumeCount = m_geom->getShapeVolumeCount();
	params.offMeshConVerts = m_geom->getOffMeshConnectionVerts();
	params.offMeshConRefPos = m_geom->getOffMeshConnectionRefPos();
	params.offMeshConRads = m_geom->getOffMeshConnectionRads();
	params.offMeshConRefYaws = m_geom->getOffMeshConnectionRefYaws();
	params.offMeshConDirs = m_geom->getOffMeshConnectionDirs();
	params.offMeshConJumps = m_geom->getOffMeshConnectionJumps();
	params.offMeshConOrders = m_geom->getOffMeshConnectionOrders();
	params.offMeshConAreas = m_geom->getOffMeshConnectionAreas();
	params.offMeshConFlags = m_geom->getOffMeshConnectionFlags();
	params.offMeshConId = m_geom->getOffMeshConnectionId();
	params.offMeshConCount = m_geom->getOffMeshConnectionCount();
	params.polyCellRes = m_polyCellRes;
	params.buildBvTree = m_buildBvTree;
	params.filterLowHangingObstacles = m_filterLowHangingObstacles;
	params.filterLedgeSpans = m_filterLedgeSpans;
	params.filterNeighborSlopes = m_filterNeighborSlopes;
	params.filterWalkableLowHeightSpans = m_filterWalkableLowHeightSpans;
	params.partitionType = m_partitionType;
	params.agentHeight = m_agentHeight;
	params.agentRadius = m_agentRadius;
	params.agentMaxClimb = m_agentMaxClimb;

	memset(&params.cfg, 0, sizeof(params.cfg));
	params.cfg.cs = m_cellSize;
	params.cfg.ch = m_cellHeight;
	params.cfg.walkableSlopeAngle = m_agentMaxSlope;
	params.cfg.walkableHeight = (int)ceilf(m_agentHeight / m_cellHeight);
	params.cfg.walkableClimb = (int)floorf(m_agentMaxClimb / m_cellHeight);
	params.cfg.walkableRadius = (int)ceilf(m_agentRadius / m_cellSize);
	params.cfg.maxEdgeLen = (int)(m_edgeMaxLen / m_cellSize);
	params.cfg.maxSimplificationError = m_edgeMaxError;
	params.cfg.minRegionArea = rdSqr(m_regionMinSize);
	params.cfg.mergeRegionArea = rdSqr(m_regionMergeSize);
	params.cfg.maxVertsPerPoly = (int)m_vertsPerPoly;
	params.cfg.tileSize = m_tileSize;
	params.cfg.borderSize = params.cfg.walkableRadius + 3;
	params.cfg.width = params.cfg.tileSize + params.cfg.borderSize*2;
	params.cfg.height = params.cfg.tileSize + params.cfg.borderSize*2;
	params.cfg.detailSampleDist = m_detailSampleDist < 0.9f ? 0 : m_cellSize * m_detailSampleDist;
	params.cfg.detailSampleMaxError = m_cellHeight * m_detailSampleMaxError;
	params.cfg.ignoreWindingOrder = m_ignoreWindingOrder;

	// Start the build process.
	m_ctx->resetTimers();
	m_ctx->startTimer(RC_TIMER_TEMP);

	// Phase 1: Build all tile data in parallel.
	std::vector<TileBuildResult> results(totalTiles);

	const int numWorkers = rdMax(1, (int)std::thread::hardware_concurrency() - 1);
	std::atomic<int> nextTile(0);

	auto workerFunc = [&]()
	{
		// Each thread gets its own context for logging/timing.
		BuildContext threadCtx;

		for (;;)
		{
			const int tileIdx = nextTile.fetch_add(1);
			if (tileIdx >= totalTiles)
				break;

			const int tx = tileIdx % tw;
			const int ty = tileIdx / tw;

			rdVec3D tileBmin, tileBmax;
			const float tileCs = ts * m_cellSize;
			tileBmin.x = bmax->x - (tx+1)*tileCs;
			tileBmin.y = bmin->y + (ty)*tileCs;
			tileBmin.z = bmin->z;
			tileBmax.x = bmax->x - (tx)*tileCs;
			tileBmax.y = bmin->y + (ty+1)*tileCs;
			tileBmax.z = bmax->z;

			int dataSize = 0;
			unsigned char* data = buildTileMeshStandalone(params, &threadCtx, tx, ty, &tileBmin, &tileBmax, dataSize);

			results[tileIdx].data = data;
			results[tileIdx].dataSize = dataSize;
			results[tileIdx].tx = tx;
			results[tileIdx].ty = ty;
		}
	};

	std::vector<std::thread> workers;
	workers.reserve(numWorkers);
	for (int i = 0; i < numWorkers; ++i)
		workers.emplace_back(workerFunc);
	for (auto& w : workers)
		w.join();

	// Phase 2: Add tiles to navmesh sequentially (dtNavMesh is not thread-safe).
	rdTimeType phaseStart = getPerfTime();

	for (int i = 0; i < totalTiles; ++i)
	{
		if (results[i].data)
		{
			m_navMesh->removeTile(m_navMesh->getTileRefAt(results[i].tx, results[i].ty, 0), 0, 0);

			dtTileRef tileRef = 0;
			dtStatus status = m_navMesh->addTile(results[i].data, results[i].dataSize, DT_TILE_FREE_DATA, 0, &tileRef);
			if (dtStatusFailed(status))
				rdFree(results[i].data);
			else
				m_navMesh->connectTile(tileRef);
		}
	}

	rdTimeType addTileEnd = getPerfTime();

	connectOffMeshLinks();

	rdTimeType offMeshEnd = getPerfTime();

	if (m_buildTraversePortals)
		createTraverseLinks();

	rdTimeType traverseEnd = getPerfTime();

	createStaticPathingData();

	rdTimeType pathingEnd = getPerfTime();

	m_ctx->log(RC_LOG_PROGRESS, ">> Add tiles:        %.1fms", getPerfTimeUsec(addTileEnd - phaseStart) / 1000.0f);
	m_ctx->log(RC_LOG_PROGRESS, ">> Off-mesh links:   %.1fms", getPerfTimeUsec(offMeshEnd - addTileEnd) / 1000.0f);
	m_ctx->log(RC_LOG_PROGRESS, ">> Traverse links:   %.1fms", getPerfTimeUsec(traverseEnd - offMeshEnd) / 1000.0f);
	m_ctx->log(RC_LOG_PROGRESS, ">> Static pathing:   %.1fms", getPerfTimeUsec(pathingEnd - traverseEnd) / 1000.0f);

	// Stop the build process.
	m_ctx->stopTimer(RC_TIMER_TEMP);

	m_totalBuildTimeMs = m_ctx->getAccumulatedTime(RC_TIMER_TEMP)/1000.0f;
	m_tileCol = duRGBA(0,0,0,64);
	invalidateNavMeshCache();

	// Rebuild the last tile using the member-function path to populate
	// intermediate results (m_solid, m_chf, m_cset, m_pmesh, m_dmesh)
	// for the debug visualization options.
	if (m_keepInterResults && totalTiles > 0)
	{
		const int lastIdx = totalTiles - 1;
		const int lastTx = lastIdx % tw;
		const int lastTy = lastIdx / tw;
		getTileExtents(lastTx, lastTy, &m_lastBuiltTileBmin, &m_lastBuiltTileBmax);
		int tempSize = 0;
		unsigned char* tempData = buildTileMesh(lastTx, lastTy, &m_lastBuiltTileBmin, &m_lastBuiltTileBmax, tempSize);
		if (tempData)
			rdFree(tempData); // Discard the tile data; we only wanted the intermediates.
	}
}

void Editor_TileMesh::removeAllTiles()
{
	if (!m_geom || !m_navMesh)
		return;

	const rdVec3D* bmin = m_geom->getNavMeshBoundsMin();
	const rdVec3D* bmax = m_geom->getNavMeshBoundsMax();
	int gw = 0, gh = 0;
	rcCalcGridSize(bmin, bmax, m_cellSize, &gw, &gh);
	const int ts = m_tileSize;
	const int tw = (gw + ts-1) / ts;
	const int th = (gh + ts-1) / ts;
	
	for (int y = 0; y < th; ++y)
		for (int x = 0; x < tw; ++x)
			m_navMesh->removeTile(m_navMesh->getTileRefAt(x,y,0),0,0);

	m_traverseLinkPolyMap.clear();
	createStaticPathingData();
	invalidateNavMeshCache();
}

void Editor_TileMesh::buildAllHulls(const char* hullFilter, const float cellSizeOverride, const bool force)
{
	for (const NavMeshDefaults_s& h : g_navMeshDefaults)
	{
		if (hullFilter && strcmp(h.name, hullFilter) != 0)
			continue;

		m_agentRadius = h.radius;
		m_agentMaxClimb = h.climbHeight;
		m_agentHeight = h.height;
		m_navmeshName = h.name;
		m_cellSize = (cellSizeOverride > 0.0f) ? cellSizeOverride : h.cellSize;
		m_polyCellRes = h.polyCellResolution;

		m_ctx->resetLog();

		// Must stay free of ImGui: this path runs headless (-console).
		int tileCountX = 0, tileCountY = 0;
		EditorCommon_CalcTileProperties(m_geom, m_minTileBits, m_maxTileBits,
			m_tileSize, m_cellSize, m_maxTiles, m_maxPolysPerTile, tileCountX, tileCountY);

		const int tileCount = tileCountX * tileCountY;
		printf("[bake] hull %s: cellSize=%.3f tiles=%dx%d (%d) maxTiles=%d maxPolysPerTile=%d\n",
			h.name, m_cellSize, tileCountX, tileCountY, tileCount, m_maxTiles, m_maxPolysPerTile);

		// Traverse table is quadratic in poly-group count; unclamped maps runaway here.
		if (tileCount > 8192 && !force)
		{
			printf("[bake] REFUSING hull %s: %d tiles exceeds the 8192 guard. Clamp --bounds, or pass --force.\n",
				h.name, tileCount);
			continue;
		}

		handleBuild();

		m_ctx->dumpLog("Build log %s:", h.name);
		Editor::saveAll(m_modelName.c_str(), m_navMesh);
	}
}

unsigned char* Editor_TileMesh::buildTileMesh(const int tx, const int ty, const rdVec3D* bmin, const rdVec3D* bmax, int& dataSize)
{
	if (!m_geom || !m_geom->getMesh() || !m_geom->getChunkyMesh())
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Input mesh is not specified.");
		return 0;
	}
	
	m_tileMemUsage = 0;
	m_tileBuildTime = 0;
	
	cleanup();
	
	const rdVec3D* verts = m_geom->getMesh()->getVerts();
	const int nverts = m_geom->getMesh()->getVertCount();
	const int ntris = m_geom->getMesh()->getTriCount();
	const rcChunkyTriMesh* chunkyMesh = m_geom->getChunkyMesh();
		
	// Init build configuration from GUI
	memset(&m_cfg, 0, sizeof(m_cfg));
	m_cfg.cs = m_cellSize;
	m_cfg.ch = m_cellHeight;
	m_cfg.walkableSlopeAngle = m_agentMaxSlope;
	m_cfg.walkableHeight = (int)ceilf(m_agentHeight / m_cfg.ch);
	m_cfg.walkableClimb = (int)floorf(m_agentMaxClimb / m_cfg.ch);
	m_cfg.walkableRadius = (int)ceilf(m_agentRadius / m_cfg.cs);
	m_cfg.maxEdgeLen = (int)(m_edgeMaxLen / m_cellSize);
	m_cfg.maxSimplificationError = m_edgeMaxError;
	m_cfg.minRegionArea = rdSqr(m_regionMinSize);		// Note: area = size*size
	m_cfg.mergeRegionArea = rdSqr(m_regionMergeSize);	// Note: area = size*size
	m_cfg.maxVertsPerPoly = (int)m_vertsPerPoly;
	m_cfg.tileSize = m_tileSize;
	m_cfg.borderSize = m_cfg.walkableRadius + 3; // Reserve enough padding.
	m_cfg.width = m_cfg.tileSize + m_cfg.borderSize*2;
	m_cfg.height = m_cfg.tileSize + m_cfg.borderSize*2;
	m_cfg.detailSampleDist = m_detailSampleDist < 0.9f ? 0 : m_cellSize * m_detailSampleDist;
	m_cfg.detailSampleMaxError = m_cellHeight * m_detailSampleMaxError;
	m_cfg.ignoreWindingOrder = m_ignoreWindingOrder;
	
	// Expand the heighfield bounding box by border size to find the extents of geometry we need to build this tile.
	//
	// This is done in order to make sure that the navmesh tiles connect correctly at the borders,
	// and the obstacles close to the border work correctly with the dilation process.
	// No polygons (or contours) will be created on the border area.
	//
	// IMPORTANT!
	//
	//   :''''''''':
	//   : +-----+ :
	//   : |     | :
	//   : |     |<--- tile to build
	//   : |     | :  
	//   : +-----+ :<-- geometry needed
	//   :.........:
	//
	// You should use this bounding box to query your input geometry.
	//
	// For example if you build a navmesh for terrain, and want the navmesh tiles to match the terrain tile size
	// you will need to pass in data from neighbour terrain tiles too! In a simple case, just pass in all the 8 neighbours,
	// or use the bounding box below to only pass in a sliver of each of the 8 neighbours.
	m_cfg.bmin = *bmin;
	m_cfg.bmax = *bmax;
	m_cfg.bmin.x -= m_cfg.borderSize*m_cfg.cs;
	m_cfg.bmin.y -= m_cfg.borderSize*m_cfg.cs;
	m_cfg.bmax.x += m_cfg.borderSize*m_cfg.cs;
	m_cfg.bmax.y += m_cfg.borderSize*m_cfg.cs;
	
	// Reset build times gathering.
	m_ctx->resetTimers();
	
	// Start the build process.
	m_ctx->startTimer(RC_TIMER_TOTAL);
	
	m_ctx->log(RC_LOG_PROGRESS, "Building navigation:");
	m_ctx->log(RC_LOG_PROGRESS, " - %d x %d cells", m_cfg.width, m_cfg.height);
	m_ctx->log(RC_LOG_PROGRESS, " - %.1fK verts, %.1fK tris", nverts/1000.0f, ntris/1000.0f);
	
	// Allocate voxel heightfield where we rasterize our input data to.
	m_solid = rcAllocHeightfield();
	if (!m_solid)
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'solid'.");
		return 0;
	}
	if (!rcCreateHeightfield(m_ctx, *m_solid, m_cfg.width, m_cfg.height, &m_cfg.bmin, &m_cfg.bmax, m_cfg.cs, m_cfg.ch))
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not create solid heightfield.");
		return 0;
	}
	
	// Allocate array that can hold triangle flags.
	// If you have multiple meshes you need to process, allocate
	// an array which can hold the max number of triangles you need to process.
	m_triareas = new unsigned char[chunkyMesh->maxTrisPerChunk];
	if (!m_triareas)
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'm_triareas' (%d).", chunkyMesh->maxTrisPerChunk);
		return 0;
	}
	
	rdVec2D tbmin(m_cfg.bmin);
	rdVec2D tbmax(m_cfg.bmax);

#if 0 //NOTE(warmist): original algo
	int cid[2048];// TODO: Make grow when returning too many items.
	const int ncid = rcGetChunksOverlappingRect(chunkyMesh, tbmin, tbmax, cid, 2048);
	if (!ncid)
		return 0;
	
	m_tileTriCount = 0;
	
	for (int i = 0; i < ncid; ++i)
	{
		const rcChunkyTriMeshNode& node = chunkyMesh->nodes[cid[i]];
		const int* ctris = &chunkyMesh->tris[node.i*3];
		const int nctris = node.n;
		
		m_tileTriCount += nctris;
		
		memset(m_triareas, 0, nctris*sizeof(unsigned char));
		rcMarkWalkableTriangles(m_ctx, m_cfg.walkableSlopeAngle,
								verts, nverts, ctris, nctris, m_triareas, m_cfg.ignoreWindingOrder);
		
		if (!rcRasterizeTriangles(m_ctx, verts, nverts, ctris, m_triareas, nctris, *m_solid, m_cfg.walkableClimb))
			return 0;
	}
#else //NOTE(warmist): algo with limited return but can be reinvoked to continue the query
	int cid[1024];//NOTE: we don't grow it but we reuse it (e.g. like a yieldable function or iterator or sth)
	int currentNode = 0;

	bool done = false;
	m_tileTriCount = 0;
	do{
		int currentCount = 0;
		done=rcGetChunksOverlappingRect(chunkyMesh, &tbmin, &tbmax, cid, 1024,currentCount,currentNode);
		for (int i = 0; i < currentCount; ++i)
		{
			const rcChunkyTriMeshNode& node = chunkyMesh->nodes[cid[i]];
			const int* ctris = &chunkyMesh->tris[node.i*3];
			const int nctris = node.n;

			m_tileTriCount += nctris;

			memset(m_triareas, 0, nctris * sizeof(unsigned char));
			rcMarkWalkableTriangles(m_ctx, m_cfg.walkableSlopeAngle,
				verts, nverts, ctris, nctris, m_triareas, m_cfg.ignoreWindingOrder);

			if (!rcRasterizeTriangles(m_ctx, verts, nverts, ctris, m_triareas, nctris, *m_solid, m_cfg.walkableClimb))
				return 0;
		}
	} while (!done);

	if (m_tileTriCount == 0)
		return 0;
#endif
	if (!m_keepInterResults)
	{
		delete [] m_triareas;
		m_triareas = 0;
	}
	
	// Once all geometry is rasterized, we do initial pass of filtering to
	// remove unwanted overhangs caused by the conservative rasterization
	// as well as filter spans where the character cannot possibly stand.
	if (m_filterLowHangingObstacles)
		rcFilterLowHangingWalkableObstacles(m_ctx, m_cfg.walkableClimb, *m_solid);
	if (m_filterLedgeSpans)
		rcFilterLedgeSpans(m_ctx, m_cfg.walkableHeight, m_cfg.walkableClimb, m_filterNeighborSlopes, *m_solid);
	if (m_filterWalkableLowHeightSpans)
		rcFilterWalkableLowHeightSpans(m_ctx, m_cfg.walkableHeight, *m_solid);
	
	// Compact the heightfield so that it is faster to handle from now on.
	// This will result more cache coherent data as well as the neighbours
	// between walkable cells will be calculated.
	m_chf = rcAllocCompactHeightfield();
	if (!m_chf)
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'chf'.");
		return 0;
	}
	if (!rcBuildCompactHeightfield(m_ctx, m_cfg.walkableHeight, m_cfg.walkableClimb, *m_solid, *m_chf))
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not build compact data.");
		return 0;
	}
	
	if (!m_keepInterResults)
	{
		rcFreeHeightField(m_solid);
		m_solid = 0;
	}

	// Erode the walkable area by agent radius.
	if (!rcErodeWalkableArea(m_ctx, m_cfg.walkableRadius, *m_chf))
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not erode.");
		return 0;
	}

	// (Optional) Mark areas.
	const ShapeVolume* vols = m_geom->getShapeVolumes();
	for (int i = 0; i < m_geom->getShapeVolumeCount(); ++i)
	{
		const ShapeVolume& vol = vols[i];

		switch (vol.type)
		{
		case VOLUME_BOX:
			rcMarkBoxArea(m_ctx, &vol.verts[0], &vol.verts[1], vol.flags, vol.area, *m_chf);
			break;
		case VOLUME_CYLINDER:
			rcMarkCylinderArea(m_ctx, &vol.verts[0], vol.verts[1].x, vol.verts[1].y, vol.flags, vol.area, *m_chf);
			break;
		case VOLUME_CONVEX:
			rcMarkConvexPolyArea(m_ctx, vol.verts, vol.nverts, vol.hmin, vol.hmax, vol.flags, vol.area, *m_chf);
			break;
		}
	}
	
	
	// Partition the heightfield so that we can use simple algorithm later to triangulate the walkable areas.
	// There are 3 partitioning methods, each with some pros and cons:
	// 1) Watershed partitioning
	//   - the classic Recast partitioning
	//   - creates the nicest tessellation
	//   - usually slowest
	//   - partitions the heightfield into nice regions without holes or overlaps
	//   - the are some corner cases where this method creates produces holes and overlaps
	//      - holes may appear when a small obstacles is close to large open area (triangulation can handle this)
	//      - overlaps may occur if you have narrow spiral corridors (i.e stairs), this make triangulation to fail
	//   * generally the best choice if you precompute the navmesh, use this if you have large open areas
	// 2) Monotone partitioning
	//   - fastest
	//   - partitions the heightfield into regions without holes and overlaps (guaranteed)
	//   - creates long thin polygons, which sometimes causes paths with detours
	//   * use this if you want fast navmesh generation
	// 3) Layer partitioning
	//   - quite fast
	//   - partitions the heighfield into non-overlapping regions
	//   - relies on the triangulation code to cope with holes (thus slower than monotone partitioning)
	//   - produces better triangles than monotone partitioning
	//   - does not have the corner cases of watershed partitioning
	//   - can be slow and create a bit ugly tessellation (still better than monotone)
	//     if you have large open areas with small obstacles (not a problem if you use tiles)
	//   * good choice to use for tiled navmesh with medium and small sized tiles
	
	if (m_partitionType == EDITOR_PARTITION_WATERSHED)
	{
		// Prepare for region partitioning, by calculating distance field along the walkable surface.
		if (!rcBuildDistanceField(m_ctx, *m_chf))
		{
			m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not build distance field.");
			return 0;
		}
		
		// Partition the walkable surface into simple regions without holes.
		if (!rcBuildRegions(m_ctx, *m_chf, m_cfg.borderSize, m_cfg.minRegionArea, m_cfg.mergeRegionArea))
		{
			m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not build watershed regions.");
			return 0;
		}
	}
	else if (m_partitionType == EDITOR_PARTITION_MONOTONE)
	{
		// Partition the walkable surface into simple regions without holes.
		// Monotone partitioning does not need distancefield.
		if (!rcBuildRegionsMonotone(m_ctx, *m_chf, m_cfg.borderSize, m_cfg.minRegionArea, m_cfg.mergeRegionArea))
		{
			m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not build monotone regions.");
			return 0;
		}
	}
	else // EDITOR_PARTITION_LAYERS
	{
		// Partition the walkable surface into simple regions without holes.
		if (!rcBuildLayerRegions(m_ctx, *m_chf, m_cfg.borderSize, m_cfg.minRegionArea))
		{
			m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not build layer regions.");
			return 0;
		}
	}
	 	
	// Create contours.
	m_cset = rcAllocContourSet();
	if (!m_cset)
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'cset'.");
		return 0;
	}
	if (!rcBuildContours(m_ctx, *m_chf, m_cfg.maxSimplificationError, m_cfg.maxEdgeLen, *m_cset))
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not create contours.");
		return 0;
	}

	if (m_cset->nconts == 0)
	{
		return 0;
	}
	
	// Build polygon navmesh from the contours.
	m_pmesh = rcAllocPolyMesh();
	if (!m_pmesh)
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'pmesh'.");
		return 0;
	}
	if (!rcBuildPolyMesh(m_ctx, *m_cset, m_cfg.maxVertsPerPoly, *m_pmesh))
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not triangulate contours.");
		return 0;
	}
	
	// Build detail mesh.
	m_dmesh = rcAllocPolyMeshDetail();
	if (!m_dmesh)
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'dmesh'.");
		return 0;
	}
	
	if (!rcBuildPolyMeshDetail(m_ctx, *m_pmesh, *m_chf,
							   m_cfg.detailSampleDist, m_cfg.detailSampleMaxError,
							   *m_dmesh))
	{
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not build polymesh detail.");
		return 0;
	}
	
	if (!m_keepInterResults)
	{
		rcFreeCompactHeightfield(m_chf);
		m_chf = 0;
		rcFreeContourSet(m_cset);
		m_cset = 0;
	}
	
	unsigned char* navData = 0;
	int navDataSize = 0;
	if (m_cfg.maxVertsPerPoly <= RD_VERTS_PER_POLYGON)
	{
		if (m_pmesh->nverts >= 0xffff)
		{
			// The vertex indices are ushorts, and cannot point to more than 0xffff vertices.
			m_ctx->log(RC_LOG_ERROR, "Too many vertices per tile %d (max: %d).", m_pmesh->nverts, 0xffff);
			return 0;
		}
		
		// Update poly flags from areas.
		for (int i = 0; i < m_pmesh->npolys; ++i)
		{
			if (m_pmesh->areas[i] == RC_WALKABLE_AREA)
				m_pmesh->areas[i] = DT_POLYAREA_GROUND;
		
			if (m_pmesh->areas[i] == DT_POLYAREA_GROUND ||
				m_pmesh->areas[i] == DT_POLYAREA_TRIGGER)
				m_pmesh->flags[i] |= DT_POLYFLAGS_WALK;

			if (m_pmesh->surfa[i] <= RC_POLY_SURFAREA_TOO_SMALL_THRESHOLD)
				m_pmesh->flags[i] |= DT_POLYFLAGS_TOO_SMALL;

			const int nvp = m_pmesh->nvp;
			const unsigned short* p = &m_pmesh->polys[i*nvp*2];

			// If polygon connects to a polygon on a neighbouring tile, flag it.
			for (int j = 0; j < nvp; ++j)
			{
				if (p[j] == RD_MESH_NULL_IDX)
					break;
				if ((p[nvp+j] & 0x8000) == 0)
					continue;
				if ((p[nvp+j] & 0xf) == 0xf)
					continue;

				m_pmesh->flags[i] |= DT_POLYFLAGS_HAS_NEIGHBOUR;
			}
		}
		
		dtNavMeshCreateParams params;
		memset(&params, 0, sizeof(params));
		params.verts = m_pmesh->verts;
		params.vertCount = m_pmesh->nverts;
		params.polys = m_pmesh->polys;
		params.polyFlags = m_pmesh->flags;
		params.polyAreas = m_pmesh->areas;
		params.surfAreas = m_pmesh->surfa;
		params.polyCount = m_pmesh->npolys;
		params.nvp = m_pmesh->nvp;
		params.cellResolution = m_polyCellRes;
		params.detailMeshes = m_dmesh->meshes;
		params.detailVerts = m_dmesh->verts;
		params.detailVertsCount = m_dmesh->nverts;
		params.detailTris = m_dmesh->tris;
		params.detailTriCount = m_dmesh->ntris;
		params.offMeshConVerts = m_geom->getOffMeshConnectionVerts();
		params.offMeshConRefPos = m_geom->getOffMeshConnectionRefPos();
		params.offMeshConRad = m_geom->getOffMeshConnectionRads();
		params.offMeshConRefYaw = m_geom->getOffMeshConnectionRefYaws();
		params.offMeshConDir = m_geom->getOffMeshConnectionDirs();
		params.offMeshConJumps = m_geom->getOffMeshConnectionJumps();
		params.offMeshConOrders = m_geom->getOffMeshConnectionOrders();
		params.offMeshConAreas = m_geom->getOffMeshConnectionAreas();
		params.offMeshConFlags = m_geom->getOffMeshConnectionFlags();
		params.offMeshConUserID = m_geom->getOffMeshConnectionId();
		params.offMeshConCount = m_geom->getOffMeshConnectionCount();
		params.walkableHeight = m_agentHeight;
		params.walkableRadius = m_agentRadius;
		params.walkableClimb = m_agentMaxClimb;
		params.tileX = tx;
		params.tileY = ty;
		params.tileLayer = 0;
		params.bmin = m_pmesh->bmin;
		params.bmax = m_pmesh->bmax;
		params.cs = m_cfg.cs;
		params.ch = m_cfg.ch;
		params.buildBvTree = m_buildBvTree;

		const bool navMeshBuildSuccess = dtCreateNavMeshData(&params, &navData, &navDataSize);

		// Restore poly areas.
		for (int i = 0; i < m_pmesh->npolys; ++i)
		{
			// The game's poly area (ground) shares the same value as
			// RC_NULL_AREA, if we try to render the recast polymesh cache
			// without restoring this, the renderer will draw it as NULL area
			// even though it's walkable. The other values will get color ID'd
			// by the renderer so we don't need to check on those.
			if (m_pmesh->areas[i] == DT_POLYAREA_GROUND)
				m_pmesh->areas[i] = RC_WALKABLE_AREA;
		}

		if (!navMeshBuildSuccess)
		{
			m_ctx->log(RC_LOG_ERROR, "Could not build Detour navmesh.");
			return 0;
		}
	}
	m_tileMemUsage = navDataSize/1024.0f;
	
	m_ctx->stopTimer(RC_TIMER_TOTAL);
	
	// Show performance stats.
	duLogBuildTimes(*m_ctx, m_ctx->getAccumulatedTime(RC_TIMER_TOTAL));
	m_ctx->log(RC_LOG_PROGRESS, ">> Polymesh: %d vertices  %d polygons", m_pmesh->nverts, m_pmesh->npolys);
	
	m_tileBuildTime = m_ctx->getAccumulatedTime(RC_TIMER_TOTAL)/1000.0f;

	dataSize = navDataSize;
	return navData;
}
