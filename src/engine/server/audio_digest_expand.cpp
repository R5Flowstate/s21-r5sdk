//=============================================================================//
//
// Purpose: S21-capacity port of the S3 audio bank digest. See header.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier0/memory_patch.h"
#include "tier1/convar.h"
#include "audio_digest_expand.h"
#include "game/shared/heap_canary.h"

#include <cstdint>
#include <cstring>
#include <climits>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//-----------------------------------------------------------------------------
// Disable + restart to revert to the 100000-entry cap.
//-----------------------------------------------------------------------------
static ConVar sdk_audio_digest_expand("sdk_audio_digest_expand", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"S21-parity audio bank digest expansion (200,000 entries vs S3's 100,000). "
	"Required for mbnk_digest files >100K events. Disable + restart to run "
	"with stock S3 limits.");

//-----------------------------------------------------------------------------
// S3 layout constants (where things live in r5apex_ds.exe's static.data).
//-----------------------------------------------------------------------------
namespace s3 {
	// Digest base at module_base + 0x25E58260 (kBaseRVA).
	constexpr uint32_t kBaseRVA      = 0x25E58260;

	// Field offsets within the digest struct
	constexpr uint32_t kCount32Off   = 0x000404;  // dword: digest file count
	constexpr uint32_t kCount64Off   = 0x000408;  // qword: entry counter
	constexpr uint32_t kEntriesOff   = 0x000410;  // entries[100000][24]
	constexpr uint32_t kEntryCount   = 100000;
	constexpr uint32_t kEntryStride  = 24;
	constexpr uint32_t kBucketsOff   = 0x024A310; // buckets[8192][4]
	constexpr uint32_t kBucketCount  = 8192;
	constexpr uint32_t kBucketBytes  = kBucketCount * 4;       // 0x8000
	constexpr uint32_t kState1Off    = 0x252310;  // dword state slot
	constexpr uint32_t kState2Off    = 0x252314;  // qword state slot
}

//-----------------------------------------------------------------------------
// Entries stay at +0x410 so r9=base+0x410; idx*24 still works. Buckets move.
//-----------------------------------------------------------------------------
namespace ours {
	constexpr uint32_t kEntryCount   = 200000;                  // S21 + headroom
	constexpr uint32_t kEntriesOff   = s3::kEntriesOff;          // 0x000410
	constexpr uint32_t kEntryBytes   = kEntryCount * s3::kEntryStride; // 0x493800
	constexpr uint32_t kBucketsOff   = kEntriesOff + kEntryBytes;      // 0x493C10
	constexpr uint32_t kBucketBytes  = s3::kBucketBytes;
	constexpr uint32_t kState1Off    = kBucketsOff + kBucketBytes;     // 0x49BC10
	constexpr uint32_t kState2Off    = kState1Off + 4;                 // 0x49BC14
	constexpr uint32_t kBufferEnd    = kState2Off + 8;                 // 0x49BC1C
	constexpr size_t   kBufferSize   = 0x500000;                       // 5 MB
	static_assert(kBufferEnd < kBufferSize, "buffer too small");
}

//-----------------------------------------------------------------------------
// Public globals
//-----------------------------------------------------------------------------
uint8_t* g_pSdkAudioDigest  = nullptr;
uint8_t* g_pOrigAudioDigest = nullptr;

static uint8_t* s_pIncStub = nullptr;
static uint8_t  s_incSiteOrig[18] = {};
static bool     s_incSitePatched = false;

static constexpr uint32_t kLoaderIncRVA = 0x103E05F;
static constexpr uint32_t kFdmrVer10    = 0x10000;
static constexpr uint32_t kFdmrVer11    = 0x10001;

//-----------------------------------------------------------------------------
// RIP-relative disp32: orig_base+s3_off -> new_buffer+new_off.
//-----------------------------------------------------------------------------
struct LeaXref
{
	uint32_t    rva;
	uint8_t     disp_off;
	uint8_t     tail;        // bytes following disp32 within the same instruction (imm8/imm32 etc)
	uint32_t    s3_off;
	uint32_t    new_off;
	const char* tag;
};

// tail = bytes after disp32 in the same instruction (imm size for mov [rip],imm).
static constexpr LeaXref kLeaXrefs[] = {
	// === 18 base-load sites: 17 LEAs (7B, disp_off=3) + 1 mov-byte init writer ===
	{ 0xCBDCDE,  3, 0, 0x0, 0x0, "base@0xCBDCDE"  },
	{ 0xD55A28,  3, 0, 0x0, 0x0, "base@0xD55A28"  },
	{ 0xDFE477,  3, 0, 0x0, 0x0, "base@0xDFE477"  },
	{ 0x103DE59, 3, 0, 0x0, 0x0, "base@0x103DE59 (loader r15 lea)" },
	{ 0x103E27C, 3, 0, 0x0, 0x0, "base@0x103E27C (lookup variant 1)" },
	{ 0x103E35C, 3, 0, 0x0, 0x0, "base@0x103E35C (lookup variant 2)" },
	{ 0x103ED57, 3, 0, 0x0, 0x0, "base@0x103ED57" },
	{ 0x103F5B7, 3, 0, 0x0, 0x0, "base@0x103F5B7" },
	{ 0x103F875, 3, 0, 0x0, 0x0, "base@0x103F875" },
	{ 0x103FBCA, 3, 0, 0x0, 0x0, "base@0x103FBCA" },
	{ 0x103FCAC, 3, 0, 0x0, 0x0, "base@0x103FCAC" },
	{ 0x103FEAF, 3, 0, 0x0, 0x0, "base@0x103FEAF" },
	{ 0x10409BF, 3, 0, 0x0, 0x0, "base@0x10409BF" },
	{ 0x1040E67, 3, 0, 0x0, 0x0, "base@0x1040E67" },
	{ 0x1041207, 3, 0, 0x0, 0x0, "base@0x1041207" },
	{ 0x10419CF, 3, 0, 0x0, 0x0, "base@0x10419CF" },
	{ 0x1044838, 3, 0, 0x0, 0x0, "base@0x1044838" },
	// 7-byte `mov byte [rip+disp32], 1` = C6 05 [disp32] [imm8] -- tail=1
	{ 0x103E156, 2, 1, 0x0, 0x0, "base@0x103E156 (mov byte [base], 1)" },

	// Digest file-count RIP refs (no trailing imm).
	{ 0x103DE28, 2, 0, s3::kCount32Off, s3::kCount32Off, "count32@0x103DE28 (cmp [rip],edi)" },
	{ 0x103E118, 3, 0, s3::kCount32Off, s3::kCount32Off, "count32@0x103E118 (cmp r14d,[rip])" },

	// Entry-counter RIP refs (7B, disp_off=3).
	{ 0x103E05F, 3, 0, s3::kCount64Off, s3::kCount64Off, "count64@0x103E05F (mov rcx,[rip])" },
	{ 0x103E06A, 3, 0, s3::kCount64Off, s3::kCount64Off, "count64@0x103E06A (mov [rip],rax)" },

	// state1 RIP refs.
	// 10-byte `mov dword [rip+disp32], 0` = C7 05 [disp32] [imm32] -- tail=4
	{ 0x103E14C, 2, 4, s3::kState1Off, ours::kState1Off, "state1@0x103E14C (mov dword,0)" },
	// 8-byte `addss xmm7, [rip+disp32]` (no trailing imm)
	{ 0x103E513, 4, 0, s3::kState1Off, ours::kState1Off, "state1@0x103E513 (addss xmm7)" },
	// 8-byte `movss [rip+disp32], xmm0` (no trailing imm)
	{ 0x10446ED, 4, 0, s3::kState1Off, ours::kState1Off, "state1@0x10446ED (movss xmm0)" },

	// state2 RIP refs.
	// 11-byte `mov qword [rip+disp32], 0` = 48 C7 05 [disp32] [imm32] -- tail=4
	{ 0x103E13F, 3, 4, s3::kState2Off, ours::kState2Off, "state2@0x103E13F (mov qword,0)" },
	// 9-byte `addss xmm8, [rip+disp32]` (no trailing imm)
	{ 0x103E4FB, 5, 0, s3::kState2Off, ours::kState2Off, "state2@0x103E4FB (addss xmm8)" },
	// 8-byte `movss [rip+disp32], xmm1` (no trailing imm)
	{ 0x10446FA, 4, 0, s3::kState2Off, ours::kState2Off, "state2@0x10446FA (movss xmm1)" },
};

//-----------------------------------------------------------------------------
// Bucket disp32 is [r15+idx*4+off] from buffer base, not RIP-relative.
//-----------------------------------------------------------------------------
struct Disp32Patch
{
	uint32_t    rva;       // address of the 4-byte disp32 itself
	uint32_t    s3_imm;
	uint32_t    new_imm;
	const char* tag;
};

static constexpr Disp32Patch kDisp32Patches[] = {
	{ 0xCBDCED,  s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0xCBDCED"  },
	{ 0xDFE486,  s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0xDFE486"  },
	{ 0x103E022, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103E022 (loader read)" },
	{ 0x103E081, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103E081 (loader chain)" },
	{ 0x103E0AB, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103E0AB (loader write)" },
	{ 0x103E28C, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103E28C" },
	{ 0x103E36C, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103E36C" },
	{ 0x103ED66, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103ED66" },
	{ 0x103F5C6, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103F5C6" },
	{ 0x103F887, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103F887" },
	{ 0x103FBDC, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103FBDC" },
	{ 0x103FCBB, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103FCBB" },
	{ 0x103FEBE, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x103FEBE" },
	{ 0x10409CE, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x10409CE" },
	{ 0x1040E76, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x1040E76" },
	{ 0x1041216, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x1041216" },
	{ 0x10419DE, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x10419DE" },
	{ 0x104484B, s3::kBucketsOff, ours::kBucketsOff, "buckdisp@0x104484B" },
};

//-----------------------------------------------------------------------------
// Validate disp32 resolves to expected source, then rewrite.
//-----------------------------------------------------------------------------
static bool ApplyLeaXref(uintptr_t moduleBase, uintptr_t origBase, uintptr_t newBase,
						 const LeaXref& xr, bool patchInForward)
{
	uint8_t* pInstr = (uint8_t*)(moduleBase + xr.rva);
	uint8_t* pDisp  = pInstr + xr.disp_off;
	// RIP-relative resolution uses the address AFTER the FULL instruction,
	// not just after the disp32. For `mov [rip+disp32], imm` the imm bytes
	// sit between disp32-end and instruction-end -- account for them via tail.
	uintptr_t nextRIP = (uintptr_t)pDisp + 4 + xr.tail;

	int32_t curDisp = *(int32_t*)pDisp;
	uintptr_t resolved = nextRIP + (intptr_t)curDisp;

	// Validate against where we EXPECT the current state to point.
	uintptr_t expected;
	if (patchInForward)
		expected = origBase + xr.s3_off;       // applying patch: should be at orig
	else
		expected = newBase + xr.new_off;       // unpatching: should be at new buffer

	if (resolved != expected)
	{
		Warning(eDLL_T::ENGINE,
			"[ADIG-EXT] LEA xref MISMATCH '%s' RVA=0x%X resolved=0x%llX expected=0x%llX (disp=0x%X)\n",
			xr.tag, xr.rva, (unsigned long long)resolved,
			(unsigned long long)expected, (unsigned)curDisp);
		return false;
	}

	uintptr_t newTarget = patchInForward
		? newBase + xr.new_off
		: origBase + xr.s3_off;
	int64_t newDisp64 = (int64_t)newTarget - (int64_t)nextRIP;
	if (newDisp64 > INT32_MAX || newDisp64 < INT32_MIN)
	{
		Warning(eDLL_T::ENGINE,
			"[ADIG-EXT] LEA xref RANGE OVERFLOW '%s' RVA=0x%X newDisp=0x%llX\n",
			xr.tag, xr.rva, (unsigned long long)newDisp64);
		return false;
	}

	int32_t newDisp = (int32_t)newDisp64;
	if (!Mem_PatchCode(pDisp, &newDisp, 4))
	{
		Warning(eDLL_T::ENGINE,
			"[ADIG-EXT] LEA xref VirtualProtect failed '%s' RVA=0x%X\n",
			xr.tag, xr.rva);
		return false;
	}
	return true;
}

static bool ApplyDisp32(uintptr_t moduleBase, const Disp32Patch& p, bool patchInForward)
{
	uint8_t* pDisp = (uint8_t*)(moduleBase + p.rva);
	uint32_t curImm = *(uint32_t*)pDisp;

	uint32_t expected = patchInForward ? p.s3_imm : p.new_imm;
	uint32_t target   = patchInForward ? p.new_imm : p.s3_imm;
	if (curImm != expected)
	{
		Warning(eDLL_T::ENGINE,
			"[ADIG-EXT] disp32 MISMATCH '%s' RVA=0x%X got=0x%X expected=0x%X\n",
			p.tag, p.rva, curImm, expected);
		return false;
	}
	if (!Mem_PatchCode(pDisp, &target, 4))
	{
		Warning(eDLL_T::ENGINE,
			"[ADIG-EXT] disp32 VirtualProtect failed '%s' RVA=0x%X\n",
			p.tag, p.rva);
		return false;
	}
	return true;
}

static uint8_t* AllocExecNear(uintptr_t nearAddr, size_t size)
{
	for (int dir = 1; dir >= -1; dir -= 2)
	{
		uintptr_t scanAddr = (nearAddr + dir * 0x10000000ULL) & ~0xFFFFULL;
		for (int attempt = 0; attempt < 4096; ++attempt)
		{
			const uintptr_t tryAddr = scanAddr + dir * attempt * 0x10000ULL;
			const int64_t dist = (int64_t)tryAddr - (int64_t)nearAddr;
			if (dist > 0x70000000LL || dist < -0x70000000LL)
				break;
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery((void*)tryAddr, &mbi, sizeof(mbi)) != sizeof(mbi))
				continue;
			if (mbi.State != MEM_FREE || mbi.RegionSize < size)
				continue;
			void* p = VirtualAlloc((void*)tryAddr, size,
				MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (p)
				return static_cast<uint8_t*>(p);
		}
	}
	return nullptr;
}

static void AudioDigest_RemoveIncHook(uintptr_t moduleBase)
{
	if (!s_incSitePatched)
		return;
	uint8_t* site = reinterpret_cast<uint8_t*>(moduleBase + kLoaderIncRVA);
	Mem_PatchCode(site, s_incSiteOrig, sizeof(s_incSiteOrig));
	s_incSitePatched = false;
	if (s_pIncStub)
	{
		VirtualFree(s_pIncStub, 0, MEM_RELEASE);
		s_pIncStub = nullptr;
	}
}

static bool AudioDigest_InstallIncHook(uintptr_t moduleBase, uint8_t* digest)
{
	uint8_t* const site = reinterpret_cast<uint8_t*>(moduleBase + kLoaderIncRVA);
	if (site[0] != 0x48 || site[1] != 0x8B)
	{
		Warning(eDLL_T::ENGINE,
			"[ADIG-EXT] loader count++ site unexpected opcode %02X %02X -- cap hook skipped\n",
			site[0], site[1]);
		return false;
	}

	s_pIncStub = AllocExecNear(reinterpret_cast<uintptr_t>(site), 64);
	if (!s_pIncStub)
	{
		Warning(eDLL_T::ENGINE,
			"[ADIG-EXT] failed to allocate increment stub -- loader cap not installed\n");
		return false;
	}

	const uint64_t countPtr = reinterpret_cast<uint64_t>(digest + s3::kCount64Off);
	uint8_t* p = s_pIncStub;
	*p++ = 0x48; *p++ = 0xB8;
	memcpy(p, &countPtr, 8); p += 8;
	*p++ = 0x48; *p++ = 0x8B; *p++ = 0x08;
	*p++ = 0x48; *p++ = 0x81; *p++ = 0xF9;
	const uint32_t cap = ours::kEntryCount;
	memcpy(p, &cap, 4); p += 4;
	*p++ = 0x72; *p++ = 0x0F;
	*p++ = 0x48; *p++ = 0xC7; *p++ = 0xC1;
	const uint32_t last = ours::kEntryCount - 1;
	memcpy(p, &last, 4); p += 4;
	*p++ = 0x48; *p++ = 0xC7; *p++ = 0xC0;
	memcpy(p, &cap, 4); p += 4;
	*p++ = 0xC3;
	*p++ = 0x48; *p++ = 0x8D; *p++ = 0x51; *p++ = 0x01;
	*p++ = 0x48; *p++ = 0x89; *p++ = 0x10;
	*p++ = 0x48; *p++ = 0x89; *p++ = 0xD0;
	*p++ = 0xC3;
	FlushInstructionCache(GetCurrentProcess(), s_pIncStub, 64);

	memcpy(s_incSiteOrig, site, sizeof(s_incSiteOrig));
	const int64_t rel = reinterpret_cast<int64_t>(s_pIncStub)
		- (reinterpret_cast<int64_t>(site) + 5);
	if (rel > INT32_MAX || rel < INT32_MIN)
	{
		Warning(eDLL_T::ENGINE, "[ADIG-EXT] increment stub out of rel32 range\n");
		VirtualFree(s_pIncStub, 0, MEM_RELEASE);
		s_pIncStub = nullptr;
		return false;
	}

	uint8_t patch[18] = {};
	patch[0] = 0xE8;
	const int32_t rel32 = static_cast<int32_t>(rel);
	memcpy(patch + 1, &rel32, 4);
	patch[5] = 0xEB;
	patch[6] = 0x0B;
	memset(patch + 7, 0x90, 11);
	if (!Mem_PatchCode(site, patch, sizeof(patch)))
	{
		Warning(eDLL_T::ENGINE, "[ADIG-EXT] VirtualProtect failed on loader count++\n");
		VirtualFree(s_pIncStub, 0, MEM_RELEASE);
		s_pIncStub = nullptr;
		return false;
	}
	s_incSitePatched = true;
	Msg(eDLL_T::ENGINE,
		"[ADIG-EXT] loader count++ saturates at %u entries\n", ours::kEntryCount);
	return true;
}

//-----------------------------------------------------------------------------
// FDMR 1.1 ingest -- stock loader rejects version != 0x10000.
//-----------------------------------------------------------------------------
static uint64_t AudioDigest_HashName(const char* pszName)
{
	uint64_t hash = 0xCBF29CE484222325ULL;
	static constexpr uint64_t FNV_PRIME = 0x100000001B3ULL;
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszName); *p; ++p)
	{
		unsigned char c = *p;
		if (c >= 'A' && c <= 'Z')
			c = static_cast<unsigned char>(c + 32);
		else if (c == '.')
			c = '_';
		hash = c ^ (FNV_PRIME * hash);
	}
	return hash;
}

static bool AudioDigest_Insert(uint8_t* pDigest, uint32_t nBucketOff, uint32_t nCap,
	uint64_t nHash, float flA, float flB)
{
	uint64_t* const pCount = reinterpret_cast<uint64_t*>(pDigest + s3::kCount64Off);
	uint32_t* const pBuckets = reinterpret_cast<uint32_t*>(pDigest + nBucketOff);
	uint8_t* const pEntries = pDigest + s3::kEntriesOff;
	const uint32_t nBucket = static_cast<uint32_t>(nHash & (s3::kBucketCount - 1));

	uint32_t nIdx = pBuckets[nBucket];
	int nSafety = 0;
	while (nIdx != 0 && nSafety++ < 64)
	{
		uint8_t* const pEnt = pEntries + static_cast<size_t>(nIdx) * s3::kEntryStride;
		if (*reinterpret_cast<uint64_t*>(pEnt) == nHash)
		{
			*reinterpret_cast<float*>(pEnt + 0x0C) = flA;
			*reinterpret_cast<float*>(pEnt + 0x10) = flB;
			return true;
		}
		nIdx = *reinterpret_cast<uint32_t*>(pEnt + 8);
	}

	uint64_t nNew = (*pCount)++;
	if (nNew == 0)
		nNew = (*pCount)++;
	if (nNew >= nCap)
	{
		*pCount = nCap;
		return false;
	}

	uint8_t* const pEnt = pEntries + static_cast<size_t>(nNew) * s3::kEntryStride;
	*reinterpret_cast<uint64_t*>(pEnt) = nHash;
	*reinterpret_cast<uint32_t*>(pEnt + 8) = pBuckets[nBucket];
	*reinterpret_cast<float*>(pEnt + 0x0C) = flA;
	*reinterpret_cast<float*>(pEnt + 0x10) = flB;
	pBuckets[nBucket] = static_cast<uint32_t>(nNew);
	return true;
}

static bool AudioDigest_Lookup(uint8_t* pDigest, uint32_t nBucketOff, uint64_t nHash)
{
	uint32_t nIdx = reinterpret_cast<uint32_t*>(pDigest + nBucketOff)[nHash & (s3::kBucketCount - 1)];
	uint8_t* const pEntries = pDigest + s3::kEntriesOff;
	int nSafety = 0;
	while (nIdx != 0 && nSafety++ < 64)
	{
		uint8_t* const pEnt = pEntries + static_cast<size_t>(nIdx) * s3::kEntryStride;
		if (*reinterpret_cast<uint64_t*>(pEnt) == nHash)
			return true;
		nIdx = *reinterpret_cast<uint32_t*>(pEnt + 8);
	}
	return false;
}

static uint32_t AudioDigest_IngestFile(uint8_t* pDigest, uint32_t nBucketOff, uint32_t nCap,
	const uint8_t* pFile, size_t nSize)
{
	if (nSize < 24)
		return 0;
	if (memcmp(pFile, "FDMR", 4) != 0)
		return 0;

	const uint32_t nVer = *reinterpret_cast<const uint32_t*>(pFile + 4);
	const uint32_t nCount = *reinterpret_cast<const uint32_t*>(pFile + 8);
	const uint32_t nExtra = *reinterpret_cast<const uint32_t*>(pFile + 12);
	const uint32_t nSec = *reinterpret_cast<const uint32_t*>(pFile + 16);

	uint32_t nRecOff;
	uint32_t nStride;
	uint32_t nFloatA;
	uint32_t nFloatB;
	if (nVer == kFdmrVer10)
	{
		nRecOff = 24;
		nStride = 20;
		nFloatA = 12;
		nFloatB = 16;
	}
	else if (nVer == kFdmrVer11)
	{
		// 28-byte records; duration/radius at +8/+12. Header +24 is tail size.
		nRecOff = 28;
		nStride = 28;
		nFloatA = 8;
		nFloatB = 12;
	}
	else
		return 0;

	if (nCount == 0 || nCount > nCap)
		return 0;

	const uint64_t nStr = static_cast<uint64_t>(nRecOff)
		+ static_cast<uint64_t>(nCount) * nStride
		+ 4ull * nExtra + 2ull * nSec;
	if (nStr >= nSize)
		return 0;

	uint32_t nInserted = 0;
	for (uint32_t i = 0; i < nCount; ++i)
	{
		const uint8_t* const pRec = pFile + nRecOff + static_cast<size_t>(i) * nStride;
		if (pRec + nStride > pFile + nSize)
			break;

		const uint32_t nOff = *reinterpret_cast<const uint32_t*>(pRec);
		const uint64_t nName = nStr + nOff;
		if (nName >= nSize)
			continue;

		const char* const psz = reinterpret_cast<const char*>(pFile + nName);
		const size_t nMax = nSize - static_cast<size_t>(nName);
		bool bTerm = false;
		for (size_t k = 0; k < nMax && k < 128; ++k)
		{
			if (psz[k] == '\0')
			{
				bTerm = true;
				break;
			}
		}
		if (!bTerm || psz[0] == '\0')
			continue;

		const float flA = *reinterpret_cast<const float*>(pRec + nFloatA);
		const float flB = *reinterpret_cast<const float*>(pRec + nFloatB);
		if (AudioDigest_Insert(pDigest, nBucketOff, nCap, AudioDigest_HashName(psz), flA, flB))
			++nInserted;
	}
	return nInserted;
}

static bool AudioDigest_ReadFile(const char* pszPath, uint8_t** ppBuf, size_t* pSize)
{
	const HANDLE hFile = CreateFileA(pszPath, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER liSize{};
	if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart <= 24 || liSize.QuadPart > (64ll * 1024 * 1024))
	{
		CloseHandle(hFile);
		return false;
	}

	const size_t nSize = static_cast<size_t>(liSize.QuadPart);
	uint8_t* const pBuf = static_cast<uint8_t*>(malloc(nSize));
	if (!pBuf)
	{
		CloseHandle(hFile);
		return false;
	}

	DWORD nRead = 0;
	const bool bOk = ReadFile(hFile, pBuf, static_cast<DWORD>(nSize), &nRead, NULL) != FALSE
		&& nRead == nSize;
	CloseHandle(hFile);
	if (!bOk)
	{
		free(pBuf);
		return false;
	}

	*ppBuf = pBuf;
	*pSize = nSize;
	return true;
}

static uint32_t AudioDigest_IngestPath(uint8_t* pDigest, const char* pszPath)
{
	uint8_t* pFile = nullptr;
	size_t nSize = 0;
	if (!AudioDigest_ReadFile(pszPath, &pFile, &nSize))
	{
		Msg(eDLL_T::ENGINE, "[ADIG-EXT] no digest '%s'\n", pszPath);
		return 0;
	}

	const uint32_t nVer = *reinterpret_cast<const uint32_t*>(pFile + 4);
	const uint32_t nCount = *reinterpret_cast<const uint32_t*>(pFile + 8);
	const uint32_t nGot = AudioDigest_IngestFile(
		pDigest, ours::kBucketsOff, ours::kEntryCount, pFile, nSize);
	Msg(eDLL_T::ENGINE,
		"[ADIG-EXT] ingested '%s' ver=0x%X fileEvents=%u inserted=%u\n",
		pszPath, nVer, nCount, nGot);
	free(pFile);
	return nGot;
}

static void AudioDigest_FillFromDisk(uint8_t* pDigest)
{
	memset(pDigest + ours::kBucketsOff, 0, ours::kBucketBytes);
	*reinterpret_cast<uint64_t*>(pDigest + s3::kCount64Off) = 1;

	const uint32_t nGot = AudioDigest_IngestPath(pDigest, "audio\\ship\\general.mbnk_digest");

	pDigest[0] = 1;
	*reinterpret_cast<uint32_t*>(pDigest + ours::kState1Off) = 0;
	*reinterpret_cast<uint64_t*>(pDigest + ours::kState2Off) = 0;

	const bool bHit = AudioDigest_Lookup(pDigest, ours::kBucketsOff,
		AudioDigest_HashName("Music_Lobby_Season21"));
	Msg(eDLL_T::ENGINE,
		"[ADIG-EXT] FDMR 1.1 fill events=%u Music_Lobby_Season21=%s\n",
		nGot, bHit ? "HIT" : "MISS");
}

//-----------------------------------------------------------------------------
// VAudioDigestExpand
//-----------------------------------------------------------------------------
void VAudioDigestExpand::GetAdr(void) const
{
	if (g_pSdkAudioDigest)
	{
		Msg(eDLL_T::ENGINE,
			"[ADIG-EXT] addresses: orig=0x%p new=0x%p (size=0x%zX)\n",
			(void*)g_pOrigAudioDigest, (void*)g_pSdkAudioDigest,
			ours::kBufferSize);
	}
}

void VAudioDigestExpand::Detour(const bool bAttach) const
{
	const uintptr_t moduleBase = g_GameDll.GetModuleBase();

	if (bAttach)
	{
		if (!sdk_audio_digest_expand.GetBool())
		{
			Msg(eDLL_T::ENGINE,
				"[ADIG-EXT] disabled by sdk_audio_digest_expand=0 -- stock S3 "
				"limits (100,000-entry cap, fatal overflow on >100K events)\n");
			return;
		}

		g_pOrigAudioDigest = (uint8_t*)(moduleBase + s3::kBaseRVA);

		// Layout sanity (compile-time)
		static_assert(s3::kEntriesOff   == 0x000410, "S3 entries offset");
		static_assert(s3::kBucketsOff   == 0x24A310, "S3 buckets offset");
		static_assert(s3::kState1Off    == 0x252310, "S3 state1 offset");
		static_assert(s3::kState2Off    == 0x252314, "S3 state2 offset");
		static_assert(ours::kEntriesOff == 0x000410, "ours entries offset");
		static_assert(ours::kEntryBytes == ours::kEntryCount * s3::kEntryStride,
					  "ours entry bytes math");
		static_assert(ours::kBucketsOff == ours::kEntriesOff + ours::kEntryBytes,
					  "ours buckets layout");
		static_assert(ours::kState1Off  == ours::kBucketsOff + ours::kBucketBytes,
					  "ours state1 layout");
		static_assert(ours::kState2Off  == ours::kState1Off + 4,
					  "ours state2 layout");
		static_assert(ours::kBufferEnd  < ours::kBufferSize,
					  "buffer too small for layout");

		g_pSdkAudioDigest = Mem_AllocNearModule(g_GameDll, ours::kBufferSize + HeapCanary::kTailBytes);
		if (!g_pSdkAudioDigest)
		{
			Warning(eDLL_T::ENGINE,
				"[ADIG-EXT] FAILED to allocate %zu-byte buffer within +-2GB of "
				"module base 0x%llX -- stock S3 limits in effect, dedi will "
				"crash if mbnk_digest > 100K entries\n",
				ours::kBufferSize, (unsigned long long)moduleBase);
			return;
		}
		HeapCanary::RegisterTail("audio-digest", g_pSdkAudioDigest, ours::kBufferSize);

		Msg(eDLL_T::ENGINE,
			"[ADIG-EXT] buffer at 0x%p (size=0x%zX, dist=%lld bytes from module)\n",
			(void*)g_pSdkAudioDigest, ours::kBufferSize,
			(long long)((intptr_t)g_pSdkAudioDigest - (intptr_t)moduleBase));

		const uintptr_t origBase = (uintptr_t)g_pOrigAudioDigest;
		const uintptr_t newBase  = (uintptr_t)g_pSdkAudioDigest;

		// All-or-nothing: track applied counts so a mid-table failure can
		// reverse only sites that actually moved, then free the heap buffer.
		const size_t nLea  = sizeof(kLeaXrefs) / sizeof(kLeaXrefs[0]);
		const size_t nDisp = sizeof(kDisp32Patches) / sizeof(kDisp32Patches[0]);
		size_t leaApplied  = 0;
		size_t dispApplied = 0;
		bool allOk = true;

		for (size_t i = 0; i < nLea; ++i)
		{
			if (!ApplyLeaXref(moduleBase, origBase, newBase, kLeaXrefs[i], true))
			{
				allOk = false;
				break;
			}
			++leaApplied;
		}
		if (allOk)
		{
			for (size_t i = 0; i < nDisp; ++i)
			{
				if (!ApplyDisp32(moduleBase, kDisp32Patches[i], true))
				{
					allOk = false;
					break;
				}
				++dispApplied;
			}
		}

		if (!allOk)
		{
			for (size_t i = 0; i < leaApplied; ++i)
				ApplyLeaXref(moduleBase, origBase, newBase, kLeaXrefs[i], false);
			for (size_t i = 0; i < dispApplied; ++i)
				ApplyDisp32(moduleBase, kDisp32Patches[i], false);

			HeapCanary::Unregister(g_pSdkAudioDigest);
			VirtualFree(g_pSdkAudioDigest, 0, MEM_RELEASE);
			g_pSdkAudioDigest  = nullptr;
			g_pOrigAudioDigest = nullptr;

			Warning(eDLL_T::ENGINE,
				"[ADIG-EXT] expansion OFF -- site failure after %zu/%zu LEA + "
				"%zu/%zu disp32; rolled back all applied patches, stock S3 "
				"layout restored (100,000-entry cap). Dedi will crash if "
				"mbnk_digest exceeds stock cap.\n",
				leaApplied, nLea, dispApplied, nDisp);
			return;
		}

		(void)AudioDigest_InstallIncHook(moduleBase, g_pSdkAudioDigest);

		Msg(eDLL_T::ENGINE,
			"[ADIG-EXT] LEA xrefs: %zu OK (of %zu)\n", leaApplied, nLea);
		Msg(eDLL_T::ENGINE,
			"[ADIG-EXT] disp32 sites: %zu OK (of %zu)\n", dispApplied, nDisp);
		Msg(eDLL_T::ENGINE,
			"[ADIG-EXT] expansion ACTIVE -- entries[%u]@0x%X..0x%X, "
			"buckets@0x%X (was 0x%X), state1@0x%X, state2@0x%X (buffer 0x%zX)\n",
			ours::kEntryCount, ours::kEntriesOff,
			ours::kEntriesOff + ours::kEntryBytes,
			ours::kBucketsOff, s3::kBucketsOff,
			ours::kState1Off, ours::kState2Off, ours::kBufferSize);

		AudioDigest_FillFromDisk(g_pSdkAudioDigest);
	}
	else
	{
		if (!g_pSdkAudioDigest)
			return;

		const uintptr_t origBase = (uintptr_t)g_pOrigAudioDigest;
		const uintptr_t newBase  = (uintptr_t)g_pSdkAudioDigest;

		AudioDigest_RemoveIncHook(moduleBase);
		for (const LeaXref& xr : kLeaXrefs)
			ApplyLeaXref(moduleBase, origBase, newBase, xr, false);
		for (const Disp32Patch& p : kDisp32Patches)
			ApplyDisp32(moduleBase, p, false);

		HeapCanary::Unregister(g_pSdkAudioDigest);
		VirtualFree(g_pSdkAudioDigest, 0, MEM_RELEASE);
		g_pSdkAudioDigest  = nullptr;
		g_pOrigAudioDigest = nullptr;

		Msg(eDLL_T::ENGINE,
			"[ADIG-EXT] expansion REVERTED -- stock S3 layout restored, heap buffer freed\n");
	}
}
