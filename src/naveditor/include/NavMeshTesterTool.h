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

#ifndef NAVMESHTESTERTOOL_H
#define NAVMESHTESTERTOOL_H

#include "Detour/Include/DetourNavMesh.h"
#include "Detour/Include/DetourNavMeshQuery.h"
#include "NavEditor/Include/Editor.h"

#include "game/server/ai_navmesh.h"

class NavMeshTesterTool : public EditorTool
{
	Editor* m_editor;
	
	dtNavMesh* m_navMesh;
	dtNavMeshQuery* m_navQuery;

	dtQueryFilter m_filter;

	dtStatus m_pathFindStatus;

	enum ToolMode
	{
		TOOLMODE_PATHFIND_FOLLOW,
		TOOLMODE_PATHFIND_STRAIGHT,
		TOOLMODE_PATHFIND_SLICED,
		TOOLMODE_RAYCAST,
		TOOLMODE_DISTANCE_TO_WALL,
		TOOLMODE_FIND_POLYS_IN_CIRCLE,
		TOOLMODE_FIND_POLYS_IN_SHAPE,
		TOOLMODE_FIND_LOCAL_NEIGHBOURHOOD,
	};
	
	ToolMode m_toolMode;
	TraverseAnimType_e m_traverseAnimType;

	int m_straightPathOptions;
	
	static const int MAX_POLYS = 512;
	static const int MAX_SMOOTH = 2048;
	
	dtPolyRef m_startRef;
	dtPolyRef m_endRef;
	dtPolyRef m_polys[MAX_POLYS];
	dtPolyRef m_parent[MAX_POLYS];
	unsigned char m_jumps[MAX_POLYS];
	int m_npolys;
	rdVec3D m_straightPath[MAX_POLYS];
	unsigned char m_straightPathFlags[MAX_POLYS];
	dtPolyRef m_straightPathPolys[MAX_POLYS];
	unsigned char m_straightPathJumps[MAX_POLYS];
	int m_nstraightPath;
	rdVec3D m_polyPickExt;
	rdVec3D m_smoothPath[MAX_SMOOTH];
	int m_nsmoothPath;
	rdVec3D m_queryPoly[4];

	static const int MAX_RAND_POINTS = 64;
	rdVec3D m_randPoints[MAX_RAND_POINTS];
	int m_nrandPoints;
	bool m_randPointsInCircle;
	
	rdVec3D m_spos;
	rdVec3D m_epos;
	rdVec3D m_hitPos;
	rdVec3D m_hitNormal;
	bool m_hitResult;
	float m_distanceToWall;
	float m_neighbourhoodRadius;
	float m_randomRadius;
	bool m_sposSet;
	bool m_eposSet;

	int m_pathIterNum;
	dtPolyRef m_pathIterPolys[MAX_POLYS];
	unsigned char m_pathIterJumps[MAX_POLYS];
	int m_pathIterPolyCount;
	rdVec3D m_prevIterPos, m_iterPos, m_steerPos, m_targetPos;
	
	static const int MAX_STEER_POINTS = 10;
	rdVec3D m_steerPoints[MAX_STEER_POINTS];
	int m_steerPointCount;
	
public:
	NavMeshTesterTool();

	virtual int type() { return TOOL_NAVMESH_TESTER; }
	virtual void init(Editor* editor);
	virtual void reset();
	virtual void handleMenu();
	virtual void handleClick(const rdVec3D* s, const rdVec3D* p, const int v, bool shift);
	virtual void handleToggle();
	virtual void handleStep();
	virtual void handleUpdate(const float dt);
	virtual void handleRender();
	virtual void handleRenderOverlay(double* model, double* proj, int* view);

	void handleStraightPathMenu();
	void recalc();
	void updateTraverseCosts();

	void drawAgent(const rdVec3D* pos, float r, float h, float c, const unsigned int col);
};

#endif // NAVMESHTESTERTOOL_H
