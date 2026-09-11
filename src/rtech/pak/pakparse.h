#if defined(CLIENT_DLL)
#ifndef RTECH_PAKPARSE_H
#define RTECH_PAKPARSE_H
#include "tier0/tslist.h"
#include "tier0/jobthread.h"
#include "launcher/launcher.h"

#include "rtech/ipakfile.h"
#include "rtech/async/asyncio.h"

// This function returns the pak handle of the patch master RPak
inline PakHandle_t(*v_Pak_Initialize)(int mode);
inline bool(*v_Pak_SetupBuffersAndLoad)(const PakHandle_t handle);

inline PakHandle_t(*v_Pak_LoadAsync)(const char* fileName, CAlignedMemAlloc* allocator, int nIdx, bool bUnk);
inline PakStatus_e(*v_Pak_WaitAsync)(PakHandle_t handle, void* finishCallback);
inline void(*v_Pak_UnloadAsync)(PakHandle_t handle);
inline void(*v_Pak_RegisterAsset)(int, int, const char*, void*, void*, void*, void*, int, int, uint32_t, int, int);

inline bool(*v_Pak_StartLoadingPak)(PakLoadedInfo_s* loadedInfo);
inline bool(*v_Pak_ProcessPakFile)(PakFile_s* const pak);
inline bool(*v_Pak_ProcessAssets)(PakLoadedInfo_s* pakInfo);
inline __int64(*v_Pak_ResolveAssetRelations)(PakFile_s* const pak, const PakAsset_s* const asset);
inline uintptr_t s_PakGlobalState_S21;

inline void (*v_Pak_RunAssetLoadingJobs)(PakFile_s* pak);
inline void (*Pak_ProcessAssetRelationsAndResolveDependencies)(PakFile_s* pak_arg, PakAsset_s* asset_arg, unsigned int asset_idx_arg, unsigned int a4);

inline int  (*Pak_TrackAsset)(PakFile_s* const a1, PakAsset_s* a2);

// Finalize loaded-info bookkeeping when a pak tracker is present (pre LOADED status).
inline void (*Pak_FinalizeLoadedInfo)(PakLoadedInfo_s* a1, int a2);

extern const char* const Pak_AllowedModuleStems[];
extern const size_t Pak_AllowedModuleStemCount;

// S21 native pak object (not S3 PakFile_s).
inline constexpr size_t kS21Pak_PakIdOffset = 0x582C;
inline constexpr size_t kS21Pak_MemPageBuffersOffset = 0x5838;
inline constexpr size_t kS21Pak_PageHeadersOffset = 0x5878; // PakPageHeader_s*
inline constexpr size_t kS21Pak_PageDescriptorsOffset = 0x5890;
inline constexpr size_t kS21Pak_HeaderOffset = 0x5D00; // copied PakFileHeader_s

inline uint32_t Pak_S21GetPageCount(const void* const pak)
{
	if (!pak)
		return 0;
	return *reinterpret_cast<const uint16_t*>(
		reinterpret_cast<const uint8_t*>(pak) + kS21Pak_HeaderOffset + 0x4E);
}

inline uint32_t Pak_S21GetGuidDescCount(const void* const pak)
{
	if (!pak)
		return 0;
	return *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(pak) + kS21Pak_HeaderOffset + 0x5C);
}

inline uint32_t Pak_S21GetPageDataSize(const void* const pak, const uint32_t pageIdx)
{
	if (!pak)
		return 0;
	const PakPageHeader_s* const headers = *reinterpret_cast<const PakPageHeader_s* const*>(
		reinterpret_cast<const uint8_t*>(pak) + kS21Pak_PageHeadersOffset);
	if (!headers)
		return 0;
	return headers[pageIdx].dataSize;
}

#endif // RTECH_PAKPARSE_H
#else // !CLIENT_DLL
#ifndef RTECH_PAKPARSE_H
#define RTECH_PAKPARSE_H
#include "tier0/tslist.h"
#include "tier0/jobthread.h"
#include "launcher/launcher.h"

#include "rtech/ipakfile.h"
#include "rtech/async/asyncio.h"

// This function returns the pak handle of the patch master RPak
inline PakHandle_t(*v_Pak_Initialize)(int mode);
inline bool(*v_Pak_SetupBuffersAndLoad)(const PakHandle_t handle);

inline PakHandle_t(*v_Pak_LoadAsync)(const char* fileName, CAlignedMemAlloc* allocator, int nIdx, bool bUnk);
inline PakStatus_e(*v_Pak_WaitAsync)(PakHandle_t handle, void* finishCallback);
inline void(*v_Pak_UnloadAsync)(PakHandle_t handle);
inline void(*v_Pak_RegisterAsset)(int, int, const char*, void*, void*, void*, void*, int, int, uint32_t, int, int);

inline bool(*v_Pak_StartLoadingPak)(PakLoadedInfo_s* loadedInfo);
inline bool(*v_Pak_ProcessPakFile)(PakFile_s* const pak);
inline bool(*v_Pak_ProcessAssets)(PakLoadedInfo_s* pakInfo);
inline void(*v_Pak_ResolveAssetRelations)(PakFile_s* const pak, const PakAsset_s* const asset);

inline void (*v_Pak_RunAssetLoadingJobs)(PakFile_s* pak);
inline void (*Pak_ProcessAssetRelationsAndResolveDependencies)(PakFile_s* pak_arg, PakAsset_s* asset_arg, unsigned int asset_idx_arg, unsigned int a4);

inline int  (*Pak_TrackAsset)(PakFile_s* const a1, PakAsset_s* a2);

// Finalize loaded-info bookkeeping when a pak tracker is present (pre LOADED status).
inline void (*Pak_FinalizeLoadedInfo)(PakLoadedInfo_s* a1, int a2);

///////////////////////////////////////////////////////////////////////////////
class V_PakParse : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Pak_Initialize", v_Pak_Initialize);
		LogFunAdr("Pak_SetupBuffersAndLoad", v_Pak_SetupBuffersAndLoad);

		LogFunAdr("Pak_LoadAsync", v_Pak_LoadAsync);
		LogFunAdr("Pak_WaitAsync", v_Pak_WaitAsync);
		LogFunAdr("Pak_UnloadAsync", v_Pak_UnloadAsync);

		LogFunAdr("Pak_RegisterAsset", v_Pak_RegisterAsset);

		LogFunAdr("Pak_StartLoadingPak", v_Pak_StartLoadingPak);

		LogFunAdr("Pak_ProcessPakFile", v_Pak_ProcessPakFile);
		LogFunAdr("Pak_ProcessAssets", v_Pak_ProcessAssets);
		LogFunAdr("Pak_ResolveAssetRelations", v_Pak_ResolveAssetRelations);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 8B D9 E8 ?? ?? ?? ??").GetPtr(v_Pak_Initialize);
		Module_FindPattern(g_GameDll, "89 4C 24 ?? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 8B C1").GetPtr(v_Pak_SetupBuffersAndLoad);

		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 89 03 8B 0B").FollowNearCallSelf().GetPtr(v_Pak_LoadAsync);
		Module_FindPattern(g_GameDll, "40 53 55 48 83 EC 38 48 89 74 24 ??").GetPtr(v_Pak_WaitAsync);
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 85 FF 74 0C").FollowNearCallSelf().GetPtr(v_Pak_UnloadAsync);
		Module_FindPattern(g_GameDll, "48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 8B 44 24 ?? 4C 8D 35 ?? ?? ?? ??").GetPtr(v_Pak_RegisterAsset);

		Module_FindPattern(g_GameDll, "48 89 4C 24 ?? 56 41 55").GetPtr(v_Pak_StartLoadingPak);
		Module_FindPattern(g_GameDll, "40 53 55 56 41 54 41 56 48 81 EC ?? ?? ?? ??").GetPtr(v_Pak_ProcessPakFile);
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 44 0F B7 05 ?? ?? ?? ??").FollowNearCallSelf().GetPtr(v_Pak_ProcessAssets);
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 48 8B 86 ?? ?? ?? ?? 42 8B 0C B0").FollowNearCallSelf().GetPtr(v_Pak_ResolveAssetRelations);

		Module_FindPattern(g_GameDll, "40 53 56 48 83 EC 58 44 8B 09").GetPtr(v_Pak_RunAssetLoadingJobs);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 54 41 55 41 56 41 57 48 83 EC 20 44 8B 0D ?? ?? ?? ?? 4C 8B E9").GetPtr(Pak_TrackAsset);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 41 8B E9").GetPtr(Pak_ProcessAssetRelationsAndResolveDependencies);

		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? EB 14 48 8D 0D ?? ?? ?? ??").FollowNearCallSelf().GetPtr(Pak_FinalizeLoadedInfo);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // RTECH_PAKPARSE_H
#endif // CLIENT_DLL
