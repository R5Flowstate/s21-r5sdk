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

#ifndef INPUTGEOM_H
#define INPUTGEOM_H

#include "NavEditor/Include/ChunkyTriMesh.h"
#include "NavEditor/Include/MeshLoaderObj.h"

enum VolumeType : unsigned char
{
	VOLUME_INVALID = 0xff,
	VOLUME_BOX = 0,
	VOLUME_CYLINDER,
	VOLUME_CONVEX
};

enum TraceMask : unsigned int
{
	TRACE_WORLD   = 1<<0, // The imported world geometry.
	TRACE_CLIP    = 1<<1, // Clip brushes.
	TRACE_TRIGGER = 1<<2, // Trigger brushes.
	TRACE_ALL = 0xffffffff
};

static const int MAX_SHAPEVOL_PTS = 12;
struct ShapeVolume
{
	ShapeVolume()
	{
		for (int i = 0; i < MAX_SHAPEVOL_PTS; i++)
		{
			verts[i].init(0.f,0.f,0.f);
		}
		hmin = 0.f;
		hmax = 0.f;
		nverts = 0;
		flags = 0;
		area = 0;
		type = VOLUME_INVALID;
	}

	rdVec3D verts[MAX_SHAPEVOL_PTS];
	float hmin, hmax;
	int nverts;
	unsigned short flags;
	unsigned char area;
	unsigned char type;
};

struct BuildSettings
{
	// Cell size in world units
	float cellSize;
	// Cell height in world units
	float cellHeight;
	// Agent height in world units
	float agentHeight;
	// Agent radius in world units
	float agentRadius;
	// Agent max climb in world units
	float agentMaxClimb;
	// Agent max slope in degrees
	float agentMaxSlope;
	// Region minimum size in voxels.
	// regionMinSize = sqrt(regionMinArea)
	int regionMinSize;
	// Region merge size in voxels.
	// regionMergeSize = sqrt(regionMergeArea)
	int regionMergeSize;
	// Edge max length in world units
	int edgeMaxLen;
	// Edge max error in voxels
	float edgeMaxError;
	int vertsPerPoly;
	// The polygon cell resolution.
	int polyCellRes;
	// Detail sample distance in voxels
	float detailSampleDist;
	// Detail sample max error in voxel heights.
	float detailSampleMaxError;
	// Partition type, see SamplePartitionType
	int partitionType;
	// Bounds of the area to mesh
	rdVec3D navMeshBMin;
	rdVec3D navMeshBMax;
	// Original bounds of the area to mesh.
	rdVec3D origNavMeshBMin;
	rdVec3D origNavMeshBMax;
	// Size of the tiles in voxels
	int tileSize;
};

class InputGeom
{
	enum MeshFormat
	{
		MESH_OBJ,
		MESH_PLY,
		MESH_BSP
	};

	rcChunkyTriMesh* m_chunkyMesh;
	IMeshLoader* m_mesh;
	rdVec3D m_meshBMin, m_meshBMax;
	rdVec3D m_navMeshBMin, m_navMeshBMax;
	BuildSettings m_buildSettings;
	bool m_hasBuildSettings;
	
	/// @name Off-Mesh connections.
	///@{
	static const int MAX_OFFMESH_CONNECTIONS = 1024;
	rdVec3D m_offMeshConVerts[MAX_OFFMESH_CONNECTIONS*2];
	rdVec3D m_offMeshConRefPos[MAX_OFFMESH_CONNECTIONS];
	float m_offMeshConRads[MAX_OFFMESH_CONNECTIONS];
	float m_offMeshConRefYaws[MAX_OFFMESH_CONNECTIONS];
	unsigned char m_offMeshConDirs[MAX_OFFMESH_CONNECTIONS];
	unsigned char m_offMeshConJumps[MAX_OFFMESH_CONNECTIONS];
	unsigned char m_offMeshConOrders[MAX_OFFMESH_CONNECTIONS];
	unsigned char m_offMeshConAreas[MAX_OFFMESH_CONNECTIONS];
	unsigned short m_offMeshConFlags[MAX_OFFMESH_CONNECTIONS];
	unsigned short m_offMeshConId[MAX_OFFMESH_CONNECTIONS];
	short m_offMeshConCount;
	///@}

	/// @name Convex Volumes.
	///@{
	// A dense map's out-of-bounds trigger set alone can exceed a few hundred:
	// mp_rr_district carries 508 trigger_out_of_bounds entities decoding to 512
	// volumes, and at 256 half of them were dropped and the bake kept navmesh
	// inside them. ShapeVolume is 160 bytes, so the headroom is cheap.
	static const int MAX_VOLUMES = 2048;
	ShapeVolume m_volumes[MAX_VOLUMES];
	int m_volumeCount;
	///@}
	
	bool loadMesh(class rcContext* ctx, const std::string& filepath, const MeshFormat format);
	bool loadGeomSet(class rcContext* ctx, const std::string& filepath);
public:
	InputGeom();
	~InputGeom();
	
	
	bool load(class rcContext* ctx, const std::string& filepath);
	bool saveGeomSet(const BuildSettings* settings);
	
	/// Method to return static mesh data.
	const IMeshLoader* getMesh() const { return m_mesh; }
	const rdVec3D* getMeshBoundsMin() const { return &m_meshBMin; }
	const rdVec3D* getMeshBoundsMax() const { return &m_meshBMax; }

	rdVec3D* getNavMeshBoundsMin() { return m_hasBuildSettings ? &m_buildSettings.navMeshBMin : &m_navMeshBMin; }
	rdVec3D* getNavMeshBoundsMax() { return m_hasBuildSettings ? &m_buildSettings.navMeshBMax : &m_navMeshBMax; }

	const rdVec3D* getNavMeshBoundsMin() const { return m_hasBuildSettings ? &m_buildSettings.navMeshBMin : &m_navMeshBMin; }
	const rdVec3D* getNavMeshBoundsMax() const { return m_hasBuildSettings ? &m_buildSettings.navMeshBMax : &m_navMeshBMax; }

	const rdVec3D* getOriginalNavMeshBoundsMin() const { return m_hasBuildSettings ? &m_buildSettings.origNavMeshBMin : &m_meshBMin; }
	const rdVec3D* getOriginalNavMeshBoundsMax() const { return m_hasBuildSettings ? &m_buildSettings.origNavMeshBMax : &m_meshBMax; }

	const rcChunkyTriMesh* getChunkyMesh() const { return m_chunkyMesh; }
	const BuildSettings* getBuildSettings() const { return m_hasBuildSettings ? &m_buildSettings : 0; }
	bool raycastMesh(const rdVec3D* src, const rdVec3D* dst, const unsigned int mask, int* vol = nullptr, float* tmin = nullptr) const;

	/// @name Off-Mesh connections.
	///@{
	int getOffMeshConnectionCount() { return m_offMeshConCount; }
	rdVec3D* getOffMeshConnectionVerts() { return m_offMeshConVerts; }
	float* getOffMeshConnectionRads() { return m_offMeshConRads; }
	unsigned char* getOffMeshConnectionDirs() { return m_offMeshConDirs; }
	unsigned char* getOffMeshConnectionJumps() { return m_offMeshConJumps; }
	unsigned char* getOffMeshConnectionOrders() { return m_offMeshConOrders; }
	unsigned char* getOffMeshConnectionAreas() { return m_offMeshConAreas; }
	unsigned short* getOffMeshConnectionFlags() { return m_offMeshConFlags; }
	unsigned short* getOffMeshConnectionId() { return m_offMeshConId; }
	rdVec3D* getOffMeshConnectionRefPos() { return m_offMeshConRefPos; }
	float* getOffMeshConnectionRefYaws() { return m_offMeshConRefYaws; }
	int addOffMeshConnection(const rdVec3D* spos, const rdVec3D* epos, const float rad,
							  unsigned char bidir, unsigned char jump, unsigned char order, 
							  unsigned char area, unsigned short flags);
	void deleteOffMeshConnection(int i);
	void drawOffMeshConnections(struct duDebugDraw* dd, const rdVec3D* offset, const int hilightIdx = -1);
	///@}

	/// @name Shape Volumes.
	///@{
	int getShapeVolumeCount() const { return m_volumeCount; }
	ShapeVolume* getShapeVolumes() { return m_volumes; }
	int addBoxVolume(const rdVec3D* bmin, const rdVec3D* bmax,
						 unsigned short flags, unsigned char area);
	int addCylinderVolume(const rdVec3D* pos, const float radius,
						 const float height, unsigned short flags, unsigned char area);
	int addConvexVolume(const rdVec3D* verts, const int nverts,
						 const float minh, const float maxh, unsigned short flags, unsigned char area);
	int addClipVolumesFromEntityPartition(const std::string& entPath);
	// A .bsp load finds its own '_script.ent' unless this is turned off, which
	// the caller does when it supplies the partition itself or wants none.
	static void setAutoClipVolumes(bool enable);
	void deleteShapeVolume(int i);
	void drawBoxVolumes(struct duDebugDraw* dd, const rdVec3D* offset, const int hilightIdx = -1);
	void drawCylinderVolumes(struct duDebugDraw* dd, const rdVec3D* offset, const int hilightIdx = -1);
	void drawConvexVolumes(struct duDebugDraw* dd, const rdVec3D* offset, const int hilightIdx = -1);
	///@}
	
private:
	// Explicitly disabled copy constructor and copy assignment operator.
	InputGeom(const InputGeom&);
	InputGeom& operator=(const InputGeom&);
};

#endif // INPUTGEOM_H
