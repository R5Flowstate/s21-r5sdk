cmake_minimum_required( VERSION 3.16 )
add_module( "lib" "vpc" "" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "IAppSystem.cpp"
    "IAppSystem.h"
    "ifactory.h"
    "interfaces.cpp"
    "interfaces.h"
)

end_sources()
target_precompile_headers( ${PROJECT_NAME} PRIVATE ${GLOBAL_PCH} )

# Client inject PCH: same headers, but CLIENT_DLL so dual-body ABI headers
# (convar.h, iconvar.h, global.h-style) freeze the S21 half. Shared vpc PCH
# is built without CLIENT_DLL and freezes the S3 half -- that AV's the client
# at ConVar registration (write-null in callback vector grow) and hides
# client-only decls. All *_cl libs + client/engine_cl REUSE_FROM vpc_cl.
add_module( "lib" "vpc_cl" "" ${FOLDER_CONTEXT} TRUE TRUE )
start_sources()
add_sources( SOURCE_GROUP "Private"
    "IAppSystem.cpp"
    "IAppSystem.h"
    "ifactory.h"
    "interfaces.cpp"
    "interfaces.h"
)
end_sources()
target_compile_definitions( vpc_cl PRIVATE "CLIENT_DLL" "GAMEDLL_S21" )
target_precompile_headers( vpc_cl PRIVATE ${GLOBAL_PCH} )
