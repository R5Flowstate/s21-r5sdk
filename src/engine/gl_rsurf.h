#pragma once
#include "public/ivrenderview.h"
#include "game/client/viewrender.h"

inline void(*V_DrawAlphaSort)(CViewRender* const viewRender, __int64 a2, __int64 a3, unsigned int a4, int a5, __int64 a6);

inline void*(*V_DrawWorldMeshes)(void* baseEntity, void* renderContext, DrawWorldLists_t worldLists);
inline void*(*V_DrawWorldMeshesDepthOnly)(void* renderContext, DrawWorldLists_t worldLists);
inline void*(*V_DrawWorldMeshesDepthAtTheEnd)(void* ptr1, void* ptr2, void* ptr3, DrawWorldLists_t worldLists);

