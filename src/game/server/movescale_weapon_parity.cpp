//=============================================================================//
//
// Purpose: offhand fire-mode must not swap the second move_speed_modifier lookup.
// Dedi uses modifier(w)*modifier(w) to match the S21 client's product.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "movescale_weapon_parity.h"

static ConVar bridge_movescale_weapon_parity("bridge_movescale_weapon_parity", "1", FCVAR_RELEASE,
	"Make the move-scale weapon term use one weapon for both modifier lookups, "
	"as the client does. 0 = keep this engine's latest-primary swap.");

void VMoveScaleWeaponParity::Detour(const bool bAttach) const
{
	if (!bAttach || !bridge_movescale_weapon_parity.GetBool())
		return;

	// Inside SetupMove, at the offhand-fire-mode branch. The 50 27 00 00 is the
	// weapon's fire_mode offset and stays literal -- it is what pins the
	// server-half twin of this function. unique.
	const CMemory site = Module_FindPattern(g_GameDll,
		"8B 86 50 27 00 00 FF C8 83 F8 04 77 ? 48 8B CE E8 ? ? ? ? "
		"33 D2 48 8B CF 0F 28 F0 E8 ? ? ? ? 48 8B F0");

	if (!site)
	{
		Warning(eDLL_T::SERVER,
			"[MOVESCALE-PARITY] SetupMove weapon-term pattern unresolved -- dedi keeps "
			"the latest-primary swap and will out-run the client's prediction\n");
		return;
	}

	// +0x1D is `call <latest primary>` followed by `mov rsi, rax`. Blanking both
	// leaves rsi on the already-tested weapon; xmm6 still holds its modifier
	// from the call two instructions earlier.
	site.Offset(0x1D).Patch({ 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 });

	Msg(eDLL_T::SERVER, "[MOVESCALE-PARITY] weapon term now matches the client\n");
}
