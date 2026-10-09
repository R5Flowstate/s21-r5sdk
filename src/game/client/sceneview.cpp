//=============================================================================//
//
// Purpose: Full-resolution scene views (see sceneview.h). Setup runs after the
// engine's monitors are set up, drawing runs where those monitors draw
// (before the main scene, after shadow depth), and the output is copied into
// per-view full-frame targets _rt_SceneView<N> / _rt_SceneViewMV<N>.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/client/sceneview.h"
#include "game/client/cubemap_capture.h"
#include <cmath>
#include <malloc.h>

static constexpr int SCENEVIEW_MAX_SLOTS = 4;

// CViewRender (S21 client).
static constexpr ptrdiff_t VIEWRENDER_MAIN_LOGVIEW = 0x81700;
static constexpr ptrdiff_t VIEWRENDER_WORLDTOCLIP_MRU0 = 0x11A350;
static constexpr ptrdiff_t VIEWRENDER_WORLDTOCLIP_MRU1 = 0x11A358;
static constexpr ptrdiff_t VIEWRENDER_HAS_3D_SKY = 0x11A375;
static constexpr ptrdiff_t VIEWRENDER_SKY_SETUP = 0x11A380;

// Monitor view block: a logical view (world + sky single views) plus the
// monitor tail. Same size and layout as the engine's m_monitors[] entries.
static constexpr size_t MONITORVIEW_SIZE = 0x2B1C0;
static constexpr ptrdiff_t MONITORVIEW_SKY_SINGLEVIEW = 0x15880;
static constexpr ptrdiff_t MONITORVIEW_SKY_SETUP = 0x2B140;
static constexpr ptrdiff_t MONITORVIEW_TSAA_ENABLED = 0x2B149;
static constexpr ptrdiff_t MONITORVIEW_CAMERA_ENT = 0x2B180;
static constexpr ptrdiff_t MONITORVIEW_PVS_JOB = 0x2B188;

// Render view setup, the first member of a single view.
static constexpr size_t RENDERVIEWSETUP_SIZE = 0x200;
static constexpr ptrdiff_t SETUP_ORIGIN = 0x0;
static constexpr ptrdiff_t SETUP_FORWARD = 0x10;
static constexpr ptrdiff_t SETUP_WORLD_TO_VIEW = 0x40;
static constexpr ptrdiff_t SETUP_VIEW_TO_PROJ = 0x80;
static constexpr ptrdiff_t SETUP_WORLD_TO_PROJ = 0xC0;
static constexpr ptrdiff_t SETUP_WORLD_TO_VIEW_PREV = 0x100;
static constexpr ptrdiff_t SETUP_VIEW_TO_PROJ_PREV = 0x140;
static constexpr ptrdiff_t SETUP_ORIGIN_PREV = 0x180;
static constexpr ptrdiff_t SETUP_TAN_HALF_FOV_X = 0x190;
static constexpr ptrdiff_t SETUP_TAN_HALF_FOV_Y = 0x194;
static constexpr ptrdiff_t SETUP_ZNEAR = 0x198;
static constexpr ptrdiff_t SETUP_ZFAR = 0x19C;
static constexpr ptrdiff_t SETUP_RECT = 0x1A0;
static constexpr ptrdiff_t SETUP_VIEW_ID = 0x1D0;
// The setup builder mirrors the camera origin into the PVS seed at +0x1B0;
// the visibility walk captures its start cell from those bytes.
static constexpr ptrdiff_t SETUP_SEED_ORIGIN_X = 0x1B0;
static constexpr ptrdiff_t SETUP_SEED_ORIGIN_Y = 0x1B4;
static constexpr ptrdiff_t SETUP_SEED_ORIGIN_Z = 0x1B8;
// World single view: the render list follows the setup; its first dword is canSeeSky.
static constexpr ptrdiff_t SINGLEVIEW_CAN_SEE_SKY = 0x200;

// Material system / render context vtable byte offsets (S21 DX12).
static constexpr ptrdiff_t MATSYS_GET_RENDERER_CONFIG = 0x188;
static constexpr ptrdiff_t MATSYS_GET_MAIN_BUFFER_FORMAT = 0x1E0;
static constexpr ptrdiff_t MATSYS_FIND_TEXTURE = 0x328;
static constexpr ptrdiff_t MATSYS_GET_RENDER_CONTEXT = 0x390;
static constexpr ptrdiff_t CTX_RELEASE = 0x8;
static constexpr ptrdiff_t CTX_SET_RENDER_TARGET = 0x48;
static constexpr ptrdiff_t CTX_GET_RENDER_TARGET = 0x78;
static constexpr ptrdiff_t CTX_CLEAR_BUFFERS = 0x150;
static constexpr ptrdiff_t CTX_SET_FB_COPY_TEXTURE = 0x1F8;
static constexpr ptrdiff_t CTX_CLEAR_COLOR_4F = 0x3E0;
static constexpr ptrdiff_t CTX_COPY_RT_TO_TEXTURE_EX = 0x4D8;
static constexpr ptrdiff_t TEXTURE_SET_REFCOUNT = 0x60;

static constexpr ptrdiff_t RENDERER_CONFIG_FLAGS = 0x3C;
static constexpr ptrdiff_t RENDERER_CONFIG_AA_MODE = 0x40;
static constexpr uint8_t AA_MODE_TSAA = 12;

// Texture creation arguments copied from the engine's _rt_MRT2 allocation.
static constexpr int RT_SIZE_FULL_FRAME = 3;
static constexpr int RT_SIZE_RENDER_RES = 16;
static constexpr int RT_TYPE_NO_DEPTH = 2;
static constexpr uint32_t RT_FLAGS_MRT = 0x10000C;
static constexpr int RT_FORMAT_UV88 = 22;

struct SceneViewRect_t
{
	int x, y, width, height;
};

struct SceneView_t
{
	uint8_t* pMonitorView;
	uintptr_t pColor;
	uintptr_t pMotion;
	uint32_t nSetupFrame;
	uint32_t nDrawnFrame;
	bool bLoggedFirstDraw;
	float storedPrevW2V[16];
	float storedPrevV2P[16];
	float storedPrevOrigin[4];
	bool bHistoryValid;
	bool bSeedApplied;
	float visStartLog[3];
	bool bClipApplied;
	float clipALog;
};

struct SceneViewRequest_t
{
	uint32_t nRequestFrame;
	bool bEverRequested;
	float origin[3];
	float angles[3];
	float tanHalfFovX;
	bool bHasClip;
	float clipPlane[4];
	bool bHasRect;
	float rect[4][3];
	bool bFitRect;
	float visStart[3];
	bool bHasVisStart;
};

static SceneView_t s_views[SCENEVIEW_MAX_SLOTS];
static SceneViewRequest_t s_requests[SCENEVIEW_MAX_SLOTS];
static uint32_t s_nFrame = 0;
static uintptr_t s_pViewRender = 0;
static int s_nLastTestMode = -1;

static uintptr_t* s_ppMaterials = nullptr;
static uint8_t* s_pFogParams = nullptr;
static uint8_t* s_pFogParamsValid = nullptr;
static uintptr_t* s_ppUseMonitors = nullptr;
static float* s_pLastMonitorWorldToProj = nullptr;
static uint8_t* s_pDrawGate = nullptr;
static bool s_bDrawGatePatched = false;

static uint32_t s_nResolveMainCalls = 0;
static uint32_t s_nResolveMainTsaa = 0;
static uint32_t s_nResolveMainCopies = 0;
static uintptr_t s_pResolveMainColor = 0;

// test r8d,r8d / jz +0x1C ... cmp [use_monitors],0 / jz +0x0F: the two
// short jumps that skip DrawMonitors when no retail monitor is live.
static constexpr ptrdiff_t DRAWGATE_JZ_COUNT = 0x0A;
static constexpr ptrdiff_t DRAWGATE_JZ_CONVAR = 0x17;

static ConVar sceneview_test("sceneview_test", "0", FCVAR_DEVELOPMENTONLY,
	"Drive scene view slot 0 for testing: 0 = off, 1 = main camera (identity), "
	"2 = main camera raised 128 units, 3 = main camera pushed 256 units forward.",
	true, 0.f, true, 3.f);
static ConVar sceneview_show("sceneview_show", "-1", FCVAR_DEVELOPMENTONLY,
	"Show scene view slot N on screen in place of the main view's resolved color (-1 = off).",
	true, -1.f, true, static_cast<float>(SCENEVIEW_MAX_SLOTS - 1));
static ConVar sceneview_debug("sceneview_debug", "0", FCVAR_DEVELOPMENTONLY,
	"Log [SCENEVIEW] per-slot state once a second.");
static ConVar sceneview_main_path("sceneview_main_path", "0", FCVAR_DEVELOPMENTONLY,
	"Draw scene views through the main-view scene path (clustered light cull, sky) instead of the "
	"monitor path (flat light list). 0 = monitor path.");
static ConVar sceneview_seed("sceneview_seed", "1", FCVAR_DEVELOPMENTONLY,
	"Start the view's visibility walk from the exit-side seed point instead of the camera "
	"origin. 0 = camera origin (A/B).", true, 0.f, true, 1.f);
static ConVar sceneview_fit_flip("sceneview_fit_flip", "0", FCVAR_DEVELOPMENTONLY,
	"Mirror the fitted portal view: bit 0 = horizontal, bit 1 = vertical.");
static ConVar sceneview_clip_mode("sceneview_clip_mode", "1", FCVAR_DEVELOPMENTONLY,
	"Near clip for requested views with a clip plane: 0 = plain near plane at the exit "
	"distance, 1 = oblique reversed-Z projection.", true, 0.f, true, 1.f);

//-----------------------------------------------------------------------------
// View requests. Requests are per-frame: a slot draws only while its
// request is from this frame or the one before, so a closed portal stops
// drawing without an explicit destroy. At most SCENEVIEW_MAX_ACTIVE slots.
//-----------------------------------------------------------------------------
// Per-frame refusals warn at most once every 300 frames per call site.
static bool SceneView_ShouldWarn(uint32_t& nLastFrame)
{
	if (nLastFrame != 0 && s_nFrame - nLastFrame < 300)
		return false;
	nLastFrame = s_nFrame ? s_nFrame : 1;
	return true;
}

static bool SceneView_Finite(const float* const p, const int n)
{
	for (int i = 0; i < n; i++)
	{
		if (!isfinite(p[i]))
			return false;
	}
	return true;
}

static void SceneView_ForwardFromAngles(const float* const pAngles, float* const pForward)
{
	const float pitch = pAngles[0] * (3.14159265f / 180.0f);
	const float yaw = pAngles[1] * (3.14159265f / 180.0f);
	const float cp = cosf(pitch);
	pForward[0] = cp * cosf(yaw);
	pForward[1] = cp * sinf(yaw);
	pForward[2] = -sinf(pitch);
}

void SceneView_Request(const int slot, const float* const pOrigin, const float* const pAngles,
	const float tanHalfFovX, const float* const pClipPlane, const float (* const pPortalRect)[3],
	const bool bFitRect)
{
	if (slot < 0 || slot >= SCENEVIEW_MAX_SLOTS || !pOrigin || !pAngles)
		return;
	if (!SceneView_Finite(pOrigin, 3) || !SceneView_Finite(pAngles, 3) || !isfinite(tanHalfFovX)
		|| tanHalfFovX <= 0.0f || tanHalfFovX > 4.0f)
	{
		static uint32_t s_nWarnCamera = 0;
		if (SceneView_ShouldWarn(s_nWarnCamera))
			Warning(eDLL_T::CLIENT, "[SCENEVIEW] slot %d request refused (bad camera)\n", slot);
		return;
	}
	if (pClipPlane && !SceneView_Finite(pClipPlane, 4))
	{
		static uint32_t s_nWarnClip = 0;
		if (SceneView_ShouldWarn(s_nWarnClip))
			Warning(eDLL_T::CLIENT, "[SCENEVIEW] slot %d request refused (bad clip plane)\n", slot);
		return;
	}

	int nActive = 0;
	for (int i = 0; i < SCENEVIEW_MAX_SLOTS; i++)
	{
		if (i != slot && s_requests[i].bEverRequested && s_nFrame - s_requests[i].nRequestFrame < 2)
			nActive++;
	}
	if (nActive >= SCENEVIEW_MAX_ACTIVE)
	{
		static uint32_t s_nWarnCap = 0;
		if (SceneView_ShouldWarn(s_nWarnCap))
			Warning(eDLL_T::CLIENT, "[SCENEVIEW] slot %d request refused (%d views already active)\n",
				slot, nActive);
		return;
	}

	SceneViewRequest_t& req = s_requests[slot];
	req.nRequestFrame = s_nFrame;
	req.bEverRequested = true;
	memcpy(req.origin, pOrigin, 3 * sizeof(float));
	memcpy(req.angles, pAngles, 3 * sizeof(float));
	req.tanHalfFovX = tanHalfFovX;
	req.bHasClip = pClipPlane != nullptr;
	if (pClipPlane)
		memcpy(req.clipPlane, pClipPlane, 4 * sizeof(float));
	req.bHasRect = pPortalRect != nullptr;
	req.bFitRect = bFitRect && pPortalRect != nullptr;
	if (pPortalRect)
		memcpy(req.rect, pPortalRect, 4 * 3 * sizeof(float));

	if (req.bHasRect)
	{
		float e0[3] = { req.rect[1][0] - req.rect[0][0], req.rect[1][1] - req.rect[0][1], req.rect[1][2] - req.rect[0][2] };
		float e1[3] = { req.rect[3][0] - req.rect[0][0], req.rect[3][1] - req.rect[0][1], req.rect[3][2] - req.rect[0][2] };
		float n[3] = { e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2], e0[0] * e1[1] - e0[1] * e1[0] };
		const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (len > 1e-6f)
		{
			req.visStart[0] = (req.rect[0][0] + req.rect[1][0] + req.rect[2][0] + req.rect[3][0]) * 0.25f + n[0] / len;
			req.visStart[1] = (req.rect[0][1] + req.rect[1][1] + req.rect[2][1] + req.rect[3][1]) * 0.25f + n[1] / len;
			req.visStart[2] = (req.rect[0][2] + req.rect[1][2] + req.rect[2][2] + req.rect[3][2]) * 0.25f + n[2] / len;
			req.bHasVisStart = true;
		}
		else
		{
			req.bHasVisStart = false;
		}
	}
	else
	{
		float fwd[3];
		SceneView_ForwardFromAngles(pAngles, fwd);
		req.visStart[0] = pOrigin[0] + fwd[0];
		req.visStart[1] = pOrigin[1] + fwd[1];
		req.visStart[2] = pOrigin[2] + fwd[2];
		req.bHasVisStart = true;
	}
}

void SceneView_InvalidateHistory(const int slot)
{
	if (slot < 0 || slot >= SCENEVIEW_MAX_SLOTS)
		return;
	s_views[slot].bHistoryValid = false;
}

//-----------------------------------------------------------------------------
// Read accessors for the portal surface renderer.
//-----------------------------------------------------------------------------
static SceneViewRequestProviderFn s_pfnRequestProvider = nullptr;

void SceneView_SetRequestProvider(SceneViewRequestProviderFn fn)
{
	s_pfnRequestProvider = fn;
}

bool SceneView_GetMainCamera(float* const pOrigin, float* const pForward, float* const pRight,
	float* const pUp, float* const pWorldToProj, float* const pTanHalfFovX)
{
	if (!s_pViewRender || !s_nFrame)
		return false;
	const uint8_t* const mainSetup = reinterpret_cast<const uint8_t*>(s_pViewRender + VIEWRENDER_MAIN_LOGVIEW);
	if (pOrigin)
		memcpy(pOrigin, mainSetup + SETUP_ORIGIN, 3 * sizeof(float));
	if (pForward)
		memcpy(pForward, mainSetup + SETUP_FORWARD, 3 * sizeof(float));
	if (pRight)
		memcpy(pRight, mainSetup + SETUP_ORIGIN + 0x20, 3 * sizeof(float));
	if (pUp)
		memcpy(pUp, mainSetup + SETUP_ORIGIN + 0x30, 3 * sizeof(float));
	if (pWorldToProj)
		memcpy(pWorldToProj, mainSetup + SETUP_WORLD_TO_PROJ, 16 * sizeof(float));
	if (pTanHalfFovX)
		*pTanHalfFovX = *reinterpret_cast<const float*>(mainSetup + SETUP_TAN_HALF_FOV_X);
	return true;
}

uintptr_t SceneView_SlotColorTexture(const int slot)
{
	if (slot < 0 || slot >= SCENEVIEW_MAX_SLOTS || !s_views[slot].nDrawnFrame)
		return 0;
	return s_views[slot].pColor;
}

uintptr_t SceneView_MainLogView(void)
{
	return s_pViewRender ? s_pViewRender + VIEWRENDER_MAIN_LOGVIEW : 0;
}

bool SceneView_SlotDrawn(const int slot)
{
	if (slot < 0 || slot >= SCENEVIEW_MAX_SLOTS || !s_nFrame)
		return false;
	return s_views[slot].nDrawnFrame == s_nFrame;
}

bool SceneView_IsOwnView(const uintptr_t logView)
{
	if (!logView)
		return false;
	for (int i = 0; i < SCENEVIEW_MAX_SLOTS; i++)
	{
		if (s_views[i].pMonitorView && reinterpret_cast<uintptr_t>(s_views[i].pMonitorView) == logView)
			return true;
	}
	return false;
}

static void SceneView_Multiply4x4(const float* const pA, const float* const pB, float* const pOut)
{
	for (int i = 0; i < 4; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			pOut[i * 4 + j] = pA[i * 4] * pB[j] + pA[i * 4 + 1] * pB[4 + j]
				+ pA[i * 4 + 2] * pB[8 + j] + pA[i * 4 + 3] * pB[12 + j];
		}
	}
}

//-----------------------------------------------------------------------------
// Engine interface helpers
//-----------------------------------------------------------------------------
template <typename T>
static T SceneView_VFunc(const uintptr_t obj, const ptrdiff_t byteOffset)
{
	const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(obj);
	return *reinterpret_cast<const T*>(vtable + byteOffset);
}

static uintptr_t SceneView_Materials(void)
{
	return s_ppMaterials ? *s_ppMaterials : 0;
}

uintptr_t SceneView_MaterialSystem(void)
{
	return SceneView_Materials();
}

bool SceneView_GetMainProjection(float* const pTanHalfFovX, float* const pTanHalfFovY, int* const pRect)
{
	if (!s_pViewRender || !s_nFrame)
		return false;
	const uint8_t* const mainSetup = reinterpret_cast<const uint8_t*>(s_pViewRender + VIEWRENDER_MAIN_LOGVIEW);
	if (pTanHalfFovX)
		*pTanHalfFovX = *reinterpret_cast<const float*>(mainSetup + SETUP_TAN_HALF_FOV_X);
	if (pTanHalfFovY)
		*pTanHalfFovY = *reinterpret_cast<const float*>(mainSetup + SETUP_TAN_HALF_FOV_Y);
	if (pRect)
		memcpy(pRect, mainSetup + SETUP_RECT, sizeof(SceneViewRect_t));
	return true;
}

static uintptr_t SceneView_FindTexture(const char* const pszName)
{
	const uintptr_t mat = SceneView_Materials();
	if (!mat)
		return 0;
	return SceneView_VFunc<uintptr_t(__fastcall*)(uintptr_t, const char*, bool)>(mat, MATSYS_FIND_TEXTURE)(mat, pszName, true);
}

static uintptr_t SceneView_RendererConfig(void)
{
	const uintptr_t mat = SceneView_Materials();
	if (!mat)
		return 0;
	return SceneView_VFunc<uintptr_t(__fastcall*)(uintptr_t)>(mat, MATSYS_GET_RENDERER_CONFIG)(mat);
}

static bool SceneView_MainIsTSAA(void)
{
	const uintptr_t cfg = SceneView_RendererConfig();
	return cfg && *reinterpret_cast<const uint8_t*>(cfg + RENDERER_CONFIG_AA_MODE) == AA_MODE_TSAA;
}

static void SceneView_SetRenderTarget(const uintptr_t ctx, const uintptr_t tex)
{
	SceneView_VFunc<void(__fastcall*)(uintptr_t, uintptr_t)>(ctx, CTX_SET_RENDER_TARGET)(ctx, tex);
}

static void SceneView_CopyRenderTarget(const uintptr_t ctx, const uintptr_t dstTex, const SceneViewRect_t& rect)
{
	SceneViewRect_t src = rect;
	SceneViewRect_t dst = rect;
	SceneView_VFunc<void(__fastcall*)(uintptr_t, uintptr_t, int, SceneViewRect_t*, SceneViewRect_t*)>(
		ctx, CTX_COPY_RT_TO_TEXTURE_EX)(ctx, dstTex, 0, &src, &dst);
}

static void SceneView_ClearColor(const uintptr_t ctx, const float r, const float g, const float b, const float a)
{
	SceneView_VFunc<void(__fastcall*)(uintptr_t, float, float, float, float)>(ctx, CTX_CLEAR_COLOR_4F)(ctx, r, g, b, a);
}

static void SceneView_ClearColorBuffer(const uintptr_t ctx)
{
	SceneView_VFunc<void(__fastcall*)(uintptr_t, bool, bool, bool)>(ctx, CTX_CLEAR_BUFFERS)(ctx, true, false, false);
}

static bool SceneView_IsReady(const SceneView_t& view)
{
	return view.pMonitorView && view.pColor && view.pMotion;
}

//-----------------------------------------------------------------------------
// Render targets: created next to the engine's frame buffer MRTs, with the
// same size mode, so they follow every resolution change.
//-----------------------------------------------------------------------------
static void SceneView_CreateTargets(const uintptr_t rendererConfig)
{
	const uintptr_t mat = SceneView_Materials();
	if (!v_SceneView_CreateRenderTarget || !mat || !rendererConfig)
		return;

	const int sizeMode = (*reinterpret_cast<const uint8_t*>(rendererConfig + RENDERER_CONFIG_FLAGS) & 0x10)
		? RT_SIZE_RENDER_RES : RT_SIZE_FULL_FRAME;
	const int colorFormat = SceneView_VFunc<int(__fastcall*)(uintptr_t)>(mat, MATSYS_GET_MAIN_BUFFER_FORMAT)(mat);

	for (int i = 0; i < SCENEVIEW_MAX_SLOTS; i++)
	{
		char szColor[32];
		char szMotion[32];
		snprintf(szColor, sizeof(szColor), "_rt_SceneView%d", i);
		snprintf(szMotion, sizeof(szMotion), "_rt_SceneViewMV%d", i);

		SceneView_t& view = s_views[i];
		view.pColor = v_SceneView_CreateRenderTarget(szColor, 0, 0, sizeMode, colorFormat, RT_TYPE_NO_DEPTH, RT_FLAGS_MRT, 24);
		view.pMotion = v_SceneView_CreateRenderTarget(szMotion, 0, 0, sizeMode, RT_FORMAT_UV88, RT_TYPE_NO_DEPTH, RT_FLAGS_MRT, 24);

		if (view.pColor)
			SceneView_VFunc<void(__fastcall*)(uintptr_t, int)>(view.pColor, TEXTURE_SET_REFCOUNT)(view.pColor, -1);
		if (view.pMotion)
			SceneView_VFunc<void(__fastcall*)(uintptr_t, int)>(view.pMotion, TEXTURE_SET_REFCOUNT)(view.pMotion, -1);

		if (!view.pColor || !view.pMotion)
			Warning(eDLL_T::CLIENT, "[SCENEVIEW] slot %d render targets failed to create\n", i);
	}

	Msg(eDLL_T::CLIENT, "[SCENEVIEW] %d view targets (size mode %d, color format %d)\n",
		SCENEVIEW_MAX_SLOTS, sizeMode, colorFormat);
}

static int64_t Hook_SceneView_AllocFrameMRTs(const uintptr_t rendererConfig)
{
	const int64_t result = v_SceneView_AllocFrameMRTs(rendererConfig);
	SceneView_CreateTargets(rendererConfig);
	return result;
}

//-----------------------------------------------------------------------------
// Setup
//-----------------------------------------------------------------------------
static uint8_t* SceneView_EnsureMonitorView(SceneView_t& view)
{
	if (!view.pMonitorView)
	{
		view.pMonitorView = static_cast<uint8_t*>(_aligned_malloc(MONITORVIEW_SIZE, 64));
		if (view.pMonitorView)
			memset(view.pMonitorView, 0, MONITORVIEW_SIZE);
	}
	return view.pMonitorView;
}

static void SceneView_PatchDrawGate(const bool bOpen)
{
	if (!s_pDrawGate || s_bDrawGatePatched == bOpen)
		return;

	uint8_t* const pJzCount = s_pDrawGate + DRAWGATE_JZ_COUNT;
	uint8_t* const pJzConVar = s_pDrawGate + DRAWGATE_JZ_CONVAR;
	const uint8_t want[2][2] = { { 0x74, 0x1C }, { 0x74, 0x0F } };

	DWORD oldProt = 0;
	if (!VirtualProtect(s_pDrawGate, 0x20, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::CLIENT, "[SCENEVIEW] VirtualProtect failed, draw gate untouched\n");
		return;
	}
	if (bOpen)
	{
		pJzCount[0] = 0x90; pJzCount[1] = 0x90;
		pJzConVar[0] = 0x90; pJzConVar[1] = 0x90;
	}
	else
	{
		pJzCount[0] = want[0][0]; pJzCount[1] = want[0][1];
		pJzConVar[0] = want[1][0]; pJzConVar[1] = want[1][1];
	}
	VirtualProtect(s_pDrawGate, 0x20, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), s_pDrawGate, 0x20);
	s_bDrawGatePatched = bOpen;
}

static void SceneView_AnglesFromForward(const float* const pForward, float* const pAngles)
{
	const float fx = pForward[0];
	const float fy = pForward[1];
	const float fz = pForward[2];
	pAngles[0] = -asinf(fz < -1.0f ? -1.0f : (fz > 1.0f ? 1.0f : fz)) * (180.0f / 3.14159265f);
	pAngles[1] = atan2f(fy, fx) * (180.0f / 3.14159265f);
	pAngles[2] = 0.0f;
}

static void SceneView_SetupTestView(const uintptr_t viewRender, SceneView_t& view, const int mode,
	float* const pVisStart, bool* const pHasVisStart)
{
	uint8_t* const mv = view.pMonitorView;
	const uint8_t* const mainSetup = reinterpret_cast<const uint8_t*>(viewRender + VIEWRENDER_MAIN_LOGVIEW);

	if (mode == 1)
	{
		memcpy(mv, mainSetup, RENDERVIEWSETUP_SIZE);
		const float* const mainOrigin = reinterpret_cast<const float*>(mainSetup + SETUP_ORIGIN);
		float angles[3];
		SceneView_AnglesFromForward(reinterpret_cast<const float*>(mainSetup + SETUP_FORWARD), angles);
		float fwd[3];
		SceneView_ForwardFromAngles(angles, fwd);
		pVisStart[0] = mainOrigin[0] + fwd[0];
		pVisStart[1] = mainOrigin[1] + fwd[1];
		pVisStart[2] = mainOrigin[2] + fwd[2];
		*pHasVisStart = true;
		return;
	}

	const float* const mainOrigin = reinterpret_cast<const float*>(mainSetup + SETUP_ORIGIN);
	const float* const mainFwd = reinterpret_cast<const float*>(mainSetup + SETUP_FORWARD);
	float origin[3];
	if (mode == 3)
	{
		origin[0] = mainOrigin[0] + mainFwd[0] * 256.0f;
		origin[1] = mainOrigin[1] + mainFwd[1] * 256.0f;
		origin[2] = mainOrigin[2] + mainFwd[2] * 256.0f;
	}
	else
	{
		origin[0] = mainOrigin[0];
		origin[1] = mainOrigin[1];
		origin[2] = mainOrigin[2] + 128.0f;
	}
	float angles[3];
	SceneView_AnglesFromForward(mainFwd, angles);

	v_SceneView_BuildSetup(reinterpret_cast<uintptr_t>(mv), 0, origin, angles,
		*reinterpret_cast<const float*>(mainSetup + SETUP_TAN_HALF_FOV_X),
		*reinterpret_cast<const float*>(mainSetup + SETUP_ZNEAR),
		*reinterpret_cast<const float*>(mainSetup + SETUP_ZFAR),
		reinterpret_cast<const int*>(mainSetup + SETUP_RECT));

	float fwd[3];
	SceneView_ForwardFromAngles(angles, fwd);
	pVisStart[0] = origin[0] + fwd[0];
	pVisStart[1] = origin[1] + fwd[1];
	pVisStart[2] = origin[2] + fwd[2];
	*pHasVisStart = true;
}

// The visibility walk captures its start cell from the origin the builder
// mirrors into the seed bytes, so an exit-side point keeps an embedded camera
// in a valid cell while the frustum matrices stay true-camera.
static void SceneView_ApplySeed(SceneView_t& view, uint8_t* const mv, const float* const pVisStart,
	const bool bHasVisStart)
{
	view.bSeedApplied = false;
	if (!bHasVisStart || !pVisStart || !sceneview_seed.GetBool())
		return;
	memcpy(mv + SETUP_ORIGIN, pVisStart, 3 * sizeof(float));
	memcpy(mv + SETUP_SEED_ORIGIN_X, pVisStart, 3 * sizeof(float));
	view.bSeedApplied = true;
	memcpy(view.visStartLog, pVisStart, 3 * sizeof(float));
}

// Exit-plane near clip. Mode 0 rebuilds with zNear at the exit distance;
// mode 1 folds the plane into the projection's z row (reversed-Z oblique).
static void SceneView_ApplyClip(SceneView_t& view, uint8_t* const mv, const SceneViewRequest_t& req)
{
	view.bClipApplied = false;
	if (!req.bHasClip)
		return;

	const float* const n = req.clipPlane;
	const float* const cam = req.origin;
	const float dist = n[0] * cam[0] + n[1] * cam[1] + n[2] * cam[2] + req.clipPlane[3];

	if (sceneview_clip_mode.GetInt() == 0)
	{
		const float zNear = dist - 1.0f > 1.0f ? dist - 1.0f : 1.0f;
		float tanX = *reinterpret_cast<const float*>(mv + SETUP_TAN_HALF_FOV_X);
		v_SceneView_BuildSetup(reinterpret_cast<uintptr_t>(mv), 0, cam, req.angles, tanX,
			zNear, *reinterpret_cast<const float*>(mv + SETUP_ZFAR),
			reinterpret_cast<const int*>(mv + SETUP_RECT));
		view.bClipApplied = true;
		view.clipALog = 0.0f;
		return;
	}

	float* const w2v = reinterpret_cast<float*>(mv + SETUP_WORLD_TO_VIEW);
	float nv[3] = {
		w2v[0] * n[0] + w2v[4] * n[1] + w2v[8] * n[2],
		w2v[1] * n[0] + w2v[5] * n[1] + w2v[9] * n[2],
		w2v[2] * n[0] + w2v[6] * n[1] + w2v[10] * n[2] };
	const float dv = req.clipPlane[3] - (nv[0] * w2v[12] + nv[1] * w2v[13] + nv[2] * w2v[14]);

	float* const v2p = reinterpret_cast<float*>(mv + SETUP_VIEW_TO_PROJ);
	const float tanX = *reinterpret_cast<const float*>(mv + SETUP_TAN_HALF_FOV_X);
	const float tanY = *reinterpret_cast<const float*>(mv + SETUP_TAN_HALF_FOV_Y);
	float maxOverW = 0.0f;
	for (int i = 0; i < 4; i++)
	{
		const float dx = (i & 1) ? tanX : -tanX;
		const float dy = (i & 2) ? tanY : -tanY;
		const float w = v2p[12] * dx + v2p[13] * dy - v2p[14] + v2p[15];
		const float cp = nv[0] * dx + nv[1] * dy - nv[2] + dv;
		if (w > 1e-6f && cp / w > maxOverW)
			maxOverW = cp / w;
	}
	if (maxOverW <= 1e-6f)
	{
		static uint32_t s_nWarnOblique = 0;
		if (SceneView_ShouldWarn(s_nWarnOblique))
			Warning(eDLL_T::CLIENT, "[SCENEVIEW] oblique clip skipped (plane behind view)\n");
		return;
	}
	const float a = 1.0f / maxOverW;
	float clipRow[4] = { nv[0], nv[1], nv[2], dv };
	for (int j = 0; j < 4; j++)
		v2p[8 + j] = v2p[12 + j] - a * clipRow[j];

	float w2p[16];
	SceneView_Multiply4x4(v2p, w2v, w2p);
	memcpy(mv + SETUP_WORLD_TO_PROJ, w2p, 16 * sizeof(float));
	view.bClipApplied = true;
	view.clipALog = a;
}

// Scales and offsets the x/y rows so the requested rect's projection spans
// the whole image. The culling frustum stays the wider symmetric one.
static void SceneView_FitRect(uint8_t* const mv, const SceneViewRequest_t& req)
{
	const float* const w2p = reinterpret_cast<const float*>(mv + SETUP_WORLD_TO_PROJ);
	float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
	for (int i = 0; i < 4; i++)
	{
		const float* const p = req.rect[i];
		const float cx = w2p[0] * p[0] + w2p[1] * p[1] + w2p[2] * p[2] + w2p[3];
		const float cy = w2p[4] * p[0] + w2p[5] * p[1] + w2p[6] * p[2] + w2p[7];
		const float cw = w2p[12] * p[0] + w2p[13] * p[1] + w2p[14] * p[2] + w2p[15];
		if (cw <= 1e-4f)
			return;
		const float x = cx / cw, y = cy / cw;
		minX = x < minX ? x : minX; maxX = x > maxX ? x : maxX;
		minY = y < minY ? y : minY; maxY = y > maxY ? y : maxY;
	}
	float sx = (maxX - minX) * 0.5f, sy = (maxY - minY) * 0.5f;
	if (sx < 1e-5f || sy < 1e-5f)
		return;
	const float ox = (maxX + minX) * 0.5f, oy = (maxY + minY) * 0.5f;
	const int flip = sceneview_fit_flip.GetInt();
	if (flip & 1)
		sx = -sx;
	if (flip & 2)
		sy = -sy;

	float* const v2p = reinterpret_cast<float*>(mv + SETUP_VIEW_TO_PROJ);
	for (int j = 0; j < 4; j++)
	{
		v2p[0 + j] = (v2p[0 + j] - ox * v2p[12 + j]) / sx;
		v2p[4 + j] = (v2p[4 + j] - oy * v2p[12 + j]) / sy;
	}
	float out[16];
	SceneView_Multiply4x4(v2p, reinterpret_cast<const float*>(mv + SETUP_WORLD_TO_VIEW), out);
	memcpy(mv + SETUP_WORLD_TO_PROJ, out, sizeof(out));
}

static void SceneView_WriteHistory(SceneView_t& view, uint8_t* const mv)
{
	if (view.bHistoryValid)
	{
		memcpy(mv + SETUP_WORLD_TO_VIEW_PREV, view.storedPrevW2V, 16 * sizeof(float));
		memcpy(mv + SETUP_VIEW_TO_PROJ_PREV, view.storedPrevV2P, 16 * sizeof(float));
		memcpy(mv + SETUP_ORIGIN_PREV, view.storedPrevOrigin, 4 * sizeof(float));
	}
	else
	{
		memcpy(mv + SETUP_WORLD_TO_VIEW_PREV, mv + SETUP_WORLD_TO_VIEW, 16 * sizeof(float));
		memcpy(mv + SETUP_VIEW_TO_PROJ_PREV, mv + SETUP_VIEW_TO_PROJ, 16 * sizeof(float));
		memcpy(mv + SETUP_ORIGIN_PREV, mv + SETUP_ORIGIN, 4 * sizeof(float));
	}
	memcpy(view.storedPrevW2V, mv + SETUP_WORLD_TO_VIEW, 16 * sizeof(float));
	memcpy(view.storedPrevV2P, mv + SETUP_VIEW_TO_PROJ, 16 * sizeof(float));
	memcpy(view.storedPrevOrigin, mv + SETUP_ORIGIN, 4 * sizeof(float));
}

static bool SceneView_SetupSlot(const uintptr_t viewRender, const int slot, const bool bTest,
	const int testMode, const SceneViewRequest_t* const pReq)
{
	SceneView_t& view = s_views[slot];
	if (s_nFrame - view.nSetupFrame > 2)
		view.bHistoryValid = false;
	static bool s_bLazyCreateTried = false;
	if ((!view.pColor || !view.pMotion) && !s_bLazyCreateTried)
	{
		s_bLazyCreateTried = true;
		SceneView_CreateTargets(SceneView_RendererConfig());
	}
	if (!view.pColor || !view.pMotion || !SceneView_EnsureMonitorView(view))
		return false;

	uint8_t* const mv = view.pMonitorView;
	float visStart[3] = { 0.0f, 0.0f, 0.0f };
	bool bHasVisStart = false;

	if (bTest)
	{
		SceneView_SetupTestView(viewRender, view, testMode, visStart, &bHasVisStart);
	}
	else
	{
		const uint8_t* const mainSetup = reinterpret_cast<const uint8_t*>(viewRender + VIEWRENDER_MAIN_LOGVIEW);
		v_SceneView_BuildSetup(reinterpret_cast<uintptr_t>(mv), 0, pReq->origin, pReq->angles,
			pReq->bFitRect ? pReq->tanHalfFovX : *reinterpret_cast<const float*>(mainSetup + SETUP_TAN_HALF_FOV_X),
			*reinterpret_cast<const float*>(mainSetup + SETUP_ZNEAR),
			*reinterpret_cast<const float*>(mainSetup + SETUP_ZFAR),
			reinterpret_cast<const int*>(mainSetup + SETUP_RECT));
		SceneViewRequest_t& req = s_requests[slot];
		SceneView_ApplyClip(view, mv, req);
		if (req.bFitRect)
			SceneView_FitRect(mv, req);
		if (req.bHasVisStart)
		{
			memcpy(visStart, req.visStart, 3 * sizeof(float));
			bHasVisStart = true;
		}
	}

	SceneView_ApplySeed(view, mv, visStart, bHasVisStart);
	SceneView_WriteHistory(view, mv);

	const bool bHas3DSky = *reinterpret_cast<const uint8_t*>(viewRender + VIEWRENDER_HAS_3D_SKY) != 0;
	*reinterpret_cast<uintptr_t*>(mv + MONITORVIEW_SKY_SETUP) = bHas3DSky ? viewRender + VIEWRENDER_SKY_SETUP : 0;
	*reinterpret_cast<uint8_t*>(mv + MONITORVIEW_TSAA_ENABLED) = SceneView_MainIsTSAA() ? 1 : 0;
	*reinterpret_cast<uintptr_t*>(mv + MONITORVIEW_CAMERA_ENT) = 0;
	*reinterpret_cast<uint32_t*>(mv + MONITORVIEW_PVS_JOB) =
		v_SceneView_StartRenderLists(reinterpret_cast<uintptr_t>(mv), reinterpret_cast<uintptr_t>(mv));

	view.nSetupFrame = s_nFrame;
	return true;
}

static void SceneView_SetupAll(const uintptr_t viewRender)
{
	const int testMode = sceneview_test.GetInt();
	if (testMode != s_nLastTestMode)
	{
		s_views[0].bHistoryValid = false;
		s_views[0].bLoggedFirstDraw = false;
		s_nLastTestMode = testMode;
	}

	if (testMode > 0)
		SceneView_SetupSlot(viewRender, 0, true, testMode, nullptr);

	for (int i = 0; i < SCENEVIEW_MAX_SLOTS; i++)
	{
		if (testMode > 0 && i == 0)
			continue;
		if (!s_requests[i].bEverRequested || s_nFrame - s_requests[i].nRequestFrame >= 2)
			continue;
		SceneView_SetupSlot(viewRender, i, false, 0, &s_requests[i]);
	}
}

static int64_t Hook_SceneView_SetupMonitors(const uintptr_t viewRender, const uintptr_t viewBundle)
{
	const int64_t result = v_SceneView_SetupMonitors(viewRender, viewBundle);

	s_pViewRender = viewRender;
	s_nFrame++;

	// Opened here, on the render thread, only once this hook is known live, so
	// DrawMonitors is never entered with zero monitors without our guard.
	SceneView_PatchDrawGate(true);
	if (s_pfnRequestProvider)
		s_pfnRequestProvider();
	SceneView_SetupAll(viewRender);
	return result;
}

//-----------------------------------------------------------------------------
// Draw
//-----------------------------------------------------------------------------
static void SceneView_DrawOne(const uintptr_t viewRender, const uintptr_t ctx, const int slot)
{
	SceneView_t& view = s_views[slot];
	uint8_t* const mv = view.pMonitorView;
	const uintptr_t mvAddr = reinterpret_cast<uintptr_t>(mv);

	const uintptr_t savedMru0 = *reinterpret_cast<const uintptr_t*>(viewRender + VIEWRENDER_WORLDTOCLIP_MRU0);
	const uintptr_t savedMru1 = *reinterpret_cast<const uintptr_t*>(viewRender + VIEWRENDER_WORLDTOCLIP_MRU1);

	if (s_pLastMonitorWorldToProj)
		memcpy(s_pLastMonitorWorldToProj, mv + SETUP_WORLD_TO_PROJ, 16 * sizeof(float));

	const bool bMainPath = sceneview_main_path.GetBool();
	if (!bMainPath)
		v_SceneView_MonitorLights(mvAddr, true, 0);
	SceneView_VFunc<void(__fastcall*)(uintptr_t, uintptr_t, int)>(ctx, CTX_SET_FB_COPY_TEXTURE)(
		ctx, SceneView_FindTexture("_rt_FullFrameFB"), 0);

	if (*reinterpret_cast<const uint8_t*>(mv + MONITORVIEW_TSAA_ENABLED))
	{
		v_SceneView_ApplyJitter(mvAddr);
		v_SceneView_ApplyJitter(mvAddr + MONITORVIEW_SKY_SINGLEVIEW);
	}
	v_SceneView_InitSubsystems(mvAddr);

	const uintptr_t fog = (s_pFogParamsValid && *s_pFogParamsValid) ? reinterpret_cast<uintptr_t>(s_pFogParams) : 0;
	v_SceneView_DrawScene(viewRender, mvAddr, mvAddr, *reinterpret_cast<const uint32_t*>(mv + MONITORVIEW_PVS_JOB), 0, fog,
		bMainPath ? 0 : 1);
	if (!bMainPath)
		v_SceneView_MonitorLights(mvAddr, false, 0);

	const SceneViewRect_t rect = *reinterpret_cast<const SceneViewRect_t*>(mv + SETUP_RECT);
	const uintptr_t sceneColor = SceneView_VFunc<uintptr_t(__fastcall*)(uintptr_t)>(ctx, CTX_GET_RENDER_TARGET)(ctx);
	SceneView_SetRenderTarget(ctx, sceneColor);
	SceneView_CopyRenderTarget(ctx, view.pColor, rect);

	SceneView_SetRenderTarget(ctx, SceneView_FindTexture("_rt_MRT2"));
	SceneView_CopyRenderTarget(ctx, view.pMotion, rect);
	SceneView_SetRenderTarget(ctx, sceneColor);

	*reinterpret_cast<uintptr_t*>(viewRender + VIEWRENDER_WORLDTOCLIP_MRU0) = savedMru0;
	*reinterpret_cast<uintptr_t*>(viewRender + VIEWRENDER_WORLDTOCLIP_MRU1) = savedMru1;

	view.nDrawnFrame = s_nFrame;
	view.bHistoryValid = true;
	if (!view.bLoggedFirstDraw)
	{
		view.bLoggedFirstDraw = true;
		Msg(eDLL_T::CLIENT, "[SCENEVIEW] slot %d first draw, rect %d,%d %dx%d "
			"viewId=%u canSeeSky=%u skySetup=%p mainPath=%d seed=%d clip=%d\n",
			slot, rect.x, rect.y, rect.width, rect.height,
			*reinterpret_cast<const uint32_t*>(mv + SETUP_VIEW_ID),
			*reinterpret_cast<const uint32_t*>(mv + SINGLEVIEW_CAN_SEE_SKY),
			*reinterpret_cast<void* const*>(mv + MONITORVIEW_SKY_SETUP),
			sceneview_main_path.GetInt(), view.bSeedApplied ? 1 : 0,
			view.bClipApplied ? 1 : 0);
		if (view.bSeedApplied)
			Msg(eDLL_T::CLIENT, "[SCENEVIEW] slot %d visStart %.1f %.1f %.1f\n",
				slot, view.visStartLog[0], view.visStartLog[1], view.visStartLog[2]);
		if (view.bClipApplied)
			Msg(eDLL_T::CLIENT, "[SCENEVIEW] slot %d oblique a=%f\n", slot, view.clipALog);
	}
}

static bool SceneView_ShouldDraw(const int slot)
{
	return SceneView_IsReady(s_views[slot]) && s_views[slot].nSetupFrame == s_nFrame;
}

static void SceneView_DrawAll(const uintptr_t viewRender)
{
	bool bAny = false;
	for (int i = 0; i < SCENEVIEW_MAX_SLOTS; i++)
	{
		if (SceneView_ShouldDraw(i))
			bAny = true;
	}
	if (!bAny)
		return;

	const uintptr_t mat = SceneView_Materials();
	if (!mat)
		return;
	const uintptr_t ctx = SceneView_VFunc<uintptr_t(__fastcall*)(uintptr_t)>(mat, MATSYS_GET_RENDER_CONTEXT)(mat);
	if (!ctx)
		return;

	// Same target prep the retail monitor pass does before its views.
	SceneView_SetRenderTarget(ctx, SceneView_FindTexture("_rt_MRT4"));
	SceneView_ClearColor(ctx, 0.0f, 0.0f, 0.0f, 1.0f);
	SceneView_ClearColorBuffer(ctx);
	SceneView_ClearColor(ctx, 0.0f, -1.0f, 0.0f, 1.0f);
	SceneView_SetRenderTarget(ctx, SceneView_FindTexture("_rt_MRT2"));
	SceneView_ClearColorBuffer(ctx);

	for (int i = 0; i < SCENEVIEW_MAX_SLOTS; i++)
	{
		if (SceneView_ShouldDraw(i))
			SceneView_DrawOne(viewRender, ctx, i);
	}

	SceneView_VFunc<void(__fastcall*)(uintptr_t)>(ctx, CTX_RELEASE)(ctx);
}

static bool SceneView_RetailMonitorsEnabled(void)
{
	if (!s_ppUseMonitors || !*s_ppUseMonitors)
		return false;
	// ConVar parent: int value at +0x64.
	return *reinterpret_cast<const int*>(*s_ppUseMonitors + 0x64) != 0;
}

static int64_t Hook_SceneView_DrawMonitors(const uintptr_t viewRender, const uintptr_t monitors, const int numMonitors)
{
	int64_t result = 0;
	if (numMonitors > 0 && SceneView_RetailMonitorsEnabled())
		result = v_SceneView_DrawMonitors(viewRender, monitors, numMonitors);

	SceneView_DrawAll(viewRender);

	if (sceneview_debug.GetBool())
	{
		static double s_flNextLog = 0.0;
		const double now = Plat_FloatTime();
		if (now >= s_flNextLog)
		{
			s_flNextLog = now + 1.0;
			Msg(eDLL_T::CLIENT, "[SCENEVIEW] main resolve calls=%u tsaa=%u copies=%u resolved=%p show=%d\n",
				s_nResolveMainCalls, s_nResolveMainTsaa, s_nResolveMainCopies,
				reinterpret_cast<void*>(s_pResolveMainColor), sceneview_show.GetInt());
			for (int i = 0; i < SCENEVIEW_MAX_SLOTS; i++)
			{
				const SceneView_t& view = s_views[i];
				if (!view.pMonitorView)
					continue;
				const uint8_t* const mv = view.pMonitorView;
				Msg(eDLL_T::CLIENT, "[SCENEVIEW] slot %d ready=%d setup=%u drawn=%u frame=%u retailMonitors=%d "
					"viewId=%u canSeeSky=%u skySetup=%p mainPath=%d seed=%d clip=%d reqAge=%u\n",
					i, SceneView_IsReady(view) ? 1 : 0, view.nSetupFrame, view.nDrawnFrame, s_nFrame, numMonitors,
					*reinterpret_cast<const uint32_t*>(mv + SETUP_VIEW_ID),
					*reinterpret_cast<const uint32_t*>(mv + SINGLEVIEW_CAN_SEE_SKY),
					*reinterpret_cast<void* const*>(mv + MONITORVIEW_SKY_SETUP),
					sceneview_main_path.GetInt(), view.bSeedApplied ? 1 : 0,
					view.bClipApplied ? 1 : 0, s_nFrame - s_requests[i].nRequestFrame);
			}
		}
	}
	return result;
}

uintptr_t SceneView_MainResolvedColor(void)
{
	return s_pResolveMainColor;
}

//-----------------------------------------------------------------------------
// Debug: overwrite the main view's resolved color with a view's output right
// after the main AA resolve, so post and the screen show the view. The main
// scene itself targets the frame buffer, which is not a texture; the resolve
// output is.
//-----------------------------------------------------------------------------
static int64_t Hook_SceneView_ResolveMain(const SceneViewRect_t* const pRect, uintptr_t* const pResolvedColor,
	uintptr_t* const pResolvedMrt2, int* const pDepthOverride, const int8_t monitorIdx, const uint8_t bTsaa,
	const uint8_t bImplicitRescale)
{
	const int64_t result = v_SceneView_ResolveMain(pRect, pResolvedColor, pResolvedMrt2, pDepthOverride,
		monitorIdx, bTsaa, bImplicitRescale);

	if (monitorIdx == -1)
	{
		s_nResolveMainCalls++;
		s_nResolveMainTsaa += bTsaa ? 1 : 0;
		s_pResolveMainColor = pResolvedColor ? *pResolvedColor : 0;
	}

	const int slot = sceneview_show.GetInt();
	if (monitorIdx != -1 || !bTsaa || !pRect || !pResolvedColor || !*pResolvedColor
		|| slot < 0 || slot >= SCENEVIEW_MAX_SLOTS)
		return result;

	const SceneView_t& view = s_views[slot];
	if (!SceneView_IsReady(view) || view.nDrawnFrame != s_nFrame)
		return result;

	const uintptr_t mat = SceneView_Materials();
	const uintptr_t ctx = mat ? SceneView_VFunc<uintptr_t(__fastcall*)(uintptr_t)>(mat, MATSYS_GET_RENDER_CONTEXT)(mat) : 0;
	if (!ctx)
		return result;

	const uintptr_t prevTarget = SceneView_VFunc<uintptr_t(__fastcall*)(uintptr_t)>(ctx, CTX_GET_RENDER_TARGET)(ctx);
	SceneView_SetRenderTarget(ctx, view.pColor);
	SceneView_CopyRenderTarget(ctx, *pResolvedColor, *pRect);
	SceneView_SetRenderTarget(ctx, prevTarget);
	s_nResolveMainCopies++;

	SceneView_VFunc<void(__fastcall*)(uintptr_t)>(ctx, CTX_RELEASE)(ctx);
	return result;
}

//-----------------------------------------------------------------------------
// Resolution
//-----------------------------------------------------------------------------
void VSceneView::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 "
		"48 81 EC 10 03 00 00 0F 29 70 C8 0F 29 78 B8").GetPtr(v_SceneView_SetupMonitors);
	// Prologue + the desaturate-flag load and the fog-params lea that follow it.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 "
		"48 83 EC 60 44 0F B6 25 ?? ?? ?? ?? 48 8D 2D").GetPtr(v_SceneView_DrawMonitors);
	Module_FindPattern(g_GameDll,
		"4C 89 44 24 18 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 "
		"48 81 EC 28 01 00 00 48 8B 0D").GetPtr(v_SceneView_DrawScene);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 F6 41 3C 10 "
		"B8 10 00 00 00").GetPtr(v_SceneView_AllocFrameMRTs);
	Module_FindPattern(g_GameDll,
		"4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 55 53 57 41 54 48 8D 6C 24 E8 "
		"48 81 EC 18 01 00 00 4C 8B E1").GetPtr(v_SceneView_ResolveMain);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 30 F3 41 0F 10 48 08 48 8D 51 10 F3 41 0F 10 40 04 4D 8B D1").GetPtr(v_SceneView_BuildSetup);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 60 "
		"48 8B B1 40 B1 02 00").GetPtr(v_SceneView_StartRenderLists);
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 63 91 AC 01 00 00 4C 8D 89 98 00 00 00").GetPtr(v_SceneView_ApplyJitter);
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 8B 81 D0 01 00 00 48 8B D9 85 C0 75 40").GetPtr(v_SceneView_InitSubsystems);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 56 48 83 EC 60 80 3D ?? ?? ?? ?? 00 0F B6 F2 48 8B D9").GetPtr(v_SceneView_MonitorLights);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 50 41 8B F1 41 8B E8 "
		"44 8B F2 48 8B F9 48 85 C9 74").GetPtr(v_SceneView_CreateRenderTarget);
}

void VSceneView::GetVar(void) const
{
	if (v_SceneView_DrawMonitors)
	{
		const CMemory drawMonitors(v_SceneView_DrawMonitors);
		// lea rbp, fogParams / cmp fogParamsValid, al / mov rcx, materials.
		s_pFogParams = drawMonitors.Offset(0x24).ResolveRelativeAddress(0x3, 0x7).RCast<uint8_t*>();
		s_pFogParamsValid = drawMonitors.Offset(0x32).ResolveRelativeAddress(0x2, 0x6).RCast<uint8_t*>();
		s_ppMaterials = drawMonitors.Offset(0x9A).ResolveRelativeAddress(0x3, 0x7).RCast<uintptr_t*>();
	}

	// RenderView: mov r8d, [r13+numMonitors] / test / jz / mov rax, use_monitors / cmp / jz / lea / call DrawMonitors.
	const CMemory gate = Module_FindPattern(g_GameDll,
		"45 8B 85 B4 01 00 00 45 85 C0 74 1C 48 8B 05 ?? ?? ?? ?? 83 78 64 00 74 0F "
		"49 8D 95 C0 01 00 00 49 8B CD E8");
	if (gate)
	{
		s_pDrawGate = gate.RCast<uint8_t*>();
		s_ppUseMonitors = gate.Offset(0x0C).ResolveRelativeAddress(0x3, 0x7).RCast<uintptr_t*>();
	}

	// Monitor draw: copy the view's world-to-proj into the last-monitor matrix.
	const CMemory lastMonitor = Module_FindPattern(g_GameDll,
		"0F 10 86 C0 00 00 00 0F 11 05 ?? ?? ?? ?? 0F 10 8E D0 00 00 00 0F 11 0D");
	if (lastMonitor)
		s_pLastMonitorWorldToProj = lastMonitor.Offset(0x7).ResolveRelativeAddress(0x3, 0x7).RCast<float*>();
}

void VSceneView::Detour(const bool bAttach) const
{
	const bool bResolved = v_SceneView_SetupMonitors && v_SceneView_DrawMonitors && v_SceneView_DrawScene
		&& v_SceneView_AllocFrameMRTs && v_SceneView_ResolveMain && v_SceneView_BuildSetup && v_SceneView_StartRenderLists
		&& v_SceneView_ApplyJitter && v_SceneView_InitSubsystems && v_SceneView_MonitorLights
		&& v_SceneView_CreateRenderTarget && s_ppMaterials && s_pDrawGate && s_ppUseMonitors;

	if (!bResolved)
	{
		if (bAttach)
			Warning(eDLL_T::CLIENT, "[SCENEVIEW] anchors unresolved -- scene views disabled\n");
		return;
	}

	if (!bAttach)
		SceneView_PatchDrawGate(false);

	DetourSetup(&v_SceneView_SetupMonitors, &Hook_SceneView_SetupMonitors, bAttach);
	DetourSetup(&v_SceneView_DrawMonitors, &Hook_SceneView_DrawMonitors, bAttach);
	DetourSetup(&v_SceneView_ResolveMain, &Hook_SceneView_ResolveMain, bAttach);
	DetourSetup(&v_SceneView_AllocFrameMRTs, &Hook_SceneView_AllocFrameMRTs, bAttach);
}
