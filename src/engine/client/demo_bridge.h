//=============================================================================//
//
// Purpose: the seams between the net bridge and the demo recorder / player.
//
//=============================================================================//
#ifndef ENGINE_CLIENT_DEMO_BRIDGE_H
#define ENGINE_CLIENT_DEMO_BRIDGE_H

#include <cstdint>

//-----------------------------------------------------------------------------
// Bridge -> demo
//-----------------------------------------------------------------------------
bool Demo_IsPlaying(void);

// Player source at the head of the PollReceive drain loop. True with a
// datagram (OOB or netchan) in pBuf that must be injected like a received one.
bool DemoPlay_NextDatagram(char* pBuf, const int nCap, int* pLen);
// Replaces the socket + C2S_CHALLENGE part of the handshake while playing.
bool DemoPlay_OnStartHandshake(void);
bool DemoPlay_TimescaleOverride(float* pOut);
void DemoPlay_OnHostFrame(void);
void DemoPlay_OnPacketProcessed(void);
void DemoPlay_OnSessionEnded(void);
// The DataBlock the player delivered last carried a full snapshot.
void DemoPlay_OnFullSnapshotBlock(void);
// True while a rewind replays from a full snapshot back up to its target.
bool DemoPlay_InSoftRewind(void);
// The engine failed to parse a snapshot; every later delta is lost. Any thread.
void DemoPlay_OnSnapshotDropped(void);

// Begin/End bracket the bridge's decode of a DataBlock, so the chunk can be
// flagged when it carried a full snapshot.
void DemoRecord_OnDataBlock(const uint8_t* pRaw, const int nSize);
void DemoRecord_OnDataBlockEnd(const bool bFull);
void DemoRecord_OnPacketBegin(const uint8_t* pData, const int nSize);
void DemoRecord_OnPacketEnd(const bool bFull, const uint32_t nTick, const int nReliableEndBit);
void DemoRecord_OnSignonState(const int nState);
void DemoRecord_OnHostFrame(void);
void DemoRecord_Shutdown(void);

// The engine's current view angles (pitch, yaw, roll); false if unresolved.
bool DemoPlay_GetEngineViewAngles(float* pAngles);
// The local player's observer mode; -1 when there is no local player.
int  DemoPlay_LocalObserverMode(void);
void DemoPlay_LockView(void);
void DemoPlay_OverlayStarted(void);
bool DemoRecord_IsRecording(void);

//-----------------------------------------------------------------------------
// Demo -> bridge
//-----------------------------------------------------------------------------
int      S21Bridge_GetClientSignonState(void);
bool     S21Bridge_HasNativeChan(void);
bool     S21Bridge_NativeConnectPending(void);
bool     S21Bridge_SignonHandoffBusy(void);
void     S21Bridge_DeliverDataBlock(const uint8_t* pData, const int nSize);
void     S21Bridge_ResetForDemoSeek(void);
void     S21Bridge_StampChanAlive(void);
// Held S2C script calls fill half the queue; feeding more would force early
// releases that run before the entities they name exist.
bool     S21Bridge_S2CHoldQueueBusy(void);
uint32_t S21Bridge_LastSnapshotTick(void);
long     S21Bridge_FullSnapshotCount(void);
float    S21Bridge_IntervalPerTick(void);
const char* S21Bridge_ConnMapName(void);
bool     Bridge_IsBareMapName(const char* psz);
uint16_t S21Bridge_GamePort(void);

// Every raw datagram the bridge sends to the peer goes through here; while a
// demo plays nothing leaves the process.
int S21Bridge_TxRaw(const void* pBuf, const int nLen);

// Starts the client-VM playback overlay once the demo is in game.
void DemoNatives_StartOverlay(void);

#endif // ENGINE_CLIENT_DEMO_BRIDGE_H
