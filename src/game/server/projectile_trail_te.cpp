//=============================================================================//
//
// Purpose: projectile trail temp entity gate.
//
// A bolt that hits during lag-compensation catch-up dies before it is ever
// networked; other players see its tracer only through this temp entity. The
// sender skips it when the server trail name (projectile_trail_effect_<n>) is
// empty, and S21 weapon files only set projectile_trail_effect_<n>_3p. The
// client picks the effect from its own weapon data, so the name only gates.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "projectile_trail_te.h"
#include "weapon_kv_s21_ext.h"
#include "game/shared/sdk_entity_state.h"

// Server-half CProjectile.
static constexpr ptrdiff_t PTT_OFF_MODBITS       = 0x120C; // u32 m_modBitfield
static constexpr ptrdiff_t PTT_OFF_TRAIL_INDEX   = 0x1214; // int m_projectileTrailIndex
static constexpr ptrdiff_t PTT_OFF_WEAPON_SOURCE = 0x1224; // EHANDLE of the firing weapon
static constexpr ptrdiff_t PTT_OFF_PREIGN_TRAIL  = 0x2170; // char* pre_ignition_trail_effect
static constexpr ptrdiff_t PTT_OFF_TRAIL_0       = 0x2210; // char* projectile_trail_effect_0, _1.._4 follow
static constexpr ptrdiff_t PTT_OFF_IGNITED       = 0x2400; // bool
// Server-half CWeaponX.
static constexpr ptrdiff_t PTT_WEAPON_OFF_NAME   = 0x15B0; // char m_weaponName[65]

static ConVar bridge_projectile_trail_te("bridge_projectile_trail_te", "1", FCVAR_RELEASE,
	"Send the projectile trail temp entity for weapons whose trail is only defined "
	"under projectile_trail_effect_<n>_3p (close-range enemy tracers).");

static ConVar bridge_projectile_trail_te_diag("bridge_projectile_trail_te_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log each trail temp entity the S21 trail keys let through ([TRAIL-TE]).");

static void(__fastcall* v_ProjectileTrailTE_Send)(__int64 bolt, __int64 owner, const float* start, const float* end) = nullptr;

static const char s_szTrailPresent[] = "projectile_trail_effect_3p";
static volatile LONG s_nFilled = 0;

static const char** ProjectileTrailTE_GetSlot(const __int64 bolt)
{
	if (!*reinterpret_cast<const bool*>(bolt + PTT_OFF_IGNITED))
		return reinterpret_cast<const char**>(bolt + PTT_OFF_PREIGN_TRAIL);

	const int nIndex = *reinterpret_cast<const int*>(bolt + PTT_OFF_TRAIL_INDEX);
	const int nSlot = (nIndex >= 1 && nIndex < WEAPON_S21_TRAIL_COUNT) ? nIndex : 0;
	return reinterpret_cast<const char**>(bolt + PTT_OFF_TRAIL_0 + static_cast<ptrdiff_t>(8 * nSlot));
}

static void __fastcall Hook_ProjectileTrailTE_Send(const __int64 bolt, const __int64 owner, const float* start, const float* end)
{
	if (!bolt || !bridge_projectile_trail_te.GetBool())
	{
		v_ProjectileTrailTE_Send(bolt, owner, start, end);
		return;
	}

	const char** const pSlot = ProjectileTrailTE_GetSlot(bolt);
	const char* const pszOld = *pSlot;
	if (pszOld && pszOld[0])
	{
		v_ProjectileTrailTE_Send(bolt, owner, start, end);
		return;
	}

	const uint32_t weaponEH = *reinterpret_cast<const uint32_t*>(bolt + PTT_OFF_WEAPON_SOURCE);
	const void* const pWeapon = SDKEntityState_Resolve(SDKEntityHandle(weaponEH), ESide::Server);

	const int nIndex = *reinterpret_cast<const int*>(bolt + PTT_OFF_TRAIL_INDEX);
	const int nKvIndex = (nIndex >= 0 && nIndex < WEAPON_S21_TRAIL_COUNT) ? nIndex : 0;
	const uint32_t nModBits = *reinterpret_cast<const uint32_t*>(bolt + PTT_OFF_MODBITS);

	if (!pWeapon || !WeaponKVS21Ext_HasTrail3p(pWeapon, nModBits, nKvIndex))
	{
		v_ProjectileTrailTE_Send(bolt, owner, start, end);
		return;
	}

	*pSlot = s_szTrailPresent;
	v_ProjectileTrailTE_Send(bolt, owner, start, end);
	*pSlot = pszOld;

	const char* const pszWeapon = static_cast<const char*>(pWeapon) + PTT_WEAPON_OFF_NAME;
	const LONG n = InterlockedIncrement(&s_nFilled);
	if (n == 1)
		Msg(eDLL_T::SERVER, "[TRAIL-TE] first trail temp entity sent from an S21 trail key (%.64s)\n", pszWeapon);
	else if (bridge_projectile_trail_te_diag.GetBool() && (n <= 64 || (n % 256) == 0))
		Msg(eDLL_T::SERVER, "[TRAIL-TE] #%ld %.64s trail %d mods 0x%X\n", n, pszWeapon, nKvIndex, nModBits);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VProjectileTrailTE::GetAdr(void) const
{
	LogFunAdr("ProjectileTrailTE_Send", v_ProjectileTrailTE_Send);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VProjectileTrailTE::GetFun(void) const
{
	// Reduced-effects early out (+0x121C), then the ignited test (+0x2400)
	// that picks the pre-ignition trail slot (+0x2170).
	Module_FindPattern(g_GameDll,
		"48 89 54 24 10 55 53 41 54 41 56 41 57 48 8B EC 48 83 EC 70 "
		"80 B9 1C 12 00 00 00 4D 8B E1 4D 8B F8 48 8B DA 4C 8B F1 0F 85 ?? ?? ?? ?? "
		"80 B9 00 24 00 00 00 75 ?? 48 8B 81 70 21 00 00")
		.GetPtr(v_ProjectileTrailTE_Send);

	if (!v_ProjectileTrailTE_Send)
		Warning(eDLL_T::SERVER,
			"[TRAIL-TE] trail temp entity sender unresolved -- close-range enemy tracers stay off\n");
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VProjectileTrailTE::Detour(const bool bAttach) const
{
	if (v_ProjectileTrailTE_Send)
		DetourSetup(&v_ProjectileTrailTE_Send, &Hook_ProjectileTrailTE_Send, bAttach);
}
