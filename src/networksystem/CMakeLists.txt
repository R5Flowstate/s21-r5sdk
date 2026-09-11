cmake_minimum_required( VERSION 3.16 )

# Dual networksystem: networksystem (server) + networksystem_cl (client inject).
#
# spire.cpp declares file-scope ConVars, and ConVar is a dual-ABI type: 0xA0 with
# m_pParent at 0x48 on S3, 0x98 with m_pParent at 0x40 on S21. Built once without
# CLIENT_DLL and linked into client.dll, the S21 constructor in tier1_cl writes
# m_pParent at 0x40 while the FORCEINLINE getters compiled here read it from 0x48,
# which holds m_pszDefaultValue -- so GetInt() dereferences a string literal.
macro( add_networksystem_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "bansystem.cpp"
    "bansystem.h"
    "hostmanager.cpp"
    "hostmanager.h"
    "listmanager.cpp"
    "listmanager.h"
    "spire.cpp"
    "spire.h"
)

add_sources( SOURCE_GROUP "Public"
    "serverlisting.h"
)

end_sources()

if( ${LIB_NAME} STREQUAL "networksystem_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_networksystem_lib( "networksystem" )
add_networksystem_lib( "networksystem_cl" )
