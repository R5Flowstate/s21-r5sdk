//=============================================================================//
//
// Purpose: couple +jump onto in_dodge when dodge is unbound, shares jump's
// key, or controller mode is on. A distinct +dodge bind splits them.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "public/const.h"
#include "game/client/classvar_natives.h"
#include "game/client/dodge_bind.h"

static ConVar bridge_dodge_jump_couple("bridge_dodge_jump_couple", "1", FCVAR_RELEASE,
	"When +dodge is unbound, shares jump's key, or controller mode is on, "
	"+jump also presses in_dodge (bit 28). 0=off.");
static ConVar bridge_dodge_jump_couple_air_only("bridge_dodge_jump_couple_air_only", "1", FCVAR_RELEASE,
	"The coupled +jump press reaches in_dodge only while the local player is airborne, "
	"so a ground jump stays a jump. 0 = couple on the ground too.");

// C_Player::m_fFlags, same slot halfduck_zip_parity reads.
static constexpr ptrdiff_t PLAYER_OFF_FFLAGS = 200;

typedef bool (*Key_CodeForBindingFn)(const char* binding, int* buttonCode, int* bindType,
	unsigned int userId, int startCount, int allowJoystick);
static Key_CodeForBindingFn Key_CodeForBinding = nullptr;
static bool (*v_ControllerModeActive)(void* pInput) = nullptr;
static void* s_pInput = nullptr;

static constexpr ptrdiff_t kKeyDownDownLoad = 0x28; // 8B 05 in_*.down[0]
static constexpr ptrdiff_t kKeyUpStateStore = 0x1D; // C7 05 in_*.state, 4
static constexpr int kMaxKeyFnScan = 200;

static uint32_t* RipDword32(const CMemory insn)
{
	if (!insn)
		return nullptr;
	const uint8_t* p = insn.RCast<const uint8_t*>();
	if (!p || (p[0] != 0x8B && p[0] != 0xC7))
		return nullptr;
	// C7 05 disp32 imm32 is 10 bytes; 8B 05/0D is 6. Len 6 on KeyUp is state-4.
	const int nLen = (p[0] == 0xC7) ? 10 : 6;
	return insn.ResolveRelativeAddress(2, nLen).RCast<uint32_t*>();
}

static CMemory FindKeyFnMatching(uint32_t* const pWant, const char* const szPat,
	const ptrdiff_t nLoadOff, const ptrdiff_t nDeltaFromState)
{
	const CModule::ModuleSections_t* text = g_GameDll.FindSectionByName(".text");
	if (!text || !text->IsSectionValid() || !pWant)
		return CMemory();

	uintptr_t p = text->m_pSectionBase;
	const uintptr_t end = p + text->m_nSectionSize;
	int nSafety = 0;
	while (p < end && nSafety++ < kMaxKeyFnScan)
	{
		CMemory found = CMemory(p).FindPattern(szPat, CMemory::Direction::DOWN,
			static_cast<int>(end - p), 1);
		if (!found)
			break;

		uint32_t* const resolved = RipDword32(found.Offset(nLoadOff));
		uint8_t* const want = reinterpret_cast<uint8_t*>(pWant) + nDeltaFromState;
		if (resolved == reinterpret_cast<uint32_t*>(want))
			return found;

		p = static_cast<uintptr_t>(found) + 1;
	}
	return CMemory();
}

static bool DodgeIsBoundToJump(void)
{
	if (s_pInput && v_ControllerModeActive && v_ControllerModeActive(s_pInput))
		return true;
	if (!Key_CodeForBinding)
		return true;

	int dodgeCode = 0;
	int dodgeType = 0;
	int jumpCode = 0;
	int jumpType = 0;
	if (!Key_CodeForBinding("dodge", &dodgeCode, &dodgeType, 0xFFFFFFFFu, 0, 0))
		return true;
	if (!Key_CodeForBinding("jump", &jumpCode, &jumpType, 0xFFFFFFFFu, 0, 0))
		return true;
	return dodgeCode == jumpCode && dodgeType == jumpType;
}

static bool LocalPlayerAirborne(void)
{
	const uintptr_t player = reinterpret_cast<uintptr_t>(ClassVar_LocalPlayer());
	if (!player)
		return false;
	return (*reinterpret_cast<const int*>(player + PLAYER_OFF_FFLAGS) & FL_ONGROUND) == 0;
}

static void Hook_IN_JumpDown(void* args)
{
	IN_JumpDown(args);
	if (!bridge_dodge_jump_couple.GetBool() || !DodgeIsBoundToJump() || !IN_DodgeDown)
		return;
	if (bridge_dodge_jump_couple_air_only.GetBool() && !LocalPlayerAirborne())
		return;
	IN_DodgeDown(args);
}

static void Hook_IN_JumpUp(void* args)
{
	IN_JumpUp(args);
	if (bridge_dodge_jump_couple.GetBool() && DodgeIsBoundToJump() && IN_DodgeUp)
		IN_DodgeUp(args);
}

void VDodgeBind::GetAdr(void) const
{
	LogFunAdr("IN_JumpDown", IN_JumpDown);
	LogFunAdr("IN_JumpUp", IN_JumpUp);
	LogFunAdr("IN_DodgeDown", IN_DodgeDown);
	LogFunAdr("IN_DodgeUp", IN_DodgeUp);
	LogFunAdr("Key_CodeForBinding", Key_CodeForBinding);
	LogFunAdr("CInput::ControllerModeActive", v_ControllerModeActive);
	LogVarAdr("g_Input", s_pInput);
}

void VDodgeBind::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 54 24 ?? "
		"57 41 54 41 55 41 56 41 57 48 83 EC ?? 45 8B E1")
		.GetPtr(Key_CodeForBinding);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 8B D9 48 8B 0D ?? ?? ?? ?? "
		"48 8B 01 FF 90 ?? ?? ?? ?? 84 C0 74 ?? 80 7B ?? ?? 74 ?? B0")
		.GetPtr(v_ControllerModeActive);

	const CMemory jumpPack = Module_FindPattern(g_GameDll,
		"F6 C1 03 75 09 80 3D ?? ?? ?? ?? 00 74 03 83 CB 02");
	const CMemory dodgePack = Module_FindPattern(g_GameDll,
		"F6 C1 03 75 09 80 3D ?? ?? ?? ?? 00 74 04 0F BA EB 1C");

	uint32_t* pJumpState = nullptr;
	uint32_t* pDodgeState = nullptr;
	if (jumpPack)
		pJumpState = RipDword32(jumpPack.Offset(-13));
	if (dodgePack)
		pDodgeState = RipDword32(dodgePack.Offset(-13));

	if (dodgePack)
	{
		const CMemory inputSite = dodgePack.FindPattern(
			"48 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? FF 90 98 00 00 00",
			CMemory::Direction::DOWN, 0x800, 1);
		if (inputSite)
			s_pInput = inputSite.Offset(7).ResolveRelativeAddress(3, 7).RCast<void*>();
	}

	static const char s_keyDownPat[] =
		"40 53 48 83 EC 20 83 79 04 00 48 8D 05 ?? ?? ?? ?? 7E 07 "
		"48 8B 81 10 04 00 00";
	static const char s_keyUpPat[] =
		"83 79 04 00 48 8D 15 ?? ?? ?? ?? 7E 07 48 8B 91 10 04 00 00 "
		"8B 01 83 F8 FF";

	FindKeyFnMatching(pJumpState, s_keyDownPat, kKeyDownDownLoad, -8).GetPtr(IN_JumpDown);
	FindKeyFnMatching(pJumpState, s_keyUpPat, kKeyUpStateStore, 0).GetPtr(IN_JumpUp);
	FindKeyFnMatching(pDodgeState, s_keyDownPat, kKeyDownDownLoad, -8).GetPtr(IN_DodgeDown);
	FindKeyFnMatching(pDodgeState, s_keyUpPat, kKeyUpStateStore, 0).GetPtr(IN_DodgeUp);

	if (!IN_JumpDown || !IN_JumpUp || !IN_DodgeDown || !IN_DodgeUp)
		Warning(eDLL_T::CLIENT, "[DODGE] jump-couple patterns unresolved "
			"(jumpDown=%p jumpUp=%p dodgeDown=%p dodgeUp=%p)\n",
			IN_JumpDown, IN_JumpUp, IN_DodgeDown, IN_DodgeUp);
	if (!Key_CodeForBinding)
		Warning(eDLL_T::CLIENT, "[DODGE] Key_CodeForBinding unresolved -- "
			"unbound +dodge still couples; a distinct bind cannot split\n");
}

void VDodgeBind::Detour(const bool bAttach) const
{
	if (IN_JumpDown && IN_DodgeDown)
		DetourSetup(&IN_JumpDown, &Hook_IN_JumpDown, bAttach);
	if (IN_JumpUp && IN_DodgeUp)
		DetourSetup(&IN_JumpUp, &Hook_IN_JumpUp, bAttach);

	if (bAttach && IN_JumpDown && IN_JumpUp && IN_DodgeDown && IN_DodgeUp)
		Msg(eDLL_T::CLIENT, "[DODGE] jump-couple attached\n");
	else if (bAttach)
		Warning(eDLL_T::CLIENT, "[DODGE] jump-couple disabled -- patterns unresolved\n");
}
