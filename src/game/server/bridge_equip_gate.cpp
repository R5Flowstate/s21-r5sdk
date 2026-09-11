//=============================================================================//
//
// Purpose: full S3 ACT_MP_EQUIP_* block for IsPlaying3pEquipActivity.
// See bridge_equip_gate.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "bridge_equip_gate.h"


typedef bool(__fastcall* PFN_CPlayer_Weapon_IsPlaying3pEquipActivity)(__int64 player, int layerArg);
typedef __int64(__fastcall* PFN_CBaseAnimating_GetModelPtr)(__int64 entity);
typedef int(__fastcall* PFN_GetSequenceActivity)(__int64 studiohdr, int sequence, int flags);

static PFN_CPlayer_Weapon_IsPlaying3pEquipActivity v_CPlayer_Weapon_IsPlaying3pEquipActivity = nullptr;
static PFN_CBaseAnimating_GetModelPtr               v_CBaseAnimating_GetModelPtr               = nullptr;
static PFN_GetSequenceActivity                     v_GetSequenceActivity                     = nullptr;

static ConVar bridge_equip_gate_full_block("bridge_equip_gate_full_block", "1",
	FCVAR_RELEASE,
	"Accept the full S3 ACT_MP_EQUIP_* block (802..810) in "
	"Weapon_IsPlaying3pEquipActivity. The S3 engine jump-table excludes two of "
	"those nine third-person equip activities while the S21 client accepts its "
	"whole equip block, so the two engines take opposite branches in "
	"Weapon_UpdateSelection on a pistol-to-pistol or rocket-to-rocket switch. "
	"0 = legacy S3 behaviour (A/B).");

static ConVar bridge_equip_gate_diag("bridge_equip_gate_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"When the reimplementation's verdict differs from the original, emit a "
	"rate-limited [EQUIP-GATE] line with layer/seq/act/isActive/cycle and both "
	"verdicts.");

// Overlay fields indexed by the engine's layerArg RAW -- the slot+1 fold the
// source form applies is already baked into these three bases. Do not add 1.
static constexpr ptrdiff_t PLAYER_OFF_OVERLAY_ISACTIVE  = 5046; // 0x13B6
static constexpr ptrdiff_t PLAYER_OFF_OVERLAY_SEQUENCE  = 5096; // 0x13E8, -1 = none
static constexpr ptrdiff_t PLAYER_OFF_OVERLAY_CYCLE     = 5132; // 0x140C

// S3 ACT_MP_EQUIP_* contiguous block (ActivityList dump).
static constexpr int EQUIP_ACT_LO = 802;
static constexpr int EQUIP_ACT_HI = 810;

static long s_nEquipGateDiagLines = 0;
static constexpr long kEquipGateDiagCap = 64;

//-----------------------------------------------------------------------------
// Purpose: accept every ACT_MP_EQUIP_* (802..810), not the jump table's 7-of-9.
//-----------------------------------------------------------------------------
static bool __fastcall Hook_CPlayer_Weapon_IsPlaying3pEquipActivity(
	__int64 player, int layerArg)
{
	if (!bridge_equip_gate_full_block.GetBool())
		return v_CPlayer_Weapon_IsPlaying3pEquipActivity(player, layerArg);

	if (!player || !v_CBaseAnimating_GetModelPtr || !v_GetSequenceActivity)
		return v_CPlayer_Weapon_IsPlaying3pEquipActivity(player, layerArg);

	const int nSeq = *reinterpret_cast<int*>(
		player + PLAYER_OFF_OVERLAY_SEQUENCE + 4 * layerArg);

	int nAct = -1;
	if (nSeq != -1)
	{
		const __int64 pHdr = v_CBaseAnimating_GetModelPtr(player);
		if (pHdr)
			nAct = v_GetSequenceActivity(pHdr, nSeq, 0);
	}

	const unsigned char nActive = *reinterpret_cast<unsigned char*>(
		player + PLAYER_OFF_OVERLAY_ISACTIVE + layerArg);
	const float flCycle = *reinterpret_cast<float*>(
		player + PLAYER_OFF_OVERLAY_CYCLE + 4 * layerArg);

	bool bNew = false;
	if (nAct >= EQUIP_ACT_LO && nAct <= EQUIP_ACT_HI && nActive != 0 && flCycle < 1.0f)
		bNew = true;

	if (bridge_equip_gate_diag.GetBool() && s_nEquipGateDiagLines < kEquipGateDiagCap)
	{
		const bool bOrig = v_CPlayer_Weapon_IsPlaying3pEquipActivity(player, layerArg);
		if (bOrig != bNew)
		{
			++s_nEquipGateDiagLines;
			Warning(eDLL_T::SERVER,
				"[EQUIP-GATE] layer=%d seq=%d act=%d isActive=%d cycle=%.3f "
				"orig=%d new=%d\n",
				layerArg, nSeq, nAct, static_cast<int>(nActive), flCycle,
				bOrig ? 1 : 0, bNew ? 1 : 0);
		}
	}

	return bNew;
}

///////////////////////////////////////////////////////////////////////////////
void VBridgeEquipGate::GetAdr(void) const
{
	LogFunAdr("CPlayer::Weapon_IsPlaying3pEquipActivity",
		v_CPlayer_Weapon_IsPlaying3pEquipActivity);
	LogFunAdr("CBaseAnimating::GetModelPtr", v_CBaseAnimating_GetModelPtr);
	LogFunAdr("GetSequenceActivity", v_GetSequenceActivity);
}

void VBridgeEquipGate::GetFun(void) const
{
	// CPlayer::Weapon_IsPlaying3pEquipActivity (server half). Landmark is the
	// overlay-sequence load at +0x13E8.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 63 FA 48 8B D9 "
		"8B B4 B9 E8 13 00 00 83 FE FF 75 04 8B C6 EB 26")
		.GetPtr(v_CPlayer_Weapon_IsPlaying3pEquipActivity);

	if (v_CPlayer_Weapon_IsPlaying3pEquipActivity)
	{
		const CMemory fn(reinterpret_cast<uintptr_t>(
			v_CPlayer_Weapon_IsPlaying3pEquipActivity));

		v_CBaseAnimating_GetModelPtr = fn.Offset(0x25)
			.FollowNearCallSelf()
			.RCast<PFN_CBaseAnimating_GetModelPtr>();
		v_GetSequenceActivity = fn.Offset(0x46)
			.FollowNearCallSelf()
			.RCast<PFN_GetSequenceActivity>();

		if (!v_CBaseAnimating_GetModelPtr || !v_GetSequenceActivity)
		{
			Warning(eDLL_T::SERVER,
				"[EQUIP-GATE] GetModelPtr/GetSequenceActivity unresolved -- "
				"full-block equip gate inactive\n");
			v_CBaseAnimating_GetModelPtr = nullptr;
			v_GetSequenceActivity = nullptr;
		}
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[EQUIP-GATE] CPlayer::Weapon_IsPlaying3pEquipActivity pattern "
			"unresolved -- full-block equip gate inactive\n");
	}
}

void VBridgeEquipGate::GetVar(void) const { }
void VBridgeEquipGate::GetCon(void) const { }

void VBridgeEquipGate::Detour(const bool bAttach) const
{
	if (v_CPlayer_Weapon_IsPlaying3pEquipActivity
		&& v_CBaseAnimating_GetModelPtr
		&& v_GetSequenceActivity)
	{
		DetourSetup(&v_CPlayer_Weapon_IsPlaying3pEquipActivity,
			&Hook_CPlayer_Weapon_IsPlaying3pEquipActivity, bAttach);
	}
}
///////////////////////////////////////////////////////////////////////////////

