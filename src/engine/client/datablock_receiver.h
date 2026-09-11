//===========================================================================//
// 
// Purpose: client side data block receiver
// 
//===========================================================================//
#ifndef DATABLOCK_RECEIVER_H
#define DATABLOCK_RECEIVER_H
#include "engine/shared/datablock.h"

class CClientState;

class ClientDataBlockReceiver : public NetDataBlockReceiver
{
public:
	virtual void AcknowledgeTransmission() override;

	bool ProcessDataBlock(const double startTime, const short transferId, const int transferSize,
		const short counter, const short currentBlockId, const void* const blockBuffer, const int blockBufferBytes);
};

struct ClientDataBlockHeader_s
{
	char reserved[3]; // unused padding
	bool isCompressed;
};

// virtual methods
inline void*(*ClientDataBlockReceiver__AcknowledgeTransmission)(ClientDataBlockReceiver* thisptr);

// non-virtual methods
inline bool (*ClientDataBlockReceiver__ProcessDataBlock)(ClientDataBlockReceiver* thisptr, const double time,
	const short transferId, const int transferSize, const short counter, const short currentBlockId,
	const void* const blockBuffer, const int blockBufferBytes);


#endif // DATABLOCK_RECEIVER_H
