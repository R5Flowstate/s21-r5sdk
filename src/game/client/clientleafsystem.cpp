//=============================================================================//
//
// Purpose: Prevent fatal error when visible object limit is reached.
//
// See clientleafsystem.h for design overview.
//
//=============================================================================//

#include "core/stdafx.h"
#include "game/client/clientleafsystem.h"

//-----------------------------------------------------------------------------
// Resolved addresses
//-----------------------------------------------------------------------------
static uintptr_t s_clientLeafBase = 0; // g_ClientLeafSystem base address

//-----------------------------------------------------------------------------
// Binary patch infrastructure (same pattern as blast_pattern.cpp)
//-----------------------------------------------------------------------------
struct BytePatchRecord
{
	uint8_t* pAddr;
	uint8_t  size;
	uint32_t origValue;
};

static constexpr int MAX_PATCHES = 16;
static BytePatchRecord s_patches[MAX_PATCHES];
static int s_numPatches = 0;

static void ApplyPatch(uint8_t* pAddr, uint32_t newValue, uint8_t size)
{
	if (s_numPatches >= MAX_PATCHES)
		return;

	BytePatchRecord& rec = s_patches[s_numPatches++];
	rec.pAddr = pAddr;
	rec.size = size;

	DWORD oldProtect;
	VirtualProtect(pAddr, size, PAGE_EXECUTE_READWRITE, &oldProtect);

	if (size == 1)
	{
		rec.origValue = *pAddr;
		*pAddr = static_cast<uint8_t>(newValue);
	}
	else
	{
		rec.origValue = *reinterpret_cast<uint32_t*>(pAddr);
		*reinterpret_cast<uint32_t*>(pAddr) = newValue;
	}

	VirtualProtect(pAddr, size, oldProtect, &oldProtect);
}

static void RestoreAllPatches()
{
	for (int i = 0; i < s_numPatches; i++)
	{
		BytePatchRecord& rec = s_patches[i];
		DWORD oldProtect;
		VirtualProtect(rec.pAddr, rec.size, PAGE_EXECUTE_READWRITE, &oldProtect);

		if (rec.size == 1)
			*rec.pAddr = static_cast<uint8_t>(rec.origValue);
		else
			*reinterpret_cast<uint32_t*>(rec.pAddr) = rec.origValue;

		VirtualProtect(rec.pAddr, rec.size, oldProtect, &oldProtect);
	}
	s_numPatches = 0;
}

//-----------------------------------------------------------------------------
// Runtime state for hooks
//-----------------------------------------------------------------------------
static std::atomic<int> s_visibleObjectCount{ 0 };
static std::atomic<bool> s_budgetWarned{ false };
static std::atomic<bool> s_overflowWarned{ false };
static bool s_overflowReserved = false;

//-----------------------------------------------------------------------------
// Public accessors
//-----------------------------------------------------------------------------
int ClientLeafSystem_GetVisibleObjectCount()
{
	return s_visibleObjectCount.load(std::memory_order_relaxed);
}

int ClientLeafSystem_GetVisibleObjectBudget()
{
	return VISIBLE_OBJECTS_BUDGET;
}

int ClientLeafSystem_GetVisibleObjectMax()
{
	return VISIBLE_OBJECTS_MAX - 1; // 8191 usable
}

bool ClientLeafSystem_IsOverflowing()
{
	return s_overflowWarned.load(std::memory_order_relaxed);
}

//-----------------------------------------------------------------------------
// Console command: cl_visible_objects
//-----------------------------------------------------------------------------
static void CC_VisibleObjects_f(const CCommand& args)
{
	const int count = s_visibleObjectCount.load(std::memory_order_relaxed);
	const int max = VISIBLE_OBJECTS_MAX - 1; // 8191 usable (8191 is overflow)
	const float pct = max > 0 ? (count * 100.0f / max) : 0.0f;

	Msg(eDLL_T::ENGINE,
		"Visible objects: %d / %d (%.1f%%) | budget: %d | overflow: %s\n",
		count, max, pct, VISIBLE_OBJECTS_BUDGET,
		s_overflowWarned.load() ? "YES" : "no");
}

static ConCommand cl_visible_objects(
	"cl_visible_objects",
	CC_VisibleObjects_f,
	"Print the current visible object count and budget status.",
	FCVAR_CLIENTDLL);

//-----------------------------------------------------------------------------
// Hook: AllocVisibleObject
//
// Called each time the engine needs a visible object handle. We intercept to
// - Reserve the overflow handle on first invocation
// - Track allocation count for budget warnings
// - Return the overflow handle when the allocator is full (instead of crash)
//-----------------------------------------------------------------------------
unsigned short h_CClientLeafSystem_AllocVisibleObject()
{
	// On first call, permanently reserve the overflow handle by setting its
	// bit in IsInUse. This prevents the normal allocator from ever handing
	// out handle 8191, keeping it available as our overflow sentinel.
	if (!s_overflowReserved)
	{
		s_overflowReserved = true;

		if (s_clientLeafBase)
		{
			volatile int64_t* pIsInUse = reinterpret_cast<volatile int64_t*>(
				s_clientLeafBase + CLIENTLEAF_ISINUSE_OFFSET);
			_InterlockedOr64(const_cast<int64_t*>(&pIsInUse[OVERFLOW_CHUNK]),
				1LL << OVERFLOW_BIT);
		}
	}

	const unsigned short result = v_CClientLeafSystem_AllocVisibleObject();

	if (result != VISIBLE_OBJECTS_OVERFLOW_HANDLE)
	{
		// Normal allocation succeeded - track count and check budget.
		const int count = s_visibleObjectCount.fetch_add(1, std::memory_order_relaxed) + 1;

		if (count > VISIBLE_OBJECTS_BUDGET && !s_budgetWarned.exchange(true))
		{
			Warning(eDLL_T::ENGINE,
				"CClientLeafSystem: Over budget with %d visible objects "
				"(budget: %d, max: %d)\n",
				count, VISIBLE_OBJECTS_BUDGET, VISIBLE_OBJECTS_MAX - 1);
		}

		return result;
	}

	// Allocation failed (all 8191 real slots full) - patched allocator
	// returned the overflow handle instead of crashing. Warn once.
	if (!s_overflowWarned.exchange(true))
	{
		Warning(eDLL_T::ENGINE,
			"CClientLeafSystem: Visible object limit reached (%d). "
			"Overflow objects will share rendering data.\n",
			VISIBLE_OBJECTS_MAX - 1);
	}

	return VISIBLE_OBJECTS_OVERFLOW_HANDLE;
}

//-----------------------------------------------------------------------------
// Hook: FreeVisibleObject
//
// Protects the overflow handle from being freed and tracks count.
//-----------------------------------------------------------------------------
void h_CClientLeafSystem_FreeVisibleObject(__int64 a1, unsigned short handle)
{
	if (handle == VISIBLE_OBJECTS_OVERFLOW_HANDLE)
		return; // Never free the reserved overflow handle.

	s_visibleObjectCount.fetch_sub(1, std::memory_order_relaxed);
	v_CClientLeafSystem_FreeVisibleObject(a1, handle);
}

//-----------------------------------------------------------------------------
// IDetour: log resolved addresses
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// IDetour: find functions via pattern scan
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// IDetour: resolve variable addresses
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// IDetour: apply patches and set up hooks
//-----------------------------------------------------------------------------
