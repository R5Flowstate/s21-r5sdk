//=============================================================================//
//
// Purpose: dt_extend DIAG -- sv_dump_seqtable / bridge_extend_audit
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
#include "game/shared/dt_extend_diag.h"

// ===========================================================================
// sv_dump_seqtable: unique animating models' sequence tables.
// ServerClass at entity+0x50; CStudioHdr* at +0xFD8; seq label at seqdesc+4 (S3 int).
// ===========================================================================
static constexpr int SVSEQ_MAX_EDICTS    = 16384; // MAX_EDICTS; entity-ptr array at m_pEdicts+0x7808
static constexpr int SVSEQ_MAX_SEQCOUNT  = 8192;
static constexpr int SVSEQ_MAX_DT_DEPTH  = 16;
static constexpr int SVSEQ_DEDUP_CAP     = 256;
static constexpr int SVSEQ_DEDUP_NAME_SZ = 80;

static char s_svSeqDumped[SVSEQ_DEDUP_CAP][SVSEQ_DEDUP_NAME_SZ];
static int  s_svSeqDumpedCount = 0;

//-----------------------------------------------------------------------------
// Purpose: linear dedup on model name; table-full is a one-shot Warning.
//-----------------------------------------------------------------------------
static bool SvSeqTable_AlreadyDumped(const char* model)
{
	for (int i = 0; i < s_svSeqDumpedCount; i++)
	{
		if (strcmp(s_svSeqDumped[i], model) == 0)
			return true;
	}

	if (s_svSeqDumpedCount < SVSEQ_DEDUP_CAP)
	{
		char* dst = s_svSeqDumped[s_svSeqDumpedCount];
		strncpy(dst, model, SVSEQ_DEDUP_NAME_SZ - 1);
		dst[SVSEQ_DEDUP_NAME_SZ - 1] = '\0';
		s_svSeqDumpedCount++;
		return false;
	}

	static bool s_warnedFull = false;
	if (!s_warnedFull)
	{
		Warning(eDLL_T::SERVER, "[SEQTABLE](sv) dedup table full (256), dumping anyway\n");
		s_warnedFull = true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: true when the ServerClass DT chain contains DT_BaseAnimating.
//-----------------------------------------------------------------------------
static bool SvSeqTable_IsAnimatingClass(uintptr_t table)
{
	for (int depth = 0; table != 0 && depth < SVSEQ_MAX_DT_DEPTH; depth++)
	{
		const char* name = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		if (name && strcmp(name, "DT_BaseAnimating") == 0)
			return true;

		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (nProps <= 0 || !props)
			return false;

		uint8_t* p0 = props; // baseclass link is always prop[0]
		if (*reinterpret_cast<int*>(p0 + SP_TYPE) != 10) // 10 = DPT_DataTable
			return false;

		table = *reinterpret_cast<uintptr_t*>(p0 + 0x70);
	}
	return false;
}

// ===========================================================================
// Appended-prop aliasing detector. Samples 4 bytes the pack path would read
// per non-proxied append: ZERO / CONSTANT / VARYING / INCONCLUSIVE.
// ===========================================================================
struct ExtendAuditStat
{
	unsigned int samples;
	unsigned int nonZero;
	unsigned int sameAsFirst;
	uint32_t     firstVal;
};
static ExtendAuditStat s_extAudit[512] = {};

// Resolve the DT chain once per ServerClass and one VirtualQuery per entity.
// Per-entity-per-prop VirtualQuery froze the dedi.

// s_assigned is capped at 512, so a 512-bit mask covers "which appends apply".
struct ExtendAuditClass
{
	uintptr_t serverClass;
	uint64_t  mask[8];
	char      name[48];   // ServerClass name, copied -- see the chain-name note below
	int       nSeen;      // live entities of this class encountered
	int       nRejected;  // reads skipped because the offset ran past the region
};
static ExtendAuditClass s_extAuditClasses[256] = {};
static int s_extAuditClassCount = 0;

static inline bool ExtendAudit_MaskTest(const uint64_t* m, int i)
{
	return (m[i >> 6] >> (i & 63)) & 1ull;
}

// Resolve (and cache) which raw appends apply to this ServerClass. Returns null
// only when the cache is full.
static const ExtendAuditClass* ExtendAudit_ResolveClass(uintptr_t serverClass, uintptr_t rootTable)
{
	for (int c = 0; c < s_extAuditClassCount; ++c)
		if (s_extAuditClasses[c].serverClass == serverClass)
			return &s_extAuditClasses[c];

	if (s_extAuditClassCount >= 256)
		return nullptr;

	// Chain table names copied into bounded buffers -- pointers would dangle.
	char chain[SVSEQ_MAX_DT_DEPTH][64];
	int nChain = 0;
	uintptr_t table = rootTable;
	for (int depth = 0; table != 0 && depth < SVSEQ_MAX_DT_DEPTH; ++depth)
	{
		if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(table), ST_NETTABLENAME + 8))
			break;

		const char* name = *reinterpret_cast<const char**>(table + ST_NETTABLENAME);
		if (name && nChain < SVSEQ_MAX_DT_DEPTH &&
			DTExtend_IsSafeToRead(name, 64))
		{
			char* dst = chain[nChain];
			int k = 0;
			for (; k < 63 && name[k]; ++k)
				dst[k] = name[k];
			dst[k] = '\0';
			if (k > 0)
				++nChain;
		}

		uint8_t* props = *reinterpret_cast<uint8_t**>(table + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(table + ST_NPROPS);
		if (nProps <= 0 || !props)
			break;
		if (!DTExtend_IsSafeToRead(props, SP_SIZE))
			break;
		if (*reinterpret_cast<int*>(props + SP_TYPE) != 10) // 10 = DPT_DataTable
			break;

		table = *reinterpret_cast<uintptr_t*>(props + 0x70);
	}

	ExtendAuditClass& ec = s_extAuditClasses[s_extAuditClassCount++];
	ec.serverClass = serverClass;
	memset(ec.mask, 0, sizeof(ec.mask));
	ec.name[0] = '\0';
	ec.nSeen = 0;
	ec.nRejected = 0;
	if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(serverClass), sizeof(const char*)))
	{
		const char* cn = *reinterpret_cast<const char* const*>(serverClass);
		if (cn && DTExtend_IsSafeToRead(cn, 48))
		{
			int k = 0;
			for (; k < 47 && cn[k]; ++k)
				ec.name[k] = cn[k];
			ec.name[k] = '\0';
		}
	}
	for (int i = 0; i < s_assignedCount && i < 512; ++i)
	{
		const AssignedOffset& ao = s_assigned[i];
		if (ao.offset < 0 || !ao.tableName)
			continue;   // proxied -- reads no entity memory, nothing to audit
		for (int k = 0; k < nChain; ++k)
			if (!strcmp(chain[k], ao.tableName))
			{
				ec.mask[i >> 6] |= 1ull << (i & 63);
				break;
			}
	}
	return &ec;
}

static void ExtendAudit_SampleOnce(void)
{
	if (!gpGlobals || !gpGlobals->m_pEdicts)
		return;

	// Per-invocation rejection tally. Class list / buckets below are cumulative.
	int nEnts = 0, nClassMiss = 0;
	int nNullSlot = 0, nVqFail = 0, nNotCommit = 0, nTooSmall = 0, nNoClass = 0, nNoRoot = 0;

	for (int idx = 0; idx < SVSEQ_MAX_EDICTS; ++idx)
	{
		const uintptr_t ent = static_cast<uintptr_t>(gpGlobals->m_pEdicts[idx + 0x7808]);
		if (!ent)
		{
			++nNullSlot;
			continue;
		}

		// ONE VirtualQuery: validity plus the region end that bounds prop reads.
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(reinterpret_cast<const void*>(ent), &mbi, sizeof(mbi)) != sizeof(mbi))
		{
			++nVqFail;
			continue;
		}
		if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))
		{
			++nNotCommit;
			continue;
		}
		const uintptr_t regionEnd =
			reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (ent + 0x58 > regionEnd)
		{
			++nTooSmall;
			continue;
		}

		const uintptr_t serverClass = *reinterpret_cast<uintptr_t*>(ent + 0x50);
		if (!serverClass ||
			!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(serverClass + 0x08), sizeof(uintptr_t)))
		{
			++nNoClass;
			continue;
		}
		const uintptr_t rootTable = *reinterpret_cast<uintptr_t*>(serverClass + 0x08);
		if (!rootTable)
		{
			++nNoRoot;
			continue;
		}

		ExtendAuditClass* ec = const_cast<ExtendAuditClass*>(
			ExtendAudit_ResolveClass(serverClass, rootTable));
		if (!ec)
		{
			++nClassMiss;
			continue;
		}
		++nEnts;
		++ec->nSeen;

		for (int i = 0; i < s_assignedCount && i < 512; ++i)
		{
			if (!ExtendAudit_MaskTest(ec->mask, i))
				continue;
			const uintptr_t at = ent + static_cast<uintptr_t>(s_assigned[i].offset);
			if (at + sizeof(uint32_t) > regionEnd)
			{
				// Large objects can span committed regions; re-query `at` before reject.
				MEMORY_BASIC_INFORMATION mbi2;
				if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi2, sizeof(mbi2))
						!= sizeof(mbi2) ||
					mbi2.State != MEM_COMMIT ||
					mbi2.Protect == PAGE_NOACCESS || (mbi2.Protect & PAGE_GUARD) ||
					at + sizeof(uint32_t) >
						reinterpret_cast<uintptr_t>(mbi2.BaseAddress) + mbi2.RegionSize)
				{
					++ec->nRejected;
					continue;
				}
			}

			const uint32_t v = *reinterpret_cast<const uint32_t*>(at);
			ExtendAuditStat& st = s_extAudit[i];
			if (st.samples == 0)
				st.firstVal = v;
			++st.samples;
			if (v == st.firstVal) ++st.sameAsFirst;
			if (v != 0)           ++st.nonZero;
		}
	}

	Warning(eDLL_T::SERVER,
		"[EXTEND-AUDIT] THIS RUN: sampled %d entities | rejected: nullSlot=%d vqFail=%d "
		"notCommit=%d regionTooSmall=%d noServerClass=%d noRootTable=%d%s\n",
		nEnts, nNullSlot, nVqFail, nNotCommit, nTooSmall, nNoClass, nNoRoot,
		nClassMiss ? " (class cache FULL)" : "");
	if (nEnts == 0)
		Warning(eDLL_T::SERVER,
			"[EXTEND-AUDIT] THIS RUN WALKED NOTHING -- everything below is CUMULATIVE from "
			"earlier runs, not current. nullSlot==%d of %d means the entity array is empty "
			"(level unloaded / between maps); anything else points at the guard that rejected.\n",
			nNullSlot, SVSEQ_MAX_EDICTS);

	Warning(eDLL_T::SERVER,
		"[EXTEND-AUDIT] CUMULATIVE across runs: %d distinct ServerClasses\n",
		s_extAuditClassCount);

	// Name the classes actually walked.
	char line[512];
	size_t o = 0;
	line[0] = '\0';
	for (int c = 0; c < s_extAuditClassCount; ++c)
	{
		const int w = snprintf(line + o, sizeof(line) > o ? sizeof(line) - o : 0,
			"%s%s(%d)", o ? " " : "",
			s_extAuditClasses[c].name[0] ? s_extAuditClasses[c].name : "?",
			s_extAuditClasses[c].nSeen);
		if (w < 0) break;
		o += (size_t)w;
		if (o > sizeof(line) - 64)
		{
			Warning(eDLL_T::SERVER, "[EXTEND-AUDIT]   classes: %s\n", line);
			o = 0; line[0] = '\0';
		}
	}
	if (line[0])
		Warning(eDLL_T::SERVER, "[EXTEND-AUDIT]   classes: %s\n", line);
}

static void ExtendAudit_Report(void)
{
	int nZero = 0, nConst = 0, nVary = 0, nThin = 0, nUnsampled = 0;

	Warning(eDLL_T::SERVER,
		"[EXTEND-AUDIT] %d appended props registered; buckets below are CUMULATIVE. "
		"CONSTANT/VARYING need a proxy, ZERO does not. Vector rows report their .x only.\n",
		s_assignedCount);

	for (int pass = 0; pass < 2; ++pass)
	{
		for (int i = 0; i < s_assignedCount && i < 512; ++i)
		{
			const AssignedOffset& ao = s_assigned[i];
			if (ao.offset < 0 || !ao.tableName || !ao.propName)
				continue;
			const ExtendAuditStat& st = s_extAudit[i];
			if (st.samples == 0)
			{
				if (pass == 0) ++nUnsampled;
				continue;
			}

			// n==1 is trivially "constant"; need kMinSamples before classifying.
			static const unsigned kMinSamples = 5;
			const bool allZero  = (st.nonZero == 0);
			const bool thin     = !allZero && (st.samples < kMinSamples);
			const bool constant = !allZero && !thin && (st.sameAsFirst == st.samples);
			if (pass == 0)
			{
				if (allZero) ++nZero; else if (thin) ++nThin;
				else if (constant) ++nConst; else ++nVary;
				continue;
			}
			if (allZero)
				continue;   // second pass prints only the actionable rows

			Warning(eDLL_T::SERVER,
				"[EXTEND-AUDIT] %-12s %-34s %-30s off=%-6d n=%-5u first=0x%08X match=%u/%u%s\n",
				thin ? "INCONCLUSIVE" : (constant ? "CONSTANT" : "VARYING"),
				ao.tableName, ao.propName, ao.offset, st.samples, st.firstVal,
				st.sameAsFirst, st.samples,
				thin ? "  (need more samples of this class)" : "");
		}
		if (pass == 0)
		{
			Warning(eDLL_T::SERVER,
				"[EXTEND-AUDIT] raw(non-proxied) sampled: ZERO=%d CONSTANT=%d VARYING=%d "
				"INCONCLUSIVE=%d | unsampled(no live entity of that class)=%d\n",
				nZero, nConst, nVary, nThin, nUnsampled);

			// Name unsampled tables. No live entity of that class is UNKNOWN, not clean.
			for (int i = 0; i < s_assignedCount && i < 512; ++i)
			{
				const AssignedOffset& ao = s_assigned[i];
				if (ao.offset < 0 || !ao.tableName || s_extAudit[i].samples != 0)
					continue;
				bool alreadyPrinted = false;
				for (int j = 0; j < i; ++j)
					if (s_assigned[j].offset >= 0 && s_assigned[j].tableName &&
						s_extAudit[j].samples == 0 &&
						!strcmp(s_assigned[j].tableName, ao.tableName))
					{
						alreadyPrinted = true;
						break;
					}
				if (alreadyPrinted)
					continue;
				int n = 0;
				for (int j = 0; j < s_assignedCount && j < 512; ++j)
					if (s_assigned[j].offset >= 0 && s_assigned[j].tableName &&
						s_extAudit[j].samples == 0 &&
						!strcmp(s_assigned[j].tableName, ao.tableName))
						++n;

				// Live class with every read rejected is BLIND, not a clean miss.
				const ExtendAuditClass* owner = nullptr;
				for (int c = 0; c < s_extAuditClassCount; ++c)
					if (ExtendAudit_MaskTest(s_extAuditClasses[c].mask, i))
					{
						owner = &s_extAuditClasses[c];
						break;
					}

				if (owner)
					Warning(eDLL_T::SERVER,
						"[EXTEND-AUDIT]   BLIND    %-34s %d prop(s) -- class '%s' WAS live "
						"(%d ents) but every read fell outside its committed region "
						"(%d rejected). Offsets unverifiable this way.\n",
						ao.tableName, n, owner->name[0] ? owner->name : "?",
						owner->nSeen, owner->nRejected);
				else
					Warning(eDLL_T::SERVER,
						"[EXTEND-AUDIT]   UNSAMPLED %-34s %d prop(s) -- no live entity of this class\n",
						ao.tableName, n);
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Which CWeaponX layout is live: twins register the same names at different
// offsets (0x1300 family vs 0x11F0 family).
//-----------------------------------------------------------------------------
static bool ExtendAudit_SafeStrEq(const char* p, const char* lit)
{
	if (!p || !DTExtend_IsSafeToRead(p, 96))
		return false;
	for (int i = 0; i < 95; ++i)
	{
		if (p[i] != lit[i])
			return false;
		if (!lit[i])
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Native prop offset on this entity's ServerClass. Nested leaves need parent
// DataTable offsets accumulated; type 10 SP_OFFSET is a proxy index.
//-----------------------------------------------------------------------------
struct NativePropOffset_t
{
	uintptr_t   serverClass;
	const char* propName;
	int         offset;
};

static NativePropOffset_t s_nativePropOffsets[64];
static int s_nativePropOffsetCount = 0;

int DTExtend_FindNativePropOffset(const void* pEntity, const char* propName)
{
	if (!pEntity || !propName || !*propName)
		return -1;

	const uintptr_t ent = reinterpret_cast<uintptr_t>(pEntity);
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(ent + 0x50), sizeof(uintptr_t)))
		return -1;

	const uintptr_t sc = *reinterpret_cast<uintptr_t*>(ent + 0x50);
	if (!sc || !DTExtend_IsSafeToRead(reinterpret_cast<const void*>(sc + 0x08), sizeof(uintptr_t)))
		return -1;

	// Callers pass string literals; pointer compare hits after the first call.
	for (int i = 0; i < s_nativePropOffsetCount; ++i)
	{
		if (s_nativePropOffsets[i].serverClass != sc)
			continue;
		if (s_nativePropOffsets[i].propName == propName ||
			ExtendAudit_SafeStrEq(s_nativePropOffsets[i].propName, propName))
			return s_nativePropOffsets[i].offset;
	}

	int result = -1;

	// Nested-table leaves are enclosing-struct relative; accumulate parent
	// DataTable offsets or the result looks like a plausible entity offset.
	constexpr int kMaxTables = 256;
	uintptr_t queue[kMaxTables];
	int base[kMaxTables];
	int qn = 0;
	bool bTruncated = false;
	queue[qn] = *reinterpret_cast<uintptr_t*>(sc + 0x08);
	base[qn] = 0;
	++qn;

	for (int qi = 0; qi < qn && result < 0; ++qi)
	{
		const uintptr_t t = queue[qi];
		const int tBase = base[qi];
		if (!t || !DTExtend_IsSafeToRead(reinterpret_cast<const void*>(t), ST_NETTABLENAME + 8))
			continue;

		uint8_t* const props = *reinterpret_cast<uint8_t**>(t + ST_PROPS);
		const int nProps = *reinterpret_cast<int*>(t + ST_NPROPS);
		if (!props || nProps <= 0 || nProps > 4096 ||
			!DTExtend_IsSafeToRead(props, static_cast<size_t>(nProps) * SP_SIZE))
			continue;

		for (int i = 0; i < nProps; ++i)
		{
			uint8_t* const p = props + static_cast<uint64_t>(i) * SP_SIZE;

			if (*reinterpret_cast<int*>(p + SP_TYPE) == 10)
			{
				const uintptr_t child = *reinterpret_cast<uintptr_t*>(p + 0x70);
				if (!child)
					continue;
				if (qn < kMaxTables)
				{
					queue[qn] = child;
					base[qn] = tBase + *reinterpret_cast<int*>(p + SP_OFFSET);
					++qn;
				}
				else
				{
					bTruncated = true;
				}
				continue;
			}

			const char* const vn = *reinterpret_cast<const char**>(p + SP_VARNAME);
			if (!ExtendAudit_SafeStrEq(vn, propName))
				continue;

			const int off = tBase + *reinterpret_cast<int*>(p + SP_OFFSET);
			if (off > 0)
			{
				result = off;
				break;
			}
		}
	}

	// Truncated walk and absent prop both return -1; say which.
	if (result < 0 && bTruncated)
		Warning(eDLL_T::SERVER,
			"[PROP-OFF] table walk hit the %d-table cap looking for '%s' -- "
			"'not registered' below may be a truncated walk, not an absent prop\n",
			kMaxTables, propName);

	if (s_nativePropOffsetCount < 64)
	{
		NativePropOffset_t& slot = s_nativePropOffsets[s_nativePropOffsetCount++];
		slot.serverClass = sc;
		slot.propName    = propName;
		slot.offset      = result;

		const char* const rawName = *reinterpret_cast<const char* const*>(sc);
		const char* const cn = DTExtend_IsSafeToRead(rawName, 96) ? rawName : "?";

		if (result < 0)
			Warning(eDLL_T::SERVER,
				"[PROP-OFF] '%s' is NOT a registered prop on %s -- dependent write skipped\n",
				propName, cn);
		else
			Msg(eDLL_T::SERVER, "[PROP-OFF] %s.%s off=%d 0x%04X\n", cn, propName, result, result);
	}

	return result;
}

static void ExtendAudit_DumpWeaponLayout(void)
{
	if (!gpGlobals || !gpGlobals->m_pEdicts)
	{
		Warning(eDLL_T::SERVER, "[LAYOUT-PROBE] no edict array\n");
		return;
	}

	static const char* const kWant[] = {
		"m_weaponOwner", "m_lastPrimaryAttack", "m_ammoInClip", "m_lastRegenTime",
		"m_burstFireCount", "m_burstFireIndex", "m_weapState", "m_shotCount",
	};

	for (int idx = 0; idx < SVSEQ_MAX_EDICTS; ++idx)
	{
		const uintptr_t ent = static_cast<uintptr_t>(gpGlobals->m_pEdicts[idx + 0x7808]);
		if (!ent ||
			!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(ent + 0x58), sizeof(uintptr_t)))
			continue;
		const uintptr_t sc = *reinterpret_cast<uintptr_t*>(ent + 0x50);
		if (!sc || !DTExtend_IsSafeToRead(reinterpret_cast<const void*>(sc + 0x10), sizeof(uintptr_t)))
			continue;
		const char* cn = *reinterpret_cast<const char* const*>(sc);
		if (!ExtendAudit_SafeStrEq(cn, "CWeaponX"))
			continue;

		Warning(eDLL_T::SERVER,
			"[LAYOUT-PROBE] live CWeaponX ent=%p edict=%d -- SendProp offsets as REGISTERED:\n",
			(void*)ent, idx);

		// Breadth walk of the table tree.
		uintptr_t queue[32];
		int qn = 0;
		queue[qn++] = *reinterpret_cast<uintptr_t*>(sc + 0x08);
		int found = 0;
		for (int qi = 0; qi < qn && qi < 32; ++qi)
		{
			const uintptr_t t = queue[qi];
			if (!t || !DTExtend_IsSafeToRead(reinterpret_cast<const void*>(t), ST_NETTABLENAME + 8))
				continue;
			uint8_t* props = *reinterpret_cast<uint8_t**>(t + ST_PROPS);
			const int nProps = *reinterpret_cast<int*>(t + ST_NPROPS);
			if (!props || nProps <= 0 || nProps > 4096 ||
				!DTExtend_IsSafeToRead(props, static_cast<size_t>(nProps) * SP_SIZE))
				continue;
			const char* tn = *reinterpret_cast<const char**>(t + ST_NETTABLENAME);
			for (int i = 0; i < nProps; ++i)
			{
				uint8_t* p = props + static_cast<uint64_t>(i) * SP_SIZE;
				const char* vn = *reinterpret_cast<const char**>(p + SP_VARNAME);
				if (vn && !DTExtend_IsSafeToRead(vn, 96))
					continue;
				if (*reinterpret_cast<int*>(p + SP_TYPE) == 10 && qn < 32)
				{
					const uintptr_t child = *reinterpret_cast<uintptr_t*>(p + 0x70);
					if (child)
						queue[qn++] = child;
					continue;
				}
				for (const char* w : kWant)
					if (ExtendAudit_SafeStrEq(vn, w))
					{
						const int off = *reinterpret_cast<int*>(p + SP_OFFSET);
						Warning(eDLL_T::SERVER,
							"[LAYOUT-PROBE]   %-28s off=%-6d 0x%04X   (%s)\n",
							vn, off, off, tn ? tn : "?");
						++found;
					}
			}
		}
		Warning(eDLL_T::SERVER,
			"[LAYOUT-PROBE] %d prop(s) matched. 0x1300/0x1304/0x1334 => the 0x140A2 layout is "
			"live (weapon_heat.cpp is WRONG); 0x11F0/0x11F4/0x1224 => the 0x140FEF layout is "
			"live (weapon_heat.cpp is RIGHT).\n", found);

		return;
	}

	Warning(eDLL_T::SERVER,
		"[LAYOUT-PROBE] no live CWeaponX found -- spawn in holding a weapon first\n");
}

static void CC_ExtendAudit_f(const CCommand& args)
{
	if (args.ArgC() > 1 && !strcmp(args.Arg(1), "layout"))
	{
		ExtendAudit_DumpWeaponLayout();
		return;
	}
	if (args.ArgC() > 1 && !strcmp(args.Arg(1), "reset"))
	{
		memset(s_extAudit, 0, sizeof(s_extAudit));
		Warning(eDLL_T::SERVER, "[EXTEND-AUDIT] stats reset\n");
		return;
	}

	if (!DTExtend_Applied())
	{
		Warning(eDLL_T::SERVER, "[EXTEND-AUDIT] dt_extend has not applied yet -- load a map first\n");
		return;
	}

	ExtendAudit_SampleOnce();
	ExtendAudit_Report();
}

static ConCommand bridge_extend_audit("bridge_extend_audit", CC_ExtendAudit_f,
	"[EXTEND-AUDIT] Sample every NON-proxied appended SendProp out of the live "
	"entities and classify it ZERO / CONSTANT / VARYING. CONSTANT and VARYING mean "
	"the append is aliasing a real S3 field and needs a proxy. Run it repeatedly to "
	"accumulate samples; 'bridge_extend_audit reset' clears them.", FCVAR_RELEASE);

//-----------------------------------------------------------------------------
// Purpose: dump one model's sequence table. ODP preflight then POD copy.
//-----------------------------------------------------------------------------
static void SvSeqTable_DumpModel(int edictIdx, const char* className, uintptr_t studioHdrPtr)
{
	char model[64];
	model[0] = '\0';
	uintptr_t studiohdr = 0;
	uintptr_t vmodel = 0;

	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(studioHdrPtr + 0x08), sizeof(uintptr_t)) ||
		!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(studioHdrPtr + 0x10), sizeof(uintptr_t)))
	{
		Warning(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d class='%s' fault reading studiohdr/model name, skipped\n",
			edictIdx, className);
		return;
	}
	studiohdr = *reinterpret_cast<uintptr_t*>(studioHdrPtr + 0x08);
	vmodel = *reinterpret_cast<uintptr_t*>(studioHdrPtr + 0x10);

	if (!studiohdr)
	{
		Warning(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d class='%s' studiohdr null, skipped\n",
			edictIdx, className);
		return;
	}

	const char* pName = reinterpret_cast<const char*>(studiohdr + 0x10);
	if (!DTExtend_IsSafeToRead(pName, 64))
	{
		Warning(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d class='%s' fault reading studiohdr/model name, skipped\n",
			edictIdx, className);
		return;
	}
	int i = 0;
	for (; i < 63; i++)
	{
		model[i] = pName[i];
		if (model[i] == '\0')
			break;
	}
	model[i < 63 ? i : 63] = '\0';

	if (SvSeqTable_AlreadyDumped(model))
		return;

	int seqCount = 0;
	uintptr_t base = 0;
	int localseqindex = 0;
	const bool useVirtual = (vmodel != 0);

	if (useVirtual)
	{
		if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(vmodel + 0x20), sizeof(int)) ||
			!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(vmodel + 0x08), sizeof(uintptr_t)))
		{
			Warning(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d class='%s' model='%s' fault reading seq header, skipped\n",
				edictIdx, className, model);
			return;
		}
		seqCount = *reinterpret_cast<int*>(vmodel + 0x20);
		base = *reinterpret_cast<uintptr_t*>(vmodel + 0x08);
	}
	else
	{
		if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(studiohdr + 0xC0), sizeof(int)) ||
			!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(studiohdr + 0xC4), sizeof(int)))
		{
			Warning(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d class='%s' model='%s' fault reading seq header, skipped\n",
				edictIdx, className, model);
			return;
		}
		seqCount = *reinterpret_cast<int*>(studiohdr + 0xC0);      // numlocalseq
		localseqindex = *reinterpret_cast<int*>(studiohdr + 0xC4); // localseqindex
	}

	if (seqCount < 0)
		seqCount = 0;
	if (seqCount > SVSEQ_MAX_SEQCOUNT)
		seqCount = SVSEQ_MAX_SEQCOUNT;

	Msg(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d class='%s' model='%s' seqCount=%d mode=%s ===BEGIN===\n",
		edictIdx, className, model, seqCount, useVirtual ? "vmodel" : "raw");

	for (int si = 0; si < seqCount; si++)
	{
		// 192 bytes: rseq labels are full asset paths; a 63-char cut collided.
		char label[192];
		label[0] = '\0';
		bool faulted = false;
		uintptr_t seqdesc = 0;

		if (useVirtual)
		{
			uintptr_t elem = base + 0x18 * static_cast<uintptr_t>(si);
			if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(elem + 0x08), sizeof(uintptr_t)))
				faulted = true;
			else
				seqdesc = *reinterpret_cast<uintptr_t*>(elem + 0x08);
		}
		else
		{
			seqdesc = studiohdr + localseqindex + 0xD0 * static_cast<uintptr_t>(si);
		}

		if (!faulted)
		{
			if (!seqdesc)
			{
				label[0] = '-';
				label[1] = '\0';
			}
			else if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(seqdesc + 4), sizeof(int)))
			{
				faulted = true;
			}
			else
			{
				const int labelOff = *reinterpret_cast<int*>(seqdesc + 4);
				const char* pLabel = reinterpret_cast<const char*>(seqdesc + labelOff);
				int maxCopy = 192;
				if (!DTExtend_IsSafeToRead(pLabel, static_cast<size_t>(maxCopy)))
				{
					while (maxCopy > 0 && !DTExtend_IsSafeToRead(pLabel, static_cast<size_t>(maxCopy)))
						maxCopy >>= 1;
					if (maxCopy <= 0)
						faulted = true;
				}
				if (!faulted)
				{
					const int limit = (maxCopy < 192) ? maxCopy : 191;
					int j = 0;
					for (; j < limit; j++)
					{
						label[j] = pLabel[j];
						if (label[j] == '\0')
							break;
					}
					label[j < limit ? j : limit] = '\0';
				}
			}
		}

		if (faulted)
		{
			label[0] = '<';
			label[1] = 'x';
			label[2] = '>';
			label[3] = '\0';
		}

		// mstudioseqdesc_t: +8 activitynameindex, +16 resolved activity id.
		// -1 = name absent from the activity table at last resolve.
		char actName[64];
		actName[0] = '\0';
		int actId = -2;
		if (!faulted && seqdesc)
		{
			if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(seqdesc + 8), sizeof(int) * 3))
			{
				actId = *reinterpret_cast<int*>(seqdesc + 16);

				const int actOff = *reinterpret_cast<int*>(seqdesc + 8);
				const char* pAct = reinterpret_cast<const char*>(seqdesc + actOff);
				if (actOff != 0 && DTExtend_IsSafeToRead(pAct, 64))
				{
					int k = 0;
					for (; k < 63; k++)
					{
						actName[k] = pAct[k];
						if (actName[k] == '\0')
							break;
					}
					actName[k < 63 ? k : 63] = '\0';
				}
			}
		}

		Msg(eDLL_T::SERVER, "[SEQTABLE](sv) %4d act=%-5d '%s' %s\n",
			si, actId, actName, label);
	}

	Msg(eDLL_T::SERVER, "[SEQTABLE](sv) model='%s' ===END=== (%d seqs)\n", model, seqCount);
}

//-----------------------------------------------------------------------------
// Purpose: dump one edict if it is animating with a cached studiohdr.
//-----------------------------------------------------------------------------
static bool SvSeqTable_TryDumpEdict(int idx, bool& outHadEntity, bool& outIsAnimating, bool& outHasStudioHdr)
{
	outHadEntity = false;
	outIsAnimating = false;
	outHasStudioHdr = false;

	if (!gpGlobals || !gpGlobals->m_pEdicts)
		return false;

	uintptr_t ent = static_cast<uintptr_t>(gpGlobals->m_pEdicts[idx + 0x7808]);
	if (!ent)
		return false;

	outHadEntity = true;

	uintptr_t serverClass = 0;
	const char* className = "<none>";
	uintptr_t studioHdrPtr = 0;

	if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(ent + 0x50), sizeof(uintptr_t)))
		return false;
	serverClass = *reinterpret_cast<uintptr_t*>(ent + 0x50);
	if (serverClass &&
		DTExtend_IsSafeToRead(reinterpret_cast<const void*>(serverClass + 0x00), sizeof(const char*)))
	{
		const char* name = *reinterpret_cast<const char**>(serverClass + 0x00);
		if (name)
			className = name;
	}

	uintptr_t rootTable = 0;
	if (serverClass)
	{
		if (!DTExtend_IsSafeToRead(reinterpret_cast<const void*>(serverClass + 0x08), sizeof(uintptr_t)))
			return false;
		rootTable = *reinterpret_cast<uintptr_t*>(serverClass + 0x08);
	}

	if (!SvSeqTable_IsAnimatingClass(rootTable))
		return false;

	outIsAnimating = true;

	if (DTExtend_IsSafeToRead(reinterpret_cast<const void*>(ent + 0xFD8), sizeof(uintptr_t)))
		studioHdrPtr = *reinterpret_cast<uintptr_t*>(ent + 0xFD8);

	if (!studioHdrPtr)
		return false;

	outHasStudioHdr = true;

	SvSeqTable_DumpModel(idx, className, studioHdrPtr);
	return true;
}

void DTExtend_DumpSeqTableForStudioHdr(uintptr_t studioHdrPtr, const char* reason)
{
	if (!studioHdrPtr)
		return;

	// Dedupes by model name; hot-path callers print once per model.
	SvSeqTable_DumpModel(-1, reason ? reason : "?", studioHdrPtr);
}

//-----------------------------------------------------------------------------
// Purpose: sv_dump_seqtable. No arg = full sweep; one numeric arg = that edict.
//-----------------------------------------------------------------------------
static void CC_SvDumpSeqTable(const CCommand& args)
{
	s_svSeqDumpedCount = 0;

	if (args.ArgC() >= 2)
	{
		const int idx = atoi(args[1]);
		bool hadEntity = false, isAnimating = false, hasStudioHdr = false;
		const bool dumped = SvSeqTable_TryDumpEdict(idx, hadEntity, isAnimating, hasStudioHdr);

		if (!dumped)
		{
			if (!hadEntity)
				Warning(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d: no entity at this index\n", idx);
			else if (!isAnimating)
				Warning(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d: entity is not CBaseAnimating-derived (no DT_BaseAnimating in DT chain)\n", idx);
			else if (!hasStudioHdr)
				Warning(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d: animating entity has no cached m_pStudioHdr (+0xFD8 null)\n", idx);
			else
				Warning(eDLL_T::SERVER, "[SEQTABLE](sv) edict=%d: qualified but dump was skipped (already-dumped model)\n", idx);
		}
		return;
	}

	int uniqueModels = 0;
	int animatingVisited = 0;
	int skippedNoHdr = 0;

	for (int idx = 0; idx < SVSEQ_MAX_EDICTS; idx++)
	{
		bool hadEntity = false, isAnimating = false, hasStudioHdr = false;
		const int preCount = s_svSeqDumpedCount;

		const bool dumped = SvSeqTable_TryDumpEdict(idx, hadEntity, isAnimating, hasStudioHdr);

		if (isAnimating)
			animatingVisited++;
		if (isAnimating && !hasStudioHdr)
			skippedNoHdr++;
		if (dumped && s_svSeqDumpedCount > preCount)
			uniqueModels++;
	}

	Msg(eDLL_T::SERVER, "[SEQTABLE](sv) sweep done: %d unique models, %d animating entities visited, %d skipped (no cached hdr)\n",
		uniqueModels, animatingVisited, skippedNoHdr);
}

static ConCommand sv_dump_seqtable("sv_dump_seqtable", CC_SvDumpSeqTable,
	"Dump sequence tables (index->name) for all animating entities' unique models to the server log. Optional arg: edict index.",
	FCVAR_RELEASE);
