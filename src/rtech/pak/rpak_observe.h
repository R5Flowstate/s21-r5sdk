//=============================================================================//
//
// Purpose: S21 RPAK load observability. Pak_InitAsyncLoad(pakHnd); slot = pakHnd & 0x1FF, stride 352.
//
//=============================================================================//

#ifndef RPAK_OBSERVE_S21_H
#define RPAK_OBSERVE_S21_H

#include "thirdparty/detours/include/idetour.h"
#include "engine/client/net_bridge_addrs.h"

inline bool S21Pak_IsDx12Exe()
{
	char exePath[MAX_PATH]{};
	const DWORD len = GetModuleFileNameA(NULL, exePath, sizeof(exePath));
	if (len == 0 || len >= sizeof(exePath))
		return false;

	return V_stristr(exePath, "dx12") != nullptr;
}

inline uintptr_t S21Pak_AssetGuidHashBase(void)
{
	return NetObs_Sym(NetObsSym_t::PakAssetGuidHash);
}

inline uintptr_t S21Pak_SlotBase(void)
{
	return NetObs_Sym(NetObsSym_t::PakSlotBase);
}

// Native Pak_InitAsyncLoad. Observability only; bypassing breaks the pak state machine.
inline char (__fastcall *v_Pak_InitAsyncLoad_S21)(__int64 pakHnd) = nullptr;

// Engine fatal logger: severity==5 is dumpless TerminateProcess. Pak-load fatals are neutered.
inline void (__fastcall *v_FatalErrorLogger_S21)(char severity, const char* fmt, __int64 vargs) = nullptr;

// Consistency check: a2+0 guid, +56 start, +64 count. a1+22672 guidDesc array, a1+22584 page ptrs.
inline __int64 (__fastcall *v_PakConsistencyCheck_S21)(__int64 a1, __int64 a2) = nullptr;

// Full 5-arg enqueue: a narrow signature drops r8/r9/[rsp+0x28]. Allocator slot is a value, not an address.
inline int (__fastcall *v_Pak_RequestLoadByName_S21)(
	const char* name,
	char        priority,
	uintptr_t   allocatorSlot,
	char        c4,
	char        trackFeature) = nullptr;

// 5-arg invoke for SDK enqueue without the script-binding wrapper.
typedef int (__fastcall *PFN_Pak_RequestLoadByName_Full_S21)(
	const char* name,
	char priority,
	uintptr_t allocatorSlot,
	char c4,
	char trackFeature);

// UnloadAsyncByHandle(handle, wait). wait=1 matches the script-native call site.
inline void (__fastcall *v_Pak_UnloadAsyncByHandle_S21)(
	unsigned int handle, int wait) = nullptr;

inline void (__fastcall *v_Pak_PreCache_UnloadAll_S21)(void) = nullptr;

// Script-native wrappers: full 5-arg including the global allocator pointer.
inline int (__fastcall *v_ClientPakFile_RequestAsyncLoad_S21)(
	const char* name, unsigned int trackFeature) = nullptr;
inline int (__fastcall *v_ClientPakFile_Unload_S21)(
	unsigned int handle) = nullptr;

// Slot table: stride 352, 512 slots (handle & 0x1FF). +0x00 handle, +0x04 status, +0x18 name.
static constexpr size_t    kS21_PakSlotStride    = 352;
static constexpr size_t    kS21_PakSlotCount     = 512;
static constexpr size_t    kS21_PakSlot_Handle   = 0x00;
static constexpr size_t    kS21_PakSlot_Status   = 0x04;
static constexpr size_t    kS21_PakSlot_Name     = 0x18;
// Same-guid tiebreak: Pak_TrackAsset keeps the current owner unless the new pak's
// slot priority is strictly greater. Never written for a plain rpak (stays 0).
static constexpr size_t    kS21_PakSlot_Priority = 0xD8;

const char* Pak_StatusToString_S21(int status);
void Pak_DumpGuidChain_S21(unsigned __int64 guid, const char* tag);
void* Pak_FindInstalledHead_S21(unsigned __int64 guid);
uintptr_t Pak_GetSlotBase_S21();
// True while any slot holds this pak name with a non-zero status (loading, loaded or unloading).
bool Pak_IsSlotNameLive_S21(const char* pszName);

// Pattern then RVA. Used by pakstate load/unload/swap callbacks.
int  Pak_RequestLoadByName_S21Resolve();
int  Pak_UnloadAsyncByHandle_S21Resolve();
bool Pak_IsAllowedLoadName_S21(const char* name);
int  ClientPakFile_RequestAsyncLoad_S21Resolve();
int  ClientPakFile_Unload_S21Resolve();

// Allocator-slot address used as a3 to Pak_RequestLoadByName. Worker later dereferences it.
uintptr_t Pak_GetGlobalAllocatorSlot_S21();


///////////////////////////////////////////////////////////////////////////////
class VRPakObserveS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Pak_InitAsyncLoad_S21",       v_Pak_InitAsyncLoad_S21);
		LogFunAdr("Pak_RequestLoadByName_S21",   v_Pak_RequestLoadByName_S21);
		LogFunAdr("Pak_UnloadAsyncByHandle_S21", v_Pak_UnloadAsyncByHandle_S21);
		LogFunAdr("Pak_PreCache_UnloadAll_S21",  v_Pak_PreCache_UnloadAll_S21);
		LogFunAdr("FatalErrorLogger_S21",        v_FatalErrorLogger_S21);
		LogFunAdr("PakConsistencyCheck_S21",     v_PakConsistencyCheck_S21);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll,
			"89 4C 24 08 55 48 8D AC 24 20 DB FF FF B8 E0 25 00 00")
			.GetPtr(v_Pak_InitAsyncLoad_S21);
		// Pak_RequestLoadByName
		Module_FindPattern(g_GameDll,
			"48 89 74 24 10 48 89 7C 24 18 4C 89 64 24 20 41 55 41 56 "
			"41 57 48 83 EC 20 45 0F B6 F9 49 8B F0 44 0F B6 F2 4C 8B E1")
			.GetPtr(v_Pak_RequestLoadByName_S21);
		// Pak_UnloadAsyncByHandle: `41 BE FF 01 00 00` is the 0x1FF slot mask.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
			"41 56 48 83 EC 20 0F B7 C1 41 BE FF 01 00 00")
			.GetPtr(v_Pak_UnloadAsyncByHandle_S21);
		// Pak_PreCache_UnloadAll: clears the precache flag byte then nulls four group pointers.
		Module_FindPattern(g_GameDll,
			"48 83 EC 28 48 8D 05 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 48 89 05 ?? ?? ?? ?? "
			"48 89 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 89 05")
			.GetPtr(v_Pak_PreCache_UnloadAll_S21);

		// Fatal logger. B8 40 27 00 00 = mov eax, 0x2740 (10000-byte Source buffer).

		Module_FindPattern(g_GameDll,
			"48 89 5C 24 10 48 89 6C 24 18 56 B8 40 27 00 00 E8 ?? ?? ?? ?? "
			"48 2B E0 0F B6 D9 49 8B F0 48 8B EA 80 FB 08")
			.GetPtr(v_FatalErrorLogger_S21);

		// Pak guidDesc consistency check.

		Module_FindPattern(g_GameDll,
			"48 89 54 24 10 48 89 4C 24 08 55 57 41 54 41 55 41 57 48 83 EC 40 "
			"8B 81 2C 58 00 00 48 8B FA 25 FF 01 00 00 33 ED")
			.GetPtr(v_PakConsistencyCheck_S21);
	}
	virtual void GetVar(void) const {}
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // RPAK_OBSERVE_S21_H
