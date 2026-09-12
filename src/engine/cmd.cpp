#include "core/stdafx.h"
#include "tier1/cmd.h"
#include "tier1/cvar.h"
#include "tier1/commandbuffer.h"
#include "tier0/threadtools.h"
#include "engine/cmd.h"
#include "engine/cmd_frame_queue.h"
#include <deque>
#include <mutex>
#include <string>

CCommandBuffer** s_pCommandBuffer = nullptr; // array size = ECommandTarget_t::CBUF_COUNT.
LPCRITICAL_SECTION s_pCommandBufferMutex = nullptr;

//=============================================================================
// List of execution markers
//=============================================================================
CUtlVector<int>* g_pExecutionMarkers = nullptr;

//-----------------------------------------------------------------------------
// Purpose: checks if there's room left for execution markers
// Input: cExecutionMarkers - 
// Output: true if there's room for execution markers, false otherwise
//-----------------------------------------------------------------------------
bool Cbuf_HasRoomForExecutionMarkers(const int cExecutionMarkers)
{
	return (g_pExecutionMarkers->Count() + cExecutionMarkers) < MAX_EXECUTION_MARKERS;
}

//-----------------------------------------------------------------------------
// Purpose: adds command text at the end of the command buffer with execution markers
// Input: *pText - 
// markerLeft - 
// markerRight - 
// Output: true if there's room for execution markers, false otherwise
//-----------------------------------------------------------------------------
bool Cbuf_AddTextWithMarkers(const char* const pText, const ECmdExecutionMarker markerLeft, const ECmdExecutionMarker markerRight)
{
	if (Cbuf_HasRoomForExecutionMarkers(2))
	{
		Cbuf_AddExecutionMarker(Cbuf_GetCurrentPlayer(), markerLeft);
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), pText, cmd_source_t::kCommandSrcCode);
		Cbuf_AddExecutionMarker(Cbuf_GetCurrentPlayer(), markerRight);

		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: adds command text at the end of the buffer
//-----------------------------------------------------------------------------
//void Cbuf_AddText(ECommandTarget_t eTarget, const char* pText, int nTickDelay)
//	LOCK_COMMAND_BUFFER;
//	if (!s_pCommandBuffer[(int)eTarget]->AddText(pText, nTickDelay, cmd_source_t::kCommandSrcInvalid))
// Error(eDLL_T::ENGINE, NO_ERROR, "%s: buffer overflow\n", __FUNCTION__);

//-----------------------------------------------------------------------------
// Purpose: Sends the entire command line over to the server
// Input: *args - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
#ifndef CLIENT_DLL

bool Cmd_ForwardToServer(const CCommand* args)
{
	Assert(0);
	return false; // Client only.
}

//-----------------------------------------------------------------------------
// Purpose: execute commands directly (ignores all protection flags)
// Input: *pCommandString - 
// *pValueString - 
// Output: true on success, false otherwise
// 
// NOTE: this function is dangerous, as it allows execution of any command
// without restrictions. Currently, this is only enabled on the
// dedicated server for the local console input and RCON, as they both
// are considered secure (local console needs physical access to the
// terminal application, RCON requires authentication and its protocol
// is secure. Do not use this anywhere else without a valid reason !!!
// 
// NOTE: if client support is ever considered (unlikely), then the convar
// flag 'FCVAR_MATERIAL_THREAD_MASK' probably needs to be taken into
// account as well, also, change the DLL context of the warning to
// ENGINE if the client ever utilizes this.
//-----------------------------------------------------------------------------
struct PendingUnrestrictedCmd_s
{
	std::string command;
	std::string value;
};

static std::mutex s_UnrestrictedQueueMutex;
static std::deque<PendingUnrestrictedCmd_s> s_UnrestrictedQueue;

// Console/RCON only; a backlog this deep means the frame thread is gone.
static constexpr size_t UNRESTRICTED_QUEUE_MAX = 64;

//-----------------------------------------------------------------------------
// Purpose: drain commands deferred by Cmd_ExecuteUnrestricted's thread guard
//-----------------------------------------------------------------------------
void Cmd_RunUnrestrictedQueue(void)
{
	for (;;)
	{
		PendingUnrestrictedCmd_s pending;

		{
			std::lock_guard<std::mutex> lock(s_UnrestrictedQueueMutex);

			if (s_UnrestrictedQueue.empty())
				return;

			pending = std::move(s_UnrestrictedQueue.front());
			s_UnrestrictedQueue.pop_front();
		}

		Cmd_ExecuteUnrestricted(pending.command.c_str(), pending.value.c_str());
	}
}

void Cmd_DropUnrestrictedQueue(void)
{
	size_t nDropped = 0;

	{
		std::lock_guard<std::mutex> lock(s_UnrestrictedQueueMutex);
		nDropped = s_UnrestrictedQueue.size();
		if (!nDropped)
			return;
		s_UnrestrictedQueue.clear();
	}

	Warning(eDLL_T::SERVER,
		"[HOST] dropped %zu unrestricted command(s); host is shutting down\n",
		nDropped);
}

bool Cmd_ExecuteUnrestricted(const char* const pCommandString, const char* const pValueString)
{
	ConCommandBase* const pCommandBase = g_pCVar->FindCommandBase(pCommandString);

	if (!pCommandBase)
	{
		// Found nothing.
		Warning(eDLL_T::SERVER, "Command '%s' doesn't exist; request '%s' ignored\n", pCommandString, pValueString);
		return false;
	}

	// Off-thread caller must queue; command callbacks are not reentrant with the server frame.
	if (g_ThreadMainThreadID && g_ThreadServerFrameThreadID && !ThreadInMainOrServerFrameThread())
	{
		std::lock_guard<std::mutex> lock(s_UnrestrictedQueueMutex);

		if (s_UnrestrictedQueue.size() >= UNRESTRICTED_QUEUE_MAX)
		{
			Warning(eDLL_T::SERVER, "Command '%s' dropped; frame-thread queue is full\n", pCommandString);
			return false;
		}

		s_UnrestrictedQueue.push_back({ pCommandString, pValueString ? pValueString : "" });
		return true;
	}

	if (pCommandBase->IsFlagSet(FCVAR_SERVER_FRAME_THREAD))
		ThreadJoinServerJob();

	if (!pCommandBase->IsCommand())
	{
		// Here we want to skip over the command string in the value buffer.
		// So if we got 'sv_cheats 1' in our value buffer, we want to skip
		// over 'sv_cheats ', so that we are pointing directly to the value.
		const char* pFound = V_strstr(pValueString, pCommandString);
		const char* pValue = nullptr;

		if (pFound)
		{
			pValue = pFound + V_strlen(pCommandString);

			// Skip any leading space characters.
			while (*pValue == ' ')
			{
				++pValue;
			}
		}

		ConVar* const pConVar = reinterpret_cast<ConVar*>(pCommandBase);
		pConVar->SetValue(pValue ? pValue : pValueString);
	}
	else // Invoke command callback directly.
	{
		CCommand cmd;

		// Only tokenize if we actually have strings in the value buffer, some
		// commands (like 'status') don't need any additional parameters.
		if (VALID_CHARSTAR(pValueString))
		{
			cmd.Tokenize(pValueString, cmd_source_t::kCommandSrcCode);
		}

		v_Cmd_Dispatch(ECommandTarget_t::CBUF_SERVER, pCommandBase, &cmd, false);
	}

	return true;
}

static bool s_bRedirectFallbackLogged = false;

//-----------------------------------------------------------------------------
// Purpose: redirect printf used by status/ping/RCON output. The redirect
// object is the invoking session's own context, alive for the call by
// construction; stdin console invocation has none (null), which the engine
// dereferences unconditionally (AV at +0x48748). Only the null case is
// guardable here -- a non-null session is engine-owned lifetime.
//-----------------------------------------------------------------------------
static void Hook_CmdRedirectPrintf(__int64 a1, int a2, const char* pFormat, ...)
{
	char szBuf[4096];

	if (pFormat)
	{
		va_list va;
		va_start(va, pFormat);
		vsnprintf(szBuf, sizeof(szBuf), pFormat, va);
		va_end(va);
	}
	else
		szBuf[0] = '\0';

	if (!a1)
	{
		if (!s_bRedirectFallbackLogged)
		{
			s_bRedirectFallbackLogged = true;
			Warning(eDLL_T::SERVER, "[CMD] command output has no redirect; printing to console\n");
		}
		Msg(eDLL_T::SERVER, "%s", szBuf);
		return;
	}

	v_CmdRedirectPrintf(a1, a2, "%s", szBuf);
}

///////////////////////////////////////////////////////////////////////////////
void VCmd::Detour(const bool bAttach) const
{
	DetourSetup(&v_Cmd_ForwardToServer, &Cmd_ForwardToServer, bAttach);

	if (v_CmdRedirectPrintf)
		DetourSetup(&v_CmdRedirectPrintf, &Hook_CmdRedirectPrintf, bAttach);
	else
		Warning(eDLL_T::SERVER, "[CMD] CmdRedirectPrintf unresolved; stdin status/ping guard off\n");
}
#endif // !CLIENT_DLL
