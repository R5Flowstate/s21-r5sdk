cmake_minimum_required( VERSION 3.16 )

macro( add_engine_project PROJECT_NAME )
add_module( "lib" ${PROJECT_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Engine"
    "clockdriftmgr.cpp"
    "clockdriftmgr.h"
    "cmd.cpp"
    "cmd.h"
    "cmd_frame_queue.h"
    "common.cpp"
    "common.h"
)

add_sources( SOURCE_GROUP "Collision"
    "cmodel_bsp_debug.cpp"
    "cmodel_bsp_debug.h"
    "enginetrace.cpp"
    "enginetrace.h"
    "traceinit.h"
)

add_sources( SOURCE_GROUP "Debug"
    "debugoverlay.cpp"
    "debugoverlay.h"
    "vis_debug.cpp"
    "vis_debug.h"
)

if( NOT ${PROJECT_NAME} STREQUAL "engine_ds" )
add_sources( SOURCE_GROUP "Debug"
)
endif()

if( NOT ${PROJECT_NAME} STREQUAL "engine_ds" )
add_sources( SOURCE_GROUP "Render"
    "framelimit.cpp"
    "framelimit.h"
    "gl_model_private.h"
    "gl_rmain.cpp"
    "gl_rmain.h"
    "gl_rsurf.cpp"
    "gl_rsurf.h"
    "gl_screen.cpp"
    "gl_screen.h"
)
endif()

add_sources( SOURCE_GROUP "Network"
    "dt_recv.cpp"
    "framesnapshot.h"
    "net.cpp"
    "net.h"
    "net_chan.cpp"
    "net_chan.h"
    "networkstringtable.cpp"
    "networkstringtable.h"
    "packed_entity.h"
    "splitpacket_recv_clamp.cpp"
    "splitpacket_recv_clamp.h"
    "staticpropmgr.cpp"
    "staticpropmgr.h"
    "staticprop_bounds_debug.cpp"
    "staticprop_bounds_debug.h"
)

if( NOT ${PROJECT_NAME} STREQUAL "engine_ds" )
add_sources( SOURCE_GROUP "Network"
    "stringtable_diag.cpp"
    "stringtable_diag.h"
    "skinnames_stub.cpp"
    "skinnames_stub.h"
    "weapon_precache_redirect.cpp"
    "weapon_precache_redirect.h"
)
endif()

if( ${PROJECT_NAME} STREQUAL "engine_cl" )
add_sources( SOURCE_GROUP "Network"
    "client/net_bridge_split.h"
    "client/net_bridge_process.cpp"
    "client/net_bridge_c2s.cpp"
    "client/net_bridge_c2s_reliable.cpp"
    "client/net_bridge_install.cpp"
    "client/host_frame_probe.cpp"
    "client/host_frame_probe.h"
    "client/net_bridge_internal.h"
    "client/net_bridge_addrs.h"
    "client/net_bridge_addrs.cpp"
    "client/net_bridge_frame.cpp"
    "client/net_bridge_decode.cpp"
    "client/net_observer.h"
    "client/bridge_connect_cmd.cpp"
    "client/bridge_join_auth.cpp"
    "client/bridge_join_auth.h"
    "client/bridge_connect_password.h"
    "client/cl_parse_ents.cpp"
    "client/cl_parse_ents.h"
    "client/pdef_parse.cpp"
    "client/pdef_parse.h"
    "client/camo_index.cpp"
    "client/camo_index.h"
)
add_sources( SOURCE_GROUP "Collision"
    "cmodel_surfdata.cpp"
    "cmodel_surfdata.h"
)
add_sources( SOURCE_GROUP "System"
    "sys_integrity.cpp"
    "sys_integrity.h"
)
endif()

add_sources( SOURCE_GROUP "Model"
    "cmodel.cpp"
    "cmodel.h"
    "cmodel_bsp.cpp"
    "cmodel_bsp.h"
    "modelinfo.cpp"
    "modelinfo.h"
    "modelloader.cpp"
    "modelloader.h"
)

if( NOT ${PROJECT_NAME} STREQUAL "engine_ds" )
add_sources( SOURCE_GROUP "Model"
    "mdl_precache_client_grow.cpp"
    "mdl_precache_client_grow.h"
)
endif()

add_sources( SOURCE_GROUP "Host"
    "host.cpp"
    "host.h"
    "host_cmd.cpp"
    "host_cmd.h"
    "host_state.cpp"
    "host_state.h"
)

add_sources( SOURCE_GROUP "System"
    "sys_dll.cpp"
    "sys_dll.h"
    "sys_dll2.cpp"
    "sys_dll2.h"
    "sys_engine.cpp"
    "sys_engine.h"
    "sys_utils.cpp"
    "sys_utils.h"
    "sdk_dll.cpp"
    "sdk_dll.h"
)

if( NOT ${PROJECT_NAME} STREQUAL "engine_ds" )
add_sources( SOURCE_GROUP "System"
    "sys_getmodes.cpp"
    "sys_getmodes.h"
    "sys_mainwind.cpp"
    "sys_mainwind.h"
)
endif()

if( NOT ${PROJECT_NAME} STREQUAL "engine_cl" )
add_sources( SOURCE_GROUP "Server"
    "server/persistence.h"
    "server/server.cpp"
    "server/server.h"
    "server/sv_rcon.cpp"
    "server/sv_rcon.h"
    "server/sv_rcon_launcher.cpp"
    "server/sv_rcon_launcher.h"
    "server/sv_main.cpp"
    "server/sv_main.h"
    "server/vengineserver_impl.cpp"
    "server/vengineserver_impl.h"
    "server/datablock_sender.cpp"
    "server/datablock_sender.h"
    "server/datablock_recv_clamp.cpp"
    "server/datablock_recv_clamp.h"
    "server/datablock_oversized.cpp"
    "server/datablock_oversized.h"
    "server/connect_password_gate.cpp"
    "server/connect_password_gate.h"
    "server/mod_policy_gate.cpp"
    "server/mod_policy_gate.h"
    "server/netchan_relrecv_free.cpp"
    "server/netchan_relrecv_free.h"
    "server/snapshot_diag.cpp"
    "server/snapshot_diag.h"
    "server/snapshot_dump.cpp"
    "server/snapshot_dump.h"
    "server/snapshot_send.cpp"
    "server/snapshot_send.h"
    "server/snapshot_ring.cpp"
    "server/snapshot_ring.h"
    "server/snapshot_ack_diag.cpp"
    "server/snapshot_ack_diag.h"
    "server/snapshot_writer.cpp"
    "server/snapshot_writer.h"
    "server/ack_gate_relax.cpp"
    "server/ack_gate_relax.h"
    "server/edict_reserve.cpp"
    "server/edict_reserve.h"
    "server/offhand_dt_resize.cpp"
    "server/offhand_dt_resize.h"
    "server/skinnames_table_inject.cpp"
    "server/skinnames_table_inject.h"
    "server/stringtable_baseline_grow.cpp"
    "server/stringtable_baseline_grow.h"
    "server/precache_natives.cpp"
    "server/precache_natives.h"
    "server/audio_digest_expand.cpp"
    "server/audio_digest_expand.h"
    "server/zipline_validation.cpp"
    "server/zipline_validation.h"
)
endif()

add_sources( SOURCE_GROUP "Client"
    "client/cdll_engine_int.cpp"
    "client/cdll_engine_int.h"
    "client/client.cpp"
    "client/client.h"
)

add_sources( SOURCE_GROUP "Shared"
    "shared/base_rcon.cpp"
    "shared/base_rcon.h"
    "shared/shared_rcon.cpp"
    "shared/shared_rcon.h"
    "shared/datablock.cpp"
    "shared/datablock.h"
    "shared/ents_shared.h"
    "shared/s21_bridge_compat.h"
)

if( NOT ${PROJECT_NAME} STREQUAL "engine_ds" )
add_sources( SOURCE_GROUP "Client" # Client only.
    "client/clientstate.cpp"
    "client/clientstate.h"
    "client/cl_main.h"
    "client/cl_splitscreen.h"
    "client/cl_rcon.cpp"
    "client/cl_rcon.h"
    "client/cl_rcon_launcher.cpp"
    "client/cl_rcon_launcher.h"
    "client/vengineclient_impl.cpp"
    "client/vengineclient_impl.h"
    "client/datablock_receiver.cpp"
    "client/datablock_receiver.h"
    "client/bridge_flag_set.h"
)

add_sources( SOURCE_GROUP "GameUI"
    "${ENGINE_SOURCE_DIR}/gameui/IBrowser.cpp"
    "${ENGINE_SOURCE_DIR}/gameui/IBrowser.h"
    "${ENGINE_SOURCE_DIR}/gameui/IConsole.cpp"
    "${ENGINE_SOURCE_DIR}/gameui/IConsole.h"
    "${ENGINE_SOURCE_DIR}/gameui/IStreamOverlay.cpp"
    "${ENGINE_SOURCE_DIR}/gameui/IStreamOverlay.h"
	"${ENGINE_SOURCE_DIR}/gameui/ITopBar.cpp"
	"${ENGINE_SOURCE_DIR}/gameui/ITopBar.h"
	"${ENGINE_SOURCE_DIR}/gameui/IDevMenu.cpp"
	"${ENGINE_SOURCE_DIR}/gameui/IDevMenu.h"
	"${ENGINE_SOURCE_DIR}/gameui/IDlssNrMenu.cpp"
	"${ENGINE_SOURCE_DIR}/gameui/IDlssNrMenu.h"

    "${ENGINE_SOURCE_DIR}/gameui/imgui_system.cpp"
    "${ENGINE_SOURCE_DIR}/gameui/imgui_system.h"
    "${ENGINE_SOURCE_DIR}/gameui/imgui_surface.cpp"
    "${ENGINE_SOURCE_DIR}/gameui/imgui_surface.h"
)
endif()

add_sources( SOURCE_GROUP "Windows"
    "${ENGINE_SOURCE_DIR}/windows/console.cpp"
    "${ENGINE_SOURCE_DIR}/windows/console.h"
    "${ENGINE_SOURCE_DIR}/windows/id3dx.cpp"
    "${ENGINE_SOURCE_DIR}/windows/id3dx.h"
    "${ENGINE_SOURCE_DIR}/windows/input.cpp"
    "${ENGINE_SOURCE_DIR}/windows/input.h"
    "${ENGINE_SOURCE_DIR}/windows/resource.cpp"
    "${ENGINE_SOURCE_DIR}/windows/resource.h"
    "${ENGINE_SOURCE_DIR}/windows/system.cpp"
    "${ENGINE_SOURCE_DIR}/windows/system.h"
)

if( ${PROJECT_NAME} STREQUAL "engine_cl" )
add_sources( SOURCE_GROUP "Windows"
    "${ENGINE_SOURCE_DIR}/windows/framegen.cpp"
    "${ENGINE_SOURCE_DIR}/windows/framegen.h"
    "${ENGINE_SOURCE_DIR}/windows/dlssnr.cpp"
    "${ENGINE_SOURCE_DIR}/windows/dlssnr.h"
    "${ENGINE_SOURCE_DIR}/windows/dlss_sr.cpp"
    "${ENGINE_SOURCE_DIR}/windows/dlss_sr.h"
    "${ENGINE_SOURCE_DIR}/windows/pso_cache.cpp"
    "${ENGINE_SOURCE_DIR}/windows/pso_cache.h"
)
endif()

add_sources( SOURCE_GROUP "Common"
    "${ENGINE_SOURCE_DIR}/common/callback.cpp"
    "${ENGINE_SOURCE_DIR}/common/callback.h"
    "${ENGINE_SOURCE_DIR}/common/completion.cpp"
    "${ENGINE_SOURCE_DIR}/common/completion.h"
    "${ENGINE_SOURCE_DIR}/common/engine_launcher_api.h"
    "${ENGINE_SOURCE_DIR}/common/experimental.h"
    "${ENGINE_SOURCE_DIR}/common/global.cpp"
    "${ENGINE_SOURCE_DIR}/common/global.h"
    "${ENGINE_SOURCE_DIR}/common/igameserverdata.h"
    "${ENGINE_SOURCE_DIR}/common/netmessages.cpp"
    "${ENGINE_SOURCE_DIR}/common/netmessages.h"
    "${ENGINE_SOURCE_DIR}/common/opcodes.cpp"
    "${ENGINE_SOURCE_DIR}/common/opcodes.h"
    "${ENGINE_SOURCE_DIR}/common/protocol.h"
    "${ENGINE_SOURCE_DIR}/common/pseudodefs.h"
    "${ENGINE_SOURCE_DIR}/common/qlimits.h"
    "${ENGINE_SOURCE_DIR}/common/sdkint.h"
    "${ENGINE_SOURCE_DIR}/common/sdkdefs.h"
    "${ENGINE_SOURCE_DIR}/common/x86defs.h"
    "${ENGINE_SOURCE_DIR}/common/xbox/xboxstubs.h"
)

file( GLOB ENGINE_PUBLIC_HEADERS
    "${ENGINE_SOURCE_DIR}/public/engine/*"
)

if( NOT ${PROJECT_NAME} STREQUAL "engine_ds" )
add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/client_class.h"
    "${ENGINE_SOURCE_DIR}/public/ivrenderview.h"
)
endif()

end_sources()

if( ${PROJECT_NAME} STREQUAL "engine_ds" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "DEDICATED"
)
elseif( ${PROJECT_NAME} STREQUAL "engine_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
)
endif()

target_include_directories( ${PROJECT_NAME} PRIVATE
    "${THIRDPARTY_SOURCE_DIR}/recast/"
    "${THIRDPARTY_SOURCE_DIR}/mbedtls/include"
    "${THIRDPARTY_SOURCE_DIR}/fidelityfx/"
)

endmacro()

add_engine_project( "engine_ds" )
add_engine_project( "engine_cl" )
