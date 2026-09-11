cmake_minimum_required( VERSION 3.16 )

macro( add_filesystem_lib LIB_NAME )
	add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

	start_sources()

	add_sources( SOURCE_GROUP "Private"
		"basefilesystem.cpp"
		"basefilesystem.h"
		"filesystem.cpp"
		"filesystem.h"
	)

	file( GLOB FILESYSTEM_PUBLIC_HEADERS
		"${ENGINE_SOURCE_DIR}/public/filesystem/*"
	)

	end_sources()

	target_include_directories( ${PROJECT_NAME} PRIVATE
		"${ENGINE_SOURCE_DIR}/tier0/"
		"${ENGINE_SOURCE_DIR}/tier1/"
	)

	if( ${LIB_NAME} STREQUAL "filesystem_cl" )
		target_compile_definitions( ${PROJECT_NAME} PRIVATE
			"CLIENT_DLL"
			"GAMEDLL_S21"
		)
	endif()
endmacro()

add_filesystem_lib( "filesystem" )
add_filesystem_lib( "filesystem_cl" )

add_module( "lib" "filesystem_std" "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
	"filesystem_std.cpp"
	"filesystem_std.h"
)

file( GLOB FILESYSTEM_PUBLIC_HEADERS
	"${ENGINE_SOURCE_DIR}/public/filesystem/*"
)

end_sources()

target_include_directories( ${PROJECT_NAME} PRIVATE
	"${ENGINE_SOURCE_DIR}/tier0/"
	"${ENGINE_SOURCE_DIR}/tier1/"
)
