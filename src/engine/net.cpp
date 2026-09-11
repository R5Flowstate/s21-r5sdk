#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: Net system utilities
//
//=============================================================================//

#include "core/stdafx.h"
#include "engine/client/net_observer.h"
#include "engine/net.h"
#include <intrin.h>
#ifndef _TOOLS
#include "tier1/cvar.h"
#include "tier2/cryptutils.h"
#include "mathlib/color.h"
#include "net.h"
#include "net_chan.h"
#endif // !_TOOLS

#ifndef _TOOLS
static const void* s_lzssCnetchanRet = nullptr;

static void NET_GetKey_f()
{
	NET_PrintKey();
}
static void NET_SetKey_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	NET_SetKey(args.Arg(1));
}
static void NET_GenerateKey_f()
{
	NET_GenerateKey();
}

// The engine wipes the key store back to its own constant during net init, so
// anything installed before 'CHostState::Setup' is discarded.
static bool s_bKeyStoreReady = false;

void NET_UseRandomKeyChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData)
{
	if (ConVar* pConVarRef = g_pCVar->FindVar(pConVar->GetName()))
	{
		if (!s_bKeyStoreReady)
			return; // CHostState::Setup installs the boot key once net init is done.

		if ((pOldString && atoi(pOldString) != 0) == pConVarRef->GetBool())
			return; // Same value; the string form may differ ("0 " vs "0").

		if (pConVarRef->GetBool())
			NET_GenerateKey();
		else
			NET_SetKey(DEFAULT_NET_ENCRYPTION_KEY);
	}
}

ConVar net_useRandomKey("net_useRandomKey", "1", FCVAR_RELEASE, "Use random AES encryption key for game packets.", false, 0.f, false, 0.f, &NET_UseRandomKeyChanged_f, nullptr);
ConVar sv_netkey("sv_netkey", "", FCVAR_RELEASE, "Specifies a custom base64 AES-128 net key to use instead of generating a random one on startup.");

static ConVar net_tracePayload("net_tracePayload", "0", FCVAR_DEVELOPMENTONLY, "Log the payload of the send/recv datagram to a file on the disk.");
static ConVar net_encryptionEnable("net_encryptionEnable", "1", FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED, "Use AES encryption on game packets.");

static ConCommand net_getkey("net_getkey", NET_GetKey_f, "Gets the installed base64 net key", FCVAR_RELEASE);
static ConCommand net_setkey("net_setkey", NET_SetKey_f, "Sets user specified base64 net key", FCVAR_RELEASE);
static ConCommand net_generatekey("net_generatekey", NET_GenerateKey_f, "Generates and sets a random base64 net key", FCVAR_RELEASE);

//-----------------------------------------------------------------------------
// Purpose: hook and log the receive datagram
// Input: iSocket - 
// *pInpacket - 
// bEncrypted - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NET_ReceiveDatagram(int iSocket, netpacket_s* pInpacket, bool bEncrypted)
{
	// S21->S3 Bridge: inject pending bridge packets first.
	if (S21Bridge_PollReceive(iSocket, pInpacket))
		return true;

	// When the bridge is active, disable decryption so our unencrypted
	// OOB packets pass through to the handler without being dropped.
	extern bool S21Bridge_IsActive();
	const bool decryptPacket = S21Bridge_IsActive() ? false : bEncrypted;
	return v_NET_ReceiveDatagram(iSocket, pInpacket, decryptPacket);
}

//-----------------------------------------------------------------------------
// Purpose: hook and log the send datagram
// Input: s - 
// *pPayload - 
// iLenght - 
// *pAdr - 
// bEncrypt - 
// Output: outgoing sequence number for this packet
//-----------------------------------------------------------------------------
int NET_SendDatagram(SOCKET s, void* pPayload, int iLenght, netadr_t* pAdr, bool bEncrypt)
{
	const bool encryptPacket = (bEncrypt && net_encryptionEnable.GetBool());
	const int result = v_NET_SendDatagram(s, pPayload, iLenght, pAdr, encryptPacket);

	if (result && net_tracePayload.GetBool())
	{
		// Log transmitted packet data.
		HexDump("[+] NET_SendDatagram ", "net_trace", pPayload, size_t(iLenght));
	}

	return result;
}

//-----------------------------------------------------------------------------
// Purpose: compresses the input buffer into the output buffer
// Input: *dest - 
// *destLen - 
// *source - 
// sourceLen - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NET_BufferToBufferCompress(uint8_t* const dest, size_t* const destLen, uint8_t* const source, const size_t sourceLen)
{
	CLZSS lzss;
	uint32_t compLen = (uint32_t)sourceLen;

	if (!lzss.CompressNoAlloc(source, (uint32_t)sourceLen, dest, &compLen))
	{
		memcpy(dest, source, sourceLen);

		*destLen = sourceLen;
		return false;
	}

	*destLen = compLen;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: decompresses the input buffer into the output buffer
// Input: *source - 
// &sourceLen - 
// *dest - 
// destLen - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
unsigned int NET_BufferToBufferDecompress(uint8_t* const source, size_t& sourceLen, uint8_t* const dest, const size_t destLen)
{
	Assert(source);
	Assert(sourceLen);

	CLZSS lzss;

	if (lzss.IsCompressed(source))
	{
		return lzss.SafeUncompress(source, dest, (unsigned int)destLen, (unsigned int)sourceLen);
	}

	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: safely decompresses the input buffer into the output buffer
// Input: *lzss - 
// *pInput - 
// *pOutput - 
// unBufSize - 
// Output: total decompressed bytes
//-----------------------------------------------------------------------------
unsigned int NET_BufferToBufferDecompress_LZSS(CLZSS* lzss, unsigned char* pInput, unsigned char* pOutput, unsigned int unBufSize)
{
	unsigned int nInput = 0;
	if (s_lzssCnetchanRet && _ReturnAddress() == s_lzssCnetchanRet && lzss)
		nInput = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(lzss));
	else
		nInput = LZSS_BoundInputBytes(pInput, unBufSize);
	if (!nInput)
		return 0;
	CLZSS decoder;
	return decoder.SafeUncompress(pInput, pOutput, unBufSize, nInput);
}

//-----------------------------------------------------------------------------
// Purpose: configures the network system
//-----------------------------------------------------------------------------
void NET_Config()
{
	v_NET_Config();
	g_pNetAdr->SetPort(htons(u_short(hostport->GetInt())));
}

void NET_EnableKeyInstall()
{
	s_bKeyStoreReady = true;
}

//-----------------------------------------------------------------------------
// Purpose: prints the currently installed encryption key
//-----------------------------------------------------------------------------
void NET_PrintKey()
{
	Msg(eDLL_T::ENGINE, "Installed NetKey: %s'%s%s%s'\n",
		g_svReset.c_str(), g_svGreyB.c_str(), g_pNetKey->GetBase64NetKey(), g_svReset.c_str());
}

//-----------------------------------------------------------------------------
// Purpose: sets the user specified encryption key
// Input: svNetKey - 
//-----------------------------------------------------------------------------
void NET_SetKey(const string& svNetKey)
{
	// v_NET_SetKey / g_pNetKey are unresolved here; encryption is already off.
	if (!v_NET_SetKey || !g_pNetKey)
	{
		static bool s_bWarned = false;

		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::ENGINE,
				"NET_SetKey unavailable on this build; packet encryption is off, key ignored\n");
		}

		return;
	}

	string svTokenizedKey;

	if (svNetKey.size() == AES_128_B64_ENCODED_SIZE &&
		IsValidBase64(svNetKey, &svTokenizedKey)) // Results are tokenized by 'IsValidBase64'.
	{
		const char* const pszInstalled = g_pNetKey->GetBase64NetKey();
		if (pszInstalled && svTokenizedKey == pszInstalled)
			return;

		v_NET_SetKey(g_pNetKey, svTokenizedKey.c_str());
		NET_PrintKey();
	}
	else
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "AES-128 key not encoded or invalid\n");
	}
}

//-----------------------------------------------------------------------------
// Purpose: calculates and sets the encryption key
//-----------------------------------------------------------------------------
void NET_GenerateKey()
{
	if (!net_useRandomKey.GetBool())
	{
		net_useRandomKey.SetValue(1);
		return; // Change callback will handle this.
	}

	uint8_t keyBuf[AES_128_KEY_SIZE];
	const char* errorMsg = nullptr;

	if (!Plat_GenerateRandom(keyBuf, sizeof(keyBuf), errorMsg))
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "%s\n", errorMsg);
		return;
	}

	NET_SetKey(Base64Encode(string(reinterpret_cast<char*>(&keyBuf), AES_128_KEY_SIZE)));
}

//-----------------------------------------------------------------------------
// Purpose: hook and log the client's signonstate to the console
// Input: *fmt - 
//			... - 
//-----------------------------------------------------------------------------
void NET_PrintFunc(const char* fmt, ...)
{
	const static eDLL_T context = eDLL_T::CLIENT;

	string result;

	va_list args;
	va_start(args, fmt);
	result = FormatV(fmt, args);
	va_end(args);

	Msg(context, result.back() == '\n' ? "%s" : "%s\n", result.c_str());
}

//-----------------------------------------------------------------------------
// Purpose: disconnect the client and shutdown netchannel
// Input: *pClient - 
// nIndex - 
// *szReason - 
// bBadRep - 
// bRemoveNow - 
//-----------------------------------------------------------------------------
void NET_RemoveChannel(CClient* pClient, int nIndex, const char* szReason, uint8_t bBadRep, bool bRemoveNow)
{
}

//-----------------------------------------------------------------------------
// Purpose: reads the net message type from buffer
// Input: &outType - 
// &buffer - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NET_ReadMessageType(int* outType, bf_read* buffer)
{
	*outType = buffer->ReadUBitLong(NETMSG_TYPE_BITS);
	return !buffer->IsOverflowed();
}

//-----------------------------------------------------------------------------
// Purpose: checks whether the provided address is the local server.
// Input: &netAdr - 
// Output: true if equal, false otherwise
//-----------------------------------------------------------------------------
bool NET_IsRemoteLocal(const CNetAdr& netAdr)
{
	NOTE_UNUSED(netAdr);
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: checks whether the provided address is an actual loopback address
// (127.0.0.0/8 or ::1). Distinct from CNetAdr::IsLoopback (NA_LOOPBACK only).
// Input: &netAdr -
// Output: true if the address is a loopback address, false otherwise
//-----------------------------------------------------------------------------
bool NET_IsAddressLoopback(const CNetAdr& netAdr)
{
	if (netAdr.IsLoopback())
		return true;

	if (netAdr.GetType() != netadrtype_t::NA_IP)
		return false;

	const in6_addr* const pIP = netAdr.GetIP();

	if (IN6_ADDR_EQUAL(pIP, &in6addr_loopback))
		return true;

	// IPv4-mapped loopback (::ffff:127.0.0.0/8); CNetAdr::SetFromString formats
	// every IPv4 address this way ('::FFFF:<ip>').
	if (IN6_IS_ADDR_V4MAPPED(pIP) && pIP->s6_addr[12] == 127)
		return true;

	return false;
}

#endif // !_TOOLS

//-----------------------------------------------------------------------------
// Purpose: returns the WSA error code
//-----------------------------------------------------------------------------
const char* NET_ErrorString(int iCode)
{
	switch (iCode)
	{
		case WSAEINTR                   : return "WSAEINTR";
		case WSAEBADF                   : return "WSAEBADF";
		case WSAEACCES                  : return "WSAEACCES";
		case WSAEFAULT                  : return "WSAEFAULT";
		case WSAEINVAL                  : return "WSAEINVAL";
		case WSAEMFILE                  : return "WSAEMFILE";
		case WSAEWOULDBLOCK             : return "WSAEWOULDBLOCK";
		case WSAEINPROGRESS             : return "WSAEINPROGRESS";
		case WSAEALREADY                : return "WSAEALREADY";
		case WSAENOTSOCK                : return "WSAENOTSOCK";
		case WSAEDESTADDRREQ            : return "WSAEDESTADDRREQ";
		case WSAEMSGSIZE                : return "WSAEMSGSIZE";
		case WSAEPROTOTYPE              : return "WSAEPROTOTYPE";
		case WSAENOPROTOOPT             : return "WSAENOPROTOOPT";
		case WSAEPROTONOSUPPORT         : return "WSAEPROTONOSUPPORT";
		case WSAESOCKTNOSUPPORT         : return "WSAESOCKTNOSUPPORT";
		case WSAEOPNOTSUPP              : return "WSAEOPNOTSUPP";
		case WSAEPFNOSUPPORT            : return "WSAEPFNOSUPPORT";
		case WSAEAFNOSUPPORT            : return "WSAEAFNOSUPPORT";
		case WSAEADDRINUSE              : return "WSAEADDRINUSE";
		case WSAEADDRNOTAVAIL           : return "WSAEADDRNOTAVAIL";
		case WSAENETDOWN                : return "WSAENETDOWN";
		case WSAENETUNREACH             : return "WSAENETUNREACH";
		case WSAENETRESET               : return "WSAENETRESET";
		case WSAECONNABORTED            : return "WSAECONNABORTED";
		case WSAECONNRESET              : return "WSAECONNRESET";
		case WSAENOBUFS                 : return "WSAENOBUFS";
		case WSAEISCONN                 : return "WSAEISCONN";
		case WSAENOTCONN                : return "WSAENOTCONN";
		case WSAESHUTDOWN               : return "WSAESHUTDOWN";
		case WSAETOOMANYREFS            : return "WSAETOOMANYREFS";
		case WSAETIMEDOUT               : return "WSAETIMEDOUT";
		case WSAECONNREFUSED            : return "WSAECONNREFUSED";
		case WSAELOOP                   : return "WSAELOOP";
		case WSAENAMETOOLONG            : return "WSAENAMETOOLONG";
		case WSAEHOSTDOWN               : return "WSAEHOSTDOWN";
		case WSAEHOSTUNREACH            : return "WSAEHOSTUNREACH";
		case WSAENOTEMPTY               : return "WSAENOTEMPTY";
		case WSAEPROCLIM                : return "WSAEPROCLIM";
		case WSAEUSERS                  : return "WSAEUSERS";
		case WSAEDQUOT                  : return "WSAEDQUOT";
		case WSAESTALE                  : return "WSAESTALE";
		case WSAEREMOTE                 : return "WSAEREMOTE";
		case WSASYSNOTREADY             : return "WSASYSNOTREADY";
		case WSAVERNOTSUPPORTED         : return "WSAVERNOTSUPPORTED";
		case WSANOTINITIALISED          : return "WSANOTINITIALISED";
		case WSAEDISCON                 : return "WSAEDISCON";
		case WSAENOMORE                 : return "WSAENOMORE";
		case WSAECANCELLED              : return "WSAECANCELLED";
		case WSAEINVALIDPROCTABLE       : return "WSAEINVALIDPROCTABLE";
		case WSAEINVALIDPROVIDER        : return "WSAEINVALIDPROVIDER";
		case WSAEPROVIDERFAILEDINIT     : return "WSAEPROVIDERFAILEDINIT";
		case WSASYSCALLFAILURE          : return "WSASYSCALLFAILURE";
		case WSASERVICE_NOT_FOUND       : return "WSASERVICE_NOT_FOUND";
		case WSATYPE_NOT_FOUND          : return "WSATYPE_NOT_FOUND";
		case WSA_E_NO_MORE              : return "WSA_E_NO_MORE";
		case WSA_E_CANCELLED            : return "WSA_E_CANCELLED";
		case WSAEREFUSED                : return "WSAEREFUSED";
		case WSAHOST_NOT_FOUND          : return "WSAHOST_NOT_FOUND";
		case WSATRY_AGAIN               : return "WSATRY_AGAIN";
		case WSANO_RECOVERY             : return "WSANO_RECOVERY";
		case WSANO_DATA                 : return "WSANO_DATA";
		case WSA_QOS_RECEIVERS          : return "WSA_QOS_RECEIVERS";
		case WSA_QOS_SENDERS            : return "WSA_QOS_SENDERS";
		case WSA_QOS_NO_SENDERS         : return "WSA_QOS_NO_SENDERS";
		case WSA_QOS_NO_RECEIVERS       : return "WSA_QOS_NO_RECEIVERS";
		case WSA_QOS_REQUEST_CONFIRMED  : return "WSA_QOS_REQUEST_CONFIRMED";
		case WSA_QOS_ADMISSION_FAILURE  : return "WSA_QOS_ADMISSION_FAILURE";
		case WSA_QOS_POLICY_FAILURE     : return "WSA_QOS_POLICY_FAILURE";
		case WSA_QOS_BAD_STYLE          : return "WSA_QOS_BAD_STYLE";
		case WSA_QOS_BAD_OBJECT         : return "WSA_QOS_BAD_OBJECT";
		case WSA_QOS_TRAFFIC_CTRL_ERROR : return "WSA_QOS_TRAFFIC_CTRL_ERROR";
		case WSA_QOS_GENERIC_ERROR      : return "WSA_QOS_GENERIC_ERROR";
		case WSA_QOS_ESERVICETYPE       : return "WSA_QOS_ESERVICETYPE";
		case WSA_QOS_EFLOWSPEC          : return "WSA_QOS_EFLOWSPEC";
		case WSA_QOS_EPROVSPECBUF       : return "WSA_QOS_EPROVSPECBUF";
		case WSA_QOS_EFILTERSTYLE       : return "WSA_QOS_EFILTERSTYLE";
		case WSA_QOS_EFILTERTYPE        : return "WSA_QOS_EFILTERTYPE";
		case WSA_QOS_EFILTERCOUNT       : return "WSA_QOS_EFILTERCOUNT";
		case WSA_QOS_EOBJLENGTH         : return "WSA_QOS_EOBJLENGTH";
		case WSA_QOS_EFLOWCOUNT         : return "WSA_QOS_EFLOWCOUNT";
		case WSA_QOS_EUNKOWNPSOBJ       : return "WSA_QOS_EUNKNOWNPSOBJ";
		case WSA_QOS_EPOLICYOBJ         : return "WSA_QOS_EPOLICYOBJ";
		case WSA_QOS_EFLOWDESC          : return "WSA_QOS_EFLOWDESC";
		case WSA_QOS_EPSFLOWSPEC        : return "WSA_QOS_EPSFLOWSPEC";
		case WSA_QOS_EPSFILTERSPEC      : return "WSA_QOS_EPSFILTERSPEC";
		case WSA_QOS_ESDMODEOBJ         : return "WSA_QOS_ESDMODEOBJ";
		case WSA_QOS_ESHAPERATEOBJ      : return "WSA_QOS_ESHAPERATEOBJ";
		case WSA_QOS_RESERVED_PETYPE    : return "WSA_QOS_RESERVED_PETYPE";
		case WSA_SECURE_HOST_NOT_FOUND  : return "WSA_SECURE_HOST_NOT_FOUND";
		case WSA_IPSEC_NAME_POLICY_ERROR: return "WSA_IPSEC_NAME_POLICY_ERROR";
	default                    : return "UNKNOWN_ERROR";
	}
}

#ifndef _TOOLS
///////////////////////////////////////////////////////////////////////////////
void VNet::Detour(const bool bAttach) const
{
	// Guard: only detour functions whose S3/S21 patterns matched.
	// On S21, only NET_ReceiveDatagram is guaranteed to resolve.
	if (v_NET_Config)
		DetourSetup(&v_NET_Config, &NET_Config, bAttach);
	if (v_NET_ReceiveDatagram)
		DetourSetup(&v_NET_ReceiveDatagram, &NET_ReceiveDatagram, bAttach);
	if (v_NET_SendDatagram)
		DetourSetup(&v_NET_SendDatagram, &NET_SendDatagram, bAttach);
	if (v_NET_BufferToBufferCompress)
		DetourSetup(&v_NET_BufferToBufferCompress, &NET_BufferToBufferCompress, bAttach);
	if (v_NET_BufferToBufferDecompress_LZSS)
		DetourSetup(&v_NET_BufferToBufferDecompress_LZSS, &NET_BufferToBufferDecompress_LZSS, bAttach);
	if (v_NET_PrintFunc)
		DetourSetup(&v_NET_PrintFunc, &NET_PrintFunc, bAttach);
}

///////////////////////////////////////////////////////////////////////////////
netadr_t* g_pNetAdr = nullptr;
netkey_t* g_pNetKey = nullptr;

double* g_pNetTime = nullptr;
#endif // !_TOOLS
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: Net system utilities
//
//=============================================================================//

#include "core/stdafx.h"
#include "engine/net.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include <intrin.h>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef _TOOLS
#include "tier1/cvar.h"
#include "tier2/cryptutils.h"
#include "mathlib/color.h"
#include "net.h"
#include "net_chan.h"
#include "server/server.h"
#include "client/client.h"
#include "engine/shared/s21_bridge_compat.h"
#include "engine/host_state.h"
#endif // !_TOOLS

#ifndef _TOOLS

static const void* s_lzssCnetchanRet = nullptr;
static uintptr_t s_lzssNative = 0;

static bool Lzss_WriteBytes(void* addr, const void* data, size_t len)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt))
		return false;
	memcpy(addr, data, len);
	DWORD restored = 0;
	VirtualProtect(addr, len, oldProt, &restored);
	FlushInstructionCache(GetCurrentProcess(), addr, len);
	return true;
}

static void Lzss_InstallCnetchanInputPatch(void)
{
	if (s_lzssCnetchanRet || !s_lzssNative)
		return;

	CMemory site = Module_FindPattern(g_GameDll,
		"8B 4A 04 48 3B CE 77 ?? 44 8B CE 4C 8B C0 E8 ?? ?? ?? ??");
	if (!site)
	{
		Warning(eDLL_T::ENGINE, "[LZSS] CNetChan input-size site not found\n");
		return;
	}

	uint8_t* const p = reinterpret_cast<uint8_t*>(site.GetPtr());
	const int32_t rel = *reinterpret_cast<int32_t*>(p + 15);
	const uintptr_t callSite = reinterpret_cast<uintptr_t>(p + 14);
	const uintptr_t target = callSite + 5 + static_cast<intptr_t>(rel);
	if (target != s_lzssNative)
	{
		Warning(eDLL_T::ENGINE, "[LZSS] CNetChan call target mismatch (got %p expect %p)\n",
			reinterpret_cast<void*>(target),
			reinterpret_cast<void*>(v_NET_BufferToBufferDecompress_LZSS));
		return;
	}

	const uint8_t patch[3] = { 0x8B, 0xCD, 0x90 };
	if (!Lzss_WriteBytes(p, patch, sizeof(patch)))
	{
		Warning(eDLL_T::ENGINE, "[LZSS] CNetChan input-size patch failed @ %p\n", p);
		return;
	}

	s_lzssCnetchanRet = p + 19;
	Msg(eDLL_T::ENGINE, "[LZSS] CNetChan input-size patch @ %p (ecx=inputLen)\n", p);
}

static void NET_GetKey_f()
{
	NET_PrintKey();
}
static void NET_SetKey_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	NET_SetKey(args.Arg(1));
}
static void NET_GenerateKey_f()
{
	NET_GenerateKey();
}

// The engine wipes the key store back to its own constant during net init, so
// anything installed before 'CHostState::Setup' is discarded.
static bool s_bKeyStoreReady = false;

void NET_UseRandomKeyChanged_f(IConVar* pConVar, const char* pOldString, float flOldValue, ChangeUserData_t pUserData)
{
	if (ConVar* pConVarRef = g_pCVar->FindVar(pConVar->GetName()))
	{
		if (!s_bKeyStoreReady)
			return; // CHostState::Setup installs the boot key once net init is done.

		if ((pOldString && atoi(pOldString) != 0) == pConVarRef->GetBool())
			return; // Same value; the string form may differ ("0 " vs "0").

		if (pConVarRef->GetBool())
			NET_GenerateKey();
		else
			NET_SetKey(DEFAULT_NET_ENCRYPTION_KEY);
	}
}

ConVar net_useRandomKey("net_useRandomKey", "1", FCVAR_RELEASE | FCVAR_CHEAT, "Use random AES encryption key for game packets.", false, 0.f, false, 0.f, &NET_UseRandomKeyChanged_f, nullptr);
ConVar sv_netkey("sv_netkey", "", FCVAR_RELEASE | FCVAR_CHEAT, "Specifies a custom base64 AES-128 net key to use instead of generating a random one on startup.");

static ConVar net_tracePayload("net_tracePayload", "0", FCVAR_RELEASE | FCVAR_CHEAT, "Log the payload of the send/recv datagram to a file on the disk.");
static ConVar net_encryptionEnable("net_encryptionEnable", "1", FCVAR_RELEASE | FCVAR_REPLICATED, "Use AES encryption on game packets.");

// Hex-dump outgoing NET_SendDatagram (post-split, pre-AES). Default off.
static ConVar net_tx_diag("net_tx_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[DEDI-TX-1S] per-second datagram count, bytes, largest and failures. 0 = off.");
static ConVar net_dumpWire("net_dumpWire", "0", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT,
    "Log [DEDI-WIRE] hex of each outgoing NET_SendDatagram (0 = disabled).");

static ConCommand net_getkey("net_getkey", NET_GetKey_f, "Gets the installed base64 net key", FCVAR_RELEASE | FCVAR_CHEAT);
static ConCommand net_setkey("net_setkey", NET_SetKey_f, "Sets user specified base64 net key", FCVAR_RELEASE | FCVAR_CHEAT);
static ConCommand net_generatekey("net_generatekey", NET_GenerateKey_f, "Generates and sets a random base64 net key", FCVAR_RELEASE | FCVAR_CHEAT);

//-----------------------------------------------------------------------------
// Purpose: hook and log the receive datagram
// Input: iSocket - 
// *pInpacket - 
// bEncrypted - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NET_ReceiveDatagram(int iSocket, netpacket_s* pInpacket, bool bEncrypted)
{
	const bool decryptPacket = (bEncrypted && net_encryptionEnable.GetBool());
	const bool result = v_NET_ReceiveDatagram(iSocket, pInpacket, decryptPacket);

	// Drop leftover OOB delta-tick (FF FF FF FF BD 7C <int32>) from older clients.
	if (result && pInpacket && pInpacket->pData && pInpacket->wiresize >= 10)
	{
		const uint8_t* p = reinterpret_cast<const uint8_t*>(pInpacket->pData);
		if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF
			&& p[4] == 0xBD && p[5] == 0x7C)
		{
			pInpacket->wiresize = 0;
			pInpacket->size     = 0;
			return result;
		}
	}

	if (result && net_tracePayload.GetBool())
	{
		// Log received packet data.
		HexDump("[+] NET_ReceiveDatagram ", "net_trace",
			pInpacket->pData, size_t(pInpacket->wiresize));
	}

	return result;
}

//-----------------------------------------------------------------------------
// Purpose: hook and log the send datagram
// Input: s - 
// *pPayload - 
// iLenght - 
// *pAdr - 
// bEncrypt - 
// Output: outgoing sequence number for this packet
//-----------------------------------------------------------------------------
int NET_SendDatagram(SOCKET s, void* pPayload, int iLenght, netadr_t* pAdr, bool bEncrypt)
{
	const bool encryptPacket = (bEncrypt && net_encryptionEnable.GetBool());

	// S3 S2C_CHALLENGE is ffffffff 49 + a ~38-byte body, not the 9-byte
	// (ffffffff 49 u32) stub. Append a 0x00 + bare map + 0x00 suffix so the
	// S21 client can 0x04-rewrite the dest map instead of mp_lobby.
	if (iLenght >= 9 && iLenght <= 128 && pPayload && g_pHostState && g_pHostState->m_levelName[0])
	{
		const uint8_t* pIn = reinterpret_cast<const uint8_t*>(pPayload);
		if (pIn[0] == 0xFF && pIn[1] == 0xFF && pIn[2] == 0xFF && pIn[3] == 0xFF && pIn[4] == 0x49)
		{
			const char* psz = g_pHostState->m_levelName;
			for (const char* q = psz; *q; ++q)
			{
				if (*q == '/' || *q == '\\')
					psz = q + 1;
			}
			char szBare[64];
			size_t n = 0;
			for (; psz[n] && psz[n] != '.' && n < 63; ++n)
			{
				const unsigned char c = static_cast<unsigned char>(psz[n]);
				if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_'))
				{
					n = 0;
					break;
				}
				szBare[n] = psz[n];
			}
			szBare[n] = '\0';
			if (n >= 3)
			{
				uint8_t ext[192];
				const int nExt = iLenght + 1 + static_cast<int>(n) + 1;
				if (nExt <= static_cast<int>(sizeof(ext)))
				{
					memcpy(ext, pIn, static_cast<size_t>(iLenght));
					ext[iLenght] = 0;
					memcpy(ext + iLenght + 1, szBare, n + 1);
					return v_NET_SendDatagram(s, ext, nExt, pAdr, encryptPacket);
				}
			}
		}
	}

	// Dump plaintext before AES. Split: flags at +20 on fragment 0; else flags at +8.
	if (net_dumpWire.GetBool() && pPayload && iLenght >= 12)
	{
		const uint8_t* p = reinterpret_cast<const uint8_t*>(pPayload);
		char hx[3 * 32 + 1] = {};
		const int n = (iLenght < 32) ? iLenght : 32;
		for (int i = 0; i < n; ++i)
			snprintf(hx + i * 3, sizeof(hx) - (i * 3), "%02X ", p[i]);

		static long long s_w = 0;
		if (++s_w <= 300 || (s_w % 100) == 0)
			Warning(eDLL_T::ENGINE,
				"[DEDI-WIRE] len=%d enc=%d bytes[0..%d]: %s\n",
				iLenght, encryptPacket ? 1 : 0, n, hx);
	}

	const int result = v_NET_SendDatagram(s, pPayload, iLenght, pAdr, encryptPacket);

	if (result && net_tracePayload.GetBool())
	{
		// Log transmitted packet data.
		HexDump("[+] NET_SendDatagram ", "net_trace", pPayload, size_t(iLenght));
	}

	if (net_tx_diag.GetBool())
	{
		static ULONGLONG s_txMs = 0;
		static int s_txCount = 0, s_txBytes = 0, s_txMax = 0, s_txFail = 0;
		++s_txCount;
		s_txBytes += iLenght;
		if (iLenght > s_txMax)
			s_txMax = iLenght;
		if (!result)
			++s_txFail;
		const ULONGLONG nowMs = GetTickCount64();
		if (s_txMs == 0)
			s_txMs = nowMs;
		else if (nowMs - s_txMs >= 1000)
		{
			Warning(eDLL_T::ENGINE, "[DEDI-TX-1S] dgrams=%d bytes=%d max=%d fail=%d\n",
				s_txCount, s_txBytes, s_txMax, s_txFail);
			s_txMs = nowMs;
			s_txCount = s_txBytes = s_txMax = s_txFail = 0;
		}
	}

	return result;
}

//-----------------------------------------------------------------------------
// Purpose: compresses the input buffer into the output buffer
// Input: *dest - 
// *destLen - 
// *source - 
// sourceLen - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NET_BufferToBufferCompress(uint8_t* const dest, size_t* const destLen, uint8_t* const source, const size_t sourceLen)
{
	CLZSS lzss;
	uint32_t compLen = (uint32_t)sourceLen;

	if (!lzss.CompressNoAlloc(source, (uint32_t)sourceLen, dest, &compLen))
	{
		memcpy(dest, source, sourceLen);

		*destLen = sourceLen;
		return false;
	}

	*destLen = compLen;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: decompresses the input buffer into the output buffer
// Input: *source - 
// &sourceLen - 
// *dest - 
// destLen - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
unsigned int NET_BufferToBufferDecompress(uint8_t* const source, size_t& sourceLen, uint8_t* const dest, const size_t destLen)
{
	Assert(source);
	Assert(sourceLen);

	CLZSS lzss;

	if (lzss.IsCompressed(source))
	{
		return lzss.SafeUncompress(source, dest, (unsigned int)destLen, (unsigned int)sourceLen);
	}

	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: safely decompresses the input buffer into the output buffer
// Input: *lzss - 
// *pInput - 
// *pOutput - 
// unBufSize - 
// Output: total decompressed bytes
//-----------------------------------------------------------------------------
unsigned int NET_BufferToBufferDecompress_LZSS(CLZSS* lzss, unsigned char* pInput, unsigned char* pOutput, unsigned int unBufSize)
{
	unsigned int nInput = 0;
	if (s_lzssCnetchanRet && _ReturnAddress() == s_lzssCnetchanRet && lzss)
		nInput = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(lzss));
	else
		nInput = LZSS_BoundInputBytes(pInput, unBufSize);
	if (!nInput)
		return 0;
	CLZSS decoder;
	return decoder.SafeUncompress(pInput, pOutput, unBufSize, nInput);
}

//-----------------------------------------------------------------------------
// Purpose: configures the network system
//-----------------------------------------------------------------------------
void NET_Config()
{
	v_NET_Config();
	g_pNetAdr->SetPort(htons(u_short(hostport->GetInt())));
}

void NET_EnableKeyInstall()
{
	s_bKeyStoreReady = true;
}

//-----------------------------------------------------------------------------
// Purpose: prints the currently installed encryption key
//-----------------------------------------------------------------------------
void NET_PrintKey()
{
	Msg(eDLL_T::ENGINE, "Installed NetKey: %s'%s%s%s'\n",
		g_svReset.c_str(), g_svGreyB.c_str(), g_pNetKey->GetBase64NetKey(), g_svReset.c_str());
}

//-----------------------------------------------------------------------------
// Purpose: sets the user specified encryption key
// Input: svNetKey - 
//-----------------------------------------------------------------------------
void NET_SetKey(const string& svNetKey)
{
	string svTokenizedKey;

	if (svNetKey.size() == AES_128_B64_ENCODED_SIZE &&
		IsValidBase64(svNetKey, &svTokenizedKey)) // Results are tokenized by 'IsValidBase64'.
	{
		const char* const pszInstalled = g_pNetKey->GetBase64NetKey();
		if (pszInstalled && svTokenizedKey == pszInstalled)
			return;

		v_NET_SetKey(g_pNetKey, svTokenizedKey.c_str());
		NET_PrintKey();
	}
	else
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "AES-128 key not encoded or invalid\n");
	}
}

//-----------------------------------------------------------------------------
// Purpose: calculates and sets the encryption key
//-----------------------------------------------------------------------------
void NET_GenerateKey()
{
	if (!net_useRandomKey.GetBool())
	{
		net_useRandomKey.SetValue(1);
		return; // Change callback will handle this.
	}

	uint8_t keyBuf[AES_128_KEY_SIZE];
	const char* errorMsg = nullptr;

	if (!Plat_GenerateRandom(keyBuf, sizeof(keyBuf), errorMsg))
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "%s\n", errorMsg);
		return;
	}

	NET_SetKey(Base64Encode(string(reinterpret_cast<char*>(&keyBuf), AES_128_KEY_SIZE)));
}

//-----------------------------------------------------------------------------
// Purpose: hook and log the client's signonstate to the console
// Input: *fmt - 
//			... - 
//-----------------------------------------------------------------------------
void NET_PrintFunc(const char* fmt, ...)
{
#ifndef DEDICATED
	const static eDLL_T context = eDLL_T::CLIENT;
#else // !DEDICATED
	const static eDLL_T context = eDLL_T::SERVER;
#endif

	string result;

	va_list args;
	va_start(args, fmt);
	result = FormatV(fmt, args);
	va_end(args);

	Msg(context, result.back() == '\n' ? "%s" : "%s\n", result.c_str());
}

//-----------------------------------------------------------------------------
// Purpose: disconnect the client and shutdown netchannel
// Input: *pClient - 
// nIndex - 
// *szReason - 
// bBadRep - 
// bRemoveNow - 
//-----------------------------------------------------------------------------
void NET_RemoveChannel(CClient* pClient, int nIndex, const char* szReason, uint8_t bBadRep, bool bRemoveNow)
{
	if (!pClient || std::strlen(szReason) == NULL || !pClient->GetNetChan())
	{
		return;
	}

	pClient->GetNetChan()->Shutdown(szReason, bBadRep, bRemoveNow); // Shutdown NetChannel.
	pClient->Clear();                                               // Reset CClient slot.
}

//-----------------------------------------------------------------------------
// Purpose: reads the net message type from buffer
// Input: &outType - 
// &buffer - 
// Output: true on success, false otherwise
//-----------------------------------------------------------------------------
bool NET_ReadMessageType(int* outType, bf_read* buffer)
{
	*outType = buffer->ReadUBitLong(NETMSG_TYPE_BITS);
	return !buffer->IsOverflowed();
}

//-----------------------------------------------------------------------------
// Purpose: checks whether the provided address is the local server.
// Input: &netAdr - 
// Output: true if equal, false otherwise
//-----------------------------------------------------------------------------
bool NET_IsRemoteLocal(const CNetAdr& netAdr)
{
	if (!g_pNetAdr)
		return false;
	return (g_pNetAdr->ComparePort(netAdr) && g_pNetAdr->CompareAdr(netAdr));
}

//-----------------------------------------------------------------------------
// Purpose: checks whether the provided address is an actual loopback address
// (127.0.0.0/8 or ::1). Distinct from CNetAdr::IsLoopback (NA_LOOPBACK only).
// Input: &netAdr -
// Output: true if the address is a loopback address, false otherwise
//-----------------------------------------------------------------------------
bool NET_IsAddressLoopback(const CNetAdr& netAdr)
{
	if (netAdr.IsLoopback())
		return true;

	if (netAdr.GetType() != netadrtype_t::NA_IP)
		return false;

	const in6_addr* const pIP = netAdr.GetIP();

	if (IN6_ADDR_EQUAL(pIP, &in6addr_loopback))
		return true;

	// IPv4-mapped loopback (::ffff:127.0.0.0/8); CNetAdr::SetFromString formats
	// every IPv4 address this way ('::FFFF:<ip>').
	if (IN6_IS_ADDR_V4MAPPED(pIP) && pIP->s6_addr[12] == 127)
		return true;

	return false;
}

#endif // !_TOOLS

//-----------------------------------------------------------------------------
// Purpose: returns the WSA error code
//-----------------------------------------------------------------------------
const char* NET_ErrorString(int iCode)
{
	switch (iCode)
	{
		case WSAEINTR                   : return "WSAEINTR";
		case WSAEBADF                   : return "WSAEBADF";
		case WSAEACCES                  : return "WSAEACCES";
		case WSAEFAULT                  : return "WSAEFAULT";
		case WSAEINVAL                  : return "WSAEINVAL";
		case WSAEMFILE                  : return "WSAEMFILE";
		case WSAEWOULDBLOCK             : return "WSAEWOULDBLOCK";
		case WSAEINPROGRESS             : return "WSAEINPROGRESS";
		case WSAEALREADY                : return "WSAEALREADY";
		case WSAENOTSOCK                : return "WSAENOTSOCK";
		case WSAEDESTADDRREQ            : return "WSAEDESTADDRREQ";
		case WSAEMSGSIZE                : return "WSAEMSGSIZE";
		case WSAEPROTOTYPE              : return "WSAEPROTOTYPE";
		case WSAENOPROTOOPT             : return "WSAENOPROTOOPT";
		case WSAEPROTONOSUPPORT         : return "WSAEPROTONOSUPPORT";
		case WSAESOCKTNOSUPPORT         : return "WSAESOCKTNOSUPPORT";
		case WSAEOPNOTSUPP              : return "WSAEOPNOTSUPP";
		case WSAEPFNOSUPPORT            : return "WSAEPFNOSUPPORT";
		case WSAEAFNOSUPPORT            : return "WSAEAFNOSUPPORT";
		case WSAEADDRINUSE              : return "WSAEADDRINUSE";
		case WSAEADDRNOTAVAIL           : return "WSAEADDRNOTAVAIL";
		case WSAENETDOWN                : return "WSAENETDOWN";
		case WSAENETUNREACH             : return "WSAENETUNREACH";
		case WSAENETRESET               : return "WSAENETRESET";
		case WSAECONNABORTED            : return "WSAECONNABORTED";
		case WSAECONNRESET              : return "WSAECONNRESET";
		case WSAENOBUFS                 : return "WSAENOBUFS";
		case WSAEISCONN                 : return "WSAEISCONN";
		case WSAENOTCONN                : return "WSAENOTCONN";
		case WSAESHUTDOWN               : return "WSAESHUTDOWN";
		case WSAETOOMANYREFS            : return "WSAETOOMANYREFS";
		case WSAETIMEDOUT               : return "WSAETIMEDOUT";
		case WSAECONNREFUSED            : return "WSAECONNREFUSED";
		case WSAELOOP                   : return "WSAELOOP";
		case WSAENAMETOOLONG            : return "WSAENAMETOOLONG";
		case WSAEHOSTDOWN               : return "WSAEHOSTDOWN";
		case WSAEHOSTUNREACH            : return "WSAEHOSTUNREACH";
		case WSAENOTEMPTY               : return "WSAENOTEMPTY";
		case WSAEPROCLIM                : return "WSAEPROCLIM";
		case WSAEUSERS                  : return "WSAEUSERS";
		case WSAEDQUOT                  : return "WSAEDQUOT";
		case WSAESTALE                  : return "WSAESTALE";
		case WSAEREMOTE                 : return "WSAEREMOTE";
		case WSASYSNOTREADY             : return "WSASYSNOTREADY";
		case WSAVERNOTSUPPORTED         : return "WSAVERNOTSUPPORTED";
		case WSANOTINITIALISED          : return "WSANOTINITIALISED";
		case WSAEDISCON                 : return "WSAEDISCON";
		case WSAENOMORE                 : return "WSAENOMORE";
		case WSAECANCELLED              : return "WSAECANCELLED";
		case WSAEINVALIDPROCTABLE       : return "WSAEINVALIDPROCTABLE";
		case WSAEINVALIDPROVIDER        : return "WSAEINVALIDPROVIDER";
		case WSAEPROVIDERFAILEDINIT     : return "WSAEPROVIDERFAILEDINIT";
		case WSASYSCALLFAILURE          : return "WSASYSCALLFAILURE";
		case WSASERVICE_NOT_FOUND       : return "WSASERVICE_NOT_FOUND";
		case WSATYPE_NOT_FOUND          : return "WSATYPE_NOT_FOUND";
		case WSA_E_NO_MORE              : return "WSA_E_NO_MORE";
		case WSA_E_CANCELLED            : return "WSA_E_CANCELLED";
		case WSAEREFUSED                : return "WSAEREFUSED";
		case WSAHOST_NOT_FOUND          : return "WSAHOST_NOT_FOUND";
		case WSATRY_AGAIN               : return "WSATRY_AGAIN";
		case WSANO_RECOVERY             : return "WSANO_RECOVERY";
		case WSANO_DATA                 : return "WSANO_DATA";
		case WSA_QOS_RECEIVERS          : return "WSA_QOS_RECEIVERS";
		case WSA_QOS_SENDERS            : return "WSA_QOS_SENDERS";
		case WSA_QOS_NO_SENDERS         : return "WSA_QOS_NO_SENDERS";
		case WSA_QOS_NO_RECEIVERS       : return "WSA_QOS_NO_RECEIVERS";
		case WSA_QOS_REQUEST_CONFIRMED  : return "WSA_QOS_REQUEST_CONFIRMED";
		case WSA_QOS_ADMISSION_FAILURE  : return "WSA_QOS_ADMISSION_FAILURE";
		case WSA_QOS_POLICY_FAILURE     : return "WSA_QOS_POLICY_FAILURE";
		case WSA_QOS_BAD_STYLE          : return "WSA_QOS_BAD_STYLE";
		case WSA_QOS_BAD_OBJECT         : return "WSA_QOS_BAD_OBJECT";
		case WSA_QOS_TRAFFIC_CTRL_ERROR : return "WSA_QOS_TRAFFIC_CTRL_ERROR";
		case WSA_QOS_GENERIC_ERROR      : return "WSA_QOS_GENERIC_ERROR";
		case WSA_QOS_ESERVICETYPE       : return "WSA_QOS_ESERVICETYPE";
		case WSA_QOS_EFLOWSPEC          : return "WSA_QOS_EFLOWSPEC";
		case WSA_QOS_EPROVSPECBUF       : return "WSA_QOS_EPROVSPECBUF";
		case WSA_QOS_EFILTERSTYLE       : return "WSA_QOS_EFILTERSTYLE";
		case WSA_QOS_EFILTERTYPE        : return "WSA_QOS_EFILTERTYPE";
		case WSA_QOS_EFILTERCOUNT       : return "WSA_QOS_EFILTERCOUNT";
		case WSA_QOS_EOBJLENGTH         : return "WSA_QOS_EOBJLENGTH";
		case WSA_QOS_EFLOWCOUNT         : return "WSA_QOS_EFLOWCOUNT";
		case WSA_QOS_EUNKOWNPSOBJ       : return "WSA_QOS_EUNKNOWNPSOBJ";
		case WSA_QOS_EPOLICYOBJ         : return "WSA_QOS_EPOLICYOBJ";
		case WSA_QOS_EFLOWDESC          : return "WSA_QOS_EFLOWDESC";
		case WSA_QOS_EPSFLOWSPEC        : return "WSA_QOS_EPSFLOWSPEC";
		case WSA_QOS_EPSFILTERSPEC      : return "WSA_QOS_EPSFILTERSPEC";
		case WSA_QOS_ESDMODEOBJ         : return "WSA_QOS_ESDMODEOBJ";
		case WSA_QOS_ESHAPERATEOBJ      : return "WSA_QOS_ESHAPERATEOBJ";
		case WSA_QOS_RESERVED_PETYPE    : return "WSA_QOS_RESERVED_PETYPE";
		case WSA_SECURE_HOST_NOT_FOUND  : return "WSA_SECURE_HOST_NOT_FOUND";
		case WSA_IPSEC_NAME_POLICY_ERROR: return "WSA_IPSEC_NAME_POLICY_ERROR";
	default                    : return "UNKNOWN_ERROR";
	}
}

#ifndef _TOOLS
///////////////////////////////////////////////////////////////////////////////
void VNet::Detour(const bool bAttach) const
{
	DetourSetup(&v_NET_Config, &NET_Config, bAttach);
	DetourSetup(&v_NET_ReceiveDatagram, &NET_ReceiveDatagram, bAttach);
	DetourSetup(&v_NET_SendDatagram, &NET_SendDatagram, bAttach);

	if (bAttach && v_NET_BufferToBufferDecompress_LZSS)
		s_lzssNative = reinterpret_cast<uintptr_t>(v_NET_BufferToBufferDecompress_LZSS);

	DetourSetup(&v_NET_BufferToBufferCompress, &NET_BufferToBufferCompress, bAttach);
	DetourSetup(&v_NET_BufferToBufferDecompress_LZSS, &NET_BufferToBufferDecompress_LZSS, bAttach);
	DetourSetup(&v_NET_PrintFunc, &NET_PrintFunc, bAttach);
	if (bAttach)
		Lzss_InstallCnetchanInputPatch();
}

///////////////////////////////////////////////////////////////////////////////
netadr_t* g_pNetAdr = nullptr;
netkey_t* g_pNetKey = nullptr;

double* g_pNetTime = nullptr;
#endif // !_TOOLS
#endif // CLIENT_DLL
