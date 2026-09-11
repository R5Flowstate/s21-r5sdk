cmake_minimum_required( VERSION 3.16 )

# S21 loader stub: a minimal DLL mapped into r5apex.exe via a
# patched PE import table. It must stay tiny and dependency-free so the
# Windows loader can always resolve it — any failure to load this DLL
# prevents the game from starting at all.
#
# REUSE_PCH is deliberately empty (third arg): the stub doesn't include
# stdafx.h / shared_pch.h, so we skip the precompiled header reuse. We
# also do NOT link against tier0/tier1/vpc — the stub only needs kernel32
# (implicit), user32 for MessageBoxA, bcrypt for optional worker SHA-256 pins,
# and libdetours for the LauncherMain gate.
add_module( "shared_lib" "loader" "" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "loader.cpp"
    "loader.h"
)

target_link_libraries( ${PROJECT_NAME} PRIVATE
    "user32.lib"    # MessageBoxA for fatal error reporting
    "bcrypt.lib"    # BCrypt SHA-256 for optional worker DLL pin (SEC-LOADER)
    "libdetours"    # LauncherMain detour for the synchronous SDK-init gate
)

end_sources()

target_link_options( ${PROJECT_NAME} PRIVATE
    "/STACK:8000000" # Match game executable stack reserve size
)

r5sdk_attach_pe_identity( ${PROJECT_NAME} )
