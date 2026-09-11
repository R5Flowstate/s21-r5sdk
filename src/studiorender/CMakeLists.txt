cmake_minimum_required( VERSION 3.16 )
# Client-inject only: take the CLIENT_DLL half + vpc_cl PCH so the dual-ABI
# public headers match the rest of client.dll.
add_module( "lib" "studiorender" "vpc_cl" ${FOLDER_CONTEXT} TRUE TRUE )
target_compile_definitions( ${PROJECT_NAME} PRIVATE "CLIENT_DLL" "GAMEDLL_S21" )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "studiorendercontext.cpp"
    "studiorendercontext.h"
)

add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/model_types.h"
    "${ENGINE_SOURCE_DIR}/public/studio.h"

    # TODO: These are from bsplib:
    "${ENGINE_SOURCE_DIR}/public/bspflags.h"
    "${ENGINE_SOURCE_DIR}/public/worldsize.h"
)

end_sources()
