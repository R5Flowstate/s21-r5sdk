cmake_minimum_required( VERSION 3.16 )
# Client-inject only: take the CLIENT_DLL half + vpc_cl PCH so the dual-ABI
# public headers match the rest of client.dll.
add_module( "lib" "vguimatsurface" "vpc_cl" ${FOLDER_CONTEXT} TRUE TRUE )
target_compile_definitions( ${PROJECT_NAME} PRIVATE "CLIENT_DLL" "GAMEDLL_S21" )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "MatSystemSurface.cpp"
    "MatSystemSurface.h"
)

end_sources()
