//=============================================================================//
// Purpose: PrecacheSkinName / PrecacheOnDemandLoadModel natives.
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "public/tier0/memaddr.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/include/sqarray.h"
#include "vscript/languages/squirrel_re/include/sqstring.h"
#include "game/server/entitylist.h"
#include "game/server/baseentity.h"
#include "game/shared/weapon_script_vars.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/dt_extend.h"
#include "game/server/zipline_extend_state.h"
#include "game/shared/edict_dirty.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "precache_natives.h"
#include "game/server/gameinterface.h"
#include "engine/server/snapshot_diag.h"
#include "engine/server/snapshot_diag.h"
#include "engine/modelloader.h"
#include "engine/server/zipline_validation.h"
#include "engine/enginetrace.h"
#include "engine/host_state.h"
#include "engine/server/sv_rcon_launcher.h"
#include "game/server/vscript_server.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <vector>

//-----------------------------------------------------------------------------
// Convars
//-----------------------------------------------------------------------------

ConVar sdk_skinnames_autopopulate(
	"sdk_skinnames_autopopulate", "1", FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Auto-populate the replicated SkinNames stringtable with the known "
	"launch roster of legend classic skins immediately after table "
	"creation. 0 = leave empty (diag), 1 = populate (default).");

//-----------------------------------------------------------------------------
// Container vt[3] FindTable; table vt[8] AddString(..., userdata). vt[9] does not insert.
//-----------------------------------------------------------------------------
using FindTable_t = void* (__fastcall*)(void* container, const char* tableName);
using AddString_t = int   (__fastcall*)(void* table, char bIsServer,
										const char* value, int length,
										const void* userdata);

// Server stringtable container singleton (CreateNetworkStringTables).
static constexpr uintptr_t kStringTableContainerServer_RVA = 0xD4ECBA0;

//-----------------------------------------------------------------------------
// Cached SkinNames pointer; generation-keyed so a changelevel cannot UAF.
//-----------------------------------------------------------------------------
static std::atomic<void*> s_pSkinNamesTable{nullptr};
static std::atomic<unsigned int> s_skinTableGeneration{0};
static std::mutex         s_resolveMu;

static std::atomic<uint64_t> s_addCount{0};
static std::atomic<uint64_t> s_addFailCount{0};

//-----------------------------------------------------------------------------
// Cached modelprecache pointer; generation-keyed. GetModelIndex walks this table.
//-----------------------------------------------------------------------------
static std::atomic<void*> s_pModelPrecacheTable{nullptr};
static std::atomic<unsigned int> s_mpTableGeneration{0};
static std::mutex         s_mpResolveMu;

// Reject null, sentinel (~0), and non-canonical table ptrs. Do not SEH-swallow:
// a silent catch loops AVs across changelevel.
static inline bool IsPlausibleHeapPtr(const void* p)
{
	const uintptr_t u = reinterpret_cast<uintptr_t>(p);
	if (!u || u == ~uintptr_t{0}) return false;
	if ((u & 0xFFFF000000000000ULL) != 0) return false; // non-canonical/kernel
	if ((u & 0x7ULL) != 0) return false;                // unaligned (vtable ptrs are 8-byte)
	return true;
}

static void* ResolveModelPrecacheTable_Locked()
{
	const unsigned int curGen = ServerGameDLL_GetLevelGeneration();
	void* cached = s_pModelPrecacheTable.load(std::memory_order_acquire);
	const unsigned int cachedGen =
		s_mpTableGeneration.load(std::memory_order_acquire);

	if (cached && cachedGen == curGen)
		return cached;

	if (cached)
	{
		// Invalidate inside the lock so two threads cannot race a half-cleared cache.
		static std::atomic<unsigned int> s_mpStaleWarnedGen{0};
		if (s_mpStaleWarnedGen.exchange(curGen, std::memory_order_acq_rel) != curGen)
		{
			Warning(eDLL_T::SERVER,
				"[s21-dedi] modelprecache table cache was stale (gen %u -> %u) -- "
				"re-resolving; a level transition skipped the SDK reset\n",
				cachedGen, curGen);
		}
		s_pModelPrecacheTable.store(nullptr, std::memory_order_release);
	}

	const uintptr_t base =
		static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	void** const containerSlot =
		reinterpret_cast<void**>(base + kStringTableContainerServer_RVA);
	void* const container = *containerSlot;
	if (!IsPlausibleHeapPtr(container)) return nullptr;

	void** const vtable = *reinterpret_cast<void***>(container);
	if (!IsPlausibleHeapPtr(vtable)) return nullptr;
	const FindTable_t FindTable =
		reinterpret_cast<FindTable_t>(vtable[3]);
	if (!IsPlausibleHeapPtr(reinterpret_cast<void*>(FindTable))) return nullptr;

	void* const table = FindTable(container, "modelprecache");
	if (!IsPlausibleHeapPtr(table)) return nullptr;
	s_pModelPrecacheTable.store(table, std::memory_order_release);
	s_mpTableGeneration.store(ServerGameDLL_GetLevelGeneration(),
		std::memory_order_release);
	return table;
}

static void* ResolveModelPrecacheTable()
{
	void* cached = s_pModelPrecacheTable.load(std::memory_order_acquire);
	if (cached)
	{
		const unsigned int curGen = ServerGameDLL_GetLevelGeneration();
		if (s_mpTableGeneration.load(std::memory_order_acquire) == curGen)
			return cached;
	}

	std::lock_guard<std::mutex> lk(s_mpResolveMu);
	return ResolveModelPrecacheTable_Locked();
}

bool PrecacheModel_IsModelPrecacheReady()
{
	return ResolveModelPrecacheTable() != nullptr;
}

//-----------------------------------------------------------------------------
// Replicated modelprecache AddString.
//-----------------------------------------------------------------------------
static std::atomic<uint64_t> s_mpAddCount{0};

// Dedupe no-userdata adds. ODL userdata blobs always pass through.
static std::mutex                s_mpSeenMu;
static std::unordered_set<std::string> s_mpSeenNames;
static std::mutex                s_mpPendingMu;
static std::unordered_set<std::string> s_mpPendingNames;
static std::atomic<bool>         s_mpFlushingPending{false};

// lock_guard cannot share a function with __try (MSVC).
static bool MarkAndCheckSeen(const char* modelName)
{
	std::lock_guard<std::mutex> lk(s_mpSeenMu);
	return s_mpSeenNames.emplace(modelName).second;
}

// Own lock_guard: must not share a function with __try.
static std::atomic<unsigned int> s_mpSeenGeneration{0};

static void ClearSeenNamesIfGenerationMoved(void)
{
	const unsigned int curGen = ServerGameDLL_GetLevelGeneration();
	if (s_mpSeenGeneration.load(std::memory_order_acquire) == curGen)
		return;

	std::lock_guard<std::mutex> lk(s_mpSeenMu);
	if (s_mpSeenGeneration.load(std::memory_order_acquire) == curGen)
		return;
	s_mpSeenNames.clear();
	s_mpSeenGeneration.store(curGen, std::memory_order_release);
}

static void QueuePendingReplicatedModel(const char* modelName)
{
	if (!modelName || !*modelName) return;

	std::lock_guard<std::mutex> lk(s_mpPendingMu);
	s_mpPendingNames.emplace(modelName);
}

static void FlushPendingReplicatedModels();

// 0xB0BA blob: [0] magic, [4] pool_index, [8] pak_path, then placeholder. Pool 2 = SKINS.
static int BuildModelPrecacheUserData(char* buf, size_t bufSize,
									   int poolIndex,
									   const char* pakPath,
									   const char* placeholder)
{
	if (!buf || bufSize < 12) return 0;

	const size_t pakLen = pakPath ? strlen(pakPath) : 0;
	const size_t phLen  = placeholder ? strlen(placeholder) : 0;
	const size_t needed = 8 + pakLen + 1 + phLen + 1;
	if (needed > bufSize) return 0;

	*reinterpret_cast<uint32_t*>(buf)     = 0xB0BA;
	*reinterpret_cast<uint32_t*>(buf + 4) =
		static_cast<uint32_t>(poolIndex);
	if (pakLen)
		memcpy(buf + 8, pakPath, pakLen);
	buf[8 + pakLen] = '\0';
	if (phLen)
		memcpy(buf + 8 + pakLen + 1, placeholder, phLen);
	buf[8 + pakLen + 1 + phLen] = '\0';
	return static_cast<int>(needed);
}

//-----------------------------------------------------------------------------
// Drop poolIndex != MAIN at the stringtable-add chokepoint.
//-----------------------------------------------------------------------------
static void PrecacheModel_AddToStringTable(const char* modelName,
										   int         poolIndex   = 0,
										   const char* pakPath     = nullptr,
										   const char* placeholder = nullptr)
{
	if (!modelName || !*modelName) return;

	void* const table = ResolveModelPrecacheTable();
	if (!table)
	{
		static std::atomic<uint64_t> s_mpFailCount{0};
		if (s_mpFailCount.fetch_add(1) < 4)
		{
			Warning(eDLL_T::SERVER,
				"[s21-dedi] PrecacheModel_AddToStringTable('%s'): "
				"modelprecache table not resolved.\n", modelName);
		}
		return;
	}

	FlushPendingReplicatedModels();

	// Drop prior-level names if the dedupe set outlived a skipped reset.
	ClearSeenNamesIfGenerationMoved();

	// Dedupe no-userdata adds only after the table resolves.
	const bool hasUserData = (pakPath && *pakPath);
	if (!hasUserData && !MarkAndCheckSeen(modelName))
		return;

	// Do not SEH-swallow a bad table ptr; IsPlausibleHeapPtr already filters sentinels.
	void** const vtable = *reinterpret_cast<void***>(table);
	if (!IsPlausibleHeapPtr(vtable)) return;
	const AddString_t AddString =
		reinterpret_cast<AddString_t>(vtable[8]);
	if (!IsPlausibleHeapPtr(reinterpret_cast<void*>(AddString))) return;

	// 0xB0BA userdata when pak_path is set; otherwise the non-ODL path.
	char udBuf[768];
	const void* userdata = nullptr;
	int udLen = -1;
	if (pakPath && *pakPath)
	{
		udLen = BuildModelPrecacheUserData(udBuf, sizeof(udBuf),
			poolIndex, pakPath, placeholder);
		if (udLen > 0)
			userdata = udBuf;
	}

	const int index = AddString(table, /*bIsServer*/1, modelName,
								udLen, userdata);
	(void)index;

	s_mpAddCount.fetch_add(1);
}

//-----------------------------------------------------------------------------
// Resolve SkinNames via container->FindTable. Memoizes on success.
//-----------------------------------------------------------------------------
static void* ResolveSkinNamesTable_Locked()
{
	const unsigned int curGen = ServerGameDLL_GetLevelGeneration();
	void* cached = s_pSkinNamesTable.load(std::memory_order_acquire);
	const unsigned int cachedGen =
		s_skinTableGeneration.load(std::memory_order_acquire);

	if (cached && cachedGen == curGen)
		return cached;

	if (cached)
	{
		// Invalidate inside the lock so two threads cannot race a half-cleared cache.
		static std::atomic<unsigned int> s_skinStaleWarnedGen{0};
		if (s_skinStaleWarnedGen.exchange(curGen, std::memory_order_acq_rel) != curGen)
		{
			Warning(eDLL_T::SERVER,
				"[s21-dedi] SkinNames table cache was stale (gen %u -> %u) -- "
				"re-resolving; a level transition skipped the SDK reset\n",
				cachedGen, curGen);
		}
		s_pSkinNamesTable.store(nullptr, std::memory_order_release);
	}

	const uintptr_t base =
		static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	void** const containerSlot =
		reinterpret_cast<void**>(base + kStringTableContainerServer_RVA);
	void* const container = *containerSlot;
	if (!IsPlausibleHeapPtr(container)) return nullptr;

	void** const vtable = *reinterpret_cast<void***>(container);
	if (!IsPlausibleHeapPtr(vtable)) return nullptr;
	const FindTable_t FindTable =
		reinterpret_cast<FindTable_t>(vtable[3]);
	if (!IsPlausibleHeapPtr(reinterpret_cast<void*>(FindTable))) return nullptr;

	void* const table = FindTable(container, "SkinNames");
	if (!IsPlausibleHeapPtr(table)) return nullptr;
	s_pSkinNamesTable.store(table, std::memory_order_release);
	s_skinTableGeneration.store(ServerGameDLL_GetLevelGeneration(),
		std::memory_order_release);
	return table;
}

static void* ResolveSkinNamesTable()
{
	void* cached = s_pSkinNamesTable.load(std::memory_order_acquire);
	if (cached)
	{
		const unsigned int curGen = ServerGameDLL_GetLevelGeneration();
		if (s_skinTableGeneration.load(std::memory_order_acquire) == curGen)
			return cached;
	}

	std::lock_guard<std::mutex> lk(s_resolveMu);
	return ResolveSkinNamesTable_Locked();
}

//-----------------------------------------------------------------------------
// Add one skin name to SkinNames.
//-----------------------------------------------------------------------------
void PrecacheSkinName_Add(const char* skinName)
{
	if (!skinName || !*skinName) return;

	void* const table = ResolveSkinNamesTable();
	if (!table)
	{
		const uint64_t f = s_addFailCount.fetch_add(1) + 1;
		if (f <= 4)
		{
			Warning(eDLL_T::SERVER,
				"[s21-dedi] PrecacheSkinName_Add('%s'): SkinNames table "
				"not resolved -- VSkinNamesTableInject may not have run "
				"yet, or container layout drifted.\n", skinName);
		}
		return;
	}

	// No SEH wrap: see PrecacheModel_AddToStringTable for rationale.
	void** const vtable = *reinterpret_cast<void***>(table);
	if (!IsPlausibleHeapPtr(vtable)) return;
	const AddString_t AddString =
		reinterpret_cast<AddString_t>(vtable[8]);
	if (!IsPlausibleHeapPtr(reinterpret_cast<void*>(AddString))) return;
	// bIsServer=1, length=-1, userdata=nullptr.
	const int index = AddString(table, /*bIsServer*/1, skinName,
								/*length*/-1, /*userdata*/nullptr);

	s_addCount.fetch_add(1);
	(void)index;
}

//-----------------------------------------------------------------------------
// Launch-set classic skins, alphabetical.
//-----------------------------------------------------------------------------
static const char* const kKnownClassicSkins[] = {
	"alter_classic",
	"ash_classic",
	"ballistic_classic",
	"bangalore_classic",
	"bloodhound_classic",
	"catalyst_classic",
	"caustic_classic",
	"conduit_classic",
	"crypto_classic",
	"fuse_classic",
	"gibraltar_classic",
	"horizon_classic",
	"lifeline_classic",
	"loba_classic",
	"madmaggie_classic",
	"mirage_classic",
	"newcastle_classic",
	"octane_classic",
	"pathfinder_classic",
	"rampart_classic",
	"revenant_classic",
	"seer_classic",
	"valkyrie_classic",
	"vantage_classic",
	"wattson_classic",
	"wraith_classic",

	// Dummy / training models.
	"dummie",
	"pilot_medium_dummie",
};

void PrecacheSkinName_PopulateAll()
{
	if (sdk_skinnames_autopopulate.GetInt() <= 0)
	{
		Msg(eDLL_T::SERVER,
			"[s21-dedi] SkinNames auto-populate skipped "
			"(sdk_skinnames_autopopulate=0).\n");
		return;
	}

	const size_t count =
		sizeof(kKnownClassicSkins) / sizeof(kKnownClassicSkins[0]);
	for (size_t i = 0; i < count; ++i)
		PrecacheSkinName_Add(kKnownClassicSkins[i]);

	Msg(eDLL_T::SERVER,
		"[s21-dedi] SkinNames auto-populate finished: %zu launch-set "
		"skins queued. Replication ships on next snapshot.\n", count);
}

//-----------------------------------------------------------------------------
// SERVER-VM natives only.
//-----------------------------------------------------------------------------
static SQRESULT Script_PrecacheSkinName(HSQUIRRELVM v)
{
	const SQChar* pszName = nullptr;
	sq_getstring(v, 2, &pszName);
	if (!pszName || !*pszName)
	{
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	PrecacheSkinName_Add(pszName);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Forward decl for the ODL registry (defined later in this TU).
int OdlAsset_Register(int pool, const char* pak, const char* model,
					  const char* loadingModel);

// Forward decl: defined below at the top of Script_PrecacheOnDemandLoadModel
// support helpers.
static const SQChar* SQ_GetStringOrAsset(HSQUIRRELVM v, SQInteger idx);

// RegisterModel: replicated modelprecache add with no 0xB0BA userdata.
static SQRESULT Script_RegisterModel(HSQUIRRELVM v)
{
	const SQChar* modelName = SQ_GetStringOrAsset(v, 2);
	if (!modelName || !*modelName)
	{
		Warning(eDLL_T::SERVER,
			"[s21-dedi] RegisterModel called with empty model name\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// Script asset stringval drops the extension; lookups use the full ".rmdl" path.
	char buf[256];
	const char* registerName = modelName;
	const size_t len = strlen(modelName);
	const bool hasExt = (len >= 5 &&
		modelName[len-5] == '.' && modelName[len-4] == 'r' &&
		modelName[len-3] == 'm' && modelName[len-2] == 'd' &&
		modelName[len-1] == 'l');
	if (!hasExt && len + 5 < sizeof(buf))
	{
		memcpy(buf, modelName, len);
		memcpy(buf + len, ".rmdl", 6); // includes null terminator
		registerName = buf;
	}

	PrecacheModel_AddToStringTable(registerName);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_Zipline_IsCurvedZipline(HSQUIRRELVM v)
{
	SQEntity zipline = nullptr;
	if (!v_sq_getentity(v, &zipline))
		return SQ_ERROR;

	using IsCurvedZiplineFn = bool(__fastcall*)(void*);

	void* const ent = zipline;
	bool isCurved = false;
	__try
	{
		void** const vtable = *reinterpret_cast<void***>(ent);
		if (vtable && vtable[0x7B0 / sizeof(void*)])
		{
			isCurved =
				reinterpret_cast<IsCurvedZiplineFn>(vtable[0x7B0 / sizeof(void*)])(ent);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		isCurved = false;
	}

	if (!isCurved)
		isCurved = ZiprailDedi_HasPath(reinterpret_cast<uintptr_t>(ent));

	sq_pushbool(v, isCurved);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// CZipline is-a-zipline virtual (byte offset into the entity vtable).
static constexpr ptrdiff_t kZipVtblIsZipline = 0x310;

static bool Zipline_IsClassZipline(void* ent)
{
	if (!ent)
		return false;

	bool isZip = false;
	__try
	{
		void** const pVtbl = *reinterpret_cast<void***>(ent);
		if (pVtbl)
		{
			auto IsZipline = *reinterpret_cast<bool(__fastcall**)(void*)>(
				reinterpret_cast<uint8_t*>(pVtbl) + kZipVtblIsZipline);
			if (IsZipline)
				isZip = IsZipline(ent);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		isZip = false;
	}
	return isZip;
}

SQRESULT Script_Zipline_SetRopeColorModulation(HSQUIRRELVM v)
{
	SQEntity zipline = nullptr;
	if (!v_sq_getentity(v, &zipline))
		return SQ_ERROR;

	if (!Zipline_IsClassZipline(zipline))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	const SQVector3D* color = nullptr;
	if (SQ_FAILED(sq_getvector(v, 2, &color)) || !color)
		return SQ_ERROR;

	const float rgb[3] = { color->x, color->y, color->z };
	Zipline_SetRopeColorModulation(zipline, rgb);
	MarkEntityEdictDirty(zipline);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_Zipline_GetRopeColorModulation(HSQUIRRELVM v)
{
	SQEntity zipline = nullptr;
	if (!v_sq_getentity(v, &zipline))
		return SQ_ERROR;

	SQVector3D out;
	out.Init(0.0f, 0.0f, 0.0f);

	if (!Zipline_IsClassZipline(zipline))
	{
		sq_pushvector(v, &out);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	float rgb[3] = { 0.0f, 0.0f, 0.0f };
	Zipline_GetRopeColorModulation(zipline, rgb);
	out.Init(rgb[0], rgb[1], rgb[2]);

	sq_pushvector(v, &out);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_GetUseStateByIndex(HSQUIRRELVM v)
{
	SQEntity harvester = nullptr;
	if (!v_sq_getentity(v, &harvester))
		return SQ_ERROR;

	// Class-gated to info_loot_ceremony_harvester; m_pServerClass at +0x50.
	{
		bool classOk = false;
		__try
		{
			const uintptr_t entPtr = reinterpret_cast<uintptr_t>(harvester);
			const uintptr_t sc = *reinterpret_cast<const uintptr_t*>(entPtr + 0x50);
			if (sc)
			{
				const char* const cn = *reinterpret_cast<const char* const*>(sc + 0x00);
				classOk = (cn && _stricmp(cn, "info_loot_ceremony_harvester") == 0);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		if (!classOk)
		{
			sq_pushbool(v, SQFalse);
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
	}

	SQInteger index = 0;
	if (SQ_FAILED(sq_getinteger(v, 2, &index)))
		return SQ_ERROR;

	// Slot [0, 128); collected-state field at +0x15E0.
	static constexpr SQInteger kAbsolutePlayerLimit = 128;
	static constexpr size_t kCollectedStateOffset = 0x15E0;

	bool isUsed = false;
	if (index >= 0 && index < kAbsolutePlayerLimit)
	{
		__try
		{
			const auto* const base =
				reinterpret_cast<const uint8_t*>(harvester) + kCollectedStateOffset;
			const uint64_t word =
				reinterpret_cast<const uint64_t*>(base)[static_cast<size_t>(index) >> 6];
			isUsed = ((word >> (static_cast<unsigned>(index) & 0x3F)) & 1ull) != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			isUsed = false;
		}
	}

	sq_pushbool(v, isUsed ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_SetUseStateByIndex(HSQUIRRELVM v)
{
	SQEntity harvester = nullptr;
	if (!v_sq_getentity(v, &harvester))
		return SQ_ERROR;

	// Class-gated to info_loot_ceremony_harvester; m_pServerClass at +0x50.
	{
		bool classOk = false;
		__try
		{
			const uintptr_t entPtr = reinterpret_cast<uintptr_t>(harvester);
			const uintptr_t sc = *reinterpret_cast<const uintptr_t*>(entPtr + 0x50);
			if (sc)
			{
				const char* const cn = *reinterpret_cast<const char* const*>(sc + 0x00);
				classOk = (cn && _stricmp(cn, "info_loot_ceremony_harvester") == 0);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		if (!classOk)
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);  // wrong entity type -- silent no-op
	}

	SQInteger index = 0;
	if (SQ_FAILED(sq_getinteger(v, 2, &index)))
		return SQ_ERROR;

	SQBool enabled = false;
	if (SQ_FAILED(sq_getbool(v, 3, &enabled)))
		return SQ_ERROR;

	static constexpr SQInteger kAbsolutePlayerLimit = 128;
	static constexpr size_t kCollectedStateOffset = 0x15E0;

	if (index >= 0 && index < kAbsolutePlayerLimit)
	{
		__try
		{
			auto* const base =
				reinterpret_cast<uint8_t*>(harvester) + kCollectedStateOffset;
			uint64_t& word =
				reinterpret_cast<uint64_t*>(base)[static_cast<size_t>(index) >> 6];
			const uint64_t bit = 1ull << (static_cast<unsigned>(index) & 0x3F);
			if (enabled)
				word |= bit;
			else
				word &= ~bit;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// GetTriggersByClassesInRealms: entity-list walk; stack POD only.
//-----------------------------------------------------------------------------

static bool Trigger_IsUserPtr(const void* p)
{
	const uintptr_t u = reinterpret_cast<uintptr_t>(p);
	return u >= 0x10000ull && u <= 0x00007FFFFFFFFFFFull;
}

static constexpr int kTriggerMaxClasses = 32;
static constexpr int kTriggerMaxRealms = 64;
// S3 CBaseEntity layout (static_assert sizeof==0xB08): m_Collision@0x328,
// m_contents@0x3A8, m_realmsBitMask@0xAE8.
static constexpr size_t kCBaseEntity_Contents = 0x3A8;
static constexpr size_t kCBaseEntity_RealmsBitMask = 0xAE8;

// className strings point into the VM string table for the duration of the call.
static bool SQ_ReadStringArrayPtrs(HSQUIRRELVM v, SQInteger idx,
	const char** out, int* outCount, int maxCount)
{
	*outCount = 0;
	if (!out || maxCount <= 0)
		return false;

	SQObjectPtr& obj = stack_get(v, idx);
	if (!sq_isarray(obj) || !_array(obj))
		return false;

	SQArray* const arr = _array(obj);
	const SQInteger count = arr->Size();
	if (count < 0)
		return false;

	const int n = (count < maxCount) ? static_cast<int>(count) : maxCount;
	int written = 0;
	for (int i = 0; i < n; ++i)
	{
		SQObjectPtr val;
		if (!arr->Get(i, val) || !sq_isstring(val))
			return false;
		const char* const str = _stringval(val);
		if (str && *str)
			out[written++] = str;
	}
	*outCount = written;
	return true;
}

static bool SQ_ReadIntArrayBuf(HSQUIRRELVM v, SQInteger idx,
	int* out, int* outCount, int maxCount)
{
	*outCount = 0;
	if (!out || maxCount <= 0)
		return false;

	SQObjectPtr& obj = stack_get(v, idx);
	if (!sq_isarray(obj) || !_array(obj))
		return false;

	SQArray* const arr = _array(obj);
	const SQInteger count = arr->Size();
	if (count < 0)
		return false;

	const int n = (count < maxCount) ? static_cast<int>(count) : maxCount;
	for (int i = 0; i < n; ++i)
	{
		SQObjectPtr val;
		if (!arr->Get(i, val) || !sq_isnumeric(val))
			return false;
		out[i] = static_cast<int>(tointeger(val));
	}
	*outCount = n;
	return true;
}

static Vector3D SQ_ToVector3D(const SQVector3D* v)
{
	return v ? Vector3D(v->x, v->y, v->z) : Vector3D(0.0f, 0.0f, 0.0f);
}

// Empty class list matches nothing.
static bool TriggerClassMatches(const char* const* classes, int classCount,
	const char* className)
{
	if (classCount <= 0 || !className || !*className)
		return false;
	if (!Trigger_IsUserPtr(className))
		return false;

	for (int i = 0; i < classCount; ++i)
	{
		const char* const wanted = classes[i];
		if (wanted && V_strcmp(wanted, className) == 0)
			return true;
	}
	return false;
}

// Realm match: (entity.m_realmsBitMask & queryRealmBitMask) != 0.
// queryMask is precomputed once (bit i for realm index i). Entity mask 0 is
// match-all on S3 (many ents never write realms).
static uint64_t TriggerBuildRealmQueryMask(const int* realms, int realmCount)
{
	uint64_t queryMask = 0;
	for (int i = 0; i < realmCount; ++i)
	{
		const int realm = realms[i];
		if (realm >= 0 && realm < 63)
			queryMask |= (1ull << realm);
	}
	return queryMask;
}

static bool TriggerRealmMatchesMask(CBaseEntity* ent, uint64_t queryMask)
{
	if (queryMask == 0 || !Trigger_IsUserPtr(ent))
		return false;

	uint64_t entMask = 0;
	memcpy(&entMask, reinterpret_cast<const uint8_t*>(ent) + kCBaseEntity_RealmsBitMask,
		sizeof(entMask));

	if (entMask == 0)
		return true;

	return (entMask & queryMask) != 0;
}

// Approximate stand-in for CollQuery contentsMask.
static bool TriggerContentsMatches(CBaseEntity* ent, int contentMask)
{
	if (contentMask == 0)
		return true;
	if (!Trigger_IsUserPtr(ent))
		return false;

	int contents = 0;
	memcpy(&contents, reinterpret_cast<const uint8_t*>(ent) + kCBaseEntity_Contents,
		sizeof(contents));

	return contents == 0 || (contents & contentMask) != 0;
}

static bool TriggerHullIsPoint(const Vector3D& hullMins, const Vector3D& hullMaxs)
{
	return hullMins.x == 0.0f && hullMins.y == 0.0f && hullMins.z == 0.0f &&
		hullMaxs.x == 0.0f && hullMaxs.y == 0.0f && hullMaxs.z == 0.0f;
}

// Overlap is ClipRayToCollideable, not origin+local AABB.
static bool TriggerOverlapsQuery(CBaseEntity* ent,
	const Vector3D& start, const Vector3D& end,
	const Vector3D& hullMins, const Vector3D& hullMaxs)
{
	if (!g_pEngineTraceServer || !Trigger_IsUserPtr(ent))
		return false;

	ICollideable* const coll = ent->CollisionProp();
	if (!Trigger_IsUserPtr(coll))
		return false;

	alignas(16) Ray_t ray;
	memset(&ray, 0, sizeof(ray));

	if (!TriggerHullIsPoint(hullMins, hullMaxs) && v_Ray_t_InitStartEndMinsMaxsUp)
	{
		const Vector3D up(0.0f, 0.0f, 1.0f);
		v_Ray_t_InitStartEndMinsMaxsUp(&ray, &start, &end, &hullMins, &hullMaxs, &up);
	}
	else
	{
		ray.Init(start, end, 0x3f800000, 0);
	}

	trace_t tr;
	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1.0f;
	tr.endpos = end;

	g_pEngineTraceServer->ClipRayToCollideable(ray, 0xBFFFFFFFu, coll, &tr);
	return tr.startsolid || tr.allsolid || tr.fraction < 1.0f;
}

// S3 has no SpatialAccel_ForEachInQuery / TriggersByClassesEnumerator.
// Walk the entity list, filter class+realm+contents, then collideable clip.
static SQRESULT Script_PushTriggersByClassesInRealms(HSQUIRRELVM v,
	const char* const* classes, int classCount,
	const Vector3D& start, const Vector3D& end,
	uint64_t realmQueryMask, int contentMask,
	const Vector3D& hullMins, const Vector3D& hullMaxs)
{
	if (classCount <= 0 || realmQueryMask == 0)
	{
		sq_newarray(v, 0);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	sq_newarray(v, 0);
	if (!g_serverEntityList || !v_CSquirrelVM_PushEntity_Server)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	int safety = 0;
	for (const CEntInfo* info = g_serverEntityList->FirstEntInfo();
		info && safety++ < 20000;
		info = g_serverEntityList->NextEntInfo(info))
	{
		if (!Trigger_IsUserPtr(info))
			break;

		CBaseEntity* const ent = reinterpret_cast<CBaseEntity*>(info->m_pEntity);
		if (!ent || !Trigger_IsUserPtr(ent))
			continue;

		const char* const className = STRING(info->m_iClassName);
		if (!TriggerClassMatches(classes, classCount, className) ||
			!TriggerRealmMatchesMask(ent, realmQueryMask) ||
			!TriggerContentsMatches(ent, contentMask))
		{
			continue;
		}

		if (!TriggerOverlapsQuery(ent, start, end, hullMins, hullMaxs))
			continue;

		v_CSquirrelVM_PushEntity_Server(v, ent);
		sq_arrayappend(v, -2);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Point/segment form: same params as hull variant but mins/maxs = origin.
SQRESULT Script_GetTriggersByClassesInRealms(HSQUIRRELVM v)
{
	const char* classes[kTriggerMaxClasses];
	int classCount = 0;
	int realms[kTriggerMaxRealms];
	int realmCount = 0;

	if (!SQ_ReadStringArrayPtrs(v, 2, classes, &classCount, kTriggerMaxClasses))
		return SQ_ERROR;

	const SQVector3D* startSq = nullptr;
	const SQVector3D* endSq = nullptr;
	if (SQ_FAILED(sq_getvector(v, 3, &startSq)) ||
		SQ_FAILED(sq_getvector(v, 4, &endSq)) ||
		!SQ_ReadIntArrayBuf(v, 5, realms, &realmCount, kTriggerMaxRealms))
	{
		return SQ_ERROR;
	}

	SQInteger contentMaskSq = 0;
	if (SQ_FAILED(sq_getinteger(v, 6, &contentMaskSq)))
		return SQ_ERROR;

	const uint64_t realmMask = TriggerBuildRealmQueryMask(realms, realmCount);
	const Vector3D zero(0.0f, 0.0f, 0.0f);
	return Script_PushTriggersByClassesInRealms(v, classes, classCount,
		SQ_ToVector3D(startSq), SQ_ToVector3D(endSq), realmMask,
		static_cast<int>(contentMaskSq), zero, zero);
}

SQRESULT Script_GetTriggersByClassesInRealms_HullSize(HSQUIRRELVM v)
{
	const char* classes[kTriggerMaxClasses];
	int classCount = 0;
	int realms[kTriggerMaxRealms];
	int realmCount = 0;

	if (!SQ_ReadStringArrayPtrs(v, 2, classes, &classCount, kTriggerMaxClasses))
		return SQ_ERROR;

	const SQVector3D* startSq = nullptr;
	const SQVector3D* endSq = nullptr;
	const SQVector3D* minsSq = nullptr;
	const SQVector3D* maxsSq = nullptr;
	if (SQ_FAILED(sq_getvector(v, 3, &startSq)) ||
		SQ_FAILED(sq_getvector(v, 4, &endSq)) ||
		!SQ_ReadIntArrayBuf(v, 5, realms, &realmCount, kTriggerMaxRealms))
	{
		return SQ_ERROR;
	}

	SQInteger contentMaskSq = 0;
	if (SQ_FAILED(sq_getinteger(v, 6, &contentMaskSq)) ||
		SQ_FAILED(sq_getvector(v, 7, &minsSq)) ||
		SQ_FAILED(sq_getvector(v, 8, &maxsSq)))
	{
		return SQ_ERROR;
	}

	const uint64_t realmMask = TriggerBuildRealmQueryMask(realms, realmCount);
	return Script_PushTriggersByClassesInRealms(v, classes, classCount,
		SQ_ToVector3D(startSq), SQ_ToVector3D(endSq), realmMask,
		static_cast<int>(contentMaskSq),
		SQ_ToVector3D(minsSq), SQ_ToVector3D(maxsSq));
}

//-----------------------------------------------------------------------------
// Generic AddString clears 0xB0BA; ODL names re-attach userdata. Script paths need ".rmdl".
//-----------------------------------------------------------------------------
struct OdlUserDataRef { int pool = 0; std::string pak, placeholder; };
static bool OdlLookupUserData(const char* name, OdlUserDataRef& out); // defined with the registry

static void AddReplicatedModelName(const char* registerName)
{
	OdlUserDataRef ref;
	if (OdlLookupUserData(registerName, ref))
		PrecacheModel_AddToStringTable(registerName, ref.pool,
			ref.pak.empty()         ? nullptr : ref.pak.c_str(),
			ref.placeholder.empty() ? nullptr : ref.placeholder.c_str());
	else
		PrecacheModel_AddToStringTable(registerName);
}

void PrecacheModel_RegisterReplicated(const char* modelName)
{
	if (!modelName || !*modelName) return;

	char buf[256];
	const char* registerName = modelName;
	const size_t len = strlen(modelName);
	const bool hasExt = (len >= 5 &&
		modelName[len-5] == '.' && modelName[len-4] == 'r' &&
		modelName[len-3] == 'm' && modelName[len-2] == 'd' &&
		modelName[len-1] == 'l');
	if (!hasExt && len + 5 < sizeof(buf))
	{
		memcpy(buf, modelName, len);
		memcpy(buf + len, ".rmdl", 6);
		registerName = buf;
	}

	if (!PrecacheModel_IsModelPrecacheReady())
	{
		QueuePendingReplicatedModel(registerName);
		return;
	}

	AddReplicatedModelName(registerName);
}

static void FlushPendingReplicatedModels()
{
	if (s_mpFlushingPending.exchange(true, std::memory_order_acq_rel))
		return;

	std::vector<std::string> pending;
	{
		std::lock_guard<std::mutex> lk(s_mpPendingMu);
		if (!s_mpPendingNames.empty())
		{
			pending.reserve(s_mpPendingNames.size());
			for (const std::string& modelName : s_mpPendingNames)
				pending.push_back(modelName);
			s_mpPendingNames.clear();
		}
	}

	if (!pending.empty())
	{
		Msg(eDLL_T::SERVER,
			"[s21-dedi] flushing %zu pending replicated modelprecache name(s)\n",
			pending.size());
		for (const std::string& modelName : pending)
			AddReplicatedModelName(modelName.c_str());
	}

	s_mpFlushingPending.store(false, std::memory_order_release);
}

// Helper: extract string from OT_STRING or OT_ASSET stack positions.
// sq_getstring only accepts OT_STRING; asset params need this.
static const SQChar* SQ_GetStringOrAsset(HSQUIRRELVM v, SQInteger idx)
{
	const SQChar* s = nullptr;
	if (SQ_SUCCEEDED(sq_getstring(v, idx, &s)))
		return s;
	// OT_ASSET: same internal layout as OT_STRING (pString->_val)
	SQObject obj;
	if (SQ_SUCCEEDED(sq_getstackobj(v, idx, &obj)))
	{
		if (sq_isstring(obj) || (obj._type == OT_ASSET))
			return _stringval(obj);
	}
	return nullptr;
}

static SQRESULT Script_PrecacheOnDemandLoadModel(HSQUIRRELVM v)
{
	SQInteger pool = 0;
	const SQChar* pak = nullptr;
	const SQChar* modelFile = nullptr;
	const SQChar* loadingModel = nullptr;

	sq_getinteger(v, 2, &pool);
	pak          = SQ_GetStringOrAsset(v, 3);
	modelFile    = SQ_GetStringOrAsset(v, 4);
	loadingModel = SQ_GetStringOrAsset(v, 5);

	// Register in the ODL pool. Idempotent.
	const int handle = OdlAsset_Register((int)pool,
										 pak          ? pak          : "",
										 modelFile    ? modelFile    : "",
										 loadingModel ? loadingModel : "");
	(void)handle;

	// Table entry only. model_t bind happens in PrecacheNatives_ReplayOdlOnMapSpawn after Host_NewGame.
	if (modelFile && *modelFile)
		PrecacheModel_AddToStringTable(modelFile, (int)pool, pak,
									   loadingModel);
	if (loadingModel && *loadingModel)
		PrecacheModel_AddToStringTable(loadingModel, (int)pool, pak,
									   loadingModel);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// ODL registry keyed by (pool, lowercase asset). IsLoaded means registered.
//=============================================================================

// Pool 2 = SKINS.
static constexpr int kOdlPool_MAIN     = 0;
static constexpr int kOdlPool_STICKERS = 1;
static constexpr int kOdlPool_SKINS    = 2;
static constexpr int kOdlPool_INVALID  = 3;

struct OdlAssetEntry
{
	int         pool;
	std::string pak;
	std::string model;        // canonical (lowercase) asset path
	std::string loadingModel;
	int         handle;       // synthetic handle, > 0
	int         refCount;
};

static std::mutex                                  s_odlMu;
static std::unordered_map<int, OdlAssetEntry>      s_odlByHandle;       // handle -> entry
static std::unordered_map<std::string, int>        s_odlByPoolAndModel; // "pool|model_lower" -> handle
static std::atomic<int>                            s_odlNextHandle{1};
static std::unordered_map<int, int>                s_odlPoolGoals;      // pool -> goal
static std::unordered_map<std::string, OdlUserDataRef> s_odlByName;      // normalized name -> 0xB0BA userdata

static std::atomic<uint64_t> s_odlRegisterCount{0};
static std::atomic<uint64_t> s_odlFindHitCount{0};
static std::atomic<uint64_t> s_odlFindMissCount{0};

static std::string OdlMakeKey(int pool, const char* model)
{
	std::string s;
	s.reserve(48);
	s += std::to_string(pool);
	s += '|';
	if (model) {
		for (const char* p = model; *p; ++p) {
			unsigned char c = static_cast<unsigned char>(*p);
			if (c >= 'A' && c <= 'Z') c += 32;
			s += static_cast<char>(c);
		}
	}
	return s;
}

// Lowercase + ".rmdl" so script names match engine lookups.
static std::string OdlNormalizeName(const char* name)
{
	std::string s = name ? name : "";
	for (char& c : s)
		if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	const size_t n = s.size();
	if (!(n >= 5 && s.compare(n - 5, 5, ".rmdl") == 0))
		s += ".rmdl";
	return s;
}

// 0xB0BA userdata for a PODLM-registered name.
static bool OdlLookupUserData(const char* name, OdlUserDataRef& out)
{
	if (!name || !*name) return false;
	const std::string key = OdlNormalizeName(name);
	std::lock_guard<std::mutex> lk(s_odlMu);
	auto it = s_odlByName.find(key);
	if (it == s_odlByName.end()) return false;
	out = it->second;
	return true;
}

// Idempotent (pool, model) register; returns the existing handle if present.
int OdlAsset_Register(int pool, const char* pak, const char* model,
					  const char* loadingModel)
{
	if (!model || !*model) return -1;

	std::lock_guard<std::mutex> lk(s_odlMu);
	const std::string key = OdlMakeKey(pool, model);
	auto it = s_odlByPoolAndModel.find(key);
	if (it != s_odlByPoolAndModel.end())
		return it->second;

	const int h = s_odlNextHandle.fetch_add(1);
	OdlAssetEntry e;
	e.pool         = pool;
	e.pak          = pak          ? pak          : "";
	e.model        = model;
	e.loadingModel = loadingModel ? loadingModel : "";
	e.handle       = h;
	e.refCount     = 0;

	s_odlByHandle.emplace(h, std::move(e));
	s_odlByPoolAndModel.emplace(key, h);
	s_odlRegisterCount.fetch_add(1);

	// Name-keyed 0xB0BA so a generic re-add cannot strip the blob. Persists across maps.
	const OdlUserDataRef ref{ pool, pak ? pak : "", loadingModel ? loadingModel : "" };
	if (model && *model)               s_odlByName[OdlNormalizeName(model)] = ref;
	if (loadingModel && *loadingModel) s_odlByName[OdlNormalizeName(loadingModel)] = ref;
	return h;
}

//-----------------------------------------------------------------------------
// Replay PrecacheModel after Host_NewGame so model_t binds stick.
//-----------------------------------------------------------------------------
int PrecacheNatives_ReplayOdlOnMapSpawn()
{
	// Snapshot registry under lock; release before Server_PrecacheModel. Re-attach 0xB0BA last.
	struct ReplayEntry { int pool; std::string pak, model, loading; };
	std::vector<ReplayEntry> entries;
	{
		std::lock_guard<std::mutex> lk(s_odlMu);
		entries.reserve(s_odlByHandle.size());
		for (const auto& kv : s_odlByHandle)
			entries.push_back({ kv.second.pool, kv.second.pak,
								kv.second.model, kv.second.loadingModel });
	}

	int replayed = 0;
	int bound    = 0;  // number of (post-replay) NON-NULL model_t bindings
	const bool canProbe = (CModelLoader__FindModel && g_pModelLoader);

	for (const auto& e : entries)
	{
		const char* const pak = e.pak.empty() ? nullptr : e.pak.c_str();
		const char* const placeholder =
			e.loading.empty() ? nullptr : e.loading.c_str();

		if (!e.model.empty())
		{
			(void)Server_PrecacheModel_Invoke(e.model.c_str());
			++replayed;
			if (canProbe && CModelLoader__FindModel(g_pModelLoader, e.model.c_str()))
				++bound;
			// Re-attach 0xB0BA last; the bind may have re-added the name without userdata.
			if (pak)
				PrecacheModel_AddToStringTable(e.model.c_str(), e.pool, pak,
											   placeholder);
		}
		if (!e.loading.empty())
		{
			(void)Server_PrecacheModel_Invoke(e.loading.c_str());
			++replayed;
			if (canProbe && CModelLoader__FindModel(g_pModelLoader, e.loading.c_str()))
				++bound;
			if (pak)
				PrecacheModel_AddToStringTable(e.loading.c_str(), e.pool, pak,
											   placeholder);
		}
	}

	Msg(eDLL_T::SERVER,
		"[s21-dedi] ODL precache replay: %d engine PrecacheModel calls / "
		"%zu ODL entries, %d model_t bindings active (userdata re-attached).\n",
		replayed, entries.size(), bound);

	if (!entries.empty() && bound == 0)
	{
		Warning(eDLL_T::SERVER,
			"[s21-dedi] ODL precache replay bound ZERO model_t for %zu entries -- "
			"the modelprecache table is not the live one for this level\n",
			entries.size());
	}

	RCON_LauncherSession_NotifyHostReady(
		(g_pHostState && g_pHostState->m_levelName[0])
			? g_pHostState->m_levelName
			: "");

	return replayed;
}

//-----------------------------------------------------------------------------
// ODL_FindAsset: registered handle or -1.
//-----------------------------------------------------------------------------
static SQRESULT Script_ODL_FindAsset(HSQUIRRELVM v)
{
	SQInteger pool = 0;
	const SQChar* model = nullptr;
	sq_getinteger(v, 2, &pool);
	model = SQ_GetStringOrAsset(v, 3);

	int handle = -1;
	{
		std::lock_guard<std::mutex> lk(s_odlMu);
		auto it = s_odlByPoolAndModel.find(OdlMakeKey((int)pool, model));
		if (it != s_odlByPoolAndModel.end())
			handle = it->second;
	}

	if (handle >= 0) s_odlFindHitCount.fetch_add(1);
	else             s_odlFindMissCount.fetch_add(1);

	sq_pushinteger(v, handle);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// ODL_IsLoaded: registered in this pool.
//-----------------------------------------------------------------------------
static SQRESULT Script_ODL_IsLoaded(HSQUIRRELVM v)
{
	SQInteger pool   = 0;
	SQInteger handle = 0;
	sq_getinteger(v, 2, &pool);
	sq_getinteger(v, 3, &handle);

	bool loaded = false;
	{
		std::lock_guard<std::mutex> lk(s_odlMu);
		auto it = s_odlByHandle.find((int)handle);
		if (it != s_odlByHandle.end() && it->second.pool == (int)pool)
			loaded = true;
	}

	sq_pushbool(v, loaded ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// ODL_AllocAsset: same register as PrecacheOnDemandLoadModel.
//-----------------------------------------------------------------------------
static SQRESULT Script_ODL_AllocAsset(HSQUIRRELVM v)
{
	SQInteger pool = 0;
	const SQChar* pak = nullptr;
	const SQChar* model = nullptr;
	const SQChar* loadingModel = nullptr;
	sq_getinteger(v, 2, &pool);
	pak          = SQ_GetStringOrAsset(v, 3);
	model        = SQ_GetStringOrAsset(v, 4);
	loadingModel = SQ_GetStringOrAsset(v, 5);

	const int h = OdlAsset_Register((int)pool, pak, model, loadingModel);

	sq_pushinteger(v, h);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// ODL_FreeAsset: drop the entry if refCount is zero.
//-----------------------------------------------------------------------------
static SQRESULT Script_ODL_FreeAsset(HSQUIRRELVM v)
{
	SQInteger handle = 0;
	sq_getinteger(v, 2, &handle);

	std::lock_guard<std::mutex> lk(s_odlMu);
	auto it = s_odlByHandle.find((int)handle);
	if (it != s_odlByHandle.end() && it->second.refCount <= 0)
	{
		s_odlByPoolAndModel.erase(OdlMakeKey(it->second.pool, it->second.model.c_str()));
		s_odlByHandle.erase(it);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// ODL_AddRef / ODL_RemoveRef.
//-----------------------------------------------------------------------------
static SQRESULT Script_ODL_AddRef(HSQUIRRELVM v)
{
	SQInteger handle = 0;
	sq_getinteger(v, 2, &handle);

	std::lock_guard<std::mutex> lk(s_odlMu);
	auto it = s_odlByHandle.find((int)handle);
	if (it != s_odlByHandle.end()) it->second.refCount++;
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_ODL_RemoveRef(HSQUIRRELVM v)
{
	SQInteger handle = 0;
	sq_getinteger(v, 2, &handle);

	std::lock_guard<std::mutex> lk(s_odlMu);
	auto it = s_odlByHandle.find((int)handle);
	if (it != s_odlByHandle.end() && it->second.refCount > 0)
		it->second.refCount--;
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// ODL_HintNeededPak / ODL_HintNeeded: no-ops on the dedi.
//-----------------------------------------------------------------------------
static SQRESULT Script_ODL_HintNeededPak(HSQUIRRELVM v)
{
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_ODL_HintNeeded(HSQUIRRELVM v)
{
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// ODL_UnloadAllUnused: drop refCount==0 entries in the pool.
//-----------------------------------------------------------------------------
static SQRESULT Script_ODL_UnloadAllUnused(HSQUIRRELVM v)
{
	SQInteger pool = 0;
	sq_getinteger(v, 2, &pool);

	int erased = 0;
	{
		std::lock_guard<std::mutex> lk(s_odlMu);
		for (auto it = s_odlByHandle.begin(); it != s_odlByHandle.end(); )
		{
			if (it->second.pool == (int)pool && it->second.refCount <= 0)
			{
				s_odlByPoolAndModel.erase(
					OdlMakeKey(it->second.pool, it->second.model.c_str()));
				it = s_odlByHandle.erase(it);
				++erased;
			}
			else
			{
				++it;
			}
		}
	}

	(void)erased;
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// ODL_SetPoolGoal: store the per-pool goal.
//-----------------------------------------------------------------------------
static SQRESULT Script_ODL_SetPoolGoal(HSQUIRRELVM v)
{
	SQInteger pool = 0;
	SQInteger goal = 0;
	sq_getinteger(v, 2, &pool);
	sq_getinteger(v, 3, &goal);

	{
		std::lock_guard<std::mutex> lk(s_odlMu);
		s_odlPoolGoals[(int)pool] = (int)goal;
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Native registration.
//-----------------------------------------------------------------------------
void Script_RegisterPrecacheServerNatives(CSquirrelVM* s)
{
	Script_RegisterFuncNamed(s, "PrecacheSkinName",
		"Script_PrecacheSkinName",
		"Add a skin name to the replicated SkinNames stringtable. "
		"Idempotent. (S21 bridge: dedi-side implementation)",
		"void", "string skinName", false,
		Script_PrecacheSkinName);

	Script_RegisterFuncNamed(s, "RegisterModel",
		"Script_RegisterModel",
		"Add a non-ODL model to the replicated modelprecache "
		"stringtable. Used by sh_weapon_cosmetics for charms / "
		"weapon skins etc. Equivalent of S21 ."
		"(S21 bridge: dedi-side implementation)",
		"void", "string modelName", false,
		Script_RegisterModel);

	Script_RegisterFuncNamed(s, "PrecacheOnDemandLoadModel",
		"Script_PrecacheOnDemandLoadModel",
		"Register a model for on-demand loading. Auto-tracks the "
		"(pool, pak, model, loading) tuple in the dedi-side ODL "
		"registry. (S21 bridge)",
		"void", "int pool, string pak, asset modelFile, asset loadingModel",
		false, Script_PrecacheOnDemandLoadModel);

	Script_RegisterFuncNamed(s, "Zipline_IsCurvedZipline",
		"Script_Zipline_IsCurvedZipline",
		"Returns true when the zipline is an S21 curved zipline/ziprail.",
		"bool", "entity zipline", false,
		Script_Zipline_IsCurvedZipline);

	Script_RegisterFuncNamed(s, "GetTriggersByClassesInRealms",
		"Script_GetTriggersByClassesInRealms",
		"Get all triggers of classes hit from start to end in given "
		"realms and content mask.",
		"array< entity >",
		"array< string > classes, vector start, vector end, "
		"array< int > realms, int contentMask",
		false, Script_GetTriggersByClassesInRealms);

	Script_RegisterFuncNamed(s, "GetTriggersByClassesInRealms_HullSize",
		"Script_GetTriggersByClassesInRealms_HullSize",
		"Get all triggers of classes hit from start to end in given "
		"realms, content mask and hull size.",
		"array< entity >",
		"array< string > classes, vector start, vector end, "
		"array< int > realms, int contentMask, vector mins, vector maxs",
		false, Script_GetTriggersByClassesInRealms_HullSize);

	Script_RegisterFuncNamed(s, "ODL_FindAsset",
		"Script_ODL_FindAsset",
		"Find a registered asset's ODL handle. Returns -1 if not "
		"registered. (S21 bridge: dedi-side ODL registry)",
		"int", "int pool, asset model", false,
		Script_ODL_FindAsset);

	Script_RegisterFuncNamed(s, "ODL_AllocAsset",
		"Script_ODL_AllocAsset",
		"Allocate (or look up) an ODL asset slot for the given "
		"(pool, pak, model, loading) tuple. (S21 bridge)",
		"int", "int pool, string pak, asset model, asset loadingModel",
		false, Script_ODL_AllocAsset);

	Script_RegisterFuncNamed(s, "ODL_FreeAsset",
		"Script_ODL_FreeAsset",
		"Free an ODL asset slot if its refCount is zero. (S21 bridge)",
		"void", "int handle", false,
		Script_ODL_FreeAsset);

	Script_RegisterFuncNamed(s, "ODL_AddRef",
		"Script_ODL_AddRef",
		"Add a reference to an ODL asset. Pair with ODL_RemoveRef. "
		"(S21 bridge)",
		"void", "int handle", false,
		Script_ODL_AddRef);

	Script_RegisterFuncNamed(s, "ODL_RemoveRef",
		"Script_ODL_RemoveRef",
		"Remove a reference from an ODL asset. (S21 bridge)",
		"void", "int handle", false,
		Script_ODL_RemoveRef);

	Script_RegisterFuncNamed(s, "ODL_IsLoaded",
		"Script_ODL_IsLoaded",
		"Check whether an ODL asset is loaded. On the dedi this is "
		"true for any registered handle; the client tracks real load "
		"state. (S21 bridge)",
		"bool", "int pool, int handle", false,
		Script_ODL_IsLoaded);

	Script_RegisterFuncNamed(s, "ODL_HintNeededPak",
		"Script_ODL_HintNeededPak",
		"Hint to ODL that a pak will be needed soon. Server no-op "
		"(paks load client-side). (S21 bridge)",
		"void", "int pool, string pak", false,
		Script_ODL_HintNeededPak);

	Script_RegisterFuncNamed(s, "ODL_HintNeeded",
		"Script_ODL_HintNeeded",
		"Hint to ODL that a specific asset is needed. Server no-op. "
		"(S21 bridge)",
		"void", "int handle, bool highPriority", false,
		Script_ODL_HintNeeded);

	Script_RegisterFuncNamed(s, "ODL_UnloadAllUnused",
		"Script_ODL_UnloadAllUnused",
		"Unload every ODL asset in the pool whose refCount is zero. "
		"(S21 bridge)",
		"void", "int pool", false,
		Script_ODL_UnloadAllUnused);

	Script_RegisterFuncNamed(s, "ODL_SetPoolGoal",
		"Script_ODL_SetPoolGoal",
		"Set the per-pool goal value. Recorded but not enforced on "
		"the dedi side. (S21 bridge)",
		"void", "int pool, int goal", false,
		Script_ODL_SetPoolGoal);

	// Scripts reference ODL_SKINS/etc. without declaring them; register via
	// RegisterConstant. Values match S21 client pool indices.

	s->RegisterConstant("ODL_MAIN",     kOdlPool_MAIN);
	s->RegisterConstant("ODL_STICKERS", kOdlPool_STICKERS);
	s->RegisterConstant("ODL_SKINS",    kOdlPool_SKINS);
	s->RegisterConstant("ODL_INVALID",  kOdlPool_INVALID);

	// S21 client engine registers these usable flags; this S3 build does not.
	// Values from the S21 client script-constant registrar (name-then-value order).
	s->RegisterConstant("USABLE_FROM_EXTENDED_RANGE", 0x20);
	s->RegisterConstant("USABLE_NO_LOS_REQUIREMENT",  0x40);
	s->RegisterConstant("USABLE_EXTENDED_USE",        0x200000);

	Msg(eDLL_T::SERVER,
		"[s21-dedi] PrecacheSkinName + RegisterModel + "
		"PrecacheOnDemandLoadModel + Zipline_IsCurvedZipline + "
		"Zipline_SetRopeColorModulation + Zipline_GetRopeColorModulation + "
		"GetTriggersByClassesInRealms + "
		"GetTriggersByClassesInRealms_HullSize + "
		"ODL_{FindAsset,AllocAsset,"
		"FreeAsset,AddRef,RemoveRef,IsLoaded,HintNeededPak,HintNeeded,"
		"UnloadAllUnused,SetPoolGoal} "
		"natives + pool enum constants + "
		"USABLE_FROM_EXTENDED_RANGE + USABLE_NO_LOS_REQUIREMENT + "
		"USABLE_EXTENDED_USE "
		"registered on SERVER VM.\n");
}

//-----------------------------------------------------------------------------
// Drop cached table pointers. Do not clear s_odlByHandle / s_odlByPoolAndModel / s_odlByName -- PODLM fires once at script-init.
//-----------------------------------------------------------------------------
void PrecacheNativesDedi_LevelShutdown()
{
	s_pSkinNamesTable.store(nullptr, std::memory_order_release);
	s_pModelPrecacheTable.store(nullptr, std::memory_order_release);

	s_addCount.store(0, std::memory_order_release);
	s_addFailCount.store(0, std::memory_order_release);
	s_mpAddCount.store(0, std::memory_order_release);
	s_odlRegisterCount.store(0, std::memory_order_release);
	s_odlFindHitCount.store(0, std::memory_order_release);
	s_odlFindMissCount.store(0, std::memory_order_release);

	{
		std::lock_guard<std::mutex> lk(s_mpSeenMu);
		s_mpSeenNames.clear();
	}

	{
		std::lock_guard<std::mutex> lk(s_mpPendingMu);
		s_mpPendingNames.clear();
		s_mpFlushingPending.store(false, std::memory_order_release);
	}

	Msg(eDLL_T::SERVER,
		"[s21-dedi] LevelShutdown: cleared SkinNames/modelprecache table "
		"caches + dedupe set + per-map counters (ODL registry persisted)\n");
}
