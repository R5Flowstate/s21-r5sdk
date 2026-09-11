//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: render surface
//
// $NoKeywords: $
//===========================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "windows/id3dx.h"
#include "engine/gl_rsurf.h"
#include "materialsystem/cmaterialsystem.h"
#include "materialsystem/texturestreaming.h"

static ConVar r_drawWorldMeshes("r_drawWorldMeshes", "1", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT, "Render world meshes.");
static ConVar r_drawWorldMeshesDepthOnly("r_drawWorldMeshesDepthOnly", "1", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT, "Render world meshes (depth only).");
static ConVar r_drawWorldMeshesDepthAtTheEnd("r_drawWorldMeshesDepthAtTheEnd", "1", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT, "Render world meshes (depth at the end).");

static void R_DrawAlphaSort(CViewRender* const viewRender, __int64 a2, __int64 a3, unsigned int a4, int a5, __int64 a6)
{
	bool commitComputeShaderResult = false;

	if (a5 && r_drawalphasort->GetBool())
	{
		const u32 flags = *(_DWORD*)(a2 + 464);

		if (!flags && gpu_driven_tex_stream->GetBool())
		{
			TextureStreamMgr_GetComputeShaderResult();
			commitComputeShaderResult = true;
		}
	}

	V_DrawAlphaSort(viewRender, a2, a3, a4, a5, a6);

	if (commitComputeShaderResult)
		TextureStreamMgr_CommitComputeShaderResult(2);
}

static void* R_DrawWorldMeshes(void* baseEntity, void* renderContext, DrawWorldLists_t worldLists)
{
	if (r_drawWorldMeshes.GetBool())
		return V_DrawWorldMeshes(baseEntity, renderContext, worldLists);
	else
		return nullptr;
}

static void* R_DrawWorldMeshesDepthOnly(void* renderContext, DrawWorldLists_t worldLists)
{
	if (r_drawWorldMeshesDepthOnly.GetBool())
		return V_DrawWorldMeshesDepthOnly(renderContext, worldLists);
	else
		return nullptr;
}

static void* R_DrawWorldMeshesDepthAtTheEnd(void* ptr1, void* ptr2, void* ptr3, DrawWorldLists_t worldLists)
{
	if (r_drawWorldMeshesDepthAtTheEnd.GetBool())
		return V_DrawWorldMeshesDepthAtTheEnd(ptr1, ptr2, ptr3, worldLists);
	else
		return nullptr;
}

