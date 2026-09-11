#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: Netchannel system utilities
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/frametask.h"
#include "tier1/cvar.h"
#include "tier1/keyvalues.h"
#include "common/callback.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "engine/client/net_observer.h"

//-----------------------------------------------------------------------------
// Console variables
//-----------------------------------------------------------------------------
static ConVar net_processTimeBudget("net_processTimeBudget", "200", FCVAR_RELEASE, "Net message process time budget in milliseconds (removing netchannel if exceeded).", true, 0.f, false, 0.f, "0 = disabled");

//-----------------------------------------------------------------------------
// Purpose: gets the netchannel resend rate
// Output: float
//-----------------------------------------------------------------------------
float CNetChan::GetResendRate() const
{
	const int64_t totalupdates = this->m_DataFlow[FLOW_INCOMING].totalupdates;

	if (!totalupdates && !this->m_nSequencesSkipped)
		return 0.0f;

	float lossRate = (float)(totalupdates + m_nSequencesSkipped);

	if (totalupdates + m_nSequencesSkipped < 0.0f)
		lossRate += 18446744073709551616.0f; // 2^64 -- corrects uint64 wrap-around

	return m_nSequencesSkipped / lossRate;
}

//-----------------------------------------------------------------------------
// Purpose: gets the netchannel sequence number
// Input: flow - 
// Output: int
//-----------------------------------------------------------------------------
int CNetChan::GetSequenceNr(int flow) const
{
	if (flow == FLOW_OUTGOING)
	{
		return m_nOutSequenceNr;
	}
	else if (flow == FLOW_INCOMING)
	{
		return m_nInSequenceNr;
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: gets the netchannel connect time
// Output: double
//-----------------------------------------------------------------------------
double CNetChan::GetTimeConnected(void) const
{
	double t = *g_pNetTime - connect_time;
	return (t > 0.0) ? t : 0.0;
}

//-----------------------------------------------------------------------------
// Purpose: gets the number of bits written in selected stream
//-----------------------------------------------------------------------------
int CNetChan::GetNumBitsWritten(const bool bReliable)
{
    bf_write* pStream = &m_StreamUnreliable;

    if (bReliable)
    {
        pStream = &m_StreamReliable;
    }

    return pStream->GetNumBitsWritten();
}

//-----------------------------------------------------------------------------
// Purpose: gets the number of bits written in selected stream
//-----------------------------------------------------------------------------
int CNetChan::GetNumBitsLeft(const bool bReliable)
{
    bf_write* pStream = &m_StreamUnreliable;

    if (bReliable)
    {
        pStream = &m_StreamReliable;
    }

    return pStream->GetNumBitsLeft();
}

//-----------------------------------------------------------------------------
// Purpose: flows a new packet
// Input: *pChan - 
// outSeqNr - 
// inSeqNr - 
// nChoked - 
// nDropped - 
// nSize - 
//-----------------------------------------------------------------------------
void CNetChan::_FlowNewPacket(CNetChan* const pChan, const int flow, const int outSeqNr, const int inSeqNr, const int nChoked, const int nDropped, const int nSize)
{
    netflow_t* const pflow = &pChan->m_DataFlow[flow];

    netframe_header_t* frameheader = nullptr;
    netframe_t* frame = nullptr;

    const int currentindex = pflow->currentindex;
    const float netTime = (float)*g_pNetTime;

    if (outSeqNr > currentindex)
    {
        // If client sends a malformed packet with an 'outSeqNr' that differs
        // greatly from our current index, the loop will hang. Make sure we
        // never execute more than NET_FRAMES_BACKUP iterations as that is the
        // total storage we have in the frames and headers. If we receive a
        // packet with a greater delta, we clear all the frames to reset the
        // statistics as they have then been invalidated.
        if (outSeqNr - currentindex > NET_FRAMES_BACKUP)
        {
            memset(pflow->frame_headers, 0, sizeof(pflow->frame_headers));
            netframe_header_t* const frameHeader = &pflow->frame_headers[outSeqNr & NET_FRAMES_MASK];

            frameHeader->time = netTime;
            frameHeader->latency = -1.0f;
        }
        else
        {
            for (int i = currentindex + 1; (i <= outSeqNr); ++i)
            {
                const int frameIndex = i & NET_FRAMES_MASK;

                frameheader = &pflow->frame_headers[frameIndex];

                frameheader->time = netTime; // Now.
                frameheader->size = 0;
                frameheader->choked = 0; // Not acknowledged yet.
                frameheader->valid = false;
                frameheader->latency = -1.0f; // Not acknowledged yet.

                frame = &pflow->frames[frameIndex];

                frame->dropped = 0;
                frame->avg_latency = pChan->GetAvgLatency(FLOW_OUTGOING);

                const int backTrack = outSeqNr - i;

                if (backTrack < (nChoked + nDropped))
                {
                    if (backTrack < nChoked)
                    {
                        frameheader->choked = 1;
                    }
                    else
                    {
                        frame->dropped = 1;
                    }
                }
            }

            frameheader->size = nSize;
            frameheader->choked = (short)nChoked;
            frameheader->valid = true;
            frame->dropped = nDropped;
            frame->avg_latency = pChan->GetAvgLatency(FLOW_OUTGOING);
        }
    }

    pflow->totalpackets++;
    pflow->currentindex = outSeqNr;
    pflow->currentframe = frame;

    // Update ping for acknowledged packet.
    const int aflowIndex = (flow == FLOW_OUTGOING) ? FLOW_INCOMING : FLOW_OUTGOING;
    netflow_t* const aflow = &pChan->m_DataFlow[aflowIndex];

    if (inSeqNr > (aflow->currentindex - NET_FRAMES_BACKUP))
    {
        netframe_header_t* const aframe = &aflow->frame_headers[inSeqNr & NET_FRAMES_MASK];

        if (aframe->valid && aframe->latency == -1.0f)
        {
            const float latency = Max(0.0f, netTime - aframe->time);
            aframe->latency = latency;

            pflow->latency += latency;
            pflow->maxlatency = Max(pflow->maxlatency, latency);

            pflow->totalupdates++;
        }
    }
    else // Acknowledged packet isn't in backup buffer anymore.
    {
        netframe_header_t* const aframe = &aflow->frame_headers[aflow->currentindex & NET_FRAMES_MASK];
        netframe_header_t* const nframe = &aflow->frame_headers[aflow->currentindex + 1 & NET_FRAMES_MASK];

        static const float DELTA_INTERP = 127.0f;

        const float delta = (aframe->time - nframe->time) / DELTA_INTERP;
        const int backTrack = aflow->currentindex - inSeqNr;

        const float latency = (delta * backTrack) + netTime - aframe->time;

        pflow->latency += latency;
        pflow->maxlatency = Max(pflow->maxlatency, latency);

        pflow->totalupdates++;
    }
}

//-----------------------------------------------------------------------------
// Purpose: shutdown netchannel
// Input: *this - 
// *szReason - 
// bBadRep - 
// bRemoveNow - 
//-----------------------------------------------------------------------------
void CNetChan::_Shutdown(CNetChan* pChan, const char* szReason, uint8_t bBadRep, bool bRemoveNow)
{
	CNetChan__Shutdown(pChan, szReason, bBadRep, bRemoveNow);
}

//-----------------------------------------------------------------------------
// Purpose: process message
// Input: *pChan - 
// *pMsg - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool CNetChan::_ProcessMessages(CNetChan* pChan, bf_read* pBuf)
{
    return pChan->ProcessMessages(pBuf);
}

//-----------------------------------------------------------------------------
// Purpose: process message
// Input: *buf - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool CNetChan::ProcessMessages(bf_read* buf)
{
    m_bStopProcessing = false;

    const char* showMsgName = net_showmsg->GetString();
    const char* blockMsgName = net_blockmsg->GetString();
    const int netPeak = net_showpeaks->GetInt();

    if (*showMsgName == '0')
    {
        showMsgName = NULL; // dont do strcmp all the time
    }

    if (*blockMsgName == '0')
    {
        blockMsgName = NULL; // dont do strcmp all the time
    }

    if (netPeak > 0 && netPeak < buf->GetNumBytesLeft())
    {
        showMsgName = "1"; // show messages for this packet only
    }

    while (true)
    {
        int cmd = net_NOP;

        while (true)
        {
            if (buf->GetNumBitsLeft() < NETMSG_TYPE_BITS)
                return true; // Reached the end.

            if (!NET_ReadMessageType(&cmd, buf) && buf->m_bOverflow)
            {
                Error(eDLL_T::ENGINE, 0, "%s(%s): Incoming buffer overflow!\n", __FUNCTION__, GetAddress());
                m_MessageHandler->ConnectionCrashed("Buffer overflow in net message");

                return false;
            }

            // S3 wire IDs -> S21 handler IDs (same numeric ID is a different message).
            {
                const int origCmd = cmd;
                const int translated = S21Bridge_TranslateNetMsgType(cmd, true);
                if (translated == -1)
                {
                    // Message has no S21 equivalent -- skip this packet
                    // (can't skip body of unknown length, abandon rest)
                    static int s_suppressHist[128] = {};
                    if (origCmd >= 0 && origCmd < 128 && s_suppressHist[origCmd]++ < 3)
                    {
                        Warning(eDLL_T::ENGINE, "%s(%s): S21 Bridge: suppressing S3 msg %d (no S21 equivalent)\n",
                            __FUNCTION__, GetAddress(), origCmd);
                    }
                    return true; // keep netchannel alive
                }
                if (translated != origCmd)
                {
                    static long long s_transLog = 0;
                    if (++s_transLog <= 20 || (s_transLog % 200) == 0)
                    {
                        Msg(eDLL_T::ENGINE, "%s(%s): S21 Bridge: msg S3:%d -> S21:%d\n",
                            __FUNCTION__, GetAddress(), origCmd, translated);
                    }
                    cmd = translated;
                }
            }

            if (cmd <= net_Disconnect)
                break; // Either a Disconnect or NOP packet; process it below.

            INetMessage* netMsg = FindMessage(cmd);

            if (!netMsg)
            {
                // S21 Bridge: unknown message after translation -- log and skip packet
                static int s_unknownHist[128] = {};
                if (cmd >= 0 && cmd < 128 && s_unknownHist[cmd]++ < 3)
                {
                    Warning(eDLL_T::ENGINE, "%s(%s): S21 Bridge: unknown msg %d after translation, skipping packet\n",
                        __FUNCTION__, GetAddress(), cmd);
                }
                return true; // keep netchannel alive
            }

            // Log before ReadFromBuffer to identify which message crashes
            {
                static long long s_readLog = 0;
                if (++s_readLog <= 30 || (s_readLog % 500) == 0)
                {
                    Msg(eDLL_T::ENGINE, "%s(%s): S21 Bridge: reading msg cmd=%d name='%s' bitsLeft=%d\n",
                        __FUNCTION__, GetAddress(), cmd, netMsg->GetName(), buf->GetNumBitsLeft());
                }
            }

            // ReadFromBuffer: an AV here means a wire-format mismatch -- force
            // disconnect so the error surfaces instead of being swallowed.
            bool readOk = false;
            __try
            {
                readOk = netMsg->ReadFromBuffer(buf);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                Warning(eDLL_T::ENGINE, "%s(%s): S21 Bridge: CRASH in ReadFromBuffer for msg cmd=%d name='%s'! Disconnecting.\n",
                    __FUNCTION__, GetAddress(), cmd, netMsg->GetName());
                m_MessageHandler->ConnectionCrashed("ReadFromBuffer AV in S21 bridge");
                return false;
            }

            if (!readOk)
            {
                DevWarning(eDLL_T::ENGINE, "%s(%s): Failed reading message '%s'!\n",
                    __FUNCTION__, GetAddress(), netMsg->GetName());
                return false;
            }

            if (showMsgName)
            {
                if ((*showMsgName == '1') || !Q_stricmp(showMsgName, netMsg->GetName()))
                {
                    Msg(eDLL_T::ENGINE, "%s(%s): Received: %s\n",
                        __FUNCTION__, GetAddress(), netMsg->ToString());
                }
            }

            if (blockMsgName)
            {
                if ((*blockMsgName == '1') || !Q_stricmp(blockMsgName, netMsg->GetName()))
                {
                    Msg(eDLL_T::ENGINE, "%s(%s): Blocked: %s\n",
                        __FUNCTION__, GetAddress(), netMsg->ToString());

                    continue;
                }
            }

            // Netmessage calls the Process function that was registered by
            // it's MessageHandler.
            m_bProcessingMessages = true;
            const bool bRet = netMsg->Process();
            m_bProcessingMessages = false;

            // This means we were deleted during the processing of that message.
            if (m_bShouldDelete)
            {
                delete this;
                return false;
            }

            // This means our message buffer was freed or invalidated during
            // the processing of that message.
            if (m_bStopProcessing)
                return false;

            if (!bRet)
            {
                DevWarning(eDLL_T::ENGINE, "%s(%s): Failed processing message '%s'!\n",
                    __FUNCTION__, GetAddress(), netMsg->GetName());
                Assert(0);
                return false;
            }

            if (IsOverflowed())
                return false;
        }

        m_bProcessingMessages = true;

        if (cmd == net_NOP) // NOP; continue to next packet.
        {
            m_bProcessingMessages = false;
            continue;
        }
        else if (cmd == net_Disconnect) // Disconnect request.
        {
            char reason[1024];
            buf->ReadString(reason, sizeof(reason), false);

            m_MessageHandler->ConnectionClosing(reason, 1);
            m_bProcessingMessages = false;
        }

        m_bProcessingMessages = false;

        if (m_bShouldDelete)
            delete this;

        return false;
    }
}

bool CNetChan::ReadSubChannelData(bf_read& buf)
{
    // TODO: rebuild this and hook
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: send message
// Input: &msg - 
// bForceReliable - 
// bVoice - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool CNetChan::SendNetMsg(INetMessage& msg, const bool bForceReliable, const bool bVoice)
{
	if (remote_address.GetType() == netadrtype_t::NA_NULL)
		return true;

	bf_write* pStream = &m_StreamUnreliable;

	if (msg.IsReliable() || bForceReliable)
		pStream = &m_StreamReliable;

	if (bVoice)
		pStream = &m_StreamVoice;

	if (pStream == &m_StreamUnreliable && pStream->GetNumBytesLeft() < NET_UNRELIABLE_STREAM_MINSIZE)
		return true;

	AcquireSRWLockExclusive(&m_Lock);

	pStream->WriteUBitLong(msg.GetType(), NETMSG_TYPE_BITS);
	const bool ret = msg.WriteToBuffer(pStream);

	ReleaseSRWLockExclusive(&m_Lock);

	return !pStream->IsOverflowed() && ret;
}

//-----------------------------------------------------------------------------
// Purpose: send data
// Input: &msg - 
// bReliable - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool CNetChan::SendData(bf_write& msg, const bool bReliable)
{
    // Always queue any pending reliable data ahead of the fragmentation buffer

    if (remote_address.GetType() == netadrtype_t::NA_NULL)
        return true;

    if (msg.GetNumBitsWritten() <= 0)
        return true;

    if (msg.IsOverflowed() && !bReliable)
        return true;

    bf_write& buf = bReliable
        ? m_StreamReliable
        : m_StreamUnreliable;

    const int dataBits = msg.GetNumBitsWritten();
    const int bitsLeft = buf.GetNumBitsLeft();

    if (dataBits > bitsLeft)
    {
        if (bReliable)
        {
            Error(eDLL_T::ENGINE, 0, "%s(%s): Data too large for reliable buffer (%i > %i)!\n", 
                __FUNCTION__, GetAddress(), msg.GetNumBytesWritten(), buf.GetNumBytesLeft());

            m_MessageHandler->ChannelDisconnect("reliable buffer is full");
        }

        return false;
    }

    return buf.WriteBits(msg.GetData(), dataBits);
}

//-----------------------------------------------------------------------------
// Purpose: finds a registered net message by type
// Input: type - 
// Output: net message pointer on success, NULL otherwise
//-----------------------------------------------------------------------------
INetMessage* CNetChan::FindMessage(const int type)
{
    const int numtypes = m_NetMessages.Count();

    for (int i = 0; i < numtypes; i++)
    {
        INetMessage* const message = m_NetMessages[i];

        if (message->GetType() == type)
            return message;
    }

    return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: registers a net message
// Input: *msg
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool CNetChan::RegisterMessage(INetMessage* msg)
{
    Assert(msg);

    if (FindMessage(msg->GetType()))
    {
        Assert(0); // Duplicate registration!
        return false;
    }

    m_NetMessages.AddToTail(msg);
    msg->SetNetChannel(this);

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: free's the receive data fragment list
//-----------------------------------------------------------------------------
void CNetChan::FreeReceiveList()
{
    m_ReceiveList.blockSize = NULL;
    m_ReceiveList.transferSize = NULL;
    if (m_ReceiveList.buffer)
    {
        delete m_ReceiveList.buffer;
        m_ReceiveList.buffer = nullptr;
    }
}

//-----------------------------------------------------------------------------
// Purpose: check if there is still data in the reliable waiting buffers
//-----------------------------------------------------------------------------
bool CNetChan::HasPendingReliableData(void)
{
	return (m_StreamReliable.GetNumBitsWritten() > 0)
		|| (m_WaitingList.Count() > 0);
}

///////////////////////////////////////////////////////////////////////////////
void VNetChan::Detour(const bool bAttach) const
{
	DetourSetup(&CNetChan__Shutdown, &CNetChan::_Shutdown, bAttach);
	// Do not hook FlowNewPacket (S3 offsets). Call native from S21Bridge_Hook_ProcessPacket.
	// DetourSetup(&CNetChan__FlowNewPacket, &CNetChan::_FlowNewPacket, bAttach);

	// ProcessMessages hook off: SDK handler +368 crashes on S21 (+8584).
	// DetourSetup(&CNetChan__ProcessMessages, &CNetChan::_ProcessMessages, bAttach);

	// S3-format ProcessPacket / SendDatagram. SendDatagram guards outSeq<=1 for OOB.
	Warning(eDLL_T::ENGINE, "[BRIDGE] VNetChan::Detour(%d): ProcessPacket=%p SendDatagram=%p SendNetMsg(S21)=%p\n",
		bAttach ? 1 : 0, (void*)CNetChan__ProcessPacket, (void*)CNetChan__SendDatagram, (void*)CNetChan__SendNetMsg);
	if (CNetChan__ProcessPacket)
		DetourSetup(&CNetChan__ProcessPacket, &S21Bridge_Hook_ProcessPacket, bAttach);
	if (CNetChan__SendDatagram)
		DetourSetup(&CNetChan__SendDatagram, &S21Bridge_Hook_SendDatagram, bAttach);
	// FlowUpdate: the bridge overwrites the channel's avg flow stats with real values.
	if (CNetChan__FlowUpdate)
		DetourSetup(&CNetChan__FlowUpdate, &S21Bridge_Hook_FlowUpdate, bAttach);
	// SendNetMsg: diagnostic only (which msgs route through SendNetMsg vs. inline).
	if (CNetChan__SendNetMsg)
		DetourSetup(&CNetChan__SendNetMsg, &S21Bridge_Hook_SendNetMsg, bAttach);
}
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: Netchannel system utilities
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/frametask.h"
#include "tier1/cvar.h"
#include "tier1/keyvalues.h"
#include "common/callback.h"
#include "engine/net.h"
#include "engine/net_chan.h"
#include "engine/host.h"
#include "engine/shared/s21_bridge_compat.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "server/vengineserver_impl.h"
#include <cmath>

//-----------------------------------------------------------------------------
// Console variables
//-----------------------------------------------------------------------------
static ConVar net_processTimeBudget("net_processTimeBudget", "200", FCVAR_RELEASE, "Net message process time budget in milliseconds (removing netchannel if exceeded).", true, 0.f, false, 0.f, "0 = disabled");

// Log ProcessMessages calls longer than this many ms. 0 = off.
static ConVar net_processTimeLogMs("net_processTimeLogMs", "25", FCVAR_RELEASE,
    "Log [NETPROC-DEEP] for any CNetChan::ProcessMessages call exceeding N ms (0 = disabled).",
    true, 0.f, false, 0.f, "0 = disabled");

static ConVar bridge_dedi_latency_refine("bridge_dedi_latency_refine", "1", FCVAR_RELEASE,
	"Overwrite FLOW_OUTGOING avglatency with the client's measured RTT, clamped "
	"to native ping plus or minus one tick. 0 = native tick-quantized ping.");
static ConVar bridge_latency_refine_diag("bridge_latency_refine_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log [RTT-REFINE] native vs reported vs published latency.");

// Break a slow net_SignonState handler into ActivatePlayer / DataBlock / post.
static ConVar net_signon_phase_log("net_signon_phase_log", "1", FCVAR_DEVELOPMENTONLY,
    "Log [SIGNON-PHASE] breakdown for any net_SignonState Process exceeding "
    "net_processTimeLogMs (0 = disable).");

// Subtract Plat_AccumulateStallTime (log-volume block) from the processing budget.
static ConVar net_processTimeStallCredit("net_processTimeStallCredit", "1", FCVAR_RELEASE,
    "Discount time the frame thread spent blocked in the SDK's own log emission from "
    "the net_processTimeBudget charge.", true, 0.f, true, 1.f, "0 = charge wall time");

// SetNetProcessingTimeMsecs stores one call, not a window sum. Require repeated overruns.
static ConVar net_processTimeStrikes("net_processTimeStrikes", "3", FCVAR_RELEASE,
    "Consecutive over-budget ProcessMessages calls required before the netchannel is "
    "removed. 1 = remove on the first overrun.", true, 1.f, false, 0.f);

// Strikes decay: an overrun this far apart from the previous one starts a new run.
static constexpr uint64_t NETPROC_STRIKE_WINDOW_MS = 5000;

// Per-slot consecutive-overrun state. File-scope rather than a CClientExtended member
// so the watchdog needs no lifecycle hook: a run is only ever consecutive AND inside
// NETPROC_STRIKE_WINDOW_MS, so a slot reused by a new client self-heals.
struct NetProcStrike_t
{
    uint64_t m_nLastOverrunMs;
    int m_nConsecutive;
};

static NetProcStrike_t s_NetProcStrikes[MAX_PLAYERS];

// Hex-dump CNetChan::SendDatagram buffer. Default off.
static ConVar net_dumpSendDatagram("net_dumpSendDatagram", "0", FCVAR_DEVELOPMENTONLY,
    "Log [DEDI-SEND] hex of each outgoing CNetChan::SendDatagram buffer (0 = disabled).");

// Set when this ProcessMessages call handled net_SignonState (cmd=5). Watchdog bypass.
thread_local bool g_bDidProcessSignonStateMsg = false;

// [SIGNON-PHASE] see net_chan.h. Reset per cmd=5 dispatch, accumulated inside
// the detours that own each phase, reported once the handler exceeds the log
// threshold.
thread_local double g_flSignonPhaseActivateMs = 0.0;
thread_local double g_flSignonPhaseDbBlockMs = 0.0;
thread_local double g_flSignonPhasePostMs = 0.0;

//-----------------------------------------------------------------------------
// Purpose: gets the netchannel resend rate
// Output: float
//-----------------------------------------------------------------------------
float CNetChan::GetResendRate() const
{
	const int64_t totalupdates = this->m_DataFlow[FLOW_INCOMING].totalupdates;

	if (!totalupdates && !this->m_nSequencesSkipped)
		return 0.0f;

	float lossRate = (float)(totalupdates + m_nSequencesSkipped);

	if (totalupdates + m_nSequencesSkipped < 0.0f)
		lossRate += float(2 ^ 64);

	return m_nSequencesSkipped / lossRate;
}

//-----------------------------------------------------------------------------
// Purpose: gets the netchannel sequence number
// Input: flow - 
// Output: int
//-----------------------------------------------------------------------------
int CNetChan::GetSequenceNr(int flow) const
{
	if (flow == FLOW_OUTGOING)
	{
		return m_nOutSequenceNr;
	}
	else if (flow == FLOW_INCOMING)
	{
		return m_nInSequenceNr;
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: gets the netchannel connect time
// Output: double
//-----------------------------------------------------------------------------
double CNetChan::GetTimeConnected(void) const
{
	double t = *g_pNetTime - connect_time;
	return (t > 0.0) ? t : 0.0;
}

//-----------------------------------------------------------------------------
// Purpose: gets the number of bits written in selected stream
//-----------------------------------------------------------------------------
int CNetChan::GetNumBitsWritten(const bool bReliable)
{
    bf_write* pStream = &m_StreamUnreliable;

    if (bReliable)
    {
        pStream = &m_StreamReliable;
    }

    return pStream->GetNumBitsWritten();
}

//-----------------------------------------------------------------------------
// Purpose: gets the number of bits written in selected stream
//-----------------------------------------------------------------------------
int CNetChan::GetNumBitsLeft(const bool bReliable)
{
    bf_write* pStream = &m_StreamUnreliable;

    if (bReliable)
    {
        pStream = &m_StreamReliable;
    }

    return pStream->GetNumBitsLeft();
}

//-----------------------------------------------------------------------------
// Purpose: flows a new packet
// Input: *pChan - 
// outSeqNr - 
// inSeqNr - 
// nChoked - 
// nDropped - 
// nSize - 
//-----------------------------------------------------------------------------
void CNetChan::_FlowNewPacket(CNetChan* const pChan, const int flow, const int outSeqNr, const int inSeqNr, const int nChoked, const int nDropped, const int nSize)
{
    netflow_t* const pflow = &pChan->m_DataFlow[flow];

    netframe_header_t* frameheader = nullptr;
    netframe_t* frame = nullptr;

    const int currentindex = pflow->currentindex;
    const float netTime = (float)*g_pNetTime;

    if (outSeqNr > currentindex)
    {
        // If client sends a malformed packet with an 'outSeqNr' that differs
        // greatly from our current index, the loop will hang. Make sure we
        // never execute more than NET_FRAMES_BACKUP iterations as that is the
        // total storage we have in the frames and headers. If we receive a
        // packet with a greater delta, we clear all the frames to reset the
        // statistics as they have then been invalidated.
        if (outSeqNr - currentindex > NET_FRAMES_BACKUP)
        {
            memset(pflow->frame_headers, 0, sizeof(pflow->frame_headers));
            netframe_header_t* const frameHeader = &pflow->frame_headers[outSeqNr & NET_FRAMES_MASK];

            frameHeader->time = netTime;
            frameHeader->latency = -1.0f;
        }
        else
        {
            for (int i = currentindex + 1; (i <= outSeqNr); ++i)
            {
                const int frameIndex = i & NET_FRAMES_MASK;

                frameheader = &pflow->frame_headers[frameIndex];

                frameheader->time = netTime; // Now.
                frameheader->size = 0;
                frameheader->choked = 0; // Not acknowledged yet.
                frameheader->valid = false;
                frameheader->latency = -1.0f; // Not acknowledged yet.

                frame = &pflow->frames[frameIndex];

                frame->dropped = 0;
                frame->avg_latency = pChan->GetAvgLatency(FLOW_OUTGOING);

                const int backTrack = outSeqNr - i;

                if (backTrack < (nChoked + nDropped))
                {
                    if (backTrack < nChoked)
                    {
                        frameheader->choked = 1;
                    }
                    else
                    {
                        frame->dropped = 1;
                    }
                }
            }

            frameheader->size = nSize;
            frameheader->choked = (short)nChoked;
            frameheader->valid = true;
            frame->dropped = nDropped;
            frame->avg_latency = pChan->GetAvgLatency(FLOW_OUTGOING);
        }
    }

    pflow->totalpackets++;
    pflow->currentindex = outSeqNr;
    pflow->currentframe = frame;

    // Update ping for acknowledged packet.
    const int aflowIndex = (flow == FLOW_OUTGOING) ? FLOW_INCOMING : FLOW_OUTGOING;
    netflow_t* const aflow = &pChan->m_DataFlow[aflowIndex];

    if (inSeqNr > (aflow->currentindex - NET_FRAMES_BACKUP))
    {
        netframe_header_t* const aframe = &aflow->frame_headers[inSeqNr & NET_FRAMES_MASK];

        if (aframe->valid && aframe->latency == -1.0f)
        {
            const float latency = Max(0.0f, netTime - aframe->time);
            aframe->latency = latency;

            pflow->latency += latency;
            pflow->maxlatency = Max(pflow->maxlatency, latency);

            pflow->totalupdates++;
        }
    }
    else // Acknowledged packet isn't in backup buffer anymore.
    {
        netframe_header_t* const aframe = &aflow->frame_headers[aflow->currentindex & NET_FRAMES_MASK];
        netframe_header_t* const nframe = &aflow->frame_headers[aflow->currentindex + 1 & NET_FRAMES_MASK];

        static const float DELTA_INTERP = 127.0f;

        const float delta = (aframe->time - nframe->time) / DELTA_INTERP;
        const int backTrack = aflow->currentindex - inSeqNr;

        const float latency = (delta * backTrack) + netTime - aframe->time;

        pflow->latency += latency;
        pflow->maxlatency = Max(pflow->maxlatency, latency);

        pflow->totalupdates++;
    }
}

//-----------------------------------------------------------------------------
// Purpose: shutdown netchannel
// Input: *this - 
// *szReason - 
// bBadRep - 
// bRemoveNow - 
//-----------------------------------------------------------------------------
void CNetChan::_Shutdown(CNetChan* pChan, const char* szReason, uint8_t bBadRep, bool bRemoveNow)
{
	CNetChan__Shutdown(pChan, szReason, bBadRep, bRemoveNow);
}

//-----------------------------------------------------------------------------
// Purpose: process message
// Input: *pChan - 
// *pMsg - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool CNetChan::_ProcessMessages(CNetChan* pChan, bf_read* pBuf)
{
    if (!net_processTimeBudget.GetInt() || !ThreadInServerFrameThread())
        return pChan->ProcessMessages(pBuf);

    // Clear the per-call net_SignonState marker. The dispatch loop inside
    // ProcessMessages sets it whenever cmd=5 fires; the watchdog reads it
    // after the call to decide whether to grant a budget bypass.
    g_bDidProcessSignonStateMsg = false;

    const double flStartTime = Plat_FloatTime();
    const double flStallBase = Plat_GetThreadStallTime();
    const bool bResult = pChan->ProcessMessages(pBuf);

    // false: ProcessMessages may have already freed this channel
    // (m_bShouldDelete). Every engine caller treats false as do-not-touch.
    if (!bResult)
        return false;

    if (!pChan->m_MessageHandler)
        return bResult;

    CClient* const pClient = reinterpret_cast<CClient*>(pChan->m_MessageHandler);
    CClientExtended* const pExtended = pClient->GetClientExtended();

    // Reset every second.
    if ((flStartTime - pExtended->GetNetProcessingTimeBase()) > 1.0)
    {
        pExtended->SetNetProcessingTimeBase(flStartTime);
        pExtended->SetNetProcessingTimeMsecs(0.0, 0.0);
    }

    const double flCurrentTime = Plat_FloatTime();
    const double flRawSecs = flCurrentTime - flStartTime;

    // [NETPROC-STALL] see the convar. Clamped to the call's own duration so the
    // credit can never exceed what actually elapsed here.
    const double flStallSecs = net_processTimeStallCredit.GetBool()
        ? Clamp(Plat_GetThreadStallTime() - flStallBase, 0.0, flRawSecs)
        : 0.0;

    // Shifting the start forward by the stall makes every downstream reader --
    // the overrun test, [NETPROC-DEEP], the removal message -- report processing
    // time rather than wall time, with no second accessor to keep in sync.
    pExtended->SetNetProcessingTimeMsecs(flStartTime + flStallSecs, flCurrentTime);

    const float flBudgetMs = net_processTimeBudget.GetFloat();
    const double flStallMs = flStallSecs * 1000.0;

    if (flStallMs > 0.0 && (flRawSecs * 1000.0) > flBudgetMs
        && pExtended->GetNetProcessingTimeMsecs() <= flBudgetMs)
    {
        Warning(eDLL_T::SERVER,
            "[NETPROC-STALL] %s(%s) call took %.1fms wall but %.1fms of that was the "
            "frame thread blocked in log emission -- %.1fms of processing, not charged "
            "(budget=%.0fms)\n",
            pChan->GetName(), pChan->GetAddress(), flRawSecs * 1000.0, flStallMs,
            pExtended->GetNetProcessingTimeMsecs(), flBudgetMs);
    }

    // One-line breakdown for a long ProcessMessages call. Duration is this call only.
    const float flProcMs = static_cast<float>(pExtended->GetNetProcessingTimeMsecs());
    const float flProcLogMs = net_processTimeLogMs.GetFloat();
    if (flProcLogMs > 0.f && flProcMs > flProcLogMs)
    {
        Msg(eDLL_T::SERVER,
            "[NETPROC-DEEP] %s(%s) ProcessMessages proc=%.1fms wall=%.1fms logstall=%.1fms "
            "budget=%.0fms addr=%p slot=%d signon=%d\n",
            pChan->GetName(), pChan->GetAddress(),
            flProcMs, flRawSecs * 1000.0, flStallMs, net_processTimeBudget.GetFloat(),
            (void*)pChan, *reinterpret_cast<int*>(reinterpret_cast<char*>(pClient) + 0x10),
            *reinterpret_cast<int*>(reinterpret_cast<char*>(pClient) + 0x3B0));
    }

    // net_SignonState (cmd=5) runs DecideRespawnPlayer synchronously; grant one-call pass.
    const int nSlot = pClient->GetUserID();
    NetProcStrike_t* const pStrike = (nSlot >= 0 && nSlot < MAX_PLAYERS)
        ? &s_NetProcStrikes[nSlot]
        : nullptr;

    if (pExtended->GetNetProcessingTimeMsecs() > flBudgetMs)
    {
        const double flOverrunMs = pExtended->GetNetProcessingTimeMsecs() - flBudgetMs;

        if (g_bDidProcessSignonStateMsg)
        {
            DevMsg(eDLL_T::SERVER,
                "[NETPROC-BUDGET] %s(%s) overran budget by %3.1fms while "
                "processing net_SignonState -- pass granted (spawn handler is synchronous)\n",
                pChan->GetName(), pChan->GetAddress(), flOverrunMs);
            pExtended->SetNetProcessingTimeBase(flCurrentTime);
            pExtended->SetNetProcessingTimeMsecs(flCurrentTime, flCurrentTime);

            if (pStrike)
                pStrike->m_nConsecutive = 0;

            return bResult;
        }

        // Real processing overrun (log-emission stalls are already discounted).
        // Only act once it repeats -- a lone spike is the synchronous handler
        // chain, not a client worth dropping.
        int nStrikes = 1;
        const int nStrikesNeeded = net_processTimeStrikes.GetInt();

        if (pStrike)
        {
            const uint64_t nNowMs = GetTickCount64();

            if (pStrike->m_nConsecutive > 0
                && (nNowMs - pStrike->m_nLastOverrunMs) <= NETPROC_STRIKE_WINDOW_MS)
                nStrikes = pStrike->m_nConsecutive + 1;

            pStrike->m_nConsecutive = nStrikes;
            pStrike->m_nLastOverrunMs = nNowMs;
        }

        if (nStrikes < nStrikesNeeded)
        {
            Warning(eDLL_T::SERVER,
                "[NETPROC-BUDGET] %s(%s) exceeded time budget by %3.1fms (strike %d/%d, "
                "log stall %.1fms discounted)\n",
                pChan->GetName(), pChan->GetAddress(), flOverrunMs,
                nStrikes, nStrikesNeeded, flStallMs);

            pExtended->SetNetProcessingTimeBase(flCurrentTime);
            pExtended->SetNetProcessingTimeMsecs(flCurrentTime, flCurrentTime);

            return bResult;
        }

        if (pStrike)
            pStrike->m_nConsecutive = 0;

        Warning(eDLL_T::SERVER, "Removing netchannel %s(%s) (exceeded time budget by %3.1fms on %d consecutive calls!)\n",
            pChan->GetName(), pChan->GetAddress(), flOverrunMs, nStrikes);
        pClient->Disconnect(Reputation_t::REP_MARK_BAD, "#DISCONNECT_NETCHAN_OVERFLOW");

        return false;
    }

    if (pStrike)
        pStrike->m_nConsecutive = 0;

    return bResult;
}

//-----------------------------------------------------------------------------
// Purpose: dump SendDatagram buffer. Stream at chan+0x1C0, bit cursor at +0x1D0.
// Input: *pChan -
// *pMsg - unreliable datagram passed straight through
// Output: outgoing sequence number (passthrough of the original)
//-----------------------------------------------------------------------------
int CNetChan::_SendDatagram(CNetChan* pChan, bf_write* pMsg)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(pChan);

    // m_nChokedPackets (chan+0x10) is reset to 0 by SendDatagram, so latch it now.
    const int chokedPre = *reinterpret_cast<const int*>(base + 0x10);

    const int result = CNetChan__SendDatagram(pChan, pMsg);

    if (net_dumpSendDatagram.GetBool())
    {
        const uint8_t* buf = *reinterpret_cast<uint8_t* const*>(base + 0x1C0); // send stream data ptr
        const int      bits = *reinterpret_cast<const int*>(base + 0x1D0);     // send stream bit cursor
        const int      nby = (bits + 7) >> 3;

        if (buf && nby >= 9 && nby < 0x40000)
        {
            const uint32_t seq = *reinterpret_cast<const uint32_t*>(buf + 0);
            const uint32_t ack = *reinterpret_cast<const uint32_t*>(buf + 4);
            const uint8_t  flags = buf[8];

            char hx[3 * 40 + 1] = {};
            const int n = (nby < 40) ? nby : 40;
            for (int i = 0; i < n; ++i)
                snprintf(hx + i * 3, sizeof(hx) - (i * 3), "%02X ", buf[i]);

            Warning(eDLL_T::ENGINE,
                "[DEDI-SEND] seq=%u ack=%u flags=0x%02X chokedPre=%d nbytes=%d buf[0..%d]: %s\n",
                seq, ack, flags, chokedPre, nby, n, hx);
        }
    }

    return result;
}

//-----------------------------------------------------------------------------
// Purpose: probe reliable-subchannel drain after ProcessPacketHeader.
// nonce = chan+0x150, subOut = +0x148, pend = +0x110.
//-----------------------------------------------------------------------------
__int64 CNetChan::_ProcessPacketHeader(__int64 pChan, __int64 pPacket)
{
    const unsigned int nonce        = *reinterpret_cast<const unsigned int*>(pChan + 0x150); // m_nNonceHost
    const int          subOutBefore = *reinterpret_cast<const int*>(pChan + 0x148);          // m_nSubOutSequenceNr
    const int          pendBefore   = *reinterpret_cast<const int*>(pChan + 0x110);          // pending frags

    const __int64 result = CNetChan__ProcessPacketHeader(pChan, pPacket);

    if (net_dumpSendDatagram.GetBool())
    {
        const int subOutAfter = *reinterpret_cast<const int*>(pChan + 0x148);
        const int pendAfter   = *reinterpret_cast<const int*>(pChan + 0x110);

        // Only log packets that mattered to the reliable subchannel: a pending
        // resend was outstanding, or the drain actually fired.
        if (pendBefore > 0 || subOutBefore != subOutAfter)
        {
            static long long s_n = 0;
            if (++s_n <= 200 || (s_n % 50) == 0)
                Warning(eDLL_T::ENGINE,
                    "[DEDI-RECV] nonce=0x%08X subOut=%d->%d pend=%d->%d hdr=0x%08X\n",
                    nonce, subOutBefore, subOutAfter, pendBefore, pendAfter,
                    static_cast<unsigned int>(result));
        }
    }

    return result;
}

//-----------------------------------------------------------------------------
// Purpose: inbound-liveness for the signon park report (ProcessMessages path).
//-----------------------------------------------------------------------------
namespace
{
    struct C2SLiveness_s
    {
        const void* pChan;
        int64_t     nMsgs;
        double      flLast;
    };

    static C2SLiveness_s s_c2sLive[MAX_PLAYERS] = {};
    static SRWLOCK       s_c2sLiveLock = SRWLOCK_INIT;
} // namespace

static void NetChan_NoteInbound(const void* pChan)
{
    AcquireSRWLockExclusive(&s_c2sLiveLock);
    C2SLiveness_s* pFree = nullptr;
    for (C2SLiveness_s& live : s_c2sLive)
    {
        if (live.pChan == pChan)
        {
            ++live.nMsgs;
            live.flLast = Plat_FloatTime();
            ReleaseSRWLockExclusive(&s_c2sLiveLock);
            return;
        }
        if (!pFree && !live.pChan)
            pFree = &live;
    }
    if (pFree)
    {
        pFree->pChan = pChan;
        pFree->nMsgs = 1;
        pFree->flLast = Plat_FloatTime();
    }
    ReleaseSRWLockExclusive(&s_c2sLiveLock);
}

bool NetChan_GetInboundLiveness(const void* pChan, int64_t* pMsgs, double* pLast)
{
    bool bFound = false;
    AcquireSRWLockShared(&s_c2sLiveLock);
    for (const C2SLiveness_s& live : s_c2sLive)
    {
        if (live.pChan == pChan)
        {
            *pMsgs = live.nMsgs;
            *pLast = live.flLast;
            bFound = true;
            break;
        }
    }
    ReleaseSRWLockShared(&s_c2sLiveLock);
    return bFound;
}

//-----------------------------------------------------------------------------
// Purpose: process message
// Input: *buf -
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool CNetChan::ProcessMessages(bf_read* buf)
{
    m_bStopProcessing = false;

    NetChan_NoteInbound(this);

    const char* showMsgName = net_showmsg->GetString();
    const char* blockMsgName = net_blockmsg->GetString();
    const int netPeak = net_showpeaks->GetInt();

    if (*showMsgName == '0')
    {
        showMsgName = NULL; // dont do strcmp all the time
    }

    if (*blockMsgName == '0')
    {
        blockMsgName = NULL; // dont do strcmp all the time
    }

    if (netPeak > 0 && netPeak < buf->GetNumBytesLeft())
    {
        showMsgName = "1"; // show messages for this packet only
    }

    while (true)
    {
        int cmd = net_NOP;

        while (true)
        {
            if (buf->GetNumBitsLeft() < NETMSG_TYPE_BITS)
                return true; // Reached the end.

            if (!NET_ReadMessageType(&cmd, buf) && buf->m_bOverflow)
            {
                Error(eDLL_T::ENGINE, 0, "%s(%s): Incoming buffer overflow!\n", __FUNCTION__, GetAddress());
                m_MessageHandler->ConnectionCrashed("Buffer overflow in net message");

                return false;
            }

            if (cmd <= net_Disconnect)
                break; // Either a Disconnect or NOP packet; process it below.

            INetMessage* netMsg = FindMessage(cmd);

            if (!netMsg)
            {
                DevWarning(eDLL_T::ENGINE, "%s(%s): Received unknown net message (%i)!\n",
                    __FUNCTION__, GetAddress(), cmd);
                Assert(0);
                return false;
            }

            if (!netMsg->ReadFromBuffer(buf))
            {
                DevWarning(eDLL_T::ENGINE, "%s(%s): Failed reading message '%s'!\n",
                    __FUNCTION__, GetAddress(), netMsg->GetName());
                Assert(0);
                return false;
            }

            if (showMsgName)
            {
                if ((*showMsgName == '1') || !Q_stricmp(showMsgName, netMsg->GetName()))
                {
                    Msg(eDLL_T::ENGINE, "%s(%s): Received: %s\n",
                        __FUNCTION__, GetAddress(), netMsg->ToString());
                }
            }

            if (blockMsgName)
            {
                if ((*blockMsgName == '1') || !Q_stricmp(blockMsgName, netMsg->GetName()))
                {
                    Msg(eDLL_T::ENGINE, "%s(%s): Blocked: %s\n",
                        __FUNCTION__, GetAddress(), netMsg->ToString());

                    continue;
                }
            }

            // Watchdog bypass: net_SignonState spawn handler is synchronous.
            if (cmd == 5) // NET_SignonState
            {
                g_bDidProcessSignonStateMsg = true;

                // [SIGNON-PHASE] start each handler from zero so the detours
                // that own the phases only ever accumulate this call's time.
                g_flSignonPhaseActivateMs = 0.0;
                g_flSignonPhaseDbBlockMs = 0.0;
                g_flSignonPhasePostMs = 0.0;
            }

            // Reconnect callers compare spawn counts (net_SignonState, clc_ClientInfo). Payloads at +32.
            if (cmd == 5 || cmd == 45)
            {
                const int* const pFields = reinterpret_cast<const int*>(
                    reinterpret_cast<const char*>(netMsg) + 32);
                const int nServerSpawn = g_pServer ? g_pServer->GetSpawnCount() : -1;
                const int nState = (cmd == 5) ? pFields[0] : -1;
                const int nSpawn = (cmd == 5) ? pFields[1] : pFields[2];
                const bool bWouldReconnect = (cmd == 5)
                    ? (nState > 2 && nSpawn != nServerSpawn)
                    : (nSpawn != nServerSpawn);

                // The client re-asks a rung until the server answers, so the same
                // tuple arrives several times a second. Collapse the repeats and
                // keep every rejection.
                static const void* s_pLastChan = nullptr;
                static int s_nLastKey = INT_MIN;
                const int nKey = (cmd << 24) ^ (nState << 16) ^ nSpawn;
                const bool bRepeat = (s_pLastChan == this && s_nLastKey == nKey);
                s_pLastChan = this;
                s_nLastKey = nKey;

                if (bWouldReconnect || !bRepeat)
                {
                    if (cmd == 5) // NET_SignonState: +32 state, +36 spawn
                        Warning(eDLL_T::ENGINE,
                            "[SRV-SIGNON] net_SignonState RECV state=%d spawn=%d srvSpawn=%d "
                            "reconnect=%d from %s\n",
                            nState, nSpawn, nServerSpawn, bWouldReconnect ? 1 : 0,
                            GetAddress());
                    else          // clc_ClientInfo: +32 sendtable CRC, +40 spawn
                        Warning(eDLL_T::ENGINE,
                            "[SRV-SIGNON] clc_ClientInfo RECV crc=0x%08X spawn=%d srvSpawn=%d "
                            "reconnect=%d from %s\n",
                            pFields[0], nSpawn, nServerSpawn, bWouldReconnect ? 1 : 0,
                            GetAddress());
                }
            }

            // Netmessage calls the Process function that was registered by
            // it's MessageHandler.
            m_bProcessingMessages = true;
            // Per-message timing against net_processTimeLogMs.
            const double flNetMsgStart = Plat_FloatTime();
            const double flNetMsgStallBase = Plat_GetThreadStallTime();
            const bool bRet = netMsg->Process();
            const double flNetMsgEnd = Plat_FloatTime();
            const float flNetMsgMs = static_cast<float>((flNetMsgEnd - flNetMsgStart) * 1000.0);
            // Split out the part of the wall time that was this thread blocked in
            // log emission rather than in the handler -- without it a stalled
            // write reads as a handler that took seconds.
            const float flNetMsgStallMs = static_cast<float>(
                Clamp(Plat_GetThreadStallTime() - flNetMsgStallBase, 0.0,
                    flNetMsgEnd - flNetMsgStart) * 1000.0);
            if (flNetMsgMs > net_processTimeLogMs.GetFloat() && net_processTimeLogMs.GetFloat() > 0.f)
            {
                Msg(eDLL_T::SERVER,
                    "[NETMSG-TIME] %s(%s) msg='%s' type=%d dur=%.1fms logstall=%.1fms\n",
                    __FUNCTION__, GetAddress(), netMsg->GetName(), cmd, flNetMsgMs,
                    flNetMsgStallMs);

                // ActivatePlayer / DataBlock / post vs native residual. Payloads at +32.
                if (cmd == 5 && net_signon_phase_log.GetBool())
                {
                    float flNative = flNetMsgMs
                        - static_cast<float>(g_flSignonPhaseActivateMs)
                        - static_cast<float>(g_flSignonPhaseDbBlockMs)
                        - static_cast<float>(g_flSignonPhasePostMs);
                    if (flNative < 0.0f)
                        flNative = 0.0f; // rounding noise, never negative
                    const int* const pFields = reinterpret_cast<const int*>(
                        reinterpret_cast<const char*>(netMsg) + 32);
                    Msg(eDLL_T::SERVER,
                        "[SIGNON-PHASE] dur=%.1fms activate=%.1fms datablock=%.1fms "
                        "post=%.1fms native=%.1fms state=%d spawn=%d\n",
                        flNetMsgMs,
                        g_flSignonPhaseActivateMs, g_flSignonPhaseDbBlockMs,
                        g_flSignonPhasePostMs, flNative, pFields[0], pFields[1]);
                }
            }
            m_bProcessingMessages = false;

            // This means we were deleted during the processing of that message.
            if (m_bShouldDelete)
            {
                delete this;
                return false;
            }

            // This means our message buffer was freed or invalidated during
            // the processing of that message.
            if (m_bStopProcessing)
                return false;

            if (!bRet)
            {
                DevWarning(eDLL_T::ENGINE, "%s(%s): Failed processing message '%s'!\n",
                    __FUNCTION__, GetAddress(), netMsg->GetName());
                Assert(0);
                return false;
            }

            if (IsOverflowed())
                return false;
        }

        m_bProcessingMessages = true;

        if (cmd == net_NOP) // NOP; continue to next packet.
        {
            m_bProcessingMessages = false;
            continue;
        }
        else if (cmd == net_Disconnect) // Disconnect request.
        {
            char reason[1024];
            buf->ReadString(reason, sizeof(reason), false);

            m_MessageHandler->ConnectionClosing(reason, 1);
            m_bProcessingMessages = false;
        }

        m_bProcessingMessages = false;

        if (m_bShouldDelete)
            delete this;

        return false;
    }
}

bool CNetChan::ReadSubChannelData(bf_read& buf)
{
    // TODO: rebuild this and hook
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: send message
// Input: &msg - 
// bForceReliable - 
// bVoice - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool CNetChan::SendNetMsg(INetMessage& msg, const bool bForceReliable, const bool bVoice)
{
	if (remote_address.GetType() == netadrtype_t::NA_NULL)
		return true;

	bf_write* pStream = &m_StreamUnreliable;

	if (msg.IsReliable() || bForceReliable)
		pStream = &m_StreamReliable;

	if (bVoice)
		pStream = &m_StreamVoice;

	if (pStream == &m_StreamUnreliable && pStream->GetNumBytesLeft() < NET_UNRELIABLE_STREAM_MINSIZE)
		return true;

	AcquireSRWLockExclusive(&m_Lock);

	pStream->WriteUBitLong(msg.GetType(), NETMSG_TYPE_BITS);
	const bool ret = msg.WriteToBuffer(pStream);

	ReleaseSRWLockExclusive(&m_Lock);

	return !pStream->IsOverflowed() && ret;
}

//-----------------------------------------------------------------------------
// Purpose: send data
// Input: &msg - 
// bReliable - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool CNetChan::SendData(bf_write& msg, const bool bReliable)
{
    // Always queue any pending reliable data ahead of the fragmentation buffer

    if (remote_address.GetType() == netadrtype_t::NA_NULL)
        return true;

    if (msg.GetNumBitsWritten() <= 0)
        return true;

    if (msg.IsOverflowed() && !bReliable)
        return true;

    bf_write& buf = bReliable
        ? m_StreamReliable
        : m_StreamUnreliable;

    const int dataBits = msg.GetNumBitsWritten();
    const int bitsLeft = buf.GetNumBitsLeft();

    if (dataBits > bitsLeft)
    {
        if (bReliable)
        {
            Error(eDLL_T::ENGINE, 0, "%s(%s): Data too large for reliable buffer (%i > %i)!\n", 
                __FUNCTION__, GetAddress(), msg.GetNumBytesWritten(), buf.GetNumBytesLeft());

            m_MessageHandler->ChannelDisconnect("reliable buffer is full");
        }

        return false;
    }

    return buf.WriteBits(msg.GetData(), dataBits);
}

//-----------------------------------------------------------------------------
// Purpose: finds a registered net message by type
// Input: type - 
// Output: net message pointer on success, NULL otherwise
//-----------------------------------------------------------------------------
INetMessage* CNetChan::FindMessage(const int type)
{
    const int numtypes = m_NetMessages.Count();

    for (int i = 0; i < numtypes; i++)
    {
        INetMessage* const message = m_NetMessages[i];

        if (message->GetType() == type)
            return message;
    }

    return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: registers a net message
// Input: *msg
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool CNetChan::RegisterMessage(INetMessage* msg)
{
    Assert(msg);

    if (FindMessage(msg->GetType()))
    {
        Assert(0); // Duplicate registration!
        return false;
    }

    m_NetMessages.AddToTail(msg);
    msg->SetNetChannel(this);

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: free's the receive data fragment list
//-----------------------------------------------------------------------------
void CNetChan::FreeReceiveList()
{
    m_ReceiveList.blockSize = NULL;
    m_ReceiveList.transferSize = NULL;
    if (m_ReceiveList.buffer)
    {
        delete m_ReceiveList.buffer;
        m_ReceiveList.buffer = nullptr;
    }
}

//-----------------------------------------------------------------------------
// Purpose: check if there is still data in the reliable waiting buffers
//-----------------------------------------------------------------------------
bool CNetChan::HasPendingReliableData(void)
{
	return (m_StreamReliable.GetNumBitsWritten() > 0)
		|| (m_WaitingList.Count() > 0);
}

void BridgeLatency_OnClientReport(CClient* pClient, unsigned nRttMs)
{
	if (!pClient || nRttMs > 2000)
		return;
	CClientExtended* const pExt = pClient->GetClientExtended();
	if (!pExt)
		return;
	pExt->SetBridgeRttReport(static_cast<float>(nRttMs) * 0.001f, Plat_FloatTime());
}

static CClient* BridgeLatency_ClientForChan(CNetChan* pChan)
{
	if (!pChan || !g_pServer)
		return nullptr;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		CClient* const pClient = g_pServer->GetClient(i);
		if (pClient && pClient->GetNetChan() == pChan)
			return pClient;
	}
	return nullptr;
}

__int64 CNetChan::_FlowUpdate(CNetChan* pChan, int flow, int addBytes)
{
	const __int64 result = CNetChan__FlowUpdate
		? CNetChan__FlowUpdate(pChan, flow, addBytes) : 0;

	if (!bridge_dedi_latency_refine.GetBool() || flow != FLOW_OUTGOING || !pChan)
		return result;

	CClient* const pClient = BridgeLatency_ClientForChan(pChan);
	if (!pClient || !pClient->IsHumanPlayer())
		return result;

	CClientExtended* const pExt = pClient->GetClientExtended();
	if (!pExt || !pExt->HasBridgeRttReport())
		return result;

	const float flAge = static_cast<float>(Plat_FloatTime() - pExt->GetBridgeRttTime());
	if (flAge < 0.0f || flAge > 2.0f)
		return result;

	const float flNative = pChan->GetAvgLatency(FLOW_OUTGOING);
	if (!(flNative > 0.0f) || !std::isfinite(flNative))
		return result;

	float flTick = 0.05f;
	if (g_pCommonHostState && g_pCommonHostState->interval_per_tick > 0.0f)
		flTick = g_pCommonHostState->interval_per_tick;

	const float flReported = pExt->GetBridgeRtt();
	const float flLo = Max(0.0f, flNative - flTick);
	const float flHi = flNative + flTick;
	float flPublished = flReported;
	if (flPublished < flLo)
		flPublished = flLo;
	if (flPublished > flHi)
		flPublished = flHi;
	if (flPublished > 2.0f)
		flPublished = 2.0f;

	pChan->m_DataFlow[FLOW_OUTGOING].avglatency = flPublished;

	if (bridge_latency_refine_diag.GetBool())
	{
		static uint32_t s_nRefineLogs = 0;
		if (++s_nRefineLogs <= 8 || (s_nRefineLogs % 256) == 0)
			Warning(eDLL_T::ENGINE,
				"[RTT-REFINE] native=%.4f report=%.4f pub=%.4f age=%.3f\n",
				flNative, flReported, flPublished, flAge);
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////////
void VNetChan::Detour(const bool bAttach) const
{
	DetourSetup(&CNetChan__Shutdown, &CNetChan::_Shutdown, bAttach);
	DetourSetup(&CNetChan__FlowNewPacket, &CNetChan::_FlowNewPacket, bAttach);
	DetourSetup(&CNetChan__ProcessMessages, &CNetChan::_ProcessMessages, bAttach);
	DetourSetup(&CNetChan__SendDatagram, &CNetChan::_SendDatagram, bAttach); // [DEDI-SEND] wire capture
	DetourSetup(&CNetChan__ProcessPacketHeader, &CNetChan::_ProcessPacketHeader, bAttach); // [DEDI-RECV] reliable-ack probe
	if (CNetChan__FlowUpdate)
		DetourSetup(&CNetChan__FlowUpdate, &CNetChan::_FlowUpdate, bAttach);
	else if (bAttach)
		Warning(eDLL_T::ENGINE, "[BRIDGE-RTT] CNetChan::FlowUpdate unresolved -- latency stays tick-quantized\n");
}
#endif // CLIENT_DLL
