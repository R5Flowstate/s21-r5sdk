#if defined(CLIENT_DLL)
// No client-side weapon script-var code. The S21 client binds these natives itself and
// the SDK registration path that reached this half no longer exists.

#else // !CLIENT_DLL
//=============================================================================
//
// Purpose: Extended weapon script variables (ScriptFloat0, ScriptInt1, ScriptTime1).
//
//=============================================================================

#include "core/stdafx.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "weapon_script_vars.h"
#include "weapon_enforce.h"
#include "heap_canary.h"
#include "dt_extend.h"
#include "player_extend_sidecar.h"
#include "sdk_entity_state.h"
#include "edict_dirty.h"
#include "offhand_slots_ext.h"
#include "tier1/keyvalues.h"
#include "game/shared/r1/weapon_parse.h"
// This TU is game_shared_static (neither SERVER_DLL nor CLIENT_DLL).
// Include both entity headers so VM-context dispatch can reach both GetScriptInstance twins.
#include "game/server/baseentity.h"
#include "game/client/c_baseentity.h"
#include "game/server/r1/weapon_x.h"
#include "game/server/trigger_cannon.h"
#include "game/server/trigger_gravity.h"
#include "game/server/entitylist.h"
#include "engine/server/vengineserver_impl.h"
#include "game/server/basecombatcharacter.h"

#include <unordered_map>

struct WeaponExtScriptVars
{
	float scriptFloat0 = 0.0f;
	int scriptInt1 = 0;
	float scriptTime1 = 0.0f;
};

// Sibling per-side maps. ScriptFloat0 / ScriptInt1 / ScriptTime1 natives are
// registered on both VMs via WeaponScriptVars_RegisterWeaponFuncs called from
// vscript_client.cpp + vscript_server.cpp.
static SDKEntityMap<WeaponExtScriptVars> s_weaponScriptVarsServer(
	ESide::Server, "weaponExtScriptVars.srv");
static SDKEntityMap<WeaponExtScriptVars> s_weaponScriptVarsClient(
	ESide::Client, "weaponExtScriptVars.cli");

static SDKEntityMap<WeaponExtScriptVars>& GetWeaponVarsMap(HSQUIRRELVM v)
{
	return (v && v->GetContext() == SQCONTEXT::SERVER) ? s_weaponScriptVarsServer
	                                                   : s_weaponScriptVarsClient;
}

static WeaponExtScriptVars& GetWeaponVars(HSQUIRRELVM v, void* pWeapon)
{
	return GetWeaponVarsMap(v)[pWeapon];
}

static void WeaponScriptVars_ClearHighlightMaps();
static void WeaponScriptVars_ClearWeaponTypeDisabledMap();
static void TurretDriver_LevelShutdown(void);

void WeaponScriptVars_LevelShutdown()
{
	s_weaponScriptVarsServer.Clear();
	s_weaponScriptVarsClient.Clear();
	WeaponScriptVars_ClearHighlightMaps();
	WeaponScriptVars_ClearWeaponTypeDisabledMap();
	TurretDriver_LevelShutdown();
}

//-----------------------------------------------------------------------------
// ScriptFloat0
//-----------------------------------------------------------------------------
static SQRESULT Script_SetScriptFloat0(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	SQFloat value;
	sq_getfloat(v, 2, &value);

	GetWeaponVars(v, pWeapon).scriptFloat0 = static_cast<float>(value);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetScriptFloat0(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushfloat(v, GetWeaponVars(v, pWeapon).scriptFloat0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// ScriptInt1
//-----------------------------------------------------------------------------
static constexpr int SCRIPT_INT_MIN = -65536;
static constexpr int SCRIPT_INT_MAX = 65535;

static SQRESULT Script_SetScriptInt1(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	SQInteger value;
	sq_getinteger(v, 2, &value);

	const int intVal = static_cast<int>(value);

	if (intVal < SCRIPT_INT_MIN || intVal > SCRIPT_INT_MAX)
	{
		v_SQVM_ScriptError("ScriptInt1 value %i out of range (%i to %i)",
			intVal, SCRIPT_INT_MIN, SCRIPT_INT_MAX);
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	GetWeaponVars(v, pWeapon).scriptInt1 = intVal;
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetScriptInt1(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushinteger(v, GetWeaponVars(v, pWeapon).scriptInt1);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// ScriptTime1
//-----------------------------------------------------------------------------
static SQRESULT Script_SetScriptTime1(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	SQFloat value;
	sq_getfloat(v, 2, &value);

	GetWeaponVars(v, pWeapon).scriptTime1 = static_cast<float>(value);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetScriptTime1(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushfloat(v, GetWeaponVars(v, pWeapon).scriptTime1);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// IsWeaponX: answered from the entity SendTable hierarchy so one binding serves every caller.
static SQRESULT Script_IsWeaponX(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	const bool isWeapon =
		v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) && pEntity &&
		DTExtend_EntityHasSendTable(pEntity, "DT_WeaponX");

	sq_pushbool(v, isWeapon);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Cosmetic identity for GetItemFlavorByGUID. Sidecar behind DT_BaseAnimating.m_itemFlavorGUID.
static SQRESULT Script_GetItemFlavorGUID(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	int64_t guid = 0;
	if (v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) && pEntity)
		DTExtend_GetItemFlavorGUID(pEntity, &guid);

	sq_pushinteger(v, static_cast<SQInteger>(guid));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetItemFlavorGUID(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	SQInteger guid = 0;
	sq_getinteger(v, 2, &guid);

	if (DTExtend_SetItemFlavorGUID(pEntity, static_cast<int64_t>(guid)))
		MarkEntityEdictDirty(pEntity);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetupArtifact(HSQUIRRELVM v)
{
	DevMsg(eDLL_T::CLIENT, "SetupArtifact called - stub\n");
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetWeaponCharmOrArtifactBladeGUID(HSQUIRRELVM v)
{
	DevMsg(eDLL_T::CLIENT, "SetWeaponCharmOrArtifactBladeGUID called - stub\n");
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_StartCustomActivityDetailed(HSQUIRRELVM v)
{
	DevMsg(eDLL_T::CLIENT, "StartCustomActivityDetailed called - stub\n");
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// IsWeaponOffhandMelee
//-----------------------------------------------------------------------------
static SQRESULT Script_IsWeaponOffhandMelee(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	const bool result = reinterpret_cast<CWeaponX*>(pWeapon)->IsWeaponOffhandMelee();

	sq_pushbool(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// C++ readers with no SQVM: HUD and heat pose. Both are client-side; route through the client map.
float WeaponScriptVars_GetScriptFloat0(void* pWeapon)
{
	if (!pWeapon) return 0.0f;
	if (WeaponExtScriptVars* pSrv = s_weaponScriptVarsServer.Find(pWeapon))
		return pSrv->scriptFloat0;
	WeaponExtScriptVars* p = s_weaponScriptVarsClient.Find(pWeapon);
	return p ? p->scriptFloat0 : 0.0f;
}

void WeaponScriptVars_SetScriptFloat0(void* pWeapon, float value)
{
	if (!pWeapon) return;
	s_weaponScriptVarsClient[pWeapon].scriptFloat0 = value;
}

// Forward declarations for weapon locked set (defined after PhaseShift block)
static SQRESULT Script_GetWeaponLockedSet(HSQUIRRELVM v);
static SQRESULT Script_SetWeaponLockedSet(HSQUIRRELVM v);

// S3 never registered Holster/FastHolster. Both bind to HolsterInternal; FastHolster's scale arg is ignored.
static SQRESULT Script_WeaponHolster(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	if (!v_WeaponX_HolsterInternal)
	{
		// GetFun's Warning already announced the pattern miss -- no-op here.
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const bool result = v_WeaponX_HolsterInternal(pWeapon, false) != 0;
	sq_pushbool(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// FastHolster: HolsterInternal(this, true). The optional scale arg is accepted and ignored.
static SQRESULT Script_WeaponFastHolster(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	SQFloat fastHolsterScale = -1.0f;
	sq_getfloat(v, 2, &fastHolsterScale); // optional; S3 native has no such param, ignored

	if (!v_WeaponX_HolsterInternal)
	{
		// GetFun's Warning already announced the pattern miss -- no-op here.
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const bool result = v_WeaponX_HolsterInternal(pWeapon, true) != 0;
	sq_pushbool(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Sidecar holds only weapons a script has explicitly set. Miss means 1 --
// the client's constructor defaults m_targetingLaserEnabledScript to true.
static SDKEntityMap<int> s_targetingLaserEnabled(ESide::Server, "targetingLaserEnabled.srv");

int WeaponScriptVars_WireGetTargetingLaserEnabled(void* pWeapon)
{
	if (!pWeapon)
		return 0;
	const int* const p = s_targetingLaserEnabled.Find(pWeapon);
	return p ? *p : 1;
}

static SQRESULT Script_GetTargetingLaserEnabled(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
		return SQ_ERROR;

	sq_pushbool(v, WeaponScriptVars_WireGetTargetingLaserEnabled(pWeapon) != 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static ConVar bridge_laser_diag("bridge_laser_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[LASER-DIAG] log the laser-colour per-tick bridge: one-shot arm line plus "
	"every state change. 0 = silent.");

static SQRESULT Script_SetTargetingLaserEnabled(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
		return SQ_ERROR;

	SQBool enabled = SQFalse;
	sq_getbool(v, 2, &enabled);

	s_targetingLaserEnabled[pWeapon] = enabled ? 1 : 0;
	MarkEntityEdictDirty(pWeapon);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Defined after OffhandOverride_ResolveOffhandEntityServer; weapon method, server-only.
static SQRESULT Script_GetOffhandActiveSlot(HSQUIRRELVM v);
static SQRESULT Script_GetInternalModBitField(HSQUIRRELVM v);

void WeaponScriptVars_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	DevMsg(eDLL_T::CLIENT, "[WeaponScriptVars] Registering weapon script variable functions\n");

	weaponStruct->AddFunction(
		"SetScriptFloat0",
		"Script_SetScriptFloat0",
		"Sets extended script float 0 on this weapon",
		"void",
		"float value",
		false,
		Script_SetScriptFloat0);

	weaponStruct->AddFunction(
		"GetScriptFloat0",
		"Script_GetScriptFloat0",
		"Gets extended script float 0 from this weapon",
		"float",
		"",
		false,
		Script_GetScriptFloat0);

	weaponStruct->AddFunction(
		"SetScriptInt1",
		"Script_SetScriptInt1",
		"Sets extended script int 1 on this weapon (range: -65536 to 65535)",
		"void",
		"int value",
		false,
		Script_SetScriptInt1);

	weaponStruct->AddFunction(
		"GetScriptInt1",
		"Script_GetScriptInt1",
		"Gets extended script int 1 from this weapon",
		"int",
		"",
		false,
		Script_GetScriptInt1);

	weaponStruct->AddFunction(
		"SetScriptTime1",
		"Script_SetScriptTime1",
		"Sets extended script time 1 on this weapon",
		"void",
		"float value",
		false,
		Script_SetScriptTime1);

	weaponStruct->AddFunction(
		"GetScriptTime1",
		"Script_GetScriptTime1",
		"Gets extended script time 1 from this weapon",
		"float",
		"",
		false,
		Script_GetScriptTime1);

	weaponStruct->AddFunction(
		"IsWeaponOffhandMelee",
		"Script_IsWeaponOffhandMelee",
		"Returns true if this weapon's fire mode is offhandMelee",
		"bool",
		"",
		false,
		Script_IsWeaponOffhandMelee);

	// IsWeaponX, GetItemFlavorGUID and SetItemFlavorGUID belong to the entity
	// descriptor, not this one -- weapons inherit them from there.
	weaponStruct->AddFunction(
		"SetupArtifact",
		"Script_SetupArtifact",
		"Sets up artifact visuals on the weapon",
		"void",
		"int tier, string bladeSkinName, string powerSourceModifier, int componentChanged",
		false,
		Script_SetupArtifact);

	weaponStruct->AddFunction(
		"SetWeaponCharmOrArtifactBladeGUID",
		"Script_SetWeaponCharmOrArtifactBladeGUID",
		"Sets the weapon charm or artifact blade GUID on this weapon",
		"void",
		"int guid",
		false,
		Script_SetWeaponCharmOrArtifactBladeGUID);

	weaponStruct->AddFunction(
		"StartCustomActivityDetailed",
		"Script_StartCustomActivityDetailed",
		"Starts a custom activity with detailed parameters",
		"void",
		"string activity, int flags, float duration, string thirdPersonActivity",
		false,
		Script_StartCustomActivityDetailed);

	// WeaponLockedSet -- getter on all VMs (SetWeaponLockedSet is SERVER-only, registered separately)
	weaponStruct->AddFunction(
		"GetWeaponLockedSet",
		"Script_GetWeaponLockedSet",
		"Get the weapon's locked set",
		"int",
		"",
		false,
		Script_GetWeaponLockedSet);

	weaponStruct->AddFunction(
		"Holster",
		"Script_WeaponHolster",
		"Holsters this weapon (S3 native CWeaponX::HolsterInternal(false)); returns whether the holster started",
		"bool",
		"",
		false,
		Script_WeaponHolster);

	weaponStruct->AddFunction(
		"FastHolster",
		"Script_WeaponFastHolster",
		"Fast-holsters this weapon (S3 native CWeaponX::HolsterInternal(true)); S21's optional fastHolsterScale arg is accepted and ignored (S3 native predates it)",
		"bool",
		"",
		false,
		Script_WeaponFastHolster);

	weaponStruct->AddFunction(
		"GetTargetingLaserEnabled",
		"Script_GetTargetingLaserEnabled",
		"Returns this weapon's script-controlled targeting laser gate (m_targetingLaserEnabledScript)",
		"bool",
		"",
		false,
		Script_GetTargetingLaserEnabled);

	weaponStruct->AddFunction(
		"SetTargetingLaserEnabled",
		"Script_SetTargetingLaserEnabled",
		"Sets this weapon's script-controlled targeting laser gate (m_targetingLaserEnabledScript)",
		"void",
		"bool enabled",
		false,
		Script_SetTargetingLaserEnabled);

	// Server-only: client already has this native.
	weaponStruct->AddFunction(
		"GetOffhandActiveSlot",
		"Script_GetOffhandActiveSlot",
		"Returns this weapon's offhand_active_slot (eActiveInventorySlot).",
		"int",
		"",
		false,
		Script_GetOffhandActiveSlot);

	weaponStruct->AddFunction(
		"GetInternalModBitField",
		"Script_GetInternalModBitField",
		"Returns this weapon's internal mod bitfield -- the value SetModBitField takes.",
		"int",
		"",
		false,
		Script_GetInternalModBitField);
}

static constexpr int HIGHLIGHT_TEAM_MAX_SLOTS = 8;

struct EntityHighlightTeamData_t
{
	uint8_t teamIndex[HIGHLIGHT_TEAM_MAX_SLOTS];
	uint32_t teamBits[HIGHLIGHT_TEAM_MAX_SLOTS];

	EntityHighlightTeamData_t()
	{
		memset(teamIndex, 0xFF, sizeof(teamIndex));
		memset(teamBits, 0, sizeof(teamBits));
	}
};

// Sibling maps per side (entity natives are registered on both VMs via
// WeaponScriptVars_RegisterEntityFuncs at vscript_client.cpp:2261 +
// vscript_server.cpp:1107).
static SDKEntityMap<EntityHighlightTeamData_t> s_entityHighlightTeamsServer(
	ESide::Server, "highlightTeams.srv");
static SDKEntityMap<EntityHighlightTeamData_t> s_entityHighlightTeamsClient(
	ESide::Client, "highlightTeams.cli");

static SDKEntityMap<EntityHighlightTeamData_t>& GetHighlightTeamsMap(HSQUIRRELVM v)
{
	return (v && v->GetContext() == SQCONTEXT::SERVER) ? s_entityHighlightTeamsServer
	                                                   : s_entityHighlightTeamsClient;
}

static SQRESULT Script_HighlightEnableForTeam(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	SQInteger contextId, team;
	sq_getinteger(v, 2, &contextId);
	sq_getinteger(v, 3, &team);

	if (contextId < 0 || contextId > 254 || team < 0 || team > 31)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	auto& data = GetHighlightTeamsMap(v)[pEntity];

	// Find existing slot for this context, or allocate free slot
	int slot = -1;
	for (int i = 0; i < HIGHLIGHT_TEAM_MAX_SLOTS; i++)
	{
		if (data.teamIndex[i] == static_cast<uint8_t>(contextId))
		{
			slot = i;
			break;
		}
	}

	if (slot < 0)
	{
		for (int i = 0; i < HIGHLIGHT_TEAM_MAX_SLOTS; i++)
		{
			if (data.teamIndex[i] == 0xFF)
			{
				slot = i;
				break;
			}
		}
	}

	if (slot < 0)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	data.teamBits[slot] |= (1u << static_cast<int>(team));
	data.teamIndex[slot] = static_cast<uint8_t>(contextId);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_HighlightDisableForTeam(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	SQInteger contextId, team;
	sq_getinteger(v, 2, &contextId);
	sq_getinteger(v, 3, &team);

	if (contextId < 0 || contextId > 254 || team < 0 || team > 31)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	EntityHighlightTeamData_t* pData = GetHighlightTeamsMap(v).Find(pEntity);
	if (!pData)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	auto& data = *pData;
	for (int i = 0; i < HIGHLIGHT_TEAM_MAX_SLOTS; i++)
	{
		if (data.teamIndex[i] == static_cast<uint8_t>(contextId))
		{
			data.teamBits[i] &= ~(1u << static_cast<int>(team));
			if (data.teamBits[i] == 0)
				data.teamIndex[i] = 0xFF;
			break;
		}
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsHighlightEnabledForTeam(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	SQInteger contextId, team;
	sq_getinteger(v, 2, &contextId);
	sq_getinteger(v, 3, &team);

	bool enabled = false;
	const EntityHighlightTeamData_t* pData = GetHighlightTeamsMap(v).Find(pEntity);
	if (pData && contextId >= 0 && contextId <= 254 && team >= 0 && team <= 31)
	{
		for (int i = 0; i < HIGHLIGHT_TEAM_MAX_SLOTS; i++)
		{
			if (pData->teamIndex[i] == static_cast<uint8_t>(contextId))
			{
				enabled = (pData->teamBits[i] & (1u << static_cast<int>(team))) != 0;
				break;
			}
		}
	}

	sq_pushbool(v, enabled);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Highlight_OverrideParam / Highlight_ClearOverrideParam
//-----------------------------------------------------------------------------
static constexpr int MAX_HIGHLIGHT_OVERRIDE_PARAMS = 4;

struct EntityHighlightOverride_t
{
	bool isOverriden[MAX_HIGHLIGHT_OVERRIDE_PARAMS] = {};
	float overrideParams[MAX_HIGHLIGHT_OVERRIDE_PARAMS][3] = {};
};

static SDKEntityMap<EntityHighlightOverride_t> s_entityHighlightOverridesServer(
	ESide::Server, "highlightOverrides.srv");
static SDKEntityMap<EntityHighlightOverride_t> s_entityHighlightOverridesClient(
	ESide::Client, "highlightOverrides.cli");

static SDKEntityMap<EntityHighlightOverride_t>& GetHighlightOverridesMap(HSQUIRRELVM v)
{
	return (v && v->GetContext() == SQCONTEXT::SERVER) ? s_entityHighlightOverridesServer
	                                                   : s_entityHighlightOverridesClient;
}

static SQRESULT Script_Highlight_OverrideParam(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	SQInteger paramId;
	sq_getinteger(v, 2, &paramId);

	const SQVector3D* value = nullptr;
	sq_getvector(v, 3, &value);

	if (paramId >= 0 && paramId < MAX_HIGHLIGHT_OVERRIDE_PARAMS && value)
	{
		auto& data = GetHighlightOverridesMap(v)[pEntity];
		data.isOverriden[paramId] = true;
		data.overrideParams[paramId][0] = value->x;
		data.overrideParams[paramId][1] = value->y;
		data.overrideParams[paramId][2] = value->z;
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Highlight_ClearOverrideParam(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	SQInteger paramId;
	sq_getinteger(v, 2, &paramId);

	if (paramId >= 0 && paramId < MAX_HIGHLIGHT_OVERRIDE_PARAMS)
	{
		EntityHighlightOverride_t* pData = GetHighlightOverridesMap(v).Find(pEntity);
		if (pData)
			pData->isOverriden[paramId] = false;
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Generic highlight context per-entity (4 slots)
//-----------------------------------------------------------------------------
static constexpr int MAX_GENERIC_HIGHLIGHT_TYPES = 4;

struct EntityGenericHighlight_t
{
	uint8_t contexts[MAX_GENERIC_HIGHLIGHT_TYPES] = { 0xFF, 0xFF, 0xFF, 0xFF };
	uint8_t focusedBits = 0;
};

static SDKEntityMap<EntityGenericHighlight_t> s_entityGenericHighlightsServer(
	ESide::Server, "genericHighlights.srv");
static SDKEntityMap<EntityGenericHighlight_t> s_entityGenericHighlightsClient(
	ESide::Client, "genericHighlights.cli");

static SDKEntityMap<EntityGenericHighlight_t>& GetGenericHighlightsMap(HSQUIRRELVM v)
{
	return (v && v->GetContext() == SQCONTEXT::SERVER) ? s_entityGenericHighlightsServer
	                                                   : s_entityGenericHighlightsClient;
}

static SQRESULT Script_Highlight_SetGenericHighlightContext(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	SQInteger genericType, contextId;
	SQBool focused;
	sq_getinteger(v, 2, &genericType);
	sq_getinteger(v, 3, &contextId);
	sq_getbool(v, 4, &focused);

	if (!pEntity || genericType < 0 || genericType >= MAX_GENERIC_HIGHLIGHT_TYPES)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	auto& data = GetGenericHighlightsMap(v)[pEntity];

	// Store -1 (HIGHLIGHT_INVALID_ID) as 0xFF internally
	if (contextId < 0 || contextId > 0xFE)
		data.contexts[genericType] = 0xFF;
	else
		data.contexts[genericType] = static_cast<uint8_t>(contextId);

	if (focused)
		data.focusedBits |= (1 << genericType);
	else
		data.focusedBits &= ~(1 << genericType);

	// HighlightSettings SendProxy drops the table unless bit 0x8000 at +0xD8 is set.
	*reinterpret_cast<int*>(reinterpret_cast<char*>(pEntity) + 0xD8) |= 0x8000;
	MarkEntityEdictDirty(pEntity);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

int WeaponScriptVars_WireGetGenericHighlightContext(void* pEntity, int genericType)
{
	if (!pEntity || genericType < 0 || genericType >= MAX_GENERIC_HIGHLIGHT_TYPES)
		return 0xFF;
	const EntityGenericHighlight_t* pData = s_entityGenericHighlightsServer.Find(pEntity);
	if (!pData)
		return 0xFF;
	return static_cast<int>(pData->contexts[genericType]);
}

int WeaponScriptVars_WireGetHighlightFocused(void* pEntity)
{
	if (!pEntity)
		return 0;
	const EntityGenericHighlight_t* pData = s_entityGenericHighlightsServer.Find(pEntity);
	return pData ? static_cast<int>(pData->focusedBits) : 0;
}

static SQRESULT Script_Highlight_GetGenericHighlightContext(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	SQInteger genericType;
	sq_getinteger(v, 2, &genericType);

	int result = -1; // HIGHLIGHT_INVALID_ID -- no context set
	if (genericType >= 0 && genericType < MAX_GENERIC_HIGHLIGHT_TYPES)
	{
		const EntityGenericHighlight_t* pData = GetGenericHighlightsMap(v).Find(pEntity);
		if (pData)
		{
			uint8_t raw = pData->contexts[genericType];
			result = (raw == 0xFF) ? -1 : static_cast<int>(raw);
		}
	}

	sq_pushinteger(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// GetUsableValue: server entity+0x624, client entity+0x44 (m_usableType).
static constexpr int ENTITY_USABLETYPE_CLIENT_OFFSET = 0x44;
static constexpr int ENTITY_USABLETYPE_SERVER_OFFSET = 0x624;

static SQRESULT Script_GetUsableValue(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	int val = *(int*)((uintptr_t)pEntity + ENTITY_USABLETYPE_SERVER_OFFSET);

	sq_pushinteger(v, val);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// SetCylinderRadius - alias for engine's SetRadius
//-----------------------------------------------------------------------------
static SQRESULT Script_SetCylinderRadius(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	// Accept both int and float (scripts pass either)
	SQFloat radius = 0.0f;
	if (SQ_FAILED(sq_getfloat(v, 2, &radius)))
	{
		SQInteger iRadius;
		if (SQ_FAILED(sq_getinteger(v, 2, &iRadius)))
			return SQ_ERROR;
		radius = static_cast<SQFloat>(iRadius);
	}

	if (v_CBaseEntity_SetRadius && pEntity)
		v_CBaseEntity_SetRadius(pEntity, static_cast<float>(radius));

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// SetLaunchDelay - gravity-cannon charge window, in seconds
//-----------------------------------------------------------------------------
static SQRESULT Script_SetLaunchDelay(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	SQFloat delay = 0.0f;
	if (SQ_FAILED(sq_getfloat(v, 2, &delay)))
	{
		SQInteger iDelay;
		if (SQ_FAILED(sq_getinteger(v, 2, &iDelay)))
			return SQ_ERROR;
		delay = static_cast<SQFloat>(iDelay);
	}

	TriggerCannon_SetLaunchDelay(pEntity, static_cast<float>(delay));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// GravityCannonIsPreparingLaunch - true while the charge window is open
//-----------------------------------------------------------------------------
static SQRESULT Script_GravityCannonIsPreparingLaunch(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	sq_pushbool(v, TriggerCannon_IsPreparingLaunch(pEntity) ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// SetLaunchTargetLocation - solve launchDir/power from a landing point
//-----------------------------------------------------------------------------
static SQRESULT Script_SetLaunchTargetLocation(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	const SQVector3D* pTarget = nullptr;
	if (SQ_FAILED(sq_getvector(v, 2, &pTarget)) || !pTarget)
		return SQ_ERROR;

	const float target[3] = {
		static_cast<float>(pTarget->x),
		static_cast<float>(pTarget->y),
		static_cast<float>(pTarget->z)
	};
	TriggerCannon_SetLaunchTargetLocation(pEntity, target);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// GetLaunchDir - return m_launchDir as a script vector
//-----------------------------------------------------------------------------
static SQRESULT Script_GetLaunchDir(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	float dir[3] = { 0.0f, 0.0f, 0.0f };
	TriggerCannon_GetLaunchDir(pEntity, dir);

	const SQVector3D result(
		static_cast<SQFloat>(dir[0]),
		static_cast<SQFloat>(dir[1]),
		static_cast<SQFloat>(dir[2]));

	sq_pushvector(v, &result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static bool Script_GetFloatOrInt(HSQUIRRELVM v, int nIdx, float* pOut);

static bool Script_GetBoolOrInt(HSQUIRRELVM v, int nIdx, bool* pOut)
{
	SQBool bVal = SQFalse;
	if (SQ_SUCCEEDED(sq_getbool(v, nIdx, &bVal)))
	{
		*pOut = bVal != SQFalse;
		return true;
	}

	SQInteger iVal = 0;
	if (SQ_SUCCEEDED(sq_getinteger(v, nIdx, &iVal)))
	{
		*pOut = iVal != 0;
		return true;
	}

	return false;
}

static SQRESULT Script_SetEnableDoubleJump(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	bool bEnable = false;
	if (!Script_GetBoolOrInt(v, 2, &bEnable))
		return SQ_ERROR;

	TriggerCannon_SetEnableDoubleJump(pEntity, bEnable);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetLimitedAirControl(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	bool bLimited = false;
	if (!Script_GetBoolOrInt(v, 2, &bLimited))
		return SQ_ERROR;

	TriggerCannon_SetLimitedAirControl(pEntity, bLimited);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetLaunchAirControlParams(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	float flSpeed = 0.0f;
	float flAccel = 0.0f;
	if (!Script_GetFloatOrInt(v, 2, &flSpeed) || !Script_GetFloatOrInt(v, 3, &flAccel))
		return SQ_ERROR;

	TriggerCannon_SetLaunchAirControlParams(pEntity, flSpeed, flAccel);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// S21 passes int literals into float slots (SetBlackholeParams(200,...)).
static bool Script_GetFloatOrInt(HSQUIRRELVM v, int nIdx, float* pOut)
{
	SQFloat fl = 0.0f;
	if (SQ_SUCCEEDED(sq_getfloat(v, nIdx, &fl)))
	{
		*pOut = static_cast<float>(fl);
		return true;
	}

	SQInteger iVal;
	if (SQ_SUCCEEDED(sq_getinteger(v, nIdx, &iVal)))
	{
		*pOut = static_cast<float>(iVal);
		return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// SetGravityLiftParams - 10 floats for TT_GRAVITY_LIFT authoring
//-----------------------------------------------------------------------------
static SQRESULT Script_SetGravityLiftParams(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	float params[10] = {};
	for (int i = 0; i < 10; ++i)
	{
		if (!Script_GetFloatOrInt(v, 2 + i, &params[i]))
			return SQ_ERROR;
	}

	TriggerGravity_SetGravityLiftParams(pEntity, params);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// SetBlackholeParams - 6 floats for TT_BLACKHOLE authoring
//-----------------------------------------------------------------------------
static SQRESULT Script_SetBlackholeParams(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	float params[6] = {};
	for (int i = 0; i < 6; ++i)
	{
		if (!Script_GetFloatOrInt(v, 2 + i, &params[i]))
			return SQ_ERROR;
	}

	TriggerGravity_SetBlackholeParams(pEntity, params);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// SetBlackholeIsStrongPulling - bool
//-----------------------------------------------------------------------------
static SQRESULT Script_SetBlackholeIsStrongPulling(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	SQBool bStrong = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bStrong)))
	{
		// Accept int 0/1 the way S21 sometimes passes it.
		SQInteger iStrong = 0;
		if (SQ_FAILED(sq_getinteger(v, 2, &iStrong)))
			return SQ_ERROR;
		bStrong = iStrong ? SQTrue : SQFalse;
	}

	TriggerGravity_SetBlackholeIsStrongPulling(pEntity, bStrong != SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// EnableTouchAutoUse - stub (auto-pickup system, server-only)
//-----------------------------------------------------------------------------
static SQRESULT Script_EnableTouchAutoUse(HSQUIRRELVM v)
{
	Warning(eDLL_T::SERVER, "EnableTouchAutoUse called - stub\n");
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Walk move-child/peer EHANDLEs. Client +0x11C/+0x120; server use baseentity.h m_hMoveChild/m_hMovePeer.
// Push via real HSQOBJECT / PushEntity; serial-validate handles before GetScriptInstance.
static constexpr int ENTITY_CLIENT_MOVECHILD_OFFSET = 0x11C;
static constexpr int ENTITY_CLIENT_MOVEPEER_OFFSET  = 0x120;
static constexpr int MAX_CHILDREN_SAFETY     = 512;

// When 1, skip GetScriptInstance on entries that fail preflight (entry 0xFFFF,
// OOB, serial mismatch, entIndex mismatch, odd script-instance field).
static ConVar bridge_getchildren_preflight(
	"bridge_getchildren_preflight", "1", FCVAR_RELEASE,
	"Skip GetScriptInstance on GetChildren entries that fail preflight");

// Accessor adds no members; compiler offsets come from baseentity.h, not a typed hex guess.
class Script_GetChildren_ServerFieldAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_hMoveChild;
	using CBaseEntity::m_hMovePeer;
	using CBaseEntity::m_entIndex;
	using CBaseEntity::m_hScriptInstance;
};

// Serial-validated resolve. Stale handles (serial mismatch / OOB / invalid)
// return null so the walk stops instead of calling GetScriptInstance on junk.
static void* Script_GetChildren_LookupServerEntity(const uint32_t rawHandle, int* outEntry)
{
	const int entry = static_cast<int>(rawHandle & ENT_ENTRY_MASK);
	const int serial = static_cast<int>(rawHandle >> NUM_SERIAL_NUM_SHIFT_BITS);
	if (outEntry)
		*outEntry = entry;

	if (rawHandle == INVALID_EHANDLE_INDEX)
		return nullptr;
	if (!g_serverEntityList)
		return nullptr;
	if (entry < 0 || entry >= NUM_ENT_ENTRIES)
		return nullptr;
	// 0xFFFF / last-slot class that produced the in-GetScriptInstance AV.
	if (entry == static_cast<int>(ENT_ENTRY_MASK) || entry == (NUM_ENT_ENTRIES - 1))
		return nullptr;

	const CBaseHandle base = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
	if (void* pEntity = g_serverEntityList->LookupEntity(base))
		return pEntity;

	// Serial-validated network-index fallback only (never ignore serial).
	const CEntInfo* info = g_serverEntityList->GetEntInfoPtrByIndex(entry);
	void* pByIdx = g_serverEntityList->LookupEntityByNetworkIndex(entry);
	if (pByIdx && info && info->m_SerialNumber == serial)
		return pByIdx;

	return nullptr;
}

static SQRESULT Script_GetChildren(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return SQ_ERROR;

	sq_newarray(v, 0);

	if (!pEntity)
		return 1;

	// Server / dedicated: walk CBaseEntity::m_hMoveChild -> m_hMovePeer via
	// the compiler-computed field accessor. Push each child by dereferencing
	// GetScriptInstance's HSCRIPT as the real 16-byte HSQOBJECT it points to.
	const bool preflight = bridge_getchildren_preflight.GetBool();

	Script_GetChildren_ServerFieldAccess* const pParent =
		static_cast<Script_GetChildren_ServerFieldAccess*>(
			reinterpret_cast<CBaseEntity*>(pEntity));

	uint32_t childHandle = static_cast<uint32_t>(pParent->m_hMoveChild.ToInt());
	int safety = 0;
	uint32_t seen[16];
	int nSeen = 0;

	while (childHandle != INVALID_EHANDLE_INDEX && safety < MAX_CHILDREN_SAFETY)
	{
		// Cycle detection (short ring buffer).
		bool cycle = false;
		for (int i = 0; i < nSeen; ++i)
		{
			if (seen[i] == childHandle)
			{
				cycle = true;
				break;
			}
		}
		if (nSeen < static_cast<int>(sizeof(seen) / sizeof(seen[0])))
			seen[nSeen++] = childHandle;

		int lookEntry = -1;
		void* const pChild = cycle
			? nullptr
			: Script_GetChildren_LookupServerEntity(childHandle, &lookEntry);

		if (!pChild)
			break;

		CBaseEntity* const pChildEnt = reinterpret_cast<CBaseEntity*>(pChild);
		auto* const pChildAccess =
			static_cast<Script_GetChildren_ServerFieldAccess*>(pChildEnt);

		// Preflight: entity index / ref-handle must agree with the walk entry;
		// odd m_hScriptInstance is the in-GetScriptInstance AV class.
		const uint32_t childRefH = *reinterpret_cast<const uint32_t*>(
			reinterpret_cast<const uint8_t*>(pChild) + 0x8);
		const int childEntIdx = pChildAccess->m_entIndex;
		const bool idxMismatch = (childEntIdx != lookEntry);
		const bool refEntryMismatch =
			((static_cast<int>(childRefH & ENT_ENTRY_MASK) != lookEntry) &&
			 childRefH != INVALID_EHANDLE_INDEX);
		const uint64_t scriptInstRaw = *reinterpret_cast<const uint64_t*>(
			pChildAccess->m_hScriptInstance);
		const bool scriptInstOdd = ((scriptInstRaw & 1ull) != 0);
		const bool blockPush = idxMismatch || refEntryMismatch || scriptInstOdd;

		if (v_CBaseEntity__GetScriptInstance && !(preflight && blockPush))
		{
			// Null m_hScriptInstance is normal (e.g. phys_bone_follower);
			// engine GetScriptInstance may create on demand.
			const HSCRIPT scriptHandle = v_CBaseEntity__GetScriptInstance(pChildEnt);
			if (scriptHandle)
			{
				const HSQOBJECT* const pObj =
					reinterpret_cast<const HSQOBJECT*>(scriptHandle);
				sq_pushobject(v, *pObj);
				sq_arrayappend(v, -2);
			}
		}

		childHandle = static_cast<uint32_t>(pChildAccess->m_hMovePeer.ToInt());
		safety++;
	}


	return 1;
}

// Phase-shift type is a side-local annotation on PhaseShiftBegin; each VM writes/reads its own map.
static SQRESULT Script_GetPhaseShiftType(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	sq_pushinteger(v, BCCExtend_GetI32(pEntity, offsetof(BCCExtendWire, m_phaseShiftType)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// PhaseShiftBegin override: accepts 3rd param (phaseShiftType), stores it,
// then calls engine's native 2-param PhaseShiftBegin for actual mechanics.
static SQRESULT Script_PhaseShiftBegin_Override(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return SQ_ERROR;

	SQFloat warmup, duration;
	sq_getfloat(v, 2, &warmup);
	sq_getfloat(v, 3, &duration);

	// 3rd param is optional for backwards compat
	SQInteger phaseType = 0;
	if (sq_gettop(v) >= 4)
		sq_getinteger(v, 4, &phaseType);

	if (phaseType < 0 || phaseType > 1023)
		phaseType = 0;

	BCCExtend_SetI32(pEntity, offsetof(BCCExtendWire, m_phaseShiftType), static_cast<int>(phaseType));
	const bool isServerVMForDirty = (v && v->GetContext() == SQCONTEXT::SERVER);
	if (isServerVMForDirty)
		MarkEntityEdictDirty(pEntity);

	// Server PhaseShiftBegin at +0x15B4/+0x15B8, client at +0x1790/+0x1794. Dispatch by VM or times stay 0.
	const bool isServerVM = (v && v->GetContext() == SQCONTEXT::SERVER);
	auto* const pNative = isServerVM ? v_PhaseShiftBegin_Server
	                                 : v_PhaseShiftBegin_Client;
	if (pNative)
	{
		pNative(pEntity, static_cast<float>(warmup), static_cast<float>(duration));
	}
	else
	{
		Warning(eDLL_T::COMMON,
			"PhaseShiftBegin: %s native unresolved — phase shift will not start. "
			"Pattern likely changed in this build.\n",
			isServerVM ? "server" : "client");
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void WeaponScriptVars_PhaseShift_LevelShutdown()
{
}

// Appended to the base-combat-character descriptor, right after the engine's
// own 2-param registration of the same name -- the later entry wins, and the
// class stays the one the engine used, so NPC call sites keep working.
void WeaponScriptVars_RegisterPhaseShiftOverride(ScriptClassDescriptor_t* combatCharStruct)
{
	if (!combatCharStruct)
	{
		Warning(eDLL_T::SERVER,
			"[PHASESHIFT-REG] combat character script class descriptor is null; "
			"PhaseShiftBegin override not registered\n");
		return;
	}

	combatCharStruct->AddFunction(
		"PhaseShiftBegin",
		"Script_PhaseShiftBegin_Override",
		"Begins phase shift with warmup, duration, and type",
		"void",
		"float warmupDuration, float duration, int phaseShiftType",
		false,
		Script_PhaseShiftBegin_Override);
}

// Reads laserSightColor / laserSightColorCustomized userinfo and writes the DT_Player fields.

// Zero-layout accessor for CBaseEntity::m_entIndex (protected) -- same
// pattern as Script_GetChildren_ServerFieldAccess above.
class Script_LaserSight_ServerFieldAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_entIndex;
};

// laserSightColor is an int packed RGB decimal string, not "R G B" text.
// Think re-asserts both fields every tick; the one-shot native alone goes stale.
static bool LaserSightColor_SyncFromUserInfo(void* pPlayer, int clientIndex)
{
	bool changed = false;

	const char* colorStr = g_pEngineServer->GetClientConVarValue(clientIndex, "laserSightColor");
	if (colorStr && colorStr[0])
	{
		const int packed = static_cast<int>(strtol(colorStr, nullptr, 10));
		const float rgb[3] = {
			static_cast<float>(packed & 0xFF),
			static_cast<float>((packed >> 8) & 0xFF),
			static_cast<float>((packed >> 16) & 0xFF),
		};

		PlayerExtendBundle bundle;
		const bool has = PlayerExtend_GetBundle(pPlayer, &bundle);
		if (!has
			|| bundle.player.m_laserSightColor[0] != rgb[0]
			|| bundle.player.m_laserSightColor[1] != rgb[1]
			|| bundle.player.m_laserSightColor[2] != rgb[2])
		{
			PlayerExtend_SetVec(pPlayer, offsetof(PlayerExtendWire, m_laserSightColor), rgb);
			changed = true;
		}
	}

	const char* customizedStr = g_pEngineServer->GetClientConVarValue(clientIndex, "laserSightColorCustomized");
	if (customizedStr && customizedStr[0])
	{
		const int customized = atoi(customizedStr);
		if (PlayerExtend_GetI32(pPlayer, offsetof(PlayerExtendWire, m_laserSightColorCustomized)) != customized)
		{
			PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_laserSightColorCustomized), customized);
			changed = true;
		}
	}

	return changed;
}

static SQRESULT Script_UpdateLaserSightColor(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	const int clientIndex = static_cast<Script_LaserSight_ServerFieldAccess*>(
		reinterpret_cast<CBaseEntity*>(pPlayer))->m_entIndex;

	if (LaserSightColor_SyncFromUserInfo(pPlayer, clientIndex))
		MarkEntityEdictDirty(pPlayer);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetTurret(HSQUIRRELVM v);

void WeaponScriptVars_RegisterLaserSightOverride(ScriptClassDescriptor_t* playerStruct)
{
	playerStruct->AddFunction(
		"UpdateLaserSightColor",
		"Script_UpdateLaserSightColor",
		"Applies this player's laserSightColor/laserSightColorCustomized userinfo ConVars to the networked entity fields",
		"void",
		"",
		false,
		Script_UpdateLaserSightColor);

	playerStruct->AddFunction(
		"GetTurret",
		"Script_GetTurret",
		"Returns the turret this player is driving, or null",
		"entity",
		"",
		false,
		Script_GetTurret);
}

// Per-tick userinfo sync into DT_Player; the one-shot native is not enough.
static uint8_t s_laserSyncPhase[2048] = {};

void LaserSightColorBridge_Think(void* pPlayer)
{
	if (!pPlayer)
		return;

	const int clientIndex = static_cast<Script_LaserSight_ServerFieldAccess*>(
		reinterpret_cast<CBaseEntity*>(pPlayer))->m_entIndex;

	if (clientIndex >= 0 && clientIndex < 2048)
	{
		if ((s_laserSyncPhase[clientIndex]++ & 3) != 0)
			return;
	}

	const bool bDiag = bridge_laser_diag.GetBool();

	if (bDiag)
	{
		static bool s_bArmed = false;
		if (!s_bArmed)
		{
			s_bArmed = true;
			Msg(eDLL_T::SERVER, "[LASER-DIAG] laser-colour think armed: sidecar client=%d\n",
				clientIndex);
		}
	}

	if (LaserSightColor_SyncFromUserInfo(pPlayer, clientIndex))
	{
		MarkEntityEdictDirty(pPlayer);

		// Change-triggered only (the sync self-heals to a steady state), so this
		// fires on the settle edges rather than every tick.
		if (bDiag)
			Msg(eDLL_T::SERVER, "[LASER-DIAG] laser colour synced from userinfo (client=%d)\n", clientIndex);
	}
}

// Slots 0..5 tail-call the native; 6/7 go through OffhandSlotsExt. Do not write engine offhandWeapons[6/7].
// PushEntity only: GetScriptInstance is an HScriptHandle, not an SQInstance (hand-roll UAF at +0x50).
static void OffhandOverride_PushEntityOrNull(HSQUIRRELVM v, void* pEntity)
{
	// Engine helper accepts null and pushes the canonical SQ null object in
	// that case -- we don't need a separate nullptr branch.
	const bool isServerVM = (v && v->GetContext() == SQCONTEXT::SERVER);
	auto* const pushFn = isServerVM
		? v_CSquirrelVM_PushEntity_Server
		: v_CSquirrelVM_PushEntity_Client;
	if (pushFn)
	{
		pushFn(v, pEntity);
		return;
	}
	// Pattern resolution failed (different build?). Fall back to a safe null
	// rather than an unsafe hand-rolled SQObject -- the script sees null just
	// like it would when the engine couldn't resolve the entity.
	Warning(isServerVM ? eDLL_T::SERVER : eDLL_T::CLIENT,
		"OffhandOverride_PushEntityOrNull: CSquirrelVM::PushEntity_%s "
		"unresolved; returning null. Check WeaponScriptVars pattern set.\n",
		isServerVM ? "Server" : "Client");
	sq_pushnull(v);
}

// CPlayer::offhandWeapons[0] -- EHandle array base (S3 server).
static constexpr uintptr_t s_offhandWeaponsBase = 5812;
// CPlayer::m_selectedOffhands[0] -- active inventory slot -> offhand index (S3 server).
static constexpr uintptr_t s_selectedOffhandsBase = 5910;

// Resolve without touching the VM stack. 0..5 read the engine EHandle array; 6/7 use the SDK shadow.
static void* OffhandOverride_ResolveOffhandEntityServer(void* pPlayer, int slot, HSQUIRRELVM v)
{
	if (!pPlayer || slot < 0 || slot > kOffhandSlotExtLast)
		return nullptr;

	if (OffhandSlotsExt_IsExtendedSlot(slot))
		return OffhandSlotsExt_GetEntity(pPlayer, slot, v);

	__try
	{
		const uint32_t eh = *reinterpret_cast<uint32_t*>(
			reinterpret_cast<uintptr_t>(pPlayer) + s_offhandWeaponsBase
			+ 4ull * static_cast<uintptr_t>(slot));
		if (eh == kOffhandSlotExtInvalidHandle)
			return nullptr;
		return SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

// Script signature: entity GetOffhandWeapon(int offhandIndex)
// Arg layout on the VM stack: [this, slot] (slot at idx 2).
static SQRESULT Script_GetOffhandWeapon_Override(HSQUIRRELVM v)
{
	SQInteger slot = 0;
	sq_getinteger(v, 2, &slot);

	// Out-of-range: push null rather than SQ_ERROR to match the native's
	// "range check fails silently to null" contract in S21. Scripts
	// branch on `if (weapon != null)` and this keeps that working.
	if (slot < 0 || slot > kOffhandSlotExtLast)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const bool isServerVM = (v && v->GetContext() == SQCONTEXT::SERVER);

	// S21 client C_BCC already has eight offhand slots at +0x1798. The SDK
	// extension (S3 +0x16B4 shadow) is dedi-only. Client slot 6/7 must use
	// the S21 client native or ActivateEmoteProjector sees null and never selects.
	if (slot < kOffhandSlotExtFirst || !isServerVM)
	{
		auto* const pNative = isServerVM ? v_Script_GetOffhandWeapon_Server
		                                 : v_Script_GetOffhandWeapon_Client;
		if (pNative)
			return pNative(v);

		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// Slot 6/7 -> SDK extension.
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* pWeapon = OffhandSlotsExt_GetEntity(pPlayer, static_cast<int>(slot), v);
	OffhandOverride_PushEntityOrNull(v, pWeapon);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Give/Take writes player+0x16B4[slot]; input reads WeaponInventory+0x2C. Mirror after native Give/Take.
static void* OffhandInv_GetWeaponInventoryPtr(void* pPlayer)
{
	if (!pPlayer)
		return nullptr;

	// Virtual call through the player's own vtable at +2648 (331st 8-byte
	// slot) returns the WeaponInventory* sub-object. No named interface for
	// this slot yet; called raw by fixed offset.
	void* const vtable = *reinterpret_cast<void**>(pPlayer);
	using GetInventoryFn = void* (__fastcall*)(void*);
	auto const pGetInventory = *reinterpret_cast<GetInventoryFn*>(
		reinterpret_cast<uint8_t*>(vtable) + 2648);
	return pGetInventory(pPlayer);
}

static void OffhandInv_MirrorArraySlot(void* pPlayer, int slot)
{
	if (!pPlayer || slot < 0 || slot >= 6)
		return;

	void* const pInventory = OffhandInv_GetWeaponInventoryPtr(pPlayer);
	if (!pInventory)
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-MIRROR] WeaponInventory resolve failed for player=%p "
			"slot=%d -- native input-driven activation will NOT see this "
			"give/take.\n", pPlayer, slot);
		return;
	}

	const uint32_t authoritative = *reinterpret_cast<uint32_t*>(
		reinterpret_cast<uintptr_t>(pPlayer) + 4 * slot + 0x16B4);
	uint32_t* const pDispatchSlot = reinterpret_cast<uint32_t*>(
		reinterpret_cast<uintptr_t>(pInventory) + 4 * slot + 44);

	if (*pDispatchSlot != authoritative)
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-MIRROR] player=%p slot=%d WeaponInventory+0x2C: "
			"0x%08X -> 0x%08X\n",
			pPlayer, slot, *pDispatchSlot, authoritative);
		*pDispatchSlot = authoritative;
	}
}

// GiveOffhandWeapon is server-VM-only. A client-VM call pushes null instead of the server native.
static SQRESULT Script_GiveOffhandWeapon_Override(HSQUIRRELVM v)
{
	if (v && v->GetContext() != SQCONTEXT::SERVER)
	{
		// Shouldn't happen (not registered on client struct), but guard
		// against future drift -- calling the server native with a client v
		// would read wrong-side DT state.
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	SQInteger slot = 0;
	sq_getinteger(v, 3, &slot);

	if (slot < 0 || slot > kOffhandSlotExtLast)
	{
		v_SQVM_ScriptError(
			"GiveOffhandWeapon: slot %d out of range [0, %d]",
			static_cast<int>(slot), kOffhandSlotExtLast);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// returnType is void; slot 6/7 also push nothing. Follow up with GetOffhandWeapon(slot) if needed.
	if (slot < kOffhandSlotExtFirst)
	{
		if (v_Script_GiveOffhandWeapon_Server)
		{
			const SQChar* pszWeapName = nullptr;
			sq_getstring(v, 2, &pszWeapName);

			void* pDiagPlayer = nullptr;
			v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pDiagPlayer));
			uint32_t slotBefore = pDiagPlayer
				? *reinterpret_cast<uint32_t*>(
					reinterpret_cast<uintptr_t>(pDiagPlayer) + 4 * slot + 0x16B4)
				: 0xDEAD;

			SQRESULT r = v_Script_GiveOffhandWeapon_Server(v);

			uint32_t slotAfter = pDiagPlayer
				? *reinterpret_cast<uint32_t*>(
					reinterpret_cast<uintptr_t>(pDiagPlayer) + 4 * slot + 0x16B4)
				: 0xDEAD;

			// Mirror into WeaponInventory+0x2C so input activation sees this weapon.
			if (pDiagPlayer && slotBefore != slotAfter)
				OffhandInv_MirrorArraySlot(pDiagPlayer, static_cast<int>(slot));

			return r;
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// Slot 6/7: route through OffhandSlotsExt_Give.
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	const SQChar* pszName = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &pszName)) || !pszName)
	{
		v_SQVM_ScriptError("GiveOffhandWeapon: weapon name is required");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// Mods bitfield: server copies only (client twins exist). Compute only when arg 4 is OT_ARRAY.
	uint32_t modsBitfield = 0;
	if (sq_gettop(v) >= 4 && sq_isarray(stack_get(v, 4))
		&& v_OffhandWeaponNameToDef && v_OffhandComputeModsBitfield)
	{
		void* const weaponDef = v_OffhandWeaponNameToDef(pszName);
		uint32_t bf = 0;
		if (weaponDef && v_OffhandComputeModsBitfield(v, 4, weaponDef, &bf))
			modsBitfield = bf;
		else
			Warning(eDLL_T::SERVER,
				"GiveOffhandWeapon: slot %d failed to compute mods bitfield for "
				"'%s' (def=%p); giving with no mods.\n",
				static_cast<int>(slot), pszName, weaponDef);
	}

	OffhandSlotsExt_Give(pPlayer, static_cast<int>(slot),
		pszName, modsBitfield, v);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Script signature: void TakeOffhandWeapon(int offhandIndex)
// Arg layout: [this, slot] (slot at idx 2).
// SERVER VM ONLY -- see note on GiveOffhandWeapon_Override.
static SQRESULT Script_TakeOffhandWeapon_Override(HSQUIRRELVM v)
{
	if (v && v->GetContext() != SQCONTEXT::SERVER)
	{
		// Client/UI VM should never reach here (we don't register this name
		// on those structs), but defend against future registration drift.
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	SQInteger slot = 0;
	sq_getinteger(v, 2, &slot);

	if (slot < 0 || slot > kOffhandSlotExtLast)
	{
		v_SQVM_ScriptError(
			"TakeOffhandWeapon: slot %d out of range [0, %d]",
			static_cast<int>(slot), kOffhandSlotExtLast);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (slot < kOffhandSlotExtFirst)
	{
		if (v_Script_TakeOffhandWeapon_Server)
		{
			void* pDiagPlayer = nullptr;
			v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pDiagPlayer));

			const SQRESULT r = v_Script_TakeOffhandWeapon_Server(v);

			// Mirror the clear into WeaponInventory+0x2C.
			if (pDiagPlayer)
				OffhandInv_MirrorArraySlot(pDiagPlayer, static_cast<int>(slot));

			return r;
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	OffhandSlotsExt_Take(pPlayer, static_cast<int>(slot), v);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Script signature: void CancelOffhandWeapon(int offhandIndex)
static SQRESULT Script_CancelOffhandWeapon(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SQInteger offhandIndex = 0;
	sq_getinteger(v, 2, &offhandIndex);

	if (offhandIndex < 0 || offhandIndex > kOffhandSlotExtLast)
	{
		Warning(eDLL_T::SERVER,
			"CancelOffhandWeapon(): Invalid offhand index [%d].  Range is [0, %d]\n",
			static_cast<int>(offhandIndex), kOffhandSlotExtLast);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* const pWeapon = OffhandOverride_ResolveOffhandEntityServer(
		pPlayer, static_cast<int>(offhandIndex), v);
	if (!pWeapon)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	if (v_WeaponX_HolsterInternal)
		v_WeaponX_HolsterInternal(pWeapon, /*bDoFastHolster=*/true);

	// Slots 6/7 never land in m_selectedOffhands (engine selection scans 0..5 only).
	// Do not write m_selectedOffhands -- Weapon_SetSelectedOffhandCleared owns that
	// write and the entity dirty-mark.
	for (unsigned int activeSlot = 0; activeSlot < 3; ++activeSlot)
	{
		uint8_t selected = 0xFF;
		__try
		{
			selected = *reinterpret_cast<uint8_t*>(
				reinterpret_cast<uintptr_t>(pPlayer) + s_selectedOffhandsBase + activeSlot);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			continue;
		}

		if (selected == static_cast<uint8_t>(offhandIndex)
			&& v_Weapon_SetSelectedOffhandCleared)
		{
			v_Weapon_SetSelectedOffhandCleared(pPlayer, activeSlot);
		}
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Script signature: entity GetSelectedOffhand(int activeSlot)
static SQRESULT Script_GetSelectedOffhand(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	SQInteger activeSlot = 0;
	sq_getinteger(v, 2, &activeSlot);

	if (activeSlot < 0 || activeSlot >= 3)
	{
		Warning(eDLL_T::SERVER,
			"GetSelectedOffhand(): Invalid weapon slot specified: %d\n",
			static_cast<int>(activeSlot));
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	uint8_t offhandIndex = 0xFF;
	__try
	{
		offhandIndex = *reinterpret_cast<uint8_t*>(
			reinterpret_cast<uintptr_t>(pPlayer) + s_selectedOffhandsBase
			+ static_cast<uintptr_t>(activeSlot));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (offhandIndex == 0xFF)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* const pWeapon = OffhandOverride_ResolveOffhandEntityServer(
		pPlayer, static_cast<int>(offhandIndex), v);
	OffhandOverride_PushEntityOrNull(v, pWeapon);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Engine GetOffhandWeapons loops exactly 6 slots and skips empty ones (never
// pushes null). This extends that to SDK slots 6/7. Return type: array of entity.
static SQRESULT Script_GetOffhandWeapons_Override(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		pPlayer = nullptr;

	// Always return an array (empty on failure) -- never null.
	sq_newarray(v, 0);

	if (pPlayer)
	{
		for (int slot = 0; slot <= kOffhandSlotExtLast; ++slot)
		{
			void* const pWeapon = OffhandOverride_ResolveOffhandEntityServer(
				pPlayer, slot, v);
			if (!pWeapon)
				continue;
			OffhandOverride_PushEntityOrNull(v, pWeapon);
			sq_arrayappend(v, -2);
		}
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// CWeaponX::m_modVars; offhand_active_slot is +0xF74 (weapon+0x2754).
// +0xF70 is fire_mode (values 1..5) and must not be read as a hand index.
// eActiveInventorySlot: mainHand=0, altHand=1, utility=2 (3 members).
static constexpr uintptr_t s_weaponModVarsBase = 0x17E0;
static constexpr uintptr_t s_offhandActiveSlotModOff = 0xF74;
static constexpr int s_activeInventorySlotCount = 3;

// int GetOffhandActiveSlot -- GetActiveWeapon only accepts 0..2, never -1.
static SQRESULT Script_GetOffhandActiveSlot(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
	{
		sq_pushinteger(v, 0);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	int slot = 0;
	__try
	{
		slot = *reinterpret_cast<const int*>(
			reinterpret_cast<uintptr_t>(pWeapon) + s_weaponModVarsBase
			+ s_offhandActiveSlotModOff);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		slot = 0;
	}

	if (slot < 0 || slot >= s_activeInventorySlotCount)
	{
		static int s_nBadSlot = 0;
		if (s_nBadSlot++ < 8)
			Warning(eDLL_T::SERVER,
				"[OFFHAND-SLOT] offhand_active_slot=%d out of range; using mainHand\n",
				slot);
		slot = 0;
	}

	sq_pushinteger(v, slot);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// m_modBitfieldFromPlayer +0x1574, Internal +0x1578, Current +0x157C. SetModBitField writes Internal.
static constexpr uintptr_t s_weaponModBitfieldInternalOff = 0x1578;

// int GetInternalModBitField -- the engine's GetModBitField returns Current,
// which already has the player's loadout mods folded in; saving that and
// replaying it through SetModBitField makes those mods permanent.
static SQRESULT Script_GetInternalModBitField(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
	{
		sq_pushinteger(v, 0);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	int bitField = 0;
	__try
	{
		bitField = *reinterpret_cast<const int*>(
			reinterpret_cast<uintptr_t>(pWeapon) + s_weaponModBitfieldInternalOff);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		bitField = 0;
	}

	sq_pushinteger(v, bitField);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Register on the combat-character descriptor (same class as the engine). isServerStruct gates Give/Take.
void WeaponScriptVars_RegisterOffhandOverrides(ScriptClassDescriptor_t* combatCharStruct,
                                               bool isServerStruct)
{
	if (!combatCharStruct)
	{
		Warning(eDLL_T::SERVER,
			"[OFFHAND-REG] combat character script class descriptor is null; "
			"offhand overrides not registered\n");
		return;
	}

	// AddFunction does not set numeric FIELD_*; supply string types "entity" / "int" or scripts fail to compile.
	combatCharStruct->AddFunction(
		"GetOffhandWeapon",
		"Script_GetOffhandWeapon_Override",
		"Returns the offhand weapon entity installed at the given slot, or "
		"null if the slot is empty. Slots 0..5 go through the engine native; "
		"slots 6..7 are SDK-extended and routed through offhand_slots_ext.",
		"entity",
		"int offhandIndex",
		false,
		Script_GetOffhandWeapon_Override);

	if (!isServerStruct)
		return;

	// Engine GetOffhandWeapons only walks 6 slots; this override also includes
	// SDK-extended slots 6/7. Signature matches S21 (array of entity, no args).
	combatCharStruct->AddFunction(
		"GetOffhandWeapons",
		"Script_GetOffhandWeapons_Override",
		"Returns offhand weapons across all slots including SDK-extended slots "
		"6 and 7. Empty slots are omitted.",
		"array< entity >",
		"",
		false,
		Script_GetOffhandWeapons_Override);

	// Server VM only. Mods arg is optional (`array< string > mods = null`) so 2-arg and 3-arg both compile.
	combatCharStruct->AddFunction(
		"GiveOffhandWeapon",
		"Script_GiveOffhandWeapon_Override",
		"Give the offhand weapon in the specified slot and optionally apply "
		"active mods.",
		"void",
		"string weaponName, int slotIndex, array< string > mods = null",
		false,
		Script_GiveOffhandWeapon_Override);

	// Supply string "void" + "int offhandIndex" so the type compiler can resolve the override.
	combatCharStruct->AddFunction(
		"TakeOffhandWeapon",
		"Script_TakeOffhandWeapon_Override",
		"Take the offhand weapon in the specified slot.",
		"void",
		"int offhandIndex",
		false,
		Script_TakeOffhandWeapon_Override);

	// S3 registers this nowhere; S21 puts it on the combat character.
	combatCharStruct->AddFunction(
		"GetSelectedOffhand",
		"Script_GetSelectedOffhand",
		"Returns the offhand weapon currently selected in the given active inventory "
		"slot, or null if none is selected.",
		"entity",
		"int activeSlot",
		false,
		Script_GetSelectedOffhand);
}

// CancelOffhandWeapon is the one offhand native S21 keeps on the player
// class rather than the combat character, so it registers on its own.
void WeaponScriptVars_RegisterOffhandPlayerOverrides(ScriptClassDescriptor_t* playerStruct,
                                                     bool isServerStruct)
{
	if (!playerStruct || !isServerStruct)
		return;

	playerStruct->AddFunction(
		"CancelOffhandWeapon",
		"Script_CancelOffhandWeapon",
		"Cancel the offhand weapon at the given index: fast-holster it and clear any "
		"active inventory slot that had it selected.",
		"void",
		"int offhandIndex",
		false,
		Script_CancelOffhandWeapon);
}

// LockedSet sidecar: appended DT_WeaponX slots alias live m_modVars (base 6100 vs 0x17E0).
static SDKEntityMap<int> s_weaponLockedSet(ESide::Server, "weaponLockedSet.srv");

int WeaponScriptVars_WireGetLockedSet(void* pWeapon)
{
	if (!pWeapon)
		return 0;
	const int* const p = s_weaponLockedSet.Find(pWeapon);
	return p ? *p : 0;
}

static SQRESULT Script_GetWeaponLockedSet(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
		return SQ_ERROR;

	sq_pushinteger(v, WeaponScriptVars_WireGetLockedSet(pWeapon));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetWeaponLockedSet(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
		return SQ_ERROR;

	SQInteger lockedSet;
	sq_getinteger(v, 2, &lockedSet);

	if (lockedSet < 0 || lockedSet > 1023)
		lockedSet = (lockedSet < 0) ? 0 : 1023;

	s_weaponLockedSet[pWeapon] = static_cast<int>(lockedSet);
	// Load-bearing: nothing writes entity memory now, so this is the only signal
	// that makes the snapshot re-pack and run the value proxy.
	MarkEntityEdictDirty(pWeapon);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void WeaponScriptVars_WeaponLockedSet_LevelShutdown()
{
}

void WeaponScriptVars_RegisterWeaponLockedSetSetter(ScriptClassDescriptor_t* weaponStruct)
{
	weaponStruct->AddFunction(
		"SetWeaponLockedSet",
		"Script_SetWeaponLockedSet",
		"Set the weapon's locked set",
		"void",
		"int lockedSet",
		false,
		Script_SetWeaponLockedSet);
}

// Lock-free handle-keyed sidecar: publish key after state so a torn read cannot see a key with garbage state.
static constexpr int INFINITEAMMO_NONE = 0;
static constexpr int INFINITEAMMO_CLIPS = 1;

struct InfiniteAmmoSlot
{
	volatile uint64_t handleKey; // 0 = free; packed SDKEntityHandle otherwise
	volatile int       state;
};
static InfiniteAmmoSlot s_infiniteAmmoSlots[512];
static LONG s_infiniteAmmoCursor = 0;
// Claimed-slot count published before the key so a reader cannot see 0 while a key is already live.
static volatile LONG s_infiniteAmmoUsed = 0;

// Sidecar key is the packed handle. INVALID_EHANDLE_INDEX maps to 0 (free-slot sentinel).
static inline uint64_t InfiniteAmmo_PackHandle(const SDKEntityHandle& h)
{
	return h.IsValid() ? static_cast<uint64_t>(h.Raw()) : 0;
}

// Linear scan, 512 entries. Cold path on set; the value proxy read (called
// from a transmit-job worker thread) scans too -- acceptable, weapons with a
// non-NONE state are few.
static InfiniteAmmoSlot* InfiniteAmmo_FindSlot(uint64_t key)
{
	if (!key || !s_infiniteAmmoUsed)
		return nullptr;
	for (InfiniteAmmoSlot& slot : s_infiniteAmmoSlots)
	{
		if (slot.handleKey == key)
			return &slot;
	}
	return nullptr;
}

// Set/update path -- game thread only. Claims a free slot, or round-robin
// evicts, if this key has no existing slot.
static void InfiniteAmmo_SetState(uint64_t key, int state)
{
	if (!key)
		return;

	if (InfiniteAmmoSlot* existing = InfiniteAmmo_FindSlot(key))
	{
		existing->state = state;
		return;
	}

	InfiniteAmmoSlot* freeSlot = nullptr;
	for (InfiniteAmmoSlot& slot : s_infiniteAmmoSlots)
	{
		if (slot.handleKey == 0)
		{
			freeSlot = &slot;
			break;
		}
	}
	if (freeSlot)
		InterlockedIncrement(&s_infiniteAmmoUsed);
	else
	{
		const LONG idx = (InterlockedIncrement(&s_infiniteAmmoCursor) - 1) & 511;
		freeSlot = &s_infiniteAmmoSlots[idx];
	}

	// Retire the evicted key BEFORE touching state, then write state BEFORE
	// publishing the new key: a concurrent reader can only ever observe a
	// free slot or a fully-consistent (key, state) pair.
	freeSlot->handleKey = 0;
	freeSlot->state = state;
	freeSlot->handleKey = key;
}

static int InfiniteAmmo_GetState(uint64_t key)
{
	const InfiniteAmmoSlot* slot = InfiniteAmmo_FindSlot(key);
	return slot ? slot->state : INFINITEAMMO_NONE;
}

// Cross-thread-safe: called from the snapshot pack value proxy (transmit
// job worker) and the server ammo detours. Sidecar scan, no locks.
// Encode path: closed-form handle pack (no SEH).
int WeaponScriptVars_GetInfiniteAmmoState(const void* pWeapon)
{
	if (!pWeapon)
		return INFINITEAMMO_NONE;

	const uint64_t key = InfiniteAmmo_PackHandle(SDKEntityState_GetHandle(pWeapon));
	return InfiniteAmmo_GetState(key);
}

static SQRESULT Script_GetInfiniteAmmoState(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushinteger(v, WeaponScriptVars_GetInfiniteAmmoState(pWeapon));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetInfiniteAmmoState(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	SQInteger state;
	sq_getinteger(v, 2, &state);

	if (state < INFINITEAMMO_NONE || state > INFINITEAMMO_CLIPS)
		state = INFINITEAMMO_NONE;

	const SDKEntityHandle handle = SDKEntityState_GetHandle(pWeapon);
	const uint64_t key = InfiniteAmmo_PackHandle(handle);

	// Invalid handle -- drop silently, matches old SDKEntityMap behavior.
	if (!key)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	InfiniteAmmo_SetState(key, static_cast<int>(state));

	if (v && v->GetContext() == SQCONTEXT::SERVER)
	{
		// Cold path (toggle only) -- the one line that proves script reached
		// the sidecar, so it stays unconditional rather than diag-gated.
		static volatile LONG s_setN = 0;
		if (InterlockedIncrement(&s_setN) <= 16)
			Warning(eDLL_T::SERVER, "[INF-AMMO] set state=%d ent=%p readback=%d\n",
				static_cast<int>(state), pWeapon,
				WeaponScriptVars_GetInfiniteAmmoState(pWeapon));

		// Do not top up reserve: S21 infinite ammo never writes it, and 0x1338 is an FSM bool on a live weapon.

		MarkEntityEdictDirty(pWeapon);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void WeaponScriptVars_InfiniteAmmo_LevelShutdown()
{
	// Retire the used count FIRST so a reader mid-shutdown skips the scan
	// rather than walking slots being zeroed underneath it.
	InterlockedExchange(&s_infiniteAmmoUsed, 0);
	memset(s_infiniteAmmoSlots, 0, sizeof(s_infiniteAmmoSlots));
}

void WeaponScriptVars_RegisterInfiniteAmmoFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	weaponStruct->AddFunction(
		"GetInfiniteAmmoState",
		"Script_GetInfiniteAmmoState",
		"Gets the infinite ammo state of the weapon",
		"int",
		"",
		false,
		Script_GetInfiniteAmmoState);
}

void WeaponScriptVars_RegisterInfiniteAmmoSetter(ScriptClassDescriptor_t* weaponStruct)
{
	weaponStruct->AddFunction(
		"SetInfiniteAmmoState",
		"Script_SetInfiniteAmmoState",
		"Sets the infinite ammo state of the weapon",
		"void",
		"int state",
		false,
		Script_SetInfiniteAmmoState);
}

// Per-entity disabled-type bitmask + refcount. Bit 0 is WPT_PRIMARY (no "cannot disable bit 0" guard).
// WPT_VIEWHANDS is sticky in weapon_enforce -- S3 has no sub-viewhands fallback.
static constexpr int WEAPON_TYPE_DISABLE_BITS = 10; // bits 0..9 inclusive

struct WeaponTypeDisableState
{
	uint32_t disabledFlags = 0;
	uint32_t holdFlags = 0; // same-frame Enable+Disable must not drop the mask
	uint8_t  refCount[WEAPON_TYPE_DISABLE_BITS] = {};
};

// Server map tracks refcount and feeds force-swap. The client reads the replicated native DT prop.
static SDKEntityMap<WeaponTypeDisableState> s_weaponTypeDisabledMapServer(
	ESide::Server, "weaponTypeDisabled.srv");

// Mirror the resulting bitmask into the native m_weaponTypeDisabledFlags DT prop
// on the entity + dirty-mark, so the engine encoder ships it to clients. No-op
// (harmless) before dt_extend has assigned the slack offset.
static void WeaponScriptVars_WriteNativeDisabledFlags(void* pEntity, uint32_t flags)
{
	if (!pEntity)
		return;
	BCCExtend_SetI32(pEntity, offsetof(BCCExtendWire, m_weaponTypeDisabledFlags),
		static_cast<int32_t>(flags));
	MarkEntityEdictDirty(pEntity);
}

// Read the native m_weaponTypeDisabledFlags DT prop off the entity. Used by the
// client/UI VMs (which have no server refcount map) so IsWeaponTypeEnabled
// reflects the replicated value the engine itself gates on.
static uint32_t WeaponScriptVars_ReadNativeDisabledFlags(const void* pEntity)
{
	if (!pEntity)
		return 0;
	return static_cast<uint32_t>(
		BCCExtend_GetI32(pEntity, offsetof(BCCExtendWire, m_weaponTypeDisabledFlags)));
}

static void WeaponScriptVars_ClearWeaponTypeDisabledMap()
{
	s_weaponTypeDisabledMapServer.Clear();
}

// Returns the server-side disabled mask for an entity, or 0.
uint32_t WeaponScriptVars_GetDisabledFlagsForEntity(const void* pEntity)
{
	if (!pEntity)
		return 0;
	if (const WeaponTypeDisableState* p = s_weaponTypeDisabledMapServer.Find(pEntity))
		return p->disabledFlags | p->holdFlags;
	return 0;
}

void WeaponScriptVars_FlushDisableHoldFlags(void)
{
	for (auto it = s_weaponTypeDisabledMapServer.begin();
		it != s_weaponTypeDisabledMapServer.end(); ++it)
	{
		WeaponTypeDisableState* const p =
			s_weaponTypeDisabledMapServer.Find(it->first);
		if (!p || p->holdFlags == 0)
			continue;
		p->holdFlags = 0;
		void* const pEntity = SDKEntityState_Resolve(it->first, ESide::Server);
		if (pEntity)
			WeaponScriptVars_WriteNativeDisabledFlags(pEntity, p->disabledFlags);
	}
}

static SQRESULT Script_DisableWeaponTypes(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	SQInteger rawFlags = 0;
	sq_getinteger(v, 2, &rawFlags);
	const uint32_t flags = static_cast<uint32_t>(rawFlags);

	if (flags >= (1u << WEAPON_TYPE_DISABLE_BITS))
	{
		v_SQVM_ScriptError("DisableWeaponTypes: flags 0x%X exceed max (must be < 0x%X)",
			flags, (1u << WEAPON_TYPE_DISABLE_BITS));
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}
	// Do not port the bit-0 guard: S3 bit 0 is WPT_PRIMARY. Only the server VM mutates the mask.
	if (v && v->GetContext() == SQCONTEXT::SERVER)
	{
		WeaponTypeDisableState& state = s_weaponTypeDisabledMapServer[pEntity];

		uint32_t bit = 1;
		for (int i = 0; i < WEAPON_TYPE_DISABLE_BITS; ++i, bit <<= 1)
		{
			if ((flags & bit) == 0)
				continue;
			if (state.refCount[i] == 0xFF)
			{
				Warning(eDLL_T::SERVER,
					"DisableWeaponTypes: refcount overflow for type %d on entity %p\n",
					i, pEntity);
				continue;
			}
			if (state.refCount[i] == 0)
				state.disabledFlags |= bit;
			++state.refCount[i];
		}

		state.holdFlags |= state.disabledFlags;
		WeaponScriptVars_WriteNativeDisabledFlags(pEntity,
			state.disabledFlags | state.holdFlags);

		// Clear any active weapon whose type is now disabled. The S3 server has
		// no engine-native gating, so this server-side rail stays; the engine's
		// next-tick re-selection picks a valid fallback.
		WeaponEnforce_ForceSwapIfNowDisabled(pEntity);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_EnableWeaponTypes(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	SQInteger rawFlags = 0;
	sq_getinteger(v, 2, &rawFlags);
	const uint32_t flags = static_cast<uint32_t>(rawFlags);

	if (flags >= (1u << WEAPON_TYPE_DISABLE_BITS))
	{
		v_SQVM_ScriptError("EnableWeaponTypes: flags 0x%X exceed max (must be < 0x%X)",
			flags, (1u << WEAPON_TYPE_DISABLE_BITS));
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	// Server-authoritative (see DisableWeaponTypes). Client/UI VMs are read-only.
	if (v && v->GetContext() == SQCONTEXT::SERVER)
	{
		WeaponTypeDisableState* pState = s_weaponTypeDisabledMapServer.Find(pEntity);
		if (!pState)
		{
			if (flags != 0)
				Warning(eDLL_T::SERVER,
					"EnableWeaponTypes: no disabled state for entity %p (refcount underflow)\n",
					pEntity);
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}

		WeaponTypeDisableState& state = *pState;

		uint32_t bit = 1;
		for (int i = 0; i < WEAPON_TYPE_DISABLE_BITS; ++i, bit <<= 1)
		{
			if ((flags & bit) == 0)
				continue;
			if (state.refCount[i] == 0)
			{
				Warning(eDLL_T::SERVER,
					"EnableWeaponTypes: refcount underflow for type %d on entity %p\n",
					i, pEntity);
				continue;
			}
			--state.refCount[i];
			if (state.refCount[i] == 0)
				state.disabledFlags &= ~bit;
		}

		// Push the new bitmask to the native DT prop + dirty-mark BEFORE any
		// map erase (erasing invalidates `state`).
		const uint32_t newFlags = state.disabledFlags | state.holdFlags;
		WeaponScriptVars_WriteNativeDisabledFlags(pEntity, newFlags);

		// Drop empty entries to keep the map bounded across long sessions.
		bool anySet = newFlags != 0;
		if (!anySet)
		{
			for (int i = 0; i < WEAPON_TYPE_DISABLE_BITS; ++i)
				if (state.refCount[i] != 0) { anySet = true; break; }
			if (!anySet)
				s_weaponTypeDisabledMapServer.Erase(pEntity);
		}
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsWeaponTypeEnabled(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

	SQInteger rawFlags = 0;
	sq_getinteger(v, 2, &rawFlags);
	const uint32_t flags = static_cast<uint32_t>(rawFlags);

	// Server VM reads the authoritative refcount map; client/UI VMs read the
	// replicated native m_weaponTypeDisabledFlags prop (the engine gates on it).
	uint32_t disabled = 0;
	if (v && v->GetContext() == SQCONTEXT::SERVER)
	{
		const WeaponTypeDisableState* pState = s_weaponTypeDisabledMapServer.Find(pEntity);
		disabled = pState ? pState->disabledFlags : 0u;
	}
	else
	{
		disabled = WeaponScriptVars_ReadNativeDisabledFlags(pEntity);
	}

	sq_pushbool(v, (disabled & flags) == 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static void OffhandOverride_PushEntityOrNull(HSQUIRRELVM v, void* pEntity);

// No CTurret::SetDriver. Mount writes a camera EHANDLE the fire-angle path honors. Leave +0x6890 alone.
static constexpr ptrdiff_t PLAYER_OFF_TURRET_CAM = 0x6878;
static constexpr uint32_t TURRET_HANDLE_NONE = 0xFFFFFFFFu;

static ConVar bridge_turret_driver("bridge_turret_driver", "1", FCVAR_RELEASE,
	"Arm CPlayer turret camera handle from script SetDriver so dedi fire aims with the turret.");
static ConVar bridge_turret_driver_log("bridge_turret_driver_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log SetDriver/ClearDriver/GetTurret.");

struct TurretDriverBind_t
{
	void* pTurret;
	void* pDriver;
};

static std::unordered_map<void*, TurretDriverBind_t> s_turretByDriver;
static std::unordered_map<void*, TurretDriverBind_t> s_driverByTurret;

static void TurretDriver_LevelShutdown(void)
{
	s_turretByDriver.clear();
	s_driverByTurret.clear();
}

static uint32_t TurretDriver_EntHandle(const void* pEnt)
{
	if (!pEnt)
		return TURRET_HANDLE_NONE;
	return *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(pEnt) + 0x08);
}

static bool TurretDriver_IsPlayer(void* pEnt)
{
	if (!pEnt)
		return false;
	void** const pVt = *reinterpret_cast<void***>(pEnt);
	if (!pVt)
		return false;
	using PFN_IsPlayer = unsigned char(__fastcall*)(void*);
	const PFN_IsPlayer pIsPlayer = reinterpret_cast<PFN_IsPlayer>(pVt[93]);
	if (!pIsPlayer)
		return false;
	return pIsPlayer(pEnt) != 0;
}

static void* TurretDriver_EntityFromStack(HSQUIRRELVM v, SQInteger sqIdx)
{
	const SQObjectPtr& o = stack_get(v, sqIdx);
	if (sq_isnull(o) || o._type != OT_ENTITY || !o._unVal.pInstance)
		return nullptr;
	return *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(o._unVal.pInstance) + 0x50);
}

static void TurretDriver_WriteCam(void* pPlayer, uint32_t handle)
{
	*reinterpret_cast<uint32_t*>(
		reinterpret_cast<uint8_t*>(pPlayer) + PLAYER_OFF_TURRET_CAM) = handle;
}

static void TurretDriver_DisarmPlayer(void* pPlayer)
{
	if (!pPlayer)
		return;
	TurretDriver_WriteCam(pPlayer, TURRET_HANDLE_NONE);
	s_turretByDriver.erase(pPlayer);
}

static void TurretDriver_ClearPair(void* pTurret, void* pPlayer)
{
	if (pPlayer)
		TurretDriver_DisarmPlayer(pPlayer);
	if (pTurret)
		s_driverByTurret.erase(pTurret);
}

static SQRESULT Script_SetDriver(HSQUIRRELVM v)
{
	void* pTurret = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pTurret)) || !pTurret)
		return SQ_ERROR;

	void* const pPlayer = TurretDriver_EntityFromStack(v, 2);
	if (!pPlayer || !TurretDriver_IsPlayer(pPlayer))
	{
		v_SQVM_ScriptError("SetDriver: argument is not a player");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	if (!bridge_turret_driver.GetBool())
	{
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const auto itPlayer = s_turretByDriver.find(pPlayer);
	if (itPlayer != s_turretByDriver.end() && itPlayer->second.pTurret != pTurret)
		TurretDriver_ClearPair(itPlayer->second.pTurret, pPlayer);

	const auto itTurret = s_driverByTurret.find(pTurret);
	if (itTurret != s_driverByTurret.end() && itTurret->second.pDriver != pPlayer)
		TurretDriver_ClearPair(pTurret, itTurret->second.pDriver);

	const uint32_t hTurret = TurretDriver_EntHandle(pTurret);
	TurretDriver_WriteCam(pPlayer, hTurret);

	const TurretDriverBind_t bind = { pTurret, pPlayer };
	s_turretByDriver[pPlayer] = bind;
	s_driverByTurret[pTurret] = bind;

	static bool s_bArmed = false;
	if (!s_bArmed)
	{
		s_bArmed = true;
		Msg(eDLL_T::SERVER, "[SHEILA] SetDriver armed\n");
	}
	if (bridge_turret_driver_log.GetBool())
		Msg(eDLL_T::SERVER, "[SHEILA] SetDriver turret=%p player=%p handle=0x%08X\n",
			pTurret, pPlayer, hTurret);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_ClearDriver(HSQUIRRELVM v)
{
	void* pTurret = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pTurret)) || !pTurret)
		return SQ_ERROR;

	void* pPlayer = nullptr;
	const auto it = s_driverByTurret.find(pTurret);
	if (it != s_driverByTurret.end())
		pPlayer = it->second.pDriver;

	TurretDriver_ClearPair(pTurret, pPlayer);

	if (bridge_turret_driver_log.GetBool())
		Msg(eDLL_T::SERVER, "[SHEILA] ClearDriver turret=%p player=%p\n", pTurret, pPlayer);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetDriver(HSQUIRRELVM v)
{
	void* pTurret = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pTurret)) || !pTurret)
		return SQ_ERROR;

	void* pPlayer = nullptr;
	const auto it = s_driverByTurret.find(pTurret);
	if (it != s_driverByTurret.end())
		pPlayer = it->second.pDriver;

	OffhandOverride_PushEntityOrNull(v, pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsTurretEnt(HSQUIRRELVM v)
{
	void* pTurret = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pTurret)) || !pTurret)
		return SQ_ERROR;

	sq_pushbool(v, s_driverByTurret.find(pTurret) != s_driverByTurret.end());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetTurret(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	void* pTurret = nullptr;
	const auto it = s_turretByDriver.find(pPlayer);
	if (it != s_turretByDriver.end())
		pTurret = it->second.pTurret;

	OffhandOverride_PushEntityOrNull(v, pTurret);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void WeaponScriptVars_RegisterEntityFuncs(ScriptClassDescriptor_t* entityStruct)
{
	DevMsg(eDLL_T::CLIENT, "[WeaponScriptVars] Registering entity highlight functions\n");

	// Register on the entity descriptor only: a second binding on weapon breaks non-weapon callers.
	entityStruct->AddFunction(
		"IsWeaponX",
		"Script_IsWeaponX",
		"Returns true if this entity is a CWeaponX",
		"bool",
		"",
		false,
		Script_IsWeaponX);

	entityStruct->AddFunction(
		"GetItemFlavorGUID",
		"Script_GetItemFlavorGUID",
		"Returns the cosmetic item flavor GUID replicated for this entity",
		"int",
		"",
		false,
		Script_GetItemFlavorGUID);

	entityStruct->AddFunction(
		"SetItemFlavorGUID",
		"Script_SetItemFlavorGUID",
		"Sets the cosmetic item flavor GUID replicated for this entity",
		"void",
		"int guid",
		false,
		Script_SetItemFlavorGUID);

	entityStruct->AddFunction(
		"EnableTouchAutoUse",
		"Script_EnableTouchAutoUse",
		"Enables touch-based auto-use within a distance",
		"void",
		"float distance",
		false,
		Script_EnableTouchAutoUse);

	entityStruct->AddFunction(
		"GetChildren",
		"Script_GetChildren",
		"Returns an array of all child entities parented to this entity",
		"array< entity >",
		"",
		false,
		Script_GetChildren);

	entityStruct->AddFunction(
		"SetLaunchDelay",
		"Script_SetLaunchDelay",
		"Sets the gravity-cannon charge window in seconds",
		"void",
		"float delay",
		false,
		Script_SetLaunchDelay);

	entityStruct->AddFunction(
		"GravityCannonIsPreparingLaunch",
		"Script_GravityCannonIsPreparingLaunch",
		"True while this gravity cannon's charge window is open",
		"bool",
		"",
		false,
		Script_GravityCannonIsPreparingLaunch);

	entityStruct->AddFunction(
		"SetLaunchTargetLocation",
		"Script_SetLaunchTargetLocation",
		"Solves launchDir/power from a landing location",
		"void",
		"vector target",
		false,
		Script_SetLaunchTargetLocation);

	entityStruct->AddFunction(
		"GetLaunchDir",
		"Script_GetLaunchDir",
		"Returns this trigger's m_launchDir",
		"vector",
		"",
		false,
		Script_GetLaunchDir);

	entityStruct->AddFunction(
		"SetEnableDoubleJump",
		"Script_SetEnableDoubleJump",
		"Sets whether this Heavy trigger grants a double jump on launch",
		"void",
		"bool enable",
		false,
		Script_SetEnableDoubleJump);

	entityStruct->AddFunction(
		"SetLimitedAirControl",
		"Script_SetLimitedAirControl",
		"Restricts air control for the duration of this trigger's launch",
		"void",
		"bool limited",
		false,
		Script_SetLimitedAirControl);

	entityStruct->AddFunction(
		"SetLaunchAirControlParams",
		"Script_SetLaunchAirControlParams",
		"Sets post-launch air-control speed and accel on a Heavy trigger",
		"void",
		"float speed, float accel",
		false,
		Script_SetLaunchAirControlParams);

	entityStruct->AddFunction(
		"SetGravityLiftParams",
		"Script_SetGravityLiftParams",
		"Sets gravity-lift speeds/accels/eject/hover parameters on a Heavy trigger",
		"void",
		"var upSpeed, var upAccel, var horizontalSpeed, var horizontalAccel, "
		"var toCenterSpeed, var toCenterAccel, var ejectUpSpeed, var ejectHorizontalSpeed, "
		"var maxEjectTime, var maxHoverTime",
		false,
		Script_SetGravityLiftParams);

	entityStruct->AddFunction(
		"SetBlackholeParams",
		"Script_SetBlackholeParams",
		"Sets blackhole pull/move speeds and inner radius on a Heavy trigger",
		"void",
		"var strongPullAddlSpeed, var pullOuterSpeed, var pullInnerSpeed, "
		"var moveOuterSpeed, var moveInnerSpeed, var innerRadius",
		false,
		Script_SetBlackholeParams);

	entityStruct->AddFunction(
		"SetBlackholeIsStrongPulling",
		"Script_SetBlackholeIsStrongPulling",
		"Sets whether this blackhole applies the strong-pull bonus",
		"void",
		"bool isStrong",
		false,
		Script_SetBlackholeIsStrongPulling);

	entityStruct->AddFunction(
		"SetDriver",
		"Script_SetDriver",
		"Mounts a player on this turret so dedi fire aims through the turret camera",
		"void",
		"entity player",
		false,
		Script_SetDriver);

	entityStruct->AddFunction(
		"ClearDriver",
		"Script_ClearDriver",
		"Dismounts the current driver of this turret",
		"void",
		"",
		false,
		Script_ClearDriver);

	entityStruct->AddFunction(
		"GetDriver",
		"Script_GetDriver",
		"Returns the player driving this turret, or null",
		"entity",
		"",
		false,
		Script_GetDriver);

	entityStruct->AddFunction(
		"IsTurretEnt",
		"Script_IsTurretEnt",
		"True after SetDriver has been called on this entity this match",
		"bool",
		"",
		false,
		Script_IsTurretEnt);

	entityStruct->AddFunction(
		"SetCylinderRadius",
		"Script_SetCylinderRadius",
		"Sets the entity's collision cylinder radius",
		"void",
		"var radius",
		false,
		Script_SetCylinderRadius);

	entityStruct->AddFunction(
		"GetUsableValue",
		"Script_GetUsableValue",
		"Returns the usable flags bitmask for this entity",
		"int",
		"",
		false,
		Script_GetUsableValue);

	entityStruct->AddFunction(
		"HighlightEnableForTeam",
		"Script_HighlightEnableForTeam",
		"Enables highlight for a team by context ID",
		"void",
		"int contextId, int team",
		false,
		Script_HighlightEnableForTeam);

	entityStruct->AddFunction(
		"HighlightDisableForTeam",
		"Script_HighlightDisableForTeam",
		"Disables highlight for a team by context ID",
		"void",
		"int contextId, int team",
		false,
		Script_HighlightDisableForTeam);

	entityStruct->AddFunction(
		"IsHighlightEnabledForTeam",
		"Script_IsHighlightEnabledForTeam",
		"Checks if highlight is enabled for a team by context ID",
		"bool",
		"int contextId, int team",
		false,
		Script_IsHighlightEnabledForTeam);

	entityStruct->AddFunction(
		"Highlight_OverrideParam",
		"Script_Highlight_OverrideParam",
		"Overrides a highlight param with a vector value",
		"void",
		"int paramId, vector value",
		false,
		Script_Highlight_OverrideParam);

	entityStruct->AddFunction(
		"Highlight_ClearOverrideParam",
		"Script_Highlight_ClearOverrideParam",
		"Clears an overridden highlight param",
		"void",
		"int paramId",
		false,
		Script_Highlight_ClearOverrideParam);

	entityStruct->AddFunction(
		"HighlightEnableForTeam",
		"Script_HighlightEnableForTeam",
		"Enables highlight for a team by context ID",
		"void",
		"int contextId, int team",
		false,
		Script_HighlightEnableForTeam);

	entityStruct->AddFunction(
		"HighlightDisableForTeam",
		"Script_HighlightDisableForTeam",
		"Disables highlight for a team by context ID",
		"void",
		"int contextId, int team",
		false,
		Script_HighlightDisableForTeam);

	entityStruct->AddFunction(
		"Highlight_SetGenericHighlightContext",
		"Script_Highlight_SetGenericHighlightContext",
		"Sets a generic highlight context by type",
		"void",
		"int genericType, int contextId, bool focused",
		false,
		Script_Highlight_SetGenericHighlightContext);

	entityStruct->AddFunction(
		"Highlight_GetGenericHighlightContext",
		"Script_Highlight_GetGenericHighlightContext",
		"Gets a generic highlight context by type",
		"int",
		"int genericType",
		false,
		Script_Highlight_GetGenericHighlightContext);

	// PhaseShift type system - GetPhaseShiftType on entity level
	entityStruct->AddFunction(
		"GetPhaseShiftType",
		"Script_GetPhaseShiftType",
		"Returns the phase shift type enum value",
		"int",
		"",
		false,
		Script_GetPhaseShiftType);
}

// Register on both VMs. Map keys are per-side entity pointers (CPlayer vs C_Player).
void WeaponScriptVars_RegisterWeaponTypeDisableFuncs(ScriptClassDescriptor_t* entityStruct)
{
	entityStruct->AddFunction(
		"DisableWeaponTypes",
		"Script_DisableWeaponTypes",
		"Disables one or more weapon types (bitmask) with refcounted semantics",
		"void",
		"int weaponTypeFlags",
		false,
		Script_DisableWeaponTypes);

	entityStruct->AddFunction(
		"EnableWeaponTypes",
		"Script_EnableWeaponTypes",
		"Re-enables one or more weapon types (bitmask) by decrementing refcount",
		"void",
		"int weaponTypeFlags",
		false,
		Script_EnableWeaponTypes);

	entityStruct->AddFunction(
		"IsWeaponTypeEnabled",
		"Script_IsWeaponTypeEnabled",
		"Returns true if none of the specified weapon-type bits are currently disabled",
		"bool",
		"int weaponTypeFlags",
		false,
		Script_IsWeaponTypeEnabled);
}

// WPT_* bits 0..7 match the engine table. Patch extends bits 8-9 (WPT_VIEWHANDS / WPT_SURVIVAL).
void WeaponScriptVars_RegisterWPTConstants(CSquirrelVM* s)
{
	// Engine-native bits (resolved in S3 r5apex.exe).
	s->RegisterConstant("WPT_PRIMARY",      0x001);
	s->RegisterConstant("WPT_MELEE",        0x002);
	s->RegisterConstant("WPT_TACTICAL",     0x004);
	s->RegisterConstant("WPT_ULTIMATE",     0x008);
	s->RegisterConstant("WPT_CONSUMABLE",   0x010);
	s->RegisterConstant("WPT_INCAP_SHIELD", 0x020);
	s->RegisterConstant("WPT_GRENADE",      0x040);
	s->RegisterConstant("WPT_OTHER",        0x080);

	// + script-only flags -- not recognised by S3's
	// GetFlagsFromString_Internal; kept out of the low byte so the
	// composite masks below stay inside 8-bit native range.
	s->RegisterConstant("WPT_VIEWHANDS",    0x100);
	s->RegisterConstant("WPT_SURVIVAL",     0x200);

	// uses WPT_INCAP in composite names ("..._OR_INCAP"); S3 only has
	// WPT_INCAP_SHIELD. Alias so scripts written for either spelling work.
	s->RegisterConstant("WPT_INCAP",        0x020);

	// Composite masks. 0xFF covers all native bits; the _OR_INCAP variant
	// additionally exempts WPT_INCAP_SHIELD (0x020) so bleedout/knockdown
	// shield stays usable while everything else is disabled.
	s->RegisterConstant("WPT_ALL_EXCEPT_VIEWHANDS",          0xFF);
	s->RegisterConstant("WPT_ALL_EXCEPT_VIEWHANDS_OR_INCAP", 0xFF & ~0x020);
}

struct EWeaponVarCompat_t
{
	const char* name;
	const char* sourceName;
};

// S21-only keys redirected onto free S3 native slots at parse time.
// custom_* index 0 is content-authored; only 1..7 are free to reserve.
struct WeaponVarSlotReservation_t
{
	const char* s21Key;
	const char* s3Slot;
};

static const WeaponVarSlotReservation_t s_reservedWeaponVarSlots[] = {
	{ "melee_knockback_velocity_magnitude",        "custom_float_1" },
	{ "lifesteal_heal_percent",                    "custom_float_2" },
	{ "deflect_missile_impacts_dot",               "custom_float_3" },
	{ "heartbeat_sensor_size",                     "custom_float_4" },
	{ "heat_per_bullet",                           "custom_float_5" },
	{ "charge_overheat_cooldown_time",             "custom_float_6" },
	{ "charge_overheat_cooldown_delay",            "custom_float_7" },
	{ "charge_overheat_cooldown_time_late1",       "charge_cooldown_time_late1" },
	{ "charge_overheat_cooldown_time_late2",       "charge_cooldown_time_late2" },
	{ "charge_overheat_cooldown_time_late3",       "charge_cooldown_time_late3" },
	{ "is_heirloom",                               "custom_bool_1"  },
	{ "is_artifact",                               "custom_bool_2"  },
	{ "offhand_restore_after_melee",               "custom_bool_3"  },
	{ "grenade_mover_destroy_when_planted",        "custom_bool_4"  },
	{ "toss_has_post_loop",                        "custom_bool_5"  },
	{ "object_placement_vehicle_attachment_index", "custom_int_1"   },
	{ "battle_chatter_event",                      "ui32_mesh_override" },
};

static ConVar bridge_weaponvar_slot_reserve("bridge_weaponvar_slot_reserve", "1",
	FCVAR_RELEASE,
	"Back S21-only weapon settings keys with reserved native slots (0 = donor aliases only).");

// Returns 1 if a new alias was added, 0 if the name already exists natively
// (left intact), -1 on error.
static int CopyEWeaponVarSlot(HSQUIRRELVM v, const EWeaponVarCompat_t& compat)
{
	// Never sq_newslot a name the native eWeaponVar enum already defines (fire_rate would become custom_float_0).
	sq_pushstring(v, compat.name, -1);
	if (sq_get(v, -2) >= 0)
	{
		sq_pop(v, 1);
		return 0;
	}

	sq_pushstring(v, compat.sourceName, -1);
	if (sq_get(v, -2) < 0)
	{
		Warning(eDLL_T::COMMON,
			"[s21-eweaponvar] source eWeaponVar.%s missing; cannot alias %s\n",
			compat.sourceName, compat.name);
		return -1;
	}

	SQInteger value = 0;
	if (sq_getinteger(v, -1, &value) < 0)
	{
		sq_pop(v, 1);
		Warning(eDLL_T::COMMON,
			"[s21-eweaponvar] source eWeaponVar.%s is not an integer; cannot alias %s\n",
			compat.sourceName, compat.name);
		return -1;
	}
	sq_pop(v, 1);

	sq_pushstring(v, compat.name, -1);
	sq_pushinteger(v, value);
	if (sq_newslot(v, -3) < 0)
	{
		Warning(eDLL_T::COMMON,
			"[s21-eweaponvar] failed adding eWeaponVar.%s -> %s (%lld)\n",
			compat.name, compat.sourceName, static_cast<long long>(value));
		return -1;
	}

	return 1;
}

void WeaponScriptVars_RegisterS21EWeaponVarAliases(CSquirrelVM* s)
{
	if (!s)
		return;

	static const EWeaponVarCompat_t compatVars[] = {
		{ "fire_rate_max", "custom_int_0" },
		{ "fire_rate", "custom_float_0" },
		{ "fire_rate_max_time_cooldown", "custom_float_0" },
		{ "fire_rate_max_time_speedup", "custom_float_0" },
		{ "burst_fire_delay_ramp_bursts", "custom_float_0" },
		{ "burst_fire_delay_ramp_max", "custom_float_0" },
		{ "clone_anim_blending", "custom_float_0" },
		{ "clone_sync_to_player", "custom_float_0" },
		{ "force_has_weapon_clone", "titanarmor_critical_hit_required" },
		{ "damage_flags", "custom_int_0" },
		{ "is_twohanded_consumable", "titanarmor_critical_hit_required" },
		{ "is_consumable", "titanarmor_critical_hit_required" },
		{ "activitymodifier3p", "printname" },
		{ "activitymodifier1p", "printname" },
		{ "alt_hand_3p_attach_name", "printname" },
		{ "update_player_last_fire_time", "custom_float_0" },
		{ "allow_zoom_on_raise", "titanarmor_critical_hit_required" },
		{ "ads_anim_blend_enabled", "custom_float_0" },
		// melee_knockback_velocity_magnitude: reserved-slot backed (custom_float_1).
		{ "ads_force_firstperson", "custom_float_0" },
		{ "deflect_missile_impacts", "titanarmor_critical_hit_required" },
		{ "play_one_handed_alt_hand_anim_on_mainhand", "titanarmor_critical_hit_required" },
		{ "is_clacker", "titanarmor_critical_hit_required" },
		{ "viewmodel_color_by_soundmeter", "printname" },
		{ "offhand_hybrid_tracks_projectiles", "custom_float_0" },
		{ "offhand_hybrid_alt_hand_uses_attack_button", "custom_float_0" },
		{ "offhand_deactivate_on_jump_toggle_or_release", "custom_float_0" },
		{ "offhand_holds_on_tactical", "titanarmor_critical_hit_required" },
		{ "offhand_disable_other_offhands", "titanarmor_critical_hit_required" },
		{ "offhand_wants_first_deploy", "titanarmor_critical_hit_required" },
		{ "offhand_allows_inpect", "titanarmor_critical_hit_required" },
		{ "offhand_hybrid_reset_shot_count_on_attack", "custom_float_0" },
		{ "has_linked_anims", "titanarmor_critical_hit_required" },
		{ "melee_allow_held", "custom_float_0" },
		{ "melee_has_gesture", "custom_float_0" },
		{ "melee_anim_1p", "custom_float_0" },
		{ "offhand_include_primary_activity_mods", "custom_float_0" },
		{ "offhand_interrupt_climbing", "titanarmor_critical_hit_required" },
		{ "offhand_only_swap_to_on_ground", "titanarmor_critical_hit_required" },
		{ "offhand_allow_swap_to_on_zipline", "titanarmor_critical_hit_required" },
		{ "offhand_clear_zoom_on_activate", "titanarmor_critical_hit_required" },
		{ "offhand_chargeEnd_holster_on_noattack", "custom_float_0" },
		{ "offhand_instant_swap_to_offhand", "titanarmor_critical_hit_required" },
		{ "projectile_trail_start_from_origin", "hud_icon" },
		{ "fire_to_redirect_projectile_mid_flight", "custom_float_0" },
		{ "althand_allow_mainhand_on_zipline", "custom_float_0" },
		{ "offhand_match_player_skin", "custom_float_0" },
		{ "regen_ammo_forced_delay", "custom_float_0" },
		{ "ammo_regen_takes_from_stockpile", "titanarmor_critical_hit_required" },
		{ "critical_hit", "titanarmor_critical_hit_required" },
		{ "impulse_force", "custom_float_0" },
		{ "grenade_arc_indicator_max_duration", "custom_float_0" },
		{ "grenade_arc_indicator_show_while_airborne", "titanarmor_critical_hit_required" },
		{ "grenade_arc_indicator_show_during_toss", "titanarmor_critical_hit_required" },
		{ "grenade_arc_indicator_show_in_sprint_if_ready", "titanarmor_critical_hit_required" },
		{ "grenade_arc_indicator_show_on_raise_if_ready", "titanarmor_critical_hit_required" },
		{ "grenade_arc_indicator_show_floor_impact", "titanarmor_critical_hit_required" },
		{ "grenade_arc_indicator_show_wall_impact", "titanarmor_critical_hit_required" },
		{ "grenade_arc_indicator_smooth_radius", "custom_float_0" },
		{ "grenade_arc_indicator_smooth", "custom_float_0" },
		{ "grenade_arc_indicator_show_landing_position", "titanarmor_critical_hit_required" },
		{ "grenade_view_launch_offset", "custom_float_0" },
		{ "projectile_gravity_scale_2", "custom_float_0" },
		{ "projectile_gravity_scale_time_2", "custom_float_0" },
		{ "projectile_air_friction", "custom_float_0" },
		{ "projectile_air_friction_final", "custom_float_0" },
		{ "projectile_gravity_scale_final", "custom_float_0" },
		{ "projectile_gravity_scale_time_final", "custom_float_0" },
		{ "projectile_air_friction_2", "custom_float_0" },
		{ "grenade_drop_to_ground_bounce_vel_frac", "titanarmor_critical_hit_required" },
		{ "grenade_drop_to_ground_on_bounce", "titanarmor_critical_hit_required" },
		{ "grenade_ignore_planted_grenades", "titanarmor_critical_hit_required" },
		{ "grenade_ignore_friendly_players", "titanarmor_critical_hit_required" },
		{ "grenade_touch_triggers_on_impact", "titanarmor_critical_hit_required" },
		{ "grenade_use_mask_ability", "titanarmor_critical_hit_required" },
		{ "projectile_predict_move_to_impact", "custom_float_0" },
		{ "projectile_muzzle_offset_decay_max_time", "custom_float_0" },
		{ "projectile_inherit_base_velocity_scale", "custom_float_0" },
		{ "show_grenade_indicator_to_owner", "titanarmor_critical_hit_required" },
		{ "show_grenade_indicator", "titanarmor_critical_hit_required" },
		{ "grenade_drop_velocity", "custom_float_0" },
		{ "reload_allow_ads", "titanarmor_critical_hit_required" },
		{ "charge_finish_primary_attack_on_cancel", "custom_float_0" },
		{ "charge_allow_anim_updates", "titanarmor_critical_hit_required" },
		{ "charge_allow_hold_when_full", "titanarmor_critical_hit_required" },
		{ "charge_attack_min_charge_required", "custom_float_0" },
		{ "charge_cooldown_time_post_fire", "custom_float_0" },
		{ "charge_delay_when_triggered_by_ADS", "custom_float_0" },
		{ "charge_curve_coefficients", "custom_float_0" },
		{ "charge_additional_damage_multiplier", "custom_float_0" },
		{ "toss_hides_world_model", "titanarmor_critical_hit_required" },
		{ "tergeting_laser_use_forward_direction", "hud_icon" },
		{ "targeting_laser_beam_length_3p_enemy", "custom_float_0" },
		{ "targeting_laser_beam_length_3p_friendly", "custom_float_0" },
		{ "targeting_laser_beam_length_1p", "custom_float_0" },
		{ "targeting_laser_range", "custom_float_0" },
		{ "targeting_laser_attachment_3p", "printname" },
		{ "targeting_laser_attachment_1p", "printname" },
		{ "targeting_laser_enabled", "titanarmor_critical_hit_required" },
		{ "deploy_allow_ads", "titanarmor_critical_hit_required" },
		{ "holster_angles_offset", "custom_float_0" },
		{ "holster_offset", "custom_float_0" },
		{ "custom_laser_sight_color_enabled", "titanarmor_critical_hit_required" },
		{ "sway_rotate_scale_zoomed", "custom_float_0" },
		{ "spread_air_ads_moving", "custom_float_0" },
		{ "spread_air_hip_moving", "custom_float_0" },
		{ "sway_rotate_scale_unzoomed", "custom_float_0" },
		{ "spread_update_hipfire_in_ads", "custom_float_0" },
		{ "spread_min_kick", "custom_float_0" },
		{ "burst_or_looping_fire_sound_resume_npc", "printname" },
		{ "burst_or_looping_fire_sound_resume_3p", "printname" },
		{ "burst_or_looping_fire_sound_resume_1p", "printname" },
		{ "sound_disabledfire", "printname" },
		{ "npc_directed_fire_ang_limit", "custom_float_0" },
		{ "ap_zoom_allowed", "custom_float_0" },
		{ "ap_optimal_range", "custom_float_0" },
		{ "ap_max_engage_range", "custom_float_0" },
		{ "ap_zoom_accuracy_hard", "custom_float_0" },
		{ "ap_zoom_accuracy_easy", "custom_float_0" },
		{ "ap_aim_accuracy_hard", "custom_float_0" },
		{ "ap_aim_accuracy_easy", "custom_float_0" },
		{ "ap_leghead_ratio_hard", "custom_float_0" },
		{ "ap_leghead_ratio_easy", "custom_float_0" },
		{ "ap_min_engage_range", "custom_float_0" },
		{ "ap_rest_time_between_bursts_max", "custom_float_0" },
		{ "ap_rest_time_between_bursts_min", "custom_float_0" },
		{ "ap_max_range_for_close_burst", "custom_float_0" },
		{ "ap_max_close_range_burst", "custom_float_0" },
		{ "ap_min_close_range_burst", "custom_float_0" },
		{ "ap_max_burst", "custom_float_0" },
		{ "ap_min_burst", "custom_float_0" },
		{ "force_zoom_in_on_activate", "titanarmor_critical_hit_required" },
		{ "sound_zoom_out_althand", "printname" },
		{ "sound_zoom_in_althand", "printname" },
		{ "anim_reuse_unarmed", "custom_float_0" },
		{ "hideForSkydive", "custom_float_0" },
		{ "object_placement_drop_to_ground_offset_max", "custom_float_0" },
		{ "object_placement_last_good_angle_max", "custom_float_0" },
		{ "object_placement_last_good_distance_max", "custom_float_0" },
		{ "object_placement_clearance_behind", "custom_float_0" },
		{ "object_placement_force_upright", "custom_float_0" },
		{ "object_placement_hill_angle_max", "custom_float_0" },
		{ "object_placement_distance_max", "custom_float_0" },
		{ "object_placer", "titanarmor_critical_hit_required" },
		{ "object_placement_ignore_players", "custom_float_0" },
		{ "object_placement_trace_through_turrets", "custom_float_0" },
		{ "object_placement_top_distance_pierce_max", "custom_float_0" },
		{ "object_placement_top_side_percent_pierce_max", "custom_float_0" },
		{ "object_placement_use_top_trace", "custom_float_0" },
		{ "object_placement_ground_penetration_max", "custom_int_0" },
		{ "object_placement_percent_off_ledge_max", "custom_float_0" },
		{ "object_placement_distance_to_ground_max", "custom_float_0" },
		{ "grenade_angle_dependant_throw_min_speed_angle", "custom_float_0" },
		{ "grenade_angle_dependant_throw_max_speed_angle", "custom_float_0" },
		{ "grenade_angle_dependant_throw_min_speed", "custom_float_0" },
		{ "object_placement_special", "custom_float_0" },
		{ "object_placement_vehicle_offset", "custom_float_0" },
		{ "challeng_req", "custom_float_0" },
		{ "threat_scope_zoomToggle_only", "custom_float_0" },
		{ "offhand_switch_slot", "custom_int_0" },
		{ "offhand_cancelled_by_melee", "titanarmor_critical_hit_required" },
		{ "energize_effect0_attachment_scope", "printname" },
		{ "energize_effect0_attachment", "printname" },
		{ "energize_activity_time", "custom_float_0" },
		{ "energized_time_consumed_per_shot", "custom_float_0" },
		{ "energized_duration", "custom_float_0" },
		{ "has_energized", "titanarmor_critical_hit_required" },
		{ "anim_stop_start_gesture_on_attack", "custom_float_0" },
		{ "object_placement_run_mode", "custom_int_0" },
		{ "ammo_pool_type", "custom_int_0" },
		{ "impact_effect_table_aliases", "printname" },
		{ "impact_effect_table", "hud_icon" },
		{ "l_trig_custom_mode", "custom_int_0" },
		{ "l_trig_custom_str", "printname" },
		{ "r_trig_custom_mode", "custom_int_0" },
		{ "r_trig_custom_str", "printname" },
		{ "custom_haptics_control", "custom_float_0" },
		{ "is_akimbo_weapon", "titanarmor_critical_hit_required" },
		{ "akimbo_weapon_flip_mouse_button_input", "custom_float_0" },
		{ "hide_when_holstered", "titanarmor_critical_hit_required" },
		{ "can_energize_when_energized", "titanarmor_critical_hit_required" },
		{ "projectile_trail_effect_1_1p", "hud_icon" },
		{ "projectile_trail_effect_0_3p", "hud_icon" },
		{ "projectile_trail_effect_0_1p", "hud_icon" },
		{ "projectile_trail_effect_4_3p", "hud_icon" },
		{ "projectile_trail_effect_4_1p", "hud_icon" },
		{ "projectile_trail_effect_3_3p", "hud_icon" },
		{ "projectile_trail_effect_3_1p", "hud_icon" },
		{ "projectile_trail_effect_2_3p", "hud_icon" },
		{ "projectile_trail_effect_2_1p", "hud_icon" },
		{ "projectile_trail_effect_1_3p", "hud_icon" },
		{ "energized_primary_attack_effect_1p", "hud_icon" },
		{ "energize_effect0_3p", "hud_icon" },
		{ "energize_effect0_1p", "hud_icon" },
		{ "targeting_laser_effect_3p_enemy", "hud_icon" },
		{ "targeting_laser_effect_3p_friendly", "hud_icon" },
		{ "targeting_laser_effect_1p", "hud_icon" },
		{ "vortex_impact_effect", "hud_icon" },
		{ "vortex_absorb_effect_third_person", "hud_icon" },
		{ "viewmodel", "hud_icon" },
		{ "zipline_station_model_wall", "hud_icon" },
		{ "zipline_station_model_ground", "hud_icon" },
		{ "fully_heated_effect_3p", "hud_icon" },
		{ "fully_heated_effect_1p", "hud_icon" },
		{ "energized_primary_attack_effect_3p", "hud_icon" },
		{ "charge_effect_burn_mod_3p", "hud_icon" },
		{ "charge_effect_burn_mod_1p", "hud_icon" },
		{ "charge_effect_3p", "hud_icon" },
		{ "charge_effect_1p", "hud_icon" },
		{ "vortex_absorb_effect", "hud_icon" },
		{ "smart_ammo_lock_effect2_3p", "hud_icon" },
		{ "smart_ammo_lock_effect2_1p", "hud_icon" },
		{ "smart_ammo_lock_effect_3p", "hud_icon" },
		{ "smart_ammo_lock_effect_1p", "hud_icon" },
		{ "player_hands_effect", "hud_icon" },
		{ "charge_effect2_3p", "hud_icon" },
		{ "charge_effect2_1p", "hud_icon" },
		{ "net_client_side_weapon_animations", "titanarmor_critical_hit_required" },
	};

	HSQUIRRELVM v = s->GetVM();
	if (!v)
		return;

	sq_startconsttable(v);
	sq_pushstring(v, "eWeaponVar", -1);
	if (sq_get(v, -2) < 0)
	{
		sq_endconsttable(v);
		Warning(eDLL_T::COMMON,
			"[s21-eweaponvar] eWeaponVar table missing; S21 names not registered\n");
		return;
	}

	HSQOBJECT enumObj;
	sq_getstackobj(v, -1, &enumObj);
	if (!sq_istable(enumObj))
	{
		sq_pop(v, 1);
		sq_endconsttable(v);
		Warning(eDLL_T::COMMON,
			"[s21-eweaponvar] eWeaponVar is not a table; S21 names not registered\n");
		return;
	}

	int added = 0;
	int nativeSkipped = 0;
	for (const EWeaponVarCompat_t& compat : compatVars)
	{
		const int r = CopyEWeaponVarSlot(v, compat);
		if (r == 1)
			++added;
		else if (r == 0)
			++nativeSkipped;
	}

	int reserved = 0;
	int reservedRedundant = 0;
	int reservedFailed = 0;
	if (bridge_weaponvar_slot_reserve.GetBool())
	{
		for (const WeaponVarSlotReservation_t& row : s_reservedWeaponVarSlots)
		{
			const EWeaponVarCompat_t compat = { row.s21Key, row.s3Slot };
			const int r = CopyEWeaponVarSlot(v, compat);
			if (r == 1)
				++reserved;
			else if (r == 0)
			{
				++reservedRedundant;
				Warning(eDLL_T::COMMON,
					"[s21-eweaponvar] reserved key '%s' already native; reservation is redundant\n",
					row.s21Key);
			}
			else
				++reservedFailed;
		}
	}

	sq_pop(v, 1);
	sq_endconsttable(v);

	Msg(eDLL_T::COMMON,
		"[s21-eweaponvar] registered %d new alias(es); protected %d native eWeaponVar(s) from clobber; %zu total\n",
		added, nativeSkipped, sizeof(compatVars) / sizeof(compatVars[0]));
	Msg(eDLL_T::COMMON,
		"[s21-eweaponvar] reserved %d slot(s), %d redundant, %d failed\n",
		reserved, reservedRedundant, reservedFailed);
}

// melee_anim_1p (S21 string) -> melee_anim_1p_number (S3 int) at parse time.
static int ResolveMeleeAnim1pNumber(const char* anim)
{
	if (!anim || !anim[0])
		return 0;
	if (strncmp(anim, "ACT_VM_MELEE_ATTACK", 19) == 0 &&
		anim[19] >= '1' && anim[19] <= '3' && anim[20] == '\0')
		return anim[19] - '0';
	return 0;
}

// Redirect S21-only settings keys onto reserved free native slots so the stock
// schema parse + mod assembly back them. Leaves the original S21 key in place
// for GetWeaponInfoFileKeyField name lookups.
static void FixupReservedWeaponVarSlots(KeyValues* kv)
{
	if (!kv || !bridge_weaponvar_slot_reserve.GetBool())
		return;

	for (const WeaponVarSlotReservation_t& row : s_reservedWeaponVarSlots)
	{
		if (!kv->FindKey(row.s21Key, false))
			continue;

		const char* const value = kv->GetString(row.s21Key, "");
		// <KEEP_DEFAULT> writes an operator the mod assembler rejects for
		// bool/string fields; forwarding it aborts assembly for the whole weapon.
		if (!value[0] || strcmp(value, "<KEEP_DEFAULT>") == 0)
			continue;

		if (kv->FindKey(row.s3Slot, false))
			continue;

		kv->SetString(row.s3Slot, value);
	}
}

// If KV has melee_anim_1p but no melee_anim_1p_number, synthesize the int. Explicit number wins.
static void FixupMeleeAnim1p(KeyValues* kv)
{
	if (!kv)
		return;

	const char* const anim = kv->GetString("melee_anim_1p", "");
	const int n = ResolveMeleeAnim1pNumber(anim);
	if (n <= 0 || kv->FindKey("melee_anim_1p_number", false))
		return;

	kv->SetInt("melee_anim_1p_number", n);
}

// Bridge activitymodifier3p onto S3 activitymodifier only. Folding 1p collides in mod assembly.
static void FixupActivityModifier(KeyValues* kv)
{
	if (!kv)
		return;

	if (kv->FindKey("activitymodifier", false))
		return; // explicit S3-native value wins, never overwritten

	const char* mod = kv->GetString("activitymodifier3p", "");
	if (!mod[0])
		mod = kv->GetString("activitymodifier3P", "");
	if (!mod[0])
		return;

	kv->SetString("activitymodifier", mod);
}

// Strip weapon_type_flags / holster_type from Mods{}: AddEntryToAssembly's whitelist omits them.
static const char* const s_unassemblableModKeys[] = {
	"weapon_type_flags",
	"holster_type",
};

static void StripUnassemblableModKeys(KeyValues* mod)
{
	if (!mod)
		return;

	for (const char* const key : s_unassemblableModKeys)
	{
		KeyValues* const sub = mod->FindKey(key, false);
		if (!sub)
			continue;

		mod->RemoveSubKey(sub);
	}
}

static void Hook_WeaponSchemaParse(__int64 a1, __int64 a2, char* a3, int a4)
{
	if (a2)
	{
		KeyValues* const kv = reinterpret_cast<KeyValues*>(a2);
		FixupMeleeAnim1p(kv); // base weapon
		FixupActivityModifier(kv); // base weapon
		FixupReservedWeaponVarSlots(kv); // base weapon

		// Per-mod overrides (proto_door_kick, energysword,...) carry their own
		// melee_anim_1p / activitymodifier1p/3p; the mod parse reads the same
		// shared KV tree downstream.
		if (KeyValues* const mods = kv->FindKey("Mods", false))
		{
			for (KeyValues* mod = mods->GetFirstTrueSubKey();
				mod != nullptr; mod = mod->GetNextTrueSubKey())
			{
				FixupMeleeAnim1p(mod);
				FixupActivityModifier(mod);
				FixupReservedWeaponVarSlots(mod);
				StripUnassemblableModKeys(mod);
			}
		}
	}

	v_WeaponSchemaParse(a1, a2, a3, a4);
}

void V_WeaponMeleeAnimFix::Detour(const bool bAttach) const
{
	DetourSetup(&v_WeaponSchemaParse, &Hook_WeaponSchemaParse, bAttach);
}

static void WeaponScriptVars_ClearHighlightMaps()
{
	s_entityHighlightTeamsServer.Clear();
	s_entityHighlightTeamsClient.Clear();
	s_entityHighlightOverridesServer.Clear();
	s_entityHighlightOverridesClient.Clear();
	s_entityGenericHighlightsServer.Clear();
	s_entityGenericHighlightsClient.Clear();
}

// Engine table has 9 entries; add "gadget" as index 9 and retarget both LEAs.
static const char* s_weaponTypeNames[] = {
	"default",
	"sidearm",
	"anti_titan",
	"melee",
	"shoulder",
	"titan_core",
	"defense",
	"tactical",
	"inventory",
	"gadget",
};
static constexpr int WEAPON_TYPE_COUNT = sizeof(s_weaponTypeNames) / sizeof(s_weaponTypeNames[0]);

// Allocated near the engine so RIP-relative LEA can reach it
static const char** s_nearTablePtr = nullptr;

static const char** AllocNearTable(void* nearAddr)
{
	if (s_nearTablePtr)
		return s_nearTablePtr;

	// Allocate within +/-2GB of nearAddr so RIP-relative offsets fit in int32
	uintptr_t base = reinterpret_cast<uintptr_t>(nearAddr);
	uintptr_t lo = (base > 0x70000000) ? base - 0x70000000 : 0x10000;
	uintptr_t hi = base + 0x70000000;

	for (uintptr_t addr = lo; addr < hi; addr += 0x10000)
	{
		void* p = VirtualAlloc(reinterpret_cast<void*>(addr),
			4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (p)
		{
			s_nearTablePtr = reinterpret_cast<const char**>(p);
			for (int i = 0; i < WEAPON_TYPE_COUNT; i++)
				s_nearTablePtr[i] = s_weaponTypeNames[i];
			// Canary at end of used region (not full 4096); spare page slack.
			HeapCanary::RegisterTail("weapon-type-table", p,
				static_cast<size_t>(WEAPON_TYPE_COUNT) * sizeof(const char*));
			return s_nearTablePtr;
		}
	}
	return nullptr;
}

static void PatchWeaponTypeLEAPair(CMemory leaPair, const char** nearTable)
{
	uint8_t* pLeaStart = reinterpret_cast<uint8_t*>(leaPair.GetPtr());
	uint8_t* pLeaEnd   = pLeaStart + 7;

	if (pLeaStart[0] != 0x4C || pLeaStart[1] != 0x8D || pLeaStart[2] != 0x1D ||
	    pLeaEnd[0]   != 0x4C || pLeaEnd[1]   != 0x8D || pLeaEnd[2]   != 0x35)
	{
		Warning(eDLL_T::SERVER, "WeaponType: LEA opcode mismatch at %p\n", pLeaStart);
		return;
	}

	uintptr_t tableStart = reinterpret_cast<uintptr_t>(&nearTable[0]);
	uintptr_t tableEnd   = reinterpret_cast<uintptr_t>(&nearTable[WEAPON_TYPE_COUNT]);

	int64_t relStart64 = static_cast<int64_t>(tableStart - (reinterpret_cast<uintptr_t>(pLeaStart) + 7));
	int64_t relEnd64   = static_cast<int64_t>(tableEnd   - (reinterpret_cast<uintptr_t>(pLeaEnd)   + 7));

	if (relStart64 > INT32_MAX || relStart64 < INT32_MIN ||
	    relEnd64   > INT32_MAX || relEnd64   < INT32_MIN)
	{
		Warning(eDLL_T::SERVER, "WeaponType: RIP offset overflow at %p\n", pLeaStart);
		return;
	}

	int32_t relStart = static_cast<int32_t>(relStart64);
	int32_t relEnd   = static_cast<int32_t>(relEnd64);

	DWORD oldProtect;
	VirtualProtect(pLeaStart, 14, PAGE_EXECUTE_READWRITE, &oldProtect);
	memcpy(pLeaStart + 3, &relStart, 4);
	memcpy(pLeaEnd   + 3, &relEnd,   4);
	VirtualProtect(pLeaStart, 14, oldProtect, &oldProtect);
}

void WeaponScriptVars_PatchWeaponTypeTable()
{
	int patched = 0;

	// CLIENT parser: 44 38 20 74 3D 4C 8D 1D ?? ?? ?? ?? 4C 8D 35
	CMemory clientMatch = Module_FindPattern(g_GameDll,
		"44 38 20 74 3D 4C 8D 1D ?? ?? ?? ?? 4C 8D 35");

	// Allocate table near engine code so RIP-relative LEA can reach it
	if (!clientMatch)
	{
		Warning(eDLL_T::SERVER, "WeaponType: CLIENT parser pattern not found\n");
		return;
	}
	void* nearAddr = reinterpret_cast<void*>(clientMatch.GetPtr());
	const char** nearTable = AllocNearTable(nearAddr);
	if (!nearTable)
	{
		Warning(eDLL_T::SERVER, "WeaponType: failed to allocate near table\n");
		return;
	}

	if (clientMatch)
		PatchWeaponTypeLEAPair(clientMatch.Offset(5), nearTable);

	// SERVER parser: 44 38 28 74 3D 4C 8D 1D ?? ?? ?? ?? 4C 8D 35
	CMemory serverMatch = Module_FindPattern(g_GameDll,
		"44 38 28 74 3D 4C 8D 1D ?? ?? ?? ?? 4C 8D 35");
	if (serverMatch)
		PatchWeaponTypeLEAPair(serverMatch.Offset(5), nearTable);

	// Count
	patched = (clientMatch ? 1 : 0) + (serverMatch ? 1 : 0);
	if (patched < 2)
		Warning(eDLL_T::SERVER, "WeaponType table: only %d of 2 parsers patched\n", patched);
}

// Cannot expand in-place (bytes after the table are another pointer array). New 10-entry table, patch two LEAs.
static const char* s_weaponTypeFlagsNames[] = {
	"WPT_PRIMARY",       // bit 0 - engine-native (preserved)
	"WPT_MELEE",         // bit 1 - engine-native (preserved)
	"WPT_TACTICAL",      // bit 2 - engine-native (preserved)
	"WPT_ULTIMATE",      // bit 3 - engine-native (preserved)
	"WPT_CONSUMABLE",    // bit 4 - engine-native (preserved)
	"WPT_INCAP_SHIELD",  // bit 5 - engine-native (preserved)
	"WPT_GRENADE",       // bit 6 - engine-native (preserved)
	"WPT_OTHER",         // bit 7 - engine-native (preserved)
	"WPT_VIEWHANDS",     // bit 8 - SDK-extended (parity, 0x100)
	"WPT_SURVIVAL",      // bit 9 - SDK-extended (parity, 0x200)
};
static constexpr int WEAPON_TYPE_FLAGS_COUNT =
	sizeof(s_weaponTypeFlagsNames) / sizeof(s_weaponTypeFlagsNames[0]);

// Separate near-alloc from the enum-table buffer so the two patches cannot step on each other.
static const char** s_nearFlagsTablePtr = nullptr;

static const char** AllocNearFlagsTable(void* nearAddr)
{
	if (s_nearFlagsTablePtr)
		return s_nearFlagsTablePtr;

	uintptr_t base = reinterpret_cast<uintptr_t>(nearAddr);
	uintptr_t lo = (base > 0x70000000) ? base - 0x70000000 : 0x10000;
	uintptr_t hi = base + 0x70000000;

	for (uintptr_t addr = lo; addr < hi; addr += 0x10000)
	{
		void* p = VirtualAlloc(reinterpret_cast<void*>(addr),
			4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (p)
		{
			s_nearFlagsTablePtr = reinterpret_cast<const char**>(p);
			for (int i = 0; i < WEAPON_TYPE_FLAGS_COUNT; i++)
				s_nearFlagsTablePtr[i] = s_weaponTypeFlagsNames[i];
			// Canary at end of used region (not full 4096); spare page slack.
			HeapCanary::RegisterTail("weapon-typeflags-table", p,
				static_cast<size_t>(WEAPON_TYPE_FLAGS_COUNT) * sizeof(const char*));
			return s_nearFlagsTablePtr;
		}
	}
	return nullptr;
}

// Patch one 7-byte LEA. Verify REX.W+LEA+modrm before write.
static bool PatchWeaponTypeFlagsLEA(uint8_t* pLea, const void* target,
	uint8_t expectedByte0, uint8_t expectedByte1, uint8_t expectedByte2,
	const char* label)
{
	if (pLea[0] != expectedByte0 ||
	    pLea[1] != expectedByte1 ||
	    pLea[2] != expectedByte2)
	{
		Warning(eDLL_T::SERVER,
			"WeaponTypeFlags: %s LEA opcode mismatch at %p (got %02X %02X %02X)\n",
			label, pLea, pLea[0], pLea[1], pLea[2]);
		return false;
	}

	int64_t rel64 = static_cast<int64_t>(
		reinterpret_cast<uintptr_t>(target)
		- (reinterpret_cast<uintptr_t>(pLea) + 7));
	if (rel64 > INT32_MAX || rel64 < INT32_MIN)
	{
		Warning(eDLL_T::SERVER,
			"WeaponTypeFlags: %s LEA RIP offset overflow at %p (delta=%lld)\n",
			label, pLea, (long long)rel64);
		return false;
	}

	int32_t rel = static_cast<int32_t>(rel64);
	DWORD oldProtect;
	VirtualProtect(pLea, 7, PAGE_EXECUTE_READWRITE, &oldProtect);
	memcpy(pLea + 3, &rel, 4);
	VirtualProtect(pLea, 7, oldProtect, &oldProtect);
	return true;
}

// Scan .text for every match (Module_FindPattern returns only the first).
static int FindAllPatternMatches(
	const char* pattern, size_t patternLen,
	uint8_t** outMatches, int maxHits)
{
	if (!outMatches || maxHits <= 0 || patternLen == 0)
		return 0;

	const CModule::ModuleSections_t& textSection =
		g_GameDll.GetSectionByName(".text");
	if (!textSection.IsSectionValid())
	{
		Warning(eDLL_T::SERVER,
			"FindAllPatternMatches: .text section not loaded\n");
		return 0;
	}

	uint8_t* const sectionBase =
		reinterpret_cast<uint8_t*>(textSection.m_pSectionBase);
	uint8_t* const sectionEnd = sectionBase + textSection.m_nSectionSize;

	int count = 0;
	uint8_t* scanStart = sectionBase;

	while (scanStart + patternLen <= sectionEnd && count < maxHits)
	{
		const size_t remaining =
			static_cast<size_t>(sectionEnd - scanStart);
		CMemory hit = CMemory(scanStart).FindPattern(
			pattern, CMemory::Direction::DOWN,
			static_cast<int>(remaining));
		if (!hit)
			break;

		uint8_t* p = reinterpret_cast<uint8_t*>(hit.GetPtr());
		// Defense-in-depth: the scanner should never return out-of-range,
		// but validate anyway in case opCodesToScan overflowed int.
		if (p < sectionBase || p + patternLen > sectionEnd)
			break;

		outMatches[count++] = p;
		scanStart = p + patternLen;  // advance strictly past this match
	}
	return count;
}

void WeaponScriptVars_PatchWeaponTypeFlagsTable()
{
	// Two GetFlagsFromString_Internal clones (client + server). FindPattern hits the client; patch both.
	static const char* const kStartPattern =
		"45 33 D2 4C 8D 0D ?? ?? ?? ?? 49 8B 01";
	static constexpr size_t kStartPatternLen = 13;

	// END LEA at function+0x67: 4C 8D 2D ?? ?? ?? ??
	static const char* const kEndPattern =
		"4C 8D 2D ?? ?? ?? ?? 0F B7 EB 8B CD 81 E9 90 00 00 00";
	static constexpr size_t kEndPatternLen = 18;

	// Expect exactly two matches per pattern. Refuse to patch if a third clone appears.
	static constexpr int kMaxSites = 4;
	uint8_t* startAnchors[kMaxSites] = {};
	uint8_t* endAnchors[kMaxSites]   = {};
	const int startCount = FindAllPatternMatches(
		kStartPattern, kStartPatternLen, startAnchors, kMaxSites);
	const int endCount = FindAllPatternMatches(
		kEndPattern,   kEndPatternLen,   endAnchors,   kMaxSites);

	if (startCount == 0 || endCount == 0)
	{
		Warning(eDLL_T::SERVER,
			"WeaponTypeFlags: pattern scan missed (start=%d, end=%d)\n",
			startCount, endCount);
		return;
	}
	if (startCount != endCount)
	{
		Warning(eDLL_T::SERVER,
			"WeaponTypeFlags: unpaired matches (start=%d, end=%d) — refusing to patch\n",
			startCount, endCount);
		return;
	}
	if (startCount != 2)
	{
		Warning(eDLL_T::SERVER,
			"WeaponTypeFlags: expected exactly 2 parser clones, found %d — refusing to patch\n",
			startCount);
		return;
	}

	// START - END must be 0x23 if the pair belongs to the same function.
	for (int i = 0; i < startCount; i++)
	{
		const ptrdiff_t delta =
			(startAnchors[i] + 3) - endAnchors[i]; // +3 skips the 45 33 D2 prefix
		if (delta != 0x23)
		{
			Warning(eDLL_T::SERVER,
				"WeaponTypeFlags: pair %d has wrong START/END delta (got 0x%tx, expected 0x23) — refusing\n",
				i, delta);
			return;
		}
	}

	// Allocate ONE shared near-table. Both parser sites are within a few
	// megabytes of each other in r5apex.exe, so a single VirtualAlloc'd
	// page within +-2GB satisfies both sets of RIP-relative displacements.
	const char** nearTable = AllocNearFlagsTable(startAnchors[0] + 3);
	if (!nearTable)
	{
		Warning(eDLL_T::SERVER,
			"WeaponTypeFlags: failed to allocate near table\n");
		return;
	}
	const void* pTableStart = &nearTable[0];
	const void* pTableEnd   = &nearTable[WEAPON_TYPE_FLAGS_COUNT];

	int patchedSites = 0;
	for (int i = 0; i < startCount; i++)
	{
		// +3 skips `45 33 D2` and lands on the 7-byte `LEA r9` opcode.
		uint8_t* pStartLea = startAnchors[i] + 3;
		uint8_t* pEndLea   = endAnchors[i];

		bool okStart = PatchWeaponTypeFlagsLEA(
			pStartLea, pTableStart, 0x4C, 0x8D, 0x0D, "start");
		bool okEnd = PatchWeaponTypeFlagsLEA(
			pEndLea,   pTableEnd,   0x4C, 0x8D, 0x2D, "end");

		if (okStart && okEnd)
			++patchedSites;
	}

	if (patchedSites != startCount)
	{
		Warning(eDLL_T::SERVER,
			"WeaponTypeFlags: only %d of %d sites patched\n",
			patchedSites, startCount);
	}
}
#endif // CLIENT_DLL
