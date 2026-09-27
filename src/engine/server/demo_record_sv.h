//=============================================================================//
//
// Purpose: server demo recorder. Each pov is one connected client; its stream
//          is exactly the bytes that client was sent.
//
//=============================================================================//
#ifndef ENGINE_SERVER_DEMO_RECORD_SV_H
#define ENGINE_SERVER_DEMO_RECORD_SV_H

#include <cstdint>

class CClient;
class CNetChan;
class CPlayer;
class CUserCmd;

// Capture seams.
void DemoSv_OnDataBlock(const CClient* pClient, const uint8_t* pData, const int nSize);
void DemoSv_OnDatagram(const CNetChan* pChan, const uint8_t* pData, const int nBytes);
void DemoSv_OnSnapshotBegin(const int nSlot, const bool bFull);
void DemoSv_OnSnapshotEnd(const int nSlot);
void DemoSv_OnUserCmd(CPlayer* pPlayer, const CUserCmd* pCmd);
void DemoSv_OnClientCleared(const CClient* pClient);
void DemoSv_LevelShutdown(void);

// True while any slot keeps history or is recorded; the hot taps check this first.
bool DemoSv_AnyActive(void);

// Script surface. A match id is [a-zA-Z0-9_-]{1,40}.
bool DemoSv_Start(const char* pszMatchId, CPlayer* const* ppPovs, const int nPovs);
bool DemoSv_Stop(const char* pszMatchId, CPlayer* pWinner, const char* pszReason);
bool DemoSv_Event(const char* pszMatchId, const char* pszType, CPlayer* pAttacker,
	CPlayer* pVictim, const char* pszWeapon, const float flDamage);
bool DemoSv_IsRecording(const char* pszMatchId);
bool DemoSv_Enabled(void);

#endif // ENGINE_SERVER_DEMO_RECORD_SV_H
