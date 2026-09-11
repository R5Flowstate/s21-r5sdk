cmake_minimum_required( VERSION 3.16 )
# Dual localize: plain (server) + _cl (client inject). Both TUs reach the
# dual-ABI convar.h; localize.cpp reaches ivscript.h.
macro( add_localize_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Runtime"
    "localize.cpp"
    "localize.h"
    "localize_disk.cpp"
    "localize_disk.h"
)

add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/localize/ilocalize.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "localize_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_localize_lib( "localize" )
add_localize_lib( "localize_cl" )
