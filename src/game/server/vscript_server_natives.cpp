//=============================================================================//
//
// Purpose: Cafe dedicated server-VM natives (Register* helpers)
//
//=============================================================================//

#include "core/stdafx.h"
#include "common/callback.h"
#include "game/shared/scriptnetdata_limits.h"
#include "engine/server/server.h"
#include "engine/server/sv_main.h"
#include "engine/host_state.h"
#include "engine/debugoverlay.h"
#include "pluginsystem/pluginsystem.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"

#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/globalnonrewind_vars.h"
#include "game/shared/weapon_heat.h"
#include "game/server/energize.h"
#include "game/shared/deathfield_system.h"
#include "game/shared/alliance_compat.h"
#include "game/shared/highlight_context.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"
#include "public/const.h"

#include "game/shared/vscript_shared.h"
#include "game/shared/vscript_debug_overlay_shared.h"

#include "vscript_server.h"
#include "vscript_server_natives.h"
#include "classvar_natives.h"
#include "vscript_server_placement.h"
#include "player.h"
#include "util_server.h"
#include "entitylist.h"
#include "detour_impl.h"
#include "game/shared/weapon_script_vars.h"
#include "game/server/jetdrive.h"
#include "game/server/track_entity.h"
#include "game/server/trigger_updraft.h"
#include "game/server/skydive.h"
#include "game/server/player_overheat.h"
#include "game/server/translocation.h"
#include "game/server/headglitch_detect.h"
#include "game/shared/status_effects_sdk.h"
#include "game/shared/util_shared.h"
#include "game/client/vscript_player.h"
#include "game/shared/vscript_remotefunctions_sdk.h"
#include "engine/enginetrace.h"
#include "engine/modelloader.h"
#include "engine/server/precache_natives.h"
#include "public/bspflags.h"
#include "tier1/keyvalues.h"
#include "tier1/convar.h"
#include "tier1/cvar.h"
#include "tier2/curlutils.h"
#include "ebisusdk/EbisuSDK.h"
#include "game/server/sound.h"
#include "vscript/languages/squirrel_re/include/sqarray.h"

#include <atomic>
#include <cfloat>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

bool ServerScript_IsDedicatedRuntime(void)
{
    return (g_pServer && g_pServer->IsDedicated()) || (s_bIsDedicated && *s_bIsDedicated);
}

static SQRESULT ServerScript_PlacementTraceForMoverBlockingShim(HSQUIRRELVM v)
{
    const SQVector3D* origin = nullptr;
    const SQVector3D* normal = nullptr;
    if (SQ_FAILED(sq_getvector(v, 2, &origin)) || !origin ||
        SQ_FAILED(sq_getvector(v, 3, &normal)) || !normal)
    {
        sq_pushbool(v, false);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    // Third script arg (mover entity) is unused; the check is a hull trace at the exit.
    const Vector3D pos(origin->x, origin->y, origin->z);
    const Vector3D norm(normal->x, normal->y, normal->z);
    const bool blocked = ServerScript_TraceForMoverBlocking(pos, norm, nullptr);
    sq_pushbool(v, blocked);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_GetPlacementSpecialOrientationFromAnglesShim(HSQUIRRELVM v)
{
    const SQVector3D* normal = nullptr;
    if (SQ_FAILED(sq_getvector(v, 2, &normal)) || !normal)
    {
        sq_pushinteger(v, 0);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    sq_pushinteger(v, ServerScript_ClassifyPortalDir(Vector3D(normal->x, normal->y, normal->z)));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Walks all clients and clears attackers whose lunge target is the subject.
//-----------------------------------------------------------------------------
static ConVar bridge_lunge_clear_log("bridge_lunge_clear_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log PlayerMelee_ClearPlayerAsLungeTarget invocations (0=silent after first, 1=every call).");

void* ServerScript_EntityPtrFromStackIdx(HSQUIRRELVM v, SQInteger sqIdx)
{
	const SQObjectPtr& o = stack_get(v, sqIdx);
	if (sq_isnull(o))
		return nullptr;
	if (o._type != OT_ENTITY || !o._unVal.pInstance)
		return nullptr;

	// SQ entity instance userdata: CBaseEntity* at +0x50, the same field the VM's
	// own instance accessor returns. +0x38 is an unrelated instance field.
	return *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(o._unVal.pInstance) + 0x50);
}

static void* ServerScript_ResolveLungeHandle(const uint32_t rawHandle)
{
	if (rawHandle == INVALID_EHANDLE_INDEX || !g_serverEntityList)
		return nullptr;

	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
	if (void* const pEntity = g_serverEntityList->LookupEntity(handle))
		return pEntity;

	const int entIndex = static_cast<int>(rawHandle & ENT_ENTRY_MASK);
	if (entIndex >= 0 && entIndex < NUM_ENT_ENTRIES)
		return g_serverEntityList->LookupEntityByNetworkIndex(entIndex);

	return nullptr;
}

static void ServerScript_ClearLungeState(CPlayer* const player)
{
	if (!player)
		return;

	player->ClearMeleeLungeState();
	MarkEntityEdictDirty(player);
}

// CBaseEntity networked-flag bit 25; replicated through DT_BaseEntity m_networkedFlags.
static constexpr int BENF_PERMANENT_ENTITY = 0x02000000;

// CBaseEntity networked-flag bit 28; replicated through DT_BaseEntity m_networkedFlags.
static constexpr int BENF_ALLOW_OBJECT_PLACEMENT = 0x10000000;

// CBaseEntity networked-flag bit 30; replicated through DT_BaseEntity m_networkedFlags.
static constexpr int BENF_CAN_BE_MELEED_BY_OWNER = 0x40000000;

// CBaseEntity networked-flag bit 10; replicated through DT_BaseEntity m_networkedFlags.
// Set while the client must skip this entity in aim assist target search.
static constexpr int BENF_AIM_ASSIST_IGNORED = 0x00000400;

static void (*v_CBaseEntity_SetNetworkedFlag)(void* pEntity, bool bSet, int nMask) = nullptr;
static bool s_bSetNetworkedFlagResolved = false;

static void ServerScript_ResolveSetNetworkedFlag(void)
{
	if (s_bSetNetworkedFlagResolved)
		return;
	s_bSetNetworkedFlagResolved = true;

	// Wildcards mask the rip-relative error-path lea/jmp; the literal
	// mov eax, [rcx+0D8h] load of m_networkedFlags anchors the signature.
	Module_FindPattern(g_GameDll,
		"4C 8B C9 48 85 C9 75 0C 48 8D 0D ?? ?? ?? ?? E9 ?? ?? ?? ?? "
		"8B 81 D8 00 00 00 84 D2 74 39 44 8B D0 45 0B D0 41 3B C2")
		.GetPtr(v_CBaseEntity_SetNetworkedFlag);

	if (!v_CBaseEntity_SetNetworkedFlag)
	{
		Warning(eDLL_T::SERVER,
			"[PERM-ENT] CBaseEntity::SetNetworkedFlag pattern unresolved -- "
			"SetIsPermanentEntity is a no-op\n");
	}
	else
	{
		Msg(eDLL_T::SERVER,
			"[PERM-ENT] CBaseEntity::SetNetworkedFlag resolved at %p\n",
			reinterpret_cast<void*>(v_CBaseEntity_SetNetworkedFlag));
	}
}

static ConVar bridge_permanent_entity_log("bridge_permanent_entity_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log SetIsPermanentEntity invocations (0=first call only, 1=every call).");

//-----------------------------------------------------------------------------
// Sets/clears BENF_PERMANENT_ENTITY. The engine helper dirty-marks the edict.
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_SetIsPermanentEntity(HSQUIRRELVM v)
{
	ServerScript_ResolveSetNetworkedFlag();

	if (!v_CBaseEntity_SetNetworkedFlag)
	{
		static bool s_bStubWarned = false;
		if (!s_bStubWarned)
		{
			s_bStubWarned = true;
			Warning(eDLL_T::SERVER,
				"[PERM-ENT] SetIsPermanentEntity stub -- native unresolved\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// Free function: entity @2, bool isPermanent @3. Stack helper is primary; v_sq_getentity is fallback.
	void* pEntity = ServerScript_EntityPtrFromStackIdx(v, 2);
	if (!pEntity)
	{
		if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		{
			static bool s_bNullWarned = false;
			if (!s_bNullWarned)
			{
				s_bNullWarned = true;
				Warning(eDLL_T::SERVER, "[PERM-ENT] null entity -- SetIsPermanentEntity skipped\n");
			}
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
	}

	SQBool isPermanent = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 3, &isPermanent)))
		isPermanent = SQFalse;

	v_CBaseEntity_SetNetworkedFlag(pEntity, isPermanent != SQFalse, BENF_PERMANENT_ENTITY);

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce || bridge_permanent_entity_log.GetBool())
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[PERM-ENT] entity=%p isPermanent=%d\n",
			pEntity, isPermanent ? 1 : 0);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetCanBeMeleedByOwner(HSQUIRRELVM v)
{
	ServerScript_ResolveSetNetworkedFlag();

	if (!v_CBaseEntity_SetNetworkedFlag)
	{
		static bool s_bStubWarned = false;
		if (!s_bStubWarned)
		{
			s_bStubWarned = true;
			Warning(eDLL_T::SERVER,
				"[SHADOW-FORM] SetCanBeMeleedByOwner -- SetNetworkedFlag native unresolved, no-op\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		return SQ_ERROR;

	SQBool bCanMelee = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bCanMelee)))
		return SQ_ERROR;

	v_CBaseEntity_SetNetworkedFlag(pEnt, bCanMelee != SQFalse, BENF_CAN_BE_MELEED_BY_OWNER);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static ConVar bridge_aimassist_log("bridge_aimassist_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log SetAimAssistAllowed invocations (0=first call only, 1=every call).");

//-----------------------------------------------------------------------------
// entity.SetAimAssistAllowed(bool) -- clears the client-tested ignore bit when
// allowed, sets it when disallowed. The engine helper dirty-marks the edict.
//-----------------------------------------------------------------------------
static SQRESULT Script_SetAimAssistAllowed(HSQUIRRELVM v)
{
	ServerScript_ResolveSetNetworkedFlag();

	if (!v_CBaseEntity_SetNetworkedFlag)
	{
		static bool s_bStubWarned = false;
		if (!s_bStubWarned)
		{
			s_bStubWarned = true;
			Warning(eDLL_T::SERVER,
				"[AIM-ASSIST] SetAimAssistAllowed -- SetNetworkedFlag native unresolved, no-op\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		return SQ_ERROR;

	SQBool bAllowed = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bAllowed)))
		return SQ_ERROR;

	v_CBaseEntity_SetNetworkedFlag(pEnt, bAllowed == SQFalse, BENF_AIM_ASSIST_IGNORED);

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce || bridge_aimassist_log.GetBool())
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[AIM-ASSIST] entity=%p allowed=%d\n",
			pEnt, bAllowed ? 1 : 0);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_DisallowObjectPlacement(HSQUIRRELVM v)
{
	ServerScript_ResolveSetNetworkedFlag();

	if (!v_CBaseEntity_SetNetworkedFlag)
	{
		static bool s_bStubWarned = false;
		if (!s_bStubWarned)
		{
			s_bStubWarned = true;
			Warning(eDLL_T::SERVER,
				"[SHADOW-FORM] DisallowObjectPlacement -- SetNetworkedFlag native unresolved, no-op\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		return SQ_ERROR;

	v_CBaseEntity_SetNetworkedFlag(pEnt, false, BENF_ALLOW_OBJECT_PLACEMENT);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static int s_nShadowShieldWriteLogCount = 0;

//-----------------------------------------------------------------------------
// player.SetShadowShieldIsActive( bool ) -- publishes DT_Player m_shadowShieldActive.
//-----------------------------------------------------------------------------
static SQRESULT Script_SetShadowShieldIsActive(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	SQBool bActive = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bActive)))
		return SQ_ERROR;

	const int nValue = (bActive != SQFalse) ? 1 : 0;
	if (PlayerExtend_GetI32(pPlayer, offsetof(PlayerExtendWire, m_shadowShieldActive)) != nValue)
	{
		PlayerExtend_SetI32(pPlayer, offsetof(PlayerExtendWire, m_shadowShieldActive), nValue);
		MarkEntityEdictDirty(pPlayer);
	}

	if (s_nShadowShieldWriteLogCount < 16)
	{
		++s_nShadowShieldWriteLogCount;
		Msg(eDLL_T::SERVER,
			"[SHADOW-FORM] SetShadowShieldIsActive player=%p value=%d\n",
			pPlayer, nValue);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// No fortified-form flag on this client; this native changes no state.
// The shield visual is driven by SetShadowShieldIsActive.
static SQRESULT Script_EnterShadowFormFortified(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	static bool s_bFortifiedWarned = false;
	if (!s_bFortifiedWarned)
	{
		s_bFortifiedWarned = true;
		Warning(eDLL_T::SERVER,
			"[SHADOW-FORM] EnterShadowFormFortified -- fortified state has no consumer "
			"on this engine pair; call intentionally changes no state. Shield visual "
			"comes from SetShadowShieldIsActive\n");
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// CPlayer+0x69B0 is the server-half slip-trigger occupancy count (16 handles
// at +0x6970, count immediately after). Same predicate as S21 IsSlipping.
static constexpr ptrdiff_t S3_CPLAYER_SLIP_TRIGGER_COUNT = 0x69B0;

static SQRESULT Script_IsSlipping(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer)
		return SQ_ERROR;

	const int nCount = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(pPlayer) + S3_CPLAYER_SLIP_TRIGGER_COUNT);
	sq_pushbool(v, nCount > 0 ? SQTrue : SQFalse);

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce)
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[SLIP] IsSlipping first call player=%p count=%d\n",
			pPlayer, nCount);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// entity.SetIgnoreMoveParentRotation() -- one-way, matches the S21 setter.
static SQRESULT Script_SetIgnoreMoveParentRotation(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		return SQ_ERROR;

	DTExtend_SetIgnoreParentRotation(pEnt, true);
	MarkEntityEdictDirty(pEnt);

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce)
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[IGN-PARENT-ROT] SetIgnoreMoveParentRotation ent=%p\n", pEnt);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// entity.Anim_EnableRelativeToGround() -- one-way; the engine's Anim_EnableCollision
// clears it, matching how that native clears planting.
static SQRESULT Script_Anim_EnableRelativeToGround(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
		return SQ_ERROR;

	DTExtend_SetAnimRelativeToGround(pEnt, true);
	MarkEntityEdictDirty(pEnt);

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce)
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[ANIM-RTG] Anim_EnableRelativeToGround ent=%p\n", pEnt);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_PlayerMeleeClearLungeTargetShim(HSQUIRRELVM v)
{
	// Free function: entity player @2, bool clearSelf @3 (not instance this@1).
	CPlayer* pSubject = reinterpret_cast<CPlayer*>(ServerScript_EntityPtrFromStackIdx(v, 2));
	if (!pSubject)
	{
		// Fallback if stack object is a bound instance the OT_ENTITY path missed.
		if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pSubject)) || !pSubject)
		{
			static bool s_bNullWarned = false;
			if (!s_bNullWarned)
			{
				s_bNullWarned = true;
				Warning(eDLL_T::SERVER, "[LUNGE-CLR] null player -- clear skipped\n");
			}
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
	}

	SQBool clearSelf = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 3, &clearSelf)))
		clearSelf = SQFalse;

	static bool s_bLoggedOnce = false;
	if (!s_bLoggedOnce || bridge_lunge_clear_log.GetBool())
	{
		s_bLoggedOnce = true;
		Msg(eDLL_T::SERVER, "[LUNGE-CLR] subject=%p clearSelf=%d\n",
			pSubject, clearSelf ? 1 : 0);
	}

	int nCleared = 0;
	const int nMaxClients = (gpGlobals) ? gpGlobals->maxClients : 0;
	for (int i = 0; i < nMaxClients; ++i)
	{
		// Gate on active CClient first -- UTIL_PlayerByIndex on empty slots
		// returns a bogus low address (weapon_enforce / physics_main pattern).
		if (!g_pServer)
			break;

		const CClient* const pClient = g_pServer->GetClient(i);
		if (!pClient || !pClient->IsActive())
			continue;

		CPlayer* const pAttacker = UTIL_PlayerByIndex(pClient->GetHandle());
		if (!pAttacker || pAttacker == pSubject)
			continue;
		if (!pAttacker->HasLungeTargetEntity())
			continue;

		void* const pTarget = ServerScript_ResolveLungeHandle(
			pAttacker->GetLungeTargetEntityHandle());
		if (pTarget != pSubject)
			continue;

		ServerScript_ClearLungeState(pAttacker);
		++nCleared;
	}

	if (clearSelf)
	{
		ServerScript_ClearLungeState(pSubject);
		++nCleared;
	}

	if (bridge_lunge_clear_log.GetBool())
	{
		Msg(eDLL_T::SERVER, "[LUNGE-CLR] cleared=%d (clearSelf=%d)\n",
			nCleared, clearSelf ? 1 : 0);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// EmitSoundAtPositionExceptToPlayer -- unique prologue pattern (single match).
static float (*v_EmitSoundAtPositionExceptToPlayer)(int team, const Vector3D* pos,
	CBaseEntity* exceptPlayer, const char* sound) = nullptr;
static bool s_bEmitSoundAtPosExceptResolved = false;

static void ServerScript_ResolveEmitSoundAtPositionExcept(void)
{
	if (s_bEmitSoundAtPosExceptResolved)
		return;
	s_bEmitSoundAtPosExceptResolved = true;

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 7C 24 ?? 55 41 56 41 57 48 8D 6C 24 ?? "
		"48 81 EC ?? ?? ?? ?? 49 8B D9")
		.GetPtr(v_EmitSoundAtPositionExceptToPlayer);

	if (!v_EmitSoundAtPositionExceptToPlayer)
	{
		Warning(eDLL_T::SERVER,
			"[WHIZBY] EmitSoundAtPositionExceptToPlayer pattern unresolved -- "
			"EmitWhizbySoundExceptToPlayer is a no-op\n");
	}
	else
	{
		Msg(eDLL_T::SERVER,
			"[WHIZBY] EmitSoundAtPositionExceptToPlayer resolved at %p\n",
			reinterpret_cast<void*>(v_EmitSoundAtPositionExceptToPlayer));
	}
}

static SQRESULT ServerScript_EmitWhizbySoundExceptToPlayer(HSQUIRRELVM v)
{
	ServerScript_ResolveEmitSoundAtPositionExcept();

	if (!v_EmitSoundAtPositionExceptToPlayer)
	{
		static bool s_bStubWarned = false;
		if (!s_bStubWarned)
		{
			s_bStubWarned = true;
			Warning(eDLL_T::SERVER,
				"[WHIZBY] EmitWhizbySoundExceptToPlayer stub -- native unresolved\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	SQInteger team = 0;
	sq_getinteger(v, 2, &team);

	CBaseEntity* pExcept = reinterpret_cast<CBaseEntity*>(
		ServerScript_EntityPtrFromStackIdx(v, 3));

	const SQVector3D* pEp0 = nullptr;
	const SQVector3D* pEp1 = nullptr;
	const SQChar* pszSound = nullptr;
	if (SQ_FAILED(sq_getvector(v, 4, &pEp0)) || !pEp0 ||
		SQ_FAILED(sq_getvector(v, 5, &pEp1)) || !pEp1 ||
		SQ_FAILED(sq_getstring(v, 6, &pszSound)) || !pszSound || !*pszSound)
	{
		static bool s_bArgWarned = false;
		if (!s_bArgWarned)
		{
			s_bArgWarned = true;
			Warning(eDLL_T::SERVER,
				"[WHIZBY] bad args (need team, exceptPlayer, ep0, ep1, sound)\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// Mid-point of the segment -- matches S21 whizby placement.
	const Vector3D mid(
		(pEp0->x + pEp1->x) * 0.5f,
		(pEp0->y + pEp1->y) * 0.5f,
		(pEp0->z + pEp1->z) * 0.5f);

	v_EmitSoundAtPositionExceptToPlayer(
		static_cast<int>(team), &mid, pExcept, pszSound);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Safe-spot natives and multi-exclude emit. Engine pointers resolved lazily.
//-----------------------------------------------------------------------------
// m_vecAbsOrigin on CBaseEntity (three consecutive floats).
static constexpr ptrdiff_t kEntOffAbsOrigin = 0x450;
// int16 edict index; 0xFFFF means no edict (engine normalises to 0).
static constexpr ptrdiff_t kEntOffEdictIndex = 0x58;
// MyPlayerPointer vtable byte offset (index 93).
static constexpr size_t kVtableOffMyPlayerPointer = 744;

static bool (*v_PutEntityInSafeSpot)(CBaseEntity* pEntity, CBaseEntity* pReferenceEnt,
	CBaseEntity* pGroundEnt, const float* pSafeStartPos, const float* pEndPos) = nullptr;
static void (*v_CBaseEntity_SetAbsOrigin)(CBaseEntity* pEntity, const float* pAbsOrigin) = nullptr;
static bool s_bSafeSpotFnsResolved = false;

static void ServerScript_ResolveSafeSpotFns(void)
{
	if (s_bSafeSpotFnsResolved)
		return;
	s_bSafeSpotFnsResolved = true;

	// Stack-frame immediates pin the server twin; do not shorten this pattern.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 20 55 56 57 41 56 41 57 48 8D AC 24 20 F7 FF FF 48 81 EC E0 09 00 00 48 8B 01")
		.GetPtr(v_PutEntityInSafeSpot);

	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 57 48 8D 68 A1 48 81 EC F0 00 00 00 0F 29 70 D8 48 8B D9 0F 29 78 C8")
		.GetPtr(v_CBaseEntity_SetAbsOrigin);

	if (!v_PutEntityInSafeSpot)
	{
		Warning(eDLL_T::SERVER,
			"[SAFESPOT] PutEntityInSafeSpot pattern unresolved -- "
			"PutEntityInSomewhatSafeSpot / CanPutPlayerInSafeSpot are no-ops\n");
	}
	else
	{
		Msg(eDLL_T::SERVER,
			"[SAFESPOT] PutEntityInSafeSpot resolved at %p\n",
			reinterpret_cast<void*>(v_PutEntityInSafeSpot));
	}

	if (!v_CBaseEntity_SetAbsOrigin)
	{
		Warning(eDLL_T::SERVER,
			"[SAFESPOT] SetAbsOrigin pattern unresolved -- "
			"CanPutPlayerInSafeSpot cannot restore origin after solve\n");
	}
	else
	{
		Msg(eDLL_T::SERVER,
			"[SAFESPOT] SetAbsOrigin resolved at %p\n",
			reinterpret_cast<void*>(v_CBaseEntity_SetAbsOrigin));
	}
}

static bool ServerScript_EntityIsPlayer(void* pEntity)
{
	if (!pEntity)
		return false;

	void** const pVtable = *reinterpret_cast<void***>(pEntity);
	if (!pVtable)
		return false;

	using MyPlayerPointer_fn = void* (__fastcall*)(void*);
	const MyPlayerPointer_fn pFn = reinterpret_cast<MyPlayerPointer_fn>(
		pVtable[kVtableOffMyPlayerPointer / sizeof(void*)]);
	if (!pFn)
		return false;

	return pFn(pEntity) != nullptr;
}

static void ServerScript_CopyAbsOrigin(void* pEntity, float* pOut)
{
	const float* const pSrc = reinterpret_cast<const float*>(
		reinterpret_cast<uintptr_t>(pEntity) + kEntOffAbsOrigin);
	pOut[0] = pSrc[0];
	pOut[1] = pSrc[1];
	pOut[2] = pSrc[2];
}

static SQRESULT ServerScript_PutEntityInSomewhatSafeSpot(HSQUIRRELVM v)
{
	ServerScript_ResolveSafeSpotFns();

	CBaseEntity* const pEntity = reinterpret_cast<CBaseEntity*>(
		ServerScript_EntityPtrFromStackIdx(v, 2));
	if (!pEntity)
	{
		static bool s_bNullWarned = false;
		if (!s_bNullWarned)
		{
			s_bNullWarned = true;
			Warning(eDLL_T::SERVER,
				"[SAFESPOT] PutEntityInSomewhatSafeSpot null entity -- skipped\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	// referenceEnt @3 and groundEnt @4 are legitimately nullable.
	CBaseEntity* const pReferenceEnt = reinterpret_cast<CBaseEntity*>(
		ServerScript_EntityPtrFromStackIdx(v, 3));
	CBaseEntity* const pGroundEnt = reinterpret_cast<CBaseEntity*>(
		ServerScript_EntityPtrFromStackIdx(v, 4));

	const SQVector3D* pSafeStart = nullptr;
	const SQVector3D* pEnd = nullptr;
	if (SQ_FAILED(sq_getvector(v, 5, &pSafeStart)) || !pSafeStart ||
		SQ_FAILED(sq_getvector(v, 6, &pEnd)) || !pEnd)
	{
		static bool s_bVecWarned = false;
		if (!s_bVecWarned)
		{
			s_bVecWarned = true;
			Warning(eDLL_T::SERVER,
				"[SAFESPOT] PutEntityInSomewhatSafeSpot bad vector args -- skipped\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!v_PutEntityInSafeSpot)
	{
		static bool s_bStubWarned = false;
		if (!s_bStubWarned)
		{
			s_bStubWarned = true;
			Warning(eDLL_T::SERVER,
				"[SAFESPOT] PutEntityInSomewhatSafeSpot stub -- native unresolved\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	v_PutEntityInSafeSpot(pEntity, pReferenceEnt, pGroundEnt,
		&pSafeStart->x, &pEnd->x);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_CanPutPlayerInSafeSpot(HSQUIRRELVM v)
{
	ServerScript_ResolveSafeSpotFns();

	CBaseEntity* const pPlayer = reinterpret_cast<CBaseEntity*>(
		ServerScript_EntityPtrFromStackIdx(v, 2));
	if (!pPlayer || !ServerScript_EntityIsPlayer(pPlayer))
	{
		static bool s_bPlayerWarned = false;
		if (!s_bPlayerWarned)
		{
			s_bPlayerWarned = true;
			Warning(eDLL_T::SERVER,
				"[SAFESPOT] CanPutPlayerInSafeSpot requires a player entity\n");
		}
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	CBaseEntity* const pReferenceEnt = reinterpret_cast<CBaseEntity*>(
		ServerScript_EntityPtrFromStackIdx(v, 3));
	CBaseEntity* const pGroundEnt = reinterpret_cast<CBaseEntity*>(
		ServerScript_EntityPtrFromStackIdx(v, 4));

	// S3 has no testHighCollision / allowNavNodesAsBackup; read for arity only.
	SQBool bTestHighCollision = SQFalse;
	SQBool bAllowNavNodesAsBackup = SQFalse;
	sq_getbool(v, 5, &bTestHighCollision);
	sq_getbool(v, 6, &bAllowNavNodesAsBackup);
	(void)bTestHighCollision;
	(void)bAllowNavNodesAsBackup;

	const SQVector3D* pSafeStart = nullptr;
	const SQVector3D* pEnd = nullptr;
	if (SQ_FAILED(sq_getvector(v, 7, &pSafeStart)) || !pSafeStart ||
		SQ_FAILED(sq_getvector(v, 8, &pEnd)) || !pEnd)
	{
		static bool s_bVecWarned = false;
		if (!s_bVecWarned)
		{
			s_bVecWarned = true;
			Warning(eDLL_T::SERVER,
				"[SAFESPOT] CanPutPlayerInSafeSpot bad vector args\n");
		}
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!v_PutEntityInSafeSpot || !v_CBaseEntity_SetAbsOrigin)
	{
		static bool s_bStubWarned = false;
		if (!s_bStubWarned)
		{
			s_bStubWarned = true;
			Warning(eDLL_T::SERVER,
				"[SAFESPOT] CanPutPlayerInSafeSpot stub -- native unresolved\n");
		}
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	float savedAbsOrigin[3];
	ServerScript_CopyAbsOrigin(pPlayer, savedAbsOrigin);

	const bool bSolved = v_PutEntityInSafeSpot(pPlayer, pReferenceEnt, pGroundEnt,
		&pSafeStart->x, &pEnd->x);
	if (!bSolved)
	{
		// Engine does not move the entity on any false path.
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	float solvedAbsOrigin[3];
	ServerScript_CopyAbsOrigin(pPlayer, solvedAbsOrigin);

	// Restore pre-solve origin before any other code observes the entity.
	v_CBaseEntity_SetAbsOrigin(pPlayer, savedAbsOrigin);

	const SQVector3D result(solvedAbsOrigin[0], solvedAbsOrigin[1], solvedAbsOrigin[2]);
	sq_pushvector(v, &result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_EmitSoundOnEntityExceptToPlayers(HSQUIRRELVM v)
{
	CBaseEntity* const pSoundEnt = reinterpret_cast<CBaseEntity*>(
		ServerScript_EntityPtrFromStackIdx(v, 2));
	if (!pSoundEnt)
	{
		static bool s_bNullWarned = false;
		if (!s_bNullWarned)
		{
			s_bNullWarned = true;
			Warning(eDLL_T::SERVER,
				"[SND-EXCLUDE] EmitSoundOnEntityExceptToPlayers null entity -- skipped\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const SQObjectPtr& arrObj = stack_get(v, 3);
	if (!sq_isarray(arrObj))
	{
		static bool s_bArrWarned = false;
		if (!s_bArrWarned)
		{
			s_bArrWarned = true;
			Warning(eDLL_T::SERVER,
				"[SND-EXCLUDE] EmitSoundOnEntityExceptToPlayers excludePlayers is not an array\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const SQChar* pszSoundName = nullptr;
	if (SQ_FAILED(sq_getstring(v, 4, &pszSoundName)) || !VALID_CHARSTAR(pszSoundName))
	{
		static bool s_bSoundWarned = false;
		if (!s_bSoundWarned)
		{
			s_bSoundWarned = true;
			Warning(eDLL_T::SERVER,
				"[SND-EXCLUDE] EmitSoundOnEntityExceptToPlayers bad sound name -- skipped\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!v_EmitSoundOnEntity)
	{
		static bool s_bStubWarned = false;
		if (!s_bStubWarned)
		{
			s_bStubWarned = true;
			Warning(eDLL_T::SERVER,
				"[SND-EXCLUDE] EmitSoundOnEntityExceptToPlayers stub -- "
				"EmitSoundOnEntity unresolved\n");
		}
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!v_SoundBridge_FilterPopulate || !v_SoundBridge_RemoveRecipient)
	{
		static bool s_bExcludeWiringWarned = false;
		if (!s_bExcludeWiringWarned)
		{
			s_bExcludeWiringWarned = true;
			Warning(eDLL_T::SERVER,
				"[SND-EXCLUDE] multi-exclude wiring unresolved -- "
				"sound emits to all recipients\n");
		}
	}

	int nSlots[8];
	int nSlotCount = 0;
	int nTotalValid = 0;

	const SQArray* const pArr = _array(arrObj);
	const SQInteger nSize = pArr ? pArr->Size() : 0;
	for (SQInteger j = 0; j < nSize; ++j)
	{
		const SQObjectPtr& element = pArr->_values[j];
		if (sq_isnull(element))
			continue;
		if (element._type != OT_ENTITY || !element._unVal.pInstance)
			continue;

		void* const pEnt = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(element._unVal.pInstance) + 0x50);
		if (!pEnt)
			continue;

		int16_t nEdict = *reinterpret_cast<const int16_t*>(
			reinterpret_cast<uintptr_t>(pEnt) + kEntOffEdictIndex);
		// Engine normalises the no-edict sentinel to 0.
		if (static_cast<uint16_t>(nEdict) == 0xFFFFu)
			nEdict = 0;

		if (nSlotCount < 8)
			nSlots[nSlotCount++] = static_cast<int>(nEdict);
		++nTotalValid;
	}

	if (nTotalValid > nSlotCount)
	{
		static bool s_bCapWarned = false;
		if (!s_bCapWarned)
		{
			s_bCapWarned = true;
			Warning(eDLL_T::SERVER,
				"[SND-EXCLUDE] %d players requested, only the first %d are excluded\n",
				nTotalValid, nSlotCount);
		}
	}

	SoundBridge_ArmRecipientExcludes(nSlots, nSlotCount);
	v_EmitSoundOnEntity(pSoundEnt, pszSoundName);
	SoundBridge_DisarmRecipientExcludes();

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: realm-scoped world particle spawn. No realm filter; leave slot 4 unread.
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_StartParticleEffectInWorldForRealmsShim(HSQUIRRELVM v)
{
    if (!v_Script_Server_StartParticleEffectInWorld)
    {
        static bool s_bWarned = false;
        if (!s_bWarned)
        {
            s_bWarned = true;
            Warning(eDLL_T::SERVER, "[FX-REALMS] StartParticleEffectInWorld unresolved -- "
                "every server script particle stays invisible\n");
        }

        sq_pushnull(v);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    const SQRESULT result = v_Script_Server_StartParticleEffectInWorld(v);
    if (result == SQ_ERROR)
        return result;

    // Registered as returning 'entity ornull'; the plain native pushes nothing.
    sq_pushnull(v);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// HIGH (2) is per-triangle static collision. Patch at TraceRayFiltered (slot 3)
// so both ignore paths get HIGH while the TLS override is set.
enum { kTraceDetailLevel_Normal = 0, kTraceDetailLevel_HighAtRayStart = 1, kTraceDetailLevel_High = 2 };

static constexpr int kRayOffDetailLevel = 0x64; // Ray_t m_nUnk68 (detailLevel)
static constexpr ptrdiff_t kEngineTrace_TraceRayFiltered = 3; // IEngineTrace vtable

static thread_local int s_scriptHullDetailOverride = -1;

using EngineTraceRayFilteredFn = void(__fastcall*)(void* pThis, Ray_t* pRay, unsigned int mask,
	ITraceFilter* pFilter, trace_t* pTrace);
static EngineTraceRayFilteredFn v_EngineTraceRayFiltered = nullptr;

struct ScriptHullDetailOverrideScope
{
	explicit ScriptHullDetailOverrideScope(const int level)
	{
		s_scriptHullDetailOverride = level;
	}
	~ScriptHullDetailOverrideScope()
	{
		s_scriptHullDetailOverride = -1;
	}
	ScriptHullDetailOverrideScope(const ScriptHullDetailOverrideScope&) = delete;
	ScriptHullDetailOverrideScope& operator=(const ScriptHullDetailOverrideScope&) = delete;
};

static void __fastcall Hook_EngineTraceRayFiltered(
	void* pThis, Ray_t* pRay, unsigned int mask, ITraceFilter* pFilter, trace_t* pTrace)
{
	if (s_scriptHullDetailOverride >= 0 && pRay)
	{
		*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(pRay) + kRayOffDetailLevel) =
			s_scriptHullDetailOverride;
	}
	v_EngineTraceRayFiltered(pThis, pRay, mask, pFilter, pTrace);
}

// S21 binds TraceHullHighDetail natively. S3 only has ScriptTraceHull (NORMAL).
// Reuse engine arg parse + TraceResults pack; force HIGH via TraceRayFiltered patch.
static SQRESULT ServerScript_TraceHullHighDetail(HSQUIRRELVM v)
{
	if (!v_EngineScriptTraceHull)
	{
		Warning(eDLL_T::SERVER, "[TRACE] TraceHullHighDetail: engine ScriptTraceHull unresolved\n");
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (!v_EngineTraceRayFiltered)
	{
		Warning(eDLL_T::SERVER, "[TRACE] TraceHullHighDetail: TraceRayFiltered unhooked -- NORMAL fallback\n");
		const SQInteger r = v_EngineScriptTraceHull(v);
		if (r < 0)
		{
			sq_pushnull(v);
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
		return SQ_OK;
	}

	const ScriptHullDetailOverrideScope detailScope(kTraceDetailLevel_High);
	const SQInteger r = v_EngineScriptTraceHull(v);
	if (r < 0)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	return SQ_OK;
}

// S21 scripts pass a 7th entitiesOnly arg. Engine body ignores stack 8.
// S3 type compiler accepts bool defaults as 0/1 (not the token 'false').
static constexpr const char* kTraceLineParamsEntitiesOnly =
	"vector startPos, vector endPos, var ignoreEntOrArrayOfEnts = null, "
	"int traceMask = 0, int collisionGroup = 0, entity tracingEntity = null, "
	"bool entitiesOnly = 0";

static thread_local int s_scriptTraceEntitiesOnly = 0;

using GetTraceTypeFn = TraceType_t(__fastcall*)(void* pThis);
static GetTraceTypeFn v_OrigFilterGetTraceType = nullptr;
static void** s_pVTable_CTraceFilterSimple = nullptr;
static void** s_pVTable_CTraceFilterSimpleList = nullptr;
static bool s_filterGetTraceTypePatched = false;

static TraceType_t __fastcall Hook_TraceFilter_GetTraceType(void* pThis)
{
	if (s_scriptTraceEntitiesOnly)
		return TRACE_ENTITIES_ONLY;
	if (v_OrigFilterGetTraceType)
		return v_OrigFilterGetTraceType(pThis);
	return TRACE_EVERYTHING;
}

static void TraceFilter_PatchGetTraceTypeSlot(void** vtable)
{
	if (!vtable)
		return;

	DWORD oldProt = 0;
	if (!VirtualProtect(&vtable[2], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
	{
		Warning(eDLL_T::SERVER, "[TRACE] VirtualProtect failed for filter GetTraceType slot\n");
		return;
	}

	if (!v_OrigFilterGetTraceType)
		v_OrigFilterGetTraceType = reinterpret_cast<GetTraceTypeFn>(vtable[2]);

	vtable[2] = reinterpret_cast<void*>(&Hook_TraceFilter_GetTraceType);
	VirtualProtect(&vtable[2], sizeof(void*), oldProt, &oldProt);
}

static void TraceFilter_EnsureGetTraceTypeHooks(void)
{
	if (s_filterGetTraceTypePatched)
		return;

	// CTraceFilterSimple vtable -- lea in UTIL_TraceLine_IgnoreEntity (0x140C41E00).
	const CMemory utilIgnore = Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 81 EC D0 00 00 00 48 8D 05 ?? ?? ?? ?? "
		"4C 89 4C 24 40 48 89 44 24 30");
	if (utilIgnore)
	{
		s_pVTable_CTraceFilterSimple = utilIgnore.Offset(0xD)
			.ResolveRelativeAddress(0x3, 0x7)
			.RCast<void**>();
	}

	// CTraceFilterSimpleList -- first lea rax,[rip] inside server ScriptTraceLine_WithDetail
	// that shares the same GetTraceType stub as Simple (array-ignore path).
	if (v_EngineScriptTraceLine)
	{
		// Wrapper prologue: 40 53 / 48 83 EC 20 / 33 D2 / 48 8B D9, near call at +0xB.
		const CMemory callSite = CMemory(reinterpret_cast<void*>(v_EngineScriptTraceLine)).Offset(0xB);
		CMemory core;
		if (callSite.CheckOpCodes({ 0xE8 }))
			core = callSite.FollowNearCall();

		const QWORD gameBase = g_GameDll.GetModuleBase();
		if (core && (core.GetPtr() < gameBase ||
			core.GetPtr() >= gameBase + g_GameDll.GetModuleSize()))
		{
			Warning(eDLL_T::SERVER,
				"[TRACE] ScriptTraceLine core call decoded outside module -- skipping SimpleList scan\n");
			core = CMemory();
		}

		if (core)
		{
			const uint8_t* const base = core.RCast<const uint8_t*>();
			for (size_t i = 0; i + 7 < 0x280; ++i)
			{
				if (base[i] != 0x48 || base[i + 1] != 0x8D || base[i + 2] != 0x05)
					continue;

				void** const vt = CMemory(core.GetPtr() + i)
					.ResolveRelativeAddress(0x3, 0x7)
					.RCast<void**>();
				if (!vt)
					continue;

				// Same GetTraceType implementation as CTraceFilterSimple (shared xor-eax stub).
				if (s_pVTable_CTraceFilterSimple &&
					vt[2] == s_pVTable_CTraceFilterSimple[2] &&
					vt != s_pVTable_CTraceFilterSimple)
				{
					s_pVTable_CTraceFilterSimpleList = vt;
					break;
				}
			}
		}
	}

	if (s_pVTable_CTraceFilterSimple)
		TraceFilter_PatchGetTraceTypeSlot(s_pVTable_CTraceFilterSimple);
	if (s_pVTable_CTraceFilterSimpleList)
		TraceFilter_PatchGetTraceTypeSlot(s_pVTable_CTraceFilterSimpleList);

	s_filterGetTraceTypePatched = (s_pVTable_CTraceFilterSimple != nullptr);

	if (s_filterGetTraceTypePatched)
	{
		Msg(eDLL_T::SERVER,
			"[TRACE] GetTraceType hook installed (Simple=%p List=%p) for entitiesOnly\n",
			s_pVTable_CTraceFilterSimple, s_pVTable_CTraceFilterSimpleList);
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[TRACE] filter GetTraceType hook unresolved -- entitiesOnly degrades to everything\n");
	}
}

struct ScriptTraceEntitiesOnlyScope
{
	explicit ScriptTraceEntitiesOnlyScope(const bool enable)
		: m_bEnabled(enable)
	{
		if (m_bEnabled)
		{
			TraceFilter_EnsureGetTraceTypeHooks();
			s_scriptTraceEntitiesOnly = 1;
		}
	}
	~ScriptTraceEntitiesOnlyScope()
	{
		if (m_bEnabled)
			s_scriptTraceEntitiesOnly = 0;
	}
	ScriptTraceEntitiesOnlyScope(const ScriptTraceEntitiesOnlyScope&) = delete;
	ScriptTraceEntitiesOnlyScope& operator=(const ScriptTraceEntitiesOnlyScope&) = delete;
	const bool m_bEnabled;
};

static bool ScriptTraceLine_ReadEntitiesOnly(HSQUIRRELVM v, SQBool* out)
{
	*out = SQFalse;
	// Squirrel stack: 1=thisfunc, 2.. = args. entitiesOnly is arg index 8.
	if (sq_gettop(v) < 8)
		return true;
	return SQ_SUCCEEDED(sq_getbool(v, 8, out));
}

static SQRESULT ServerScript_TraceLine_CallEngine(
	HSQUIRRELVM v, SQInteger (*engineFn)(HSQUIRRELVM), const char* tag)
{
	if (!engineFn)
	{
		Warning(eDLL_T::SERVER, "[TRACE] %s: engine native unresolved\n", tag);
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	SQBool entitiesOnly = SQFalse;
	if (!ScriptTraceLine_ReadEntitiesOnly(v, &entitiesOnly))
		return SQ_ERROR;

	const ScriptTraceEntitiesOnlyScope entitiesScope(entitiesOnly != 0);
	const SQInteger r = engineFn(v);
	if (r < 0)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	return SQ_OK;
}

static SQRESULT ServerScript_TraceLine_EntitiesOnly(HSQUIRRELVM v)
{
	return ServerScript_TraceLine_CallEngine(v, v_EngineScriptTraceLine, "TraceLine");
}

static SQRESULT ServerScript_TraceLineHighDetail_EntitiesOnly(HSQUIRRELVM v)
{
	return ServerScript_TraceLine_CallEngine(
		v, v_EngineScriptTraceLineHighDetail, "TraceLineHighDetail");
}

// BREACH_TRACE_RESULT_* values must match the S21 client; shared script uses both.
enum
{
	kBreachTraceResult_Success = 0,
	kBreachTraceResult_Failure = 1,
	kBreachTraceResult_InvalidEndPoint = 2,
	kBreachTraceResult_WallTooThin = 3,
	kBreachTraceResult_WallTooThick = 4,
};

// BreachTrace content masks (line vs hull).
static constexpr unsigned int kBreachTraceLineMask = 0x46404033u;
static constexpr unsigned int kBreachTraceHullMask = 0x06404033u;

static ConVar breachtrace_additional_line_traces("breachtrace_additional_line_traces", "1", FCVAR_RELEASE,
	"BreachTrace: also run HIGH detail line probes (default 1).");
static ConVar breachtrace_max_trace_attempts("breachtrace_max_trace_attempts", "50", FCVAR_RELEASE,
	"BreachTrace: max probe iterations (default 50).");
static ConVar breachtrace_min_trace_offset("breachtrace_min_trace_offset", "4.0", FCVAR_RELEASE,
	"BreachTrace: initial probe offset (default 4.0).");
static ConVar breachtrace_max_trace_offset("breachtrace_max_trace_offset", "16.0", FCVAR_RELEASE,
	"BreachTrace: per-step offset growth cap (default 16.0).");
static ConVar breachtrace_wall_thickness_min("breachtrace_wall_thickness_min", "0.18", FCVAR_RELEASE,
	"BreachTrace: minimum wall thickness for a valid exit (default 0.18).");

static float BreachTrace_Dist3(const Vector3D& a, const Vector3D& b)
{
	const float dx = a.x - b.x;
	const float dy = a.y - b.y;
	const float dz = a.z - b.z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void BreachTrace_TraceLineBack(
	const Vector3D& probe, const Vector3D& start, trace_t& out)
{
	memset(&out, 0, sizeof(out));
	out.fraction = 1.0f;
	out.endpos = start;
	if (!g_pEngineTraceServer)
		return;

	Ray_t ray;
	ray.Init(probe, start, 0x3f800000, 0);

	// HIGH detail for thin geo.
	const ScriptHullDetailOverrideScope detailScope(kTraceDetailLevel_High);
	CTraceFilterSimple filter(nullptr, 0);
	g_pEngineTraceServer->TraceRayFiltered(ray, kBreachTraceLineMask, &filter, &out);
}

// Hull sweep: ray up axis is dir, not world up. Use the engine initializer.
static void BreachTrace_TraceHullBack(
	const Vector3D& probe, const Vector3D& start, const Vector3D& dir,
	const Vector3D& mins, const Vector3D& maxs, trace_t& out)
{
	memset(&out, 0, sizeof(out));
	out.fraction = 1.0f;
	out.endpos = start;
	if (!g_pEngineTraceServer || !v_Ray_t_InitStartEndMinsMaxsUp)
		return;

	alignas(16) Ray_t ray;
	memset(&ray, 0, sizeof(ray));
	v_Ray_t_InitStartEndMinsMaxsUp(&ray, &probe, &start, &mins, &maxs, &dir);

	CTraceFilterSimple filter(nullptr, 0);
	g_pEngineTraceServer->TraceRayFiltered(ray, kBreachTraceHullMask, &filter, &out);
}

// FindFunction allocates a 24-byte HSCRIPT per call and never frees it. The
// handle copies the closure object and dies with the Squirrel VM at level
// change while the CSquirrelVM wrapper is reused, so key on the live
// HSQUIRRELVM.
static HSCRIPT s_hBreachEarlyExitOnEnt = nullptr;
static HSCRIPT s_hBreachIsValidPos = nullptr;
static HSQUIRRELVM s_hBreachCallbackVM = nullptr;

void BreachTrace_LevelShutdown(void)
{
	s_hBreachEarlyExitOnEnt = nullptr;
	s_hBreachIsValidPos = nullptr;
	s_hBreachCallbackVM = nullptr;
}

static HSCRIPT BreachTrace_ResolveCallback(HSCRIPT& hCached, const char* const pszName)
{
	if (!g_pServerScript)
		return nullptr;

	const HSQUIRRELVM hVMNow = g_pServerScript->GetVM();
	if (!hVMNow)
		return nullptr;

	if (s_hBreachCallbackVM != hVMNow)
	{
		s_hBreachCallbackVM = hVMNow;
		s_hBreachEarlyExitOnEnt = nullptr;
		s_hBreachIsValidPos = nullptr;
	}

	if (!hCached)
		hCached = g_pServerScript->FindFunction(pszName, nullptr, nullptr);

	return hCached;
}

// Pre-type FIELD_BOOLEAN; force m_flags 0 so ~ScriptVariant_t does not CRT-delete a non-scalar.
bool BreachTrace_CallBoolCallback(const HSCRIPT hFunc, const ScriptVariant_t& arg)
{
	ScriptVariant_t ret;
	ret.m_bool = false;
	ret.m_flags = 0;
	ret.m_type = FIELD_BOOLEAN;

	g_pServerScript->ExecuteFunction(hFunc, &arg, 1, &ret, nullptr);

	const bool result = (ret.m_type == FIELD_BOOLEAN) && ret.m_bool;
	ret.m_flags = 0;
	return result;
}

static bool BreachTrace_CallEarlyExitOnEnt(CBaseEntity* const pEnt)
{
	if (!pEnt)
		return false;
	const HSCRIPT hFunc = BreachTrace_ResolveCallback(
		s_hBreachEarlyExitOnEnt, "CodeCallback_BreachTraceEarlyExitOnEnt");
	if (!hFunc)
		return false;
	const HSCRIPT hInst = pEnt->GetScriptInstance();
	if (!hInst)
		return false;

	return BreachTrace_CallBoolCallback(hFunc, ScriptVariant_t(hInst));
}

// Absent callback reports success; missing must not be treated as a rejection.
static bool BreachTrace_CallIsValidPos(const Vector3D& pos, bool& bCalled)
{
	bCalled = false;
	const HSCRIPT hFunc = BreachTrace_ResolveCallback(
		s_hBreachIsValidPos, "CodeCallback_BreachTraceIsValidPos");
	if (!hFunc)
		return true;

	bCalled = true;
	return BreachTrace_CallBoolCallback(hFunc, ScriptVariant_t(pos));
}

// Field order must match `global struct BreachTraceResults` in init.nut -- the VM resolves
// struct members by index, so a mismatch here reads the wrong slot rather than failing.
enum
{
	kBreachTraceField_Result = 0,
	kBreachTraceField_EndPos = 1,
	kBreachTraceField_SurfaceNormal = 2,
	kBreachTraceField_Count = 3,
};

static void BreachTrace_PushResults(
	HSQUIRRELVM v, const int result, const Vector3D& endPos, const Vector3D& surfaceNormal)
{
	if (!v_sq_newstruct || !v_sq_setstructfield)
	{
		Warning(eDLL_T::SERVER, "[BREACH] struct push unresolved -- BreachTrace cannot return results\n");
		sq_pushnull(v);
		return;
	}

	v_sq_newstruct(v, kBreachTraceField_Count);

	sq_pushinteger(v, result);
	v_sq_setstructfield(v, kBreachTraceField_Result);

	const SQVector3D end(endPos.x, endPos.y, endPos.z);
	sq_pushvector(v, &end);
	v_sq_setstructfield(v, kBreachTraceField_EndPos);

	const SQVector3D nrm(surfaceNormal.x, surfaceNormal.y, surfaceNormal.z);
	sq_pushvector(v, &nrm);
	v_sq_setstructfield(v, kBreachTraceField_SurfaceNormal);
}

static SQRESULT ServerScript_BreachTrace(HSQUIRRELVM v)
{
	const SQVector3D* pStart = nullptr;
	const SQVector3D* pDir = nullptr;
	const SQVector3D* pMins = nullptr;
	const SQVector3D* pMaxs = nullptr;
	sq_getvector(v, 2, &pStart);
	sq_getvector(v, 3, &pDir);
	sq_getvector(v, 4, &pMins);
	sq_getvector(v, 5, &pMaxs);
	float maxThickness = 0.0f;
	sq_getfloat(v, 6, &maxThickness);

	if (!pStart || !pDir || !pMins || !pMaxs || !g_pEngineTraceServer)
	{
		Warning(eDLL_T::SERVER, "[BREACH] BreachTrace: bad args or no engine trace\n");
		BreachTrace_PushResults(v, kBreachTraceResult_Failure, Vector3D(0, 0, 0), Vector3D(0, 0, 1));
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const Vector3D start(pStart->x, pStart->y, pStart->z);
	const Vector3D dir(pDir->x, pDir->y, pDir->z);
	const Vector3D mins(pMins->x, pMins->y, pMins->z);
	const Vector3D maxs(pMaxs->x, pMaxs->y, pMaxs->z);

	const bool doLine = breachtrace_additional_line_traces.GetBool();
	const int maxAttempts = breachtrace_max_trace_attempts.GetInt();
	const float minOffset = breachtrace_min_trace_offset.GetFloat();
	const float maxStep = breachtrace_max_trace_offset.GetFloat();
	const float wallMin = breachtrace_wall_thickness_min.GetFloat();

	bool lineFound = false;
	bool hullFound = false;
	bool lineThickEnough = false;
	bool hullThickEnough = false;
	trace_t lineTr{};
	trace_t hullTr{};
	lineTr.fraction = 1.0f;
	hullTr.fraction = 1.0f;

	float offset = minOffset;
	int attempts = 0;
	Vector3D probe = start;

	while (maxThickness >= offset && attempts < maxAttempts)
	{
		probe = Vector3D(
			start.x + dir.x * offset,
			start.y + dir.y * offset,
			start.z + dir.z * offset);

		if (doLine && !lineFound)
		{
			BreachTrace_TraceLineBack(probe, start, lineTr);
			const float dist = BreachTrace_Dist3(start, lineTr.endpos);
			lineThickEnough = dist > wallMin;
			if (lineThickEnough && lineTr.fraction != 1.0f && !lineTr.startsolid)
				lineFound = true;
		}

		if (!hullFound)
		{
			BreachTrace_TraceHullBack(probe, start, dir, mins, maxs, hullTr);
			const float dist = BreachTrace_Dist3(start, hullTr.endpos);
			hullThickEnough = dist > wallMin;
			// Valid hull exit: thickness ok, not fraction1, not startsolid. plane.normal*dir>0 is front-face (S3 has no backface flag).
			const float nDot = hullTr.plane.normal.x * dir.x
				+ hullTr.plane.normal.y * dir.y
				+ hullTr.plane.normal.z * dir.z;
			if (hullThickEnough && hullTr.fraction != 1.0f && !hullTr.startsolid && nDot > 0.0f)
				hullFound = true;
		}

		if (lineTr.hit_entity && BreachTrace_CallEarlyExitOnEnt(lineTr.hit_entity))
		{
			lineFound = true;
			hullFound = true;
		}

		if (hullFound)
		{
			if (!doLine)
				break;
			if (lineFound)
				break;
		}

		++attempts;
		offset += fminf(offset, maxStep);
	}

	int resultCode = kBreachTraceResult_WallTooThick;
	Vector3D endPos = start;
	Vector3D surfaceNormal(-dir.x, -dir.y, -dir.z);

	if (hullFound || lineFound)
	{
		// When both probes landed, keep the farther exit. Nearer one stops the drill short on angled geo.
		const bool preferHull = hullFound && (!lineFound
			|| BreachTrace_Dist3(start, hullTr.endpos) >= BreachTrace_Dist3(start, lineTr.endpos));
		if (preferHull)
		{
			endPos = hullTr.endpos;
			surfaceNormal = hullTr.plane.normal;
		}
		else
		{
			endPos = lineTr.endpos;
			surfaceNormal = lineTr.plane.normal;
		}

		// Validity callback only when hull hit an entity; feed last probe position,
		// not the resolved exit. Rejection stays WALL_TOO_THICK (not INVALID_END_POINT).
		resultCode = kBreachTraceResult_Success;
		if (hullTr.hit_entity)
		{
			bool bCalled = false;
			if (!BreachTrace_CallIsValidPos(probe, bCalled) && bCalled)
				resultCode = kBreachTraceResult_WallTooThick;
		}
	}
	else if (offset <= maxThickness)
	{
		// Still within maxDist: thin-wall signal vs invalid placement.
		resultCode = (lineThickEnough || hullThickEnough)
			? kBreachTraceResult_WallTooThin
			: kBreachTraceResult_InvalidEndPoint;
	}
	else
	{
		resultCode = kBreachTraceResult_Failure;
	}

	BreachTrace_PushResults(v, resultCode, endPos, surfaceNormal);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static const matrix3x4_t* ServerScript_GetEntityToWorld(CBaseEntity* const pEnt)
{
	if (!pEnt || !v_CBaseEntity_EntityToWorldTransform)
		return nullptr;
	return v_CBaseEntity_EntityToWorldTransform(pEnt);
}

static void ServerScript_PushVector3(HSQUIRRELVM v, const Vector3D& vec)
{
	const SQVector3D out(vec.x, vec.y, vec.z);
	sq_pushvector(v, &out);
}

// CalcLocalToWorldOrigin_Entity(entity, localPos) -> vector
static SQRESULT ServerScript_CalcLocalToWorldOrigin_Entity(HSQUIRRELVM v)
{
	CBaseEntity* pEnt = nullptr;
	const SQVector3D* pLocal = nullptr;
	v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt));
	sq_getvector(v, 2, &pLocal);
	if (!pLocal)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const Vector3D local(pLocal->x, pLocal->y, pLocal->z);
	Vector3D world = local;
	if (const matrix3x4_t* const mat = ServerScript_GetEntityToWorld(pEnt))
		VectorTransform(local, *mat, world);
	// null entity: engine ScriptError then returns input -- we pass through.

	ServerScript_PushVector3(v, world);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// CalcWorldToLocalOrigin_Entity(entity, worldPos) -> vector
static SQRESULT ServerScript_CalcWorldToLocalOrigin_Entity(HSQUIRRELVM v)
{
	CBaseEntity* pEnt = nullptr;
	const SQVector3D* pWorld = nullptr;
	v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt));
	sq_getvector(v, 2, &pWorld);
	if (!pWorld)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const Vector3D world(pWorld->x, pWorld->y, pWorld->z);
	Vector3D local = world;
	if (const matrix3x4_t* const mat = ServerScript_GetEntityToWorld(pEnt))
		VectorITransform(world, *mat, local);

	ServerScript_PushVector3(v, local);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// CalcLocalToWorldAngles_Entity(entity, localAngles) -> vector
// Engine: AngleMatrix(local) -> ConcatTransforms(entity, local) -> MatrixAngles
static SQRESULT ServerScript_CalcLocalToWorldAngles_Entity(HSQUIRRELVM v)
{
	CBaseEntity* pEnt = nullptr;
	const SQVector3D* pLocal = nullptr;
	v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt));
	sq_getvector(v, 2, &pLocal);
	if (!pLocal)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const QAngle localAngles(pLocal->x, pLocal->y, pLocal->z);
	QAngle worldAngles = localAngles;
	if (const matrix3x4_t* const mat = ServerScript_GetEntityToWorld(pEnt))
		worldAngles = TransformAnglesToWorldSpace(localAngles, *mat);

	ServerScript_PushVector3(v, Vector3D(worldAngles.x, worldAngles.y, worldAngles.z));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// CalcWorldToLocalAngles_Entity(entity, worldAngles) -> vector
// Engine: InvertOrthogonal(entity) -> AngleMatrix(world) -> Concat -> MatrixAngles
static SQRESULT ServerScript_CalcWorldToLocalAngles_Entity(HSQUIRRELVM v)
{
	CBaseEntity* pEnt = nullptr;
	const SQVector3D* pWorld = nullptr;
	v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt));
	sq_getvector(v, 2, &pWorld);
	if (!pWorld)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const QAngle worldAngles(pWorld->x, pWorld->y, pWorld->z);
	QAngle localAngles = worldAngles;
	if (const matrix3x4_t* const mat = ServerScript_GetEntityToWorld(pEnt))
		localAngles = TransformAnglesToLocalSpace(worldAngles, *mat);

	ServerScript_PushVector3(v, Vector3D(localAngles.x, localAngles.y, localAngles.z));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// entitiesOnly arity. Run after engine registration and again before precompile.
// File-scope bindings so successive calls cannot clobber each other.
void Script_RegisterTraceLineEntitiesOnlyArity(CSquirrelVM* s)
{
	if (!s)
		return;

	// Engine re-registers SetDeathFieldParams after VM Init; re-apply before compile.
	DeathField_RegisterOnVM(s);

	if (v_EngineScriptTraceLine)
	{
		static ScriptFunctionBinding_t s_bindTraceLine;
		s_bindTraceLine.Init(
			"TraceLine",
			"Server_Script_TraceLine_EntitiesOnly",
			"Does a trace and returns struct of result values.",
			"TraceResults",
			kTraceLineParamsEntitiesOnly,
			false,
			ServerScript_TraceLine_EntitiesOnly);
		const SQRESULT r = s->RegisterFunction(&s_bindTraceLine, true);
		Msg(eDLL_T::SERVER,
			"[TRACE] TraceLine entitiesOnly arity registered r=%d eng=%p\n",
			static_cast<int>(r),
			reinterpret_cast<void*>(v_EngineScriptTraceLine));
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[TRACE] TraceLine entitiesOnly arity SKIPPED -- engine ScriptTraceLine unresolved\n");
	}

	if (v_EngineScriptTraceLineHighDetail)
	{
		static ScriptFunctionBinding_t s_bindTraceLineHD;
		s_bindTraceLineHD.Init(
			"TraceLineHighDetail",
			"Server_Script_TraceLineHighDetail_EntitiesOnly",
			"Does a high-detail (per poly on static models) trace and returns struct of result values.",
			"TraceResults",
			kTraceLineParamsEntitiesOnly,
			false,
			ServerScript_TraceLineHighDetail_EntitiesOnly);
		const SQRESULT r = s->RegisterFunction(&s_bindTraceLineHD, true);
		Msg(eDLL_T::SERVER,
			"[TRACE] TraceLineHighDetail entitiesOnly arity registered r=%d eng=%p\n",
			static_cast<int>(r),
			reinterpret_cast<void*>(v_EngineScriptTraceLineHighDetail));
	}
}

// Per-player preference blobs on disk. Identity from CClient only.
static ConVar cafe_prefs_disk("cafe_prefs_disk", "1", FCVAR_RELEASE,
	"Master gate for Cafe_PlayerPrefs disk I/O. 0 = read returns \"\", write is a no-op.");
static ConVar cafe_prefs_allow_anon("cafe_prefs_allow_anon", "1", FCVAR_RELEASE,
	"Allow offline players (platform uid 9990000) to use the hashed-name anon prefs bucket.");
static ConVar cafe_prefs_verbose("cafe_prefs_verbose", "0", FCVAR_DEVELOPMENTONLY,
	"Log every successful Cafe_PlayerPrefs write (default logs only the first).");
static ConVar fs_http_timeout("fs_http_timeout", "5", FCVAR_RELEASE,
	"HTTPS POST timeout in seconds for FS_StatsIngest", true, 1.f, true, 15.f);
static ConVar fs_stats_url("fs_stats_url",
	"https://play.r5flowstate.org/stats/1v1/ingest", FCVAR_RELEASE,
	"1v1 stats ingest URL. Empty disables posts.");
// PROTECTED: clients see 0/1, never the key. DONTRECORD / SERVER_CANNOT_QUERY:
// not in demos, not via StartQueryCvarValue.
static ConVar fs_stats_host_key("fs_stats_host_key", "",
	FCVAR_RELEASE | FCVAR_PROTECTED | FCVAR_DONTRECORD | FCVAR_SERVER_CANNOT_QUERY,
	"Verified-host key for 1v1 stats ingest. Empty disables posts.");

static constexpr int kFsHttpMaxInflight = 4;
static constexpr size_t kFsHttpMinUrl = 8;
static constexpr size_t kFsHttpMaxUrl = 256;
static constexpr size_t kFsHttpMaxHeaderName = 64;
static constexpr size_t kFsHttpMaxHeaderValue = 256;
static constexpr size_t kFsHttpMaxBody = 65536;
static constexpr size_t kFsHttpMaxResponse = 65536;

static std::atomic<int> s_nFsHttpInflight{ 0 };
static bool s_bFsHttpFirst = false;
static bool s_bFsHttpCapWarned = false;
static std::atomic<bool> s_bFsHttpFirstOk{ false };

static constexpr size_t kCafePrefsMaxPayload = 4096;
static constexpr size_t kCafePrefsMaxKeyLen = 32;

static bool CafePrefs_MkDir(const char* pszPath)
{
	if (CreateDirectoryA(pszPath, nullptr))
		return true;
	if (GetLastError() == ERROR_ALREADY_EXISTS)
		return true;
	return false;
}

static bool CafePrefs_EnsureDirTree(const char* pszDir)
{
	static bool s_bMkDirFailLogged = false;

	if (!pszDir || !pszDir[0])
		return false;

	char szWalk[MAX_PATH];
	V_strncpy(szWalk, pszDir, sizeof(szWalk));
	szWalk[sizeof(szWalk) - 1] = '\0';

	// Create each intermediate level; ignore ERROR_ALREADY_EXISTS inside MkDir.
	for (char* p = szWalk; *p; ++p)
	{
		if (*p != '\\' && *p != '/')
			continue;

		*p = '\0';
		if (szWalk[0] && !CafePrefs_MkDir(szWalk))
		{
			if (!s_bMkDirFailLogged)
			{
				s_bMkDirFailLogged = true;
				Warning(eDLL_T::SERVER, "[FR-PREFS] CreateDirectory failed for '%s' (err=%lu)\n",
					szWalk, GetLastError());
			}
			return false;
		}
		*p = '\\';
	}

	if (!CafePrefs_MkDir(szWalk))
	{
		if (!s_bMkDirFailLogged)
		{
			s_bMkDirFailLogged = true;
			Warning(eDLL_T::SERVER, "[FR-PREFS] CreateDirectory failed for '%s' (err=%lu)\n",
				szWalk, GetLastError());
		}
		return false;
	}

	return true;
}

// Client-supplied names must never be path components (reserved devices, etc.).
static uint64_t CafePrefs_Fnv1a64(const char* psz, size_t nLen)
{
	uint64_t nHash = 14695981039346656037ULL;
	for (size_t i = 0; i < nLen; ++i)
	{
		nHash ^= static_cast<uint8_t>(psz[i]);
		nHash *= 1099511628211ULL;
	}
	return nHash;
}

static bool CafePrefs_ResolveDir(CPlayer* pPlayer, char* pszOut, size_t nOutLen)
{
	if (!pPlayer || !pszOut || nOutLen == 0 || !g_pServer)
		return false;

	CClient* const pClient = g_pServer->GetClient(pPlayer->GetEdict() - 1);
	if (!pClient)
		return false;

	const uint64_t uid = static_cast<uint64_t>(pClient->GetPlatformUserId());
	if (uid == 0)
		return false;

	if (uid != static_cast<uint64_t>(FAKE_BASE_NUCLEUD_ID))
	{
		V_snprintf(pszOut, nOutLen, "platform\\playerprefs\\%llu",
			static_cast<unsigned long long>(uid));
		return true;
	}

	// FAKE_BASE_NUCLEUD_ID (9990000): offline/pre-identity shared sentinel.
	if (!cafe_prefs_allow_anon.GetBool())
		return false;

	const char* pszName = pClient->GetClientName();
	if (!pszName)
		pszName = "";

	const uint64_t nHash = CafePrefs_Fnv1a64(pszName, strlen(pszName));

	static std::set<uint64_t> s_AnonWarned;
	if (s_AnonWarned.find(nHash) == s_AnonWarned.end())
	{
		s_AnonWarned.insert(nHash);
		Warning(eDLL_T::SERVER,
			"[FR-PREFS] no platform identity, using anon bucket %016llx\n",
			static_cast<unsigned long long>(nHash));
	}

	V_snprintf(pszOut, nOutLen, "platform\\playerprefs\\anon\\%016llx",
		static_cast<unsigned long long>(nHash));
	return true;
}

static bool CafePrefs_SanitizeKey(const char* pszKey, char* pszOut, size_t nOutLen)
{
	if (!pszKey || !pszKey[0] || !pszOut || nOutLen < 2)
		return false;

	const size_t nMax = (nOutLen - 1) < kCafePrefsMaxKeyLen ? (nOutLen - 1) : kCafePrefsMaxKeyLen;
	size_t nOut = 0;
	for (const char* p = pszKey; *p && nOut < nMax; ++p)
	{
		const unsigned char c = static_cast<unsigned char>(*p);
		const bool bOk = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9') || c == '_' || c == '-';
		pszOut[nOut++] = bOk ? static_cast<char>(c) : '_';
	}
	pszOut[nOut] = '\0';
	return nOut > 0;
}

static SQRESULT ServerScript_CafePlayerPrefs_Write(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	const SQChar* pszKey = nullptr;
	const SQChar* pszPayload = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	sq_getstring(v, 2, &pszKey);
	sq_getstring(v, 3, &pszPayload);

	if (!pszKey)
	{
		v_SQVM_ScriptError("Null prefs key string");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}
	if (!pszPayload)
	{
		v_SQVM_ScriptError("Null prefs payload string");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	if (!cafe_prefs_disk.GetBool())
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const size_t nPayloadLen = strlen(pszPayload);
	if (nPayloadLen > kCafePrefsMaxPayload)
	{
		static bool s_bPayloadTooLongLogged = false;
		if (!s_bPayloadTooLongLogged)
		{
			s_bPayloadTooLongLogged = true;
			Warning(eDLL_T::SERVER,
				"[FR-PREFS] write rejected: payload longer than %zu bytes\n",
				kCafePrefsMaxPayload);
		}
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	char szKey[kCafePrefsMaxKeyLen + 1];
	if (!CafePrefs_SanitizeKey(pszKey, szKey, sizeof(szKey)))
	{
		static bool s_bBadKeyLogged = false;
		if (!s_bBadKeyLogged)
		{
			s_bBadKeyLogged = true;
			Warning(eDLL_T::SERVER, "[FR-PREFS] write rejected: empty or invalid key\n");
		}
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	char szDir[MAX_PATH];
	if (!CafePrefs_ResolveDir(pPlayer, szDir, sizeof(szDir)))
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!CafePrefs_EnsureDirTree(szDir))
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	char szFinal[MAX_PATH];
	char szTmp[MAX_PATH];
	V_snprintf(szFinal, sizeof(szFinal), "%s\\%s.txt", szDir, szKey);
	V_snprintf(szTmp, sizeof(szTmp), "%s\\%s.txt.tmp", szDir, szKey);

	// Temp + MoveFileEx so a kill mid-write leaves the previous good file intact.
	{
		std::ofstream out(szTmp, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
		{
			static bool s_bOpenFailLogged = false;
			if (!s_bOpenFailLogged)
			{
				s_bOpenFailLogged = true;
				Warning(eDLL_T::SERVER, "[FR-PREFS] write open failed for '%s'\n", szTmp);
			}
			sq_pushbool(v, false);
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}

		if (nPayloadLen > 0)
			out.write(pszPayload, static_cast<std::streamsize>(nPayloadLen));
		out.flush();
		const bool bGood = out.good();
		out.close();

		if (!bGood)
		{
			DeleteFileA(szTmp);
			static bool s_bWriteFailLogged = false;
			if (!s_bWriteFailLogged)
			{
				s_bWriteFailLogged = true;
				Warning(eDLL_T::SERVER, "[FR-PREFS] write stream failed for '%s'\n", szTmp);
			}
			sq_pushbool(v, false);
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
	}

	if (!MoveFileExA(szTmp, szFinal, MOVEFILE_REPLACE_EXISTING))
	{
		DeleteFileA(szTmp);
		static bool s_bMoveFailLogged = false;
		if (!s_bMoveFailLogged)
		{
			s_bMoveFailLogged = true;
			Warning(eDLL_T::SERVER,
				"[FR-PREFS] MoveFileEx failed for '%s' (err=%lu)\n",
				szFinal, GetLastError());
		}
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	static bool s_bFirstWriteLogged = false;
	if (!s_bFirstWriteLogged || cafe_prefs_verbose.GetBool())
	{
		s_bFirstWriteLogged = true;
		Msg(eDLL_T::SERVER, "[FR-PREFS] write ok key=%s path=%s bytes=%zu\n",
			szKey, szFinal, nPayloadLen);
	}

	sq_pushbool(v, true);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_CafePlayerPrefs_Read(HSQUIRRELVM v)
{
	CPlayer* pPlayer = nullptr;
	const SQChar* pszKey = nullptr;

	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	sq_getstring(v, 2, &pszKey);

	if (!pszKey)
	{
		v_SQVM_ScriptError("Null prefs key string");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	if (!cafe_prefs_disk.GetBool())
	{
		sq_pushstring(v, "", -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	char szKey[kCafePrefsMaxKeyLen + 1];
	if (!CafePrefs_SanitizeKey(pszKey, szKey, sizeof(szKey)))
	{
		sq_pushstring(v, "", -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	char szDir[MAX_PATH];
	if (!CafePrefs_ResolveDir(pPlayer, szDir, sizeof(szDir)))
	{
		sq_pushstring(v, "", -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	char szPath[MAX_PATH];
	V_snprintf(szPath, sizeof(szPath), "%s\\%s.txt", szDir, szKey);

	std::ifstream file(szPath, std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		sq_pushstring(v, "", -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const std::streamoff nSize = file.tellg();
	if (nSize < 0)
	{
		sq_pushstring(v, "", -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (static_cast<size_t>(nSize) > kCafePrefsMaxPayload)
	{
		static bool s_bCorruptLogged = false;
		if (!s_bCorruptLogged)
		{
			s_bCorruptLogged = true;
			Warning(eDLL_T::SERVER,
				"[FR-PREFS] load treated as corrupt (>%zu bytes): '%s'\n",
				kCafePrefsMaxPayload, szPath);
		}
		sq_pushstring(v, "", -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	file.seekg(0, std::ios::beg);
	std::string data;
	data.resize(static_cast<size_t>(nSize));
	if (nSize > 0)
		file.read(&data[0], nSize);

	if (!file)
	{
		static bool s_bReadFailLogged = false;
		if (!s_bReadFailLogged)
		{
			s_bReadFailLogged = true;
			Warning(eDLL_T::SERVER, "[FR-PREFS] load read failed for '%s'\n", szPath);
		}
		sq_pushstring(v, "", -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	static bool s_bFirstLoadLogged = false;
	if (!s_bFirstLoadLogged)
	{
		s_bFirstLoadLogged = true;
		Msg(eDLL_T::SERVER, "[FR-PREFS] load ok key=%s path=%s bytes=%zu\n",
			szKey, szPath, data.size());
	}

	sq_pushstring(v, data.c_str(), static_cast<SQInteger>(data.size()));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static bool FsHttp_HasCrLf(const char* psz)
{
	for (const char* p = psz; *p; ++p)
	{
		if (*p == '\r' || *p == '\n')
			return true;
	}
	return false;
}

static bool FsHttp_UrlOk(const char* pszUrl)
{
	if (!pszUrl)
		return false;

	const size_t nLen = strlen(pszUrl);
	if (nLen < kFsHttpMinUrl || nLen > kFsHttpMaxUrl)
		return false;

	for (size_t i = 0; i < nLen; ++i)
	{
		const unsigned char c = static_cast<unsigned char>(pszUrl[i]);
		if (c == '\r' || c == '\n' || c == ' ')
			return false;
	}

	if (V_strnicmp(pszUrl, "https://", 8) == 0)
		return true;

	if (V_strnicmp(pszUrl, "http://", 7) != 0)
		return false;

	const char* pszHost = pszUrl + 7;
	size_t nHost = 0;
	if (V_strnicmp(pszHost, "127.0.0.1", 9) == 0)
		nHost = 9;
	else if (V_strnicmp(pszHost, "localhost", 9) == 0)
		nHost = 9;
	else
		return false;

	const char cNext = pszHost[nHost];
	return cNext == '\0' || cNext == '/' || cNext == ':' || cNext == '?' || cNext == '#';
}


static bool FsHttp_HeaderValueOk(const char* pszValue)
{
	if (!pszValue)
		return false;

	const size_t nLen = strlen(pszValue);
	if (nLen > kFsHttpMaxHeaderValue)
		return false;
	return !FsHttp_HasCrLf(pszValue);
}

static bool FsHttp_BodyOk(const char* pszBody)
{
	if (!pszBody)
		return false;
	return strlen(pszBody) <= kFsHttpMaxBody;
}

static void FsHttp_CopyHost(const char* pszUrl, char* pszOut, size_t nOut)
{
	if (!pszOut || nOut == 0)
		return;
	pszOut[0] = '\0';
	if (!pszUrl || !pszUrl[0])
		return;

	const char* pszScheme = strstr(pszUrl, "://");
	const char* pszHost = pszScheme ? pszScheme + 3 : pszUrl;
	const char* pszEnd = pszHost;
	while (*pszEnd && *pszEnd != '/' && *pszEnd != '?' && *pszEnd != '#')
		++pszEnd;

	const size_t nCopyMax = nOut - 1;
	const size_t nNeed = static_cast<size_t>(pszEnd - pszUrl);
	const size_t nCopy = nNeed < nCopyMax ? nNeed : nCopyMax;
	memcpy(pszOut, pszUrl, nCopy);
	pszOut[nCopy] = '\0';
}

static size_t FsHttp_WriteCapped(char* data, const size_t size, const size_t nmemb, string* userp)
{
	const size_t nAdd = size * nmemb;
	if (!userp || nAdd > kFsHttpMaxResponse || userp->size() > kFsHttpMaxResponse - nAdd)
		return 0;
	userp->append(data, nAdd);
	return nAdd;
}

static void FsHttp_Worker(string url, string headerName, string headerValue, string body, const int nTimeout)
{
	struct FsHttpInflightDec
	{
		~FsHttpInflightDec()
		{
			s_nFsHttpInflight.fetch_sub(1, std::memory_order_acq_rel);
		}
	} inflightDec;

	char szHost[kFsHttpMaxUrl + 1];
	FsHttp_CopyHost(url.c_str(), szHost, sizeof(szHost));

	CURLParams params;
	params.writeFunction = FsHttp_WriteCapped;
	params.timeout = nTimeout;
	params.verifyPeer = (V_strnicmp(url.c_str(), "https://", 8) == 0);
	params.followRedirect = false;
	params.failOnError = false;

	string response;
	curl_slist* slist = nullptr;
	// POSTFIELDS is not copied by curl; body must live until perform returns.
	CURL* curl = CURLInitRequest(url.c_str(), body.c_str(), response, slist, params);
	if (!curl)
	{
		Warning(eDLL_T::SERVER, "[FS-HTTP] fail=%s host=%s\n", "init", szHost);
		return;
	}

	if (!headerName.empty())
	{
		char szHdr[kFsHttpMaxHeaderName + 2 + kFsHttpMaxHeaderValue + 1];
		V_snprintf(szHdr, sizeof(szHdr), "%s: %s", headerName.c_str(), headerValue.c_str());
		slist = CURLSlistAppend(slist, szHdr);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
	}

	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

	const CURLcode res = CURLSubmitRequest(curl, slist);
	string err;
	if (!CURLHandleError(curl, res, err, false))
	{
		Warning(eDLL_T::SERVER, "[FS-HTTP] fail=%s host=%s\n", err.c_str(), szHost);
		return;
	}

	const int nStatus = static_cast<int>(CURLRetrieveInfo(curl));
	if (nStatus >= 400)
	{
		Warning(eDLL_T::SERVER, "[FS-HTTP] status=%d host=%s\n", nStatus, szHost);
		return;
	}

	if (!s_bFsHttpFirstOk.exchange(true, std::memory_order_acq_rel))
		Msg(eDLL_T::SERVER, "[FS-HTTP] status=%d host=%s\n", nStatus, szHost);
}

SQRESULT ServerScript_FS_StatsIngest(HSQUIRRELVM v)
{
	const SQChar* pszBody = nullptr;
	sq_getstring(v, 2, &pszBody);

	const char* pszUrl = fs_stats_url.GetString();
	const char* pszKey = fs_stats_host_key.GetString();

	if (!pszBody || !pszUrl || !pszUrl[0] || !pszKey || !pszKey[0]
		|| !FsHttp_UrlOk(pszUrl)
		|| !FsHttp_HeaderValueOk(pszKey)
		|| !FsHttp_BodyOk(pszBody))
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const int nPrev = s_nFsHttpInflight.fetch_add(1, std::memory_order_acq_rel);
	if (nPrev >= kFsHttpMaxInflight)
	{
		s_nFsHttpInflight.fetch_sub(1, std::memory_order_acq_rel);
		if (!s_bFsHttpCapWarned)
		{
			s_bFsHttpCapWarned = true;
			Warning(eDLL_T::SERVER, "[FS-HTTP] inflight cap\n");
		}
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (!s_bFsHttpFirst)
	{
		s_bFsHttpFirst = true;
		Msg(eDLL_T::SERVER, "[FS-HTTP] FS_StatsIngest live\n");
	}

	int nTimeout = fs_http_timeout.GetInt();
	if (nTimeout < 1)
		nTimeout = 1;
	if (nTimeout > 15)
		nTimeout = 15;

	std::thread(FsHttp_Worker, string(pszUrl), string("X-Host-Key"),
		string(pszKey), string(pszBody), nTimeout).detach();

	sq_pushbool(v, true);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Head-glitch readouts. Scoring is native; match policy is script.
//-----------------------------------------------------------------------------
static CPlayer* ServerScript_HeadGlitchPlayerArg(HSQUIRRELVM v)
{
	return reinterpret_cast<CPlayer*>(ServerScript_EntityPtrFromStackIdx(v, 2));
}

static SQRESULT ServerScript_HeadGlitch_GetScore(HSQUIRRELVM v)
{
	sq_pushfloat(v, HeadGlitch_GetScore(ServerScript_HeadGlitchPlayerArg(v)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_GetExposure(HSQUIRRELVM v)
{
	sq_pushfloat(v, HeadGlitch_GetExposure(ServerScript_HeadGlitchPlayerArg(v)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_GetHoldTime(HSQUIRRELVM v)
{
	sq_pushfloat(v, HeadGlitch_GetHoldTime(ServerScript_HeadGlitchPlayerArg(v)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_GetTotalTime(HSQUIRRELVM v)
{
	sq_pushfloat(v, HeadGlitch_GetTotalTime(ServerScript_HeadGlitchPlayerArg(v)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_GetPeakScore(HSQUIRRELVM v)
{
	sq_pushfloat(v, HeadGlitch_GetPeakScore(ServerScript_HeadGlitchPlayerArg(v)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_GetEpisodeCount(HSQUIRRELVM v)
{
	sq_pushinteger(v, HeadGlitch_GetEpisodeCount(ServerScript_HeadGlitchPlayerArg(v)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_IsActive(HSQUIRRELVM v)
{
	sq_pushbool(v, HeadGlitch_IsActive(ServerScript_HeadGlitchPlayerArg(v)) ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_IsEnabled(HSQUIRRELVM v)
{
	sq_pushbool(v, HeadGlitch_IsEnabled() ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_IsFlagged(HSQUIRRELVM v)
{
	sq_pushbool(v, HeadGlitch_IsFlagged(ServerScript_HeadGlitchPlayerArg(v)) ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_GetFlagCount(HSQUIRRELVM v)
{
	sq_pushinteger(v, HeadGlitch_GetFlagCount(ServerScript_HeadGlitchPlayerArg(v)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_ResetScore(HSQUIRRELVM v)
{
	HeadGlitch_ResetScore(ServerScript_HeadGlitchPlayerArg(v));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_HeadGlitch_Reset(HSQUIRRELVM v)
{
	HeadGlitch_ClearPlayer(ServerScript_HeadGlitchPlayerArg(v));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Entity pointer table is m_pEdicts[idx + 0x7808]; a null slot is free.
static constexpr int EDICT_ENT_PTR_BASE = 0x7808;

static int ServerScript_CountUsedEdicts(void)
{
	int used = 0;
	if (!gpGlobals || !gpGlobals->m_pEdicts)
		return 0;

	for (int idx = 0; idx < MAX_EDICTS; ++idx)
	{
		const uintptr_t ent = static_cast<uintptr_t>(
			gpGlobals->m_pEdicts[idx + EDICT_ENT_PTR_BASE]);
		if (ent)
			++used;
	}
	return used;
}

static SQRESULT ServerScript_GetEdictUsed(HSQUIRRELVM v)
{
	sq_pushinteger(v, ServerScript_CountUsedEdicts());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_GetEdictMax(HSQUIRRELVM v)
{
	sq_pushinteger(v, MAX_EDICTS);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void Script_RegisterDedicatedS21ServerNatives(CSquirrelVM* s)
{
	// TraceLine arity is not gated on IsDedicatedRuntime -- g_pServer may still
	// be null at CSquirrelVM::Init callback time on the dedi boot path.
	Script_RegisterTraceLineEntitiesOnlyArity(s);

    if (!ServerScript_IsDedicatedRuntime())
        return;

	Script_RegisterClassVarNatives(s);

	// S3 parity with S21 native TraceHullHighDetail (ray.m_detailLevel = HIGH).
	// Signature matches engine TraceHull (array ignore + optional upDir/tracingEntity).
	Script_RegisterFuncNamed(s, "TraceHullHighDetail",
		"Server_Script_TraceHullHighDetail",
		"Hull sweep with TRACEDETAILLEVEL_HIGH (per-triangle statics; S21 native parity)",
		"TraceResults",
		"vector startPos, vector endPos, vector hullMins, vector hullMaxs, var ignoreEntOrArrayOfEnts = null, int traceMask = 0, int collisionGroup = 0, vector upDir = null, entity tracingEntity = null",
		true,
		ServerScript_TraceHullHighDetail);

	// S3 has no engine BreachTrace; native implementation below.
	Script_RegisterFuncNamed(s, "BreachTrace",
		"Server_Script_BreachTrace",
		"Trace through geo along direction to find a valid breach exit",
		"BreachTraceResults",
		"vector startPos, vector direction, vector hullMin, vector hullMax, float maxDist",
		true,
		ServerScript_BreachTrace);

	// Calc*Entity: EntityToWorldTransform + mathlib (S3 has no natives).
	if (v_CBaseEntity_EntityToWorldTransform)
	{
		Script_RegisterFuncNamed(s, "CalcLocalToWorldOrigin_Entity",
			"Server_Script_CalcLocalToWorldOrigin_Entity",
			"Transform a local-space origin into world space using the entity transform",
			"vector", "entity ent, vector localOrigin", true,
			ServerScript_CalcLocalToWorldOrigin_Entity);
		Script_RegisterFuncNamed(s, "CalcWorldToLocalOrigin_Entity",
			"Server_Script_CalcWorldToLocalOrigin_Entity",
			"Transform a world-space origin into the entity local space",
			"vector", "entity ent, vector worldOrigin", true,
			ServerScript_CalcWorldToLocalOrigin_Entity);
		Script_RegisterFuncNamed(s, "CalcLocalToWorldAngles_Entity",
			"Server_Script_CalcLocalToWorldAngles_Entity",
			"Transform local-space angles into world space using the entity transform",
			"vector", "entity ent, vector localAngles", true,
			ServerScript_CalcLocalToWorldAngles_Entity);
		Script_RegisterFuncNamed(s, "CalcWorldToLocalAngles_Entity",
			"Server_Script_CalcWorldToLocalAngles_Entity",
			"Transform world-space angles into the entity local space",
			"vector", "entity ent, vector worldAngles", true,
			ServerScript_CalcWorldToLocalAngles_Entity);
		Msg(eDLL_T::SERVER, "[XFORM] EntityToWorld transform natives registered\n");
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[XFORM] EntityToWorldTransform unresolved -- Calc*Origin/Angles_Entity not registered\n");
	}

    Script_RegisterFuncNamed(s, "ObjectPlacementSpecial_TraceForMoverBlocking",
        "Script_PlacementTraceForMoverBlockingShim",
        "Checks whether a special placement point is blocked by mover rules",
        "bool",
        "vector origin, vector surfaceNormal, entity mover",
        false,
        ServerScript_PlacementTraceForMoverBlockingShim);

    Script_RegisterFuncNamed(s, "GetObjectPlacementSpecialOrientationFromAngles",
        "Script_GetPlacementSpecialOrientationFromAnglesShim",
        "Converts a placement surface normal to a placement orientation",
        "int",
        "vector surfaceNormal",
        false,
        ServerScript_GetPlacementSpecialOrientationFromAnglesShim);

    Script_RegisterFuncNamed(s, "PlayerMelee_ClearPlayerAsLungeTarget",
        "Script_PlayerMeleeClearLungeTargetShim",
        "Clears any melee lunge targeting state for a player",
        "void",
        "entity player, bool clear",
        false,
        ServerScript_PlayerMeleeClearLungeTargetShim);

    Script_RegisterFuncNamed(s, "SetIsPermanentEntity",
        "Script_SetIsPermanentEntity",
        "Marks an entity as permanent so it is flagged as such to clients",
        "void",
        "entity ent, bool isPermanent",
        false,
        ServerScript_SetIsPermanentEntity);

    ServerScript_ResolveEmitSoundAtPositionExcept();
    Script_RegisterFuncNamed(s, "EmitWhizbySoundExceptToPlayer",
        "Script_EmitWhizbySoundExceptToPlayer",
        "Emits a positional whizby sound at the mid-point of two positions, excluding a player",
        "void",
        "int team, entity exceptPlayer, vector ep0, vector ep1, string sound",
        false,
        ServerScript_EmitWhizbySoundExceptToPlayer);

    Script_RegisterFuncNamed(s, "StartParticleEffectInWorldForRealms",
        "Script_StartParticleEffectInWorldForRealmsShim",
        "Starts a world particle effect for realm-filtered viewers",
        "entity ornull",
        "int particleIndex, vector origin, vector angles, entity viewer",
        false,
        ServerScript_StartParticleEffectInWorldForRealmsShim);

    Script_RegisterFuncNamed(s, "PutEntityInSomewhatSafeSpot",
        "Script_PutEntityInSomewhatSafeSpot",
        "Puts an entity in a position that is not in solid, without the tomb check",
        "void",
        "entity ent, entity ornull referenceEnt, entity ornull groundEnt, vector safeStartOrigin, vector endOrigin",
        false,
        ServerScript_PutEntityInSomewhatSafeSpot);

    Script_RegisterFuncNamed(s, "CanPutPlayerInSafeSpot",
        "Script_CanPutPlayerInSafeSpot",
        "Solves a safe position for a player without moving them; null if none exists",
        "vector ornull",
        "entity player, entity ornull referenceEnt, entity ornull groundEnt, bool testHighCollision, bool allowNavNodesAsBackup, vector safeStartOrigin, vector endOrigin",
        false,
        ServerScript_CanPutPlayerInSafeSpot);

    Script_RegisterFuncNamed(s, "EmitSoundOnEntityExceptToPlayers",
        "Script_EmitSoundOnEntityExceptToPlayers",
        "Emits a sound on an entity for everyone except the given players",
        "void",
        "entity soundEnt, array<entity> excludePlayers, string soundName",
        false,
        ServerScript_EmitSoundOnEntityExceptToPlayers);

	Translocation_RegisterFreeFuncs(s);

	Script_RegisterFuncNamed(s, "HeadGlitch_GetScore",
		"Script_HeadGlitch_GetScore",
		"Accumulated head-glitch abuse score for a player",
		"float", "entity player", false, ServerScript_HeadGlitch_GetScore);
	Script_RegisterFuncNamed(s, "HeadGlitch_GetExposure",
		"Script_HeadGlitch_GetExposure",
		"Fraction of a player's silhouette their current target can see; 1.0 when not glitching",
		"float", "entity player", false, ServerScript_HeadGlitch_GetExposure);
	Script_RegisterFuncNamed(s, "HeadGlitch_GetHoldTime",
		"Script_HeadGlitch_GetHoldTime",
		"Seconds a player has held the current head-glitch pose, 0 when not in one",
		"float", "entity player", false, ServerScript_HeadGlitch_GetHoldTime);
	Script_RegisterFuncNamed(s, "HeadGlitch_IsFlagged",
		"Script_HeadGlitch_IsFlagged",
		"True once a player's score crossed bridge_headglitch_flag",
		"bool", "entity player", false, ServerScript_HeadGlitch_IsFlagged);
	Script_RegisterFuncNamed(s, "HeadGlitch_IsActive",
		"Script_HeadGlitch_IsActive",
		"True while a player is standing in head-glitch geometry right now",
		"bool", "entity player", false, ServerScript_HeadGlitch_IsActive);
	Script_RegisterFuncNamed(s, "HeadGlitch_GetPeakScore",
		"Script_HeadGlitch_GetPeakScore",
		"Highest head-glitch score a player has reached since their last reset",
		"float", "entity player", false, ServerScript_HeadGlitch_GetPeakScore);
	Script_RegisterFuncNamed(s, "HeadGlitch_GetTotalTime",
		"Script_HeadGlitch_GetTotalTime",
		"Total seconds a player has spent in scoring head-glitch episodes",
		"float", "entity player", false, ServerScript_HeadGlitch_GetTotalTime);
	Script_RegisterFuncNamed(s, "HeadGlitch_GetEpisodeCount",
		"Script_HeadGlitch_GetEpisodeCount",
		"Number of scoring head-glitch episodes a player has completed",
		"int", "entity player", false, ServerScript_HeadGlitch_GetEpisodeCount);
	Script_RegisterFuncNamed(s, "HeadGlitch_IsEnabled",
		"Script_HeadGlitch_IsEnabled",
		"True when head-glitch detection is armed for this gamemode",
		"bool", "", false, ServerScript_HeadGlitch_IsEnabled);
	Script_RegisterFuncNamed(s, "HeadGlitch_GetFlagCount",
		"Script_HeadGlitch_GetFlagCount",
		"Number of times a player has crossed the flag threshold this session",
		"int", "entity player", false, ServerScript_HeadGlitch_GetFlagCount);
	Script_RegisterFuncNamed(s, "HeadGlitch_ResetScore",
		"Script_HeadGlitch_ResetScore",
		"Ends the live pose and clears the score and flag, keeping the session tally",
		"void", "entity player", false, ServerScript_HeadGlitch_ResetScore);
	Script_RegisterFuncNamed(s, "HeadGlitch_Reset",
		"Script_HeadGlitch_Reset",
		"Clears a player's head-glitch score, flag and session tally",
		"void", "entity player", false, ServerScript_HeadGlitch_Reset);

	Script_RegisterFuncNamed(s, "GetEdictUsed",
		"Server_Script_GetEdictUsed",
		"Count of live edicts currently occupying the server entity table",
		"int", "", false, ServerScript_GetEdictUsed);
	Script_RegisterFuncNamed(s, "GetEdictMax",
		"Server_Script_GetEdictMax",
		"Maximum edict slots on this server",
		"int", "", false, ServerScript_GetEdictMax);

	// Cafe_PlayerPrefs_* are player methods; a free function would pass the root table as this.
}


void Script_RegisterDedicatedEntityNatives(ScriptClassDescriptor_t* entityStruct)
{
	if (!entityStruct)
		return;

    entityStruct->AddFunction(
        "Zipline_IsCurvedZipline",
        "Script_Zipline_IsCurvedZipline",
        "Returns true if this zipline uses the S21 curved zipline path.",
        "bool",
        "",
        false,
        Script_Zipline_IsCurvedZipline);
    entityStruct->AddFunction(
        "Zipline_SetRopeColorModulation",
        "Script_Zipline_SetRopeColorModulation",
        "Sets the zipline rope colour modulation.",
        "void",
        "vector color",
        false,
        Script_Zipline_SetRopeColorModulation);
    entityStruct->AddFunction(
        "Zipline_GetRopeColorModulation",
        "Script_Zipline_GetRopeColorModulation",
        "Returns the zipline rope colour modulation.",
        "vector",
        "",
        false,
        Script_Zipline_GetRopeColorModulation);
    // Class-name check inside each body: only info_loot_ceremony_harvester
    // touches +0x15E0.
    entityStruct->AddFunction(
        "GetUseStateByIndex",
        "Script_GetUseStateByIndex",
        "Returns the material harvester collected-state bit for a player index.",
        "bool",
        "int index",
        false,
        Script_GetUseStateByIndex);
    entityStruct->AddFunction(
        "SetUseStateByIndex",
        "Script_SetUseStateByIndex",
        "Sets the material harvester collected-state bit for a player index.",
        "void",
        "int index, bool used",
        false,
        Script_SetUseStateByIndex);
    entityStruct->AddFunction(
        "IsMoverOrChildOfMover",
        "Script_IsMoverOrChildOfMover",
        "Returns true if this entity, or an ancestor in its move-parent chain, is a MOVETYPE_PUSH mover.",
        "bool",
        "",
        false,
        Script_IsMoverOrChildOfMover);
    entityStruct->AddFunction(
        "SetEnableScriptAnimModifier",
        "Script_SetEnableScriptAnimModifier",
        "Flags this entity's animation as script-driven (networked via m_animNetworkFlags).",
        "void",
        "bool enable",
        false,
        Script_SetEnableScriptAnimModifier);
    // Write half of DT_LootRoller m_tier / m_hasVaultKey. The dedi parent
    // factory has no such members.
    entityStruct->AddFunction(
        "SetTier",
        "Script_SetTier",
        "Sets this loot roller's loot tier (networked; drives the client eye FX colour).",
        "void",
        "int tier",
        false,
        Script_SetTier);
    entityStruct->AddFunction(
        "SetHasVaultKey",
        "Script_SetHasVaultKey",
        "Flags this loot roller as carrying a vault key (networked; drives the red eye FX hints).",
        "void",
        "bool hasVaultKey",
        false,
        Script_SetHasVaultKey);
    entityStruct->AddFunction(
        "SetLootGrabDist",
        "Script_SetLootGrabDist",
        "Sets the radius this loot grabber pulls nearby loot from (networked).",
        "void",
        "float dist",
        false,
        Script_SetLootGrabDist);
    entityStruct->AddFunction(
        "GetLootGrabDist",
        "Script_GetLootGrabDist",
        "Gets the radius this loot grabber pulls nearby loot from.",
        "float",
        "",
        false,
        Script_GetLootGrabDist);
    entityStruct->AddFunction(
        "SetIsVendingMachine",
        "Script_SetIsVendingMachine",
        "Marks this loot grabber as the firing range vending machine (networked).",
        "void",
        "",
        false,
        Script_SetIsVendingMachine);
    entityStruct->AddFunction(
        "IsVendingMachine",
        "Script_IsVendingMachine",
        "Returns true if this loot grabber is the firing range vending machine.",
        "bool",
        "",
        false,
        Script_IsVendingMachine);
    entityStruct->AddFunction(
        "IsLinkedBox",
        "Script_IsLinkedBox",
        "Returns true if this loot grabber sources its loot from a linked box.",
        "bool",
        "",
        false,
        Script_IsLinkedBox);
    entityStruct->AddFunction(
        "IncrementPlayersGrabbingLoot",
        "Script_IncrementPlayersGrabbingLoot",
        "Adds one to the count of players with this grabber's ground list open (networked).",
        "void",
        "",
        false,
        Script_IncrementPlayersGrabbingLoot);
    entityStruct->AddFunction(
        "DecrementPlayersGrabbingLoot",
        "Script_DecrementPlayersGrabbingLoot",
        "Removes one from the count of players with this grabber's ground list open (networked).",
        "void",
        "",
        false,
        Script_DecrementPlayersGrabbingLoot);
    entityStruct->AddFunction(
        "SetImpactEffectColorID",
        "Script_SetImpactEffectColorID",
        "Sets the palette colour ID this loot grabber tints its bullet impact FX with (networked).",
        "void",
        "int colorID",
        false,
        Script_SetImpactEffectColorID);
    // CCarePackageInsightProp (prop_care_package_insight) -- the pathfinder
    // perk's hidden/revealed airdrop markers.
    entityStruct->AddFunction(
        "SetLootIndex",
        "Script_SetLootIndex",
        "Sets the survival loot index this care package marker reveals (networked).",
        "void",
        "int lootIndex",
        false,
        Script_SetLootIndex);
    entityStruct->AddFunction(
        "GetLootIndex",
        "Script_GetLootIndex",
        "Gets the survival loot index this care package marker reveals.",
        "int",
        "",
        false,
        Script_GetLootIndex);
    entityStruct->AddFunction(
        "SetAreContentsTaken",
        "Script_SetAreContentsTaken",
        "Flags this care package marker's best item as looted (networked; greys the map icon).",
        "void",
        "bool contentsTaken",
        false,
        Script_SetAreContentsTaken);
    entityStruct->AddFunction(
        "GetAreContentsTaken",
        "Script_GetAreContentsTaken",
        "Returns true if this care package marker's best item has been looted.",
        "bool",
        "",
        false,
        Script_GetAreContentsTaken);
    entityStruct->AddFunction(
        "SetCanBeMeleedByOwner",
        "Script_SetCanBeMeleedByOwner",
        "Allows or blocks this entity being meleed by its own owner (networked).",
        "void",
        "bool canBeMeleed",
        false,
        Script_SetCanBeMeleedByOwner);
    entityStruct->AddFunction(
        "DisallowObjectPlacement",
        "Script_DisallowObjectPlacement",
        "Blocks players placing deployables on or against this entity (networked).",
        "void",
        "",
        false,
        Script_DisallowObjectPlacement);
    entityStruct->AddFunction(
        "SetIgnoreMoveParentRotation",
        "Script_SetIgnoreMoveParentRotation",
        "Stops this entity inheriting its move-parent's rotation (networked).",
        "void",
        "",
        false,
        Script_SetIgnoreMoveParentRotation);
    entityStruct->AddFunction(
        "Anim_EnableRelativeToGround",
        "Script_Anim_EnableRelativeToGround",
        "Projects this entity's scripted-anim movement onto the ground (networked).",
        "void",
        "",
        false,
        Script_Anim_EnableRelativeToGround);
    entityStruct->AddFunction(
        "SetAimAssistAllowed",
        "Script_SetAimAssistAllowed",
        "Allows or excludes this entity from controller aim assist (networked).",
        "void",
        "bool allowed",
        false,
        Script_SetAimAssistAllowed);
}

void Script_RegisterDedicatedPlayerNatives(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

    playerStruct->AddFunction("Cafe_PlayerPrefs_Write",
        "ScriptCafePlayerPrefsWrite",
        "Write this player's preferences blob to disk. Returns true on success",
        "bool",
        "string key, string payload",
        false,
        ServerScript_CafePlayerPrefs_Write);

    playerStruct->AddFunction("Cafe_PlayerPrefs_Read",
        "ScriptCafePlayerPrefsRead",
        "Read this player's preferences blob from disk. Returns an empty string when absent",
        "string",
        "string key",
        false,
        ServerScript_CafePlayerPrefs_Read);

    playerStruct->AddFunction(
        "SetShadowShieldIsActive",
        "Script_SetShadowShieldIsActive",
        "Publishes whether this player currently has an active shadow shield (networked).",
        "void",
        "bool isActive",
        false,
        Script_SetShadowShieldIsActive);
    playerStruct->AddFunction(
        "EnterShadowFormFortified",
        "Script_EnterShadowFormFortified",
        "Marks this player as being in the fortified shadow form state.",
        "void",
        "",
        false,
        Script_EnterShadowFormFortified);
    playerStruct->AddFunction(
        "IsSlipping",
        "Script_IsSlipping",
        "Returns true if this player is currently inside a slip trigger.",
        "bool",
        "",
        false,
        Script_IsSlipping);

    TrackEntity_RegisterScriptFunctions(playerStruct);
}

void Script_DedicatedTraceDetour(const bool bAttach)
{
	if (g_pEngineTraceServerVFTable)
	{
		if (bAttach)
		{
			if (!v_EngineTraceRayFiltered)
			{
				CMemory::HookVirtualMethod(
					reinterpret_cast<uintptr_t>(g_pEngineTraceServerVFTable),
					reinterpret_cast<const void*>(&Hook_EngineTraceRayFiltered),
					kEngineTrace_TraceRayFiltered,
					reinterpret_cast<void**>(&v_EngineTraceRayFiltered));
			}
			if (!v_EngineTraceRayFiltered)
				Warning(eDLL_T::SERVER, "[TRACE] TraceRayFiltered vtable hook failed -- TraceHullHighDetail stays NORMAL\n");
			else
				LogFunAdr("EngineTraceRayFiltered", v_EngineTraceRayFiltered);
		}
		else if (v_EngineTraceRayFiltered)
		{
			void* discarded = nullptr;
			CMemory::HookVirtualMethod(
				reinterpret_cast<uintptr_t>(g_pEngineTraceServerVFTable),
				reinterpret_cast<const void*>(v_EngineTraceRayFiltered),
				kEngineTrace_TraceRayFiltered,
				&discarded);
			v_EngineTraceRayFiltered = nullptr;
		}
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER, "[TRACE] g_pEngineTraceServerVFTable null -- TraceHullHighDetail stays NORMAL\n");
	}
}
