//=============================================================================//
//
// Purpose: Live CClient::SendSnapshot / CServer::SendClientMessages path.
//
//=============================================================================//
#ifndef ENGINE_SERVER_SNAPSHOT_SEND_H
#define ENGINE_SERVER_SNAPSHOT_SEND_H

#include "thirdparty/detours/include/idetour.h"

inline int64_t (*v_CClient_SendSnapshot)(int64_t a1, int64_t a2, int a3, int a4) = nullptr;
inline int64_t (*v_CServer_SendClientMessages)(int64_t, char) = nullptr;

void SnapshotSend_LevelShutdown(void);
void SnapshotSend_OnPackFreezeReleased(void);
bool Bridge_SnapSyncSendActive(void);

// Decide whether the engine fans per-client CClient::SendSnapshot out to job
// workers, and log which path the run will take. Call once the engine ConVars
// exist (SendTable_Init time).
void Bridge_ApplyParallelSendPolicy(void);

///////////////////////////////////////////////////////////////////////////////
class VCClientSendSnapshotDiag : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CClient::SendSnapshot", v_CClient_SendSnapshot);
		LogFunAdr("CServer::SendClientMessages", v_CServer_SendClientMessages);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ENGINE_SERVER_SNAPSHOT_SEND_H
