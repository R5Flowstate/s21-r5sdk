//=============================================================================//
//
// Purpose: ContextAction emote natives. See context_action.h.
//
//=============================================================================//
#include "core/stdafx.h"


#include "context_action.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript_server.h"
#include "tier0/dbg.h"
#include <cstddef>

static constexpr ptrdiff_t kContextAction = 0x17FC; // S3 server BCC m_contextAction
static constexpr int kContextActionNone  = 0;
static constexpr int kContextActionEmote = 13;

static int* ContextAction_Field(void* pEnt)
{
	return reinterpret_cast<int*>(reinterpret_cast<char*>(pEnt) + kContextAction);
}

static void ContextAction_LogOnce(const char* tag, void* pEnt)
{
	static LONG s_logged = 0;
	if (InterlockedIncrement(&s_logged) != 1)
		return;
	Msg(eDLL_T::SERVER, "[CTX-ACT] %s first call ent=%p\n", tag, pEnt);
}

static SQRESULT Script_ContextAction_SetEmoting(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	int* const pField = ContextAction_Field(pEnt);
	const int cur = *pField;
	if (cur != kContextActionNone)
	{
		Warning(eDLL_T::SERVER, "[CTX-ACT] SetEmoting blocked cur=%d\n", cur);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	MarkEntityEdictDirty(pEnt);
	*pField = kContextActionEmote;
	ContextAction_LogOnce("SetEmoting", pEnt);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_ContextAction_ClearEmoting(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	int* const pField = ContextAction_Field(pEnt);
	const int cur = *pField;
	if (cur == kContextActionEmote)
	{
		MarkEntityEdictDirty(pEnt);
		*pField = kContextActionNone;
		ContextAction_LogOnce("ClearEmoting", pEnt);
	}
	else if (cur != kContextActionNone)
	{
		Warning(eDLL_T::SERVER, "[CTX-ACT] ClearEmoting refused cur=%d\n", cur);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_ContextAction_IsEmoting(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	sq_pushbool(v, *ContextAction_Field(pEnt) == kContextActionEmote);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void ContextAction_RegisterScriptFunctions(ScriptClassDescriptor_t* bccStruct)
{
	if (!bccStruct)
		return;

	bccStruct->AddFunction(
		"ContextAction_SetEmoting",
		"Script_ContextAction_SetEmoting",
		"Marks this combat character as performing an emote context action.",
		"void",
		"",
		false,
		Script_ContextAction_SetEmoting);
	bccStruct->AddFunction(
		"ContextAction_ClearEmoting",
		"Script_ContextAction_ClearEmoting",
		"Clears the emote context action if this combat character is emoting.",
		"void",
		"",
		false,
		Script_ContextAction_ClearEmoting);
	bccStruct->AddFunction(
		"ContextAction_IsEmoting",
		"Script_ContextAction_IsEmoting",
		"Returns true if this combat character is in the emote context action.",
		"bool",
		"",
		false,
		Script_ContextAction_IsEmoting);
}

