//=============================================================================//
//
// Purpose: one code-patch and near-module alloc primitive for executable rewrites.
//
//=============================================================================//
#ifndef TIER0_MEMORY_PATCH_H
#define TIER0_MEMORY_PATCH_H

class CModule;

// Writes over executable bytes and restores the original protection. Returns
// false and writes nothing when the protect call fails.
bool Mem_PatchCode(void* const pTarget, const void* const pSrc, const size_t nSize);

// Reserves nSize bytes within +/-2GB of the module base so a RIP-relative disp32
// from patched instructions stays in int32 range. Returns nullptr on failure.
uint8_t* Mem_AllocNearModule(const CModule& mod, const size_t nSize);

#endif // TIER0_MEMORY_PATCH_H
