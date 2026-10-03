//=============================================================================//
//
// Purpose: client half of the titan class test (game/shared/titan_gate.h).
// C_Player::IsTitan reads the general class out of the player's class settings;
// this reads the same field without the virtual call.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "game/shared/titan_gate.h"
#include "game/client/classvar_natives.h"
#include "game/client/titan_gate.h"

static ConVar bridge_titan_gate("bridge_titan_gate", "1", FCVAR_RELEASE,
	"Pilot-only movement overrides (dodge rules, +jump to +dodge coupling, movement-ability "
	"input, glide, mantle boost, double jump) skip players in a titan class. 0 = they apply "
	"to titans too. Must match the dedi.");

static constexpr ptrdiff_t PLAYER_OFF_CLASS_SETTINGS = 0x2598;
static constexpr uint32_t  SETTINGS_OFF_CAP = 0x100000u;

// Holds the general-class field's offset into the settings block; -1 until the
// settings layout loads, so it is read per call.
static const uint32_t* s_pGeneralClassField = nullptr;

bool TitanGate_ReadIsTitanPlayer(const void* const pPlayer)
{
	if (!pPlayer || !s_pGeneralClassField)
		return false;

	const uint32_t nField = *s_pGeneralClassField;
	if (nField >= SETTINGS_OFF_CAP)
		return false;

	const uint8_t* const pClass = *reinterpret_cast<const uint8_t* const*>(
		reinterpret_cast<uintptr_t>(pPlayer) + PLAYER_OFF_CLASS_SETTINGS);
	if (!pClass)
		return false;

	return TitanGate_IsTitanClass(*reinterpret_cast<const uint32_t*>(pClass + nField));
}

bool TitanGate_IsTitanPlayer(const void* const pPlayer)
{
	return bridge_titan_gate.GetBool() && TitanGate_ReadIsTitanPlayer(pPlayer);
}

bool TitanGate_IsLocalPlayerTitan(void)
{
	return TitanGate_IsTitanPlayer(ClassVar_LocalPlayer());
}

void VTitanGate::GetAdr(void) const
{
	LogVarAdr("TitanGate::GeneralClassField", s_pGeneralClassField);
}

void VTitanGate::GetFun(void) const
{
	// C_Player::IsTitan: mov rax,[rcx+2598h] / mov edx,<general class field> /
	// mov ecx,[rdx+rax] / dec ecx / test ecx,0FFFFFFFDh / setz al.
	const CMemory isTitan = Module_FindPattern(g_GameDll,
		"48 8B 81 98 25 00 00 8B 15 ?? ?? ?? ?? 8B 0C 02 FF C9 F7 C1 FD FF FF FF 0F 94 C0 C3");
	if (isTitan)
		s_pGeneralClassField = isTitan.Offset(0x7).ResolveRelativeAddress(2, 6).RCast<const uint32_t*>();

	if (!s_pGeneralClassField)
		Warning(eDLL_T::CLIENT, "[TITAN] C_Player::IsTitan pattern unresolved -- pilot movement overrides "
			"stay active for titans\n");
}
