//=============================================================================//
//
// Purpose: dedi twin of the client melee lunge-boost probe.
//
// MeleeWalkMove accumulates m_melee.lungeBoost toward
//     maxSpd = <lunge no-target max speed> * (clamp(m_flForwardMove, 0, 1) * 0.5 + 0.5)
// and that clamp term is the one input that can make the two engines agree while
// standing and disagree while moving. m_flForwardMove is at mv+48 here too; the
// C_MoveData layouts only diverge later (velocity is mv+304 here, mv+292 on the
// client). Patterns are pinned to the SERVER half -- this binary carries a
// client-half twin of the same functions.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "melee_lunge_probe.h"
#include "entitylist.h"   // g_serverEntityList

static ConVar bridge_melee_lunge_probe("bridge_melee_lunge_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[LUNGE-PROBE] log m_flForwardMove and the lungeBoost accumulation inside "
	"MeleeWalkMove. Compare against the client's lines. 0 = off (default).");

static constexpr ptrdiff_t kMoveDataForwardMove = 48;    // m_flForwardMove
static constexpr ptrdiff_t kMoveDataVelocity    = 304;   // m_vecVelocity (dedi layout)
static constexpr ptrdiff_t kPlayerLungeBoost    = 28392; // m_melee.lungeBoost
static constexpr ptrdiff_t kPlayerMoveSpeedScale = 24880; // m_playerMoveSpeedScale
static constexpr ptrdiff_t kPlayerCachedMoveScale = 24884; // m_cachedMoveScale

// This engine builds the weapon term from TWO different entities: it tests the
// fire mode of activeWeapons[0], and when that says "offhand" it multiplies that
// weapon's modifier by a SECOND one taken from the latest-primary array. The
// client half resolves the same handle twice instead, so log both entities.
static constexpr ptrdiff_t kPlayerActiveWeapons  = 5836;  // activeWeapons[slot]
static constexpr ptrdiff_t kPlayerLatestPrimary  = 5852;  // latest-primary array
static constexpr ptrdiff_t kWeaponMoveSpeedMod   = 6536;  // move_speed_modifier
static constexpr ptrdiff_t kWeaponMoveSpeedAdsP  = 6540;  // ..._ads_passive
static constexpr ptrdiff_t kWeaponFireMode       = 10064; // fireMode

static ConVar bridge_melee_press_probe("bridge_melee_press_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[MELEE-PRESS]/[MELEE-HIT] log the melee press machine on every state change and "
	"every hit registration, in the same field order as the client, so press and hit "
	"events pair one-for-one across the two logs. 0 = off (default).");

static ConVar bridge_lunge_interp_rewind("bridge_lunge_interp_rewind", "0", FCVAR_RELEASE,
	"Rewind the lunge owner's lag-comp ring to the command time instead of the "
	"latency-shifted time when setting a melee lunge target, matching the client's "
	"predicted-origin basis. 0 = engine default.");

static ConVar bridge_lunge_rewind_diag("bridge_lunge_rewind_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[LUNGE-RW] log both backtrack vectors (latency-time and command-time) and the "
	"resulting lunge offsets on every lunge target set. 0 = off.");

// Networked lunge offset fields on the server player (post SetTargetEntity).
static constexpr ptrdiff_t kPlayerLungeStartOffset     = 0x6F0C; // m_lungeStartPositionOffset
static constexpr ptrdiff_t kPlayerLungeStartOffsetNoLC  = 0x6F18; // m_lungeStartPositionOffset_notLagCompensated
static constexpr ptrdiff_t kPlayerLungeEndOffset        = 0x6F24; // m_lungeEndPositionOffset
// Melee settings object: latencyTicks int used by the lag-comp ring walk.
static constexpr ptrdiff_t kMeleeSettingsLatencyTicks   = 0x6C;

// Pointer-slot for the melee-settings global, resolved from the ring-walk prologue.
// Runtime: settings = *slot; latencyTicks at settings+0x6C.
static void** s_ppMeleeSettings = nullptr;

static bool s_bInLungeSetter = false;
static unsigned s_nLungeRwDiagCount = 0;

struct LungeRewindCapture_t
{
	float latVec[3];
	float cmdVec[3];
	bool bLatValid;
	bool bCmdValid;
};
static LungeRewindCapture_t s_lungeRewindCap = {};

// m_Local -- the press machine's whole state.
static constexpr ptrdiff_t kPlayerQueuedMeleePress   = 23660;
static constexpr ptrdiff_t kPlayerQueuedGrappleMelee = 23664;
static constexpr ptrdiff_t kPlayerDisableUntilRel    = 23669;
static constexpr ptrdiff_t kPlayerMeleePressTime     = 23672;
static constexpr ptrdiff_t kPlayerMeleeInputIndex    = 23680;
static constexpr ptrdiff_t kPlayerButtonsHeld        = 24796;
static constexpr ptrdiff_t kPlayerButtonsPressed     = 24800;
static constexpr ptrdiff_t kPlayerButtonsReleased    = 24804;

// m_melee (struct base 28352).
static constexpr ptrdiff_t kMeleeParity        = 28360;
static constexpr ptrdiff_t kMeleeAttackActive  = 28364;
static constexpr ptrdiff_t kMeleeAttackStart   = 28368;
static constexpr ptrdiff_t kMeleeHitEntity     = 28372;
static constexpr ptrdiff_t kMeleeHitTime       = 28376;
static constexpr ptrdiff_t kMeleeLastNonWorld  = 28380;
static constexpr ptrdiff_t kMeleeScriptedState = 28384;
static constexpr ptrdiff_t kMeleePendingPress  = 28388;

static bool s_bFirstFire = true;
static unsigned s_nMoveScaleProbeCount = 0;
static bool s_bPressFirstFire = true;

// CBaseHandle::Get has no implementation in this SDK -- same raw
// g_serverEntityList lookup the other server-side resolvers use.
static uintptr_t MeleeProbe_ResolveHandle(const unsigned nHandle)
{
	if (nHandle == INVALID_EHANDLE_INDEX || !g_serverEntityList)
		return 0;

	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(nHandle));
	return reinterpret_cast<uintptr_t>(g_serverEntityList->LookupEntity(handle));
}

struct MeleePressState_t
{
	float flQueued;
	float flQueuedGrapple;
	float flPressTime;
	int   nIndex;
	int   nParity;
	int   nState;
	unsigned char bDisableUntilRel;
	unsigned char bAttackActive;
	unsigned char bPendingPress;
};

static void MeleePress_Read(const uintptr_t pPlayer, MeleePressState_t& s)
{
	s.flQueued         = *reinterpret_cast<const float*>(pPlayer + kPlayerQueuedMeleePress);
	s.flQueuedGrapple  = *reinterpret_cast<const float*>(pPlayer + kPlayerQueuedGrappleMelee);
	s.flPressTime      = *reinterpret_cast<const float*>(pPlayer + kPlayerMeleePressTime);
	s.nIndex           = *reinterpret_cast<const int*>(pPlayer + kPlayerMeleeInputIndex);
	s.nParity          = *reinterpret_cast<const int*>(pPlayer + kMeleeParity);
	s.nState           = *reinterpret_cast<const int*>(pPlayer + kMeleeScriptedState);
	s.bDisableUntilRel = *reinterpret_cast<const unsigned char*>(pPlayer + kPlayerDisableUntilRel);
	s.bAttackActive    = *reinterpret_cast<const unsigned char*>(pPlayer + kMeleeAttackActive);
	s.bPendingPress    = *reinterpret_cast<const unsigned char*>(pPlayer + kMeleePendingPress);
}

static bool MeleePress_Differs(const MeleePressState_t& a, const MeleePressState_t& b)
{
	return a.flQueued != b.flQueued
		|| a.flQueuedGrapple != b.flQueuedGrapple
		|| a.flPressTime != b.flPressTime
		|| a.nIndex != b.nIndex
		|| a.nParity != b.nParity
		|| a.nState != b.nState
		|| a.bDisableUntilRel != b.bDisableUntilRel
		|| a.bAttackActive != b.bAttackActive
		|| a.bPendingPress != b.bPendingPress;
}

static void Hook_PlayerMelee_ItemPreFrame(void* player)
{
	const bool bProbe = bridge_melee_press_probe.GetBool() && player;
	const uintptr_t pPlayer = reinterpret_cast<uintptr_t>(player);

	MeleePressState_t before = {};
	unsigned nHeld = 0, nPressed = 0, nReleased = 0;

	if (bProbe)
	{
		MeleePress_Read(pPlayer, before);
		nHeld     = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerButtonsHeld);
		nPressed  = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerButtonsPressed);
		nReleased = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerButtonsReleased);
	}

	PlayerMelee_ItemPreFrame(player);

	if (!bProbe)
		return;

	MeleePressState_t after = {};
	MeleePress_Read(pPlayer, after);

	if (!MeleePress_Differs(before, after) && !s_bPressFirstFire)
		return;

	if (s_bPressFirstFire)
	{
		Warning(eDLL_T::SERVER, "[MELEE-PRESS] FIRST FIRE -- dedi press machine probe live\n");
		s_bPressFirstFire = false;
	}

	const unsigned nWpn = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerActiveWeapons);
	const uintptr_t pWeapon = MeleeProbe_ResolveHandle(nWpn);
	const int nFireMode = pWeapon ? *reinterpret_cast<const int*>(pWeapon + kWeaponFireMode) : -1;

	// launch is printed as -1 because this engine never reads a launch state here;
	// that missing gate is the thing the client's field is there to expose.
	Warning(eDLL_T::SERVER,
		"[MELEE-PRESS] dedi held=%08X pressed=%08X released=%08X launch=%d "
		"idx=%d->%d queued=%.4f->%.4f grapple=%.4f->%.4f pressT=%.4f->%.4f "
		"disableUntilRel=%d->%d active=%d->%d pend=%d->%d parity=%d->%d state=%d->%d "
		"wpn=%d:%d fm=%d\n",
		nHeld, nPressed, nReleased, -1,
		before.nIndex, after.nIndex,
		before.flQueued, after.flQueued,
		before.flQueuedGrapple, after.flQueuedGrapple,
		before.flPressTime, after.flPressTime,
		before.bDisableUntilRel, after.bDisableUntilRel,
		before.bAttackActive, after.bAttackActive,
		before.bPendingPress, after.bPendingPress,
		before.nParity, after.nParity,
		before.nState, after.nState,
		(nWpn == INVALID_EHANDLE_INDEX) ? -1 : static_cast<int>(nWpn & 0xFFFF),
		static_cast<int>(nWpn >> 16), nFireMode);
}

static int Hook_PlayerMelee_SetAttackHitEntity(void* player, void* ent)
{
	const bool bProbe = bridge_melee_press_probe.GetBool() && player;
	const uintptr_t pPlayer = reinterpret_cast<uintptr_t>(player);

	float flHitBefore = 0.0f, flNonWorldBefore = 0.0f;
	if (bProbe)
	{
		flHitBefore      = *reinterpret_cast<const float*>(pPlayer + kMeleeHitTime);
		flNonWorldBefore = *reinterpret_cast<const float*>(pPlayer + kMeleeLastNonWorld);
	}

	const int result = CPlayer__PlayerMelee_SetAttackHitEntity(player, ent);

	if (bProbe)
	{
		const unsigned nHit = *reinterpret_cast<const unsigned*>(pPlayer + kMeleeHitEntity);
		Warning(eDLL_T::SERVER,
			"[MELEE-HIT] dedi entPtr=%p hitEnt=%d:%d active=%d startT=%.4f "
			"hitT=%.4f->%.4f lastNonWorldT=%.4f->%.4f parity=%d state=%d\n",
			ent,
			(nHit == INVALID_EHANDLE_INDEX) ? -1 : static_cast<int>(nHit & 0xFFFF),
			static_cast<int>(nHit >> 16),
			*reinterpret_cast<const unsigned char*>(pPlayer + kMeleeAttackActive),
			*reinterpret_cast<const float*>(pPlayer + kMeleeAttackStart),
			flHitBefore, *reinterpret_cast<const float*>(pPlayer + kMeleeHitTime),
			flNonWorldBefore, *reinterpret_cast<const float*>(pPlayer + kMeleeLastNonWorld),
			*reinterpret_cast<const int*>(pPlayer + kMeleeParity),
			*reinterpret_cast<const int*>(pPlayer + kMeleeScriptedState));
	}

	return result;
}

static float* Hook_LungeLagCompBacktrack(void* a1, float* out, void* player)
{
	// Other lag-comp callers (non-lunge) stay on the natural path.
	if (!s_bInLungeSetter || !s_ppMeleeSettings)
		return LungeLagCompBacktrack(a1, out, player);

	float tmpLat[3] = {};
	float tmpCmd[3] = {};

	LungeLagCompBacktrack(a1, tmpLat, player);
	s_lungeRewindCap.latVec[0] = tmpLat[0];
	s_lungeRewindCap.latVec[1] = tmpLat[1];
	s_lungeRewindCap.latVec[2] = tmpLat[2];
	s_lungeRewindCap.bLatValid = true;

	void* settings = *s_ppMeleeSettings;
	if (settings)
	{
		int* const pLatency = reinterpret_cast<int*>(
			reinterpret_cast<uintptr_t>(settings) + kMeleeSettingsLatencyTicks);
		const int nSave = *pLatency;
		*pLatency = 0;
		LungeLagCompBacktrack(a1, tmpCmd, player);
		*pLatency = nSave;

		s_lungeRewindCap.cmdVec[0] = tmpCmd[0];
		s_lungeRewindCap.cmdVec[1] = tmpCmd[1];
		s_lungeRewindCap.cmdVec[2] = tmpCmd[2];
		s_lungeRewindCap.bCmdValid = true;
	}

	const float* selected = (bridge_lunge_interp_rewind.GetBool() && s_lungeRewindCap.bCmdValid)
		? tmpCmd
		: tmpLat;

	if (out)
	{
		out[0] = selected[0];
		out[1] = selected[1];
		out[2] = selected[2];
	}
	return out;
}

static char Hook_Lunge_SetTargetEntity(void* player, void* target)
{
	s_lungeRewindCap.bLatValid = false;
	s_lungeRewindCap.bCmdValid = false;

	s_bInLungeSetter = true;
	const char result = CPlayer__Lunge_SetTargetEntity(player, target);
	s_bInLungeSetter = false;

	if (bridge_lunge_rewind_diag.GetBool() && player)
	{
		++s_nLungeRwDiagCount;
		if (s_nLungeRwDiagCount <= 256 || (s_nLungeRwDiagCount % 16) == 0)
		{
			const float lat0 = s_lungeRewindCap.bLatValid ? s_lungeRewindCap.latVec[0] : -9999.0f;
			const float lat1 = s_lungeRewindCap.bLatValid ? s_lungeRewindCap.latVec[1] : -9999.0f;
			const float lat2 = s_lungeRewindCap.bLatValid ? s_lungeRewindCap.latVec[2] : -9999.0f;
			const float cmd0 = s_lungeRewindCap.bCmdValid ? s_lungeRewindCap.cmdVec[0] : -9999.0f;
			const float cmd1 = s_lungeRewindCap.bCmdValid ? s_lungeRewindCap.cmdVec[1] : -9999.0f;
			const float cmd2 = s_lungeRewindCap.bCmdValid ? s_lungeRewindCap.cmdVec[2] : -9999.0f;

			float flMag = -9999.0f;
			if (s_lungeRewindCap.bLatValid && s_lungeRewindCap.bCmdValid)
			{
				const float dx = lat0 - cmd0;
				const float dy = lat1 - cmd1;
				const float dz = lat2 - cmd2;
				flMag = sqrtf(dx * dx + dy * dy + dz * dz);
			}

			const uintptr_t p = reinterpret_cast<uintptr_t>(player);
			const float* start = reinterpret_cast<const float*>(p + kPlayerLungeStartOffset);
			const float* startNoLC = reinterpret_cast<const float*>(p + kPlayerLungeStartOffsetNoLC);
			const float* end = reinterpret_cast<const float*>(p + kPlayerLungeEndOffset);

			Warning(eDLL_T::SERVER,
				"[LUNGE-RW] mode=%d lat=(%.3f %.3f %.3f) cmd=(%.3f %.3f %.3f) |d|=%.3f "
				"start=(%.3f %.3f %.3f) startNoLC=(%.3f %.3f %.3f) end=(%.3f %.3f %.3f)\n",
				bridge_lunge_interp_rewind.GetInt(),
				lat0, lat1, lat2, cmd0, cmd1, cmd2, flMag,
				start[0], start[1], start[2],
				startNoLC[0], startNoLC[1], startNoLC[2],
				end[0], end[1], end[2]);
		}
	}

	return result;
}

static void Hook_MeleeWalkMove(void* ctx)
{
	const bool bProbe = bridge_melee_lunge_probe.GetBool() && ctx;

	uintptr_t pPlayer = 0, pMove = 0;
	float flFwd = 0.0f, boostBefore[3] = { 0.0f, 0.0f, 0.0f };

	if (bProbe)
	{
		pPlayer = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(ctx) + 8);
		pMove   = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(ctx) + 16);

		if (pPlayer && pMove)
		{
			flFwd = *reinterpret_cast<const float*>(pMove + kMoveDataForwardMove);
			memcpy(boostBefore, reinterpret_cast<const void*>(pPlayer + kPlayerLungeBoost), sizeof(boostBefore));
		}
	}

	CGameMovement__MeleeWalkMove(ctx);

	if (bProbe && pPlayer && pMove)
	{
		const float* b = reinterpret_cast<const float*>(pPlayer + kPlayerLungeBoost);
		const float* v = reinterpret_cast<const float*>(pMove + kMoveDataVelocity);

		const bool bChanged = fabsf(b[0] - boostBefore[0]) > 1e-6f
			|| fabsf(b[1] - boostBefore[1]) > 1e-6f
			|| fabsf(b[2] - boostBefore[2]) > 1e-6f;

		if (bChanged || s_bFirstFire)
		{
			if (s_bFirstFire)
			{
				Warning(eDLL_T::SERVER, "[LUNGE-PROBE] FIRST FIRE -- dedi MeleeWalkMove probe live\n");
				s_bFirstFire = false;
			}

			const float flScale = (fminf(fmaxf(flFwd, 0.0f), 1.0f) * 0.5f) + 0.5f;

			const float flDur   = GetLungeDuration ? GetLungeDuration(reinterpret_cast<void*>(pPlayer)) : -1.0f;
			const float flRange = GetLungeNoTargetRange ? GetLungeNoTargetRange(reinterpret_cast<void*>(pPlayer)) : -1.0f;
			const float flMax   = (flDur > 0.0f) ? ((2.0f * flRange) / flDur) * flScale : -1.0f;

			const float flWalkMax = CPlayer__GetPlayerMoveMaxSpeed
				? CPlayer__GetPlayerMoveMaxSpeed(reinterpret_cast<void*>(pPlayer)) : -1.0f;

			// walkMax = m_cachedMoveScale * classSettings[pose][speed].
			const float flMoveScale = *reinterpret_cast<const float*>(pPlayer + kPlayerCachedMoveScale);
			const float flSetting   = (flMoveScale != 0.0f) ? (flWalkMax / flMoveScale) : -1.0f;

			Warning(eDLL_T::SERVER,
				"[LUNGE-PROBE] dedi fwdMove=%.4f scale=%.4f dur=%.4f range=%.4f maxSpd=%.3f "
				"walkMax=%.3f moveScale=%.4f setting=%.4f boost=(%.3f %.3f %.3f) vel=(%.3f %.3f %.3f) |vh|=%.3f\n",
				flFwd, flScale, flDur, flRange, flMax, flWalkMax, flMoveScale, flSetting,
				b[0], b[1], b[2], v[0], v[1], v[2], sqrtf(v[0] * v[0] + v[1] * v[1]));
		}

		// Decompose m_cachedMoveScale into its four multiplicative factors.
		// Print on lunge-boost change, first fire, or every 60th probed call.
		++s_nMoveScaleProbeCount;
		if (bChanged || s_nMoveScaleProbeCount == 1 || (s_nMoveScaleProbeCount % 60) == 0)
		{
			const float flMsss = *reinterpret_cast<const float*>(pPlayer + kPlayerMoveSpeedScale);
			const float flCached = *reinterpret_cast<const float*>(pPlayer + kPlayerCachedMoveScale);

			const float flSlow = GetStatusEffectSeverity_MoveSlow
				? GetStatusEffectSeverity_MoveSlow(reinterpret_cast<void*>(pPlayer)) : -1.0f;
			const float flLand = CPlayer__GetRecentLandingMoveSpeedScale
				? CPlayer__GetRecentLandingMoveSpeedScale(reinterpret_cast<void*>(pPlayer)) : -1.0f;

			float flSev = 0.0f;
			float flTime = 0.0f;
			float flBoost = -1.0f;
			if (GetStatusEffectSeverityAndTime && g_pStatusEffectTypeIndex_SpeedBoost)
			{
				GetStatusEffectSeverityAndTime(reinterpret_cast<void*>(pPlayer),
					*g_pStatusEffectTypeIndex_SpeedBoost, &flSev, &flTime, false);
				flBoost = 1.0f + 2.0f * flSev;
			}
			else
			{
				flSev = -1.0f;
			}

			// weaponChain is the residual: cached / (msss * (1-slow) * boost * land)
			const float flDenom = flMsss * (1.0f - flSlow) * flBoost * flLand;
			const float flWeap = (fabsf(flDenom) > 1e-6f) ? (flCached / flDenom) : -1.0f;

			const unsigned nWpn = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerActiveWeapons);
			const unsigned nPri = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerLatestPrimary);
			const uintptr_t pWeapon = MeleeProbe_ResolveHandle(nWpn);
			const uintptr_t pPrimary = MeleeProbe_ResolveHandle(nPri);

			int nFireMode = -1;
			float flMsm = -1.0f, flAdsp = -1.0f;
			if (pWeapon)
			{
				nFireMode = *reinterpret_cast<const int*>(pWeapon + kWeaponFireMode);
				flMsm     = *reinterpret_cast<const float*>(pWeapon + kWeaponMoveSpeedMod);
				flAdsp    = *reinterpret_cast<const float*>(pWeapon + kWeaponMoveSpeedAdsP);
			}

			float flPriMsm = -1.0f, flPriAdsp = -1.0f;
			if (pPrimary)
			{
				flPriMsm  = *reinterpret_cast<const float*>(pPrimary + kWeaponMoveSpeedMod);
				flPriAdsp = *reinterpret_cast<const float*>(pPrimary + kWeaponMoveSpeedAdsP);
			}

			Warning(eDLL_T::SERVER,
				"[MOVESCALE] dedi cached=%.4f msss=%.4f slow=%.4f boost=%.4f land=%.4f weap=%.4f sevRaw=%.4f "
				"wpn=%d:%d ptr=%p fm=%d msm=%.4f adsp=%.4f pri=%d:%d priMsm=%.4f priAdsp=%.4f\n",
				flCached, flMsss, flSlow, flBoost, flLand, flWeap, flSev,
				(nWpn == 0xFFFFFFFFu) ? -1 : static_cast<int>(nWpn & 0xFFFF),
				static_cast<int>(nWpn >> 16), reinterpret_cast<const void*>(pWeapon),
				nFireMode, flMsm, flAdsp,
				(nPri == 0xFFFFFFFFu) ? -1 : static_cast<int>(nPri & 0xFFFF),
				static_cast<int>(nPri >> 16), flPriMsm, flPriAdsp);
		}
	}
}

//-----------------------------------------------------------------------------
// IDetour implementation
//-----------------------------------------------------------------------------
void VMeleeLungeProbeServer::GetFun(void) const
{
	// CGameMovement::MeleeWalkMove (server half) -- unique.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 56 48 81 EC ? ? ? ? 48 8B 51 ? 48 8B F1 80 BA ? ? ? ? ? 75 ? "
		"48 8D 9A ? ? ? ? 48 8B 43 ? 48 8D 4B ? 48 8B D3 FF 10")
		.GetPtr(CGameMovement__MeleeWalkMove);

	// GetLungeDuration -- unique.
	Module_FindPattern(g_GameDll,
		"48 89 74 24 ? 57 48 83 EC ? 8B 91 ? ? ? ? 48 8D 35 ? ? ? ? 48 89 5C 24 ? "
		"48 8B F9 83 FA ? 74 ? 0F B7 C2 C1 EA ? 4C 8D 04 40 4D 03 C0")
		.GetPtr(GetLungeDuration);

	// GetLungeNoTargetRange -- unique.
	Module_FindPattern(g_GameDll,
		"80 B9 ? ? ? ? ? 4C 8B C1 74 ? 8B 91 ? ? ? ? 83 FA ? 74 ? 0F B7 C2 "
		"48 8D 0D ? ? ? ? C1 EA ? 48 8D 04 40 48 C1 E0 ? 48 03 C1 8B 48")
		.GetPtr(GetLungeNoTargetRange);

	// CPlayer::GetPlayerMoveMaxSpeed, resolved from the rescale call inside the
	// melee-lunge overspeed clamp. The leading sprint-flag displacement stays
	// LITERAL -- it is what pins the server-half twin. unique.
	// The bool check is load-bearing -- see the client twin's note.
	const CMemory clampSite = Module_FindPattern(g_GameDll,
		"41 80 B9 24 6D 00 00 00 41 8B 81 ? ? ? ? 74 ? 85 C0 7F ? BA ? ? ? ? 49 8B C9 "
		"E8 ? ? ? ? EB ? 41 0F B6 91 ? ? ? ? 49 8B C9 D1 EA 83 E2 ? E8 ? ? ? ? EB ? "
		"85 C0 7F ? BA ? ? ? ? EB ? 41 0F B6 91 ? ? ? ? D1 EA 83 E2 ? 49 8B C9 "
		"E8 ? ? ? ? F3 0F 59 C0 0F 2F D8 76 ?");
	if (clampSite)
	{
		CPlayer__GetPlayerMoveMaxSpeed = clampSite.Offset(0x7C)
			.FollowNearCallSelf()
			.RCast<float(*)(void*)>();
	}
	else
	{
		Warning(eDLL_T::SERVER, "[LUNGE-PROBE] GetPlayerMoveMaxSpeed unresolved -- walkMax not logged\n");
	}

	// Tail of the inlined move-scale product in SetupMove. The trailing
	// 30 61 00 00 is m_playerMoveSpeedScale -- leave literal; it pins the
	// server-half twin (this binary also has a client-half copy).
	// unique.
	const CMemory moveScaleSite = Module_FindPattern(g_GameDll,
		"8B 15 ? ? ? ? 4C 8D 8C 24 ? ? ? ? 4C 8D 84 24 ? ? ? ? "
		"C7 84 24 ? ? ? ? 00 00 00 00 48 8B CF C7 84 24 ? ? ? ? 00 00 00 00 "
		"C6 44 24 ? 00 E8 ? ? ? ? 48 8B CF E8 ? ? ? ? 41 0F 28 F1 "
		"48 8B CF F3 0F 5C F0 E8 ? ? ? ? F3 0F 59 BF 30 61 00 00");
	if (moveScaleSite)
	{
		g_pStatusEffectTypeIndex_SpeedBoost = moveScaleSite
			.ResolveRelativeAddress(2, 6)
			.RCast<int*>();
		GetStatusEffectSeverityAndTime = moveScaleSite.Offset(0x34)
			.FollowNearCallSelf()
			.RCast<void(*)(void*, int, float*, float*, bool)>();
		GetStatusEffectSeverity_MoveSlow = moveScaleSite.Offset(0x3C)
			.FollowNearCallSelf()
			.RCast<float(*)(void*)>();
		CPlayer__GetRecentLandingMoveSpeedScale = moveScaleSite.Offset(0x4C)
			.FollowNearCallSelf()
			.RCast<float(*)(void*)>();
	}
	else
	{
		Warning(eDLL_T::SERVER, "[LUNGE-PROBE] move-scale factor site unresolved -- factors not logged\n");
	}

	// PlayerMelee_ItemPreFrame. The four literal displacements are m_nButtons
	// (0x60DC), m_afButtonReleased (0x60E4), m_queuedMeleePressTime (0x5C6C) and
	// m_queuedGrappleMeleeTime (0x5C70) -- all server-half, which is what keeps
	// this off the client-half twin. unique.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 70 0F 29 74 24 40 48 8B D9 E8 ? ? ? ? "
		"8B 93 DC 60 00 00 0F 57 F6 0B 93 E4 60 00 00 85 C2 75 ? "
		"0F 2F B3 6C 5C 00 00 76 ? 0F 2F B3 70 5C 00 00")
		.GetPtr(PlayerMelee_ItemPreFrame);

	if (!PlayerMelee_ItemPreFrame)
		Warning(eDLL_T::SERVER, "[MELEE-PRESS] ItemPreFrame pattern unresolved -- press events not logged\n");

	// CPlayer::PlayerMelee_SetAttackHitEntity. Pinned by m_pCurrentCommand
	// (0x6578) and m_melee.attackActive (0x6ECC), both server-half.
	// unique.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 57 48 83 EC 30 48 83 B9 78 65 00 00 00 48 8B FA 48 8B D9 "
		"75 ? 48 8D 0D ? ? ? ? 48 8B 5C 24 48 48 83 C4 30 5F E9 ? ? ? ? "
		"80 B9 CC 6E 00 00 00")
		.GetPtr(CPlayer__PlayerMelee_SetAttackHitEntity);

	if (!CPlayer__PlayerMelee_SetAttackHitEntity)
		Warning(eDLL_T::SERVER, "[MELEE-HIT] SetAttackHitEntity pattern unresolved -- hits not logged\n");

	// CPlayer::Lunge_SetTargetEntity (server half) -- finds the networked lunge
	// start/end offset writer. unique.
	Module_FindPattern(g_GameDll,
		"40 55 53 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 83 B9 ? ? ? ? ? 48 8B FA")
		.GetPtr(CPlayer__Lunge_SetTargetEntity);

	if (!CPlayer__Lunge_SetTargetEntity)
		Warning(eDLL_T::SERVER, "[LUNGE-RW] Lunge_SetTargetEntity pattern unresolved -- lever inert\n");

	// Lag-comp ring walk -- rewinds the lunge owner's origin history. The
	// latencyTicks load at +0x12 pins the settings global used by the lever.
	// unique.
	const CMemory ringWalk = Module_FindPattern(g_GameDll,
		"48 83 EC ? 48 8B 05 ? ? ? ? 48 8B 0D ? ? ? ? 66 0F 6E 40");
	ringWalk.GetPtr(LungeLagCompBacktrack);

	if (LungeLagCompBacktrack)
	{
		// +0x4: mov rax, cs:meleeSettings  -- pointer slot for settings+0x6C latencyTicks
		s_ppMeleeSettings = ringWalk.Offset(0x4)
			.ResolveRelativeAddress(0x3, 0x7)
			.RCast<void**>();
		if (!s_ppMeleeSettings)
			Warning(eDLL_T::SERVER, "[LUNGE-RW] latency slot unresolved -- lever inert\n");
	}
	else
	{
		Warning(eDLL_T::SERVER, "[LUNGE-RW] lag-comp ring walk pattern unresolved -- lever inert\n");
	}
}

///////////////////////////////////////////////////////////////////////////////
void VMeleeLungeProbeServer::Detour(const bool bAttach) const
{
	if (CGameMovement__MeleeWalkMove)
	{
		DetourSetup(&CGameMovement__MeleeWalkMove, &Hook_MeleeWalkMove, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::SERVER, "[LUNGE-PROBE] disabled: MeleeWalkMove pattern unresolved\n");
	}

	if (PlayerMelee_ItemPreFrame)
		DetourSetup(&PlayerMelee_ItemPreFrame, &Hook_PlayerMelee_ItemPreFrame, bAttach);

	if (CPlayer__PlayerMelee_SetAttackHitEntity)
		DetourSetup(&CPlayer__PlayerMelee_SetAttackHitEntity, &Hook_PlayerMelee_SetAttackHitEntity, bAttach);

	if (CPlayer__Lunge_SetTargetEntity)
		DetourSetup(&CPlayer__Lunge_SetTargetEntity, &Hook_Lunge_SetTargetEntity, bAttach);

	if (LungeLagCompBacktrack)
		DetourSetup(&LungeLagCompBacktrack, &Hook_LungeLagCompBacktrack, bAttach);
}
///////////////////////////////////////////////////////////////////////////////
