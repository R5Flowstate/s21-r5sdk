#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "filesystem/basefilesystem.h"
#include "filesystem/filesystem.h"
#include "pluginsystem/modsystem.h"
#include "bspfile.h"
#include "engine/host_state.h"
#include "engine/modelloader.h"
#include "vpklib/packedstore.h"

static ConVar fs_showWarnings("fs_showWarnings", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS, "Logs the FileSystem warnings to the console, filtered by 'fs_warning_level' ( !slower! ).", true, 0.f, true, 2.f, "0 = log to file. 1 = 0 + log to console. 2 = 1 + log to notify");

static ConVar fs_vpk_prioritizeDisk("fs_vpk_prioritizeDisk", "0", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS, "Whether to look for our file on the disk before looking for it in the VPK.");
static ConVar fs_stalePlaylistPreload("fs_stalePlaylistPreload", "0", FCVAR_RELEASE, "Mount the map VPK the boot playlist declares even when the command line names a different map.");
static ConVar fs_guardLiveMapUnmount("fs_guardLiveMapUnmount", "1", FCVAR_RELEASE, "Refuse to unmount the live map's server VPK while HS_RUN still simulates on it (an eager playlist remount otherwise pulls collision out from under player traces).");

static ConVar fs_vpk_prioritizeDiskPath("fs_vpk_prioritizeDiskPath", "platform/", FCVAR_DEVELOPMENTONLY | FCVAR_ACCESSIBLE_FROM_THREADS, "Where to look for our file on the disk before looking for it in the VPK.");

//---------------------------------------------------------------------------------
// Purpose: prints the output of the filesystem based on the warning level
// Input: *this - 
// level - 
// *pFmt - 
//---------------------------------------------------------------------------------
void CBaseFileSystem::Warning(CBaseFileSystem* pFileSystem, FileWarningLevel_t level, const char* pFmt, ...)
{
	if (level >= FileWarningLevel_t::FILESYSTEM_WARNING_REPORTALLACCESSES)
	{
		// Logging reads are very verbose! Explicitly toggle..
		if (!fs_showAllReads->GetBool())
		{
			return;
		}
	}

	va_list args;
	va_start(args, pFmt);
	CoreMsgV(LogType_t::LOG_WARNING, static_cast<LogLevel_t>(fs_showWarnings.GetInt()), eDLL_T::FS, "filesystem", pFmt, args);
	va_end(args);
}

//---------------------------------------------------------------------------------
// Purpose: attempts to load files from disk if exist before loading from VPK/cache
// Input: *pszFilePath - 
// Output: handle to file on success, NULL on failure
//---------------------------------------------------------------------------------
bool CBaseFileSystem::VCheckDisk(const char* pszFilePath)
{
	if (!fs_vpk_prioritizeDisk.GetBool())
		return false;

	// Only load material files from the disk if the mode isn't zero,
	// use -novpk to load valve materials from the disk.
	if (FileSystem()->CheckVPKMode(0) && V_strstr(pszFilePath, ".vmt"))
	{
		return false;
	}

	if (V_IsAbsolutePath(pszFilePath))
	{
		// Skip absolute file paths.
		return false;
	}

	CUtlString filePath = fs_vpk_prioritizeDiskPath.GetString();

	if (filePath[0])
		filePath.AppendSlash();

	filePath.Append(pszFilePath);
	filePath.FixSlashes();

	filePath = filePath.Replace("\\*\\", "");

	if (::FileExists(filePath.Get()))
	{
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------------
// Purpose: loads files from VPK
// Input: *this - 
// *pResults - 
// *pszFilePath - 
// Output: handle to file on success, NULL on failure
//---------------------------------------------------------------------------------
FileHandle_t CBaseFileSystem::VReadFromVPK(CBaseFileSystem* pFileSystem, FileHandle_t pResults, const char* pszFilePath)
{
	if (VCheckDisk(pszFilePath))
	{
		*reinterpret_cast<int64_t*>(pResults) = -1;
		return pResults;
	}

	return CBaseFileSystem__LoadFromVPK(pFileSystem, pResults, pszFilePath);
}

//---------------------------------------------------------------------------------
// Purpose: loads files from cache
// Input: *this - 
// *pszFilePath - 
// *pCache - 
// Output: true if file exists, false otherwise
//---------------------------------------------------------------------------------
bool CBaseFileSystem::VReadFromCache(CBaseFileSystem* pFileSystem, const char* pszFilePath, FileSystemCache* pCache)
{
	if (VCheckDisk(pszFilePath))
	{
		return false;
	}

	bool result = CBaseFileSystem__LoadFromCache(pFileSystem, pszFilePath, pCache);
	return result;
}

//---------------------------------------------------------------------------------
// Purpose: mounts a BSP packfile lump as search path
// Input: *this - 
// *pPath - 
// *pPathID - 
// *addType - 
//---------------------------------------------------------------------------------
void CBaseFileSystem::VAddMapPackFile(CBaseFileSystem* pFileSystem, const char* pPath, const char* pPathID, SearchPathAdd_t addType)
{
	// Since the mounting of the packfile lump is performed before the BSP header
	// is loaded and parsed, we have to do it here. The internal 'AddMapPackFile'
	// function has been patched to load the fields in the global 's_MapHeader'
	// field, instead of the one that is getting initialized (see r5apex.patch).
	if (s_MapHeader->ident != IDBSPHEADER || s_MapHeader->version != BSPVERSION)
	{
		FileHandle_t hBspFile = FileSystem()->Open(pPath, "rb", pPathID);
		if (hBspFile != FILESYSTEM_INVALID_HANDLE)
		{
			memset(s_MapHeader, '\0', sizeof(BSPHeader_t));
			FileSystem()->Read(s_MapHeader, sizeof(BSPHeader_t), hBspFile);
		}
	}

	// If a lump exists, replace the path pointer with that of the lump so that
	// the internal function loads this instead.
	char lumpPathBuf[MAX_PATH];
	V_snprintf(lumpPathBuf, sizeof(lumpPathBuf), "%s.%.4X.bsp_lump", pPath, LUMP_PAKFILE);

	if (FileSystem()->FileExists(lumpPathBuf, pPathID))
	{
		pPath = lumpPathBuf;
	}

	CBaseFileSystem__AddMapPackFile(pFileSystem, pPath, pPathID, addType);
}

//---------------------------------------------------------------------------------
// Purpose: whether this mount is the boot playlist prewarming a map the command
// line already overrode. Setting the playlist mounts the VPK of the map the
// playlist declares; a '+map' on the command line then unmounts it a frame later
// and mounts the real one, so the first mount is pure boot cost.
// Input: *pszVpkPath -
// Output: true if the mount should be skipped
//---------------------------------------------------------------------------------
static bool FileSystem_IsStalePlaylistPreload(const char* const pszVpkPath)
{
	static const char* s_pszBootMap = CommandLine()->ParmValue("+map", (const char*)nullptr);
	static bool s_bBootMapPending = (s_pszBootMap && s_pszBootMap[0]);

	if (!s_bBootMapPending || fs_stalePlaylistPreload.GetBool())
		return false;

	const char* pszMap = V_strstr(pszVpkPath, "server_");

	if (!pszMap)
		pszMap = V_strstr(pszVpkPath, "client_");

	if (!pszMap)
		return false;

	pszMap += sizeof("server_") - 1;

	char szMap[MAX_MAP_NAME];
	V_StripExtension(pszMap, szMap, sizeof(szMap));

	if (V_strncmp(szMap, "mp_rr_", sizeof("mp_rr_") - 1) != NULL)
		return false; // Not a level VPK (mp_common and friends stay mounted).

	if (!V_stricmp(szMap, s_pszBootMap))
	{
		s_bBootMapPending = false; // The level load reached us; stop filtering.
		return false;
	}

	::Msg(eDLL_T::FS, "Skipped playlist preload of vpk file: '%s' ('%s' was requested)\n",
		pszVpkPath, s_pszBootMap);

	return true;
}

#ifndef CLIENT_DLL
//---------------------------------------------------------------------------------
// Purpose: whether this unmount would pull the live map's server VPK while HS_RUN
// still simulates on it. The engine launchplaylist command runs Playlists_Parse
// synchronously; on a playlist change that parse remounts VPKs at once, so the
// resident map is unmounted frames before the queued changelevel runs its
// LevelShutdown. The next player FullWalkMove then walks freed brushModelColl
// in Coll_RunQueryGuts and AVs. Veto the unmount and mimic success: the target
// map stays mounted as a harmless preload, the live map keeps simulating, and
// the real transition unmounts again once loading (IsRunning false then).
// Input: *pszVpkPath -
// Output: true if the unmount must be skipped
//---------------------------------------------------------------------------------
static bool FileSystem_ShouldVetoLiveMapUnmount(const char* const pszVpkPath)
{
	if (!fs_guardLiveMapUnmount.GetBool())
		return false;

	if (!g_pHostState || !g_pHostState->IsRunning() || HostState_IsTransitioningToLoad())
		return false;

	const char* pszMap = V_strstr(pszVpkPath, "server_");

	if (!pszMap)
		return false;

	pszMap += sizeof("server_") - 1;

	char szMap[MAX_MAP_NAME];
	V_StripExtension(pszMap, szMap, sizeof(szMap));

	if (V_stricmp(szMap, g_pHostState->m_levelName) != 0)
		return false; // Not the resident map (stale preload cleanup, mod VPK).

	return true;
}
#endif // !CLIENT_DLL

//---------------------------------------------------------------------------------
// Purpose: attempts to mount VPK file for filesystem usage
// Input: *this - 
// *pszVpkPath - 
// Output: pointer to VPK on success, NULL on failure
//---------------------------------------------------------------------------------
CPackedStore* CBaseFileSystem::VMountVPKFile(CBaseFileSystem* pFileSystem, const char* pszVpkPath)
{
	if (FileSystem_IsStalePlaylistPreload(pszVpkPath))
		return nullptr;

	int nHandle = CBaseFileSystem__GetMountedVPKHandle(pFileSystem, pszVpkPath);
	CPackedStore* pPakData = CBaseFileSystem__MountVPKFile(pFileSystem, pszVpkPath);

	CUtlString modLookupPath;
	const char* fileToLoad = pszVpkPath;

	if (!pPakData && ModSystem()->IsEnabled())
	{
		ModSystem()->LockModList();

		// Look for the file in our mods and obtain the first one we find.
		FOR_EACH_VEC(ModSystem()->GetResolvedModList(), i)
		{
			const CModSystem::ModInstance_t* const mod = ModSystem()->GetResolvedModList()[i];

			if (!mod->IsEnabled())
				continue;

			if (!ModSystem_IsSafeRelativePath(fileToLoad))
				continue;

			modLookupPath = mod->GetBasePath() + fileToLoad;
			const char* const pModLookupPath = modLookupPath.String();

			nHandle = CBaseFileSystem__GetMountedVPKHandle(pFileSystem, pModLookupPath);
			pPakData = CBaseFileSystem__MountVPKFile(pFileSystem, pModLookupPath);

			if (pPakData)
			{
				fileToLoad = pModLookupPath;
				break;
			}
		}

		ModSystem()->UnlockModList();
	}

	if (pPakData)
	{
		if (nHandle < 0) // Only log if VPK hasn't been mounted yet.
			::Msg(eDLL_T::FS, "Mounted vpk file: '%s' with handle: %i\n", fileToLoad, pPakData->GetPackFileID());
	}
	else // VPK failed to load or does not exist...
	{
		::Error(eDLL_T::FS, 0, "Unable to mount vpk file: '%s'\n", fileToLoad);
	}

	return pPakData;
}

//---------------------------------------------------------------------------------
// Purpose: unmount a VPK file
// Input: *this - 
// *pszVpkPath - 
// Output: pointer to formatted VPK path string
//---------------------------------------------------------------------------------
const char* CBaseFileSystem::VUnmountVPKFile(CBaseFileSystem* pFileSystem, const char* pszVpkPath)
{
	const char* pRet = strstr(pszVpkPath, ".bsp");

	if (!pRet || pRet == pszVpkPath)
		return pRet; // Invalid VPK file name.

#ifndef CLIENT_DLL
	if (FileSystem_ShouldVetoLiveMapUnmount(pszVpkPath))
	{
		::Warning(eDLL_T::FS, "[PACK-GUARD] vetoed unmount of live map VPK '%s' (HS_RUN still tracing it; transition will unmount)\n",
			pszVpkPath);
		return pRet;
	}
#endif // !CLIENT_DLL

	// NOTE: for unmounting VPK's, we don't need to resolve the paths for mods
	// even if the VPK was loaded from a mod directory, because internally the
	// code compares the name from vpk/ and skips anything before it.
	const int nHandle = CBaseFileSystem__GetMountedVPKHandle(pFileSystem, pszVpkPath);

	if (nHandle >= 0)
	{
		pRet = CBaseFileSystem__UnmountVPKFile(pFileSystem, pszVpkPath);
		::Msg(eDLL_T::FS, "Unmounted vpk file: '%s' with handle: %i\n", pszVpkPath, nHandle);

		return pRet;
	}
	else // VPK failed to unload or does not exist...
	{
		::Error(eDLL_T::FS, 0, "Unable to unmount vpk file: '%s'\n", pszVpkPath);
		return nullptr;
	}
}

//---------------------------------------------------------------------------------
// Purpose: reads a string until its null terminator
// Input: *pFile - 
// Output: string
//---------------------------------------------------------------------------------
CUtlString CBaseFileSystem::ReadString(FileHandle_t pFile)
{
	CUtlString result;
	char c = '\0';
	constexpr ssize_t kMaxLen = 4096;

	for (;;)
	{
		if (Read(&c, sizeof(char), pFile) != sizeof(char))
			return CUtlString();

		if (!c)
			return result;

		if (result.Length() >= kMaxLen)
			return CUtlString();

		result += c;
	}
}

#ifndef CLIENT_DLL
void VBaseFileSystem::Detour(const bool bAttach) const
{
	DetourSetup(&CBaseFileSystem__Warning, &CBaseFileSystem::Warning, bAttach);
	DetourSetup(&CBaseFileSystem__LoadFromVPK, &CBaseFileSystem::VReadFromVPK, bAttach);
	DetourSetup(&CBaseFileSystem__LoadFromCache, &CBaseFileSystem::VReadFromCache, bAttach);
	DetourSetup(&CBaseFileSystem__AddMapPackFile, &CBaseFileSystem::VAddMapPackFile, bAttach);
	DetourSetup(&CBaseFileSystem__MountVPKFile, &CBaseFileSystem::VMountVPKFile, bAttach);
	DetourSetup(&CBaseFileSystem__UnmountVPKFile, &CBaseFileSystem::VUnmountVPKFile, bAttach);
}
#endif // !CLIENT_DLL

CBaseFileSystem* g_pFileSystem = nullptr;
