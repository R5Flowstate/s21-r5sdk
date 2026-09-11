//=============================================================================//
//
// Purpose: Process memory and rpak slot table census at map pak changes.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "rpak_observe.h"
#include "pak_census.h"
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include "tier0/memstd.h"

struct CensusRegion_t
{
	uintptr_t base;
	size_t    commit;
	size_t    reserve;
	bool      readable;
};

static CensusRegion_t s_censusRegions[8192];
static uintptr_t s_prevBases[8192];
static size_t s_nPrevBases = 0;

// Regions carried across censuses with the census that first saw them and
// the owning frame, so long-lived regions can be attributed by age.
struct CensusTracked_t
{
	uintptr_t base;
	size_t    commit;
	unsigned  firstSeen;
	void*     owner;
};
static CensusTracked_t s_tracked[8192];
static size_t s_nTracked = 0;
static unsigned s_censusIndex = 0;
static size_t s_nPrevBasesRegions = 0;

// Every VirtualAlloc commit is recorded with its caller chain so a census can
// name the owner of each region that appeared since the previous census.
struct CensusAllocRec_t
{
	uintptr_t base;
	size_t    size;
	void*     frames[8];
	USHORT    nFrames;
};

static CensusAllocRec_t s_allocRing[8192];
static volatile LONG s_allocRingHead = 0;
static volatile LONG s_allocRingLock = 0;

typedef LPVOID(WINAPI* PFN_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
static PFN_VirtualAlloc v_VirtualAlloc = nullptr;
static bool s_bCensusHookActive = false;

// Every commit, whatever its size, is charged to its caller chain so commits
// inside existing reservations are attributed too.
struct CensusCommitStat_t
{
	void*  frames[6];
	USHORT nFrames;
	size_t count;
	size_t bytes;
	size_t countAtLastReport;
	size_t bytesAtLastReport;
};
static CensusCommitStat_t s_commitStats[512];
static size_t s_nCommitStats = 0;
static volatile LONG s_commitLock = 0;

static void Pak_CensusChargeCommit(const size_t bytes)
{
	void* frames[6] = {};
	const USHORT nFrames = RtlCaptureStackBackTrace(2, ARRAYSIZE(frames), frames, nullptr);
	while (InterlockedCompareExchange(&s_commitLock, 1, 0) != 0)
		YieldProcessor();
	size_t k = 0;
	for (; k < s_nCommitStats; ++k)
		if (memcmp(s_commitStats[k].frames, frames, sizeof(frames)) == 0)
			break;
	if (k == s_nCommitStats && s_nCommitStats < ARRAYSIZE(s_commitStats))
	{
		memcpy(s_commitStats[k].frames, frames, sizeof(frames));
		s_commitStats[k].nFrames = nFrames;
		s_commitStats[k].count = 0;
		s_commitStats[k].bytes = 0;
		s_commitStats[k].countAtLastReport = 0;
		s_commitStats[k].bytesAtLastReport = 0;
		++s_nCommitStats;
	}
	if (k < s_nCommitStats)
	{
		++s_commitStats[k].count;
		s_commitStats[k].bytes += bytes;
	}
	InterlockedExchange(&s_commitLock, 0);
}

static LPVOID WINAPI Hook_VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
	LPVOID const result = v_VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
	if (!result || !(flAllocationType & MEM_COMMIT) || !s_bCensusHookActive)
		return result;
	Pak_CensusChargeCommit(dwSize);
	if (dwSize < 0x10000)
		return result;

	CensusAllocRec_t rec;
	rec.base = reinterpret_cast<uintptr_t>(result);
	rec.size = dwSize;
	rec.nFrames = RtlCaptureStackBackTrace(1, ARRAYSIZE(rec.frames), rec.frames, nullptr);

	while (InterlockedCompareExchange(&s_allocRingLock, 1, 0) != 0)
		YieldProcessor();
	s_allocRing[s_allocRingHead & (ARRAYSIZE(s_allocRing) - 1)] = rec;
	++s_allocRingHead;
	InterlockedExchange(&s_allocRingLock, 0);
	return result;
}

static const CensusAllocRec_t* Pak_CensusFindAlloc(const uintptr_t base, const size_t commit)
{
	const LONG head = s_allocRingHead;
	const LONG count = head < static_cast<LONG>(ARRAYSIZE(s_allocRing)) ? head : static_cast<LONG>(ARRAYSIZE(s_allocRing));
	for (LONG i = 1; i <= count; ++i)
	{
		const CensusAllocRec_t& r = s_allocRing[(head - i) & (ARRAYSIZE(s_allocRing) - 1)];
		if (r.base >= base && r.base < base + commit)
			return &r;
	}
	return nullptr;
}

static void Pak_CensusFormatFrame(void* const frame, char* const out, const size_t outSize)
{
	HMODULE hMod = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(frame), &hMod) && hMod)
	{
		char szMod[MAX_PATH] = {};
		GetModuleFileNameA(hMod, szMod, sizeof(szMod));
		const char* const pszBase = strrchr(szMod, '\\') ? strrchr(szMod, '\\') + 1 : szMod;
		snprintf(out, outSize, "%s+%llX", pszBase,
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frame) - reinterpret_cast<uintptr_t>(hMod)));
	}
	else
	{
		snprintf(out, outSize, "%p", frame);
	}
}

// Walk a region as a dlmalloc segment: chunk head at +8 carries the size in
// the low 53 bits, bit 1 = chunk in use. Returns false when the layout does
// not decode as a segment.
// Regions can be released by another thread between the address-space walk
// and the read, so foreign memory is copied through the kernel and a failure
// ends the walk instead of faulting.
static bool Pak_CensusSafeRead(const uintptr_t addr, void* const out, const size_t size)
{
	SIZE_T nRead = 0;
	return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, size, &nRead) && nRead == size;
}

static bool Pak_CensusWalkSegment(const uintptr_t base, const size_t commit, size_t* pInUse, size_t* pFree, size_t* pChunks)
{
	*pInUse = 0; *pFree = 0; *pChunks = 0;
	const uintptr_t end = base + commit;
	uintptr_t c = base;
	for (int guard = 0; guard < 100000; ++guard)
	{
		if (c + 16 > end)
			break;
		unsigned long long head = 0;
		if (!Pak_CensusSafeRead(c + 8, &head, sizeof(head)))
			break;
		const size_t size = static_cast<size_t>(head & 0x1FFFFFFFFFFFF8ull);
		if (size < 16 || c + size > end)
			break;
		if (head & 2) *pInUse += size; else *pFree += size;
		++*pChunks;
		c += size;
	}
	return *pChunks > 0 && (*pInUse + *pFree) >= commit / 2;
}

// Allocation-level tracking on the engine allocator: every block of 32 KB or
// more is kept with its caller chain until it is freed, so a census can name
// the owner of blocks that outlive several censuses.
struct CensusLiveAlloc_t
{
	void*    ptr;
	size_t   size;
	unsigned birth;
	USHORT   nFrames;
	void*    frames[8];
};

static constexpr size_t kLiveTableSize = 1 << 18;
static CensusLiveAlloc_t s_liveTable[kLiveTableSize];
static volatile LONG s_liveLock = 0;
static size_t s_liveCount = 0;
static size_t s_liveDropped = 0;
static constexpr size_t kLiveMinSize = 0x8000;

static inline size_t Pak_CensusLiveSlot(const void* const ptr)
{
	return (reinterpret_cast<uintptr_t>(ptr) >> 4) * 0x9E3779B97F4A7C15ull >> (64 - 18);
}

static inline void Pak_CensusLiveLock(void)
{
	while (InterlockedCompareExchange(&s_liveLock, 1, 0) != 0)
		YieldProcessor();
}
static inline void Pak_CensusLiveUnlock(void) { InterlockedExchange(&s_liveLock, 0); }

static void Pak_CensusLiveInsert(void* const ptr, const size_t size)
{
	if (!ptr || size < kLiveMinSize || !s_bCensusHookActive)
		return;
	CensusLiveAlloc_t rec;
	rec.ptr = ptr;
	rec.size = size;
	rec.birth = s_censusIndex;
	rec.nFrames = RtlCaptureStackBackTrace(2, ARRAYSIZE(rec.frames), rec.frames, nullptr);

	Pak_CensusLiveLock();
	size_t slot = Pak_CensusLiveSlot(ptr);
	size_t tomb = SIZE_MAX;
	for (size_t probe = 0; probe < 256; ++probe, slot = (slot + 1) & (kLiveTableSize - 1))
	{
		void* const cur = s_liveTable[slot].ptr;
		if (cur == reinterpret_cast<void*>(1))
		{
			if (tomb == SIZE_MAX) tomb = slot;
			continue;
		}
		if (!cur || cur == ptr)
		{
			if (!cur && tomb != SIZE_MAX) slot = tomb;
			if (cur != ptr) ++s_liveCount;
			s_liveTable[slot] = rec;
			Pak_CensusLiveUnlock();
			return;
		}
	}
	if (tomb != SIZE_MAX)
	{
		s_liveTable[tomb] = rec;
		++s_liveCount;
	}
	else
	{
		++s_liveDropped;
	}
	Pak_CensusLiveUnlock();
}

static void Pak_CensusLiveErase(void* const ptr)
{
	if (!ptr || !s_liveCount)
		return;
	Pak_CensusLiveLock();
	size_t slot = Pak_CensusLiveSlot(ptr);
	for (size_t probe = 0; probe < 256; ++probe, slot = (slot + 1) & (kLiveTableSize - 1))
	{
		if (!s_liveTable[slot].ptr)
			break;
		if (s_liveTable[slot].ptr == ptr)
		{
			// Tombstone keeps the probe chain intact; size 0 marks it reusable.
			s_liveTable[slot].size = 0;
			s_liveTable[slot].ptr = reinterpret_cast<void*>(1);
			--s_liveCount;
			break;
		}
	}
	Pak_CensusLiveUnlock();
}

typedef void* (__fastcall* PFN_MemAlloc)(IMemAlloc*, size_t);
typedef void* (__fastcall* PFN_MemAllocDbg)(IMemAlloc*, size_t, const char*, int);
typedef void* (__fastcall* PFN_MemRealloc)(IMemAlloc*, void*, size_t);
typedef void* (__fastcall* PFN_MemReallocDbg)(IMemAlloc*, void*, size_t, const char*, int);
typedef void  (__fastcall* PFN_MemFree)(IMemAlloc*, void*);
typedef void  (__fastcall* PFN_MemFreeDbg)(IMemAlloc*, void*, const char*, int);

static PFN_MemAllocDbg   v_MemInternalAlloc = nullptr;
static PFN_MemAlloc      v_MemAlloc = nullptr;
static PFN_MemReallocDbg v_MemInternalRealloc = nullptr;
static PFN_MemRealloc    v_MemRealloc = nullptr;
static PFN_MemFreeDbg    v_MemInternalFree = nullptr;
static PFN_MemFree       v_MemFree = nullptr;

static void* __fastcall Hook_MemInternalAlloc(IMemAlloc* pThis, size_t nSize, const char* pszFile, int nLine)
{
	void* const p = v_MemInternalAlloc(pThis, nSize, pszFile, nLine);
	Pak_CensusLiveInsert(p, nSize);
	return p;
}
static void* __fastcall Hook_MemAlloc(IMemAlloc* pThis, size_t nSize)
{
	void* const p = v_MemAlloc(pThis, nSize);
	Pak_CensusLiveInsert(p, nSize);
	return p;
}
static void* __fastcall Hook_MemInternalRealloc(IMemAlloc* pThis, void* pMem, size_t nSize, const char* pszFile, int nLine)
{
	Pak_CensusLiveErase(pMem);
	void* const p = v_MemInternalRealloc(pThis, pMem, nSize, pszFile, nLine);
	Pak_CensusLiveInsert(p, nSize);
	return p;
}
static void* __fastcall Hook_MemRealloc(IMemAlloc* pThis, void* pMem, size_t nSize)
{
	Pak_CensusLiveErase(pMem);
	void* const p = v_MemRealloc(pThis, pMem, nSize);
	Pak_CensusLiveInsert(p, nSize);
	return p;
}
static void __fastcall Hook_MemInternalFree(IMemAlloc* pThis, void* pMem, const char* pszFile, int nLine)
{
	Pak_CensusLiveErase(pMem);
	v_MemInternalFree(pThis, pMem, pszFile, nLine);
}
static void __fastcall Hook_MemFree(IMemAlloc* pThis, void* pMem)
{
	Pak_CensusLiveErase(pMem);
	v_MemFree(pThis, pMem);
}

struct CensusLiveStat_t
{
	void*  key0;
	void*  key1;
	size_t n;
	size_t bytes;
	size_t sample;
};

static unsigned s_switchCensusIndex = 0;

// Commits charged since the previous census, largest first.
static void Pak_CensusReportCommits(void)
{
	CensusCommitStat_t local[512];
	size_t nLocal = 0;
	while (InterlockedCompareExchange(&s_commitLock, 1, 0) != 0)
		YieldProcessor();
	nLocal = s_nCommitStats;
	if (nLocal > ARRAYSIZE(local))
		nLocal = ARRAYSIZE(local);
	if (nLocal)
		memcpy(local, s_commitStats, nLocal * sizeof(local[0]));
	for (size_t k = 0; k < s_nCommitStats; ++k)
	{
		s_commitStats[k].bytesAtLastReport = s_commitStats[k].bytes;
		s_commitStats[k].countAtLastReport = s_commitStats[k].count;
	}
	InterlockedExchange(&s_commitLock, 0);

	size_t totalDelta = 0;
	for (size_t k = 0; k < nLocal; ++k)
		totalDelta += local[k].bytes - local[k].bytesAtLastReport;
	Msg(eDLL_T::RTECH, "[PAK-CENSUS]   commits since last census: %llu MB across %zu caller chains\n",
		static_cast<unsigned long long>(totalDelta >> 20), nLocal);
	for (int pass = 0; pass < 10; ++pass)
	{
		size_t best = SIZE_MAX;
		size_t bestDelta = 0;
		for (size_t k = 0; k < nLocal; ++k)
		{
			const size_t delta = local[k].bytes - local[k].bytesAtLastReport;
			if (delta > bestDelta) { bestDelta = delta; best = k; }
		}
		if (best == SIZE_MAX || bestDelta < (1u << 20))
			break;
		CensusCommitStat_t& st = local[best];
		char szChain[512] = {};
		size_t len = 0;
		for (USHORT f = 0; f < st.nFrames && len < sizeof(szChain) - 40; ++f)
		{
			char szFrame[96];
			Pak_CensusFormatFrame(st.frames[f], szFrame, sizeof(szFrame));
			len += snprintf(szChain + len, sizeof(szChain) - len, "%s%s", f ? " < " : "", szFrame);
		}
		Msg(eDLL_T::RTECH, "[PAK-CENSUS]   commit +%llu MB (%zu calls) via %s\n",
			static_cast<unsigned long long>(bestDelta >> 20), st.count - st.countAtLastReport, szChain);
		st.bytesAtLastReport = st.bytes;
		st.countAtLastReport = st.count;
	}
}

// bySwitch: report blocks born before the last map-switch census (the ones a
// map change failed to release). Otherwise: blocks alive for three censuses.
static void Pak_CensusReportLiveAllocs(const bool bySwitch)
{
	static CensusLiveStat_t s_stats[128];
	size_t nStats = 0;
	size_t nOld = 0, oldBytes = 0, nAll = 0, allBytes = 0;
	if (bySwitch && (s_switchCensusIndex == 0 || s_censusIndex < s_switchCensusIndex + 2))
		return;

	Pak_CensusLiveLock();
	for (size_t i = 0; i < kLiveTableSize; ++i)
	{
		const CensusLiveAlloc_t& a = s_liveTable[i];
		if (!a.ptr || a.ptr == reinterpret_cast<void*>(1) || !a.size)
			continue;
		++nAll;
		allBytes += a.size;
		if (bySwitch ? (a.birth >= s_switchCensusIndex || a.birth == 0) : (s_censusIndex - a.birth < 3))
			continue;
		++nOld;
		oldBytes += a.size;
		void* const k0 = a.nFrames > 0 ? a.frames[0] : nullptr;
		void* const k1 = a.nFrames > 1 ? a.frames[1] : nullptr;
		size_t k = 0;
		for (; k < nStats; ++k)
			if (s_stats[k].key0 == k0 && s_stats[k].key1 == k1)
				break;
		if (k == nStats && nStats < ARRAYSIZE(s_stats))
		{
			s_stats[nStats].key0 = k0;
			s_stats[nStats].key1 = k1;
			s_stats[nStats].n = 0;
			s_stats[nStats].bytes = 0;
			s_stats[nStats].sample = i;
			++nStats;
		}
		if (k < nStats)
		{
			++s_stats[k].n;
			s_stats[k].bytes += a.size;
		}
	}
	Pak_CensusLiveUnlock();

	Msg(eDLL_T::RTECH, "[PAK-CENSUS]   live allocs >=32K: %zu (%llu MB); %s: %zu (%llu MB); table dropped=%zu\n",
		nAll, static_cast<unsigned long long>(allBytes >> 20),
		bySwitch ? "SURVIVED LAST MAP SWITCH" : "aged >=3 censuses",
		nOld, static_cast<unsigned long long>(oldBytes >> 20), s_liveDropped);

	for (int pass = 0; pass < 14; ++pass)
	{
		size_t best = SIZE_MAX;
		for (size_t k = 0; k < nStats; ++k)
			if (s_stats[k].bytes && (best == SIZE_MAX || s_stats[k].bytes > s_stats[best].bytes))
				best = k;
		if (best == SIZE_MAX)
			break;
		const CensusLiveAlloc_t& a = s_liveTable[s_stats[best].sample];
		char szChain[512] = {};
		size_t len = 0;
		for (USHORT f = 0; f < a.nFrames && len < sizeof(szChain) - 40; ++f)
		{
			char szFrame[96];
			Pak_CensusFormatFrame(a.frames[f], szFrame, sizeof(szFrame));
			len += snprintf(szChain + len, sizeof(szChain) - len, "%s%s", f ? " < " : "", szFrame);
		}
		Msg(eDLL_T::RTECH, "[PAK-CENSUS]   %s n=%zu bytes=%llu KB sample=%llu KB via %s\n",
			bySwitch ? "survivor" : "aged", s_stats[best].n, static_cast<unsigned long long>(s_stats[best].bytes >> 10),
			static_cast<unsigned long long>(a.size >> 10), szChain);
		s_stats[best].bytes = 0;
	}
}

struct CensusCallerStat_t
{
	void*  frame;
	size_t count;
	size_t bytes;
};

static bool Pak_CensusSeenBase(const uintptr_t base)
{
	size_t lo = 0, hi = s_nPrevBases;
	while (lo < hi)
	{
		const size_t mid = (lo + hi) / 2;
		if (s_prevBases[mid] < base) lo = mid + 1; else hi = mid;
	}
	return lo < s_nPrevBases && s_prevBases[lo] == base;
}

static void* Pak_CensusOwnerForRegion(const uintptr_t base, const size_t commit)
{
	const CensusAllocRec_t* const rec = Pak_CensusFindAlloc(base, commit);
	if (!rec)
		return nullptr;
	int nGameFrames = 0;
	for (USHORT f = 0; f < rec->nFrames; ++f)
	{
		HMODULE hMod = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(rec->frames[f]), &hMod))
			continue;
		if (hMod == GetModuleHandleA(NULL) && ++nGameFrames == 5)
			return rec->frames[f];
	}
	return rec->nFrames ? rec->frames[rec->nFrames - 1] : nullptr;
}

// Carry every committed region across censuses; regions still present three
// censuses after they appeared are the ones nobody frees.
static void Pak_CensusReportPersisting(void)
{
	static CensusTracked_t s_next[8192];
	size_t nNext = 0;
	size_t oldIdx = 0;
	for (size_t i = 0; i < s_nPrevBasesRegions && nNext < ARRAYSIZE(s_next); ++i)
	{
		const CensusRegion_t& r = s_censusRegions[i];
		if (!r.commit)
			continue;
		while (oldIdx < s_nTracked && s_tracked[oldIdx].base < r.base)
			++oldIdx;
		CensusTracked_t& t = s_next[nNext++];
		t.base = r.base;
		t.commit = r.commit;
		if (oldIdx < s_nTracked && s_tracked[oldIdx].base == r.base)
		{
			t.firstSeen = s_tracked[oldIdx].firstSeen;
			t.owner = s_tracked[oldIdx].owner;
		}
		else
		{
			t.firstSeen = s_censusIndex;
			t.owner = Pak_CensusOwnerForRegion(r.base, r.commit);
		}
	}
	memcpy(s_tracked, s_next, nNext * sizeof(CensusTracked_t));
	s_nTracked = nNext;

	struct Stat { void* owner; size_t n; size_t bytes; };
	static Stat s_stats[96];
	size_t nStats = 0;
	size_t nOld = 0, oldBytes = 0;
	for (size_t i = 0; i < s_nTracked; ++i)
	{
		const CensusTracked_t& t = s_tracked[i];
		if (s_censusIndex - t.firstSeen < 3 || t.firstSeen == 0)
			continue;
		++nOld;
		oldBytes += t.commit;
		size_t k = 0;
		for (; k < nStats; ++k)
			if (s_stats[k].owner == t.owner)
				break;
		if (k == nStats && nStats < ARRAYSIZE(s_stats))
		{
			s_stats[nStats].owner = t.owner;
			s_stats[nStats].n = 0;
			s_stats[nStats].bytes = 0;
			++nStats;
		}
		if (k < nStats)
		{
			++s_stats[k].n;
			s_stats[k].bytes += t.commit;
		}
	}
	for (int pass = 0; pass < 12; ++pass)
	{
		size_t best = SIZE_MAX;
		for (size_t k = 0; k < nStats; ++k)
			if (s_stats[k].bytes && (best == SIZE_MAX || s_stats[k].bytes > s_stats[best].bytes))
				best = k;
		if (best == SIZE_MAX)
			break;
		char szFrame[96];
		Pak_CensusFormatFrame(s_stats[best].owner, szFrame, sizeof(szFrame));
		Msg(eDLL_T::RTECH, "[PAK-CENSUS]   persist %-32s regions=%zu commit=%llu MB\n",
			szFrame, s_stats[best].n, static_cast<unsigned long long>(s_stats[best].bytes >> 20));
		s_stats[best].bytes = 0;
	}
	Msg(eDLL_T::RTECH, "[PAK-CENSUS]   persisting (seen >=3 censuses ago, born after boot census): regions=%zu commit=%llu MB\n",
		nOld, static_cast<unsigned long long>(oldBytes >> 20));
	++s_censusIndex;
}

// Regions that appeared since the previous census: size histogram plus a
// sample of their first bytes so the owner can be recognised from the log.
static void Pak_CensusLogNewRegions(const size_t nRegions)
{
	s_nPrevBasesRegions = nRegions;
	if (s_nPrevBases)
	{
		size_t hist[8] = {};
		size_t histBytes[8] = {};
		size_t nNew = 0, newBytes = 0, nSampled = 0, nAttributed = 0;
		size_t newSegInUse = 0, newSegFree = 0, nSegs = 0;
		static CensusCallerStat_t s_callers[64];
		size_t nCallers = 0;
		for (size_t i = 0; i < nRegions; ++i)
		{
			const CensusRegion_t& r = s_censusRegions[i];
			if (!r.commit || Pak_CensusSeenBase(r.base))
				continue;
			++nNew;
			newBytes += r.commit;

			const CensusAllocRec_t* const rec = Pak_CensusFindAlloc(r.base, r.commit);
			void* owner = nullptr;
			if (rec)
			{
				++nAttributed;
				// Deepest game-image frame past the allocator is the owner.
				int nGameFrames = 0;
				for (USHORT f = 0; f < rec->nFrames; ++f)
				{
					HMODULE hMod = nullptr;
					if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCSTR>(rec->frames[f]), &hMod))
						continue;
					if (hMod == GetModuleHandleA(NULL) && ++nGameFrames == 5)
					{
						owner = rec->frames[f];
						break;
					}
				}
				if (!owner && rec->nFrames)
					owner = rec->frames[rec->nFrames - 1];

				size_t c = 0;
				for (; c < nCallers; ++c)
					if (s_callers[c].frame == owner)
						break;
				if (c == nCallers && nCallers < ARRAYSIZE(s_callers))
				{
					s_callers[nCallers].frame = owner;
					s_callers[nCallers].count = 0;
					s_callers[nCallers].bytes = 0;
					++nCallers;
				}
				if (c < nCallers)
				{
					++s_callers[c].count;
					s_callers[c].bytes += r.commit;
				}

				size_t segInUse = 0, segFree = 0, segChunks = 0;
				const bool bSeg = r.readable && Pak_CensusWalkSegment(r.base, r.commit, &segInUse, &segFree, &segChunks);
				if (bSeg)
				{
					newSegInUse += segInUse;
					newSegFree += segFree;
					++nSegs;
				}

				if (nSampled < 24)
				{
					char szChain[512] = {};
					size_t len = 0;
					for (USHORT f = 0; f < rec->nFrames && len < sizeof(szChain) - 40; ++f)
					{
						char szFrame[96];
						Pak_CensusFormatFrame(rec->frames[f], szFrame, sizeof(szFrame));
						len += snprintf(szChain + len, sizeof(szChain) - len, "%s%s", f ? " < " : "", szFrame);
					}
					Msg(eDLL_T::RTECH, "[PAK-CENSUS]   new base=%p commit=%7llu KB seg[inuse=%llu KB free=%llu KB chunks=%zu] via %s\n",
						reinterpret_cast<void*>(r.base), static_cast<unsigned long long>(r.commit >> 10),
						static_cast<unsigned long long>(segInUse >> 10), static_cast<unsigned long long>(segFree >> 10), segChunks, szChain);
				}
			}
			int bucket = 0;
			for (size_t sz = r.commit >> 16; sz && bucket < 7; sz >>= 1) ++bucket;
			++hist[bucket];
			histBytes[bucket] += r.commit;

			if (!rec && nSampled < 24 && r.readable && r.commit >= 0x10000)
			{
				++nSampled;
				static const size_t s_offsets[] = { 0x00, 0x10, 0x20, 0x40, 0x80, 0x100 };
				for (size_t o = 0; o < ARRAYSIZE(s_offsets); ++o)
				{
					if (s_offsets[o] + 16 > r.commit)
						break;
					unsigned char b[16] = {};
					if (!Pak_CensusSafeRead(r.base + s_offsets[o], b, sizeof(b)))
						break;
					char ascii[17] = {};
					for (int k = 0; k < 16; ++k)
						ascii[k] = (b[k] >= 0x20 && b[k] < 0x7F) ? static_cast<char>(b[k]) : '.';
					Msg(eDLL_T::RTECH, "[PAK-CENSUS]   new base=%p commit=%7llu KB +%03zX %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X '%s'\n",
						reinterpret_cast<void*>(r.base), static_cast<unsigned long long>(r.commit >> 10), s_offsets[o],
						b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], ascii);
				}
			}
		}
		for (size_t c = 0; c < nCallers; ++c)
		{
			char szFrame[96];
			Pak_CensusFormatFrame(s_callers[c].frame, szFrame, sizeof(szFrame));
			Msg(eDLL_T::RTECH, "[PAK-CENSUS]   owner %-32s regions=%zu commit=%llu KB\n",
				szFrame, s_callers[c].count, static_cast<unsigned long long>(s_callers[c].bytes >> 10));
		}
		Pak_CensusReportPersisting();
		Pak_CensusReportLiveAllocs(false);
		Pak_CensusReportLiveAllocs(true);
		Pak_CensusReportCommits();
		Msg(eDLL_T::RTECH, "[PAK-CENSUS]   new segments=%zu inuse=%llu MB free=%llu MB\n",
			nSegs, static_cast<unsigned long long>(newSegInUse >> 20), static_cast<unsigned long long>(newSegFree >> 20));
		Msg(eDLL_T::RTECH, "[PAK-CENSUS]   new regions=%zu (attributed %zu) commit=%llu MB; by size <64K:%zu(%lluK) <128K:%zu(%lluK) <256K:%zu(%lluK) <512K:%zu(%lluK) <1M:%zu(%lluK) <2M:%zu(%lluK) <4M:%zu(%lluK) >=4M:%zu(%lluK)\n",
			nNew, nAttributed, static_cast<unsigned long long>(newBytes >> 20),
			hist[0], static_cast<unsigned long long>(histBytes[0] >> 10), hist[1], static_cast<unsigned long long>(histBytes[1] >> 10),
			hist[2], static_cast<unsigned long long>(histBytes[2] >> 10), hist[3], static_cast<unsigned long long>(histBytes[3] >> 10),
			hist[4], static_cast<unsigned long long>(histBytes[4] >> 10), hist[5], static_cast<unsigned long long>(histBytes[5] >> 10),
			hist[6], static_cast<unsigned long long>(histBytes[6] >> 10), hist[7], static_cast<unsigned long long>(histBytes[7] >> 10));
	}

	s_nPrevBases = 0;
	for (size_t i = 0; i < nRegions && s_nPrevBases < ARRAYSIZE(s_prevBases); ++i)
		if (s_censusRegions[i].commit)
			s_prevBases[s_nPrevBases++] = s_censusRegions[i].base;
}

static void Pak_CensusLogRegions(void)
{
	size_t totalPrivate = 0;
	size_t totalMapped = 0;
	size_t totalImage = 0;
	size_t nRegions = 0;
	size_t overflowCommit = 0;

	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = 0;
	int guard = 0;
	while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi) && guard++ < 2000000)
	{
		const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (next <= addr)
			break;
		addr = next;

		if (mbi.State == MEM_FREE)
			continue;

		const bool bCommit = mbi.State == MEM_COMMIT;
		if (mbi.Type == MEM_IMAGE)
		{
			if (bCommit) totalImage += mbi.RegionSize;
			continue;
		}
		if (mbi.Type == MEM_MAPPED)
		{
			if (bCommit) totalMapped += mbi.RegionSize;
			continue;
		}
		if (bCommit)
			totalPrivate += mbi.RegionSize;

		const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
		if (nRegions && s_censusRegions[nRegions - 1].base == base)
		{
			if (bCommit) s_censusRegions[nRegions - 1].commit += mbi.RegionSize;
			s_censusRegions[nRegions - 1].reserve += mbi.RegionSize;
		}
		else if (nRegions < ARRAYSIZE(s_censusRegions))
		{
			const bool bReadable = bCommit && base == reinterpret_cast<uintptr_t>(mbi.BaseAddress)
				&& (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0
				&& (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
			s_censusRegions[nRegions].base = base;
			s_censusRegions[nRegions].commit = bCommit ? mbi.RegionSize : 0;
			s_censusRegions[nRegions].reserve = mbi.RegionSize;
			s_censusRegions[nRegions].readable = bReadable;
			++nRegions;
		}
		else if (bCommit)
		{
			overflowCommit += mbi.RegionSize;
		}
	}

	Msg(eDLL_T::RTECH, "[PAK-CENSUS]   regions: private=%llu MB mapped=%llu MB image=%llu MB bases=%zu overflow=%llu MB\n",
		static_cast<unsigned long long>(totalPrivate >> 20),
		static_cast<unsigned long long>(totalMapped >> 20),
		static_cast<unsigned long long>(totalImage >> 20),
		nRegions,
		static_cast<unsigned long long>(overflowCommit >> 20));

	Pak_CensusLogNewRegions(nRegions);

	size_t smallCommit = 0;
	size_t nSmall = 0;
	for (int pass = 0; pass < 16; ++pass)
	{
		size_t best = SIZE_MAX;
		for (size_t i = 0; i < nRegions; ++i)
			if (s_censusRegions[i].commit && (best == SIZE_MAX || s_censusRegions[i].commit > s_censusRegions[best].commit))
				best = i;
		if (best == SIZE_MAX)
			break;

		HMODULE hMod = nullptr;
		char szMod[MAX_PATH] = "heap";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(s_censusRegions[best].base), &hMod) && hMod)
		{
			GetModuleFileNameA(hMod, szMod, sizeof(szMod));
		}
		Msg(eDLL_T::RTECH, "[PAK-CENSUS]   arena base=%p commit=%llu MB reserve=%llu MB %s\n",
			reinterpret_cast<void*>(s_censusRegions[best].base),
			static_cast<unsigned long long>(s_censusRegions[best].commit >> 20),
			static_cast<unsigned long long>(s_censusRegions[best].reserve >> 20),
			strrchr(szMod, '\\') ? strrchr(szMod, '\\') + 1 : szMod);
		s_censusRegions[best].commit = 0;
	}
	for (size_t i = 0; i < nRegions; ++i)
	{
		if (s_censusRegions[i].commit)
		{
			smallCommit += s_censusRegions[i].commit;
			++nSmall;
		}
	}
	Msg(eDLL_T::RTECH, "[PAK-CENSUS]   remaining %zu base(s) commit=%llu MB\n",
		nSmall, static_cast<unsigned long long>(smallCommit >> 20));
}


// s_pakLoad neighbours of the slot table, same layout in the DX11 and DX12 builds.
static constexpr ptrdiff_t kS21_PakLoad_LivePaks   = -0x08;
static constexpr ptrdiff_t kS21_PakLoad_LiveTracks = -0x28;
static constexpr ptrdiff_t kS21_PakLoad_LiveAssets = 0x2C000;
static constexpr size_t    kS21_PakSlot_AssetCount = 0x10;
static constexpr size_t    kS21_PakSlot_Mem        = 0x30;
static constexpr size_t    kS21_PakSlot_MemStride  = 32;
static constexpr size_t    kS21_PakSlot_MemCount   = 4;
static constexpr size_t    kS21_PakSlot_MemSizes   = 0xB0;

static void Pak_CensusLogVideoMemory(void)
{
	typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
	static PFN_CreateDXGIFactory1 s_pfnCreate = nullptr;
	if (!s_pfnCreate)
	{
		const HMODULE hDxgi = GetModuleHandleA("dxgi.dll");
		if (hDxgi)
			s_pfnCreate = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(hDxgi, "CreateDXGIFactory1"));
		if (!s_pfnCreate)
			return;
	}

	IDXGIFactory1* pFactory = nullptr;
	if (FAILED(s_pfnCreate(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&pFactory))) || !pFactory)
		return;

	IDXGIAdapter1* pAdapter = nullptr;
	if (SUCCEEDED(pFactory->EnumAdapters1(0, &pAdapter)) && pAdapter)
	{
		IDXGIAdapter3* pAdapter3 = nullptr;
		if (SUCCEEDED(pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&pAdapter3))) && pAdapter3)
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
			DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal = {};
			pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
			pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
			Msg(eDLL_T::RTECH, "[PAK-CENSUS]   dxgi: local(vram)=%llu MB budget=%llu MB  nonlocal(sysmem)=%llu MB budget=%llu MB\n",
				static_cast<unsigned long long>(local.CurrentUsage >> 20),
				static_cast<unsigned long long>(local.Budget >> 20),
				static_cast<unsigned long long>(nonLocal.CurrentUsage >> 20),
				static_cast<unsigned long long>(nonLocal.Budget >> 20));
			pAdapter3->Release();
		}
		pAdapter->Release();
	}
	pFactory->Release();
}

static void Pak_CensusLogPakLoadCounters(const uintptr_t slotBase)
{
	const int livePaks   = *reinterpret_cast<const int*>(slotBase + kS21_PakLoad_LivePaks);
	const int liveTracks = *reinterpret_cast<const int*>(slotBase + kS21_PakLoad_LiveTracks);
	const int liveAssets = *reinterpret_cast<const int*>(slotBase + kS21_PakLoad_LiveAssets);
	Msg(eDLL_T::RTECH, "[PAK-CENSUS]   pakload: paks=%d tracks=%d assets=%d\n", livePaks, liveTracks, liveAssets);
}

static ConVar sdk_pak_census("sdk_pak_census", "0", FCVAR_DEVELOPMENTONLY,
	"Log process memory and every live rpak slot at each map pak change.");

void Pak_CensusLog(const char* pszReason, bool bForce)
{
	if (!bForce && !sdk_pak_census.GetBool())
		return;
	// A switch logs twice (before unload, after load); only the first counts.
	if (pszReason && strncmp(pszReason, "SetupMapPaks", 12) == 0 && s_censusIndex > s_switchCensusIndex + 1)
		s_switchCensusIndex = s_censusIndex;

	PROCESS_MEMORY_COUNTERS_EX pmc = {};
	pmc.cb = sizeof(pmc);
	GetProcessMemoryInfo(GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));

	Msg(eDLL_T::RTECH, "[PAK-CENSUS] %s: private=%llu MB workingset=%llu MB peak=%llu MB\n",
		pszReason ? pszReason : "?",
		static_cast<unsigned long long>(pmc.PrivateUsage >> 20),
		static_cast<unsigned long long>(pmc.WorkingSetSize >> 20),
		static_cast<unsigned long long>(pmc.PeakWorkingSetSize >> 20));

	Pak_CensusLogRegions();
	Pak_CensusLogVideoMemory();

	const uintptr_t slotBase = Pak_GetSlotBase_S21();
	if (!slotBase)
		return;

	Pak_CensusLogPakLoadCounters(slotBase);

	int nLive = 0;
	unsigned long long totalPages = 0;
	for (size_t slot = 0; slot < kS21_PakSlotCount; ++slot)
	{
		const uintptr_t entry = slotBase + slot * kS21_PakSlotStride;
		int status = 0;
		unsigned long long pakPages = 0;
		const char* pszName = nullptr;
		unsigned int assetCount = 0;
		bool bRowOk = false;
		__try
		{
			status = *reinterpret_cast<const int*>(entry + kS21_PakSlot_Status);
			if (status != 0)
			{
				for (size_t i = 0; i < kS21_PakSlot_MemCount; ++i)
				{
					const int memType = *reinterpret_cast<const int*>(entry + kS21_PakSlot_Mem + i * kS21_PakSlot_MemStride);
					if (memType)
						pakPages += *reinterpret_cast<const unsigned long long*>(entry + kS21_PakSlot_MemSizes + i * sizeof(unsigned long long));
				}
				pszName = *reinterpret_cast<const char* const*>(entry + kS21_PakSlot_Name);
				assetCount = *reinterpret_cast<const unsigned int*>(entry + kS21_PakSlot_AssetCount);
			}
			bRowOk = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			bRowOk = false;
		}
		if (!bRowOk)
			continue;
		if (status == 0)
			continue;
		totalPages += pakPages;
		++nLive;
		Msg(eDLL_T::RTECH, "[PAK-CENSUS]   slot=%3zu %-30s pages=%7llu KB assets=%5u '%s'\n",
			slot, Pak_StatusToString_S21(status), pakPages >> 10, assetCount, pszName ? pszName : "");
	}
	Msg(eDLL_T::RTECH, "[PAK-CENSUS]   %d live slot(s), pages=%llu MB\n", nLive, totalPages >> 20);
}

void Pak_CensusTick(void)
{
	static double s_dLast = 0.0;
	s_bCensusHookActive = sdk_pak_census.GetBool();
	if (!s_bCensusHookActive)
		return;
	const double now = Plat_FloatTime();
	if (now - s_dLast < 30.0)
		return;
	s_dLast = now;
	Pak_CensusLog("periodic", false);
}

void VPakCensus::GetAdr(void) const
{
	LogFunAdr("VirtualAlloc", v_VirtualAlloc);
	LogFunAdr("CStdMemAlloc::Alloc", v_MemAlloc);
	LogFunAdr("CStdMemAlloc::Free", v_MemFree);
}

void VPakCensus::GetFun(void) const
{
	const HMODULE hKernelBase = GetModuleHandleA("kernelbase.dll");
	if (hKernelBase)
		v_VirtualAlloc = reinterpret_cast<PFN_VirtualAlloc>(GetProcAddress(hKernelBase, "VirtualAlloc"));
	if (!v_VirtualAlloc)
		Warning(eDLL_T::RTECH, "[PAK-CENSUS] kernelbase VirtualAlloc unresolved -- new regions stay unattributed\n");
}

void VPakCensus::Detour(const bool bAttach) const
{
	if (v_VirtualAlloc)
		DetourSetup(&v_VirtualAlloc, &Hook_VirtualAlloc, bAttach);

	if (!bAttach)
		return;

	// The client image exports no g_pMemAllocSingleton. The pak allocator's
	// Free thunk loads it: `mov rax, cs:g_pMemAllocSingleton` at thunk+9.
	const uintptr_t pakAllocVtbl = Pak_GetGlobalAllocatorSlot_S21();
	if (!pakAllocVtbl)
		return;
	const uint8_t* const freeThunk = *reinterpret_cast<uint8_t* const*>(pakAllocVtbl + 8);
	if (!freeThunk || freeThunk[9] != 0x48 || freeThunk[10] != 0x8B || freeThunk[11] != 0x05)
	{
		Warning(eDLL_T::RTECH, "[PAK-CENSUS] pak Free thunk shape mismatch -- allocator not hooked\n");
		return;
	}
	const int32_t rel = *reinterpret_cast<const int32_t*>(freeThunk + 12);
	IMemAlloc* const pAlloc = *reinterpret_cast<IMemAlloc* const*>(freeThunk + 16 + rel);
	if (!pAlloc)
	{
		Warning(eDLL_T::RTECH, "[PAK-CENSUS] g_pMemAllocSingleton still null -- allocator not hooked\n");
		return;
	}
	const uintptr_t vtbl = *reinterpret_cast<const uintptr_t*>(pAlloc);
	if (!vtbl)
		return;
	// Other threads allocate while the slots are swapped, so every original
	// must be in place before the first slot is written.
	const uintptr_t* const slots = reinterpret_cast<const uintptr_t*>(vtbl);
	v_MemInternalAlloc   = reinterpret_cast<PFN_MemAllocDbg>(slots[0]);
	v_MemAlloc           = reinterpret_cast<PFN_MemAlloc>(slots[1]);
	v_MemInternalRealloc = reinterpret_cast<PFN_MemReallocDbg>(slots[2]);
	v_MemRealloc         = reinterpret_cast<PFN_MemRealloc>(slots[3]);
	v_MemInternalFree    = reinterpret_cast<PFN_MemFreeDbg>(slots[4]);
	v_MemFree            = reinterpret_cast<PFN_MemFree>(slots[5]);
	MemoryBarrier();

	void* discard = nullptr;
	CMemory::HookVirtualMethod(vtbl, &Hook_MemInternalAlloc,   0, &discard);
	CMemory::HookVirtualMethod(vtbl, &Hook_MemAlloc,           1, &discard);
	CMemory::HookVirtualMethod(vtbl, &Hook_MemInternalRealloc, 2, &discard);
	CMemory::HookVirtualMethod(vtbl, &Hook_MemRealloc,         3, &discard);
	CMemory::HookVirtualMethod(vtbl, &Hook_MemInternalFree,    4, &discard);
	CMemory::HookVirtualMethod(vtbl, &Hook_MemFree,            5, &discard);
	Msg(eDLL_T::RTECH, "[PAK-CENSUS] CStdMemAlloc vtable hooked @ %p\n", reinterpret_cast<void*>(vtbl));
}

static void CC_PakCensus_f(const CCommand& args)
{
	(void)args;
	Pak_CensusLog("console", true);
}
static ConCommand pak_census("pak_census", CC_PakCensus_f,
	"Print process memory and every live rpak slot.", FCVAR_DEVELOPMENTONLY);
