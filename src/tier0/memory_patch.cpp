//=============================================================================//
//
// Purpose: Mem_PatchCode / Mem_AllocNearModule implementation.
//
//=============================================================================//
#include "tier0/memory_patch.h"
#include "tier0/module.h"

#include <cstring>

//-----------------------------------------------------------------------------
// Purpose: write over executable bytes; restore prior page protection.
// Returns false when VirtualProtect fails (nothing written).
//-----------------------------------------------------------------------------
bool Mem_PatchCode(void* const pTarget, const void* const pSrc, const size_t nSize)
{
	if (!pTarget || !pSrc || nSize == 0)
		return false;

	DWORD oldProt = 0;
	if (!VirtualProtect(pTarget, nSize, PAGE_EXECUTE_READWRITE, &oldProt))
		return false;

	std::memcpy(pTarget, pSrc, nSize);

	// Separate scratch -- never alias oldProt as both in and out.
	DWORD scratch = 0;
	VirtualProtect(pTarget, nSize, oldProt, &scratch);
	FlushInstructionCache(GetCurrentProcess(), pTarget, nSize);
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: reserve nSize bytes within +/-2GB of the module so RIP-relative
// disp32 from patched code stays in int32 range. Step 0x10000, bound 0x70000000.
// Returns nullptr on failure -- never asserts.
//-----------------------------------------------------------------------------
uint8_t* Mem_AllocNearModule(const CModule& mod, const size_t nSize)
{
	if (nSize == 0)
		return nullptr;

	const uintptr_t moduleBase = static_cast<uintptr_t>(mod.GetModuleBase());
	if (!moduleBase)
		return nullptr;

	for (int dir = 1; dir >= -1; dir -= 2)
	{
		uintptr_t scanAddr = (moduleBase + dir * 0x10000000ULL) & ~0xFFFFULL;
		for (int attempt = 0; attempt < 4096; ++attempt)
		{
			const uintptr_t tryAddr = scanAddr + dir * attempt * 0x10000ULL;
			const int64_t dist = static_cast<int64_t>(tryAddr) - static_cast<int64_t>(moduleBase);
			if (dist > 0x70000000LL || dist < -0x70000000LL)
				break;

			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(reinterpret_cast<void*>(tryAddr), &mbi, sizeof(mbi)) != sizeof(mbi))
				continue;
			if (mbi.State != MEM_FREE || mbi.RegionSize < nSize)
				continue;

			void* p = VirtualAlloc(reinterpret_cast<void*>(tryAddr), nSize,
				MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (p)
				return static_cast<uint8_t*>(p);
		}
	}
	return nullptr;
}
