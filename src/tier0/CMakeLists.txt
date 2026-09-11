cmake_minimum_required( VERSION 3.16 )

# Separate lib as we aren't implementing the memalloc system!
# This library mainly interfaces the program to the game's
# internal memalloc system. Only link if its getting used or
# is using the game module!!!
#
# Dual products: memoverride (server) + memoverride_cl (client inject).
# memstd.cpp is both_targets_gated -- CRT on client, engine-export on server.
macro( add_memoverride_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

add_sources( SOURCE_GROUP "Runtime"
    "memstd.cpp"
    "memstd.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "memoverride_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_memoverride_lib( "memoverride" )
add_memoverride_lib( "memoverride_cl" )

# Dual tier0: server links "tier0" (engine cmdline export), client links
# "tier0_cl" (standalone commandline_s21.cpp). Shared body TUs gate on CLIENT_DLL.
macro( add_tier0_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Debug"
    "dbg.cpp"
    "fasttimer.cpp"
)

add_sources( SOURCE_GROUP "CPU"
    "cpu.cpp"
    "cpu.h"
    "cputopology.cpp"
    "cputopology.h"
)

if( ${LIB_NAME} STREQUAL "tier0_cl" )
add_sources( SOURCE_GROUP "Runtime"
    "commandline_s21.cpp"
    "commandline.h"
    "crashhandler.cpp"
    "frametask.cpp"
    "jobthread.cpp"
    "memaddr.cpp"
    "memory_patch.cpp"
    "memvalidate.cpp"
    "module.cpp"
    "module_statics.cpp"
    "platform.cpp"
    "sigcache.cpp"
    "threadtools.cpp"
    "tslist.cpp"
    "vtable.cpp"
    "tier0_iface.cpp"
    "utility.cpp"
    "binstream.cpp"
)
else()
add_sources( SOURCE_GROUP "Runtime"
    "commandline.cpp"
    "commandline.h"
    "crashhandler.cpp"
    "frametask.cpp"
    "jobthread.cpp"
    "memaddr.cpp"
    "memory_patch.cpp"
    "memvalidate.cpp"
    "module.cpp"
    "module_statics.cpp"
    "platform.cpp"
    "sigcache.cpp"
    "threadtools.cpp"
    "tslist.cpp"
    "vtable.cpp"
    "tier0_iface.cpp"
    "utility.cpp"
    "binstream.cpp"
)
endif()

file( GLOB TIER0_PUBLIC_HEADERS
    "${ENGINE_SOURCE_DIR}/public/tier0/*"
)

end_sources()

if( ${LIB_NAME} STREQUAL "tier0_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_tier0_lib( "tier0" )
add_tier0_lib( "tier0_cl" )
