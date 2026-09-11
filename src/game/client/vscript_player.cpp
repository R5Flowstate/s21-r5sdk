#if defined(CLIENT_DLL)
// No client-side player script code. The S21 client binds these natives itself and
// the SDK registration path that reached this half no longer exists.

#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: Player script functions (combat, shields, stance, offhand,
// skydive, skyward)
//
//=============================================================================//

#include "core/stdafx.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/globalnonrewind_vars.h"
#include "game/shared/deathfield_system.h"
#include "game/shared/weapon_script_vars.h"
#include "public/edict.h"
#include "public/const.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/scriptremotefunctions_shared.h"
#include "game/shared/offhand_slots_ext.h"
#include "game/shared/offhand_activation_patches.h"
#include "game/server/util_server.h"
#include "game/server/entitylist.h"
#include "game/server/player_launch.h"
#include "game/shared/dt_extend.h"     // ConnQuality_* (connection quality natives)
#include "game/shared/player_extend_sidecar.h"
#include "vscript_player.h"

#include <unordered_map>
#include <vector>

// This translation unit is compiled into game_shared_static (linked into both client.dll
// and dedicated). In dedicated, only the server-side gpGlobals symbol (CGlobalVars*)
// is available, so we declare against CGlobalVars which inherits CGlobalVarsBase.
extern CGlobalVars* gpGlobals;

//=============================================================================
// Player field offsets
//=============================================================================
// Read off the live DT_Player builder, which
// names m_duckState at 0x65F0. The 0x1A7C / 0x2AExx / 0x2CAC values belong to a
// dead second CPlayer layout in the same binary -- do not write those.
static constexpr int CPLAYER_M_PLAYERFLAGS_OFFSET = 0x6128;
// m_forceStance (2=crouch, 1=stand).
static constexpr int CPLAYER_M_FORCESTANCE_OFFSET = 0x5AAC;
static constexpr int CPLAYER_M_LASTTIMEDAMAGED_BYPLAYER_OFFSET = 0x69C8;
static constexpr int CPLAYER_M_LASTTIMEDAMAGED_BYNPC_OFFSET = 0x69CC;
static constexpr int CPLAYER_M_LASTTIMEDID_DAMAGE_TO_PLAYER_OFFSET = 0x69D0;
static constexpr int CPLAYER_M_LASTTIMEDID_DAMAGE_TO_NPC_OFFSET = 0x69D4;
// Server CPlayer freefall fields; C_Player twin in this binary sits 0x3AC8 lower.
static constexpr int CPLAYER_M_FREEFALLSTATE_OFFSET = 0x7B60;
static constexpr int CPLAYER_M_FREEFALLSTARTTIME_OFFSET = 0x7B64;

// EHandle-keyed so respawn/map reuse cannot sticky-true a recycled pointer.
static SDKEntityMap<bool> s_dediTeleportingPlayers(ESide::Server, "teleporting.srv");

static constexpr int CPLAYER_M_SKYDIVESPEED_OFFSET       = 0x7B80;
static constexpr int CPLAYER_M_SKYDIVEPLAYERYAW_OFFSET   = 0x7B9C;

//=============================================================================
// Skydive aliases (mapped to freefall fields)
//=============================================================================
static SQRESULT Script_Player_IsSkydiving(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	int state = *(int*)((uintptr_t)pPlayer + CPLAYER_M_FREEFALLSTATE_OFFSET);
	sq_pushbool(v, state != 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_IsSkydiveAnticipating(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	// 0 = not diving, 1 = diving, 2 = anticipating the landing.
	int state = *(int*)((uintptr_t)pPlayer + CPLAYER_M_FREEFALLSTATE_OFFSET);
	sq_pushbool(v, state == 2);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_GetSkydiveStartTime(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	float startTime = *(float*)((uintptr_t)pPlayer + CPLAYER_M_FREEFALLSTARTTIME_OFFSET);
	sq_pushfloat(v, startTime);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: reads back the skydive speed the engine seeded when freefall began, so
//          the simulation continues from it instead of inventing a start value
//-----------------------------------------------------------------------------
static SQRESULT Script_Skydive_GetSpeed(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	sq_pushfloat(v, *reinterpret_cast<const float*>(
		reinterpret_cast<uintptr_t>(pPlayer) + CPLAYER_M_SKYDIVESPEED_OFFSET));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: the seeded skydive yaw -- the facing freefall started from
//-----------------------------------------------------------------------------
static SQRESULT Script_Skydive_GetPlayerYaw(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	sq_pushfloat(v, *reinterpret_cast<const float*>(
		reinterpret_cast<uintptr_t>(pPlayer) + CPLAYER_M_SKYDIVEPLAYERYAW_OFFSET));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// Dedicated PlayerLaunch -- latch + wire, consumed in FullWalkMove
//=============================================================================
static SQRESULT Script_DediPlayerLaunch(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	const SQVector3D* launchVelocity = nullptr;
	if (SQ_FAILED(sq_getvector(v, 2, &launchVelocity)) || !launchVelocity)
		return SQ_ERROR;

	SQBool lock3pRotation = false;
	if (sq_gettop(v) >= 3)
		sq_getbool(v, 3, &lock3pRotation);

	if (v && v->GetContext() != SQCONTEXT::SERVER)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	PlayerLaunch_Latch(pPlayer,
		launchVelocity->x, launchVelocity->y, launchVelocity->z,
		lock3pRotation != 0);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_DediStartTeleport(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	s_dediTeleportingPlayers[pPlayer] = true;
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_DediEndTeleport(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	s_dediTeleportingPlayers.Erase(pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_DediIsTeleporting(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	sq_pushbool(v, s_dediTeleportingPlayers.Find(pPlayer) != nullptr);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Zero-offset downcast to reach protected CBaseEntity parent/name fields.
class Script_PlayerVehFieldAccess : public CBaseEntity
{
public:
	using CBaseEntity::m_iClassname;
	using CBaseEntity::m_hMoveParent;
	using CBaseEntity::m_scriptName;
};

static void* Script_DediResolveEHandle(const EHANDLE& h)
{
	const uint32_t raw = static_cast<uint32_t>(h.ToInt());
	if (raw == INVALID_EHANDLE_INDEX)
		return nullptr;

	if (void* const pResolved = SDKEntityState_Resolve(SDKEntityHandle(raw), ESide::Server))
		return pResolved;

	if (!g_serverEntityList)
		return nullptr;

	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(raw));
	if (void* const pEntity = g_serverEntityList->LookupEntity(handle))
		return pEntity;

	const int entIndex = static_cast<int>(raw & ENT_ENTRY_MASK);
	if (entIndex >= 0 && entIndex < NUM_ENT_ENTRIES)
		return g_serverEntityList->LookupEntityByNetworkIndex(entIndex);

	return nullptr;
}

static bool Script_NameSuggestsVehicle(const char* psz)
{
	if (!psz || !psz[0])
		return false;
	if (V_stristr(psz, "hover_vehicle"))
		return true;
	if (V_stristr(psz, "player_vehicle"))
		return true;
	return false;
}

static bool Script_EntityLooksLikeVehicle(CBaseEntity* pEnt)
{
	if (!pEnt)
		return false;

	auto* const acc = static_cast<Script_PlayerVehFieldAccess*>(pEnt);
	if (Script_NameSuggestsVehicle(acc->m_scriptName))
		return true;
	if (!acc->m_iClassname)
		return false;
	return Script_NameSuggestsVehicle(STRING(acc->m_iClassname));
}

static SQRESULT Script_IsPlayerInAnyVehicle(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	CPlayer* const pCPlayer = reinterpret_cast<CPlayer*>(pPlayer);

	// Primary: driven/occupied vehicle handle on the player.
	const EHANDLE& veh = pCPlayer->GetPlayerVehicle();
	if (veh.IsValid() && Script_DediResolveEHandle(veh))
	{
		sq_pushbool(v, true);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// Fallback: parented under a hover_vehicle / player_vehicle.
	auto* const self = static_cast<Script_PlayerVehFieldAccess*>(
		static_cast<CBaseEntity*>(pCPlayer));
	if (self->m_hMoveParent.IsValid())
	{
		CBaseEntity* const pParent = reinterpret_cast<CBaseEntity*>(
			Script_DediResolveEHandle(self->m_hMoveParent));
		if (Script_EntityLooksLikeVehicle(pParent))
		{
			sq_pushbool(v, true);
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
	}

	sq_pushbool(v, false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static CPlayer* Script_DediResolvePlayer(void* pEntity)
{
	if (!pEntity || !gpGlobals)
		return nullptr;

	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CPlayer* const pPlayer = UTIL_PlayerByIndex(i);
		if (pPlayer == pEntity)
			return pPlayer;
	}

	return nullptr;
}

static void Script_DediWritePlayerTime(CPlayer* pPlayer, const int offset, const float time)
{
	if (!pPlayer)
		return;

	*reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(pPlayer) + offset) = time;
	MarkEntityEdictDirty(pPlayer);
}

static SQRESULT Script_DediUpdateLastTimeDamaged(HSQUIRRELVM v)
{
	void* pVictim = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pVictim)) || !pVictim)
		return SQ_ERROR;

	void* pAttacker = nullptr;
	if (sq_gettop(v) >= 2)
		v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pAttacker));

	const float curTime = gpGlobals ? gpGlobals->curTime : 0.0f;
	CPlayer* const pVictimPlayer = reinterpret_cast<CPlayer*>(pVictim);
	CPlayer* const pAttackerPlayer = Script_DediResolvePlayer(pAttacker);

	if (pAttackerPlayer)
	{
		if (pAttackerPlayer != pVictimPlayer)
		{
			Script_DediWritePlayerTime(pVictimPlayer, CPLAYER_M_LASTTIMEDAMAGED_BYPLAYER_OFFSET, curTime);
			Script_DediWritePlayerTime(pAttackerPlayer, CPLAYER_M_LASTTIMEDID_DAMAGE_TO_PLAYER_OFFSET, curTime);
		}
	}
	else if (pAttacker)
	{
		Script_DediWritePlayerTime(pVictimPlayer, CPLAYER_M_LASTTIMEDAMAGED_BYNPC_OFFSET, curTime);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// Skyward stubs (jump towers / Valk ult - no engine support)
//=============================================================================
// These are reached from shared script that runs every frame in places (ping,
// tracking vision), so each announces itself once and then stays quiet.
static void Skydive_AnnounceStubOnce(bool& bAnnounced, const char* const pszName)
{
	if (bAnnounced)
		return;

	bAnnounced = true;
	DevMsg(eDLL_T::SERVER, "[SKYDIVE-STUB] %s has no engine backing -- always false\n", pszName);
}

static SQRESULT Script_Player_IsSkywardLaunching(HSQUIRRELVM v)
{
	static bool s_bAnnounced = false;
	Skydive_AnnounceStubOnce(s_bAnnounced, "Player_IsSkywardLaunching");
	sq_pushbool(v, false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_IsSkywardFollowing(HSQUIRRELVM v)
{
	static bool s_bAnnounced = false;
	Skydive_AnnounceStubOnce(s_bAnnounced, "Player_IsSkywardFollowing");
	sq_pushbool(v, false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Player_IsSkywardDiving(HSQUIRRELVM v)
{
	static bool s_bAnnounced = false;
	Skydive_AnnounceStubOnce(s_bAnnounced, "Player_IsSkywardDiving");
	sq_pushbool(v, false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Skydive_IsFromUpdraft(HSQUIRRELVM v)
{
	static bool s_bAnnounced = false;
	Skydive_AnnounceStubOnce(s_bAnnounced, "Skydive_IsFromUpdraft");
	sq_pushbool(v, false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Skydive_IsFromSkywardLaunch(HSQUIRRELVM v)
{
	static bool s_bAnnounced = false;
	Skydive_AnnounceStubOnce(s_bAnnounced, "Skydive_IsFromSkywardLaunch");
	sq_pushbool(v, false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// Connection quality. Index 0 (best) to 5 (worst / not connected yet).
//=============================================================================
static SQRESULT Script_GetConnectionQualityIndex(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	int index = 5;
	ConnQuality_GetForPlayer(pPlayer, &index);

	sq_pushinteger(v, index);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetConnectionLatencyMS(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	float latencyMs = 0.0f;
	ConnQuality_GetNetStatsForPlayer(pPlayer, &latencyMs, nullptr);

	sq_pushfloat(v, latencyMs);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetConnectionPacketLoss(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	float lossPct = 0.0f;
	ConnQuality_GetNetStatsForPlayer(pPlayer, nullptr, &lossPct);

	sq_pushfloat(v, lossPct);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetConnectionQualityIndex(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger index = 0;
	sq_getinteger(v, 2, &index);

	ConnQuality_SetOverrideForPlayer(pPlayer, static_cast<int>(index));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_ClearConnectionQualityOverride(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	ConnQuality_ClearOverrideForPlayer(pPlayer);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetLastTimeDamaged(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	uintptr_t base = reinterpret_cast<uintptr_t>(pPlayer);
	float byPlayer = *(float*)(base + CPLAYER_M_LASTTIMEDAMAGED_BYPLAYER_OFFSET);
	float byNPC = *(float*)(base + CPLAYER_M_LASTTIMEDAMAGED_BYNPC_OFFSET);
	float result = (byPlayer > byNPC) ? byPlayer : byNPC;

	sq_pushfloat(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// Forced stance stack system
// Engine values: 0 = no force, 1 = force stand, 2 = force crouch
//=============================================================================
struct ForcedStanceEntry_t
{
	int handle;
	int stanceType;
};

struct PlayerStanceStack_t
{
	std::vector<ForcedStanceEntry_t> stack;
	int nextHandle = 1;
};

// Sibling maps per side -- EHandle values can collide across allocators.
static SDKEntityMap<PlayerStanceStack_t> s_stanceStacksServer(ESide::Server, "stanceStacks.srv");
static SDKEntityMap<PlayerStanceStack_t> s_stanceStacksClient(ESide::Client, "stanceStacks.cli");

static SDKEntityMap<PlayerStanceStack_t>& GetStanceStacksMap(HSQUIRRELVM v)
{
	return (v && v->GetContext() == SQCONTEXT::SERVER) ? s_stanceStacksServer
	                                                   : s_stanceStacksClient;
}

static void UpdateForceStanceField(void* pPlayer, PlayerStanceStack_t& ss)
{
	int engineValue = 0;
	if (!ss.stack.empty())
	{
		int scriptStance = ss.stack.back().stanceType;
		engineValue = scriptStance + 1;
	}
	*(int*)((uintptr_t)pPlayer + CPLAYER_M_FORCESTANCE_OFFSET) = engineValue;
	// m_forceStance is networked; dirty-mark or the change may not replicate this tick.
	MarkEntityEdictDirty(pPlayer);
}

static SQRESULT Script_PushForcedStance(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger stanceType;
	sq_getinteger(v, 2, &stanceType);

	if (stanceType < 0 || stanceType > 1)
	{
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	auto& ss = GetStanceStacksMap(v)[pPlayer];
	int handle = ss.nextHandle++;

	ForcedStanceEntry_t entry;
	entry.handle = handle;
	entry.stanceType = static_cast<int>(stanceType);
	ss.stack.push_back(entry);

	UpdateForceStanceField(pPlayer, ss);

	sq_pushinteger(v, handle);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_RemoveForcedStance(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger handle;
	sq_getinteger(v, 2, &handle);

	PlayerStanceStack_t* pSS = GetStanceStacksMap(v).Find(pPlayer);
	if (pSS)
	{
		auto& stack = pSS->stack;
		for (auto sit = stack.begin(); sit != stack.end(); ++sit)
		{
			if (sit->handle == static_cast<int>(handle))
			{
				stack.erase(sit);
				break;
			}
		}
		UpdateForceStanceField(pPlayer, *pSS);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// Extra shield system -- DT-native replication
//
static SQRESULT Script_GetExtraShieldHealth(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	sq_pushinteger(v, PlayerExtend_GetI32(pPlayer, offsetof(PlayerExtendWire, m_extraShieldHealth)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetTempshieldHealth(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	sq_pushinteger(v, PlayerExtend_GetI32(pPlayer, offsetof(PlayerExtendWire, m_tempShieldHealth)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetExtraShieldTier(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	sq_pushinteger(v, PlayerExtend_GetI32(pPlayer, offsetof(PlayerExtendWire, m_extraShieldTier)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetExtraShieldHealth(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger val;
	sq_getinteger(v, 2, &val);
	if (val < 0) val = 0;

	if (pPlayer)
	{
		PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_extraShieldHealth), static_cast<int>(val));
		MarkEntityEdictDirty(pPlayer);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetTempshieldHealth(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger val;
	sq_getinteger(v, 2, &val);
	if (val < 0) val = 0;

	if (pPlayer)
	{
		PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_tempShieldHealth), static_cast<int>(val));
		MarkEntityEdictDirty(pPlayer);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetExtraShieldTier(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger val;
	sq_getinteger(v, 2, &val);
	if (val < 0 || val > 1023)
		val = (val < 0) ? 0 : 1023;

	if (pPlayer)
	{
		PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_extraShieldTier), static_cast<int>(val));
		MarkEntityEdictDirty(pPlayer);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// IsConnectionActive
//=============================================================================
static SQRESULT Script_IsConnectionActive(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	const int playerFlags = *reinterpret_cast<int*>(
		reinterpret_cast<char*>(pPlayer) + CPLAYER_M_PLAYERFLAGS_OFFSET);

	sq_pushbool(v, (playerFlags & 2) == 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// Shield change source tracking
//=============================================================================
static constexpr int SHIELD_HISTORY_SIZE = 16;
static constexpr int SHIELD_SOURCE_COUNT = 2;

struct ShieldChangeEntry_t
{
	float time = 0.0f;
	float newShieldHealth = 0.0f;
	int changePerSource[SHIELD_SOURCE_COUNT] = {};
};

struct PlayerShieldHistory_t
{
	ShieldChangeEntry_t history[SHIELD_HISTORY_SIZE] = {};
	int currentIdx = 0;
};

// Sibling maps per side (same rationale as s_stanceStacks above).
static SDKEntityMap<PlayerShieldHistory_t> s_shieldHistoryServer(ESide::Server, "shieldHistory.srv");
static SDKEntityMap<PlayerShieldHistory_t> s_shieldHistoryClient(ESide::Client, "shieldHistory.cli");

static SDKEntityMap<PlayerShieldHistory_t>& GetShieldHistoryMap(HSQUIRRELVM v)
{
	return (v && v->GetContext() == SQCONTEXT::SERVER) ? s_shieldHistoryServer
	                                                   : s_shieldHistoryClient;
}

static SQRESULT Script_SetShieldHealthFromSource(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger newHealth;
	sq_getinteger(v, 2, &newHealth);

	SQInteger source;
	sq_getinteger(v, 3, &source);

	if (source < 0 || source >= SHIELD_SOURCE_COUNT)
		source = 0;

	auto& hist = GetShieldHistoryMap(v)[pPlayer];
	int idx = hist.currentIdx % SHIELD_HISTORY_SIZE;
	hist.history[idx].time = gpGlobals ? gpGlobals->curTime : 0.0f;
	hist.history[idx].newShieldHealth = static_cast<float>(newHealth);
	hist.history[idx].changePerSource[0] = 0;
	hist.history[idx].changePerSource[1] = 0;
	hist.history[idx].changePerSource[source] = 1;
	hist.currentIdx++;

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsMostRecentShieldChangeFromSingleSource(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger sourceType;
	sq_getinteger(v, 2, &sourceType);

	SQInteger curShieldAmount;
	sq_getinteger(v, 3, &curShieldAmount);

	SQFloat timeThreshold;
	sq_getfloat(v, 4, &timeThreshold);

	const PlayerShieldHistory_t* pHist = GetShieldHistoryMap(v).Find(pPlayer);
	if (!pHist)
	{
		sq_pushbool(v, true);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const auto& hist = *pHist;
	const float curTime = gpGlobals ? gpGlobals->curTime : 0.0f;
	const float targetHealth = static_cast<float>(curShieldAmount);

	int startIdx = (hist.currentIdx + SHIELD_HISTORY_SIZE - 1) % SHIELD_HISTORY_SIZE;
	for (int n = 0; n < SHIELD_HISTORY_SIZE; n++)
	{
		int idx = (startIdx - n + SHIELD_HISTORY_SIZE) % SHIELD_HISTORY_SIZE;
		const auto& entry = hist.history[idx];

		if (entry.newShieldHealth != targetHealth)
			continue;

		float elapsed = curTime - entry.time;
		if (elapsed > static_cast<float>(timeThreshold) || elapsed < 0.0f)
			continue;

		for (int s = 0; s < SHIELD_SOURCE_COUNT; s++)
		{
			if (s != static_cast<int>(sourceType) && entry.changePerSource[s] != 0)
			{
				sq_pushbool(v, false);
				SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
			}
		}
	}

	sq_pushbool(v, true);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// TrySelectOffhand
//=============================================================================
static SQRESULT Script_TrySelectOffhand(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	SQInteger offhandIndex;
	sq_getinteger(v, 2, &offhandIndex);

	if (offhandIndex < 0 || offhandIndex > 7)
	{
		Warning(eDLL_T::SERVER, "TrySelectOffhand: offhand index %d is not valid\n",
			static_cast<int>(offhandIndex));
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// Dispatcher outer gate is test [player+0x60DC], 0x07F00000. The command
	// byte is a local-input singleton and does not drive dedicated think.
	static const uint32_t kOffhandButtonBit[8] = {
		0x00100000u, 0x00200000u, 0x00400000u, 0x00800000u,
		0x01000000u, 0x00000000u, 0x02000000u, 0x04000000u
	};
	uint32_t* const pButtons = reinterpret_cast<uint32_t*>(
		static_cast<uint8_t*>(pPlayer) + 0x60DC);
	*pButtons |= kOffhandButtonBit[offhandIndex];

	if (g_pOffhandCommandByte)
		*g_pOffhandCommandByte = static_cast<uint8_t>(offhandIndex);

	OffhandSlotsExt_RequestSelect(pPlayer, static_cast<int>(offhandIndex));

	if (OffhandActivation_TossDiagEnabled())
	{
		static volatile LONG s_trySelectLog = 0;
		const LONG n = InterlockedIncrement(&s_trySelectLog);
		if (n <= 8)
			Msg(eDLL_T::SERVER,
				"[OFFHAND-EXT] TrySelectOffhand slot=%d buttons=0x%X (#%d)\n",
				static_cast<int>(offhandIndex), *pButtons, static_cast<int>(n));
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//=============================================================================
// Registration
//=============================================================================
void Script_RegisterPlayerScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	// Slots 6/7 cross the wire as native DT props (offhandWeapons resized 6->8).
	OffhandSlotsExt_Register(playerStruct);

	playerStruct->AddFunction(
		"IsConnectionActive",
		"Script_IsConnectionActive",
		"Returns true if the player's network connection is active",
		"bool",
		"",
		false,
		Script_IsConnectionActive);

	playerStruct->AddFunction(
		"TrySelectOffhand",
		"Script_TrySelectOffhand",
		"Requests the engine to select an offhand weapon by index",
		"void",
		"int offhandIndex",
		false,
		Script_TrySelectOffhand);

	playerStruct->AddFunction(
		"DeathFieldIndex",
		"Script_DeathFieldIndex",
		"Gets the deathfield index for this player",
		"int",
		"",
		false,
		Script_DeathFieldIndex);

	playerStruct->AddFunction(
		"SetDeathFieldIndex",
		"Script_SetDeathFieldIndex",
		"Sets the deathfield index for this player",
		"void",
		"int deathFieldIndex",
		false,
		Script_SetDeathFieldIndex);

	playerStruct->AddFunction(
		"SetShieldHealthFromSource",
		"Script_SetShieldHealthFromSource",
		"Sets shield health and records the change source",
		"void",
		"int newHealth, int source",
		false,
		Script_SetShieldHealthFromSource);

	playerStruct->AddFunction(
		"IsMostRecentShieldChangeFromSingleSource",
		"Script_IsMostRecentShieldChangeFromSingleSource",
		"Checks if most recent shield change matching the amount came from a single source",
		"bool",
		"int sourceType, int currentShieldHealth, float timeDelta",
		false,
		Script_IsMostRecentShieldChangeFromSingleSource);

	playerStruct->AddFunction(
		"GetExtraShieldHealth", "Script_GetExtraShieldHealth",
		"Gets the player's extra shield health", "int", "", false,
		Script_GetExtraShieldHealth);

	playerStruct->AddFunction(
		"GetExtraShieldTier", "Script_GetExtraShieldTier",
		"Gets the player's extra shield tier", "int", "", false,
		Script_GetExtraShieldTier);

	playerStruct->AddFunction(
		"GetTempshieldHealth", "Script_GetTempshieldHealth",
		"Gets the player's temp shield health", "int", "", false,
		Script_GetTempshieldHealth);

	playerStruct->AddFunction(
		"SetExtraShieldHealth", "Script_SetExtraShieldHealth",
		"Sets the player's extra shield health", "void", "int value", false,
		Script_SetExtraShieldHealth);

	playerStruct->AddFunction(
		"SetTempshieldHealth", "Script_SetTempshieldHealth",
		"Sets the player's temp shield health", "void", "int value", false,
		Script_SetTempshieldHealth);

	playerStruct->AddFunction(
		"SetExtraShieldTier", "Script_SetExtraShieldTier",
		"Sets the player's extra shield tier", "void", "int value", false,
		Script_SetExtraShieldTier);

	playerStruct->AddFunction(
		"PushForcedStance", "Script_PushForcedStance",
		"Forces player into a stance, returns handle for removal", "int", "int stanceType", false,
		Script_PushForcedStance);

	playerStruct->AddFunction(
		"RemoveForcedStance", "Script_RemoveForcedStance",
		"Removes a previously pushed forced stance by handle", "void", "int handle", false,
		Script_RemoveForcedStance);

	playerStruct->AddFunction(
		"GetLastTimeDamaged", "Script_GetLastTimeDamaged",
		"Returns the last time the player was damaged by any source", "float", "", false,
		Script_GetLastTimeDamaged);

	playerStruct->AddFunction(
		"GetNonRewindRespawnTime", "Script_GetNonRewindRespawnTime",
		"Gets the non-rewind respawn time for this player", "float", "", false,
		Script_GetNonRewindRespawnTime);

	playerStruct->AddFunction(
		"GetNonRewindMusicPack", "Script_GetNonRewindMusicPack",
		"Gets the non-rewind music pack for this player", "int", "", false,
		Script_GetNonRewindMusicPack);

	playerStruct->AddFunction(
		"GetConnectionQualityIndex", "Script_GetConnectionQualityIndex",
		"Returns this player's connection quality, 0 (best) to 5 (worst)", "int", "", false,
		Script_GetConnectionQualityIndex);

	playerStruct->AddFunction(
		"GetConnectionLatencyMS", "Script_GetConnectionLatencyMS",
		"Returns this player's average outgoing netchan latency in milliseconds", "float", "", false,
		Script_GetConnectionLatencyMS);

	playerStruct->AddFunction(
		"GetConnectionPacketLoss", "Script_GetConnectionPacketLoss",
		"Returns this player's average incoming netchan packet loss as a percentage", "float", "", false,
		Script_GetConnectionPacketLoss);

	playerStruct->AddFunction(
		"Player_IsSkydiving", "Script_Player_IsSkydiving",
		"Returns true if the player is skydiving", "bool", "", false,
		Script_Player_IsSkydiving);

	playerStruct->AddFunction(
		"Player_IsSkydiveAnticipating", "Script_Player_IsSkydiveAnticipating",
		"Returns true if the player is about to land from skydive", "bool", "", false,
		Script_Player_IsSkydiveAnticipating);

	playerStruct->AddFunction(
		"Player_GetSkydiveStartTime", "Script_Player_GetSkydiveStartTime",
		"Returns the time skydive started", "float", "", false,
		Script_Player_GetSkydiveStartTime);

	playerStruct->AddFunction(
		"Skydive_GetSpeed", "Script_Skydive_GetSpeed",
		"Returns the skydive speed the engine is holding for this player", "float", "", false,
		Script_Skydive_GetSpeed);

	playerStruct->AddFunction(
		"Skydive_GetPlayerYaw", "Script_Skydive_GetPlayerYaw",
		"Returns the skydive yaw the engine is holding for this player", "float", "", false,
		Script_Skydive_GetPlayerYaw);

	playerStruct->AddFunction(
		"Player_IsSkywardLaunching", "Script_Player_IsSkywardLaunching",
		"Returns true if the player is skyward launching", "bool", "", false,
		Script_Player_IsSkywardLaunching);

	playerStruct->AddFunction(
		"Player_IsSkywardFollowing", "Script_Player_IsSkywardFollowing",
		"Returns true if the player is following a skyward launch", "bool", "", false,
		Script_Player_IsSkywardFollowing);

	playerStruct->AddFunction(
		"Player_IsSkywardDiving", "Script_Player_IsSkywardDiving",
		"Returns true if the player is skyward diving", "bool", "", false,
		Script_Player_IsSkywardDiving);

	playerStruct->AddFunction(
		"Skydive_IsFromUpdraft", "Script_Skydive_IsFromUpdraft",
		"Returns true if skydive was triggered by an updraft", "bool", "", false,
		Script_Skydive_IsFromUpdraft);

	playerStruct->AddFunction(
		"Skydive_IsFromSkywardLaunch", "Script_Skydive_IsFromSkywardLaunch",
		"Returns true if skydive was triggered by a skyward launch", "bool", "", false,
		Script_Skydive_IsFromSkywardLaunch);
}

void Script_RegisterDedicatedPlayerScriptFunctions(ScriptClassDescriptor_t* playerStruct)
{
	playerStruct->AddFunction(
		"PlayerLaunch",
		"Script_DediPlayerLaunch",
		"Launches the player with the supplied velocity",
		"void",
		"vector launchVelocity, bool lock3pRotation",
		false,
		Script_DediPlayerLaunch);

	playerStruct->AddFunction(
		"StartTeleport",
		"Script_DediStartTeleport",
		"Marks the player as teleporting for dedicated scripts",
		"void",
		"",
		false,
		Script_DediStartTeleport);

	playerStruct->AddFunction(
		"EndTeleport",
		"Script_DediEndTeleport",
		"Clears the player's teleporting state for dedicated scripts",
		"void",
		"",
		false,
		Script_DediEndTeleport);

	playerStruct->AddFunction(
		"IsTeleporting",
		"Script_DediIsTeleporting",
		"Returns true while the player is marked as teleporting",
		"bool",
		"",
		false,
		Script_DediIsTeleporting);

	playerStruct->AddFunction(
		"IsPlayerInAnyVehicle",
		"Script_IsPlayerInAnyVehicle",
		"Returns true if the player is in or parented to a vehicle",
		"bool",
		"",
		false,
		Script_IsPlayerInAnyVehicle);

	playerStruct->AddFunction(
		"UpdateLastTimeDamaged",
		"Script_DediUpdateLastTimeDamaged",
		"Updates the player damage timestamp buckets from a damage attacker",
		"void",
		"entity attacker",
		false,
		Script_DediUpdateLastTimeDamaged);
}

void Script_RegisterPlayerScriptSetters(ScriptClassDescriptor_t* playerStruct)
{
	playerStruct->AddFunction(
		"SetNonRewindRespawnTime", "Script_SetNonRewindRespawnTime",
		"Sets the non-rewind respawn time for this player", "void", "float time", false,
		Script_SetNonRewindRespawnTime);

	playerStruct->AddFunction(
		"SetNonRewindMusicPack", "Script_SetNonRewindMusicPack",
		"Sets the non-rewind music pack for this player", "void", "int pack", false,
		Script_SetNonRewindMusicPack);

	playerStruct->AddFunction(
		"SetConnectionQualityIndex", "Script_SetConnectionQualityIndex",
		"Overrides this player's replicated connection quality index (0-5)", "void", "int index", false,
		Script_SetConnectionQualityIndex);

	playerStruct->AddFunction(
		"ClearConnectionQualityOverride", "Script_ClearConnectionQualityOverride",
		"Drops a SetConnectionQualityIndex override and resumes netchan tracking", "void", "", false,
		Script_ClearConnectionQualityOverride);
}

void VScriptPlayer_LevelShutdown()
{
	// Defense-in-depth — SDKEntityState_FlushAll already clears these via the
	// destroy subscription, but this matches the rest of the LevelShutdown
	// pattern and is harmless after the flush.
	s_stanceStacksServer.Clear();
	s_stanceStacksClient.Clear();
	s_shieldHistoryServer.Clear();
	s_shieldHistoryClient.Clear();
	s_dediTeleportingPlayers.Clear();
}

// Late-join burst -- no-op now that DT replication carries the values
// natively. Kept as a stub so the engine call site in client.cpp doesn't
// need updating; DT's own FULL-snapshot path handles late joiners.
void VScriptPlayer_SendExtraShieldInitialState(const CPlayer* /*pTarget*/)
{
}
#endif // CLIENT_DLL
