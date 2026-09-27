//=============================================================================//
//
// Purpose: [MB-VM] trace of the sprint / weapon-state chain around a mantle
// climb and boost. The 1p slide clip is the raise-from-sprint sequence picked
// while m_sliding is set, so every sprint start/stop inside the window is
// logged with the weapon state it found and the airborne sprint grace.
//
//=============================================================================//
#include "core/stdafx.h"

#include "mantle_boost_vm_probe.h"
#include "player.h"
#include "game/shared/dt_extend.h"
#include "game/shared/sdk_entity_state.h"
#include "public/edict.h"

extern CGlobalVars* gpGlobals;

// Server CPlayer layout.
static constexpr ptrdiff_t VMP_OFF_GROUND_ENTITY  = 0x3C4;  // m_hGroundEntity
static constexpr ptrdiff_t VMP_OFF_MOVETYPE       = 776;    // byte, 2 = WALK, 12 = TRAVERSE
static constexpr ptrdiff_t VMP_OFF_ACTIVE_WEAPONS = 0x16CC; // m_inventory.activeWeapons[3]
static constexpr ptrdiff_t VMP_OFF_BUTTONS        = 0x60DC; // m_nButtons
static constexpr ptrdiff_t VMP_OFF_DUCKSTATE      = 26096;
static constexpr ptrdiff_t VMP_OFF_SLIDING        = 26565;  // m_sliding
static constexpr ptrdiff_t VMP_OFF_ON_SLOPE_TIME  = 26932;  // airborne sprint grace anchor (CanSprintNow: 0.25s)
static constexpr ptrdiff_t VMP_OFF_STICKY_SPRINT  = 27924;
static constexpr ptrdiff_t VMP_OFF_IS_SPRINTING   = 27940;  // m_fIsSprinting

// Server CWeaponX layout.
static constexpr ptrdiff_t VMP_WEAPON_OFF_WEAPSTATE = 0x1234; // m_weapState, 8 = SPRINT

static constexpr int   VMP_WEAPSTATE_SPRINT = 8;
static constexpr int   VMP_IN_SPEED         = 0x8000;
static constexpr float VMP_CLIMB_WINDOW     = 3.0f;
static constexpr float VMP_BOOST_WINDOW     = 1.5f;
static constexpr int   VMP_MAX_TICK_LINES   = 120;

static ConVar sdk_mantle_boost_vm_probe("sdk_mantle_boost_vm_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[MB-VM] Trace sprint start/stop, weapon state, m_sliding and the airborne sprint grace "
	"from each mantle climb until shortly after its boost.");

static void (*v_Player_StartSprint)(CPlayer* player) = nullptr;
static void (*v_Player_StopSprint)(CPlayer* player) = nullptr;

struct VmProbeSlot_t
{
	float m_flWindowEnd = -1.0f;
	float m_flClimbStart = 0.0f;
	int   m_nTickLines = 0;
};
static VmProbeSlot_t s_vmProbe[MAX_PLAYERS];

template <typename T>
static T VMP_Read(const CPlayer* const player, const ptrdiff_t off)
{
	return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(player) + off);
}

static float VMP_CurTime(void)
{
	return gpGlobals ? gpGlobals->curTime : 0.0f;
}

static VmProbeSlot_t* VMP_Slot(const CPlayer* const player)
{
	if (!player)
		return nullptr;
	const int slot = static_cast<int>(player->GetEdict()) - 1;
	if (slot < 0 || slot >= MAX_PLAYERS)
		return nullptr;
	return &s_vmProbe[slot];
}

static bool VMP_Armed(const CPlayer* const player)
{
	if (!sdk_mantle_boost_vm_probe.GetBool())
		return false;
	const VmProbeSlot_t* const s = VMP_Slot(player);
	return s && VMP_CurTime() <= s->m_flWindowEnd;
}

static const void* VMP_ActiveWeapon(const CPlayer* const player, const int nSlot)
{
	const uint32_t eh = VMP_Read<uint32_t>(player, VMP_OFF_ACTIVE_WEAPONS + 4 * nSlot);
	if (eh == 0xFFFFFFFFu)
		return nullptr;
	return SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server);
}

static int VMP_IdealActivity(const void* const pWeapon)
{
	static int s_nOff = -2;
	if (s_nOff == -2)
	{
		s_nOff = DTExtend_FindNativePropOffset(pWeapon, "m_IdealActivity");
		if (s_nOff <= 0)
			s_nOff = DTExtend_FindNativePropOffset(pWeapon, "m_idealActivity");
		if (s_nOff <= 0)
			Warning(eDLL_T::SERVER, "[MB-VM] weapon ideal activity prop unresolved -- act=-1 in lines\n");
	}
	if (s_nOff <= 0)
		return -1;
	return *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(pWeapon) + s_nOff);
}

static void VMP_Print(const CPlayer* const player, const char* const pszWhat)
{
	const VmProbeSlot_t* const s = VMP_Slot(player);
	const float flNow = VMP_CurTime();
	const void* const pWeapon = VMP_ActiveWeapon(player, 0);
	const int nWeapState = pWeapon ? *reinterpret_cast<const int*>(
		reinterpret_cast<uintptr_t>(pWeapon) + VMP_WEAPON_OFF_WEAPSTATE) : -1;

	Msg(eDLL_T::SERVER, "[MB-VM] %-12s t=+%.3f ground=%d move=%d duck=%d sprint=%d sticky=%d speedBtn=%d "
		"sliding=%d slopeAge=%.3f weapState=%d act=%d\n",
		pszWhat, s ? flNow - s->m_flClimbStart : 0.0f,
		VMP_Read<uint32_t>(player, VMP_OFF_GROUND_ENTITY) != 0xFFFFFFFFu,
		VMP_Read<uint8_t>(player, VMP_OFF_MOVETYPE),
		VMP_Read<int>(player, VMP_OFF_DUCKSTATE),
		VMP_Read<uint8_t>(player, VMP_OFF_IS_SPRINTING),
		VMP_Read<uint8_t>(player, VMP_OFF_STICKY_SPRINT),
		(VMP_Read<int>(player, VMP_OFF_BUTTONS) & VMP_IN_SPEED) != 0,
		VMP_Read<uint8_t>(player, VMP_OFF_SLIDING),
		flNow - VMP_Read<float>(player, VMP_OFF_ON_SLOPE_TIME),
		nWeapState,
		pWeapon ? VMP_IdealActivity(pWeapon) : -1);
}

void MantleBoostVmProbe_OnClimbStart(CPlayer* const player)
{
	if (!sdk_mantle_boost_vm_probe.GetBool())
		return;
	VmProbeSlot_t* const s = VMP_Slot(player);
	if (!s)
		return;
	s->m_flClimbStart = VMP_CurTime();
	s->m_flWindowEnd = s->m_flClimbStart + VMP_CLIMB_WINDOW;
	s->m_nTickLines = 0;
	VMP_Print(player, "climb-start");
}

void MantleBoostVmProbe_OnBoostStep(CPlayer* const player, const char* const pszStep)
{
	if (!VMP_Armed(player))
		return;
	VmProbeSlot_t* const s = VMP_Slot(player);
	s->m_flWindowEnd = VMP_CurTime() + VMP_BOOST_WINDOW;
	VMP_Print(player, pszStep);
}

void MantleBoostVmProbe_PostRunCommand(CPlayer* const player)
{
	if (!VMP_Armed(player))
		return;
	VmProbeSlot_t* const s = VMP_Slot(player);
	if (s->m_nTickLines >= VMP_MAX_TICK_LINES)
		return;
	++s->m_nTickLines;
	VMP_Print(player, "tick");
}

static void Hook_Player_StartSprint(CPlayer* player)
{
	const bool bArmed = VMP_Armed(player);
	if (bArmed)
		VMP_Print(player, "sprint-start");
	v_Player_StartSprint(player);
}

static void Hook_Player_StopSprint(CPlayer* player)
{
	// Stop raises every active weapon still in the sprint state from sprint; log
	// that state before the original changes it.
	if (VMP_Armed(player))
	{
		VMP_Print(player, "sprint-stop");
		for (int i = 0; i < 3; ++i)
		{
			const void* const pWeapon = VMP_ActiveWeapon(player, i);
			if (!pWeapon)
				continue;
			const int nState = *reinterpret_cast<const int*>(
				reinterpret_cast<uintptr_t>(pWeapon) + VMP_WEAPON_OFF_WEAPSTATE);
			if (nState == VMP_WEAPSTATE_SPRINT)
				Msg(eDLL_T::SERVER, "[MB-VM]   slot %d raises FROM SPRINT with sliding=%d\n",
					i, VMP_Read<uint8_t>(player, VMP_OFF_SLIDING));
		}
	}
	v_Player_StopSprint(player);
}

void VMantleBoostVmProbe::GetAdr(void) const
{
	LogFunAdr("Player_StartSprint", v_Player_StartSprint);
	LogFunAdr("Player_StopSprint", v_Player_StopSprint);
}

void VMantleBoostVmProbe::GetFun(void) const
{
	// Server half: +0x6D1C (sprint-start effects latch) only exists on CPlayer.
	Module_FindPattern(g_GameDll, "40 53 48 83 EC 30 80 B9 1C 6D 00 00 00 48 8B D9 75 ?? F3 0F 10 05")
		.GetPtr(v_Player_StartSprint);
	// Server half: +0x5B10 is CPlayer's active suit-device mask.
	Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 8B 05 ?? ?? ?? ?? "
		"48 8B F9 0F 29 7C 24 20 85 81 10 5B 00 00")
		.GetPtr(v_Player_StopSprint);

	if (!v_Player_StartSprint || !v_Player_StopSprint)
		Warning(eDLL_T::SERVER, "[MB-VM] sprint start/stop pattern unresolved -- sprint lines disabled\n");
}

void VMantleBoostVmProbe::Detour(const bool bAttach) const
{
	if (v_Player_StartSprint)
		DetourSetup(&v_Player_StartSprint, &Hook_Player_StartSprint, bAttach);
	if (v_Player_StopSprint)
		DetourSetup(&v_Player_StopSprint, &Hook_Player_StopSprint, bAttach);
}
