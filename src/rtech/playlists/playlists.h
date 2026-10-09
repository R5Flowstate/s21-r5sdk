#ifndef RTECH_PLAYLISTS_H
#define RTECH_PLAYLISTS_H
#include "tier1/keyvalues.h"

///////////////////////////////////////////////////////////////////////////////
void Playlists_SDKInit(void);
bool Playlists_Load(const char* pszPlaylist);
bool Playlists_Parse(const char* pszPlaylist);
#if defined(CLIENT_DLL)
void MergeModPlaylistsIntoFile(void);
#endif // CLIENT_DLL
KeyValues* Playlists_GetRootKV(void);
void Playlists_LoadOverlayCatalog(void);

// Null-safe current-playlist name for BOTH products; never returns null.
// v_Playlists_GetCurrent is permanently null in client.dll (VPlaylists is
// server-only), so the client half reads S21's own playlist state instead.
const char* Playlists_GetCurrentName(void);

// Host settings: a playlist opens a var to runtime overrides by declaring
//   vars { r5f_setting_<var> "<type> [args]|<label>" }
// with type int <min> <max>, float <min> <max>, bool, or choice <a> <b> ...
// Both engines accept an override of a declared var only when the value fits.
#define PLAYLIST_SETTING_PREFIX "r5f_setting_"

const char* Playlists_FindVar(const char* pszPlaylist, const char* pszVar);
const char* Playlists_FindSettingDecl(const char* pszPlaylist, const char* pszVar);
bool Playlists_ValidateSetting(const char* pszDecl, const char* pszValue, char* pszReason, size_t nReasonSize);
bool Playlists_IsSettingDeclName(const char* pszVar);
#if !defined(CLIENT_DLL)
void Playlists_DescribeActiveSettings(char* pszOut, const size_t nOutSize);
#endif // !CLIENT_DLL

///////////////////////////////////////////////////////////////////////////////
inline bool(*v_Playlists_Load)(const char* pszPlaylist);
inline bool(*v_Playlists_Parse)(const char* pszPlaylist);
inline const char* (*v_Playlists_GetCurrent)(void);
inline void(*v_Playlists_Download_f)(void);

// Reads the playlist file (-playlistFile) into the engine's text buffer and
// length (+1 for the NUL); the loader parses that buffer next.
inline bool(*v_Playlists_ReadFile)(void);
extern char** g_ppPlaylistFileText;
extern int* g_pnPlaylistFileTextSize;

// Runtime playlist var overrides. Authoring is server-side only; the values ride
// svc_PlaylistOverrides (dedi msg 34) to every connected client, which the bridge
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
extern bool* g_pPlaylistOverridesDirty;
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
		LogFunAdr("Playlists_ReadFile", v_Playlists_ReadFile);
		LogFunAdr("Playlist_SetVarOverride", v_Playlist_SetVarOverride);
		LogFunAdr("Playlist_ClearVarOverrides", v_Playlist_ClearVarOverrides);
		LogVarAdr("g_pPlaylistKeyValues", g_pPlaylistKeyValues);
		LogVarAdr("g_pPlaylistMapToLoad", g_pPlaylistMapToLoad);
		LogVarAdr("g_pPlaylistOverrideCount", g_pPlaylistOverrideCount);
		LogVarAdr("g_pPlaylistOverridesDirty", g_pPlaylistOverridesDirty);
		LogVarAdr("g_pPlaylistOverrideTable", g_pPlaylistOverrideTable);
		LogVarAdr("g_ppPlaylistFileText", g_ppPlaylistFileText);
		LogVarAdr("g_pnPlaylistFileTextSize", g_pnPlaylistFileTextSize);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC 40 48 8B F1").GetPtr(v_Playlists_Load);
		Module_FindPattern(g_GameDll, "E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 74 0C").FollowNearCallSelf().GetPtr(v_Playlists_Parse);
		Module_FindPattern(g_GameDll, "48 8B 05 ?? ?? ?? ?? 48 85 C0 75 08 48 8D 05 ?? ?? ?? ?? C3 0F B7 50 2A").GetPtr(v_Playlists_GetCurrent);
		Module_FindPattern(g_GameDll, "33 C9 C6 05 ?? ?? ?? ?? ?? E9 ?? ?? ?? ??").GetPtr(v_Playlists_Download_f);

		// The `lea rcx, g_pCmdLine; cmp byte [rip+disp32], r14b` pair is the -playlistFile lookup.
		Module_FindPattern(g_GameDll, "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 B9 48 81 EC A0 00 00 00 45 33 F6 48 8D 0D ?? ?? ?? ?? 44 38 35")
			.GetPtr(v_Playlists_ReadFile);
		if (!v_Playlists_ReadFile)
			Warning(eDLL_T::ENGINE, "[PLAYLIST] playlist file reader unresolved; mod playlists will not merge\n");

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
		{
			g_pPlaylistOverrideCount = CMemory(v_Playlist_ClearVarOverrides).ResolveRelativeAddressSelf(0x3, 0x8).RCast<int64_t*>();
			// +0x15: `mov byte [rip+disp32], 1` on the rebroadcast dirty flag.
			g_pPlaylistOverridesDirty = CMemory(v_Playlist_ClearVarOverrides).OffsetSelf(0x15).ResolveRelativeAddressSelf(0x2, 0x7).RCast<bool*>();
		}

		// First `lea rax, [rip+disp32]` in Playlist_SetVarOverride is the table base on the
		// append path (the earlier lea in the at-capacity warning targets rcx, not rax).
		// After the file-size query: `mov rdx, [text]; mov rsi, rax; lea ecx, [rax+1]; mov [size], ecx`.
		if (v_Playlists_ReadFile)
		{
			CMemory sizeSite = CMemory(v_Playlists_ReadFile).FindPatternSelf("48 8B 15 ?? ?? ?? ?? 48 8B F0 8D 48 01 89 0D", CMemory::Direction::DOWN, 0x100);
			if (sizeSite)
			{
				g_ppPlaylistFileText = sizeSite.ResolveRelativeAddress(0x3, 0x7).RCast<char**>();
				g_pnPlaylistFileTextSize = sizeSite.Offset(0xD).ResolveRelativeAddress(0x2, 0x6).RCast<int*>();
			}
			else
				Warning(eDLL_T::ENGINE, "[PLAYLIST] playlist text buffer unresolved; mod playlists will not merge\n");
		}

		if (v_Playlist_SetVarOverride)
			g_pPlaylistOverrideTable = CMemory(v_Playlist_SetVarOverride).FindPatternSelf("48 8D 05", CMemory::Direction::DOWN, 200).ResolveRelativeAddressSelf(0x3, 0x7).RCast<char*>();
	}
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // RTECH_PLAYLISTS_H
