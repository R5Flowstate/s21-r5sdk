//=============================================================================//
//
// Purpose: client half of the movement-ability input routing
// (game/shared/jump_input_gate.h). The airborne Jump actions, the jetpack and
// glide engage tests and the jump-toggle offhand select / hold / release read
// IN_DODGE instead of IN_JUMP. Server twin: game/server/movement_ability_input.cpp
// plus the constants in offhand_jump_toggle.cpp, jetpack.cpp and glide.cpp.
//
//=============================================================================//
#include "core/stdafx.h"
#include "public/game/shared/in_buttons.h"
#include "game/shared/jump_input_gate.h"
#include "game/shared/titan_gate.h"
#include "game/client/pred_authority.h"
#include "game/client/movement_ability_input.h"

static const JumpGateLayout_t s_layout =
{
	0x2598, // class settings
	0x3670, // m_flTimeLastJumped
	0x2C,   // m_nButtonsPressed
};

static CMemory s_jumpSite;
static CMemory s_jumpBail;
static const uint32_t* s_pDoubleJumpField = nullptr;
static CJumpInputGatePatch s_jumpGate;

static constexpr uint8_t JUMP_CTX_REG_RBX = 3;

static int __fastcall MovementAbility_JumpDecide(uintptr_t ctx, int bGround)
{
	const void* const player = *reinterpret_cast<const void* const*>(ctx + JUMPGATE_CTX_OFF_PLAYER);
	if (TitanGate_IsTitanPlayer(player))
		return JUMPGATE_REPLAY;
	return JumpGate_Decide(ctx, bGround != 0, s_layout, s_pDoubleJumpField, PredNative_CurTime());
}

// `test byte ptr [reg+disp32], IN_JUMP` on m_afButtonPressed (0x2A04) or m_nButtons
// (0x2A0C). IN_DODGE is bit 4 of the same field's top byte.
struct ButtonTestSite_t
{
	const char* pszName;
	ptrdiff_t nOffset;
};

// Same order as the patterns in GetFun.
static const ButtonTestSite_t s_buttonSites[] =
{
	{ "jetpack engage",       0x5 },
	{ "glide engage",         0x5 },
	{ "offhand toggle press", 0x6 },
	{ "offhand hold release", 0x11 },
	{ "busy hold",            0x4 },
	{ "busy charge press",    0x9 },
	{ "offhand select",       0xD },
};

static constexpr size_t BUTTON_SITE_COUNT = sizeof(s_buttonSites) / sizeof(s_buttonSites[0]);
static constexpr size_t BUTTON_TEST_LEN = 7;
static CMemory s_buttonSite[BUTTON_SITE_COUNT];
static uint8_t s_buttonOrig[BUTTON_SITE_COUNT][BUTTON_TEST_LEN] = {};

static bool ButtonTest_IsJump(const uint8_t* const p)
{
	int32_t disp;
	memcpy(&disp, p + 2, sizeof(disp));
	return p[0] == 0xF6 && (p[1] & 0xF8) == 0x80 && (disp == 0x2A04 || disp == 0x2A0C) && p[6] == IN_JUMP;
}

void VMovementAbilityInput::GetAdr(void) const
{
	LogVarAdr("MovementAbility::JumpSite", s_jumpSite.RCast<void*>());
	LogVarAdr("MovementAbility::JumpBail", s_jumpBail.RCast<void*>());
	LogVarAdr("MovementAbility::DoubleJumpField", s_pDoubleJumpField);
	for (size_t i = 0; i < BUTTON_SITE_COUNT; ++i)
		LogVarAdr(s_buttonSites[i].pszName, s_buttonSite[i].RCast<void*>());
}

void VMovementAbilityInput::GetFun(void) const
{
	// Jump: test r15b(can ground jump) / jnz / test r14b(dodge press) / jnz, then the
	// double-jump setting read that picks double jump over dodge.
	s_jumpSite = Module_FindPattern(g_GameDll,
		"45 84 FF 75 ?? 45 84 F6 75 ?? 48 8B 43 08 8B 0D ?? ?? ?? ?? 48 8B 80 98 25 00 00");
	if (s_jumpSite && CJumpInputGatePatch::IsSite(s_jumpSite.RCast<const uint8_t*>()))
	{
		const uint8_t* const pSite = s_jumpSite.RCast<const uint8_t*>();
		const CMemory skip(reinterpret_cast<uintptr_t>(pSite + 5 + static_cast<int8_t>(pSite[4])));
		// skip: ... test dil,dil / jz <xor al,al; return>
		if (skip.CheckOpCodes({ 0x4C, 0x8B, 0x53, 0x08, 0x41, 0x83, 0xBA, 0x70, 0x2A }))
			s_jumpBail = skip.Offset(0x11).ResolveRelativeAddress(2, 6);
		s_pDoubleJumpField = s_jumpSite.Offset(0xE).ResolveRelativeAddress(2, 6).RCast<const uint32_t*>();
	}

	const CMemory update = Module_FindPattern(g_GameDll,
		"83 78 64 00 74 0B F6 86 04 2A 00 00 02 74 ?? EB ?? F6 86 0C 2A 00 00 02 75");
	const CMemory found[BUTTON_SITE_COUNT] =
	{
		Module_FindPattern(g_GameDll, "0F 2F C1 73 ?? F6 87 04 2A 00 00 02 74 ?? F3 0F 5C 97"),
		Module_FindPattern(g_GameDll, "0F 2F EA 73 ?? F6 87 04 2A 00 00 02 75 ?? 8B 05"),
		update,
		update,
		Module_FindPattern(g_GameDll, "85 F6 74 09 F6 87 0C 2A 00 00 02 75 ?? 80 BB BF 1C 00 00 00"),
		Module_FindPattern(g_GameDll, "83 BB A4 15 00 00 05 75 ?? F6 87 04 2A 00 00 02 74"),
		Module_FindPattern(g_GameDll, "80 BE C0 1C 00 00 00 0F 84 ?? ?? ?? ?? F6 83 0C 2A 00 00 02 0F 84"),
	};

	for (size_t i = 0; i < BUTTON_SITE_COUNT; ++i)
	{
		const CMemory site = found[i];
		if (site && ButtonTest_IsJump(site.Offset(s_buttonSites[i].nOffset).RCast<const uint8_t*>()))
			s_buttonSite[i] = site.Offset(s_buttonSites[i].nOffset);
		else
			Warning(eDLL_T::CLIENT, "[MOVE-ABILITY] %s button test unresolved -- it stays on jump\n",
				s_buttonSites[i].pszName);
	}

	if (!s_jumpSite || !s_jumpBail || !s_pDoubleJumpField)
		Warning(eDLL_T::CLIENT, "[MOVE-ABILITY] Jump gate unresolved (site=%p bail=%p field=%p) -- "
			"airborne jump actions stay on jump\n", s_jumpSite.RCast<void*>(), s_jumpBail.RCast<void*>(),
			s_pDoubleJumpField);
}

void VMovementAbilityInput::Detour(const bool bAttach) const
{
	if (!bAttach)
	{
		s_jumpGate.Remove();
		for (size_t i = 0; i < BUTTON_SITE_COUNT; ++i)
		{
			if (s_buttonSite[i] && s_buttonOrig[i][0])
				s_buttonSite[i].Patch(vector<uint8_t>(s_buttonOrig[i], s_buttonOrig[i] + BUTTON_TEST_LEN));
		}
		return;
	}

	int nPatched = 0;
	for (size_t i = 0; i < BUTTON_SITE_COUNT; ++i)
	{
		if (!s_buttonSite[i])
			continue;
		uint8_t* const p = s_buttonSite[i].RCast<uint8_t*>();
		memcpy(s_buttonOrig[i], p, BUTTON_TEST_LEN);

		int32_t disp;
		memcpy(&disp, p + 2, sizeof(disp));
		disp += 3;
		uint8_t insn[BUTTON_TEST_LEN] = { p[0], p[1] };
		memcpy(insn + 2, &disp, sizeof(disp));
		insn[6] = static_cast<uint8_t>(IN_DODGE >> 24);
		s_buttonSite[i].Patch(vector<uint8_t>(insn, insn + BUTTON_TEST_LEN));
		++nPatched;
	}

	const bool bGate = s_jumpSite && s_jumpBail && s_pDoubleJumpField
		&& s_jumpGate.Install(s_jumpSite.RCast<uint8_t*>(), s_jumpBail.RCast<const uint8_t*>(),
			JUMP_CTX_REG_RBX, &MovementAbility_JumpDecide);

	if (bGate && nPatched == static_cast<int>(BUTTON_SITE_COUNT))
		Msg(eDLL_T::CLIENT, "[MOVE-ABILITY] attached: Jump gate + %d button tests on IN_DODGE\n", nPatched);
	else
		Warning(eDLL_T::CLIENT, "[MOVE-ABILITY] partial: Jump gate=%d, %d/%zu button tests on IN_DODGE\n",
			bGate ? 1 : 0, nPatched, BUTTON_SITE_COUNT);
}
