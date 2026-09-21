//=============================================================================//
//
// Purpose: usercmd dodge gate -- ground and once-per-airtime rules the S3
// movement code has no settings field for.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "public/const.h"
#include "game/shared/in_buttons.h"
#include "game/shared/usercmd.h"
#include "game/server/player.h"
#include "game/server/dodge_gate.h"

static ConVar sv_dodge_ground("sv_dodge_ground", "1", FCVAR_RELEASE,
	"Players may dodge while on the ground. 0 strips IN_DODGE unless airborne.");
static ConVar sv_dodge_once_in_air("sv_dodge_once_in_air", "1", FCVAR_RELEASE,
	"One dodge per airtime; the next needs a ground touch. 0 = every charge is usable midair.");
static ConVar sv_dodge_gate_diag("sv_dodge_gate_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[DODGE-GATE] log every strip and every latched airborne dodge.");

struct DodgeSlotState_t
{
	bool bAirDodgeUsed;
	bool bWasAirborne;
	float flLastDodgeTimeBefore;
};

static DodgeSlotState_t s_dodgeSlots[MAX_PLAYERS] = {};

static DodgeSlotState_t* DodgeGate_Slot(const CPlayer* const player)
{
	const int nSlot = static_cast<int>(player->GetEdict()) - 1;
	if (nSlot < 0 || nSlot >= MAX_PLAYERS)
		return nullptr;
	return &s_dodgeSlots[nSlot];
}

void DodgeGate_PreRun(CPlayer* player, CUserCmd* ucmd)
{
	if (!player || !ucmd)
		return;

	DodgeSlotState_t* const slot = DodgeGate_Slot(player);
	if (!slot)
		return;

	const bool bOnGround = (player->GetFlags() & FL_ONGROUND) != 0;
	slot->bWasAirborne = !bOnGround;
	slot->flLastDodgeTimeBefore = player->GetLastDodgeTime();

	if (bOnGround)
		slot->bAirDodgeUsed = false;

	if (!(ucmd->buttons & IN_DODGE))
		return;

	const char* pszReason = nullptr;
	if (bOnGround && !sv_dodge_ground.GetBool())
		pszReason = "ground";
	else if (!bOnGround && slot->bAirDodgeUsed && sv_dodge_once_in_air.GetBool())
		pszReason = "air-used";

	if (!pszReason)
		return;

	ucmd->buttons &= ~IN_DODGE;

	if (sv_dodge_gate_diag.GetBool())
		Msg(eDLL_T::SERVER, "[DODGE-GATE] strip edict=%d cmd=%d reason=%s\n",
			static_cast<int>(player->GetEdict()), ucmd->command_number, pszReason);
}

void DodgeGate_PostRun(CPlayer* player, CUserCmd* ucmd)
{
	if (!player || !ucmd)
		return;

	DodgeSlotState_t* const slot = DodgeGate_Slot(player);
	if (!slot || !slot->bWasAirborne)
		return;

	if (player->GetLastDodgeTime() == slot->flLastDodgeTimeBefore)
		return;

	slot->bAirDodgeUsed = true;

	if (sv_dodge_gate_diag.GetBool())
		Msg(eDLL_T::SERVER, "[DODGE-GATE] airborne dodge latched edict=%d cmd=%d t=%.3f\n",
			static_cast<int>(player->GetEdict()), ucmd->command_number, player->GetLastDodgeTime());
}
