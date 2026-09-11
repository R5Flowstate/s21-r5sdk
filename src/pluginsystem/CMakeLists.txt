cmake_minimum_required( VERSION 3.16 )
# Dual pluginsystem: plain (server) + _cl (client inject). Both TUs reach the
# dual-ABI convar.h and modsystem.cpp reaches ivscript.h.
macro( add_pluginsystem_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "modsystem.cpp"
    "modsystem.h"
    "pluginsystem.cpp"
    "pluginsystem.h"
)

add_sources( SOURCE_GROUP "Public"
    "ipluginsystem.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "pluginsystem_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_pluginsystem_lib( "pluginsystem" )
add_pluginsystem_lib( "pluginsystem_cl" )
