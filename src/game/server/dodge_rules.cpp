//=============================================================================//
//
// Purpose: server half of the airborne dodge rules (game/shared/dodge_rules.h)
// plus the crouch/slide dodge: S3's Jump refuses any dodge while ducked.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "public/const.h"
#include "game/shared/dodge_rules.h"
#include "game/server/player.h"
#include "game/server/dodge_rules.h"

static ConVar bridge_dodge_rules("bridge_dodge_rules", "1", FCVAR_RELEASE,
	"Airborne dodge rules: once per airtime, velocity direction without input, "
	"full keep against velocity, dodgeVerticalHeight floor. Must match the client.");
static ConVar bridge_dodge_diag("bridge_dodge_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[DODGE] log every airborne jump/dodge press of a dodge-enabled player and why it did or did not dash.");

static char (*v_CGameMovement__Jump)(void* ctx) = nullptr;

static const DodgeRulesLayout_t s_layout =
{
	0x5F08, // class settings
	0x6948, // m_lastDodgeTime
	0x6214, // m_flTimeLastTouchedGround
	0x6128, // player flags
	0x2C,   // m_nButtonsPressed
	0x78,   // move direction
	0x130,  // velocity
};

static DodgeRulesFieldPtrs_t s_fieldPtrs = {};
static bool s_bFieldsLive = false;

// Jump's standing-only test: test eax(duckState),eax / jnz <return 0>.
static CMemory s_duckGate;
static constexpr ptrdiff_t DUCK_GATE_JNZ = 0x2;
static uint8_t s_duckGateOrig[6] = {};

static constexpr ptrdiff_t PLAYER_OFF_DUCKSTATE = 0x65F0;
static constexpr ptrdiff_t PLAYER_OFF_SUITPOWER = 0x5AC8;

static void DodgeRules_Diag(const uintptr_t ctx, const DodgeRulesCall_t& st, const bool bFired)
{
	const uintptr_t player = *reinterpret_cast<uintptr_t*>(ctx + DODGE_CTX_OFF_PLAYER);
	const uintptr_t mv = *reinterpret_cast<uintptr_t*>(ctx + DODGE_CTX_OFF_MOVEDATA);
	const float* const md = st.vecMoveDir0;
	Msg(eDLL_T::SERVER, "[DODGE] srv fired=%d blocked=%d velDir=%d pressed=%08X duck=%d pflags=%08X "
		"power=%.2f lastDodge=%.3f touched=%.3f moveDir=(%.2f %.2f %.2f) vel=(%.0f %.0f %.0f)\n",
		bFired, st.bBlocked, st.bDirFromVelocity,
		*reinterpret_cast<const uint32_t*>(mv + s_layout.mvButtonsPressed),
		*reinterpret_cast<const int*>(player + PLAYER_OFF_DUCKSTATE),
		*reinterpret_cast<const uint32_t*>(player + s_layout.plrPlayerFlags),
		*reinterpret_cast<const float*>(player + PLAYER_OFF_SUITPOWER),
		st.flLastDodge0, *reinterpret_cast<const float*>(player + s_layout.plrLastTouchedGround),
		md[0], md[1], md[2], st.vecVel0[0], st.vecVel0[1], st.vecVel0[2]);
}

static char __fastcall Hook_CGameMovement_Jump(void* ctx)
{
	const uintptr_t c = reinterpret_cast<uintptr_t>(ctx);
	CPlayer* const player = *reinterpret_cast<CPlayer**>(c + DODGE_CTX_OFF_PLAYER);
	DodgeRulesFields_t fields;
	if (!player || !bridge_dodge_rules.GetBool() || !DodgeRules_LoadFields(s_fieldPtrs, fields))
		return v_CGameMovement__Jump(ctx);

	const bool bAirborne = (player->GetFlags() & FL_ONGROUND) == 0;
	if (!s_bFieldsLive)
	{
		s_bFieldsLive = true;
		Msg(eDLL_T::SERVER, "[DODGE] settings fields live (dodge=%X speed=%X keep=%X vert=%X)\n",
			fields.nDodge, fields.nDodgeSpeed, fields.nKeepSpeedFrac, fields.nVerticalHeight);
	}

	DodgeRulesCall_t st;
	DodgeRules_Pre(c, bAirborne, true, s_layout, fields, st);

	const char result = v_CGameMovement__Jump(ctx);

	const bool bFired = DodgeRules_Post(c, s_layout, fields, st);
	if (st.bActive && bridge_dodge_diag.GetBool())
		DodgeRules_Diag(c, st, bFired);
	return result;
}

static const uint32_t* DodgeRules_Field(const CMemory insn)
{
	return insn ? insn.ResolveRelativeAddress(2, 6).RCast<const uint32_t*>() : nullptr;
}

void VDodgeRules::GetAdr(void) const
{
	LogFunAdr("CGameMovement::Jump", v_CGameMovement__Jump);
	LogVarAdr("DodgeRules::DuckGate", s_duckGate.RCast<void*>());
}

void VDodgeRules::GetFun(void) const
{
	Module_FindPattern(g_GameDll, "40 55 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 51")
		.GetPtr(v_CGameMovement__Jump);
	if (!v_CGameMovement__Jump)
	{
		Warning(eDLL_T::SERVER, "[DODGE] CGameMovement::Jump unresolved -- dodge rules disabled\n");
		return;
	}

	const CMemory jump(reinterpret_cast<uintptr_t>(v_CGameMovement__Jump));
	static const int s_nRange = 0x2000;

	// test eax,eax / jnz / mov eax,[r10+6128h] (player flags) / ... / mov eax,stf_dodge
	s_duckGate = jump.FindPattern(
		"85 C0 0F 85 ?? ?? ?? ?? ?? 8B ?? 28 61 00 00 C1 E8 04 A8 01 0F 85 ?? ?? ?? ?? ?? 8B ?? 08 5F 00 00 8B 05",
		CMemory::Direction::DOWN, s_nRange, 1);
	s_fieldPtrs.pDodge = s_duckGate ? DodgeRules_Field(s_duckGate.Offset(0x21)) : nullptr;

	const CMemory vert = jump.FindPattern("84 C0 8B 05 ?? ?? ?? ?? 75 06 8B 05 ?? ?? ?? ?? 44 8B 05",
		CMemory::Direction::DOWN, s_nRange, 1);
	const CMemory speed = jump.FindPattern("EB 4E 8B 15 ?? ?? ?? ?? 4C 8D 4C 24",
		CMemory::Direction::DOWN, s_nRange, 1);
	const CMemory keep = jump.FindPattern("45 84 ED 74 1B 8B 0D ?? ?? ?? ?? 41 0F 28 C5",
		CMemory::Direction::DOWN, s_nRange, 1);

	s_fieldPtrs.pVerticalHeight = vert ? DodgeRules_Field(vert.Offset(0x2)) : nullptr;
	s_fieldPtrs.pDodgeSpeed = speed ? DodgeRules_Field(speed.Offset(0x2)) : nullptr;
	s_fieldPtrs.pKeepSpeedFrac = keep ? DodgeRules_Field(keep.Offset(0x5)) : nullptr;

	if (!s_duckGate || !DodgeRules_PtrsValid(s_fieldPtrs))
		Warning(eDLL_T::SERVER, "[DODGE] Jump interior unresolved (duck=%p dodge=%p speed=%p keep=%p vert=%p) -- "
			"dodge rules disabled\n", s_duckGate.RCast<void*>(), s_fieldPtrs.pDodge, s_fieldPtrs.pDodgeSpeed,
			s_fieldPtrs.pKeepSpeedFrac, s_fieldPtrs.pVerticalHeight);
}

void VDodgeRules::Detour(const bool bAttach) const
{
	if (!v_CGameMovement__Jump)
		return;
	DetourSetup(&v_CGameMovement__Jump, &Hook_CGameMovement_Jump, bAttach);

	if (!s_duckGate)
		return;
	const CMemory jnz = s_duckGate.Offset(DUCK_GATE_JNZ);
	if (bAttach)
	{
		memcpy(s_duckGateOrig, jnz.RCast<const void*>(), sizeof(s_duckGateOrig));
		jnz.Patch({ 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 });
		Msg(eDLL_T::SERVER, "[DODGE] rules attached (fields %s), ducked dodge allowed\n",
			DodgeRules_PtrsValid(s_fieldPtrs) ? "resolved" : "UNRESOLVED");
	}
	else if (s_duckGateOrig[0])
	{
		jnz.Patch(vector<uint8_t>(s_duckGateOrig, s_duckGateOrig + sizeof(s_duckGateOrig)));
	}
}
