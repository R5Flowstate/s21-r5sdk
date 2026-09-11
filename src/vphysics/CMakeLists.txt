cmake_minimum_required( VERSION 3.16 )
# Dual vphysics: plain (server) + _cl (client inject). Both TUs reach the
# dual-ABI convar.h, and gametrace.h is dual-ABI too.
macro( add_vphysics_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "physics_collide.cpp"
    "physics_collide.h"
    "QHull.cpp"
    "QHull.h"
)

add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/vphysics/vphysics_interface.h"
    "${ENGINE_SOURCE_DIR}/public/vphysics/phyfile.h"
    "${ENGINE_SOURCE_DIR}/public/vphysics/vcollide.h"
    "${ENGINE_SOURCE_DIR}/public/trace.h"
    "${ENGINE_SOURCE_DIR}/public/gametrace.h"
    "${ENGINE_SOURCE_DIR}/public/cmodel.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "vphysics_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_vphysics_lib( "vphysics" )
add_vphysics_lib( "vphysics_cl" )
