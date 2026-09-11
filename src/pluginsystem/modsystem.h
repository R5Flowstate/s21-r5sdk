#pragma once

#include "tier1/utlhash.h"
#include "tier1/keyvalues.h"
#include "rtech/rson.h"
#include "filesystem/filesystem.h"
#include "vscript/ivscript.h"

#define MOD_BASE_DIRECTORY "mods"
#define MOD_STATUS_LIST_FILE MOD_BASE_DIRECTORY"/mods.vdf"
#define MOD_REQUIRED_LIST_FILE MOD_BASE_DIRECTORY"/required_mods.vdf"
#define MOD_ALLOWED_LIST_FILE MOD_BASE_DIRECTORY"/allowed_mods.vdf"
#define MOD_SETTINGS_FILE "mod.vdf"
#define MAX_MODS_TO_LOAD 1024
#define MOD_MAX_MANIFEST_BYTES (4 * 1024 * 1024)
#define MOD_MAX_SCAN_DEPTH 8
#define MOD_ATTESTATION_MAX_LEN 256

bool ModSystem_IsSafeRelativePath(const char* const pPath);

class CModAppSystemGroup;

class CModSystem
{
public:
	enum eModState : int8_t
	{
		UNLOADED = -1, // loading was unsuccessful (error occurred)
		LOADING,       // if mod is being loaded
		LOADED,        // if a mod has been loaded
		DISABLED,      // if disabled by user
		ENABLED,       // if enabled by user and loaded properly
	};

	struct ModInstance_t
	{
		enum eModRealm : int8_t
		{
			MOD_REALM_BOTH = 0,
			MOD_REALM_CLIENT,
			MOD_REALM_SERVER,
		};

		ModInstance_t(CModSystem* const _parentClass, const CUtlString& basePath);
		~ModInstance_t();

		bool ParseSettings();
		void ParseConVars();
		void ParseDependencies();
		void ParseLocalizationFiles();

		inline void SetState(const eModState newState) { state = newState; };

		inline bool IsLoaded() const { return state == eModState::LOADED; };
		inline bool IsEnabled() const { return state == eModState::ENABLED; };

		bool ShouldLoadPaks(const char* const targetPlaylist) const;

		inline const CUtlString& GetBasePath() const { return basePath; };
		inline CUtlString GetScriptCompileListPath() const { return basePath + GAME_SCRIPT_COMPILELIST; };

		KeyValues* GetSettingsKeyRequired(const char* settingsPath, const char* key) const;

		inline RSON::Node_t* LoadScriptCompileList(bool* const parseFailure) const
		{
			return RSON::LoadFromFile(GetScriptCompileListPath().Get(), "GAME", parseFailure);
		};

		CModSystem* parentClass;
		KeyValues* settingsKV;

		UtlHashHandle_t idHashHandle;

		eModState state = eModState::UNLOADED;
		eModRealm realm = MOD_REALM_BOTH;
		bool hasSearchPath;
		bool hasPrecompiledScripts;
		bool hasScripts;
		bool userEnabled;
		int loadOrder; // resolved execution index; mods.vdf uses m_ModList order

		CUtlVector<CUtlString> localizationFiles;
		CUtlVector<ConVar*> conVars;
		CUtlVector<CUtlString> dependencies;
		CUtlVector<CUtlString> loadAfter;

		CUtlString author;
		CUtlString name;
		CUtlString id;
		CUtlString description;
		CUtlString version;

		CUtlString basePath;
	};

	CModSystem();
	~CModSystem();

	void Init();
	void Shutdown();

	// load mod enabled/disabled status from file on disk
	void LoadModStatusList(CUtlMap<CUtlString, bool>& enabledList, CUtlVector<CUtlString>* pOrderOut = nullptr);
	void WriteModStatusList();

	// required mods list lives in a separate file
	void LoadRequiredMods();

	void LoadAllowedMods();

	bool IsEnabled() const;

	const inline CUtlVector<ModInstance_t*>& GetModList() { return m_ModList; };
	const inline CUtlVector<ModInstance_t*>& GetResolvedModList() { return m_ResolvedModList; };
	const void LockModList() { m_ModListMutex.Lock(); }
	const void UnlockModList() { m_ModListMutex.Unlock(); }

	// Required mods (read from MOD_REQUIRED_LIST_FILE)
	const inline CUtlVector<CUtlString>& GetRequiredMods() const { return m_RequiredMods; }
	inline void SetRequiredMods(const CUtlVector<CUtlString>& mods) { m_RequiredMods = mods; }

	const inline CUtlVector<CUtlString>& GetAllowedMods() const { return m_AllowedMods; }
	inline void SetAllowedMods(const CUtlVector<CUtlString>& mods) { m_AllowedMods = mods; }

	const CUtlString& GetNormalizedModID(const ModInstance_t* const mod) const;

private:
	void AssignLoadOrder(const CUtlVector<CUtlString>& statusOrder);
	void StableSortByLoadOrder(void);
	void ApplyStatusEnabled(const CUtlMap<CUtlString, bool>& enabledList);
	bool IsStatusListDirty(const CUtlVector<CUtlString>& statusOrder) const;
	void ApplyRealmFilter(void);
	void ResolveHardDependencies(void);
	void SortByDependencies(void);
	ModInstance_t* FindModById(const char* const pszId) const;
	int IndexOfModId(const char* const pszId) const;

	CUtlVector<ModInstance_t*> m_ModList;
	CUtlVector<ModInstance_t*> m_ResolvedModList;
	CUtlHash<CUtlString> m_ModIdHashMap;
	CThreadMutex m_ModListMutex;
	CUtlVector<CUtlString> m_RequiredMods;
	CUtlVector<CUtlString> m_AllowedMods;
};

extern CModSystem g_ModSystem;

FORCEINLINE CModSystem* ModSystem()
{
	return &g_ModSystem;
}

const char* ModSystem_StateToString(const CModSystem::eModState state);
const char* ModSystem_RealmToString(const CModSystem::ModInstance_t::eModRealm realm);

// FNV-1a checksum over the transmitted payload; detects corruption, not a MAC.
bool ModSystem_BuildAttestation(char* const pBuf, const size_t bufSize);
bool ModSystem_ParseAttestation(const char* const pAttestation, CUtlVector<CUtlString>& outIds,
	bool& bTruncated);
void ModSystem_ComputeMissing(const CUtlVector<CUtlString>& required, CUtlVector<CUtlString>& outMissing);
bool ModSystem_HasRequiredMods(const CUtlVector<CUtlString>& required);
