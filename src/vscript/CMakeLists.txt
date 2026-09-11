cmake_minimum_required( VERSION 3.16 )

# Dual vscript: vscript (server) + vscript_cl (client inject).
#
# squirrel_re/squirrel/sqapi.cpp and sqvm.h are dual-body. Built once without
# CLIENT_DLL and linked into client.dll, the client got the S3 half: the getters
# walk the VM stack at S3 offsets and v->Push writes through _top at 0x78, which
# on S21 is the _stack.data pointer. Pressing Servers AVd inside sq_pushbool.
# The _cl name also selects the vpc_cl PCH (see cmake/Macros.cmake).
macro( add_vscript_lib LIB_NAME )
add_module( "lib" ${LIB_NAME} "vpc" ${FOLDER_CONTEXT} TRUE TRUE )

start_sources()

add_sources( SOURCE_GROUP "Private"
    "vscript.cpp"
    "vscript.h"
)

# Client S21 override TUs (client_target_only). Detour registration is
# client-only (Systems_Init_S21). Shared static lib; dedicated does not
# reference these symbols so objs stay unlinked there. Force CLIENT_DLL /
# GAMEDLL_S21 on the .cpp bodies so #ifndef CLIENT_DLL branches match inject.
add_sources( SOURCE_GROUP "Private/Client"
    "vscript_s21_override.cpp"
    "vscript_s21_override.h"
    "weapon_kv_disk.cpp"
    "weapon_kv_disk.h"
    "vsquirrel_s21.cpp"
    "vsquirrel_s21.h"
)
set_source_files_properties(
    "vscript_s21_override.cpp"
    "weapon_kv_disk.cpp"
    "vsquirrel_s21.cpp"
    PROPERTIES COMPILE_DEFINITIONS "CLIENT_DLL;GAMEDLL_S21"
)

add_sources( SOURCE_GROUP "Squirrel_RE"
    "languages/squirrel_re/vsquirrel.cpp"
    "languages/squirrel_re/vsquirrel.h"
    "languages/squirrel_re/vsquirrel_bridge.cpp"
    "languages/squirrel_re/vsquirrel_bridge.h"
)

add_sources( SOURCE_GROUP "Squirrel_RE/squirrel"
    "languages/squirrel_re/squirrel/sqapi.cpp"
    "languages/squirrel_re/squirrel/sqdebug.cpp"
    #"languages/squirrel_re/squirrel/sqcompiler.cpp"
    "languages/squirrel_re/squirrel/sqfuncstate.cpp"
    #"languages/squirrel_re/squirrel/sqlexer.cpp"
    "languages/squirrel_re/squirrel/sqobject.cpp"
    "languages/squirrel_re/squirrel/sqstring.cpp"
    "languages/squirrel_re/squirrel/sqclass.cpp"
    "languages/squirrel_re/squirrel/sqtable.cpp"
    "languages/squirrel_re/squirrel/sqmem.cpp"
    "languages/squirrel_re/squirrel/sqstate.cpp"
    "languages/squirrel_re/squirrel/sqvm.cpp"
)

add_sources( SOURCE_GROUP "Squirrel_RE/sqstdlib"
    "languages/squirrel_re/sqstdlib/sqstdaux.cpp"
)

add_sources( SOURCE_GROUP "Squirrel_RE/include"
    "languages/squirrel_re/vsquirrel.cpp"
    "languages/squirrel_re/vsquirrel.h"
    #"languages/squirrel_re/include/sqcompiler.h"
    "languages/squirrel_re/include/sqfuncstate.h"
    #"languages/squirrel_re/include/sqlexer.h"
    "languages/squirrel_re/include/sqarray.h"
    "languages/squirrel_re/include/squtils.h"
    "languages/squirrel_re/include/sqobject.h"
    "languages/squirrel_re/include/sqopcodes.h"
    "languages/squirrel_re/include/sqstate.h"
    "languages/squirrel_re/include/sqstdaux.h"
    "languages/squirrel_re/include/sqstdstring.h"
    "languages/squirrel_re/include/sqstring.h"
    "languages/squirrel_re/include/sqclass.h"
    "languages/squirrel_re/include/sqclosure.h"
    "languages/squirrel_re/include/sqfuncproto.h"
    "languages/squirrel_re/include/sqtable.h"
    "languages/squirrel_re/include/squirrel.h"
    "languages/squirrel_re/include/sqvm.h"
)

add_sources( SOURCE_GROUP "Public"
    "${ENGINE_SOURCE_DIR}/public/vscript/ivscript.h"
)

end_sources()

target_include_directories( ${PROJECT_NAME} PRIVATE "${ENGINE_SOURCE_DIR}/vscript/languages/squirrel_re/include/" )

if( ${LIB_NAME} STREQUAL "vscript_cl" )
target_compile_definitions( ${PROJECT_NAME} PRIVATE
    "CLIENT_DLL"
    "GAMEDLL_S21"
)
endif()
endmacro()

add_vscript_lib( "vscript" )
add_vscript_lib( "vscript_cl" )
