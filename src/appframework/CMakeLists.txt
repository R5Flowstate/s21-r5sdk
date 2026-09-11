cmake_minimum_required( VERSION 3.16 )
# Dual appframework: plain (server) + _cl (client inject). IAppSystemGroup.cpp
# reaches the dual-ABI convar.h.
macro( add_appframework_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "IAppSystemGroup.cpp"
)

add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/appframework/IAppSystem.h"
    "${ENGINE_SOURCE_DIR}/public/appframework/IAppSystemGroup.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "appframework_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_appframework_lib( "appframework" )
add_appframework_lib( "appframework_cl" )
