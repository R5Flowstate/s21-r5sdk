//=============================================================================//
//
// Purpose: bridge alliance membership for FreeDM TDM / Control / mixtape.
//
//=============================================================================//
#include "core/stdafx.h"
#include "alliance_compat.h"

#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"

//-----------------------------------------------------------------------------
// Constants -- S21 TEAM_ALLIANCES_COUNT is 2; teams are 0..127.
//-----------------------------------------------------------------------------
static constexpr int kAllianceNone     = -1;
static constexpr int kAllianceMax      = 2;
static constexpr int kTeamMax          = 128;

// team index -> alliance (0 or 1), or kAllianceNone
static int s_teamAlliance[kTeamMax];
static bool s_matrixInited = false;

static ConVar bridge_alliance_log("bridge_alliance_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log alliance matrix writes / friend-foe overrides (0=off, 1=writes, 2=hot path).");

static void AllianceCompat_EnsureInit(void)
{
	if (s_matrixInited)
		return;
	for (int i = 0; i < kTeamMax; ++i)
		s_teamAlliance[i] = kAllianceNone;
	s_matrixInited = true;
}

int AllianceCompat_GetAllianceFromTeam(int team)
{
	AllianceCompat_EnsureInit();
	if (team < 0 || team >= kTeamMax)
		return kAllianceNone;
	return s_teamAlliance[team];
}

bool AllianceCompat_AreTeamsInAlliance(int teamA, int teamB)
{
	AllianceCompat_EnsureInit();
	if (teamA < 0 || teamA >= kTeamMax || teamB < 0 || teamB >= kTeamMax)
		return false;
	const int a = s_teamAlliance[teamA];
	const int b = s_teamAlliance[teamB];
	if (a == kAllianceNone || b == kAllianceNone)
		return false;
	return a == b;
}

void AllianceCompat_SetTeamIsInAlliance(int team, int alliance, bool inAlliance)
{
	AllianceCompat_EnsureInit();
	if (team < 0 || team >= kTeamMax)
		return;
	if (alliance < 0 || alliance >= kAllianceMax)
		return;

	if (inAlliance)
		s_teamAlliance[team] = alliance;
	else if (s_teamAlliance[team] == alliance)
		s_teamAlliance[team] = kAllianceNone;

	if (bridge_alliance_log.GetInt() > 0)
	{
		Msg(eDLL_T::COMMON, "[ALLIANCE] SetTeamIsInAlliance team=%d alliance=%d in=%d -> now=%d\n",
			team, alliance, inAlliance ? 1 : 0, s_teamAlliance[team]);
	}
}

//-----------------------------------------------------------------------------
// Script: SetTeamIsInAlliance(int team, int alliance, bool inAlliance)
// S21 SERVER-only native; we expose on both VMs so client friend/foe
// matrix matches the dedi after AllianceProximity_SetTeamToAlliance_Internal.
//-----------------------------------------------------------------------------
static SQRESULT SharedScript_SetTeamIsInAlliance(HSQUIRRELVM v)
{
	SQInteger team = 0;
	SQInteger alliance = 0;
	SQBool inAlliance = SQFalse;

	if (SQ_FAILED(sq_getinteger(v, 2, &team))
		|| SQ_FAILED(sq_getinteger(v, 3, &alliance))
		|| SQ_FAILED(sq_getbool(v, 4, &inAlliance)))
	{
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	AllianceCompat_SetTeamIsInAlliance(
		static_cast<int>(team),
		static_cast<int>(alliance),
		inAlliance != SQFalse);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// S21 scripts only CALL these -- never re-define in .nut. SERVER VM only.

static SQRESULT SharedScript_IsTeamInAlliance(HSQUIRRELVM v)
{
	SQInteger team = 0;
	SQInteger alliance = 0;
	if (SQ_FAILED(sq_getinteger(v, 2, &team))
		|| SQ_FAILED(sq_getinteger(v, 3, &alliance)))
	{
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	const int got = AllianceCompat_GetAllianceFromTeam(static_cast<int>(team));
	const bool in = (got != kAllianceNone && got == static_cast<int>(alliance));
	sq_pushbool(v, in);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void AllianceCompat_RegisterOnVM(CSquirrelVM* s)
{
	if (!s)
		return;

#if defined(CLIENT_DLL)
	// Client: S21 scripts never call SetTeamIsInAlliance (SERVER only).
	// IsTeamInAlliance is already an S21 engine native. No registration.
	(void)s;
	return;
#else
	Script_RegisterFuncNamed(s, "SetTeamIsInAlliance",
		"Script_SetTeamIsInAlliance",
		"Put or remove a team (squad) from an alliance (S21 native)",
		"void", "int team, int alliance, bool inAlliance", false,
		SharedScript_SetTeamIsInAlliance);

	Script_RegisterFuncNamed(s, "IsTeamInAlliance",
		"Script_IsTeamInAlliance",
		"True if team is in the given alliance (S21 native)",
		"bool", "int team, int alliance", false,
		SharedScript_IsTeamInAlliance);

	static bool s_announced = false;
	if (!s_announced)
	{
		s_announced = true;
		Msg(eDLL_T::SERVER, "[ALLIANCE] RegisterOnVM: SetTeamIsInAlliance + IsTeamInAlliance\n");
	}
#endif
}

//-----------------------------------------------------------------------------
// Engine detours. Dedi: IsEnemyTeam only (damage / friend-foe authority).
//-----------------------------------------------------------------------------
#if !defined(CLIENT_DLL)

// Dedi IsEnemyTeam -- damage and the script-side IsEnemyTeam both land here.
// IsFriendlyTeam = !IsEnemy then same-team / both>1 -- so fixing IsEnemy alone
// makes multi-squad allies friendly on the dedi.
static char (*v_IsEnemyTeam)(unsigned int teamA, unsigned int teamB) = nullptr;

static char Hook_IsEnemyTeam(unsigned int teamA, unsigned int teamB)
{
	if (AllianceCompat_AreTeamsInAlliance(static_cast<int>(teamA), static_cast<int>(teamB)))
	{
		if (bridge_alliance_log.GetInt() >= 2)
		{
			static int s_n = 0;
			if (s_n++ < 40)
				Msg(eDLL_T::SERVER, "[ALLIANCE] IsEnemyTeam override %u/%u -> 0 (allies)\n", teamA, teamB);
		}
		return 0;
	}
	if (v_IsEnemyTeam)
		return v_IsEnemyTeam(teamA, teamB);
	if (teamA == teamB || teamA <= 1 || teamB <= 1)
		return 0;
	return 1;
}

bool AllianceCompat_IsFriendlyTeam(int teamA, int teamB)
{
	return Hook_IsEnemyTeam(static_cast<unsigned>(teamA), static_cast<unsigned>(teamB)) == 0;
}

#endif // !CLIENT_DLL

//-----------------------------------------------------------------------------
// VAllianceCompat
//-----------------------------------------------------------------------------
void VAllianceCompat::GetAdr(void) const
{
#if !defined(CLIENT_DLL)
	LogFunAdr("IsEnemyTeam", v_IsEnemyTeam);
#endif
}

void VAllianceCompat::GetFun(void) const
{
#if !defined(CLIENT_DLL)
	// Dedi IsEnemyTeam pattern (unique in the dedi image).
	Module_FindPattern(g_GameDll,
		"44 8B CA 4C 8D 1D ?? ?? ?? ?? 44 8B D1 83 F9 ?? 77 ?? "
		"45 8B C2 41 8B C2 49 C1 E8 ?? 83 E0 ?? 0F B6 C8 4F 8B 84 C3")
		.GetPtr(v_IsEnemyTeam);

	if (!v_IsEnemyTeam)
		Warning(eDLL_T::SERVER, "[ALLIANCE] IsEnemyTeam pattern unresolved\n");
#endif
}

void VAllianceCompat::Detour(const bool bAttach) const
{
#if !defined(CLIENT_DLL)
	if (v_IsEnemyTeam)
		DetourSetup(&v_IsEnemyTeam, &Hook_IsEnemyTeam, bAttach);
	Msg(eDLL_T::SERVER, "[ALLIANCE] VAllianceCompat dedi IsEnemyTeam detour %s\n",
		bAttach ? "attach" : "detach");
#else
	// Client product does not REGISTER this class (SERVER-only natives).
	(void)bAttach;
#endif
}
