//=============================================================================//
//
// Purpose: Load a pak without paying for its UI images in the atlas.
//
// A BR map pak carries a 4096x4096 fullmap uiia that holds ~17,700 atlas
// tiles for as long as the pak is resident, so two such paks loaded at once
// exhaust the BC1 atlas and the engine fatals with "UI images ran out of
// room". A pak marked here has every uiia clamped to 1x1 before the engine's
// load handler runs: the asset still gets a real allocation, upload, and
// unload, it just costs one tile and draws as a 1x1 image.
//
//=============================================================================//

#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier0/threadtools.h"
#include "rpak_observe.h"
#include "ui_image_skip.h"

static constexpr int kMaxNoUiPaks = 16;
static char s_szNoUiPaks[kMaxNoUiPaks][64] = {};
static CThreadFastMutex s_noUiMutex;
static volatile LONG s_nStubbed = 0;

static const char* PakBaseName(const char* psz)
{
	const char* pszBase = psz;
	for (const char* p = psz; *p; ++p)
	{
		if (*p == '\\' || *p == '/')
			pszBase = p + 1;
	}
	return pszBase;
}

bool UIImageSkip_MarkPak(const char* pszPakName)
{
	if (!pszPakName || !pszPakName[0])
		return false;
	pszPakName = PakBaseName(pszPakName);
	if (strlen(pszPakName) >= sizeof(s_szNoUiPaks[0]))
		return false;

	AUTO_LOCK(s_noUiMutex);
	int nFree = -1;
	for (int i = 0; i < kMaxNoUiPaks; ++i)
	{
		if (!s_szNoUiPaks[i][0])
		{
			if (nFree < 0)
				nFree = i;
			continue;
		}
		if (_stricmp(s_szNoUiPaks[i], pszPakName) == 0)
			return true;
	}
	if (nFree < 0)
		return false;
	strncpy(s_szNoUiPaks[nFree], pszPakName, sizeof(s_szNoUiPaks[nFree]) - 1);
	s_szNoUiPaks[nFree][sizeof(s_szNoUiPaks[nFree]) - 1] = '\0';
	return true;
}

void UIImageSkip_UnmarkPak(const char* pszPakName)
{
	if (!pszPakName)
		return;
	pszPakName = PakBaseName(pszPakName);
	AUTO_LOCK(s_noUiMutex);
	for (int i = 0; i < kMaxNoUiPaks; ++i)
	{
		if (s_szNoUiPaks[i][0] && _stricmp(s_szNoUiPaks[i], pszPakName) == 0)
			s_szNoUiPaks[i][0] = '\0';
	}
}

static bool UIImageSkip_IsMarked(const char* pszPakName)
{
	if (!pszPakName)
		return false;
	pszPakName = PakBaseName(pszPakName);
	AUTO_LOCK(s_noUiMutex);
	for (int i = 0; i < kMaxNoUiPaks; ++i)
	{
		if (s_szNoUiPaks[i][0] && _stricmp(s_szNoUiPaks[i], pszPakName) == 0)
			return true;
	}
	return false;
}

static const char* UIImageSkip_PakName(int pakHandle)
{
	const uintptr_t base = Pak_GetSlotBase_S21();
	if (!base)
		return nullptr;
	const uintptr_t slot = base + static_cast<uintptr_t>(pakHandle & 0x1FF) * kS21_PakSlotStride;
	return *reinterpret_cast<const char**>(slot + kS21_PakSlot_Name);
}

static void Hook_UIImage_Load(unsigned char* hdr, unsigned char* data, int pakHandle, void* asset)
{
	const char* const pszPak = UIImageSkip_PakName(pakHandle);
	if (hdr && data && UIImageSkip_IsMarked(pszPak))
	{
		// Header dims are the layout rect; data +32..+38 are hi/lo stored dims,
		// which set the tile grid the handler allocates and uploads. A mixed
		// (class 3) image indexes a per-tile table whose first entry need not
		// be a data tile, so the stub is always read as one plain BC1 tile.
		uint16_t* const pFlags = reinterpret_cast<uint16_t*>(hdr + 18);
		const uint16_t nFlags = *pFlags;
		const uint16_t nW = *reinterpret_cast<uint16_t*>(data + 32);
		const uint16_t nH = *reinterpret_cast<uint16_t*>(data + 34);
		*pFlags = (nFlags & ~3) | 1;
		*reinterpret_cast<uint16_t*>(hdr + 24) = 1;
		*reinterpret_cast<uint16_t*>(hdr + 26) = 1;
		for (int off = 32; off <= 38; off += 2)
			*reinterpret_cast<uint16_t*>(data + off) = 1;

		const LONG n = InterlockedIncrement(&s_nStubbed);
		if (n <= 32)
		{
			Msg(eDLL_T::RTECH, "[UI-SKIP] '%s': uiia %ux%u flags 0x%X -> 1x1 BC1 stub\n",
				pszPak, nW, nH, nFlags);
		}
	}
	v_UIImage_Load_S21(hdr, data, pakHandle, asset);
}

void VUIImageSkipS21::GetFun(void) const
{
	// uiia load handler: bumps the cache generation, takes the cache SRW
	// lock, then memcmp's the owning slot's name against "ui.rpak".
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 4C 89 4C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 "
		"48 83 EC 30 FF 05 ?? ?? ?? ?? 48 8B D9 48 8B 0D ?? ?? ?? ?? 45 8B F0 "
		"48 81 C1 F0 01 00 00 48 8B F2 FF 15 ?? ?? ?? ?? 41 8B C6 "
		"4C 8D 0D ?? ?? ?? ?? 25 FF 01 00 00")
		.GetPtr(v_UIImage_Load_S21);

	if (!v_UIImage_Load_S21)
		Warning(eDLL_T::RTECH, "[UI-SKIP] uiia load handler pattern unresolved -- pak_requestload_noui disabled\n");
}

void VUIImageSkipS21::Detour(const bool bAttach) const
{
	if (v_UIImage_Load_S21)
		DetourSetup(&v_UIImage_Load_S21, &Hook_UIImage_Load, bAttach);
}
