//=============================================================================//
//
// Purpose: VScript player bindings (client)
//
//=============================================================================//
#if defined(CLIENT_DLL)
#ifndef VSCRIPT_PLAYER_H
#define VSCRIPT_PLAYER_H

// No client-side player script surface. The S21 client binds these natives
// itself and the SDK registration path that reached this half no longer exists.

#endif // VSCRIPT_PLAYER_H

#else // !CLIENT_DLL
#ifndef VSCRIPT_PLAYER_H
#define VSCRIPT_PLAYER_H

struct ScriptClassDescriptor_t;

void Script_RegisterPlayerScriptFunctions(ScriptClassDescriptor_t* playerStruct);
void Script_RegisterPlayerScriptSetters(ScriptClassDescriptor_t* playerStruct);
void Script_RegisterDedicatedPlayerScriptFunctions(ScriptClassDescriptor_t* playerStruct);
void VScriptPlayer_LevelShutdown();

// Late-join state burst for L2 S->C replicated ExtraShield* fields.
// Called from CClient::VActivatePlayer (SIGNONSTATE_FULL equivalent).
class CPlayer;
void VScriptPlayer_SendExtraShieldInitialState(const CPlayer* pTarget);

#endif // VSCRIPT_PLAYER_H
#endif // CLIENT_DLL
