//=============================================================================//
//
// Purpose: frame-thread marshalling for unrestricted command execution
//
//=============================================================================//
#ifndef ENGINE_CMD_FRAME_QUEUE_H
#define ENGINE_CMD_FRAME_QUEUE_H
#ifndef CLIENT_DLL

// Drains commands that Cmd_ExecuteUnrestricted deferred off-thread. Must only
// be called from the server frame thread (CHostState::FrameUpdate).
extern void Cmd_RunUnrestrictedQueue(void);

// Discards deferred commands without executing them (host is shutting down).
extern void Cmd_DropUnrestrictedQueue(void);

#endif // !CLIENT_DLL
#endif // ENGINE_CMD_FRAME_QUEUE_H
