//=============================================================================//
//
// Purpose: Skip the weapon world-model proxy update when the weapon's owner is
//          not a player.
//
// The proxy is created from the weapon's model state alone, but its update reads
// the owner as a C_Player (a handle array at +0x2A5C, count at +0x2A64). A weapon
// held by an NPC titan (0x1E50 bytes) sends that walk off the end of the object.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "game/client/weapon_world_proxy.h"

static ConVar cl_weapon_proxy_player_owner_only("cl_weapon_proxy_player_owner_only", "1", FCVAR_RELEASE,
	"Update a weapon's world-model proxy only while a player owns the weapon. 0 = stock update for any owner.");

static constexpr ptrdiff_t PROXY_OFF_WEAPON  = 0x1598; // proxy -> owning weapon
static constexpr ptrdiff_t WEAPON_OFF_OWNER  = 0x1560; // weapon m_hOwner (EHANDLE)
static constexpr ptrdiff_t PROXY_UPDATE_TABLE_LEA = 0x33; // lea rdx, [rip+entity handle table]
static constexpr uint32_t  INVALID_EHANDLE = 0xFFFFFFFFu;
static constexpr size_t    ENT_VT_ISPLAYER = 0x4E0 / sizeof(void*); // the slot the proxy update itself tests

typedef void(__fastcall* PFN_ProxyUpdate)(uintptr_t proxy);

static PFN_ProxyUpdate v_WeaponWorldProxy_Update = nullptr;
// Entity handle table: 32-byte entries, entity pointer at +0, serial at +8.
static const uint8_t* s_pEntTable = nullptr;
static bool s_bLoggedSkip = false;

static uintptr_t ResolveHandle(const uint32_t hEnt)
{
	if (hEnt == INVALID_EHANDLE)
		return 0;

	const uint8_t* const pEntry = s_pEntTable + (static_cast<size_t>(hEnt & 0xFFFF) << 5);
	if (*reinterpret_cast<const uint32_t*>(pEntry + 8) != (hEnt >> 16))
		return 0;

	return *reinterpret_cast<const uintptr_t*>(pEntry);
}

static void __fastcall Hook_WeaponWorldProxy_Update(const uintptr_t proxy)
{
	if (cl_weapon_proxy_player_owner_only.GetBool())
	{
		const uintptr_t weapon = *reinterpret_cast<const uintptr_t*>(proxy + PROXY_OFF_WEAPON);
		const uintptr_t owner = weapon ? ResolveHandle(*reinterpret_cast<const uint32_t*>(weapon + WEAPON_OFF_OWNER)) : 0;

		typedef bool(__fastcall* PFN_IsPlayer)(uintptr_t);
		if (owner && !(*reinterpret_cast<PFN_IsPlayer* const*>(owner))[ENT_VT_ISPLAYER](owner))
		{
			if (!s_bLoggedSkip)
			{
				s_bLoggedSkip = true;
				Msg(eDLL_T::CLIENT, "[WEAPON-PROXY] skipped world-model proxy update for non-player owner %p (weapon %p)\n",
					reinterpret_cast<void*>(owner), reinterpret_cast<void*>(weapon));
			}
			return;
		}
	}

	v_WeaponWorldProxy_Update(proxy);
}

void VWeaponWorldProxy::GetAdr(void) const
{
	LogFunAdr("WeaponWorldProxy_Update", v_WeaponWorldProxy_Update);
	LogVarAdr("WeaponWorldProxy_EntTable", s_pEntTable);
}

void VWeaponWorldProxy::GetFun(void) const
{
	// Proxy flags at +0x334, then the owning weapon (+0x1598) and its owner handle (+0x1560).
	const CMemory update = Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 8B 81 34 03 00 00 48 8B D9 C1 E8 05 A8 01 0F 85 ?? ?? ?? ?? "
		"48 8B 81 98 15 00 00 8B 88 60 15 00 00");
	if (!update)
	{
		Warning(eDLL_T::CLIENT, "[WEAPON-PROXY] pattern unresolved -- stock proxy update kept\n");
		return;
	}

	const uint8_t* const p = update.RCast<const uint8_t*>();
	if (p[PROXY_UPDATE_TABLE_LEA] != 0x48 || p[PROXY_UPDATE_TABLE_LEA + 1] != 0x8D || p[PROXY_UPDATE_TABLE_LEA + 2] != 0x15)
	{
		Warning(eDLL_T::CLIENT, "[WEAPON-PROXY] layout mismatch -- stock proxy update kept\n");
		return;
	}

	s_pEntTable = update.Offset(PROXY_UPDATE_TABLE_LEA).ResolveRelativeAddress(3, 7).RCast<const uint8_t*>();
	update.GetPtr(v_WeaponWorldProxy_Update);
}

void VWeaponWorldProxy::Detour(const bool bAttach) const
{
	if (v_WeaponWorldProxy_Update && s_pEntTable)
		DetourSetup(&v_WeaponWorldProxy_Update, &Hook_WeaponWorldProxy_Update, bAttach);
}
