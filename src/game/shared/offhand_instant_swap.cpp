//=============================================================================//
// Purpose: return-address-scoped OR-escape on IsOffhandInInteruptResumeableState.
//=============================================================================//
#include "core/stdafx.h"
#include "offhand_instant_swap.h"
#include "public/tier0/memaddr.h"
#include "public/tier0/module.h"
#include "public/tier0/tier0_iface.h"
#include "thirdparty/detours/include/detours.h"

#if !defined(CLIENT_DLL)
#include "game/server/weapon_kv_s21_ext.h"
#endif // !CLIENT_DLL

#include <intrin.h>
#include <cstdint>

static char (*v_IsOffhandInInteruptResumeableState)(int64_t pWeapon) = nullptr;
static const void* s_pGateRetAddr = nullptr; // dispatcher call site + 5 (see GetFun)

//-----------------------------------------------------------------------------
static ConVar bridge_offhand_instant_swap("bridge_offhand_instant_swap", "1", FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Honor the S21 'offhand_instant_swap_to_offhand' weapon KV in the S3 offhand "
	"dispatcher's switch-away gate (S3 predates the modvar; without this, "
	"entry-shim tacticals like Vantage's block their real weapon's activation).");
static ConVar bridge_offhand_instant_swap_diag("bridge_offhand_instant_swap_diag", "0", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL,
	"Log the switch-away gate override once per active offhand.");

static constexpr uintptr_t OFFHANDISWAP_WEAPNAME_OFF = 0x15B0; // server CWeapon m_weaponName[65]

// offhand_instant_swap_to_offhand as the weapon's active mods resolve it.
static bool OffhandISwap_WeaponHasFlag(int64_t pWeapon)
{
#if !defined(CLIENT_DLL)
	return WeaponKVS21Ext_GetBool(reinterpret_cast<const void*>(pWeapon), WeaponS21Bool_e::OFFHAND_INSTANT_SWAP_TO_OFFHAND);
#else
	return false;
#endif // !CLIENT_DLL
}

//-----------------------------------------------------------------------------
static char Hook_IsOffhandInInteruptResumeableState(int64_t pWeapon)
{
	const char result = v_IsOffhandInInteruptResumeableState(pWeapon);
	if (result)
		return result;

	if (_ReturnAddress() != s_pGateRetAddr) // only the dispatcher's switch-away gate; other callers keep bare semantics
		return result;

	if (!pWeapon || !bridge_offhand_instant_swap.GetBool())
		return result;

	if (!OffhandISwap_WeaponHasFlag(pWeapon))
		return result;

	// The dispatcher reaches this gate every command while the offhand is up.
	static int64_t s_nLastLogged = 0;
	if (bridge_offhand_instant_swap_diag.GetBool() && pWeapon != s_nLastLogged)
	{
		s_nLastLogged = pWeapon;
		Msg(eDLL_T::SERVER, "[OFFHAND-ISWAP] switch-away gate override: active offhand '%s' has "
			"offhand_instant_swap_to_offhand\n", reinterpret_cast<const char*>(pWeapon + OFFHANDISWAP_WEAPNAME_OFF));
	}

	return 1;
}

//-----------------------------------------------------------------------------
void VOffhandInstantSwap::GetAdr(void) const
{
	LogFunAdr("IsOffhandInInteruptResumeableState", v_IsOffhandInInteruptResumeableState);
	LogVarAdr("OffhandISwapGateRetAddr", s_pGateRetAddr);
}

void VOffhandInstantSwap::GetFun(void) const
{
	// IsOffhandInInteruptResumeableState prologue (fireMode/weapState gate).
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 83 B9 50 27 00 00 03 75 4F 8B 91 34 12 00 00")
		.GetPtr(v_IsOffhandInInteruptResumeableState);

	// Dispatcher switch-away gate call site (modvar-union path).
	CMemory gate = Module_FindPattern(g_GameDll,
		"41 8B 8C 24 50 27 00 00 8D 41 FF 83 F8 04 77 3F 4D 3B EC 75 2C 83 F9 05 74 10 49 8B CC E8");

	if (gate)
		s_pGateRetAddr = gate.Offset(0x22).RCast<const void*>(); // E8 at +0x1D; return address = +0x22

	Msg(eDLL_T::SERVER, "[OFFHAND-ISWAP] GetFun() resolution: predicate=%p gateRetAddr=%p\n",
		reinterpret_cast<void*>(v_IsOffhandInInteruptResumeableState), s_pGateRetAddr);

	if (!v_IsOffhandInInteruptResumeableState)
	{
		Warning(eDLL_T::SERVER, "[OFFHAND-ISWAP] PATTERN MISS -- "
			"IsOffhandInInteruptResumeableState unresolved, instant-swap escape will NOT fire.\n");
	}

	if (!s_pGateRetAddr)
	{
		Warning(eDLL_T::SERVER, "[OFFHAND-ISWAP] PATTERN MISS -- dispatcher switch-away gate call "
			"site unresolved, instant-swap escape will NOT fire.\n");
	}
}

void VOffhandInstantSwap::GetVar(void) const
{
}

void VOffhandInstantSwap::Detour(const bool bAttach) const
{
	// Without the gate return address the override cannot be scoped to the
	// single -parity call site -- attaching would wrongly widen all 4 S3
	// callers of the predicate. Skip loudly instead.
	if (!v_IsOffhandInInteruptResumeableState || !s_pGateRetAddr)
	{
		Warning(eDLL_T::SERVER, "[OFFHAND-ISWAP] Detour() skipped -- missing resolved "
			"predicate and/or gate return address (see GetFun() warnings above).\n");
		return;
	}

	DetourSetup(&v_IsOffhandInInteruptResumeableState, &Hook_IsOffhandInInteruptResumeableState, bAttach);
}
