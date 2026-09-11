//=============================================================================//
//
// Purpose: Manage loading mods
//
//-----------------------------------------------------------------------------
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier2/fileutils.h"
#include "engine/host.h"
#include "engine/cmodel_bsp.h"
#include "rtech/rson.h"
#include "localize/localize.h"
#include "modsystem.h"

//-----------------------------------------------------------------------------
// Mod discovery file access.
//
// The S21 client's filesystem vtable diverges from the S3 layout these headers
// describe past the basic Open/Read/Size/Close block, so IsDirectory,
// FindFirstEx, LoadKeyValues and WriteFile land on unrelated slots there. On
// that product the mod tree is read from the install root directly; the dedi
// keeps using the engine filesystem.
//-----------------------------------------------------------------------------
#if defined(CLIENT_DLL)
static bool ModSys_AbsPath(const char* const pszRel, char* const pszOut, const size_t nSize)
{
	char exePath[MAX_PATH];
	if (!GetModuleFileNameA(NULL, exePath, MAX_PATH))
		return false;

	char* pSlash = strrchr(exePath, '\\');
	if (!pSlash)
		pSlash = strrchr(exePath, '/');
	if (!pSlash)
		return false;

	*pSlash = '\0';
	return _snprintf_s(pszOut, nSize, _TRUNCATE, "%s\\%s", exePath, pszRel) > 0;
}

static bool ModSys_FileExists(const char* const pszRel)
{
	char abs[MAX_PATH * 2];
	if (!ModSys_AbsPath(pszRel, abs, sizeof(abs)))
		return false;

	const DWORD attr = GetFileAttributesA(abs);
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ModSys_IsDirectory(const char* const pszRel)
{
	char abs[MAX_PATH * 2];
	if (!ModSys_AbsPath(pszRel, abs, sizeof(abs)))
		return false;

	const DWORD attr = GetFileAttributesA(abs);
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ModSys_ReadFile(const char* const pszRel, CUtlVector<char>& outBuf)
{
	outBuf.RemoveAll();

	char abs[MAX_PATH * 2];
	if (!ModSys_AbsPath(pszRel, abs, sizeof(abs)))
		return false;

	FILE* pFile = NULL;
	if (fopen_s(&pFile, abs, "rb") != 0 || !pFile)
		return false;

	if (fseek(pFile, 0, SEEK_END) != 0)
	{
		fclose(pFile);
		return false;
	}

	const long nSize = ftell(pFile);
	if (nSize <= 0 || nSize > MOD_MAX_MANIFEST_BYTES)
	{
		fclose(pFile);
		return false;
	}

	fseek(pFile, 0, SEEK_SET);
	outBuf.SetCount(static_cast<int>(nSize) + 1);

	const size_t nRead = fread(outBuf.Base(), 1, static_cast<size_t>(nSize), pFile);
	fclose(pFile);

	if (nRead == 0)
		return false;

	outBuf[static_cast<int>(nRead)] = '\0';
	outBuf.SetCountNonDestructively(static_cast<int>(nRead) + 1);
	return true;
}

static bool ModSys_WriteFile(const char* const pszRel, CUtlBuffer& buf)
{
	char abs[MAX_PATH * 2];
	if (!ModSys_AbsPath(pszRel, abs, sizeof(abs)))
		return false;

	FILE* pFile = NULL;
	if (fopen_s(&pFile, abs, "wb") != 0 || !pFile)
		return false;

	const size_t nWant = static_cast<size_t>(buf.TellPut());
	const size_t nWrote = nWant ? fwrite(buf.Base(), 1, nWant, pFile) : 0;
	fclose(pFile);
	return nWrote == nWant;
}

static KeyValues* ModSys_LoadKeyValues(const char* const pszRel)
{
	CUtlVector<char> text;
	if (!ModSys_ReadFile(pszRel, text))
		return NULL;

	KeyValues* const pKV = new KeyValues("ModSystem");
	if (!pKV->LoadFromBuffer(pszRel, text.Base(), NULL, NULL))
	{
		delete pKV;
		return NULL;
	}

	return pKV;
}

static void ModSys_FindManifestsRecursive(CUtlVector<CUtlString>& outList,
	const char* const pszRelDir, const int nDepth)
{
	if (nDepth > MOD_MAX_SCAN_DEPTH || outList.Count() >= MAX_MODS_TO_LOAD)
		return;

	char absGlob[MAX_PATH * 2];
	char relGlob[MAX_PATH];
	if (_snprintf_s(relGlob, sizeof(relGlob), _TRUNCATE, "%s\\*", pszRelDir) <= 0)
		return;
	if (!ModSys_AbsPath(relGlob, absGlob, sizeof(absGlob)))
		return;

	WIN32_FIND_DATAA findData;
	const HANDLE hFind = FindFirstFileA(absGlob, &findData);
	if (hFind == INVALID_HANDLE_VALUE)
		return;

	do
	{
		if (findData.cFileName[0] == '.')
			continue;

		// A reparse point can point anywhere; the mod tree is not a place to
		// follow one.
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			continue;

		char relChild[MAX_PATH];
		if (_snprintf_s(relChild, sizeof(relChild), _TRUNCATE, "%s/%s",
			pszRelDir, findData.cFileName) <= 0)
		{
			continue;
		}

		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			ModSys_FindManifestsRecursive(outList, relChild, nDepth + 1);
		}
		else if (V_stricmp(findData.cFileName, MOD_SETTINGS_FILE) == 0)
		{
			outList.AddToTail(relChild);
		}

	} while (FindNextFileA(hFind, &findData) && outList.Count() < MAX_MODS_TO_LOAD);

	FindClose(hFind);
}

static void ModSys_FindManifests(CUtlVector<CUtlString>& outList)
{
	ModSys_FindManifestsRecursive(outList, MOD_BASE_DIRECTORY, 0);
}
#else // CLIENT_DLL
static bool ModSys_FileExists(const char* const pszRel)
{
	return FileSystem()->FileExists(pszRel, "GAME");
}

static bool ModSys_IsDirectory(const char* const pszRel)
{
	return FileSystem()->IsDirectory(pszRel, "GAME");
}

static bool ModSys_WriteFile(const char* const pszRel, CUtlBuffer& buf)
{
	return FileSystem()->WriteFile(pszRel, "GAME", buf);
}

static KeyValues* ModSys_LoadKeyValues(const char* const pszRel)
{
	return FileSystem()->LoadKeyValues(IFileSystem::TYPE_COMMON, pszRel, "GAME");
}

static void ModSys_FindManifests(CUtlVector<CUtlString>& outList)
{
	RecursiveFindFilesMatchingName(outList,
		MOD_BASE_DIRECTORY, MOD_SETTINGS_FILE, "GAME", '/');
}
#endif // !CLIENT_DLL

// NOTE: code using this should be development only; if the convars are used by
// VGUI (i.e. attached to a slider or button), and we free them, but no longer
// add them back in, the VGUI system will crash. This is therefore considered
// an advanced function -- Possible fix: shutdown VGUI, reload mods, init VGUI?
// Currently we shutdown the mod system, init it again and then shutdown VGUI
// and all other systems.
static void ModSystem_Reload_f()
{
	ModSystem()->Shutdown();
	ModSystem()->Init();

	Mod_InitiateUserLevelModPaksReprocess(); // Make sure our level mod rpaks reload.
	Host_ReparseAllScripts(); // Reparse every script of the game.
}

static void ModSystem_EnableChanged_f(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t pUserData)
{
	NOTE_UNUSED(var);
	NOTE_UNUSED(pOldValue);
	NOTE_UNUSED(flOldValue);
	NOTE_UNUSED(pUserData);
	ModSystem_Reload_f();
}

//-----------------------------------------------------------------------------
// Console variables
//-----------------------------------------------------------------------------
static ConVar modsystem_enable("modsystem_enable", "1", FCVAR_DEVELOPMENTONLY, "Enable the modsystem", ModSystem_EnableChanged_f);
static ConVar modsystem_debug("modsystem_debug", "0", FCVAR_RELEASE, "Debug the modsystem");

//-----------------------------------------------------------------------------
// Purpose: returns the mod state as string
//-----------------------------------------------------------------------------
const char* ModSystem_StateToString(const CModSystem::eModState state)
{
	switch (state)
	{
	case CModSystem::eModState::UNLOADED: return "unloaded";
	case CModSystem::eModState::LOADING: return "loading";
	case CModSystem::eModState::LOADED: return "loaded";
	case CModSystem::eModState::DISABLED: return "disabled";
	case CModSystem::eModState::ENABLED: return "enabled";
	default: Assert(0); return "unknown";
	}
}

const char* ModSystem_RealmToString(const CModSystem::ModInstance_t::eModRealm realm)
{
	switch (realm)
	{
	case CModSystem::ModInstance_t::MOD_REALM_BOTH: return "both";
	case CModSystem::ModInstance_t::MOD_REALM_CLIENT: return "client";
	case CModSystem::ModInstance_t::MOD_REALM_SERVER: return "server";
	default: Assert(0); return "both";
	}
}

static void ModSystem_List_f()
{
	if (!ModSystem()->IsEnabled())
	{
		Msg(eDLL_T::ENGINE, "*** modsystem is disabled ***\n");
		return;
	}

	Msg(eDLL_T::ENGINE, "*** loaded mods ***\n");
	Msg(eDLL_T::ENGINE, "-------------------------------------------------------------------------------\n");
	ModSystem()->LockModList();

	int nSafety = 0;
	FOR_EACH_VEC(ModSystem()->GetModList(), i)
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			break;

		const CModSystem::ModInstance_t* const mod = ModSystem()->GetModList()[i];

		Msg(eDLL_T::ENGINE, "order: %d realm: %s hasScripts: %s\n",
			mod->loadOrder, ModSystem_RealmToString(mod->realm),
			mod->hasScripts ? "true" : "false");
		Msg(eDLL_T::ENGINE, "author: %s\n", mod->author.String());
		Msg(eDLL_T::ENGINE, "name: %s\n", mod->name.String());
		Msg(eDLL_T::ENGINE, "id: %s\n", mod->id.String());
		Msg(eDLL_T::ENGINE, "description: %s\n", mod->description.String());
		Msg(eDLL_T::ENGINE, "version: %s\n", mod->version.String());
		Msg(eDLL_T::ENGINE, "path: %s\n", mod->basePath.String());
		Msg(eDLL_T::ENGINE, "state: %s\n", ModSystem_StateToString(mod->state));
		Msg(eDLL_T::ENGINE, "-------------------------------------------------------------------------------\n");
	}

	ModSystem()->UnlockModList();
}

//-----------------------------------------------------------------------------
// Console commands
//-----------------------------------------------------------------------------
static ConCommand modsystem_reload("modsystem_reload", ModSystem_Reload_f, "Reload the modsystem", FCVAR_DEVELOPMENTONLY);
static ConCommand modsystem_list("modsystem_list", ModSystem_List_f, "Show list of mods", FCVAR_RELEASE);

static unsigned int ModSystem_HashModId(const CUtlString& s)
{
	return HashStringCaseless(s.String());
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CModSystem::CModSystem()
	: m_ModIdHashMap(MAX_MODS_TO_LOAD, 0, 0, UtlStringCompareFunc, ModSystem_HashModId)
{
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CModSystem::~CModSystem()
{
	Shutdown();
}

//-----------------------------------------------------------------------------
// Purpose: initialize the mod system
// Input  :
//-----------------------------------------------------------------------------
void CModSystem::Init()
{
	if (CommandLine()->CheckParm("-modsystem_disable"))
		return;

	if (!modsystem_enable.GetBool())
		return;

	// no mods installed, no point in initializing.
	if (!ModSys_IsDirectory(MOD_BASE_DIRECTORY))
		return;

	// mod system initializes before the first Cbuf_Execute call, which
	// executes commands/convars over the command line. we check for an
	// explicit modsystem debug flag, and set the convar from here.
	if (CommandLine()->CheckParm("-modsystem_debug"))
		modsystem_debug.SetValue(true);

	CUtlVector<CUtlString> modFileList;
	ModSys_FindManifests(modFileList);

	LockModList();

	FOR_EACH_VEC(modFileList, i)
	{
		if (i == MAX_MODS_TO_LOAD)
		{
			Error(eDLL_T::ENGINE, NO_ERROR, "Exceeded MAX_MODS_TO_LOAD; only %d out of %d mods are loaded.\n",
				MAX_MODS_TO_LOAD, modFileList.Count());
			break;
		}

		// allocate dynamically, so less memory/resources are required when
		// the vector has to grow and reallocate everything. we also got
		// a vector member in the modinstance struct, which would ultimately
		// lead into each item getting copy constructed into the mod list
		// by the 'move' constructor (CUtlVector also doesn't support being
		// nested unless its a pointer).
		CModSystem::ModInstance_t* mod = 
			new CModSystem::ModInstance_t(this, modFileList.Element(i).DirName());

		if (!mod->IsLoaded())
		{
			delete mod;
			continue;
		}

		// The mod ID gets normalized here ('SDK.BaseMod' gets turned into
		// 'SDK_BaseMod'. This ensures that we can generate unique script code
		// callbacks and anything else that might be of interest in the future.
		bool didInsert; // Mod ID's must be unique!
		const UtlHashHandle_t idHandle = m_ModIdHashMap.Insert(mod->id.Replace('.', '_'), &didInsert);

		if (!didInsert)
		{
			Error(eDLL_T::ENGINE, NO_ERROR,
				"Mod \"%s\" has ID \"%s\" that was already used by another mod; skipping...\n",
				mod->name.String(), mod->id.String());

			delete mod;
			continue;
		}

		mod->idHashHandle = idHandle;
		m_ModList.AddToTail(mod);
	}

	CUtlMap<CUtlString, bool> statusEnabled(UtlStringLessFunc);
	CUtlVector<CUtlString> statusOrder;
	LoadModStatusList(statusEnabled, &statusOrder);
	AssignLoadOrder(statusOrder);
	StableSortByLoadOrder();
	ApplyStatusEnabled(statusEnabled);
	if (IsStatusListDirty(statusOrder))
		WriteModStatusList();

	LoadRequiredMods();
	LoadAllowedMods();
	ApplyRealmFilter();
	ResolveHardDependencies();
	SortByDependencies();
	UnlockModList(); // Unlock after to make sure nothing uses it during init.
}

//-----------------------------------------------------------------------------
// Purpose: shutdown the mod system
// Input  :
//-----------------------------------------------------------------------------
void CModSystem::Shutdown()
{
	AUTO_LOCK(m_ModListMutex);
	m_ResolvedModList.RemoveAll();
	m_ModList.PurgeAndDeleteElements(); // clear all allocated mod instances.
	m_ModIdHashMap.Purge();
}

//-----------------------------------------------------------------------------
// Purpose: returns whether the mod system is enabled
//-----------------------------------------------------------------------------
bool CModSystem::IsEnabled() const
{
	return modsystem_enable.GetBool();
}

void CModSystem::ApplyStatusEnabled(const CUtlMap<CUtlString, bool>& enabledList)
{
	int nSafety = 0;
	FOR_EACH_VEC(m_ModList, i)
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			break;

		ModInstance_t* const mod = m_ModList[i];
		Assert(mod->IsLoaded());

		const bool bListed = enabledList.HasElement(mod->id) ||
			enabledList.HasElement(GetNormalizedModID(mod));

		if (!bListed)
		{
			if (modsystem_debug.GetBool())
			{
				Msg(eDLL_T::ENGINE, "Mod '%s'(\"%s\") does not exist in '%s'; enabling...\n",
					mod->name.String(), mod->id.String(), MOD_STATUS_LIST_FILE);
			}

			mod->userEnabled = true;
			mod->SetState(eModState::ENABLED);
		}
		else
		{
			bool bEnabled = false;
			if (enabledList.HasElement(mod->id))
				bEnabled = enabledList.FindElement(mod->id, false);
			else
				bEnabled = enabledList.FindElement(GetNormalizedModID(mod), false);

			mod->userEnabled = bEnabled;
			mod->SetState(mod->userEnabled ? eModState::ENABLED : eModState::DISABLED);

			if (modsystem_debug.GetBool())
			{
				Msg(eDLL_T::ENGINE, "Mod '%s'(\"%s\") exists in '%s' and is %s.\n",
					mod->name.String(), mod->id.String(), MOD_STATUS_LIST_FILE, ModSystem_StateToString(mod->state));
			}
		}
	}
}

bool CModSystem::IsStatusListDirty(const CUtlVector<CUtlString>& statusOrder) const
{
	if (m_ModList.Count() != statusOrder.Count())
		return true;

	int nSafety = 0;
	FOR_EACH_VEC(m_ModList, i)
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			return true;

		const ModInstance_t* const pMod = m_ModList[i];
		const char* const pszFileId = statusOrder[i].String();
		if (V_stricmp(pMod->id.String(), pszFileId) != 0 &&
			V_stricmp(GetNormalizedModID(pMod).String(), pszFileId) != 0)
		{
			return true;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: loads the mod status file from the disk
// Input  : &enabledList - 
//-----------------------------------------------------------------------------
void CModSystem::LoadModStatusList(CUtlMap<CUtlString, bool>& enabledList, CUtlVector<CUtlString>* pOrderOut)
{
	if (pOrderOut)
		pOrderOut->RemoveAll();

	if (!ModSys_FileExists(MOD_STATUS_LIST_FILE))
		return;

	const KeyValues* pModList = ModSys_LoadKeyValues(MOD_STATUS_LIST_FILE);

	if (!pModList)
		return;

	int nSafety = 0;
	for (KeyValues* pSubKey = pModList->GetFirstSubKey();
		pSubKey != nullptr; pSubKey = pSubKey->GetNextKey())
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			break;

		const char* const pszName = pSubKey->GetName();
		if (!pszName || !pszName[0])
			continue;

		enabledList.Insert(pszName, pSubKey->GetBool());
		if (pOrderOut)
			pOrderOut->AddToTail(pszName);
	}
}

//-----------------------------------------------------------------------------
// Purpose: loads required mods from a separate file
//-----------------------------------------------------------------------------
void CModSystem::LoadRequiredMods()
{
	m_RequiredMods.RemoveAll();
	if (!ModSys_FileExists(MOD_REQUIRED_LIST_FILE))
		return;

	const KeyValues* pReqList = ModSys_LoadKeyValues(MOD_REQUIRED_LIST_FILE);
	if (!pReqList)
		return;

	for (KeyValues* pSubKey = pReqList->GetFirstSubKey(); pSubKey != nullptr; pSubKey = pSubKey->GetNextKey())
	{
		const char* id = pSubKey->GetName();
		if (id && *id)
			m_RequiredMods.AddToTail(id);
	}
}

//-----------------------------------------------------------------------------
// Purpose: loads allowed mods from a separate file
//-----------------------------------------------------------------------------
void CModSystem::LoadAllowedMods()
{
	m_AllowedMods.RemoveAll();
	if (!ModSys_FileExists(MOD_ALLOWED_LIST_FILE))
		return;

	const KeyValues* pAllowList = ModSys_LoadKeyValues(MOD_ALLOWED_LIST_FILE);

	if (!pAllowList)
		return;

	for (KeyValues* pSubKey = pAllowList->GetFirstSubKey(); pSubKey != nullptr; pSubKey = pSubKey->GetNextKey())
	{
		const char* id = pSubKey->GetName();
		if (id && *id)
			m_AllowedMods.AddToTail(id);
	}
}

//-----------------------------------------------------------------------------
// Purpose: writes the mod status file to the disk
//-----------------------------------------------------------------------------
void CModSystem::WriteModStatusList()
{
	KeyValues kv("ModList");
	LockModList();

	int nSafety = 0;
	FOR_EACH_VEC(m_ModList, i)
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			break;

		const ModInstance_t* mod = m_ModList[i];
		bool enabled = false;

		if (mod->userEnabled)
			enabled = true;

		kv.SetBool(mod->id.String(), enabled);
	}

	UnlockModList();

	CUtlBuffer buf = CUtlBuffer(ssize_t(0), 0, CUtlBuffer::TEXT_BUFFER);
	kv.RecursiveSaveToFile(buf, 0);

	if (!ModSys_WriteFile(MOD_STATUS_LIST_FILE, buf))
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Failed to write mod status list '%s'.\n", MOD_STATUS_LIST_FILE);
		return;
	}
}

//-----------------------------------------------------------------------------
// Purpose: returns the normalized mod ID for the given mod instance
// Input  : *mod - 
// Output : normalized mod ID (i.e. SDK.BaseMod gets returned as SDK_BaseMod)
//-----------------------------------------------------------------------------
const CUtlString& CModSystem::GetNormalizedModID(const ModInstance_t* const mod) const
{
	return m_ModIdHashMap[mod->idHashHandle];
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *_parentClass - 
//          &_basePath    - 
//-----------------------------------------------------------------------------
CModSystem::ModInstance_t::ModInstance_t(CModSystem* const _parentClass, const CUtlString& _basePath)
{
	parentClass = _parentClass;
	settingsKV = nullptr;

	hasSearchPath = false;
	hasPrecompiledScripts = false;
	hasScripts = false;
	userEnabled = false;
	loadOrder = INT_MAX;
	realm = MOD_REALM_BOTH;

	basePath = _basePath;
	basePath.AppendSlash('/');

	SetState(eModState::LOADING);

	if (!ParseSettings())
	{
		SetState(eModState::UNLOADED);
		return;
	}

	// parse any additional info from mod.vdf
	ParseConVars();
	ParseLocalizationFiles();

	// add mod folder to search paths so files can be easily loaded from here
	// [rexx]: maybe this isn't ideal as the only way of finding the mod's files,
	//         as there may be name clashes in files where the engine
	//         won't really care about the input file name. it may be better to,
	//         where possible, request files by file path relative to root
	//         (i.e. including platform/mods/{mod}/)
	// [amos]: it might be better to pack core files into the VPK, and disable
	//         the filesystem cache to disk reroute to avoid the file name
	//         clashing problems, research required.
#if !defined(CLIENT_DLL)
	FileSystem()->AddSearchPath(basePath.String(), "GAME", SearchPathAdd_t::PATH_ADD_TO_TAIL);
	hasSearchPath = true;
#endif // !CLIENT_DLL

	SetState(eModState::LOADED);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CModSystem::ModInstance_t::~ModInstance_t()
{
	if (settingsKV)
		delete settingsKV;

#if !defined(CLIENT_DLL)
	if (hasSearchPath)
		FileSystem()->RemoveSearchPath(basePath.String(), "GAME");
#endif // !CLIENT_DLL

	FOR_EACH_VEC(conVars, i)
	{
		ConVar* const cvar = conVars.Element(i);
		cvar->Shutdown(); // Removes it from the linked list.

		delete cvar;
	}
}

//-----------------------------------------------------------------------------
// Purpose: returns whether pak loading should happen for current playlist
// Input  : *targetPlaylist - 
// Output : true if we should load, false otherwise
//-----------------------------------------------------------------------------
bool CModSystem::ModInstance_t::ShouldLoadPaks(const char* const targetPlaylist) const
{
	Assert(settingsKV);
	const KeyValues* const pPlaylistsKV = settingsKV->FindKey("PakLoadOnPlaylists");

	// If the pak load filter is empty or absent, return true as that means no
	// filter should be applied here.
	if (!pPlaylistsKV || !pPlaylistsKV->GetFirstSubKey())
		return true;

	for (KeyValues* pSubKey = pPlaylistsKV->GetFirstSubKey();
		pSubKey != nullptr; pSubKey = pSubKey->GetNextKey())
	{
		if (!pSubKey->GetBool())
			continue; // Pak load is disabled on this mode.

		if (V_strcmp(pSubKey->GetName(), targetPlaylist) == 0)
			return true;
	}

	// Current playlist not found in filter, paks shouldn't be loaded.
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: gets a keyvalue from settings KV, and logs an error on failure
// Input  : *settingsPath - 
//          *key          - 
// Output : pointer to KeyValues object
//-----------------------------------------------------------------------------
KeyValues* CModSystem::ModInstance_t::GetSettingsKeyRequired(
	const char* settingsPath, const char* key) const
{
	KeyValues* const pKeyValue = settingsKV->FindKey(key);

	if (!pKeyValue)
	{
		Error(eDLL_T::ENGINE, NO_ERROR,
			"Mod settings \"%s\" is missing key \"%s\" which is required; skipping...\n",
			settingsPath, key);

		return nullptr;
	}

	return pKeyValue;
}

//-----------------------------------------------------------------------------
// Purpose: gets a string value from settings KV, and logs an error on failure
// Input  : *mod          - 
//          *settingsPath - 
//          *key          - 
//          &out          - 
// Output : true on success, false otherwise
//-----------------------------------------------------------------------------
static bool ModSystem_GetSettingsKeyValueString(CModSystem::ModInstance_t* const mod, const char* settingsPath, const char* const key, CUtlString& out)
{
	KeyValues* const pKeyValue = mod->GetSettingsKeyRequired(settingsPath, key);

	if (!pKeyValue)
		return false;

	const char* const value = pKeyValue->GetString();

	if (!value[0])
	{
		Error(eDLL_T::ENGINE, NO_ERROR,
			"Mod settings \"%s\" contains key \"%s\" with no value; skipping...\n",
			settingsPath, key);

		return false;
	}

	out = value;
	return true;
}

#define MIN_MOD_ID_LENGTH 4
#define MAX_MOD_ID_LENGTH 32
#define MOD_ATTESTATION_PAYLOAD_MAX 224

static bool ModSystem_ModIDHasValidShape(const char* const pModId)
{
	if (!pModId || !pModId[0])
		return false;

	const size_t nLen = V_strlen(pModId);
	if (nLen < MIN_MOD_ID_LENGTH || nLen > MAX_MOD_ID_LENGTH)
		return false;

	const char chFirst = pModId[0];
	if (!((chFirst >= 'A' && chFirst <= 'Z') || (chFirst >= 'a' && chFirst <= 'z')))
		return false;

	const size_t nPos = strspn(pModId, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._");
	return pModId[nPos] == '\0';
}

//-----------------------------------------------------------------------------
// Purpose: make sure mod ID's are valid (version numbers for example shouldn't
//          be used inside mod ID's since we have a dedicated "version" field
//          for this. Mod ID's are also used to generate the script entry point
//          callback names, and in the future they can be used for even more
//          automation to significantly reduce the surface area for user error.
//-----------------------------------------------------------------------------
static bool ModSystem_ValidateModID(const CUtlString& modId, const char* const settingsPath)
{
	const ssize_t modIdLength = modId.Length();
	const char* const pModId = modId.String();

	if (modIdLength < MIN_MOD_ID_LENGTH)
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Mod settings \"%s\" has ID \"%s\" with length %zd, however the minimum length is %zd; skipping...\n",
			settingsPath, pModId, modIdLength, (ssize_t)MIN_MOD_ID_LENGTH);

		return false;
	}

	if (modIdLength > MAX_MOD_ID_LENGTH)
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Mod settings \"%s\" has ID \"%s\" with length %zd, however the maximum length is %zd; skipping...\n",
			settingsPath, pModId, modIdLength, (ssize_t)MAX_MOD_ID_LENGTH);

		return false;
	}

	const char chFirst = pModId[0];
	if (!((chFirst >= 'A' && chFirst <= 'Z') || (chFirst >= 'a' && chFirst <= 'z')))
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Mod settings \"%s\" has ID \"%s\" that does not start with a letter; skipping...\n",
			settingsPath, pModId);

		return false;
	}

	const size_t pos = strspn(pModId, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._");
	const bool hasInvalidChars = pModId[pos] != '\0';

	if (hasInvalidChars)
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Mod settings \"%s\" has ID \"%s\" containing disallowed characters. ID's can only contain letters, digits, periods (.) and underscores (_), and must start with a letter; skipping...\n",
			settingsPath, pModId);

		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: loads the settings KV and parses the main values
// Output : true on success, false otherwise
//-----------------------------------------------------------------------------
bool CModSystem::ModInstance_t::ParseSettings()
{
	const CUtlString settingsPath = basePath + MOD_SETTINGS_FILE;
	const char* const pSettingsPath = settingsPath.String();

	settingsKV = ModSys_LoadKeyValues(pSettingsPath);

	if (!settingsKV)
	{
		Error(eDLL_T::ENGINE, NO_ERROR,
			"Failed to parse mod settings \"%s\"; skipping...\n", pSettingsPath);
		return false;
	}

	// "author" "MyName"
	if (!ModSystem_GetSettingsKeyValueString(this, pSettingsPath, "author", author))
		return false;

	// "name" "An R5Flowstate Mod"
	if (!ModSystem_GetSettingsKeyValueString(this, pSettingsPath, "name", name))
		return false;

	// "id" "r5flowstate.TestMod"
	if (!ModSystem_GetSettingsKeyValueString(this, pSettingsPath, "id", id))
		return false;

	if (!ModSystem_ValidateModID(id, pSettingsPath))
		return false;

	// "description" "This mod does X and Y using Z"
	if (!ModSystem_GetSettingsKeyValueString(this, pSettingsPath, "description", description))
		return false;

	// "version" "1.0.0"
	if (!ModSystem_GetSettingsKeyValueString(this, pSettingsPath, "version", version))
		return false;

	const char* const pszRealm = settingsKV->GetString("realm", "both");
	if (!V_stricmp(pszRealm, "client"))
		realm = MOD_REALM_CLIENT;
	else if (!V_stricmp(pszRealm, "server"))
		realm = MOD_REALM_SERVER;
	else if (!V_stricmp(pszRealm, "both"))
		realm = MOD_REALM_BOTH;
	else
	{
		Warning(eDLL_T::MODSYSTEM, "[MOD-LOAD] '%s' has unrecognized realm '%s'; defaulting to both\n",
			id.String(), pszRealm);
		realm = MOD_REALM_BOTH;
	}

	hasScripts = ModSys_FileExists(GetScriptCompileListPath().String());

	ParseDependencies();

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: parses and registers convars listed in settings KV
//-----------------------------------------------------------------------------
void CModSystem::ModInstance_t::ParseConVars()
{
	Assert(settingsKV);
	const KeyValues* pConVars = settingsKV->FindKey("ConVars");

	if (!pConVars)
		return;

	for (KeyValues* pSubKey = pConVars->GetFirstSubKey();
		pSubKey != nullptr; pSubKey = pSubKey->GetNextKey())
	{
		const char* pszName = pSubKey->GetName();
		const char* pszFlagsString = pSubKey->GetString("flags", "NONE");
		const char* pszHelpString = pSubKey->GetString("helpText");
		const char* pszUsageString = pSubKey->GetString("usageText");

		KeyValues* pValues = pSubKey->FindKey("Values");

		const char* pszDefaultValue = "0";
		bool bMin = false;
		bool bMax = false;
		float fMin = 0.f;
		float fMax = 0.f;

		if (pValues)
		{
			pszDefaultValue = pValues->GetString("default", "0");

			// minimum cvar value
			if (pValues->FindKey("min"))
			{
				bMin = true; // has min value
				fMin = pValues->GetFloat("min", 0.f);
			}

			// maximum cvar value
			if (pValues->FindKey("max"))
			{
				bMax = true; // has max value
				fMax = pValues->GetFloat("max", 1.f);
			}
		}

		int flags;
		if (ConVar_ParseFlagString(pszFlagsString, flags, pszName))
		{
			// Engine, stub, or another mod already owns this name. The
			// listing is a create-if-absent fallback, not a second owner.
			if (g_pCVar->FindCommandBase(pszName) != nullptr)
				continue;

			ConVar* cvar = new ConVar(pszName, pszDefaultValue, flags, pszHelpString, bMin, fMin, bMax, fMax, nullptr, pszUsageString);

			if (!cvar)
			{
				// Quit as we ran out of memory.
				Error(eDLL_T::ENGINE, EXIT_FAILURE, "Failed to register ConVar \"%s\" for mod '%s'(\"%s\"); allocation failure.\n",
					pszName, name.String(), id.String());

				return;
			}

			conVars.AddToTail(cvar);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: parses hard dependencies and soft LoadAfter ids from settings KV
//-----------------------------------------------------------------------------
void CModSystem::ModInstance_t::ParseDependencies()
{
	Assert(settingsKV);

	const char* const pszKeys[2] = { "Dependencies", "LoadAfter" };
	CUtlVector<CUtlString>* const pOut[2] = { &dependencies, &loadAfter };

	for (int nKey = 0; nKey < 2; ++nKey)
	{
		const KeyValues* const pKey = settingsKV->FindKey(pszKeys[nKey]);
		if (!pKey)
			continue;

		for (KeyValues* pSubKey = pKey->GetFirstSubKey();
			pSubKey != nullptr; pSubKey = pSubKey->GetNextKey())
		{
			const char* const pszEntry = pSubKey->GetName();
			if (!ModSystem_ModIDHasValidShape(pszEntry))
			{
				Warning(eDLL_T::MODSYSTEM, "[MOD-LOAD] '%s' dropped malformed %s id '%s'\n",
					id.String(), pszKeys[nKey], pszEntry ? pszEntry : "");
				continue;
			}

			if (!pSubKey->GetBool())
				continue;

			bool bDup = false;
			FOR_EACH_VEC(*pOut[nKey], nExist)
			{
				if (!V_stricmp((*pOut[nKey])[nExist].String(), pszEntry))
				{
					bDup = true;
					break;
				}
			}

			if (bDup)
				continue;

			pOut[nKey]->AddToTail(pszEntry);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: true if a mod-provided relative path can be safely joined onto a mod
//          basePath: no absolute form, no parent walk, no separator or drive
//          smuggling. Reject, never strip.
//-----------------------------------------------------------------------------
bool ModSystem_IsSafeRelativePath(const char* const pPath)
{
	if (!pPath)
		return false;

	bool bAny = false;
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pPath); *p; ++p)
	{
		if (!V_isspace(*p))
		{
			bAny = true;
			break;
		}
	}

	if (!bAny)
		return false;

	if (pPath[0] == '/' || pPath[0] == '\\')
		return false;

	if (V_IsAbsolutePath(pPath))
		return false;

	if (Q_strstr(pPath, "..") || Q_strstr(pPath, "\\") ||
		Q_strstr(pPath, "//") || Q_strstr(pPath, ":"))
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: parses and stores localization file paths in a vector
//-----------------------------------------------------------------------------
void CModSystem::ModInstance_t::ParseLocalizationFiles()
{
	Assert(settingsKV);
	const KeyValues* pLocalizationFiles = settingsKV->FindKey("LocalizationFiles");

	if (!pLocalizationFiles)
		return;

	for (KeyValues* pSubKey = pLocalizationFiles->GetFirstSubKey();
		pSubKey != nullptr; pSubKey = pSubKey->GetNextKey())
	{
		if (!ModSystem_IsSafeRelativePath(pSubKey->GetName()))
		{
			Warning(eDLL_T::ENGINE, "Skipped localization file with unsafe path from mod '%s': '%s'\n",
				name.String(), pSubKey->GetName());
			continue;
		}

		localizationFiles.AddToTail(basePath + pSubKey->GetName());
	}
}

void CModSystem::AssignLoadOrder(const CUtlVector<CUtlString>& statusOrder)
{
	int nSafety = 0;
	FOR_EACH_VEC(m_ModList, i)
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			break;

		ModInstance_t* const pMod = m_ModList[i];
		pMod->loadOrder = INT_MAX;

		FOR_EACH_VEC(statusOrder, j)
		{
			if (j >= MAX_MODS_TO_LOAD)
				break;

			if (!V_stricmp(statusOrder[j].String(), pMod->id.String()) ||
				!V_stricmp(statusOrder[j].String(), GetNormalizedModID(pMod).String()))
			{
				pMod->loadOrder = j;
				break;
			}
		}
	}
}

void CModSystem::StableSortByLoadOrder(void)
{
	const int nCount = m_ModList.Count();
	for (int i = 1; i < nCount; ++i)
	{
		ModInstance_t* const pKey = m_ModList[i];
		int j = i - 1;

		while (j >= 0)
		{
			ModInstance_t* const pCmp = m_ModList[j];
			bool bKeyGoesBefore = false;

			if (pKey->loadOrder != pCmp->loadOrder)
				bKeyGoesBefore = pKey->loadOrder < pCmp->loadOrder;
			else
				bKeyGoesBefore = V_stricmp(pKey->id.String(), pCmp->id.String()) < 0;

			if (!bKeyGoesBefore)
				break;

			m_ModList[j + 1] = pCmp;
			--j;
		}

		m_ModList[j + 1] = pKey;
	}
}

CModSystem::ModInstance_t* CModSystem::FindModById(const char* const pszId) const
{
	const int nIndex = IndexOfModId(pszId);
	if (nIndex < 0)
		return nullptr;

	return m_ModList[nIndex];
}

int CModSystem::IndexOfModId(const char* const pszId) const
{
	if (!pszId || !pszId[0])
		return -1;

	int nSafety = 0;
	FOR_EACH_VEC(m_ModList, i)
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			break;

		const ModInstance_t* const pMod = m_ModList[i];
		if (!V_stricmp(pMod->id.String(), pszId))
			return i;
		if (!V_stricmp(GetNormalizedModID(pMod).String(), pszId))
			return i;
	}

	return -1;
}

void CModSystem::ApplyRealmFilter(void)
{
	int nSafety = 0;
	FOR_EACH_VEC(m_ModList, i)
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			break;

		ModInstance_t* const pMod = m_ModList[i];

#if defined(CLIENT_DLL)
		if (pMod->realm != ModInstance_t::MOD_REALM_SERVER)
			continue;
#else
		if (pMod->realm != ModInstance_t::MOD_REALM_CLIENT)
			continue;
#endif // !CLIENT_DLL

		if (pMod->state == eModState::DISABLED)
			continue;

		pMod->SetState(eModState::DISABLED);
		Warning(eDLL_T::MODSYSTEM, "[MOD-ORDER] '%s' disabled: realm '%s' does not apply\n",
			pMod->id.String(), ModSystem_RealmToString(pMod->realm));
	}
}

void CModSystem::ResolveHardDependencies(void)
{
	const int nCount = m_ModList.Count();
	if (nCount <= 0)
		return;

	for (int nPass = 0; nPass < nCount && nPass < MAX_MODS_TO_LOAD; ++nPass)
	{
		bool bChanged = false;

		int nSafety = 0;
		FOR_EACH_VEC(m_ModList, i)
		{
			if (++nSafety > MAX_MODS_TO_LOAD)
				break;

			ModInstance_t* const pMod = m_ModList[i];
			if (!pMod->IsEnabled())
				continue;

			FOR_EACH_VEC(pMod->dependencies, d)
			{
				const char* const pszDep = pMod->dependencies[d].String();
				const ModInstance_t* const pDep = FindModById(pszDep);
				if (pDep && pDep->IsEnabled())
					continue;

				Warning(eDLL_T::MODSYSTEM, "[MOD-ORDER] '%s' disabled: missing dependency '%s'\n",
					pMod->id.String(), pszDep);
				pMod->SetState(eModState::DISABLED);
				bChanged = true;
				break;
			}
		}

		if (!bChanged)
			break;
	}
}

static int ModSystem_CountListMatches(const CUtlVector<CUtlString>& list,
	const char* const pszId, const char* const pszNormalizedId)
{
	int nCount = 0;

	FOR_EACH_VEC(list, i)
	{
		const char* const pszEntry = list[i].String();
		if ((pszId && !V_stricmp(pszEntry, pszId)) ||
			(pszNormalizedId && pszNormalizedId[0] && !V_stricmp(pszEntry, pszNormalizedId)))
		{
			++nCount;
		}
	}

	return nCount;
}

void CModSystem::SortByDependencies(void)
{
	m_ResolvedModList.RemoveAll();

	const int nCount = m_ModList.Count();
	const int nCap = (nCount < MAX_MODS_TO_LOAD) ? nCount : MAX_MODS_TO_LOAD;
	if (nCap <= 0)
		return;

	for (int i = 0; i < nCap; ++i)
	{
		m_ResolvedModList.AddToTail(m_ModList[i]);
		m_ModList[i]->loadOrder = i;
	}

	if (nCap <= 1)
		return;

	CUtlVector<int> enabledIdx;
	for (int i = 0; i < nCap; ++i)
	{
		if (m_ModList[i]->IsEnabled())
			enabledIdx.AddToTail(i);
	}

	const int nEnabled = enabledIdx.Count();
	if (nEnabled <= 0)
		return;

	CUtlVector<int> listToEnabled;
	listToEnabled.SetCount(nCap);
	listToEnabled.FillWithValue(-1);
	for (int e = 0; e < nEnabled; ++e)
		listToEnabled[enabledIdx[e]] = e;

	CUtlVector<int> nInDegree;
	nInDegree.SetCount(nEnabled);
	nInDegree.FillWithValue(0);

	for (int e = 0; e < nEnabled; ++e)
	{
		const ModInstance_t* const pMod = m_ModList[enabledIdx[e]];

		FOR_EACH_VEC(pMod->dependencies, d)
		{
			const int nDepList = IndexOfModId(pMod->dependencies[d].String());
			if (nDepList >= 0 && nDepList < nCap && listToEnabled[nDepList] >= 0)
				nInDegree[e]++;
		}

		FOR_EACH_VEC(pMod->loadAfter, d)
		{
			const int nDepList = IndexOfModId(pMod->loadAfter[d].String());
			if (nDepList >= 0 && nDepList < nCap && listToEnabled[nDepList] >= 0)
				nInDegree[e]++;
		}
	}

	CUtlVector<int> nPlaced;
	nPlaced.SetCount(nEnabled);
	nPlaced.FillWithValue(0);

	CUtlVector<int> sortedEnabled;

	for (int nPass = 0; nPass < nEnabled; ++nPass)
	{
		int nFound = -1;

		for (int e = 0; e < nEnabled; ++e)
		{
			if (!nPlaced[e] && nInDegree[e] == 0)
			{
				nFound = e;
				break;
			}
		}

		if (nFound < 0)
			break;

		sortedEnabled.AddToTail(enabledIdx[nFound]);
		nPlaced[nFound] = 1;

		const char* const pszPlacedId = m_ModList[enabledIdx[nFound]]->id.String();
		const char* const pszPlacedNorm = GetNormalizedModID(m_ModList[enabledIdx[nFound]]).String();

		for (int e = 0; e < nEnabled; ++e)
		{
			if (nPlaced[e])
				continue;

			const ModInstance_t* const pMod = m_ModList[enabledIdx[e]];
			nInDegree[e] -= ModSystem_CountListMatches(pMod->dependencies, pszPlacedId, pszPlacedNorm);
			nInDegree[e] -= ModSystem_CountListMatches(pMod->loadAfter, pszPlacedId, pszPlacedNorm);
		}
	}

	if (sortedEnabled.Count() != nEnabled)
	{
		const char* pszCycle = "?";
		for (int e = 0; e < nEnabled; ++e)
		{
			if (!nPlaced[e])
			{
				pszCycle = m_ModList[enabledIdx[e]]->id.String();
				break;
			}
		}

		Error(eDLL_T::MODSYSTEM, NO_ERROR,
			"[MOD-ORDER] dependency cycle involving '%s'; keeping file order\n", pszCycle);
		return;
	}

	m_ResolvedModList.RemoveAll();

	for (int e = 0; e < sortedEnabled.Count(); ++e)
		m_ResolvedModList.AddToTail(m_ModList[sortedEnabled[e]]);

	for (int i = 0; i < nCap; ++i)
	{
		if (!m_ModList[i]->IsEnabled())
			m_ResolvedModList.AddToTail(m_ModList[i]);
	}

	FOR_EACH_VEC(m_ResolvedModList, i)
		m_ResolvedModList[i]->loadOrder = i;
}

static uint64_t ModSystem_HashFNV1a64(const char* const pData, const size_t nLen)
{
	uint64_t nHash = 0xCBF29CE484222325ULL;

	for (size_t i = 0; i < nLen; ++i)
	{
		nHash ^= static_cast<unsigned char>(pData[i]);
		nHash *= 0x100000001B3ULL;
	}

	return nHash;
}

static int __cdecl ModSystem_CompareCUtlStringCI(const CUtlString* pA, const CUtlString* pB)
{
	const int nCmp = V_stricmp(pA->String(), pB->String());
	if (nCmp != 0)
		return nCmp;

	return V_strcmp(pA->String(), pB->String());
}

static bool ModSystem_IsAttestationChar(const char ch)
{
	if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
		return true;
	if (ch >= '0' && ch <= '9')
		return true;

	return ch == '.' || ch == '_' || ch == ',' || ch == '+' || ch == '#' || ch == '-';
}

static bool ModSystem_IsHexChar(const char ch)
{
	if (ch >= '0' && ch <= '9')
		return true;
	if (ch >= 'a' && ch <= 'f')
		return true;
	if (ch >= 'A' && ch <= 'F')
		return true;

	return false;
}

static bool ModSystem_ParseHexU64(const char* const pHex, uint64_t& nOut)
{
	nOut = 0;

	for (int i = 0; i < 16; ++i)
	{
		const char ch = pHex[i];
		unsigned int nNibble;
		if (ch >= '0' && ch <= '9')
			nNibble = static_cast<unsigned int>(ch - '0');
		else if (ch >= 'a' && ch <= 'f')
			nNibble = static_cast<unsigned int>(ch - 'a') + 10;
		else if (ch >= 'A' && ch <= 'F')
			nNibble = static_cast<unsigned int>(ch - 'A') + 10;
		else
			return false;

		nOut = (nOut << 4) | nNibble;
	}

	return true;
}

bool ModSystem_BuildAttestation(char* const pBuf, const size_t bufSize)
{
	if (!pBuf || bufSize < 1)
		return false;

	CUtlVector<CUtlString> ids;

	if (ModSystem()->IsEnabled())
	{
		ModSystem()->LockModList();

		int nSafety = 0;
		FOR_EACH_VEC(ModSystem()->GetModList(), i)
		{
			if (++nSafety > MAX_MODS_TO_LOAD)
				break;

			const CModSystem::ModInstance_t* const pMod = ModSystem()->GetModList()[i];
			if (!pMod || !pMod->IsEnabled())
				continue;

			ids.AddToTail(ModSystem()->GetNormalizedModID(pMod));
		}

		ModSystem()->UnlockModList();
	}

	if (ids.Count() > 1)
	{
		const int nCount = ids.Count();
		for (int i = 1; i < nCount; ++i)
		{
			CUtlString key = ids[i];
			int j = i - 1;
			while (j >= 0 && ModSystem_CompareCUtlStringCI(&key, &ids[j]) < 0)
			{
				ids[j + 1] = ids[j];
				--j;
			}
			ids[j + 1] = key;
		}
	}

	CUtlString fullJoin;
	FOR_EACH_VEC(ids, i)
	{
		if (i)
			fullJoin.Append(",");
		fullJoin.Append(ids[i].String());
	}

	char szPayload[MOD_ATTESTATION_MAX_LEN];
	szPayload[0] = '\0';

	if (fullJoin.IsEmpty())
	{
		szPayload[0] = '-';
		szPayload[1] = '\0';
	}
	else if (fullJoin.Length() <= MOD_ATTESTATION_PAYLOAD_MAX)
	{
		V_snprintf(szPayload, sizeof(szPayload), "%s", fullJoin.String());
	}
	else
	{
		const char* const pJoin = fullJoin.String();
		size_t nCut = 0;

		for (size_t i = 0; pJoin[i] && i <= MOD_ATTESTATION_PAYLOAD_MAX; ++i)
		{
			if (pJoin[i] == ',')
				nCut = i;
		}

		if (nCut == 0)
		{
			szPayload[0] = ',';
			szPayload[1] = '+';
			szPayload[2] = '\0';
		}
		else
		{
			memcpy(szPayload, pJoin, nCut);
			szPayload[nCut] = ',';
			szPayload[nCut + 1] = '+';
			szPayload[nCut + 2] = '\0';
		}
	}

	const size_t nPayloadLen = V_strlen(szPayload);
	const uint64_t nHash = ModSystem_HashFNV1a64(szPayload, nPayloadLen);

	const size_t nCap = (bufSize < MOD_ATTESTATION_MAX_LEN) ? bufSize : MOD_ATTESTATION_MAX_LEN;
	const int nWritten = V_snprintf(pBuf, nCap, "%s#%016llx", szPayload, static_cast<unsigned long long>(nHash));
	if (nWritten < 0 || static_cast<size_t>(nWritten) >= nCap)
		return false;

	return true;
}

bool ModSystem_ParseAttestation(const char* const pAttestation, CUtlVector<CUtlString>& outIds,
	bool& bTruncated)
{
	outIds.RemoveAll();
	bTruncated = false;

	if (!pAttestation || !pAttestation[0])
		return false;

	const size_t nLen = V_strlen(pAttestation);
	if (nLen >= MOD_ATTESTATION_MAX_LEN)
		return false;

	for (size_t i = 0; i < nLen; ++i)
	{
		if (!ModSystem_IsAttestationChar(pAttestation[i]))
			return false;
	}

	const char* pHash = nullptr;
	for (size_t i = nLen; i > 0; --i)
	{
		if (pAttestation[i - 1] == '#')
		{
			pHash = pAttestation + (i - 1);
			break;
		}
	}

	if (!pHash || pHash == pAttestation)
		return false;

	const char* const pHex = pHash + 1;
	if (V_strlen(pHex) != 16)
		return false;

	for (int i = 0; i < 16; ++i)
	{
		if (!ModSystem_IsHexChar(pHex[i]))
			return false;
	}

	uint64_t nExpect = 0;
	if (!ModSystem_ParseHexU64(pHex, nExpect))
		return false;

	const size_t nPayloadLen = static_cast<size_t>(pHash - pAttestation);
	if (nPayloadLen == 0 || nPayloadLen >= MOD_ATTESTATION_MAX_LEN)
		return false;

	char szPayload[MOD_ATTESTATION_MAX_LEN];
	memcpy(szPayload, pAttestation, nPayloadLen);
	szPayload[nPayloadLen] = '\0';

	for (size_t i = 0; i < nPayloadLen; ++i)
	{
		if (szPayload[i] == '#')
			return false;
	}

	const uint64_t nGot = ModSystem_HashFNV1a64(szPayload, nPayloadLen);
	if (nGot != nExpect)
		return false;

	if (nPayloadLen == 1 && szPayload[0] == '-')
		return true;

	size_t nIdsLen = nPayloadLen;
	if (nIdsLen >= 2 && szPayload[nIdsLen - 2] == ',' && szPayload[nIdsLen - 1] == '+')
	{
		bTruncated = true;
		nIdsLen -= 2;
		szPayload[nIdsLen] = '\0';
	}

	if (nIdsLen == 0)
		return true;

	size_t nStart = 0;
	for (size_t i = 0; i <= nIdsLen; ++i)
	{
		if (i < nIdsLen && szPayload[i] != ',')
			continue;

		const size_t nTokLen = i - nStart;
		if (nTokLen == 0)
			return false;

		if (outIds.Count() >= MAX_MODS_TO_LOAD)
			return false;

		char szTok[MOD_ATTESTATION_MAX_LEN];
		if (nTokLen >= sizeof(szTok))
			return false;

		memcpy(szTok, szPayload + nStart, nTokLen);
		szTok[nTokLen] = '\0';

		if (!ModSystem_ModIDHasValidShape(szTok))
			return false;

		outIds.AddToTail(szTok);
		nStart = i + 1;
	}

	return true;
}

void ModSystem_ComputeMissing(const CUtlVector<CUtlString>& required, CUtlVector<CUtlString>& outMissing)
{
	outMissing.RemoveAll();

	if (required.Count() == 0)
		return;

	if (!ModSystem()->IsEnabled())
	{
		int nSafety = 0;
		FOR_EACH_VEC(required, i)
		{
			if (++nSafety > MAX_MODS_TO_LOAD)
				break;

			outMissing.AddToTail(required[i]);
		}

		return;
	}

	CUtlVector<CUtlString> haveRaw;
	CUtlVector<CUtlString> haveNorm;

	ModSystem()->LockModList();

	int nSafety = 0;
	FOR_EACH_VEC(ModSystem()->GetModList(), i)
	{
		if (++nSafety > MAX_MODS_TO_LOAD)
			break;

		const CModSystem::ModInstance_t* const pMod = ModSystem()->GetModList()[i];
		if (!pMod || !pMod->IsEnabled())
			continue;

		haveRaw.AddToTail(pMod->id);
		haveNorm.AddToTail(ModSystem()->GetNormalizedModID(pMod));
	}

	ModSystem()->UnlockModList();

	int nReqSafety = 0;
	FOR_EACH_VEC(required, r)
	{
		if (++nReqSafety > MAX_MODS_TO_LOAD)
			break;

		const char* const pszReq = required[r].String();
		bool bFound = false;

		FOR_EACH_VEC(haveRaw, i)
		{
			if (!V_stricmp(haveRaw[i].String(), pszReq))
			{
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			FOR_EACH_VEC(haveNorm, i)
			{
				if (!V_stricmp(haveNorm[i].String(), pszReq))
				{
					bFound = true;
					break;
				}
			}
		}

		if (!bFound)
			outMissing.AddToTail(required[r]);
	}
}

bool ModSystem_HasRequiredMods(const CUtlVector<CUtlString>& required)
{
	CUtlVector<CUtlString> missing;
	ModSystem_ComputeMissing(required, missing);
	return missing.Count() == 0;
}

CModSystem g_ModSystem;

