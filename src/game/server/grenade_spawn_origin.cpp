//=============================================================================//
//
// Purpose: thrown-grenade spawn origin. See grenade_spawn_origin.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "grenade_spawn_origin.h"

static ConVar bridge_grenade_spawn_at_pos("bridge_grenade_spawn_at_pos", "1", FCVAR_RELEASE,
	"[GRENADE-SPAWN] Create FireWeaponGrenade projectiles at the requested position, not at the owner's eye. "
	"Read when the server attaches; set it on the launch line.");

// FireWeaponGrenade's owner-position call: call [rax+4A8h] returns the vector the projectile
// is created at. The requested position pointer lives in r12 (mov r12, r8 in the prologue).
static constexpr ptrdiff_t GSO_CALL_OFFSET = 0x11;
static constexpr uint8_t GSO_CALL_BYTES[6] = { 0xFF, 0x90, 0xA8, 0x04, 0x00, 0x00 };
static const vector<uint8_t> GSO_PATCH_BYTES = { 0x49, 0x8B, 0xC4, 0x0F, 0x1F, 0x00 }; // mov rax, r12 / nop3

static CMemory s_ownerPosCall;

void VGrenadeSpawnOrigin::GetAdr(void) const
{
	LogVarAdr("FireWeaponGrenade::OwnerPosCall", s_ownerPosCall.RCast<void*>());
}

void VGrenadeSpawnOrigin::GetFun(void) const
{
	const CMemory site = Module_FindPattern(g_GameDll,
		"48 8B 07 48 8D 55 ?? 48 8B 9E E0 27 00 00 48 8B CF FF 90 A8 04 00 00 F3 0F 10 86 08 1C 00 00");
	if (!site)
	{
		Warning(eDLL_T::SERVER, "[GRENADE-SPAWN] FireWeaponGrenade owner-position call unresolved -- remote grenade spawns stay swept to the owner\n");
		return;
	}

	s_ownerPosCall = site.Offset(GSO_CALL_OFFSET);
	if (memcmp(s_ownerPosCall.RCast<const void*>(), GSO_CALL_BYTES, sizeof(GSO_CALL_BYTES)) != 0)
	{
		Warning(eDLL_T::SERVER, "[GRENADE-SPAWN] unexpected bytes at the owner-position call -- patch skipped\n");
		s_ownerPosCall = CMemory();
	}
}

void VGrenadeSpawnOrigin::Detour(const bool bAttach) const
{
	if (!s_ownerPosCall)
		return;

	if (bAttach)
	{
		if (!bridge_grenade_spawn_at_pos.GetBool())
			return;
		s_ownerPosCall.Patch(GSO_PATCH_BYTES);
		Msg(eDLL_T::SERVER, "[GRENADE-SPAWN] FireWeaponGrenade creates projectiles at the requested position\n");
	}
	else
	{
		s_ownerPosCall.Patch(vector<uint8_t>(GSO_CALL_BYTES, GSO_CALL_BYTES + sizeof(GSO_CALL_BYTES)));
	}
}
