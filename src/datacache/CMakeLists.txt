cmake_minimum_required( VERSION 3.16 )

macro( add_datacache_lib LIB_NAME )
	add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

	start_sources()

	add_sources( SOURCE_GROUP "Private"
		"mdlcache.cpp"
		"mdlcache.h"
	)

	add_sources( SOURCE_GROUP "Public"
		"${ENGINE_SOURCE_DIR}/public/datacache/idatacache.h"
		"${ENGINE_SOURCE_DIR}/public/datacache/imdlcache.h"
	)

	if( ${LIB_NAME} STREQUAL "datacache_cl" )
	add_sources( SOURCE_GROUP "Private"
		"anim_desc.cpp"
		"anim_desc.h"
		"studio_seqdesc.cpp"
		"studio_seqdesc.h"
	)
	endif()

	end_sources()

	target_include_directories( ${PROJECT_NAME} PRIVATE
		"${ENGINE_SOURCE_DIR}/tier0/"
		"${ENGINE_SOURCE_DIR}/tier1/"
	)

	if( ${LIB_NAME} STREQUAL "datacache_cl" )
		target_compile_definitions( ${PROJECT_NAME} PRIVATE
			"CLIENT_DLL"
			"GAMEDLL_S21"
		)
	endif()
endmacro()

add_datacache_lib( "datacache" )
add_datacache_lib( "datacache_cl" )
