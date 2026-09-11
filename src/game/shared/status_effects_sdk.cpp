#if defined(CLIENT_DLL)
// No client-side status-effects code. The S21 client binds these natives itself and
// the SDK registration path that reached this half no longer exists.

#else // !CLIENT_DLL
//=============================================================================
//
// Purpose: StatusEffect_GetTotalSeverity -- SUM semantics for severity.
//
//=============================================================================

#include "core/stdafx.h"
#include <cmath>
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/status_effect.h"
#include "status_effects_sdk.h"
#include "public/edict.h"

// game_shared_static is linked into both client.dll and dedicated. Dedicated only exposes the
// server-side gpGlobals (CGlobalVars*, from gameinterface.cpp). CGlobalVars inherits
// CGlobalVarsBase, so any client-side use via curTime/tickInterval still resolves correctly.
extern CGlobalVars* gpGlobals;

//------------------------------------------------------------------------------
// Engine entity extraction function
//------------------------------------------------------------------------------
typedef void* (__fastcall *EntityExtractFn_t)(HSQUIRRELVM vm, SQObject* sqobj, void* typeDescAddr);
static EntityExtractFn_t s_fnEntityExtract = nullptr;

// Class gate and offsets from the engine severity walker (three virtual predicates, then the plugin).
static constexpr int VTBL_OFF_IS_PLAYER    = 744;
static constexpr int VTBL_OFF_IS_TITANSOUL = 560;
static constexpr int VTBL_OFF_IS_NPC       = 760;

// CBaseEntity -> status_effect_plugin back-pointer.
static constexpr int ENT_OFF_SE_PLUGIN = 2784;

struct StatusEffectArrays_t
{
	const StatusEffectTimedData* timed;
	int timedCount;
	const StatusEffectEndlessData* endless;
	int endlessCount;
};

static constexpr int STATUS_EFFECT_TYPE_MAX = 256;

//------------------------------------------------------------------------------
// StatusEffects_SDK_Init
//------------------------------------------------------------------------------
static void StatusEffects_SDK_Init()
{
	CMemory entityExtractMem = Module_FindPattern(g_GameDll,
		"48 85 D2 75 16 8B 05 ?? ?? ?? ?? 83 F8 01 7D 08 FF C0 89 05 ?? ?? ?? ?? 33 C0 C3 F7 02 00 80 40 00");

	if (entityExtractMem)
	{
		entityExtractMem.GetPtr(s_fnEntityExtract);
		Msg(eDLL_T::ENGINE, "[StatusEffects_SDK] Entity extract function found at %p\n", s_fnEntityExtract);
	}
	else
	{
		Warning(eDLL_T::ENGINE, "[StatusEffects_SDK] Entity extract function not found!\n");
	}
}

static void StatusEffects_SDK_InitOnce()
{
	static bool s_initDone = false;
	if (s_initDone)
		return;

	StatusEffects_SDK_Init();
	s_initDone = true;
}

//------------------------------------------------------------------------------
// GetSeverityForTimedItem - cosine-based ease-out curve
//------------------------------------------------------------------------------
static float GetSeverityForTimedItem(const StatusEffectTimedData* item, float curTime)
{
	const float pausedTimeRemaining = item->sePausedTimeRemaining;
	const float seTimeEnd = item->seTimeEnd;
	const bool isPaused = pausedTimeRemaining > 0.0f;

	float timeRemaining;
	if (isPaused)
		timeRemaining = pausedTimeRemaining;
	else
		timeRemaining = seTimeEnd - curTime;

	if (curTime >= seTimeEnd && !isPaused)
		return 0.0f;

	// S3 storage: severity at bits[14:7] (final <<7). Wire proxy remaps for S21.
	const float baseSeverity = static_cast<float>(
		static_cast<unsigned char>(item->seComboVars >> 7)) / 255.0f;

	const float easeOut = item->seEaseOut;

	if (timeRemaining > easeOut || easeOut <= 0.0f)
		return baseSeverity;

	const float angle = (timeRemaining / easeOut) * 3.14159265f + 1.57079633f;
	const float factor = (1.0f - cosf(angle)) * 0.5f;
	return factor * baseSeverity;
}

//------------------------------------------------------------------------------
// ExtractEntityFromStack
//------------------------------------------------------------------------------
static void* ExtractEntityFromStack(HSQUIRRELVM v, SQInteger idx)
{
	if (!s_fnEntityExtract)
		return nullptr;

	SQObject obj;
	if (SQ_FAILED(sq_getstackobj(v, idx, &obj)))
		return nullptr;

	return s_fnEntityExtract(v, &obj, nullptr);
}

//------------------------------------------------------------------------------
// CallEntityPredicate - invoke one of the engine's virtual class predicates.
//------------------------------------------------------------------------------
static bool CallEntityPredicate(void* entity, int vtblOffset)
{
	typedef bool(__fastcall * PredicateFn_t)(void*);

	void** const vtable = *reinterpret_cast<void***>(entity);
	if (!vtable)
		return false;

	const PredicateFn_t fn = reinterpret_cast<PredicateFn_t>(
		vtable[vtblOffset / sizeof(void*)]);

	return fn && fn(entity);
}

// Resolve timed/endless arrays. Read-only: never create status_effect_plugin.
static bool GetStatusEffectArrays(void* entity, StatusEffectArrays_t& out)
{
	if (!entity)
		return false;

	const char* const base = reinterpret_cast<const char*>(entity);
	const char* timedBase;
	const char* endlessBase;

	if (CallEntityPredicate(entity, VTBL_OFF_IS_PLAYER))
	{
		timedBase = base + 29456; endlessBase = base + 29696;
		out.timedCount = 10; out.endlessCount = 10;
	}
	else if (CallEntityPredicate(entity, VTBL_OFF_IS_TITANSOUL))
	{
		timedBase = base + 25784; endlessBase = base + 25856;
		out.timedCount = 3; out.endlessCount = 6;
	}
	else if (CallEntityPredicate(entity, VTBL_OFF_IS_NPC))
	{
		timedBase = base + 2928; endlessBase = base + 3168;
		out.timedCount = 10; out.endlessCount = 10;
	}
	else
	{
		const char* const plugin =
			*reinterpret_cast<const char* const*>(base + ENT_OFF_SE_PLUGIN);
		if (!plugin)
			return false;

		timedBase = plugin + 2832; endlessBase = plugin + 2856;
		out.timedCount = 1; out.endlessCount = 1;
	}

	out.timed = reinterpret_cast<const StatusEffectTimedData*>(timedBase);
	out.endless = reinterpret_cast<const StatusEffectEndlessData*>(endlessBase);
	return true;
}

// Sum severity across timed + endless. The engine walker only takes MAX.
static float GetStatusEffectTotalSeverity(void* entity, int effectType, float curTime)
{
	StatusEffectArrays_t arrays;
	if (!GetStatusEffectArrays(entity, arrays))
		return 0.0f;

	float totalSeverity = 0.0f;

	for (int i = 0; i < arrays.timedCount; i++)
	{
		const int comboVars = arrays.timed[i].seComboVars;
		// S3 storage: type at bits[31:25].
		if (((comboVars >> 25) & 0x7F) == effectType)
		{
			totalSeverity += GetSeverityForTimedItem(&arrays.timed[i], curTime);
		}
	}

	for (int i = 0; i < arrays.endlessCount; i++)
	{
		const int comboVars = arrays.endless[i].seComboVars;
		if (comboVars != 0 && ((comboVars >> 25) & 0x7F) == effectType)
		{
			const float severity = static_cast<float>(
				static_cast<unsigned char>(comboVars >> 7)) / 255.0f;
			totalSeverity += severity;
		}
	}

	return totalSeverity;
}

// Script: float StatusEffect_GetTotalSeverity(entity ent, int eStatusEffect)
static SQRESULT ServerScript_StatusEffect_GetTotalSeverity(HSQUIRRELVM v)
{
	void* pEntity = ExtractEntityFromStack(v, 2);
	if (!pEntity)
	{
		v_SQVM_ScriptError("StatusEffect_GetTotalSeverity: entity is null or invalid");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	SQInteger effectType = 0;
	sq_getinteger(v, 3, &effectType);

	if (effectType < 0 || effectType >= STATUS_EFFECT_TYPE_MAX)
	{
		v_SQVM_ScriptError("StatusEffect_GetTotalSeverity: effectType %d out of range (0-%d)",
			static_cast<int>(effectType), STATUS_EFFECT_TYPE_MAX - 1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	const float result = GetStatusEffectTotalSeverity(
		pEntity, static_cast<int>(effectType), gpGlobals->curTime);

	sq_pushfloat(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ClientScript_StatusEffect_GetTotalSeverity(HSQUIRRELVM v)
{
	void* pEntity = ExtractEntityFromStack(v, 2);
	if (!pEntity)
	{
		v_SQVM_ScriptError("StatusEffect_GetTotalSeverity: entity is null or invalid");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	SQInteger effectType = 0;
	sq_getinteger(v, 3, &effectType);

	if (effectType < 0 || effectType >= STATUS_EFFECT_TYPE_MAX)
	{
		v_SQVM_ScriptError("StatusEffect_GetTotalSeverity: effectType %d out of range (0-%d)",
			static_cast<int>(effectType), STATUS_EFFECT_TYPE_MAX - 1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	const float result = GetStatusEffectTotalSeverity(
		pEntity, static_cast<int>(effectType), gpGlobals->curTime);

	sq_pushfloat(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_StatusEffect_GetTotalSeverity(HSQUIRRELVM v)
{
	void* pEntity = ExtractEntityFromStack(v, 2);
	if (!pEntity)
	{
		v_SQVM_ScriptError("StatusEffect_GetTotalSeverity: entity is null or invalid");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	SQInteger effectType = 0;
	sq_getinteger(v, 3, &effectType);

	if (effectType < 0 || effectType >= STATUS_EFFECT_TYPE_MAX)
	{
		v_SQVM_ScriptError("StatusEffect_GetTotalSeverity: effectType %d out of range (0-%d)",
			static_cast<int>(effectType), STATUS_EFFECT_TYPE_MAX - 1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	const float result = GetStatusEffectTotalSeverity(
		pEntity, static_cast<int>(effectType), gpGlobals->curTime);

	sq_pushfloat(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//------------------------------------------------------------------------------
// Registration
//------------------------------------------------------------------------------
void StatusEffects_SDK_RegisterServerFunctions(CSquirrelVM* vm)
{
	if (!vm)
		return;

	StatusEffects_SDK_InitOnce();

	Script_RegisterFuncNamed(vm,
		"StatusEffect_GetTotalSeverity",
		"Server_StatusEffect_GetTotalSeverity",
		"Gets the total (summed) severity of all status effects of the given type on an entity",
		"float",
		"entity ent, int statusEffectType",
		false,
		ServerScript_StatusEffect_GetTotalSeverity);
}

void StatusEffects_SDK_RegisterClientFunctions(CSquirrelVM* vm)
{
	if (!vm)
		return;

	StatusEffects_SDK_InitOnce();

	Script_RegisterFuncNamed(vm,
		"StatusEffect_GetTotalSeverity",
		"Client_StatusEffect_GetTotalSeverity",
		"Gets the total (summed) severity of all status effects of the given type on an entity",
		"float",
		"entity ent, int statusEffectType",
		false,
		ClientScript_StatusEffect_GetTotalSeverity);
}

void StatusEffects_SDK_RegisterUIFunctions(CSquirrelVM* vm)
{
	if (!vm)
		return;

	StatusEffects_SDK_InitOnce();

	Script_RegisterFuncNamed(vm,
		"StatusEffect_GetTotalSeverity",
		"UI_StatusEffect_GetTotalSeverity",
		"Gets the total (summed) severity of all status effects of the given type on an entity",
		"float",
		"entity ent, int statusEffectType",
		false,
		UIScript_StatusEffect_GetTotalSeverity);
}

// NOP the fatal disable_wall_run_and_double_jump parser path. S21 file only has the split names.
void VStatusEffectParseFix::Detour(const bool bAttach) const
{
	if (!bAttach)
		return;

	// Module_FindPattern compiles the signature at compile time, so each call
	// must be a string literal (a runtime const char* will not compile). Resolve
	// each site with a literal and hand the CMemory to a helper for verify + NOP.
	auto nopFatalCall = [](const CMemory hit, const char* const tag)
	{
		if (!hit)
		{
			Warning(eDLL_T::ENGINE,
				"[statuseffect-parsefix] fatal-call site not found (%s) -- dedi "
				"may still die on missing 'disable_wall_run_and_double_jump'.\n",
				tag);
			return;
		}

		// +21 is the E8 rel32 of the call to the severity-3 logger.
		const uint8_t* const pCall = reinterpret_cast<uint8_t*>(hit.GetPtr() + 21);
		if (*pCall != 0xE8)
		{
			Warning(eDLL_T::ENGINE,
				"[statuseffect-parsefix] %s +21 is not a call (0x%02X) -- not "
				"patching.\n", tag, *pCall);
			return;
		}

		hit.Offset(21).Patch({ 0x90, 0x90, 0x90, 0x90, 0x90 });

		Msg(eDLL_T::ENGINE,
			"[statuseffect-parsefix] NOPed fatal 'Code expects "
			"disable_wall_run_and_double_jump' call in %s (@ %p).\n",
			tag, pCall);
	};

	nopFatalCall(Module_FindPattern(g_GameDll,
		"4C 8D 05 95 DF A7 00 48 8D 15 F6 38 BC 00 48 8D 0D EF 3B BC 00 E8 4A 87 BC FF"),
		"Bridge diagnostic control.");
	nopFatalCall(Module_FindPattern(g_GameDll,
		"4C 8D 05 87 86 6E 00 48 8D 15 30 D0 82 00 48 8D 0D 29 D3 82 00 E8 84 1E 83 FF"),
		"Bridge diagnostic control.");
}

#endif // CLIENT_DLL
