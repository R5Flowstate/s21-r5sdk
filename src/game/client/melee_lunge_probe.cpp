//=============================================================================//
//
// Purpose: name the input behind the melee lunge-boost prediction divergence.
// m_flForwardMove at mv+48 scales maxSpd; engines disagree only while moving.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/client/melee_lunge_probe.h"
#include "game/client/pred_authority.h"   // PredNative_ResolveEHandle

static ConVar bridge_melee_lunge_probe("bridge_melee_lunge_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[LUNGE-PROBE] log m_flForwardMove and the lungeBoost accumulation inside "
	"MeleeWalkMove. Compare against the dedi's lines. 0 = off (default).");

static constexpr ptrdiff_t kMoveDataForwardMove = 48;    // m_flForwardMove
static constexpr ptrdiff_t kMoveDataVelocity    = 292;   // m_vecVelocity (client layout)
static constexpr ptrdiff_t kPlayerLungeBoost    = 12872; // m_melee.lungeBoost
static constexpr ptrdiff_t kPlayerMoveSpeedScale = 12916; // m_playerMoveSpeedScale
static constexpr ptrdiff_t kPlayerCachedMoveScale = 12920; // m_cachedMoveScale

// SetupMove reads the whole weapon term through this ONE handle -- both the
// fireMode branch test and the modvar select resolve it, so it is the identity
// behind the `weap` residual.
static constexpr ptrdiff_t kPlayerActiveWeapon  = 6448;  // active-weapon EHANDLE
static constexpr ptrdiff_t kWeaponMoveSpeedMod  = 7044;  // move_speed_modifier
static constexpr ptrdiff_t kWeaponMoveSpeedAdsP = 7048;  // ..._ads_passive
static constexpr ptrdiff_t kWeaponFireMode      = 11208; // fireMode

static ConVar bridge_melee_press_probe("bridge_melee_press_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[MELEE-PRESS]/[MELEE-HIT] log the melee press machine on every state change and "
	"every hit registration, in the same field order as the dedi, so press and hit "
	"events pair one-for-one across the two logs. 0 = off (default).");

// m_Local -- the press machine's whole state.
static constexpr ptrdiff_t kPlayerQueuedMeleePress   = 8012;
static constexpr ptrdiff_t kPlayerQueuedGrappleMelee = 8016;
static constexpr ptrdiff_t kPlayerDisableUntilRel    = 8028;
static constexpr ptrdiff_t kPlayerMeleePressTime     = 8032;
static constexpr ptrdiff_t kPlayerMeleeInputIndex    = 8036;
static constexpr ptrdiff_t kPlayerButtonsPressed     = 10756;
static constexpr ptrdiff_t kPlayerButtonsReleased    = 10760;
static constexpr ptrdiff_t kPlayerButtonsHeld        = 10764;
// The press QUEUE is gated on this being != 2 (launching). The dedi has no such
// gate, so a press queued mid-launch exists on one engine only.
static constexpr ptrdiff_t kPlayerLaunchState        = 7484;

// m_melee (struct base 12832).
static constexpr ptrdiff_t kMeleeParity        = 12840;
static constexpr ptrdiff_t kMeleeAttackActive  = 12844;
static constexpr ptrdiff_t kMeleeAttackStart   = 12848;
static constexpr ptrdiff_t kMeleeHitEntity     = 12852;
static constexpr ptrdiff_t kMeleeHitTime       = 12856;
static constexpr ptrdiff_t kMeleeLastNonWorld  = 12860;
static constexpr ptrdiff_t kMeleeScriptedState = 12864;
static constexpr ptrdiff_t kMeleePendingPress  = 12868;

static bool s_bFirstFire = true;
static unsigned s_nMoveScaleProbeCount = 0;
static bool s_bPressFirstFire = true;

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
	int nLaunch = -1;

	if (bProbe)
	{
		MeleePress_Read(pPlayer, before);
		nHeld     = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerButtonsHeld);
		nPressed  = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerButtonsPressed);
		nReleased = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerButtonsReleased);
		nLaunch   = *reinterpret_cast<const int*>(pPlayer + kPlayerLaunchState);
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
		Warning(eDLL_T::CLIENT, "[MELEE-PRESS] FIRST FIRE -- client press machine probe live\n");
		s_bPressFirstFire = false;
	}

	const unsigned nWpn = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerActiveWeapon);
	const uintptr_t pWeapon = reinterpret_cast<uintptr_t>(PredNative_ResolveEHandle(nWpn));
	const int nFireMode = pWeapon ? *reinterpret_cast<const int*>(pWeapon + kWeaponFireMode) : -1;

	Warning(eDLL_T::CLIENT,
		"[MELEE-PRESS] client held=%08X pressed=%08X released=%08X launch=%d "
		"idx=%d->%d queued=%.4f->%.4f grapple=%.4f->%.4f pressT=%.4f->%.4f "
		"disableUntilRel=%d->%d active=%d->%d pend=%d->%d parity=%d->%d state=%d->%d "
		"wpn=%d:%d fm=%d\n",
		nHeld, nPressed, nReleased, nLaunch,
		before.nIndex, after.nIndex,
		before.flQueued, after.flQueued,
		before.flQueuedGrapple, after.flQueuedGrapple,
		before.flPressTime, after.flPressTime,
		before.bDisableUntilRel, after.bDisableUntilRel,
		before.bAttackActive, after.bAttackActive,
		before.bPendingPress, after.bPendingPress,
		before.nParity, after.nParity,
		before.nState, after.nState,
		(nWpn == 0xFFFFFFFFu) ? -1 : static_cast<int>(nWpn & 0xFFFF),
		static_cast<int>(nWpn >> 16), nFireMode);
}

static int64_t Hook_PlayerMelee_SetAttackHitEntity(void* player, void* ent, void* unused)
{
	const bool bProbe = bridge_melee_press_probe.GetBool() && player;
	const uintptr_t pPlayer = reinterpret_cast<uintptr_t>(player);

	float flHitBefore = 0.0f, flNonWorldBefore = 0.0f;
	if (bProbe)
	{
		flHitBefore      = *reinterpret_cast<const float*>(pPlayer + kMeleeHitTime);
		flNonWorldBefore = *reinterpret_cast<const float*>(pPlayer + kMeleeLastNonWorld);
	}

	const int64_t result = C_Player__PlayerMelee_SetAttackHitEntity(player, ent, unused);

	if (bProbe)
	{
		const unsigned nHit = *reinterpret_cast<const unsigned*>(pPlayer + kMeleeHitEntity);
		Warning(eDLL_T::CLIENT,
			"[MELEE-HIT] client entPtr=%p hitEnt=%d:%d active=%d startT=%.4f "
			"hitT=%.4f->%.4f lastNonWorldT=%.4f->%.4f parity=%d state=%d\n",
			ent,
			(nHit == 0xFFFFFFFFu) ? -1 : static_cast<int>(nHit & 0xFFFF),
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

	C_GameMovement__MeleeWalkMove(ctx);

	if (bProbe && pPlayer && pMove)
	{
		const float* b = reinterpret_cast<const float*>(pPlayer + kPlayerLungeBoost);
		const float* v = reinterpret_cast<const float*>(pMove + kMoveDataVelocity);

		const bool bChanged = fabsf(b[0] - boostBefore[0]) > 1e-6f
			|| fabsf(b[1] - boostBefore[1]) > 1e-6f
			|| fabsf(b[2] - boostBefore[2]) > 1e-6f;

		// Only the commands that actually accumulate boost are interesting.
		if (bChanged || s_bFirstFire)
		{
			if (s_bFirstFire)
			{
				Warning(eDLL_T::CLIENT, "[LUNGE-PROBE] FIRST FIRE -- client MeleeWalkMove probe live\n");
				s_bFirstFire = false;
			}

			const float flScale = (fminf(fmaxf(flFwd, 0.0f), 1.0f) * 0.5f) + 0.5f;

			// maxSpeed = 2 * range / duration; whichever of the two differs from
			// the dedi's line is the divergent dataset value.
			const float flDur   = GetLungeDuration ? GetLungeDuration(reinterpret_cast<void*>(pPlayer)) : -1.0f;
			const float flRange = GetLungeNoTargetRange ? GetLungeNoTargetRange(reinterpret_cast<void*>(pPlayer)) : -1.0f;
			const float flMax   = (flDur > 0.0f) ? ((2.0f * flRange) / flDur) * flScale : -1.0f;

			const float flWalkMax = C_Player__GetPlayerMoveMaxSpeed
				? C_Player__GetPlayerMoveMaxSpeed(reinterpret_cast<void*>(pPlayer)) : -1.0f;

			// walkMax = m_cachedMoveScale * classSettings[pose][speed]; splitting the
			// product says whether the divergence is a runtime modifier or the data.
			const float flMoveScale = *reinterpret_cast<const float*>(pPlayer + kPlayerCachedMoveScale);
			const float flSetting   = (flMoveScale != 0.0f) ? (flWalkMax / flMoveScale) : -1.0f;

			Warning(eDLL_T::CLIENT,
				"[LUNGE-PROBE] client fwdMove=%.4f scale=%.4f dur=%.4f range=%.4f maxSpd=%.3f "
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
			const float flLand = C_Player__GetRecentLandingMoveSpeedScale
				? C_Player__GetRecentLandingMoveSpeedScale(reinterpret_cast<void*>(pPlayer)) : -1.0f;

			float flSev = 0.0f;
			float flTime = 0.0f;
			float flBoost = -1.0f;
			if (GetStatusEffectSeverityAndTime && g_pStatusEffectTypeIndex_SpeedBoost)
			{
				GetStatusEffectSeverityAndTime(reinterpret_cast<void*>(pPlayer),
					*g_pStatusEffectTypeIndex_SpeedBoost, &flSev, &flTime, false, false);
				flBoost = 1.0f + 2.0f * flSev;
			}
			else
			{
				flSev = -1.0f;
			}

			// weaponChain is the residual: cached / (msss * (1-slow) * boost * land)
			const float flDenom = flMsss * (1.0f - flSlow) * flBoost * flLand;
			const float flWeap = (fabsf(flDenom) > 1e-6f) ? (flCached / flDenom) : -1.0f;

			// `weap` is two-valued (1.0 / 1.15) on both engines but with different
			// duty cycles, so the open question is which weapon each engine has
			// active at the same instant, and what modvars that weapon carries.
			const unsigned nWpn = *reinterpret_cast<const unsigned*>(pPlayer + kPlayerActiveWeapon);
			const uintptr_t pWeapon = reinterpret_cast<uintptr_t>(PredNative_ResolveEHandle(nWpn));

			int nFireMode = -1;
			float flMsm = -1.0f, flAdsp = -1.0f;
			if (pWeapon)
			{
				nFireMode = *reinterpret_cast<const int*>(pWeapon + kWeaponFireMode);
				flMsm     = *reinterpret_cast<const float*>(pWeapon + kWeaponMoveSpeedMod);
				flAdsp    = *reinterpret_cast<const float*>(pWeapon + kWeaponMoveSpeedAdsP);
			}

			Warning(eDLL_T::CLIENT,
				"[MOVESCALE] client cached=%.4f msss=%.4f slow=%.4f boost=%.4f land=%.4f weap=%.4f sevRaw=%.4f "
				"wpn=%d:%d ptr=%p fm=%d msm=%.4f adsp=%.4f\n",
				flCached, flMsss, flSlow, flBoost, flLand, flWeap, flSev,
				(nWpn == 0xFFFFFFFFu) ? -1 : static_cast<int>(nWpn & 0xFFFF),
				static_cast<int>(nWpn >> 16), reinterpret_cast<const void*>(pWeapon),
				nFireMode, flMsm, flAdsp);
		}
	}
}

//-----------------------------------------------------------------------------
// [ANIMEVT-DEDUP] Rebase restores a consumed stamp; re-zero identical stamps.
//-----------------------------------------------------------------------------
static ConVar bridge_animevt_dedup("bridge_animevt_dedup", "1", FCVAR_RELEASE,
	"[ANIMEVT-DEDUP] Re-zero predicted anim-event slots a prediction rebase handed "
	"back after this client already consumed them, so the event cannot fire twice. "
	"1 = on (default), 0 = engine behaviour.");

// m_predictedAnimEventData, entity-relative. Confirmed against both the S21
// binary and the client's own [PRED-FIELDS] dump.
static constexpr ptrdiff_t kAnimEvtTimes   = 2720;
static constexpr ptrdiff_t kAnimEvtCount   = 2784;
static constexpr int       kAnimEvtSlots   = 8;
// A restore reinstates the exact float; only the domain shift on element 0 moves
// it, and that is ~0.02. The next real event on a slot is a swing away.
static constexpr float     kAnimEvtSameTol = 0.05f;

struct AnimEvtLedger_t
{
	void* pEnt;
	float flFired[kAnimEvtSlots];
};
static AnimEvtLedger_t s_animEvtLedger[8] = {};
static int s_animEvtNextSlot = 0;

static AnimEvtLedger_t* AnimEvt_LedgerFor(void* pEnt)
{
	for (AnimEvtLedger_t& l : s_animEvtLedger)
	{
		if (l.pEnt == pEnt)
			return &l;
	}
	AnimEvtLedger_t* const pNew = &s_animEvtLedger[s_animEvtNextSlot];
	s_animEvtNextSlot = (s_animEvtNextSlot + 1) % static_cast<int>(sizeof(s_animEvtLedger) / sizeof(s_animEvtLedger[0]));
	pNew->pEnt = pEnt;
	memset(pNew->flFired, 0, sizeof(pNew->flFired));
	return pNew;
}

static void Hook_ProcessPredictedAnimEvents(void* pAnimating)
{
	uint8_t* const pEnt = reinterpret_cast<uint8_t*>(pAnimating);
	int nCount = pAnimating ? *reinterpret_cast<int*>(pEnt + kAnimEvtCount) : 0;

	if (!pAnimating || nCount <= 0 || !bridge_animevt_dedup.GetBool())
	{
		C_BaseAnimating__ProcessPredictedAnimEvents(pAnimating);
		return;
	}

	if (nCount > kAnimEvtSlots)
		nCount = kAnimEvtSlots;

	float* const pTimes = reinterpret_cast<float*>(pEnt + kAnimEvtTimes);
	AnimEvtLedger_t* const pLedger = AnimEvt_LedgerFor(pAnimating);

	for (int i = 0; i < nCount; ++i)
	{
		if (pTimes[i] == 0.0f || pLedger->flFired[i] == 0.0f
			|| fabsf(pTimes[i] - pLedger->flFired[i]) >= kAnimEvtSameTol)
			continue;

		pTimes[i] = 0.0f;

	}

	float flBefore[kAnimEvtSlots];
	memcpy(flBefore, pTimes, sizeof(float) * nCount);

	C_BaseAnimating__ProcessPredictedAnimEvents(pAnimating);

	// Whatever the engine consumed this frame is what a later rebase may hand back.
	for (int i = 0; i < nCount; ++i)
	{
		if (flBefore[i] != 0.0f && pTimes[i] == 0.0f)
			pLedger->flFired[i] = flBefore[i];
	}
}

///////////////////////////////////////////////////////////////////////////////
void VMeleeLungeProbe::GetFun(void) const
{
	// C_GameMovement::MeleeWalkMove -- unique.
	Module_FindPattern(g_GameDll,
		"40 57 48 81 EC ? ? ? ? 48 8B 41 ? 48 8B F9 80 B8 ? ? ? ? ? 75 ? "
		"F3 0F 10 05 ? ? ? ? F3 0F 11 80 ? ? ? ? F3 0F 10 0D")
		.GetPtr(C_GameMovement__MeleeWalkMove);

	// GetLungeDuration -- unique.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 8B 81 ? ? ? ? 48 8D 35 ? ? ? ? "
		"48 8B D9 83 F8 ? 74 ? 0F B7 D0 48 C1 E2 ? C1 E8 ? 39 44 32")
		.GetPtr(GetLungeDuration);

	// GetLungeNoTargetRange -- unique.
	Module_FindPattern(g_GameDll,
		"80 B9 ? ? ? ? ? 48 8B D1 74 ? 8B 81 ? ? ? ? 83 F8 ? 74 ? 0F B7 C8 "
		"4C 8D 05 ? ? ? ? 48 C1 E1 ? C1 E8 ? 42 39 44 01 ? 75 ? 4A 8B 04 01")
		.GetPtr(GetLungeNoTargetRange);

	// GetPlayerMoveMaxSpeed via the clamp's near-call. On a miss, do not Offset
	// a failed pattern -- that dereferences the displacement and dies at spawn.
	const CMemory clampSite = Module_FindPattern(g_GameDll,
		"F3 0F 51 C9 F3 0F 5E D9 E8 ? ? ? ?");
	if (clampSite)
	{
		C_Player__GetPlayerMoveMaxSpeed = clampSite.Offset(0x8)
			.FollowNearCallSelf()
			.RCast<float(*)(void*)>();
	}
	else
	{
		Warning(eDLL_T::CLIENT, "[LUNGE-PROBE] GetPlayerMoveMaxSpeed unresolved -- walkMax not logged\n");
	}

	// Tail of the inlined move-scale product in SetupMove. The trailing
	// 74 32 00 00 is m_playerMoveSpeedScale -- leave literal; it pins the site.
	// unique.
	const CMemory moveScaleSite = Module_FindPattern(g_GameDll,
		"8B 15 ? ? ? ? 4C 8D 8C 24 ? ? ? ? 44 88 74 24 ? 4C 8D 84 24 ? ? ? ? "
		"48 8B CF 44 88 74 24 ? 44 89 B4 24 ? ? ? ? 44 89 B4 24 ? ? ? ? "
		"E8 ? ? ? ? 48 8B CF E8 ? ? ? ? 48 8B CF 0F 28 F0 E8 ? ? ? ? "
		"F3 0F 59 BF 74 32 00 00");
	if (moveScaleSite)
	{
		g_pStatusEffectTypeIndex_SpeedBoost = moveScaleSite
			.ResolveRelativeAddress(2, 6)
			.RCast<int*>();
		GetStatusEffectSeverityAndTime = moveScaleSite.Offset(0x33)
			.FollowNearCallSelf()
			.RCast<void(*)(void*, int, float*, float*, bool, bool)>();
		GetStatusEffectSeverity_MoveSlow = moveScaleSite.Offset(0x3B)
			.FollowNearCallSelf()
			.RCast<float(*)(void*)>();
		C_Player__GetRecentLandingMoveSpeedScale = moveScaleSite.Offset(0x46)
			.FollowNearCallSelf()
			.RCast<float(*)(void*)>();
	}
	else
	{
		Warning(eDLL_T::CLIENT, "[LUNGE-PROBE] move-scale factor site unresolved -- factors not logged\n");
	}

	// PlayerMelee_ItemPreFrame. Displacements 0x2A08/0x2A0C/0x1F4C/0x1F50.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 50 0F 29 74 24 40 48 8B D9 E8 ? ? ? ? "
		"8B 93 08 2A 00 00 0F 57 F6 0B 93 0C 2A 00 00 85 C2 75 ? "
		"0F 2F B3 4C 1F 00 00 76 ? 0F 2F B3 50 1F 00 00")
		.GetPtr(PlayerMelee_ItemPreFrame);

	if (!PlayerMelee_ItemPreFrame)
		Warning(eDLL_T::CLIENT, "[MELEE-PRESS] ItemPreFrame pattern unresolved -- press events not logged\n");

	// C_Player::PlayerMelee_SetAttackHitEntity. Pinned by m_pCurrentCommand
	// (0x34B8) and m_melee.attackActive (0x322C). unique.
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 48 83 B9 B8 34 00 00 00 4C 8B C9 75 ? 48 8D 0D ? ? ? ? "
		"48 83 C4 28 E9 ? ? ? ? 80 B9 2C 32 00 00 00")
		.GetPtr(C_Player__PlayerMelee_SetAttackHitEntity);

	if (!C_Player__PlayerMelee_SetAttackHitEntity)
		Warning(eDLL_T::CLIENT, "[MELEE-HIT] SetAttackHitEntity pattern unresolved -- hits not logged\n");

	// ProcessPredictedAnimEvents. Displacements 0x754 / 0xAE4 / 0xAEC.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 50 80 B9 54 07 00 00 00 48 8B D9 0F 84 ? ? ? ? "
		"8B 81 E4 0A 00 00 48 89 74 24 ? 83 F8 FF 0F 84 ? ? ? ? 0F B7 F0 "
		"48 8D 0D ? ? ? ? 48 C1 E6 05 C1 E8 10 39 44 0E 08 0F 85 ? ? ? ? "
		"48 8B 34 0E 48 85 F6 0F 84 ? ? ? ? 0F BF 86 D8 00 00 00 3B 83 EC 0A 00 00")
		.GetPtr(C_BaseAnimating__ProcessPredictedAnimEvents);

	if (!C_BaseAnimating__ProcessPredictedAnimEvents)
		Warning(eDLL_T::CLIENT, "[ANIMEVT-DEDUP] ProcessPredictedAnimEvents pattern unresolved -- "
			"a rebase can still re-fire a consumed anim event\n");
}

///////////////////////////////////////////////////////////////////////////////
void VMeleeLungeProbe::Detour(const bool bAttach) const
{
	if (C_GameMovement__MeleeWalkMove)
	{
		DetourSetup(&C_GameMovement__MeleeWalkMove, &Hook_MeleeWalkMove, bAttach);
	}
	else if (bAttach)
	{
		Warning(eDLL_T::CLIENT, "[LUNGE-PROBE] disabled: MeleeWalkMove pattern unresolved\n");
	}

	if (PlayerMelee_ItemPreFrame)
		DetourSetup(&PlayerMelee_ItemPreFrame, &Hook_PlayerMelee_ItemPreFrame, bAttach);

	if (C_Player__PlayerMelee_SetAttackHitEntity)
		DetourSetup(&C_Player__PlayerMelee_SetAttackHitEntity, &Hook_PlayerMelee_SetAttackHitEntity, bAttach);

	if (C_BaseAnimating__ProcessPredictedAnimEvents)
		DetourSetup(&C_BaseAnimating__ProcessPredictedAnimEvents, &Hook_ProcessPredictedAnimEvents, bAttach);
}
///////////////////////////////////////////////////////////////////////////////
