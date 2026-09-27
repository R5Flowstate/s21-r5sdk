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
#include "tier0/commandline.h"

static ConVar playlist_debug("playlist_debug", "0", FCVAR_DEVELOPMENTONLY, "Enable debug logging for playlist mod system");

KeyValues** g_pPlaylistKeyValues = nullptr; // The KeyValue for the playlist file.
char* g_pPlaylistMapToLoad = nullptr;
int64_t* g_pPlaylistOverrideCount = nullptr;
bool* g_pPlaylistOverridesDirty = nullptr;
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
	V_strcat_sized(path, "platform\\", n);
	V_strcat_sized(path, pszFile, n);
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

// Each enabled mod's playlist patch, parsed at most once per merge. The orphan
// checks run once per base entry, so reloading the file per entry is quadratic.
struct ModPatchCache_t
{
	CUtlVector<CUtlString> ids;
	CUtlVector<KeyValues*> patches;

	~ModPatchCache_t()
	{
		FOR_EACH_VEC(patches, i)
			delete patches[i];
	}

	KeyValues* Get(const CModSystem::ModInstance_t* const pMod)
	{
		FOR_EACH_VEC(ids, i)
		{
			if (!V_strcmp(ids[i].String(), pMod->id.String()))
				return patches[i];
		}

		static const char* const kPatchFiles[] = { "playlists_r5_patch.txt", "playlist_r5_patch.txt" };

		KeyValues* pLoaded = nullptr;
		for (int j = 0; j < Q_ARRAYSIZE(kPatchFiles); ++j)
		{
			CUtlString patchPath = pMod->GetBasePath();
			patchPath += kPatchFiles[j];
			if (!FileSystem()->FileExists(patchPath.Get(), "GAME"))
				continue;

			pLoaded = new KeyValues("playlists");
			if (!pLoaded->LoadFromFile(FileSystem(), patchPath.Get(), "GAME"))
			{
				delete pLoaded;
				pLoaded = nullptr;
			}
			break;
		}

		ids.AddToTail(pMod->id);
		patches.AddToTail(pLoaded);
		return pLoaded;
	}
};

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
	ModPatchCache_t patchCache;

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
						if (pMod && pMod->IsEnabled() && V_strcmp(pMod->id.String(), forMod) == 0)
						{
							KeyValues* const pModKV = patchCache.Get(pMod);
							KeyValues* const pModSection = pModKV ? pModKV->FindKey("Playlists") : nullptr;
							if (pModSection && pModSection->FindKey(playlistName, false))
							{
								playlistStillProvided = true;
								if (playlist_debug.GetBool())
									Msg(eDLL_T::ENGINE, "Mod '%s' still provides playlist '%s', keeping it\n", forMod, playlistName);
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
					pPlaylist->DeleteThis();
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
						if (pMod && pMod->IsEnabled() && V_strcmp(pMod->id.String(), forMod) == 0)
						{
							KeyValues* const pModKV = patchCache.Get(pMod);
							KeyValues* const pModSection = pModKV ? pModKV->FindKey("Gamemodes") : nullptr;
							if (pModSection && pModSection->FindKey(gamemodeName, false))
							{
								gamemodeStillProvided = true;
								if (playlist_debug.GetBool())
									Msg(eDLL_T::ENGINE, "Mod '%s' still provides gamemode '%s', keeping it\n", forMod, gamemodeName);
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
					pGamemode->DeleteThis();
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
							pMod->id.String(), playlistName);
						pModPlaylists->RemoveSubKey(pPlaylist);
						pPlaylist->DeleteThis();
						pPlaylist = pNext;
						continue;
					}
					
					// Check if for_mod matches this mod's name
					if (V_strcmp(forMod, pMod->id.String()) != 0)
					{
						Warning(eDLL_T::ENGINE, "Mod '%s': Playlist '%s' has for_mod='%s' but should be '%s' - skipping\n", 
							pMod->id.String(), playlistName, forMod, pMod->id.String());
						pModPlaylists->RemoveSubKey(pPlaylist);
						pPlaylist->DeleteThis();
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
								pMod->id.String(), playlistName);
							pModPlaylists->RemoveSubKey(pPlaylist);
							pPlaylist->DeleteThis();
							pPlaylist = pNext;
							continue;
						}
						else if (V_strcmp(existingForMod, pMod->id.String()) != 0)
						{
							Warning(eDLL_T::ENGINE, "Mod '%s': playlist '%s' belongs to mod '%s' - skipping\n",
								pMod->id.String(), playlistName, existingForMod);
							pModPlaylists->RemoveSubKey(pPlaylist);
							pPlaylist->DeleteThis();
							pPlaylist = pNext;
							continue;
						}
						else
						{
							// Remove existing mod playlist so this one replaces it
							pBasePlaylists->RemoveSubKey(pExistingPlaylist);
							pExistingPlaylist->DeleteThis();
							Msg(eDLL_T::ENGINE, "Replacing existing mod playlist '%s' with version from mod '%s'\n", 
								playlistName, pMod->id.String());
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
							pMod->id.String(), gamemodeName);
						pModGamemodes->RemoveSubKey(pGamemode);
						pGamemode->DeleteThis();
						pGamemode = pNext;
						continue;
					}
					
					// Check if for_mod matches this mod's name
					if (V_strcmp(forMod, pMod->id.String()) != 0)
					{
						Warning(eDLL_T::ENGINE, "Mod '%s': Gamemode '%s' has for_mod='%s' but should be '%s' - skipping\n", 
							pMod->id.String(), gamemodeName, forMod, pMod->id.String());
						pModGamemodes->RemoveSubKey(pGamemode);
						pGamemode->DeleteThis();
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
								pMod->id.String(), gamemodeName);
							pModGamemodes->RemoveSubKey(pGamemode);
							pGamemode->DeleteThis();
							pGamemode = pNext;
							continue;
						}
						else if (V_strcmp(existingForMod, pMod->id.String()) != 0)
						{
							Warning(eDLL_T::ENGINE, "Mod '%s': gamemode '%s' belongs to mod '%s' - skipping\n",
								pMod->id.String(), gamemodeName, existingForMod);
							pModGamemodes->RemoveSubKey(pGamemode);
							pGamemode->DeleteThis();
							pGamemode = pNext;
							continue;
						}
						else
						{
							// Remove existing mod gamemode so this one replaces it
							pBaseGamemodes->RemoveSubKey(pExistingGamemode);
							pExistingGamemode->DeleteThis();
							Msg(eDLL_T::ENGINE, "Replacing existing mod gamemode '%s' with version from mod '%s'\n", 
								gamemodeName, pMod->id.String());
						}
					}
					
					pGamemode = pNext;
				}
			}

			// A patch contributes playlists and gamemodes only; any other section
			// (KVFileOverrides and the like) would reach past its own content.
			// Only the first section of each name was validated above; a repeated one would merge unchecked.
			for (KeyValues* pSection = pModKV->GetFirstSubKey(); pSection; )
			{
				KeyValues* const pNextSection = pSection->GetNextKey();
				const char* const pszSection = pSection->GetName();
				if (!(pSection == pModPlaylists && pBasePlaylists) && !(pSection == pModGamemodes && pBaseGamemodes))
				{
					Warning(eDLL_T::ENGINE, "Mod '%s': playlist patch section '%s' is not allowed - dropped\n",
						pMod->id.String(), pszSection);
					pModKV->RemoveSubKey(pSection);
					pSection->DeleteThis();
				}
				pSection = pNextSection;
			}

			// Engines divide by max_teams as a byte.
			for (KeyValues* pSection = pModKV->GetFirstTrueSubKey(); pSection; pSection = pSection->GetNextTrueSubKey())
			{
				for (KeyValues* pEntry = pSection->GetFirstTrueSubKey(); pEntry; pEntry = pEntry->GetNextTrueSubKey())
				{
					KeyValues* const pVars = pEntry->FindKey("vars");
					KeyValues* const pMaxTeams = pVars ? pVars->FindKey("max_teams") : nullptr;
					if (!pMaxTeams)
						continue;

					const unsigned long nTeams = strtoul(pMaxTeams->GetString(), nullptr, 10);
					if (nTeams < 1 || nTeams > 255)
					{
						Warning(eDLL_T::ENGINE, "Mod '%s': '%s' max_teams '%s' out of range - dropped\n",
							pMod->id.String(), pEntry->GetName(), pMaxTeams->GetString());
						pVars->RemoveSubKey(pMaxTeams);
						pMaxTeams->DeleteThis();
					}
				}
			}

			// Merge the mod patch into base
			pBaseKV->RecursiveMergeKeyValues(pModKV);
			delete pModKV;
			hasChanges = true;
			
			Msg(eDLL_T::ENGINE, "Merged playlist patch from mod '%s' into base file\n", pMod->id.String());
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
// Purpose: reads a var from a playlist's own vars block, following its
//          inherit chain; returns nullptr when no playlist in the chain sets it
//-----------------------------------------------------------------------------
const char* Playlists_FindVar(const char* pszPlaylist, const char* pszVar)
{
	if (!pszPlaylist || !pszPlaylist[0] || !pszVar || !pszVar[0])
		return nullptr;

	KeyValues* const pRoot = Playlists_GetRootKV();
	KeyValues* const pPlaylists = pRoot ? pRoot->FindKey("Playlists") : nullptr;
	if (!pPlaylists)
		return nullptr;

	KeyValues* pPl = pPlaylists->FindKey(pszPlaylist);
	for (int depth = 0; pPl && depth < 16; depth++)
	{
		KeyValues* const pVars = pPl->FindKey("vars");
		KeyValues* const pVal = pVars ? pVars->FindKey(pszVar) : nullptr;
		if (pVal)
			return pVal->GetString();

		const char* const pszParent = pPl->GetString("inherit", "");
		if (!pszParent[0])
			break;
		pPl = pPlaylists->FindKey(pszParent);
	}

	return nullptr;
}

bool Playlists_IsSettingDeclName(const char* pszVar)
{
	return pszVar && V_strnicmp(pszVar, PLAYLIST_SETTING_PREFIX, sizeof(PLAYLIST_SETTING_PREFIX) - 1) == 0;
}

const char* Playlists_FindSettingDecl(const char* pszPlaylist, const char* pszVar)
{
	if (!pszVar || !pszVar[0] || Playlists_IsSettingDeclName(pszVar))
		return nullptr;

	char szDeclName[256];
	V_snprintf(szDeclName, sizeof(szDeclName), PLAYLIST_SETTING_PREFIX "%s", pszVar);

	const char* const pszDecl = Playlists_FindVar(pszPlaylist, szDeclName);
	return (pszDecl && pszDecl[0]) ? pszDecl : nullptr;
}

static bool Playlists_ParseNumber(const char* psz, const bool bInteger, double& out)
{
	if (!psz || !psz[0])
		return false;

	char* pEnd = nullptr;
	if (bInteger)
		out = static_cast<double>(strtoll(psz, &pEnd, 10));
	else
		out = strtod(psz, &pEnd);

	return pEnd && *pEnd == '\0' && isfinite(out);
}

//-----------------------------------------------------------------------------
// Purpose: checks a value against a setting declaration ("<type> [args]|<label>")
//-----------------------------------------------------------------------------
bool Playlists_ValidateSetting(const char* pszDecl, const char* pszValue, char* pszReason, size_t nReasonSize)
{
	char szScratch[4];
	if (!pszReason || !nReasonSize)
	{
		pszReason = szScratch;
		nReasonSize = sizeof(szScratch);
	}
	pszReason[0] = '\0';

	if (!pszDecl || !pszValue || !pszValue[0])
	{
		V_snprintf(pszReason, nReasonSize, "empty value");
		return false;
	}

	// Values are plain tokens; anything else could smuggle separators into the
	// command buffer or the wire table.
	for (const char* p = pszValue; *p; p++)
	{
		const char c = *p;
		if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-' && c != '+')
		{
			V_snprintf(pszReason, nReasonSize, "value has a character outside [A-Za-z0-9_.+-]");
			return false;
		}
	}

	char szSpec[256];
	V_strncpy(szSpec, pszDecl, sizeof(szSpec));
	if (char* const pBar = strchr(szSpec, '|'))
		*pBar = '\0';

	const char* apszTok[32];
	int nTok = 0;
	char* pCtx = nullptr;
	for (char* pTok = strtok_s(szSpec, " \t", &pCtx); pTok && nTok < 32; pTok = strtok_s(nullptr, " \t", &pCtx))
		apszTok[nTok++] = pTok;

	if (nTok == 0)
	{
		V_snprintf(pszReason, nReasonSize, "declaration has no type");
		return false;
	}

	const char* const pszType = apszTok[0];

	if (!V_stricmp(pszType, "bool"))
	{
		if (!V_strcmp(pszValue, "0") || !V_strcmp(pszValue, "1"))
			return true;
		V_snprintf(pszReason, nReasonSize, "expected 0 or 1");
		return false;
	}

	if (!V_stricmp(pszType, "choice"))
	{
		for (int i = 1; i < nTok; i++)
		{
			if (!V_strcmp(apszTok[i], pszValue))
				return true;
		}
		V_snprintf(pszReason, nReasonSize, "not one of the declared choices");
		return false;
	}

	const bool bInteger = !V_stricmp(pszType, "int");
	if (!bInteger && V_stricmp(pszType, "float"))
	{
		V_snprintf(pszReason, nReasonSize, "unknown setting type '%s'", pszType);
		return false;
	}

	double flMin, flMax, flValue;
	if (nTok < 3 || !Playlists_ParseNumber(apszTok[1], bInteger, flMin) || !Playlists_ParseNumber(apszTok[2], bInteger, flMax))
	{
		V_snprintf(pszReason, nReasonSize, "declaration needs '%s <min> <max>'", pszType);
		return false;
	}
	if (!Playlists_ParseNumber(pszValue, bInteger, flValue))
	{
		V_snprintf(pszReason, nReasonSize, "expected %s", bInteger ? "an integer" : "a number");
		return false;
	}
	if (flValue < flMin || flValue > flMax)
	{
		V_snprintf(pszReason, nReasonSize, "outside %s..%s", apszTok[1], apszTok[2]);
		return false;
	}

	return true;
}

#if !defined(CLIENT_DLL)
//-----------------------------------------------------------------------------
// Purpose: "Label value, Label value" for the active overrides that are declared
//          settings of the current playlist; empty when there are none
//-----------------------------------------------------------------------------
void Playlists_DescribeActiveSettings(char* pszOut, const size_t nOutSize)
{
	if (!pszOut || !nOutSize)
		return;
	pszOut[0] = '\0';

	if (!g_pPlaylistOverrideCount || !g_pPlaylistOverrideTable)
		return;

	const char* const pszPlaylist = Playlists_GetCurrentName();
	const int64_t count = *g_pPlaylistOverrideCount;
	size_t used = 0;

	for (int64_t i = 0; i < count && i < PLAYLIST_OVERRIDE_MAX_ENTRIES; i++)
	{
		const char* const pszName = g_pPlaylistOverrideTable + i * PLAYLIST_OVERRIDE_STRIDE;
		const char* const pszValue = pszName + PLAYLIST_OVERRIDE_VALUE_OFFSET;
		const char* const pszDecl = Playlists_FindSettingDecl(pszPlaylist, pszName);
		if (!pszDecl)
			continue;

		const char* const pszBar = strchr(pszDecl, '|');
		const char* const pszLabel = (pszBar && pszBar[1]) ? pszBar + 1 : pszName;
		const bool bBool = !V_strnicmp(pszDecl, "bool", 4);
		const char* const pszShown = bBool ? (!V_strcmp(pszValue, "1") ? "on" : "off") : pszValue;

		const int n = V_snprintf(pszOut + used, nOutSize - used, "%s%s %s", used ? ", " : "", pszLabel, pszShown);
		if (n < 0 || used + n >= nOutSize)
		{
			pszOut[used] = '\0';
			break;
		}
		used += n;
	}
}
#endif // !CLIENT_DLL

//-----------------------------------------------------------------------------
// Purpose: server-authoritative playlist var overrides. Wire nameLen < 128, valueLen < 64.
//-----------------------------------------------------------------------------
#define PLAYLIST_OVERRIDE_NAME_MAX 127
#define PLAYLIST_OVERRIDE_VALUE_MAX 63

static int Playlist_GetVarOverrideCount(void)
{
	return g_pPlaylistOverrideCount ? (int)*g_pPlaylistOverrideCount : -1;
}

//-----------------------------------------------------------------------------
// Purpose: writes an override into the engine table. The engine helper only
//          authors once the server is active; before that it sends a clc to a
//          server that does not exist yet and drops the write, which loses every
//          launch-time override. The table survives level load, so write it here.
//-----------------------------------------------------------------------------
static void Playlist_AuthorVarOverride(const char* const pszName, const char* const pszValue)
{
#if !defined(CLIENT_DLL)
	if (g_pPlaylistOverrideCount && g_pPlaylistOverrideTable && g_pPlaylistOverridesDirty)
	{
		const int64_t count = *g_pPlaylistOverrideCount;
		char* pszEntry = nullptr;

		for (int64_t i = 0; i < count && i < PLAYLIST_OVERRIDE_MAX_ENTRIES; i++)
		{
			char* const pszSlot = g_pPlaylistOverrideTable + i * PLAYLIST_OVERRIDE_STRIDE;
			if (V_stricmp(pszSlot, pszName) == 0)
			{
				pszEntry = pszSlot;
				break;
			}
		}

		if (!pszEntry)
		{
			if (count < 0 || count >= PLAYLIST_OVERRIDE_MAX_ENTRIES)
				return;

			pszEntry = g_pPlaylistOverrideTable + count * PLAYLIST_OVERRIDE_STRIDE;
			V_strncpy(pszEntry, pszName, PLAYLIST_OVERRIDE_VALUE_OFFSET);
			*g_pPlaylistOverrideCount = count + 1;
		}

		V_strncpy(pszEntry + PLAYLIST_OVERRIDE_VALUE_OFFSET, pszValue, PLAYLIST_OVERRIDE_STRIDE - PLAYLIST_OVERRIDE_VALUE_OFFSET);
		*g_pPlaylistOverridesDirty = true;
		return;
	}
#endif // !CLIENT_DLL

	v_Playlist_SetVarOverride(pszName, pszValue);
}

static void Playlist_ApplyVarOverride(const char* const pszName, const char* const pszValue)
{
	if (!v_Playlist_SetVarOverride)
	{
		Warning(eDLL_T::ENGINE, "[PLO] Playlist_SetVarOverride unresolved -- overrides unavailable\n");
		return;
	}

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

	if (Playlists_IsSettingDeclName(pszName))
	{
		Warning(eDLL_T::ENGINE, "[PLO] '%s' is a setting declaration and cannot be overridden\n", pszName);
		return;
	}

	const char* const pszPlaylist = Playlists_GetCurrentName();
	const char* const pszDecl = Playlists_FindSettingDecl(pszPlaylist, pszName);
	if (pszDecl)
	{
		char szReason[128];
		if (!Playlists_ValidateSetting(pszDecl, pszValue, szReason, sizeof(szReason)))
		{
			Warning(eDLL_T::ENGINE, "[PLO] '%s' = '%s' rejected for playlist '%s': %s\n", pszName, pszValue, pszPlaylist, szReason);
			return;
		}
	}
	else if (pszPlaylist[0])
	{
		Warning(eDLL_T::ENGINE, "[PLO] '%s' is not a declared setting of playlist '%s'; the server uses it, "
			"clients only if their bridge_playlist_override_allow lists it\n", pszName, pszPlaylist);
	}

	const int before = Playlist_GetVarOverrideCount();
	Playlist_AuthorVarOverride(pszName, pszValue);
	const int after = Playlist_GetVarOverrideCount();

	// A new name that did not raise the count means the engine hit its 64-entry
	// cap and dropped the write (it warns, but only into its own log channel).
	if (after == before && after >= PLAYLIST_OVERRIDE_MAX_ENTRIES)
		Warning(eDLL_T::ENGINE, "[PLO] override table full (%d) -- '%s' not added\n", after, pszName);
	else
		Msg(eDLL_T::ENGINE, "[PLO] override '%s' = '%s' (%d active)\n", pszName, pszValue, after);
}

//-----------------------------------------------------------------------------
// Purpose: the engine runs every "+playlist_override_set" launch token with the
//          single token after the FIRST occurrence, so each run sees the first
//          var name and no value. Apply every pair from the launch line once.
//-----------------------------------------------------------------------------
static bool Playlist_ApplyLaunchOverrides(const char* const pszName)
{
	static bool s_bApplied = false;
	if (!CommandLine() || !pszName || !pszName[0])
		return false;

	const int nParms = CommandLine()->ParmCount();
	bool bNamed = false;
	for (int i = 0; i + 1 < nParms; i++)
	{
		const char* const pszParm = CommandLine()->GetParm(i);
		const char* const pszParmName = CommandLine()->GetParm(i + 1);
		if (pszParm && pszParmName && V_stricmp(pszParm, "+playlist_override_set") == 0 && V_stricmp(pszParmName, pszName) == 0)
		{
			bNamed = true;
			break;
		}
	}
	if (!bNamed)
		return false;
	if (s_bApplied)
		return true;
	s_bApplied = true;

	for (int i = 0; i + 2 < nParms; i++)
	{
		const char* const pszParm = CommandLine()->GetParm(i);
		if (!pszParm || V_stricmp(pszParm, "+playlist_override_set") != 0)
			continue;

		const char* const pszParmName = CommandLine()->GetParm(i + 1);
		const char* const pszValue = CommandLine()->GetParm(i + 2);
		if (!pszParmName || !pszValue || pszParmName[0] == '+' || pszValue[0] == '+')
		{
			Warning(eDLL_T::ENGINE, "[PLO] launch-line override at token %d has no <var> <value> pair\n", i);
			continue;
		}

		Playlist_ApplyVarOverride(pszParmName, pszValue);
	}
	return true;
}

static void CC_Playlist_SetVarOverride_f(const CCommand& args)
{
	if (args.ArgC() == 2 && Playlist_ApplyLaunchOverrides(args.Arg(1)))
		return;

	if (args.ArgC() != 3)
	{
		Msg(eDLL_T::ENGINE, "usage: playlist_override_set <var> <value>\n");
		return;
	}

	Playlist_ApplyVarOverride(args.Arg(1), args.Arg(2));
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

			// Same cap as the other fill path: the browser rescans this list every frame.
			int nSafety = 0;
			for (KeyValues* pSubKey = pPlaylists->GetFirstTrueSubKey(); pSubKey != nullptr && nSafety++ < 4096; pSubKey = pSubKey->GetNextTrueSubKey())
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
