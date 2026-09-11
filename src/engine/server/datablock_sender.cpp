//===========================================================================//
// 
// Purpose: server side data block sender
// 
//===========================================================================//
#include "engine/client/client.h"
#include "common/proto_oob.h"
#include "datablock_sender.h"
#include <stdlib.h>
#include "engine/shared/s21_bridge_compat.h"
#include "engine/server/datablock_oversized.h"

static ConVar net_compressDataBlockLzAcceleration("net_compressDataBlockLzAcceleration", "1", FCVAR_DEVELOPMENTONLY, "The acceleration value for LZ4 data block compression");

//-----------------------------------------------------------------------------
// Purpose: sends the data block
//-----------------------------------------------------------------------------
void ServerDataBlockSender::SendDataBlock(const short transferId, const int transferSize,
	const short transferNr, const short blockNr, const uint8_t* const blockData, const int blockSize)
{
	const CClient* const cl = m_pClient;

	if (!cl)
	{
		Assert(0, "ServerDataBlockSender::SendDataBlock() called without a valid client handle!");
		return;
	}

	const CNetChan* const chan = cl->m_NetChannel;

	if (!chan)
	{
		Assert(0, "ServerDataBlockSender::SendDataBlock() called without a valid net channel!");
		return;
	}

	char dataBuf[DATABLOCK_FRAGMENT_PACKET_SIZE];
	bf_write buf(&dataBuf, sizeof(dataBuf));

	// msg data (gets processed on client's out of band packet handler)
	buf.WriteLong(CONNECTIONLESS_HEADER);
	buf.WriteByte(S2C_DATABLOCK_FRAGMENT);

	// transfer info -- S3 wire is shorts (client parser at 0x4F)
	buf.WriteShort(transferId);
	buf.WriteLong(transferSize);
	buf.WriteShort(transferNr);

	// block info
	buf.WriteShort(blockNr);
	buf.WriteLong(blockSize);

	// block data
	buf.WriteBytes(blockData, blockSize);

	// send the data block packet
	v_NET_SendPacket(NULL, 
		chan->GetSocket(), 
		chan->GetRemoteAddress(),
		buf.GetData(),
		buf.GetNumBytesWritten(), 
		NULL, false, NULL, true);
}

//-----------------------------------------------------------------------------
// Purpose: gets the resend rate
//-----------------------------------------------------------------------------
float ServerDataBlockSender::GetResendRate() const
{
    const CClient* const pClient = m_pClient;

    if (!pClient)
        return 0.0f;

    const CNetChan* const pChan = pClient->GetNetChan();

    if (!pChan)
        return 0.0f;

    if (m_bStartedTransfer)
        return 0.0f;

    const float netResendRate = pChan->GetResendRate();

    if (netResendRate < net_datablock_networkLossForSlowSpeed->GetFloat())
        return m_flResendRate;

    return netResendRate;
}

//-----------------------------------------------------------------------------
// Purpose: gets the receiver name (client name as registered on the server)
//-----------------------------------------------------------------------------
const char* ServerDataBlockSender::GetReceiverName() const
{
    return m_pClient->m_szServerName;
}

//-----------------------------------------------------------------------------
// Purpose: write the whole data in the data block scratch buffer
//-----------------------------------------------------------------------------
void ServerDataBlockSender::WriteDataBlock(const uint8_t* const sourceData, const int dataSize,
	const bool isMultiplayer, const char* const debugName)
{
	AcquireSRWLockExclusive(&m_Lock);

	const int headerSize = (int)sizeof(ServerDataBlockHeader_s);
	const int engineCap = SNAPSHOT_SCRATCH_BUFFER_SIZE - headerSize;
	const int sdkCap = DATABLOCK_SDK_SCRATCH_SIZE - headerSize;

	if (!sourceData || dataSize <= 0 || dataSize > sdkCap || !m_pScratchBuffer)
	{
		Warning(eDLL_T::SERVER, "[DATABLOCK] data block size %d rejected (sdk cap %d).\n",
			dataSize, sdkCap);
		ReleaseSRWLockExclusive(&m_Lock);
		return;
	}

	uint8_t* scratch = m_pScratchBuffer;
	int cap = engineCap;
	uint8_t* bigScratch = nullptr;
	if (dataSize > engineCap)
	{
		bigScratch = static_cast<uint8_t*>(malloc(DATABLOCK_SDK_SCRATCH_SIZE));
		if (!bigScratch)
		{
			Warning(eDLL_T::SERVER, "[DATABLOCK] oversized scratch alloc failed (%d bytes).\n", dataSize);
			ReleaseSRWLockExclusive(&m_Lock);
			return;
		}
		scratch = bigScratch;
		cap = sdkCap;
	}

	ServerDataBlockHeader_s* const pHeader = reinterpret_cast<ServerDataBlockHeader_s*>(scratch);
	bool copyRaw = true;
	int actualDataSize = dataSize;

	if (net_compressDataBlock->GetBool())
	{
		const int encodedSize = LZ4_compress_fast((const char*)sourceData,
			(char*)scratch + headerSize, dataSize, cap,
			net_compressDataBlockLzAcceleration.GetInt());

		if (encodedSize > 0 && encodedSize < dataSize)
		{
			actualDataSize = encodedSize;
			pHeader->isCompressed = true;
			copyRaw = false;
		}
		else if (!encodedSize)
		{
			Warning(eDLL_T::SERVER,
				"[DATABLOCK] LZ4 produced 0 for size %d -- sending raw.\n", dataSize);
		}
	}

	if (actualDataSize <= 0 || actualDataSize > cap)
	{
		Warning(eDLL_T::SERVER, "[DATABLOCK] data block size %d exceeds scratch cap %d.\n",
			actualDataSize, cap);
		free(bigScratch);
		ReleaseSRWLockExclusive(&m_Lock);
		return;
	}

	if (copyRaw)
	{
		pHeader->isCompressed = false;
		memcpy(scratch + headerSize, sourceData, actualDataSize);
	}

	const int transferSize = actualDataSize + headerSize;
	if (transferSize <= SNAPSHOT_SCRATCH_BUFFER_SIZE)
	{
		if (scratch != m_pScratchBuffer)
			memcpy(m_pScratchBuffer, scratch, transferSize);
		StartBlockSender(transferSize, isMultiplayer, debugName);
	}
	else
	{
		ResetBlockSender();
		++m_nTransferNr;
		if (DataBlockOversized_Begin(this, m_pClient, bigScratch, transferSize, m_nTransferId, m_nTransferNr))
			bigScratch = nullptr;
		else
			Warning(eDLL_T::SERVER, "[DATABLOCK] oversized transfer %d bytes not started.\n", transferSize);
	}

	free(bigScratch);
	ReleaseSRWLockExclusive(&m_Lock);
}
