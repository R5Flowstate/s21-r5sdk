//=============================================================================//
//
// Purpose: Instant stand / crouch on the server CPlayer. See player_stance.h.
//
//=============================================================================//
#include "core/stdafx.h"

#include "player_stance.h"
#include "baseentity.h"
#include "game/shared/collisionproperty.h"
#include "game/shared/edict_dirty.h"

// Server CPlayer layout.
static constexpr ptrdiff_t PS_OFF_FLAGS           = 0x234;  // m_fFlags
static constexpr ptrdiff_t PS_OFF_HULLSTATE       = 0x6280; // 0 and 5 use the pose hull table below
static constexpr ptrdiff_t PS_OFF_DUCKSTATE       = 0x65F0;
static constexpr ptrdiff_t PS_OFF_HULL_STAND_MINS = 0x65FC;
static constexpr ptrdiff_t PS_OFF_HULL_STAND_MAXS = 0x6608;
static constexpr ptrdiff_t PS_OFF_HULL_DUCK_MINS  = 0x6614;
static constexpr ptrdiff_t PS_OFF_HULL_DUCK_MAXS  = 0x6620;
static constexpr ptrdiff_t PS_OFF_HULLHEIGHT      = 0x6B10;

static constexpr int PS_FL_DUCKING         = 0x2;
static constexpr int PS_DUCKSTATE_STANDING = 0;
static constexpr int PS_DUCKSTATE_DUCKED   = 2;

// Reads only the player at ctx+8.
static void (*v_CGameMovement__SetDuckedEyeOffset)(void* ctx, float flDuckFraction) = nullptr;
static float (*v_CPlayer__GetHullHeight)(void* pPlayer) = nullptr;

void PlayerStance_SetInstant(void* pPlayer, const bool bDucked)
{
	if (!pPlayer)
		return;

	uint8_t* const p = static_cast<uint8_t*>(pPlayer);
	int& nFlags = *reinterpret_cast<int*>(p + PS_OFF_FLAGS);
	nFlags = bDucked ? (nFlags | PS_FL_DUCKING) : (nFlags & ~PS_FL_DUCKING);
	*reinterpret_cast<int*>(p + PS_OFF_DUCKSTATE) = bDucked ? PS_DUCKSTATE_DUCKED : PS_DUCKSTATE_STANDING;

	if (v_CGameMovement__SetDuckedEyeOffset)
	{
		void* fakeCtx[2] = { nullptr, pPlayer };
		v_CGameMovement__SetDuckedEyeOffset(fakeCtx, bDucked ? 1.0f : 0.0f);
	}
	if (v_CPlayer__GetHullHeight)
		*reinterpret_cast<float*>(p + PS_OFF_HULLHEIGHT) = v_CPlayer__GetHullHeight(pPlayer);

	const int nHullState = *reinterpret_cast<const int*>(p + PS_OFF_HULLSTATE);
	if (nHullState == 0 || nHullState == 5)
	{
		const Vector3D& mins = *reinterpret_cast<const Vector3D*>(p + (bDucked ? PS_OFF_HULL_DUCK_MINS : PS_OFF_HULL_STAND_MINS));
		const Vector3D& maxs = *reinterpret_cast<const Vector3D*>(p + (bDucked ? PS_OFF_HULL_DUCK_MAXS : PS_OFF_HULL_STAND_MAXS));
		reinterpret_cast<CBaseEntity*>(pPlayer)->CollisionProp()->SetBounds(mins, maxs);
	}

	MarkEntityEdictDirty(pPlayer);
}

void VPlayerStance::GetAdr(void) const
{
	LogFunAdr("CGameMovement::SetDuckedEyeOffset", v_CGameMovement__SetDuckedEyeOffset);
	LogFunAdr("CPlayer::GetHullHeight", v_CPlayer__GetHullHeight);
}

void VPlayerStance::GetFun(void) const
{
	// Server halves: the +0x5F08 settings block and the duck hull maxs at
	// +0x6620 (the standing-hull twin reads +0x6614) pin them.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 60 4C 8B 41 08 0F 28 D9 8B 0D ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? "
		"48 03 C1 0F 29 74 24 50 49 8B 90 08 5F 00 00")
		.GetPtr(v_CGameMovement__SetDuckedEyeOffset);
	Module_FindPattern(g_GameDll,
		"44 8B 81 80 62 00 00 44 8B 0D ?? ?? ?? ?? 45 85 C0 74 1B 41 83 F8 05 74 15 8B 05 ?? ?? ?? ?? "
		"48 8B 91 08 5F 00 00 48 03 D0 49 03 D1 EB 1A 8B 81 34 02 00 00 48 8D 91 20 66 00 00")
		.GetPtr(v_CPlayer__GetHullHeight);

	if (!v_CGameMovement__SetDuckedEyeOffset || !v_CPlayer__GetHullHeight)
		Warning(eDLL_T::SERVER,
			"[STANCE] pattern unresolved (eye=%d hull=%d) -- instant stance changes keep the old eye/hull height\n",
			v_CGameMovement__SetDuckedEyeOffset ? 1 : 0, v_CPlayer__GetHullHeight ? 1 : 0);
}
