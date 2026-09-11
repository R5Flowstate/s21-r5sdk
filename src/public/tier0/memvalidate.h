//=============================================================================//
//
// Purpose: boundary validation for pointers that arrive from the engine.
// Mem_IsReadable is VirtualQuery -- cold/boundary only, never per-prop.
//
//=============================================================================//
#ifndef TIER0_MEMVALIDATE_H
#define TIER0_MEMVALIDATE_H

class CModule;

// Committed, readable, and not a guard page. VirtualQuery -- COLD/BOUNDARY only.
bool Mem_IsReadable(const void* const pAddr, const size_t nSize);

// Mem_IsReadable with a TLS region cache; pMissCounter counts real VirtualQuery misses.
bool Mem_IsReadableCached(const void* const pAddr, const size_t nSize,
	volatile long* const pMissCounter = nullptr);

// Pure range compare against a resolved module -- cheap enough for a cold
// boot/init check; still not free for per-prop loops.
bool Mem_InModule(const CModule& mod, const void* const pAddr, const size_t nSize);

// Alignment plus non-null, for pointers that must be a valid object base.
inline bool Mem_IsAlignedPtr(const void* const pAddr, const size_t nAlign)
{
	return pAddr != nullptr &&
		(reinterpret_cast<uintptr_t>(pAddr) & (nAlign - 1)) == 0;
}

#endif // TIER0_MEMVALIDATE_H
