#ifndef CORE_NET_OBSERVER_H
#define CORE_NET_OBSERVER_H

//=============================================================================//
//
// Purpose: Connection diagnostics for the S21 client
//
//=============================================================================//

// Install every observer hook. Safe to call once per process lifetime; does
// nothing if called twice. Returns the number of hooks successfully attached
// so callers can log a summary.
int NetObserver_Install();

// Shared verbosity: 0=quiet, 1=per-packet SDK_Log, 2=extra-chatty.
extern int g_sdkObserveNet;
#define SDK_OBSERVE_NET_GE(lvl) (g_sdkObserveNet >= (lvl))

// Push client net convars to match the dedi. Encryption force-off is gated
// by bridge_force_encryption_off (must be set before any connect).
void NetObserver_PushClientConvars();

// Engine game socket is AF_INET6 IPV6_V6ONLY=1; bridge uses a parallel IPv4 socket.
// Called from NET_ReceiveDatagram after original returns false; true if injected.
struct netpacket_s;
bool S21Bridge_PollReceive(int iSocket, netpacket_s* pInpacket);

// Translate net message type IDs between S3 and S21 enumerations.
int S21Bridge_TranslateNetMsgType(int msgType, bool bIncoming);

// S3-format netchannel compatibility hooks.
// When the bridge is active, these replace the S21 engine's ProcessPacket
// and SendDatagram to speak S3 netchannel format on the wire.
class CNetChan;
struct netpacket_s;
class bf_write;
void S21Bridge_Hook_ProcessPacket(CNetChan* pChan, netpacket_s* pPacket);
int  S21Bridge_Hook_SendDatagram(CNetChan* pChan, bf_write* pMsg);

// The live channel, captured from ProcessPacket. This is the only channel the
// client product has: VClientState is not registered here, so g_pClientState
// stays null and anything reading m_NetChannel finds nothing.
CNetChan* S21Bridge_GetActiveChan(void);
// CNetChan::FlowUpdate writes per-flow avg stats. Bridge overwrites with real values.
__int64 S21Bridge_Hook_FlowUpdate(CNetChan* pChan, int flow);
// Performance-HUD SPING in ms (engine NET_GetSPing). 0 when not FULL / offline.
int S21Bridge_GetConnectionPingMs(void);
// Ms since last successful C2S flush / engine frame. -1 if never stamped.
double S21BridgeDiag_MsSinceC2SFlush(void);
double S21BridgeDiag_MsSinceEngineFrame(void);
// CNetChan::SendNetMsg. SDK CNetChan layout is S3 and does not match S21.
char S21Bridge_Hook_SendNetMsg(void* pChan, void* pMsg, char bForceReliable, char bVoice);

//-----------------------------------------------------------------------------
// IDetour wrappers for net-bridge diagnostic hooks. Bodies in engine/net_bridge TUs.
//-----------------------------------------------------------------------------
#ifndef DEDICATED
#include <cstdint>
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/tier0_iface.h"
#include "thirdparty/detours/include/idetour.h"

// Decode/recvtable/prop-apply diagnostics. Detour relocates the verbatim
// install blocks (resolve+attach+patches) out of NetObserver_Install.
class VNetDecodeDiagS21 : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

// Frame/clock/snapshot/ziprail diagnostics.
class VNetFrameDiagS21 : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

// Connection observer: cbuf / cmd / connect / net-send / signon / winsock probes.
class VNetObserverDiagS21 : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

inline void(__fastcall* v_SR_ExecuteCallQueue)(int64_t, int) = nullptr;
inline void(__fastcall* s_pFakeRecreate)(void*) = nullptr;
inline void(__fastcall* s_pNotifyScriptOfNewEntities)(void) = nullptr;
inline void*** s_ppNotifyScriptEnts = nullptr;
inline uint8_t* s_pAcceptEntsForScript = nullptr;
inline float(__fastcall* s_pDeathFieldRadiusForTime)(float flTime, int nRingIndex) = nullptr;
inline uint8_t** s_ppClientWorld = nullptr;

// Releases S->C ScriptRemote calls on the same snapshot tick as the native queue.
class VScriptRemoteS2CGate : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("SR_ExecuteCallQueue", v_SR_ExecuteCallQueue);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

// [SCRIPT-CREATE] Detours C_BaseEntity::NotifyScriptOfNewEntities for gated
// queue diagnostics; resolves FakeRecreate for post-apply re-arm. Bodies in
// engine/client/net_bridge_install.cpp.
class VScriptCreateNotifyS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("C_BaseEntity::NotifyScriptOfNewEntities", s_pNotifyScriptOfNewEntities);
		LogFunAdr("C_BaseEntity::FakeRecreate", s_pFakeRecreate);
		LogVarAdr("g_NotifyScriptEnts", s_ppNotifyScriptEnts);
		LogVarAdr("g_acceptEntsForScript", s_pAcceptEntsForScript);
		LogFunAdr("DeathField_GetRadiusForTime", s_pDeathFieldRadiusForTime);
		LogVarAdr("g_pClientWorld", s_ppClientWorld);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
#endif // !DEDICATED

#endif // CORE_NET_OBSERVER_H
