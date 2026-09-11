#if defined(CLIENT_DLL)
#include "core/stdafx.h"
//------------------------------
#define STB_IMAGE_IMPLEMENTATION
#include "tier0/threadtools.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "windows/id3dx.h"
#include "windows/framegen.h"
#include "windows/dlssnr.h"
#include "windows/dlss_sr.h"
#include "windows/input.h"
#include "gameui/IConsole.h"
#include "gameui/IBrowser.h"
#include "gameui/IStreamOverlay.h"
#include "gameui/ITopBar.h"
#include "gameui/IDevMenu.h"
#include "gameui/IDlssNrMenu.h"
#include "gameui/imgui_system.h"
#include "engine/framelimit.h"
#include "engine/sys_mainwind.h"
#include "engine/client/clientstate.h"
#include "vgui/vgui_baseui_interface.h"
#include "inputsystem/inputsystem.h"
#include "materialsystem/cmaterialsystem.h"
#include "public/bitmap/stb_image.h"
#include "public/rendersystem/schema/texture.g.h"

// IDXGISwapChain1 / Present1 lives in dxgi1_2.h - not pulled in by the
// default d3d11.h include. Required for the Present1 hook we install
// alongside the legacy Present hook.
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <d3d12.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

/**********************************************************************************
Purpose: Microsoft DirectX 11 'IDXGISwapChain::Present' hook implementation
**********************************************************************************/

///////////////////////////////////////////////////////////////////////////////////
typedef BOOL(WINAPI* IPostMessageA)(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
typedef BOOL(WINAPI* IPostMessageW)(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

///////////////////////////////////////////////////////////////////////////////////
static IDXGIResizeBuffers       s_fnResizeBuffers    = NULL;
static IDXGISwapChainPresent    s_fnSwapChainPresent = NULL;
static bool                     s_bDx12Mode          = false;

// Flip-model calls Present1 every frame; legacy Present is setup-only.
typedef HRESULT(__stdcall* IDXGISwapChain1Present1_t)(
	IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags,
	const DXGI_PRESENT_PARAMETERS* pPresentParameters);
static IDXGISwapChain1Present1_t s_fnSwapChainPresent1 = NULL;

// Render-thread Present via g_pSwapChain vtable slot 8.
typedef __int64 (__fastcall* fnSpinPresent)(LARGE_INTEGER ticks);
static fnSpinPresent s_fnSpinPresent = NULL;

///////////////////////////////////////////////////////////////////////////////////

//#################################################################################
// WINDOW PROCEDURE
//#################################################################################

extern void SDK_Log(const char* fmt, ...);
extern volatile LONG g_imguiWndProcToggleSerial;

LRESULT CALLBACK DXGIMsgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static bool Dx12_KeyPressedEdge(int vkey)
{
	static SHORT s_prevF10 = 0;
	static SHORT s_prevBacktick = 0;
	static SHORT s_prevF9 = 0;

	SHORT* prev = &s_prevBacktick;
	if (vkey == VK_F10)
		prev = &s_prevF10;
	else if (vkey == VK_F9)
		prev = &s_prevF9;
	const SHORT now = GetAsyncKeyState(vkey);
	const bool pressed = (now & 0x8000) != 0 && (*prev & 0x8000) == 0;
	*prev = now;
	return pressed;
}

static void Dx12_PollToggleFallback()
{
	if (!s_bDx12Mode || !ImguiSystem() || !ImguiSystem()->IsInitialized())
		return;

	// GetAsyncKeyState is global to the desktop, so consume the edges without
	// acting on them whenever the keystroke did not belong to this game.
	if (GetForegroundWindow() != DirectX_GetMainWindow())
	{
		Dx12_KeyPressedEdge(VK_F10);
		Dx12_KeyPressedEdge(VK_OEM_3);
		Dx12_KeyPressedEdge(VK_F9);
		return;
	}

	static LONG s_seenWndProcToggleSerial = 0;
	const LONG wndProcSerial = InterlockedCompareExchange(
		&g_imguiWndProcToggleSerial, 0, 0);
	if (wndProcSerial != s_seenWndProcToggleSerial)
	{
		s_seenWndProcToggleSerial = wndProcSerial;
		Dx12_KeyPressedEdge(VK_F10);
		Dx12_KeyPressedEdge(VK_OEM_3);
		Dx12_KeyPressedEdge(VK_F9);
		return;
	}

	if (Dx12_KeyPressedEdge(VK_F10) || Dx12_KeyPressedEdge(VK_OEM_3))
	{
		g_Console.ToggleTab(CConsole::kTabConsole);
		SDK_Log("[IMGUI-DX12] console toggle fallback fired\n");
	}
	if (Dx12_KeyPressedEdge(VK_F9))
	{
		g_DlssNrMenu.ToggleActive();
		ResetInput();
		SDK_Log("[IMGUI-DX12] DLSS NR menu toggle fallback fired\n");
	}
}

//#################################################################################
// IDXGI
//#################################################################################

static ConVar fps_max_rt("fps_max_rt", "0", FCVAR_RELEASE | FCVAR_MATERIAL_SYSTEM_THREAD, "Frame rate limiter within the render thread. -1 indicates the use of desktop refresh. 0 is disabled.", true, -1.f, true, 295.f);
static ConVar fps_max_rt_tolerance("fps_max_rt_tolerance", "0.25", FCVAR_RELEASE | FCVAR_MATERIAL_SYSTEM_THREAD, "Maximum amount of frame time before frame limiter restarts.", true, 0.f, false, 0.f);
static ConVar fps_max_rt_sleep_threshold("fps_max_rt_sleep_threshold", "0.016666667", FCVAR_RELEASE | FCVAR_MATERIAL_SYSTEM_THREAD, "Frame limiter starts to sleep when frame time exceeds this threshold.", true, 0.f, false, 0.f);

// Shared state for Present / Present1 hooks. Static at file scope so both
// entry points see the same disable flag and frame counter - the two hooks
// both drive the same ImGui overlay and must not fight each other.
static volatile bool     s_imguiDisabled = false;
static volatile long long s_presentCount = 0;

// Separate counters per entry point so we can tell from the log WHICH
// hook is actually firing. The SpinPresent -> direct DrivePresentImGui
// path has its own counter in the SpinPresent section below.
static volatile long long s_presentHookCount  = 0;  // DXGI Present slot-8 hook
static volatile long long s_present1HookCount = 0;  // DXGI Present1 slot-22 hook
static volatile long long s_enginePresentCount = 0;  // engine-side DX12 present wrapper
static volatile long long s_spinPresentCount = 0;    // engine render-thread present driver

// Exactly one of engine wrapper, spin, or DXGI Present may own the overlay per frame.
enum Dx12FrameDriver_t
{
	DX12_DRIVER_NONE = 0,
	DX12_DRIVER_DXGI_PRESENT,
	DX12_DRIVER_SPIN_PRESENT,
	DX12_DRIVER_ENGINE_PRESENT,
};

static volatile long s_dx12FrameDriver = DX12_DRIVER_NONE;

static const char* Dx12_FrameDriverName(const long driver)
{
	switch (driver)
	{
	case DX12_DRIVER_DXGI_PRESENT:   return "IDXGISwapChain::Present";
	case DX12_DRIVER_SPIN_PRESENT:   return "SpinPresent";
	case DX12_DRIVER_ENGINE_PRESENT: return "engine present wrapper";
	default:                         return "none";
	}
}

static bool Dx12_ClaimFrameDriver(const Dx12FrameDriver_t driver)
{
	const long owner = InterlockedCompareExchange(&s_dx12FrameDriver, 0, 0);

	if (driver < owner)
		return false;

	if (driver > owner)
	{
		InterlockedExchange(&s_dx12FrameDriver, driver);
		SDK_Log("[IMGUI-DX12] overlay driven from the %s\n", Dx12_FrameDriverName(driver));
	}

	return true;
}

struct Dx12FrameContext
{
	ID3D12CommandAllocator*      CommandAllocator = nullptr;
	ID3D12Resource*              RenderTarget = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE  RtvHandle = {};
	UINT64                      FenceValue = 0;
};

static IDXGISwapChain3*        s_dx12SwapChain = nullptr;
static ID3D12Device*           s_dx12Device = nullptr;
static ID3D12CommandQueue*     s_dx12CommandQueue = nullptr;
static ID3D12CommandQueue*     s_dx12GameCommandQueue = nullptr;
static ID3D12DescriptorHeap*   s_dx12RtvHeap = nullptr;
static ID3D12DescriptorHeap*   s_dx12SrvHeap = nullptr;
static ID3D12GraphicsCommandList* s_dx12CommandList = nullptr;
static ID3D12Fence*            s_dx12Fence = nullptr;
static HANDLE                  s_dx12FenceEvent = NULL;
static Dx12FrameContext*       s_dx12Frames = nullptr;
static UINT                    s_dx12FrameCount = 0;
static UINT                    s_dx12RtvDescriptorSize = 0;
static UINT                    s_dx12SrvDescriptorSize = 0;
static UINT                    s_dx12SrvDescriptorCapacity = 0;
static UINT                    s_dx12SrvNextDescriptor = 1; // slot 0 is the ImGui font atlas
static UINT64                  s_dx12FenceLastSignaled = 0;
static bool                    s_dx12ImguiReady = false;
static HWND                    s_cachedMainWindow = NULL;

// The game's swapchain, kept alive independently of the render state so the
// engine-side present driver still has one after a resize tears that state
// down. Released only in DirectX_Shutdown.
static IDXGISwapChain*         s_dx12GameSwapChain = nullptr;

// Set for the duration of the engine's ResizeBuffers. DXGI fails the resize if
// anything still references a back buffer, so the overlay must not re-acquire
// one from a present driver that runs inside that window.
static volatile bool           s_dx12InResize = false;

// DXGI Present fires once per process; log the return chain to see who presented.
static void Dx12_LogPresentCallers(const char* tag)
{
	void* frames[10] = {};
	const USHORT captured = RtlCaptureStackBackTrace(1, ARRAYSIZE(frames), frames, nullptr);

	char line[1024];
	// Shared DXGI Present path -- tag is IMGUI-DXGI so a DX11 run is not
	// misread as the DX12 overlay pipeline.
	int used = _snprintf_s(line, sizeof(line), _TRUNCATE, "[IMGUI-DXGI] %s callers:", tag);
	for (USHORT i = 0; i < captured && used > 0; ++i)
	{
		HMODULE mod = NULL;
		char path[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &mod))
			GetModuleFileNameA(mod, path, sizeof(path));

		const char* base = strrchr(path, '\\');
		const uintptr_t rva = mod
			? reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(mod) : 0;

		const int n = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
			" %s+0x%llX", base ? base + 1 : path, (unsigned long long)rva);
		if (n < 0)
			break;
		used += n;
	}
	SDK_Log("%s\n", line);
}

static void Dx12_CaptureGameSwapChain(IDXGISwapChain* pSwapChain)
{
	if (!pSwapChain)
		return;

	pSwapChain->AddRef();
	IDXGISwapChain* const prev = static_cast<IDXGISwapChain*>(InterlockedExchangePointer(
		reinterpret_cast<PVOID volatile*>(&s_dx12GameSwapChain), pSwapChain));

	if (prev && prev != pSwapChain)
		prev->Release();
	else if (prev == pSwapChain)
		pSwapChain->Release();
}

IDXGISwapChain* Dx12_GetGameSwapChain(void)
{
	return s_dx12GameSwapChain;
}

typedef void(__stdcall* ID3D12CommandQueueExecuteCommandLists_t)(
	ID3D12CommandQueue* pQueue, UINT NumCommandLists,
	ID3D12CommandList* const* ppCommandLists);
static ID3D12CommandQueueExecuteCommandLists_t s_fnD3D12ExecuteCommandLists = nullptr;

template <typename T>
static void SafeReleaseT(T*& p)
{
	if (p)
	{
		p->Release();
		p = nullptr;
	}
}

static void Dx12_CaptureGameCommandQueue(ID3D12CommandQueue* pQueue)
{
	if (!pQueue || s_dx12GameCommandQueue)
		return;

	const D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
	if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
		return;

	pQueue->AddRef();
	void* const previous = InterlockedCompareExchangePointer(
		reinterpret_cast<PVOID volatile*>(&s_dx12GameCommandQueue),
		pQueue,
		nullptr);
	if (previous)
	{
		pQueue->Release();
		return;
	}

	SDK_Log("[IMGUI-DX12] captured game command queue=%p flags=0x%X priority=%d node=%u\n",
		(void*)pQueue, desc.Flags, desc.Priority, desc.NodeMask);
}

static void __stdcall D3D12ExecuteCommandLists_Hook(
	ID3D12CommandQueue* pQueue, UINT NumCommandLists,
	ID3D12CommandList* const* ppCommandLists)
{
	Dx12_CaptureGameCommandQueue(pQueue);

	// The game's own submission path, so it ticks every frame whether or not
	// our present hooks do. That makes it the reference clock for deciding
	// whether the game stopped rendering or merely stopped presenting through us.
	static volatile long long s_eclCount = 0;
	const long long n = ++s_eclCount;
	if (n == 1 || (n % 60000) == 0)
	{
		// The game's submission clock. Comparing it against the present and
		// draw counters is what distinguishes "the game stopped rendering"
		// from "the overlay lost its driver".
		SDK_Log("[IMGUI-DX12] ECL n=%lld enginePresent=%lld spin=%lld drive=%lld ready=%d\n",
			n, (long long)s_enginePresentCount, (long long)s_spinPresentCount,
			(long long)s_presentCount, s_dx12ImguiReady ? 1 : 0);
	}

	s_fnD3D12ExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
}

static BOOL CALLBACK DirectX_EnumWindowProc(HWND hWnd, LPARAM lParam)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hWnd, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(hWnd))
		return TRUE;

	const LONG_PTR style = GetWindowLongPtrA(hWnd, GWL_STYLE);
	if ((style & WS_DISABLED) != 0)
		return TRUE;

	*reinterpret_cast<HWND*>(lParam) = hWnd;
	return FALSE;
}

HWND DirectX_GetMainWindow()
{
	if (g_pGame && g_pGame->GetWindow())
		return g_pGame->GetWindow();
	if (s_cachedMainWindow && IsWindow(s_cachedMainWindow))
		return s_cachedMainWindow;

	HWND hWnd = NULL;
	EnumWindows(DirectX_EnumWindowProc, reinterpret_cast<LPARAM>(&hWnd));
	s_cachedMainWindow = hWnd;
	return hWnd;
}

bool DirectX_IsDx12Mode()
{
	return s_bDx12Mode;
}

ID3D12CommandQueue* DirectX_GetDx12GameQueue()
{
	return s_dx12GameCommandQueue;
}

static void Dx12_WaitForFrame(Dx12FrameContext& frame)
{
	if (!s_dx12Fence || !s_dx12FenceEvent || frame.FenceValue == 0)
		return;

	if (s_dx12Fence->GetCompletedValue() < frame.FenceValue)
	{
		s_dx12Fence->SetEventOnCompletion(frame.FenceValue, s_dx12FenceEvent);
		WaitForSingleObject(s_dx12FenceEvent, INFINITE);
	}
	frame.FenceValue = 0;
}

static void Dx12_ShutdownRenderState()
{
	if (s_dx12ImguiReady)
	{
		ImGui_ImplDX12_Shutdown();
		s_dx12ImguiReady = false;
	}

	if (s_dx12Frames)
	{
		for (UINT i = 0; i < s_dx12FrameCount; ++i)
		{
			SafeReleaseT(s_dx12Frames[i].RenderTarget);
			SafeReleaseT(s_dx12Frames[i].CommandAllocator);
		}
		delete[] s_dx12Frames;
		s_dx12Frames = nullptr;
	}

	if (s_dx12FenceEvent)
	{
		CloseHandle(s_dx12FenceEvent);
		s_dx12FenceEvent = NULL;
	}

	SafeReleaseT(s_dx12CommandList);
	SafeReleaseT(s_dx12Fence);
	SafeReleaseT(s_dx12SrvHeap);
	SafeReleaseT(s_dx12RtvHeap);
	SafeReleaseT(s_dx12CommandQueue);
	SafeReleaseT(s_dx12Device);
	SafeReleaseT(s_dx12SwapChain);

	s_dx12FrameCount = 0;
	s_dx12RtvDescriptorSize = 0;
	s_dx12SrvDescriptorSize = 0;
	s_dx12SrvDescriptorCapacity = 0;
	s_dx12SrvNextDescriptor = 1;
	s_dx12FenceLastSignaled = 0;
}

static bool Dx12_InitRenderState(IDXGISwapChain* pSwapChain)
{
	if (s_dx12ImguiReady)
		return true;
	// A back-buffer reference taken here makes the engine's next ResizeBuffers
	// fail, and the engine derefs its all-NULL buffer table without checking.
	if (s_dx12InResize)
		return false;
	if (!ImguiSystem()->IsInitialized())
		return false;

	IDXGISwapChain3* swapChain3 = nullptr;
	if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || !swapChain3)
		return false;

	ID3D12Device* device = nullptr;
	if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&device))) || !device)
	{
		swapChain3->Release();
		return false;
	}

	DXGI_SWAP_CHAIN_DESC desc = {};
	if (FAILED(pSwapChain->GetDesc(&desc)) || desc.BufferCount == 0)
	{
		device->Release();
		swapChain3->Release();
		return false;
	}

	if (!s_dx12GameCommandQueue)
	{
		static bool s_bLoggedNoQueue = false;
		if (!s_bLoggedNoQueue)
		{
			s_bLoggedNoQueue = true;
			Warning(eDLL_T::MS, "[IMGUI-DX12] no game command queue captured; overlay init deferred\n");
		}
		device->Release();
		swapChain3->Release();
		return false;
	}
	s_dx12CommandQueue = s_dx12GameCommandQueue;
	s_dx12CommandQueue->AddRef();

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.NumDescriptors = desc.BufferCount;
	if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&s_dx12RtvHeap))))
	{
		device->Release();
		swapChain3->Release();
		Dx12_ShutdownRenderState();
		return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.NumDescriptors = 64;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&s_dx12SrvHeap))))
	{
		device->Release();
		swapChain3->Release();
		Dx12_ShutdownRenderState();
		return false;
	}

	s_dx12FrameCount = desc.BufferCount;
	s_dx12Frames = new Dx12FrameContext[s_dx12FrameCount];
	s_dx12RtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	s_dx12SrvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	s_dx12SrvDescriptorCapacity = srvHeapDesc.NumDescriptors;
	s_dx12SrvNextDescriptor = 1;

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = s_dx12RtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < s_dx12FrameCount; ++i)
	{
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&s_dx12Frames[i].CommandAllocator))))
		{
			device->Release();
			swapChain3->Release();
			Dx12_ShutdownRenderState();
			return false;
		}

		if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&s_dx12Frames[i].RenderTarget))))
		{
			device->Release();
			swapChain3->Release();
			Dx12_ShutdownRenderState();
			return false;
		}

		s_dx12Frames[i].RtvHandle = rtvHandle;
		device->CreateRenderTargetView(s_dx12Frames[i].RenderTarget, nullptr, rtvHandle);
		rtvHandle.ptr += s_dx12RtvDescriptorSize;
	}

	if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		s_dx12Frames[0].CommandAllocator, nullptr, IID_PPV_ARGS(&s_dx12CommandList))))
	{
		device->Release();
		swapChain3->Release();
		Dx12_ShutdownRenderState();
		return false;
	}
	s_dx12CommandList->Close();

	if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_dx12Fence))))
	{
		device->Release();
		swapChain3->Release();
		Dx12_ShutdownRenderState();
		return false;
	}
	s_dx12FenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	if (!s_dx12FenceEvent)
	{
		device->Release();
		swapChain3->Release();
		Dx12_ShutdownRenderState();
		return false;
	}

	ImGui_ImplDX12_InitInfo initInfo;
	initInfo.Device = device;
	initInfo.CommandQueue = s_dx12CommandQueue;
	initInfo.NumFramesInFlight = (int)s_dx12FrameCount;
	initInfo.RTVFormat = desc.BufferDesc.Format;
	initInfo.SrvDescriptorHeap = s_dx12SrvHeap;
	initInfo.LegacySingleSrvCpuDescriptor = s_dx12SrvHeap->GetCPUDescriptorHandleForHeapStart();
	initInfo.LegacySingleSrvGpuDescriptor = s_dx12SrvHeap->GetGPUDescriptorHandleForHeapStart();

	if (!ImGui_ImplDX12_Init(&initInfo))
	{
		static bool s_bLoggedImguiInitFail = false;
		if (!s_bLoggedImguiInitFail)
		{
			s_bLoggedImguiInitFail = true;
			Warning(eDLL_T::MS, "[IMGUI-DX12] ImGui_ImplDX12_Init failed; overlay disabled\n");
		}
		device->Release();
		swapChain3->Release();
		Dx12_ShutdownRenderState();
		return false;
	}

	s_dx12SwapChain = swapChain3;
	s_dx12Device = device;
	s_dx12ImguiReady = true;
	// Dimensions and output window identify WHICH swapchain this is: the
	// temporary one created here for vtable access is tiny and owns a dummy
	// window, the game's is the size of the client area.
	SDK_Log("[IMGUI-DX12] initialized from swapchain=%p %ux%u outWnd=%p mainWnd=%p device=%p buffers=%u format=%u\n",
		(void*)pSwapChain, desc.BufferDesc.Width, desc.BufferDesc.Height,
		(void*)desc.OutputWindow, (void*)DirectX_GetMainWindow(),
		(void*)device, s_dx12FrameCount, desc.BufferDesc.Format);
	return true;
}

//---------------------------------------------------------------------------
// Overlay bring-up against hWnd. DX12 retries from the frame driver (no window at DirectX_Init).
//---------------------------------------------------------------------------
static bool Imgui_Bringup(const HWND hWnd)
{
	static bool s_bringupFailed = false;
	// Claim once -- DirectX_Init and the frame driver can race and double-register surfaces.
	static volatile long s_bringupClaimed = 0;

	CImguiSystem* const imguiSystem = ImguiSystem();

	if (imguiSystem->IsInitialized())
		return true;
	if (!hWnd || s_bringupFailed || !imguiSystem->IsEnabled())
		return false;
	if (InterlockedCompareExchange(&s_bringupClaimed, 1, 0) != 0)
		return imguiSystem->IsInitialized();

	if (!imguiSystem->Init(hWnd))
	{
		s_bringupFailed = true;
		Warning(eDLL_T::MS, "[IMGUI] overlay bring-up failed -- console and server browser are unavailable\n");
		return false;
	}

	imguiSystem->AddSurface(&g_Console);
	imguiSystem->AddSurface(&g_Browser);
	imguiSystem->AddSurface(&g_streamOverlay);
	imguiSystem->AddSurface(&g_TopBar);
	imguiSystem->AddSurface(&g_DevMenu);
	imguiSystem->AddSurface(&g_DlssNrMenu);

	SDK_Log("[IMGUI] overlay ready (Console+Browser+StreamOverlay+TopBar+DevMenu+DlssNr)\n");
	return true;
}

//---------------------------------------------------------------------------
// The swapchain's own output window, which is the window the overlay must
// size and read input from -- DirectX_GetMainWindow only enumerates.
//---------------------------------------------------------------------------
static HWND Dx12_GetPresentWindow(IDXGISwapChain* const pSwapChain)
{
	DXGI_SWAP_CHAIN_DESC desc = {};

	if (pSwapChain && SUCCEEDED(pSwapChain->GetDesc(&desc)) && desc.OutputWindow)
		return desc.OutputWindow;

	return DirectX_GetMainWindow();
}

static void DrivePresentImGuiDX12(IDXGISwapChain* pSwapChain)
{
	CImguiSystem* const imguiSystem = ImguiSystem();
	++s_presentCount;

	Input_MaintainCursorClip(DirectX_GetMainWindow());

	if (s_imguiDisabled)
		return;
	if (!imguiSystem->IsInitialized() && !Imgui_Bringup(Dx12_GetPresentWindow(pSwapChain)))
		return;
	if (!Dx12_InitRenderState(pSwapChain))
		return;

	ImGuiContext* const ctx = ImGui::GetCurrentContext();
	if (!ctx)
		return;

	static volatile int s_lastStep = 0;
	__try
	{
		const UINT frameIndex = s_dx12SwapChain->GetCurrentBackBufferIndex();
		Dx12FrameContext& frame = s_dx12Frames[frameIndex];
		Dx12_WaitForFrame(frame);

		s_lastStep = 1;
		frame.CommandAllocator->Reset();
		s_dx12CommandList->Reset(frame.CommandAllocator, nullptr);

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = frame.RenderTarget;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		s_dx12CommandList->ResourceBarrier(1, &barrier);

		s_lastStep = 2;
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		Dx12_PollToggleFallback();

		s_lastStep = 3;
		ImguiSystem()->DrawSurfaces();
		ImGui::EndFrame();
		ImGui::Render();

		// Liveness sample: zero display size means a bad window; zero verts with console active means no emit.
		const bool consoleActive = g_Console.IsActivated();

		if (s_presentCount == 1 || s_presentCount == 600 ||
			(s_presentCount % 36000) == 0)
		{
			const ImGuiIO& io = ImGui::GetIO();
			const ImDrawData* const drawData = ImGui::GetDrawData();

			// Both window candidates, because DirectX_GetMainWindow prefers
			// g_pGame's handle and falls back to an enumerated one -- when they
			// disagree, ImGui is sized and fed input from the wrong window.
			const HWND hGame = (g_pGame ? g_pGame->GetWindow() : NULL);
			HWND hEnum = NULL;
			EnumWindows(DirectX_EnumWindowProc, reinterpret_cast<LPARAM>(&hEnum));
			RECT rc = {};
			GetClientRect(DirectX_GetMainWindow(), &rc);

			SDK_Log("[IMGUI-DX12] present=%lld display=%.0fx%.0f client=%ldx%ld "
				"gameHwnd=%p enumHwnd=%p consoleActive=%d vtx=%d\n",
				(long long)s_presentCount, io.DisplaySize.x, io.DisplaySize.y,
				rc.right - rc.left, rc.bottom - rc.top,
				(void*)hGame, (void*)hEnum, consoleActive ? 1 : 0,
				drawData ? drawData->TotalVtxCount : -1);
		}

		s_lastStep = 4;
		s_dx12CommandList->OMSetRenderTargets(1, &frame.RtvHandle, FALSE, nullptr);
		ID3D12DescriptorHeap* heaps[] = { s_dx12SrvHeap };
		s_dx12CommandList->SetDescriptorHeaps(1, heaps);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), s_dx12CommandList);

		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		s_dx12CommandList->ResourceBarrier(1, &barrier);
		s_dx12CommandList->Close();

		s_lastStep = 5;
		ID3D12CommandList* lists[] = { s_dx12CommandList };
		s_dx12CommandQueue->ExecuteCommandLists(1, lists);
		frame.FenceValue = ++s_dx12FenceLastSignaled;
		s_dx12CommandQueue->Signal(s_dx12Fence, frame.FenceValue);
		s_lastStep = 6;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		s_imguiDisabled = true;
		SDK_Log("[IMGUI-DX12] pipeline CRASHED at step %d (0x%08X)\n",
			(int)s_lastStep, GetExceptionCode());
		// Draw is dead but m_activated would still latch IsSurfaceActive /
		// g_bBlockInput (invisible modal + multi-mon free cursor).
		if (ImguiSystem())
			ImguiSystem()->ForceDeactivateModals();
	}
}

//---------------------------------------------------------------------------
// Per-frame ImGui pipeline shared by Present and Present1, and the only
// per-frame driver in DX11 mode. Callers gate through Dx12_ClaimFrameDriver
// so exactly one hook drives each frame.
//---------------------------------------------------------------------------
static void DrivePresentImGui(IDXGISwapChain* pSwapChain)
{
	CImguiSystem* const imguiSystem = ImguiSystem();

	++s_presentCount;

	Input_MaintainCursorClip(DirectX_GetMainWindow());

	if (s_imguiDisabled)
		return;
	if (!imguiSystem->IsInitialized() && !Imgui_Bringup(DirectX_GetMainWindow()))
		return;

	ImGuiContext* const ctx = ImGui::GetCurrentContext();
	if (!ctx)
		return;

	// s_lastStep is for the SEH handler if the pipeline faults.
	static volatile int s_lastStep = 0;
	__try
	{
		s_lastStep = 1;
		ImGui_ImplDX11_NewFrame();

		s_lastStep = 2;
		ImGui_ImplWin32_NewFrame();

		s_lastStep = 3;
		ImGui::NewFrame();

		// DrawSurfaces holds the input-event-queue mutex so WndProc cannot race.
		s_lastStep = 4;
		ImguiSystem()->DrawSurfaces();

		s_lastStep = 5;
		ImGui::EndFrame();

		s_lastStep = 6;
		ImGui::Render();

		// Bind back buffer and submit the draw data.
		s_lastStep = 7;
		ID3D11Device* const pDevice = *g_ppGameDevice;
		ID3D11DeviceContext* pCtx = nullptr;
		pDevice->GetImmediateContext(&pCtx);
		if (pCtx)
		{
			s_lastStep = 8;
			ID3D11Texture2D* pBackBuffer = nullptr;
			if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
				reinterpret_cast<void**>(&pBackBuffer))) && pBackBuffer)
			{
				s_lastStep = 9;
				ID3D11RenderTargetView* pRTV = nullptr;
				if (SUCCEEDED(pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRTV)) && pRTV)
				{
					s_lastStep = 10;
					pCtx->OMSetRenderTargets(1, &pRTV, nullptr);

					s_lastStep = 11;
					ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

					s_lastStep = 12;
					pRTV->Release();
				}
				pBackBuffer->Release();
			}
			pCtx->Release();
		}
		s_lastStep = 13;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		s_imguiDisabled = true;
		SDK_Log("[IMGUI-RT] pipeline CRASHED at step %d (0x%08X)\n",
			(int)s_lastStep, GetExceptionCode());
		// Draw is dead but m_activated would still latch IsSurfaceActive /
		// g_bBlockInput (invisible modal + multi-mon free cursor).
		if (ImguiSystem())
			ImguiSystem()->ForceDeactivateModals();
	}
}

//---------------------------------------------------------------------------
// Legacy Present (IDXGISwapChain slot 8). Setup-only on S21; Present1 is the frame path.
//---------------------------------------------------------------------------
HRESULT __stdcall Present(IDXGISwapChain* pSwapChain, UINT nSyncInterval, UINT nFlags)
{
	++s_presentHookCount;

	{
		const long long n = s_presentHookCount;
		if (n == 1 || n == 120 || n == 600 || (n % 3600) == 0)
		{
			SDK_Log("[IMGUI-DXGI] Present hook n=%lld flags=0x%X sc=%p drive=%lld mode=%s\n",
				n, nFlags, (void*)pSwapChain, (long long)s_presentCount,
				s_bDx12Mode ? "dx12" : "dx11");
			if (n == 1)
				Dx12_LogPresentCallers("Present");
		}
	}

	if (s_bDx12Mode)
		Dx12_CaptureGameSwapChain(pSwapChain);

	if (nFlags & DXGI_PRESENT_TEST)
		return s_fnSwapChainPresent(pSwapChain, nSyncInterval, nFlags);
	if (s_bDx12Mode)
	{
		if (Dx12_ClaimFrameDriver(DX12_DRIVER_DXGI_PRESENT))
			DrivePresentImGuiDX12(pSwapChain);
		return s_fnSwapChainPresent(pSwapChain, nSyncInterval, nFlags);
	}
	if (!g_ppGameDevice || !*g_ppGameDevice)
		return s_fnSwapChainPresent(pSwapChain, nSyncInterval, nFlags);

	// The DXGI Present hook fires every frame on S21, not just during setup
	// -- the claim keeps it and SpinPresent from driving the same frame.
	if (Dx12_ClaimFrameDriver(DX12_DRIVER_DXGI_PRESENT))
		DrivePresentImGui(pSwapChain);
	return s_fnSwapChainPresent(pSwapChain, nSyncInterval, nFlags);
}

//---------------------------------------------------------------------------
// Present1 (IDXGISwapChain1 slot 22) is the per-frame flip-model path.
//---------------------------------------------------------------------------
HRESULT __stdcall Present1(IDXGISwapChain1* pSwapChain, UINT nSyncInterval,
	UINT nFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
	++s_present1HookCount;

	// Reported from the hook entry, not the ImGui pipeline: when the overlay
	// stops drawing, the question is whether this hook still fires at all, and
	// the pipeline's own counter cannot answer that.
	{
		const long long n = s_present1HookCount;
		if (n == 1 || n == 120 || n == 600 || (n % 3600) == 0)
			SDK_Log("[IMGUI-DXGI] Present1 hook n=%lld flags=0x%X sc=%p drive=%lld mode=%s\n",
				n, nFlags, (void*)pSwapChain, (long long)s_presentCount,
				s_bDx12Mode ? "dx12" : "dx11");
	}

	if (s_bDx12Mode)
		Dx12_CaptureGameSwapChain(static_cast<IDXGISwapChain*>(pSwapChain));

	if (nFlags & DXGI_PRESENT_TEST)
		return s_fnSwapChainPresent1(pSwapChain, nSyncInterval, nFlags, pPresentParameters);
	if (s_bDx12Mode)
	{
		if (Dx12_ClaimFrameDriver(DX12_DRIVER_DXGI_PRESENT))
			DrivePresentImGuiDX12(static_cast<IDXGISwapChain*>(pSwapChain));
		return s_fnSwapChainPresent1(pSwapChain, nSyncInterval, nFlags, pPresentParameters);
	}
	if (!g_ppGameDevice || !*g_ppGameDevice)
		return s_fnSwapChainPresent1(pSwapChain, nSyncInterval, nFlags, pPresentParameters);

	// IDXGISwapChain1 IS-A IDXGISwapChain - safe upcast.
	if (Dx12_ClaimFrameDriver(DX12_DRIVER_DXGI_PRESENT))
		DrivePresentImGui(static_cast<IDXGISwapChain*>(pSwapChain));
	return s_fnSwapChainPresent1(pSwapChain, nSyncInterval, nFlags, pPresentParameters);
}

//---------------------------------------------------------------------------
// Engine DXGI present wrapper (DX12). Last point before Present; swapchain is the second arg.
//---------------------------------------------------------------------------
typedef __int64(__fastcall* fnEnginePresent)(void* pThis, void* pFrameCtx);
static fnEnginePresent s_fnEnginePresent = NULL;

static __int64 __fastcall EnginePresent_Hook(void* pThis, void* pFrameCtx)
{
	const long long n = ++s_enginePresentCount;

	IDXGISwapChain* swapChain = nullptr;
	__try
	{
		void* const* const ctx = *reinterpret_cast<void* const* const*>(pFrameCtx);
		swapChain = reinterpret_cast<IDXGISwapChain*>(ctx[6]); // +0x30
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		swapChain = nullptr;
	}

	if (n == 1 || (n % 18000) == 0)
		SDK_Log("[IMGUI-DX12] EnginePresent n=%lld sc=%p drive=%lld\n",
			n, (void*)swapChain, (long long)s_presentCount);

	if (swapChain)
	{
		Dx12_CaptureGameSwapChain(swapChain);
		DlssSr_TryInstall();
		DlssNr_OnEnginePresent();
		DlssSr_PresentNrPass(swapChain);
		FrameGen_OnEnginePresent(pFrameCtx, &swapChain);

		if (Dx12_ClaimFrameDriver(DX12_DRIVER_ENGINE_PRESENT))
			DrivePresentImGuiDX12(swapChain);
	}

	return s_fnEnginePresent(pThis, pFrameCtx);
}

//---------------------------------------------------------------------------
// Engine swapchain wrapper resize (DX12). Zeroes its back-buffer table, calls
// IDXGISwapChain::ResizeBuffers and refills the table only on success; the
// caller then rebuilds wrappers from that table with no null check, and the
// engine's error reporter ignores everything but device loss. Every swapchain
// class passes through here, unlike the DXGI vtable hook, so this is where the
// overlay must drop its back-buffer references and where a failure gets loud.
//---------------------------------------------------------------------------
typedef __int64(__fastcall* fnEngineResize)(void* pEngineSwapChain, UINT nBufferCount,
	UINT nWidth, UINT nHeight, int nFormat, UINT nFlags);
static fnEngineResize s_fnEngineResize = NULL;

static __int64 __fastcall EngineResize_Hook(void* pEngineSwapChain, UINT nBufferCount,
	UINT nWidth, UINT nHeight, int nFormat, UINT nFlags)
{
	s_dx12InResize = true;
	Dx12_ShutdownRenderState();

	__int64 hr = s_fnEngineResize(pEngineSwapChain, nBufferCount, nWidth, nHeight, nFormat, nFlags);
	if (static_cast<int>(hr) < 0)
	{
		Dx12_ShutdownRenderState();
		const __int64 retry = s_fnEngineResize(pEngineSwapChain, nBufferCount, nWidth, nHeight, nFormat, nFlags);
		Warning(eDLL_T::MS,
			"[IMGUI-DXGI] engine resize %ux%u count=%u fmt=%d failed 0x%08X, retry 0x%08X\n",
			nWidth, nHeight, nBufferCount, nFormat,
			static_cast<unsigned>(hr), static_cast<unsigned>(retry));
		hr = retry;
	}
	else
	{
		SDK_Log("[IMGUI-DXGI] engine resize %ux%u count=%u fmt=%d ok\n",
			nWidth, nHeight, nBufferCount, nFormat);
	}

	s_dx12InResize = false;
	return hr;
}

static __int64 __fastcall SpinPresent_Hook(LARGE_INTEGER ticks)
{
	const long long n = ++s_spinPresentCount;
	if (n == 1 || n == 600 || (n % 18000) == 0)
		SDK_Log("[IMGUI-RT] SpinPresent n=%lld drive=%lld sc=%p\n",
			n, (long long)s_presentCount, (void*)s_dx12GameSwapChain);

	// On DX12 drive from the engine present wrapper unless another hook already claimed the frame.
	if (s_bDx12Mode && s_dx12GameSwapChain && Dx12_ClaimFrameDriver(DX12_DRIVER_SPIN_PRESENT))
		DrivePresentImGuiDX12(s_dx12GameSwapChain);

	// Drive ImGui before SpinPresent only if DXGI Present has not claimed this frame.
	if (g_ppSwapChain && *g_ppSwapChain
		&& g_ppGameDevice && *g_ppGameDevice
		&& Dx12_ClaimFrameDriver(DX12_DRIVER_SPIN_PRESENT))
	{
		DrivePresentImGui(*g_ppSwapChain);
	}

	return s_fnSpinPresent(ticks);
}


HRESULT __stdcall ResizeBuffers(IDXGISwapChain* pSwapChain, UINT nBufferCount, UINT nWidth, UINT nHeight, DXGI_FORMAT dxFormat, UINT nSwapChainFlags)
{
	///////////////////////////////////////////////////////////////////////////////
	if (g_pGame)
		g_pGame->SetWindowSize(nWidth, nHeight);
	SDK_Log("[IMGUI-DXGI] ResizeBuffers sc=%p %ux%u count=%u flags=0x%X mode=%s -- render state torn down\n",
		(void*)pSwapChain, nWidth, nHeight, nBufferCount, nSwapChainFlags,
		s_bDx12Mode ? "dx12" : "dx11");
	if (!s_bDx12Mode)
		return s_fnResizeBuffers(pSwapChain, nBufferCount, nWidth, nHeight, dxFormat, nSwapChainFlags);

	Dx12_CaptureGameSwapChain(pSwapChain);
	s_dx12InResize = true;
	Dx12_ShutdownRenderState();

	HRESULT hr = s_fnResizeBuffers(pSwapChain, nBufferCount, nWidth, nHeight, dxFormat, nSwapChainFlags);

	// The engine zeroes its back-buffer table before resizing and only refills it
	// on success, then indexes it unconditionally -- a failure here is a null
	// deref one call later, so retry once rather than hand it an empty table.
	if (FAILED(hr))
	{
		Dx12_ShutdownRenderState();
		const HRESULT retry = s_fnResizeBuffers(pSwapChain, nBufferCount, nWidth, nHeight,
			dxFormat, nSwapChainFlags);

		Warning(eDLL_T::MS, "[IMGUI-DXGI] ResizeBuffers failed 0x%08X, retry 0x%08X\n",
			static_cast<unsigned>(hr), static_cast<unsigned>(retry));
		hr = retry;
	}

	s_dx12InResize = false;
	return hr;
}

//#################################################################################
// INTERNALS
//#################################################################################

#pragma warning( push )
// Disable stack warning, tells us to move more data to the heap instead. Not really possible with 'initialData' here. Since its parallel processed.
// Also disable 6378, complains that there is no control path where it would use 'nullptr', if that happens 'Error' will be called though.
#pragma warning( disable : 6262 6387)
void(*v_CreateTextureResource)(TextureAsset_s*, INT_PTR);
constexpr uint32_t ALIGNMENT_SIZE = 15; // Creates 2D texture and shader resource from textureHeader and imageData.
void CreateTextureResource(TextureAsset_s* textureHeader, INT_PTR imageData)
{
	if (textureHeader->depth && !textureHeader->height) // Return never gets hit. Maybe its some debug check?
		return;

	i64 initialData[4096]{};
	textureHeader->textureMipLevels = textureHeader->permanentMipLevels;

	const int totalStreamedMips = textureHeader->optStreamedMipLevels + textureHeader->streamedMipLevels;
	int mipLevel = textureHeader->permanentMipLevels + totalStreamedMips;
	if (mipLevel != totalStreamedMips)
	{
		do
		{
			--mipLevel;
			if (textureHeader->arraySize)
			{
				int mipWidth = 0;
				if (textureHeader->width >> mipLevel > 1)
					mipWidth = (textureHeader->width >> mipLevel) - 1;

				int mipHeight = 0;
				if (textureHeader->height >> mipLevel > 1)
					mipHeight = (textureHeader->height >> mipLevel) - 1;

				const TextureBytesPerPixel_s& perPixel = s_pBytesPerPixel[textureHeader->imageFormat];

				const u8 x = perPixel.x;
				const u8 y = perPixel.y;

				const u32 bppWidth = (y + mipWidth) >> (y >> 1);
				const u32 bppHeight = (y + mipHeight) >> (y >> 1);
				const u32 sliceWidth = x * (y >> (y >> 1));

				const u32 rowPitch = sliceWidth * bppWidth;
				const u32 slicePitch = x * bppWidth * bppHeight;

				u32 subResourceEntry = mipLevel;
				for (int i = 0; i < textureHeader->arraySize; i++)
				{
					const u32 offsetCurrentResourceData = subResourceEntry << 4u;

					*(s64*)((u8*)initialData + offsetCurrentResourceData) = imageData;
					*(u32*)((u8*)&initialData[1] + offsetCurrentResourceData) = rowPitch;
					*(u32*)((u8*)&initialData[1] + offsetCurrentResourceData + 4) = slicePitch;

					imageData += (slicePitch + ALIGNMENT_SIZE) & ~ALIGNMENT_SIZE;
					subResourceEntry += textureHeader->permanentMipLevels;
				}
			}
		} while (mipLevel != totalStreamedMips);
	}

	const DXGI_FORMAT dxgiFormat = g_TxtrAssetToDxgiFormat[textureHeader->imageFormat]; // Get dxgi format

	D3D11_TEXTURE2D_DESC textureDesc{};
	textureDesc.Width = textureHeader->width >> mipLevel;
	textureDesc.Height = textureHeader->height >> mipLevel;
	textureDesc.MipLevels = textureHeader->permanentMipLevels;
	textureDesc.ArraySize = textureHeader->arraySize;
	textureDesc.Format = dxgiFormat;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;
	textureDesc.Usage = textureHeader->usageFlags != 2 ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	textureDesc.MiscFlags = 2 * (textureHeader->layerCount & 2);

	const u32 offsetStartResourceData = mipLevel << 4u;
	const D3D11_SUBRESOURCE_DATA* subResData = (D3D11_SUBRESOURCE_DATA*)((uint8_t*)initialData + offsetStartResourceData);

	const HRESULT createTextureRes = D3D11Device()->CreateTexture2D(&textureDesc, subResData, &textureHeader->pInputTexture);
	if (createTextureRes < S_OK)
		Error(eDLL_T::RTECH, EXIT_FAILURE, "Couldn't create texture \"%s\" (%llX): error code = %08x\n",
			textureHeader->debugName, textureHeader->assetGuid, createTextureRes);

	D3D11_SHADER_RESOURCE_VIEW_DESC shaderResource{};
	shaderResource.Format = dxgiFormat;
	shaderResource.Texture2D.MipLevels = textureHeader->textureMipLevels;

	const u8 arraySize = textureHeader->arraySize;

	if (arraySize > 1) // Do we have a texture array?
	{
		const bool isCubeMap = (textureHeader->layerCount & 2);

		if (!isCubeMap)
		{
			shaderResource.ViewDimension = D3D_SRV_DIMENSION_TEXTURE2DARRAY;
			shaderResource.Texture2DArray.FirstArraySlice = 0;
			shaderResource.Texture2DArray.ArraySize = arraySize;
		}
		else
		{
			// Cube textures have 6 faces to form the cube; one texture per
			// cube face. If we have more, we have a cube map array.
			if (arraySize == 6)
				shaderResource.ViewDimension = D3D_SRV_DIMENSION_TEXTURECUBE;
			else
			{
				// Must have a multiple of 6 textures per cube in the array,
				// else we have one or more cubes with missing faces.
				Assert(arraySize % 6 == 0);

				shaderResource.ViewDimension = D3D_SRV_DIMENSION_TEXTURECUBEARRAY;
				shaderResource.Texture2DArray.FirstArraySlice = 0;
				shaderResource.Texture2DArray.ArraySize = arraySize / 6;
			}
		}
	}
	else
	{
		shaderResource.ViewDimension = D3D_SRV_DIMENSION_TEXTURE2D;
	}

	const HRESULT createShaderResourceRes = D3D11Device()->CreateShaderResourceView(textureHeader->pInputTexture, &shaderResource, &textureHeader->pShaderResourceView);
	if (createShaderResourceRes < S_OK)
		Error(eDLL_T::RTECH, EXIT_FAILURE, "Couldn't create shader resource view for texture \"%s\" (%llX): error code = %08x\n", 
			textureHeader->debugName, textureHeader->assetGuid, createShaderResourceRes);
}
#pragma warning( pop )

static D3D12_CPU_DESCRIPTOR_HANDLE Dx12_GetSrvCpuHandle(const UINT slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = s_dx12SrvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += SIZE_T(slot) * SIZE_T(s_dx12SrvDescriptorSize);
	return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE Dx12_GetSrvGpuHandle(const UINT slot)
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle = s_dx12SrvHeap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += UINT64(slot) * UINT64(s_dx12SrvDescriptorSize);
	return handle;
}

static bool Dx12_UploadTextureRGBA(const unsigned char* pImageData, const int width, const int height,
	uint64_t* out_imgui_texture_id, ID3D12Resource** out_texture, ID3D12Resource** out_upload)
{
	if (!s_dx12Device || !s_dx12CommandQueue || !s_dx12SrvHeap || !out_imgui_texture_id || !out_texture || !out_upload)
		return false;
	if (s_dx12SrvNextDescriptor >= s_dx12SrvDescriptorCapacity)
	{
		SDK_Log("[IMGUI-DX12] texture SRV heap exhausted (%u descriptors)\n", s_dx12SrvDescriptorCapacity);
		return false;
	}

	const UINT srvSlot = s_dx12SrvNextDescriptor++;

	D3D12_RESOURCE_DESC textureDesc = {};
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	textureDesc.Width = UINT64(width);
	textureDesc.Height = UINT(height);
	textureDesc.DepthOrArraySize = 1;
	textureDesc.MipLevels = 1;
	textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

	ID3D12Resource* texture = nullptr;
	HRESULT hr = s_dx12Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
		&textureDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
	if (FAILED(hr) || !texture)
		return false;

	UINT64 uploadBufferSize = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT numRows = 0;
	UINT64 rowSizeInBytes = 0;
	s_dx12Device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &uploadBufferSize);

	D3D12_RESOURCE_DESC uploadDesc = {};
	uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	uploadDesc.Width = uploadBufferSize;
	uploadDesc.Height = 1;
	uploadDesc.DepthOrArraySize = 1;
	uploadDesc.MipLevels = 1;
	uploadDesc.SampleDesc.Count = 1;
	uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

	ID3D12Resource* upload = nullptr;
	hr = s_dx12Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
		&uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
	if (FAILED(hr) || !upload)
	{
		texture->Release();
		return false;
	}

	unsigned char* mapped = nullptr;
	D3D12_RANGE readRange = {};
	hr = upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
	if (FAILED(hr) || !mapped)
	{
		upload->Release();
		texture->Release();
		return false;
	}

	const size_t srcPitch = size_t(width) * 4u;
	unsigned char* dst = mapped + footprint.Offset;
	for (UINT row = 0; row < numRows; ++row)
		memcpy(dst + size_t(row) * footprint.Footprint.RowPitch, pImageData + size_t(row) * srcPitch, srcPitch);
	upload->Unmap(0, nullptr);

	ID3D12CommandAllocator* allocator = nullptr;
	ID3D12GraphicsCommandList* commandList = nullptr;
	ID3D12Fence* fence = nullptr;
	HANDLE fenceEvent = NULL;
	bool ok = false;

	if (SUCCEEDED(s_dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
		&& SUCCEEDED(s_dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&commandList)))
		&& SUCCEEDED(s_dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
	{
		D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
		dstLoc.pResource = texture;
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLoc.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
		srcLoc.pResource = upload;
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLoc.PlacedFootprint = footprint;

		commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		commandList->ResourceBarrier(1, &barrier);

		if (SUCCEEDED(commandList->Close()))
		{
			ID3D12CommandList* lists[] = { commandList };
			s_dx12CommandQueue->ExecuteCommandLists(1, lists);
			fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
			if (fenceEvent && SUCCEEDED(s_dx12CommandQueue->Signal(fence, 1)))
			{
				if (fence->GetCompletedValue() < 1)
				{
					fence->SetEventOnCompletion(1, fenceEvent);
					WaitForSingleObject(fenceEvent, INFINITE);
				}
				ok = true;
			}
		}
	}

	if (fenceEvent)
		CloseHandle(fenceEvent);
	SafeReleaseT(fence);
	SafeReleaseT(commandList);
	SafeReleaseT(allocator);

	if (!ok)
	{
		upload->Release();
		texture->Release();
		return false;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = textureDesc.Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	s_dx12Device->CreateShaderResourceView(texture, &srvDesc, Dx12_GetSrvCpuHandle(srvSlot));

	*out_texture = texture;
	*out_upload = upload;
	*out_imgui_texture_id = Dx12_GetSrvGpuHandle(srvSlot).ptr;
	return true;
}

bool LoadTextureBuffer(unsigned char* buffer, int len, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height,
	uint64_t* out_imgui_texture_id, ID3D12Resource** out_dx12_texture, ID3D12Resource** out_dx12_upload)
{
	// Load PNG buffer to a raw RGBA buffer
	int nImageWidth = 0;
	int nImageHeight = 0;
	unsigned char* pImageData = stbi_load_from_memory(buffer, len, &nImageWidth, &nImageHeight, NULL, 4);

	if (!pImageData)
	{
		assert(pImageData);
		return false;
	}

	if (DirectX_IsDx12Mode())
	{
		const bool ok = Dx12_UploadTextureRGBA(pImageData, nImageWidth, nImageHeight,
			out_imgui_texture_id, out_dx12_texture, out_dx12_upload);
		*out_width = nImageWidth;
		*out_height = nImageHeight;
		if (out_srv)
			*out_srv = nullptr;
		stbi_image_free(pImageData);
		return ok;
	}

	///////////////////////////////////////////////////////////////////////////////
	ID3D11Texture2D* pTexture = nullptr;
	D3D11_TEXTURE2D_DESC            desc;
	D3D11_SUBRESOURCE_DATA          subResource;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;

	///////////////////////////////////////////////////////////////////////////////
	ZeroMemory(&desc, sizeof(desc));
	desc.Width = nImageWidth;
	desc.Height = nImageHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;

	///////////////////////////////////////////////////////////////////////////////
	subResource.pSysMem = pImageData;
	subResource.SysMemPitch = desc.Width * 4;
	subResource.SysMemSlicePitch = 0;
	D3D11Device()->CreateTexture2D(&desc, &subResource, &pTexture);

	// Create texture view
	ZeroMemory(&srvDesc, sizeof(srvDesc));
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = desc.MipLevels;
	srvDesc.Texture2D.MostDetailedMip = 0;

	if (pTexture)
	{
		D3D11Device()->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
		pTexture->Release();
	}

	*out_width = nImageWidth;
	*out_height = nImageHeight;
	if (out_imgui_texture_id)
		*out_imgui_texture_id = reinterpret_cast<uint64_t>(*out_srv);
	stbi_image_free(pImageData);

	return true;
}

// True when the player is in relative-look gameplay (cursor should hide +
// mouse capture). False on main menu, loading, and when GameUI is up.
// g_pClientState is never resolved in this product -- do not deref it.
static bool Input_WantGameplayLookCursor(void)
{
	if (g_pEngineVGui && g_pEngineVGui->IsGameUIVisible())
		return false;

	return Input_WasLookCursorHidden();
}

void ResetInput()
{
	// g_pInputSystem is assigned before the engine singleton exists; re-resolve here.
	// Clip to the game window while focused. Never ClipCursor(nullptr).
	extern bool InputSystem_ResolveSingletonFromEngine();
	const bool surfaceActive = ImguiSystem()->IsSurfaceActive();
	const bool gameplayLook = !surfaceActive && Input_WantGameplayLookCursor();

	if (InputSystem_ResolveSingletonFromEngine())
	{
		// Poll gate only. Do not force SetMouseCursorVisible; menu cursor is InputStack-owned.
		g_pInputSystem->EnableInput(!surfaceActive);

		if (surfaceActive)
		{
			g_pInputSystem->DisableMouseCapture();
		}
		else if (gameplayLook)
		{
			const PlatWindow_t hAttached = g_pInputSystem->GetAttachedWindow();
			if (hAttached)
				g_pInputSystem->EnableMouseCapture(hAttached);
		}
		else
		{
			g_pInputSystem->DisableMouseCapture();
		}
	}

	const HWND hWnd = DirectX_GetMainWindow();
	if (hWnd)
		Input_NoteGameWindow(hWnd);

	if (!surfaceActive)
	{
		g_bBlockInput.exchange(false);
		if (hWnd)
			Input_RestoreCursorClip(hWnd);

		if (gameplayLook)
			Input_EnsureCursorHidden();
		else
			Input_EnsureCursorVisible();
	}
	else
	{
		if (!g_bBlockInput.exchange(true))
			Input_ReleaseCursorClip(hWnd);
	}
}

bool PanelsVisible()
{
	if (ImguiSystem()->IsSurfaceActive())
	{
		return true;
	}
	return false;
}

//#################################################################################
// ENTRYPOINT
//#################################################################################

void DirectX_Init()
{
	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(NULL, exePath, SDK_ARRAYSIZE(exePath));
	// Filename gate only: r5apex_dx12.exe vs r5apex.exe. Path substrings like
	// a folder named "dx12" must not flip the overlay into the D3D12 pipeline.
	const char* exeLeaf = exePath;
	for (const char* p = exePath; *p; ++p)
	{
		if (*p == '\\' || *p == '/')
			exeLeaf = p + 1;
	}
	s_bDx12Mode = V_stristr(exeLeaf, "dx12") != nullptr;
	SDK_Log("DirectX_Init: begin exe=%s mode=%s\n",
		exeLeaf, s_bDx12Mode ? "dx12" : "dx11");

	// Begin the detour transaction
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

	IDXGISwapChain* hookSwapChain = nullptr;
	ID3D12Device* dummyDevice = nullptr;
	ID3D12CommandQueue* dummyQueue = nullptr;
	IDXGIFactory4* dummyFactory = nullptr;
	HWND dummyWindow = NULL;

	if (s_bDx12Mode)
	{
		SDK_Log("DirectX_Init: DX12 mode, creating temporary swapchain for DXGI vtable hooks\n");

		WNDCLASSEXA wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = GetModuleHandleA(NULL);
		wc.lpszClassName = "cafe_dx12_hook_window";
		RegisterClassExA(&wc);
		dummyWindow = CreateWindowExA(0, wc.lpszClassName, "cafe_dx12_hook_window",
			WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);

		if (dummyWindow
			&& SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dummyFactory)))
			&& SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice))))
		{
			D3D12_COMMAND_QUEUE_DESC queueDesc = {};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (SUCCEEDED(dummyDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dummyQueue))))
			{
				DXGI_SWAP_CHAIN_DESC1 desc = {};
				desc.BufferCount = 2;
				desc.Width = 16;
				desc.Height = 16;
				desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				desc.SampleDesc.Count = 1;
				desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

				IDXGISwapChain1* swapChain1 = nullptr;
				if (SUCCEEDED(dummyFactory->CreateSwapChainForHwnd(dummyQueue, dummyWindow,
					&desc, nullptr, nullptr, &swapChain1)))
				{
					hookSwapChain = static_cast<IDXGISwapChain*>(swapChain1);
				}
			}
		}

		if (!hookSwapChain)
		{
			SDK_Log("DirectX_Init: DX12 temporary swapchain creation FAILED\n");
			DetourTransactionAbort();
			SafeReleaseT(dummyQueue);
			SafeReleaseT(dummyDevice);
			SafeReleaseT(dummyFactory);
			if (dummyWindow)
				DestroyWindow(dummyWindow);
			return;
		}
	}
	else
	{
		hookSwapChain = *g_ppSwapChain;
	}

	SDK_Log("DirectX_Init: reading vtable from SwapChain %p\n", (void*)hookSwapChain);

	// Hook SwapChain
	DWORD_PTR* pSwapChainVtable = *reinterpret_cast<DWORD_PTR**>(hookSwapChain);

	SDK_Log("DirectX_Init: vtable=%p, Present[%d], Resize[%d]\n",
		(void*)pSwapChainVtable,
		(int)DXGISwapChainVTbl::Present,
		(int)DXGISwapChainVTbl::ResizeBuffers);

	int pIDX = static_cast<int>(DXGISwapChainVTbl::Present);
	s_fnSwapChainPresent = reinterpret_cast<IDXGISwapChainPresent>(pSwapChainVtable[pIDX]);

	int rIDX = static_cast<int>(DXGISwapChainVTbl::ResizeBuffers);
	s_fnResizeBuffers = reinterpret_cast<IDXGIResizeBuffers>(pSwapChainVtable[rIDX]);

	// Flip-model Present1 is vtable slot 22 on the same IDXGISwapChain1 table. No QueryInterface.
	constexpr int kPresent1VtblSlot = 22;
	s_fnSwapChainPresent1 = reinterpret_cast<IDXGISwapChain1Present1_t>(
		pSwapChainVtable[kPresent1VtblSlot]);

	SDK_Log("DirectX_Init: Present=%p, Present1=%p, ResizeBuffers=%p\n",
		(void*)s_fnSwapChainPresent, (void*)s_fnSwapChainPresent1,
		(void*)s_fnResizeBuffers);

	if (s_bDx12Mode && dummyQueue)
	{
		constexpr int kExecuteCommandListsVtblSlot = 10;
		DWORD_PTR* pCommandQueueVtable = *reinterpret_cast<DWORD_PTR**>(dummyQueue);
		s_fnD3D12ExecuteCommandLists =
			reinterpret_cast<ID3D12CommandQueueExecuteCommandLists_t>(
				pCommandQueueVtable[kExecuteCommandListsVtblSlot]);
		SDK_Log("DirectX_Init: D3D12 ExecuteCommandLists=%p\n",
			(void*)s_fnD3D12ExecuteCommandLists);
		DetourAttach(&(LPVOID&)s_fnD3D12ExecuteCommandLists,
			(PBYTE)D3D12ExecuteCommandLists_Hook);
	}

	// Present / Present1 are the DXGI-level entry points for both DX11 and
	// DX12. DX12 initializes the ImGui renderer lazily from the real game
	// swapchain on the first Present/Present1 call.
	DetourAttach(&(LPVOID&)s_fnSwapChainPresent,  (PBYTE)Present);
	DetourAttach(&(LPVOID&)s_fnSwapChainPresent1, (PBYTE)Present1);
	DetourAttach(&(LPVOID&)s_fnResizeBuffers,     (PBYTE)ResizeBuffers);

	// The engine's own present driver. Both images route every frame through
	// it while the DXGI Present/Present1 hooks above fire only once at
	// startup, so this is the per-frame entry point on DX11 AND DX12.
	{
		// DX11 inlines Present into a large frame; DX12 is a small push rbx / sub rsp,20 wrapper.
		CMemory spinMem = Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 4C 24 ?? 57 48 81 EC ?? ?? ?? ?? "
			"FF 05 ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 8D 53 FF B9 04 00 00 00 E8");
		if (!spinMem.GetPtr())
			spinMem = Module_FindPattern(g_GameDll,
				"40 53 48 83 EC 20 FF 05 ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
				"84 C0 74 ?? 8D 53 FF B9 04 00 00 00 E8");

		if (s_bDx12Mode)
		{
			CMemory presentMem = Module_FindPattern(g_GameDll,
				"40 53 48 81 EC 40 08 00 00 48 8B 02 44 0F B6 4A 0C 48 8B 48 30 "
				"41 0F B6 C1 F6 D8 45 1B C0 33 D2 4C 8B 11");
			if (presentMem.GetPtr())
			{
				s_fnEnginePresent = reinterpret_cast<fnEnginePresent>(presentMem.GetPtr());
				SDK_Log("DirectX_Init: engine present wrapper at %p, hooking\n",
					(void*)s_fnEnginePresent);
				DetourAttach(&(LPVOID&)s_fnEnginePresent, (PBYTE)EnginePresent_Hook);
			}
			else
			{
				SDK_Log("DirectX_Init: engine present wrapper pattern NOT FOUND\n");
			}

			CMemory resizeMem = Module_FindPattern(g_GameDll,
				"40 55 56 41 54 41 57 48 8D 6C 24 D1 48 81 EC E8 00 00 00 45 33 E4 "
				"45 8B D8 48 8B F1 45 8B D4 44 39 61 40");
			if (resizeMem.GetPtr())
			{
				s_fnEngineResize = reinterpret_cast<fnEngineResize>(resizeMem.GetPtr());
				SDK_Log("DirectX_Init: engine resize wrapper at %p, hooking\n",
					(void*)s_fnEngineResize);
				DetourAttach(&(LPVOID&)s_fnEngineResize, (PBYTE)EngineResize_Hook);
			}
			else
			{
				Warning(eDLL_T::MS, "[IMGUI-DXGI] engine resize wrapper pattern NOT FOUND -- "
					"a resize on an unhooked swapchain class will crash the engine\n");
			}
		}

		if (spinMem.GetPtr())
		{
			s_fnSpinPresent = reinterpret_cast<fnSpinPresent>(spinMem.GetPtr());
			SDK_Log("DirectX_Init: SpinPresent resolved to %p, hooking\n",
				(void*)s_fnSpinPresent);
			DetourAttach(&(LPVOID&)s_fnSpinPresent, (PBYTE)SpinPresent_Hook);
		}
		else
		{
			SDK_Log("DirectX_Init: SpinPresent pattern NOT FOUND -- overlay has no per-frame driver\n");
		}
	}

	// Commit the transaction
	HRESULT hr = DetourTransactionCommit();
	SDK_Log("DirectX_Init: detour commit hr=0x%08X\n", hr);

	if (s_bDx12Mode)
	{
		if (hookSwapChain)
			hookSwapChain->Release();
		SafeReleaseT(dummyQueue);
		SafeReleaseT(dummyDevice);
		SafeReleaseT(dummyFactory);
		if (dummyWindow)
			DestroyWindow(dummyWindow);
	}

	if (hr != NO_ERROR)
	{
		SDK_Log("DirectX_Init: DETOUR FAILED\n");
		return;
	}

	// DX12 bootstraps off a temp swapchain before the game has a window; wait for the frame driver.
	if (!s_bDx12Mode)
		Imgui_Bringup(DirectX_GetMainWindow());
}

void DirectX_Shutdown()
{
	// Begin the detour transaction
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

	// Unhook SwapChain
	DetourDetach(&(LPVOID&)s_fnSwapChainPresent,  (PBYTE)Present);
	if (s_fnSwapChainPresent1)
		DetourDetach(&(LPVOID&)s_fnSwapChainPresent1, (PBYTE)Present1);
	DetourDetach(&(LPVOID&)s_fnResizeBuffers,     (PBYTE)ResizeBuffers);
	if (s_fnSpinPresent)
		DetourDetach(&(LPVOID&)s_fnSpinPresent, (PBYTE)SpinPresent_Hook);
	if (s_fnEnginePresent)
		DetourDetach(&(LPVOID&)s_fnEnginePresent, (PBYTE)EnginePresent_Hook);
	if (s_fnEngineResize)
		DetourDetach(&(LPVOID&)s_fnEngineResize, (PBYTE)EngineResize_Hook);
	if (s_fnD3D12ExecuteCommandLists)
		DetourDetach(&(LPVOID&)s_fnD3D12ExecuteCommandLists, (PBYTE)D3D12ExecuteCommandLists_Hook);

	// Commit the transaction
	DetourTransactionCommit();

	if (s_bDx12Mode)
	{
		FrameGen_Shutdown();
		DlssNr_Shutdown();
		DlssSr_Shutdown();
		Dx12_ShutdownRenderState();
		SafeReleaseT(s_dx12GameSwapChain);
	}

	if (ImguiSystem()->IsInitialized())
	{
		ImguiSystem()->Shutdown();
	}
}

void VDXGI::GetAdr(void) const
{
	///////////////////////////////////////////////////////////////////////////////
	LogFunAdr("IDXGISwapChain::Present", s_fnSwapChainPresent);
	LogFunAdr("CreateTextureResource", v_CreateTextureResource);
	LogVarAdr("g_pSwapChain", g_ppSwapChain);
	LogVarAdr("g_pGameDevice", g_ppGameDevice);
	LogVarAdr("g_pImmediateContext", g_ppImmediateContext);
}

void VDXGI::GetFun(void) const
{
	// S21: CreateTextureResource pattern -- TODO: find S21 equivalent
	// Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 4C 8B C7 48 8B D5 48 8B CB 48 83 C4 60").FollowNearCallSelf.GetPtr(v_CreateTextureResource);
}

void VDXGI::GetVar(void) const
{
	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(NULL, exePath, SDK_ARRAYSIZE(exePath));
	const char* exeLeaf = exePath;
	for (const char* p = exePath; *p; ++p)
	{
		if (*p == '\\' || *p == '/')
			exeLeaf = p + 1;
	}
	if (V_stristr(exeLeaf, "dx12"))
	{
		SDK_Log("VDXGI: DX12 executable detected (%s), using DXGI hook bootstrap instead of D3D11 globals\n",
			exeLeaf);
		g_ppGameDevice = nullptr;
		g_ppImmediateContext = nullptr;
		g_ppSwapChain = nullptr;
		return;
	}

	// D3D11 globals: device 0x74D8838, context 0x74D8840, swapchain 0xCF045B8.
	const uintptr_t base = g_GameDll.GetModuleBase();

	g_ppGameDevice       = reinterpret_cast<ID3D11Device**>(base + 0x74D8838);
	g_ppImmediateContext = reinterpret_cast<ID3D11DeviceContext**>(base + 0x74D8840);
	g_ppSwapChain        = reinterpret_cast<IDXGISwapChain**>(base + 0xCF045B8);
}

void VDXGI::Detour(const bool bAttach) const
{
	// S21: CreateTextureResource pattern not yet resolved.
	if (v_CreateTextureResource)
		DetourSetup(&v_CreateTextureResource, &CreateTextureResource, bAttach);
}

#else // !CLIENT_DLL
#include "core/stdafx.h"
#endif // CLIENT_DLL
