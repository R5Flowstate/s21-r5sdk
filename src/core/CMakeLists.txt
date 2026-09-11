cmake_minimum_required( VERSION 3.16 )

# Prefer the Windows `py` launcher, then python3/python from PATH. No machine-local
# absolute fallbacks -- set SDK_PYTHON_EXECUTABLE on the cmake command line if needed.
if( NOT SDK_PYTHON_EXECUTABLE )
	find_program( SDK_PYTHON_EXECUTABLE NAMES py python3 python
		DOC "Python launcher for build-time checks"
	)
endif()

macro( add_sdk_project PROJECT_NAME )
add_module( "shared_lib" ${PROJECT_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

if( NOT ${PROJECT_NAME} STREQUAL "server" )
file( GLOB PNG_SOURCES
    "${ENGINE_SOURCE_DIR}/resource/png/*.png"
)
add_sources( SOURCE_GROUP "Resource"
    "${ENGINE_SOURCE_DIR}/resource/r5dev.rc"
    "${PNG_SOURCES}"
)

endif()

add_sources( SOURCE_GROUP "Core"
    "assert.h"
    "init.h"
    "logdef.cpp"
    "logdef.h"
    "logger.cpp"
    "logger.h"
    "r5dev.h"
    "resource.h"
    "shared_pch.h"
    "stdafx.cpp"
    "stdafx.h"
    "termutil.cpp"
    "termutil.h"
)

# Role-specific init/dllmain TUs (do not #include .cpp into dual wrappers -- breaks PCH/type visibility).
if( ${PROJECT_NAME} STREQUAL "client" )
add_sources( SOURCE_GROUP "Core"
    "init_client.cpp"
    "dllmain_client.cpp"
)
else()
add_sources( SOURCE_GROUP "Core"
    "init_server.cpp"
    "dllmain_server.cpp"
)
endif()

if( ${PROJECT_NAME} STREQUAL "client" )
add_sources( SOURCE_GROUP "Core"
    "detour_registry.cpp"
    "detour_registry.h"
    "detour_validator.cpp"
    "detour_validator.h"
)
endif()

target_link_libraries( ${PROJECT_NAME} PRIVATE
    "advapi32.lib"
    "bcrypt.lib"
    "crypt32.lib"
    "dbghelp.lib"
    "wldap32.lib"
    "ws2_32.lib"
    "Rpcrt4.lib"
    "iphlpapi.lib"
    "Winmm.lib"

    "boost_charconv"
    "boost_regex"

    "vpklib"
    "mathlib"

    "SigCache_Pb"
    "NetCon_Pb"

    "liblzham"
    "libzstd"
    "liblz4"

    "libdetours"
    "libcurl"
    "libprotobuf"
    "libspdlog"
    "libdetour"
    "navdebugutils"
    "navsharedcommon"

    "EAThread"
    "DirtySDK"

    "libmbedcrypto"
    "libmbedtls"
    "libmbedx509"
)

# Dual foundation + dual-body shared: client inject links *_cl (CLIENT_DLL so
# strict dual-body TUs compile the live-client half). Server keeps plain libs.
# Every lib whose TUs reach a dual-ABI public header (convar.h, iconvar.h,
# ivscript.h, gametrace.h, basehandle.h) must appear on both sides -- a plain
# lib here would put a second ConVar/ConCommand layout in the same binary.
if( ${PROJECT_NAME} STREQUAL "client" )
target_link_libraries( ${PROJECT_NAME} PRIVATE
    "vpc_cl"
    "memoverride_cl"
    "tier0_cl"
    "tier1_cl"
    "tier2_cl"
    "launcher_cl"
    "appframework_cl"
    "vstdlib_cl"
    "vphysics_cl"
    "rson_cl"
    "playlists_cl"
    "stryder_cl"
    "pluginsystem_cl"
    "localize_cl"
    "game_shared_static_cl"
    "EbisuSDK_cl"
    "rtech_game_cl"
    "filesystem_cl"
    "datacache_cl"
    # spire's ConVars are dual-ABI; a plain networksystem here reads m_pParent
    # from the S3 offset while tier1_cl writes it at the S21 one.
    "networksystem_cl"
    # sqapi/sqvm are dual-body too; a plain vscript here gives the client the S3
    # stack layout and AVs in sq_pushbool the first time a native runs.
    "vscript_cl"
)
else()
target_link_libraries( ${PROJECT_NAME} PRIVATE
    "vpc"
    "memoverride"
    "tier0"
    "tier1"
    "tier2"
    "launcher"
    "appframework"
    "vstdlib"
    "vphysics"
    "rson"
    "playlists"
    "stryder"
    "pluginsystem"
    "localize"
    "game_shared_static"
    "EbisuSDK"
    "rtech_game"
    "filesystem"
    "datacache"
    "networksystem"
    "vscript"
)
endif()

if( NOT ${PROJECT_NAME} STREQUAL "server" )
target_link_libraries( ${PROJECT_NAME} PRIVATE
    "libimgui"

    "milessdk"


    "studiorender"

    "vguimatsurface"
    "vgui"
    "rui"
    "datatable_cl"

    "d3d11.lib"
)
target_link_libraries( ${PROJECT_NAME} PRIVATE
    "codecs_cl"
    "inputsystem_cl"
    "materialsystem_cl"
)
endif()

if( NOT ${PROJECT_NAME} STREQUAL "client" )
target_link_libraries( ${PROJECT_NAME} PRIVATE
    "libjwt"
)
endif()

# Bridge products only: client.dll + server.dll (no hybrid gamesdk product).
if( ${PROJECT_NAME} STREQUAL "server" )
end_sources()

target_link_libraries( ${PROJECT_NAME} PRIVATE
    "materialsystem_nodx" # Needs the No-DirectX version for patching.
    "engine_ds"
    "server_static"
    # Required for V_Datatable::Detour and the disk-CSV datatable override.
    # datatable.cpp has no client-only deps (only core/tier1/filesystem) so it
    # links cleanly into the dedicated server. Without this, V_Datatable would
    # have to be skipped on server -- but its GetFun()/GetVar() populate inline
    # function pointers (v_Script_GetDatatableRowCount, v_HashNameAligned, etc.)
    # that game_shared_static's vscript_shared.cpp calls unconditionally.
    "datatable"
)
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "MATERIALSYSTEM_NODX"
    "SERVER_DLL"
    "DEDICATED"
)
# Target name "server" already emits server.dll (no OUTPUT_NAME override).

elseif( ${PROJECT_NAME} STREQUAL "client" )
# Inject client.dll at game root (not legacy bin/x64_retail game-module layout).
end_sources()

target_link_libraries( ${PROJECT_NAME} PRIVATE
    "engine_cl"
    "client_static"
)
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
# Client product + every client-only lib it links. core is add_subdirectory'd
# last, so these targets already exist. Plain (server) twins keep /EHsc.
apply_client_seh_options( ${PROJECT_NAME} )
apply_client_seh_options( client_static )
apply_client_seh_options( engine_cl )
apply_client_seh_options( game_shared_static_cl )
apply_client_seh_options( tier0_cl )
apply_client_seh_options( tier1_cl )
apply_client_seh_options( tier2_cl )
apply_client_seh_options( launcher_cl )
apply_client_seh_options( appframework_cl )
apply_client_seh_options( vstdlib_cl )
apply_client_seh_options( vphysics_cl )
apply_client_seh_options( rson_cl )
apply_client_seh_options( playlists_cl )
apply_client_seh_options( stryder_cl )
apply_client_seh_options( pluginsystem_cl )
apply_client_seh_options( localize_cl )
apply_client_seh_options( datatable_cl )
apply_client_seh_options( studiorender )
apply_client_seh_options( vguimatsurface )
apply_client_seh_options( vgui )
apply_client_seh_options( rui )
apply_client_seh_options( memoverride_cl )
apply_client_seh_options( EbisuSDK_cl )
apply_client_seh_options( rtech_game_cl )
apply_client_seh_options( filesystem_cl )
apply_client_seh_options( datacache_cl )
apply_client_seh_options( networksystem_cl )
apply_client_seh_options( vscript_cl )
apply_client_seh_options( codecs_cl )
apply_client_seh_options( inputsystem_cl )
apply_client_seh_options( materialsystem_cl )

else()
message( FATAL_ERROR "add_sdk_project: unknown product '${PROJECT_NAME}' (expected client or server)" )
endif()

target_include_directories( ${PROJECT_NAME} PRIVATE
	"${THIRDPARTY_SOURCE_DIR}/dirtysdk/include/"
	"${THIRDPARTY_SOURCE_DIR}/ea/"
)

target_link_options( ${PROJECT_NAME} PRIVATE
    "/STACK:8000000" # Match game executable stack reserve size

    # The only reason this has to be done, is because
    # the _xxxx_dbg malloc functions could otherwise
    # throw a linker error, as they are already defined
    # in the CRT library, but there doesn't seem to be
    # a standard way to override these, unlike the
    # _xxxx_base malloc functions. Since this is only
    # relevant for debug builds, this linker flag should
    # only be toggled for DEBUG!!!
    $<$<CONFIG:Debug>:/FORCE:MULTIPLE>
)

# Fail the build on duplicate ConVar/ConCommand names within one product.
# Absent script is a skip so a tree without tools/ still builds.
set( CONVAR_CHECK_SCRIPT "${ENGINE_SOURCE_DIR}/../tools/check_convar_names.py" )
if( SDK_PYTHON_EXECUTABLE AND EXISTS "${CONVAR_CHECK_SCRIPT}" )
	add_custom_command( TARGET ${PROJECT_NAME} PRE_BUILD
		COMMAND "${SDK_PYTHON_EXECUTABLE}"
			"${CONVAR_CHECK_SCRIPT}"
			--product ${PROJECT_NAME}
			--root "${ENGINE_SOURCE_DIR}/.."
		COMMENT "check_convar_names (${PROJECT_NAME})"
		VERBATIM
	)
elseif( SDK_PYTHON_EXECUTABLE )
	message( STATUS "check_convar_names: no script; skipping for ${PROJECT_NAME}" )
else()
	message( STATUS "check_convar_names: no python found; skipping for ${PROJECT_NAME}" )
endif()

add_custom_command( TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -P ${ENGINE_SOURCE_DIR}/cmake/PostBuild.cmake
)

if( ${PROJECT_NAME} STREQUAL "server" )
set_target_properties( ${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_COMMAND "r5apex_ds.exe"
)
else()
set_target_properties( ${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_COMMAND "r5apex.exe"
)
endif()

if( ${PROJECT_NAME} STREQUAL "client" )
set_target_properties( ${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_COMMAND_ARGUMENTS "-wconsole -ansicolor -dev -devsdk -noserverdll"
)
else()
set_target_properties( ${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_COMMAND_ARGUMENTS "-wconsole -ansicolor -dev -devsdk"
    VS_DEBUGGER_WORKING_DIRECTORY "$(ProjectDir)../../../${BUILD_OUTPUT_DIR}/"
)
endif()

set_target_properties( ${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$(ProjectDir)../../../${BUILD_OUTPUT_DIR}/"
)

r5sdk_attach_pe_identity( ${PROJECT_NAME} )

endmacro()

# Bridge products: client.dll + server.dll (dedicated server inject).
add_sdk_project( "client" )
add_sdk_project( "server" )
