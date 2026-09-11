//=============================================================================//
//
// Purpose: Free the live CNetChan receive buffer before a first-fragment
// new[] overwrites the pointer. Native write is bounded to the allocation
// (cap 0x40000); the leftover is a leak if a new first fragment stores
// without freeing the previous buffer.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/memstd.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "netchan_relrecv_free.h"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// dataFragments_t::buffer on the S3 CNetChan receive list.
static constexpr ptrdiff_t NETCHAN_RECV_BUFFER = 0x118;

static uint8_t* s_pStoreSite = nullptr;
static uint8_t* s_pThunk = nullptr;
static uint8_t s_origStore[7] = {};
static bool s_bPatched = false;
static void* s_pReadReliable = nullptr;

static ConVar sdk_netchan_relrecv_free("sdk_netchan_relrecv_free", "1",
	FCVAR_RELEASE,
	"Free the live reliable-receive buffer before a first-fragment store. 0 = stock.");

static void RelRecv_WarnFree(void* pOld, void* pNew)
{
	static int s_nWarns = 0;
	if (s_nWarns >= 16)
		return;
	++s_nWarns;
	Warning(eDLL_T::ENGINE, "[RELRECV-FREE] freed stale recv buf %p (new %p)\n",
		pOld, pNew);
}

static void RelRecv_StoreAndFree(void* pChan, void* pNew)
{
	if (!pChan)
		return;

	void** const ppBuf = reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(pChan) + NETCHAN_RECV_BUFFER);
	void* const pOld = *ppBuf;

	if (pOld && pOld != pNew && sdk_netchan_relrecv_free.GetBool())
	{
		IMemAlloc* const pAlloc = MemAllocSingleton();
		if (pAlloc)
		{
			pAlloc->Free(pOld);
			RelRecv_WarnFree(pOld, pNew);
		}
	}

	*ppBuf = pNew;
}

static bool RelRecv_WriteBytes(void* addr, const void* data, size_t len)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::ENGINE,
			"[RELRECV-FREE] VirtualProtect failed @ %p (gle=%lu)\n",
			addr, GetLastError());
		return false;
	}
	memcpy(addr, data, len);
	DWORD restored = 0;
	VirtualProtect(addr, len, oldProt, &restored);
	FlushInstructionCache(GetCurrentProcess(), addr, len);
	return true;
}

static void* RelRecv_AllocNear(void* pNear, size_t nSize)
{
	SYSTEM_INFO si = {};
	GetSystemInfo(&si);
	const uintptr_t nGran = si.dwAllocationGranularity
		? static_cast<uintptr_t>(si.dwAllocationGranularity)
		: 0x10000u;
	const uintptr_t nBase = reinterpret_cast<uintptr_t>(pNear);
	const uintptr_t nLimit = 0x70000000u;

	for (uintptr_t off = 0; off < nLimit; off += nGran)
	{
		if (off != 0)
		{
			const uintptr_t nUp = nBase + off;
			if (nUp > nBase)
			{
				void* const pUp = VirtualAlloc(reinterpret_cast<void*>(nUp), nSize,
					MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (pUp)
					return pUp;
			}
		}

		if (nBase > off)
		{
			const uintptr_t nDown = nBase - off;
			if (nDown >= 0x10000u)
			{
				void* const pDown = VirtualAlloc(reinterpret_cast<void*>(nDown), nSize,
					MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (pDown)
					return pDown;
			}
		}

		if (off == 0)
		{
			void* const pExact = VirtualAlloc(pNear, nSize,
				MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (pExact)
				return pExact;
		}
	}

	return nullptr;
}

static bool RelRecv_Rel32Ok(const uint8_t* pFrom, const uint8_t* pTo)
{
	const intptr_t nRel = reinterpret_cast<const uint8_t*>(pTo)
		- (pFrom + 5);
	return nRel >= INT32_MIN && nRel <= INT32_MAX;
}

void VNetChanRelRecvFree::GetAdr(void) const
{
	LogFunAdr("CNetChan::ReadReliableMessages", s_pReadReliable);
	LogVarAdr("CNetChan::ReadReliableMessages/store", s_pStoreSite);
}

void VNetChanRelRecvFree::GetFun(void) const
{
	// Unique prologue: push frame + sub rsp,290h.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 "
		"57 41 54 41 55 41 56 41 57 48 81 EC 90 02 00 00")
		.GetPtr(s_pReadReliable);

	if (!s_pReadReliable)
		Warning(eDLL_T::ENGINE, "[RELRECV-FREE] ReadReliableMessages pattern unresolved\n");

	// First-fragment store: mov [r13+144h],edi; mov ecx,edi; mov [r13+118h],rax
	CMemory storeCtx = Module_FindPattern(g_GameDll,
		"41 89 BD 44 01 00 00 8B CF 49 89 85 18 01 00 00");
	if (!storeCtx)
	{
		Warning(eDLL_T::ENGINE, "[RELRECV-FREE] first-fragment store pattern unresolved\n");
		return;
	}

	s_pStoreSite = reinterpret_cast<uint8_t*>(storeCtx.GetPtr()) + 9;
}

void VNetChanRelRecvFree::Detour(const bool bAttach) const
{
	if (!s_pStoreSite)
		return;

	if (!bAttach)
	{
		if (s_bPatched)
		{
			RelRecv_WriteBytes(s_pStoreSite, s_origStore, sizeof(s_origStore));
			s_bPatched = false;
		}
		if (s_pThunk)
		{
			VirtualFree(s_pThunk, 0, MEM_RELEASE);
			s_pThunk = nullptr;
		}
		return;
	}

	if (s_bPatched)
		return;

	memcpy(s_origStore, s_pStoreSite, sizeof(s_origStore));

	s_pThunk = static_cast<uint8_t*>(RelRecv_AllocNear(s_pStoreSite, 64));
	if (!s_pThunk)
	{
		Warning(eDLL_T::ENGINE, "[RELRECV-FREE] near-alloc failed; store left stock\n");
		return;
	}

	if (!RelRecv_Rel32Ok(s_pStoreSite, s_pThunk))
	{
		Warning(eDLL_T::ENGINE, "[RELRECV-FREE] thunk out of rel32 range; store left stock\n");
		VirtualFree(s_pThunk, 0, MEM_RELEASE);
		s_pThunk = nullptr;
		return;
	}

	// rcx=r13 (CNetChan thisptr). r12 is the compressed flag at this site
	// (stored to [r13+128h] immediately after). rdx=rax (new buf).
	// Restore ecx=edi after the helper; continuation lea eax,[rcx+rbp] needs it.
	// sub rsp,28h: nested x64 call needs 0x20 shadow plus 8-byte alignment.
	uint8_t thunk[29] = {
		0x48, 0x83, 0xEC, 0x28,
		0x4C, 0x89, 0xE9,
		0x48, 0x89, 0xC2,
		0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
		0xFF, 0xD0,
		0x48, 0x83, 0xC4, 0x28,
		0x89, 0xF9,
		0xC3
	};
	const uint64_t nFn = reinterpret_cast<uint64_t>(&RelRecv_StoreAndFree);
	memcpy(thunk + 12, &nFn, sizeof(nFn));
	memcpy(s_pThunk, thunk, sizeof(thunk));
	FlushInstructionCache(GetCurrentProcess(), s_pThunk, sizeof(thunk));

	const intptr_t nRel = s_pThunk - (s_pStoreSite + 5);
	uint8_t patch[7] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
	const int32_t nRel32 = static_cast<int32_t>(nRel);
	memcpy(patch + 1, &nRel32, sizeof(nRel32));

	if (!RelRecv_WriteBytes(s_pStoreSite, patch, sizeof(patch)))
	{
		VirtualFree(s_pThunk, 0, MEM_RELEASE);
		s_pThunk = nullptr;
		return;
	}

	s_bPatched = true;
	Msg(eDLL_T::ENGINE, "[RELRECV-FREE] first-fragment store hooked @ %p\n",
		s_pStoreSite);
}
