//=============================================================================//
//
// Purpose: Implementation of HeapCanary tail tracking.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "tier1/convar.h"
#include "thirdparty/detours/include/detours.h"
#include "heap_canary.h"

#include <vector>
#include <string>
#include <cstring>

// Default off. Re-arm with +sdk_heap_canary 1.
static ConVar sdk_heap_canary("sdk_heap_canary", "0", FCVAR_DEVELOPMENTONLY,
	"Arm the heap-corruption diagnostic (tail-canary registration + per-tick "
	"mspace tree-bin walker + per-alloc mspace_malloc/free validators). "
	"0 = dormant, no runtime cost (default); 1 = on.");

static ConVar sdk_canary_period("sdk_canary_period", "0", FCVAR_DEVELOPMENTONLY,
	"When >0, PollTick runs Checkpoint(\"periodic\") every N frames. "
	"0 = off (default). Observe-only.");

static void CC_SdkCanaryCheck_f(const CCommand& args)
{
	(void)args;
	HeapCanary::DumpAll();
}
static ConCommand sdk_canary_check("sdk_canary_check", CC_SdkCanaryCheck_f,
	"Scan every registered heap-canary tail and dump stomped entries.",
	FCVAR_DEVELOPMENTONLY);

namespace
{
	struct Entry
	{
		std::string name;
		void*       canaryAt;     // start of the canary bytes
		size_t      canaryBytes;  // length of the canary
		// origBase / origSize stored for log clarity only.
		void*       origBase;
		size_t      origBytes;
	};

	// Single-threaded: SDK expansion module init runs on the main thread.
	static std::vector<Entry> s_entries;

	static inline void WritePattern(uint8_t* dst, size_t n)
	{
		for (size_t i = 0; i + 8 <= n; i += 8)
			*reinterpret_cast<uint64_t*>(dst + i) = HeapCanary::kTailPattern;
	}
}

namespace HeapCanary
{

void RegisterTail(const char* name, void* bufStart, size_t bufBytes)
{
	if (!sdk_heap_canary.GetBool()) // dormant: no canary, no [CANARY] prints
		return;
	if (!name || !bufStart || bufBytes == 0)
		return;

	uint8_t* tail = static_cast<uint8_t*>(bufStart) + bufBytes;
	WritePattern(tail, kTailBytes);

	Entry e;
	e.name        = name;
	e.canaryAt    = tail;
	e.canaryBytes = kTailBytes;
	e.origBase    = bufStart;
	e.origBytes   = bufBytes;
	s_entries.push_back(std::move(e));
}

void Unregister(void* bufStart)
{
	if (!sdk_heap_canary.GetBool())
		return;
	if (!bufStart)
		return;

	for (size_t i = s_entries.size(); i > 0; --i)
	{
		const size_t idx = i - 1;
		if (s_entries[idx].origBase == bufStart)
			s_entries.erase(s_entries.begin() + static_cast<std::ptrdiff_t>(idx));
	}
}

bool Armed(void)
{
	return sdk_heap_canary.GetBool();
}

void RegisterRegion(const char* name, void* regionStart, size_t regionBytes)
{
	if (!sdk_heap_canary.GetBool()) // dormant: no canary, no [CANARY] prints
		return;
	if (!name || !regionStart || regionBytes < 8)
		return;

	WritePattern(static_cast<uint8_t*>(regionStart), regionBytes);

	Entry e;
	e.name        = name;
	e.canaryAt    = regionStart;
	e.canaryBytes = regionBytes;
	e.origBase    = regionStart;
	e.origBytes   = regionBytes;
	s_entries.push_back(std::move(e));
}

int CheckAll()
{
	int stomped = 0;
	for (const Entry& e : s_entries)
	{
		const uint8_t* tail = static_cast<const uint8_t*>(e.canaryAt);
		for (size_t i = 0; i + 8 <= e.canaryBytes; i += 8)
		{
			uint64_t v = *reinterpret_cast<const uint64_t*>(tail + i);
			if (v != kTailPattern)
			{
				Warning(eDLL_T::ENGINE,
					"[CANARY] STOMPED '%s' base=0x%p size=%zu "
					"canary[+%zu]=0x%016llX (expected 0x%016llX)\n",
					e.name.c_str(), e.origBase, e.origBytes, i,
					static_cast<unsigned long long>(v),
					static_cast<unsigned long long>(kTailPattern));
				++stomped;
				break; // one report per buffer is enough
			}
		}
	}
	return stomped;
}

void DumpAll()
{
	Msg(eDLL_T::ENGINE,
		"[CANARY] === scanning %zu registered buffers ===\n",
		s_entries.size());
	const int stomped = CheckAll();
	Msg(eDLL_T::ENGINE,
		"[CANARY] === %d stomped of %zu ===\n",
		stomped, s_entries.size());
}

// dlmalloc tree-bin roots at mspace+600. 0x8001/0x8009 is an overwritten chunk header.
namespace
{
	// Set at InitMspaceMonitor; remains nullptr if pattern-find failed.
	static void** s_ppMspaceGlobal = nullptr;
	static constexpr ptrdiff_t kTreebinOffset = 600; // bytes from mspace base
	static constexpr int       kNumTreebins   = 32;

	// Any value below this is clearly NOT a userspace heap pointer; valid
	// chunks live well above 64 KB.
	static constexpr uint64_t kMinPlausiblePtr  = 0x10000ULL;
	// Upper bound for plausible user-mode pointers on Windows x64 (47-bit).
	static constexpr uint64_t kMaxPlausiblePtr  = 0x7FFFFFFFFFFFULL;

	// mspace_free hook state.
	static void  (*v_mspace_free)(void*) = nullptr;
	static uint64_t* s_pMagicCookie = nullptr;
	// Size-field mask paired with the matched mspace_free pattern.
	// S3 dedi: 0xFFFFFFFFFFFFFFF8; S21 client: 0x001FFFFFFFFFFFF8.
	static uint64_t s_mspaceSizeMask = 0xFFFFFFFFFFFFFFF8ull;
	static bool   s_mspaceFreeLatched = false;

	// mspace_malloc hook state.
	static void* (*v_mspace_malloc)(void*, size_t) = nullptr;
	static bool   s_mallocTreeLatched = false;

	// CRT abort hook (game-image body, not an import thunk).
	static void (*v_abort)(void) = nullptr;
	static volatile LONG s_abortReporting = 0;
}

// mspace_free: replicate the cookie XOR check and log before the abort.
static void __fastcall Hook_mspace_free(void* userPtr)
{
	if (userPtr && !s_mspaceFreeLatched && s_pMagicCookie)
	{
		__try
		{
			uintptr_t a1 = reinterpret_cast<uintptr_t>(userPtr);
			uint64_t sizeField = *reinterpret_cast<uint64_t*>(a1 - 8);
			uint64_t cleanSize = sizeField & s_mspaceSizeMask;
			// Engine reads at: (size_with_flags_masked) + (a1 - 16) =
			// chunk_start + size = the boundary just past chunk end.
			uintptr_t footerAddr = cleanSize + (a1 - 16);
			uint64_t footerVal = *reinterpret_cast<uint64_t*>(footerAddr);
			uint64_t cookie    = *s_pMagicCookie;
			uint64_t mspacePtr = footerVal ^ cookie;

			uint64_t cookieAt40 = 0;
			bool cookieAt40Ok = false;
			if (mspacePtr >= kMinPlausiblePtr && mspacePtr <= kMaxPlausiblePtr)
			{
				cookieAt40 = *reinterpret_cast<uint64_t*>(mspacePtr + 0x40);
				cookieAt40Ok = (cookieAt40 == cookie);
			}

			const bool rangeBad = (mspacePtr < kMinPlausiblePtr || mspacePtr > kMaxPlausiblePtr);
			const bool cookieBad = !rangeBad && !cookieAt40Ok;

			if (rangeBad || cookieBad)
			{
				s_mspaceFreeLatched = true;
				Warning(eDLL_T::ENGINE,
					"[CANARY-MSPACE-FREE] !!! CORRUPT CHUNK FOOTER !!!\n"
					"  userPtr     = 0x%p\n"
					"  chunkStart  = 0x%p\n"
					"  sizeField   = 0x%016llX (cleanSize=%llu / 0x%llX bytes)\n"
					"  sizeMask    = 0x%016llX\n"
					"  footerAddr  = 0x%p\n"
					"  footerVal   = 0x%016llX\n"
					"  cookie      = 0x%016llX\n"
					"  decodedMspc = 0x%016llX%s\n"
					"  mspc+0x40   = 0x%016llX (expected cookie 0x%016llX)%s\n",
					userPtr, reinterpret_cast<void*>(a1 - 16),
					(unsigned long long)sizeField,
					(unsigned long long)cleanSize,
					(unsigned long long)cleanSize,
					(unsigned long long)s_mspaceSizeMask,
					reinterpret_cast<void*>(footerAddr),
					(unsigned long long)footerVal,
					(unsigned long long)cookie,
					(unsigned long long)mspacePtr,
					rangeBad ? " (OUT OF RANGE)" : "",
					(unsigned long long)cookieAt40,
					(unsigned long long)cookie,
					cookieBad ? " (MISMATCH)" : "");

				// Hex-dump the start of user data so we can identify what
				// struct was allocated. First 8 qwords often contain a
				// vtable pointer or recognizable values.
				__try
				{
					Warning(eDLL_T::ENGINE,
						"  user data dump (first 128 bytes from userPtr):\n");
					const uint8_t* p = static_cast<const uint8_t*>(userPtr);
					for (int row = 0; row < 8; ++row)
					{
						uint64_t qw0 = *reinterpret_cast<const uint64_t*>(p + row * 16);
						uint64_t qw1 = *reinterpret_cast<const uint64_t*>(p + row * 16 + 8);
						Warning(eDLL_T::ENGINE,
							"    +0x%03X: %016llX %016llX\n",
							row * 16,
							(unsigned long long)qw0,
							(unsigned long long)qw1);
					}
					// Also dump the last 64 bytes of user data (closer to the
					// stomp boundary -- overflows write here).
					if (cleanSize > 16 + 64)
					{
						Warning(eDLL_T::ENGINE,
							"  user data tail (last 64 bytes before footer):\n");
						const uint8_t* tailBase = static_cast<const uint8_t*>(userPtr) +
							(cleanSize - 16 - 64);
						for (int row = 0; row < 4; ++row)
						{
							uint64_t qw0 = *reinterpret_cast<const uint64_t*>(tailBase + row * 16);
							uint64_t qw1 = *reinterpret_cast<const uint64_t*>(tailBase + row * 16 + 8);
							Warning(eDLL_T::ENGINE,
								"    +0x%03X: %016llX %016llX\n",
								(int)(cleanSize - 16 - 64 + row * 16),
								(unsigned long long)qw0,
								(unsigned long long)qw1);
						}
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER) { /* truncated dump */ }
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// SEH'd to avoid double-faulting before we can log.
		}
	}
	v_mspace_free(userPtr);
}

static bool CheckMspaceTreebins(uint64_t& outBadSlot, int& outBadIdx,
                                void*& outSlotAddr, void*& outMspace,
                                const char*& outWhere);

// abort hook: backtrace + canary + tree-bin walk, then call through. Latched; SEH'd.
static void __cdecl Hook_abort(void)
{
	if (InterlockedCompareExchange(&s_abortReporting, 1, 0) == 0)
	{
		__try
		{
			const uintptr_t modBase = g_GameDll.GetModuleBase();
			Warning(eDLL_T::ENGINE,
				"[CANARY-ABORT] process is aborting -- almost always "
				"allocator-detected heap corruption\n");

			void* frames[32] = {};
			const USHORT nFrames = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
			Warning(eDLL_T::ENGINE,
				"[CANARY-ABORT] backtrace (%u frames, moduleBase=0x%p):\n",
				(unsigned)nFrames, reinterpret_cast<void*>(modBase));
			for (USHORT i = 0; i < nFrames; ++i)
			{
				const uintptr_t abs = reinterpret_cast<uintptr_t>(frames[i]);
				const ptrdiff_t off = static_cast<ptrdiff_t>(abs - modBase);
				Warning(eDLL_T::ENGINE,
					"[CANARY-ABORT]   #%02u abs=0x%p off=0x%llX\n",
					(unsigned)i, frames[i],
					(unsigned long long)(abs >= modBase ? static_cast<uint64_t>(off) : 0));
			}

			HeapCanary::CheckAll();

			uint64_t badSlot = 0;
			int      badIdx = -1;
			void*    slotAddr = nullptr;
			void*    mspace = nullptr;
			const char* where = "?";
			if (!s_ppMspaceGlobal)
			{
				Warning(eDLL_T::ENGINE,
					"[CANARY-ABORT] mspace monitor unarmed -- no tree walk\n");
			}
			else if (CheckMspaceTreebins(badSlot, badIdx, slotAddr, mspace, where))
			{
				Warning(eDLL_T::ENGINE,
					"[CANARY-ABORT] TREE CORRUPT mspace=0x%p where='%s' bin=%d "
					"badval=0x%016llX slot=0x%p (HW BP target)\n",
					mspace, where, badIdx, (unsigned long long)badSlot, slotAddr);
			}
			else
			{
				Warning(eDLL_T::ENGINE,
					"[CANARY-ABORT] tree walk found no bad node -- corruption is "
					"in chunk headers or a non-tree bin\n");
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// never mask the original abort
		}
	}

	if (v_abort)
		v_abort();
}

// mspace_malloc: validate only treebins[v8] the engine is about to read.
static void* __fastcall Hook_mspace_malloc(void* mspace, size_t size)
{
	if (!s_mallocTreeLatched && s_ppMspaceGlobal && size > 0xE0 && mspace)
	{
		__try
		{
			const size_t aligned = (size + 31) & ~15ULL;
			const uint64_t v3 = aligned >> 8;
			int v8;
			if (v3 == 0)             v8 = 0;
			else if (v3 > 0xFFFFu)   v8 = 31;
			else
			{
				unsigned long lz;
				_BitScanReverse(&lz, static_cast<unsigned long>(v3));
				v8 = static_cast<int>(((aligned >> (lz + 7)) & 1u) + 2u * lz);
			}

			const uint64_t* treebins = reinterpret_cast<const uint64_t*>(
				static_cast<const uint8_t*>(mspace) + kTreebinOffset);
			const uint64_t root = treebins[v8];

			if (root != 0 && (root < kMinPlausiblePtr || root > kMaxPlausiblePtr))
			{
				s_mallocTreeLatched = true;
				Warning(eDLL_T::ENGINE,
					"[CANARY-TMALLOC-ROOT] !!! TREEBIN ROOT CORRUPT !!!\n"
					"  mspace      = 0x%p\n"
					"  size        = %zu (aligned=%zu)\n"
					"  bin idx     = %d (v8)\n"
					"  root slot   = 0x%p\n"
					"  root value  = 0x%016llX (expected NULL or valid ptr)\n"
					"-- engine is about to walk this bin and AV.\n",
					mspace, size, aligned, v8,
					reinterpret_cast<const void*>(&treebins[v8]),
					(unsigned long long)root);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
	}
	return v_mspace_malloc(mspace, size);
}

void InitMspaceMonitor()
{
	// mspace_malloc prologue. Follow the E8 to mspace_init for the mspace pointer.
	CMemory mspaceMalloc = Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 55 48 83 EC 20 48 8B EA 48 8B D9 48 85 C9 75 ?? E8");
	if (!mspaceMalloc)
	{
		Warning(eDLL_T::ENGINE,
			"[CANARY-MSPACE] mspace_malloc pattern not found -- treebin "
			"monitor disabled\n");
		return;
	}

	// At func+0x1F, the instruction `mov, rax` is 7 bytes
	// `48 89 05 disp32`. disp32 sits at func+0x22.
	uint8_t* p = mspaceMalloc.Offset(0x1F).RCast<uint8_t*>();
	if (p[0] != 0x48 || p[1] != 0x89 || p[2] != 0x05)
	{
		Warning(eDLL_T::ENGINE,
			"[CANARY-MSPACE] mspace_malloc +0x1F header mismatch "
			"(got %02X %02X %02X, expected 48 89 05) -- monitor disabled\n",
			p[0], p[1], p[2]);
		return;
	}
	int32_t disp32 = *reinterpret_cast<int32_t*>(p + 3);
	uintptr_t nextRIP = reinterpret_cast<uintptr_t>(p) + 7;
	s_ppMspaceGlobal = reinterpret_cast<void**>(nextRIP + disp32);

	Msg(eDLL_T::ENGINE,
		"[CANARY-MSPACE] monitor armed: mspace_malloc=0x%p, "
		"mspace global slot=0x%p\n",
		mspaceMalloc.GetPtr(), static_cast<void*>(s_ppMspaceGlobal));

	// mspace_free: S3 cookie at fn+0x27 mask ~7; S21 cookie at fn+0x36 mask 0x001FFFFFFFFFFFF8.
	CMemory mspaceFree = Module_FindPattern(g_GameDll,
		"48 83 EC 28 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 41 F8 48 89 5C 24 30 "
		"48 83 E0 F8 48 89 7C 24 20 48 8D 79 F0 48 8B 1C 38 48 8B 05");
	ptrdiff_t cookieOff = 0x27;
	const char* freeBuild = "S3";
	if (mspaceFree)
	{
		s_mspaceSizeMask = 0xFFFFFFFFFFFFFFF8ull;
	}
	else
	{
		// S21 client: trailing mov r12, 1FFFFFFFFFFFF8h is the unique anchor.
		mspaceFree = Module_FindPattern(g_GameDll,
			"48 8B C4 48 83 EC 38 48 85 C9 0F 84 ?? ?? ?? ?? 48 89 58 08 48 89 68 10 "
			"48 89 70 18 48 89 78 20 4C 89 60 F8 49 BC F8 FF FF FF FF FF 1F 00");
		cookieOff = 0x36;
		freeBuild = "S21";
		s_mspaceSizeMask = 0x001FFFFFFFFFFFF8ull;
	}
	if (!mspaceFree)
	{
		Warning(eDLL_T::ENGINE,
			"[CANARY-MSPACE-FREE] mspace_free pattern not found -- "
			"chunk validator disabled\n");
		return;
	}

	// Cookie load: `48 8B 0D/05 disp32` (7 bytes); RIP base is the next insn.
	// S21 client uses rcx form (0D); the S3 dedicated server build uses rax (05).
	uint8_t* mfp = mspaceFree.Offset(cookieOff).RCast<uint8_t*>();
	if (!(mfp[0] == 0x48 && mfp[1] == 0x8B && (mfp[2] == 0x0D || mfp[2] == 0x05)))
	{
		Warning(eDLL_T::ENGINE,
			"[CANARY-MSPACE-FREE] mspace_free +0x%X header mismatch "
			"(got %02X %02X %02X, expected 48 8B 0D/05) -- validator disabled\n",
			(unsigned)cookieOff, mfp[0], mfp[1], mfp[2]);
		return;
	}
	int32_t mfDisp = *reinterpret_cast<int32_t*>(mfp + 3);
	uintptr_t mfNextRIP = reinterpret_cast<uintptr_t>(mfp) + 7;
	s_pMagicCookie = reinterpret_cast<uint64_t*>(mfNextRIP + mfDisp);

	v_mspace_free = reinterpret_cast<void (*)(void*)>(mspaceFree.GetPtr());
	v_mspace_malloc = reinterpret_cast<void* (*)(void*, size_t)>(mspaceMalloc.GetPtr());

	// CRT abort in the game image (unique on both builds; SIGABRT path).
	CMemory abortMem = Module_FindPattern(g_GameDll,
		"48 83 EC 28 E8 ?? ?? ?? ?? 48 85 C0 74 ?? B9 16 00 00 00 E8 ?? ?? ?? ?? "
		"F6 05 ?? ?? ?? ?? 02");
	if (abortMem)
		v_abort = reinterpret_cast<void (*)(void)>(abortMem.GetPtr());
	else
		Warning(eDLL_T::ENGINE,
			"[CANARY-ABORT] abort pattern not found -- abort interceptor disabled\n");

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	LONG err1 = DetourAttach(&v_mspace_free, &Hook_mspace_free);
	LONG err2 = DetourAttach(&v_mspace_malloc, &Hook_mspace_malloc);
	LONG err3 = 0;
	if (v_abort)
		err3 = DetourAttach(&v_abort, &Hook_abort);
	LONG commitErr = DetourTransactionCommit();
	if (err1 != 0 || err2 != 0 || err3 != 0 || commitErr != 0)
	{
		Warning(eDLL_T::ENGINE,
			"[CANARY-MSPACE-FREE] DetourAttach failed: free=%ld "
			"malloc=%ld abort=%ld commit=%ld -- validator disabled\n",
			err1, err2, err3, commitErr);
		s_pMagicCookie = nullptr;
		v_abort = nullptr;
		return;
	}

	Msg(eDLL_T::ENGINE,
		"[CANARY-MSPACE-FREE] hook armed (%s): mspace_free=0x%p, "
		"sizeMask=0x%016llX, magic cookie=0x%p (value=0x%016llX)%s\n",
		freeBuild, mspaceFree.GetPtr(),
		(unsigned long long)s_mspaceSizeMask,
		static_cast<void*>(s_pMagicCookie),
		(unsigned long long)*s_pMagicCookie,
		v_abort ? "" : " (abort hook OFF)");
	if (v_abort)
		Msg(eDLL_T::ENGINE,
			"[CANARY-ABORT] hook armed: abort=0x%p\n", abortMem.GetPtr());
}

// Full-tree walker. Each of the 32 tree bins gets its own per-bin node
// budget so a deep bin can't starve later bins of coverage.
// SEH'd because a "plausible" pointer can still resolve to unmapped memory.
static constexpr int kPerBinBudget = 2048;
static constexpr int kMaxStackDepth = 8192;
static uint64_t* s_walkStack[kMaxStackDepth];

static bool CheckMspaceTreebins(uint64_t& outBadSlot, int& outBadIdx,
                                void*& outSlotAddr, void*& outMspace,
                                const char*& outWhere)
{
	if (!s_ppMspaceGlobal) return false;
	void* mspace = *s_ppMspaceGlobal;
	if (!mspace) return false;

	uint64_t* treebins = reinterpret_cast<uint64_t*>(
		static_cast<uint8_t*>(mspace) + kTreebinOffset);

	for (int i = 0; i < kNumTreebins; ++i)
	{
		uint64_t slot = treebins[i];
		if (slot == 0) continue;

		// Root validity.
		if (slot < kMinPlausiblePtr || slot > kMaxPlausiblePtr)
		{
			outBadSlot  = slot;
			outBadIdx   = i;
			outSlotAddr = static_cast<void*>(&treebins[i]);
			outMspace   = mspace;
			outWhere    = "treebin root";
			return true;
		}

		// Iterative DFS into this bin's tree. Each bin gets its own budget
		// so a pathologically deep bin can't starve the bins that follow.
		int budget = kPerBinBudget;
		int top = 0;
		s_walkStack[top++] = reinterpret_cast<uint64_t*>(slot);

		while (top > 0 && budget > 0)
		{
			uint64_t* chunk = s_walkStack[--top];
			--budget;

			__try
			{
				// Also validate the chunk's own size field looks sane. A
				// chunk in the free tree should have CINUSE bit clear (0)
				// and PINUSE indeterminate.
				uint64_t sizeField = chunk[1];
				uint64_t cleanSize = sizeField & 0xFFFFFFFFFFFFFFF8ull;
				if ((sizeField & 1) != 0)
				{
					// CINUSE bit set on a tree node = chunk is in-use but
					// still linked into the free tree. Use-after-free or
					// allocator-state corruption.
					outBadSlot  = sizeField;
					outBadIdx   = i;
					outSlotAddr = static_cast<void*>(&chunk[1]);
					outMspace   = mspace;
					outWhere    = "tree node CINUSE (chunk allocated but in free tree)";
					return true;
				}
				if (cleanSize < 32 || cleanSize > 0x10000000ull)
				{
					outBadSlot  = sizeField;
					outBadIdx   = i;
					outSlotAddr = static_cast<void*>(&chunk[1]);
					outMspace   = mspace;
					outWhere    = "tree node size field (implausible)";
					return true;
				}

				for (int childIdx = 4; childIdx <= 5; ++childIdx)
				{
					uint64_t child = chunk[childIdx];
					if (child == 0) continue;
					if (child < kMinPlausiblePtr || child > kMaxPlausiblePtr)
					{
						outBadSlot  = child;
						outBadIdx   = i;
						outSlotAddr = static_cast<void*>(&chunk[childIdx]);
						outMspace   = mspace;
						outWhere    = (childIdx == 4)
							? "tree node child[L]"
							: "tree node child[R]";
						return true;
					}
					if (top < kMaxStackDepth)
						s_walkStack[top++] = reinterpret_cast<uint64_t*>(child);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				outBadSlot  = reinterpret_cast<uint64_t>(chunk);
				outBadIdx   = i;
				outSlotAddr = static_cast<void*>(chunk);
				outMspace   = mspace;
				outWhere    = "tree node (deref-faulted)";
				return true;
			}
		}
	}
	return false;
}

// Lazy one-shot arm of the mspace monitor. Shared by PollTick (per-frame) and
// Checkpoint (manual/periodic) so a console-driven sdk_canary_check before the
// first server frame still arms the tree-bin walker + free/malloc validators.
static void EnsureMspaceInit()
{
	static bool s_initOnce = false;
	if (!s_initOnce)
	{
		s_initOnce = true;
		InitMspaceMonitor();
	}
}

// Running server-frame count, bumped by PollTick. Used to timestamp stomp
// reports and to tag periodic checkpoints so the log carries a timeline.
static uint64_t s_frameCounter = 0;

bool Checkpoint(const char* phase)
{
	if (!sdk_heap_canary.GetBool())
		return true; // dormant -- nothing registered, nothing to check

	EnsureMspaceInit();

	const char* tag = (phase && phase[0]) ? phase : "?";

	// 1) Full byte-for-byte sweep of every registered tail/region canary.
	const int stompedTails = CheckAll();

	// 2) Live mspace tree-bin integrity (root + free-tree DFS).
	uint64_t badSlot = 0;
	int      badIdx  = -1;
	void*    slotAddr = nullptr;
	void*    mspace   = nullptr;
	const char* where = "?";
	const bool mspaceArmed = (s_ppMspaceGlobal != nullptr);
	const bool treeBad = mspaceArmed &&
		CheckMspaceTreebins(badSlot, badIdx, slotAddr, mspace, where);

	const bool clean = (stompedTails == 0) && !treeBad;

	if (clean)
	{
		// Clean heartbeat -> message.log. These are the breadcrumbs: the LAST
		// clean checkpoint before the first stomped one brackets the corruptor.
		return true;
	}

	// Dirty -> warning.log. CheckAll above already emitted a per-buffer
	// [CANARY] STOMPED line for each offending tail (name + offset + bytes);
	// this is the one-line verdict that ties them to the phase + frame.
	Warning(eDLL_T::ENGINE,
		"[CANARY-CKPT] %s frame=%llu !!! CORRUPTION !!! tails_stomped=%d tree_bad=%d\n",
		tag, (unsigned long long)s_frameCounter, stompedTails, treeBad ? 1 : 0);
	if (treeBad)
		Warning(eDLL_T::ENGINE,
			"[CANARY-CKPT]   mspace=0x%p where='%s' bin=%d badval=0x%016llX "
			"slot=0x%p (HW BP target)\n",
			mspace, where, badIdx, (unsigned long long)badSlot, slotAddr);
	return false;
}

void PollTick()
{
	if (!sdk_heap_canary.GetBool())
		return;

	EnsureMspaceInit();
	++s_frameCounter;

	// Periodic heartbeat is above the stomp latch: timeline continues even
	// after first-stomp so the log keeps naming the phase.
	const int period = sdk_canary_period.GetInt();
	if (period > 0 && (s_frameCounter % static_cast<uint64_t>(period)) == 0)
		Checkpoint("periodic");

	// Once we've reported a stomp via the fast path, go silent on it. We've got
	// what we need.
	static bool s_latched = false;
	if (s_latched) return;

	// 1) Our heap-back / near-alloc buffer tail canaries (one qword per entry).
	for (const Entry& e : s_entries)
	{
		const uint64_t* tail = reinterpret_cast<const uint64_t*>(e.canaryAt);
		if (*tail != kTailPattern)
		{
			s_latched = true;
			Warning(eDLL_T::ENGINE,
				"[CANARY] !!! FIRST STOMP DETECTED @ frame=%llu -- dumping all "
				"entries !!!\n", (unsigned long long)s_frameCounter);
			DumpAll();
			return;
		}
	}

	// 2) Live mspace tree-bin corruption check (root + free-tree DFS).
	{
		uint64_t badSlot = 0;
		int      badIdx  = -1;
		void*    slotAddr = nullptr;
		void*    mspace   = nullptr;
		const char* where = "?";
		if (CheckMspaceTreebins(badSlot, badIdx, slotAddr, mspace, where))
		{
			s_latched = true;
			Warning(eDLL_T::ENGINE,
				"[CANARY-MSPACE] !!! TREEBIN CORRUPTION DETECTED @ frame=%llu !!!\n"
				"  mspace      = 0x%p\n"
				"  where       = %s\n"
				"  bin idx     = %d\n"
				"  bad value   = 0x%016llX (expected NULL or valid ptr)\n"
				"  slot addr   = 0x%p (HW BP target for next session)\n",
				(unsigned long long)s_frameCounter,
				mspace, where, badIdx,
				(unsigned long long)badSlot, slotAddr);
		}
	}
}

} // namespace HeapCanary
