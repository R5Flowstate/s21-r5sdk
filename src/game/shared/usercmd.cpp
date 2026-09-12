#if defined(CLIENT_DLL)
//====== Copyright (c) 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose
//
// $NoKeywords: $
//
//=============================================================================//
#include "usercmd.h"
#include "tier1/utlbuffer.h"
#include "game/shared/in_buttons.h"
#include "game/shared/weapon_types.h"
#include "game/shared/shared_activity.h"

ConVar usercmd_frametime_max("usercmd_frametime_max", "0.100"   , FCVAR_REPLICATED | FCVAR_DEVELOPMENTONLY, "The largest amount of simulation seconds a UserCmd can have." );
ConVar usercmd_frametime_min("usercmd_frametime_min", "0.002857", FCVAR_REPLICATED | FCVAR_DEVELOPMENTONLY, "The smallest amount of simulation seconds a UserCmd can have.");

ConVar usercmd_dualwield_enable("usercmd_dualwield_enable", "0", FCVAR_REPLICATED | FCVAR_RELEASE, "Allows setting dual wield cycle slots, and activating multiple inventory weapons from UserCmd.");

//-----------------------------------------------------------------------------
// Purpose: Clamp untrusted usercommand.
// Input: *ucmd -
//-----------------------------------------------------------------------------
void ClampUserCmd(CUserCmd* ucmd)
{
	// Initialize the camera position as <0,0,0>, this should at least avoid
	// crash and meme behaviors.
	if (!ucmd->camerapos.IsValid())
		ucmd->camerapos.Init();

	// Viewangles must be normalized; applying invalid angles on the client
	// will result in undefined behavior.
	ucmd->viewangles.Normalize();
	ucmd->pitchangles.Normalize();

	// Some players abused a feature of the engine which allows you to perform
	// custom weapon activities. After some research, it appears that only the
	// 'ACT_VM_WEAPON_INSPECT' was supposed to work for standard games, all the
	// other activities appears to be for development or testing, therefore, we
	// should only allow 'ACT_VM_WEAPON_INSPECT' if cheats are disabled.
	if (!sv_cheats->GetBool() && ucmd->weaponactivity != ACT_VM_WEAPON_INSPECT)
		ucmd->weaponactivity = ACT_NONE;

	// On the client, the frame time must be within 'usercmd_frametime_min'
	// and 'usercmd_frametime_max'. Testing revealed that speed hacking could
	// be achieved by sending bogus frame times. Clamp the networked frame
	// time to the exact values that the client should be using to make sure
	// it couldn't be circumvented by busting out the client side clamps.
	if (host_timescale->GetFloat() == 1.0f)
		ucmd->frametime = Clamp(ucmd->frametime,
			usercmd_frametime_min.GetFloat(),
			usercmd_frametime_max.GetFloat());

	// Checks are only required if cycleslot is valid; see 'CPlayer::UpdateWeaponSlots'.
	// weaponindex folds below regardless: a HOLSTERED/ANY marker must never
	// smuggle a raw wire hand past this gate into UpdateWeaponSlots.
	const bool dualWieldEnabled = usercmd_dualwield_enable.GetBool();

	if (ucmd->cycleslot != WEAPON_INVENTORY_SLOT_INVALID
		&& ucmd->cycleslot != WEAPON_INVENTORY_SLOT_HOLSTERED
		&& ucmd->cycleslot != WEAPON_INVENTORY_SLOT_ANY)
	{
		// Dual-wield cycle slots stay blocked unless explicitly enabled.
		// Bound is the S21 gadget slot (5=ordnance, 6=survival), not S3 anti-titan. Dual wield starts at 7.
		if (!dualWieldEnabled && ucmd->cycleslot > WEAPON_INVENTORY_SLOT_S21_GADGET)
			ucmd->cycleslot = WEAPON_INVENTORY_SLOT_S21_GADGET;

		// m_selectedWeapons is size 2; clamp so it never reads OOB.
		// weaponindex INVALID is 0xFF -- invalidate the slot, do not fold it to a hand.
		if (ucmd->weaponindex == WEAPON_INVENTORY_SLOT_INVALID)
			ucmd->cycleslot = WEAPON_INVENTORY_SLOT_INVALID;
	}

	// Fold the hand unconditionally. Weapon_UpdateSelection indexes unchecked.
	if (ucmd->weaponindex != WEAPON_INVENTORY_SLOT_INVALID
		&& ucmd->weaponindex >= WEAPON_INVENTORY_SLOT_PRIMARY_1)
		dualWieldEnabled
		? ucmd->weaponindex = WEAPON_INVENTORY_SLOT_PRIMARY_1
		: ucmd->weaponindex = WEAPON_INVENTORY_SLOT_PRIMARY_0;
}

 //-----------------------------------------------------------------------------
// Purpose: Read in a delta compressed usercommand.
// Input: *buf -
// *move -
// *from -
// Output: random seed
//-----------------------------------------------------------------------------
int ReadUserCmd(bf_read* buf, CUserCmd* move, CUserCmd* from)
{
	const int seed = v_ReadUserCmd(buf, move, from);
	ClampUserCmd(move);

	return seed;
}
int ReadUserCmdExtended(bf_read* buf, CUserCmdExtended* move, CUserCmdExtended* from)
{
	const int seed = v_ReadUserCmdExtended(buf, move, from);
	ClampUserCmd(move);

	return seed;
}

//-----------------------------------------------------------------------------
void VUserCmd::Detour(const bool bAttach) const
{
	DetourSetup(&v_ReadUserCmd, &ReadUserCmd, bAttach);
	DetourSetup(&v_ReadUserCmdExtended, &ReadUserCmdExtended, bAttach);
}
#else // !CLIENT_DLL
//====== Copyright (c) 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose
//
// $NoKeywords: $
//
//=============================================================================//
#include "usercmd.h"
#include "tier1/utlbuffer.h"
#include "game/shared/in_buttons.h"
#include "game/shared/weapon_types.h"
#include "game/shared/shared_activity.h"

ConVar usercmd_frametime_max("usercmd_frametime_max", "0.100"   , FCVAR_REPLICATED | FCVAR_DEVELOPMENTONLY, "The largest amount of simulation seconds a UserCmd can have." );
ConVar usercmd_frametime_min("usercmd_frametime_min", "0.002857", FCVAR_REPLICATED | FCVAR_DEVELOPMENTONLY, "The smallest amount of simulation seconds a UserCmd can have.");

ConVar usercmd_dualwield_enable("usercmd_dualwield_enable", "0", FCVAR_REPLICATED | FCVAR_RELEASE, "Allows setting dual wield cycle slots, and activating multiple inventory weapons from UserCmd.");

static ConVar usercmd_clamp_diag("usercmd_clamp_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Emit a rate-limited [USERCMD-CLAMP] line for every command the server-side "
	"clamp actually modified.");

static long s_nClampDiagLines = 0;
static constexpr long kClampDiagCap = 64;

static ConVar usercmd_select_diag("usercmd_select_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Emit a capped [USERCMD-SEL] line for every accepted command carrying a "
	"weapon-select edge: final cycleslot, weaponindex, weaponselect word and "
	"setlastcycleslot.");

static long s_nSelectDiagLines = 0;
static constexpr long kSelectDiagCap = 160;

//-----------------------------------------------------------------------------
// Purpose: Clamp untrusted usercommand.
// Input: *ucmd -
//-----------------------------------------------------------------------------
void ClampUserCmd(CUserCmd* ucmd)
{
	// Initialize the camera position as <0,0,0>, this should at least avoid
	// crash and meme behaviors.
	if (!ucmd->camerapos.IsValid())
		ucmd->camerapos.Init();

	if (!ucmd->viewangles.IsValid())
		ucmd->viewangles.Init();
	if (!ucmd->pitchangles.IsValid())
		ucmd->pitchangles.Init();

	// Viewangles must be normalized; applying invalid angles on the client
	// will result in undefined behavior.
	ucmd->viewangles.Normalize();
	ucmd->pitchangles.Normalize();

	// Only S21 inspect (564) passes when cheats are off. 557 is S21
	// COOLDOWN_OVERHEAT_LATE1 on the wire; clamp runs before c2s xlat.
	static const int kS21_ACT_VM_WEAPON_INSPECT = 564;
	if (!sv_cheats->GetBool() && ucmd->weaponactivity != kS21_ACT_VM_WEAPON_INSPECT)
		ucmd->weaponactivity = ACT_NONE;

	// On the client, the frame time must be within 'usercmd_frametime_min'
	// and 'usercmd_frametime_max'. Testing revealed that speed hacking could
	// be achieved by sending bogus frame times. Clamp the networked frame
	// time to the exact values that the client should be using to make sure
	// it couldn't be circumvented by busting out the client side clamps.
	if (host_timescale->GetFloat() == 1.0f)
		ucmd->frametime = Clamp(ucmd->frametime,
			usercmd_frametime_min.GetFloat(),
			usercmd_frametime_max.GetFloat());

	// NaN survives Clamp untouched (both comparisons are false); scrub it so no
	// non-finite value leaves intake.
	if (ucmd->frametime != ucmd->frametime)
		ucmd->frametime = usercmd_frametime_min.GetFloat();

	if (ucmd->forwardmove != ucmd->forwardmove)
		ucmd->forwardmove = 0.f;
	else
		ucmd->forwardmove = Clamp(ucmd->forwardmove, -1.0e9f, 1.0e9f);
	if (ucmd->sidemove != ucmd->sidemove)
		ucmd->sidemove = 0.f;
	else
		ucmd->sidemove = Clamp(ucmd->sidemove, -1.0e9f, 1.0e9f);
	if (ucmd->upmove != ucmd->upmove)
		ucmd->upmove = 0.f;
	else
		ucmd->upmove = Clamp(ucmd->upmove, -1.0e9f, 1.0e9f);

	const byte nCycleBefore = ucmd->cycleslot;
	const byte nIndexBefore = ucmd->weaponindex;

	// Checks are only required if cycleslot is valid; see 'CPlayer::UpdateWeaponSlots'.
	// weaponindex folds below regardless: a HOLSTERED/ANY marker must never
	// smuggle a raw wire hand past this gate into UpdateWeaponSlots.
	const bool dualWieldEnabled = usercmd_dualwield_enable.GetBool();

	if (ucmd->cycleslot != WEAPON_INVENTORY_SLOT_INVALID
		&& ucmd->cycleslot != WEAPON_INVENTORY_SLOT_HOLSTERED
		&& ucmd->cycleslot != WEAPON_INVENTORY_SLOT_ANY)
	{
		// Dual-wield cycle slots stay blocked unless explicitly enabled.
		// Bound is the S21 gadget slot (5=ordnance, 6=survival), not S3 anti-titan. Dual wield starts at 7.
		if (!dualWieldEnabled && ucmd->cycleslot > WEAPON_INVENTORY_SLOT_S21_GADGET)
			ucmd->cycleslot = WEAPON_INVENTORY_SLOT_S21_GADGET;

		// m_selectedWeapons is size 2; clamp so it never reads OOB.
		// weaponindex INVALID is 0xFF -- invalidate the slot, do not fold it to a hand.
		if (ucmd->weaponindex == WEAPON_INVENTORY_SLOT_INVALID)
			ucmd->cycleslot = WEAPON_INVENTORY_SLOT_INVALID;
	}

	// Fold the hand unconditionally. Weapon_UpdateSelection indexes unchecked.
	if (ucmd->weaponindex != WEAPON_INVENTORY_SLOT_INVALID
		&& ucmd->weaponindex >= WEAPON_INVENTORY_SLOT_PRIMARY_1)
		dualWieldEnabled
		? ucmd->weaponindex = WEAPON_INVENTORY_SLOT_PRIMARY_1
		: ucmd->weaponindex = WEAPON_INVENTORY_SLOT_PRIMARY_0;

	if (usercmd_clamp_diag.GetBool()
		&& (ucmd->cycleslot != nCycleBefore || ucmd->weaponindex != nIndexBefore)
		&& s_nClampDiagLines < kClampDiagCap)
	{
		++s_nClampDiagLines;
		Warning(eDLL_T::SERVER,
			"[USERCMD-CLAMP] cmd=%d cycleslot %u->%u weaponindex %u->%u\n",
			ucmd->command_number, nCycleBefore, ucmd->cycleslot,
			nIndexBefore, ucmd->weaponindex);
	}

	if (usercmd_select_diag.GetBool() && s_nSelectDiagLines < kSelectDiagCap)
	{
		const int16_t nSelWord = ucmd->weaponselect;
		if (ucmd->cycleslot != WEAPON_INVENTORY_SLOT_INVALID
			|| (nSelWord & (int16_t)0x8000) == 0
			|| ucmd->setlastcycleslot)
		{
			++s_nSelectDiagLines;
			Warning(eDLL_T::SERVER,
				"[USERCMD-SEL] cmd=%d cycleslot=%u weaponindex=%u selword=0x%04X setlast=%d\n",
				ucmd->command_number, ucmd->cycleslot, ucmd->weaponindex,
				(unsigned)nSelWord & 0xFFFFu, ucmd->setlastcycleslot ? 1 : 0);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Read in a delta compressed usercommand.
// Input: *buf -
// *move -
// *from -
// Output: random seed
//-----------------------------------------------------------------------------
int ReadUserCmd(bf_read* buf, CUserCmd* move, CUserCmd* from)
{
	const int seed = v_ReadUserCmd(buf, move, from);
	ClampUserCmd(move);

	return seed;
}
int ReadUserCmdExtended(bf_read* buf, CUserCmdExtended* move, CUserCmdExtended* from)
{
	const int seed = v_ReadUserCmdExtended(buf, move, from);
	ClampUserCmd(move);

	return seed;
}

//-----------------------------------------------------------------------------
void VUserCmd::Detour(const bool bAttach) const
{
	DetourSetup(&v_ReadUserCmd, &ReadUserCmd, bAttach);
	DetourSetup(&v_ReadUserCmdExtended, &ReadUserCmdExtended, bAttach);
}
#endif // CLIENT_DLL
