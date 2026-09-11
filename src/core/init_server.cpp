//=============================================================================//
//
// Purpose: Main systems initialization file
//
//=============================================================================//

#include "core/stdafx.h"
#include "core/logdef.h"
#include "core/init.h"
#include "core/bridge_ready.h"
#include "core/bridge_stats.h"
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
#include "vphysics/physics_collide.h"
#include "vphysics/QHull.h"
#include "engine/staticpropmgr.h"
#include "engine/staticprop_bounds_debug.h"
#include "materialsystem/cmaterialsystem.h"
#include "materialsystem/cmatqueuedrendercontext.h"
#include "materialsystem/constbuffer.h"
#include "engine/server/server.h"
#include "engine/server/persistence.h"
#include "engine/server/vengineserver_impl.h"
#include "engine/server/datablock_sender.h"
#include "engine/server/datablock_recv_clamp.h"
#include "engine/server/datablock_oversized.h"
#include "engine/server/netchan_relrecv_free.h"
#include "engine/server/connect_password_gate.h"
#include "engine/server/mod_policy_gate.h"
#include "engine/server/snapshot_diag.h"
#include "engine/server/snapshot_dump.h"
#include "engine/server/snapshot_send.h"
#include "engine/server/snapshot_ring.h"
#include "engine/server/snapshot_ack_diag.h"
#include "engine/server/snapshot_writer.h"
#include "engine/server/ack_gate_relax.h"
#include "engine/server/edict_reserve.h"
#include "engine/server/offhand_dt_resize.h"
#include "engine/server/skinnames_table_inject.h"
#include "engine/server/stringtable_baseline_grow.h"
#include "engine/server/audio_digest_expand.h"
#include "engine/server/zipline_validation.h"
#include "game/server/vscript_remotefunctions_buffer_expand.h"
#include "rtech/rstdlib.h"
#include "rtech/rson.h"
#include "rtech/async/asyncio.h"
#include "rtech/pak/pakalloc.h"
#include "rtech/pak/pakparse.h"
#include "rtech/pak/pakstate.h"
#include "rtech/pak/pakstream.h"
#include "rtech/pak/stlt_field_rescue.h"
#include "rtech/pak/camo_skins_cap.h"
#include "rtech/pak/layout_cache_grow.h"
#include "rtech/pak/string_table_grow.h"
#include "rtech/pak/engine_soft_warnings.h"
#include "rtech/pak/signon_buffer_grow.h"
#include "rtech/pak/settings_disk.h"
#include "rtech/stryder/stryder.h"
#include "rtech/playlists/playlists.h"
#include "rtech/datatable/datatable.h"
#include "engine/client/client.h"
#include "localize/localize.h"
#include "engine/enginetrace.h"
#include "engine/traceinit.h"
#include "engine/common.h"
#include "engine/cmodel_bsp.h"
#include "engine/modelinfo.h"
#include "engine/host.h"
#include "engine/host_cmd.h"
#include "engine/host_state.h"
#include "engine/modelloader.h"
#include "engine/cmd.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "engine/splitpacket_recv_clamp.h"
#include "engine/networkstringtable.h"
#include "engine/debugoverlay.h"
#include "engine/vis_debug.h"
#include "engine/server/sv_main.h"
#include "engine/server/sv_rcon.h"
#include "engine/sdk_dll.h"
#include "engine/sys_dll.h"
#include "engine/sys_dll2.h"
#include "engine/sys_engine.h"
#include "engine/sys_utils.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/sqstdaux.h"
#include "vscript/languages/squirrel_re/include/sqstdstring.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/vscript.h"
#include "game/shared/r1/weapon_parse.h"
#include "game/server/r1/blast_pattern.h"
#include "game/server/persistence_ext.h"
#include "game/server/persistence_basevar_overflow.h"
#include "game/shared/weapon_legendary_ext.h"
#include "game/shared/weapon_customact_c2s_xlat.h"
#include "game/shared/scriptremotefunctions_server.h"
#include "game/server/passive_changed.h"
#include "game/server/extended_range_use.h"
#include "game/server/jetdrive.h"
#include "game/server/track_entity.h"
#include "game/server/player_launch.h"
#include "game/server/translocation.h"
#include "game/server/skydive.h"
#include "game/server/infinite_ammo.h"
#include "game/server/weapon_ammo_pool_mod.h"
#include "game/server/consumable_inv.h"
#include "game/server/mantle_boost.h"
#include "game/server/halfduck_zip_parity.h"
#include "game/server/move_sim_trace.h"
#include "game/server/repel_realm_gate.h"
#include "game/server/zipline_cooldown.h"
#include "game/server/zipline_exit_parity.h"
#include "game/server/poseparam_ext.h"
#include "game/server/melee_lunge_probe.h"
#include "game/server/melee_activity_trace.h"
#include "game/server/movescale_weapon_parity.h"
#include "game/shared/alliance_compat.h"
#include "game/shared/deathfield_system.h"
#include "game/server/tapstrafe.h"
#include "game/server/trigger_slip_diag.h"
#include "game/server/trigger_starttouch_dedupe.h"
#include "game/server/trigger_clientpredict.h"
#include "game/server/jumppad_parity.h"
#include "game/server/trigger_cannon.h"
#include "game/server/trigger_gravity.h"
#include "game/server/trigger_updraft.h"
#include "game/shared/weapon_script_vars.h"
#include "game/shared/offhand_activation_patches.h" // server dispatcher runs on DEDICATED too, must not be client-only
#include "game/shared/weapon_dualwield_slot_patch.h"
#include "game/shared/offhand_instant_swap.h" // -parity offhand_instant_swap_to_offhand escape in the S3 dispatcher switch-away gate
#include "game/shared/r1/weapon_bolt.h"
#include "game/shared/bridge_cmd_seed.h"
#include "game/server/bridge_fire_clock.h"
#include "game/server/bridge_zoom_gate.h"
#include "game/server/bridge_equip_gate.h"
#include "game/server/bridge_deploy_gate.h"
#include "game/shared/util_shared.h"
#include "game/shared/usercmd.h"
#include "game/shared/animation.h"
#include "game/shared/activity.h"
#include "game/shared/activitymodifier.h"
#include "game/shared/vscript_shared.h"
#include "game/server/util_server.h"
#include "game/server/ai_node.h"
#include "game/server/ai_network.h"
#include "game/server/ai_networkmanager.h"
#include "game/server/ai_utility.h"
#include "game/server/detour_impl.h"
#include "game/server/gameinterface.h"
#include "game/shared/dt_extend.h"
#include "game/shared/dt_extend_diag.h"
#include "game/shared/dt_extend_system14.h"
#include "game/server/sndc_alloc.h"
#include "game/shared/scriptnetdata_ext.h"
#include "game/shared/scriptnetdata_limits.h"
#include "game/shared/status_effects_sdk.h"
#include "game/server/movehelper_server.h"
#include "game/server/player.h"
#include "game/server/player_command.h"
#include "game/server/bridge_cmd_chain.h"
#include "game/server/ai_basenpc.h"
#include "game/server/physics_main.h"
#include "game/server/vscript_server.h"
#include "game/server/entitylist.h"
#include "game/server/baseentity.h"
#include "game/server/recipientfilter.h"
#include "game/server/sound.h"
#include "game/shared/weapon_heat.h"
#include "game/server/akimbo.h"
#include "public/edict.h"
#include "game/shared/sdk_entity_state.h"

#include "DirtySDK/dirtysock.h"
#include "DirtySDK/dirtysock/netconn.h"
#include "DirtySDK/proto/protossl.h"
#include "DirtySDK/proto/protowebsocket.h"


/////////////////////////////////////////////////////////////////////////////////////////////////
//
// ██╗███╗ ██╗██╗████████╗██╗ █████╗ ██╗ ██╗███████╗ █████╗ ████████╗██╗ ██████╗ ███╗ ██╗
// ██║████╗ ██║██║╚══██╔══╝██║██╔══██╗██║ ██║╚══███╔╝██╔══██╗╚══██╔══╝██║██╔═══██╗████╗ ██║
// ██║██╔██╗ ██║██║ ██║ ██║███████║██║ ██║ ███╔╝ ███████║ ██║ ██║██║ ██║██╔██╗ ██║
// ██║██║╚██╗██║██║ ██║ ██║██╔══██║██║ ██║ ███╔╝ ██╔══██║ ██║ ██║██║ ██║██║╚██╗██║
// ██║██║ ╚████║██║ ██║ ██║██║ ██║███████╗██║███████╗██║ ██║ ██║ ██║╚██████╔╝██║ ╚████║
// ╚═╝╚═╝ ╚═══╝╚═╝ ╚═╝ ╚═╝╚═╝ ╚═╝╚══════╝╚═╝╚══════╝╚═╝ ╚═╝ ╚═╝ ╚═╝ ╚═════╝ ╚═╝ ╚═══╝
//
/////////////////////////////////////////////////////////////////////////////////////////////////

// These command line parameters disable a bunch of things in the engine that
// the dedicated server does not need, therefore, reducing a lot of overhead.
void InitCommandLineParameters()
{
	CommandLine()->AppendParm("-collate", "");
	CommandLine()->AppendParm("-multiple", "");
	CommandLine()->AppendParm("-noorigin", "");
	CommandLine()->AppendParm("-nodiscord", "");
	CommandLine()->AppendParm("-noshaderapi", "");
	CommandLine()->AppendParm("-nobakedparticles", "");
	CommandLine()->AppendParm("-novid", "");
	CommandLine()->AppendParm("-nomenuvid", "");
	CommandLine()->AppendParm("-nosound", "");
	CommandLine()->AppendParm("-nomouse", "");
	CommandLine()->AppendParm("-nojoy", "");
	CommandLine()->AppendParm("-nosendtable", "");
}

void ScriptConstantRegistrationCallback(CSquirrelVM* s)
{
	Script_RegisterListenServerConstants(s);
	TrackEntity_RegisterScriptConstants(s);
}

// Forwards the detour target-failure reports to the SDK log. Without a sink they
// only reach OutputDebugStringA, so an unresolved pattern is invisible unless a
// debugger is attached -- the aggregate skip count below says how many, not which.
static void Systems_Init_DetourLogSink(const char* const pszMsg)
{
	Warning(eDLL_T::COMMON, "%s", pszMsg ? pszMsg : "[DETOUR] (null message)\n");
}

void Systems_Init()
{
	DetourRegister();

	CFastTimer initTimer;
	initTimer.Start();
	DetourInit();
	initTimer.End();

	Msg(eDLL_T::NONE, "+-------------------------------------------------------------+\n");
	Msg(eDLL_T::NONE, "%-16s '%10.6f' seconds ('%12lu' clocks)\n",
		"Detour->InitDB()",
		initTimer.GetDuration().GetSeconds(), initTimer.GetDuration().GetCycles());

	initTimer.Start();

	// Begin the detour transaction to hook the process
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	g_DetourLogSink = &Systems_Init_DetourLogSink;
	Detour_ResetNullSkipCount();
	Detour_ResetAttachTargets();

	// Hook functions
	for (const IDetour* pd : g_DetourVec)
	{
		Detour_SetAttachingClass(typeid(*pd).name());
		pd->Detour(true);
	}
	Detour_SetAttachingClass(nullptr);

	const LONG nullSkips = Detour_ConsumeNullSkipCount();
	if (nullSkips > 0)
	{
		Warning(eDLL_T::COMMON,
			"[DETOUR] %ld null-target DetourSetup skip(s) during attach -- "
			"those patterns never installed (check GetFun logs)\n",
			(long)nullSkips);
	}

	// Patch instructions
	RuntimePtc_Init();

	// Commit the transaction
	HRESULT hr = DetourTransactionCommit();
	const size_t nRegistered = g_DetourVec.size();
	if (hr != NO_ERROR)
	{
		// Failed to hook into the process, terminate -- ledger first so the
		// FAIL line lands in the log even when Error() does not return.
		Msg(eDLL_T::SERVER,
			"[BRIDGE-READY] server: %zu registered | commit=FAIL 0x%08X\n",
			nRegistered, static_cast<unsigned>(hr));
		BridgeReady_Print("server");
		Assert(0);
		Error(eDLL_T::COMMON, 0xBAD0C0DE, "Failed to detour process: error code = %08x\n", hr);
	}

	initTimer.End();
	Msg(eDLL_T::NONE, "%-16s '%10.6f' seconds ('%12lu' clocks)\n",
		"Detour->Attach()",
		initTimer.GetDuration().GetSeconds(), initTimer.GetDuration().GetCycles());
	Msg(eDLL_T::NONE, "+-------------------------------------------------------------+\n");
	Msg(eDLL_T::NONE, "\n");

	BridgeReady_Print("server");

	InitCommandLineParameters();

	// Script context registration callbacks.
	ScriptConstantRegister_Callback = ScriptConstantRegistrationCallback;

	ServerScriptRegister_Callback = Script_RegisterServerFunctions;
	ServerScriptRegisterEnum_Callback = Script_RegisterServerEnums;


}

//////////////////////////////////////////////////////////////////////////
//
// ███████╗██╗ ██╗██╗ ██╗████████╗██████╗ ██████╗ ██╗ ██╗███╗ ██╗
// ██╔════╝██║ ██║██║ ██║╚══██╔══╝██╔══██╗██╔═══██╗██║ ██║████╗ ██║
// ███████╗███████║██║ ██║ ██║ ██║ ██║██║ ██║██║ █╗ ██║██╔██╗ ██║
// ╚════██║██╔══██║██║ ██║ ██║ ██║ ██║██║ ██║██║███╗██║██║╚██╗██║
// ███████║██║ ██║╚██████╔╝ ██║ ██████╔╝╚██████╔╝╚███╔███╔╝██║ ╚████║
// ╚══════╝╚═╝ ╚═╝ ╚═════╝ ╚═╝ ╚═════╝ ╚═════╝ ╚══╝╚══╝ ╚═╝ ╚═══╝
//
//////////////////////////////////////////////////////////////////////////

void Systems_Shutdown()
{
	// Dedi half of the wire-authority counters; the client dumps its own.
	BridgeStats_PrintAndReset("dedi shutdown");

	// Shutdown RCON (closes all open sockets)
	RCONServer()->Shutdown();


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

/////////////////////////////////////////////////////
//
// ██╗ ██╗████████╗██╗██╗ ██╗████████╗██╗ ██╗
// ██║ ██║╚══██╔══╝██║██║ ██║╚══██╔══╝╚██╗ ██╔╝
// ██║ ██║ ██║ ██║██║ ██║ ██║ ╚████╔╝ 
// ██║ ██║ ██║ ██║██║ ██║ ██║ ╚██╔╝ 
// ╚██████╔╝ ██║ ██║███████╗██║ ██║ ██║ 
// ╚═════╝ ╚═╝ ╚═╝╚══════╝╚═╝ ╚═╝ ╚═╝ 
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
	const int32_t netConStartupRet = NetConnStartup("-servicename=sourcesdk");

	if (netConStartupRet < 0)
	{
		Error(eDLL_T::COMMON, 0, "%s: Network connection module startup failure: (%i)\n",
			__FUNCTION__, netConStartupRet);
	}
}

void DirtySDK_Shutdown()
{
	const int32_t netConShutdownRet = NetConnShutdown(0);

	if (netConShutdownRet < 0)
	{
		Error(eDLL_T::COMMON, 0, "%s: Network connection module shutdown failure: (%i)\n",
			__FUNCTION__, netConShutdownRet);
	}
}

#define SIGDB_FILE "cfg/server/startup.bin"

void DetourInit() // Run the sigscan
{
	const bool bNoSmap = CommandLine()->CheckParm("-nosmap") ? true : false;
	const bool bLogAdr = CommandLine()->CheckParm("-sig_toconsole") ? true : false;
	bool bInitDivider = false;

	g_SigCache.SetDisabled(bNoSmap);
	g_SigCache.ReadCache(SIGDB_FILE);

	// No debug logging in non dev builds.
	const bool bDevMode = !IsCert() && !IsRetail();

	for (const IDetour* pd : g_DetourVec)
	{
		pd->GetCon(); // Constants.
		pd->GetFun(); // Functions.
		pd->GetVar(); // Variables.

		if (bDevMode && bLogAdr)
		{
			if (!bInitDivider)
			{
				bInitDivider = true;
				spdlog::debug("+---------------------------------------------------------------------+\n");
			}
			pd->GetAdr();
			spdlog::debug("+---------------------------------------------------------------------+\n");
		}
	}

	// Must be performed after detour init as we patch instructions which alters the function signatures.
	Dedicated_Init();

	g_SigCache.WriteCache(SIGDB_FILE);
	g_SigCache.InvalidateMap();
}

void DetourAddress() // Test the sigscan results
{
	spdlog::debug("+---------------------------------------------------------------------+\n");
	for (const IDetour* pd : g_DetourVec)
	{
		pd->GetAdr();
		spdlog::debug("+---------------------------------------------------------------------+\n");
	}
}

void DetourRegister() // Register detour classes to be searched and hooked.
{
	// Tier0
	REGISTER(VPlatform);
	REGISTER(VJobThread);
	REGISTER(VThreadTools);
	REGISTER(VTSListBase);

	// Tier1
	REGISTER(VCommandLine);
	REGISTER(VCVar);

	// Tier2
	REGISTER(V_RenderUtils);

	// VPC
	REGISTER(VKeyValues);
	REGISTER(VRSON);
	REGISTER(VFactory);

	// VstdLib
	REGISTER(VCallback);
	REGISTER(VCompletion);
	REGISTER(HKeyValuesSystem);

	// Common
	REGISTER(VOpcodes);
	REGISTER(V_NetMessages);

	// Launcher
	REGISTER(VPRX);
	REGISTER(VLauncher);

	REGISTER(VAppSystemGroup);

	// FileSystem
	REGISTER(VBaseFileSystem);
	REGISTER(VFileSystem_Stdio);

	// DataCache
	REGISTER(VMDLCache);

	// Ebisu
	REGISTER(VEbisuSDK);


	// VPhysics
	REGISTER(VPhysicsCollide);
	REGISTER(VQHull);

	// StaticPropMgr
	REGISTER(VStaticPropMgr);
	REGISTER(VStaticPropBoundsDebug);

	// MaterialSystem
	REGISTER(VMaterialSystem);


	// Server
	REGISTER(VServer); // REGISTER SERVER ONLY!
	REGISTER(HVEngineServer); // REGISTER SERVER ONLY!
	REGISTER(VServerDataBlockSender); // REGISTER SERVER ONLY!
REGISTER(VDataBlockRecvClamp);    // REGISTER SERVER ONLY! clamp ProcessDataBlock write to the 768KB scratch
REGISTER(VDataBlockOversized);    // REGISTER SERVER ONLY! ack + resend for data blocks past the engine scratch
REGISTER(VNetChanRelRecvFree);    // REGISTER SERVER ONLY! free live recv buf before first-fragment store
REGISTER(VConnectPasswordGate);   // REGISTER SERVER ONLY! challenge-bind the connect password tag (attaches nothing; run from CServer::ConnectClient, VServer owns the attach)
	REGISTER(VModPolicyGate);         // REGISTER SERVER ONLY! required/allowed mod policy at first usercmd
	REGISTER(VSnapshotDiag);          // REGISTER SERVER ONLY! per-tick + snapshot RSS waypoints for state=4 leak hunt
	REGISTER(VPrecacheModelGuard);    // REGISTER SERVER ONLY! catches Server_PrecacheModel((BYTE*)-1) before AV
	REGISTER(VPlayerAnimUpdateGuard); // REGISTER SERVER ONLY! applies spectator SettingsBlock pre-anim-update so OnPlayerChangedTeam path can't dereference -1 sentinel
	REGISTER(VCClientSendSnapshotDiag);
	REGISTER(VSnapshotWriterTrace);      // REGISTER SERVER ONLY! [SNAP28]/[PROP-BALLOON] -- pins the prop that floods the malformed delta ( + )
	REGISTER(VSnapshotDump);            // REGISTER SERVER ONLY! dump/census (FLATN, MB-PACK, bonefollow, ring watch)
	REGISTER(VAckGateRelax);            // REGISTER SERVER ONLY! NOPs the strict ack-equality jl at +0x79 so the dedi accepts S21's late async-worker acks natively
	REGISTER(VEdictReserve);            // REGISTER SERVER ONLY! forces global_non_rewinding onto edict 257 (the S21 client's fixed slot) and keeps num_edicts above it
	REGISTER(VSnapshotRingDeepen);      // REGISTER SERVER ONLY! raises the snapshot frame-store ring 160 -> sdk_bridge_snapshot_ring so a late ack costs latency, not a full entity rebuild
	REGISTER(VAckDiag);                 // REGISTER SERVER ONLY! [ACK-DIAG]/[ACK-REGRESS] -- observes every incoming delta ack and the engine gates that silently drop one
	REGISTER(VOffhandDTResize);         // REGISTER SERVER ONLY! patches alloc+loop+register to build DT_WeaponInventory.offhandWeapons with 8 templates (S21 native wire shape)
	REGISTER(VSettingsStringGuard);     // REGISTER SERVER ONLY! guards classActivityModifier -1 sentinel deref in 
	REGISTER(VSkinNamesTableInject);    // REGISTER SERVER ONLY! creates "SkinNames" stringtable so S21 client SetSkinModByName resolves instead of AV'ing
	REGISTER(VStringTableBaselineGrow); // REGISTER SERVER ONLY! grows the 512KB per-table baseline scratch in CClient::SendServerInfo and reports per-table fill ([STBL-BASELINE])
	REGISTER(VAudioDigestExpand);       // REGISTER SERVER ONLY! heap-backs audio bank digest table to support >100K-entry mbnk_digest
	REGISTER(VRemoteFuncBufferExpand);  // REGISTER SERVER ONLY! heap-backs Remote_RegisterClientFunction buffer (16KB/256 -> 256KB/2048) so S21 scripts can register all client functions
	REGISTER(VZiplineValidationDedi);
	REGISTER(VStatusEffectParseFix);    // REGISTER SERVER ONLY! NOPs the S3 status_effect_types.txt parser's fatal 'Code expects disable_wall_run_and_double_jump' so the dedi boots with the S21-aligned enum
	REGISTER(VMapEntitySkipper);        // REGISTER SERVER ONLY! [MAP-SKIP] refuses crasher classes (prop_dynamic/info_target/...) baked into the.bsp LUMP_ENTITIES during MapEntity_ParseEntity -- unreachable by VPK strip / script BlockMapEntityParseCreationOf
	REGISTER(VGibFinderGuard);          // REGISTER SERVER ONLY! [GIB-FINDERS]/[GIB-GUARD] resolve gibModels finders + fail-closed HasGibModel / gib-spawn
	REGISTER(VScriptRemoteS2CBridge);   // REGISTER SERVER ONLY! [BRIDGE-S2C-SR] hooks the shared S3 Remote_CallFunction_NonReplay/_Replay/_UI impl and forwards name-carried S->C remote calls to the S21 client on the Bridge S2C ScriptRemote lane (bridge_s2c_scriptremote, default on)
	REGISTER(VPassiveChangedBridge);    // REGISTER SERVER ONLY! [PASSIVE-BRIDGE] hooks the native GivePassive/RemovePassive (/) and manually fires CodeCallback_OnPassiveChanged into the server VM on a genuine bit flip -- this S3 build's engine never calls it natively (confirmed absent from the compiled string table; every AddCallback_OnPassiveChanged consumer, e.g. Seer's heartbeat sensor, silently never fired without this)
	REGISTER(VExtendedRangeUse);        // REGISTER SERVER ONLY! [EXT-USE] injects CodeCallback_GetExtendedRangeUseEntitiesForPlayer results into the native use-candidate list (Alter remote deathbox + Void Nexus)
	REGISTER(VJetDrive);                // REGISTER SERVER ONLY! [JETDRIVE] from-scratch port of Vantage's tactical recall-launch movement subsystem -- S3 has zero trace of it (Season 14+ content, confirmed absent from the whole binary via ), unlike the S21 client which has it natively..
	REGISTER(VTrackEntity);             // REGISTER SERVER ONLY! [TRACK-ENT] ClearTrackEntitySettings detour so S21 camera sidecar resets with the S3 native
	REGISTER(VPlayerLaunch);            // REGISTER SERVER ONLY! [PLAYER-LAUNCH] CheckJumpButton-gated ApplyPlayerLaunch parity inside FullWalkMove
	REGISTER(VSkydiveBridge);           // REGISTER SERVER ONLY! [SKYDIVE-SIM] per-executed-usercmd skydive simulation through the engine's own skydive wrappers. S21 predicts the skydive client-side every command; a server copy on a script thread integrates the same springs at a different rate and can never agree with it.
	REGISTER(VInfiniteAmmoDedi);        // REGISTER SERVER ONLY! [INF-AMMO] InfiniteAmmoState enforcement on the S3 native ammo paths; patterns are r5apex_ds-only (client twin left unhooked)
	REGISTER(VWeaponAmmoPoolMod);       // REGISTER SERVER ONLY! [ALT-AMMO] per-entity WeaponInfo clone so a mod can override ammo_pool_type (field sits outside the S3 0x1150 moddable block)
	REGISTER(VConsumableInvBridge);     // REGISTER SERVER ONLY! [CONSUMABLEINV-FULL] u16 type shadow for m_consumableInventory (S3 u8 type truncates loot idx>=256)
	REGISTER(VMantleBoostBridge);       // REGISTER SERVER ONLY! [MANTLE-BOOST] mantle-exit boost: one TraversalMove detour -- pre-orig sweet-spot, post-orig finish boost + forced Jump
	REGISTER(VMoveScaleWeaponParity);   // REGISTER SERVER ONLY! move-scale weapon term -> client parity
	REGISTER(VHalfDuckZipParity);       // REGISTER SERVER ONLY! [HALFDUCK] m_doingHalfDuck is latched once at duck-start and is not networked; duck is suppressed while ziplining, so the two engines sample it one command apart and only one applies the (standHull-duckHull)*0.5 origin step. Forces the latch on a duck that begins just after a zipline release. Twin: VHalfDuckZipParityClient.
	REGISTER(VMoveSimTrace);            // REGISTER SERVER ONLY! [MOVE-TRACE] per-command FullWalkMove state dump; twin: VMoveSimTraceClient (attaches nothing; sampled from VJetDrive's hook)
	REGISTER(VRepelRealmGate);          // REGISTER SERVER ONLY! [REPEL-REALM] player-vs-player repel pass gated on shared m_realmsBitMask; disjoint-realm players no longer push each other
	REGISTER(VZiplineExitParity);       // REGISTER SERVER ONLY! [ZIP-EXIT] auto-detach exit-velocity rewrite (client rope clamp + vertical magnitude) so both engines leave the rope with the same velocity
	REGISTER(VMeleeActivityTraceServer);   // REGISTER SERVER ONLY! [MELEE-ACT] bridge_melee_trace: melee custom-activity lifetime, diffed against the client twin
	REGISTER(VMeleeLungeProbeServer);   // REGISTER SERVER ONLY! [LUNGE-PROBE] bridge_melee_lunge_probe: melee-lunge overspeed-clamp cap value, diffed against the client twin
	REGISTER(VAllianceCompat);          // REGISTER SERVER! FreeDM/Control alliance matrix + IsEnemyTeam detour (SetTeamIsInAlliance native)
	REGISTER(VDeathFieldSystem);        // REGISTER SERVER! SetDeathFieldParams hook + g_pWorldEntity resolve for realm rings
	REGISTER(VTapStrafeBridge);         // REGISTER SERVER ONLY! [TAPSTRAFE] Gap B: from-scratch port of 's "jump grace"/tap-strafe (lurch) assist -- S3 has zero trace of the mechanic (confirmed via convar-string sweep, only 2 leftover ConVar-registration stubs survive). ONE detour on CGameMovement::FullWalkMove, airborne-gated, mantle_boost_disables_tap_strafes-gated via MantleBoost_ShouldSuppressTapStrafe. See tapstrafe.h.
	REGISTER(VTriggerSlipDiag);         // REGISTER SERVER ONLY! [SLIP-TOUCH]/[SLIP-END]/[SLIP-FORCE] enter/leave/force diag for CTriggerSlip (promoted CTriggerSlipSphere). sdk_slip_diag 0/1/2. StartTouch EndTouch FullWalkMove.
	REGISTER(VBridgeFireClock);         // REGISTER SERVER ONLY! [FIRE-CLOCK] bounded command_time stamp on weapon-sim latestPredictedTime for one shared fire clock.
	REGISTER(VTriggerStartTouchDedupe); // REGISTER SERVER ONLY! [TRIG-DEDUP] CBaseTrigger::StartTouch fires OnStartTouch + the script m_enterCallback on EVERY call, not just the first (EndTouch guards the same lookup) -- one detour restores the guard for the 19 trigger classes that reach that body.
	REGISTER(VTriggerClientPredict);    // REGISTER SERVER ONLY! [TRIG-PRED] author CBaseTrigger::m_bClientSidePredicted on CTriggerCylinderHeavy so the S21 client can predict jump-pad touch
	REGISTER(VJumpPadParity);           // REGISTER SERVER ONLY! [JP-DUCKVERT][JP-DEBOUNCE] ducked m_vertOverride scale + per-player jump-pad relaunch debounce (m_jumpPadDebounceExpireTime)
	REGISTER(VTriggerCannonBridge);     // REGISTER SERVER ONLY! [TRIG-CANNON] server-authoritative TT_GRAVITY_CANNON launch; driven from VJumpPadParity's hook, no detour of its own
	REGISTER(VTriggerGravityBridge);    // REGISTER SERVER ONLY! [TRIG-GRAV] server-authoritative TT_GRAVITY_LIFT / TT_BLACKHOLE force; driven from VJumpPadParity's hook, no detour of its own
	REGISTER(VTriggerUpdraftBridge);    // REGISTER SERVER ONLY! [UPDRAFT] server-authoritative updraft state + movement; driven from VJumpPadParity's hook, no detour of its own
	REGISTER(VPoseParamExt);            // REGISTER SERVER ONLY! [POSE-EXT] script Set/GetPoseParameter indices 12..23 side-table so S21 models (Ballistic characterScriptParam=15) do not abort with Parameter index invalid


	// Engine/client
	REGISTER(VClient);

	// RTech
	REGISTER(V_ReSTD);

	REGISTER(V_AsyncIO);

	REGISTER(V_PakAlloc);
	REGISTER(V_PakParse);
	REGISTER(V_PakState);
	REGISTER(V_PakStream);
	REGISTER(VStltFieldRescue);   // detour stlt field-finder resolver to alias missing fields
	REGISTER(VCamoSkinsCap);      // raise hardcoded 512-entry cap on camo-skins pad rpaks
	REGISTER(VLayoutCacheGrow);   // grow s_settingsLayoutRuntimeData hash table 64 -> 128 slots
	REGISTER(VStringTableGrow);   // raise SettingsAssets network string table max entries
	REGISTER(VEngineSoftWarnings); // NOP Unrecognized-entry logger CALL (both parser clones)
	REGISTER(VSignonBufferGrow);  // grow SignonInfo bf_write static buffer past stock 0xC0000 cap
	REGISTER(VSettingsDiskS21);   // synthesize settings (stgs) assets from platform/settings/**/*.json

	REGISTER(VStryder);
	REGISTER(VPlaylists);
	REGISTER(V_Datatable);


	// Engine
	REGISTER(VCommon);

	REGISTER(VSys_Dll);
	REGISTER(VSys_Dll2);
	REGISTER(VSys_Utils);
	REGISTER(VEngine);
	REGISTER(VEngineTrace);
	REGISTER(VModelInfo);

	REGISTER(VTraceInit);
	REGISTER(VModel_BSP);
	REGISTER(VHost);
	REGISTER(VHostCmd);
	REGISTER(VHostState);
	REGISTER(VModelLoader);
	REGISTER(VCmd);
	REGISTER(VNet);
	REGISTER(VSplitPacketRecvClamp);
	REGISTER(VNetChan);
	REGISTER(VNetworkStringTableContainer);

	REGISTER(VLocalize);


	REGISTER(HSV_Main);


	REGISTER(VDebugOverlay);
	REGISTER(VVisDebug);

	// VScript
	REGISTER(VSquirrel);
	REGISTER(VScript);
	REGISTER(VScriptShared);
	REGISTER(VScriptServer);

	// Squirrel
	REGISTER(VSquirrelAPI);
	REGISTER(VSquirrelStdAux);
	REGISTER(VSquirrelStdString);
	REGISTER(VSquirrelVM);

	// Game/shared
	REGISTER(VUserCmd);
	REGISTER(VAnimation);
	REGISTER(VActivityList);
	REGISTER(VActivityModifiers);
	REGISTER(V_UTIL_Shared);

	REGISTER(V_Weapon_Parse);
	REGISTER(V_WeaponMeleeAnimFix);
	REGISTER(VBlastPattern);
	REGISTER(VPersistenceExt);        // must run before pdef parse
	REGISTER(VPersistenceBaseVarOverflow); // heap-back baseStruct items past 180
	REGISTER(VWeaponLegendaryExt);    // raise legendary-skin slot cap 8 -> 32
	REGISTER(VWeaponCustomActC2SXlat); // translate usercmd weaponCustomActivity S21->S3 before server-side StartCustomActivity
	REGISTER(VWeaponScriptVars);      // shared script/native resolver; dedicated needs server PhaseShiftBegin
	REGISTER(VOffhandActivationPatches); // slot 0..5 native dispatcher diag + slot 6/7 ext; server dispatcher runs on DEDICATED too, must not be client-only
	REGISTER(VOffhandInstantSwap);        // -parity offhand_instant_swap_to_offhand escape in the S3 dispatcher switch-away gate (Vantage tactical)
	REGISTER(VWeaponDualWieldSlotPatch);  // pair the dual-wield partner S21-style (main+7); S3's main+5 auto-draws the ordnance with the first primary


	// In shared code, but weapon bolt is SERVER only.
	REGISTER(V_Weapon_Bolt);
	REGISTER(VBridgeCmdSeed); // bridge_cmd_seed_parity: usercmd RNG-seed parity with the S21 client (view-kick jitter / pellet scatter) --
	REGISTER(VBridgeZoomGate); // ADS-reload + air-spread parity (bridge_ads_reload_parity / bridge_spread_air_priority)
	REGISTER(VBridgeEquipGate); // bridge_equip_gate: full S3 ACT_MP_EQUIP_* block for IsPlaying3pEquipActivity so pistol/rocket same-class switches match S21
	REGISTER(VBridgeDeployGate); // first-raise DRAWFIRST before the S3 sprint DRAW_TO_SPRINT skip (ground pickup while running)

	// Game/server
	REGISTER(VAI_Network);
	REGISTER(VAI_NetworkManager);
	REGISTER(VRecast);
	REGISTER(VServerGameDLL);
	REGISTER(VDTExtend);              // native SendTable expansion -- hooks SendTable_Init (server-side); no-op until props registered
	REGISTER(VSNDCAllocHook);         // SNDC registration hooks -- AllocateInternalVar detour + category bound + scriptNetCategories expand
	REGISTER(VScriptNetDataLimits);   // SNDC native layout patcher (vtable disp32 + SendProp.bss) -- ConVar sndc_native_layout
	REGISTER(VMoveHelperServer);
	REGISTER(VPhysics_Main); // REGISTER SERVER ONLY
	REGISTER(VBaseAnimating);
	REGISTER(VPlayer);
	REGISTER(VTranslocation);          // REGISTER SERVER ONLY! Loba toss-hold + drop-click latch
	REGISTER(VAI_BaseNPC);
	REGISTER(VPlayerMove);
	REGISTER(VBridgeCmdChain);
	REGISTER(VServerEntityList);
	REGISTER(VCBaseEntity);

	REGISTER(V_UTIL_Server);
	REGISTER(VRecipientFilter);
	REGISTER(VScriptNetDataExt);    // SNDC var slot lookup -- needs g_pClientNetVars resolved
	REGISTER(VSoundBridge);         // REGISTER SERVER ONLY! EmitSoundOnEntity -> SVC_Sounds broadcast (native send is CSOMET-dead on dedi)
	REGISTER(VWeaponHeat);
	REGISTER(VAkimboBridge);



	// Public
	REGISTER(VEdict);


	// L1 entity-state observer after VServerEntityList / VClientEntityList.
	REGISTER(VSDKEntityState);
}

//-----------------------------------------------------------------------------
// Singleton accessors
//-----------------------------------------------------------------------------
IKeyValuesSystem* KeyValuesSystem()
{
	return g_pKeyValuesSystem;
}
