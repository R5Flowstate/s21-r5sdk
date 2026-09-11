//=============================================================================//
//
// Purpose: boundary validation helpers (Mem_IsReadable / Mem_InModule).
//
//=============================================================================//
#include "tier0/memvalidate.h"
#include "tier0/module.h"

//-----------------------------------------------------------------------------
// Purpose: committed + readable + not guard for [pAddr, pAddr+nSize).
// Walks multiple VirtualQuery regions when the range crosses a boundary.
//-----------------------------------------------------------------------------
bool Mem_IsReadable(const void* const pAddr, const size_t nSize)
{
	if (!pAddr)
		return false;
	if (nSize == 0)
		return true;

	const uintptr_t nStart = reinterpret_cast<uintptr_t>(pAddr);
	// User-space only: null page and non-canonical / kernel half never map.
	if (nStart < 0x10000)
		return false;
	if (nStart >= 0x800000000000ULL)
		return false;

	const uintptr_t nEnd = nStart + nSize;
	if (nEnd < nStart)
		return false;

	uintptr_t nCur = nStart;
	while (nCur < nEnd)
	{
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(reinterpret_cast<LPCVOID>(nCur), &mbi, sizeof(mbi)) == 0)
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
			return false;

		const DWORD nReadable =
			PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
			PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if ((mbi.Protect & nReadable) == 0)
			return false;

		const uintptr_t nRegionEnd =
			reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (nRegionEnd <= nCur)
			return false;
		nCur = nRegionEnd;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Mem_IsReadable with a thread-local 8-entry region ring.
// Bridge entity decode runs on a second thread -- ring must not be shared.
//-----------------------------------------------------------------------------
bool Mem_IsReadableCached(const void* const pAddr, const size_t nSize,
	volatile long* const pMissCounter)
{
	if (!pAddr)
		return false;
	const uintptr_t a = reinterpret_cast<uintptr_t>(pAddr);
	if (a < 0x10000)
		return false;
	if (nSize == 0)
		return true;

	const uintptr_t aEnd = a + nSize;
	if (aEnd < a)
		return false;

	struct OkRegion { uintptr_t base, end; };
	static thread_local OkRegion s_ok[8] = {};
	static thread_local int s_okNext = 0;
	for (int i = 0; i < 8; ++i)
	{
		if (s_ok[i].end != 0 && a >= s_ok[i].base && aEnd <= s_ok[i].end)
			return true;
	}

	if (pMissCounter)
		InterlockedIncrement(pMissCounter);

	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(pAddr, &mbi, sizeof(mbi)) == 0)
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return false;

	const DWORD nReadable =
		PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if ((mbi.Protect & nReadable) == 0)
		return false;

	const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
	const uintptr_t regionEnd = regionBase + mbi.RegionSize;
	if (aEnd > regionEnd)
	{
		// Range spans more than one region: multi-region walk, no ring insert.
		return Mem_IsReadable(pAddr, nSize);
	}

	s_ok[s_okNext].base = regionBase;
	s_ok[s_okNext].end = regionEnd;
	s_okNext = (s_okNext + 1) & 7;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: pure [base, base+size) range test against a resolved module image.
//-----------------------------------------------------------------------------
bool Mem_InModule(const CModule& mod, const void* const pAddr, const size_t nSize)
{
	if (!pAddr || nSize == 0)
		return false;

	const QWORD nBase = mod.GetModuleBase();
	const QWORD nModSize = mod.GetModuleSize();
	if (!nBase || !nModSize)
		return false;

	const QWORD nAddr = reinterpret_cast<QWORD>(pAddr);
	if (nAddr < nBase)
		return false;

	const QWORD nModEnd = nBase + nModSize;
	if (nAddr >= nModEnd)
		return false;

	const QWORD nRangeEnd = nAddr + static_cast<QWORD>(nSize);
	if (nRangeEnd < nAddr)
		return false;
	return nRangeEnd <= nModEnd;
}
