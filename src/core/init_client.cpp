//=============================================================================//
//
// Purpose: S21 client SDK systems init -- detour classes for the client bridge.
//
//=============================================================================//

#include "core/stdafx.h"
#include "core/logdef.h"
#include "core/init.h"
#include "core/detour_registry.h"
#include "core/detour_validator.h"
#include "core/bridge_ready.h"
#include "core/bridge_stats.h"
#include "core/sdk_stage.h"
#include "game/shared/heap_canary.h"
#include "tier0/jobthread.h"
#include "tier0/threadtools.h"
#include "tier0/tslist.h"
#include "tier0/memstd.h"
#include "tier0/fasttimer.h"
#include "tier0/commandline.h"
#include "tier0/platform_internal.h"
#include "tier0/sigcache.h"
#include "tier1/cmd.h"
#include "tier1/cvar.h"
#include "tier1/keyvalues_iface.h"
#include "tier2/renderutils.h"
#include "vpc/IAppSystem.h"
#include "vpc/interfaces.h"
#include "common/callback.h"
#include "common/completion.h"
#include "vstdlib/keyvaluessystem.h"
#include "common/opcodes.h"
#include "common/netmessages.h"
#include "launcher/prx.h"
#include "launcher/launcher.h"
#include "filesystem/basefilesystem.h"
#include "filesystem/filesystem.h"
#include "datacache/mdlcache.h"
#include "ebisusdk/EbisuSDK.h"

// Engine
#include "engine/common.h"
#include "engine/host.h"
#include "engine/host_cmd.h"
#include "engine/host_state.h"
#include "engine/cmd.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "engine/client/net_observer.h"
#include "engine/client/host_frame_probe.h"
#include "engine/splitpacket_recv_clamp.h"
#include "rtech/vfx_alias.h"
#include "engine/cmodel_surfdata.h"
#include "game/client/body_skin.h"
#include "game/client/weapon_mod_visual.h"
#include "game/client/classvar_natives.h"
#include "game/client/fs_1v1_convars.h"
#include "game/client/fov_limit.h"
#include "game/client/visual_clutter.h"
#include "game/client/mantle_boost_rui.h"
#include "game/shared/pose_param.h"
#include "datacache/anim_desc.h"
#include "materialsystem/shader_teardown.h"
#include "materialsystem/texture_stream_abort_free.h"
#include "rtech/efct_child_link.h"
#include "datacache/studio_seqdesc.h"
#include "engine/sys_integrity.h"
#include "codecs/miles/miles_banklist.h"
#include "rtech/pak/pak_lobby_world.h"
#include "engine/sys_dll.h"
#include "engine/sys_dll2.h"
#include "engine/sys_engine.h"
#include "engine/sys_utils.h"

// Squirrel
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/sqstdaux.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/vscript.h"
#include "vscript/vscript_s21_override.h"
#include "vscript/weapon_kv_disk.h"
#include "localize/localize_disk.h"
#include "vscript/vsquirrel_s21.h"
#include "rtech/pak/pakparse.h"
#include "rtech/datatable/datatable.h"
#include "rtech/pak/rpak_observe.h"
#include "rtech/pak/pak_census.h"
#include "rtech/async/asyncio.h"
#include "rtech/pak/rpak_sigbypass.h"
#include "rtech/pak/pak_opt_stream_drop.h"
#include "rtech/pak/settings_disk.h"
#include "engine/stringtable_diag.h"
#include "engine/skinnames_stub.h"
#include "engine/weapon_precache_redirect.h"
#include "engine/mdl_precache_client_grow.h"
#include "engine/staticpropmgr.h"
#include "engine/modelloader.h"
#include "engine/cmodel_bsp_debug.h"
#include "engine/debugoverlay.h"

// Game
#include "game/shared/usercmd.h"
#include "game/shared/activity.h"
#include "game/shared/util_shared.h"
#include "game/client/pred_diag.h"
#include "game/client/grapple_rope_diag.h"
#include "game/client/mantle_boost.h"
#include "game/client/trigger_cannon.h"
#include "game/client/halfduck_zip_parity.h"
#include "game/client/move_sim_trace.h"
#include "game/client/melee_lunge_probe.h"
#include "game/client/melee_activity_trace.h"
#include "game/client/bridge_cmd_seed.h"
#include "game/client/bridge_fire_tap.h"
#include "game/client/jumppad_viewpunch_diag.h"
#include "game/client/ruitracks.h"
#include "game/client/hud_basechat.h"
#include "rtech/rui/rui.h"

// Public
#include "public/edict.h"
#include "game/shared/globalnonrewind_vars.h"

// Rendering / ImGui
#include "windows/id3dx.h"
#include "windows/pso_cache.h"
#include "engine/sys_mainwind.h"
#include "engine/client/bridge_flag_set.h"
#include "engine/client/clientstate.h"
#include "materialsystem/cmaterialsystem.h"
#include "inputsystem/inputsystem.h"
#include "vgui/vgui_baseui_interface.h"
#include "vguimatsurface/MatSystemSurface.h"

/////////////////////////////////////////////////////////////////////////////////////////////////
//
// INITIALIZATION
//
/////////////////////////////////////////////////////////////////////////////////////////////////

void ScriptConstantRegistrationCallback(CSquirrelVM* s)
{
	// TODO: Register S21 script constants as needed
}

//////////////////////////////////////////////////////////////////////////
// Per-class SEH isolation: one GetCon/GetFun/GetVar failure cannot break others.
//////////////////////////////////////////////////////////////////////////

extern void SDK_LogDevFile(const char* fmt, ...);

// High-resolution timer for Phase A/C timing
static LARGE_INTEGER s_PerfFreq = {};
static uint64_t GetMicroseconds()
{
	if (s_PerfFreq.QuadPart == 0)
		QueryPerformanceFrequency(&s_PerfFreq);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return static_cast<uint64_t>((now.QuadPart * 1000000ULL) / s_PerfFreq.QuadPart);
}

// Log the class name before each call so a hard crash still names the last class.
static void Systems_Init_S21_PhaseA_Scan()
{
	const size_t total = DetourRegistry_Size();
	SDK_LogDevFile("Phase A: Pattern scanning (%zu classes)\n", total);

	const auto& registry = DetourRegistry_GetAll();
	for (size_t i = 0; i < registry.size(); ++i)
	{
		DetourEntry& entry = const_cast<DetourEntry&>(registry[i]);

		// Skip already-gated entries (SkippedByConfig, SkippedByFlag)
		if (entry.state != DetourState::NotAttempted)
			continue;

		const uint64_t t0 = GetMicroseconds();

		__try
		{
			entry.instance->GetCon();
			entry.instance->GetFun();
			entry.instance->GetVar();
			entry.state = DetourState::ScanSucceeded;
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			entry.state = DetourState::ScanFailed;
			entry.exceptionCode = GetExceptionCode();
			entry.failureReason = "exception during GetCon/GetFun/GetVar";
		}

		entry.scanTimeUs = GetMicroseconds() - t0;

		if (entry.state == DetourState::ScanFailed)
		{
			SDK_LogDevFile("  [%2zu/%2zu] %-32s FAIL (code=0x%08X, %llu us)\n",
				i + 1, total, entry.name,
				entry.exceptionCode, (unsigned long long)entry.scanTimeUs);
		}
		else
		{
			SDK_LogDevFile("  [%2zu/%2zu] %-32s OK   (%llu us)\n",
				i + 1, total, entry.name,
				(unsigned long long)entry.scanTimeUs);
		}
	}

	const size_t scanned = DetourRegistry_CountByState(DetourState::ScanSucceeded);
	const size_t failed  = DetourRegistry_CountByState(DetourState::ScanFailed);
	uint64_t scanUs = 0;
	for (const DetourEntry& e : registry)
		scanUs += e.scanTimeUs;
	SDK_LogDevFile("Phase A complete: %zu succeeded, %zu failed, %llu us total\n",
		scanned, failed, (unsigned long long)scanUs);
}

// Phase B: pre-attach validator. Null targets fail DetourSetup; LogFunAdr is a no-op here.
static bool Systems_Init_S21_PreAttachValidate(void* pFn, const char** ppszReason)
{
	const DetourValidationResult r =
		DetourValidator_ValidateFunctionAddress(reinterpret_cast<uintptr_t>(pFn));
	if (!r.valid)
	{
		if (ppszReason)
			*ppszReason = r.reason ? r.reason : "DetourValidator rejected";
		return false;
	}
	return true;
}

static void Systems_Init_S21_DetourLogSink(const char* const pszMsg)
{
	Warning(eDLL_T::COMMON, "%s", pszMsg ? pszMsg : "[DETOUR] (null message)\n");
}

static void Systems_Init_S21_PhaseB_Validate()
{
	g_DetourLogSink = &Systems_Init_S21_DetourLogSink;
	g_DetourPreAttachValidate = &Systems_Init_S21_PreAttachValidate;
	SDK_LogDevFile("Phase B: Validation (pre-attach DetourValidator wired; null targets fail DetourSetup)\n");

	for (DetourEntry& entry : const_cast<std::vector<DetourEntry>&>(DetourRegistry_GetAll()))
	{
		if (entry.state == DetourState::ScanSucceeded)
			entry.state = DetourState::Validated;
	}
}

// Phase C: one transaction per class; failure rolls back only that class.
static void Systems_Init_S21_PhaseC_Attach()
{
	const size_t total = DetourRegistry_Size();
	SDK_LogDevFile("Phase C: Attaching hooks (per-class transactions)\n");
	Detour_ResetAttachTargets();

	const auto& registry = DetourRegistry_GetAll();
	for (size_t i = 0; i < registry.size(); ++i)
	{
		DetourEntry& entry = const_cast<DetourEntry&>(registry[i]);

		if (entry.state != DetourState::Validated)
		{
			// Only log skips for classes that scanned OK but were gated out --
			// the common case (already-failed in Phase A) would spam the log.
			continue;
		}

		const uint64_t t0 = GetMicroseconds();

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		Detour_ResetNullSkipCount();

		bool attached = false;
		Detour_SetAttachingClass(entry.name);
		__try
		{
			entry.instance->Detour(true);
			attached = true;
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			entry.state = DetourState::HookFailed;
			entry.exceptionCode = GetExceptionCode();
			entry.failureReason = "exception during Detour(true)";
		}

		const LONG nullSkips = Detour_ConsumeNullSkipCount();

		if (!attached)
		{
			// Abort the transaction without committing
			DetourTransactionAbort();
			entry.hookTimeUs = GetMicroseconds() - t0;
			SDK_LogDevFile("  [%2zu/%2zu] %-32s FAIL (code=0x%08X, %llu us, aborted)\n",
				i + 1, total, entry.name,
				entry.exceptionCode, (unsigned long long)entry.hookTimeUs);
			continue;
		}

		const LONG hr = DetourTransactionCommit();
		entry.hookTimeUs = GetMicroseconds() - t0;

		if (hr == NO_ERROR)
		{
			// Null DetourSetup skips still let commit succeed; surface them.
			if (nullSkips > 0)
			{
				entry.state = DetourState::Hooked;
				entry.failureReason = "partial: null/invalid DetourSetup skip(s)";
				SDK_LogDevFile("  [%2zu/%2zu] %-32s OK*  (%llu us, null_skips=%ld)\n",
					i + 1, total, entry.name,
					(unsigned long long)entry.hookTimeUs, (long)nullSkips);
				Warning(eDLL_T::ENGINE,
					"[DETOUR] %s attached with %ld null/invalid target skip(s) "
					"-- some hooks never installed\n",
					entry.name, (long)nullSkips);
			}
			else
			{
				entry.state = DetourState::Hooked;
				SDK_LogDevFile("  [%2zu/%2zu] %-32s OK   (%llu us)\n",
					i + 1, total, entry.name,
					(unsigned long long)entry.hookTimeUs);
			}
		}
		else
		{
			// Commit failed -- try to detach what we attached
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			__try { entry.instance->Detour(false); }
			__except(EXCEPTION_EXECUTE_HANDLER) {}
			DetourTransactionCommit();

			entry.state = DetourState::HookFailed;
			entry.exceptionCode = static_cast<uint32_t>(hr);
			entry.failureReason = "DetourTransactionCommit failed";

			SDK_LogDevFile("  [%2zu/%2zu] %-32s FAIL (commit=%ld, %llu us, rolled back)\n",
				i + 1, total, entry.name,
				hr, (unsigned long long)entry.hookTimeUs);
		}
	}
	Detour_SetAttachingClass(nullptr);

	const size_t hooked = DetourRegistry_CountByState(DetourState::Hooked);
	const size_t failed = DetourRegistry_CountByState(DetourState::HookFailed);
	uint64_t hookUs = 0;
	for (const DetourEntry& e : registry)
		hookUs += e.hookTimeUs;
	SDK_LogDevFile("Phase C complete: %zu hooked, %zu failed, %llu us total\n",
		hooked, failed, (unsigned long long)hookUs);
}

void Systems_Init_S21()
{
	SDK_LogDevFile("========== Systems_Init_S21 ==========\n");

	SDK_LogDevFile("Calling DetourRegister() to populate g_DetourVec...\n");
	DetourRegister();
	SDK_LogDevFile("g_DetourVec now has %zu classes\n", g_DetourVec.size());

	FS1v1ConVars_Init();

	CFastTimer initTimer;
	initTimer.Start();
	DetourRegistry_LoadConfig("platform/cfg/sdk_detours.cfg");
	DetourRegistry_Build();
	initTimer.End();

	SDK_LogDevFile("+-------------------------------------------------------------+\n");
	SDK_LogDevFile("%-16s '%10.6f' seconds ('%12lu' clocks)\n",
		"Detour->InitDB()",
		initTimer.GetDuration().GetSeconds(), initTimer.GetDuration().GetCycles());

	initTimer.Start();
	Systems_Init_S21_PhaseA_Scan();
	Systems_Init_S21_PhaseB_Validate();
	Systems_Init_S21_PhaseC_Attach();
	initTimer.End();

	SDK_LogDevFile("%-16s '%10.6f' seconds ('%12lu' clocks)\n",
		"Detour->Attach()",
		initTimer.GetDuration().GetSeconds(), initTimer.GetDuration().GetCycles());
	SDK_LogDevFile("+-------------------------------------------------------------+\n");
	SDK_LogDevFile("\n");
	EbisuSDK_StubNativePollIfUnhooked();

	SdkStage_Run("RuntimePtc_Init", RuntimePtc_Init);

	HeapCanary::Checkpoint("sdk-init");

	// Final report
	DetourRegistry_PrintReport();

	{
		const size_t nScanFail  = DetourRegistry_CountByState(DetourState::ScanFailed);
		const size_t nHookFail  = DetourRegistry_CountByState(DetourState::HookFailed);
		const size_t nValFail   = DetourRegistry_CountByState(DetourState::ValidationFailed);

		if (nScanFail + nHookFail + nValFail > 0)
		{
			Warning(eDLL_T::CLIENT,
				"[BRIDGE-READY] ************************************************************\n");
			Warning(eDLL_T::CLIENT,
				"[BRIDGE-READY] ** DETOUR FAILURES -- scan/attach/validation short **\n");
			for (const DetourEntry& entry : DetourRegistry_GetAll())
			{
				if (entry.state != DetourState::ScanFailed &&
					entry.state != DetourState::HookFailed &&
					entry.state != DetourState::ValidationFailed)
					continue;

				Warning(eDLL_T::CLIENT,
					"[BRIDGE-READY] **   %s: %s%s%s\n",
					entry.name ? entry.name : "?",
					DetourState_ToString(entry.state),
					entry.failureReason ? " -- " : "",
					entry.failureReason ? entry.failureReason : "");
			}
			Warning(eDLL_T::CLIENT,
				"[BRIDGE-READY] ************************************************************\n");
		}
	}

	BridgeReady_Print("client");

	SDK_LogDevFile("========== Systems_Init_S21 done ==========\n");
}

//////////////////////////////////////////////////////////////////////////
//
// SHUTDOWN
//
//////////////////////////////////////////////////////////////////////////

void Systems_Shutdown()
{
	BridgeStats_PrintAndReset("shutdown");

	CFastTimer shutdownTimer;
	shutdownTimer.Start();

	// Begin the detour transaction to unhook the process
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

	// Unhook functions
	for (const IDetour* pd : g_DetourVec)
	{
		pd->Detour(false);
	}

	// Commit the transaction
	DetourTransactionCommit();

	shutdownTimer.End();
	Msg(eDLL_T::NONE, "+-------------------------------------------------------------+\n");
	Msg(eDLL_T::NONE, "%-16s '%10.6f' seconds ('%12lu' clocks)\n",
		"Detour->Detach()",
		shutdownTimer.GetDuration().GetSeconds(),
		shutdownTimer.GetDuration().GetCycles());
	Msg(eDLL_T::NONE, "+-------------------------------------------------------------+\n");
	Msg(eDLL_T::NONE, "\n");
}

static void CC_BridgeStats_Print(const CCommand& args)
{
	NOTE_UNUSED(args);
	BridgeStats_Print("console");
}
static ConCommand bridge_stats("bridge_stats", CC_BridgeStats_Print,
	"Print bridge session scoreboard counters (no reset).", FCVAR_RELEASE);

/////////////////////////////////////////////////////
//
// UTILITY
//
/////////////////////////////////////////////////////

void Winsock_Startup()
{
	WSAData wsaData{};
	const int nError = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (nError != 0)
	{
		Error(eDLL_T::COMMON, 0, "%s: Windows Sockets API startup failure: (%s)\n",
			__FUNCTION__, NET_ErrorString(WSAGetLastError()));
	}
}

void Winsock_Shutdown()
{
	const int nError = ::WSACleanup();
	if (nError != 0)
	{
		Error(eDLL_T::COMMON, 0, "%s: Windows Sockets API shutdown failure: (%s)\n",
			__FUNCTION__, NET_ErrorString(WSAGetLastError()));
	}
}

void DirtySDK_Startup()
{
	// DirtySDK not needed for direct connect mode
}

void DirtySDK_Shutdown()
{
	// DirtySDK not needed for direct connect mode
}

#define SIGDB_FILE "cfg/startup.bin"

//=============================================================================
// S21 client detour registration.
//=============================================================================
void DetourRegister()
{
	//-------------------------------------------------------------------------
	// Tier0 - Core infrastructure (always needed)
	//-------------------------------------------------------------------------
	REGISTER(VPlatform);

	//-------------------------------------------------------------------------
	// RTech - lobby world
	//-------------------------------------------------------------------------
	REGISTER(VPakLobbyWorldS21);
	REGISTER(VVfxAliasNullGuardS21);
	REGISTER(VSurfDataFallbackGuardS21);
	REGISTER(VBodySkinGuardS21);
	REGISTER(VWeaponModVisual);         // [WEAP-MOD-VIS] RecalcMods -> RequestBodygroupUpdate so held-weapon optics apply without holster
	REGISTER(VPoseParamGuardS21);
	REGISTER(VAnimDescGuardS21);
	REGISTER(VShaderTeardownGuardS21);
	REGISTER(VTextureStreamAbortFree);
	REGISTER(VEffectChildLinkGuardS21);
	REGISTER(VStudioSeqdescGuardS21);
	REGISTER(VClassVarNativesCl);
	REGISTER(VFOVLimit);
	REGISTER(VVisualClutter);

	//-------------------------------------------------------------------------
	// FileSystem
	//-------------------------------------------------------------------------
	REGISTER(VFileSystem_Stdio);

	//-------------------------------------------------------------------------
	// Ebisu/Platform (critical for connection)
	//-------------------------------------------------------------------------
	REGISTER(VEbisuSDK);

	//-------------------------------------------------------------------------
	// Engine - Core systems
	//-------------------------------------------------------------------------
	REGISTER(VNet);
	REGISTER(VSplitPacketRecvClamp);
	REGISTER(VNetChan);
	REGISTER(VNetDecodeDiagS21);
	REGISTER(VNetFrameDiagS21);
	REGISTER(VNetObserverDiagS21);
	REGISTER(VPakCensus);
	REGISTER(VHostFrameProbe);          // [HOST-FRAME] zero-dt tripwire on _Host_RunFrame
	REGISTER(VClientStringCmdRestrict);
	REGISTER(VScriptRemoteS2CGate);
	REGISTER(VScriptCreateNotifyS21);
	REGISTER(VStaticPropMgr);
	REGISTER(VBSPCollisionDebug);

	//-------------------------------------------------------------------------
	// VScript / Squirrel -- S21 client path (S3 VSquirrel* set is not registered here).
	//-------------------------------------------------------------------------
	REGISTER(VSquirrelS21Core);
	REGISTER(VScriptS21Override);
	REGISTER(VPlatformFSOverrideS21);
	REGISTER(VFSScriptRedirectS21);
	REGISTER(VRPakObserveS21);
	REGISTER(V_AsyncIO_S21);
	REGISTER(VRPakSigBypassS21);
	REGISTER(VPakOptStreamDropS21);
	REGISTER(VStringTableDiagS21);
	REGISTER(VSkinNamesStubS21);
	REGISTER(VWeaponPrecacheRedirectS21);
	REGISTER(VWeaponKVDiskS21);
	REGISTER(VLocalizeDiskS21);
	REGISTER(V_Datatable);
	REGISTER(VSettingsDiskS21);
	REGISTER(VIntegrityNeuterS21);
	REGISTER(VMilesBankListS21);

	//-------------------------------------------------------------------------
	// Game/client - prediction diagnostics
	//-------------------------------------------------------------------------
	REGISTER(VPredDiag);
	REGISTER(VGrappleRopeDiag);
	REGISTER(VJumpPadViewPunchDiag);
	REGISTER(VTriggerClientPredictForce);
	REGISTER(VMantleBoostClient);
	REGISTER(VMantleBoostRuiCl);
	REGISTER(VTriggerCannonClient);
	REGISTER(VMeleeLungeProbe);
	REGISTER(VMeleeActivityTrace);
	REGISTER(VHalfDuckZipParityClient);
	REGISTER(VMoveSimTraceClient);
	REGISTER(VBridgeCmdSeedClient);
	REGISTER(VBridgeFireTapClient);
	// VRuiTracks: S21 already owns ids through that range.
	// VClockMonotonicFix: Hook_ClockDrift already strips non-monotonic feeds.
	// V_ViewRender: GetVar pattern is S3-only (0 hits on S21).

	//-------------------------------------------------------------------------
	// Rendering / ImGui -- D3D11 globals + WindowProc for input
	//-------------------------------------------------------------------------
	REGISTER(VDXGI);
	REGISTER(V_RenderUtils);
	REGISTER(VDebugOverlay);
	REGISTER(VEngineVGui);
	REGISTER(VHudChat);
	REGISTER(VMatSystemSurface);
	REGISTER(VRuiDrawEnable);
	REGISTER(VGame);
	REGISTER(VRawInputAccum); // WM_INPUT still fires while EnableInput(false)
	REGISTER(VWeapCycleKbm);
	REGISTER(VBridgeFlagSet);
	REGISTER(VModelLoader);
	REGISTER(VModelPrecacheClientGrowS21);
	REGISTER(VMaterialMissingLogS21);
	REGISTER(VTexStreamFeedbackSyncS21);
	REGISTER(VMaterialGlue);
	REGISTER(VModelMissingLogS21);
	REGISTER(VPsoCacheS21);
}

//-----------------------------------------------------------------------------
// Singleton accessors
//-----------------------------------------------------------------------------
IKeyValuesSystem* KeyValuesSystem()
{
	return g_pKeyValuesSystem;
}
