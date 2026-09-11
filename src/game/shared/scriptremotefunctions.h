#ifndef SCRIPTREMOTEFUNCTIONS_H
#define SCRIPTREMOTEFUNCTIONS_H
//=============================================================================//
//
// Purpose: Script remote-function shared declarations
//
//=============================================================================//
#include "thirdparty/detours/include/idetour.h"

constexpr int SCRIPT_REMOTE_ARG_BUFFER_SIZE = 8192;

inline char(*v_ScriptRemote_AddEntry)(__int64 a1, __int64 a2, char a3, char a4, char a5, void* Src);
inline __int64(*v_ScriptRemote_RegisterName)(__int64 a1, unsigned char* a2);

char ScriptRemote_AddEntry(__int64 a1, __int64 a2, char a3, char a4, char a5, void* Src);
__int64 ScriptRemote_RegisterName(__int64 a1, unsigned char* a2);
void ScriptRemote_ResetExtendedArgBuffer(void);


#endif // SCRIPTREMOTEFUNCTIONS_H
