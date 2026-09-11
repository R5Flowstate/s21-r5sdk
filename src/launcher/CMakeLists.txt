cmake_minimum_required( VERSION 3.16 )
# Dual launcher: plain (server) + _cl (client inject). launcher.cpp / prx.cpp
# reach the dual-ABI convar.h.
macro( add_launcher_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Runtime"
    "launcher.cpp"
    "launcher.h"
    "prx.cpp"
    "prx.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "launcher_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_launcher_lib( "launcher" )
add_launcher_lib( "launcher_cl" )
