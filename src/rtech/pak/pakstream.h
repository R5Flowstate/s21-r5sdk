#ifndef RTECH_PAKSTREAM_H
#define RTECH_PAKSTREAM_H
#include "rtech/ipakfile.h"
#include "pakstate.h"

// Paks a mod requested by its own path; their headers are mod-authored.
inline bool Pak_IsModSourcedPath(const char* const pszPath)
{
    return pszPath && !V_strnicmp(pszPath, "mods", 4) && (pszPath[4] == '\\' || pszPath[4] == '/');
}

extern void Pak_OpenAssociatedStreamingFiles(PakLoadedInfo_s* const loadedInfo, PakLoadedInfo_s::StreamingInfo_t& streamInfo,
    const uint16_t fileNamesBufSize, const PakStreamSet_e set);

extern void Pak_EnableEmbeddedStreamingData(PakLoadedInfo_s* const loadedInfo, PakLoadedInfo_s::StreamingInfo_t& streamInfo);
extern void Pak_LoadStreamingData(PakLoadedInfo_s* const loadedInfo);

// the current download progress of optional streaming assets
inline float* g_pStreamingDownloadProgress = nullptr;

// NOTE: must use these when incrementing asset counts !!!
inline void Pak_IncrementNonFullyInstalledAssetsCount() { ThreadInterlockedIncrement64(&g_pakGlobals->numAssetsWithDiscardedStreamableAssets); }
inline void Pak_DecrementNonFullyInstalledAssetsCount() { ThreadInterlockedDecrement64(&g_pakGlobals->numAssetsWithDiscardedStreamableAssets); }

inline int64_t Pak_GetNonFullyInstalledAssetsLoadedCount() { return g_pakGlobals->numAssetsWithDiscardedStreamableAssets; }
inline bool Pak_HasNonFullyInstalledAssetsLoaded() { return Pak_GetNonFullyInstalledAssetsLoadedCount() != 0; }

inline float Pak_GetStreamingDownloadProgress() { return *g_pStreamingDownloadProgress; }
inline bool Pak_StreamingDownloadFinished()     { return Pak_GetStreamingDownloadProgress() == 1.0f; }

inline bool Pak_StreamingEnabled() { return g_pakGlobals->useStreamingSystem != NULL; }

class V_PakStream : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogVarAdr("g_streamingDownloadProgress", g_pStreamingDownloadProgress);
	}
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const
	{
		Module_FindPattern(g_GameDll, "F3 0F 10 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC 48 89 5C 24 ?? 57 48 81 EC ?? ?? ?? ??")
			.ResolveRelativeAddress(0x4, 0x8).GetPtr(g_pStreamingDownloadProgress);
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const { };
};

#endif // RTECH_PAKSTREAM_H
