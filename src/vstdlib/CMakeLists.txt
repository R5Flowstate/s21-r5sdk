cmake_minimum_required( VERSION 3.16 )
# Dual vstdlib: plain (server + tools) + _cl (client inject). All four TUs
# reach the dual-ABI convar.h; concommandhash walks ConCommandBase directly.
macro( add_vstdlib_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "autocompletefilelist.cpp"
    "autocompletefilelist.h"
    "concommandhash.cpp"
    "concommandhash.h"
    "keyvaluessystem.cpp"
    "keyvaluessystem.h"
    "random.cpp"
    "random.h"
)

add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/vstdlib/ikeyvaluessystem.h"
)

end_sources()

target_include_directories( ${PROJECT_NAME} PRIVATE
    "${ENGINE_SOURCE_DIR}/tier0/"
    "${ENGINE_SOURCE_DIR}/tier1/"
)

if( ${LIB_NAME} STREQUAL "vstdlib_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_vstdlib_lib( "vstdlib" )
add_vstdlib_lib( "vstdlib_cl" )
