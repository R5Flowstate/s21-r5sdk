//=============================================================================//
//
// Purpose: Synthesize settings (stgs) rpak assets from loose JSON under
// platform/settings/. Clones a packed donor layout/data block and overwrites
// named fields; packed assets always win.
//
// Client (CLIENT_DLL): detour Pak_FindAssetVoid / uniqueId lookup.
// Server (DEDICATED): insert into the engine pak asset hash table after
// asset-publish (S3 lookups are inlined, so there is no find chokepoint).
//
//=============================================================================//

#ifndef SETTINGS_DISK_S21_H
#define SETTINGS_DISK_S21_H

#include "thirdparty/detours/include/idetour.h"

#if defined(CLIENT_DLL)
// Pak_FindAssetVoid(guid, handleOut) -> SettingsHeader* (or null).
// handleOut may be null; when non-null the engine writes a pak handle.
inline void* (__fastcall *v_Pak_FindAssetVoid_S21)(uint64_t guid, uint32_t* handleOut) = nullptr;

// Settings_GetSettingsHeaderForUniqueId(uniqueId) -> SettingsHeader*.
// Shared by client + UI script VMs; one hook covers both.
inline void* (__fastcall *v_Settings_GetSettingsHeaderForUniqueId_S21)(uint32_t uniqueId) = nullptr;
#else
// Asset-publish walk: drops dep refs; writes real guid when fully available.
inline __int64 (__fastcall *v_Pak_AssetPublish_S3)(void* a1, __int64 a2) = nullptr;
// Asset-TYPE registry base; guid->slot table begins at +0x1000.
inline uintptr_t g_pPakAssetTypeRegistry_S3 = 0;
#endif // CLIENT_DLL

///////////////////////////////////////////////////////////////////////////////
class VSettingsDiskS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
#if defined(CLIENT_DLL)
		LogFunAdr("Pak_FindAssetVoid_S21", v_Pak_FindAssetVoid_S21);
		LogFunAdr("Settings_GetSettingsHeaderForUniqueId_S21",
			v_Settings_GetSettingsHeaderForUniqueId_S21);
#else
		LogFunAdr("Pak_AssetPublish_S3", v_Pak_AssetPublish_S3);
		LogVarAdr("g_pPakAssetTypeRegistry_S3",
			reinterpret_cast<const void*>(g_pPakAssetTypeRegistry_S3));
#endif // CLIENT_DLL
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const {}
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // SETTINGS_DISK_S21_H
