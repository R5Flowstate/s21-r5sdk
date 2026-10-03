//=============================================================================//
//
// Purpose: Call CodeCallback_CanUseEntity with its declared arguments from the
//          player-inside-titan "use self" check.
//
// While the player owns a titan soul this check pushes only ( player, player ) to a
// callback declared ( entity player, entity ent, int useFlags ); every other caller passes three.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "titan_use_self.h"

static ConVar cl_titan_use_self_args("cl_titan_use_self_args", "1", FCVAR_RELEASE,
	"Call CodeCallback_CanUseEntity with ( player, ent, useFlags ) from the "
	"use-self check. 0 = stock two-argument call.");

// Script value type of an entity argument, as the engine builds it for this call.
static constexpr int16 SCRIPT_TYPE_ENTITY = 0x23;

// Offsets into the check, each the start of the instruction that carries the operand.
static constexpr ptrdiff_t USE_SELF_CALLBACK_LOAD = 0x97; // mov rbp, [rip+callback]
static constexpr ptrdiff_t USE_SELF_HANDLE_CALL   = 0xDF; // call entity -> script handle
static constexpr ptrdiff_t USE_SELF_VM_LOAD       = 0xF6; // mov rdi, [rip+client vm]

typedef __int64(__fastcall* PFN_UseSelfCheck)(__int64 player);
typedef HSCRIPT(__fastcall* PFN_EntityScriptHandle)(__int64 entity);

static PFN_UseSelfCheck v_UseSelfCheck = nullptr;
static PFN_EntityScriptHandle v_EntityScriptHandle = nullptr;
static HSCRIPT* g_phCanUseEntity = nullptr;
static CSquirrelVM** g_ppClientVM = nullptr;

static bool ScriptResultIsTrue(const ScriptVariant_t& ret)
{
	switch (ret.m_type)
	{
	case FIELD_BOOLEAN: return ret.m_bool;
	case FIELD_INTEGER: return ret.m_int != 0;
	case FIELD_FLOAT:   return ret.m_float != 0.0f;
	default:            return false;
	}
}

static __int64 __fastcall Hook_UseSelfCheck(__int64 player)
{
	if (!cl_titan_use_self_args.GetBool())
		return v_UseSelfCheck(player);

	const HSCRIPT hCallback = *g_phCanUseEntity;
	if (!hCallback)
		return v_UseSelfCheck(player);

	// Stock gating without its two-argument script call.
	*g_phCanUseEntity = nullptr;
	const __int64 result = v_UseSelfCheck(player);
	*g_phCanUseEntity = hCallback;

	if (!result)
		return 0;

	CSquirrelVM* const vm = *g_ppClientVM;
	if (!vm)
		return 0;

	const HSCRIPT hPlayer = v_EntityScriptHandle(player);

	ScriptVariant_t args[3];
	for (int i = 0; i < 2; i++)
	{
		args[i].m_hScript = hPlayer;
		args[i].m_type = SCRIPT_TYPE_ENTITY;
	}
	args[2] = 0;

	ScriptVariant_t ret;
	if (vm->ExecuteFunction(hCallback, args, SDK_ARRAYSIZE(args), &ret, nullptr) != SCRIPT_DONE)
		return 0;

	return ScriptResultIsTrue(ret) ? result : 0;
}

void VTitanUseSelf::GetAdr(void) const
{
	LogFunAdr("UseSelfCheck", v_UseSelfCheck);
	LogFunAdr("EntityScriptHandle", v_EntityScriptHandle);
	LogVarAdr("CanUseEntityCallback", g_phCanUseEntity);
	LogVarAdr("UseSelfClientVM", g_ppClientVM);
}

void VTitanUseSelf::GetFun(void) const
{
	// Player state gate, then the titan-soul handle at +0x19D4.
	const CMemory check = Module_FindPattern(g_GameDll,
		"40 55 56 48 83 EC 68 8B 15 ?? ?? ?? ?? 48 8B F1 48 8B 81 ?? ?? 00 00 "
		"8B 14 02 FF CA F7 C2 FD FF FF FF 0F 85");
	if (!check)
	{
		Warning(eDLL_T::CLIENT, "[TITAN-USE-SELF] pattern unresolved -- stock two-argument call kept\n");
		return;
	}

	const uint8_t* const p = check.RCast<const uint8_t*>();
	if (p[USE_SELF_CALLBACK_LOAD] != 0x48 || p[USE_SELF_CALLBACK_LOAD + 1] != 0x8B ||
		p[USE_SELF_HANDLE_CALL] != 0xE8 ||
		p[USE_SELF_VM_LOAD] != 0x48 || p[USE_SELF_VM_LOAD + 1] != 0x8B)
	{
		Warning(eDLL_T::CLIENT, "[TITAN-USE-SELF] layout mismatch -- stock two-argument call kept\n");
		return;
	}

	g_phCanUseEntity = check.Offset(USE_SELF_CALLBACK_LOAD).ResolveRelativeAddress(3, 7).RCast<HSCRIPT*>();
	g_ppClientVM = check.Offset(USE_SELF_VM_LOAD).ResolveRelativeAddress(3, 7).RCast<CSquirrelVM**>();
	check.Offset(USE_SELF_HANDLE_CALL).FollowNearCall().GetPtr(v_EntityScriptHandle);
	check.GetPtr(v_UseSelfCheck);
}

void VTitanUseSelf::Detour(const bool bAttach) const
{
	if (v_UseSelfCheck && v_EntityScriptHandle && g_phCanUseEntity && g_ppClientVM)
		DetourSetup(&v_UseSelfCheck, &Hook_UseSelfCheck, bAttach);
}
