//===========================================================================//
// 
// Purpose: data block sender & receiver
// 
//===========================================================================//
#ifndef IDATABLOCK_H
#define IDATABLOCK_H

// the maximum size of each data block fragment
#define MAX_DATABLOCK_FRAGMENT_SIZE 1024

// the maximum amount of fragments per data block transfer
#define MAX_DATABLOCK_FRAGMENTS 768

// ceil-aligned payload the 0xC0004 scratch can hold (768 * 1024).
// Wire transferSize is attacker-controlled; never use it as the write bound.
#define MAX_DATABLOCK_TRANSFER_SIZE (MAX_DATABLOCK_FRAGMENTS * MAX_DATABLOCK_FRAGMENT_SIZE)
#define DATABLOCK_SDK_SCRATCH_SIZE 4194304

#define DATABLOCK_DEBUG_NAME_LEN 64
#define DATABLOCK_INVALID_BLOCK_NR -1

// the maximum size of a data block fragment packet (encoded header + actual data)
#define DATABLOCK_FRAGMENT_PACKET_SIZE (MAX_DATABLOCK_FRAGMENT_SIZE + 176)

//-----------------------------------------------------------------------------
// Forward decelerations
//-----------------------------------------------------------------------------
class CClient;
class CClientState;

abstract_class NetDataBlockSender
{
public:
	virtual ~NetDataBlockSender();


	virtual void SendDataBlock(const short transferId, const int transferSize,
		const short transferNr, const short blockNr, const uint8_t* const blockData, const int blockSize) = 0;
	virtual float GetResendRate() const = 0;
	virtual const char* GetReceiverName() const = 0;

	void StartBlockSender(const int transferSize, const bool isMultiplayer, const char* const debugName);
	void ResetBlockSender();

protected:
	char pad_0008[56];
	RTL_SRWLOCK m_Lock;
	char pad_0048[56];

	// the server side client handle that is our 'receiving' end
	CClient* m_pClient;

	char m_bInitialized;
	char m_bStartedTransfer;

	char m_bMultiplayer;
	char field_8B;

	// the current transfer id, and the global transfer count for this
	// particular client. the transfer nr keeps getting incremented on
	// each new context setup
	short m_nTransferId;
	short m_nTransferNr;

	// the total transfer size for the data, and the number of blocks this data
	// has been carved up to
	int m_nTransferSize;
	int m_nTotalBlocks;

	// last block that has been ack'd, and the current block that is pending to
	// be sent to the receiver
	int m_nBlockAckTick;
	int m_nCurrentBlock;

	// the total number of bytes remaining to be sent, and the number of times
	// we attempted to send data blocks
	int m_nTotalSizeRemaining;
	int m_nBlockSendsAttempted;

	// the resend rate for this connection, which depends of the stability/loss
	// and other factors computed from the netchan
	float m_flResendRate;
	char pad_00AC[4]; // padding, in case we want to stuff our own vars in here

	// times used to determine when a data block has been sent, and how long it
	// took to get this out and acknowledged
	double m_TimeCurrentSend;
	double m_TimeFirstSend;
	double m_TimeLastSend;

	// the last time we attempted to send this block, this gets updated when
	// a data block hasn't been acknowledged in time and is being resent
	double m_flBlockSendTimes[MAX_DATABLOCK_FRAGMENTS];

	// the debug name used when details get dumped to the console
	char m_szDebugName[DATABLOCK_DEBUG_NAME_LEN];

	// if a data block has been acknowledged by the receiver, we mark that
	// particular block as acknowledged
	bool m_bBlockAckStatus[MAX_DATABLOCK_FRAGMENTS];
	uint8_t* m_pScratchBuffer;

	// this member is true when
	// ( m_TimeCurrentSend - m_TimeFirstSend ) > net_datablock_longSendTime.GetFloat, or
	// m_nTransferId < 4
	//
	// if the above condition is true, function ServerDataBlockSender::SendPendingDataBlocks
	// also returns true, else this function always returns false
	bool m_bAbnormalSend;
};

abstract_class NetDataBlockReceiver
{
public:
	virtual ~NetDataBlockReceiver() {};
	// Called when cvar 'net_debugDataBlockReceiver' is set;
	// currently a nullsub in the engine.
	virtual void DebugDataBlockReceiver() {};
	virtual void AcknowledgeTransmission() = 0;

	void StartBlockReceiver(const int transferSize, const double startTime);
	void ResetBlockReceiver(const short transferNr);

protected:
	// the client side 'client' handle
	CClientState* m_pClientState;

	// whether the transfer has been started and completed
	bool m_bStartedRecv;
	bool m_bCompletedRecv;

	bool byte12;

	// the current transfer id, and the global transfer count for this
	// particular client. the transfer nr keeps getting incremented on
	// each new context setup
	short m_TransferId;
	short m_nTransferNr;

	bool m_bInitialized;

	// the total transfer size, and the # amount of blocks the data has been
	// carved into
	int m_nTransferSize;
	int m_nTotalBlocks;

	int m_nBlockAckTick;

	// the time the data block receiver was started for this transfer
	double m_flStartTime;

	// if we successfully processed a data block, we mark it as processed
	bool m_BlockStatus[MAX_DATABLOCK_FRAGMENTS];
	char* m_pScratchBuffer;
};

#endif // IDATABLOCK_H
