//=============================================================================//
//
// Purpose: team alliances for multi-squad playlists on the bridge.
//
//=============================================================================//
#ifndef ALLIANCE_COMPAT_H
#define ALLIANCE_COMPAT_H

#include "thirdparty/detours/include/idetour.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"

// Script registration -- call from Script_RegisterServerFunctions next to
// DeathField_RegisterOnVM (same bridge pattern for new SERVER natives).
void AllianceCompat_RegisterOnVM(CSquirrelVM* s);

// C++ accessors for detours / diagnostics.
void AllianceCompat_SetTeamIsInAlliance(int team, int alliance, bool inAlliance);
bool AllianceCompat_AreTeamsInAlliance(int teamA, int teamB);
int  AllianceCompat_GetAllianceFromTeam(int team);

#if !defined(CLIENT_DLL)
// Goes through Hook_IsEnemyTeam so the alliance override is honoured.
bool AllianceCompat_IsFriendlyTeam(int teamA, int teamB);
#endif // !CLIENT_DLL

///////////////////////////////////////////////////////////////////////////////
class VAllianceCompat : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // ALLIANCE_COMPAT_H
