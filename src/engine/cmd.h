#if defined(CLIENT_DLL)
#ifndef CMD_H
#define CMD_H
#include "tier1/commandbuffer.h"

#define MAX_EXECUTION_MARKERS 2048

typedef enum
{
	eCmdExecutionMarker_Enable_FCVAR_SERVER_CAN_EXECUTE = 'a',
	eCmdExecutionMarker_Disable_FCVAR_SERVER_CAN_EXECUTE = 'b',

	eCmdExecutionMarker_Enable_FCVAR_CLIENTCMD_CAN_EXECUTE = 'c',
	eCmdExecutionMarker_Disable_FCVAR_CLIENTCMD_CAN_EXECUTE = 'd'
} ECmdExecutionMarker;

//-----------------------------------------------------------------------------
// Purpose: Returns current player calling this function
// Output: ECommandTarget_t - 
//-----------------------------------------------------------------------------
FORCEINLINE ECommandTarget_t Cbuf_GetCurrentPlayer(void)
{
	// Always returns 'CBUF_FIRST_PLAYER' in Respawn's code.
	return ECommandTarget_t::CBUF_FIRST_PLAYER;
}

extern bool Cbuf_HasRoomForExecutionMarkers(const int cExecutionMarkers);
extern bool Cbuf_AddTextWithMarkers(const char* text, const ECmdExecutionMarker markerLeft, const ECmdExecutionMarker markerRight);


/* ==== COMMAND_BUFFER ================================================================================================================================================== */
inline void(*Cbuf_AddText)(ECommandTarget_t eTarget, const char* pText, cmd_source_t cmdSource);
inline void(*Cbuf_AddExecutionMarker)(ECommandTarget_t target, ECmdExecutionMarker marker);
inline void(*Cbuf_Execute)(void);
inline void(*v_Cmd_Dispatch)(ECommandTarget_t eTarget, const ConCommandBase* pCmdBase, const CCommand* pCommand, bool bCallBackupCallback);
inline bool(*v_Cmd_ForwardToServer)(const CCommand* pCommand);

// S21 Cmd_ExecuteString(player, CCommand*, source). CCommand: +0x04 argc, +0x10 raw[1024], +0x410 argv[0].
inline __int64 (*v_S21_Cmd_ExecuteString)(unsigned int player, void* parsedCmd, int source);

// Connect worker: CHAR** address. CClientState vtable slot 11 is the network dispatcher.
inline void (*v_S21_Connect_Worker)(void** addrPtrPtr);

//---------------------------------------------------------------------
// NET_SendPacket public a3 is netadr_t*. Loopback is type==1. SetSignonState(self, state, spawncount, a4).
//---------------------------------------------------------------------
inline __int64 (*v_S21_NET_SendPacket_Public)(__int64 a1, unsigned int a2, __int64 a3, const void* a4, unsigned int a5, unsigned int a6);
inline __int64 (*v_S21_NET_SendPacket_Inner)(__int64 a1, __int64 a2, __int64 a3, const void* a4, int a5, unsigned int* a6, char a7);
// Every call site passes 4 qwords; fixed 4-arg is MS x64 ABI compatible.
inline void   (*v_S21_NET_SendLoopback)(unsigned int a1, int a2, const void* a3, __int64 a4);
inline char   (*v_S21_CBaseClient_SetSignonState)(__int64 a1, unsigned int a2, int a3, __int64 a4);

extern CCommandBuffer** s_pCommandBuffer;
extern LPCRITICAL_SECTION s_pCommandBufferMutex;

extern CUtlVector<int>* g_pExecutionMarkers;

#endif // CMD_H
#else // !CLIENT_DLL
#ifndef CMD_H
#define CMD_H
#include "tier1/commandbuffer.h"

#define MAX_EXECUTION_MARKERS 2048

typedef enum
{
	eCmdExecutionMarker_Enable_FCVAR_SERVER_CAN_EXECUTE = 'a',
	eCmdExecutionMarker_Disable_FCVAR_SERVER_CAN_EXECUTE = 'b',

	eCmdExecutionMarker_Enable_FCVAR_CLIENTCMD_CAN_EXECUTE = 'c',
	eCmdExecutionMarker_Disable_FCVAR_CLIENTCMD_CAN_EXECUTE = 'd'
} ECmdExecutionMarker;

//-----------------------------------------------------------------------------
// Purpose: Returns current player calling this function
// Output: ECommandTarget_t - 
//-----------------------------------------------------------------------------
FORCEINLINE ECommandTarget_t Cbuf_GetCurrentPlayer(void)
{
	// Always returns 'CBUF_FIRST_PLAYER' in Respawn's code.
	return ECommandTarget_t::CBUF_FIRST_PLAYER;
}

extern bool Cbuf_HasRoomForExecutionMarkers(const int cExecutionMarkers);
extern bool Cbuf_AddTextWithMarkers(const char* text, const ECmdExecutionMarker markerLeft, const ECmdExecutionMarker markerRight);

extern bool Cmd_ExecuteUnrestricted(const char* const pCommandString, const char* const pValueString);

/* ==== COMMAND_BUFFER ================================================================================================================================================== */
inline void(*Cbuf_AddText)(ECommandTarget_t eTarget, const char* pText, cmd_source_t cmdSource);
inline void(*Cbuf_AddExecutionMarker)(ECommandTarget_t target, ECmdExecutionMarker marker);
inline void(*Cbuf_Execute)(void);
inline void(*v_Cmd_Dispatch)(ECommandTarget_t eTarget, const ConCommandBase* pCmdBase, const CCommand* pCommand, bool bCallBackupCallback);
inline bool(*v_Cmd_ForwardToServer)(const CCommand* pCommand);
inline void(*v_CmdRedirectPrintf)(__int64 a1, int a2, const char* pFormat, ...);

extern CCommandBuffer** s_pCommandBuffer;
extern LPCRITICAL_SECTION s_pCommandBufferMutex;

extern CUtlVector<int>* g_pExecutionMarkers;


///////////////////////////////////////////////////////////////////////////////
class VCmd : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Cbuf_AddText", Cbuf_AddText);
		LogFunAdr("Cbuf_AddExecutionMarker", Cbuf_AddExecutionMarker);
		LogFunAdr("Cbuf_Execute", Cbuf_Execute);
		LogFunAdr("Cmd_Dispatch", v_Cmd_Dispatch);
		LogFunAdr("Cmd_ForwardToServer", v_Cmd_ForwardToServer);
		LogFunAdr("CmdRedirectPrintf", v_CmdRedirectPrintf);
		LogVarAdr("s_CommandBuffer", s_pCommandBuffer);
		LogVarAdr("s_CommandBufferMutex", s_pCommandBufferMutex);
		LogVarAdr("g_ExecutionMarkers", g_pExecutionMarkers);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 63 D9 41 8B F8 48 8D 0D ?? ?? ?? ?? 48 8B F2 FF 15 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 41 B9 ?? ?? ?? ??").GetPtr(Cbuf_AddText);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ??").GetPtr(Cbuf_AddExecutionMarker);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 FF 15 ?? ?? ?? ??").GetPtr(Cbuf_Execute);

		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 8B ?? 0C 49 FF C7").FollowNearCallSelf().GetPtr(v_Cmd_Dispatch);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 44 8B 59 04").GetPtr(v_Cmd_ForwardToServer);

		// Redirect printf for status/ping/RCON output. Disp8/disp32
		// wildcards: large-frame variadic prologue + reentrancy-guard load at +0x48748.
		Module_FindPattern(g_GameDll, "4C 8B DC 4D 89 43 ?? 4D 89 4B ?? 55 56 57 48 81 EC ?? ?? ?? ?? 80 B9 ?? ?? ?? ?? ?? 49 8B F8 8B EA 48 8B F1 0F 85 ?? ?? ?? ?? 48 8B 81").GetPtr(v_CmdRedirectPrintf);
	}
	virtual void GetVar(void) const
	{
		s_pCommandBuffer      = CMemory(Cbuf_AddText).FindPattern("48 8D 05").ResolveRelativeAddressSelf(3, 7).RCast<CCommandBuffer**>();
		s_pCommandBufferMutex = CMemory(Cbuf_AddText).FindPattern("48 8D 0D").ResolveRelativeAddressSelf(3, 7).RCast<LPCRITICAL_SECTION>();
		g_pExecutionMarkers   = CMemory(Cbuf_AddExecutionMarker).FindPattern("48 8B 0D").ResolveRelativeAddressSelf(3, 7).RCast<CUtlVector<int>*>();
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // CMD_H
#endif // CLIENT_DLL
