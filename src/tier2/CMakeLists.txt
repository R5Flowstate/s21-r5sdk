cmake_minimum_required( VERSION 3.16 )
# Dual tier2: tier2 (server + tools) + tier2_cl (client inject). curlutils.cpp
# reaches the dual-ABI convar.h, so the client half must be its own object set.
macro( add_tier2_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Utility"
    "crashreporter.cpp"
    "cryptutils.cpp"
    "curlutils.cpp"
    "fileutils.cpp"
    "jsonutils.cpp"
    "meshutils.cpp"
    "renderutils.cpp"
    "socketcreator.cpp"
    "websocket.cpp"
    "zstdutils.cpp"
)

file( GLOB TIER2_PUBLIC_HEADERS
    "${ENGINE_SOURCE_DIR}/public/tier2/*"
)
add_sources( SOURCE_GROUP "Public"
    "${TIER2_PUBLIC_HEADERS}"
)

end_sources()

# MSVC 14.50 LTCG ICE (C1001/LNK1000) in c2.dll on this TU.
set_source_files_properties( "jsonutils.cpp" PROPERTIES
    COMPILE_OPTIONS "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GL->"
)

target_include_directories( ${PROJECT_NAME} PRIVATE 
    "${ENGINE_SOURCE_DIR}/tier0/"
    "${ENGINE_SOURCE_DIR}/tier1/"
)

target_include_directories( ${PROJECT_NAME} PRIVATE 
    "${THIRDPARTY_SOURCE_DIR}/mbedtls/include"
    "${THIRDPARTY_SOURCE_DIR}/dirtysdk/include/"
    "${THIRDPARTY_SOURCE_DIR}/ea/"
)

if( ${LIB_NAME} STREQUAL "tier2_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_tier2_lib( "tier2" )
add_tier2_lib( "tier2_cl" )
