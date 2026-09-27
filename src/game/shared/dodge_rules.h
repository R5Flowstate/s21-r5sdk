//=============================================================================//
//
// Purpose: airborne dodge rules the S21/S3 Jump lacks, applied identically
// around each engine's Jump so prediction and the server agree:
//   - once per airtime (dodgeOnlyOnceInAir)
//   - no directional input dashes along horizontal velocity
//   - dashing against current velocity keeps all of it
//   - vertical speed after the dash is at least dodgeVerticalHeight
//
//=============================================================================//
#ifndef SHARED_DODGE_RULES_H
#define SHARED_DODGE_RULES_H

#include "public/game/shared/in_buttons.h"

struct DodgeRulesLayout_t
{
	ptrdiff_t plrClassSettings;     // settings block with class mods applied
	ptrdiff_t plrLastDodgeTime;
	ptrdiff_t plrLastTouchedGround;
	ptrdiff_t plrPlayerFlags;       // bit 0x10 makes the dodge branch return silently
	ptrdiff_t mvButtonsPressed;
	ptrdiff_t mvMoveDir;
	ptrdiff_t mvVelocity;
};

// Settings-field offsets. The engine fills its stf_* globals when the settings
// layout loads, after detours attach, so read them through the pointers per call.
struct DodgeRulesFields_t
{
	uint32_t nDodge;
	uint32_t nDodgeSpeed;
	uint32_t nKeepSpeedFrac;
	uint32_t nVerticalHeight;
};

struct DodgeRulesFieldPtrs_t
{
	const uint32_t* pDodge;
	const uint32_t* pDodgeSpeed;
	const uint32_t* pKeepSpeedFrac;
	const uint32_t* pVerticalHeight;
};

struct DodgeRulesCall_t
{
	bool bActive;
	bool bBlocked;
	bool bDirFromVelocity;
	bool bFlagSet;
	float flLastDodge0;
	float vecMoveDir0[3];
	float vecVel0[3];
};

static constexpr ptrdiff_t DODGE_CTX_OFF_PLAYER   = 0x8;
static constexpr ptrdiff_t DODGE_CTX_OFF_MOVEDATA = 0x10;
static constexpr ptrdiff_t DODGE_CTX_OFF_SETTINGS = 0x18;
static constexpr uint32_t DODGE_PLAYERFLAG_NO_DODGE = 0x10;
static constexpr uint32_t DODGE_SETTINGS_OFF_CAP = 0x100000u;
static constexpr float DODGE_OPPOSING_KEEP_FRAC = 1.0f;

inline bool DodgeRules_FieldsValid(const DodgeRulesFields_t& f)
{
	return f.nDodge < DODGE_SETTINGS_OFF_CAP && f.nDodgeSpeed < DODGE_SETTINGS_OFF_CAP
		&& f.nKeepSpeedFrac < DODGE_SETTINGS_OFF_CAP && f.nVerticalHeight < DODGE_SETTINGS_OFF_CAP;
}

inline bool DodgeRules_PtrsValid(const DodgeRulesFieldPtrs_t& p)
{
	return p.pDodge && p.pDodgeSpeed && p.pKeepSpeedFrac && p.pVerticalHeight;
}

inline bool DodgeRules_LoadFields(const DodgeRulesFieldPtrs_t& p, DodgeRulesFields_t& f)
{
	if (!DodgeRules_PtrsValid(p))
		return false;
	f.nDodge = *p.pDodge;
	f.nDodgeSpeed = *p.pDodgeSpeed;
	f.nKeepSpeedFrac = *p.pKeepSpeedFrac;
	f.nVerticalHeight = *p.pVerticalHeight;
	return DodgeRules_FieldsValid(f);
}

template <typename T>
inline T& DodgeRules_At(const uintptr_t base, const ptrdiff_t off)
{
	return *reinterpret_cast<T*>(base + off);
}

inline void DodgeRules_Pre(const uintptr_t ctx, const bool bAirborne, const bool bOnceInAir,
	const DodgeRulesLayout_t& L, const DodgeRulesFields_t& F, DodgeRulesCall_t& st)
{
	st = {};
	if (!bAirborne)
		return;

	const uintptr_t player = DodgeRules_At<uintptr_t>(ctx, DODGE_CTX_OFF_PLAYER);
	const uintptr_t mv = DodgeRules_At<uintptr_t>(ctx, DODGE_CTX_OFF_MOVEDATA);
	if (!player || !mv)
		return;
	if (!(DodgeRules_At<uint32_t>(mv, L.mvButtonsPressed) & (IN_JUMP | IN_DODGE)))
		return;

	const uint8_t* const pClass = DodgeRules_At<const uint8_t*>(player, L.plrClassSettings);
	if (!pClass || !pClass[F.nDodge])
		return;

	st.bActive = true;
	st.flLastDodge0 = DodgeRules_At<float>(player, L.plrLastDodgeTime);
	const float* const md = &DodgeRules_At<float>(mv, L.mvMoveDir);
	const float* const vel = &DodgeRules_At<float>(mv, L.mvVelocity);
	for (int i = 0; i < 3; ++i)
	{
		st.vecMoveDir0[i] = md[i];
		st.vecVel0[i] = vel[i];
	}

	if (bOnceInAir && st.flLastDodge0 > DodgeRules_At<float>(player, L.plrLastTouchedGround))
	{
		uint32_t& flags = DodgeRules_At<uint32_t>(player, L.plrPlayerFlags);
		if (!(flags & DODGE_PLAYERFLAG_NO_DODGE))
		{
			flags |= DODGE_PLAYERFLAG_NO_DODGE;
			st.bFlagSet = true;
		}
		st.bBlocked = true;
		return;
	}

	const float flDirSqr = md[0] * md[0] + md[1] * md[1] + md[2] * md[2];
	const float flVelSqr = vel[0] * vel[0] + vel[1] * vel[1];
	if (flDirSqr <= 1e-7f && flVelSqr >= 1e-7f)
	{
		float* const mdw = &DodgeRules_At<float>(mv, L.mvMoveDir);
		mdw[0] = vel[0];
		mdw[1] = vel[1];
		mdw[2] = 0.0f;
		st.bDirFromVelocity = true;
	}
}

// Returns true when the Jump just performed a dodge.
inline bool DodgeRules_Post(const uintptr_t ctx, const DodgeRulesLayout_t& L,
	const DodgeRulesFields_t& F, const DodgeRulesCall_t& st)
{
	if (!st.bActive)
		return false;

	const uintptr_t player = DodgeRules_At<uintptr_t>(ctx, DODGE_CTX_OFF_PLAYER);
	const uintptr_t mv = DodgeRules_At<uintptr_t>(ctx, DODGE_CTX_OFF_MOVEDATA);
	const uint8_t* const pSettings = DodgeRules_At<const uint8_t*>(ctx, DODGE_CTX_OFF_SETTINGS);
	const uint8_t* const pClass = DodgeRules_At<const uint8_t*>(player, L.plrClassSettings);

	if (st.bFlagSet)
		DodgeRules_At<uint32_t>(player, L.plrPlayerFlags) &= ~DODGE_PLAYERFLAG_NO_DODGE;

	float* const md = &DodgeRules_At<float>(mv, L.mvMoveDir);
	const float dir[3] = { md[0], md[1], md[2] };
	if (st.bDirFromVelocity)
	{
		md[0] = st.vecMoveDir0[0];
		md[1] = st.vecMoveDir0[1];
		md[2] = st.vecMoveDir0[2];
	}

	if (st.bBlocked || DodgeRules_At<float>(player, L.plrLastDodgeTime) == st.flLastDodge0)
		return false;
	if (!pSettings || !pClass)
		return true;

	float* const vel = &DodgeRules_At<float>(mv, L.mvVelocity);

	// Same dash vector and keep fraction the engine just applied.
	const float flDirLen = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	const float flInv = 1.0f / fmaxf(flDirLen, FLT_MIN);
	const float flSpeed = *reinterpret_cast<const float*>(pClass + F.nDodgeSpeed);
	const float dashX = dir[0] * flInv * flSpeed;
	const float dashY = dir[1] * flInv * flSpeed;
	const float flOld = sqrtf(st.vecVel0[0] * st.vecVel0[0] + st.vecVel0[1] * st.vecVel0[1]);
	const float flDash = sqrtf(dashX * dashX + dashY * dashY);
	const float flFrac = (flOld > flDash) ? 1.0f - flDash / flOld : 0.0f;

	if (dashX * st.vecVel0[0] + dashY * st.vecVel0[1] < 0.0f)
	{
		const float flKeep = *reinterpret_cast<const float*>(pSettings + F.nKeepSpeedFrac);
		const float flEngineK = flFrac + (1.0f - flFrac) * flKeep;
		const float flWantK = flFrac + (1.0f - flFrac) * DODGE_OPPOSING_KEEP_FRAC;
		vel[0] += st.vecVel0[0] * (flWantK - flEngineK);
		vel[1] += st.vecVel0[1] * (flWantK - flEngineK);
	}

	const float flVertical = *reinterpret_cast<const float*>(pSettings + F.nVerticalHeight);
	vel[2] = fmaxf(flVertical, vel[2]);
	return true;
}

#endif // SHARED_DODGE_RULES_H
