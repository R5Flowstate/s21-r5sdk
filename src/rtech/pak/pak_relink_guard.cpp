//=============================================================================//
//
// Purpose: Keep the pak relink drain from spinning forever.
//
// The drain relinks entries whose blockCount reached 0 and repeats until all
// are done; a reference cycle or self-use keeps an entry above 0 forever and
// the loop spins with the pak unload lock held. A pass that relinks nothing is
// reported with the stuck guids, then stuck entries are forced through.
//
//=============================================================================//

#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "rpak_observe.h"
#include "pak_relink_guard.h"

// Relink work area (the rcx of the drain): entries are { guid, blockCount }.
static constexpr ptrdiff_t kRelink_Entries = 0x410;
static constexpr ptrdiff_t kRelink_Count   = 0x39D010; // uint32
static constexpr ptrdiff_t kRelink_IsUnload = 0x8;     // nonzero = unload; also gates the lock
static constexpr __int64   kRelink_Done    = -1234;    // stamp the drain writes on a relinked entry

// Stall rounds that break one entry each before falling back to breaking all.
static constexpr unsigned kBreakOneRounds = 64;

struct RelinkEntry_t
{
	unsigned __int64 guid;
	__int64 blockCount;
};

struct RelinkDrainState_t
{
	void* pWork;
	unsigned nLastDone;
	unsigned nStallRounds;
	unsigned nForced;
	bool bReported;
};

static thread_local RelinkDrainState_t s_drain = { nullptr, UINT_MAX, 0, 0, false };
static volatile LONG s_nStalledDrains = 0;

static uint8_t* s_pSite = nullptr;
static uint8_t* s_pStub = nullptr;
static uint8_t s_origBytes[10] = {};

static void RelinkGuard_Reset(void)
{
	s_drain = { nullptr, UINT_MAX, 0, 0, false };
}

static void RelinkGuard_Report(uint8_t* pWork, unsigned nCount, const RelinkEntry_t* pEntries)
{
	const LONG nDrain = InterlockedIncrement(&s_nStalledDrains);
	unsigned nStuck = 0;
	for (unsigned i = 0; i < nCount; ++i)
	{
		if (pEntries[i].blockCount != kRelink_Done)
			++nStuck;
	}

	Warning(eDLL_T::RTECH,
		"[PAK-RELINK] ******** relink drain STALLED (#%ld, %s): %u of %u assets can never relink -- "
		"reference cycle or self-use in a loaded pak. Forcing them through so the game does not hang. "
		"Report this log. ********\n",
		nDrain, *reinterpret_cast<const __int64*>(pWork + kRelink_IsUnload) ? "unload" : "load",
		nStuck, nCount);

	unsigned nListed = 0;
	for (unsigned i = 0; i < nCount && nListed < 32; ++i)
	{
		if (pEntries[i].blockCount == kRelink_Done)
			continue;
		Warning(eDLL_T::RTECH, "[PAK-RELINK]   stuck guid 0x%016llX blockCount=%lld\n",
			pEntries[i].guid, pEntries[i].blockCount);
		if (nListed < 8)
			Pak_DumpGuidChain_S21(pEntries[i].guid, "relink-stuck");
		++nListed;
	}
	if (nStuck > nListed)
		Warning(eDLL_T::RTECH, "[PAK-RELINK]   ... %u more not listed\n", nStuck - nListed);
}

// Called at the end of every drain pass. Returns true to run another pass,
// false to leave the drain (unlock and return).
static bool __fastcall RelinkGuard_OnPassEnd(uint8_t* pWork, unsigned nDone, unsigned nCount)
{
	if (nDone == nCount)
	{
		if (s_drain.bReported)
		{
			Warning(eDLL_T::RTECH, "[PAK-RELINK] stalled drain finished: %u assets forced over %u stall rounds\n",
				s_drain.nForced, s_drain.nStallRounds);
		}
		RelinkGuard_Reset();
		return false;
	}

	if (s_drain.pWork != pWork)
		s_drain = { pWork, UINT_MAX, 0, 0, false };

	if (nDone != s_drain.nLastDone)
	{
		s_drain.nLastDone = nDone;
		return true;
	}

	RelinkEntry_t* const pEntries = reinterpret_cast<RelinkEntry_t*>(pWork + kRelink_Entries);
	if (!s_drain.bReported)
	{
		RelinkGuard_Report(pWork, nCount, pEntries);
		s_drain.bReported = true;
	}

	// Each stall round relinks at least one entry, so this only trips on a
	// work area that is not laid out as expected.
	if (++s_drain.nStallRounds > nCount + kBreakOneRounds + 4)
	{
		Warning(eDLL_T::RTECH,
			"[PAK-RELINK] ******** giving up after %u stall rounds: %u of %u assets left unrelinked ********\n",
			s_drain.nStallRounds, nCount - nDone, nCount);
		RelinkGuard_Reset();
		return false;
	}

	// One entry at a time keeps the rest draining in dependency order; the
	// lowest-index stuck entry is always relinked on the next pass.
	const bool bBreakAll = s_drain.nStallRounds > kBreakOneRounds;
	for (unsigned i = 0; i < nCount; ++i)
	{
		if (pEntries[i].blockCount == kRelink_Done || pEntries[i].blockCount == 0)
			continue;
		pEntries[i].blockCount = 0;
		++s_drain.nForced;
		if (!bBreakAll)
			break;
	}

	s_drain.nLastDone = nDone;
	return true;
}

static void* RelinkGuard_AllocNear(uintptr_t target, size_t size)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	const uintptr_t gran = si.dwAllocationGranularity;
	const uintptr_t range = 0x70000000;

	for (uintptr_t addr = (target & ~(gran - 1)) + gran; addr < target + range; addr += gran)
	{
		if (void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
			return p;
	}
	for (uintptr_t addr = (target & ~(gran - 1)) - gran; addr > target - range; addr -= gran)
	{
		if (void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
			return p;
	}
	return nullptr;
}

static int32_t RelinkGuard_Rel32(const uint8_t* from, size_t insnLen, const uint8_t* to)
{
	return static_cast<int32_t>(reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from + insnLen));
}

void VPakRelinkGuard::GetAdr(void) const
{
	LogFunAdr("PakRelink_PassEnd", s_pSite);
}

void VPakRelinkGuard::GetFun(void) const
{
	// Drain pass tail (identical in the DX11 and DX12 client): reload count,
	// step the entry cursor, then "mov edx,eax; cmp ebx,eax; jne passStart"
	// (ebx = relinked so far) ahead of the unload-lock epilogue.
	CMemory tail = Module_FindPattern(g_GameDll,
		"8B 85 10 D0 39 00 49 FF C7 48 83 C6 10 4C 89 BC 24 A8 00 00 00 "
		"48 89 74 24 30 4C 3B F8 0F 82 ?? ?? ?? ?? "
		"8B D0 3B D8 0F 85 ?? ?? ?? ?? 48 83 7D 08 00");

	if (!tail)
	{
		Warning(eDLL_T::RTECH, "[PAK-RELINK] drain tail pattern unresolved -- relink stall guard DISABLED\n");
		return;
	}
	s_pSite = tail.Offset(35).RCast<uint8_t*>();
}

void VPakRelinkGuard::Detour(const bool bAttach) const
{
	if (!s_pSite)
		return;

	DWORD oldProt;
	if (!bAttach)
	{
		if (!s_pStub)
			return;
		if (VirtualProtect(s_pSite, sizeof(s_origBytes), PAGE_EXECUTE_READWRITE, &oldProt))
		{
			memcpy(s_pSite, s_origBytes, sizeof(s_origBytes));
			VirtualProtect(s_pSite, sizeof(s_origBytes), oldProt, &oldProt);
			FlushInstructionCache(GetCurrentProcess(), s_pSite, sizeof(s_origBytes));
		}
		return;
	}

	if (s_pStub)
		return;

	memcpy(s_origBytes, s_pSite, sizeof(s_origBytes));
	const uint8_t* const pPassStart = s_pSite + 10 + *reinterpret_cast<const int32_t*>(s_pSite + 6);
	const uint8_t* const pEpilogue = s_pSite + 10;

	uint8_t* const stub = static_cast<uint8_t*>(RelinkGuard_AllocNear(reinterpret_cast<uintptr_t>(s_pSite), 128));
	if (!stub)
	{
		Warning(eDLL_T::RTECH, "[PAK-RELINK] no stub memory near the drain -- relink stall guard DISABLED\n");
		return;
	}

	// Volatile regs are saved around the call: the pass start reads edx
	// (count) and r9-r11 (table bases set up before the loop).
	static const uint8_t kHead[] =
	{
		0x8B, 0xD0,                         // mov edx, eax
		0x50, 0x51, 0x52,                   // push rax, rcx, rdx
		0x41, 0x50, 0x41, 0x51,             // push r8, r9
		0x41, 0x52, 0x41, 0x53,             // push r10, r11
		0x48, 0x83, 0xEC, 0x28,             // sub rsp, 28h
		0x48, 0x8B, 0xCD,                   // mov rcx, rbp
		0x8B, 0xD3,                         // mov edx, ebx
		0x44, 0x8B, 0xC0,                   // mov r8d, eax
		0x48, 0xB8,                         // mov rax, imm64
	};
	static const uint8_t kTail[] =
	{
		0xFF, 0xD0,                         // call rax
		0x48, 0x83, 0xC4, 0x28,             // add rsp, 28h
		0x84, 0xC0,                         // test al, al
		0x41, 0x5B, 0x41, 0x5A,             // pop r11, r10
		0x41, 0x59, 0x41, 0x58,             // pop r9, r8
		0x5A, 0x59, 0x58,                   // pop rdx, rcx, rax
	};

	size_t n = 0;
	memcpy(stub + n, kHead, sizeof(kHead)); n += sizeof(kHead);
	const uintptr_t fn = reinterpret_cast<uintptr_t>(&RelinkGuard_OnPassEnd);
	memcpy(stub + n, &fn, sizeof(fn)); n += sizeof(fn);
	memcpy(stub + n, kTail, sizeof(kTail)); n += sizeof(kTail);

	stub[n++] = 0x0F; stub[n++] = 0x85;     // jnz passStart
	const int32_t relPass = RelinkGuard_Rel32(stub + n - 2, 6, pPassStart);
	memcpy(stub + n, &relPass, 4); n += 4;

	stub[n++] = 0xE9;                       // jmp epilogue
	const int32_t relEpi = RelinkGuard_Rel32(stub + n - 1, 5, pEpilogue);
	memcpy(stub + n, &relEpi, 4); n += 4;

	VirtualProtect(stub, 128, PAGE_EXECUTE_READ, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), stub, 128);

	uint8_t patch[10] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
	const int32_t relStub = RelinkGuard_Rel32(s_pSite, 5, stub);
	memcpy(&patch[1], &relStub, 4);

	if (!VirtualProtect(s_pSite, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::RTECH, "[PAK-RELINK] VirtualProtect failed -- relink stall guard DISABLED\n");
		return;
	}
	memcpy(s_pSite, patch, sizeof(patch));
	VirtualProtect(s_pSite, sizeof(patch), oldProt, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), s_pSite, sizeof(patch));

	s_pStub = stub;
	Msg(eDLL_T::RTECH, "[PAK-RELINK] relink stall guard armed at %p (stub %p)\n", s_pSite, stub);
}
