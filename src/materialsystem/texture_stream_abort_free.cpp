//=============================================================================//
//
// Purpose: Free the texture streamer's staging block when a mip request set
// aborts. The engine frees it only on the success path.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "rtech/pak/rpak_observe.h"
#include "texture_stream_abort_free.h"

// The streamer keeps 64 slots of 360 bytes. A slot holds the texture pointer,
// the request cursor and count, and 160-byte read requests from +0x20. The
// staging block shared by every request in the slot is stored as the first
// request's data pointer, which lands at slot+0x98.
static constexpr size_t kSlot_Texture       = 0x00;
static constexpr size_t kSlot_RequestCursor = 0x1C;
static constexpr size_t kSlot_RequestCount  = 0x1E;
static constexpr size_t kSlot_Requests      = 0x20;
static constexpr size_t kSlot_Staging       = 0x98;
static constexpr size_t kRequestStride      = 160;
static constexpr size_t kRequest_FileHandle = 136;
static constexpr size_t kRequest_FileOffset = 144;
static constexpr size_t kRequest_ChunkBytes = 64;

static ConVar sdk_stream_abort_free("sdk_stream_abort_free", "1", FCVAR_RELEASE,
	"Free the texture streaming staging block when a mip request set aborts.");

typedef __int64(__fastcall* PFN_TextureStream_ChargeBudget)(void* slotOrPair, int mipLevels);
static PFN_TextureStream_ChargeBudget v_TextureStream_ChargeBudget = nullptr;

static uintptr_t s_imageBase = 0;
static uintptr_t s_imageEnd = 0;

// Async file table: HANDLE at table[2 * (handle & 0x3FF)].
static const HANDLE* s_pAsyncFileTable = nullptr;

static const char* AsyncFilePath(const int fileHandle, char* const out, const size_t outSize)
{
	out[0] = '\0';
	if (!s_pAsyncFileTable || fileHandle < 0)
		return "<none>";
	const HANDLE h = s_pAsyncFileTable[2 * (fileHandle & 0x3FF)];
	if (!h || h == INVALID_HANDLE_VALUE)
		return "<invalid>";
	if (!GetFinalPathNameByHandleA(h, out, static_cast<DWORD>(outSize), FILE_NAME_NORMALIZED))
		return "<unknown>";
	const char* const slash = strrchr(out, '\\');
	return slash ? slash + 1 : out;
}
static volatile LONG s_nFreed = 0;
static volatile LONG s_nLogged = 0;

// The budget charge is also called with a stack pair; only the static slot
// array carries a staging block.
static bool IsInImage(const void* const p)
{
	const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
	return addr >= s_imageBase && addr < s_imageEnd;
}

static void FreeThroughPakAllocator(void* const ptr)
{
	const uintptr_t vtbl = Pak_GetGlobalAllocatorSlot_S21();
	if (!vtbl)
		return;
	typedef void(__fastcall* PFN_PakFree)(void*, void*);
	const PFN_PakFree pfnFree = *reinterpret_cast<const PFN_PakFree*>(vtbl + 8);
	if (pfnFree)
		pfnFree(reinterpret_cast<void*>(vtbl), ptr);
}

// The pak asset tables: 64-byte chunks of eight guids, and a parallel table
// 0x400000 bytes later holding the live header pointer per index.
static constexpr size_t kPakAssetIndexCount = 0x80000;
static constexpr size_t kPakHeadTableOffset = 0x400000;

static unsigned long long GuidForAssetHeader(const void* const header)
{
	const HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe || !header)
		return 0;
	const uintptr_t hashBase = S21Pak_AssetGuidHashBase();
	const uintptr_t heads = hashBase + kPakHeadTableOffset;
	for (size_t idx = 0; idx < kPakAssetIndexCount; ++idx)
	{
		if (*reinterpret_cast<const void* const*>(heads + 16 * idx) != header)
			continue;
		return *reinterpret_cast<const unsigned long long*>(hashBase + 64 * (idx >> 3) + 8 * (idx & 7));
	}
	return 0;
}

// Per-texture abort tally so the offenders can be read off the log.
struct Culprit_t
{
	const void*        header;
	unsigned long long guid;
	int                fileHandle;
	long long          fileOffset;
	size_t             hits;
};
static Culprit_t s_culprits[128];
static size_t s_nCulprits = 0;

static size_t Culprit_Count(const void* const header, unsigned long long* const pGuid, const int fileHandle, const long long fileOffset)
{
	for (size_t i = 0; i < s_nCulprits; ++i)
	{
		if (s_culprits[i].header == header)
		{
			s_culprits[i].fileHandle = fileHandle;
			s_culprits[i].fileOffset = fileOffset;
			*pGuid = s_culprits[i].guid;
			return ++s_culprits[i].hits;
		}
	}
	*pGuid = GuidForAssetHeader(header);
	if (s_nCulprits < ARRAYSIZE(s_culprits))
	{
		s_culprits[s_nCulprits] = { header, *pGuid, fileHandle, fileOffset, 1 };
		++s_nCulprits;
	}
	return 1;
}

static void Culprit_Report(void)
{
	Warning(eDLL_T::MS, "[STREAM-ABORT] culprits: %zu textures with failed streaming reads\n", s_nCulprits);
	for (size_t i = 0; i < s_nCulprits; ++i)
	{
		char szPath[MAX_PATH];
		const char* const pszFile = AsyncFilePath(s_culprits[i].fileHandle, szPath, sizeof(szPath));
		Warning(eDLL_T::MS, "[STREAM-ABORT]   tex=0x%016llX hits=%zu last file='%s' offset=%lld\n",
			s_culprits[i].guid, s_culprits[i].hits, pszFile, s_culprits[i].fileOffset);
	}
}

// The abort branch releases its requests after the budget charge; a release
// waits for in-flight IO and skips its own free when the output pointer still
// equals the data pointer. So the slot is left untouched here and the block
// is freed on the streamer thread's next call, after that loop has finished.
static void* s_pending[64];
static size_t s_nPending = 0;

static void DrainPending(void)
{
	for (size_t i = 0; i < s_nPending; ++i)
		FreeThroughPakAllocator(s_pending[i]);
	s_nPending = 0;
}

static __int64 __fastcall Hook_TextureStream_ChargeBudget(void* slotOrPair, int mipLevels)
{
	DrainPending();

	const __int64 result = v_TextureStream_ChargeBudget(slotOrPair, mipLevels);
	if (!sdk_stream_abort_free.GetBool() || !IsInImage(slotOrPair))
		return result;

	uint8_t* const slot = static_cast<uint8_t*>(slotOrPair);
	void* const pStaging = *reinterpret_cast<void**>(slot + kSlot_Staging);
	if (!pStaging || s_nPending >= ARRAYSIZE(s_pending))
		return result;
	for (size_t i = 0; i < s_nPending; ++i)
		if (s_pending[i] == pStaging)
			return result;
	s_pending[s_nPending++] = pStaging;

	const uint16_t cursor = *reinterpret_cast<const uint16_t*>(slot + kSlot_RequestCursor);
	const uint16_t count = *reinterpret_cast<const uint16_t*>(slot + kSlot_RequestCount);
	const uint8_t state = cursor < count ? slot[kSlot_Requests + kRequestStride * cursor] : 0xFF;

	// Texture header: width @10, height @12, per-mip compression bits @30
	// (two bits per level; 0 = raw, else a codec the chunk decoder must match).
	const uint8_t* const tex = *reinterpret_cast<const uint8_t* const*>(slot + kSlot_Texture);
	const unsigned width = tex ? *reinterpret_cast<const uint16_t*>(tex + 10) : 0;
	const unsigned height = tex ? *reinterpret_cast<const uint16_t*>(tex + 12) : 0;
	const unsigned compBits = tex ? *reinterpret_cast<const uint16_t*>(tex + 30) : 0;

	const uint8_t* const req = slot + kSlot_Requests + kRequestStride * (cursor < count ? cursor : 0);
	const int fileHandle = *reinterpret_cast<const int*>(req + kRequest_FileHandle);
	const long long fileOffset = *reinterpret_cast<const long long*>(req + kRequest_FileOffset);
	const unsigned chunkBytes = *reinterpret_cast<const unsigned*>(req + kRequest_ChunkBytes);

	unsigned long long guid = 0;
	const size_t nHits = Culprit_Count(tex, &guid, fileHandle, fileOffset);

	const LONG nFreed = InterlockedIncrement(&s_nFreed);
	if (InterlockedIncrement(&s_nLogged) <= 16 || nHits == 1 || (nFreed % 256) == 0)
	{
		// Request: level count @7, per-level codec @1+k, cumulative decoded
		// bytes @16+4k, cumulative on-disk bytes @40+4k.
		const unsigned levels = req[7];
		char szLevels[160] = {};
		size_t len = 0;
		for (unsigned k = 0; k < levels && k < 6 && len < sizeof(szLevels) - 32; ++k)
		{
			len += snprintf(szLevels + len, sizeof(szLevels) - len, "%s%u:%u/%u", k ? " " : "",
				req[1 + k],
				*reinterpret_cast<const unsigned*>(req + 16 + 4 * k),
				*reinterpret_cast<const unsigned*>(req + 40 + 4 * k));
		}
		char szPath[MAX_PATH];
		const char* const pszFile = AsyncFilePath(fileHandle, szPath, sizeof(szPath));
		Warning(eDLL_T::MS, "[STREAM-ABORT] tex=0x%016llX %ux%u comp=0x%04X state=%u request %u/%u levels[codec:decoded/disk]=%s file=%d '%s' offset=%lld chunk=%u hits=%zu (total %ld)\n",
			guid, width, height, compBits, state, cursor, count, szLevels,
			fileHandle, pszFile, fileOffset, chunkBytes, nHits, nFreed);
	}
	if ((nFreed % 1024) == 0)
		Culprit_Report();
	return result;
}

// FS_AsyncThread_NotifyFinishedSubreq(subreq, status, win32Error): status 2 is
// a failed ReadFileEx. The request sits at subreq[1]: file handle @12,
// offset @16, byte count @48.
typedef void(__fastcall* PFN_FS_AsyncNotifyFinishedSubreq)(uint64_t* subreq, char status, int win32Error);
static PFN_FS_AsyncNotifyFinishedSubreq v_FS_AsyncNotifyFinishedSubreq = nullptr;
static volatile LONG s_nIoErrors = 0;

static void __fastcall Hook_FS_AsyncNotifyFinishedSubreq(uint64_t* subreq, char status, int win32Error)
{
	if (status != 1 && subreq && subreq[1])
	{
		const uint8_t* const req = reinterpret_cast<const uint8_t*>(subreq[1]);
		const int fileHandle = *reinterpret_cast<const int*>(req + 12);
		const long long offset = *reinterpret_cast<const long long*>(req + 16);
		const unsigned bytes = *reinterpret_cast<const unsigned*>(req + 48);
		const LONG n = InterlockedIncrement(&s_nIoErrors);
		if (n <= 16 || (n % 256) == 0)
		{
			char szPath[MAX_PATH];
			const char* const pszFile = AsyncFilePath(fileHandle, szPath, sizeof(szPath));
			Warning(eDLL_T::FS, "[STREAM-IOERR] status=%d win32=%d file=%d '%s' offset=%lld bytes=%u (total %ld)\n",
				static_cast<int>(status), win32Error, fileHandle, pszFile, offset, bytes, n);
		}
	}
	v_FS_AsyncNotifyFinishedSubreq(subreq, status, win32Error);
}

void VTextureStreamAbortFree::GetAdr(void) const
{
	LogFunAdr("TextureStream_ChargeBudget", v_TextureStream_ChargeBudget);
	LogFunAdr("FS_AsyncThread_NotifyFinishedSubreq", v_FS_AsyncNotifyFinishedSubreq);
}

void VTextureStreamAbortFree::GetFun(void) const
{
	// Unique interior of the budget charge: the mip-count arithmetic over the
	// texture header bytes. The prologue is walked up from there.
	CMemory interior = Module_FindPattern(g_GameDll,
		"41 0F B6 42 7C 41 0F B6 52 1C 2B D0 41 0F B6 42 1A 03 D0 "
		"41 0F B6 42 19 03 D0 41 0F B6 42 18 03 D0 41 0F B6 42 10");
	if (!interior.GetPtr())
	{
		Warning(eDLL_T::MS, "[STREAM-ABORT] budget charge pattern unresolved -- aborted mip sets keep their staging\n");
		return;
	}
	interior.FindPatternSelf("48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 0F B6 3D", CMemory::Direction::UP)
		.GetPtr(v_TextureStream_ChargeBudget);
	if (!v_TextureStream_ChargeBudget)
		Warning(eDLL_T::MS, "[STREAM-ABORT] budget charge prologue unresolved -- aborted mip sets keep their staging\n");

	// FS_GetAsyncFileTime: `and ecx,3FFh; lea rax,table; ...` -- the lea sits
	// ten bytes in.
	const CMemory fileTime = Module_FindPattern(g_GameDll,
		"48 83 EC 28 81 E1 FF 03 00 00 48 8D 05 ?? ?? ?? ?? 48 03 C9 4C 8D 4C 24 38 45 33 C0 33 D2 48 8B 0C C8 FF 15");
	if (fileTime.GetPtr())
		s_pAsyncFileTable = fileTime.Offset(10).ResolveRelativeAddress(3, 7).RCast<const HANDLE*>();
	else
		Warning(eDLL_T::MS, "[STREAM-ABORT] async file table unresolved -- abort log prints handles only\n");

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 54 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8B 79")
		.GetPtr(v_FS_AsyncNotifyFinishedSubreq);
	if (!v_FS_AsyncNotifyFinishedSubreq)
		Warning(eDLL_T::MS, "[STREAM-ABORT] FS_AsyncThread_NotifyFinishedSubreq unresolved -- read errors not logged\n");

	const HMODULE hExe = GetModuleHandleA(NULL);
	if (hExe)
	{
		const IMAGE_DOS_HEADER* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hExe);
		const IMAGE_NT_HEADERS64* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
			reinterpret_cast<const uint8_t*>(hExe) + dos->e_lfanew);
		s_imageBase = reinterpret_cast<uintptr_t>(hExe);
		s_imageEnd = s_imageBase + nt->OptionalHeader.SizeOfImage;
	}
}

void VTextureStreamAbortFree::Detour(const bool bAttach) const
{
	if (v_TextureStream_ChargeBudget && s_imageEnd)
		DetourSetup(&v_TextureStream_ChargeBudget, &Hook_TextureStream_ChargeBudget, bAttach);
	if (v_FS_AsyncNotifyFinishedSubreq)
	{
		DetourSetup(&v_FS_AsyncNotifyFinishedSubreq, &Hook_FS_AsyncNotifyFinishedSubreq, bAttach);
		Msg(eDLL_T::MS, "[STREAM-ABORT] hooks %s: budget charge %p, subreq notify %p\n",
			bAttach ? "attached" : "detached", v_TextureStream_ChargeBudget, v_FS_AsyncNotifyFinishedSubreq);
	}
}
