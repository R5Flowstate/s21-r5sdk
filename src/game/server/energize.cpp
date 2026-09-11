//=============================================================================//
//
// Purpose: Energize ("revved") weapon mechanic bridge -- see energize.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "public/globalvars_base.h"
#include "public/game/shared/in_buttons.h"
#include "energize.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/dt_extend.h"
#include "game/shared/activity.h"
#include "game/shared/weapstate_s3_to_s21.h"

#include "public/edict.h"
#include "game/server/baseentity.h"
#include "game/server/basecombatcharacter.h"
#include "game/server/bridge_cmd_chain.h"
#include "game/shared/usercmd.h"
#include "game/shared/edict_dirty.h"

#include <unordered_map>
#include <string>
#include <fstream>
#include "public/tier1/sdk_parse.h"

extern CGlobalVars* gpGlobals;

// Runtime gate for the FSM driver. Accessor natives stay registered either
// way (harmless reads default to NONE/0) -- this only gates Think.
static ConVar sdk_energize_bridge("sdk_energize_bridge", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Enables the native Energize ('revved') FSM driver (0=off, 1=on).");

// Mirrors the S21 client ConVar of the same name (default "0.25", read off its
// registration at 0x1400E5030). Both the ENERGIZED edge and the cancel push
// m_nextEnergizeReadyTime this far past m_nextReadyTime.
static ConVar sdk_energize_next_cooldown("sdk_energize_next_cooldown", "0.25",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Seconds added on top of m_nextReadyTime for the re-energize gate "
	"(the S21 client's nextEnergizeCooldownTime, default 0.25).");

static ConVar bridge_sv_btn_trace("bridge_sv_btn_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[SV-BTN] Log first-seen button bits on the dedi (0 = off).");

// EnergizeState_e matches the script enum and the networked m_energizeState.
enum EnergizeState_e
{
	ENERGIZE_NONE      = 0,
	ENERGIZE_ENERGIZING = 1,
	ENERGIZE_ENERGIZED  = 2,
};

// m_weaponName[65] at the same CWeapon offset snapshot_diag uses.
static constexpr int WEAPON_CLASSNAME_OFFSET = 0x15B0;

static inline const char* GetWeaponClassNameRaw(void* pWeapon)
{
	return pWeapon ? (const char*)((uintptr_t)pWeapon + WEAPON_CLASSNAME_OFFSET) : "";
}

// Per-class config parsed once from the weapon txt.
struct EnergizeWeaponConfig
{
	bool hasEnergized = false;
	float energizedDuration = 0.0f;
	float energizeActivityTime = 0.0f;
	// Each primary-fire while ENERGIZED subtracts this from the energize pool.
	float energizedTimeConsumedPerShot = 0.0f;
	bool canEnergizeWhenEnergized = false;
	// KV "zoom_effects", modvar default TRUE. Gates the two ADS bits in the
	// wind-up cancel mask.
	bool zoomEffects = true;

	// GetFireRateDelay = m_modVars.fireDuration + (fireRate ? 1/fireRate : 0).
	float fireRate = 0.0f;
	float fireDuration = 0.0f;

	std::string cbTryEnergize;
	std::string cbStartEnergizing;
	std::string cbEnergizedStart;
	std::string cbEnergizedEnd;
};

// Instance FSM. Networked: m_energizeState / m_lastEnergizeState / m_energizedEndTime / m_startEnergizingTime.
struct EnergizeInstanceState
{
	int energizeState = ENERGIZE_NONE;
	int lastEnergizeState = ENERGIZE_NONE;
	float startEnergizingTime = 0.0f;
	float energizedEndTime = 0.0f;

	bool energizeValidated = false;
	bool wasEnergizedWhenStartEnergizing = false;
	float lastEnergizedEndTime = 0.0f;

	// m_nextEnergizeReadyTime. The S21 field (weapon+0x2EB8) sits past m_modVars
	// and is not in DT_WeaponX, so it cannot be networked -- it lives here and
	// gates our own entry the way the client's copy gates Input_CreateMove.
	float nextEnergizeReadyTime = 0.0f;

	// Edge detector for per-shot drain: last observed CWeaponX::m_lastPrimaryAttack.
	// Seeded on enter-ENERGIZED so a stale pre-charge fire time does not
	// immediately burn a shot.
	float lastSeenPrimaryAttack = 0.0f;
	bool lastSeenPrimaryAttackValid = false;

	// m_lastEnergizeState is an edge detector. Leaving last at ENERGIZING re-fires CheckForEnergize every snapshot.
	int energizingEnteredTick = -1;
	bool chargeAnimPlayed = false;

	// Bonus natives: IsWeaponActivelyFiring / NeedsRechambering (sling-weapon gates).
	bool isActivelyFiringStub = false;
	bool needsRechamberingStub = false;
};


// Live CWeaponX field access via helpers -- not hardcoded offsets.
static bool Energize_GetWeaponFloat(void* pWeapon, const char* propName, float* pOut)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*pOut = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(pWeapon) + off);
	return true;
}

static bool Energize_SetWeaponFloat(void* pWeapon, const char* propName, float value)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pWeapon) + off) = value;
	MarkEntityEdictDirty(pWeapon);
	return true;
}

static bool Energize_SetWeaponBool(void* pWeapon, const char* propName, bool value)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(pWeapon) + off) = value ? 1 : 0;
	MarkEntityEdictDirty(pWeapon);
	return true;
}

static bool Energize_GetWeaponInt(void* pWeapon, const char* propName, int* pOut)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*pOut = *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(pWeapon) + off);
	return true;
}

static float GetWeaponLastPrimaryAttack(void* pWeapon)
{
	float value = 0.0f;
	Energize_GetWeaponFloat(pWeapon, "m_lastPrimaryAttack", &value);
	return value;
}

// Charge anim: ACT_VM_ENERGIZE via WeaponCustomAct_ServerExecuteByName. Do not re-translate an S3 id.
static constexpr ptrdiff_t kWpnOffStudioHdr      = 0xFD8;
static constexpr ptrdiff_t kWpnOffIdealSequence  = 0x1214;
static constexpr ptrdiff_t kWpnOffIdealActivity  = 0x1218;
static constexpr ptrdiff_t kWpnOffWeaponActivity = 0x121C;

// char __fastcall SetIdealWeaponActivity(CWeaponX* this, unsigned activity)
typedef char (__fastcall* SetIdealWeaponActivity_t)(void* pWeapon, unsigned int activity);
static SetIdealWeaponActivity_t v_SetIdealWeaponActivity = nullptr;
static bool s_setIdealResolved = false;

static void Energize_ResolveSetIdealWeaponActivity(void)
{
	if (s_setIdealResolved)
		return;
	s_setIdealResolved = true;

	// Unique prologue (offhand_activation_patches / r5apex_ds)
	// mov [rsp+8],rbx; mov [rsp+10h],rdi; push rbp; mov rbp,rsp;
	// sub rsp,70h; cmp qword ptr [rcx+0FD8h],0
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 7C 24 10 55 48 8B EC 48 83 EC 70 "
		"48 83 B9 D8 0F 00 00 00")
		.GetPtr(v_SetIdealWeaponActivity);

	if (v_SetIdealWeaponActivity)
		Msg(eDLL_T::SERVER, "[Energize] SetIdealWeaponActivity resolved @ %p\n",
			reinterpret_cast<void*>(v_SetIdealWeaponActivity));
	else
		Warning(eDLL_T::SERVER,
			"[Energize] SetIdealWeaponActivity pattern unresolved -- charge anim "
			"falls back to StartCustomActivity only\n");
}

static bool Energize_PlayChargeAnim(void* pPlayer, void* pWeapon)
{
	const int actId = FindActivityByName("ACT_VM_ENERGIZE");

	Energize_ResolveSetIdealWeaponActivity();

	int seqBefore = -2, idealBefore = -2, weapBefore = -2;
	int seqAfter  = -2, idealAfter  = -2, weapAfter  = -2;
	uintptr_t studioHdr = 0;
	char idealOk = 0;
	// Activity binding is a one-shot latch. Re-resolve when the model or global act ver changes.
	int mdlActVer = -2, globalActVer = -2;

	if (pWeapon)
	{
		uint8_t* const w = reinterpret_cast<uint8_t*>(pWeapon);
		seqBefore   = *reinterpret_cast<int*>(w + kWpnOffIdealSequence);
		idealBefore = *reinterpret_cast<int*>(w + kWpnOffIdealActivity);
		weapBefore  = *reinterpret_cast<int*>(w + kWpnOffWeaponActivity);
		studioHdr   = *reinterpret_cast<uintptr_t*>(w + kWpnOffStudioHdr);

		if (v_SetIdealWeaponActivity && actId >= 0)
		{
			// Full native pipeline -- SelectWeightedSequence gate included.
			idealOk = v_SetIdealWeaponActivity(pWeapon, static_cast<unsigned int>(actId));
		}

		seqAfter   = *reinterpret_cast<int*>(w + kWpnOffIdealSequence);
		idealAfter = *reinterpret_cast<int*>(w + kWpnOffIdealActivity);
		weapAfter  = *reinterpret_cast<int*>(w + kWpnOffWeaponActivity);
		studioHdr  = *reinterpret_cast<uintptr_t*>(w + kWpnOffStudioHdr);

		if (g_pActivityListVersion_Server)
			globalActVer = *g_pActivityListVersion_Server;
		if (studioHdr)
		{
			const uintptr_t hdr = *reinterpret_cast<uintptr_t*>(studioHdr + 8);
			if (hdr)
				mdlActVer = *reinterpret_cast<int*>(hdr + 0xC8);
		}
	}

	// No StartCustomActivity fallback: S21 plays the charge through the IDEAL
	// path only, and m_customActivity is a networked prop shared with the inspect
	// system, so writing it here handed the client a second, contradictory pose.
	const bool bindFailed = actId >= 0 && !idealOk && v_SetIdealWeaponActivity;

	if (bindFailed)
	{
		// mdlActVer vs globalActVer: model never re-resolved vs activity table changed.
		Warning(eDLL_T::SERVER,
			"[Energize] charge-anim FAILED player=%p weapon=%p ACT_VM_ENERGIZE id=%d "
			"studioHdr=%p mdlActVer=%d globalActVer=%d seq %d->%d ideal %d->%d "
			"weapAct %d->%d (no sequence bound to this activity -- seqtable below)\n",
			pPlayer, pWeapon, actId, reinterpret_cast<void*>(studioHdr),
			mdlActVer, globalActVer,
			seqBefore, seqAfter, idealBefore, idealAfter, weapBefore, weapAfter);

		if (studioHdr)
			DTExtend_DumpSeqTableForStudioHdr(studioHdr, "energize-charge");
	}
	else
	{
		DevMsg(eDLL_T::SERVER,
			"[Energize] charge-anim weapon=%p seq %d->%d ideal %d->%d weapAct %d->%d\n",
			pWeapon, seqBefore, seqAfter, idealBefore, idealAfter, weapBefore, weapAfter);

		// AE_WPN_ENERGIZED is not portable to this host; completion stays on
		// energize_activity_time. Opt-in event dumps live on the bind-failed path
		// and DTExtend helpers -- do not auto-dump on every successful charge.
	}

	return idealOk != 0;
}

static SDKEntityMap<EnergizeInstanceState> s_energizeStatesServer(ESide::Server, "energize.srv");
static SDKEntityMap<EnergizeInstanceState> s_energizeStatesClient(ESide::Client, "energize.cli");

static SDKEntityMap<EnergizeInstanceState>& GetStatesMap(HSQUIRRELVM v)
{
	return (v && v->GetContext() == SQCONTEXT::SERVER) ? s_energizeStatesServer
	                                                    : s_energizeStatesClient;
}

static std::unordered_map<uint64_t, EnergizeWeaponConfig> s_configCache;
static constexpr size_t MAX_ENERGIZE_CONFIGS = 64;

static uint64_t EnergizeConfigHash(const char* s)
{
	uint64_t h = 14695981039346656037ull;
	for (; *s; ++s)
		h = (h ^ static_cast<unsigned char>(*s)) * 1099511628211ull;
	return h;
}

//-----------------------------------------------------------------------------
// KV parser -- same shape as weapon_heat (ParseQuotedString in sdk_parse.h).
//-----------------------------------------------------------------------------
static EnergizeWeaponConfig LoadEnergizeConfigFromFile(const std::string& weaponClassName)
{
	EnergizeWeaponConfig config;

	if (weaponClassName.find("..") != std::string::npos ||
		weaponClassName.find('/') != std::string::npos ||
		weaponClassName.find('\\') != std::string::npos)
	{
		Warning(eDLL_T::SERVER, "[Energize] Rejected invalid weapon classname '%s'\n",
			weaponClassName.c_str());
		return config;
	}

	const char* searchPaths[] = {
		"platform/scripts/weapons/"
	};

	std::ifstream file;
	for (const char* basePath : searchPaths)
	{
		std::string path = std::string(basePath) + weaponClassName + ".txt";
		file.open(path);
		if (file.is_open())
			break;
	}

	if (!file.is_open())
		return config;

	std::string line;
	while (std::getline(file, line))
	{
		size_t commentPos = line.find("//");
		if (commentPos != std::string::npos)
			line = line.substr(0, commentPos);

		Sdk_TrimWhitespace(line);
		if (line.empty())
			continue;

		size_t pos = 0;
		std::string key, value;
		if (!ParseQuotedString(line, pos, key))
			continue;
		if (!ParseQuotedString(line, pos, value))
			continue;

		if (key == "has_energized")
			config.hasEnergized = (Sdk_ParseInt(value) != 0);
		else if (key == "energized_duration")
			config.energizedDuration = Sdk_ParseFloat(value);
		else if (key == "energize_activity_time")
			config.energizeActivityTime = Sdk_ParseFloat(value);
		else if (key == "energized_time_consumed_per_shot")
			config.energizedTimeConsumedPerShot = Sdk_ParseFloat(value);
		else if (key == "can_energize_when_energized")
			config.canEnergizeWhenEnergized = (Sdk_ParseInt(value) != 0);
		else if (key == "zoom_effects")
			config.zoomEffects = (Sdk_ParseInt(value) != 0);
		else if (key == "fire_rate" && config.fireRate == 0.0f)
			config.fireRate = Sdk_ParseFloat(value);   // first hit only: the Mods blocks re-declare it
		else if (key == "fire_duration" && config.fireDuration == 0.0f)
			config.fireDuration = Sdk_ParseFloat(value);
		else if (key == "OnWeaponTryEnergize")
			config.cbTryEnergize = value;
		else if (key == "OnWeaponStartEnergizing")
			config.cbStartEnergizing = value;
		else if (key == "OnWeaponEnergizedStart")
			config.cbEnergizedStart = value;
		else if (key == "OnWeaponEnergizedEnd")
			config.cbEnergizedEnd = value;
	}

	file.close();

	if (config.hasEnergized)
	{
		const float shots = (config.energizedTimeConsumedPerShot > 0.0f)
			? (config.energizedDuration / config.energizedTimeConsumedPerShot)
			: 0.0f;
		DevMsg(eDLL_T::SERVER,
			"[Energize] Loaded '%s' -- duration=%.1f perShot=%.2f (~%.0f shots) "
			"activityTime=%.2f reenergize=%d cbTry='%s' cbStart='%s' cbStart2='%s' cbEnd='%s'\n",
			weaponClassName.c_str(), config.energizedDuration, config.energizedTimeConsumedPerShot,
			shots, config.energizeActivityTime,
			config.canEnergizeWhenEnergized, config.cbTryEnergize.c_str(),
			config.cbStartEnergizing.c_str(), config.cbEnergizedStart.c_str(),
			config.cbEnergizedEnd.c_str());
	}

	return config;
}

static const EnergizeWeaponConfig& GetEnergizeConfig(const char* weaponClassName)
{
	if (!weaponClassName || !*weaponClassName)
	{
		static EnergizeWeaponConfig s_empty;
		return s_empty;
	}

	const uint64_t hash = EnergizeConfigHash(weaponClassName);
	auto it = s_configCache.find(hash);
	if (it != s_configCache.end())
		return it->second;

	if (s_configCache.size() >= MAX_ENERGIZE_CONFIGS)
	{
		static EnergizeWeaponConfig s_empty;
		return s_empty;
	}

	EnergizeWeaponConfig config = LoadEnergizeConfigFromFile(weaponClassName);
	auto result = s_configCache.emplace(hash, config);
	return result.first->second;
}

static EnergizeInstanceState& GetInstanceState(HSQUIRRELVM v, void* pWeapon)
{
	return GetStatesMap(v)[pWeapon];
}

// Energize sidecar: appended DT_WeaponX slots alias live m_modVars. Do not write the append offsets.
static void SyncNetworkedFields(void* /*pWeapon*/, const EnergizeInstanceState& /*inst*/)
{
	// Intentionally empty. State reaches the client through the value proxies;
	// see EnergizeBridge_WireGet. Kept as a named no-op so the call sites keep
	// documenting where a state change becomes visible to the client.
}

// Wire accessor. False if this weapon has no FSM entry (proxy emits 0).
bool EnergizeBridge_WireGet(void* pWeapon, int* pState, float* pStartTime, float* pEndTime)
{
	if (!pWeapon)
		return false;

	const EnergizeInstanceState* const st = s_energizeStatesServer.Find(pWeapon);
	if (!st)
		return false;

	if (pState)     *pState     = st->energizeState;
	if (pStartTime) *pStartTime = st->startEnergizingTime;
	if (pEndTime)   *pEndTime   = st->energizedEndTime;
	return true;
}

// Script-closure invoke: resolve weapon.<method>, call with this=weaponObj.
static bool InvokeGlobalClosure(const std::string& closureName, ScriptVariant_t* args, unsigned int nArgs,
	ScriptVariant_t* pReturn)
{
	if (closureName.empty())
		return false; // this weapon's.txt doesn't bind this slot -- silent no-op, matches native GetWeaponCallback(null) semantics

	if (!g_pServerScript)
	{
		Warning(eDLL_T::SERVER, "[Energize] no server VM -- cannot fire '%s'\n", closureName.c_str());
		return false;
	}

	const HSCRIPT hFunc = g_pServerScript->FindFunction(closureName.c_str(), nullptr, nullptr);
	if (!hFunc)
	{
		Warning(eDLL_T::SERVER, "[Energize] closure '%s' not found in VM\n", closureName.c_str());
		return false;
	}

	g_pServerScript->ExecuteFunction(hFunc, args, nArgs, pReturn, nullptr);
	return true;
}

// AddMod/RemoveMod(modName) swap the KV Mods[modName] block. Call through the real script method.
bool WeaponBridge_InvokeChangeMod(HSCRIPT hWeaponScript, const char* modName, bool isAdding)
{
	if (!g_pServerScript || !hWeaponScript || !modName)
		return false;

	HSQUIRRELVM const v = g_pServerScript->GetVM();
	const HSQOBJECT& weaponObj = *reinterpret_cast<const HSQOBJECT*>(hWeaponScript);
	const char* const methodName = isAdding ? "AddMod" : "RemoveMod";

	// Resolve weapon.<methodName> into a local HSQOBJECT; pop the pushed slot.
	sq_pushobject(v, weaponObj);
	sq_pushstring(v, methodName, -1);
	if (SQ_FAILED(sq_get(v, -2)))
	{
		sq_pop(v, 1);
		Warning(eDLL_T::SERVER, "[Energize] '%s' not resolvable on weapon instance\n", methodName);
		return false;
	}
	HSQOBJECT closureObj;
	sq_getstackobj(v, -1, &closureObj);
	sq_pop(v, 2);

	// Call closure(weaponObj, modName): [closure, this=weaponObj, arg1].
	sq_pushobject(v, closureObj);
	sq_pushobject(v, weaponObj);
	sq_pushstring(v, modName, -1);
	const bool ok = SQ_SUCCEEDED(sq_call(v, 2, SQFalse, SQTrue));
	sq_pop(v, 1);
	return ok;
}

// Nine script natives (7 Energize + 2 bonus) on the weapon class descriptor.
static SQRESULT Script_GetEnergizeState(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushinteger(v, GetInstanceState(v, pWeapon).energizeState);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetLastEnergizeState(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushinteger(v, GetInstanceState(v, pWeapon).lastEnergizeState);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetEnergizedEndTime(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushfloat(v, GetInstanceState(v, pWeapon).energizedEndTime);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Matches Script_ForceEnergizeState exactly: gated on hasEnergized, warns
// (not silently no-ops) if the weapon isn't an energize weapon. Does NOT
// touch m_lastEnergizeState (-confirmed: the real native doesn't either).
static SQRESULT Script_ForceEnergizeState(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	SQInteger newState = 0;
	sq_getinteger(v, 2, &newState);

	const EnergizeWeaponConfig& config = GetEnergizeConfig(GetWeaponClassNameRaw(pWeapon));
	if (!config.hasEnergized)
	{
		Warning(eDLL_T::SERVER,
			"ForceEnergizeState: Cannot force energize state on weapon, It's not an energize weapon\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	EnergizeInstanceState& inst = GetInstanceState(v, pWeapon);
	inst.energizeState = static_cast<int>(newState);
	SyncNetworkedFields(pWeapon, inst);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_ForceEnergizedEndTime(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	SQFloat newEndTime = 0.0f;
	sq_getfloat(v, 2, &newEndTime);

	const EnergizeWeaponConfig& config = GetEnergizeConfig(GetWeaponClassNameRaw(pWeapon));
	if (!config.hasEnergized)
	{
		Warning(eDLL_T::SERVER,
			"ForceEnergizedEndTime: Cannot force energize end time on weapon, It's not an energize weapon\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	EnergizeInstanceState& inst = GetInstanceState(v, pWeapon);
	inst.energizedEndTime = static_cast<float>(newEndTime);
	SyncNetworkedFields(pWeapon, inst);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// -confirmed formula: (endTime - now) / duration, 0-guarded, NO max(x,0)
// clamp (the native doesn't clamp either -- callers that need [0,1] already
// clamp themselves, e.g. this bridge's own FSM tick).
static SQRESULT Script_GetEnergizeFrac(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	const EnergizeWeaponConfig& config = GetEnergizeConfig(GetWeaponClassNameRaw(pWeapon));
	if (config.energizedDuration == 0.0f)
	{
		sq_pushfloat(v, 0.0f);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const EnergizeInstanceState& inst = GetInstanceState(v, pWeapon);
	const float curTime = gpGlobals ? gpGlobals->curTime : 0.0f;
	sq_pushfloat(v, (inst.energizedEndTime - curTime) / config.energizedDuration);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsEnergizeWeapon(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushbool(v, GetEnergizeConfig(GetWeaponClassNameRaw(pWeapon)).hasEnergized);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Bundled bonus: honest stubs, see EnergizeInstanceState comment.
static SQRESULT Script_IsWeaponActivelyFiring(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	static bool s_warnedOnce = false;
	if (!s_warnedOnce)
	{
		s_warnedOnce = true;
		Warning(eDLL_T::SERVER,
			"[Energize] IsWeaponActivelyFiring STUB invoked (always false) -- "
			"real firing-state signal not wired, see energize.cpp\n");
	}

	sq_pushbool(v, GetInstanceState(v, pWeapon).isActivelyFiringStub);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_NeedsRechambering(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	static bool s_warnedOnce = false;
	if (!s_warnedOnce)
	{
		s_warnedOnce = true;
		Warning(eDLL_T::SERVER,
			"[Energize] NeedsRechambering STUB invoked (always false) -- "
			"real rechamber-state signal not wired, see energize.cpp\n");
	}

	sq_pushbool(v, GetInstanceState(v, pWeapon).needsRechamberingStub);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void EnergizeBridge_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	DevMsg(eDLL_T::SERVER, "[Energize] Registering weapon energize functions\n");

	weaponStruct->AddFunction("GetEnergizeState", "Script_GetEnergizeState",
		"Returns the current energize state (0=none, 1=energizing, 2=energized)", "int", "", false,
		Script_GetEnergizeState);

	weaponStruct->AddFunction("GetLastEnergizeState", "Script_GetLastEnergizeState",
		"Returns the energize state as of the previous transition", "int", "", false,
		Script_GetLastEnergizeState);

	weaponStruct->AddFunction("GetEnergizedEndTime", "Script_GetEnergizedEndTime",
		"Returns the time the current energized window ends", "float", "", false,
		Script_GetEnergizedEndTime);

	weaponStruct->AddFunction("ForceEnergizeState", "Script_ForceEnergizeState",
		"Force-sets the energize state (debug/override -- does not fire callbacks or mod-swap)",
		"void", "int newEnergizeState", false, Script_ForceEnergizeState);

	weaponStruct->AddFunction("ForceEnergizedEndTime", "Script_ForceEnergizedEndTime",
		"Force-sets the energized end time (debug/override)", "void", "float newEnergizedEndTime",
		false, Script_ForceEnergizedEndTime);

	weaponStruct->AddFunction("GetEnergizeFrac", "Script_GetEnergizeFrac",
		"get remaining energe for the energized weapon", "float", "", false, Script_GetEnergizeFrac);

	weaponStruct->AddFunction("IsEnergizeWeapon", "Script_IsEnergizeWeapon",
		"Returns true if this is a weapon that can be energized", "bool", "", false,
		Script_IsEnergizeWeapon);

	weaponStruct->AddFunction("IsWeaponActivelyFiring", "Script_IsWeaponActivelyFiring",
		"Returns whether the weapon is actively firing", "bool", "", false,
		Script_IsWeaponActivelyFiring);

	weaponStruct->AddFunction("NeedsRechambering", "Script_NeedsRechambering",
		"Returns true if the weapon needs to be rechambered", "bool", "", false,
		Script_NeedsRechambering);
}


// Per-tick FSM on activeWeapons[0..2] that are hasEnergized or mid-FSM.
static constexpr uintptr_t kServerInventoryOffset = 0x1688;

// Must match client net_bridge_core.cpp kS21ExtraFlag_StartEnergize (0x80).
// No shared header across the two repos/binaries -- duplicated constant.
static constexpr uint8_t kS21ExtraFlag_StartEnergize = 0x80;

struct EnergizeTriggerState
{
	bool lastRawEnergizeBit = false;
};
static SDKEntityMap<EnergizeTriggerState> s_energizeTriggerServer(ESide::Server, "energize.trig");

// Collect up to 3 activeWeapons that have hasEnergized or are mid-FSM.
static int CollectEnergizeWeapons(void* pPlayer, void** outWeapons, int maxOut)
{
	if (!pPlayer || !outWeapons || maxOut <= 0)
		return 0;

	const WeaponInventory* const pInv = reinterpret_cast<const WeaponInventory*>(
		reinterpret_cast<const uint8_t*>(pPlayer) + kServerInventoryOffset);

	int n = 0;
	for (int slot = 0; slot < 3 && n < maxOut; slot++)
	{
		SDKEntityHandle h(static_cast<uint32_t>(pInv->activeWeapons[slot].ToInt()));
		if (!h.IsValid())
			continue;

		void* const pWeapon = SDKEntityState_Resolve(h, ESide::Server);
		if (!pWeapon)
			continue;

		const bool hasCfg = GetEnergizeConfig(GetWeaponClassNameRaw(pWeapon)).hasEnergized;
		const EnergizeInstanceState* const st = s_energizeStatesServer.Find(pWeapon);
		const int state = st ? st->energizeState : ENERGIZE_NONE;
		if (!hasCfg && state == ENERGIZE_NONE)
			continue;

		// Dedup if the same handle lands in two slots (shouldn't, but cheap).
		bool already = false;
		for (int i = 0; i < n; i++)
		{
			if (outWeapons[i] == pWeapon)
			{
				already = true;
				break;
			}
		}
		if (already)
			continue;

		outWeapons[n++] = pWeapon;
	}
	return n;
}

// HaveEnoughConsumables -> OnWeaponTryEnergize. TWO args on this build: push
// m_hScriptInstance for weapon and player (nparams=3 = this + 2). A third
// suppressAnnouncements arg is a hard "wrong number of parameters" VM error.
static bool InvokeTryEnergize(const EnergizeWeaponConfig& config,
	HSCRIPT hWeaponScript, HSCRIPT hPlayerScript)
{
	ScriptVariant_t args[2] = { hWeaponScript, hPlayerScript };
	ScriptVariant_t ret;
	if (!InvokeGlobalClosure(config.cbTryEnergize, args, 2, &ret))
		return false;

	return ret.m_type == FIELD_BOOLEAN && ret.m_bool;
}

// Apply the ENERGIZED edge (AddMod + costConsumable callback) and leave
// last == ENERGIZED. MUST complete in the same Sync as state=ENERGIZED --
// see TickWeaponEnergize comment on the Quickchat spam failure mode.
static void ApplyEnergizedEdge(void* pWeapon, EnergizeInstanceState& inst,
	const EnergizeWeaponConfig& config, HSCRIPT hWeaponScript, HSCRIPT hPlayerScript)
{
	const bool costConsumable = !inst.energizeValidated;

	// Re-validate on the completion edge: a charge started on a now-invalid weapon aborts.
	if (costConsumable && !InvokeTryEnergize(config, hWeaponScript, hPlayerScript))
	{
		inst.energizedEndTime = inst.wasEnergizedWhenStartEnergizing
			? inst.lastEnergizedEndTime
			: ((gpGlobals ? gpGlobals->curTime : 0.0f) - 1.0f);
	}

	if (!WeaponBridge_InvokeChangeMod(hWeaponScript, "energized", /*isAdding=*/true))
		Warning(eDLL_T::SERVER, "[Energize] AddMod('energized') FAILED weapon=%p\n", pWeapon);

	ScriptVariant_t startedArgs[3] = {
		hWeaponScript, hPlayerScript, ScriptVariant_t(costConsumable)
	};
	InvokeGlobalClosure(config.cbEnergizedStart, startedArgs, 3, nullptr);

	// S21 ENERGIZED edge arms the re-energize gate off the weapon's own
	// ready time, so a completed charge cannot be immediately re-fed.
	float nextReady = 0.0f;
	Energize_GetWeaponFloat(pWeapon, "m_nextReadyTime", &nextReady);
	inst.nextEnergizeReadyTime = nextReady + sdk_energize_next_cooldown.GetFloat();

	inst.energizeValidated = true;
	inst.wasEnergizedWhenStartEnergizing = false;
	inst.lastEnergizeState = ENERGIZE_ENERGIZED;
	// Seed fire-edge detector so a pre-existing lastPrimaryAttack does not
	// immediately consume a shot of the fresh charge pool.
	inst.lastSeenPrimaryAttack = GetWeaponLastPrimaryAttack(pWeapon);
	inst.lastSeenPrimaryAttackValid = true;
	SyncNetworkedFields(pWeapon, inst);
}

// Apply the exit-to-NONE edge (RemoveMod + end callback). Same-tick with
// state=NONE so the client never sees a lingering ENERGIZED+last mismatch.
static void ApplyNoneEdge(void* pPlayer, void* pWeapon, EnergizeInstanceState& inst,
	const EnergizeWeaponConfig& config, HSCRIPT hWeaponScript, HSCRIPT hPlayerScript)
{
	// NONE edge has no exit activity. End the charge by removing the energized mod.
	(void)pPlayer;

	WeaponBridge_InvokeChangeMod(hWeaponScript, "energized", /*isAdding=*/false);

	ScriptVariant_t endArgs[2] = { hWeaponScript, hPlayerScript };
	InvokeGlobalClosure(config.cbEnergizedEnd, endArgs, 2, nullptr);

	inst.energizeValidated = false;
	inst.wasEnergizedWhenStartEnergizing = false;
	inst.lastSeenPrimaryAttackValid = false;
	inst.lastEnergizeState = ENERGIZE_NONE;
	SyncNetworkedFields(pWeapon, inst);
}

// Wind-up abort mask from CheckForEnergize. Sprint (IN_SPEED) is not a cancel.
static constexpr uint32_t kCancelMask_Base = 0x00041001u; // attack | reload | melee
static constexpr uint32_t kCancelMask_Ads  = 0x00030000u; // zoom | zoom_toggle
static constexpr uint32_t kBtn_Speed       = 1u << 15;    // sprint -- NOT in S21 mask

static ConVar sdk_energize_cancel_on_input("sdk_energize_cancel_on_input", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Abort an energize wind-up on attack/reload/melee (and ADS when both "
	"zoomEffects and sdk_energize_cancel_on_ads are set). 0 = wind-up is "
	"uninterruptible by the input mask.");

// ADS cancel is only valid against the real AE_WPN_ENERGIZED wind-up, not a pose substitute.
static ConVar sdk_energize_cancel_on_ads("sdk_energize_cancel_on_ads", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Also OR the ADS bits into the energize cancel mask when the weapon has "
	"zoomEffects. Default 0: bridge wind-up timer is still KV-based and longer "
	"than S21, so hard ADS cancel mid-window desyncs.");

// CancelEnergize also has four weapon-lifecycle callers besides the button mask.
static ConVar sdk_energize_cancel_on_holster("sdk_energize_cancel_on_holster", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Abort an energize wind-up when the weapon stops being the deployed main "
	"weapon (holster, swap, deactivate) -- S21 "
	"Lower/Holster/Deploy/Deactivate -> CancelEnergize path. 0 = input mask only.");

// Sprint is not a cancel: IN_SPEED is absent from both abort masks.
static ConVar sdk_energize_cancel_on_sprint("sdk_energize_cancel_on_sprint", "0",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"non-ship: also abort an energize wind-up on IN_SPEED (sprint). The S21 "
	"client does NOT cancel on sprint, so 1 makes the two sides disagree.");

static ConVar sdk_energize_cancel_reset_anim("sdk_energize_cancel_reset_anim", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"On cancel, hand the weapon's ideal activity back to ACT_VM_IDLE. Needed "
	"because the bridge authors the charge pose server-side; 0 leaves the "
	"viewmodel pinned mid-charge.");

// Main weapon is activeWeapons[0] (active inventory slot 0).
static bool Energize_IsDeployedWeapon(void* pPlayer, const void* pWeapon)
{
	if (!pPlayer || !pWeapon)
		return true;   // cannot tell -- never cancel on a failed read

	const WeaponInventory* const pInv = reinterpret_cast<const WeaponInventory*>(
		reinterpret_cast<const uint8_t*>(pPlayer) + kServerInventoryOffset);

	const SDKEntityHandle h(static_cast<uint32_t>(pInv->activeWeapons[0].ToInt()));
	if (!h.IsValid())
		return true;

	const void* const pActive = SDKEntityState_Resolve(h, ESide::Server);
	if (!pActive)
		return true;

	return pActive == pWeapon;
}

// IsHolstering: HOLSTER or LOWER (weapstate 2/4; ordinals 0..7 match S3).
// LOWER time guard and m_isHolstering are not networked here; test LOWER bare.
// Engine Lower cancels energize unconditionally.
static constexpr int kWeapState_Holster = 2;
static constexpr int kWeapState_Lower   = 4;

static bool Energize_IsHolstering(void* pWeapon)
{
	int weapState = 0;
	if (!Energize_GetWeaponInt(pWeapon, "m_weapState", &weapState))
		return false;

	return weapState == kWeapState_Holster || weapState == kWeapState_Lower;
}

// C_WeaponX::GetFireRateDelay = m_modVars.fireDuration + (fireRate ? 1/fireRate: 0).
static float Energize_GetFireRateDelay(const EnergizeWeaponConfig& config)
{
	return config.fireDuration
		+ (config.fireRate != 0.0f ? (1.0f / config.fireRate) : 0.0f);
}

// 1p charge pose is authored server-side (m_idealSequence / m_idealPlaybackRate).
static void Energize_ReleaseChargePose(void* pWeapon)
{
	if (!sdk_energize_cancel_reset_anim.GetBool() || !pWeapon)
		return;

	Energize_ResolveSetIdealWeaponActivity();
	if (!v_SetIdealWeaponActivity)
		return;

	const int idleAct = FindActivityByName("ACT_VM_IDLE");
	if (idleAct < 0)
	{
		Warning(eDLL_T::SERVER,
			"[Energize] ACT_VM_IDLE unresolved -- charge pose left pinned after cancel\n");
		return;
	}

	v_SetIdealWeaponActivity(pWeapon, static_cast<unsigned int>(idleAct));
}

static ConVar sdk_energize_entry_gates("sdk_energize_entry_gates", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Apply the S21 CanStartEnergize timing gates before entering a wind-up. "
	"0 = skip the gates (a cancel can be undone on the next tick).");

// CanStartEnergize timing half plus the two remaining compares.
static bool Energize_CanStart(void* pWeapon, const EnergizeInstanceState& inst, float now)
{
	if (!sdk_energize_entry_gates.GetBool())
		return true;

	if (now < inst.nextEnergizeReadyTime)
	{
		DevMsg(eDLL_T::SERVER, "[Energize] start refused -- re-energize cooldown "
			"(now=%.3f nextEnergizeReady=%.3f)\n", now, inst.nextEnergizeReadyTime);
		return false;
	}

	float nextReady = 0.0f;
	float nextPrimary = 0.0f;
	Energize_GetWeaponFloat(pWeapon, "m_nextReadyTime", &nextReady);
	Energize_GetWeaponFloat(pWeapon, "m_nextPrimaryAttackTime", &nextPrimary);

	const float readyAt = (nextReady > nextPrimary) ? nextReady : nextPrimary;
	if (now < readyAt)
	{
		DevMsg(eDLL_T::SERVER, "[Energize] start refused -- weapon not ready "
			"(now=%.3f nextReady=%.3f nextPrimary=%.3f)\n", now, nextReady, nextPrimary);
		return false;
	}

	// The dedi field carries S3 ordinals; translate before comparing against the
	// S21 constant, exactly as the wire proxy does. (The holster ordinals below
	// need no translation -- 0..7 are identical on both sides.)
	int weapStateS3 = 0;
	if (Energize_GetWeaponInt(pWeapon, "m_weapState", &weapStateS3)
		&& WeapState_S3ToS21(weapStateS3) == 14)
	{
		DevMsg(eDLL_T::SERVER, "[Energize] start refused -- rechambering\n");
		return false;
	}

	if (Energize_IsHolstering(pWeapon))
	{
		DevMsg(eDLL_T::SERVER, "[Energize] start refused -- holstering\n");
		return false;
	}

	return true;
}

// CancelEnergize abort tail -- also the body its four lifecycle callers reach.
static bool Energize_Cancel(void* pPlayer, void* pWeapon, EnergizeInstanceState& inst,
	const EnergizeWeaponConfig& config, HSCRIPT hWeaponScript, HSCRIPT hPlayerScript,
	const char* reason)
{
	// S21 gate: hasEnergized && state == ENERGIZING (no IsPredicted on dedi).
	if (!config.hasEnergized || inst.energizeState != ENERGIZE_ENERGIZING)
		return false;

	const float now = gpGlobals ? gpGlobals->curTime : 0.0f;
	const float fireRateDelay = Energize_GetFireRateDelay(config);
	const float nextReady = now + fireRateDelay;

	inst.chargeAnimPlayed = false;

	if (inst.wasEnergizedWhenStartEnergizing)
	{
		// Aborted re-feed: fall back into the window the previous charge had
		// rather than dropping the buff the player already paid for.
		inst.energizeState = ENERGIZE_ENERGIZED;
		inst.energizedEndTime = inst.lastEnergizedEndTime;
		ApplyEnergizedEdge(pWeapon, inst, config, hWeaponScript, hPlayerScript);
	}
	else
	{
		// S21 also calls StopWeaponParticleEffect here; the 1p/3p charge FX
		// are client-side and the S21 client stops them itself on its own
		// NONE edge once m_energizeState lands.
		inst.energizeState = ENERGIZE_NONE;
		ApplyNoneEdge(pPlayer, pWeapon, inst, config, hWeaponScript, hPlayerScript);
	}

	const bool armedReady  = Energize_SetWeaponFloat(pWeapon, "m_nextReadyTime", nextReady);
	const bool armedReload = Energize_SetWeaponBool(pWeapon, "m_needsReloadCheck", true);
	// CheckForEnergize's own tail after the abort: SetWeaponIdleTime(now).
	const bool armedIdle   = Energize_SetWeaponFloat(pWeapon, "m_flTimeWeaponIdle", now);

	inst.nextEnergizeReadyTime = nextReady + sdk_energize_next_cooldown.GetFloat();

	Energize_ReleaseChargePose(pWeapon);

	Msg(eDLL_T::SERVER,
		"[Energize] CANCEL (%s) weapon=%p state->%d now=%.3f nextReady=%.3f (+%.3f) "
		"nextEnergizeReady=%.3f armed[ready=%d reload=%d idle=%d]\n",
		reason, pWeapon, inst.energizeState, now, nextReady, fireRateDelay,
		inst.nextEnergizeReadyTime, armedReady ? 1 : 0, armedReload ? 1 : 0,
		armedIdle ? 1 : 0);

	if (!armedReady)
		Warning(eDLL_T::SERVER,
			"[Energize] m_nextReadyTime could not be armed -- the client is free to "
			"re-assert startEnergize and the wind-up will restart\n");

	return true;
}

// Which abort applies this tick, or nullptr to keep charging.
static const char* Energize_GetCancelReason(void* pPlayer, void* pWeapon,
	const EnergizeWeaponConfig& config, uint32_t buttons,
	HSCRIPT hWeaponScript, HSCRIPT hPlayerScript)
{
	uint32_t mask = 0;
	if (sdk_energize_cancel_on_input.GetBool())
	{
		mask = kCancelMask_Base;
		if (config.zoomEffects && sdk_energize_cancel_on_ads.GetBool())
			mask |= kCancelMask_Ads;
		if (sdk_energize_cancel_on_sprint.GetBool())
			mask |= kBtn_Speed;
	}

	int weapState = -1;
	Energize_GetWeaponInt(pWeapon, "m_weapState", &weapState);
	const bool deployed = Energize_IsDeployedWeapon(pPlayer, pWeapon);

	// Trace every observed-state transition during wind-up, not just button edges.
	{
		static uint32_t s_lastButtons = 0;
		static int s_lastWeapState = -2;
		static bool s_lastDeployed = true;
		static int s_traceN = 0;
		if ((buttons != s_lastButtons || weapState != s_lastWeapState || deployed != s_lastDeployed)
			&& s_traceN < 80)
		{
			++s_traceN;
			Msg(eDLL_T::SERVER,
				"[Energize] wind-up buttons=0x%08X (was 0x%08X) mask=0x%08X hit=0x%08X "
				"weapState=%d (was %d) deployed=%d\n",
				buttons, s_lastButtons, mask, buttons & mask,
				weapState, s_lastWeapState, deployed ? 1 : 0);
			s_lastButtons = buttons;
			s_lastWeapState = weapState;
			s_lastDeployed = deployed;
		}
	}

	const uint32_t hit = buttons & mask;
	if (hit)
	{
		if (hit & (1u << 18))      return "melee";
		if (hit & kCancelMask_Ads) return "ads";
		if (hit & (1u << 0))       return "attack";
		if (hit & (1u << 12))      return "reload";
		return "sprint";
	}

	if (sdk_energize_cancel_on_holster.GetBool())
	{
		// S21 HolsterInternal and Lower both call CancelEnergize; the weapon's
		// own state is the signal for both, and unlike the inventory slot it is a
		// per-weapon fact we resolve by prop name.
		if (weapState == kWeapState_Holster || weapState == kWeapState_Lower)
			return "holstering";

		// Covers the rest of the lifecycle set (Deploy/Deactivate, or a swap the
		// weapon's own state never reflected).
		if (!deployed)
			return "undeployed";
	}

	// Losing the consumable mid-charge aborts too. Announcements suppressed --
	// the press already announced, and this runs every tick.
	if (!InvokeTryEnergize(config, hWeaponScript, hPlayerScript))
		return "no-consumable";

	return nullptr;
}

// Per-weapon FSM tick.
static void TickWeaponEnergize(void* pPlayer, void* pWeapon, bool triggerPressed,
	uint32_t buttons, float now)
{
	const EnergizeWeaponConfig& config = GetEnergizeConfig(GetWeaponClassNameRaw(pWeapon));
	if (!config.hasEnergized && s_energizeStatesServer[pWeapon].energizeState == ENERGIZE_NONE)
		return;

	EnergizeInstanceState& inst = s_energizeStatesServer[pWeapon];

	const HSCRIPT hPlayerScript = reinterpret_cast<CBaseEntity*>(pPlayer)->GetScriptInstance();
	const HSCRIPT hWeaponScript = reinterpret_cast<CBaseEntity*>(pWeapon)->GetScriptInstance();
	if (!hPlayerScript || !hWeaponScript)
		return;

	// --- Residual edges (not the normal ENERGIZING entry path) ---
	if (inst.energizeState != inst.lastEnergizeState
		&& inst.energizeState != ENERGIZE_ENERGIZING)
	{
		if (inst.energizeState == ENERGIZE_ENERGIZED)
		{
			ApplyEnergizedEdge(pWeapon, inst, config, hWeaponScript, hPlayerScript);
			return;
		}
		// state == NONE, last residual non-NONE
		ApplyNoneEdge(pPlayer, pWeapon, inst, config, hWeaponScript, hPlayerScript);
		return;
	}

	// --- Press to start / re-feed ---
	if (triggerPressed
		&& config.hasEnergized
		&& inst.energizeState != ENERGIZE_ENERGIZING
		&& (inst.energizeState != ENERGIZE_ENERGIZED || config.canEnergizeWhenEnergized)
		&& Energize_CanStart(pWeapon, inst, now))
	{
		if (InvokeTryEnergize(config, hWeaponScript, hPlayerScript))
		{
			if (inst.energizeState == ENERGIZE_ENERGIZED)
			{
				inst.wasEnergizedWhenStartEnergizing = true;
				inst.lastEnergizedEndTime = inst.energizedEndTime;
			}
			else
			{
				inst.wasEnergizedWhenStartEnergizing = false;
			}

			// Enter ENERGIZING, play charge anim NOW, leave last lagging until
			// a later *server tick* (not the next usercmd in this batch).
			inst.energizeState = ENERGIZE_ENERGIZING;
			inst.startEnergizingTime = now;
			inst.energizingEnteredTick = gpGlobals ? gpGlobals->tickCount : 0;
			inst.chargeAnimPlayed = Energize_PlayChargeAnim(pPlayer, pWeapon);
			inst.energizeValidated = true;

			ScriptVariant_t startArgs[2] = { hWeaponScript, hPlayerScript };
			InvokeGlobalClosure(config.cbStartEnergizing, startArgs, 2, nullptr);

			// last intentionally NOT set to ENERGIZING here.
			SyncNetworkedFields(pWeapon, inst);
			Warning(eDLL_T::SERVER,
				"[Energize] ENTER ENERGIZING weapon=%p last=%d tick=%d animPlayed=%d "
				"(last catch-up deferred to next server tick)\n",
				pWeapon, inst.lastEnergizeState,
				inst.energizingEnteredTick, inst.chargeAnimPlayed ? 1 : 0);
			return;
		}
	}

	// --- Steady-state advances ---
	if (inst.energizeState == ENERGIZE_ENERGIZING)
	{
		// Catch up last after at least one server tick so a snapshot has gone
		// out with last lagging (client native SendWeaponAnim edge).
		if (inst.lastEnergizeState != ENERGIZE_ENERGIZING
			&& gpGlobals
			&& gpGlobals->tickCount > inst.energizingEnteredTick)
		{
			if (!inst.chargeAnimPlayed)
				inst.chargeAnimPlayed = Energize_PlayChargeAnim(pPlayer, pWeapon);

			inst.lastEnergizeState = ENERGIZE_ENERGIZING;
			SyncNetworkedFields(pWeapon, inst);
			Warning(eDLL_T::SERVER,
				"[Energize] last catch-up ENERGIZING weapon=%p tick=%d (client edge window closed)\n",
				pWeapon, gpGlobals->tickCount);
		}

		// Abort the wind-up on any of the inputs S21 cancels on, when the
		// weapon stops being deployed (S21 Lower/Holster/Deploy/Deactivate
		// route), or when the consumable disappears mid-charge.
		const char* const cancelReason = Energize_GetCancelReason(
			pPlayer, pWeapon, config, buttons, hWeaponScript, hPlayerScript);

		if (cancelReason)
		{
			Energize_Cancel(pPlayer, pWeapon, inst, config,
				hWeaponScript, hPlayerScript, cancelReason);
			return;
		}

		const float activityTime = config.energizeActivityTime > 0.0f
			? config.energizeActivityTime
			: 1.0f;
		if (now - inst.startEnergizingTime >= activityTime)
		{
			// COMPLETE IN ONE SYNC: state=ENERGIZED + AddMod + last=ENERGIZED.
			// Never leave last=ENERGIZING on the wire while state is ENERGIZED.
			if (inst.lastEnergizeState != ENERGIZE_ENERGIZING)
			{
				// Timer fired before tick catch-up -- force last to ENERGIZING
				// only for the duration of ApplyEnergizedEdge's own last write
				// (it sets last=ENERGIZED). Safe: same Sync, no intermediate snap.
				inst.lastEnergizeState = ENERGIZE_ENERGIZING;
			}
			inst.energizeState = ENERGIZE_ENERGIZED;
			inst.energizeValidated = false; // costConsumable = true on the edge
			inst.energizedEndTime = now + config.energizedDuration;
			inst.chargeAnimPlayed = false;
			ApplyEnergizedEdge(pWeapon, inst, config, hWeaponScript, hPlayerScript);
			return;
		}
	}
	else if (inst.energizeState == ENERGIZE_ENERGIZED)
	{
		// Per-shot pool drain: each engine primary-fire while ENERGIZED subtracts the pool cost.
		if (config.energizedTimeConsumedPerShot > 0.0f)
		{
			const float engineLastAttack = GetWeaponLastPrimaryAttack(pWeapon);
			if (!inst.lastSeenPrimaryAttackValid)
			{
				inst.lastSeenPrimaryAttack = engineLastAttack;
				inst.lastSeenPrimaryAttackValid = true;
			}
			else if (engineLastAttack > inst.lastSeenPrimaryAttack + 0.0001f)
			{
				inst.lastSeenPrimaryAttack = engineLastAttack;
				inst.energizedEndTime -= config.energizedTimeConsumedPerShot;
				SyncNetworkedFields(pWeapon, inst);
			}
		}

		if (now >= inst.energizedEndTime)
		{
			inst.energizeState = ENERGIZE_NONE;
			ApplyNoneEdge(pPlayer, pWeapon, inst, config, hWeaponScript, hPlayerScript);
			return;
		}
	}
}

void EnergizeBridge_Think(void* pPlayer, void* pUserCmd)
{
	CmdChain_Bump(CMDCHAIN_TICK_ENERGIZE);

	static bool s_thinkReached = false;
	if (!s_thinkReached)
	{
		s_thinkReached = true;
		Warning(eDLL_T::SERVER, "[Energize] EnergizeBridge_Think reached for the first time "
			"(hook alive, called from CPlayerMove::StaticRunCommand)\n");
	}

	if (!sdk_energize_bridge.GetBool())
		return;

	if (!pPlayer || !pUserCmd || !g_pServerScript || !gpGlobals)
		return;

	// Rising edge of the smuggled startEnergize bit (impulse top bit).
	const CUserCmd* const cmd = reinterpret_cast<const CUserCmd*>(pUserCmd);
	const bool rawEnergizeBit = (cmd->impulse & kS21ExtraFlag_StartEnergize) != 0;

	EnergizeTriggerState& trigState = s_energizeTriggerServer[pPlayer];
	const bool triggerPressed = rawEnergizeBit && !trigState.lastRawEnergizeBit;
	trigState.lastRawEnergizeBit = rawEnergizeBit;

	const uint32_t buttons = static_cast<uint32_t>(cmd->buttons);
	const float now = gpGlobals->curTime;

	// Opt-in: first-sighting button bits at the far end of the usercmd wire.
	// Max 32 lines per process when on; off by default.
	if (bridge_sv_btn_trace.GetBool())
	{
		static const char* const s_btnNames[32] = {
			"ATTACK","JUMP","DUCK","FORWARD","BACK","USE","PAUSEMENU","LEFT",
			"RIGHT","MOVELEFT","MOVERIGHT","WALK","RELOAD","USE_LONG","WEAPON_DISCARD","SPEED",
			"ZOOM","ZOOM_TOGGLE","MELEE","WEAPON_CYCLE","OFFHAND0","OFFHAND1","OFFHAND2","OFFHAND3",
			"OFFHAND4","OFFHAND_QUICK","DUCKTOGGLE","USE_AND_RELOAD","DODGE","VARIABLE_SCOPE_TOGGLE",
			"PING","USE_ALT" };
		static uint32_t s_btnSeen = 0;
		const uint32_t fresh = buttons & ~s_btnSeen;
		if (fresh)
		{
			s_btnSeen |= fresh;
			for (int b = 0; b < 32; ++b)
			{
				if (fresh & (1u << b))
					Msg(eDLL_T::SERVER, "[SV-BTN] first sighting: bit %d IN_%s (buttons=0x%08X)\n",
						b, s_btnNames[b], buttons);
			}
		}
	}

	void* weapons[3] = {};
	const int nWeapons = CollectEnergizeWeapons(pPlayer, weapons, 3);
	for (int i = 0; i < nWeapons; i++)
		TickWeaponEnergize(pPlayer, weapons[i], triggerPressed, buttons, now);
}


void EnergizeBridge_LevelShutdown()
{
	s_energizeStatesServer.Clear();
	s_energizeStatesClient.Clear();
	s_configCache.clear();
	s_energizeTriggerServer.Clear();
}
