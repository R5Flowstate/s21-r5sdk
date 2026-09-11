//=============================================================================//
//
// Purpose: DLSS Super Resolution on the S21 DX12 client.
//
// S21's live temporal pass is native TSAA. The exported FSR2 API is linked
// but never called, so SuperSampling rides Tsaa_Resolve. The FSR2 hooks stay
// as an observer. AMD and a missing NGX runtime stay on TSAA.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_bridge_addrs.h"

#include "windows/dlss_sr.h"
#include "windows/dlssnr.h"
#include "windows/id3dx.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "tier0/dbg.h"
#include "mathlib/mathlib.h"
#include "mathlib/vmatrix.h"
#include "game/client/viewrender.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstring>
#pragma comment(lib, "d3dcompiler.lib")

static ConVar settings_dlss_sr("settings_dlss_sr", "0", FCVAR_RELEASE,
	"Replace the engine TSAA resolve with NGX SuperSampling on NVIDIA DX12. 0 = stock TSAA. Native-res SuperSampling is DLAA and is a quality cost, not a perf win.");
static ConVar sdk_dlss_sr_probe("sdk_dlss_sr_probe", "1", FCVAR_DEVELOPMENTONLY,
	"Log the TSAA/FSR2 upscale pass. 0 = off, 1 = first and every change, 2 = every dispatch.");
static ConVar settings_dlss_sr_skip_tsaa("settings_dlss_sr_skip_tsaa", "1", FCVAR_RELEASE,
	"Skip the stock TSAA pixel dispatch while SuperSampling owns the resolve output. 0 = stack both passes.");
static ConVar sdk_dlss_mv_source("sdk_dlss_mv_source", "0", FCVAR_RELEASE,
	"Motion vector source. 0 = engine _rt_MRT1. 2 = none. 1 is refused (camera reconstruct binds a heap on the engine list).");
// _rt_FullFrameFB is the jittered pre-resolve scene colour. NR is a full-frame
// enhancement network, not a temporal resolve, so it wants the image TSAA has
// already converged; reading the resolve target after the stock pixel dispatch
// gives it that on the same command list.
static ConVar sdk_dlssnr_color_src("sdk_dlssnr_color_src", "0", FCVAR_RELEASE,
	"NR colour input. 0 = the TSAA resolve target after the stock dispatch, 1 = pre-resolve _rt_FullFrameFB.");
// Scene colour is HDR linear -- measured max 24.5 on a lit interior. The network
// is display-referred, so bright sources land far outside its trained domain and
// come back as colour artifacts. Encode to a display curve for the network, then
// re-apply its change to the HDR original as a bounded ratio so range survives.
// The backbuffer at engine-present time is the finished, tonemapped frame, and
// the list we submit it on is our own -- so the network gets display-referred
// colour and nothing of the engine's command-list state is disturbed.
static ConVar sdk_dlssnr_present_pass("sdk_dlssnr_present_pass", "1", FCVAR_RELEASE,
	"Run Neural Rendering over the backbuffer at present on a private command list. 0 = run it at the TSAA resolve instead.");
static ConVar sdk_dlssnr_tonemap("sdk_dlssnr_tonemap", "0", FCVAR_RELEASE,
	"Run NR on a display-referred encoding of scene colour and recombine as a ratio. 0 = hand it raw HDR.");
static ConVar sdk_dlssnr_tonemap_clamp("sdk_dlssnr_tonemap_clamp", "4.0", FCVAR_RELEASE,
	"Largest factor by which the NR recombine may scale a pixel. Bounds the reconstruction near white.");
static ConVar sdk_dlssnr_range_probe("sdk_dlssnr_range_probe", "0", FCVAR_DEVELOPMENTONLY,
	"Read back the NR colour input every N evaluates and log its value range. 0 = off. The network is display-referred; values far above 1.0 are the tell.");
// NVSDK_NGX_DLSS_Feature_Flags: AutoExposure is 1<<6 (64), not 1<<5. 1<<5 is DoSharpening.
static ConVar sdk_dlss_sr_flags("sdk_dlss_sr_flags", "75", FCVAR_RELEASE,
	"NGX DLSS.Feature.Create.Flags. 1=IsHDR 2=MVLowRes 4=MVJittered 8=DepthInverted 16=DisableSubrect 32=DoSharpening 64=AutoExposure. Default 75 = IsHDR|MVLowRes|DepthInverted|AutoExposure.");

// FSR 2.1 public layout. FfxResource carries the raw ID3D12Resource* because
// ffxGetResourceDX12 stores the D3D12 pointer verbatim.
struct FfxSrResourceDescription
{
	uint32_t type;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t mipCount;
	uint32_t flags;
};

struct FfxSrResource
{
	void* resource;
	FfxSrResourceDescription description;
	uint32_t state;
	bool isDepth;
	uint8_t pad[7];
	uint64_t descriptorData;
};
static_assert(sizeof(FfxSrResource) == 56, "FSR 2.1 FfxResource is 56 bytes");

struct FfxSrFloat2 { float x, y; };
struct FfxSrDim2 { uint32_t width, height; };

struct FfxSrDispatchDescription
{
	void* commandList;
	FfxSrResource color;
	FfxSrResource depth;
	FfxSrResource motionVectors;
	FfxSrResource exposure;
	FfxSrResource reactive;
	FfxSrResource transparencyAndComposition;
	FfxSrResource output;
	FfxSrFloat2 jitterOffset;
	FfxSrFloat2 motionVectorScale;
	FfxSrDim2 renderSize;
	bool enableSharpening;
	uint8_t pad0[3];
	float sharpness;
	float frameTimeDelta;
	float preExposure;
	bool reset;
	uint8_t pad1[3];
	float cameraNear;
	float cameraFar;
	float cameraFovAngleVertical;
};
static_assert(sizeof(FfxSrDispatchDescription) == 456, "FSR 2.1 dispatch description is 456 bytes");

struct FfxSrContextDescription
{
	uint32_t flags;
	FfxSrDim2 maxRenderSize;
	FfxSrDim2 displaySize;
	uint8_t pad0[4];
	uint8_t callbacks[112];
	void* device;
};
static_assert(sizeof(FfxSrContextDescription) == 144, "FSR 2.1 context description is 144 bytes");

typedef int32_t(__cdecl* PfnFfxFsr2ContextCreate)(void* context, const FfxSrContextDescription* desc);
typedef int32_t(__cdecl* PfnFfxFsr2ContextDispatch)(void* context, const FfxSrDispatchDescription* desc);
typedef int32_t(__cdecl* PfnFfxFsr2ContextDestroy)(void* context);

static PfnFfxFsr2ContextCreate s_fnCreate = nullptr;
static PfnFfxFsr2ContextDispatch s_fnDispatch = nullptr;
static PfnFfxFsr2ContextDestroy s_fnDestroy = nullptr;

static bool s_installed = false;
static bool s_installFailed = false;
static bool s_sawContext = false;
static bool s_sawDispatch = false;

static uint32_t s_ctxFlags = 0;
static FfxSrDim2 s_maxRenderSize = {};
static FfxSrDim2 s_displaySize = {};
static FfxSrDim2 s_lastRenderSize = {};
static uint64_t s_dispatchCount = 0;
static uint64_t s_presentsSinceInstall = 0;
static bool s_reportedIdle = false;

// Tsaa_Resolve -- unique in r5apex_dx12.
typedef int64_t(__fastcall* PfnTsaaResolve)(int mode, void* params);
static PfnTsaaResolve s_fnTsaaResolve = nullptr;

// Draw submit the resolve records its fullscreen pass through. The resolve
// body reaches it exactly once and through no other callee, so a thread-scoped
// window around the original call selects the pixel dispatch alone; the RT and
// texture binds keep running.
typedef int64_t(__fastcall* PfnTsaaDraw)(unsigned int a1, int64_t a2, int64_t a3,
	int a4, int a5, int a6, int64_t a7, unsigned int a8, unsigned int a9);
static PfnTsaaDraw s_fnTsaaDraw = nullptr;
static bool s_tsaaDrawInstalled = false;
static void DlssSr_ReleasePresentPass(void);
static thread_local bool t_skipTsaaDraw = false;
static thread_local void* t_nrAfterDrawParams = nullptr;
static thread_local int t_nrAfterDrawMode = 0;
static thread_local bool t_inTsaaResolve = false;
static thread_local ID3D12GraphicsCommandList* t_engineCmd = nullptr;
static uint64_t s_srDrawsSkipped = 0;
static bool s_tsaaInstalled = false;
static bool s_tsaaFailed = false;
static bool s_srLatched = false;
static bool s_loggedFail = false;
static uint64_t s_tsaaCount = 0;
static bool s_needFirstReset = true;
static uint64_t s_srEvalOk = 0;
static uint64_t s_srEvalFail = 0;
static uint64_t s_srLastOkTsaa = 0;
static uint64_t s_srSkippedViews = 0;
static uint64_t s_srBusySkips = 0;
static uint64_t s_presentSeq = 0;
static uint64_t s_srEvalPresent = 0;
static uint64_t s_srDupSkips = 0;

// Main-view key of the last successful SuperSampling create. A change in the
// input half means _rt_FullFrameFB was rebuilt (resolution change) -- the only
// event that recreates the feature.
static unsigned s_srKeyInW = 0;
static unsigned s_srKeyInH = 0;
static unsigned s_srKeyOutW = 0;
static unsigned s_srKeyOutH = 0;
static unsigned s_srKeyFlags = 0;

// The GPU may still be executing the previous private list when the job
// thread resolves again; each slot is fenced and a busy slot skips the frame.
static constexpr int kSrSlots = 3;
static ID3D12CommandAllocator* s_srAllocs[kSrSlots] = {};
static ID3D12GraphicsCommandList* s_srCmds[kSrSlots] = {};
static UINT64 s_srSlotFence[kSrSlots] = {};
static int s_srSlot = 0;
static ID3D12Fence* s_srFence = nullptr;
static UINT64 s_srFenceValue = 0;
static ID3D12Resource* s_dummyDepth = nullptr;
static ID3D12Resource* s_dummyMv = nullptr;
static ID3D12Resource* s_srOut = nullptr;
static D3D12_RESOURCE_STATES s_srOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static ID3D12Resource* s_nrColor = nullptr;
static D3D12_RESOURCE_STATES s_nrColorState = D3D12_RESOURCE_STATE_COPY_DEST;
static ID3D12Resource* s_nrMv = nullptr;
static D3D12_RESOURCE_STATES s_nrMvState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static ID3D12Resource* s_nrDepth = nullptr;
static ID3D12RootSignature* s_mvRoot = nullptr;
static ID3D12PipelineState* s_mvPso = nullptr;
static ID3D12DescriptorHeap* s_mvHeap = nullptr;
static ID3D12Resource* s_mvCb = nullptr;
static UINT s_mvDescSize = 0;
static bool s_havePrevOrigin = false;
static Vector3D s_prevOrigin;
// ITexture* for _rt_MRT1 -- standard-texture slot 20, and the motion input the
// stock resolve itself binds.
// Main-view depth. Both slots hold a texture asset directly, not an ITexture:
// 0x1B5E4D8 is "fullScreen_Zbuffer" and aliases 0x1B5E4C8, the "Swap chain
// depth buffer" that the stock resolve samples, when MSAA is off.
static constexpr size_t kTlsRhiCtx = 8736;
static constexpr size_t kRhiCtxBytes = 0x5200;
// RHIGraphicsContext field holding the recording ID3D12GraphicsCommandList.
static constexpr size_t kRhiCtxCmdList = 0x5090;
static char s_lastFail[96] = {};

// ITexture* stored by the _rt_FullFrameFB create. Not a TextureAsset*.
static constexpr size_t kTexAssetWidth = 0x0A;
static constexpr size_t kTexAssetHeight = 0x0C;
static constexpr size_t kTexAssetRhiTexture = 0x80;
static constexpr size_t kTexAssetRhiRt = 0x88;
static constexpr size_t kTexAssetRhiDepth = 0x90;
// RHITexture layout. m_textureSpec is the anchor: its first dword is the
// texture width, which must equal the owning asset's width.
static constexpr size_t kRhiTexSpec = 0x58;
static constexpr size_t kRhiTexParent = 0x50;
static constexpr size_t kRhiTexD3DResource = 0x70;
static constexpr int kRhiParentHops = 4;
static constexpr size_t kRhiDumpBytes = 0x98;


static constexpr size_t kResourceStateStride = 24;
static constexpr size_t kResourceStateCount = 4096;
static constexpr uint8_t kRhiStateCount = 16;

static constexpr size_t kTsaaParamsJitter = 0x20; // float2 c_jitter (NDC*0.5, y flipped). TLS+0x460 is viewport scale.

static constexpr int kNgxSuccess = 1;
static constexpr int kNgxAlreadyExists = static_cast<int>(0xBAD00003); // create keeps the live handle on a key mismatch

static void DlssSr_Log(const char* fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
	va_end(ap);
	Warning(eDLL_T::MS, "[DLSS-SR] %s", line);
	DlssNr_FileLog("[DLSS-SR] %s", line);
}

static void DlssSr_FailOnce(const char* why);
static bool DlssSr_ReadJitterUv(void* params, float& x, float& y);

static const char* DlssSr_FlagText(uint32_t flags, char* out, size_t cap)
{
	// FfxFsr2InitializationFlagBits
	_snprintf_s(out, cap, _TRUNCATE, "%s%s%s%s%s%s%s%s",
		(flags & 0x01) ? "HDR " : "",
		(flags & 0x02) ? "DISPLAY_RES_MV " : "",
		(flags & 0x04) ? "MV_JITTER_CANCEL " : "",
		(flags & 0x08) ? "DEPTH_INVERTED " : "",
		(flags & 0x10) ? "DEPTH_INFINITE " : "",
		(flags & 0x20) ? "AUTO_EXPOSURE " : "",
		(flags & 0x40) ? "DYNAMIC_RES " : "",
		(flags & 0x80) ? "TEXTURE1D " : "");
	return out;
}

// NVSDK_NGX_DLSS_Feature_Flags the FSR2 context flags imply. Observer only --
// SuperSampling create reads sdk_dlss_sr_flags, not this.
static uint32_t DlssSr_NgxFlagsFrom(uint32_t ffxFlags)
{
	uint32_t ngx = 0;
	if (ffxFlags & 0x01) ngx |= 0x01; // IsHDR
	if (!(ffxFlags & 0x02)) ngx |= 0x02; // MVLowRes -- FSR2 only sets 0x02 for display-res MVs
	if (ffxFlags & 0x04) ngx |= 0x04; // MVJittered
	if (ffxFlags & 0x08) ngx |= 0x08; // DepthInverted
	if (ffxFlags & 0x20) ngx |= 0x40; // FSR2 AUTO_EXPOSURE -> NGX AutoExposure
	return ngx;
}

static unsigned DlssSr_CreateFlags(void)
{
	const int v = sdk_dlss_sr_flags.GetInt();
	return v < 0 ? 0u : static_cast<unsigned>(v);
}

static int DlssSr_MvMode(void)
{
	const int v = sdk_dlss_mv_source.GetInt();
	if (v != 1)
		return v;
	static bool s_logged = false;
	if (!s_logged)
	{
		s_logged = true;
		DlssSr_Log("sdk_dlss_mv_source 1 refused -- camera reconstruct binds a heap on the engine list\n");
	}
	return 0;
}

static const char* DlssSr_NgxFlagText(unsigned flags, char* out, size_t cap)
{
	_snprintf_s(out, cap, _TRUNCATE, "%s%s%s%s%s%s%s",
		(flags & 0x01) ? "IsHDR " : "",
		(flags & 0x02) ? "MVLowRes " : "",
		(flags & 0x04) ? "MVJittered " : "",
		(flags & 0x08) ? "DepthInverted " : "",
		(flags & 0x10) ? "DisableSubrect " : "",
		(flags & 0x20) ? "DoSharpening " : "",
		(flags & 0x40) ? "AutoExposure " : "");
	return out;
}

static bool DlssSr_PtrOk(const void* p, size_t bytes)
{
	if (!p)
		return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(p, &mbi, sizeof(mbi)) < sizeof(mbi))
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
	const uintptr_t end = start + mbi.RegionSize;
	const uintptr_t at = reinterpret_cast<uintptr_t>(p);
	return at >= start && (at + bytes) <= end;
}

static bool DlssSr_AddrInModule(const void* p, uintptr_t base)
{
	if (!base || !p)
		return false;
	const uint8_t* m = reinterpret_cast<const uint8_t*>(base);
	const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
	if (!DlssSr_PtrOk(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(m + dos->e_lfanew);
	if (!DlssSr_PtrOk(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	const uintptr_t at = reinterpret_cast<uintptr_t>(p);
	return at >= base && at < base + nt->OptionalHeader.SizeOfImage;
}

// A live D3D12 object's vtable belongs to the D3D12 runtime or the display
// driver. One resolving inside the game exe or this module is an unrelated C++
// object; calling through it faults, and the process-wide vectored handler
// turns that into a crash dump no matter what __try says.
static bool DlssSr_VtableIsForeign(const void* vt)
{
	static uintptr_t s_selfBase = 0;
	static bool s_selfProbed = false;
	if (!s_selfProbed)
	{
		s_selfProbed = true;
		HMODULE self = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&DlssSr_AddrInModule), &self))
			s_selfBase = reinterpret_cast<uintptr_t>(self);
	}
	if (DlssSr_AddrInModule(vt, g_GameDll.GetModuleBase()))
		return false;
	if (DlssSr_AddrInModule(vt, s_selfBase))
		return false;
	return true;
}

static bool DlssSr_IsExecutable(const void* p)
{
	if (!p)
		return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(p, &mbi, sizeof(mbi)) < sizeof(mbi))
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ
		| PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	return (mbi.Protect & exec) != 0;
}

// Candidates come from scanning raw engine memory, so "points at something
// readable" is not enough: a data pointer whose first qword lands in .rdata
// gets called and executes data. Require IUnknown's three slots to be real
// code owned by neither the game exe nor this module.
static bool DlssSr_VtableIsCom(void** vt)
{
	if (!DlssSr_PtrOk(vt, 3 * sizeof(void*)))
		return false;
	for (int i = 0; i < 3; ++i)
	{
		if (!DlssSr_IsExecutable(vt[i]))
			return false;
	}
	return DlssSr_VtableIsForeign(vt[0]);
}

static ID3D12Resource* DlssSr_QiResource(void* cand)
{
	if (!DlssSr_PtrOk(cand, sizeof(void*)))
		return nullptr;
	void** vt = *reinterpret_cast<void***>(cand);
	if (!DlssSr_VtableIsCom(vt))
		return nullptr;
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
	return nullptr;
}

// Takes m_d3dResource, following m_parentResource while the field is empty.
// The hop cap keeps a corrupt chain finite.
static ID3D12Resource* DlssSr_ResourceFromRhi(void* rhi)
{
	for (int hop = 0; rhi && hop < kRhiParentHops; ++hop)
	{
		if (!DlssSr_PtrOk(rhi, kRhiDumpBytes))
			return nullptr;
		uint8_t* const r = static_cast<uint8_t*>(rhi);
		void* res = *reinterpret_cast<void**>(r + kRhiTexD3DResource);
		if (res)
		{
			ID3D12Resource* out = DlssSr_QiResource(res);
			if (out)
				return out;
		}
		rhi = *reinterpret_cast<void**>(r + kRhiTexParent);
	}
	return nullptr;
}

// Reports the asset's RHI slots and the head of each parent chain once, so a
// layout change is diagnosed from a log instead of guessed at.
static void DlssSr_ReportAsset(void* asset)
{
	static bool s_reported = false;
	if (s_reported || sdk_dlss_sr_probe.GetInt() < 1)
		return;
	s_reported = true;

	uint8_t* const p = static_cast<uint8_t*>(asset);
	DlssSr_Log("asset %p %ux%u\n", asset,
		*reinterpret_cast<uint16_t*>(p + kTexAssetWidth),
		*reinterpret_cast<uint16_t*>(p + kTexAssetHeight));
	for (size_t slot = 0x70; slot <= 0x98; slot += sizeof(void*))
	{
		void* rhi = *reinterpret_cast<void**>(p + slot);
		if (!rhi || !DlssSr_PtrOk(rhi, kRhiDumpBytes))
		{
			DlssSr_Log("  +0x%02zX = %p\n", slot, rhi);
			continue;
		}
		uint8_t* const r = static_cast<uint8_t*>(rhi);
		DlssSr_Log("  +0x%02zX = %p spec=%ux%u\n", slot, rhi,
			*reinterpret_cast<uint32_t*>(r + kRhiTexSpec),
			*reinterpret_cast<uint16_t*>(r + kRhiTexSpec + 4));
		for (size_t f = 0x30; f + sizeof(void*) <= kRhiDumpBytes; f += sizeof(void*))
		{
			void* v = *reinterpret_cast<void**>(r + f);
			if (!v)
				continue;
			ID3D12Resource* q = DlssSr_QiResource(v);
			DlssSr_Log("      rhi+0x%02zX = %p%s\n", f, v, q ? "  <== ID3D12Resource" : "");
			if (q)
				q->Release();
		}
	}
}

static ID3D12Resource* DlssSr_FromTextureAsset(void* asset)
{
	// The engine reads a byte at +152, so the struct outlives that offset.
	if (!DlssSr_PtrOk(asset, 160))
		return nullptr;

	// The engine's own binds take the RHI object from +0x80.
	static const size_t kSlots[] = {
		kTexAssetRhiTexture,
		kTexAssetRhiRt,
		kTexAssetRhiDepth,
	};

	uint8_t* const p = static_cast<uint8_t*>(asset);
	for (int i = 0; i < 3; ++i)
	{
		ID3D12Resource* res = DlssSr_ResourceFromRhi(*reinterpret_cast<void**>(p + kSlots[i]));
		if (!res)
			continue;
		static bool s_loggedSlot = false;
		if (!s_loggedSlot)
		{
			s_loggedSlot = true;
			DlssSr_Log("texture asset slot +0x%zX -> resource\n", kSlots[i]);
		}
		return res;
	}
	DlssSr_ReportAsset(asset);
	return nullptr;
}

static ID3D12Resource* DlssSr_FromITexture(void* tex)
{
	if (!DlssSr_PtrOk(tex, sizeof(void*)))
		return nullptr;
	void** vt = *reinterpret_cast<void***>(tex);
	void* asset = nullptr;
	if (DlssSr_PtrOk(vt, 29 * sizeof(void*)) && DlssSr_PtrOk(vt[28], 1))
	{
		__try
		{
			using Fn = void* (__fastcall*)(void*, unsigned);
			asset = reinterpret_cast<Fn>(vt[28])(tex, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			asset = nullptr;
		}
	}
	return asset ? DlssSr_FromTextureAsset(asset) : nullptr;
}

// Resolving an engine render target walks the RHI parent chain and probes every
// candidate with VirtualQuery plus QueryInterface -- hundreds of syscalls. Done
// per present that is a measurable frame cost, and within one resolution the
// only thing that can change the answer is the ITexture slot itself.
struct DlssSrRtCacheEnt
{
	uintptr_t rva;
	void* slot;
	ID3D12Resource* res;
	bool have;
};
static constexpr int kRtCacheSlots = 6;
static DlssSrRtCacheEnt s_rtCache[kRtCacheSlots] = {};

static void DlssSr_ResetRtCache(void)
{
	for (int i = 0; i < kRtCacheSlots; ++i)
	{
		if (s_rtCache[i].res)
			s_rtCache[i].res->Release();
		s_rtCache[i] = DlssSrRtCacheEnt();
	}
}

static ID3D12Resource* DlssSr_RtCacheGet(uintptr_t rva, void* slot, bool& hit)
{
	hit = false;
	for (int i = 0; i < kRtCacheSlots; ++i)
	{
		if (!s_rtCache[i].have || s_rtCache[i].rva != rva)
			continue;
		if (s_rtCache[i].slot != slot)
			return nullptr;
		hit = true;
		if (s_rtCache[i].res)
			s_rtCache[i].res->AddRef();
		return s_rtCache[i].res;
	}
	return nullptr;
}

static void DlssSr_RtCachePut(uintptr_t rva, void* slot, ID3D12Resource* res)
{
	int at = -1;
	for (int i = 0; i < kRtCacheSlots; ++i)
	{
		if (s_rtCache[i].have && s_rtCache[i].rva == rva)
		{
			at = i;
			break;
		}
		if (at < 0 && !s_rtCache[i].have)
			at = i;
	}
	if (at < 0)
		at = 0;
	if (s_rtCache[at].res)
		s_rtCache[at].res->Release();
	s_rtCache[at].rva = rva;
	s_rtCache[at].slot = slot;
	s_rtCache[at].res = res;
	s_rtCache[at].have = true;
	if (res)
		res->AddRef();
}

static ID3D12Resource* DlssSr_FromSlot(const NetObsSym_t sym)
{
	void** slot = reinterpret_cast<void**>(NetObs_Sym(sym));
	if (!slot)
		return nullptr;
	const uintptr_t rva = reinterpret_cast<uintptr_t>(slot);
	if (!DlssSr_PtrOk(slot, sizeof(void*)))
		return nullptr;
	bool hit = false;
	ID3D12Resource* cached = DlssSr_RtCacheGet(rva, *slot, hit);
	if (hit)
		return cached;
	ID3D12Resource* res = DlssSr_FromITexture(*slot);
	DlssSr_RtCachePut(rva, *slot, res);
	if (!res && sdk_dlss_sr_probe.GetInt() >= 1)
	{
		static uintptr_t s_logged = 0;
		if (s_logged != rva)
		{
			s_logged = rva;
			DlssSr_Log("render target +0x%zX unresolved (itexture=%p)\n", rva, *slot);
		}
	}
	return res;
}


static const char* const kResourceStatePattern =
	"48 8B F1 0F B6 FA 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? "
	"33 DB 4C 8B C2 66 90 4D 8B 08 49 8B C1 48 F7 D0 48 0F BC C8";

static uintptr_t DlssSr_ResourceStateTable(void)
{
	static uintptr_t s_table = 0;
	static bool s_resolved = false;
	if (s_resolved)
		return s_table;
	s_resolved = true;

	CMemory hit = Module_FindPattern(g_GameDll, kResourceStatePattern);
	if (hit.GetPtr())
	{
		const uintptr_t resolved = hit.Offset(0x64).ResolveRelativeAddress(3, 7).GetPtr();
		if (resolved)
		{
			s_table = resolved;
			DlssSr_Log("s_ResourceState via pattern at %p\n", reinterpret_cast<void*>(s_table));
			return s_table;
		}
	}

	s_table = NetObs_Sym(NetObsSym_t::ResourceState);
	if (s_table)
		DlssSr_Log("s_ResourceState via symbol table at %p\n", reinterpret_cast<void*>(s_table));
	return s_table;
}

struct DlssSrStateCacheEnt
{
	ID3D12Resource* res;
	D3D12_RESOURCE_STATES state;
};
static constexpr int kStateCacheSlots = 32;
static DlssSrStateCacheEnt s_stateCache[kStateCacheSlots] = {};
static int s_stateCacheNext = 0;

// Engine resources die and are reallocated on a resolution change, so a cached
// pointer can be reused by a resource with a different registered state.
static void DlssSr_ResetStateCache(void)
{
	for (int i = 0; i < kStateCacheSlots; ++i)
		s_stateCache[i].res = nullptr;
	s_stateCacheNext = 0;
	DlssSr_ResetRtCache();
}

static D3D12_RESOURCE_STATES DlssSr_EngineState(ID3D12Resource* res,
	D3D12_RESOURCE_STATES fallback)
{
	if (!res)
		return fallback;
	const uintptr_t moduleBase = g_GameDll.GetModuleBase();
	if (!moduleBase)
		return fallback;

	for (int i = 0; i < kStateCacheSlots; ++i)
	{
		if (s_stateCache[i].res == res)
			return s_stateCache[i].state;
	}

	D3D12_RESOURCE_STATES st = fallback;
	const uintptr_t table = DlssSr_ResourceStateTable();
	// One range check for the whole table -- a per-entry VirtualQuery would be
	// thousands of syscalls on the resolve thread for every cache miss.
	if (table && DlssSr_PtrOk(reinterpret_cast<void*>(table), kResourceStateCount * kResourceStateStride))
	{
		for (size_t i = 0; i < kResourceStateCount; ++i)
		{
			const uint8_t* e = reinterpret_cast<const uint8_t*>(table + i * kResourceStateStride);
			if (*reinterpret_cast<ID3D12Resource* const*>(e) != res)
				continue;
			const uint8_t idx = e[0x08];
			const uint32_t* map = reinterpret_cast<const uint32_t*>(NetObs_Sym(NetObsSym_t::RhiToDx12States) + 4u * idx);
			if (idx < kRhiStateCount && DlssSr_PtrOk(map, sizeof(uint32_t)))
				st = static_cast<D3D12_RESOURCE_STATES>(*map);
			break;
		}
	}

	// A miss is cached too; the alternative is rescanning the whole table every
	// frame for any resource the RHI never registered.
	s_stateCache[s_stateCacheNext].state = st;
	s_stateCache[s_stateCacheNext].res = res;
	s_stateCacheNext = (s_stateCacheNext + 1) % kStateCacheSlots;
	return st;
}

// NGX inputs need NON_PIXEL_SHADER_RESOURCE; the engine's combined read states
// already contain it, so those frames need no barrier at all.
static bool DlssSr_NeedsReadBarrier(D3D12_RESOURCE_STATES have)
{
	return (have & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;
}

static void DlssSr_SwapChainSize(unsigned& w, unsigned& h)
{
	static unsigned s_w = 0;
	static unsigned s_h = 0;
	if (!s_w)
	{
		IDXGISwapChain* sc = Dx12_GetGameSwapChain();
		DXGI_SWAP_CHAIN_DESC desc = {};
		if (sc && SUCCEEDED(sc->GetDesc(&desc)))
		{
			s_w = desc.BufferDesc.Width;
			s_h = desc.BufferDesc.Height;
		}
	}
	w = s_w;
	h = s_h;
}

static int DlssSr_Quality(unsigned displayW, unsigned renderW)
{
	if (!renderW)
		return 5;
	const float ratio = static_cast<float>(displayW) / static_cast<float>(renderW);
	if (ratio >= 2.9f)
		return 3; // UltraPerformance
	if (ratio >= 1.9f)
		return 0; // MaxPerf
	if (ratio >= 1.6f)
		return 1; // Balanced
	if (ratio >= 1.4f)
		return 2; // MaxQuality
	return 5; // DLAA
}

static bool DlssSr_EnsureCmd(ID3D12Device* device)
{
	if (!s_srFence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_srFence))))
		return false;
	for (int i = 0; i < kSrSlots; ++i)
	{
		if (s_srCmds[i])
			continue;
		if (!s_srAllocs[i] && FAILED(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_srAllocs[i]))))
			return false;
		if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			s_srAllocs[i], nullptr, IID_PPV_ARGS(&s_srCmds[i]))))
			return false;
		s_srCmds[i]->Close();
	}
	return true;
}

static bool DlssSr_SlotDone(int slot)
{
	return s_srSlotFence[slot] == 0
		|| s_srFence->GetCompletedValue() >= s_srSlotFence[slot];
}

static bool DlssSr_AllSlotsDone(void)
{
	for (int i = 0; i < kSrSlots; ++i)
	{
		if (!DlssSr_SlotDone(i))
			return false;
	}
	return true;
}

static ID3D12Resource* DlssSr_EnsureDummy(ID3D12Device* device, ID3D12Resource*& slot,
	UINT w, UINT h, DXGI_FORMAT fmt)
{
	if (slot)
	{
		const D3D12_RESOURCE_DESC have = slot->GetDesc();
		if (have.Width == w && have.Height == h && have.Format == fmt)
			return slot;
		slot->Release();
		slot = nullptr;
	}
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&slot))))
		return nullptr;
	return slot;
}

static bool DlssSr_EnsureUav(ID3D12Device* device, unsigned w, unsigned h, DXGI_FORMAT fmt)
{
	if (s_srOut)
	{
		const D3D12_RESOURCE_DESC have = s_srOut->GetDesc();
		if (have.Width == w && have.Height == h && have.Format == fmt)
			return true;
		s_srOut->Release();
		s_srOut = nullptr;
		DlssSr_ResetStateCache();
	}
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&s_srOut))))
		return false;
	s_srOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	return true;
}

static bool DlssSr_EnsureColorCopy(ID3D12Device* device, unsigned w, unsigned h, DXGI_FORMAT fmt)
{
	if (s_nrColor)
	{
		const D3D12_RESOURCE_DESC have = s_nrColor->GetDesc();
		if (have.Width == w && have.Height == h && have.Format == fmt)
			return true;
		s_nrColor->Release();
		s_nrColor = nullptr;
	}
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_nrColor))))
		return false;
	s_nrColorState = D3D12_RESOURCE_STATE_COPY_DEST;
	return true;
}


static void DlssSr_CopyMip0(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src)
{
	D3D12_TEXTURE_COPY_LOCATION d = {};
	d.pResource = dst;
	d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	d.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION s = {};
	s.pResource = src;
	s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	s.SubresourceIndex = 0;
	list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
}

static void DlssSr_Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	if (!list || !res || before == after)
		return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	list->ResourceBarrier(1, &b);
}

static void DlssSr_UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res)
{
	if (!list || !res)
		return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	b.UAV.pResource = res;
	list->ResourceBarrier(1, &b);
}

static void* s_trustedList = nullptr;

static ID3D12GraphicsCommandList* DlssSr_QiList(void* cand)
{
	if (!DlssSr_PtrOk(cand, sizeof(void*)))
		return nullptr;
	if (cand != s_trustedList)
	{
		void** vt = *reinterpret_cast<void***>(cand);
		if (!DlssSr_VtableIsCom(vt))
			return nullptr;
	}
	ID3D12GraphicsCommandList* list = nullptr;
	__try
	{
		IUnknown* unk = reinterpret_cast<IUnknown*>(cand);
		if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&list))) || !list)
			return nullptr;
		if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
		{
			list->Release();
			return nullptr;
		}
		s_trustedList = cand;
		return list;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	return nullptr;
}

static ID3D12GraphicsCommandList* DlssSr_ListFromTls(void)
{
	uint8_t* teb = reinterpret_cast<uint8_t*>(NtCurrentTeb());
	if (!teb)
		return nullptr;
	void** slots = *reinterpret_cast<void***>(teb + 0x58);
	if (!slots || !DlssSr_PtrOk(slots, sizeof(void*)))
		return nullptr;
	uint8_t* tls = static_cast<uint8_t*>(slots[0]);
	if (!tls || !DlssSr_PtrOk(tls, kTlsRhiCtx + sizeof(void*)))
		return nullptr;
	void** ctxpp = *reinterpret_cast<void***>(tls + kTlsRhiCtx);
	if (!ctxpp || !DlssSr_PtrOk(ctxpp, sizeof(void*)) || !*ctxpp)
		return nullptr;
	uint8_t* ctx = static_cast<uint8_t*>(*ctxpp);
	if (!DlssSr_PtrOk(ctx, kRhiCtxBytes))
		return nullptr;
	static size_t s_listOff = kRhiCtxCmdList;
	static bool s_listOffProven = false;
	if (s_listOff)
	{
		ID3D12GraphicsCommandList* list = DlssSr_QiList(*reinterpret_cast<void**>(ctx + s_listOff));
		if (list)
		{
			if (!s_listOffProven)
			{
				s_listOffProven = true;
				DlssSr_Log("engine command list at RHI+0x%zX\n", s_listOff);
			}
			return list;
		}
		if (s_listOffProven)
			return nullptr;
		// The known field did not hold on this build; earn the offset once.
		DlssSr_Log("command list not at RHI+0x%zX -- scanning\n", s_listOff);
		s_listOff = 0;
	}
	for (size_t off = sizeof(void*); off + sizeof(void*) <= kRhiCtxBytes; off += sizeof(void*))
	{
		ID3D12GraphicsCommandList* list = DlssSr_QiList(*reinterpret_cast<void**>(ctx + off));
		if (!list)
			continue;
		s_listOff = off;
		s_listOffProven = true;
		DlssSr_Log("engine command list at RHI+0x%zX (scanned)\n", off);
		return list;
	}
	return nullptr;
}

static DXGI_FORMAT DlssSr_DepthSrvFormat(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS:
		return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS:
		return DXGI_FORMAT_R16_UNORM;
	default:
		return fmt;
	}
}

static bool DlssSr_EnsureMvTex(ID3D12Device* device, unsigned w, unsigned h)
{
	if (s_nrMv)
	{
		const D3D12_RESOURCE_DESC have = s_nrMv->GetDesc();
		if (have.Width == w && have.Height == h)
			return true;
		s_nrMv->Release();
		s_nrMv = nullptr;
	}
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = DXGI_FORMAT_R16G16_FLOAT;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&s_nrMv))))
		return false;
	s_nrMvState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	return true;
}

static const char kMvCs[] =
	"cbuffer CB : register(b0) {"
	"  float4 inv0; float4 inv1; float4 inv2; float4 inv3;"
	"  float4 prev0; float4 prev1; float4 prev2; float4 prev3;"
	"  float2 invSize; float2 pad; };"
	"Texture2D<float> DepthTex : register(t0);"
	"RWTexture2D<float2> MvTex : register(u0);"
	"float4 Mul4(float4 r0, float4 r1, float4 r2, float4 r3, float4 v) {"
	"  return float4(dot(r0,v), dot(r1,v), dot(r2,v), dot(r3,v)); }"
	"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {"
	"  uint w, h; MvTex.GetDimensions(w, h);"
	"  if (id.x >= w || id.y >= h) return;"
	"  float z = DepthTex.Load(int3(id.xy, 0));"
	"  float2 uv = (float2(id.xy) + 0.5) * invSize;"
	"  float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, z, 1);"
	"  float4 world = Mul4(inv0, inv1, inv2, inv3, clip);"
	"  world.xyz /= max(world.w, 1e-6); world.w = 1;"
	"  float4 prev = Mul4(prev0, prev1, prev2, prev3, world);"
	"  prev.xyz /= max(prev.w, 1e-6);"
	"  float2 prevUv = float2(prev.x * 0.5 + 0.5, 0.5 - prev.y * 0.5);"
	"  MvTex[id.xy] = (prevUv - uv) * float2(w, h); }";

static bool DlssSr_EnsureMvPass(ID3D12Device* device)
{
	if (s_mvPso && s_mvRoot && s_mvHeap && s_mvCb)
		return true;
	D3D12_DESCRIPTOR_RANGE ranges[2] = {};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].OffsetInDescriptorsFromTableStart = 1;
	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].DescriptorTable.NumDescriptorRanges = 2;
	params[1].DescriptorTable.pDescriptorRanges = ranges;
	D3D12_ROOT_SIGNATURE_DESC rs = {};
	rs.NumParameters = 2;
	rs.pParameters = params;
	ID3DBlob* blob = nullptr;
	ID3DBlob* err = nullptr;
	if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
	{
		if (err)
			err->Release();
		return false;
	}
	if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s_mvRoot))))
	{
		blob->Release();
		return false;
	}
	blob->Release();
	ID3DBlob* cs = nullptr;
	if (FAILED(D3DCompile(kMvCs, sizeof(kMvCs) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err)))
	{
		if (err)
		{
			DlssSr_Log("MV CS compile failed: %s\n", static_cast<char*>(err->GetBufferPointer()));
			err->Release();
		}
		return false;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
	pso.pRootSignature = s_mvRoot;
	pso.CS.pShaderBytecode = cs->GetBufferPointer();
	pso.CS.BytecodeLength = cs->GetBufferSize();
	const HRESULT pr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&s_mvPso));
	cs->Release();
	if (FAILED(pr))
		return false;
	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = 2;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_mvHeap))))
		return false;
	s_mvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = 256;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s_mvCb))))
		return false;
	return true;
}

static bool DlssSr_BuildViewProj(VMatrix& viewProj, VMatrix& invViewProj)
{
	if (!g_vecRenderOrigin || !g_vecRenderAngles)
		return false;
	float fov = 90.0f;
	float aspect = 16.0f / 9.0f;
	float zNear = 1.0f;
	float zFar = 16000.0f;
	if (g_pViewRender)
	{
		const float vfov = g_pViewRender->GetFieldOfView();
		if (vfov > 1.0f && vfov < 170.0f)
			fov = vfov;
		const float ar = g_pViewRender->GetAspectRatio();
		if (ar > 0.2f && ar < 5.0f)
			aspect = ar;
		const float zn = g_pViewRender->GetZNear();
		const float zf = g_pViewRender->GetZFar();
		if (zn > 0.0f && zf > zn)
		{
			zNear = zn;
			zFar = zf;
		}
	}
	matrix3x4_t camToWorld;
	AngleMatrix(*g_vecRenderAngles, *g_vecRenderOrigin, camToWorld);
	VMatrix view;
	view.Init(camToWorld.InverseTR());
	VMatrix proj;
	MatrixBuildPerspectiveX(proj, fov, aspect, zNear, zFar);
	MatrixMultiply(proj, view, viewProj);
	return viewProj.InverseGeneral(invViewProj);
}


static bool DlssSr_IsDepthFormat(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS:
		return true;
	default:
		return false;
	}
}

static ID3D12Resource* DlssSr_DepthFromSlot(const NetObsSym_t sym)
{
	void** slot = reinterpret_cast<void**>(NetObs_Sym(sym));
	if (!slot)
		return nullptr;
	const uintptr_t rva = reinterpret_cast<uintptr_t>(slot);
	if (!DlssSr_PtrOk(slot, sizeof(void*)))
		return nullptr;
	bool hit = false;
	ID3D12Resource* cached = DlssSr_RtCacheGet(rva, *slot, hit);
	if (hit)
		return cached;
	ID3D12Resource* res = DlssSr_FromTextureAsset(*slot);
	if (!res)
	{
		DlssSr_RtCachePut(rva, *slot, nullptr);
		return nullptr;
	}
	const DXGI_FORMAT fmt = res->GetDesc().Format;
	static uintptr_t s_logged = 0;
	if (s_logged != rva && sdk_dlss_sr_probe.GetInt() >= 1)
	{
		s_logged = rva;
		DlssSr_Log("depth +0x%zX fmt=%u depth=%d\n", rva, fmt, DlssSr_IsDepthFormat(fmt) ? 1 : 0);
	}
	if (!DlssSr_IsDepthFormat(fmt))
	{
		res->Release();
		res = nullptr;
	}
	DlssSr_RtCachePut(rva, *slot, res);
	return res;
}

static ID3D12Resource* DlssSr_SceneDepth(void)
{
	ID3D12Resource* res = DlssSr_DepthFromSlot(NetObsSym_t::SceneDepth);
	return res ? res : DlssSr_DepthFromSlot(NetObsSym_t::SwapChainDepth);
}

static ID3D12Resource* s_tmEnc = nullptr;
static ID3D12Resource* s_tmOut = nullptr;
static ID3D12RootSignature* s_tmRoot = nullptr;
static ID3D12PipelineState* s_tmEncPso = nullptr;
static ID3D12PipelineState* s_tmCombinePso = nullptr;
static ID3D12DescriptorHeap* s_tmHeap = nullptr;
static UINT s_tmDescSize = 0;
// Descriptors are written by the CPU at record time and read by the GPU frames
// later, so each in-flight frame needs its own slots.
static const unsigned kTmRingFrames = 8;
static const unsigned kTmSlotsPerFrame = 6;
static unsigned s_tmRing = 0;

static const char kTmEncCs[] =
	"cbuffer CB : register(b0) { float4 tune; };"
	"Texture2D<float4> SrcTex : register(t0);"
	"Texture2D<float4> NrTex  : register(t1);"
	"RWTexture2D<float4> OutTex : register(u0);"
	"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {"
	"  uint w, h; OutTex.GetDimensions(w, h);"
	"  if (id.x >= w || id.y >= h) return;"
	"  float3 c = max(SrcTex.Load(int3(id.xy, 0)).rgb, 0.0);"
	"  float3 t = c / (1.0 + c);"
	"  OutTex[id.xy] = float4(pow(t, 1.0 / 2.2), 1.0); }";

static const char kTmCombineCs[] =
	"cbuffer CB : register(b0) { float4 tune; };"
	"Texture2D<float4> SrcTex : register(t0);"
	"Texture2D<float4> NrTex  : register(t1);"
	"RWTexture2D<float4> OutTex : register(u0);"
	"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {"
	"  uint w, h; OutTex.GetDimensions(w, h);"
	"  if (id.x >= w || id.y >= h) return;"
	"  float3 c = max(SrcTex.Load(int3(id.xy, 0)).rgb, 0.0);"
	"  float3 t0 = c / (1.0 + c);"
	"  float3 t1 = pow(saturate(NrTex.Load(int3(id.xy, 0)).rgb), 2.2);"
	"  float3 ratio = clamp(t1 / max(t0, 1e-4), 1.0 / tune.x, tune.x);"
	"  OutTex[id.xy] = float4(c * ratio, 1.0); }";

static bool DlssSr_EnsureTonemapTargets(ID3D12Device* device, unsigned w, unsigned h)
{
	ID3D12Resource** targets[2] = { &s_tmEnc, &s_tmOut };
	for (int i = 0; i < 2; ++i)
	{
		ID3D12Resource*& tex = *targets[i];
		if (tex)
		{
			const D3D12_RESOURCE_DESC have = tex->GetDesc();
			if (have.Width == w && have.Height == h)
				continue;
			tex->Release();
			tex = nullptr;
		}
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = w;
		d.Height = h;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		d.SampleDesc.Count = 1;
		d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&tex))))
			return false;
	}
	return true;
}

static bool DlssSr_CompileCs(ID3D12Device* device, const char* src, size_t len,
	const char* label, ID3D12PipelineState** pso)
{
	ID3DBlob* cs = nullptr;
	ID3DBlob* err = nullptr;
	if (FAILED(D3DCompile(src, len, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err)))
	{
		if (err)
		{
			DlssSr_Log("%s CS compile failed: %s\n", label, static_cast<char*>(err->GetBufferPointer()));
			err->Release();
		}
		return false;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
	desc.pRootSignature = s_tmRoot;
	desc.CS.pShaderBytecode = cs->GetBufferPointer();
	desc.CS.BytecodeLength = cs->GetBufferSize();
	const HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pso));
	cs->Release();
	return SUCCEEDED(hr);
}

static bool DlssSr_EnsureTonemapPass(ID3D12Device* device)
{
	if (s_tmRoot && s_tmEncPso && s_tmCombinePso && s_tmHeap)
		return true;
	if (!s_tmRoot)
	{
		D3D12_DESCRIPTOR_RANGE ranges[2] = {};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 2;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 1;
		ranges[1].OffsetInDescriptorsFromTableStart = 2;
		D3D12_ROOT_PARAMETER params[2] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.Num32BitValues = 4;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].DescriptorTable.NumDescriptorRanges = 2;
		params[1].DescriptorTable.pDescriptorRanges = ranges;
		D3D12_ROOT_SIGNATURE_DESC rs = {};
		rs.NumParameters = 2;
		rs.pParameters = params;
		ID3DBlob* blob = nullptr;
		ID3DBlob* err = nullptr;
		if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
		{
			if (err)
				err->Release();
			return false;
		}
		const HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
			blob->GetBufferSize(), IID_PPV_ARGS(&s_tmRoot));
		blob->Release();
		if (FAILED(hr))
			return false;
	}
	if (!s_tmEncPso && !DlssSr_CompileCs(device, kTmEncCs, sizeof(kTmEncCs) - 1, "NR encode", &s_tmEncPso))
		return false;
	if (!s_tmCombinePso && !DlssSr_CompileCs(device, kTmCombineCs, sizeof(kTmCombineCs) - 1, "NR combine", &s_tmCombinePso))
		return false;
	if (!s_tmHeap)
	{
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = kTmRingFrames * kTmSlotsPerFrame;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_tmHeap))))
			return false;
		s_tmDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}
	return true;
}

// Descriptors 0..2 belong to the encode dispatch and 3..5 to the recombine, so
// both can be recorded into one command list without aliasing.
static void DlssSr_TonemapDispatch(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
	ID3D12PipelineState* pso, unsigned base, ID3D12Resource* srv0, ID3D12Resource* srv1,
	ID3D12Resource* uav, unsigned w, unsigned h)
{
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = s_tmHeap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += static_cast<SIZE_T>(base) * s_tmDescSize;
	D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
	sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sv.Texture2D.MipLevels = 1;
	sv.Format = srv0->GetDesc().Format;
	device->CreateShaderResourceView(srv0, &sv, cpu);
	cpu.ptr += s_tmDescSize;
	sv.Format = srv1->GetDesc().Format;
	device->CreateShaderResourceView(srv1, &sv, cpu);
	cpu.ptr += s_tmDescSize;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
	uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uv.Format = uav->GetDesc().Format;
	device->CreateUnorderedAccessView(uav, nullptr, &uv, cpu);

	ID3D12DescriptorHeap* heaps[] = { s_tmHeap };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetComputeRootSignature(s_tmRoot);
	cmd->SetPipelineState(pso);
	float tune[4] = { sdk_dlssnr_tonemap_clamp.GetFloat(), 0.0f, 0.0f, 0.0f };
	if (!(tune[0] >= 1.0f))
		tune[0] = 1.0f;
	cmd->SetComputeRoot32BitConstants(0, 4, tune, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = s_tmHeap->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += static_cast<UINT64>(base) * s_tmDescSize;
	cmd->SetComputeRootDescriptorTable(1, gpu);
	cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

static ID3D12Resource* s_rangeBuf = nullptr;
static unsigned long long s_rangeBytes = 0;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_rangeFoot = {};
static unsigned s_rangeRows = 0;
static unsigned long long s_rangePendingAt = 0;
static unsigned long long s_rangeEvalSeq = 0;
static char s_rangeTag[16] = "colour";
static DXGI_FORMAT s_rangeFmt = DXGI_FORMAT_UNKNOWN;

// R11G11B10_FLOAT: 5-bit exponent with a 6/6/5-bit mantissa, no sign bit.
static float DlssSr_DecodeR11G11B10(unsigned bits, int shift, int mantBits)
{
	const unsigned field = (bits >> shift) & ((1u << (5 + mantBits)) - 1u);
	const unsigned exp = field >> mantBits;
	const unsigned mant = field & ((1u << mantBits) - 1u);
	const float scale = 1.0f / static_cast<float>(1u << mantBits);
	if (exp == 0)
		return static_cast<float>(mant) * scale * 6.103515625e-5f;
	if (exp == 31)
		return mant ? 0.0f : 65504.0f;
	return ldexpf(1.0f + static_cast<float>(mant) * scale, static_cast<int>(exp) - 15);
}

static float DlssSr_RangeMaxChannel(unsigned px, DXGI_FORMAT fmt)
{
	if (fmt == DXGI_FORMAT_R11G11B10_FLOAT)
	{
		const float r = DlssSr_DecodeR11G11B10(px, 0, 6);
		const float g = DlssSr_DecodeR11G11B10(px, 11, 6);
		const float b = DlssSr_DecodeR11G11B10(px, 22, 5);
		return r > g ? (r > b ? r : b) : (g > b ? g : b);
	}
	const float r = static_cast<float>(px & 0xFFu) / 255.0f;
	const float g = static_cast<float>((px >> 8) & 0xFFu) / 255.0f;
	const float b = static_cast<float>((px >> 16) & 0xFFu) / 255.0f;
	return r > g ? (r > b ? r : b) : (g > b ? g : b);
}

static bool DlssSr_RangeFmtOk(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

static void DlssSr_RangeReadback(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
	ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, const char* tag)
{
	const int every = sdk_dlssnr_range_probe.GetInt();
	++s_rangeEvalSeq;
	if (every <= 0 || !src)
		return;

	if (s_rangePendingAt && s_rangeEvalSeq >= s_rangePendingAt + 8 && s_rangeBuf)
	{
		s_rangePendingAt = 0;
		void* mapped = nullptr;
		D3D12_RANGE all = { 0, static_cast<SIZE_T>(s_rangeBytes) };
		if (SUCCEEDED(s_rangeBuf->Map(0, &all, &mapped)) && mapped)
		{
			float lo = 3.4e38f, hi = 0.0f, sum = 0.0f;
			unsigned long long over1 = 0, count = 0;
			const unsigned char* base = static_cast<const unsigned char*>(mapped) + s_rangeFoot.Offset;
			for (unsigned y = 0; y < s_rangeRows; y += 4)
			{
				const unsigned* row = reinterpret_cast<const unsigned*>(base + y * s_rangeFoot.Footprint.RowPitch);
				for (unsigned x = 0; x < s_rangeFoot.Footprint.Width; x += 4)
				{
					const float m = DlssSr_RangeMaxChannel(row[x], s_rangeFmt);
					if (m < lo) lo = m;
					if (m > hi) hi = m;
					sum += m;
					if (m > 1.0f) ++over1;
					++count;
				}
			}
			D3D12_RANGE none = { 0, 0 };
			s_rangeBuf->Unmap(0, &none);
			if (count)
				DlssSr_Log("%s colour range min=%.4f max=%.3f mean=%.4f above1=%.2f%% (%llu px) fmt=%u\n",
					s_rangeTag, lo, hi, sum / static_cast<float>(count),
					100.0 * static_cast<double>(over1) / static_cast<double>(count),
					static_cast<unsigned long long>(count),
					static_cast<unsigned>(s_rangeFmt));
		}
		return;
	}
	if (s_rangePendingAt || (s_rangeEvalSeq % static_cast<unsigned long long>(every)) != 0)
		return;

	const D3D12_RESOURCE_DESC sd = src->GetDesc();
	if (!DlssSr_RangeFmtOk(sd.Format))
	{
		static DXGI_FORMAT s_loggedSkip = DXGI_FORMAT_UNKNOWN;
		if (s_loggedSkip != sd.Format)
		{
			s_loggedSkip = sd.Format;
			DlssSr_Log("%s colour range skip fmt=%u\n", tag ? tag : "colour",
				static_cast<unsigned>(sd.Format));
		}
		return;
	}
	unsigned long long bytes = 0;
	device->GetCopyableFootprints(&sd, 0, 1, 0, &s_rangeFoot, &s_rangeRows, nullptr, &bytes);
	if (!bytes)
		return;
	if (s_rangeBuf && s_rangeBytes < bytes)
	{
		s_rangeBuf->Release();
		s_rangeBuf = nullptr;
	}
	if (!s_rangeBuf)
	{
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = bytes;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_rangeBuf))))
			return;
		s_rangeBytes = bytes;
	}

	D3D12_TEXTURE_COPY_LOCATION d = {};
	d.pResource = s_rangeBuf;
	d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	d.PlacedFootprint = s_rangeFoot;
	D3D12_TEXTURE_COPY_LOCATION sl = {};
	sl.pResource = src;
	sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	sl.SubresourceIndex = 0;
	DlssSr_Barrier(cmd, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
	DlssSr_Barrier(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
	strncpy_s(s_rangeTag, tag ? tag : "colour", _TRUNCATE);
	s_rangeFmt = sd.Format;
	s_rangePendingAt = s_rangeEvalSeq;
}

static bool DlssSr_EvalNrOnEngineList(ID3D12Device* device, ID3D12Resource* color,
	ID3D12Resource* output, unsigned outW, unsigned outH, DXGI_FORMAT fmt)
{
	if (!t_engineCmd)
		t_engineCmd = DlssSr_ListFromTls();
	ID3D12GraphicsCommandList* cmd = t_engineCmd;
	if (!cmd)
	{
		DlssSr_FailOnce("no engine command list");
		return false;
	}

	const D3D12_RESOURCE_DESC cd = color->GetDesc();
	const unsigned inW = static_cast<unsigned>(cd.Width);
	const unsigned inH = cd.Height;
	if (inW != outW || inH != outH)
	{
		DlssSr_FailOnce("scene color size != resolve dest");
		return false;
	}
	if (inW >= 0xFF00 || inH >= 0xFF00)
	{
		DlssSr_FailOnce("scene color has placeholder dims");
		return false;
	}

	if (!DlssSr_EnsureUav(device, outW, outH, fmt) || !DlssSr_EnsureColorCopy(device, outW, outH, cd.Format))
	{
		DlssSr_FailOnce("NR working-buffer alloc failed");
		return false;
	}

	ID3D12Resource* depth = DlssSr_SceneDepth();
	const D3D12_RESOURCE_STATES depthState = DlssSr_EngineState(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	const D3D12_RESOURCE_STATES colorState = DlssSr_EngineState(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
	const D3D12_RESOURCE_STATES outputState = DlssSr_EngineState(output, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (depth && s_needFirstReset)
	{
		const D3D12_RESOURCE_DESC dd = depth->GetDesc();
		DlssSr_Log("scene depth %ux%u fmt=%u flags=0x%X\n",
			static_cast<unsigned>(dd.Width), dd.Height, dd.Format, dd.Flags);
	}

	DlssSr_Barrier(cmd, color, colorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	if (s_nrColorState != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		DlssSr_Barrier(cmd, s_nrColor, s_nrColorState, D3D12_RESOURCE_STATE_COPY_DEST);
		s_nrColorState = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	DlssSr_CopyMip0(cmd, s_nrColor, color);
	DlssSr_Barrier(cmd, s_nrColor, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	s_nrColorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	DlssSr_Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, colorState);
	if (s_srOutState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	{
		DlssSr_Barrier(cmd, s_srOut, s_srOutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		s_srOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}

	bool reset = s_needFirstReset;
	if (g_vecRenderOrigin)
	{
		if (s_havePrevOrigin)
		{
			const float dx = g_vecRenderOrigin->x - s_prevOrigin.x;
			const float dy = g_vecRenderOrigin->y - s_prevOrigin.y;
			const float dz = g_vecRenderOrigin->z - s_prevOrigin.z;
			if ((dx * dx + dy * dy + dz * dz) > (250.0f * 250.0f))
				reset = true;
		}
		else
			reset = true;
		s_prevOrigin = *g_vecRenderOrigin;
		s_havePrevOrigin = true;
	}

	int mvMode = DlssSr_MvMode();
	ID3D12Resource* mv = nullptr;
	float mvScaleX = 0.0f;
	float mvScaleY = 0.0f;
	const char* mvSrc = "none";
	bool mvOwned = false;
	bool mvNeedRestore = false;
	bool depthNeedRestore = false;
	const D3D12_RESOURCE_STATES shaderReadOnly = static_cast<D3D12_RESOURCE_STATES>(
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	D3D12_RESOURCE_STATES mvState = shaderReadOnly;

	if (mvMode == 2)
	{
		mvSrc = "none";
	}
	else
	{
		mv = DlssSr_FromSlot(NetObsSym_t::RtMrt1);
		if (mv)
		{
			// _rt_MRT1 is (curUV-prevUV)*1024; NGX wants prev-cur in output pixels.
			mvScaleX = -(static_cast<float>(outW) / 1024.0f);
			mvScaleY = -(static_cast<float>(outH) / 1024.0f);
			mvSrc = "engine";
			mvOwned = true;
			mvState = DlssSr_EngineState(mv, shaderReadOnly);
			if (DlssSr_NeedsReadBarrier(mvState))
			{
				DlssSr_Barrier(cmd, mv, mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				mvNeedRestore = true;
			}
		}
		else
		{
			static bool s_loggedMvFallback = false;
			if (!s_loggedMvFallback)
			{
				s_loggedMvFallback = true;
				DlssSr_Log("engine motion buffer missing -- evaluating without MVs\n");
			}
		}
	}

	if (depth && DlssSr_NeedsReadBarrier(depthState))
	{
		DlssSr_Barrier(cmd, depth, depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		depthNeedRestore = true;
	}

	DlssSr_RangeReadback(cmd, device, s_nrColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "NR");

	const bool tonemap = sdk_dlssnr_tonemap.GetInt() != 0
		&& DlssSr_EnsureTonemapTargets(device, outW, outH)
		&& DlssSr_EnsureTonemapPass(device);
	const unsigned tmBase = (s_tmRing % kTmRingFrames) * kTmSlotsPerFrame;
	if (tonemap)
	{
		++s_tmRing;
		DlssSr_TonemapDispatch(cmd, device, s_tmEncPso, tmBase, s_nrColor, s_nrColor,
			s_tmEnc, outW, outH);
		DlssSr_UavBarrier(cmd, s_tmEnc);
		DlssSr_Barrier(cmd, s_tmEnc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	const int ev = DlssNr_ReplaceTsaa(cmd, tonemap ? s_tmEnc : s_nrColor,
		tonemap ? s_tmOut : s_srOut, depth, mv,
		outW, outH, reset, mvScaleX, mvScaleY);
	if (depthNeedRestore)
		DlssSr_Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState);
	if (mvNeedRestore)
		DlssSr_Barrier(cmd, mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mvState);

	if (tonemap)
	{
		DlssSr_Barrier(cmd, s_tmEnc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (ev == kNgxSuccess)
		{
			DlssSr_UavBarrier(cmd, s_tmOut);
			DlssSr_Barrier(cmd, s_tmOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			DlssSr_TonemapDispatch(cmd, device, s_tmCombinePso, tmBase + 3, s_nrColor, s_tmOut,
				s_srOut, outW, outH);
			DlssSr_Barrier(cmd, s_tmOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
	}

	if (ev == kNgxSuccess)
	{
		DlssSr_UavBarrier(cmd, s_srOut);
		DlssSr_Barrier(cmd, s_srOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		s_srOutState = D3D12_RESOURCE_STATE_COPY_SOURCE;
		DlssSr_Barrier(cmd, output, outputState, D3D12_RESOURCE_STATE_COPY_DEST);
		DlssSr_CopyMip0(cmd, output, s_srOut);
		DlssSr_Barrier(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, outputState);
		DlssSr_Barrier(cmd, s_srOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		s_srOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		++s_srEvalOk;
	if (tonemap && s_srEvalOk == 1)
		DlssSr_Log("NR tonemap encode active, recombine clamp %.2f\n",
			sdk_dlssnr_tonemap_clamp.GetFloat());
		s_needFirstReset = false;
		s_srEvalPresent = s_presentSeq;
		s_srLastOkTsaa = s_tsaaCount;
		s_lastRenderSize.width = outW;
		s_lastRenderSize.height = outH;
		s_displaySize.width = outW;
		s_displaySize.height = outH;
		s_srKeyOutW = outW;
		s_srKeyOutH = outH;
		if (sdk_dlss_sr_probe.GetInt() >= 1 && (s_srEvalOk == 1 || s_needFirstReset))
			DlssSr_Log("NR engine-list %ux%u fmt=%u depth=%d mv=%d mvsrc=%s reset=%d evals=%llu\n",
				outW, outH, fmt, depth ? 1 : 0, mv ? 1 : 0, mvSrc, reset ? 1 : 0,
				(unsigned long long)s_srEvalOk);
	}
	else
	{
		++s_srEvalFail;
		char why[64];
		_snprintf_s(why, _TRUNCATE, "EvaluateFeature 18 -> 0x%08X", static_cast<unsigned>(ev));
		DlssSr_FailOnce(why);
	}
	if (mvOwned && mv)
		mv->Release();
	if (depth)
		depth->Release();
	return ev == kNgxSuccess;
}

static void DlssSr_LogResource(const char* label, const FfxSrResource& res)
{
	DlssSr_Log("  %-14s ptr=%p %ux%u mips=%u fmt=%u state=%u depth=%d\n",
		label, res.resource, res.description.width, res.description.height,
		res.description.mipCount, res.description.format, res.state, res.isDepth ? 1 : 0);
}

static void DlssSr_LogDispatch(const FfxSrDispatchDescription* desc)
{
	char flagText[192] = {};
	DlssSr_Log("dispatch #%llu render=%ux%u display=%ux%u ratio=%.3f\n",
		(unsigned long long)s_dispatchCount,
		desc->renderSize.width, desc->renderSize.height,
		desc->output.description.width, desc->output.description.height,
		desc->renderSize.width ? (float)desc->output.description.width / (float)desc->renderSize.width : 0.0f);
	DlssSr_Log("  ctx flags=%u (%s) -> ngx dlss flags=0x%02X\n",
		s_ctxFlags, DlssSr_FlagText(s_ctxFlags, flagText, sizeof(flagText)), DlssSr_NgxFlagsFrom(s_ctxFlags));
	DlssSr_Log("  cmdlist=%p jitter=%.4f,%.4f mvScale=%.6f,%.6f reset=%d\n",
		desc->commandList, desc->jitterOffset.x, desc->jitterOffset.y,
		desc->motionVectorScale.x, desc->motionVectorScale.y, desc->reset ? 1 : 0);
	DlssSr_Log("  near=%.4f far=%.1f fovV=%.4f dt=%.4f preExposure=%.3f sharpen=%d/%.3f\n",
		desc->cameraNear, desc->cameraFar, desc->cameraFovAngleVertical,
		desc->frameTimeDelta, desc->preExposure,
		desc->enableSharpening ? 1 : 0, desc->sharpness);
	DlssSr_LogResource("color", desc->color);
	DlssSr_LogResource("depth", desc->depth);
	DlssSr_LogResource("motionVectors", desc->motionVectors);
	DlssSr_LogResource("output", desc->output);
	if (desc->exposure.resource)
		DlssSr_LogResource("exposure", desc->exposure);
	if (desc->reactive.resource)
		DlssSr_LogResource("reactive", desc->reactive);
	if (desc->output.description.mipCount > 1)
		DlssSr_Log("  WARN output has %u mips -- NGX rejects a mip chain on the UAV\n",
			desc->output.description.mipCount);
}

static int32_t __cdecl DlssSr_ContextCreate_Hook(void* context, const FfxSrContextDescription* desc)
{
	if (desc)
	{
		char flagText[192] = {};
		s_ctxFlags = desc->flags;
		s_maxRenderSize = desc->maxRenderSize;
		s_displaySize = desc->displaySize;
		s_sawContext = true;
		DlssSr_Log("ContextCreate flags=%u (%s) maxRender=%ux%u display=%ux%u device=%p\n",
			desc->flags, DlssSr_FlagText(desc->flags, flagText, sizeof(flagText)),
			desc->maxRenderSize.width, desc->maxRenderSize.height,
			desc->displaySize.width, desc->displaySize.height, desc->device);
	}
	return s_fnCreate(context, desc);
}

static int32_t __cdecl DlssSr_ContextDispatch_Hook(void* context, const FfxSrDispatchDescription* desc)
{
	if (desc)
	{
		const bool changed = desc->renderSize.width != s_lastRenderSize.width
			|| desc->renderSize.height != s_lastRenderSize.height;
		s_lastRenderSize = desc->renderSize;
		++s_dispatchCount;
		s_sawDispatch = true;

		const int probe = sdk_dlss_sr_probe.GetInt();
		if (probe >= 2 || (probe >= 1 && (s_dispatchCount == 1 || changed)))
			DlssSr_LogDispatch(desc);
	}
	return s_fnDispatch(context, desc);
}

static int32_t __cdecl DlssSr_ContextDestroy_Hook(void* context)
{
	DlssSr_Log("ContextDestroy after %llu dispatches\n", (unsigned long long)s_dispatchCount);
	s_dispatchCount = 0;
	s_lastRenderSize = FfxSrDim2{};
	return s_fnDestroy(context);
}

static void DlssSr_FailOnce(const char* why)
{
	if (!why)
		why = "unknown";
	if (s_loggedFail && strncmp(s_lastFail, why, sizeof(s_lastFail) - 1) == 0)
		return;
	strncpy_s(s_lastFail, why, _TRUNCATE);
	s_loggedFail = true;
	DlssSr_Log("TSAA replace skipped: %s\n", why);
}

static bool DlssSr_TryReplaceTsaa(int mode, void* params)
{
	// SuperSampling (feature 1) replaces TSAA on a private list. Neural
	// Rendering records on the engine list with scene color, depth, and MRT1.
	const bool useSr = settings_dlss_sr.GetInt() != 0;
	const bool useNr = !useSr && DlssNr_NrReplaceEnabled();
	if (s_srLatched || (!useSr && !useNr))
		return false;
	if (s_presentSeq != 0 && s_srEvalPresent == s_presentSeq)
	{
		++s_srDupSkips;
		return false;
	}
	if (!params || !DlssSr_PtrOk(params, 88))
	{
		DlssSr_FailOnce("params not readable");
		return false;
	}

	void* outAsset = *reinterpret_cast<void**>(params);
	ID3D12Resource* output = DlssSr_FromTextureAsset(outAsset);
	if (!output)
	{
		DlssSr_FailOnce("resolve output is not a D3D12 resource");
		return false;
	}

	const D3D12_RESOURCE_DESC od = output->GetDesc();
	const unsigned outW = static_cast<unsigned>(od.Width);
	const unsigned outH = od.Height;
	if (!outW || !outH)
	{
		output->Release();
		return false;
	}

	ID3D12Device* device = nullptr;
	if (FAILED(output->GetDevice(IID_PPV_ARGS(&device))) || !device)
	{
		output->Release();
		return false;
	}

	if (!DlssNr_EnsureNgxSession(device))
	{
		DlssSr_FailOnce("NGX session not ready");
		device->Release();
		output->Release();
		return false;
	}

	static unsigned s_skipInW = 0, s_skipInH = 0, s_skipOutW = 0, s_skipOutH = 0;

	if (useNr)
	{
		// Neural Rendering owns the main view only. The swapchain names its size
		// without depending on any engine layout.
		unsigned scW = 0, scH = 0;
		DlssSr_SwapChainSize(scW, scH);
		if (scW && scH && (outW != scW || outH != scH))
		{
			++s_srSkippedViews;
			if (outW != s_skipOutW || outH != s_skipOutH)
			{
				s_skipOutW = outW;
				s_skipOutH = outH;
				DlssSr_Log("skip-view %ux%u (main view is %ux%u)\n", outW, outH, scW, scH);
			}
			device->Release();
			output->Release();
			return false;
		}

		ID3D12Resource* color = nullptr;
		if (sdk_dlssnr_color_src.GetInt() != 0)
		{
			color = DlssSr_FromSlot(NetObsSym_t::FullFrameFb);
			if (!color)
			{
				DlssSr_FailOnce("_rt_FullFrameFB walk failed");
				device->Release();
				output->Release();
				return false;
			}
		}
		else
		{
			color = output;
			color->AddRef();
		}

		const bool ok = DlssSr_EvalNrOnEngineList(device, color, output, outW, outH, od.Format);
		color->Release();
		device->Release();
		output->Release();
		return ok;
	}

	ID3D12Resource* color = DlssSr_FromSlot(NetObsSym_t::FullFrameFb);
	if (!color)
	{
		DlssSr_FailOnce("_rt_FullFrameFB walk failed");
		device->Release();
		output->Release();
		return false;
	}

	const D3D12_RESOURCE_DESC cd = color->GetDesc();
	const unsigned inW = static_cast<unsigned>(cd.Width);
	const unsigned inH = cd.Height;
	if (!inW || !inH || inW >= 0xFF00 || inH >= 0xFF00)
	{
		color->Release();
		device->Release();
		output->Release();
		if (inW >= 0xFF00 || inH >= 0xFF00)
			DlssSr_FailOnce("scene color has placeholder dims");
		return false;
	}

	// Only the main-view resolve consumes the full frame buffer into an
	// equal-or-larger target. Every other resolve (minimap, scopes, any
	// downscale) stays on stock TSAA and never touches the feature key.
	const int quality = DlssSr_Quality(outW, inW);
	if (outW < inW || outH < inH)
	{
		++s_srSkippedViews;
		if (inW != s_skipInW || inH != s_skipInH || outW != s_skipOutW || outH != s_skipOutH)
		{
			s_skipInW = inW;
			s_skipInH = inH;
			s_skipOutW = outW;
			s_skipOutH = outH;
			DlssNr_SsFileLog("skip-view", inW, inH, outW, outH, quality, DlssSr_CreateFlags());
		}
		color->Release();
		device->Release();
		output->Release();
		return false;
	}

	ID3D12CommandQueue* queue = DirectX_GetDx12GameQueue();
	if (!queue || !DlssSr_EnsureCmd(device))
	{
		DlssSr_FailOnce("no game queue or command list");
		device->Release();
		color->Release();
		output->Release();
		return false;
	}

	const int slot = s_srSlot;
	if (!DlssSr_SlotDone(slot))
	{
		// GPU still owns this slot's list; stock TSAA already ran this frame.
		++s_srBusySkips;
		device->Release();
		color->Release();
		output->Release();
		return false;
	}

	const unsigned flags = DlssSr_CreateFlags();
	if (DlssNr_SuperSamplingLive() && s_srKeyInW
		&& (inW != s_srKeyInW || inH != s_srKeyInH
			|| outW != s_srKeyOutW || outH != s_srKeyOutH
			|| flags != s_srKeyFlags))
	{
		if (!DlssSr_AllSlotsDone())
		{
			++s_srBusySkips;
			device->Release();
			color->Release();
			output->Release();
			return false;
		}
		DlssNr_ReleaseSuperSampling();
		s_srKeyInW = s_srKeyInH = s_srKeyOutW = s_srKeyOutH = 0;
		s_srKeyFlags = 0;
	}

	ID3D12GraphicsCommandList* const cmd = s_srCmds[slot];
	if (FAILED(s_srAllocs[slot]->Reset()) || FAILED(cmd->Reset(s_srAllocs[slot], nullptr)))
	{
		DlssSr_FailOnce("command list reset failed");
		device->Release();
		color->Release();
		output->Release();
		return false;
	}

	if (useSr)
	{
		const int created = DlssNr_CreateSuperSampling(cmd, inW, inH, outW, outH, flags, quality);
		if (created == kNgxAlreadyExists)
		{
			++s_srSkippedViews;
			cmd->Close();
			device->Release();
			color->Release();
			output->Release();
			return false;
		}
		if (created != kNgxSuccess)
		{
			char why[64];
			_snprintf_s(why, _TRUNCATE, "CreateFeature 1 -> 0x%08X", static_cast<unsigned>(created));
			DlssSr_FailOnce(why);
			cmd->Close();
			device->Release();
			color->Release();
			output->Release();
			if (created == static_cast<int>(0xBAD00001) || created == static_cast<int>(0xBAD00011))
				s_srLatched = true;
			return false;
		}
	}
	s_srKeyInW = inW;
	s_srKeyInH = inH;
	s_srKeyOutW = outW;
	s_srKeyOutH = outH;
	s_srKeyFlags = flags;

	if (!DlssSr_EnsureUav(device, outW, outH, od.Format))
	{
		DlssSr_FailOnce("UAV output alloc failed");
		cmd->Close();
		device->Release();
		color->Release();
		output->Release();
		return false;
	}

	// SuperSampling is a temporal upscaler. Fed a blank depth and a blank
	// motion field it has nothing to reproject and every frame resolves to the
	// same history, so the real engine buffers are mandatory, not an upgrade.
	ID3D12Resource* depth = DlssSr_SceneDepth();
	ID3D12Resource* mv = (DlssSr_MvMode() == 2) ? nullptr : DlssSr_FromSlot(NetObsSym_t::RtMrt1);
	const bool depthOwned = depth != nullptr;
	const bool mvOwned = mv != nullptr;
	if (!depth)
		depth = DlssSr_EnsureDummy(device, s_dummyDepth, inW, inH, DXGI_FORMAT_R32_FLOAT);
	if (!mv)
		mv = DlssSr_EnsureDummy(device, s_dummyMv, inW, inH, DXGI_FORMAT_R16G16_FLOAT);
	if (!depth || !mv)
	{
		DlssSr_FailOnce("depth/MV unavailable");
		if (depthOwned) depth->Release();
		if (mvOwned) mv->Release();
		cmd->Close();
		device->Release();
		color->Release();
		output->Release();
		return false;
	}

	float uvx = 0.0f;
	float uvy = 0.0f;
	DlssSr_ReadJitterUv(params, uvx, uvy);
	const float jx = uvx * static_cast<float>(inW);
	const float jy = uvy * static_cast<float>(inH);
	// _rt_MRT1 stores (curUV - prevUV) * 1024; NGX wants prev-cur in pixels.
	const float mvScaleX = mvOwned ? -(static_cast<float>(inW) / 1024.0f) : 0.0f;
	const float mvScaleY = mvOwned ? -(static_cast<float>(inH) / 1024.0f) : 0.0f;

	if (s_needFirstReset)
	{
		char ngxText[192] = {};
		const D3D12_RESOURCE_DESC dd = depth->GetDesc();
		DlssSr_Log("first resolve mode=%d in=%ux%u out=%ux%u mips=%u colorFmt=%u outFmt=%u depthFmt=%u depthFlags=0x%X depth=%d mv=%d jitterUv=%.6f,%.6f jitterPx=%.4f,%.4f mvScale=%.3f,%.3f ngxFlags=0x%X (%s)\n",
			mode, inW, inH, outW, outH, od.MipLevels, cd.Format, od.Format, dd.Format, dd.Flags,
			depthOwned ? 1 : 0, mvOwned ? 1 : 0, uvx, uvy, jx, jy, mvScaleX, mvScaleY, flags,
			DlssSr_NgxFlagText(flags, ngxText, sizeof(ngxText)));
	}

	const D3D12_RESOURCE_STATES colorState = DlssSr_EngineState(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
	const D3D12_RESOURCE_STATES outputState = DlssSr_EngineState(output, D3D12_RESOURCE_STATE_RENDER_TARGET);
	const D3D12_RESOURCE_STATES depthState = DlssSr_EngineState(depthOwned ? depth : nullptr,
		D3D12_RESOURCE_STATE_DEPTH_WRITE);
	const D3D12_RESOURCE_STATES mvState = DlssSr_EngineState(mvOwned ? mv : nullptr,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	const bool depthBarrier = depthOwned && DlssSr_NeedsReadBarrier(depthState);
	const bool mvBarrier = mvOwned && DlssSr_NeedsReadBarrier(mvState);
	if (DlssSr_NeedsReadBarrier(colorState))
		DlssSr_Barrier(cmd, color, colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if (depthBarrier)
		DlssSr_Barrier(cmd, depth, depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if (mvBarrier)
		DlssSr_Barrier(cmd, mv, mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if (s_srOutState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
	{
		DlssSr_Barrier(cmd, s_srOut, s_srOutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		s_srOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}

	const D3D12_RESOURCE_STATES colorSrv = DlssSr_NeedsReadBarrier(colorState)
		? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : colorState;
	DlssSr_RangeReadback(cmd, device, color, colorSrv, "SR");

	const int ev = DlssNr_EvaluateSuperSampling(cmd, color, depth, mv, s_srOut,
		jx, jy, mvScaleX, mvScaleY, inW, inH, s_needFirstReset);

	if (DlssSr_NeedsReadBarrier(colorState))
		DlssSr_Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, colorState);
	if (depthBarrier)
		DlssSr_Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState);
	if (mvBarrier)
		DlssSr_Barrier(cmd, mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mvState);
	if (ev == kNgxSuccess)
	{
		DlssSr_UavBarrier(cmd, s_srOut);
		DlssSr_Barrier(cmd, s_srOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		s_srOutState = D3D12_RESOURCE_STATE_COPY_SOURCE;
		DlssSr_Barrier(cmd, output, outputState, D3D12_RESOURCE_STATE_COPY_DEST);
		DlssSr_CopyMip0(cmd, output, s_srOut);
		DlssSr_Barrier(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, outputState);
		DlssSr_Barrier(cmd, s_srOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		s_srOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	cmd->Close();

	if (depthOwned)
		depth->Release();
	if (mvOwned)
		mv->Release();

	if (ev != kNgxSuccess)
	{
		++s_srEvalFail;
		char why[64];
		_snprintf_s(why, _TRUNCATE, "EvaluateFeature 1 -> 0x%08X",
			static_cast<unsigned>(ev));
		DlssSr_FailOnce(why);
		device->Release();
		color->Release();
		output->Release();
		return false;
	}

	ID3D12CommandList* lists[] = { cmd };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(s_srFence, ++s_srFenceValue);
	s_srSlotFence[slot] = s_srFenceValue;
	s_srSlot = (slot + 1) % kSrSlots;
	++s_srEvalOk;
	s_needFirstReset = false;
	s_srEvalPresent = s_presentSeq;
	s_srLastOkTsaa = s_tsaaCount;
	s_lastRenderSize.width = inW;
	s_lastRenderSize.height = inH;
	s_displaySize.width = outW;
	s_displaySize.height = outH;

	const int probe = sdk_dlss_sr_probe.GetInt();
	if (probe >= 2 || (probe >= 1 && s_srEvalOk == 1))
		DlssSr_Log("TSAA replaced mode=%d in=%ux%u out=%ux%u q=%d flags=0x%X evals=%llu jitterUv=%.6f,%.6f jitterPx=%.4f,%.4f\n",
			mode, inW, inH, outW, outH, quality, flags, (unsigned long long)s_srEvalOk, uvx, uvy, jx, jy);

	device->Release();
	color->Release();
	output->Release();
	return true;
}

static bool DlssSr_ReadJitterUv(void* params, float& x, float& y)
{
	x = 0.0f;
	y = 0.0f;
	if (!params || !DlssSr_PtrOk(params, kTsaaParamsJitter + 2 * sizeof(float)))
		return false;
	x = *reinterpret_cast<float*>(static_cast<uint8_t*>(params) + kTsaaParamsJitter);
	y = *reinterpret_cast<float*>(static_cast<uint8_t*>(params) + kTsaaParamsJitter + sizeof(float));
	return true;
}

static bool DlssSr_IsQueuedCall(void)
{
	uint8_t* teb = reinterpret_cast<uint8_t*>(NtCurrentTeb());
	if (!teb)
		return false;
	void** slots = *reinterpret_cast<void***>(teb + 0x58); // TEB.ThreadLocalStoragePointer
	if (!slots || !DlssSr_PtrOk(slots, sizeof(void*)))
		return false;
	uint8_t* tls = static_cast<uint8_t*>(slots[0]);
	if (!tls || !DlssSr_PtrOk(tls, 64))
		return false;
	return *reinterpret_cast<int*>(tls + 60) != 0;
}

static int64_t __fastcall DlssSr_TsaaDraw_Hook(unsigned int a1, int64_t a2, int64_t a3,
	int a4, int a5, int a6, int64_t a7, unsigned int a8, unsigned int a9)
{
	// The list is only wanted for the resolve we are wrapping. Probing on every
	// unrelated post-pass submit costs the walk for nothing.
	if (!t_engineCmd && t_inTsaaResolve
		&& settings_dlss_sr.GetInt() == 0 && sdk_dlssnr_present_pass.GetInt() == 0
		&& DlssNr_NrReplaceEnabled())
		t_engineCmd = DlssSr_ListFromTls();
	if (t_skipTsaaDraw)
	{
		t_skipTsaaDraw = false;
		if (++s_srDrawsSkipped == 1)
			DlssSr_Log("TSAA pixel dispatch skipped -- replacement owns the output\n");
		return 0;
	}
	const int64_t drawn = s_fnTsaaDraw(a1, a2, a3, a4, a5, a6, a7, a8, a9);
	if (t_nrAfterDrawParams)
	{
		void* nrParams = t_nrAfterDrawParams;
		t_nrAfterDrawParams = nullptr;
		if (!t_engineCmd)
			t_engineCmd = DlssSr_ListFromTls();
		DlssSr_TryReplaceTsaa(t_nrAfterDrawMode, nrParams);
	}
	return drawn;
}

static int64_t __fastcall DlssSr_TsaaResolve_Hook(int mode, void* params)
{
	struct EngineCmdRelease
	{
		~EngineCmdRelease()
		{
			if (t_engineCmd)
			{
				t_engineCmd->Release();
				t_engineCmd = nullptr;
			}
		}
	} cmdRelease;

	++s_tsaaCount;
	if (s_tsaaCount == 1)
		DlssSr_Log("Tsaa_Resolve first call mode=%d params=%p queued=%d\n",
			mode, params, DlssSr_IsQueuedCall() ? 1 : 0);
	if (DlssSr_IsQueuedCall())
		return s_fnTsaaResolve(mode, params);

	// SuperSampling and Neural Rendering both read _rt_FullFrameFB. They run
	// first; the stock pixel dispatch is skipped only if that submit succeeded.
	const bool useSr = settings_dlss_sr.GetInt() != 0;
	const bool useNr = !useSr && DlssNr_NrReplaceEnabled()
		&& sdk_dlssnr_present_pass.GetInt() == 0;
	const bool nrAfterDraw = useNr && s_tsaaDrawInstalled && sdk_dlssnr_color_src.GetInt() == 0;
	bool replaced = false;
	if (nrAfterDraw)
	{
		t_nrAfterDrawParams = params;
		t_nrAfterDrawMode = mode;
	}
	else if (useSr || useNr)
	{
		if (useNr && !t_engineCmd)
			t_engineCmd = DlssSr_ListFromTls();
		replaced = DlssSr_TryReplaceTsaa(mode, params);
		t_skipTsaaDraw = replaced && s_tsaaDrawInstalled
			&& (useSr ? settings_dlss_sr_skip_tsaa.GetInt() != 0 : true);
	}

	t_inTsaaResolve = true;
	const int64_t r = s_fnTsaaResolve(mode, params);
	t_inTsaaResolve = false;
	t_skipTsaaDraw = false;
	t_nrAfterDrawParams = nullptr;
	return r;
}

static void DlssSr_TryInstallTsaa(void)
{
	if (s_tsaaInstalled || s_tsaaFailed)
		return;

	Module_FindPattern(g_GameDll,
		"40 55 56 41 56 41 57 48 8D AC 24 F8 FE FF FF 48 81 EC 08 02 00 00")
		.GetPtr(s_fnTsaaResolve);
	if (!s_fnTsaaResolve)
	{
		s_tsaaFailed = true;
		DlssSr_Log("TSAA resolve pattern unresolved -- SuperSampling stays idle\n");
		return;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(LPVOID&)s_fnTsaaResolve, (PBYTE)DlssSr_TsaaResolve_Hook);
	const LONG tsaaErr = DetourTransactionCommit();
	if (tsaaErr != NO_ERROR)
	{
		s_tsaaFailed = true;
		DlssSr_Log("TSAA resolve detour failed err=%ld\n", tsaaErr);
		return;
	}

	s_tsaaInstalled = true;
	DlssSr_Log("TSAA resolve hooked at %p\n", s_fnTsaaResolve);

	// Generic post-pass draw submit; unique prologue.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? "
		"41 54 41 56 41 57 48 83 EC ?? 65 48 8B 04 25 ?? ?? ?? ?? 48 8B F2")
		.GetPtr(s_fnTsaaDraw);
	if (!s_fnTsaaDraw)
	{
		DlssSr_Log("TSAA draw-submit pattern unresolved -- SuperSampling will stack on stock TSAA\n");
		return;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(LPVOID&)s_fnTsaaDraw, (PBYTE)DlssSr_TsaaDraw_Hook);
	const LONG drawErr = DetourTransactionCommit();
	if (drawErr != NO_ERROR)
	{
		s_fnTsaaDraw = nullptr;
		DlssSr_Log("TSAA draw-submit detour failed err=%ld -- SuperSampling will stack on stock TSAA\n", drawErr);
		return;
	}

	s_tsaaDrawInstalled = true;
	DlssSr_Log("TSAA draw submit hooked at %p\n", s_fnTsaaDraw);
}

void DlssSr_TryInstall(void)
{
	// Everything below detours the retail render path -- the TSAA resolve, the
	// post-pass draw submit and the exported FSR2 entry points. None of it can
	// produce a frame without an NVIDIA adapter AND an NGX runtime beside the
	// exe, and riding the FSR2 entry points would take the stock upscaler with
	// it, so without both the render path is left completely alone.
	if (!DlssNr_DlssStackAllowed())
	{
		if (!s_installFailed)
		{
			s_installFailed = true;
			s_tsaaFailed = true;
			DlssSr_Log("DLSS cannot run here (nvidia=%d nvngx_dlss=%d nvngx_dlssnr=%d) -- "
				"TSAA and FSR2 hooks not installed, stock upscaler untouched\n",
				DlssNr_IsNvidia() ? 1 : 0, DlssNr_SrRuntimePresent() ? 1 : 0,
				DlssNr_DllPresent() ? 1 : 0);
		}
		return;
	}

	++s_presentSeq;
	DlssSr_TryInstallTsaa();

	if (s_tsaaInstalled && !s_sawDispatch && s_tsaaCount == 0 && !s_reportedIdle && ++s_presentsSinceInstall >= 300)
	{
		s_reportedIdle = true;
		DlssSr_DumpStatus();
	}

	if (s_installed)
	{
		return;
	}
	if (s_installFailed)
		return;

	const HMODULE exe = GetModuleHandleW(NULL);
	s_fnCreate = reinterpret_cast<PfnFfxFsr2ContextCreate>(GetProcAddress(exe, "ffxFsr2ContextCreate"));
	s_fnDispatch = reinterpret_cast<PfnFfxFsr2ContextDispatch>(GetProcAddress(exe, "ffxFsr2ContextDispatch"));
	s_fnDestroy = reinterpret_cast<PfnFfxFsr2ContextDestroy>(GetProcAddress(exe, "ffxFsr2ContextDestroy"));

	if (!s_fnCreate || !s_fnDispatch || !s_fnDestroy)
	{
		s_installFailed = true;
		DlssSr_Log("no exported FSR2 API on this exe (create=%p dispatch=%p destroy=%p) -- DX11 build?\n",
			s_fnCreate, s_fnDispatch, s_fnDestroy);
		return;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(LPVOID&)s_fnCreate, (PBYTE)DlssSr_ContextCreate_Hook);
	DetourAttach(&(LPVOID&)s_fnDispatch, (PBYTE)DlssSr_ContextDispatch_Hook);
	DetourAttach(&(LPVOID&)s_fnDestroy, (PBYTE)DlssSr_ContextDestroy_Hook);
	const LONG err = DetourTransactionCommit();
	if (err != NO_ERROR)
	{
		s_installFailed = true;
		DlssSr_Log("detour commit failed err=%ld\n", err);
		return;
	}

	s_installed = true;
	DlssSr_Log("watching exported FSR2 upscale (dispatch %p)\n", s_fnDispatch);
}

void DlssSr_Shutdown(void)
{
	if (s_tsaaInstalled && s_fnTsaaResolve)
	{
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourDetach(&(LPVOID&)s_fnTsaaResolve, (PBYTE)DlssSr_TsaaResolve_Hook);
		DetourTransactionCommit();
		s_tsaaInstalled = false;
	}

	if (s_tsaaDrawInstalled && s_fnTsaaDraw)
	{
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourDetach(&(LPVOID&)s_fnTsaaDraw, (PBYTE)DlssSr_TsaaDraw_Hook);
		DetourTransactionCommit();
		s_tsaaDrawInstalled = false;
	}

	if (s_dummyDepth)
	{
		s_dummyDepth->Release();
		s_dummyDepth = nullptr;
	}
	if (s_dummyMv)
	{
		s_dummyMv->Release();
		s_dummyMv = nullptr;
	}
	if (s_srOut)
	{
		s_srOut->Release();
		s_srOut = nullptr;
	}
	if (s_nrColor)
	{
		s_nrColor->Release();
		s_nrColor = nullptr;
	}
	if (s_nrDepth)
	{
		s_nrDepth->Release();
		s_nrDepth = nullptr;
	}
	if (s_nrMv)
	{
		s_nrMv->Release();
		s_nrMv = nullptr;
	}
	DlssSr_ReleasePresentPass();
	if (s_tmEnc)
	{
		s_tmEnc->Release();
		s_tmEnc = nullptr;
	}
	if (s_tmOut)
	{
		s_tmOut->Release();
		s_tmOut = nullptr;
	}
	if (s_tmEncPso)
	{
		s_tmEncPso->Release();
		s_tmEncPso = nullptr;
	}
	if (s_tmCombinePso)
	{
		s_tmCombinePso->Release();
		s_tmCombinePso = nullptr;
	}
	if (s_tmRoot)
	{
		s_tmRoot->Release();
		s_tmRoot = nullptr;
	}
	if (s_tmHeap)
	{
		s_tmHeap->Release();
		s_tmHeap = nullptr;
	}
	if (s_rangeBuf)
	{
		s_rangeBuf->Release();
		s_rangeBuf = nullptr;
	}
	if (s_mvPso)
	{
		s_mvPso->Release();
		s_mvPso = nullptr;
	}
	if (s_mvRoot)
	{
		s_mvRoot->Release();
		s_mvRoot = nullptr;
	}
	if (s_mvHeap)
	{
		s_mvHeap->Release();
		s_mvHeap = nullptr;
	}
	if (s_mvCb)
	{
		s_mvCb->Release();
		s_mvCb = nullptr;
	}
	if (s_srFence)
	{
		UINT64 want = 0;
		for (int i = 0; i < kSrSlots; ++i)
		{
			if (s_srSlotFence[i] > want)
				want = s_srSlotFence[i];
		}
		if (want && s_srFence->GetCompletedValue() < want)
		{
			HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (ev)
			{
				if (SUCCEEDED(s_srFence->SetEventOnCompletion(want, ev)))
					WaitForSingleObject(ev, 2000);
				CloseHandle(ev);
			}
		}
	}
	for (int i = 0; i < kSrSlots; ++i)
	{
		if (s_srCmds[i])
		{
			s_srCmds[i]->Release();
			s_srCmds[i] = nullptr;
		}
		if (s_srAllocs[i])
		{
			s_srAllocs[i]->Release();
			s_srAllocs[i] = nullptr;
		}
		s_srSlotFence[i] = 0;
	}
	if (s_srFence)
	{
		s_srFence->Release();
		s_srFence = nullptr;
	}

	if (!s_installed)
		return;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourDetach(&(LPVOID&)s_fnCreate, (PBYTE)DlssSr_ContextCreate_Hook);
	DetourDetach(&(LPVOID&)s_fnDispatch, (PBYTE)DlssSr_ContextDispatch_Hook);
	DetourDetach(&(LPVOID&)s_fnDestroy, (PBYTE)DlssSr_ContextDestroy_Hook);
	DetourTransactionCommit();
	s_installed = false;
}

bool DlssSr_Installed(void) { return s_installed || s_tsaaInstalled; }
bool DlssSr_SawContext(void) { return s_sawContext; }
bool DlssSr_SawDispatch(void) { return s_sawDispatch || s_tsaaCount != 0; }
// Live means a successful SR eval within the last 8 job-thread resolves, not
// "succeeded once at boot".
bool DlssSr_Live(void) { return s_srEvalOk != 0 && (s_tsaaCount - s_srLastOkTsaa) <= 8; }
bool DlssSr_TsaaHooked(void) { return s_tsaaInstalled; }
unsigned long long DlssSr_TsaaCount(void) { return s_tsaaCount; }
unsigned long long DlssSr_EvalOkCount(void) { return s_srEvalOk; }
unsigned long long DlssSr_EvalFailCount(void) { return s_srEvalFail; }
unsigned long long DlssSr_SkippedViews(void) { return s_srSkippedViews; }
const char* DlssSr_LastFail(void) { return s_lastFail; }
unsigned DlssSr_RenderWidth(void) { return s_lastRenderSize.width; }
unsigned DlssSr_RenderHeight(void) { return s_lastRenderSize.height; }
unsigned DlssSr_DisplayWidth(void) { return s_displaySize.width; }
unsigned DlssSr_DisplayHeight(void) { return s_displaySize.height; }
unsigned DlssSr_ContextFlags(void) { return s_ctxFlags; }

void DlssSr_DumpStatus(void)
{
	char flagText[192] = {};
	char ngxText[192] = {};
	const unsigned ngxFlags = DlssSr_CreateFlags();
	DlssSr_Log("installed=%d tsaaHook=%d context=%d dispatch=%d tsaa=%llu srOk=%llu srFail=%llu live=%d\n",
		s_installed ? 1 : 0, s_tsaaInstalled ? 1 : 0, s_sawContext ? 1 : 0, s_sawDispatch ? 1 : 0,
		(unsigned long long)s_tsaaCount, (unsigned long long)s_srEvalOk,
		(unsigned long long)s_srEvalFail, DlssSr_Live() ? 1 : 0);
	DlssSr_Log("ss creates=%d destroys=%d evals=%d key=%ux%u->%ux%u flags=0x%X skippedViews=%llu busySkips=%llu dupSkips=%llu drawSkips=%llu drawHook=%d presents=%llu\n",
		DlssNr_SsCreates(), DlssNr_SsDestroys(), DlssNr_SsEvals(),
		s_srKeyInW, s_srKeyInH, s_srKeyOutW, s_srKeyOutH,
		s_srKeyFlags,
		(unsigned long long)s_srSkippedViews, (unsigned long long)s_srBusySkips,
		(unsigned long long)s_srDupSkips, (unsigned long long)s_srDrawsSkipped,
		s_tsaaDrawInstalled ? 1 : 0, (unsigned long long)s_presentSeq);
	DlssSr_Log("count=%llu render=%ux%u display=%ux%u fsr2flags=%u (%s) ngxFlags=0x%X (%s) ngx=%d ss=%d\n",
		(unsigned long long)s_dispatchCount,
		s_lastRenderSize.width, s_lastRenderSize.height,
		s_displaySize.width, s_displaySize.height,
		s_ctxFlags, DlssSr_FlagText(s_ctxFlags, flagText, sizeof(flagText)),
		ngxFlags, DlssSr_NgxFlagText(ngxFlags, ngxText, sizeof(ngxText)),
		DlssNr_NgxReady() ? 1 : 0, DlssNr_SuperSamplingLive() ? 1 : 0);
	if (s_tsaaCount == 0)
		DlssSr_Log("TSAA has not run yet\n");
	if (s_lastFail[0])
		DlssSr_Log("srSkip=%s\n", s_lastFail);
}


// ---------------------------------------------------------------------------
// Present-time Neural Rendering.
//
// Colour is the swapchain backbuffer: post-tonemap, display-referred, which is
// the domain the network was trained on. Scene colour at the resolve is HDR
// linear and peaks past 24.0, so bright sources land outside that domain and
// come back as artifacts. Everything here records onto a command list this file
// owns, submitted after the engine has finished the frame, so no engine
// command-list state is touched.
// ---------------------------------------------------------------------------

static const unsigned kPcFrames = 3;
static ID3D12CommandAllocator* s_pcAlloc[kPcFrames] = {};
static uint64_t s_pcAllocFence[kPcFrames] = {};
static ID3D12GraphicsCommandList* s_pcList = nullptr;
static ID3D12Fence* s_pcFence = nullptr;
static HANDLE s_pcEvent = nullptr;
static uint64_t s_pcFenceValue = 0;
static unsigned s_pcFrame = 0;
static ID3D12Resource* s_pcColor = nullptr;
static ID3D12Resource* s_pcOut = nullptr;
static D3D12_RESOURCE_STATES s_pcColorState = D3D12_RESOURCE_STATE_COMMON;
static D3D12_RESOURCE_STATES s_pcOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
static unsigned long long s_pcEvals = 0;
static unsigned long long s_pcFails = 0;
static bool s_pcLatched = false;
static bool s_pcLoggedRun = false;
static Vector3D s_pcPrevOrigin;
static bool s_pcHavePrevOrigin = false;

// A UAV cannot carry an _SRGB format, but CopyResource is legal between members
// of one typeless family, so the sRGB backbuffer still takes the result.
static DXGI_FORMAT DlssSr_UavTwin(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
	default: return f;
	}
}

static bool DlssSr_EnsurePresentCmd(ID3D12Device* device)
{
	if (!s_pcFence)
	{
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_pcFence))))
			return false;
		s_pcEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!s_pcEvent)
			return false;
	}
	for (unsigned i = 0; i < kPcFrames; ++i)
	{
		if (s_pcAlloc[i])
			continue;
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&s_pcAlloc[i]))))
			return false;
	}
	if (!s_pcList)
	{
		if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			s_pcAlloc[0], nullptr, IID_PPV_ARGS(&s_pcList))))
			return false;
		s_pcList->Close();
	}
	return true;
}

static bool DlssSr_EnsurePresentTargets(ID3D12Device* device, unsigned w, unsigned h,
	DXGI_FORMAT backFmt)
{
	const DXGI_FORMAT uavFmt = DlssSr_UavTwin(backFmt);
	if (s_pcColor)
	{
		const D3D12_RESOURCE_DESC have = s_pcColor->GetDesc();
		if (have.Width != w || have.Height != h || have.Format != uavFmt)
		{
			DlssSr_ResetStateCache();
			s_pcColor->Release();
			s_pcColor = nullptr;
			if (s_pcOut)
			{
				s_pcOut->Release();
				s_pcOut = nullptr;
			}
		}
	}
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = uavFmt;
	d.SampleDesc.Count = 1;
	if (!s_pcColor)
	{
		d.Flags = D3D12_RESOURCE_FLAG_NONE;
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_pcColor))))
			return false;
		s_pcColorState = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	if (!s_pcOut)
	{
		d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&s_pcOut))))
			return false;
		s_pcOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	return true;
}

static void DlssSr_PcTo(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
	D3D12_RESOURCE_STATES& tracked, D3D12_RESOURCE_STATES want)
{
	DlssSr_Barrier(cmd, res, tracked, want);
	tracked = want;
}

void DlssSr_PresentNrPass(void* swapChain)
{
	if (!DlssNr_DlssStackAllowed())
		return;
	if (s_pcLatched || sdk_dlssnr_present_pass.GetInt() == 0 || !DlssNr_NrReplaceEnabled())
		return;
	IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(swapChain);
	ID3D12CommandQueue* queue = DirectX_GetDx12GameQueue();
	if (!sc || !queue)
		return;

	ID3D12Device* device = nullptr;
	if (FAILED(sc->GetDevice(IID_PPV_ARGS(&device))) || !device)
		return;
	if (!DlssNr_EnsureNgxSession(device) || !DlssSr_EnsurePresentCmd(device))
	{
		device->Release();
		return;
	}

	UINT index = 0;
	IDXGISwapChain3* sc3 = nullptr;
	if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3)
	{
		index = sc3->GetCurrentBackBufferIndex();
		sc3->Release();
	}
	ID3D12Resource* back = nullptr;
	if (FAILED(sc->GetBuffer(index, IID_PPV_ARGS(&back))) || !back)
	{
		device->Release();
		return;
	}

	const D3D12_RESOURCE_DESC bd = back->GetDesc();
	const unsigned w = static_cast<unsigned>(bd.Width);
	const unsigned h = bd.Height;
	if (!w || !h || !DlssSr_EnsurePresentTargets(device, w, h, bd.Format))
	{
		back->Release();
		device->Release();
		return;
	}

	// The allocator this frame reuses may still be executing from two frames ago.
	const unsigned slot = s_pcFrame % kPcFrames;
	if (s_pcAllocFence[slot] && s_pcFence->GetCompletedValue() < s_pcAllocFence[slot])
	{
		s_pcFence->SetEventOnCompletion(s_pcAllocFence[slot], s_pcEvent);
		WaitForSingleObject(s_pcEvent, 1000);
	}
	if (FAILED(s_pcAlloc[slot]->Reset()) || FAILED(s_pcList->Reset(s_pcAlloc[slot], nullptr)))
	{
		back->Release();
		device->Release();
		return;
	}
	ID3D12GraphicsCommandList* cmd = s_pcList;

	const D3D12_RESOURCE_STATES backState =
		DlssSr_EngineState(back, D3D12_RESOURCE_STATE_RENDER_TARGET);

	DlssSr_Barrier(cmd, back, backState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	DlssSr_PcTo(cmd, s_pcColor, s_pcColorState, D3D12_RESOURCE_STATE_COPY_DEST);
	DlssSr_CopyMip0(cmd, s_pcColor, back);
	DlssSr_PcTo(cmd, s_pcColor, s_pcColorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	DlssSr_Barrier(cmd, back, D3D12_RESOURCE_STATE_COPY_SOURCE, backState);
	DlssSr_PcTo(cmd, s_pcOut, s_pcOutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	bool reset = !s_pcLoggedRun;
	if (g_vecRenderOrigin)
	{
		if (s_pcHavePrevOrigin)
		{
			const float dx = g_vecRenderOrigin->x - s_pcPrevOrigin.x;
			const float dy = g_vecRenderOrigin->y - s_pcPrevOrigin.y;
			const float dz = g_vecRenderOrigin->z - s_pcPrevOrigin.z;
			if ((dx * dx + dy * dy + dz * dz) > (250.0f * 250.0f))
				reset = true;
		}
		else
			reset = true;
		s_pcPrevOrigin = *g_vecRenderOrigin;
		s_pcHavePrevOrigin = true;
	}

	const D3D12_RESOURCE_STATES shaderRead = static_cast<D3D12_RESOURCE_STATES>(
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	ID3D12Resource* depth = DlssSr_SceneDepth();
	D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	if (depth)
	{
		depthState = DlssSr_EngineState(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		DlssSr_Barrier(cmd, depth, depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
	ID3D12Resource* mv = (DlssSr_MvMode() == 2) ? nullptr : DlssSr_FromSlot(NetObsSym_t::RtMrt1);
	D3D12_RESOURCE_STATES mvState = shaderRead;
	if (mv)
	{
		mvState = DlssSr_EngineState(mv, shaderRead);
		DlssSr_Barrier(cmd, mv, mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
	// _rt_MRT1 stores (curUV - prevUV) * 1024; NGX wants prev-cur in pixels.
	const float mvScaleX = mv ? -(static_cast<float>(w) / 1024.0f) : 0.0f;
	const float mvScaleY = mv ? -(static_cast<float>(h) / 1024.0f) : 0.0f;

	const int ev = DlssNr_ReplaceTsaa(cmd, s_pcColor, s_pcOut, depth, mv,
		w, h, reset, mvScaleX, mvScaleY);

	if (mv)
		DlssSr_Barrier(cmd, mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mvState);
	if (depth)
		DlssSr_Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState);

	if (ev == kNgxSuccess)
	{
		DlssSr_UavBarrier(cmd, s_pcOut);
		DlssSr_PcTo(cmd, s_pcOut, s_pcOutState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		DlssSr_Barrier(cmd, back, backState, D3D12_RESOURCE_STATE_COPY_DEST);
		DlssSr_CopyMip0(cmd, back, s_pcOut);
		DlssSr_Barrier(cmd, back, D3D12_RESOURCE_STATE_COPY_DEST, backState);
		DlssSr_PcTo(cmd, s_pcOut, s_pcOutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		++s_pcEvals;
	}
	else if (++s_pcFails >= 16)
	{
		s_pcLatched = true;
		DlssSr_Log("present NR latched off after %llu failures, last 0x%08X\n",
			(unsigned long long)s_pcFails, static_cast<unsigned>(ev));
	}

	if (SUCCEEDED(cmd->Close()))
	{
		ID3D12CommandList* lists[] = { cmd };
		queue->ExecuteCommandLists(1, lists);
		++s_pcFenceValue;
		queue->Signal(s_pcFence, s_pcFenceValue);
		s_pcAllocFence[slot] = s_pcFenceValue;
		++s_pcFrame;
	}

	if (!s_pcLoggedRun && ev == kNgxSuccess)
	{
		s_pcLoggedRun = true;
		DlssSr_Log("present NR %ux%u backfmt=%u depth=%d mv=%d\n",
			w, h, static_cast<unsigned>(bd.Format), depth ? 1 : 0, mv ? 1 : 0);
	}

	if (mv)
		mv->Release();
	if (depth)
		depth->Release();
	back->Release();
	device->Release();
}

static void DlssSr_ReleasePresentPass(void)
{
	if (s_pcFence && s_pcEvent && s_pcFenceValue
		&& s_pcFence->GetCompletedValue() < s_pcFenceValue)
	{
		s_pcFence->SetEventOnCompletion(s_pcFenceValue, s_pcEvent);
		WaitForSingleObject(s_pcEvent, 1000);
	}
	if (s_pcList)
	{
		s_pcList->Release();
		s_pcList = nullptr;
	}
	for (unsigned i = 0; i < kPcFrames; ++i)
	{
		if (s_pcAlloc[i])
		{
			s_pcAlloc[i]->Release();
			s_pcAlloc[i] = nullptr;
		}
	}
	if (s_pcFence)
	{
		s_pcFence->Release();
		s_pcFence = nullptr;
	}
	if (s_pcEvent)
	{
		CloseHandle(s_pcEvent);
		s_pcEvent = nullptr;
	}
	if (s_pcColor)
	{
		s_pcColor->Release();
		s_pcColor = nullptr;
	}
	if (s_pcOut)
	{
		s_pcOut->Release();
		s_pcOut = nullptr;
	}
	DlssSr_ResetRtCache();
}

static void DlssSr_Dump_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	DlssSr_DumpStatus();
}
static ConCommand dlss_sr_dump("dlss_sr_dump", DlssSr_Dump_f, "Report what the engine's FSR2 upscale pass is doing.", FCVAR_RELEASE);

