#pragma once
//=============================================================================//
//
// Purpose: Compile-time layout asserts for detour-touched structs
//
//=============================================================================//
#include "thirdparty/recast/Detour/Include/DetourNavMesh.h"

// Layout read from the engine tile fixup at r5apex_ds.exe RVA 0xF44A00 and
// verified against a shipped mesh. Hooks hand engine-allocated Detour objects
// to SDK code, so a drift here is silent corruption.

#if DT_NAVMESH_SET_VERSION == 8

// Element sizes from the engine's section stride arithmetic.
static_assert(sizeof(rdVec3D) == 12);
static_assert(sizeof(dtMeshHeader) == 108);
static_assert(sizeof(dtPoly) == 52);
static_assert(sizeof(dtLink) == 16);
static_assert(sizeof(dtPolyDetail) == 12);
static_assert(sizeof(dtBVNode) == 16);
static_assert(sizeof(dtOffMeshConnection) == 52);
static_assert(sizeof(dtCell) == 76);
static_assert(sizeof(dtMeshTile) == 128);

// Field offsets matching the engine's tile/header layout.
static_assert(offsetof(dtMeshTile, header) == 8);
static_assert(offsetof(dtMeshTile, polys) == 16);
static_assert(offsetof(dtMeshTile, polyMap) == 24);
static_assert(offsetof(dtMeshTile, verts) == 32);
static_assert(offsetof(dtMeshTile, links) == 40);
static_assert(offsetof(dtMeshTile, detailMeshes) == 48);
static_assert(offsetof(dtMeshTile, detailVerts) == 56);
static_assert(offsetof(dtMeshTile, detailTris) == 64);
static_assert(offsetof(dtMeshTile, bvTree) == 72);
static_assert(offsetof(dtMeshTile, offMeshCons) == 80);
static_assert(offsetof(dtMeshTile, cells) == 88);
static_assert(offsetof(dtMeshTile, data) == 96);
static_assert(offsetof(dtMeshTile, dataSize) == 104);
static_assert(offsetof(dtMeshTile, flags) == 108);
static_assert(offsetof(dtMeshTile, next) == 112);
static_assert(offsetof(dtMeshTile, tracker) == 120);
static_assert(offsetof(dtMeshHeader, polyCount) == 24);
static_assert(offsetof(dtMeshHeader, polyMapCount) == 28);
static_assert(offsetof(dtMeshHeader, vertCount) == 32);
static_assert(offsetof(dtMeshHeader, maxLinkCount) == 36);
static_assert(offsetof(dtMeshHeader, detailMeshCount) == 40);
static_assert(offsetof(dtMeshHeader, detailVertCount) == 44);
static_assert(offsetof(dtMeshHeader, detailTriCount) == 48);
static_assert(offsetof(dtMeshHeader, bvNodeCount) == 52);
static_assert(offsetof(dtMeshHeader, offMeshConCount) == 56);

#endif // DT_NAVMESH_SET_VERSION == 8
