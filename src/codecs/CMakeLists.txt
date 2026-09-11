cmake_minimum_required( VERSION 3.16 )

# Dual product: codecs (server) + codecs_cl (client inject, CLIENT_DLL for
# strict dual-body Miles/client audio path).
macro( add_codecs_lib LIB_NAME )
	add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

	start_sources()

	add_sources( SOURCE_GROUP "Bink"
		"bink/bink_impl.cpp"
		"bink/bink_impl.h"
	)

	add_sources( SOURCE_GROUP "Miles"
		"miles/miles_types.h"
	)

	if( ${LIB_NAME} STREQUAL "codecs_cl" )
		add_sources( SOURCE_GROUP "Miles"
			"miles/miles_banklist.cpp"
			"miles/miles_banklist.h"
		)
	endif()

	# FMOD disabled for S21 -- Miles only (fmodstudio.dll LoadLibrary 0x7E trap).

	add_sources( SOURCE_GROUP "Public"
		"${ENGINE_SOURCE_DIR}/public/avi/iavi.h"
		"${ENGINE_SOURCE_DIR}/public/avi/ibik.h"
	)

	end_sources()

	target_include_directories( ${PROJECT_NAME} PRIVATE
		"${ENGINE_SOURCE_DIR}/tier0/"
		"${ENGINE_SOURCE_DIR}/tier1/"
	)
	target_link_libraries( ${PROJECT_NAME} PRIVATE "winmm.lib" )

	if( ${LIB_NAME} STREQUAL "codecs_cl" )
		target_compile_definitions( ${PROJECT_NAME} PRIVATE
			"CLIENT_DLL"
			"GAMEDLL_S21"
		)
	endif()
endmacro()

add_codecs_lib( "codecs_cl" )
