//=============================================================================//
//
// Purpose: client.dll entry point (DllMain) for the S21 inject product
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/r5dev.h"
#include "core/init.h"
#include "core/logdef.h"
#include "core/logger.h"
#include "engine/client/net_observer.h"
#include "engine/client/net_bridge_addrs.h"
#include "tier0/dbg.h"
#include "core/sdk_stage.h"
#include "engine/sys_integrity.h"
#include "ebisusdk/origin_offline.h"
#include "ebisusdk/origin_igo.h"
#include "tier0/tier0_iface.h"
#include "tier0/cpu.h"
#include "tier0/basetypes.h"
#include "tier0/crashhandler.h"
#include "tier0/commandline.h"
#include "tier0/platform.h"
#include "tier2/crashreporter.h"
#include "rtech/pak/pakstate.h"
#include "engine/client/clientstate.h"
#include "ebisusdk/EbisuSDK.h"
#include "common/global.h"
/*****************************************************************************/
#include "windows/id3dx.h"
#include "windows/input.h"
#include "windows/console.h"
#include "windows/system.h"
#include "mathlib/mathlib.h"
#include "launcher/launcher.h"
#include "protobuf/stubs/common.h"
#include "gameui/imgui_system.h"
#include "gameui/IConsole.h"
#include "inputsystem/inputsystem.h"
#include "engine/sys_mainwind.h"
#include "filesystem/filesystem.h"
#include "pluginsystem/modsystem.h"
#include "vstdlib/keyvaluessystem.h"
#include "vscript/vscript_s21_override.h"
#include "vscript/vsquirrel_s21.h"
#include <engine/cmd.h>
#include "engine/net_chan.h"
#include "engine/client/clientstate.h"
#include "engine/client/client.h"
#include "ebisusdk/EbisuSDK.h"
#include "public/tier1/cvar.h"
#include "tier1/convar.h"

#define SDK_DEFAULT_CFG "cfg/system/startup_default.cfg"

bool g_bSdkInitialized = false;

bool g_bSdkInitCallInitiated = false;
bool g_bSdkShutdownCallInitiated = false;

bool g_bSdkShutdownInitiatedFromConsoleHandler = false;

static bool s_bConsoleInitialized = false;
static HMODULE s_hModuleHandle = NULL;
static bool s_bRunningDx12Exe = false;

void SDK_Log(const char* fmt, ...);
void SDK_LogDevFile(const char* fmt, ...);

// NetObserver_Install returns int; PFN_SdkStage is void (*)(void).
static void SDK_Stage_NetObserver_Install()
{
	NetObserver_Install();
}

//#############################################################################
// UTILITY
//#############################################################################

static const char* SDK_GetExeBaseName()
{
	static char s_exeName[MAX_PATH] = {};

	if (!s_exeName[0])
	{
		char path[MAX_PATH] = {};
		const DWORD len = GetModuleFileNameA(NULL, path, SDK_ARRAYSIZE(path));
		if (len > 0 && len < SDK_ARRAYSIZE(path))
		{
			const char* slash = strrchr(path, '\\');
			const char* fslash = strrchr(path, '/');
			if (!slash || (fslash && fslash > slash))
				slash = fslash;

			V_strncpy(s_exeName, slash ? slash + 1 : path, sizeof(s_exeName));
		}
		else
		{
			V_strncpy(s_exeName, "<unknown>", sizeof(s_exeName));
		}
	}

	return s_exeName;
}

// Exposed (declared in ebisusdk/EbisuSDK.h) so VEbisuSDK can guard its DX11-only
// S21 Origin RVAs on the DX12 image.
bool SDK_IsDx12Exe()
{
	return V_stristr(SDK_GetExeBaseName(), "dx12") != nullptr;
}

static bool SDK_WaitForEngineRendererReady()
{
	if (CommandLine()->CheckParm("-sdk_skip_render_wait"))
	{
		SDK_Log("Render wait skipped by -sdk_skip_render_wait\n");
		return true;
	}

	if (SDK_IsDx12Exe())
	{
		SDK_Log("DX12 executable detected (%s); waiting for DX12 renderer modules...\n",
			SDK_GetExeBaseName());

		int waitedMs = 0;
		while (waitedMs < 30000)
		{
			const HMODULE dxgi = GetModuleHandleA("dxgi.dll");
			const HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
			const HMODULE d3d12Core = GetModuleHandleA("d3d12core.dll");

			if (dxgi && (d3d12 || d3d12Core))
			{
				SDK_Log("Engine ready (DX12 modules found after %dms: dxgi=%p d3d12=%p d3d12core=%p)\n",
					waitedMs, (void*)dxgi, (void*)d3d12, (void*)d3d12Core);

				// DX12 has no D3D11 globals to poll; 2s grace before overlay init.
				Sleep(2000);
				return true;
			}

			Sleep(100);
			waitedMs += 100;
		}

		SDK_Log("TIMEOUT: DX12 renderer modules not found after 30s, aborting\n");
		return false;
	}

	SDK_Log("DX11 executable detected (%s); waiting for engine D3D11 init...\n",
		SDK_GetExeBaseName());

	// Wait for D3D11 device + swap chain. Pattern-scanning before both exist can crash.
	ID3D11Device**    ppDevice    = reinterpret_cast<ID3D11Device**>(NetObs_Sym(NetObsSym_t::D3D11DeviceGlobal));
	IDXGISwapChain**  ppSwapChain = reinterpret_cast<IDXGISwapChain**>(NetObs_Sym(NetObsSym_t::LegacySwapChainGlobal));
	if (!ppDevice || !ppSwapChain)
		return false;

	int waitedMs = 0;
	while ((!*ppDevice || !*ppSwapChain) && waitedMs < 30000)
	{
		Sleep(100);
		waitedMs += 100;
	}

	if (!*ppDevice || !*ppSwapChain)
	{
		SDK_Log("TIMEOUT: D3D11 not fully initialized after 30s, aborting\n");
		SDK_Log("  ppDevice=%p ppSwapChain=%p\n", (void*)*ppDevice, (void*)*ppSwapChain);
		return false;
	}

	SDK_Log("Engine ready (D3D11 device found after %dms)\n", waitedMs);
	return true;
}

// True when overlay renderer modules are already mapped.
static bool SDK_RendererModulesPresent()
{
	if (SDK_IsDx12Exe())
	{
		const HMODULE dxgi = GetModuleHandleA("dxgi.dll");
		const HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
		const HMODULE d3d12Core = GetModuleHandleA("d3d12core.dll");
		return dxgi && (d3d12 || d3d12Core);
	}

	ID3D11Device**   ppDevice    = reinterpret_cast<ID3D11Device**>(NetObs_Sym(NetObsSym_t::D3D11DeviceGlobal));
	IDXGISwapChain** ppSwapChain = reinterpret_cast<IDXGISwapChain**>(NetObs_Sym(NetObsSym_t::LegacySwapChainGlobal));
	return ppDevice && ppSwapChain && *ppDevice && *ppSwapChain;
}

void Crash_Callback(const CCrashHandler* handler)
{
	CrashReporter_SubmitToCollector(handler);
	SpdLog_Shutdown(); // Shutdown SpdLog to flush all buffers.
}

static bool s_bEmblemPrinted = false;

void Show_Emblem()
{
	if (s_bEmblemPrinted)
		return;
	s_bEmblemPrinted = true;

	// Logged as 'SYSTEM_ERROR' for its red color.
	for (size_t i = 0; i < SDK_ARRAYSIZE(R5F_EMBLEM); i++)
	{
		Msg(eDLL_T::SYSTEM_ERROR, "%s\n", R5F_EMBLEM[i]);
	}

	// Log the SDK's 'build_id' under the emblem.
	Msg(eDLL_T::SYSTEM_ERROR,
		"+---- [CLIENT] Apex Legends S21 -----------[%s%010u%s]--+\n",
		g_svYellowF.c_str(), g_SDKDll.GetNTHeaders()->FileHeader.TimeDateStamp, g_svRedF.c_str());
	// Msg(eDLL_T::SYSTEM_ERROR, "\n");
}

// Stdout before Msg is wired. Build id is the PE TimeDateStamp (g_SDKDll is not up yet).
static void SDK_PrintEmblemRaw()
{
	extern bool g_bSdkConsoleAnsi; // defined further down in this TU

	unsigned int buildId = 0;
	if (s_hModuleHandle)
	{
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(s_hModuleHandle);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			reinterpret_cast<const uint8_t*>(s_hModuleHandle) + dos->e_lfanew);
		buildId = nt->FileHeader.TimeDateStamp;
	}

	const char* clr = g_bSdkConsoleAnsi ? "\033[38;2;097;214;214m" : "";
	const char* rst = g_bSdkConsoleAnsi ? "\033[0m" : "";

	// Same "[uptime] " prefix EngineLoggerSink puts on Msg -- fprintf skips it.
	char uptime[32];
	for (size_t i = 0; i < SDK_ARRAYSIZE(R5F_EMBLEM); i++)
	{
		Plat_GetProcessUpTime(uptime, sizeof(uptime));
		fprintf(stdout, "%s%s%s%s\n", uptime, clr, R5F_EMBLEM[i], rst);
	}
	Plat_GetProcessUpTime(uptime, sizeof(uptime));
	fprintf(stdout, "%s%s+---- [CLIENT] Apex Legends S21 -----------[%010u]--+%s\n",
		uptime, clr, buildId, rst);
	fflush(stdout);
	s_bEmblemPrinted = true;
}

//#############################################################################
// INITIALIZATION
//#############################################################################

void Tier0_Init()
{
	// Codec DLLs are already loaded at inject time; skip early codec init.
	g_CoreMsgVCallback = &EngineLoggerSink;
}

//-----------------------------------------------------------------------------
// Bootstrap: stdout + platform/logs/client/sdk_init.log until SpdLog is up;
// then SDK_LogSwitchToSpdLog replays into the session folder.
//-----------------------------------------------------------------------------
FILE* s_pLogFile = nullptr;
static const char* const s_BootstrapLogPath = "platform/logs/client/sdk_init.log";
static bool s_bSpdLogReady = false;
static std::vector<std::string> s_EarlyMessageBuffer;

// True after Console_ColorInit; SDK_Log wraps stdout in cyan.
bool g_bSdkConsoleAnsi = false;
static constexpr const char* s_SdkLogAnsiColor = "\033[38;2;097;214;214m"; // cyan
static constexpr const char* s_SdkLogAnsiReset = "\033[0m";

// Default off: console stays emblem + engine Msg. File sink still gets every line.
bool g_bSdkObserveInit = false;

static void SDK_Logv(const char* fmt, va_list args, const bool bAllowConsole)
{
	char body[4096];
	const int bodyLen = vsnprintf(body, sizeof(body), fmt, args);
	if (bodyLen <= 0)
		return;

	// Same "[uptime] " prefix as engine Msg. Early boot is "[0.000] ".
	char uptime[32] = "[0.000] ";
	Plat_GetProcessUpTime(uptime, sizeof(uptime));

	char buf[4160];
	const int len = snprintf(buf, sizeof(buf), "%s%s", uptime, body);
	if (len <= 0)
		return;

	// Console echo: g_bSdkObserveInit, sdk_observe_net, or no file sinks.
	extern int g_sdkObserveNet;
	// Without file sinks stdout is the complete history, so always echo there.
	const bool bConsoleEcho = bAllowConsole && (g_bSdkObserveInit || g_sdkObserveNet >= 1
		|| !SpdLog_FileLogsEnabled());
	if (bConsoleEcho)
	{
		if (g_bSdkConsoleAnsi)
		{
			fputs(s_SdkLogAnsiColor, stdout);
			fputs(buf, stdout);
			fputs(s_SdkLogAnsiReset, stdout);
		}
		else
		{
			fputs(buf, stdout);
		}
		fflush(stdout);
	}

	if (s_bSpdLogReady)
	{
		// SPDLOG_EOL is "" in tweakme.h; format strings already include "\n".
		// Pass buf unmodified -- stripping the newline concatenates lines.
		std::shared_ptr<spdlog::logger> logger = spdlog::get("sdk");
		if (logger)
		{
			// std::string picks info<T>, not format_string_t -- buf may contain '%'.
			logger->info(std::string(buf, (size_t)len));
			logger->flush(); // don't lose last lines on crash
		}
	}
	else
	{
		// Bootstrap: write to flat file + buffer for replay
		if (s_pLogFile)
		{
			fputs(buf, s_pLogFile);
			fflush(s_pLogFile);
		}
		s_EarlyMessageBuffer.emplace_back(buf, (size_t)len);
	}

	// Overlay AddLog is unsafe until g_bSdkObserveInit: Cbuf observers run before ImGui exists.
	if (bAllowConsole && g_bSdkObserveInit)
		g_Console.AddLog(buf, IM_COL32(97, 214, 214, 255));
}

void SDK_Log(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	SDK_Logv(fmt, args, true);
	va_end(args);
}

// Boot/patch traces: message.log under -devsdk/-dev/-developer/-logfiles only.
// Never stdout or overlay -- SDK_Log uses stdout as the log when file sinks are off.
void SDK_LogDevFile(const char* fmt, ...)
{
	if (!SpdLog_FileLogsEnabled())
		return;

	va_list args;
	va_start(args, fmt);
	SDK_Logv(fmt, args, false);
	va_end(args);
}

void SDK_LogSwitchToSpdLog()
{
	// Minimum-disk mode: no file sinks exist. Drop the bootstrap file and let
	// the stdout echo in SDK_Log carry everything from here on.
	if (!SpdLog_FileLogsEnabled())
	{
		s_EarlyMessageBuffer.clear();
		if (s_pLogFile)
		{
			fclose(s_pLogFile);
			s_pLogFile = nullptr;
		}
		remove(s_BootstrapLogPath);
		s_bSpdLogReady = true;
		return;
	}

	std::shared_ptr<spdlog::logger> logger = spdlog::get("sdk");
	if (!logger)
	{
		// SpdLog is up but the sdk logger wasn't created. Stay in bootstrap
		// mode so we don't silently drop messages.
		SDK_Log("WARNING: spdlog 'sdk' logger missing, staying in bootstrap log mode\n");
		return;
	}

	// Replay bootstrap lines unmodified (SPDLOG_EOL="" -- keep trailing "\n").
	for (const std::string& msg : s_EarlyMessageBuffer)
	{
		logger->info(msg);
	}
	logger->flush();
	s_EarlyMessageBuffer.clear();

	s_bSpdLogReady = true;

	// Bootstrap file is redundant once the session log has the replay.
	if (s_pLogFile)
	{
		fclose(s_pLogFile);
		s_pLogFile = nullptr;
	}
	remove(s_BootstrapLogPath);

	SDK_Log("Logs unified at %s/message.log\n",
		g_LogSessionDirectory.c_str());
}

#define SDK_TRACE(msg) ((void)0)

// DX12 overlay needs dxgi/d3d12 mapped; gated init defers this until they are.
static void SDK_InitOverlay_Stage()
{
	SDK_Log("ImGui step 1: Input_Init...\n");
	Input_Init();
	SDK_Log("ImGui step 1: OK\n");

	SDK_Log("ImGui step 2: SetEnabled...\n");
	ImguiSystem()->SetEnabled(true);
	SDK_Log("ImGui step 2: OK\n");

	SDK_Log("ImGui step 3: DirectX_Init...\n");
	DirectX_Init();
	SDK_Log("ImGui step 3: OK\n");

	SDK_Log("ImGui:         INITIALIZED\n");
	SDK_Log("Toggle overlay: ` / F10 console, F11 browser, F12 manage local\n");
}

static void SDK_InitOverlay()
{
	SDK_TRACE("ImGui initialization...");
	if (s_bRunningDx12Exe || (g_ppSwapChain && *g_ppSwapChain && g_ppGameDevice && *g_ppGameDevice))
	{
		SdkStage_Run("ImGui", SDK_InitOverlay_Stage);
	}
	else
	{
		SDK_Log("WARNING: renderer globals not found, ImGui DISABLED\n");
		SDK_Log("  g_ppSwapChain:  %p -> %p\n",
			(void*)g_ppSwapChain, g_ppSwapChain ? (void*)*g_ppSwapChain : nullptr);
		SDK_Log("  g_ppGameDevice: %p -> %p\n",
			(void*)g_ppGameDevice, g_ppGameDevice ? (void*)*g_ppGameDevice : nullptr);
	}
}

static DWORD WINAPI SDK_DeferredOverlayThread(LPVOID)
{
	if (SDK_WaitForEngineRendererReady())
		SDK_InitOverlay();
	else
		SDK_Log("Deferred overlay TIMEOUT: renderer never became ready; overlay disabled only, game continues\n");
	return 0;
}

static void SDK_InitArmOverlay()
{
	if (SDK_RendererModulesPresent())
	{
		SDK_InitOverlay();
	}
	else
	{
		SDK_Log("Renderer not up yet (gated init); deferring ImGui/DirectX overlay to worker\n");
		CreateThread(NULL, 0, SDK_DeferredOverlayThread, NULL, 0, NULL);
	}
}

static void SDK_SetupConsole()
{
	static bool s_consoleSetupDone = false;
	if (s_consoleSetupDone)
		return;
	s_consoleSetupDone = true;

	// Open console IMMEDIATELY so we can see what happens during init.
	// FreeConsole first in case we inherited one from the injector.
	FreeConsole();
	AllocConsole();
	// Hosted: hide before anything can draw it. The launcher already starts us
	// hidden, so this is the fallback for a run that was not started by it.
	Console_HideIfHosted();
	SetConsoleTitleA("R5F Client");

	FILE* fDummy;
	freopen_s(&fDummy, "CONIN$", "r", stdin);
	freopen_s(&fDummy, "CONOUT$", "w", stdout);
	freopen_s(&fDummy, "CONOUT$", "w", stderr);

	// Enable ANSI virtual terminal processing on stdout so the engine's ANSI
	// color escape codes render as colored text instead of literal garbage.
	extern bool g_bSdkConsoleAnsi;
	g_bSdkConsoleAnsi = Console_ColorInit();
	Console_ApplyHostedSession();

	// Open persistent crash-safe log file FIRST -- survives any crash.
	CreateDirectoryA("platform", nullptr);
	CreateDirectoryA("platform/logs", nullptr);
	CreateDirectoryA("platform/logs/client", nullptr);
	fopen_s(&s_pLogFile, s_BootstrapLogPath, "w");

	SDK_PrintEmblemRaw();

	s_bRunningDx12Exe = SDK_IsDx12Exe();
}

void SDK_Init()
{
	SDK_SetupConsole();
	SDK_TRACE("SDK_Init entered");

	if (g_bSdkInitCallInitiated)
	{
		SDK_Log("ERROR: Recursive SDK_Init call!\n");
		return;
	}
	g_bSdkInitCallInitiated = true;

	SDK_TRACE("CheckSystemCPU...");
	CheckSystemCPU();

	SDK_TRACE("MathLib_Init...");
	MathLib_Init();

	SDK_TRACE("GetProcessEnvironmentBlock...");
	PEB64* pEnv = CModule::GetProcessEnvironmentBlock();

	SDK_TRACE("InitFromBase (game dll)...");
	g_GameDll.InitFromBase(pEnv->ImageBaseAddress);

	SDK_TRACE("InitFromBase (sdk dll)...");
	g_SDKDll.InitFromBase((QWORD)s_hModuleHandle);

	SDK_TRACE("Host image check...");
	if (!NetObs_HostImageSupported())
	{
		const IMAGE_NT_HEADERS64* const nt = g_GameDll.GetNTHeaders();
		SDK_Log("ERROR: unsupported host image (stamp=0x%08X size=0x%08X); this SDK targets the S21 DX11/DX12 client builds only. Not installing.\n",
			nt ? nt->FileHeader.TimeDateStamp : 0u, nt ? nt->OptionalHeader.SizeOfImage : 0u);
		MessageBoxA(NULL, "This game executable is not a supported Season 21 client build.\nThe SDK will not be installed.", "R5 SDK", MB_OK | MB_ICONERROR);
		return;
	}

	SDK_TRACE("Tier0_Init...");
	Tier0_Init();

	SDK_TRACE("CommandLine parse...");
	{
		extern void CommandLine_CreateFromProcess(const char* psz);
		const char* pszCL = GetCommandLineA();
		CommandLine_CreateFromProcess(pszCL);
		SDK_Log("  GetCommandLineA=%p CommandLine=%p\n",
			pszCL, static_cast<void*>(CommandLine()));
		if (pszCL)
			SDK_Log("  cmdline: %.512s\n", pszCL);
	}
	SDK_TRACE("CommandLine parse done");

	SDK_TRACE("SpdLog_Init...");
	s_bConsoleInitialized = true;
	SpdLog_Init(true);

	SDK_LogSwitchToSpdLog();

	SDK_TRACE("Show_Emblem...");
	Show_Emblem();

	SDK_TRACE("Winsock_Startup...");
	Winsock_Startup();

	//=========================================================================
	// Verified S21 patterns only -- Systems_Init would scan unmatched S3 sites.
	//=========================================================================
	SDK_TRACE("S21 pattern scanning (verified functions only)...");
	{
		// Scan for verified S21 functions using their S3 patterns

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 63 D9 41 8B F8 48 8D 0D")
			.GetPtr(Cbuf_AddText);

		// Discriminate Cbuf_Execute by full prologue; short pattern hits two other S21 funcs.
		// mov-edi immediate is a struct stride that differs DX11 vs DX12.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 BF ?? 00 00 00")
			.GetPtr(Cbuf_Execute);

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 44 8B 5A 04 45 8B F8 48 8B EA")
			.GetPtr(v_S21_Cmd_ExecuteString);

		Module_FindPattern(g_GameDll,
			"40 53 48 83 EC 30 48 8B D9 48 8B 09 48 85 C9 0F 84 ?? ?? ?? ?? 80 39 00")
			.GetPtr(v_S21_Connect_Worker);

		//---------------------------------------------------------------------
		// S21 NET_SendPacket chain.
		//---------------------------------------------------------------------
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 44 8B 54 24 78 4D 8B D8 45 8B 00 49 8B F1 48 63 FA 48 8B E9 41 83 F8 04")
			.GetPtr(v_S21_NET_SendPacket_Public);

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 54 41 56 41 57 48 81 EC 70 05 00 00 4C 8B B4 24 B8 05 00 00 33 F6 49 8B D9 4D 8B E0 4C 8B F9")
			.GetPtr(v_S21_NET_SendPacket_Inner);

		Module_FindPattern(g_GameDll,
			"41 56 41 57 48 83 EC 28 80 3D ?? ?? ?? ?? 00 48 89 5C 24 40 48 8B D9")
			.GetPtr(CNetChan__ProcessPacket);

		Module_FindPattern(g_GameDll,
			"4C 89 4C 24 20 41 54 41 57 48 83 EC 38 80 3D ?? ?? ?? ?? 00 48 89 5C 24 50 48 89 6C 24 58 48 89 7C 24 28 8B F9 4C 89 74 24 20 4D 8B F0")
			.GetPtr(v_S21_NET_SendLoopback);

		Module_FindPattern(g_GameDll,
			"40 55 56 57 41 56 41 57 48 8D AC 24 50 FB FF FF 48 81 EC B0 05 00 00 48 8B F9 48 63 F2 48 8B 49 60 4D 8B F1 45 8B F8")
			.GetPtr(v_S21_CBaseClient_SetSignonState);

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 44 8B 59 04")
			.GetPtr(v_Cmd_ForwardToServer);

		// CNetChan
		Module_FindPattern(g_GameDll,
			"88 54 24 10 53 55 57")
			.GetPtr(CNetChan__Clear);

		Module_FindPattern(g_GameDll,
			"48 89 6C 24 18 56 57 41 56 48 83 EC 30 83 B9")
			.GetPtr(CNetChan__Shutdown);

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 55 56 57 41 56 41 57 48 83 EC 70")
			.GetPtr(CNetChan__SendDatagram);

		Module_FindPattern(g_GameDll,
			"40 53 48 83 EC 20 83 B9 ?? ?? ?? ?? ?? 48 8B D9 75 15 48 8B 05")
			.GetPtr(CNetChan__CanPacket);

		// EbisuSDK
		Module_FindPattern(g_GameDll,
			"40 57 48 83 EC 40 83 3D")
			.GetPtr(EbisuSDK_CVar_Init);

		// (CNetChan::SendNetMsg is a class method, not a separate function pointer)

		// CClientState -- S21-specific pattern (S3 pattern didn't match)
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 8B 32 48 8B DA 48 8B 11")
			.GetPtr(CClientState__Connect);

		Module_FindPattern(g_GameDll,
			"40 53 48 83 EC 20 F6 81 ?? ?? ?? ?? ?? 48 8B D9 74 08")
			.GetPtr(CClientState__ConnectionClosing);

		Module_FindPattern(g_GameDll,
			"40 57 48 83 EC 20 83 B9 ?? ?? ?? ?? ?? 48 8B F9 7C 66")
			.GetPtr(CClientState__ProcessServerTick);

		Module_FindPattern(g_GameDll,
			"40 53 48 81 EC ?? ?? ?? ?? 80 B9 ?? ?? ?? ?? ?? 48 8B DA")
			.GetPtr(CClientState__ProcessStringCmd);

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 89 ?? ?? ?? ?? 48 85 C9 0F 84")
			.GetPtr(CClientState__HookClientStringTable);

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 45 33 F6")
			.GetPtr(CClientState__RunFrame);

		// ConVar system -- CCvar__Connect resolves g_pCVar singleton
		Module_FindPattern(g_GameDll,
			"48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 85 C0 48 0F 45 C8 FF 05 ?? ?? ?? ?? 48 89 0D")
			.GetPtr(CCvar__Connect);

		Module_FindPattern(g_GameDll,
			"B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 48 8B 01 48 89 9C 24")
			.GetPtr(v_ConVar_PrintDescription);

		// Print results
		SDK_LogDevFile("Pattern scan results:\n");
		SDK_LogDevFile("  Cbuf_AddText:       %s\n", Cbuf_AddText ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  Cbuf_Execute:        %s\n", Cbuf_Execute ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  Cmd_ForwardToServer: %s\n", v_Cmd_ForwardToServer ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CNetChan::Clear:     %s\n", CNetChan__Clear ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CNetChan::Shutdown:  %s\n", CNetChan__Shutdown ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CNetChan::SendDgram: %s\n", CNetChan__SendDatagram ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CNetChan::CanPacket: %s\n", CNetChan__CanPacket ? "FOUND" : "NOT FOUND");
		// CNetChan::SendNetMsg scanned separately above as SendDatagram
		SDK_LogDevFile("  EbisuSDK_CVar_Init:  %s\n", EbisuSDK_CVar_Init ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  EbisuSDK_GetLang:    %s\n", EbisuSDK_GetLanguage ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CClientState Connect:%s\n", CClientState__Connect ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CClientState ClosCon:%s\n", CClientState__ConnectionClosing ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CClientState SrvTick:%s\n", CClientState__ProcessServerTick ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CClientState StrCmd: %s\n", CClientState__ProcessStringCmd ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CClientState StrTbl: %s\n", CClientState__HookClientStringTable ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CClientState RunFrm: %s\n", CClientState__RunFrame ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CCvar::Connect:      %s\n", CCvar__Connect ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  ConVar::PrintDesc:   %s\n", v_ConVar_PrintDescription ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  Cmd_ExecuteString:   %s\n", v_S21_Cmd_ExecuteString ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  Connect_Worker:      %s\n", v_S21_Connect_Worker ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  ProcessPacket:       %s\n", CNetChan__ProcessPacket ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  SendDatagram:        %s\n", CNetChan__SendDatagram ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  NET_SendPacket(pub): %s\n", v_S21_NET_SendPacket_Public ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  NET_SendPacket(in):  %s\n", v_S21_NET_SendPacket_Inner ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  NET_SendLoopback:    %s\n", v_S21_NET_SendLoopback ? "FOUND" : "NOT FOUND");
		SDK_LogDevFile("  CBaseClient SetSign: %s\n", v_S21_CBaseClient_SetSignonState ? "FOUND" : "NOT FOUND");

		// Inject-after-boot: CVar_Connect never fires; resolve g_pCVar from the S21 RVA.
		extern bool Cvar_ResolveSingletonFromEngine();
		const bool bCvarSingleton = Cvar_ResolveSingletonFromEngine();
		SDK_LogDevFile("  CCvar singleton:     %s\n",
			bCvarSingleton ? "RESOLVED (g_pCVar set)"
											  : "NOT RESOLVED (engine CCvar still NULL)");

		// -sdk_observe_* parsed here: ConVars are not in the engine registry yet.
		{
			extern int g_sdkObserveNet;
			if (const char* arg = CommandLine()->ParmValue("-sdk_observe_net"))
			{
				g_sdkObserveNet = atoi(arg);
				SDK_LogDevFile("  sdk_observe_net:     %d (via cmdline)\n", g_sdkObserveNet);
			}

			if (CommandLine()->CheckParm("-sdk_observe_init"))
			{
				g_bSdkObserveInit = true;
				SDK_LogDevFile("  sdk_observe_init:    ON (via cmdline)\n");
			}
		}
	}

	//=========================================================================
	// Renderer globals + CGame for ImGui.
	//=========================================================================
	SDK_TRACE("Renderer + CGame globals...");
	{
		g_pGame = reinterpret_cast<CGame*>(NetObs_Sym(NetObsSym_t::CGameInstance));
		g_pFileSystem_Stdio = reinterpret_cast<CFileSystem_Stdio*>(NetObs_Sym(NetObsSym_t::FileSystemStdioInstance));
		g_pFullFileSystem = reinterpret_cast<CFileSystem_Stdio**>(NetObs_Sym(NetObsSym_t::FullFileSystemPtr));
		g_pKeyValuesSystem = reinterpret_cast<CKeyValuesSystem*>(NetObs_Sym(NetObsSym_t::KeyValuesSystemInstance));

		if (!s_bRunningDx12Exe)
		{
			g_ppGameDevice       = reinterpret_cast<ID3D11Device**>(NetObs_Sym(NetObsSym_t::D3D11DeviceGlobal));
			g_ppImmediateContext = reinterpret_cast<ID3D11DeviceContext**>(NetObs_Sym(NetObsSym_t::D3D11ContextGlobal));
			g_ppSwapChain        = reinterpret_cast<IDXGISwapChain**>(NetObs_Sym(NetObsSym_t::LegacySwapChainGlobal));
		}
		else
		{
			// DX12 does not expose D3D11 device/context globals. The overlay
			// hooks DXGI from a temporary DX12 swapchain and initializes the
			// DX12 ImGui backend from the real game swapchain on first Present.
			g_ppGameDevice = nullptr;
			g_ppImmediateContext = nullptr;
			g_ppSwapChain = nullptr;
		}

		// CGame::WindowProc pattern is n=1 in both images.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 56 41 57 48 8D 6C 24 B9 48 81 EC C0 00 00 00 45 33 FF 49 8B F1 44 39")
			.GetPtr(CGame__WindowProc);

		SDK_Log("Renderer globals (%s):\n", s_bRunningDx12Exe ? "DX12/DXGI hook bootstrap" : "D3D11");
		SDK_Log("  ppDevice:     %p -> %p\n", (void*)g_ppGameDevice, g_ppGameDevice ? (void*)*g_ppGameDevice : nullptr);
		SDK_Log("  ppContext:    %p -> %p\n", (void*)g_ppImmediateContext, g_ppImmediateContext ? (void*)*g_ppImmediateContext : nullptr);
		SDK_Log("  ppSwapChain:  %p -> %p\n", (void*)g_ppSwapChain, g_ppSwapChain ? (void*)*g_ppSwapChain : nullptr);
		SDK_Log("  g_pGame:      %p (HWND=%p)\n", (void*)g_pGame, g_pGame ? (void*)g_pGame->GetWindow() : nullptr);
		SDK_Log("  FullFileSys:  %p -> %p\n", (void*)g_pFullFileSystem, g_pFullFileSystem ? (void*)*g_pFullFileSystem : nullptr);
		SDK_Log("  FileSystem:   %p\n", (void*)g_pFileSystem_Stdio);
		SDK_Log("  KeyValues:    %p\n", (void*)g_pKeyValuesSystem);
		SDK_Log("  WindowProc:   %s\n", CGame__WindowProc ? "FOUND" : "NOT FOUND");
	}

	//=========================================================================
	// Platform globals (Ebisu). Origin mode unblocks connect/map.
	// g_platState @ 0x74F5198, g_platAuxState @ 0x955EC10, g_bPlatAuxReady @ 0x955EDDD.
	//=========================================================================
	SDK_TRACE("Setting platform globals (Ebisu)...");
	{
		int*     p_platState     = reinterpret_cast<int*>(NetObs_Sym(NetObsSym_t::PlatState));
		int*     p_platAuxState    = reinterpret_cast<int*>(NetObs_Sym(NetObsSym_t::PlatAuxState));
		uint8_t* p_platAuxReady    = reinterpret_cast<uint8_t*>(NetObs_Sym(NetObsSym_t::PlatAuxReady));

		if (!p_platState || !p_platAuxState || !p_platAuxReady)
		{
			SDK_Log("ERROR: platform globals unresolved (%p %p %p); Origin platform mode left untouched\n",
				(void*)p_platState, (void*)p_platAuxState, (void*)p_platAuxReady);
		}
		else
		{
			SDK_Log("Platform globals BEFORE:\n");
			SDK_Log("  g_platState     = %d (want 1=origin)\n", *p_platState);
			SDK_Log("  g_platAuxState    = %d (unused at mode=1; leave 0)\n", *p_platAuxState);
			SDK_Log("  g_bPlatAuxReady   = %d (unused at mode=1; leave 0)\n", *p_platAuxReady);

			// Platform mode must be Origin (1). Mode 2 uses an alt allocator we never init.
			*p_platState     = 1;  // PC_PLATFORM_ORIGIN
			*p_platAuxState    = 0;  // alt platform inactive
			*p_platAuxReady    = 0;  // alt platform inactive

			SDK_Log("Platform globals SET: platState=1 (Origin only), platAuxState=0, platAuxReady=0\n");
		}
	}

	//=========================================================================
	// NOP OriginGetSettingSync's Error() so one-time Origin init still sets the entity-pool gate.
	// Leave the periodic poll hooked by VEbisuSDK; do not stub it here.
	//=========================================================================
	SDK_TRACE("Patching Origin platform (NOP fatal + silence poll)...");
	{
		uint8_t* pFatal = reinterpret_cast<uint8_t*>(NetObs_Sym(NetObsSym_t::OriginInitFatalCall));
		DWORD oldProt;
		if (pFatal && pFatal[0] == 0xE8 && VirtualProtect(pFatal, 5, PAGE_EXECUTE_READWRITE, &oldProt))
		{
			pFatal[0] = 0x90; // nop
			pFatal[1] = 0x90;
			pFatal[2] = 0x90;
			pFatal[3] = 0x90;
			pFatal[4] = 0x90;
			VirtualProtect(pFatal, 5, oldProt, &oldProt);
			SDK_Log("Origin init fatal Error() call NOPed (%p)\n", (void*)pFatal);
		}

		// Origin poll stays intact. Byte-stubbing to `ret` first made DetourTransactionCommit fail.
		if (IsOriginDisabled())
		{
			SDK_Log("Offline mode -- Origin poll left intact for VEbisuSDK "
					"(HEbisuSDK_RunFrame no-ops the native body).\n");
		}
		else
		{
			SDK_Log("Online mode -- Origin poll left LIVE for native EA/Origin identity "
					"(connects to running EA App over LSX -> real Nucleus id + persona).\n");
		}

		// NOP LauncherMain's platState zeroing so platState=1 survives -noorigin.
		uint8_t* pZero = reinterpret_cast<uint8_t*>(NetObs_Sym(NetObsSym_t::PatchSite_LauncherPlatStateZero));
		if (pZero && pZero[0] == 0x44 && pZero[1] == 0x89 && pZero[2] == 0x35
			&& VirtualProtect(pZero, 7, PAGE_EXECUTE_READWRITE, &oldProt))
		{
			memset(pZero, 0x90, 7); // NOP x7
			VirtualProtect(pZero, 7, oldProt, &oldProt);
			SDK_Log("LauncherMain platState zeroing NOPed (%p, 7 bytes)\n", (void*)pZero);
		}

		// Neuter playlist-rules loader (rules_Win64.rson HTTP blocks D3D11 ~30s).
		uint8_t* pRules = reinterpret_cast<uint8_t*>(NetObs_Sym(NetObsSym_t::PlaylistRulesLoader));
		if (pRules)
		{
			if (pRules[0] == 0x48 && pRules[1] == 0x8B && pRules[2] == 0xC4
				&& VirtualProtect(pRules, 3, PAGE_EXECUTE_READWRITE, &oldProt))
			{
				pRules[0] = 0x33; // xor eax, eax
				pRules[1] = 0xC0;
				pRules[2] = 0xC3; // ret
				VirtualProtect(pRules, 3, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), pRules, 3);
				SDK_Log("Online playlist-rules loader (rules_Win64.rson) neutered (%p -> xor eax,eax;ret)\n", (void*)pRules);
			}
			else
			{
				SDK_Log("Rules loader patch: prologue mismatch @ %p (got %02X %02X %02X) -- NOT applied\n",
					(void*)pRules, pRules[0], pRules[1], pRules[2]);
			}
		}

		Origin_InstallOfflineGuard();
		Origin_InstallIgoGuard();

		extern bool InputSystem_ResolveSingletonFromEngine();
		InputSystem_ResolveSingletonFromEngine();
		SDK_Log("  g_pInputSystem = %p (expected NULL this early; retried later)\n", (void*)g_pInputSystem);
	}

	//=========================================================================
	// EOS launcher gate: 74 3F jz -> EB jmp. This client never participates in EAC.
	//=========================================================================
	SDK_TRACE("EOS anti-cheat launcher gate...");
	{
		uint8_t* const gate = reinterpret_cast<uint8_t*>(NetObs_Sym(NetObsSym_t::PatchSite_EosLauncherGate));

		DWORD oldProtect = 0;
		if (!gate)
		{
			SDK_Log("[SEC-BYPASS] EOS launcher gate unresolved -- skipped\n");
		}
		else if (gate[0] == 0xEB)
		{
			SDK_Log("[SEC-BYPASS] EOS launcher gate already patched at %p\n",
				(void*)gate);
			Msg(eDLL_T::ENGINE,
				"[SEC-BYPASS] EOS launcher gate ACTIVE (already patched @ %p)\n",
				(void*)gate);
		}
		else if (gate[0] == 0x74 && VirtualProtect(gate, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			gate[0] = 0xEB;
			VirtualProtect(gate, 1, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), gate, 1);
			SDK_Log("[SEC-BYPASS] EOS launcher gate PATCHED at %p (jz -> jmp)\n",
				(void*)gate);
			Msg(eDLL_T::ENGINE,
				"[SEC-BYPASS] EOS launcher gate ACTIVE @ %p\n",
				(void*)gate);
		}
		else
		{
			SDK_Log("ERROR: EOS anti-cheat launcher gate @ %p expected 0x74, found 0x%02X -- NOT APPLIED\n",
				(void*)gate, gate[0]);
		}
	}

	//=========================================================================
	// Unconditional jmp past the platform-mode/session gate so connect runs without a matchmaking session.
	// ja rel32 at dispatcher+0x9E -> jmp rel32; nop (same landing).
	//=========================================================================
	SDK_TRACE("Patching connect dispatcher platform-mode gate...");
	{
		CMemory dispatcherMem = Module_FindPattern(g_GameDll,
			"48 89 5C 24 18 55 57 41 56 48 81 EC 40 04 00 00 48 8B 2A 48 8B F9 48 8D 0D ?? ?? ?? ?? 4C 8B F2");
		if (!dispatcherMem.GetPtr())
		{
			SDK_Log("WARNING: connect dispatcher NOT FOUND - pattern mismatch. Connect may hang.\n");
		}
		else
		{
			BYTE* const pDispatcher = reinterpret_cast<BYTE*>(dispatcherMem.GetPtr());
			BYTE* const pPatchSite = pDispatcher + 0x9E;

			// Defense in depth: verify the bytes are what we expect
			// (`0F 87 97 00 00 00` = ja rel32) before overwriting.
			const BYTE expected[6] = { 0x0F, 0x87, 0x97, 0x00, 0x00, 0x00 };
			bool matches = true;
			for (int i = 0; i < 6; ++i)
			{
				if (pPatchSite[i] != expected[i])
				{
					matches = false;
					break;
				}
			}

			if (!matches)
			{
				SDK_Log("WARNING: connect dispatcher gate bytes do not match expected"
						" ja 0x97000000 sequence at %p:\n", (void*)pPatchSite);
				SDK_Log("  got: %02X %02X %02X %02X %02X %02X\n",
					pPatchSite[0], pPatchSite[1], pPatchSite[2],
					pPatchSite[3], pPatchSite[4], pPatchSite[5]);
				SDK_Log("  Skipping patch. Connect may hang in eternal black screen.\n");
			}
			else
			{
				DWORD oldProtect = 0;
				if (VirtualProtect(pPatchSite, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
				{
					// ja +0x97 is 6 bytes; jmp is 5, so disp grows by 1 to hit the same target.
					pPatchSite[0] = 0xE9;  // jmp rel32
					pPatchSite[1] = 0x98;  // disp32 byte 0
					pPatchSite[2] = 0x00;  // disp32 byte 1
					pPatchSite[3] = 0x00;  // disp32 byte 2
					pPatchSite[4] = 0x00;  // disp32 byte 3
					pPatchSite[5] = 0x90;  // nop (leftover byte)

					DWORD tmp = 0;
					VirtualProtect(pPatchSite, 6, oldProtect, &tmp);

					SDK_Log("Connect dispatcher platform-mode gate PATCHED at %p\n",
						(void*)pPatchSite);
				}
				else
				{
					SDK_Log("ERROR: VirtualProtect failed on connect dispatcher patch site %p\n",
						(void*)pPatchSite);
				}
			}
		}
	}

	//=========================================================================
	// Force net_usesocketsforloopback=1 or loopback connect never hits sendto.
	//=========================================================================
	SDK_TRACE("Forcing net_usesocketsforloopback = 1...");
	{
		int* const pIntValue = reinterpret_cast<int*>(NetObs_Sym(NetObsSym_t::UseSocketsForLoopbackIntValue));

		DWORD oldProtect = 0;
		if (pIntValue && VirtualProtect(pIntValue, sizeof(int), PAGE_READWRITE, &oldProtect))
		{
			const int prevValue = *pIntValue;
			*pIntValue = 1;
			const int newValue = *pIntValue;
			DWORD tmp = 0;
			VirtualProtect(pIntValue, sizeof(int), oldProtect, &tmp);

			SDK_Log("net_usesocketsforloopback @ %p: %d -> %d (loopback shortcut bypassed)\n",
				(void*)pIntValue, prevValue, newValue);
			if (newValue != 1)
			{
				SDK_Log("  WARNING: write did not take effect. Connect to 127.0.0.1 will still loop back.\n");
			}
		}
		else
		{
			SDK_Log("ERROR: VirtualProtect failed on net_usesocketsforloopback site %p (err=%lu)\n",
				(void*)pIntValue, GetLastError());
		}
	}

	//=========================================================================
	// Disable S21 per-packet AES-GCM. ConVar int value at object+0x64.
	//=========================================================================
	SDK_TRACE("Disabling DTLS encryption (net_encrypt_dtls/multiKey/copyCtx = 0)...");
	{
		struct EncryptCvar
		{
			const char* name;
			NetObsSym_t intValue;
		};
		const EncryptCvar cvars[] = {
			{ "net_encrypt_dtls",     NetObsSym_t::EncryptDtlsIntValue },
			{ "net_encrypt_multiKey", NetObsSym_t::EncryptMultiKeyIntValue },
			{ "net_encrypt_copyCtx",  NetObsSym_t::EncryptCopyCtxIntValue },
		};

		for (const EncryptCvar& encCvar : cvars)
		{
			int* const pInt = reinterpret_cast<int*>(NetObs_Sym(encCvar.intValue));
			DWORD oldProtect = 0;
			if (pInt && VirtualProtect(pInt, sizeof(int), PAGE_READWRITE, &oldProtect))
			{
				const int prev = *pInt;
				*pInt = 0;
				const int now = *pInt;
				DWORD tmp = 0;
				VirtualProtect(pInt, sizeof(int), oldProtect, &tmp);
				SDK_Log("%-22s @ %p: %d -> %d\n", encCvar.name, (void*)pInt, prev, now);
			}
			else
			{
				SDK_Log("ERROR: VirtualProtect failed for %s @ %p (err=%lu)\n",
					encCvar.name, (void*)pInt, GetLastError());
			}
		}
	}

	//=========================================================================
	// Emit S3 C2S_CHALLENGE: type 0x48, bit-count lea keeps r8d=8, encryption flags=0.
	//=========================================================================
	SDK_TRACE("Patching connect-challenge builder (type byte + encryption)...");
	{
		struct PatchSite {
			const char* desc;
			NetObsSym_t site;
			uint8_t from;
			uint8_t to;
		};
		const PatchSite patches[] = {
			{ "type byte 0x0A->0x48",        NetObsSym_t::PatchSite_ChallengeTypeByte, 0x0A, 0x48 },
			{ "lea disp  0xFE->0xC0",        NetObsSym_t::PatchSite_ChallengeLeaDisp, 0xFE, 0xC0 },
			{ "encryption flags 0x02->0x00", NetObsSym_t::PatchSite_ChallengeEncryptFlags, 0x02, 0x00 },
			{ "OOB dispatch 0x49->handler1 (S2C_CHALLENGE)", NetObsSym_t::PatchSite_OobDispatch49, 0x07, 0x01 },
		};
		int ok = 0;
		for (const auto& p : patches)
		{
			uint8_t* const addr = reinterpret_cast<uint8_t*>(NetObs_Sym(p.site));
			if (!addr)
			{
				SDK_Log("  %s: site unresolved -- skipped\n", p.desc);
				continue;
			}
			if (*addr != p.from)
			{
				SDK_Log("ERROR: %s @ %p: expected 0x%02X, found 0x%02X -- NOT APPLIED\n",
					p.desc, (void*)addr, p.from, *addr);
				continue;
			}
			DWORD oldProtect = 0;
			if (VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				*addr = p.to;
				DWORD tmp = 0;
				VirtualProtect(addr, 1, oldProtect, &tmp);
				SDK_Log("PATCHED %s @ %p\n", p.desc, (void*)addr);
				++ok;
			}
			else
			{
				SDK_Log("ERROR: VirtualProtect failed for %s @ %p (err=%lu)\n",
					p.desc, (void*)addr, GetLastError());
			}
		}
		SDK_Log("Connect-challenge patches: %d/%d applied\n", ok, (int)SDK_ARRAYSIZE(patches));
	}

	//=========================================================================
	// NOP S2C_CHALLENGE sequence (dl) and cl_failremoteconnections rejects.
	//=========================================================================
	SDK_TRACE("Patching S2C_CHALLENGE handler rejection checks...");
	{
		struct NopPatch {
			const char* desc;
			NetObsSym_t site;
			size_t len;
			uint8_t expected[8]; // first N bytes must match
		};
		const NopPatch nops[] = {
			{ "dl check #1 NOP (case 4 entry)", NetObsSym_t::PatchSite_ChallengeDlCheck1, 8, { 0x84, 0xD2, 0x0F, 0x84, 0x89, 0x11, 0x00, 0x00 } },
			{ "dl check #2 NOP", NetObsSym_t::PatchSite_ChallengeDlCheck2, 8, { 0x84, 0xD2, 0x0F, 0x84, 0xCF, 0x0B, 0x00, 0x00 } },
			{ "ConVar check NOP", NetObsSym_t::PatchSite_ChallengeConVarCheck, 6, { 0x0F, 0x85, 0xBF, 0x0B, 0x00, 0x00 } },
		};
		int ok = 0;
		for (const auto& p : nops)
		{
			uint8_t* const addr = reinterpret_cast<uint8_t*>(NetObs_Sym(p.site));
			if (!addr)
			{
				SDK_Log("  %s: site unresolved -- skipped\n", p.desc);
				continue;
			}
			bool match = true;
			for (size_t i = 0; i < p.len; i++)
			{
				if (addr[i] != p.expected[i]) { match = false; break; }
			}
			if (!match)
			{
				SDK_Log("ERROR: %s @ %p: byte mismatch -- NOT APPLIED\n", p.desc, (void*)addr);
				continue;
			}
			DWORD oldProtect = 0;
			if (VirtualProtect(addr, p.len, PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				memset(addr, 0x90, p.len); // NOP sled
				DWORD tmp = 0;
				VirtualProtect(addr, p.len, oldProtect, &tmp);
				SDK_Log("PATCHED %s @ %p (%zu bytes)\n", p.desc, (void*)addr, p.len);
				++ok;
			}
			else
			{
				SDK_Log("ERROR: VirtualProtect failed for %s @ %p (err=%lu)\n",
					p.desc, (void*)addr, GetLastError());
			}
		}
		SDK_Log("S2C_CHALLENGE handler rejection patches: %d/3 applied\n", ok);
	}

	//=========================================================================
	// S2C_CHALLENGE: force conn-type flags, skip protocol hash, jmp past bitstream exhaust, NOP re-entry jz.
	//=========================================================================
	SDK_TRACE("Patching S2C_CHALLENGE handler conn type + protocol hash...");
	{
		struct BytePatch {
			const char* desc;
			NetObsSym_t site;
			const uint8_t* expected;
			const uint8_t* replacement;
			size_t len;
		};

		static const uint8_t connExpected[] = { 0x0F, 0x85, 0x91, 0x07, 0x00, 0x00 };
		static const uint8_t connReplace[]  = { 0x41, 0xB3, 0x01, 0x40, 0xB7, 0x01 };

		static const uint8_t hashExpected[] = { 0x0F, 0x85, 0x4B, 0xFD, 0xFF, 0xFF };
		static const uint8_t hashReplace[]  = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

		static const uint8_t exhaustExpected[] = { 0x74, 0x0A };
		static const uint8_t exhaustReplace[]  = { 0xEB, 0x0A };

		static const uint8_t reentryExpected[] = { 0x0F, 0x84, 0x1B, 0x06, 0x00, 0x00 };
		static const uint8_t reentryReplace[]  = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

		const BytePatch patches[] = {
			{ "conn type -> mov r11b,1", NetObsSym_t::PatchSite_ChallengeConnType, connExpected, connReplace, 6 },
			{ "protocol hash jnz NOP",   NetObsSym_t::PatchSite_ChallengeProtocolHash, hashExpected, hashReplace, 6 },
			{ "exhaust jz->jmp",         NetObsSym_t::PatchSite_ChallengeExhaust, exhaustExpected, exhaustReplace, 2 },
			{ "re-entry guard jz NOP",   NetObsSym_t::PatchSite_ChallengeReentry, reentryExpected, reentryReplace, 6 },
		};
		int ok = 0;
		for (const auto& p : patches)
		{
			uint8_t* const addr = reinterpret_cast<uint8_t*>(NetObs_Sym(p.site));
			if (!addr)
			{
				SDK_Log("  %s: site unresolved -- skipped\n", p.desc);
				continue;
			}
			bool match = true;
			for (size_t i = 0; i < p.len; i++)
			{
				if (addr[i] != p.expected[i]) { match = false; break; }
			}
			if (!match)
			{
				SDK_Log("ERROR: %s @ %p: byte mismatch -- NOT APPLIED\n", p.desc, (void*)addr);
				continue;
			}
			DWORD oldProtect = 0;
			if (VirtualProtect(addr, p.len, PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				memcpy(addr, p.replacement, p.len);
				DWORD tmp = 0;
				VirtualProtect(addr, p.len, oldProtect, &tmp);
				SDK_Log("PATCHED %s @ %p\n", p.desc, (void*)addr);
				++ok;
			}
			else
			{
				SDK_Log("ERROR: VirtualProtect failed for %s @ %p (err=%lu)\n",
					p.desc, (void*)addr, GetLastError());
			}
		}
		SDK_Log("S2C_CHALLENGE advanced patches: %d/%d applied\n", ok, (int)(sizeof(patches)/sizeof(patches[0])));
	}
	// S3 pattern: 48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 63 D9 41 8B F8 48 8D 0D
	//=========================================================================
	SDK_TRACE("Finding Cbuf_AddText...");
	{
		CMemory cbufMem = Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 63 D9 41 8B F8 48 8D 0D");
		if (cbufMem.GetPtr())
		{
			SDK_Log("Cbuf_AddText found at: 0x%llX\n", (unsigned long long)cbufMem.GetPtr());
			cbufMem.GetPtr(Cbuf_AddText);
		}
		else
		{
			SDK_Log("WARNING: Cbuf_AddText NOT FOUND (S3 pattern failed, need S21 pattern)\n");
		}
	}

	SDK_Log("\n");
	SDK_Log("+-------------------------------------------------------------+\n");
	SDK_Log("| R5Flowstate initialized                                    |\n");
	SDK_Log("+-------------------------------------------------------------+\n");
	SDK_Log("\n");
	SDK_Log("Game DLL base: 0x%p\n", (void*)g_GameDll.GetModuleBase());
	SDK_Log("Game DLL size: 0x%llX\n", (unsigned long long)g_GameDll.GetModuleSize());
	SDK_Log("SDK  DLL base: 0x%p\n", (void*)g_SDKDll.GetModuleBase());
	SDK_Log("Platform:      READY (Origin mode)\n");
	SDK_Log("Cbuf_AddText:  %s\n", Cbuf_AddText ? "FOUND" : "NOT FOUND");
	SDK_Log("\n");

	//=========================================================================
	// Install observer hooks only. Do not Cbuf_Execute here -- SNS reinit deadlocks SDK_InitThread.
	//=========================================================================
	SDK_TRACE("Installing NetObserver...");
	if (!NetObs_ResolveSymbols())
	{
		Error(eDLL_T::CLIENT, NO_ERROR,
			"[NETOBS-SYM] unresolved %zu symbol(s) -- skipping NetObserver_Install "
			"and net-dependent Systems_Init_S21\n",
			NetObs_UnresolvedCount());
	}
	else
	{
		SdkStage_Run("NetObserver", SDK_Stage_NetObserver_Install);

		SDK_TRACE("Systems_Init_S21 (crash-safe detour init)...");
		SdkStage_Run("Systems_Init_S21", Systems_Init_S21);
	}

	SDK_TRACE("Installing platform script disk redirect...");
	SdkStage_Run("ScriptDiskRedirect", InstallScriptDiskRedirectHooks_S21);

	SDK_InitArmOverlay();

	// Start console command input thread
	if (Cbuf_AddText)
	{
		// Do NOT set "developer 1" -- it loads sh_dev_items.gnut and crashes the VM.

		// Start input worker using Win32 ReadConsole (not std::cin which
		// has issues with freopen'd consoles on injected threads)
		CreateThread(NULL, 0, [](LPVOID) -> DWORD {
			char buf[1024];
			HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
			while (true)
			{
				fputs("] ", stdout);
				fflush(stdout);

				DWORD bytesRead = 0;
				const BOOL ok = (GetFileType(hIn) == FILE_TYPE_PIPE)
					? ReadFile(hIn, buf, sizeof(buf) - 2, &bytesRead, NULL)
					: ReadConsoleA(hIn, buf, sizeof(buf) - 2, &bytesRead, NULL);
				if (!ok)
					break;

				// Strip trailing \r\n
				while (bytesRead > 0 && (buf[bytesRead-1] == '\n' || buf[bytesRead-1] == '\r'))
					bytesRead--;
				buf[bytesRead] = '\n';
				buf[bytesRead + 1] = '\0';

				if (bytesRead > 0 && Cbuf_AddText)
				{
					Cbuf_AddText(ECommandTarget_t::CBUF_FIRST_PLAYER,
								 buf, cmd_source_t::kCommandSrcCode);
				}
			}
			return 0;
		}, NULL, 0, NULL);
	}

	g_bSdkInitialized = true;
	SDK_TRACE("SDK_Init complete");
}

//#############################################################################
// SHUTDOWN
//#############################################################################

void SDK_Shutdown()
{
	assert(g_bSdkInitialized);

	// Also check CPU in shutdown, since this function is exported, if they
	// call this with an unsupported CPU we should let them know rather than
	// crashing the process.
	CheckSystemCPU();

	if (g_bSdkShutdownCallInitiated)
	{
		spdlog::error("Recursive shutdown!\n");
		return;
	}

	g_bSdkShutdownCallInitiated = true;

	if (!g_bSdkInitialized)
	{
		spdlog::error("Not initialized!\n");
		return;
	}

	Msg(eDLL_T::NONE, "SDK shutdown initiated\n");

	// Overlay AddLog is dead after WinSys_Shutdown; Detach/CRT still call SDK_Log.
	g_bSdkObserveInit = false;

	ModSystem()->Shutdown();

	Input_Shutdown();

	WinSys_Shutdown();
	Systems_Shutdown();

	DirtySDK_Shutdown();
	Winsock_Shutdown();

	curl_global_cleanup();
	google::protobuf::ShutdownProtobufLibrary();

	SpdLog_Shutdown();

	// If the shutdown was initiated from the console window itself, don't
	// shutdown the console as it would otherwise deadlock in FreeConsole!
	if (s_bConsoleInitialized && !g_bSdkShutdownInitiatedFromConsoleHandler)
		Console_Shutdown();

	g_bSdkInitialized = false;
}

//#############################################################################
// ENTRYPOINT
//#############################################################################

// Loader detours LauncherMain and calls SDK_Init before the engine runs.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	NOTE_UNUSED(lpReserved);

	switch (dwReason)
	{
		case DLL_PROCESS_ATTACH:
		{
			s_hModuleHandle = hModule;
			DisableThreadLibraryCalls(hModule);

			// NOP .nut strstr rejection before the UI VM starts.
			{
				HMODULE hExe = GetModuleHandleA(NULL);
				if (hExe)
				{
					uintptr_t exeBase = reinterpret_cast<uintptr_t>(hExe);
					const bool dx12Exe = SDK_IsDx12Exe();

					uint8_t* site1 = reinterpret_cast<uint8_t*>(
						exeBase + (dx12Exe ? 0x46A580 : 0x44EB00));
					uint8_t* site2 = reinterpret_cast<uint8_t*>(
						exeBase + (dx12Exe ? 0x46A598 : 0x44EB18));

					DWORD oldProt1 = 0, oldProt2 = 0;
					if (site1[0] == 0x0F && site1[1] == 0x85)
					{
						VirtualProtect(site1, 6, PAGE_EXECUTE_READWRITE, &oldProt1);
						memset(site1, 0x90, 6);
						VirtualProtect(site1, 6, oldProt1, &oldProt1);
					}
					if (site2[0] == 0x0F && site2[1] == 0x85)
					{
						VirtualProtect(site2, 6, PAGE_EXECUTE_READWRITE, &oldProt2);
						memset(site2, 0x90, 6);
						VirtualProtect(site2, 6, oldProt2, &oldProt2);
					}
					FlushInstructionCache(GetCurrentProcess(), site1, 6);
					FlushInstructionCache(GetCurrentProcess(), site2, 6);
				}
			}

			SdkStage_Run("PreInit_CompileBuffer",
				VSquirrelS21Core_PreInit_AttachCompileBufferHook);

			break;
		}
		case DLL_PROCESS_DETACH:
		{
			if (g_bSdkInitialized)
				SDK_Shutdown();
			s_hModuleHandle = NULL;
			break;
		}
	}

	return TRUE;
}
