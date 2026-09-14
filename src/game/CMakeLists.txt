cmake_minimum_required( VERSION 3.16 )

macro( add_game_project PROJECT_NAME )
add_module( "lib" ${PROJECT_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

if( ${PROJECT_NAME} STREQUAL "game_shared_static"
 OR ${PROJECT_NAME} STREQUAL "game_shared_static_cl" )

add_sources( SOURCE_GROUP "AI"
    "shared/ai_utility_shared.cpp"
    "shared/ai_utility_shared.h"
    "shared/navmesh_debug_draw.cpp"
    "shared/navmesh_debug_draw.h"
)

add_sources( SOURCE_GROUP "Entity"
    "shared/ehandle.h"
)

add_sources( SOURCE_GROUP "Simulation"
    "shared/predictioncopy.h"
    "shared/simtimer.cpp"
    "shared/simtimer.h"
)

add_sources( SOURCE_GROUP "Animation"
    "shared/animation.cpp"
    "shared/animation.h"
    "shared/pose_param.cpp"
    "shared/pose_param.h"
)

add_sources( SOURCE_GROUP "Activity"
    "shared/activity.cpp"
    "shared/activity.h"
    "shared/activitymodifier.cpp"
    "shared/activitymodifier.h"
    # DEDI server activity xlat (S3 ordinals -> S21)
    "shared/activity_s3_to_s21.h"
    "shared/activity_s21_enum.h"
    # CLIENT activity xlat (folded from s21-client)
    "shared/activity_s3_to_s21_client.cpp"
    "shared/activity_s3_to_s21_client.h"
    "shared/activity_s3_to_s21_table.h"
    # DEDI weapon-state xlat (S3 ordinals -> S21), same wire-encode class
    "shared/weapstate_s3_to_s21.h"
)

add_sources( SOURCE_GROUP "Effects"
    "shared/status_effect.h"
    "shared/status_effects_sdk.cpp"
    "shared/status_effects_sdk.h"
    "shared/particleproperty.h"
)

add_sources( SOURCE_GROUP "Physics"
    "shared/collisionproperty.cpp"
    "shared/collisionproperty.h"
)

add_sources( SOURCE_GROUP "Player"
    "shared/player_viewoffset.h"
    "shared/player_melee.h"
)

add_sources( SOURCE_GROUP "Weapon"
    "shared/r1/weapon_playerdata.h"
    "shared/r1/weapon_parse.h"
    "shared/r1/weapon_bolt.cpp"
    "shared/r1/weapon_bolt.h"
    "shared/r1/grapple.h"
    "shared/r1/smartammo.h"
    "shared/weapon_script_vars.cpp"
    "shared/weapon_script_vars.h"
    "shared/weapon_enforce.h"
    "shared/offhand_slots_ext.h"
    "shared/offhand_activation_patches.cpp"
    "shared/offhand_activation_patches.h"
    "shared/offhand_instant_swap.cpp"
    "shared/offhand_instant_swap.h"
    "shared/weapon_dualwield_slot_patch.cpp"
    "shared/weapon_dualwield_slot_patch.h"
    "shared/heap_canary.cpp"
    "shared/heap_canary.h"
    "shared/weapon_legendary_ext.cpp"
    "shared/weapon_legendary_ext.h"
    "shared/weapon_customact_c2s_xlat.cpp"
    "shared/weapon_customact_c2s_xlat.h"
    "shared/weapon_heat.cpp"
    "shared/weapon_heat.h"
    "shared/globalnonrewind_vars.h"
    "shared/deathfield_system.cpp"
    "shared/deathfield_system.h"
    "shared/alliance_compat.cpp"
    "shared/alliance_compat.h"
    "shared/highlight_context.cpp"
    "shared/highlight_context.h"
    "shared/edict_dirty.h"
    "shared/dt_extend.h"
    "shared/dt_extend_diag.h"
    "shared/dt_extend_system14.h"
    "shared/player_extend_sidecar.h"
    "shared/sdk_entity_state.h"
    "shared/scriptnetdata_ext.cpp"
    "shared/scriptnetdata_ext.h"
    "shared/scriptnetdata_limits.h"
)

if( ${PROJECT_NAME} STREQUAL "game_shared_static" )
add_sources( SOURCE_GROUP "Server Only"
    "shared/activity_s3_to_s21.cpp"
    "shared/offhand_slots_ext.cpp"
    "shared/globalnonrewind_vars.cpp"
    "shared/util_shared.cpp"
    "shared/vscript_debug_overlay_shared.cpp"
    "shared/scriptremotefunctions_shared.cpp"
)

add_sources( SOURCE_GROUP "Weapon"
    "shared/dt_extend.cpp"
    "shared/dt_extend_diag.cpp"
    "shared/dt_extend_system14.cpp"
    "shared/bridge_anim_anchor.cpp"
    "shared/player_extend_sidecar.cpp"
)
endif()

add_sources( SOURCE_GROUP "Network"
    "shared/playernet_vars.h"
    "shared/usermessages.h"
)

add_sources( SOURCE_GROUP "Input"
    "shared/usercmd.cpp"
    "shared/usercmd.h"
    "shared/bridge_cmd_seed.cpp"
    "shared/bridge_cmd_seed.h"
    "shared/pingcmd.h"
)

add_sources( SOURCE_GROUP "Utility"
    "shared/imovehelper.h"
    "shared/util_shared.h"
)

add_sources( SOURCE_GROUP "Shared"
    "shared/shared_classnames.h"
    "shared/shareddefs.h"
    "shared/takedamageinfo.h"
    "shared/shared_activity.h"
    "shared/chatbuilder.h"
)

add_sources( SOURCE_GROUP "Script"
    "shared/vscript_shared.cpp"
    "shared/vscript_shared.h"
    "shared/vscript_debug_overlay_shared.h"
    "shared/vscript_gamedll_defs.h"
    "shared/scriptremotefunctions.cpp"
    "shared/scriptremotefunctions.h"
    "shared/scriptremotefunctions_shared.h"
    "shared/cbaseentity_getscriptinstance_client_stub.cpp"
    "shared/vscript_remotefunctions_sdk.h"
    "client/vscript_player.cpp"
    "client/vscript_player.h"
    "client/scriptnetdata_client.cpp"
    "client/scriptnetdata_client.h"
    "client/convar_stubs.cpp"
    "shared/convar_script_compat.cpp"
)

add_sources( SOURCE_GROUP "ModSystem"
    "shared/pluginsystem/modsystem.cpp"
    "shared/pluginsystem/modsystem.h"
)

endif()

if( ${PROJECT_NAME} STREQUAL "server_static" )

add_sources( SOURCE_GROUP "AI"
    "server/ai_basenpc.cpp"
    "server/ai_basenpc.h"
    "server/ai_network.cpp"
    "server/ai_network.h"
    "server/ai_networkmanager.cpp"
    "server/ai_networkmanager.h"
    "server/ai_commands.cpp"
    "server/ai_node.h"
    "server/ai_utility.cpp"
    "server/ai_utility.h"
    "server/ai_task.cpp"
    "server/ai_task.h"
    "server/ai_schedule.cpp"
    "server/ai_schedule.h"
    "server/ai_component.h"
    "server/ai_senses.h"
    "server/ai_localnavigator.h"
    "server/ai_navigator.h"
    "server/ai_navtypes.h"
    "server/ai_movetypes.h"
    "server/ai_moveprobe.h"
    "server/ai_motor.h"
    "server/ai_route.h"
    "server/ai_interest.h"
    "server/ai_waypoint.h"
    "server/ai_pathfinder.h"
    "server/ai_memory.h"
    "server/ai_squad.h"
    "server/ai_tacticalservices.h"
    "server/ai_moveshoot.h"
    "server/ai_battlelocation.h"
    "server/ai_syncedmelee.h"
    "server/ai_shootcover.h"
    "server/ai_hint.h"
    "server/ai_npcstate.h"
    "server/detour_impl.h"
)

add_sources( SOURCE_GROUP "Entity"
    "server/baseanimating.cpp"
    "server/baseanimating.h"
    "server/baseanimatingoverlay.h"
    "server/basecombatcharacter.h"
    "server/baseentity.cpp"
    "server/baseentity.h"
    "server/entitylist.cpp"
    "server/entitylist.h"
    "server/entityoutput.cpp"
    "server/entityoutput.h"
    "server/soundent.cpp"
    "server/soundent.h"
    "server/entitylist_serverbase.h"
)

add_sources( SOURCE_GROUP "Network"
    "server/networkproperty.cpp"
    "server/networkproperty.h"
)

add_sources( SOURCE_GROUP "Player"
    "server/player.cpp"
    "server/player.h"
    "server/player_command.cpp"
    "server/player_command.h"
    "server/bridge_cmd_chain.cpp"
    "server/bridge_cmd_chain.h"
    "server/playerlocaldata.h"
    "server/jetdrive.cpp"
    "server/jetdrive.h"
    "server/track_entity.cpp"
    "server/track_entity.h"
    "server/zipline_extend_state.cpp"
    "server/zipline_extend_state.h"
    "server/context_action.cpp"
    "server/context_action.h"
    "server/player_launch.cpp"
    "server/player_launch.h"
    "server/skydive.cpp"
    "server/skydive.h"
    "server/mantle_boost.cpp"
    "server/mantle_boost.h"
    "server/melee_lunge_probe.cpp"
    "server/melee_lunge_probe.h"
    "server/melee_activity_trace.cpp"
    "server/melee_activity_trace.h"
    "server/movescale_weapon_parity.cpp"
    "server/movescale_weapon_parity.h"
    "server/halfduck_zip_parity.cpp"
    "server/halfduck_zip_parity.h"
    "server/move_sim_trace.cpp"
    "server/move_sim_trace.h"
    "server/surfaceprop_id.cpp"
    "server/surfaceprop_id.h"
    "server/repel_realm_gate.cpp"
    "server/repel_realm_gate.h"
    "server/zipline_cooldown.cpp"
    "server/zipline_cooldown.h"
    "server/zipline_exit_parity.cpp"
    "server/zipline_exit_parity.h"
    "server/poseparam_ext.cpp"
    "server/poseparam_ext.h"
    "server/tapstrafe.cpp"
    "server/tapstrafe.h"
    "server/wallclimb.cpp"
    "server/wallclimb.h"
    "server/trigger_slip_diag.cpp"
    "server/trigger_slip_diag.h"
    "server/trigger_starttouch_dedupe.cpp"
    "server/trigger_starttouch_dedupe.h"
    "server/trigger_clientpredict.cpp"
    "server/trigger_clientpredict.h"
    "server/jumppad_parity.cpp"
    "server/jumppad_parity.h"
    "server/trigger_cannon.cpp"
    "server/trigger_cannon.h"
    "server/trigger_gravity.cpp"
    "server/trigger_gravity.h"
    "server/trigger_updraft.cpp"
    "server/trigger_updraft.h"
    "server/consumable_inv.cpp"
    "server/consumable_inv.h"
    "server/bridge_fire_clock.cpp"
    "server/bridge_fire_clock.h"
    "server/bridge_zoom_gate.cpp"
    "server/bridge_zoom_gate.h"
    "server/weapon_kv_s21_ext.cpp"
    "server/weapon_kv_s21_ext.h"
    "server/bridge_equip_gate.cpp"
    "server/bridge_equip_gate.h"
    "server/bridge_deploy_gate.cpp"
    "server/bridge_deploy_gate.h"
)


add_sources( SOURCE_GROUP "Weapon"
    "server/r1/weapon_x.cpp"
    "server/r1/weapon_x.h"
    "server/r1/blast_pattern.cpp"
    "server/r1/blast_pattern.h"
    "server/infinite_ammo.cpp"
    "server/infinite_ammo.h"
    "server/weapon_ammo_pool_mod.cpp"
    "server/weapon_ammo_pool_mod.h"
    "server/weapon_enforce.cpp"
    "server/weapon_select_mirror.cpp"
    "server/weapon_select_mirror.h"
    "server/weapon_holster_reselect.cpp"
    "server/weapon_holster_reselect.h"
    "server/weapon_realm_follow.cpp"
    "server/weapon_realm_follow.h"
    "server/energize.cpp"
    "server/energize.h"
    "server/akimbo.cpp"
    "server/akimbo.h"
    "server/sndc_alloc.cpp"
    "server/sndc_alloc.h"
)

add_sources( SOURCE_GROUP "Script"
    "server/vscript_server.cpp"
    "server/vscript_server.h"
    "server/vscript_server_natives.cpp"
    "server/vscript_server_natives.h"
    "server/classvar_natives.cpp"
    "server/classvar_natives.h"
    "server/vscript_server_placement.cpp"
    "server/vscript_server_placement.h"
    "shared/scriptremotefunctions_server.h"
    "server/scriptremotefunctions.cpp"
    "server/vscript_remotefunctions_sdk.cpp"
    "server/vscript_remotefunctions_buffer_expand.cpp"
    "server/vscript_remotefunctions_buffer_expand.h"
    "server/sdk_entity_state.cpp"
    "server/scriptnetdata_limits.cpp"
    "server/persistence_ext.cpp"
    "server/persistence_ext.h"
    "server/persistence_basevar_overflow.cpp"
    "server/persistence_basevar_overflow.h"
    "server/passive_changed.cpp"
    "server/passive_changed.h"
    "server/extended_range_use.cpp"
    "server/extended_range_use.h"
    "server/player_overheat.cpp"
    "server/player_overheat.h"
    "server/translocation.cpp"
    "server/translocation.h"
    "server/chatbuilder.cpp"
    "server/chatbuilder.h"
)

add_sources( SOURCE_GROUP "Physics"
    "server/hitbox_debug.cpp"
    "server/hitbox_debug.h"
    "server/headglitch_detect.cpp"
    "server/headglitch_detect.h"
    "server/physics_main.cpp"
    "server/physics_main.h"
)

add_sources( SOURCE_GROUP "Utility"
    "server/cbase.cpp"
    "server/cbase.h"
    "server/gameinterface.cpp"
    "server/gameinterface.h"
    "server/movehelper_server.cpp"
    "server/movehelper_server.h"
    "server/util_server.cpp"
    "server/util_server.h"
    "server/variant_t.cpp"
    "server/variant_t.h"
)

add_sources( SOURCE_GROUP "Recipient Filter"
    "server/recipientfilter.cpp"
    "server/recipientfilter.h"
)

add_sources( SOURCE_GROUP "Sound"
    "server/sound.cpp"
    "server/sound.h"
)

file( GLOB GAME_SERVER_PUBLIC_HEADERS
    "${ENGINE_SOURCE_DIR}/public/game/server/*"
)

endif()

if( ${PROJECT_NAME} STREQUAL "client_static" )

add_sources( SOURCE_GROUP "Entity"
    "client/c_baseentity.h"
    "client/cliententitylist.cpp"
    "client/cliententitylist.h"
    "client/entitylist_clientbase.h"
    "client/icliententityinternal.h"
    "client/body_skin.cpp"
    "client/body_skin.h"
)

add_sources( SOURCE_GROUP "Weapon"
    "client/r1/c_weapon_x.cpp"
    "client/r1/c_weapon_x.h"
    "client/weapon_mod_visual.cpp"
    "client/weapon_mod_visual.h"
)

add_sources( SOURCE_GROUP "Input"
    "client/in_main.cpp"
    "client/input.cpp"
    "client/input.h"
    "client/gamepad.cpp"
    "client/gamepad.h"
    "client/kbutton.h"
)

add_sources( SOURCE_GROUP "Leaf System"
    "client/clientleafsystem.cpp"
    "client/clientleafsystem.h"
)

add_sources( SOURCE_GROUP "Rendering"
    "client/enginesprite.h"
    "client/hud.h"
    "client/viewrender.cpp"
    "client/viewrender.h"
    "client/iviewrender.h"
    "client/fov_limit.cpp"
    "client/fov_limit.h"
    "client/visual_clutter.cpp"
    "client/visual_clutter.h"
)

add_sources( SOURCE_GROUP "Animation"
)

add_sources( SOURCE_GROUP "Utility"
    "client/util_client.cpp"
    "client/util_client.h"
)

add_sources( SOURCE_GROUP "Script"
    "client/vscript_client.cpp"
    "client/vscript_client.h"
    # Client-only send path (ClientScript_Remote_ServerCallFunction).
    "client/concommandcallback.cpp"
    "client/concommandcallback.h"
    "client/classvar_natives.cpp"
    "client/classvar_natives.h"
    "client/fs_1v1_convars.cpp"
    "client/fs_1v1_convars.h"
)

add_sources( SOURCE_GROUP "Viewmodel"
    "client/viewmodel_poseparam.cpp"
    "client/viewmodel_poseparam.h"
)

add_sources( SOURCE_GROUP "Hud"
    "client/hud_basechat.cpp"
    "client/hud_basechat.h"
)

add_sources( SOURCE_GROUP "Zipline"
    "client/c_zipline.cpp"
    "client/c_zipline.h"
)

add_sources( SOURCE_GROUP "RUI"
    "client/ruitracks.cpp"
    "client/ruitracks.h"
    "client/rui_walltime.cpp"
    "client/rui_walltime.h"
)

# CLIENT exclusives folded from s21-client (pred / bridge / diag)
add_sources( SOURCE_GROUP "Prediction"
    "client/pred_diag.cpp"
    "client/pred_diag.h"
    "client/pred_authority.cpp"
    "client/pred_authority.h"
    "client/pred_authority_probes.cpp"
    "client/pred_authority_probes.h"
    "client/mantle_boost.cpp"
    "client/mantle_boost.h"
    "client/dodge_bind.cpp"
    "client/dodge_bind.h"
    "client/aimassist.cpp"
    "client/aimassist.h"
    "client/mantle_boost_rui.cpp"
    "client/mantle_boost_rui.h"
    "client/trigger_cannon.cpp"
    "client/trigger_cannon.h"
    "client/halfduck_zip_parity.cpp"
    "client/halfduck_zip_parity.h"
    "client/move_sim_trace.cpp"
    "client/move_sim_trace.h"
    "client/wallclimb.cpp"
    "client/wallclimb.h"
    "client/melee_lunge_probe.cpp"
    "client/melee_lunge_probe.h"
    "client/melee_activity_trace.cpp"
    "client/melee_activity_trace.h"
    "client/bridge_cmd_seed.cpp"
    "client/bridge_cmd_seed.h"
    "client/bridge_fire_tap.cpp"
    "client/bridge_fire_tap.h"
)

add_sources( SOURCE_GROUP "Grapple Rope"
    "client/grapple_rope_diag.cpp"
    "client/grapple_rope_diag.h"
)

add_sources( SOURCE_GROUP "Jump Pad"
    "client/jumppad_viewpunch_diag.cpp"
    "client/jumppad_viewpunch_diag.h"
)

file( GLOB GAME_CLIENT_PUBLIC_HEADERS
    "${ENGINE_SOURCE_DIR}/public/game/client/*"
)

endif()

file( GLOB GAME_SHARED_PUBLIC_HEADERS
    "${ENGINE_SOURCE_DIR}/public/game/shared/*"
)

end_sources()

if( ${PROJECT_NAME} STREQUAL "server_static" )

target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "SERVER_DLL"
)
elseif( ${PROJECT_NAME} STREQUAL "client_static" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
)
elseif( ${PROJECT_NAME} STREQUAL "game_shared_static"
     OR ${PROJECT_NAME} STREQUAL "game_shared_static_cl" )
# Dual game_shared: plain (server/dedicated bodies) + _cl (CLIENT_DLL bodies).
# Dual-body shared TUs (#if CLIENT_DLL) compile client path only on *_cl.
# Legacy single-lib rebuild: -DR5SDK_UNIFY_CLIENT_SHARED=ON forces CLIENT_DLL
# on plain game_shared_static (client-only worktree builds).
if( ${PROJECT_NAME} STREQUAL "game_shared_static_cl" OR R5SDK_UNIFY_CLIENT_SHARED )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "R5SDK_UNIFY_CLIENT_SHARED"
)
endif()

endif()

target_include_directories( ${PROJECT_NAME} PRIVATE
    "${ENGINE_SOURCE_DIR}/tier0/"
    "${ENGINE_SOURCE_DIR}/tier1/"
    "${THIRDPARTY_SOURCE_DIR}/mbedtls/include"
)

endmacro()

add_game_project( "server_static" )
add_game_project( "client_static" )
add_game_project( "game_shared_static" )
add_game_project( "game_shared_static_cl" )

if( TARGET game_shared_static_cl )
    target_sources( game_shared_static_cl PRIVATE
        "${ENGINE_SOURCE_DIR}/game/shared/dt_extend_client_stub.cpp"
        "${ENGINE_SOURCE_DIR}/game/shared/dt_extend.h"
    )
endif()

