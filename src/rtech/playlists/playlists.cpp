//===========================================================================//
//
// Purpose: Playlists system
//
//===========================================================================//
#include "core/stdafx.h"
#include "engine/sys_dll2.h"
#include "engine/cmodel_bsp.h"
#include "playlists.h"
#include "pluginsystem/modsystem.h"
#include "filesystem/filesystem.h"

static ConVar playlist_debug("playlist_debug", "0", FCVAR_DEVELOPMENTONLY, "Enable debug logging for playlist mod system");

KeyValues** g_pPlaylistKeyValues = nullptr; // The KeyValue for the playlist file.
char* g_pPlaylistMapToLoad = nullptr;
int64_t* g_pPlaylistOverrideCount = nullptr;
char* g_pPlaylistOverrideTable = nullptr;

CUtlVector<CUtlString> g_vecAllPlaylists;   // Cached playlists entries.
CUtlVector<CUtlString> g_vecOverlayMaps;    // r5f_map_names.txt stems, file order.

#if defined(CLIENT_DLL)
static KeyValues* s_pClientPlaylistFile = nullptr;
#endif // CLIENT_DLL

KeyValues* Playlists_GetRootKV(void)
{
	if (g_pPlaylistKeyValues && *g_pPlaylistKeyValues)
		return *g_pPlaylistKeyValues;
#if defined(CLIENT_DLL)
	return s_pClientPlaylistFile;
#else
	return nullptr;
#endif // CLIENT_DLL
}

#if defined(CLIENT_DLL)
static bool Playlists_MakeExePlatformPath(char* path, const size_t n, const char* pszFile)
{
	if (!GetModuleFileNameA(nullptr, path, static_cast<DWORD>(n)))
		return false;

	char* const slash = strrchr(path, '\\');
	if (!slash)
		return false;

	slash[1] = '\0';
	V_strncat(path, "platform\\", n);
	V_strncat(path, pszFile, n);
	return true;
}

static void Playlists_LoadOverlayMaps(void)
{
	g_vecOverlayMaps.Purge();

	char path[MAX_PATH];
	if (!Playlists_MakeExePlatformPath(path, sizeof(path), "r5f_map_names.txt"))
		return;

	FILE* fp = nullptr;
	if (fopen_s(&fp, path, "r") != 0 || !fp)
		return;

	char line[256];
	int n = 0;
	while (fgets(line, sizeof(line), fp) && n < 64)
	{
		char* p = line;
		while (*p == ' ' || *p == '\t')
			p++;

		char* nl = p;
		while (*nl && *nl != '\r' && *nl != '\n')
			nl++;
		*nl = '\0';

		if (!p[0] || p[0] == '#' || (p[0] == '/' && p[1] == '/'))
			continue;

		char* const eq = strchr(p, '=');
		if (!eq || eq == p)
			continue;

		*eq = '\0';
		char* end = eq;
		while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
		{
			end--;
			*end = '\0';
		}

		if (!p[0])
			continue;

		g_vecOverlayMaps.AddToTail(p);
		n++;
	}

	fclose(fp);
}

static bool Playlists_LoadFileFromExeDir(KeyValues* pKV)
{
	char path[MAX_PATH];
	if (!Playlists_MakeExePlatformPath(path, sizeof(path), "playlists_r5_patch.txt"))
		return false;

	FILE* fp = nullptr;
	if (fopen_s(&fp, path, "rb") != 0 || !fp)
		return false;

	if (fseek(fp, 0, SEEK_END) != 0)
	{
		fclose(fp);
		return false;
	}

	const long sz = ftell(fp);
	if (sz <= 0 || sz > 4 * 1024 * 1024)
	{
		fclose(fp);
		return false;
	}

	if (fseek(fp, 0, SEEK_SET) != 0)
	{
		fclose(fp);
		return false;
	}

	char* const buf = new char[static_cast<size_t>(sz) + 1];
	const size_t nRead = fread(buf, 1, static_cast<size_t>(sz), fp);
	fclose(fp);
	buf[nRead] = '\0';

	const bool ok = pKV->LoadFromBuffer("playlists_r5_patch.txt", buf, nullptr, nullptr);
	delete[] buf;
	return ok;
}
#endif // CLIENT_DLL

void Playlists_LoadOverlayCatalog(void)
{
#if defined(CLIENT_DLL)
	if (s_pClientPlaylistFile)
		return;

	s_pClientPlaylistFile = new KeyValues("playlists");
	if (!Playlists_LoadFileFromExeDir(s_pClientPlaylistFile))
	{
		Warning(eDLL_T::ENGINE, "[PLAYLIST] overlay catalog failed to load playlists_r5_patch.txt\n");
		delete s_pClientPlaylistFile;
		s_pClientPlaylistFile = nullptr;
		return;
	}

	KeyValues* const pPlaylists = s_pClientPlaylistFile->FindKey("Playlists");
	if (!pPlaylists)
		return;

	struct OverlayPl_t
	{
		char szName[64];
		int nOrder;
	};

	OverlayPl_t entries[64];
	int nEntries = 0;

	// r5f_mode = launcher+overlay; r5f_overlay = overlay only.
	for (KeyValues* pSub = pPlaylists->GetFirstTrueSubKey(); pSub && nEntries < 64; pSub = pSub->GetNextTrueSubKey())
	{
		KeyValues* const pVars = pSub->FindKey("vars");
		if (!pVars)
			continue;
		if (pVars->GetInt("r5f_mode", 0) != 1 && pVars->GetInt("r5f_overlay", 0) != 1)
			continue;

		V_strncpy(entries[nEntries].szName, pSub->GetName(), sizeof(entries[nEntries].szName));
		entries[nEntries].nOrder = pVars->GetInt("r5f_mode_order", 1000);
		nEntries++;
	}

	for (int i = 1; i < nEntries; i++)
	{
		const OverlayPl_t key = entries[i];
		int j = i - 1;
		while (j >= 0 && entries[j].nOrder > key.nOrder)
		{
			entries[j + 1] = entries[j];
			j--;
		}
		entries[j + 1] = key;
	}

	g_vecAllPlaylists.Purge();
	for (int i = 0; i < nEntries; i++)
		g_vecAllPlaylists.AddToTail(entries[i].szName);

	Playlists_LoadOverlayMaps();

	Msg(eDLL_T::ENGINE, "[PLAYLIST] overlay catalog %d playlist(s), %d map(s)\n",
		nEntries, g_vecOverlayMaps.Count());
#endif // CLIENT_DLL
}

//-----------------------------------------------------------------------------
// Purpose: Merges mod playlist patches into the base playlist file at startup
//-----------------------------------------------------------------------------
void MergeModPlaylistsIntoFile()
{
	const char* playlistFilePath = "platform/playlists_r5_patch.txt";
	
	// Load the base playlist file
	KeyValues* pBaseKV = new KeyValues("playlists");
	if (!pBaseKV->LoadFromFile(FileSystem(), playlistFilePath, "GAME"))
	{
		Msg(eDLL_T::ENGINE, "Could not load base playlist file for mod merging\n");
		delete pBaseKV;
		return;
	}

	bool hasChanges = false;

	// First, clean up orphaned mod playlists from the base file
	KeyValues* pBasePlaylists = pBaseKV->FindKey("Playlists");
	if (pBasePlaylists)
	{
		if (playlist_debug.GetBool())
			Msg(eDLL_T::ENGINE, "Checking for orphaned playlists in base file...\n");
		int playlistCount = 0;
		for (KeyValues* pPlaylist = pBasePlaylists->GetFirstTrueSubKey(); pPlaylist; )
		{
			KeyValues* pNext = pPlaylist->GetNextTrueSubKey();
			const char* playlistName = pPlaylist->GetName();
			// Check both direct for_mod and vars/for_mod
			const char* forMod = pPlaylist->GetString("for_mod", "");
			if (!forMod || !forMod[0])
			{
				// Try looking in vars subsection
				KeyValues* pVars = pPlaylist->FindKey("vars");
				if (pVars)
				{
					forMod = pVars->GetString("for_mod", "");
				}
			}
			playlistCount++;
			
			if (playlist_debug.GetBool())
				Msg(eDLL_T::ENGINE, "Playlist #%d: '%s', for_mod='%s'\n", playlistCount, playlistName, forMod ? forMod : "(null)");
			
			if (forMod && forMod[0]) // Has for_mod var
			{
				if (playlist_debug.GetBool())
					Msg(eDLL_T::ENGINE, "Found playlist '%s' with for_mod='%s', checking if mod is enabled...\n", 
						playlistName, forMod);
				
				// Check if the mod is still enabled and still provides this playlist
				bool playlistStillProvided = false;
				if (ModSystem()->IsEnabled())
				{
					ModSystem()->LockModList();
					FOR_EACH_VEC(ModSystem()->GetModList(), i)
					{
						CModSystem::ModInstance_t* const pMod = ModSystem()->GetModList()[i];
						if (pMod && pMod->IsEnabled() && V_strcmp(pMod->name.String(), forMod) == 0)
						{
							// Check if this mod still provides this specific playlist
							static const char* kPatchFiles[] = {
								"playlists_r5_patch.txt",
								"playlist_r5_patch.txt"
							};

							for (int j = 0; j < Q_ARRAYSIZE(kPatchFiles); ++j)
							{
								CUtlString patchPath = pMod->GetBasePath();
								patchPath += kPatchFiles[j];

								if (!FileSystem()->FileExists(patchPath.Get(), "GAME"))
									continue;

								// Load mod's playlist patch to check if it still provides this playlist
								KeyValues* pModKV = new KeyValues("playlists");
								if (pModKV->LoadFromFile(FileSystem(), patchPath.Get(), "GAME"))
								{
									KeyValues* pModPlaylists = pModKV->FindKey("Playlists");
									if (pModPlaylists && pModPlaylists->FindKey(playlistName, false))
									{
										playlistStillProvided = true;
										if (playlist_debug.GetBool())
											Msg(eDLL_T::ENGINE, "Mod '%s' still provides playlist '%s', keeping it\n", forMod, playlistName);
									}
								}
								delete pModKV;
								break; // Only check first found patch file
							}
							break;
						}
					}
					ModSystem()->UnlockModList();
				}
				
				if (!playlistStillProvided)
				{
					Msg(eDLL_T::ENGINE, "Removing orphaned playlist '%s' (mod '%s' no longer provides it)\n", 
						playlistName, forMod);
					pBasePlaylists->RemoveSubKey(pPlaylist);
					hasChanges = true;
				}
			}
			// Playlists without for_mod are base game playlists, leave them alone
			
			pPlaylist = pNext;
		}
	}

	// Do the same for Gamemodes
	KeyValues* pBaseGamemodes = pBaseKV->FindKey("Gamemodes");
	if (pBaseGamemodes)
	{
		if (playlist_debug.GetBool())
			Msg(eDLL_T::ENGINE, "Checking for orphaned gamemodes in base file...\n");
		for (KeyValues* pGamemode = pBaseGamemodes->GetFirstTrueSubKey(); pGamemode; )
		{
			KeyValues* pNext = pGamemode->GetNextTrueSubKey();
			const char* gamemodeName = pGamemode->GetName();
			// Check both direct for_mod and vars/for_mod
			const char* forMod = pGamemode->GetString("for_mod", "");
			if (!forMod || !forMod[0])
			{
				// Try looking in vars subsection
				KeyValues* pVars = pGamemode->FindKey("vars");
				if (pVars)
				{
					forMod = pVars->GetString("for_mod", "");
				}
			}
			
			if (forMod && forMod[0]) // Has for_mod var
			{
				if (playlist_debug.GetBool())
					Msg(eDLL_T::ENGINE, "Found gamemode '%s' with for_mod='%s', checking if mod is enabled...\n", 
						gamemodeName, forMod);
				
				// Check if the mod is still enabled and still provides this gamemode
				bool gamemodeStillProvided = false;
				if (ModSystem()->IsEnabled())
				{
					ModSystem()->LockModList();
					FOR_EACH_VEC(ModSystem()->GetModList(), i)
					{
						CModSystem::ModInstance_t* const pMod = ModSystem()->GetModList()[i];
						if (pMod && pMod->IsEnabled() && V_strcmp(pMod->name.String(), forMod) == 0)
						{
							// Check if this mod still provides this specific gamemode
							static const char* kPatchFiles[] = {
								"playlists_r5_patch.txt",
								"playlist_r5_patch.txt"
							};

							for (int j = 0; j < Q_ARRAYSIZE(kPatchFiles); ++j)
							{
								CUtlString patchPath = pMod->GetBasePath();
								patchPath += kPatchFiles[j];

								if (!FileSystem()->FileExists(patchPath.Get(), "GAME"))
									continue;

								// Load mod's playlist patch to check if it still provides this gamemode
								KeyValues* pModKV = new KeyValues("playlists");
								if (pModKV->LoadFromFile(FileSystem(), patchPath.Get(), "GAME"))
								{
									KeyValues* pModGamemodes = pModKV->FindKey("Gamemodes");
									if (pModGamemodes && pModGamemodes->FindKey(gamemodeName, false))
									{
										gamemodeStillProvided = true;
										if (playlist_debug.GetBool())
											Msg(eDLL_T::ENGINE, "Mod '%s' still provides gamemode '%s', keeping it\n", forMod, gamemodeName);
									}
								}
								delete pModKV;
								break; // Only check first found patch file
							}
							break;
						}
					}
					ModSystem()->UnlockModList();
				}
				
				if (!gamemodeStillProvided)
				{
					Msg(eDLL_T::ENGINE, "Removing orphaned gamemode '%s' (mod '%s' no longer provides it)\n", 
						gamemodeName, forMod);
					pBaseGamemodes->RemoveSubKey(pGamemode);
					hasChanges = true;
				}
			}
			// Gamemodes without for_mod are base game gamemodes, leave them alone
			
			pGamemode = pNext;
		}
	}
	
	// Only process new mod playlists if mod system is enabled
	if (!ModSystem()->IsEnabled())
	{
		if (playlist_debug.GetBool())
			Msg(eDLL_T::ENGINE, "Mod system disabled, skipping mod playlist processing\n");
	}
	else
	{
		ModSystem()->LockModList();
		FOR_EACH_VEC(ModSystem()->GetModList(), i)
	{
		CModSystem::ModInstance_t* const pMod = ModSystem()->GetModList()[i];
		if (!pMod || !pMod->IsEnabled())
			continue;

		// Try both filename variants in mod root directory
		static const char* kPatchFiles[] = {
			"playlists_r5_patch.txt",
			"playlist_r5_patch.txt"
		};

		for (int j = 0; j < Q_ARRAYSIZE(kPatchFiles); ++j)
		{
			CUtlString patchPath = pMod->GetBasePath();
			patchPath += kPatchFiles[j];

			if (!FileSystem()->FileExists(patchPath.Get(), "GAME"))
				continue;

			// Load mod's playlist patch
			KeyValues* pModKV = new KeyValues("playlists");
			if (!pModKV->LoadFromFile(FileSystem(), patchPath.Get(), "GAME"))
			{
				delete pModKV;
				continue;
			}

			// Validate and process mod playlists
			pBasePlaylists = pBaseKV->FindKey("Playlists");
			KeyValues* pModPlaylists = pModKV->FindKey("Playlists");
			if (pBasePlaylists && pModPlaylists)
			{
				for (KeyValues* pPlaylist = pModPlaylists->GetFirstTrueSubKey(); pPlaylist; )
				{
					KeyValues* pNext = pPlaylist->GetNextTrueSubKey();
					const char* playlistName = pPlaylist->GetName();
					// Check both direct for_mod and vars/for_mod
					const char* forMod = pPlaylist->GetString("for_mod", "");
					if (!forMod || !forMod[0])
					{
						// Try looking in vars subsection
						KeyValues* pVars = pPlaylist->FindKey("vars");
						if (pVars)
						{
							forMod = pVars->GetString("for_mod", "");
						}
					}
					
					// Check if playlist has required for_mod variable
					if (!forMod || !forMod[0])
					{
						Warning(eDLL_T::ENGINE, "Mod '%s': Playlist '%s' missing required 'for_mod' variable - skipping\n", 
							pMod->name.String(), playlistName);
						pModPlaylists->RemoveSubKey(pPlaylist);
						pPlaylist = pNext;
						continue;
					}
					
					// Check if for_mod matches this mod's name
					if (V_strcmp(forMod, pMod->name.String()) != 0)
					{
						Warning(eDLL_T::ENGINE, "Mod '%s': Playlist '%s' has for_mod='%s' but should be '%s' - skipping\n", 
							pMod->name.String(), playlistName, forMod, pMod->name.String());
						pModPlaylists->RemoveSubKey(pPlaylist);
						pPlaylist = pNext;
						continue;
					}
					
					// Check if we're trying to override a base game playlist (no for_mod)
					KeyValues* pExistingPlaylist = pBasePlaylists->FindKey(playlistName, false);
					if (pExistingPlaylist)
					{
						// Check both direct for_mod and vars/for_mod for existing playlist
						const char* existingForMod = pExistingPlaylist->GetString("for_mod", "");
						if (!existingForMod || !existingForMod[0])
						{
							// Try looking in vars subsection
							KeyValues* pExistingVars = pExistingPlaylist->FindKey("vars");
							if (pExistingVars)
							{
								existingForMod = pExistingVars->GetString("for_mod", "");
							}
						}
						
						if (!existingForMod || !existingForMod[0])
						{
							// This is a base game playlist, don't override it
							Warning(eDLL_T::ENGINE, "Mod '%s': Cannot override base game playlist '%s' - skipping\n", 
								pMod->name.String(), playlistName);
							pModPlaylists->RemoveSubKey(pPlaylist);
							pPlaylist = pNext;
							continue;
						}
						else
						{
							// Remove existing mod playlist so this one replaces it
							pBasePlaylists->RemoveSubKey(pExistingPlaylist);
							Msg(eDLL_T::ENGINE, "Replacing existing mod playlist '%s' with version from mod '%s'\n", 
								playlistName, pMod->name.String());
						}
					}
					
					pPlaylist = pNext;
				}
			}

			// Validate and process mod gamemodes
			pBaseGamemodes = pBaseKV->FindKey("Gamemodes");
			KeyValues* pModGamemodes = pModKV->FindKey("Gamemodes");
			if (pBaseGamemodes && pModGamemodes)
			{
				for (KeyValues* pGamemode = pModGamemodes->GetFirstTrueSubKey(); pGamemode; )
				{
					KeyValues* pNext = pGamemode->GetNextTrueSubKey();
					const char* gamemodeName = pGamemode->GetName();
					// Check both direct for_mod and vars/for_mod
					const char* forMod = pGamemode->GetString("for_mod", "");
					if (!forMod || !forMod[0])
					{
						// Try looking in vars subsection
						KeyValues* pVars = pGamemode->FindKey("vars");
						if (pVars)
						{
							forMod = pVars->GetString("for_mod", "");
						}
					}
					
					// Check if gamemode has required for_mod variable
					if (!forMod || !forMod[0])
					{
						Warning(eDLL_T::ENGINE, "Mod '%s': Gamemode '%s' missing required 'for_mod' variable - skipping\n", 
							pMod->name.String(), gamemodeName);
						pModGamemodes->RemoveSubKey(pGamemode);
						pGamemode = pNext;
						continue;
					}
					
					// Check if for_mod matches this mod's name
					if (V_strcmp(forMod, pMod->name.String()) != 0)
					{
						Warning(eDLL_T::ENGINE, "Mod '%s': Gamemode '%s' has for_mod='%s' but should be '%s' - skipping\n", 
							pMod->name.String(), gamemodeName, forMod, pMod->name.String());
						pModGamemodes->RemoveSubKey(pGamemode);
						pGamemode = pNext;
						continue;
					}
					
					// Check if we're trying to override a base game gamemode (no for_mod)
					KeyValues* pExistingGamemode = pBaseGamemodes->FindKey(gamemodeName, false);
					if (pExistingGamemode)
					{
						// Check both direct for_mod and vars/for_mod for existing gamemode
						const char* existingForMod = pExistingGamemode->GetString("for_mod", "");
						if (!existingForMod || !existingForMod[0])
						{
							// Try looking in vars subsection
							KeyValues* pExistingVars = pExistingGamemode->FindKey("vars");
							if (pExistingVars)
							{
								existingForMod = pExistingVars->GetString("for_mod", "");
							}
						}
						
						if (!existingForMod || !existingForMod[0])
						{
							// This is a base game gamemode, don't override it
							Warning(eDLL_T::ENGINE, "Mod '%s': Cannot override base game gamemode '%s' - skipping\n", 
								pMod->name.String(), gamemodeName);
							pModGamemodes->RemoveSubKey(pGamemode);
							pGamemode = pNext;
							continue;
						}
						else
						{
							// Remove existing mod gamemode so this one replaces it
							pBaseGamemodes->RemoveSubKey(pExistingGamemode);
							Msg(eDLL_T::ENGINE, "Replacing existing mod gamemode '%s' with version from mod '%s'\n", 
								gamemodeName, pMod->name.String());
						}
					}
					
					pGamemode = pNext;
				}
			}

			// Merge the mod patch into base
			pBaseKV->RecursiveMergeKeyValues(pModKV);
			delete pModKV;
			hasChanges = true;
			
			Msg(eDLL_T::ENGINE, "Merged playlist patch from mod '%s' into base file\n", pMod->name.String());
			break; // Only use first found patch file per mod
		}
	}
	ModSystem()->UnlockModList();
	} // End mod processing block

	// Save the merged playlist back to the file if we made changes
	if (hasChanges)
	{
#if defined(CLIENT_DLL)
		if (s_pClientPlaylistFile && s_pClientPlaylistFile != pBaseKV)
			delete s_pClientPlaylistFile;
		s_pClientPlaylistFile = pBaseKV;
		pBaseKV = nullptr;

		KeyValues* const pPlaylists = s_pClientPlaylistFile->FindKey("Playlists");
		if (pPlaylists)
		{
			g_vecAllPlaylists.Purge();
			int nSafety = 0;
			for (KeyValues* pSub = pPlaylists->GetFirstTrueSubKey();
				pSub != nullptr && nSafety++ < 4096;
				pSub = pSub->GetNextTrueSubKey())
			{
				g_vecAllPlaylists.AddToTail(pSub->GetName());
			}
		}

		Msg(eDLL_T::ENGINE, "[PLAYLIST] merged mod patches in memory (no disk write)\n");
#else
		CUtlBuffer outBuf;
		pBaseKV->RecursiveSaveToFile(outBuf, 0);
		
		if (FileSystem()->WriteFile(playlistFilePath, "GAME", outBuf))
		{
			Msg(eDLL_T::ENGINE, "Successfully updated playlist file with mod patches\n");
		}
		else
		{
			Warning(eDLL_T::ENGINE, "Failed to write merged playlist file\n");
		}
#endif // !CLIENT_DLL
	}

	delete pBaseKV;
}

/*
=====================
Host_ReloadPlaylists_f
=====================
*/
static void Host_ReloadPlaylists_f()
{
	// First, merge mod playlists into the base file
	MergeModPlaylistsIntoFile();
	
	// Then reload the merged playlist file
	v_Playlists_Download_f();
}

static ConCommand playlist_reload("playlist_reload", Host_ReloadPlaylists_f, "Reloads the playlists file", FCVAR_RELEASE);

//-----------------------------------------------------------------------------
// Purpose: server-authoritative playlist var overrides. Wire nameLen < 128, valueLen < 64.
//-----------------------------------------------------------------------------
#define PLAYLIST_OVERRIDE_NAME_MAX 127
#define PLAYLIST_OVERRIDE_VALUE_MAX 63

static int Playlist_GetVarOverrideCount(void)
{
	return g_pPlaylistOverrideCount ? (int)*g_pPlaylistOverrideCount : -1;
}

static void CC_Playlist_SetVarOverride_f(const CCommand& args)
{
	if (args.ArgC() != 3)
	{
		Msg(eDLL_T::ENGINE, "usage: playlist_override_set <var> <value>\n");
		return;
	}

	if (!v_Playlist_SetVarOverride)
	{
		Warning(eDLL_T::ENGINE, "[PLO] Playlist_SetVarOverride unresolved -- overrides unavailable\n");
		return;
	}

	const char* const pszName = args.Arg(1);
	const char* const pszValue = args.Arg(2);

	if (!pszName[0] || V_strlen(pszName) > PLAYLIST_OVERRIDE_NAME_MAX)
	{
		Warning(eDLL_T::ENGINE, "[PLO] var name must be 1..%d characters\n", PLAYLIST_OVERRIDE_NAME_MAX);
		return;
	}
	if (V_strlen(pszValue) > PLAYLIST_OVERRIDE_VALUE_MAX)
	{
		Warning(eDLL_T::ENGINE, "[PLO] value must be at most %d characters\n", PLAYLIST_OVERRIDE_VALUE_MAX);
		return;
	}

	const int before = Playlist_GetVarOverrideCount();
	v_Playlist_SetVarOverride(pszName, pszValue);
	const int after = Playlist_GetVarOverrideCount();

	// A new name that did not raise the count means the engine hit its 64-entry
	// cap and dropped the write (it warns, but only into its own log channel).
	if (after == before && after >= PLAYLIST_OVERRIDE_MAX_ENTRIES)
		Warning(eDLL_T::ENGINE, "[PLO] override table full (%d) -- '%s' not added\n", after, pszName);
	else
		Msg(eDLL_T::ENGINE, "[PLO] override '%s' = '%s' (%d active)\n", pszName, pszValue, after);
}

static void CC_Playlist_ClearVarOverrides_f(const CCommand& args)
{
	NOTE_UNUSED(args);

	if (!v_Playlist_ClearVarOverrides)
	{
		Warning(eDLL_T::ENGINE, "[PLO] Playlist_ClearVarOverrides unresolved -- overrides unavailable\n");
		return;
	}

	const int before = Playlist_GetVarOverrideCount();
	v_Playlist_ClearVarOverrides();
	Msg(eDLL_T::ENGINE, "[PLO] cleared %d override(s)\n", before);
}

static void CC_Playlist_ListVarOverrides_f(const CCommand& args)
{
	NOTE_UNUSED(args);

	if (!g_pPlaylistOverrideCount || !g_pPlaylistOverrideTable)
	{
		Warning(eDLL_T::ENGINE, "[PLO] override table unresolved -- cannot list\n");
		return;
	}

	const int count = Playlist_GetVarOverrideCount();
	Msg(eDLL_T::ENGINE, "[PLO] %d playlist var override(s):\n", count);

	for (int i = 0; i < count && i < PLAYLIST_OVERRIDE_MAX_ENTRIES; i++)
	{
		const char* const pszEntry = g_pPlaylistOverrideTable + (ptrdiff_t)i * PLAYLIST_OVERRIDE_STRIDE;
		Msg(eDLL_T::ENGINE, "[PLO]   %s = %s\n", pszEntry, pszEntry + PLAYLIST_OVERRIDE_VALUE_OFFSET);
	}
}

// FCVAR_CHEAT: same remote gate as CLC_SetPlaylistVarOverride (stringcmd is untrusted).
static ConCommand playlist_override_set("playlist_override_set", CC_Playlist_SetVarOverride_f,
	"Overrides a playlist var for every connected client. Usage: playlist_override_set <var> <value>", FCVAR_RELEASE | FCVAR_CHEAT);
static ConCommand playlist_override_clear("playlist_override_clear", CC_Playlist_ClearVarOverrides_f,
	"Clears every runtime playlist var override.", FCVAR_RELEASE | FCVAR_CHEAT);
static ConCommand playlist_override_list("playlist_override_list", CC_Playlist_ListVarOverrides_f,
	"Lists the active runtime playlist var overrides.", FCVAR_RELEASE);


//-----------------------------------------------------------------------------
// Purpose: Initializes the playlist globals
//-----------------------------------------------------------------------------
void Playlists_SDKInit(void)
{
	KeyValues* pRoot = Playlists_GetRootKV();

#if defined(CLIENT_DLL)
	// VPlaylists never attaches on the S21 client; read the playlist file.
	if (!pRoot && FileSystem())
	{
		s_pClientPlaylistFile = new KeyValues("playlists");
		if (!s_pClientPlaylistFile->LoadFromFile(FileSystem(), "platform/playlists_r5_patch.txt", "GAME"))
		{
			Warning(eDLL_T::ENGINE, "[PLAYLIST] failed to load platform/playlists_r5_patch.txt\n");
			delete s_pClientPlaylistFile;
			s_pClientPlaylistFile = nullptr;
		}
		pRoot = s_pClientPlaylistFile;
	}
#endif // CLIENT_DLL

	if (pRoot)
	{
		KeyValues* pPlaylists = pRoot->FindKey("Playlists");
		if (pPlaylists)
		{
			g_vecAllPlaylists.Purge();

			for (KeyValues* pSubKey = pPlaylists->GetFirstTrueSubKey(); pSubKey != nullptr; pSubKey = pSubKey->GetNextTrueSubKey())
			{
				g_vecAllPlaylists.AddToTail(pSubKey->GetName());
			}
		}
	}

	Mod_GetAllInstalledMaps();
}

//-----------------------------------------------------------------------------
// Purpose: loads the playlists
// Input: *szPlaylist - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool Playlists_Load(const char* pszPlaylist)
{
	ThreadJoinServerJob();

	const bool bResults = v_Playlists_Load(pszPlaylist);
	Playlists_SDKInit();

	return bResults;
}

//-----------------------------------------------------------------------------
// Purpose: parses the playlists
// Input: *szPlaylist - 
// Output: true on success, false on failure
//-----------------------------------------------------------------------------
bool Playlists_Parse(const char* pszPlaylist)
{
	const bool bResult = v_Playlists_Parse(pszPlaylist);
	
	// No runtime merging needed - we modify the file directly at startup

	return bResult;
}

#if defined(CLIENT_DLL)
// Defined in engine/client/net_bridge_process.cpp -- derives S21's current-playlist name
// buffer off the SetSignonState playlist-selector call site.
extern const char* S21Bridge_GetCurrentPlaylistName(void);
#endif // CLIENT_DLL

const char* Playlists_GetCurrentName(void)
{
#if defined(CLIENT_DLL)
	return S21Bridge_GetCurrentPlaylistName();
#else
	const char* const pszCurrent = v_Playlists_GetCurrent ? v_Playlists_GetCurrent() : nullptr;
	return pszCurrent ? pszCurrent : "";
#endif // CLIENT_DLL
}

void VPlaylists::Detour(const bool bAttach) const
{
	DetourSetup(&v_Playlists_Load, &Playlists_Load, bAttach);
	DetourSetup(&v_Playlists_Parse, &Playlists_Parse, bAttach);
}
