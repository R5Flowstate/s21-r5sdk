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

#include "Shared/Include/SharedCommon.h"
#include "Detour/Include/DetourNode.h"
#include "DetourCrowd/Include/DetourCrowd.h"
#include "DetourCrowd/Include/DetourObstacleAvoidance.h"
#include "DebugUtils/Include/DetourDebugDraw.h"
#include "NavEditor/Include/CrowdTool.h"
#include "NavEditor/Include/InputGeom.h"
#include "NavEditor/Include/Editor.h"
#include "NavEditor/Include/EditorInterfaces.h"
#include "NavEditor/Include/CameraUtils.h"
#include "DetourCrowd/Include/DetourCrowdInternal.h"

static void getAgentBounds(const dtCrowdAgent* ag, rdVec3D* bmin, rdVec3D* bmax)
{
	const rdVec3D* p = &ag->npos;
	const float r = ag->params.radius;
	const float h = ag->params.height;
	bmin->x = p->x - r;
	bmin->y = p->y - r;
	bmin->z = p->z;
	bmax->x = p->x + r;
	bmax->y = p->y + r;
	bmax->z = p->z + h;
}

CrowdToolState::CrowdToolState() :
	m_editor(0),
	m_nav(0),
	m_crowd(0),
	m_targetRef(0),
	m_graphSampleTime(0.0f),
	m_run(true)
{
	m_toolParams.m_showCorners = false;
	m_toolParams.m_showCollisionSegments = false;
	m_toolParams.m_showPath = false;
	m_toolParams.m_showVO = false;
	m_toolParams.m_showOpt = false;
	m_toolParams.m_showNeis = false;
	m_toolParams.m_showLabels = false;
	m_toolParams.m_showGrid = false;
	m_toolParams.m_showNodes = false;
	m_toolParams.m_showPerfGraph = false;
	m_toolParams.m_showDetailAll = false;
	m_toolParams.m_anticipateTurns = true;
	m_toolParams.m_optimizeVis = true;
	m_toolParams.m_optimizeTopo = true;
	m_toolParams.m_obstacleAvoidance = true;
	m_toolParams.m_obstacleAvoidanceType = 3;
	m_toolParams.m_separation = true;
	m_toolParams.m_separationWeight = 20.0f;
	m_toolParams.m_maxAcceleration = 800.f;
	m_toolParams.m_maxSpeed = 200.f;
	m_toolParams.m_traverseAnimType = ANIMTYPE_NONE;
	
	memset(m_trails, 0, sizeof(m_trails));
	
	m_vod = dtAllocObstacleAvoidanceDebugData();
	m_vod->init(2048);
	
	memset(&m_agentDebug, 0, sizeof(m_agentDebug));
	m_agentDebug.idx = -1;
	m_agentDebug.vod = m_vod;
}

CrowdToolState::~CrowdToolState()
{
	dtFreeObstacleAvoidanceDebugData(m_vod);
}

void CrowdToolState::init(class Editor* editor)
{
	if (m_editor != editor)
	{
		m_editor = editor;
	}
	
	dtNavMesh* nav = m_editor->getNavMesh();
	dtCrowd* crowd = m_editor->getCrowd();

	m_toolParams.m_traverseAnimType = NavMesh_GetFirstTraverseAnimTypeForType(m_editor->getLoadedNavMeshType());
	
	if (nav && crowd && (m_nav != nav || m_crowd != crowd))
	{
		m_nav = nav;
		m_crowd = crowd;
	
		crowd->init(MAX_AGENTS, m_editor->getAgentRadius(), nav);
		
		for (int i = ANIMTYPE_NONE; i < ANIMTYPE_COUNT; i++)
		{
			// +1 because the first one is the default one, for crowd agents
			// with traverseAnimType == ANIMTYPE_NONE. ANIMTYPE_NONE is -1.
			dtQueryFilter* const filter = crowd->getEditableFilter(i+1);

			filter->setIncludeFlags(DT_POLYFLAGS_ALL);
			filter->setExcludeFlags(DT_POLYFLAGS_DISABLED);

			if (i > ANIMTYPE_NONE)
				filter->setTraverseFlags(Editor::getTraverseFlags(TraverseAnimType_e(i)));
		}
		
		// Setup local avoidance params to different qualities.
		dtObstacleAvoidanceParams params;
		// Use mostly default settings, copy from dtCrowd.
		memcpy(&params, crowd->getObstacleAvoidanceParams(0), sizeof(dtObstacleAvoidanceParams));
		
		// Low (11)
		params.velBias = 0.5f;
		params.adaptiveDivs = 5;
		params.adaptiveRings = 2;
		params.adaptiveDepth = 1;
		crowd->setObstacleAvoidanceParams(0, &params);
		
		// Medium (22)
		params.velBias = 0.5f;
		params.adaptiveDivs = 5; 
		params.adaptiveRings = 2;
		params.adaptiveDepth = 2;
		crowd->setObstacleAvoidanceParams(1, &params);
		
		// Good (45)
		params.velBias = 0.5f;
		params.adaptiveDivs = 7;
		params.adaptiveRings = 2;
		params.adaptiveDepth = 3;
		crowd->setObstacleAvoidanceParams(2, &params);
		
		// High (66)
		params.velBias = 0.5f;
		params.adaptiveDivs = 7;
		params.adaptiveRings = 3;
		params.adaptiveDepth = 3;
		
		crowd->setObstacleAvoidanceParams(3, &params);
	}
}

void CrowdToolState::reset()
{
}

void CrowdToolState::handleRender()
{
	duDebugDraw& dd = m_editor->getDebugDraw();
	const float rad = m_editor->getAgentRadius();
	
	dtNavMesh* nav = m_editor->getNavMesh();
	dtCrowd* crowd = m_editor->getCrowd();
	if (!nav || !crowd)
		return;

	const rdVec3D* drawOffset = m_editor->getDetourDrawOffset();
	const unsigned int drawFlags = m_editor->getNavMeshDrawFlags();
	
	if (m_toolParams.m_showNodes && crowd->getPathQueue())
	{
		const dtNavMeshQuery* navquery = crowd->getPathQueue()->getNavQuery();
		if (navquery)
			duDebugDrawNavMeshNodes(&dd, *navquery, drawOffset);
	}

	dd.depthMask(false);
	
	// Draw paths
	if (m_toolParams.m_showPath)
	{
		for (int i = 0; i < crowd->getAgentCount(); i++)
		{
			if (m_toolParams.m_showDetailAll == false && i != m_agentDebug.idx)
				continue;
			const dtCrowdAgent* ag =crowd->getAgent(i);
			if (!ag->active)
				continue;
			const dtPolyRef* path = ag->corridor.getPath();
			const int npath = ag->corridor.getPathCount();			
			for (int j = 0; j < npath; ++j)
				duDebugDrawNavMeshPoly(&dd, *nav, path[j], drawOffset, drawFlags, duRGBA(255,255,255,24));
		}
	}
	
	if (m_targetRef)
		duDebugDrawCross(&dd, m_targetPos[0],m_targetPos[1],m_targetPos[2]+0.1f, rad, duRGBA(255,255,255,192), 2.0f, drawOffset);
	
	// Occupancy grid.
	if (m_toolParams.m_showGrid)
	{
		float gridz = -FLT_MAX;
		for (int i = 0; i < crowd->getAgentCount(); ++i)
		{
			const dtCrowdAgent* ag = crowd->getAgent(i);
			if (!ag->active) continue;
			const rdVec3D* pos = ag->corridor.getPos();
			gridz = rdMax(gridz, pos->z);
		}
		gridz += 1.0f;
		
		dd.begin(DU_DRAW_QUADS,1.0f,drawOffset);
		const dtProximityGrid* grid = crowd->getGrid();
		const int* bounds = grid->getBounds();
		const float cs = grid->getCellSize();
		for (int y = bounds[1]; y <= bounds[3]; ++y)
		{
			for (int x = bounds[0]; x <= bounds[2]; ++x)
			{
				const int count = grid->getItemCountAt(x,y);
				if (!count) continue;
				unsigned int col = duRGBA(128,0,0,rdMin(count*40,255));

				dd.vertex(x*cs+cs,y*cs,gridz, col);
				dd.vertex(x*cs+cs,y*cs+cs,gridz,col);
				dd.vertex(x*cs,y*cs+cs,gridz,col);
				dd.vertex(x*cs,y*cs,gridz, col);
			}
		}
		dd.end();
	}
	
	// Trail
	for (int i = 0; i < crowd->getAgentCount(); ++i)
	{
		const dtCrowdAgent* ag = crowd->getAgent(i);
		if (!ag->active) continue;
		
		const AgentTrail* trail = &m_trails[i];
		const rdVec3D* pos = &ag->npos;
		
		dd.begin(DU_DRAW_LINES,3.0f,drawOffset);
		rdVec3D prev(*pos);
		float preva = 1;
		for (int j = 0; j < AGENT_MAX_TRAIL-1; ++j)
		{
			const int idx = (trail->htrail + AGENT_MAX_TRAIL-j) % AGENT_MAX_TRAIL;
			const rdVec3D* v = &trail->trail[idx];
			float a = 1 - j/(float)AGENT_MAX_TRAIL;
			dd.vertex(prev.x,prev.y,prev.z+0.1f, duRGBA(0,0,0,(int)(128*preva)));
			dd.vertex(v->x,v->y,v->z+0.1f, duRGBA(0,0,0,(int)(128*a)));
			preva = a;
			rdVcopy(&prev, v);
		}
		dd.end();
		
	}
	
	// Corners & co
	for (int i = 0; i < crowd->getAgentCount(); i++)
	{
		if (m_toolParams.m_showDetailAll == false && i != m_agentDebug.idx)
			continue;
		const dtCrowdAgent* ag =crowd->getAgent(i);
		if (!ag->active)
			continue;
			
		const float agentRadius = ag->params.radius;
		const rdVec3D* agentPos = &ag->npos;
		
		if (m_toolParams.m_showCorners)
		{
			if (ag->ncorners)
			{
				dd.begin(DU_DRAW_LINES, 2.0f,drawOffset);
				for (int j = 0; j < ag->ncorners; ++j)
				{
					const rdVec3D* va = j == 0 ? agentPos : &ag->cornerVerts[(j-1)];
					const rdVec3D* vb = &ag->cornerVerts[j];
					dd.vertex(va->x,va->y,va->z+agentRadius, duRGBA(128,0,0,192));
					dd.vertex(vb->x,vb->y,vb->z+agentRadius, duRGBA(128,0,0,192));
				}
				if (ag->ncorners && dtIsStraightPathOffmeshConnection(ag->cornerFlags[ag->ncorners-1]))
				{
					const rdVec3D* v = &ag->cornerVerts[(ag->ncorners-1)];
					dd.vertex(v->x,v->y,v->z, duRGBA(192,0,0,192));
					dd.vertex(v->x,v->y,v->z+agentRadius*2, duRGBA(192,0,0,192));
				}
				
				dd.end();
				
				
				if (m_toolParams.m_anticipateTurns)
				{
					rdVec3D dvel, pos;
					calcSmoothSteerDirection(ag, &dvel);
					pos.x = ag->npos.x + dvel.x;
					pos.y = ag->npos.y + dvel.y;
					pos.z = ag->npos.z + dvel.z;

					const float off = ag->params.radius+0.1f;
					const rdVec3D* tgt = ag->cornerVerts;
					const float z = ag->npos.z+off;

					dd.begin(DU_DRAW_LINES, 2.0f, drawOffset);

					dd.vertex(ag->npos[0],ag->npos[1],z, duRGBA(255,0,0,192));
					dd.vertex(pos.x,pos.y,z, duRGBA(255,0,0,192));
					dd.vertex(pos.x,pos.y,z, duRGBA(255,0,0,192));
					dd.vertex(tgt->x,tgt->y,z, duRGBA(255,0,0,192));

					dd.end();
				}
			}
		}
		
		if (m_toolParams.m_showCollisionSegments)
		{
			const rdVec3D* center = ag->boundary.getCenter();
			duDebugDrawCross(&dd, center->x,center->y,center->z+agentRadius, 0.2f, duRGBA(192,0,128,255), 2.0f, drawOffset);
			duDebugDrawCircle(&dd, center->x,center->y,center->z+agentRadius, ag->params.collisionQueryRange,
							  duRGBA(192,0,128,128), 2.0f, drawOffset);
			
			dd.begin(DU_DRAW_LINES, 3.0f, drawOffset);
			for (int j = 0; j < ag->boundary.getSegmentCount(); ++j)
			{
				const rdVec3D* s = ag->boundary.getSegmentStart(j);
				const rdVec3D* e = ag->boundary.getSegmentEnd(j);
				unsigned int col = duRGBA(192,0,128,192);
				if (rdTriArea2D(agentPos, s, e) < 0.0f)
					col = duDarkenCol(col);
				
				duAppendArrow(&dd, s->x,s->y,s->z+0.2f, e->x,e->y,e->z+0.2f, 0.0f, 30.0f, col);
			}
			dd.end();
		}
		
		if (m_toolParams.m_showNeis)
		{
			duDebugDrawCircle(&dd, agentPos->x,agentPos->y,agentPos->z+agentRadius, ag->params.collisionQueryRange,
							  duRGBA(0,192,128,128), 2.0f, drawOffset);
			
			dd.begin(DU_DRAW_LINES, 2.0f, drawOffset);
			for (int j = 0; j < ag->nneis; ++j)
			{
				// Get 'n'th active agent.
				// TODO: fix this properly.
				const dtCrowdAgent* nei = crowd->getAgent(ag->neis[j].idx);
				if (nei)
				{
					dd.vertex(agentPos->x,agentPos->y,agentPos->z+agentRadius, duRGBA(0,192,128,128));
					dd.vertex(nei->npos.x,nei->npos.y,nei->npos.z+agentRadius, duRGBA(0,192,128,128));
				}
			}
			dd.end();
		}
		
		if (m_toolParams.m_showOpt)
		{
			dd.begin(DU_DRAW_LINES, 2.0f, drawOffset);
			dd.vertex(m_agentDebug.optStart[0],m_agentDebug.optStart[1],m_agentDebug.optStart[2]+0.3f, duRGBA(0,128,0,192));
			dd.vertex(m_agentDebug.optEnd[0],m_agentDebug.optEnd[1],m_agentDebug.optEnd[2]+0.3f, duRGBA(0,128,0,192));
			dd.end();
		}
	}
	
	// Agent cylinders.
	for (int i = 0; i < crowd->getAgentCount(); ++i)
	{
		const dtCrowdAgent* ag = crowd->getAgent(i);
		if (!ag->active) continue;
		
		const float radius = ag->params.radius;
		const rdVec3D* pos = &ag->npos;
		
		unsigned int col = duRGBA(0,0,0,32);
		if (m_agentDebug.idx == i)
			col = duRGBA(255,0,0,128);
			
		duDebugDrawCircle(&dd, pos->x, pos->y, pos->z, radius, col, 2.0f, drawOffset);
	}
	
	for (int i = 0; i < crowd->getAgentCount(); ++i)
	{
		const dtCrowdAgent* ag = crowd->getAgent(i);
		if (!ag->active) continue;
		
		const float height = ag->params.height;
		const float radius = ag->params.radius;
		const rdVec3D* pos = &ag->npos;
		
		unsigned int col = duRGBA(220,220,220,128);
		if (ag->targetState == DT_CROWDAGENT_TARGET_REQUESTING || ag->targetState == DT_CROWDAGENT_TARGET_WAITING_FOR_QUEUE)
			col = duLerpCol(col, duRGBA(128,0,255,128), 32);
		else if (ag->targetState == DT_CROWDAGENT_TARGET_WAITING_FOR_PATH)
			col = duLerpCol(col, duRGBA(128,0,255,128), 128);
		else if (ag->targetState == DT_CROWDAGENT_TARGET_FAILED)
			col = duRGBA(255,32,16,128);
		else if (ag->targetState == DT_CROWDAGENT_TARGET_VELOCITY)
			col = duLerpCol(col, duRGBA(64,255,0,128), 128);
		
		duDebugDrawCylinder(&dd, pos->x-radius, pos->y-radius, pos->z+radius*0.1f,
							pos->x+radius, pos->y+radius, pos->z+height, col, drawOffset);
	}
	
	if (m_toolParams.m_showVO)
	{
		for (int i = 0; i < crowd->getAgentCount(); i++)
		{
			if (m_toolParams.m_showDetailAll == false && i != m_agentDebug.idx)
				continue;
			const dtCrowdAgent* ag =crowd->getAgent(i);
			if (!ag->active)
				continue;
		
			// Draw detail about agent sela
			const dtObstacleAvoidanceDebugData* vod = m_agentDebug.vod;
			
			const float dx = ag->npos.x;
			const float dy = ag->npos.y;
			const float dz = ag->npos.z+ag->params.height;
			
			duDebugDrawCircle(&dd, dx,dy,dz, ag->params.maxSpeed, duRGBA(255,255,255,64), 2.0f, drawOffset);
			
			dd.begin(DU_DRAW_QUADS, 1.0f, drawOffset);
			for (int j = 0; j < vod->getSampleCount(); ++j)
			{
				const rdVec3D* p = vod->getSampleVelocity(j);
				const float sr = vod->getSampleSize(j);
				const float pen = vod->getSamplePenalty(j);
				const float pen2 = vod->getSamplePreferredSidePenalty(j);
				unsigned int col = duLerpCol(duRGBA(255,255,255,220), duRGBA(128,96,0,220), (int)(pen*255));
				col = duLerpCol(col, duRGBA(128,0,0,220), (int)(pen2*128));
				dd.vertex(dx+p->x-sr, dy+p->y-sr, dz, col);
				dd.vertex(dx+p->x-sr, dy+p->y+sr, dz, col);
				dd.vertex(dx+p->x+sr, dy+p->y+sr, dz, col);
				dd.vertex(dx+p->x+sr, dy+p->y-sr, dz, col);
			}
			dd.end();
		}
	}
	
	// Velocity stuff.
	for (int i = 0; i < crowd->getAgentCount(); ++i)
	{
		const dtCrowdAgent* ag = crowd->getAgent(i);
		if (!ag->active) continue;
		
		const float radius = ag->params.radius;
		const float height = ag->params.height;
		const rdVec3D* pos = &ag->npos;
		const rdVec3D* vel = &ag->vel;
		const rdVec3D* dvel = &ag->dvel;
		
		unsigned int col = duRGBA(220,220,220,192);
		if (ag->targetState == DT_CROWDAGENT_TARGET_REQUESTING || ag->targetState == DT_CROWDAGENT_TARGET_WAITING_FOR_QUEUE)
			col = duLerpCol(col, duRGBA(128,0,255,192), 32);
		else if (ag->targetState == DT_CROWDAGENT_TARGET_WAITING_FOR_PATH)
			col = duLerpCol(col, duRGBA(128,0,255,192), 128);
		else if (ag->targetState == DT_CROWDAGENT_TARGET_FAILED)
			col = duRGBA(255,32,16,192);
		else if (ag->targetState == DT_CROWDAGENT_TARGET_VELOCITY)
			col = duLerpCol(col, duRGBA(64,255,0,192), 128);
		
		duDebugDrawCircle(&dd, pos->x, pos->y, pos->z+height, radius, col, 2.0f, drawOffset);
		
		duDebugDrawArrow(&dd, pos->x,pos->y,pos->z+height,
						 pos->x+dvel->x,pos->y+dvel->y,pos->z+height+dvel->z,
						 0.0f, 30.0f, duRGBA(0,192,255,192), (m_agentDebug.idx == i) ? 2.0f : 1.0f, drawOffset);
		
		duDebugDrawArrow(&dd, pos->x,pos->y,pos->z+height,
						 pos->x+vel->x,pos->y+vel->y,pos->z+height+vel->z,
						 0.0f, 30.0f, duRGBA(0,0,0,160), 2.0f, drawOffset);
	}
	
	dd.depthMask(true);
}

void CrowdToolState::handleRenderOverlay(double* model, double* proj, int* view)
{
	rdVec2D screenPos;
	const int windowHeight = view[3];
	const rdVec3D* drawOffset = m_editor->getDetourDrawOffset();

	// Draw start and end point labels
	if (m_targetRef && worldToScreen(model, proj, view, m_targetPos.x+drawOffset->x, m_targetPos.y+drawOffset->y, m_targetPos.z+drawOffset->z, screenPos))
	{
		ImGui_RenderText(ImGuiTextAlign_e::kAlignCenter,
			ImVec2(screenPos.x, windowHeight-(screenPos.y+25)), ImVec4(0,0,0,0.8f), "TARGET");
	}
	

	if (m_toolParams.m_showNodes)
	{
		dtCrowd* crowd = m_editor->getCrowd();
		if (crowd && crowd->getPathQueue())
		{
			const dtNavMeshQuery* navquery = crowd->getPathQueue()->getNavQuery();
			const dtNodePool* pool = navquery->getNodePool();
			if (pool)
			{
				const float off = 0.5f;
				for (int i = 0; i < pool->getHashSize(); ++i)
				{
					for (dtNodeIndex j = pool->getFirst(i); j != DT_NULL_IDX; j = pool->getNext(j))
					{
						const dtNode* node = pool->getNodeAtIdx(j+1);
						if (!node) continue;

						if (worldToScreen(model, proj, view, node->pos.x+drawOffset->x,node->pos.y+drawOffset->y,node->pos.z+drawOffset->z+off, screenPos))
						{
							const float heuristic = node->total;// - node->cost;
							ImGui_RenderText(ImGuiTextAlign_e::kAlignCenter,
								ImVec2(screenPos.x, windowHeight-(screenPos.y+25)), ImVec4(0,0,0,0.8f), "%.2f", heuristic);
						}
					}
				}
			}
		}
	}
	
	if (m_toolParams.m_showLabels)
	{
		dtCrowd* crowd = m_editor->getCrowd();
		if (crowd)
		{
			for (int i = 0; i < crowd->getAgentCount(); ++i)
			{
				const dtCrowdAgent* ag = crowd->getAgent(i);
				if (!ag->active) continue;
				const rdVec3D* pos = &ag->npos;
				const float h = ag->params.height;

				if (worldToScreen(model, proj, view, pos->x+drawOffset->x, pos->y+drawOffset->y, pos->z+drawOffset->z+h, screenPos))
				{
					const TraverseAnimType_e animType = ag->params.traverseAnimType;
					const char* animTypeName = animType == ANIMTYPE_NONE
						? "none"
						: g_traverseAnimTypeNames[animType];

					ImGui_RenderText(ImGuiTextAlign_e::kAlignCenter,
						ImVec2(screenPos.x, windowHeight-(screenPos.y+15)), ImVec4(0,0,0,0.8f), "%s (%d)", animTypeName, i);
				}
			}
		}
	}
	if (m_agentDebug.idx != -1)
	{
		dtCrowd* crowd = m_editor->getCrowd();
		if (crowd) 
		{
			for (int i = 0; i < crowd->getAgentCount(); i++)
			{
				if (m_toolParams.m_showDetailAll == false && i != m_agentDebug.idx)
					continue;
				const dtCrowdAgent* ag =crowd->getAgent(i);
				if (!ag->active)
					continue;
				const float radius = ag->params.radius;
				if (m_toolParams.m_showNeis)
				{
					for (int j = 0; j < ag->nneis; ++j)
					{
						const dtCrowdAgent* nei = crowd->getAgent(ag->neis[j].idx);
						if (!nei->active) continue;

						if (worldToScreen(model, proj, view, nei->npos.x+drawOffset->x, nei->npos.y+drawOffset->y, nei->npos.z+drawOffset->z+radius, screenPos))
						{
							ImGui_RenderText(ImGuiTextAlign_e::kAlignCenter, 
								ImVec2(screenPos.x, windowHeight-(screenPos.y+15)), ImVec4(1.0f,1.0f,1.0f,0.8f), "%.3f", ag->neis[j].dist);
						}
					}
				}
			}
		}
	}
	
	if (m_toolParams.m_showPerfGraph)
	{
		static const ImPlotAxisFlags flags = ImPlotAxisFlags_None;
		ImVec2* totalSample = m_crowdTotalTime.getSampleBuffer();
		ImVec2* crowdSample = m_crowdSampleCount.getSampleBuffer();

		ImGui::SetNextWindowPos(ImVec2(270.f+30.f, 10.f), ImGuiCond_Once);
		ImGui::SetNextWindowSizeConstraints(ImVec2(420, 200), ImVec2(FLT_MAX, FLT_MAX));

		if (ImGui::Begin("Graph", nullptr))
		{
			static const float history = 4.0f;

			ImVec2 windowSize = ImGui::GetWindowSize();
			if (ImPlot::BeginPlot("##GraphPlotter", ImVec2(windowSize.x-16.f, windowSize.y-52.f)))
			{
				ImPlot::SetupAxes(nullptr, nullptr, flags, flags);
				ImPlot::SetupAxisLimits(ImAxis_X1, m_graphSampleTime - history, m_graphSampleTime, ImGuiCond_Always);
				ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 2);
				ImPlot::SetNextFillStyle(IMPLOT_AUTO_COL, 0.5f);

				ImPlot::PushStyleVar(ImPlotStyleVar_FillAlpha, 0.25f);
				ImPlot::PlotShaded("Total Time", &totalSample[0].x, &totalSample[0].y, m_crowdTotalTime.getSampleCount(), -INFINITY, 0, m_crowdTotalTime.getSampleOffset(), 2 * sizeof(float));
				ImPlot::PlotShaded("Sample Count", &crowdSample[0].x, &crowdSample[0].y, m_crowdSampleCount.getSampleCount(), -INFINITY, 0, m_crowdSampleCount.getSampleOffset(), 2 * sizeof(float));
				ImPlot::PopStyleVar();

				ImPlot::PlotLine("Total Time", &totalSample[0].x, &totalSample[0].y, m_crowdTotalTime.getSampleCount(), 0, m_crowdTotalTime.getSampleOffset(), 2 * sizeof(float));
				ImPlot::PlotLine("Sample Count", &crowdSample[0].x, &crowdSample[0].y, m_crowdSampleCount.getSampleCount(), 0, m_crowdSampleCount.getSampleOffset(), 2 * sizeof(float));

				ImPlot::EndPlot();

				char labelBuffer[256];

				snprintf(labelBuffer, sizeof(labelBuffer), "Total Time (avg %.2f ms).", m_crowdTotalTime.getAverage());
				ImGui::Text(labelBuffer);

				ImGui::SameLine();

				snprintf(labelBuffer, sizeof(labelBuffer), "Sample Count (avg %.2f).", m_crowdSampleCount.getAverage());
				ImGui::Text(labelBuffer);
			}
		}

		ImGui::End();
	}
}

void CrowdToolState::handleUpdate(const float dt)
{
	if (m_run)
		updateTick(dt);
}

static unsigned char getFilterTypeForTraverseType(const TraverseAnimType_e type)
{
	// +1 because the first type is reserved for ANIMTYPE_NONE, which is -1.
	return (unsigned char)type + 1;
}

void CrowdToolState::addAgent(const rdVec3D* p)
{
	if (!m_editor) return;
	dtCrowd* crowd = m_editor->getCrowd();
	
	dtCrowdAgentParams ap;
	memset(&ap, 0, sizeof(ap));
	ap.radius = m_editor->getAgentRadius();
	ap.height = m_editor->getAgentHeight();
	ap.maxAcceleration = m_toolParams.m_maxAcceleration;
	ap.maxSpeed = m_toolParams.m_maxSpeed;
	ap.collisionQueryRange = ap.radius * 50.0f;
	ap.pathOptimizationRange = ap.radius * 300.0f;
	ap.updateFlags = 0; 
	if (m_toolParams.m_anticipateTurns)
		ap.updateFlags |= DT_CROWD_ANTICIPATE_TURNS;
	if (m_toolParams.m_optimizeVis)
		ap.updateFlags |= DT_CROWD_OPTIMIZE_VIS;
	if (m_toolParams.m_optimizeTopo)
		ap.updateFlags |= DT_CROWD_OPTIMIZE_TOPO;
	if (m_toolParams.m_obstacleAvoidance)
		ap.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
	if (m_toolParams.m_separation)
		ap.updateFlags |= DT_CROWD_SEPARATION;
	ap.obstacleAvoidanceType = (unsigned char)m_toolParams.m_obstacleAvoidanceType;
	ap.separationWeight = m_toolParams.m_separationWeight;
	ap.traverseAnimType = m_toolParams.m_traverseAnimType;
	ap.queryFilterType = getFilterTypeForTraverseType(m_toolParams.m_traverseAnimType);
	
	int idx = crowd->addAgent(p, &ap);
	if (idx != -1)
	{
		if (m_targetRef)
			crowd->requestMoveTarget(idx, m_targetRef, &m_targetPos);
		
		// Init trail
		AgentTrail* trail = &m_trails[idx];
		for (int i = 0; i < AGENT_MAX_TRAIL; ++i)
			rdVcopy(&trail->trail[i], p);
		trail->htrail = 0;
	}
}

void CrowdToolState::removeAgent(const int idx)
{
	if (!m_editor) return;
	dtCrowd* crowd = m_editor->getCrowd();

	crowd->removeAgent(idx);
	
	if (idx == m_agentDebug.idx)
		m_agentDebug.idx = -1;
}

void CrowdToolState::hilightAgent(const int idx)
{
	m_agentDebug.idx = idx;
}

static void calcVel(rdVec3D* vel, const rdVec3D* pos, const rdVec3D* tgt, const float speed)
{
	rdVsub(vel, tgt, pos);
	vel->z = 0.0;
	rdVnormalize(vel);
	rdVscale(vel, vel, speed);
}

void CrowdToolState::setMoveTarget(const rdVec3D* p, bool adjust)
{
	if (!m_editor) return;
	
	// Find nearest point on navmesh and set move request to that location.
	dtNavMeshQuery* navquery = m_editor->getNavMeshQuery();
	dtCrowd* crowd = m_editor->getCrowd();
	const dtQueryFilter* filter = crowd->getFilter(0);
	const rdVec3D* halfExtents = crowd->getQueryExtents();

	if (adjust)
	{
		rdVec3D vel;
		// Request velocity
		if (m_agentDebug.idx != -1)
		{
			const dtCrowdAgent* ag = crowd->getAgent(m_agentDebug.idx);
			if (ag && ag->active)
			{
				calcVel(&vel, &ag->npos, p, ag->params.maxSpeed);
				crowd->requestMoveVelocity(m_agentDebug.idx, &vel);
			}
		}
		else
		{
			for (int i = 0; i < crowd->getAgentCount(); ++i)
			{
				const dtCrowdAgent* ag = crowd->getAgent(i);
				if (!ag->active) continue;
				calcVel(&vel, &ag->npos, p, ag->params.maxSpeed);
				crowd->requestMoveVelocity(i, &vel);
			}
		}
	}
	else
	{
		navquery->findNearestPoly(p, halfExtents, filter, &m_targetRef, &m_targetPos);
		
		if (m_agentDebug.idx != -1)
		{
			const dtCrowdAgent* ag = crowd->getAgent(m_agentDebug.idx);
			if (ag && ag->active)
				crowd->requestMoveTarget(m_agentDebug.idx, m_targetRef, &m_targetPos);
		}
		else
		{
			for (int i = 0; i < crowd->getAgentCount(); ++i)
			{
				const dtCrowdAgent* ag = crowd->getAgent(i);
				if (!ag->active) continue;
				crowd->requestMoveTarget(i, m_targetRef, &m_targetPos);
			}
		}
	}
}

int CrowdToolState::hitTestAgents(const rdVec3D* s, const rdVec3D* p)
{
	if (!m_editor) return -1;
	dtCrowd* crowd = m_editor->getCrowd();
	
	int isel = -1;
	float tsel = 1;

	for (int i = 0; i < crowd->getAgentCount(); ++i)
	{
		const dtCrowdAgent* ag = crowd->getAgent(i);
		if (!ag->active) continue;
		rdVec3D bmin, bmax;
		getAgentBounds(ag, &bmin, &bmax);
		float tmin, tmax;
		if (rdIntersectSegmentAABB(s, p, &bmin,&bmax, tmin, tmax))
		{
			if (tmin > 0 && tmin < tsel)
			{
				isel = i;
				tsel = tmin;
			} 
		}
	}

	return isel;
}

void CrowdToolState::updateAgentParams()
{
	if (!m_editor) return;
	dtCrowd* crowd = m_editor->getCrowd();
	if (!crowd) return;
	
	unsigned char updateFlags = 0;
	unsigned char obstacleAvoidanceType = 0;
	
	if (m_toolParams.m_anticipateTurns)
		updateFlags |= DT_CROWD_ANTICIPATE_TURNS;
	if (m_toolParams.m_optimizeVis)
		updateFlags |= DT_CROWD_OPTIMIZE_VIS;
	if (m_toolParams.m_optimizeTopo)
		updateFlags |= DT_CROWD_OPTIMIZE_TOPO;
	if (m_toolParams.m_obstacleAvoidance)
		updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
	if (m_toolParams.m_obstacleAvoidance)
		updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
	if (m_toolParams.m_separation)
		updateFlags |= DT_CROWD_SEPARATION;
	
	obstacleAvoidanceType = (unsigned char)m_toolParams.m_obstacleAvoidanceType;
	
	dtCrowdAgentParams params;
	
	for (int i = 0; i < crowd->getAgentCount(); ++i)
	{
		const dtCrowdAgent* ag = crowd->getAgent(i);
		if (!ag->active) continue;
		memcpy(&params, &ag->params, sizeof(dtCrowdAgentParams));
		params.updateFlags = updateFlags;
		params.obstacleAvoidanceType = obstacleAvoidanceType;
		params.separationWeight = m_toolParams.m_separationWeight;
		params.maxAcceleration = m_toolParams.m_maxAcceleration;
		params.maxSpeed = m_toolParams.m_maxSpeed;

		// Retain the traverse anim type and query filter type.
		params.traverseAnimType = ag->params.traverseAnimType;
		params.queryFilterType = ag->params.queryFilterType;

		crowd->updateAgentParameters(i, &params);
	}
}

void CrowdToolState::updateTick(const float dt)
{
	if (!m_editor) return;
	dtNavMesh* nav = m_editor->getNavMesh();
	dtCrowd* crowd = m_editor->getCrowd();
	if (!nav || !crowd) return;
	
	rdTimeType startTime = getPerfTime();
	
	crowd->update(dt, &m_agentDebug);
	
	rdTimeType endTime = getPerfTime();
	
	// Update agent trails
	for (int i = 0; i < crowd->getAgentCount(); ++i)
	{
		const dtCrowdAgent* ag = crowd->getAgent(i);
		AgentTrail* trail = &m_trails[i];
		if (!ag->active)
			continue;
		// Update agent movement trail.
		trail->htrail = (trail->htrail + 1) % AGENT_MAX_TRAIL;
		rdVcopy(&trail->trail[trail->htrail], &ag->npos);
	}
	
	m_agentDebug.vod->normalizeSamples();
	

	m_graphSampleTime += ImGui::GetIO().DeltaTime;

	m_crowdSampleCount.addSample(m_graphSampleTime, (float)crowd->getVelocitySampleCount());
	m_crowdTotalTime.addSample(m_graphSampleTime, getPerfTimeUsec(endTime - startTime) / 1000.0f);
}




CrowdTool::CrowdTool() :
	m_editor(0),
	m_state(0),
	m_mode(TOOLMODE_CREATE)
{
}

void CrowdTool::init(Editor* editor)
{
	if (m_editor != editor)
	{
		m_editor = editor;
	}
	
	if (!editor)
		return;
		
	m_state = (CrowdToolState*)editor->getToolState(type());
	if (!m_state)
	{
		m_state = new CrowdToolState();
		editor->setToolState(type(), m_state);
	}
	m_state->init(editor);
}

void CrowdTool::reset()
{	
}

void CrowdTool::handleMenu()
{
	if (!m_state)
		return;
	CrowdToolParams* params = m_state->getToolParams();

	bool isEnabled = m_mode == TOOLMODE_CREATE;

	if (ImGui::Checkbox("Create Agents", &isEnabled))
		m_mode = TOOLMODE_CREATE;

	isEnabled = m_mode == TOOLMODE_MOVE_TARGET;

	if (ImGui::Checkbox("Move Target", &isEnabled))
		m_mode = TOOLMODE_MOVE_TARGET;

	isEnabled = m_mode == TOOLMODE_SELECT;

	if (ImGui::Checkbox("Select Agent", &isEnabled))
		m_mode = TOOLMODE_SELECT;

	isEnabled = m_mode == TOOLMODE_TOGGLE_POLYS;

	if (ImGui::Checkbox("Toggle Polys", &isEnabled))
		m_mode = TOOLMODE_TOGGLE_POLYS;
	
	ImGui::Separator();
	
	if (ImGui::CollapsingHeader("Options"))
	{
		if (ImGui::Checkbox("Optimize Visibility", &params->m_optimizeVis))
			m_state->updateAgentParams();

		if (ImGui::Checkbox("Optimize Topology", &params->m_optimizeTopo))
			m_state->updateAgentParams();

		if (ImGui::Checkbox("Anticipate Turns", &params->m_anticipateTurns))
			m_state->updateAgentParams();

		if (ImGui::Checkbox("Obstacle Avoidance", &params->m_obstacleAvoidance))
			m_state->updateAgentParams();

		ImGui::PushItemWidth(120.f);
		if (ImGui::SliderInt("Avoidance Quality", &params->m_obstacleAvoidanceType, 0, 3))
		{
			m_state->updateAgentParams();
		}
		if (ImGui::Checkbox("Separation", &params->m_separation))
			m_state->updateAgentParams();

		if (ImGui::SliderFloat("Separation Weight", &params->m_separationWeight, 0.0f, 200.0f))
		{
			m_state->updateAgentParams();
		}
		if (ImGui::SliderFloat("Max Acceleration", &params->m_maxAcceleration, 0.0f, 2000.0f))
		{
			m_state->updateAgentParams();
		}
		if (ImGui::SliderFloat("Max Speed", &params->m_maxSpeed, 0.0f, 2000.0f))
		{
			m_state->updateAgentParams();
		}
		ImGui::PopItemWidth();
	}

	if (ImGui::CollapsingHeader("Traverse Animation Type"))
	{
		const NavMeshType_e loadedNavMeshType = m_editor->getLoadedNavMeshType();

		// Clamp to the mesh's own table count so a sparse mesh cannot index past its array.
		int traverseTableCount = NavMesh_GetTraverseTableCountForNavMeshType(loadedNavMeshType);
		if (const dtNavMesh* nav = m_editor->getNavMesh())
			traverseTableCount = rdMin(traverseTableCount, nav->getParams()->traverseTableCount);
		const TraverseAnimType_e baseType = NavMesh_GetFirstTraverseAnimTypeForType(loadedNavMeshType);

		for (int i = ANIMTYPE_NONE; i < traverseTableCount; i++)
		{
			const bool noAnimtype = i == ANIMTYPE_NONE;

			const TraverseAnimType_e animTypeIndex = noAnimtype ? ANIMTYPE_NONE : TraverseAnimType_e((int)baseType + i);
			const char* animtypeName = noAnimtype ? "none" : g_traverseAnimTypeNames[animTypeIndex];

			bool isAnimTypeEnabled = params->m_traverseAnimType == animTypeIndex;

			if (ImGui::Checkbox(animtypeName, &isAnimTypeEnabled))
			{
				params->m_traverseAnimType = animTypeIndex;
				m_state->updateAgentParams();
			}
		}
	}

	if (ImGui::CollapsingHeader("Selected Debug Draw"))
	{
		ImGui::Checkbox("Show Corners", &params->m_showCorners);
		ImGui::Checkbox("Show Collision Segs", &params->m_showCollisionSegments);
		ImGui::Checkbox("Show Path", &params->m_showPath);
		ImGui::Checkbox("Show VO", &params->m_showVO);
		ImGui::Checkbox("Show Path Optimization", &params->m_showOpt);
		ImGui::Checkbox("Show Neighbours", &params->m_showNeis);
	}
		
	if (ImGui::CollapsingHeader("Debug Draw"))
	{
		ImGui::Checkbox("Show Labels", &params->m_showLabels);
		ImGui::Checkbox("Show Prox Grid", &params->m_showGrid);
		ImGui::Checkbox("Show Nodes", &params->m_showNodes);
		ImGui::Checkbox("Show Perf Graph", &params->m_showPerfGraph);
		ImGui::Checkbox("Show Details For All", &params->m_showDetailAll);
	}
}

void CrowdTool::handleClick(const rdVec3D* s, const rdVec3D* p, const int /*v*/, bool shift)
{
	if (!m_editor) return;
	if (!m_state) return;
	InputGeom* geom = m_editor->getInputGeom();
	if (!geom) return;
	dtCrowd* crowd = m_editor->getCrowd();
	if (!crowd) return;

	if (m_mode == TOOLMODE_CREATE)
	{
		if (shift)
		{
			// Delete
			int ahit = m_state->hitTestAgents(s,p);
			if (ahit != -1)
				m_state->removeAgent(ahit);
		}
		else
		{
			// Add
			m_state->addAgent(p);
		}
	}
	else if (m_mode == TOOLMODE_MOVE_TARGET)
	{
		m_state->setMoveTarget(p, shift);
	}
	else if (m_mode == TOOLMODE_SELECT)
	{
		// Highlight
		int ahit = m_state->hitTestAgents(s,p);
		m_state->hilightAgent(ahit);
	}
	else if (m_mode == TOOLMODE_TOGGLE_POLYS)
	{
		dtNavMesh* nav = m_editor->getNavMesh();
		dtNavMeshQuery* navquery = m_editor->getNavMeshQuery();
		if (nav && navquery)
		{
			dtQueryFilter filter;
			const rdVec3D* halfExtents = crowd->getQueryExtents();
			rdVec3D tgt;
			dtPolyRef ref;
			navquery->findNearestPoly(p, halfExtents, &filter, &ref, &tgt);
			if (ref)
			{
				unsigned short flags = 0;
				if (dtStatusSucceed(nav->getPolyFlags(ref, &flags)))
				{
					flags ^= DT_POLYFLAGS_DISABLED;
					nav->setPolyFlags(ref, flags);
				}
			}
		}
	}
}

void CrowdTool::handleStep()
{
	if (!m_state) return;
	
	const float dt = 1.0f/20.0f;
	m_state->updateTick(dt);

	m_state->setRunning(false);
}

void CrowdTool::handleToggle()
{
	if (!m_state) return;
	m_state->setRunning(!m_state->isRunning());
}

void CrowdTool::handleUpdate(const float dt)
{
	rdIgnoreUnused(dt);
}

void CrowdTool::handleRender()
{
}

void CrowdTool::handleRenderOverlay(double* model, double* proj, int* view)
{
	rdIgnoreUnused(model);
	rdIgnoreUnused(proj);
	rdIgnoreUnused(view);

	// Tool help
	float ty = 40;
	
	if (m_mode == TOOLMODE_CREATE)
	{
		ImGui_RenderText(ImGuiTextAlign_e::kAlignLeft,
			ImVec2(300, ty), ImVec4(1.0f,1.0f,1.0f,0.75f), "LMB: add agent.  Shift+LMB: remove agent.");
	}
	else if (m_mode == TOOLMODE_MOVE_TARGET)
	{
		ImGui_RenderText(ImGuiTextAlign_e::kAlignLeft,
			ImVec2(300, ty), ImVec4(1.0f,1.0f,1.0f,0.75f), "LMB: set move target.  Shift+LMB: adjust set velocity.");

		ty += 20;
		ImGui_RenderText(ImGuiTextAlign_e::kAlignLeft,
			ImVec2(300, ty), ImVec4(1.0f,1.0f,1.0f,0.75f), "Setting velocity will move the agents without pathfinder.");
	}
	else if (m_mode == TOOLMODE_SELECT)
	{
		ImGui_RenderText(ImGuiTextAlign_e::kAlignLeft,
			ImVec2(300, ty), ImVec4(1.0f,1.0f,1.0f,0.75f), "LMB: select agent.");
	}

	ty += 20.f;
	ImGui_RenderText(ImGuiTextAlign_e::kAlignLeft,
		ImVec2(300, ty), ImVec4(1.0f,1.0f,1.0f,0.75f), "SPACE: Run/Pause simulation.  1: Step simulation.");

	ty += 20.f;
	if (m_state && m_state->isRunning())
	{
		ImGui_RenderText(ImGuiTextAlign_e::kAlignLeft,
			ImVec2(300, ty), ImVec4(0.15f,1.0f,0.05f,0.8f), "- RUNNING -");
	}
	else
	{
		ImGui_RenderText(ImGuiTextAlign_e::kAlignLeft,
			ImVec2(300, ty), ImVec4(1.0f,0.15f,0.05f,0.8f), "- PAUSED -");
	}
}
