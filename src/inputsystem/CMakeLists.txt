cmake_minimum_required( VERSION 3.16 )

macro( add_inputsystem_lib LIB_NAME )
	add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

	start_sources()

	add_sources( SOURCE_GROUP "Private"
		"inputstacksystem.cpp"
		"inputstacksystem.h"
		"inputsystem.cpp"
		"inputsystem.h"
	)

	add_sources( SOURCE_GROUP "Public"
		"${ENGINE_SOURCE_DIR}/public/inputsystem/iinputstacksystem.h"
		"${ENGINE_SOURCE_DIR}/public/inputsystem/iinputsystem.h"
		"${ENGINE_SOURCE_DIR}/public/inputsystem/AnalogCode.h"
		"${ENGINE_SOURCE_DIR}/public/inputsystem/ButtonCode.h"
		"${ENGINE_SOURCE_DIR}/public/inputsystem/InputEnums.h"
	)

	end_sources()

	if( ${LIB_NAME} STREQUAL "inputsystem_cl" )
		target_compile_definitions( ${PROJECT_NAME} PRIVATE
			"CLIENT_DLL"
			"GAMEDLL_S21"
		)
	endif()
endmacro()

add_inputsystem_lib( "inputsystem_cl" )
