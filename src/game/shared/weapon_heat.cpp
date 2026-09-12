#if defined(CLIENT_DLL)
// Nothing client-side -- see weapon_heat.h for why the S21 client owns this
// mechanic natively and what registering duplicates against it broke.
#else // !CLIENT_DLL
//=============================================================================
//
// Purpose: Weapon heat -- dedi-side decay/accumulate so networked fields match the S21 client.
//
//=============================================================================

#include "core/stdafx.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "public/globalvars_base.h"
#include "weapon_heat.h"
#include "weapon_script_vars.h"
#include "sdk_entity_state.h"
#include "dt_extend.h"

#include "public/edict.h"
#include "game/server/r1/weapon_x.h"
#include "game/server/basecombatcharacter.h"

#include <unordered_map>
#include <string>
#include <fstream>
#include "public/tier1/sdk_parse.h"
#include "game/server/player_overheat.h"

extern CGlobalVars* gpGlobals;

// Kill switch for the modVar0 interpolation write in UpdateHeatForWeapon.
static ConVar sdk_weapon_heat_modvar_write("sdk_weapon_heat_modvar_write", "1",
	FCVAR_DEVELOPMENTONLY | FCVAR_REPLICATED,
	"Enables the heat_mod_var0 (e.g. burst_fire_delay) interpolation WRITE in "
	"UpdateHeatForWeapon (0=off, 1=on).");

// Kill switch for the burst-count seed. Off means every burst weapon fires full length.
static ConVar bridge_burst_fire_seed("bridge_burst_fire_seed", "1",
	FCVAR_RELEASE,
	"Seed CWeaponX::m_burstFireCount from the weapon KV, which the S3 server "
	"never does on its own (0=off, 1=on).");

// Kill switch for the charge-hold stand-in. Off restores the stock behaviour,
// which is that a full draw releases and fires itself.
static ConVar bridge_charge_hold_when_full("bridge_charge_hold_when_full", "1",
	FCVAR_RELEASE,
	"Honour charge_allow_hold_when_full from the weapon KV, which this engine "
	"has no setting for (0=off, 1=on).");

static ConVar bridge_charge_min_required("bridge_charge_min_required", "1",
	FCVAR_RELEASE,
	"Honour charge_attack_min_charge_required from the weapon KV, which this "
	"engine has no setting for (0=off, 1=on).");

static ConVar bridge_charge_overheat_cooldown("bridge_charge_overheat_cooldown", "1",
	FCVAR_RELEASE,
	"Use charge_overheat_cooldown_time/delay when a charge weapon overheats "
	"(0=off, 1=on).");

// Charge-system offsets. GetWeaponChargeFraction inner must match these.
static constexpr int WEAPON_CLASSNAME_OFFSET             = 0x15B0;
// m_lastPrimaryAttack. Sits one dword above m_weaponOwner, which all three live
// weapon-frame functions load from entity+0x11F0.
static constexpr int WEAPON_LAST_PRIMARY_ATTACK_OFFSET   = 0x11F4;
// m_modVars base. The live [KICK-BASE]/[ZOOM-GATE] taps read burst_fire_count
// (+0x34), viewkick decay delay/rate and m_weapState through this base and print
// values that match mp_weapon_nemesis.txt exactly on a running server.
static constexpr int WEAPON_MODVARS_OFFSET               = 0x17E0;
// m_burstFireCount / m_burstFireIndex are the runtime networked counters, not the KV.
static constexpr int WEAPON_BURST_FIRE_COUNT_OFFSET      = 0x155C;
static constexpr int WEAPON_BURST_FIRE_INDEX_OFFSET      = 0x1560;
// burst_fire_delay: descriptor index 0x0C, float, KV offset 0x38.
static constexpr int MODVAR_BURST_FIRE_COUNT_OFFSET      = 0x34;
static constexpr int MODVAR_BURST_FIRE_DELAY_OFFSET      = 0x38;
// m_weapState at DT_WeaponX +0x1234.
static constexpr int WEAPON_STATE_DWORD_OFFSET           = 0x1234;
// Charge state is read from the same live weapon as classname / m_modVars.
static constexpr int WEAPON_CHARGE_START_TIME_OFFSET     = 0x153C;
static constexpr int WEAPON_CHARGE_END_TIME_OFFSET       = 0x1540;
static constexpr int WEAPON_LAST_CHARGE_FRAC_OFFSET      = 0x1544;
static constexpr int WEAPON_IS_CHARGING_BYTE_OFFSET      = 0x1551;
// WEAP_STATE_CHARGE. The FSM treats this and the next value (charge-release) as
// "in a charge state", which is the `(state - 5) <= 1` test the engine repeats
// at every charge site.
static constexpr int WEAP_STATE_CHARGE                   = 5;
// Modvar KV offsets straight off the weapon-KV table: shared_energy_charge_cost
// 0x36C, charge_weapon_fires_while_charging 0x4F0.
static constexpr int WEAPON_SHARED_ENERGY_COST_OFFSET    = WEAPON_MODVARS_OFFSET + 0x36C;
static constexpr int WEAPON_FIRES_WHILE_CHARGING_OFFSET  = WEAPON_MODVARS_OFFSET + 0x4F0;
static constexpr int WEAPON_CHARGE_TIME_ABSOLUTE_OFFSET  = WEAPON_MODVARS_OFFSET + 0x4C4;
static constexpr int WEAPON_CHARGE_COOLDOWN_TIME_OFFSET  = WEAPON_MODVARS_OFFSET + 0x4C8;
static constexpr int WEAPON_CHARGE_COOLDOWN_DELAY_OFFSET = WEAPON_MODVARS_OFFSET + 0x4D8;
static constexpr int WEAPON_CHARGE_LEVELS_OFFSET         = WEAPON_MODVARS_OFFSET + 0x4E0;
static constexpr int WEAPON_CHARGE_LEVEL_BASE_OFFSET     = WEAPON_MODVARS_OFFSET + 0x4E4;
// charge_overheats_when_full -- S3 KV bool at modvars+0x4EE.
static constexpr int WEAPON_CHARGE_OVERHEATS_WHEN_FULL_OFFSET = WEAPON_MODVARS_OFFSET + 0x4EE;
// charge_remain_full_when_fired -- GetChargeFraction's no-decay early-out.
static constexpr int WEAPON_CHARGE_REMAIN_FULL_OFFSET    = WEAPON_MODVARS_OFFSET + 0x4EC;
// charge_require_input -- KV bool at modvars+0x4E8 (entity +0x1CC8).
static constexpr int WEAPON_CHARGE_REQUIRE_INPUT_OFFSET  = WEAPON_MODVARS_OFFSET + 0x4E8;
// S3 charge_cooldown_time_late1/2/3. S21 renamed these to charge_overheat_*.
static constexpr int MODVAR_CHARGE_COOLDOWN_TIME_LATE1   = 0x4CC;
static constexpr int MODVAR_CHARGE_COOLDOWN_TIME_LATE2   = 0x4D0;
static constexpr int MODVAR_CHARGE_COOLDOWN_TIME_LATE3   = 0x4D4;
// Reserved native slots. Schema parse + mod assembly land hop-up multipliers here.
static constexpr int MODVAR_CUSTOM_FLOAT_6               = 0xE84;
static constexpr int MODVAR_CUSTOM_FLOAT_7               = 0xE88;

//-----------------------------------------------------------------------------
// Per-weapon-class heat configuration
//-----------------------------------------------------------------------------
struct WeaponHeatConfig
{
	bool hasHeatDecay = false;
	float heatPerBullet = 0.0f;
	float heatDecayTime = 0.0f;
	float heatDecayDelay = 0.0f;

	bool hasHeatModVar0 = false;
	int heatModVar0Offset = -1;
	float heatModVar0Start = 0.0f;
	float heatModVar0End = 0.0f;

	std::string weaponBaseClass;

	// burst_fire_count straight out of the .txt. This is the one burst value we
	// can state without trusting a struct offset, so it doubles as the anchor
	// that identifies the weapon layout at runtime (see the seed pass).
	int burstFireCount = 0;

	bool hasChargeCurve = false;
	float chargeCurveA = 0.0f;
	float chargeCurveB = 0.0f;
	float chargeCurveC = 0.0f;

	// charge_allow_hold_when_full. Parsed here for the same reason the charge
	// curve is: this engine's weapon-KV table has no entry for the key, so the
	// value never reaches m_modVars and the charge FSM cannot see it.
	bool chargeAllowHoldWhenFull = false;

	// charge_attack_min_charge_required. Same missing-key case; this engine
	// only has charge_attack_requires_full_charge (bool, default 0).
	float chargeAttackMinChargeRequired = 0.0f;

	// charge_overheat_cooldown_time / _delay. S3 has the overheat predicate
	// and the late-ramp slots; it has no schema entries for these two.
	bool hasOverheatCooldown = false;
	float overheatCooldownTime = 0.0f;
	float overheatCooldownDelay = 0.0f;
	float overheatCooldownTimeLate1 = 0.0f;
	float overheatCooldownTimeLate2 = 0.0f;
	float overheatCooldownTimeLate3 = 0.0f;
};

static std::unordered_map<std::string, WeaponHeatConfig> s_weaponHeatConfigs;
static constexpr size_t MAX_WEAPON_HEAT_CONFIGS = 256;

// Heat sidecar: appended DT_WeaponX slots alias live m_modVars (base 6100 vs 0x17E0).
struct WeaponHeatState
{
	float heatValue      = 0.0f;
	float heatOnLastFire = 0.0f;
	bool  fullyHeated    = false;
};
static SDKEntityMap<WeaponHeatState> s_weaponHeatState(ESide::Server, "weaponHeat.srv");

static inline WeaponHeatState& HeatState(void* pWeapon)
{
	return s_weaponHeatState[static_cast<const void*>(pWeapon)];
}

static inline float GetHeatValue(void* pWeapon)
{
	return HeatState(pWeapon).heatValue;
}

static inline void SetHeatValue(void* pWeapon, float value)
{
	HeatState(pWeapon).heatValue = value;
}

static inline float GetHeatValueOnLastFire(void* pWeapon)
{
	return HeatState(pWeapon).heatOnLastFire;
}

static inline void SetHeatValueOnLastFire(void* pWeapon, float value)
{
	HeatState(pWeapon).heatOnLastFire = value;
}

static inline bool GetFullyHeated(void* pWeapon)
{
	return HeatState(pWeapon).fullyHeated;
}

static inline void SetFullyHeated(void* pWeapon, bool value)
{
	HeatState(pWeapon).fullyHeated = value;
}

// Wire accessors for DT_WeaponX heat proxies. Read-only.
float WeaponHeat_WireGetHeatValue(void* pWeapon)
{
	const WeaponHeatState* const p = s_weaponHeatState.Find(static_cast<const void*>(pWeapon));
	return p ? p->heatValue : 0.0f;
}

float WeaponHeat_WireGetHeatValueOnLastFire(void* pWeapon)
{
	const WeaponHeatState* const p = s_weaponHeatState.Find(static_cast<const void*>(pWeapon));
	return p ? p->heatOnLastFire : 0.0f;
}

int WeaponHeat_WireGetFullyHeated(void* pWeapon)
{
	const WeaponHeatState* const p = s_weaponHeatState.Find(static_cast<const void*>(pWeapon));
	return (p && p->fullyHeated) ? 1 : 0;
}

// The old gate required the three appended offsets to resolve. The sidecar has
// no such dependency, so heat now works from the first shot rather than only
// after dt_extend applies.
static bool HeatFieldsResolved()
{
	return true;
}

static inline int GetWeaponBurstFireCount(void* pWeapon)
{
	return *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_BURST_FIRE_COUNT_OFFSET);
}

static inline int GetWeaponBurstFireIndex(void* pWeapon)
{
	return *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_BURST_FIRE_INDEX_OFFSET);
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static inline float Clamp01(float val)
{
	if (val < 0.0f) return 0.0f;
	if (val > 1.0f) return 1.0f;
	return val;
}

static inline const char* GetWeaponClassName(void* pWeapon)
{
	return (const char*)((uintptr_t)pWeapon + WEAPON_CLASSNAME_OFFSET);
}

bool WeaponHeat_IsChargeOverheated(void* pWeapon)
{
	if (!pWeapon)
		return false;

	const uintptr_t base = reinterpret_cast<uintptr_t>(pWeapon);
	return *reinterpret_cast<const unsigned char*>(base + WEAPON_CHARGE_OVERHEATS_WHEN_FULL_OFFSET) != 0
		&& *reinterpret_cast<const float*>(base + WEAPON_LAST_CHARGE_FRAC_OFFSET) == 1.0f;
}

static inline float GetWeaponLastPrimaryAttack(void* pWeapon)
{
	return *(float*)((uintptr_t)pWeapon + WEAPON_LAST_PRIMARY_ATTACK_OFFSET);
}

static inline float* GetWeaponModVarFloat(void* pWeapon, int modVarByteOffset)
{
	return (float*)((uintptr_t)pWeapon + WEAPON_MODVARS_OFFSET + modVarByteOffset);
}

static int ResolveModVarOffset(const std::string& modVarName)
{
	if (modVarName == "burst_fire_delay")
		return MODVAR_BURST_FIRE_DELAY_OFFSET;

	Warning(eDLL_T::SERVER, "WeaponHeat: Unknown heat_mod_var0 '%s' - interpolation disabled\n",
		modVarName.c_str());
	return -1;
}

// Weapon.txt parser for heat-specific keys.
static WeaponHeatConfig LoadHeatConfigFromFile(const std::string& weaponClassName)
{
	WeaponHeatConfig config;

	// Reject path traversal attempts in weapon classnames
	if (weaponClassName.find("..") != std::string::npos ||
		weaponClassName.find('/') != std::string::npos ||
		weaponClassName.find('\\') != std::string::npos)
	{
		Warning(eDLL_T::SERVER, "WeaponHeat: Rejected invalid weapon classname '%s'\n",
			weaponClassName.c_str());
		return config;
	}

	const char* searchPaths[] = {
		"platform/scripts/weapons/"
	};

	std::ifstream file;
	std::string lastPath;
	for (const char* basePath : searchPaths)
	{
		lastPath = std::string(basePath) + weaponClassName + ".txt";
		// A failed open leaves failbit set and ifstream::open refuses to reopen a
		// failed stream, so without this clear() the second search path could
		// never be reached -- only the first one was ever really tried.
		file.clear();
		file.open(lastPath);
		if (file.is_open())
			break;
	}

	if (!file.is_open())
	{
		// These paths are relative, so they resolve against the process working
		// directory. Until now a miss returned an all-false config in silence,
		// which is indistinguishable from a weapon that simply has no heat.
		static int s_openFailures = 0;
		if (s_openFailures++ < 8)
			Warning(eDLL_T::SERVER,
				"[WeaponHeat] could not open weapon KV for '%s' (last tried '%s') -- "
				"heat and burst seed disabled for it\n",
				weaponClassName.c_str(), lastPath.c_str());
		return config;
	}

	std::string line;
	std::string heatModVar0Name;

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

		if (key == "has_heat_decay")
			config.hasHeatDecay = (Sdk_ParseInt(value) != 0);
		else if (key == "heat_per_bullet")
			config.heatPerBullet = Sdk_ParseFloat(value);
		else if (key == "heat_decay_time")
			config.heatDecayTime = Sdk_ParseFloat(value);
		else if (key == "heat_decay_delay")
			config.heatDecayDelay = Sdk_ParseFloat(value);
		else if (key == "heat_mod_var0")
			heatModVar0Name = value;
		else if (key == "heat_mod_var0_start")
			config.heatModVar0Start = Sdk_ParseFloat(value);
		else if (key == "heat_mod_var0_end")
			config.heatModVar0End = Sdk_ParseFloat(value);
		else if (key == "weaponBaseClass")
			config.weaponBaseClass = value;
		else if (key == "burst_fire_count")
			config.burstFireCount = Sdk_ParseInt(value);
		else if (key == "charge_allow_hold_when_full")
			config.chargeAllowHoldWhenFull = (Sdk_ParseInt(value) != 0);
		else if (key == "charge_attack_min_charge_required")
			config.chargeAttackMinChargeRequired = Sdk_ParseFloat(value);
		else if (key == "charge_overheat_cooldown_time")
			config.overheatCooldownTime = Sdk_ParseFloat(value);
		else if (key == "charge_overheat_cooldown_delay")
			config.overheatCooldownDelay = Sdk_ParseFloat(value);
		else if (key == "charge_overheat_cooldown_time_late1")
			config.overheatCooldownTimeLate1 = Sdk_ParseFloat(value);
		else if (key == "charge_overheat_cooldown_time_late2")
			config.overheatCooldownTimeLate2 = Sdk_ParseFloat(value);
		else if (key == "charge_overheat_cooldown_time_late3")
			config.overheatCooldownTimeLate3 = Sdk_ParseFloat(value);
		else if (key == "charge_curve_coefficients")
		{
			// Parse "a b c" space-separated floats
			float a = 0.0f, b = 0.0f, c = 0.0f;
			if (sscanf(value.c_str(), "%f %f %f", &a, &b, &c) == 3)
			{
				config.chargeCurveA = a;
				config.chargeCurveB = b;
				config.chargeCurveC = c;
				config.hasChargeCurve = true;
			}
		}
	}

	file.close();

	if (config.hasHeatDecay && !heatModVar0Name.empty())
	{
		config.heatModVar0Offset = ResolveModVarOffset(heatModVar0Name);
		config.hasHeatModVar0 = (config.heatModVar0Offset >= 0);
	}

	if (config.hasHeatDecay)
	{
		DevMsg(eDLL_T::SERVER,
			"WeaponHeat: Loaded '%s' - perBullet=%.5f, decayTime=%.1f, decayDelay=%.1f\n",
			weaponClassName.c_str(), config.heatPerBullet, config.heatDecayTime, config.heatDecayDelay);
	}

	if (config.hasChargeCurve || config.chargeAllowHoldWhenFull)
	{
		Warning(eDLL_T::SERVER,
			"[BowCharge] Loaded '%s': curve=%d a=%.4f b=%.4f c=%.4f allowHoldWhenFull=%d\n",
			weaponClassName.c_str(), config.hasChargeCurve ? 1 : 0,
			config.chargeCurveA, config.chargeCurveB, config.chargeCurveC,
			config.chargeAllowHoldWhenFull ? 1 : 0);
	}

	if (config.chargeAttackMinChargeRequired > 0.0f)
	{
		Msg(eDLL_T::SERVER,
			"[ChargeMin] Loaded '%s': min=%.3f\n",
			weaponClassName.c_str(), config.chargeAttackMinChargeRequired);
	}

	if (config.overheatCooldownTime > 0.0f)
	{
		config.hasOverheatCooldown = true;
		Msg(eDLL_T::SERVER,
			"[CHARGE-OH] Loaded '%s': overheat time=%.3f delay=%.3f late=(%.3f, %.3f, %.3f)\n",
			weaponClassName.c_str(), config.overheatCooldownTime, config.overheatCooldownDelay,
			config.overheatCooldownTimeLate1, config.overheatCooldownTimeLate2,
			config.overheatCooldownTimeLate3);
	}

	return config;
}

// Classname -> config cache. Heat runs per weapon slot, so cache by class not instance.
namespace {
	struct HeatConfigCacheEntry { uint64_t hash; const WeaponHeatConfig* cfg; };
}
static HeatConfigCacheEntry s_heatConfigCache[64] = {};

static inline uint64_t HeatConfigHash(const char* s)
{
	uint64_t h = 14695981039346656037ull; // FNV-1a
	for (; *s; ++s)
		h = (h ^ static_cast<unsigned char>(*s)) * 1099511628211ull;
	return h ? h : 1; // 0 doubles as the empty-slot marker
}

static const WeaponHeatConfig& GetHeatConfig(const char* weaponClassName)
{
	if (!weaponClassName || !*weaponClassName)
	{
		static WeaponHeatConfig s_empty;
		return s_empty;
	}

	const uint64_t hash = HeatConfigHash(weaponClassName);
	HeatConfigCacheEntry& slot = s_heatConfigCache[hash & 63];
	if (slot.hash == hash && slot.cfg)
		return *slot.cfg;

	std::string name(weaponClassName);
	auto it = s_weaponHeatConfigs.find(name);
	if (it != s_weaponHeatConfigs.end())
	{
		slot.hash = hash;
		slot.cfg = &it->second;
		return it->second;
	}

	if (s_weaponHeatConfigs.size() >= MAX_WEAPON_HEAT_CONFIGS)
	{
		static WeaponHeatConfig s_empty;
		return s_empty;
	}

	WeaponHeatConfig config = LoadHeatConfigFromFile(name);
	auto result = s_weaponHeatConfigs.emplace(name, config);
	slot.hash = hash;
	slot.cfg = &result.first->second;
	return result.first->second;
}

// Seed m_burstFireCount from the KV. The S3 server never does this for itself.
void WeaponHeat_SeedBurstFireCount(void* pWeapon)
{
	if (!pWeapon)
		return;

	const uintptr_t base = reinterpret_cast<uintptr_t>(pWeapon);

	// Read burst count off the weapon's own modvars -- same source as the live client.
	const int wanted = *reinterpret_cast<const int*>(
		base + WEAPON_MODVARS_OFFSET + MODVAR_BURST_FIRE_COUNT_OFFSET);

	// One-shot announce naming the first weapon this pass sees.
	static bool s_announcedFirstWeapon = false;
	if (!s_announcedFirstWeapon)
	{
		s_announcedFirstWeapon = true;
		const char* const className = GetWeaponClassName(pWeapon);
		const WeaponHeatConfig& config = GetHeatConfig(className);
		Msg(eDLL_T::SERVER,
			"[WeaponHeat] first weapon: class='%s' modVarBurst=%d txt=%d heatDecay=%d seedCvar=%d\n",
			className ? className : "(null)", wanted, config.burstFireCount,
			config.hasHeatDecay ? 1 : 0, bridge_burst_fire_seed.GetBool() ? 1 : 0);
	}

	if (!bridge_burst_fire_seed.GetBool())
		return;

	if (wanted <= 0)
		return;

	int* const pCount = reinterpret_cast<int*>(base + WEAPON_BURST_FIRE_COUNT_OFFSET);

	if (*pCount != 0 || GetWeaponBurstFireIndex(pWeapon) != 0)
		return;

	*pCount = wanted;

	static bool s_announcedSeed = false;
	if (!s_announcedSeed)
	{
		s_announcedSeed = true;
		Msg(eDLL_T::SERVER, "[WeaponHeat] burst seed live: '%s' m_burstFireCount 0 -> %d\n",
			GetWeaponClassName(pWeapon), wanted);
	}
}

static void WeaponHeat_SeedOverheatLateCooldowns(void* pWeapon)
{
	if (!pWeapon || !bridge_charge_overheat_cooldown.GetBool())
		return;

	const WeaponHeatConfig& config = GetHeatConfig(GetWeaponClassName(pWeapon));
	if (!config.hasOverheatCooldown)
		return;

	float* const pLate1 = GetWeaponModVarFloat(pWeapon, MODVAR_CHARGE_COOLDOWN_TIME_LATE1);
	float* const pLate2 = GetWeaponModVarFloat(pWeapon, MODVAR_CHARGE_COOLDOWN_TIME_LATE2);
	float* const pLate3 = GetWeaponModVarFloat(pWeapon, MODVAR_CHARGE_COOLDOWN_TIME_LATE3);

	if (*pLate1 == 0.0f && config.overheatCooldownTimeLate1 > 0.0f)
		*pLate1 = config.overheatCooldownTimeLate1;
	if (*pLate2 == 0.0f && config.overheatCooldownTimeLate2 > 0.0f)
		*pLate2 = config.overheatCooldownTimeLate2;
	if (*pLate3 == 0.0f && config.overheatCooldownTimeLate3 > 0.0f)
		*pLate3 = config.overheatCooldownTimeLate3;
}

// Derive: server twin of UpdateHeatDecay + fully-heated release + modvar ramp.
void WeaponHeat_Derive(void* pWeapon)
{
	if (!pWeapon || !gpGlobals || !HeatFieldsResolved())
		return;

	const WeaponHeatConfig& config = GetHeatConfig(GetWeaponClassName(pWeapon));
	if (!config.hasHeatDecay || config.heatDecayTime <= 0.0f)
		return;

	const float lastAttack = GetWeaponLastPrimaryAttack(pWeapon);
	if (lastAttack <= 0.0f)
		return;

	const float elapsed = gpGlobals->curTime - lastAttack;
	const float decayProgress = Clamp01((elapsed - config.heatDecayDelay) / config.heatDecayTime);
	const float heat = Clamp01(GetHeatValueOnLastFire(pWeapon) - decayProgress);

	SetHeatValue(pWeapon, heat);
	// Mirror client UpdateHeatDecay: scriptFloat0 tracks heat every derive step.
	// Without this the wire freezes at last-fire heat while the client decays,
	// sole-blaming m_scriptFloat0 for the whole idle after a heat weapon burst.
	WeaponScriptVars_SetScriptFloat0(pWeapon, heat);

	// Release only. The latch belongs to the fire path (see WeaponHeat_OnFired):
	// a burst weapon must stay un-heated mid-burst even once its heat has
	// crossed 1.0, which a plain `heat >= 1` rule here would break.
	if (heat < 1.0f && GetFullyHeated(pWeapon))
		SetFullyHeated(pWeapon, false);

	// Interpolate modVar0 by heat (e.g. burst_fire_delay). Skipped once fully
	// heated: at that point the engine hands the field over to the weapon's
	// fully_heated_mod, and continuing to lerp would fight it.
	if (config.hasHeatModVar0 && heat < 1.0f && sdk_weapon_heat_modvar_write.GetBool())
	{
		float* const modVarPtr = GetWeaponModVarFloat(pWeapon, config.heatModVar0Offset);
		*modVarPtr = (config.heatModVar0End - config.heatModVar0Start) * heat
					+ config.heatModVar0Start;

		static bool s_announcedWrite = false;
		if (!s_announcedWrite)
		{
			s_announcedWrite = true;
			Msg(eDLL_T::SERVER, "[WeaponHeat] modvar write live: '%s' +0x%X = %.4f\n",
				GetWeaponClassName(pWeapon),
				WEAPON_MODVARS_OFFSET + config.heatModVar0Offset, *modVarPtr);
		}
	}
}

// Accumulate: server twin of the client's per-shot heat block.
void WeaponHeat_OnFired(void* pWeapon)
{
	if (!pWeapon || !HeatFieldsResolved())
		return;

	const WeaponHeatConfig& config = GetHeatConfig(GetWeaponClassName(pWeapon));
	if (!config.hasHeatDecay)
		return;

	const float heatBefore = GetHeatValue(pWeapon);
	const float heat = Clamp01(heatBefore + config.heatPerBullet);

	SetHeatValue(pWeapon, heat);
	SetHeatValueOnLastFire(pWeapon, heat);
	WeaponScriptVars_SetScriptFloat0(pWeapon, heat);

	const int burstCount = GetWeaponBurstFireCount(pWeapon);
	if (burstCount > 0)
	{
		// Engine arms a burst flag on the first round. Do not double-count that shot.
		const int shotIndex = GetWeaponBurstFireIndex(pWeapon) - 1; // engine already incremented
		if (shotIndex == burstCount - 1 && heat >= 1.0f)
			SetFullyHeated(pWeapon, true);
	}
	else
	{
		SetFullyHeated(pWeapon, heat >= 1.0f);
	}

	static bool s_announcedFireEdge = false;
	if (!s_announcedFireEdge)
	{
		s_announcedFireEdge = true;
		Msg(eDLL_T::SERVER, "[WeaponHeat] first fire: '%s' heat=%.4f burst=%d/%d\n",
			GetWeaponClassName(pWeapon), heat,
			GetWeaponBurstFireIndex(pWeapon), burstCount);
	}
}

//-----------------------------------------------------------------------------
// Script natives
//-----------------------------------------------------------------------------
static SQRESULT Script_HasHeatDecay(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	const char* className = GetWeaponClassName(pWeapon);
	const WeaponHeatConfig& config = GetHeatConfig(className);

	sq_pushbool(v, config.hasHeatDecay);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetHeatValue(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	if (!HeatFieldsResolved())
		return SQ_ERROR;

	// No update here -- the engine's own weapon frame already derived this value
	// for the current command; polling from a script native would tie the
	// update cadence to script traffic.
	sq_pushfloat(v, GetHeatValue(pWeapon));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetHeatValueOnLastFire(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	if (!HeatFieldsResolved())
		return SQ_ERROR;

	sq_pushfloat(v, GetHeatValueOnLastFire(pWeapon));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetHeatValue(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	SQFloat value;
	sq_getfloat(v, 2, &value);

	if (!HeatFieldsResolved())
		return SQ_ERROR;

	// Matches the engine's own setter: both the accumulator and the derived
	// value, so the next decay starts from here instead of snapping back.
	const float clamped = Clamp01(static_cast<float>(value));
	SetHeatValueOnLastFire(pWeapon, clamped);
	SetHeatValue(pWeapon, clamped);
	WeaponScriptVars_SetScriptFloat0(pWeapon, clamped);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Weapon base class functions
//-----------------------------------------------------------------------------
static SQRESULT Script_WeaponGetBaseClassName(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	const char* className = GetWeaponClassName(pWeapon);
	const WeaponHeatConfig& config = GetHeatConfig(className);

	// If has baseclass, return it; otherwise return the weapon's own classname
	if (!config.weaponBaseClass.empty())
		sq_pushstring(v, config.weaponBaseClass.c_str(), -1);
	else
		sq_pushstring(v, className ? className : "", -1);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_WeaponGetBaseClassNameOrEmpty(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	const char* className = GetWeaponClassName(pWeapon);
	const WeaponHeatConfig& config = GetHeatConfig(className);

	// If has baseclass, return it; otherwise return empty string
	if (!config.weaponBaseClass.empty())
		sq_pushstring(v, config.weaponBaseClass.c_str(), -1);
	else
		sq_pushstring(v, "", -1);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_WeaponHasBaseClass(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	const char* className = GetWeaponClassName(pWeapon);
	const WeaponHeatConfig& config = GetHeatConfig(className);

	sq_pushbool(v, !config.weaponBaseClass.empty());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_Global_Weapon_GetBaseClassName(HSQUIRRELVM v)
{
	const SQChar* weaponName = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &weaponName)) || !weaponName)
		return SQ_ERROR;

	const WeaponHeatConfig& config = GetHeatConfig(weaponName);

	if (!config.weaponBaseClass.empty())
		sq_pushstring(v, config.weaponBaseClass.c_str(), -1);
	else
		sq_pushstring(v, weaponName, -1);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT Script_Global_Weapon_GetBaseClassNameOrEmpty(HSQUIRRELVM v)
{
	const SQChar* weaponName = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &weaponName)) || !weaponName)
		return SQ_ERROR;

	const WeaponHeatConfig& config = GetHeatConfig(weaponName);

	if (!config.weaponBaseClass.empty())
		sq_pushstring(v, config.weaponBaseClass.c_str(), -1);
	else
		sq_pushstring(v, "", -1);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Charge curve: linear [0,1] then the KV curve, matching GetWeaponChargeFractionCurved.
static inline float GetWeaponChargeStartTime(void* pWeapon)
{
	return *(float*)((uintptr_t)pWeapon + WEAPON_CHARGE_START_TIME_OFFSET);
}

static inline float GetWeaponLastChargeFrac(void* pWeapon)
{
	return *(float*)((uintptr_t)pWeapon + WEAPON_LAST_CHARGE_FRAC_OFFSET);
}

static inline bool GetWeaponIsChargingFlag(void* pWeapon)
{
	return *(unsigned char*)((uintptr_t)pWeapon + WEAPON_IS_CHARGING_BYTE_OFFSET) != 0;
}

static inline int GetWeaponStateDword(void* pWeapon)
{
	return *(int*)((uintptr_t)pWeapon + WEAPON_STATE_DWORD_OFFSET);
}

static inline float GetWeaponChargeTime(void* pWeapon)
{
	return *(float*)((uintptr_t)pWeapon + WEAPON_CHARGE_TIME_ABSOLUTE_OFFSET);
}

// Charge FSM clock is gpGlobals+0x28 (not curTime); keep fraction on that timebase.
static constexpr int GLOBALVARS_CHARGE_CLOCK_OFFSET = 0x28;

static inline float GetChargeClockTime(void)
{
	if (!gpGlobals)
		return 0.0f;

	return *(const float*)((const unsigned char*)gpGlobals + GLOBALVARS_CHARGE_CLOCK_OFFSET);
}

static void ResolveOverheatCooldownTimes(void* pWeapon, const WeaponHeatConfig& config,
	float* pTime, float* pDelay)
{
	const float assembledTime = *GetWeaponModVarFloat(pWeapon, MODVAR_CUSTOM_FLOAT_6);
	const float assembledDelay = *GetWeaponModVarFloat(pWeapon, MODVAR_CUSTOM_FLOAT_7);
	*pTime = (assembledTime > 0.0f) ? assembledTime : config.overheatCooldownTime;
	*pDelay = (assembledDelay > 0.0f) ? assembledDelay : config.overheatCooldownDelay;
}

static float ComputeOverheatCooldownFraction(void* pWeapon, float duration, float delay)
{
	const uintptr_t base = reinterpret_cast<uintptr_t>(pWeapon);
	const float lastFrac = *reinterpret_cast<const float*>(base + WEAPON_LAST_CHARGE_FRAC_OFFSET);
	const float startTime = *reinterpret_cast<const float*>(base + WEAPON_CHARGE_START_TIME_OFFSET);
	const float endTime = *reinterpret_cast<const float*>(base + WEAPON_CHARGE_END_TIME_OFFSET);

	if (*reinterpret_cast<const unsigned char*>(base + WEAPON_CHARGE_REMAIN_FULL_OFFSET)
		&& startTime > endTime)
		return lastFrac;

	if (duration <= 0.0f)
		return lastFrac;

	const float elapsed = GetChargeClockTime() - endTime - delay;
	const float frac = lastFrac - ((elapsed > 0.0f) ? elapsed : 0.0f) / duration;
	if (frac <= 0.0f)
		return 0.0f;
	return frac;
}

// Linear [0,1] charge fraction, matching the engine GetChargeFraction helper.
static float ComputeWeaponLinearChargeFraction(void* pWeapon)
{
	if (pWeapon && bridge_charge_overheat_cooldown.GetBool()
		&& WeaponHeat_IsChargeOverheated(pWeapon))
	{
		const WeaponHeatConfig& config = GetHeatConfig(GetWeaponClassName(pWeapon));
		if (config.hasOverheatCooldown)
		{
			float duration = 0.0f;
			float delay = 0.0f;
			ResolveOverheatCooldownTimes(pWeapon, config, &duration, &delay);
			static bool s_seenOverheat = false;
			if (!s_seenOverheat)
			{
				s_seenOverheat = true;
				Msg(eDLL_T::SERVER,
					"[CHARGE-OH] '%s' overheat cooldown time=%.3f delay=%.3f "
					"(stock time=%.3f delay=%.3f)\n",
					GetWeaponClassName(pWeapon),
					duration, delay,
					*reinterpret_cast<const float*>(
						reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_CHARGE_COOLDOWN_TIME_OFFSET),
					*reinterpret_cast<const float*>(
						reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_CHARGE_COOLDOWN_DELAY_OFFSET));
			}
			return ComputeOverheatCooldownFraction(pWeapon, duration, delay);
		}
	}

	if (v_CWeaponX_GetChargeFraction)
		return v_CWeaponX_GetChargeFraction(pWeapon);

	// The engine function is a 1-hit pattern, so reaching this is a real fault,
	// not a tuning path -- say so rather than quietly serving a second opinion.
	static bool s_warned = false;
	if (!s_warned)
	{
		s_warned = true;
		Warning(eDLL_T::SERVER,
			"[BowCharge] CWeaponX::GetChargeFraction unresolved -- charge fraction "
			"falls back to the SDK reimplementation\n");
	}

	const float chargeTime = GetWeaponChargeTime(pWeapon);
	if (chargeTime <= 0.0f)
		return 0.0f;

	const int state = GetWeaponStateDword(pWeapon);
	const bool stateIsCharging = ((unsigned int)(state - 5) <= 1u);
	const bool flagIsCharging  = GetWeaponIsChargingFlag(pWeapon);

	if (stateIsCharging || flagIsCharging)
	{
		// Active-charging branch
		if (!gpGlobals) return 0.0f;
		const float startTime = GetWeaponChargeStartTime(pWeapon);
		float f = (GetChargeClockTime() - startTime) / chargeTime;
		return Clamp01(f);
	}

	// Just-ended / cooldown branch: engine returns preserved m_lastChargeFrac
	// (optionally decayed; for fire-frame this is essentially the cached value)
	return Clamp01(GetWeaponLastChargeFrac(pWeapon));
}

static SQRESULT Script_GetWeaponChargeFractionCurved(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	const char* className = GetWeaponClassName(pWeapon);
	const WeaponHeatConfig& config = GetHeatConfig(className);

	static bool s_diagOnce = false;
	if (!s_diagOnce)
	{
		s_diagOnce = true;
		Warning(eDLL_T::SERVER,
			"[BowCharge DIAG] weapon='%s' hasChargeCurve=%d curve=(%.4f, %.4f, %.4f) engineFn=%p\n",
			className ? className : "null",
			config.hasChargeCurve, config.chargeCurveA, config.chargeCurveB, config.chargeCurveC,
			(void*)v_CWeaponX_GetChargeFraction);
		Warning(eDLL_T::SERVER,
			"[BowCharge DIAG] chargeTime=%.3f state=%d isChgFlag=%d startTime=%.3f lastFrac=%.3f curTime=%.3f\n",
			GetWeaponChargeTime(pWeapon),
			GetWeaponStateDword(pWeapon),
			(int)GetWeaponIsChargingFlag(pWeapon),
			GetWeaponChargeStartTime(pWeapon),
			GetWeaponLastChargeFrac(pWeapon),
			GetChargeClockTime());
	}

	if (!config.hasChargeCurve)
	{
		sq_pushfloat(v, 0.0f);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const float x = ComputeWeaponLinearChargeFraction(pWeapon);

	const float a = config.chargeCurveA;
	const float b = config.chargeCurveB;
	const float c = config.chargeCurveC;
	const float result = Clamp01(((a * x + b) * x + c) * x);

	sq_pushfloat(v, result);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Engine hooks: seed, derive, accumulate, charge-hold. Attach in GetFun order.
static void(__fastcall* v_CWeaponX_PlayerWeapon_PostFrame)(__int64 weapon) = nullptr;
static void(__fastcall* v_CWeaponX_PlayerWeapon_BusyFrame)(__int64 weapon) = nullptr;
static char(__fastcall* v_CWeaponX_PrimaryAttack)(__int64 weapon) = nullptr;
static char(__fastcall* v_CWeaponX_HandleChargeAttack)(__int64 weapon, char isHoldingAttack,
	char isPrimaryAttack) = nullptr;
static char(__fastcall* v_CWeaponX_ChargeEndNoAttack)(__int64 weapon) = nullptr;

// Unconditional first-call announce so a silent stub cannot hide.
static void WeaponHeat_AnnounceHookOnce(bool& flag, const char* name)
{
	if (flag)
		return;

	flag = true;
	Msg(eDLL_T::SERVER, "[WeaponHeat] HOOK LIVE: %s\n", name);
}

static void __fastcall Hook_CWeaponX_PlayerWeapon_PostFrame(__int64 weapon)
{
	static bool s_seen = false;
	WeaponHeat_AnnounceHookOnce(s_seen, "PlayerWeapon_PostFrame");

	if (weapon)
	{
		WeaponHeat_SeedOverheatLateCooldowns(reinterpret_cast<void*>(weapon));
		WeaponHeat_Derive(reinterpret_cast<void*>(weapon));
	}

	v_CWeaponX_PlayerWeapon_PostFrame(weapon);
}

static void __fastcall Hook_CWeaponX_PlayerWeapon_BusyFrame(__int64 weapon)
{
	static bool s_seen = false;
	WeaponHeat_AnnounceHookOnce(s_seen, "PlayerWeapon_BusyFrame");

	if (weapon)
	{
		WeaponHeat_SeedOverheatLateCooldowns(reinterpret_cast<void*>(weapon));
		WeaponHeat_Derive(reinterpret_cast<void*>(weapon));
	}

	v_CWeaponX_PlayerWeapon_BusyFrame(weapon);
}

static char __fastcall Hook_CWeaponX_PrimaryAttack(__int64 weapon)
{
	static bool s_seen = false;
	WeaponHeat_AnnounceHookOnce(s_seen, "PrimaryAttack");

	// Seed before the engine body -- the only weapon function proven to run every shot.
	if (weapon)
		WeaponHeat_SeedBurstFireCount(reinterpret_cast<void*>(weapon));

	const char result = v_CWeaponX_PrimaryAttack(weapon);

	// Only a shot that actually went out adds heat -- the engine returns 0 for
	// every early-out (no ammo, blocked, script veto).
	if (result && weapon)
	{
		void* const pWeapon = reinterpret_cast<void*>(weapon);
		WeaponHeat_OnFired(pWeapon);
		PlayerOverheat_OnWeaponFired(pWeapon);
	}

	return result;
}

// charge_allow_hold_when_full: keep charge latched at 1.0 while the attack button is held.
static bool WeaponHeat_ShouldHoldFullCharge(void* pWeapon, char isHoldingAttack)
{
	if (!bridge_charge_hold_when_full.GetBool() || !pWeapon)
		return false;

	const WeaponHeatConfig& config = GetHeatConfig(GetWeaponClassName(pWeapon));
	if (!config.chargeAllowHoldWhenFull)
		return false;

	// Term 1: releasing the button must still fire. chargeRequireInput is set on
	// every weapon that carries the hold key, so a released button is a release.
	if (!isHoldingAttack)
		return false;

	// Only while the FSM is already in a charge state -- otherwise the original
	// still has ChargeBegin to run.
	const int state = GetWeaponStateDword(pWeapon);
	if ((unsigned int)(state - WEAP_STATE_CHARGE) > 1u && !GetWeaponIsChargingFlag(pWeapon))
		return false;

	// Term 2: a weapon drawing from the shared energy pool can be starved mid
	// charge, and that judgement belongs to the engine. Only weapons that cost
	// nothing (the bow among them) can be held here without asking.
	if (*(const int*)((uintptr_t)pWeapon + WEAPON_SHARED_ENERGY_COST_OFFSET) != 0)
		return false;

	// Term 3: is the charge actually full? The engine is missing this check.
	const float chargeTime = GetWeaponChargeTime(pWeapon);
	if (chargeTime <= 0.0f)
		return false;

	return ComputeWeaponLinearChargeFraction(pWeapon) >= 1.0f;
}

// True when the stock FSM is about to ChargeEnd (release, or elapsed >= charge_time).
static bool WeaponHeat_ChargeFsmWouldComplete(void* pWeapon, char isHoldingAttack, char isPrimaryAttack)
{
	const int state = GetWeaponStateDword(pWeapon);
	if ((unsigned int)(state - WEAP_STATE_CHARGE) > 1u && !GetWeaponIsChargingFlag(pWeapon))
		return false;

	const uintptr_t base = reinterpret_cast<uintptr_t>(pWeapon);
	const bool requireInput = *reinterpret_cast<const unsigned char*>(
		base + WEAPON_CHARGE_REQUIRE_INPUT_OFFSET) != 0;
	if (!isHoldingAttack && requireInput)
		return true;

	const float chargeTime = GetWeaponChargeTime(pWeapon);
	if (chargeTime <= 0.0f)
	{
		const bool remainFull = *reinterpret_cast<const unsigned char*>(
			base + WEAPON_CHARGE_REMAIN_FULL_OFFSET) != 0;
		return remainFull && isPrimaryAttack;
	}

	return (GetChargeClockTime() - GetWeaponChargeStartTime(pWeapon)) >= chargeTime;
}

static bool WeaponHeat_ShouldChargeEndNoAttack(void* pWeapon, char isHoldingAttack, char isPrimaryAttack)
{
	if (!bridge_charge_min_required.GetBool() || !pWeapon || !v_CWeaponX_ChargeEndNoAttack)
		return false;

	const WeaponHeatConfig& config = GetHeatConfig(GetWeaponClassName(pWeapon));
	if (config.chargeAttackMinChargeRequired <= 0.0f)
		return false;

	if (*reinterpret_cast<const unsigned char*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_FIRES_WHILE_CHARGING_OFFSET))
		return false;

	if (!WeaponHeat_ChargeFsmWouldComplete(pWeapon, isHoldingAttack, isPrimaryAttack))
		return false;

	return Clamp01(ComputeWeaponLinearChargeFraction(pWeapon)) < config.chargeAttackMinChargeRequired;
}

static char __fastcall Hook_CWeaponX_HandleChargeAttack(__int64 weapon, char isHoldingAttack,
	char isPrimaryAttack)
{
	static bool s_seen = false;
	WeaponHeat_AnnounceHookOnce(s_seen, "HandleChargeAttack_Internal");

	if (WeaponHeat_ShouldHoldFullCharge(reinterpret_cast<void*>(weapon), isHoldingAttack))
	{
		// charge_weapon_fires_while_charging weapons keep firing through the hold
		// window, which is a whole second behaviour this stand-in does not carry.
		// Nothing shipped sets both keys; say so if anything ever does.
		if (*(const unsigned char*)((uintptr_t)weapon + WEAPON_FIRES_WHILE_CHARGING_OFFSET))
		{
			static bool s_warnedFiring = false;
			if (!s_warnedFiring)
			{
				s_warnedFiring = true;
				Warning(eDLL_T::SERVER,
					"[BowCharge] '%s' sets charge_allow_hold_when_full AND "
					"charge_weapon_fires_while_charging -- holding is not emulated for it\n",
					GetWeaponClassName(reinterpret_cast<void*>(weapon)));
			}
		}
		else
		{
			static bool s_seenHold = false;
			if (!s_seenHold)
			{
				s_seenHold = true;
				Msg(eDLL_T::SERVER, "[BowCharge] holding '%s' at full charge\n",
					GetWeaponClassName(reinterpret_cast<void*>(weapon)));
			}

			// What the newer builds do here: fall through the release test and
			// return, leaving the weapon charged.
			return 1;
		}
	}

	if (WeaponHeat_ShouldChargeEndNoAttack(reinterpret_cast<void*>(weapon), isHoldingAttack,
		isPrimaryAttack))
	{
		static bool s_seenMin = false;
		if (!s_seenMin)
		{
			s_seenMin = true;
			Msg(eDLL_T::SERVER,
				"[ChargeMin] '%s' ChargeEndNoAttack frac=%.3f min=%.3f hold=%d\n",
				GetWeaponClassName(reinterpret_cast<void*>(weapon)),
				Clamp01(ComputeWeaponLinearChargeFraction(reinterpret_cast<void*>(weapon))),
				GetHeatConfig(GetWeaponClassName(reinterpret_cast<void*>(weapon))).chargeAttackMinChargeRequired,
				isHoldingAttack ? 1 : 0);
		}

		return v_CWeaponX_ChargeEndNoAttack(weapon);
	}

	return v_CWeaponX_HandleChargeAttack(weapon, isHoldingAttack, isPrimaryAttack);
}

static float __fastcall Hook_CWeaponX_GetChargeFraction(void* pWeapon)
{
	static bool s_seen = false;
	WeaponHeat_AnnounceHookOnce(s_seen, "GetChargeFraction");

	return ComputeWeaponLinearChargeFraction(pWeapon);
}

void WeaponHeat_LevelShutdown()
{
	// Order matters: the cache holds pointers into the map.
	memset(s_heatConfigCache, 0, sizeof(s_heatConfigCache));
	s_weaponHeatConfigs.clear();
	s_weaponHeatState.Clear();
}

void VWeaponHeat::GetAdr(void) const
{
	LogFunAdr("PlayWeaponEffectNoCull_Native", v_PlayWeaponEffectNoCull_Native);
	LogFunAdr("CWeaponX::GetChargeFraction", v_CWeaponX_GetChargeFraction);
	LogFunAdr("CWeaponX::PlayerWeapon_PostFrame", v_CWeaponX_PlayerWeapon_PostFrame);
	LogFunAdr("CWeaponX::PlayerWeapon_BusyFrame", v_CWeaponX_PlayerWeapon_BusyFrame);
	LogFunAdr("CWeaponX::PrimaryAttack", v_CWeaponX_PrimaryAttack);
	LogFunAdr("CWeaponX::HandleChargeAttack_Internal", v_CWeaponX_HandleChargeAttack);
	LogFunAdr("CWeaponX::ChargeEndNoAttack", v_CWeaponX_ChargeEndNoAttack);
}

void VWeaponHeat::GetFun(void) const
{
	// PlayWeaponEffectNoCull native SQRESULT function
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8D 54 24 ?? 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74")
		.GetPtr(v_PlayWeaponEffectNoCull_Native);

	// GetChargeFraction helper. The three offset constants must match the live CWeaponX layout.
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 F3 0F 10 81 58 1D 00 00 0F 57 E4 0F 2F C4 76 12 80 B9 65 1D 00 00 00 "
		"74 09 48 83 C4 28 E9 ?? ?? ?? ?? F3 0F 10 A9 A4 1C 00 00")
		.GetPtr(v_CWeaponX_GetChargeFraction);

	// CWeaponX::HandleChargeAttack_Internal (engine helper) -- the charge FSM's
	// per-frame hold-or-release decision. Prologue through the m_weaponOwner load
	// at weapon+0x11F0 and the two bool args being widened into ebp/r14d.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 7C 24 18 41 56 48 83 EC 30 48 8B D9 "
		"0F 29 74 24 20 8B 89 F0 11 00 00 41 0F B6 E8 44 0F B6 F2")
		.GetPtr(v_CWeaponX_HandleChargeAttack);

	// CWeaponX::ChargeEndNoAttack. Live half: weapState +0x1234, isCharging +0x1551,
	// then ChargeEnd_Internal(0, 0).
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 8B 81 34 12 00 00 83 E8 05 83 F8 01 76 16 E8 ?? ?? ?? ?? "
		"84 C0 75 0D 38 81 51 15 00 00 75 05 48 83 C4 28 C3 45 33 C0 33 D2")
		.GetPtr(v_CWeaponX_ChargeEndNoAttack);

	// CWeaponX::PlayerWeapon_PostFrame (engine helper). Prologue through the
	// m_weaponOwner load at weapon+0x11F0 and its -1 test; the wildcard is the
	// entity-list lea rel32.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 "
		"48 83 EC 20 48 8B D9 4C 8D 35 ?? ?? ?? ?? 8B 89 F0 11 00 00 33 ED 83 F9 FF")
		.GetPtr(v_CWeaponX_PlayerWeapon_PostFrame);

	// CWeaponX::PlayerWeapon_BusyFrame (engine helper). Same m_weaponOwner anchor,
	// loaded first here; wildcards are the long branch displacement and the
	// entity-list lea rel32.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 40 8B 91 F0 11 00 00 48 8B D9 83 FA FF 0F 84 ?? ?? ?? ?? "
		"0F B7 C2 4C 89 7C 24 30 4C 8D 3D ?? ?? ?? ?? C1 EA 10")
		.GetPtr(v_CWeaponX_PlayerWeapon_BusyFrame);

	// CWeaponX::PrimaryAttack (engine helper). Prologue plus the 0xD8 frame and
	// the same m_weaponOwner+0x11F0 deref that opens every live weapon function.
	Module_FindPattern(g_GameDll,
		"40 55 56 57 41 55 48 81 EC D8 00 00 00 48 8B F9 33 ED 8B 89 F0 11 00 00 "
		"8B C1 83 F9 FF 74 ?? 0F B7 C1 C1 E9 10")
		.GetPtr(v_CWeaponX_PrimaryAttack);

	if (!v_CWeaponX_PlayerWeapon_PostFrame)
		Warning(eDLL_T::SERVER,
			"[WeaponHeat] CWeaponX::PlayerWeapon_PostFrame pattern unresolved -- heat decay disabled\n");
	if (!v_CWeaponX_PlayerWeapon_BusyFrame)
		Warning(eDLL_T::SERVER,
			"[WeaponHeat] CWeaponX::PlayerWeapon_BusyFrame pattern unresolved -- "
			"heat decay blind during cooldown\n");
	if (!v_CWeaponX_PrimaryAttack)
		Warning(eDLL_T::SERVER,
			"[WeaponHeat] CWeaponX::PrimaryAttack pattern unresolved -- heat accumulation disabled\n");
	if (!v_CWeaponX_HandleChargeAttack)
		Warning(eDLL_T::SERVER,
			"[BowCharge] CWeaponX::HandleChargeAttack_Internal pattern unresolved -- "
			"charge_allow_hold_when_full stays dead, full draws will self-fire\n");
	if (!v_CWeaponX_GetChargeFraction)
		Warning(eDLL_T::SERVER,
			"[BowCharge] CWeaponX::GetChargeFraction pattern unresolved\n");
	if (!v_CWeaponX_ChargeEndNoAttack)
		Warning(eDLL_T::SERVER,
			"[ChargeMin] CWeaponX::ChargeEndNoAttack pattern unresolved -- "
			"charge_attack_min_charge_required stays dead\n");
}

void VWeaponHeat::Detour(const bool bAttach) const
{
	// DetourSetup returns DetourAttach's status; treat a failed attach as unresolved.
	LONG postRes   = ERROR_INVALID_HANDLE;
	LONG busyRes   = ERROR_INVALID_HANDLE;
	LONG paRes     = ERROR_INVALID_HANDLE;
	LONG chargeRes = ERROR_INVALID_HANDLE;
	LONG fracRes   = ERROR_INVALID_HANDLE;

	if (v_CWeaponX_PlayerWeapon_PostFrame)
		postRes = DetourSetup(&v_CWeaponX_PlayerWeapon_PostFrame, &Hook_CWeaponX_PlayerWeapon_PostFrame, bAttach);
	if (v_CWeaponX_PlayerWeapon_BusyFrame)
		busyRes = DetourSetup(&v_CWeaponX_PlayerWeapon_BusyFrame, &Hook_CWeaponX_PlayerWeapon_BusyFrame, bAttach);
	if (v_CWeaponX_PrimaryAttack)
		paRes = DetourSetup(&v_CWeaponX_PrimaryAttack, &Hook_CWeaponX_PrimaryAttack, bAttach);
	if (v_CWeaponX_HandleChargeAttack)
		chargeRes = DetourSetup(&v_CWeaponX_HandleChargeAttack, &Hook_CWeaponX_HandleChargeAttack, bAttach);
	if (v_CWeaponX_GetChargeFraction)
		fracRes = DetourSetup(&v_CWeaponX_GetChargeFraction, &Hook_CWeaponX_GetChargeFraction, bAttach);

	if (bAttach)
		Msg(eDLL_T::SERVER,
			"[WeaponHeat] DetourAttach PostFrame result=0x%lX (target=%p) | "
			"BusyFrame result=0x%lX (target=%p) | PrimaryAttack result=0x%lX (target=%p) | "
			"HandleChargeAttack result=0x%lX (target=%p) | "
			"GetChargeFraction result=0x%lX (target=%p)\n",
			postRes,   reinterpret_cast<void*>(v_CWeaponX_PlayerWeapon_PostFrame),
			busyRes,   reinterpret_cast<void*>(v_CWeaponX_PlayerWeapon_BusyFrame),
			paRes,     reinterpret_cast<void*>(v_CWeaponX_PrimaryAttack),
			chargeRes, reinterpret_cast<void*>(v_CWeaponX_HandleChargeAttack),
			fracRes,   reinterpret_cast<void*>(v_CWeaponX_GetChargeFraction));
}

// PlayWeaponEffectNoCullReturnViewEffectHandle: VM wrapper, not the inner.
static SQRESULT Script_PlayWeaponEffectNoCullReturnViewEffectHandle(HSQUIRRELVM v)
{
	if (v_PlayWeaponEffectNoCull_Native)
	{
		// The engine native parses args directly from the VM stack
		SQRESULT res = v_PlayWeaponEffectNoCull_Native(v);
		if (SQ_FAILED(res))
			return res;
	}

	// Engine native pushes no return value (void).
	// We push -1 as handle (EffectSetControlPointVector with -1 is a no-op).
	sq_pushinteger(v, -1);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Registration
//-----------------------------------------------------------------------------
void WeaponHeat_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	DevMsg(eDLL_T::SERVER, "[WeaponHeat] Registering weapon heat functions\n");

	weaponStruct->AddFunction("HasHeatDecay", "Script_HasHeatDecay",
		"Returns true if this weapon has the heat decay mechanic", "bool", "", false, Script_HasHeatDecay);

	weaponStruct->AddFunction("GetHeatValue", "Script_GetHeatValue",
		"Returns the current heat value (0.0 - 1.0)", "float", "", false, Script_GetHeatValue);

	weaponStruct->AddFunction("GetHeatValueOnLastFire", "Script_GetHeatValueOnLastFire",
		"Returns the heat value at the time of the last fire", "float", "", false, Script_GetHeatValueOnLastFire);

	weaponStruct->AddFunction("SetHeatValue", "Script_SetHeatValue",
		"Sets the heat value (also sets heat value on last fire)", "void", "float value", false, Script_SetHeatValue);

	weaponStruct->AddFunction("GetWeaponBaseClassName", "Script_WeaponGetBaseClassName",
		"Gets the base class name, or the weapon's own classname if none", "string", "", false, Script_WeaponGetBaseClassName);

	weaponStruct->AddFunction("GetWeaponBaseClassNameOrEmpty", "Script_WeaponGetBaseClassNameOrEmpty",
		"Gets the base class name, or empty string if none", "string", "", false, Script_WeaponGetBaseClassNameOrEmpty);

	weaponStruct->AddFunction("HasBaseClass", "Script_WeaponHasBaseClass",
		"Returns true if this weapon has a base class defined", "bool", "", false, Script_WeaponHasBaseClass);

	weaponStruct->AddFunction("GetWeaponChargeFractionCurved", "Script_GetWeaponChargeFractionCurved",
		"Returns charge fraction with curve applied (0.0 - 1.0)", "float", "", false, Script_GetWeaponChargeFractionCurved);

	weaponStruct->AddFunction("PlayWeaponEffectNoCullReturnViewEffectHandle", "Script_PlayWeaponEffectNoCullReturnViewEffectHandle",
		"Plays a weapon effect and returns the view effect handle", "int",
		"asset effect1p, asset effect3p, string attachPoint, bool usePrimaryAttachPoint, int attachType", false,
		Script_PlayWeaponEffectNoCullReturnViewEffectHandle);
}
#endif // CLIENT_DLL
