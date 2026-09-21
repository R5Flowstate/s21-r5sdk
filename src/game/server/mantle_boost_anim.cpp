//=============================================================================//
//
// Purpose: ACT_MP_MANTLE_BOOST_AIR selection in CMultiPlayerAnimState.
//
// The activity chain in CalcMainActivity predates mantle boost, so the check
// is grafted at the point later builds evaluate it: after the skydive states
// and before the moving/float fallback. State 4 while the animstate reports
// airborne selects the activity, so the sequence the server authors on the
// wire is the one the client's own animstate picks from the same state.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/server/player.h"
#include "game/server/mantle_boost.h"
#include "game/server/mantle_boost_anim.h"
#include "game/shared/activity.h"
#include "game/shared/midfunc_jump_patch.h"

static ConVar bridge_mantle_boost_air_anim("bridge_mantle_boost_air_anim", "1", FCVAR_RELEASE,
	"Select ACT_MP_MANTLE_BOOST_AIR while a mantle boost is airborne.");
static ConVar bridge_mantle_boost_air_anim_log("bridge_mantle_boost_air_anim_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log [MB-ANIM] activity selections.");
static ConVar bridge_mantle_boost_air_anim_early("bridge_mantle_boost_air_anim_early", "1", FCVAR_RELEASE,
	"1 = select the boost-air activity ahead of the jump/slide handlers (plays on every boost); 0 = after them (late order: only once the slide flag clears).");

static constexpr ptrdiff_t ANIMSTATE_OFF_PLAYER = 0x1C0;   // CMultiPlayerAnimState::m_player

static uint8_t* s_pSelectSite  = nullptr;   // late order: after the late special states
static uint8_t* s_pEarlySite   = nullptr;   // ahead of the jump/crouch/zipline/land/slide chain
static uint8_t* s_pEpilogue    = nullptr;
static CMidFuncJumpPatch s_patch;

static int MantleBoostAnim_ActivityId(void)
{
	static int s_nId = -1;
	static int s_nGeneration = -1;
	const int gen = ActivityList_Generation();
	if (gen != s_nGeneration)
	{
		s_nGeneration = gen;
		s_nId = FindActivityByName("ACT_MP_MANTLE_BOOST_AIR");
		if (s_nId < 0)
			Warning(eDLL_T::SERVER, "[MB-ANIM] ACT_MP_MANTLE_BOOST_AIR is not registered -- boost-air activity disabled\n");
	}
	return s_nId;
}

static int __fastcall MantleBoostAnim_Select(void* const pAnimState)
{
	if (!bridge_mantle_boost_air_anim.GetBool() || !pAnimState || !CMultiPlayerAnimState__IsOnGround)
		return -1;

	const CPlayer* const pPlayer = *reinterpret_cast<const CPlayer* const*>(
		reinterpret_cast<uintptr_t>(pAnimState) + ANIMSTATE_OFF_PLAYER);
	if (!pPlayer)
		return -1;

	if (MantleBoost_GetState(pPlayer) != 4 || CMultiPlayerAnimState__IsOnGround(pAnimState))
		return -1;

	const int nActivity = MantleBoostAnim_ActivityId();
	if (nActivity < 0)
		return -1;

	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		Warning(eDLL_T::SERVER, "[MB-ANIM] first ACT_MP_MANTLE_BOOST_AIR selection (id %d, edict %d)\n",
			nActivity, static_cast<int>(pPlayer->GetEdict()));
	}
	if (bridge_mantle_boost_air_anim_log.GetBool())
		Msg(eDLL_T::SERVER, "[MB-ANIM] edict %d -> ACT_MP_MANTLE_BOOST_AIR (%d)\n",
			static_cast<int>(pPlayer->GetEdict()), nActivity);

	return nActivity;
}

void VMantleBoostAnimServer::GetAdr(void) const
{
	LogFunAdr("CMultiPlayerAnimState::IsOnGround", CMultiPlayerAnimState__IsOnGround);
	LogVarAdr("CMultiPlayerAnimState::CalcMainActivity boost-air site", s_pSelectSite);
}

void VMantleBoostAnimServer::GetFun(void) const
{
	// Server CalcMainActivity, end of the skydive block: `cmp ecx, ACT ; cmovz eax, edx ;
	// jmp epilogue` then `test r15b, r15b ; jnz` -- the jump-anim/crouch flag tests that
	// start the moving/float fallback. Unique to the server half; the client twin in
	// the same binary has a different tail.
	const CMemory base = Module_FindPattern(g_GameDll,
		"81 F9 ?? ?? 00 00 0F 44 C2 E9 ?? ?? ?? ?? 45 84 FF 75 1C 40 84 ED 75 17 45 84 F6 75 12 40 84 F6 75 0D 48 8D 54 24 60 48 8B CF E8 ?? ?? ?? ?? 48 8B CF E8");
	if (!base)
	{
		Warning(eDLL_T::SERVER, "[MB-ANIM] CalcMainActivity site pattern unresolved -- boost-air activity disabled\n");
		return;
	}

	s_pEpilogue   = base.Offset(0x9).FollowNearCall().RCast<uint8_t*>();
	s_pSelectSite = base.Offset(0xE).RCast<uint8_t*>();
	base.Offset(0x32).FollowNearCall().GetPtr(CMultiPlayerAnimState__IsOnGround);

	// Same function, the register spill between `mov rcx, rdi` and the wallrun
	// handler call; RCX is re-established by the trampoline tail.
	const CMemory early = Module_FindPattern(g_GameDll,
		"48 8B CF 4C 89 74 24 38 4C 89 7C 24 30 E8 ?? ?? ?? ?? 48 8B 8F C0 01 00 00 33 DB 44 0F B6 F8 84 C0 75 07");
	if (early)
		s_pEarlySite = early.Offset(0x3).RCast<uint8_t*>();
	else
		Warning(eDLL_T::SERVER, "[MB-ANIM] early site pattern unresolved -- falling back to the late order\n");
}

void VMantleBoostAnimServer::Detour(const bool bAttach) const
{
	if (!s_pSelectSite || !s_pEpilogue || !CMultiPlayerAnimState__IsOnGround)
		return;

	if (bAttach)
	{
		static const uint8_t kRestoreRcx[] = { 0x48, 0x8B, 0xCF };   // mov rcx, rdi
		const bool bEarly = bridge_mantle_boost_air_anim_early.GetBool() && s_pEarlySite;
		const bool ok = bEarly
			? s_patch.InstallReplay(s_pEarlySite, 5, kRestoreRcx, sizeof(kRestoreRcx), s_pEpilogue, &MantleBoostAnim_Select)
			: s_patch.Install(s_pSelectSite, s_pEpilogue, &MantleBoostAnim_Select);
		if (!ok)
			Warning(eDLL_T::SERVER, "[MB-ANIM] CalcMainActivity site patch failed at %p\n", bEarly ? s_pEarlySite : s_pSelectSite);
		else
			Msg(eDLL_T::SERVER, "[MB-ANIM] boost-air activity selector installed (%s site %p, trampoline %p)\n",
				bEarly ? "early" : "late-order", bEarly ? s_pEarlySite : s_pSelectSite, s_patch.GetTrampoline());
	}
	else
	{
		s_patch.Remove();
	}
}
