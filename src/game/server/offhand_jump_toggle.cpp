//=============================================================================//
//
// Purpose: offhand_jump_toggle.h implementation.
//
// The S21 client selects an offhand whose weapon sets
// 'offhand_deactivate_on_jump_toggle_or_release' while the movement ability
// (IN_DODGE) is held, and with the toggle_on_jump_to_deactivate userinfo on,
// holsters it on the next press. The dedi server halves predate the key; both branches run here after
// the stock per-offhand frames, on the same command the client runs them.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "offhand_jump_toggle.h"
#include "weapon_kv_s21_ext.h"
#include "energize.h"
#include "vscript_server_natives.h"
#include "baseentity.h"
#include "engine/server/vengineserver_impl.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/titan_gate.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/weapon_script_vars.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include <cfloat>
#include <cstdlib>
#include <intrin.h>

// Server CPlayer layout.
static constexpr ptrdiff_t OJ_PLAYER_OFF_GROUNDENTITY  = 0x3C4;  // m_hGroundEntity
static constexpr ptrdiff_t OJ_PLAYER_OFF_CONTEXTACTION = 0x17FC; // m_contextAction
static constexpr ptrdiff_t OJ_PLAYER_OFF_OFFHANDS      = 0x16B4; // m_inventory.offhandWeapons[]
static constexpr ptrdiff_t OJ_PLAYER_OFF_ACTIVEWEAPONS = 0x16CC; // m_inventory.activeWeapons[3]
static constexpr ptrdiff_t OJ_PLAYER_OFF_SELECTEDWEAP  = 0x16D8; // selected weapon index per active slot
static constexpr ptrdiff_t OJ_PLAYER_OFF_SELECTEDOFFHAND = 0x16EE; // m_selectedOffhands[3], 0xFF = none
static constexpr ptrdiff_t OJ_PLAYER_OFF_ZOOMING       = 0x5A61;
static constexpr ptrdiff_t OJ_PLAYER_OFF_BUTTONS       = 0x60DC; // m_nButtons
static constexpr ptrdiff_t OJ_PLAYER_OFF_PRESSED       = 0x60E0; // m_afButtonPressed
static constexpr ptrdiff_t OJ_PLAYER_OFF_FLAGS         = 0x6128; // weapon-disable flags word
static constexpr ptrdiff_t OJ_PLAYER_OFF_GRAPPLEACTIVE = 0x67B8;
static constexpr ptrdiff_t OJ_PLAYER_OFF_MELEESCRIPTED = 0x6EE0; // m_melee.scriptedState

// Server CWeaponX layout.
static constexpr ptrdiff_t OJ_WEAPON_OFF_OWNER          = 0x11F0; // m_weaponOwner
static constexpr ptrdiff_t OJ_WEAPON_OFF_NEXTREADYTIME  = 0x11F8; // m_nextReadyTime
static constexpr ptrdiff_t OJ_WEAPON_OFF_ACTIVESTATE    = 0x1220; // m_ActiveState, 2 = WEAPON_IS_ACTIVE
static constexpr ptrdiff_t OJ_WEAPON_OFF_WEAPSTATE      = 0x1234; // m_weapState
static constexpr ptrdiff_t OJ_WEAPON_OFF_ALLOWEDTOUSE   = 0x1238;
static constexpr ptrdiff_t OJ_WEAPON_OFF_BURSTCOUNT     = 0x155C; // m_burstFireCount
static constexpr ptrdiff_t OJ_WEAPON_OFF_BURSTINDEX     = 0x1560; // m_burstFireIndex
static constexpr ptrdiff_t OJ_WEAPON_OFF_CLASSNAME      = 0x15B0;
static constexpr ptrdiff_t OJ_WEAPON_OFF_ENERGYCOST     = 0x1580; // m_curSharedEnergyCost
static constexpr ptrdiff_t OJ_WEAPON_OFF_CHARGEENERGYCOST = 0x1B4C; // sharedEnergyChargeCost
static constexpr ptrdiff_t OJ_WEAPON_OFF_DISABLEMASK    = 0x19B0;
static constexpr ptrdiff_t OJ_WEAPON_OFF_ZOOMACTIVATES  = 0x1A74; // offhand_activates_on_zoom
static constexpr ptrdiff_t OJ_WEAPON_OFF_JUMPHOLDTIME   = 0x1A80; // offhand_activates_on_jump_hold_time
static constexpr ptrdiff_t OJ_WEAPON_OFF_GRAPPLE        = 0x2730;
static constexpr ptrdiff_t OJ_WEAPON_OFF_FIREMODE       = 0x2750;
static constexpr ptrdiff_t OJ_WEAPON_OFF_ACTIVESLOT     = 0x2754; // offhand_active_slot

static constexpr int      OJ_IN_MOVEMENT         = 0x10000000; // IN_DODGE; jump presses it unless +dodge is bound apart
static constexpr int      OJ_IN_OFFHAND1         = 0x00200000; // tactical
static constexpr int      OJ_FIREMODE_INSTANT    = 2;
static constexpr int      OJ_ACTIVESTATE_ACTIVE  = 2;
static constexpr int      OJ_WEAPSTATE_HOLSTER   = 2;
static constexpr int      OJ_WEAPSTATE_CHARGE    = 5;
static constexpr int      OJ_ACTIVE_SLOT_COUNT   = 3;
static constexpr int      OJ_CONTEXT_ACTION_ZIPLINE = 9;
static constexpr int      OJ_STOCK_OFFHANDS      = 6;
static constexpr uint32_t OJ_INVALID_HANDLE      = 0xFFFFFFFFu;

static ConVar bridge_offhand_jump("bridge_offhand_jump", "1", FCVAR_RELEASE,
	"Run the S21 offhand_deactivate_on_jump_toggle_or_release rules on the server: a held "
	"jump selects the offhand, a jump press holsters it when toggle_on_jump_to_deactivate is on.");

static ConVar bridge_offhand_switch_rules("bridge_offhand_switch_rules", "1", FCVAR_RELEASE,
	"Apply the S21 offhand switch test on top of the S3 one: scripted melee blocks, active "
	"instant-swap offhands skip the ready time, offhand_allow_swap_to_on_zipline and "
	"offhand_only_swap_to_on_ground gate ziplines and the air.");

static ConVar bridge_offhand_jump_diag("bridge_offhand_jump_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[OFFHAND-JUMP] select / holster lines.");

static int64_t (*v_WeaponX_OffhandFrame)(void* pWeapon, void* pPlayer, unsigned int nOffhand) = nullptr;
static bool (*v_Player_IsSlotDisabled)(void* pPlayer, int nSlot) = nullptr;
static bool (*v_WeaponX_CanSwitchToOffhand)(void* pWeapon) = nullptr;
static void (*v_Player_SetSelectedOffhand)(void* pPlayer, int nSlot, void* pWeapon, int nPendingHybrid) = nullptr;
static bool (*v_WeaponX_Lower)(void* pWeapon) = nullptr;
static char (*v_Player_SwitchToOffhand)(void* pPlayer, int nSlot) = nullptr;
static bool (*v_Player_IsOffhandButtonHeld)(void* pPlayer, void* pWeapon) = nullptr;
static const void* s_pDispatchCanSwitchRet = nullptr; // candidate test of the offhand dispatcher
static const void* s_pActiveFrameHeldRet = nullptr; // trigger-held test of the active offhand frame
static const void* s_pBusyFrameHeldRet = nullptr;   // fast-holster release test of the busy frame

// Protected CBaseEntity::m_entIndex; player entindex == client index.
class OJ_PlayerEntIndexAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_entIndex;
};

template <typename T>
static inline T OJ_Read(const void* p, const ptrdiff_t off)
{
	return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(p) + off);
}

static const WeaponKVS21Ext_t& OJ_WeaponKV(const void* pWeapon)
{
	const char* const pszName = static_cast<const char*>(pWeapon) + OJ_WEAPON_OFF_CLASSNAME;
	return WeaponKVS21Ext_Get(pszName);
}

// toggle_on_jump_to_deactivate is FCVAR_USERINFO on the S21 client, default 1.
static bool OJ_ToggleOnJump(const void* pPlayer)
{
	if (!g_pEngineServer)
		return true;

	const int nClient = static_cast<const OJ_PlayerEntIndexAccess*>(
		reinterpret_cast<const CBaseEntity*>(pPlayer))->m_entIndex;
	const char* const psz = g_pEngineServer->GetClientConVarValue(nClient, "toggle_on_jump_to_deactivate");
	if (!psz || !psz[0])
		return true;
	return strtol(psz, nullptr, 10) != 0;
}

static void* OJ_ActiveWeapon(const void* pPlayer, const int nSlot)
{
	if (nSlot < 0 || nSlot >= OJ_ACTIVE_SLOT_COUNT)
		return nullptr;

	const uint32_t eh = OJ_Read<uint32_t>(pPlayer, OJ_PLAYER_OFF_ACTIVEWEAPONS + 4 * nSlot);
	if (eh == OJ_INVALID_HANDLE)
		return nullptr;
	return SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server);
}

// Active-frame release of a jump offhand: the jump edge (press when toggling,
// let go when holding), unless offhand_holds_on_tactical keeps it up.
static bool OJ_TriggerReleased(const void* pPlayer, const WeaponKVS21Ext_t& kv)
{
	const int nButtons = OJ_Read<int>(pPlayer, OJ_PLAYER_OFF_BUTTONS);
	if (kv.bOffhandHoldsOnTactical && (nButtons & OJ_IN_OFFHAND1))
		return false;
	return OJ_ToggleOnJump(pPlayer)
		? (OJ_Read<int>(pPlayer, OJ_PLAYER_OFF_PRESSED) & OJ_IN_MOVEMENT) != 0
		: (nButtons & OJ_IN_MOVEMENT) == 0;
}

bool OffhandJumpToggle_IsReleasing(const void* pPlayer)
{
	if (!pPlayer || !bridge_offhand_jump.GetBool() || TitanGate_IsTitanPlayer(pPlayer))
		return false;

	const void* const pMain = OJ_ActiveWeapon(pPlayer, 0);
	if (!pMain)
		return false;
	const WeaponKVS21Ext_t& kv = OJ_WeaponKV(pMain);
	if (!kv.bOffhandJumpToggle)
		return false;
	const int nState = OJ_Read<int>(pMain, OJ_WEAPON_OFF_WEAPSTATE);
	if (nState == OJ_WEAPSTATE_HOLSTER)
		return true;
	// dedi runs the active offhand frame after movement; S21 releases before it.
	return nState == OJ_WEAPSTATE_CHARGE
		&& OJ_Read<int>(pMain, OJ_WEAPON_OFF_ACTIVESTATE) == OJ_ACTIVESTATE_ACTIVE
		&& OJ_TriggerReleased(pPlayer, kv);
}

static bool OJ_WeaponHasMod(HSCRIPT hWeapon, const char* pszMod, bool* pbOut)
{
	if (!g_pServerScript || !hWeapon)
		return false;

	HSQUIRRELVM const v = g_pServerScript->GetVM();
	const HSQOBJECT& weaponObj = *reinterpret_cast<const HSQOBJECT*>(hWeapon);

	sq_pushobject(v, weaponObj);
	sq_pushstring(v, "HasMod", -1);
	if (SQ_FAILED(sq_get(v, -2)))
	{
		sq_pop(v, 1);
		return false;
	}
	HSQOBJECT closureObj;
	sq_getstackobj(v, -1, &closureObj);
	sq_pop(v, 2);

	sq_pushobject(v, closureObj);
	sq_pushobject(v, weaponObj);
	sq_pushstring(v, pszMod, -1);
	if (SQ_FAILED(sq_call(v, 2, SQTrue, SQTrue)))
	{
		sq_pop(v, 1);
		return false;
	}

	SQBool b = SQFalse;
	sq_getbool(v, -1, &b);
	sq_pop(v, 2);
	*pbOut = b != SQFalse;
	return true;
}

// The heirloom viewmodel of a jump offhand follows the main-hand weapon it replaces.
static void OJ_SyncHeirloomMod(void* pWeapon, const WeaponKVS21Ext_t& kv, const void* pActive)
{
	if (!kv.bHasHeirloomMod)
		return;

	bool bWant = false;
	if (pActive)
	{
		const WeaponKVS21Ext_t& activeKv = OJ_WeaponKV(pActive);
		bWant = activeKv.bIsHeirloom && !activeKv.bIsArtifact;
	}

	const HSCRIPT hWeapon = reinterpret_cast<CBaseEntity*>(pWeapon)->GetScriptInstance();
	bool bHas = false;
	if (!OJ_WeaponHasMod(hWeapon, "heirloom", &bHas) || bHas == bWant)
		return;

	WeaponBridge_InvokeChangeMod(hWeapon, "heirloom", bWant);
}

static void OJ_ClearSelectedWeapon(void* pPlayer, const int nSlot)
{
	uint8_t* const pSel = static_cast<uint8_t*>(pPlayer) + OJ_PLAYER_OFF_SELECTEDWEAP + nSlot;
	if (*pSel == 0xFF)
		return;
	MarkEntityEdictDirty(pPlayer);
	*pSel = 0xFF;
}

// Guard of the stock server offhand frame, and its branches that run before the jump one.
static bool OJ_FrameReachesJumpBranch(const void* pWeapon, const void* pPlayer)
{
	const uint32_t nFlags = OJ_Read<uint32_t>(pPlayer, OJ_PLAYER_OFF_FLAGS);
	if ((nFlags & 8) || ((OJ_Read<uint8_t>(pWeapon, OJ_WEAPON_OFF_DISABLEMASK) & 0xC) && (nFlags & 0x4000)))
		return false;
	if (!OJ_Read<uint8_t>(pWeapon, OJ_WEAPON_OFF_ALLOWEDTOUSE))
		return false;
	if (OJ_Read<uint8_t>(pWeapon, OJ_WEAPON_OFF_GRAPPLE) && OJ_Read<uint8_t>(pPlayer, OJ_PLAYER_OFF_GRAPPLEACTIVE))
		return false;
	if (OJ_Read<int>(pWeapon, OJ_WEAPON_OFF_FIREMODE) == OJ_FIREMODE_INSTANT)
		return false;

	// The stock frame already switched through its own zoom or jump-hold rule.
	if (OJ_Read<uint8_t>(pWeapon, OJ_WEAPON_OFF_ZOOMACTIVATES) && OJ_Read<uint8_t>(pPlayer, OJ_PLAYER_OFF_ZOOMING)
		&& OJ_Read<int>(pWeapon, OJ_WEAPON_OFF_ACTIVESTATE) != OJ_ACTIVESTATE_ACTIVE)
		return false;
	if (OJ_Read<float>(pWeapon, OJ_WEAPON_OFF_JUMPHOLDTIME) > 0.0f)
		return false;

	return true;
}

static void OJ_DiagInput(const char* pszWhat, const void* pWeapon, const void* pPlayer);
static bool OJ_CanSwitchToOffhand(void* pWeapon);

static void OJ_TrySelect(void* pWeapon, void* pPlayer, const WeaponKVS21Ext_t& kv)
{
	if (!(OJ_Read<int>(pPlayer, OJ_PLAYER_OFF_BUTTONS) & OJ_IN_MOVEMENT))
		return;

	if (OJ_ToggleOnJump(pPlayer)
		&& OJ_Read<int>(pWeapon, OJ_WEAPON_OFF_ACTIVESTATE) == OJ_ACTIVESTATE_ACTIVE
		&& !(OJ_Read<int>(pWeapon, OJ_WEAPON_OFF_WEAPSTATE) == OJ_WEAPSTATE_HOLSTER && kv.bOffhandInstantSwap))
		return;

	const int nSlot = OJ_Read<int>(pWeapon, OJ_WEAPON_OFF_ACTIVESLOT);
	if (nSlot < 0 || nSlot >= OJ_ACTIVE_SLOT_COUNT)
		return;
	if (v_Player_IsSlotDisabled(pPlayer, nSlot) || !OJ_CanSwitchToOffhand(pWeapon))
		return;

	void* const pActive = OJ_ActiveWeapon(pPlayer, nSlot);
	const bool bActiveBursting = pActive && OJ_Read<int>(pActive, OJ_WEAPON_OFF_BURSTCOUNT) > 0
		&& OJ_Read<int>(pActive, OJ_WEAPON_OFF_BURSTINDEX) >= 1;
	if (bActiveBursting)
		return;

	OJ_SyncHeirloomMod(pWeapon, kv, pActive);
	v_Player_SetSelectedOffhand(pPlayer, nSlot, pWeapon, 0);
	OJ_ClearSelectedWeapon(pPlayer, nSlot);

	if (!pActive || !v_WeaponX_Lower(pActive) || (pActive == pWeapon && kv.bOffhandInstantSwap))
		v_Player_SwitchToOffhand(pPlayer, nSlot);

	if (bridge_offhand_jump_diag.GetBool())
		OJ_DiagInput("select", pWeapon, pPlayer);
}

static int64_t Hook_WeaponX_OffhandFrame(void* pWeapon, void* pPlayer, unsigned int nOffhand)
{
	const int64_t result = v_WeaponX_OffhandFrame(pWeapon, pPlayer, nOffhand);

	if (!pWeapon || !pPlayer || !bridge_offhand_jump.GetBool())
		return result;

	const WeaponKVS21Ext_t& kv = OJ_WeaponKV(pWeapon);
	if (!kv.bOffhandJumpToggle || TitanGate_IsTitanPlayer(pPlayer) || !OJ_FrameReachesJumpBranch(pWeapon, pPlayer))
		return result;

	OJ_TrySelect(pWeapon, pPlayer, kv);
	return result;
}

//-----------------------------------------------------------------------------
// S21 treats the jump button as a jump offhand's trigger. Active frame: held
// while jump is held (toggle off), or until the next jump press (toggle on).
// Busy frame: the idle fast-holster is skipped while jump is held with toggle
// on. offhand_holds_on_tactical also counts a held tactical button in both.
// The dedi halves only know the offhand's own IN_OFFHAND button.
//-----------------------------------------------------------------------------
static void OJ_DiagInput(const char* pszWhat, const void* pWeapon, const void* pPlayer)
{
	const int nButtons = OJ_Read<int>(pPlayer, OJ_PLAYER_OFF_BUTTONS);
	const int nPressed = OJ_Read<int>(pPlayer, OJ_PLAYER_OFF_PRESSED);
	Msg(eDLL_T::SERVER, "[OFFHAND-JUMP] %s '%s' toggle=%d jump=%d jumpPress=%d tac=%d weapState=%d active=%d\n",
		pszWhat, static_cast<const char*>(pWeapon) + OJ_WEAPON_OFF_CLASSNAME, OJ_ToggleOnJump(pPlayer) ? 1 : 0,
		(nButtons & OJ_IN_MOVEMENT) ? 1 : 0, (nPressed & OJ_IN_MOVEMENT) ? 1 : 0, (nButtons & OJ_IN_OFFHAND1) ? 1 : 0,
		OJ_Read<int>(pWeapon, OJ_WEAPON_OFF_WEAPSTATE), OJ_Read<int>(pWeapon, OJ_WEAPON_OFF_ACTIVESTATE));
}

static bool Hook_Player_IsOffhandButtonHeld(void* pPlayer, void* pWeapon)
{
	const bool bHeld = v_Player_IsOffhandButtonHeld(pPlayer, pWeapon);
	if (bHeld || !pPlayer || !pWeapon || !bridge_offhand_jump.GetBool())
		return bHeld;

	const void* const pRet = _ReturnAddress();
	const bool bActiveFrame = pRet == s_pActiveFrameHeldRet;
	if (!bActiveFrame && pRet != s_pBusyFrameHeldRet)
		return bHeld;

	const WeaponKVS21Ext_t& kv = OJ_WeaponKV(pWeapon);
	if ((!kv.bOffhandJumpToggle && !kv.bOffhandHoldsOnTactical) || TitanGate_IsTitanPlayer(pPlayer))
		return bHeld;

	const int nButtons = OJ_Read<int>(pPlayer, OJ_PLAYER_OFF_BUTTONS);
	const bool bTacticalHeld = kv.bOffhandHoldsOnTactical && (nButtons & OJ_IN_OFFHAND1);
	if (!kv.bOffhandJumpToggle)
		return !bActiveFrame && bTacticalHeld;

	const bool bToggle = OJ_ToggleOnJump(pPlayer);
	const bool bJumpHeld = (nButtons & OJ_IN_MOVEMENT) != 0;
	if (!bActiveFrame)
		return (bToggle && bJumpHeld) || bTacticalHeld;

	const bool bResult = !OJ_TriggerReleased(pPlayer, kv);
	if (!bResult && bridge_offhand_jump_diag.GetBool())
		OJ_DiagInput("release", pWeapon, pPlayer);
	return bResult;
}

void OffhandJumpToggle_OnHolster(void* pWeapon, const bool bFast, const void* pCaller)
{
	if (!pWeapon || !bridge_offhand_jump_diag.GetBool() || !OJ_WeaponKV(pWeapon).bOffhandJumpToggle)
		return;

	const uint32_t hOwner = OJ_Read<uint32_t>(pWeapon, OJ_WEAPON_OFF_OWNER);
	void* const pPlayer = hOwner != OJ_INVALID_HANDLE
		? SDKEntityState_Resolve(SDKEntityHandle(hOwner), ESide::Server) : nullptr;
	if (!pPlayer || !ServerScript_EntityIsPlayer(pPlayer))
		return;

	char szWhat[64];
	V_snprintf(szWhat, sizeof(szWhat), "holster fast=%d caller=0x%llX", bFast ? 1 : 0,
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pCaller) - g_GameDll.GetModuleBase() + 0x140000000ull));
	OJ_DiagInput(szWhat, pWeapon, pPlayer);
}

static void* OJ_OwnerPlayer(const void* pWeapon)
{
	const uint32_t hOwner = OJ_Read<uint32_t>(pWeapon, OJ_WEAPON_OFF_OWNER);
	void* const pOwner = hOwner != OJ_INVALID_HANDLE
		? SDKEntityState_Resolve(SDKEntityHandle(hOwner), ESide::Server) : nullptr;
	return pOwner && ServerScript_EntityIsPlayer(pOwner) ? pOwner : nullptr;
}

static bool OJ_IsActiveInAnyHand(const void* pPlayer, const void* pWeapon)
{
	for (int i = 0; i < OJ_ACTIVE_SLOT_COUNT; ++i)
	{
		if (OJ_ActiveWeapon(pPlayer, i) == pWeapon)
			return true;
	}
	return false;
}

// An active instant-swap offhand passes without the ready-time and shared
// energy tests. The stock body still runs its own earlier tests (allowed to
// use, owner alive and enabled, script callback) with those fields cleared.
static bool OJ_CanSwitchSkippingReadyTime(void* pWeapon)
{
	uint8_t* const p = static_cast<uint8_t*>(pWeapon);
	float& flNextReady = *reinterpret_cast<float*>(p + OJ_WEAPON_OFF_NEXTREADYTIME);
	int& nEnergyCost = *reinterpret_cast<int*>(p + OJ_WEAPON_OFF_ENERGYCOST);
	int& nChargeEnergyCost = *reinterpret_cast<int*>(p + OJ_WEAPON_OFF_CHARGEENERGYCOST);

	const float flSavedReady = flNextReady;
	const int nSavedCost = nEnergyCost;
	const int nSavedChargeCost = nChargeEnergyCost;
	flNextReady = -FLT_MAX;
	nEnergyCost = 0;
	nChargeEnergyCost = 0;

	const bool bCan = v_WeaponX_CanSwitchToOffhand(pWeapon);

	flNextReady = flSavedReady;
	nEnergyCost = nSavedCost;
	nChargeEnergyCost = nSavedChargeCost;
	return bCan;
}

//-----------------------------------------------------------------------------
// The S21 test adds, around the stock body: a scripted melee blocks it; an
// active instant-swap offhand skips the ready time; a zipline or the air
// blocks it unless offhand_allow_swap_to_on_zipline /
// offhand_only_swap_to_on_ground say otherwise.
//-----------------------------------------------------------------------------
static bool OJ_CanSwitchToOffhand(void* pWeapon)
{
	void* const pPlayer = bridge_offhand_switch_rules.GetBool() ? OJ_OwnerPlayer(pWeapon) : nullptr;
	if (!pPlayer)
		return v_WeaponX_CanSwitchToOffhand(pWeapon);

	if (OJ_Read<int>(pPlayer, OJ_PLAYER_OFF_MELEESCRIPTED))
		return false;

	if (WeaponKVS21Ext_GetBool(pWeapon, WeaponS21Bool_e::OFFHAND_INSTANT_SWAP_TO_OFFHAND)
		&& OJ_IsActiveInAnyHand(pPlayer, pWeapon))
		return OJ_CanSwitchSkippingReadyTime(pWeapon);

	if (!v_WeaponX_CanSwitchToOffhand(pWeapon))
		return false;

	if (OJ_Read<int>(pPlayer, OJ_PLAYER_OFF_CONTEXTACTION) == OJ_CONTEXT_ACTION_ZIPLINE
		&& !WeaponKVS21Ext_GetBool(pWeapon, WeaponS21Bool_e::OFFHAND_ALLOW_SWAP_TO_ON_ZIPLINE))
		return false;

	if (OJ_Read<uint32_t>(pPlayer, OJ_PLAYER_OFF_GROUNDENTITY) == OJ_INVALID_HANDLE
		&& WeaponKVS21Ext_GetBool(pWeapon, WeaponS21Bool_e::OFFHAND_ONLY_SWAP_TO_ON_GROUND))
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// offhand_switch_slot: a candidate that is not already in a hand raises in the
// alt hand while the main hand holds an offhand that is not holstering (Missile
// Swarm over Valkyrie's jets). The dispatcher buckets the candidate by its
// active slot right after this test passes; dedi predates the key.
//-----------------------------------------------------------------------------
static bool Hook_WeaponX_CanSwitchToOffhand(void* pWeapon)
{
	if (!pWeapon)
		return v_WeaponX_CanSwitchToOffhand(pWeapon);

	const bool bCan = OJ_CanSwitchToOffhand(pWeapon);
	if (!bCan || _ReturnAddress() != s_pDispatchCanSwitchRet || !bridge_offhand_jump.GetBool())
		return bCan;
	if (!OJ_WeaponKV(pWeapon).bOffhandSwitchSlot)
		return bCan;

	void* const pPlayer = OJ_OwnerPlayer(pWeapon);
	if (!pPlayer || TitanGate_IsTitanPlayer(pPlayer))
		return bCan;

	const void* const pMain = OJ_ActiveWeapon(pPlayer, 0);
	if (pWeapon == pMain || pWeapon == OJ_ActiveWeapon(pPlayer, 1))
		return bCan;

	const bool bMainIsOffhand = pMain
		&& static_cast<unsigned int>(OJ_Read<int>(pMain, OJ_WEAPON_OFF_FIREMODE) - 1) <= 4
		&& OJ_Read<int>(pMain, OJ_WEAPON_OFF_WEAPSTATE) != OJ_WEAPSTATE_HOLSTER;
	const int nSlot = bMainIsOffhand ? 1 : 0;

	int* const pSlot = reinterpret_cast<int*>(static_cast<uint8_t*>(pWeapon) + OJ_WEAPON_OFF_ACTIVESLOT);
	if (*pSlot != nSlot)
	{
		MarkEntityEdictDirty(pWeapon);
		*pSlot = nSlot;
		if (bridge_offhand_jump_diag.GetBool())
			Msg(eDLL_T::SERVER, "[OFFHAND-JUMP] switch slot '%s' -> %d (main '%s')\n",
				static_cast<const char*>(pWeapon) + OJ_WEAPON_OFF_CLASSNAME, nSlot,
				pMain ? static_cast<const char*>(pMain) + OJ_WEAPON_OFF_CLASSNAME : "-");
	}
	return bCan;
}

//-----------------------------------------------------------------------------
// Calls hEnt.<pszMethod>(nArg...) on the server VM. The int or string return
// is copied out before the stack is popped.
//-----------------------------------------------------------------------------
static bool OJ_CallMethod(HSCRIPT hEnt, const char* pszMethod, const SQInteger* pArgs, const int nArgs,
	SQInteger* pOutInt = nullptr, char* pszOut = nullptr, const size_t nOutSize = 0)
{
	if (!g_pServerScript || !hEnt)
		return false;

	HSQUIRRELVM const v = g_pServerScript->GetVM();
	const HSQOBJECT& entObj = *reinterpret_cast<const HSQOBJECT*>(hEnt);

	sq_pushobject(v, entObj);
	sq_pushstring(v, pszMethod, -1);
	if (SQ_FAILED(sq_get(v, -2)))
	{
		sq_pop(v, 1);
		return false;
	}
	HSQOBJECT closureObj;
	sq_getstackobj(v, -1, &closureObj);
	sq_pop(v, 2);

	sq_pushobject(v, closureObj);
	sq_pushobject(v, entObj);
	for (int i = 0; i < nArgs; ++i)
		sq_pushinteger(v, pArgs[i]);
	if (SQ_FAILED(sq_call(v, nArgs + 1, SQTrue, SQFalse)))
	{
		sq_pop(v, 1);
		return false;
	}

	bool bOk = true;
	if (pOutInt)
		bOk = SQ_SUCCEEDED(sq_getinteger(v, -1, pOutInt));
	else if (pszOut && nOutSize)
	{
		const SQChar* psz = nullptr;
		bOk = SQ_SUCCEEDED(sq_getstring(v, -1, &psz)) && psz;
		if (bOk)
			V_strncpy(pszOut, psz, static_cast<int>(nOutSize));
	}
	sq_pop(v, 2);
	return bOk;
}

// offhand_match_player_skin: the offhand takes the owner's skin (by name) and camo.
static void OJ_MatchPlayerSkin(void* pPlayer, void* pOffhand)
{
	const HSCRIPT hPlayer = reinterpret_cast<CBaseEntity*>(pPlayer)->GetScriptInstance();
	const HSCRIPT hOffhand = reinterpret_cast<CBaseEntity*>(pOffhand)->GetScriptInstance();

	SQInteger nSkinCount = 0;
	if (!OJ_CallMethod(hOffhand, "GetSkinCount", nullptr, 0, &nSkinCount) || nSkinCount <= 1)
		return;

	SQInteger nPlayerSkin = 0;
	char szSkin[128];
	if (!OJ_CallMethod(hPlayer, "GetSkin", nullptr, 0, &nPlayerSkin)
		|| !OJ_CallMethod(hPlayer, "GetSkinNameByIndex", &nPlayerSkin, 1, nullptr, szSkin, sizeof(szSkin)))
		return;

	// GetSkinIndexByName takes a string; push it directly.
	HSQUIRRELVM const v = g_pServerScript->GetVM();
	const HSQOBJECT& offObj = *reinterpret_cast<const HSQOBJECT*>(hOffhand);
	sq_pushobject(v, offObj);
	sq_pushstring(v, "GetSkinIndexByName", -1);
	if (SQ_FAILED(sq_get(v, -2)))
	{
		sq_pop(v, 1);
		return;
	}
	HSQOBJECT closureObj;
	sq_getstackobj(v, -1, &closureObj);
	sq_pop(v, 2);
	sq_pushobject(v, closureObj);
	sq_pushobject(v, offObj);
	sq_pushstring(v, szSkin, -1);
	if (SQ_FAILED(sq_call(v, 2, SQTrue, SQFalse)))
	{
		sq_pop(v, 1);
		return;
	}
	SQInteger nSkin = -1;
	sq_getinteger(v, -1, &nSkin);
	sq_pop(v, 2);
	if (nSkin <= 0)
		return;

	SQInteger nCamo = 0;
	OJ_CallMethod(hOffhand, "SetSkin", &nSkin, 1);
	if (OJ_CallMethod(hPlayer, "GetCamo", nullptr, 0, &nCamo))
		OJ_CallMethod(hOffhand, "SetCamo", &nCamo, 1);
}

//-----------------------------------------------------------------------------
// After the offhand deploys, the S21 switch matches its skin to the owner's
// and, for offhand_disable_other_offhands, clears whatever the other hands hold.
//-----------------------------------------------------------------------------
void OffhandJumpToggle_PostSwitchToOffhand(void* pPlayer, void* pOffhand)
{
	if (!pPlayer || !pOffhand || !bridge_offhand_switch_rules.GetBool() || !ServerScript_EntityIsPlayer(pPlayer)
		|| TitanGate_IsTitanPlayer(pPlayer))
		return;

	if (WeaponKVS21Ext_GetBool(pOffhand, WeaponS21Bool_e::OFFHAND_MATCH_PLAYER_SKIN))
		OJ_MatchPlayerSkin(pPlayer, pOffhand);

	if (WeaponKVS21Ext_GetBool(pOffhand, WeaponS21Bool_e::OFFHAND_DISABLE_OTHER_OFFHANDS))
	{
		const HSCRIPT hPlayer = reinterpret_cast<CBaseEntity*>(pPlayer)->GetScriptInstance();
		for (SQInteger nOther = 0; nOther < OJ_ACTIVE_SLOT_COUNT; ++nOther)
		{
			void* const pActive = OJ_ActiveWeapon(pPlayer, static_cast<int>(nOther));
			if (pActive && pActive != pOffhand)
				OJ_CallMethod(hPlayer, "ClearOffhand", &nOther, 1);
		}
	}
}

void OffhandJumpToggle_OnBusyFrame(void* pWeapon)
{
	if (!pWeapon || !bridge_offhand_jump.GetBool() || !v_WeaponX_HolsterInternal)
		return;
	if (OJ_Read<int>(pWeapon, OJ_WEAPON_OFF_WEAPSTATE) != OJ_WEAPSTATE_CHARGE)
		return;

	const WeaponKVS21Ext_t& kv = OJ_WeaponKV(pWeapon);
	if (!kv.bOffhandJumpToggle)
		return;

	const uint32_t hOwner = OJ_Read<uint32_t>(pWeapon, OJ_WEAPON_OFF_OWNER);
	void* const pPlayer = hOwner != OJ_INVALID_HANDLE
		? SDKEntityState_Resolve(SDKEntityHandle(hOwner), ESide::Server) : nullptr;
	if (!pPlayer || !ServerScript_EntityIsPlayer(pPlayer) || TitanGate_IsTitanPlayer(pPlayer)
		|| !(OJ_Read<int>(pPlayer, OJ_PLAYER_OFF_PRESSED) & OJ_IN_MOVEMENT) || !OJ_ToggleOnJump(pPlayer))
		return;

	v_WeaponX_HolsterInternal(pWeapon, false);

	if (bridge_offhand_jump_diag.GetBool())
		Msg(eDLL_T::SERVER, "[OFFHAND-JUMP] holster '%s' player=%p\n",
			static_cast<const char*>(pWeapon) + OJ_WEAPON_OFF_CLASSNAME, pPlayer);
}

void OffhandJumpToggle_ReleaseOnLanding(void* pPlayer)
{
	if (!pPlayer || !bridge_offhand_jump.GetBool() || !v_WeaponX_HolsterInternal || TitanGate_IsTitanPlayer(pPlayer))
		return;

	void* const pMain = OJ_ActiveWeapon(pPlayer, 0);
	if (!pMain || !OJ_WeaponKV(pMain).bOffhandJumpToggle)
		return;
	if (OJ_Read<int>(pMain, OJ_WEAPON_OFF_WEAPSTATE) != OJ_WEAPSTATE_CHARGE
		|| OJ_Read<int>(pMain, OJ_WEAPON_OFF_ACTIVESTATE) != OJ_ACTIVESTATE_ACTIVE)
		return;

	v_WeaponX_HolsterInternal(pMain, false);

	if (bridge_offhand_jump_diag.GetBool())
		OJ_DiagInput("landed", pMain, pPlayer);
}

void VOffhandJumpToggle::GetAdr(void) const
{
	LogFunAdr("WeaponX_OffhandFrame", v_WeaponX_OffhandFrame);
	LogFunAdr("Player_IsSlotDisabled", v_Player_IsSlotDisabled);
	LogFunAdr("WeaponX_CanSwitchToOffhand", v_WeaponX_CanSwitchToOffhand);
	LogFunAdr("Player_SetSelectedOffhand", v_Player_SetSelectedOffhand);
	LogFunAdr("WeaponX_Lower", v_WeaponX_Lower);
	LogFunAdr("Player_SwitchToOffhand", v_Player_SwitchToOffhand);
	LogFunAdr("Player_IsOffhandButtonHeld", v_Player_IsOffhandButtonHeld);
}

void VOffhandJumpToggle::GetFun(void) const
{
	// Server half: the offhand_active_slot read at weapon+0x2754 exists only there.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 50 4D 63 E0 48 8B D9 48 8D 0D")
		.GetPtr(v_WeaponX_OffhandFrame);

	// The stock zoom/jump-hold switch in the offhand frame calls, in order:
	// IsSlotDisabled, CanSwitchToOffhand, SetSelectedOffhand ... Lower, SwitchToOffhand.
	const CMemory select = Module_FindPattern(g_GameDll,
		"8B 93 54 27 00 00 48 8B CF E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? "
		"8B 93 54 27 00 00 45 33 C9 4C 8B C3 48 8B CF E8");
	if (select)
	{
		select.Offset(0x09).FollowNearCall().GetPtr(v_Player_IsSlotDisabled);
		select.Offset(0x19).FollowNearCall().GetPtr(v_WeaponX_CanSwitchToOffhand);
		select.Offset(0x35).FollowNearCall().GetPtr(v_Player_SetSelectedOffhand);
	}
	const CMemory lower = Module_FindPattern(g_GameDll,
		"48 85 C9 74 09 E8 ?? ?? ?? ?? 84 C0 75 0E 8B 93 54 27 00 00 48 8B CF E8");
	if (lower)
	{
		lower.Offset(0x05).FollowNearCall().GetPtr(v_WeaponX_Lower);
		lower.Offset(0x17).FollowNearCall().GetPtr(v_Player_SwitchToOffhand);
	}

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 45 33 C0 4C 8D 89 B4 16 00 00 45 33 D2 48 8D 1D ?? ?? ?? ?? 4C 8B D9")
		.GetPtr(v_Player_IsOffhandButtonHeld);
	// Return addresses of the two trigger-held calls in the active and busy offhand frames.
	const CMemory activeHeld = Module_FindPattern(g_GameDll,
		"41 80 BE 40 12 00 00 02 74 ?? 49 8B D6 49 8B CF E8 ?? ?? ?? ?? 84 C0 75");
	if (activeHeld)
		s_pActiveFrameHeldRet = activeHeld.Offset(0x15).RCast<const void*>();
	const CMemory busyHeld = Module_FindPattern(g_GameDll,
		"80 BB 73 1A 00 00 00 74 ?? 48 8B D3 48 8B CF E8 ?? ?? ?? ?? 84 C0 75");
	if (busyHeld)
		s_pBusyFrameHeldRet = busyHeld.Offset(0x14).RCast<const void*>();
	// Offhand dispatcher candidate loop: CanSwitchToOffhand, then the bucket by active slot.
	const CMemory dispatch = Module_FindPattern(g_GameDll,
		"83 BF 8C 5C 00 00 00 75 11 45 84 E4 75 0C 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 0C 48 8B CB E8 ?? ?? ?? ?? 84 C0 75");
	if (dispatch && v_WeaponX_CanSwitchToOffhand
		&& dispatch.Offset(0x1D).FollowNearCall().RCast<void*>() == reinterpret_cast<void*>(v_WeaponX_CanSwitchToOffhand))
		s_pDispatchCanSwitchRet = dispatch.Offset(0x22).RCast<const void*>();
	else
		Warning(eDLL_T::SERVER, "[OFFHAND-JUMP] dispatcher candidate site unresolved -- offhand_switch_slot ignored\n");

	if (!v_Player_IsOffhandButtonHeld || !s_pActiveFrameHeldRet || !s_pBusyFrameHeldRet)
		Warning(eDLL_T::SERVER, "[OFFHAND-JUMP] trigger-held sites unresolved -- jump offhands release at once\n");

	if (!v_WeaponX_OffhandFrame || !v_Player_IsSlotDisabled || !v_WeaponX_CanSwitchToOffhand
		|| !v_Player_SetSelectedOffhand || !v_WeaponX_Lower || !v_Player_SwitchToOffhand)
	{
		Warning(eDLL_T::SERVER, "[OFFHAND-JUMP] pattern unresolved (frame=%d select=%d lower=%d) "
			"-- jump offhands stay unselectable on the server\n",
			v_WeaponX_OffhandFrame ? 1 : 0, select ? 1 : 0, lower ? 1 : 0);
	}
}

void VOffhandJumpToggle::Detour(const bool bAttach) const
{
	if (!v_WeaponX_OffhandFrame || !v_Player_IsSlotDisabled || !v_WeaponX_CanSwitchToOffhand
		|| !v_Player_SetSelectedOffhand || !v_WeaponX_Lower || !v_Player_SwitchToOffhand)
		return;

	DetourSetup(&v_WeaponX_OffhandFrame, &Hook_WeaponX_OffhandFrame, bAttach);
	if (v_Player_IsOffhandButtonHeld && s_pActiveFrameHeldRet && s_pBusyFrameHeldRet)
		DetourSetup(&v_Player_IsOffhandButtonHeld, &Hook_Player_IsOffhandButtonHeld, bAttach);
	DetourSetup(&v_WeaponX_CanSwitchToOffhand, &Hook_WeaponX_CanSwitchToOffhand, bAttach);
}
