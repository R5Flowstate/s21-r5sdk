//=============================================================================//
//
// Purpose: DLSS Neural Rendering (NGX feature 18) on the S21 DX12 path.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_bridge_addrs.h"

#include "windows/dlssnr.h"
#include "windows/dlss_sr.h"
#include "windows/id3dx.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "tier1/cmd.h"
#include "tier0/dbg.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <winver.h>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")
#include <cstdio>
#include <cstdarg>
#include <cstring>

// ITexture* globals in r5apex_dx12 (same sites as framegen).

static constexpr int kNgxFeatureSuperSampling = 1;
static constexpr int kNgxFeatureNeuralRendering = 18;
static constexpr int kNgxSuccess = 1;
static constexpr int kNgxFailNotSupported = static_cast<int>(0xBAD00001);
static constexpr int kNgxFailAlreadyExists = static_cast<int>(0xBAD00003);
static constexpr int kNgxFailInvalidParameter = static_cast<int>(0xBAD00005);
static constexpr int kNgxFailMissingInput = static_cast<int>(0xBAD0000A);
static constexpr int kNgxFailNotInitialized = static_cast<int>(0xBAD00007);
static constexpr int kNgxFailUnableToInit = static_cast<int>(0xBAD0000B);
static constexpr int kNgxFailOutOfDate = static_cast<int>(0xBAD0000C);
static constexpr int kNgxFailDenied = static_cast<int>(0xBAD00011);

// NVSDK_NGX_Version_API.
static const int kNgxSdkVersion = 0x15;
// Generic Streamline project/app id. A made-up 0x1000000 never maps an NGX CMS id.
static const unsigned long long kNgxAppId = 0x24480451ull;
static const char* kNgxProjectId = "24480451-f00d-face-1304-0308dabad187";
static const int kNgxEngineCustom = 0;

struct ID3D11Resource;

typedef int NVSDK_NGX_Result;
struct NVSDK_NGX_Handle { unsigned int Id; };

struct NVSDK_NGX_Parameter
{
	virtual void Set(const char*, unsigned long long) = 0;
	virtual void Set(const char*, float) = 0;
	virtual void Set(const char*, double) = 0;
	virtual void Set(const char*, unsigned int) = 0;
	virtual void Set(const char*, int) = 0;
	virtual void Set(const char*, ID3D11Resource*) = 0;
	virtual void Set(const char*, ID3D12Resource*) = 0;
	virtual void Set(const char*, void*) = 0;

	virtual NVSDK_NGX_Result Get(const char*, unsigned long long*) const = 0;
	virtual NVSDK_NGX_Result Get(const char*, float*) const = 0;
	virtual NVSDK_NGX_Result Get(const char*, double*) const = 0;
	virtual NVSDK_NGX_Result Get(const char*, unsigned int*) const = 0;
	virtual NVSDK_NGX_Result Get(const char*, int*) const = 0;
	virtual NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const = 0;
	virtual NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const = 0;
	virtual NVSDK_NGX_Result Get(const char*, void**) const = 0;

	virtual void Reset() = 0;
};

struct NVSDK_NGX_PathListInfo
{
	wchar_t const* const* Path;
	unsigned int Length;
};
struct NVSDK_NGX_LoggingInfo
{
	void* LoggingCallback;
	int MinimumLoggingLevel;
	bool DisableOtherLoggingSinks;
};
struct NVSDK_NGX_FeatureCommonInfo
{
	NVSDK_NGX_PathListInfo PathListInfo;
	void* InternalData;
	NVSDK_NGX_LoggingInfo LoggingInfo;
};

static void __cdecl DlssNr_NgxLog(const char* message, int level, int component);

typedef NVSDK_NGX_Result (*PFN_NgxEval)(void*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result (*PFN_NgxCreate)(void*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (*PFN_NgxRelease)(NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result (*PFN_NgxAllocParams)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result (*PFN_NgxDestroyParams)(NVSDK_NGX_Parameter*);
typedef NVSDK_NGX_Result (*PFN_NgxGetCaps)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result (*PFN_NgxScratch)(int, NVSDK_NGX_Parameter*, unsigned long long*);
// Driver-module tails differ from the app-side SDK header: Init_Ext and
// Init_ProjectID take InSDKVersion BEFORE InFeatureInfo. Calling them with the
// header order feeds the info pointer in as the version -- ASLR garbage that the
// core rejects as OutOfDate.
typedef NVSDK_NGX_Result (*PFN_NgxInit)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
typedef NVSDK_NGX_Result (*PFN_NgxInitExt)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
typedef NVSDK_NGX_Result (*PFN_NgxInitProject)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
typedef NVSDK_NGX_Result (*PFN_NgxShutdown)(void);
typedef NVSDK_NGX_Result (*PFN_NgxShutdown1)(ID3D12Device*);

static ConVar settings_dlssnr("settings_dlssnr", "1", FCVAR_RELEASE,
	"DLSS Neural Rendering. Auto-on when nvngx_dlssnr.dll is next to r5apex_dx12.exe on NVIDIA. 0 = force off.");
// The snippet runs its PostProcess kernel -- the blend of the network output
// against the original-colour snapshot, and the only consumer of LocalTone and
// LocalStructure -- only while Intensity is strictly below 1.0. At 1.0 or above
// the raw network output is copied straight to the target.
static ConVar settings_dlssnr_intensity("settings_dlssnr_intensity", "0.85", FCVAR_RELEASE,
	"NR strength, 0.0 to 0.999. A value >= 1.0 is clamped: it would disable the NR post-process blend.");
static ConVar settings_dlssnr_localtone("settings_dlssnr_localtone", "1.0", FCVAR_RELEASE,
	"NR local tone strength (DLSSNR.LocalToneStrength).");
static ConVar settings_dlssnr_localstructure("settings_dlssnr_localstructure", "1.0", FCVAR_RELEASE,
	"NR local structure strength (DLSSNR.LocalStructureStrength).");
// At UseAutoMask 0 the snippet forces its skin and structure strengths to -1.0,
// which switches the network's structure guidance off entirely.
static ConVar settings_dlssnr_automask("settings_dlssnr_automask", "1", FCVAR_RELEASE,
	"NR auto mask (DLSSNR.UseAutoMask). 0 disables the network's structure guidance.");
static ConVar settings_dlssnr_skinstructure("settings_dlssnr_skinstructure", "-1.0", FCVAR_RELEASE,
	"NR skin structure strength. Negative follows settings_dlssnr_localstructure.");
static ConVar sdk_dlssnr_bypass("sdk_dlssnr_bypass", "0", FCVAR_DEVELOPMENTONLY,
	"1 = ask the snippet to blit Color to Output untouched (DLSSNR.Enabled 0). Proves the plumbing without the network.");
static ConVar settings_dlssnr_style("settings_dlssnr_style", "0", FCVAR_RELEASE,
	"NR style index (DLSSNR.Style).");
static ConVar settings_dlssnr_preset("settings_dlssnr_preset", "0", FCVAR_RELEASE,
	"NR preset index (DLSSNR.Preset).");
// Present-time NR scavenges engine RTs through fixed RVAs and a guessed
// vtable slot; keep it opt-in and feed NR from a real NGX/FSR2 contract instead.
static ConVar settings_dlssnr_present("settings_dlssnr_present", "0", FCVAR_RELEASE,
	"Present-time NR from engine RTs (experimental). 0 = off. 1 = on. -1 = auto when NGX SuperSampling never evaluates.");
static ConVar sdk_dlssnr_diag("sdk_dlssnr_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log NGX NR create/evaluate and resource descriptors.");

static PFN_NgxEval s_origEval = nullptr;
static PFN_NgxEval s_origEvalC = nullptr;
static PFN_NgxCreate s_origCreate = nullptr;
static PFN_NgxRelease s_origRelease = nullptr;
static PFN_NgxAllocParams s_origAllocParams = nullptr;
static PFN_NgxDestroyParams s_origDestroyParams = nullptr;
static PFN_NgxGetCaps s_origGetCaps = nullptr;
static PFN_NgxScratch s_origScratch = nullptr;
static PFN_NgxInit s_origInit = nullptr;
static PFN_NgxInitExt s_origInitExt = nullptr;
static PFN_NgxInitProject s_origInitProject = nullptr;
static PFN_NgxShutdown s_origShutdown = nullptr;
static PFN_NgxShutdown1 s_origShutdown1 = nullptr;

static HMODULE s_ngxMod = nullptr;
static HMODULE s_ngxCands[8];
static int s_ngxCandCount = 0;
static int s_lastInitResult = 0;
static ID3D12Device* s_initDevice = nullptr;
static bool s_hooksInstalled = false;
static bool s_hooksFailed = false;
static bool s_loggedNeedDx12 = false;
static bool s_loggedNeedDll = false;
static bool s_loggedNeedNgx = false;
static bool s_loggedFirstRun = false;
static bool s_createLatched = false;
static bool s_initedNgx = false;
static bool s_didPathA = false;
static bool s_gaveUpInstall = false;
static bool s_vendorProbed = false;
static bool s_isNvidia = false;
static bool s_loggedNotNvidia = false;
static bool s_initFailed = false;
static const int kNgxInitAttempts = 2;
static int s_initAttempts = 0;
static int s_initRetryAt = 0;
static bool s_needReset = true;
static int s_createTries = 0;
static int s_presents = 0;
static int s_nrOffPresents = 0;
static int s_ssEvals = 0;
static int s_nrEvals = 0;
static int s_lastNrResult = 0;
static int s_nrProbeResult = 0;
static HMODULE s_nrDllMod = nullptr;

struct FeatureRec { NVSDK_NGX_Handle* handle; int id; };
static FeatureRec s_features[32];
static int s_featureCount = 0;

static NVSDK_NGX_Handle* s_nrHandle = nullptr;
static NVSDK_NGX_Parameter* s_nrParams = nullptr;
static bool s_nrParamsOwned = false;
static UINT s_nrW = 0;
static UINT s_nrH = 0;
static DXGI_FORMAT s_nrFmt = DXGI_FORMAT_UNKNOWN;

static NVSDK_NGX_Handle* s_ssHandle = nullptr;
static NVSDK_NGX_Parameter* s_ssParams = nullptr;
static bool s_ssParamsOwned = false;
static UINT s_ssRenderW = 0;
static UINT s_ssRenderH = 0;
static UINT s_ssDisplayW = 0;
static UINT s_ssDisplayH = 0;
static unsigned s_ssFlags = 0;
static int s_ssQuality = -1;
static bool s_ssCreateLatched = false;
static int s_ssCreateTries = 0;
static int s_ssCreates = 0;
static int s_ssDestroys = 0;
static int s_ssEvalsSinceCreate = 0;
static bool s_ssReuseLogged = false;
static bool s_probe18Done = false;

// Direct feature-18 snippet host. See the block near DlssNr_ReleaseFeature for
// the caller-origin gate this bypasses.
typedef NVSDK_NGX_Result(*PFN_SnipInitExt)(unsigned long long, const wchar_t*, ID3D12Device*, unsigned int, const NVSDK_NGX_FeatureCommonInfo*);
typedef NVSDK_NGX_Result(*PFN_SnipCreate)(void*, void*, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result(*PFN_SnipRelease)(NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result(*PFN_SnipShutdown1)(ID3D12Device*);
typedef DWORD(WINAPI* PFN_GetModuleFileNameW)(HMODULE, LPWSTR, DWORD);

static PFN_SnipInitExt s_snipInit = nullptr;
static PFN_SnipCreate s_snipCreate = nullptr;
static PFN_NgxEval s_snipEval = nullptr;
static PFN_SnipRelease s_snipRelease = nullptr;
static PFN_SnipShutdown1 s_snipShutdown = nullptr;
static HMODULE s_snipMod = nullptr;
static bool s_snipReady = false;
static bool s_snipInited = false;
static bool s_snipGatePatched = false;
static int s_snipInitResult = 0;
static int s_snipCreateResult = 0;
static PFN_GetModuleFileNameW s_realGmfnw = nullptr;
static HMODULE s_selfModule = nullptr;

struct SubTex
{
	ID3D12Resource* tex;
	UINT64 width;
	UINT height;
	DXGI_FORMAT fmt;
	D3D12_RESOURCE_STATES state;
};

static SubTex s_subColor = {};
static SubTex s_subOut = {};
static SubTex s_subDepth = {};
static SubTex s_subMv = {};

struct Retired { ID3D12Resource* tex; int frame; };
static Retired s_retired[8];
static int s_frameNo = 0;

static ID3D12CommandAllocator* s_presentAlloc = nullptr;
static ID3D12GraphicsCommandList* s_presentCmd = nullptr;
static ID3D12Resource* s_dummyMv = nullptr;
static UINT s_dummyW = 0;
static UINT s_dummyH = 0;
static uint64_t s_presentFrameId = 0;
static ID3D12Resource* s_scratch = nullptr;
static unsigned long long s_scratchBytes = 0;
static ID3D12Resource* s_nrScratch = nullptr;

static CRITICAL_SECTION s_cs;
static bool s_csInit = false;

static void DlssNr_EnsureCs(void)
{
	if (!s_csInit)
	{
		InitializeCriticalSection(&s_cs);
		s_csInit = true;
	}
}

template <typename T>
static void Nr_Release(T*& p)
{
	if (p)
	{
		p->Release();
		p = nullptr;
	}
}

static const char* DlssNr_ResultName(int r)
{
	switch (static_cast<unsigned>(r))
	{
	case 0x00000001: return "Success";
	case 0xBAD00000: return "Fail";
	case 0xBAD00001: return "FeatureNotSupported";
	case 0xBAD00002: return "PlatformError";
	case 0xBAD00003: return "FeatureAlreadyExists";
	case 0xBAD00004: return "FeatureNotFound";
	case 0xBAD00005: return "InvalidParameter";
	case 0xBAD00006: return "ScratchBufferTooSmall";
	case 0xBAD00007: return "NotInitialized";
	case 0xBAD00008: return "UnsupportedInputFormat";
	case 0xBAD00009: return "RWFlagMissing";
	case 0xBAD0000A: return "MissingInput";
	case 0xBAD0000B: return "UnableToInitializeFeature";
	case 0xBAD0000C: return "OutOfDate";
	case 0xBAD0000D: return "OutOfGPUMemory";
	case 0xBAD0000E: return "UnsupportedFormat";
	case 0xBAD0000F: return "UnableToWriteToAppDataPath";
	case 0xBAD00010: return "UnsupportedParameter";
	case 0xBAD00011: return "Denied";
	case 0xBAD00012: return "NotImplemented";
	default:         return "other";
	}
}

static bool DlssNr_Diag(void)
{
	return sdk_dlssnr_diag.GetBool();
}

// Level 2 makes the NGX core and the CG2R snippet emit several lines per
// evaluate. Every one reaches DlssNr_FileLog on the present thread, so it is a
// per-frame cost and stays behind the diag gate.
static int DlssNr_NgxLogLevel(void)
{
	return DlssNr_Diag() ? 2 : 0;
}

static void DlssNr_ApplyNgxLogEnv(void)
{
	SetEnvironmentVariableA("__NGX_LOG_LEVEL", DlssNr_Diag() ? "2" : "0");
}

static void DlssNr_ExeDirW(wchar_t* out, size_t cap)
{
	GetModuleFileNameW(NULL, out, static_cast<DWORD>(cap));
	wchar_t* slash = out;
	for (wchar_t* p = out; *p; ++p)
	{
		if (*p == L'\\' || *p == L'/')
			slash = p;
	}
	*slash = L'\0';
}

static void DlssNr_LogPathA(char* out, size_t cap)
{
	wchar_t dir[MAX_PATH] = {};
	DlssNr_ExeDirW(dir, MAX_PATH);
	_snprintf_s(out, cap, _TRUNCATE, "%ls\\dlssnr.log", dir);
}

void DlssNr_FileLog(const char* fmt, ...)
{
	char line[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
	va_end(ap);

	// One open/append/close per line is a synchronous directory-metadata write.
	// Hold the handle and flush instead; the log still survives a crash.
	static FILE* s_f = nullptr;
	if (!s_f)
	{
		char path[MAX_PATH] = {};
		DlssNr_LogPathA(path, MAX_PATH);
		if (fopen_s(&s_f, path, "a") != 0)
			s_f = nullptr;
	}
	if (!s_f)
		return;

	SYSTEMTIME st = {};
	GetLocalTime(&st);
	fprintf(s_f, "%02u:%02u:%02u.%03u  %s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
	if (!line[0] || line[strlen(line) - 1] != '\n')
		fputc('\n', s_f);
	fflush(s_f);
}

static void DlssNr_Log(const char* fmt, ...)
{
	char line[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
	va_end(ap);

	Warning(eDLL_T::MS, "[DLSSNR] %s", line);
	DlssNr_FileLog("%s", line);
}

static void DlssNr_LogExport(HMODULE mod, const char* name)
{
	void* fn = GetProcAddress(mod, name);
	if (fn)
		DlssNr_Log("    %s %p\n", name, fn);
}

static bool DlssNr_NrDllPresent(void)
{
	wchar_t dir[MAX_PATH] = {};
	DlssNr_ExeDirW(dir, MAX_PATH);
	wchar_t path[MAX_PATH] = {};
	_snwprintf_s(path, _TRUNCATE, L"%s\\nvngx_dlssnr.dll", dir);
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// nvngx_dlss.dll is the SuperSampling runtime. Cached: the SR install runs off
// the present hook, and the answer cannot change without a relaunch.
static bool DlssNr_SrDllPresent(void)
{
	static int s_present = -1;
	if (s_present < 0)
	{
		wchar_t dir[MAX_PATH] = {};
		DlssNr_ExeDirW(dir, MAX_PATH);
		wchar_t path[MAX_PATH] = {};
		_snwprintf_s(path, _TRUNCATE, L"%s\\nvngx_dlss.dll", dir);
		s_present = (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
	}
	return s_present != 0;
}

bool DlssNr_SrRuntimePresent(void)
{
	return DlssNr_SrDllPresent();
}

bool DlssNr_DllPresent(void)
{
	return DlssNr_NrDllPresent();
}

static void DlssNr_ProbeVendorFromDevice(ID3D12Device* dev)
{
	if (s_vendorProbed || !dev)
		return;

	const LUID luid = dev->GetAdapterLuid();

	HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
	if (!dxgi)
		dxgi = LoadLibraryW(L"dxgi.dll");
	if (!dxgi)
		return;
	typedef HRESULT(WINAPI* PFN_CreateFactory)(REFIID, void**);
	auto create = reinterpret_cast<PFN_CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
	if (!create)
		return;

	IDXGIFactory4* factory = nullptr;
	if (FAILED(create(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || !factory)
		return;

	IDXGIAdapter1* adapter = nullptr;
	if (SUCCEEDED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1),
		reinterpret_cast<void**>(&adapter))) && adapter)
	{
		DXGI_ADAPTER_DESC1 d = {};
		if (SUCCEEDED(adapter->GetDesc1(&d)))
		{
			s_isNvidia = (d.VendorId == 0x10DE);
			s_vendorProbed = true;
			if (!s_isNvidia && DlssNr_NrDllPresent() && !s_loggedNotNvidia)
			{
				s_loggedNotNvidia = true;
				Warning(eDLL_T::MS, "[DLSSNR] not NVIDIA (vendor 0x%04X) -- idle\n", d.VendorId);
			}
		}
		adapter->Release();
	}
	factory->Release();
}

static void DlssNr_ProbeVendor(void)
{
	if (s_vendorProbed)
		return;
	IDXGISwapChain* sc = Dx12_GetGameSwapChain();
	if (!sc)
		return;

	ID3D12Device* dev = nullptr;
	if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))) || !dev)
		return;
	DlssNr_ProbeVendorFromDevice(dev);
	dev->Release();
}

bool DlssNr_VendorKnown(void)
{
	return s_vendorProbed;
}

bool DlssNr_IsNvidia(void)
{
	return s_vendorProbed && s_isNvidia;
}

bool DlssNr_DlssStackAllowed(void)
{
	DlssNr_ProbeVendor();
	if (s_vendorProbed && !s_isNvidia)
		return false;
	return DlssNr_SrDllPresent() || DlssNr_NrDllPresent();
}

bool DlssNr_IsHooked(void)
{
	return s_origEval != nullptr && s_initedNgx;
}

bool DlssNr_NgxReady(void)
{
	return s_origEval != nullptr && s_origCreate != nullptr && s_initedNgx;
}

bool DlssNr_SuperSamplingLive(void)
{
	return s_ssHandle != nullptr;
}

bool DlssNr_FeatureLive(void)
{
	return s_nrHandle != nullptr;
}

bool DlssNr_Latched(void)
{
	return s_createLatched;
}

bool DlssNr_Running(void)
{
	return s_loggedFirstRun;
}

unsigned DlssNr_Width(void)
{
	return s_nrW;
}

unsigned DlssNr_Height(void)
{
	return s_nrH;
}

int DlssNr_SsEvals(void)
{
	return s_ssEvals;
}

int DlssNr_SsCreates(void)
{
	return s_ssCreates;
}

int DlssNr_SsDestroys(void)
{
	return s_ssDestroys;
}

void DlssNr_SsFileLog(const char* action, unsigned renderW, unsigned renderH,
	unsigned displayW, unsigned displayH, int quality, unsigned flags)
{
	DlssNr_FileLog("[DLSS-SR] ss key in=%ux%u out=%ux%u q=%d flags=0x%X action=%s creates=%d destroys=%d evals=%d\n",
		renderW, renderH, displayW, displayH, quality, flags, action,
		s_ssCreates, s_ssDestroys, s_ssEvals);
}

int DlssNr_NrEvals(void)
{
	return s_nrEvals;
}

int DlssNr_LastNrResult(void)
{
	return s_lastNrResult;
}

int DlssNr_NrProbeResult(void)
{
	return s_nrProbeResult;
}

const char* DlssNr_LastNrResultName(void)
{
	return DlssNr_ResultName(s_lastNrResult);
}

const char* DlssNr_NrHostText(void)
{
	static char buf[160];
	if (!DlssNr_NrDllPresent())
		return "snippet dll missing";
	if (!s_snipReady)
		return s_snipGatePatched ? "snippet bound" : "snippet not bound";
	_snprintf_s(buf, sizeof(buf), _TRUNCATE,
		"snippet gate=%s init=0x%08X create=0x%08X",
		s_snipGatePatched ? "patched" : "raw",
		static_cast<unsigned>(s_snipInitResult), static_cast<unsigned>(s_snipCreateResult));
	return buf;
}

const char* DlssNr_NrStatusText(void)
{
	if (s_loggedFirstRun)
		return "RUNNING";
	if (s_createLatched)
		return "FAILED";
	if (s_initedNgx && DlssSr_TsaaCount() == 0)
		return "WAITING";
	if (s_initedNgx)
		return "STANDBY";
	return "STANDBY";
}

static void DlssNr_ReleaseFeature(const char* why);
static void DlssNr_ReleaseSuperSamplingHandle(void);
static bool DlssNr_TryInstallHooks(void);
static bool DlssNr_TryInit(ID3D12Device* device);
static bool DlssNr_EnsureScratch(int featureId, NVSDK_NGX_Parameter* p, ID3D12Device* dev);
static void DlssNr_ShutdownSession(void);
static void DlssNr_PreloadNrModel(void);
static bool DlssNr_ProbeFeature18(void);

void DlssNr_ResetLatch(void)
{
	DlssNr_EnsureCs();
	EnterCriticalSection(&s_cs);
	s_createLatched = false;
	s_createTries = 0;
	s_loggedFirstRun = false;
	s_nrEvals = 0;
	s_lastNrResult = 0;
	s_initFailed = false;
	s_initAttempts = 0;
	s_initRetryAt = 0;
	s_hooksFailed = false;
	s_gaveUpInstall = false;
	s_loggedNeedNgx = false;
	s_loggedNeedDll = false;
	if (s_initedNgx)
		DlssNr_ShutdownSession();
	s_nrDllMod = GetModuleHandleW(L"nvngx_dlssnr.dll");
	s_nrProbeResult = 0;
	s_initedNgx = false;
	s_lastInitResult = 0;
	s_ngxCandCount = 0;
	s_ssCreateLatched = false;
	s_ssCreateTries = 0;
	s_probe18Done = false;
	DlssNr_ReleaseSuperSamplingHandle();
	s_origEval = nullptr;
	s_origEvalC = nullptr;
	s_origCreate = nullptr;
	s_origInit = nullptr;
	s_origInitExt = nullptr;
	s_origInitProject = nullptr;
	s_origShutdown = nullptr;
	s_origShutdown1 = nullptr;
	s_ngxMod = nullptr;
	DlssNr_ReleaseFeature("reset latch");
	DlssNr_TryInstallHooks();
	LeaveCriticalSection(&s_cs);
}

const char* DlssNr_HookText(void)
{
	static char buf[192];
	if (!s_origEval)
		return s_gaveUpInstall ? "no NGX runtime found (see dlssnr.log)" : "waiting for the driver NGX runtime";
	if (s_initedNgx)
		return "NGX session ready";
	if (!s_initFailed)
		return "NGX loaded, initializing";
	if (s_lastInitResult == kNgxFailOutOfDate)
		return "NGX Init OutOfDate -- update the NVIDIA driver";
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "NGX Init failed 0x%08X %s",
		static_cast<unsigned>(s_lastInitResult), DlssNr_ResultName(s_lastInitResult));
	return buf;
}

static bool DlssNr_ShouldEvaluate(void)
{
	if (!DirectX_IsDx12Mode() || settings_dlssnr.GetInt() == 0)
		return false;
	if (!DlssNr_NrDllPresent())
		return false;
	if (s_initedNgx)
		return true;
	return s_vendorProbed && s_isNvidia;
}

static void DlssNr_Remember(NVSDK_NGX_Handle* h, int id)
{
	if (!h || s_featureCount >= 32)
		return;
	for (int i = 0; i < s_featureCount; ++i)
	{
		if (s_features[i].handle == h)
		{
			s_features[i].id = id;
			return;
		}
	}
	s_features[s_featureCount].handle = h;
	s_features[s_featureCount].id = id;
	++s_featureCount;
}

static void DlssNr_CopyUInt(const NVSDK_NGX_Parameter* src, NVSDK_NGX_Parameter* dst, const char* key)
{
	unsigned int v = 0;
	if (src->Get(key, &v) == kNgxSuccess)
		dst->Set(key, v);
}

static void DlssNr_Retire(ID3D12Resource* tex)
{
	if (!tex)
		return;
	for (int i = 0; i < 8; ++i)
	{
		if (!s_retired[i].tex)
		{
			s_retired[i].tex = tex;
			s_retired[i].frame = s_frameNo;
			return;
		}
	}
	s_retired[0].tex->Release();
	s_retired[0].tex = tex;
	s_retired[0].frame = s_frameNo;
}

static void DlssNr_DrainRetired(void)
{
	for (int i = 0; i < 8; ++i)
	{
		if (s_retired[i].tex && (s_frameNo - s_retired[i].frame) > 8)
		{
			s_retired[i].tex->Release();
			s_retired[i].tex = nullptr;
		}
	}
}

static void DlssNr_Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
	D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
	if (!list || !res || from == to)
		return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.StateBefore = from;
	b.Transition.StateAfter = to;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	list->ResourceBarrier(1, &b);
}

static void DlssNr_ToState(ID3D12GraphicsCommandList* list, SubTex& s, D3D12_RESOURCE_STATES target)
{
	if (!s.tex || s.state == target)
		return;
	DlssNr_Transition(list, s.tex, s.state, target);
	s.state = target;
}

static void DlssNr_CopyMip0(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src)
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

static bool DlssNr_EnsureSub(SubTex& s, ID3D12Device* dev, const D3D12_RESOURCE_DESC& src, const char* label)
{
	if (s.tex && s.width == src.Width && s.height == src.Height && s.fmt == src.Format)
		return true;

	if (s.tex)
	{
		DlssNr_Retire(s.tex);
		s.tex = nullptr;
	}

	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC d = src;
	d.MipLevels = 1;
	d.DepthOrArraySize = 1;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.tex));
	if (FAILED(hr))
	{
		Warning(eDLL_T::MS, "[DLSSNR] %s create failed %llux%u fmt=%u hr=0x%08X\n",
			label, src.Width, src.Height, src.Format, hr);
		return false;
	}
	s.width = src.Width;
	s.height = src.Height;
	s.fmt = src.Format;
	s.state = D3D12_RESOURCE_STATE_COMMON;
	if (DlssNr_Diag())
		Warning(eDLL_T::MS, "[DLSSNR] %s sub %llux%u fmt=%u\n", label, s.width, s.height, s.fmt);
	return true;
}

static ID3D12Resource* DlssNr_CreateTex(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT fmt)
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
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	ID3D12Resource* res = nullptr;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res))))
		return nullptr;
	return res;
}

// The VEH writes the dump before SEH unwinds, so __try cannot make a wild
// virtual call survivable -- every candidate must be proven live first.
static bool DlssNr_PtrOk(const void* p, size_t bytes, bool wantExec)
{
	const uintptr_t v = reinterpret_cast<uintptr_t>(p);
	if (v < 0x10000 || v > 0x00007FFFFFFFFFFFull)
		return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(p, &mbi, sizeof(mbi)))
		return false;
	if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
		return false;
	if (wantExec && !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
		| PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
		return false;
	return v + bytes <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

static bool DlssNr_LooksLikeComObject(void* p)
{
	if (!DlssNr_PtrOk(p, sizeof(void*), false))
		return false;
	void** vt = *reinterpret_cast<void***>(p);
	if (!DlssNr_PtrOk(vt, 3 * sizeof(void*), false))
		return false;
	return DlssNr_PtrOk(vt[0], 1, true) && DlssNr_PtrOk(vt[1], 1, true)
		&& DlssNr_PtrOk(vt[2], 1, true);
}

static ID3D12Resource* DlssNr_TryQiResource(void* cand)
{
	if (!cand || !DlssNr_LooksLikeComObject(cand))
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

static ID3D12Resource* DlssNr_ProbeResource(void* pObj, size_t nbytes, size_t* hint)
{
	if (!pObj || !DlssNr_PtrOk(pObj, nbytes, false))
		return nullptr;
	const uintptr_t base = reinterpret_cast<uintptr_t>(pObj);
	if (hint && *hint && *hint + sizeof(void*) <= nbytes)
	{
		ID3D12Resource* res = DlssNr_TryQiResource(*reinterpret_cast<void**>(base + *hint));
		if (res)
			return res;
	}
	for (size_t off = 0; off + sizeof(void*) <= nbytes; off += sizeof(void*))
	{
		ID3D12Resource* res = DlssNr_TryQiResource(*reinterpret_cast<void**>(base + off));
		if (res)
		{
			if (hint)
				*hint = off;
			return res;
		}
	}
	return nullptr;
}

static ID3D12Resource* DlssNr_FromEngineRt(const NetObsSym_t sym)
{
	static size_t s_innerHint = 0;
	static size_t s_outerHint = 0;

	const uintptr_t slot = NetObs_Sym(sym);
	if (!slot)
		return nullptr;
	void* pTex = *reinterpret_cast<void**>(slot);
	if (!pTex || !DlssNr_PtrOk(pTex, sizeof(void*), false))
		return nullptr;
	void* inner = nullptr;
	void** vt = *reinterpret_cast<void***>(pTex);
	if (DlssNr_PtrOk(vt, 29 * sizeof(void*), false) && DlssNr_PtrOk(vt[28], 1, true))
	{
		__try
		{
			using Fn = void* (__fastcall*)(void*, unsigned);
			const Fn getInner = reinterpret_cast<Fn>(vt[28]);
			inner = getInner(pTex, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			inner = nullptr;
		}
	}
	ID3D12Resource* res = DlssNr_ProbeResource(inner ? inner : pTex, 0x180, &s_innerHint);
	if (!res)
		res = DlssNr_ProbeResource(pTex, 0x80, &s_outerHint);
	return res;
}

// nvngx_*.dll are feature MODELS. They export the same NGX entry points as the
// runtime; calling into one instead of nvngx/_nvngx is the OutOfDate footgun.
static bool DlssNr_IsFeatureModel(HMODULE mod)
{
	wchar_t path[MAX_PATH] = {};
	if (!GetModuleFileNameW(mod, path, MAX_PATH))
		return false;
	const wchar_t* leaf = wcsrchr(path, L'\\');
	leaf = leaf ? leaf + 1 : path;
	return _wcsnicmp(leaf, L"nvngx_", 6) == 0;
}

static HMODULE DlssNr_ModuleHasD3D12Eval(HMODULE mod)
{
	if (!mod || DlssNr_IsFeatureModel(mod))
		return nullptr;
	if (!GetProcAddress(mod, "NVSDK_NGX_D3D12_CreateFeature"))
		return nullptr;
	if (GetProcAddress(mod, "NVSDK_NGX_D3D12_EvaluateFeature")
		|| GetProcAddress(mod, "NVSDK_NGX_D3D12_EvaluateFeature_C"))
		return mod;
	return nullptr;
}

static HMODULE DlssNr_FindNgx(void)
{
	static const wchar_t* kNames[] = {
		L"nvngx.dll", L"_nvngx.dll", L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll"
	};
	for (int i = 0; i < 5; ++i)
	{
		HMODULE hit = DlssNr_ModuleHasD3D12Eval(GetModuleHandleW(kNames[i]));
		if (hit)
			return hit;
	}

	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (!k32)
		return nullptr;
	typedef BOOL(WINAPI* PFN_Enum)(HANDLE, HMODULE*, DWORD, LPDWORD);
	auto enumMods = reinterpret_cast<PFN_Enum>(GetProcAddress(k32, "K32EnumProcessModules"));
	if (!enumMods)
	{
		HMODULE psapi = LoadLibraryW(L"psapi.dll");
		if (psapi)
			enumMods = reinterpret_cast<PFN_Enum>(GetProcAddress(psapi, "EnumProcessModules"));
	}
	if (!enumMods)
		return nullptr;

	HMODULE mods[1024];
	DWORD needed = 0;
	const BOOL ok = enumMods(GetCurrentProcess(), mods, sizeof(mods), &needed);
	const DWORD n = needed / sizeof(HMODULE);
	if (!ok && n == 0)
		return nullptr;
	const DWORD cap = n < 1024 ? n : 1024;
	for (DWORD i = 0; i < cap; ++i)
	{
		HMODULE hit = DlssNr_ModuleHasD3D12Eval(mods[i]);
		if (hit)
			return hit;
	}
	return nullptr;
}

struct NgxDisk
{
	wchar_t path[MAX_PATH];
	unsigned long long ver;
	bool stub;
	bool canon;
	bool game;
};

static NgxDisk s_ngxDisk[16];
static int s_ngxDiskCount = 0;

static unsigned long long DlssNr_FileVer(const wchar_t* path)
{
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
	if (!size)
		return 0;
	void* buf = malloc(size);
	if (!buf)
		return 0;
	unsigned long long v = 0;
	VS_FIXEDFILEINFO* ffi = nullptr;
	UINT len = 0;
	if (GetFileVersionInfoW(path, 0, size, buf)
		&& VerQueryValueW(buf, L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi)
	{
		v = (static_cast<unsigned long long>(ffi->dwFileVersionMS) << 32)
			| ffi->dwFileVersionLS;
	}
	free(buf);
	return v;
}

static const wchar_t* DlssNr_Leaf(const wchar_t* path)
{
	const wchar_t* leaf = wcsrchr(path, L'\\');
	return leaf ? leaf + 1 : path;
}

static void DlssNr_NoteDisk(const wchar_t* path, bool canon = false, bool game = false)
{
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return;
	if (s_ngxDiskCount >= 16)
		return;
	for (int i = 0; i < s_ngxDiskCount; ++i)
	{
		if (_wcsicmp(s_ngxDisk[i].path, path) == 0)
		{
			s_ngxDisk[i].canon = s_ngxDisk[i].canon || canon;
			s_ngxDisk[i].game = s_ngxDisk[i].game || game;
			return;
		}
	}
	NgxDisk& d = s_ngxDisk[s_ngxDiskCount++];
	wcsncpy_s(d.path, path, _TRUNCATE);
	d.ver = DlssNr_FileVer(path);
	d.stub = _wcsicmp(DlssNr_Leaf(path), L"nvngx.dll") == 0;
	d.canon = canon;
	d.game = game;
}

// The kernel driver publishes the directory of ITS matching NGX core here; the
// FileRepository glob can also surface leftovers from uninstalled drivers.
static void DlssNr_NoteRegistryCore(void)
{
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", 0, KEY_READ, &key) != ERROR_SUCCESS)
		return;
	wchar_t dir[MAX_PATH] = {};
	DWORD size = sizeof(dir) - sizeof(wchar_t);
	DWORD type = 0;
	const LSTATUS ls = RegQueryValueExW(key, L"NGXPath", nullptr, &type,
		reinterpret_cast<LPBYTE>(dir), &size);
	RegCloseKey(key);
	if (ls != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || !dir[0])
		return;
	DlssNr_Log("registry NGXPath %ls\n", dir);
	wchar_t path[MAX_PATH] = {};
	_snwprintf_s(path, _TRUNCATE, L"%s\\_nvngx.dll", dir);
	DlssNr_NoteDisk(path, true);
	_snwprintf_s(path, _TRUNCATE, L"%s\\nvngx.dll", dir);
	DlssNr_NoteDisk(path, true);
}

// nvngx.dll is a shim whose file version does not track the driver core, so a
// version test on it rejects the module Init needs mapped. Nothing is rejected.
static bool DlssNr_RejectDisk(const NgxDisk& d)
{
	NOTE_UNUSED(d);
	return false;
}

static bool DlssNr_PreferDisk(const NgxDisk& a, const NgxDisk& b)
{
	if (a.stub != b.stub)
		return !a.stub;
	if (DlssNr_NrDllPresent() && a.game != b.game)
		return a.game;
	if (a.canon != b.canon)
		return a.canon;
	return a.ver > b.ver;
}

static HMODULE DlssNr_TryLoadRuntime(const wchar_t* path)
{
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return nullptr;
	HMODULE mod = LoadLibraryW(path);
	return DlssNr_ModuleHasD3D12Eval(mod);
}

static void DlssNr_AddCand(HMODULE mod)
{
	if (!mod || s_ngxCandCount >= 8)
		return;
	for (int i = 0; i < s_ngxCandCount; ++i)
	{
		if (s_ngxCands[i] == mod)
			return;
	}
	s_ngxCands[s_ngxCandCount++] = mod;
}

static void DlssNr_ScanDriverStoreDisk(void)
{
	wchar_t sys[MAX_PATH] = {};
	GetSystemDirectoryW(sys, MAX_PATH);
	wchar_t glob[MAX_PATH] = {};
	_snwprintf_s(glob, _TRUNCATE, L"%s\\DriverStore\\FileRepository\\nv*", sys);

	WIN32_FIND_DATAW fd = {};
	HANDLE find = FindFirstFileW(glob, &fd);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
			continue;
		wchar_t path[MAX_PATH] = {};
		_snwprintf_s(path, _TRUNCATE, L"%s\\DriverStore\\FileRepository\\%s\\nvngx.dll",
			sys, fd.cFileName);
		DlssNr_NoteDisk(path);
		_snwprintf_s(path, _TRUNCATE, L"%s\\DriverStore\\FileRepository\\%s\\_nvngx.dll",
			sys, fd.cFileName);
		DlssNr_NoteDisk(path);
	} while (FindNextFileW(find, &fd));
	FindClose(find);
}

static void DlssNr_CollectRuntimes(void)
{
	s_ngxDiskCount = 0;
	s_ngxCandCount = 0;

	wchar_t sys[MAX_PATH] = {};
	GetSystemDirectoryW(sys, MAX_PATH);
	wchar_t exeDir[MAX_PATH] = {};
	DlssNr_ExeDirW(exeDir, MAX_PATH);

	DlssNr_NoteRegistryCore();

	wchar_t path[MAX_PATH] = {};
	_snwprintf_s(path, _TRUNCATE, L"%s\\nvngx.dll", sys);
	DlssNr_NoteDisk(path);
	_snwprintf_s(path, _TRUNCATE, L"%s\\_nvngx.dll", sys);
	DlssNr_NoteDisk(path);
	DlssNr_ScanDriverStoreDisk();
	_snwprintf_s(path, _TRUNCATE, L"%s\\nvngx.dll", exeDir);
	DlssNr_NoteDisk(path, false, true);
	_snwprintf_s(path, _TRUNCATE, L"%s\\_nvngx.dll", exeDir);
	DlssNr_NoteDisk(path, false, true);

	NgxDisk keep[16];
	int nKeep = 0;
	for (int i = 0; i < s_ngxDiskCount; ++i)
	{
		if (DlssNr_RejectDisk(s_ngxDisk[i]))
		{
			const DWORD ms = static_cast<DWORD>(s_ngxDisk[i].ver >> 32);
			const DWORD ls = static_cast<DWORD>(s_ngxDisk[i].ver);
			DlssNr_Log("skip %ls ver %u.%u.%u.%u (stale vs 32.x core)\n",
				s_ngxDisk[i].path,
				HIWORD(ms), LOWORD(ms), HIWORD(ls), LOWORD(ls));
			continue;
		}
		keep[nKeep++] = s_ngxDisk[i];
	}

	for (int i = 0; i < nKeep; ++i)
	{
		for (int j = i + 1; j < nKeep; ++j)
		{
			if (DlssNr_PreferDisk(keep[j], keep[i]))
			{
				NgxDisk t = keep[i];
				keep[i] = keep[j];
				keep[j] = t;
			}
		}
	}

	for (int i = 0; i < nKeep && s_ngxCandCount < 4; ++i)
		DlssNr_AddCand(DlssNr_TryLoadRuntime(keep[i].path));

	HMODULE pulled = GetModuleHandleW(L"nvngx.dll");
	if (pulled)
	{
		wchar_t pulledPath[MAX_PATH] = {};
		GetModuleFileNameW(pulled, pulledPath, MAX_PATH);
		DlssNr_Log("shim mapped %ls\n", pulledPath);
	}
	else
	{
		DlssNr_Log("WARN no nvngx.dll mapped -- Init has never opened a session without the shim\n");
	}
}

static void DlssNr_LogModuleVersion(HMODULE mod, const wchar_t* path)
{
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
	if (!size)
	{
		DlssNr_Log("  %ls (no version resource)\n", path);
		return;
	}
	void* buf = malloc(size);
	if (!buf)
		return;
	VS_FIXEDFILEINFO* ffi = nullptr;
	UINT len = 0;
	if (GetFileVersionInfoW(path, 0, size, buf)
		&& VerQueryValueW(buf, L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi)
	{
		DlssNr_Log("  %ls ver %u.%u.%u.%u initP=%p base=%p\n", path,
			HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
			HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS),
			GetProcAddress(mod, "NVSDK_NGX_D3D12_Init_Ext"), mod);
	}
	free(buf);
}

void DlssNr_DumpStatus(void)
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	DlssNr_Log("--- dump ---\n");
	DlssNr_Log("exe %ls\n", exe);
	DlssNr_Log("dx12=%d vendorKnown=%d nvidia=%d dll=%d session=%d feature=%d ssFeature=%d latched=%d running=%d ss=%d presents=%d\n",
		DirectX_IsDx12Mode() ? 1 : 0,
		s_vendorProbed ? 1 : 0,
		s_isNvidia ? 1 : 0,
		DlssNr_NrDllPresent() ? 1 : 0,
		(s_origEval && s_initedNgx) ? 1 : 0,
		s_nrHandle ? 1 : 0,
		s_ssHandle ? 1 : 0,
		s_createLatched ? 1 : 0,
		s_loggedFirstRun ? 1 : 0,
		s_ssEvals,
		s_presents);
	const char* srSkip = DlssSr_LastFail();
	DlssNr_Log("tsaaHook=%d tsaa=%llu srOk=%llu srFail=%llu srLive=%d srSkip=\"%s\"\n",
		DlssSr_TsaaHooked() ? 1 : 0,
		DlssSr_TsaaCount(),
		DlssSr_EvalOkCount(),
		DlssSr_EvalFailCount(),
		DlssSr_Live() ? 1 : 0,
		(srSkip && srSkip[0]) ? srSkip : "");
	DlssNr_Log("ss creates=%d destroys=%d evals=%d tries=%d latched=%d key=%ux%u->%ux%u flags=0x%X q=%d skippedViews=%llu\n",
		s_ssCreates, s_ssDestroys, s_ssEvals, s_ssCreateTries, s_ssCreateLatched ? 1 : 0,
		s_ssRenderW, s_ssRenderH, s_ssDisplayW, s_ssDisplayH, s_ssFlags, s_ssQuality,
		DlssSr_SkippedViews());
	DlssNr_Log("settings_dlssnr=%d present=%d initFailed=%d gaveUpInstall=%d lastInit=0x%08X %s\n",
		settings_dlssnr.GetInt(), settings_dlssnr_present.GetInt(), s_initFailed ? 1 : 0, s_gaveUpInstall ? 1 : 0,
		static_cast<unsigned>(s_lastInitResult), DlssNr_ResultName(s_lastInitResult));
	DlssNr_Log("nrProbe=0x%08X %s nrDllMod=%p app=0x%X project=%s\n",
		static_cast<unsigned>(s_nrProbeResult), DlssNr_ResultName(s_nrProbeResult), s_nrDllMod,
		static_cast<unsigned>(kNgxAppId), kNgxProjectId);
	DlssNr_Log("snippet ready=%d gatePatched=%d inited=%d init=0x%08X %s create=0x%08X %s nrHandle=%p\n",
		s_snipReady ? 1 : 0, s_snipGatePatched ? 1 : 0, s_snipInited ? 1 : 0,
		static_cast<unsigned>(s_snipInitResult), DlssNr_ResultName(s_snipInitResult),
		static_cast<unsigned>(s_snipCreateResult), DlssNr_ResultName(s_snipCreateResult),
		s_nrHandle);

	if (s_ngxMod)
	{
		wchar_t ngxPath[MAX_PATH] = {};
		GetModuleFileNameW(s_ngxMod, ngxPath, MAX_PATH);
		DlssNr_Log("ngx runtime %ls initP=%p shutdown=%p\n", ngxPath, s_origInitProject, s_origShutdown);
	}

	wchar_t dir[MAX_PATH] = {};
	DlssNr_ExeDirW(dir, MAX_PATH);
	wchar_t nrPath[MAX_PATH] = {};
	_snwprintf_s(nrPath, _TRUNCATE, L"%s\\nvngx_dlssnr.dll", dir);
	DlssNr_Log("nr dll path %ls attr=%d\n", nrPath,
		GetFileAttributesW(nrPath) != INVALID_FILE_ATTRIBUTES ? 1 : 0);

	static const wchar_t* kNames[] = {
		L"nvngx.dll", L"_nvngx.dll", L"nvngx_dlss.dll", L"nvngx_dlssnr.dll",
		L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", L"sl.dlss_nr.dll"
	};
	for (int i = 0; i < 8; ++i)
	{
		HMODULE m = GetModuleHandleW(kNames[i]);
		DlssNr_Log("GetModuleHandle %ls -> %p\n", kNames[i], m);
		if (!m)
			continue;
		DlssNr_LogExport(m, "NVSDK_NGX_D3D12_EvaluateFeature");
		DlssNr_LogExport(m, "NVSDK_NGX_D3D12_EvaluateFeature_C");
		DlssNr_LogExport(m, "NVSDK_NGX_D3D12_CreateFeature");
		DlssNr_LogExport(m, "NVSDK_NGX_D3D12_Init");
		DlssNr_LogExport(m, "NVSDK_NGX_D3D12_Init_Ext");
		DlssNr_LogExport(m, "NVSDK_NGX_D3D12_Init_ProjectID");
		DlssNr_LogExport(m, "NVSDK_NGX_D3D12_Init_with_ProjectID");
		DlssNr_LogExport(m, "NVSDK_NGX_D3D11_EvaluateFeature");
		DlssNr_LogExport(m, "NVSDK_NGX_D3D11_EvaluateFeature_C");
		DlssNr_LogExport(m, "slEvaluateFeature");
	}

	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	typedef BOOL(WINAPI* PFN_Enum)(HANDLE, HMODULE*, DWORD, LPDWORD);
	auto enumMods = k32 ? reinterpret_cast<PFN_Enum>(GetProcAddress(k32, "K32EnumProcessModules")) : nullptr;
	if (!enumMods)
	{
		HMODULE psapi = LoadLibraryW(L"psapi.dll");
		if (psapi)
			enumMods = reinterpret_cast<PFN_Enum>(GetProcAddress(psapi, "EnumProcessModules"));
	}
	if (!enumMods)
	{
		DlssNr_Log("EnumProcessModules unavailable\n");
		return;
	}

	HMODULE mods[1024];
	DWORD needed = 0;
	const BOOL ok = enumMods(GetCurrentProcess(), mods, sizeof(mods), &needed);
	DlssNr_Log("enum ok=%d needed=%u lastError=%u\n", ok ? 1 : 0, needed, GetLastError());
	const DWORD n = needed / sizeof(HMODULE);
	const DWORD cap = n < 1024 ? n : 1024;
	int interesting = 0;
	for (DWORD i = 0; i < cap; ++i)
	{
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(mods[i], path, MAX_PATH);
		const wchar_t* leaf = wcsrchr(path, L'\\');
		leaf = leaf ? leaf + 1 : path;
		const bool ngx = _wcsnicmp(leaf, L"nvngx", 5) == 0
			|| _wcsnicmp(leaf, L"sl.", 3) == 0
			|| wcsstr(leaf, L"ngx") != nullptr;
		const bool exp = GetProcAddress(mods[i], "NVSDK_NGX_D3D12_EvaluateFeature")
			|| GetProcAddress(mods[i], "NVSDK_NGX_D3D12_EvaluateFeature_C")
			|| GetProcAddress(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature")
			|| GetProcAddress(mods[i], "slEvaluateFeature");
		if (!ngx && !exp)
			continue;
		++interesting;
		DlssNr_Log("mod %ls ngx=%d exp=%d\n", path, ngx ? 1 : 0, exp ? 1 : 0);
		DlssNr_LogExport(mods[i], "NVSDK_NGX_D3D12_EvaluateFeature");
		DlssNr_LogExport(mods[i], "NVSDK_NGX_D3D12_EvaluateFeature_C");
		DlssNr_LogExport(mods[i], "NVSDK_NGX_D3D12_CreateFeature");
		DlssNr_LogExport(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature");
		DlssNr_LogExport(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature_C");
		DlssNr_LogExport(mods[i], "slEvaluateFeature");
	}
	DlssNr_Log("interesting modules %d  FindNgx=%p\n", interesting, DlssNr_FindNgx());
	DlssNr_Log("--- dump end ---\n");
}

// The 310.8 nvngx_dlssnr.dll is a feature snippet, not the NGX runtime. Every
// D3D12 entry opens with GetModuleHandleExA(FROM_ADDRESS,retaddr) ->
// GetModuleFileNameW -> wcsstr(name,"nvngx.dll"), returning 0xBAD00002 when the
// caller is not the runtime. The 616.56 core (1.4.0.0) never routes feature 18
// to this snippet (CreateFeature(18)=0xBAD0000B), so the feature is driven off
// the snippet's own exports. The origin check reads the file name of OUR
// module, so the snippet's imported GetModuleFileNameW is redirected to report
// an nvngx.dll path for our handle alone; every other query passes through.
static const wchar_t kSnipFakeCaller[] = L"nvngx.dll";

static DWORD WINAPI DlssNr_FakeGmfnw(HMODULE mod, LPWSTR buf, DWORD cap)
{
	if (mod == s_selfModule && buf && cap)
	{
		DWORD n = static_cast<DWORD>(wcslen(kSnipFakeCaller));
		if (n >= cap)
			n = cap - 1;
		wmemcpy(buf, kSnipFakeCaller, n);
		buf[n] = L'\0';
		return n;
	}
	return s_realGmfnw ? s_realGmfnw(mod, buf, cap) : 0;
}

static bool DlssNr_PatchSnippetGate(HMODULE snip)
{
	if (!snip)
		return false;
	BYTE* base = reinterpret_cast<BYTE*>(snip);
	const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress)
		return false;

	for (const IMAGE_IMPORT_DESCRIPTOR* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
		desc->Name; ++desc)
	{
		const IMAGE_THUNK_DATA* oft = reinterpret_cast<const IMAGE_THUNK_DATA*>(
			base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
		IMAGE_THUNK_DATA* ft = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
		for (; oft->u1.AddressOfData; ++oft, ++ft)
		{
			if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG)
				continue;
			const IMAGE_IMPORT_BY_NAME* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + oft->u1.AddressOfData);
			if (strcmp(reinterpret_cast<const char*>(ibn->Name), "GetModuleFileNameW") != 0)
				continue;
			DWORD prot = 0;
			if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &prot))
				return false;
			ft->u1.Function = reinterpret_cast<ULONGLONG>(&DlssNr_FakeGmfnw);
			VirtualProtect(&ft->u1.Function, sizeof(void*), prot, &prot);
			return true;
		}
	}
	return false;
}

static bool DlssNr_EnsureParams(void);

static bool DlssNr_BindSnippet(void)
{
	if (s_snipReady)
		return true;
	if (!s_nrDllMod)
		DlssNr_PreloadNrModel();
	HMODULE m = s_nrDllMod;
	if (!m)
		return false;

	s_snipInit = reinterpret_cast<PFN_SnipInitExt>(GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext"));
	s_snipCreate = reinterpret_cast<PFN_SnipCreate>(GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature"));
	void* ev = GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature");
	s_snipEval = reinterpret_cast<PFN_NgxEval>(ev);
	s_snipRelease = reinterpret_cast<PFN_SnipRelease>(GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature"));
	s_snipShutdown = reinterpret_cast<PFN_SnipShutdown1>(GetProcAddress(m, "NVSDK_NGX_D3D12_Shutdown1"));
	if (!s_snipInit || !s_snipCreate || !s_snipEval || !s_snipRelease)
	{
		DlssNr_Log("snippet exports missing init=%p create=%p eval=%p release=%p\n",
			s_snipInit, s_snipCreate, s_snipEval, s_snipRelease);
		return false;
	}

	if (!s_snipGatePatched)
	{
		if (!s_selfModule)
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&DlssNr_FakeGmfnw), &s_selfModule);
		HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
		s_realGmfnw = reinterpret_cast<PFN_GetModuleFileNameW>(GetProcAddress(k32, "GetModuleFileNameW"));
		s_snipGatePatched = s_realGmfnw && s_selfModule && DlssNr_PatchSnippetGate(m);
		DlssNr_Log("snippet origin-gate patch %s (self=%p realGmfnw=%p)\n",
			s_snipGatePatched ? "ok" : "FAILED", s_selfModule, s_realGmfnw);
		if (!s_snipGatePatched)
			return false;
	}

	s_snipMod = m;
	s_snipReady = true;
	DlssNr_Log("snippet host bound init=%p create=%p eval=%p release=%p shutdown1=%p\n",
		s_snipInit, s_snipCreate, s_snipEval, s_snipRelease, s_snipShutdown);
	return true;
}

static bool DlssNr_SnippetInit(ID3D12Device* dev)
{
	if (s_snipInited)
		return true;
	if (!dev || !DlssNr_BindSnippet())
		return false;
	if (!DlssNr_EnsureParams())
		return false;

	wchar_t dir[MAX_PATH] = {};
	DlssNr_ExeDirW(dir, MAX_PATH);

	DlssNr_ApplyNgxLogEnv();

	// The fifth argument is InFeatureInfo, not a parameter block. LoggingInfo
	// here is inert -- this snippet discards it -- so the callback is set only
	// to keep the struct honest; the live channel is __NGX_LOG_LEVEL.
	const wchar_t* paths[1] = { dir };
	NVSDK_NGX_FeatureCommonInfo info = {};
	info.PathListInfo.Path = paths;
	info.PathListInfo.Length = 1;
	info.LoggingInfo.LoggingCallback = reinterpret_cast<void*>(&DlssNr_NgxLog);
	info.LoggingInfo.MinimumLoggingLevel = DlssNr_NgxLogLevel();
	info.LoggingInfo.DisableOtherLoggingSinks = !DlssNr_Diag();

	NVSDK_NGX_Result r = kNgxFailUnableToInit;
	__try
	{
		r = s_snipInit(kNgxAppId, dir, dev, static_cast<unsigned int>(kNgxSdkVersion), &info);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		r = kNgxFailUnableToInit;
	}
	s_snipInitResult = r;
	DlssNr_FileLog("[DLSSNR] snippet Init_Ext -> 0x%08X %s\n", static_cast<unsigned>(r), DlssNr_ResultName(r));
	if (r == kNgxSuccess || r == kNgxFailAlreadyExists)
	{
		s_snipInited = true;
		return true;
	}
	return false;
}

// Releasing a feature frees the GPU memory its Evaluate still references. Our
// Evaluate rides the engine's command list, which we cannot fence, so a handle
// is parked for a few presents before the snippet frees it.
static constexpr int kNrRetireSlots = 4;
static constexpr uint64_t kNrRetireFrames = 8;
static NVSDK_NGX_Handle* s_nrRetireHandle[kNrRetireSlots] = {};
static uint64_t s_nrRetireFrame[kNrRetireSlots] = {};

static void DlssNr_DrainRetiredFeatures(bool force)
{
	for (int i = 0; i < kNrRetireSlots; ++i)
	{
		if (!s_nrRetireHandle[i])
			continue;
		if (!force && (s_presentFrameId - s_nrRetireFrame[i]) < kNrRetireFrames)
			continue;
		if (s_snipRelease)
			s_snipRelease(s_nrRetireHandle[i]);
		s_nrRetireHandle[i] = nullptr;
	}
}

static void DlssNr_ReleaseFeature(const char* why)
{
	if (s_nrHandle)
	{
		DlssNr_FileLog("[DLSSNR] release (%s) handle=%p after %d evals\n",
			why, s_nrHandle, s_nrEvals);
		int slot = -1;
		for (int i = 0; i < kNrRetireSlots; ++i)
		{
			if (!s_nrRetireHandle[i])
			{
				slot = i;
				break;
			}
		}
		if (slot < 0)
		{
			// Every slot is occupied; the oldest has waited longest.
			DlssNr_DrainRetiredFeatures(true);
			slot = 0;
		}
		s_nrRetireHandle[slot] = s_nrHandle;
		s_nrRetireFrame[slot] = s_presentFrameId;
	}
	s_nrHandle = nullptr;
	s_nrW = 0;
	s_nrH = 0;
	s_nrFmt = DXGI_FORMAT_UNKNOWN;
}

static void DlssNr_ReleaseSuperSamplingHandle(void)
{
	if (s_ssHandle && s_origRelease)
	{
		s_origRelease(s_ssHandle);
		++s_ssDestroys;
		DlssNr_SsFileLog("destroy", s_ssRenderW, s_ssRenderH, s_ssDisplayW, s_ssDisplayH, s_ssQuality, s_ssFlags);
	}
	s_ssHandle = nullptr;
	s_ssRenderW = 0;
	s_ssRenderH = 0;
	s_ssDisplayW = 0;
	s_ssDisplayH = 0;
	s_ssFlags = 0;
	s_ssQuality = -1;
	s_ssEvalsSinceCreate = 0;
	s_ssReuseLogged = false;
}

void DlssNr_ReleaseSuperSampling(void)
{
	DlssNr_ReleaseSuperSamplingHandle();
}

static bool DlssNr_EnsureParams(void)
{
	if (s_nrParams)
		return true;
	if (s_origAllocParams && s_origAllocParams(&s_nrParams) == kNgxSuccess && s_nrParams)
	{
		s_nrParamsOwned = true;
		return true;
	}
	if (s_origGetCaps && s_origGetCaps(&s_nrParams) == kNgxSuccess && s_nrParams)
	{
		s_nrParamsOwned = false;
		return true;
	}
	return false;
}

static bool DlssNr_EnsureSsParams(void)
{
	if (s_ssParams)
		return true;
	if (s_origAllocParams && s_origAllocParams(&s_ssParams) == kNgxSuccess && s_ssParams)
	{
		s_ssParamsOwned = true;
		return true;
	}
	if (s_origGetCaps && s_origGetCaps(&s_ssParams) == kNgxSuccess && s_ssParams)
	{
		s_ssParamsOwned = false;
		return true;
	}
	return false;
}

bool DlssNr_EnsureNgxSession(ID3D12Device* device)
{
	DlssNr_EnsureCs();
	if (s_initedNgx && s_origEval && s_origCreate)
		return true;
	if (!device)
		return false;
	DlssNr_ProbeVendorFromDevice(device);
	if (!s_origEval || !s_origCreate)
	{
		if (!DlssNr_TryInstallHooks())
			return false;
	}
	return DlssNr_TryInit(device);
}

int DlssNr_CreateSuperSampling(void* cmdlist, unsigned renderW, unsigned renderH,
	unsigned displayW, unsigned displayH, unsigned flags, int quality)
{
	if (s_ssCreateLatched)
		return kNgxFailNotSupported;
	if (!cmdlist || !s_origCreate || !s_initedNgx)
		return kNgxFailNotInitialized;
	if (renderW == 0 || renderH == 0 || displayW == 0 || displayH == 0)
		return kNgxFailInvalidParameter;

	if (s_ssHandle && s_ssRenderW == renderW && s_ssRenderH == renderH
		&& s_ssDisplayW == displayW && s_ssDisplayH == displayH
		&& s_ssFlags == flags && s_ssQuality == quality)
	{
		if (!s_ssReuseLogged)
		{
			s_ssReuseLogged = true;
			DlssNr_SsFileLog("reuse", renderW, renderH, displayW, displayH, quality, flags);
		}
		return kNgxSuccess;
	}

	if (s_ssHandle)
	{
		// A foreign key never destroys the live feature; the caller releases
		// first when the main view itself changed size.
		static UINT s_misW = 0, s_misH = 0, s_misOutW = 0, s_misOutH = 0;
		if (renderW != s_misW || renderH != s_misH || displayW != s_misOutW || displayH != s_misOutH)
		{
			s_misW = renderW;
			s_misH = renderH;
			s_misOutW = displayW;
			s_misOutH = displayH;
			DlssNr_FileLog("[DLSS-SR] ss key in=%ux%u out=%ux%u q=%d flags=0x%X action=skip-mismatch held=%ux%u->%ux%u flags=0x%X q=%d creates=%d destroys=%d evals=%d\n",
				renderW, renderH, displayW, displayH, quality, flags,
				s_ssRenderW, s_ssRenderH, s_ssDisplayW, s_ssDisplayH, s_ssFlags, s_ssQuality,
				s_ssCreates, s_ssDestroys, s_ssEvals);
		}
		return kNgxFailAlreadyExists;
	}

	if (!DlssNr_EnsureSsParams())
		return kNgxFailUnableToInit;

	s_ssParams->Reset();
	s_ssParams->Set("Width", renderW);
	s_ssParams->Set("Height", renderH);
	s_ssParams->Set("OutWidth", displayW);
	s_ssParams->Set("OutHeight", displayH);
	s_ssParams->Set("PerfQualityValue", quality);
	s_ssParams->Set("DLSS.Feature.Create.Flags", static_cast<int>(flags));
	s_ssParams->Set("CreationNodeMask", 1u);
	s_ssParams->Set("VisibilityNodeMask", 1u);

	ID3D12Device* dev = nullptr;
	if (s_initDevice)
		dev = s_initDevice;
	DlssNr_EnsureScratch(kNgxFeatureSuperSampling, s_ssParams, dev);
	if (s_scratch)
		s_ssParams->Set("Scratch", static_cast<void*>(s_scratch));

	NVSDK_NGX_Handle* handle = nullptr;
	NVSDK_NGX_Result r = kNgxFailUnableToInit;
	__try
	{
		r = s_origCreate(cmdlist, kNgxFeatureSuperSampling, s_ssParams, &handle);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::MS, "[DLSS-SR] CreateFeature 1 faulted -- latched off\n");
		DlssNr_SsFileLog("create-fail:seh-fault", renderW, renderH, displayW, displayH, quality, flags);
		s_ssCreateLatched = true;
		return kNgxFailUnableToInit;
	}

	if (r != kNgxSuccess || !handle)
	{
		if (r == kNgxSuccess)
			r = kNgxFailUnableToInit;
		char action[48];
		_snprintf_s(action, _TRUNCATE, "create-fail:0x%08X", static_cast<unsigned>(r));
		DlssNr_SsFileLog(action, renderW, renderH, displayW, displayH, quality, flags);
		Warning(eDLL_T::MS, "[DLSS-SR] CreateFeature 1 -> 0x%08X %s %ux%u -> %ux%u flags=0x%X q=%d\n",
			r, DlssNr_ResultName(r), renderW, renderH, displayW, displayH, flags, quality);
		if (r == kNgxFailNotSupported || r == kNgxFailDenied)
			s_ssCreateLatched = true;
		else if (++s_ssCreateTries >= 8)
		{
			Warning(eDLL_T::MS, "[DLSS-SR] CreateFeature 1 failed %d times -- latched off\n", s_ssCreateTries);
			DlssNr_FileLog("[DLSS-SR] CreateFeature 1 failed %d times -- latched off\n", s_ssCreateTries);
			s_ssCreateLatched = true;
		}
		return r;
	}

	s_ssHandle = handle;
	s_ssRenderW = renderW;
	s_ssRenderH = renderH;
	s_ssDisplayW = displayW;
	s_ssDisplayH = displayH;
	s_ssFlags = flags;
	s_ssQuality = quality;
	s_ssCreateTries = 0;
	s_ssEvalsSinceCreate = 0;
	s_ssReuseLogged = false;
	++s_ssCreates;
	DlssNr_Remember(handle, kNgxFeatureSuperSampling);
	DlssNr_SsFileLog("create", renderW, renderH, displayW, displayH, quality, flags);
	Msg(eDLL_T::MS, "[DLSS-SR] SuperSampling created %ux%u -> %ux%u flags=0x%X q=%d\n",
		renderW, renderH, displayW, displayH, flags, quality);
	return kNgxSuccess;
}

int DlssNr_EvaluateSuperSampling(void* cmdlist, ID3D12Resource* color, ID3D12Resource* depth,
	ID3D12Resource* motionVectors, ID3D12Resource* output,
	float jitterX, float jitterY, float mvScaleX, float mvScaleY,
	unsigned renderW, unsigned renderH, bool reset)
{
	DlssNr_EnsureCs();
	if (!cmdlist || !s_origEval || !s_ssHandle || !s_ssParams)
		return kNgxFailNotInitialized;
	if (!color || !output)
		return kNgxFailMissingInput;

	s_ssParams->Reset();
	s_ssParams->Set("Color", color);
	s_ssParams->Set("Output", output);
	if (depth)
		s_ssParams->Set("Depth", depth);
	if (motionVectors)
		s_ssParams->Set("MotionVectors", motionVectors);
	s_ssParams->Set("Jitter.Offset.X", jitterX);
	s_ssParams->Set("Jitter.Offset.Y", jitterY);
	s_ssParams->Set("MV.Scale.X", mvScaleX);
	s_ssParams->Set("MV.Scale.Y", mvScaleY);
	s_ssParams->Set("DLSS.Render.Subrect.Dimensions.Width", renderW);
	s_ssParams->Set("DLSS.Render.Subrect.Dimensions.Height", renderH);
	s_ssParams->Set("Reset", reset ? 1 : 0);
	s_ssParams->Set("Sharpness", 0.0f);
	s_ssParams->Set("Exposure.Scale", 1.0f);
	s_ssParams->Set("DLSS.Pre.Exposure", 1.0f);
	if (s_scratch)
		s_ssParams->Set("Scratch", static_cast<void*>(s_scratch));

	NVSDK_NGX_Result r = kNgxFailUnableToInit;
	__try
	{
		r = s_origEval(cmdlist, s_ssHandle, s_ssParams, nullptr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::MS, "[DLSS-SR] EvaluateFeature 1 faulted -- latched off\n");
		s_ssCreateLatched = true;
		return kNgxFailUnableToInit;
	}

	if (r == kNgxSuccess)
	{
		++s_ssEvals;
		++s_ssEvalsSinceCreate;
	}
	else
		Warning(eDLL_T::MS, "[DLSS-SR] EvaluateFeature 1 -> 0x%08X %s\n", r, DlssNr_ResultName(r));
	return r;
}

static void DlssNr_FillNrParams(NVSDK_NGX_Parameter* dst, const NVSDK_NGX_Parameter* src,
	ID3D12Resource* color, ID3D12Resource* output, ID3D12Resource* depth, ID3D12Resource* mv,
	UINT w, UINT h, float mvScaleX, float mvScaleY)
{
	dst->Reset();
	dst->Set("CreationNodeMask", 1u);
	dst->Set("VisibilityNodeMask", 1u);
	dst->Set("DLSSNR.Width", w);
	dst->Set("DLSSNR.Height", h);
	dst->Set("DLSSNR.ScalingRatio", 1.0f);
	dst->Set("DLSSNR.Color", color);
	dst->Set("DLSSNR.ColorSubrectBaseX", 0u);
	dst->Set("DLSSNR.ColorSubrectBaseY", 0u);
	dst->Set("DLSSNR.ColorSubrectWidth", w);
	dst->Set("DLSSNR.ColorSubrectHeight", h);
	dst->Set("DLSSNR.Output", output);
	dst->Set("DLSSNR.OutputSubrectBaseX", 0u);
	dst->Set("DLSSNR.OutputSubrectBaseY", 0u);
	dst->Set("DLSSNR.OutputSubrectWidth", w);
	dst->Set("DLSSNR.OutputSubrectHeight", h);
	if (depth)
	{
		dst->Set("DLSSNR.Depth", depth);
		dst->Set("DLSSNR.DepthInverted", 1u);
		dst->Set("DLSSNR.DepthSubrectBaseX", 0u);
		dst->Set("DLSSNR.DepthSubrectBaseY", 0u);
		dst->Set("DLSSNR.DepthSubrectWidth", w);
		dst->Set("DLSSNR.DepthSubrectHeight", h);
	}
	if (mv)
	{
		dst->Set("DLSSNR.MVec", mv);
		dst->Set("DLSSNR.MVecScaleX", mvScaleX);
		dst->Set("DLSSNR.MVecScaleY", mvScaleY);
		dst->Set("DLSSNR.MVecSubrectBaseX", 0u);
		dst->Set("DLSSNR.MVecSubrectBaseY", 0u);
		dst->Set("DLSSNR.MVecSubrectWidth", w);
		dst->Set("DLSSNR.MVecSubrectHeight", h);
	}
	float intensity = settings_dlssnr_intensity.GetFloat();
	if (intensity < 0.0f)
		intensity = 0.0f;
	else if (intensity >= 1.0f)
		intensity = 0.999f;
	dst->Set("DLSSNR.Enabled", sdk_dlssnr_bypass.GetInt() ? 0u : 1u);
	dst->Set("DLSSNR.Intensity", intensity);
	dst->Set("DLSSNR.LocalToneStrength", settings_dlssnr_localtone.GetFloat());
	dst->Set("DLSSNR.LocalStructureStrength", settings_dlssnr_localstructure.GetFloat());
	dst->Set("DLSSNR.UseAutoMask", settings_dlssnr_automask.GetInt() ? 1u : 0u);
	dst->Set("DLSSNR.SkinStructureStrength", settings_dlssnr_skinstructure.GetFloat());
	dst->Set("DLSSNR.Style", static_cast<unsigned int>(settings_dlssnr_style.GetInt()));
	dst->Set("DLSSNR.Hint.Render.Preset", static_cast<unsigned int>(settings_dlssnr_preset.GetInt()));
	if (src)
	{
		DlssNr_CopyUInt(src, dst, "CreationNodeMask");
		DlssNr_CopyUInt(src, dst, "VisibilityNodeMask");
	}
}

static bool DlssNr_EnsureScratchBuf(int featureId, NVSDK_NGX_Parameter* p, ID3D12Device* dev,
	ID3D12Resource*& buf, unsigned long long& cap)
{
	if (!s_origScratch || !dev)
		return true;
	unsigned long long bytes = 0;
	if (s_origScratch(featureId, p, &bytes) != kNgxSuccess || bytes == 0)
		return true;
	if (buf && cap >= bytes)
		return true;
	Nr_Release(buf);
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	d.Width = bytes;
	d.Height = 1;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&buf))))
	{
		Warning(eDLL_T::MS, "[DLSSNR] scratch alloc failed (%llu bytes)\n", bytes);
		return false;
	}
	cap = bytes;
	return true;
}

static bool DlssNr_EnsureScratch(int featureId, NVSDK_NGX_Parameter* p, ID3D12Device* dev)
{
	return DlssNr_EnsureScratchBuf(featureId, p, dev, s_scratch, s_scratchBytes);
}

static bool DlssNr_CreateNr(void* cmdlist, NVSDK_NGX_Parameter* p, ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt)
{
	if (s_createLatched)
		return false;
	if (!DlssNr_SnippetInit(dev) || !s_snipCreate)
		return false;
	if (s_nrHandle && s_nrW == w && s_nrH == h && s_nrFmt == fmt)
		return true;

	if (s_nrHandle)
		DlssNr_FileLog("[DLSSNR] recreate %ux%u fmt=%u -> %ux%u fmt=%u handle=%p\n",
			s_nrW, s_nrH, static_cast<unsigned>(s_nrFmt), w, h, static_cast<unsigned>(fmt), s_nrHandle);
	else
		DlssNr_FileLog("[DLSSNR] create (no live handle) %ux%u fmt=%u\n", w, h, static_cast<unsigned>(fmt));

	DlssNr_ReleaseFeature("size or format change");

	NVSDK_NGX_Handle* handle = nullptr;
	NVSDK_NGX_Result r = kNgxFailUnableToInit;
	__try
	{
		// Feature id is implicit in the dlssnr snippet; the command list gives
		// it the device, params carry Width/Height/Color/Output/DLSSNR.*.
		r = s_snipCreate(cmdlist, nullptr, p, &handle);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::MS, "[DLSSNR] snippet CreateFeature 18 faulted -- latched off\n");
		s_createLatched = true;
		return false;
	}

	s_lastNrResult = r;
	s_snipCreateResult = r;
	DlssNr_FileLog("[DLSSNR] snippet CreateFeature 18 -> 0x%08X %s handle=%p %ux%u fmt=%u\n",
		static_cast<unsigned>(r), DlssNr_ResultName(r), handle, w, h, fmt);
	if (DlssNr_Diag() || r != kNgxSuccess)
		Warning(eDLL_T::MS, "[DLSSNR] snippet CreateFeature 18 -> 0x%08X %s handle=%p %ux%u\n",
			r, DlssNr_ResultName(r), handle, w, h);

	if (r == kNgxFailNotSupported || r == kNgxFailDenied)
	{
		Warning(eDLL_T::MS, "[DLSSNR] snippet refused feature 18 (%s) -- driver/GPU lacks DLSS-NR support\n",
			DlssNr_ResultName(r));
		s_createLatched = true;
		return false;
	}
	if (r != kNgxSuccess || !handle)
	{
		if (++s_createTries >= 8)
		{
			Warning(eDLL_T::MS, "[DLSSNR] snippet CreateFeature 18 failed %d times -- latched off\n", s_createTries);
			s_createLatched = true;
		}
		return false;
	}

	for (int i = 0; i < kNrRetireSlots; ++i)
	{
		if (s_nrRetireHandle[i] != handle)
			continue;
		DlssNr_FileLog("[DLSSNR] retire slot %d reissued as the live handle %p -- dropped\n", i, handle);
		s_nrRetireHandle[i] = nullptr;
	}

	s_nrHandle = handle;
	s_nrW = w;
	s_nrH = h;
	s_nrFmt = fmt;
	s_createTries = 0;
	s_needReset = true;
	DlssNr_Remember(handle, kNgxFeatureNeuralRendering);
	Msg(eDLL_T::MS, "[DLSSNR] feature 18 created %ux%u fmt=%u\n", w, h, fmt);
	return true;
}

static NVSDK_NGX_Result DlssNr_EvalNr(void* cmdlist, NVSDK_NGX_Parameter* p)
{
	if (!s_snipEval || !s_nrHandle)
		return kNgxFailUnableToInit;
	NVSDK_NGX_Result r = kNgxFailUnableToInit;
	__try
	{
		r = s_snipEval(cmdlist, s_nrHandle, p, nullptr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::MS, "[DLSSNR] EvaluateFeature 18 faulted -- latched off\n");
		s_createLatched = true;
		s_lastNrResult = kNgxFailUnableToInit;
		return kNgxFailUnableToInit;
	}
	s_lastNrResult = r;
	return r;
}

bool DlssNr_NrReplaceEnabled(void)
{
	if (settings_dlssnr.GetInt() == 0 || s_createLatched)
		return false;
	if (!DirectX_IsDx12Mode() || !DlssNr_NrDllPresent())
		return false;
	return !s_vendorProbed || s_isNvidia;
}

// Feature-18 path. Color is _rt_FullFrameFB (pre-resolve). Depth/MV are
// optional; the snippet only requires Color+Output.
int DlssNr_ReplaceTsaa(void* cmdlist, ID3D12Resource* color, ID3D12Resource* output,
	ID3D12Resource* depth, ID3D12Resource* mv, unsigned w, unsigned h, bool reset,
	float mvScaleX, float mvScaleY)
{
	DlssNr_EnsureCs();
	if (!cmdlist || !color || !output)
		return kNgxFailMissingInput;
	if (settings_dlssnr.GetInt() == 0 || s_createLatched)
		return kNgxFailNotSupported;

	ID3D12Device* dev = nullptr;
	if (FAILED(output->GetDevice(IID_PPV_ARGS(&dev))) || !dev)
		return kNgxFailNotInitialized;

	if (!DlssNr_EnsureParams())
	{
		dev->Release();
		return kNgxFailUnableToInit;
	}

	const DXGI_FORMAT fmt = output->GetDesc().Format;

	EnterCriticalSection(&s_cs);
	DlssNr_FillNrParams(s_nrParams, nullptr, color, output, depth, mv, w, h, mvScaleX, mvScaleY);
	s_nrParams->Set("DLSSNR.Reset", (reset || s_needReset) ? 1u : 0u);

	if (!DlssNr_CreateNr(cmdlist, s_nrParams, dev, w, h, fmt))
	{
		const int fail = s_lastNrResult ? s_lastNrResult : kNgxFailUnableToInit;
		LeaveCriticalSection(&s_cs);
		dev->Release();
		return fail;
	}

	const NVSDK_NGX_Result r = DlssNr_EvalNr(cmdlist, s_nrParams);
	LeaveCriticalSection(&s_cs);
	if (r == kNgxSuccess)
	{
		s_needReset = false;
		++s_nrEvals;
		if (!s_loggedFirstRun)
		{
			s_loggedFirstRun = true;
			Msg(eDLL_T::MS, "[DLSSNR] running (resolve path) %ux%u intensity=%.2f style=%d preset=%d\n",
				w, h, settings_dlssnr_intensity.GetFloat(),
				settings_dlssnr_style.GetInt(), settings_dlssnr_preset.GetInt());
			DlssNr_FileLog("[DLSSNR] engine-list evaluate ok %ux%u fmt=%u\n", w, h, fmt);
		}
	}
	else if (DlssNr_Diag() || r != kNgxFailInvalidParameter)
		Warning(eDLL_T::MS, "[DLSSNR] resolve EvaluateFeature 18 -> 0x%08X %s\n", r, DlssNr_ResultName(r));
	dev->Release();
	return r;
}

static bool DlssNr_RunOnList(void* cmdlist, const NVSDK_NGX_Parameter* src,
	ID3D12Resource* origOut, ID3D12Resource* origDepth, ID3D12Resource* origMv,
	D3D12_RESOURCE_STATES origOutState)
{
	if (!cmdlist || !origOut)
		return false;

	auto* list = static_cast<ID3D12GraphicsCommandList*>(cmdlist);
	ID3D12Device* dev = nullptr;
	if (FAILED(origOut->GetDevice(IID_PPV_ARGS(&dev))) || !dev)
		return false;

	const D3D12_RESOURCE_DESC od = origOut->GetDesc();
	const UINT w = static_cast<UINT>(od.Width);
	const UINT h = od.Height;
	if (w == 0 || h == 0)
	{
		dev->Release();
		return false;
	}

	if (!DlssNr_EnsureSub(s_subColor, dev, od, "Color") || !DlssNr_EnsureSub(s_subOut, dev, od, "Output"))
	{
		dev->Release();
		return false;
	}

	ID3D12Resource* depth = origDepth;
	if (origDepth)
	{
		const D3D12_RESOURCE_DESC dd = origDepth->GetDesc();
		if (dd.MipLevels > 1)
		{
			if (DlssNr_EnsureSub(s_subDepth, dev, dd, "Depth"))
			{
				DlssNr_Transition(list, origDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
				DlssNr_ToState(list, s_subDepth, D3D12_RESOURCE_STATE_COPY_DEST);
				DlssNr_CopyMip0(list, s_subDepth.tex, origDepth);
				DlssNr_ToState(list, s_subDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				DlssNr_Transition(list, origDepth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				depth = s_subDepth.tex;
			}
		}
	}

	ID3D12Resource* mv = origMv;
	if (origMv)
	{
		const D3D12_RESOURCE_DESC md = origMv->GetDesc();
		if (md.MipLevels > 1)
		{
			if (DlssNr_EnsureSub(s_subMv, dev, md, "MV"))
			{
				DlssNr_Transition(list, origMv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
				DlssNr_ToState(list, s_subMv, D3D12_RESOURCE_STATE_COPY_DEST);
				DlssNr_CopyMip0(list, s_subMv.tex, origMv);
				DlssNr_ToState(list, s_subMv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				DlssNr_Transition(list, origMv, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				mv = s_subMv.tex;
			}
		}
	}

	DlssNr_Transition(list, origOut, origOutState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	DlssNr_ToState(list, s_subColor, D3D12_RESOURCE_STATE_COPY_DEST);
	DlssNr_CopyMip0(list, s_subColor.tex, origOut);
	DlssNr_ToState(list, s_subColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	DlssNr_ToState(list, s_subOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	if (!DlssNr_EnsureParams())
	{
		DlssNr_Transition(list, origOut, D3D12_RESOURCE_STATE_COPY_SOURCE, origOutState);
		dev->Release();
		return false;
	}

	DlssNr_FillNrParams(s_nrParams, src, s_subColor.tex, s_subOut.tex, depth, mv,
		w, h, static_cast<float>(w), static_cast<float>(h));
	s_nrParams->Set("DLSSNR.Reset", s_needReset ? 1u : 0u);

	if (!DlssNr_CreateNr(cmdlist, s_nrParams, dev, w, h, od.Format))
	{
		DlssNr_Transition(list, origOut, D3D12_RESOURCE_STATE_COPY_SOURCE, origOutState);
		dev->Release();
		return false;
	}

	const NVSDK_NGX_Result r = DlssNr_EvalNr(cmdlist, s_nrParams);
	if (r != kNgxSuccess)
	{
		if (DlssNr_Diag() || r != kNgxFailInvalidParameter)
			Warning(eDLL_T::MS, "[DLSSNR] EvaluateFeature 18 -> 0x%08X %s\n", r, DlssNr_ResultName(r));
		DlssNr_Transition(list, origOut, D3D12_RESOURCE_STATE_COPY_SOURCE, origOutState);
		dev->Release();
		return false;
	}

	DlssNr_ToState(list, s_subOut, D3D12_RESOURCE_STATE_COPY_SOURCE);
	DlssNr_Transition(list, origOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	DlssNr_CopyMip0(list, origOut, s_subOut.tex);
	DlssNr_Transition(list, origOut, D3D12_RESOURCE_STATE_COPY_DEST, origOutState);
	DlssNr_ToState(list, s_subOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	s_needReset = false;
	++s_nrEvals;
	if (!s_loggedFirstRun)
	{
		s_loggedFirstRun = true;
		Msg(eDLL_T::MS, "[DLSSNR] running %ux%u intensity=%.2f style=%d preset=%d\n",
			w, h, settings_dlssnr_intensity.GetFloat(),
			settings_dlssnr_style.GetInt(), settings_dlssnr_preset.GetInt());
	}
	dev->Release();
	return true;
}

static NVSDK_NGX_Result DlssNr_HookEval(void* cmdlist, const NVSDK_NGX_Handle* handle,
	const NVSDK_NGX_Parameter* p, void* cb)
{
	return s_origEval ? s_origEval(cmdlist, handle, p, cb) : kNgxFailNotInitialized;
}

static NVSDK_NGX_Result DlssNr_HookEvalC(void* cmdlist, const NVSDK_NGX_Handle* handle,
	const NVSDK_NGX_Parameter* p, void* cb)
{
	if (s_origEvalC && s_origEvalC != s_origEval)
		return s_origEvalC(cmdlist, handle, p, cb);
	return DlssNr_HookEval(cmdlist, handle, p, cb);
}

static NVSDK_NGX_Result DlssNr_HookCreate(void* cmdlist, int featureId, NVSDK_NGX_Parameter* p,
	NVSDK_NGX_Handle** out)
{
	const NVSDK_NGX_Result r = s_origCreate ? s_origCreate(cmdlist, featureId, p, out) : kNgxFailNotInitialized;
	if (r == kNgxSuccess && out && *out)
		DlssNr_Remember(*out, featureId);
	if (DlssNr_Diag() && (featureId == kNgxFeatureSuperSampling || featureId == kNgxFeatureNeuralRendering || r != kNgxSuccess))
		Warning(eDLL_T::MS, "[DLSSNR] CreateFeature id=%d -> 0x%08X %s\n", featureId, r, DlssNr_ResultName(r));
	return r;
}

static bool DlssNr_BindExports(HMODULE ngx)
{
	void* eval = GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature");
	void* evalC = GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature_C");
	void* create = GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature");
	if (!eval && evalC)
		eval = evalC;
	if (!eval || !create)
		return false;

	s_origEval = reinterpret_cast<PFN_NgxEval>(eval);
	s_origEvalC = reinterpret_cast<PFN_NgxEval>(evalC);
	s_origCreate = reinterpret_cast<PFN_NgxCreate>(create);
	s_origRelease = reinterpret_cast<PFN_NgxRelease>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
	s_origAllocParams = reinterpret_cast<PFN_NgxAllocParams>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
	s_origDestroyParams = reinterpret_cast<PFN_NgxDestroyParams>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_DestroyParameters"));
	s_origGetCaps = reinterpret_cast<PFN_NgxGetCaps>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
	s_origScratch = reinterpret_cast<PFN_NgxScratch>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetScratchBufferSize"));
	s_origInit = reinterpret_cast<PFN_NgxInit>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init"));
	s_origInitExt = reinterpret_cast<PFN_NgxInitExt>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext"));
	s_origInitProject = reinterpret_cast<PFN_NgxInitProject>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID"));
	s_origShutdown = reinterpret_cast<PFN_NgxShutdown>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown"));
	s_origShutdown1 = reinterpret_cast<PFN_NgxShutdown1>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown1"));

	s_ngxMod = ngx;
	return true;
}

static bool DlssNr_TryInstallHooks(void)
{
	if (s_origEval && s_origCreate)
		return true;
	if (s_hooksFailed)
		return false;

	if (!s_ngxCandCount)
		DlssNr_CollectRuntimes();

	if (!s_ngxCandCount)
	{
		if (!s_loggedNeedNgx)
		{
			s_loggedNeedNgx = true;
			DlssNr_Log("no NGX runtime: System32, DriverStore and exe dir all lack a usable nvngx/_nvngx.dll (err=%u)\n",
				GetLastError());
		}
		return false;
	}

	DlssNr_Log("NGX runtime candidates (%d), in Init order:\n", s_ngxCandCount);
	for (int i = 0; i < s_ngxCandCount; ++i)
	{
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(s_ngxCands[i], path, MAX_PATH);
		DlssNr_LogModuleVersion(s_ngxCands[i], path);
	}

	if (!DlssNr_BindExports(s_ngxCands[0]))
	{
		s_hooksFailed = true;
		DlssNr_Log("NGX D3D12 exports missing on %p\n", s_ngxCands[0]);
		return false;
	}

	DlssNr_Log("bound %p eval=%p create=%p initP=%p shutdown=%p nrDll=%d\n",
		s_ngxMod, s_origEval, s_origCreate, s_origInitProject, s_origShutdown,
		DlssNr_NrDllPresent() ? 1 : 0);

	// nvngx_dlssnr.dll is never LoadLibrary'd here -- NGX loads the model itself off
	// the Init PathList, and mapping it would expose a second set of NGX exports.
	return true;
}


// NVSDK_NGX_AppLogCallback. NGX states its own reason for a refusal here; every
// other channel only reports the result code.
static void __cdecl DlssNr_NgxLog(const char* message, int level, int component)
{
	if (!message || !DlssNr_Diag())
		return;
	char line[1024] = {};
	_snprintf_s(line, sizeof(line), _TRUNCATE, "%s", message);
	for (char* p = line; *p; ++p)
	{
		if (*p == '\r' || *p == '\n')
			*p = ' ';
	}
	DlssNr_Log("ngx[%d/%d] %s\n", level, component, line);
}

static void DlssNr_ShutdownSession(void)
{
	if (!s_initedNgx)
		return;
	if (s_origShutdown1)
		s_origShutdown1(s_initDevice);
	else if (s_origShutdown)
		s_origShutdown();
	s_initedNgx = false;
	s_initDevice = nullptr;
}

static void DlssNr_PreloadNrModel(void)
{
	if (s_nrDllMod)
		return;
	wchar_t dir[MAX_PATH] = {};
	DlssNr_ExeDirW(dir, MAX_PATH);
	wchar_t path[MAX_PATH] = {};
	_snwprintf_s(path, _TRUNCATE, L"%s\\nvngx_dlssnr.dll", dir);

	// NGXInitLog reads this at Init_Ext. Clearing it first silences the snippet.
	DlssNr_ApplyNgxLogEnv();

	s_nrDllMod = LoadLibraryW(path);
	const DWORD ms = s_nrDllMod ? 0 : GetLastError();
	DlssNr_Log("preload nvngx_dlssnr.dll -> %p err=%u\n", s_nrDllMod, s_nrDllMod ? 0u : ms);
	if (s_nrDllMod)
	{
		wchar_t mapped[MAX_PATH] = {};
		GetModuleFileNameW(s_nrDllMod, mapped, MAX_PATH);
		DlssNr_LogModuleVersion(s_nrDllMod, mapped);
	}
}

static void DlssNr_LogCapKey(NVSDK_NGX_Parameter* p, const char* key)
{
	if (!p || !key)
		return;
	int v = 0;
	const NVSDK_NGX_Result r = p->Get(key, &v);
	if (r == kNgxSuccess)
		DlssNr_Log("  cap %s = %d\n", key, v);
	else
		DlssNr_Log("  cap %s -> 0x%08X %s\n", key, static_cast<unsigned>(r), DlssNr_ResultName(r));
}

static bool DlssNr_ProbeFeature18(void)
{
	s_nrProbeResult = kNgxFailNotInitialized;
	if (!s_origScratch)
	{
		DlssNr_Log("GetScratchBufferSize export missing -- cannot probe feature 18\n");
		return false;
	}
	if (!DlssNr_EnsureParams())
		return false;

	s_nrParams->Reset();
	s_nrParams->Set("Width", 1920u);
	s_nrParams->Set("Height", 1080u);
	s_nrParams->Set("OutWidth", 1920u);
	s_nrParams->Set("OutHeight", 1080u);
	s_nrParams->Set("CreationNodeMask", 1u);
	s_nrParams->Set("VisibilityNodeMask", 1u);

	unsigned long long bytes = 0;
	s_nrProbeResult = s_origScratch(kNgxFeatureNeuralRendering, s_nrParams, &bytes);
	s_lastNrResult = s_nrProbeResult;
	DlssNr_Log("GetScratch 18 -> 0x%08X %s bytes=%llu\n",
		static_cast<unsigned>(s_nrProbeResult), DlssNr_ResultName(s_nrProbeResult), bytes);

	if (s_origGetCaps)
	{
		NVSDK_NGX_Parameter* caps = nullptr;
		if (s_origGetCaps(&caps) == kNgxSuccess && caps)
		{
			static const char* keys[] = {
				"SuperSampling.Available",
				"SuperSampling.NeedsUpdatedDriver",
				"SuperSampling.MinDriverVersionMajor",
				"SuperSampling.MinDriverVersionMinor",
				"SuperSampling.FeatureInitResult",
				"SuperSamplingDenoising.Available",
				"Reserved18.Available",
				"DLSSNR.Available",
				"NeuralRendering.Available"
			};
			for (int i = 0; i < 9; ++i)
				DlssNr_LogCapKey(caps, keys[i]);
		}
	}

	return s_nrProbeResult == kNgxSuccess;
}

// One-shot Create 18 through the snippet's own exports on a throwaway list. The
// snippet ignores GetScratch (returns 0) and reads DLSSNR.* keys, so a minimal
// param set proves whether the driver/GPU carries DLSS-NR at all.
// NotSupported/Denied latches NR; every other failure keeps the after-resolve
// path's 8-try budget.
static void DlssNr_ProbeCreate18(ID3D12Device* device)
{
	if (s_probe18Done)
		return;
	s_probe18Done = true;

	if (!device || !DlssNr_SnippetInit(device) || !s_snipCreate || !s_snipRelease)
	{
		DlssNr_Log("probe create18 skipped -- snippet init/exports unavailable\n");
		return;
	}
	if (!DlssNr_EnsureParams())
	{
		DlssNr_Log("probe create18 skipped -- AllocateParameters failed\n");
		return;
	}

	ID3D12CommandAllocator* alloc = nullptr;
	ID3D12GraphicsCommandList* list = nullptr;
	if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
		|| FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))))
	{
		Nr_Release(list);
		Nr_Release(alloc);
		DlssNr_Log("probe create18 skipped -- no throwaway command list\n");
		return;
	}

	s_nrParams->Reset();
	s_nrParams->Set("CreationNodeMask", 1u);
	s_nrParams->Set("VisibilityNodeMask", 1u);
	s_nrParams->Set("DLSSNR.Width", 1920u);
	s_nrParams->Set("DLSSNR.Height", 1080u);
	s_nrParams->Set("DLSSNR.ScalingRatio", 1.0f);

	NVSDK_NGX_Handle* handle = nullptr;
	NVSDK_NGX_Result r = kNgxFailUnableToInit;
	bool faulted = false;
	__try
	{
		r = s_snipCreate(list, nullptr, s_nrParams, &handle);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		faulted = true;
		handle = nullptr;
	}

	if (faulted)
	{
		DlssNr_Log("probe create18 (snippet) faulted -- latched off\n");
		s_createLatched = true;
	}
	else
	{
		s_nrProbeResult = r;
		DlssNr_FileLog("[DLSSNR] probe snippet CreateFeature 18 (minimal) -> 0x%08X %s handle=%p\n",
			static_cast<unsigned>(r), DlssNr_ResultName(r), handle);
		if (handle)
			s_snipRelease(handle);
		if (r == kNgxFailNotSupported || r == kNgxFailDenied)
			s_createLatched = true;
	}

	list->Close();
	list->Release();
	alloc->Release();
}

static NVSDK_NGX_Result DlssNr_InitIdentity(ID3D12Device* device, const wchar_t* logDir,
	const NVSDK_NGX_FeatureCommonInfo* info, const wchar_t* leaf, int cand)
{
	NVSDK_NGX_Result r = kNgxFailNotInitialized;
	const int sdk = kNgxSdkVersion;
	if (s_origInitProject)
	{
		r = s_origInitProject(kNgxProjectId, kNgxEngineCustom, "1.0.0", logDir, device, sdk, info);
		DlssNr_Log("Init_ProjectID [%d] %ls sdk=0x%02X id=%s -> 0x%08X %s\n",
			cand, leaf, sdk, kNgxProjectId, r, DlssNr_ResultName(r));
	}
	if (r != kNgxSuccess && r != kNgxFailAlreadyExists && s_origInitExt)
	{
		r = s_origInitExt(kNgxAppId, logDir, device, sdk, info);
		DlssNr_Log("Init_Ext [%d] %ls sdk=0x%02X app=0x%X -> 0x%08X %s\n",
			cand, leaf, sdk, static_cast<unsigned>(kNgxAppId), r, DlssNr_ResultName(r));
	}
	if (r != kNgxSuccess && r != kNgxFailAlreadyExists && s_origInit)
	{
		r = s_origInit(kNgxAppId, logDir, device, info, sdk);
		DlssNr_Log("Init [%d] %ls sdk=0x%02X -> 0x%08X %s\n",
			cand, leaf, sdk, r, DlssNr_ResultName(r));
	}
	return r;
}

static bool DlssNr_TryInit(ID3D12Device* device)
{
	if (s_initedNgx)
		return true;
	if (s_presents < s_initRetryAt)
		return false;
	if (s_initFailed || !device || (!s_origInit && !s_origInitExt && !s_origInitProject))
		return false;

	wchar_t dir[MAX_PATH] = {};
	DlssNr_ExeDirW(dir, MAX_PATH);
	wchar_t logDir[MAX_PATH] = {};
	_snwprintf_s(logDir, _TRUNCATE, L"%s\\platform\\logs", dir);
	CreateDirectoryW(logDir, nullptr);

	wchar_t models[MAX_PATH] = {};
	wchar_t ngxRoot[MAX_PATH] = {};
	wchar_t pd[MAX_PATH] = {};
	GetEnvironmentVariableW(L"PROGRAMDATA", pd, MAX_PATH);
	_snwprintf_s(ngxRoot, _TRUNCATE, L"%s\\NVIDIA\\NGX", pd);
	_snwprintf_s(models, _TRUNCATE, L"%s\\NVIDIA\\NGX\\models", pd);
	const wchar_t* paths[3] = { dir, ngxRoot, models };
	NVSDK_NGX_FeatureCommonInfo info = {};
	info.PathListInfo.Path = paths;
	info.PathListInfo.Length = 3;
	info.LoggingInfo.LoggingCallback = reinterpret_cast<void*>(&DlssNr_NgxLog);
	info.LoggingInfo.MinimumLoggingLevel = DlssNr_NgxLogLevel();
	info.LoggingInfo.DisableOtherLoggingSinks = !DlssNr_Diag();

	NVSDK_NGX_Result r = kNgxFailNotInitialized;
	int kept = -1;
	bool nrOk = false;
	const bool wantNr = DlssNr_NrDllPresent();

	for (int c = 0; c < s_ngxCandCount && !nrOk; ++c)
	{
		if (c > 0 || s_ngxMod != s_ngxCands[c])
		{
			if (s_initedNgx)
				DlssNr_ShutdownSession();
			if (!DlssNr_BindExports(s_ngxCands[c]))
				continue;
		}

		wchar_t modPath[MAX_PATH] = {};
		GetModuleFileNameW(s_ngxCands[c], modPath, MAX_PATH);
		const wchar_t* leaf = wcsrchr(modPath, L'\\');
		leaf = leaf ? leaf + 1 : modPath;

		r = DlssNr_InitIdentity(device, logDir, &info, leaf, c);
		if (r != kNgxSuccess && r != kNgxFailAlreadyExists)
			continue;

		s_initedNgx = true;
		s_initDevice = device;
		s_lastInitResult = r;
		kept = c;
		DlssNr_Log("NGX core session open on %p %ls\n", s_ngxMod, modPath);
		DlssNr_PreloadNrModel();
		// The core hosts SuperSampling (feature 1). Feature 18 is never routed
		// through this core -- it is created off the snippet's own exports, so
		// the core-side GetScratch/caps probe is diagnostic only.
		if (wantNr)
			DlssNr_ProbeFeature18();
		return true;
	}

	s_lastInitResult = r;
	if (nrOk)
		return true;

	if (kept >= 0)
	{
		if (!s_initedNgx)
		{
			if (!DlssNr_BindExports(s_ngxCands[kept]))
			{
				s_initFailed = true;
				return false;
			}
			wchar_t modPath[MAX_PATH] = {};
			GetModuleFileNameW(s_ngxCands[kept], modPath, MAX_PATH);
			const wchar_t* leaf = wcsrchr(modPath, L'\\');
			leaf = leaf ? leaf + 1 : modPath;
			r = DlssNr_InitIdentity(device, logDir, &info, leaf, kept);
			if (r == kNgxSuccess || r == kNgxFailAlreadyExists)
			{
				s_initedNgx = true;
				s_initDevice = device;
				s_lastInitResult = r;
				DlssNr_PreloadNrModel();
				if (wantNr)
					DlssNr_ProbeFeature18();
			}
		}
		if (s_initedNgx)
			return true;
	}

	if (s_ngxCandCount)
		DlssNr_BindExports(s_ngxCands[0]);

	if (r == kNgxFailOutOfDate && ++s_initAttempts < kNgxInitAttempts)
	{
		s_initRetryAt = s_presents + 45;
		DlssNr_Log("NGX Init 0x%08X %s -- attempt %d of %d, retrying at present %d\n",
			r, DlssNr_ResultName(r), s_initAttempts, kNgxInitAttempts, s_initRetryAt);
		return false;
	}

	s_initFailed = true;
	DlssNr_Log("NGX Init failed 0x%08X %s after %d attempts -- latched off, dlssnr_reset to retry\n",
		r, DlssNr_ResultName(r), s_initAttempts ? s_initAttempts : 1);
	return false;
}

static bool DlssNr_EnsurePresentCmd(ID3D12Device* device)
{
	if (s_presentAlloc && s_presentCmd)
		return true;
	if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_presentAlloc))))
		return false;
	if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_presentAlloc, nullptr, IID_PPV_ARGS(&s_presentCmd))))
	{
		Nr_Release(s_presentAlloc);
		return false;
	}
	s_presentCmd->Close();
	return true;
}

static void DlssNr_PresentFallback(void)
{
	ID3D12CommandQueue* queue = DirectX_GetDx12GameQueue();
	IDXGISwapChain* sc = Dx12_GetGameSwapChain();
	if (!queue || !sc)
		return;

	ID3D12Device* device = nullptr;
	if (FAILED(sc->GetDevice(IID_PPV_ARGS(&device))) || !device)
		return;

	if (!s_origEval || !s_origCreate)
	{
		device->Release();
		return;
	}

	if (!DlssNr_TryInit(device))
	{
		device->Release();
		return;
	}

	if (!DlssNr_EnsurePresentCmd(device)
		|| FAILED(s_presentAlloc->Reset())
		|| FAILED(s_presentCmd->Reset(s_presentAlloc, nullptr)))
	{
		device->Release();
		return;
	}

	ID3D12Resource* back = nullptr;
	sc->GetBuffer(0, IID_PPV_ARGS(&back));
	const NetObsSym_t tsaaSym = (s_presentFrameId & 1ull) ? NetObsSym_t::RtTsaa1 : NetObsSym_t::RtTsaa0;
	ID3D12Resource* color = DlssNr_FromEngineRt(tsaaSym);
	ID3D12Resource* depth = DlssNr_FromEngineRt(NetObsSym_t::RtFullFrameDepth);
	const bool hudless = (color != nullptr);
	if (!color)
		color = back;
	if (!color)
	{
		if (back)
			back->Release();
		s_presentCmd->Close();
		device->Release();
		return;
	}

	const D3D12_RESOURCE_DESC cd = color->GetDesc();
	const UINT w = static_cast<UINT>(cd.Width);
	const UINT h = cd.Height;
	if (!s_dummyMv || s_dummyW != w || s_dummyH != h)
	{
		Nr_Release(s_dummyMv);
		s_dummyMv = DlssNr_CreateTex(device, w, h, DXGI_FORMAT_R16G16_FLOAT);
		s_dummyW = w;
		s_dummyH = h;
	}

	const D3D12_RESOURCE_STATES colorState = hudless
		? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
		: D3D12_RESOURCE_STATE_PRESENT;

	const bool ok = DlssNr_RunOnList(s_presentCmd, nullptr, color, depth, s_dummyMv, colorState);
	s_presentCmd->Close();
	if (ok)
	{
		ID3D12CommandList* lists[] = { s_presentCmd };
		queue->ExecuteCommandLists(1, lists);
		s_didPathA = true;
	}

	if (back)
		back->Release();
	if (hudless && color && color != back)
		color->Release();
	if (depth)
		depth->Release();
	device->Release();
}

static void DlssNr_ResetLatch_f(const CCommand& args)
{
	(void)args;
	DlssNr_ResetLatch();
	Msg(eDLL_T::MS, "[DLSSNR] failure latch cleared\n");
}

static ConCommand dlssnr_reset("dlssnr_reset", DlssNr_ResetLatch_f,
	"Clear DLSS NR failure latch and recreate feature 18.", FCVAR_RELEASE);

static void DlssNr_Dump_f(const CCommand& args)
{
	(void)args;
	DlssNr_DumpStatus();
}

static ConCommand dlssnr_dump("dlssnr_dump", DlssNr_Dump_f,
	"Write NGX/NR census to the console and dlssnr.log next to the exe.", FCVAR_RELEASE);

void DlssNr_OnEnginePresent(void)
{
	DlssNr_EnsureCs();

	if (!DirectX_IsDx12Mode())
	{
		if (DlssNr_NrDllPresent() && !s_loggedNeedDx12)
		{
			s_loggedNeedDx12 = true;
			Warning(eDLL_T::MS, "[DLSSNR] nvngx_dlssnr.dll present -- launch r5apex_dx12.exe\n");
		}
		return;
	}

	++s_presents;
	++s_presentFrameId;
	EnterCriticalSection(&s_cs);
	DlssNr_DrainRetiredFeatures(false);
	LeaveCriticalSection(&s_cs);
	DlssNr_ProbeVendor();

	if (s_vendorProbed && !s_isNvidia)
	{
		s_didPathA = false;
		return;
	}

	// SuperSampling needs this session even when nvngx_dlssnr.dll is absent.
	if (!s_origEval && !s_gaveUpInstall)
	{
		if (s_presents == 1 || s_presents == 60 || s_presents == 300 || s_presents == 900)
		{
			if (!DlssNr_TryInstallHooks() && s_presents >= 900)
				s_gaveUpInstall = true;
		}
	}

	if (s_origEval && !s_initedNgx && !s_initFailed)
	{
		IDXGISwapChain* sc = Dx12_GetGameSwapChain();
		ID3D12Device* device = nullptr;
		if (sc && SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device))) && device)
		{
			DlssNr_TryInit(device);
			device->Release();
		}
	}

	if (s_initedNgx && !s_probe18Done && !s_createLatched && DlssNr_NrDllPresent())
	{
		IDXGISwapChain* sc = Dx12_GetGameSwapChain();
		ID3D12Device* device = nullptr;
		if (sc && SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device))) && device)
		{
			EnterCriticalSection(&s_cs);
			DlssNr_ProbeCreate18(device);
			LeaveCriticalSection(&s_cs);
			device->Release();
		}
	}

	if (s_presents == 1 || s_presents == 60 || s_presents == 180)
		DlssNr_DumpStatus();

	// The feature belongs to the thread that records with it. This one only
	// observes: it read 0 here while the render thread was still evaluating
	// against the same ConVar, and tore the feature down every thirty frames.
	if (settings_dlssnr.GetInt() == 0)
	{
		if (s_nrOffPresents < 10000)
			++s_nrOffPresents;
		if (s_nrOffPresents == 30)
			DlssNr_FileLog("[DLSSNR] present sees settings_dlssnr 0 for 30 frames, handle=%p evals=%d\n",
				s_nrHandle, s_nrEvals);
		s_didPathA = false;
		return;
	}
	if (s_nrOffPresents >= 30)
		DlssNr_FileLog("[DLSSNR] present sees settings_dlssnr non-zero again after %d frames\n",
			s_nrOffPresents);
	s_nrOffPresents = 0;

	if (!DlssNr_ShouldEvaluate())
	{
		s_didPathA = false;
		return;
	}

	const int presentMode = settings_dlssnr_present.GetInt();
	const bool wantPresent = presentMode != 0;

	if (!s_didPathA && wantPresent && s_origEval && !s_createLatched && !s_initFailed)
	{
		if (presentMode == -1 && s_ssEvals == 0 && s_presents == 181)
			Warning(eDLL_T::MS, "[DLSSNR] no SuperSampling evaluate yet -- present-time path\n");
		EnterCriticalSection(&s_cs);
		++s_frameNo;
		DlssNr_DrainRetired();
		DlssNr_PresentFallback();
		LeaveCriticalSection(&s_cs);
	}

	s_didPathA = false;
}

void DlssNr_Shutdown(void)
{
	if (!s_csInit)
		return;
	EnterCriticalSection(&s_cs);
	DlssNr_ReleaseFeature("shutdown");
	DlssNr_DrainRetiredFeatures(true);
	DlssNr_ReleaseSuperSamplingHandle();
	if (s_nrParams && s_nrParamsOwned && s_origDestroyParams)
		s_origDestroyParams(s_nrParams);
	s_nrParams = nullptr;
	if (s_ssParams && s_ssParamsOwned && s_origDestroyParams)
		s_origDestroyParams(s_ssParams);
	s_ssParams = nullptr;
	Nr_Release(s_subColor.tex);
	Nr_Release(s_subOut.tex);
	Nr_Release(s_subDepth.tex);
	Nr_Release(s_subMv.tex);
	Nr_Release(s_dummyMv);
	Nr_Release(s_scratch);
	Nr_Release(s_nrScratch);
	Nr_Release(s_presentCmd);
	Nr_Release(s_presentAlloc);
	for (int i = 0; i < 8; ++i)
		Nr_Release(s_retired[i].tex);
	if (s_initedNgx)
	{
		if (s_origShutdown1)
			s_origShutdown1(s_initDevice);
		else if (s_origShutdown)
			s_origShutdown();
		s_initedNgx = false;
		s_initDevice = nullptr;
	}
	if (s_hooksInstalled)
	{
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		if (s_origEval)
			DetourDetach(&(LPVOID&)s_origEval, DlssNr_HookEval);
		if (s_origCreate)
			DetourDetach(&(LPVOID&)s_origCreate, DlssNr_HookCreate);
		if (s_origEvalC && s_origEvalC != s_origEval)
			DetourDetach(&(LPVOID&)s_origEvalC, DlssNr_HookEvalC);
		DetourTransactionCommit();
		s_hooksInstalled = false;
	}
	LeaveCriticalSection(&s_cs);
}

