//=============================================================================//
//
// Purpose: Spatial-grid build safety cap for the S21 bridge. See header.
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/shared/s21_bridge_compat.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "engine/server/server.h"
#include "engine/server/precache_natives.h"
#include "snapshot_diag.h"
#include "snapshot_send.h"
#include "snapshot_writer.h"
#include "public/edict.h"
#include "game/shared/dt_extend.h"
#include "game/server/weapon_select_mirror.h"
#include "public/tier0/tslist.h"
#include <mutex>
#include <unordered_set>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <atomic>
#include <vector>
#include <climits>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#include <intrin.h>
#include "tier0/memstd.h"
#include "tier0/memvalidate.h"
#include "engine/server/datablock_oversized.h"

#pragma intrinsic(_ReturnAddress)

//-----------------------------------------------------------------------------
// BuildEntityGrid: NaN bbox sticks world sentinels; grid memset wedges.
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Settings_ApplySettings_Server(block, class GUID, mask, forceReset).
//-----------------------------------------------------------------------------
using ApplySettings_t = void(__fastcall*)(uint64_t block, uint64_t guid,
                                          uint64_t mask, char forceReset);
using PlayerClassSettingsInit_t = uint64_t(__fastcall*)();
using HashFn_t = uint64_t(__fastcall*)(const char*);
using LookupFn_t = uint64_t(__fastcall*)(uint64_t);
static ApplySettings_t v_Settings_ApplySettings_Server = nullptr;
static PlayerClassSettingsInit_t v_PlayerClassSettings_Init = nullptr;
static HashFn_t v_Pak_StringHash = nullptr;       // (thunks to)
static LookupFn_t v_Pak_FindAssetVoid = nullptr;
static uint64_t* g_pSpectatorGuid = nullptr;

static void (*v_ApplySettingsModifiers)(__int64 a1, __int64 a2, char a3) = nullptr;

// SettingsLayout::LookupField: entry+4 low 24 bits = data offset.
using SettingsLayoutLookupField_t = int64_t(__fastcall*)(void* hashTable,
                                                          const char* fieldName);
static SettingsLayoutLookupField_t v_SettingsLayout_LookupField = nullptr;

static const char s_globalEmptyStr[8] = {0};


// NOP m_playerFlags bit 0x2 SET. S21 reads that bit as IsConnectionActive.
// One-shot at load; toggling the convar at runtime does not re-apply.
static ConVar sdk_bridge_suppress_connflag("sdk_bridge_suppress_connflag", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"BAND-AID: NOP the m_playerFlags bit-2 OR in the conn-writer so the S21 client "
	"reads the bridged player as connected (clears bit2 only; cannot black-screen, "
	"but also suppresses real disconnects). Default 1 (on); one-shot patch at load.");

// Warm pool capacity immediate sizes two of four backings; the other two are fixed.
// Raising it overruns the fixed regions. Default 0.
static ConVar sdk_snap_grow_propcap("sdk_snap_grow_propcap", "0",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"UNSAFE: grow the snapshot pool's per-frame prop/data entry capacity 16384 -> "
	"262144. Two of the four backing regions are fixed-size, so this overruns them "
	"and corrupts the heap. One-shot immediate patch at load. Default 0 (off).");


// Pre-install SettingsBlock at entity+0x5F08. Spawn path skips engine apply.
static ConVar sdk_bridge_preinstall_settings("sdk_bridge_preinstall_settings", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Per-player apply of the initial SettingsBlock to entity+0x5F08 -- the "
	"engine's own pre-pick spectator state, which the bridge's spawn path "
	"skips (without it: bbox NaN + tick stall). Sourced from "
	"sdk_player_fallback_class; the script's SetPlayerSettingsWithMods swaps "
	"in the real legend at pick. Default 1 (load-bearing). 0 = off.");







using FindStringIndex_t = __int64 (__fastcall*)(const char* a1);
static FindStringIndex_t v_FindStringIndex = nullptr;
static std::atomic<uint64_t> s_findStringIndexCalls{0};
static std::atomic<uint64_t> s_findStringIndexNullBypassed{0};

//-----------------------------------------------------------------------------
// ExtraParticleFilesTable is NULL on dedi; return -1 instead of deref.
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_FindStringIndex(const char* a1)
{
	s_findStringIndexCalls.fetch_add(1, std::memory_order_relaxed);

	const uintptr_t mod = static_cast<uintptr_t>(
		g_GameDll.GetModuleBase());
	const uint64_t tablePtr = mod
		? *reinterpret_cast<uint64_t*>(mod + 0xD415030)
		: 0;

	if (!tablePtr)
	{
		const uint64_t n = s_findStringIndexNullBypassed.fetch_add(
			1, std::memory_order_relaxed);
		if (n < 4 || (n & 0x3FFu) == 0)
		{
			Msg(eDLL_T::SERVER,
				"[FSI-NULL] bypass #%llu"
				"(=NULL on dedi; returning -1 for"
				"string='%s')\n",
				(unsigned long long)(n + 1),
				a1 ? a1 : "(null)");
		}
		return 0xFFFFFFFFLL;
	}

	return v_FindStringIndex(a1);
}


// Public accessors for other diagnostic TUs (declared in snapshot_diag.h).
// Returns 0 if the bridge's resolution hasn't run yet.
uint64_t Bridge_PakHash(const char* pakPath)
{
	if (!v_Pak_StringHash || !pakPath || !*pakPath) return 0;
	return v_Pak_StringHash(pakPath);
}

uint64_t Bridge_PakFindAssetVoid(uint64_t hash)
{
	if (!v_Pak_FindAssetVoid || !hash) return 0;
	return v_Pak_FindAssetVoid(hash);
}

//-----------------------------------------------------------------------------
// CGlobalNonRewinding ctor installs CObserverMode trackers at +0xB10+16*N.
//-----------------------------------------------------------------------------
using CGlobalNonRewinding_ctor_t = void* (__fastcall*)(void* instance);
static CGlobalNonRewinding_ctor_t v_CGlobalNonRewinding_ctor = nullptr;
static std::atomic<bool> s_observerTrackersInstalled{false};

// Patch rpak_defaults[0..7] to a static empty string before Apply.
// Field reads treat data[0] as BYTE*; VEH fires before SEH.
static ConVar sdk_settings_sweep_floats("sdk_settings_sweep_floats", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Sweep isolated 0xFFFFFFFF 4-byte slots (= -NaN as float, = -1 as int) "
	"in player settings defaults. Set to 1 (default) to catch single-field "
	"NaN floats (e.g. hull_mins.x/y) that the 8-byte qword sweep misses. "
	"Set to 0 to disable (revert to qword-only).");

static ConVar sdk_settings_sweep_latch("sdk_settings_sweep_latch", "1",
	FCVAR_RELEASE,
	"Latch the per-tick player settings -1 sentinel sweep after two clean "
	"passes on an unchanged block pointer/size. 0 = sweep every tick.");

// Spectator has no body/arms; SOLID_HITBOXES can read uninit hitboxes.
static ConVar sdk_player_fallback_class("sdk_player_fallback_class",
	"spectator",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Player-settings rpak applied to entity+0x5F08 before the script's "
	"SetPlayerSettingsWithMods swaps in the real legend at pick. Looked up "
	"as 'settings/player/mp/<name>.rpak'. Default 'spectator' (the engine's "
	"neutral pre-pick state -- NOT a legend). Restart required.");


// 0xFFFFFFFF is a float sentinel only if a neighbor looks float-shaped.
static inline bool LooksLikeFloatField(uint32_t w)
{
	// Sentinel/NaN/Inf, or exponent in 100..150. Zero counts; subnormals do not.
	const uint32_t exp = (w >> 23) & 0xFF;
	if (w == 0xFFFFFFFFu) return true;          // -NaN bit pattern
	if (exp == 0xFF) return true;                // Inf or NaN -- still float-shaped
	if (exp == 0) return (w == 0);               // pure zero counts; subnormal unlikely
	// Most real floats land in exponents 110..145 (1e-5.. 1e6 range). Anything
	// in that range we accept as float-shaped. Outside, probably int-like.
	return (exp >= 100 && exp <= 150);
}

static void Hook_ApplySettingsModifiers(__int64 a1, __int64 a2, char a3)
{
	v_ApplySettingsModifiers(a1, a2, a3);
}

// Hash spectator.rpak and write the GUID; PCS_Init Apply AVs on zero-offset fields.
static std::atomic<bool> s_guidPopulated{false};

static void EnsureSpectatorGuidPopulated()
{
	if (s_guidPopulated.exchange(true)) return;
	if (!v_Pak_StringHash || !v_Pak_FindAssetVoid || !g_pSpectatorGuid) return;
	if (*g_pSpectatorGuid != 0) return;

	const uint64_t hash =
		v_Pak_StringHash("settings/player/mp/spectator.rpak");
	if (!hash) return;

	const uint64_t hdr = v_Pak_FindAssetVoid(hash);
	if (!hdr)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] Pak_FindAssetVoid(0x%llX) returned NULL -- "
			"spectator.rpak not in asset table\n",
			(unsigned long long)hash);
		return;
	}

	*g_pSpectatorGuid = hash;
	Msg(eDLL_T::SERVER,
		"[s21-bridge] spectator GUID populated: 0x%llX (via direct "
		"Pak_StringHash + Pak_FindAssetVoid)\n",
		(unsigned long long)hash);
}

//-----------------------------------------------------------------------------
// PlayerClassSettings_Init once per map. SpawnServer's map-load churns paks.
//-----------------------------------------------------------------------------
static std::atomic<bool> s_canonicalClassInitDone{false};

static void RunCanonicalClassSettingsInit_Now(const char* siteTag)
{
	if (!v_PlayerClassSettings_Init)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] canonical class-init SKIPPED (%s): "
			"PlayerClassSettings_Init not resolved\n", siteTag);
		return;
	}

	// Make sure the spectator GUID is resolvable before PCS_Init runs its own
	// native Pak_FindAssetVoid lookup (harmless if PCS_Init repopulates it).
	EnsureSpectatorGuidPopulated();

	const uint64_t guidBefore = g_pSpectatorGuid ? *g_pSpectatorGuid : 0;
	Msg(eDLL_T::SERVER,
		"[s21-bridge] canonical class-init RUN (%s): PCS_Init=%p "
		"spectatorGuid(before)=0x%llX\n",
		siteTag, (void*)v_PlayerClassSettings_Init,
		(unsigned long long)guidBefore);

	__try
	{
		v_PlayerClassSettings_Init();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] canonical class-init FAULTED (%s, code=0x%lX) -- "
			"PCS_Init AV'd; legacy preinstall path still active\n",
			siteTag, GetExceptionCode());
		return;
	}

	const uint64_t guidAfter = g_pSpectatorGuid ? *g_pSpectatorGuid : 0;
	Msg(eDLL_T::SERVER,
		"[s21-bridge] canonical class-init OK (%s): spectatorGuid(after)=0x%llX\n",
		siteTag, (unsigned long long)guidAfter);
}

// Map-spawn entry: per-map one-shot, then run.
void Bridge_RunCanonicalClassSettingsInit(const char* siteTag)
{
	if (s_canonicalClassInitDone.exchange(true))
		return;
	RunCanonicalClassSettingsInit_Now(siteTag);
}


// Gated on the -1 sentinel in the layout's first qword.
static std::mutex g_settingsAppliedMutex;
static std::unordered_set<uint64_t> g_settingsAppliedEntities;

// Sweep and loadout writes trust this cap; buffer must be at least this big.
static constexpr uint32_t kSettingsBlockCap = 65536;

//-----------------------------------------------------------------------------
// Freeze packs across LevelShutdown so workers do not memmove a freed BCC.
//-----------------------------------------------------------------------------
static ConVar sdk_bridge_pack_freeze("sdk_bridge_pack_freeze", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"1 = during LevelShutdown..LevelInit, SendSnapshot forces native keepalive "
	"(no entity pack). Prevents changelevel pack AV on freed entity bases. "
	"0 = never freeze (legacy behaviour).");

static std::atomic<bool> s_packFrozen{ false };
static std::atomic<int>  s_packFreezeLogCount{ 0 };

void SnapshotDiag_SetPackFrozen(bool bFrozen)
{
	const bool prev = s_packFrozen.exchange(bFrozen, std::memory_order_acq_rel);
	if (prev == bFrozen)
		return;
	s_packFreezeLogCount.store(0, std::memory_order_relaxed);
	Warning(eDLL_T::SERVER,
		"[PACK-FREEZE] %s (sdk_bridge_pack_freeze=%d) -- "
		"SendSnapshot %s entity packs until LevelInit complete\n",
		bFrozen ? "ARMED" : "RELEASED",
		sdk_bridge_pack_freeze.GetInt(),
		bFrozen ? "will NOT build" : "resumes");
	if (!bFrozen)
		SnapshotSend_OnPackFreezeReleased();
}

bool SnapshotDiag_IsPackFrozen(void)
{
	return sdk_bridge_pack_freeze.GetBool()
		&& s_packFrozen.load(std::memory_order_acquire);
}

int SnapshotDiag_PackFreezeLogInc(void)
{
	return s_packFreezeLogCount.fetch_add(1, std::memory_order_relaxed);
}

//-----------------------------------------------------------------------------
// Clear per-entity tracking; map-2 reuse of a map-1 pointer would skip apply.
//-----------------------------------------------------------------------------
void SnapshotDiag_LevelShutdown()
{
	// Arm FIRST -- race with pack workers that may still be mid-frame.
	SnapshotDiag_SetPackFrozen(true);

	{
		std::lock_guard<std::mutex> lk(g_settingsAppliedMutex);
		g_settingsAppliedEntities.clear();
	}

	// Re-arm the canonical class-settings init so it runs again on the next map
	// (the engine drives PCS_Init from every CServer::SpawnServer).
	s_canonicalClassInitDone.store(false, std::memory_order_relaxed);

	// Drop map-1 anim-anchor latches; edict slots reuse on the next map.
	SnapshotWriter_LevelShutdown();
	SnapshotSend_LevelShutdown();
	DataBlockOversized_LevelShutdown();
}


// settings_player_layout via runtime table mod+0xD4ED1F0, slot[1]. Cache hits only.
static void* ResolvePlayerSettingsLayoutPtr(uint64_t** ppOutSlot = nullptr)
{
	static void* s_pLayout = nullptr;
	static uint64_t* s_pSlot = nullptr;

	if (s_pLayout)
	{
		if (ppOutSlot)
			*ppOutSlot = s_pSlot;
		return s_pLayout;
	}

	if (ppOutSlot)
		*ppOutSlot = nullptr;

	if (!v_Pak_StringHash)
		return nullptr;

	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!mod)
		return nullptr;

	const char* const layoutName =
		"settings_layout/settings_player_layout.rpak";
	const uint64_t layoutHash = v_Pak_StringHash(layoutName);
	if (!layoutHash)
		return nullptr;

	uint64_t* const tableBase = reinterpret_cast<uint64_t*>(mod + 0xD4ED1F0);
	for (int v2 = 0; v2 < 0x40; ++v2)
	{
		const uint64_t idx =
			(static_cast<uint8_t>(v2) +
			 static_cast<uint8_t>(layoutHash)) & 0x3F;
		uint64_t* const slot = &tableBase[5ull * idx];
		if (slot[0] == layoutHash)
		{
			if (slot[1])
			{
				s_pLayout = reinterpret_cast<void*>(slot[1]);
				s_pSlot = slot;
			}
			break;
		}
		if (slot[0] == 0)
			break;
	}

	if (ppOutSlot)
		*ppOutSlot = s_pSlot;
	return s_pLayout;
}

uint32_t Bridge_LookupPlayerSettingsFieldOffset(const char* pszFieldName)
{
	if (!pszFieldName || !pszFieldName[0] || !v_SettingsLayout_LookupField)
		return 0xFFFFFFFFu;

	void* const pLayout = ResolvePlayerSettingsLayoutPtr();
	if (!pLayout)
		return 0xFFFFFFFFu;

	const uint8_t* const entry = reinterpret_cast<const uint8_t*>(
		v_SettingsLayout_LookupField(pLayout, pszFieldName));
	if (!entry)
		return 0xFFFFFFFFu;

	return *reinterpret_cast<const uint32_t*>(entry + 4) & 0xFFFFFFu;
}

bool Bridge_HasPlayerSettingsLayout(void)
{
	return ResolvePlayerSettingsLayoutPtr() != nullptr;
}

// Pose-offset globals file-default to 0; hull then reads the string-pointer region.

static ConVar sdk_gib_finders_patch("sdk_gib_finders_patch", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Patch gibModels / gibModelsSoftened settings-finder offsets from the live player layout.");

static ConVar sdk_gib_finder_guard("sdk_gib_finder_guard", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Fail-closed reimplementation of HasGibModel (both class halves). 0 = call original.");

static ConVar sdk_gib_spawn_guard("sdk_gib_spawn_guard", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Early-out the gib spawn path while either gib finder pair is unresolved.");

static std::atomic<bool> s_gibModelsResolved{false};
static std::atomic<bool> s_gibModelsSoftenedResolved{false};

enum GibFinderCheck_e
{
	GIBCHECK_OK = 0,
	GIBCHECK_HEADER_UNREADABLE,
	GIBCHECK_SLOT_UNREADABLE,
	GIBCHECK_EMPTY,          // slot reachable, model pointer null -- layout carries no gibs
	GIBCHECK_STR_UNREADABLE,
};

// Evaluate the engine's gib-array accessor against a live settings block. The engine spawn loop
// dereferences slot 0 without a count check, so a null pointer here means gibs must stay off.
static GibFinderCheck_e GibFinder_ValidatePair(const uint8_t* const pData, const uint32_t A,
	const uint32_t B)
{
	uint32_t base = A;
	if (A & 0x80000000u)
	{
		const uint8_t* const pHdr = pData + (A & 0x7FFFFFFFu) + 4;
		if (!Mem_IsReadableCached(pHdr, 4))
			return GIBCHECK_HEADER_UNREADABLE;
		base = *reinterpret_cast<const uint32_t*>(pHdr);
	}

	const uint8_t* const pSlot = pData + static_cast<uint64_t>(base) + B;
	if (!Mem_IsReadableCached(pSlot, 8))
		return GIBCHECK_SLOT_UNREADABLE;

	char* const psz = *reinterpret_cast<char* const*>(const_cast<uint8_t*>(pSlot));
	if (!psz)
		return GIBCHECK_EMPTY;
	if (!Mem_IsReadableCached(psz, 1))
		return GIBCHECK_STR_UNREADABLE;

	return GIBCHECK_OK;
}

// Resolve one gib array finder pair (big finder + its val leaf). On validation
// failure restores the three written dwords to their pre-patch values.
static bool PatchOneGibFinderPair(const uintptr_t mod, const uint64_t layoutPtr,
	const uint8_t* const pLiveBlock, const char* const pszFieldName,
	const uintptr_t bigFinderRva, const uintptr_t leafRva,
	const char** const ppszMissStep, uint32_t* const pnValueOff,
	uint32_t* const pnSubIdx, uint32_t* const pnElemSize, uint32_t* const pnMemberOff)
{
	*ppszMissStep = "lookup";
	*pnValueOff = 0;
	*pnSubIdx = 0;
	*pnElemSize = 0;
	*pnMemberOff = 0;

	const uint8_t* const entry = reinterpret_cast<const uint8_t*>(
		v_SettingsLayout_LookupField(
			reinterpret_cast<void*>(layoutPtr), pszFieldName));
	if (!entry)
		return false;

	const uint32_t entry4 = *reinterpret_cast<const uint32_t*>(entry + 4);
	const uint32_t valueOff = entry4 & 0xFFFFFFu;
	const uint32_t subIdx = (entry4 >> 24) & 0xFFu;
	*pnValueOff = valueOff;
	*pnSubIdx = subIdx;

	*ppszMissStep = "sublayout";
	const uint64_t sublayArrBase =
		*reinterpret_cast<const uint64_t*>(layoutPtr + 64);
	if (!sublayArrBase || subIdx >= 64u)
		return false;

	const uint8_t* const subLayout =
		reinterpret_cast<const uint8_t*>(sublayArrBase + 72ull * subIdx);
	if (!Mem_IsReadableCached(subLayout, 72))
		return false;
	const int32_t elemSize =
		*reinterpret_cast<const int32_t*>(subLayout + 48);
	*ppszMissStep = "elemSize";
	if (elemSize < 8 || elemSize > 1024)
		return false;
	*pnElemSize = static_cast<uint32_t>(elemSize);

	*ppszMissStep = "member";
	uint32_t memberOff = 0xFFFFFFFFu;
	const uint8_t* const memberEntry = reinterpret_cast<const uint8_t*>(
		v_SettingsLayout_LookupField(
			const_cast<uint8_t*>(subLayout), "val"));
	if (memberEntry)
		memberOff = *reinterpret_cast<const uint32_t*>(memberEntry + 4) & 0xFFFFFFu;
	else if (elemSize == 8)
		memberOff = 0;
	else
		return false;
	*pnMemberOff = memberOff;

	volatile uint32_t* const pBigOff =
		reinterpret_cast<volatile uint32_t*>(mod + bigFinderRva + 0x1C);
	volatile uint32_t* const pBigElem =
		reinterpret_cast<volatile uint32_t*>(mod + bigFinderRva + 0x24);
	volatile uint32_t* const pLeaf =
		reinterpret_cast<volatile uint32_t*>(mod + leafRva);

	const uint32_t prevBigOff = *pBigOff;
	const uint32_t prevBigElem = *pBigElem;
	const uint32_t prevLeaf = *pLeaf;

	*pBigOff = valueOff;
	*pBigElem = static_cast<uint32_t>(elemSize);
	*pLeaf = memberOff;

	const GibFinderCheck_e check =
		GibFinder_ValidatePair(pLiveBlock, valueOff, memberOff);
	if (check != GIBCHECK_OK)
	{
		// An empty list means the offsets are RIGHT and the layout simply has no gibs; keep
		// them so HasGibModel reads the real slot and answers false instead of refusing.
		if (check != GIBCHECK_EMPTY)
		{
			*pLeaf = prevLeaf;
			*pBigElem = prevBigElem;
			*pBigOff = prevBigOff;
		}

		switch (check)
		{
		case GIBCHECK_HEADER_UNREADABLE: *ppszMissStep = "indirect header unreadable"; break;
		case GIBCHECK_SLOT_UNREADABLE:   *ppszMissStep = "model slot unreadable"; break;
		case GIBCHECK_EMPTY:             *ppszMissStep = "no gib models in this layout"; break;
		default:                         *ppszMissStep = "model string unreadable"; break;
		}
		return false;
	}

	*ppszMissStep = nullptr;
	return true;
}

static void PatchGibFinders(const uintptr_t mod, const uint64_t layoutPtr,
	const uint64_t entPtr)
{
	if (!sdk_gib_finders_patch.GetBool())
		return;

	if (!entPtr)
	{
		Warning(eDLL_T::SERVER,
			"[GIB-FINDERS] skipped -- null entity; globals left untouched\n");
		return;
	}

	const uint64_t liveBlock =
		*reinterpret_cast<const uint64_t*>(entPtr + 24328); // m_classSettings data
	if (!liveBlock ||
		!Mem_IsReadableCached(reinterpret_cast<const void*>(liveBlock), 8))
	{
		Warning(eDLL_T::SERVER,
			"[GIB-FINDERS] skipped -- live settings block unreadable; "
			"globals left untouched\n");
		return;
	}

	const uint8_t* const pLiveBlock =
		reinterpret_cast<const uint8_t*>(liveBlock);

	struct GibPair_t
	{
		const char* pszName;
		uintptr_t bigFinderRva;
		uintptr_t leafRva;
		std::atomic<bool>* pResolved;
	};

	const GibPair_t pairs[] =
	{
		{ "gibModels", 0x23884E0, 0x2387D90, &s_gibModelsResolved },
		{ "gibModelsSoftened", 0x2388470, 0x2387D48, &s_gibModelsSoftenedResolved },
	};

	for (const GibPair_t& pair : pairs)
	{
		const char* pszMiss = nullptr;
		uint32_t valueOff = 0, subIdx = 0, elemSize = 0, memberOff = 0;
		const bool ok = PatchOneGibFinderPair(mod, layoutPtr, pLiveBlock,
			pair.pszName, pair.bigFinderRva, pair.leafRva,
			&pszMiss, &valueOff, &subIdx, &elemSize, &memberOff);
		if (ok)
		{
			pair.pResolved->store(true, std::memory_order_release);
			Msg(eDLL_T::SERVER,
				"[GIB-FINDERS] %s resolved: valueOff=%u subIdx=%u "
				"elemSize=%u memberOff=%u\n",
				pair.pszName, valueOff, subIdx, elemSize, memberOff);
		}
		else
		{
			// No usable gibModels array; spawn guard stays closed.
			pair.pResolved->store(false, std::memory_order_release);
		}
	}
}

static void PatchPlayerLayoutGlobals(const uint64_t entPtr)
{
	static std::atomic<bool> s_patched{false};
	if (s_patched.load(std::memory_order_acquire)) return;
	if (!v_Pak_StringHash || !v_SettingsLayout_LookupField) return;

	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!mod) return;

	uint64_t* playerSlot = nullptr;
	void* const pLayout = ResolvePlayerSettingsLayoutPtr(&playerSlot);
	if (!pLayout)
		return;

	const uint64_t layoutPtr = reinterpret_cast<uint64_t>(pLayout);

	// Finder dump: name@+8, layout@+0x10, index@+0x18, offset@+0x1C, elemSize@+0x24.
	{
		const uint64_t Raddr = mod + 0x2385BE0; // R = array-field root finder
		Msg(eDLL_T::SERVER,
			"[FINDER-DIAG] playerSlot=%p hash=0x%llX layout=%p "
			"slot3(roots)=0x%llX slot4(cache)=0x%llX R=0x%llX\n",
			(void*)playerSlot,
			playerSlot ? (unsigned long long)playerSlot[0] : 0ull,
			(void*)layoutPtr,
			playerSlot ? (unsigned long long)playerSlot[3] : 0ull,
			playerSlot ? (unsigned long long)playerSlot[4] : 0ull,
			(unsigned long long)Raddr);

		bool foundR = false;
		uint64_t node = playerSlot ? playerSlot[3] : 0ull;
		for (int g = 0; node && g < 64; ++g)
		{
			const char*    nm = *reinterpret_cast<const char**>(node + 8);
			const int      fi = *reinterpret_cast<int*>(node + 0x18);
			const uint32_t fo = *reinterpret_cast<uint32_t*>(node + 0x1C);
			const uint64_t fl = *reinterpret_cast<uint64_t*>(node + 0x10);
			const uint64_t ch = *reinterpret_cast<uint64_t*>(node + 0x28);
			Msg(eDLL_T::SERVER,
				"[FINDER-DIAG]   root@0x%llX name='%s' idx=%d off=%u "
				"layout=0x%llX childHead=0x%llX%s\n",
				(unsigned long long)node, nm ? nm : "(null)", fi, fo,
				(unsigned long long)fl, (unsigned long long)ch,
				node == Raddr ? "  <== R (array root)" : "");
			if (node == Raddr) foundR = true;
			node = *reinterpret_cast<uint64_t*>(node + 0x30);
		}
		Msg(eDLL_T::SERVER, "[FINDER-DIAG] R present in slot3 root list: %s\n",
			foundR ? "YES" : "NO");

		Msg(eDLL_T::SERVER,
			"[FINDER-DIAG] R@0x%llX off=%u layout=0x%llX childHead=0x%llX "
			"subHead=0x%llX\n",
			(unsigned long long)Raddr,
			*reinterpret_cast<uint32_t*>(Raddr + 0x1C),
			*reinterpret_cast<uint64_t*>(Raddr + 0x10),
			*reinterpret_cast<uint64_t*>(Raddr + 0x28),
			*reinterpret_cast<uint64_t*>(Raddr + 0x38));
		uint64_t c = *reinterpret_cast<uint64_t*>(Raddr + 0x28);
		for (int g = 0; c && g < 32; ++g)
		{
			const char*    cn = *reinterpret_cast<const char**>(c + 8);
			const int      ci = *reinterpret_cast<int*>(c + 0x18);
			const uint32_t co = *reinterpret_cast<uint32_t*>(c + 0x1C);
			const uint32_t cs = *reinterpret_cast<uint32_t*>(c + 0x24);
			Msg(eDLL_T::SERVER,
				"[FINDER-DIAG]   child@0x%llX name='%s' idx=%d off=%u stride=%u\n",
				(unsigned long long)c, cn ? cn : "(null)", ci, co, cs);
			c = *reinterpret_cast<uint64_t*>(c + 0x30);
		}
	}

	// Do not re-invoke the field-finder resolver: it AVs on the 0xFFFFFFFF header.

	// Look up "poseSettings" in the layout's hash table -> entry holds
	// {uint16 type, uint16 nameOff, uint32 (low24=valueOffset, high8=
	// sublayoutIdx)} at entry+0/2/4.
	uint32_t poseValueOff  = 0xFFFFFFFFu;
	uint32_t poseSubIdx    = 0xFFu;
	uint32_t poseElemSize  = 0;
	{
		const uint8_t* entry = reinterpret_cast<const uint8_t*>(
			v_SettingsLayout_LookupField(
				reinterpret_cast<void*>(layoutPtr), "poseSettings"));
		if (entry)
		{
			const uint32_t entry4 =
				*reinterpret_cast<const uint32_t*>(entry + 4);
			poseValueOff = entry4 & 0xFFFFFFu;
			poseSubIdx   = (entry4 >> 24) & 0xFFu;

			const uint64_t sublayArrBase =
				*reinterpret_cast<const uint64_t*>(layoutPtr + 64);
			if (sublayArrBase && poseSubIdx < 64u)
			{
				const uint8_t* subLayout =
					reinterpret_cast<const uint8_t*>(
						sublayArrBase + 72ull * poseSubIdx);
				const int32_t layoutSize =
					*reinterpret_cast<const int32_t*>(subLayout + 48);
				if (layoutSize >= 12 && layoutSize <= 1024)
					poseElemSize = static_cast<uint32_t>(layoutSize);
			}
			if (poseElemSize == 0) poseElemSize = 104; // observed default
		}
	}

	// Map ents bake slipSpeed/slipAcceleration = -1 (class default). Offset must not stay -1.
	uint32_t slipSpeedOff = 0xFFFFFFFFu;
	uint32_t slipAccelOff = 0xFFFFFFFFu;
	uint32_t skydiveSlipSpeedOff = 0xFFFFFFFFu;
	uint32_t skydiveSlipAccelOff = 0xFFFFFFFFu;
	{
		auto lookupOff = [&](const char* name) -> uint32_t
		{
			const uint8_t* entry = reinterpret_cast<const uint8_t*>(
				v_SettingsLayout_LookupField(
					reinterpret_cast<void*>(layoutPtr), name));
			if (!entry)
				return 0xFFFFFFFFu;
			return *reinterpret_cast<const uint32_t*>(entry + 4) & 0xFFFFFFu;
		};
		slipSpeedOff         = lookupOff("slipSpeed");
		slipAccelOff         = lookupOff("slipAcceleration");
		skydiveSlipSpeedOff  = lookupOff("skydive_slipSpeed");
		skydiveSlipAccelOff  = lookupOff("skydive_slipAcceleration");

		// SettingsFieldFinder leaf layout: +0 offset dword (the global
		// FullWalkMove reads). RVAs = 0x14238xxxx -.
		if (slipSpeedOff != 0xFFFFFFFFu)
			*reinterpret_cast<volatile uint32_t*>(mod + 0x23875A0) = slipSpeedOff;
		if (slipAccelOff != 0xFFFFFFFFu)
			*reinterpret_cast<volatile uint32_t*>(mod + 0x2386728) = slipAccelOff;
		if (skydiveSlipSpeedOff != 0xFFFFFFFFu)
			*reinterpret_cast<volatile uint32_t*>(mod + 0x23860D0) = skydiveSlipSpeedOff;
		if (skydiveSlipAccelOff != 0xFFFFFFFFu)
			*reinterpret_cast<volatile uint32_t*>(mod + 0x2386608) = skydiveSlipAccelOff;
	}

	Msg(eDLL_T::SERVER,
		"[SLIP-SETTINGS] finders: slipSpeed off=%u%s  slipAccel off=%u%s  "
		"skydive_slipSpeed off=%u%s  skydive_slipAccel off=%u%s\n",
		slipSpeedOff, slipSpeedOff == 0xFFFFFFFFu ? " (MISS)" : "",
		slipAccelOff, slipAccelOff == 0xFFFFFFFFu ? " (MISS)" : "",
		skydiveSlipSpeedOff, skydiveSlipSpeedOff == 0xFFFFFFFFu ? " (MISS)" : "",
		skydiveSlipAccelOff, skydiveSlipAccelOff == 0xFFFFFFFFu ? " (MISS)" : "");

	if (poseValueOff == 0xFFFFFFFFu || poseElemSize == 0)
	{
		Warning(eDLL_T::SERVER,
			"[POSE-GLOBALS] LookupField failed for 'poseSettings' "
			"(layoutPtr=%p) -- pose globals not patched; hull falls back to "
			"boundary fixes. Slip finders attempted above regardless.\n",
			(void*)layoutPtr);
		PatchGibFinders(mod, layoutPtr, entPtr);
		// Still mark patched so we don't re-enter forever; slip write already
		// ran. Pose can still fall back to Layer B.
		s_patched.store(true, std::memory_order_release);
		return;
	}

	const uint32_t standingOff = poseValueOff;
	const uint32_t crouchOff   = poseValueOff + poseElemSize;

	// .data is RW; no VirtualProtect.
	*reinterpret_cast<volatile uint32_t*>(mod + 0x2387CAC) = standingOff;
	*reinterpret_cast<volatile uint32_t*>(mod + 0x2387ECC) = crouchOff;

	// idx 3 is valueOff + 3*elemSize, not standingOff. WalkMove reads idx -1 as the array base.
	*reinterpret_cast<volatile uint32_t*>(mod + 0x238856C) = poseValueOff + 2u * poseElemSize; // poseSettings[2]
	*reinterpret_cast<volatile uint32_t*>(mod + 0x23864AC) = poseValueOff + 3u * poseElemSize; // poseSettings[3]
	*reinterpret_cast<volatile uint32_t*>(mod + 0x2387F6C) = poseValueOff;                     // poseSettings array base (WalkMove)
	*reinterpret_cast<volatile uint32_t*>(mod + 0x2387F74) = poseElemSize;                     // its stride (finder+0x24)

	Msg(eDLL_T::SERVER,
		"[POSE-GLOBALS] PATCHED 5 poseSettings finders: layoutPtr=%p valueOff=%u "
		"subIdx=%u elemSize=%u -> CAC=%u(idx0) ECC=%u(idx1) 0x238856C=%u(idx2) "
		"0x23864AC=%u(idx3) 0x2387F6C=%u(arrayBase/WalkMove) stride=%u. Hull AND "
		"ground movement now read the correct pose offsets.\n",
		(void*)layoutPtr, poseValueOff, poseSubIdx, poseElemSize,
		standingOff, crouchOff, poseValueOff + 2u * poseElemSize,
		poseValueOff + 3u * poseElemSize, poseValueOff, poseElemSize);

	// Verify: re-walk R's children and log resolved offsets (pose + gibs).
	// If the engine re-resolve worked, every child off is now non-zero
	// (poseSettings -> 120/224/328/432/120, gib arrays -> their layout offsets).
	{
		uint64_t c = *reinterpret_cast<uint64_t*>((mod + 0x2385BE0) + 0x28);
		for (int g = 0; c && g < 32; ++g)
		{
			const char*    cn = *reinterpret_cast<const char**>(c + 8);
			const int      ci = *reinterpret_cast<int*>(c + 0x18);
			const uint32_t co = *reinterpret_cast<uint32_t*>(c + 0x1C);
			const uint32_t cs = *reinterpret_cast<uint32_t*>(c + 0x24);
			Msg(eDLL_T::SERVER,
				"[FINDER-DIAG] POST-RESOLVE child name='%s' idx=%d off=%u stride=%u\n",
				cn ? cn : "(null)", ci, co, cs);
			c = *reinterpret_cast<uint64_t*>(c + 0x30);
		}
	}

	PatchGibFinders(mod, layoutPtr, entPtr);

	s_patched.store(true, std::memory_order_release);
}


static void EnsurePlayerSettingsApplied_Inner(uint64_t entPtr, const char* siteTag)
{
	__try
	{
		if (!entPtr)
			return;
		const uintptr_t serverClass = *reinterpret_cast<uintptr_t*>(entPtr + 0x50);
		if (!serverClass)
			return;
		const char* const className = *reinterpret_cast<const char**>(serverClass + 0x00);
		if (!className || _stricmp(className, "CPlayer") != 0)
			return;

		const uint64_t layoutData =
			*reinterpret_cast<const uint64_t*>(entPtr + 24328);
		if (!layoutData) return;

		// Try the configured fallback class first (default "bangalore" -- a
		// real character with valid bodyModel/armsModel). If the rpak isn't
		// in the asset table, fall back to "spectator" (always present).
		const char* fallbackClass = sdk_player_fallback_class.GetString();
		if (!fallbackClass || !*fallbackClass) fallbackClass = "bangalore";

		char rpakPath[128];
		V_snprintf(rpakPath, sizeof(rpakPath),
			"settings/player/mp/%s.rpak", fallbackClass);

		uint64_t hash = v_Pak_StringHash(rpakPath);
		uint64_t hdrPtr = hash ? v_Pak_FindAssetVoid(hash) : 0;
		const char* resolvedClass = fallbackClass;

		if (!hash || !hdrPtr)
		{
			Warning(eDLL_T::SERVER,
				"[s21-bridge] fallback class '%s' rpak not found "
				"(hash=0x%llX hdr=%p) -- falling back to 'spectator'\n",
				fallbackClass, (unsigned long long)hash, (void*)hdrPtr);
			hash = v_Pak_StringHash("settings/player/mp/spectator.rpak");
			if (!hash) return;
			hdrPtr = v_Pak_FindAssetVoid(hash);
			if (!hdrPtr) return;
			resolvedClass = "spectator";
		}

		const uint64_t* hdr = reinterpret_cast<const uint64_t*>(hdrPtr);
		const void* defaultData = reinterpret_cast<const void*>(hdr[1]);
		const uint32_t dataSize =
			*reinterpret_cast<const uint32_t*>(hdrPtr + 0x38);
		if (!defaultData || dataSize < 8)
		{
			Warning(eDLL_T::SERVER,
				"[s21-bridge] class '%s' rpak loaded but defaultData=%p "
				"dataSize=%u -- skip\n",
				resolvedClass, defaultData, dataSize);
			return;
		}

		constexpr uint32_t kExtraSize = 64;
		IMemAlloc* pAlloc = g_pMemAllocSingleton;
		if (!pAlloc) pAlloc = CreateGlobalMemAlloc();
		if (!pAlloc) return;

		// kExtraSize must stay 64 (SettingsBlockExtraDataSize for player).
		// Oversize to kSettingsBlockCap so same-size loadout reuse cannot overrun.
		const uint32_t allocDataSize =
			(dataSize > kSettingsBlockCap) ? dataSize : kSettingsBlockCap;
		uint8_t* allocBase = reinterpret_cast<uint8_t*>(
			pAlloc->Alloc(kExtraSize + allocDataSize));
		if (!allocBase) return;

		uint8_t* data = allocBase + kExtraSize;
		// Copy rpak defaultData so nested poseSettings hulls survive. Zero the tail.
		memcpy(data, defaultData, dataSize);
		if (allocDataSize > dataSize)
			memset(data + dataSize, 0, allocDataSize - dataSize);

		*reinterpret_cast<uint64_t*>(data) =
			reinterpret_cast<uint64_t>(s_globalEmptyStr);

		// Sweep 0xFFFFFFFF inherit-markers in scalar float fields.
		if (sdk_settings_sweep_floats.GetBool())
		{
			for (uint32_t off = 16; off + 4 <= dataSize; off += 4)
			{
				uint32_t* slot =
					reinterpret_cast<uint32_t*>(data + off);
				if (*slot != 0xFFFFFFFFu) continue;
				const uint32_t prev = (off >= 4)
					? *reinterpret_cast<uint32_t*>(data + off - 4)
					: 0u;
				const uint32_t next = (off + 8 <= dataSize)
					? *reinterpret_cast<uint32_t*>(data + off + 4)
					: 0u;
				if (LooksLikeFloatField(prev) ||
				    LooksLikeFloatField(next))
				{
					*slot = 0;
				}
			}
		}

		// m_classSettings[32] @0x5F08; clearing 48 ran into m_modInventory @0x5F2C.
		memset(reinterpret_cast<void*>(entPtr + 0x5F08), 0, 32);
		*reinterpret_cast<int*>(entPtr + 0x5F28) = 0;
		*reinterpret_cast<uint64_t*>(entPtr + 0x5F08) =
			reinterpret_cast<uint64_t>(data);
		*reinterpret_cast<uint64_t*>(entPtr + 0x5F10) = hdrPtr;

		// Apply with the asset's modFlags at hdr+60, not all-bits.
		static std::atomic<bool> s_engineApplyAnnounced{false};
		bool engineApplyAttempted = false;
		bool engineApplySucceeded = false;
		uint64_t postApplyData = 0;
		uint64_t postApplyLayout = 0;
		uint64_t assetModFlags = 0;
		uint64_t assetModValuesPtr = 0;
		uint32_t assetModValuesCount = 0;
		// hdrPtr from Pak_FindAssetVoid success path -- closed-form header fields.
		assetModFlags = static_cast<uint64_t>(
			*reinterpret_cast<uint32_t*>(hdrPtr + 60));
		assetModValuesPtr = *reinterpret_cast<uint64_t*>(
			hdrPtr + 48);
		assetModValuesCount = *reinterpret_cast<uint32_t*>(
			hdrPtr + 68);

		// One-shot dump of asset modValues that target low offsets
		// (likely-hull range). Helps pinpoint which modifier writes
		// 0xFFFFFFFF into the hull positions.
		static std::atomic<bool> s_modValuesDumped{false};
		if (!s_modValuesDumped.exchange(true) &&
		    assetModValuesPtr && assetModValuesCount > 0 &&
		    assetModValuesCount < 4096)
		{
			Msg(eDLL_T::SERVER,
				"[MODS] class='%s' hdr=%p modFlags=0x%llX "
				"modValuesPtr=%p modValuesCount=%u\n",
				resolvedClass, (void*)hdrPtr,
				(unsigned long long)assetModFlags,
				(void*)assetModValuesPtr, assetModValuesCount);

			const uint8_t* modBytes =
				reinterpret_cast<const uint8_t*>(assetModValuesPtr);
			int logged = 0;
			for (uint32_t i = 0;
				i < assetModValuesCount && logged < 32; ++i)
			{
				const uint8_t* m = modBytes + 12 * i;
				const uint16_t nameIdx =
					*reinterpret_cast<const uint16_t*>(m + 0);
				const uint16_t op =
					*reinterpret_cast<const uint16_t*>(m + 2);
				const uint32_t voff =
					*reinterpret_cast<const uint32_t*>(m + 4);
				const uint32_t val =
					*reinterpret_cast<const uint32_t*>(m + 8);
				// Log mods in hull range OR with 0xFFFFFFFF value
				// (= candidate corruption writers).
				const bool hullRange = (voff < 48u);
				const bool sentinel = (val == 0xFFFFFFFFu);
				if (hullRange || sentinel)
				{
					Msg(eDLL_T::SERVER,
						"[MODS] mod#%u nameIdx=%u op=%u "
						"voff=%u val=0x%08X%s%s\n",
						i, nameIdx, op, voff, val,
						hullRange ? " HULL_RANGE" : "",
						sentinel ? " SENTINEL" : "");
					++logged;
				}
			}
			Msg(eDLL_T::SERVER,
				"[MODS] dump complete (showed %d entries)\n",
				logged);
		}

		if (v_Settings_ApplySettings_Server)
		{
			engineApplyAttempted = true;
			__try
			{
				// Asset modFlags, not all-bits.
				v_Settings_ApplySettings_Server(
					entPtr + 24328, hash,
					assetModFlags, 1);
				engineApplySucceeded = true;
				postApplyData = *reinterpret_cast<uint64_t*>(
					entPtr + 24328);
				postApplyLayout = *reinterpret_cast<uint64_t*>(
					entPtr + 24336);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				Warning(eDLL_T::SERVER,
					"[s21-bridge] Settings_ApplySettings_Server SEH "
					"class='%s' hash=0x%llX mask=0x%llX entPtr=%p "
					"-- falling back to bridge memcpy.\n",
					resolvedClass,
					(unsigned long long)hash,
					(unsigned long long)assetModFlags,
					(void*)entPtr);
				*reinterpret_cast<uint64_t*>(entPtr + 24328) =
					reinterpret_cast<uint64_t>(data);
				*reinterpret_cast<uint64_t*>(entPtr + 24336) = hdrPtr;
			}
		}
		if (!s_engineApplyAnnounced.exchange(true))
		{
			Msg(eDLL_T::SERVER,
				"[s21-bridge] Engine pipeline attempt=%d "
				"succeeded=%d mask=0x%llX post_data=%p post_layout=%p "
				"(class='%s' hash=0x%llX)\n",
				(int)engineApplyAttempted,
				(int)engineApplySucceeded,
				(unsigned long long)assetModFlags,
				(void*)postApplyData, (void*)postApplyLayout,
				resolvedClass,
				(unsigned long long)hash);

		}

		// Patch pose-offset globals once so hull reads the pose region.
		PatchPlayerLayoutGlobals(entPtr);

		Warning(eDLL_T::SERVER,
			"[s21-bridge] built '%s' SettingsBlock for player "
			"(entPtr=%p data=%p size=%u site=%s) [memcpy-defaultData]\n",
			resolvedClass, (void*)entPtr, (void*)data, dataSize, siteTag);

		// Body model comes from SetPlayerSettingsWithMods, not C++ SetModel.
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] EnsurePlayerSettingsApplied SEH fault at %s\n",
			siteTag);
	}
}

void EnsurePlayerSettingsApplied(uint64_t entPtr, const char* siteTag)
{
	if (!sdk_bridge_preinstall_settings.GetBool()) return;
	if (!v_Pak_StringHash || !v_Pak_FindAssetVoid) return;
	if (!entPtr) return;

	g_settingsAppliedMutex.lock();
	bool isNew = g_settingsAppliedEntities.insert(entPtr).second;
	g_settingsAppliedMutex.unlock();
	if (!isNew) return;

	EnsurePlayerSettingsApplied_Inner(entPtr, siteTag);
}

//-----------------------------------------------------------------------------
// If slot 1 vtable at handle_table+0xB10 is out of module range, run the ctor.
//-----------------------------------------------------------------------------
void EnsureObserverModeTrackers()
{
	if (s_observerTrackersInstalled.load(std::memory_order_relaxed)) return;
	if (!v_CGlobalNonRewinding_ctor) return;

	__try
	{
		const uintptr_t base =
			static_cast<uintptr_t>(g_GameDll.GetModuleBase());
		const uintptr_t modEnd = base + static_cast<uintptr_t>(
			g_GameDll.GetModuleSize());
		const uint64_t mgrRoot =
			*reinterpret_cast<const uint64_t*>(base + 0xD4EC368);
		if (!mgrRoot) return;
		const uint64_t entTable =
			*reinterpret_cast<const uint64_t*>(mgrRoot + 0x78);
		if (!entTable) return;
		const uint64_t handleTable =
			*reinterpret_cast<const uint64_t*>(entTable + 0x3C448);
		if (!handleTable) return;

		const uint64_t preContainerVtbl =
			*reinterpret_cast<const uint64_t*>(handleTable);
		const uint64_t preSlot1Vtbl =
			*reinterpret_cast<const uint64_t*>(handleTable + 0xB10);
		const bool preInRange =
			(preSlot1Vtbl >= base && preSlot1Vtbl < modEnd);

		Warning(eDLL_T::SERVER,
			"[OBSV-INIT] pre  handle_table=0x%llX container_vtbl=0x%llX "
			"slot1_vtbl=0x%llX in_module=%d\n",
			(unsigned long long)handleTable,
			(unsigned long long)preContainerVtbl,
			(unsigned long long)preSlot1Vtbl,
			preInRange ? 1 : 0);

		if (preInRange)
		{
			// Engine's ctor already ran; slot 1 has a valid vtable in module
			// range. Trust the install and skip re-running the ctor (which
			// would reset CBaseEntity sub-object state).
			s_observerTrackersInstalled.store(true,
				std::memory_order_relaxed);
			Msg(eDLL_T::SERVER,
				"[OBSV-INIT] already initialized -- skipping ctor call\n");
			return;
		}

		// Slot 1 vtable is garbage. Run the engine's CGlobalNonRewinding ctor
		// to install &CObserverMode::vftable + handle=-1 in all 128 slots.
		v_CGlobalNonRewinding_ctor(reinterpret_cast<void*>(handleTable));

		const uint64_t postContainerVtbl =
			*reinterpret_cast<const uint64_t*>(handleTable);
		const uint64_t postSlot1Vtbl =
			*reinterpret_cast<const uint64_t*>(handleTable + 0xB10);
		const uint32_t postSlot1Handle =
			*reinterpret_cast<const uint32_t*>(handleTable + 0xB1C);
		const bool postInRange =
			(postSlot1Vtbl >= base && postSlot1Vtbl < modEnd);

		Warning(eDLL_T::SERVER,
			"[OBSV-INIT] post handle_table=0x%llX container_vtbl_rva=0x%llX "
			"slot1_vtbl_rva=0x%llX slot1_handle=0x%08X in_module=%d\n",
			(unsigned long long)handleTable,
			postContainerVtbl ?
				(unsigned long long)(postContainerVtbl - base) : 0ULL,
			postInRange ?
				(unsigned long long)(postSlot1Vtbl - base) : 0ULL,
			postSlot1Handle,
			postInRange ? 1 : 0);

		if (postInRange)
			s_observerTrackersInstalled.store(true,
				std::memory_order_relaxed);
		else
			Warning(eDLL_T::SERVER,
				"[OBSV-INIT] post-ctor slot1 vtable still out of module range "
				"-- the ctor call did not take effect; bridge will likely "
				"still AV at +0xD0\n");
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::SERVER,
			"[OBSV-INIT] EnsureObserverModeTrackers SEH code=%lu\n",
			GetExceptionCode());
	}
}

//-----------------------------------------------------------------------------
// NULL NaN/extreme-bbox slots around BuildEntityGrid, then restore.
//-----------------------------------------------------------------------------
static constexpr uint32_t kMaxNullify = 16;
struct NullifiedSlot { uint32_t slot; uint64_t savedPtr; };

static void Hook_CSpatialPartition_BuildEntityGrid(int64_t a1)
{
	NullifiedSlot nullified[kMaxNullify] = {};
	uint32_t numNullified = 0;
	uint64_t entTableForRestore = 0;

	bool didNullify = false;

	{
		const uintptr_t base =
			static_cast<uintptr_t>(g_GameDll.GetModuleBase());
		if (!base)
		{
			v_CSpatialPartition_BuildEntityGrid(a1);
			return;
		}

		const uint64_t mgr =
			*reinterpret_cast<const uint64_t*>(base + 0xD4EC388);
		const auto ptrOk = [](uint64_t p) -> bool {
			return p >= 0x10000ULL && p < 0x800000000000ULL;
		};
		if (!mgr || !ptrOk(mgr))
		{
			v_CSpatialPartition_BuildEntityGrid(a1);
			return;
		}
		const uint64_t mgrVtable =
			*reinterpret_cast<const uint64_t*>(mgr);
		if (!mgrVtable || !ptrOk(mgrVtable))
		{
			v_CSpatialPartition_BuildEntityGrid(a1);
			return;
		}
		using GetEntCount_t = uint32_t(__fastcall*)(uint64_t);
		const GetEntCount_t fn = reinterpret_cast<GetEntCount_t>(
			*reinterpret_cast<const uint64_t*>(mgrVtable + 0x160));
		if (!fn)
		{
			v_CSpatialPartition_BuildEntityGrid(a1);
			return;
		}
		const uint32_t maxCount = fn(mgr);

		const uint64_t structPtr =
			*reinterpret_cast<const uint64_t*>(base + 0xD4EC368);
		if (!structPtr || !ptrOk(structPtr))
		{
			v_CSpatialPartition_BuildEntityGrid(a1);
			return;
		}
		const uint64_t entTable =
			*reinterpret_cast<const uint64_t*>(structPtr + 0x78);
		if (!entTable || !ptrOk(entTable))
		{
			v_CSpatialPartition_BuildEntityGrid(a1);
			return;
		}
		entTableForRestore = entTable;

		// Layout first qword is a 0xFFFF... sentinel until Apply replaces it.
		{
			const uint64_t entPtr = *reinterpret_cast<uint64_t*>(
				entTable + 8ULL * 0 + 0x3C048);
			EnsurePlayerSettingsApplied(entPtr, "snapshot-pre");


		}


		for (uint32_t s = 0; s < maxCount && s < 65535; ++s)
		{
			if (s == 0xFFFE) continue;
			uint64_t* const slotPtr = reinterpret_cast<uint64_t*>(
				entTable + 8ULL * s + 0x3C048);
			const uint64_t entPtr = *slotPtr;
			if (!entPtr) continue;

			if (!ptrOk(entPtr))
				continue;
			const uint64_t entVtable =
				*reinterpret_cast<const uint64_t*>(entPtr);
			if (!entVtable || !ptrOk(entVtable))
			{
				if (numNullified < kMaxNullify)
				{
					nullified[numNullified].slot = s;
					nullified[numNullified].savedPtr = entPtr;
					++numNullified;
					*slotPtr = 0;
					didNullify = true;
				}
				continue;
			}
			using GetBbox_t =
				void(__fastcall*)(uint64_t self, float* outXyz);
			const uint64_t fnSlot =
				*reinterpret_cast<const uint64_t*>(entVtable + 0x4A8);
			if (!fnSlot)
				continue;
			const GetBbox_t getBbox = reinterpret_cast<GetBbox_t>(fnSlot);
			float bbox[3] = { 0, 0, 0 };
			getBbox(entPtr, bbox);

			const bool isNan = (bbox[0] != bbox[0])
				|| (bbox[1] != bbox[1]) || (bbox[2] != bbox[2]);
			const bool isExtreme =
				bbox[0] > 1e7f || bbox[0] < -1e7f ||
				bbox[1] > 1e7f || bbox[1] < -1e7f;

			if (!isNan && !isExtreme) continue;

			if (numNullified < kMaxNullify)
			{
				nullified[numNullified].slot = s;
				nullified[numNullified].savedPtr = entPtr;
				++numNullified;
				*slotPtr = 0;
				didNullify = true;
			}
		}

		if (didNullify)
		{
			Warning(eDLL_T::SERVER,
				"[s21-bridge] EXCLUDING %u NaN/extreme entities"
				"from spatial grid build; original will run with valid set.\n",
				numNullified);
		}
	}

	v_CSpatialPartition_BuildEntityGrid(a1);

	// Restore NULLed slots so other systems see the entities again.
	// nullified[] is bounds-capped; entTableForRestore set only on success path.
	if (entTableForRestore)
	{
		for (uint32_t i = 0; i < numNullified; ++i)
		{
			uint64_t* const slotPtr = reinterpret_cast<uint64_t*>(
				entTableForRestore + 8ULL * nullified[i].slot + 0x3C048);
			*slotPtr = nullified[i].savedPtr;
		}
	}
}

//-----------------------------------------------------------------------------
// Engine bail is !a1 || !*a1; (BYTE*)-1 still AVs.
//-----------------------------------------------------------------------------
static std::atomic<uint64_t> g_precacheGuardHits{0};

static int64_t Hook_Server_PrecacheModel(uint8_t* a1)
{
	// Engine's own check accepts NULL. Add the missing -1 case.
	if (a1 == reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(-1)))
	{
		const uint64_t n = ++g_precacheGuardHits;
		// Log first hit + every power of 10, so the log doesn't flood under
		// repeated calls but we know if this fires unexpectedly often.
		if (n == 1 || n == 10 || n == 100 || n == 1000 ||
			n == 10000 || n == 100000)
		{
			Warning(eDLL_T::SERVER,
				"[s21-bridge] Server_PrecacheModel called with (BYTE*)-1 "
				"(uninitialized class-settings field); returning -1 like the "
				"NULL bail. n=%llu\n", (unsigned long long)n);
		}
		return 0xFFFFFFFFLL;
	}
	const int64_t result = v_Server_PrecacheModel(a1);

	// S3 PrecacheModel is server-local only; S21 also replicates into
	// modelprecache so the client can resolve GetModelIndex / SetModel.
	// Mirror that here for every successful script-side PrecacheModel.
	if (a1 && *a1 && result != 0xFFFFFFFFLL)
	{
		PrecacheModel_RegisterReplicated(reinterpret_cast<const char*>(a1));
	}
	return result;
}

int64_t Server_PrecacheModel_Invoke(const char* modelName)
{
	if (!modelName || !*modelName) return 0xFFFFFFFFLL;
	if (!v_Server_PrecacheModel)   return 0xFFFFFFFFLL;
	// Cast away const to match the engine's signature (uint8_t*); engine
	// never writes through this pointer.
	return v_Server_PrecacheModel(reinterpret_cast<uint8_t*>(
		const_cast<char*>(modelName)));
}

void VPrecacheModelGuard::GetFun() const
{
	// Prologue + bail-check + vtable[8] dispatch into precache manager.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B D9 48 85 C9 74 ?? 80 39 00 74 ?? "
		"48 8B 0D ?? ?? ?? ?? 48 8B D3 48 8B 01 FF 50 40 84 C0")
		.GetPtr(v_Server_PrecacheModel);
	if (!v_Server_PrecacheModel)
		Warning(eDLL_T::SERVER,
			"[s21-bridge] Server_PrecacheModel pattern unresolved -- "
			"-1 guard NOT active. Bridge OnPlayerChangedTeam may crash.\n");
}

void VPrecacheModelGuard::Detour(const bool bAttach) const
{
	if (!v_Server_PrecacheModel)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] Server_PrecacheModel pattern unresolved -- "
			"-1 guard NOT active. Bridge OnPlayerChangedTeam may crash.\n");
		return;
	}
	const LONG result = bAttach
		? DetourAttach(reinterpret_cast<void**>(&v_Server_PrecacheModel),
			reinterpret_cast<void*>(&Hook_Server_PrecacheModel))
		: DetourDetach(reinterpret_cast<void**>(&v_Server_PrecacheModel),
			reinterpret_cast<void*>(&Hook_Server_PrecacheModel));
	if (bAttach)
	{
		Msg(eDLL_T::SERVER,
			"[s21-bridge] DetourAttach Server_PrecacheModel result=0x%lX "
			"(target=0x%p)\n", result, (void*)v_Server_PrecacheModel);
	}
}


void VSnapshotDiag::GetFun() const
{
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 56 57 48 8D 68 A1 48 81 EC C8 00 00 00 "
		"44 0F 29 48")
		.GetPtr(v_CSpatialPartition_BuildEntityGrid);
	if (!v_CSpatialPartition_BuildEntityGrid)
		Warning(eDLL_T::SERVER,
			"[s21-bridge] pattern unresolved -- spatial grid"
			"safety cap NOT active. Bridge will leak when player has NaN bbox.\n");



	// PlayerClassSettings_Init (server) -- unique sig.
	// From it we derive Settings_ApplySettings_Server and the
	// spectator GUID global.
	const CMemory pcInit = Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 57 48 83 EC 20 48 8D 3D ?? ?? ?? ?? "
		"48 8D 05 ?? ?? ?? ?? 40 F6 C7 03 48 8D 1D");
	if (pcInit)
	{
		// +0x4A: mov [rip+disp32], rax -- stores spectator GUID
		g_pSpectatorGuid = CMemory(pcInit.GetPtr() + 0x4A)
			.ResolveRelativeAddress(0x3, 0x7)
			.RCast<uint64_t*>();

		// +0x5E: call rel32 Settings_ApplySettings_Server
		v_Settings_ApplySettings_Server = CMemory(pcInit.GetPtr() + 0x5E)
			.FollowNearCallSelf()
			.RCast<ApplySettings_t>();

		// PlayerClassSettings_Init itself -- callable so we can invoke it
		// when the bridge bypasses CServer::SpawnServer.
		v_PlayerClassSettings_Init =
			pcInit.RCast<PlayerClassSettingsInit_t>();

		// +0x1C: lea rbx, [rip+disp32] Pak_StringHash.
		v_Pak_StringHash = CMemory(pcInit.GetPtr() + 0x1C)
			.ResolveRelativeAddress(0x3, 0x7)
			.RCast<HashFn_t>();

		// +0x34: call rel32 Pak_FindAssetVoid.
		v_Pak_FindAssetVoid = CMemory(pcInit.GetPtr() + 0x34)
			.FollowNearCallSelf()
			.RCast<LookupFn_t>();

	}

	WeaponSelectMirror_GetFun();

	// Patch data[0] before zero-offset field reads AV.
	Module_FindPattern(g_GameDll,
		"40 56 57 48 83 EC 28 48 8B F2 48 8B F9 48 3B 51 10 "
		"75 ?? 45 84 C0 0F 84")
		.GetPtr(v_ApplySettingsModifiers);

	if (!v_ApplySettingsModifiers)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] ApplySettingsModifiers pattern unresolved -- "
			"PCS_Init Apply will crash on stlt-rescued zero-offset fields\n");
	}

	// SettingsLayout::LookupField prologue.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 57 44 0F B6 0A 45 33 C0 48 8B FA 4C 8B D2 45 84 C9")
		.GetPtr(v_SettingsLayout_LookupField);
	if (!v_SettingsLayout_LookupField)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] SettingsLayout_LookupField pattern unresolved -- "
			"POSE-FIX will be skipped; hull falls back to Layer B "
			"(NaN-substitute at SetCollisionBounds).\n");
	}

	// CGlobalNonRewinding ctor: CObserverMode trackers at +0xB10+16*N.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 33 D2 48 8B D9 E8 ?? ?? ?? ?? "
		"48 8D 05 ?? ?? ?? ?? 48 89 03 48 8D 05 ?? ?? ?? ?? "
		"48 89 83 10 0B 00 00")
		.GetPtr(v_CGlobalNonRewinding_ctor);
	if (!v_CGlobalNonRewinding_ctor)
		Warning(eDLL_T::SERVER,
			"[s21-bridge] CGlobalNonRewinding ctor pattern unresolved -- "
			"EnsureObserverModeTrackers will be a no-op; bridge will AV at "
			"+0xD0 during DecideRespawnPlayer\n");

	// FindStringIndex: ExtraParticleFilesTable is NULL on dedi.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 30 48 8B F9 48 C7 44 24 20 "
		"00 00 00 00 48 8B 0D ?? ?? ?? ?? 41 B9 FF FF FF FF "
		"4C 8B C7 33 D2 48 8B 01 FF 50 40")
		.GetPtr(v_FindStringIndex);
	if (v_FindStringIndex)
		Msg(eDLL_T::SERVER,
			"[FSI-NULL] resolved: %p"
			"(unblocks engine apply pipeline on dedi)\n",
			(void*)v_FindStringIndex);
	else
		Warning(eDLL_T::SERVER,
			"[FSI-NULL] pattern unresolved -- engine"
			"apply will still AV on dedi when called from bridge.\n");

}

void VSnapshotDiag::Detour(const bool bAttach) const
{
	if (!v_CSpatialPartition_BuildEntityGrid)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] pattern unresolved -- spatial grid"
			"safety cap NOT active. Bridge will leak when player has NaN bbox.\n");
		return;
	}
	const LONG result = bAttach
		? DetourAttach(reinterpret_cast<void**>(&v_CSpatialPartition_BuildEntityGrid),
			reinterpret_cast<void*>(&Hook_CSpatialPartition_BuildEntityGrid))
		: DetourDetach(reinterpret_cast<void**>(&v_CSpatialPartition_BuildEntityGrid),
			reinterpret_cast<void*>(&Hook_CSpatialPartition_BuildEntityGrid));
	if (bAttach)
	{
		Msg(eDLL_T::SERVER,
			"[s21-bridge] DetourAttach result=0x%lX (target=0x%p)\n",
			result, (void*)v_CSpatialPartition_BuildEntityGrid);
	}

	// NOP m_playerFlags bit-2 SET; leave the dirty-mark before it.
	if (bAttach && sdk_bridge_suppress_connflag.GetBool())
	{
		static bool s_connFlagPatched = false;
		if (!s_connFlagPatched)
		{

			// (F0 44 09 44 51 40) immediately followed by the bit-2 set
			// `or dword [rbx+6128h], 2` (83 8B 28 61 00 00 02). NOP only the
			// 7-byte OR at +6.
			CMemory hit = Module_FindPattern(g_GameDll,
				"F0 44 09 44 51 40 83 8B 28 61 00 00 02");
			if (hit)
			{
				uint8_t* pOr = hit.Offset(6).RCast<uint8_t*>();
				if (pOr[0] == 0x83 && pOr[1] == 0x8B && pOr[2] == 0x28 &&
					pOr[3] == 0x61 && pOr[4] == 0x00 && pOr[5] == 0x00 &&
					pOr[6] == 0x02)
				{
					DWORD oldProt = 0;
					if (VirtualProtect(pOr, 7, PAGE_EXECUTE_READWRITE, &oldProt))
					{
						memset(pOr, 0x90, 7); // 7x NOP
						VirtualProtect(pOr, 7, oldProt, &oldProt);
						FlushInstructionCache(GetCurrentProcess(), pOr, 7);
						s_connFlagPatched = true;
						Msg(eDLL_T::SERVER,
							"[CONNFLAG-SUPPRESS] NOP'd m_playerFlags bit-2 SET @ %p "
							"-- bridged players will read as connected on the S21 client.\n",
							(void*)pOr);
					}
					else
						Warning(eDLL_T::SERVER,
							"[CONNFLAG-SUPPRESS] VirtualProtect failed @ %p (gle=%lu)\n",
							(void*)pOr, GetLastError());
				}
				else
					Warning(eDLL_T::SERVER,
						"[CONNFLAG-SUPPRESS] site byte mismatch @ %p "
						"(%02X %02X %02X %02X %02X %02X %02X) -- not patching.\n",
						(void*)pOr, pOr[0], pOr[1], pOr[2], pOr[3],
						pOr[4], pOr[5], pOr[6]);
			}
			else
				Warning(eDLL_T::SERVER,
					"[CONNFLAG-SUPPRESS] conn-writer bit-2 SET pattern not found "
					"-- false disconnected icon NOT suppressed.\n");
		}
	}

	// Grow warm pool immediate 0x4000 -> 0x40000. Two of four backings stay fixed.
	if (bAttach && sdk_snap_grow_propcap.GetBool())
	{
		static bool s_propCapPatched = false;
		if (!s_propCapPatched)
		{

			// followed by `lea rcx, [rbx+1085C0h]` (the +1082816 pool base),
			// `mov r8d, r9d`, `mov edx, 1`, then the near-call.
			CMemory capHit = Module_FindPattern(g_GameDll,
				"41 B9 00 40 00 00 48 8D 8B C0 85 10 00 45 8B C1 BA 01 00 00 00 E8");
			if (capHit)
			{
				uint8_t* pImm = capHit.Offset(2).RCast<uint8_t*>(); // imm32 of mov r9d
				if (pImm[0] == 0x00 && pImm[1] == 0x40 &&
					pImm[2] == 0x00 && pImm[3] == 0x00)
				{
					DWORD oldProt = 0;
					if (VirtualProtect(pImm, 4, PAGE_EXECUTE_READWRITE, &oldProt))
					{
						*reinterpret_cast<uint32_t*>(pImm) = 0x40000u; // 262144
						VirtualProtect(pImm, 4, oldProt, &oldProt);
						FlushInstructionCache(GetCurrentProcess(), pImm, 4);
						s_propCapPatched = true;
						Msg(eDLL_T::SERVER,
							"[SNAP-PROPCAP] grew snapshot prop/data entry backing "
							"16384 -> 262144 @ %p -- full-snapshots stay inside "
							"sv_max_props / sv_max_prop_data_dwords.\n",
							(void*)pImm);
					}
					else
						Warning(eDLL_T::SERVER,
							"[SNAP-PROPCAP] VirtualProtect failed @ %p (gle=%lu)\n",
							(void*)pImm, GetLastError());
				}
				else
					Warning(eDLL_T::SERVER,
						"[SNAP-PROPCAP] immediate mismatch @ %p "
						"(%02X %02X %02X %02X) -- not patching.\n",
						(void*)pImm, pImm[0], pImm[1], pImm[2], pImm[3]);
			}
			else
				Warning(eDLL_T::SERVER,
					"[SNAP-PROPCAP] 0x4000 cap site not found --"
					"canonical snapshot overflow NOT mitigated.\n");
		}
	}

	if (v_ApplySettingsModifiers)
	{
		const LONG r2 = bAttach
			? DetourAttach(reinterpret_cast<void**>(&v_ApplySettingsModifiers),
				reinterpret_cast<void*>(&Hook_ApplySettingsModifiers))
			: DetourDetach(reinterpret_cast<void**>(&v_ApplySettingsModifiers),
				reinterpret_cast<void*>(&Hook_ApplySettingsModifiers));
		if (bAttach)
		{
			Msg(eDLL_T::SERVER,
				"[s21-bridge] DetourAttach ApplySettingsModifiers result=0x%lX "
				"(target=0x%p)\n", r2, (void*)v_ApplySettingsModifiers);
		}
	}
	if (v_FindStringIndex)
	{
		const LONG rFsi = bAttach
			? DetourAttach(reinterpret_cast<void**>(&v_FindStringIndex),
				reinterpret_cast<void*>(&Hook_FindStringIndex))
			: DetourDetach(reinterpret_cast<void**>(&v_FindStringIndex),
				reinterpret_cast<void*>(&Hook_FindStringIndex));
		if (bAttach)
			Msg(eDLL_T::SERVER,
				"[FSI-NULL] DetourAttach result=0x%lX"
				"(target=0x%p) -- engine apply pipeline now safe "
				"to invoke from bridge.\n",
				rFsi, (void*)v_FindStringIndex);
	}
	WeaponSelectMirror_Detour(bAttach);

	(void)bAttach;
}
//-----------------------------------------------------------------------------
// Apply spectator SettingsBlock before PlayerAnimUpdate reads a -1 sentinel.
//-----------------------------------------------------------------------------
struct SettingsSweepLatchEntry
{
	uint64_t blockPtr;
	uint32_t blockSize;
	uint8_t cleanPasses;
};
static SettingsSweepLatchEntry s_settingsSweepLatch[2048] = {};

static int64_t Hook_PlayerAnimUpdate(int64_t player)
{
	EnsurePlayerSettingsApplied(static_cast<uint64_t>(player), "anim-pre");

	// S21 string inherit sentinel is (BYTE*)-1. Skip if size is outside [16, kSettingsBlockCap].
	if (player)
	{
		const uintptr_t serverClass = *reinterpret_cast<uintptr_t*>(player + 0x50);
		if (serverClass)
		{
			const char* const className = *reinterpret_cast<const char**>(serverClass + 0x00);
			if (className && _stricmp(className, "CPlayer") == 0)
			{
				const uint64_t entPtr = static_cast<uint64_t>(player);
				const uint64_t dataPtr = *reinterpret_cast<uint64_t*>(entPtr + 24328);
				const uint64_t hdr = *reinterpret_cast<uint64_t*>(entPtr + 24336);

				if (dataPtr && hdr)
				{
					const uint32_t dataSize = *reinterpret_cast<uint32_t*>(hdr + 0x38);

					if (dataSize >= 16 && dataSize <= kSettingsBlockCap)
					{
						const uint16_t edict = *reinterpret_cast<uint16_t*>(entPtr + 0x58);
						const bool bLatch = sdk_settings_sweep_latch.GetBool()
							&& edict < 2048u;
						SettingsSweepLatchEntry* latch = bLatch
							? &s_settingsSweepLatch[edict] : nullptr;

						bool bSkipSweep = false;
						if (latch)
						{
							if (latch->blockPtr == dataPtr
								&& latch->blockSize == dataSize
								&& latch->cleanPasses >= 2)
							{
								bSkipSweep = true;
							}
							else if (latch->blockPtr != dataPtr
								|| latch->blockSize != dataSize)
							{
								latch->blockPtr = dataPtr;
								latch->blockSize = dataSize;
								latch->cleanPasses = 0;
							}
						}

						if (!bSkipSweep)
						{
							bool repaired = false;
							for (uint32_t off = 0; off + 8 <= dataSize; off += 8)
							{
								uint64_t* slot = reinterpret_cast<uint64_t*>(dataPtr + off);
								if (*slot == 0xFFFFFFFFFFFFFFFFull)
								{
									*slot = 0;
									repaired = true;
								}
							}
							if (*reinterpret_cast<uint64_t*>(dataPtr) == 0)
							{
								*reinterpret_cast<uint64_t*>(dataPtr) =
									reinterpret_cast<uint64_t>(s_globalEmptyStr);
								repaired = true;
							}

							if (latch)
							{
								if (repaired)
									latch->cleanPasses = 0;
								else if (latch->cleanPasses < 255)
									++latch->cleanPasses;
							}
						}
					}
				}
			}
		}
	}

	return v_PlayerAnimUpdate(player);
}

void VPlayerAnimUpdateGuard::GetFun() const
{
	// 30-byte sig including the vtable[62] dispatch is unambiguous.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 30 48 8B D9 48 8B 0D ?? ?? ?? ?? "
		"0F BF 53 58 48 8B 01 FF CA FF 90 F0 01 00 00")
		.GetPtr(v_PlayerAnimUpdate);
	if (!v_PlayerAnimUpdate)
		Warning(eDLL_T::SERVER,
			"[s21-bridge] PlayerAnimUpdate pattern unresolved -- pre-snapshot "
			"-1 sentinel guard NOT active; bridge OnPlayerChangedTeam will AV.\n");
}

void VPlayerAnimUpdateGuard::Detour(const bool bAttach) const
{
	if (!v_PlayerAnimUpdate)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] PlayerAnimUpdate pattern unresolved -- pre-snapshot "
			"-1 sentinel guard NOT active; bridge OnPlayerChangedTeam will AV.\n");
		return;
	}
	const LONG result = bAttach
		? DetourAttach(reinterpret_cast<void**>(&v_PlayerAnimUpdate),
			reinterpret_cast<void*>(&Hook_PlayerAnimUpdate))
		: DetourDetach(reinterpret_cast<void**>(&v_PlayerAnimUpdate),
			reinterpret_cast<void*>(&Hook_PlayerAnimUpdate));
	if (bAttach)
	{
		Msg(eDLL_T::SERVER,
			"[s21-bridge] DetourAttach PlayerAnimUpdate result=0x%lX "
			"(target=0x%p)\n", result, (void*)v_PlayerAnimUpdate);
	}
}


// Cached ServerClass id + flat prop count; retries until AssignClassIds.
S21Bridge_ClassMeta S21Bridge_LookupClassMeta(const char* className)
{
	if (!className || !className[0])
		return {-1, 0};

	// Small per-name cache (only a handful of classes ever need sanitizing).
	struct Entry { const char* name; S21Bridge_ClassMeta meta; };
	static Entry s_cache[16] = {};
	static int s_cacheSize = 0;
	static std::mutex s_cacheMutex;

	{
		std::lock_guard<std::mutex> lk(s_cacheMutex);
		for (int i = 0; i < s_cacheSize; ++i)
		{
			if (s_cache[i].name == className ||
				(s_cache[i].name && strcmp(s_cache[i].name, className) == 0))
				return s_cache[i].meta;
		}
	}

	const uintptr_t base = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!base)
		return {-1, 0};

	const int nClasses = S3_RdD(base + kS3_RVA_NumServerClasses);
	if (nClasses <= 0 || nClasses > 1024)
		return {-1, 0};

	const uintptr_t byId = base + kS3_RVA_ServerClassesByID;
	S21Bridge_ClassMeta meta{-1, 0};

	for (int i = 0; i < nClasses; ++i)
	{
		const uintptr_t sc = S3_RdQ(byId + static_cast<uintptr_t>(i) * 8);
		if (!sc || !S3_PtrLooksHeap(sc))
			continue;

		const uintptr_t namePtr = S3_RdQ(sc + kS3_SC_NetworkName);
		if (!namePtr || !S3_PtrLooksHeap(namePtr))
			continue;

		char nameBuf[96] = {};
		S3_RdStr(namePtr, nameBuf, sizeof(nameBuf));
		if (strcmp(nameBuf, className) != 0)
			continue;

		meta.classId = S3_RdD(sc + kS3_SC_ClassID);

		const uintptr_t st = S3_RdQ(sc + kS3_SC_SendTable);
		if (st && S3_PtrLooksHeap(st))
		{
			const uintptr_t pc = S3_RdQ(st + kS3_ST_Precalc);
			if (pc && S3_PtrLooksHeap(pc))
			{
				const int n = S3_RdD(pc + kS3_PC_FlatCount);
				if (n > 0 && n < 8192)
					meta.propCount = static_cast<uint16_t>(n);
			}
		}
		break;
	}

	// Only cache successful lookups so transient pre-AssignClassIds failures
	// can be retried.
	if (meta.classId >= 0 && meta.propCount > 0)
	{
		std::lock_guard<std::mutex> lk(s_cacheMutex);
		if (s_cacheSize < (int)(sizeof(s_cache) / sizeof(s_cache[0])))
		{
			s_cache[s_cacheSize].name = className;
			s_cache[s_cacheSize].meta = meta;
			++s_cacheSize;
		}

		static std::atomic<uint32_t> s_loggedOk{0};
		if (s_loggedOk.fetch_add(1) < 16)
		{
			Msg(eDLL_T::SERVER,
				"[S21Bridge] LookupClassMeta('%s'): classID=%d propCount=%u\n",
				className, meta.classId, (unsigned)meta.propCount);
		}
	}
	else
	{
		static std::atomic<uint32_t> s_loggedFail{0};
		if (s_loggedFail.fetch_add(1) < 4)
		{
			Warning(eDLL_T::SERVER,
				"[S21Bridge] LookupClassMeta('%s'): not yet resolved "
				"(classID=%d propCount=%u) -- will retry\n",
				className, meta.classId, (unsigned)meta.propCount);
		}
	}

	return meta;
}

//-----------------------------------------------------------------------------
// Engine NULL-checks classActivityModifier; -1 still AVs. Cave: cmp rbx,-1.
//-----------------------------------------------------------------------------
void VSettingsStringGuard::GetFun() const
{
	CMemory match = Module_FindPattern(g_GameDll,
		"48 8B 1C 18 48 85 DB 74 ?? 44 38 23 74");
	if (match)
		s_settingsStrPatchSite = reinterpret_cast<uint8_t*>(match.GetPtr() + 4);
	else
		Warning(eDLL_T::SERVER,
			"[s21-bridge] settings string guard pattern unresolved -- "
			"classActivityModifier -1 crash NOT guarded.\n");
}

void VSettingsStringGuard::Detour(const bool bAttach) const
{
	if (!s_settingsStrPatchSite)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] settings string guard pattern unresolved -- "
			"classActivityModifier -1 crash NOT guarded.\n");
		return;
	}
	if (!bAttach) return;

	uint8_t* pSite = s_settingsStrPatchSite;
	if (pSite[0] != 0x48 || pSite[1] != 0x85 || pSite[2] != 0xDB
		|| pSite[3] != 0x74 || pSite[5] != 0x44 || pSite[6] != 0x38
		|| pSite[7] != 0x23 || pSite[8] != 0x74)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] settings string guard byte mismatch at %p\n", pSite);
		return;
	}

	const uintptr_t siteAddr = (uintptr_t)pSite;
	const uintptr_t skipTarget = siteAddr + 5 + (int8_t)pSite[4];
	const uintptr_t continueTarget = siteAddr + 10;

	void* cave = nullptr;
	for (uintptr_t probe = (siteAddr - 0x08000000) & ~0xFFFFULL;
		 probe < siteAddr + 0x08000000;
		 probe += 0x10000)
	{
		cave = VirtualAlloc((void*)probe, 64, MEM_COMMIT | MEM_RESERVE,
			PAGE_EXECUTE_READWRITE);
		if (cave) break;
	}
	if (!cave)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] could not allocate cave for settings string guard\n");
		return;
	}

	uint8_t* c = reinterpret_cast<uint8_t*>(cave);
	int pos = 0;

	// test rbx, rbx
	c[pos++] = 0x48; c[pos++] = 0x85; c[pos++] = 0xDB;
	// jz.skip
	c[pos++] = 0x74;
	int jz1Off = pos++;
	// cmp rbx, -1
	c[pos++] = 0x48; c[pos++] = 0x83; c[pos++] = 0xFB; c[pos++] = 0xFF;
	// jz.skip
	c[pos++] = 0x74;
	int jz2Off = pos++;
	// cmp [rbx], r12b
	c[pos++] = 0x44; c[pos++] = 0x38; c[pos++] = 0x23;
	// jz.skip
	c[pos++] = 0x74;
	int jz3Off = pos++;
	// jmp continueTarget
	c[pos++] = 0xE9;
	int32_t contRel = (int32_t)(continueTarget - ((uintptr_t)c + pos + 4));
	memcpy(c + pos, &contRel, 4); pos += 4;

	//.skip: jmp skipTarget
	int skipPos = pos;
	c[pos++] = 0xE9;
	int32_t skipRel = (int32_t)(skipTarget - ((uintptr_t)c + pos + 4));
	memcpy(c + pos, &skipRel, 4); pos += 4;

	c[jz1Off] = (uint8_t)(skipPos - jz1Off - 1);
	c[jz2Off] = (uint8_t)(skipPos - jz2Off - 1);
	c[jz3Off] = (uint8_t)(skipPos - jz3Off - 1);

	DWORD oldProt;
	VirtualProtect(pSite, 10, PAGE_EXECUTE_READWRITE, &oldProt);
	pSite[0] = 0xE9;
	int32_t caveRel = (int32_t)((uintptr_t)cave - (siteAddr + 5));
	memcpy(pSite + 1, &caveRel, 4);
	memset(pSite + 5, 0x90, 5);
	VirtualProtect(pSite, 10, oldProt, &oldProt);

	Msg(eDLL_T::SERVER,
		"[s21-bridge] installed settings string -1 guard at %p via cave %p\n",
		pSite, cave);
}

static ConVar sdk_skip_crasher_entities("sdk_skip_crasher_entities", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Block s_pszMapSkipClasses at map LUMP_ENTITIES parse. Canonical DT rebuild "
	"retires classes as cleared; player_vehicle remains. Set 0 to unblock all.");

// Classes whose S21 recv-table dropped props vs the S3 SendTable.
static const char* const s_pszMapSkipClasses[] =
{
	// Remaining wire-desync class names only.
	"player_vehicle",
};

static bool MapSkip_IsBlockedClass(const char* const pszClassName)
{
	for (const char* const pszBlocked : s_pszMapSkipClasses)
	{
		if (strcmp(pszClassName, pszBlocked) == 0)
			return true;
	}
	return false;
}

// Read-only classname peek; pStream is just past '{'.
static bool MapSkip_PeekClassName(const char* pStream, char* const pszOut, const size_t nOutLen)
{
	pszOut[0] = '\0';

	int nSafety = 0;
	while (*pStream && nSafety++ < 8192)
	{
		while (*pStream && static_cast<unsigned char>(*pStream) <= ' ')
			++pStream;                       // skip whitespace
		if (*pStream == '}' || !*pStream)
			break;
		if (*pStream != '"')                 // malformed -- bail
			break;

		++pStream;                           // key token
		const char* const pszKey = pStream;
		while (*pStream && *pStream != '"')
			++pStream;
		const size_t nKeyLen = static_cast<size_t>(pStream - pszKey);
		if (*pStream == '"')
			++pStream;

		while (*pStream && static_cast<unsigned char>(*pStream) <= ' ')
			++pStream;                       // skip whitespace before value
		if (*pStream != '"')                 // key without value -- bail
			break;

		++pStream;                           // value token
		const char* const pszVal = pStream;
		while (*pStream && *pStream != '"')
			++pStream;
		const size_t nValLen = static_cast<size_t>(pStream - pszVal);
		if (*pStream == '"')
			++pStream;

		if (nKeyLen == 9 && strncmp(pszKey, "classname", 9) == 0)
		{
			const size_t n = (nValLen < nOutLen - 1) ? nValLen : (nOutLen - 1);
			memcpy(pszOut, pszVal, n);
			pszOut[n] = '\0';
			return true;
		}
	}
	return false;
}

static int64_t __fastcall Hook_MapEntity_ParseEntity(void** a1, int64_t a2, int64_t a3)
{
	if (sdk_skip_crasher_entities.GetBool() && a2)
	{
		char szClassName[128];
		if (MapSkip_PeekClassName(reinterpret_cast<const char*>(a2),
				szClassName, sizeof(szClassName))
			&& MapSkip_IsBlockedClass(szClassName))
		{
			// Return the stream unchanged so the brace-skipper stays in sync.
			if (a1)
				*a1 = nullptr;

			static int s_nLog = 0;
			if (++s_nLog <= 64 || (s_nLog % 256) == 0)
				Msg(eDLL_T::SERVER,
					"[MAP-SKIP] #%d refused LUMP_ENTITIES '%s' (crasher class)\n",
					s_nLog, szClassName);

			return a2;
		}
	}
	return v_MapEntity_ParseEntity(a1, a2, a3);
}

void VMapEntitySkipper::GetFun(void) const
{
	// MapEntity_ParseEntity: unique after __chkstk; stack disps wildcarded.

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 54 41 55 41 56 41 57 "
		"48 8D AC 24 ?? ?? FF FF B8 ?? ?? 00 00 E8 ?? ?? ?? ?? 48 2B E0 45 33 E4 "
		"48 89 54 24 ?? C7 44 24 ?? FF FF FF FF 49 8B F0 48 89 54 24 ?? 4C 8B FA "
		"4C 8B E9 4C 8B F2 48 85 D2")
		.GetPtr(v_MapEntity_ParseEntity);

	if (!v_MapEntity_ParseEntity)
		Warning(eDLL_T::SERVER,
			"[MAP-SKIP] MapEntity_ParseEntity pattern unresolved -- crasher "
			"classes baked into LUMP_ENTITIES will spawn\n");
}

void VMapEntitySkipper::Detour(const bool bAttach) const
{
	if (v_MapEntity_ParseEntity)
		DetourSetup(&v_MapEntity_ParseEntity, &Hook_MapEntity_ParseEntity, bAttach);
}

//-----------------------------------------------------------------------------
// [GIB-GUARD] Fail-closed HasGibModel (slot 97) + gib spawn early-out.
//-----------------------------------------------------------------------------
static std::atomic<uint32_t> s_gibGuardWarnCount{0};
static std::atomic<uint32_t> s_gibSpawnWarnCount{0};

static void GibGuard_WarnLimited(std::atomic<uint32_t>& counter, const char* const pszReason)
{
	const uint32_t n = counter.fetch_add(1, std::memory_order_relaxed);
	if (n >= 4)
		return;
	Warning(eDLL_T::SERVER, "[GIB-GUARD] %s\n", pszReason);
}

// gibModels big-finder offset @ +0x1C and its val leaf @ +0x00.
static constexpr uintptr_t kGibModelsOffRva = 0x23884FC;
static constexpr uintptr_t kGibModelsLeafRva = 0x2387D90;
static constexpr uintptr_t kGibSoftenedOffRva = 0x238848C;
static constexpr uintptr_t kGibSoftenedLeafRva = 0x2387D48;

static bool HasGibModel_Impl(const int64_t entity, const ptrdiff_t blockOff)
{
	if (!entity)
	{
		GibGuard_WarnLimited(s_gibGuardWarnCount, "HasGibModel refused -- null entity");
		return false;
	}

	const uint64_t data = *reinterpret_cast<const uint64_t*>(entity + blockOff);
	if (!data || !Mem_IsReadableCached(reinterpret_cast<const void*>(data), 8))
	{
		GibGuard_WarnLimited(s_gibGuardWarnCount,
			"HasGibModel refused -- settings block null/unreadable");
		return false;
	}

	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!mod)
	{
		GibGuard_WarnLimited(s_gibGuardWarnCount, "HasGibModel refused -- null module");
		return false;
	}

	const uint32_t A = *reinterpret_cast<const uint32_t*>(mod + kGibModelsOffRva);
	const uint32_t B = *reinterpret_cast<const uint32_t*>(mod + kGibModelsLeafRva);
	if (A == 0xFFFFFFFFu || B == 0xFFFFFFFFu)
	{
		GibGuard_WarnLimited(s_gibGuardWarnCount,
			"HasGibModel refused -- finder offset sentinel");
		return false;
	}

	uint32_t base = A;
	if (A & 0x80000000u)
	{
		const uint8_t* const pHdr =
			reinterpret_cast<const uint8_t*>(data) + (A & 0x7FFFFFFFu) + 4;
		if (!Mem_IsReadableCached(pHdr, 4))
		{
			GibGuard_WarnLimited(s_gibGuardWarnCount,
				"HasGibModel refused -- indirect header unreadable");
			return false;
		}
		base = *reinterpret_cast<const uint32_t*>(pHdr);
	}

	const uint8_t* const pSlot =
		reinterpret_cast<const uint8_t*>(data) + static_cast<uint64_t>(base) + B;
	if (!Mem_IsReadableCached(pSlot, 8))
	{
		GibGuard_WarnLimited(s_gibGuardWarnCount,
			"HasGibModel refused -- string slot unreadable");
		return false;
	}

	char* const psz = *reinterpret_cast<char* const*>(
		const_cast<uint8_t*>(pSlot));
	if (!psz || !Mem_IsReadableCached(psz, 1))
	{
		GibGuard_WarnLimited(s_gibGuardWarnCount,
			"HasGibModel refused -- string null/unreadable");
		return false;
	}

	return psz[0] != 0;
}

static bool __fastcall Hook_HasGibModel(int64_t entity)
{
	if (!sdk_gib_finder_guard.GetBool())
		return v_HasGibModel(entity);
	return HasGibModel_Impl(entity, 0x5F08); // player settings block
}

static bool __fastcall Hook_HasGibModel_Twin(int64_t entity)
{
	if (!sdk_gib_finder_guard.GetBool())
		return v_HasGibModel_Twin(entity);
	return HasGibModel_Impl(entity, 0x2240); // twin-class settings block
}

static bool GibSpawn_FindersUnresolved(void)
{
	if (!s_gibModelsResolved.load(std::memory_order_acquire) ||
		!s_gibModelsSoftenedResolved.load(std::memory_order_acquire))
		return true;

	const uintptr_t mod = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (!mod)
		return true;

	const uint32_t offs[4] =
	{
		*reinterpret_cast<const uint32_t*>(mod + kGibModelsOffRva),
		*reinterpret_cast<const uint32_t*>(mod + kGibModelsLeafRva),
		*reinterpret_cast<const uint32_t*>(mod + kGibSoftenedOffRva),
		*reinterpret_cast<const uint32_t*>(mod + kGibSoftenedLeafRva),
	};
	for (const uint32_t off : offs)
	{
		if (off == 0xFFFFFFFFu)
			return true;
	}
	return false;
}

static void __fastcall Hook_GibSpawn(int64_t thisptr)
{
	if (sdk_gib_spawn_guard.GetBool() && GibSpawn_FindersUnresolved())
	{
		GibGuard_WarnLimited(s_gibSpawnWarnCount,
			"gib spawn skipped -- finders unresolved");
		return;
	}
	v_GibSpawn(thisptr);
}

void VGibFinderGuard::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 8B 81 08 5F 00 00 8B 0D ?? ?? ?? ?? 85 C9 79 0A 0F BA F1 1F 8B 54 01 04 "
		"EB 03 48 8B D1 8B 0D ?? ?? ?? ?? 48 03 CA 48 8B 0C 01 80 39 00 0F 95 C0 C3")
		.GetPtr(v_HasGibModel);
	if (!v_HasGibModel)
		Warning(eDLL_T::SERVER,
			"[GIB-GUARD] HasGibModel (player) pattern unresolved -- "
			"fail-closed predicate NOT active for +0x5F08\n");

	Module_FindPattern(g_GameDll,
		"48 8B 81 40 22 00 00 8B 0D ?? ?? ?? ?? 85 C9 79 0A 0F BA F1 1F 8B 54 01 04 "
		"EB 03 48 8B D1 8B 0D ?? ?? ?? ?? 48 03 CA 48 8B 0C 01 80 39 00 0F 95 C0 C3")
		.GetPtr(v_HasGibModel_Twin);
	if (!v_HasGibModel_Twin)
		Warning(eDLL_T::SERVER,
			"[GIB-GUARD] HasGibModel (twin) pattern unresolved -- "
			"fail-closed predicate NOT active for +0x2240\n");

	Module_FindPattern(g_GameDll,
		"4C 8B DC 55 41 55 49 8D AB 08 FF FF FF 48 81 EC E8 01 00 00 48 8B 05 ?? ?? ?? ?? "
		"4C 8B E9 41 0F 29 7B B8")
		.GetPtr(v_GibSpawn);
	if (!v_GibSpawn)
		Warning(eDLL_T::SERVER,
			"[GIB-GUARD] GibSpawn pattern unresolved -- "
			"spawn-path guard NOT active\n");
}

void VGibFinderGuard::Detour(const bool bAttach) const
{
	if (v_HasGibModel)
	{
		const LONG result = bAttach
			? DetourAttach(reinterpret_cast<void**>(&v_HasGibModel),
				reinterpret_cast<void*>(&Hook_HasGibModel))
			: DetourDetach(reinterpret_cast<void**>(&v_HasGibModel),
				reinterpret_cast<void*>(&Hook_HasGibModel));
		if (bAttach)
			Msg(eDLL_T::SERVER,
				"[GIB-GUARD] DetourAttach HasGibModel result=0x%lX (target=%p)\n",
				result, reinterpret_cast<void*>(v_HasGibModel));
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER,
			"[GIB-GUARD] HasGibModel (player) pattern unresolved -- "
			"fail-closed predicate NOT active for +0x5F08\n");
	}

	if (v_HasGibModel_Twin)
	{
		const LONG result = bAttach
			? DetourAttach(reinterpret_cast<void**>(&v_HasGibModel_Twin),
				reinterpret_cast<void*>(&Hook_HasGibModel_Twin))
			: DetourDetach(reinterpret_cast<void**>(&v_HasGibModel_Twin),
				reinterpret_cast<void*>(&Hook_HasGibModel_Twin));
		if (bAttach)
			Msg(eDLL_T::SERVER,
				"[GIB-GUARD] DetourAttach HasGibModel_Twin result=0x%lX (target=%p)\n",
				result, reinterpret_cast<void*>(v_HasGibModel_Twin));
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER,
			"[GIB-GUARD] HasGibModel (twin) pattern unresolved -- "
			"fail-closed predicate NOT active for +0x2240\n");
	}

	if (v_GibSpawn)
	{
		const LONG result = bAttach
			? DetourAttach(reinterpret_cast<void**>(&v_GibSpawn),
				reinterpret_cast<void*>(&Hook_GibSpawn))
			: DetourDetach(reinterpret_cast<void**>(&v_GibSpawn),
				reinterpret_cast<void*>(&Hook_GibSpawn));
		if (bAttach)
			Msg(eDLL_T::SERVER,
				"[GIB-GUARD] DetourAttach GibSpawn result=0x%lX (target=%p)\n",
				result, reinterpret_cast<void*>(v_GibSpawn));
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER,
			"[GIB-GUARD] GibSpawn pattern unresolved -- "
			"spawn-path guard NOT active\n");
	}
}
