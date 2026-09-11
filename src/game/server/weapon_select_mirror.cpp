//=============================================================================//
//
// Purpose: Impl of m_selectedWeapons pending-mirror -- see header.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "weapon_select_mirror.h"

#include "game/server/util_server.h"
#include "game/server/player.h"
#include "game/server/gameinterface.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "game/server/jetdrive.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/weapon_enforce.h"
#include "game/server/akimbo.h"

#include <cstring>

// m_inventory at CPlayer+0x1688. weapons[0..8] +0x1690; offhandWeapons[0..7] +0x16B4.
static constexpr uintptr_t WEAPINV_WEAPONS_BASE = 0x1690; // weapons[0]
static constexpr uintptr_t WEAPINV_OFFHAND_BASE = 0x16B4; // offhandWeapons[0]

static ConVar bridge_weap_select_mirror("bridge_weap_select_mirror", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Mirror server-side weapon activations into the networked m_selectedWeapons "
	"pending field so the S21 client's native switch logic follows script-driven "
	"gives (SetActiveWeaponBySlot).");

static ConVar bridge_weap_select_mirror_hold("bridge_weap_select_mirror_hold", "1.0",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Seconds to hold the mirrored m_selectedWeapons value before resetting it to "
	"-1 (cleared). Must outlive at least one snapshot.");

// Server CBaseCombatCharacter offsets (S3 dedi): m_selectedWeapons +0x16D8,
// activeWeapons +0x16CC + 4*slot. Do not use client-half offsets.
static constexpr uintptr_t WEAPSEL_SELECTED_OFF     = 0x16D8; // m_selectedWeapons[2], int8 per active slot
static constexpr uintptr_t WEAPSEL_ACTIVE_NATIVE    = 0x16CC; // server activeWeapons[3] (DT-visible), EHandle per slot
static constexpr int8_t    WEAPSEL_SLOT_INVALID     = -1;     // WEAPON_INVENTORY_SLOT_INVALID

// Pending reset entries: after the hold expires, put m_selectedWeapons back to
// -1 (only if it still holds the exact value we wrote -- a newer selection,
// script- or engine-driven, always wins).
struct WeapSelPendingReset
{
	uint32_t playerEH = 0xFFFFFFFFu; // m_RefEHandle of the player (entity+8), 0xFFFFFFFF = free
	float    expiry   = 0.f;         // Plat_FloatTime deadline
	int8_t   wroteVal = -1;          // the backpack slot we mirrored
	uint8_t  slot     = 0;           // active slot 0/1
};
static WeapSelPendingReset s_weapSelPending[64];

// Raw mirror write from live SetActiveWeapon/UTIL player+weapon.
// Returns the backpack slot written, or -1 if the mirror did not apply.
static int WeapSelMirror_Apply(void* player, unsigned int slot, __int64 weaponEnt, uint32_t oldActiveEH)
{
	if (!player || !weaponEnt || slot >= 2)
		return -1;

	const uint32_t newEH = *reinterpret_cast<uint32_t*>(weaponEnt + 8);
	if (newEH == oldActiveEH) // no-op activation (native early-outs too)
		return -1;

	// Locate the weapon in the 9-slot S3 backpack; the wire grafts
	// weapons 1:1 by position, so this index IS the client-side
	// Inv_GetNormalWeapon_Client index.
	int backpackSlot = -1;
	for (int i = 0; i < 9; ++i)
	{
		if (*reinterpret_cast<uint32_t*>(
			reinterpret_cast<uintptr_t>(player) + WEAPINV_WEAPONS_BASE + 4u * i) == newEH)
		{
			backpackSlot = i;
			break;
		}
	}
	if (backpackSlot < 0)
		return -1; // not a backpack weapon (offhand path has its own channel)

	int8_t* const pSel = reinterpret_cast<int8_t*>(
		reinterpret_cast<uintptr_t>(player) + WEAPSEL_SELECTED_OFF + slot);
	if (*pSel != static_cast<int8_t>(backpackSlot))
		*pSel = static_cast<int8_t>(backpackSlot);
	return backpackSlot;
}

// Do not mirror client-initiated switches (slot keys / melee). Mirroring those re-Deploys ~RTT later.
static thread_local bool s_weapSelMirrorSuppressed = false;

// Backpack history by entity index (player+88). Sampled before the tick's game frame.
struct WeapSelBackpackHist
{
	uint32_t playerEH = 0xFFFFFFFFu;    // owner check (entity index reuse)
	uint32_t prev[9] = {};
	uint32_t cur[9]  = {};
	bool     valid   = false;
};
static WeapSelBackpackHist s_weapSelHist[128];

// True if the weapon EH was already in the player's backpack at the start of
// this or the previous tick (pre-owned -> the activation is a switch, not a
// give). Fails OPEN (returns false -> mirror) when no history exists yet.
static bool WeapSelMirror_WasPreOwned(void* player, uint32_t weaponEH)
{
	if (!player)
		return false;

	const uint16_t entIdx = *reinterpret_cast<uint16_t*>(
		reinterpret_cast<uintptr_t>(player) + 88);
	if (entIdx == 0xFFFF)
		return false;
	const WeapSelBackpackHist& h = s_weapSelHist[entIdx & 127];
	const uint32_t playerEH = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<uintptr_t>(player) + 8);
	if (!h.valid || h.playerEH != playerEH)
		return false;
	for (int i = 0; i < 9; ++i)
	{
		if (h.cur[i] == weaponEH || h.prev[i] == weaponEH)
			return true;
	}
	return false;
}

// Roll cur -> prev and snapshot weapons[9] before the engine game frame.
static void WeapSelMirror_SampleBackpacks()
{
	if (!bridge_weap_select_mirror.GetBool())
		return;
	if (!gpGlobals || !g_pServer)
		return;

	const int nMax = gpGlobals->maxClients;
	for (int i = 0; i < nMax; i++)
	{
		const CClient* const pClient = g_pServer->GetClient(i);
		if (!pClient || !pClient->IsActive())
			continue;

		CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());
		if (!pPlayer || !pPlayer->IsConnected())
			continue;

		const uint16_t entIdx = *reinterpret_cast<uint16_t*>(
			reinterpret_cast<uintptr_t>(pPlayer) + 88);
		if (entIdx == 0xFFFF)
			continue;

		WeapSelBackpackHist& h = s_weapSelHist[entIdx & 127];
		const uint32_t playerEH = *reinterpret_cast<uint32_t*>(
			reinterpret_cast<uintptr_t>(pPlayer) + 8);
		if (!h.valid || h.playerEH != playerEH)
		{
			// new occupant: seed both snapshots from live state
			h.playerEH = playerEH;
			for (int s = 0; s < 9; ++s)
				h.prev[s] = h.cur[s] = *reinterpret_cast<uint32_t*>(
					reinterpret_cast<uintptr_t>(pPlayer) + WEAPINV_WEAPONS_BASE + 4u * s);
			h.valid = true;
			continue;
		}
		memcpy(h.prev, h.cur, sizeof(h.prev));
		for (int s = 0; s < 9; ++s)
			h.cur[s] = *reinterpret_cast<uint32_t*>(
				reinterpret_cast<uintptr_t>(pPlayer) + WEAPINV_WEAPONS_BASE + 4u * s);
	}
}

void WeapSelMirror_SetSuppress(bool bSuppress)
{
	s_weapSelMirrorSuppressed = bSuppress;
}

static void WeapSelMirror_OnSetActiveWeapon(void* player, unsigned int slot, __int64 weaponEnt, uint32_t oldActiveEH)
{
	if (!bridge_weap_select_mirror.GetBool() || !weaponEnt || slot >= 2)
		return;

	if (s_weapSelMirrorSuppressed)
		return;

	// Only mirror NEWLY-ACQUIRED weapons (gives). Switches among pre-owned
	// weapons are client-driven on the bridge and must never be echoed.
	// Closed-form: player/weaponEnt are live engine pointers (SetActiveWeapon path).
	{
		const uint32_t newEH = *reinterpret_cast<uint32_t*>(weaponEnt + 8);
		if (newEH == 0xFFFFFFFFu)
			return;
		if (WeapSelMirror_WasPreOwned(player, newEH))
			return;
	}

	const int wrote = WeapSelMirror_Apply(player, slot, weaponEnt, oldActiveEH);
	if (wrote < 0)
		return;

	MarkEntityEdictDirty(player);

	// Schedule the reset-to--1. One live entry per player+slot.
	const uint32_t playerEH = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<uintptr_t>(player) + 8);
	if (playerEH != 0xFFFFFFFFu)
	{
		WeapSelPendingReset* pFree = nullptr;
		for (WeapSelPendingReset& e : s_weapSelPending)
		{
			if (e.playerEH == playerEH && e.slot == slot) { pFree = &e; break; }
			if (!pFree && e.playerEH == 0xFFFFFFFFu)
				pFree = &e;
		}
		if (pFree)
		{
			pFree->playerEH = playerEH;
			pFree->expiry   = static_cast<float>(Plat_FloatTime()) + bridge_weap_select_mirror_hold.GetFloat();
			pFree->wroteVal = static_cast<int8_t>(wrote);
			pFree->slot     = static_cast<uint8_t>(slot);
		}
	}
}

void WeaponSelectMirror_TickServer()
{
	WeapSelMirror_SampleBackpacks();

	const float now = static_cast<float>(Plat_FloatTime());
	for (WeapSelPendingReset& e : s_weapSelPending)
	{
		if (e.playerEH == 0xFFFFFFFFu || now < e.expiry)
			continue;

		void* const player = SDKEntityState_Resolve(SDKEntityHandle(e.playerEH), ESide::Server);
		if (player)
		{
			int8_t* const pSel = reinterpret_cast<int8_t*>(
				reinterpret_cast<uintptr_t>(player) + WEAPSEL_SELECTED_OFF + e.slot);
			if (*pSel == e.wroteVal) // untouched since our mirror -> consume
			{
				*pSel = WEAPSEL_SLOT_INVALID;
				MarkEntityEdictDirty(player);
			}
		}
		e.playerEH = 0xFFFFFFFFu; // free the entry (player gone counts as done)
	}
}

static __int64 __fastcall Hook_SetActiveWeapon(void* player, unsigned int slot, __int64 weaponEnt)
{
	// Capture the pre-call occupant of the native active array; the original
	// overwrites it, and the mirror must only fire on a REAL change.
	uint32_t oldActiveEH = 0xFFFFFFFFu;
	if (player && slot < 3)
	{
		oldActiveEH = *reinterpret_cast<uint32_t*>(
			reinterpret_cast<uintptr_t>(player) + WEAPSEL_ACTIVE_NATIVE + 4u * slot);
	}

	if (player && weaponEnt
		&& WeaponEnforce_Check(player, reinterpret_cast<void*>(weaponEnt))
			== WeaponEnforceResult::BlockedByType)
	{
		static int s_nBlockLog = 16;
		if (s_nBlockLog > 0)
		{
			--s_nBlockLog;
			Warning(eDLL_T::SERVER,
				"[WeaponEnforce] blocked SetActiveWeapon slot=%u player=%p weapon=%p\n",
				slot, player, reinterpret_cast<void*>(weaponEnt));
		}
		return 0;
	}

	if (player && weaponEnt
		&& JetDrive_ShouldBlockSetActiveWeapon(player, reinterpret_cast<void*>(weaponEnt)))
	{
		static int s_nJdBlockLog = 8;
		if (s_nJdBlockLog > 0)
		{
			--s_nJdBlockLog;
			Warning(eDLL_T::SERVER,
				"[JETDRIVE] blocked SetActiveWeapon slot=%u player=%p weapon=%p\n",
				slot, player, reinterpret_cast<void*>(weaponEnt));
		}
		return 0;
	}

	if (player && weaponEnt && slot == 1
		&& !AkimboBridge_CanActivateAlthand(player, reinterpret_cast<void*>(weaponEnt)))
		return 0;

	const __int64 result = v_SetActiveWeapon(player, slot, weaponEnt);

	if (player)
		WeapSelMirror_OnSetActiveWeapon(player, slot, weaponEnt, oldActiveEH);

	if (player)
		AkimboBridge_OnSetActiveWeapon(player, slot, reinterpret_cast<void*>(weaponEnt));

	return result;
}

static __int16 __fastcall Hook_WeaponDirectSelect(void* player, uint16_t word)
{
	// ucmd-driven select = client-initiated (melee V-key etc.); the client
	// already switched locally -- suppress the m_selectedWeapons mirror for any
	// SetActiveWeapon this handler performs downstream.
	WeapSelMirror_SetSuppress(true);
	const __int16 dselResult = v_WeaponDirectSelect(player, word);
	WeapSelMirror_SetSuppress(false);
	return dselResult;
}

void WeaponSelectMirror_GetAdr(void)
{
	LogFunAdr("SetActiveWeapon", v_SetActiveWeapon);
	LogFunAdr("WeaponDirectSelect", v_WeaponDirectSelect);
}

void WeaponSelectMirror_GetFun(void)
{
	// Real server CBaseCombatCharacter::SetActiveWeapon (player, slot, weaponEnt):
	// activeWeapons[slot] @ +0x16CC. Wrong half would attach but never run on dedi.
	Module_FindPattern(g_GameDll,
		"40 53 55 57 41 55 41 56 41 57 48 83 EC 48 4C 63 EA 4C 8B F1 BB FF FF FF FF 49 8B E8 42 8B 8C A9 CC 16 00 00 8B C1 3B CB 74 20 0F B7 C1 C1 E9 10 48 8D 3C 40 48 03 FF 48 8D 05 ?? ?? ?? ??")
		.GetPtr(v_SetActiveWeapon);
	if (!v_SetActiveWeapon)
		Warning(eDLL_T::SERVER,
			"[WEAP-SEL] SetActiveWeapon pattern UNRESOLVED -- mirror NOT installed\n");

	// S3 direct weapon-select handler. Hook for [WEAP-DSEL].
	Module_FindPattern(g_GameDll,
		"40 55 48 83 EC 40 0F B7 C2 48 8B E9 66 C1 E8 08 84 C0 0F 88 40 02 00 00 4C 0F BE C0 48 89 5C 24 50 41 83 F8 09")
		.GetPtr(v_WeaponDirectSelect);
	if (!v_WeaponDirectSelect)
		Warning(eDLL_T::SERVER,
			"[WEAP-DSEL] WeaponDirectSelect pattern UNRESOLVED -- hook NOT installed\n");
}

void WeaponSelectMirror_Detour(const bool bAttach)
{
	if (v_SetActiveWeapon)
	{
		const LONG rSAW = bAttach
			? DetourAttach(reinterpret_cast<void**>(&v_SetActiveWeapon),
				reinterpret_cast<void*>(&Hook_SetActiveWeapon))
			: DetourDetach(reinterpret_cast<void**>(&v_SetActiveWeapon),
				reinterpret_cast<void*>(&Hook_SetActiveWeapon));
		if (bAttach)
			Msg(eDLL_T::SERVER,
				"[WEAP-SEL] DetourAttach SetActiveWeapon result=0x%lX (target=0x%p)\n",
				rSAW, (void*)v_SetActiveWeapon);
	}
	if (v_WeaponDirectSelect)
	{
		const LONG rWDS = bAttach
			? DetourAttach(reinterpret_cast<void**>(&v_WeaponDirectSelect),
				reinterpret_cast<void*>(&Hook_WeaponDirectSelect))
			: DetourDetach(reinterpret_cast<void**>(&v_WeaponDirectSelect),
				reinterpret_cast<void*>(&Hook_WeaponDirectSelect));
		if (bAttach)
			Msg(eDLL_T::SERVER,
				"[WEAP-DSEL] DetourAttach WeaponDirectSelect result=0x%lX (target=0x%p)\n",
				rWDS, (void*)v_WeaponDirectSelect);
	}
}

