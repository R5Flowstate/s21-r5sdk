//=============================================================================//
//
// Purpose: ADS/spread divergence probe (dedi side). See bridge_zoom_gate.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "bridge_zoom_gate.h"

#include "entitylist.h"
#include "mathlib/vector.h"
#include "weapon_kv_s21_ext.h"

#include <cctype>
#include <unordered_set>
#include <string>

typedef float(__fastcall* PFN_CPlayer_GetZoomFrac)(__int64 player);
typedef char(__fastcall* PFN_CPlayer_UpdateZoom)(__int64 player);
typedef void(__fastcall* PFN_CPlayer_StartZooming)(__int64 player);
typedef __int64(__fastcall* PFN_CBaseCombatCharacter_Weapon_GetTargetingWeapon)(__int64 player);
typedef __int64(__fastcall* PFN_CGameMovement_DecayPunchAngles)(__int64* movement);
typedef __int64(__fastcall* PFN_CWeaponX_FireWeaponBolt)(__int64 weapon, float* pos, float* dir,
	float speed, char adjustForZeroDist, int touchDmgType, int explosionDmgType,
	char clientPredicted, int additionalRandomSeed, char spreadOption, int projectileIndex);
typedef __int64(__fastcall* PFN_CWeaponX_UpdateWeaponSpread)(
	__int64 weapon, float speed, float topRegularSpeed, float topFastSpeed,
	char isMovingFast, char isWallRunning, char isWallHanging,
	float duckFrac, float zoomFrac, char onGround);
typedef Vector3D*(__fastcall* PFN_InterpOwnerVel)(void* unused, Vector3D* out, void* owner);
typedef void(__fastcall* PFN_CBaseEntity_SetAbsVelocity)(void* entity, const Vector3D* velocity);
typedef Vector3D*(__fastcall* PFN_CBaseEntity_GetAbsVelocity)(void* entity, Vector3D* out, int unused);

static PFN_CPlayer_GetZoomFrac       v_CPlayer_GetZoomFrac       = nullptr;
static PFN_CPlayer_UpdateZoom        v_CPlayer_UpdateZoom        = nullptr;
static PFN_CPlayer_StartZooming      v_CPlayer_StartZooming      = nullptr;
static PFN_CBaseCombatCharacter_Weapon_GetTargetingWeapon
	v_CBaseCombatCharacter_Weapon_GetTargetingWeapon = nullptr;
static PFN_CWeaponX_FireWeaponBolt   v_CWeaponX_FireWeaponBolt   = nullptr;
static PFN_CGameMovement_DecayPunchAngles v_CGameMovement_DecayPunchAngles = nullptr;
static PFN_CWeaponX_UpdateWeaponSpread v_CWeaponX_UpdateWeaponSpread = nullptr;
static PFN_InterpOwnerVel            v_InterpOwnerVel            = nullptr;
static PFN_CBaseEntity_SetAbsVelocity v_CBaseEntity_SetAbsVelocity = nullptr;

static ConVar bridge_zoom_gate("bridge_zoom_gate", "0", FCVAR_RELEASE,
	"Per-shot [ZOOM-GATE] probe: log the server's zoomFrac, the spread cone and "
	"every input to the ADS gate at each CWeaponX::FireWeaponBolt. Join to the "
	"CLIENT [FIRE-TAP] fwb line by command number.");

static ConVar bridge_zoom_gate_edges("bridge_zoom_gate_edges", "1", FCVAR_RELEASE,
	"Log a [ZOOM-EDGE] line whenever the server's m_bZooming changes, with the "
	"button and gate state at the transition. 0 = per-shot lines only.");

static ConVar bridge_zoom_gate_spring("bridge_zoom_gate_spring", "0", FCVAR_RELEASE,
	"Log a [PUNCH-SPRING] line per command while the view punch is non-zero: the "
	"frametime the server integrates the spring with, and the angle/velocity step "
	"it produced. Join to the client's punch by command number.");

static ConVar bridge_spread_air_priority("bridge_spread_air_priority", "1",
	FCVAR_RELEASE,
	"S21 spread branch order: an airborne player uses the air spread family "
	"even while sliding/sprinting. The S3 engine checks isMovingFast first, "
	"which desyncs m_moveSpread from the S21 client during sprint-jumps and "
	"slide-hops. 0 = legacy S3 order (A/B).");

static ConVar bridge_ads_reload_parity("bridge_ads_reload_parity", "1",
	FCVAR_RELEASE,
	"S21 StartZooming gate: while a weapon with m_semiAutoNeedsRechamber is "
	"reloading, allow ADS. The S3 engine has no reload exemption on that term "
	"and refuses to enter ADS. 0 = legacy S3 behaviour (A/B).");

static ConVar bridge_spread_gate("bridge_spread_gate", "1", FCVAR_RELEASE,
	"Log a rate-limited [SPREAD-GATE] line while the player is airborne with "
	"isMovingFast set (the S3/S21 divergent state), plus a periodic tally.");

static ConVar bridge_kick_base_tap("bridge_kick_base_tap", "0", FCVAR_DEVELOPMENTONLY,
	"Log a [KICK-BASE] line per predicted shot: the two clocks, the selected "
	"one, and the three view-kick decay accumulators. Join the client and dedi "
	"lines by command number to find which input diverges. Default 0; re-arm with "
	"+bridge_kick_base_tap 1 for a one-line-per-shot hunt.");

static ConVar bridge_ammo_tap("bridge_ammo_tap", "0", FCVAR_DEVELOPMENTONLY,
	"[AMMO-TAP] per-shot ammo pair for the cross-engine clip divergence. 0=off (default).");

static ConVar bridge_bolt_vel_parity("bridge_bolt_vel_parity", "1", FCVAR_RELEASE,
	"Strip S3 FireWeaponBolt's InterpOwnerVel/baseVel substitution so dedi bolts "
	"match the S21 client (baseVel * inherit_base_velocity_scale). 0 = legacy S3 (A/B).");

static ConVar bridge_bolt_vel_tap("bridge_bolt_vel_tap", "0", FCVAR_DEVELOPMENTONLY,
	"[BOLT-VEL] per-shot extra/wanted velocity. 0=off.");

// CPlayer offsets from dedicated SendTables (m_bZooming / zoom times).
static constexpr ptrdiff_t PLAYER_OFF_ACTIVEWEAPON  = 5836;  // EHandle, CanZoom's source
static constexpr ptrdiff_t PLAYER_OFF_SWITCHSLOT    = 5848;  // byte, 0xFF = not switching
static constexpr ptrdiff_t PLAYER_OFF_BZOOMING      = 23137; // 0x5A61
static constexpr ptrdiff_t PLAYER_OFF_ZOOMTOGGLET   = 23140; // 0x5A64
static constexpr ptrdiff_t PLAYER_OFF_ZOOMBASEFRAC  = 23144; // 0x5A68
static constexpr ptrdiff_t PLAYER_OFF_ZOOMBASETIME  = 23148; // 0x5A6C
static constexpr ptrdiff_t PLAYER_OFF_BUTTONS       = 24796; // 0x60DC m_nButtons (HELD)
static constexpr ptrdiff_t PLAYER_OFF_BUTTONPRESSED = 24800; // 0x60E0 m_afButtonPressed
static constexpr ptrdiff_t PLAYER_OFF_CURRENTCOMMAND = 25976; // 0x6578, holds a cmd*
static constexpr ptrdiff_t PLAYER_OFF_BASEVELOCITY   = 972;   // m_vecBaseVelocity
// Punch the live aim path actually folds. NOT +8380 -- that is a dead
// prediction-flavoured copy and never marches. Base pair sits 24 bytes below
// the weapon pair.
static constexpr ptrdiff_t PLAYER_OFF_PUNCHBASE_X    = 27508;
static constexpr ptrdiff_t PLAYER_OFF_PUNCHBASE_Y    = 27512;
static constexpr ptrdiff_t PLAYER_OFF_PUNCHBASEVEL_X = 27520;
static constexpr ptrdiff_t PLAYER_OFF_PUNCHBASEVEL_Y = 27524;
static constexpr ptrdiff_t PLAYER_OFF_PUNCHANGLE_X  = 27532; // 0x6B8C
static constexpr ptrdiff_t PLAYER_OFF_PUNCHANGLE_Y  = 27536; // 0x6B90
// m_vecPunchWeapon_AngleVel -- the spring's velocity half, the impulse target.
static constexpr ptrdiff_t PLAYER_OFF_PUNCHVEL_X    = 27544; // 0x6B98
static constexpr ptrdiff_t PLAYER_OFF_PUNCHVEL_Y    = 27548; // 0x6B9C

// CWeaponX. Fire window from DT_WeaponX_LocalWeaponData; spread trio from
// DT_WeaponPlayerData (sub-struct base weapon+4696).
static constexpr ptrdiff_t WEAPON_OFF_OWNER             = 4592;  // m_weaponOwner EHandle
static constexpr ptrdiff_t WEAPON_OFF_LASTPRIMARYATTACK = 4596;  // m_lastPrimaryAttackTime
static constexpr ptrdiff_t WEAPON_OFF_INHERIT_OWNER_VEL = 7200; // projectile_inherit_owner_velocity_scale
static constexpr ptrdiff_t WEAPON_OFF_WEAPONNAME        = 0x15B0; // m_weaponName[65]
static constexpr ptrdiff_t BOLT_OFF_STORED_SPEED        = 9328;  // CrossbowBolt launch speed
static constexpr ptrdiff_t VTBL_GETABSVELOCITY          = 0x4F0;
static constexpr ptrdiff_t WEAPON_OFF_NEXTREADYTIME     = 4600;
static constexpr ptrdiff_t WEAPON_OFF_ATTACKTIMETHISFRAME = 4608; // m_attackTimeThisFrame
static constexpr ptrdiff_t WEAPON_OFF_AMMOINCLIP        = 0x1224; // m_ammoInClip
static constexpr ptrdiff_t WEAPON_OFF_AMMOINSTOCKPILE   = 0x1228; // m_ammoInStockpile
static constexpr ptrdiff_t WEAPON_OFF_WEAPSTATE         = 4660;
static constexpr ptrdiff_t WEAPON_OFF_BINRELOAD         = 4666; // m_bInReload (0x123A)
static constexpr ptrdiff_t WEAPON_OFF_SEMIAUTONEEDSRECHAMBER = 4758; // 0x1296
static constexpr ptrdiff_t WEAPON_OFF_MOVESPREAD        = 4704;
static constexpr ptrdiff_t WEAPON_OFF_KICKSPREADHIP     = 4720;
static constexpr ptrdiff_t WEAPON_OFF_KICKSPREADADS     = 4724;
static constexpr ptrdiff_t WEAPON_OFF_KICKSCALEBASEPITCH = 4732; // m_kickScaleBasePitch
static constexpr ptrdiff_t WEAPON_OFF_KICKSCALEBASEYAW   = 4736; // m_kickScaleBaseYaw
static constexpr ptrdiff_t WEAPON_OFF_KICKPATTERNSCALEBASE = 4740; // m_kickPatternScaleBase
// modVars. zoomEffects is a hard CanZoom gate: false denies ADS outright.
static constexpr ptrdiff_t WEAPON_OFF_BURSTFIRECOUNT    = 6164; // burstFireCount
static constexpr ptrdiff_t WEAPON_OFF_ISSEMIAUTO        = 6497; // is_semi_auto
static constexpr ptrdiff_t WEAPON_OFF_ADS_BLENDFRAC_LO  = 6548;  // ads_fov_zoomfrac_start (spread ADS-blend ramp)
static constexpr ptrdiff_t WEAPON_OFF_ADS_BLENDFRAC_HI  = 6552;  // ads_fov_zoomfrac_end
static constexpr ptrdiff_t WEAPON_OFF_PUNCHSCALEHIP     = 7764;
static constexpr ptrdiff_t WEAPON_OFF_PUNCHSCALEADS     = 7772;
static constexpr ptrdiff_t WEAPON_OFF_VIEWKICKDECAYDELAY = 7828; // viewkickScaleValueDecayDelay
static constexpr ptrdiff_t WEAPON_OFF_VIEWKICKDECAYRATE  = 7832; // viewkickScaleValueDecayRate
static constexpr ptrdiff_t WEAPON_OFF_SPREAD_STAND_ADS  = 8000;  // modVars spread_stand_ads
static constexpr ptrdiff_t WEAPON_OFF_SPREAD_AIR_HIP    = 8012;  // modVars spread_air_hip
static constexpr ptrdiff_t WEAPON_OFF_SPREAD_AIR_ADS    = 8016;  // modVars spread_air_ads
static constexpr ptrdiff_t WEAPON_OFF_ZOOMEFFECTS       = 9212;
static constexpr ptrdiff_t WEAPON_OFF_ZOOMTIMEIN        = 9216;
static constexpr ptrdiff_t WEAPON_OFF_FIREMODE          = 10064; // fireMode
static constexpr ptrdiff_t WEAPON_OFF_WEAPONISACTIVELYFIRING = 10684; // m_weaponIsActivelyFiring (int32)

// CUserCmd::command_number at +0x00.
static constexpr ptrdiff_t CMD_OFF_COMMANDNUMBER = 0x00;

// C_MoveData::m_viewSpringCorrection -- DecayPunchAngles reads it as v24[6..8].
static constexpr ptrdiff_t MOVEDATA_OFF_VIEWSPRINGCORR_X = 24;
static constexpr ptrdiff_t MOVEDATA_OFF_VIEWSPRINGCORR_Y = 28;
static constexpr ptrdiff_t MOVEDATA_OFF_VIEWSPRINGCORR_Z = 32;

// gpGlobals slot, resolved off GetZoomFrac's own `mov rax, cs:gpGlobals` at
// fn+0x19. latestPredictedTime is the float at *slot + 0x28 -- the clock both
// the zoom ramp and the CanZoom fire-window compare run against.
static uintptr_t* s_pZoomGlobalsSlot = nullptr;
static constexpr ptrdiff_t GLOBALS_OFF_LATESTPREDICTEDTIME = 0x28;
// dt for the punch spring integration -- the term the client steps at its own
// cadence, so a mismatch here cannot be reconciled, only corrected.
static constexpr ptrdiff_t GLOBALS_OFF_FRAMETIME = 0x30;

// Captured by the GetZoomFrac hook while a FireWeaponBolt is on the stack:
// GetSpread calls GetZoomFrac(owner) synchronously, so this pairs the weapon
// the bolt came from with its owning player without resolving an EHandle.
static int      s_nFwbDepth = 0;
static __int64  s_pShotPlayer = 0;
static float    s_flShotZoomFrac = 0.f;

static long s_nSpringSteps = 0;
static long s_nShotsZoomed = 0;
static long s_nShotsHip = 0;
static long s_nLogWindow = 0;
static long s_nLoggedThisWindow = 0;
// Own budget for [PUNCH-SPRING] -- do not share ZoomGate_RateLimitOk with shots.
static long s_nSpringLogWindow = 0;
static long s_nSpringLoggedThisWindow = 0;

static long s_nSpreadCalls = 0;
static long s_nSpreadRewrites = 0;
static long s_nSpreadLogWindow = 0;
static long s_nSpreadLoggedThisWindow = 0;
static bool s_bSpreadAirRewriteAnnounced = false;
static std::unordered_set<std::string> s_spreadMinKickWarned;

//-----------------------------------------------------------------------------
// Purpose: the server clock the zoom ramp and the fire window are compared
// against. Returns 0 when the slot is unresolved so a line still prints.
//-----------------------------------------------------------------------------
static float ZoomGate_LatestPredictedTime(void)
{
	if (!s_pZoomGlobalsSlot || !*s_pZoomGlobalsSlot)
		return 0.f;

	return *reinterpret_cast<float*>(*s_pZoomGlobalsSlot + GLOBALS_OFF_LATESTPREDICTEDTIME);
}

//-----------------------------------------------------------------------------
// Purpose: the command being simulated, or -1 when the pointer slot is empty.
//-----------------------------------------------------------------------------
static int ZoomGate_CommandNumber(const __int64 player)
{
	if (!player)
		return -1;

	const __int64 pCmd = *reinterpret_cast<__int64*>(player + PLAYER_OFF_CURRENTCOMMAND);

	if (!pCmd)
		return -1;

	return *reinterpret_cast<int*>(pCmd + CMD_OFF_COMMANDNUMBER);
}

//-----------------------------------------------------------------------------
// Purpose: cap the per-shot lines so a held trigger cannot flood the log.
//-----------------------------------------------------------------------------
static bool ZoomGate_RateLimitOk(void)
{
	const long window = static_cast<long>(ZoomGate_LatestPredictedTime());

	if (window != s_nLogWindow)
	{
		s_nLogWindow = window;
		s_nLoggedThisWindow = 0;
	}

	return (++s_nLoggedThisWindow) <= 32;
}

//-----------------------------------------------------------------------------
// Purpose: denser cap for the per-command spring lines (200/window).
//-----------------------------------------------------------------------------
static bool PunchSpring_RateLimitOk(void)
{
	const long window = static_cast<long>(ZoomGate_LatestPredictedTime());

	if (window != s_nSpringLogWindow)
	{
		s_nSpringLogWindow = window;
		s_nSpringLoggedThisWindow = 0;
	}

	return (++s_nSpringLoggedThisWindow) <= 200;
}

static float ZoomGate_ReadFloat(const __int64 base, const ptrdiff_t off)
{
	return base ? *reinterpret_cast<float*>(base + off) : 0.f;
}

//-----------------------------------------------------------------------------
// Purpose: capture player + zoomFrac while a bolt is being created.
//-----------------------------------------------------------------------------
static float __fastcall Hook_CPlayer_GetZoomFrac(__int64 player)
{
	const float flFrac = v_CPlayer_GetZoomFrac(player);

	if (s_nFwbDepth > 0 && player)
	{
		s_pShotPlayer = player;
		s_flShotZoomFrac = flFrac;
	}

	return flFrac;
}

//-----------------------------------------------------------------------------
// Purpose: clear m_semiAutoNeedsRechamber during StartZooming if the weapon is reloading.
//-----------------------------------------------------------------------------
static void __fastcall Hook_CPlayer_StartZooming(__int64 player)
{
	if (bridge_ads_reload_parity.GetBool()
		&& player
		&& v_CBaseCombatCharacter_Weapon_GetTargetingWeapon)
	{
		const __int64 weapon = v_CBaseCombatCharacter_Weapon_GetTargetingWeapon(player);

		if (weapon
			&& *reinterpret_cast<unsigned char*>(weapon + WEAPON_OFF_SEMIAUTONEEDSRECHAMBER)
			&& *reinterpret_cast<unsigned char*>(weapon + WEAPON_OFF_BINRELOAD))
		{
			unsigned char* const pSemi = reinterpret_cast<unsigned char*>(
				weapon + WEAPON_OFF_SEMIAUTONEEDSRECHAMBER);
			const unsigned char nSaved = *pSemi;
			*pSemi = 0;

			if (bridge_zoom_gate.GetBool() && ZoomGate_RateLimitOk())
			{
				Warning(eDLL_T::SERVER,
					"[ADS-PARITY] cmd=%d weapState=%d reload=%d\n",
					ZoomGate_CommandNumber(player),
					*reinterpret_cast<int*>(weapon + WEAPON_OFF_WEAPSTATE),
					*reinterpret_cast<unsigned char*>(weapon + WEAPON_OFF_BINRELOAD));
			}

			v_CPlayer_StartZooming(player);
			*pSemi = nSaved;
			return;
		}
	}

	v_CPlayer_StartZooming(player);
}

//-----------------------------------------------------------------------------
// Purpose: log the ADS edge from IN_ZOOM and CanZoom's inputs (do not call CanZoom).
//-----------------------------------------------------------------------------
static char __fastcall Hook_CPlayer_UpdateZoom(__int64 player)
{
	const bool bWas = player && *reinterpret_cast<unsigned char*>(player + PLAYER_OFF_BZOOMING) != 0;
	const unsigned int nButtonsBefore = player
		? *reinterpret_cast<unsigned int*>(player + PLAYER_OFF_BUTTONS) : 0u;

	const char result = v_CPlayer_UpdateZoom(player);

	if (player && bridge_zoom_gate.GetBool() && bridge_zoom_gate_edges.GetBool())
	{
		const bool bNow = *reinterpret_cast<unsigned char*>(player + PLAYER_OFF_BZOOMING) != 0;

		if (bWas != bNow)
		{
			const unsigned int nWeapon = *reinterpret_cast<unsigned int*>(
				player + PLAYER_OFF_ACTIVEWEAPON);

			Warning(eDLL_T::SERVER,
				"[ZOOM-EDGE] cmd=%d t=%.4f zooming %d->%d btnHeld=0x%08X (IN_ZOOM=%d) "
				"btnPress=0x%08X toggleT=%.4f baseT=%.4f baseFrac=%.3f switchSlot=0x%02X weapH=0x%08X\n",
				ZoomGate_CommandNumber(player), ZoomGate_LatestPredictedTime(),
				bWas ? 1 : 0, bNow ? 1 : 0,
				nButtonsBefore, (nButtonsBefore & 0x10000u) ? 1 : 0,
				*reinterpret_cast<unsigned int*>(player + PLAYER_OFF_BUTTONPRESSED),
				ZoomGate_ReadFloat(player, PLAYER_OFF_ZOOMTOGGLET),
				ZoomGate_ReadFloat(player, PLAYER_OFF_ZOOMBASETIME),
				ZoomGate_ReadFloat(player, PLAYER_OFF_ZOOMBASEFRAC),
				*reinterpret_cast<unsigned char*>(player + PLAYER_OFF_SWITCHSLOT),
				nWeapon);
		}
	}

	return result;
}

//-----------------------------------------------------------------------------
// Purpose: per-command view-punch spring step (DecayPunchAngles).
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_CGameMovement_DecayPunchAngles(__int64* movement)
{
	const __int64 player = movement ? movement[1] : 0;

	// before[0..3] weapon ang/vel; before[4..7] base ang/vel
	float before[8] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
	if (player)
	{
		before[0] = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHANGLE_X);
		before[1] = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHANGLE_Y);
		before[2] = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHVEL_X);
		before[3] = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHVEL_Y);
		before[4] = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHBASE_X);
		before[5] = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHBASE_Y);
		before[6] = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHBASEVEL_X);
		before[7] = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHBASEVEL_Y);
	}

	const __int64 result = v_CGameMovement_DecayPunchAngles(movement);

	if (player && bridge_zoom_gate.GetBool() && bridge_zoom_gate_spring.GetBool())
	{
		++s_nSpringSteps;

		// Only speak while the spring is actually carrying something -- an idle
		// player would otherwise emit a line every command for a zero step.
		const float ax = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHANGLE_X);
		const float baseAx = ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHBASE_X);
		const __int64 pMove = movement[2];
		const float vscX = ZoomGate_ReadFloat(pMove, MOVEDATA_OFF_VIEWSPRINGCORR_X);
		const float vscY = ZoomGate_ReadFloat(pMove, MOVEDATA_OFF_VIEWSPRINGCORR_Y);

		if ((fabsf(ax) > 0.02f || fabsf(baseAx) > 0.02f || vscX != 0.f || vscY != 0.f)
			&& PunchSpring_RateLimitOk())
		{
			float flFrameTime = 0.f;
			if (s_pZoomGlobalsSlot && *s_pZoomGlobalsSlot)
				flFrameTime = *reinterpret_cast<float*>(
					*s_pZoomGlobalsSlot + GLOBALS_OFF_FRAMETIME);

			// m_viewSpringCorrection is the only term that separates weapon punch
			// from base punch; it reaches the spring through MoveData.
			Warning(eDLL_T::SERVER,
				"[PUNCH-SPRING] cmd=%d t=%.4f ft=%.5f zoomFrac=%.3f vsc=%.5f %.5f %.5f "
				"pB=%.4f %.4f vB=%.3f %.3f ang %.4f %.4f -> %.4f %.4f  vel %.3f %.3f -> %.3f %.3f steps=%ld\n",
				ZoomGate_CommandNumber(player), ZoomGate_LatestPredictedTime(),
				flFrameTime, v_CPlayer_GetZoomFrac ? v_CPlayer_GetZoomFrac(player) : -1.f,
				vscX, vscY,
				ZoomGate_ReadFloat(pMove, MOVEDATA_OFF_VIEWSPRINGCORR_Z),
				ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHBASE_X),
				ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHBASE_Y),
				ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHBASEVEL_X),
				ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHBASEVEL_Y),
				before[0], before[1], ax,
				ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHANGLE_Y),
				before[2], before[3],
				ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHVEL_X),
				ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHVEL_Y),
				s_nSpringSteps);
		}
	}

	return result;
}

static void* BoltVel_ResolveOwner(__int64 weapon)
{
	if (!weapon || !g_serverEntityList)
		return nullptr;

	const uint32_t rawHandle = *reinterpret_cast<uint32_t*>(weapon + WEAPON_OFF_OWNER);
	if (rawHandle == INVALID_EHANDLE_INDEX)
		return nullptr;

	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
	if (void* const pEntity = g_serverEntityList->LookupEntity(handle))
		return pEntity;

	const int entIndex = static_cast<int>(rawHandle & ENT_ENTRY_MASK);
	if (entIndex >= 0 && entIndex < NUM_ENT_ENTRIES)
		return g_serverEntityList->LookupEntityByNetworkIndex(entIndex);

	return nullptr;
}

static void ZoomGate_CopyWeaponName(__int64 weapon, char* pszOut, size_t nOut)
{
	pszOut[0] = '\0';
	if (!weapon || nOut < 2)
		return;

	const char* const pszSrc = reinterpret_cast<const char*>(weapon + WEAPON_OFF_WEAPONNAME);
	if (!pszSrc[0] || !isprint(static_cast<unsigned char>(pszSrc[0])))
		return;

	size_t i = 0;
	for (; i + 1 < nOut && i < 64 && pszSrc[i] != '\0'; ++i)
		pszOut[i] = pszSrc[i];
	pszOut[i] = '\0';
}

static Vector3D BoltVel_GetAbsVelocity(void* entity)
{
	Vector3D out(0.f, 0.f, 0.f);
	if (!entity)
		return out;

	void** const vt = *reinterpret_cast<void***>(entity);
	if (!vt)
		return out;

	const auto fn = reinterpret_cast<PFN_CBaseEntity_GetAbsVelocity>(vt[VTBL_GETABSVELOCITY / sizeof(void*)]);
	if (!fn)
		return out;

	Vector3D* const p = fn(entity, &out, 0);
	if (p && p != &out)
		out = *p;
	return out;
}

// S3 substitutes InterpOwnerVel (lag-history full velocity) for m_vecBaseVelocity
// whenever the owner has an active usercmd. S21 adds m_vecBaseVelocity *
// projectile_inherit_base_velocity_scale (default 1).
static void BoltVel_StripS3Inherit(__int64 weapon, __int64 bolt, char clientPredicted)
{
	if (!bridge_bolt_vel_parity.GetBool() || clientPredicted || !bolt || !weapon)
		return;
	if (!v_CBaseEntity_SetAbsVelocity)
		return;

	void* owner = BoltVel_ResolveOwner(weapon);
	if (!owner && s_pShotPlayer)
		owner = reinterpret_cast<void*>(s_pShotPlayer);
	if (!owner)
		return;

	const Vector3D baseVel = *reinterpret_cast<const Vector3D*>(
		reinterpret_cast<uintptr_t>(owner) + PLAYER_OFF_BASEVELOCITY);

	float flInheritBaseScale = 1.0f;
	char szWeaponName[65];
	ZoomGate_CopyWeaponName(weapon, szWeaponName, sizeof(szWeaponName));
	if (szWeaponName[0])
		flInheritBaseScale = WeaponKVS21Ext_Get(szWeaponName).flInheritBaseVelocityScale;

	Vector3D extra = baseVel;
	void* const player = s_pShotPlayer ? reinterpret_cast<void*>(s_pShotPlayer) : nullptr;
	if (player && v_InterpOwnerVel && *reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>(player) + PLAYER_OFF_CURRENTCOMMAND))
	{
		Vector3D interp(0.f, 0.f, 0.f);
		if (Vector3D* const pInterp = v_InterpOwnerVel(nullptr, &interp, player))
			extra = *pInterp;
	}

	const Vector3D boltVel = BoltVel_GetAbsVelocity(reinterpret_cast<void*>(bolt));
	const Vector3D wanted = boltVel - extra + baseVel * flInheritBaseScale;

	if (!wanted.IsValid())
		return;

	v_CBaseEntity_SetAbsVelocity(reinterpret_cast<void*>(bolt), &wanted);
	*reinterpret_cast<float*>(bolt + BOLT_OFF_STORED_SPEED) = wanted.Length();

	static bool s_bAnnounced = false;
	if (!s_bAnnounced && extra.LengthSqr() > 1.f)
	{
		s_bAnnounced = true;
		Warning(eDLL_T::SERVER,
			"[BOLT-VEL] parity active extra=(%.1f %.1f %.1f) wanted=(%.1f %.1f %.1f) inheritBase=%.3f\n",
			extra.x, extra.y, extra.z,
			wanted.x, wanted.y, wanted.z, flInheritBaseScale);
	}

	if (bridge_bolt_vel_tap.GetBool())
	{
		Warning(eDLL_T::SERVER,
			"[BOLT-VEL] extra=%.1f %.1f %.1f wanted=%.1f %.1f %.1f inheritBase=%.3f\n",
			extra.x, extra.y, extra.z,
			wanted.x, wanted.y, wanted.z, flInheritBaseScale);
	}
}

//-----------------------------------------------------------------------------
// Purpose: one line per authoritative shot: cone from GetSpread, plus hip/ADS delta.
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_CWeaponX_FireWeaponBolt(__int64 weapon, float* pos, float* dir,
	float speed, char adjustForZeroDist, int touchDmgType, int explosionDmgType,
	char clientPredicted, int additionalRandomSeed, char spreadOption, int projectileIndex)
{
	s_pShotPlayer = 0;
	s_flShotZoomFrac = 0.f;
	++s_nFwbDepth;

	const __int64 result = v_CWeaponX_FireWeaponBolt(weapon, pos, dir, speed,
		adjustForZeroDist, touchDmgType, explosionDmgType, clientPredicted,
		additionalRandomSeed, spreadOption, projectileIndex);

	BoltVel_StripS3Inherit(weapon, result, clientPredicted);

	--s_nFwbDepth;

	if (bridge_ammo_tap.GetBool() && weapon)
		Warning(eDLL_T::SERVER, "[AMMO-TAP] cmd=%d clip=%d stock=%d reload=%d\n",
			ZoomGate_CommandNumber(s_pShotPlayer),
			*reinterpret_cast<int*>(weapon + WEAPON_OFF_AMMOINCLIP),
			*reinterpret_cast<int*>(weapon + WEAPON_OFF_AMMOINSTOCKPILE),
			*reinterpret_cast<unsigned char*>(weapon + WEAPON_OFF_BINRELOAD));

	if (!bridge_zoom_gate.GetBool() || !weapon)
		return result;

	const __int64 player = s_pShotPlayer;
	const float flFrac = s_flShotZoomFrac;

	if (flFrac >= 0.5f)
		++s_nShotsZoomed;
	else
		++s_nShotsHip;

	if (!ZoomGate_RateLimitOk())
		return result;

	const float flHip  = ZoomGate_ReadFloat(weapon, WEAPON_OFF_KICKSPREADHIP);
	const float flAds  = ZoomGate_ReadFloat(weapon, WEAPON_OFF_KICKSPREADADS);
	const float flMove = ZoomGate_ReadFloat(weapon, WEAPON_OFF_MOVESPREAD);
	const float flCone = ((flAds - flHip) * flFrac) + flHip + flMove;
	const float flNextReady = ZoomGate_ReadFloat(weapon, WEAPON_OFF_NEXTREADYTIME);
	const float flNow = ZoomGate_LatestPredictedTime();
	const unsigned int nButtons = player
		? *reinterpret_cast<unsigned int*>(player + PLAYER_OFF_BUTTONS) : 0u;

	Warning(eDLL_T::SERVER,
		"[ZOOM-GATE] cmd=%d t=%.4f zoomFrac=%.3f zooming=%d cone=%.4f "
		"(hip=%.4f ads=%.4f move=%.4f dHipAds=%+.4f) punchScale=%.3f punch=%.3f %.3f "
		"state=%d dRdy=%+.4f reload=%d zoomFx=%d switchSlot=0x%02X btnHeld=0x%08X "
		"IN_ZOOM=%d spreadOpt=%d addseed=%d dir=%.4f %.4f %.4f\n",
		ZoomGate_CommandNumber(player), flNow, flFrac,
		player ? *reinterpret_cast<unsigned char*>(player + PLAYER_OFF_BZOOMING) : 0,
		flCone, flHip, flAds, flMove, flAds - flHip,
		((ZoomGate_ReadFloat(weapon, WEAPON_OFF_PUNCHSCALEADS)
			- ZoomGate_ReadFloat(weapon, WEAPON_OFF_PUNCHSCALEHIP)) * flFrac)
			+ ZoomGate_ReadFloat(weapon, WEAPON_OFF_PUNCHSCALEHIP),
		ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHANGLE_X),
		ZoomGate_ReadFloat(player, PLAYER_OFF_PUNCHANGLE_Y),
		*reinterpret_cast<int*>(weapon + WEAPON_OFF_WEAPSTATE),
		flNow - flNextReady,
		*reinterpret_cast<unsigned char*>(weapon + WEAPON_OFF_BINRELOAD),
		*reinterpret_cast<unsigned char*>(weapon + WEAPON_OFF_ZOOMEFFECTS),
		player ? *reinterpret_cast<unsigned char*>(player + PLAYER_OFF_SWITCHSLOT) : 0xFF,
		nButtons, (nButtons & 0x10000u) ? 1 : 0,
		static_cast<int>(spreadOption), additionalRandomSeed,
		dir ? dir[0] : 0.f, dir ? dir[1] : 0.f, dir ? dir[2] : 0.f);

	if (bridge_kick_base_tap.GetBool())
	{
		const float flClkG = flNow;
		const float flClkA = ZoomGate_ReadFloat(weapon, WEAPON_OFF_ATTACKTIMETHISFRAME);
		const float flLastAtk = ZoomGate_ReadFloat(weapon, WEAPON_OFF_LASTPRIMARYATTACK);
		const float flDelay = ZoomGate_ReadFloat(weapon, WEAPON_OFF_VIEWKICKDECAYDELAY);
		const float flRate = ZoomGate_ReadFloat(weapon, WEAPON_OFF_VIEWKICKDECAYRATE);
		const float flPat = ZoomGate_ReadFloat(weapon, WEAPON_OFF_KICKPATTERNSCALEBASE);
		const float flPit = ZoomGate_ReadFloat(weapon, WEAPON_OFF_KICKSCALEBASEPITCH);
		const float flYaw = ZoomGate_ReadFloat(weapon, WEAPON_OFF_KICKSCALEBASEYAW);
		const int nBurst = *reinterpret_cast<int*>(weapon + WEAPON_OFF_BURSTFIRECOUNT);
		const int nMode = *reinterpret_cast<int*>(weapon + WEAPON_OFF_FIREMODE);
		const int nSemi = *reinterpret_cast<unsigned char*>(weapon + WEAPON_OFF_ISSEMIAUTO) ? 1 : 0;
		const int nFiring = (*reinterpret_cast<int*>(weapon + WEAPON_OFF_WEAPONISACTIVELYFIRING) > 0) ? 1 : 0;
		// S3 branch order: no isAkimboWeapon term.
		const int nSel = ((nBurst > 0 || (nMode == 0 && nSemi == 0)) && nFiring > 0) ? 1 : 0;
		const float flDt = (nSel ? flClkA : flClkG) - flLastAtk - flDelay;

		Warning(eDLL_T::SERVER,
			"[KICK-BASE] cmd=%d clkG=%.4f clkA=%.4f lastAtk=%.4f sel=%d dt=%.4f "
			"pat=%.4f pit=%.4f yaw=%.4f delay=%.4f rate=%.4f burst=%d mode=%d "
			"semi=%d akimbo=%d firing=%d\n",
			ZoomGate_CommandNumber(player), flClkG, flClkA, flLastAtk, nSel, flDt,
			flPat, flPit, flYaw, flDelay, flRate, nBurst, nMode, nSemi, -1, nFiring);
	}

	if (((s_nShotsZoomed + s_nShotsHip) % 32) == 0)
		Warning(eDLL_T::SERVER, "[ZOOM-GATE] tally: shots ADS=%ld HIP=%ld\n",
			s_nShotsZoomed, s_nShotsHip);

	return result;
}

//-----------------------------------------------------------------------------
// Purpose: S21 airborne-before-isMovingFast order on the S3 m_moveSpread updater.
//-----------------------------------------------------------------------------
static __int64 __fastcall Hook_CWeaponX_UpdateWeaponSpread(
	__int64 weapon, float speed, float topRegularSpeed, float topFastSpeed,
	char isMovingFast, char isWallRunning, char isWallHanging,
	float duckFrac, float zoomFrac, char onGround)
{
	const bool bDivergent = isMovingFast && !onGround;
	const char nMovingFastEff = (bDivergent && bridge_spread_air_priority.GetBool())
		? 0 : isMovingFast;

	if (!weapon)
		return v_CWeaponX_UpdateWeaponSpread(weapon, speed, topRegularSpeed,
			topFastSpeed, isMovingFast, isWallRunning, isWallHanging,
			duckFrac, zoomFrac, onGround);

	const float flMoveBefore = ZoomGate_ReadFloat(weapon, WEAPON_OFF_MOVESPREAD);

	const __int64 result = v_CWeaponX_UpdateWeaponSpread(weapon, speed,
		topRegularSpeed, topFastSpeed, nMovingFastEff, isWallRunning,
		isWallHanging, duckFrac, zoomFrac, onGround);

	++s_nSpreadCalls;
	if (nMovingFastEff != isMovingFast)
		++s_nSpreadRewrites;

	if (!s_bSpreadAirRewriteAnnounced && bDivergent && nMovingFastEff != isMovingFast)
	{
		s_bSpreadAirRewriteAnnounced = true;
		Warning(eDLL_T::SERVER,
			"[SPREAD-GATE] air-priority rewrite active spd=%.1f zf=%.3f move=%.4f\n",
			speed, zoomFrac, ZoomGate_ReadFloat(weapon, WEAPON_OFF_MOVESPREAD));
	}

	if (bridge_spread_gate.GetBool())
	{
		char szWeaponName[65];
		ZoomGate_CopyWeaponName(weapon, szWeaponName, sizeof(szWeaponName));
		if (szWeaponName[0])
		{
			const WeaponKVS21Ext_t& ext = WeaponKVS21Ext_Get(szWeaponName);
			if ((ext.bHasSpreadMinKick && ext.flSpreadMinKick >= 0.0f)
				|| ext.bSpreadUpdateHipfireInAds)
			{
				if (s_spreadMinKickWarned.insert(szWeaponName).second)
				{
					Warning(eDLL_T::SERVER,
						"[SPREAD-GATE] '%s' spread_min_kick=%.3f hipfireInAds=%d -- "
						"dedi cannot reproduce this S21 term\n",
						szWeaponName, ext.flSpreadMinKick,
						ext.bSpreadUpdateHipfireInAds ? 1 : 0);
				}
			}
		}
	}

	if (bridge_zoom_gate.GetBool() && bridge_spread_gate.GetBool())
	{
		if (bDivergent)
		{
			const long window = static_cast<long>(ZoomGate_LatestPredictedTime());
			if (window != s_nSpreadLogWindow)
			{
				s_nSpreadLogWindow = window;
				s_nSpreadLoggedThisWindow = 0;
			}

			if ((++s_nSpreadLoggedThisWindow) <= 8)
			{
				Warning(eDLL_T::SERVER,
					"[SPREAD-GATE] t=%.4f spd=%.1f topReg=%.1f topFast=%.1f "
					"fast=%d(eff %d) wallR=%d wallH=%d duck=%.2f zf=%.3f ground=%d "
					"move %.4f -> %.4f airHip=%.4f airADS=%.4f standADS=%.4f "
					"blend=%.2f..%.2f rewrites=%ld\n",
					ZoomGate_LatestPredictedTime(),
					speed, topRegularSpeed, topFastSpeed,
					static_cast<int>(isMovingFast), static_cast<int>(nMovingFastEff),
					static_cast<int>(isWallRunning), static_cast<int>(isWallHanging),
					duckFrac, zoomFrac, static_cast<int>(onGround),
					flMoveBefore, ZoomGate_ReadFloat(weapon, WEAPON_OFF_MOVESPREAD),
					ZoomGate_ReadFloat(weapon, WEAPON_OFF_SPREAD_AIR_HIP),
					ZoomGate_ReadFloat(weapon, WEAPON_OFF_SPREAD_AIR_ADS),
					ZoomGate_ReadFloat(weapon, WEAPON_OFF_SPREAD_STAND_ADS),
					ZoomGate_ReadFloat(weapon, WEAPON_OFF_ADS_BLENDFRAC_LO),
					ZoomGate_ReadFloat(weapon, WEAPON_OFF_ADS_BLENDFRAC_HI),
					s_nSpreadRewrites);
			}
		}

		if ((s_nSpreadCalls % 1024) == 0)
			Warning(eDLL_T::SERVER,
				"[SPREAD-GATE] tally: calls=%ld rewrites=%ld airPriority=%d\n",
				s_nSpreadCalls, s_nSpreadRewrites,
				bridge_spread_air_priority.GetBool() ? 1 : 0);
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////////
void VBridgeZoomGate::GetAdr(void) const
{
	LogFunAdr("CPlayer::GetZoomFrac", v_CPlayer_GetZoomFrac);
	LogFunAdr("CPlayer::UpdateZoom", v_CPlayer_UpdateZoom);
	LogFunAdr("CPlayer::StartZooming", v_CPlayer_StartZooming);
	LogFunAdr("CBaseCombatCharacter::Weapon_GetTargetingWeapon",
		v_CBaseCombatCharacter_Weapon_GetTargetingWeapon);
	LogFunAdr("CWeaponX::FireWeaponBolt", v_CWeaponX_FireWeaponBolt);
	LogFunAdr("CGameMovement::DecayPunchAngles", v_CGameMovement_DecayPunchAngles);
	LogFunAdr("CWeaponX::UpdateWeaponSpread", v_CWeaponX_UpdateWeaponSpread);
	LogFunAdr("InterpOwnerVel", v_InterpOwnerVel);
	LogFunAdr("CBaseEntity::SetAbsVelocity", v_CBaseEntity_SetAbsVelocity);
	LogVarAdr("ZoomGateGlobalsSlot", s_pZoomGlobalsSlot);
}

void VBridgeZoomGate::GetFun(void) const
{
	// CPlayer::GetZoomFrac. Landmark is its own m_bZooming read
	// (`movzx ebx, byte ptr [rdi+5A61h]`), so the pattern cannot slide onto a
	// sibling getter.
	Module_FindPattern(g_GameDll,
		"40 57 48 83 EC 50 44 0F 29 44 24 ?? 48 8B F9 48 89 5C 24 ?? E8 ?? ?? ?? ?? "
		"48 8B 05 ?? ?? ?? ?? 48 8B CF 0F B6 9F 61 5A 00 00")
		.GetPtr(v_CPlayer_GetZoomFrac);

	// CPlayer::UpdateZoom. Landmark is the m_afButtonPressed read
	// (`mov eax, [rbx+60E0h]`).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 60 0F 29 74 24 ?? "
		"48 8B D9 E8 ?? ?? ?? ?? 48 8B F8 33 F6 8B 83 E0 60 00 00")
		.GetPtr(v_CPlayer_UpdateZoom);

	// CPlayer::StartZooming (server half). Landmark is m_weapState==RECHAMBER
	// (0x0D) then m_semiAutoNeedsRechamber at +0x1296.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 20 55 48 83 EC 60 48 8B D9 E8 ?? ?? ?? ?? 48 8B E8 "
		"83 B8 34 12 00 00 0D 75 19 48 8B 15 ?? ?? ?? ?? F3 0F 10 42 28 "
		"0F 2F 80 F8 11 00 00 0F 82 ?? ?? ?? ?? 80 B8 96 12 00 00 00 0F 85")
		.GetPtr(v_CPlayer_StartZooming);

	if (v_CPlayer_StartZooming)
	{
		const CMemory fn(reinterpret_cast<uintptr_t>(v_CPlayer_StartZooming));
		v_CBaseCombatCharacter_Weapon_GetTargetingWeapon = fn.Offset(0x0D)
			.FollowNearCallSelf()
			.RCast<PFN_CBaseCombatCharacter_Weapon_GetTargetingWeapon>();

		if (!v_CBaseCombatCharacter_Weapon_GetTargetingWeapon)
			Warning(eDLL_T::SERVER,
				"[ADS-PARITY] Weapon_GetTargetingWeapon unresolved -- "
				"reload ADS parity inactive\n");
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[ZOOM-GATE] CPlayer::StartZooming pattern unresolved -- "
			"reload ADS parity inactive\n");
	}

	// CWeaponX::FireWeaponBolt. The lea reaching its own "FireWeaponBolt" guard
	// string is wildcarded; the rest is the prologue plus the 11-arg frame setup.
	Module_FindPattern(g_GameDll,
		"40 55 57 41 55 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 4D 8B F8 "
		"0F 29 BC 24 ?? ?? ?? ?? 44 0F B6 45 ?? 4C 8B EA 48 8D 15 ?? ?? ?? ?? "
		"0F 28 FB 48 8B F9")
		.GetPtr(v_CWeaponX_FireWeaponBolt);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 30 49 8B F8 48 8B DA 4D 85 C0 74 06 41 8B 40 08")
		.GetPtr(v_InterpOwnerVel);

	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 48 81 EC B0 00 00 00 "
		"48 8B 1D ?? ?? ?? ?? 48 8B E9 44 0F 29 40 C8 48 8B F2 0F BF 41 58")
		.GetPtr(v_CBaseEntity_SetAbsVelocity);

	if (!v_InterpOwnerVel)
		Warning(eDLL_T::SERVER, "[BOLT-VEL] InterpOwnerVel pattern unresolved\n");
	if (!v_CBaseEntity_SetAbsVelocity)
		Warning(eDLL_T::SERVER, "[BOLT-VEL] CBaseEntity::SetAbsVelocity pattern unresolved\n");

	// gpGlobals slot: GetZoomFrac at fn+0x19 does `mov rax, cs:gpGlobals`
	// (48 8B 05 rel32) to reach latestPredictedTime.
	if (v_CPlayer_GetZoomFrac)
	{
		const CMemory fn(reinterpret_cast<uintptr_t>(v_CPlayer_GetZoomFrac));

		if (fn.Offset(0x19).CheckOpCodes({ 0x48, 0x8B, 0x05 }))
			s_pZoomGlobalsSlot = fn.Offset(0x19)
				.ResolveRelativeAddress(0x3, 0x7).RCast<uintptr_t*>();
		else
			Warning(eDLL_T::SERVER,
				"[ZOOM-GATE] globals opcode check failed -- times will log as 0\n");
	}

	// CGameMovement::DecayPunchAngles -- the per-command spring step. Landmark is
	// the weapon-spring index load
	// (`movsxd rdx, cs:<idx>` then `movsxd rcx, [rax+rdx+10h]`).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 50 "
		"48 63 15 ?? ?? ?? ?? 48 8B F9 48 8B 41 18 48 63 4C 10 10 85 C9")
		.GetPtr(v_CGameMovement_DecayPunchAngles);

	if (!v_CGameMovement_DecayPunchAngles)
		Warning(eDLL_T::SERVER,
			"[ZOOM-GATE] CGameMovement::DecayPunchAngles pattern unresolved\n");

	// CWeaponX::UpdateWeaponSpread -- the per-command m_moveSpread/kickSpread
	// updater ItemPostFrame runs for each active weapon. The +1F38h/+1260h
	// displacements pin it.
	Module_FindPattern(g_GameDll,
		"48 8B C4 F3 0F 11 58 ?? F3 0F 11 50 ?? 53 55 57 48 81 EC D0 00 00 00 "
		"F3 0F 10 81 38 1F 00 00 48 8D B9 60 12 00 00")
		.GetPtr(v_CWeaponX_UpdateWeaponSpread);

	if (!v_CWeaponX_UpdateWeaponSpread)
		Warning(eDLL_T::SERVER,
			"[SPREAD-GATE] CWeaponX::UpdateWeaponSpread pattern unresolved -- "
			"S21 air-priority spread fix inactive\n");

	if (!v_CPlayer_GetZoomFrac)
		Warning(eDLL_T::SERVER, "[ZOOM-GATE] CPlayer::GetZoomFrac pattern unresolved\n");
	if (!v_CPlayer_UpdateZoom)
		Warning(eDLL_T::SERVER, "[ZOOM-GATE] CPlayer::UpdateZoom pattern unresolved\n");
	if (!v_CWeaponX_FireWeaponBolt)
		Warning(eDLL_T::SERVER, "[ZOOM-GATE] CWeaponX::FireWeaponBolt pattern unresolved\n");
}

void VBridgeZoomGate::GetVar(void) const { }
void VBridgeZoomGate::GetCon(void) const { }

void VBridgeZoomGate::Detour(const bool bAttach) const
{
	if (v_CPlayer_GetZoomFrac)
		DetourSetup(&v_CPlayer_GetZoomFrac, &Hook_CPlayer_GetZoomFrac, bAttach);

	if (v_CPlayer_UpdateZoom)
		DetourSetup(&v_CPlayer_UpdateZoom, &Hook_CPlayer_UpdateZoom, bAttach);

	if (v_CPlayer_StartZooming)
		DetourSetup(&v_CPlayer_StartZooming, &Hook_CPlayer_StartZooming, bAttach);

	if (v_CWeaponX_FireWeaponBolt)
		DetourSetup(&v_CWeaponX_FireWeaponBolt, &Hook_CWeaponX_FireWeaponBolt, bAttach);

	if (v_CGameMovement_DecayPunchAngles)
		DetourSetup(&v_CGameMovement_DecayPunchAngles,
			&Hook_CGameMovement_DecayPunchAngles, bAttach);

	if (v_CWeaponX_UpdateWeaponSpread)
		DetourSetup(&v_CWeaponX_UpdateWeaponSpread,
			&Hook_CWeaponX_UpdateWeaponSpread, bAttach);
}
///////////////////////////////////////////////////////////////////////////////

