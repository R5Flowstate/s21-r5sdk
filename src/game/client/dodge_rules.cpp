//=============================================================================//
//
// Purpose: client half of the airborne dodge rules (game/shared/dodge_rules.h)
// plus the crouch/slide dodge: S21's Jump refuses any dodge while ducked.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "public/const.h"
#include "game/shared/dodge_rules.h"
#include "game/shared/titan_gate.h"
#include "game/client/dodge_rules.h"

static ConVar bridge_dodge_rules("bridge_dodge_rules", "1", FCVAR_RELEASE,
	"Airborne dodge rules: once per airtime, velocity direction without input, "
	"full keep against velocity, dodgeVerticalHeight floor. Must match the server.");
static ConVar bridge_dodge_diag("bridge_dodge_diag", "0", FCVAR_DEVELOPMENTONLY,
	"[DODGE] log every predicted airborne jump/dodge press and why it did or did not dash.");

static char (*v_C_GameMovement__Jump)(void* ctx) = nullptr;

static const DodgeRulesLayout_t s_layout =
{
	0x2598, // class settings
	0x3640, // m_lastDodgeTime
	0x366C, // m_flTimeLastTouchedGround
	0x3268, // player flags
	0x2C,   // m_nButtonsPressed
	0x78,   // move direction
	0x124,  // velocity
};

static DodgeRulesFieldPtrs_t s_fieldPtrs = {};
static bool s_bFieldsLive = false;

// Jump's standing-only test: cmp [player+2A70h](duckState),0 / jnz <return 0>.
static CMemory s_duckGate;
static constexpr ptrdiff_t DUCK_GATE_JNZ = 0x7;
static uint8_t s_duckGateOrig[6] = {};

static constexpr ptrdiff_t PLAYER_OFF_FFLAGS    = 0xC8;
static constexpr ptrdiff_t PLAYER_OFF_DUCKSTATE = 0x2A70;
static constexpr ptrdiff_t PLAYER_OFF_SUITPOWER = 0x1DD8;

// Jump's early "return 0" inputs that precede the dodge branch.
static constexpr ptrdiff_t PLAYER_OFF_GATE_HANDLE  = 0x148;  // handle; any live entity refuses
static constexpr ptrdiff_t PLAYER_OFF_GATE_ONESHOT = 0x1DC4; // byte; consumed by the next Jump
static constexpr ptrdiff_t PLAYER_OFF_GATE_BLOCK   = 0x252C; // byte
static constexpr ptrdiff_t PLAYER_OFF_GATE_STATE   = 0x4838; // dword; nonzero refuses
static constexpr ptrdiff_t PLAYER_OFF_GATE_MODE    = 0x19B0; // dword; 13 refuses

static void DodgeRules_Diag(const uintptr_t ctx, const DodgeRulesCall_t& st, const bool bFired,
	const bool bClassDodge, const uint32_t nPressed)
{
	const uintptr_t player = *reinterpret_cast<uintptr_t*>(ctx + DODGE_CTX_OFF_PLAYER);
	const uintptr_t mv = *reinterpret_cast<uintptr_t*>(ctx + DODGE_CTX_OFF_MOVEDATA);
	const float* const md = st.vecMoveDir0;
	Msg(eDLL_T::CLIENT, "[DODGE] cl fired=%d classDodge=%d blocked=%d velDir=%d pressed=%08X duck=%d pflags=%08X "
		"power=%.2f lastDodge=%.3f touched=%.3f moveDir=(%.2f %.2f %.2f) vel=(%.0f %.0f %.0f) "
		"early(h=%08X once=%d blk=%d st=%d mode=%d mv0=%08X)\n",
		bFired, bClassDodge, st.bBlocked, st.bDirFromVelocity, nPressed,
		*reinterpret_cast<const int*>(player + PLAYER_OFF_DUCKSTATE),
		*reinterpret_cast<const uint32_t*>(player + s_layout.plrPlayerFlags),
		*reinterpret_cast<const float*>(player + PLAYER_OFF_SUITPOWER),
		st.flLastDodge0, *reinterpret_cast<const float*>(player + s_layout.plrLastTouchedGround),
		md[0], md[1], md[2], st.vecVel0[0], st.vecVel0[1], st.vecVel0[2],
		*reinterpret_cast<const uint32_t*>(player + PLAYER_OFF_GATE_HANDLE),
		*reinterpret_cast<const uint8_t*>(player + PLAYER_OFF_GATE_ONESHOT),
		*reinterpret_cast<const uint8_t*>(player + PLAYER_OFF_GATE_BLOCK),
		*reinterpret_cast<const int*>(player + PLAYER_OFF_GATE_STATE),
		*reinterpret_cast<const int*>(player + PLAYER_OFF_GATE_MODE),
		*reinterpret_cast<const uint32_t*>(mv));
}

static void DodgeRules_PatchDuckGate(const bool bAllowDucked)
{
	const CMemory jnz = s_duckGate.Offset(DUCK_GATE_JNZ);
	if (bAllowDucked)
		jnz.Patch({ 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 });
	else
		jnz.Patch(vector<uint8_t>(s_duckGateOrig, s_duckGateOrig + sizeof(s_duckGateOrig)));
}

// A titan keeps the engine's own Jump: the duck gate patched out for pilots is put back
// for the length of the call while the titan is ducked.
static char DodgeRules_TitanJump(void* const ctx, const uintptr_t player)
{
	if (!s_duckGate || !s_duckGateOrig[0]
		|| *reinterpret_cast<const int*>(player + PLAYER_OFF_DUCKSTATE) == 0)
		return v_C_GameMovement__Jump(ctx);

	DodgeRules_PatchDuckGate(false);
	const char result = v_C_GameMovement__Jump(ctx);
	DodgeRules_PatchDuckGate(true);
	return result;
}

static char __fastcall Hook_C_GameMovement_Jump(void* ctx)
{
	const uintptr_t c = reinterpret_cast<uintptr_t>(ctx);
	const uintptr_t player = *reinterpret_cast<uintptr_t*>(c + DODGE_CTX_OFF_PLAYER);
	const uintptr_t mv = *reinterpret_cast<uintptr_t*>(c + DODGE_CTX_OFF_MOVEDATA);
	if (player && TitanGate_IsTitanPlayer(reinterpret_cast<const void*>(player)))
		return DodgeRules_TitanJump(ctx, player);
	DodgeRulesFields_t fields;
	if (!player || !mv || !bridge_dodge_rules.GetBool() || !DodgeRules_LoadFields(s_fieldPtrs, fields))
		return v_C_GameMovement__Jump(ctx);

	if (!s_bFieldsLive)
	{
		s_bFieldsLive = true;
		Msg(eDLL_T::CLIENT, "[DODGE] settings fields live (dodge=%X speed=%X keep=%X vert=%X)\n",
			fields.nDodge, fields.nDodgeSpeed, fields.nKeepSpeedFrac, fields.nVerticalHeight);
	}

	const bool bAirborne = (*reinterpret_cast<const int*>(player + PLAYER_OFF_FFLAGS) & FL_ONGROUND) == 0;
	const uint32_t nPressed = *reinterpret_cast<const uint32_t*>(mv + s_layout.mvButtonsPressed);
	DodgeRulesCall_t st;
	DodgeRules_Pre(c, bAirborne, true, s_layout, fields, st);

	const char result = v_C_GameMovement__Jump(ctx);

	const bool bFired = DodgeRules_Post(c, s_layout, fields, st);
	if (bridge_dodge_diag.GetBool() && bAirborne && (nPressed & (IN_JUMP | IN_DODGE)))
	{
		const uint8_t* const pClass = *reinterpret_cast<const uint8_t* const*>(player + s_layout.plrClassSettings);
		DodgeRules_Diag(c, st, bFired, pClass && pClass[fields.nDodge], nPressed);
	}
	return result;
}

static const uint32_t* DodgeRules_Field(const CMemory insn)
{
	return insn ? insn.ResolveRelativeAddress(2, 6).RCast<const uint32_t*>() : nullptr;
}

void VDodgeRules::GetAdr(void) const
{
	LogFunAdr("C_GameMovement::Jump", v_C_GameMovement__Jump);
	LogVarAdr("DodgeRules::DuckGate", s_duckGate.RCast<void*>());
}

void VDodgeRules::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"40 55 53 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 4C 8B 41")
		.GetPtr(v_C_GameMovement__Jump);
	if (!v_C_GameMovement__Jump)
	{
		Warning(eDLL_T::CLIENT, "[DODGE] C_GameMovement::Jump unresolved -- dodge rules disabled\n");
		return;
	}

	const CMemory jump(reinterpret_cast<uintptr_t>(v_C_GameMovement__Jump));
	static const int s_nRange = 0x2000;

	// cmp duckState / jnz / mov eax,[rcx+3268h] (player flags) / ... / mov eax,stf_dodge
	s_duckGate = jump.FindPattern(
		"?? 39 ?? 70 2A 00 00 0F 85 ?? ?? ?? ?? 8B ?? 68 32 00 00 C1 E8 04 A8 01 0F 85 ?? ?? ?? ?? 48 8B ?? 98 25 00 00 8B 05",
		CMemory::Direction::DOWN, s_nRange, 1);
	s_fieldPtrs.pDodge = s_duckGate ? DodgeRules_Field(s_duckGate.Offset(0x25)) : nullptr;

	const CMemory vert = jump.FindPattern("0F 2F ?? 76 0F 8B 05 ?? ?? ?? ?? C6 85 ?? ?? ?? ?? 01 EB 0D 8B 05",
		CMemory::Direction::DOWN, s_nRange, 1);
	const CMemory speed = jump.FindPattern("EB 5A 8B 15 ?? ?? ?? ?? 4C 8D 8D",
		CMemory::Direction::DOWN, s_nRange, 1);
	const CMemory keep = jump.FindPattern("45 84 F6 74 1C 8B 05 ?? ?? ?? ?? 41 0F 28 C5",
		CMemory::Direction::DOWN, s_nRange, 1);

	s_fieldPtrs.pVerticalHeight = vert ? DodgeRules_Field(vert.Offset(0x5)) : nullptr;
	s_fieldPtrs.pDodgeSpeed = speed ? DodgeRules_Field(speed.Offset(0x2)) : nullptr;
	s_fieldPtrs.pKeepSpeedFrac = keep ? DodgeRules_Field(keep.Offset(0x5)) : nullptr;

	if (!s_duckGate || !DodgeRules_PtrsValid(s_fieldPtrs))
		Warning(eDLL_T::CLIENT, "[DODGE] Jump interior unresolved (duck=%p dodge=%p speed=%p keep=%p vert=%p) -- "
			"dodge rules disabled\n", s_duckGate.RCast<void*>(), s_fieldPtrs.pDodge, s_fieldPtrs.pDodgeSpeed,
			s_fieldPtrs.pKeepSpeedFrac, s_fieldPtrs.pVerticalHeight);
}

void VDodgeRules::Detour(const bool bAttach) const
{
	if (!v_C_GameMovement__Jump)
		return;
	DetourSetup(&v_C_GameMovement__Jump, &Hook_C_GameMovement_Jump, bAttach);

	if (!s_duckGate)
		return;
	if (bAttach)
	{
		memcpy(s_duckGateOrig, s_duckGate.Offset(DUCK_GATE_JNZ).RCast<const void*>(), sizeof(s_duckGateOrig));
		DodgeRules_PatchDuckGate(true);
		Msg(eDLL_T::CLIENT, "[DODGE] rules attached (fields %s), ducked dodge allowed\n",
			DodgeRules_PtrsValid(s_fieldPtrs) ? "resolved" : "UNRESOLVED");
	}
	else if (s_duckGateOrig[0])
	{
		DodgeRules_PatchDuckGate(false);
	}
}
