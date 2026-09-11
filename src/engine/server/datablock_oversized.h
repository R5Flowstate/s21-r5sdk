//=============================================================================//
//
// Purpose: SDK-owned delivery for data block transfers larger than the engine
// sender's scratch. The engine sender tracks 768 fragments; a transfer past
// that is paced, acknowledged and resent here instead.
//
//=============================================================================//
#ifndef ENGINE_SERVER_DATABLOCK_OVERSIZED_H
#define ENGINE_SERVER_DATABLOCK_OVERSIZED_H

#include "thirdparty/detours/include/idetour.h"

class CClient;
class CServer;
class ServerDataBlockSender;

#define DATABLOCK_OVERSIZED_MAX_FRAGMENTS 4096

// Takes ownership of pData (malloc'd, transferSize bytes including the block
// header). Returns false and leaves pData to the caller when it cannot start.
bool DataBlockOversized_Begin(ServerDataBlockSender* pSender, CClient* pClient,
	uint8_t* pData, int transferSize, short transferId, short transferNr);
void DataBlockOversized_Cancel(const CClient* pClient);
void DataBlockOversized_Pump(CServer* pServer);
void DataBlockOversized_LevelShutdown(void);

class VDataBlockOversized : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // ENGINE_SERVER_DATABLOCK_OVERSIZED_H
