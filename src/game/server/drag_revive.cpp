//=============================================================================//
//
// Purpose: Drag revive (Newcastle passive) natives. See drag_revive.h.
//
//=============================================================================//
#include "core/stdafx.h"

#include "drag_revive.h"
#include "player_stance.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "public/edict.h"
#include <cmath>

extern CGlobalVars* gpGlobals;

static constexpr int   DRAG_REVIVE_STATE_NONE = 0;
static constexpr int   DRAG_REVIVE_STATE_ACTIVE = 1;
static constexpr float DRAG_REVIVE_NO_OUTRO = -1.0f;

static void* DR_ScriptThis(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return nullptr;
	return pEntity;
}

static void DR_Mirror(void* pPlayer, const int nState, const float flOutroStart, const uint32_t nTargetHandle)
{
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_dragReviveState), nState);
	PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_dragReviveOutroStartTime), flOutroStart);
	PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_reviveTarget),
		SDKEntityState_PackS21RecvEHandle(static_cast<int32_t>(nTargetHandle)));
	MarkEntityEdictDirty(pPlayer);
}

// Player_StartDragRevive( float outroDelayTime, entity reviveTarget ): crouch now.
static SQRESULT Script_StartDragRevive(HSQUIRRELVM v)
{
	void* const pPlayer = DR_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;

	SQFloat flOutroDelay = 0.0f;
	sq_getfloat(v, 2, &flOutroDelay);
	if (!std::isfinite(flOutroDelay))
	{
		Warning(eDLL_T::SERVER, "[DRAG-REVIVE] Player_StartDragRevive rejected non-finite outro delay\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	uint32_t nTarget = INVALID_EHANDLE_INDEX;
	const SQObjectPtr& o = stack_get(v, 3);
	if (o._type == OT_ENTITY && o._unVal.pInstance)
	{
		void* const pTarget = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o._unVal.pInstance) + 0x50);
		if (pTarget)
			nTarget = SDKEntityState_GetHandle(pTarget).Raw();
	}

	PlayerStance_SetInstant(pPlayer, true);
	const float flNow = gpGlobals ? gpGlobals->curTime : 0.0f;
	DR_Mirror(pPlayer, DRAG_REVIVE_STATE_ACTIVE, flNow + static_cast<float>(flOutroDelay), nTarget);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Player_FinishDragRevive( entity reviveTarget ): the target argument is unused, as on the client.
static SQRESULT Script_FinishDragRevive(HSQUIRRELVM v)
{
	void* const pPlayer = DR_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;
	DR_Mirror(pPlayer, DRAG_REVIVE_STATE_NONE, DRAG_REVIVE_NO_OUTRO, INVALID_EHANDLE_INDEX);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetDragReviveState(HSQUIRRELVM v)
{
	void* const pPlayer = DR_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;
	sq_pushinteger(v, PlayerExtend_GetI32(pPlayer, offsetof(PlayerExtendWire, m_dragReviveState)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void DragRevive_RegisterScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	playerStruct->AddFunction("Player_StartDragRevive", "Script_StartDragRevive",
		"Starts dragging a downed ally while reviving them", "void",
		"float outroDelayTime, entity reviveTarget", false, Script_StartDragRevive);
	playerStruct->AddFunction("Player_FinishDragRevive", "Script_FinishDragRevive",
		"Ends the drag revive", "void", "entity reviveTarget", false, Script_FinishDragRevive);
	playerStruct->AddFunction("GetDragReviveState", "Script_GetDragReviveState",
		"Drag revive state (0 none, 1 dragging)", "int", "", false, Script_GetDragReviveState);
}
