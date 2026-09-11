#if defined(CLIENT_DLL)
//=============================================================================//
// 
// Purpose
// 
//=============================================================================//

#ifndef NET_CHAN_H
#define NET_CHAN_H

#include "tier1/bitbuf.h"
#include "tier1/NetAdr.h"
#include "tier1/NetKey.h"
#include "tier1/utlmemory.h"
#include "tier1/utlvector.h"
#include "common/netmessages.h"
#include "common/protocol.h"
#include "public/inetchannel.h"

#define NET_FRAMES_BACKUP 128
#define NET_UNRELIABLE_STREAM_MINSIZE 256
#define NET_CHANNELNAME_MAXLEN 32
#define NET_FRAMES_MASK   (NET_FRAMES_BACKUP-1)

//-----------------------------------------------------------------------------
// Purpose: forward declarations
//-----------------------------------------------------------------------------
class CClient;
class CNetChan;

//-----------------------------------------------------------------------------
typedef struct netframe_header_s
{
	float time;
	int size;
	short choked;
	bool valid;
	float latency;
} netframe_header_t;

typedef struct netframe_s
{
	int dropped;
	float avg_latency;
} netframe_t;

//-----------------------------------------------------------------------------
typedef struct netflow_s
{
	float nextcompute;
	float avgbytespersec;
	float avgpacketspersec;
	float avgloss;
	float avgchoke;
	float avglatency;
	float latency;
	float maxlatency;
	int64_t totalpackets;
	int64_t totalbytes;
	int64_t totalupdates;
	int currentindex;
	netframe_header_t frame_headers[NET_FRAMES_BACKUP];
	netframe_t frames[NET_FRAMES_BACKUP];
	netframe_t* currentframe;
} netflow_t;

//-----------------------------------------------------------------------------
struct dataFragments_t
{
	char* buffer;
	int64_t blockSize;
	bool isCompressed;
	uint8_t gap11[7];
	int64_t uncompressedSize;
	bool firstFragment;
	bool lastFragment;
	bool isOutbound;
	int transferID;
	int transferSize;
	int currentOffset;
};

//-----------------------------------------------------------------------------
enum EBufType
{
	BUF_RELIABLE = 0,
	BUF_UNRELIABLE,
	BUF_VOICE
};

inline void(*CNetChan__Clear)(CNetChan* pChan, bool bStopProcessing);
inline void(*CNetChan__Shutdown)(CNetChan* pChan, const char* szReason, uint8_t bBadRep, bool bRemoveNow);
inline bool(*CNetChan__CanPacket)(const CNetChan* pChan);
inline void(*CNetChan__FlowNewPacket)(CNetChan* pChan, int flow, int outSeqNr, int inSeqNr, int nChoked, int nDropped, int nSize);
inline __int64(*CNetChan__FlowUpdate)(CNetChan* pChan, int flow);
inline int(*CNetChan__SendDatagram)(CNetChan* pChan, bf_write* pMsg);
inline bool(*CNetChan__ProcessMessages)(CNetChan* pChan, bf_read* pMsg);

// S21 engine-native functions (different from SDK patterns -- S21 CNetChan layout differs)
struct netpacket_s;
inline void(*CNetChan__ProcessPacket)(CNetChan* pChan, netpacket_s* pPacket);
// Note: CNetChan__SendDatagram above is reused for S21 (same signature, different pattern)
// CNetChan::SendNetMsg (S21 engine ). Raw void* args: the SDK's
// CNetChan/INetMessage layouts don't match the S21 netchannel.
inline char(*CNetChan__SendNetMsg)(void* pChan, void* pMsg, char bForceReliable, char bVoice);

//-----------------------------------------------------------------------------
class CNetChan
{
public:
	~CNetChan()
	{
		Shutdown("NetChannel removed.", 1, false);
		FreeReceiveList();
	}

	inline const char* GetName(void)                     const { return m_Name; }
	inline const char* GetAddress(bool onlyBase = false) const { return remote_address.ToString(onlyBase); }
	inline int         GetPort(void)                     const { return int(ntohs(remote_address.GetPort())); }
	inline int         GetDataRate(void)                 const { return m_Rate; }
	inline int         GetBufferSize(void)               const { return NET_FRAMES_BACKUP; }

	float        GetResendRate() const;

	inline float GetLatency(int flow)        const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].latency; }
	inline float GetAvgChoke(int flow)       const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avgchoke; }
	inline float GetAvgLatency(int flow)     const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avglatency; }
	inline float GetAvgLoss(int flow)        const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avgloss; }
	inline float GetAvgPackets(int flow)     const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avgpacketspersec; }
	inline float GetAvgData(int flow)        const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avgbytespersec; }
	inline int64_t GetTotalData(int flow)    const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].totalbytes; }
	inline int64_t GetTotalPackets(int flow) const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].totalpackets; }

	int         GetSequenceNr(int flow) const;
	double      GetTimeConnected(void) const;

	inline float GetTimeoutSeconds(void)             const { return m_Timeout; }
	inline void  SetLastReceivedTime(double t)             { last_received = t; }
	inline int   GetSocket(void)                     const { return m_Socket; }
	inline const bf_write& GetStreamVoice(void)      const { return m_StreamVoice; }
	inline const bf_write& GetStreamReliable(void)   const { return m_StreamReliable; }
	inline const bf_write& GetStreamUnreliable(void) const { return m_StreamUnreliable; }
	inline const netadr_t& GetRemoteAddress(void)    const { return remote_address; }

	int         GetNumBitsWritten(const bool bReliable);
	int         GetNumBitsLeft(const bool bReliable);
	inline bool IsOverflowed(void)                const { return m_StreamReliable.IsOverflowed(); }

	bool HasPendingReliableData(void);

	inline bool CanPacket(void) const { return CNetChan__CanPacket(this); }
	inline int SendDatagram(bf_write* pDatagram) { return CNetChan__SendDatagram(this, pDatagram); }
	bool SendNetMsg(INetMessage& msg, const bool bForceReliable, const bool bVoice);
	bool SendData(bf_write& msg, const bool bReliable);

	INetMessage* FindMessage(const int type);
	bool RegisterMessage(INetMessage* msg);

	inline void Clear(bool bStopProcessing) { CNetChan__Clear(this, bStopProcessing); }
	inline void Shutdown(const char* szReason, uint8_t bBadRep, bool bRemoveNow) { CNetChan__Shutdown(this, szReason, bBadRep, bRemoveNow); }
	void FreeReceiveList();
	bool ProcessMessages(bf_read* pMsg);

	bool ReadSubChannelData(bf_read& buf);

	static void _Shutdown(CNetChan* pChan, const char* szReason, uint8_t bBadRep, bool bRemoveNow);
	static bool _ProcessMessages(CNetChan* pChan, bf_read* pMsg);

	static void _FlowNewPacket(CNetChan* const pChan, const int flow, const int outSeqNr, const int inSeqNr, const int nChoked, const int nDropped, const int nSize);

	void SetChoked();
	void SetRemoteFramerate(float flFrameTime, float flFrameTimeStdDeviation);
	inline void SetRemoteCPUStatistics(uint8_t nStats) { m_nServerCPU = nStats; }

	//-----------------------------------------------------------------------------
public:
	bool                m_bProcessingMessages;
	bool                m_bShouldDelete;
	bool                m_bStopProcessing;
	bool                m_bShuttingDown;
	int                 m_nOutSequenceNr;
	int                 m_nInSequenceNr;
	int                 m_nOutSequenceNrAck;
	int                 m_nChokedPackets;
	int                 m_nRealTimePackets; // Number of packets without pre-scaled frame times.

private:
	int                 m_nLastRecvFlags;
	RTL_SRWLOCK         m_Lock;
	bf_write            m_StreamReliable;
	CUtlMemory<byte>    m_ReliableDataBuffer;
	bf_write            m_StreamUnreliable;
	CUtlMemory<byte>    m_UnreliableDataBuffer;
	bf_write            m_StreamVoice;
	CUtlMemory<byte>    m_VoiceDataBuffer;
	int                 m_Socket;
	int                 m_MaxReliablePayloadSize;
	double              last_received;
	double              connect_time;
	uint32_t            m_Rate;
	int                 padding_maybe;
	double              m_fClearTime;
	CUtlVector<dataFragments_t*> m_WaitingList;
	dataFragments_t     m_ReceiveList;
	int                 m_nSubOutFragmentsAck;
	int                 m_nSubInFragments;
	int                 m_nNonceHost;
	uint32_t            m_nNonceRemote;
	bool                m_bReceivedRemoteNonce;
	bool                m_bInReliableState;
	bool                m_bPendingRemoteNonceAck;
	uint32_t            m_nSubOutSequenceNr;
	int                 m_nLastRecvNonce;
	bool                m_bUseCompression;
	uint32_t            m_ChallengeNr;
	float               m_Timeout;
	INetChannelHandler* m_MessageHandler;
	CUtlVector<INetMessage*> m_NetMessages;
	void*               m_UnusedInterfacePointer;
	int                 m_nQueuedPackets;
	float               m_flRemoteFrameTime;
	float               m_flRemoteFrameTimeStdDeviation;
	uint8_t             m_nServerCPU;
	int                 m_nMaxRoutablePayloadSize;
	int                 m_nSplitPacketSequence;
	int64_t             m_StreamSendBuffer;
	bf_write            m_StreamSend;
	bool                m_bConnecting; // true if SetSignonState is called with signon < SIGNONSTATE_FULL.
	netflow_t           m_DataFlow[MAX_FLOWS];
	int                 m_nLifetimePacketsDropped;
	int                 m_nSessionPacketsDropped;
	int                 m_nSequencesSkipped;
	int                 m_nSessionRecvs;
	uint32_t            m_nLiftimeRecvs;
	bool                m_bRetrySendLong;
	char                m_Name[NET_CHANNELNAME_MAXLEN];
	netadr_t            remote_address;
};
static_assert(sizeof(CNetChan) == 0x1AC8);

//-----------------------------------------------------------------------------
// Purpose: sets the remote frame times
// Input: flFrameTime - 
// flFrameTimeStdDeviation - 
//-----------------------------------------------------------------------------
inline void CNetChan::SetRemoteFramerate(float flFrameTime, float flFrameTimeStdDeviation)
{
	m_flRemoteFrameTime = flFrameTime;
	m_flRemoteFrameTimeStdDeviation = flFrameTimeStdDeviation;
}

//-----------------------------------------------------------------------------
// Purpose: increments choked packet count
//-----------------------------------------------------------------------------
inline void CNetChan::SetChoked(void)
{
	m_nOutSequenceNr++; // Sends to be done since move command use sequence number.
	m_nChokedPackets++;
}


///////////////////////////////////////////////////////////////////////////////
class VNetChan : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CNetChan::Clear", CNetChan__Clear);
		LogFunAdr("CNetChan::Shutdown", CNetChan__Shutdown);
		LogFunAdr("CNetChan::CanPacket", CNetChan__CanPacket);
		LogFunAdr("CNetChan::FlowNewPacket", CNetChan__FlowNewPacket);
		LogFunAdr("CNetChan::FlowUpdate", CNetChan__FlowUpdate);
		LogFunAdr("CNetChan::SendDatagram", CNetChan__SendDatagram);
		LogFunAdr("CNetChan::ProcessMessages", CNetChan__ProcessMessages);
		LogFunAdr("CNetChan::SendNetMsg(S21)", CNetChan__SendNetMsg);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "88 54 24 10 53 55 57").GetPtr(CNetChan__Clear);
		Module_FindPattern(g_GameDll, "48 89 6C 24 18 56 57 41 56 48 83 EC 30 83 B9").GetPtr(CNetChan__Shutdown);
		Module_FindPattern(g_GameDll, "40 53 48 83 EC 20 83 B9 ?? ?? ?? ?? ?? 48 8B D9 75 15 48 8B 05 ?? ?? ?? ??").GetPtr(CNetChan__CanPacket);
		// S21 FlowNewPacket: lea rsi,[rcx+0x21F8] is m_DataFlow.
		// flow=1 clears the -1.0 unacked-latency sentinel on the outgoing ring.
		Module_FindPattern(g_GameDll, "40 53 56 57 41 54 41 55 48 83 EC 30 48 63 84 24 90 00 00 00 48 8D B1 F8 21 00 00").GetPtr(CNetChan__FlowNewPacket);
		// S21-native FlowUpdate -- the sole writer of m_DataFlow avg
		// stats. prologue + `lea r8, [rcx+0x21F8]` (m_DataFlow base) is the unique sig.
		Module_FindPattern(g_GameDll, "89 54 24 10 4C 8B DC 57 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? 4C 8D 81 F8 21 00 00").GetPtr(CNetChan__FlowUpdate);
		// S21-specific patterns (S3 patterns match WRONG functions in S21!)
		Module_FindPattern(g_GameDll, "48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 EB FF FF").GetPtr(CNetChan__SendDatagram);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B FA").GetPtr(CNetChan__ProcessMessages);
		// CNetChan::SendNetMsg (S21 ): prologue + the `cmp [rcx+0x32D8],0`
		// netchannel-isReal check makes this signature unique.
		Module_FindPattern(g_GameDll, "48 89 54 24 10 55 53 56 57 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 83 B9 D8 32 00 00 00").GetPtr(CNetChan__SendNetMsg);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // NET_CHAN_H
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose
//
//=============================================================================//

#ifndef NET_CHAN_H
#define NET_CHAN_H

#include "tier1/bitbuf.h"
#include "tier1/NetAdr.h"
#include "tier1/NetKey.h"
#include "tier1/utlmemory.h"
#include "tier1/utlvector.h"
#include "common/netmessages.h"
#include "common/protocol.h"
#include "public/inetchannel.h"

#define NET_FRAMES_BACKUP 128
#define NET_UNRELIABLE_STREAM_MINSIZE 256
#define NET_CHANNELNAME_MAXLEN 32
#define NET_FRAMES_MASK   (NET_FRAMES_BACKUP-1)

// [SIGNON-PHASE] wall-time accumulators for the net_SignonState handler
// breakdown. Written by CClient::VActivatePlayer / CClient::WriteDataBlock on
// the frame thread, reset and reported by the ProcessMessages dispatch loop.
extern thread_local double g_flSignonPhaseActivateMs;
extern thread_local double g_flSignonPhaseDbBlockMs;
extern thread_local double g_flSignonPhasePostMs;

//-----------------------------------------------------------------------------
// Purpose: forward declarations
//-----------------------------------------------------------------------------
class CClient;
class CNetChan;

//-----------------------------------------------------------------------------
typedef struct netframe_header_s
{
	float time;
	int size;
	short choked;
	bool valid;
	float latency;
} netframe_header_t;

typedef struct netframe_s
{
	int dropped;
	float avg_latency;
} netframe_t;

//-----------------------------------------------------------------------------
typedef struct netflow_s
{
	float nextcompute;
	float avgbytespersec;
	float avgpacketspersec;
	float avgloss;
	float avgchoke;
	float avglatency;
	float latency;
	float maxlatency;
	int64_t totalpackets;
	int64_t totalbytes;
	int64_t totalupdates;
	int currentindex;
	netframe_header_t frame_headers[NET_FRAMES_BACKUP];
	netframe_t frames[NET_FRAMES_BACKUP];
	netframe_t* currentframe;
} netflow_t;

//-----------------------------------------------------------------------------
struct dataFragments_t
{
	char* buffer;
	int64_t blockSize;
	bool isCompressed;
	uint8_t gap11[7];
	int64_t uncompressedSize;
	bool firstFragment;
	bool lastFragment;
	bool isOutbound;
	int transferID;
	int transferSize;
	int currentOffset;
};

//-----------------------------------------------------------------------------
enum EBufType
{
	BUF_RELIABLE = 0,
	BUF_UNRELIABLE,
	BUF_VOICE
};

inline void(*CNetChan__Clear)(CNetChan* pChan, bool bStopProcessing);
inline void(*CNetChan__Shutdown)(CNetChan* pChan, const char* szReason, uint8_t bBadRep, bool bRemoveNow);
inline bool(*CNetChan__CanPacket)(const CNetChan* pChan);
inline void(*CNetChan__FlowNewPacket)(CNetChan* pChan, int flow, int outSeqNr, int inSeqNr, int nChoked, int nDropped, int nSize);
inline int(*CNetChan__SendDatagram)(CNetChan* pChan, bf_write* pMsg);
inline bool(*CNetChan__ProcessMessages)(CNetChan* pChan, bf_read* pMsg);
inline __int64(*CNetChan__ProcessPacketHeader)(__int64 pChan, __int64 pPacket); // [DEDI-RECV] reliable-ack drain probe
inline __int64(*CNetChan__FlowUpdate)(CNetChan* pChan, int flow, int addBytes);

//-----------------------------------------------------------------------------
class CNetChan
{
public:
	~CNetChan()
	{
		Shutdown("NetChannel removed.", 1, false);
		FreeReceiveList();
	}

	inline const char* GetName(void)                     const { return m_Name; }
	inline const char* GetAddress(bool onlyBase = false) const { return remote_address.ToString(onlyBase); }
	inline int         GetPort(void)                     const { return int(ntohs(remote_address.GetPort())); }
	inline int         GetDataRate(void)                 const { return m_Rate; }
	inline int         GetBufferSize(void)               const { return NET_FRAMES_BACKUP; }

	float        GetResendRate() const;

	inline float GetLatency(int flow)        const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].latency; }
	inline float GetAvgChoke(int flow)       const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avgchoke; }
	inline float GetAvgLatency(int flow)     const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avglatency; }
	inline float GetAvgLoss(int flow)        const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avgloss; }
	inline float GetAvgPackets(int flow)     const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avgpacketspersec; }
	inline float GetAvgData(int flow)        const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].avgbytespersec; }
	inline int64_t GetTotalData(int flow)    const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].totalbytes; }
	inline int64_t GetTotalPackets(int flow) const { Assert(flow >= 0 && flow < SDK_ARRAYSIZE(m_DataFlow)); return m_DataFlow[flow].totalpackets; }

	int         GetSequenceNr(int flow) const;
	double      GetTimeConnected(void) const;

	inline double GetLastReceivedTime(void)          const { return last_received; }
	inline float GetTimeoutSeconds(void)             const { return m_Timeout; }
	inline int   GetSocket(void)                     const { return m_Socket; }
	inline const bf_write& GetStreamVoice(void)      const { return m_StreamVoice; }
	inline const bf_write& GetStreamReliable(void)   const { return m_StreamReliable; }
	inline const bf_write& GetStreamUnreliable(void) const { return m_StreamUnreliable; }
	inline const netadr_t& GetRemoteAddress(void)    const { return remote_address; }

	int         GetNumBitsWritten(const bool bReliable);
	int         GetNumBitsLeft(const bool bReliable);
	inline bool IsOverflowed(void)                const { return m_StreamReliable.IsOverflowed(); }

	bool HasPendingReliableData(void);

	inline bool CanPacket(void) const { return CNetChan__CanPacket(this); }
	inline int SendDatagram(bf_write* pDatagram) { return CNetChan__SendDatagram(this, pDatagram); }
	bool SendNetMsg(INetMessage& msg, const bool bForceReliable, const bool bVoice);
	bool SendData(bf_write& msg, const bool bReliable);

	INetMessage* FindMessage(const int type);
	bool RegisterMessage(INetMessage* msg);

	inline void Clear(bool bStopProcessing) { CNetChan__Clear(this, bStopProcessing); }
	inline void Shutdown(const char* szReason, uint8_t bBadRep, bool bRemoveNow) { CNetChan__Shutdown(this, szReason, bBadRep, bRemoveNow); }
	void FreeReceiveList();
	bool ProcessMessages(bf_read* pMsg);

	bool ReadSubChannelData(bf_read& buf);

	static void _Shutdown(CNetChan* pChan, const char* szReason, uint8_t bBadRep, bool bRemoveNow);
	static bool _ProcessMessages(CNetChan* pChan, bf_read* pMsg);
	static int  _SendDatagram(CNetChan* pChan, bf_write* pMsg); // [DEDI-SEND] wire-capture hook
	static __int64 _ProcessPacketHeader(__int64 pChan, __int64 pPacket); // [DEDI-RECV] reliable-ack drain probe
	static __int64 _FlowUpdate(CNetChan* pChan, int flow, int addBytes);

	static void _FlowNewPacket(CNetChan* const pChan, const int flow, const int outSeqNr, const int inSeqNr, const int nChoked, const int nDropped, const int nSize);

	void SetChoked();
	void SetRemoteFramerate(float flFrameTime, float flFrameTimeStdDeviation);
	inline void SetRemoteCPUStatistics(uint8_t nStats) { m_nServerCPU = nStats; }

	//-----------------------------------------------------------------------------
public:
	bool                m_bProcessingMessages;
	bool                m_bShouldDelete;
	bool                m_bStopProcessing;
	bool                m_bShuttingDown;
	int                 m_nOutSequenceNr;
	int                 m_nInSequenceNr;
	int                 m_nOutSequenceNrAck;
	int                 m_nChokedPackets;
	int                 m_nRealTimePackets; // Number of packets without pre-scaled frame times.

private:
	int                 m_nLastRecvFlags;
	RTL_SRWLOCK         m_Lock;
	bf_write            m_StreamReliable;
	CUtlMemory<byte>    m_ReliableDataBuffer;
	bf_write            m_StreamUnreliable;
	CUtlMemory<byte>    m_UnreliableDataBuffer;
	bf_write            m_StreamVoice;
	CUtlMemory<byte>    m_VoiceDataBuffer;
	int                 m_Socket;
	int                 m_MaxReliablePayloadSize;
	double              last_received;
	double              connect_time;
	uint32_t            m_Rate;
	int                 padding_maybe;
	double              m_fClearTime;
	CUtlVector<dataFragments_t*> m_WaitingList;
	dataFragments_t     m_ReceiveList;
	int                 m_nSubOutFragmentsAck;
	int                 m_nSubInFragments;
	int                 m_nNonceHost;
	uint32_t            m_nNonceRemote;
	bool                m_bReceivedRemoteNonce;
	bool                m_bInReliableState;
	bool                m_bPendingRemoteNonceAck;
	uint32_t            m_nSubOutSequenceNr;
	int                 m_nLastRecvNonce;
	bool                m_bUseCompression;
	uint32_t            m_ChallengeNr;
	float               m_Timeout;
	INetChannelHandler* m_MessageHandler;
	CUtlVector<INetMessage*> m_NetMessages;
	void*               m_UnusedInterfacePointer;
	int                 m_nQueuedPackets;
	float               m_flRemoteFrameTime;
	float               m_flRemoteFrameTimeStdDeviation;
	uint8_t             m_nServerCPU;
	int                 m_nMaxRoutablePayloadSize;
	int                 m_nSplitPacketSequence;
	int64_t             m_StreamSendBuffer;
	bf_write            m_StreamSend;
	bool                m_bConnecting; // true if SetSignonState is called with signon < SIGNONSTATE_FULL.
	netflow_t           m_DataFlow[MAX_FLOWS];
	int                 m_nLifetimePacketsDropped;
	int                 m_nSessionPacketsDropped;
	int                 m_nSequencesSkipped;
	int                 m_nSessionRecvs;
	uint32_t            m_nLiftimeRecvs;
	bool                m_bRetrySendLong;
	char                m_Name[NET_CHANNELNAME_MAXLEN];
	netadr_t            remote_address;
};
static_assert(sizeof(CNetChan) == 0x1AC8);

//-----------------------------------------------------------------------------
// Purpose: sets the remote frame times
// Input: flFrameTime - 
// flFrameTimeStdDeviation - 
//-----------------------------------------------------------------------------
inline void CNetChan::SetRemoteFramerate(float flFrameTime, float flFrameTimeStdDeviation)
{
	m_flRemoteFrameTime = flFrameTime;
	m_flRemoteFrameTimeStdDeviation = flFrameTimeStdDeviation;
}

//-----------------------------------------------------------------------------
// Purpose: increments choked packet count
//-----------------------------------------------------------------------------
inline void CNetChan::SetChoked(void)
{
	m_nOutSequenceNr++; // Sends to be done since move command use sequence number.
	m_nChokedPackets++;
}


///////////////////////////////////////////////////////////////////////////////
class VNetChan : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CNetChan::Clear", CNetChan__Clear);
		LogFunAdr("CNetChan::Shutdown", CNetChan__Shutdown);
		LogFunAdr("CNetChan::CanPacket", CNetChan__CanPacket);
		LogFunAdr("CNetChan::FlowNewPacket", CNetChan__FlowNewPacket);
		LogFunAdr("CNetChan::SendDatagram", CNetChan__SendDatagram);
		LogFunAdr("CNetChan::ProcessMessages", CNetChan__ProcessMessages);
		LogFunAdr("CNetChan::ProcessPacketHeader", CNetChan__ProcessPacketHeader);
		LogFunAdr("CNetChan::FlowUpdate", CNetChan__FlowUpdate);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "88 54 24 10 53 55 57").GetPtr(CNetChan__Clear);
		Module_FindPattern(g_GameDll, "48 89 6C 24 18 56 57 41 56 48 83 EC 30 83 B9").GetPtr(CNetChan__Shutdown);
		Module_FindPattern(g_GameDll, "40 53 48 83 EC 20 83 B9 ?? ?? ?? ?? ?? 48 8B D9 75 15 48 8B 05 ?? ?? ?? ??").GetPtr(CNetChan__CanPacket);
		Module_FindPattern(g_GameDll, "44 89 4C 24 ?? 44 89 44 24 ?? 89 54 24 10 56").GetPtr(CNetChan__FlowNewPacket);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 55 56 57 41 56 41 57 48 83 EC 70").GetPtr(CNetChan__SendDatagram);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B FA").GetPtr(CNetChan__ProcessMessages);
		Module_FindPattern(g_GameDll, "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 48 48 8B FA 48 8D 1D ?? ?? ?? ?? 8B 52 54 4C 8D 5F 50").GetPtr(CNetChan__ProcessPacketHeader);
		Module_FindPattern(g_GameDll, "48 83 EC ?? F2 0F 10 0D ?? ?? ?? ?? 4C 8D 89").GetPtr(CNetChan__FlowUpdate);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

// Inbound message liveness for a netchan, counted where the messages are
// actually parsed. GetTotalPackets(FLOW_INCOMING) never moves on a bridge
// client, so it cannot answer "is this client still talking to us".
extern bool NetChan_GetInboundLiveness(const void* pChan, int64_t* pMsgs, double* pLast);

#endif // NET_CHAN_H
#endif // CLIENT_DLL
