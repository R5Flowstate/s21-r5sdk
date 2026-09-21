//=============================================================================//
//
// Purpose: weapon_akimbo_activity.h implementation. See that header.
//
//=============================================================================//
#include "core/stdafx.h"
#include <intrin.h>
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "game/shared/activity.h"
#include "weapon_akimbo_activity.h"
#include "game/client/cliententitylist.h"
#include "rtech/pak/settings_disk.h"
#include "rtech/pak/rpak_observe.h"

static ConVar sdk_akimbo_reload_anims("sdk_akimbo_reload_anims", "1", FCVAR_RELEASE,
	"Translate the one-handed reload activities to the akimbo reload set while dual wielding.");

static ConVar sdk_akimbo_activity_log("sdk_akimbo_activity_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log akimbo reload activity translations. [AKIMBO-ACT]");

static constexpr int kActOneHandedReloadFirst = 0x267; // ACT_VM_ONEHANDED_RELOAD
static constexpr int kActOneHandedReloadCount = 12;    // ..ACT_VM_ONEHANDED_RELOADEMPTY_LATE5
static constexpr uintptr_t kWeaponOwnerOffset = 0x1560;
static constexpr uintptr_t kAkimboStateOffset = 0x199C;
static constexpr char kAkimboStateActive = 3;

static const char* const s_akimboReloadActs[kActOneHandedReloadCount] = {
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE1",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE2",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE3",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE4",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE5",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE1",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE2",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE3",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE4",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE5",
};

static __int64 (__fastcall* v_C_WeaponX_TranslateWeaponActivity)(void* pWeapon, __int16 act) = nullptr;
static unsigned int (__fastcall* v_C_WeaponX_GetActivityModifiers)(void* pWeapon, uint16_t* pMods) = nullptr;
static void (__fastcall* v_C_Player_AkimboSetState)(void* pPlayer, char newState) = nullptr;
static char (__fastcall* v_C_WeaponX_FireGate)(void* pWeapon, unsigned __int8 a2) = nullptr;
static char (__fastcall* v_C_WeaponX_SetIdealActivityWithModifiers)(void* pWeapon, uint16_t act, const uint16_t* pMods, unsigned int count) = nullptr;
static constexpr uintptr_t kWeaponIdealSequenceOffset = 0x1584;
static constexpr uintptr_t kWeaponIsAkimboOffset = 0x2BBE;
static constexpr uintptr_t kWeaponActiveSlotOffset = 0x2E44;
static const char* s_pEntityList = nullptr;
static int s_akimboActIds[kActOneHandedReloadCount] = {};
static bool s_akimboActIdsResolved = false;

static void* EntityFromHandle(const uint32_t nHandle)
{
	if (nHandle == 0xFFFFFFFFu || !s_pEntityList)
		return nullptr;
	const char* const pSlot = s_pEntityList + (static_cast<size_t>(nHandle & 0xFFFFu) << 5);
	if (*reinterpret_cast<const uint32_t*>(pSlot + 8) != (nHandle >> 16))
		return nullptr;
	return *reinterpret_cast<void* const*>(pSlot);
}

// The custom activities register at host init; resolve once they exist.
static bool ResolveAkimboActIds(void)
{
	if (s_akimboActIdsResolved)
		return true;

	for (int i = 0; i < kActOneHandedReloadCount; ++i)
	{
		const int id = FindActivityByName(s_akimboReloadActs[i]);
		if (id < 0)
			return false;
		s_akimboActIds[i] = id;
	}

	s_akimboActIdsResolved = true;
	Msg(eDLL_T::CLIENT, "[AKIMBO-ACT] akimbo reload activities %d..%d\n",
		s_akimboActIds[0], s_akimboActIds[kActOneHandedReloadCount - 1]);
	return true;
}

static __int64 __fastcall Hook_C_WeaponX_TranslateWeaponActivity(void* pWeapon, __int16 act)
{
	const __int64 translated = v_C_WeaponX_TranslateWeaponActivity(pWeapon, act);

	if (!sdk_akimbo_reload_anims.GetBool() || !pWeapon)
		return translated;

	const int idx = static_cast<int>(translated) - kActOneHandedReloadFirst;
	if (idx < 0 || idx >= kActOneHandedReloadCount)
		return translated;

	const uint32_t nOwner = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const char*>(pWeapon) + kWeaponOwnerOffset);
	const char* const pOwner = reinterpret_cast<const char*>(EntityFromHandle(nOwner));
	if (!pOwner || pOwner[kAkimboStateOffset] != kAkimboStateActive)
		return translated;

	if (!ResolveAkimboActIds())
	{
		static bool s_warned = false;
		if (!s_warned)
		{
			s_warned = true;
			Warning(eDLL_T::CLIENT,
				"[AKIMBO-ACT] ACT_VM_ONEHANDED_AKIMBO_RELOAD* not registered -- one-handed reload clips play instead\n");
		}
		return translated;
	}

	if (sdk_akimbo_activity_log.GetBool())
		Msg(eDLL_T::CLIENT, "[AKIMBO-ACT] weapon=%p act %d -> %d -> %d\n",
			pWeapon, static_cast<int>(act), static_cast<int>(translated), s_akimboActIds[idx]);

	return s_akimboActIds[idx];
}

// One line per distinct (weapon, state, slot, count) so a run shows which
// modifiers the alt hand actually requested.
static unsigned int __fastcall Hook_C_WeaponX_GetActivityModifiers(void* pWeapon, uint16_t* pMods)
{
	const unsigned int count = v_C_WeaponX_GetActivityModifiers(pWeapon, pMods);
	if (!pWeapon || !reinterpret_cast<const char*>(pWeapon)[kWeaponIsAkimboOffset])
		return count;

	const uint32_t nOwner = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const char*>(pWeapon) + kWeaponOwnerOffset);
	const char* const pOwner = reinterpret_cast<const char*>(EntityFromHandle(nOwner));
	const int state = pOwner ? pOwner[kAkimboStateOffset] : -1;
	const int slot = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(pWeapon) + kWeaponActiveSlotOffset);

	static uint64_t s_lastKey = 0;
	static int s_budget = 24;
	const uint64_t key = (reinterpret_cast<uint64_t>(pWeapon) << 16) ^ (static_cast<uint64_t>(state & 0xF) << 8)
		^ (static_cast<uint64_t>(slot & 0xF) << 4) ^ (count & 0xF);
	if (key == s_lastKey || s_budget <= 0)
		return count;
	s_lastKey = key;
	--s_budget;

	char mods[256];
	int n = 0;
	for (unsigned int i = 0; i < count && i < 20 && n < 240; ++i)
		n += _snprintf_s(mods + n, sizeof(mods) - n, _TRUNCATE, "%u ", pMods[i]);
	Msg(eDLL_T::CLIENT, "[AKIMBO-CL] mods weapon=%p state=%d slot=%d count=%u [%s]\n",
		pWeapon, state, slot, count, mods);
	return count;
}

// Which model / rig the alt-hand weapon actually animates from. [AKIMBO-CL]
static void LogAkimboModelBinding(const void* pWeapon)
{
	static bool s_done = false;
	if (s_done || !v_Pak_FindAssetVoid_S21)
		return;
	s_done = true;

	static constexpr uint64_t kP2011Model = 0x08EA0CB36176C0C5ull;
	static constexpr uint64_t kP2011Rig = 0x21F3D0537FC5319Bull;
	const char* const mdl = reinterpret_cast<const char*>(v_Pak_FindAssetVoid_S21(kP2011Model, nullptr));
	const char* const rig = reinterpret_cast<const char*>(v_Pak_FindAssetVoid_S21(kP2011Rig, nullptr));
	const void* const pStudioHdrWrap = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(pWeapon) + 0x1000);
	const void* const pWeaponStudio = pStudioHdrWrap ? *reinterpret_cast<void* const*>(pStudioHdrWrap) : nullptr;
	if (!mdl || !rig)
	{
		Msg(eDLL_T::CLIENT, "[AKIMBO-CL] bind: mdl=%p rig=%p (lookup failed)\n", mdl, rig);
		return;
	}
	const void* const pData = *reinterpret_cast<void* const*>(mdl);
	const char* const pName = *reinterpret_cast<const char* const*>(mdl + 8);
	const void* const* pRigs = *reinterpret_cast<const void* const* const*>(mdl + 0x20);
	const uint32_t nRigs = *reinterpret_cast<const uint32_t*>(mdl + 0x28);
	const uint16_t nRigSeqs = *reinterpret_cast<const uint16_t*>(rig + 0x12);
	Msg(eDLL_T::CLIENT, "[AKIMBO-CL] bind: mdl=%p '%s' data=%p rigs=%u rig[0]=%p | live rig=%p seqs=%u | weapon studiohdr=%p\n",
		mdl, pName ? pName : "?", pData, nRigs, (pRigs && nRigs) ? pRigs[0] : nullptr, rig, nRigSeqs, pWeaponStudio);
	Pak_DumpGuidChain_S21(kP2011Model, "p2011 model");
	Pak_DumpGuidChain_S21(kP2011Rig, "p2011 rig");
}

// Fire gate: while ACTIVE the hand that fires must match owner.m_akimboShouldAltFire.
static char __fastcall Hook_C_WeaponX_FireGate(void* pWeapon, unsigned __int8 a2)
{
	const char result = v_C_WeaponX_FireGate(pWeapon, a2);
	if (!pWeapon || !reinterpret_cast<const char*>(pWeapon)[kWeaponIsAkimboOffset])
		return result;
	const uint32_t nOwner = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(pWeapon) + kWeaponOwnerOffset);
	const char* const pOwner = reinterpret_cast<const char*>(EntityFromHandle(nOwner));
	static int s_budget = 48;
	if (s_budget-- > 0)
		Msg(eDLL_T::CLIENT, "[AKIMBO-CL] fire weapon=%p slot=%d state=%d shouldAlt=%d -> %d\n",
			pWeapon, *reinterpret_cast<const int*>(reinterpret_cast<const char*>(pWeapon) + kWeaponActiveSlotOffset),
			pOwner ? pOwner[kAkimboStateOffset] : -1, pOwner ? pOwner[kAkimboStateOffset + 1] : -1, static_cast<int>(result));
	return result;
}

static char __fastcall Hook_C_WeaponX_SetIdealActivityWithModifiers(void* pWeapon, uint16_t act, const uint16_t* pMods, unsigned int count)
{
	const char result = v_C_WeaponX_SetIdealActivityWithModifiers(pWeapon, act, pMods, count);
	if (!pWeapon || !reinterpret_cast<const char*>(pWeapon)[kWeaponIsAkimboOffset])
		return result;
	const int slot = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(pWeapon) + kWeaponActiveSlotOffset);
	const int seq = *reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(pWeapon) + kWeaponIdealSequenceOffset);
	if (slot == 1)
		LogAkimboModelBinding(pWeapon);
	static uint64_t s_lastKey = 0;
	static int s_budget = 40;
	const uint64_t key = (reinterpret_cast<uint64_t>(pWeapon) << 20) ^ (static_cast<uint64_t>(act) << 4) ^ (static_cast<uint64_t>(seq & 0xFFFF) << 24) ^ (slot & 0xF);
	if (key == s_lastKey || (s_budget <= 0 && !sdk_akimbo_activity_log.GetBool()))
		return result;
	s_lastKey = key;
	--s_budget;
	Msg(eDLL_T::CLIENT, "[AKIMBO-CL] ideal weapon=%p slot=%d act=%u mods=%u -> seq=%d ok=%d\n",
		pWeapon, slot, static_cast<unsigned>(act), count, seq, static_cast<int>(result));
	return result;
}

static ConVar sdk_akimbo_wire_state("sdk_akimbo_wire_state", "1", FCVAR_RELEASE,
	"Replay every networked m_akimboState change through the client's own akimbo SetState.");

static void (__fastcall* v_C_Player_PostDataUpdate)(void* pNetworkable, int updateType, float oldTime, float newTime) = nullptr;
static void* (__fastcall* v_C_WeaponX_GetAkimboPartner)(void* pWeapon) = nullptr;
static constexpr uintptr_t kNetworkableToEntity = 0x18;
static constexpr uintptr_t kEntityRefHandleOffset = 0x8;
static constexpr uintptr_t kWeaponInfoOffset = 0x17A8;
static constexpr uintptr_t kPlayerActiveWeaponsOffset = 0x1930;
static constexpr int kAkimboWireSlots = 256;

struct AkimboWireState
{
	uint32_t handle;
	char state;
};
static AkimboWireState s_akimboWireState[kAkimboWireSlots] = {};

static AkimboWireState* AkimboWireSlot(const void* pPlayer)
{
	const uint32_t handle = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const char*>(pPlayer) + kEntityRefHandleOffset);
	const uint32_t slot = handle & 0xFFFFu;
	if (slot >= kAkimboWireSlots)
		return nullptr;
	AkimboWireState& st = s_akimboWireState[slot];
	if (st.handle != handle)
	{
		st.handle = handle;
		st.state = 0;
	}
	return &st;
}

static void __fastcall Hook_C_Player_AkimboSetState(void* pPlayer, char newState)
{
	const int oldState = pPlayer ? reinterpret_cast<const char*>(pPlayer)[kAkimboStateOffset] : -1;
	v_C_Player_AkimboSetState(pPlayer, newState);
	if (pPlayer)
	{
		if (AkimboWireState* const st = AkimboWireSlot(pPlayer))
			st->state = reinterpret_cast<const char*>(pPlayer)[kAkimboStateOffset];
	}
	static int s_budget = 32;
	if (oldState != newState && s_budget-- > 0)
		Msg(eDLL_T::CLIENT, "[AKIMBO-CL] client SetState %d -> %d player=%p\n", oldState, static_cast<int>(newState), pPlayer);
}

static bool AkimboClient_WeaponReady(const void* pWeapon)
{
	return pWeapon && *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(pWeapon) + kWeaponInfoOffset) != nullptr;
}

// The wire writes m_akimboState straight into the player, so the client's
// own SetState (mod trio, optic disable, weapon callback) early-outs on a
// value it already holds. Rewind the byte and play the transition through it.
static void __fastcall Hook_C_Player_PostDataUpdate(void* pNetworkable, int updateType, float oldTime, float newTime)
{
	v_C_Player_PostDataUpdate(pNetworkable, updateType, oldTime, newTime);
	if (!sdk_akimbo_wire_state.GetBool() || !pNetworkable)
		return;

	char* const pPlayer = reinterpret_cast<char*>(pNetworkable) - kNetworkableToEntity;
	AkimboWireState* const st = AkimboWireSlot(pPlayer);
	if (!st)
		return;

	const char wireState = pPlayer[kAkimboStateOffset];
	if (wireState == st->state)
		return;
	if (wireState < 0 || wireState > 3)
	{
		st->state = wireState;
		return;
	}

	// The weapons of this packet may not have run their own PostDataUpdate
	// yet; a later player update retries until both hands carry weapon data.
	void* const main = EntityFromHandle(*reinterpret_cast<const uint32_t*>(pPlayer + kPlayerActiveWeaponsOffset));
	if (!AkimboClient_WeaponReady(main))
		return;
	void* const other = v_C_WeaponX_GetAkimboPartner(main);
	if (other && !AkimboClient_WeaponReady(other))
		return;

	const char oldState = st->state;
	pPlayer[kAkimboStateOffset] = oldState;
	v_C_Player_AkimboSetState(pPlayer, wireState);
	st->state = pPlayer[kAkimboStateOffset];

	static int s_budget = 32;
	if (s_budget-- > 0)
		Msg(eDLL_T::CLIENT, "[AKIMBO-CL] wire SetState %d -> %d player=%p main=%p other=%p\n",
			static_cast<int>(oldState), static_cast<int>(wireState), pPlayer, main, other);
}

static char (__fastcall* v_C_Player_CanZoom)(void* pPlayer) = nullptr;
static bool AkimboClient_IsDualWielding(const void* pPlayer);
static unsigned int (__fastcall* v_C_BCC_WeaponDisableReason)(void* pPlayer, void* pWeapon, char allChecks) = nullptr;
static bool (__fastcall* v_C_BCC_IsSlotDisabled)(void* pPlayer, unsigned int slot) = nullptr;

static constexpr uintptr_t kPlayerSelectedWeaponsOffset = 0x1940;
static constexpr uintptr_t kPlayerMoveTypeOffset = 0x3B2;
static constexpr uintptr_t kPlayerZoomingOffset = 0x1BE1;
static constexpr uintptr_t kPlayerButtonsOffset = 0x2A0C;
static constexpr uintptr_t kPlayerFlagsOffset = 0x3268;
static constexpr uintptr_t kWeaponStateOffset = 0x15A4;
static constexpr uintptr_t kWeaponNextReadyOffset = 0x1568;
static constexpr uintptr_t kWeaponReloadingOffset = 0x15AA;
static constexpr uintptr_t kWeaponReloadAllowAdsOffset = 0x1F31;
static constexpr uintptr_t kWeaponDisableZoomedRechamberOffset = 0x1F35;
static constexpr uintptr_t kWeaponSemiNeedsRechamberOffset = 0x161E;
static constexpr uintptr_t kWeaponZoomEffectsOffset = 0x2774;
static constexpr uintptr_t kWeaponClipOffset = 0x1590;

static __int64 (__fastcall* v_C_Player_StartZoom)(void* pPlayer) = nullptr;
static __int64 (__fastcall* v_C_Player_StopZoom)(void* pPlayer, unsigned __int8 a2) = nullptr;
static void* (__fastcall* v_C_Player_TargetingWeapon)(void* pPlayer) = nullptr;

static bool AkimboClient_IsDualWielding(const void* pPlayer)
{
	const char* const p = reinterpret_cast<const char*>(pPlayer);
	const char* const main = reinterpret_cast<const char*>(EntityFromHandle(*reinterpret_cast<const uint32_t*>(p + kPlayerActiveWeaponsOffset)));
	const char* const alt = reinterpret_cast<const char*>(EntityFromHandle(*reinterpret_cast<const uint32_t*>(p + kPlayerActiveWeaponsOffset + 4)));
	return main && alt && main[kWeaponIsAkimboOffset] && alt[kWeaponIsAkimboOffset];
}

// StartZoom exits before zooming when the targeting weapon is rechambering.
static __int64 __fastcall Hook_C_Player_StartZoom(void* pPlayer)
{
	const __int64 result = v_C_Player_StartZoom(pPlayer);
	if (!pPlayer || !AkimboClient_IsDualWielding(pPlayer))
		return result;
	const char* const p = reinterpret_cast<const char*>(pPlayer);
	if (p[kPlayerZoomingOffset])
		return result;
	const char* const w = v_C_Player_TargetingWeapon ? reinterpret_cast<const char*>(v_C_Player_TargetingWeapon(pPlayer)) : nullptr;
	static int s_budget = 24;
	if (s_budget-- > 0)
		Msg(eDLL_T::CLIENT, "[AKIMBO-CL] startzoom refused weapon=%p state=%d ready=%.3f needsRechamber=%d reloading=%d\n",
			w, w ? *reinterpret_cast<const int*>(w + kWeaponStateOffset) : -1, w ? *reinterpret_cast<const float*>(w + kWeaponNextReadyOffset) : 0.f,
			w ? w[kWeaponSemiNeedsRechamberOffset] : -1, w ? w[kWeaponReloadingOffset] : -1);
	return result;
}

static __int64 __fastcall Hook_C_Player_StopZoom(void* pPlayer, unsigned __int8 a2)
{
	if (pPlayer && AkimboClient_IsDualWielding(pPlayer) && reinterpret_cast<const char*>(pPlayer)[kPlayerZoomingOffset])
	{
		static int s_budget = 24;
		if (s_budget-- > 0)
			Msg(eDLL_T::CLIENT, "[AKIMBO-CL] stopzoom arg=%u caller=0x%llX\n", a2,
				static_cast<unsigned long long>(0x140000000ull + (reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_GameDll.GetModuleBase())));
	}
	return v_C_Player_StopZoom(pPlayer, a2);
}

// While dual wielding, name every CanZoom term the moment it refuses. [AKIMBO-CL]
static char __fastcall Hook_C_Player_CanZoom(void* pPlayer)
{
	const char result = v_C_Player_CanZoom(pPlayer);
	if (!pPlayer)
		return result;

	const char* const p = reinterpret_cast<const char*>(pPlayer);
	if (AkimboClient_IsDualWielding(pPlayer))
	{
		const unsigned int btn = *reinterpret_cast<const unsigned int*>(p + kPlayerButtonsOffset);
		const unsigned int pressed = *reinterpret_cast<const unsigned int*>(p + kPlayerButtonsOffset + 4);
		const unsigned int key = (btn & 0x30001u) | (p[kPlayerZoomingOffset] ? 0x100u : 0u) | (result ? 0x200u : 0u)
			| ((*reinterpret_cast<const float*>(p + 0x1BE4) != 0.0f) ? 0x400u : 0u);
		static unsigned int s_lastKey = ~0u;
		static int s_budget = 80;
		if (key != s_lastKey && s_budget > 0)
		{
			s_lastKey = key;
			--s_budget;
			Msg(eDLL_T::CLIENT, "[AKIMBO-CL] zoomtick btn=0x%X pressed=0x%X zooming=%d canzoom=%d toggleT=%.3f\n",
				btn, pressed, p[kPlayerZoomingOffset], result, *reinterpret_cast<const float*>(p + 0x1BE4));
		}
	}
	if (result)
		return result;

	const char* const main = reinterpret_cast<const char*>(EntityFromHandle(*reinterpret_cast<const uint32_t*>(p + kPlayerActiveWeaponsOffset)));
	const char* const alt = reinterpret_cast<const char*>(EntityFromHandle(*reinterpret_cast<const uint32_t*>(p + kPlayerActiveWeaponsOffset + 4)));
	if (!main || !alt || !main[kWeaponIsAkimboOffset] || !alt[kWeaponIsAkimboOffset])
		return result;

	const unsigned int reason = v_C_BCC_WeaponDisableReason ? v_C_BCC_WeaponDisableReason(pPlayer, const_cast<char*>(main), 0) : 99;
	const int slotDisabled = v_C_BCC_IsSlotDisabled ? (v_C_BCC_IsSlotDisabled(pPlayer, 0) ? 1 : 0) : 9;
	const int state = *reinterpret_cast<const int*>(main + kWeaponStateOffset);
	const uint64_t key = (static_cast<uint64_t>(reason) << 40) ^ (static_cast<uint64_t>(slotDisabled) << 32) ^ (static_cast<uint64_t>(state) << 16)
		^ static_cast<uint8_t>(p[kPlayerSelectedWeaponsOffset]) ^ (static_cast<uint64_t>(main[kWeaponReloadingOffset]) << 8) ^ (static_cast<uint64_t>(main[kWeaponZoomEffectsOffset]) << 9);
	static uint64_t s_lastKey = ~0ull;
	static int s_budget = 24;
	if (key == s_lastKey || s_budget <= 0)
		return result;
	s_lastKey = key;
	--s_budget;
	Msg(eDLL_T::CLIENT, "[AKIMBO-CL] canzoom=0 reason=%u slotDis=%d main=%p state=%d ready=%.3f zoomFx=%d reload=%d/%d rech=%d/%d clip=%d sel0=%d movetype=%d zooming=%d btn=0x%X pflags=0x%X alt=%p altState=%d altReady=%.3f\n",
		reason, slotDisabled, main, state, *reinterpret_cast<const float*>(main + kWeaponNextReadyOffset),
		main[kWeaponZoomEffectsOffset], main[kWeaponReloadingOffset], main[kWeaponReloadAllowAdsOffset],
		main[kWeaponDisableZoomedRechamberOffset], main[kWeaponSemiNeedsRechamberOffset],
		*reinterpret_cast<const int*>(main + kWeaponClipOffset), static_cast<int8_t>(p[kPlayerSelectedWeaponsOffset]),
		p[kPlayerMoveTypeOffset], p[kPlayerZoomingOffset], *reinterpret_cast<const unsigned int*>(p + kPlayerButtonsOffset),
		*reinterpret_cast<const unsigned int*>(p + kPlayerFlagsOffset), alt, *reinterpret_cast<const int*>(alt + kWeaponStateOffset),
		*reinterpret_cast<const float*>(alt + kWeaponNextReadyOffset));
	return result;
}

//-----------------------------------------------------------------------------
// Purpose: dump what the local client's akimbo runtime sees. [AKIMBO-CL]
//-----------------------------------------------------------------------------
static void AkimboClientProbe_f(const CCommand& /*args*/)
{
	if (!g_pClientEntityList || !s_pEntityList)
	{
		Warning(eDLL_T::CLIENT, "[AKIMBO-CL] entity list unavailable\n");
		return;
	}
	const char* const pPlayer = reinterpret_cast<const char*>(g_pClientEntityList->GetClientEntity(1));
	if (!pPlayer)
	{
		Warning(eDLL_T::CLIENT, "[AKIMBO-CL] no local player\n");
		return;
	}
	const char* const pInv = pPlayer + 0x18D8;
	Msg(eDLL_T::CLIENT, "[AKIMBO-CL] player=%p akimboState=%d shouldAltFire=%d selectedWeapons=%d/%d\n",
		pPlayer, pPlayer[kAkimboStateOffset], pPlayer[kAkimboStateOffset + 1],
		static_cast<int8_t>(pPlayer[0x1941]), static_cast<int8_t>(pPlayer[0x1942]));
	for (int slot = 0; slot < 12; ++slot)
	{
		const uint32_t h = *reinterpret_cast<const uint32_t*>(pInv + 8 + 4 * slot);
		const char* const w = reinterpret_cast<const char*>(EntityFromHandle(h));
		if (!w)
			continue;
		Msg(eDLL_T::CLIENT, "[AKIMBO-CL]  inv[%d]=%p isAkimbo=%d flip=%d fireMode=%d disabled=%d\n",
			slot, w, w[0x2BBE], w[0x2BBD], *reinterpret_cast<const int*>(w + 0x2BC8), w[0x16C4]);
	}
	for (int hand = 0; hand < 3; ++hand)
	{
		const uint32_t h = *reinterpret_cast<const uint32_t*>(pInv + 0x58 + 4 * hand);
		Msg(eDLL_T::CLIENT, "[AKIMBO-CL]  active[%d]=%p\n", hand, EntityFromHandle(h));
	}
}

static ConCommand sdk_akimbo_probe("sdk_akimbo_probe", AkimboClientProbe_f,
	"Dump the local client's akimbo state, inventory akimbo flags and active hands. [AKIMBO-CL]",
	FCVAR_DEVELOPMENTONLY);

void VWeaponAkimboActivity::GetAdr(void) const
{
	LogFunAdr("C_WeaponX::TranslateWeaponActivity", v_C_WeaponX_TranslateWeaponActivity);
	LogFunAdr("C_WeaponX::GetActivityModifiers", v_C_WeaponX_GetActivityModifiers);
	LogFunAdr("C_Player::AkimboSetState", v_C_Player_AkimboSetState);
	LogFunAdr("C_Player::PostDataUpdate", v_C_Player_PostDataUpdate);
	LogFunAdr("C_WeaponX::GetAkimboPartner", v_C_WeaponX_GetAkimboPartner);
	LogFunAdr("C_WeaponX::SetIdealActivityWithModifiers", v_C_WeaponX_SetIdealActivityWithModifiers);
	LogFunAdr("C_WeaponX::FireGate", v_C_WeaponX_FireGate);
	LogFunAdr("C_Player::CanZoom", v_C_Player_CanZoom);
	LogFunAdr("C_Player::StartZoom", v_C_Player_StartZoom);
	LogFunAdr("C_Player::StopZoom", v_C_Player_StopZoom);
	LogFunAdr("C_Player::TargetingWeapon", v_C_Player_TargetingWeapon);
	LogFunAdr("C_BaseCombatCharacter::WeaponDisableReason", v_C_BCC_WeaponDisableReason);
	LogFunAdr("C_BaseCombatCharacter::IsSlotDisabled", v_C_BCC_IsSlotDisabled);
	LogVarAdr("g_clientEntityList (akimbo)", reinterpret_cast<void*>(const_cast<char*>(s_pEntityList)));
}

void VWeaponAkimboActivity::GetFun(void) const
{
	// TranslateWeaponActivity: anim-alt step (alt index +0x27E0) then the one-handed table.
	Module_FindPattern(g_GameDll,
		"44 0F BF D2 4C 8B C1 48 63 91 E0 27 00 00 8D 42 FF 83 F8 02 0F 87 ?? ?? ?? ?? 4C 8B CA 41 8D 82 30 FE FF FF")
		.GetPtr(v_C_WeaponX_TranslateWeaponActivity);
	if (!v_C_WeaponX_TranslateWeaponActivity)
		Warning(eDLL_T::CLIENT,
			"[AKIMBO-ACT] TranslateWeaponActivity pattern unresolved -- akimbo reload clips disabled\n");

	Module_FindPattern(g_GameDll,
		"48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B FA 48 8B F1 8B 91 60 15 00 00 83 FA FF 0F 84")
		.GetPtr(v_C_WeaponX_GetActivityModifiers);
	Module_FindPattern(g_GameDll,
		"48 8B C4 56 48 81 EC 90 00 00 00 48 8B F1 3A 91 9C 19 00 00")
		.GetPtr(v_C_Player_AkimboSetState);
	Module_FindPattern(g_GameDll,
		"48 8B C4 41 56 48 81 EC A0 00 00 00 48 89 58 18 48 89 68 F0 48 89 70 E8 48 89 78 E0 48 8B F9 4C 89 60 D8 4C 89 68 D0 44 8B EA 0F 29 70 B8 0F 28 F3 0F 29 78 A8 0F 28 FA 44 0F 29 48 88 83 FA 01 75 ?? 0F B6 81 78 40 00 00 88 81 79 40 00 00")
		.GetPtr(v_C_Player_PostDataUpdate);
	// Partner lookup through the owner's dual-primary slot pair (slot +-7).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B 81 60 15 00 00 48 8B D9 83 F8 FF 74 55 0F B7 C8 48 8D 2D ?? ?? ?? ?? 48 C1 E1 05")
		.GetPtr(v_C_WeaponX_GetAkimboPartner);
	if (!v_C_Player_PostDataUpdate || !v_C_WeaponX_GetAkimboPartner)
		Warning(eDLL_T::CLIENT,
			"[AKIMBO-CL] PostDataUpdate/GetAkimboPartner pattern unresolved -- wire akimbo state is not replayed\n");
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 56 41 56 41 57 48 81 EC 80 00 00 00 8B 81 80 16 00 00 45 8B F1 0F BF EA")
		.GetPtr(v_C_WeaponX_SetIdealActivityWithModifiers);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 20 8B 81 60 15 00 00 4C 8D 05 ?? ?? ?? ?? 48 8B D9 0F B6 EA 8B C8 83 F8 FF 75")
		.GetPtr(v_C_WeaponX_FireGate);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 84 C0 74 11 48 8B 05 ?? ?? ?? ?? 83 78 64 00")
		.GetPtr(v_C_Player_CanZoom);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 30 48 8B F1 0F 29 74 24 20 48 8B 0D ?? ?? ?? ?? 41 0F B6 D8 48 8B FA 48 8B 01 FF 90 38")
		.GetPtr(v_C_BCC_WeaponDisableReason);
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 4C 8B C1 8B DA 0F B6 CA B8 01 00 00 00 D2 E0 41 84 80 82 19 00 00 75 5A 41 83 B8 64 19 00 00 00 75 50")
		.GetPtr(v_C_BCC_IsSlotDisabled);
	Module_FindPattern(g_GameDll,
		"40 57 48 83 EC 50 48 8B F9 E8 ?? ?? ?? ?? 83 B8 A4 15 00 00 0E 75 19 48 8B 15 ?? ?? ?? ?? F3 0F 10 42 28 0F 2F 80 68 15 00 00")
		.GetPtr(v_C_Player_StartZoom);
	if (v_C_Player_StartZoom)
		v_C_Player_TargetingWeapon = CMemory(v_C_Player_StartZoom).Offset(0x9).FollowNearCallSelf().RCast<void* (__fastcall*)(void*)>();
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 48 8D 68 A1 48 81 EC D8 00 00 00 48 89 70 08 48 8B D9 48 89 78 10 4C 89 70 20 44 0F B6 F2 4C 89 78 E8 E8 ?? ?? ?? ??")
		.GetPtr(v_C_Player_StopZoom);

	// RequestBodygroupUpdate carries the entity-list lea (owner handle +0x1560).
	const CMemory bodygroup = Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 8B 81 60 15 00 00 48 8B D9 83 F8 FF "
		"0F 84 ?? ?? ?? ?? 0F B7 C8 48 C1 E1 05 48 89 74 24 ?? 48 8D 35");
	if (bodygroup.IsValid())
	{
		const CMemory entLea = bodygroup.FindPattern("48 8D 35");
		if (entLea.IsValid())
			s_pEntityList = entLea.ResolveRelativeAddress(3, 7).RCast<const char*>();
	}
	if (!s_pEntityList)
		Warning(eDLL_T::CLIENT,
			"[AKIMBO-ACT] entity list unresolved -- akimbo reload clips disabled\n");
}

void VWeaponAkimboActivity::Detour(const bool bAttach) const
{
	if (v_C_WeaponX_TranslateWeaponActivity && s_pEntityList)
		DetourSetup(&v_C_WeaponX_TranslateWeaponActivity, &Hook_C_WeaponX_TranslateWeaponActivity, bAttach);
	if (v_C_WeaponX_GetActivityModifiers && s_pEntityList)
		DetourSetup(&v_C_WeaponX_GetActivityModifiers, &Hook_C_WeaponX_GetActivityModifiers, bAttach);
	if (v_C_Player_AkimboSetState)
		DetourSetup(&v_C_Player_AkimboSetState, &Hook_C_Player_AkimboSetState, bAttach);
	if (v_C_Player_PostDataUpdate && v_C_Player_AkimboSetState && v_C_WeaponX_GetAkimboPartner && s_pEntityList)
		DetourSetup(&v_C_Player_PostDataUpdate, &Hook_C_Player_PostDataUpdate, bAttach);
	if (v_C_WeaponX_SetIdealActivityWithModifiers)
		DetourSetup(&v_C_WeaponX_SetIdealActivityWithModifiers, &Hook_C_WeaponX_SetIdealActivityWithModifiers, bAttach);
	if (v_C_WeaponX_FireGate && s_pEntityList)
		DetourSetup(&v_C_WeaponX_FireGate, &Hook_C_WeaponX_FireGate, bAttach);
	if (v_C_Player_CanZoom && s_pEntityList)
		DetourSetup(&v_C_Player_CanZoom, &Hook_C_Player_CanZoom, bAttach);
	if (v_C_Player_StartZoom && s_pEntityList)
		DetourSetup(&v_C_Player_StartZoom, &Hook_C_Player_StartZoom, bAttach);
	if (v_C_Player_StopZoom && s_pEntityList)
		DetourSetup(&v_C_Player_StopZoom, &Hook_C_Player_StopZoom, bAttach);
}
