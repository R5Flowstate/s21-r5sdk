//=============================================================================//
//
// Purpose: server demo recorder script natives. Arguments are validated here
//          and a bad one warns and returns: a raised error in the server VM
//          shuts the host game down.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/sqarray.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "public/demo/r5dem.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "engine/server/demo_record_sv.h"
#include "vscript_server.h"
#include "vscript_server_natives.h"
#include "player.h"
#include "demo_natives_sv.h"

static const char* DemoSvNative_String(HSQUIRRELVM v, const SQInteger idx)
{
	const SQChar* psz = nullptr;
	if (SQ_FAILED(sq_getstring(v, idx, &psz)) || !psz)
		return nullptr;
	return psz;
}

static CPlayer* DemoSvNative_Player(HSQUIRRELVM v, const SQInteger idx)
{
	return reinterpret_cast<CPlayer*>(ServerScript_EntityPtrFromStackIdx(v, idx));
}

static SQRESULT ServerScript_Demo_ServerStart(HSQUIRRELVM v)
{
	const char* const pszMatch = DemoSvNative_String(v, 2);
	const SQObjectPtr& arrObj = stack_get(v, 3);
	if (!pszMatch || !sq_isarray(arrObj))
	{
		Warning(eDLL_T::SERVER, "[DEMO-SV] Demo_ServerStart( string matchId, array<entity> povs ) -- bad arguments\n");
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	CPlayer* povs[R5DEM_MAX_POVS] = {};
	int nPovs = 0;
	const SQArray* const pArr = _array(arrObj);
	const SQInteger nSize = pArr ? pArr->Size() : 0;
	for (SQInteger i = 0; i < nSize && nPovs < R5DEM_MAX_POVS; ++i)
	{
		const SQObjectPtr& el = pArr->_values[i];
		if (el._type != OT_ENTITY || !el._unVal.pInstance)
			continue;
		void* const pEnt = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(el._unVal.pInstance) + 0x50);
		if (pEnt)
			povs[nPovs++] = reinterpret_cast<CPlayer*>(pEnt);
	}

	sq_pushbool(v, nPovs > 0 && DemoSv_Start(pszMatch, povs, nPovs));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_Demo_ServerStop(HSQUIRRELVM v)
{
	const char* const pszMatch = DemoSvNative_String(v, 2);
	CPlayer* const pWinner = DemoSvNative_Player(v, 3);
	const char* const pszReason = DemoSvNative_String(v, 4);
	sq_pushbool(v, pszMatch && DemoSv_Stop(pszMatch, pWinner, pszReason ? pszReason : "unknown"));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_Demo_ServerEvent(HSQUIRRELVM v)
{
	const char* const pszMatch = DemoSvNative_String(v, 2);
	const char* const pszType = DemoSvNative_String(v, 3);
	CPlayer* const pAttacker = DemoSvNative_Player(v, 4);
	CPlayer* const pVictim = DemoSvNative_Player(v, 5);
	const char* const pszWeapon = DemoSvNative_String(v, 6);
	SQFloat flDamage = 0.0f;
	sq_getfloat(v, 7, &flDamage);
	sq_pushbool(v, pszMatch && pszType && DemoSv_Event(pszMatch, pszType, pAttacker, pVictim, pszWeapon, flDamage));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_Demo_ServerIsRecording(HSQUIRRELVM v)
{
	const char* const pszMatch = DemoSvNative_String(v, 2);
	sq_pushbool(v, pszMatch && DemoSv_IsRecording(pszMatch));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_Demo_ServerEnabled(HSQUIRRELVM v)
{
	sq_pushbool(v, DemoSv_Enabled());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void DemoSv_RegisterServerFunctions(CSquirrelVM* s)
{
	Script_RegisterFuncNamed(s, "Demo_ServerStart", "Script_Demo_ServerStart",
		"Start recording a match; one pov per player. False when sv_demo_record is 0 or a pov cannot be recorded",
		"bool", "string matchId, array<entity> povPlayers", false, ServerScript_Demo_ServerStart);
	Script_RegisterFuncNamed(s, "Demo_ServerStop", "Script_Demo_ServerStop",
		"Finish a match recording with its winner and a reason token",
		"bool", "string matchId, entity ornull winner, string reason", false, ServerScript_Demo_ServerStop);
	Script_RegisterFuncNamed(s, "Demo_ServerEvent", "Script_Demo_ServerEvent",
		"Add a timeline event (kill, knock, damage, round_start, round_end) to a match recording",
		"bool", "string matchId, string type, entity ornull attacker, entity ornull victim, string weapon, float damage",
		false, ServerScript_Demo_ServerEvent);
	Script_RegisterFuncNamed(s, "Demo_ServerIsRecording", "Script_Demo_ServerIsRecording",
		"True while the match is being recorded", "bool", "string matchId", false, ServerScript_Demo_ServerIsRecording);
	Script_RegisterFuncNamed(s, "Demo_ServerEnabled", "Script_Demo_ServerEnabled",
		"True when sv_demo_record allows match recordings", "bool", "", false, ServerScript_Demo_ServerEnabled);
}
