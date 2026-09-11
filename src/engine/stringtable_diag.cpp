//=============================================================================//
// S21 CNetworkStringTable diagnostic + crash mitigation implementation
//=============================================================================//

#include "core/stdafx.h"
#include "stringtable_diag.h"
#include "tier0/dbg.h"
#include <atomic>

// +0x10 name, +0x48 m_pItems, +0x50 m_pItemsClientSide; dict storage +0x18.
// GetEntryUserData: *(dict+0x18)+72*idx+56; NULL storage + idx=0 returns 0x38.

static constexpr size_t STBL_NAME_OFF      = 0x10;
static constexpr size_t STBL_PITEMS_OFF    = 0x48;
static constexpr size_t STBL_PITEMS_CS_OFF = 0x50;
static constexpr size_t DICT_STORAGE_OFF   = 0x18;

// Returns the table name pointer for diagnostic logging, with SEH so a
// corrupted pointer never takes down the diag path.
static const char* SafeTableName(const void* tbl)
{
	if (!tbl) return "(null-table)";
	__try
	{
		const char* name = *reinterpret_cast<const char* const*>(
			static_cast<const uint8_t*>(tbl) + STBL_NAME_OFF);
		// Sanity: pointer must be in a sensible range and start with a
		// printable byte. Prevents random heap garbage from being logged
		// as a 4 GiB string.
		if (!name) return "(null-name)";
		const char first = name[0];
		if (first < 0x20 || first > 0x7E) return "(name-junk)";
		return name;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return "(table-AV)";
	}
}

// Returns the storage pointer of m_pItems / m_pItemsClientSide depending on
// the index sign, mirroring the dispatch logic inside GetEntryUserData
// itself. Used to pre-validate before letting the original function run.
static const void* ResolveItemsDict(const void* tbl, int stringNumber)
{
	const uint8_t* base = static_cast<const uint8_t*>(tbl);
	const void* clientSide = *reinterpret_cast<void* const*>(
		base + STBL_PITEMS_CS_OFF);
	if (clientSide && stringNumber < -1) return clientSide;
	return *reinterpret_cast<void* const*>(base + STBL_PITEMS_OFF);
}

static const void* GetDictStorage(const void* dict)
{
	if (!dict) return nullptr;
	__try
	{
		return *reinterpret_cast<void* const*>(
			static_cast<const uint8_t*>(dict) + DICT_STORAGE_OFF);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
	}
}

// One-shot warning suppression by table-name pointer + storage-NULL state.
// Hash collisions are fine; we just want to avoid log spam when a worker
// thread polls the same empty modelprecache hundreds of times per second.
static std::atomic<uint64_t> s_warnedTableHash[64] = {};
static bool ShouldWarn(const char* name, bool storageNull)
{
	if (!name) return true;
	uint64_t key = storageNull ? 1ULL : 0ULL;
	for (const char* p = name; *p; ++p)
		key = key * 1099511628211ULL ^ (uint8_t)*p;
	size_t slot = (key & (64 - 1));
	uint64_t prev = s_warnedTableHash[slot].load(std::memory_order_relaxed);
	if (prev == key) return false;
	s_warnedTableHash[slot].store(key, std::memory_order_relaxed);
	return true;
}

// Short-circuit empty storage (would return 0x38). Healthy path is stock.
static const void* __fastcall Hook_CNetworkStringTable_GetEntryUserData_S21(
	void* thisp, int stringNumber, int* length)
{
	// NULL table guard (defensive; stock would crash inside [rcx+0x50] read).
	if (!thisp)
	{
		Warning(eDLL_T::ENGINE,
			"[stbl-diag] GetEntryUserData(NULL,%d) -- caller=%p\n",
			stringNumber, _ReturnAddress());
		if (length) *length = 0;
		return nullptr;
	}

	const char* tblName = SafeTableName(thisp);
	const void* dict    = ResolveItemsDict(thisp, stringNumber);
	const void* storage = GetDictStorage(dict);
	const bool  storageNull = (storage == nullptr);
	const bool  storageBad  = (storage == reinterpret_cast<void*>(
									static_cast<intptr_t>(-1)));

	// SAFE PATH: storage allocated -- delegate to the original. We ignore
	// a NULL dict here because that is a real corruption case the engine
	// would crash on anyway, and our short-circuit above wouldn't help.
	if (!storageNull && !storageBad && dict)
	{
		return v_CNetworkStringTable_GetEntryUserData_S21(
			thisp, stringNumber, length);
	}

	// CRASH SHIELD: storage is NULL/inaccessible. Returning NULL here lets
	// the caller (e.g. ) take its v5==0 branch, which memsets
	// the output buffer and returns 0 cleanly.
	if (ShouldWarn(tblName, storageNull))
	{
		Warning(eDLL_T::ENGINE,
			"[stbl-diag] EMPTY-TABLE QUERY table='%s' (this=%p) idx=%d "
			"dict=%p storage=%p caller=%p -- returning NULL "
			"(would have crashed at 0x%llx in stock GetEntryUserData)\n",
			tblName, thisp, stringNumber, dict, storage, _ReturnAddress(),
			(unsigned long long)(72ULL * (uint32_t)(
				stringNumber < -1 ? -stringNumber : stringNumber) + 56ULL));
	}
	if (length) *length = 0;
	return nullptr;
}

void VStringTableDiagS21::Detour(const bool bAttach) const
{
	DetourSetup(&v_CNetworkStringTable_GetEntryUserData_S21,
				&Hook_CNetworkStringTable_GetEntryUserData_S21, bAttach);
}
