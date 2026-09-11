//=============================================================================//
//
// Purpose: Jump-pad parity on the dedi -- ducked m_vertOverride scale during
// the launch pass, and per-player relaunch debounce authored into
// m_jumpPadDebounceExpireTime so the client stops re-punching every tick.
//
//=============================================================================//
#include "core/stdafx.h"


#include "jumppad_parity.h"
#include "trigger_cannon.h"
#include "trigger_gravity.h"
#include "trigger_updraft.h"
#include "entitylist.h" // g_serverEntityList
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"
#include <cmath>

//-----------------------------------------------------------------------------
// Raw layout -- movement ctx, player, trigger.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t JP_CTX_OFF_PLAYER = 8; // CPlayer*

static constexpr ptrdiff_t JP_PLAYER_OFF_DUCKSTATE     = 26096; // int m_duckState
static constexpr ptrdiff_t JP_PLAYER_OFF_TOUCHED_TRIG  = 27096; // EHANDLE array start
static constexpr ptrdiff_t JP_PLAYER_OFF_TOUCHED_COUNT = 27160; // int64 live entry count
static constexpr ptrdiff_t JP_PLAYER_OFF_LAUNCHCOUNT   = 27172; // int m_launchCount

static constexpr ptrdiff_t JP_TRIG_OFF_VERTOVERRIDE = 3364; // float m_vertOverride
static constexpr ptrdiff_t JP_TRIG_OFF_TRIGGERTYPE  = 3384; // int m_triggerType; 1 = jump pad
static constexpr ptrdiff_t JP_TRIG_OFF_LAUNCHDIR    = 3416; // float[3] m_launchDir

static constexpr int JP_TOUCHED_CAP = 16;
static constexpr int JP_TRIGGER_TYPE_JUMPPAD = 1;

static ConVar bridge_jumppad_ducked_vert(
	"bridge_jumppad_ducked_vert", "1", FCVAR_RELEASE,
	"Scale jump-pad m_vertOverride for ducked players during the launch pass so "
	"server launch matches client prediction. Transient; field is restored before return.");

// Default 0.5 mirrors the client's live ducked vert-override scale.
static ConVar bridge_jumppad_ducked_vert_scalar(
	"bridge_jumppad_ducked_vert_scalar", "0.5", FCVAR_RELEASE,
	"Scalar applied to jump-pad m_vertOverride when the launching player is ducked. "
	"Default 0.5 matches the client's live ducked vert-override scale.");

static ConVar bridge_jumppad_debounce(
	"bridge_jumppad_debounce", "1", FCVAR_RELEASE,
	"Gate jump-pad relaunch on the dedi by authoring m_jumpPadDebounceExpireTime. "
	"Suppresses pad triggers while the debounce is active.");

static ConVar bridge_jumppad_debounce_time(
	"bridge_jumppad_debounce_time", "1.0", FCVAR_RELEASE,
	"Seconds added to curtime when arming m_jumpPadDebounceExpireTime after a real launch.");

// One restore loop for both mutation kinds.
struct JumpPadMutation_t
{
	void* pEnt;
	float flVertOriginal;
	int   nTypeOriginal;
	bool  bScaledVert;
	bool  bSuppressedType;
};

static int s_nDuckVertDiagCount = 0;
static int s_nDebounceArmDiagCount = 0;
static int s_nDebounceBlockDiagCount = 0;

static void JumpPad_ArmLimitedAirLocks(void* pPlayer)
{
	if (!pPlayer || !g_serverEntityList)
		return;

	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);
	int64_t nCount = *reinterpret_cast<int64_t*>(pPlayerBytes + JP_PLAYER_OFF_TOUCHED_COUNT);
	if (nCount < 0)
		nCount = 0;
	if (nCount > JP_TOUCHED_CAP)
		nCount = JP_TOUCHED_CAP;

	for (int i = 0; i < static_cast<int>(nCount); ++i)
	{
		const uint32_t rawHandle = *reinterpret_cast<uint32_t*>(
			pPlayerBytes + JP_PLAYER_OFF_TOUCHED_TRIG + 4 * i);
		if (rawHandle == 0xFFFFFFFFu)
			continue;

		const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
		void* const pEnt = g_serverEntityList->LookupEntity(handle);
		if (!pEnt)
			continue;

		const int nType = *reinterpret_cast<const int*>(
			static_cast<uint8_t*>(pEnt) + JP_TRIG_OFF_TRIGGERTYPE);
		if (nType != JP_TRIGGER_TYPE_JUMPPAD)
			continue;

		TriggerCannon_OnJumpPadLaunched(pPlayer, pEnt);
	}
}

static int64_t JumpPad_OrigAndArm(void* pCtx, void* pPlayer)
{
	int nBefore = 0;
	if (pPlayer)
		nBefore = *reinterpret_cast<int*>(
			static_cast<uint8_t*>(pPlayer) + JP_PLAYER_OFF_LAUNCHCOUNT);

	const int64_t nResult = JumpPad__ApplyLaunchPass(pCtx);

	if (pPlayer)
	{
		const int nAfter = *reinterpret_cast<int*>(
			static_cast<uint8_t*>(pPlayer) + JP_PLAYER_OFF_LAUNCHCOUNT);
		if (nAfter > nBefore)
			JumpPad_ArmLimitedAirLocks(pPlayer);
	}

	return nResult;
}



//-----------------------------------------------------------------------------
// Purpose: JumpPad launch pass -- ducked vert scale + relaunch debounce.
//-----------------------------------------------------------------------------
static int64_t JumpPad_ApplyLaunchPassInner(void* pCtx)
{
	const bool bDuckFeat = bridge_jumppad_ducked_vert.GetBool();
	const bool bDebounceFeat = bridge_jumppad_debounce.GetBool();

	if ((!bDuckFeat && !bDebounceFeat) || !pCtx)
		return JumpPad_OrigAndArm(pCtx, nullptr);

	void* const pPlayer = *reinterpret_cast<void**>(
		static_cast<uint8_t*>(pCtx) + JP_CTX_OFF_PLAYER);
	if (!pPlayer)
		return JumpPad_OrigAndArm(pCtx, nullptr);

	// Debounce: allowed only when curtime > expire. Unresolved offset fails open.
	bool bDebounceOk = false;
	bool bLaunchAllowed = true;
	float flDebounce = 0.0f;
	if (bDebounceFeat)
	{
		bDebounceOk = true;
		flDebounce = PlayerExtend_GetF32(pPlayer,
			offsetof(PlayerExtendWire, m_jumpPadDebounceExpireTime));
		if (gpGlobals && gpGlobals->curTime <= flDebounce)
			bLaunchAllowed = false;
	}

	// Duck scale readiness (same gate as before).
	bool bDoDuckScale = false;
	float flScalar = 1.0f;
	int nDuckState = 0;
	if (bDuckFeat)
	{
		nDuckState = *reinterpret_cast<int*>(
			static_cast<uint8_t*>(pPlayer) + JP_PLAYER_OFF_DUCKSTATE);
		// Client gate: (unsigned)(duckState - 1) <= 1  =>  duckState is 1 or 2.
		if (static_cast<unsigned>(nDuckState - 1) <= 1u)
		{
			flScalar = bridge_jumppad_ducked_vert_scalar.GetFloat();
			if (std::isfinite(flScalar) && flScalar > 0.0f && flScalar != 1.0f)
				bDoDuckScale = true;
		}
	}

	const bool bNeedSuppress = bDebounceOk && !bLaunchAllowed;
	const bool bNeedWalk = bDoDuckScale || bNeedSuppress;

	// Debounce arm-only path still needs the original call + launchCount check.
	if (!bNeedWalk && !bDebounceOk)
		return JumpPad_OrigAndArm(pCtx, pPlayer);

	uint8_t* const pPlayerBytes = static_cast<uint8_t*>(pPlayer);

	JumpPadMutation_t mut[JP_TOUCHED_CAP];
	int nMut = 0;

	if (bNeedWalk && g_serverEntityList)
	{
		int64_t nCount = *reinterpret_cast<int64_t*>(pPlayerBytes + JP_PLAYER_OFF_TOUCHED_COUNT);
		if (nCount < 0)
			nCount = 0;
		if (nCount > JP_TOUCHED_CAP)
			nCount = JP_TOUCHED_CAP;

		for (int i = 0; i < static_cast<int>(nCount); ++i)
		{
			const uint32_t rawHandle = *reinterpret_cast<uint32_t*>(
				pPlayerBytes + JP_PLAYER_OFF_TOUCHED_TRIG + 4 * i);
			if (rawHandle == 0xFFFFFFFFu)
				continue;

			const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
			void* const pEnt = g_serverEntityList->LookupEntity(handle);
			if (!pEnt)
				continue;

			uint8_t* const pEntBytes = static_cast<uint8_t*>(pEnt);
			int* const pType = reinterpret_cast<int*>(pEntBytes + JP_TRIG_OFF_TRIGGERTYPE);
			if (*pType != JP_TRIGGER_TYPE_JUMPPAD)
				continue;

			// Debounce blocks launch: poke type so the engine skips this pad.
			if (bNeedSuppress)
			{
				mut[nMut].pEnt = pEnt;
				mut[nMut].flVertOriginal = 0.0f;
				mut[nMut].nTypeOriginal = *pType;
				mut[nMut].bScaledVert = false;
				mut[nMut].bSuppressedType = true;
				++nMut;

				*pType = 0;

				if (s_nDebounceBlockDiagCount < 8)
				{
					++s_nDebounceBlockDiagCount;
					DevMsg(eDLL_T::SERVER,
						"[JP-DEBOUNCE] blocked relaunch t=%.3f expire=%.3f\n",
						gpGlobals ? gpGlobals->curTime : 0.0f, flDebounce);
				}
				continue;
			}

			// Vert scale only when launch is not suppressed.
			if (!bDoDuckScale)
				continue;

			// Engine only takes the vert-override path when launch dir is unset.
			const float* const pLaunchDir = reinterpret_cast<const float*>(
				pEntBytes + JP_TRIG_OFF_LAUNCHDIR);
			if (pLaunchDir[0] != 0.0f || pLaunchDir[1] != 0.0f || pLaunchDir[2] != 0.0f)
				continue;

			float* const pVert = reinterpret_cast<float*>(pEntBytes + JP_TRIG_OFF_VERTOVERRIDE);
			const float flOriginal = *pVert;

			mut[nMut].pEnt = pEnt;
			mut[nMut].flVertOriginal = flOriginal;
			mut[nMut].nTypeOriginal = 0;
			mut[nMut].bScaledVert = true;
			mut[nMut].bSuppressedType = false;
			++nMut;

			*pVert = flOriginal * flScalar;

			if (s_nDuckVertDiagCount < 8)
			{
				++s_nDuckVertDiagCount;
				DevMsg(eDLL_T::SERVER,
					"[JP-DUCKVERT] duckState=%d vert %.3f -> %.3f\n",
					nDuckState, flOriginal, flOriginal * flScalar);
			}
		}
	}

	const int nLaunchBefore = bDebounceOk
		? *reinterpret_cast<int*>(pPlayerBytes + JP_PLAYER_OFF_LAUNCHCOUNT)
		: 0;

	const int64_t nResult = JumpPad_OrigAndArm(pCtx, pPlayer);

	// Restore before any further work -- these fields are replicated.
	for (int i = 0; i < nMut; ++i)
	{
		uint8_t* const pEntBytes = static_cast<uint8_t*>(mut[i].pEnt);
		if (mut[i].bScaledVert)
		{
			float* const pVert = reinterpret_cast<float*>(pEntBytes + JP_TRIG_OFF_VERTOVERRIDE);
			*pVert = mut[i].flVertOriginal;
		}
		if (mut[i].bSuppressedType)
		{
			int* const pType = reinterpret_cast<int*>(pEntBytes + JP_TRIG_OFF_TRIGGERTYPE);
			*pType = mut[i].nTypeOriginal;
		}
	}

	if (bDebounceOk)
	{
		const int nLaunchAfter = *reinterpret_cast<int*>(
			pPlayerBytes + JP_PLAYER_OFF_LAUNCHCOUNT);
		if (nLaunchAfter > nLaunchBefore && gpGlobals)
		{
			const float flExpire =
				gpGlobals->curTime + bridge_jumppad_debounce_time.GetFloat();
			PlayerExtend_SetF32(pPlayer, offsetof(PlayerExtendWire, m_jumpPadDebounceExpireTime), flExpire);
			MarkEntityEdictDirty(pPlayer);

			if (s_nDebounceArmDiagCount < 8)
			{
				++s_nDebounceArmDiagCount;
				DevMsg(eDLL_T::SERVER,
					"[JP-DEBOUNCE] armed t=%.3f (launches=%d)\n",
					flExpire, nLaunchAfter);
			}
		}
	}

	return nResult;
}

//-----------------------------------------------------------------------------
// Purpose: this detour owns the only entry into the predicted-trigger pass, so
// the trigger types the engine has no case for are applied here rather than
// through a second hook on the same function.
//-----------------------------------------------------------------------------
static int64_t Hook_JumpPad_ApplyLaunchPass(void* pCtx)
{
	const int64_t nResult = JumpPad_ApplyLaunchPassInner(pCtx);
	TriggerCannon_ApplyPass(pCtx);
	TriggerGravity_ApplyPass(pCtx);
	UpdraftBridge_ApplyPass(pCtx);
	return nResult;
}

void VJumpPadParity::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"40 55 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 48 8B F9 48 8B 49 08 80 B9 99 04 00 00 00 75 ?? 48 8B 05 ?? ?? ?? ?? F3 0F 10 48 28 0F 2F 89 B4 15 00 00")
		.GetPtr(JumpPad__ApplyLaunchPass);

	if (!JumpPad__ApplyLaunchPass)
		Warning(eDLL_T::SERVER,
			"[JP-PARITY] JumpPad::ApplyLaunchPass pattern unresolved -- "
			"jumppad ducked-vert and debounce parity are disabled\n");
}

void VJumpPadParity::Detour(const bool bAttach) const
{
	if (JumpPad__ApplyLaunchPass)
		DetourSetup(&JumpPad__ApplyLaunchPass, &Hook_JumpPad_ApplyLaunchPass, bAttach);
}

