//=============================================================================//
//
// Purpose: infinite_ammo.h implementation. See that header.
//
//=============================================================================//
#include "core/stdafx.h"


#include "infinite_ammo.h"
#include "game/shared/weapon_script_vars.h"

// The reserve S21 C_WeaponX::GetAmmo returns for AMMOSOURCE_INFINITE.
static constexpr int64_t INFINITE_AMMO_COUNT = 999;

// LIVE CWeaponX "this weapon has an ammo system" -- the first test in both
// CanReload and HasAmmoEventually. Zero means the answer is NO regardless of
// reserve, so infinite ammo must not override it.
static constexpr uintptr_t WEAPONX_OFF_USESAMMO = 0x122C;

// S3 has no native infinite-ammo surface. Per-weapon sidecar; empty is stock passthrough.
static ConVar bridge_infinite_ammo("bridge_infinite_ammo", "1", FCVAR_RELEASE,
	"Honor InfiniteAmmoState on the S3 dedi's native ammo paths (999 reserve, "
	"no-op remove, reload gates answered as if the reserve were full) so the "
	"authoritative weapon mirrors the S21 client's native infinite prediction. "
	"1 = S21 infinite (default), 0 = S3 stock.");

//-----------------------------------------------------------------------------
// GetAmmoCount is the funnel; CanReload and HasAmmoEventually read the reserve inline.
//-----------------------------------------------------------------------------
static int64_t (*v_CWeaponX_GetAmmoCount)(int64_t weapon, int source) = nullptr;
static void    (*v_CWeaponX_RemoveAmmo)(int64_t weapon, int source, uint32_t count) = nullptr;
static char    (*v_CWeaponX_CanReload)(int64_t weapon) = nullptr;
static bool    (*v_CWeaponX_HasAmmoEventually)(int64_t weapon) = nullptr;

static inline bool InfiniteAmmo_Enforce(int64_t weapon)
{
	return bridge_infinite_ammo.GetBool() && weapon &&
		WeaponScriptVars_GetInfiniteAmmoState(reinterpret_cast<const void*>(weapon)) != 0;
}

static inline bool InfiniteAmmo_WeaponUsesAmmo(int64_t weapon)
{
	return *reinterpret_cast<const int*>(weapon + WEAPONX_OFF_USESAMMO) != 0;
}

//-----------------------------------------------------------------------------
// S21 semantics: state != 0 reads 999 regardless of the source argument.
// This is the whole reserve contract -- see the funnel note above.
//-----------------------------------------------------------------------------
static int64_t __fastcall Hook_GetAmmoCount(int64_t weapon, int source)
{
	if (InfiniteAmmo_Enforce(weapon))
		return INFINITE_AMMO_COUNT;

	return v_CWeaponX_GetAmmoCount(weapon, source);
}

//-----------------------------------------------------------------------------
// state != 0 suppresses reserve removal. Magazine still drains via the clip path.
//-----------------------------------------------------------------------------
static void __fastcall Hook_RemoveAmmo(int64_t weapon, int source, uint32_t count)
{
	if (InfiniteAmmo_Enforce(weapon))
		return;

	v_CWeaponX_RemoveAmmo(weapon, source, count);
}

//-----------------------------------------------------------------------------
// Inline reserve readers. Promote false->true on infinite; do not force true.
//-----------------------------------------------------------------------------
static char __fastcall Hook_CanReload(int64_t weapon)
{
	const char result = v_CWeaponX_CanReload(weapon);
	if (result || !InfiniteAmmo_Enforce(weapon) || !InfiniteAmmo_WeaponUsesAmmo(weapon))
		return result;

	return 1;
}

static bool __fastcall Hook_HasAmmoEventually(int64_t weapon)
{
	const bool result = v_CWeaponX_HasAmmoEventually(weapon);
	if (result || !InfiniteAmmo_Enforce(weapon) || !InfiniteAmmo_WeaponUsesAmmo(weapon))
		return result;

	return true;
}

void VInfiniteAmmoDedi::GetAdr(void) const
{
	LogFunAdr("CWeaponX::GetAmmoCount", v_CWeaponX_GetAmmoCount);
	LogFunAdr("CWeaponX::RemoveAmmo", v_CWeaponX_RemoveAmmo);
	LogFunAdr("CWeaponX::CanReload", v_CWeaponX_CanReload);
	LogFunAdr("CWeaponX::HasAmmoEventually", v_CWeaponX_HasAmmoEventually);
}

void VInfiniteAmmoDedi::GetFun(void) const
{
	// CWeaponX::GetAmmoCount(weapon, source) -- engine helper. Stockpile at
	// +0x1228 for source 0, else the owner's m_ammoPoolCount via the entity
	// handle table (the `lea rax, [rip+disp]` is wildcarded).
	Module_FindPattern(g_GameDll,
		"4C 8B C1 85 D2 75 07 8B 81 28 12 00 00 C3 8B 89 F0 11 00 00 83 F9 FF 74 20 0F B7 C1 C1 E9 10 48 8D 14 40 48 03 D2 48 8D 05 ?? ?? ?? ?? 39 4C D0 08")
		.GetPtr(v_CWeaponX_GetAmmoCount);
	if (!v_CWeaponX_GetAmmoCount)
		Warning(eDLL_T::SERVER, "[INF-AMMO] CWeaponX::GetAmmoCount pattern unresolved -- infinite ammo enforcement disabled\n");

	// CWeaponX::RemoveAmmo(weapon, source, count) -- engine helper. Interior
	// rel32 branch displacements wildcarded.
	Module_FindPattern(g_GameDll,
		"45 8B D8 4C 8B C9 85 D2 0F 85 ?? ?? ?? ?? 38 91 F5 1A 00 00 0F 85 ?? ?? ?? ?? 8B 81 28 12 00 00 41 3B C0 73 38 85 C0 0F 84 ?? ?? ?? ?? 0F B7 41 58 66 83 F8 FF 74 1C")
		.GetPtr(v_CWeaponX_RemoveAmmo);
	if (!v_CWeaponX_RemoveAmmo)
		Warning(eDLL_T::SERVER, "[INF-AMMO] CWeaponX::RemoveAmmo pattern unresolved -- infinite reserve drain not suppressed\n");

	// CanReload vs HasAmmoEventually: shared +0x122C prologue; discriminator is
	// clip +0x1AC8 vs burst +0x1AE0.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 83 B9 2C 12 00 00 00 48 8B D9 75 08 32 C0 48 83 C4 20 5B C3 83 B9 C8 1A 00 00 00 7E 39 80 B9 E0 1A 00 00 00 74 10 83 B9 64 15 00 00 00")
		.GetPtr(v_CWeaponX_CanReload);
	if (!v_CWeaponX_CanReload)
		Warning(eDLL_T::SERVER, "[INF-AMMO] CWeaponX::CanReload pattern unresolved -- infinite reload gate disabled\n");

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 83 B9 2C 12 00 00 00 48 8B D9 75 08 32 C0 48 83 C4 20 5B C3 80 B9 E0 1A 00 00 00 48 89 74 24 30 48 89 7C 24 38 4C 89 74 24 40")
		.GetPtr(v_CWeaponX_HasAmmoEventually);
	if (!v_CWeaponX_HasAmmoEventually)
		Warning(eDLL_T::SERVER, "[INF-AMMO] CWeaponX::HasAmmoEventually pattern unresolved -- infinite fire gate disabled\n");
}

void VInfiniteAmmoDedi::Detour(const bool bAttach) const
{
	if (v_CWeaponX_GetAmmoCount)
		DetourSetup(&v_CWeaponX_GetAmmoCount, &Hook_GetAmmoCount, bAttach);
	if (v_CWeaponX_RemoveAmmo)
		DetourSetup(&v_CWeaponX_RemoveAmmo, &Hook_RemoveAmmo, bAttach);
	if (v_CWeaponX_CanReload)
		DetourSetup(&v_CWeaponX_CanReload, &Hook_CanReload, bAttach);
	if (v_CWeaponX_HasAmmoEventually)
		DetourSetup(&v_CWeaponX_HasAmmoEventually, &Hook_HasAmmoEventually, bAttach);
}

