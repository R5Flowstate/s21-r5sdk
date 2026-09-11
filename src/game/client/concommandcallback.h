//=============================================================================//
//
// Purpose: grow the ConCommand script-callback pool past the engine's hardcoded
// limit of 50 entries.
//
// RegisterConCommandTriggeredCallback (Squirrel) draws from a fixed pool of
// ConCommandScriptCallback nodes; exhaustion raises "[CLIENT] Can not register
// more than 50 ConCommand callbacks". This hook grows the free list instead.
//
//=============================================================================//

#ifndef GAME_CLIENT_CONCOMMANDCALLBACK_H
#define GAME_CLIENT_CONCOMMANDCALLBACK_H

#include "thirdparty/detours/include/idetour.h"

//-----------------------------------------------------------------------------
// ConCommandScriptCallback node layout (40 bytes / 0x28)
// 0x00: tagSQObject callback (16 bytes)
// 0x10: ConCommand* command (8 bytes)
// 0x18: prev pointer (8 bytes)
// 0x20: next pointer (8 bytes)
//-----------------------------------------------------------------------------
inline constexpr int CONCOMMAND_CALLBACK_NODE_SIZE = 0x28;  // 40 bytes

// How many extra nodes to allocate each time the pool runs out.
inline constexpr int CONCOMMAND_CALLBACK_GROW_COUNT = 50;

//-----------------------------------------------------------------------------
// Original function pointer
//-----------------------------------------------------------------------------
inline __int64(*v_RegisterConCommandTriggeredCallback)(__int64 sqvm);

//-----------------------------------------------------------------------------
// Hook
//-----------------------------------------------------------------------------
__int64 h_RegisterConCommandTriggeredCallback(__int64 sqvm);


#endif // GAME_CLIENT_CONCOMMANDCALLBACK_H
