//=============================================================================//
//
// Purpose: carried-weapon realm follow.
//
// The S3 server stamps realms onto move-children (SetRealmsBitMask recurses
// the move chain) and onto engine-spawned children (grenades, missiles,
// ropes, particle systems adopt the spawner mask at creation), but nothing
// ever stamps a carried weapon: CWeaponX::Equip has no realm adoption, and
// script realm moves (FS_SetRealmForPlayer: RemoveFromAllRealms +
// AddToRealm) only touch the player entity. A weapon therefore keeps
// whatever mask it spawned with (0 with no owner yet) while its owner moves
// into a fight realm.
//
// The S21 client gates remote-bullet FX on realms twice: the whole
// OnRemoteBulletFired block needs DoesShareRealms(owner, view player), and
// the tracer particle itself is created with the WEAPON's mask. A
// disjoint weapon mask discards the opponent's tracers while models, sounds
// and impacts (owner-gated only) keep working -- the 1v1 symptom.
//
// Fix at both ends on the dedi: adopting the owner mask at activation time
// (weapons given or picked up after the last realm change) and pushing the
// full inventory on every realm change (weapons held across a realm move).
// Both go through the real SetRealmsBitMask so edict dirty-marking and the
// transmit-cache invalidation come for free.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "weapon_realm_follow.h"
#include "game/shared/sdk_entity_state.h"
#include "game/server/util_server.h"

// Server-half CBaseEntity realm bitfield + identity.
static constexpr ptrdiff_t WRF_ENT_OFF_REALMSBITMASK = 0xAE8; // u64 m_realmsBitMask
static constexpr ptrdiff_t WRF_ENT_OFF_REFHANDLE     = 0x8;   // u32 m_RefEHandle
static constexpr ptrdiff_t WRF_ENT_OFF_EDICTINDEX    = 88;    // u16 edict index word
// Server-half CWeaponX owner handle.
static constexpr ptrdiff_t WRF_WEAPON_OFF_OWNER      = 0x11F0; // u32 m_weaponOwner EHandle
// Server-half CPlayer inventory (S3 dedi): 9 backpack + 8 offhand + 3 active,
// EHandle per slot.
static constexpr ptrdiff_t WRF_INV_WEAPONS_BASE = 0x1690;
static constexpr int WRF_INV_WEAPONS_COUNT      = 9;
static constexpr ptrdiff_t WRF_INV_OFFHAND_BASE = 0x16B4;
static constexpr int WRF_INV_OFFHAND_COUNT      = 8;
static constexpr ptrdiff_t WRF_INV_ACTIVE_BASE  = 0x16CC;
static constexpr int WRF_INV_ACTIVE_COUNT       = 3;

static ConVar bridge_weapon_realm_follow("bridge_weapon_realm_follow", "1", FCVAR_RELEASE,
	"Adopt owner realms onto carried weapons: at activation (SetActiveWeapon) "
	"and by pushing the full inventory on every SetRealmsBitMask. Keeps "
	"opponent bullet tracers visible across 1v1 realm moves");

static __int64 (*v_SetRealmsBitMask)(__int64 ent, uint64_t mask) = nullptr;

//-----------------------------------------------------------------------------
// Purpose: stamp one weapon when its mask is stale. The engine setter
// clears the transmit cache, dirties the edict and recurses move children.
//-----------------------------------------------------------------------------
static void WeaponRealmFollow_StampWeapon(const __int64 weaponEnt, const uint64_t ownerMask)
{
	const uint64_t weaponMask = *reinterpret_cast<const uint64_t*>(weaponEnt + WRF_ENT_OFF_REALMSBITMASK);
	if (weaponMask == ownerMask)
		return;

	v_SetRealmsBitMask(weaponEnt, ownerMask);
}

//-----------------------------------------------------------------------------
// Purpose: push ent's fresh mask onto every carried weapon it owns.
// Runs after the engine setter (which already handled the move chain).
// Player-gated: inventory offsets are only mapped on CPlayer-sized
// entities; reading them off a small entity over-reads its allocation
// (live crash: ent+0x1690 on a prop during Spawn). UTIL_PlayerByIndex
// bounds-checks the slot and the pointer-identity check proves ent is the
// live occupant, so stale/freed entities can never reach the walk.
//-----------------------------------------------------------------------------
static void WeaponRealmFollow_PushInventory(const __int64 ent)
{
	if (!ent || !v_SetRealmsBitMask)
		return;

	// +88 is the edict index word on every entity; player slots are
	// 1..maxClients and always CPlayer-sized.
	const int16_t edictIdx = *reinterpret_cast<const int16_t*>(ent + WRF_ENT_OFF_EDICTINDEX);
	if (edictIdx < 1)
		return;
	void* const player = UTIL_PlayerByIndex(edictIdx);
	if (player != reinterpret_cast<void*>(ent))
		return;

	const uint32_t entEH = *reinterpret_cast<const uint32_t*>(ent + WRF_ENT_OFF_REFHANDLE);
	if (entEH == 0xFFFFFFFFu || entEH == 0)
		return;

	const uint64_t entMask = *reinterpret_cast<const uint64_t*>(ent + WRF_ENT_OFF_REALMSBITMASK);

	const ptrdiff_t bases[3] = { WRF_INV_WEAPONS_BASE, WRF_INV_OFFHAND_BASE, WRF_INV_ACTIVE_BASE };
	const int counts[3] = { WRF_INV_WEAPONS_COUNT, WRF_INV_OFFHAND_COUNT, WRF_INV_ACTIVE_COUNT };

	for (int b = 0; b < 3; ++b)
	{
		for (int i = 0; i < counts[b]; ++i)
		{
			const uint32_t weaponEH = *reinterpret_cast<const uint32_t*>(
				ent + bases[b] + static_cast<ptrdiff_t>(4 * i));
			if (weaponEH == 0xFFFFFFFFu || weaponEH == 0)
				continue;

			void* const weapon = SDKEntityState_Resolve(SDKEntityHandle(weaponEH), ESide::Server);
			if (!weapon)
				continue;

			const uint32_t ownerEH = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<uintptr_t>(weapon) + WRF_WEAPON_OFF_OWNER);
			if (ownerEH != entEH)
				continue;

			WeaponRealmFollow_StampWeapon(reinterpret_cast<__int64>(weapon), entMask);
		}
	}
}

static thread_local bool s_bInPushInventory = false;

static __int64 __fastcall Hook_SetRealmsBitMask(const __int64 ent, const uint64_t mask)
{
	const __int64 result = v_SetRealmsBitMask(ent, mask);

	// The stamp path below re-enters this setter on each weapon (orig only,
	// never this hook), but a nested push would redo the same walk; skip it.
	if (bridge_weapon_realm_follow.GetBool() && ent && !s_bInPushInventory)
	{
		s_bInPushInventory = true;
		WeaponRealmFollow_PushInventory(ent);
		s_bInPushInventory = false;
	}

	return result;
}

//-----------------------------------------------------------------------------
// Purpose: activation-time adoption for weapon_select_mirror's
// SetActiveWeapon post-hook. A weapon that fires was activated; stamping
// here guarantees every firer carries its owner's realms.
//-----------------------------------------------------------------------------
void WeaponRealmFollow_StampActiveWeapon(void* const player, const __int64 weaponEnt)
{
	if (!bridge_weapon_realm_follow.GetBool() || !player || !weaponEnt || !v_SetRealmsBitMask)
		return;

	// weaponEnt arrives after orig ran; the engine may have rejected or
	// freed it. Resolve through the handle map and prove this pointer is
	// the live occupant before writing its mask.
	const SDKEntityHandle weaponEH = SDKEntityState_GetHandle(reinterpret_cast<const void*>(weaponEnt));
	if (!weaponEH.IsValid()
		|| SDKEntityState_Resolve(weaponEH, ESide::Server) != reinterpret_cast<void*>(weaponEnt))
		return;

	const uint64_t playerMask = *reinterpret_cast<const uint64_t*>(
		reinterpret_cast<uintptr_t>(player) + WRF_ENT_OFF_REALMSBITMASK);
	WeaponRealmFollow_StampWeapon(weaponEnt, playerMask);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VWeaponRealmFollow::GetAdr(void) const
{
	LogFunAdr("SetRealmsBitMask", v_SetRealmsBitMask);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VWeaponRealmFollow::GetFun(void) const
{
	// Server-half CBaseEntity::SetRealmsBitMask. The tail carries the
	// server-half mask compare (cmp [rbx+0AE8h]); the client twin keeps
	// its mask at +0x938, so it cannot match these bytes. Single hit on
	// the dedi; transmit-serial bump and move-child resolve up front.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 "
		"48 8B 01 48 8B FA 48 8B D9 FF 90 E8 02 00 00 "
		"48 8D 35 ?? ?? ?? ?? 84 C0 74 ?? F0 FF 05 ?? ?? ?? ?? "
		"48 8B 03 48 8B CB FF 90 E8 02 00 00 33 C9 84 C0 "
		"48 0F 45 CB 8B 91 40 63 00 00 8B C2 83 FA FF 74 ?? "
		"0F B7 C2 C1 EA 10 48 8D 0C 40 48 03 C9 39 54 CE 08 "
		"75 ?? 48 8B 0C CE EB ?? 33 C9 48 8B D7 E8 ?? ?? ?? ?? "
		"C7 83 00 0B 00 00 00 00 00 00 48 39 BB E8 0A 00 00")
		.GetPtr(v_SetRealmsBitMask);

	if (!v_SetRealmsBitMask)
		Warning(eDLL_T::SERVER,
			"[REALM-FOLLOW] SetRealmsBitMask pattern unresolved -- weapon realm follow disabled\n");
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VWeaponRealmFollow::Detour(const bool bAttach) const
{
	if (v_SetRealmsBitMask)
		DetourSetup(&v_SetRealmsBitMask, &Hook_SetRealmsBitMask, bAttach);
}
