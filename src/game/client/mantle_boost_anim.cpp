//=============================================================================//
//
// Purpose: ACT_MP_MANTLE_BOOST_AIR selection in C_MultiPlayerAnimState.
//
// The activity chain in CalcMainActivity predates mantle boost, so the check
// is grafted at the point later builds evaluate it: after the armored-leap
// states and before the moving/float fallback. State 4 while the animstate
// reports airborne selects the activity for that player's third-person model;
// the local player reads the predicted FSM, everyone else the replicated value.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "engine/client/net_bridge_internal.h"
#include "game/client/mantle_boost.h"
#include "game/client/mantle_boost_anim.h"
#include "game/shared/activity.h"
#include "game/shared/midfunc_jump_patch.h"

static ConVar bridge_mantle_boost_air_anim("bridge_mantle_boost_air_anim", "1", FCVAR_RELEASE,
	"Select ACT_MP_MANTLE_BOOST_AIR while a mantle boost is airborne (3p).");
static ConVar bridge_mantle_boost_air_anim_log("bridge_mantle_boost_air_anim_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log [MB-ANIM] activity selections.");
static ConVar bridge_mantle_boost_air_anim_early("bridge_mantle_boost_air_anim_early", "1", FCVAR_RELEASE,
	"1 = select the boost-air activity ahead of the jump/slide handlers (plays on every boost); 0 = after them (late order: only once the slide flag clears).");

static constexpr ptrdiff_t ANIMSTATE_OFF_PLAYER = 0x1D0;   // C_MultiPlayerAnimState::m_player
static constexpr ptrdiff_t ENT_OFF_REFEHANDLE   = 0x8;     // C_BaseEntity::m_RefEHandle, low 16 bits = entity index
static constexpr int       kMaxWireEdicts       = 128;     // 7-bit edict in the packed wire value

static uint8_t* s_pSelectSite  = nullptr;   // late order: after the late special states
static uint8_t* s_pEarlySite   = nullptr;   // ahead of the jump/crouch/zipline/land/slide chain
static uint8_t* s_pEpilogue    = nullptr;
static CMidFuncJumpPatch s_patch;

static int s_wireState[kMaxWireEdicts];

void MantleBoostAnim_OnWireState(const int nEdict, const int nState)
{
	if (nEdict <= 0 || nEdict >= kMaxWireEdicts)
		return;
	s_wireState[nEdict] = nState;
}

void MantleBoostAnim_OnSessionReset(void)
{
	memset(s_wireState, 0, sizeof(s_wireState));
}

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
			Warning(eDLL_T::CLIENT, "[MB-ANIM] ACT_MP_MANTLE_BOOST_AIR is not registered -- boost-air activity disabled\n");
	}
	return s_nId;
}

static int MantleBoostAnim_StateFor(const uintptr_t pPlayer)
{
	if (pPlayer == MantleBoostClient_GetPredictedPlayer())
		return MantleBoostClient_GetState();

	const int nIndex = *reinterpret_cast<const uint32_t*>(pPlayer + ENT_OFF_REFEHANDLE) & 0xFFFF;
	if (nIndex <= 0 || nIndex >= kMaxWireEdicts)
		return 0;

	const uintptr_t pTable = NetObs_EntityHandleTableAddr();
	if (!pTable || *reinterpret_cast<const uintptr_t*>(pTable + 32ull * nIndex) != pPlayer)
		return 0;

	return s_wireState[nIndex];
}

static int __fastcall MantleBoostAnim_Select(void* const pAnimState)
{
	if (!bridge_mantle_boost_air_anim.GetBool() || !pAnimState || !C_MultiPlayerAnimState__IsOnGround)
		return -1;

	const uintptr_t pPlayer = *reinterpret_cast<const uintptr_t*>(
		reinterpret_cast<uintptr_t>(pAnimState) + ANIMSTATE_OFF_PLAYER);
	if (!pPlayer)
		return -1;

	if (MantleBoostAnim_StateFor(pPlayer) != 4 || C_MultiPlayerAnimState__IsOnGround(pAnimState))
		return -1;

	const int nActivity = MantleBoostAnim_ActivityId();
	if (nActivity < 0)
		return -1;

	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		Warning(eDLL_T::CLIENT, "[MB-ANIM] first ACT_MP_MANTLE_BOOST_AIR selection (id %d, player %p)\n",
			nActivity, reinterpret_cast<void*>(pPlayer));
	}
	if (bridge_mantle_boost_air_anim_log.GetBool())
		Msg(eDLL_T::CLIENT, "[MB-ANIM] player %p -> ACT_MP_MANTLE_BOOST_AIR (%d)\n",
			reinterpret_cast<void*>(pPlayer), nActivity);

	return nActivity;
}

void VMantleBoostAnimClient::GetAdr(void) const
{
	LogFunAdr("C_MultiPlayerAnimState::IsOnGround", C_MultiPlayerAnimState__IsOnGround);
	LogVarAdr("C_MultiPlayerAnimState::CalcMainActivity boost-air site", s_pSelectSite);
}

void VMantleBoostAnimClient::GetFun(void) const
{
	// CalcMainActivity, tail of the armored-leap switch: `mov eax, ACT ; jmp epilogue`
	// then `test r12b, r12b ; jnz` -- the jump-anim/crouch flag tests that start the
	// moving/float fallback. The site is the test, the epilogue is the jmp target,
	// IsOnGround is the first call after the fallback tests.
	const CMemory base = Module_FindPattern(g_GameDll,
		"B8 ?? ?? 00 00 E9 ?? ?? ?? ?? 45 84 E4 75 1D 45 84 FF 75 1D 84 DB 75 33 45 84 F6 75 2E 48 8D 54 24 60 48 8B CF E8");
	if (!base)
	{
		Warning(eDLL_T::CLIENT, "[MB-ANIM] CalcMainActivity site pattern unresolved -- boost-air activity disabled\n");
		return;
	}

	s_pEpilogue   = base.Offset(0x5).FollowNearCall().RCast<uint8_t*>();
	s_pSelectSite = base.Offset(0xA).RCast<uint8_t*>();
	base.Offset(0x34).FollowNearCall().GetPtr(C_MultiPlayerAnimState__IsOnGround);

	// Same function, right after the wallrun handler's `jnz`: `mov rax, [rdi+1D0h]`
	// opens the jump/crouch/zipline/land/slide chain. Position-independent, so the
	// displaced 7 bytes replay verbatim in the trampoline.
	const CMemory early = Module_FindPattern(g_GameDll,
		"0F 85 ?? ?? ?? ?? 48 8B 87 D0 01 00 00 33 F6 40 38 B0 AC 1F 00 00 74 ?? 48 8B CF 40 38 B0 B4 1F 00 00");
	if (early)
		s_pEarlySite = early.Offset(0x6).RCast<uint8_t*>();
	else
		Warning(eDLL_T::CLIENT, "[MB-ANIM] early site pattern unresolved -- falling back to the late order\n");
}

void VMantleBoostAnimClient::Detour(const bool bAttach) const
{
	if (!s_pSelectSite || !s_pEpilogue || !C_MultiPlayerAnimState__IsOnGround)
		return;

	if (bAttach)
	{
		const bool bEarly = bridge_mantle_boost_air_anim_early.GetBool() && s_pEarlySite;
		const bool ok = bEarly
			? s_patch.InstallReplay(s_pEarlySite, 7, nullptr, 0, s_pEpilogue, &MantleBoostAnim_Select)
			: s_patch.Install(s_pSelectSite, s_pEpilogue, &MantleBoostAnim_Select);
		if (!ok)
			Warning(eDLL_T::CLIENT, "[MB-ANIM] CalcMainActivity site patch failed at %p\n", bEarly ? s_pEarlySite : s_pSelectSite);
		else
			Msg(eDLL_T::CLIENT, "[MB-ANIM] boost-air activity selector installed (%s site %p, trampoline %p)\n",
				bEarly ? "early" : "late-order", bEarly ? s_pEarlySite : s_pSelectSite, s_patch.GetTrampoline());
	}
	else
	{
		s_patch.Remove();
	}
}
