//=============================================================================//
//
// Purpose: AI system utilities
//
//=============================================================================//

#include "core/stdafx.h"
#include <cstdint>
#include "tier0/fasttimer.h"
#include "tier1/cvar.h"
#include "mathlib/bitvec.h"
#include "engine/server/server.h"
#include "public/edict.h"
#include "game/server/detour_impl.h"
#include "game/server/ai_networkmanager.h"
#include "game/shared/util_shared.h"

#include "vscript/languages/squirrel_re/vsquirrel.h"

#include "ai_basenpc.h"

static ConVar navmesh_always_reachable("navmesh_always_reachable", "0", FCVAR_DEVELOPMENTONLY, "Marks goal poly from agent poly as reachable by ignoring static pathing ( !slower! )");

//-----------------------------------------------------------------------------
// Purpose: gets the navmesh by type from global array [small, med_short, medium, large, extra_large]
// Input: navMeshType - 
// Output: pointer to navmesh
//-----------------------------------------------------------------------------
dtNavMesh* Detour_GetNavMeshByType(const NavMeshType_e navMeshType)
{
    Assert(navMeshType >= NULL && navMeshType < NAVMESH_COUNT); // Programmer error.
    return g_navMeshArray[navMeshType];
}

//-----------------------------------------------------------------------------
// Purpose: free's the navmesh by type from global array [small, med_short, medium, large, extra_large]
// Input: navMeshType - 
//-----------------------------------------------------------------------------
static void Detour_FreeNavMeshByType(const NavMeshType_e navMeshType)
{
    Assert(navMeshType >= NULL && navMeshType < NAVMESH_COUNT); // Programmer error.
    dtNavMesh* const nav = g_navMeshArray[navMeshType];

    if (nav) // Only free if NavMesh for type is loaded.
    {
        // Frees tiles, polys, tris, anything dynamically
        // allocated for this navmesh, and the navmesh itself.
        delete nav;
        g_navMeshArray[navMeshType] = nullptr;
    }
}

//-----------------------------------------------------------------------------
// Purpose: determines whether goal poly is reachable from agent poly
// (only checks static pathing)
// Input: *nav - 
// fromRef - 
// goalRef - 
// animType - 
// Output: value if reachable, false otherwise
//-----------------------------------------------------------------------------
static bool Detour_IsGoalPolyReachable(dtNavMesh* const nav, const dtPolyRef fromRef,
    const dtPolyRef goalRef, const TraverseAnimType_e animType)
{
    if (navmesh_always_reachable.GetBool())
        return true;

    const bool hasAnimType = animType != ANIMTYPE_NONE;
    const int traverseTableIndex = hasAnimType
        ? NavMesh_GetTraverseTableIndexForAnimType(animType)
        : NULL;

    return nav->isGoalPolyReachable(fromRef, goalRef, !hasAnimType, traverseTableIndex);
}

static bool Navmesh_Mul(const int64_t a, const int64_t b, int64_t* const out)
{
    if (!out || a < 0 || b < 0)
        return false;
    if (a != 0 && b > (INT64_MAX / a))
        return false;
    *out = a * b;
    return true;
}

static int64_t Navmesh_Align4(const int64_t x)
{
    return (x + 3) & ~int64_t(3);
}

static bool Navmesh_AddSection(int64_t* const required, const int64_t elem, const int64_t n)
{
    int64_t part = 0;
    if (!Navmesh_Mul(elem, n, &part))
        return false;
    part = Navmesh_Align4(part);
    if (*required > INT64_MAX - part)
        return false;
    *required += part;
    return true;
}

static bool Detour_TileDataFits(const unsigned char* const data, const int dataSize)
{
    if (!data || dataSize < static_cast<int>(sizeof(dtMeshHeader)))
        return false;
    if (dataSize > 4 * 1024 * 1024)
        return false;

    const dtMeshHeader* const header = reinterpret_cast<const dtMeshHeader*>(data);
    if (header->magic != DT_NAVMESH_MAGIC || header->version != DT_NAVMESH_VERSION)
        return false;

    if (header->polyCount < 0 || header->polyMapCount < 0 || header->vertCount < 0
        || header->maxLinkCount < 0 || header->detailMeshCount < 0
        || header->detailVertCount < 0 || header->detailTriCount < 0
        || header->bvNodeCount < 0 || header->offMeshConCount < 0
        || header->maxCellCount < 0)
        return false;

    int64_t mapWords = 0;
    if (!Navmesh_Mul(header->polyCount, header->polyMapCount, &mapWords))
        return false;

    int64_t required = 0;
    if (!Navmesh_AddSection(&required, sizeof(dtMeshHeader), 1)
        || !Navmesh_AddSection(&required, sizeof(rdVec3D), header->vertCount)
        || !Navmesh_AddSection(&required, sizeof(dtPoly), header->polyCount)
        || !Navmesh_AddSection(&required, sizeof(int), mapWords)
        || !Navmesh_AddSection(&required, sizeof(dtLink), header->maxLinkCount)
        || !Navmesh_AddSection(&required, sizeof(dtPolyDetail), header->detailMeshCount)
        || !Navmesh_AddSection(&required, sizeof(rdVec3D), header->detailVertCount)
        || !Navmesh_AddSection(&required, sizeof(unsigned char) * 4, header->detailTriCount)
        || !Navmesh_AddSection(&required, sizeof(dtBVNode), header->bvNodeCount)
        || !Navmesh_AddSection(&required, sizeof(dtOffMeshConnection), header->offMeshConCount)
        || !Navmesh_AddSection(&required, sizeof(dtCell), header->maxCellCount))
        return false;

    return required <= dataSize;
}

//-----------------------------------------------------------------------------
// Purpose: adds a tile to the NavMesh.
// Output: the status flags for the operation.
//-----------------------------------------------------------------------------
static dtStatus Detour_AddTile(dtNavMesh* nav, void* unused, unsigned char* data,
    int dataSize, int flags, dtTileRef lastRef)
{
    if (!nav || !Detour_TileDataFits(data, dataSize))
    {
        int x = 0;
        int y = 0;
        int layer = 0;
        if (data && dataSize >= static_cast<int>(sizeof(dtMeshHeader)))
        {
            const dtMeshHeader* const header = reinterpret_cast<const dtMeshHeader*>(data);
            x = header->x;
            y = header->y;
            layer = header->layer;
        }
        Warning(eDLL_T::SERVER,
            "[NAVMESH] refusing tile x=%d y=%d layer=%d dataSize=%d\n",
            x, y, layer, dataSize);
        return DT_FAILURE | DT_INVALID_PARAM;
    }

    //There are no statically allocated tiles passed through this function as it is all read from the nav files on disk
    //Set the free data flag here to avoid leaking memory when we come to destroy the tile
    flags |= DT_TILE_FREE_DATA;

    // note(kawe): replaced with SDK's variant for easier debugging.
    return nav->addTile(data, dataSize, flags, lastRef, nullptr);
}

//-----------------------------------------------------------------------------
// Purpose: finds the nearest polygon to specified center point.
// Output: the status flags for the query.
//-----------------------------------------------------------------------------
static dtStatus Detour_FindNearestPoly(dtNavMeshQuery* const query, const rdVec3D* const center,
    const rdVec3D* const halfExtents, const dtQueryFilter* const filter,
    dtPolyRef* const nearestRef, rdVec3D* const nearestPt)
{
    // note(kawe): the SDK's implementation fixes the following issue
    // https://github.com/recastnavigation/recastnavigation/issues/107
    // 
    // Its also more accurate and robust compared to the old one, as the new
    // one favors climb height over straight list distances to nearest poly.
    return query->findNearestPoly(center, halfExtents, filter, nearestRef, nearestPt, nullptr);
}

//-----------------------------------------------------------------------------
// Purpose: finds a path from the start polygon to the end polygon.
// Output: the status flags for the query.
//-----------------------------------------------------------------------------
static dtStatus Detour_FindPath(dtNavMeshQuery* query, dtPolyRef startRef, dtPolyRef endRef,
    const rdVec3D* startPos, const rdVec3D* endPos, const dtQueryFilter* filter, dtPolyRef* path,
    unsigned char* jump, int* pathCount, const int maxPath)
{
    // note(kawe): replaced with SDK's version, because the implementation in
    // the engine returns the wrong status code when we are out of nodes.
    return query->findPath(startRef, endRef, startPos, endPos, filter, path, jump, pathCount, maxPath);
}

//-----------------------------------------------------------------------------
// Purpose: finds the straight path from the start to the end position within the polygon corridor.
// Output: the status flags for the query.
//-----------------------------------------------------------------------------
static dtStatus Detour_FindStraightPath(dtNavMeshQuery* const query, const rdVec3D* const startPos,
    const rdVec3D* const endPos, const dtPolyRef* const path, const unsigned char* const jumpTypes,
    const int pathSize, rdVec3D* const straightPath, unsigned char* const straightPathFlags,
    dtPolyRef* const straightPathRefs, unsigned char* const straightPathJumps,
    int* const straightPathCount, const int unused, const int jumpFilter, const int options)
{
    // note(kawe): engine's implementation has been replaced with the SDK's
    // version, because there was a possibility for an underflow when the
    // function tried to add a jump vertex into the path corridor while the
    // current index was still 0. In order to add jump vertices, the code
    // must retrieve the previous polygon, but when the iterator is 0, it
    // will underflow.
    //
    // The second reason this function has been replaced, is because this
    // function calls dtNavMeshQuery::appendPortals which has a special
    // case for any traverse type below the value DT_MAX_TRAVERSE_TYPES,
    // however the code looked for <= DT_MAX_TRAVERSE_TYPES while it has
    // to check for < DT_MAX_TRAVERSE_TYPES since traverse types are zero
    // indexed and masked out with (DT_MAX_TRAVERSE_TYPES-1), meaning that
    // the value equal to DT_MAX_TRAVERSE_TYPES will wrap back to 0, and 0
    // is a valid traverse type. So invalid input defines it twice.
    //
    // The third reason this function has been replaced, is because the
    // SDK's implementation fixes the following issues
    // https://github.com/recastnavigation/recastnavigation/issues/515
    // https://github.com/recastnavigation/recastnavigation/issues/735
    return query->findStraightPath(startPos, endPos, path, jumpTypes, pathSize,
                  straightPath, straightPathFlags, straightPathRefs, straightPathJumps,
                  straightPathCount, DT_DEFAULT_STRAIGHT_PATH_RESOLUTION, jumpFilter, options);
}

//-----------------------------------------------------------------------------
// Purpose: moves from the start to the end position constrained to the navigation mesh.
// Output: the status flags for the query.
//-----------------------------------------------------------------------------
static dtStatus Detour_MoveAlongSurface(dtNavMeshQuery* const query, dtPolyRef startRef, const rdVec3D* startPos,
    const rdVec3D* endPos, const dtQueryFilter* filter, rdVec3D* resultPos, dtPolyRef* visitedPolys,
    int* visitedCount, const int maxVisitedSize, const unsigned char options)
{
    const dtStatus stat = query->moveAlongSurface(startRef, startPos, endPos, filter, resultPos,
                                                  visitedPolys, visitedCount, maxVisitedSize, options);

    if (dtStatusSucceed(stat) && navmesh_move_along_surface_asserts->GetBool() && !*visitedCount)
    {
        Error(eDLL_T::SERVER, 0, "%s - Failed to visit any polygons from <%g, %g, %g> to <%g, %g, %g>\n",
            __FUNCTION__, startPos->x, startPos->y, startPos->z, endPos->x, endPos->y, endPos->z);
    }

    return stat;
}

//-----------------------------------------------------------------------------
// Purpose: casts a 'walkability' ray along the surface of the navigation mesh.
// Output: the status flags for the query.
//-----------------------------------------------------------------------------
static dtStatus Detour_Raycast(dtNavMeshQuery* const query, const dtPolyRef startRef,
    const rdVec3D* const startPos, const rdVec3D* const endPos,
    const dtQueryFilter* const filter, dtRaycastHit* const hit)
{
    // note(kawe): engine's implementation has been replaced with the SDK's
    // version, because the coordinate system of the engine's implementation 
    // hasn't been properly converted. The hit normals were still following
    // the XZY scheme (Y up) while this engine uses the XYZ scheme (Z up).
    // There is also a special case for tile borders which were also using
    // the incorrect coordinate system causing the NPC's to becoming stuck
    // in rare occasions when they try to perform a close range action.
    return query->raycast(startRef, startPos, endPos, filter, 0, hit, 0);
}

//-----------------------------------------------------------------------------
// Purpose: finds the closest point on the specified polygon.
// Output: the status flags for the query.
//-----------------------------------------------------------------------------
static bool Detour_ClosestPointOnPoly(dtNavMeshQuery* query, const dtPolyRef ref,
    const rdVec3D* pos, rdVec3D* closest, bool* posOverPoly, float* dist)
{
    // note(kawe): function has been replaced with the SDK's variant due to
    // https://github.com/recastnavigation/recastnavigation/issues/556
    // 
    // This API is also a lot more robust than the game's implementation.
    return dtStatusSucceed(query->closestPointOnPoly(ref, pos, closest, posOverPoly, dist, nullptr));
}

//-----------------------------------------------------------------------------
// Purpose: returns a point on the boundary closest to the source point if the
// source point is outside the polygon's xy-bounds.
// Output: the status flags for the query.
//-----------------------------------------------------------------------------
static dtStatus Detour_ClosestPointOnPolyBoundary(dtNavMeshQuery* query,
    const dtPolyRef ref, const rdVec3D* pos, rdVec3D* closest, float* dist)
{
    // note(kawe): function has been replaced with the SDK's variant due to
    // https://github.com/recastnavigation/recastnavigation/issues/556
    // 
    // This API is also a lot more robust than the game's implementation.
    return query->closestPointOnPolyBoundary(ref, pos, closest, dist);
}

//-----------------------------------------------------------------------------
// Purpose: finds the closest point on the specified polygon.
// Output: the status flags for the query.
//-----------------------------------------------------------------------------
static dtStatus Detour_GetPolyHeight(dtNavMeshQuery* query, const dtPolyRef ref,
    const rdVec3D* pos, float* height, rdVec3D* normal)
{
    // note(kawe): see
    // https://github.com/recastnavigation/recastnavigation/issues/556
    //
    // the game is based on a Detour implementation from 2015, which is
    // before the rdPointInPolygon check was being added in getPolyHeight.
    // The implementation of 2015 also fails when the point happens to be
    // on the edge of the polygon. The old code has now been replaced with
    // the new closestPointOnPoly check below since we want it to be as
    // permissive and robust as possible, getPolHeight now discards the 
    // query if the point happens to reside outside polygon's XY bounds.
    rdVec3D closest;
    const dtStatus stat = query->closestPointOnPoly(ref, pos, &closest, nullptr, nullptr, normal);

    if (dtStatusSucceed(stat))
        *height = closest.z;

    return stat;
}

//-----------------------------------------------------------------------------
// Purpose:.
// Output: a pointer to the requested node.
//-----------------------------------------------------------------------------
static dtNode* Detour_GetNode(dtNodePool* const nodePool, const dtPolyRef id,
    const unsigned char state)
{
    // note(kawe): engine's implementation has been replaced with the SDK's
    // version, because the bit field 'dtNode::jump' was never set in the
    // engine code. It must be initialized to DT_NULL_TRAVERSE_TYPE when
    // nodes are being initialized.
    return nodePool->getNode(id, state);
}

//-----------------------------------------------------------------------------
// Purpose: initialize NavMesh and Detour query singleton for level
//-----------------------------------------------------------------------------
void Detour_LevelInit()
{
    v_Detour_LevelInit();
    Detour_IsLoaded(); // Inform user which NavMesh files had failed to load.
}

//-----------------------------------------------------------------------------
// Purpose: free's the memory used by all valid NavMesh slots
//-----------------------------------------------------------------------------
void Detour_LevelShutdown()
{
    for (int i = 0; i < NAVMESH_COUNT; i++)
    {
        Detour_FreeNavMeshByType(NavMeshType_e(i));
    }
}

//-----------------------------------------------------------------------------
// Purpose: checks if the NavMesh has failed to load
// Output: true if a NavMesh has successfully loaded, false otherwise
//-----------------------------------------------------------------------------
bool Detour_IsLoaded()
{
    int ret = 0;
    for (int i = 0; i < NAVMESH_COUNT; i++)
    {
        const dtNavMesh* nav = Detour_GetNavMeshByType(NavMeshType_e(i));
        if (!nav) // Failed to load...
        {
            DevWarning(eDLL_T::SERVER, "NavMesh '%s%s_%s%s' not loaded\n", 
                NAVMESH_PATH, gpGlobals->mapName.ToCStr(),
                NavMesh_GetNameForType(NavMeshType_e(i)), NAVMESH_EXT);

            ret++;
        }
    }

    Assert(ret <= NAVMESH_COUNT);

    if (ret == NAVMESH_COUNT)
        DevMsg(eDLL_T::SERVER, "[NAVMESH] no NavMesh present for map '%s'\n", gpGlobals->mapName.ToCStr());
    else if (ret > 0)
        Warning(eDLL_T::SERVER, "[NAVMESH] %d of %d hulls failed to load for map '%s'\n",
            ret, NAVMESH_COUNT, gpGlobals->mapName.ToCStr());

    return (ret != NAVMESH_COUNT);
}

//-----------------------------------------------------------------------------
// Purpose: hot swaps the NavMesh with the current files on the disk
// (All types will be reloaded! If NavMesh for type no longer exist, it will be kept empty!!!)
//-----------------------------------------------------------------------------
void Detour_HotSwap()
{
    // Make sure when we hotswap, that the calling thread has latched
    // into the server frame thread, or has waited for its completion.
    // We cannot start the mutation while the server frame is active.
    Assert(ThreadCouldDoServerWork());

    if (g_pServerScript)
        g_pServerScript->ExecuteCodeCallback("CodeCallback_OnNavMeshHotSwapBegin");

    const dtNavMesh* const queryNav = g_navMeshQuery->getAttachedNavMesh();
    NavMeshType_e queryNavType = NAVMESH_INVALID;

    if (queryNav)
    {
        // Figure out which NavMesh type is attached to the global query.
        for (int i = 0; i < NAVMESH_COUNT; i++)
        {
            const NavMeshType_e in = (NavMeshType_e)i;

            if (queryNav != Detour_GetNavMeshByType(in))
                continue;

            queryNavType = in;
            break;
        }
    }

    // Free and re-init NavMesh.
    Detour_LevelShutdown();
    v_Detour_LevelInit();

    if (!Detour_IsLoaded())
        Error(eDLL_T::SERVER, NOERROR, "%s - Failed to hot swap NavMesh: %s\n", __FUNCTION__, 
            "one or more missing NavMesh types, Detour logic may be unavailable");

    // Attach the new NavMesh to the global Detour query.
    if (queryNavType != NAVMESH_INVALID)
    {
        const dtNavMesh* const newQueryNav = Detour_GetNavMeshByType(queryNavType);

        if (newQueryNav)
            g_navMeshQuery->attachNavMeshUnsafe(newQueryNav);
        else
            Error(eDLL_T::SERVER, NOERROR, "%s - Failed to hot swap NavMesh: %s\n", __FUNCTION__, 
                "previously attached NavMesh type is no longer available for global Detour query");
    }

    const int numAis = g_AI_Manager->NumAIs();
    CAI_BaseNPC** const pAis = g_AI_Manager->AccessAIs();

    // Reinitialize the AI's navmesh query to update the navmesh cache.
    for (int i = 0; i < numAis; i++)
    {
        CAI_BaseNPC* const npc = pAis[i];
        CAI_Pathfinder* const pathFinder = npc->GetPathfinder();

        const NavMeshType_e navType = NAI_Hull::NavMeshType(npc->GetHullType());
        const dtNavMesh* const navMesh = Detour_GetNavMeshByType(navType);

        if (dtStatusFailed(pathFinder->GetNavMeshQuery()->init(navMesh, 2048)))
            Error(eDLL_T::SERVER, NOERROR, "%s - Failed to initialize Detour NavMesh query for %s\n", __FUNCTION__, UTIL_GetEntityScriptInfo(npc));
    }

    if (g_pServerScript)
        g_pServerScript->ExecuteCodeCallback("CodeCallback_OnNavMeshHotSwapEnd");
}

/*
=====================
Detour_HotSwap_f

  Hot swaps the NavMesh
  while the game is running
=====================
*/
static void Detour_HotSwap_f()
{
    if (!g_pServer->IsActive())
        return; // Only execute if server is initialized and active.

    Msg(eDLL_T::SERVER, "Executing NavMesh hot swap for level '%s'\n",
        gpGlobals->mapName.ToCStr());

    CFastTimer timer;

    timer.Start();
    Detour_HotSwap();

    timer.End();
    Msg(eDLL_T::SERVER, "Hot swap took %lf seconds\n", timer.GetDuration().GetSeconds());
}

static ConCommand navmesh_hotswap("navmesh_hotswap", Detour_HotSwap_f, "Hot swap the NavMesh for all hulls", FCVAR_DEVELOPMENTONLY | FCVAR_SERVER_FRAME_THREAD);

///////////////////////////////////////////////////////////////////////////////
void VRecast::Detour(const bool bAttach) const
{
	DetourSetup(&v_Detour_IsGoalPolyReachable, &Detour_IsGoalPolyReachable, bAttach);
	DetourSetup(&v_Detour_LevelInit, &Detour_LevelInit, bAttach);
	DetourSetup(&dtNavMesh__addTile, &Detour_AddTile, bAttach);
	DetourSetup(&dtNavMeshQuery__findNearestPoly, &Detour_FindNearestPoly, bAttach);
	DetourSetup(&dtNavMeshQuery__findPath, &Detour_FindPath, bAttach);
	DetourSetup(&dtNavMeshQuery__findStraightPath, &Detour_FindStraightPath, bAttach);
	DetourSetup(&dtNavMeshQuery__moveAlongSurface, &Detour_MoveAlongSurface, bAttach);
	DetourSetup(&dtNavMeshQuery__raycast, &Detour_Raycast, bAttach);
	DetourSetup(&dtNavMeshQuery__closestPointOnPoly, &Detour_ClosestPointOnPoly, bAttach);
	DetourSetup(&dtNavMeshQuery__closestPointOnPolyBoundary, &Detour_ClosestPointOnPolyBoundary, bAttach);
	DetourSetup(&dtNavMeshQuery__getPolyHeight, &Detour_GetPolyHeight, bAttach);
	DetourSetup(&dtNodePool__getPool, &Detour_GetNode, bAttach);

    extern void AICommands_Dummy();
    AICommands_Dummy();
}
