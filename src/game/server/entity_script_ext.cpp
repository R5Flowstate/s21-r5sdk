//=============================================================================//
//
// Purpose: S21 server entity/player script natives the dedicated server
// lacks. See entity_script_ext.h.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "entity_script_ext.h"
#include "baseentity.h"
#include "entitylist.h"
#include "vscript_server.h"
#include "trigger_cannon.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "mathlib/mathlib.h"
#include "public/edict.h"
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <utility>

extern CGlobalVars* gpGlobals;

// S21 SPF value; the dedi enum leaves bit 4 free and stores the flags unmasked.
static constexpr int SPF_OBJECT_PLACEMENT_SPECIAL_IGNORE = 4;

static constexpr ptrdiff_t ESE_PLAYER_OFF_LOCALGRAVITY = 0x5B8; // half-gravity scale; 0 reads as 1
static constexpr ptrdiff_t ESE_PLAYER_OFF_SETTINGS     = 0x5DF0; // player settings asset handle
static constexpr ptrdiff_t ESE_PLAYER_OFF_CLASSMODS    = 0x5EF8; // m_classModsActive
static constexpr uint64_t  ESE_CLASSMOD_COUNT          = 64;     // m_classModsActive bits

// Server inventory. Active weapons are handles and follow a swap; these per-hand
// bytes hold backpack slot numbers and must be remapped with it.
static constexpr ptrdiff_t ESE_PLAYER_OFF_WEAPONS          = 0x1690; // m_inventory.weapons[9], EHANDLE
static constexpr ptrdiff_t ESE_PLAYER_OFF_SELECTED_WEAPONS = 0x16D8; // m_selectedWeapons[2]
static constexpr ptrdiff_t ESE_PLAYER_OFF_LATEST_NONOFFHAND = 0x16EC; // m_latestNonOffhandWeapons[2]
static constexpr ptrdiff_t ESE_PLAYER_OFF_LAST_CYCLE_SLOT  = 0x16F4; // m_lastCycleSlot
static constexpr ptrdiff_t ESE_PLAYER_OFF_INV_CHANGED_CALL = 0x1719; // m_wantInventoryChangedScriptCall
static constexpr int ESE_PRIMARY_SLOT_COUNT = 5; // WEAPON_INVENTORY_SLOT_PRIMARY_0..4; 5+ are not main weapons

static constexpr int   ESE_DISSOLVE_DEFAULT_MAGNITUDE = 500;
static constexpr float ESE_DEG_TO_RAD = 0.017453292f;
static constexpr int   ESE_MOVE_PARENT_DEPTH = 64;

// Entities flagged with SetNeverCrush( true ); either side of a push skips the crush.
static SDKEntityMap<uint8_t> s_neverCrush(ESide::Server, "entityExt.neverCrush");
static SDKEntityMap<int> s_targetingCapacity(ESide::Server, "entityExt.targetingCapacity");

static void* (*v_Pusher_Crush)(void* pPusher, void* pBlocker, bool bFlag) = nullptr;
static void (*v_CBaseEntity__Dissolve)(void* pEntity, float flStartTime, int nType,
	const Vector3D* pOrigin, int nMagnitude, bool bKill) = nullptr;
static void (*v_CBaseEntity__CalcAbsoluteVelocity)(void* pEntity) = nullptr;
static void (*v_Script_RegisterAnimatingClassFuncs)() = nullptr;
static bool (*v_PlayerSettings_FindModIndex)(uint64_t hSettings, const char* pszMod, uint64_t* pIndex) = nullptr;
static void (*v_CPlayer__SetSettingsWithMods)(void* pPlayer, uint64_t hSettings, uint64_t nMods) = nullptr;
static ScriptClassDescriptor_t* s_pAnimatingDesc = nullptr;

class CEntityScriptExtAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_vecAngVelocity;
	using CBaseEntity::m_hMoveParent;
};

static void* ESE_ScriptThis(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)))
		return nullptr;
	return pEntity;
}

static void* ESE_EntityArg(HSQUIRRELVM v, const SQInteger nIdx)
{
	const SQObjectPtr& o = stack_get(v, nIdx);
	if (o._type != OT_ENTITY || !o._unVal.pInstance)
		return nullptr;
	return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o._unVal.pInstance) + 0x50);
}

//-----------------------------------------------------------------------------
// SetNeverCrush.
//-----------------------------------------------------------------------------
static void* Hook_Pusher_Crush(void* pPusher, void* pBlocker, bool bFlag)
{
	if (s_neverCrush.Size() && (s_neverCrush.Find(pPusher) || s_neverCrush.Find(pBlocker)))
	{
		static bool s_bLogged = false;
		if (!s_bLogged)
		{
			s_bLogged = true;
			Msg(eDLL_T::SERVER, "[NEVER-CRUSH] first refused crush pusher=%p blocker=%p\n", pPusher, pBlocker);
		}
		return nullptr;
	}
	return v_Pusher_Crush(pPusher, pBlocker, bFlag);
}

static SQRESULT Script_SetNeverCrush(HSQUIRRELVM v)
{
	void* const pEntity = ESE_ScriptThis(v);
	if (!pEntity)
		return SQ_ERROR;
	SQBool bNever = SQFalse;
	sq_getbool(v, 2, &bNever);
	if (bNever)
		s_neverCrush[pEntity] = 1;
	else
		s_neverCrush.Erase(pEntity);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// GetAbsVelocityAtPoint: linear velocity plus the angular velocity of the
// entity and every move parent above it, taken at a world point.
//-----------------------------------------------------------------------------
Vector3D EntityScriptExt_AbsVelocityAtPoint(void* pEntity, const Vector3D& point)
{
	if (v_CBaseEntity__CalcAbsoluteVelocity)
		v_CBaseEntity__CalcAbsoluteVelocity(pEntity);
	Vector3D result = reinterpret_cast<CBaseEntity*>(pEntity)->Diag_AbsVelocity();

	void* pCur = pEntity;
	for (int nDepth = 0; pCur && nDepth < ESE_MOVE_PARENT_DEPTH; ++nDepth)
	{
		CEntityScriptExtAccess* const pAccess = static_cast<CEntityScriptExtAccess*>(reinterpret_cast<CBaseEntity*>(pCur));
		const Vector3D& angVel = pAccess->m_vecAngVelocity;
		if (angVel.x != 0.0f || angVel.y != 0.0f || angVel.z != 0.0f)
		{
			TriggerPass_EnsureAbsOrigin(pCur);
			const matrix3x4_t* const mat = v_CBaseEntity_EntityToWorldTransform
				? v_CBaseEntity_EntityToWorldTransform(reinterpret_cast<CBaseEntity*>(pCur))
				: nullptr;
			const Vector3D& absAngles = pAccess->Diag_AbsRotation();

			Vector3D offset;
			if ((absAngles.x == 0.0f && absAngles.y == 0.0f && absAngles.z == 0.0f) || !mat)
				offset = point - pAccess->Diag_AbsOrigin();
			else
				VectorITransform(point, *mat, offset);

			const float wx = angVel.x * ESE_DEG_TO_RAD;
			const float wy = angVel.y * ESE_DEG_TO_RAD;
			const float wz = angVel.z * ESE_DEG_TO_RAD;
			const Vector3D localRotVel(
				(offset.z * wx + 0.0f) - offset.y * wy,
				offset.x * wy + (0.0f - offset.z * wz),
				(offset.y * wz + 0.0f) - offset.x * wx);

			if (mat)
			{
				Vector3D worldRotVel;
				VectorRotate(localRotVel, *mat, worldRotVel);
				result += worldRotVel;
			}
		}

		const uint32_t nParent = static_cast<uint32_t>(pAccess->m_hMoveParent.ToInt());
		pCur = SDKEntityState_Resolve(SDKEntityHandle(nParent), ESide::Server);
	}
	return result;
}

static SQRESULT Script_GetAbsVelocityAtPoint(HSQUIRRELVM v)
{
	void* const pEntity = ESE_ScriptThis(v);
	if (!pEntity)
		return SQ_ERROR;

	const SQVector3D* pPoint = nullptr;
	if (SQ_FAILED(sq_getvector(v, 2, &pPoint)) || !pPoint)
		return SQ_ERROR;

	Vector3D vel(0.0f, 0.0f, 0.0f);
	const Vector3D point(pPoint->x, pPoint->y, pPoint->z);
	if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z))
		vel = EntityScriptExt_AbsVelocityAtPoint(pEntity, point);

	const SQVector3D out(vel.x, vel.y, vel.z);
	sq_pushvector(v, &out);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Dissolve( type, origin = <0,0,0>, magnitude = 500 ).
//-----------------------------------------------------------------------------
static SQRESULT Script_Dissolve(HSQUIRRELVM v)
{
	void* const pEntity = ESE_ScriptThis(v);
	if (!pEntity)
		return SQ_ERROR;

	SQInteger nType = 0;
	sq_getinteger(v, 2, &nType);

	Vector3D origin(0.0f, 0.0f, 0.0f);
	SQInteger nMagnitude = ESE_DISSOLVE_DEFAULT_MAGNITUDE;
	const SQInteger nTop = sq_gettop(v);
	if (nTop >= 3)
	{
		const SQVector3D* pOrigin = nullptr;
		if (SQ_SUCCEEDED(sq_getvector(v, 3, &pOrigin)) && pOrigin)
			origin = Vector3D(pOrigin->x, pOrigin->y, pOrigin->z);
	}
	if (nTop >= 4)
		sq_getinteger(v, 4, &nMagnitude);

	if (!v_CBaseEntity__Dissolve || !gpGlobals)
	{
		Warning(eDLL_T::SERVER, "[DISSOLVE] CBaseEntity::Dissolve unresolved -- entity %p not dissolved\n", pEntity);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	v_CBaseEntity__Dissolve(pEntity, gpGlobals->curTime, static_cast<int>(nType), &origin, static_cast<int>(nMagnitude), true);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Local gravity: the scale the engine's half-gravity step already reads.
//-----------------------------------------------------------------------------
static SQRESULT Script_SetLocalGravityStrength(HSQUIRRELVM v)
{
	void* const pPlayer = ESE_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;
	SQFloat flScale = 1.0f;
	sq_getfloat(v, 2, &flScale);
	if (!std::isfinite(flScale))
	{
		Warning(eDLL_T::SERVER, "[LOCAL-GRAVITY] SetLocalGravityStrength rejected non-finite value\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	*reinterpret_cast<float*>(static_cast<uint8_t*>(pPlayer) + ESE_PLAYER_OFF_LOCALGRAVITY) = flScale;
	MarkEntityEdictDirty(pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetLocalGravityStrength(HSQUIRRELVM v)
{
	void* const pPlayer = ESE_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;
	const float flScale = *reinterpret_cast<const float*>(static_cast<uint8_t*>(pPlayer) + ESE_PLAYER_OFF_LOCALGRAVITY);
	sq_pushfloat(v, flScale == 0.0f ? 1.0f : flScale);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Swaps two main-weapon backpack slots in place. The weapons keep their state
// and the one in hand stays raised.
//-----------------------------------------------------------------------------
static void ESE_RemapSlotByte(int8_t* const pSlot, const int8_t a, const int8_t b)
{
	if (*pSlot == a)
		*pSlot = b;
	else if (*pSlot == b)
		*pSlot = a;
}

static SQRESULT Script_SwapPrimaryWeaponsInSlots(HSQUIRRELVM v)
{
	void* const pPlayer = ESE_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;

	SQInteger nSlotA = -1;
	SQInteger nSlotB = -1;
	sq_getinteger(v, 2, &nSlotA);
	sq_getinteger(v, 3, &nSlotB);
	if (nSlotA < 0 || nSlotA >= ESE_PRIMARY_SLOT_COUNT || nSlotB < 0 || nSlotB >= ESE_PRIMARY_SLOT_COUNT)
	{
		Warning(eDLL_T::SERVER, "[WEAP-SWAP] SwapPrimaryWeaponsInSlots rejected slots %lld, %lld\n",
			static_cast<long long>(nSlotA), static_cast<long long>(nSlotB));
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (nSlotA == nSlotB)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	uint8_t* const p = static_cast<uint8_t*>(pPlayer);
	uint32_t* const pWeapons = reinterpret_cast<uint32_t*>(p + ESE_PLAYER_OFF_WEAPONS);
	std::swap(pWeapons[nSlotA], pWeapons[nSlotB]);

	const int8_t a = static_cast<int8_t>(nSlotA);
	const int8_t b = static_cast<int8_t>(nSlotB);
	for (int hand = 0; hand < 2; ++hand)
	{
		ESE_RemapSlotByte(reinterpret_cast<int8_t*>(p + ESE_PLAYER_OFF_SELECTED_WEAPONS + hand), a, b);
		ESE_RemapSlotByte(reinterpret_cast<int8_t*>(p + ESE_PLAYER_OFF_LATEST_NONOFFHAND + hand), a, b);
	}
	ESE_RemapSlotByte(reinterpret_cast<int8_t*>(p + ESE_PLAYER_OFF_LAST_CYCLE_SLOT), a, b);
	*(p + ESE_PLAYER_OFF_INV_CHANGED_CALL) = 1;
	MarkEntityEdictDirty(pPlayer);

	static bool s_bLogged = false;
	if (!s_bLogged)
	{
		s_bLogged = true;
		Msg(eDLL_T::SERVER, "[WEAP-SWAP] first swap player=%p slots %d <-> %d\n", pPlayer, a, b);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// AddPlayerClassMod / RemovePlayerClassMod: one class mod on the current
// settings. The client predicts the same call, so both sides change
// m_classModsActive on the same command. An unknown mod is refused with a
// warning rather than a script error.
//-----------------------------------------------------------------------------
static SQRESULT ESE_ChangeClassMod(HSQUIRRELVM v, const bool bAdd)
{
	void* const pPlayer = ESE_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;

	const SQChar* pszMod = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &pszMod)) || !pszMod || !pszMod[0])
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	if (!v_PlayerSettings_FindModIndex || !v_CPlayer__SetSettingsWithMods)
	{
		static bool s_bWarned = false;
		if (!s_bWarned)
		{
			s_bWarned = true;
			Warning(eDLL_T::SERVER, "[CLASS-MOD] %s called but the settings natives are unresolved -- mod '%s' ignored\n",
				bAdd ? "AddPlayerClassMod" : "RemovePlayerClassMod", pszMod);
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	uint8_t* const p = static_cast<uint8_t*>(pPlayer);
	const uint64_t hSettings = *reinterpret_cast<const uint64_t*>(p + ESE_PLAYER_OFF_SETTINGS);
	uint64_t nIndex = ESE_CLASSMOD_COUNT;
	if (!hSettings || !v_PlayerSettings_FindModIndex(hSettings, pszMod, &nIndex) || nIndex >= ESE_CLASSMOD_COUNT)
	{
		Warning(eDLL_T::SERVER, "[CLASS-MOD] undefined mod '%s' for player %p\n", pszMod, pPlayer);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const uint64_t nMods = *reinterpret_cast<const uint64_t*>(p + ESE_PLAYER_OFF_CLASSMODS);
	const uint64_t nBit = 1ull << nIndex;
	const uint64_t nNew = bAdd ? (nMods | nBit) : (nMods & ~nBit);
	if (nNew != nMods)
		v_CPlayer__SetSettingsWithMods(pPlayer, hSettings, nNew);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_AddPlayerClassMod(HSQUIRRELVM v)
{
	return ESE_ChangeClassMod(v, true);
}

static SQRESULT Script_RemovePlayerClassMod(HSQUIRRELVM v)
{
	return ESE_ChangeClassMod(v, false);
}

//-----------------------------------------------------------------------------
// Bleedout bookkeeping. These run engine-side match bookkeeping on the S21
// server that has no counterpart here; the calls are accepted and validated.
//-----------------------------------------------------------------------------
static void ESE_LogFirstCall(bool& bLogged, const char* pszNative, const void* pEntity)
{
	if (bLogged)
		return;
	bLogged = true;
	Msg(eDLL_T::SERVER, "[BLEEDOUT-NATIVE] %s first call entity=%p (no engine bookkeeping on this server)\n",
		pszNative, pEntity);
}

static SQRESULT Script_OnStartBleedout(HSQUIRRELVM v)
{
	void* const pPlayer = ESE_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;
	static bool s_bLogged = false;
	ESE_LogFirstCall(s_bLogged, "OnStartBleedout", pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_OverrideTargetingCapacity(HSQUIRRELVM v)
{
	void* const pPlayer = ESE_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;
	SQInteger nCapacity = 0;
	sq_getinteger(v, 2, &nCapacity);
	s_targetingCapacity[pPlayer] = static_cast<int>(nCapacity);
	static bool s_bLogged = false;
	ESE_LogFirstCall(s_bLogged, "OverrideTargetingCapacity", pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_ClearTargetingCapacityOverride(HSQUIRRELVM v)
{
	void* const pPlayer = ESE_ScriptThis(v);
	if (!pPlayer)
		return SQ_ERROR;
	s_targetingCapacity.Erase(pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_GameRules_OnPlayerGotDowned(HSQUIRRELVM v)
{
	static bool s_bLogged = false;
	ESE_LogFirstCall(s_bLogged, "GameRules_OnPlayerGotDowned", ESE_EntityArg(v, 2));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_GameRules_OnPlayerGotRevived(HSQUIRRELVM v)
{
	static bool s_bLogged = false;
	ESE_LogFirstCall(s_bLogged, "GameRules_OnPlayerGotRevived", ESE_EntityArg(v, 2));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Registration.
//-----------------------------------------------------------------------------
void EntityScriptExt_RenameBinding(ScriptClassDescriptor_t* pDesc, const char* pszName, const char* pszNewName)
{
	if (!pDesc)
		return;
	for (CUtlVector<ScriptFunctionBinding_t>* pVec : { &pDesc->m_NumTypedFunctions, &pDesc->m_StrTypedFunctions })
	{
		for (int i = 0; i < pVec->Count(); ++i)
		{
			ScriptFunctionBinding_t& b = (*pVec)[i];
			if (b.m_Descriptor.m_ScriptName && strcmp(b.m_Descriptor.m_ScriptName, pszName) == 0)
				b.m_Descriptor.m_ScriptName = pszNewName;
		}
	}
}

// CBaseAnimating owns the engine Dissolve; the replacement takes optional arguments.
static void Hook_Script_RegisterAnimatingClassFuncs(void)
{
	v_Script_RegisterAnimatingClassFuncs();

	static bool s_bRegistered = false;
	if (s_bRegistered)
		return;
	s_bRegistered = true;

	ScriptClassDescriptor_t* const d = s_pAnimatingDesc;
	if (!d || !d->m_ScriptName || strcmp(d->m_ScriptName, "CBaseAnimating") != 0)
	{
		Warning(eDLL_T::SERVER, "[DISSOLVE] CBaseAnimating descriptor not found -- Dissolve keeps its three required arguments\n");
		return;
	}
	EntityScriptExt_RenameBinding(d, "Dissolve", "Dissolve_Engine");
	d->AddFunction("Dissolve", "Script_Dissolve",
		"Dissolve this entity", "void",
		"int dissolveType, vector dissolveOrigin = null, int magnitude = 500", false, Script_Dissolve);
}

void EntityScriptExt_RegisterEntityFunctions(ScriptClassDescriptor_t* entityStruct)
{
	if (!entityStruct)
		return;

	entityStruct->AddFunction("SetNeverCrush", "Script_SetNeverCrush",
		"When true, pushes involving this entity never crush", "void", "bool neverCrush",
		false, Script_SetNeverCrush);
	entityStruct->AddFunction("GetAbsVelocityAtPoint", "Script_GetAbsVelocityAtPoint",
		"Velocity of this entity (including its move parents' rotation) at a world point", "vector",
		"vector point", false, Script_GetAbsVelocityAtPoint);
}

void EntityScriptExt_RegisterPlayerFunctions(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	EntityScriptExt_RenameBinding(playerStruct, "GetLocalGravityStrength", "GetLocalGravityStrength_Engine");
	playerStruct->AddFunction("SetLocalGravityStrength", "Script_SetLocalGravityStrength",
		"Scales gravity for this player", "void", "float strength", false, Script_SetLocalGravityStrength);
	playerStruct->AddFunction("GetLocalGravityStrength", "Script_GetLocalGravityStrength",
		"Gravity scale for this player", "float", "", false, Script_GetLocalGravityStrength);
	playerStruct->AddFunction("OnStartBleedout", "Script_OnStartBleedout",
		"Notifies code that this player started bleeding out", "void", "entity weapon",
		false, Script_OnStartBleedout);
	playerStruct->AddFunction("OverrideTargetingCapacity", "Script_OverrideTargetingCapacity",
		"Overrides how many AI may target this player", "void", "int capacity",
		false, Script_OverrideTargetingCapacity);
	playerStruct->AddFunction("ClearTargetingCapacityOverride", "Script_ClearTargetingCapacityOverride",
		"Clears OverrideTargetingCapacity", "void", "", false, Script_ClearTargetingCapacityOverride);
	playerStruct->AddFunction("SwapPrimaryWeaponsInSlots", "Script_SwapPrimaryWeaponsInSlots",
		"Swaps the main weapons in two backpack slots; the weapon in hand stays raised", "void",
		"int slotA, int slotB", false, Script_SwapPrimaryWeaponsInSlots);
	playerStruct->AddFunction("AddPlayerClassMod", "Script_AddPlayerClassMod",
		"Adds one class mod to the player's current settings", "void", "string mod",
		false, Script_AddPlayerClassMod);
	playerStruct->AddFunction("RemovePlayerClassMod", "Script_RemovePlayerClassMod",
		"Removes one class mod from the player's current settings", "void", "string mod",
		false, Script_RemovePlayerClassMod);
}

void EntityScriptExt_RegisterServerFunctions(CSquirrelVM* s)
{
	if (!s)
		return;

	Script_RegisterFuncNamed(s, "GameRules_OnPlayerGotDowned", "Server_Script_GameRules_OnPlayerGotDowned",
		"Match bookkeeping when a player is downed", "void", "entity player, entity attacker",
		false, ServerScript_GameRules_OnPlayerGotDowned);
	Script_RegisterFuncNamed(s, "GameRules_OnPlayerGotRevived", "Server_Script_GameRules_OnPlayerGotRevived",
		"Match bookkeeping when a player is revived", "void", "entity player, entity reviver",
		false, ServerScript_GameRules_OnPlayerGotRevived);
}

void EntityScriptExt_RegisterScriptConstants(CSquirrelVM* s)
{
	if (!s)
		return;
	s->RegisterConstant("SPF_OBJECT_PLACEMENT_SPECIAL_IGNORE", SPF_OBJECT_PLACEMENT_SPECIAL_IGNORE);
}

void VEntityScriptExt::GetAdr(void) const
{
	LogFunAdr("Pusher_Crush", v_Pusher_Crush);
	LogFunAdr("CBaseEntity::Dissolve", v_CBaseEntity__Dissolve);
	LogFunAdr("CBaseEntity::CalcAbsoluteVelocity", v_CBaseEntity__CalcAbsoluteVelocity);
	LogFunAdr("Script_RegisterAnimatingClassFuncs", v_Script_RegisterAnimatingClassFuncs);
	LogVarAdr("g_serverScriptAnimatingStruct", s_pAnimatingDesc);
	LogFunAdr("PlayerSettings_FindModIndex", v_PlayerSettings_FindModIndex);
	LogFunAdr("EntityScriptExt_SetSettingsWithMods", v_CPlayer__SetSettingsWithMods);
}

void VEntityScriptExt::GetFun(void) const
{
	// Crush a blocked entity: pushed entity's owner (vtable +0x298) resolved first.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 41 56 48 81 EC C0 00 00 00 "
		"48 8B 02 48 8B F1 48 8B CA 41 0F B6 E8 48 8B DA FF 90 98 02 00 00 45 33 F6")
		.GetPtr(v_Pusher_Crush);
	// Dissolve(startTime, type, origin, magnitude, kill); EFL_KILLME test at +0x230.
	Module_FindPattern(g_GameDll,
		"40 56 57 41 56 48 83 EC 40 F6 81 37 02 00 00 01 49 8B F1 0F 29 74 24 30 48 8B F9 0F 28 F1")
		.GetPtr(v_CBaseEntity__Dissolve);
	Module_FindPattern(g_GameDll,
		"4C 8B DC 56 48 81 EC 80 00 00 00 8B 81 30 02 00 00 48 8B F1 C1 E8 0C A8 01 0F 84")
		.GetPtr(v_CBaseEntity__CalcAbsoluteVelocity);

	// CBaseAnimating registrar: "Animating models" description, then the class name store.
	const CMemory registrar = Module_FindPattern(g_GameDll,
		"48 83 EC 28 80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 48 89 5C 24 30 48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
		"48 8D 15 ?? ?? ?? ?? 48 89 6C 24 38 48 8D 05 ?? ?? ?? ?? 48 89 74 24 40 48 89 05 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? "
		"48 89 7C 24 48 4C 89 74 24 20 45 33 F6 48 89 05 ?? ?? ?? ?? 41 8B CE B0 43");
	registrar.GetPtr(v_Script_RegisterAnimatingClassFuncs);
	if (registrar)
		registrar.FindPattern("48 89 15", CMemory::Direction::DOWN, 0x80)
			.ResolveRelativeAddressSelf(0x3, 0x7).GetPtr(s_pAnimatingDesc);

	// Settings handle + mod name -> index into the settings' mod list.
	Module_FindPattern(g_GameDll,
		"48 89 74 24 10 57 44 8B D1 48 8B FA 41 81 E2 FF FF 03 00 48 8D 15")
		.GetPtr(v_PlayerSettings_FindModIndex);
	// Stores the settings handle and m_classModsActive, then reapplies the class settings.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F8 48 8B EA 48 8B D9 48 39 91 F0 5D 00 00")
		.GetPtr(v_CPlayer__SetSettingsWithMods);
	if (!v_PlayerSettings_FindModIndex || !v_CPlayer__SetSettingsWithMods)
		Warning(eDLL_T::SERVER, "[CLASS-MOD] settings natives unresolved (find=%p set=%p) -- "
			"AddPlayerClassMod/RemovePlayerClassMod do nothing\n",
			reinterpret_cast<void*>(v_PlayerSettings_FindModIndex), reinterpret_cast<void*>(v_CPlayer__SetSettingsWithMods));

	if (!v_Script_RegisterAnimatingClassFuncs || !s_pAnimatingDesc)
		Warning(eDLL_T::SERVER, "[DISSOLVE] CBaseAnimating registrar unresolved -- Dissolve keeps its three required arguments\n");
	if (!v_Pusher_Crush)
		Warning(eDLL_T::SERVER, "[NEVER-CRUSH] crush pattern unresolved -- SetNeverCrush has no effect\n");
	if (!v_CBaseEntity__Dissolve)
		Warning(eDLL_T::SERVER, "[DISSOLVE] CBaseEntity::Dissolve pattern unresolved\n");
	if (!v_CBaseEntity__CalcAbsoluteVelocity)
		Warning(eDLL_T::SERVER, "[ABSVEL] CalcAbsoluteVelocity pattern unresolved -- GetAbsVelocityAtPoint reads the cached velocity\n");
}

void VEntityScriptExt::Detour(const bool bAttach) const
{
	if (v_Pusher_Crush)
		DetourSetup(&v_Pusher_Crush, &Hook_Pusher_Crush, bAttach);
	if (v_Script_RegisterAnimatingClassFuncs && s_pAnimatingDesc)
		DetourSetup(&v_Script_RegisterAnimatingClassFuncs, &Hook_Script_RegisterAnimatingClassFuncs, bAttach);
}
