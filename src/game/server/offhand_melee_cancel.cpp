//=============================================================================//
//
// Purpose: offhand_melee_cancel.h implementation.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "offhand_melee_cancel.h"
#include "weapon_kv_s21_ext.h"
#include "game/shared/sdk_entity_state.h"

// Server CPlayer.
static constexpr ptrdiff_t OMC_PLAYER_OFF_OFFHANDS        = 0x16B4; // m_inventory.offhandWeapons[]
static constexpr ptrdiff_t OMC_PLAYER_OFF_ACTIVEWEAPONS   = 0x16CC; // m_inventory.activeWeapons[3]
static constexpr ptrdiff_t OMC_PLAYER_OFF_SELECTEDOFFHAND = 0x16EE; // m_selectedOffhands[], main hand first

// Server CWeaponX.
static constexpr ptrdiff_t OMC_WEAPON_OFF_WEAPSTATE = 0x1234; // m_weapState
static constexpr ptrdiff_t OMC_WEAPON_OFF_FIREMODE  = 0x2750; // fire_mode

static constexpr uint8_t  OMC_SELECTED_NONE     = 0xFF;
static constexpr int      OMC_STOCK_OFFHANDS    = 6;    // the stock getter's bound
static constexpr int      OMC_WEAPSTATE_IDLE    = 0;
static constexpr int      OMC_WEAPSTATE_ATTACK  = 9;
static constexpr uint32_t OMC_INVALID_HANDLE    = 0xFFFFFFFFu;

static ConVar bridge_offhand_melee_cancel("bridge_offhand_melee_cancel", "1", FCVAR_RELEASE,
	"Decide melee over a selected offhand by its offhand_cancelled_by_melee setting and let "
	"offhands in their attack state be meleed out of, as the S21 client does.");

static char (*v_Player_CanMelee)(void* pPlayer) = nullptr;

static void* OMC_Resolve(const void* pBase, const ptrdiff_t off)
{
	const uint32_t eh = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pBase) + off);
	return eh != OMC_INVALID_HANDLE ? SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server) : nullptr;
}

static char Hook_Player_CanMelee(void* pPlayer)
{
	if (!pPlayer || !bridge_offhand_melee_cancel.GetBool())
		return v_Player_CanMelee(pPlayer);

	uint8_t* const p = static_cast<uint8_t*>(pPlayer);
	uint8_t& nSelected = p[OMC_PLAYER_OFF_SELECTEDOFFHAND];
	const uint8_t nSavedSelected = nSelected;

	if (nSelected < OMC_STOCK_OFFHANDS)
	{
		void* const pSelected = OMC_Resolve(p, OMC_PLAYER_OFF_OFFHANDS + 4 * nSelected);
		if (pSelected)
		{
			if (!WeaponKVS21Ext_GetBool(pSelected, WeaponS21Bool_e::OFFHAND_CANCELLED_BY_MELEE))
				return 0;
			// The stock body would apply the older rule to it.
			nSelected = OMC_SELECTED_NONE;
		}
	}

	// Hand weapons in the attack state block melee through the button-press
	// protection; offhand fire modes (1..4) are exempt from it.
	int* apState[2] = {};
	for (int i = 0; i < 2; ++i)
	{
		uint8_t* const pWeapon = static_cast<uint8_t*>(OMC_Resolve(p, OMC_PLAYER_OFF_ACTIVEWEAPONS + 4 * i));
		if (!pWeapon)
			continue;
		const int nFireMode = *reinterpret_cast<const int*>(pWeapon + OMC_WEAPON_OFF_FIREMODE);
		int* const pState = reinterpret_cast<int*>(pWeapon + OMC_WEAPON_OFF_WEAPSTATE);
		if (static_cast<unsigned int>(nFireMode - 1) <= 3 && *pState == OMC_WEAPSTATE_ATTACK)
		{
			apState[i] = pState;
			*pState = OMC_WEAPSTATE_IDLE;
		}
	}

	const char bCan = v_Player_CanMelee(pPlayer);

	for (int* const pState : apState)
	{
		if (pState)
			*pState = OMC_WEAPSTATE_ATTACK;
	}
	nSelected = nSavedSelected;
	return bCan;
}

void VOffhandMeleeCancel::GetAdr(void) const
{
	LogFunAdr("Player_CanMelee", v_Player_CanMelee);
}

void VOffhandMeleeCancel::GetFun(void) const
{
	// Server half: m_contextAction at +0x17FC and the class settings at +0x5F08.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B D9 8B 89 FC 17 00 00 85 C9 74 ?? 8B 15 ?? ?? ?? ?? "
		"48 8B 83 08 5F 00 00 80 3C 02 00 74 ?? 83 F9 09")
		.GetPtr(v_Player_CanMelee);
	if (!v_Player_CanMelee)
		Warning(eDLL_T::SERVER,
			"[OFFHAND-MELEE] Player_CanMelee pattern unresolved -- melee over offhands keeps the S3 rule\n");
}

void VOffhandMeleeCancel::Detour(const bool bAttach) const
{
	if (v_Player_CanMelee)
		DetourSetup(&v_Player_CanMelee, &Hook_Player_CanMelee, bAttach);
}
