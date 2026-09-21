//=============================================================================//
//
// Purpose: five-byte mid-function jump patch with a near trampoline.
//
// The site must be a `test r8, r8` (3 bytes) followed by a `jnz rel8` (2 bytes)
// with no live volatile register and RSP 16-byte aligned; the callee gets
// `this` from RDI and returns an int, where a negative value replays the
// displaced test/jnz and anything else is returned through the function's
// shared epilogue in EAX.
//
//=============================================================================//
#ifndef MIDFUNC_JUMP_PATCH_H
#define MIDFUNC_JUMP_PATCH_H

#include <windows.h>
#include <stdint.h>
#include <string.h>

typedef int (__fastcall* MidFuncSelectFn_t)(void* pThis);

class CMidFuncJumpPatch
{
public:
	CMidFuncJumpPatch(void) : m_pSite(nullptr), m_pTramp(nullptr), m_bInstalled(false) { memset(m_origBytes, 0, sizeof(m_origBytes)); }

	// pEpilogue: the function's shared return block (expects EAX).
	bool Install(uint8_t* const pSite, uint8_t* const pEpilogue, const MidFuncSelectFn_t pfnSelect)
	{
		if (m_bInstalled || !pSite || !pEpilogue || !pfnSelect)
			return m_bInstalled;
		if (pSite[0] != 0x45 && pSite[0] != 0x40)
			return false;
		if (pSite[1] != 0x84 || pSite[3] != 0x75)
			return false;

		uint8_t* const pJnzTarget = pSite + 5 + static_cast<int8_t>(pSite[4]);
		uint8_t* const pResume    = pSite + 5;

		m_pTramp = AllocNear(pSite, 64);
		if (!m_pTramp)
			return false;

		uint8_t* p = m_pTramp;
		p = Emit(p, "\x48\x83\xEC\x20", 4);           // sub rsp, 20h
		p = Emit(p, "\x48\x8B\xCF", 3);               // mov rcx, rdi
		p = Emit(p, "\x48\xB8", 2);                   // mov rax, imm64
		const uint64_t fn = reinterpret_cast<uint64_t>(pfnSelect);
		p = Emit(p, &fn, 8);
		p = Emit(p, "\xFF\xD0", 2);                   // call rax
		p = Emit(p, "\x48\x83\xC4\x20", 4);           // add rsp, 20h
		p = Emit(p, "\x85\xC0", 2);                   // test eax, eax
		p = Emit(p, "\x78\x05", 2);                   // js +5 (over the jmp below)
		p = EmitJmp(p, pEpilogue);                    // jmp epilogue (E9 rel32)
		p = Emit(p, pSite, 3);                        // replay: test r8, r8
		p = Emit(p, "\x0F\x85", 2);                   // jnz rel32
		p = EmitRel32(p, pJnzTarget);
		p = EmitJmp(p, pResume);                      // jmp site+5

		FlushInstructionCache(GetCurrentProcess(), m_pTramp, static_cast<size_t>(p - m_pTramp));

		uint8_t patch[5];
		patch[0] = 0xE9;
		const int32_t rel = static_cast<int32_t>(m_pTramp - (pSite + 5));
		memcpy(patch + 1, &rel, 4);

		m_nSiteLen = 5;
		memcpy(m_origBytes, pSite, 5);
		if (!WriteCode(pSite, patch, 5))
			return false;

		m_pSite = pSite;
		m_bInstalled = true;
		return true;
	}

	// Variant for a site whose displaced instructions are position-independent:
	// the first `replayLen` bytes are copied into the trampoline verbatim, `pTail`
	// (optional) is emitted after them, then control returns to site+replayLen.
	bool InstallReplay(uint8_t* const pSite, const size_t replayLen, const uint8_t* const pTail, const size_t tailLen,
		uint8_t* const pEpilogue, const MidFuncSelectFn_t pfnSelect)
	{
		if (m_bInstalled || !pSite || !pEpilogue || !pfnSelect || replayLen < 5 || replayLen > 16 || tailLen > 16)
			return m_bInstalled;

		m_pTramp = AllocNear(pSite, 96);
		if (!m_pTramp)
			return false;

		uint8_t* p = m_pTramp;
		p = Emit(p, "\x48\x83\xEC\x20", 4);           // sub rsp, 20h
		p = Emit(p, "\x48\x8B\xCF", 3);               // mov rcx, rdi
		p = Emit(p, "\x48\xB8", 2);                   // mov rax, imm64
		const uint64_t fn = reinterpret_cast<uint64_t>(pfnSelect);
		p = Emit(p, &fn, 8);
		p = Emit(p, "\xFF\xD0", 2);                   // call rax
		p = Emit(p, "\x48\x83\xC4\x20", 4);           // add rsp, 20h
		p = Emit(p, "\x85\xC0", 2);                   // test eax, eax
		p = Emit(p, "\x78\x05", 2);                   // js +5 (over the jmp below)
		p = EmitJmp(p, pEpilogue);
		p = Emit(p, pSite, replayLen);
		if (pTail && tailLen)
			p = Emit(p, pTail, tailLen);
		p = EmitJmp(p, pSite + replayLen);

		FlushInstructionCache(GetCurrentProcess(), m_pTramp, static_cast<size_t>(p - m_pTramp));

		uint8_t patch[16];
		memset(patch, 0x90, sizeof(patch));
		patch[0] = 0xE9;
		const int32_t rel = static_cast<int32_t>(m_pTramp - (pSite + 5));
		memcpy(patch + 1, &rel, 4);

		m_nSiteLen = replayLen;
		memcpy(m_origBytes, pSite, replayLen);
		if (!WriteCode(pSite, patch, static_cast<int>(replayLen)))
			return false;

		m_pSite = pSite;
		m_bInstalled = true;
		return true;
	}

	void Remove(void)
	{
		if (!m_bInstalled)
			return;
		WriteCode(m_pSite, m_origBytes, static_cast<int>(m_nSiteLen));
		m_bInstalled = false;
		m_pSite = nullptr;
	}

	bool IsInstalled(void) const { return m_bInstalled; }
	void* GetTrampoline(void) const { return m_pTramp; }

private:
	static uint8_t* Emit(uint8_t* p, const void* src, size_t len)
	{
		memcpy(p, src, len);
		return p + len;
	}
	static uint8_t* EmitRel32(uint8_t* p, const uint8_t* target)
	{
		const int32_t rel = static_cast<int32_t>(target - (p + 4));
		memcpy(p, &rel, 4);
		return p + 4;
	}
	static uint8_t* EmitJmp(uint8_t* p, const uint8_t* target)
	{
		*p++ = 0xE9;
		return EmitRel32(p, target);
	}

	// A rel32 jump reaches 2 GB either way; walk 64 KB granules outward from the site.
	static uint8_t* AllocNear(const uint8_t* const pSite, const size_t len)
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
		const uintptr_t base = reinterpret_cast<uintptr_t>(pSite) & ~(gran - 1);

		for (uintptr_t step = gran; step < 0x7FF00000ull; step += gran)
		{
			for (int dir = 0; dir < 2; ++dir)
			{
				const uintptr_t addr = dir ? base - step : base + step;
				if (addr < reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress) ||
					addr > reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress))
					continue;
				void* const mem = VirtualAlloc(reinterpret_cast<void*>(addr), len,
					MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (mem)
					return static_cast<uint8_t*>(mem);
			}
		}
		return nullptr;
	}

	static bool WriteCode(uint8_t* const site, const uint8_t* const bytes, const int len)
	{
		DWORD oldProt = 0;
		if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &oldProt))
			return false;
		memcpy(site, bytes, len);
		VirtualProtect(site, len, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), site, len);
		return true;
	}

	uint8_t* m_pSite;
	uint8_t* m_pTramp;
	uint8_t  m_origBytes[16];
	size_t   m_nSiteLen = 0;
	bool     m_bInstalled;
};

#endif // MIDFUNC_JUMP_PATCH_H
