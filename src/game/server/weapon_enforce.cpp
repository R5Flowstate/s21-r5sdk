//=============================================================================//
//
// Purpose: Impl of engine-level DisableWeaponTypes enforcement -- see header.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "public/const.h"
#include "game/shared/weapon_enforce.h"
#include "game/shared/weapon_script_vars.h"
#include "game/shared/sdk_entity_state.h"

#include "game/shared/edict_dirty.h"
#include "game/server/basecombatcharacter.h"
#include "game/server/util_server.h"
#include "game/server/player.h"
#include "game/server/gameinterface.h"
#include "engine/server/server.h"
#include "engine/client/client.h"

//-----------------------------------------------------------------------------
// S3 weapon type flags offset.
//
//
// Both are the script-native backing for "GetWeaponTypeFlags" registered on
// their respective VMs. Offsets differ because server CWeapon has additional
// m_On*Pickup string handlers not present on the client variant. Verified

//-----------------------------------------------------------------------------
static constexpr uintptr_t kWeaponTypeFlagsOffsetClient = 0x1A40;
static constexpr uintptr_t kWeaponTypeFlagsOffsetServer = 0x19B0;

//-----------------------------------------------------------------------------
// WPT_VIEWHANDS = bit 8 (SDK-added flag, see weapon_script_vars.cpp:1487).
// reserves bit 0 as a non-disable-able "default" sentinel -- S3 has no
// such sentinel (bit 0 is WPT_PRIMARY). Viewhands is S3's actual last-resort
// weapon state, so we treat it as sticky: the enforcement sweep never clears
// an active slot whose weapon carries WPT_VIEWHANDS, even if the disabled
// mask includes bit 8. This prevents a script-authored mask that accidentally
// includes WPT_VIEWHANDS from stranding the player with no selectable weapon.
//-----------------------------------------------------------------------------
static constexpr uint32_t kWeaponTypeBitViewhands = 0x100u;
static constexpr uint32_t kWeaponTypeBitTactical  = 0x004u;

//-----------------------------------------------------------------------------
// m_inventory offset on CBaseCombatCharacter (server). Derived from the SDK
// shadow struct at src/game/server/basecombatcharacter.h -- the field follows
// gap_1684[4], so m_inventory starts at 0x1688. Static_assert on the class
// size (0x5960) guards against layout drift.
// activeWeapons[3] is at m_inventory + 8 (vtable) + 9*4 (weapons) + 6*4
// (offhands) = +0x44.
//-----------------------------------------------------------------------------
static constexpr uintptr_t kServerInventoryOffset = 0x1688;

//-----------------------------------------------------------------------------
// Runtime gate -- lets us disable enforcement at console without redeploying if
// the offset is wrong on a future engine revision. Default 1 (on).
//-----------------------------------------------------------------------------
static ConVar sdk_weapon_enforce("sdk_weapon_enforce", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Engine-level enforcement for DisableWeaponTypes (0=off, 1=on).");

//-----------------------------------------------------------------------------
// GetWeaponTypeFlags -- direct field read, no vtable call, matches the engine
// impl semantically. Side-specific offset: the weapon pointer's provenance
// (server entity list vs client entity list) determines which layout applies.
//-----------------------------------------------------------------------------
static inline uint32_t ReadWeaponTypeFlagsServer(const void* pWeapon)
{
	if (!pWeapon) return 0;
	return *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(pWeapon) + kWeaponTypeFlagsOffsetServer);
}

static inline uint32_t ReadWeaponTypeFlagsClient(const void* pWeapon)
{
	if (!pWeapon) return 0;
	return *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(pWeapon) + kWeaponTypeFlagsOffsetClient);
}

//-----------------------------------------------------------------------------
// WeaponEnforce_Check
//-----------------------------------------------------------------------------
WeaponEnforceResult WeaponEnforce_Check(const void* pPlayer, const void* pWeapon)
{
	if (!sdk_weapon_enforce.GetBool())
		return WeaponEnforceResult::Allow;
	if (!pPlayer || !pWeapon)
		return WeaponEnforceResult::Allow;

	const uint32_t disabled = WeaponScriptVars_GetDisabledFlagsForEntity(pPlayer);
	if (disabled == 0)
		return WeaponEnforceResult::Allow;

	// The decider is currently callable only from server-side enforcement
	// (WeaponEnforce_ForceSwapIfNowDisabled below). If/when we add client-side
	// prediction-parity hooks, add a side-aware ReadWeaponTypeFlags.
	const uint32_t weaponFlags = ReadWeaponTypeFlagsServer(pWeapon);
	if ((weaponFlags & disabled) != 0)
		return WeaponEnforceResult::BlockedByType;

	return WeaponEnforceResult::Allow;
}

//-----------------------------------------------------------------------------
// WeaponEnforce_ForceSwapIfNowDisabled
//
// Called on the server VM right after Script_DisableWeaponTypes bumps the
// disabled flags. For each activeWeapons[slot] whose type is now disabled:
// prefer CWeaponX::HolsterInternal (already pattern-resolved for script
// Holster/FastHolster), then Term the slot EHandle so next-tick re-selection
// can pick a non-disabled fallback. Dirty-mark the player so the clear
// replicates without waiting on an unrelated write.
//-----------------------------------------------------------------------------
void WeaponEnforce_ForceSwapIfNowDisabled(void* pPlayer)
{
	if (!sdk_weapon_enforce.GetBool() || !pPlayer)
		return;

	// Re-entry guard: if a slot-clear triggers a script callback that calls
	// DisableWeaponTypes again we'd recurse indefinitely. Thread-local because
	// the server VM may run on a worker thread during RPC dispatch.
	static thread_local bool s_inForceSwap = false;
	if (s_inForceSwap)
		return;

	const uint32_t disabled = WeaponScriptVars_GetDisabledFlagsForEntity(pPlayer);
	if (disabled == 0)
		return;  // fast-path: no disabled mask -> nothing to enforce

	s_inForceSwap = true;

	WeaponInventory* const pInv = reinterpret_cast<WeaponInventory*>(
		reinterpret_cast<uint8_t*>(pPlayer) + kServerInventoryOffset);

	// play_one_handed_alt_hand_anim_on_mainhand plays the tac's left-hand
	// pose on the mainhand VM. Holster/Term of that slot deletes the host;
	// the script FastHolster is the only put-away the S21 client runs.
	bool bKeepAnimHost = false;
	for (int slot = 0; slot < 3; slot++)
	{
		SDKEntityHandle h(static_cast<uint32_t>(pInv->activeWeapons[slot].ToInt()));
		if (!h.IsValid())
			continue;
		void* const pKeep = SDKEntityState_Resolve(h, ESide::Server);
		if (!pKeep)
			continue;
		const uint32_t keepFlags = ReadWeaponTypeFlagsServer(pKeep);
		if ((keepFlags & kWeaponTypeBitTactical) != 0 && (keepFlags & disabled) == 0)
		{
			bKeepAnimHost = true;
			break;
		}
	}

	bool bMutated = false;
	static int s_nClearLogBudget = 32;

	for (int slot = 0; slot < 3; slot++)
	{
		SDKEntityHandle h(static_cast<uint32_t>(pInv->activeWeapons[slot].ToInt()));
		if (!h.IsValid())
			continue;

		void* const pWeapon = SDKEntityState_Resolve(h, ESide::Server);
		if (!pWeapon)
			continue;

		const uint32_t weaponFlags = ReadWeaponTypeFlagsServer(pWeapon);
		if ((weaponFlags & disabled) == 0)
			continue;

		// Sticky viewhands: never clear the last-resort hands weapon, even if
		// the script-provided disabled mask happens to include WPT_VIEWHANDS.
		// Without this, a mask like (WPT_ALL | WPT_VIEWHANDS) would strand the
		// player with zero selectable weapons -- S3 has no sub-viewhands
		// fallback. The refcount/bitmask stays authoritative at the script
		// layer; this is purely a swap-safety rail.
		if ((weaponFlags & kWeaponTypeBitViewhands) != 0)
		{
			continue;
		}

		// Prefer the resolved holster path so anim/state unwind matches a real
		// put-away before we drop the active EHandle. SetActiveWeapon is only
		// resolved inside snapshot_diag (diag detour), not a shared helper.
		if (bKeepAnimHost)
			continue;

		const bool bHolstered = v_WeaponX_HolsterInternal
			? (v_WeaponX_HolsterInternal(pWeapon, /*bDoFastHolster=*/true) != 0)
			: false;

		pInv->activeWeapons[slot].Term();
		bMutated = true;

		if (s_nClearLogBudget > 0)
		{
			--s_nClearLogBudget;
			DevMsg(eDLL_T::SERVER,
				"[WeaponEnforce] cleared disabled active slot=%d player=%p weapon=%p holster=%d\n",
				slot, pPlayer, pWeapon, bHolstered ? 1 : 0);
		}
	}

	if (bMutated)
		MarkEntityEdictDirty(pPlayer);

	s_inForceSwap = false;
}

//-----------------------------------------------------------------------------
// WeaponEnforce_TickAllServer -- per-tick sweep to catch engine-driven weapon
// switches (V-key, 1/2 slot select, TAB cycle) that bypass the script call.
//-----------------------------------------------------------------------------
void WeaponEnforce_TickAllServer()
{
	WeaponScriptVars_FlushDisableHoldFlags();

	if (!sdk_weapon_enforce.GetBool())
		return;
	if (!gpGlobals || !g_pServer)
		return;

	// Gate on g_pServer->GetClient(i)->IsActive before calling
	// UTIL_PlayerByIndex -- for slots that are allocated but not currently
	// in use, UTIL_PlayerByIndex returns a bogus low-address pointer
	// (reinterpret_cast of an uninitialized edict entry). Canonical safe
	// pattern per physics_main.cpp: check the engine-side client first, then
	// map to the game-side CPlayer via the client's handle.
	const int nMax = gpGlobals->maxClients;
	for (int i = 0; i < nMax; i++)
	{
		const CClient* const pClient = g_pServer->GetClient(i);
		if (!pClient || !pClient->IsActive())
			continue;

		CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());
		if (!pPlayer || !pPlayer->IsConnected())
			continue;

		WeaponEnforce_ForceSwapIfNowDisabled(pPlayer);
	}
}
