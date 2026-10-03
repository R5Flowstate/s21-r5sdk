//=============================================================================//
//
// Purpose: Portal gun server natives and gun state. Placement itself lives in
// portal_placement; the reference gun props are kept in a server-only sidecar
// because S21 DT_WeaponX has no RecvProp for them (appending unknown props
// would shear the bit-exact wire).
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "mathlib/mathlib.h"
#include "public/globalvars_base.h"
#include "engine/server/server.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/dt_extend.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/util_shared.h"
#include "game/server/player.h"
#include "game/server/util_server.h"
#include "game/server/entitylist.h"
#include "game/server/baseentity.h"
#include "game/server/gameinterface.h"
#include "game/server/r1/weapon_x.h"
#include "game/server/portal/weapon_portalgun.h"
#include "game/server/portal/portal_placement.h"
#include "game/server/portal/prop_portal.h"
#include "game/shared/portal/portal_placement_shared.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"

static constexpr ptrdiff_t WEAPONGUN_OFF_SERVERCLASS = 0x50;
static constexpr const char* kWeaponGunClass = "CWeaponX";

struct PortalGunState_t
{
	bool m_bCanFirePortal1;
	bool m_bCanFirePortal2;
	int m_iLastFiredPortal;
	bool m_bOpenProngs;
	float m_fCanPlacePortal1OnThisSurface;
	float m_fCanPlacePortal2OnThisSurface;
	int m_EffectState;
};

static SDKEntityMap<PortalGunState_t> s_gunState(ESide::Server, "portalgun.state.srv");
static SDKEntityMap<float> s_gunLastFire(ESide::Server, "portalgun.lastfire.srv");
static EntityDestroySubscription* s_gunDestroySub = nullptr;

static const char* PortalGun_ResultName(int r)
{
	switch (r)
	{
	case 0: return "SUCCESS";
	case 1: return "BUMPED";
	case 2: return "CANT_FIT";
	case 3: return "OVERLAP_LINKED";
	case 4: return "NEAR";
	case 5: return "INVALID_VOLUME";
	case 6: return "INVALID_SURFACE";
	case 7: return "PASSTHROUGH_SURFACE";
	case 8: return "RATE_LIMITED";
	default: return "?";
	}
}

static const char* PortalGun_ServerClassName(const void* ent)
{
	if (!ent)
		return nullptr;
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const uint8_t*>(ent) + WEAPONGUN_OFF_SERVERCLASS, sizeof(uintptr_t)))
		return nullptr;
	const uintptr_t sc = *reinterpret_cast<const uintptr_t*>(
		reinterpret_cast<const uint8_t*>(ent) + WEAPONGUN_OFF_SERVERCLASS);
	if (!sc || !DTExtend_IsSafeToRead(reinterpret_cast<const void*>(sc), sizeof(uintptr_t)))
		return nullptr;
	return *reinterpret_cast<const char* const*>(sc);
}

static bool PortalGun_IsWeaponEntity(const void* ent)
{
	const char* cn = PortalGun_ServerClassName(ent);
	return cn && strcmp(cn, kWeaponGunClass) == 0;
}

static void* PortalGun_ResolveHandle(const EHANDLE& h)
{
	const uint32_t raw = static_cast<uint32_t>(h.ToInt());
	if (raw == (uint32_t)INVALID_EHANDLE_INDEX || !g_serverEntityList)
		return nullptr;
	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(raw));
	if (void* const pEntity = g_serverEntityList->LookupEntity(handle))
		return pEntity;
	const int entIndex = static_cast<int>(raw & ENT_ENTRY_MASK);
	if (entIndex >= 0 && entIndex < NUM_ENT_ENTRIES)
		return g_serverEntityList->LookupEntityByNetworkIndex(entIndex);
	return nullptr;
}

static CPlayer* PortalGun_WeaponOwnerPlayer(void* pWeapon)
{
	if (!pWeapon)
		return nullptr;
	const EHANDLE& hOwner = reinterpret_cast<CWeaponX*>(pWeapon)->GetWeaponOwnerHandle();
	void* pOwner = PortalGun_ResolveHandle(hOwner);
	if (!pOwner || !g_ServerGlobalVariables)
		return nullptr;
	const int max = g_ServerGlobalVariables->maxClients;
	for (int i = 1; i <= max && i < 128; ++i)
	{
		if (UTIL_PlayerByIndex(i) == reinterpret_cast<CPlayer*>(pOwner))
			return reinterpret_cast<CPlayer*>(pOwner);
	}
	return nullptr;
}

static uint32_t PortalGun_EntityHandle(const void* pEntity)
{
	if (!pEntity)
		return 0;
	if (!DTExtend_IsSafeToRead(reinterpret_cast<const uint8_t*>(pEntity) + 0x08, sizeof(uint32_t)))
		return 0;
	return *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(pEntity) + 0x08);
}

static void PortalGun_OnDestroy(SDKEntityHandle handle, void* pEntity, ESide side, void* userData)
{
	(void)handle; (void)side; (void)userData;
	if (!pEntity)
		return;
	if (PortalEntity_OnEntityDestroyed(pEntity) > 0)
		return;
	if (PortalGun_IsWeaponEntity(pEntity))
	{
		CPlayer* pOwner = PortalGun_WeaponOwnerPlayer(pEntity);
		if (pOwner)
			PortalEntity_CloseOwnerPair(PortalGun_EntityHandle(pOwner));
	}
}

static void PortalGun_EnsureDestroySub(void)
{
	if (!s_gunDestroySub)
		s_gunDestroySub = SDKEntityState_SubscribeDestroy(PortalGun_OnDestroy, nullptr, ESide::Server);
}

void PortalGun_LevelShutdown(void)
{
	s_gunState.Clear();
	s_gunLastFire.Clear();
}

void Portal_OnPlayerRunCommand(void* pPlayerVoid)
{
	CPlayer* pPlayer = reinterpret_cast<CPlayer*>(pPlayerVoid);
	if (!pPlayer)
		return;
	if (PortalEntity_ActiveCount() <= 0)
		return;
	if (pPlayer->GetLifeState() == 0)
		return;
	PortalEntity_CloseOwnerPair(PortalGun_EntityHandle(pPlayer));
}

// Eye height plus crouch/mantle slack above the owner's feet.
static constexpr float kPortalShotMaxOriginDist = 128.0f;

static bool PortalGun_ValidateShot(CPlayer* pPlayer, const Vector3D& origin, const Vector3D& dir)
{
	if (!origin.IsValid() || !dir.IsValid())
		return false;
	const float len2 = dir.LengthSqr();
	if (len2 < 0.81f || len2 > 1.21f)
		return false;
	const Vector3D& feet = reinterpret_cast<CBaseEntity*>(pPlayer)->Diag_AbsOrigin();
	return (origin - feet).LengthSqr() <= kPortalShotMaxOriginDist * kPortalShotMaxOriginDist;
}

static int PortalGun_FireInternal(void* pWeapon, int slot, const Vector3D& shotOrigin, const Vector3D& shotDir)
{
	if (!pWeapon || !PortalGun_IsWeaponEntity(pWeapon))
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] Fire refused: not a weapon entity\n");
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED;
	}
	if (slot < 0 || slot > 1)
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] Fire refused: bad portalIndex %d\n", slot);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED;
	}
	CPlayer* pPlayer = PortalGun_WeaponOwnerPlayer(pWeapon);
	if (!pPlayer)
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] Fire refused: weapon has no player owner\n");
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED;
	}
	if (pPlayer->GetLifeState() != 0)
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] Fire refused: owner dead\n");
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED;
	}
	const float curTime = gpGlobals ? gpGlobals->curTime : 0.0f;
	const SDKEntityHandle wh = SDKEntityState_GetHandle(pWeapon);
	if (const float* pLast = s_gunLastFire.Find(wh))
	{
		if (curTime - *pLast < kPortalFireDelay)
			return (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED;
	}
	if (!PortalGun_ValidateShot(pPlayer, shotOrigin, shotDir))
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] Fire refused: shot (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) invalid for owner\n",
			shotOrigin.x, shotOrigin.y, shotOrigin.z, shotDir.x, shotDir.y, shotDir.z);
		return (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED;
	}
	s_gunLastFire[wh] = curTime;
	PortalGun_EnsureDestroySub();

	PortalShot_t shot;
	shot.m_pPlayer = pPlayer;
	shot.m_eye = shotOrigin;
	shot.m_dir = shotDir;

	const uint32_t ownerHandle = PortalGun_EntityHandle(pPlayer);
	const void* pIgnore = nullptr;
	{
		struct Ctx_t { uint32_t m_owner; int m_slot; const void* m_found; };
		Ctx_t ctx{ ownerHandle, slot, nullptr };
		struct V { static bool Fn(const PortalPlacementInfo_t* info, void* c) {
			Ctx_t* x = reinterpret_cast<Ctx_t*>(c);
			if (info->m_ownerHandle == x->m_owner && info->m_slot == x->m_slot) { x->m_found = info->m_portalPtr; return false; }
			return true;
		} };
		PortalEntity_VisitActive(V::Fn, &ctx);
		pIgnore = ctx.m_found;
	}

	PortalPlaceOut_t placeOut;
	const int result = PortalPlacement_Place(shot, slot, pIgnore, &placeOut);

	if (result != (int)ePortalPlaceResult_t::PORTAL_PLACE_SUCCESS && result != (int)ePortalPlaceResult_t::PORTAL_PLACE_BUMPED)
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] fire slot=%d %s (no portal)\n",
			slot, PortalGun_ResultName(result));
	}

	PortalGunState_t& st = s_gunState[wh];
	st.m_bCanFirePortal1 = true;
	st.m_bCanFirePortal2 = true;
	if (slot == 0)
		st.m_fCanPlacePortal1OnThisSurface = placeOut.m_analog;
	else
		st.m_fCanPlacePortal2OnThisSurface = placeOut.m_analog;

	if (result == (int)ePortalPlaceResult_t::PORTAL_PLACE_SUCCESS || result == (int)ePortalPlaceResult_t::PORTAL_PLACE_BUMPED)
	{
		if (!pIgnore && PortalEntity_ActiveCount() >= kPortalMaxActive)
		{
			Warning(eDLL_T::SERVER, "[PORTAL-GUN] global cap %d hit -- placement refused\n", kPortalMaxActive);
			return (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED;
		}
		bool reused = false;
		void* pPortal = PortalEntity_Acquire(ownerHandle, slot, &reused);
		if (!pPortal)
		{
			Warning(eDLL_T::SERVER, "[PORTAL-GUN] global cap %d hit -- placement refused\n", kPortalMaxActive);
			return (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED;
		}
		PortalEntity_NewLocation(pPortal, placeOut.m_pos, placeOut.m_ang);
		st.m_iLastFiredPortal = slot + 1;
		st.m_bOpenProngs = true;
		st.m_EffectState++;
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] fire slot=%d %s %s pos=(%.1f %.1f %.1f)\n",
			slot, PortalGun_ResultName(result), reused ? "reused" : "created",
			placeOut.m_pos.x, placeOut.m_pos.y, placeOut.m_pos.z);
	}
	return result;
}

static SQRESULT ServerScript_PortalGunFire(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity || !v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] Fire refused: no weapon\n");
		sq_pushinteger(v, (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	SQInteger slot = 0;
	const SQVector3D* pOrigin = nullptr;
	const SQVector3D* pDir = nullptr;
	if (sq_getinteger(v, 2, &slot) != SQ_OK
		|| SQ_FAILED(sq_getvector(v, 3, &pOrigin)) || !pOrigin
		|| SQ_FAILED(sq_getvector(v, 4, &pDir)) || !pDir)
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] Fire refused: expected (int portalIndex, vector origin, vector dir)\n");
		sq_pushinteger(v, (int)ePortalPlaceResult_t::PORTAL_PLACE_RATE_LIMITED);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const Vector3D origin(pOrigin->x, pOrigin->y, pOrigin->z);
	const Vector3D dir(pDir->x, pDir->y, pDir->z);
	const int result = PortalGun_FireInternal(pWeapon, (int)slot, origin, dir);
	sq_pushinteger(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_PortalGunClosePortals(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity || !v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] ClosePortals refused: no weapon\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (!PortalGun_IsWeaponEntity(pWeapon))
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] ClosePortals refused: not a weapon entity\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	CPlayer* pPlayer = PortalGun_WeaponOwnerPlayer(pWeapon);
	if (!pPlayer)
	{
		Warning(eDLL_T::SERVER, "[PORTAL-GUN] ClosePortals refused: weapon has no player owner\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const int n = PortalEntity_CloseOwnerPair(PortalGun_EntityHandle(pPlayer));
	s_gunState.Erase(SDKEntityState_GetHandle(pWeapon));
	Warning(eDLL_T::SERVER, "[PORTAL-GUN] close portals=%d\n", n);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Weapon methods: the weapon is the call's "this", which is what sq_getentity reads.
void PortalGun_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	if (!weaponStruct)
		return;
	weaponStruct->AddFunction("PortalGun_Fire",
		"Server_Script_PortalGunFire",
		"Runs portal placement for this weapon's pair slot from a shot origin near the owner and a unit direction, returns ePortalPlaceResult",
		"int", "int portalIndex, vector origin, vector dir", false,
		ServerScript_PortalGunFire);
	weaponStruct->AddFunction("PortalGun_ClosePortals",
		"Server_Script_PortalGunClosePortals",
		"Closes this weapon owner's portal pair",
		"void", "", false,
		ServerScript_PortalGunClosePortals);
	Msg(eDLL_T::SERVER, "[PORTAL-GUN] weapon methods registered\n");
}

static void CC_PortalDevPlace(const CCommand& args)
{
	int slot = 0;
	if (args.ArgC() >= 2)
		slot = atoi(args.Arg(1));
	if (slot < 0 || slot > 1)
	{
		Msg(eDLL_T::SERVER, "usage 'portal_dev_place': <0|1>\n");
		return;
	}
	if (!g_pServer || !g_pServer->IsActive())
	{
		Msg(eDLL_T::SERVER, "portal_dev_place: server not active\n");
		return;
	}
	CPlayer* pPlayer = UTIL_GetCommandClient();
	if (!pPlayer)
	{
		Msg(eDLL_T::SERVER, "portal_dev_place: no calling player\n");
		return;
	}
	Vector3D eye(0, 0, 0);
	QAngle eyeAng(0, 0, 0);
	pPlayer->EyePosition(&eye);
	pPlayer->EyeAngles(&eyeAng);
	Vector3D dir(0, 0, 0);
	AngleVectors(eyeAng, &dir, nullptr, nullptr);

	PortalShot_t shot;
	shot.m_pPlayer = pPlayer;
	shot.m_eye = eye;
	shot.m_dir = dir;

	PortalPlaceOut_t placeOut;
	const int result = PortalPlacement_Place(shot, slot, nullptr, &placeOut);
	Msg(eDLL_T::SERVER, "portal_dev_place: slot=%d result=%d (%s)%s\n",
		slot, result, PortalGun_ResultName(result),
		(result == 0 || result == 1) ? "" : " (portal not moved)");
	if (result == (int)ePortalPlaceResult_t::PORTAL_PLACE_SUCCESS || result == (int)ePortalPlaceResult_t::PORTAL_PLACE_BUMPED)
	{
		bool reused = false;
		void* pPortal = PortalEntity_Acquire(PortalGun_EntityHandle(pPlayer), slot, &reused);
		if (pPortal)
			PortalEntity_NewLocation(pPortal, placeOut.m_pos, placeOut.m_ang);
	}
}

static ConCommand portal_dev_place("portal_dev_place", CC_PortalDevPlace,
	"Dev-only: run full portal placement from the calling player's eye. Usage: portal_dev_place <0|1>",
	FCVAR_DEVELOPMENTONLY);
