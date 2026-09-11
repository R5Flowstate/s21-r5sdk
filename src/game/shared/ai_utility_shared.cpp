//=============================================================================//
//
// Purpose: Shared AI utility.
//
//=============================================================================//
// ai_utility_shared.cpp: requires server.dll and client.dll!
//
/////////////////////////////////////////////////////////////////////////////////

#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "tier2/renderutils.h"
#include "mathlib/color.h"
#include "mathlib/vector.h"
#include "mathlib/ssemath.h"
#include "engine/debugoverlay.h"
#include "game/shared/ai_utility_shared.h"
#include "game/server/ai_utility.h"
#include "game/server/ai_networkmanager.h"
#include "game/server/ai_network.h"
#include "game/client/viewrender.h"
#include "thirdparty/recast/Shared/Include/SharedCommon.h"
#include "thirdparty/recast/Detour/Include/DetourNavMesh.h"
#include "thirdparty/recast/DebugUtils/Include/DetourDebugDraw.h"

//------------------------------------------------------------------------------
// Purpose: console variables
//------------------------------------------------------------------------------
static ConVar navmesh_draw_enable("navmesh_draw_enable", "0", FCVAR_DEVELOPMENTONLY, "Enable the debug drawing of the NavMesh");
static ConVar navmesh_draw_type("navmesh_draw_type", "0", FCVAR_DEVELOPMENTONLY, "The NavMesh type to use for debug drawing", true, 0.f, true, 4.f, nullptr, "0 = small, 1 = med_short, 2 = medium, 3 = large, 4 = extra_large");
static ConVar navmesh_draw_range("navmesh_draw_range", "3000", FCVAR_DEVELOPMENTONLY, "Only draw tiles within this distance from our camera", true, 0.f, false, 0.f);

static ConVar navmesh_draw_use_plane_culling("navmesh_draw_use_plane_culling", "0", FCVAR_DEVELOPMENTONLY, "Use plane culling to improve render performance (can be too aggressive on larger tiles)");
static ConVar navmesh_draw_additional_z("navmesh_draw_additional_z", "0", FCVAR_DEVELOPMENTONLY, "Offset NavMesh drawing by this amount on the Z-axis");

static ConVar navmesh_draw_off_mesh_connections("navmesh_draw_off_mesh_connections", "1", FCVAR_DEVELOPMENTONLY, "Draws the off-mesh connections of the NavMesh");
static ConVar navmesh_draw_bvtree("navmesh_draw_bvtree", "0", FCVAR_DEVELOPMENTONLY, "Draws the BVTree of the NavMesh polygons");
static ConVar navmesh_draw_portals("navmesh_draw_portals", "0", FCVAR_DEVELOPMENTONLY, "Draws the portal network connecting the tiles of the NavMesh");

static ConVar navmesh_draw_tile_bounds("navmesh_draw_tile_bounds", "0", FCVAR_DEVELOPMENTONLY, "Draws the bounds of the NavMesh tiles");
static ConVar navmesh_draw_tile_cells("navmesh_draw_tile_cells", "0", FCVAR_DEVELOPMENTONLY, "Draws the cells of the NavMesh tiles");

static ConVar navmesh_draw_poly_faces("navmesh_draw_poly_faces", "1", FCVAR_DEVELOPMENTONLY, "Draws the polygon faces of the NavMesh tiles");
static ConVar navmesh_draw_poly_detail("navmesh_draw_poly_detail", "0", FCVAR_DEVELOPMENTONLY, "Draws the detail of the NavMesh polygons");
static ConVar navmesh_draw_poly_verts("navmesh_draw_poly_verts", "0", FCVAR_DEVELOPMENTONLY, "Draws the vertices of the NavMesh polygons");
static ConVar navmesh_draw_poly_centers("navmesh_draw_poly_centers", "0", FCVAR_DEVELOPMENTONLY, "Draws the center of the NavMesh polygons");

static ConVar navmesh_draw_poly_bounds_inner("navmesh_draw_poly_bounds_inner", "1", FCVAR_DEVELOPMENTONLY, "Draws the inner polygon bounds of the NavMesh tiles");
static ConVar navmesh_draw_poly_bounds_outer("navmesh_draw_poly_bounds_outer", "1", FCVAR_DEVELOPMENTONLY, "Draws the outer polygon bounds of the NavMesh tiles");

static ConVar navmesh_draw_traverse_portals("navmesh_draw_traverse_portals", "0", FCVAR_DEVELOPMENTONLY, "Draws the traversal network connecting the polygons of the NavMesh");
static ConVar navmesh_draw_traverse_portals_type("navmesh_draw_traverse_portals_type", "-1", FCVAR_DEVELOPMENTONLY, "Only draw traverse portals of this type (-1 = everything)", true, -1, true, DT_MAX_TRAVERSE_TYPES-1, "Type: >= -1 && < DT_MAX_TRAVERSE_TYPES - 1");

static ConVar navmesh_draw_force_opaque("navmesh_draw_force_opaque", "0", FCVAR_DEVELOPMENTONLY, "Disable transparency in NavMesh debug draw");
static ConVar navmesh_draw_flag_show_tile_id("navmesh_draw_show_tile_ids", "0", FCVAR_DEVELOPMENTONLY, "Color NavMesh tiles by their lookup ID");
static ConVar navmesh_draw_flag_show_poly_groups("navmesh_draw_show_poly_groups", "0", FCVAR_DEVELOPMENTONLY, "Color NavMesh polygons by their group ID");

static ConVar ai_script_nodes_draw_range("ai_script_nodes_draw_range", "1000", FCVAR_DEVELOPMENTONLY, "Only draw AIN script nodes within this distance from our camera");
static ConVar ai_script_nodes_draw_nearest("ai_script_nodes_draw_nearest", "1", FCVAR_DEVELOPMENTONLY, "Debug draw AIN script node links to nearest node (build order is used if null)");

//------------------------------------------------------------------------------
// Purpose
//------------------------------------------------------------------------------
CAI_Utility::CAI_Utility(void)
{
}

//------------------------------------------------------------------------------
// Purpose: run the NavMesh renderer
//------------------------------------------------------------------------------
void CAI_Utility::RunRenderFrame(void)
{
    if (navmesh_draw_enable.GetBool())
    {
        const dtNavMesh* const nav = Detour_GetNavMeshByType(NavMeshType_e(navmesh_draw_type.GetInt()));

        if (nav)
        {
            u32 navmeshDrawFlags = 0;

            if (navmesh_draw_off_mesh_connections.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_OFFMESHCONS;
            if (navmesh_draw_bvtree.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_BVTREE;
            if (navmesh_draw_portals.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_PORTALS;
            if (navmesh_draw_tile_bounds.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_TILE_BOUNDS;
            if (navmesh_draw_tile_cells.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_TILE_CELLS;
            if (navmesh_draw_poly_faces.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_POLY_FACES;
            if (navmesh_draw_poly_detail.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_POLY_EDGES;
            if (navmesh_draw_poly_verts.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_POLY_VERTS;
            if (navmesh_draw_poly_bounds_inner.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_POLY_BOUNDS_INNER;
            if (navmesh_draw_poly_bounds_outer.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_POLY_BOUNDS_OUTER;
            if (navmesh_draw_poly_centers.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_POLY_CENTERS;
            if (navmesh_draw_traverse_portals.GetBool())
                navmeshDrawFlags |= DU_DRAW_DETOURMESH_TRAVERSE_LINKS;

            if (navmeshDrawFlags != 0)
            {
                // These are checked and added here, because they require the
                // above flags to be used. If none of those are set, these
                // won't do anything.
                if (navmesh_draw_flag_show_tile_id.GetBool())
                    navmeshDrawFlags |= DU_DRAW_DETOURMESH_TILE_COLORS;
                if (navmesh_draw_flag_show_poly_groups.GetBool())
                    navmeshDrawFlags |= DU_DRAW_DETOURMESH_POLY_GROUPS;
                if (!navmesh_draw_force_opaque.GetBool())
                    navmeshDrawFlags |= DU_DRAW_DETOURMESH_ALPHA;

                DrawNavMesh(*nav, navmeshDrawFlags);
            }
        }
    }

#ifndef CLIENT_DLL
    if (ai_script_nodes_draw && ai_script_nodes_draw->GetBool())
        DrawAIScriptNetwork(*g_pAINetwork, MainViewOrigin(), ai_script_nodes_draw_range.GetFloat(), true);
#endif // !CLIENT_DLL
}

//------------------------------------------------------------------------------
// Purpose: draw NavMesh
//------------------------------------------------------------------------------
void CAI_Utility::DrawNavMesh(const dtNavMesh& mesh, const u32 flags)
{
    const Vector3D& camPos = MainViewOrigin();
    const VPlane* pPlane = nullptr;

    VPlane plane;

    if (navmesh_draw_use_plane_culling.GetBool())
    {
        const QAngle& camAng = MainViewAngles();
        const Vector3D normal = camPos - camAng.GetNormal() * 256.0f;

        plane.Init(normal, camAng);
        pPlane = &plane;
    }

    m_navMeshQuery.attachNavMeshUnsafe(&mesh);
    const float maxRadius = navmesh_draw_range.GetFloat();

    const rdVec3D offset(0.f, 0.f, navmesh_draw_additional_z.GetFloat());
    duDrawTraverseLinkParams traverseLinkDrawParams;

    for (int i = 0; i < mesh.getMaxTiles(); i++)
    {
        const dtMeshTile* const tile = mesh.getTile(i);

        if (!tile->header)
            continue;

        if (!IsTileWithinRange(tile, pPlane, camPos, maxRadius))
            continue;

        if (flags & DU_DRAW_DETOURMESH_TRAVERSE_LINKS)
        {
            traverseLinkDrawParams.traverseLinkType = navmesh_draw_traverse_portals_type.GetInt();
            traverseLinkDrawParams.extraOffset = tile->header->walkableRadius * 2;
        }

        duDebugDrawMeshTile(&m_navMeshDebugDraw, mesh, &m_navMeshQuery, tile, &offset, flags, traverseLinkDrawParams);
    }
}

static const VectorAligned s_vMaxs = { 50.0f, 50.0f, 50.0f };
static const VectorAligned s_vSubMask = { 25.0f, 25.0f, 25.0f };

static const fltx4 s_xMins = LoadZeroSIMD();
static const fltx4 s_xMaxs = LoadAlignedSIMD(s_vMaxs);
static const fltx4 s_xSubMask = LoadAlignedSIMD(s_vSubMask);

//------------------------------------------------------------------------------
// Purpose: draw AI script network
// Input: *pNetwork - 
// &vCameraPos - 
// flCameraRange - 
// bUseDepthBuffer - 
//------------------------------------------------------------------------------
void CAI_Utility::DrawAIScriptNetwork(
    const CAI_Network* pNetwork,
    const Vector3D& vCameraPos,
    const float flCameraRange,
    const bool bUseDepthBuffer) const
{
    if (!pNetwork)
        return; // AI Network not build or loaded.

    const bool bDrawNearest = ai_script_nodes_draw_nearest.GetBool();

    matrix3x4_t vTransforms;
    std::unordered_set<int64_t> uLinkSet;

    for (int i = 0, ns = pNetwork->NumScriptNodes(); i < ns; i++)
    {
        const CAI_ScriptNode* pScriptNode = &pNetwork->m_ScriptNode[i];
        const fltx4 xOrigin = SubSIMD(// Subtract 25.f from our scalars to align box with node.
            LoadUnaligned3SIMD(&pScriptNode->m_vOrigin), s_xSubMask);

        if (flCameraRange > 0.0f)
        {
            // Flip the script node Z axis with that of the camera, so that it won't be used for
            // the final distance computation. This allows for viewing the AI Network from above.
            const fltx4 xOriginCamZ = SetComponentSIMD(xOrigin, 2, vCameraPos.z);

            if (vCameraPos.DistTo(*reinterpret_cast<const Vector3D*>(&xOriginCamZ)) > flCameraRange)
                continue; // Do not render if node is not within range set by cvar 'navmesh_debug_camera_range'.
        }

        // Construct box matrix transforms.
        vTransforms.Init(
            { 1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            *reinterpret_cast<const Vector3D*>(&xOrigin));

        static const Color boxColor(0, 255, 0, 30);
        RenderBox(vTransforms, *reinterpret_cast<const Vector3D*>(&s_xMins),
            *reinterpret_cast<const Vector3D*>(&s_xMaxs), boxColor, bUseDepthBuffer);

        static const Color boxColorWireframe(0, 255, 0, 255);
        RenderWireframeBox(vTransforms, *reinterpret_cast<const Vector3D*>(&s_xMins),
            *reinterpret_cast<const Vector3D*>(&s_xMaxs), boxColorWireframe, bUseDepthBuffer);

        static const Color linkColor(255, 0, 0, 255);

        if (bDrawNearest) // Render links to the nearest node.
        {
            int nNearest = GetNearestNodeToPos(pNetwork, &pScriptNode->m_vOrigin);
            if (nNearest != NO_NODE) // NO_NODE = -1
            {
                shortx8 packedLinks = PackNodeLink(i, nNearest);
                packedLinks = _mm_srli_si128(packedLinks, 8); // Only the upper 64bits are used.

                auto p = uLinkSet.insert(reinterpret_cast<int64_t&>(packedLinks));
                if (p.second) // Only render if link hasn't already been rendered.
                {
                    const CAI_ScriptNode* pNearestNode = &pNetwork->m_ScriptNode[nNearest];
                    RenderLine(pScriptNode->m_vOrigin, pNearestNode->m_vOrigin, linkColor, bUseDepthBuffer);
                }
            }
        }
        else if (i > 0) // Render links in the order the AI Network was build.
            RenderLine((pScriptNode - 1)->m_vOrigin, pScriptNode->m_vOrigin, linkColor, bUseDepthBuffer);
    }
}

//------------------------------------------------------------------------------
// Purpose: packs 4 node indices together
// Input: a - (set 1)
// c - (set 2)
// Output: packed node set as i64x2
//------------------------------------------------------------------------------
shortx8 CAI_Utility::PackNodeLink(const i32 a, const i32 b, const i32 c, const i32 d)
{
    shortx8 xResult = _mm_set_epi32(a, b, c, d);

    // We shuffle a b and c d if following condition is met, this is to 
    // ensure we always end up with one possible combination of indices.
    if (a < b) // Swap 'a' with 'b'.
        xResult = _mm_shuffle_epi32(xResult, _MM_SHUFFLE(2, 3, 1, 0));
    if (c < d) // Swap 'c' with 'd'.
        xResult = _mm_shuffle_epi32(xResult, _MM_SHUFFLE(3, 2, 0, 1));

    return xResult;
}

//------------------------------------------------------------------------------
// Purpose: checks if the NavMesh tile is within the camera radius
// Input: *pTile - 
// &vCamera - 
// flCameraRadius - 
// Output: true if within radius, false otherwise
//------------------------------------------------------------------------------
bool CAI_Utility::IsTileWithinRange(const dtMeshTile* pTile, const VPlane* vPlane, const Vector3D& vCamera, const float flCameraRadius) const
{
    const fltx4 xMinBound = LoadGatherSIMD(pTile->header->bmin[0], pTile->header->bmin[1], vCamera.z, 0.0f);
    const fltx4 xMaxBound = LoadGatherSIMD(pTile->header->bmax[0], pTile->header->bmax[1], vCamera.z, 0.0f);

    const Vector3D* vecMinBound = reinterpret_cast<const Vector3D*>(&xMinBound);
    const Vector3D* vecMaxBound = reinterpret_cast<const Vector3D*>(&xMaxBound);

    if (flCameraRadius > 0.0f)
    {
        // Too far from camera, do not render.
        if (vCamera.DistTo(*vecMinBound) > flCameraRadius ||
            vCamera.DistTo(*vecMaxBound) > flCameraRadius)
            return false;
    }

    if (vPlane)
    {
        // Behind the camera, do not render.
        if (vPlane->GetPointSide(*vecMinBound) != SIDE_FRONT ||
            vPlane->GetPointSide(*vecMaxBound) != SIDE_FRONT)
            return false;
    }

    return true;
}

//------------------------------------------------------------------------------
// Purpose: gets the nearest node index to position
// Input: *pAINetwork - 
// *vPos - 
// Output: node index ('NO_NODE' if no node has been found)
//------------------------------------------------------------------------------
int CAI_Utility::GetNearestNodeToPos(const CAI_Network* pAINetwork, const Vector3D* vPos)
{
    int result; // rax
    unsigned int v3; // er10
    __int64 v4; // rdx
    float v5; // xmm3_4
    unsigned int v6; // er8
    CAI_ScriptNode* v7; // rax
    float v8; // xmm4_4
    float v9; // xmm5_4
    float v10; // xmm6_4
    float* v11; // rcx
    float* v12; // rax
    float v13; // xmm7_4
    float v14; // xmm2_4
    unsigned int v15; // er9
    float v16; // xmm8_4
    float v17; // xmm2_4
    unsigned int v18; // er8
    float v19; // xmm9_4
    float v20; // xmm2_4
    unsigned int v21; // er9
    float v22; // xmm7_4
    float v23; // xmm2_4
    float* v24; // r9
    float v25; // xmm4_4
    float v26; // xmm2_4
    unsigned int v27; // ecx

    if (pAINetwork)
    {
        v3 = pAINetwork->m_iNumScriptNodes;
        v4 = 0i64;
        v5 = 640000.0;
        v6 = (unsigned int)NO_NODE;
        if (v3 >= 4)
        {
            v7 = pAINetwork->m_ScriptNode;
            v8 = vPos->x;
            v9 = vPos->y;
            v10 = vPos->z;
            v11 = &v7->m_vOrigin.z;
            v12 = &v7[1].m_vOrigin.y;
            do
            {
                v13 = v5;
                v14 = (float)((float)((float)(*(v11 - 1) - v9) * (float)(*(v11 - 1) - v9)) + (float)((float)(*(v11 - 2) - v8) * (float)(*(v11 - 2) - v8))) + (float)((float)(*v11 - v10) * (float)(*v11 - v10));
                if (v5 > v14)
                    v5 = (float)((float)((float)(*(v11 - 1) - v9) * (float)(*(v11 - 1) - v9)) + (float)((float)(*(v11 - 2) - v8) * (float)(*(v11 - 2) - v8))) + (float)((float)(*v11 - v10) * (float)(*v11 - v10));
                v15 = (unsigned int)v4;
                if (v13 <= v14)
                    v15 = v6;
                v16 = v5;
                v17 = (float)((float)((float)(*(v12 - 1) - v9) * (float)(*(v12 - 1) - v9)) + (float)((float)(v11[3] - v8) * (float)(v11[3] - v8))) + (float)((float)(*v12 - v10) * (float)(*v12 - v10));
                if (v5 > v17)
                    v5 = (float)((float)((float)(*(v12 - 1) - v9) * (float)(*(v12 - 1) - v9)) + (float)((float)(v11[3] - v8) * (float)(v11[3] - v8))) + (float)((float)(*v12 - v10) * (float)(*v12 - v10));
                v18 = (unsigned int)v4 + 1;
                if (v16 <= v17)
                    v18 = v15;
                v19 = v5;
                v20 = (float)((float)((float)(v12[4] - v9) * (float)(v12[4] - v9)) + (float)((float)(v11[8] - v8) * (float)(v11[8] - v8))) + (float)((float)(v12[5] - v10) * (float)(v12[5] - v10));
                if (v5 > v20)
                    v5 = (float)((float)((float)(v12[4] - v9) * (float)(v12[4] - v9)) + (float)((float)(v11[8] - v8) * (float)(v11[8] - v8))) + (float)((float)(v12[5] - v10) * (float)(v12[5] - v10));
                v21 = (unsigned int)v4 + 2;
                if (v19 <= v20)
                    v21 = v18;
                v22 = v5;
                v23 = (float)((float)((float)(v12[9] - v9) * (float)(v12[9] - v9)) + (float)((float)(v11[13] - v8) * (float)(v11[13] - v8))) + (float)((float)(v12[10] - v10) * (float)(v12[10] - v10));
                if (v5 > v23)
                    v5 = (float)((float)((float)(v12[9] - v9) * (float)(v12[9] - v9)) + (float)((float)(v11[13] - v8) * (float)(v11[13] - v8))) + (float)((float)(v12[10] - v10) * (float)(v12[10] - v10));
                v6 = (unsigned int)v4 + 3;
                if (v22 <= v23)
                    v6 = v21;
                v11 += 20;
                v12 += 20;
                v4 = (unsigned int)(v4 + 4);
            } while ((unsigned int)v4 < v3 - 3);
        }
        if ((unsigned int)v4 < v3)
        {
            v24 = &pAINetwork->m_ScriptNode->m_vOrigin.x + 5 * v4;
            do
            {
                v25 = v5;
                v26 = (float)((float)((float)(v24[1] - vPos->y) * (float)(v24[1] - vPos->y)) + (float)((float)(*v24 - vPos->x) * (float)(*v24 - vPos->x)))
                    + (float)((float)(v24[2] - vPos->z) * (float)(v24[2] - vPos->z));
                if (v5 > v26)
                    v5 = (float)((float)((float)(v24[1] - vPos->y) * (float)(v24[1] - vPos->y)) + (float)((float)(*v24 - vPos->x) * (float)(*v24 - vPos->x)))
                    + (float)((float)(v24[2] - vPos->z) * (float)(v24[2] - vPos->z));
                v27 = (unsigned int)v4;
                if (v25 <= v26)
                    v27 = v6;
                v24 += 5;
                LODWORD(v4) = (unsigned int)v4 + 1;
                v6 = v27;
            } while ((unsigned int)v4 < v3);
        }
        result = v6;
    }
    else
    {
        result = NULL;
    }
    return result;
}

CAI_Utility g_AIUtility;
