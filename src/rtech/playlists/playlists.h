#ifndef RTECH_PLAYLISTS_H
#define RTECH_PLAYLISTS_H
#include "tier1/keyvalues.h"

///////////////////////////////////////////////////////////////////////////////
void Playlists_SDKInit(void);
bool Playlists_Load(const char* pszPlaylist);
bool Playlists_Parse(const char* pszPlaylist);
void MergeModPlaylistsIntoFile(void);
KeyValues* Playlists_GetRootKV(void);
void Playlists_LoadOverlayCatalog(void);

// Null-safe current-playlist name for BOTH products; never returns null.
// v_Playlists_GetCurrent is permanently null in client.dll (VPlaylists is
// server-only), so the client half reads S21's own playlist state instead.
const char* Playlists_GetCurrentName(void);

///////////////////////////////////////////////////////////////////////////////
inline bool(*v_Playlists_Load)(const char* pszPlaylist);
inline bool(*v_Playlists_Parse)(const char* pszPlaylist);
inline const char* (*v_Playlists_GetCurrent)(void);
inline void(*v_Playlists_Download_f)(void);

// Runtime playlist var overrides. Authoring is server-side only; the values ride
// svc_PlaylistOverrides (S3 msg 34) to every connected client, which the bridge
// client decodes and applies (S21 deleted its own override subsystem).
inline void(*v_Playlist_SetVarOverride)(const char* pszName, const char* pszValue);
inline void(*v_Playlist_ClearVarOverrides)(void);

// Override table entry: name[128] then value[64]; 64 entries max (engine cap).
#define PLAYLIST_OVERRIDE_STRIDE 192
#define PLAYLIST_OVERRIDE_VALUE_OFFSET 128
#define PLAYLIST_OVERRIDE_MAX_ENTRIES 64

extern KeyValues** g_pPlaylistKeyValues;
extern char* g_pPlaylistMapToLoad;
extern int64_t* g_pPlaylistOverrideCount;
extern char* g_pPlaylistOverrideTable;

extern CUtlVector<CUtlString> g_vecAllPlaylists;
extern CUtlVector<CUtlString> g_vecOverlayMaps;

///////////////////////////////////////////////////////////////////////////////
class VPlaylists : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Playlists_Load", v_Playlists_Load);
		LogFunAdr("Playlists_Parse", v_Playlists_Parse);
		LogFunAdr("Playlists_GetCurrent", v_Playlists_GetCurrent);
		LogFunAdr("Playlists_Download_f", v_Playlists_Download_f);
		LogFunAdr("Playlist_SetVarOverride", v_Playlist_SetVarOverride);
		LogFunAdr("Playlist_ClearVarOverrides", v_Playlist_ClearVarOverrides);
		LogVarAdr("g_pPlaylistKeyValues", g_pPlaylistKeyValues);
		LogVarAdr("g_pPlaylistMapToLoad", g_pPlaylistMapToLoad);
		LogVarAdr("g_pPlaylistOverrideCount", g_pPlaylistOverrideCount);
		LogVarAdr("g_pPlaylistOverrideTable", g_pPlaylistOverrideTable);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC 40 48 8B F1").GetPtr(v_Playlists_Load);
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 74 0C").FollowNearCallSelf().GetPtr(v_Playlists_Parse);
		Module_FindPattern(g_GameDll, "48 8B 05 ?? ?? ?? ?? 48 85 C0 75 08 48 8D 05 ?? ?? ?? ?? C3 0F B7 50 2A").GetPtr(v_Playlists_GetCurrent);
		Module_FindPattern(g_GameDll, "33 C9 C6 05 ?? ?? ?? ?? ?? E9 ?? ?? ?? ??").GetPtr(v_Playlists_Download_f);

		// Playlist_SetVarOverride(name, value) -- engine helper. The `83 3D ?? ?? ?? ?? 02`
		// is the host-state >= 2 test that picks the authoring branch over the client-side
		// clc_SetPlaylistVarOverride branch.
		Module_FindPattern(g_GameDll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC 50 01 00 00 83 3D ?? ?? ?? ?? 02 48 8B EA 48 8B F1")
			.GetPtr(v_Playlist_SetVarOverride);

		// Playlist_ClearVarOverrides() -- engine helper; zeroes the count and raises the
		// dirty byte the host frame watches to rebroadcast svc_PlaylistOverrides.
		Module_FindPattern(g_GameDll, "48 83 3D ?? ?? ?? ?? 00 74 12 48 C7 05 ?? ?? ?? ?? 00 00 00 00 C6 05 ?? ?? ?? ?? 01 C3")
			.GetPtr(v_Playlist_ClearVarOverrides);
	}
	virtual void GetVar(void) const
	{
		g_pPlaylistKeyValues = Module_FindPattern(g_GameDll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F9 E8 B4")
			.FindPatternSelf("48 8B 0D", CMemory::Direction::DOWN, 100).ResolveRelativeAddressSelf(0x3, 0x7).RCast<KeyValues**>();

		g_pPlaylistMapToLoad = CMemory(v_Playlists_Parse).OffsetSelf(0x130).FindPatternSelf("80 3D").ResolveRelativeAddressSelf(0x2, 0x7).RCast<char*>();

		// Playlist_ClearVarOverrides opens with `cmp qword [rip+disp32], 0` (8 bytes incl.
		// the imm8) on the override count.
		if (v_Playlist_ClearVarOverrides)
			g_pPlaylistOverrideCount = CMemory(v_Playlist_ClearVarOverrides).ResolveRelativeAddressSelf(0x3, 0x8).RCast<int64_t*>();

		// First `lea rax, [rip+disp32]` in Playlist_SetVarOverride is the table base on the
		// append path (the earlier lea in the at-capacity warning targets rcx, not rax).
		if (v_Playlist_SetVarOverride)
			g_pPlaylistOverrideTable = CMemory(v_Playlist_SetVarOverride).FindPatternSelf("48 8D 05", CMemory::Direction::DOWN, 200).ResolveRelativeAddressSelf(0x3, 0x7).RCast<char*>();
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // RTECH_PLAYLISTS_H
