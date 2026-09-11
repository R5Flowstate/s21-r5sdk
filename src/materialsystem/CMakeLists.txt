cmake_minimum_required( VERSION 3.16 )

# Dual product: materialsystem + materialsystem_cl (CLIENT_DLL for dual-body
# cmaterialsystem client half / VMaterialMissingLogS21).
macro( add_materialsystem_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "cmaterialglue.cpp"
    "cmaterialglue.h"
    "cmaterialsystem.cpp"
    "cmaterialsystem.h"
    "cmatrendercontext.cpp"
    "cmatrendercontext.h"
    "cmatqueuedrendercontext.h"
    "constbuffer.cpp"
    "constbuffer.h"
    "cshaderglue.h"
    "meshbuilder.cpp"
    "texturestreaming.cpp"
    "texturestreaming.h"
)

add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/materialsystem/imaterial.h"
    "${ENGINE_SOURCE_DIR}/public/materialsystem/imaterialinternal.h"
    "${ENGINE_SOURCE_DIR}/public/materialsystem/imaterialsystem.h"
    "${ENGINE_SOURCE_DIR}/public/materialsystem/meshbuilder.h"
)

if( ${LIB_NAME} STREQUAL "materialsystem_cl" )
add_sources( SOURCE_GROUP "Private"
    "shader_teardown.cpp"
    "shader_teardown.h"
    "texture_stream_abort_free.cpp"
    "texture_stream_abort_free.h"
)
endif()

end_sources()

if( ${LIB_NAME} STREQUAL "materialsystem_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_materialsystem_lib( "materialsystem_cl" )

add_module( "lib" "materialsystem_nodx" "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "cmaterialsystem.cpp"
    "cmaterialsystem.h"
    "meshbuilder.cpp"
)

add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/materialsystem/imaterialsystem.h"
    "${ENGINE_SOURCE_DIR}/public/materialsystem/imatrendercontext.h"
    "${ENGINE_SOURCE_DIR}/public/materialsystem/meshbuilder.h"
)

end_sources()

target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "MATERIALSYSTEM_NODX"
)