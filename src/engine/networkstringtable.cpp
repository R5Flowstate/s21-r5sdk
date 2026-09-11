#if defined(CLIENT_DLL)
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
// $NoKeywords: $
//===========================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "net.h"
#include "networkstringtable.h"

//-----------------------------------------------------------------------------
// Purpose
// Input: i - 
// Output: CNetworkStringTableItem
//-----------------------------------------------------------------------------
//CNetworkStringTableItem* CNetworkStringTable::GetItem(int i)
//	if (i >= 0)
// return &m_pItems->Element(i);
//
//	Assert(m_pItemsClientSide);
//	return &m_pItemsClientSide->Element(-i);

//-----------------------------------------------------------------------------
// Purpose: Returns the table identifier
// Output: TABLEID
//-----------------------------------------------------------------------------
TABLEID CNetworkStringTable::GetTableId(void) const
{
	return m_id;
}

//-----------------------------------------------------------------------------
// Purpose: Returns the max size of the table
// Output: int
//-----------------------------------------------------------------------------
int CNetworkStringTable::GetMaxStrings(void) const
{
	return m_nMaxEntries;
}

//-----------------------------------------------------------------------------
// Purpose: Returns a table, by name
// Output: const char
//-----------------------------------------------------------------------------
const char* CNetworkStringTable::GetTableName(void) const
{
	return m_pszTableName;
}

//-----------------------------------------------------------------------------
// Purpose: Returns the number of bits needed to encode an entry index
// Output: int
//-----------------------------------------------------------------------------
int CNetworkStringTable::GetEntryBits(void) const
{
	return m_nEntryBits;
}

//-----------------------------------------------------------------------------
// Purpose: Sets the tick count
//-----------------------------------------------------------------------------
void CNetworkStringTable::SetTick(int tick_count)
{
	Assert(tick_count >= m_nTickCount);
	m_nTickCount = tick_count;
}

//-----------------------------------------------------------------------------
// Purpose: Locks the string table
//-----------------------------------------------------------------------------
bool CNetworkStringTable::Lock(bool bLock)
{
	bool bState = m_bLocked;
	m_bLocked = bLock;
	return bState;
}

//-----------------------------------------------------------------------------
// Purpose: Writes network string table delta's to snapshot buffer
// Input: *pClient - 
// nTickAck - 
// *pMsg - 
//-----------------------------------------------------------------------------
void CNetworkStringTableContainer::WriteUpdateMessage(CNetworkStringTableContainer* thisp, CClient* pClient, unsigned int nTickAck, bf_write* pMsg)
{

	Assert(!pMsg->IsOverflowed(), "Snapshot buffer overflowed before string table update!");
	CNetworkStringTableContainer__WriteUpdateMessage(thisp, pClient, nTickAck, pMsg);
}

void VNetworkStringTableContainer::Detour(const bool bAttach) const
{
	DetourSetup(&CNetworkStringTableContainer__WriteUpdateMessage, &CNetworkStringTableContainer::WriteUpdateMessage, bAttach);
}
#else // !CLIENT_DLL
//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose
//
// $NoKeywords: $
//===========================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "server/server.h"
#include "host.h"
#include "net.h"
#include "networkstringtable.h"
#include "engine/client/client.h"
#include "server/snapshot_send.h"
#include <atomic>

//-----------------------------------------------------------------------------
// Purpose
// Input: i - 
// Output: CNetworkStringTableItem
//-----------------------------------------------------------------------------
//CNetworkStringTableItem* CNetworkStringTable::GetItem(int i)
//	if (i >= 0)
// return &m_pItems->Element(i);
//
//	Assert(m_pItemsClientSide);
//	return &m_pItemsClientSide->Element(-i);

//-----------------------------------------------------------------------------
// Purpose: Returns the table identifier
// Output: TABLEID
//-----------------------------------------------------------------------------
TABLEID CNetworkStringTable::GetTableId(void) const
{
	return m_id;
}

//-----------------------------------------------------------------------------
// Purpose: Returns the max size of the table
// Output: int
//-----------------------------------------------------------------------------
int CNetworkStringTable::GetMaxStrings(void) const
{
	return m_nMaxEntries;
}

//-----------------------------------------------------------------------------
// Purpose: Returns a table, by name
// Output: const char
//-----------------------------------------------------------------------------
const char* CNetworkStringTable::GetTableName(void) const
{
	return m_pszTableName;
}

//-----------------------------------------------------------------------------
// Purpose: Returns the number of bits needed to encode an entry index
// Output: int
//-----------------------------------------------------------------------------
int CNetworkStringTable::GetEntryBits(void) const
{
	return m_nEntryBits;
}

//-----------------------------------------------------------------------------
// Purpose: Sets the tick count
//-----------------------------------------------------------------------------
void CNetworkStringTable::SetTick(int tick_count)
{
	Assert(tick_count >= m_nTickCount);
	m_nTickCount = tick_count;
}

//-----------------------------------------------------------------------------
// Purpose: Locks the string table
//-----------------------------------------------------------------------------
bool CNetworkStringTable::Lock(bool bLock)
{
	bool bState = m_bLocked;
	m_bLocked = bLock;
	return bState;
}

static ConVar bridge_real_cmdtick("bridge_real_cmdtick", "1", FCVAR_RELEASE,
	"Send the real per-client command tick in svc_ServerTick (not the legacy -1) so the native "
	"client clock-drift can phase-lock -> fixes bridge prediction/noclip jitter. 0 = legacy -1.");

static ConVar bridge_cmdtick_executed("bridge_cmdtick_executed", "1", FCVAR_RELEASE,
	"Stamp svc_ServerTick's command tick with the last EXECUTED usercmd number instead of "
	"the last received one (fixes the prediction off-by-one rubber-band at the wire). "
	"0 = legacy last-received stamp.");

extern std::atomic<int> g_bridgeLastExecCmdNumber[MAX_PLAYERS];

static ConVar bridge_cmdtick_pack("bridge_cmdtick_pack", "1", FCVAR_RELEASE,
	"Stamp svc_ServerTick's command tick with the exec count this packet's payload "
	"carries. 0 = off.");
static ConVar bridge_cmdtick_log("bridge_cmdtick_log", "0", FCVAR_DEVELOPMENTONLY,
	"[CMDTICK] Log every 256th svc_ServerTick stamp (0 = off).");
static std::atomic<int> s_prevWriteExec[MAX_PLAYERS] = {};

//-----------------------------------------------------------------------------
// Purpose: Writes network string table delta's to snapshot buffer
// Input: *pClient -
// nTickAck -
// *pMsg -
//-----------------------------------------------------------------------------
void CNetworkStringTableContainer::WriteUpdateMessage(CNetworkStringTableContainer* thisp, CClient* pClient, unsigned int nTickAck, bf_write* pMsg)
{
	if (sv_stats->GetBool())
	{
		const uint8_t nCPUPercentage = static_cast<uint8_t>(g_pServer->GetCPUUsage() * 100.0f);
		SVC_ServerTick serverTick(g_pServer->GetTick(), *host_frametime_unbounded, *host_frametime_stddeviation, nCPUPercentage);

		serverTick.m_nGroup = 0;
		serverTick.m_bReliable = true;

		// Stamp last EXECUTED usercmd, not last received. Window (received-64, received].
		int cmdTick = -1;
		int nExecUsed = 0; // [CMDTICK-EXEC] 1 when the executed-cmd stamp was applied
		if (bridge_real_cmdtick.GetBool() && pClient)
		{
			const int real = pClient->GetCommandTick();
			if (real > 0)
				cmdTick = real;

			if (bridge_cmdtick_executed.GetBool() && real > 0)
			{
				const int nSlot = static_cast<int>(pClient->GetUserID());
				if (nSlot >= 0 && nSlot < MAX_PLAYERS)
				{
					const int exec = g_bridgeLastExecCmdNumber[nSlot].load(std::memory_order_relaxed);
					if (exec > 0 && exec <= real && real - exec < 64)
					{
						cmdTick = exec;
						nExecUsed = 1;
					}

					if (bridge_cmdtick_pack.GetBool() && nSlot < 128)
					{
						const int execNow = g_bridgeLastExecCmdNumber[nSlot].load(std::memory_order_relaxed);
						if (!Bridge_SnapSyncSendActive())
						{
							const int packExec = s_prevWriteExec[nSlot].load(std::memory_order_relaxed);
							if (packExec > 0 && packExec <= real && real - packExec < 128)
							{
								cmdTick = packExec;
								nExecUsed = 5;
							}
						}
						if (execNow > 0)
							s_prevWriteExec[nSlot].store(execNow, std::memory_order_relaxed);
					}
				}
			}
		}
		serverTick.m_NetTick.m_nCommandTick = cmdTick;

		if (bridge_cmdtick_log.GetBool())
		{
			static int s_cmdTickLog = 0;
			if ((s_cmdTickLog++ % 256) == 0)
				DevMsg(eDLL_T::SERVER,
					"[CMDTICK] svc_ServerTick sent cmdTick=%d execUsed=%d clientTick=%d (GetCommandTick=%d srvTick=%d)\n",
					cmdTick, nExecUsed, serverTick.m_NetTick.m_nClientTick,
					pClient ? pClient->GetCommandTick() : -2, g_pServer->GetTick());
		}

		pMsg->WriteUBitLong(serverTick.GetType(), NETMSG_TYPE_BITS);

		if (!pMsg->IsOverflowed())
		{
			serverTick.WriteToBuffer(pMsg);
		}
	}

	Assert(!pMsg->IsOverflowed(), "Snapshot buffer overflowed before string table update!");
	CNetworkStringTableContainer__WriteUpdateMessage(thisp, pClient, nTickAck, pMsg);
}

void VNetworkStringTableContainer::Detour(const bool bAttach) const
{
	DetourSetup(&CNetworkStringTableContainer__WriteUpdateMessage, &CNetworkStringTableContainer::WriteUpdateMessage, bAttach);
}
#endif // CLIENT_DLL
