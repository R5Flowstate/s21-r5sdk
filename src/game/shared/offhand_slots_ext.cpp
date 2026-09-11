//=============================================================================
//
// Purpose: SDK-side replication for offhand slots 6 and 7. Do not write engine inventory bytes.
//
//=============================================================================
#include "core/stdafx.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/scriptremotefunctions_shared.h"
#include "game/shared/weapon_script_vars.h"
#include "game/shared/edict_dirty.h"
#include "offhand_slots_ext.h"
#include "offhand_activation_patches.h"
#include "tier1/convar.h"
#include "tier0/commandline.h"  // CommandLine->CheckParm (init-time launch-arg read)
#include "public/edict.h"
extern CGlobalVars* gpGlobals;

// Slots 6/7 are side-band only. Do not write engine offhandWeapons[6/7] (aliases other CBCC).
static ConVar sdk_offhand_ext_diag("sdk_offhand_ext_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Read-only diagnostic for offhandWeapons[6/7] SendProp pStruct identity. "
	"Does not enable extended offhand slots. 0 = off (default).");

static ConVar bridge_offhand_ext_hold_time("bridge_offhand_ext_hold_time", "0.5",
	FCVAR_RELEASE, "Seconds an extended offhand slot keeps its button bit set so the "
	"weapon-frame toss state machine can reach the release branch.");

static ConVar bridge_offhand_ext_restore_time("bridge_offhand_ext_restore_time", "1.5",
	FCVAR_RELEASE, "Seconds after an extended offhand select before the failsafe "
	"holster runs if the engine has not deselected the slot itself.");

bool OffhandSlotsExt_IsDiagEnabled()
{
	// SendTable-init runs before +convar launch commands. Read CommandLine at init for the diag switch.
	const char* val = nullptr;
	if (CommandLine() && CommandLine()->CheckParm("+sdk_offhand_ext_diag", &val) && val && val[0] == '1')
		return true;
	return sdk_offhand_ext_diag.GetBool();
}

// offhandWeapons[k] = CPlayer + 5812 + 4*k. Server bytes past [5] are aliased; do not write them.
static constexpr uintptr_t CPLAYER_OFFHAND_BASE = 5812; // +0x16B4 (offhandWeapons[0])

static inline uintptr_t OffhandExt_SlotOffsetBytes(int slot)
{
	return CPLAYER_OFFHAND_BASE + 4ull * static_cast<uintptr_t>(slot);
}

// Per-player shadow for slots 6/7. Default both entries to the empty sentinel
// so operator[] value-init never reports entity index 0 for an untouched slot.
struct OffhandExtSlots
{
	uint32_t slot[2] = {
		kOffhandSlotExtInvalidHandle,
		kOffhandSlotExtInvalidHandle
	}; // [0] = slot 6, [1] = slot 7
	int pendingSelect = -1;
	int   activeSlot   = -1;  // slot being driven through its toss window
	float holdUntil    = 0.0f;  // gpGlobals->curTime deadline for the button bits
	float restoreAfter = 0.0f;  // gpGlobals->curTime deadline for the failsafe holster
};

static SDKEntityMap<OffhandExtSlots>& OffhandExt_ShadowMap()
{
	static SDKEntityMap<OffhandExtSlots> s_map(ESide::Server, "OffhandSlotsExt");
	return s_map;
}

// Server: shadow map. Client: native entity bytes (S21 has eight real slots).
static uint32_t OffhandExt_ReadSlot(void* pPlayer, int slot)
{
	if (!pPlayer || !OffhandSlotsExt_IsExtendedSlot(slot))
		return kOffhandSlotExtInvalidHandle;
	const OffhandExtSlots* rec = OffhandExt_ShadowMap().Find(pPlayer);
	if (!rec)
		return kOffhandSlotExtInvalidHandle;
	return rec->slot[slot - kOffhandSlotExtFirst];
}

static bool OffhandExt_WriteSlot(void* pPlayer, int slot, uint32_t eh)
{
	if (!pPlayer || !OffhandSlotsExt_IsExtendedSlot(slot))
		return false;
	OffhandExt_ShadowMap()[pPlayer].slot[slot - kOffhandSlotExtFirst] = eh;
	return true;
}

// LooksLikeValidEntity: cheap vtable-range check so GetEntity does not follow a freed instance.
static bool LooksLikeValidEntity(void* pEntity)
{
	if (!pEntity) return false;
	const uintptr_t addr = reinterpret_cast<uintptr_t>(pEntity);
	// Valid kernel-mode / too-low / too-high addresses that can't be heap
	// entities in user-mode (user-mode x64 heap is typically in the range
	// 0x00007FF'0000'0000 downward to 0x1'0000'0000 or so).
	if (addr < 0x10000ull) return false;
	if (addr >= 0x800000000000ull) return false;

	// Read first qword (vtable pointer). If the vtable is at an impossible
	// address, reject.
	__try
	{
		const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(addr);
		if (vt < 0x10000ull || vt >= 0x800000000000ull) return false;
		// Sanity check: vtable's first slot should also be a valid code
		// pointer. If the "vtable" is a heap string, its first qword is
		// another heap pointer or ASCII bytes -- both fail this check.
		const uintptr_t fn0 = *reinterpret_cast<const uintptr_t*>(vt);
		if (fn0 < 0x10000ull || fn0 >= 0x800000000000ull) return false;
	}
	__except (1 /*EXCEPTION_EXECUTE_HANDLER*/)
	{
		return false;
	}
	return true;
}

// Public C++ API for Give/Take/Get overrides.
bool OffhandSlotsExt_Set(void* pPlayer, int slot, uint32_t weaponEHandle, SQVM* v)
{
	if (!pPlayer || !OffhandSlotsExt_IsExtendedSlot(slot))
		return false;

	(void)v; // server-authoritative; VM context no longer branches behaviour

	// Side-band write only; engine inventory bytes are never touched.
	OffhandExt_WriteSlot(pPlayer, slot, weaponEHandle);
	// Shadow does not dirty any entity field; the encoder only re-ships when
	// the edict is marked dirty.
	MarkEntityEdictDirty(pPlayer);
	return true;
}

uint32_t OffhandSlotsExt_Get(void* pPlayer, int slot, SQVM* v)
{
	(void)v;
	return OffhandExt_ReadSlot(pPlayer, slot);
}

static const uint32_t kOffhandButtonBit[8] = {
	0x00100000u, 0x00200000u, 0x00400000u, 0x00800000u,
	0x01000000u, 0x00000000u, 0x02000000u, 0x04000000u
};

void OffhandSlotsExt_RequestSelect(void* pPlayer, int slot)
{
	if (!pPlayer || !OffhandSlotsExt_IsExtendedSlot(slot))
		return;
	OffhandExt_ShadowMap()[pPlayer].pendingSelect = slot;
}

// Dedi ammo-regen FSM is behind CWeaponX::IsPredicted / cl_predict and never
// runs. First throw empties clip; engine then skips TossPrep. Refill here.
static void OffhandExt_ResetTossWeapon(void* pWeapon)
{
	if (!pWeapon)
		return;

	uint8_t* const w = static_cast<uint8_t*>(pWeapon);
	int32_t minFire = *reinterpret_cast<int32_t*>(w + 0x122C); // ammo_min_to_fire
	if (minFire < 1)
		minFire = 1;
	*reinterpret_cast<int32_t*>(w + 0x1224) = minFire; // m_ammoInClip
	int32_t* const pStock = reinterpret_cast<int32_t*>(w + 0x1228); // m_ammoInStockpile
	if (*pStock < minFire)
		*pStock = minFire;
	w[0x1298] = 0; // toss armed
	w[0x1299] = 0; // toss pending
	*reinterpret_cast<float*>(w + 0x11F8) = 0.0f; // next-attack / busy
	MarkEntityEdictDirty(pWeapon);
}

void OffhandSlotsExt_EndSelectHold(void* pPlayer)
{
	if (!pPlayer)
		return;
	OffhandExtSlots* const rec = OffhandExt_ShadowMap().Find(pPlayer);
	if (!rec)
		return;
	rec->holdUntil = 0.0f;
}

void OffhandSlotsExt_EndTossWindow(void* pPlayer)
{
	if (!pPlayer)
		return;
	OffhandExtSlots* const rec = OffhandExt_ShadowMap().Find(pPlayer);
	if (!rec)
		return;
	rec->holdUntil = 0.0f;
	rec->restoreAfter = 0.0f;
	rec->activeSlot = -1;
}

void OffhandSlotsExt_ApplyPendingSelect(void* pPlayer)
{
	if (!pPlayer || !gpGlobals)
		return;
	OffhandExtSlots* const rec = OffhandExt_ShadowMap().Find(pPlayer);
	if (!rec)
		return;

	const float now = gpGlobals->curTime;
	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);

	if (rec->pendingSelect >= 0 && OffhandSlotsExt_IsExtendedSlot(rec->pendingSelect))
	{
		rec->activeSlot   = rec->pendingSelect;
		rec->holdUntil    = now + bridge_offhand_ext_hold_time.GetFloat();
		rec->restoreAfter = now + bridge_offhand_ext_restore_time.GetFloat();

		const uint32_t eh = OffhandSlotsExt_GetServer(pPlayer, rec->activeSlot);
		void* pWeapon = nullptr;
		int32_t clipBefore = -1;
		if (eh != kOffhandSlotExtInvalidHandle)
			pWeapon = SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server);
		if (pWeapon)
		{
			clipBefore = *reinterpret_cast<int32_t*>(
				static_cast<uint8_t*>(pWeapon) + 0x1224);
			OffhandExt_ResetTossWeapon(pWeapon);
		}

		if (OffhandActivation_TossDiagEnabled())
		{
			static volatile LONG s_applyLog = 0;
			const LONG n = InterlockedIncrement(&s_applyLog);
			if (n <= 8)
				Msg(eDLL_T::SERVER,
					"[OFFHAND-EXT] apply-pending slot=%d buttons=0x%X bits=0x%X "
					"clip %d->%d (#%d)\n",
					rec->activeSlot,
					*reinterpret_cast<uint32_t*>(pPlayerBytes + 0x60DC),
					*reinterpret_cast<uint32_t*>(pPlayerBytes + 0x5B0C),
					clipBefore,
					pWeapon ? *reinterpret_cast<int32_t*>(
						static_cast<uint8_t*>(pWeapon) + 0x1224) : -1,
					static_cast<int>(n));
		}
	}
	rec->pendingSelect = -1;

	if (rec->activeSlot < 0)
		return;

	if (now <= rec->holdUntil)
	{
		// First-loop needs the slot bit and m_nButtons & table[slot].
		// Never write m_nButtons2 (+0x60E0) -- bit 25 there is a global flag.
		*reinterpret_cast<uint32_t*>(pPlayerBytes + 0x5B0C) |= (1u << rec->activeSlot);
		*reinterpret_cast<uint32_t*>(pPlayerBytes + 0x60DC) |= kOffhandButtonBit[rec->activeSlot];
		return;
	}

	if (now <= rec->restoreAfter)
		return;

	// Engine deselect clears m_selectedOffhands without holstering an extended-
	// slot weapon, so the unwind must not be gated on that byte.
	const int activeSlot = rec->activeSlot;
	const uint32_t eh = OffhandSlotsExt_GetServer(pPlayer, activeSlot);
	void* pWeapon = nullptr;
	if (eh != kOffhandSlotExtInvalidHandle)
		pWeapon = SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server);

	int32_t weapState = -1;
	if (pWeapon)
		weapState = *reinterpret_cast<int32_t*>(
			static_cast<uint8_t*>(pWeapon) + 0x1234); // m_weapState

	if (pWeapon && v_WeaponX_HolsterInternal)
		v_WeaponX_HolsterInternal(pWeapon, true);

	uint32_t clearedMask = 0;
	for (int k = 0; k < 3; ++k)
	{
		const uint8_t selByte = *(pPlayerBytes + k + 0x16EE);
		const uint32_t activeEh = *reinterpret_cast<uint32_t*>(
			pPlayerBytes + 0x16CC + 4 * k); // activeWeapons[k]
		const bool selMatch = (selByte == static_cast<uint8_t>(activeSlot));
		const bool ehMatch = (eh != kOffhandSlotExtInvalidHandle
			&& activeEh == eh);
		if (!selMatch && !ehMatch)
			continue;
		if (v_Weapon_SetSelectedOffhandCleared)
			v_Weapon_SetSelectedOffhandCleared(pPlayer,
				static_cast<unsigned int>(k));
		clearedMask |= (1u << k);
	}

	Warning(eDLL_T::SERVER,
		"[OFFHAND-EXT] failsafe-unwind slot=%d eh=0x%08X weapState=%d "
		"cleared=0x%X\n",
		activeSlot, eh, weapState, clearedMask);

	OffhandActivation_SetOneHanded(pPlayer, false);
	MarkEntityEdictDirty(pPlayer);
	rec->activeSlot = -1;
}

uint32_t OffhandSlotsExt_GetServer(void* pPlayer, int slot)
{
	// Feeds the SendProp proxy and activation trampolines. A handle write must dirty the player.
	const uint32_t eh = OffhandExt_ReadSlot(pPlayer, slot);
	if (eh == kOffhandSlotExtInvalidHandle)
		return kOffhandSlotExtInvalidHandle;

	if (!SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server))
		return kOffhandSlotExtInvalidHandle;

	return eh;
}

uint32_t OffhandSlotsExt_GetAnySide(void* pPlayer, int slot, ESide* outSide)
{
	// Server: shadow map. Client: native entity field (decoded from the wire).
	// Report Server on a hit; miss leaves *outSide untouched.
	const uint32_t eh = OffhandExt_ReadSlot(pPlayer, slot);
	if (outSide && eh != kOffhandSlotExtInvalidHandle)
		*outSide = ESide::Server;
	return eh;
}

// Script natives: thin wrappers over the public C++ API, registered on the player class.
static SQRESULT Script_GetOffhandSlotExtEHandle(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	SQInteger slot = 0;
	sq_getinteger(v, 2, &slot);

	const uint32_t handle = OffhandSlotsExt_Get(pPlayer,
		static_cast<int>(slot), v);
	sq_pushinteger(v, static_cast<SQInteger>(handle));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetOffhandSlotExtEHandle(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	SQInteger slot = 0;
	SQInteger rawHandle = 0;
	sq_getinteger(v, 2, &slot);
	sq_getinteger(v, 3, &rawHandle);

	if (!OffhandSlotsExt_IsExtendedSlot(static_cast<int>(slot)))
	{
		v_SQVM_ScriptError(
			"SetOffhandSlotExtEHandle: slot %d out of range [%d, %d]",
			static_cast<int>(slot),
			kOffhandSlotExtFirst, kOffhandSlotExtLast);
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	OffhandSlotsExt_Set(pPlayer, static_cast<int>(slot),
		static_cast<uint32_t>(rawHandle), v);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Register once per VM from Script_RegisterPlayerScriptFunctions.
void OffhandSlotsExt_Register(ScriptClassDescriptor_t* playerStruct)
{
	playerStruct->AddFunction(
		"GetOffhandSlotExtEHandle",
		"Script_GetOffhandSlotExtEHandle",
		"Returns the raw EHandle stored in SDK-extended offhand slot 6 or 7, "
		"or 0xFFFFFFFF if the slot is empty / slot ID out of range.",
		"int",
		"int slot",
		false,
		Script_GetOffhandSlotExtEHandle);

	playerStruct->AddFunction(
		"SetOffhandSlotExtEHandle",
		"Script_SetOffhandSlotExtEHandle",
		"Writes the raw EHandle for SDK-extended offhand slot 6 or 7 into the "
		"side-band slot store; the engine replicates it to all clients, which "
		"decode and activate it natively. Server VM only.",
		"void",
		"int slot, int ehandle",
		false,
		Script_SetOffhandSlotExtEHandle);
}

// Spawn + bind, then L1 write + L2 broadcast. DestroyEntity takes pWeapon+0x40 (IHandleEntity).
// Post-spawn Init runs twice; that double-tick is load-bearing.
// Trim pWeapon's entries in the engine weapon list. Off-path destroy leaves a stale CWeaponX*.
static constexpr int kOffhandExtWeaponListMax = 65536; // corrupt-count backstop

static int OffhandExt_TrimEngineWeaponList(void* pWeapon, int keep)
{
	if (!pWeapon || !g_pWeaponListMem || !g_pWeaponListCount)
		return -1;

	void** const pList = *g_pWeaponListMem;
	int count = *g_pWeaponListCount;

	if (!pList || count <= 0)
		return 0;

	if (count > kOffhandExtWeaponListMax)
	{
		Warning(eDLL_T::SERVER, "[offhand-ext] engine weapon list count %d exceeds the "
			"sanity bound; refusing to touch it.\n", count);
		return -1;
	}

	int seen = 0;
	int removed = 0;

	// Walk backwards: the fast-remove pulls the tail down into the hole, and a
	// descending index never revisits an element it has already moved.
	for (int i = count - 1; i >= 0; i--)
	{
		if (pList[i] != pWeapon)
			continue;

		if (++seen <= keep)
			continue;

		if (i != count - 1)
			pList[i] = pList[count - 1];
		count--;
		removed++;
	}

	if (removed)
		*g_pWeaponListCount = count;

	return removed;
}

uint32_t OffhandSlotsExt_Give(void* pPlayer, int slot, const char* pszClassName,
                              uint32_t modsBitfield, SQVM* v)
{
	if (!pPlayer || !pszClassName || !*pszClassName)
		return kOffhandSlotExtInvalidHandle;
	if (!OffhandSlotsExt_IsExtendedSlot(slot))
		return kOffhandSlotExtInvalidHandle;

	// Server-VM-only: these entry points touch engine server-side structures
	// (team DT, owner DT, dirty-mark) and emit the S->C broadcast. A client
	// VM caller would corrupt state.
	if (!v || v->GetContext() != SQCONTEXT::SERVER)
	{
		Warning(eDLL_T::SERVER,
			"OffhandSlotsExt_Give: invoked outside SERVER VM -- rejected "
			"(slot %d, weapon %s)\n", slot, pszClassName);
		return kOffhandSlotExtInvalidHandle;
	}

	if (!v_CPlayer_SpawnOffhandWeapon ||
	    !v_CWeapon_InitTeamColor      ||
	    !v_CWeapon_BindToOwner        ||
	    !v_CWeapon_PostSpawnInit      ||
	    !v_CWeapon_OwnerDirtyReplicate||
	    !v_CWeapon_DestroyEntity)
	{
		Warning(eDLL_T::SERVER,
			"OffhandSlotsExt_Give: one or more engine helpers unresolved; "
			"the offhand-slot-ext pattern set needs revisiting for this build.\n");
		return kOffhandSlotExtInvalidHandle;
	}

	// Occupied slot returns InvalidHandle. Take first to swap. Shipping scripts never overwrite 0..5 either.
	const uint32_t occupantEH = OffhandExt_ReadSlot(pPlayer, slot);
	if (occupantEH != kOffhandSlotExtInvalidHandle)
	{
		if (SDKEntityState_Resolve(SDKEntityHandle(occupantEH), ESide::Server))
		{
			Warning(eDLL_T::SERVER,
				"[offhand-ext] Give: slot %d still holds live EH=0x%08X on "
				"player %p; taking it before the re-Give.\n",
				slot, occupantEH, pPlayer);
			OffhandSlotsExt_Take(pPlayer, slot, v);
		}
		else
		{
			Warning(eDLL_T::SERVER,
				"[offhand-ext] Give: slot %d held stale EH=0x%08X on player %p "
				"(weapon already destroyed); reclaiming the slot.\n",
				slot, occupantEH, pPlayer);
			OffhandExt_WriteSlot(pPlayer, slot, kOffhandSlotExtInvalidHandle);
		}

		// Either path must have emptied the slot; if not, something else owns
		// this entry and overwriting it would leak the weapon entity.
		if (OffhandExt_ReadSlot(pPlayer, slot) != kOffhandSlotExtInvalidHandle)
		{
			Warning(eDLL_T::SERVER,
				"[offhand-ext] Give: slot %d could not be cleared on player %p; "
				"refusing to overwrite.\n", slot, pPlayer);
			return kOffhandSlotExtInvalidHandle;
		}
	}

	// Step 1: spawn. Signature mirrors the native Give inner.
	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);
	void* const pWeapon = v_CPlayer_SpawnOffhandWeapon(
		pszClassName, modsBitfield,
		pPlayerBytes + 1104, pPlayerBytes + 1116, pPlayer,
		/*unused_a6=*/0, /*unused_a7=*/0);
	if (!pWeapon)
	{
		Warning(eDLL_T::SERVER,
			"OffhandSlotsExt_Give: spawn failed for weapon '%s' in slot %d. "
			"Likely unknown class name or unprecached.\n",
			pszClassName, slot);
		return kOffhandSlotExtInvalidHandle;
	}

	// EHandle is at CBaseEntity+0x8 (IHandleEntity::m_RefEHandle). Same
	// offset used by SDKEntityState_GetHandle and the native slot-store.
	const uint32_t weaponEHandle = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<uintptr_t>(pWeapon) + 8);

	// Steps 2..7 mirror the native slot-store helper and its caller tail.
	OffhandSlotsExt_Set(pPlayer, slot, weaponEHandle, v);

	// Native slot-store sets this bit. First-loop _bittest-skips the slot
	// without it. Server-internal, not a SendProp.
	*reinterpret_cast<uint32_t*>(pPlayerBytes + 0x5B0C) |= (1u << slot);

	// Step B: team color binding. Mirrors +0x49D.
	const uint32_t playerTeamId = *reinterpret_cast<uint32_t*>(pPlayerBytes + 1428);
	v_CWeapon_InitTeamColor(pWeapon, playerTeamId);

	// Owner-bind + DT init. Mirrors +0x4AB with flag=1.
	v_CWeapon_BindToOwner(pWeapon, pPlayer, /*setDefaults=*/1);

	// Step D: first post-spawn. Mirrors +0x4B3.
	v_CWeapon_PostSpawnInit(pWeapon);

	// Native slot-store is a byte copy into offhandWeapons[slot] -- we substitute L1+L2.
	*(pPlayerBytes + 0x171A) = *(pPlayerBytes + 0x1719);

	// Step F (tail): owner-dirty replicate. Triggers next-tick
	// DT re-encode for the weapon so remote clients see the owner binding.
	v_CWeapon_OwnerDirtyReplicate(pWeapon, pPlayer);

	// Invoke CWeapon::Deploy so the anim-event handler table at weapon+5752 is populated.
	OffhandActivation_InvokeDeploy(pWeapon);

	// Deploy also registers the weapon in the engine's global weapon list.
	const int dupes = OffhandExt_TrimEngineWeaponList(pWeapon, /*keep=*/1);
	if (dupes > 0)
	{
		Warning(eDLL_T::SERVER,
			"[offhand-ext] Give: slot %d weapon %p held %d duplicate engine "
			"weapon-list entr%s; trimmed to one.\n",
			slot, pWeapon, dupes, dupes == 1 ? "y" : "ies");
	}

	// Step G (tail): second post-spawn. Native intentionally
	// fires twice (once inside at step D, once
	// here at the tail). Some weapons rely on the double-tick of OnAttach.
	v_CWeapon_PostSpawnInit(pWeapon);

	// Native slot-store writes a non-zero warm time. Zero makes first-loop
	// require m_nButtons2, which TrySelectOffhand never sets.
	*reinterpret_cast<float*>(static_cast<uint8_t*>(pWeapon) + 0x1CA4) = 1.0f;

	// The button-word selector is `+0x1CA4 != 0 && +0x1CC8`. With only the
	// timestamp set, the offhand button tests read m_nButtons2, whose bit 25
	// is a global engine flag we must not drive.
	*reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(pWeapon) + 0x1CC8) = 1;

	return weaponEHandle;
}

bool OffhandSlotsExt_Take(void* pPlayer, int slot, SQVM* v)
{
	if (!pPlayer || !OffhandSlotsExt_IsExtendedSlot(slot))
		return false;
	if (!v || v->GetContext() != SQCONTEXT::SERVER)
	{
		Warning(eDLL_T::SERVER,
			"OffhandSlotsExt_Take: invoked outside SERVER VM -- rejected "
			"(slot %d)\n", slot);
		return false;
	}

	const uint32_t eh = OffhandExt_ReadSlot(pPlayer, slot);
	if (eh == kOffhandSlotExtInvalidHandle)
		return false; // slot empty; no-op is not an error

	void* const pWeapon = SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server);

	// Clear the shadow slot + dirty-mark so the proxy ships EMPTY next encode.
	OffhandSlotsExt_Set(pPlayer, slot, kOffhandSlotExtInvalidHandle, v);

	*reinterpret_cast<uint32_t*>(
		static_cast<uint8_t*>(pPlayer) + 0x5B0C) &= ~(1u << slot);

	if (pWeapon && v_CWeapon_DestroyEntity)
	{
		// Unregister before destroying. ~CWeaponX skips its own list removal
		// whenever the EHandle has already gone invalid, so relying on it means
		// the per-frame walk keeps a pointer to the freed weapon.
		const int scrubbed = OffhandExt_TrimEngineWeaponList(pWeapon, /*keep=*/0);
		if (scrubbed < 0)
		{
			// Destroying without unregistering is the crash. Leaking the weapon
			// costs one entity per life and keeps the server up, so prefer it.
			Warning(eDLL_T::SERVER,
				"[offhand-ext] Take: slot %d cannot reach the engine weapon list; "
				"leaking weapon %p rather than destroying it (see the "
				"[WeaponScriptVars] weapon-list warning at boot).\n", slot, pWeapon);
			return true;
		}

		Warning(eDLL_T::SERVER,
			"[offhand-ext] Take: slot %d unregistered weapon %p from %d engine "
			"weapon-list slot(s) before destroy.\n", slot, pWeapon, scrubbed);

		// Destroy takes the IHandleEntity offset embedded inside CWeapon
		// (pWeapon + 0x40).
		uint8_t* pHandleEnt = static_cast<uint8_t*>(pWeapon) + 64;
		v_CWeapon_DestroyEntity(pHandleEnt);
	}
	return true;
}

void* OffhandSlotsExt_GetEntity(void* pPlayer, int slot, SQVM* v)
{
	if (!pPlayer || !OffhandSlotsExt_IsExtendedSlot(slot))
		return nullptr;

	const bool isServerVM = (v && v->GetContext() == SQCONTEXT::SERVER);
	const uint32_t storedEH = OffhandExt_ReadSlot(pPlayer, slot);
	if (storedEH == kOffhandSlotExtInvalidHandle)
		return nullptr;

	const ESide side = isServerVM ? ESide::Server : ESide::Client;
	void* pEntity = SDKEntityState_Resolve(SDKEntityHandle(storedEH), side);
	if (!pEntity)
	{
		// Stale handle (entity destroyed, serial mismatch). Next Set/Take
		// rewrites the shadow / client DT -- nothing to evict here.
		Warning(isServerVM ? eDLL_T::SERVER : eDLL_T::CLIENT,
			"[offhand-ext] GetEntity: slot %d EH=0x%08X resolved to null "
			"(stale/serial-miss) for pPlayer=%p.\n", slot, storedEH, pPlayer);
		return nullptr;
	}

	// Vtable sanity check: freed instance, storage still hashed.
	if (!LooksLikeValidEntity(pEntity))
	{
		Warning(isServerVM ? eDLL_T::SERVER : eDLL_T::CLIENT,
			"[offhand-ext] GetEntity: slot %d EH=0x%08X resolved to pEntity=%p but "
			"the vtable looks invalid -- treating as stale (pPlayer=%p).\n",
			slot, storedEH, pEntity, pPlayer);
		return nullptr;
	}

	return pEntity;
}

// SDKEntityState_FlushAll also clears the map via the destroy subscription.
void OffhandSlotsExt_LevelShutdown()
{
	OffhandExt_ShadowMap().Clear();
}

void* OffhandSlotsExt_PlayerFromOffhandArrayBase(void* pArrayBase)
{
	if (!pArrayBase)
		return nullptr;
	return reinterpret_cast<void*>(
		reinterpret_cast<uintptr_t>(pArrayBase) - CPLAYER_OFFHAND_BASE);
}

int OffhandSlotsExt_SlotFromPropOffset(int propOffset)
{
	for (int slot = kOffhandSlotExtFirst; slot <= kOffhandSlotExtLast; ++slot)
	{
		const int rel = static_cast<int>(
			OffhandExt_SlotOffsetBytes(slot) - CPLAYER_OFFHAND_BASE);
		if (rel == propOffset)
			return slot;
	}
	return -1;
}