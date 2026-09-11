#pragma once
#include "thirdparty/recast/Detour/Include/DetourStatus.h"
#include "thirdparty/recast/Detour/Include/DetourNavMesh.h"
#include "thirdparty/recast/Detour/Include/DetourNode.h"
#include "thirdparty/recast/Detour/Include/DetourNavMeshQuery.h"
#include "game/server/ai_navmesh.h"
#include "game/server/detour_layout_asserts.h"

//-------------------------------------------------------------------------
// RUNTIME: DETOUR
//-------------------------------------------------------------------------
inline void(*v_Detour_LevelInit)(void);
inline bool(*v_Detour_IsGoalPolyReachable)(dtNavMesh* const nav, const dtPolyRef fromPoly, const dtPolyRef goalPoly, const TraverseAnimType_e animType);
inline dtStatus(*dtNavMesh__Init)(dtNavMesh* thisptr, unsigned char* data, int flags);
inline dtStatus(*dtNavMesh__addTile)(dtNavMesh* thisptr, void* unused, unsigned char* data, int dataSize, int flags, dtTileRef lastRef);

inline dtStatus(*dtNavMeshQuery__findNearestPoly)(dtNavMeshQuery* query, const rdVec3D* center, const rdVec3D* halfExtents,
	const dtQueryFilter* filter, dtPolyRef* nearestRef, rdVec3D* nearestPt);

inline dtStatus(*dtNavMeshQuery__findPath)(dtNavMeshQuery* query, dtPolyRef startRef, dtPolyRef endRef,
	const rdVec3D* startPos, const rdVec3D* endPos, const dtQueryFilter* filter, dtPolyRef* path, unsigned char* jump, int* pathCount, const int maxPath);

inline dtStatus(*dtNavMeshQuery__findStraightPath)(dtNavMeshQuery* query, const rdVec3D* startPos, const rdVec3D* endPos,
	const dtPolyRef* path, const unsigned char* jumpTypes, const int pathSize, rdVec3D* straightPath, unsigned char* straightPathFlags,
	dtPolyRef* straightPathRefs, unsigned char* straightPathJumps, int* straightPathCount, const int unused, const int jumpFilter, const int options);

inline dtStatus(*dtNavMeshQuery__moveAlongSurface)(dtNavMeshQuery* const query, dtPolyRef startRef, const rdVec3D* startPos,
	const rdVec3D* endPos, const dtQueryFilter* filter, rdVec3D* resultPos, dtPolyRef* visitedPolys,
	int* visitedCount, const int maxVisitedSize, const unsigned char options);

inline dtStatus(*dtNavMeshQuery__raycast)(dtNavMeshQuery* query, const dtPolyRef startRef, const rdVec3D* startPos, const rdVec3D* endPos,
	const dtQueryFilter* filter, dtRaycastHit* hit);

inline bool(*dtNavMeshQuery__closestPointOnPoly)(dtNavMeshQuery* query, const dtPolyRef ref, const rdVec3D* pos, rdVec3D* closest, bool* posOverPoly, float* dist);
inline dtStatus(*dtNavMeshQuery__closestPointOnPolyBoundary)(dtNavMeshQuery* query, const dtPolyRef ref, const rdVec3D* pos, rdVec3D* closest, float* dist);
inline dtStatus(*dtNavMeshQuery__getPolyHeight)(dtNavMeshQuery* query, const dtPolyRef ref, const rdVec3D* pos, float* height, rdVec3D* normal);

inline dtStatus(*dtNavMeshQuery__queryPolygonsSmallArea)(dtNavMeshQuery* query, const rdVec3D* center, const rdVec3D* halfExtents,
	const dtQueryFilter* filter, dtPolyRef* polys, int* polyCount);
inline dtStatus(*dtNavMeshQuery__queryPolygonsLargeArea)(dtNavMeshQuery* query, const rdVec3D* center, const rdVec3D* halfExtents,
	const dtQueryFilter* filter, dtPolyRef* polys, int* polyCount);

inline dtNode*(*dtNodePool__getPool)(dtNodePool* pool, const dtPolyRef id, const unsigned char state);

constexpr const char* NAVMESH_PATH = "maps/navmesh/";
constexpr const char* NAVMESH_EXT = ".nm";

inline dtNavMesh** g_navMeshArray = nullptr; // size = NavMeshType_e::NAVMESH_COUNT.
inline dtNavMeshQuery* g_navMeshQuery = nullptr;

dtNavMesh* Detour_GetNavMeshByType(const NavMeshType_e navMeshType);

void Detour_LevelInit();
void Detour_LevelShutdown();
bool Detour_IsLoaded();
void Detour_HotSwap();
///////////////////////////////////////////////////////////////////////////////
class VRecast : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Detour_LevelInit", v_Detour_LevelInit);
		LogFunAdr("Detour_IsGoalPolyReachable", v_Detour_IsGoalPolyReachable);

		LogFunAdr("dtNavMesh::Init", dtNavMesh__Init);
		LogFunAdr("dtNavMesh::addTile", dtNavMesh__addTile);

		LogFunAdr("dtNavMeshQuery::findNearestPolyInBounds", dtNavMeshQuery__findNearestPoly);
		LogFunAdr("dtNavMeshQuery::findPath", dtNavMeshQuery__findPath);
		LogFunAdr("dtNavMeshQuery::findStraightPath", dtNavMeshQuery__findStraightPath);

		LogFunAdr("dtNavMeshQuery::moveAlongSurface", dtNavMeshQuery__moveAlongSurface);
		LogFunAdr("dtNavMeshQuery::raycast", dtNavMeshQuery__raycast);

		LogFunAdr("dtNavMeshQuery::closestPointOnPoly", dtNavMeshQuery__closestPointOnPoly);
		LogFunAdr("dtNavMeshQuery::closestPointOnPolyBoundary", dtNavMeshQuery__closestPointOnPolyBoundary);
		LogFunAdr("dtNavMeshQuery::getPolyHeight", dtNavMeshQuery__getPolyHeight);

		LogFunAdr("dtNavMeshQuery::queryPolygonsSmallArea", dtNavMeshQuery__queryPolygonsSmallArea);
		LogFunAdr("dtNavMeshQuery::queryPolygonsLargeArea", dtNavMeshQuery__queryPolygonsLargeArea);

		LogFunAdr("dtNodePool::getPool", dtNodePool__getPool);

		LogVarAdr("g_navMeshArray[ NavMeshType_e::NAVMESH_COUNT ]", g_navMeshArray);
		LogVarAdr("g_navMeshQuery", g_navMeshQuery);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 45 33 E4").GetPtr(v_Detour_LevelInit);
		Module_FindPattern(g_GameDll, "48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 49 63 F1").GetPtr(v_Detour_IsGoalPolyReachable);
		Module_FindPattern(g_GameDll, "4C 89 44 24 ?? 53 41 56 48 81 EC ?? ?? ?? ?? 0F 10 11").GetPtr(dtNavMesh__Init);
		Module_FindPattern(g_GameDll, "44 89 4C 24 ?? 41 55").GetPtr(dtNavMesh__addTile);
		Module_FindPattern(g_GameDll, "4C 8B DC 49 89 4B ?? 41 54 41 57").GetPtr(dtNavMeshQuery__findNearestPoly);
		Module_FindPattern(g_GameDll, "44 89 44 24 ?? 56 57 41 56 41 57").GetPtr(dtNavMeshQuery__findPath);
		Module_FindPattern(g_GameDll, "4C 89 44 24 ?? 48 89 4C 24 ?? 55 53 56 57 41 54 41 55 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 4C 8B BD").GetPtr(dtNavMeshQuery__findStraightPath);
		Module_FindPattern(g_GameDll, "4C 89 4C 24 ?? 48 89 4C 24 ?? 55 53 56 57").GetPtr(dtNavMeshQuery__moveAlongSurface);
		Module_FindPattern(g_GameDll, "4C 89 4C 24 ?? 4C 89 44 24 ?? 48 89 4C 24 ?? 53 55 57").GetPtr(dtNavMeshQuery__raycast);
		Module_FindPattern(g_GameDll, "4C 89 44 24 ?? 53 55 56 57 41 55 48 81 EC").GetPtr(dtNavMeshQuery__closestPointOnPoly);
		Module_FindPattern(g_GameDll, "40 53 57 41 55 41 56 41 57").GetPtr(dtNavMeshQuery__closestPointOnPolyBoundary);
		Module_FindPattern(g_GameDll, "4C 89 4C 24 ?? 4C 89 44 24 ?? 53 56 57 41 56").GetPtr(dtNavMeshQuery__getPolyHeight);
		Module_FindPattern(g_GameDll, "4C 8B DC 4D 89 4B ?? 53").GetPtr(dtNavMeshQuery__queryPolygonsSmallArea);
		Module_FindPattern(g_GameDll, "48 8B C4 4C 89 48 ?? 48 89 48 ?? 53 55 56").GetPtr(dtNavMeshQuery__queryPolygonsLargeArea);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 44 8B CA").GetPtr(dtNodePool__getPool);
	}
	virtual void GetVar(void) const
	{
		g_navMeshArray = Module_FindPattern(g_GameDll, "48 89 54 24 ?? 48 89 4C 24 ?? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 02")
			.FindPatternSelf("48 8D 3D").ResolveRelativeAddressSelf(0x3, 0x7).RCast<dtNavMesh**>();
		g_navMeshQuery = Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 81 EC ?? ?? ?? ?? 48 63 D9")
			.FindPatternSelf("48 89 0D").ResolveRelativeAddressSelf(0x3, 0x7).RCast<dtNavMeshQuery*>();
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
