//=============================================================================//
//
// Purpose: seed C_WeaponX::m_burstFireCount from the burst_fire_count
// modvar at predicted PrimaryAttack when the engine's own mod-application
// push left it at zero. A zero latch makes every burst weapon fire
// continuously and latch fully-heated on its first round.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/client/bridge_fire_tap.h"

typedef __int64(__fastcall* PFN_C_WeaponX_PrimaryAttack)(__int64 weapon, __int64 a2, __int64 a3, __int64 a4);
static PFN_C_WeaponX_PrimaryAttack v_C_WeaponX_PrimaryAttack = nullptr;

static ConVar bridge_burst_seed_client("bridge_burst_seed_client", "1", FCVAR_RELEASE,
	"Seed C_WeaponX::m_burstFireCount from the burst_fire_count modvar at predicted "
	"PrimaryAttack when the engine's own mod-application push left it at zero. A zero "
	"latch makes every burst weapon fire continuously and latch fully-heated on its "
	"first round (0=off, 1=on).");

static constexpr ptrdiff_t S21WEAPON_OFF_MODVAR_BURSTFIRECOUNT = 6660;
static constexpr ptrdiff_t S21WEAPON_OFF_MBURSTFIRECOUNT = 5880;
static constexpr ptrdiff_t S21WEAPON_OFF_MBURSTFIREINDEX = 5884;

static __int64 __fastcall Hook_C_WeaponX_PrimaryAttack(__int64 weapon, __int64 a2, __int64 a3, __int64 a4)
{
	if (bridge_burst_seed_client.GetBool() && weapon)
	{
		int* const pLatch = reinterpret_cast<int*>(weapon + S21WEAPON_OFF_MBURSTFIRECOUNT);
		const int nModVarBurst = *reinterpret_cast<int*>(weapon + S21WEAPON_OFF_MODVAR_BURSTFIRECOUNT);
		if (*pLatch == 0 && nModVarBurst > 0
			&& *reinterpret_cast<int*>(weapon + S21WEAPON_OFF_MBURSTFIREINDEX) == 0)
		{
			*pLatch = nModVarBurst;
		}
	}

	return v_C_WeaponX_PrimaryAttack(weapon, a2, a3, a4);
}

///////////////////////////////////////////////////////////////////////////////
void VBridgeFireTapClient::GetAdr(void) const
{
	LogFunAdr("C_WeaponX_PrimaryAttack", v_C_WeaponX_PrimaryAttack);
}

///////////////////////////////////////////////////////////////////////////////
void VBridgeFireTapClient::GetFun(void) const
{
	Module_FindPattern(g_GameDll, "40 56 41 54 41 56 48 81 EC")
		.GetPtr(v_C_WeaponX_PrimaryAttack);

	if (!v_C_WeaponX_PrimaryAttack)
		Warning(eDLL_T::CLIENT, "[BURST-SEED] C_WeaponX::PrimaryAttack pattern unresolved\n");
}

///////////////////////////////////////////////////////////////////////////////
void VBridgeFireTapClient::GetVar(void) const
{
}

///////////////////////////////////////////////////////////////////////////////
void VBridgeFireTapClient::GetCon(void) const
{
}

///////////////////////////////////////////////////////////////////////////////
void VBridgeFireTapClient::Detour(const bool bAttach) const
{
	if (v_C_WeaponX_PrimaryAttack)
		DetourSetup(&v_C_WeaponX_PrimaryAttack, &Hook_C_WeaponX_PrimaryAttack, bAttach);
	else if (bAttach)
		Warning(eDLL_T::CLIENT, "[BURST-SEED] disabled: PrimaryAttack pattern unresolved\n");
}
