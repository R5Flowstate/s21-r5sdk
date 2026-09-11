//=============================================================================
//
// Purpose: widen offhand activation 6 -> 8. One near page holds the 8-entry bitmask table and trampolines.
//
//=============================================================================
#include "core/stdafx.h"
#include "offhand_activation_patches.h"
#include "offhand_slots_ext.h"
#include "game/shared/sdk_entity_state.h"
#ifndef CLIENT_DLL
#include "game/shared/edict_dirty.h"
#endif
#include "public/tier0/memaddr.h"
#include "public/tier0/module.h"
#include "public/tier0/tier0_iface.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "thirdparty/detours/include/detours.h"

#include <vector>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <Windows.h>

// Every trampoline lands here. Returns the EHandle for the requested slot.
extern "C" uint32_t OffhandReadHelper(void* pPlayer,
                                       int64_t slot,
                                       void* pInventoryOrNull)
{
	// Normalise slot to unsigned -- the engine sign-extends.
	if (static_cast<uint32_t>(slot) < 6u)
	{
		if (pInventoryOrNull)
			return *reinterpret_cast<uint32_t*>(
				reinterpret_cast<uint8_t*>(pInventoryOrNull)
					+ 0x2C + static_cast<int>(slot) * 4);

		return *reinterpret_cast<uint32_t*>(
			reinterpret_cast<uint8_t*>(pPlayer)
				+ 0x16B4 + static_cast<int>(slot) * 4);
	}
	if (slot == 6 || slot == 7)
	{
		// Dispatcher runs on both ticks. Probe both L1 maps; they are disjoint by EHandle.
		return OffhandSlotsExt_GetAnySide(pPlayer,
			static_cast<int>(slot), nullptr);
	}
	// Widening the gate from <6 to <8 means slot >= 8 can't enter here via
	// the engine (gate still rejects). Defensive sentinel.
	return 0xFFFFFFFFu;
}

// Resolves m_selectedOffhands[k] then offhandWeapons[slot]. Slot >= 6 returns NULL unpatched (cmp bl,6).
// Hook: 6/7 via OffhandSlotsExt_GetServer + Resolve; 0..5 pass through.
static int64_t (*v_SelectedOffhandResolver_orig)(int64_t a1, int a2) = nullptr;

#ifndef CLIENT_DLL
// Set to the simulating player for the duration of a usercmd. FireWeaponGrenade
// with clientPredicted=true (stock toss scripts) requires this non-null.
static void** g_pPredictablePlayer = nullptr;

// Script VM context global. The silent Squirrel-call gate reads tag at +24.
static void** g_pScriptContext = nullptr;

// Per-player offhand think. First-loop samples m_nButtons live. C2S
// TrySelectOffhand runs after usercmd apply, so the next cmd wipes the bit
// before this function sees it. Apply pending at entry.
static char (*v_OffhandDispatch_orig)(void* pPlayer, int64_t a2, int64_t a3) = nullptr;

// TossPrep dispatch (idx 24 / ACT 544+). Slot 6/7 tap then needs DoToss.
static int64_t (*v_WeaponTossRelease_orig)(int64_t weapon, uint8_t a2) = nullptr;

// DoToss: SetIdealWeaponActivity(ACT_VM_TOSS) + GRENADE gesture. Emits id 81.
static char (*v_WeaponDoToss_orig)(int64_t weapon) = nullptr;

// S3 SetIdealWeaponActivity(weapon, activity). SetupPredictedAnimEvents is inside.
static char (*v_WeaponSetIdealActivity_orig)(int64_t weapon, int activity) = nullptr;

// CBaseEntity::SetModel(const char*). Server half; m_nModelIndex is WORD at +0xDE.
static int64_t (*v_EntitySetModel_orig)(int64_t entity, const char* name) = nullptr;

extern int64_t Server_PrecacheModel_Invoke(const char* modelName);

static void (*v_CPlayer_SetOneHandedOn)(void* pPlayer) = nullptr;
static void (*v_CPlayer_SetOneHandedOff)(void* pPlayer) = nullptr;

// S3 ACT_VM_TOSS. Holospray rseqs name-bind this; they have no ONEHANDED_TOSS.
static constexpr int kActVmToss = 542;

// CPlayer weapon frame. Re-applies the select latch so button bits survive
// either ordering of dispatcher vs weapon frame within a tick.
static void* (*v_PlayerWeaponFrame_orig)(int64_t player) = nullptr;

// Weapon script-callback dispatcher (handler table; slot 24 is TossPrep).
static char (*v_WeaponAnimEventDispatch_orig)(void* pWeapon, int64_t eventIdx,
	int64_t a3, int64_t a4, void* a5, char a6) = nullptr;

// Weapon animation-event switch (ids 72..115; 81 is toss release).
static char (*v_WeaponAnimEvent_orig)(void* pWeapon, int animEventId,
	int64_t a3, int64_t a4) = nullptr;

static ConVar sdk_offhand_ext_toss_diag("sdk_offhand_ext_toss_diag", "0",
	FCVAR_DEVELOPMENTONLY, "Dump the toss state machine's decision fields on each "
	"extended-slot offhand activation (first 8).");

static ConVar bridge_offhand_ext_autotoss("bridge_offhand_ext_autotoss", "1",
	FCVAR_RELEASE,
	"After slot 6/7 TossPrep, invoke DoToss so a tap-to-throw one-hand weapon "
	"plays ACT_VM_TOSS (event 81). Native 0..5 offhands already reach DoToss.");

static bool OffhandExt_IsExtendedWeapon(int64_t weapon, void** ppOwner)
{
	if (ppOwner)
		*ppOwner = nullptr;
	if (!weapon)
		return false;

	const uint32_t ownerEh = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<uint8_t*>(weapon) + 0x11F0);
	void* const owner = SDKEntityState_Resolve(
		SDKEntityHandle(ownerEh), ESide::Server);
	if (ppOwner)
		*ppOwner = owner;
	if (!owner)
		return false;

	const uint32_t weaponEh = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<uint8_t*>(weapon) + 0x8);
	if (weaponEh == kOffhandSlotExtInvalidHandle)
		return false;

	const uint32_t eh6 = OffhandSlotsExt_GetServer(owner, 6);
	const uint32_t eh7 = OffhandSlotsExt_GetServer(owner, 7);
	return weaponEh == eh6 || weaponEh == eh7;
}

static const char* OffhandExt_StudioName(int64_t weapon)
{
	if (!weapon)
		return "";
	const uintptr_t cstudio = *reinterpret_cast<uintptr_t*>(
		reinterpret_cast<uint8_t*>(weapon) + 0xFD8);
	if (!cstudio)
		return "";
	const uintptr_t hdr = *reinterpret_cast<uintptr_t*>(cstudio + 0x08);
	if (!hdr)
		return "";
	return reinterpret_cast<const char*>(hdr + 0x10);
}

// Slot-6 Give leaves the playermodel studio (no ACT_VM_TOSS). Bind ptpov
// for SetIdeal, then restore m_nModelIndex so the 1p mesh does not replicate.
static void OffhandExt_BindTossStudio(int64_t weapon)
{
	if (!weapon || !v_EntitySetModel_orig)
		return;

	const char* const cur = OffhandExt_StudioName(weapon);
	if (!cur || !std::strstr(cur, "holo_spray") || std::strstr(cur, "ptpov"))
		return;

	uint8_t* const w = reinterpret_cast<uint8_t*>(weapon);
	const int16_t modelIdx = *reinterpret_cast<int16_t*>(w + 0xDE);
	const int32_t worldIdx = *reinterpret_cast<int32_t*>(w + 0x1208); // m_iWorldModelIndex

	const char* const path = "mdl/weapons/holo_spray/ptpov_holo_spray.rmdl";
	(void)Server_PrecacheModel_Invoke(path);
	v_EntitySetModel_orig(weapon, path);
	*reinterpret_cast<int16_t*>(w + 0xDE) = modelIdx;
	*reinterpret_cast<int32_t*>(w + 0x1208) = worldIdx;
	MarkEntityEdictDirty(reinterpret_cast<void*>(weapon));
}

void OffhandActivation_SetOneHanded(void* pPlayer, bool on)
{
	if (!pPlayer)
		return;
	if (on)
	{
		if (v_CPlayer_SetOneHandedOn)
			v_CPlayer_SetOneHandedOn(pPlayer);
	}
	else if (v_CPlayer_SetOneHandedOff)
	{
		v_CPlayer_SetOneHandedOff(pPlayer);
	}
}

bool OffhandActivation_TossDiagEnabled(void)
{
	return sdk_offhand_ext_toss_diag.GetBool();
}

// Comma-separated indices of non-null anim-event handlers at weapon+5752 (45 slots).
static void OffhandExt_FormatHandlerSlots(const void* pWeapon, char* pOut, size_t nOut)
{
	if (!pOut || nOut == 0)
		return;
	pOut[0] = '\0';
	if (!pWeapon || nOut < 2)
		return;

	const uint8_t* const base =
		static_cast<const uint8_t*>(pWeapon) + 5752;
	size_t used = 0;
	for (int i = 0; i < 45; ++i)
	{
		void* const fn = *reinterpret_cast<void* const*>(base + 8 * i);
		if (!fn)
			continue;
		const int room = static_cast<int>(nOut - used);
		if (room <= 1)
			break;
		const int wrote = snprintf(pOut + used, static_cast<size_t>(room),
			"%s%d", used ? "," : "", i);
		if (wrote <= 0 || wrote >= room)
			break;
		used += static_cast<size_t>(wrote);
	}
}

static char Hook_OffhandDispatch(void* pPlayer, int64_t a2, int64_t a3)
{
	OffhandSlotsExt_ApplyPendingSelect(pPlayer);
	return v_OffhandDispatch_orig(pPlayer, a2, a3);
}

static void* Hook_PlayerWeaponFrame(int64_t player)
{
	OffhandSlotsExt_ApplyPendingSelect(reinterpret_cast<void*>(player));
	return v_PlayerWeaponFrame_orig(player);
}

static char Hook_WeaponAnimEvent(void* pWeapon, int animEventId,
	int64_t a3, int64_t a4)
{
	void* owner = nullptr;
	const bool isExt = OffhandExt_IsExtendedWeapon(
		reinterpret_cast<int64_t>(pWeapon), &owner);

	if (sdk_offhand_ext_toss_diag.GetBool())
	{
		static volatile LONG s_weaponAnimEventLog = 0;
		const LONG n = InterlockedIncrement(&s_weaponAnimEventLog);
		if (n <= 40)
		{
			const char* className = "";
			if (pWeapon)
			{
				// Class name is an inline char array at weapon+0x15B0.
				className = reinterpret_cast<const char*>(
					static_cast<const uint8_t*>(pWeapon) + 0x15B0);
			}
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] animevent weap=%p class='%.63s' id=%d (#%d)\n",
				pWeapon, className, animEventId, static_cast<int>(n));
		}
	}

	// ThrowDeployable(clientPredicted=true) is legal only while this is set.
	void* savedPred = nullptr;
	bool predArmed = false;
	if (isExt && animEventId == 81 && owner
		&& g_pPredictablePlayer && !*g_pPredictablePlayer)
	{
		savedPred = *g_pPredictablePlayer;
		*g_pPredictablePlayer = owner;
		predArmed = true;
	}

	const char result = v_WeaponAnimEvent_orig(pWeapon, animEventId, a3, a4);

	if (predArmed && g_pPredictablePlayer)
		*g_pPredictablePlayer = savedPred;

	if (isExt && animEventId == 81 && owner)
	{
		OffhandSlotsExt_EndTossWindow(owner);
		OffhandActivation_SetOneHanded(owner, false);
		if (sdk_offhand_ext_toss_diag.GetBool())
		{
			static volatile LONG s_endHoldLog = 0;
			const LONG n = InterlockedIncrement(&s_endHoldLog);
			if (n <= 8)
				Msg(eDLL_T::SERVER,
					"[OFFHAND-EXT] toss-endhold weap=%p id=81 pred=%d (#%d)\n",
					pWeapon, predArmed ? 1 : 0, static_cast<int>(n));
		}
	}

	return result;
}

static char Hook_WeaponAnimEventDispatch(void* pWeapon, int64_t eventIdx,
	int64_t a3, int64_t a4, void* a5, char a6)
{
	if (eventIdx != 24 || !sdk_offhand_ext_toss_diag.GetBool())
		return v_WeaponAnimEventDispatch_orig(pWeapon, eventIdx, a3, a4, a5, a6);

	static volatile LONG s_animEvent24Log = 0;
	const LONG n = InterlockedIncrement(&s_animEvent24Log);
	if (n <= 8)
	{
		void* handler = nullptr;
		if (pWeapon)
			handler = *reinterpret_cast<void**>(
				static_cast<uint8_t*>(pWeapon) + 0x1738);

		if (!g_pScriptContext)
		{
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] animevent24 weap=%p handler=%p ctx=null (#%d)\n",
				pWeapon, handler, static_cast<int>(n));
		}
		else
		{
			void* const ctx = *g_pScriptContext;
			uint32_t gate = 0;
			int gateOk = 0;
			int32_t stackTop = 0;
			if (ctx)
			{
				// Script-call silent-bail tag lives at context+24.
				gate = *reinterpret_cast<uint32_t*>(
					static_cast<uint8_t*>(ctx) + 24);
				gateOk = (gate == 0x01000001u) ? 1 : 0;
				void* const stack = *reinterpret_cast<void**>(
					static_cast<uint8_t*>(ctx) + 8);
				if (stack)
					stackTop = *reinterpret_cast<int32_t*>(
						static_cast<uint8_t*>(stack) + 120);
			}
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] animevent24 weap=%p handler=%p ctx=%p "
				"gate=0x%08X gateOk=%d stackTop=%d (#%d)\n",
				pWeapon, handler, ctx, gate, gateOk, static_cast<int>(stackTop),
				static_cast<int>(n));
		}

		// 45-slot anim-event handler table copied at Deploy (base +5752).
		char slotsBuf[256];
		OffhandExt_FormatHandlerSlots(pWeapon, slotsBuf, sizeof(slotsBuf));
		void* slot23 = nullptr;
		void* slot24 = nullptr;
		void* slot26 = nullptr;
		if (pWeapon)
		{
			uint8_t* const base = static_cast<uint8_t*>(pWeapon) + 5752;
			slot23 = *reinterpret_cast<void**>(base + 8 * 23);
			slot24 = *reinterpret_cast<void**>(base + 8 * 24);
			slot26 = *reinterpret_cast<void**>(base + 8 * 26);
		}
		Msg(eDLL_T::SERVER,
			"[OFFHAND-EXT] animevent24 table weap=%p slot23=%p slot24=%p slot26=%p "
			"slots=%s (#%d)\n",
			pWeapon, slot23, slot24, slot26, slotsBuf, static_cast<int>(n));
	}

	const char result = v_WeaponAnimEventDispatch_orig(
		pWeapon, eventIdx, a3, a4, a5, a6);

	if (n <= 8)
		Msg(eDLL_T::SERVER,
			"[OFFHAND-EXT] animevent24 ret=%d (#%d)\n",
			static_cast<int>(result), static_cast<int>(n));

	return result;
}

static int64_t Hook_WeaponTossRelease(int64_t weapon, uint8_t a2)
{
	void* owner = nullptr;
	const bool isExtSlot = OffhandExt_IsExtendedWeapon(weapon, &owner);

	static volatile LONG s_tossReleaseLog = 0;
	const LONG n = InterlockedIncrement(&s_tossReleaseLog);
	if (sdk_offhand_ext_toss_diag.GetBool() && n <= 8)
		Msg(eDLL_T::SERVER,
			"[OFFHAND-EXT] toss-prep weap=%p a2=%u owner=%p ext=%d (#%d)\n",
			reinterpret_cast<void*>(weapon), static_cast<unsigned>(a2),
			owner, isExtSlot ? 1 : 0, static_cast<int>(n));

	if (isExtSlot && owner)
		OffhandActivation_SetOneHanded(owner, true);

	const int64_t result = v_WeaponTossRelease_orig(weapon, a2);

	// Slot 6 is altHand one-hand: DoToss think never sees it. Tap never
	// cooks. One-hand remaps DoToss to 639; holospray rseqs are ACT_VM_TOSS.
	if (isExtSlot && bridge_offhand_ext_autotoss.GetBool() && v_WeaponDoToss_orig)
	{
		OffhandExt_BindTossStudio(weapon);
		const char tossed = v_WeaponDoToss_orig(weapon);
		char ideal = -1;
		if (v_WeaponSetIdealActivity_orig)
			ideal = v_WeaponSetIdealActivity_orig(weapon, kActVmToss);

		if (sdk_offhand_ext_toss_diag.GetBool() && n <= 8)
		{
			const char* studioName = "";
			int vmodel = 0;
			int nseq = -1;
			const uintptr_t cstudio = *reinterpret_cast<uintptr_t*>(
				reinterpret_cast<uint8_t*>(weapon) + 0xFD8);
			if (cstudio)
			{
				const uintptr_t hdr = *reinterpret_cast<uintptr_t*>(cstudio + 0x08);
				const uintptr_t vm = *reinterpret_cast<uintptr_t*>(cstudio + 0x10);
				vmodel = vm ? 1 : 0;
				if (hdr)
					studioName = reinterpret_cast<const char*>(hdr + 0x10);
				if (vm)
					nseq = *reinterpret_cast<int*>(vm + 0x20);
				else if (hdr)
					nseq = *reinterpret_cast<int*>(hdr + 0xC0);
			}
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] autotoss weap=%p ret=%d ideal=%d studio='%.63s' "
				"vmodel=%d nseq=%d (#%d)\n",
				reinterpret_cast<void*>(weapon), static_cast<int>(tossed),
				static_cast<int>(ideal), studioName, vmodel, nseq,
				static_cast<int>(n));
		}

		// Toss seqs live on ptpov. If bind+SetIdeal still miss, fire 81
		// in-usercmd so the plant is not gated on the studio.
		if (ideal != 1 && v_WeaponAnimEvent_orig)
		{
			if (sdk_offhand_ext_toss_diag.GetBool() && n <= 8)
				Msg(eDLL_T::SERVER,
					"[OFFHAND-EXT] synth-81 weap=%p ideal=%d (#%d)\n",
					reinterpret_cast<void*>(weapon), static_cast<int>(ideal),
					static_cast<int>(n));
			Hook_WeaponAnimEvent(reinterpret_cast<void*>(weapon), 81, 0, 0);
		}
	}

	return result;
}
#endif // !CLIENT_DLL

// Per-selector activate. Early-return if a3 is NULL: slot 6/7 can race m_selectedOffhands still 0xFF.
static void (*v_PerSelectorActivateInner_orig)(int64_t a1, int64_t a2, int64_t a3) = nullptr;

static void Hook_PerSelectorActivateInner(int64_t a1, int64_t a2, int64_t a3)
{
	if (a3 == 0)
	{
		// No weapon to process -- skip the body rather than deref NULL.
		return;
	}
	v_PerSelectorActivateInner_orig(a1, a2, a3);
}

// Per-selector activate driver. Slot 6/7 must not crash on a NULL weapon.
static void (*v_PerSelectorActivateOuter_orig)(int64_t a1, int a2) = nullptr;

static int64_t Hook_SelectedOffhandResolver(int64_t a1, int a2);  // fwd decl

static void Hook_PerSelectorActivateOuter(int64_t a1, int a2)
{
	const uint8_t slotByte = *reinterpret_cast<uint8_t*>(a1 + a2 + 0x16EE);
	const int64_t weapon = Hook_SelectedOffhandResolver(a1, a2);
	if (weapon == 0)
	{
		// Either m_selectedOffhands[k] is 0xFF (no selection) or the slot 6/7
		// extension map is empty / stale. The engine's body would deref NULL
		// at [v4+0x15A8]; skip instead.
		return;
	}

#ifndef CLIENT_DLL
	// Toss-release is event 24 on this activate. Stock path never sees slots 6/7.
	void* savedPred = nullptr;
	bool predArmed = false;
	if (g_pPredictablePlayer && !*g_pPredictablePlayer)
	{
		savedPred = *g_pPredictablePlayer;
		*g_pPredictablePlayer = reinterpret_cast<void*>(a1);
		predArmed = true;
	}
#endif // !CLIENT_DLL

	v_PerSelectorActivateOuter_orig(a1, a2);

#ifndef CLIENT_DLL
	if (predArmed && g_pPredictablePlayer)
		*g_pPredictablePlayer = savedPred;

	if (sdk_offhand_ext_toss_diag.GetBool())
	{
		static volatile LONG s_tossLog = 0;
		const LONG n = InterlockedIncrement(&s_tossLog);
		if (n <= 8)
		{
			const void* handler = *reinterpret_cast<void**>(weapon + 5944);
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] toss-activate player=%p weap=%p handler@1738=%p pred=%d (#%d)\n",
				reinterpret_cast<void*>(a1), reinterpret_cast<void*>(weapon),
				handler, predArmed ? 1 : 0, static_cast<int>(n));
		}
	}

	if ((slotByte == 6 || slotByte == 7) && sdk_offhand_ext_toss_diag.GetBool())
	{
		static volatile LONG s_tossFieldsLog = 0;
		const LONG fn = InterlockedIncrement(&s_tossFieldsLog);
		if (fn <= 8)
		{
			uint8_t* const w = reinterpret_cast<uint8_t*>(weapon);
			uint8_t* const p = reinterpret_cast<uint8_t*>(a1);
			void* const pInfo = *reinterpret_cast<void**>(w + 0x15A8);
			const uint8_t instant = (pInfo)
				? *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(pInfo) + 0x277)
				: 0;
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] toss-fields slot=%u type=%d sel=%d tossArmed=%u tossPending=%u "
				"clip=%d minFire=%d warmTime=%.3f warmActive=%u instant=%u "
				"buttons=0x%X buttons2=0x%X (#%d)\n",
				static_cast<unsigned>(slotByte),
				*reinterpret_cast<int32_t*>(w + 0x2750),
				*reinterpret_cast<int32_t*>(w + 0x2754),
				static_cast<unsigned>(*(w + 0x1298)),
				static_cast<unsigned>(*(w + 0x1299)),
				*reinterpret_cast<int32_t*>(w + 0x1224),
				*reinterpret_cast<int32_t*>(w + 0x122C),
				*reinterpret_cast<float*>(w + 0x1CA4),
				static_cast<unsigned>(*(w + 0x1CC8)),
				static_cast<unsigned>(instant),
				*reinterpret_cast<uint32_t*>(p + 0x60DC),
				*reinterpret_cast<uint32_t*>(p + 0x60E0),
				static_cast<int>(fn));
		}
	}
#endif // !CLIENT_DLL
}

static int64_t Hook_SelectedOffhandResolver(int64_t a1, int a2)
{
	// Slot byte: m_selectedOffhands[a2] at player+0x16EE. Same read shape
	// as the engine's prologue (we're replacing the whole function so we
	// have to duplicate this step).
	const uint8_t slotByte = *reinterpret_cast<uint8_t*>(
		a1 + a2 + 0x16EE);

	if (slotByte == 0xFF)
		return 0;

	// Slot 0..5: engine's native path is still correct, just call through.
	if (slotByte < 6)
		return v_SelectedOffhandResolver_orig(a1, a2);

	// Slot 6/7: resolve via the SDK-owned L1 map.
	if (slotByte != 6 && slotByte != 7)
	{
		// Garbage slot byte written by something that bypassed the gate
		// widening. Log once per offending byte so a corruption pattern
		// shows up in the log without spamming every tick.
		Warning(eDLL_T::ENGINE,
			"[offhand-activate] Hook_SelectedOffhandResolver: player=%p selector=%d"
			"m_selectedOffhands byte=0x%02X out of widened range (0..7)\n",
			reinterpret_cast<void*>(a1), a2, slotByte);
		return 0;
	}

	// Probe both L1 maps. Listen-server ticks both sides.
	ESide side = ESide::Server;
	const uint32_t eh = OffhandSlotsExt_GetAnySide(
		reinterpret_cast<void*>(a1), slotByte, &side);
	if (eh == kOffhandSlotExtInvalidHandle)
	{
		// Double miss: on the server, Give never installed this slot.
		Warning(eDLL_T::ENGINE,
			"[offhand-activate] Hook_SelectedOffhandResolver: player=%p selector=%d"
			"slot=%u -- BOTH maps empty (server AND client miss). Early-"
			"return NULL; hook will short-circuit.\n",
			reinterpret_cast<void*>(a1), a2,
			static_cast<unsigned>(slotByte));
		return 0;
	}

	void* const pEntity = SDKEntityState_Resolve(
		SDKEntityHandle(eh), side);
	if (!pEntity)
	{
		// Stale EHandle: serial mismatch. Treat as empty.
		Warning(eDLL_T::ENGINE,
			"[offhand-activate] Hook_SelectedOffhandResolver: player=%p selector=%d"
			"slot=%u EH=0x%08X side=%d -- resolve returned NULL (stale "
			"serial or destroyed entity). Early-return NULL.\n",
			reinterpret_cast<void*>(a1), a2,
			static_cast<unsigned>(slotByte), eh,
			static_cast<int>(side));
	}
	return reinterpret_cast<int64_t>(pEntity);
}

// Weapon search whose -1 result strands slot 6/7 in m_selectedOffhands and suppresses the viewmodel swap.
static int64_t (*v_WeaponDeploy_orig)(int64_t a1) = nullptr;

// Exposed API: invoke Deploy at Give time. Used by OffhandSlotsExt_Give.
bool OffhandActivation_InvokeDeploy(void* pWeapon)
{
	if (!pWeapon || !v_WeaponDeploy_orig)
		return false;
#ifndef CLIENT_DLL
	const bool diag = sdk_offhand_ext_toss_diag.GetBool();
	char slotsBefore[256];
	char slotsAfter[256];
	void* slot24Before = nullptr;
	if (diag)
	{
		OffhandExt_FormatHandlerSlots(pWeapon, slotsBefore, sizeof(slotsBefore));
		slot24Before = *reinterpret_cast<void**>(
			static_cast<uint8_t*>(pWeapon) + 5752 + 8 * 24);
	}
#endif // !CLIENT_DLL
	v_WeaponDeploy_orig(reinterpret_cast<int64_t>(pWeapon));
#ifndef CLIENT_DLL
	if (diag)
	{
		OffhandExt_FormatHandlerSlots(pWeapon, slotsAfter, sizeof(slotsAfter));
		void* const slot24After = *reinterpret_cast<void**>(
			static_cast<uint8_t*>(pWeapon) + 5752 + 8 * 24);
		const void* def = *reinterpret_cast<void**>(
			static_cast<uint8_t*>(pWeapon) + 5544);
		const void* handler = *reinterpret_cast<void**>(
			static_cast<uint8_t*>(pWeapon) + 5944);
		static volatile LONG s_deployLog = 0;
		const LONG n = InterlockedIncrement(&s_deployLog);
		if (n <= 8)
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] Deploy weap=%p def@15A8=%p handler@1738=%p "
				"slot24 %p -> %p slots [%s] -> [%s] (#%d)\n",
				pWeapon, def, handler, slot24Before, slot24After,
				slotsBefore, slotsAfter, static_cast<int>(n));
	}
#endif // !CLIENT_DLL
	return true;
}

// Pattern addresses. Non-null iff Detour(true) anchored.
namespace
{
	// Client input processor (+0xA98): range gate + bitmask table + sentinel reset.
	uintptr_t g_clientGateAnchor      = 0;  // points at `3C 06`
	uintptr_t g_clientSentinelAnchor  = 0;  // points at `C6 87 80 01 00 00 06`
	uint8_t   g_clientGateSaved       = 0;
	uint8_t   g_clientSentinelSaved   = 0;
	int32_t   g_clientTableDispSaved  = 0;

	// Server dispatcher: loop gates, 8-entry bitmask (bits 25/26), offhandWeapons[slot] trampolines.
	uintptr_t g_serverTableLea        = 0;  // points at `4C 8D 2D`
	int32_t   g_serverTableDispSaved  = 0;
	uintptr_t g_serverLoop1Anchor     = 0;  // points at `83 FE 06`
	uint8_t   g_serverLoop1GateSaved  = 0;
	uint8_t   g_serverLoop1ReadSaved[7]  = {};
	uintptr_t g_serverLoop1EndAnchor  = 0;  // points at `48 FF C6 48 83 FE 06`
	uint8_t   g_serverLoop1EndSaved   = 0;

	// Second-loop holster/deactivation gate widening.
	uintptr_t g_serverLoop2Anchor     = 0;  // points at `41 83 FE 06`
	uint8_t   g_serverLoop2GateSaved  = 0;
	uint8_t   g_serverLoop2ReadSaved[7]  = {};
	uintptr_t g_serverLoop2EndAnchor  = 0;  // points at `49 FF C6 4C 8D 3D`
	uint8_t   g_serverLoop2EndSaved   = 0;

	uintptr_t g_serverLoop3Anchor     = 0;  // points at `80 FB 06`
	uint8_t   g_serverLoop3GateSaved  = 0;
	uint8_t   g_serverLoop3ReadSaved[8]  = {};

	// Outer-gate widening. Mask must include bit 26 for slot 7.
	uintptr_t g_serverOuterGateAnchor = 0;   // points at `F7 81 DC 60 00 00`
	uint32_t  g_serverOuterGateSaved  = 0;

	// Search function whose -1 result would strand slot 6/7.
	uintptr_t g_searchAnchor          = 0;
	uint8_t   g_searchGateSaved       = 0;
	uint8_t   g_searchReadSaved[5]    = {};
	uintptr_t g_searchEndAnchor       = 0;
	uint8_t   g_searchEndSaved        = 0;

#ifndef CLIENT_DLL
	// Inventory offhand-slot search (0..5 -> 0..7). r10=inventory at read.
	// gate @+0x02 (06 -> 08); read @+0x08 (5 bytes -> JMP); end @+0x06
	uintptr_t g_invSearchAnchor       = 0;
	uint8_t   g_invSearchGateSaved    = 0;
	uint8_t   g_invSearchReadSaved[5] = {};
	uintptr_t g_invSearchEndAnchor    = 0;
	uint8_t   g_invSearchEndSaved     = 0;

	// Per-slot offhand button state (0..5 -> 0..7 + 8-entry table + mask).
	// gate @+0x03; read @+0x06 (8 bytes -> JMP+3 NOP); table LEA disp32 @+3;
	// mask imm32 @+3 (0x03F00000 -> 0x07F00000)
	uintptr_t g_btnStateAnchor        = 0;
	uint8_t   g_btnStateGateSaved     = 0;
	uint8_t   g_btnStateReadSaved[8]  = {};
	uintptr_t g_btnStateTableLea      = 0;
	int32_t   g_btnStateTableDispSaved = 0;
	uintptr_t g_btnStateMaskAnchor    = 0;
	uint32_t  g_btnStateMaskSaved     = 0;
#endif // !CLIENT_DLL

	// Near-allocated page. 4096 bytes; bitmask table @ [0..0x20],
	// trampolines grow from 0x20.
	uint8_t*  g_pNearPage             = nullptr;
	uint32_t* g_pNearBitmaskTable     = nullptr; // = (uint32_t*)(g_pNearPage + 0)

	bool      g_patchesAttached       = false;
}

// 4KB near-alloc within +-2GB of nearAddr so RIP-relative displacements fit.
static uint8_t* AllocNearPage(void* nearAddr)
{
	uintptr_t base = reinterpret_cast<uintptr_t>(nearAddr);
	uintptr_t lo = (base > 0x70000000) ? base - 0x70000000 : 0x10000;
	uintptr_t hi = base + 0x70000000;

	for (uintptr_t addr = lo; addr < hi; addr += 0x10000)
	{
		void* p = VirtualAlloc(reinterpret_cast<void*>(addr),
			4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (p)
			return static_cast<uint8_t*>(p);
	}
	return nullptr;
}

// Emit a trampoline stub at dst. Returns the address one past the stub.
enum StubKind : int
{
	STUB_SERVER_LOOP1 = 0,  // first loop (7 bytes replaced)
	STUB_SERVER_LOOP3 = 1,  // third loop (8 bytes replaced)
	STUB_SEARCH       = 2,  // search (5 bytes replaced)
	STUB_SERVER_LOOP2 = 3,  // second loop (7 bytes replaced)
#ifndef CLIENT_DLL
	STUB_INV_SEARCH   = 4,  // inv-search read (5 bytes); rax is loop counter
	STUB_BTN_STATE    = 5,  // button-state read (8 bytes); rcx=player, r9=slot
#endif // !CLIENT_DLL
};

static uint8_t* EmitStub(uint8_t* dst, StubKind kind,
                         uint8_t* returnAddr, const void* helperFn)
{
	std::vector<uint8_t> bytes;
	auto put = [&](std::initializer_list<uint8_t> b)
		{ for (uint8_t x : b) bytes.push_back(x); };
	auto put_imm64 = [&](uint64_t v) {
		for (int i = 0; i < 8; ++i)
			bytes.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
	};

#ifndef CLIENT_DLL
	// STUB_INV_SEARCH: rax is the live loop counter and MUST be preserved.
	// Frame: 5 pushes (40) + sub rsp 0x28 (40) = 80, RSP%16==0 at call.
	if (kind == STUB_INV_SEARCH)
	{
		put({0x50});                              // push rax
		put({0x41, 0x50});                        // push r8
		put({0x41, 0x51});                        // push r9
		put({0x41, 0x52});                        // push r10
		put({0x41, 0x53});                        // push r11
		put({0x48, 0x83, 0xEC, 0x28});            // sub rsp, 0x28
		put({0x48, 0x8B, 0xD1});                  // mov rdx, rcx  (slot)
		// lea rcx, [r10-0x1688] -- inventory is player+0x1688, embedded
		put({0x49, 0x8D, 0x8A, 0x78, 0xE9, 0xFF, 0xFF});
		put({0x4D, 0x8B, 0xC2});                  // mov r8, r10   (inventory)
		put({0x48, 0xB8});
		put_imm64(reinterpret_cast<uint64_t>(helperFn));
		put({0xFF, 0xD0});                        // call rax
		put({0x48, 0x83, 0xC4, 0x28});            // add rsp, 0x28
		put({0x8B, 0xD0});                        // mov edx, eax
		put({0x41, 0x5B});                        // pop r11
		put({0x41, 0x5A});                        // pop r10
		put({0x41, 0x59});                        // pop r9
		put({0x41, 0x58});                        // pop r8
		put({0x58});                              // pop rax
		const uintptr_t jmpFrom = reinterpret_cast<uintptr_t>(dst) + bytes.size();
		const int64_t rel = static_cast<int64_t>(
			reinterpret_cast<uintptr_t>(returnAddr) - (jmpFrom + 5));
		put({0xE9});
		int32_t rel32 = static_cast<int32_t>(rel);
		for (int i = 0; i < 4; ++i)
			bytes.push_back(static_cast<uint8_t>((rel32 >> (8 * i)) & 0xFF));
		std::memcpy(dst, bytes.data(), bytes.size());
		return dst + bytes.size();
	}

	// STUB_BTN_STATE: rcx is the live player and must be preserved.
	if (kind == STUB_BTN_STATE)
	{
		put({0x51});                              // push rcx
		put({0x41, 0x50});                        // push r8
		put({0x41, 0x51});                        // push r9
		put({0x41, 0x52});                        // push r10
		put({0x41, 0x53});                        // push r11
		put({0x48, 0x83, 0xEC, 0x28});            // sub rsp, 0x28
		put({0x4C, 0x89, 0xCA});                  // mov rdx, r9  (slot; rcx = player)
		put({0x4D, 0x33, 0xC0});                  // xor r8, r8
		put({0x48, 0xB8});
		put_imm64(reinterpret_cast<uint64_t>(helperFn));
		put({0xFF, 0xD0});                        // call rax
		put({0x48, 0x83, 0xC4, 0x28});            // add rsp, 0x28
		put({0x8B, 0xD0});                        // mov edx, eax
		put({0x41, 0x5B});                        // pop r11
		put({0x41, 0x5A});                        // pop r10
		put({0x41, 0x59});                        // pop r9
		put({0x41, 0x58});                        // pop r8
		put({0x59});                              // pop rcx
		const uintptr_t jmpFrom = reinterpret_cast<uintptr_t>(dst) + bytes.size();
		const int64_t rel = static_cast<int64_t>(
			reinterpret_cast<uintptr_t>(returnAddr) - (jmpFrom + 5));
		put({0xE9});
		int32_t rel32 = static_cast<int32_t>(rel);
		for (int i = 0; i < 4; ++i)
			bytes.push_back(static_cast<uint8_t>((rel32 >> (8 * i)) & 0xFF));
		std::memcpy(dst, bytes.data(), bytes.size());
		return dst + bytes.size();
	}
#endif // !CLIENT_DLL

	// Preserve only the volatile regs the engine reuses across the helper call.
	put({0x41, 0x50});        // push r8
	put({0x41, 0x51});        // push r9
	put({0x41, 0x52});        // push r10
	put({0x41, 0x53});        // push r11
	const uint8_t shadowSize = 0x20;
	put({0x48, 0x83, 0xEC, shadowSize});    // sub rsp, 0x20

	// Marshal rcx=player, rdx=slot, r8=inventory or 0.
	if (kind == STUB_SERVER_LOOP1 || kind == STUB_SERVER_LOOP2)
	{
		// rdi = player, rax = slot (int64 signed from movsxd rax, esi/r14d).
		put({0x48, 0x8B, 0xCF});        // mov rcx, rdi
		put({0x48, 0x8B, 0xD0});        // mov rdx, rax
		put({0x4D, 0x33, 0xC0});        // xor r8, r8
	}
	else if (kind == STUB_SERVER_LOOP3)
	{
		// rdi = player, bl = slot byte (signed), rcx = WeaponInventory.
		// Copy rcx -> r8 BEFORE clobbering rcx with the player.
		// `mov r8, rcx` = REX.WR (0x4C) + 8B /r + ModR/M C1 => 4C 8B C1.
		put({0x4C, 0x8B, 0xC1});              // mov r8, rcx (inventory)
		put({0x48, 0x8B, 0xCF});              // mov rcx, rdi (player)
		put({0x48, 0x0F, 0xBE, 0xD3});        // movsx rdx, bl (slot)
	}
	else // STUB_SEARCH
	{
		// r14 = player, rax = slot (already sign-ext via movsxd rax, ebx),
		// r8 = WeaponInventory. r8 was pushed but unmodified => still valid
		// as arg3 for the helper call.
		put({0x49, 0x8B, 0xCE});              // mov rcx, r14 (player)
		put({0x48, 0x8B, 0xD0});              // mov rdx, rax (slot)
	}

	// --- mov rax, helper; call rax
	put({0x48, 0xB8});
	put_imm64(reinterpret_cast<uint64_t>(helperFn));
	put({0xFF, 0xD0});

	// Restore shadow, then move the return into the target reg. Order matters.
	put({0x48, 0x83, 0xC4, shadowSize});    // add rsp, 0x20
	if (kind == STUB_SEARCH || kind == STUB_SERVER_LOOP2)
		put({0x8B, 0xC8});            // mov ecx, eax
	else
		put({0x8B, 0xD0});            // mov edx, eax

	// --- Epilogue: pop in reverse order.
	put({0x41, 0x5B});                // pop r11
	put({0x41, 0x5A});                // pop r10
	put({0x41, 0x59});                // pop r9
	put({0x41, 0x58});                // pop r8

	// --- Jmp back to engine instruction AFTER the replaced bytes.
	const uintptr_t jmpFrom = reinterpret_cast<uintptr_t>(dst) + bytes.size();
	const int64_t rel = static_cast<int64_t>(
		reinterpret_cast<uintptr_t>(returnAddr) - (jmpFrom + 5));
	put({0xE9});
	int32_t rel32 = static_cast<int32_t>(rel);
	for (int i = 0; i < 4; ++i)
		bytes.push_back(static_cast<uint8_t>((rel32 >> (8 * i)) & 0xFF));

	std::memcpy(dst, bytes.data(), bytes.size());
	return dst + bytes.size();
}

//-----------------------------------------------------------------------------
// VOffhandActivationPatches ---------------------------
//-----------------------------------------------------------------------------
void VOffhandActivationPatches::GetAdr(void) const
{
	LogVarAdr("OffhandActivationPatches::NearPage",       g_pNearPage);
	LogVarAdr("OffhandActivationPatches::BitmaskTable",   g_pNearBitmaskTable);
	LogVarAdr("OffhandActivationPatches::ClientGate",
		reinterpret_cast<const void*>(g_clientGateAnchor));
	LogVarAdr("OffhandActivationPatches::ClientSentinel",
		reinterpret_cast<const void*>(g_clientSentinelAnchor));
	LogVarAdr("OffhandActivationPatches::ServerTableLea",
		reinterpret_cast<const void*>(g_serverTableLea));
	LogVarAdr("OffhandActivationPatches::ServerLoop1",
		reinterpret_cast<const void*>(g_serverLoop1Anchor));
	LogVarAdr("OffhandActivationPatches::ServerLoop1End",
		reinterpret_cast<const void*>(g_serverLoop1EndAnchor));
	LogVarAdr("OffhandActivationPatches::ServerLoop2",
		reinterpret_cast<const void*>(g_serverLoop2Anchor));
	LogVarAdr("OffhandActivationPatches::ServerLoop2End",
		reinterpret_cast<const void*>(g_serverLoop2EndAnchor));
	LogVarAdr("OffhandActivationPatches::ServerOuterGate",
		reinterpret_cast<const void*>(g_serverOuterGateAnchor));
	LogVarAdr("OffhandActivationPatches::ServerLoop3",
		reinterpret_cast<const void*>(g_serverLoop3Anchor));
	LogVarAdr("OffhandActivationPatches::SearchAnchor",
		reinterpret_cast<const void*>(g_searchAnchor));
	LogVarAdr("OffhandActivationPatches::SearchEnd",
		reinterpret_cast<const void*>(g_searchEndAnchor));
#ifndef CLIENT_DLL
	LogVarAdr("OffhandActivationPatches::InvSearch",
		reinterpret_cast<const void*>(g_invSearchAnchor));
	LogVarAdr("OffhandActivationPatches::InvSearchEnd",
		reinterpret_cast<const void*>(g_invSearchEndAnchor));
	LogVarAdr("OffhandActivationPatches::BtnState",
		reinterpret_cast<const void*>(g_btnStateAnchor));
	LogVarAdr("OffhandActivationPatches::BtnStateTableLea",
		reinterpret_cast<const void*>(g_btnStateTableLea));
	LogVarAdr("OffhandActivationPatches::BtnStateMask",
		reinterpret_cast<const void*>(g_btnStateMaskAnchor));
	LogVarAdr("OffhandActivationPatches::ScriptContext",
		reinterpret_cast<const void*>(g_pScriptContext));
	LogFunAdr("(offhand dispatch)",
		v_OffhandDispatch_orig);
	LogFunAdr("(weapon toss prep)",
		v_WeaponTossRelease_orig);
	LogFunAdr("(weapon DoToss)",
		v_WeaponDoToss_orig);
	LogFunAdr("(weapon SetIdealActivity)",
		v_WeaponSetIdealActivity_orig);
	LogFunAdr("(entity SetModel)",
		v_EntitySetModel_orig);
	LogFunAdr("(CPlayer SetOneHandedOn)",
		v_CPlayer_SetOneHandedOn);
	LogFunAdr("(CPlayer SetOneHandedOff)",
		v_CPlayer_SetOneHandedOff);
	LogFunAdr("(player weapon frame)",
		v_PlayerWeaponFrame_orig);
	LogFunAdr("(weapon anim-event dispatch)",
		v_WeaponAnimEventDispatch_orig);
	LogFunAdr("(weapon anim event)",
		v_WeaponAnimEvent_orig);
#endif // !CLIENT_DLL
	LogFunAdr("(selected-offhand resolver)",
		v_SelectedOffhandResolver_orig);
	LogFunAdr("(per-selector activate inner)",
		v_PerSelectorActivateInner_orig);
	LogFunAdr("(per-selector activate outer)",
		v_PerSelectorActivateOuter_orig);
	LogFunAdr("(CWeapon::Deploy)",
		v_WeaponDeploy_orig);
}

void VOffhandActivationPatches::GetFun(void) const
{
	// Client r5apex .text matches these anchors. Dedi uses the server set.
	CMemory clientGate = Module_FindPattern(g_GameDll,
		"3C 06 73 0D 0F B6 C0 8B 84 81");
	g_clientGateAnchor = clientGate ? clientGate.GetPtr() : 0;

	// Client reset-sentinel (same function, later instruction)
	CMemory clientSentinel = Module_FindPattern(g_GameDll,
		"45 33 C9 C6 87 80 01 00 00 06");
	g_clientSentinelAnchor = clientSentinel ? clientSentinel.GetPtr() : 0;

	// --- Server dispatcher: table LEA + first-loop + first-loop-end
	// + third-loop read ---
	CMemory serverTableLea = Module_FindPattern(g_GameDll,
		"4C 8D 2D ?? ?? ?? ?? 41 C1 EC 19 41 80 E4 01");
	g_serverTableLea = serverTableLea ? serverTableLea.GetPtr() : 0;

	CMemory serverLoop1 = Module_FindPattern(g_GameDll,
		"83 FE 06 0F 83 ?? ?? ?? ?? 48 63 C6 8B 94 87 B4 16 00 00");
	g_serverLoop1Anchor = serverLoop1 ? serverLoop1.GetPtr() : 0;

	// Backward-jump rel32 `71 FE FF FF` disambiguates this site.
	CMemory serverLoop1End = Module_FindPattern(g_GameDll,
		"48 FF C6 48 83 FE 06 0F 8C 71 FE FF FF");
	g_serverLoop1EndAnchor = serverLoop1End ? serverLoop1End.GetPtr() : 0;

	// --- Second loop (holster/deactivation) gate + read + end ---

	CMemory serverLoop2 = Module_FindPattern(g_GameDll,
		"41 83 FE 06 0F 83 ?? ?? ?? ?? 49 63 C6 8B 8C 87 B4 16 00 00");
	g_serverLoop2Anchor = serverLoop2 ? serverLoop2.GetPtr() : 0;

	// Unique 15-byte pattern including LEA r15 reload + cmp + jl back-edge.
	CMemory serverLoop2End = Module_FindPattern(g_GameDll,
		"49 FF C6 4C 8D 3D ?? ?? ?? ?? 49 83 FE 06 0F 8C");
	g_serverLoop2EndAnchor = serverLoop2End ? serverLoop2End.GetPtr() : 0;

	// Outer-gate anchor: `test [rcx+60DCh], 0x03F00000`. Widen to 0x07F00000
	// so slot 7 (bit 26) can enter the first loop.
	CMemory serverOuterGate = Module_FindPattern(g_GameDll,
		"F7 81 DC 60 00 00 00 00 F0 03");
	g_serverOuterGateAnchor = serverOuterGate ? serverOuterGate.GetPtr() : 0;

	CMemory serverLoop3 = Module_FindPattern(g_GameDll,
		"80 FB 06 73 27 48 0F BE C3 8B 54 81 2C");
	g_serverLoop3Anchor = serverLoop3 ? serverLoop3.GetPtr() : 0;

	// --- search function ---
	CMemory searchSite = Module_FindPattern(g_GameDll,
		"83 FB 06 73 27 48 63 C3 41 8B 4C 80 2C");
	g_searchAnchor = searchSite ? searchSite.GetPtr() : 0;

	// Backward-jump rel8 `C4` (jl back to loop top).
	CMemory searchEnd = Module_FindPattern(g_GameDll,
		"48 FF C3 48 83 FB 06 7C C4");
	g_searchEndAnchor = searchEnd ? searchEnd.GetPtr() : 0;

	// ---: m_selectedOffhands[k] -> CWeapon* helper ---

#ifndef CLIENT_DLL
	{
		// Offset(24) is the predictable-player store (`48 89 3D` rel32).
		const CMemory predWrite = Module_FindPattern(g_GameDll,
			"48 89 9F 78 65 00 00 8B 83 84 01 00 00");
		void** byPattern = nullptr;
		if (predWrite)
			byPattern = predWrite.Offset(24)
				.ResolveRelativeAddress(3, 7).RCast<void**>();

		const uintptr_t moduleBase =
			static_cast<uintptr_t>(g_GameDll.GetModuleBase());
		void** const byRva = moduleBase
			? reinterpret_cast<void**>(moduleBase + (0x14D4EB6F0ull - 0x140000000ull))
			: nullptr;

		if (byPattern && byRva && byPattern == byRva)
		{
			g_pPredictablePlayer = byPattern;
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] predictable-player global @ %p\n",
				reinterpret_cast<void*>(g_pPredictablePlayer));
		}
		else
		{
			g_pPredictablePlayer = nullptr;
			Warning(eDLL_T::ENGINE,
				"[OFFHAND-EXT] predictable-player global REJECTED pat=%p rva=%p -- "
				"stock toss FireWeaponGrenade will reject outside usercmd simulate\n",
				reinterpret_cast<void*>(byPattern),
				reinterpret_cast<void*>(byRva));
		}
	}

	{
		// mov rax, cs:script-context at anchor+13 (48 8B 05 rel32).
		const CMemory ctxLoad = Module_FindPattern(g_GameDll,
			"48 39 AF 38 17 00 00 0F 84 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ??");
		void** byPattern = nullptr;
		if (ctxLoad)
			byPattern = ctxLoad.Offset(13)
				.ResolveRelativeAddress(3, 7).RCast<void**>();

		const uintptr_t moduleBase =
			static_cast<uintptr_t>(g_GameDll.GetModuleBase());
		void** const byRva = moduleBase
			? reinterpret_cast<void**>(moduleBase + (0x14D4EA680ull - 0x140000000ull))
			: nullptr;

		if (byPattern && byRva && byPattern == byRva)
		{
			g_pScriptContext = byPattern;
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] script-context global @ %p\n",
				reinterpret_cast<void*>(g_pScriptContext));
		}
		else
		{
			g_pScriptContext = nullptr;
			Warning(eDLL_T::ENGINE,
				"[OFFHAND-EXT] script-context global REJECTED pat=%p rva=%p -- "
				"animevent24 gate dump will print ctx=null\n",
				reinterpret_cast<void*>(byPattern),
				reinterpret_cast<void*>(byRva));
		}
	}

	// Unique via frame + CPlayer+0x6128 load. Client twin uses a different
	// displacement so this does not hit the listen-server client half.
	Module_FindPattern(g_GameDll,
		"4C 8B DC 55 57 49 8D 6B A1 48 81 EC F8 00 00 00 8B 81 28 61 00 00")
		.GetPtr(v_OffhandDispatch_orig);
	if (!v_OffhandDispatch_orig)
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: offhand dispatch prologue unresolved -- "
			"slot 6/7 TrySelectOffhand will not survive usercmd overwrite\n");

	// Inventory offhand-slot search: gate + read + loop end.
	// Trailing read bytes keep this site unique from the structurally
	// identical search already widened via g_searchAnchor.
	CMemory invSearch = Module_FindPattern(g_GameDll,
		"83 F8 06 73 27 48 63 C8 41 8B 54 8A 2C");
	g_invSearchAnchor = invSearch ? invSearch.GetPtr() : 0;

	CMemory invSearchEnd = Module_FindPattern(g_GameDll,
		"48 FF C0 48 83 F8 06 7C C3");
	g_invSearchEndAnchor = invSearchEnd ? invSearchEnd.GetPtr() : 0;

	// Per-slot offhand button state: gate+read, table LEA, outer mask.
	CMemory btnState = Module_FindPattern(g_GameDll,
		"41 83 F9 06 73 2E 42 8B 94 89 B4 16 00 00");
	g_btnStateAnchor = btnState ? btnState.GetPtr() : 0;

	CMemory btnStateTable = Module_FindPattern(g_GameDll,
		"48 8D 15 ?? ?? ?? ?? 42 8B 14 8A 41 23 D0 75 11");
	g_btnStateTableLea = btnStateTable ? btnStateTable.GetPtr() : 0;

	CMemory btnStateMask = Module_FindPattern(g_GameDll,
		"41 F7 C0 00 00 F0 03 74 93");
	g_btnStateMaskAnchor = btnStateMask ? btnStateMask.GetPtr() : 0;

	// TossPrep dispatch (idx 24). Trailing `8B 97 E0 0F 00 00` is the
	// server offset that separates this site from its client twin.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 "
		"0F B6 F2 48 8B F9 0F 29 74 24 30 8D 9E 20 02 00 00 8B D3 "
		"E8 ?? ?? ?? ?? 3B C3 74 0E 8B D0 48 8B CF E8 ?? ?? ?? ?? "
		"84 C0 75 0A 8B D3 48 8B CF E8 ?? ?? ?? ?? 8B 97 E0 0F 00 00")
		.GetPtr(v_WeaponTossRelease_orig);
	if (!v_WeaponTossRelease_orig)
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: weapon TossPrep prologue unresolved -- "
			"slot 6/7 tap will not start DoToss\n");

	// DoToss. Unique via stack 0x70 + owner EHandle at +0x11F0.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 70 "
		"48 8B D9 33 FF 8B 89 F0 11 00 00")
		.GetPtr(v_WeaponDoToss_orig);
	if (!v_WeaponDoToss_orig)
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: weapon DoToss prologue unresolved -- "
			"slot 6/7 tap will not play ACT_VM_TOSS\n");

	// SetIdealWeaponActivity. Unique via studio-hdr null check + 0F BF 91.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 7C 24 ?? 55 48 8B EC 48 83 EC ?? "
		"48 83 B9 ?? ?? ?? ?? ?? 8B FA 48 8B D9 75 ?? 0F BF 91")
		.GetPtr(v_WeaponSetIdealActivity_orig);
	if (!v_WeaponSetIdealActivity_orig)
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: SetIdealWeaponActivity unresolved -- "
			"slot 6/7 will not select ACT_VM_TOSS\n");

	// CBaseEntity::SetModel(const char*). Unique on the dedi (server half;
	// client twin uses a different stack-arg spill). m_nModelIndex at +0xDE.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? "
		"57 48 83 EC 20 33 FF 48 8B DA 48 8B F1")
		.GetPtr(v_EntitySetModel_orig);
	if (!v_EntitySetModel_orig)
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: CBaseEntity::SetModel unresolved -- "
			"slot 6/7 cannot bind the toss viewmodel studio\n");

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 80 B9 90 5C 00 00 01 48 8B D9 74 1A")
		.GetPtr(v_CPlayer_SetOneHandedOn);
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 80 B9 90 5C 00 00 00 48 8B D9 74 1A")
		.GetPtr(v_CPlayer_SetOneHandedOff);
	if (!v_CPlayer_SetOneHandedOn)
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: SetOneHandedOn unresolved -- "
			"slot 6/7 will not publish m_oneHandedWeaponUsage\n");

	// CPlayer weapon frame. Re-applies the select latch at entry.
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 54 41 55 "
		"41 56 41 57 48 81 EC D0 00 00 00 80 B9 99 04 00 00 00 48 8B F9")
		.GetPtr(v_PlayerWeaponFrame_orig);
	if (!v_PlayerWeaponFrame_orig)
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: player weapon-frame prologue unresolved -- "
			"slot 6/7 button hold may miss the weapon-frame ordering\n");

	// Weapon script-callback dispatcher. Diagnostic-only for event 24 gate dump.
	Module_FindPattern(g_GameDll,
		"4C 8B DC 49 89 6B 10 49 89 73 18 57 48 83 EC 60 48 63 C2 49 8B E9 "
		"48 8B F9 48 8D 34 C1 48 83 BE 78 16 00 00 00 75 10")
		.GetPtr(v_WeaponAnimEventDispatch_orig);
	if (!v_WeaponAnimEventDispatch_orig)
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: weapon anim-event dispatch unresolved -- "
			"animevent24 gate dump will not attach\n");

	// Weapon animation-event handler (switch on animEventId; 81 = toss release).
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 40 8D 42 B8 48 8B D9 83 F8 2B 0F 87 ?? ?? ?? ?? 48 89 74")
		.GetPtr(v_WeaponAnimEvent_orig);
	if (!v_WeaponAnimEvent_orig)
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: weapon anim event unresolved -- "
			"animevent id tap will not attach\n");
#endif // !CLIENT_DLL

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 63 C2 0F B6 9C 08 EE 16 00 00")
		.GetPtr(v_SelectedOffhandResolver_orig);

	// Per-selector weapon-activate routine.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 01 49 8B D8 48 8B F9 FF 90 40 0A 00 00")
		.GetPtr(v_PerSelectorActivateInner_orig);

	// Per-selector activate driver.
	Module_FindPattern(g_GameDll,
		"89 54 24 10 53 57 41 54 41 57 48 83 EC 28")
		.GetPtr(v_PerSelectorActivateOuter_orig);

	// CWeapon::Deploy. Slot 6/7 Give must call this so weapon+5752 handler table is populated.
	Module_FindPattern(g_GameDll,
		"48 8B C4 53 57 48 81 EC B8 00 00 00 48 89 68 10 48 8D 50 08 "
		"48 89 70 18 48 8B D9")
		.GetPtr(v_WeaponDeploy_orig);

	// Log every resolved site. A silent miss leaves slots 6/7 on the native 6-slot path.
	{
		struct AnchorCheck { const char* name; uintptr_t addr; };
		const AnchorCheck anchors[] = {
			{ "clientGate (CInput processor gate)",        g_clientGateAnchor },
			{ "clientSentinel (CInput reset sentinel)",     g_clientSentinelAnchor },
			{ "serverTableLea (dispatcher bitmask table)",  g_serverTableLea },
			{ "serverLoop1 (first-loop gate+read)",         g_serverLoop1Anchor },
			{ "serverLoop1End (first-loop end gate)",       g_serverLoop1EndAnchor },
			{ "serverLoop2 (holster-loop gate+read)",       g_serverLoop2Anchor },
			{ "serverLoop2End (holster-loop end gate)",     g_serverLoop2EndAnchor },
			{ "serverOuterGate (m_nButtons mask test)",     g_serverOuterGateAnchor },
			{ "serverLoop3 (third-loop gate+read)",         g_serverLoop3Anchor },
			{ "searchAnchor ( gate+read)",     g_searchAnchor },
			{ "searchEndAnchor ( end gate)",   g_searchEndAnchor },
#ifndef CLIENT_DLL
			{ "invSearch (inventory offhand-slot search gate+read)", g_invSearchAnchor },
			{ "invSearchEnd (inventory offhand-slot search end)",    g_invSearchEndAnchor },
			{ "btnState (per-slot button-state gate+read)",          g_btnStateAnchor },
			{ "btnStateTableLea (per-slot button-state table LEA)",  g_btnStateTableLea },
			{ "btnStateMask (per-slot button-state mask)",           g_btnStateMaskAnchor },
#endif // !CLIENT_DLL
		};
		const AnchorCheck hooks[] = {
			{ "(selected-offhand resolver)",  reinterpret_cast<uintptr_t>(v_SelectedOffhandResolver_orig) },
			{ "(per-selector activate inner)",reinterpret_cast<uintptr_t>(v_PerSelectorActivateInner_orig) },
			{ "(per-selector activate outer)",reinterpret_cast<uintptr_t>(v_PerSelectorActivateOuter_orig) },
			{ "(CWeapon::Deploy)",            reinterpret_cast<uintptr_t>(v_WeaponDeploy_orig) },
#ifndef CLIENT_DLL
			{ "(offhand dispatch)",           reinterpret_cast<uintptr_t>(v_OffhandDispatch_orig) },
			{ "(weapon toss prep)",           reinterpret_cast<uintptr_t>(v_WeaponTossRelease_orig) },
			{ "(weapon DoToss)",              reinterpret_cast<uintptr_t>(v_WeaponDoToss_orig) },
			{ "(weapon SetIdealActivity)",    reinterpret_cast<uintptr_t>(v_WeaponSetIdealActivity_orig) },
			{ "(entity SetModel)",            reinterpret_cast<uintptr_t>(v_EntitySetModel_orig) },
			{ "(CPlayer SetOneHandedOn)",     reinterpret_cast<uintptr_t>(v_CPlayer_SetOneHandedOn) },
			{ "(player weapon frame)",        reinterpret_cast<uintptr_t>(v_PlayerWeaponFrame_orig) },
			{ "(weapon anim-event dispatch)", reinterpret_cast<uintptr_t>(v_WeaponAnimEventDispatch_orig) },
			{ "(weapon anim event)",          reinterpret_cast<uintptr_t>(v_WeaponAnimEvent_orig) },
#endif // !CLIENT_DLL
		};

		int missCount = 0;
		for (const AnchorCheck& a : anchors)
		{
			if (!a.addr)
			{
				Warning(eDLL_T::ENGINE,
					"[offhand-patches] PATTERN MISS (anchor): %s\n", a.name);
				missCount++;
			}
		}
		for (const AnchorCheck& h : hooks)
		{
			if (!h.addr)
			{
				Warning(eDLL_T::ENGINE,
					"[offhand-patches] PATTERN MISS (hook): %s\n", h.name);
				missCount++;
			}
		}

		const int total = static_cast<int>(std::size(anchors) + std::size(hooks));
		Msg(eDLL_T::ENGINE,
			"[offhand-patches] GetFun() resolution: %d/%d sites resolved "
			"(%d missing) on this build.\n",
			total - missCount, total, missCount);
	}
}

void VOffhandActivationPatches::GetVar(void) const
{
	// Bitmask tables are embedded; nothing else to resolve here.
}

//-----------------------------------------------------------------------------
// Attach / detach --------------------------------
//-----------------------------------------------------------------------------
static void PatchByte(uintptr_t addr, uint8_t newValue, uint8_t* saved)
{
	DWORD oldProtect = 0;
	VirtualProtect(reinterpret_cast<void*>(addr), 1,
		PAGE_EXECUTE_READWRITE, &oldProtect);
	*saved = *reinterpret_cast<uint8_t*>(addr);
	*reinterpret_cast<uint8_t*>(addr) = newValue;
	VirtualProtect(reinterpret_cast<void*>(addr), 1, oldProtect, &oldProtect);
}

static void PatchBytes(uintptr_t addr, const uint8_t* newBytes,
                       size_t count, uint8_t* saved)
{
	DWORD oldProtect = 0;
	VirtualProtect(reinterpret_cast<void*>(addr), count,
		PAGE_EXECUTE_READWRITE, &oldProtect);
	std::memcpy(saved, reinterpret_cast<void*>(addr), count);
	std::memcpy(reinterpret_cast<void*>(addr), newBytes, count);
	VirtualProtect(reinterpret_cast<void*>(addr), count,
		oldProtect, &oldProtect);
}

static void PatchDisp32(uintptr_t addr, int32_t newDisp, int32_t* saved)
{
	DWORD oldProtect = 0;
	VirtualProtect(reinterpret_cast<void*>(addr), 4,
		PAGE_EXECUTE_READWRITE, &oldProtect);
	std::memcpy(saved, reinterpret_cast<void*>(addr), 4);
	std::memcpy(reinterpret_cast<void*>(addr), &newDisp, 4);
	VirtualProtect(reinterpret_cast<void*>(addr), 4,
		oldProtect, &oldProtect);
}

void VOffhandActivationPatches::Detour(const bool bAttach) const
{
	if (!bAttach)
	{
		if (!g_patchesAttached)
			return;

		// Detach Detours first so mid-tick calls stop entering the hooks.
#ifndef CLIENT_DLL
		if (v_WeaponAnimEvent_orig)
			DetourSetup(&v_WeaponAnimEvent_orig,
				&Hook_WeaponAnimEvent, /*bAttach=*/false);
		if (v_WeaponAnimEventDispatch_orig)
			DetourSetup(&v_WeaponAnimEventDispatch_orig,
				&Hook_WeaponAnimEventDispatch, /*bAttach=*/false);
		if (v_PlayerWeaponFrame_orig)
			DetourSetup(&v_PlayerWeaponFrame_orig,
				&Hook_PlayerWeaponFrame, /*bAttach=*/false);
		if (v_WeaponTossRelease_orig)
			DetourSetup(&v_WeaponTossRelease_orig,
				&Hook_WeaponTossRelease, /*bAttach=*/false);
		if (v_OffhandDispatch_orig)
			DetourSetup(&v_OffhandDispatch_orig,
				&Hook_OffhandDispatch, /*bAttach=*/false);
#endif // !CLIENT_DLL
		if (v_PerSelectorActivateOuter_orig)
			DetourSetup(&v_PerSelectorActivateOuter_orig,
				&Hook_PerSelectorActivateOuter, /*bAttach=*/false);
		if (v_PerSelectorActivateInner_orig)
			DetourSetup(&v_PerSelectorActivateInner_orig,
				&Hook_PerSelectorActivateInner, /*bAttach=*/false);
		if (v_SelectedOffhandResolver_orig)
			DetourSetup(&v_SelectedOffhandResolver_orig,
				&Hook_SelectedOffhandResolver, /*bAttach=*/false);

		// --- Restore original bytes (reverse order of attach) ---
		uint8_t tmp = 0;
#ifndef CLIENT_DLL
		// Seven new sites, reverse attach order:
		// mask, table LEA, btn read, btn gate, inv end, inv read, inv gate.
		if (g_btnStateMaskAnchor)
		{
			const uintptr_t immAddr = g_btnStateMaskAnchor + 3;
			DWORD oldProt = 0;
			VirtualProtect(reinterpret_cast<void*>(immAddr), 4,
				PAGE_EXECUTE_READWRITE, &oldProt);
			std::memcpy(reinterpret_cast<void*>(immAddr),
				&g_btnStateMaskSaved, 4);
			VirtualProtect(reinterpret_cast<void*>(immAddr), 4,
				oldProt, &oldProt);
		}
		if (g_btnStateTableLea)
		{
			int32_t tmp32 = 0;
			PatchDisp32(g_btnStateTableLea + 3,
				g_btnStateTableDispSaved, &tmp32);
		}
		if (g_btnStateAnchor)
		{
			PatchBytes(g_btnStateAnchor + 6, g_btnStateReadSaved,
				sizeof(g_btnStateReadSaved), &tmp);
			PatchByte(g_btnStateAnchor + 3, g_btnStateGateSaved, &tmp);
		}
		if (g_invSearchEndAnchor)
			PatchByte(g_invSearchEndAnchor + 6, g_invSearchEndSaved, &tmp);
		if (g_invSearchAnchor)
		{
			PatchBytes(g_invSearchAnchor + 8, g_invSearchReadSaved,
				sizeof(g_invSearchReadSaved), &tmp);
			PatchByte(g_invSearchAnchor + 2, g_invSearchGateSaved, &tmp);
		}
#endif // !CLIENT_DLL
		if (g_searchEndAnchor)
			PatchByte(g_searchEndAnchor + 6, g_searchEndSaved, &tmp);
		if (g_searchAnchor)
		{
			// Restore the 5-byte read first, then the gate byte (gate lives
			// INSIDE the read-pattern prefix, but at a fixed offset 2 which
			// is part of `83 FB 06`, not of the replaced 5-byte read @+8).
			PatchBytes(g_searchAnchor + 8, g_searchReadSaved,
				sizeof(g_searchReadSaved), &tmp);
			PatchByte(g_searchAnchor + 2, g_searchGateSaved, &tmp);
		}
		if (g_serverLoop3Anchor)
		{
			PatchBytes(g_serverLoop3Anchor + 5, g_serverLoop3ReadSaved,
				sizeof(g_serverLoop3ReadSaved), &tmp);
			PatchByte(g_serverLoop3Anchor + 2, g_serverLoop3GateSaved, &tmp);
		}
		// Restore outer-gate mask 0x07F00000 -> 0x03F00000 (saved value).
		if (g_serverOuterGateAnchor)
		{
			const uintptr_t immAddr = g_serverOuterGateAnchor + 6;
			DWORD oldProt = 0;
			VirtualProtect(reinterpret_cast<void*>(immAddr), 4,
				PAGE_EXECUTE_READWRITE, &oldProt);
			std::memcpy(reinterpret_cast<void*>(immAddr),
				&g_serverOuterGateSaved, 4);
			VirtualProtect(reinterpret_cast<void*>(immAddr), 4,
				oldProt, &oldProt);
		}
		if (g_serverLoop2EndAnchor)
			PatchByte(g_serverLoop2EndAnchor + 0x0D, g_serverLoop2EndSaved, &tmp);
		if (g_serverLoop2Anchor)
		{
			PatchBytes(g_serverLoop2Anchor + 0x0D, g_serverLoop2ReadSaved,
				sizeof(g_serverLoop2ReadSaved), &tmp);
			PatchByte(g_serverLoop2Anchor + 3, g_serverLoop2GateSaved, &tmp);
		}
		if (g_serverLoop1EndAnchor)
			PatchByte(g_serverLoop1EndAnchor + 6, g_serverLoop1EndSaved, &tmp);
		if (g_serverLoop1Anchor)
		{
			PatchBytes(g_serverLoop1Anchor + 12, g_serverLoop1ReadSaved,
				sizeof(g_serverLoop1ReadSaved), &tmp);
			PatchByte(g_serverLoop1Anchor + 2, g_serverLoop1GateSaved, &tmp);
		}
		if (g_serverTableLea)
		{
			int32_t tmp32 = 0;
			PatchDisp32(g_serverTableLea + 3,
				g_serverTableDispSaved, &tmp32);
		}
		if (g_clientSentinelAnchor)
			PatchByte(g_clientSentinelAnchor + 9, g_clientSentinelSaved, &tmp);
		if (g_clientGateAnchor)
		{
			int32_t tmp32 = 0;
			PatchDisp32(g_clientGateAnchor + 10,
				g_clientTableDispSaved, &tmp32);
			PatchByte(g_clientGateAnchor + 1, g_clientGateSaved, &tmp);
		}
		g_patchesAttached = false;
		return;
	}

	// --- Attach ---
	if (g_patchesAttached)
		return;

	// Widen the native 6-slot offhand dispatch pipeline to 8 slots and
	// install the Detours hooks that feed slots 6/7 through the same path
	// as slots 0..5 (including every legend tactical/ultimate/ordnance).

	if (!g_clientGateAnchor && !g_serverLoop1Anchor)
	{
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: no anchors matched -- nothing to "
			"patch. Verify pattern signatures vs live r5apex.exe.\n");
		return;
	}

	// Allocate the near page. Anchor choice: any resolved engine address.
	void* nearAnchor = reinterpret_cast<void*>(
		g_serverLoop1Anchor ? g_serverLoop1Anchor : g_clientGateAnchor);
	g_pNearPage = AllocNearPage(nearAnchor);
	if (!g_pNearPage)
	{
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: AllocNearPage failed -- aborting "
			"attach. Slot 6/7 activation will NOT work.\n");
		return;
	}

	Msg(eDLL_T::ENGINE,
		"[offhand-patches] installing native dispatch rewiring "
		"(gate/bitmask widening + 12 Detours hooks on the shared offhand "
		"activation path, including slots 0..5).\n");

	// Layout: [0..0x20] bitmask table; [0x20..] trampolines.
	g_pNearBitmaskTable = reinterpret_cast<uint32_t*>(g_pNearPage);
	const uint32_t kTable[8] = {
		0x00100000u, 0x00200000u, 0x00400000u, 0x00800000u,
		0x01000000u, 0x00000000u, 0x02000000u, 0x04000000u
	};
	std::memcpy(g_pNearBitmaskTable, kTable, sizeof(kTable));

	uint8_t* tramp = g_pNearPage + 0x20;

	// === Client patches ===================================================
	if (g_clientGateAnchor)
	{
		// G1: cmp al, 6 -> 8
		PatchByte(g_clientGateAnchor + 1, 0x08, &g_clientGateSaved);

		// D1: rewrite table disp32 in `mov eax, [rcx+rax*4+disp32]`.
		uintptr_t tableVA  = reinterpret_cast<uintptr_t>(g_pNearBitmaskTable);
		uintptr_t moduleVA = static_cast<uintptr_t>(g_GameDll.GetModuleBase());
		int64_t deltaFromBase = static_cast<int64_t>(tableVA) - static_cast<int64_t>(moduleVA);
		if (deltaFromBase > INT32_MAX || deltaFromBase < INT32_MIN)
		{
			Warning(eDLL_T::ENGINE,
				"OffhandActivationPatches: near-table at 0x%llx is beyond "
				"+-2GB of r5apex base 0x%llx (delta=%lld); client "
				"[rcx+rax*4+disp32] form cannot reach. Slot 6/7 client "
				"input will NOT pass range gate.\n",
				static_cast<unsigned long long>(tableVA),
				static_cast<unsigned long long>(moduleVA),
				static_cast<long long>(deltaFromBase));
		}
		else
		{
			int32_t newDisp = static_cast<int32_t>(deltaFromBase);
			PatchDisp32(g_clientGateAnchor + 10, newDisp,
				&g_clientTableDispSaved);
		}
	}
	if (g_clientSentinelAnchor)
	{
		// G2: reset-sentinel 6 -> 8.
		PatchByte(g_clientSentinelAnchor + 9, 0x08, &g_clientSentinelSaved);
	}

	// === Server patches ===================================================
	if (g_serverTableLea)
	{
		// D2: rewrite LEA disp32 (RIP-relative) to point at our 8-entry
		// table. next_instr = serverTableLea + 7.
		uintptr_t tableVA = reinterpret_cast<uintptr_t>(g_pNearBitmaskTable);
		int64_t rel = static_cast<int64_t>(
			tableVA - (g_serverTableLea + 7));
		if (rel > INT32_MAX || rel < INT32_MIN)
		{
			Warning(eDLL_T::ENGINE,
				"OffhandActivationPatches: near-table is >2GB from server "
				"table LEA -- RIP-relative disp32 cannot reach. Slot 6/7 "
				"server-side bitmask will NOT resolve; AllocNearPage "
				"returned too far a page.\n");
		}
		else
		{
			int32_t newDisp = static_cast<int32_t>(rel);
			PatchDisp32(g_serverTableLea + 3, newDisp,
				&g_serverTableDispSaved);
		}
	}

	if (g_serverLoop1Anchor)
	{
		// G3: gate 6 -> 8.
		PatchByte(g_serverLoop1Anchor + 2, 0x08, &g_serverLoop1GateSaved);

		// T1 trampoline: replace 7-byte read @anchor+12 with
		// `E9 rel32` + 2 NOPs; stub lives at `tramp`.
		uint8_t* readSite = reinterpret_cast<uint8_t*>(g_serverLoop1Anchor + 12);
		uint8_t* returnAddr = readSite + 7;
		uint8_t* stubStart = tramp;
		tramp = EmitStub(tramp, STUB_SERVER_LOOP1, returnAddr,
			reinterpret_cast<const void*>(&OffhandReadHelper));

		uint8_t patch[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
		int64_t rel = static_cast<int64_t>(
			reinterpret_cast<uintptr_t>(stubStart)
			- (reinterpret_cast<uintptr_t>(readSite) + 5));
		int32_t rel32 = static_cast<int32_t>(rel);
		std::memcpy(patch + 1, &rel32, 4);
		PatchBytes(reinterpret_cast<uintptr_t>(readSite), patch,
			sizeof(patch), g_serverLoop1ReadSaved);
	}

	if (g_serverLoop1EndAnchor)
	{
		// G4: first-loop end 6 -> 8.
		PatchByte(g_serverLoop1EndAnchor + 6, 0x08, &g_serverLoop1EndSaved);
	}

	// === Second loop (holster/deactivation) ==============================
	if (g_serverLoop2Anchor)
	{
		// G5: gate 6 -> 8. Byte at anchor+3 is the immediate `06`.
		PatchByte(g_serverLoop2Anchor + 3, 0x08, &g_serverLoop2GateSaved);

		// T2: replace `movsxd rax, r14d; mov ecx, [rdi+rax*4+16B4h]` with a trampoline through OffhandSlotsExt_Get.
		uint8_t* readSite = reinterpret_cast<uint8_t*>(g_serverLoop2Anchor + 0x0D);
		uint8_t* returnAddr = readSite + 7;
		uint8_t* stubStart = tramp;
		tramp = EmitStub(tramp, STUB_SERVER_LOOP2, returnAddr,
			reinterpret_cast<const void*>(&OffhandReadHelper));

		uint8_t patch[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
		int64_t rel = static_cast<int64_t>(
			reinterpret_cast<uintptr_t>(stubStart)
			- (reinterpret_cast<uintptr_t>(readSite) + 5));
		int32_t rel32 = static_cast<int32_t>(rel);
		std::memcpy(patch + 1, &rel32, 4);
		PatchBytes(reinterpret_cast<uintptr_t>(readSite), patch,
			sizeof(patch), g_serverLoop2ReadSaved);
	}

	if (g_serverLoop2EndAnchor)
	{
		// G5b: second-loop end 6 -> 8. Byte at anchor+0x0D is the immediate.
		PatchByte(g_serverLoop2EndAnchor + 0x0D, 0x08, &g_serverLoop2EndSaved);
	}

	// G10: outer-gate mask 0x03F00000 -> 0x07F00000 (bit 26 = slot 7).
	if (g_serverOuterGateAnchor)
	{
		const uintptr_t immAddr = g_serverOuterGateAnchor + 6;
		DWORD oldProt = 0;
		VirtualProtect(reinterpret_cast<void*>(immAddr), 4,
			PAGE_EXECUTE_READWRITE, &oldProt);
		std::memcpy(&g_serverOuterGateSaved,
			reinterpret_cast<const void*>(immAddr), 4);
		const uint32_t newImm = 0x07F00000u;
		std::memcpy(reinterpret_cast<void*>(immAddr), &newImm, 4);
		VirtualProtect(reinterpret_cast<void*>(immAddr), 4, oldProt, &oldProt);
	}

	if (g_serverLoop3Anchor)
	{
		// G6: gate 6 -> 8.
		PatchByte(g_serverLoop3Anchor + 2, 0x08, &g_serverLoop3GateSaved);

		// T3 trampoline: replace 8-byte `movsx rax, bl; mov edx, [rcx+rax*4+0x2C]`
		// @anchor+5 with `E9 rel32` + 3 NOPs.
		uint8_t* readSite = reinterpret_cast<uint8_t*>(g_serverLoop3Anchor + 5);
		uint8_t* returnAddr = readSite + 8;
		uint8_t* stubStart = tramp;
		tramp = EmitStub(tramp, STUB_SERVER_LOOP3, returnAddr,
			reinterpret_cast<const void*>(&OffhandReadHelper));

		uint8_t patch[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
		int64_t rel = static_cast<int64_t>(
			reinterpret_cast<uintptr_t>(stubStart)
			- (reinterpret_cast<uintptr_t>(readSite) + 5));
		int32_t rel32 = static_cast<int32_t>(rel);
		std::memcpy(patch + 1, &rel32, 4);
		PatchBytes(reinterpret_cast<uintptr_t>(readSite), patch,
			sizeof(patch), g_serverLoop3ReadSaved);
	}

	// === search function ====================================
	if (g_searchAnchor)
	{
		// G8: search gate 6 -> 8.
		PatchByte(g_searchAnchor + 2, 0x08, &g_searchGateSaved);

		// T5 trampoline: replace 5-byte `mov ecx, [r8+rax*4+0x2C]` @anchor+8
		// with `E9 rel32` (exactly 5 bytes, no NOP padding).
		uint8_t* readSite = reinterpret_cast<uint8_t*>(g_searchAnchor + 8);
		uint8_t* returnAddr = readSite + 5;
		uint8_t* stubStart = tramp;
		tramp = EmitStub(tramp, STUB_SEARCH, returnAddr,
			reinterpret_cast<const void*>(&OffhandReadHelper));

		uint8_t patch[5] = {0xE9, 0, 0, 0, 0};
		int64_t rel = static_cast<int64_t>(
			reinterpret_cast<uintptr_t>(stubStart)
			- (reinterpret_cast<uintptr_t>(readSite) + 5));
		int32_t rel32 = static_cast<int32_t>(rel);
		std::memcpy(patch + 1, &rel32, 4);
		PatchBytes(reinterpret_cast<uintptr_t>(readSite), patch,
			sizeof(patch), g_searchReadSaved);
	}

	if (g_searchEndAnchor)
	{
		// G9: search loop-end 6 -> 8.
		PatchByte(g_searchEndAnchor + 6, 0x08, &g_searchEndSaved);
	}

#ifndef CLIENT_DLL
	// === Inventory offhand-slot search (toss FSM slot lookup) ============
	if (g_invSearchAnchor)
	{
		// Gate 6 -> 8 at anchor+2.
		PatchByte(g_invSearchAnchor + 2, 0x08, &g_invSearchGateSaved);

		// Replace 5-byte `mov edx, [r10+rcx*4+0x2C]` @anchor+8 with `E9 rel32`.
		// STUB_INV_SEARCH preserves rax (live loop counter).
		uint8_t* readSite = reinterpret_cast<uint8_t*>(g_invSearchAnchor + 8);
		uint8_t* returnAddr = readSite + 5;
		uint8_t* stubStart = tramp;
		tramp = EmitStub(tramp, STUB_INV_SEARCH, returnAddr,
			reinterpret_cast<const void*>(&OffhandReadHelper));

		uint8_t patch[5] = {0xE9, 0, 0, 0, 0};
		int64_t rel = static_cast<int64_t>(
			reinterpret_cast<uintptr_t>(stubStart)
			- (reinterpret_cast<uintptr_t>(readSite) + 5));
		int32_t rel32 = static_cast<int32_t>(rel);
		std::memcpy(patch + 1, &rel32, 4);
		PatchBytes(reinterpret_cast<uintptr_t>(readSite), patch,
			sizeof(patch), g_invSearchReadSaved);
	}

	if (g_invSearchEndAnchor)
	{
		// Loop end 6 -> 8 at endAnchor+6.
		PatchByte(g_invSearchEndAnchor + 6, 0x08, &g_invSearchEndSaved);
	}

	// === Per-slot offhand button state ===================================
	if (g_btnStateAnchor)
	{
		// Gate 6 -> 8 at anchor+3.
		PatchByte(g_btnStateAnchor + 3, 0x08, &g_btnStateGateSaved);

		// Replace 8-byte read @anchor+6 with `E9 rel32` + 3 NOPs.
		uint8_t* readSite = reinterpret_cast<uint8_t*>(g_btnStateAnchor + 6);
		uint8_t* returnAddr = readSite + 8;
		uint8_t* stubStart = tramp;
		tramp = EmitStub(tramp, STUB_BTN_STATE, returnAddr,
			reinterpret_cast<const void*>(&OffhandReadHelper));

		uint8_t patch[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
		int64_t rel = static_cast<int64_t>(
			reinterpret_cast<uintptr_t>(stubStart)
			- (reinterpret_cast<uintptr_t>(readSite) + 5));
		int32_t rel32 = static_cast<int32_t>(rel);
		std::memcpy(patch + 1, &rel32, 4);
		PatchBytes(reinterpret_cast<uintptr_t>(readSite), patch,
			sizeof(patch), g_btnStateReadSaved);
	}

	if (g_btnStateTableLea)
	{
		// Retarget LEA rdx, [rip+disp32] to g_pNearBitmaskTable.
		// next_instr = tableLea + 7.
		uintptr_t tableVA = reinterpret_cast<uintptr_t>(g_pNearBitmaskTable);
		int64_t rel = static_cast<int64_t>(
			tableVA - (g_btnStateTableLea + 7));
		if (rel > INT32_MAX || rel < INT32_MIN)
		{
			Warning(eDLL_T::ENGINE,
				"OffhandActivationPatches: near-table is >2GB from button-state "
				"table LEA -- RIP-relative disp32 cannot reach. Slot 6/7 "
				"button-state bitmask will NOT resolve.\n");
		}
		else
		{
			int32_t newDisp = static_cast<int32_t>(rel);
			PatchDisp32(g_btnStateTableLea + 3, newDisp,
				&g_btnStateTableDispSaved);
		}
	}

	if (g_btnStateMaskAnchor)
	{
		// Widen mask 0x03F00000 -> 0x07F00000 (add bit 26 for slot 7).
		// Imm32 at anchor+3.
		const uintptr_t immAddr = g_btnStateMaskAnchor + 3;
		DWORD oldProt = 0;
		VirtualProtect(reinterpret_cast<void*>(immAddr), 4,
			PAGE_EXECUTE_READWRITE, &oldProt);
		std::memcpy(&g_btnStateMaskSaved,
			reinterpret_cast<const void*>(immAddr), 4);
		const uint32_t newImm = 0x07F00000u;
		std::memcpy(reinterpret_cast<void*>(immAddr), &newImm, 4);
		VirtualProtect(reinterpret_cast<void*>(immAddr), 4, oldProt, &oldProt);
	}

	// Whole-function hook.
	if (v_OffhandDispatch_orig)
	{
		DetourSetup(&v_OffhandDispatch_orig,
			&Hook_OffhandDispatch, /*bAttach=*/true);
	}

	if (v_WeaponTossRelease_orig)
	{
		DetourSetup(&v_WeaponTossRelease_orig,
			&Hook_WeaponTossRelease, /*bAttach=*/true);
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: weapon TossPrep unresolved -- "
			"slot 6/7 tap will not start DoToss\n");
	}

	if (v_PlayerWeaponFrame_orig)
	{
		DetourSetup(&v_PlayerWeaponFrame_orig,
			&Hook_PlayerWeaponFrame, /*bAttach=*/true);
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: player weapon-frame unresolved -- "
			"button hold may miss the weapon-frame ordering\n");
	}

	if (v_WeaponAnimEventDispatch_orig)
	{
		DetourSetup(&v_WeaponAnimEventDispatch_orig,
			&Hook_WeaponAnimEventDispatch, /*bAttach=*/true);
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: weapon anim-event dispatch unresolved -- "
			"animevent24 gate dump will not attach\n");
	}

	if (v_WeaponAnimEvent_orig)
	{
		DetourSetup(&v_WeaponAnimEvent_orig,
			&Hook_WeaponAnimEvent, /*bAttach=*/true);
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: weapon anim event unresolved -- "
			"animevent id tap will not attach\n");
	}
#endif // !CLIENT_DLL

	if (v_SelectedOffhandResolver_orig)
	{
		DetourSetup(&v_SelectedOffhandResolver_orig,
			&Hook_SelectedOffhandResolver, /*bAttach=*/true);
	}

	// Whole-function hook: NULL-guard.
	if (v_PerSelectorActivateInner_orig)
	{
		DetourSetup(&v_PerSelectorActivateInner_orig,
			&Hook_PerSelectorActivateInner, /*bAttach=*/true);
	}

	// Whole-function hook: outer NULL-guard.
	if (v_PerSelectorActivateOuter_orig)
	{
		DetourSetup(&v_PerSelectorActivateOuter_orig,
			&Hook_PerSelectorActivateOuter, /*bAttach=*/true);
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"OffhandActivationPatches: pattern unresolved --"
			"slot 6/7 activation WILL crash on NULL-deref at +0x15A8 when "
			"the extension map is empty or stale.\n");
	}

	g_patchesAttached = true;
}
