//=============================================================================//
//
// Purpose: Observation hook on CClient::UpdateAcknowledgedFramecount.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "snapshot_diag.h"
#include "snapshot_ack_diag.h"

#include <cstdint>
#include <atomic>

static constexpr ptrdiff_t CLIENT_OFF_SLOT         = 0x10;
static constexpr ptrdiff_t CLIENT_OFF_DELTAACKTICK = 0x3C8;
static constexpr ptrdiff_t CLIENT_OFF_WAITTICK     = 0x59C;

//-----------------------------------------------------------------------------
// Only runtime writer of m_nDeltaAckTick. Dropped acks delta-encode from a frame the client may not hold.
//-----------------------------------------------------------------------------
static ConVar sdk_bridge_ack_diag("sdk_bridge_ack_diag", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"[ACK-DIAG] Log the bridge slot's incoming clc_ClientTick acks: the first N, then "
	"every ack that changes or fails to change m_nDeltaAckTick. 0 = off.");

static ConVar sdk_bridge_ack_diag_n("sdk_bridge_ack_diag_n", "600",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"[ACK-DIAG] How many acks to log unconditionally after connect before falling back "
	"to change/drop-only logging.");

static char Hook_CClient_UpdateAckFramecount(int64_t pClient, int nTick, int nRawTick)
{
	if (pClient && nRawTick >= 0)
	{
		int ringOldest = -1, ringNewest = -1;
		SnapshotRing_HasTick(nRawTick, &ringOldest, &ringNewest);
		if (ringOldest >= 0 && nRawTick < ringOldest)
			nRawTick = ringOldest;
		if (ringNewest >= 0 && nRawTick > ringNewest)
		{
			static std::atomic<uint32_t> s_highClampN{ 0 };
			const uint32_t n = s_highClampN.fetch_add(1, std::memory_order_relaxed);
			if (n == 0 || (n % 256) == 0)
			{
				Warning(eDLL_T::SERVER,
					"[ACK-CLAMP] slot=%d nRawTick=%d > ringNewest=%d -- clamped (hit %u)\n",
					*reinterpret_cast<int*>(pClient + CLIENT_OFF_SLOT),
					nRawTick, ringNewest, n + 1);
			}
			nRawTick = ringNewest;
		}
		const int prevAck = *reinterpret_cast<int*>(pClient + CLIENT_OFF_DELTAACKTICK);
		if (prevAck >= 0 && nRawTick < prevAck)
			nRawTick = prevAck;
	}

	if (!pClient || !sdk_bridge_ack_diag.GetBool()
		|| *reinterpret_cast<int*>(pClient + CLIENT_OFF_SLOT) != 0)
		return v_CClient_UpdateAckFramecount(pClient, nTick, nRawTick);

	const int prevAck  = *reinterpret_cast<int*>(pClient + CLIENT_OFF_DELTAACKTICK);
	const int waitTick = *reinterpret_cast<int*>(pClient + CLIENT_OFF_WAITTICK);

	const char result = v_CClient_UpdateAckFramecount(pClient, nTick, nRawTick);

	const int newAck = *reinterpret_cast<int*>(pClient + CLIENT_OFF_DELTAACKTICK);

	static std::atomic<uint32_t> s_ackN{ 0 };
	const uint32_t n = s_ackN.fetch_add(1, std::memory_order_relaxed) + 1;

	const bool bRegressed = (newAck >= 0 && prevAck >= 0 && newAck < prevAck);
	const bool bDropped   = (newAck == prevAck && nRawTick != prevAck);
	const bool bChanged   = (newAck != prevAck);
	const bool bFullReq   = (nTick == -1);

	// Hot path inside ProcessMessages -- sample, do not log every FULLREQ.
	static std::atomic<uint32_t> s_fullReqN{ 0 };
	bool bLogFullReq = false;
	if (bFullReq)
	{
		const uint32_t f = s_fullReqN.fetch_add(1, std::memory_order_relaxed) + 1;
		bLogFullReq = (f == 1 || (f % 100) == 0);
	}

	// Steady-state acks fire every tick; sample those. Anomalies still log every time.
	static std::atomic<uint32_t> s_changedN{ 0 };
	bool bLogChanged = false;
	if (bChanged)
	{
		const uint32_t c = s_changedN.fetch_add(1, std::memory_order_relaxed) + 1;
		bLogChanged = (c % 100) == 0;
	}

	static std::atomic<uint32_t> s_droppedN{ 0 };
	bool bLogDropped = false;
	if (bDropped)
	{
		const uint32_t d = s_droppedN.fetch_add(1, std::memory_order_relaxed) + 1;
		bLogDropped = (d == 1 || (d % 100) == 0);
	}

	const char* pszMark = bFullReq
		? "  FULLREQ (client acked tick=-1; engine forces a full update)"
		: (bDropped ? "  DROPPED (engine gate rejected this ack)" : "");

	if (bRegressed)
	{
		Warning(eDLL_T::SERVER,
			"[ACK-REGRESS] slot=0 m_nDeltaAckTick %d -> %d (msg tick=%d raw=%d waitTick=%d) "
			"-- the dedi's delta base just moved BACKWARD\n",
			prevAck, newAck, nTick, nRawTick, waitTick);
	}
	else if (bLogFullReq || bLogDropped || bLogChanged || n <= static_cast<uint32_t>(sdk_bridge_ack_diag_n.GetInt()))
	{
		Warning(eDLL_T::SERVER,
			"[ACK-DIAG] #%u slot=0 msg{tick=%d raw=%d} ack %d -> %d waitTick=%d%s\n",
			n, nTick, nRawTick, prevAck, newAck, waitTick, pszMark);
	}
	return result;
}


void VAckDiag::GetFun(void) const
{
	// UpdateAcknowledgedFramecount -- opens on the m_bFakePlayer (+0x5A0) test
	// followed by the replay-convar read. Verified 1 hit.
	Module_FindPattern(g_GameDll,
		"80 B9 ?? ?? ?? ?? ?? 74 ?? 48 8B 05 ?? ?? ?? ?? 83 78")
		.GetPtr(v_CClient_UpdateAckFramecount);
}

void VAckDiag::Detour(const bool bAttach) const
{
	if (!v_CClient_UpdateAckFramecount)
	{
		Warning(eDLL_T::SERVER,
			"[ACK-DIAG] CClient::UpdateAcknowledgedFramecount pattern unresolved -- "
			"incoming delta-ack observation NOT active.\n");
		return;
	}
	const LONG result = bAttach
		? DetourAttach(reinterpret_cast<void**>(&v_CClient_UpdateAckFramecount),
			reinterpret_cast<void*>(&Hook_CClient_UpdateAckFramecount))
		: DetourDetach(reinterpret_cast<void**>(&v_CClient_UpdateAckFramecount),
			reinterpret_cast<void*>(&Hook_CClient_UpdateAckFramecount));
	if (bAttach)
	{
		Msg(eDLL_T::SERVER,
			"[ACK-DIAG] DetourAttach CClient::UpdateAcknowledgedFramecount result=0x%lX "
			"(target=0x%p)\n", result, (void*)v_CClient_UpdateAckFramecount);
	}
}
