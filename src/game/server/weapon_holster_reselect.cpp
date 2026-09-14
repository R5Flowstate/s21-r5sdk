//=============================================================================//
//
// Purpose: unholster a leftover-active weapon.
//
// Native holster writes m_selectedWeapons[hand] = 0xFD and does not clear
// activeWeapons[]. Select then does GetSelectedWeapon == null &&
// Inv_IsWeaponActive(target) -> return, so a redraw of that same gun never
// writes selected. Traversal disable Holster() on the client drops the local
// active handle, so prediction draws; the dedi never runs that path and the
// snapshot stays 0xFD (HUD holding pip for one predicted frame).
//
// After native Select returns still-holstered with the requested gun still
// in activeWeapons[], run the native apply so selected is written.
//
//=============================================================================//
#include "core/stdafx.h"
#include "weapon_holster_reselect.h"

#include "tier0/dbg.h"
#include "tier0/module.h"
#include "tier1/convar.h"
#include "public/tier0/memaddr.h"
#include "game/shared/sdk_entity_state.h"
#include "public/game/shared/weapon_types.h"

static constexpr uintptr_t HOLSTER_WEAPONS_BASE = 0x1690; // weapons[9]
static constexpr uintptr_t HOLSTER_ACTIVE_BASE  = 0x16CC; // activeWeapons[3]
static constexpr uintptr_t HOLSTER_SELECTED_OFF = 0x16D8; // m_selectedWeapons[2]
static constexpr uintptr_t HOLSTER_BLOCK_OFF    = 0x6280; // native Select refuse-all
static constexpr int HOLSTER_ACTIVE_COUNT       = 3;
static constexpr unsigned int HOLSTER_INV_SLOTS = 9;

static ConVar bridge_holster_reselect("bridge_holster_reselect", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"When m_selectedWeapons is holstered, allow a usercmd select of a gun "
	"still sitting in activeWeapons[] to write the slot (native already-active "
	"early-out skips that write).");

static ConVar bridge_holster_reselect_diag("bridge_holster_reselect_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log leftover-active holster redraws that native Select skipped.");

static void(__fastcall* v_Weapon_Select_Internal)(void*, unsigned int, unsigned char, char) = nullptr;
static char(__fastcall* v_Weapon_Select_Apply)(void*, unsigned int, void*) = nullptr;

static bool HolsterReselect_InActive(const void* const player, const uint32_t weaponEH)
{
	if (!player || weaponEH == 0xFFFFFFFFu || weaponEH == 0)
		return false;

	const uintptr_t base = reinterpret_cast<uintptr_t>(player);
	for (int i = 0; i < HOLSTER_ACTIVE_COUNT; ++i)
	{
		if (*reinterpret_cast<const uint32_t*>(base + HOLSTER_ACTIVE_BASE + 4u * i) == weaponEH)
			return true;
	}
	return false;
}

static void __fastcall Hook_Weapon_Select_Internal(void* player, unsigned int hand,
	unsigned char slot, char setlast)
{
	v_Weapon_Select_Internal(player, hand, slot, setlast);

	if (!bridge_holster_reselect.GetBool() || !v_Weapon_Select_Apply || !player)
		return;
	if (hand >= 2 || slot >= HOLSTER_INV_SLOTS)
		return;
	if (*reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(player) + HOLSTER_BLOCK_OFF))
		return;

	const uintptr_t base = reinterpret_cast<uintptr_t>(player);
	if (*reinterpret_cast<const int8_t*>(base + HOLSTER_SELECTED_OFF + hand)
		!= static_cast<int8_t>(WEAPON_INVENTORY_SLOT_HOLSTERED))
		return;

	const uint32_t weaponEH = *reinterpret_cast<const uint32_t*>(
		base + HOLSTER_WEAPONS_BASE + 4u * slot);
	if (!HolsterReselect_InActive(player, weaponEH))
		return;

	void* const weapon = SDKEntityState_Resolve(SDKEntityHandle(weaponEH), ESide::Server);
	if (!weapon)
		return;

	if (bridge_holster_reselect_diag.GetBool())
	{
		static unsigned s_nLogged = 0;
		if (s_nLogged < 32)
		{
			++s_nLogged;
			Warning(eDLL_T::SERVER,
				"[HOLSTER-RESEL] hand=%u slot=%u leftover-active -- apply\n",
				hand, static_cast<unsigned>(slot));
		}
	}

	v_Weapon_Select_Apply(player, hand, weapon);
}

void VHolsterReselect::GetAdr(void) const
{
	LogFunAdr("Weapon_Select_Internal", v_Weapon_Select_Internal);
	LogFunAdr("Weapon_Select_Apply", v_Weapon_Select_Apply);
}

void VHolsterReselect::GetFun(void) const
{
	// Server Select_Internal. Prologue cmp [rcx+6ECCh] is this half;
	// unique on the dedi.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 55 56 41 57 48 83 EC 20 80 B9 CC 6E 00 00 00")
		.GetPtr(v_Weapon_Select_Internal);

	if (!v_Weapon_Select_Internal)
		Warning(eDLL_T::SERVER,
			"[HOLSTER-RESEL] Weapon_Select_Internal pattern unresolved -- hook NOT installed\n");

	// Apply after the already-active early-out (same-hand leftover writes
	// selected; other-slot leftover does the full switch).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC ?? 48 63 EA")
		.GetPtr(v_Weapon_Select_Apply);

	if (!v_Weapon_Select_Apply)
		Warning(eDLL_T::SERVER,
			"[HOLSTER-RESEL] Weapon_Select_Apply pattern unresolved -- hook NOT installed\n");
}

void VHolsterReselect::Detour(const bool bAttach) const
{
	if (v_Weapon_Select_Internal && v_Weapon_Select_Apply)
		DetourSetup(&v_Weapon_Select_Internal, &Hook_Weapon_Select_Internal, bAttach);
}
