//=============================================================================//
//
// Purpose: cross-realm player repel gate. The movement repel pass shoves
// players apart while their hulls overlap; the modern engine only pairs two
// players that share a realm bit, this dedi build predates that check and
// pushes players standing on the same spot across realm boundaries. Gate the
// per-pair repel visitor with the same m_realmsBitMask AND the modern engine
// uses (mask 0 = match-none, mirroring its visibility rule).
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "repel_realm_gate.h"

// AddUpRepel functor: +8 holds the moving player (installed by RepelFromOtherPlayers).
static constexpr ptrdiff_t RR_FUNCTOR_OFF_PLAYER = 8;
// Server-half CBaseEntity realm bitfield.
static constexpr ptrdiff_t RR_ENT_OFF_REALMSBITMASK = 0xAE8; // i64 m_realmsBitMask

static ConVar bridge_repel_realm_gate("bridge_repel_realm_gate", "1", FCVAR_RELEASE,
	"Gate the player-vs-player repel pass on shared realms: players in "
	"disjoint realms no longer push each other apart");
static ConVar bridge_repel_realm_gate_diag("bridge_repel_realm_gate_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log repel pairs rejected by the realm gate (rate limited)");

static char (*v_AddUpRepel_ForEachEnt)(__int64 functor, __int64 other) = nullptr;

//-----------------------------------------------------------------------------
// Purpose: true when both entities carry at least one common realm bit.
//-----------------------------------------------------------------------------
static bool RepelRealmGate_SharesRealm(const __int64 self, const __int64 other)
{
	const uint64_t selfMask = *reinterpret_cast<const uint64_t*>(self + RR_ENT_OFF_REALMSBITMASK);
	const uint64_t otherMask = *reinterpret_cast<const uint64_t*>(other + RR_ENT_OFF_REALMSBITMASK);
	return (selfMask & otherMask) != 0;
}

//-----------------------------------------------------------------------------
// Purpose: per-pair repel visitor. Returning 1 continues the iteration without
// accumulating a repel vector for this pair.
//-----------------------------------------------------------------------------
static char __fastcall Hook_AddUpRepel_ForEachEnt(const __int64 functor, const __int64 other)
{
	if (bridge_repel_realm_gate.GetBool() && other)
	{
		const __int64 self = *reinterpret_cast<const __int64*>(functor + RR_FUNCTOR_OFF_PLAYER);

		if (self && !RepelRealmGate_SharesRealm(self, other))
		{
			if (bridge_repel_realm_gate_diag.GetBool())
			{
				static int s_diagCount = 0;

				if (s_diagCount++ < 32 || (s_diagCount % 512) == 0)
				{
					const int16_t selfIndex = *reinterpret_cast<const int16_t*>(self + 88); // edict index word
					const int16_t otherIndex = *reinterpret_cast<const int16_t*>(other + 88);
					DevMsg(eDLL_T::SERVER, "[REPEL-REALM] skipped cross-realm pair self=%d other=%d (n=%d)\n",
						static_cast<int>(selfIndex), static_cast<int>(otherIndex), s_diagCount);
				}
			}

			return 1;
		}
	}

	return v_AddUpRepel_ForEachEnt(functor, other);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VRepelRealmGate::GetAdr(void) const
{
	LogFunAdr("AddUpRepel::ForEachEnt", v_AddUpRepel_ForEachEnt);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VRepelRealmGate::GetFun(void) const
{
	// RepelFromOtherPlayers' AddUpRepel::ForEachEnt -- the per-pair visitor
	// invoked once per candidate player during the repel pass. Stack sizes
	// wildcarded; the xor ebp/mov rsi,rdx/mov rbx,rcx unpack is unique
	// module-wide (unique).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? "
		"57 48 83 EC ?? 33 ED 48 8B F2 48 8B D9")
		.GetPtr(v_AddUpRepel_ForEachEnt);

	if (!v_AddUpRepel_ForEachEnt)
		Warning(eDLL_T::SERVER,
			"[REPEL-REALM] AddUpRepel::ForEachEnt pattern unresolved -- cross-realm repel gate disabled\n");
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void VRepelRealmGate::Detour(const bool bAttach) const
{
	if (v_AddUpRepel_ForEachEnt)
		DetourSetup(&v_AddUpRepel_ForEachEnt, &Hook_AddUpRepel_ForEachEnt, bAttach);
}

