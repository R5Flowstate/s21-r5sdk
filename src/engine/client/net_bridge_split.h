#ifndef ENGINE_NET_BRIDGE_SPLIT_H
#define ENGINE_NET_BRIDGE_SPLIT_H

//=============================================================================//
//
// Purpose: Cross-TU symbols for the net_bridge process / c2s / install split.
// Include after engine/client/net_bridge_internal.h and engine net headers.
//
//=============================================================================//

#include <atomic>
#include <cstdint>


// S2C split reassembly. Header 0xFFFFFFFE; fragment:
// [4B flag][4B requestID][1B pktNum][1B pktCount][2B splitSize][payload]
struct SplitPacket {
	int32_t  requestID     = 0;
	int      packetCount   = 0;
	int      splitSize     = 0;
	uint64_t receivedMask[4] = {}; // one bit per fragment; the wire count is a byte
	int      receivedCount = 0;
	int      totalSize     = 0;    // total reassembled bytes
	unsigned long long lastMs = 0; // last fragment arrival, for slot recycling
	uint8_t  data[262144]  = {};   // reassembly buffer, the engine packet scratch size
};


// FIFO of synthetic net_SignonState messages to send to S3 server.
// Written by the SetSignonState hooks, drained by BuildS3Packet.
struct PendingSignon_s { int state; int spawn; };

// Wire prefix: "brq <seq> <original command>"
struct StringCmdResend_s
{
	uint32_t seq;
	char     cmd[1024];
	int      sendsLeft;
	double   nextDueMs;
};

// Blind one-shot resend of runtime NET_SetConVar (S3 t=4) bits; ring under s_c2sTxLock.
struct SetConVarResend_s
{
	uint8_t data[1024];
	int     nBits;
	double  nextDueMs;
	bool    active;
};

// S3 subchannel reliable-fragment reassembly. Reset() keeps the buffer;
// Free() releases it. Shared by ProcessMessages and ResetReliableRecv.
struct BridgeReliable_s
{
	uint8_t* buffer;
	int      capacity;
	int      totalSize;
	int      receivedSize;
	bool     isCompressed;
	int      uncompressedSize;
	bool     active;
	uint32_t entrySeq;

	void Free();
	void Reset();
};

struct LerpDepthSlot_s
{
	std::atomic<uint32_t> tick{ 0 };
	std::atomic<uint64_t> qpc{ 0 };
	std::atomic<uint8_t>  ready{ 0 };
};


// S21-only CUserCmd bits never land in S3 memory. Carry them in impulse (+0x34)
// high bits: 0x80 startEnergize, 0x40 toggleAkimbo. Low 7 bits stay real impulse.
enum ES21ExtraCmdFlags : uint8_t
{
	kS21ExtraFlag_StartEnergize     = 0x80,
	// S21 weaponToggleAkimbo, smuggled in impulse so the dedi toggle author can see it.
	kS21ExtraFlag_ToggleAkimbo      = 0x40,
};




// [ACK-XLATE] bridgeSeq -> engine outSeq ring, written at flush, read at ack.
struct AckXlate_s { uint32_t bridgeSeq; int engineSeq; };


// Function pointer: S21 bf_read initializer.
typedef void* (__fastcall *S21BfReadInit_fn)(void* s21buf, const uint8_t* data, uint64_t size);


//-----------------------------------------------------------------------------
// typeTag is the S21 ScriptRemote schema code, not ScriptVariant_t::m_type.
//-----------------------------------------------------------------------------
struct BridgeS2CScriptRemoteArg
{
	uint8_t  typeTag;
	uint8_t  strLen;
	char     strBuf[256];  // valid only when typeTag == 0x22 (string), NOT null-terminated on wire; we null-terminate on decode
	float    vec[3];
	int32_t  i;           // valid for int(5)/bool(6, 0/1)
	float    f;
	uint32_t eh;           // valid only when typeTag == 0x28/0x29 (entity/typed_entity), raw EHandle
};


struct BridgeS2CDeferred_t
{
	char     name[65];
	uint32_t nameLen;
	uint8_t  isUI;
	uint32_t argCount;
	uint32_t snapshotTick;   // wire nRawTick this call's state lands on
	uint32_t holdPasses;     // engine drain passes this entry has waited
	HSQUIRRELVM hVM;         // target m_hVM at enqueue (identity stamp)
	BridgeS2CScriptRemoteArg args[16];
};


// Split-packet injection queue: Hook_sendto pushes, PollReceive pops.
#define SPLIT_QUEUE_SIZE 512
// Phase 4: DataBlock signon receiver (SDK-verified format)
// See engine/shared/datablock.h, engine/server/datablock_sender.cpp,
// engine/client/datablock_receiver.cpp
#define BRIDGE_DB_MAX_FRAG_SIZE   1024    // MAX_DATABLOCK_FRAGMENT_SIZE
#define BRIDGE_DB_MAX_FRAGMENTS   4096    // MAX_DATABLOCK_FRAGMENTS
// 4 MB scratch; S21 signon DataBlocks decompress past the stock 768 KB.
#define BRIDGE_DB_SCRATCH_SIZE    4194304 // 4 MB to match dedi signon buffer

//-----------------------------------------------------------------------------
// Playlist-var overrides: S21 has no client override subsystem. Detour the two
// getter targets (vtable 90/92 and 75/76). Lookup is name-keyed.
//-----------------------------------------------------------------------------
#define S21BR_PLO_MAX_ENTRIES 64    // engine cap; the wire reader rejects more
#define S21BR_PLO_NAME_SIZE 128     // wire nameLen must be < this
#define S21BR_PLO_VALUE_SIZE 64     // wire valueLen must be < this

//=============================================================================
// S3 vs S21 netchan: same [32 seq][32 ack][8 flags]; S3 choked=bit4, S21=bit1.
// +4 outSeq +8 inSeq +12 outSeqAck +8584 handler +8592 msgs +8616 count
// +8496 subData +8504 subLen +8512 compressed +8520 expected +8536 total +8540 recv
//=============================================================================

#define S21_NC_OutSeqNr(p)          (*(int*)((char*)(p) + 4))
#define S21_NC_InSeqNr(p)           (*(int*)((char*)(p) + 8))
#define S21_NC_OutSeqNrAck(p)       (*(int*)((char*)(p) + 12))
#define S21_NC_ChokedPackets(p)     (*(int*)((char*)(p) + 0x2028))
#define S21_NC_StreamUnreliable(p)  ((bf_write*)((char*)(p) + 0x2078))
#define S21_NC_MessageHandler(p)    (*(INetChannelHandler**)((char*)(p) + 8584))
#define S21_NC_NetMessages(p)       (*(void***)((char*)(p) + 8592))
#define S21_NC_NetMessageCount(p)   (*(int*)((char*)(p) + 8616))

// S21 netpacket_t layout (0x90 bytes, differs from S3's 0x88)
#define S21_PKT_Message(p)          ((bf_read*)((char*)(p) + 0x0038))
#define S21_PKT_Size(p)             (*(int*)((char*)(p) + 0x0078))

// S3 subchannel constants
#define S3_MAX_FRAGMENT_PER_PACKET  560
// S3 writes 0xFDBAC34D, S21 expects 0xFDB97A8D. Accept both.
#define S3_SUBCHAN_MAGIC_S3         0xFDBAC34Du
#define S3_SUBCHAN_MAGIC_S21        0xFDB97A8Du

// ScriptRemote orchestrator: no __try here (ScriptVariant_t[16] has a dtor).
// Defer queue is file-scope and fixed-size; packet path allocates nothing.
#define BRIDGE_S2C_DEFER_MAX 128
#define SPLIT_QUEUE_PKT_MAX 2048
#define S21_NC_StreamReliable(p)    ((bf_write*)((char*)(p) + 0x2040))
#define S21_NC_Socket(p)            (*(int*)((char*)(p) + 0x20E8))
#define S21_PKT_Wiresize(p)         (*(int*)((char*)(p) + 0x007C))

//=============================================================================
// S21 bf_read is 64 bytes (S3 is 32). ProcessMessages expects this layout.
// +0x00 m_pDebugName +0x08 m_bOverflow +0x10 m_nDataBits +0x18 m_nDataBytes
// +0x20 m_nInBufWord +0x24 m_nBitsAvail +0x28 m_pDataIn +0x30 m_pBufferEnd +0x38 m_pData
//=============================================================================
#define S21BR_DEBUGNAME  0x00
#define S21BR_OVERFLOW   0x08
#define S21BR_DATABITS   0x10
#define S21BR_DATABYTES  0x18
#define S21BR_INBUFWORD  0x20
#define S21BR_BITSAVAIL  0x24
#define S21BR_PDATAIN    0x28
#define S21BR_PBUFEND    0x30
#define S21BR_PDATA      0x38
#define S21BR_SIZE       0x40

enum class BridgeHsStage : int {
	Idle = 0,
	ChallengeSent,
	ConnectSent,
};

struct SplitQueueEntry {
	int len;
	char data[SPLIT_QUEUE_PKT_MAX];
};

struct BridgeFlowStat { float avgloss; float avgchoke; float avgpackets; float avgbytes; float avglatency; };

struct S21PlaylistOverride_t
{
	char m_szName[S21BR_PLO_NAME_SIZE];
	char m_szValue[S21BR_PLO_VALUE_SIZE];
};

constexpr int kLerpDepthRingSize = 32;

inline double Bridge_NetTime()
{
	const uintptr_t a = NetObs_NetTimeAddr();
	return a ? *reinterpret_cast<double*>(a) : (double)GetTickCount64() / 1000.0;
}

typedef int (WSAAPI *PFN_sendto)(SOCKET s, const char* buf, int len, int flags,
	const sockaddr* to, int tolen);
typedef int (WSAAPI *PFN_recvfrom)(SOCKET s, char* buf, int len, int flags,
	sockaddr* from, int* fromlen);
typedef __int64 (__fastcall *PFN_PlayerDidDamageParse)(__int64 a1);
typedef __int64 (__fastcall *PFN_RemoteBulletFired)(__int64 a1);
typedef __int64 (__fastcall *PFN_RemoteWeaponReload)(__int64 a1);
typedef __int64 (__fastcall *PFN_WeapProjFireCB)(__int64 a1);
typedef BOOL (WINAPI *PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, DWORD, void*, void*, void*);

namespace S21BridgeCmd {
struct State {
	uint32_t commandNumber;
	float    snapshotInterpAcc;
	float    commandTime;
	float    commandViewAngles[3];
	float    viewSpringCorr[2];
	float    viewSpringCorrRoll;
	float    forwardmove;
	float    sidemove;
	float    upmove;
	float    leftTrigger;
	float    rightTrigger;
	uint32_t buttons;
	uint8_t  impulse;
	int32_t  weaponSelect;
	int32_t  weaponSelectType;
	uint32_t realtimeWeaponMod;
	uint8_t  weaponToggleAkimbo;
	int32_t  weaponCustomActivity;
	uint32_t meleetarget;
	uint8_t  controllerMode;
	uint8_t  vehicleCameraControls;
	uint8_t  updateCycleWeapon;
	uint8_t  hasWrittenAngle[3];
	uint8_t  startEnergize;
	bool     ziplinePresent;
	uint32_t ziplineHandle;
	float    ziplineFloats[13];
	struct Ping {
		bool present;
		uint8_t typeBits;
		uint32_t longs[4];
		float pos[3];
	} pings[4];
	uint8_t  respawnInputDebounce;
	uint8_t  queuePrimaryAttack;
	bool     useCameraOverride;
	float    cameraFloats[6];
	bool     hasThirdPersonAttackFocus;
	float    thirdPersonFocus[3];
	bool     hasKnockBack;
	float    knockBackPos[3];
	uint8_t  skydiveUnfollow;
	uint32_t baseSnapshotTickCount;
	uint32_t predictedServerEventAck;
	bool     bulletTracePresent;
	float    frametime;
};

// Both engines delta-encode the first cmd of a clc_Move batch against a
// CUserCmd::Reset() null cmd, not a zeroed one. A zero baseline decodes an
// inherited weaponSelect as slot 0, so a later explicit slot-0 select reads
// as no-change and is never emitted.
inline void ResetToNullCmd(State& s)
{
	memset(&s, 0, sizeof(s));
	s.commandNumber        = 0xFFFFFFFFu;
	s.weaponSelect         = -1;
	s.weaponSelectType     = -1;
	s.realtimeWeaponMod    = 0xFF00u;
	s.weaponCustomActivity = -1;
	s.meleetarget          = 0xFFFFFFFFu;
}
}


extern uint64_t S21Bridge_GetConnectNucleusID();
extern bool Bridge_DiagFirehoseEnabled(void);
extern void BridgeTrace_Log(const char* fmt, ...);
extern void BridgeTrace_Flush();
extern void BridgeStubDiag(const char* fmt, ...);
extern SOCKET s_bridgeSocket;
extern sockaddr_in6 s_bridgeDest;
extern bool S21Bridge_DestResolved(void);
extern bool S21Bridge_FromMatchesPeer(const sockaddr* pFrom, int nFromLen);
extern bool Bridge_IsBareMapName(const char* psz);
extern bool s_bridgeActive;
extern bool s_connAcceptDone;
extern BridgeHsStage s_hsStage;
extern ULONGLONG s_hsStageDeadline;
extern CNetChan* s_bridgeChan;
extern char g_bridgeConnMapName[64];
extern SplitQueueEntry s_splitQueue[SPLIT_QUEUE_SIZE];
extern volatile long s_splitQueueHead;
extern volatile long s_splitQueueTail;
extern volatile long s_splitSessionGen;
extern volatile long s_splitQueueGen[SPLIT_QUEUE_SIZE];
extern volatile long s_splitSlotGen[4];
void S21Bridge_ResetSplitReassembly(void);
extern std::unordered_set<std::string> s_s21RecvTableNames;
extern void S21Bridge_ExtractRecvTableNames();
extern bool S21Bridge_PreprocessSendTablesInBuffer(uint8_t* data, int dataSize, const char* tag);
extern bool S21Bridge_HasRecvTable(const char* name);
extern bool S21Bridge_CanRecordStub(const char* name);
extern void S21Bridge_RecordStub(const char* name, uintptr_t ptr);
extern uintptr_t S21Bridge_FindStub(const char* name);
extern uintptr_t S21Bridge_BuildStubRecvTable( const char* tblName, const uint8_t* signonData, int totalBits, int& bitPos);
extern void S21Bridge_RegisterStubsInList();
extern bool S21Bridge_CIDiag_On(void);
extern void S21Bridge_CIDiag_Gate(const char* where);
extern void S21Bridge_CIDiag_STSummary(const char* tag, int matched, int flipped, int noDecoder);
extern void S21Bridge_CIDiag_Flush(void);
extern bool s_dbSawSendTables;
extern uint32_t s_c2sSeqCounter;
extern BridgeFlowStat s_flowStat[2];
extern double s_relaySendTime[1024];
extern long long s_outBytesAcc;
extern uint32_t s_bridgeNonceHost;
extern uint32_t s_serverNonce;
extern bool s_needNonceAck;
extern bool s_serverAckedUs;
extern uint32_t s_bridgeSubSeq;
extern uint32_t s_bridgeInSeqNr;
extern uint32_t s_serverSubSeqRecv;
extern bool s_subSeqRebased;
extern bool s_serverNonceCaptured;
extern double s_lastBridgeC2STime;
extern bool s_dbBlockStatus[BRIDGE_DB_MAX_FRAGMENTS];
extern uint16_t s_dbBlockSizes[BRIDGE_DB_MAX_FRAGMENTS];
extern int s_dbTransferSize;
extern int s_dbTotalBlocks;
extern int s_dbBlocksReceived;
extern uint16_t s_dbTransferId;
extern uint16_t s_dbTransferNr;
extern bool s_dbComplete;
extern int s_dbTransferCount;
extern uint8_t* s_pendingSignonBuf;
extern int s_pendingSignonSize;
extern volatile bool s_pendingSignonReady;
extern bool s_deferredSignon3;
extern int s_deferredSignon3Bits;
extern int s_deferredSignon3Spawn;
extern int s_reliableSize;
extern int s_lastSentSignonState;
extern bool s_pendingUserInfo;
extern int s_userInfoSendsLeft;
extern ULONGLONG s_userInfoNextMs;
enum { kBridgeUserInfoCopies = 3 };
enum { kBridgeUserInfoIntervalMs = 250 };
void S21Bridge_ArmUserInfo(void);
void S21Bridge_ClearUserInfo(void);
extern int s_signonSeqKey;
extern int s_signonSeqSpawn;
extern int s_signonSeqMax;
extern bool s_signonSeqDidCL;
extern bool s_signonSawStateSinceCL;
extern bool s_signonRewalk;
extern int s_rewalkTraceLeft;
extern int s_rewalkTraceSignonLeft;
extern bool S21Bridge_RewalkTrace(void);
extern bool S21Bridge_RewalkTraceSignon(void);
extern int s_signonEchoState;
extern ULONGLONG s_signonEchoUntilMs;
extern void S21Bridge_QueueSignon(int state, int spawn);
extern void S21Bridge_ClearPendingSignon(void);
extern void S21Bridge_ResetReliableRecv(const char* reason);
extern BridgeReliable_s s_bridgeReliable;
extern bf_write s_c2sPend;
extern SRWLOCK s_c2sTxLock;
extern uint32_t s_stringCmdSeq;
extern StringCmdResend_s s_stringCmdRing[32];
extern int s_nStringCmdRingHead;
extern SetConVarResend_s s_setConVarRing[4];
extern int s_nSetConVarRingHead;
extern ConVar bridge_net_flow_reconcile;
extern S21BridgeCmd::State s_bridgeC2sPrevCmd;
extern uint8_t s_bridgeC2sPrevS3ImpulseWire;
extern uint32_t s_ftEstLastCmdNr;
extern double s_ftEstLastWall;
extern float s_ftEstPerCmd;
extern AckXlate_s s_ackXlate[1024];
extern bool S21Bridge_FlushC2SNow(const char* reason);
extern void S21Bridge_StartKeepaliveThread(void);
extern void S21Bridge_C2SUnrelSlices_Reset(void);
extern void S21Bridge_SendDisconnect(void);
extern void S21Bridge_C2SRel_PumpLocked(void);
extern void S21Bridge_C2SRel_Reset(void);
extern bool S21Bridge_C2SRel_RelayScriptRemote(const uint8_t* d, int sliceBytes, int b0, int b1);
extern bool S21Bridge_C2SRel_RelayChat(const uint8_t* d, int sliceBytes, int b0, int b1);
extern int S21Bridge_BuildS3Packet(uint8_t* outBuf, int outBufSize, uint32_t seq, uint32_t ack);
extern PFN_sendto s_origSendto;
extern PFN_recvfrom s_origRecvfrom;
extern int BridgeBudget_Level(void);
extern bool BridgeBudget_Armed(int level, long n);
extern double BridgeBudget_Ms(LONGLONG a, LONGLONG b, LONGLONG freq);
extern PFN_PlayerDidDamageParse s_origPlayerDidDamageParse;
extern PFN_RemoteBulletFired s_origRemoteBulletFired;
extern PFN_RemoteWeaponReload s_origRemoteWeaponReload;
extern PFN_WeapProjFireCB s_origWeapProjFireCB;
extern volatile LONG s_gdDummyPoolNext;
extern void GenerateDeltas_ClearFilled(void);
extern bool s_bridgePdefReady;
extern void S21Bridge_FlushS2CScriptRemote(const char* reason);
extern void S21Bridge_S2CScriptRemote_OnSnapshotApplied(uint32_t wireTick);
extern void S21Bridge_S2CScriptRemote_ResetAppliedTick(void);
extern bool S21Bridge_S2CScriptRemote_TickNearAccepted(uint32_t tick);
extern void S21Bridge_FlowStatsReset(void);
extern volatile long s_bridgeReportedRttMs;
extern void S21Bridge_OnConnAccept(const char* mapName, const char* gameMode);
extern void S21Bridge_OnConnReject(const char* reason);
extern const char* S21Bridge_PickChallengeMap(const unsigned char* pPkt, int nLen);
extern void S21Bridge_RememberChallengeMap(const char* pszMap);
extern int S21Bridge_WriteChallenge04(unsigned char* pBuf, int nBufLen, const char* pszMap);
extern bool S21Bridge_RewritePollToDestChallenge04(unsigned char* pBuf, int nBufLen, int* pOutLen);
extern int S21Bridge_DrainSocketToQueue(void);
extern bool S21Bridge_TryDequeueS2C(void* buf, int cap, int* pLen);
extern void S21Bridge_PumpWhileStalled(void);
extern volatile LONG s_splitAbandoned;
extern void S21Bridge_RequestValidatorDisconnect(const char* tag);
extern void S21Bridge_FlushValidatorDisconnect(void);
extern volatile LONG s_tickAckLog;
extern bool s_haveLastAck;
extern int s_lastRawAck;
extern volatile LONG s_ackFullReqLog;
extern volatile LONG s_ackInvariantLog;
extern S21PlaylistOverride_t s_playlistOverrides[S21BR_PLO_MAX_ENTRIES];
extern volatile long s_nPlaylistOverrides;
extern unsigned long long DeathObs_ImageVA(void* ret);
extern PFN_MiniDumpWriteDump s_origMiniDumpWriteDump;
extern void S21Bridge_OnDataBlockComplete(const uint8_t* rawBuf, int rawSize);
extern bool Bridge_CopyBoundedCString(const char* pSrc, size_t nMax, char* pOut, size_t nOut);
extern void S21Bridge_CIDiag_ReadClassMeta(int* outN, uintptr_t* outArr);
extern bool S21Bridge_ProcessMessages(CNetChan* pChan, bf_read* s3buf);
extern const int s_S3ToS21[69];
extern void S21Bridge_SendDataBlockAck();

#endif // ENGINE_NET_BRIDGE_SPLIT_H
