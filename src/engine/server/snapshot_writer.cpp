//=============================================================================//
//
// Purpose: Live snapshot encode/write path.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/shared/s21_bridge_compat.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "engine/server/server.h"
#include "snapshot_diag.h"
#include "snapshot_writer.h"
#include "snapshot_dump.h"
#include "tier0/memstd.h"
#include "tier0/memvalidate.h"
#include "tier0/tslist.h"
#include "game/shared/sdk_entity_state.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

extern CGlobalVars* gpGlobals;


//-----------------------------------------------------------------------------
// Hold m_animStartTime (+0xFEC) at sequence start; m_animStartCycle (+0xFF0)
// carries the cycle that was live when the anchor latched.
//-----------------------------------------------------------------------------
static ConVar bridge_anim_anchor_hold("bridge_anim_anchor_hold", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Hold m_animStartTime at sequence start instead of the dedi's per-tick re-anchor, "
	"so the S21 client extrapolates 3p anim cycles. Re-latches on loop wrap, same-seq "
	"restart, or clock reset. 0 = native integrator.");

static int      s_animAnchorLastSeq[2048];
static float    s_animAnchorHeldStart[2048];
static float    s_animAnchorHeldStartCyc[2048];
static float    s_animAnchorPrevQCyc[2048];
static uint32_t s_animAnchorOccupant[2048];
static bool     s_animAnchorInit = false;

static void __fastcall Hook_AnimAnchorUpdate(__int64 ent)
{
	v_AnimAnchorUpdate(ent);   // native: re-anchor to "now" + mark the fields net-dirty
	if (!bridge_anim_anchor_hold.GetBool() || !ent)
		return;

	// Native just ran on this ent -- no VirtualQuery, no SEH.
	if (!s_animAnchorInit)
	{
		for (int i = 0; i < 2048; ++i)
		{
			s_animAnchorLastSeq[i]      = -2;
			s_animAnchorHeldStart[i]    = 0.0f;
			s_animAnchorHeldStartCyc[i] = 0.0f;
			s_animAnchorPrevQCyc[i]     = -1.0f;
			s_animAnchorOccupant[i]     = 0;
		}
		s_animAnchorInit = true;
		Warning(eDLL_T::SERVER, "[ANIM-ANCHOR-HOLD] armed (players in-place, restart-aware latch; non-players via [ANIM-ANCHOR-PROXY])\n");
	}
	const uint16_t edict = *reinterpret_cast<uint16_t*>(ent + 0x58);
	if (edict >= 2048u)
		return;
	const uint32_t occupant = SDKEntityState_GetHandle(reinterpret_cast<const void*>(ent)).Raw();
	if (occupant != s_animAnchorOccupant[edict])
	{
		s_animAnchorOccupant[edict]     = occupant;
		s_animAnchorLastSeq[edict]      = -2;
		s_animAnchorHeldStart[edict]    = 0.0f;
		s_animAnchorHeldStartCyc[edict] = 0.0f;
		s_animAnchorPrevQCyc[edict]     = -1.0f;
	}
	// Class-type gate: only write anim anchor fields for CPlayer entities.
	// On every other class this pair is live integrator state; those are held
	// at encode time instead ([ANIM-ANCHOR-PROXY]).
	// m_pServerClass is at ent+0x50; name ptr is at serverClass+0x00.
	const uintptr_t serverClass = *reinterpret_cast<uintptr_t*>(ent + 0x50);
	if (!serverClass)
		return;
	const char* const className = *reinterpret_cast<const char**>(serverClass + 0x00);
	if (!className || _stricmp(className, "CPlayer") != 0)
		return;
	const int   seq  = *reinterpret_cast<int*>(ent + 0xFE0);   // m_animSequence
	const float nowT = *reinterpret_cast<float*>(ent + 0xFEC); // m_animStartTime
	const float qCyc = *reinterpret_cast<float*>(ent + 0xFF0); // m_animStartCycle

	// Re-anchor on sequence change, on a backward jump of the quantized cycle
	// (loop wrap / same-seq restart; 0.05 sits far above 10-bit quantize noise),
	// or on the native stamp moving backwards (server clock reset).
	const bool seqChanged   = (seq != s_animAnchorLastSeq[edict]);
	const bool cycleRestart = (s_animAnchorPrevQCyc[edict] >= 0.0f && qCyc < s_animAnchorPrevQCyc[edict] - 0.05f)
	                       || (nowT < s_animAnchorHeldStart[edict]);
	if (seqChanged || cycleRestart)
	{
		s_animAnchorHeldStart[edict]    = nowT;
		s_animAnchorHeldStartCyc[edict] = qCyc;
		s_animAnchorLastSeq[edict]      = seq;
	}
	s_animAnchorPrevQCyc[edict] = qCyc;

	// Native already marked these net-dirty; held values replicate every tick.
	*reinterpret_cast<float*>(ent + 0xFEC) = s_animAnchorHeldStart[edict];
	*reinterpret_cast<float*>(ent + 0xFF0) = s_animAnchorHeldStartCyc[edict];
}

// Class currently being encoded (WriteEntityProps a4). Read-only.
static std::atomic<int> g_curEncClass{ -1 };

// Pack record at *(a2[2]+0x20000)+8*idx: bits 0..10 flat, 13..20 cellsStaged.
// Type 0/1/8/9 = 1 cell; 2 = 3; Vector is always 3.

// 768KB svc_Snapshot slot is nDataBits/8; the frame pointer sits in the next 8 bytes.
// Overflow log is capped -- one overflowing snapshot refuses every remaining write.
static constexpr uint32_t SNAP_OVERFLOW_LOG_CAP = 48;
static constexpr const char* SNAP_OVERFLOW_LOG_CAPPED =
	" -- log capped, further refusals are SILENT";

static bool SnapOverflow_ShouldLog(std::atomic<uint32_t>& counter, uint32_t& outN)
{
	outN = counter.fetch_add(1, std::memory_order_relaxed) + 1;
	if (outN <= 16)
		return true;
	if ((outN % 512) != 0)
		return false;
	return (outN / 512) <= SNAP_OVERFLOW_LOG_CAP;
}

// True exactly once, on the line that reaches the cap.
static bool SnapOverflow_IsLastLog(uint32_t n)
{
	return n == SNAP_OVERFLOW_LOG_CAP * 512;
}

static void BfWrite_MarkOverflow(void* bf)
{
	if (!bf)
		return;
	reinterpret_cast<char*>(bf)[0x14] = 1;
}

// Direct bf_write bit emit (byte-addressed, LSB-first). Dedi layout differs from
// client: +0x00 m_pData, +0x0C m_nDataBits, +0x10 m_iCurBit, +0x14 m_bOverflow.
static inline int S21_BfRemaining(const int64_t* bf)
{
	if (!bf)
		return 0;
	const int cap = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(bf) + 0x0C);
	const int cur = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(bf) + 0x10);
	if (cur < 0 || cap < 0 || cur > cap)
		return 0;
	return cap - cur;
}

static inline void S21_BfWriteOneBit(int64_t* bf, int v)
{
	int* pCur = reinterpret_cast<int*>(reinterpret_cast<char*>(bf) + 0x10);
	const int cur = *pCur;
	if (cur >= *reinterpret_cast<int*>(reinterpret_cast<char*>(bf) + 0x0C))
	{
		*reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(bf) + 0x14) = 1; // overflow
		return;
	}
	uint8_t* data = *reinterpret_cast<uint8_t**>(bf);
	if (v) data[cur >> 3] |=  static_cast<uint8_t>(1 << (cur & 7));
	else   data[cur >> 3] &= static_cast<uint8_t>(~(1 << (cur & 7)));
	*pCur = cur + 1;
}
static inline void S21_BfWriteUBits(int64_t* bf, uint32_t value, int numBits)
{
	for (int i = 0; i < numBits; ++i)
		S21_BfWriteOneBit(bf, (value >> i) & 1u);
}

// DPT_Time(9)/DPT_Ticks(8) FULL form: Time bit0=0,bit1=0+float; Ticks bit0=0+int.
static ConVar bridge_time_encode("bridge_time_encode", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Encode DPT_Time(9)/DPT_Ticks(8) in the S21 client's wire format (FULL form) in the "
	"DT_EncodePropValue hook. Default 1 (ON). 0 = passthrough the S3 codec (the zipline "
	"DPT_Time desync returns).");

static ConVar bridge_arr_encode_diag("bridge_arr_encode_diag", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"Log first 500 array (type 5) encodes with element count. 0 = off (default).");

static ConVar bridge_team_enc_diag("bridge_team_enc_diag", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"Trace first 192 DT_EncodePropValue calls while encoding CTeam (class 82): "
	"type, nBits, cell pointer, cell value, and the native return (= packed dwords "
	"consumed). 0 = off (default).");

// Merge assumes backing arrays at +0x08/+0x10/+0x18; packedRuns NULL still AVs at +0xA6.
static ConVar sdk_snap_merge_guard("sdk_snap_merge_guard", "0",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"1 = 2048-slot light-bucket registry + SEH pointer probes on merge/reset. "
	"0 = ship: index-overflow check only.");

// Per-entity pack orchestrator (writes the 8-byte warm records + packed cells).
// Type 0/1/8/9 packers write one cell; a cellsStaged of 2 on those leaves an
// orphan dword that the next body (CTeam) then reads as strlen.
static ConVar sdk_snap_fixed_cell_clamp("sdk_snap_fixed_cell_clamp", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"1 = after pack, clamp type 0/1/8/9 records to 1 cell and compact the "
	"blob (stops a leftover cell shearing the next entity). 0 = observe only.");

static ConVar sdk_snap_pack_census("sdk_snap_pack_census", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"1 = PACK-VS-DESC / SNAP-ALIGN copies and dumps. 0 = clamp only.");

static ConVar sdk_snap_prefix_skip("sdk_snap_prefix_skip", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Compact DT_Player dest cells to dirty-list order when dest is a "
	"full-table pack (first dirty > 0). 0 = encode sequential from cell 0.");

static unsigned __int64 Hook_SGE_CollectChangedProps(int64_t a1,
	unsigned int a2, unsigned int a3, int a4, int64_t a5, int64_t a6,
	int64_t a7, int64_t a8)
{
	return v_SGE_CollectChangedProps(a1, a2, a3, a4, a5, a6, a7, a8);
}

static std::atomic<uint32_t> s_mergeBucketNullSkips{0};
static std::atomic<uint32_t> s_mergeBucketLightSkips{0};
static std::atomic<uint32_t> s_mergeCopyRangeSkips{0};
static std::atomic<uint32_t> s_resetBucketLightBackfillCount{0};
static std::atomic<uint32_t> s_resetBucketLightOverflowCount{0};
static constexpr size_t kSnapshotBucketBackingSize = 0x650C0;
static constexpr size_t kSnapshotBucketLightClearSize = 0x420;
static SRWLOCK s_resetBucketLightLock = SRWLOCK_INIT;
static uintptr_t s_resetBucketLightBuckets[2048] = {};

static bool SGE_IsLightResetBucketLocked(uintptr_t bucket)
{
	for (uintptr_t registered : s_resetBucketLightBuckets)
	{
		if (registered == bucket)
			return true;
	}
	return false;
}

static bool SGE_MarkLightResetBucket(uintptr_t bucket)
{
	if (!bucket)
		return false;

	AcquireSRWLockExclusive(&s_resetBucketLightLock);
	if (SGE_IsLightResetBucketLocked(bucket))
	{
		ReleaseSRWLockExclusive(&s_resetBucketLightLock);
		return true;
	}

	for (uintptr_t& registered : s_resetBucketLightBuckets)
	{
		if (!registered)
		{
			registered = bucket;
			ReleaseSRWLockExclusive(&s_resetBucketLightLock);
			return true;
		}
	}
	ReleaseSRWLockExclusive(&s_resetBucketLightLock);

	const uint32_t n = s_resetBucketLightOverflowCount.fetch_add(
		1, std::memory_order_relaxed);
	if (n < 8 || (n % 128) == 0)
	{
		Warning(eDLL_T::SERVER,
			"[FRAME-SNAP-GUARD] reset bucket light registry full #%u "
			"bucket=%p caller=%p\n",
			n + 1, reinterpret_cast<void*>(bucket), _ReturnAddress());
	}
	return false;
}

static bool SGE_IsLightResetBucket(uintptr_t bucket)
{
	AcquireSRWLockShared(&s_resetBucketLightLock);
	const bool result = SGE_IsLightResetBucketLocked(bucket);
	ReleaseSRWLockShared(&s_resetBucketLightLock);
	return result;
}

static void SGE_LightInitResetBucketBacking(void* backing, const bool fullClear)
{
	if (!backing)
		return;

	if (fullClear)
		memset(backing, 0, kSnapshotBucketBackingSize);
	else
		memset(backing, 0, kSnapshotBucketLightClearSize);

	int32_t* dwords = reinterpret_cast<int32_t*>(backing);
	dwords[0] = -1;
	dwords[1] = -1;
}

static void* SGE_AllocSnapshotBucketBacking()
{
	CAlignedMemAlloc* alignedAlloc = AlignedMemAlloc();
	if (!alignedAlloc)
		return nullptr;

	return alignedAlloc->Alloc(kSnapshotBucketBackingSize, 64);
}

static bool SGE_LooksLikeUserPointer(uintptr_t addr)
{
	return addr >= 0x10000ULL &&
		addr < 0x0000080000000000ULL &&
		(addr & 7) == 0;
}

// Snapshot full-bucket copier walks runs 0..*(head+0x1C) inclusive; that dword
// is run high-water -- guard rejects index >= 16384.
static constexpr int kSnapRunIndexCap = 0x4000; // valid indices 0..16383

static void SGE_FormatPtrRva(char* buf, size_t bufLen, const void* p)
{
	if (!buf || bufLen < 8)
		return;
	const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
	const uintptr_t base = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	const uintptr_t size = static_cast<uintptr_t>(g_GameDll.GetModuleSize());
	if (base && addr >= base && addr < base + size)
		snprintf(buf, bufLen, "%p rva=0x%llX", p,
			static_cast<unsigned long long>(addr - base));
	else
		snprintf(buf, bufLen, "%p rva=?", p);
}

static int64_t Hook_SGE_MergeSnapshotBucket(int64_t a1, uint64_t* src,
	uint64_t* dst, int srcIndex, int dstIndex, uint32_t* cursor)
{
	uint64_t srcHead = 0;
	uint64_t srcRuns = 0;
	uint64_t srcProps = 0;
	uint64_t dstHead = 0;
	uint64_t dstRuns = 0;
	uint64_t dstProps = 0;
	uint32_t dstPropCursor = 0;

	const uintptr_t srcBucket = reinterpret_cast<uintptr_t>(src);
	const uintptr_t dstBucket = reinterpret_cast<uintptr_t>(dst);
	// Run index 16384 writes one past each backing region. Reject >= 0x4000.
	const bool indexOverflow = srcIndex >= 0 && dstIndex >= 0 &&
		(srcIndex >= kSnapRunIndexCap || dstIndex >= kSnapRunIndexCap);
	bool bad = !srcBucket || !dstBucket || !cursor ||
		srcIndex < 0 || dstIndex < 0 ||
		srcIndex >= kSnapRunIndexCap || dstIndex >= kSnapRunIndexCap;
	bool lightBucket = false;

	if (indexOverflow)
	{
		static std::atomic<uint32_t> s_runIndexOverflowSkips{0};
		static std::atomic<int> s_runIndexOverflowPeak{0};
		const uint32_t n = s_runIndexOverflowSkips.fetch_add(
			1, std::memory_order_relaxed);
		const int peakIdx = (srcIndex > dstIndex) ? srcIndex : dstIndex;
		int prevPeak = s_runIndexOverflowPeak.load(std::memory_order_relaxed);
		while (peakIdx > prevPeak &&
			!s_runIndexOverflowPeak.compare_exchange_weak(
				prevPeak, peakIdx, std::memory_order_relaxed))
		{
		}

		if (n < 32 || (n % 256) == 0)
		{
			void* const retAddr = _ReturnAddress();
			char callerBuf[64];
			SGE_FormatPtrRva(callerBuf, sizeof(callerBuf), retAddr);
			Warning(eDLL_T::SERVER,
				"[FRAME-SNAP-GUARD] RUN-INDEX-OVERFLOW skip#%u srcIndex=%d "
				"dstIndex=%d peakIdx=%d (run backing 0..%d; >=%d overruns "
				"0x650C0 bucket) caller=%s\n",
				n + 1, srcIndex, dstIndex,
				s_runIndexOverflowPeak.load(std::memory_order_relaxed),
				kSnapRunIndexCap - 1, kSnapRunIndexCap,
				callerBuf);
		}
	}

	if (sdk_snap_merge_guard.GetBool() && !bad)
	{
		lightBucket = SGE_IsLightResetBucket(srcBucket) ||
			SGE_IsLightResetBucket(dstBucket);
		if (lightBucket)
			bad = true;
	}

	if (sdk_snap_merge_guard.GetBool() && !bad)
	{
		bad = !SGE_TryReadU64(srcBucket + 8, srcHead) ||
			!SGE_TryReadU64(srcBucket + 0x10, srcRuns) ||
			!SGE_TryReadU64(srcBucket + 0x18, srcProps) ||
			!SGE_TryReadU64(dstBucket + 8, dstHead) ||
			!SGE_TryReadU64(dstBucket + 0x10, dstRuns) ||
			!SGE_TryReadU64(dstBucket + 0x18, dstProps) ||
			!SGE_LooksLikeUserPointer(static_cast<uintptr_t>(srcHead)) ||
			!SGE_LooksLikeUserPointer(static_cast<uintptr_t>(srcRuns)) ||
			!SGE_LooksLikeUserPointer(static_cast<uintptr_t>(srcProps)) ||
			!SGE_LooksLikeUserPointer(static_cast<uintptr_t>(dstHead)) ||
			!SGE_LooksLikeUserPointer(static_cast<uintptr_t>(dstRuns)) ||
			!SGE_LooksLikeUserPointer(static_cast<uintptr_t>(dstProps));
	}

	if (sdk_snap_merge_guard.GetBool() && !bad)
	{
		SGE_TryReadU32(reinterpret_cast<uintptr_t>(cursor), dstPropCursor);
	}

	// snapshot_t: max_props@0 max_dwords@4 hot@8 warm@0x10 cold@0x18.
	// src+0 is not an alloc cap for warm runs; never bound nStart by it.
	if (sdk_snap_merge_guard.GetBool() && !bad)
	{
		const uint32_t* const srcCaps = reinterpret_cast<const uint32_t*>(src);
		const uint32_t* const dstCaps = reinterpret_cast<const uint32_t*>(dst);
		uint32_t srcMaxProps = 0;
		uint32_t srcMaxDwords = 0;
		uint32_t dstMaxProps = 0;
		uint32_t dstMaxDwords = 0;
		const bool capsOk = srcCaps && dstCaps
			&& SGE_TryReadU32(reinterpret_cast<uintptr_t>(srcCaps), srcMaxProps)
			&& SGE_TryReadU32(reinterpret_cast<uintptr_t>(srcCaps + 1), srcMaxDwords)
			&& SGE_TryReadU32(reinterpret_cast<uintptr_t>(dstCaps), dstMaxProps)
			&& SGE_TryReadU32(reinterpret_cast<uintptr_t>(dstCaps + 1), dstMaxDwords)
			&& srcMaxProps >= 16 && srcMaxProps <= 4000000
			&& srcMaxDwords >= 16 && srcMaxDwords <= 8000000
			&& dstMaxProps >= 16 && dstMaxProps <= 4000000
			&& dstMaxDwords >= 16 && dstMaxDwords <= 8000000;

		uint64_t srcWarm = 0;
		uint64_t srcCold = 0;
		uint64_t dstWarm = 0;
		uint64_t dstCold = 0;
		uint64_t srcWarmData = 0;
		uint64_t srcColdData = 0;
		uint64_t dstWarmData = 0;
		uint64_t dstColdData = 0;
		const bool ptrsOk = SGE_TryReadU64(srcBucket + 0x10, srcWarm)
			&& SGE_TryReadU64(srcBucket + 0x18, srcCold)
			&& SGE_TryReadU64(dstBucket + 0x10, dstWarm)
			&& SGE_TryReadU64(dstBucket + 0x18, dstCold)
			&& SGE_LooksLikeUserPointer(srcWarm)
			&& SGE_LooksLikeUserPointer(srcCold)
			&& SGE_LooksLikeUserPointer(dstWarm)
			&& SGE_LooksLikeUserPointer(dstCold)
			&& SGE_TryReadU64(srcWarm + 0x20000, srcWarmData)
			&& SGE_TryReadU64(srcCold + 0x10000, srcColdData)
			&& SGE_TryReadU64(dstWarm + 0x20000, dstWarmData)
			&& SGE_TryReadU64(dstCold + 0x10000, dstColdData)
			&& SGE_LooksLikeUserPointer(srcWarmData)
			&& SGE_LooksLikeUserPointer(srcColdData)
			&& SGE_LooksLikeUserPointer(dstWarmData)
			&& SGE_LooksLikeUserPointer(dstColdData);

		uint32_t nFields = 0;
		uint32_t nStart = 0;
		uint32_t nDwords = 0;
		uint32_t srcBase = 0;
		uint32_t dstBase = 0;
		uint32_t dstPropTotal = 0;

		if (!ptrsOk)
		{
			bad = true;
		}
		else
		{
			uint32_t runLo = 0;
			uint32_t runHi = 0;
			uint64_t dstHot = 0;
			const bool runOk = SGE_TryReadU32(srcWarm + 8ull * static_cast<uint32_t>(srcIndex), runLo)
				&& SGE_TryReadU32(srcWarm + 8ull * static_cast<uint32_t>(srcIndex) + 4, runHi)
				&& SGE_TryReadU32(srcCold + 4ull * static_cast<uint32_t>(srcIndex), srcBase)
				&& SGE_TryReadU32(reinterpret_cast<uintptr_t>(cursor), dstBase)
				&& SGE_TryReadU64(dstBucket + 8, dstHot)
				&& SGE_LooksLikeUserPointer(dstHot)
				&& SGE_TryReadU32(dstHot + 0x14, dstPropTotal);
			nFields = runLo & 0x7FFu;
			nStart = runLo >> 11;
			nDwords = runHi;
			if (!runOk || nDwords > 0x100000u)
			{
				bad = true;
			}
			else if (capsOk
				&& (srcBase + nDwords < srcBase
					|| srcBase + nDwords > srcMaxDwords
					|| dstBase + nDwords < dstBase
					|| dstBase + nDwords > dstMaxDwords
					|| dstPropTotal + nFields < dstPropTotal
					|| dstPropTotal + nFields > dstMaxProps))
			{
				bad = true;
			}
			else
			{
				uint32_t scratch = 0;
				const uintptr_t srcFirst = srcColdData + 4ull * srcBase;
				const uintptr_t srcLast = srcColdData + 4ull * (srcBase + nDwords) - 4ull;
				const uintptr_t dstFirst = dstColdData + 4ull * dstBase;
				const uintptr_t dstLast = dstColdData + 4ull * (dstBase + nDwords) - 4ull;
				const uintptr_t warmSrc = srcWarmData + 8ull * nStart;
				const uintptr_t warmDst = dstWarmData + 8ull * dstPropTotal;
				if (nDwords > 0
					&& (!SGE_TryReadU32(srcFirst, scratch)
						|| !SGE_TryReadU32(srcLast, scratch)
						|| !SGE_TryReadU32(dstFirst, scratch)
						|| !SGE_TryReadU32(dstLast, scratch)))
				{
					bad = true;
				}
				if (nFields > 0
					&& (!SGE_TryReadU32(warmSrc, scratch)
						|| !SGE_TryReadU32(warmDst, scratch)))
				{
					bad = true;
				}
			}
		}

		if (bad)
		{
			const uint32_t n = s_mergeCopyRangeSkips.fetch_add(
				1, std::memory_order_relaxed);
			if (n < 32 || (n % 256) == 0)
			{
				Warning(eDLL_T::SERVER,
					"[FRAME-SNAP-GUARD] copy-range skip#%u src=%p dst=%p "
					"srcIdx=%d dstIdx=%d nDwords=%u nFields=%u nStart=%u "
					"srcBase=%u dstBase=%u dstPropTotal=%u "
					"srcMaxP=%u srcMaxD=%u dstMaxP=%u dstMaxD=%u cursor=%p\n",
					n + 1, src, dst, srcIndex, dstIndex,
					nDwords, nFields, nStart,
					srcBase, dstBase, dstPropTotal,
					srcMaxProps, srcMaxDwords, dstMaxProps, dstMaxDwords,
					cursor);
			}
			return 0;
		}
	}

	if (bad)
	{
		const uint32_t n = (lightBucket ? s_mergeBucketLightSkips :
			s_mergeBucketNullSkips).fetch_add(1, std::memory_order_relaxed);
		if (n < 16 || (n % 256) == 0)
		{
			Warning(eDLL_T::SERVER,
				"[FRAME-SNAP-GUARD] %s skip#%u src=%p dst=%p srcPtrs={%p,%p,%p} "
				"dstPtrs={%p,%p,%p} srcIdx=%d dstIdx=%d cursor=%p cur=%u "
				"caller=%p\n",
				lightBucket ? "light-bucket" : "bad-bucket",
				n + 1, src, dst,
				reinterpret_cast<void*>(srcHead),
				reinterpret_cast<void*>(srcRuns),
				reinterpret_cast<void*>(srcProps),
				reinterpret_cast<void*>(dstHead),
				reinterpret_cast<void*>(dstRuns),
				reinterpret_cast<void*>(dstProps),
				srcIndex, dstIndex, cursor,
				dstPropCursor,
				_ReturnAddress());
		}
		return 0;
	}

	return v_SGE_MergeSnapshotBucket(a1, src, dst, srcIndex, dstIndex, cursor);
}

static void* Hook_SGE_ResetSnapshotBucket(int64_t bucket)
{
	if (!bucket)
		return nullptr;

	uint64_t* backingSlot = reinterpret_cast<uint64_t*>(bucket + 8);
	uint64_t backingValue = 0;
	__try
	{
		backingValue = *backingSlot;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}

	const bool wasLightBucket = SGE_IsLightResetBucket(static_cast<uintptr_t>(bucket));
	const bool badBacking = !SGE_LooksLikeUserPointer(static_cast<uintptr_t>(backingValue));
	if (badBacking || wasLightBucket)
	{
		void* backing = badBacking ? nullptr : reinterpret_cast<void*>(backingValue);
		bool newlyAllocated = false;
		if (!backing)
		{
			backing = SGE_AllocSnapshotBucketBacking();
			if (!backing)
			{
				const uint32_t n = s_resetBucketLightBackfillCount.fetch_add(
					1, std::memory_order_relaxed);
				if (n < 8 || (n % 128) == 0)
				{
					Warning(eDLL_T::SERVER,
						"[FRAME-SNAP-GUARD] reset bucket light backfill FAILED #%u "
						"bucket=%p size=0x%zX caller=%p\n",
						n + 1, reinterpret_cast<void*>(bucket),
						kSnapshotBucketBackingSize, _ReturnAddress());
				}
				return nullptr;
			}
			newlyAllocated = true;

			__try
			{
				*backingSlot = reinterpret_cast<uint64_t>(backing);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
			SGE_MarkLightResetBucket(static_cast<uintptr_t>(bucket));
		}

		SGE_LightInitResetBucketBacking(backing, newlyAllocated);
		const uint32_t n = s_resetBucketLightBackfillCount.fetch_add(
			1, std::memory_order_relaxed);
		if (n < 16 || (n % 128) == 0)
		{
			Warning(eDLL_T::SERVER,
				"[FRAME-SNAP-GUARD] reset bucket light backfill #%u "
				"bucket=%p backing=%p clear=0x%zX fullSize=0x%zX "
				"newAlloc=%d caller=%p\n",
				n + 1, reinterpret_cast<void*>(bucket), backing,
				newlyAllocated ? kSnapshotBucketBackingSize
					: kSnapshotBucketLightClearSize,
				kSnapshotBucketBackingSize, newlyAllocated ? 1 : 0,
				_ReturnAddress());
		}
		return backing;
	}

	return v_SGE_ResetSnapshotBucket(bucket);
}

// Stale dirty bits can have an empty/torn bucket head; skip those edicts.
static bool s_collectChangedPropsBucketFixInstalled = false;

static void* S21Bridge_AllocNear(uintptr_t siteAddr, size_t size)
{
	for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
		 probe < siteAddr + 0x08000000;
		 probe += 0x10000)
	{
		void* p = VirtualAlloc(reinterpret_cast<void*>(probe), size,
			MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (p)
			return p;
	}
	return nullptr;
}

static bool S21Bridge_InstallCollectBucketNullPatch(uint8_t* site)
{
	if (!site)
		return false;
	const uint8_t expected[] = {
		0x4D, 0x8B, 0x50, 0x08,       // mov r10, [r8+8]
		0x45, 0x39, 0x22,             // cmp [r10], r12d
		0x0F, 0x85                    // jnz skip
	};
	if (memcmp(site, expected, sizeof(expected)) != 0)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] SGE dirty-bucket fix byte mismatch at %p\n",
			site);
		return false;
	}

	const uintptr_t siteAddr = reinterpret_cast<uintptr_t>(site);
	const uintptr_t continueTarget = siteAddr + 13;
	const uintptr_t skipTarget =
		continueTarget + *reinterpret_cast<int32_t*>(site + 9);

	// Torn table bases can manufacture unmapped pointers; skip stale dirty entries.
	{
		constexpr int kCollectBucketSkipCaveSize = 96;
		uint8_t* cave = reinterpret_cast<uint8_t*>(
			S21Bridge_AllocNear(siteAddr, kCollectBucketSkipCaveSize));
		if (!cave)
		{
			Warning(eDLL_T::SERVER,
				"[s21-bridge] could not allocate skip cave for SGE "
				"dirty-bucket fix\n");
			return false;
		}

		int pos = 0;
		auto emitJccToSkip = [&](uint8_t cc)
		{
			cave[pos++] = 0x0F; cave[pos++] = cc;
			int32_t r = static_cast<int32_t>(skipTarget -
				(reinterpret_cast<uintptr_t>(cave) + pos + 4));
			memcpy(cave + pos, &r, 4); pos += 4;
		};

		cave[pos++] = 0x4D; cave[pos++] = 0x8B; cave[pos++] = 0x50; cave[pos++] = 0x08; // mov r10, [r8+8]
		cave[pos++] = 0x4D; cave[pos++] = 0x85; cave[pos++] = 0xD2; // test r10, r10
		emitJccToSkip(0x84); // jz skipTarget
		cave[pos++] = 0x41; cave[pos++] = 0xF6; cave[pos++] = 0xC2; cave[pos++] = 0x07; // test r10b, 7
		emitJccToSkip(0x85); // jnz skipTarget

		cave[pos++] = 0x48; cave[pos++] = 0xB8; // mov rax, minUserPtr
		uint64_t minUserPtr = 0x10000ULL;
		memcpy(cave + pos, &minUserPtr, sizeof(minUserPtr)); pos += 8;
		cave[pos++] = 0x49; cave[pos++] = 0x39; cave[pos++] = 0xC2; // cmp r10, rax
		emitJccToSkip(0x82); // jb skipTarget

		// Real heap/module pointers in the S21 process sit far below this.
		// Keep the ceiling tight so torn values like 0x00000FFF00000000 skip.
		cave[pos++] = 0x48; cave[pos++] = 0xB8; // mov rax, maxLikelyHeapPtr
		uint64_t maxUserPtr = 0x0000080000000000ULL;
		memcpy(cave + pos, &maxUserPtr, sizeof(maxUserPtr)); pos += 8;
		cave[pos++] = 0x49; cave[pos++] = 0x39; cave[pos++] = 0xC2; // cmp r10, rax
		emitJccToSkip(0x83); // jae skipTarget

		cave[pos++] = 0x45; cave[pos++] = 0x39; cave[pos++] = 0x22; // cmp [r10], r12d
		emitJccToSkip(0x85); // jnz skipTarget

		cave[pos++] = 0xE9;
		int32_t rel = static_cast<int32_t>(continueTarget -
			(reinterpret_cast<uintptr_t>(cave) + pos + 4));
		memcpy(cave + pos, &rel, 4); pos += 4;

		if (pos > kCollectBucketSkipCaveSize)
		{
			Warning(eDLL_T::SERVER,
				"[s21-bridge] SGE dirty-bucket skip cave overflow "
				"(%d > %d)\n", pos, kCollectBucketSkipCaveSize);
			return false;
		}

		DWORD oldProt;
		if (!VirtualProtect(site, 13, PAGE_EXECUTE_READWRITE, &oldProt))
		{
			Warning(eDLL_T::SERVER,
				"[s21-bridge] SGE dirty-bucket skip VirtualProtect "
				"failed at %p\n", site);
			return false;
		}
		site[0] = 0xE9;
		rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(cave) -
			(siteAddr + 5));
		memcpy(site + 1, &rel, 4);
		memset(site + 5, 0x90, 8);
		VirtualProtect(site, 13, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), site, 13);
		return true;
	}

}

static void S21Bridge_InstallCollectChangedPropsBucketFix()
{
	if (s_collectChangedPropsBucketFixInstalled || !v_SGE_CollectChangedProps)
		return;

	uint8_t* fn = reinterpret_cast<uint8_t*>(v_SGE_CollectChangedProps);
	uint8_t* first = fn + 0x81;
	uint8_t* second = fn + 0x1A0;
	const bool ok1 = S21Bridge_InstallCollectBucketNullPatch(first);
	const bool ok2 = S21Bridge_InstallCollectBucketNullPatch(second);
	s_collectChangedPropsBucketFixInstalled = ok1 && ok2;
	if (s_collectChangedPropsBucketFixInstalled)
	{
		Msg(eDLL_T::SERVER,
			"[s21-bridge] installed SGE dirty-bucket null-skip fix "
			"at %p and %p\n", first, second);
	}
}

struct SGE_PackMeter
{
	uint32_t nFields;
	uint32_t nDwords;
	uint32_t staged;
	uint32_t extraTotal;
	LONG valid;
};
static SGE_PackMeter s_meterWorld;
static SGE_PackMeter s_meterPlayer;
static SGE_PackMeter s_meterGnr;

struct SGE_PackCopy
{
	uint32_t nFields;
	uint32_t nDwords;
	uint32_t rec[2048];
	uint32_t cells[16384];
	LONG valid;
};

struct SGE_PackSlot
{
	SGE_PackCopy latest;
	SGE_PackCopy full;
};
static SGE_PackSlot s_slotWorld;
static SGE_PackSlot s_slotPlayer;
static SGE_PackSlot s_slotGnr;

static SGE_PackMeter* SGE_MeterForTable(const char* tableName)
{
	if (!tableName)
		return nullptr;
	if (strcmp(tableName, "DT_WORLD") == 0 || strcmp(tableName, "DT_World") == 0)
		return &s_meterWorld;
	if (strcmp(tableName, "DT_Player") == 0)
		return &s_meterPlayer;
	if (strcmp(tableName, "DT_GlobalNonRewinding") == 0)
		return &s_meterGnr;
	return nullptr;
}

static SGE_PackSlot* SGE_PackSlotForTable(const char* tableName)
{
	if (!tableName)
		return nullptr;
	if (strcmp(tableName, "DT_WORLD") == 0 || strcmp(tableName, "DT_World") == 0)
		return &s_slotWorld;
	if (strcmp(tableName, "DT_Player") == 0)
		return &s_slotPlayer;
	if (strcmp(tableName, "DT_GlobalNonRewinding") == 0)
		return &s_slotGnr;
	return nullptr;
}

static int SGE_PackIsSequential(const SGE_PackCopy& p)
{
	if (!p.valid || p.nFields == 0 || p.nFields > 2048)
		return 0;
	for (uint32_t i = 0; i < p.nFields; ++i)
	{
		if ((p.rec[i] & 0x7FFu) != i)
			return 0;
	}
	return 1;
}

static void SGE_StorePackCopy(const char* tableName, uint32_t nFields, uint32_t nDwords,
	uintptr_t recBlob, uint32_t recBase, uintptr_t cellBlob, uint32_t cellOff)
{
	SGE_PackSlot* slot = SGE_PackSlotForTable(tableName);
	if (!slot || nFields == 0 || nFields > 2048 || nDwords == 0 || nDwords > 16384)
		return;
	slot->latest.nFields = nFields;
	slot->latest.nDwords = nDwords;
	for (uint32_t i = 0; i < nFields; ++i)
	{
		uint32_t rec = 0;
		SGE_TryReadU32(recBlob + 8ull * (recBase + i), rec);
		slot->latest.rec[i] = rec;
	}
	for (uint32_t i = 0; i < nDwords; ++i)
	{
		uint32_t v = 0;
		SGE_TryReadU32(cellBlob + 4ull * (cellOff + i), v);
		slot->latest.cells[i] = v;
	}
	slot->latest.valid = 1;
	if (SGE_PackIsSequential(slot->latest)
		&& (!slot->full.valid || nFields >= slot->full.nFields))
		slot->full = slot->latest;
}

static void SGE_EmitPackVsEnc(const char* tag, const SGE_PackMeter& meter, int encodeCells)
{
	if (!meter.valid)
	{
		Warning(eDLL_T::SERVER, "[PACK-VS-ENC] %s (no pack)\n", tag);
		return;
	}
	Warning(eDLL_T::SERVER,
		"[PACK-VS-ENC] %s nFields=%u nDwords=%u staged=%u extra=%u encode=%d "
		"dwords-enc=%d staged-enc=%d\n",
		tag, meter.nFields, meter.nDwords, meter.staged, meter.extraTotal, encodeCells,
		static_cast<int>(meter.nDwords) - encodeCells,
		static_cast<int>(meter.staged) - encodeCells);
}

static constexpr ptrdiff_t kSgeDescBlobOff    = 32960;
static constexpr ptrdiff_t kSgeDescEntriesOff = 1216;

static uintptr_t SGE_DescBlob(void)
{
	const uintptr_t mgr = SnapshotRing_Mgr();
	return mgr ? (mgr + kSgeDescBlobOff) : 0;
}

static int SGE_DescRuleCells(uint32_t descType, uint32_t cell0)
{
	switch (descType)
	{
	case 0: case 1: case 8: case 9:
		return 1;
	case 2:
		return 3;
	case 3: case 7:
		return 2;
	case 6:
		return 4;
	case 4:
		return (cell0 <= 511u) ? static_cast<int>(1u + (cell0 + 3u) / 4u) : -1;
	default:
		return 0;
	}
}

static void SGE_ClampFixedWidthPackRecords(uint64_t* snap, int16_t entIdx,
	int64_t sendTable)
{
	if (!snap || entIdx < 0)
		return;

	const uintptr_t recTab = S3_RdQ(reinterpret_cast<uintptr_t>(snap) + 16);
	const uintptr_t cellTab = S3_RdQ(reinterpret_cast<uintptr_t>(snap) + 24);
	if (!recTab || !cellTab)
		return;

	uint32_t hdr = 0, nDwords = 0;
	if (!SGE_TryReadU32(recTab + 8ull * static_cast<uint32_t>(entIdx), hdr)
		|| !SGE_TryReadU32(recTab + 8ull * static_cast<uint32_t>(entIdx) + 4, nDwords))
		return;

	const uint32_t nFields = hdr & 0x7FFu;
	const uint32_t recBase = hdr >> 11;
	if (nFields == 0 || nFields > 4096 || nDwords > 65536)
		return;

	uint64_t recBlob = 0, cellBlob = 0;
	uint32_t cellOff = 0;
	if (!SGE_TryReadU64(recTab + 0x20000, recBlob)
		|| !SGE_TryReadU64(cellTab + 0x10000, cellBlob)
		|| !SGE_TryReadU32(cellTab + 4ull * static_cast<uint32_t>(entIdx), cellOff))
		return;
	if (!recBlob || !cellBlob || !S3_PtrLooksHeap(recBlob) || !S3_PtrLooksHeap(cellBlob))
		return;

	uintptr_t flatArr = 0;
	uint32_t flatN = 0;
	const char* tableName = nullptr;
	if (sendTable && S3_PtrLooksHeap(static_cast<uintptr_t>(sendTable)))
	{
		const uintptr_t st = S3_RdQ(static_cast<uintptr_t>(sendTable) + 8);
		const uintptr_t nm = st ? S3_RdQ(st + 0x4B8) : 0;
		if (nm)
			tableName = reinterpret_cast<const char*>(nm);
		const uintptr_t pc = st ? S3_RdQ(st + kS3_ST_Precalc) : 0;
		if (pc && S3_PtrLooksHeap(pc))
		{
			flatArr = S3_RdQ(pc + 8);
			uint32_t n = 0;
			if (SGE_TryReadU32(pc + kS3_PC_FlatCount, n) && n <= 4096)
				flatN = n;
		}
	}

	// [PACK-VS-DESC] resolve this class's encode-descriptor run once; each
	// record's staged cell count is then checked against the width the
	// svc_Snapshot encoder will actually consume for that flat.
	const uintptr_t descBlob = SGE_DescBlob();
	uint32_t descBase = 0;
	bool descOk = false;
	if (descBlob && sendTable && S3_PtrLooksHeap(static_cast<uintptr_t>(sendTable)))
	{
		const int cid = S3_RdD(static_cast<uintptr_t>(sendTable) + kS3_SC_ClassID);
		if (cid >= 0 && cid < 1024)
		{
			uint32_t chdr = 0;
			if (SGE_TryReadU32(descBlob + 8ull * static_cast<uint32_t>(cid), chdr)
				&& (chdr & 0x7FFu))
			{
				descBase = chdr >> 11;
				descOk = true;
			}
		}
	}
	const bool dumpPack = bridge_team_enc_diag.GetBool() && tableName
		&& (strcmp(tableName, "DT_Player") == 0
			|| strcmp(tableName, "DT_WORLD") == 0);
	static std::atomic<uint32_t> s_packDumpN{ 0 };

	const bool census = sdk_snap_pack_census.GetBool();
	const bool clampOn = sdk_snap_fixed_cell_clamp.GetBool();
	const bool alignOn = sdk_snap_prefix_skip.GetBool();
	if (!clampOn && !census && !alignOn)
		return;
	uint32_t cellCursor = 0;
	uint32_t extraTotal = 0;
	static std::atomic<uint32_t> s_clampN{ 0 };
	static std::atomic<uint32_t> s_gnrDumpN{ 0 };
	const bool dumpGnr = census && tableName && strcmp(tableName, "DT_GlobalNonRewinding") == 0
		&& s_gnrDumpN.load(std::memory_order_relaxed) < 4;

	for (uint32_t i = 0; i < nFields; ++i)
	{
		uint32_t rec = 0;
		if (!SGE_TryReadU32(recBlob + 8ull * (recBase + i), rec))
			break;
		const uint32_t fi = rec & 0x7FFu;
		const uint32_t cells = (rec >> 13) & 0xFFu;
		uint32_t ty = 0xFFFFu;
		const char* pn = nullptr;
		if (flatArr && S3_PtrLooksHeap(flatArr) && fi < flatN)
		{
			const uintptr_t sp = S3_RdQ(flatArr + 8ull * fi);
			if (sp && S3_PtrLooksHeap(sp))
			{
				SGE_TryReadU32(sp, ty);
				const uintptr_t nmp = S3_RdQ(sp + 0x40);
				if (nmp)
					pn = reinterpret_cast<const char*>(nmp);
			}
		}

		if (census && descOk)
		{
			uint32_t desc = 0;
			SGE_TryReadU32(descBlob + kSgeDescEntriesOff + 4ull * (descBase + fi),
				desc);
			const uint32_t dType = desc >> 24;
			uint32_t c0 = 0;
			if (dType == 4 || dumpPack)
				SGE_TryReadU32(cellBlob + 4ull * (cellOff + cellCursor), c0);
			const int ruleCells = SGE_DescRuleCells(dType, c0);
			if (ruleCells > 0 && cells != static_cast<uint32_t>(ruleCells))
			{
				static std::atomic<uint32_t> s_pvdN{ 0 };
				if (s_pvdN.fetch_add(1, std::memory_order_relaxed) < 32)
					Warning(eDLL_T::SERVER,
						"[PACK-VS-DESC] %s ent=%d flat=%u '%s' spType=%u "
						"descType=%u staged=%u ruleCells=%d cell0=0x%08X\n",
						tableName ? tableName : "?", entIdx, fi, pn ? pn : "?",
						ty, dType, cells, ruleCells, c0);
			}
			if (dumpPack && i < 8
				&& s_packDumpN.fetch_add(1, std::memory_order_relaxed) < 64)
				Warning(eDLL_T::SERVER,
					"[PACK-DUMP] %s ent=%d rec[%u] flat=%u '%s' spType=%u "
					"descType=%u cells=%u cell0=0x%08X\n",
					tableName ? tableName : "?", entIdx, i, fi, pn ? pn : "?",
					ty, dType, cells, c0);
		}

		int expect = 0;
		if (ty == 0 || ty == 1 || ty == 8 || ty == 9) expect = 1;
		else if (ty == 2) expect = 3;
		else if (ty == 3 || ty == 7) expect = 2;
		else if (ty == 6) expect = 4;
		const bool mismatch = (expect > 0 && cells != static_cast<uint32_t>(expect));
		const bool dumpPlayer = census && tableName && strcmp(tableName, "DT_Player") == 0
			&& s_gnrDumpN.load(std::memory_order_relaxed) < 24;
		const bool fixedOne = (ty == 0 || ty == 1 || ty == 8 || ty == 9);
		if (census && ((dumpGnr && (fi == 255 || cells != 1 || !fixedOne))
			|| (dumpPlayer && mismatch)
			|| (mismatch && expect == 1)))
		{
			s_gnrDumpN.fetch_add(1, std::memory_order_relaxed);
			uint32_t cell0 = 0, cell1 = 0;
			SGE_TryReadU32(cellBlob + 4ull * (cellOff + cellCursor), cell0);
			if (cells > 1)
				SGE_TryReadU32(cellBlob + 4ull * (cellOff + cellCursor + 1), cell1);
			Warning(eDLL_T::SERVER,
				"[PACK-CELL] %s rec[%u] flat=%u '%s' type=%u cells=%u "
				"cell0=0x%08X cell1=0x%08X nFields=%u nDwords=%u\n",
				tableName ? tableName : "?", i, fi, pn ? pn : "?", ty, cells,
				cell0, cell1, nFields, nDwords);
		}

		if (fixedOne && cells > 1)
		{
			uint32_t extra = cells - 1;
			const uint32_t n = s_clampN.fetch_add(1, std::memory_order_relaxed);
			if (n < 16)
				Warning(eDLL_T::SERVER,
					"[PACK-CELL] %s%s flat=%u '%s' type=%u cellsStaged=%u -> 1 "
					"(orphan would shear the next body)\n",
					clampOn ? "CLAMP " : "WOULD-CLAMP ",
					tableName ? tableName : "?", fi, pn ? pn : "?", ty, cells);
			if (clampOn)
			{
				const uint32_t rel = cellCursor + cells;
				if (rel <= nDwords && extra > 0)
				{
					const uint32_t maxExtra = nDwords - (cellCursor + 1);
					if (extra > maxExtra)
						extra = maxExtra;
					const uint32_t src = cellOff + cellCursor + cells;
					const uint32_t dst = cellOff + cellCursor + 1;
					if (src <= cellOff + nDwords && dst < src)
					{
						const uint32_t remain = (cellOff + nDwords) - src;
						if (remain)
							memmove(reinterpret_cast<void*>(cellBlob + 4ull * dst),
								reinterpret_cast<const void*>(cellBlob + 4ull * src),
								4ull * remain);
					}
					rec = (rec & ~0x1FE000u) | (1u << 13);
					*reinterpret_cast<uint32_t*>(recBlob + 8ull * (recBase + i)) = rec;
					nDwords -= extra;
					extraTotal += extra;
				}
			}
			cellCursor += clampOn ? 1u : cells;
			continue;
		}
		cellCursor += cells;
	}

	if (cellCursor != nDwords)
	{
		static std::atomic<uint32_t> s_sumDriftN{ 0 };
		if (s_sumDriftN.fetch_add(1, std::memory_order_relaxed) < 16)
			Warning(eDLL_T::SERVER,
				"[PACK-SUMDRIFT] %s ent=%d walked=%u nDwords=%u nFields=%u\n",
				tableName ? tableName : "?", entIdx, cellCursor, nDwords, nFields);
	}

	if (extraTotal)
	{
		*reinterpret_cast<uint32_t*>(recTab + 8ull * static_cast<uint32_t>(entIdx) + 4)
			= nDwords;
		const uintptr_t hdrBlk = S3_RdQ(reinterpret_cast<uintptr_t>(snap) + 8);
		if (hdrBlk && S3_PtrLooksHeap(hdrBlk))
		{
			uint32_t runCells = 0;
			if (SGE_TryReadU32(hdrBlk + 24, runCells) && runCells >= extraTotal)
				*reinterpret_cast<uint32_t*>(hdrBlk + 24) = runCells - extraTotal;
		}
	}

	if ((census || alignOn) && tableName)
	{
		if (census)
		{
			SGE_PackMeter* slot = SGE_MeterForTable(tableName);
			if (slot)
			{
				slot->nFields = nFields;
				slot->nDwords = nDwords;
				slot->staged = cellCursor;
				slot->extraTotal = extraTotal;
				slot->valid = 1;
			}
		}
		const bool bGnr = (strcmp(tableName, "DT_GlobalNonRewinding") == 0);
		if (census || !bGnr)
			SGE_StorePackCopy(tableName, nFields, nDwords, recBlob, recBase,
				cellBlob, cellOff);
	}
}

// Pool ctor args size the record/cell blobs; packers do not bounds-check.
// Grow the args; the 16384-entry run/inline regions are indexed separately.
static ConVar sdk_snap_pool_grow("sdk_snap_pool_grow", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Grow the under-sized snapshot pool backings at construct (baseline pool, "
	"staging buckets, worker blocks). 0 = stock sizes (packers overrun them "
	"on maps with extended tables).");

static ConVar sdk_snap_pool_canary("sdk_snap_pool_canary", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Stamp and verify overrun canaries on the grown snapshot pool blobs.");

static constexpr uint64_t s_snapCanaryLo = 0x53504F4F4C434E31ull;
static constexpr uint64_t s_snapCanaryHi = 0x53504F4F4C434E32ull;

struct SnapPoolCanaryEntry_t
{
	uint64_t* pPool;
	int nCount;
	int nA3;
	int nA4;
	int nN3;
	int nN4;
	bool bStamped;
	bool bReqEndValid;
};

static constexpr int s_snapCanaryMax = 8;
static SnapPoolCanaryEntry_t s_snapCanaryTable[s_snapCanaryMax];
static int s_snapCanaryCount = 0;
static std::mutex s_snapCanaryMutex;
static std::atomic<uint32_t> s_snapCanaryWarnN{ 0 };

static bool SnapPoolCanary_WarnBudget(void)
{
	return s_snapCanaryWarnN.fetch_add(1, std::memory_order_relaxed) < 8;
}

static void SnapPool_WritebackRequestedCaps(uint64_t* pool, int count, int a3, int a4)
{
	if (!pool || !Mem_IsReadable(pool, 200))
		return;

	const int nStored = *reinterpret_cast<const int*>(pool);
	if (nStored <= 0)
		return;

	const int nLoop = (count < nStored) ? count : nStored;
	uint8_t* const pBase = reinterpret_cast<uint8_t*>(pool);
	void* const pDesc = *reinterpret_cast<void**>(pBase + 64);   // descriptor array
	void* const pWarm = *reinterpret_cast<void**>(pBase + 192);  // warm element array
	if (!pDesc || !pWarm)
		return;
	if (!Mem_IsReadable(pDesc, static_cast<size_t>(nLoop) * 32) ||
		!Mem_IsReadable(pWarm, static_cast<size_t>(nLoop) * 0x20080))
		return;

	uint8_t* const desc = reinterpret_cast<uint8_t*>(pDesc);
	uint8_t* const warm = reinterpret_cast<uint8_t*>(pWarm);
	for (int i = 0; i < nLoop; ++i)
	{
		*reinterpret_cast<int*>(desc + 32 * i + 0) = a3;
		*reinterpret_cast<int*>(desc + 32 * i + 4) = a4;
		*reinterpret_cast<int*>(warm + 0x20080 * static_cast<size_t>(i) + 0x20040) = a3;
	}
}

static void SnapPool_StampBlobCanary(uint8_t* blob, int64_t reqBytes, int64_t grownBytes,
	bool bReqEndValid, bool* pStampedAny)
{
	if (!blob || grownBytes < 16)
		return;

	const int64_t reqOff = reqBytes;
	const int64_t tailOff = grownBytes - 16;
	const bool bReqFits = (reqOff >= 0) && (reqOff + 16 <= grownBytes);
	const bool bTailFits = (tailOff >= 0) && (tailOff + 16 <= grownBytes);
	bool bStampReq = bReqEndValid && bReqFits;
	bool bStampTail = bTailFits;
	if (bStampReq && bStampTail)
	{
		const bool bOverlap = !(reqOff + 16 <= tailOff || tailOff + 16 <= reqOff);
		if (bOverlap)
			bStampTail = false;
	}

	if (bStampReq && Mem_IsReadable(blob + reqOff, 16))
	{
		uint64_t* const p = reinterpret_cast<uint64_t*>(blob + reqOff);
		p[0] = s_snapCanaryLo;
		p[1] = s_snapCanaryHi;
		*pStampedAny = true;
	}
	if (bStampTail && Mem_IsReadable(blob + tailOff, 16))
	{
		uint64_t* const p = reinterpret_cast<uint64_t*>(blob + tailOff);
		p[0] = s_snapCanaryLo;
		p[1] = s_snapCanaryHi;
		*pStampedAny = true;
	}
}

static void SnapPool_StampCanaries(uint64_t* pool, int count, int a3, int a4, int n3, int n4,
	bool bReqEndValid)
{
	if (!pool || count <= 0)
		return;
	if (!Mem_IsReadable(pool, 264))
		return;

	const int nStored = *reinterpret_cast<const int*>(pool);
	if (nStored <= 0)
		return;

	const int nLoop = (count < nStored) ? count : nStored;
	uint8_t* const pBase = reinterpret_cast<uint8_t*>(pool);
	void* const pWarm = *reinterpret_cast<void**>(pBase + 192); // warm element array
	void* const pCold = *reinterpret_cast<void**>(pBase + 256); // cold element array
	if (!pWarm || !pCold)
		return;
	if (!Mem_IsReadable(pWarm, static_cast<size_t>(nLoop) * 0x20080) ||
		!Mem_IsReadable(pCold, static_cast<size_t>(nLoop) * 0x10040))
		return;

	uint8_t* const warm = reinterpret_cast<uint8_t*>(pWarm);
	uint8_t* const cold = reinterpret_cast<uint8_t*>(pCold);
	bool bStampedAny = false;
	const int64_t recReq = static_cast<int64_t>(a3) * 8;
	const int64_t recGrown = static_cast<int64_t>(n3) * 8;
	const int64_t cellReq = static_cast<int64_t>(a4) * 4;
	const int64_t cellGrown = static_cast<int64_t>(n4) * 4;

	for (int i = 0; i < nLoop; ++i)
	{
		uint8_t* const recBlob = *reinterpret_cast<uint8_t**>(
			warm + 0x20080 * static_cast<size_t>(i) + 0x20000);
		uint8_t* const cellBlob = *reinterpret_cast<uint8_t**>(
			cold + 0x10040 * static_cast<size_t>(i) + 0x10000);
		SnapPool_StampBlobCanary(recBlob, recReq, recGrown, bReqEndValid, &bStampedAny);
		SnapPool_StampBlobCanary(cellBlob, cellReq, cellGrown, bReqEndValid, &bStampedAny);
	}

	{
		std::lock_guard<std::mutex> lock(s_snapCanaryMutex);
		int nSlot = -1;
		for (int i = 0; i < s_snapCanaryCount; ++i)
		{
			if (s_snapCanaryTable[i].pPool == pool)
			{
				nSlot = i;
				break;
			}
		}
		if (nSlot < 0)
		{
			if (s_snapCanaryCount >= s_snapCanaryMax)
			{
				static bool s_tableFullWarned = false;
				if (!s_tableFullWarned)
				{
					s_tableFullWarned = true;
					Warning(eDLL_T::SERVER,
						"[SNAP-CANARY] canary table full (%d); dropping pool=%p\n",
						s_snapCanaryMax, (void*)pool);
				}
				return;
			}
			nSlot = s_snapCanaryCount++;
		}
		SnapPoolCanaryEntry_t& e = s_snapCanaryTable[nSlot];
		e.pPool = pool;
		e.nCount = count;
		e.nA3 = a3;
		e.nA4 = a4;
		e.nN3 = n3;
		e.nN4 = n4;
		e.bStamped = bStampedAny;
		e.bReqEndValid = bReqEndValid;
	}
}

static bool SnapPool_CheckCanarySite(uint64_t* pool, uint8_t* blob, int64_t off,
	int elem, const char* pszBlob, const char* pszSite, int reqCap, int grownCap)
{
	if (!blob || off < 0)
		return true;
	uint8_t* const pSite = blob + off;
	if (!Mem_IsReadable(pSite, 16))
		return true;

	const uint64_t* const p = reinterpret_cast<const uint64_t*>(pSite);
	if (p[0] == s_snapCanaryLo && p[1] == s_snapCanaryHi)
		return true;

	if (SnapPoolCanary_WarnBudget())
	{
		const uint32_t* const dw = reinterpret_cast<const uint32_t*>(pSite);
		Warning(eDLL_T::SERVER,
			"[SNAP-CANARY] pool=%p elem=%d %s blob overrun: requested cap %d, "
			"grown cap %d, site=%s, got %08X %08X %08X %08X\n",
			(void*)pool, elem, pszBlob, reqCap, grownCap, pszSite,
			dw[0], dw[1], dw[2], dw[3]);
	}
	return false;
}

static void SnapPool_ClearCanaryEntry(uint64_t* pool)
{
	std::lock_guard<std::mutex> lock(s_snapCanaryMutex);
	for (int i = 0; i < s_snapCanaryCount; ++i)
	{
		if (s_snapCanaryTable[i].pPool != pool)
			continue;
		s_snapCanaryTable[i] = s_snapCanaryTable[s_snapCanaryCount - 1];
		s_snapCanaryTable[s_snapCanaryCount - 1] = {};
		--s_snapCanaryCount;
		return;
	}
}

static void SnapPool_VerifyCanaries(uint64_t* pool)
{
	SnapPoolCanaryEntry_t e{};
	{
		std::lock_guard<std::mutex> lock(s_snapCanaryMutex);
		int nSlot = -1;
		for (int i = 0; i < s_snapCanaryCount; ++i)
		{
			if (s_snapCanaryTable[i].pPool == pool)
			{
				nSlot = i;
				break;
			}
		}
		if (nSlot < 0)
			return;
		e = s_snapCanaryTable[nSlot];
	}

	if (e.bStamped && e.pPool && Mem_IsReadable(e.pPool, 264))
	{
		const int nStored = *reinterpret_cast<const int*>(e.pPool);
		if (nStored > 0)
		{
			const int nLoop = (e.nCount < nStored) ? e.nCount : nStored;
			uint8_t* const pBase = reinterpret_cast<uint8_t*>(e.pPool);
			void* const pWarm = *reinterpret_cast<void**>(pBase + 192);
			void* const pCold = *reinterpret_cast<void**>(pBase + 256);
			if (pWarm && pCold &&
				Mem_IsReadable(pWarm, static_cast<size_t>(nLoop) * 0x20080) &&
				Mem_IsReadable(pCold, static_cast<size_t>(nLoop) * 0x10040))
			{
				uint8_t* const warm = reinterpret_cast<uint8_t*>(pWarm);
				uint8_t* const cold = reinterpret_cast<uint8_t*>(pCold);
				const int64_t recReq = static_cast<int64_t>(e.nA3) * 8;
				const int64_t recGrown = static_cast<int64_t>(e.nN3) * 8;
				const int64_t cellReq = static_cast<int64_t>(e.nA4) * 4;
				const int64_t cellGrown = static_cast<int64_t>(e.nN4) * 4;
				bool bRecStampReq = e.bReqEndValid && (recReq >= 0) && (recReq + 16 <= recGrown);
				bool bRecStampTail = (recGrown >= 16);
				if (bRecStampReq && bRecStampTail)
				{
					const int64_t tailOff = recGrown - 16;
					if (!(recReq + 16 <= tailOff || tailOff + 16 <= recReq))
						bRecStampTail = false;
				}
				bool bCellStampReq = e.bReqEndValid && (cellReq >= 0) && (cellReq + 16 <= cellGrown);
				bool bCellStampTail = (cellGrown >= 16);
				if (bCellStampReq && bCellStampTail)
				{
					const int64_t tailOff = cellGrown - 16;
					if (!(cellReq + 16 <= tailOff || tailOff + 16 <= cellReq))
						bCellStampTail = false;
				}

				for (int i = 0; i < nLoop; ++i)
				{
					uint8_t* const recBlob = *reinterpret_cast<uint8_t**>(
						warm + 0x20080 * static_cast<size_t>(i) + 0x20000);
					uint8_t* const cellBlob = *reinterpret_cast<uint8_t**>(
						cold + 0x10040 * static_cast<size_t>(i) + 0x10000);

					if (bRecStampReq &&
						!SnapPool_CheckCanarySite(e.pPool, recBlob, recReq, i, "record",
							"reqEnd", e.nA3, e.nN3))
						break;
					if (bRecStampTail &&
						!SnapPool_CheckCanarySite(e.pPool, recBlob, recGrown - 16, i,
							"record", "tail", e.nA3, e.nN3))
						break;
					if (bCellStampReq &&
						!SnapPool_CheckCanarySite(e.pPool, cellBlob, cellReq, i, "cell",
							"reqEnd", e.nA4, e.nN4))
						break;
					if (bCellStampTail &&
						!SnapPool_CheckCanarySite(e.pPool, cellBlob, cellGrown - 16, i,
							"cell", "tail", e.nA4, e.nN4))
						break;
				}
			}
		}
	}

	SnapPool_ClearCanaryEntry(pool);
}

static void Hook_SGE_SnapPoolDtor(uint64_t* pool)
{
	if (sdk_snap_pool_canary.GetBool() && pool)
		SnapPool_VerifyCanaries(pool);
	v_SGE_SnapPoolDtor(pool);
}

static void Hook_SGE_SnapPoolCtor(uint64_t* pool, int count, int a3, int a4)
{
	int n3 = a3, n4 = a4;
	bool bWritebackCaps = false;
	if (sdk_snap_pool_grow.GetBool())
	{
		if (count == 1 && a3 == 0x4000 && a4 == 0x4000)
		{
			n3 = 0x40000;
			n4 = 0x40000;
		}
		else if (count > 1 && count != 256 && a3 == 0x1000 && a4 == 0x1000)
		{
			n3 = 0x8000;
			n4 = 0x8000;
		}
		else if (count == 256)
		{
			if (n3 < 0x4000)  n3 = 0x4000;
			if (n4 < 0x10000) n4 = 0x10000;
			bWritebackCaps = true;
		}
		else if (count == 1)
		{
			if (a3 > 0)
			{
				int64_t t3 = static_cast<int64_t>(a3) * 4;
				if (t3 < 0x8000)
					t3 = 0x8000;
				if (t3 > 0x100000)
					t3 = 0x100000;
				n3 = static_cast<int>(t3);
				if (n3 < a3)
					n3 = a3;
			}
			if (a4 > 0)
			{
				int64_t t4 = static_cast<int64_t>(a4) * 4;
				if (t4 < 0x20000)
					t4 = 0x20000;
				if (t4 > 0x400000)
					t4 = 0x400000;
				n4 = static_cast<int>(t4);
				if (n4 < a4)
					n4 = a4;
			}
			bWritebackCaps = true;
		}
	}
	const bool bGrew = (n3 != a3 || n4 != a4);
	if (bGrew)
	{
		// A saturated counter reports the cap, not the truth -- say so on the
		// last line so a ramp that grew more than this is never read as 64.
		static constexpr uint32_t kGrowLogCap = 64;
		static std::atomic<uint32_t> s_growN{ 0 };
		const uint32_t growN = s_growN.fetch_add(1, std::memory_order_relaxed) + 1;
		if (growN <= kGrowLogCap)
			Warning(eDLL_T::SERVER,
				"[SNAP-POOL] grow #%u pool=%p count=%d records %d->%d cells %d->%d\n",
				growN, (void*)pool, count, a3, n3, a4, n4);
		else if (growN == kGrowLogCap + 1)
			Warning(eDLL_T::SERVER,
				"[SNAP-POOL] grow log capped at %u -- further grows are SILENT, "
				"the printed count is a floor\n", kGrowLogCap);
	}
	v_SGE_SnapPoolCtor(pool, count, n3, n4);

	if (sdk_snap_pool_grow.GetBool() && bWritebackCaps && bGrew)
		SnapPool_WritebackRequestedCaps(pool, count, a3, a4);

	if (sdk_snap_pool_grow.GetBool() && sdk_snap_pool_canary.GetBool() && bGrew)
		SnapPool_StampCanaries(pool, count, a3, a4, n3, n4, bWritebackCaps);
}

static int64_t Hook_SGE_PackEntityProps(int64_t a1, uint64_t* a2, int64_t a3,
	int16_t a4, int16_t a5, int a6, void* a7, int64_t a8, int64_t a9,
	int a10)
{
	const int64_t ret = v_SGE_PackEntityProps(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
	static std::atomic<LONG> s_descAuditOnce{ 0 };
	if (s_descAuditOnce.exchange(1, std::memory_order_acq_rel) == 0)
		SnapshotDump_OnFirstPack();
	if (a2)
		SGE_ClampFixedWidthPackRecords(a2, a4, a8);
	return ret;
}

// 2047 is the changed-prop end marker; do not clamp it. S21 Huffman maps it to 0xFFFF.
static void S21Bridge_SanitizePlayerDecoyPropIndices(int64_t frameObj,
	int* propIdxCursor, int serverClassId)
{
	static constexpr uint16_t kServerPropEndMarker = 2047;

	const S21Bridge_ClassMeta meta = S21Bridge_LookupClassMeta("CPlayerDecoy");
	if (meta.classId < 0 || meta.propCount == 0)
		return;

	if (serverClassId != meta.classId || frameObj == 0 || !propIdxCursor)
		return;

	// Encode-path closed-form: frameObj is live engine frame, arr at fixed off.
	const uint64_t arr = *reinterpret_cast<uint64_t*>(frameObj + 114816);
	if (arr == 0 || !S3_PtrLooksHeap(arr))
		return;

	const int start = *propIdxCursor;
	if (start < 0 || start > 8192)
		return;

	uint16_t* const indices = reinterpret_cast<uint16_t*>(arr);
	for (int i = start; i < start + 512; ++i)
	{
		const uint16_t idx = indices[i];

		if (idx == kServerPropEndMarker)
			return;

		if (idx >= meta.propCount)
		{
			// Log-only: do not write the end marker; that shears later bodies.
			static std::atomic<uint32_t> s_decoyClampLog{0};
			const uint32_t n = s_decoyClampLog.fetch_add(1);
			if (n < 20)
			{
				Warning(eDLL_T::SERVER,
					"[DECOY-PROP-FIX] OBSERVED impossible CPlayerDecoy "
					"prop idx=%u at frameSlot=%d cursor=%d (valid 0..%u) "
					"-- log-only, NOT mutating (#%u)\n",
					idx, i, start, (unsigned)(meta.propCount - 1), n + 1);
			}
			return;
		}
	}
}

static char Hook_SGE_WriteSnapshotMsg28(int64_t a1, int a2, int64_t a3,
	int64_t a4, uint32_t* a5)
{
	return v_SGE_WriteSnapshotMsg28(a1, a2, a3, a4, a5);
}
// Compact dest to dirty-list order; destSpan is the sequential pack width.
static const char* SGE_ClassTableName(int cls)
{
	if (cls < 0 || cls >= 1024)
		return nullptr;
	const uintptr_t fb = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!fb)
		return nullptr;
	uint64_t sc = 0, st = 0, nm = 0;
	if (!SGE_TryReadU64(fb + kS3_RVA_ServerClassesByID + 8ull * static_cast<uint32_t>(cls), sc)
		|| !S3_PtrLooksHeap(sc)
		|| !SGE_TryReadU64(sc + kS3_SC_SendTable, st)
		|| !S3_PtrLooksHeap(st)
		|| !SGE_TryReadU64(st + kS3_ST_NetTableName, nm)
		|| !nm)
		return nullptr;
	return reinterpret_cast<const char*>(nm);
}

// Align cache keyed on ServerClass pointer plus id; ids remap across changelevel.
struct SGE_ClsSlotCache_t
{
	std::atomic<uint64_t> sc;
	std::atomic<uint8_t>  kind; // 0 unresolved, 1 none, 2 world, 3 player
};
static SGE_ClsSlotCache_t s_clsSlotCache[1024];

static SGE_PackSlot* SGE_AlignSlotForClass(int cls)
{
	if (cls < 0 || cls >= 1024)
		return nullptr;

	const uintptr_t fb = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!fb)
		return nullptr;

	uint64_t sc = 0;
	if (!SGE_TryReadU64(fb + kS3_RVA_ServerClassesByID + 8ull * static_cast<uint32_t>(cls), sc)
		|| !S3_PtrLooksHeap(sc))
		return nullptr;

	SGE_ClsSlotCache_t& e = s_clsSlotCache[cls];
	uint8_t kind = e.kind.load(std::memory_order_relaxed);

	if (kind == 0 || e.sc.load(std::memory_order_relaxed) != sc)
	{
		const char* const name = SGE_ClassTableName(cls);
		kind = 1;
		if (name)
		{
			if (strcmp(name, "DT_WORLD") == 0 || strcmp(name, "DT_World") == 0)
				kind = 2;
			else if (strcmp(name, "DT_Player") == 0)
				kind = 3;
		}
		e.sc.store(sc, std::memory_order_relaxed);
		e.kind.store(kind, std::memory_order_relaxed);
	}

	if (kind == 2)
		return &s_slotWorld;
	return nullptr;
}

// Compaction is memoised per (frame, cell0); WriteEntityProps fix-up is thread-local.
static thread_local int t_alignStart = -1;
static thread_local int t_alignFullW = 0;

struct SGE_AlignMemo_t
{
	int cell0;
	int span;
};

// [SNAP-ALIGN] warning budget. File scope so the name resolve below can be
// skipped once it is spent.
static std::atomic<uint32_t> s_alignN{ 0 };

static SRWLOCK s_alignMemoLock = SRWLOCK_INIT;
static SGE_AlignMemo_t s_alignMemo[128];
static int s_alignMemoCount = 0;
static const void* s_alignMemoFrame = nullptr;

// Lock must be held EXCLUSIVE: a frame-pointer change retires the table, so
// this both reads and writes. Readers that only need a lookup take
// SGE_AlignMemoGet instead.
static int SGE_AlignMemoFindOrRetire(const void* frame, int cell0, int* pSpan)
{
	if (s_alignMemoFrame != frame)
	{
		s_alignMemoFrame = frame;
		s_alignMemoCount = 0;
		return 0;
	}
	for (int i = 0; i < s_alignMemoCount; ++i)
	{
		if (s_alignMemo[i].cell0 == cell0)
		{
			*pSpan = s_alignMemo[i].span;
			return 1;
		}
	}
	return 0;
}

static void SGE_AlignMemoStore(const void* frame, int cell0, int span)
{
	if (s_alignMemoFrame != frame)
		return;
	for (int i = 0; i < s_alignMemoCount; ++i)
	{
		if (s_alignMemo[i].cell0 == cell0)
		{
			s_alignMemo[i].span = span;
			return;
		}
	}
	if (s_alignMemoCount < static_cast<int>(sizeof(s_alignMemo) / sizeof(s_alignMemo[0])))
	{
		s_alignMemo[s_alignMemoCount].cell0 = cell0;
		s_alignMemo[s_alignMemoCount].span = span;
		++s_alignMemoCount;
	}
}

// Shared lock. Frame-pointer mismatch is a miss; exclusive path retires the table.
static int SGE_AlignMemoGet(const void* frame, int cell0, int* pSpan)
{
	int hit = 0;
	AcquireSRWLockShared(&s_alignMemoLock);
	if (s_alignMemoFrame == frame)
	{
		for (int i = 0; i < s_alignMemoCount; ++i)
		{
			if (s_alignMemo[i].cell0 == cell0)
			{
				*pSpan = s_alignMemo[i].span;
				hit = 1;
				break;
			}
		}
	}
	ReleaseSRWLockShared(&s_alignMemoLock);
	return hit;
}

static int SGE_DestMatchesCells(uintptr_t cellArr, int cell0,
	const uint32_t* cells, uint32_t nDwords)
{
	if (!cellArr || !cells || cell0 < 0 || nDwords == 0)
		return 0;
	const int n = (nDwords >= 4u) ? 4 : static_cast<int>(nDwords);
	for (int i = 0; i < n; ++i)
	{
		uint32_t v = 0;
		if (!SGE_TryReadU32(cellArr + 4ull * static_cast<uint32_t>(cell0 + i), v)
			|| v != cells[i])
			return 0;
	}
	return 1;
}

static void SGE_AlignPlayerDirtyCells(int cls, int64_t frame, int64_t descBlob,
	int* pIdx, int* pCell)
{
	t_alignStart = -1;
	t_alignFullW = 0;
	if (!sdk_snap_prefix_skip.GetBool() || !frame || !pIdx || !pCell)
		return;
	if (*pIdx < 0 || *pCell < 0)
		return;

	// Class first: the memo can only ever HIT for a class that owns a slot, so
	// resolving it here keeps every other entity off the global memo lock.
	SGE_PackSlot* const slot = SGE_AlignSlotForClass(cls);
	if (!slot || (!slot->latest.valid && !slot->full.valid))
		return;

	{
		int memoSpan = 0;
		if (SGE_AlignMemoGet(reinterpret_cast<const void*>(frame), *pCell, &memoSpan))
		{
			t_alignStart = *pCell;
			t_alignFullW = memoSpan;
			return;
		}
	}

	// Name resolve is only for capped warnings; skip it past the cap.
	const char* const tableName = (s_alignN.load(std::memory_order_relaxed) < 16)
		? SGE_ClassTableName(cls) : nullptr;
	const char* const tableLabel = tableName ? tableName : "?";

	uint64_t fieldArr = 0, cellArr = 0;
	if (!SGE_TryReadU64(static_cast<uintptr_t>(frame) + 114816, fieldArr)
		|| !S3_PtrLooksHeap(fieldArr)
		|| !SGE_TryReadU64(static_cast<uintptr_t>(frame) + 114824, cellArr)
		|| !S3_PtrLooksHeap(cellArr))
		return;

	uint16_t dirty[512];
	int dirtyN = 0;
	int maxFi = -1;
	int holes = 0;
	int expect = 0;
	for (int k = 0; k < 512; ++k)
	{
		uint32_t w = 0;
		if (!SGE_TryReadU32(fieldArr + 2ull * static_cast<uint32_t>(*pIdx + k), w))
			break;
		const uint16_t fi = static_cast<uint16_t>(w);
		if (fi == 2047)
			break;
		if (fi != static_cast<uint16_t>(expect))
			holes = 1;
		dirty[dirtyN++] = fi;
		if (static_cast<int>(fi) > maxFi)
			maxFi = static_cast<int>(fi);
		expect = static_cast<int>(fi) + 1;
	}

	const int cell0 = *pCell;
	uint32_t dest0 = 0;
	SGE_TryReadU32(cellArr + 4ull * static_cast<uint32_t>(cell0), dest0);

	if (dirtyN <= 0 || maxFi < 0 || maxFi >= 2048)
		return;
	if (!holes && dirty[0] == 0)
		return;
	// Dirty-only dest (Composite / thin Aqueduct). Compacting from the
	// spawn pack overwrites the 0x400 run and shears CTeam.
	if (dest0 == 0x400u)
		return;

	int destIsSeq = 0;
	int seqDwords = 0;
	if (SGE_PackIsSequential(slot->latest)
		&& slot->latest.nDwords > 8u
		&& SGE_DestMatchesCells(cellArr, cell0, slot->latest.cells,
			slot->latest.nDwords))
	{
		destIsSeq = 1;
		seqDwords = static_cast<int>(slot->latest.nDwords);
	}
	else if (slot->full.valid
		&& slot->full.nDwords > 8u
		&& SGE_DestMatchesCells(cellArr, cell0, slot->full.cells,
			slot->full.nDwords))
	{
		destIsSeq = 1;
		seqDwords = static_cast<int>(slot->full.nDwords);
	}
	if (!destIsSeq)
	{
		const uint32_t n = s_alignN.fetch_add(1, std::memory_order_relaxed);
		if (n < 16)
			Warning(eDLL_T::SERVER,
				"[SNAP-ALIGN] #%u %s SKIP dest0=%08X pack0=%08X dirty0=%u "
				"dirtyN=%d (dest not sequential pack)\n",
				n, tableLabel, dest0, slot->latest.cells[0], dirty[0], dirtyN);
		return;
	}

	const bool packOk = slot->latest.valid
		&& slot->latest.nFields > 0 && slot->latest.nFields <= 2048
		&& slot->latest.nDwords > 0 && slot->latest.nDwords <= 16384;
	if (!packOk)
	{
		const uint32_t n = s_alignN.fetch_add(1, std::memory_order_relaxed);
		if (n < 16)
			Warning(eDLL_T::SERVER,
				"[SNAP-ALIGN] #%u %s NO-PACK dirty0=%u dirtyN=%d dest0=%08X\n",
				n, tableLabel, dirty[0], dirtyN, dest0);
		return;
	}

	static thread_local uint32_t s_rec[2048];
	static thread_local uint32_t s_cells[16384];
	static thread_local uint32_t s_off[2048];
	static thread_local uint16_t s_wid[2048];
	static thread_local uint8_t s_have[2048];
	static thread_local uint32_t s_compact[16384];

	const SGE_PackCopy* used = nullptr;
	int missFi = -1;
	int out = 0;
	uint32_t nFields = 0;
	uint32_t nDwords = 0;
	const SGE_PackCopy* tryPacks[2];
	int nTry = 0;
	tryPacks[nTry++] = &slot->latest;
	if (slot->full.valid)
		tryPacks[nTry++] = &slot->full;

	for (int t = 0; t < nTry; ++t)
	{
		const SGE_PackCopy* pack = tryPacks[t];
		nFields = pack->nFields;
		nDwords = pack->nDwords;
		if (nFields == 0 || nFields > 2048 || nDwords == 0 || nDwords > 16384)
			continue;
		memcpy(s_rec, pack->rec, sizeof(uint32_t) * nFields);
		memcpy(s_cells, pack->cells, sizeof(uint32_t) * nDwords);
		memset(s_have, 0, sizeof(s_have));

		uint32_t cursor = 0;
		int bad = 0;
		for (uint32_t i = 0; i < nFields; ++i)
		{
			const uint32_t fi = s_rec[i] & 0x7FFu;
			const uint32_t cells = (s_rec[i] >> 13) & 0xFFu;
			if (fi >= 2048 || cursor + cells > nDwords)
			{
				bad = 1;
				break;
			}
			if (cells > 0)
			{
				s_off[fi] = cursor;
				s_wid[fi] = static_cast<uint16_t>(cells);
				s_have[fi] = 1;
			}
			cursor += cells;
		}
		if (bad)
			continue;

		missFi = -1;
		int compactOut = 0;
		for (int i = 0; i < dirtyN; ++i)
		{
			const uint16_t fi = dirty[i];
			if (fi >= 2048 || !s_have[fi])
			{
				missFi = static_cast<int>(fi);
				break;
			}
			compactOut += static_cast<int>(s_wid[fi]);
			if (compactOut > 16384)
			{
				missFi = static_cast<int>(fi);
				break;
			}
		}
		if (missFi >= 0 || compactOut <= 0)
			continue;

		out = 0;
		for (int i = 0; i < dirtyN; ++i)
		{
			const uint16_t fi = dirty[i];
			const int n = static_cast<int>(s_wid[fi]);
			const uint32_t off = s_off[fi];
			for (int c = 0; c < n; ++c)
				s_compact[out++] = s_cells[off + static_cast<uint32_t>(c)];
		}
		used = pack;
		break;
	}

	if (!used)
	{
		const uint32_t n = s_alignN.fetch_add(1, std::memory_order_relaxed);
		if (n < 16)
			Warning(eDLL_T::SERVER,
				"[SNAP-ALIGN] #%u %s MISS fi=%d dirty0=%u dirtyN=%d "
				"packN=%u packDwords=%u dest0=%08X pack0=%08X\n",
				n, tableLabel, missFi, dirty[0], dirtyN,
				slot->latest.nFields, slot->latest.nDwords, dest0,
				slot->latest.cells[0]);
		return;
	}

	const int destSpan = seqDwords;
	const void* const memoFrame = reinterpret_cast<const void*>(frame);

	// Rewrite + store are one locked section so a concurrent miss cannot
	// observe compacted dest with an empty memo and SKIP the cursor fix-up.
	AcquireSRWLockExclusive(&s_alignMemoLock);
	int wonSpan = 0;
	if (SGE_AlignMemoFindOrRetire(memoFrame, cell0, &wonSpan))
	{
		t_alignStart = cell0;
		t_alignFullW = wonSpan;
		ReleaseSRWLockExclusive(&s_alignMemoLock);
		(void)descBlob;
		return;
	}

	for (int i = 0; i < out; ++i)
		*reinterpret_cast<uint32_t*>(cellArr + 4ull * static_cast<uint32_t>(cell0 + i))
			= s_compact[i];

	t_alignStart = cell0;
	t_alignFullW = destSpan;
	SGE_AlignMemoStore(memoFrame, cell0, destSpan);
	ReleaseSRWLockExclusive(&s_alignMemoLock);
	(void)descBlob;
	const uint32_t n = s_alignN.fetch_add(1, std::memory_order_relaxed);
	if (n < 16)
		Warning(eDLL_T::SERVER,
			"[SNAP-ALIGN] #%u %s dirty0=%u dirtyN=%d maxFi=%d "
			"packN=%u packDwords=%u dest0=%08X pack0=%08X "
			"compact=%d span=%d cell=%d fromFull=%d\n",
			n, tableLabel, dirty[0], dirtyN, maxFi, nFields, nDwords, dest0,
			s_cells[0], out, destSpan, cell0,
			used == &slot->full ? 1 : 0);
}

static int64_t Hook_SGE_WriteEntityProps(int64_t a1, int64_t a2, int64_t a3,
	int a4, int64_t a5, int* a6, int* a7, int64_t* a8)
{
	const int bitsBefore = (a8 != nullptr)
		? *reinterpret_cast<int*>(reinterpret_cast<char*>(a8) + 0x10) : 0;
	const int idxBefore  = (a6 != nullptr) ? *a6 : 0;
	g_curEncClass.store(a4, std::memory_order_relaxed); // for [ARR-DIAG] class naming
	if (a4 == 116 || a4 == 51)
	{
		static std::atomic<uint32_t> s_dirtyDump{ 0 };
		if (s_dirtyDump.fetch_add(1, std::memory_order_relaxed) < 4 && a6)
		{
			uint64_t fieldArr = 0;
			int dirtyN = 0, maxFi = -1, holes = 0;
			char head[192];
			size_t hl = 0;
			if (SGE_TryReadU64(static_cast<uintptr_t>(a3) + 114816, fieldArr)
				&& S3_PtrLooksHeap(fieldArr))
			{
				int expect = 0;
				for (int k = 0; k < 512; ++k)
				{
					uint32_t w = 0;
					if (!SGE_TryReadU32(fieldArr + 2ull * static_cast<uint32_t>(*a6 + k), w))
						break;
					const uint16_t fi = static_cast<uint16_t>(w);
					if (fi == 2047)
						break;
					if (hl < sizeof(head) - 8 && k < 16)
						hl += snprintf(head + hl, sizeof(head) - hl, "%u ", fi);
					if (fi != static_cast<uint16_t>(expect))
						holes = 1;
					if (static_cast<int>(fi) > maxFi)
						maxFi = static_cast<int>(fi);
					++dirtyN;
					expect = static_cast<int>(fi) + 1;
				}
			}
			head[hl] = 0;
			uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
			uint64_t cellArr = 0;
			if (a7 && SGE_TryReadU64(static_cast<uintptr_t>(a3) + 114824, cellArr)
				&& S3_PtrLooksHeap(cellArr))
			{
				SGE_TryReadU32(cellArr + 4ull * static_cast<uint32_t>(*a7), c0);
				SGE_TryReadU32(cellArr + 4ull * static_cast<uint32_t>(*a7 + 1), c1);
				SGE_TryReadU32(cellArr + 4ull * static_cast<uint32_t>(*a7 + 2), c2);
				SGE_TryReadU32(cellArr + 4ull * static_cast<uint32_t>(*a7 + 3), c3);
			}
			Warning(eDLL_T::SERVER,
				"[SNAP-DIRTY] cls=%d dirtyN=%d maxFi=%d holes=%d cell=%d "
				"cells=%08X %08X %08X %08X head=%s\n",
				a4, dirtyN, maxFi, holes, a7 ? *a7 : -1, c0, c1, c2, c3, head);
		}
	}
	// Unconditional: the callee owns the sdk_snap_prefix_skip check and resets
	// t_align* ahead of it. Guarding here instead skips that reset, and the
	// cursor fix-up below then replays the previous entity's span.
	SGE_AlignPlayerDirtyCells(a4, a3, a5, a6, a7);

	// [BODY-RING] last-64 bodies (class + field/cell cursor in/out), dumped once
	// at the first CTeam body: names the body that advanced the field cursor
	// without a matching cell advance (or vice versa).
	struct BodyRec { int cls; int idxIn; int cellIn; int idxOut; int cellOut; };
	static BodyRec s_bodyRing[64];
	static std::atomic<uint32_t> s_bodyRingN{ 0 };
	static volatile LONG s_bodyRingDumped = 0;
	int ringSlot = -1;
	if (bridge_team_enc_diag.GetBool())
	{
		if (a4 == 82 && s_bodyRingDumped == 0
			&& InterlockedCompareExchange(&s_bodyRingDumped, 1, 0) == 0)
		{
			const uint32_t n = s_bodyRingN.load(std::memory_order_relaxed);
			const uint32_t from = (n > 64u) ? (n - 64u) : 0u;
			for (uint32_t i = from; i < n; ++i)
			{
				const BodyRec& r = s_bodyRing[i & 63u];
				Warning(eDLL_T::SERVER,
					"[BODY-RING] body#%u cls=%d idx %d->%d cells %d->%d\n",
					i, r.cls, r.idxIn, r.idxOut, r.cellIn, r.cellOut);
			}
			{
				int encWorld = -1, encPlayer = -1, encGnr = -1;
				for (uint32_t i = from; i < n; ++i)
				{
					const BodyRec& r = s_bodyRing[i & 63u];
					const int span = (r.cellOut >= r.cellIn) ? (r.cellOut - r.cellIn) : -1;
					if (r.cls == 116) encWorld = span;
					else if (r.cls == 51) encPlayer = span;
					else if (r.cls == 32) encGnr = span;
				}
				SGE_EmitPackVsEnc("DT_WORLD", s_meterWorld, encWorld);
				SGE_EmitPackVsEnc("DT_Player", s_meterPlayer, encPlayer);
				SGE_EmitPackVsEnc("DT_GlobalNonRewinding", s_meterGnr, encGnr);
			}
			// Last three completed bodies at first CTeam: World, Player, GNR.
			if (a3)
			{
				const int showFrom = (n > 3u) ? static_cast<int>(n - 3) : 0;
				for (int bi = showFrom; bi < static_cast<int>(n); ++bi)
				{
					const BodyRec& r = s_bodyRing[bi & 63];
					if (r.idxIn < 0 || r.cellIn < 0)
						continue;
					uint64_t flatArr = 0;
					uint32_t flatN = 0;
					const uintptr_t fb = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
					uint64_t sc = 0, st = 0, pc = 0;
					if (fb
						&& SGE_TryReadU64(fb + 0xC05F740 + static_cast<uintptr_t>(r.cls) * 8, sc)
						&& S3_PtrLooksHeap(sc)
						&& SGE_TryReadU64(sc + 0x08, st) && S3_PtrLooksHeap(st)
						&& SGE_TryReadU64(st + 0x4C0, pc) && S3_PtrLooksHeap(pc))
					{
						SGE_TryReadU64(pc + 0x08, flatArr);
						SGE_TryReadU32(pc + 0x10, flatN);
					}
					uint64_t fieldArr = 0, cellArr = 0;
					SGE_TryReadU64(static_cast<uintptr_t>(a3) + 114816, fieldArr);
					SGE_TryReadU64(static_cast<uintptr_t>(a3) + 114824, cellArr);
					if (!fieldArr || !S3_PtrLooksHeap(fieldArr))
						continue;
					int cell = r.cellIn;
					const int last = (r.idxOut > r.idxIn) ? r.idxOut : (r.idxIn + 128);
					Warning(eDLL_T::SERVER,
						"[PREV-BODY] cls=%d idx %d..%d cells %d..%d\n",
						r.cls, r.idxIn, r.idxOut, r.cellIn, r.cellOut);
					for (int wi = r.idxIn; wi < last && wi < r.idxIn + 220; ++wi)
					{
						uint32_t w = 0;
						if (!SGE_TryReadU32(fieldArr + 2ull * static_cast<uint32_t>(wi), w))
							break;
						const uint16_t fi = static_cast<uint16_t>(w);
						if (fi == 2047)
							break;
						const char* pn = nullptr;
						uint32_t ty = 0xFF;
						uint64_t sp = 0, nmp = 0;
						if (flatArr && S3_PtrLooksHeap(flatArr) && fi < flatN
							&& SGE_TryReadU64(flatArr + 8ull * fi, sp)
							&& S3_PtrLooksHeap(sp))
						{
							SGE_TryReadU32(sp, ty);
							if (SGE_TryReadU64(sp + 0x40, nmp) && nmp)
								pn = reinterpret_cast<const char*>(nmp);
						}
						uint32_t cell0 = 0;
						if (cellArr && S3_PtrLooksHeap(cellArr) && cell >= 0)
							SGE_TryReadU32(cellArr + 4ull * static_cast<uint32_t>(cell), cell0);
						int expect = 1;
						if (ty == 2) expect = 3;
						else if (ty == 3 || ty == 7) expect = 2;
						else if (ty == 6) expect = 4;
						else if (ty == 4)
							expect = (cell0 > 511u) ? 1
								: static_cast<int>(1u + (cell0 + 3u) / 4u);
						else if (ty == 5)
							expect = (cell0 > 4096u) ? 3 : (3 + static_cast<int>(cell0));
						Warning(eDLL_T::SERVER,
							"[PREV-BODY]   w=%d flat=%u '%s' t=%u expect=%d "
							"cell=%d v=0x%08X\n",
							wi, fi, pn ? pn : "?", ty, expect, cell, cell0);
						cell += expect;
					}
					Warning(eDLL_T::SERVER,
						"[PREV-BODY] cls=%d walk-end cell=%d body-end=%d leftover=%d\n",
						r.cls, cell, r.cellOut, r.cellOut - cell);
				}
			}
		}
		const uint32_t n = s_bodyRingN.fetch_add(1, std::memory_order_relaxed);
		ringSlot = static_cast<int>(n & 63u);
		s_bodyRing[ringSlot] = { a4, (a6 ? *a6 : -1), (a7 ? *a7 : -1), -2, -2 };
	}
	struct BodyRingScope
	{
		BodyRec* rec; int* pIdx; int* pCell;
		~BodyRingScope()
		{
			if (rec)
			{
				rec->idxOut = pIdx ? *pIdx : -1;
				rec->cellOut = pCell ? *pCell : -1;
			}
		}
	} bodyRingScope{ (ringSlot >= 0 ? &s_bodyRing[ringSlot] : nullptr), a6, a7 };

	SnapshotDump_OnWriteEntityProps(a3, a6, a4);

	static int s_decoyClassId = -2;
	if (s_decoyClassId == -2)
	{
		const S21Bridge_ClassMeta decoyMeta = S21Bridge_LookupClassMeta("CPlayerDecoy");
		if (decoyMeta.classId >= 0)
			s_decoyClassId = decoyMeta.classId;
		else
		{
			const uintptr_t base = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
			const int nClasses = base ? S3_RdD(base + kS3_RVA_NumServerClasses) : 0;
			if (nClasses > 0)
				s_decoyClassId = -1;
		}
	}
	if (s_decoyClassId >= 0 && a4 == s_decoyClassId)
	{
		S21Bridge_SanitizePlayerDecoyPropIndices(a3, a6, a4);
	}

	// CTeam (82) / DT_GlobalNonRewinding (32): field cursor, cell cursor, 2047 end marker.
	if (bridge_team_enc_diag.GetBool() && (a4 == 82 || a4 == 32) && a6 && a7)
	{
		static std::atomic<uint32_t> s_thN{ 0 };
		const uint32_t n = s_thN.fetch_add(1, std::memory_order_relaxed);
		if (n < 64)
		{
			uint64_t flatArr = 0;
			uint32_t flatN = 0;
			{
				const uintptr_t fb = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
				uint64_t sc = 0, st = 0, pc = 0;
				if (fb
					&& SGE_TryReadU64(fb + 0xC05F740 + static_cast<uintptr_t>(a4) * 8, sc)
					&& S3_PtrLooksHeap(sc)
					&& SGE_TryReadU64(sc + 0x08, st) && S3_PtrLooksHeap(st)
					&& SGE_TryReadU64(st + 0x4C0, pc) && S3_PtrLooksHeap(pc))
				{
					SGE_TryReadU64(pc + 0x08, flatArr);
					SGE_TryReadU32(pc + 0x10, flatN);
				}
			}

			uint64_t fieldArr = 0;
			char idxBuf[440];
			size_t bl = 0;
			if (SGE_TryReadU64(static_cast<uintptr_t>(a3) + 114816, fieldArr)
				&& S3_PtrLooksHeap(fieldArr))
			{
				for (int i = 0; i < 24 && bl < sizeof(idxBuf) - 64; ++i)
				{
					uint32_t w = 0;
					if (!SGE_TryReadU32(fieldArr + 2ull * static_cast<uint32_t>(*a6 + i), w))
						break;
					const uint16_t fi = static_cast<uint16_t>(w);
					if (fi == 2047)
					{
						bl += snprintf(idxBuf + bl, sizeof(idxBuf) - bl, "END ");
						break;
					}
					const char* pn = nullptr;
					uint32_t ty = 0xFF;
					uint64_t sp = 0, nmp = 0;
					if (flatArr && S3_PtrLooksHeap(flatArr) && fi < flatN
						&& SGE_TryReadU64(flatArr + 8ull * fi, sp) && S3_PtrLooksHeap(sp))
					{
						SGE_TryReadU32(sp, ty);
						if (SGE_TryReadU64(sp + 0x40, nmp) && nmp)
							pn = reinterpret_cast<const char*>(nmp);
					}
					bl += snprintf(idxBuf + bl, sizeof(idxBuf) - bl, "%u(%s/t%u) ",
						fi, pn ? pn : "?", ty);
				}
			}
			idxBuf[bl] = 0;
			char cellWin[96];
			size_t cl = 0;
			uint64_t cellArr = 0;
			if (SGE_TryReadU64(static_cast<uintptr_t>(a3) + 114824, cellArr)
				&& S3_PtrLooksHeap(cellArr))
			{
				for (int k = -1; k <= 4 && cl < sizeof(cellWin) - 12; ++k)
				{
					if (*a7 + k < 0)
					{
						cl += snprintf(cellWin + cl, sizeof(cellWin) - cl, "-------- ");
						continue;
					}
					uint32_t dw = 0;
					if (SGE_TryReadU32(cellArr + 4ull * static_cast<uint32_t>(*a7 + k), dw))
						cl += snprintf(cellWin + cl, sizeof(cellWin) - cl, "%08X ", dw);
					else
						cl += snprintf(cellWin + cl, sizeof(cellWin) - cl, "???????? ");
				}
			}
			cellWin[cl] = 0;
			Warning(eDLL_T::SERVER,
				"[TEAM-HDR] #%u body class=%d idxCursor=%d cellCursor=%d "
				"fields=[%s] cells-1..+4=[%s]\n",
				n, a4, *a6, *a7, idxBuf, cellWin);
		}
	}

	// bf_write: m_pData@0, m_nDataBits@0x0C, m_iCurBit@0x10, m_bOverflow@0x14.
	// Drop this body if m_pData is not a heap pointer.
	if (a8 != nullptr)
	{
		if (reinterpret_cast<const char*>(a8)[0x14])
			return 0;
		const uintptr_t bufData = *reinterpret_cast<uintptr_t*>(a8);
		const int bufBits = *reinterpret_cast<int*>(reinterpret_cast<char*>(a8) + 0x0C);
		const int curBit = *reinterpret_cast<int*>(reinterpret_cast<char*>(a8) + 0x10);
		if (!S3_PtrLooksHeap(bufData) || bufBits <= 0 || curBit < 0 ||
			static_cast<unsigned>(curBit) > static_cast<unsigned>(bufBits))
		{
			static std::atomic<uint32_t> s_bfBadN{ 0 };
			const uint32_t n = s_bfBadN.fetch_add(1, std::memory_order_relaxed);
			if (n < 32)
				Warning(eDLL_T::SERVER,
					"[SNAP-BFGUARD] #%u REFUSING entity body: bf_write is corrupt "
					"(m_pData=%p m_nDataBits=%d m_iCurBit=%d) class=%d idxCursor=%d "
					"dataCursor=%d -- dropping this entity instead of faulting in "
					"WriteUBitLong.\n",
					n, reinterpret_cast<void*>(bufData), bufBits, curBit, a4,
					idxBefore, (a7 != nullptr) ? *a7 : -1);
			return 0;
		}
	}

	const int64_t ret = v_SGE_WriteEntityProps(a1, a2, a3, a4, a5, a6, a7, a8);
	if (t_alignFullW > 0 && a7 && *a7 < t_alignStart + t_alignFullW)
		*a7 = t_alignStart + t_alignFullW;

	// Cell-stream desync can write millions of bits into the 768KB slot. Log bodies >20000 bits.
	if (a8)
	{
		const int balloonBits =
			*reinterpret_cast<int*>(reinterpret_cast<char*>(a8) + 0x10) - bitsBefore;
		if (balloonBits > 20000)
		{
			const char* tn = "?";
			{
				const uintptr_t base = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
				const uintptr_t sc = (base && a4 >= 0 && a4 < 512)
					? *reinterpret_cast<uintptr_t*>(
						base + 0xC05F740 + static_cast<uintptr_t>(a4) * 8) : 0;
				const uintptr_t st = (sc && S3_PtrLooksHeap(sc))
					? *reinterpret_cast<uintptr_t*>(sc + 0x08) : 0;
				const char* n = (st && S3_PtrLooksHeap(st))
					? *reinterpret_cast<const char**>(st + 0x4B8) : nullptr;
				if (n) tn = n;
			}
			const int idxAfter = (a6 != nullptr) ? *a6 : 0;
			static std::atomic<uint32_t> s_balloonN{ 0 };
			Warning(eDLL_T::SERVER,
				"[BALLOON] #%u class=%d ent=%d tbl=%s props=%d BITS=%d (~%dKB) "
				"-- snapshot blow-up body\n",
				s_balloonN.fetch_add(1, std::memory_order_relaxed), a4, (int)a5, tn,
				idxAfter - idxBefore, balloonBits, balloonBits / 8192);
		}
	}

	return ret;
}

// Per-prop value encoder. Replaces the divergent S3 Time/Ticks codec with the
// S21 wire form (bridge_time_encode) and hosts the read-only [ARR-DIAG] array
// element-count probe.
static int64_t Hook_DT_EncodePropValue(int a1, int a2, int a3, uint32_t a4,
	uint32_t* a5, int64_t a6, int64_t* a7)
{
	// FULL form for Time/Ticks; array elements recurse through this entry.
	if ((a2 == 9 || a2 == 8) && a7 && bridge_time_encode.GetBool())
	{
		const uint32_t bits = a5 ? *a5 : 0u;
		int need = 33;
		if (a2 == 9)
		{
			float fv;
			memcpy(&fv, &bits, sizeof(fv));
			need = (fv == 0.0f) ? 2 : 34;
		}
		if (S21_BfRemaining(a7) < need)
		{
			BfWrite_MarkOverflow(a7);
			return 0;
		}
		if (a2 == 9)
		{
			float fv;
			memcpy(&fv, &bits, sizeof(fv));
			S21_BfWriteOneBit(a7, 0);
			if (fv == 0.0f)
				S21_BfWriteOneBit(a7, 1);
			else
			{
				S21_BfWriteOneBit(a7, 0);
				S21_BfWriteUBits(a7, bits, 32);
			}
		}
		else
		{
			S21_BfWriteOneBit(a7, 0);
			S21_BfWriteUBits(a7, bits, 32);
		}
		return 1;
	}

	// Wire string length is 9 bits (max 511). Do not pass a desynced strlen to the native encoder.
	if (a2 == 4 && a5 && a7)
	{
		const uint32_t len = *a5;
		if (len > 511)
		{
			static std::atomic<uint32_t> s_strBadN{ 0 };
			const uint32_t n = s_strBadN.fetch_add(1, std::memory_order_relaxed);
			if (n < 16)
			{
				char win[96];
				size_t wl = 0;
				for (int i = -2; i <= 5 && wl < sizeof(win) - 12; ++i)
				{
					uint32_t dw = 0;
					if (SGE_TryReadU32(reinterpret_cast<uintptr_t>(a5 + i), dw))
						wl += snprintf(win + wl, sizeof(win) - wl, "%08X ", dw);
					else
						wl += snprintf(win + wl, sizeof(win) - wl, "???????? ");
				}
				Warning(eDLL_T::SERVER,
					"[SNAP-STRLEN] #%u class=%d len=%u (0x%X) a5=%p "
					"cells[a5-2..a5+5]=%s-- empty string (9-bit wire max is 511).\n",
					n, g_curEncClass.load(std::memory_order_relaxed),
					len, len, reinterpret_cast<void*>(a5), win);
			}
			if (S21_BfRemaining(a7) < 9)
			{
				BfWrite_MarkOverflow(a7);
				return 0;
			}
			S21_BfWriteUBits(a7, 0, 9);
			int remain = 1;
			for (int i = 1; i < 8; ++i)
			{
				uint32_t dw = 0;
				if (!SGE_TryReadU32(reinterpret_cast<uintptr_t>(a5 + i), dw) || !dw)
					break;
				const unsigned char* b = reinterpret_cast<const unsigned char*>(&dw);
				int ascii = 1;
				for (int k = 0; k < 4; ++k)
				{
					if (b[k] && (b[k] < 32 || b[k] > 126))
					{
						ascii = 0;
						break;
					}
				}
				if (!ascii)
					break;
				++remain;
			}
			return remain;
		}
	}

	// Same desync class: a garbage array count loops the native encoder
	// for hundreds of millions of elements.
	if (a2 == 5 && a5 && a7)
	{
		const int cnt = *reinterpret_cast<int*>(a5);
		if (cnt < 0 || cnt > 4096)
		{
			static std::atomic<uint32_t> s_arrBadN{ 0 };
			const uint32_t n = s_arrBadN.fetch_add(1, std::memory_order_relaxed);
			if (n < 16)
				Warning(eDLL_T::SERVER,
					"[SNAP-ARRLEN] #%u class=%d count=%d -- empty array.\n",
					n, g_curEncClass.load(std::memory_order_relaxed), cnt);
			const int countBits = static_cast<int>(a5[1]);
			if (countBits > 0 && countBits <= 32)
			{
				if (S21_BfRemaining(a7) < countBits)
				{
					BfWrite_MarkOverflow(a7);
					return 0;
				}
				S21_BfWriteUBits(a7, 0, countBits);
			}
			return 3;
		}
	}

	if (a2 == 5 && bridge_arr_encode_diag.GetBool())
	{
		static std::atomic<uint32_t> s_arrDiagN{ 0 };
		const uint32_t n = s_arrDiagN.fetch_add(1, std::memory_order_relaxed);
		if (n < 500)
		{
			const int cnt = a5 ? *reinterpret_cast<int*>(a5) : -3;
			const bool insane = (cnt < 0 || cnt > 4096);
			Warning(eDLL_T::SERVER,
				"[ARR-DIAG]%s #%u class=%d count=%d nBits=%u a5=%p a6=%p\n",
				insane ? " INSANE" : "", n, g_curEncClass.load(std::memory_order_relaxed),
				cnt, a4, reinterpret_cast<void*>(a5), reinterpret_cast<void*>(a6));
		}
	}

	const int64_t ret = v_DT_EncodePropValue(a1, a2, a3, a4, a5, a6, a7);

	const bool teamEncDiag =
		g_curEncClass.load(std::memory_order_relaxed) == 82
		&& bridge_team_enc_diag.GetBool();

	if (teamEncDiag)
	{
		static std::atomic<uint32_t> s_teN{ 0 };
		const uint32_t n = s_teN.fetch_add(1, std::memory_order_relaxed);
		if (n < 192)
		{
			uint32_t cell = 0;
			if (a5)
				SGE_TryReadU32(reinterpret_cast<uintptr_t>(a5), cell);
			Warning(eDLL_T::SERVER,
				"[TEAM-ENC] #%u type=%d nBits=%u a5=%p cell=0x%08X ret=%lld\n",
				n, a2, a4, reinterpret_cast<void*>(a5), cell,
				static_cast<long long>(ret));
		}
	}
	return ret;
}

static int64_t Hook_bf_WriteUBitLong(int64_t a1, unsigned int a2, int a3)
{
	if (!a1)
		return 0;

	const int cur = *reinterpret_cast<int*>(reinterpret_cast<char*>(a1) + 0x10);
	const int cap = *reinterpret_cast<int*>(reinterpret_cast<char*>(a1) + 0x0C);
	if (a3 < 0 || a3 > 32 ||
		static_cast<unsigned>(cur) + static_cast<unsigned>(a3) >
		static_cast<unsigned>(cap))
	{
		static std::atomic<uint32_t> s_ubitBadN{ 0 };
		uint32_t n = 0;
		if (SnapOverflow_ShouldLog(s_ubitBadN, n))
		{
			char callerBuf[64];
			SGE_FormatPtrRva(callerBuf, sizeof(callerBuf), _ReturnAddress());
			Warning(eDLL_T::SERVER,
				"[SNAP-UBIT] #%u refuse nBits=%d cur=%d cap=%d bf=%p caller=%s%s\n",
				n, a3, cur, cap, reinterpret_cast<void*>(a1), callerBuf,
				SnapOverflow_IsLastLog(n) ? SNAP_OVERFLOW_LOG_CAPPED : "");
		}
		BfWrite_MarkOverflow(reinterpret_cast<void*>(a1));
		return 0;
	}
	if (a3 == 0)
		return 0;
	return v_bf_WriteUBitLong(a1, a2, a3);
}

static bool Hook_bf_WriteBits(int64_t a1, unsigned int* a2, int64_t a3)
{
	if (!a1)
		return false;

	const int nBits = static_cast<int>(a3);
	const int cur = *reinterpret_cast<int*>(reinterpret_cast<char*>(a1) + 0x10);
	const int cap = *reinterpret_cast<int*>(reinterpret_cast<char*>(a1) + 0x0C);
	if (nBits < 0 ||
		static_cast<unsigned>(cur) + static_cast<unsigned>(nBits) >
		static_cast<unsigned>(cap))
	{
		static std::atomic<uint32_t> s_bitsBadN{ 0 };
		uint32_t n = 0;
		if (SnapOverflow_ShouldLog(s_bitsBadN, n))
		{
			char callerBuf[64];
			SGE_FormatPtrRva(callerBuf, sizeof(callerBuf), _ReturnAddress());
			Warning(eDLL_T::SERVER,
				"[SNAP-BFBITS] #%u refuse nBits=%d (0x%X) cur=%d cap=%d src=%p "
				"bf=%p caller=%s%s\n",
				n, nBits, static_cast<unsigned>(nBits), cur, cap,
				reinterpret_cast<void*>(a2), reinterpret_cast<void*>(a1), callerBuf,
				SnapOverflow_IsLastLog(n) ? SNAP_OVERFLOW_LOG_CAPPED : "");
		}
		BfWrite_MarkOverflow(reinterpret_cast<void*>(a1));
		return false;
	}
	if (nBits == 0)
		return true;
	return v_bf_WriteBits(a1, a2, a3);
}

static int64_t Hook_bf_WriteUBitLongRaw(int64_t a1, unsigned int a2, int a3)
{
	if (!a1)
		return 0;
	if (reinterpret_cast<const char*>(a1)[0x14])
		return 0;

	const int cur = *reinterpret_cast<int*>(reinterpret_cast<char*>(a1) + 0x10);
	const int cap = *reinterpret_cast<int*>(reinterpret_cast<char*>(a1) + 0x0C);
	if (a3 < 0 || a3 > 32 ||
		static_cast<unsigned>(cur) + static_cast<unsigned>(a3) >
		static_cast<unsigned>(cap))
	{
		static std::atomic<uint32_t> s_rawBadN{ 0 };
		uint32_t n = 0;
		if (SnapOverflow_ShouldLog(s_rawBadN, n))
		{
			char callerBuf[64];
			SGE_FormatPtrRva(callerBuf, sizeof(callerBuf), _ReturnAddress());
			Warning(eDLL_T::SERVER,
				"[SNAP-UBITRAW] #%u refuse nBits=%d cur=%d cap=%d bf=%p "
				"caller=%s%s\n",
				n, a3, cur, cap, reinterpret_cast<void*>(a1), callerBuf,
				SnapOverflow_IsLastLog(n) ? SNAP_OVERFLOW_LOG_CAPPED : "");
		}
		BfWrite_MarkOverflow(reinterpret_cast<void*>(a1));
		return 0;
	}
	if (a3 == 0)
		return 0;
	return v_bf_WriteUBitLongRaw(a1, a2, a3);
}

void VSnapshotWriterTrace::GetFun() const
{

	Module_FindPattern(g_GameDll,
		"48 8B C4 53 56 57 41 55 48 81 EC 88 00 00 00 48 89 68 08 "
		"49 8B C9 4C 89 70 D0 8B EA")
		.GetPtr(v_SGE_WriteSnapshotMsg28);


	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 "
		"48 83 EC 40 4C 8B A4 24 90 00 00 00 49 8B D8")
		.GetPtr(v_SGE_WriteEntityProps);


	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 "
		"41 56 41 57 48 83 EC 50 48 8B B4 24 B0 00 00 00 45 8B F9")
		.GetPtr(v_DT_EncodePropValue);

	Module_FindPattern(g_GameDll,
		"40 53 44 8B 59 10 4C 8B C9 8B 49 0C 8B DA")
		.GetPtr(v_bf_WriteUBitLong);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 "
		"4C 89 74 24 20 49 63 D8")
		.GetPtr(v_bf_WriteUBitLongRaw);

	Module_FindPattern(g_GameDll,
		"40 53 55 41 56 48 83 EC 30 4C 63 49 10 48 8B D9")
		.GetPtr(v_bf_WriteBits);

	// prologue -- changed-prop collector. This sits before
	// SGE_WriteEntityProps and can name the dirty edict that crashes during
	// the snapshot-manager hash walk.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 44 89 44 24 ?? 89 54 24 ?? 55 56 57 41 54 41 55 "
		"41 56 41 57 48 8D AC 24 ?? ?? ?? ?? B8 A0 32 00 00")
		.GetPtr(v_SGE_CollectChangedProps);

	// -- dirty-bucket merge/copy helper. Guarded because stale
	// repaired buckets can still have a null packedRuns/proptime backing array.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
		"41 55 41 56 41 57 48 83 EC 20 48 8B 42 10 4D 8B E8")
		.GetPtr(v_SGE_MergeSnapshotBucket);

	// -- snapshot bucket reset. Some S21 bridge flows leave a
	// bucket wrapper allocated but its backing block at +0x08 null; native code
	// immediately writes through that pointer.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 33 D2 48 8B 41 08 48 8B D9 "
		"C7 00 FF FF FF FF")
		.GetPtr(v_SGE_ResetSnapshotBucket);

	// Per-entity pack orchestrator.
	Module_FindPattern(g_GameDll,
		"4C 89 44 24 18 48 89 4C 24 08 55 53 57 41 54 41 55 41 57 "
		"48 8D 6C 24 F9 48 81 EC C8 00 00 00")
		.GetPtr(v_SGE_PackEntityProps);

	// Snapshot pool constructor. Prologue is distinctive: the two
	// movsxd of the record/cell counts right after the pushes.
	Module_FindPattern(g_GameDll,
		"40 53 55 57 41 55 48 83 EC 28 4D 63 E9 48 8B D9 49 63 E8 85 D2")
		.GetPtr(v_SGE_SnapPoolCtor);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 55 48 83 EC 20 33 ED 48 8B D9 39 29 0F 8E ?? ?? ?? ?? "
		"48 89 74 24 30 8B F5 48 89 7C 24 38 8B FD")
		.GetPtr(v_SGE_SnapPoolDtor);

	// -- S3 anim re-anchor (sets m_animStartTime/StartCycle per tick).
	// Distinctive: movss [rcx+0FECh] + mov esi,200h + movss [rcx+0FE4h]. Single-match.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 3D ?? ?? ?? ?? "
		"48 8B D9 F3 0F 10 81 EC 0F 00 00 BE 00 02 00 00 F3 0F 10 91 E4 0F 00 00")
		.GetPtr(v_AnimAnchorUpdate);
}

void VSnapshotWriterTrace::Detour(const bool bAttach) const
{
	if (v_SGE_WriteSnapshotMsg28)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_SGE_WriteSnapshotMsg28),
				reinterpret_cast<void*>(&Hook_SGE_WriteSnapshotMsg28))
			: DetourDetach(
				reinterpret_cast<void**>(&v_SGE_WriteSnapshotMsg28),
				reinterpret_cast<void*>(&Hook_SGE_WriteSnapshotMsg28));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"SGE_WriteSnapshotMsg28 result=0x%lX (target=0x%p)\n",
				r, (void*)v_SGE_WriteSnapshotMsg28);
	}
	else
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] SGE_WriteSnapshotMsg28 "
			"pattern unresolved -- [SNAP28] trace NOT active.\n");
	}

	if (v_SGE_WriteEntityProps)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_SGE_WriteEntityProps),
				reinterpret_cast<void*>(&Hook_SGE_WriteEntityProps))
			: DetourDetach(
				reinterpret_cast<void**>(&v_SGE_WriteEntityProps),
				reinterpret_cast<void*>(&Hook_SGE_WriteEntityProps));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"SGE_WriteEntityProps result=0x%lX (target=0x%p)\n",
				r, (void*)v_SGE_WriteEntityProps);
	}
	else
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] SGE_WriteEntityProps "
			"pattern unresolved -- [ENT-BITS] trace NOT active.\n");
	}

	if (v_DT_EncodePropValue)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_DT_EncodePropValue),
				reinterpret_cast<void*>(&Hook_DT_EncodePropValue))
			: DetourDetach(
				reinterpret_cast<void**>(&v_DT_EncodePropValue),
				reinterpret_cast<void*>(&Hook_DT_EncodePropValue));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"DT_EncodePropValue result=0x%lX (target=0x%p)\n",
				r, (void*)v_DT_EncodePropValue);
	}
	else
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] DT_EncodePropValue "
			"pattern unresolved -- [CP-PROP] trace NOT active.\n");
	}

	if (v_bf_WriteUBitLong)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_bf_WriteUBitLong),
				reinterpret_cast<void*>(&Hook_bf_WriteUBitLong))
			: DetourDetach(
				reinterpret_cast<void**>(&v_bf_WriteUBitLong),
				reinterpret_cast<void*>(&Hook_bf_WriteUBitLong));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"bf_WriteUBitLong result=0x%lX (target=0x%p)\n",
				r, (void*)v_bf_WriteUBitLong);
	}
	else
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] bf_WriteUBitLong "
			"pattern unresolved -- signed-nBits AV guard NOT active.\n");
	}

	if (v_bf_WriteBits)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_bf_WriteBits),
				reinterpret_cast<void*>(&Hook_bf_WriteBits))
			: DetourDetach(
				reinterpret_cast<void**>(&v_bf_WriteBits),
				reinterpret_cast<void*>(&Hook_bf_WriteBits));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"bf_WriteBits result=0x%lX (target=0x%p)\n",
				r, (void*)v_bf_WriteBits);
	}
	else
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] bf_WriteBits "
			"pattern unresolved -- signed-nBits AV guard NOT active.\n");
	}

	if (v_bf_WriteUBitLongRaw)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_bf_WriteUBitLongRaw),
				reinterpret_cast<void*>(&Hook_bf_WriteUBitLongRaw))
			: DetourDetach(
				reinterpret_cast<void**>(&v_bf_WriteUBitLongRaw),
				reinterpret_cast<void*>(&Hook_bf_WriteUBitLongRaw));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"bf_WriteUBitLongRaw result=0x%lX (target=0x%p)\n",
				r, (void*)v_bf_WriteUBitLongRaw);
	}
	else
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] bf_WriteUBitLongRaw "
			"pattern unresolved -- unchecked bit-emit still writes past cap.\n");
	}

	if (v_SGE_CollectChangedProps)
	{
		if (bAttach)
			S21Bridge_InstallCollectChangedPropsBucketFix();

		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_SGE_CollectChangedProps),
				reinterpret_cast<void*>(&Hook_SGE_CollectChangedProps))
			: DetourDetach(
				reinterpret_cast<void**>(&v_SGE_CollectChangedProps),
				reinterpret_cast<void*>(&Hook_SGE_CollectChangedProps));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"SGE_CollectChangedProps result=0x%lX (target=0x%p)\n",
				r, (void*)v_SGE_CollectChangedProps);
	}
	else
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] SGE_CollectChangedProps "
			"pattern unresolved -- [DIRTY320] trace NOT active.\n");
	}

	if (v_SGE_MergeSnapshotBucket)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_SGE_MergeSnapshotBucket),
				reinterpret_cast<void*>(&Hook_SGE_MergeSnapshotBucket))
			: DetourDetach(
				reinterpret_cast<void**>(&v_SGE_MergeSnapshotBucket),
				reinterpret_cast<void*>(&Hook_SGE_MergeSnapshotBucket));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"SGE_MergeSnapshotBucket result=0x%lX (target=0x%p)\n",
				r, (void*)v_SGE_MergeSnapshotBucket);
	}
	else
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] SGE_MergeSnapshotBucket "
			"pattern unresolved -- [FRAME-SNAP-GUARD] not active.\n");
	}

	if (v_SGE_ResetSnapshotBucket && sdk_snap_merge_guard.GetBool())
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_SGE_ResetSnapshotBucket),
				reinterpret_cast<void*>(&Hook_SGE_ResetSnapshotBucket))
			: DetourDetach(
				reinterpret_cast<void**>(&v_SGE_ResetSnapshotBucket),
				reinterpret_cast<void*>(&Hook_SGE_ResetSnapshotBucket));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"SGE_ResetSnapshotBucket result=0x%lX (target=0x%p)\n",
				r, (void*)v_SGE_ResetSnapshotBucket);
	}
	else if (bAttach && !v_SGE_ResetSnapshotBucket)
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] SGE_ResetSnapshotBucket "
			"pattern unresolved -- [FRAME-SNAP-GUARD] reset backfill not active.\n");
	}

	if (v_SGE_PackEntityProps)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_SGE_PackEntityProps),
				reinterpret_cast<void*>(&Hook_SGE_PackEntityProps))
			: DetourDetach(
				reinterpret_cast<void**>(&v_SGE_PackEntityProps),
				reinterpret_cast<void*>(&Hook_SGE_PackEntityProps));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"SGE_PackEntityProps result=0x%lX (target=0x%p)\n",
				r, (void*)v_SGE_PackEntityProps);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] SGE_PackEntityProps "
			"pattern unresolved -- [PACK-CELL] clamp NOT active.\n");
	}

	if (v_SGE_SnapPoolCtor)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_SGE_SnapPoolCtor),
				reinterpret_cast<void*>(&Hook_SGE_SnapPoolCtor))
			: DetourDetach(
				reinterpret_cast<void**>(&v_SGE_SnapPoolCtor),
				reinterpret_cast<void*>(&Hook_SGE_SnapPoolCtor));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"SGE_SnapPoolCtor result=0x%lX (target=0x%p)\n",
				r, (void*)v_SGE_SnapPoolCtor);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] SGE_SnapPoolCtor "
			"pattern unresolved -- [SNAP-POOL] grow NOT active; packers can "
			"overrun the stock pool backings.\n");
	}

	if (v_SGE_SnapPoolDtor)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_SGE_SnapPoolDtor),
				reinterpret_cast<void*>(&Hook_SGE_SnapPoolDtor))
			: DetourDetach(
				reinterpret_cast<void**>(&v_SGE_SnapPoolDtor),
				reinterpret_cast<void*>(&Hook_SGE_SnapPoolDtor));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"SGE_SnapPoolDtor result=0x%lX (target=0x%p)\n",
				r, (void*)v_SGE_SnapPoolDtor);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] SGE_SnapPoolDtor "
			"pattern unresolved -- [SNAP-CANARY] verification is not active.\n");
	}

	if (v_AnimAnchorUpdate)
	{
		const LONG r = bAttach
			? DetourAttach(
				reinterpret_cast<void**>(&v_AnimAnchorUpdate),
				reinterpret_cast<void*>(&Hook_AnimAnchorUpdate))
			: DetourDetach(
				reinterpret_cast<void**>(&v_AnimAnchorUpdate),
				reinterpret_cast<void*>(&Hook_AnimAnchorUpdate));
		if (bAttach)
			Msg(eDLL_T::SERVER, "[s21-bridge] DetourAttach "
				"AnimAnchorUpdate result=0x%lX (target=0x%p)\n",
				r, (void*)v_AnimAnchorUpdate);
	}
	else
	{
		Warning(eDLL_T::SERVER, "[s21-bridge] AnimAnchorUpdate "
			"pattern unresolved -- [ANIM-ANCHOR-HOLD] NOT active.\n");
	}
}

void SnapshotWriter_LevelShutdown(void)
{
	s_animAnchorInit = false;

	for (int i = 0; i < 1024; ++i)
		s_clsSlotCache[i].kind.store(0, std::memory_order_relaxed);
}
