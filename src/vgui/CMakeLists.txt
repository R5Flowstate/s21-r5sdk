cmake_minimum_required( VERSION 3.16 )
# Client-inject only (core links this under NOT server), so the whole lib takes
# the CLIENT_DLL half and the vpc_cl PCH -- otherwise its TUs carry the other
# ConVar/ConCommand layout into client.dll alongside tier1_cl's.
add_module( "lib" "vgui" "vpc_cl" ${FOLDER_CONTEXT} TRUE TRUE )
target_compile_definitions( ${PROJECT_NAME} PRIVATE "CLIENT_DLL" "GAMEDLL_S21" )

start_sources()

add_sources( SOURCE_GROUP "Runtime"
    "vgui_baseui_interface.cpp"
    "vgui_baseui_interface.h"
)

add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/vgui/vgui.h"
    "${ENGINE_SOURCE_DIR}/public/vgui/ienginevgui.h"
)

add_sources( SOURCE_GROUP "Debug"
    "vgui_debugpanel.cpp"
    "vgui_debugpanel.h"
)

add_sources( SOURCE_GROUP "Controls"
    "vgui_controls/RichText.cpp"
)

end_sources()
