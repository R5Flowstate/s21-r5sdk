//=============================================================================//
//
// Purpose: SDK-owned delivery for data block transfers larger than the engine
// sender's scratch. The engine sender tracks 768 fragments; a transfer past
// that is paced, acknowledged and resent here instead.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/platform.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "tier1/bitbuf.h"
#include "tier1/NetAdr.h"
#include "common/proto_oob.h"
#include "public/inetchannel.h"
#include "engine/net_chan.h"
#include "engine/client/client.h"
#include "engine/server/server.h"
#include "engine/shared/datablock.h"
#include "engine/server/datablock_sender.h"
#include "datablock_oversized.h"

#include <stdlib.h>
#include <string.h>

static char (*v_CServer_ProcessConnectionlessPacket)(CServer* pServer, netpacket_t* pPacket) = nullptr;

static ConVar sdk_datablock_xl_resend("sdk_datablock_xl_resend", "0.25", FCVAR_RELEASE,
	"Seconds before an unacknowledged oversized data block fragment is resent.", true, 0.05f, true, 5.0f);
static ConVar sdk_datablock_xl_timeout("sdk_datablock_xl_timeout", "45", FCVAR_RELEASE,
	"Seconds before an unacknowledged oversized data block transfer is abandoned.", true, 5.0f, true, 600.0f);
static ConVar sdk_datablock_xl_burst("sdk_datablock_xl_burst", "64", FCVAR_RELEASE,
	"Fragments sent per client per server frame for an oversized data block transfer.", true, 1.0f, true, 1024.0f);

struct OversizedTransfer_t
{
	ServerDataBlockSender* pSender;
	CClient*  pClient;
	uint8_t*  pData;
	int       nSize;
	int       nBlocks;
	int       nAcked;
	short     nTransferId;
	short     nTransferNr;
	double    flStart;
	bool*     pAcked;
	double*   pSent;
};

static OversizedTransfer_t s_transfers[MAX_PLAYERS] = {};
static SRWLOCK s_lock = SRWLOCK_INIT;

static void Oversized_Free(OversizedTransfer_t& t)
{
	free(t.pData);
	free(t.pAcked);
	free(t.pSent);
	memset(&t, 0, sizeof(t));
}

static OversizedTransfer_t* Oversized_Find(const CClient* pClient)
{
	for (OversizedTransfer_t& t : s_transfers)
	{
		if (t.pData && t.pClient == pClient)
			return &t;
	}
	return nullptr;
}

bool DataBlockOversized_Begin(ServerDataBlockSender* pSender, CClient* pClient,
	uint8_t* pData, int transferSize, short transferId, short transferNr)
{
	if (!pSender || !pClient || !pData || transferSize <= 0)
		return false;

	const int nBlocks = (transferSize + MAX_DATABLOCK_FRAGMENT_SIZE - 1) / MAX_DATABLOCK_FRAGMENT_SIZE;
	if (nBlocks > DATABLOCK_OVERSIZED_MAX_FRAGMENTS)
	{
		Warning(eDLL_T::SERVER, "[DATABLOCK-XL] %d bytes is %d fragments, cap %d -- refused\n",
			transferSize, nBlocks, DATABLOCK_OVERSIZED_MAX_FRAGMENTS);
		return false;
	}

	bool* pAcked = static_cast<bool*>(calloc(static_cast<size_t>(nBlocks), sizeof(bool)));
	double* pSent = static_cast<double*>(calloc(static_cast<size_t>(nBlocks), sizeof(double)));
	if (!pAcked || !pSent)
	{
		free(pAcked);
		free(pSent);
		Warning(eDLL_T::SERVER, "[DATABLOCK-XL] state alloc failed for %d fragments\n", nBlocks);
		return false;
	}

	AcquireSRWLockExclusive(&s_lock);
	OversizedTransfer_t* t = Oversized_Find(pClient);
	if (t)
	{
		Warning(eDLL_T::SERVER, "[DATABLOCK-XL] client %d: replacing transfer nr=%d (%d/%d acked)\n",
			pClient->GetUserID(), t->nTransferNr, t->nAcked, t->nBlocks);
		Oversized_Free(*t);
	}
	else
	{
		for (OversizedTransfer_t& cand : s_transfers)
		{
			if (!cand.pData)
			{
				t = &cand;
				break;
			}
		}
	}
	if (!t)
	{
		ReleaseSRWLockExclusive(&s_lock);
		free(pAcked);
		free(pSent);
		Warning(eDLL_T::SERVER, "[DATABLOCK-XL] no free transfer slot\n");
		return false;
	}

	t->pSender     = pSender;
	t->pClient     = pClient;
	t->pData       = pData;
	t->nSize       = transferSize;
	t->nBlocks     = nBlocks;
	t->nAcked      = 0;
	t->nTransferId = transferId;
	t->nTransferNr = transferNr;
	t->flStart     = Plat_FloatTime();
	t->pAcked      = pAcked;
	t->pSent       = pSent;
	ReleaseSRWLockExclusive(&s_lock);

	Msg(eDLL_T::SERVER, "[DATABLOCK-XL] client %d: transfer id=%d nr=%d %d bytes in %d fragments\n",
		pClient->GetUserID(), transferId, transferNr, transferSize, nBlocks);
	return true;
}

void DataBlockOversized_Cancel(const CClient* pClient)
{
	AcquireSRWLockExclusive(&s_lock);
	OversizedTransfer_t* t = Oversized_Find(pClient);
	if (t)
		Oversized_Free(*t);
	ReleaseSRWLockExclusive(&s_lock);
}

void DataBlockOversized_LevelShutdown(void)
{
	AcquireSRWLockExclusive(&s_lock);
	for (OversizedTransfer_t& t : s_transfers)
	{
		if (t.pData)
			Oversized_Free(t);
	}
	ReleaseSRWLockExclusive(&s_lock);
}

void DataBlockOversized_Pump(CServer* pServer)
{
	const double now = Plat_FloatTime();
	const double resend = sdk_datablock_xl_resend.GetFloat();
	const double timeout = sdk_datablock_xl_timeout.GetFloat();
	const int burst = sdk_datablock_xl_burst.GetInt();

	AcquireSRWLockExclusive(&s_lock);
	for (OversizedTransfer_t& t : s_transfers)
	{
		if (!t.pData)
			continue;

		const CClient* pClient = t.pClient;
		if (!pClient || !pClient->IsConnected() || !pClient->GetNetChan())
		{
			Warning(eDLL_T::SERVER, "[DATABLOCK-XL] transfer nr=%d dropped: client gone\n", t.nTransferNr);
			Oversized_Free(t);
			continue;
		}
		if (now - t.flStart > timeout)
		{
			Warning(eDLL_T::SERVER, "[DATABLOCK-XL] client %d: transfer nr=%d timed out (%d/%d acked)\n",
				pClient->GetUserID(), t.nTransferNr, t.nAcked, t.nBlocks);
			Oversized_Free(t);
			continue;
		}

		int sent = 0;
		for (int i = 0; i < t.nBlocks && sent < burst; ++i)
		{
			if (t.pAcked[i] || (t.pSent[i] > 0.0 && now - t.pSent[i] < resend))
				continue;

			const int off = i * MAX_DATABLOCK_FRAGMENT_SIZE;
			int sz = t.nSize - off;
			if (sz > MAX_DATABLOCK_FRAGMENT_SIZE)
				sz = MAX_DATABLOCK_FRAGMENT_SIZE;

			t.pSender->SendDataBlock(t.nTransferId, t.nSize, t.nTransferNr,
				static_cast<short>(i), t.pData + off, sz);
			t.pSent[i] = now;
			++sent;
		}
	}
	ReleaseSRWLockExclusive(&s_lock);
	(void)pServer;
}

static void Oversized_OnAck(const netpacket_t* pPacket)
{
	const uint8_t* const p = pPacket->pData;
	const int size = pPacket->size;
	if (!p || size < 9)
		return;

	uint16_t transferId = 0, transferNr = 0;
	memcpy(&transferId, p + 5, 2);
	memcpy(&transferNr, p + 7, 2);

	AcquireSRWLockExclusive(&s_lock);
	for (OversizedTransfer_t& t : s_transfers)
	{
		if (!t.pData || !t.pClient)
			continue;
		const CNetChan* pChan = t.pClient->GetNetChan();
		if (!pChan || !pChan->GetRemoteAddress().CompareAdr(pPacket->from)
			|| !pChan->GetRemoteAddress().ComparePort(pPacket->from))
			continue;
		if (transferNr != static_cast<uint16_t>(t.nTransferNr))
			break;

		if (transferId == static_cast<uint16_t>(t.nTransferId + 1))
		{
			Msg(eDLL_T::SERVER, "[DATABLOCK-XL] client %d: transfer nr=%d complete in %.2fs\n",
				t.pClient->GetUserID(), t.nTransferNr, Plat_FloatTime() - t.flStart);
			Oversized_Free(t);
			break;
		}
		if (transferId != static_cast<uint16_t>(t.nTransferId))
			break;

		// Bit-packed from byte 9: bit 0 = bitmap present, bit 1+i = fragment i held.
		const uint8_t* const bits = p + 9;
		const int nBitsAvail = (size - 9) * 8;
		if (nBitsAvail < 1 || !(bits[0] & 1u))
			break;
		const int n = (t.nBlocks < nBitsAvail - 1) ? t.nBlocks : (nBitsAvail - 1);
		for (int i = 0; i < n; ++i)
		{
			const int bit = 1 + i;
			if (!t.pAcked[i] && (bits[bit >> 3] & (1u << (bit & 7))))
			{
				t.pAcked[i] = true;
				++t.nAcked;
			}
		}
		break;
	}
	ReleaseSRWLockExclusive(&s_lock);
}

static char Hook_CServer_ProcessConnectionlessPacket(CServer* pServer, netpacket_t* pPacket)
{
	if (pPacket && pPacket->pData && pPacket->size >= 9)
	{
		uint32_t hdr = 0;
		memcpy(&hdr, pPacket->pData, 4);
		if (hdr == 0xFFFFFFFFu && pPacket->pData[4] == C2S_DATABLOCK_ACK)
			Oversized_OnAck(pPacket);
	}
	// Native still runs: it stamps the channel's last-received time on every ack.
	return v_CServer_ProcessConnectionlessPacket(pServer, pPacket);
}

void VDataBlockOversized::GetAdr(void) const
{
	LogFunAdr("CServer::ProcessConnectionlessPacket", v_CServer_ProcessConnectionlessPacket);
}

void VDataBlockOversized::GetFun(void) const
{
	// CServer::ProcessConnectionlessPacket. The 'P' data block ack case is
	// inlined into this dispatcher; the prologue's 0x198 frame is unique.
	Module_FindPattern(g_GameDll,
		"48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 "
		"48 8D AC 24 68 FF FF FF 48 81 EC 98 01 00 00 0F 10 52 50")
		.GetPtr(v_CServer_ProcessConnectionlessPacket);

	if (!v_CServer_ProcessConnectionlessPacket)
		Warning(eDLL_T::SERVER, "[DATABLOCK-XL] ProcessConnectionlessPacket pattern unresolved -- oversized acks disabled\n");
}

void VDataBlockOversized::Detour(const bool bAttach) const
{
	if (v_CServer_ProcessConnectionlessPacket)
		DetourSetup(&v_CServer_ProcessConnectionlessPacket, &Hook_CServer_ProcessConnectionlessPacket, bAttach);
}
