#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: Activity registration system (ACT_*, ACT_VM_*, etc.)
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/convar.h"
#include "filesystem/filesystem.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "thirdparty/detours/include/detours.h"
#include "activity.h"
#include <vector>
#include <string>

static std::vector<std::pair<std::string, int>> s_customActivities;
static bool s_initialized = false;
static int s_predefinedMaxId = -1;

int RegisterCustomActivity(const char* activityName)
{
	if (!activityName || !*activityName)
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] Cannot register empty activity name\n");
		return -1;
	}

	if (!IsActivitySystemInitialized())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] Cannot register activity '%s' - system not initialized\n", activityName);
		return -1;
	}

	const int existing = v_ActivityList_LookupByName(activityName);
	if (existing >= 0 && existing != 0xFFFF)
		return existing;

	if (s_predefinedMaxId < 0)
		s_predefinedMaxId = *g_pMaxActivityId;

	const int activityId = *g_pMaxActivityId + 1;
	if (activityId > 0x7FFF)
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] activity table full, cannot register '%s'\n", activityName);
		return -1;
	}

	v_ActivityList_RegisterActivity(activityName, static_cast<__int16>(activityId));
	s_customActivities.push_back({ activityName, activityId });
	return activityId;
}

bool IsActivitySystemInitialized()
{
	return s_initialized && g_pMaxActivityId && v_ActivityList_RegisterActivity
		&& v_ActivityList_LookupByName && *g_pMaxActivityId >= 100;
}

int GetActivityCount()
{
	if (!g_pMaxActivityId)
		return 0;
	return *g_pMaxActivityId + 1;
}

void ListAllActivities()
{
	if (!IsActivitySystemInitialized())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] System not initialized\n");
		return;
	}
	
	if (!s_customActivities.empty())
	{
		Msg(eDLL_T::ENGINE, "[ACTIVITY] Custom activities (%zu):\n", s_customActivities.size());
		for (const auto& act : s_customActivities)
			Msg(eDLL_T::ENGINE, "  ID %4d: %s\n", act.second, act.first.c_str());
	}
}

int FindActivityByName(const char* name)
{
	if (!name || !*name)
		return -1;

	for (const auto& act : s_customActivities)
	{
		if (act.first == name)
			return act.second;
	}

	if (v_ActivityList_LookupByName)
	{
		const int id = v_ActivityList_LookupByName(name);
		if (id >= 0 && id != 0xFFFF)
			return id;
	}
	return -1;
}

static char* TrimWhitespace(char* str)
{
	if (!str)
		return str;

	while (*str && (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n'))
		str++;

	if (*str == '\0')
		return str;

	char* end = str + strlen(str) - 1;
	while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
		end--;
	*(end + 1) = '\0';

	return str;
}

void ClearCustomActivities()
{
	s_customActivities.clear();
	Msg(eDLL_T::ENGINE, "[ACTIVITY] Cleared %zu custom activities from tracking\n", s_customActivities.size());
}

int LoadCustomActivitiesFromFile()
{
	// CRT, not IFileSystem: list init runs before the filesystem interface is up.
	const char* filePath = "platform/scripts/activity_types.txt";
	FILE* const pFile = fopen(filePath, "r");
	if (!pFile)
	{
		Msg(eDLL_T::ENGINE, "[ACTIVITY] No custom activity file found at %s\n", filePath);
		return 0;
	}

	char line[256];
	int count = 0;
	int lineNum = 0;
	int engineDupes = 0;

	while (fgets(line, sizeof(line), pFile))
	{
		lineNum++;

		char* trimmed = TrimWhitespace(line);

		if (!trimmed[0])
			continue;

		if (trimmed[0] == '/' && trimmed[1] == '/')
			continue;

		char* comment = strstr(trimmed, "//");
		if (comment)
			*comment = '\0';

		trimmed = TrimWhitespace(trimmed);
		if (!trimmed[0])
			continue;

		if (strncmp(trimmed, "ACT_", 4) != 0)
		{
			Warning(eDLL_T::ENGINE, "[ACTIVITY] %s:%d: Invalid name '%s' (must start with ACT_)\n",
				filePath, lineNum, trimmed);
			continue;
		}

		bool duplicate = false;
		for (const auto& act : s_customActivities)
		{
			if (act.first == trimmed)
			{
				Warning(eDLL_T::ENGINE, "[ACTIVITY] %s:%d: Duplicate '%s' - skipping\n",
					filePath, lineNum, trimmed);
				duplicate = true;
				break;
			}
		}

		if (!duplicate)
		{
			const int engineId = v_ActivityList_LookupByName(trimmed);
			if (engineId >= 0 && engineId != 0xFFFF)
			{
				duplicate = true;
				engineDupes++;
			}
		}

		if (duplicate)
			continue;

		int id = RegisterCustomActivity(trimmed);
		if (id >= 0)
			count++;
	}

	fclose(pFile);
	Msg(eDLL_T::ENGINE, "[ACTIVITY] Loaded %d custom activities from %s (skipped %d engine-duplicates)\n",
		count, filePath, engineDupes);

	return count;
}

// The engine rebuilds the activity table here on every list init, before any
// model resolves its sequence activities. Customs go in right after.
static bool s_customsAtListInit = false;

bool ActivityList_CustomsRegisteredAtListInit()
{
	return s_customsAtListInit;
}

static int s_nListGeneration = 0;
int ActivityList_Generation()
{
	return s_nListGeneration;
}

static void Hook_ActivityList_RegisterSharedActivities(void)
{
	v_ActivityList_RegisterSharedActivities();
	ClearCustomActivities();
	++s_nListGeneration;
	s_initialized = true;

	const int count = LoadCustomActivitiesFromFile();
	s_customsAtListInit = count > 0;
	Msg(eDLL_T::ENGINE, "[ACTIVITY] registered %d custom activities at list init (max id %d)\n",
		count, g_pMaxActivityId ? *g_pMaxActivityId : -1);
}

void VActivityList::GetFun(void) const
{
	// RegisterSharedActivities: the ACT_RESET / ACT_IDLE_CASUAL / ACT_IDLE run.
	const CMemory shared = Module_FindPattern(g_GameDll,
		"48 83 EC 28 33 D2 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? BA 01 00 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? BA 02 00 00 00");
	if (shared.IsValid())
	{
		shared.GetPtr(v_ActivityList_RegisterSharedActivities);
		shared.Offset(0xD).FollowNearCallSelf().GetPtr(v_ActivityList_RegisterActivity);
	}

	// Inside RegisterActivity: the max-id update (movzx eax, [max]; cmp; cmovg; mov [max], ax).
	const CMemory maxId = Module_FindPattern(g_GameDll,
		"66 89 46 02 33 C0 66 89 46 04 0F B7 05 ?? ?? ?? ?? 66 44 3B F0 66 41 0F 4F C6 66 89 05");
	if (maxId.IsValid())
		g_pMaxActivityId = maxId.Offset(0xA).ResolveRelativeAddress(3, 7).RCast<__int16*>();

	// LookupByName: symbol-table find, then the {id, symbol} table walk.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B D1 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 0F B7 D8 66 3B 59 10 73 48 66 3B 59 26 77 42")
		.GetPtr(v_ActivityList_LookupByName);

	if (!v_ActivityList_RegisterSharedActivities || !v_ActivityList_RegisterActivity || !g_pMaxActivityId || !v_ActivityList_LookupByName)
		Warning(eDLL_T::ENGINE, "[ACTIVITY] client activity list unresolved (shared=%p register=%p max=%p lookup=%p) -- custom activities disabled\n",
			v_ActivityList_RegisterSharedActivities, v_ActivityList_RegisterActivity, g_pMaxActivityId, v_ActivityList_LookupByName);
}

void VActivityList::Detour(const bool bAttach) const
{
	if (v_ActivityList_RegisterSharedActivities && v_ActivityList_RegisterActivity && g_pMaxActivityId && v_ActivityList_LookupByName)
		DetourSetup(&v_ActivityList_RegisterSharedActivities, &Hook_ActivityList_RegisterSharedActivities, bAttach);
}

static void CC_ActivityList_Dump(const CCommand& args)
{
	if (!IsActivitySystemInitialized())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] System not initialized\n");
		return;
	}
	const char* filter = (args.ArgC() > 1) ? args.Arg(1) : nullptr;
	if (filter)
	{
		Msg(eDLL_T::ENGINE, "[ACTIVITY] '%s' = %d\n", filter, v_ActivityList_LookupByName(filter));
		return;
	}
	Msg(eDLL_T::ENGINE, "[ACTIVITY] max id %d, %zu custom:\n", *g_pMaxActivityId, s_customActivities.size());
	for (const auto& act : s_customActivities)
		Msg(eDLL_T::ENGINE, "  ID %4d: %s\n", act.second, act.first.c_str());
}

static ConCommand activity_dump("activity_dump", CC_ActivityList_Dump, "List registered activities. Usage: activity_dump [filter]", FCVAR_RELEASE);

static void CC_ActivityList_Reload(const CCommand& args)
{
	if (!IsActivitySystemInitialized())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] System not initialized\n");
		return;
	}

	ClearCustomActivities();
	LoadCustomActivitiesFromFile();
}

static ConCommand activity_reload("activity_reload", CC_ActivityList_Reload, "Reload custom activities from scripts/activity_types.txt", FCVAR_RELEASE);
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: Activity registration system (ACT_*, ACT_VM_*, etc.)
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/convar.h"
#include "filesystem/filesystem.h"
#include "activity.h"
#include <vector>
#include <string>

static std::vector<std::pair<std::string, int>> s_customActivities;
static bool s_initialized = false;
static bool s_serverInitialized = false;
static int s_predefinedMaxId = -1;
static int s_predefinedMaxId_Server = -1;

int RegisterCustomActivity(const char* activityName)
{
	if (!activityName || !*activityName)
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] Cannot register empty activity name\n");
		return -1;
	}

	if (!IsActivitySystemReady())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] Cannot register activity '%s' - system not initialized "
			"(client=%d server=%d)\n", activityName, s_initialized ? 1 : 0, s_serverInitialized ? 1 : 0);
		return -1;
	}

	// Prefer server max on dedi (weapon sim / StartCustomActivity consume the
	// SERVER activity table). Client max is used when only the client list is up.
	int* pMaxId = nullptr;
	if (s_serverInitialized && g_pMaxActivityId_Server)
		pMaxId = g_pMaxActivityId_Server;
	else if (s_initialized && g_pMaxActivityId)
		pMaxId = g_pMaxActivityId;

	if (!pMaxId)
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] No max-activity-id pointer for '%s'\n", activityName);
		return -1;
	}

	if (s_predefinedMaxId < 0 && g_pMaxActivityId)
		s_predefinedMaxId = *g_pMaxActivityId;
	if (s_predefinedMaxId_Server < 0 && g_pMaxActivityId_Server)
		s_predefinedMaxId_Server = *g_pMaxActivityId_Server;

	const int activityId = *pMaxId + 1;

	// Register into every list that is live so client+server stay in lockstep
	// when both exist; on dedi only the server list is present.
	if (v_ActivityList_RegisterActivity && g_pMaxActivityId)
		v_ActivityList_RegisterActivity(activityName, activityId, 0);

	if (v_ActivityList_RegisterActivity_Server && g_pMaxActivityId_Server)
		v_ActivityList_RegisterActivity_Server(activityName, activityId, 0);

	if (!v_ActivityList_RegisterActivity && !v_ActivityList_RegisterActivity_Server)
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] No RegisterActivity entrypoint for '%s'\n", activityName);
		return -1;
	}

	s_customActivities.push_back({ activityName, activityId });
	return activityId;
}

bool IsActivitySystemInitialized()
{
	if (!s_initialized || !g_pActivityList || !g_pMaxActivityId)
		return false;

	if (*g_pMaxActivityId < 100)
		return false;

	return true;
}

bool IsActivitySystemReady()
{
	if (IsActivitySystemInitialized())
		return true;

	// S3 dedicated: client activity patterns 0-hit; server list is hardcoded
	// base+RVA and is the only live table.
	if (s_serverInitialized && g_pActivityList_Server && g_pMaxActivityId_Server
		&& *g_pMaxActivityId_Server >= 100)
		return true;

	return false;
}

int GetActivityCount()
{
	if (g_pMaxActivityId_Server && s_serverInitialized)
		return *g_pMaxActivityId_Server + 1;
	if (!g_pMaxActivityId)
		return 0;
	return *g_pMaxActivityId + 1;
}

void ListAllActivities()
{
	if (!IsActivitySystemReady())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] System not initialized\n");
		return;
	}
	
	if (!s_customActivities.empty())
	{
		Msg(eDLL_T::ENGINE, "[ACTIVITY] Custom activities (%zu):\n", s_customActivities.size());
		for (const auto& act : s_customActivities)
			Msg(eDLL_T::ENGINE, "  ID %4d: %s\n", act.second, act.first.c_str());
	}
}

int FindActivityByName(const char* name)
{
	if (!name || !*name)
		return -1;

	for (const auto& act : s_customActivities)
	{
		if (act.first == name)
			return act.second;
	}

	if (v_ActivityList_GetActivityName && g_pMaxActivityId)
	{
		const int maxId = *g_pMaxActivityId;
		for (int i = 0; i <= maxId; i++)
		{
			const char* actName = v_ActivityList_GetActivityName(i);
			if (actName && actName[0] != '\0' && strcmp(actName, name) == 0)
				return i;
		}
	}

	// Dedi: walk the SERVER activity table (only live list on r5apex_ds).
	if (v_ActivityList_GetActivityName_Server && g_pMaxActivityId_Server)
	{
		const int maxId = *g_pMaxActivityId_Server;
		for (int i = 0; i <= maxId; i++)
		{
			const char* actName = v_ActivityList_GetActivityName_Server(i);
			if (actName && actName[0] != '\0' && strcmp(actName, name) == 0)
				return i;
		}
	}

	return -1;
}

static char* TrimWhitespace(char* str)
{
	if (!str)
		return str;

	while (*str && (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n'))
		str++;

	if (*str == '\0')
		return str;

	char* end = str + strlen(str) - 1;
	while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
		end--;
	*(end + 1) = '\0';

	return str;
}

void ClearCustomActivities()
{
	s_customActivities.clear();
	Msg(eDLL_T::ENGINE, "[ACTIVITY] Cleared %zu custom activities from tracking\n", s_customActivities.size());
}

int LoadCustomActivitiesFromFile()
{
	const char* filePath = "scripts/activity_types.txt";

	if (!FileSystem())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] FileSystem not available\n");
		return 0;
	}

	FileHandle_t pFile = FileSystem()->Open(filePath, "r", "GAME");
	if (!pFile)
	{
		Msg(eDLL_T::ENGINE, "[ACTIVITY] No custom activity file found at %s\n", filePath);
		return 0;
	}

	// Snapshot the engine's pre-load max ID so the engine-duplicate check below
	// only matches engine-originals, not entries we register as we walk the file.
	// Prefer the SERVER max on dedi (client max is null there).
	const int engineMaxIdSnapshot = (g_pMaxActivityId_Server && s_serverInitialized)
		? *g_pMaxActivityId_Server
		: (g_pMaxActivityId ? *g_pMaxActivityId : -1);
	auto getEngineName = [](int id) -> const char*
	{
		if (v_ActivityList_GetActivityName_Server && g_pMaxActivityId_Server && s_serverInitialized)
			return v_ActivityList_GetActivityName_Server(id);
		if (v_ActivityList_GetActivityName)
			return v_ActivityList_GetActivityName(id);
		return nullptr;
	};

	char line[256];
	int count = 0;
	int lineNum = 0;
	int engineDupes = 0;

	while (FileSystem()->ReadLine(line, sizeof(line), pFile))
	{
		lineNum++;

		char* trimmed = TrimWhitespace(line);

		if (!trimmed[0])
			continue;

		if (trimmed[0] == '/' && trimmed[1] == '/')
			continue;

		char* comment = strstr(trimmed, "//");
		if (comment)
			*comment = '\0';

		trimmed = TrimWhitespace(trimmed);
		if (!trimmed[0])
			continue;

		if (strncmp(trimmed, "ACT_", 4) != 0)
		{
			Warning(eDLL_T::ENGINE, "[ACTIVITY] %s:%d: Invalid name '%s' (must start with ACT_)\n",
				filePath, lineNum, trimmed);
			continue;
		}

		bool duplicate = false;
		for (const auto& act : s_customActivities)
		{
			if (act.first == trimmed)
			{
				Warning(eDLL_T::ENGINE, "[ACTIVITY] %s:%d: Duplicate '%s' - skipping\n",
					filePath, lineNum, trimmed);
				duplicate = true;
				break;
			}
		}

		if (!duplicate && engineMaxIdSnapshot >= 0)
		{
			for (int i = 0; i <= engineMaxIdSnapshot; i++)
			{
				const char* engineName = getEngineName(i);
				if (engineName && engineName[0] != '\0' && strcmp(engineName, trimmed) == 0)
				{
					Warning(eDLL_T::ENGINE, "[ACTIVITY] %s:%d: '%s' already engine-registered at ID %d - skipping\n",
						filePath, lineNum, trimmed, i);
					duplicate = true;
					engineDupes++;
					break;
				}
			}
		}

		if (duplicate)
			continue;

		int id = RegisterCustomActivity(trimmed);
		if (id >= 0)
			count++;
	}

	FileSystem()->Close(pFile);
	Msg(eDLL_T::ENGINE, "[ACTIVITY] Loaded %d custom activities from %s (skipped %d engine-duplicates)\n",
		count, filePath, engineDupes);

	// A sequence's activity is stored as a NAME and resolved once per model
	// against whatever the table held at that moment. Anything registered above
	// is invisible to models that already resolved, so retire their caches.
	if (count > 0)
		InvalidateModelActivityBindings();

	return count;
}

// activity->sequence maps are built once per model name+checksum and never
// rebuilt by a version bump -- register customs before the first model resolves.
static bool s_customsAtListInit = false;

bool ActivityList_CustomsRegisteredAtListInit()
{
	return s_customsAtListInit;
}

static int s_nListGeneration = 0;
int ActivityList_Generation()
{
	return s_nListGeneration;
}

static void Hook_ActivityList_RegisterSharedActivities()
{
	v_ActivityList_RegisterSharedActivities_Server();
	++s_nListGeneration;

	// The engine just rebuilt the table, so our bookkeeping of what is already
	// registered is stale -- without the clear, the duplicate check would skip
	// every name and a level change would silently drop all customs.
	ClearCustomActivities();

	const int count = LoadCustomActivitiesFromFile();
	s_customsAtListInit = count > 0;

	Msg(eDLL_T::ENGINE, "[ACTIVITY] registered %d custom activities at list init "
		"(before any model resolves)\n", count);
}

void VActivityList::GetFun(void) const
{
	Module_FindPattern(g_GameDll, "48 89 5C 24 08 48 89 74 24 10 44 88 44 24 18 57 48 83 EC 20").GetPtr(v_ActivityList_RegisterActivity);
	Module_FindPattern(g_GameDll, "57 48 83 EC 20 44 8B 05 ?? ?? ?? ?? 33 FF 8B C7 45 85 C0").GetPtr(v_ActivityList_GetActivityName);

	if (!v_ActivityList_RegisterActivity)
		Warning(eDLL_T::ENGINE, "[ACTIVITY] ActivityList_RegisterActivity (client) not found\n");

	if (!v_ActivityList_GetActivityName)
		Warning(eDLL_T::ENGINE, "[ACTIVITY] ActivityList_GetActivityName (client) not found\n");

	const uintptr_t base = g_GameDll.GetModuleBase();
	v_ActivityList_RegisterActivity_Server = reinterpret_cast<decltype(v_ActivityList_RegisterActivity_Server)>(base + 0xB38DA0);
	v_ActivityList_GetActivityName_Server  = reinterpret_cast<decltype(v_ActivityList_GetActivityName_Server)> (base + 0xB38F21);

	// Byte signature is not unique; prove the first call is RegisterActivity_Server
	// or leave the pointer null so Detour skips loudly.
	const uintptr_t sharedFn = base + 0xB38FC0;
	const uint8_t* const pShared = reinterpret_cast<const uint8_t*>(sharedFn);
	bool sharedOk = pShared[0] == 0x48 && pShared[1] == 0x83 && pShared[2] == 0xEC
		&& pShared[3] == 0x28 && pShared[0x10] == 0xE8;
	if (sharedOk)
	{
		const int32_t rel = *reinterpret_cast<const int32_t*>(sharedFn + 0x11);
		const uintptr_t target = sharedFn + 0x15 + static_cast<intptr_t>(rel);
		sharedOk = target == reinterpret_cast<uintptr_t>(v_ActivityList_RegisterActivity_Server);
	}

	if (sharedOk)
		v_ActivityList_RegisterSharedActivities_Server =
			reinterpret_cast<decltype(v_ActivityList_RegisterSharedActivities_Server)>(sharedFn);
	else
		Warning(eDLL_T::ENGINE, "[ACTIVITY] RegisterSharedActivities failed verification at %p "
			"-- customs fall back to the late load and may not bind\n",
			reinterpret_cast<void*>(sharedFn));
}

void VActivityList::GetVar(void) const
{
	if (v_ActivityList_RegisterActivity)
	{
		CMemory func((uintptr_t)v_ActivityList_RegisterActivity);

		// lea rcx, [activity_list] at +0x19
		CMemory leaActivityList = func.Offset(0x19);
		if (*(uint8_t*)leaActivityList.GetPtr() == 0x48 &&
		    *(uint8_t*)(leaActivityList.GetPtr() + 1) == 0x8D &&
		    *(uint8_t*)(leaActivityList.GetPtr() + 2) == 0x0D)
		{
			g_pActivityList = leaActivityList.ResolveRelativeAddress(0x3, 0x7).GetPtr();
		}

		// mov rcx, [symbol_table] at +0x36
		CMemory movSymbolTable = func.Offset(0x36);
		if (*(uint8_t*)movSymbolTable.GetPtr() == 0x48 &&
		    *(uint8_t*)(movSymbolTable.GetPtr() + 1) == 0x8B &&
		    *(uint8_t*)(movSymbolTable.GetPtr() + 2) == 0x0D)
		{
			g_pActivitySymbolTable = movSymbolTable.ResolveRelativeAddress(0x3, 0x7).GetPtr();
		}

		// mov ecx, [max_id] at +0x4F
		CMemory movMaxId = func.Offset(0x4F);
		if (*(uint8_t*)movMaxId.GetPtr() == 0x8B &&
		    *(uint8_t*)(movMaxId.GetPtr() + 1) == 0x0D)
		{
			g_pMaxActivityId = movMaxId.ResolveRelativeAddress(0x2, 0x6).RCast<int*>();
		}
	}

	if (v_ActivityList_RegisterActivity && g_pActivityList && g_pMaxActivityId)
	{
		s_initialized = true;
	}
	else
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] Client system init failed: RegisterActivity=%s List=%s MaxId=%s\n",
			v_ActivityList_RegisterActivity ? "OK" : "MISSING",
			g_pActivityList ? "OK" : "MISSING",
			g_pMaxActivityId ? "OK" : "MISSING");
	}

	if (v_ActivityList_RegisterActivity_Server)
	{
		CMemory func((uintptr_t)v_ActivityList_RegisterActivity_Server);

		CMemory leaActivityList = func.Offset(0x19);
		if (*(uint8_t*)leaActivityList.GetPtr() == 0x48 &&
		    *(uint8_t*)(leaActivityList.GetPtr() + 1) == 0x8D &&
		    *(uint8_t*)(leaActivityList.GetPtr() + 2) == 0x0D)
		{
			g_pActivityList_Server = leaActivityList.ResolveRelativeAddress(0x3, 0x7).GetPtr();
		}

		CMemory movSymbolTable = func.Offset(0x36);
		if (*(uint8_t*)movSymbolTable.GetPtr() == 0x48 &&
		    *(uint8_t*)(movSymbolTable.GetPtr() + 1) == 0x8B &&
		    *(uint8_t*)(movSymbolTable.GetPtr() + 2) == 0x0D)
		{
			g_pActivitySymbolTable_Server = movSymbolTable.ResolveRelativeAddress(0x3, 0x7).GetPtr();
		}

		CMemory movMaxId = func.Offset(0x4F);
		if (*(uint8_t*)movMaxId.GetPtr() == 0x8B &&
		    *(uint8_t*)(movMaxId.GetPtr() + 1) == 0x0D)
		{
			g_pMaxActivityId_Server = movMaxId.ResolveRelativeAddress(0x2, 0x6).RCast<int*>();
		}
	}

	if (v_ActivityList_RegisterActivity_Server && g_pActivityList_Server && g_pMaxActivityId_Server)
	{
		s_serverInitialized = true;
	}
	else
	{
		g_pActivityList_Server = 0;
		g_pActivitySymbolTable_Server = 0;
		g_pMaxActivityId_Server = nullptr;
		s_serverInitialized = false;
	}

	// g_nActivityListVersion, off SelectWeightedSequenceFromModifiers' staleness gate.
	CMemory versionCmp = Module_FindPattern(g_GameDll,
		"48 8B CB 48 89 7C 24 78 E8 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 0F 8D");
	if (versionCmp)
		g_pActivityListVersion_Server = versionCmp.Offset(0xD).ResolveRelativeAddress(0x2, 0x6).RCast<int*>();

	if (!g_pActivityListVersion_Server)
		Warning(eDLL_T::ENGINE, "[ACTIVITY] g_nActivityListVersion pattern unresolved -- custom "
			"activities cannot bind on models that already resolved their sequences\n");
}

//-----------------------------------------------------------------------------
// Purpose: bump g_nActivityListVersion so models re-resolve sequence activity names.
//-----------------------------------------------------------------------------
void InvalidateModelActivityBindings()
{
	if (!g_pActivityListVersion_Server)
		return;

	const int before = *g_pActivityListVersion_Server;
	*g_pActivityListVersion_Server = before + 1;

	Msg(eDLL_T::ENGINE, "[ACTIVITY] g_nActivityListVersion %d -> %d (models re-resolve sequence "
		"activities on next use)\n", before, before + 1);
}

void VActivityList::Detour(const bool bAttach) const
{
	if (v_ActivityList_RegisterSharedActivities_Server)
		DetourSetup(&v_ActivityList_RegisterSharedActivities_Server,
			&Hook_ActivityList_RegisterSharedActivities, bAttach);
}

static void CC_ActivityList_Dump(const CCommand& args)
{
	if (!IsActivitySystemInitialized())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] System not initialized\n");
		return;
	}

	const char* filter = (args.ArgC() > 1) ? args.Arg(1) : nullptr;

	if (!v_ActivityList_GetActivityName || !g_pMaxActivityId)
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] GetActivityName not found\n");
		return;
	}

	int maxId = *g_pMaxActivityId;
	int totalCount = 0;
	int shownCount = 0;
	int customCount = 0;

	Msg(eDLL_T::ENGINE, "[ACTIVITY] Listing activities%s%s%s:\n",
		filter ? " matching '" : "", filter ? filter : "", filter ? "'" : "");
	Msg(eDLL_T::ENGINE, "--------------------------------------------\n");

	for (int i = 0; i <= maxId; i++)
	{
		const char* name = v_ActivityList_GetActivityName(i);
		if (name && name[0] != '\0' && strcmp(name, "(invalid activity index)") != 0)
		{
			totalCount++;

			bool isCustom = (s_predefinedMaxId >= 0 && i > s_predefinedMaxId);
			if (isCustom) customCount++;

			if (!filter || strstr(name, filter) != nullptr)
			{
				Msg(eDLL_T::ENGINE, "  [%3d] %s%s\n", i, name, isCustom ? " (custom)" : "");
				shownCount++;
			}
		}
	}

	Msg(eDLL_T::ENGINE, "--------------------------------------------\n");
	Msg(eDLL_T::ENGINE, "[ACTIVITY] Shown %d / %d (predefined: %d, custom: %d)\n",
		shownCount, totalCount, totalCount - customCount, customCount);
}

static ConCommand activity_dump("activity_dump", CC_ActivityList_Dump, "List registered activities. Usage: activity_dump [filter]", FCVAR_RELEASE);

static void CC_ActivityList_Reload(const CCommand& args)
{
	if (!IsActivitySystemInitialized())
	{
		Warning(eDLL_T::ENGINE, "[ACTIVITY] System not initialized\n");
		return;
	}

	ClearCustomActivities();
	LoadCustomActivitiesFromFile();
}

static ConCommand activity_reload("activity_reload", CC_ActivityList_Reload, "Reload custom activities from scripts/activity_types.txt", FCVAR_RELEASE);
#endif // CLIENT_DLL
