//=============================================================================//
//
// Purpose: S21 .opt.starpak policy: auto-drop when core HD packs are missing.
//
//=============================================================================//
#include "core/stdafx.h"
#include "pak_opt_stream_drop.h"
#include "tier0/commandline.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"

// -1 auto, 0 never drop, 1 always drop. Attach-time only.
static ConVar sdk_pak_drop_opt_stream(
	"sdk_pak_drop_opt_stream", "-1", FCVAR_RELEASE,
	"Optional stream (.opt.starpak) policy: -1=auto (drop if HD packs missing), "
	"0=never drop, 1=always drop. Restart required.");

// Decision site inside S21 Pak_LoadHeaderToAllocHunkMemory.
// Pattern is unique in the client image.
static constexpr const char* kDecisionPat =
	"84 C0 0F 85 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? 0F 2E C6";

static constexpr const char* kEmulateLeaPat =
	"48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 50 ?? 48 85 C0 74 ?? "
	"48 8B 4C 24 ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 85 C0";

// Core HD packs used as the auto slim/full probe (same set as former slim hardlinks).
static const char* const kCoreOptRelPaths[] = {
	"paks\\Win64\\pc_all.opt.starpak",
	"paks\\Win64\\pc_all(01).opt.starpak",
	"paks\\Win64\\pc_roots(01).opt.starpak",
	"paks\\Win64\\pc_roots(02).opt.starpak",
};

static char s_stubPath[MAX_PATH] = {};
static bool s_stubReady = false;

static bool FileExistsA(const char* path)
{
	if (!path || !path[0])
		return false;
	const DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool GetGameRoot(char* out, size_t outSz)
{
	char exe[MAX_PATH] = {};
	const DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return false;
	char* slash = strrchr(exe, '\\');
	if (!slash)
		slash = strrchr(exe, '/');
	if (!slash)
		return false;
	*slash = '\0';
	strncpy_s(out, outSz, exe, _TRUNCATE);
	return true;
}

// True when the install has the core HD stream packs on disk.
static bool HasCoreOptStarpaks(void)
{
	char root[MAX_PATH] = {};
	if (!GetGameRoot(root, sizeof(root)))
		return false;

	for (size_t i = 0; i < sizeof(kCoreOptRelPaths) / sizeof(kCoreOptRelPaths[0]); ++i)
	{
		char full[MAX_PATH] = {};
		_snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", root, kCoreOptRelPaths[i]);
		if (!FileExistsA(full))
		{
			Msg(eDLL_T::RTECH,
				"[PAK-OPT-DROP] auto: missing '%s' -- treat as slim\n",
				kCoreOptRelPaths[i]);
			return false;
		}
	}
	return true;
}

// Whether to patch the engine force-drop site this boot.
static bool ShouldForceDropOptStreams(void)
{
	const int mode = sdk_pak_drop_opt_stream.GetInt();
	if (mode == 0)
		return false;
	if (mode > 0)
		return true;

	// auto (-1 or any negative)
	const bool hasHd = HasCoreOptStarpaks();
	if (hasHd)
		Msg(eDLL_T::RTECH,
			"[PAK-OPT-DROP] auto: core .opt.starpak present -- stock HD open path\n");
	else
		Msg(eDLL_T::RTECH,
			"[PAK-OPT-DROP] auto: core .opt.starpak absent -- force-drop optional set\n");
	return !hasHd;
}

static bool EnsureOptStreamStub(void)
{
	if (s_stubReady)
		return s_stubPath[0] != '\0';

	s_stubReady = true;
	char tmp[MAX_PATH] = {};
	const DWORD n = GetTempPathA(static_cast<DWORD>(sizeof(tmp)), tmp);
	if (n == 0 || n >= sizeof(tmp))
		return false;

	_snprintf_s(s_stubPath, sizeof(s_stubPath), _TRUNCATE,
		"%ss21_opt_stream_stub.bin", tmp);

	const HANDLE h = CreateFileA(s_stubPath, GENERIC_WRITE, FILE_SHARE_READ,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (h == INVALID_HANDLE_VALUE)
	{
		s_stubPath[0] = '\0';
		return false;
	}
	CloseHandle(h);
	return true;
}

static bool PathLooksLikeOptStarpak(const char* path)
{
	return path && V_stristr(path, ".opt.starpak") != nullptr;
}

// Resolve relative paths against game root for existence checks.
static bool OptStarpakExistsOnDisk(const char* path)
{
	if (FileExistsA(path))
		return true;

	// Relative to cwd (often game root)
	// Relative to exe dir
	char root[MAX_PATH] = {};
	if (!GetGameRoot(root, sizeof(root)))
		return false;

	// strip leading .\ or ./
	const char* rel = path;
	if (rel[0] == '.' && (rel[1] == '\\' || rel[1] == '/'))
		rel += 2;

	char full[MAX_PATH] = {};
	_snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", root, rel);
	return FileExistsA(full);
}

static int OpenStubOrInvalid(int logChannel, size_t* outSize, unsigned char flags,
	FsAsyncOpenPath_fn origOpen, const char* reason)
{
	static volatile long s_n = 0;
	const long n = InterlockedIncrement(&s_n);

	if (EnsureOptStreamStub())
	{
		if (n == 1)
			Msg(eDLL_T::RTECH,
				"[PAK-OPT-DROP] stream open remap -> stub '%s' (%s; later silent)\n",
				s_stubPath, reason);
		return origOpen(s_stubPath, logChannel, outSize, flags);
	}

	if (n <= 4 || (n % 512) == 0)
		Warning(eDLL_T::RTECH,
			"[PAK-OPT-DROP] #%ld stub create failed (%s flags=0x%X) -- INVALID\n",
			n, reason, (unsigned)flags);
	return -1;
}

bool PakOptStreamDrop_GuardOpen(const char* path, int logChannel, size_t* outSize,
	unsigned char flags, FsAsyncOpenPath_fn origOpen, int* pResult)
{
	// Null/empty = dropped optional streamFileName lazy-open.
	if (!path || !path[0])
	{
		*pResult = OpenStubOrInvalid(logChannel, outSize, flags, origOpen, "null/empty path");
		return true;
	}

	// Missing HD pack on a partial/full tree that still lists the path:
	// open stub instead of CreateFile miss / FATAL cascade.
	if (PathLooksLikeOptStarpak(path) && !OptStarpakExistsOnDisk(path))
	{
		*pResult = OpenStubOrInvalid(logChannel, outSize, flags, origOpen, "missing .opt.starpak");
		return true;
	}

	return false;
}

void VPakOptStreamDropS21::GetFun(void) const
{
	const CMemory decision = Module_FindPattern(g_GameDll, kDecisionPat);
	s_decisionSite = decision.GetPtr();
	if (!s_decisionSite)
	{
		Warning(eDLL_T::RTECH,
			"[PAK-OPT-DROP] decision site unresolved -- optional streams use stock path\n");
	}
	else
	{
		Msg(eDLL_T::RTECH,
			"[PAK-OPT-DROP] decision site @ %p (test al / jnz drop)\n",
			reinterpret_cast<void*>(s_decisionSite));
	}
}

void VPakOptStreamDropS21::Detour(const bool bAttach) const
{
	if (!s_decisionSite)
		return;

	uint8_t* const p = reinterpret_cast<uint8_t*>(s_decisionSite);
	uint8_t* const jnz = p + 2;

	if (bAttach)
	{
		if (!ShouldForceDropOptStreams())
		{
			Msg(eDLL_T::RTECH,
				"[PAK-OPT-DROP] force-drop patch skipped "
				"(sdk_pak_drop_opt_stream=%d) -- open guard still active\n",
				sdk_pak_drop_opt_stream.GetInt());
			return;
		}

		if (jnz[0] != 0x0F || jnz[1] != 0x85)
		{
			Warning(eDLL_T::RTECH,
				"[PAK-OPT-DROP] unexpected bytes at jnz @ %p (%02X %02X) -- abort patch\n",
				jnz, jnz[0], jnz[1]);
			return;
		}

		for (int i = 0; i < 6; ++i)
			s_origJnz[i] = jnz[i];

		const int32_t oldRel =
			*reinterpret_cast<const int32_t*>(jnz + 2);
		const int32_t newRel = oldRel + 1;
		const uint8_t patch[6] = {
			0x90,
			0xE9,
			static_cast<uint8_t>(newRel & 0xFF),
			static_cast<uint8_t>((newRel >> 8) & 0xFF),
			static_cast<uint8_t>((newRel >> 16) & 0xFF),
			static_cast<uint8_t>((newRel >> 24) & 0xFF),
		};
		CMemory(jnz).Patch({
			patch[0], patch[1], patch[2], patch[3], patch[4], patch[5]
		});
		s_patched = true;

		const CMemory emu = Module_FindPattern(g_GameDll, kEmulateLeaPat);
		if (emu.GetPtr())
		{
			const uint8_t* scan = reinterpret_cast<const uint8_t*>(emu.GetPtr());
			for (int off = 0; off < 0x80; ++off)
			{
				if (scan[off] == 0x88 && scan[off + 1] == 0x05 &&
					scan[off + 6] == 0xC6 && scan[off + 7] == 0x05 &&
					scan[off + 12] == 0x01)
				{
					const int32_t dDrop =
						*reinterpret_cast<const int32_t*>(scan + off + 2);
					const int32_t dChk =
						*reinterpret_cast<const int32_t*>(scan + off + 8);
					uint8_t* const pDrop = const_cast<uint8_t*>(
						scan + off + 6 + dDrop);
					uint8_t* const pChk = const_cast<uint8_t*>(
						scan + off + 13 + dChk);
					DWORD old = 0;
					if (VirtualProtect(pDrop, 1, PAGE_READWRITE, &old))
					{
						*pDrop = 1;
						VirtualProtect(pDrop, 1, old, &old);
					}
					if (VirtualProtect(pChk, 1, PAGE_READWRITE, &old))
					{
						*pChk = 1;
						VirtualProtect(pChk, 1, old, &old);
					}
					Msg(eDLL_T::RTECH,
						"[PAK-OPT-DROP] cache flags forced isDropping=1 hasChecked=1 "
						"(@ %p / %p)\n",
						pDrop, pChk);
					break;
				}
			}
		}

		Msg(eDLL_T::RTECH,
			"[PAK-OPT-DROP] optional stream set force-drop INSTALLED "
			"(jnz->jmp @ %p) -- .opt.starpak opens skipped\n",
			jnz);

		if (CommandLine()->CheckParm("-emulate_streaming_install"))
			Msg(eDLL_T::RTECH,
				"[PAK-OPT-DROP] note: -emulate_streaming_install also present "
				"(redundant with force-drop patch)\n");
	}
	else if (s_patched)
	{
		CMemory(jnz).Patch({
			s_origJnz[0], s_origJnz[1], s_origJnz[2],
			s_origJnz[3], s_origJnz[4], s_origJnz[5]
		});
		s_patched = false;
		Msg(eDLL_T::RTECH, "[PAK-OPT-DROP] force-drop restored to stock jnz\n");
	}
}
