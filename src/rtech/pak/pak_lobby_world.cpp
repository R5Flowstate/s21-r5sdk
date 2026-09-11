//=============================================================================//
//
// Purpose: Dest loadscreen at challenge; dest world paks after one Present.
//
//=============================================================================//

#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier0/threadtools.h"
#include "tier1/cvar.h"
#include "pak_lobby_world.h"
#include "pak_census.h"
#include "rpak_observe.h"

ConVar bridge_direct_map_load("bridge_direct_map_load", "1", FCVAR_RELEASE,
	"1 = first connect uses dest 0x04 loadscreen and defers dest world paks until the loadscreen has presented. 0 = S21 default (lobby world first).");

typedef void(__fastcall* PFN_SetupMapPaks)(int mode, const char* pszMap);
typedef char*(__fastcall* PFN_SetupLoadScreenPaks)(const char* pszMap);
typedef int(__fastcall* PFN_PakWaitAsync)(int handle, void* finishCallback);

static PFN_SetupMapPaks v_SetupMapPaks = nullptr;
static PFN_SetupLoadScreenPaks v_SetupLoadScreenPaks = nullptr;
static PFN_PakWaitAsync v_PakWaitAsync = nullptr;

static const char** s_ppszMapRpakSlot = nullptr;
static const char** s_ppszMapPermSlot = nullptr;
static const char** s_ppszMapTempSlot = nullptr;
static const char s_szEmpty[] = "";
static volatile LONG s_nStripLog = 0;
static volatile LONG s_nDeferLog = 0;
static volatile LONG s_nTornLog = 0;
static volatile LONG s_worldPaksReleased = 0;
static volatile LONG s_needWorldPaks = 0;
static volatile LONG s_pollSawPending = 0;
static char s_pendingWorldMap[64] = {};

extern bool s_connAcceptDone;
extern bool s_bridgeActive;
extern void S21Bridge_DrainSocketToQueue(void);

static bool IsBareMapName(const char* psz)
{
	if (!psz || !psz[0])
		return false;
	size_t n = 0;
	for (; psz[n]; ++n)
	{
		const unsigned char c = static_cast<unsigned char>(psz[n]);
		if (n >= 63)
			return false;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	if (n >= 5 && _strnicmp(psz, "root_", 5) == 0)
		return false;
	return n >= 3;
}

static bool IsLobbyMapName(const char* psz)
{
	return psz && (_strnicmp(psz, "mp_lobby", 8) == 0) &&
		(psz[8] == '\0' || psz[8] == '_' || psz[8] == '.');
}

bool PakLobby_DirectMapLoad(void)
{
	return bridge_direct_map_load.GetBool();
}

void PakLobby_OnSessionReset(void)
{
	s_pendingWorldMap[0] = '\0';
	InterlockedExchange(&s_needWorldPaks, 0);
	InterlockedExchange(&s_pollSawPending, 0);
	InterlockedExchange(&s_worldPaksReleased, 0);
	InterlockedExchange(&s_nStripLog, 0);
	InterlockedExchange(&s_nDeferLog, 0);
}

static int StripLobbySlot(const char** ppszSlot, const char* pszPak)
{
	if (!ppszSlot || !*ppszSlot || _stricmp(*ppszSlot, pszPak) != 0)
		return 0;
	if (Pak_IsSlotNameLive_S21(pszPak))
		return -1;
	*ppszSlot = s_szEmpty;
	return 1;
}

static void StripLobbyWorldSlots(void)
{
	if (!bridge_direct_map_load.GetBool())
		return;

	// Blanking a slot whose pak is already resident unloads the lobby world
	// under the renderer (PVS jobs keep walking its portal data). Only stop a
	// pak that has not been picked up yet.
	int nMap = StripLobbySlot(s_ppszMapRpakSlot, "mp_lobby.rpak");
	int nPerm = StripLobbySlot(s_ppszMapPermSlot, "mp_lobby_client_perm.rpak");
	int nTemp = StripLobbySlot(s_ppszMapTempSlot, "mp_lobby_client_temp.rpak");
	if (!nMap && !nPerm && !nTemp)
		return;

	const LONG n = InterlockedIncrement(&s_nStripLog);
	if (n <= 12)
	{
		Msg(eDLL_T::RTECH, "[LOBBY-PAK] lobby slot(s) map=%d perm=%d temp=%d (1=stripped, -1=kept, pak resident)\n",
			nMap, nPerm, nTemp);
	}
}

static void RememberPendingWorldMap(const char* pszMap)
{
	if (!pszMap || !IsBareMapName(pszMap) || IsLobbyMapName(pszMap))
		return;
	strncpy(s_pendingWorldMap, pszMap, sizeof(s_pendingWorldMap) - 1);
	s_pendingWorldMap[sizeof(s_pendingWorldMap) - 1] = '\0';
	InterlockedExchange(&s_needWorldPaks, 1);
}

static bool ShouldDeferWorldPaks(int mode, const char* pszMap)
{
	if (!bridge_direct_map_load.GetBool())
		return false;
	if (s_worldPaksReleased)
		return false;
	if (mode == 1)
		return false;
	if (!IsBareMapName(pszMap) || IsLobbyMapName(pszMap))
		return false;
	return true;
}

static const char* DestMapFallback(void)
{
	extern char g_bridgeConnMapName[64];
	if (!bridge_direct_map_load.GetBool())
		return nullptr;
	if (!IsBareMapName(g_bridgeConnMapName) || IsLobbyMapName(g_bridgeConnMapName))
		return nullptr;
	return g_bridgeConnMapName;
}

static void Hook_SetupMapPaks(int mode, const char* pszMap)
{
	if (mode != 1)
	{
		char szReason[96];
		snprintf(szReason, sizeof(szReason), "SetupMapPaks mode=%d map='%s'",
			mode, IsBareMapName(pszMap) ? pszMap : "?");
		Pak_CensusLog(szReason, false);
	}
	// mode!=1 sprintf's dest paks from pszMap. Garbage here becomes
	// "<junk>_client_temp.rpak" and pops ErrorDialog.
	if (mode != 1 && !IsBareMapName(pszMap))
	{
		const char* const pszDest = DestMapFallback();
		if (pszDest)
		{
			pszMap = pszDest;
			Msg(eDLL_T::RTECH,
				"[LOBBY-PAK] SetupMapPaks mode=%d map invalid -- using dest '%s'\n",
				mode, pszMap);
		}
		else
		{
			Warning(eDLL_T::RTECH,
				"[LOBBY-PAK] SetupMapPaks mode=%d map invalid -- lobby setup only\n",
				mode);
			mode = 1;
			pszMap = nullptr;
		}
	}
	// mode=1 restamps slots with mp_lobby.rpak. After dest is released that
	// unloads the dest map pak.
	if (s_worldPaksReleased && mode == 1)
	{
		static volatile LONG s_nSkipLobbyStamp = 0;
		const LONG n = InterlockedIncrement(&s_nSkipLobbyStamp);
		if (n <= 8)
		{
			Msg(eDLL_T::RTECH,
				"[LOBBY-PAK] skip lobby restamp (dest world already released)\n");
		}
		return;
	}
	if (ShouldDeferWorldPaks(mode, pszMap))
	{
		RememberPendingWorldMap(pszMap);
		v_SetupMapPaks(1, nullptr);
		StripLobbyWorldSlots();
		const LONG n = InterlockedIncrement(&s_nDeferLog);
		if (n <= 12)
		{
			Msg(eDLL_T::RTECH, "[LOBBY-PAK] defer dest world paks map='%s'\n",
				s_pendingWorldMap[0] ? s_pendingWorldMap : "?");
		}
		return;
	}
	v_SetupMapPaks(mode, pszMap);
	if (mode == 1 || !IsLobbyMapName(pszMap))
		StripLobbyWorldSlots();
}

static char* Hook_SetupLoadScreenPaks(const char* pszMap)
{
	if (!IsBareMapName(pszMap))
	{
		const char* const pszDest = DestMapFallback();
		if (pszDest)
		{
			pszMap = pszDest;
			Msg(eDLL_T::RTECH,
				"[LOBBY-PAK] loadscreen map invalid -- using dest '%s'\n",
				pszMap);
		}
		else
		{
			Warning(eDLL_T::RTECH,
				"[LOBBY-PAK] loadscreen map invalid -- using mp_lobby\n");
			pszMap = "mp_lobby";
		}
	}
	char* const pResult = v_SetupLoadScreenPaks(pszMap);
	if (!IsLobbyMapName(pszMap))
		StripLobbyWorldSlots();
	return pResult;
}

static thread_local void* s_pakWaitCb = nullptr;
static thread_local int s_pakWaitDepth = 0;

bool PakLobby_InPakWait(void)
{
	return s_pakWaitDepth > 0;
}

static void PakWaitPump(void)
{
	S21Bridge_DrainSocketToQueue();
	if (s_pakWaitCb)
		reinterpret_cast<void(*)(void)>(s_pakWaitCb)();
}

static int Hook_PakWaitAsync(int handle, void* finishCallback)
{
	if (!v_PakWaitAsync)
		return 0;
	if (!s_bridgeActive || !ThreadInMainThread() || !finishCallback)
		return v_PakWaitAsync(handle, finishCallback);

	void* const pPrev = s_pakWaitCb;
	s_pakWaitCb = finishCallback;
	++s_pakWaitDepth;
	const int nResult = v_PakWaitAsync(handle, reinterpret_cast<void*>(&PakWaitPump));
	--s_pakWaitDepth;
	s_pakWaitCb = pPrev;
	return nResult;
}

void PakLobby_OnPollReceive(void)
{
	if (!bridge_direct_map_load.GetBool() || !v_SetupMapPaks)
		return;
	if (!s_needWorldPaks || !s_pendingWorldMap[0] || !s_connAcceptDone)
	{
		InterlockedExchange(&s_pollSawPending, 0);
		return;
	}
	if (InterlockedCompareExchange(&s_pollSawPending, 1, 0) == 0)
		return;

	char szMap[64];
	strncpy(szMap, s_pendingWorldMap, sizeof(szMap) - 1);
	szMap[sizeof(szMap) - 1] = '\0';
	if (!IsBareMapName(szMap) || IsLobbyMapName(szMap))
	{
		s_pendingWorldMap[0] = '\0';
		InterlockedExchange(&s_needWorldPaks, 0);
		InterlockedExchange(&s_pollSawPending, 0);
		const LONG n = InterlockedIncrement(&s_nTornLog);
		if (n <= 8)
			Warning(eDLL_T::RTECH, "[LOBBY-PAK] torn pending map -- drop\n");
		return;
	}
	s_pendingWorldMap[0] = '\0';
	InterlockedExchange(&s_needWorldPaks, 0);
	InterlockedExchange(&s_pollSawPending, 0);
	InterlockedExchange(&s_worldPaksReleased, 1);

	Msg(eDLL_T::RTECH, "[LOBBY-PAK] kick dest world paks map='%s'\n", szMap);
	S21Bridge_DrainSocketToQueue();
	v_SetupMapPaks(3, szMap);
	S21Bridge_DrainSocketToQueue();
	if (!IsLobbyMapName(szMap))
		StripLobbyWorldSlots();
}

void VPakLobbyWorldS21::GetAdr(void) const
{
	LogFunAdr("Pak_SetupMapPaks", v_SetupMapPaks);
	LogFunAdr("Pak_SetupLoadScreenPaks", v_SetupLoadScreenPaks);
	LogFunAdr("Pak_WaitAsync", v_PakWaitAsync);
	LogVarAdr("MapRpakSlot", s_ppszMapRpakSlot);
	LogVarAdr("MapPermSlot", s_ppszMapPermSlot);
	LogVarAdr("MapTempSlot", s_ppszMapTempSlot);
}

void VPakLobbyWorldS21::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"85 C9 0F 84 ?? ?? ?? ?? 56 48 83 EC ?? 48 89 5C 24")
		.GetPtr(v_SetupMapPaks);

	Module_FindPattern(g_GameDll,
		"48 81 EC ?? ?? ?? ?? 0F B6 05 ?? ?? ?? ?? 4C 8D 05")
		.GetPtr(v_SetupLoadScreenPaks);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 89 6C 24 ?? 8B C1")
		.GetPtr(v_PakWaitAsync);

	if (!v_SetupMapPaks)
	{
		Warning(eDLL_T::RTECH, "[LOBBY-PAK] Pak_SetupMapPaks pattern unresolved\n");
		return;
	}
	if (!v_PakWaitAsync)
		Warning(eDLL_T::RTECH, "[LOBBY-PAK] Pak_WaitAsync pattern unresolved -- handshake drain during pak wait disabled\n");

	// Three consecutive default-slot stores: map.rpak, client_perm, client_temp.
	const CMemory stamp = CMemory(v_SetupMapPaks).FindPattern(
		"48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
		"48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
		"48 8D 05 ?? ?? ?? ?? 48 89 05");
	if (!stamp.IsValid() || stamp.GetValue<uint8_t>() != 0x48)
	{
		Warning(eDLL_T::RTECH, "[LOBBY-PAK] perm/temp stamp bytes mismatch -- slots not resolved\n");
		return;
	}

	s_ppszMapRpakSlot = stamp.Offset(0x07).ResolveRelativeAddress(0x03, 0x07).RCast<const char**>();
	s_ppszMapPermSlot = stamp.Offset(0x15).ResolveRelativeAddress(0x03, 0x07).RCast<const char**>();
	s_ppszMapTempSlot = stamp.Offset(0x23).ResolveRelativeAddress(0x03, 0x07).RCast<const char**>();
	if (!s_ppszMapRpakSlot || !s_ppszMapPermSlot || !s_ppszMapTempSlot)
	{
		Warning(eDLL_T::RTECH, "[LOBBY-PAK] map/perm/temp slot pointers unresolved\n");
		return;
	}

	Msg(eDLL_T::RTECH, "[LOBBY-PAK] slots resolved map=%p perm=%p temp=%p\n",
		(void*)s_ppszMapRpakSlot, (void*)s_ppszMapPermSlot, (void*)s_ppszMapTempSlot);
}

void VPakLobbyWorldS21::Detour(const bool bAttach) const
{
	if (v_SetupMapPaks)
		DetourSetup(&v_SetupMapPaks, &Hook_SetupMapPaks, bAttach);
	if (v_SetupLoadScreenPaks)
		DetourSetup(&v_SetupLoadScreenPaks, &Hook_SetupLoadScreenPaks, bAttach);
	if (v_PakWaitAsync)
		DetourSetup(&v_PakWaitAsync, &Hook_PakWaitAsync, bAttach);
}
