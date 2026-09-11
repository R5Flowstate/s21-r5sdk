//=============================================================================//
//
// Purpose: Bridge for passive-changed replication events
//
//=============================================================================//
#include "core/stdafx.h"
#include "passive_changed.h"


#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "baseentity.h"

//-----------------------------------------------------------------------------

// GivePassive/RemovePassive: 128-bit bitfield RMW at player+0x5FF0
// (word = passiveIdx>>6, bit = 1<<(passiveIdx&0x3F)) plus network-dirty at
// +0x58; no Squirrel callback in-body.
//-----------------------------------------------------------------------------
typedef __int64(__fastcall* PassiveBitOp_t)(CBaseEntity* player, unsigned int passiveIdx);
static PassiveBitOp_t v_GivePassive = nullptr;
static PassiveBitOp_t v_RemovePassive = nullptr;

static constexpr ptrdiff_t PASSIVE_BITFIELD_OFF = 0x5FF0;

static bool Passive_BitIsSet(CBaseEntity* const player, const unsigned int passiveIdx)
{
	const uint64_t word = *reinterpret_cast<const uint64_t*>(
		reinterpret_cast<uintptr_t>(player) + PASSIVE_BITFIELD_OFF + 8 * (passiveIdx >> 6));
	return ((word >> (passiveIdx & 0x3F)) & 1) != 0;
}

// Manually invokes the missing native->script dispatch
// CodeCallback_OnPassiveChanged(player, passiveIdx). Mirrors the working
// FindFunction/ExecuteFunction pattern in scriptremotefunctions_server.cpp.
static void Bridge_FirePassiveChanged(CBaseEntity* const player, const unsigned int passiveIdx)
{
	if (!g_pServerScript)
	{
		Warning(eDLL_T::SERVER, "[PASSIVE-BRIDGE] no server VM -- cannot fire CodeCallback_OnPassiveChanged\n");
		return;
	}

	const HSCRIPT hPlayerScript = player->GetScriptInstance();
	if (!hPlayerScript)
	{
		Warning(eDLL_T::SERVER, "[PASSIVE-BRIDGE] player has no script instance -- cannot fire CodeCallback_OnPassiveChanged\n");
		return;
	}

	const HSCRIPT hFunc = g_pServerScript->FindFunction("CodeCallback_OnPassiveChanged", nullptr, nullptr);
	if (!hFunc)
	{
		Warning(eDLL_T::SERVER, "[PASSIVE-BRIDGE] CodeCallback_OnPassiveChanged not found in VM\n");
		return;
	}

	ScriptVariant_t args[2];
	args[0] = hPlayerScript;
	args[1] = static_cast<int>(passiveIdx);

	g_pServerScript->ExecuteFunction(hFunc, args, 2, nullptr, nullptr);
}

static __int64 __fastcall Hook_GivePassive(CBaseEntity* player, unsigned int passiveIdx)
{
	const bool hadBefore = Passive_BitIsSet(player, passiveIdx);
	const __int64 result = v_GivePassive(player, passiveIdx);

	if (!hadBefore && Passive_BitIsSet(player, passiveIdx))
		Bridge_FirePassiveChanged(player, passiveIdx);

	return result;
}

static __int64 __fastcall Hook_RemovePassive(CBaseEntity* player, unsigned int passiveIdx)
{
	const bool hadBefore = Passive_BitIsSet(player, passiveIdx);
	const __int64 result = v_RemovePassive(player, passiveIdx);

	if (hadBefore && !Passive_BitIsSet(player, passiveIdx))
		Bridge_FirePassiveChanged(player, passiveIdx);

	return result;
}

void VPassiveChangedBridge::GetAdr(void) const
{
	LogFunAdr("GivePassive", v_GivePassive);
	LogFunAdr("RemovePassive", v_RemovePassive);
}

void VPassiveChangedBridge::GetFun(void) const
{

	// session). The two natives are byte-identical except the trailing
	// conditional jump polarity (75/jnz for Give's skip-if-already-set,
	// 74/jz for Remove's skip-if-not-set).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 57 48 83 EC ? 8B DA 48 8B F9 83 FA ? 76 ? 41 B8 ? ? ? ? 48 8D 0D ? ? ? ? "
		"8B D3 E8 ? ? ? ? 8B CB 48 8B C3 48 C1 E8 ? 83 E1 ? BA ? ? ? ? 48 D3 E2 48 8D 0C C7 48 8B 81 ? ? ? ? 48 85 C2 75")
		.GetPtr(v_GivePassive);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ? 57 48 83 EC ? 8B DA 48 8B F9 83 FA ? 76 ? 41 B8 ? ? ? ? 48 8D 0D ? ? ? ? "
		"8B D3 E8 ? ? ? ? 8B CB 48 8B C3 48 C1 E8 ? 83 E1 ? BA ? ? ? ? 48 D3 E2 48 8D 0C C7 48 8B 81 ? ? ? ? 48 85 C2 74")
		.GetPtr(v_RemovePassive);

	if (!v_GivePassive || !v_RemovePassive)
		Warning(eDLL_T::SERVER, "[PASSIVE-BRIDGE] GivePassive/RemovePassive pattern unresolved -- passive-changed bridge disabled\n");
}

void VPassiveChangedBridge::Detour(const bool bAttach) const
{
	if (v_GivePassive)
		DetourSetup(&v_GivePassive, &Hook_GivePassive, bAttach);

	if (v_RemovePassive)
		DetourSetup(&v_RemovePassive, &Hook_RemovePassive, bAttach);
}

