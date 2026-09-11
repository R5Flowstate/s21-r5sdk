//=============================================================================//
//
// Purpose: out-of-bounds clip volumes recovered from a map's entity partition.
//
// A dedicated-server BSP zeroes the brush-model AABBs in LUMP_MODELS, so the
// only surviving description of a trigger brush is the base64 `*coll0..N`
// collision blob on the entity itself.
//
//=============================================================================//
#ifndef ENTITYVOLUMES_H
#define ENTITYVOLUMES_H

#include <string>
#include <vector>

#include "Shared/Include/SharedCommon.h"

// Matches MAX_SHAPEVOL_PTS in InputGeom.h; a volume that needs more points
// than this is dropped rather than silently truncated into a wrong shape.
static const int MAX_ENTITYVOL_PTS = 12;

// One convex clip volume: a 2D polygon extruded between hmin and hmax.
struct EntityClipVolume
{
	rdVec3D verts[MAX_ENTITYVOL_PTS];
	int nverts = 0;
	float hmin = 0.0f;
	float hmax = 0.0f;
};

// `<dir>/<map>.bsp` -> `<dir>/<map>_script.ent`, or empty if it is not there.
std::string rcFindScriptEntPath(const std::string& bspPath);

// Appends every `trigger_out_of_bounds` brush in the partition, placed in world
// space. Returns false only when the file cannot be read.
bool rcLoadEntityClipVolumes(const std::string& entPath, std::vector<EntityClipVolume>& out);

#endif // ENTITYVOLUMES_H
