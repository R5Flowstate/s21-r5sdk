//=============================================================================//
//
// Purpose: dt_extend SYSTEM 14 -- legacy SendTable graft path
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "core/bridge_ready.h"
#include "core/bridge_stats.h"
#include "tier0/module.h"
#include "tier0/commandline.h"
#include "tier1/cvar.h"
#include "tier0/memvalidate.h"
#include "tier0/memstd.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "game/shared/dt_extend.h"
#include "game/shared/s21_dt_schema.h"
#include "game/shared/scriptnetdata_ext.h"
#include "common/global.h" // host_timescale
#include "game/server/consumable_inv.h"
#include "game/server/jetdrive.h"
#include "game/server/trigger_gravity.h"
#include "game/server/trigger_updraft.h"
#include "game/server/poseparam_ext.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/heap_canary.h"
#include "game/shared/scriptnetdata_limits.h"
#include "game/shared/deathfield_system.h"
#include "game/shared/offhand_slots_ext.h"
#include "game/shared/activity_s3_to_s21.h"
#include "game/shared/weapstate_s3_to_s21.h"
#include "engine/server/zipline_validation.h"
#include "game/shared/weapon_script_vars.h"
#include "game/shared/weapon_heat.h"
#include "game/server/energize.h"
#include "game/shared/globalnonrewind_vars.h"
#include "engine/server/server.h"
#include "engine/client/client.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <ctime>
#include "game/shared/dt_extend_system14.h"

// SYSTEM 14: LEGACY GRAFT PATH. Runs only when bridge_canonical_dt 0;
// the canonical rebuild subsumes it and heap-corrupts on rebuilt tables.
// ===========================================================================

//-----------------------------------------------------------------------------
// Extend-window backing.
// An appended prop's offset is a raw entity offset and the pack path copies SP_SIZEOFVAR bytes from entity + SP_OFFSET with no bounds check.
//-----------------------------------------------------------------------------
struct ExtendSpan
{
	const char* tableName;
	uintptr_t   table;
	int         base;
	int         requiredEnd;
};

static constexpr int kMaxExtendSpans = 64;
static ExtendSpan s_extendSpans[kMaxExtendSpans] = {};
static int        s_extendSpanCount = 0;

// Classes whose allocation is smaller than the extend window their SendTable carries. nativeSize is the sizeof the shipped Create allocates; the patch refuses to fire on any other immediate, so a moved layout fails closed into the audit rather than rewriting an unrelated instruction.
struct ExtendBackedClass
{
	const char* tableName;
	const char* dictName;
	const char* className;
	uint32_t    nativeSize;
};

static const ExtendBackedClass s_extendBackedClasses[] = {
	{ "DT_ScriptMover",            "script_mover",             "CScriptMover",           5600 },
	{ "DT_ScriptMoverLightweight", "script_mover_lightweight", "ScriptMoverLightweight", 5664 },
	{ "DT_PlayerVehicle",          "player_vehicle",           "CPlayerVehicle",         4752 },
	{ "DT_BaseGrenade",            "grenade",                  "CBaseGrenade",           9568 },
	{ "DT_BaseGrenade",            "rpg_missile",              "CMissile",               9568 },
	{ "DT_BaseGrenade",            "crossbow_bolt",            "CCrossbowBolt",          9488 },
};
static constexpr int kNumExtendBackedClasses =
	sizeof(s_extendBackedClasses) / sizeof(s_extendBackedClasses[0]);

// Native occupancy floor, snapshotted before any append: the highest entity
// offset the shipped SendTables already read. An append base below it is inside
// live fields, which the FACT_ALLOCSIZE check above cannot see -- it only proves
// the window fits inside the object, not that the bytes are unclaimed.
struct NativeTableEnd
{
	uintptr_t table;
	int       nativeEnd;
};

static constexpr int kMaxNativeTableEnds = 2048;
static NativeTableEnd s_nativeTableEnds[kMaxNativeTableEnds] = {};
static int            s_nativeTableEndCount = 0;

static ConVar bridge_extend_occupancy_gate("bridge_extend_occupancy_gate", "1", FCVAR_RELEASE,
	"Zero-proxy any appended prop whose base offset lands below the highest entity "
	"offset the carrier's native SendTables already read (a mid-class window, not "
	"allocation slack). 0 = ship the raw base and let the append alias live fields.");

static int DTExtend_RecordNativeEnds(uintptr_t table, int depth)
{
	if (!table || depth > 32) return 0;

	for (int i = 0; i < s_nativeTableEndCount; ++i)
		if (s_nativeTableEnds[i].table == table)
			return s_nativeTableEnds[i].nativeEnd;

	uint8_t* const props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return 0;

	int end = 0;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* const p = props + static_cast<uint64_t>(i) * SP_SIZE;
		if (*reinterpret_cast<int*>(p + SP_TYPE) == 10)
		{
			uint8_t* const child = *reinterpret_cast<uint8_t**>(p + SP_CHILDTABLE);
			const int childEnd = DTExtend_RecordNativeEnds(
				reinterpret_cast<uintptr_t>(child), depth + 1);
			if (childEnd > end) end = childEnd;
			continue;
		}
		// Packed offsets carry codec bits above the low 20 (see the m_localOrigin
		// 0x500554 form); the entity offset is the low word.
		const int off = *reinterpret_cast<int*>(p + SP_OFFSET) & 0xFFFFF;
		int width = *reinterpret_cast<int*>(p + SP_SIZEOFVAR);
		if (width <= 0 || width > 4096) width = 4;
		if (off > 0 && off < 0x100000 && off + width > end)
			end = off + width;
	}

	if (s_nativeTableEndCount < kMaxNativeTableEnds)
	{
		s_nativeTableEnds[s_nativeTableEndCount].table     = table;
		s_nativeTableEnds[s_nativeTableEndCount].nativeEnd = end;
		++s_nativeTableEndCount;
	}
	return end;
}

// Highest native offset anywhere in this carrier's tree, from the snapshot.
static int DTExtend_TreeNativeEnd(uint8_t* root, int depth)
{
	if (!root || depth > 32) return 0;

	int end = 0;
	for (int i = 0; i < s_nativeTableEndCount; ++i)
		if (s_nativeTableEnds[i].table == reinterpret_cast<uintptr_t>(root))
		{
			end = s_nativeTableEnds[i].nativeEnd;
			break;
		}

	if (!ODP_IsReadable(root, 0x4C0 + 8)) return end;
	uint8_t* const props = *reinterpret_cast<uint8_t**>(root + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(root + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return end;
	if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) return end;

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* const p = props + static_cast<uint64_t>(i) * SP_SIZE;
		if (*reinterpret_cast<int*>(p + SP_TYPE) != 10) continue;
		const int childEnd = DTExtend_TreeNativeEnd(
			*reinterpret_cast<uint8_t**>(p + SP_CHILDTABLE), depth + 1);
		if (childEnd > end) end = childEnd;
	}
	return end;
}

static const ExtendSpan* DTExtend_FindSpan(const char* tableName)
{
	if (!tableName) return nullptr;
	for (int i = 0; i < s_extendSpanCount; ++i)
		if (s_extendSpans[i].tableName && !strcmp(s_extendSpans[i].tableName, tableName))
			return &s_extendSpans[i];
	return nullptr;
}

// True when target is root or any DPT_DataTable descendant of it.
static bool DTExtend_TreeCarriesTable(uint8_t* root, uintptr_t target, int depth)
{
	if (!root || depth > 32) return false;
	if (reinterpret_cast<uintptr_t>(root) == target) return true;
	if (!ODP_IsReadable(root, 0x4C0 + 8)) return false;

	uint8_t* props = *reinterpret_cast<uint8_t**>(root + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(root + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return false;
	if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) return false;

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		if (*reinterpret_cast<int*>(p + SP_TYPE) != 10) continue; // DPT_DataTable
		uint8_t* child = *reinterpret_cast<uint8_t**>(p + SP_CHILDTABLE);
		if (child && DTExtend_TreeCarriesTable(child, target, depth + 1))
			return true;
	}
	return false;
}

// True only for a prop THIS table registered in s_extendProps. Native props
// share the window's offset range (CParticleSystem has 8 inside 2700..2972,
// m_iEffectIndex among them) and disarming one silently stops replicating it.
static bool DTExtend_IsAppendedProp(const char* tableName, const char* propName)
{
	if (!tableName || !propName)
		return false;

	for (int i = 0; i < kNumExtendProps; ++i)
	{
		const DTExtendProp& ep = s_extendProps[i];
		if (ep.tableName && ep.propName &&
			!strcmp(ep.tableName, tableName) && !strcmp(ep.propName, propName))
			return true;
	}
	return false;
}

// Set every non-proxied append on this table back to a zero proxy, so nothing
// reads entity memory the carrier does not own.
static int DTExtend_ZeroProxySpan(const ExtendSpan& span)
{
	uint8_t* table = reinterpret_cast<uint8_t*>(span.table);
	if (!table || !ODP_IsReadable(table, 0x4C0 + 8)) return 0;

	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return 0;

	int disarmed = 0;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		if (*reinterpret_cast<int*>(p + SP_TYPE) == 10) continue;
		const int off = *reinterpret_cast<int*>(p + SP_OFFSET);
		if (off < span.base || off >= span.requiredEnd) continue;

		const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
		if (!DTExtend_IsAppendedProp(span.tableName, nm))
			continue;

		*reinterpret_cast<int*>(p + SP_OFFSET) = 0;
		*reinterpret_cast<uintptr_t*>(p + 0x60) = (uintptr_t)DTExtend_ZeroProxyForProp(nm);
		*reinterpret_cast<uintptr_t*>(p + SP_CHILDTABLE) = 0;
		++disarmed;

		for (int a = 0; a < s_assignedCount && a < 512; ++a)
			if (s_assigned[a].offset == off && s_assigned[a].tableName &&
				!strcmp(s_assigned[a].tableName, span.tableName))
				s_assigned[a].offset = -1;
	}
	return disarmed;
}

static int DTExtend_GrowExtendBackedClasses()
{
	if (!v_GetEntityFactory)
	{
		Warning(eDLL_T::ENGINE,
			"[EXT-BACKING] grow SKIPPED: v_GetEntityFactory unresolved\n");
		return 0;
	}
	void** dict = (void**)v_GetEntityFactory();
	if (!dict || !dict[0])
	{
		Warning(eDLL_T::ENGINE, "[EXT-BACKING] grow SKIPPED: factory dict unresolved\n");
		return 0;
	}
	typedef uintptr_t(__fastcall* PFN_DictFindByName)(void**, const char*);
	PFN_DictFindByName pfnFind = *(PFN_DictFindByName*)((uintptr_t)dict[0] + 0x18);

	int grown = 0;
	for (int i = 0; i < kNumExtendBackedClasses; ++i)
	{
		const ExtendBackedClass& bc = s_extendBackedClasses[i];
		const ExtendSpan* span = DTExtend_FindSpan(bc.tableName);
		if (!span || span->requiredEnd <= (int)bc.nativeSize) continue;

		const uint32_t target = (uint32_t)((span->requiredEnd + 15) & ~15);
		uintptr_t factory = pfnFind(dict, bc.dictName);
		if (!factory)
		{
			Warning(eDLL_T::ENGINE,
				"[EXT-BACKING] '%s' NOT in the entity factory dict -- %s stays at %u while "
				"%s reads out to %d\n",
				bc.dictName, bc.className, bc.nativeSize, bc.tableName, span->requiredEnd);
			continue;
		}
		const uintptr_t createFn = *(uintptr_t*)(*(uintptr_t*)factory); // vtable slot[0]
		const int n = PatchEntityCreateAllocImm(createFn, target, bc.dictName, bc.nativeSize);
		if (n <= 0) continue;

		// Only now is the declared size true: replication and baseline code read
		// this field, and raising it ahead of the Create patch would hand out a
		// tail the allocation does not have.
		if (g_pFactoryListHead && *g_pFactoryListHead)
		{
			uintptr_t node = *g_pFactoryListHead;
			for (int guard = 0; node && guard < 4096; ++guard)
			{
				const char* cn = *(const char**)(node + FACT_CLASSNAME);
				if (cn && !strcmp(cn, bc.className))
				{
					if (*(int*)(node + FACT_ALLOCSIZE) < (int)target)
						*(int*)(node + FACT_ALLOCSIZE) = (int)target;
					break;
				}
				node = *(uintptr_t*)(node + FACT_NEXT);
			}
		}

		++grown;
	}
	return grown;
}

// Whole-tree backing check, independent of who authored the prop.
// The append audit above only knows about props this file appended.
static int DTExtend_AuditTreeBacking(uint8_t* table, const char* className, int alloc,
	int depth, int& reported, int& ringExempted)
{
	if (!table || depth > 32) return 0;
	if (!ODP_IsReadable(table, 0x4C0 + 8)) return 0;

	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return 0;
	if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) return 0;

	const char* tableName = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
	int over = 0;

	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		const int type = *reinterpret_cast<int*>(p + SP_TYPE);
		const int off  = *reinterpret_cast<int*>(p + SP_OFFSET) & 0xFFFFF;

		if (type == 10) // DPT_DataTable
		{
			if (off != 0) continue; // sub-object nest: base unknown here
			uint8_t* child = *reinterpret_cast<uint8_t**>(p + SP_CHILDTABLE);
			if (child)
				over += DTExtend_AuditTreeBacking(child, className, alloc, depth + 1,
					reported, ringExempted);
			continue;
		}

		// Offset 0 means the value comes from a proxy, not entity memory.
		if (off == 0) continue;

		int width = *reinterpret_cast<int*>(p + SP_SIZEOFVAR);
		if (width <= 0 || width > 0x10000) width = 4;
		if (off + width <= alloc) continue;

		// Deathfield ring leaves use non-zero offsets as changeframe slots only;
		// the value proxy never dereferences entity memory at that offset.
		if (DeathField_IsRingProxy(*reinterpret_cast<void**>(p + 0x60)))
		{
			++ringExempted;
			continue;
		}

		++over;
		const char* pn = *reinterpret_cast<const char**>(p + SP_VARNAME);
		*reinterpret_cast<int*>(p + SP_OFFSET) = 0;
		*reinterpret_cast<uintptr_t*>(p + 0x60) = (uintptr_t)DTExtend_ZeroProxyForProp(pn);
		*reinterpret_cast<uintptr_t*>(p + SP_CHILDTABLE) = 0;
		if (reported < 32)
			++reported;
	}
	return over;
}

int DTExtend_AuditAllClassBacking()
{
	if (!g_pFactoryListHead || !*g_pFactoryListHead) return 0;

	int classesOver = 0, propsOver = 0, reported = 0, classesSeen = 0, ringExempted = 0;
	uintptr_t node = *g_pFactoryListHead;
	for (int guard = 0; node && guard < 4096; ++guard)
	{
		uint8_t* st = *(uint8_t**)(node + FACT_SENDTABLE);
		const int alloc = *(int*)(node + FACT_ALLOCSIZE);
		const char* cn = *(const char**)(node + FACT_CLASSNAME);
		if (st && alloc > 0)
		{
			++classesSeen;
			const int over = DTExtend_AuditTreeBacking(st, cn, alloc, 0, reported, ringExempted);
			if (over) { ++classesOver; propsOver += over; }
		}
		node = *(uintptr_t*)(node + FACT_NEXT);
	}

	if (ringExempted)
		Msg(eDLL_T::ENGINE,
			"[dt_extend] class-backing audit: %d deathfield ring props exempted (proxy-sourced)\n",
			ringExempted);

	if (propsOver)
		Warning(eDLL_T::ENGINE,
			"[EXT-BACKING] whole-tree audit: %d prop(s) across %d of %d class(es) read past "
			"their allocation (%d named above). Every one is an out-of-bounds snapshot read.\n",
			propsOver, classesOver, classesSeen, reported);
	else
		Msg(eDLL_T::ENGINE,
			"[EXT-BACKING] whole-tree audit: %d classes, every entity-relative prop backed\n",
			classesSeen);
	return propsOver;
}

//-----------------------------------------------------------------------------
// [EXT-UNWRITTEN] static classification of every s_extendProps entry by how its value is produced. entity-slot appends need a writer. "disarmed" means the prop reached s_assigned then AuditExtendBacking force-zero-proxied it (offset cleared to -1); "no-slot" never reached s_assigned at all.
// Walks s_assigned directly -- DTExtend_GetOffset would set resolved and poison the dynamic half.
//-----------------------------------------------------------------------------
static void DTExtend_AuditExtendUnwritten(void)
{
	int nEntitySlot = 0;
	int nValueProxy = 0;
	int nZeroProxy = 0;
	int nRename = 0;
	int nDisarmed = 0;
	int nNoSlot = 0;
	const char* disarmedTable[32] = {};
	const char* disarmedProp[32] = {};
	int nDisarmedNamed = 0;
	int nDisarmedExtra = 0;
	const char* noSlotTable[32] = {};
	const char* noSlotProp[32] = {};
	int nNoSlotNamed = 0;
	int nNoSlotExtra = 0;

	for (int i = 0; i < kNumExtendProps; ++i)
	{
		const DTExtendProp& ep = s_extendProps[i];
		if (!ep.tableName || !ep.propName)
			continue;

		if (DTExtend_AppendSuppressedByRename(ep.tableName, ep.propName))
		{
			++nRename;
			continue;
		}
		if (DTExtend_ShouldZeroProxyAppendedProp(ep.tableName, ep.propName))
		{
			++nZeroProxy;
			continue;
		}
		if (DTExtend_ValueProxyForAppendedProp(ep.tableName, ep.propName) != nullptr)
		{
			++nValueProxy;
			continue;
		}

		bool found = false;
		int off = -1;
		for (int a = 0; a < s_assignedCount; ++a)
		{
			if (s_assigned[a].tableName && s_assigned[a].propName &&
				!strcmp(s_assigned[a].tableName, ep.tableName) &&
				!strcmp(s_assigned[a].propName, ep.propName))
			{
				found = true;
				off = s_assigned[a].offset;
				break;
			}
		}

		if (found && off > 0)
		{
			++nEntitySlot;
			continue;
		}

		if (found)
		{
			// In s_assigned with offset <= 0: backing audit force-disarmed it.
			++nDisarmed;
			if (nDisarmedNamed < 32)
			{
				disarmedTable[nDisarmedNamed] = ep.tableName;
				disarmedProp[nDisarmedNamed] = ep.propName;
				++nDisarmedNamed;
			}
			else
				++nDisarmedExtra;
			continue;
		}

		++nNoSlot;
		if (nNoSlotNamed < 32)
		{
			noSlotTable[nNoSlotNamed] = ep.tableName;
			noSlotProp[nNoSlotNamed] = ep.propName;
			++nNoSlotNamed;
		}
		else
			++nNoSlotExtra;
	}

	const int nTotal = nEntitySlot + nValueProxy + nZeroProxy + nRename + nDisarmed + nNoSlot;
	Msg(eDLL_T::ENGINE,
		"[EXT-UNWRITTEN] %d appends: %d entity-slot, %d value-proxy, %d zero-proxy, "
		"%d rename-suppressed, %d disarmed, %d no-slot\n",
		nTotal, nEntitySlot, nValueProxy, nZeroProxy, nRename, nDisarmed, nNoSlot);

	for (int i = 0; i < nDisarmedNamed; ++i)
	{
		Warning(eDLL_T::ENGINE,
			"[EXT-UNWRITTEN] disarmed %s.%s -- force-zero-proxied for lack of backing; "
			"see [EXT-BACKING] for table %s\n",
			disarmedTable[i], disarmedProp[i], disarmedTable[i]);
	}
	if (nDisarmedExtra > 0)
	{
		Warning(eDLL_T::ENGINE,
			"[EXT-UNWRITTEN] (+%d more disarmed)\n", nDisarmedExtra);
	}

	for (int i = 0; i < nNoSlotNamed; ++i)
	{
		Warning(eDLL_T::ENGINE,
			"[EXT-UNWRITTEN] no-slot %s.%s -- append never reached s_assigned\n",
			noSlotTable[i], noSlotProp[i]);
	}
	if (nNoSlotExtra > 0)
	{
		Warning(eDLL_T::ENGINE,
			"[EXT-UNWRITTEN] (+%d more no-slot)\n", nNoSlotExtra);
	}
}

static void CC_DTExtendUnwrittenReport_f(const CCommand& args)
{
	(void)args;

	if (!DTExtend_Applied())
	{
		Msg(eDLL_T::ENGINE,
			"[EXT-UNWRITTEN] extension has not run yet\n");
		return;
	}

	DTExtend_AuditExtendUnwritten();

	int nUnresolved = 0;
	for (int i = 0; i < kNumExtendProps; ++i)
	{
		const DTExtendProp& ep = s_extendProps[i];
		if (!ep.tableName || !ep.propName)
			continue;
		if (DTExtend_AppendSuppressedByRename(ep.tableName, ep.propName))
			continue;
		if (DTExtend_ShouldZeroProxyAppendedProp(ep.tableName, ep.propName))
			continue;
		if (DTExtend_ValueProxyForAppendedProp(ep.tableName, ep.propName) != nullptr)
			continue;

		for (int a = 0; a < s_assignedCount; ++a)
		{
			if (!s_assigned[a].tableName || !s_assigned[a].propName)
				continue;
			if (strcmp(s_assigned[a].tableName, ep.tableName) != 0 ||
				strcmp(s_assigned[a].propName, ep.propName) != 0)
				continue;

			if (s_assigned[a].offset > 0 && !s_assigned[a].resolved)
			{
				Msg(eDLL_T::ENGINE,
					"[EXT-UNWRITTEN] %s.%s offset=%d -- no code path has resolved this "
					"slot\n",
					ep.tableName, ep.propName, s_assigned[a].offset);
				++nUnresolved;
			}
			break;
		}
	}

	Msg(eDLL_T::ENGINE,
		"[EXT-UNWRITTEN] %d entity-slot append(s) never resolved\n", nUnresolved);
}

static ConCommand sdk_dt_unwritten_report("sdk_dt_unwritten_report",
	CC_DTExtendUnwrittenReport_f,
	"[EXT-UNWRITTEN] Report appended SendProps with no entity slot, and entity-slot "
	"appends that no code path has resolved via DTExtend_GetOffset.",
	FCVAR_DEVELOPMENTONLY);

static int DTExtend_AuditExtendBacking()
{
	if (!g_pFactoryListHead || !*g_pFactoryListHead)
	{
		Warning(eDLL_T::ENGINE,
			"[EXT-BACKING] audit SKIPPED: factory list head unresolved -- appended offsets "
			"are UNVERIFIED against entity allocations\n");
		return 0;
	}

	int violations = 0;
	for (int s = 0; s < s_extendSpanCount; ++s)
	{
		const ExtendSpan& span = s_extendSpans[s];
		const char* worstClass = nullptr;
		int worstAlloc = 0;
		int carriers = 0;
		const char* aliasClass = nullptr;
		int aliasNativeEnd = 0;

		uintptr_t node = *g_pFactoryListHead;
		for (int guard = 0; node && guard < 4096; ++guard)
		{
			uint8_t* st = *(uint8_t**)(node + FACT_SENDTABLE);
			if (st && DTExtend_TreeCarriesTable(st, span.table, 0))
			{
				++carriers;
				const int alloc = *(int*)(node + FACT_ALLOCSIZE);
				if (alloc < span.requiredEnd && (!worstClass || alloc < worstAlloc))
				{
					worstClass = *(const char**)(node + FACT_CLASSNAME);
					worstAlloc = alloc;
				}
				const int nativeEnd = DTExtend_TreeNativeEnd(st, 0);
				if (nativeEnd > span.base && nativeEnd > aliasNativeEnd)
				{
					aliasClass = *(const char**)(node + FACT_CLASSNAME);
					aliasNativeEnd = nativeEnd;
				}
			}
			node = *(uintptr_t*)(node + FACT_NEXT);
		}

		if (worstClass)
		{
			++violations;
			const int disarmed = DTExtend_ZeroProxySpan(span);
			Warning(eDLL_T::ENGINE,
				"[EXT-BACKING] %s window %d..%d OVERRUNS %s (alloc %d) -- %d appended prop(s) "
				"forced to a zero proxy. Give that class a backed allocation or lower the base.\n",
				span.tableName, span.base, span.requiredEnd, worstClass, worstAlloc, disarmed);
			continue;
		}

		if (!aliasClass)
			continue;

		++violations;
		if (!bridge_extend_occupancy_gate.GetBool())
		{
			Warning(eDLL_T::ENGINE,
				"[EXT-OCCUPANCY] %s base %d is BELOW %s's native field end %d -- gate is OFF, "
				"the appends read/write live entity state\n",
				span.tableName, span.base, aliasClass, aliasNativeEnd);
			continue;
		}

		const int disarmed = DTExtend_ZeroProxySpan(span);
		Warning(eDLL_T::ENGINE,
			"[EXT-OCCUPANCY] %s window %d..%d is MID-CLASS on %s (native fields reach %d) -- "
			"%d appended prop(s) forced to a zero proxy. Prove a window past %d or give the "
			"prop a sidecar value proxy.\n",
			span.tableName, span.base, span.requiredEnd, aliasClass, aliasNativeEnd,
			disarmed, aliasNativeEnd);
	}
	return violations;
}

//-----------------------------------------------------------------------------
// DTExtend_Apply -- the core: for each table that has registered props, grow its SendProp array, clone a same-type template (for m_ProxyFn inheritance), patch the fields, bump count.
// Runs BEFORE SetupFlatPropertyArray so the precalcs/baselines pick up the new props automatically.
//-----------------------------------------------------------------------------
void DTExtend_Apply(void** tables, int count)
{
	// Must run before any DTExtend_BaseOffset / zero-proxy decision: the Heavy
	// append base and proxy gate both read s_triggerHeavyGrown.
	DTExtend_EnsureTriggerCylinderHeavyGrown();

	if (kNumExtendProps <= 0)
	{
		Warning(eDLL_T::ENGINE, "[dt_extend] SendTable_Init reached: 0 props registered -- no-op\n");
		return;
	}

	Warning(eDLL_T::ENGINE, "[dt_extend] SendTable_Init: extending %d props across %d tables\n",
		kNumExtendProps, count);

	s_extendSpanCount = 0;
	int nDupNameSuppressed = 0;

	// Snapshot native occupancy BEFORE the first append -- afterwards our own
	// props are in the arrays and the floor would include them.
	s_nativeTableEndCount = 0;
	for (int i = 0; i < count; ++i)
		DTExtend_RecordNativeEnds((uintptr_t)tables[i], 0);

	// --- Pre-scan: pick a CLEAN template SendProp per leaf type, across ALL tables, BEFORE any mutation. "Clean" = no EXCLUDE(0x40); for Vector prefer flags==4 (NOSCALE, the plain SendProxy_VectorToVector), for Int64 prefer flags==1 (UNSIGNED), for Float prefer flags==4.
	// We clone this so m_ProxyFn et al. are inherited from a well-formed prop of the same type. ---
	const uint8_t* tmplForType[16] = {};
	int tmplScore[16] = {};
	for (int i = 0; i < count; ++i)
	{
		uintptr_t table = (uintptr_t)tables[i];
		const uint8_t* props = *(const uint8_t**)(table + ST_PROPS);
		int nProps = *(int*)(table + ST_NPROPS);
		for (int j = 0; j < nProps; ++j)
		{
			const uint8_t* prop = props + (uint64_t)j * SP_SIZE;
			int type = *(const int*)(prop + SP_TYPE);
			if (type < 0 || type >= 16 || type == 10) continue; // DataTable handled separately
			int flags = *(const int*)(prop + SP_FLAGS);
			int score;
			if (flags & 0x40)              score = -1;                                  // EXCLUDE -- never
			else if (type == 2 || type == 1) score = (flags == 4) ? 3 : (flags & 4) ? 2 : 1; // Vector/Float: NOSCALE clean
			else if (type == 7)            score = (flags == 1) ? 3 : (flags & 1) ? 2 : 1;   // Int64: UNSIGNED
			else if (type == 5)            score = (*(const int*)(prop + SP_NELEMENTS) > 1) ? 2 : 1; // Array: a real array
			else                           score = 1;                                   // Int/String/Time/...: any non-EXCLUDE
			if (score > tmplScore[type]) { tmplScore[type] = score; tmplForType[type] = prop; }
		}
	}

	// [TMPL-PICK] One line per leaf type -- which donor prop won the pre-scan and what +0x60 ProxyFn rides along.
	// Pack calls the proxy for every type; non-proxied appends inherit the donor's proxy verbatim (a translating EHANDLE donor would transform every appended value of that type).
	const char* s_nestedExtendTables[4];
	int kNumNestedExtendTables = 0;
	s_nestedExtendTables[kNumNestedExtendTables++] = "DT_WeaponX_LocalWeaponData";
	// m_shotIndexForSpread (value-proxied).
	// Nested under DT_WeaponX like LocalWeaponData; without this row its registered extend prop was silently dropped and the client's viewkick pattern row froze at the start-of-burst clamp (m_shotIndexForSpread stuck at wire-0).
	s_nestedExtendTables[kNumNestedExtendTables++] = "DT_WeaponX_PredictingClientOnly";
	s_nestedExtendTables[kNumNestedExtendTables++] = "DT_LocalPlayerExclusive";
	// m_gameTimescale (value-proxied). Nested under the GNR ServerClass, so the
	// engine list never carries it and its one registered prop was dropped the
	// same way DT_WeaponX_PredictingClientOnly was. The client then reads a
	// wire-0 m_gameTimescale, and CL_CalcMoveFrametime divides by it.
	s_nestedExtendTables[kNumNestedExtendTables++] = "DT_GlobalNonRewinding";
	if (!JetDrive_WireEnabled())
		Msg(eDLL_T::ENGINE,
			"[dt_extend] DT_LocalPlayerExclusive: bridge_jetdrive_wire 0 -- the 14 "
			"jetdrive props stay off the wire\n");
	if (!TriggerGravity_WireEnabled())
		Msg(eDLL_T::ENGINE,
			"[dt_extend] DT_LocalPlayerExclusive: bridge_trigger_gravity_wire 0 -- "
			"m_gravityLiftActive / m_blackholeActive stay off the wire\n");
	if (!UpdraftBridge_WireEnabled())
		Msg(eDLL_T::ENGINE,
			"[dt_extend] DT_LocalPlayerExclusive: bridge_updraft_wire 0 -- the 12 "
			"updraft props stay off the wire\n");

	for (int i = 0; i < count + kNumNestedExtendTables; ++i)
	{
		uintptr_t table;
		const bool isNestedVisit = (i >= count);
		if (i < count)
			table = (uintptr_t)tables[i];
		else
		{
			table = (uintptr_t)DTExtend_FindTableByName(s_nestedExtendTables[i - count]);
			if (!table)
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] nested extend table '%s' NOT found in the recursive "
					"cache -- its registered props are DROPPED\n",
					s_nestedExtendTables[i - count]);
				continue;
			}
			// If a future engine list ever contains it top-level, skip the
			// second visit (double-append guard).
			bool alreadyTopLevel = false;
			for (int t = 0; t < count; ++t)
				if ((uintptr_t)tables[t] == table) { alreadyTopLevel = true; break; }
			if (alreadyTopLevel)
				continue;
		}
		const char* tableName = *(const char**)(table + ST_NETTABLENAME);
		if (!tableName) continue;

		uint8_t* oldProps = *(uint8_t**)(table + ST_PROPS);
		int oldNProps = *(int*)(table + ST_NPROPS);

		// Count how many extend props target this table (rename-suppressed and,
		// when bridge_dt_dup_append_suppress is on, name-already-native both skip
		// the array slot; native collisions still reserve offset in the append loop).
		const bool bDupSuppress = bridge_dt_dup_append_suppress.GetBool();
		int numForTable = 0;
		for (int p = 0; p < kNumExtendProps; ++p)
		{
			if (!s_extendProps[p].tableName || strcmp(s_extendProps[p].tableName, tableName) != 0)
				continue;
			if (DTExtend_AppendSuppressedByRename(tableName, s_extendProps[p].propName))
				continue;
			if (DTExtend_AppendSuppressedByWireLever(tableName, s_extendProps[p].propName))
				continue;
			if (bDupSuppress
				&& DTExtend_FindExistingPropIdx(oldProps, oldNProps, s_extendProps[p].propName) >= 0)
				continue;
			++numForTable;
		}
		if (numForTable == 0)
		{
			// No array growth, but still announce every native-name collision so
			// boot is loud even when the whole table is suppress-only.
			for (int p = 0; p < kNumExtendProps; ++p)
			{
				const DTExtendProp& ep = s_extendProps[p];
				if (!ep.tableName || strcmp(ep.tableName, tableName) != 0)
					continue;
				if (DTExtend_AppendSuppressedByRename(tableName, ep.propName))
					continue;
				const int existIdx = DTExtend_FindExistingPropIdx(oldProps, oldNProps, ep.propName);
				if (existIdx < 0)
					continue;
				const uint8_t* const existProp =
					oldProps + static_cast<uint64_t>(existIdx) * SP_SIZE;
				const int existOff = *reinterpret_cast<const int*>(existProp + SP_OFFSET);
				const int existBits = *reinterpret_cast<const int*>(existProp + SP_NBITS);
				// numForTable==0 with a collision only happens when suppress is on;
				// lever-0 counts collisions into numForTable and never reaches here.
				Warning(eDLL_T::ENGINE,
					"[EXT-DUP] %s.%s append SUPPRESSED -- name already native at slot %d "
					"off=0x%X nBits=%d (dedi already sends it)\n",
					tableName, ep.propName, existIdx, existOff, existBits);
				++nDupNameSuppressed;
			}
			continue;
		}

		int newNProps = oldNProps + numForTable;

		int baseOffset = DTExtend_BaseOffset(tableName);

		// Allocate new prop array (don't free old -- unknown allocator, one-time leak)
		uint8_t* newProps = (uint8_t*)malloc((uint64_t)newNProps * SP_SIZE);
		if (!newProps)
		{
			Warning(eDLL_T::ENGINE, "[dt_extend] FATAL: malloc(%d) failed for %s\n",
				newNProps * (int)SP_SIZE, tableName);
			continue;
		}
		memcpy(newProps, oldProps, (uint64_t)oldNProps * SP_SIZE);

		int appendIdx = oldNProps;
		int curOffset = baseOffset;
		// Highest entity byte any NON-proxied append will read. Proxied props
		// serve their value from a proxy and never touch entity memory, so they
		// advance the cursor without contributing a backing requirement.
		int rawEnd = 0;

		for (int p = 0; p < kNumExtendProps; ++p)
		{
			const DTExtendProp& ep = s_extendProps[p];
			if (!ep.tableName || strcmp(ep.tableName, tableName) != 0) continue;
			if (DTExtend_AppendSuppressedByRename(tableName, ep.propName))
			{
				// RESERVE the offset slot. curOffset is a running cursor over the synthetic append region, so skipping a prop WITHOUT advancing it re-bases every LATER appended prop.
				const int suppressedSize = ep.valBytes > 0 ? ep.valBytes : 4;
				Warning(eDLL_T::ENGINE,
					"[dt_extend] %s.%s append SUPPRESSED (offset %d reserved) -- the rename "
					"pass binds this name to a real S3 prop; appending it too would duplicate "
					"the name\n",
					tableName, ep.propName, curOffset);
				curOffset = (curOffset + suppressedSize + 3) & ~3;
				continue;
			}
			// Wire-lever suppress: drop the slot entirely. This table's props are
			// all value-proxied, so the offset cursor is unused for them.
			if (DTExtend_AppendSuppressedByWireLever(tableName, ep.propName))
				continue;

			// Name already native on this table. Lever on: suppress and reserve the
			// offset slot (same arithmetic as the rename path). Lever off: warn and
			// fall through so the prop is counted/appended/offset-consumed as legacy.
			{
				const int existIdx = DTExtend_FindExistingPropIdx(oldProps, oldNProps, ep.propName);
				if (existIdx >= 0)
				{
					const uint8_t* const existProp =
						oldProps + static_cast<uint64_t>(existIdx) * SP_SIZE;
					const int existOff = *reinterpret_cast<const int*>(existProp + SP_OFFSET);
					const int existBits = *reinterpret_cast<const int*>(existProp + SP_NBITS);
					++nDupNameSuppressed;
					if (bDupSuppress)
					{
						const int suppressedSize = ep.valBytes > 0 ? ep.valBytes : 4;
						Warning(eDLL_T::ENGINE,
							"[EXT-DUP] %s.%s append SUPPRESSED -- name already native at slot %d "
							"off=0x%X nBits=%d (dedi already sends it); offset %d reserved\n",
							tableName, ep.propName, existIdx, existOff, existBits, curOffset);
						curOffset = (curOffset + suppressedSize + 3) & ~3;
						continue;
					}
					Warning(eDLL_T::ENGINE,
						"[EXT-DUP] %s.%s APPENDED ANYWAY (legacy) -- name already native at "
						"slot %d off=0x%X nBits=%d (two identically-named props)\n",
						tableName, ep.propName, existIdx, existOff, existBits);
				}
			}

			// Clone the clean template for this type (from the pre-scan). Falls back
			// to a same-type prop in THIS table, then to zero-init.
			const uint8_t* tmpl = ((int)ep.type >= 0 && (int)ep.type < 16) ? tmplForType[(int)ep.type] : nullptr;
			if (!tmpl)
			{
				for (int j = 0; j < oldNProps; ++j)
				{
					const uint8_t* prop = oldProps + (uint64_t)j * SP_SIZE;
					if (*(const int*)(prop + SP_TYPE) == (int)ep.type && !(*(const int*)(prop + SP_FLAGS) & 0x40))
					{ tmpl = prop; break; }
				}
			}

			uint8_t* dst = newProps + (uint64_t)appendIdx * SP_SIZE;
			if (tmpl) memcpy(dst, tmpl, SP_SIZE);
			else      memset(dst, 0, SP_SIZE);

			// Patch the cloned prop to be our new prop
			*(int*)(dst + SP_TYPE)      = (int)ep.type;
			*(int*)(dst + SP_NBITS)     = ep.nBits;
			*(float*)(dst + SP_LOW)     = ep.lowValue;
			*(float*)(dst + SP_HIGH)    = ep.highValue;
			*(int*)(dst + SP_NELEMENTS) = ep.nElements > 0 ? ep.nElements : 1;
			*(const char**)(dst + SP_VARNAME) = ep.propName;
			*(uint8_t*)(dst + SP_PRIORITY)    = 128;
			*(int*)(dst + SP_FLAGS)     = ep.flags;
			*(int*)(dst + SP_OFFSET)    = curOffset;
			// Pack path sizes struct reads from SP_SIZEOFVAR (+0x48); donor
			// template width is wrong for the registered field.
			*(int*)(dst + SP_SIZEOFVAR) = ep.valBytes > 0 ? ep.valBytes : 4;

			const bool zeroProxy = DTExtend_ShouldZeroProxyAppendedProp(tableName, ep.propName);
			if (zeroProxy)
			{
				// [VEC-ZERO-PROXY-FIX] dst is memcpy'd from a TEMPLATE prop; for type==2 (Vector), +0x70 is carried over unchanged. +0x70 is the same field DPT_DataTable uses for its child-table pointer.
				// Vector props can route their value through +0x70's target rather than the +0x60 proxy -- clear +0x70 so the clone does not read the template's chain.
				const uintptr_t preClearChild = *(uintptr_t*)(dst + 0x70);
				if ((int)ep.type == 2)
					Warning(eDLL_T::ENGINE, "[VEC-ZERO-PROXY-FIX] %s.%s: template +0x70 = 0x%llX before clear\n",
						tableName, ep.propName, (unsigned long long)preClearChild);
				*(int*)(dst + SP_OFFSET) = 0;
				*(uintptr_t*)(dst + 0x60) =
					(uintptr_t)DTExtend_ZeroProxyForProp(ep.propName);
				*(uintptr_t*)(dst + 0x70) = 0;
			}

			// [LAUNCH-ORIGIN] value-proxied props: same install shape as the zero-proxy (offset 0 so no slack memory is ever read; +0x70 cleared so no template child-table pointer rides along), but the proxy serves a REAL captured value.
			// Distinct proxies remain correct hygiene.
			const DTExtendProxyFn valueProxy = zeroProxy ? nullptr
				: DTExtend_ValueProxyForAppendedProp(tableName, ep.propName);
			// DT_Player / DT_BCC windows alias m_weaponAnimEvents. A missed
			// proxy must not fall through to a raw mid-class read.
			const bool aliasTable = (strcmp(tableName, "DT_Player") == 0)
				|| (strcmp(tableName, "DT_BaseCombatCharacter") == 0);
			bool forcedZero = false;
			if (aliasTable && !zeroProxy && !valueProxy)
			{
				forcedZero = true;
				Warning(eDLL_T::ENGINE,
					"[dt_extend] %s.%s has no value proxy -- forcing zero-proxy "
					"(18700/7200 alias m_weaponAnimEvents)\n",
					tableName, ep.propName);
				*(int*)(dst + SP_OFFSET) = 0;
				*(uintptr_t*)(dst + 0x60) =
					(uintptr_t)DTExtend_ZeroProxyForProp(ep.propName);
				*(uintptr_t*)(dst + 0x70) = 0;
			}
			if (valueProxy)
			{
				*(int*)(dst + SP_OFFSET) = 0;
				*(uintptr_t*)(dst + 0x60) = (uintptr_t)valueProxy;
				*(uintptr_t*)(dst + 0x70) = 0;
			}

			// NON-proxied appends must not keep the donor template's +0x70
			// child-table pointer (DPT_DataTable slot).
			if ((int)ep.type != 10 && !zeroProxy && !valueProxy && !forcedZero)
				*(uintptr_t*)(dst + 0x70) = 0;

			const bool proxied = zeroProxy || (valueProxy != nullptr) || forcedZero;
			if (!proxied)
			{
				const int end = curOffset + (ep.valBytes > 0 ? ep.valBytes : 4);
				if (end > rawEnd) rawEnd = end;
			}
		if (s_assignedCount < 512)
		{
			s_assigned[s_assignedCount].tableName  = ep.tableName;
			s_assigned[s_assignedCount].propName   = ep.propName;
			// Proxied props have NO entity-memory slot: record -1 (not 0) so
			// DTExtend_GetOffset callers can't mistake it for a real offset
			// and read/write *(entity + 0) -- the vptr.
			s_assigned[s_assignedCount].offset      = proxied ? -1 : curOffset;
			s_assigned[s_assignedCount].resolved    = false;
			s_assignedCount++;
			if (isNestedVisit)
				Msg(eDLL_T::ENGINE, "[dt_extend] nested append %s.%s %s\n",
					ep.tableName, ep.propName,
					proxied ? (zeroProxy ? "zero-proxy" : "value-proxy") : "entity-slot");
		}
			else
				Warning(eDLL_T::ENGINE,
					"[dt_extend] s_assigned FULL (512) -- %s.%s offset NOT recorded "
					"(DTExtend_GetOffset returns -1 for it; grow the array)\n",
					ep.tableName, ep.propName);

			int valSize = ep.valBytes > 0 ? ep.valBytes : 4;
			curOffset = (curOffset + valSize + 3) & ~3;
			appendIdx++;
		}

		// [TRIG-HEAVY-GROW] bounds fail-safe: grown window is [3440, 3584). If any
		// assigned offset reaches or past the end, disarm the whole table rather
		// than leave a phantom tail (base moved but bytes not owned).
		if (s_triggerHeavyGrown
			&& tableName
			&& strcmp(tableName, "DT_TriggerCylinderHeavy") == 0)
		{
			const char* badProp = nullptr;
			int badOff = -1;
			for (int j = oldNProps; j < appendIdx; ++j)
			{
				uint8_t* dst = newProps + (uint64_t)j * SP_SIZE;
				const int off = *(int*)(dst + SP_OFFSET);
				if (off <= 0)
					continue;
				const int sz = *(int*)(dst + SP_SIZEOFVAR);
				const int end = off + (sz > 0 ? sz : 4);
				if (off >= kTriggerHeavyGrownAlloc || end > kTriggerHeavyGrownAlloc)
				{
					badProp = *(const char**)(dst + SP_VARNAME);
					badOff = off;
					break;
				}
			}
			if (badProp || rawEnd > kTriggerHeavyGrownAlloc)
			{
				if (!badProp)
				{
					// rawEnd alone tripped -- name the last non-proxied append.
					for (int j = appendIdx - 1; j >= oldNProps; --j)
					{
						uint8_t* dst = newProps + (uint64_t)j * SP_SIZE;
						const int off = *(int*)(dst + SP_OFFSET);
						if (off > 0)
						{
							badProp = *(const char**)(dst + SP_VARNAME);
							badOff = off;
							break;
						}
					}
				}
				Warning(eDLL_T::ENGINE,
					"[TRIG-HEAVY-GROW] %s.%s offset %d reaches past grown alloc %d -- "
					"disarming whole table to zero-proxy (no phantom tail)\n",
					tableName, badProp ? badProp : "?", badOff, kTriggerHeavyGrownAlloc);
				s_triggerHeavyGrown = false;
				for (int j = oldNProps; j < appendIdx; ++j)
				{
					uint8_t* dst = newProps + (uint64_t)j * SP_SIZE;
					const char* nm = *(const char**)(dst + SP_VARNAME);
					*(int*)(dst + SP_OFFSET) = 0;
					*(uintptr_t*)(dst + 0x60) =
						(uintptr_t)DTExtend_ZeroProxyForProp(nm);
					*(uintptr_t*)(dst + 0x70) = 0;
				}
				for (int a = 0; a < s_assignedCount && a < 512; ++a)
				{
					if (s_assigned[a].tableName
						&& !strcmp(s_assigned[a].tableName, "DT_TriggerCylinderHeavy"))
						s_assigned[a].offset = -1;
				}
				rawEnd = 0;
			}
		}

		// Apply insertAfter reorders for this table: any registered prop whose ep.insertAfter is set gets memmove'd from its tail-append slot to immediately after the named anchor, so the S21 client's changed-prop index stream matches the dedi's flat layout.
		// Generalizes the per-class DTExtend_FixPlayerDecoyPropOrder hack.
		for (int p = 0; p < kNumExtendProps; ++p)
		{
			const DTExtendProp& ep = s_extendProps[p];
			if (!ep.tableName || strcmp(ep.tableName, tableName) != 0) continue;
			if (!ep.insertAfter || !ep.insertAfter[0]) continue;

			int srcIdx = -1;
			for (int j = oldNProps; j < newNProps; ++j)
			{
				const char* nm = *(const char**)(newProps + (uint64_t)j * SP_SIZE + SP_VARNAME);
				if (nm && strcmp(nm, ep.propName) == 0) { srcIdx = j; break; }
			}
			if (srcIdx < 0) continue;

			int anchorIdx = -1;
			for (int j = 0; j < newNProps; ++j)
			{
				if (j == srcIdx) continue;
				const char* nm = *(const char**)(newProps + (uint64_t)j * SP_SIZE + SP_VARNAME);
				if (nm && strcmp(nm, ep.insertAfter) == 0) { anchorIdx = j; break; }
			}
			if (anchorIdx < 0)
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] %s.%s: insertAfter anchor '%s' not found, "
					"leaving at tail slot %d\n",
					tableName, ep.propName, ep.insertAfter, srcIdx);
				continue;
			}

			const int wantIdx = anchorIdx + 1;
			if (srcIdx == wantIdx)
			{
				Msg(eDLL_T::ENGINE,
					"[dt_extend] %s.%s already at S21 slot %d (after %s)\n",
					tableName, ep.propName, srcIdx, ep.insertAfter);
				continue;
			}

			uint8_t tmp[SP_SIZE];
			memcpy(tmp, newProps + (uint64_t)srcIdx * SP_SIZE, SP_SIZE);
			if (srcIdx > wantIdx)
			{
				memmove(newProps + (uint64_t)(wantIdx + 1) * SP_SIZE,
					newProps + (uint64_t)wantIdx * SP_SIZE,
					(uint64_t)(srcIdx - wantIdx) * SP_SIZE);
				memcpy(newProps + (uint64_t)wantIdx * SP_SIZE, tmp, SP_SIZE);
			}
			else
			{
				memmove(newProps + (uint64_t)srcIdx * SP_SIZE,
					newProps + (uint64_t)(srcIdx + 1) * SP_SIZE,
					(uint64_t)(wantIdx - srcIdx) * SP_SIZE);
				memcpy(newProps + (uint64_t)wantIdx * SP_SIZE, tmp, SP_SIZE);
			}

		}

		// Record the entity-memory window this table now reads, so the backing
		// pass below can prove every carrier class is actually that big.
		if (rawEnd > 0)
		{
			if (s_extendSpanCount < kMaxExtendSpans)
			{
				ExtendSpan& es  = s_extendSpans[s_extendSpanCount++];
				es.tableName    = tableName;
				es.table        = table;
				es.base         = baseOffset;
				es.requiredEnd  = rawEnd;
			}
			else
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] extend-span table FULL (%d) -- %s is NOT backing-checked; "
					"grow kMaxExtendSpans\n",
					kMaxExtendSpans, tableName);
			}
		}

		// Commit: update the SendTable's prop pointer and count
		*(uint8_t**)(table + ST_PROPS) = newProps;
		*(int*)(table + ST_NPROPS) = newNProps;

		// Appended props were memcpy'd from a template (often a foreign table).
		// Repoint parentTable to THIS table so pack guts resolve the right base.
		DTExtend_FixParentTablesInTree(reinterpret_cast<uint8_t*>(table));
	}

	{
		const int nDupLever = bridge_dt_dup_append_suppress.GetInt();
		const char* const pszDupAct = nDupLever
			? "SUPPRESSED (native kept)"
			: "APPENDED ANYWAY (legacy)";
		if (nDupNameSuppressed > 0)
			Warning(eDLL_T::ENGINE,
				"[EXT-DUP] %d name-collision props %s (bridge_dt_dup_append_suppress=%d)\n",
				nDupNameSuppressed, pszDupAct, nDupLever);
		else
			Msg(eDLL_T::ENGINE,
				"[EXT-DUP] %d name-collision props %s (bridge_dt_dup_append_suppress=%d)\n",
				nDupNameSuppressed, pszDupAct, nDupLever);
	}

	// Back every appended offset with real allocation before a map can spawn one
	// of these entities. Grow first, then audit what the grow did not reach.
	const int extGrown = DTExtend_GrowExtendBackedClasses();
	const int extShort = DTExtend_AuditExtendBacking();
	DTExtend_AuditExtendUnwritten();

	// Create-imm is the only allocator. Relocate and FACT_ALLOCSIZE follow it.
	int sndcGetSizeOverrides = DTExtend_OverrideSNDCFactoryGetSize();

	// --- SNDC Array Resize --- Modify existing Array SendProps in-place to match S21's nElements + offsets.
	// Each SNDC table has exactly 5 Array props (m_bools, m_ranges, m_int32s, m_times, m_entities).
	int sndcResized = 0;
	for (int s = 0; s < kNumSNDCSpecs; ++s)
	{
		const SNDCTableSpec& spec = s_sndcSpecs[s];
		if (!s_sndcFamilyCreateOk[s])
		{
			Error(eDLL_T::ENGINE, EXIT_FAILURE,
				"[dt_extend] SNDC %s: Create-imm not grown -- "
				"refusing mixed native nElem against S21 Recv\n",
				spec.tableName);
		}

		// Find this SNDC table in the tables array
		uintptr_t sndcTable = 0;
		for (int i = 0; i < count; ++i)
		{
			const char* tn = *(const char**)((uintptr_t)tables[i] + ST_NETTABLENAME);
			if (tn && strcmp(tn, spec.tableName) == 0)
			{
				sndcTable = (uintptr_t)tables[i];
				break;
			}
		}
		if (!sndcTable)
		{
			Warning(eDLL_T::ENGINE, "[dt_extend] SNDC table '%s' NOT FOUND in tables[]\n",
				spec.tableName);
			continue;
		}

		uint8_t* props = *(uint8_t**)(sndcTable + ST_PROPS);
		int nProps = *(int*)(sndcTable + ST_NPROPS);

		for (int a = 0; a < 5; ++a)
		{
			const SNDCArrayInfo& ai = spec.arrays[a];

			// Find the Array SendProp by name (track index for template lookup)
			uint8_t* arrayProp = nullptr;
			int arrayPropIdx = -1;
			for (int j = 0; j < nProps; ++j)
			{
				uint8_t* prop = props + (uint64_t)j * SP_SIZE;
				int type = *(int*)(prop + SP_TYPE);
				if (type != 5) continue; // DPT_Array only
				const char* name = *(const char**)(prop + SP_VARNAME);
				if (name && strcmp(name, ai.name) == 0)
				{
					arrayProp = prop;
					arrayPropIdx = j;
					break;
				}
			}
			if (!arrayProp)
			{
				Warning(eDLL_T::ENGINE, "[dt_extend] SNDC %s.%s Array prop NOT FOUND\n",
					spec.tableName, ai.name);
				continue;
			}

			int oldNElem = *(int*)(arrayProp + SP_NELEMENTS);

			// Patch array container: nElements + sizeofVar.
			// Array prop's own m_Offset stays 0 (container; engine reads element [k]
			// at template.SP_OFFSET + k*elemSize via the preceding template prop).
			*(int*)(arrayProp + SP_NELEMENTS) = ai.nElements;
			*(int*)(arrayProp + SP_SIZEOFVAR) = ai.nElements * ai.elemSize;

			// The dispatcher is now ALSO patched via scriptnetdata_limits.cpp's PatchSNDCNativeLayout vtable disp32 rewrites, so encoder + dispatcher move together.
			// Template is the PRECEDING prop with the same varname (PE_EXPANDED pattern at line 2407+).
			int oldTemplateOff = -1, newTemplateOff = -1;
			if (arrayPropIdx > 0)
			{
				uint8_t* tmplProp = props + (uint64_t)(arrayPropIdx - 1) * SP_SIZE;
				const char* tmplName = *(const char**)(tmplProp + SP_VARNAME);
				if (tmplName && strcmp(tmplName, ai.name) == 0)
				{
					oldTemplateOff = *(int*)(tmplProp + SP_OFFSET);
					*(int*)(tmplProp + SP_OFFSET) = ai.offset;
					newTemplateOff = ai.offset;
				}
				else
				{
					Warning(eDLL_T::ENGINE,
						"[dt_extend] SNDC %s.%s: prev prop name '%s' != '%s' -- template offset NOT patched\n",
						spec.tableName, ai.name, tmplName ? tmplName : "<null>", ai.name);
				}
			}

			Warning(eDLL_T::ENGINE,
				"[dt_extend] SNDC %s.%s: nElem %d->%d tmplOff %d->%d\n",
				spec.tableName, ai.name,
				oldNElem, ai.nElements,
				oldTemplateOff, newTemplateOff);
			sndcResized++;
		}
	}

	DTExtend_RetargetNonRewindSendProps();

	// --- DT_Player.m_passives 2 -> 3 element resize (SNDC method + tail-zero) --- S3 sends m_passives as a 2-element Int64 array (128 passives @ CPlayer+0x5FF0); the S21 client's RecvProp is array[3] (192).
	// The shape mismatch left the prop ?_unmatched on the client, so BOTH real passive words were discarded.
	{
		uintptr_t playerTable = 0;
		for (int i = 0; i < count; ++i)
		{
			const char* tn = *(const char**)((uintptr_t)tables[i] + ST_NETTABLENAME);
			if (tn && strcmp(tn, "DT_Player") == 0)
			{
				playerTable = (uintptr_t)tables[i];
				break;
			}
		}
		if (!playerTable)
		{
			Warning(eDLL_T::ENGINE, "[dt_extend] m_passives: DT_Player NOT FOUND in tables[]\n");
		}
		else
		{
			uint8_t* props = *(uint8_t**)(playerTable + ST_PROPS);
			int nProps = *(int*)(playerTable + ST_NPROPS);

			uint8_t* arrayProp = nullptr;
			int arrayIdx = -1;
			for (int j = 0; j < nProps; ++j)
			{
				uint8_t* prop = props + (uint64_t)j * SP_SIZE;
				if (*(int*)(prop + SP_TYPE) != 5) continue; // DPT_Array
				const char* name = *(const char**)(prop + SP_VARNAME);
				if (name && strcmp(name, "m_passives") == 0)
				{
					arrayProp = prop;
					arrayIdx = j;
					break;
				}
			}
			if (!arrayProp)
			{
				Warning(eDLL_T::ENGINE, "[dt_extend] m_passives Array prop NOT FOUND in DT_Player\n");
			}
			else
			{
				constexpr int kS21PassiveWords = 3;  // S21 RecvProp array[3] (192 passives)
				constexpr int kPassiveElemSize = 8;  // Int64
				const int oldNElem = *(int*)(arrayProp + SP_NELEMENTS);

				*(int*)(arrayProp + SP_NELEMENTS) = kS21PassiveWords;
				*(int*)(arrayProp + SP_SIZEOFVAR) = kS21PassiveWords * kPassiveElemSize;

				// Template = preceding prop sharing the varname (engine [template,
				// array] idiom). Keep its SP_OFFSET (+0x5FF0); only swap the value
				// proxy so words 0..1 pass through and word 2 emits 0.
				int tmplOff = -1;
				bool proxied = false;
				if (arrayIdx > 0)
				{
					uint8_t* tmpl = props + (uint64_t)(arrayIdx - 1) * SP_SIZE;
					const char* tmplName = *(const char**)(tmpl + SP_VARNAME);
					if (tmplName && strcmp(tmplName, "m_passives") == 0)
					{
						tmplOff = *(int*)(tmpl + SP_OFFSET);
						*(uintptr_t*)(tmpl + 0x60) = (uintptr_t)&Passives_TailZeroProxy;
						proxied = true;
					}
				}

				if (proxied)
				{
					Warning(eDLL_T::ENGINE,
						"[dt_extend] DT_Player.m_passives: nElem %d->%d sizeofVar->%d tmplOff=0x%X (tail-zero word>=2)\n",
						oldNElem, kS21PassiveWords, kS21PassiveWords * kPassiveElemSize, tmplOff);
				}
				else
				{
					// No safe template -> a grown count without the tail-zero proxy
					// would leak +0x6000. Revert rather than ship a leaking word.
					*(int*)(arrayProp + SP_NELEMENTS) = oldNElem;
					*(int*)(arrayProp + SP_SIZEOFVAR) = oldNElem * kPassiveElemSize;
					Warning(eDLL_T::ENGINE,
						"[dt_extend] m_passives: no 'm_passives' template at idx %d -- resize REVERTED (avoids +0x6000 leak)\n",
						arrayIdx - 1);
				}
			}
		}
	}

	// --- DT_HighlightSettings full rebuild to the S21 6-member shape (HUD highlights) --- The S21 highlight subsystem fully diverged from S3: only m_highlightTeamBits is name-shared (S3 scalar @ +0x21C / ctx @ +0x218 -> S21 per-context Int32[8]).
	// The S3 dedi shipping its raw 7-member DT_HighlightSettings left the S21 client with 6 ?_unmatched array/datatable nodes (m_highlightParams/FunctionBits/ServerFade*/ ServerContextID) that mis-consume in the enter-PVS instance-baseline decode -> bitstream desync -> spawn crash (canyonlands_hu CDynamicProp).
	DTExtend_RebuildHighlightSettings(tables, count);

	// --- Time/Ticks codec divergence --- DPT_Time(9)/DPT_Ticks(8) wire formats diverge S3<->S21 (the S21 decoders Time_Read / Ticks_Read are network-time-relative variable-width codecs).
	// The reconciliation is now ENCODE-side, NOT a send-table retype: snapshot_diag.cpp's DT_EncodePropValue hook (bridge_time_encode) emits the S21 wire format directly, keeping the prop type-9/8 so SNDC m_times native dispatch is preserved.

	// --- SNDC Entity Factory Growth --- Walk the factory linked list, find each SNDC factory by name, patch allocSize (+0x1C) to fit the resized arrays.
	// Factory names are "C..." where table names are "DT_..." -- compare after skipping the prefix.
	int factoriesGrown = 0;
	if (g_pFactoryListHead && *g_pFactoryListHead)
	{
		for (int s = 0; s < kNumSNDCSpecs; ++s)
		{
			const SNDCTableSpec& spec = s_sndcSpecs[s];
			if (!s_sndcFamilyCreateOk[s])
				continue;
			const char* suffix = spec.tableName + 3; // skip "DT_"

			uintptr_t node = *g_pFactoryListHead;
			for (int guard = 0; node && guard < 4096; ++guard)
			{
				const char* className = *(const char**)(node + FACT_CLASSNAME);
				if (className && className[0] == 'C' && strcmp(className + 1, suffix) == 0)
				{
					int oldSize = *(int*)(node + FACT_ALLOCSIZE);
					if (oldSize < spec.requiredAllocSize)
					{
						*(int*)(node + FACT_ALLOCSIZE) = spec.requiredAllocSize;
						factoriesGrown++;
					}
					break;
				}
				node = *(uintptr_t*)(node + FACT_NEXT);
			}
		}

		// Also grow the CScriptNetDataGlobal wrapper entity (classID 73).
		// It uses DT_ScriptNetDataGlobal which nests DT_ScriptNetData_SNDC_GLOBAL
		// as a sub-table. Same data layout, same required alloc.
		{
			uintptr_t node = *g_pFactoryListHead;
			for (int guard = 0; node && guard < 4096; ++guard)
			{
				const char* className = *(const char**)(node + FACT_CLASSNAME);
				if (className && strcmp(className, "CScriptNetDataGlobal") == 0)
				{
					if (!s_sndcFamilyCreateOk[5])
						break;
					int oldSize = *(int*)(node + FACT_ALLOCSIZE);
					if (oldSize < s_sndcSpecs[0].requiredAllocSize)
					{
						*(int*)(node + FACT_ALLOCSIZE) = s_sndcSpecs[0].requiredAllocSize;
						factoriesGrown++;
					}
					break;
				}
				node = *(uintptr_t*)(node + FACT_NEXT);
			}
		}
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"[dt_extend] WARNING: factory list head unresolved, SNDC factories NOT grown\n");
	}

	bool sndcAllBacked = true;
	for (int fi = 0; fi < kNumSNDCFamily; ++fi)
	{
		if (!s_sndcFamilyCreateOk[fi])
			sndcAllBacked = false;
	}
	if (sndcAllBacked)
		SNDC_ApplyNativeLayoutIfBacked();
	else
		Warning(eDLL_T::ENGINE,
			"[dt_extend] SNDC native layout SKIPPED -- not every Create-imm meets the S21 high-water (overrides=%d)\n",
			sndcGetSizeOverrides);

	// Grow CWorld so the 64-ring deathfield arrays have REAL backing memory instead of overrunning into adjacent heap (the shutdown-crash cause).
	// DEATHFIELD-ONLY: gated on launch intent so sdk_deathfield_native_dt=0 leaves the world entity at its native size.
	int worldGrown = 0;
	if (DeathField_NativeDTRequestedAtLaunch())
		worldGrown = DTExtend_GrowWorldEntity();
	else
		Warning(eDLL_T::ENGINE,
			"[dt_extend] world grow SKIPPED (sdk_deathfield_native_dt off -- "
			"world entity stays native size)\n");

	// [PARENT-FIX-FINAL] Global parentTable pass is NOT here: running it before PostApplyS21Overrides / Relocate / Copy / InsertWrapper / Bracketize / Offhand / DeathField left later structural passes free to memcpy SendProps with foreign +0x08 pointers.
	// Authoritative walk is in Hook_SendTable_Init immediately before v_SendTable_Init (after the last structural mutator).

	Warning(eDLL_T::ENGINE,
		"[dt_extend] done: %d offsets assigned, %d SNDC arrays resized, %d factories grown, %d GetSize overrides, world grow imms=%d, %d extend windows backed, %d disarmed\n",
		s_assignedCount, sndcResized, factoriesGrown, sndcGetSizeOverrides, worldGrown,
		extGrown, extShort);

	// Heap-integrity checkpoint at the SendTable-extension boundary. dt_extend resizes ServerClass alloc sizes + grows SNDC factory entity sizes; if any grow under-sized a backing region this is the first place an overflow would be visible.
	// No-op unless sdk_heap_canary 1.
	HeapCanary::Checkpoint("post-dt-extend");
}
// ServerClass list lookup by network name -> its SendTable (node +0x08).
static uintptr_t DTExtend_FindServerClassTable(const char* className)
{
	if (!className || !g_pFactoryListHead || !*g_pFactoryListHead)
		return 0;
	uintptr_t node = *g_pFactoryListHead;
	for (int safety = 0; node && safety < 4096; ++safety)
	{
		const char* cn = *(const char**)(node + FACT_CLASSNAME);
		if (cn && strcmp(cn, className) == 0)
			return *(uintptr_t*)(node + 0x08);
		node = *(uintptr_t*)(node + FACT_NEXT);
	}
	return 0;
}

void DTExtend_PostApplyS21Overrides(void** tables, int count)
{
	if (!s_s21Slots) return;
	bool clonedAny = false;
	for (int ci = 0; ci < kNumS21Classes; ++ci)
	{
		const S21ClassDef& def = s_s21Classes[ci];

		S21ClassSlot& slot = s_s21Slots[ci];
		if (!*(const char**)(slot.factory + FACT_CLASSNAME)) continue; // slot never initialized

		// Allocate/reset the clone pool. Every synthetic S21 ServerClass must
		// own an independent SendTable tree; SendTable_Init writes precalc and
		// matcher metadata into those nodes.
		if (!slot.treePool.base)
		{
			slot.treePool.base = (uint8_t*)VirtualAlloc(
				nullptr, CLONE_POOL_SIZE,
				MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (!slot.treePool.base)
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] S21Class '%s': VirtualAlloc clone pool failed -- SKIPPED\n",
					def.className);
				continue;
			}
			slot.treePool.used = 0;
		}
		else
		{
			slot.treePool.used = 0;
		}

		// Find parent wrapper SendTable from factory list. wireParentDT wins
		// where the S21 inheritance chain diverges from the S3 entity chain --
		// only the transmitted tree moves, the entity stays parentFactory's.
		const char* cloneSrcDT =
			def.wireParentDT ? def.wireParentDT : def.parentDT;
		uintptr_t parentWrapper = DTExtend_FindServerClassTable(cloneSrcDT);
		if (!parentWrapper && def.wireParentDT)
		{
			// Fall back rather than drop the class: skipping one here changes the
			// wire class count for every other class too, which is a far worse
			// failure than shipping the old (wrong-chain) clone source.
			Warning(eDLL_T::ENGINE,
				"[dt_extend] S21Class '%s': wire parent '%s' NOT FOUND -- "
				"falling back to entity parent '%s'\n",
				def.className, cloneSrcDT, def.parentDT);
			cloneSrcDT = def.parentDT;
			parentWrapper = DTExtend_FindServerClassTable(cloneSrcDT);
		}
		if (!parentWrapper)
		{
			Warning(eDLL_T::ENGINE,
				"[dt_extend] S21Class '%s': wire parent '%s' NOT FOUND -- skipped\n",
				def.className, cloneSrcDT);
			continue;
		}

		// Deep-clone parent's wrapper -> independent tree in slot's pool.
		uint8_t* clonedWrapper = DeepCloneSendTable((void*)parentWrapper, slot.treePool);
		if (!clonedWrapper)
		{
			Warning(eDLL_T::ENGINE,
				"[dt_extend] S21Class '%s': DeepCloneSendTable failed -- skipped\n",
				def.className);
			continue;
		}

		// Replace slot.wrapperTable contents with the cloned wrapper (preserves
		// the slot.wrapperTable address that the factory's m_pSendTable still
		// points at -- engine reads through that pointer post-this step).
		if (strcmp(def.dtName, "DT_MaterialHarvester") == 0)
		{
			if (!DTExtend_MaterialHarvesterStateHasBacking())
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] material harvester: Create alloc was not grown -- "
					"m_collectedState wrapper NOT built (the prop would pack past "
					"the entity allocation)\n");
				continue;
			}
			if (!DTExtend_BuildMaterialHarvesterWrapper(
				slot.wrapperTable, clonedWrapper, slot.treePool))
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] material harvester: wrapper build failed -- skipped\n");
				continue;
			}
			clonedAny = true;
			Warning(eDLL_T::ENGINE,
				"[dt_extend] material harvester: built wrapper with collected-state table "
				"(pool %zu/%zu)\n",
				slot.treePool.used, CLONE_POOL_SIZE);
			continue;
		}

		// Clone native DT_TriggerSlip (parentDT=CTriggerSlip), rename root + inheritance prop to DT_TriggerSlipSphere so S21 client binds the three force fields (dir/speed/accel) for prediction.
		// Create path stays native CTriggerSlip (vtable steal) so offsets 3296/3308/3312 are live.
		if (strcmp(def.className, "CTriggerSlipSphere") == 0)
		{
			memcpy(slot.wrapperTable, clonedWrapper, NR_SENDTABLE_SIZE);
			if (!DTExtend_RetargetTriggerSlipSphereWrapper(slot.wrapperTable))
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CTriggerSlipSphere: retarget FAILED -- skipped "
					"(pool %zu/%zu)\n",
					slot.treePool.used, CLONE_POOL_SIZE);
				continue;
			}
			clonedAny = true;
			Warning(eDLL_T::ENGINE,
				"[dt_extend] CTriggerSlipSphere: deep-cloned DT_TriggerSlip -> "
				"DT_TriggerSlipSphere with force props (pool %zu/%zu)\n",
				slot.treePool.used, CLONE_POOL_SIZE);
			continue;
		}

		// S21 CreateDecoders: every DPT_DataTable prop resolves the CHILD table BY NAME from the received send-table list (name array at sendTable[162], table name at table+0x4B8, child bound to prop+0x70).
		// Prop varname is only for the error string -- what must line up is the CHAIN OF TABLE NAMES.
		if (def.nestRoot)
		{
			// The wrapper was seeded at registration with a memcpy of the ENTITY parent's table (def.parentDT).
			// Anything on its root now that the seed did not have was put there by a later pass and belongs to this class -- DT_TriggerCylinderNetworked gets m_triggerFilterMask / m_radius / m_aboveHeight / m_belowHeight from s_propRelocations, and CTriggerCylinderHeavy inherits them through the WRAPPER-RDR splice.
			uint8_t* const oldProps = *(uint8_t**)(slot.wrapperTable + ST_PROPS);
			const int oldN = *(int*)(slot.wrapperTable + ST_NPROPS);

			const uint8_t* dtTemplate = DTExtend_FindPropTemplateInTree(
				clonedWrapper, static_cast<int>(SendPropType::DPT_DataTable));
			const char* childName =
				*(const char**)(clonedWrapper + ST_NETTABLENAME);

			const uintptr_t seedTable = DTExtend_FindServerClassTable(def.parentDT);
			uint8_t* const seedProps = seedTable
				? *(uint8_t**)(seedTable + ST_PROPS) : nullptr;
			const int seedN = seedTable ? *(int*)(seedTable + ST_NPROPS) : 0;

			int keptN = 0;
			int keptIdx[32];
			for (int i = 0; oldProps && seedProps && i < oldN && i < 4096 && keptN < 32; ++i)
			{
				const char* nm =
					*(const char**)(oldProps + (uint64_t)i * SP_SIZE + SP_VARNAME);
				if (!nm)
					continue;
				bool inSeed = false;
				for (int j = 0; j < seedN && j < 4096; ++j)
				{
					const char* pn = *(const char**)(
						seedProps + (uint64_t)j * SP_SIZE + SP_VARNAME);
					if (pn && strcmp(pn, nm) == 0) { inSeed = true; break; }
				}
				if (!inSeed)
					keptIdx[keptN++] = i;
			}

			const int newN = keptN + 1;
			uint8_t* rootProps = static_cast<uint8_t*>(
				slot.treePool.alloc(static_cast<size_t>(newN) * SP_SIZE));
			if (!rootProps || !dtTemplate)
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] S21Class '%s': nested wrapper build FAILED "
					"(props=%p template=%p) -- skipped\n",
					def.className, (void*)rootProps, (const void*)dtTemplate);
				continue;
			}
			for (int k = 0; k < keptN; ++k)
				memcpy(rootProps + (uint64_t)(k + 1) * SP_SIZE,
					oldProps + (uint64_t)keptIdx[k] * SP_SIZE, SP_SIZE);

			memcpy(slot.wrapperTable, clonedWrapper, NR_SENDTABLE_SIZE);

			memcpy(rootProps, dtTemplate, SP_SIZE);
			*(int*)(rootProps + SP_TYPE) =
				static_cast<int>(SendPropType::DPT_DataTable);
			*(const char**)(rootProps + SP_VARNAME) = def.dtName;
			*(int*)(rootProps + SP_NELEMENTS) = 1;
			*(int*)(rootProps + SP_OFFSET) = 0;
			*(uintptr_t*)(rootProps + 0x70) =
				reinterpret_cast<uintptr_t>(clonedWrapper);

			*(uint8_t**)(slot.wrapperTable + ST_PROPS) = rootProps;
			*(int*)(slot.wrapperTable + ST_NPROPS) = newN;
			*(const char**)(slot.wrapperTable + ST_NETTABLENAME) = def.dtName;
			*(uintptr_t*)(slot.wrapperTable + 0x4C0) = 0;
			*(uintptr_t*)(slot.wrapperTable + 0x508) = 0;
			DTExtend_FixParentTablesInTree(slot.wrapperTable);
			clonedAny = true;
			Warning(eDLL_T::ENGINE,
				"[dt_extend] S21Class '%s': NESTED wrapper root '%s' (%d props: "
				"nest + %d carried) -> child '%s' (pool %zu/%zu)\n",
				def.className, def.dtName, newN, keptN,
				childName ? childName : "<null>",
				slot.treePool.used, CLONE_POOL_SIZE);
			continue;
		}

		if (strcmp(def.className, "CZiprail") == 0)
		{
			// [ZIPRAIL-NEST] Flat rename (memcpy clone -> root "DT_Ziprail") decodes to nothing: client pairs by name per table; native recv DT_Ziprail root is ["DT_Ziprail"(dt->DT_Zipline), 9 ziprail props], so a wire root carrying DT_Zipline props matches zero names (bits consumed, nothing applied; 0xF8C stays 0).
			// Transmit the shape the client expects: root "DT_Ziprail" with one inheritance DataTable prop named "DT_Ziprail" whose child is the full DT_Zipline clone.
			memcpy(slot.wrapperTable, clonedWrapper, NR_SENDTABLE_SIZE);

			// [ZIPRAIL-WIRE] Root = 1 inheritance shim + the 9 native DT_Ziprail props (name-matched to the S21 recv root).
			// Values pack from the per-chain side block via the ZiprailWire_* proxies (see the [ZIPRAIL-WIRE] block above for the ABI ground truth); the arrays carry ALL chain nodes (32 max) -- this retires the 16-slot rest-array downsampling that cut poles off long chains.
			constexpr int kZpRootProps = 10;
			uint8_t* zpProps = static_cast<uint8_t*>(
				slot.treePool.alloc(static_cast<size_t>(kZpRootProps) * SP_SIZE));
			const uint8_t* zpCloneProps = *(uint8_t**)(clonedWrapper + ST_PROPS);
			// Donor = the clone's own inheritance prop (prop[0] of DT_Zipline
			// type-10, name "DT_Zipline", flags 0x500, offset 0, dt proxy).
			const bool zpDonorOk = zpCloneProps &&
				*(const int*)(zpCloneProps + SP_TYPE) == 10;
			if (!zpProps || !zpDonorOk)
			{
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CZiprail: nested wrapper build FAILED "
					"(props=0x%p donorType=%d) -- skipped\n",
					(void*)zpProps,
					zpCloneProps ? *(const int*)(zpCloneProps + SP_TYPE) : -1);
				continue;
			}
			memcpy(zpProps, zpCloneProps, SP_SIZE);
			// Must equal the client recv root's inheritance prop name
			// ("DT_Ziprail", verified at).
			*(const char**)(zpProps + SP_VARNAME) = def.dtName;
			*(int*)(zpProps + SP_OFFSET) = 0;
			*(uintptr_t*)(zpProps + 0x70) = (uintptr_t)clonedWrapper;
			*(uintptr_t*)(zpProps + SP_PARENTTABLE) = (uintptr_t)slot.wrapperTable;

			// Donors, all native S3 props living in the DT_Zipline clone.
			uint8_t* zpIntDonor  = ZiprailWire_FindRootProp(clonedWrapper, "m_ziplineMaterialIndex", 0);
			uint8_t* zpBoolDonor = ZiprailWire_FindRootProp(clonedWrapper, "m_ziplineEnabled", 0);
			uint8_t* zpArrDonor  = ZiprailWire_FindRootProp(clonedWrapper, "m_ziplineRestPositions", 10);
			// Full-width float donor (quantized low-bit floats wreck arc lengths in the tens of thousands of units).
			// DT_Zipline raw floats (m_ziplineWidth / m_ziplineSpeedScale / m_ziplineFadeDist / m_ziplineAutoDetachDistance) are type-1 SP_NBITS=0 SP_FLAGS=0x4 (NOSCALE).
			uint8_t* zpFloatDonor =
				ZiprailWire_FindRootProp(clonedWrapper, "m_ziplineWidth", 1);
			if (!zpFloatDonor)
			{
				uint8_t* cp = *(uint8_t**)(clonedWrapper + ST_PROPS);
				const int cn = *(int*)(clonedWrapper + ST_NPROPS);
				for (int i = 0; cp && i < cn && i < 4096; ++i)
				{
					uint8_t* p = cp + (uint64_t)i * SP_SIZE;
					if (*(int*)(p + SP_TYPE) == 1 &&
						((*(int*)(p + SP_FLAGS) & 0x4) ||
						 *(int*)(p + SP_NBITS) >= 32))
					{
						zpFloatDonor = p;
						break;
					}
				}
			}
			const uint8_t* zpVecElemDonor = nullptr; // rest child element [0000]: ty=2 world-coord
			if (zpArrDonor)
			{
				const uint8_t* restChild = *(const uint8_t* const*)(zpArrDonor + 0x70);
				const uint8_t* restElems = restChild ?
					*(const uint8_t* const*)(restChild + ST_PROPS) : nullptr;
				if (restElems && *(const int*)(restElems + SP_TYPE) == 2)
					zpVecElemDonor = restElems;
			}

			bool zpOk = zpIntDonor && zpBoolDonor && zpArrDonor && zpFloatDonor && zpVecElemDonor;
			if (zpOk)
			{
				s_ziprailWireMapCount = 0; // rebuildable (changelevel re-registration)
				uint8_t* d = zpProps;
				zpOk = zpOk && ZiprailWire_BuildScalar(d + 1 * SP_SIZE, zpIntDonor,
					"m_numZiprailPathNodes", offsetof(ZiprailWireBlock, numNodes), 0);
				zpOk = zpOk && ZiprailWire_BuildArray(d + 2 * SP_SIZE, zpArrDonor, zpIntDonor,
					"m_numSmoothPointsForPathNodes", offsetof(ZiprailWireBlock, numSmooth), 4, slot.treePool);
				zpOk = zpOk && ZiprailWire_BuildArray(d + 3 * SP_SIZE, zpArrDonor, zpIntDonor,
					"m_tangentTypesForPathNodes", offsetof(ZiprailWireBlock, tangentTypes), 4, slot.treePool);
				zpOk = zpOk && ZiprailWire_BuildArray(d + 4 * SP_SIZE, zpArrDonor, zpVecElemDonor,
					"m_positionsForPathNodes", offsetof(ZiprailWireBlock, positions), 12, slot.treePool);
				zpOk = zpOk && ZiprailWire_BuildArray(d + 5 * SP_SIZE, zpArrDonor, zpFloatDonor,
					"m_smoothDistanceToNode", offsetof(ZiprailWireBlock, smoothDistToNode), 4, slot.treePool);
				zpOk = zpOk && ZiprailWire_BuildScalar(d + 6 * SP_SIZE, zpFloatDonor,
					"m_ziprailPathLen", offsetof(ZiprailWireBlock, pathLen), 1);
				zpOk = zpOk && ZiprailWire_BuildScalar(d + 7 * SP_SIZE, zpVecElemDonor,
					"m_pathExtentsMins", offsetof(ZiprailWireBlock, extentsMins), 2);
				zpOk = zpOk && ZiprailWire_BuildScalar(d + 8 * SP_SIZE, zpVecElemDonor,
					"m_pathExtentsMaxs", offsetof(ZiprailWireBlock, extentsMaxs), 2);
				zpOk = zpOk && ZiprailWire_BuildScalar(d + 9 * SP_SIZE, zpBoolDonor,
					"m_ziprailUseAutoDetachSpeed", offsetof(ZiprailWireBlock, useAutoDetachSpeed), 3);
			}

			// m_ropeColorModulation is S21-only. s_extendProps already injects it into DT_Zipline (EV), so the deep-clone ALREADY carries one copy.
			// Appending a second copy pushes server flatN 340 vs client 339 -- NULL-slot crash (baseline+delta merge derefs m_Props[idx] past decoder slots).
			const char* zpRopeTag = " <no-rope>";
			if (zpOk)
			{
				uint8_t* existingRope =
					ZiprailWire_FindRootProp(clonedWrapper, "m_ropeColorModulation", 2);
				if (existingRope)
				{
					// SP_OFFSET = abs block offset: flatten clones this prop
					// (nested under the inheritance type-10), so the pointer
					// map alone would miss and pack black (0,0,0).
					*reinterpret_cast<int*>(existingRope + SP_OFFSET) =
						static_cast<int>(offsetof(ZiprailWireBlock, ropeColorModulation));
					*reinterpret_cast<uint8_t*>(existingRope + SP_PRIORITY) = 128;
					*reinterpret_cast<uintptr_t*>(existingRope + 0x60) =
						reinterpret_cast<uintptr_t>(&ZiprailWire_ScalarVectorProxy);
					// Map is best-effort (survives only if flatten keeps this
					// exact prop pointer); proxy does not depend on it.
					(void)ZiprailWire_AddMap(existingRope,
						offsetof(ZiprailWireBlock, ropeColorModulation), 2);
					zpRopeTag = " rewired-m_ropeColorModulation";
				}
				else
				{
					// Fallback: extend did not inject -- append once (rare).
					uint8_t* cloneProps = *(uint8_t**)(clonedWrapper + ST_PROPS);
					const int cloneN = *(int*)(clonedWrapper + ST_NPROPS);
					uint8_t* newCloneProps = static_cast<uint8_t*>(
						slot.treePool.alloc((static_cast<size_t>(cloneN) + 1) * SP_SIZE));
					if (!newCloneProps || cloneN <= 0 || cloneN > 4096)
					{
						zpOk = false;
					}
					else
					{
						memcpy(newCloneProps, cloneProps,
							static_cast<size_t>(cloneN) * SP_SIZE);
						uint8_t* rc = newCloneProps +
							static_cast<uint64_t>(cloneN) * SP_SIZE;
						if (ZiprailWire_BuildScalar(rc, zpVecElemDonor,
							"m_ropeColorModulation",
							offsetof(ZiprailWireBlock, ropeColorModulation), 2))
						{
							*(uint8_t**)(clonedWrapper + ST_PROPS) = newCloneProps;
							*(int*)(clonedWrapper + ST_NPROPS) = cloneN + 1;
							zpRopeTag = " appended-m_ropeColorModulation(fallback)";
						}
						else
						{
							zpOk = false;
						}
					}
				}

				uint8_t* existingRev =
					ZiprailWire_FindRootProp(clonedWrapper, "m_ziplineMountReverseDistance", 1);
				if (existingRev)
				{
					*reinterpret_cast<int*>(existingRev + SP_OFFSET) =
						static_cast<int>(offsetof(ZiprailWireBlock, mountReverseDistance));
					*reinterpret_cast<uint8_t*>(existingRev + SP_PRIORITY) = 128;
					*reinterpret_cast<uintptr_t*>(existingRev + 0x60) =
						reinterpret_cast<uintptr_t>(&ZiprailWire_ScalarFloatProxy);
					(void)ZiprailWire_AddMap(existingRev,
						offsetof(ZiprailWireBlock, mountReverseDistance), 1);
				}

				uint8_t* existingPrevent =
					ZiprailWire_FindRootProp(clonedWrapper, "m_ziplinePreventManualDetach", 0);
				if (existingPrevent)
				{
					*reinterpret_cast<int*>(existingPrevent + SP_OFFSET) =
						static_cast<int>(offsetof(ZiprailWireBlock, preventManualDetach));
					*reinterpret_cast<uint8_t*>(existingPrevent + SP_PRIORITY) = 128;
					*reinterpret_cast<uintptr_t*>(existingPrevent + 0x60) =
						reinterpret_cast<uintptr_t>(&ZiprailWire_ScalarIntProxy);
					(void)ZiprailWire_AddMap(existingPrevent,
						offsetof(ZiprailWireBlock, preventManualDetach), 0);
				}
			}

			if (!zpOk)
			{
				// Loud degrade: keep the proven 1-prop shim (rails render from
				// the rest-position fallback) instead of shipping a half-built
				// prop set.
				Warning(eDLL_T::ENGINE,
					"[dt_extend] CZiprail: [ZIPRAIL-WIRE] prop build FAILED "
					"(donors int=0x%p bool=0x%p arr=0x%p float=0x%p vecElem=0x%p, "
					"map=%d) -- falling back to 1-prop shim\n",
					(void*)zpIntDonor, (void*)zpBoolDonor, (void*)zpArrDonor,
					(void*)zpFloatDonor, (const void*)zpVecElemDonor,
					s_ziprailWireMapCount);
				s_ziprailWireMapCount = 0;
			}

			*(uint8_t**)(slot.wrapperTable + ST_PROPS) = zpProps;
			*(int*)(slot.wrapperTable + ST_NPROPS) = zpOk ? kZpRootProps : 1;
			*(const char**)(slot.wrapperTable + ST_NETTABLENAME) = def.dtName;
			*(uintptr_t*)(slot.wrapperTable + 0x4C0) = 0;
			*(uintptr_t*)(slot.wrapperTable + 0x508) = 0;
			// Re-own every prop onto its owning table: the shim root's props onto slot.wrapperTable, the clone tree (incl. rewired m_ropeColorModulation) onto the clone, and the new 32-element array children onto their child tables.
			// The pack resolves structBase through prop.parentTable's precalc index, so this must run AFTER all appends (same defect family as dedi 41e23726).
			DTExtend_FixParentTablesInTree(slot.wrapperTable);
			clonedAny = true;
			Warning(eDLL_T::ENGINE,
				"[dt_extend] CZiprail: built NESTED DT_Ziprail wrapper "
				"(root %d-prop '%s' -> DT_Zipline clone%s, pool %zu/%zu) -- "
				"native-shape ziprail wire%s\n",
				zpOk ? kZpRootProps : 1, def.dtName,
				zpOk ? zpRopeTag : "",
				slot.treePool.used, CLONE_POOL_SIZE,
				zpOk ? "" : " DEGRADED to rest-position fallback");
			continue;
		}

		memcpy(slot.wrapperTable, clonedWrapper, NR_SENDTABLE_SIZE);
		*(const char**)(slot.wrapperTable + ST_NETTABLENAME) = def.dtName;

		*(uintptr_t*)(slot.wrapperTable + 0x4C0) = 0;
		// For CZiprail this precalc is built from THIS wrapper's own tree
		// post-SendTable_Init by DTExtend_ZiprailBuildOwnPrecalc (the wrapper is
		// unreachable from tables, so the engine never builds one here).
		*(uintptr_t*)(slot.wrapperTable + 0x508) = 0;
		// Factory m_pSendTable points at slot.wrapperTable, but DeepClone fixed
		// parentTable to the pool copy (clonedWrapper). Re-own root props onto
		// the factory-facing table so encoder class indices land in THIS tree.
		DTExtend_FixParentTablesInTree(slot.wrapperTable);
		clonedAny = true;

		if (!def.propNElementsOverride)
		{
			Warning(eDLL_T::ENGINE,
				"[dt_extend] %s: deep-cloned wrapper (pool %zu/%zu)\n",
				def.className, slot.treePool.used, CLONE_POOL_SIZE);
			continue;
		}
		if (!bridge_pe_expanded.GetBool())
			continue;

		// Find the sub-table holding the 5 SNDC Array props.
		// PE's wrapper has 11 props (1 DataTable to parent base + 5 pairs of inline-template + Array).
		uint8_t* wProps = *(uint8_t**)(slot.wrapperTable + ST_PROPS);
		uintptr_t subTable = 0;
		{
			constexpr int kMaxQueue = 32;
			uintptr_t queue[kMaxQueue];
			int qHead = 0, qTail = 0;
			queue[qTail++] = (uintptr_t)slot.wrapperTable;
			while (qHead < qTail && qHead < kMaxQueue)
			{
				uintptr_t tbl = queue[qHead++];
				uint8_t* props = *(uint8_t**)(tbl + ST_PROPS);
				int nProps = *(int*)(tbl + ST_NPROPS);
				if (!props || nProps <= 0) continue;
				// Check if this table has m_bools.
				for (int j = 0; j < nProps; ++j)
				{
					uint8_t* p = props + (uint64_t)j * SP_SIZE;
					if (*(int*)(p + SP_TYPE) == 5)
					{
						const char* n = *(const char**)(p + SP_VARNAME);
						if (n && strcmp(n, "m_bools") == 0) { subTable = tbl; break; }
					}
				}
				if (subTable) break;
				// Enqueue DataTable children.
				for (int j = 0; j < nProps && qTail < kMaxQueue; ++j)
				{
					uint8_t* p = props + (uint64_t)j * SP_SIZE;
					if (*(int*)(p + SP_TYPE) == 10)
					{
						uintptr_t child = *(uintptr_t*)(p + 0x70);
						if (child) queue[qTail++] = child;
					}
				}
			}
		}
		if (!subTable)
		{
			Warning(eDLL_T::ENGINE,
				"[dt_extend] %s: SNDC sub-table (with m_bools Array prop) not found in cloned tree -- skipped\n",
				def.className);
			continue;
		}

		// Patch sub-table name to PE_EXPANDED's DT (S21 name on wire).
		*(const char**)(subTable + ST_NETTABLENAME) = "DT_ScriptNetData_SNDC_PLAYER_EXCLUSIVE_EXPANDED";

		// For each Array prop matching s_peExpandedLayout, patch nElements
		// and the preceding (inline-template) prop's SP_OFFSET to S21.
		if (slot.actualAllocSize == 0)
		{
			Error(eDLL_T::ENGINE, EXIT_FAILURE,
				"[dt_extend] %s: Create alloc not grown -- "
				"refusing PE_EXPANDED nElem against S21 Recv\n",
				def.className);
		}
		uint8_t* subProps = *(uint8_t**)(subTable + ST_PROPS);
		int subNProps = *(int*)(subTable + ST_NPROPS);
		int patched = 0;
		for (int p = 0; p < subNProps && subProps; ++p)
		{
			uint8_t* arrayProp = subProps + (uint64_t)p * SP_SIZE;
			if (*(int*)(arrayProp + SP_TYPE) != 5) continue; // not Array
			const char* propName = *(const char**)(arrayProp + SP_VARNAME);
			if (!propName) continue;

			const PEExpandedArrayInfo* match = nullptr;
			for (int a = 0; a < 5; ++a)
			{
				if (strcmp(propName, s_peExpandedLayout[a].name) == 0)
				{
					match = &s_peExpandedLayout[a];
					break;
				}
			}
			if (!match) continue;

			const int oldNElem = *(int*)(arrayProp + SP_NELEMENTS);
			*(int*)(arrayProp + SP_NELEMENTS) = match->nElements;

			// In this engine, Array SendProps don't store the element template via SP_ARRAYPROP.
			// Instead, the template is the PRECEDING prop in the same props array (e.g. prop[N-1] type=0 name='m_bools' is the template for prop[N] type=5 name='m_bools').
			int templateOff = -1;
			int oldTemplateOff = -1;
			if (p > 0)
			{
				uint8_t* tmplProp = subProps + (uint64_t)(p - 1) * SP_SIZE;
				const char* tmplName = *(const char**)(tmplProp + SP_VARNAME);
				// Sanity: the inline template should share the array's varname.
				if (tmplName && strcmp(tmplName, propName) == 0)
				{
					oldTemplateOff = *(int*)(tmplProp + SP_OFFSET);
					*(int*)(tmplProp + SP_OFFSET) = match->offset;
					templateOff = match->offset;
				}
				else
				{
					Warning(eDLL_T::ENGINE,
						"[dt_extend] %s.%s: prev prop name '%s' != '%s' -- template offset NOT patched\n",
						def.className, match->name,
						tmplName ? tmplName : "<null>", propName);
				}
			}

			(void)oldNElem; (void)oldTemplateOff; (void)templateOff;
			patched++;
		}

		Warning(eDLL_T::ENGINE,
			"[dt_extend] %s: deep-cloned wrapper, %d/%d Array props patched (pool %zu/%zu)\n",
			def.className, patched, 5,
			slot.treePool.used, CLONE_POOL_SIZE);
	}

	if (clonedAny)
		DTExtend_RebuildSendTableCache(tables, count,
			"after S21 ServerClass deep-clone");
}

// S3 networks path arrays natively; no flatten exclusion.
// Flag 0x100 only keeps the array OFF the precalc's PROXIED datatable list; pack resolves element base as entityBase+accumOffset (+0x4FC).

// [ZIPRAIL-ISO] isolation suite retired with the array-flatten wall. Do not
// re-add firehose dumps without a fresh need.

// The synthetic CZiprail wrapper (DT_Ziprail) is only referenced by its entity factory, never by tables, so the engine's SendTable_Init precalc build never reaches it -- its +0x4C0 precalc is cleared at clone time and stays NULL, so the encoder sees zero flat props (header-only entities + an empty class instancebaseline, and the S21 client correctly never instantiates it).
// The previous fix ADOPTED the parent CZipline's just-built precalc into the wrapper's +0x4C0.
void DTExtend_ZiprailBuildOwnPrecalc(void)
{
	if (s_ziprailSlotIndex < 0 || !s_s21Slots)
		return;

	uint8_t* slotWrapper = s_s21Slots[s_ziprailSlotIndex].wrapperTable;
	if (!slotWrapper)
	{
		Warning(eDLL_T::ENGINE, "[ZIPRAIL-PACK] build: no wrapper table\n");
		return;
	}

	if (!v_SendTable_BuildPrecalc)
	{
		Warning(eDLL_T::ENGINE,
			"[ZIPRAIL-PACK] build: v_SendTable_BuildPrecalc unresolved -- cannot build precalc\n");
		return;
	}

	const uintptr_t wpc = *(uintptr_t*)(slotWrapper + 0x4C0); // SendTable precalc
	if (wpc != 0)
	{
		// The engine already built the wrapper a precalc through its own (lazy) path, and that precalc is CORRECT: flatN=206 INCLUDING the 32 zipline array element leaves (flat[97..128] on the native layout; elements are named '[0000]'..'[0015]', not after the parent array -- see the RETRACTION above).
		// Leave it completely alone: the old re-flatten + 0x100 un-exclude here rebuilt an already-correct precalc and mutated the wire-visible prop flags for zero gain.
		static bool s_loggedExisting = false;
		if (!s_loggedExisting)
		{
			s_loggedExisting = true;
			__try
			{
				Msg(eDLL_T::ENGINE,
					"[ZIPRAIL-PACK] wrapper precalc already present (0x%p flatN=%d) -- left untouched\n",
					(void*)wpc, *(int*)(wpc + 0x10));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		return;
	}

	// Same allocator the engine's SendTable_Init loop uses for the precalc object
	// (16496 bytes = sizeof(CSendTablePrecalc)); it is never freed (the engine
	// leaks precalcs across changelevel too), so no engine-owned free path exists.
	IMemAlloc* pAlloc = g_pMemAllocSingleton;
	if (!pAlloc) pAlloc = CreateGlobalMemAlloc();
	if (!pAlloc)
	{
		Warning(eDLL_T::ENGINE, "[ZIPRAIL-PACK] build: no allocator -- skipped\n");
		return;
	}

	constexpr size_t kPrecalcSize = 16496; // sizeof(CSendTablePrecalc), from SendTable_Init
	uint8_t* precalc = reinterpret_cast<uint8_t*>(pAlloc->Alloc(kPrecalcSize));
	if (!precalc)
	{
		Warning(eDLL_T::ENGINE, "[ZIPRAIL-PACK] build: precalc alloc failed -- skipped\n");
		return;
	}
	memset(precalc, 0, kPrecalcSize);

	__try
	{
		// The CSendTablePrecalc vftable ptr lives at precalc+0x00.
		// Read it from the parent CZipline wrapper's own (already-built) precalc so we need no extra symbol resolve; if unavailable, leave it null (the encoder reads the flat arrays, not the vftable, when packing).
		uintptr_t parentWrapper = 0;
		if (g_pFactoryListHead && *g_pFactoryListHead)
		{
			uintptr_t node = *g_pFactoryListHead;
			for (int guard = 0; node && guard < 4096; ++guard)
			{
				const char* cn = *(const char**)(node + FACT_CLASSNAME);
				if (cn && strcmp(cn, s_s21Classes[s_ziprailSlotIndex].parentDT) == 0)
				{
					parentWrapper = *(uintptr_t*)(node + 0x08);
					break;
				}
				node = *(uintptr_t*)(node + FACT_NEXT);
			}
		}
		if (parentWrapper)
		{
			const uintptr_t ppc = *(uintptr_t*)(parentWrapper + 0x4C0);
			if (ppc)
				*(uintptr_t*)(precalc + 0x00) = *(uintptr_t*)ppc; // CSendTablePrecalc vftable
		}

		*(uintptr_t*)(precalc + 0x48) = (uintptr_t)slotWrapper; // precalc -> table back-link
		*(uintptr_t*)(slotWrapper + 0x4C0) = (uintptr_t)precalc; // table -> precalc
		*(uintptr_t*)(slotWrapper + 0x508) = 0;                  // list next: clean terminator

		// Native flatten: builds precalc+0x08 (flat SendProp**) / +0x10 (count) and
		// the datatable-props/proxy stack from the wrapper's OWN tree.
		v_SendTable_BuildPrecalc(precalc, 1 /*bServerSide*/);

		const uintptr_t flatArr = *(uintptr_t*)(precalc + 0x08);
		const int       flatN   = *(int*)(precalc + 0x10);

		Warning(eDLL_T::ENGINE,
			"[ZIPRAIL-PACK] built own precalc for CZiprail wrapper: precalc=0x%p flatN=%d flatArr=0x%p -- promoted entities and the class baseline now pack base origin + m_ziplineRestPositions[]\n",
			(void*)precalc, flatN, (void*)flatArr);

		if (flatN <= 0 || flatN > 4096)
		{
			Warning(eDLL_T::ENGINE,
				"[ZIPRAIL-PACK] WARNING: built precalc flatN=%d looks wrong\n",
				flatN);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE,
			"[ZIPRAIL-PACK] build: SEH fault building CZiprail precalc -- wrapper left with %s precalc\n",
			*(uintptr_t*)(slotWrapper + 0x4C0) ? "partial" : "NULL");
	}
}

// Extend DT_WeaponInventory.offhandWeapons from 6 props to 8. verified from [OFFHAND-SUB-DUMP]: the sub-table at depth=1 inside DT_WeaponInventory holds a contiguous 136-byte SendProp array (same SP_SIZE as top-level props).
// Each prop is ty=0 nb=24 fl=0x10001 ne=1 off=4*k name="000k".

// Value proxy for cloned offhandWeapons[6/7]. Reads the server shadow map so
// the encoder never ships the activeWeapons bytes that share those offsets.
// Default pOut to EMPTY; every early exit leaves that sentinel in place.
static void __fastcall OffhandSlotExt_ShadowProxy(void* pProp, void* pStruct,
	void* pData, void* pOut, int /*iElement*/, int objectID)
{
	if (!pOut)
		return;

	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	// S21 RecvProxy_IntToEHandle invalid is 0xFFFFFF, not S3 0xFFFFFFFF.
	*(uint32_t*)pOut = 0x00FFFFFFu;

	int propOff = -1;
	if (pProp)
		propOff = *reinterpret_cast<const int*>(
			reinterpret_cast<const char*>(pProp) + 0x78);

	const int slot = OffhandSlotsExt_SlotFromPropOffset(propOff);
	if (slot < 0)
	{
		static bool s_warnedBadOff = false;
		if (!s_warnedBadOff)
		{
			s_warnedBadOff = true;
			Warning(eDLL_T::ENGINE,
				"[OFFHAND-EXT] ShadowProxy: propOff=%d is not an extended slot\n",
				propOff);
		}
		return;
	}

	void* const pPlayer = OffhandSlotsExt_PlayerFromOffhandArrayBase(pStruct);
	const uintptr_t playerAddr = reinterpret_cast<uintptr_t>(pPlayer);
	if (!pPlayer || (playerAddr & 0xF) != 0)
	{
		static bool s_warnedBadPlayer = false;
		if (!s_warnedBadPlayer)
		{
			s_warnedBadPlayer = true;
			Warning(eDLL_T::ENGINE,
				"[OFFHAND-EXT] ShadowProxy: bad player from pStruct=%p propOff=%d "
				"(resolved=%p)\n",
				pStruct, propOff, pPlayer);
		}
		return;
	}

	const uint32_t eh = OffhandSlotsExt_GetServer(pPlayer, slot);
	// Shadow is an S3 memory EHANDLE. Cloning prop[5] replaced the native
	// SendPropEHandle packer, so we have to emit S21 wire form ourselves.
	const int32_t wire = SDKEntityState_PackS21RecvEHandle(static_cast<int32_t>(eh));
	*(uint32_t*)pOut = static_cast<uint32_t>(wire);

	static volatile LONG s_offhandWireLog = 0;
	if (eh != 0xFFFFFFFFu && eh != 0)
	{
		const LONG n = InterlockedIncrement(&s_offhandWireLog);
		if (n <= 8)
			Msg(eDLL_T::ENGINE,
				"[OFFHAND-EXT] slot=%d s3=0x%X wire=0x%X idx=%d ser=%d (#%d)\n",
				slot, eh, wire, eh & 0xFFFF, (eh >> 16) & 0xFFFF,
				static_cast<int>(n));
	}

	if (OffhandSlotsExt_IsDiagEnabled())
	{
		static std::atomic<int> s_diagCalls{0};
		const int n = s_diagCalls.fetch_add(1, std::memory_order_relaxed);
		if (n < 8)
		{
			Msg(eDLL_T::ENGINE,
				"[OFFHAND-DIAG] call=%d pProp=%p propOff=%d pStruct=%p pData=%p "
				"pPlayer=%p slot=%d eh=0x%08X wire=0x%08X objectID=%d\n",
				n, pProp, propOff, pStruct, pData, pPlayer, slot, eh, wire, objectID);
		}
	}
}

static bool s_offhandExtApplied = false;
void DTExtend_ExtendOffhandWeaponsTo8(void** tables, int count)
{
	if (s_offhandExtApplied) return;
	if (!tables || count <= 0) return;

	OffhandFindCtx ctx = {};
	ctx.targetName = "offhandWeapons";
	for (int t = 0; t < count && !ctx.matchedProp; ++t)
	{
		uint8_t* topTable = reinterpret_cast<uint8_t*>(tables[t]);
		if (!topTable) continue;
		DTExtend_RecursiveOffhandFind(topTable, ctx, 0);
	}

	if (!ctx.matchedProp)
	{
		Warning(eDLL_T::ENGINE,
			"[OFFHAND-EXT] 'offhandWeapons' prop NOT FOUND; sub-table "
			"extension SKIPPED (S21 client offhand[6..7] will stay empty). "
			"Walked %d tables.\n", ctx.visitedN);
		return;
	}

	uint8_t* p = ctx.matchedProp;
	const int type = *reinterpret_cast<int*>(p + SP_TYPE);
	if (type != 10)
	{
		Warning(eDLL_T::ENGINE,
			"[OFFHAND-EXT] unexpected parent prop type %d (want 10 "
			"DPT_DataTable); skipping.\n", type);
		return;
	}
	uint8_t* subTable = *reinterpret_cast<uint8_t**>(p + 0x70);
	if (!subTable || !ODP_IsReadable(subTable, 0x10))
	{
		Warning(eDLL_T::ENGINE,
			"[OFFHAND-EXT] m_pDataTable=%p NOT readable; skipping.\n",
			subTable);
		return;
	}

	uint8_t* oldProps = *reinterpret_cast<uint8_t**>(subTable + ST_PROPS);
	const int  oldN  = *reinterpret_cast<int*>(subTable + ST_NPROPS);
	Msg(eDLL_T::ENGINE, "[OFFHAND-EXT] offhandWeapons nProps=%d\n", oldN);
	if (!oldProps || (oldN != 6 && oldN != 8))
	{
		Warning(eDLL_T::ENGINE,
			"[OFFHAND-EXT] sub-table unexpected shape (props=%p nProps=%d, "
			"want 6 or 8); refusing to mutate.\n", oldProps, oldN);
		return;
	}
	if (oldN == 8)
	{
		if (!ODP_IsReadable(oldProps, static_cast<size_t>(oldN) * SP_SIZE))
		{
			Warning(eDLL_T::ENGINE,
				"[OFFHAND-EXT] oldProps[0..%zu) not fully readable; skipping.\n",
				static_cast<size_t>(oldN) * SP_SIZE);
			return;
		}
		for (int k = 6; k < 8; ++k)
		{
			uint8_t* dst = oldProps + static_cast<uint64_t>(k) * SP_SIZE;
			*reinterpret_cast<uintptr_t*>(dst + 0x60) =
				reinterpret_cast<uintptr_t>(&OffhandSlotExt_ShadowProxy);
		}
		s_offhandExtApplied = true;
		Msg(eDLL_T::ENGINE,
			"[OFFHAND-EXT] native-8 offhandWeapons; installed shadow proxy on props 6/7\n");
		return;
	}
	if (!ODP_IsReadable(oldProps, static_cast<size_t>(oldN) * SP_SIZE))
	{
		Warning(eDLL_T::ENGINE,
			"[OFFHAND-EXT] oldProps[0..%zu) not fully readable; skipping.\n",
			static_cast<size_t>(oldN) * SP_SIZE);
		return;
	}

	const int    newN     = 8;
	const size_t newBytes = static_cast<size_t>(newN) * SP_SIZE;
	uint8_t*     newProps = static_cast<uint8_t*>(malloc(newBytes));
	if (!newProps)
	{
		Warning(eDLL_T::ENGINE,
			"[OFFHAND-EXT] malloc(%zu) FAILED; skipping.\n", newBytes);
		return;
	}

	// Copy existing 6 props verbatim.
	memcpy(newProps, oldProps, static_cast<size_t>(oldN) * SP_SIZE);

	// Clone prop[5] -> [6] and [7]; mutate only SP_OFFSET and SP_VARNAME.
	// All other fields (type/nBits/flags/proxy/etc) stay identical so the wire encoding for elements 6,7 is byte-compatible with 0..5.
	static const char* const k_name0006 = "[0006]";
	static const char* const k_name0007 = "[0007]";

	for (int k = oldN; k < newN; ++k)
	{
		uint8_t* dst = newProps + static_cast<uint64_t>(k) * SP_SIZE;
		uint8_t* src = newProps + static_cast<uint64_t>(oldN - 1) * SP_SIZE;
		memcpy(dst, src, SP_SIZE);
		*reinterpret_cast<int*>(dst + SP_OFFSET) = 4 * k;
		*reinterpret_cast<const char**>(dst + SP_VARNAME) =
			(k == 6) ? k_name0006 : k_name0007;
		// Always install the shadow proxy: feature-on ships the shadow;
		// feature-off leaves shadow empty and ships EMPTY (never activeWeapons).
		*reinterpret_cast<uintptr_t*>(dst + 0x60) =
			reinterpret_cast<uintptr_t>(&OffhandSlotExt_ShadowProxy);
	}

	*reinterpret_cast<uint8_t**>(subTable + ST_PROPS) = newProps;
	*reinterpret_cast<int*>(subTable + ST_NPROPS) = newN;
	s_offhandExtApplied = true;

	Msg(eDLL_T::ENGINE,
		"[OFFHAND-EXT] PATCHED DT_WeaponInventory.offhandWeapons sub-table "
		"6 -> 8 props (newProps=%p, table=%p, parent=%p). New entries: "
		"off=0x%X nm='[0006]', off=0x%X nm='[0007]'. Wire ships slots 6/7 "
		"from the OffhandSlotsExt shadow map via ShadowProxy.\n",
		newProps, subTable, ctx.parentTable, 4 * 6, 4 * 7);
}

static bool s_deathFieldArraysBuilt = false;
// Per-field child SendProp arrays kept after the builder so they can be
// re-installed on the engine's name-bound sub-tables post-SendTable_Init.
static uint8_t* s_dfChildren[DF_FIELD_COUNT] = {};
static int      s_dfChildrenN = 0;

void DTExtend_BuildDeathFieldArrays(void** tables, int count)
{
	if (s_deathFieldArraysBuilt) return;

	// Loud gate (no more silent return): report the launch-intent decision so the
	// log proves whether the build was requested and why.
	const char* cmdval = nullptr;
	const bool onCmdline = CommandLine()->CheckParm("+sdk_deathfield_native_dt", &cmdval) != 0;
	const bool requested = DeathField_NativeDTRequestedAtLaunch();
	Msg(eDLL_T::ENGINE,
		"[DEATHFIELD-DT] gate: requested=%d (convar.GetBool=%d cmdline=%d val=%s) ring_count=%d\n",
		requested, sdk_deathfield_native_dt.GetBool(), onCmdline,
		cmdval ? cmdval : "(null)", DeathField_RingCountAtLaunch());

	if (!requested) return;
	if (!tables || count <= 0) return;
	s_deathFieldArraysBuilt = true;

	// Self-sufficient discovery + per-type template capture (CANON's pass is
	// gated off, so it may not have populated these). Idempotent + read-only.
	if (s_canonS3IndexCount == 0)
		for (int i = 0; i < count; ++i)
			CanonDiscoverTable((uintptr_t)tables[i], 0);
	for (int i = 0; i < s_canonS3IndexCount; ++i)
	{
		uint8_t* props = *(uint8_t**)(s_canonS3Index[i].table + ST_PROPS);
		const int n    = *(int*)(s_canonS3Index[i].table + ST_NPROPS);
		for (int j = 0; j < n && props; ++j)
		{
			const uint8_t* p = props + (uint64_t)j * SP_SIZE;
			const int ty = *(const int*)(p + SP_TYPE);
			if ((unsigned)ty < 16 && !s_canonTmpl[ty] &&
				!(*(const int*)(p + SP_FLAGS) & 0x40 /* SPROP_EXCLUDE */))
				s_canonTmpl[ty] = p;
		}
	}
	if (s_canonS3IndexCount == 0 || !s_canonTmpl[0] || !s_canonTmpl[1] ||
		!s_canonTmpl[2] || !s_canonTmpl[10])
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-DT] missing templates/index (idx=%d Int=%p Float=%p "
			"Vec=%p DT=%p) -- ABORT\n", s_canonS3IndexCount, (void*)s_canonTmpl[0],
			(void*)s_canonTmpl[1], (void*)s_canonTmpl[2], (void*)s_canonTmpl[10]);
		return;
	}

	const uintptr_t world = CanonS3TableFind("DT_WORLD");
	if (!world)
	{
		Warning(eDLL_T::ENGINE, "[DEATHFIELD-DT] DT_WORLD not found -- ABORT\n");
		return;
	}

	struct DFField { const char* name; int type; };
	const DFField fields[6] = {
		{ "m_deathFieldIsActive",    0 },
		{ "m_deathFieldOrigin",      2 },
		{ "m_deathFieldRadiusStart", 1 },
		{ "m_deathFieldRadiusEnd",   1 },
		{ "m_deathFieldTimeStart",   1 },
		{ "m_deathFieldTimeEnd",     1 },
	};
	int N = DeathField_RingCountAtLaunch();   // launch-tunable; 64 overflows snapshot
	if (N < 1)  N = 1;
	if (N > 64) N = 64;
	const uintptr_t cloneBase = s_canonS3Index[0].table;
	uint8_t*  wprops          = *(uint8_t**)(world + ST_PROPS);
	const int wn              = *(int*)(world + ST_NPROPS);
	int built = 0;

	// fields order MUST match the DF_FieldId enum (DF_ISACTIVE..DF_TIMEEND) so
	// fi indexes DF_LAYOUT for this field's disjoint child-offset block.
	static_assert(DF_FIELD_COUNT == 6, "DF_LAYOUT must cover all 6 deathfield arrays");
	for (int fi = 0; fi < DF_FIELD_COUNT; ++fi)
	{
		const DFField& f = fields[fi];
		uint8_t* parent = nullptr;
		for (int j = 0; j < wn && wprops; ++j)
		{
			uint8_t* p = wprops + (uint64_t)j * SP_SIZE;
			const char* nm = *(const char**)(p + SP_VARNAME);
			if (nm && strcmp(nm, f.name) == 0) { parent = p; break; }
		}
		if (!parent)
		{
			Warning(eDLL_T::ENGINE, "[DEATHFIELD-DT] '%s' not on DT_WORLD -- skip\n", f.name);
			continue;
		}

		// Capture before the parent prop is overwritten by the type-10 flip.
		// SendTable_Init rebinds DataTable children BY TABLE NAME to the registered
		// object of that name; mutating that object in place keeps our children.
		const int parentTypeBefore = *(int*)(parent + SP_TYPE);
		uint8_t* const existingSub = *(uint8_t**)(parent + 0x70);
		const bool reuseSub = (parentTypeBefore == 10 && existingSub != nullptr);

		// 64 element children: clone the type template, install a per-ring
		// value proxy, bracketed [000k] name so the S21 name-matcher pairs them.
		uint8_t* children = static_cast<uint8_t*>(malloc((size_t)N * SP_SIZE));
		if (!children) continue;
		bool fieldOk = true;
		for (int k = 0; k < N; ++k)
		{
			void* const proxy = DeathField_GetRingProxy(fi, k);
			if (!proxy)
			{
				Warning(eDLL_T::ENGINE,
					"[DEATHFIELD-DT] no proxy for field %d ring %d -- skip\n", fi, k);
				fieldOk = false;
				break;
			}
			uint8_t* c = children + (uint64_t)k * SP_SIZE;
			memcpy(c, s_canonTmpl[f.type], SP_SIZE);
			char* nm = static_cast<char*>(malloc(8));
			if (nm)
			{
				nm[0] = '['; nm[1] = '0' + (k / 1000) % 10; nm[2] = '0' + (k / 100) % 10;
				nm[3] = '0' + (k / 10) % 10; nm[4] = '0' + k % 10; nm[5] = ']'; nm[6] = 0;
				*(const char**)(c + SP_VARNAME) = nm;
			}
			// Disjoint absolute child offset. Parent offset is 0 so flatten leaves
			// keep non-colliding changeframe slots; ring identity is the proxy.
			*(int*)(c + SP_OFFSET)    = DF_LAYOUT[fi].firstChild + k * DF_LAYOUT[fi].stride;
			*(int*)(c + SP_NELEMENTS) = 1;
			*(uintptr_t*)(c + 0x60)   = (uintptr_t)proxy;
			*(uintptr_t*)(c + 0x70)   = 0;                       // scalar child, no sub-table
		}
		if (!fieldOk)
		{
			free(children);
			continue;
		}

		uint8_t* sub = nullptr;
		if (reuseSub)
		{
			sub = existingSub;
			*(uint8_t**)(sub + ST_PROPS) = children;
			*(int*)(sub + ST_NPROPS)     = N;
			*(uintptr_t*)(sub + 0x4C0)   = 0;   // precalc (rebuilt by SendTable_Init)
			*(uintptr_t*)(sub + 0x508)   = 0;
		}
		else
		{
			// No registered table of this name yet -- allocate and clone a real
			// S3 SendTable so engine-internal fields stay valid (CANON precedent).
			sub = static_cast<uint8_t*>(malloc(NR_SENDTABLE_SIZE));
			if (!sub) { free(children); continue; }
			memcpy(sub, (void*)cloneBase, NR_SENDTABLE_SIZE);
			*(uint8_t**)(sub + ST_PROPS)           = children;
			*(int*)(sub + ST_NPROPS)               = N;
			*(const char**)(sub + ST_NETTABLENAME) = f.name;
			*(uintptr_t*)(sub + 0x4C0)             = 0;   // precalc (rebuilt by SendTable_Init)
			*(uintptr_t*)(sub + 0x508)             = 0;
		}

		Msg(eDLL_T::ENGINE,
			"[DEATHFIELD-DT] '%s' sub=%p reused=%d childrenN=%d\n",
			f.name, sub, reuseSub ? 1 : 0, N);

		// Flip parent scalar -> type-10 DataTable (clone type-10 template for a
		// valid DataTable proxy, then set identity + m_pDataTable).
		memcpy(parent, s_canonTmpl[10], SP_SIZE);
		*(int*)(parent + SP_TYPE)            = 10;
		*(const char**)(parent + SP_VARNAME) = f.name;
		*(int*)(parent + SP_NELEMENTS)       = 1;
		*(int*)(parent + SP_OFFSET)          = 0;   // children carry the full disjoint offset
		*(uintptr_t*)(parent + 0x70)         = (uintptr_t)sub;
		s_dfChildren[fi] = children;
		s_dfChildrenN    = N;
		++built;
	}

	Msg(eDLL_T::ENGINE,
		"[DEATHFIELD-DT] built %d/6 m_deathField* DataTable[%d] arrays on DT_WORLD "
		"(world=%p cloneBase=%p; child offsets disjoint in [%d,%d))\n",
		built, N, (void*)world, (void*)cloneBase,
		DF_CHILD_BASE, DF_LAYOUT[DF_TIMEEND].firstChild + N * DF_LAYOUT[DF_TIMEEND].stride);
}

// SendTable_Init rebinds DataTable children BY NAME, discarding our malloc'd
// sub-tables. Re-point the engine's bound sub at our children and rebuild the
// DT_WORLD precalc so the flat array the encoder walks includes them.
void DTExtend_DeathFieldReinstallPostInit(void)
{
	if (s_dfChildrenN <= 0)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-REINSTALL] disabled -- no children arrays from builder\n");
		return;
	}
	if (!v_SendTable_BuildPrecalc)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-REINSTALL] disabled -- v_SendTable_BuildPrecalc unresolved\n");
		return;
	}

	const uintptr_t tableU = CanonS3TableFind("DT_WORLD");
	if (!tableU)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-REINSTALL] disabled -- DT_WORLD not found\n");
		return;
	}
	uint8_t* const table = reinterpret_cast<uint8_t*>(tableU);

	static const char* const s_dfNames[DF_FIELD_COUNT] = {
		"m_deathFieldIsActive",
		"m_deathFieldOrigin",
		"m_deathFieldRadiusStart",
		"m_deathFieldRadiusEnd",
		"m_deathFieldTimeStart",
		"m_deathFieldTimeEnd",
	};

	int installed = 0;
	__try
	{
		uint8_t* const wprops = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int wn = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (wprops && wn > 0 && wn <= 4096)
		{
			for (int fi = 0; fi < DF_FIELD_COUNT; ++fi)
			{
				if (!s_dfChildren[fi])
					continue;
				for (int j = 0; j < wn; ++j)
				{
					uint8_t* const prop = wprops + static_cast<size_t>(j) * SP_SIZE;
					const char* const nm = *reinterpret_cast<const char* const*>(prop + SP_VARNAME);
					if (!nm || strcmp(nm, s_dfNames[fi]) != 0)
						continue;
					if (*reinterpret_cast<int*>(prop + SP_TYPE) != 10)
						break;
					uint8_t* const sub = *reinterpret_cast<uint8_t**>(prop + 0x70);
					if (!sub)
						break;
					*reinterpret_cast<uint8_t**>(sub + ST_PROPS) = s_dfChildren[fi];
					*reinterpret_cast<int*>(sub + ST_NPROPS)     = s_dfChildrenN;
					*reinterpret_cast<uintptr_t*>(sub + 0x4C0)   = 0;
					*reinterpret_cast<uintptr_t*>(sub + 0x508)   = 0;
					++installed;
					break;
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-REINSTALL] exception -- DT_WORLD precalc left as the engine built it\n");
		return;
	}

	uintptr_t oldPrecalc = 0;
	__try { oldPrecalc = *reinterpret_cast<uintptr_t*>(table + 0x4C0); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-REINSTALL] exception -- DT_WORLD precalc left as the engine built it\n");
		return;
	}
	if (!oldPrecalc)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-REINSTALL] installed=%d/6 childrenN=%d oldPrecalc=(null) -- engine precalc absent, children left in place\n",
			installed, s_dfChildrenN);
		return;
	}

	IMemAlloc* pAlloc = g_pMemAllocSingleton;
	if (!pAlloc)
		pAlloc = CreateGlobalMemAlloc();
	if (!pAlloc)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-REINSTALL] disabled -- no allocator\n");
		return;
	}

	constexpr size_t kPrecalcSize = 16496;
	uint8_t* const newPrecalc = reinterpret_cast<uint8_t*>(pAlloc->Alloc(kPrecalcSize));
	if (!newPrecalc)
	{
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-REINSTALL] disabled -- precalc alloc failed\n");
		return;
	}
	memset(newPrecalc, 0, kPrecalcSize);

	int newFlatN = -1;
	__try
	{
		*reinterpret_cast<uintptr_t*>(newPrecalc + 0x00) =
			*reinterpret_cast<uintptr_t*>(oldPrecalc + 0x00);
		*reinterpret_cast<uintptr_t*>(newPrecalc + 0x48) = reinterpret_cast<uintptr_t>(table);
		v_SendTable_BuildPrecalc(newPrecalc, 1);
		*reinterpret_cast<uintptr_t*>(table + 0x4C0) = reinterpret_cast<uintptr_t>(newPrecalc);
		newFlatN = *reinterpret_cast<int*>(newPrecalc + 0x10);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*reinterpret_cast<uintptr_t*>(table + 0x4C0) = oldPrecalc;
		Warning(eDLL_T::ENGINE,
			"[DEATHFIELD-REINSTALL] exception -- DT_WORLD precalc left as the engine built it\n");
		return;
	}

	Warning(eDLL_T::ENGINE,
		"[DEATHFIELD-REINSTALL] installed=%d/6 childrenN=%d oldPrecalc=%p newPrecalc=%p newFlatN=%d\n",
		installed, s_dfChildrenN,
		reinterpret_cast<void*>(oldPrecalc),
		reinterpret_cast<void*>(newPrecalc),
		newFlatN);
}

// ---------------------------------------------------------------------------
// m_playerMiscData: N x DT_NonRewindMiscData under DT_GlobalNonRewinding.
// Leaf proxies read the slot-indexed store; element index is stashed in
// SP_OFFSET (iElement is 0 for every element on this encode path).
// ---------------------------------------------------------------------------
static const char* const kNonRewindMiscElemNames[128] = {
	"[0000]", "[0001]", "[0002]", "[0003]", "[0004]", "[0005]", "[0006]", "[0007]",
	"[0008]", "[0009]", "[0010]", "[0011]", "[0012]", "[0013]", "[0014]", "[0015]",
	"[0016]", "[0017]", "[0018]", "[0019]", "[0020]", "[0021]", "[0022]", "[0023]",
	"[0024]", "[0025]", "[0026]", "[0027]", "[0028]", "[0029]", "[0030]", "[0031]",
	"[0032]", "[0033]", "[0034]", "[0035]", "[0036]", "[0037]", "[0038]", "[0039]",
	"[0040]", "[0041]", "[0042]", "[0043]", "[0044]", "[0045]", "[0046]", "[0047]",
	"[0048]", "[0049]", "[0050]", "[0051]", "[0052]", "[0053]", "[0054]", "[0055]",
	"[0056]", "[0057]", "[0058]", "[0059]", "[0060]", "[0061]", "[0062]", "[0063]",
	"[0064]", "[0065]", "[0066]", "[0067]", "[0068]", "[0069]", "[0070]", "[0071]",
	"[0072]", "[0073]", "[0074]", "[0075]", "[0076]", "[0077]", "[0078]", "[0079]",
	"[0080]", "[0081]", "[0082]", "[0083]", "[0084]", "[0085]", "[0086]", "[0087]",
	"[0088]", "[0089]", "[0090]", "[0091]", "[0092]", "[0093]", "[0094]", "[0095]",
	"[0096]", "[0097]", "[0098]", "[0099]", "[0100]", "[0101]", "[0102]", "[0103]",
	"[0104]", "[0105]", "[0106]", "[0107]", "[0108]", "[0109]", "[0110]", "[0111]",
	"[0112]", "[0113]", "[0114]", "[0115]", "[0116]", "[0117]", "[0118]", "[0119]",
	"[0120]", "[0121]", "[0122]", "[0123]", "[0124]", "[0125]", "[0126]", "[0127]",
};

static void __fastcall NonRewindMisc_RespawnTimeProxy(void* pProp, void* /*pStruct*/,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pProp) return;

	const int slot = *(const int*)((const uint8_t*)pProp + SP_OFFSET);
	float val = 0.0f;
	if (!GlobalNonRewind_GetSlotMisc(slot, &val, nullptr))
		return;
	*reinterpret_cast<float*>(pOut) = val;
}

static void __fastcall NonRewindMisc_MusicPackProxy(void* pProp, void* /*pStruct*/,
	void* /*pData*/, void* pOut, int /*iElement*/, int /*objectID*/)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;
	if (!pProp) return;

	const int slot = *(const int*)((const uint8_t*)pProp + SP_OFFSET);
	int val = 0;
	if (!GlobalNonRewind_GetSlotMisc(slot, nullptr, &val))
		return;
	*reinterpret_cast<int*>(pOut) = val;
}

static bool s_nonRewindMiscArrayBuilt = false;
static uint8_t* s_nrMiscElemTables[128] = {};
static int s_nrMiscElemN = 0;

void DTExtend_BuildNonRewindMiscArray(void** tables, int count)
{
	if (s_nonRewindMiscArrayBuilt) return;

	const char* cmdval = nullptr;
	const bool onCmdline = CommandLine()->CheckParm("+sdk_nonrewind_misc_dt", &cmdval) != 0;
	const bool requested = NonRewindMisc_DTRequestedAtLaunch();
	int N = NonRewindMisc_CountAtLaunch();
	if (N < 1)   N = 1;
	if (N > 128) N = 128;

	Msg(eDLL_T::ENGINE,
		"[NRMISC-DT] gate: requested=%d (convar.GetBool=%d cmdline=%d val=%s) count=%d\n",
		requested, sdk_nonrewind_misc_dt.GetBool(), onCmdline,
		cmdval ? cmdval : "(null)", N);

	// Gate off: return before any mutation so the wire is byte-identical.
	if (!requested) return;
	if (!tables || count <= 0)
	{
		Warning(eDLL_T::ENGINE, "[NRMISC-DT] no SendTables -- ABORT\n");
		return;
	}

	if (s_canonS3IndexCount == 0)
		for (int i = 0; i < count; ++i)
			CanonDiscoverTable((uintptr_t)tables[i], 0);
	for (int i = 0; i < s_canonS3IndexCount; ++i)
	{
		uint8_t* props = *(uint8_t**)(s_canonS3Index[i].table + ST_PROPS);
		const int n    = *(int*)(s_canonS3Index[i].table + ST_NPROPS);
		for (int j = 0; j < n && props; ++j)
		{
			const uint8_t* p = props + (uint64_t)j * SP_SIZE;
			const int ty = *(const int*)(p + SP_TYPE);
			if ((unsigned)ty < 16 && !s_canonTmpl[ty] &&
				!(*(const int*)(p + SP_FLAGS) & 0x40 /* SPROP_EXCLUDE */))
				s_canonTmpl[ty] = p;
		}
	}
	if (s_canonS3IndexCount == 0 || !s_canonTmpl[0] || !s_canonTmpl[1] || !s_canonTmpl[10])
	{
		Warning(eDLL_T::ENGINE,
			"[NRMISC-DT] missing templates/index (idx=%d Int=%p Float=%p DT=%p) -- ABORT\n",
			s_canonS3IndexCount, (void*)s_canonTmpl[0], (void*)s_canonTmpl[1],
			(void*)s_canonTmpl[10]);
		return;
	}

	const uintptr_t gnr = CanonS3TableFind("DT_GlobalNonRewinding");
	if (!gnr)
	{
		Warning(eDLL_T::ENGINE, "[NRMISC-DT] DT_GlobalNonRewinding not found -- ABORT\n");
		return;
	}

	uint8_t* gprops = *(uint8_t**)(gnr + ST_PROPS);
	const int gn    = *(int*)(gnr + ST_NPROPS);
	for (int j = 0; j < gn && gprops; ++j)
	{
		const char* nm = *(const char**)(gprops + (uint64_t)j * SP_SIZE + SP_VARNAME);
		if (nm && strcmp(nm, "m_playerMiscData") == 0)
		{
			Msg(eDLL_T::ENGINE, "[NRMISC-DT] m_playerMiscData already present -- skip\n");
			s_nonRewindMiscArrayBuilt = true;
			return;
		}
	}

	const uintptr_t cloneBase = s_canonS3Index[0].table;

	// Child table: N DataTable props, each pointing at a 2-leaf element sub-table.
	uint8_t* childProps = static_cast<uint8_t*>(malloc((size_t)N * SP_SIZE));
	uint8_t* childTable = static_cast<uint8_t*>(malloc(NR_SENDTABLE_SIZE));
	if (!childProps || !childTable)
	{
		free(childProps);
		free(childTable);
		Warning(eDLL_T::ENGINE, "[NRMISC-DT] child alloc FAILED -- ABORT\n");
		return;
	}
	memcpy(childTable, (void*)cloneBase, NR_SENDTABLE_SIZE);
	*(uint8_t**)(childTable + ST_PROPS)           = childProps;
	*(int*)(childTable + ST_NPROPS)               = N;
	*(const char**)(childTable + ST_NETTABLENAME) = "m_playerMiscData";
	*(uintptr_t*)(childTable + 0x4C0)             = 0;
	*(uintptr_t*)(childTable + 0x508)             = 0;

	for (int e = 0; e < N; ++e)
	{
		uint8_t* leafProps = static_cast<uint8_t*>(malloc(2 * SP_SIZE));
		uint8_t* elemTable = static_cast<uint8_t*>(malloc(NR_SENDTABLE_SIZE));
		if (!leafProps || !elemTable)
		{
			free(leafProps);
			free(elemTable);
			Warning(eDLL_T::ENGINE, "[NRMISC-DT] element %d alloc FAILED -- ABORT\n", e);
			return;
		}
		memcpy(elemTable, (void*)cloneBase, NR_SENDTABLE_SIZE);
		*(uint8_t**)(elemTable + ST_PROPS)           = leafProps;
		*(int*)(elemTable + ST_NPROPS)               = 2;
		*(const char**)(elemTable + ST_NETTABLENAME) = "DT_NonRewindMiscData";
		*(uintptr_t*)(elemTable + 0x4C0)             = 0;
		*(uintptr_t*)(elemTable + 0x508)             = 0;

		// prop 0: float m_nextRespawnTime -- SP_OFFSET stashes slot index
		uint8_t* p0 = leafProps + 0 * SP_SIZE;
		memcpy(p0, s_canonTmpl[1], SP_SIZE);
		*(const char**)(p0 + SP_VARNAME) = "m_nextRespawnTime";
		*(int*)(p0 + SP_OFFSET)          = e;
		*(int*)(p0 + SP_NELEMENTS)       = 1;
		*(uintptr_t*)(p0 + 0x60)         = (uintptr_t)&NonRewindMisc_RespawnTimeProxy;
		*(uintptr_t*)(p0 + 0x70)         = 0;

		// prop 1: int m_musicPackAssigned
		uint8_t* p1 = leafProps + 1 * SP_SIZE;
		memcpy(p1, s_canonTmpl[0], SP_SIZE);
		*(const char**)(p1 + SP_VARNAME) = "m_musicPackAssigned";
		*(int*)(p1 + SP_OFFSET)          = e;
		*(int*)(p1 + SP_NELEMENTS)       = 1;
		*(uintptr_t*)(p1 + 0x60)         = (uintptr_t)&NonRewindMisc_MusicPackProxy;
		*(uintptr_t*)(p1 + 0x70)         = 0;

		// child prop e: DataTable -> elemTable, name [000N]
		uint8_t* ce = childProps + (uint64_t)e * SP_SIZE;
		memcpy(ce, s_canonTmpl[10], SP_SIZE);
		*(const char**)(ce + SP_VARNAME) = kNonRewindMiscElemNames[e];
		*(int*)(ce + SP_TYPE)            = 10;
		*(int*)(ce + SP_OFFSET)          = 0;
		*(int*)(ce + SP_NELEMENTS)       = 1;
		*(uintptr_t*)(ce + 0x70)         = (uintptr_t)elemTable;
		if (e < 128)
			s_nrMiscElemTables[e] = elemTable;
	}
	s_nrMiscElemN = N;

	// Parent prop: DataTable m_playerMiscData on DT_GlobalNonRewinding
	uint8_t* parentProp = static_cast<uint8_t*>(malloc(SP_SIZE));
	if (!parentProp)
	{
		Warning(eDLL_T::ENGINE, "[NRMISC-DT] parent prop alloc FAILED -- ABORT\n");
		return;
	}
	memcpy(parentProp, s_canonTmpl[10], SP_SIZE);
	*(const char**)(parentProp + SP_VARNAME) = "m_playerMiscData";
	*(int*)(parentProp + SP_TYPE)            = 10;
	*(int*)(parentProp + SP_OFFSET)          = 0;
	*(int*)(parentProp + SP_NELEMENTS)       = 1;
	*(uintptr_t*)(parentProp + 0x70)         = (uintptr_t)childTable;

	// Grow DT_GlobalNonRewinding props by one
	uint8_t* newProps = static_cast<uint8_t*>(malloc((size_t)(gn + 1) * SP_SIZE));
	if (!newProps)
	{
		Warning(eDLL_T::ENGINE, "[NRMISC-DT] grow alloc FAILED -- ABORT\n");
		return;
	}
	if (gprops && gn > 0)
		memcpy(newProps, gprops, (size_t)gn * SP_SIZE);
	memcpy(newProps + (uint64_t)gn * SP_SIZE, parentProp, SP_SIZE);
	*(uint8_t**)(gnr + ST_PROPS) = newProps;
	*(int*)(gnr + ST_NPROPS)     = gn + 1;
	*(uintptr_t*)(gnr + 0x4C0)   = 0;
	*(uintptr_t*)(gnr + 0x508)   = 0;
	free(parentProp);

	// Structural dump: confirm shape matches client RecvTable before trusting.
	uint8_t* e0 = childProps;
	uintptr_t e0tab = e0 ? *(uintptr_t*)(e0 + 0x70) : 0;
	const char* e0tn = e0tab ? *(const char**)(e0tab + ST_NETTABLENAME) : nullptr;
	uint8_t* e0props = e0tab ? *(uint8_t**)(e0tab + ST_PROPS) : nullptr;
	const char* p0n = e0props ? *(const char**)(e0props + 0 * SP_SIZE + SP_VARNAME) : nullptr;
	const char* p1n = e0props ? *(const char**)(e0props + 1 * SP_SIZE + SP_VARNAME) : nullptr;
	const int p0ty = e0props ? *(int*)(e0props + 0 * SP_SIZE + SP_TYPE) : -1;
	const int p1ty = e0props ? *(int*)(e0props + 1 * SP_SIZE + SP_TYPE) : -1;
	Msg(eDLL_T::ENGINE,
		"[NRMISC-BUILD] N=%d parent='m_playerMiscData' elem0 table='%s' "
		"props: ['%s' ty=%d, '%s' ty=%d] gnr nProps=%d\n",
		N, e0tn ? e0tn : "<null>",
		p0n ? p0n : "<null>", p0ty, p1n ? p1n : "<null>", p1ty, gn + 1);
	s_nonRewindMiscArrayBuilt = true;
}

void DTExtend_NonRewindMiscLevelShutdown(void)
{
	s_nonRewindMiscArrayBuilt = false;
}

void DTExtend_NonRewindMiscReinstallPostInit(void)
{
	if (s_nrMiscElemN <= 0)
		return;
	if (!v_SendTable_BuildPrecalc)
	{
		Warning(eDLL_T::ENGINE,
			"[NRMISC-REINSTALL] disabled -- v_SendTable_BuildPrecalc unresolved\n");
		return;
	}

	const uintptr_t tableU = CanonS3TableFind("DT_GlobalNonRewinding");
	if (!tableU)
	{
		Warning(eDLL_T::ENGINE,
			"[NRMISC-REINSTALL] disabled -- DT_GlobalNonRewinding not found\n");
		return;
	}
	uint8_t* const table = reinterpret_cast<uint8_t*>(tableU);

	int installed = 0;
	__try
	{
		uint8_t* const gprops = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int gn = *reinterpret_cast<int*>(table + ST_NPROPS);
		uint8_t* childTable = nullptr;
		if (gprops && gn > 0 && gn <= 4096)
		{
			for (int j = 0; j < gn; ++j)
			{
				uint8_t* const prop = gprops + static_cast<size_t>(j) * SP_SIZE;
				const char* const nm = *reinterpret_cast<const char* const*>(prop + SP_VARNAME);
				if (!nm || strcmp(nm, "m_playerMiscData") != 0)
					continue;
				if (*reinterpret_cast<int*>(prop + SP_TYPE) != 10)
					break;
				childTable = *reinterpret_cast<uint8_t**>(prop + 0x70);
				break;
			}
		}
		if (childTable)
		{
			uint8_t* const cprops = *reinterpret_cast<uint8_t**>(childTable + ST_PROPS);
			const int cn = *reinterpret_cast<int*>(childTable + ST_NPROPS);
			if (cprops && cn > 0 && cn <= 128)
			{
				const int n = (cn < s_nrMiscElemN) ? cn : s_nrMiscElemN;
				for (int e = 0; e < n; ++e)
				{
					if (!s_nrMiscElemTables[e])
						continue;
					uint8_t* const prop = cprops + static_cast<size_t>(e) * SP_SIZE;
					*reinterpret_cast<uint8_t**>(prop + 0x70) = s_nrMiscElemTables[e];
					++installed;
				}
			}
			*reinterpret_cast<uintptr_t*>(childTable + 0x4C0) = 0;
			*reinterpret_cast<uintptr_t*>(childTable + 0x508) = 0;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE,
			"[NRMISC-REINSTALL] exception -- GNR precalc left as the engine built it\n");
		return;
	}

	uintptr_t oldPrecalc = 0;
	__try { oldPrecalc = *reinterpret_cast<uintptr_t*>(table + 0x4C0); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE,
			"[NRMISC-REINSTALL] exception -- GNR precalc left as the engine built it\n");
		return;
	}
	if (!oldPrecalc)
	{
		Warning(eDLL_T::ENGINE,
			"[NRMISC-REINSTALL] installed=%d/%d oldPrecalc=(null) -- engine precalc absent\n",
			installed, s_nrMiscElemN);
		return;
	}

	IMemAlloc* pAlloc = g_pMemAllocSingleton;
	if (!pAlloc)
		pAlloc = CreateGlobalMemAlloc();
	if (!pAlloc)
	{
		Warning(eDLL_T::ENGINE, "[NRMISC-REINSTALL] disabled -- no allocator\n");
		return;
	}

	constexpr size_t kPrecalcSize = 16496;
	uint8_t* const newPrecalc = reinterpret_cast<uint8_t*>(pAlloc->Alloc(kPrecalcSize));
	if (!newPrecalc)
	{
		Warning(eDLL_T::ENGINE, "[NRMISC-REINSTALL] disabled -- precalc alloc failed\n");
		return;
	}
	memset(newPrecalc, 0, kPrecalcSize);

	int newFlatN = -1;
	__try
	{
		*reinterpret_cast<uintptr_t*>(newPrecalc + 0x00) =
			*reinterpret_cast<uintptr_t*>(oldPrecalc + 0x00);
		*reinterpret_cast<uintptr_t*>(newPrecalc + 0x48) = reinterpret_cast<uintptr_t>(table);
		v_SendTable_BuildPrecalc(newPrecalc, 1);
		*reinterpret_cast<uintptr_t*>(table + 0x4C0) = reinterpret_cast<uintptr_t>(newPrecalc);
		newFlatN = *reinterpret_cast<int*>(newPrecalc + 0x10);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*reinterpret_cast<uintptr_t*>(table + 0x4C0) = oldPrecalc;
		Warning(eDLL_T::ENGINE,
			"[NRMISC-REINSTALL] exception -- GNR precalc left as the engine built it\n");
		return;
	}

	Warning(eDLL_T::ENGINE,
		"[NRMISC-REINSTALL] installed=%d/%d oldPrecalc=%p newPrecalc=%p newFlatN=%d\n",
		installed, s_nrMiscElemN,
		reinterpret_cast<void*>(oldPrecalc),
		reinterpret_cast<void*>(newPrecalc),
		newFlatN);
}

// ---------------------------------------------------------------------------
// DT_Player.connectionQualityIndex -- a one-leaf DT_ConnectionQualityIndex nested under the player.
// S21 nests it instead of putting the int on DT_Player directly, so a flat append can never pair: the client resolves a DataTable prop's child BY TABLE NAME over the tables it received.
// ---------------------------------------------------------------------------
static ConVar sdk_conn_quality_dt("sdk_conn_quality_dt", "1", FCVAR_RELEASE,
	"Build DT_Player.connectionQualityIndex -> DT_ConnectionQualityIndex "
	"(m_connectionQualityIndex) from each client's netchan. Default 1. Disable "
	"with +sdk_conn_quality_dt 0.");

static ConVar sdk_conn_quality_interval("sdk_conn_quality_interval", "1.0", FCVAR_RELEASE,
	"Seconds between connection-quality recomputes (min 0.1). The S21 "
	"consumer polls at 1 Hz, so faster only buys snapshot churn.");

static constexpr int kConnQualityMaxSlots = MAX_PLAYERS;
static constexpr int kConnQualityBest     = 0;
static constexpr int kConnQualityWorst    = 5;

struct ConnQualitySlot_t
{
	int       index;
	int       overrideIndex;
	bool      hasOverride;
	float     latencyMs;
	float     lossPct;
	CNetChan* pChan;
};

static ConnQualitySlot_t s_connQuality[kConnQualityMaxSlots] = {};
static bool s_connQualityPrimed = false;
static bool s_connQualityBuilt = false;

// Zero-init would publish 0 (=best) for everyone until the first tick, which is
// the one wrong answer that looks plausible. Prime to worst instead.
static void ConnQuality_Prime()
{
	for (int i = 0; i < kConnQualityMaxSlots; ++i)
	{
		s_connQuality[i].index         = kConnQualityWorst;
		s_connQuality[i].overrideIndex = kConnQualityWorst;
		s_connQuality[i].hasOverride   = false;
		s_connQuality[i].latencyMs     = 0.0f;
		s_connQuality[i].lossPct       = 0.0f;
		s_connQuality[i].pChan         = nullptr;
	}
	s_connQualityPrimed = true;
}

// edictIdx at entity+0x58 (the field MarkEntityEdictDirty uses); slot = idx - 1.
static int ConnQuality_SlotForEntity(const void* pEntity)
{
	if (!pEntity)
		return -1;
	const int16_t edictIdx = *reinterpret_cast<const int16_t*>(
		reinterpret_cast<uintptr_t>(pEntity) + 0x58);
	const int slot = edictIdx - 1;
	if (edictIdx < 1 || slot >= kConnQualityMaxSlots)
		return -1;
	return slot;
}

// The published contract for script. A gamemode wanting a different curve calls
// SetConnectionQualityIndex and owns the value outright.
static int ConnQuality_Classify(float latencyMs, float lossPct)
{
	if (lossPct >= 10.0f || latencyMs >= 300.0f) return 5;
	if (lossPct >=  5.0f || latencyMs >= 200.0f) return 4;
	if (lossPct >=  2.0f || latencyMs >= 120.0f) return 3;
	if (lossPct >=  1.0f || latencyMs >=  80.0f) return 2;
	if (                    latencyMs >=  50.0f) return 1;
	return kConnQualityBest;
}

bool ConnQuality_GetForPlayer(const void* pPlayer, int* outIndex)
{
	if (!s_connQualityPrimed)
		ConnQuality_Prime();
	const int slot = ConnQuality_SlotForEntity(pPlayer);
	if (slot < 0)
		return false;
	const ConnQualitySlot_t& s = s_connQuality[slot];
	if (outIndex)
		*outIndex = s.hasOverride ? s.overrideIndex : s.index;
	return true;
}

bool ConnQuality_GetNetStatsForPlayer(const void* pPlayer, float* outLatencyMs, float* outLossPct)
{
	if (!s_connQualityPrimed)
		ConnQuality_Prime();
	const int slot = ConnQuality_SlotForEntity(pPlayer);
	if (slot < 0)
		return false;
	if (outLatencyMs)
		*outLatencyMs = s_connQuality[slot].latencyMs;
	if (outLossPct)
		*outLossPct = s_connQuality[slot].lossPct;
	return true;
}

void ConnQuality_SetOverrideForPlayer(const void* pPlayer, int index)
{
	if (!s_connQualityPrimed)
		ConnQuality_Prime();
	const int slot = ConnQuality_SlotForEntity(pPlayer);
	if (slot < 0)
		return;
	if (index < kConnQualityBest)  index = kConnQualityBest;
	if (index > kConnQualityWorst) index = kConnQualityWorst;
	s_connQuality[slot].overrideIndex = index;
	s_connQuality[slot].hasOverride   = true;
}

void ConnQuality_ClearOverrideForPlayer(const void* pPlayer)
{
	const int slot = ConnQuality_SlotForEntity(pPlayer);
	if (slot < 0)
		return;
	s_connQuality[slot].hasOverride = false;
}

void ConnQuality_LevelShutdown()
{
	// Data only -- the SendTable build is once-per-process.
	ConnQuality_Prime();
}

void ConnQuality_TickServer(float deltaTime)
{
	if (!s_connQualityBuilt)
		return;
	if (!s_connQualityPrimed)
		ConnQuality_Prime();

	CServer* const pServer = g_pServer;

	for (int i = 0; i < kConnQualityMaxSlots; ++i)
	{
		ConnQualitySlot_t& s = s_connQuality[i];
		CClient* const pClient = pServer ? pServer->GetClient(i) : nullptr;
		CNetChan* const pChan = (pClient && pClient->IsHumanPlayer())
			? pClient->GetNetChan() : nullptr;

		if (!pChan)
		{
			s.latencyMs     = 0.0f;
			s.lossPct       = 0.0f;
			s.index         = kConnQualityWorst;
			s.hasOverride   = false;
			s.overrideIndex = kConnQualityWorst;
			s.pChan         = nullptr;
			continue;
		}

		if (s.pChan != pChan)
		{
			s.hasOverride   = false;
			s.overrideIndex = kConnQualityWorst;
			s.index         = kConnQualityWorst;
			s.pChan         = pChan;
		}
	}

	float interval = sdk_conn_quality_interval.GetFloat();
	if (interval < 0.1f)
		interval = 0.1f;

	static float s_accum = 0.0f;
	s_accum += deltaTime;
	if (s_accum < interval)
		return;
	s_accum = 0.0f;

	if (!pServer)
		return;

	for (int i = 0; i < kConnQualityMaxSlots; ++i)
	{
		ConnQualitySlot_t& s = s_connQuality[i];
		CClient* const pClient = pServer->GetClient(i);
		CNetChan* const pChan = (pClient && pClient->IsHumanPlayer())
			? pClient->GetNetChan() : nullptr;
		if (!pChan || s.pChan != pChan)
			continue;

		s.latencyMs = 1000.0f * pChan->GetAvgLatency(FLOW_OUTGOING);
		s.lossPct   =  100.0f * pChan->GetAvgLoss(FLOW_INCOMING);
		s.index     = ConnQuality_Classify(s.latencyMs, s.lossPct);
	}
}

static void __fastcall ConnQuality_IndexProxy(void* /*pProp*/, void* pStruct,
	void* /*pData*/, void* pOut, int /*iElement*/, int objectID)
{
	if (!pOut) return;
	*(uint64_t*)pOut       = 0;
	*((uint64_t*)pOut + 1) = 0;
	*((uint64_t*)pOut + 2) = 0;

	int val = kConnQualityWorst;
	const bool byEntity = ConnQuality_GetForPlayer(pStruct, &val);
	if (!byEntity && objectID >= 1 && objectID <= kConnQualityMaxSlots)
	{
		const ConnQualitySlot_t& s = s_connQuality[objectID - 1];
		val = s.hasOverride ? s.overrideIndex : s.index;
	}
	*reinterpret_cast<int*>(pOut) = val;
}

static bool ConnQuality_DTRequestedAtLaunch()
{
	// Launch arg wins -- SendTable_Init runs before +convar commands execute.
	const char* val = nullptr;
	if (CommandLine()->CheckParm("+sdk_conn_quality_dt", &val))
		return !val || val[0] != '0';
	return sdk_conn_quality_dt.GetBool();
}

void DTExtend_BuildConnectionQualityIndex(void** tables, int count)
{
	if (s_connQualityBuilt) return;

	const bool requested = ConnQuality_DTRequestedAtLaunch();
	if (!requested) return;
	if (!tables || count <= 0)
	{
		Warning(eDLL_T::ENGINE, "[CONNQ-DT] no SendTables -- ABORT\n");
		return;
	}

	if (s_canonS3IndexCount == 0)
		for (int i = 0; i < count; ++i)
			CanonDiscoverTable((uintptr_t)tables[i], 0);
	for (int i = 0; i < s_canonS3IndexCount; ++i)
	{
		uint8_t* props = *(uint8_t**)(s_canonS3Index[i].table + ST_PROPS);
		const int n    = *(int*)(s_canonS3Index[i].table + ST_NPROPS);
		for (int j = 0; j < n && props; ++j)
		{
			const uint8_t* p = props + (uint64_t)j * SP_SIZE;
			const int ty = *(const int*)(p + SP_TYPE);
			if ((unsigned)ty < 16 && !s_canonTmpl[ty] &&
				!(*(const int*)(p + SP_FLAGS) & 0x40 /* SPROP_EXCLUDE */))
				s_canonTmpl[ty] = p;
		}
	}
	if (s_canonS3IndexCount == 0 || !s_canonTmpl[0] || !s_canonTmpl[10])
	{
		Warning(eDLL_T::ENGINE,
			"[CONNQ-DT] missing templates/index (idx=%d Int=%p DT=%p) -- ABORT\n",
			s_canonS3IndexCount, (void*)s_canonTmpl[0], (void*)s_canonTmpl[10]);
		return;
	}

	const uintptr_t player = CanonS3TableFind("DT_Player");
	if (!player)
	{
		Warning(eDLL_T::ENGINE, "[CONNQ-DT] DT_Player not found -- ABORT\n");
		return;
	}

	uint8_t* pprops = *(uint8_t**)(player + ST_PROPS);
	const int pn    = *(int*)(player + ST_NPROPS);
	for (int j = 0; j < pn && pprops; ++j)
	{
		const char* nm = *(const char**)(pprops + (uint64_t)j * SP_SIZE + SP_VARNAME);
		if (nm && strcmp(nm, "connectionQualityIndex") == 0)
			return;
	}

	const uintptr_t cloneBase = s_canonS3Index[0].table;

	uint8_t* leafProps = static_cast<uint8_t*>(malloc(SP_SIZE));
	uint8_t* childTable = static_cast<uint8_t*>(malloc(NR_SENDTABLE_SIZE));
	uint8_t* newProps = static_cast<uint8_t*>(malloc((size_t)(pn + 1) * SP_SIZE));
	if (!leafProps || !childTable || !newProps)
	{
		free(leafProps);
		free(childTable);
		free(newProps);
		Warning(eDLL_T::ENGINE, "[CONNQ-DT] alloc FAILED -- ABORT\n");
		return;
	}

	memcpy(childTable, (void*)cloneBase, NR_SENDTABLE_SIZE);
	*(uint8_t**)(childTable + ST_PROPS)           = leafProps;
	*(int*)(childTable + ST_NPROPS)               = 1;
	*(const char**)(childTable + ST_NETTABLENAME) = "DT_ConnectionQualityIndex";
	*(uintptr_t*)(childTable + 0x4C0)             = 0;
	*(uintptr_t*)(childTable + 0x508)             = 0;

	memcpy(leafProps, s_canonTmpl[0], SP_SIZE);
	*(const char**)(leafProps + SP_VARNAME) = "m_connectionQualityIndex";
	*(int*)(leafProps + SP_OFFSET)          = 0;
	*(int*)(leafProps + SP_NELEMENTS)       = 1;
	*(uintptr_t*)(leafProps + 0x60)         = (uintptr_t)&ConnQuality_IndexProxy;
	*(uintptr_t*)(leafProps + 0x70)         = 0;

	uint8_t* parentProp = newProps + (uint64_t)pn * SP_SIZE;
	if (pprops && pn > 0)
		memcpy(newProps, pprops, (size_t)pn * SP_SIZE);
	memcpy(parentProp, s_canonTmpl[10], SP_SIZE);
	*(const char**)(parentProp + SP_VARNAME) = "connectionQualityIndex";
	*(int*)(parentProp + SP_TYPE)            = 10;
	*(int*)(parentProp + SP_OFFSET)          = 0;
	*(int*)(parentProp + SP_NELEMENTS)       = 1;
	*(uintptr_t*)(parentProp + 0x70)         = (uintptr_t)childTable;

	*(uint8_t**)(player + ST_PROPS) = newProps;
	*(int*)(player + ST_NPROPS)     = pn + 1;
	*(uintptr_t*)(player + 0x4C0)   = 0;
	*(uintptr_t*)(player + 0x508)   = 0;

	s_connQualityBuilt = true;
	ConnQuality_Prime();
}

// S3's sets child names via [k] which holds unbracketed `000N` strings.
// Bridge name-match fails -> client [DEC-FLAT] shows '?_unmatched' for every weapons / offhandWeapons / activeWeapons child today; the bits get consumed by the dummy-skip cave and the value never reaches S21 entity bytes.
struct BracketizeCtx {
	uintptr_t visited[1024];
	int       visitedN;
	int       renamedTotal;
	int       tablesTouched;
};

static bool ODP_IsNumericName(const char* s, int len)
{
	if (len <= 0 || len > 8) return false;
	for (int i = 0; i < len; ++i)
		if (s[i] < '0' || s[i] > '9') return false;
	return true;
}

static void DTExtend_BracketizeRecursive(uint8_t* table, BracketizeCtx& ctx, int depth)
{
	if (!table || depth > 32) return;
	if (ctx.visitedN >= 1024) return;
	if (!ODP_IsReadable(table, 0x4C0 + 8)) return;

	const uintptr_t pt = reinterpret_cast<uintptr_t>(table);
	for (int v = 0; v < ctx.visitedN; ++v)
		if (ctx.visited[v] == pt) return;
	ctx.visited[ctx.visitedN++] = pt;

	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return;
	if (!ODP_IsReadable(props, static_cast<size_t>(nProps) * SP_SIZE)) return;

	int touched = 0;

	// Pass 1: array element template bracketize.
	// S21 native names array element templates as `m_X[0]` while S3 dedi emits a duplicate bare `m_X` immediately before the array DataTable's bare `m_X`.
	for (int j = 0; j + 1 < nProps; ++j)
	{
		uint8_t* p0 = props + static_cast<uint64_t>(j) * SP_SIZE;
		uint8_t* p1 = props + static_cast<uint64_t>(j + 1) * SP_SIZE;
		const int t1 = *reinterpret_cast<int*>(p1 + SP_TYPE);
		if (t1 != 10) continue;

		const int f0 = *reinterpret_cast<int*>(p0 + SP_FLAGS);
		if (f0 & 0x40 /* SPROP_EXCLUDE */) continue;

		const int t0 = *reinterpret_cast<int*>(p0 + SP_TYPE);
		if (t0 == 10) continue; // p0 must not itself be a DataTable

		const char* n0 = *reinterpret_cast<const char**>(p0 + SP_VARNAME);
		const char* n1 = *reinterpret_cast<const char**>(p1 + SP_VARNAME);
		const int   l0 = ODP_StrLenSafe(n0, 64);
		const int   l1 = ODP_StrLenSafe(n1, 64);
		if (l0 <= 0 || l1 <= 0 || l0 != l1) continue;
		if (strncmp(n0, n1, static_cast<size_t>(l0)) != 0) continue;

		// Already bracketized? Skip.
		if (n0[l0 - 1] == ']') continue;

		// Allocate "<n0>[0]" + null. Caller-owned; never freed (matches
		// the numeric-bracketize allocator pattern below).
		const size_t bufLen = static_cast<size_t>(l0) + 4; // name + [0] + null
		char* tmplName = static_cast<char*>(malloc(bufLen));
		if (!tmplName) continue;
		memcpy(tmplName, n0, static_cast<size_t>(l0));
		tmplName[l0]     = '[';
		tmplName[l0 + 1] = '0';
		tmplName[l0 + 2] = ']';
		tmplName[l0 + 3] = '\0';
		*reinterpret_cast<const char**>(p0 + SP_VARNAME) = tmplName;
		++ctx.renamedTotal;
		++touched;
	}

	// Pass 2: numeric-children bracketize (`000N` -> `[000N]`) and recurse.
	for (int j = 0; j < nProps; ++j)
	{
		uint8_t*    p   = props + static_cast<uint64_t>(j) * SP_SIZE;
		const int   ty  = *reinterpret_cast<int*>(p + SP_TYPE);
		const char* nm  = *reinterpret_cast<const char**>(p + SP_VARNAME);
		const int   nLen = ODP_StrLenSafe(nm, 32);

		if (nLen > 0 && ODP_IsNumericName(nm, nLen))
		{
			// Allocate "[NAME]" + null. Caller-owned; never freed.
			const size_t bufLen = static_cast<size_t>(nLen) + 3; // [ + name + ] + null
			char* bracketed = static_cast<char*>(malloc(bufLen));
			if (bracketed)
			{
				bracketed[0] = '[';
				memcpy(bracketed + 1, nm, nLen);
				bracketed[1 + nLen] = ']';
				bracketed[2 + nLen] = '\0';
				*reinterpret_cast<const char**>(p + SP_VARNAME) = bracketed;
				++ctx.renamedTotal;
				++touched;
			}
		}

		// Recurse into DPT_DataTable children even if we renamed this prop.
		if (ty == 10)
		{
			uint8_t* child = *reinterpret_cast<uint8_t**>(p + 0x70);
			if (child) DTExtend_BracketizeRecursive(child, ctx, depth + 1);
		}
	}
	if (touched > 0) ++ctx.tablesTouched;
}

// ============================================================================
// PROP RELOCATION (cross-scope match enabler) The bridge matcher pairs SendProps to RecvProps by NAME within sub-table scope -- it walks both trees in parallel and only matches a wire prop to a recv prop at the SAME tree position.
// When the dedi (S3 SendTable) emits a prop at top-level but the S21 client has the equivalent RecvProp nested inside a DataTable child (or vice versa), the names exist in both trees but the matcher never sees them as candidates.
struct PropRelocation {
	const char* sourceTableName;
	const char* propName;
	const char* destTableName;
};
static const PropRelocation s_propRelocations[] = {
	// m_animSequence: dedi has at DT_BaseAnimating top-level (idx 9, off=4064); S21 schema has it nested in DT_ServerAnimationData (s21_dt_schema.h `s_sch_DT_ServerAnimationData[0]`).
	// Audit pre-relocation: 27 unmatched sites; post-relocation 19 classes now match (CPlayer, CBaseAnimating, CBaseViewModel, CDynamicProp, CNPC_*, etc.).
	{ "DT_BaseAnimating", "m_animSequence", "DT_ServerAnimationData" },
	// They are NOT in DT_BaseCombatCharacter, which is why the prior DT_BaseCombatCharacter source silently skipped -- the props never moved, so the dedi emitted them at DT_Player scope while the S21 client's RecvProps expect them in DT_LocalPlayerExclusive (s21_dt_schema.h lines 3364-3365), so m_iObserverMode failed to replicate.
	// Move them to DT_LocalPlayerExclusive to match the S21 client's layout.
	{ "DT_Player", "m_hObserverTarget", "DT_LocalPlayerExclusive" },
	{ "DT_Player", "m_iObserverMode",   "DT_LocalPlayerExclusive" },
	// c105 trigger-cylinder cohort: dedi natively has these 4 at DT_TriggerCylinderHeavy top-level (offsets 3248/3344/3348/3352, props [1]/[2]/[3]/[4] in the sdk_dt_dump_tree output).
	// S21 expects them at DT_TriggerCylinderNetworked scope (the s_s21Classes synth that DT_TriggerCylinderHeavy now inherits from via the WRAPPER-RDR splice).
	{ "DT_TriggerCylinderHeavy", "m_triggerFilterMask", "DT_TriggerCylinderNetworked" },
	{ "DT_TriggerCylinderHeavy", "m_radius",            "DT_TriggerCylinderNetworked" },
	{ "DT_TriggerCylinderHeavy", "m_aboveHeight",       "DT_TriggerCylinderNetworked" },
	{ "DT_TriggerCylinderHeavy", "m_belowHeight",       "DT_TriggerCylinderNetworked" },
};
static constexpr int kNumPropRelocations =
	sizeof(s_propRelocations) / sizeof(s_propRelocations[0]);

static bool s_propRelocApplied = false;
void DTExtend_RelocateProps()
{
	if (s_propRelocApplied) return;
	s_propRelocApplied = true;

	int moved = 0;
// s_propRelocations RE-ENABLED (propCopies + highlights
	// exonerated). If the m_passives AV returns now, propRelocs IS the producer.
	for (int r = 0; r < kNumPropRelocations; ++r)
	{
		const PropRelocation& reloc = s_propRelocations[r];

		uint8_t* source = DTExtend_FindTableByName(reloc.sourceTableName);
		if (!source)
		{
			Warning(eDLL_T::ENGINE,
				"[PROP-RELOC] source '%s' not found; skipping '%s' -> '%s'\n",
				reloc.sourceTableName, reloc.propName, reloc.destTableName);
			continue;
		}
		const int srcIdx = DTExtend_FindPropIdx(source, reloc.propName);
		if (srcIdx < 0)
		{
			Warning(eDLL_T::ENGINE,
				"[PROP-RELOC] prop '%s' not at top-level of '%s'; skipping\n",
				reloc.propName, reloc.sourceTableName);
			continue;
		}
		// Global ST_NETTABLENAME lookup -- works for both top-level class tables and registered sub-tables (DT_ServerAnimationData / DT_LocalPlayerExclusive / etc.), so the same call resolves intra- parent and cross-table destinations.
		uint8_t* dest = DTExtend_FindTableByName(reloc.destTableName);
		if (!dest)
		{
			Warning(eDLL_T::ENGINE,
				"[PROP-RELOC] dest '%s' not found; skipping '%s' from '%s'\n",
				reloc.destTableName, reloc.propName, reloc.sourceTableName);
			continue;
		}

		uint8_t* srcProps = *reinterpret_cast<uint8_t**>(source + ST_PROPS);
		const int srcN = *reinterpret_cast<int*>(source + ST_NPROPS);
		uint8_t* srcProp = srcProps + static_cast<uint64_t>(srcIdx) * SP_SIZE;

		uint8_t* destProps = *reinterpret_cast<uint8_t**>(dest + ST_PROPS);
		const int destN = *reinterpret_cast<int*>(dest + ST_NPROPS);
		if (destN < 0 || destN > 4096) continue;

		// Append srcProp to dest's props array.
		const int newDestN = destN + 1;
		uint8_t* newDestProps =
			static_cast<uint8_t*>(malloc(static_cast<size_t>(newDestN) * SP_SIZE));
		if (!newDestProps) continue;
		if (destN > 0)
			memcpy(newDestProps, destProps, static_cast<size_t>(destN) * SP_SIZE);
		memcpy(newDestProps + static_cast<uint64_t>(destN) * SP_SIZE,
			srcProp, SP_SIZE);
		*reinterpret_cast<uint8_t**>(dest + ST_PROPS) = newDestProps;
		*reinterpret_cast<int*>(dest + ST_NPROPS) = newDestN;

		const int newSrcN = srcN - 1;
		if (newSrcN > 0)
		{
			uint8_t* newSrcProps =
				static_cast<uint8_t*>(malloc(static_cast<size_t>(newSrcN) * SP_SIZE));
			if (!newSrcProps)
			{
				*reinterpret_cast<uint8_t**>(dest + ST_PROPS) = destProps;
				*reinterpret_cast<int*>(dest + ST_NPROPS) = destN;
				free(newDestProps);
				continue;
			}
			if (srcIdx > 0)
				memcpy(newSrcProps, srcProps,
					static_cast<size_t>(srcIdx) * SP_SIZE);
			if (srcIdx < srcN - 1)
				memcpy(newSrcProps + static_cast<uint64_t>(srcIdx) * SP_SIZE,
					srcProps + static_cast<uint64_t>(srcIdx + 1) * SP_SIZE,
					static_cast<size_t>(srcN - 1 - srcIdx) * SP_SIZE);
			*reinterpret_cast<uint8_t**>(source + ST_PROPS) = newSrcProps;
			*reinterpret_cast<int*>(source + ST_NPROPS) = newSrcN;
		}
		else
		{
			*reinterpret_cast<int*>(source + ST_NPROPS) = 0;
		}

		++moved;
	}

	Msg(eDLL_T::ENGINE,
		"[PROP-RELOC] applied %d/%d relocations\n", moved, kNumPropRelocations);
}

// ============================================================================
// PROP COPY (additive cross-scope match enabler) -- bulk relocation mechanism Same goal as PropRelocation above (make the matcher pair wire props at a destination sub-table scope on the client) but with two differences 1.
// SOURCE is auto-discovered.
struct PropCopy {
	const char* propName;        // prop to copy (auto-source by name)
	const char* destTableName;   // destination sub-table (added to)
};

// Grouped by destination DT, sorted by site count within each group.
// The 3 already-shipped MOVE entries (m_animSequence -> DT_ServerAnimationData, m_hObserverTarget + m_iObserverMode -> DT_LocalPlayerExclusive) are NOT in this list.
static const PropCopy s_propCopies[] = {
	// --- DT_SequenceTransitionerLayer (6 props, 42 sites each -- highest impact)
	{ "m_sequenceTransitionerLayerStartCycle",        "DT_SequenceTransitionerLayer" },
	{ "m_sequenceTransitionerLayerSequence",          "DT_SequenceTransitionerLayer" },
	{ "m_sequenceTransitionerLayerPlaybackRate",      "DT_SequenceTransitionerLayer" },
	{ "m_sequenceTransitionerLayerStartTime",         "DT_SequenceTransitionerLayer" },
	{ "m_sequenceTransitionerLayerActive",            "DT_SequenceTransitionerLayer" },
	{ "m_sequenceTransitionerLayerFadeOutDuration",   "DT_SequenceTransitionerLayer" },
	// --- DT_BaseEntity (32 props) NOTE: m_wantsScopeHighlight is DELIBERATELY NOT here -- it is already added to DT_BaseEntity by the curated EI registry (~line 112).
	// Listing it here too double-registers it, so it flattens TWICE into every BaseEntity-derived class (DynamicProp/ScriptProp/ShieldProp/...).
	{ "m_dissolveEffectEntityHandle",                 "DT_BaseEntity" },
	{ "m_bossPlayer",                                 "DT_BaseEntity" },
	{ "m_nRenderFX",                                  "DT_BaseEntity" },
	{ "m_nRenderMode",                                "DT_BaseEntity" },
	{ "m_clrRender",                                  "DT_BaseEntity" },
	{ "m_clIntensity",                                "DT_BaseEntity" },
	{ "m_bRenderWithViewModels",                      "DT_BaseEntity" },
	{ "m_teamMemberIndex",                            "DT_BaseEntity" },
	{ "m_squadID",                                    "DT_BaseEntity" },
	{ "m_grade",                                      "DT_BaseEntity" },
	{ "m_ignorePredictedTriggerFlags",                "DT_BaseEntity" },
	{ "m_passThroughFlags",                           "DT_BaseEntity" },
	{ "m_passThroughThickness",                       "DT_BaseEntity" },
	{ "m_passThroughDirection",                       "DT_BaseEntity" },
	{ "m_contents",                                   "DT_BaseEntity" },
	{ "m_collideWithOwner",                           "DT_BaseEntity" },
	{ "m_attachmentLerpStartTime",                    "DT_BaseEntity" },
	{ "m_attachmentLerpEndTime",                      "DT_BaseEntity" },
	{ "m_attachmentLerpStartOrigin",                  "DT_BaseEntity" },
	{ "m_attachmentLerpStartAngles",                  "DT_BaseEntity" },
	{ "m_scriptNameIndex",                            "DT_BaseEntity" },
	{ "m_instanceNameIndex",                          "DT_BaseEntity" },
	{ "movetype",                                     "DT_BaseEntity" },
	{ "movecollide",                                  "DT_BaseEntity" },
	{ "m_baseTakeDamage",                             "DT_BaseEntity" },
	{ "m_invulnerableToDamageCount",                  "DT_BaseEntity" },
	{ "m_shieldHealth",                               "DT_BaseEntity" },
	{ "m_shieldHealthMax",                            "DT_BaseEntity" },
	{ "m_bIsSoundCodeControllerValueSet",             "DT_BaseEntity" },
	{ "m_flSoundCodeControllerValue",                 "DT_BaseEntity" },
	{ "m_ignoreParentRotation",                       "DT_BaseEntity" },
	// --- DT_DeathBoxProp (28 props) -- FLAT class on S21; props inlined at top-level.
	// The m_Collision DataTable copy below gives c19 access to DT_CollisionProperty via the FLAT path (`DT_DeathBoxProp -> m_Collision -> DT_CollisionProperty`) because dedi natively places m_Collision inside DT_BaseEntity (whose wrapper c19's FLAT recv tree doesn't traverse).
	{ "m_Collision",                                  "DT_DeathBoxProp" },
	{ "m_hOwnerEntity",                               "DT_DeathBoxProp" },
	{ "m_networkedFlags",                             "DT_DeathBoxProp" },
	{ "moveparent",                                   "DT_DeathBoxProp" },
	{ "m_nModelIndex",                                "DT_DeathBoxProp" },
	{ "m_fEffects",                                   "DT_DeathBoxProp" },
	{ "m_CollisionGroup",                             "DT_DeathBoxProp" },
	{ "m_parentAttachmentModel",                      "DT_DeathBoxProp" },
	{ "m_iName",                                      "DT_DeathBoxProp" },
	{ "m_holdUsePrompt",                              "DT_DeathBoxProp" },
	{ "m_pressUsePrompt",                             "DT_DeathBoxProp" },
	{ "m_iSignifierName",                             "DT_DeathBoxProp" },
	{ "m_visibilityFlags",                            "DT_DeathBoxProp" },
	{ "m_usableType",                                 "DT_DeathBoxProp" },
	{ "m_usablePriority",                             "DT_DeathBoxProp" },
	{ "m_usableDistanceOverride",                     "DT_DeathBoxProp" },
	{ "m_usableFOV",                                  "DT_DeathBoxProp" },
	{ "m_usePromptSize",                              "DT_DeathBoxProp" },
	{ "m_phaseShiftFlags",                            "DT_DeathBoxProp" },
	{ "m_firstChildEntityLink",                       "DT_DeathBoxProp" },
	{ "m_firstParentEntityLink",                      "DT_DeathBoxProp" },
	{ "m_realmsBitMask",                              "DT_DeathBoxProp" },
	{ "m_localAngles",                                "DT_DeathBoxProp" },
	{ "m_cellX",                                      "DT_DeathBoxProp" },
	{ "m_cellY",                                      "DT_DeathBoxProp" },
	{ "m_cellZ",                                      "DT_DeathBoxProp" },
	{ "m_localOrigin",                                "DT_DeathBoxProp" },
	{ "m_iTeamNum",                                   "DT_DeathBoxProp" },
	{ "m_nSkin",                                      "DT_DeathBoxProp" },
	{ "m_bUseHitboxesForRenderBox",                   "DT_DeathBoxProp" },
	{ "m_bAnimateInStaticShadow",                     "DT_DeathBoxProp" },
	{ "m_lifeState",                                  "DT_DeathBoxProp" },
	{ "m_parentAttachment",                           "DT_DeathBoxProp" },
	{ "m_fadeDist",                                   "DT_DeathBoxProp" },
	{ "m_customOwnerName",                            "DT_DeathBoxProp" },
	{ "m_scriptNetData",                              "DT_DeathBoxProp" },
	// --- DT_CollisionProperty (10 props)
	{ "m_triggerBloat",                               "DT_CollisionProperty" },
	{ "m_collisionDetailLevel",                       "DT_CollisionProperty" },
	{ "m_vecSpecifiedSurroundingMins",                "DT_CollisionProperty" },
	{ "m_vecSpecifiedSurroundingMaxs",                "DT_CollisionProperty" },
	{ "m_vecMaxs",                                    "DT_CollisionProperty" },
	{ "m_nSolidType",                                 "DT_CollisionProperty" },
	{ "m_usSolidFlags",                               "DT_CollisionProperty" },
	{ "m_vecMins",                                    "DT_CollisionProperty" },
	{ "m_nSurroundType",                              "DT_CollisionProperty" },
	// --- DT_BaseAnimating (26 props)
	{ "m_animPlaybackRate",                           "DT_BaseAnimating" },
	{ "m_nNewSequenceParity",                         "DT_BaseAnimating" },
	{ "m_syncingWithEntity",                          "DT_BaseAnimating" },
	{ "m_nForceBone",                                 "DT_BaseAnimating" },
	{ "m_vecForce",                                   "DT_BaseAnimating" },
	{ "m_skinMod",                                    "DT_BaseAnimating" },
	{ "m_nBody",                                      "DT_BaseAnimating" },
	{ "m_camoIndex",                                  "DT_BaseAnimating" },
	{ "m_flModelScale",                               "DT_BaseAnimating" },
	{ "m_animModelIndex",                             "DT_BaseAnimating" },
	{ "m_bSequenceFinished",                          "DT_BaseAnimating" },
	{ "m_lockedAnimDeltaYaw",                         "DT_BaseAnimating" },
	{ "m_animFrozen",                                 "DT_BaseAnimating" },
	{ "m_bClientSideRagdoll",                         "DT_BaseAnimating" },
	{ "m_nRagdollImpactFXTableId",                    "DT_BaseAnimating" },
	{ "m_flSkyScaleStartValue",                       "DT_BaseAnimating" },
	{ "m_flSkyScaleEndValue",                         "DT_BaseAnimating" },
	{ "m_flSkyScaleStartTime",                        "DT_BaseAnimating" },
	{ "m_flSkyScaleEndTime",                          "DT_BaseAnimating" },
	{ "m_passDamageToParent",                         "DT_BaseAnimating" },
	{ "m_flEstIkOffset",                              "DT_BaseAnimating" },
	{ "m_animActive",                                 "DT_BaseAnimating" },
	{ "m_animCollisionEnabled",                       "DT_BaseAnimating" },
	{ "m_animPlantingEnabled",                        "DT_BaseAnimating" },
	{ "m_animNetworkFlags",                           "DT_BaseAnimating" },
	{ "m_animRelativeToGroundEnabled",                "DT_BaseAnimating" },
	{ "m_itemFlavorGUID",                             "DT_BaseAnimating" },
	// --- DT_AnimRelativeData (13 props)
	{ "m_animInitialCorrectPos",                      "DT_AnimRelativeData" },
	{ "m_animInitialCorrectRot",                      "DT_AnimRelativeData" },
	{ "m_animInitialPos",                             "DT_AnimRelativeData" },
	{ "m_animInitialVel",                             "DT_AnimRelativeData" },
	{ "m_animInitialRot",                             "DT_AnimRelativeData" },
	{ "m_animEntityToRefOffset",                      "DT_AnimRelativeData" },
	{ "m_animEntityToRefRotation",                    "DT_AnimRelativeData" },
	{ "m_animBlendBeginTime",                         "DT_AnimRelativeData" },
	{ "m_animBlendEndTime",                           "DT_AnimRelativeData" },
	{ "m_animScriptSequence",                         "DT_AnimRelativeData" },
	{ "m_animScriptModel",                            "DT_AnimRelativeData" },
	{ "m_animIgnoreParentRot",                        "DT_AnimRelativeData" },
	{ "m_animMotionMode",                             "DT_AnimRelativeData" },
	// --- DT_ServerAnimationData (2 props -- m_animSequence already in MOVE list)
	{ "m_animStartTime",                              "DT_ServerAnimationData" },
	{ "m_animStartCycle",                             "DT_ServerAnimationData" },
	{ "m_nResetEventsParity",                         "DT_ServerAnimationData" },
	// --- DT_PredictedAnimEventData (5 props)
	{ "m_predictedAnimEventTarget",                   "DT_PredictedAnimEventData" },
	{ "m_predictedAnimEventCount",                    "DT_PredictedAnimEventData" },
	{ "m_predictedAnimEventSequence",                 "DT_PredictedAnimEventData" },
	{ "m_predictedAnimEventModel",                    "DT_PredictedAnimEventData" },
	{ "m_predictedAnimEventsReadyToFireTime",         "DT_PredictedAnimEventData" },
	// --- DT_SequenceTransitioner (1 prop -- count of layers)
	{ "m_sequenceTransitionerLayerCount",             "DT_SequenceTransitioner" },
	// --- DT_PredictableId (1 prop)
	{ "m_PredictableID",                              "DT_PredictableId" },
	// --- DT_BaseToggle / DT_BaseTrigger (5 props)
	{ "m_vecFinalDest",                               "DT_BaseToggle" },
	{ "m_movementType",                               "DT_BaseToggle" },
	{ "m_flMoveTargetTime",                           "DT_BaseToggle" },
	{ "m_bClientSidePredicted",                       "DT_BaseTrigger" },
	{ "m_spawnflags",                                 "DT_BaseTrigger" },
	// --- DT_TriggerCylinderNetworked (4 props) -- converted to MOVE in s_propRelocations above.
	// Leaving as COPY here would either no-source (after MOVE removes them from DT_TriggerCylinderHeavy) or silently skip via the idempotence check.
	{ "m_AnimOverlayCount",                           "DT_OverlayVars" },
	// --- DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND (5 props, c121 only)
	{ "m_bools",                                      "DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND" },
	{ "m_ranges",                                     "DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND" },
	{ "m_int32s",                                     "DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND" },
	{ "m_times",                                      "DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND" },
	{ "m_entities",                                   "DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND" },
};
static constexpr int kNumPropCopies = sizeof(s_propCopies) / sizeof(s_propCopies[0]);

// Auto-discover the first cached SendTable (other than `excludeTable`) that contains a non-EXCLUDE SendProp named `propName`.
// Returns the table pointer + sets `*outIdx` to the prop's index within that table, or nullptr if not found.
static uint8_t* DTExtend_FindAnyNonExcludeSourceWithProp(const char* propName,
	uint8_t* excludeTable, int* outIdx, bool skipExcludeProps = true)
{
	if (outIdx) *outIdx = -1;
	if (!propName) return nullptr;
	for (int t = 0; t < s_cachedSendTableCount; ++t)
	{
		uint8_t* st = reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[t]);
		if (!st || st == excludeTable) continue;
		uint8_t* props = nullptr;
		int nProps = 0;
		__try
		{
			props = *reinterpret_cast<uint8_t**>(st + ST_PROPS);
			nProps = *reinterpret_cast<int*>(st + ST_NPROPS);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!props || nProps <= 0 || nProps > 4096)
			continue;

		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
			__try
			{
				const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
				const int flags = *reinterpret_cast<int*>(p + SP_FLAGS);
				if (nm && strcmp(nm, propName) == 0 &&
					(!skipExcludeProps || !(flags & 0x40)))
				{
					if (outIdx) *outIdx = i;
					return st;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}
	return nullptr;
}

static bool s_propCopyApplied = false;
void DTExtend_CopyProps()
{
	if (s_propCopyApplied) return;
	s_propCopyApplied = true;

	int copied = 0, skipDest = 0, skipNoSrc = 0;
// s_propCopies RE-ENABLED (highlights exonerated).
	for (int r = 0; r < kNumPropCopies; ++r)
	{
		const PropCopy& cp = s_propCopies[r];

		uint8_t* dest = DTExtend_FindTableByName(cp.destTableName);
		if (!dest)
		{
			++skipDest;
			continue;
		}
		int srcIdx = -1;
		// Skip SPROP_EXCLUDE sources when auto-discovering the donor prop.
		uint8_t* source = DTExtend_FindAnyNonExcludeSourceWithProp(cp.propName, dest, &srcIdx);
		if (!source || srcIdx < 0)
		{
			++skipNoSrc;
			continue;
		}

		uint8_t* srcProps = *reinterpret_cast<uint8_t**>(source + ST_PROPS);
		uint8_t* srcProp  = srcProps + static_cast<uint64_t>(srcIdx) * SP_SIZE;

		uint8_t*  destProps = *reinterpret_cast<uint8_t**>(dest + ST_PROPS);
		const int destN     = *reinterpret_cast<int*>(dest + ST_NPROPS);
		if (destN < 0 || destN > 4096) continue;

		// Idempotence: skip if dest already has a prop with this name (could
		// happen if multiple PropCopy entries share a name, or if a future
		// re-run lands).
		if (DTExtend_FindPropIdx(dest, cp.propName) >= 0)
		{
			continue;
		}

		const int newDestN = destN + 1;
		uint8_t* newDestProps =
			static_cast<uint8_t*>(malloc(static_cast<size_t>(newDestN) * SP_SIZE));
		if (!newDestProps) continue;
		if (destN > 0)
			memcpy(newDestProps, destProps, static_cast<size_t>(destN) * SP_SIZE);
		memcpy(newDestProps + static_cast<uint64_t>(destN) * SP_SIZE,
			srcProp, SP_SIZE);
		*reinterpret_cast<uint8_t**>(dest + ST_PROPS) = newDestProps;
		*reinterpret_cast<int*>(dest + ST_NPROPS)     = newDestN;

		// Cheap: capture source name for the log line so we can tell which cached table the prop came from (useful for verifying that the auto-discover picked a sensible source -- e.g. m_iTeamNum coming from DT_BaseEntity rather than some sub-table).
		const char* srcName = "?";
		__try { srcName = *reinterpret_cast<const char**>(source + ST_NETTABLENAME); }
		__except(EXCEPTION_EXECUTE_HANDLER) { srcName = "<AV>"; }

		Msg(eDLL_T::ENGINE,
			"[PROP-COPY] '%s' copied '%s'[%d] -> '%s' (now %d props)\n",
			cp.propName, srcName ? srcName : "<NULL>", srcIdx,
			cp.destTableName, newDestN);
		++copied;
	}

	Msg(eDLL_T::ENGINE,
		"[PROP-COPY] applied %d/%d copies (skipped: %d no-dest, %d no-source)\n",
		copied, kNumPropCopies, skipDest, skipNoSrc);
}

// ============================================================================
// WRAPPER TABLE INSERTION (chain-mismatch fixer) When S21 added an inheritance level the dedi's S3 SendTable chain doesn't have (e.g.
// CPlayerDecoy on S21 inherits via DT_BaseAnimatingOverlay between DT_PlayerDecoy and DT_BaseAnimating, but the dedi's CPlayerDecoy goes straight DT_PlayerDecoy -> DT_BaseAnimating), the matcher's parallel depth-first walk hits a structural mismatch one level in: wire's first inheritance child name is DT_BaseAnimating, recv's is DT_BaseAnimatingOverlay.
struct WrapperInsertion {
	const char* parentTableName;    // table whose inheritance child gets re-targeted
	const char* wrapperName;        // synthesized intermediate table's name
	const char* existingChildName;  // safety check: parent's current child must match this name
};

// Each entry inserts one synthetic level.
// Derived from s21_dt_schema.h chain walks vs dedi's actual sdk_dt_dump_tree output.
static const WrapperInsertion s_wrapperInsertions[] = {
	// c52 CPlayerDecoy: S21 has DT_BaseAnimatingOverlay between DT_PlayerDecoy
	// and DT_BaseAnimating; dedi skips it. Verified via sdk_dt_dump_tree
	// DT_PlayerDecoy on the May 26 dedi run.
	{ "DT_PlayerDecoy",   "DT_BaseAnimatingOverlay", "DT_BaseAnimating" },
	// c55 CPlayerVehicle: same MISSING-LEVEL pattern as c52.
	{ "DT_PlayerVehicle", "DT_BaseAnimatingOverlay", "DT_BaseAnimating" },
};
static constexpr int kNumWrapperInsertions =
	sizeof(s_wrapperInsertions) / sizeof(s_wrapperInsertions[0]);

struct OverlayScalarTableSpec {
	const char* tableName;
	SendPropType type;
	int nBits;
	int flags;
	float lowValue;
	float highValue;
};

// m_animOverlayPlaybackRate lives in serveranimdata after m_animSequence
// (measured S21 flat[25..33]), not beside top-level m_animPlaybackRate.

static const OverlayScalarTableSpec s_overlayScalarTables[] = {
	{ "m_animOverlayIsActive",        SendPropType::DPT_Int,   32, 0, 0.0f,       0.0f },
	{ "m_animOverlayModelIndex",      SendPropType::DPT_Int,   32, 0, 0.0f,       0.0f },
	{ "m_animOverlaySequence",        SendPropType::DPT_Int,   12, 1, 0.0f,       0.0f },
	{ "m_animOverlayStartTime",       SendPropType::DPT_Time,  32, 0, 0.0f,       0.0f },
	{ "m_animOverlayStartCycle",      SendPropType::DPT_Float,  0, 4, 0.0f,       0.0f },
	{ "m_animOverlayPlaybackRate",    SendPropType::DPT_Float, 10, 0, 0.0f,       0.0f },
	{ "m_animOverlayWeight",          SendPropType::DPT_Float,  0, 4, 0.0f,       0.0f },
	{ "m_animOverlayOrder",           SendPropType::DPT_Int,   32, 0, 0.0f,       0.0f },
	{ "m_animOverlayAnimTime",        SendPropType::DPT_Time,  32, 0, 0.0f,       0.0f },
	{ "m_animOverlayFadeInDuration",  SendPropType::DPT_Float,  0, 4, 0.0f,       0.0f },
	{ "m_animOverlayFadeOutDuration", SendPropType::DPT_Float,  0, 4, 0.0f,       0.0f },
};
static constexpr int kNumOverlayScalarTables =
	sizeof(s_overlayScalarTables) / sizeof(s_overlayScalarTables[0]);

static void DTExtend_CacheSyntheticTable(uint8_t* table)
{
	if (table && s_cachedSendTableCount < kMaxCachedSendTables)
		s_cachedSendTablePtrs[s_cachedSendTableCount++] =
			reinterpret_cast<uintptr_t>(table);
}

static uint8_t* DTExtend_AllocSyntheticTable(const char* name, int nProps)
{
	uint8_t* table = static_cast<uint8_t*>(calloc(1, NR_SENDTABLE_SIZE));
	if (!table)
		return nullptr;
	uint8_t* props = nullptr;
	if (nProps > 0)
	{
		props = static_cast<uint8_t*>(calloc(static_cast<size_t>(nProps), SP_SIZE));
		if (!props)
		{
			free(table);
			return nullptr;
		}
	}

	*reinterpret_cast<uint8_t**>(table + ST_PROPS) = props;
	*reinterpret_cast<int*>(table + ST_NPROPS) = nProps;
	*reinterpret_cast<const char**>(table + ST_NETTABLENAME) = name;
	DTExtend_CacheSyntheticTable(table);
	return table;
}

static void DTExtend_InitSyntheticDataTableProp(uint8_t* dst, const uint8_t* dataTableTemplate,
	const char* propName, uint8_t* childTable)
{
	if (dataTableTemplate)
		memcpy(dst, dataTableTemplate, SP_SIZE);
	else
		memset(dst, 0, SP_SIZE);
	*reinterpret_cast<int*>(dst + SP_TYPE) = static_cast<int>(SendPropType::DPT_DataTable);
	*reinterpret_cast<const char**>(dst + SP_VARNAME) = propName;
	*reinterpret_cast<int*>(dst + SP_NELEMENTS) = 1;
	*reinterpret_cast<int*>(dst + SP_OFFSET) = 0;
	*reinterpret_cast<uint8_t**>(dst + 0x70) = childTable;
}

// priority < 0 keeps whatever the donor template carried (legacy).
// The flatten sorts by this byte, so a synthesized element that keeps a donor's arbitrary priority is torn away from the scalar it belongs beside: measured, our overlay elements carried 128 while m_animSequence carries 10, which is what pushed them out of flat[15..23] to flat[197].
static void DTExtend_InitSyntheticScalarProp(uint8_t* dst, const OverlayScalarTableSpec& spec,
	const char* propName, int priority)
{
	const uint8_t* tmpl = DTExtend_FindCleanTemplateProp(static_cast<int>(spec.type));
	if (tmpl)
		memcpy(dst, tmpl, SP_SIZE);
	else
		memset(dst, 0, SP_SIZE);

	*reinterpret_cast<int*>(dst + SP_TYPE) = static_cast<int>(spec.type);
	*reinterpret_cast<int*>(dst + SP_NBITS) = spec.nBits;
	*reinterpret_cast<float*>(dst + SP_LOW) = spec.lowValue;
	*reinterpret_cast<float*>(dst + SP_HIGH) = spec.highValue;
	*reinterpret_cast<const char**>(dst + SP_VARNAME) = propName;
	*reinterpret_cast<int*>(dst + SP_NELEMENTS) = 1;
	*reinterpret_cast<int*>(dst + SP_FLAGS) = spec.flags;
	*reinterpret_cast<int*>(dst + SP_OFFSET) = 0;
	if (priority >= 0)
		*reinterpret_cast<uint8_t*>(dst + SP_PRIORITY) = static_cast<uint8_t>(priority);
	*reinterpret_cast<uintptr_t*>(dst + 0x60) =
		reinterpret_cast<uintptr_t>(DTExtend_ZeroProxyForProp(propName));
}

static uint8_t* DTExtend_BuildOverlayArrayTable(const OverlayScalarTableSpec& spec,
	int priority)
{
	uint8_t* table = DTExtend_AllocSyntheticTable(spec.tableName, 9);
	if (!table)
		return nullptr;
	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	for (int i = 0; i < 9; ++i)
	{
		static const char* const kElemNames[] = {
			"[0000]", "[0001]", "[0002]", "[0003]", "[0004]",
			"[0005]", "[0006]", "[0007]", "[0008]",
		};
		DTExtend_InitSyntheticScalarProp(
			props + static_cast<uint64_t>(i) * SP_SIZE, spec, kElemNames[i], priority);
	}
	return table;
}

static bool DTExtend_IsBaseAnimatingOverlayInlineArray(const char* tableName)
{
	return tableName &&
		(strcmp(tableName, "m_animOverlaySequence") == 0 ||
		 strcmp(tableName, "m_animOverlayPlaybackRate") == 0);
}

static bool DTExtend_InitOverlayArrayDataTable(uint8_t* dst,
	const uint8_t* dataTableTemplate, const char* tableName, int priority)
{
	if (!dst || !tableName)
		return false;

	for (int i = 0; i < kNumOverlayScalarTables; ++i)
	{
		const OverlayScalarTableSpec& spec = s_overlayScalarTables[i];
		if (strcmp(spec.tableName, tableName) != 0)
			continue;

		uint8_t* child = DTExtend_BuildOverlayArrayTable(spec, priority);
		if (!child)
			return false;

		DTExtend_InitSyntheticDataTableProp(dst, dataTableTemplate,
			spec.tableName, child);
		return true;
	}

	return false;
}

// anchorAfter=false inserts the array DataTable prop immediately BEFORE anchorPropName, true immediately AFTER it.
// The client inlines this sub-table at its own position, so an array's elements land exactly where the array prop sits relative to its scalar -- which is the whole reason placement matters.
static uint8_t* DTExtend_CloneTableWithOverlayInsert(uint8_t* table,
	const uint8_t* dataTableTemplate, const char* insertTableName,
	const char* anchorPropName, bool* outInserted, bool anchorAfter = false)
{
	const char* beforePropName = anchorPropName;
	if (outInserted)
		*outInserted = false;
	if (!table || !insertTableName || !beforePropName)
		return nullptr;

	uint8_t* oldProps = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int oldN = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!oldProps || oldN <= 0 || oldN > 4096)
		return nullptr;

	if (DTExtend_FindPropIdx(table, insertTableName) >= 0 ||
		DTExtend_FindPropIdx(table, beforePropName) < 0)
		return table;

	uint8_t* clone = static_cast<uint8_t*>(malloc(NR_SENDTABLE_SIZE));
	if (!clone)
		return nullptr;
	memcpy(clone, table, NR_SENDTABLE_SIZE);
	*reinterpret_cast<uintptr_t*>(clone + 0x4C0) = 0;
	*reinterpret_cast<uintptr_t*>(clone + 0x508) = 0;

	const int newN = oldN + 1;
	uint8_t* newProps =
		static_cast<uint8_t*>(calloc(static_cast<size_t>(newN), SP_SIZE));
	if (!newProps)
	{
		free(clone);
		return nullptr;
	}

	int out = 0;
	bool inserted = false;
	for (int i = 0; i < oldN; ++i)
	{
		uint8_t* src = oldProps + static_cast<uint64_t>(i) * SP_SIZE;
		const char* name = *reinterpret_cast<const char**>(src + SP_VARNAME);

		const bool atAnchor = (!inserted && name && strcmp(name, beforePropName) == 0);
		// The elements must share the anchor scalar's priority or the flatten's
		// priority sort tears them away from it.
		const int anchorPri = atAnchor
			? *reinterpret_cast<const uint8_t*>(src + SP_PRIORITY) : -1;

		if (atAnchor && !anchorAfter)
		{
			if (!DTExtend_InitOverlayArrayDataTable(
				newProps + static_cast<uint64_t>(out++) * SP_SIZE,
				dataTableTemplate, insertTableName, anchorPri))
			{
				free(newProps);
				free(clone);
				return nullptr;
			}
			inserted = true;
		}

		memcpy(newProps + static_cast<uint64_t>(out++) * SP_SIZE, src, SP_SIZE);

		if (atAnchor && anchorAfter)
		{
			if (!DTExtend_InitOverlayArrayDataTable(
				newProps + static_cast<uint64_t>(out++) * SP_SIZE,
				dataTableTemplate, insertTableName, anchorPri))
			{
				free(newProps);
				free(clone);
				return nullptr;
			}
			inserted = true;
		}
	}

	if (!inserted || out != newN)
	{
		free(newProps);
		free(clone);
		return nullptr;
	}

	*reinterpret_cast<uint8_t**>(clone + ST_PROPS) = newProps;
	*reinterpret_cast<int*>(clone + ST_NPROPS) = newN;
	if (strcmp(insertTableName, "m_animOverlaySequence") == 0 ||
		strcmp(insertTableName, "m_animOverlayPlaybackRate") == 0)
	{
		const char* oldName = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		if (oldName && strcmp(oldName, "DT_ServerAnimationData") == 0)
			*reinterpret_cast<const char**>(clone + ST_NETTABLENAME) =
				"DT_ServerAnimationData_BaseAnimatingOverlay";
	}
	DTExtend_CacheSyntheticTable(clone);
	if (outInserted)
		*outInserted = true;
	return clone;
}

static uint8_t* DTExtend_CloneBaseAnimatingForOverlay(uint8_t* existingChild,
	const uint8_t* dataTableTemplate)
{
	if (!existingChild)
		return nullptr;

	uint8_t* oldProps = *reinterpret_cast<uint8_t**>(existingChild + ST_PROPS);
	const int oldN = *reinterpret_cast<int*>(existingChild + ST_NPROPS);
	if (!oldProps || oldN <= 0 || oldN > 4096)
		return existingChild;

	const bool needsTopSeq =
		DTExtend_FindPropIdx(existingChild, "m_animSequence") >= 0 &&
		DTExtend_FindPropIdx(existingChild, "m_animOverlaySequence") < 0;

	// Where m_animOverlayPlaybackRate goes.
	// Measured against the client: the rate elements belong at flat[25..33], i.e. as the LAST prop of the serveranimdata table, right after m_animSequence and just before this table's m_animPlaybackRate.
	uint8_t* animDataChild = nullptr;
	{
		const int nScan = (oldN < 4096) ? oldN : 4096;
		for (int i = 0; i < nScan && !animDataChild; ++i)
		{
			const uint8_t* p = oldProps + static_cast<uint64_t>(i) * SP_SIZE;
			if (*reinterpret_cast<const int*>(p + SP_TYPE) !=
				static_cast<int>(SendPropType::DPT_DataTable))
				continue;
			uint8_t* c = *reinterpret_cast<uint8_t* const*>(p + SP_CHILDTABLE);
			if (!c)
				continue;
			const char* cn = *reinterpret_cast<const char* const*>(c + ST_NETTABLENAME);
			if (cn && strncmp(cn, "DT_ServerAnimationData", 22) == 0)
				animDataChild = c;
		}
	}
	const bool rateInAnimData =
		animDataChild &&
		DTExtend_FindPropIdx(animDataChild, "m_animSequence") >= 0 &&
		DTExtend_FindPropIdx(animDataChild, "m_animOverlayPlaybackRate") < 0;

	const bool needsTopRate = !rateInAnimData &&
		DTExtend_FindPropIdx(existingChild, "m_animPlaybackRate") >= 0 &&
		DTExtend_FindPropIdx(existingChild, "m_animOverlayPlaybackRate") < 0;
	const int topInsertN = (needsTopSeq ? 1 : 0) + (needsTopRate ? 1 : 0);

	uint8_t* clone = static_cast<uint8_t*>(malloc(NR_SENDTABLE_SIZE));
	if (!clone)
		return nullptr;
	memcpy(clone, existingChild, NR_SENDTABLE_SIZE);
	*reinterpret_cast<uintptr_t*>(clone + 0x4C0) = 0;
	*reinterpret_cast<uintptr_t*>(clone + 0x508) = 0;

	const int newN = oldN + topInsertN;
	uint8_t* newProps =
		static_cast<uint8_t*>(calloc(static_cast<size_t>(newN), SP_SIZE));
	if (!newProps)
	{
		free(clone);
		return nullptr;
	}

	int out = 0;
	bool insertedSeq = false;
	bool insertedRate = false;
	bool clonedChild = false;
	for (int i = 0; i < oldN; ++i)
	{
		uint8_t* src = oldProps + static_cast<uint64_t>(i) * SP_SIZE;
		const char* name = *reinterpret_cast<const char**>(src + SP_VARNAME);

		if (needsTopSeq && !insertedSeq && name &&
			strcmp(name, "m_animSequence") == 0)
		{
			if (!DTExtend_InitOverlayArrayDataTable(
				newProps + static_cast<uint64_t>(out++) * SP_SIZE,
				dataTableTemplate, "m_animOverlaySequence",
				*reinterpret_cast<const uint8_t*>(src + SP_PRIORITY)))
			{
				free(newProps);
				free(clone);
				return nullptr;
			}
			insertedSeq = true;
		}

		if (needsTopRate && !insertedRate && name &&
			strcmp(name, "m_animPlaybackRate") == 0)
		{
			if (!DTExtend_InitOverlayArrayDataTable(
				newProps + static_cast<uint64_t>(out++) * SP_SIZE,
				dataTableTemplate, "m_animOverlayPlaybackRate",
				*reinterpret_cast<const uint8_t*>(src + SP_PRIORITY)))
			{
				free(newProps);
				free(clone);
				return nullptr;
			}
			insertedRate = true;
		}

		uint8_t* dst = newProps + static_cast<uint64_t>(out++) * SP_SIZE;
		memcpy(dst, src, SP_SIZE);

		if (*reinterpret_cast<int*>(dst + SP_TYPE) !=
			static_cast<int>(SendPropType::DPT_DataTable))
			continue;

		uint8_t* child = *reinterpret_cast<uint8_t**>(dst + 0x70);
		if (!child)
			continue;

		const char* childName =
			*reinterpret_cast<const char**>(child + ST_NETTABLENAME);
		if (!childName || strcmp(childName, "DT_ServerAnimationData") != 0)
			continue;

		bool childInserted = false;
		uint8_t* childClone = DTExtend_CloneTableWithOverlayInsert(
			child, dataTableTemplate, "m_animOverlaySequence",
			"m_animSequence", &childInserted);
		if (!childClone)
		{
			free(newProps);
			free(clone);
			return nullptr;
		}
		if (childInserted)
		{
			insertedSeq = true;
			clonedChild = true;
		}

		bool rateInserted = false;
		uint8_t* childClone2 = rateInAnimData
			? DTExtend_CloneTableWithOverlayInsert(
				childClone, dataTableTemplate, "m_animOverlayPlaybackRate",
				"m_animSequence", &rateInserted, /*anchorAfter*/ true)
			: DTExtend_CloneTableWithOverlayInsert(
				childClone, dataTableTemplate, "m_animOverlayPlaybackRate",
				"m_animPlaybackRate", &rateInserted);
		if (!childClone2)
		{
			free(newProps);
			free(clone);
			return nullptr;
		}
		if (rateInserted)
		{
			childClone = childClone2;
			insertedRate = true;
			clonedChild = true;
		}
		if (clonedChild)
			*reinterpret_cast<uint8_t**>(dst + 0x70) = childClone;
	}

	if (out != newN || !insertedSeq || !insertedRate)
	{
		Warning(eDLL_T::ENGINE,
			"[WRAPPER-INS] DT_BaseAnimatingOverlay clone incomplete: "
			"topSeq=%d topRate=%d seq=%d rate=%d child=%d; using original child\n",
			needsTopSeq ? 1 : 0, needsTopRate ? 1 : 0,
			insertedSeq ? 1 : 0, insertedRate ? 1 : 0, clonedChild ? 1 : 0);
		free(newProps);
		free(clone);
		return existingChild;
	}

	*reinterpret_cast<uint8_t**>(clone + ST_PROPS) = newProps;
	*reinterpret_cast<int*>(clone + ST_NPROPS) = newN;
	DTExtend_CacheSyntheticTable(clone);

	(void)existingChild;
	return clone;
}

static uint8_t* DTExtend_BuildBaseAnimatingOverlayWrapper(uint8_t* existingChild,
	const uint8_t* inheritProp)
{
	const uint8_t* dataTableTemplate = inheritProp
		? inheritProp
		: DTExtend_FindCleanTemplateProp(static_cast<int>(SendPropType::DPT_DataTable));

	uint8_t* emptyAnimationLayer = DTExtend_AllocSyntheticTable("DT_Animationlayer", 0);
	uint8_t* animOverlayArray = DTExtend_AllocSyntheticTable("m_AnimOverlay", 9);
	uint8_t* overlayVars = DTExtend_AllocSyntheticTable("DT_OverlayVars", 2);
	uint8_t* wrapper = DTExtend_AllocSyntheticTable("DT_BaseAnimatingOverlay", 11);
	if (!emptyAnimationLayer || !animOverlayArray || !overlayVars || !wrapper)
		return nullptr;

	uint8_t* animOverlayProps = *reinterpret_cast<uint8_t**>(animOverlayArray + ST_PROPS);
	static const char* const kElemNames[] = {
		"[0000]", "[0001]", "[0002]", "[0003]", "[0004]",
		"[0005]", "[0006]", "[0007]", "[0008]",
	};
	for (int i = 0; i < 9; ++i)
	{
		DTExtend_InitSyntheticDataTableProp(
			animOverlayProps + static_cast<uint64_t>(i) * SP_SIZE,
			dataTableTemplate, kElemNames[i], emptyAnimationLayer);
	}

	uint8_t* overlayVarsProps = *reinterpret_cast<uint8_t**>(overlayVars + ST_PROPS);
	DTExtend_InitSyntheticDataTableProp(overlayVarsProps, dataTableTemplate,
		"m_AnimOverlay", animOverlayArray);
	const OverlayScalarTableSpec countSpec = {
		"m_AnimOverlayCount", SendPropType::DPT_Int, 4, 1, 0.0f, 0.0f,
	};
	// -1 = keep the donor template's priority. Only the two INLINE arrays pair
	// with a scalar whose priority they must share; these hang off
	// DT_BaseAnimatingOverlay with no anchor, so leave them exactly as they were.
	DTExtend_InitSyntheticScalarProp(overlayVarsProps + SP_SIZE, countSpec,
		"m_AnimOverlayCount", -1);

	uint8_t* overlayChild =
		DTExtend_CloneBaseAnimatingForOverlay(existingChild, dataTableTemplate);
	if (!overlayChild)
		return nullptr;

	uint8_t* wrapperProps = *reinterpret_cast<uint8_t**>(wrapper + ST_PROPS);
	if (dataTableTemplate)
		memcpy(wrapperProps, dataTableTemplate, SP_SIZE);
	else
		memset(wrapperProps, 0, SP_SIZE);
	*reinterpret_cast<int*>(wrapperProps + SP_TYPE) =
		static_cast<int>(SendPropType::DPT_DataTable);
	*reinterpret_cast<const char**>(wrapperProps + SP_VARNAME) =
		"DT_BaseAnimatingOverlay";
	*reinterpret_cast<uint8_t**>(wrapperProps + 0x70) = overlayChild;
	DTExtend_InitSyntheticDataTableProp(wrapperProps + SP_SIZE, dataTableTemplate,
		"overlay_vars", overlayVars);

	int wrapperOut = 2;
	for (int i = 0; i < kNumOverlayScalarTables; ++i)
	{
		if (DTExtend_IsBaseAnimatingOverlayInlineArray(s_overlayScalarTables[i].tableName))
			continue;

		uint8_t* child = DTExtend_BuildOverlayArrayTable(s_overlayScalarTables[i], -1);
		if (!child)
			return nullptr;
		DTExtend_InitSyntheticDataTableProp(
			wrapperProps + static_cast<uint64_t>(wrapperOut++) * SP_SIZE,
			dataTableTemplate, s_overlayScalarTables[i].tableName, child);
	}

	return wrapper;
}

// Find the first SendProp in `table` of type DataTable (10) whose VAR_NAME equals `table`'s own ST_NETTABLENAME (the Source-SDK inheritance convention) AND whose linked sub-table's ST_NETTABLENAME equals `expectedChildName` (if non-NULL).
// Returns the byte pointer to the SendProp, or nullptr.
static uint8_t* DTExtend_FindInheritanceProp(uint8_t* table, const char* expectedChildName)
{
	if (!table) return nullptr;
	const char* tableNm = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
	if (!tableNm) return nullptr;
	uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
	if (!props || nProps <= 0 || nProps > 4096) return nullptr;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
		__try
		{
			if (*reinterpret_cast<int*>(p + SP_TYPE) != 10) continue;
			const char* nm = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (!nm || strcmp(nm, tableNm) != 0) continue;
			if (!expectedChildName) return p;
			uint8_t* child = *reinterpret_cast<uint8_t**>(p + 0x70);
			if (!child) continue;
			const char* childNm = *reinterpret_cast<const char**>(child + ST_NETTABLENAME);
			if (childNm && strcmp(childNm, expectedChildName) == 0) return p;
		}
		__except(EXCEPTION_EXECUTE_HANDLER) {}
	}
	return nullptr;
}

// REDIRECT variant: instead of allocating a synth wrapper, re-point an existing class's inheritance SP_DATATABLE to an existing table (typically one built by s_s21Classes).
// Used for classes where S21 inherits from a synth class the bridge already created -- no need to allocate a fresh wrapper, just splice the existing synth into the chain.
struct WrapperRedirect {
	const char* parentTableName;        // e.g. "DT_TriggerCylinderHeavy"
	const char* targetSynthName;        // e.g. "DT_TriggerCylinderNetworked" (must exist already)
	const char* currentDirectChildName; // e.g. "DT_BaseTrigger" (sanity check)
};

static const WrapperRedirect s_wrapperRedirects[] = {
	// c105 CTriggerCylinderHeavy: dedi inherits DT_BaseTrigger; S21 inherits the s_s21Classes synth DT_TriggerCylinderNetworked.
	// The synth already has the cohort props (m_triggerFilterMask/m_radius/m_aboveHeight/ m_belowHeight) via s_propCopies, so re-pointing puts them in the chain where c105's recv expects them.
	{ "DT_TriggerCylinderHeavy", "DT_TriggerCylinderNetworked", "DT_BaseTrigger" },
};
static constexpr int kNumWrapperRedirects =
	sizeof(s_wrapperRedirects) / sizeof(s_wrapperRedirects[0]);

static bool s_wrapperInsertApplied = false;
void DTExtend_InsertWrapperTables()
{
	if (s_wrapperInsertApplied) return;
	s_wrapperInsertApplied = true;

	int inserted = 0, skipped = 0;
	for (int r = 0; r < kNumWrapperInsertions; ++r)
	{
		const WrapperInsertion& w = s_wrapperInsertions[r];

		uint8_t* parent = DTExtend_FindTableByName(w.parentTableName);
		if (!parent)
		{
			Warning(eDLL_T::ENGINE, "[WRAPPER-INS] parent '%s' not found; skipping\n",
				w.parentTableName);
			++skipped;
			continue;
		}
		uint8_t* inheritProp = DTExtend_FindInheritanceProp(parent, w.existingChildName);
		if (!inheritProp)
		{
			Warning(eDLL_T::ENGINE,
				"[WRAPPER-INS] '%s' has no inheritance prop pointing to '%s'; skipping\n",
				w.parentTableName, w.existingChildName);
			++skipped;
			continue;
		}
		uint8_t* existingChild = *reinterpret_cast<uint8_t**>(inheritProp + 0x70);
		if (!existingChild)
		{
			++skipped;
			continue;
		}

		uint8_t* synth = nullptr;
		if (strcmp(w.wrapperName, "DT_BaseAnimatingOverlay") == 0)
		{
			synth = DTExtend_BuildBaseAnimatingOverlayWrapper(existingChild, inheritProp);
		}
		else
		{
			// Allocate synth SendTable.
			// The struct has live fields up to +0x508 (per NR_SENDTABLE_SIZE = 0x510 used elsewhere in this file for the NonRewind wrapper table).
			synth = DTExtend_AllocSyntheticTable(w.wrapperName, 1);
			if (synth)
			{
				uint8_t* synthProps = *reinterpret_cast<uint8_t**>(synth + ST_PROPS);
				// Clone the parent's existing inheritance SendProp bytes -- this preserves SP_TYPE=10, SP_FLAGS (inheritance flags), the proxy fn, SP_OFFSET (typically 0 for inheritance), nBits, and crucially SP_DATATABLE which still points to the existing child sub-table.
				memcpy(synthProps, inheritProp, SP_SIZE);
				// Rewrite the cloned prop's VAR_NAME to be the wrapper's own name
				// (Source convention: inheritance entry's name == own table name).
				*reinterpret_cast<const char**>(synthProps + SP_VARNAME) = w.wrapperName;
				// SP_DATATABLE at +0x70 stays = existingChild (preserved by memcpy).
			}
		}
		if (!synth)
		{
			Warning(eDLL_T::ENGINE,
				"[WRAPPER-INS] '%s' synth allocation failed; skipping\n",
				w.wrapperName);
			++skipped;
			continue;
		}
		// +0x4C0 m_pPrecalc stays NULL: v_SendTable_Init will allocate +
		// populate the precalc when it walks this table as part of the
		// parent's precalc build.

		// Splice: parent's inheritance prop now points to the wrapper.
		*reinterpret_cast<uint8_t**>(inheritProp + 0x70) = synth;

		++inserted;
	}

	// REDIRECT pass: for classes whose S21 inheritance target is an existing
	// s_s21Classes synth, re-point the parent's inheritance SP_DATATABLE
	// directly at that synth. No allocation.
	int redirected = 0, redirSkipped = 0;
	for (int r = 0; r < kNumWrapperRedirects; ++r)
	{
		const WrapperRedirect& wr = s_wrapperRedirects[r];

		uint8_t* parent = DTExtend_FindTableByName(wr.parentTableName);
		if (!parent)
		{
			Warning(eDLL_T::ENGINE,
				"[WRAPPER-RDR] parent '%s' not found; skipping\n", wr.parentTableName);
			++redirSkipped;
			continue;
		}
		uint8_t* inheritProp = DTExtend_FindInheritanceProp(parent, wr.currentDirectChildName);
		if (!inheritProp)
		{
			Warning(eDLL_T::ENGINE,
				"[WRAPPER-RDR] '%s' has no inheritance prop linking to '%s'; skipping\n",
				wr.parentTableName, wr.currentDirectChildName);
			++redirSkipped;
			continue;
		}
		uint8_t* target = DTExtend_FindTableByName(wr.targetSynthName);
		if (!target)
		{
			Warning(eDLL_T::ENGINE,
				"[WRAPPER-RDR] target synth '%s' not found in cache (s_s21Classes "
				"must have built it before this pass); skipping\n",
				wr.targetSynthName);
			++redirSkipped;
			continue;
		}

		// Re-point the parent's inheritance SP_DATATABLE at the synth.
		// Source's structure stays the same on the parent side; only the linked sub-table changes.
		*reinterpret_cast<uint8_t**>(inheritProp + 0x70) = target;

		Msg(eDLL_T::ENGINE,
			"[WRAPPER-RDR] '%s' inheritance re-pointed: '%s' -> '%s' (synth=%p)\n",
			wr.parentTableName, wr.currentDirectChildName,
			wr.targetSynthName, target);
		++redirected;
	}

	Msg(eDLL_T::ENGINE,
		"[WRAPPER-RDR] applied %d/%d redirects (%d skipped)\n",
		redirected, kNumWrapperRedirects, redirSkipped);
}

static bool s_bracketizeApplied = false;
void DTExtend_BracketizeSubTableChildren(void** tables, int count)
{
	if (s_bracketizeApplied) return;
	if (!tables || count <= 0) return;
	s_bracketizeApplied = true;

	BracketizeCtx ctx = {};
	for (int t = 0; t < count; ++t)
	{
		uint8_t* topTable = reinterpret_cast<uint8_t*>(tables[t]);
		if (!topTable) continue;
		DTExtend_BracketizeRecursive(topTable, ctx, 0);
	}

	Msg(eDLL_T::ENGINE,
		"[DT-BRACKET] Renamed %d numerically-named sub-table children to "
		"bracketed form across %d tables (walked %d distinct SendTables). "
		"S21 client's RecvProps for [000N] now have matching SendProps.\n",
		ctx.renamedTotal, ctx.tablesTouched, ctx.visitedN);
}
// ---------------------------------------------------------------------------
// [S21-CHAIN] Inheritance-chain audit for the synthesized S21 wrappers.
// Every S21 wrapper is a DEEP CLONE of a native parent tree, so each level below the renamed root keeps its parent's table NAME.
// ---------------------------------------------------------------------------
static void DTExtend_S21ChainLevelDiff(const char* className, int level,
	uint8_t* clone, uint8_t* nativeTbl)
{
	const int cn = *reinterpret_cast<int*>(clone + ST_NPROPS);
	const int nn = *reinterpret_cast<int*>(nativeTbl + ST_NPROPS);
	uint8_t* cp = *reinterpret_cast<uint8_t**>(clone + ST_PROPS);
	uint8_t* np = *reinterpret_cast<uint8_t**>(nativeTbl + ST_PROPS);
	if (cn != nn)
	{
		Warning(eDLL_T::ENGINE,
			"[S21-CHAIN] %s L%d: DIVERGENT nProps clone=%d native=%d "
			"-- client binds the native, encoder walks the clone\n",
			className, level, cn, nn);
		return;
	}
	for (int i = 0; cp && np && i < cn && i < 4096; ++i)
	{
		uint8_t* a = cp + static_cast<uint64_t>(i) * SP_SIZE;
		uint8_t* b = np + static_cast<uint64_t>(i) * SP_SIZE;
		const int ta = *reinterpret_cast<int*>(a + SP_TYPE);
		const int tb = *reinterpret_cast<int*>(b + SP_TYPE);
		const int ba = *reinterpret_cast<int*>(a + SP_NBITS);
		const int bb = *reinterpret_cast<int*>(b + SP_NBITS);
		const char* na = *reinterpret_cast<const char**>(a + SP_VARNAME);
		const char* nb = *reinterpret_cast<const char**>(b + SP_VARNAME);
		const bool nameDiff = (!na != !nb) || (na && nb && strcmp(na, nb) != 0);
		if (ta != tb || ba != bb || nameDiff)
		{
			Warning(eDLL_T::ENGINE,
				"[S21-CHAIN] %s L%d: DIVERGENT prop[%d] clone(ty=%d nb=%d '%s') "
				"native(ty=%d nb=%d '%s')\n",
				className, level, i, ta, ba, na ? na : "<null>",
				tb, bb, nb ? nb : "<null>");
			return;
		}
	}
}

void DTExtend_S21ChainAudit(void)
{
	if (!s_s21Slots)
		return;
	for (int ci = 0; ci < kNumS21Classes; ++ci)
	{
		S21ClassSlot& sl = s_s21Slots[ci];
		const char* cn = *(const char**)(sl.factory + FACT_CLASSNAME);
		if (!cn)
			continue;   // class skipped this boot

		uint8_t* tbl = sl.wrapperTable;
		for (int level = 0; tbl && level < 16; ++level)
		{
			if (!ODP_IsReadable(tbl, 0x4C0 + 8))
				break;
			const char* tn = *(const char**)(tbl + ST_NETTABLENAME);
			uint8_t* firstByName = DTExtend_FindTableByName(tn);
			if (level > 0)
			{
				if (!firstByName)
				{
					Warning(eDLL_T::ENGINE,
						"[S21-CHAIN] %s L%d: '%s' has no same-named table in the "
						"engine list -- clone-only level\n",
						cn, level, tn ? tn : "?");
				}
				else if (firstByName != tbl)
				{
					DTExtend_S21ChainLevelDiff(cn, level, tbl, firstByName);
				}
			}

			uint8_t* props = *(uint8_t**)(tbl + ST_PROPS);
			const int nProps = *(int*)(tbl + ST_NPROPS);
			uint8_t* next = nullptr;
			for (int i = 0; props && i < nProps && i < 4096; ++i)
			{
				uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
				if (*(int*)(p + SP_TYPE) != 10)   // DPT_DataTable
					continue;
				next = *(uint8_t**)(p + 0x70);
				break;                            // prop[0] is the inheritance slot
			}
			tbl = next;
		}
	}
}
