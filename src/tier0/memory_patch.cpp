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
// disp32 from patched code stays in int32 range. Walks free regions outward
// from the module in both directions, region by region, up to 0x70000000.
// Returns nullptr on failure -- never asserts.
//-----------------------------------------------------------------------------
uint8_t* Mem_AllocNearModule(const CModule& mod, const size_t nSize)
{
	if (nSize == 0)
		return nullptr;

	const uintptr_t moduleBase = static_cast<uintptr_t>(mod.GetModuleBase());
	if (!moduleBase)
		return nullptr;

	constexpr uintptr_t kGranularity = 0x10000;
	constexpr int64_t   kMaxDistance = 0x70000000LL;
	const uintptr_t moduleEnd = moduleBase + static_cast<uintptr_t>(mod.GetModuleSize());

	for (int dir = 1; dir >= -1; dir -= 2)
	{
		uintptr_t cursor = (dir > 0)
			? ((moduleEnd + kGranularity - 1) & ~(kGranularity - 1))
			: (moduleBase & ~(kGranularity - 1));

		for (int guard = 0; guard < 65536; ++guard)
		{
			if (dir < 0)
			{
				if (cursor < kGranularity)
					break;
				cursor -= kGranularity;
			}

			const int64_t dist = static_cast<int64_t>(cursor) - static_cast<int64_t>(moduleBase);
			if (dist > kMaxDistance || dist < -kMaxDistance)
				break;

			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
				break;

			const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			const uintptr_t regionEnd = regionBase + mbi.RegionSize;

			if (mbi.State == MEM_FREE)
			{
				// Nearest end of the free region to the module.
				const uintptr_t tryAddr = (dir > 0)
					? ((cursor + kGranularity - 1) & ~(kGranularity - 1))
					: ((regionEnd >= nSize ? regionEnd - nSize : 0) & ~(kGranularity - 1));

				if (tryAddr >= regionBase && tryAddr + nSize <= regionEnd)
				{
					const int64_t tryDist = static_cast<int64_t>(tryAddr) - static_cast<int64_t>(moduleBase);
					if (tryDist <= kMaxDistance && tryDist >= -kMaxDistance)
					{
						void* p = VirtualAlloc(reinterpret_cast<void*>(tryAddr), nSize,
							MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
						if (p)
							return static_cast<uint8_t*>(p);
					}
				}
			}

			if (dir > 0)
				cursor = regionEnd;
			else
				cursor = regionBase;
		}
	}
	return nullptr;
}
