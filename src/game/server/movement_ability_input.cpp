//=============================================================================//
//
// Purpose: server half of the movement-ability input routing at the S3 Jump
// (game/shared/jump_input_gate.h). Client twin: game/client/movement_ability_input.cpp.
// The offhand, jetpack and glide halves read IN_DODGE in their own files.
//
//=============================================================================//
#include "core/stdafx.h"
#include "game/server/gameinterface.h"
#include "game/shared/jump_input_gate.h"
#include "game/shared/titan_gate.h"
#include "game/server/movement_ability_input.h"

static const JumpGateLayout_t s_layout =
{
	0x5F08, // class settings
	0x6218, // m_flTimeLastJumped
	0x2C,   // m_nButtonsPressed
};

static CMemory s_jumpSite;
static CMemory s_jumpBail;
static const uint32_t* s_pDoubleJumpField = nullptr;
static CJumpInputGatePatch s_jumpGate;

static constexpr uint8_t JUMP_CTX_REG_RDI = 7;

static int __fastcall MovementAbility_JumpDecide(uintptr_t ctx, int bGround)
{
	const void* const player = *reinterpret_cast<const void* const*>(ctx + JUMPGATE_CTX_OFF_PLAYER);
	if (TitanGate_IsTitanPlayer(player))
		return JUMPGATE_REPLAY;
	return JumpGate_Decide(ctx, bGround != 0, s_layout, s_pDoubleJumpField, gpGlobals ? gpGlobals->curTime : 0.0f);
}

void VMovementAbilityInput::GetAdr(void) const
{
	LogVarAdr("MovementAbility::JumpSite", s_jumpSite.RCast<void*>());
	LogVarAdr("MovementAbility::JumpBail", s_jumpBail.RCast<void*>());
	LogVarAdr("MovementAbility::DoubleJumpField", s_pDoubleJumpField);
}

void VMovementAbilityInput::GetFun(void) const
{
	// Jump: test r15b(can ground jump) / jnz / test r13b(dodge press) / jnz, then the
	// double-jump setting read that picks double jump over dodge.
	s_jumpSite = Module_FindPattern(g_GameDll,
		"45 84 FF 75 ?? 45 84 ED 75 ?? 48 8B 47 08 8B 0D ?? ?? ?? ?? 48 8B 80 08 5F 00 00");
	if (s_jumpSite && CJumpInputGatePatch::IsSite(s_jumpSite.RCast<const uint8_t*>()))
	{
		const uint8_t* const pSite = s_jumpSite.RCast<const uint8_t*>();
		const CMemory skip(reinterpret_cast<uintptr_t>(pSite + 5 + static_cast<int8_t>(pSite[4])));
		// skip: ... test sil,sil / jnz / xor al,al / jmp <return>
		const CMemory bail = skip.Offset(0x15);
		if (skip.CheckOpCodes({ 0x4C, 0x8B, 0x57, 0x08, 0x41, 0x8B, 0x82, 0xF0, 0x65 })
			&& bail.CheckOpCodes({ 0x32, 0xC0, 0xE9 }))
			s_jumpBail = bail;
		s_pDoubleJumpField = s_jumpSite.Offset(0xE).ResolveRelativeAddress(2, 6).RCast<const uint32_t*>();
	}

	if (!s_jumpSite || !s_jumpBail || !s_pDoubleJumpField)
		Warning(eDLL_T::SERVER, "[MOVE-ABILITY] Jump gate unresolved (site=%p bail=%p field=%p) -- "
			"airborne jump actions stay on jump\n", s_jumpSite.RCast<void*>(), s_jumpBail.RCast<void*>(),
			s_pDoubleJumpField);
}

void VMovementAbilityInput::Detour(const bool bAttach) const
{
	if (!bAttach)
	{
		s_jumpGate.Remove();
		return;
	}
	if (!s_jumpSite || !s_jumpBail || !s_pDoubleJumpField)
		return;

	if (s_jumpGate.Install(s_jumpSite.RCast<uint8_t*>(), s_jumpBail.RCast<const uint8_t*>(),
		JUMP_CTX_REG_RDI, &MovementAbility_JumpDecide))
		Msg(eDLL_T::SERVER, "[MOVE-ABILITY] Jump gate attached\n");
	else
		Warning(eDLL_T::SERVER, "[MOVE-ABILITY] Jump gate install failed -- airborne jump actions stay on jump\n");
}
