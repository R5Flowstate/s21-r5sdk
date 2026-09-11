//=============================================================================
//
// Purpose: raise SNDC slot limits and patch SendProp nElements to match S21 RecvTables.
//
//=============================================================================

#include "core/stdafx.h"
#include "game/shared/scriptnetdata_limits.h"
#include "game/shared/heap_canary.h"
#include "thirdparty/detours/include/detours.h"
#include <unordered_map>
#include <string>
#include <vector>

// Entity configuration table. Must match s_sndcSpecs[] / SendProp nElem patches.
static SNDCEntityConfig s_entityConfigs[SNDC_ENT_COUNT] = {
	// SNDC_GLOBAL mirrors the S21 client layout.
	{
		"GLOBAL", 3456,
		{
			{ 18, 18, 2944, 1 },  // Bool -- m_bools[18] @ 2944..2962
			{ 34, 34, 2962, 2 },  // Range -- m_ranges[34] @ 2962..3030
			{ 18, 18, 3032, 4 },  // BigInt -- m_int32s[18] @ 3032..3104
			{ 26, 26, 3104, 4 },  // Time -- m_times[26] @ 3104..3208
			{ 10, 10, 3208, 4 },  // Entity -- m_entities[10] @ 3208..3248
		},
		0, {}
	},
	// SNDC_PLAYER_GLOBAL -- same layout as GLOBAL
	{
		"PLAYER_GLOBAL", 3328,
		{
			{ 18, 18, 2832, 1 },  // Bool
			{ 30, 30, 2850, 2 },  // Range
			{ 34, 34, 2912, 4 },  // BigInt
			{ 14, 14, 3048, 4 },  // Time
			{  6,  6, 3104, 4 },  // Entity
		},
		0, {}
	},
	// SNDC_PLAYER_EXCLUSIVE -- B@2832(32) R@2864(60) I@2924(24) T@2948(40) E@2988(88) = end 3076
	{
		"PLAYER_EXCLUSIVE", 3328,
		{
			{ 32, 32, 2832, 1 },  // Bool
			{ 30, 30, 2864, 2 },  // Range
			{  6,  6, 2924, 4 },  // BigInt
			{ 10, 10, 2948, 4 },  // Time
			{ 22, 22, 2988, 4 },  // Entity
		},
		0, {}
	},
	// SNDC_TITAN_SOUL -- B@2832(10) R@2842(36) I@2880(16) T@2896(40) E@2936(24) = end 2960
	{
		"TITAN_SOUL", 3264,
		{
			{ 10, 10, 2832, 1 },  // Bool
			{ 18, 18, 2842, 2 },  // Range
			{  4,  4, 2880, 4 },  // BigInt
			{ 10, 10, 2896, 4 },  // Time
			{  6,  6, 2936, 4 },  // Entity
		},
		0, {}
	},
	// SNDC_DEATH_BOX -- B@2832(5) R@2838(8) I@2848(56) T@2904(12) E@2916(12) = end 2928
	{
		"DEATH_BOX", 3200,
		{
			{  5,  5, 2832, 1 },  // Bool
			{  4,  4, 2838, 2 },  // Range
			{ 14, 14, 2848, 4 },  // BigInt
			{  3,  3, 2904, 4 },  // Time
			{  3,  3, 2916, 4 },  // Entity
		},
		0, {}
	},
};

//=============================================================================
// 2. EXTENSION LAYOUT COMPUTATION
//=============================================================================

static void ComputeExtensionLayouts()
{
	for (int e = 0; e < SNDC_ENT_COUNT; e++)
	{
		auto& cfg = s_entityConfigs[e];
		int offset = cfg.origEntitySize;

		for (int t = 0; t < SNDC_ITYPE_COUNT; t++)
		{
			int delta = cfg.arrays[t].targetCount - cfg.arrays[t].origCount;
			if (delta <= 0)
			{
				cfg.extOffsets[t] = -1; // No extension needed
				continue;
			}

			// Align offset to element size
			int align = cfg.arrays[t].elemSize;
			if (align > 1)
				offset = (offset + align - 1) & ~(align - 1);

			cfg.extOffsets[t] = offset;
			offset += delta * cfg.arrays[t].elemSize;
		}

		// Align final size to 8 bytes
		cfg.extEntitySize = (offset + 7) & ~7;
	}

	for (int e = 0; e < SNDC_ENT_COUNT; e++)
	{
		const auto& cfg = s_entityConfigs[e];
		if (cfg.extEntitySize > cfg.origEntitySize)
		{
			DevMsg(eDLL_T::ENGINE,
				"[SNDC_Limits] %s: %d -> %d bytes (ext offsets: Bool=%d Range=%d Int32=%d Time=%d Ent=%d)\n",
				cfg.name, cfg.origEntitySize, cfg.extEntitySize,
				cfg.extOffsets[0], cfg.extOffsets[1], cfg.extOffsets[2],
				cfg.extOffsets[3], cfg.extOffsets[4]);
		}
	}
}


// NET_ScriptMessage extension storage for overflow vars.

#include "common/netmessages.h"
#include "game/shared/scriptnetdata_ext.h"
#include "game/client/scriptnetdata_client.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "engine/client/clientstate.h"
#include "engine/net_chan.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"

// Global extension state
SNDCExtCategory g_sndcExt[SNDC_ENT_COUNT] = {};
bool g_sndcExtActive = false;

// Forward declarations for globals resolved later in GetVar
// SERVER cat struct (524B stride, 7 cats) -- written by RegisterNetVar
static uintptr_t s_catStructBase = 0;
// CLIENT cat struct (264B stride, 7 cats) -- written by RegisterNetVar
// Field offsets are the SAME as SERVER (bool@+40, range@+56, int@+120, time@+168) but
// the per-cat stride is smaller because the callback table at +0x108 is shorter on CLIENT.
static uintptr_t s_catStructBaseClient = 0;

static const uint8_t s_itypeSizes[] = { 1, 2, 4, 4, 4 }; // Bool, Range, Int32, Time, Entity

static void InitExtensionStorage()
{
	memset(g_sndcExt, 0, sizeof(g_sndcExt));

	for (int e = 0; e < SNDC_ENT_COUNT; e++)
	{
		auto& ext = g_sndcExt[e];
		int idx = 0;

		for (int t = 0; t < SNDC_ITYPE_COUNT; t++)
		{
			int delta = s_entityConfigs[e].arrays[t].targetCount - s_entityConfigs[e].arrays[t].origCount;
			ext.typeStartIdx[t] = (delta > 0) ? idx : -1;
			ext.typeExtCount[t] = delta;

			for (int i = 0; i < delta && idx < SNDC_MAX_EXT_VARS; i++, idx++)
			{
				ext.meta[idx].internalType = static_cast<uint8_t>(t);
				ext.meta[idx].sizeBytes = s_itypeSizes[t];
			}
		}
		ext.totalExtVars = idx;

		if (idx > 0)
		{
			DevMsg(eDLL_T::ENGINE,
				"[SNDC_Ext] %s: %d extension vars (B:%d R:%d I:%d T:%d E:%d)\n",
				s_entityConfigs[e].name, idx,
				ext.typeExtCount[0], ext.typeExtCount[1], ext.typeExtCount[2],
				ext.typeExtCount[3], ext.typeExtCount[4]);
		}
	}

	g_sndcExtActive = true;
}

// Entity pointer globals, resolved in GetVar.
static uintptr_t* s_pGlobalEntClient = nullptr; // CLIENT entity (DT, vtable[233-237])
uintptr_t* s_pGlobalEntServer = nullptr; // SERVER entity (script Set/Get writes here!)
static uint8_t* g_pServerNetVarsBase = nullptr;
static uint8_t* g_pClientNetVarsBase = nullptr;



// Squirrel string hash -- must match SQString._hash used by engine hash tables
static uint64_t SNDC_HashVarName(const char* name)
{
	size_t l = strlen(name);
	uint64_t h = static_cast<uint64_t>(l);
	size_t step = (l >> 5) + 1;
	for (size_t l1 = l; l1 >= step; l1 -= step)
		h = h ^ ((h << 5) + (h >> 2) + static_cast<uint8_t>(name[l1 - 1]));
	return h;
}

// Name -> slot lookup map (built during SDKRegisterNetVar at registration time)
struct SNDCVarInfo { int category; int internalType; int scriptType; int slotIndex; };
static std::unordered_map<std::string, SNDCVarInfo> s_varLookupMap;
// Hash -> name reverse lookup (for server lookup hook which only receives hash)
static std::unordered_map<uint64_t, std::string> s_hashToNameMap;


//--- Test commands for verifying the NET_ScriptMessage pipeline ---
#include "tier1/cmd.h"
static void CC_SNDC_Diag(const CCommand& args)
{
	int ovCount = 0, normCount = 0;
	for (auto it = s_varLookupMap.begin(); it != s_varLookupMap.end(); ++it)
	{
		int oc = s_entityConfigs[it->second.category].arrays[it->second.internalType].origCount;
		if (it->second.slotIndex >= oc) ovCount++;
		else normCount++;
	}
	Msg(eDLL_T::ENGINE, "[SNDC_Diag] varLookupMap: %d total (%d normal, %d overflow)\n",
		(int)s_varLookupMap.size(), normCount, ovCount);

	Msg(eDLL_T::ENGINE, "[SNDC_Diag] --- OVERFLOW VARS ---\n");
	for (auto it = s_varLookupMap.begin(); it != s_varLookupMap.end(); ++it)
	{
		int oc = s_entityConfigs[it->second.category].arrays[it->second.internalType].origCount;
		if (it->second.slotIndex >= oc)
			Msg(eDLL_T::ENGINE, "  [OVF] '%s' cat=%d itype=%d stype=%d slot=%d\n",
				it->first.c_str(), it->second.category, it->second.internalType, it->second.scriptType, it->second.slotIndex);
	}
	Msg(eDLL_T::ENGINE, "[SNDC_Diag] --- NORMAL VARS ---\n");
	for (auto it = s_varLookupMap.begin(); it != s_varLookupMap.end(); ++it)
	{
		int oc = s_entityConfigs[it->second.category].arrays[it->second.internalType].origCount;
		if (it->second.slotIndex < oc)
			Msg(eDLL_T::ENGINE, "  [NRM] '%s' cat=%d type=%d slot=%d\n",
				it->first.c_str(), it->second.category, it->second.internalType, it->second.slotIndex);
	}

	auto& ext = g_sndcExt[SNDC_ENT_GLOBAL];
	Msg(eDLL_T::ENGINE, "[SNDC_Diag] GLOBAL ext: total=%d\n", ext.totalExtVars);
	for (int i = 0; i < ext.totalExtVars && i < 20; i++)
		Msg(eDLL_T::ENGINE, "  slot %d: srv=%d cli=%d\n", i, ext.values[0][i], ext.clientValues[0][i]);
}

static ConCommand sndc_diag("sndc_diag", CC_SNDC_Diag, "SNDC diagnostics", FCVAR_DEVELOPMENTONLY);

static void CC_SNDC_TestSet(const CCommand& args)
{
	if (!g_sndcExtActive) { Msg(eDLL_T::ENGINE, "[SNDC_Test] Not active\n"); return; }
	int val = (args.ArgC() > 1) ? atoi(args.Arg(1)) : 42;
	auto& ext = g_sndcExt[SNDC_ENT_GLOBAL];
	if (ext.totalExtVars <= 0) { Msg(eDLL_T::ENGINE, "[SNDC_Test] No ext vars for GLOBAL\n"); return; }
	ext.values[0][0] = val;
	ext.dirty[0][0] |= 1;
	ext.anyDirty[0] = true;
	Msg(eDLL_T::ENGINE, "[SNDC_Test] SET slot 0 = %d (dirty, will flush next tick)\n", val);
}
static void CC_SNDC_TestGet(const CCommand& args)
{
	if (!g_sndcExtActive) { Msg(eDLL_T::ENGINE, "[SNDC_Test] Not active\n"); return; }
	auto& ext = g_sndcExt[SNDC_ENT_GLOBAL];
	if (ext.totalExtVars <= 0) { Msg(eDLL_T::ENGINE, "[SNDC_Test] No ext vars for GLOBAL\n"); return; }
	int slot = (args.ArgC() > 1) ? atoi(args.Arg(1)) : 0;
	if (slot < 0 || slot >= ext.totalExtVars) slot = 0;
	Msg(eDLL_T::ENGINE, "[SNDC_Test] Slot %d: srv=%d cli=%d (typeStart: B=%d R=%d I=%d T=%d E=%d) total=%d dirty=%d\n",
		slot, ext.values[0][slot], ext.clientValues[0][slot],
		ext.typeStartIdx[0], ext.typeStartIdx[1], ext.typeStartIdx[2],
		ext.typeStartIdx[3], ext.typeStartIdx[4], ext.totalExtVars, ext.anyDirty[0] ? 1 : 0);
}
// SDK_SetNetVarExt <category> <type> <slot> <value>
static void CC_SNDC_SetExt(const CCommand& args)
{
	if (!g_sndcExtActive || args.ArgC() < 5) return;
	int cat = atoi(args.Arg(1));
	int type = atoi(args.Arg(2));
	int slot = atoi(args.Arg(3));
	int value = atoi(args.Arg(4));
	if (cat < 0 || cat >= SNDC_ENT_COUNT) return;
	auto& ext = g_sndcExt[cat];
	int origCount = s_entityConfigs[cat].arrays[type].origCount;
	if (slot < origCount) return; // Not overflow -- engine handles it
	int fi = ext.typeStartIdx[type];
	if (fi < 0) return;
	int ei = fi + (slot - origCount);
	if (ei >= ext.totalExtVars) return;
	int ps = 0; // GLOBAL; per-player TODO
	ext.values[ps][ei] = value;
	ext.dirty[ps][ei / 8] |= (1 << (ei % 8));
	ext.anyDirty[ps] = true;
}

static ConCommand sndc_test_set("sndc_test_set", CC_SNDC_TestSet, "Set SNDC ext GLOBAL slot 0", FCVAR_DEVELOPMENTONLY);
static ConCommand sndc_test_get("sndc_test_get", CC_SNDC_TestGet, "Read SNDC ext GLOBAL slot 0", FCVAR_DEVELOPMENTONLY);
// SDK_SetNetVarExt: gated DEVELOPMENTONLY -- arbitrary entity memory
// write via rcon must not be reachable on a public server.
static ConCommand sndc_set_ext("SDK_SetNetVarExt", CC_SNDC_SetExt, "Sync overflow netvar to SDK buffer: cat type slot value", FCVAR_DEVELOPMENTONLY);

// SDKNotifySetGlobalInt(name, value)
#include "game/shared/vscript_gamedll_defs.h"

// Deferred callback queue (for listen server -- can't call client VM from server frame)
struct DeferredCallback { std::string name; int32_t oldVal; int32_t newVal; };
static std::vector<DeferredCallback> s_deferredCallbacks;

// NonRewind callback polling (entity-backed, DT delivers data, we detect changes)
struct NonRewindCallbackVar { std::string name; int internalType; int slot; int32_t prevValue; };
static std::vector<NonRewindCallbackVar> s_nonRewindCallbackVars;

// Category data element sizes per internal type (used by NonRewind entity polling).
static constexpr int CATEGORY_ELEM_SIZES[5] = { 1, 2, 4, 4, 4 }; // Bool, Range, BigInt, Time, Entity

void SNDC_QueueDeferredCallback(const char* name, int32_t oldVal, int32_t newVal)
{
	s_deferredCallbacks.push_back({ name, oldVal, newVal });
}

// Track a NonRewind (category 5) var for change detection polling.
// Standard vars (categories 0-4) fire callbacks via script-side SDKNotifyVarChanged
// -- no polling needed. The script knows name+oldValue+newValue at Set time.
void SNDC_TrackNonRewindCallback(const char* name)
{
	auto it = s_varLookupMap.find(name);
	if (it == s_varLookupMap.end()) return;

	if (it->second.category != 5) return; // Standard vars use script-side notify

	int itype = it->second.internalType;
	int slot = it->second.slotIndex;

	for (auto& nrv : s_nonRewindCallbackVars)
		if (nrv.name == name) return;

	s_nonRewindCallbackVars.push_back({ name, itype, slot, 0 });
	DevMsg(eDLL_T::ENGINE, "[SNDC_CB] Tracking NonRewind callback for '%s' type=%d slot=%d\n",
		name, itype, slot);
}

// Client-context per-frame flush of dirty overflow vars.
SQRESULT ClientScript_SDKProcessCallbacks(HSQUIRRELVM v)
{
	// Phase 1: Deferred callbacks (from server Set wrappers + overflow replication)
	if (!s_deferredCallbacks.empty())
	{
		auto pending = std::move(s_deferredCallbacks);
		s_deferredCallbacks.clear();

		for (auto& cb : pending)
			SNDC_FireOverflowCallbacks(cb.name.c_str(), cb.oldVal, cb.newVal);
	}

	// Phase 2: NonRewind entity change detection
	// DT replicates NonRewind entity to client. We compare with previous values
	// and fire callbacks when changes are detected.
	if (g_pScriptNetDataNonRewindEnt && !s_nonRewindCallbackVars.empty())
	{
		static const int NR_OFFSETS[] = { 0xC40, 0xC50, 0xC90, 0xCB0, 0xD10 };
		uintptr_t ent = reinterpret_cast<uintptr_t>(g_pScriptNetDataNonRewindEnt);
		for (auto& nrVar : s_nonRewindCallbackVars)
		{
			if (nrVar.internalType < 0 || nrVar.internalType >= 5) continue;
			uintptr_t addr = ent + NR_OFFSETS[nrVar.internalType]
				+ nrVar.slot * CATEGORY_ELEM_SIZES[nrVar.internalType];
			int32_t curVal = 0;
			switch (CATEGORY_ELEM_SIZES[nrVar.internalType]) {
				case 1: curVal = *reinterpret_cast<uint8_t*>(addr); break;
				case 2: curVal = (int32_t)*reinterpret_cast<uint16_t*>(addr); break;
				case 4: curVal = *reinterpret_cast<int32_t*>(addr); break;
			}
			if (curVal != nrVar.prevValue)
			{
				int32_t oldVal = nrVar.prevValue;
				nrVar.prevValue = curVal;
				SNDC_FireOverflowCallbacks(nrVar.name.c_str(), oldVal, curVal);
			}
		}
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// (SNDCVarInfo + s_varLookupMap declared above near globals)

static bool SNDC_LookupVarByName(const char* name, int& outCat, int& outType, int& outSlot)
{
	auto it = s_varLookupMap.find(name);
	if (it == s_varLookupMap.end()) return false;
	outCat  = it->second.category;
	outType = it->second.internalType;
	outSlot = it->second.slotIndex;
	return true;
}

// Internal: write overflow value to our buffer by (cat, type, slot)
static bool SNDC_WriteOverflowValue(int cat, int type, int slot, int32_t value)
{
	if (cat < 0 || cat >= SNDC_ENT_COUNT) return false;
	if (type < 0 || type >= SNDC_ITYPE_COUNT) return false;

	auto& ext = g_sndcExt[cat];
	int origCount = s_entityConfigs[cat].arrays[type].origCount;
	if (slot < origCount) return false; // Not overflow -- engine handles it

	int fi = ext.typeStartIdx[type];
	if (fi < 0) return false;
	int ei = fi + (slot - origCount);
	if (ei < 0 || ei >= ext.totalExtVars) return false;

	int ps = 0; // GLOBAL=0; per-player TODO via entity resolver
	ext.values[ps][ei] = value;
	ext.dirty[ps][ei / 8] |= (1 << (ei % 8));
	ext.anyDirty[ps] = true;
	return true;
}

// SDKSetGlobalNetInt(name, value) -- drop-in companion to SetGlobalNetInt
// Script calls this AFTER SetGlobalNetInt. Syncs overflow vars to NET_ScriptMessage buffer.
// No-op for non-overflow vars. Scripts don't need to know slot/category/type.
SQRESULT ServerScript_SDKSetGlobalNetInt(HSQUIRRELVM v)
{
	if (!g_sndcExtActive) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	const SQChar* name = nullptr;
	SQInteger value = 0;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	sq_getinteger(v, 3, &value);

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
		SNDC_WriteOverflowValue(cat, type, slot, static_cast<int32_t>(value));

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKNotifyVarChanged(name, oldValue, newValue) -- server native.
SQRESULT ServerScript_SDKNotifyVarChanged(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	SQInteger oldVal = 0, newVal = 0;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	sq_getinteger(v, 3, &oldVal);
	sq_getinteger(v, 4, &newVal);

	SNDC_QueueDeferredCallback(name, static_cast<int32_t>(oldVal), static_cast<int32_t>(newVal));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKNotifyFloatVarChanged(name, oldFloat, newFloat) -- SERVER native
// Same as SDKNotifyVarChanged but for float/time types. Converts float bits to int32
// for the callback queue. FireSingleCallback reinterprets back to float.
SQRESULT ServerScript_SDKNotifyFloatVarChanged(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	SQFloat oldVal = 0.0f, newVal = 0.0f;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	sq_getfloat(v, 3, &oldVal);
	sq_getfloat(v, 4, &newVal);

	float oldF = static_cast<float>(oldVal);
	float newF = static_cast<float>(newVal);
	int32_t oldBits, newBits;
	memcpy(&oldBits, &oldF, sizeof(int32_t));
	memcpy(&newBits, &newF, sizeof(int32_t));

	SNDC_QueueDeferredCallback(name, oldBits, newBits);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKRegisterNetVar(name, category, scriptType)
// Tracks ALL var registrations. Only adds to overflow map when slot >= origCount.
// Called for EVERY RegisterNetworkedVariableSafe -- handles both non-overflow and overflow.
static int s_regCounter[SNDC_ENT_COUNT][SNDC_ITYPE_COUNT] = {};

SQRESULT ServerScript_SDKRegisterNetVar(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	SQInteger cat = 0, type = 0;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	sq_getinteger(v, 3, &cat);
	sq_getinteger(v, 4, &type);

	// Map script type (SNVT) to internal type (SNDC_ITYPE)
	static const int snvtToItype[] = { 0, 1, 1, 2, 1, 1, 3, 4 };
	int itype = (type >= 0 && type < 8) ? snvtToItype[type] : -1;
	if (itype < 0 || cat < 0 || cat >= SNDC_ENT_COUNT) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	// Skip if already registered (prevents double-counting on listen server).
	// NO hash injection -- overflow slots in engine hash tables cause category
	// struct corruption when engine Set writes past bounds for Int/Range types.
	auto existing = s_varLookupMap.find(name);
	if (existing != s_varLookupMap.end())
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	// This var's slot is the current count for this (cat, type)
	int slot = s_regCounter[cat][itype];
	s_regCounter[cat][itype]++;

	// Always add to map (needed for SDKIsOverflowVar + Get routing).
	int origCount = s_entityConfigs[cat].arrays[itype].origCount;
	if (slot < s_entityConfigs[cat].arrays[itype].targetCount)
	{
		s_varLookupMap[name] = { (int)cat, itype, (int)type, slot };

		uint64_t nameHash = SNDC_HashVarName(name);
		s_hashToNameMap[nameHash] = name;

		if (slot >= origCount)
			DevMsg(eDLL_T::ENGINE, "[SNDC_Reg] Overflow '%s' -> cat=%d itype=%d stype=%d slot=%d (orig=%d)\n",
				name, (int)cat, itype, (int)type, slot, origCount);
		// NO hash injection -- SDKGetNetworkedVariableIndex searches s_varLookupMap directly.
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKIsOverflowVar(name) -> bool
// Returns true if the var is in the overflow map (registered via SDKRegisterNetVar).
// Used by script wrappers to route Get calls to engine vs SDK.
SQRESULT ServerScript_SDKIsOverflowVar(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	int cat, type, slot;
	bool found = SNDC_LookupVarByName(name, cat, type, slot);
	sq_pushbool(v, found && slot >= s_entityConfigs[cat].arrays[type].origCount);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKSetGlobalNetBool(name, value)
SQRESULT ServerScript_SDKSetGlobalNetBool(HSQUIRRELVM v)
{
	if (!g_sndcExtActive) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	const SQChar* name = nullptr;
	SQBool value = false;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	sq_getbool(v, 3, &value);

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
		SNDC_WriteOverflowValue(cat, type, slot, value ? 1 : 0);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKSetGlobalNetFloat(name, value) -- for FLOAT_RANGE, FLOAT_OVER_TIME
SQRESULT ServerScript_SDKSetGlobalNetFloat(HSQUIRRELVM v)
{
	if (!g_sndcExtActive) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	const SQChar* name = nullptr;
	SQFloat fvalue = 0.0f;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	sq_getfloat(v, 3, &fvalue);

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
	{
		float f = static_cast<float>(fvalue);
		SNDC_WriteOverflowValue(cat, type, slot, *reinterpret_cast<int32_t*>(&f));
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKSetGlobalNetTime(name, value) -- for TIME type
SQRESULT ServerScript_SDKSetGlobalNetTime(HSQUIRRELVM v)
{
	if (!g_sndcExtActive) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	const SQChar* name = nullptr;
	SQFloat fvalue = 0.0f;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	sq_getfloat(v, 3, &fvalue);

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
	{
		float f = static_cast<float>(fvalue);
		SNDC_WriteOverflowValue(cat, type, slot, *reinterpret_cast<int32_t*>(&f));
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKSetGlobalNetEntity(name, entity) -- for ENTITY type
SQRESULT ServerScript_SDKSetGlobalNetEntity(HSQUIRRELVM v)
{
	if (!g_sndcExtActive) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name) SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	// Read entity handle as int (entity index / ehandle)
	SQInteger entHandle = 0;
	sq_getinteger(v, 3, &entHandle);

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
		SNDC_WriteOverflowValue(cat, type, slot, static_cast<int32_t>(entHandle));

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Client-side Get natives for overflow vars.
static SQRESULT ServerSDKGet_ByName(HSQUIRRELVM v, int pushType)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
	{
		if (pushType == 0) sq_pushbool(v, false);
		else if (pushType == 1) sq_pushinteger(v, 0);
		else sq_pushfloat(v, 0.0f);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
	{
		int origCount = s_entityConfigs[cat].arrays[type].origCount;
		if (slot >= origCount)
		{
			auto& ext = g_sndcExt[cat];
			int fi = ext.typeStartIdx[type];
			if (fi >= 0)
			{
				int ei = fi + (slot - origCount);
				if (ei < ext.totalExtVars)
				{
					int32_t raw = ext.values[0][ei];
					if (pushType == 0) sq_pushbool(v, raw != 0);
					else if (pushType == 1) sq_pushinteger(v, raw);
					else sq_pushfloat(v, *reinterpret_cast<float*>(&raw));
					SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
				}
			}
		}
	}

	if (pushType == 0) sq_pushbool(v, false);
	else if (pushType == 1) sq_pushinteger(v, 0);
	else sq_pushfloat(v, 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT ServerScript_SDKGetGlobalNetBool(HSQUIRRELVM v)   { return ServerSDKGet_ByName(v, 0); }
SQRESULT ServerScript_SDKGetGlobalNetInt(HSQUIRRELVM v)    { return ServerSDKGet_ByName(v, 1); }
SQRESULT ServerScript_SDKGetGlobalNetFloat(HSQUIRRELVM v)  { return ServerSDKGet_ByName(v, 2); }
SQRESULT ServerScript_SDKGetGlobalNetTime(HSQUIRRELVM v)   { return ServerSDKGet_ByName(v, 2); }
SQRESULT ServerScript_SDKGetGlobalNetEntity(HSQUIRRELVM v) { return ServerSDKGet_ByName(v, 1); }

// CLIENT-side Get natives -- read from clientValues (NET_ScriptMessage replicated)
SQRESULT ClientScript_SDKGetGlobalNetInt(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
	{
		sq_pushinteger(v, 0);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
	{
		int origCount = s_entityConfigs[cat].arrays[type].origCount;
		if (slot >= origCount)
		{
			auto& ext = g_sndcExt[cat];
			int fi = ext.typeStartIdx[type];
			if (fi >= 0)
			{
				int ei = fi + (slot - origCount);
				if (ei < ext.totalExtVars)
				{
					sq_pushinteger(v, ext.clientValues[0][ei]);
					SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
				}
			}
		}
	}

	// Non-overflow or not found -- call original engine Get
	// (can't easily call original from here, return 0 as fallback)
	sq_pushinteger(v, 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKGetGlobalNetFloat(name) -> float
SQRESULT ClientScript_SDKGetGlobalNetFloat(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
	{
		sq_pushfloat(v, 0.0f);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
	{
		int origCount = s_entityConfigs[cat].arrays[type].origCount;
		if (slot >= origCount)
		{
			auto& ext = g_sndcExt[cat];
			int fi = ext.typeStartIdx[type];
			if (fi >= 0)
			{
				int ei = fi + (slot - origCount);
				if (ei < ext.totalExtVars)
				{
					int32_t raw = ext.clientValues[0][ei];
					sq_pushfloat(v, *reinterpret_cast<float*>(&raw));
					SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
				}
			}
		}
	}

	sq_pushfloat(v, 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKGetGlobalNetBool(name) -> bool
SQRESULT ClientScript_SDKGetGlobalNetBool(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
	{
		int origCount = s_entityConfigs[cat].arrays[type].origCount;
		if (slot >= origCount)
		{
			auto& ext = g_sndcExt[cat];
			int fi = ext.typeStartIdx[type];
			if (fi >= 0)
			{
				int ei = fi + (slot - origCount);
				if (ei < ext.totalExtVars)
				{
					sq_pushbool(v, ext.clientValues[0][ei] != 0);
					SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
				}
			}
		}
	}

	sq_pushbool(v, false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKGetGlobalNetTime(name) -> float (same as Float, Time is stored as float bits)
SQRESULT ClientScript_SDKGetGlobalNetTime(HSQUIRRELVM v)
{
	return ClientScript_SDKGetGlobalNetFloat(v);
}

// SDKGetGlobalNetEntity(name) -> int (entity handle as integer)
SQRESULT ClientScript_SDKGetGlobalNetEntity(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
	{
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
	{
		int origCount = s_entityConfigs[cat].arrays[type].origCount;
		if (slot >= origCount)
		{
			auto& ext = g_sndcExt[cat];
			int fi = ext.typeStartIdx[type];
			if (fi >= 0)
			{
				int ei = fi + (slot - origCount);
				if (ei < ext.totalExtVars)
				{
					sq_pushinteger(v, ext.clientValues[0][ei]);
					SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
				}
			}
		}
	}

	sq_pushinteger(v, -1);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// SDKGetNetworkedVariableIndex(name) -> int
SQRESULT Script_SDKGetNetworkedVariableIndex(HSQUIRRELVM v)
{
	const SQChar* name = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &name)) || !name)
	{
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// For overflow vars: return the slot from s_varLookupMap.
	// This isn't a real hash table index, but it identifies the var for SDK operations.
	// For non-overflow vars: search the engine hash tables for the real index.
	int cat, type, slot;
	if (SNDC_LookupVarByName(name, cat, type, slot))
	{
		int origCount = s_entityConfigs[cat].arrays[type].origCount;
		if (slot >= origCount)
		{
			// Overflow var -- return slot as identifier (no engine hash entry exists)
			sq_pushinteger(v, slot);
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
	}

	// Non-overflow: search engine hash tables for the real entry index
	uint64_t nameHash = SNDC_HashVarName(name);
	auto searchTable = [&](uint8_t* table, int entrySize) -> int {
		if (!table) return -1;
		uint64_t startBucket = nameHash % 250;
		for (int probe = 0; probe < 250; probe++)
		{
			uint64_t idx = (startBucket + probe) % 250;
			uint8_t* entry = table + entrySize * idx;
			uint64_t stored = *reinterpret_cast<uint64_t*>(entry);
			if (stored == nameHash) return static_cast<int>(idx);
			if (stored == 0) break;
		}
		return -1;
	};

	int idx = searchTable(g_pClientNetVarsBase, 32);
	if (idx < 0)
		idx = searchTable(g_pServerNetVarsBase, 56);

	sq_pushinteger(v, idx);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// NET_ScriptMessage replication: server flush + client receive.
static bool SNDC_BuildDeltaMessage(NET_ScriptMessage& msg, int cat, int playerSlot)
{
	auto& ext = g_sndcExt[cat];
	if (!ext.anyDirty[playerSlot])
		return false;

	msg.InitWrite();
	msg.m_bIsTyped = false; // SNDC extension data, NOT a remote function call

	bf_write& out = msg.m_DataOut;
	out.WriteByte(SNDC_MSG_MAGIC);    // 0xA7
	out.WriteByte(SNDC_MSG_VERSION);
	out.WriteByte(SNDC_SUBMSG_DELTA); // delta update
	out.WriteUBitLong(cat, 4);
	out.WriteUBitLong(playerSlot < 127 ? playerSlot : 127, 7);

	// Count dirty vars first
	int dirtyCount = 0;
	for (int i = 0; i < ext.totalExtVars; i++)
		if (ext.dirty[playerSlot][i / 8] & (1 << (i % 8)))
			dirtyCount++;

	if (dirtyCount == 0)
	{
		ext.anyDirty[playerSlot] = false;
		return false;
	}

	out.WriteByte(dirtyCount > 255 ? 255 : dirtyCount);

	int written = 0;
	for (int i = 0; i < ext.totalExtVars && written < 255; i++)
	{
		if (!(ext.dirty[playerSlot][i / 8] & (1 << (i % 8))))
			continue;

		out.WriteUBitLong(i, 7);                          // extIndex (0-127)
		out.WriteUBitLong(ext.meta[i].internalType, 3);   // type (0-4)
		out.WriteLong(ext.values[playerSlot][i]);          // value (always 4 bytes)
		written++;

		// Safety: don't overflow the 2048-byte buffer
		if (out.GetNumBitsWritten() > (NET_ScriptMessage::SCRIPT_MESSAGE_BUFFER_SIZE - 64) * 8)
			break;
	}

	// Clear dirty state
	memset(ext.dirty[playerSlot], 0, sizeof(ext.dirty[playerSlot]));
	ext.anyDirty[playerSlot] = false;

	return !out.IsOverflowed() && written > 0;
}

// Server tick flush: send dirty extension vars to clients.
// Called from CServer::RunFrame after the original frame completes.
void SNDC_FlushDirtyVars(void* pServerRaw)
{
	if (!g_sndcExtActive || !pServerRaw)
		return;

	CServer* pServer = static_cast<CServer*>(pServerRaw);

	for (int cat = 0; cat < SNDC_ENT_COUNT; cat++)
	{
		auto& ext = g_sndcExt[cat];
		if (ext.totalExtVars == 0)
			continue;

		// GLOBAL (cat 0): single buffer at playerSlot 0, broadcast to all
		// Per-player categories: one buffer per player slot
		int maxSlot = (cat == SNDC_ENT_GLOBAL) ? 1 : MAX_PLAYERS;

		for (int ps = 0; ps < maxSlot; ps++)
		{
			if (!ext.anyDirty[ps])
				continue;

			NET_ScriptMessage msg;
			if (!SNDC_BuildDeltaMessage(msg, cat, ps))
				continue;

			// Send to appropriate clients
			if (cat == SNDC_ENT_PLAYER_EXCLUSIVE)
			{
				// Exclusive: only to the owning client
				CClient* pClient = pServer->GetClient(ps);
				if (pClient && pClient->IsActive())
					pClient->SendNetMsgEx(&msg, false, true, false);
			}
			else
			{
				// GLOBAL, PLAYER_GLOBAL, TITAN_SOUL, DEATH_BOX: broadcast
				for (int c = 0; c < MAX_PLAYERS; c++)
				{
					CClient* pClient = pServer->GetClient(c);
					if (!pClient || !pClient->IsActive())
						continue;

					// Listen server host: direct memcpy + queue callbacks for deferred firing
					CNetChan* pNetChan = pClient->GetNetChan();
					if (!pNetChan || pNetChan->GetRemoteAddress().IsLoopback())
					{
						for (int i = 0; i < ext.totalExtVars; i++)
						{
							int32_t oldVal = ext.clientValues[ps][i];
							int32_t newVal = ext.values[ps][i];
							ext.clientValues[ps][i] = newVal;
							if (oldVal != newVal)
							{
								// Queue callback for deferred firing in client context
								for (auto it = s_varLookupMap.begin(); it != s_varLookupMap.end(); ++it)
								{
									if (it->second.category == cat)
									{
										int oc = s_entityConfigs[cat].arrays[it->second.internalType].origCount;
										if (it->second.slotIndex < oc) continue;
										int fi = ext.typeStartIdx[it->second.internalType];
										if (fi >= 0 && i == fi + (it->second.slotIndex - oc))
										{
											// Queue: store name + old + new for deferred firing
											SNDC_QueueDeferredCallback(it->first.c_str(), oldVal, newVal);
											break;
										}
									}
								}
							}
						}
						continue;
					}

					pClient->SendNetMsgEx(&msg, false, true, false);
				}
			}
		}
	}
}

// Client-side receive handler: validate and store incoming extension data.
// Called from NET_ScriptMessage::Process when m_bIsTyped=false.
bool SNDC_ProcessClientMessage(NET_ScriptMessage* pMsg)
{
	if (!g_sndcExtActive || !pMsg)
		return false;

	bf_read& in = pMsg->m_DataIn;

	// Validate magic
	int magic = in.ReadByte();
	if (magic != SNDC_MSG_MAGIC)
		return false; // Not an SNDC message -- silently ignore

	int version = in.ReadByte();
	if (version != SNDC_MSG_VERSION)
	{
		Warning(eDLL_T::CLIENT, "[SNDC_Ext] Unknown protocol version %d, ignoring\n", version);
		return false;
	}

	int subType = in.ReadByte(); (void)subType; // delta vs fullsync (same handling)
	int category = in.ReadUBitLong(4);
	int playerSlot = in.ReadUBitLong(7);
	int varCount = in.ReadByte();

	// Bounds check category
	if (category >= SNDC_ENT_COUNT)
	{
		Warning(eDLL_T::CLIENT, "[SNDC_Ext] Invalid category %d\n", category);
		return false;
	}

	auto& ext = g_sndcExt[category];

	// Bounds check player slot
	if (playerSlot >= MAX_PLAYERS)
		playerSlot = 0;

	for (int i = 0; i < varCount; i++)
	{
		if (in.IsOverflowed())
			break;

		int extIndex = in.ReadUBitLong(7);
		int internalType = in.ReadUBitLong(3);
		int32_t value = in.ReadLong();

		// Validate extIndex bounds
		if (extIndex >= ext.totalExtVars)
			continue; // Skip invalid entry

		// Validate type matches expected
		if (internalType != ext.meta[extIndex].internalType)
			continue; // Type mismatch -- skip

		// Write to client mirror buffer + fire callbacks if changed
		int32_t oldValue = ext.clientValues[playerSlot][extIndex];
		ext.clientValues[playerSlot][extIndex] = value;

		// Event-driven callbacks: fire immediately when overflow value changes
		if (oldValue != value)
		{
			// Reverse-lookup the var name from extIndex for callback dispatch (overflow only)
			for (auto it = s_varLookupMap.begin(); it != s_varLookupMap.end(); ++it)
			{
				if (it->second.category == category && it->second.internalType == internalType)
				{
					int origCount = s_entityConfigs[category].arrays[internalType].origCount;
					if (it->second.slotIndex < origCount) continue; // skip non-overflow
					int fi = ext.typeStartIdx[internalType];
					if (fi >= 0 && extIndex == fi + (it->second.slotIndex - origCount))
					{
						SNDC_FireOverflowCallbacks(it->first.c_str(), oldValue, value);
						break;
					}
				}
			}
		}
	}

	return true;
}

// Level shutdown: zero all extension buffers, reset repair flag
void SNDC_ExtensionLevelShutdown()
{
	for (int cat = 0; cat < SNDC_ENT_COUNT; cat++)
	{
		auto& ext = g_sndcExt[cat];
		memset(ext.values, 0, sizeof(ext.values));
		memset(ext.dirty, 0, sizeof(ext.dirty));
		memset(ext.anyDirty, 0, sizeof(ext.anyDirty));
		memset(ext.clientValues, 0, sizeof(ext.clientValues));
	}
	// Reset state for next map load
	s_varLookupMap.clear();
	s_hashToNameMap.clear();
	memset(s_regCounter, 0, sizeof(s_regCounter));
	s_deferredCallbacks.clear();
	s_nonRewindCallbackVars.clear();
}

// Full sync: send ALL current extension values to a newly connected player.
// Called from CClient::VActivatePlayer after the player is fully connected.
void SNDC_SendFullSync(CClient* pClient)
{
	if (!g_sndcExtActive || !pClient || !pClient->IsActive())
		return;

	for (int cat = 0; cat < SNDC_ENT_COUNT; cat++)
	{
		auto& ext = g_sndcExt[cat];
		if (ext.totalExtVars == 0)
			continue;

		int maxSlot = (cat == SNDC_ENT_GLOBAL) ? 1 : MAX_PLAYERS;

		for (int ps = 0; ps < maxSlot; ps++)
		{
			// Check if any values are non-zero for this slot
			bool hasData = false;
			for (int i = 0; i < ext.totalExtVars && !hasData; i++)
				hasData = (ext.values[ps][i] != 0);
			if (!hasData)
				continue;

			NET_ScriptMessage msg;
			msg.InitWrite();
			msg.m_bIsTyped = false;

			bf_write& out = msg.m_DataOut;
			out.WriteByte(SNDC_MSG_MAGIC);
			out.WriteByte(SNDC_MSG_VERSION);
			out.WriteByte(SNDC_SUBMSG_FULLSYNC);
			out.WriteUBitLong(cat, 4);
			out.WriteUBitLong(ps < 127 ? ps : 127, 7);
			out.WriteByte(ext.totalExtVars);

			for (int i = 0; i < ext.totalExtVars; i++)
			{
				out.WriteUBitLong(i, 7);
				out.WriteUBitLong(ext.meta[i].internalType, 3);
				out.WriteLong(ext.values[ps][i]);
			}

			if (!out.IsOverflowed())
				pClient->SendNetMsgEx(&msg, false, true, false);
		}
	}

	DevMsg(eDLL_T::ENGINE, "[SNDC_Ext] Sent full sync to player %d\n", pClient->GetUserID());
}


//=============================================================================
// REGISTRATION LOGGER -- hooks AllocateInternalVar to log every slot allocation
//=============================================================================

static __int64 (*v_AllocateInternalVar)(__int64 catStruct, int slotType) = nullptr;


static __int64 Hook_AllocateInternalVar(__int64 catStruct, int slotType)
{
	// PURE PASSTHROUGH -- the JL->JMP patch inside the original function handles overflow.
	// We just log and call original. The original AllocVar always succeeds (JL->JMP patched).
	int curCount = *reinterpret_cast<int*>(catStruct + 4 * slotType + 20);
	int maxSlots = *reinterpret_cast<int*>(catStruct + 4 * slotType);
	__int64 result = v_AllocateInternalVar(catStruct, slotType);

	if (curCount >= maxSlots)
		DevMsg(eDLL_T::ENGINE, "[SNDC_AllocVar] Overflow: type=%d slot=%lld (max=%d) — engine handled via JL->JMP\n",
			slotType, result, maxSlots);

	return result;
}


// IDetour implementation.

ConVar sndc_native_layout("sndc_native_layout", "1", FCVAR_RELEASE,
	"Relocate SNDC arrays to S21 native packed layout (vtable + SendProp patches). ON BY DEFAULT.");

// PE_EXPANDED C++ vtable fork (file scope for SNDC_Get/Apply exports).
static uintptr_t* s_peExpandedVtable = nullptr;
static bool s_nativeLayoutApplied = false;

namespace {

struct VtableInplacePatch {
	const char* label;
	uintptr_t funcRVA;   // function start RVA in r5apex_ds.exe
	int funcSize;        // bytes
	int s3Disp;          // current disp32 literal
	int targetDisp;      // new disp32 literal
	int expectedSites;   // 1 for GET, 2 for SET (CMP + MOV)
};

struct VtableCopyPatch {
	const char* label;
	uintptr_t srcFuncRVA;  // function to copy (after any in-place patch)
	int srcFuncSize;
	int srcCurrentDisp;    // disp32 value in the source function (post in-place if applicable)
	int copyTargetDisp;    // disp32 value for the copy
	int expectedSites;
	uintptr_t vtableRVA;   // vtable RVA to update slot in
	int vtableSlot;        // slot index (281..290)
};

// Forward declarations (defined below PatchSNDCNativeLayout).
static void BuildPEExpandedVtable(uintptr_t base);

// In-place patches: 24 unique-target functions + 6 shared funcs (where primary cat gets in-place, secondary gets copy).
// RVAs are absolute -.
static const VtableInplacePatch s_inplacePatches[] = {
	// GLOBAL unique: mirror S21 client offsets
	// (m_entities@3208 per). All shared functions below also
	// re-targeted to the S21 client layout for GLOBAL.
	{ "GLOBAL.get_entity",   0xC15850, 0x10, 3040, 3208, 1 },
	{ "GLOBAL.set_entity",   0xC159B0, 0x44, 3040, 3208, 2 },

	// PG unique (PG m_int32s relocates by +8, m_times by +96, m_entities by +120)
	{ "PG.get_int",          0xC16110, 0x0B, 2904, 2912, 1 },
	{ "PG.set_int",          0xC16190, 0x48, 2904, 2912, 2 },
	{ "PG.get_time",         0xC16120, 0x0D, 2952, 3048, 1 },
	{ "PG.set_time",         0xC161E0, 0x51, 2952, 3048, 2 },
	{ "PG.get_entity",       0xC16130, 0x10, 2984, 3104, 1 },
	{ "PG.set_entity",       0xC16240, 0x44, 2984, 3104, 2 },

	// PE unique (only get_entity/set_entity, others shared with GLOBAL)
	{ "PE.get_entity",       0xC16990, 0x10, 2976, 2988, 1 },
	{ "PE.set_entity",       0xC169A0, 0x44, 2976, 2988, 2 },

	// TITAN unique
	{ "TITAN.get_int",       0xC170F0, 0x0B, 2872, 2880, 1 },
	{ "TITAN.set_int",       0xC17120, 0x48, 2872, 2880, 2 },
	{ "TITAN.get_time",      0xC17100, 0x0D, 2880, 2896, 1 },
	{ "TITAN.set_time",      0xC17170, 0x51, 2880, 2896, 2 },
	{ "TITAN.get_entity",    0xC17110, 0x10, 2912, 2936, 1 },
	{ "TITAN.set_entity",    0xC171D0, 0x44, 2912, 2936, 2 },

	// DEATHBOX unique
	{ "DEATHBOX.get_range",  0xC17920, 0x0C, 2834, 2838, 1 },
	{ "DEATHBOX.set_range",  0xC17960, 0x4C, 2834, 2838, 2 },
	{ "DEATHBOX.get_int",    0xC17930, 0x0B, 2868, 2848, 1 },
	{ "DEATHBOX.set_int",    0xC179B0, 0x48, 2868, 2848, 2 },
	{ "DEATHBOX.get_time",   0xC17940, 0x0D, 2872, 2904, 1 },
	{ "DEATHBOX.set_time",   0xC17A00, 0x51, 2872, 2904, 2 },
	{ "DEATHBOX.get_entity", 0xC17950, 0x10, 2876, 2916, 1 },
	{ "DEATHBOX.set_entity", 0xC17A60, 0x44, 2876, 2916, 2 },

	// Shared GLOBAL+PE -- in-place to GLOBAL's S21-client target (PE gets a COPY below).
	{ "GLOBAL.get_range",    0xC15820, 0x0C, 2848, 2962, 1 }, // PE wants 2864 (copy)
	{ "GLOBAL.set_range",    0xC158B0, 0x4C, 2848, 2962, 2 },
	{ "GLOBAL.get_int",      0xC15830, 0x0B, 2912, 3032, 1 }, // PE wants 2924 (copy)
	{ "GLOBAL.set_int",      0xC15900, 0x48, 2912, 3032, 2 },
	{ "GLOBAL.get_time",     0xC15840, 0x0D, 2944, 3104, 1 }, // PE wants 2948 (copy)
	{ "GLOBAL.set_time",     0xC15950, 0x51, 2944, 3104, 2 },

	// Shared PG+TITAN -- in-place to PG's target (TITAN gets a COPY below)
	{ "PG.get_range",        0xC16100, 0x0C, 2840, 2850, 1 }, // TITAN wants 2842 (copy)
	{ "PG.set_range",        0xC16140, 0x4C, 2840, 2850, 2 },
};

// Copy patches: shared functions where a secondary category wants a different target.
static const VtableCopyPatch s_copyPatches[] = {
	// GLOBAL targets re-derived from the S21 client layout.
	{ "PE.get_int(copy)",    0xC15830, 0x0B, 3032, 2924, 1, 0x14F3DC0, 283 },
	{ "PE.set_int(copy)",    0xC15900, 0x48, 3032, 2924, 2, 0x14F3DC0, 288 },
	// get_range/set_range: GLOBAL in-place 2848->2962; PE wants 2864.
	{ "PE.get_range(copy)",  0xC15820, 0x0C, 2962, 2864, 1, 0x14F3DC0, 282 },
	{ "PE.set_range(copy)",  0xC158B0, 0x4C, 2962, 2864, 2, 0x14F3DC0, 287 },
	// get_time/set_time: GLOBAL in-place 2944->3104; PE wants 2948.
	{ "PE.get_time(copy)",   0xC15840, 0x0D, 3104, 2948, 1, 0x14F3DC0, 284 },
	{ "PE.set_time(copy)",   0xC15950, 0x51, 3104, 2948, 2, 0x14F3DC0, 289 },

	// TITAN shares get_range/set_range with PG. PG patched to 2850, TITAN wants 2842.
	{ "TITAN.get_range(copy)", 0xC16100, 0x0C, 2850, 2842, 1, 0x14EFA50, 282 },
	{ "TITAN.set_range(copy)", 0xC16140, 0x4C, 2850, 2842, 2, 0x14EFA50, 287 },

	// GLOBAL get_bool/set_bool need their own copies; they cannot share the other-cat thunk.
	{ "GLOBAL.get_bool(copy)", 0xC15810, 0x0C, 2832, 2944, 1, 0x14ECB20, 281 },
	{ "GLOBAL.set_bool(copy)", 0xC15860, 0x49, 2832, 2944, 2, 0x14ECB20, 286 },
};

// Apply a disp32 replacement to a function body. Returns sites patched.
// Verifies expected site count BEFORE writing (safety: no partial patch).
static int ApplyDispPatch(uint8_t* fn, int fnSize, int s3Disp, int targetDisp,
                          int expectedSites, const char* label)
{
	int sites = 0;
	for (int i = 0; i + 4 <= fnSize; ++i)
	{
		if (*reinterpret_cast<int32_t*>(fn + i) == s3Disp) ++sites;
	}
	if (sites != expectedSites)
	{
		Warning(eDLL_T::ENGINE,
			"[SNDC_Native] %s: disp32=%d found %d sites, expected %d -- SKIP\n",
			label, s3Disp, sites, expectedSites);
		return 0;
	}

	DWORD oldProt = 0;
	if (!VirtualProtect(fn, fnSize, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[SNDC_Native] %s: VirtualProtect RWX failed -- SKIP\n", label);
		return 0;
	}
	int patched = 0;
	for (int i = 0; i + 4 <= fnSize; ++i)
	{
		int32_t* p = reinterpret_cast<int32_t*>(fn + i);
		if (*p == s3Disp)
		{
			*p = targetDisp;
			++patched;
		}
	}
	VirtualProtect(fn, fnSize, oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), fn, fnSize);

	(void)label;
	return patched;
}

// Relocate RIP-relative instructions after memcpy moves a function.
static int RelocateRipRelativeInCopy(uint8_t* copy, int copySize, uintptr_t origBase)
{
	int relocated = 0;
	int64_t delta = static_cast<int64_t>(origBase) - reinterpret_cast<int64_t>(copy);
	for (int i = 0; i + 7 <= copySize; ++i)
	{
		// Reloc patterns: 3-byte opcode + 4-byte disp32.
		const bool isRipRel = (copy[i] == 0x48) &&
		                      ((copy[i+1] == 0x8B && copy[i+2] == 0x05) ||
		                       (copy[i+1] == 0x8D && copy[i+2] == 0x05) ||
		                       (copy[i+1] == 0x89 && copy[i+2] == 0x05));
		if (!isRipRel) continue;

		int32_t* dispP = reinterpret_cast<int32_t*>(copy + i + 3);
		int64_t origDisp = *dispP;
		int64_t newDisp = origDisp + delta;
		if (newDisp < INT32_MIN || newDisp > INT32_MAX)
		{
			Warning(eDLL_T::ENGINE,
				"[SNDC_Native] RIP-relative reloc out of range at copy+0x%x (delta=%lld)\n",
				i, static_cast<long long>(delta));
			continue;
		}
		*dispP = static_cast<int32_t>(newDisp);
		++relocated;
		// Skip past this instruction to avoid double-counting
		i += 6;
	}
	return relocated;
}

// Allocate executable memory within +/-256MB of the module so RIP-relative
// disp32 references in copied functions fit in INT32. Mirrors the
// AllocNearModule pattern in sndc_alloc_hook.cpp.
static void* AllocExecNearModule(uintptr_t base, size_t size)
{
	for (int dir = -1; dir <= 1; dir += 2)
	{
		for (int attempt = 1; attempt < 4096; ++attempt)
		{
			uintptr_t addr = (base + dir * attempt * 0x10000ULL) & ~0xFFFFULL;
			if (dir < 0 && addr > base) continue; // underflow guard
			int64_t dist = (int64_t)addr - (int64_t)base;
			if (dist > 0x10000000LL || dist < -0x10000000LL) break; // +/-256 MB
			MEMORY_BASIC_INFORMATION mbi;
			if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)
				&& mbi.State == MEM_FREE && mbi.RegionSize >= size)
			{
				void* p = VirtualAlloc((void*)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (p) return p;
			}
		}
	}
	return nullptr;
}

// Allocate executable memory, copy the source function bytes, patch the
// disp32 in the copy AND relocate RIP-relative offsets, redirect the
// secondary cat's vtable slot to the copy.
static bool ApplyCopyPatch(uintptr_t base, const VtableCopyPatch& cp)
{
	uint8_t* src = reinterpret_cast<uint8_t*>(base + cp.srcFuncRVA);

	// Use near-module allocator so RIP-relative disp32 references in the copy
	// stay within INT32 range. Vanilla VirtualAlloc returns memory far from
	// the module, breaking RIP-relative relocation for those instructions.
	void* copy = AllocExecNearModule(base, cp.srcFuncSize);
	if (!copy)
	{
		Warning(eDLL_T::ENGINE,
			"[SNDC_Native] %s: AllocExecNearModule(%d) failed\n", cp.label, cp.srcFuncSize);
		return false;
	}
	memcpy(copy, src, cp.srcFuncSize);

	// 1. Relocate RIP-relative instructions FIRST so disp32 patch search below
	// doesn't accidentally match a relocated displacement that contains the
	// original disp value (unlikely but defensive).
	int ripReloc = RelocateRipRelativeInCopy(
		static_cast<uint8_t*>(copy), cp.srcFuncSize,
		reinterpret_cast<uintptr_t>(src));

	// 2. Patch the m_<type> offset disp32(s) -- this is the per-cat target offset
	// for the array we're moving.
	int sites = 0;
	for (int i = 0; i + 4 <= cp.srcFuncSize; ++i)
	{
		int32_t* p = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(copy) + i);
		if (*p == cp.srcCurrentDisp) ++sites;
	}
	if (sites != cp.expectedSites)
	{
		Warning(eDLL_T::ENGINE,
			"[SNDC_Native] %s: copy disp32=%d found %d sites, expected %d -- ABORT (ripReloc=%d)\n",
			cp.label, cp.srcCurrentDisp, sites, cp.expectedSites, ripReloc);
		VirtualFree(copy, 0, MEM_RELEASE);
		return false;
	}
	for (int i = 0; i + 4 <= cp.srcFuncSize; ++i)
	{
		int32_t* p = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(copy) + i);
		if (*p == cp.srcCurrentDisp) *p = cp.copyTargetDisp;
	}
	FlushInstructionCache(GetCurrentProcess(), copy, cp.srcFuncSize);

	// 3. Redirect vtable slot in.rdata
	uintptr_t* slot = reinterpret_cast<uintptr_t*>(
		base + cp.vtableRVA + static_cast<uintptr_t>(cp.vtableSlot) * 8);
	DWORD oldProt = 0;
	if (!VirtualProtect(slot, 8, PAGE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[SNDC_Native] %s: VirtualProtect vtable slot failed\n", cp.label);
		VirtualFree(copy, 0, MEM_RELEASE);
		return false;
	}
	uintptr_t prev = *slot;
	*slot = reinterpret_cast<uintptr_t>(copy);
	VirtualProtect(slot, 8, oldProt, &oldProt);

	(void)prev;
	return true;
}

// SendProp metadata patches (re-extracted from the old #if 0 block).
// Patches static SendProp.bss memory at known RVAs so the encoder reads from
// the same offset the dispatcher (now patched above) writes to.
struct SendPropOffsetPatch {
	const char* label;
	uintptr_t templatePropRVA;  // template SendProp +0x78 = m_Offset
	uintptr_t arrayPropRVA;     // DPT_Array SendProp +0x28/0x2C/0x48
	int targetOffset;
	int s21nElements;
	int elemStride;
};

static const SendPropOffsetPatch s_sendPropPatches[] = {
	// GLOBAL -- mirror S21 client
	// m_bools[18]@2944 m_ranges[34]@2962 m_int32s[18]@3032
	// m_times[26]@3104 m_entities[10]@3208 end=3248
	{ "GLOBAL.m_bools",     0x2A3153C0, 0x2A315448, 2944, 18, 1 },
	{ "GLOBAL.m_ranges",    0x2A3154D0, 0x2A315558, 2962, 34, 2 },
	{ "GLOBAL.m_int32s",    0x2A3155E0, 0x2A315668, 3032, 18, 4 },
	{ "GLOBAL.m_times",     0x2A3156F0, 0x2A315778, 3104, 26, 4 },
	{ "GLOBAL.m_entities",  0x2A315800, 0x2A315888, 3208, 10, 4 },
	// PG
	{ "PG.m_ranges",        0x2A31E440, 0x2A31E4C8, 2850, 30, 2 },
	{ "PG.m_int32s",        0x2A31E550, 0x2A31E5D8, 2912, 34, 4 },
	{ "PG.m_times",         0x2A31E660, 0x2A31E6E8, 3048, 14, 4 },
	{ "PG.m_entities",      0x2A31E770, 0x2A31E7F8, 3104,  6, 4 },
	// PE
	{ "PE.m_ranges",        0x2A317FA0, 0x2A318028, 2864, 30, 2 },
	{ "PE.m_int32s",        0x2A3180B0, 0x2A318138, 2924,  6, 4 },
	{ "PE.m_times",         0x2A3181C0, 0x2A318248, 2948, 10, 4 },
	{ "PE.m_entities",      0x2A3182D0, 0x2A318358, 2988, 22, 4 },
	// TITAN
	{ "TITAN.m_ranges",     0x2A3209B0, 0x2A320A38, 2842, 18, 2 },
	{ "TITAN.m_int32s",     0x2A320AC0, 0x2A320B48, 2880,  4, 4 },
	{ "TITAN.m_times",      0x2A320BD0, 0x2A320C58, 2896, 10, 4 },
	{ "TITAN.m_entities",   0x2A320CE0, 0x2A320D68, 2936,  6, 4 },
	// DEATH_BOX
	{ "DEATHBOX.m_ranges",  0x2A314C70, 0x2A314CF8, 2838,  4, 2 },
	{ "DEATHBOX.m_int32s",  0x2A314D80, 0x2A314E08, 2848, 14, 4 },
	{ "DEATHBOX.m_times",   0x2A314E90, 0x2A314F18, 2904,  3, 4 },
	{ "DEATHBOX.m_entities",0x2A314FA0, 0x2A315028, 2916,  3, 4 },
};

static void PatchSNDCNativeLayout(uintptr_t base)
{
	(void)0;

	// === 1. Vtable in-place patches ===
	int inplaceOK = 0;
	for (const auto& p : s_inplacePatches)
	{
		uint8_t* fn = reinterpret_cast<uint8_t*>(base + p.funcRVA);
		int n = ApplyDispPatch(fn, p.funcSize, p.s3Disp, p.targetDisp,
		                       p.expectedSites, p.label);
		if (n == p.expectedSites) ++inplaceOK;
	}
	(void)inplaceOK;

	// === 2. Vtable copy+redirect patches ===
	int copyOK = 0;
	for (const auto& cp : s_copyPatches)
	{
		if (ApplyCopyPatch(base, cp)) ++copyOK;
	}
	(void)copyOK;

	// SetGlobalNetBool uses CScriptNetDataGlobal (0x14F46E0), not
	// CScriptNetData_SNDC_GLOBAL (0x14ECB20). Bool get/set copies for S21
	// offset 2944 only rewrote the latter; sync the wrapper slots.
	{
		uintptr_t* sndcVt = reinterpret_cast<uintptr_t*>(base + 0x14ECB20);
		uintptr_t* wrapVt = reinterpret_cast<uintptr_t*>(base + 0x14F46E0);
		// slots 281=get_bool.. 286=set_bool
		DWORD oldProt = 0;
		const size_t bytes = 6 * sizeof(uintptr_t);
		if (VirtualProtect(&wrapVt[281], bytes, PAGE_READWRITE, &oldProt))
		{
			wrapVt[281] = sndcVt[281];
			wrapVt[286] = sndcVt[286];
			VirtualProtect(&wrapVt[281], bytes, oldProt, &oldProt);
			FlushInstructionCache(GetCurrentProcess(), &wrapVt[281], bytes);
		}
		else
		{
			Warning(eDLL_T::ENGINE,
				"[SNDC_Native] CScriptNetDataGlobal bool-slot VirtualProtect FAILED\n");
		}
	}

	// PE_EXPANDED: SendProp uses S21 offsets (bools@2944, ranges@2976, ...).
	BuildPEExpandedVtable(base);

	// === 3. SendProp metadata patches ===
	int spOK = 0;
	for (const auto& sp : s_sendPropPatches)
	{
		// arrayProp: +0x28 nElements, +0x2C ElementStride, +0x48 sizeofVar
		int32_t* pNElem  = reinterpret_cast<int32_t*>(base + sp.arrayPropRVA + 0x28);
		int32_t* pStride = reinterpret_cast<int32_t*>(base + sp.arrayPropRVA + 0x2C);
		int32_t* pSizeof = reinterpret_cast<int32_t*>(base + sp.arrayPropRVA + 0x48);
		int32_t oldNElem  = *pNElem;
		int32_t oldStride = *pStride;
		int32_t oldSizeof = *pSizeof;
		*pNElem  = sp.s21nElements;
		*pStride = sp.elemStride;
		*pSizeof = sp.s21nElements * sp.elemStride;

		// templateProp: +0x78 m_Offset
		int32_t* pOffset = reinterpret_cast<int32_t*>(base + sp.templatePropRVA + 0x78);
		int32_t oldOffset = *pOffset;
		*pOffset = sp.targetOffset;

		++spOK;
		(void)oldNElem;
		(void)oldStride;
		(void)oldSizeof;
		(void)oldOffset;
	}
	(void)spOK;
}

// PE_EXPANDED S21 array offsets (must match dt_extend.cpp s_peExpandedLayout).
// PE-base after PatchSNDCNativeLayout: B@2832 R@2864 I@2924 T@2948 E@2988.
static constexpr uintptr_t kPEVtableRVA = 0x14F3DC0; // SNDC_PLAYER_EXCLUSIVE vtable
static constexpr int kPEVtCloneSlots = 400;

struct PEExpandedSlotPatch {
	int slot;
	int peBaseDisp;
	int peExpDisp;
	int fnSize;
	int expectedSites; // 1 get, 2 set
	const char* label;
};

static const PEExpandedSlotPatch s_peExpandedSlots[] = {
	{ 281, 2832, 2944, 0x0C, 1, "PE_EXP.get_bool" },
	{ 282, 2864, 2976, 0x0C, 1, "PE_EXP.get_range" },
	{ 283, 2924, 3076, 0x0B, 1, "PE_EXP.get_int" },
	{ 284, 2948, 3100, 0x0D, 1, "PE_EXP.get_time" },
	{ 285, 2988, 3140, 0x10, 1, "PE_EXP.get_entity" },
	{ 286, 2832, 2944, 0x49, 2, "PE_EXP.set_bool" },
	{ 287, 2864, 2976, 0x4C, 2, "PE_EXP.set_range" },
	{ 288, 2924, 3076, 0x48, 2, "PE_EXP.set_int" },
	{ 289, 2948, 3100, 0x51, 2, "PE_EXP.set_time" },
	{ 290, 2988, 3140, 0x44, 2, "PE_EXP.set_entity" },
};

static void BuildPEExpandedVtable(uintptr_t base)
{
	uintptr_t* peVt = reinterpret_cast<uintptr_t*>(base + kPEVtableRVA);
	const size_t vtBytes = static_cast<size_t>(kPEVtCloneSlots) * sizeof(uintptr_t);
	s_peExpandedVtable = static_cast<uintptr_t*>(
		VirtualAlloc(nullptr, vtBytes + HeapCanary::kTailBytes,
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (!s_peExpandedVtable)
	{
		Warning(eDLL_T::ENGINE, "[SNDC_Native] PE_EXPANDED vtable alloc FAILED\n");
		return;
	}
	memcpy(s_peExpandedVtable, peVt, vtBytes);
	HeapCanary::RegisterTail("pe-expanded-vtable", s_peExpandedVtable, vtBytes);

	int ok = 0;
	for (const auto& sp : s_peExpandedSlots)
	{
		uint8_t* src = reinterpret_cast<uint8_t*>(peVt[sp.slot]);
		if (!src)
		{
			Warning(eDLL_T::ENGINE,
				"[SNDC_Native] PE_EXPANDED vt[%d] null -- SKIP\n", sp.slot);
			continue;
		}

		void* copy = AllocExecNearModule(base, sp.fnSize);
		if (!copy)
		{
			Warning(eDLL_T::ENGINE,
				"[SNDC_Native] PE_EXPANDED vt[%d] AllocExecNearModule(%d) FAILED\n",
				sp.slot, sp.fnSize);
			continue;
		}
		memcpy(copy, src, sp.fnSize);
		RelocateRipRelativeInCopy(static_cast<uint8_t*>(copy), sp.fnSize,
			reinterpret_cast<uintptr_t>(src));

		int n = ApplyDispPatch(static_cast<uint8_t*>(copy), sp.fnSize,
			sp.peBaseDisp, sp.peExpDisp, sp.expectedSites, sp.label);
		if (n != sp.expectedSites)
		{
			VirtualFree(copy, 0, MEM_RELEASE);
			continue;
		}

		s_peExpandedVtable[sp.slot] = reinterpret_cast<uintptr_t>(copy);
		++ok;
	}

	Warning(eDLL_T::ENGINE,
		"[SNDC_Native] PE_EXPANDED vtable fork: %d/%zu get/set slots "
		"(S21 B@2944 R@2976 I@3076 T@3100 E@3140)\n",
		ok, sizeof(s_peExpandedSlots) / sizeof(s_peExpandedSlots[0]));
}

} // anonymous namespace

void* SNDC_GetPEExpandedVtable(void)
{
	return s_peExpandedVtable;
}

void SNDC_ApplyPEExpandedVtable(void* entity)
{
	if (!entity || !s_peExpandedVtable)
		return;
	*reinterpret_cast<uintptr_t*>(entity) = reinterpret_cast<uintptr_t>(s_peExpandedVtable);
}

void SNDC_ApplyNativeLayoutIfBacked(void)
{
	if (s_nativeLayoutApplied)
		return;
	if (!sndc_native_layout.GetBool())
	{
		DevMsg(eDLL_T::ENGINE, "[SNDC_Native] sndc_native_layout=0 -- skipping native layout patcher\n");
		return;
	}
	PatchSNDCNativeLayout(g_GameDll.GetModuleBase());
	s_nativeLayoutApplied = true;
}

//=============================================================================

void VScriptNetDataLimits::GetAdr(void) const
{
	LogFunAdr("AllocateInternalVar", v_AllocateInternalVar);
}

void VScriptNetDataLimits::GetFun(void) const
{
	uintptr_t base = g_GameDll.GetModuleBase();

	// AllocateInternalVar (RVA 0x888DD0) -- slot allocator, hooked for logging
	v_AllocateInternalVar = reinterpret_cast<decltype(v_AllocateInternalVar)>(base + 0x888DD0);

}

void VScriptNetDataLimits::GetVar(void) const
{
	uintptr_t base = g_GameDll.GetModuleBase();

	// scriptNetVars bases (same as scriptnetdata_ext.cpp)
	g_pServerNetVarsBase = reinterpret_cast<uint8_t*>(base + 0x285A01A0);
	g_pClientNetVarsBase = reinterpret_cast<uint8_t*>(base + 0x29509820);

	// Category struct bases for the registration logger.
	s_catStructBase       = base + 0x285AE020;
	s_catStructBaseClient = base + 0x295091F0;

	// SNDC GLOBAL entity pointers (from sndc_diag research)
	// 0x2859E480 = CLIENT entity (C_ScriptNetDataGlobal, CLIENT offsets 3136/3152/3216)
	// 0x295075E8 = SERVER entity (script Set/Get writes here, SERVER offsets 2832/2848/2912)
	s_pGlobalEntClient = reinterpret_cast<uintptr_t*>(base + 0x2859E480);
	s_pGlobalEntServer = reinterpret_cast<uintptr_t*>(base + 0x295075E8);

	// Compute extension layouts (for origCount/targetCount metadata)
	ComputeExtensionLayouts();
}

void VScriptNetDataLimits::Detour(const bool bAttach) const
{
	if (!bAttach)
		return;

	// Always init extension storage (SQ natives need it)
	ComputeExtensionLayouts();

	// NOP "not registered" errors in hash-lookup functions.
	{
		uintptr_t base = g_GameDll.GetModuleBase();
		auto nopErrorCall = [](uintptr_t addr, const char* label) {
			uint8_t* p = reinterpret_cast<uint8_t*>(addr);
			// Verify it's a CALL instruction (E8 xx xx xx xx)
			if (p[0] == 0xE8)
			{
				DWORD oldProt;
				VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
				memset(p, 0x90, 5); // NOP x 5
				VirtualProtect(p, 5, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), p, 5);
				Warning(eDLL_T::ENGINE, "[SNDC_Limits] %s hash lookup 'not registered' error NOP'd at 0x%p\n",
					label, (void*)addr);
			}
			else
			{
				Warning(eDLL_T::ENGINE, "[SNDC_Limits] %s hash lookup NOP SKIPPED: expected E8, got %02X at 0x%p\n",
					label, p[0], (void*)addr);
			}
		};
		nopErrorCall(base + 0x8895B6, "SERVER");
		nopErrorCall(base + 0xC18B46, "CLIENT");
	}

	// --- Hook AllocateInternalVar for overflow logging ---
	if (v_AllocateInternalVar)
	{
		DetourSetup(&v_AllocateInternalVar, &Hook_AllocateInternalVar, bAttach);
		Warning(eDLL_T::ENGINE, "[SNDC_Limits] AllocateInternalVar detour attached\n");
	}
	else
		Warning(eDLL_T::ENGINE, "[SNDC_Limits] v_AllocateInternalVar is NULL -- detour NOT attached!\n");

	// --- Initialize NET_ScriptMessage-based extension storage ---
	InitExtensionStorage();

	// Patch SendProp nElements to match S21 RecvTables.
	{
		uintptr_t base = g_GameDll.GetModuleBase();

		// DPT_Array nElements is at arrayPropAddr + 0x28.
		Warning(eDLL_T::ENGINE, "[SNDC_DT] vanilla-S3-DT mode: SendProp nElements/offset rewrites SKIPPED (native prop-replication WIP)\n");

		// Patch scriptNetCategories internalTypeMax.
		if (s_catStructBase)
		{
			struct CatLimitPatch {
				const char* name;
				int catIndex;
				int limits[5]; // Bool, Range, Int32, Time, Entity
			};

			// Must match s_entityConfigs[] / s_sndcSpecs[] / SendProp nElem patches.
			static const CatLimitPatch s_catLimits[] = {
				{ "GLOBAL",           0, { 18, 34, 18, 26, 10 } },
				{ "PLAYER_GLOBAL",    1, { 18, 30, 34, 14,  6 } },
				{ "PLAYER_EXCLUSIVE", 2, { 32, 30,  6, 10, 22 } },
				{ "TITAN_SOUL",       3, { 10, 18,  4, 10,  6 } },
				{ "DEATH_BOX",        4, {  5,  4, 14,  3,  3 } },
			};

			for (const auto& cl : s_catLimits)
			{
				int32_t* pMax = reinterpret_cast<int32_t*>(s_catStructBase + cl.catIndex * 524);
				for (int t = 0; t < 5; t++)
				{
					if (pMax[t] != cl.limits[t])
						pMax[t] = cl.limits[t];
				}
			}
		}

		// Timing bypasses for late registration (groups C/D/F).
		{
			struct TimingPatch {
				const char* label;
				uintptr_t rva;
				uint8_t origByte;
				uint8_t patchByte;
			};
			static const TimingPatch s_timingPatches[] = {
				// Group F: BeginCheck bypass ("Haven't called BeginRegisteringFunctions")
				{ "SRV BeginCheck",      0x888E62, 0x7D, 0xEB },
				{ "CLI BeginCheck",      0xC183F2, 0x7D, 0xEB },
				// Group C: EndCheck bypass ("Already called EndRegisteringFunctions")
				{ "SRV EndCheck",        0x888E7C, 0x7E, 0xEB },
				{ "CLI EndCheck",        0xC1840C, 0x7E, 0xEB },
				// Group D: Callback late-registration bypass
				{ "Bool  Callback",      0x8EBC7B, 0x7E, 0xEB },
				// JLE->JMP category-bounds entry lives in scriptnetdata_ext.cpp.
				{ "Float Callback",      0x8EC1EB, 0x7E, 0xEB },
				{ "Time  Callback",      0x8EC4AB, 0x7E, 0xEB },
				{ "Ent   Callback",      0x8EC76B, 0x7E, 0xEB },
			};

			int timingPatched = 0;
			for (const auto& tp : s_timingPatches)
			{
				uint8_t* p = reinterpret_cast<uint8_t*>(base + tp.rva);
				if (*p == tp.origByte)
				{
					DWORD oldProt;
					VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProt);
					*p = tp.patchByte;
					VirtualProtect(p, 1, oldProt, &oldProt);
					FlushInstructionCache(GetCurrentProcess(), p, 1);
					timingPatched++;
				}
				else if (*p != tp.patchByte)
				{
					Warning(eDLL_T::ENGINE,
						"[SNDC_Timing] %s: unexpected byte 0x%02X at RVA 0x%X (expected 0x%02X)\n",
						tp.label, *p, (unsigned)tp.rva, tp.origByte);
				}
			}
			Warning(eDLL_T::ENGINE, "[SNDC_Timing] Patched %d/%d registration timing bypasses\n",
				timingPatched, (int)(sizeof(s_timingPatches) / sizeof(s_timingPatches[0])));
		}

		// Raise max category from 4 to 6 (7 categories).
		{
			auto patchCatBounds = [&](uintptr_t rva, const char* label) {
				uint8_t* p = reinterpret_cast<uint8_t*>(base + rva);
				if (*p == 4)
				{
					DWORD oldProt;
					VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProt);
					*p = 6;
					VirtualProtect(p, 1, oldProt, &oldProt);
					FlushInstructionCache(GetCurrentProcess(), p, 1);
					DevMsg(eDLL_T::ENGINE, "[SNDC_Limits] %s category bounds: 4 -> 6\n", label);
				}
				else if (*p == 6)
					DevMsg(eDLL_T::ENGINE, "[SNDC_Limits] %s category bounds already 6\n", label);
				else
					Warning(eDLL_T::ENGINE, "[SNDC_Limits] %s category bounds: unexpected value %d at RVA 0x%X\n",
						label, *p, (unsigned)rva);
			};

			patchCatBounds(0x888EE4, "SERVER");
			patchCatBounds(0xC18470, "CLIENT");
		}

		// Patch native SNDC layout (vtable disp32 + SendProp metadata) to the S21 offsets.
	}
}
