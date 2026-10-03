//=============================================================================//
//
// Purpose: Extra full-resolution scene views rendered by the engine's own
// scene pass, alongside the retail monitors. A view renders the raw
// pre-post scene (color + motion) at the main render rect with the main
// jitter, so a surface in the main frame can composite it and let the main
// AA and post run once over both.
//
//=============================================================================//
#ifndef CLIENT_SCENEVIEW_H
#define CLIENT_SCENEVIEW_H

#include "thirdparty/detours/include/idetour.h"

inline int64_t(*v_SceneView_SetupMonitors)(uintptr_t viewRender, uintptr_t viewBundle) = nullptr;
inline int64_t(*v_SceneView_DrawMonitors)(uintptr_t viewRender, uintptr_t monitors, int numMonitors) = nullptr;
inline uintptr_t(*v_SceneView_DrawScene)(uintptr_t viewRender, uintptr_t logView, uintptr_t setup,
	uint32_t pvsJob, uintptr_t renderTarget, uintptr_t fogParams, uint8_t bMonitor) = nullptr;
inline int64_t(*v_SceneView_AllocFrameMRTs)(uintptr_t rendererConfig) = nullptr;
struct SceneViewRect_t;
inline int64_t(*v_SceneView_ResolveMain)(const SceneViewRect_t* pRect, uintptr_t* pResolvedColor, uintptr_t* pResolvedMrt2,
	int* pDepthOverride, int8_t monitorIdx, uint8_t bTsaa, uint8_t bImplicitRescale) = nullptr;

inline void(*v_SceneView_BuildSetup)(uintptr_t setup, int viewId, const float* pOrigin, const float* pAngles,
	float tanHalfFovX, float zNear, float zFar, const int* pRect) = nullptr;
inline uint32_t(*v_SceneView_StartRenderLists)(uintptr_t logView, uintptr_t pvsSetup) = nullptr;
inline void(*v_SceneView_ApplyJitter)(uintptr_t setup) = nullptr;
inline void(*v_SceneView_InitSubsystems)(uintptr_t singleView) = nullptr;
inline void(*v_SceneView_MonitorLights)(uintptr_t monitorView, bool bBegin, uintptr_t unused) = nullptr;
inline uintptr_t(*v_SceneView_CreateRenderTarget)(const char* pszName, int w, int h, int sizeMode,
	int format, int type, uint32_t flags, int unk) = nullptr;

//-----------------------------------------------------------------------------
// Purpose: Per-frame view request. The portal renderer calls
// SceneView_Request every frame for each visible portal (at most
// SCENEVIEW_MAX_ACTIVE active); a slot not requested this frame is not drawn.
// sceneview_test drives slot 0 independently for bring-up.
//-----------------------------------------------------------------------------
static constexpr int SCENEVIEW_MAX_ACTIVE = 2;

// bFitRect: use tanHalfFovX (not the main view's) and remap the projection so
// pPortalRect fills the image, for surfaces whose UVs span that rect.
void SceneView_Request(int slot, const float* pOrigin, const float* pAngles, float tanHalfFovX,
	const float* pClipPlane, const float (*pPortalRect)[3], bool bFitRect = false);
// The slot's colour target (ITexture*), or 0 before its first draw.
uintptr_t SceneView_SlotColorTexture(int slot);
void SceneView_InvalidateHistory(int slot);
// Called once per frame, after the main view is set up and before the slots
// are, so requests made from it draw in the same frame.
typedef void (*SceneViewRequestProviderFn)(void);
void SceneView_SetRequestProvider(SceneViewRequestProviderFn fn);

// Read accessors for the portal surface renderer (additive; no behaviour change).
// SceneView_GetMainCamera copies the main view setup (origin, basis, jittered
// world-to-projection, tanHalfFovX); returns false when no frame ran yet.
bool SceneView_GetMainCamera(float* pOrigin, float* pForward, float* pRight, float* pUp,
	float* pWorldToProj, float* pTanHalfFovX);
uintptr_t SceneView_MainLogView(void);
// Main view projection of the last frame: tangents of the half field of view
// and the render rect (x, y, width, height).
bool SceneView_GetMainProjection(float* pTanHalfFovX, float* pTanHalfFovY, int* pRect);
uintptr_t SceneView_MaterialSystem(void);
// The main view's resolved (post-AA, pre-post) colour texture of the last frame.
uintptr_t SceneView_MainResolvedColor(void);
bool SceneView_SlotDrawn(int slot);
bool SceneView_IsOwnView(uintptr_t logView);

///////////////////////////////////////////////////////////////////////////////
class VSceneView : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("SceneView_SetupMonitors", v_SceneView_SetupMonitors);
		LogFunAdr("SceneView_DrawMonitors", v_SceneView_DrawMonitors);
		LogFunAdr("SceneView_DrawScene", v_SceneView_DrawScene);
		LogFunAdr("SceneView_AllocFrameMRTs", v_SceneView_AllocFrameMRTs);
		LogFunAdr("SceneView_ResolveMain", v_SceneView_ResolveMain);
		LogFunAdr("SceneView_BuildSetup", v_SceneView_BuildSetup);
		LogFunAdr("SceneView_StartRenderLists", v_SceneView_StartRenderLists);
		LogFunAdr("SceneView_ApplyJitter", v_SceneView_ApplyJitter);
		LogFunAdr("SceneView_InitSubsystems", v_SceneView_InitSubsystems);
		LogFunAdr("SceneView_MonitorLights", v_SceneView_MonitorLights);
		LogFunAdr("SceneView_CreateRenderTarget", v_SceneView_CreateRenderTarget);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const;
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // CLIENT_SCENEVIEW_H
