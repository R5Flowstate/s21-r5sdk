#if defined(CLIENT_DLL)
//===========================================================================//
//
// Purpose
//
//===========================================================================//
#include "core/stdafx.h"
#include "tier0/crashhandler.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier1/keyvalues.h"
#include "rtech/pak/pakstate.h"
#include "engine/cmodel_bsp.h"
#include "engine/sys_engine.h"
#include "engine/sys_dll2.h"
#ifndef MATERIALSYSTEM_NODX
#include "windows/id3dx.h"
#include "gameui/imgui_system.h"
#include "materialsystem/cmaterialglue.h"
#include "materialsystem/texturestreaming.h"
#endif // !MATERIALSYSTEM_NODX
#include "materialsystem/cmaterialsystem.h"

bool CMaterialSystem::Connect(CMaterialSystem* thisptr, const CreateInterfaceFn factory)
{
	const bool result = CMaterialSystem__Connect(thisptr, factory);
	return result;
}

void CMaterialSystem::Disconnect(CMaterialSystem* thisptr)
{
	CMaterialSystem__Disconnect(thisptr);
}

//-----------------------------------------------------------------------------
// Purpose: initialization of the material system
//-----------------------------------------------------------------------------
InitReturnVal_t CMaterialSystem::Init(CMaterialSystem* thisptr)
{
#ifdef MATERIALSYSTEM_NODX
	// Only load the startup pak files, as 'common_early.rpak' has assets
	// that references assets in 'startup.rpak'.
	g_pakLoadApi->LoadAsyncAndWait("startup.rpak", AlignedMemAlloc(), 5, 0);
	g_pakLoadApi->LoadAsyncAndWait("startup_sdk.rpak", AlignedMemAlloc(), 5, 0);

	// Trick: return INIT_FAILED to disable the loading of hardware
	// configuration data, since we don't need it on the dedi.
	return INIT_FAILED;
#else
	// Initialize as usual.
	const InitReturnVal_t result = CMaterialSystem__Init(thisptr);

	// After CMaterialSystem::Init so startup_sdk.rpak loads after startup.rpak.
	g_pakLoadApi->LoadAsyncAndWait("startup_sdk.rpak", AlignedMemAlloc(), 5, 0);
	return result;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: shutdown of the material system
//-----------------------------------------------------------------------------
int CMaterialSystem::Shutdown(CMaterialSystem* thisptr)
{
	return CMaterialSystem__Shutdown(thisptr);
}

#ifndef MATERIALSYSTEM_NODX
//---------------------------------------------------------------------------------
// Purpose: draw frame
//---------------------------------------------------------------------------------
void* __fastcall DispatchDrawCall(int64_t a1, uint64_t a2, int a3, int a4, int64_t a5, int a6, uint8_t a7, int64_t a8, uint32_t a9, uint32_t a10, int a11, __m128* a12, int a13, int64_t a14)
{
	// This only happens when the BSP is in a horrible condition (bad depth buffer draw calls!)
	// but allows you to load BSP's with virtually all missing shaders/materials and models 
	// being replaced with 'material_for_aspect/error.rpak' and 'mdl/error.rmdl'.
	if (!*s_pRenderContext)
		return nullptr;

	return v_DispatchDrawCall(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

//---------------------------------------------------------------------------------
// Purpose: run IDXGISwapChain::Present
//---------------------------------------------------------------------------------
ssize_t SpinPresent(void)
{
	CImguiSystem* const imguiSystem = ImguiSystem();

	if (imguiSystem->IsInitialized())
		imguiSystem->RenderFrame();

	const ssize_t val = v_SpinPresent();
	return val;
}

void* CMaterialSystem::SwapBuffers(CMaterialSystem* pMatSys)
{
	CImguiSystem* const imguiSystem = ImguiSystem();

	// See https://github.com/ocornut/imgui/issues/7615, looking for status msg
	// DXGI_STATUS_OCCLUDED isn't compatible with DXGI_SWAP_EFFECT_FLIP_DISCARD.
	// This engine however does not use the flip model.
	if (imguiSystem->IsInitialized() && D3D11SwapChain()->Present(0, DXGI_PRESENT_TEST) != DXGI_STATUS_OCCLUDED)
	{
		imguiSystem->SampleFrame();
		imguiSystem->SwapBuffers();
	}

	return CMaterialSystem__SwapBuffers(pMatSys);
}

//-----------------------------------------------------------------------------
// Purpose: finds a material
// Input: *pMatSys - 
// *pMaterialName - 
// nMaterialType - 
// nUnk - 
// bComplain - 
// Output: pointer to material
//-----------------------------------------------------------------------------
static ConVar mat_alwaysComplain("mat_alwaysComplain", "0", FCVAR_RELEASE | FCVAR_MATERIAL_SYSTEM_THREAD, "Always complain when a material is missing");

static bool Mat_ShouldSuppressMissingLog(const char* const pMaterialName)
{
	return (pMaterialName && !V_stricmp(pMaterialName, "error"));
}

CMaterialGlue* CMaterialSystem::FindMaterialEx(CMaterialSystem* pMatSys, const char* pMaterialName, uint8_t nMaterialType, int nUnk, bool bComplain)
{
	CMaterialGlue* const pMaterial = CMaterialSystem__FindMaterialEx(pMatSys, pMaterialName, nMaterialType, nUnk, bComplain);

	// Miss returns g_ppErrorMaterials[type]. Do not use S3 IsErrorMaterial vftable[0].
	if ((bComplain || mat_alwaysComplain.GetBool()) && pMaterialName && pMaterial && g_ppErrorMaterials)
	{
		CMaterialGlue* const* const ppErrorMaterials = *g_ppErrorMaterials;

		if (ppErrorMaterials && pMaterial == ppErrorMaterials[nMaterialType] &&
			!Mat_ShouldSuppressMissingLog(pMaterialName))
		{
			Error(eDLL_T::MS, NO_ERROR, "Material \"%s\" not found; replacing with error material.\n", pMaterialName);
		}
	}
	return pMaterial;
}

//-----------------------------------------------------------------------------
// Purpose: get screen size
// Input: *pMatSys - 
// Output: Vector2D screen size
//-----------------------------------------------------------------------------
Vector2D CMaterialSystem::GetScreenSize(CMaterialSystem* pMatSys)
{
	Vector2D vecScreenSize;

	CMaterialSystem__GetScreenSize(pMatSys, &vecScreenSize.x, &vecScreenSize.y);

	return vecScreenSize;
}

//-----------------------------------------------------------------------------
// Purpose: same as StreamDB_CreditWorldTextures, but also takes the coverage
// of the dynamic model into account.
// Input: *pMatSys - 
// *materialGlue - 
// a3 - 
// a4 - 
// a5 - 
// *pViewOrigin - 
// tanOfHalfFov - 
// viewWidthPixels - 
// a9 - 
//-----------------------------------------------------------------------------
void CMaterialSystem::CreditModelTextures(CMaterialSystem* const pMatSys, CMaterialGlue* const materialGlue, __int64 a3, __int64 a4, unsigned int a5, const Vector3D* const pViewOrigin, const float tanOfHalfFov, const float viewWidthPixels, int a9)
{
	if (!materialGlue->CanCreditModelTextures())
		return;

	// If we use the GPU driven texture streaming system, do not run this code
	// as the compute shaders deals with both static and dynamic model textures.
	if (gpu_driven_tex_stream->GetBool())
		return;

	MaterialGlue_s* const material = materialGlue->Get();
	material->lastFrame = s_textureStreamMgr->thisFrame;

	v_StreamDB_CreditModelTextures(material->streamingTextureHandles, material->streamingTextureHandleCount, a3, a4, a5, pViewOrigin, tanOfHalfFov, viewWidthPixels, a9);
}

//-----------------------------------------------------------------------------
// Purpose: updates the stream camera used for getting the column from the STBSP
// Input: *pMatSys - 
// *camPos - 
// *camAng - 
// halfFovX - 
// viewWidth - 
//-----------------------------------------------------------------------------
void CMaterialSystem::UpdateStreamCamera(CMaterialSystem* const pMatSys, const Vector3D* const camPos, 
	const QAngle* const camAng, const float halfFovX, const float viewWidth)
{
	// The stream camera is only used for the STBSP. If we use the GPU feedback
	// driven texture streaming system instead, do not run this code.
	if (gpu_driven_tex_stream->GetBool())
		return;

	// NOTE: 'camAng' is set and provided to the function below, but the actual
	// function that updates the global state (StreamDB_SetCameraPosition)
	// isn't using it. The parameter is unused.
	CMaterialSystem__UpdateStreamCamera(pMatSys, camPos, camAng, halfFovX, viewWidth);
}
#endif // !MATERIALSYSTEM_NODX

///////////////////////////////////////////////////////////////////////////////
void VMaterialSystem::Detour(const bool bAttach) const
{
	DetourSetup(&CMaterialSystem__Init, &CMaterialSystem::Init, bAttach);
	DetourSetup(&CMaterialSystem__Shutdown, &CMaterialSystem::Shutdown, bAttach);

	DetourSetup(&CMaterialSystem__Connect, &CMaterialSystem::Connect, bAttach);
	DetourSetup(&CMaterialSystem__Disconnect, &CMaterialSystem::Disconnect, bAttach);

#ifndef MATERIALSYSTEM_NODX
	DetourSetup(&CMaterialSystem__SwapBuffers, &CMaterialSystem::SwapBuffers, bAttach);

	DetourSetup(&CMaterialSystem__CreditModelTextures, &CMaterialSystem::CreditModelTextures, bAttach);
	DetourSetup(&CMaterialSystem__UpdateStreamCamera, &CMaterialSystem::UpdateStreamCamera, bAttach);

	DetourSetup(&v_DispatchDrawCall, &DispatchDrawCall, bAttach);
	DetourSetup(&v_SpinPresent, &SpinPresent, bAttach);
#endif // !MATERIALSYSTEM_NODX
}

#ifndef MATERIALSYSTEM_NODX
///////////////////////////////////////////////////////////////////////////////
void VMaterialMissingLogS21::Detour(const bool bAttach) const
{
	// Only the missing-material logger -- no render-thread hooks.
	if (CMaterialSystem__FindMaterialEx)
		DetourSetup(&CMaterialSystem__FindMaterialEx, &CMaterialSystem::FindMaterialEx, bAttach);
}
#endif // !MATERIALSYSTEM_NODX

#ifndef MATERIALSYSTEM_NODX
// MaterialGlue_s: streamingTextureHandles at +0x68, its count at +0x70.
static const size_t kMaterialStreamingHandles = 0x68;
static const size_t kMaterialStreamingHandleCount = 0x70;
// Ids at or above this go through a per-frame remap table instead.
static const size_t kStreamingMaterialIdCount = 0xE400;

// The stream id the register callback stamped on the material. The two
// client executables lay this out differently.
static inline size_t Material_StreamIdOffset()
{
	return SDK_IsDx12Exe() ? 0x11E : 0x126;
}

static ConVar sdk_texstream_fb_sync("sdk_texstream_fb_sync", "1", FCVAR_RELEASE,
	"Hold a material unload while the GPU-feedback job table is being built and dispatched.");

// Nonzero while a feedback build owns entries of the material id table.
static volatile LONG s_texStreamFeedbackBuild = 0;
static thread_local int s_texStreamFeedbackDepth = 0;

//-----------------------------------------------------------------------------
// Purpose: the frame processor maps the readback ids through the material id
// table and dispatches the crediting job, but publishes the job handle only
// after the dispatch. An unload landing in that window finds a null handle,
// waits for nothing, and frees a material the live job still walks. Mark the
// build so the unload below can wait for it.
//-----------------------------------------------------------------------------
int64_t TextureStreamMgr_ProcessGPUFeedback(const unsigned int nSlot)
{
	if (!sdk_texstream_fb_sync.GetBool())
		return v_TextureStreamMgr_ProcessGPUFeedback(nSlot);

	static bool s_bLoggedFirstBuild = false;
	if (!s_bLoggedFirstBuild)
	{
		s_bLoggedFirstBuild = true;
		Msg(eDLL_T::MS, "[TEXSTREAM-SYNC] armed on the first feedback build\n");
	}

	++s_texStreamFeedbackDepth;
	InterlockedIncrement(&s_texStreamFeedbackBuild);

	const int64_t nResult = v_TextureStreamMgr_ProcessGPUFeedback(nSlot);

	InterlockedDecrement(&s_texStreamFeedbackBuild);
	--s_texStreamFeedbackDepth;

	return nResult;
}

//-----------------------------------------------------------------------------
// Purpose: frees the material's stream id and invalidates the feedback state.
// Two things are missing from it. The id stays published to the streamer
// after the material is gone, which is what the clear below fixes; and a
// build already holding the pointer is not covered by the invalidate's wait,
// which is what the hold above covers.
//-----------------------------------------------------------------------------
void Material_UnregisterStreaming(void* const pMaterial)
{
	// A nonzero depth means this thread is the one building and reached the
	// unload through an inlined job -- waiting would be waiting on itself.
	if (sdk_texstream_fb_sync.GetBool() && !s_texStreamFeedbackDepth && s_texStreamFeedbackBuild)
	{
		const double flDeadline = Plat_FloatTime() + 0.100;

		while (s_texStreamFeedbackBuild)
		{
			if (Plat_FloatTime() > flDeadline)
			{
				static bool s_bTimedOut = false;
				if (!s_bTimedOut)
				{
					s_bTimedOut = true;
					Warning(eDLL_T::MS, "[TEXSTREAM-SYNC] feedback build did not finish in 100 ms -- unloading anyway\n");
				}
				break;
			}
			ThreadSleep(0);
		}

		static bool s_bLoggedFirstWait = false;
		if (!s_bLoggedFirstWait)
		{
			s_bLoggedFirstWait = true;
			Msg(eDLL_T::MS, "[TEXSTREAM-SYNC] held first material unload for an in-flight feedback build\n");
		}
	}

	// The callback frees the id for reuse but leaves the id table pointing at
	// the material it is about to release, and nothing on the read path checks
	// that an id is still allocated. Publish the empty slot the crediting job
	// already tests for.
	if (g_ppStreamingMaterialById && pMaterial)
	{
		const uint16_t nId = *reinterpret_cast<const uint16_t*>(
			reinterpret_cast<const uint8_t*>(pMaterial) + Material_StreamIdOffset());

		if (nId < kStreamingMaterialIdCount && g_ppStreamingMaterialById[nId] == pMaterial)
			g_ppStreamingMaterialById[nId] = nullptr;
	}

	v_Material_UnregisterStreaming(pMaterial);
}

static ConVar sdk_texstream_fb_validate("sdk_texstream_fb_validate", "1", FCVAR_RELEASE,
	"Skip a GPU-feedback entry whose material carries an unreadable streaming handle array instead of dereferencing it.");

static bool TexStream_PtrLooksSane(const void* const pPointer)
{
	const uintptr_t nAddress = reinterpret_cast<uintptr_t>(pPointer);

	// User-mode canonical addresses leave the top 17 bits clear; a #GP on a
	// non-canonical pointer is what Windows reports as a read of -1.
	return nAddress && !(nAddress & 7) && (nAddress >> 47) == 0;
}

static volatile LONG s_texStreamBadEntries = 0;

static void TexStream_ReportBadEntry(const uint8_t* const pObject, const void* const pHandles,
	const uint16_t nCount, const bool bObjectItself)
{
	const LONG nSeen = InterlockedIncrement(&s_texStreamBadEntries);
	if (nSeen > 4)
		return;

	int nId = -1;
	if (g_ppStreamingMaterialById)
	{
		for (size_t i = 0; i < kStreamingMaterialIdCount; i++)
		{
			if (g_ppStreamingMaterialById[i] == pObject)
			{
				nId = static_cast<int>(i);
				break;
			}
		}
	}

	if (bObjectItself)
	{
		Warning(eDLL_T::MS, "[TEXSTREAM-FB] skipped entry #%d -- object pointer %p is not canonical, id=%d\n",
			nSeen, pObject, nId);
		return;
	}

	char szHead[3 * 32 + 1];
	for (int i = 0; i < 32; i++)
		V_snprintf(&szHead[i * 3], 4, "%02X ", pObject[i]);

	Warning(eDLL_T::MS, "[TEXSTREAM-FB] skipped entry #%d -- object %p id=%d publishes %hu handles at %p\n",
		nSeen, pObject, nId, nCount, pHandles);
	Warning(eDLL_T::MS, "[TEXSTREAM-FB]   head: %s\n", szHead);
}

//-----------------------------------------------------------------------------
// Purpose: credits the mip histogram for one material the GPU reported. The
// readback index is taken on trust: nothing checks that the id is still
// allocated before the material id table is dereferenced, so a stale id hands
// the job an object whose handle array is not one.
//-----------------------------------------------------------------------------
void TextureStreamMgr_CreditFeedbackEntry(void* const pHandleBase, int64_t* const pEntry)
{
	if (sdk_texstream_fb_validate.GetBool() && pEntry)
	{
		const uint8_t* const pMaterial = *reinterpret_cast<uint8_t* const*>(pEntry);

		if (pMaterial && !TexStream_PtrLooksSane(pMaterial))
		{
			TexStream_ReportBadEntry(pMaterial, nullptr, 0, true);
			return;
		}

		if (pMaterial)
		{
			const uint16_t nCount = *reinterpret_cast<const uint16_t*>(pMaterial + kMaterialStreamingHandleCount);
			const void* const pHandles = *reinterpret_cast<const void* const*>(pMaterial + kMaterialStreamingHandles);

			if (nCount && !TexStream_PtrLooksSane(pHandles))
			{
				TexStream_ReportBadEntry(pMaterial, pHandles, nCount, false);
				return;
			}
		}
	}

	v_TextureStreamMgr_CreditFeedbackEntry(pHandleBase, pEntry);
}

///////////////////////////////////////////////////////////////////////////////
void VTexStreamFeedbackSyncS21::Detour(const bool bAttach) const
{
	if (v_TextureStreamMgr_ProcessGPUFeedback)
		DetourSetup(&v_TextureStreamMgr_ProcessGPUFeedback, &TextureStreamMgr_ProcessGPUFeedback, bAttach);

	if (v_Material_UnregisterStreaming)
		DetourSetup(&v_Material_UnregisterStreaming, &Material_UnregisterStreaming, bAttach);

	if (v_TextureStreamMgr_CreditFeedbackEntry)
		DetourSetup(&v_TextureStreamMgr_CreditFeedbackEntry, &TextureStreamMgr_CreditFeedbackEntry, bAttach);
}
#endif // !MATERIALSYSTEM_NODX
#else // !CLIENT_DLL
//===========================================================================//
//
// Purpose
//
//===========================================================================//
#include "core/stdafx.h"
#include "tier0/crashhandler.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier1/keyvalues.h"
#include "rtech/pak/pakstate.h"
#include "engine/cmodel_bsp.h"
#include "engine/sys_engine.h"
#include "engine/sys_dll2.h"
#ifndef MATERIALSYSTEM_NODX
#include "windows/id3dx.h"
#include "gameui/imgui_system.h"
#include "materialsystem/cmaterialglue.h"
#include "materialsystem/texturestreaming.h"
#endif // !MATERIALSYSTEM_NODX
#include "materialsystem/cmaterialsystem.h"

bool CMaterialSystem::Connect(CMaterialSystem* thisptr, const CreateInterfaceFn factory)
{
	const bool result = CMaterialSystem__Connect(thisptr, factory);
	return result;
}

void CMaterialSystem::Disconnect(CMaterialSystem* thisptr)
{
	CMaterialSystem__Disconnect(thisptr);
}

//-----------------------------------------------------------------------------
// Purpose: initialization of the material system
//-----------------------------------------------------------------------------
InitReturnVal_t CMaterialSystem::Init(CMaterialSystem* thisptr)
{
#ifdef MATERIALSYSTEM_NODX
	// Only load the startup pak files, as 'common_early.rpak' has assets
	// that references assets in 'startup.rpak'.
	//g_pakLoadApi->LoadAsyncAndWait("startup.rpak", AlignedMemAlloc, 5, 0);
	//g_pakLoadApi->LoadAsyncAndWait("startup_sdk.rpak", AlignedMemAlloc, 5, 0);

	// Trick: return INIT_FAILED to disable the loading of hardware
	// configuration data, since we don't need it on the dedi.
	return INIT_FAILED;
#else
	// Initialize as usual.
	const InitReturnVal_t result = CMaterialSystem__Init(thisptr);

	// After CMaterialSystem::Init so startup_sdk.rpak loads after startup.rpak.
	// g_pakLoadApi->LoadAsyncAndWait("startup_sdk.rpak", AlignedMemAlloc, 5, 0);
	return result;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: shutdown of the material system
//-----------------------------------------------------------------------------
int CMaterialSystem::Shutdown(CMaterialSystem* thisptr)
{
	return CMaterialSystem__Shutdown(thisptr);
}

#ifndef MATERIALSYSTEM_NODX
//---------------------------------------------------------------------------------
// Purpose: draw frame
//---------------------------------------------------------------------------------
void* __fastcall DispatchDrawCall(int64_t a1, uint64_t a2, int a3, int a4, int64_t a5, int a6, uint8_t a7, int64_t a8, uint32_t a9, uint32_t a10, int a11, __m128* a12, int a13, int64_t a14)
{
	// This only happens when the BSP is in a horrible condition (bad depth buffer draw calls!)
	// but allows you to load BSP's with virtually all missing shaders/materials and models 
	// being replaced with 'material_for_aspect/error.rpak' and 'mdl/error.rmdl'.
	if (!*s_pRenderContext)
		return nullptr;

	return v_DispatchDrawCall(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

//---------------------------------------------------------------------------------
// Purpose: run IDXGISwapChain::Present
//---------------------------------------------------------------------------------
ssize_t SpinPresent(void)
{
	CImguiSystem* const imguiSystem = ImguiSystem();

	if (imguiSystem->IsInitialized())
		imguiSystem->RenderFrame();

	const ssize_t val = v_SpinPresent();
	return val;
}

void* CMaterialSystem::SwapBuffers(CMaterialSystem* pMatSys)
{
	CImguiSystem* const imguiSystem = ImguiSystem();

	// See https://github.com/ocornut/imgui/issues/7615, looking for status msg
	// DXGI_STATUS_OCCLUDED isn't compatible with DXGI_SWAP_EFFECT_FLIP_DISCARD.
	// This engine however does not use the flip model.
	if (imguiSystem->IsInitialized() && D3D11SwapChain()->Present(0, DXGI_PRESENT_TEST) != DXGI_STATUS_OCCLUDED)
	{
		imguiSystem->SampleFrame();
		imguiSystem->SwapBuffers();
	}

	return CMaterialSystem__SwapBuffers(pMatSys);
}

//-----------------------------------------------------------------------------
// Purpose: finds a material
// Input: *pMatSys - 
// *pMaterialName - 
// nMaterialType - 
// nUnk - 
// bComplain - 
// Output: pointer to material
//-----------------------------------------------------------------------------
static ConVar mat_alwaysComplain("mat_alwaysComplain", "0", FCVAR_RELEASE | FCVAR_MATERIAL_SYSTEM_THREAD, "Always complain when a material is missing");

static bool Mat_ShouldSuppressMissingLog(const char* const pMaterialName)
{
	return (pMaterialName && !V_stricmp(pMaterialName, "error"));
}

CMaterialGlue* CMaterialSystem::FindMaterialEx(CMaterialSystem* pMatSys, const char* pMaterialName, uint8_t nMaterialType, int nUnk, bool bComplain)
{
	CMaterialGlue* pMaterial = CMaterialSystem__FindMaterialEx(pMatSys, pMaterialName, nMaterialType, nUnk, bComplain);

	if ((bComplain || mat_alwaysComplain.GetBool()) && pMaterial->IsErrorMaterial())
	{
		if (!Mat_ShouldSuppressMissingLog(pMaterialName))
		{
			Error(eDLL_T::MS, NO_ERROR, "Material \"%s\" not found; replacing with \"%s\".\n", pMaterialName, pMaterial->GetName());
		}
	}
	return pMaterial;
}

//-----------------------------------------------------------------------------
// Purpose: get screen size
// Input: *pMatSys - 
// Output: Vector2D screen size
//-----------------------------------------------------------------------------
Vector2D CMaterialSystem::GetScreenSize(CMaterialSystem* pMatSys)
{
	Vector2D vecScreenSize;

	CMaterialSystem__GetScreenSize(pMatSys, &vecScreenSize.x, &vecScreenSize.y);

	return vecScreenSize;
}

//-----------------------------------------------------------------------------
// Purpose: same as StreamDB_CreditWorldTextures, but also takes the coverage
// of the dynamic model into account.
// Input: *pMatSys - 
// *materialGlue - 
// a3 - 
// a4 - 
// a5 - 
// *pViewOrigin - 
// tanOfHalfFov - 
// viewWidthPixels - 
// a9 - 
//-----------------------------------------------------------------------------
void CMaterialSystem::CreditModelTextures(CMaterialSystem* const pMatSys, CMaterialGlue* const materialGlue, __int64 a3, __int64 a4, unsigned int a5, const Vector3D* const pViewOrigin, const float tanOfHalfFov, const float viewWidthPixels, int a9)
{
	if (!materialGlue->CanCreditModelTextures())
		return;

	// If we use the GPU driven texture streaming system, do not run this code
	// as the compute shaders deals with both static and dynamic model textures.
	if (gpu_driven_tex_stream->GetBool())
		return;

	MaterialGlue_s* const material = materialGlue->Get();
	material->lastFrame = s_textureStreamMgr->thisFrame;

	v_StreamDB_CreditModelTextures(material->streamingTextureHandles, material->streamingTextureHandleCount, a3, a4, a5, pViewOrigin, tanOfHalfFov, viewWidthPixels, a9);
}

//-----------------------------------------------------------------------------
// Purpose: updates the stream camera used for getting the column from the STBSP
// Input: *pMatSys - 
// *camPos - 
// *camAng - 
// halfFovX - 
// viewWidth - 
//-----------------------------------------------------------------------------
void CMaterialSystem::UpdateStreamCamera(CMaterialSystem* const pMatSys, const Vector3D* const camPos, 
	const QAngle* const camAng, const float halfFovX, const float viewWidth)
{
	// The stream camera is only used for the STBSP. If we use the GPU feedback
	// driven texture streaming system instead, do not run this code.
	if (gpu_driven_tex_stream->GetBool())
		return;

	// NOTE: 'camAng' is set and provided to the function below, but the actual
	// function that updates the global state (StreamDB_SetCameraPosition)
	// isn't using it. The parameter is unused.
	CMaterialSystem__UpdateStreamCamera(pMatSys, camPos, camAng, halfFovX, viewWidth);
}
#endif // !MATERIALSYSTEM_NODX

///////////////////////////////////////////////////////////////////////////////
void VMaterialSystem::Detour(const bool bAttach) const
{
	DetourSetup(&CMaterialSystem__Init, &CMaterialSystem::Init, bAttach);
	DetourSetup(&CMaterialSystem__Shutdown, &CMaterialSystem::Shutdown, bAttach);

	DetourSetup(&CMaterialSystem__Connect, &CMaterialSystem::Connect, bAttach);
	DetourSetup(&CMaterialSystem__Disconnect, &CMaterialSystem::Disconnect, bAttach);

#ifndef MATERIALSYSTEM_NODX
	DetourSetup(&CMaterialSystem__SwapBuffers, &CMaterialSystem::SwapBuffers, bAttach);
	DetourSetup(&CMaterialSystem__FindMaterialEx, &CMaterialSystem::FindMaterialEx, bAttach);

	DetourSetup(&CMaterialSystem__CreditModelTextures, &CMaterialSystem::CreditModelTextures, bAttach);
	DetourSetup(&CMaterialSystem__UpdateStreamCamera, &CMaterialSystem::UpdateStreamCamera, bAttach);

	DetourSetup(&v_DispatchDrawCall, &DispatchDrawCall, bAttach);
	DetourSetup(&v_SpinPresent, &SpinPresent, bAttach);
#endif // !MATERIALSYSTEM_NODX
}
#endif // CLIENT_DLL
