cmake_minimum_required( VERSION 3.16 )

# Dual product: rtech_game (server) + rtech_game_cl (client inject with
# CLIENT_DLL so dual-body pakstate/pakencode take live client halves).
macro( add_rtech_game_lib LIB_NAME )
	add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

	start_sources()

	add_sources( SOURCE_GROUP "Core"
		"core/strutils.cpp"
		"core/strutils.h"
	)

	add_sources( SOURCE_GROUP "Async"
		"async/asyncio.cpp"
		"async/asyncio.h"
	)

	add_sources( SOURCE_GROUP "Pak"
		"pak/pakparse.cpp"
		"pak/pakparse.h"
		"pak/pakalloc.cpp"
		"pak/pakalloc.h"
		"pak/pakencode.cpp"
		"pak/pakencode.h"
		"pak/pakdecode.cpp"
		"pak/pakdecode.h"
		"pak/pakpatch.cpp"
		"pak/pakpatch.h"
		"pak/pakstate.cpp"
		"pak/pakstate.h"
		"pak/pakstream.cpp"
		"pak/pakstream.h"
		"pak/paktools.cpp"
		"pak/paktools.h"
	)

	add_sources( SOURCE_GROUP "Pak/Server_Capacity"
		"pak/stlt_field_rescue.cpp"
		"pak/stlt_field_rescue.h"
		"pak/camo_skins_cap.cpp"
		"pak/camo_skins_cap.h"
		"pak/layout_cache_grow.cpp"
		"pak/layout_cache_grow.h"
		"pak/string_table_grow.cpp"
		"pak/string_table_grow.h"
		"pak/engine_soft_warnings.cpp"
		"pak/engine_soft_warnings.h"
		"pak/signon_buffer_grow.cpp"
		"pak/signon_buffer_grow.h"
	)

	add_sources( SOURCE_GROUP "Pak/Client"
		"pak/rpak_observe.cpp"
		"pak/rpak_observe.h"
		"pak/rpak_sigbypass.cpp"
		"pak/rpak_sigbypass.h"
		"pak/pak_opt_stream_drop.cpp"
		"pak/pak_opt_stream_drop.h"
		"pak/settings_disk.cpp"
		"pak/settings_disk.h"
	)

	if( ${LIB_NAME} STREQUAL "rtech_game_cl" )
	add_sources( SOURCE_GROUP "Pak/Client"
		"pak/pak_lobby_world.cpp"
		"pak/pak_lobby_world.h"
		"pak/pak_census.cpp"
		"pak/pak_census.h"
	)
	add_sources( SOURCE_GROUP "Vfx"
		"vfx_alias.cpp"
		"vfx_alias.h"
		"efct_child_link.cpp"
		"efct_child_link.h"
	)
	endif()

	add_sources( SOURCE_GROUP "Public"
		"${ENGINE_SOURCE_DIR}/public/rtech/iasync.h"
		"${ENGINE_SOURCE_DIR}/public/rtech/ipakfile.h"
		"${ENGINE_SOURCE_DIR}/public/rtech/istreamdb.h"
	)

	end_sources()

	target_include_directories( ${PROJECT_NAME} PRIVATE
		"${THIRDPARTY_SOURCE_DIR}/dirtysdk/include/"
		"${THIRDPARTY_SOURCE_DIR}/ea/"
	)

	if( ${LIB_NAME} STREQUAL "rtech_game_cl" )
		target_compile_definitions( ${PROJECT_NAME} PRIVATE
			"CLIENT_DLL"
			"GAMEDLL_S21"
		)
	else()
		# On the plain (server) lib, force CLIENT_DLL only on client-only TU
		# objects so role gates match inject registration without flipping
		# dual-body shared pak sources to the client half.
		set_source_files_properties(
			"pak/rpak_observe.cpp"
			"pak/rpak_sigbypass.cpp"
			"pak/pak_opt_stream_drop.cpp"
			PROPERTIES COMPILE_DEFINITIONS "CLIENT_DLL;GAMEDLL_S21"
		)
	endif()
endmacro()

add_rtech_game_lib( "rtech_game" )
add_rtech_game_lib( "rtech_game_cl" )

# Dual: plain (server) + _cl (client inject). rson.cpp reaches the dual-ABI
# convar.h.
macro( add_rson_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "RSON"
	"rdf/rson.cpp"
)

add_sources( SOURCE_GROUP "Public"
	"${ENGINE_SOURCE_DIR}/public/rtech/rson.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "rson_cl" )
	target_compile_definitions( ${PROJECT_NAME} PRIVATE
		"CLIENT_DLL"
		"GAMEDLL_S21"
	)
endif()
endmacro()

add_rson_lib( "rson" )
add_rson_lib( "rson_cl" )

# Dual: plain (server) + _cl (client inject). playlists.cpp reaches the dual-ABI
# convar.h and ivscript.h.
macro( add_playlists_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Source"
	"playlists/playlists.cpp"
	"playlists/playlists.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "playlists_cl" )
	target_compile_definitions( ${PROJECT_NAME} PRIVATE
		"CLIENT_DLL"
		"GAMEDLL_S21"
	)
endif()
endmacro()

add_playlists_lib( "playlists" )
add_playlists_lib( "playlists_cl" )

# Dual: plain (server, V_Datatable + disk-CSV override) + _cl (client inject).
# datatable.cpp reaches the dual-ABI convar.h.
macro( add_datatable_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Source"
	"datatable/datatable.cpp"
	"datatable/datatable.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "datatable_cl" )
	target_compile_definitions( ${PROJECT_NAME} PRIVATE
		"CLIENT_DLL"
		"GAMEDLL_S21"
	)
endif()
endmacro()

add_datatable_lib( "datatable" )
add_datatable_lib( "datatable_cl" )

# Client-inject only: take the CLIENT_DLL half + vpc_cl PCH so the dual-ABI
# public headers match the rest of client.dll.
add_module( "lib" "rui" "vpc_cl" ${FOLDER_CONTEXT} TRUE TRUE )
target_compile_definitions( ${PROJECT_NAME} PRIVATE "CLIENT_DLL" "GAMEDLL_S21" )

start_sources()

add_sources( SOURCE_GROUP "Source"
	"rui/rui.cpp"
	"rui/rui.h"
)

end_sources()

# Dual: plain (server) + _cl (client inject). stryder.cpp reaches the dual-ABI
# convar.h.
macro( add_stryder_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Source"
	"stryder/stryder.cpp"
	"stryder/stryder.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "stryder_cl" )
	target_compile_definitions( ${PROJECT_NAME} PRIVATE
		"CLIENT_DLL"
		"GAMEDLL_S21"
	)
endif()
endmacro()

add_stryder_lib( "stryder" )
add_stryder_lib( "stryder_cl" )
