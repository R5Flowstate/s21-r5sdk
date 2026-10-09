//=============================================================================//
//
// Purpose: auto bunny hop on the dedicated server. While sv_autobunnyhopping
// is set, a grounded command with the jump held is marked pressed before the
// move runs, so the first command on the ground jumps, as in CS:GO.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "public/game/shared/in_buttons.h"
#include "autobunnyhop.h"

// Replicated, not CHEAT: the bridge client refuses replicated cheat convars.
static ConVar sv_autobunnyhopping("sv_autobunnyhopping", "0", FCVAR_RELEASE | FCVAR_REPLICATED,
	"Players automatically re-jump while holding the jump button.");

static constexpr ptrdiff_t ABH_CTX_OFF_PLAYER    = 0x08;
static constexpr ptrdiff_t ABH_CTX_OFF_MV        = 0x10;
static constexpr ptrdiff_t ABH_MV_OFF_BUTTONS    = 0x24;
static constexpr ptrdiff_t ABH_MV_OFF_PRESSED    = 0x2C; // buttons pressed this command; the jump check reads it
static constexpr ptrdiff_t ABH_OFF_GROUNDENT     = 0x3C4;

void AutoBunnyHop_BeforeFullWalkMove(void* ctx)
{
	if (!ctx || !sv_autobunnyhopping.GetBool())
		return;

	const uint8_t* const player = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + ABH_CTX_OFF_PLAYER);
	uint8_t* const mv = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + ABH_CTX_OFF_MV);
	if (!player || !mv || *reinterpret_cast<const int*>(player + ABH_OFF_GROUNDENT) == -1)
		return;

	if (*reinterpret_cast<const int*>(mv + ABH_MV_OFF_BUTTONS) & IN_JUMP)
		*reinterpret_cast<int*>(mv + ABH_MV_OFF_PRESSED) |= IN_JUMP;
}
