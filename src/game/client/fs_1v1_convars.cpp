//=============================================================================//
//
// Purpose: Flowstate 1v1 settings ConVars (from the r5_flowstate_mod mod.vdf
//          "ConVars" block). S21 has no UI CreateConVar; these ship as statics.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/convar.h"
#include "game/client/fs_1v1_convars.h"

// 1v1 Settings (mod.vdf)
static ConVar fs_1v1_startinrest(
	"fs_1v1_startinrest", "0", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Start In Rest", true, 0.f, true, 1.f);

static ConVar fs_1v1_ibmm(
	"fs_1v1_ibmm", "0", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"IBMM enabled", true, 0.f, true, 1.f);

static ConVar fs_1v1_acceptchallenges(
	"fs_1v1_acceptchallenges", "1", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Accept Challenges", true, 0.f, true, 1.f);

static ConVar fs_1v1_showinputbanner(
	"fs_1v1_showinputbanner", "1", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Show Input Banner", true, 0.f, true, 1.f);

static ConVar fs_1v1_showvsui(
	"fs_1v1_showvsui", "1", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Show Vs UI", true, 0.f, true, 1.f);

static ConVar fs_1v1_camo(
	"fs_1v1_camo", "0", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Camo Color", true, 0.f, true, 9.f);

static ConVar fs_1v1_maxenemylatency(
	"fs_1v1_maxenemylatency", "999", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Max Enemy Latency", true, 5.f, true, 999.f);

static ConVar fs_1v1_maxibmmtime(
	"fs_1v1_maxibmmtime", "3", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Max time player will wait for IBMM", true, 0.f, true, 30.f);

//-----------------------------------------------------------------------------
// Purpose: referenced from client init so the linker keeps this object; the
//          ConVars above only exist through static registration
//-----------------------------------------------------------------------------
void FS1v1ConVars_Init(void)
{
	DevMsg(eDLL_T::CLIENT, "[FS-1V1] settings ConVars linked (%s)\n", fs_1v1_startinrest.GetName());
}
