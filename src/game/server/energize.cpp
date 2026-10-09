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

// Mirrors the S21 client ConVar of the same name (default "0.25"). Both the
// ENERGIZED edge and the cancel push m_nextEnergizeReadyTime this far past m_nextReadyTime.
static ConVar sdk_energize_next_cooldown("sdk_energize_next_cooldown", "0.25",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Seconds added on top of m_nextReadyTime for the re-energize gate "
	"(the S21 client's nextEnergizeCooldownTime, default 0.25).");

static ConVar sdk_energize_drain_trace("sdk_energize_drain_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[Energize] log every per-shot pool drain, every cancel and the wind-up input trace.");
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

	// Absolute time the wind-up completes: AE_WPN_ENERGIZED on the charge sequence,
	// the same clock the client predicts (its weapon+0x2EAC). 0 = not armed yet.
	float energizedSeqEventTime = 0.0f;

	// m_lastEnergizeState is an edge detector. Leaving last at ENERGIZING re-fires CheckForEnergize every snapshot.
	int energizingEnteredTick = -1;
	bool chargeAnimPlayed = false;

	// Ideal activity the charge set; anything else mid-wind-up means the pose was taken.
	int chargeIdealActivity = -1;
	bool chargePoseLossReported = false;
	bool holsterReported = false;

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

static bool Energize_SetWeaponInt(void* pWeapon, const char* propName, int value)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(pWeapon) + off) = value;
	MarkEntityEdictDirty(pWeapon);
	return true;
}

// Charge anim: ACT_VM_ENERGIZE via WeaponCustomAct_ServerExecuteByName. Do not re-translate a dedi id.
static constexpr ptrdiff_t kWpnOffStudioHdr      = 0xFD8;
static constexpr ptrdiff_t kWpnOffIdealSequence  = 0x1214;
static constexpr ptrdiff_t kWpnOffIdealActivity  = 0x1218;
static constexpr ptrdiff_t kWpnOffWeaponActivity = 0x121C;

typedef char (__fastcall* SetIdealWeaponActivity_t)(void* pWeapon, unsigned int activity);
static SetIdealWeaponActivity_t v_SetIdealWeaponActivity = nullptr;

// Shared by the start-sprint and keep-sprinting checks.
typedef char (__fastcall* WeaponBlocksSprint_t)(void* pPlayer, int buttons);
static WeaponBlocksSprint_t v_WeaponBlocksSprint = nullptr;

// Raw animation length with the entity's pose parameters; no weapon KV overrides.
typedef float (__fastcall* StudioSequenceDuration_t)(void* pEntity, void* pStudioHdr, int sequence);
static StudioSequenceDuration_t v_StudioSequenceDuration = nullptr;

// Must resolve in GetFun: translocation detours this function, and a scan after
// attach sees the jmp instead of the prologue. Calls go through that hook.
void VEnergize::GetFun(void) const
{
	// Opens by testing the weapon's studio header at +0xFD8.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 7C 24 10 55 48 8B EC 48 83 EC 70 "
		"48 83 B9 D8 0F 00 00 00")
		.GetPtr(v_SetIdealWeaponActivity);

	if (!v_SetIdealWeaponActivity)
		Warning(eDLL_T::SERVER,
			"[Energize] SetIdealWeaponActivity pattern unresolved -- the charge "
			"anim cannot play\n");

	// Validates the studio header, then reads its virtual model at +0x10.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B F8 48 8B DA 48 8B F1 "
		"48 85 D2 74 ?? 48 8B CA E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 43 10 48 85 C0")
		.GetPtr(v_StudioSequenceDuration);

	if (!v_StudioSequenceDuration)
		Warning(eDLL_T::SERVER,
			"[Energize] SequenceDuration pattern unresolved -- the wind-up falls back "
			"to energize_activity_time and will not match the client\n");

	// Tests button bit 18, then two player fields before answering true.
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 8B C2 4C 8B C1 C1 E8 12 A8 01 74 ?? 83 B9 ?? ?? 00 00 00 7F ?? "
		"80 B9 ?? ?? 00 00 00 74 ?? B0 01")
		.GetPtr(v_WeaponBlocksSprint);

	if (!v_WeaponBlocksSprint)
		Warning(eDLL_T::SERVER,
			"[Energize] WeaponBlocksSprint pattern unresolved -- players can sprint "
			"and slide through a wind-up\n");
}

void VEnergize::GetAdr(void) const
{
	LogFunAdr("Energize_SetIdealActivity", v_SetIdealWeaponActivity);
	LogFunAdr("Energize_SequenceDuration", v_StudioSequenceDuration);
	LogFunAdr("Energize_WeaponBlocksSprint", v_WeaponBlocksSprint);
}

// Cycle of the first event named pszEvent on the sequence, or defaultCycle.
// Matched by name: the host assigns the S21 AE_WPN_* events fresh ids at load.
static constexpr int kSeqDescSize      = 0xD0;
static constexpr int kSeqEventStride   = 272;
static constexpr int kSeqEventNameOff  = 268;  // int szeventindex, event-relative
static constexpr int kMaxSeqEvents     = 512;

static uintptr_t Energize_GetSeqDesc(uintptr_t pStudioHdr, int sequence)
{
	if (!pStudioHdr || sequence < 0)
		return 0;

	const uintptr_t studiohdr = *reinterpret_cast<uintptr_t*>(pStudioHdr + 0x08);
	const uintptr_t vmodel    = *reinterpret_cast<uintptr_t*>(pStudioHdr + 0x10);

	if (vmodel)
	{
		if (sequence >= *reinterpret_cast<int*>(vmodel + 0x20))
			return 0;
		const uintptr_t seqs = *reinterpret_cast<uintptr_t*>(vmodel + 0x08);
		return seqs ? *reinterpret_cast<uintptr_t*>(seqs + 0x18 * static_cast<uintptr_t>(sequence) + 0x08) : 0;
	}

	if (!studiohdr || sequence >= *reinterpret_cast<int*>(studiohdr + 0xC0)) // numlocalseq
		return 0;

	return studiohdr + *reinterpret_cast<int*>(studiohdr + 0xC4)            // localseqindex
		+ kSeqDescSize * static_cast<uintptr_t>(sequence);
}

static float Energize_GetSeqEventCycle(uintptr_t seqdesc, const char* pszEvent, float defaultCycle,
	bool* pFound)
{
	*pFound = false;
	if (!seqdesc)
		return defaultCycle;

	const int numEvents  = *reinterpret_cast<int*>(seqdesc + 0x18);
	const int eventIndex = *reinterpret_cast<int*>(seqdesc + 0x1C);
	if (numEvents <= 0 || numEvents > kMaxSeqEvents || !eventIndex)
		return defaultCycle;

	const uintptr_t events = seqdesc + eventIndex;
	for (int i = 0; i < numEvents; i++)
	{
		const uintptr_t e = events + static_cast<uintptr_t>(i) * kSeqEventStride;
		const int nameOff = *reinterpret_cast<int*>(e + kSeqEventNameOff);
		if (!nameOff)
			continue;

		if (strcmp(reinterpret_cast<const char*>(e + nameOff), pszEvent) == 0)
		{
			*pFound = true;
			return *reinterpret_cast<float*>(e);
		}
	}
	return defaultCycle;
}

static bool Energize_PlayChargeAnim(void* pPlayer, void* pWeapon)
{
	const int actId = FindActivityByName("ACT_VM_ENERGIZE");

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
	}

	return idealOk != 0;
}

// The client's ENERGIZING edge: complete at AE_WPN_ENERGIZED, held until AE_WPN_READYTOFIRE,
// both fractions of the charge sequence (missing = full length), as the client times it.
static void Energize_ArmChargeClock(void* pWeapon, EnergizeInstanceState& inst,
	const EnergizeWeaponConfig& config, bool chargeAnimPlayed)
{
	const float start = inst.startEnergizingTime;
	const float kvTime = config.energizeActivityTime > 0.0f ? config.energizeActivityTime : 1.0f;

	uint8_t* const w = reinterpret_cast<uint8_t*>(pWeapon);
	const uintptr_t pStudioHdr = *reinterpret_cast<uintptr_t*>(w + kWpnOffStudioHdr);
	const int sequence = *reinterpret_cast<int*>(w + kWpnOffIdealSequence);

	const uintptr_t seqdesc = Energize_GetSeqDesc(pStudioHdr, sequence);
	if (!chargeAnimPlayed || !v_StudioSequenceDuration || !seqdesc)
	{
		inst.energizedSeqEventTime = start + kvTime;
		static int s_budget = 8;
		if (s_budget-- > 0)
			Warning(eDLL_T::SERVER,
			"[Energize] wind-up clock on energize_activity_time %.3f (anim=%d duration=%d hdr=%p) "
			"-- the client times it off the charge sequence, expect a mismatch\n",
			kvTime, chargeAnimPlayed ? 1 : 0, v_StudioSequenceDuration ? 1 : 0,
			reinterpret_cast<void*>(pStudioHdr));
		return;
	}

	const float duration = v_StudioSequenceDuration(pWeapon, reinterpret_cast<void*>(pStudioHdr), sequence);

	bool foundEnergized = false, foundReady = false;
	const float energizedCycle = Energize_GetSeqEventCycle(seqdesc, "AE_WPN_ENERGIZED", 1.0f, &foundEnergized);
	const float readyCycle     = Energize_GetSeqEventCycle(seqdesc, "AE_WPN_READYTOFIRE", 1.0f, &foundReady);

	inst.energizedSeqEventTime = start + energizedCycle * duration;

	const float readyTime = start + readyCycle * duration;
	Energize_SetWeaponFloat(pWeapon, "m_nextReadyTime", readyTime);
	Energize_SetWeaponFloat(pWeapon, "m_flTimeWeaponIdle", readyTime);
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
	// Weapon this player is winding up. The FSM only ticks held weapons, so a
	// swap that skips HOLSTER would otherwise leave it ENERGIZING unticked.
	SDKEntityHandle hWindUpWeapon;
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
	inst.energizedSeqEventTime = 0.0f;
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
	inst.energizedSeqEventTime = 0.0f;
	inst.lastEnergizeState = ENERGIZE_NONE;
	SyncNetworkedFields(pWeapon, inst);
}

// Wind-up abort mask from CheckForEnergize. Sprint (IN_SPEED) is not a cancel.
static constexpr uint32_t kCancelMask_Base = 0x00041001u; // attack | reload | melee
static constexpr uint32_t kCancelMask_Ads  = 0x00030000u; // zoom | zoom_toggle
static constexpr uint32_t kBtn_Speed       = 1u << 15;    // sprint -- NOT in S21 mask

static ConVar sdk_energize_cancel_on_input("sdk_energize_cancel_on_input", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Abort an energize wind-up on attack/reload/melee, and ADS when the weapon "
	"has zoomEffects. 0 = wind-up is uninterruptible by the input mask.");

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

// IsHolstering: HOLSTER or LOWER (weapstate 2/4; ordinals 0..7 match dedi).
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

// S21 parks the weapon in WEAP_STATE_ENERGIZE for the wind-up; dedi has no such
// state, so a transitional state left running here finishes mid-charge and
// plays its exit anim over the charge pose. IDLE is inert until
// m_flTimeWeaponIdle, which the charge clock holds to READYTOFIRE.
static constexpr int kWeapStateS3_Idle           = 0;
static constexpr int kWeapStateS3_Deploy         = 1;
static constexpr int kWeapStateS3_Raise          = 3;
static constexpr int kWeapStateS3_Sprint         = 8;
static constexpr int kWeapStateS3_Attack         = 9;
static constexpr int kWeapStateS3_CustomActivity = 12;

static void Energize_ParkWeaponState(void* pWeapon)
{
	int weapState = kWeapStateS3_Idle;
	if (!Energize_GetWeaponInt(pWeapon, "m_weapState", &weapState))
		return;

	// SPRINT matters most: the sprint gate ends the sprint on the next tick and
	// the weapon would play its raise-from-sprint over the charge.
	if (weapState != kWeapStateS3_Deploy && weapState != kWeapStateS3_Raise
		&& weapState != kWeapStateS3_Sprint && weapState != kWeapStateS3_Attack
		&& weapState != kWeapStateS3_CustomActivity)
		return;

	Energize_SetWeaponInt(pWeapon, "m_weapState", kWeapStateS3_Idle);

	if (sdk_energize_drain_trace.GetBool())
		Msg(eDLL_T::SERVER, "[Energize] wind-up parks weapState %d -> IDLE weapon=%p\n",
			weapState, pWeapon);
}

// 1p charge pose is authored server-side (m_idealSequence / m_idealPlaybackRate).
static void Energize_ReleaseChargePose(void* pWeapon)
{
	if (!sdk_energize_cancel_reset_anim.GetBool() || !pWeapon)
		return;

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

	// The dedi field carries dedi ordinals; translate before comparing against the
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

	// S21 releases the weapon at once and only holds the re-energize gate.
	const float now = gpGlobals ? gpGlobals->curTime : 0.0f;
	const float nextReady = now;

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

	if (sdk_energize_drain_trace.GetBool())
		Msg(eDLL_T::SERVER,
		"[Energize] CANCEL (%s) weapon=%p state->%d now=%.3f nextReady=%.3f "
		"nextEnergizeReady=%.3f armed[ready=%d reload=%d idle=%d]\n",
		reason, pWeapon, inst.energizeState, now, nextReady,
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
		// The S21 client cancels on ADS whenever the weapon has zoom effects.
		if (config.zoomEffects)
			mask |= kCancelMask_Ads;
		if (sdk_energize_cancel_on_sprint.GetBool())
			mask |= kBtn_Speed;
	}

	int weapState = -1;
	Energize_GetWeaponInt(pWeapon, "m_weapState", &weapState);
	const bool deployed = Energize_IsDeployedWeapon(pPlayer, pWeapon);

	if (sdk_energize_drain_trace.GetBool())
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
			Energize_ParkWeaponState(pWeapon);
			inst.chargeAnimPlayed = Energize_PlayChargeAnim(pPlayer, pWeapon);
			inst.chargeIdealActivity = *reinterpret_cast<int*>(
				reinterpret_cast<uint8_t*>(pWeapon) + kWpnOffIdealActivity);
			inst.chargePoseLossReported = false;
			inst.holsterReported = false;
			Energize_ArmChargeClock(pWeapon, inst, config, inst.chargeAnimPlayed);
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
			{
				inst.chargeAnimPlayed = Energize_PlayChargeAnim(pPlayer, pWeapon);
				if (inst.chargeAnimPlayed)
				{
					inst.chargeIdealActivity = *reinterpret_cast<int*>(
						reinterpret_cast<uint8_t*>(pWeapon) + kWpnOffIdealActivity);
					Energize_ArmChargeClock(pWeapon, inst, config, true);
				}
			}

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

		if (inst.chargeAnimPlayed && !inst.chargePoseLossReported)
		{
			const uint8_t* const w = reinterpret_cast<const uint8_t*>(pWeapon);
			const int ideal = *reinterpret_cast<const int*>(w + kWpnOffIdealActivity);
			if (ideal != inst.chargeIdealActivity)
			{
				inst.chargePoseLossReported = true;
				int weapState = -1;
				Energize_GetWeaponInt(pWeapon, "m_weapState", &weapState);
				Warning(eDLL_T::SERVER,
					"[Energize] charge pose replaced mid-wind-up weapon=%p ideal %d -> %d "
					"weapState=%d at %.3fs -- the state stays ENERGIZING\n",
					pWeapon, inst.chargeIdealActivity, ideal, weapState,
					now - inst.startEnergizingTime);
			}
		}

		if (inst.energizedSeqEventTime > 0.0f && now >= inst.energizedSeqEventTime)
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
		// The per-shot drain lives in the fire path (EnergizeBridge_OnWeaponFired).
		if (now >= inst.energizedEndTime)
		{
			inst.energizeState = ENERGIZE_NONE;
			ApplyNoneEdge(pPlayer, pWeapon, inst, config, hWeaponScript, hPlayerScript);
			return;
		}
	}
}

// S21 refuses to start or keep a sprint while any held weapon is winding up,
// which also rules out sliding. The host build has no energize, so add the case here.
static bool Energize_PlayerHasWindUp(void* pPlayer)
{
	const WeaponInventory* const pInv = reinterpret_cast<const WeaponInventory*>(
		reinterpret_cast<const uint8_t*>(pPlayer) + kServerInventoryOffset);

	for (int slot = 0; slot < 3; slot++)
	{
		const SDKEntityHandle h(static_cast<uint32_t>(pInv->activeWeapons[slot].ToInt()));
		if (!h.IsValid())
			continue;

		void* const pWeapon = SDKEntityState_Resolve(h, ESide::Server);
		const EnergizeInstanceState* const st = pWeapon ? s_energizeStatesServer.Find(pWeapon) : nullptr;
		if (st && st->energizeState == ENERGIZE_ENERGIZING)
			return true;
	}
	return false;
}

static char __fastcall Hook_WeaponBlocksSprint(void* pPlayer, int buttons)
{
	if (v_WeaponBlocksSprint(pPlayer, buttons))
		return 1;

	return (pPlayer && sdk_energize_bridge.GetBool() && Energize_PlayerHasWindUp(pPlayer)) ? 1 : 0;
}

void VEnergize::Detour(const bool bAttach) const
{
	if (v_WeaponBlocksSprint)
		DetourSetup(&v_WeaponBlocksSprint, &Hook_WeaponBlocksSprint, bAttach);
}

void EnergizeBridge_OnHolster(void* pWeapon)
{
	EnergizeInstanceState* const inst = pWeapon ? s_energizeStatesServer.Find(pWeapon) : nullptr;
	if (!inst || inst->energizeState != ENERGIZE_ENERGIZING || inst->holsterReported)
		return;

	inst->holsterReported = true;

	void* frames[8] = {};
	const USHORT nFrames = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
	const uintptr_t base = g_GameDll.GetModuleBase();
	const uintptr_t size = g_GameDll.GetModuleSize();

	char szStack[256] = {};
	size_t len = 0;
	for (USHORT i = 0; i < nFrames && len < sizeof(szStack) - 20; i++)
	{
		const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
		if (a < base || a >= base + size)
			continue;
		const int n = V_snprintf(szStack + len, sizeof(szStack) - len, " 0x%llX",
			static_cast<unsigned long long>(a - base + 0x140000000ull));
		if (n <= 0)
			break;
		len += static_cast<size_t>(n);
	}

	const float now = gpGlobals ? gpGlobals->curTime : 0.0f;
	Warning(eDLL_T::SERVER, "[Energize] holster during wind-up weapon=%p at %.3fs callers:%s\n",
		pWeapon, now - inst->startEnergizingTime, szStack);
}

void EnergizeBridge_OnWeaponFired(void* pWeapon)
{
	EnergizeInstanceState* const inst = pWeapon ? s_energizeStatesServer.Find(pWeapon) : nullptr;
	if (!inst || inst->energizeState != ENERGIZE_ENERGIZED)
		return;

	const EnergizeWeaponConfig& config = GetEnergizeConfig(GetWeaponClassNameRaw(pWeapon));
	inst->energizedEndTime -= config.energizedTimeConsumedPerShot;
	SyncNetworkedFields(pWeapon, *inst);

	if (sdk_energize_drain_trace.GetBool())
	{
		const float now = gpGlobals ? gpGlobals->curTime : 0.0f;
		Msg(eDLL_T::SERVER, "[Energize] shot drained %.2f weapon=%p end=%.3f left=%.2f\n",
			config.energizedTimeConsumedPerShot, pWeapon, inst->energizedEndTime,
			inst->energizedEndTime - now);
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

	// S21 cancels on Holster/Deploy/Deactivate; catch a wind-up whose weapon
	// left the hands without passing through a state we tick.
	if (trigState.hWindUpWeapon.IsValid())
	{
		void* const pPrev = SDKEntityState_Resolve(trigState.hWindUpWeapon, ESide::Server);
		EnergizeInstanceState* const inst = pPrev ? s_energizeStatesServer.Find(pPrev) : nullptr;

		bool held = false;
		for (int i = 0; i < nWeapons; i++)
			held |= weapons[i] == pPrev;

		if (inst && inst->energizeState == ENERGIZE_ENERGIZING && !held)
		{
			const HSCRIPT hPlayerScript = reinterpret_cast<CBaseEntity*>(pPlayer)->GetScriptInstance();
			const HSCRIPT hWeaponScript = reinterpret_cast<CBaseEntity*>(pPrev)->GetScriptInstance();
			if (hPlayerScript && hWeaponScript)
			{
				Energize_Cancel(pPlayer, pPrev, *inst, GetEnergizeConfig(GetWeaponClassNameRaw(pPrev)),
					hWeaponScript, hPlayerScript, "left-hands");
			}
		}
		trigState.hWindUpWeapon = SDKEntityHandle();
	}

	for (int i = 0; i < nWeapons; i++)
	{
		const EnergizeInstanceState* const inst = s_energizeStatesServer.Find(weapons[i]);
		if (inst && inst->energizeState == ENERGIZE_ENERGIZING)
			trigState.hWindUpWeapon = SDKEntityState_GetHandle(weapons[i]);
	}
}


void EnergizeBridge_LevelShutdown()
{
	s_energizeStatesServer.Clear();
	s_energizeStatesClient.Clear();
	s_configCache.clear();
	s_energizeTriggerServer.Clear();
}
