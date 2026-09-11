cmake_minimum_required( VERSION 3.16 )

# Dual product: plain EbisuSDK (dedi/server, no CLIENT_DLL) + EbisuSDK_cl
# (client inject, CLIENT_DLL so strict dual-body takes live client half).
macro( add_ebisu_lib LIB_NAME )
	add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

	start_sources()

	add_sources( SOURCE_GROUP "Private"
		"EbisuSDK.cpp"
		"EbisuSDK.h"
	)

	add_sources( SOURCE_GROUP "Public"
		"${ENGINE_SOURCE_DIR}/public/ebisusdk/EbisuTypes.h"
	)

	if( ${LIB_NAME} STREQUAL "EbisuSDK_cl" )
	add_sources( SOURCE_GROUP "Private"
		"origin_offline.cpp"
		"origin_offline.h"
		"origin_igo.cpp"
		"origin_igo.h"
	)
	endif()

	end_sources()

	target_include_directories( ${PROJECT_NAME} PRIVATE
		"${ENGINE_SOURCE_DIR}/tier0/"
		"${ENGINE_SOURCE_DIR}/tier1/"
	)

	if( ${LIB_NAME} STREQUAL "EbisuSDK_cl" )
		target_compile_definitions( ${PROJECT_NAME} PRIVATE
			"CLIENT_DLL"
			"GAMEDLL_S21"
		)
	endif()
endmacro()

add_ebisu_lib( "EbisuSDK" )
add_ebisu_lib( "EbisuSDK_cl" )
