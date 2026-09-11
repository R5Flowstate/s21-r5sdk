//=============================================================================//
//
// Purpose: FSR 3.1 frame interpolation on the S21 DX12 present path.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_bridge_addrs.h"

#include "windows/framegen.h"
#include "windows/id3dx.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "tier0/dbg.h"

#include <d3d12.h>
#include <dxgi1_5.h>

#ifndef FFX_API_ENTRY
#define FFX_API_ENTRY
#endif
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_api_types.h"
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/dx12/ffx_api_dx12.h"

// ITexture* globals in r5apex_dx12 (resolved from module base).

static ConVar settings_framegen("settings_framegen", "0", FCVAR_RELEASE,
	"FSR 3.1 frame interpolation (DX12 only). 0 = Off. 1 = On. 2 = On + debug tear lines.");

static HMODULE s_hFfx = nullptr;
static PfnFfxCreateContext s_fnCreate = nullptr;
static PfnFfxDestroyContext s_fnDestroy = nullptr;
static PfnFfxConfigure s_fnConfigure = nullptr;
static PfnFfxDispatch s_fnDispatch = nullptr;

static ffxContext s_fgCtx = nullptr;
static ffxContext s_scCtx = nullptr;
static IDXGISwapChain4* s_fgSwapChain = nullptr;
static ID3D12Device* s_device = nullptr;
static ID3D12CommandAllocator* s_alloc = nullptr;
static ID3D12GraphicsCommandList* s_cmd = nullptr;
static ID3D12Resource* s_dummyDepth = nullptr;
static ID3D12Resource* s_dummyMv = nullptr;
static UINT s_dummyW = 0;
static UINT s_dummyH = 0;
static uint64_t s_frameId = 0;
static bool s_bLoadFailed = false;
static bool s_bCreateFailed = false;
static bool s_bLoggedNeedDll = false;
static bool s_bLoggedNeedDx12 = false;
static LARGE_INTEGER s_lastQpc = {};
static LARGE_INTEGER s_qpcFreq = {};

static ID3D12CommandQueue* FrameGen_Queue(void)
{
	return DirectX_GetDx12GameQueue();
}

template <typename T>
static void FG_SafeRelease(T*& p)
{
	if (p)
	{
		p->Release();
		p = nullptr;
	}
}

static void FrameGen_DestroyDummy(void)
{
	FG_SafeRelease(s_dummyDepth);
	FG_SafeRelease(s_dummyMv);
	s_dummyW = 0;
	s_dummyH = 0;
}

static void FrameGen_DestroyContext(void)
{
	if (s_fnDestroy)
	{
		if (s_fgCtx)
			s_fnDestroy(&s_fgCtx, nullptr);
		if (s_scCtx)
			s_fnDestroy(&s_scCtx, nullptr);
	}
	s_fgCtx = nullptr;
	s_scCtx = nullptr;
	s_fgSwapChain = nullptr;
	s_frameId = 0;
}

static bool FrameGen_LoadDll(void)
{
	if (s_hFfx && s_fnCreate && s_fnDestroy && s_fnConfigure && s_fnDispatch)
		return true;
	if (s_bLoadFailed)
		return false;

	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(NULL, exePath, MAX_PATH);
	char* slash = exePath;
	for (char* p = exePath; *p; ++p)
	{
		if (*p == '\\' || *p == '/')
			slash = p;
	}
	*slash = '\0';

	const char* names[] = {
		"amd_fidelityfx_dx12.dll",
		"amd_fidelityfx_loader_dx12.dll",
		"amd_fidelityfx_loader.dll",
	};

	for (const char* name : names)
	{
		char full[MAX_PATH] = {};
		const int n = _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", exePath, name);
		if (n <= 0)
			continue;
		s_hFfx = LoadLibraryA(full);
		if (s_hFfx)
		{
			Warning(eDLL_T::MS, "[FRAMEGEN] loaded %s\n", full);
			break;
		}
	}

	if (!s_hFfx)
	{
		s_bLoadFailed = true;
		if (!s_bLoggedNeedDll)
		{
			s_bLoggedNeedDll = true;
			Warning(eDLL_T::MS, "[FRAMEGEN] amd_fidelityfx_dx12.dll not next to the exe -- FG off\n");
		}
		return false;
	}

	s_fnCreate = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(s_hFfx, "ffxCreateContext"));
	s_fnDestroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(s_hFfx, "ffxDestroyContext"));
	s_fnConfigure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(s_hFfx, "ffxConfigure"));
	s_fnDispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(s_hFfx, "ffxDispatch"));
	if (!s_fnCreate || !s_fnDestroy || !s_fnConfigure || !s_fnDispatch)
	{
		s_bLoadFailed = true;
		Warning(eDLL_T::MS, "[FRAMEGEN] ffx exports missing -- FG off\n");
		return false;
	}
	return true;
}

static ID3D12Resource* FrameGen_CreateTex(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags)
{
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = w;
	desc.Height = h;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.Flags = flags;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	ID3D12Resource* res = nullptr;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res))))
		return nullptr;
	return res;
}

static bool FrameGen_EnsureDummy(ID3D12Device* device, UINT w, UINT h)
{
	if (s_dummyDepth && s_dummyMv && s_dummyW == w && s_dummyH == h)
		return true;

	FrameGen_DestroyDummy();
	s_dummyDepth = FrameGen_CreateTex(device, w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	s_dummyMv = FrameGen_CreateTex(device, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	if (!s_dummyDepth || !s_dummyMv)
	{
		FrameGen_DestroyDummy();
		Warning(eDLL_T::MS, "[FRAMEGEN] dummy depth/mv create failed\n");
		return false;
	}
	s_dummyW = w;
	s_dummyH = h;
	return true;
}

static bool FrameGen_EnsureCmd(ID3D12Device* device)
{
	if (s_alloc && s_cmd)
		return true;
	if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_alloc))))
		return false;
	if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alloc, nullptr, IID_PPV_ARGS(&s_cmd))))
	{
		FG_SafeRelease(s_alloc);
		return false;
	}
	s_cmd->Close();
	return true;
}

static ID3D12Resource* FrameGen_ProbeResource(void* pObj, size_t nbytes)
{
	if (!pObj)
		return nullptr;
	const uintptr_t base = reinterpret_cast<uintptr_t>(pObj);
	for (size_t off = 0; off + sizeof(void*) <= nbytes; off += sizeof(void*))
	{
		void* cand = nullptr;
		__try
		{
			cand = *reinterpret_cast<void**>(base + off);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			continue;
		}
		if (!cand)
			continue;

		ID3D12Resource* res = nullptr;
		__try
		{
			IUnknown* unk = reinterpret_cast<IUnknown*>(cand);
			if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&res))) && res)
				return res;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
	return nullptr;
}

static ID3D12Resource* FrameGen_FromEngineRt(const NetObsSym_t sym)
{
	const uintptr_t slot = NetObs_Sym(sym);
	if (!slot)
		return nullptr;

	void* pTex = nullptr;
	__try
	{
		pTex = *reinterpret_cast<void**>(slot);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
	if (!pTex)
		return nullptr;

	void* inner = nullptr;
	__try
	{
		void** vt = *reinterpret_cast<void***>(pTex);
		using Fn = void* (__fastcall*)(void*, unsigned);
		const Fn getInner = reinterpret_cast<Fn>(vt[28]); // +0xE0
		inner = getInner(pTex, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		inner = pTex;
	}

	ID3D12Resource* res = FrameGen_ProbeResource(inner ? inner : pTex, 0x180);
	if (!res)
		res = FrameGen_ProbeResource(pTex, 0x80);
	return res;
}

static void FrameGen_OnFfxMsg(uint32_t type, const wchar_t* message)
{
	const char* kind = (type == FFX_API_MESSAGE_TYPE_ERROR) ? "ERR" : "WRN";
	Warning(eDLL_T::MS, "[FRAMEGEN] ffx %s: %ls\n", kind, message ? message : L"");
}

static bool FrameGen_Create(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
	if (s_bCreateFailed)
		return false;
	if (!FrameGen_LoadDll())
		return false;

	IDXGISwapChain4* sc4 = nullptr;
	if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&sc4))) || !sc4)
	{
		Warning(eDLL_T::MS, "[FRAMEGEN] IDXGISwapChain4 QI failed\n");
		s_bCreateFailed = true;
		return false;
	}

	ID3D12Device* device = nullptr;
	if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device)
	{
		sc4->Release();
		Warning(eDLL_T::MS, "[FRAMEGEN] GetDevice failed\n");
		s_bCreateFailed = true;
		return false;
	}

	DXGI_SWAP_CHAIN_DESC desc = {};
	sc4->GetDesc(&desc);
	if (desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0)
	{
		device->Release();
		sc4->Release();
		return false;
	}

	if (!FrameGen_EnsureDummy(device, desc.BufferDesc.Width, desc.BufferDesc.Height)
		|| !FrameGen_EnsureCmd(device))
	{
		device->Release();
		sc4->Release();
		s_bCreateFailed = true;
		return false;
	}

	ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap = {};
	wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
	wrap.swapchain = &sc4;
	wrap.gameQueue = queue;

	const ffxReturnCode_t scRc = s_fnCreate(&s_scCtx, &wrap.header, nullptr);
	if (scRc != FFX_API_RETURN_OK || !sc4)
	{
		Warning(eDLL_T::MS, "[FRAMEGEN] swapchain wrap failed rc=%u (overlay/capture often returns ACCESS_DENIED)\n", scRc);
		device->Release();
		if (sc4)
			sc4->Release();
		s_bCreateFailed = true;
		return false;
	}

	ffxCreateBackendDX12Desc backend = {};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend.device = device;

	ffxCreateContextDescFrameGeneration fg = {};
	fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	fg.header.pNext = &backend.header;
	fg.flags = FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE
		| FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS
		| FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
	fg.displaySize.width = desc.BufferDesc.Width;
	fg.displaySize.height = desc.BufferDesc.Height;
	fg.maxRenderSize = fg.displaySize;
	fg.backBufferFormat = ffxApiGetSurfaceFormatDX12(desc.BufferDesc.Format);

	const ffxReturnCode_t fgRc = s_fnCreate(&s_fgCtx, &fg.header, nullptr);
	if (fgRc != FFX_API_RETURN_OK)
	{
		Warning(eDLL_T::MS, "[FRAMEGEN] fg context failed rc=%u\n", fgRc);
		s_fnDestroy(&s_scCtx, nullptr);
		s_scCtx = nullptr;
		device->Release();
		s_bCreateFailed = true;
		return false;
	}

	ffxConfigureDescGlobalDebug1 dbg = {};
	dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
	dbg.fpMessage = &FrameGen_OnFfxMsg;
	dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
	s_fnConfigure(nullptr, &dbg.header);

	s_fgSwapChain = sc4;
	s_device = device;
	s_frameId = 0;
	QueryPerformanceFrequency(&s_qpcFreq);
	QueryPerformanceCounter(&s_lastQpc);

	Warning(eDLL_T::MS, "[FRAMEGEN] live %ux%u fmt=%u sc=%p\n",
		desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format, (void*)sc4);
	return true;
}

static float FrameGen_DeltaMs(void)
{
	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);
	double dt = 0.0;
	if (s_qpcFreq.QuadPart)
		dt = 1000.0 * double(now.QuadPart - s_lastQpc.QuadPart) / double(s_qpcFreq.QuadPart);
	s_lastQpc = now;
	if (dt < 1.0)
		dt = 1.0;
	if (dt > 100.0)
		dt = 100.0;
	return static_cast<float>(dt);
}

static bool FrameGen_DispatchPrepare(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
	if (FAILED(s_alloc->Reset()) || FAILED(s_cmd->Reset(s_alloc, nullptr)))
		return false;

	ID3D12Resource* back = nullptr;
	if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
	{
		s_cmd->Close();
		return false;
	}

	const D3D12_RESOURCE_DESC bd = back->GetDesc();
	const UINT w = static_cast<UINT>(bd.Width);
	const UINT h = bd.Height;
	if (!FrameGen_EnsureDummy(s_device, w, h))
	{
		back->Release();
		s_cmd->Close();
		return false;
	}

	ID3D12Resource* color = nullptr;
	const NetObsSym_t tsaaSym = (s_frameId & 1ull) ? NetObsSym_t::RtTsaa1 : NetObsSym_t::RtTsaa0;
	color = FrameGen_FromEngineRt(tsaaSym);
	ID3D12Resource* depth = FrameGen_FromEngineRt(NetObsSym_t::RtFullFrameDepth);

	const bool bHudless = (color != nullptr);
	if (!color)
		color = back;

	ID3D12Resource* mv = s_dummyMv;
	if (!depth)
		depth = s_dummyDepth;

	ffxDispatchDescFrameGenerationPrepare prepare = {};
	prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
	prepare.frameID = s_frameId;
	prepare.flags = 0;
	prepare.commandList = s_cmd;
	prepare.renderSize.width = w;
	prepare.renderSize.height = h;
	prepare.jitterOffset.x = 0.f;
	prepare.jitterOffset.y = 0.f;
	prepare.motionVectorScale.x = 1.f;
	prepare.motionVectorScale.y = 1.f;
	prepare.frameTimeDelta = FrameGen_DeltaMs();
	prepare.cameraNear = 1.f;
	prepare.cameraFar = 16384.f;
	prepare.cameraFovAngleVertical = 1.22173047f; // 70 deg
	prepare.viewSpaceToMetersFactor = 0.0254f;
	prepare.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	prepare.motionVectors = ffxApiGetResourceDX12(mv, FFX_API_RESOURCE_STATE_COMPUTE_READ);

	ffxDispatchDescFrameGenerationPrepareCameraInfo cam = {};
	cam.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
	cam.cameraPosition[0] = 0.f;
	cam.cameraPosition[1] = 0.f;
	cam.cameraPosition[2] = 0.f;
	cam.cameraUp[1] = 1.f;
	cam.cameraRight[0] = 1.f;
	cam.cameraForward[2] = -1.f;
	prepare.header.pNext = &cam.header;

	const ffxReturnCode_t rc = s_fnDispatch(&s_fgCtx, &prepare.header);
	s_cmd->Close();
	if (rc != FFX_API_RETURN_OK)
	{
		if ((s_frameId % 120ull) == 0)
			Warning(eDLL_T::MS, "[FRAMEGEN] prepare rc=%u\n", rc);
		back->Release();
		if (bHudless && color && color != back)
			color->Release();
		if (depth && depth != s_dummyDepth)
			depth->Release();
		return false;
	}

	ID3D12CommandList* lists[] = { s_cmd };
	queue->ExecuteCommandLists(1, lists);

	ffxConfigureDescFrameGeneration cfg = {};
	cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	cfg.swapChain = s_fgSwapChain;
	cfg.presentCallback = nullptr;
	cfg.frameGenerationCallback = nullptr;
	cfg.frameGenerationEnabled = true;
	cfg.allowAsyncWorkloads = false;
	if (bHudless)
		cfg.HUDLessColor = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	cfg.flags = 0;
	if (settings_framegen.GetInt() >= 2)
		cfg.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES;
	cfg.onlyPresentGenerated = false;
	cfg.generationRect.left = 0;
	cfg.generationRect.top = 0;
	cfg.generationRect.width = static_cast<int32_t>(w);
	cfg.generationRect.height = static_cast<int32_t>(h);
	cfg.frameID = s_frameId;

	const ffxReturnCode_t cfgRc = s_fnConfigure(&s_fgCtx, &cfg.header);
	if (cfgRc != FFX_API_RETURN_OK && (s_frameId % 120ull) == 0)
		Warning(eDLL_T::MS, "[FRAMEGEN] configure rc=%u hudless=%d\n", cfgRc, bHudless ? 1 : 0);

	if ((s_frameId % 300ull) == 0)
	{
		Warning(eDLL_T::MS, "[FRAMEGEN] frame=%llu %ux%u hudless=%d dt=%.2f\n",
			static_cast<unsigned long long>(s_frameId), w, h, bHudless ? 1 : 0, prepare.frameTimeDelta);
	}

	back->Release();
	if (bHudless && color && color != back)
		color->Release();
	if (depth && depth != s_dummyDepth)
		depth->Release();
	return true;
}

void FrameGen_OnEnginePresent(void* pFrameCtx, IDXGISwapChain** ppSwapChain)
{
	if (!DirectX_IsDx12Mode())
	{
		if (settings_framegen.GetInt() > 0 && !s_bLoggedNeedDx12)
		{
			s_bLoggedNeedDx12 = true;
			Warning(eDLL_T::MS, "[FRAMEGEN] DX12 exe only -- launch r5apex_dx12.exe\n");
		}
		return;
	}

	if (settings_framegen.GetInt() <= 0)
	{
		if (s_fgCtx)
		{
			ffxConfigureDescFrameGeneration cfg = {};
			cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
			cfg.swapChain = s_fgSwapChain;
			cfg.frameGenerationEnabled = false;
			cfg.frameID = s_frameId;
			s_fnConfigure(&s_fgCtx, &cfg.header);
		}
		return;
	}

	if (!ppSwapChain || !*ppSwapChain)
		return;

	ID3D12CommandQueue* queue = FrameGen_Queue();
	if (!queue)
		return;

	ConVar* pFpsMaxRt = g_pCVar ? g_pCVar->FindVar("fps_max_rt") : nullptr;
	if (pFpsMaxRt && pFpsMaxRt->GetInt() != 0)
		pFpsMaxRt->SetValue(0);

	if (!s_fgCtx || !s_scCtx)
	{
		if (!FrameGen_Create(*ppSwapChain, queue))
			return;
	}

	if (!s_fgSwapChain || !s_fgCtx)
		return;

	++s_frameId;
	FrameGen_DispatchPrepare(*ppSwapChain, queue);

	if (pFrameCtx && s_fgSwapChain)
	{
		__try
		{
			void** ctx = *reinterpret_cast<void***>(pFrameCtx);
			ctx[6] = s_fgSwapChain;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
	*ppSwapChain = reinterpret_cast<IDXGISwapChain*>(s_fgSwapChain);
}

void FrameGen_Shutdown(void)
{
	FrameGen_DestroyContext();
	FrameGen_DestroyDummy();
	FG_SafeRelease(s_cmd);
	FG_SafeRelease(s_alloc);
	FG_SafeRelease(s_device);
	s_bCreateFailed = false;
}

