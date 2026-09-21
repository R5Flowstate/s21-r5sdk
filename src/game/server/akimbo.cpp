//=============================================================================//
//
// Purpose: Akimbo (dual pistol) dedi authors -- see akimbo.h.
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
#include "akimbo.h"
#include "energize.h"
#include "weapon_select_mirror.h"
#include "bridge_zoom_gate.h"
#include "game/shared/activitymodifier.h"
#include "game/shared/activity.h"
#include "vscript_server_natives.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/usercmd.h"
#include "game/shared/edict_dirty.h"
#include "public/edict.h"
#include "game/server/baseentity.h"
#include "game/server/basecombatcharacter.h"
#include "public/tier1/sdk_parse.h"
#include "public/game/shared/weapon_types.h"

#include <unordered_map>
#include <string>
#include <vector>
#include <fstream>
#include <cstddef>

extern CGlobalVars* gpGlobals;

static ConVar bridge_akimbo("bridge_akimbo", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Enables the native akimbo state machine (0=off, 1=on).");

static ConVar bridge_akimbo_diag("bridge_akimbo_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log akimbo state transitions, toggles, reload fills and cascades.");

// Must match client net_bridge_split.h kS21ExtraFlag_ToggleAkimbo (0x40).
// No shared header across the two products -- duplicated constant.
static constexpr uint8_t kS21ExtraFlag_ToggleAkimbo = 0x40;

static constexpr int WEAPON_CLASSNAME_OFFSET = 0x15B0; // m_weaponName[65]
static constexpr uintptr_t kServerInventoryOffset = 0x1688; // WeaponInventory
static constexpr uintptr_t kWeaponOwnerOffset = 0x11F0; // m_weaponOwner
static constexpr uintptr_t kWeaponPlayerDataOffset = 0x1258; // m_playerData
static constexpr uintptr_t kWeaponFireModeOffset = 0x2750; // fireMode; 1..5 = offhand
static constexpr uintptr_t kWeaponActiveHandOffset = 0x2930; // hand stamped by SetActiveWeapon; 1 = alt fires on the zoom buttons
static constexpr uintptr_t kWeaponStateOffset = 0x1234; // m_weapState
static constexpr uintptr_t kPlayerButtonsOffset = 0x60DC; // m_nButtons, m_afButtonPressed follows

static constexpr uintptr_t kActiveWeaponsOffset = 0x16CC; // activeWeapons[3]
static constexpr uintptr_t kSelectedWeaponsOffset = 0x16D8; // m_selectedWeapons[2] int8
static constexpr uintptr_t kLatestPrimaryWeaponsOffset = 0x16DC; // m_latestPrimaryWeapons[2]
static constexpr int kEntityHandleOffset = 8; // m_RefEHandle

static constexpr int8_t kSelectedSlotInvalid = -1;
static constexpr int8_t kSelectedSlotEmpty = -3;

static constexpr size_t MAX_AKIMBO_CONFIGS = 64;
static constexpr size_t MAX_AKIMBO_MODS = 32;

struct AkimboWeaponConfig
{
	bool isAkimboWeapon = false;
	bool useAkimboDamageSource = false;
	std::string cbStateChanged;
	std::vector<std::string> modNames; // bit index -> name, txt Mods order
};

struct AkimboWeaponState
{
	bool disabled = false;
	uint32_t modBitfieldDisabled = 0;
};

typedef __int64 (__fastcall* ChargeAndPrimaryAttack_t)(__int64 weapon, __int64 a2, __int64 a3);
typedef __int64 (__fastcall* DualWieldPartnerRaise_t)(void* player, __int64 weapon);
typedef char (__fastcall* DeployWeapon_t)(__int64 weapon, unsigned char force);
typedef char (__fastcall* HolsterInternal_t)(__int64 weapon, char fastHolster);
typedef void (__fastcall* FillClipAmmoFromStock_t)(__int64 weapon, __int64 unused);
typedef char (__fastcall* WeaponSwitch_t)(void* player, unsigned int hand, __int64 weapon);
typedef float (__fastcall* WeaponFireInterval_t)(__int64 weapon);
typedef unsigned int (__fastcall* WeaponActivityModifiers_t)(__int64 weapon, uint16_t* pMods);

static ChargeAndPrimaryAttack_t v_ChargeAndPrimaryAttack = nullptr;
static DualWieldPartnerRaise_t v_DualWieldPartnerRaise = nullptr;
static DeployWeapon_t v_DeployWeapon = nullptr;
static HolsterInternal_t v_HolsterInternal = nullptr;
typedef char (__fastcall* SwitchToOffhand_t)(void* player, __int64 offhand);
static SwitchToOffhand_t v_SwitchToOffhand = nullptr;
static bool Weapon_IsOffhand(const void* pWeapon);
static FillClipAmmoFromStock_t v_FillClipAmmoFromStock = nullptr;
static WeaponSwitch_t v_WeaponSwitch = nullptr;
static WeaponFireInterval_t v_WeaponFireInterval = nullptr;
static WeaponActivityModifiers_t v_WeaponActivityModifiers = nullptr;
typedef __int64 (__fastcall* WeaponTranslateActivity_t)(__int64 weapon, unsigned int act);
static WeaponTranslateActivity_t v_WeaponTranslateActivity = nullptr;

static SDKEntityMap<AkimboWeaponState> s_akimboWeaponServer(ESide::Server, "akimbo.wpn");

// Alt-hand attack input. Every site is `mov r,1 ; mov r2,30000h ;
// cmp [weapon+2930h],r ; cmovz`: hand 1 reads the zoom buttons. The S21
// client reads IN_ATTACK for both hands while akimbo_weapon_can_zoom is
// set, so the immediate becomes 1.
static const char* const s_altHandInputSiteNames[] = {
	"idle attack",
	"busy frame",
	"weapon update",
	"offhand update",
	"trigger hold",
	"primary attack",
	"attack release",
	"charge hold",
};

static constexpr uint32_t kAltHandZoomButtons = 0x30000;
static constexpr uint32_t kAltHandAttackButton = 0x1;
static uintptr_t s_altHandInputImm[SDK_ARRAYSIZE(s_altHandInputSiteNames)] = {};
static bool s_altHandInputPatched = false;

static uintptr_t AltHandInput_Site(const CMemory& site, const ptrdiff_t immOffset)
{
	return site.IsValid() ? site.GetPtr() + immOffset : 0;
}

static std::unordered_map<uint64_t, AkimboWeaponConfig> s_configCache;

static inline const char* GetWeaponClassNameRaw(void* pWeapon)
{
	return pWeapon ? (const char*)((uintptr_t)pWeapon + WEAPON_CLASSNAME_OFFSET) : "";
}

static inline uint32_t EntityHandle(const void* pEnt)
{
	if (!pEnt)
		return 0xFFFFFFFFu;
	return *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(pEnt) + kEntityHandleOffset);
}

static inline WeaponInventory* GetInventory(void* pPlayer)
{
	if (!pPlayer)
		return nullptr;
	return reinterpret_cast<WeaponInventory*>(
		reinterpret_cast<uint8_t*>(pPlayer) + kServerInventoryOffset);
}

static uint64_t AkimboConfigHash(const char* s)
{
	uint64_t h = 14695981039346656037ull;
	for (; *s; ++s)
		h = (h ^ static_cast<unsigned char>(*s)) * 1099511628211ull;
	return h;
}

static AkimboWeaponConfig LoadAkimboConfigFromFile(const std::string& weaponClassName)
{
	AkimboWeaponConfig config;

	if (weaponClassName.find("..") != std::string::npos ||
		weaponClassName.find('/') != std::string::npos ||
		weaponClassName.find('\\') != std::string::npos)
	{
		Warning(eDLL_T::SERVER, "[Akimbo] Rejected invalid weapon classname '%s'\n",
			weaponClassName.c_str());
		return config;
	}

	std::ifstream file("platform/scripts/weapons/" + weaponClassName + ".txt");
	if (!file.is_open())
		return config;

	std::string line;
	int depth = 0;
	bool inMods = false;
	std::string pendingKey;
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
		const bool gotKey = ParseQuotedString(line, pos, key);
		const bool gotValue = gotKey && ParseQuotedString(line, pos, value);

		if (gotKey && gotValue && depth == 1)
		{
			if (key == "is_akimbo_weapon")
				config.isAkimboWeapon = (Sdk_ParseInt(value) != 0);
			else if (key == "use_akimbo_damage_source")
				config.useAkimboDamageSource = (Sdk_ParseInt(value) != 0);
			else if (key == "OnWeaponAkimboStateChanged")
				config.cbStateChanged = value;
		}
		else if (gotKey && !gotValue)
		{
			pendingKey = key;
		}

		bool inQuote = false;
		for (const char c : line)
		{
			if (c == '"')
			{
				inQuote = !inQuote;
				continue;
			}
			if (inQuote)
				continue;
			if (c == '{')
			{
				++depth;
				if (!inMods && depth == 2 && pendingKey == "Mods")
					inMods = true;
				else if (inMods && depth == 3 && config.modNames.size() < MAX_AKIMBO_MODS)
					config.modNames.push_back(pendingKey);
				pendingKey.clear();
			}
			else if (c == '}')
			{
				if (--depth < 0)
					depth = 0;
				if (depth < 2)
					inMods = false;
			}
		}
	}

	if (config.isAkimboWeapon)
	{
		Msg(eDLL_T::SERVER, "[Akimbo] Loaded '%s' is_akimbo_weapon=1 mods=%d\n",
			weaponClassName.c_str(), static_cast<int>(config.modNames.size()));
	}

	return config;
}

static const AkimboWeaponConfig& GetAkimboConfig(const char* weaponClassName)
{
	static AkimboWeaponConfig s_empty;
	if (!weaponClassName || !*weaponClassName)
		return s_empty;

	const uint64_t hash = AkimboConfigHash(weaponClassName);
	auto it = s_configCache.find(hash);
	if (it != s_configCache.end())
		return it->second;

	if (s_configCache.size() >= MAX_AKIMBO_CONFIGS)
		return s_empty;

	AkimboWeaponConfig config = LoadAkimboConfigFromFile(weaponClassName);
	auto result = s_configCache.emplace(hash, config);
	return result.first->second;
}

static bool Akimbo_GetWeaponFloat(void* pWeapon, const char* propName, float* pOut)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*pOut = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(pWeapon) + off);
	return true;
}

static bool Akimbo_SetWeaponFloat(void* pWeapon, const char* propName, float value)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	float* const p = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pWeapon) + off);
	if (*p == value)
		return true;
	*p = value;
	MarkEntityEdictDirty(pWeapon);
	return true;
}

static bool Akimbo_GetWeaponInt(void* pWeapon, const char* propName, int* pOut)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*pOut = *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(pWeapon) + off);
	return true;
}

static bool Akimbo_SetWeaponInt(void* pWeapon, const char* propName, int value)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(pWeapon) + off) = value;
	MarkEntityEdictDirty(pWeapon);
	return true;
}

static bool Akimbo_GetWeaponBool(void* pWeapon, const char* propName, bool* pOut)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*pOut = *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(pWeapon) + off) != 0;
	return true;
}

static bool Akimbo_SetWeaponBool(void* pWeapon, const char* propName, bool value)
{
	const int off = pWeapon ? DTExtend_FindNativePropOffset(pWeapon, propName) : -1;
	if (off <= 0)
		return false;

	*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(pWeapon) + off) = value ? 1 : 0;
	MarkEntityEdictDirty(pWeapon);
	return true;
}

static int GetWeaponDisabledFlags(void* pPlayer)
{
	const int off = pPlayer ? DTExtend_FindNativePropOffset(pPlayer, "m_weaponDisabledFlags") : -1;
	if (off <= 0)
		return 0;

	return *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(pPlayer) + off);
}

static int GetAkimboState(void* pPlayer)
{
	return pPlayer ? BCCExtend_GetI32(pPlayer, offsetof(BCCExtendWire, m_akimboState)) : AKIMBO_STATE_NONE;
}

static void SetBccI32(void* pPlayer, size_t fieldOff, int32_t value)
{
	BCCExtend_SetI32(pPlayer, fieldOff, value);
	MarkEntityEdictDirty(pPlayer);
}

static void SetSelectedAlt(void* pPlayer, int8_t value)
{
	int8_t* const pSel = reinterpret_cast<int8_t*>(
		reinterpret_cast<uint8_t*>(pPlayer) + kSelectedWeaponsOffset + 1);
	if (*pSel == value)
		return;
	*pSel = value;
	MarkEntityEdictDirty(pPlayer);
}

static void* ResolveHandle(uint32_t eh)
{
	if (eh == 0xFFFFFFFFu)
		return nullptr;
	return SDKEntityState_Resolve(SDKEntityHandle(eh), ESide::Server);
}

static int Inv_GetIndex(void* pPlayer, void* pWeapon)
{
	if (!pPlayer || !pWeapon)
		return -1;

	const WeaponInventory* const pInv = GetInventory(pPlayer);
	if (!pInv)
		return -1;

	const uint32_t weaponEh = EntityHandle(pWeapon);
	for (int i = 0; i < 9; ++i)
	{
		if (static_cast<uint32_t>(pInv->weapons[i].ToInt()) == weaponEh)
			return i;
	}
	return -1;
}

static void* GetOwner(void* pWeapon)
{
	if (!pWeapon)
		return nullptr;

	const uint32_t eh = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(pWeapon) + kWeaponOwnerOffset);
	return ResolveHandle(eh);
}

static void* GetActive(void* pPlayer, int hand)
{
	if (!pPlayer || hand < 0 || hand > 2)
		return nullptr;

	const uint32_t eh = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<const uint8_t*>(pPlayer) + kActiveWeaponsOffset + 4 * hand);
	return ResolveHandle(eh);
}

static constexpr uintptr_t kPlayerContextActionOffset = 6140; // m_contextAction; 9 = zipline
static constexpr int kContextActionZipline = 9;

// Weapon_IsEnabled for the partner raise: the engine's disable reasons that a
// dual-wield alt can hit -- not usable/discarded, the owner's weapon-disabled
// flags, and an alt hand while the owner rides a zipline.
static bool Weapon_IsEnabled(void* pPlayer, void* pWeapon)
{
	bool allowed = true;
	bool discarded = false;
	if (Akimbo_GetWeaponBool(pWeapon, "m_allowedToUse", &allowed) == false)
		allowed = true;
	if (Akimbo_GetWeaponBool(pWeapon, "m_discarded", &discarded) == false)
		discarded = false;
	if (!allowed || discarded)
		return false;
	if (pPlayer && (GetWeaponDisabledFlags(pPlayer) & 3) != 0)
		return false;
	if (pPlayer && AkimboBridge_IsAlthand(pWeapon)
		&& *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(pPlayer) + kPlayerContextActionOffset) == kContextActionZipline)
		return false;
	return true;
}

static bool InvokeGlobalClosure(const char* closureName, ScriptVariant_t* args, unsigned int nArgs,
	ScriptVariant_t* pReturn)
{
	if (!closureName || !*closureName)
		return false;

	if (!g_pServerScript)
	{
		Warning(eDLL_T::SERVER, "[Akimbo] no server VM -- cannot fire '%s'\n", closureName);
		return false;
	}

	const HSCRIPT hFunc = g_pServerScript->FindFunction(closureName, nullptr, nullptr);
	if (!hFunc)
	{
		static int s_nMissLog = 8;
		if (s_nMissLog > 0)
		{
			--s_nMissLog;
			Warning(eDLL_T::SERVER, "[Akimbo] closure '%s' not found in VM\n", closureName);
		}
		return false;
	}

	return g_pServerScript->ExecuteFunction(hFunc, args, nArgs, pReturn, nullptr) != SCRIPT_ERROR;
}

bool AkimboBridge_IsAkimboWeapon(void* pWeapon)
{
	if (!pWeapon)
		return false;
	return GetAkimboConfig(GetWeaponClassNameRaw(pWeapon)).isAkimboWeapon;
}

void* AkimboBridge_GetOtherWeapon(void* pWeapon)
{
	void* const owner = GetOwner(pWeapon);
	const int slot = Inv_GetIndex(owner, pWeapon);
	if (!owner || slot == -1 || !AkimboBridge_IsAkimboWeapon(pWeapon))
		return nullptr;

	const WeaponInventory* const pInv = GetInventory(owner);
	if (!pInv)
		return nullptr;

	if (slot <= 4)
	{
		if (slot + 7 < 9)
			return ResolveHandle(static_cast<uint32_t>(pInv->weapons[slot + 7].ToInt()));
		return nullptr;
	}

	if (static_cast<unsigned>(slot - 7) <= 4)
		return ResolveHandle(static_cast<uint32_t>(pInv->weapons[slot - 7].ToInt()));

	return nullptr;
}

bool AkimboBridge_IsAlthand(void* pWeapon)
{
	if (!AkimboBridge_IsAkimboWeapon(pWeapon))
		return false;

	void* const owner = GetOwner(pWeapon);
	if (!owner)
		return false;

	bool discarded = false;
	if (Akimbo_GetWeaponBool(pWeapon, "m_discarded", &discarded) && discarded)
		return false;

	const int slot = Inv_GetIndex(owner, pWeapon);
	if (slot == -1)
		return false;

	return static_cast<unsigned>(slot - 7) <= 4;
}

bool AkimboBridge_IsDisabled(void* pWeapon)
{
	if (!pWeapon)
		return false;
	const AkimboWeaponState* const st = s_akimboWeaponServer.Find(pWeapon);
	return st && st->disabled;
}

bool AkimboBridge_IsDualWielding(void* pPlayer)
{
	if (!bridge_akimbo.GetBool())
		return false;
	void* const main = GetActive(pPlayer, 0);
	void* const alt = GetActive(pPlayer, 1);
	return main && alt && AkimboBridge_IsAkimboWeapon(main) && AkimboBridge_IsAkimboWeapon(alt);
}

int AkimboBridge_GetDisabledFromPlayerData(const void* pPlayerData)
{
	if (!pPlayerData)
		return 0;

	void* const pWeapon = reinterpret_cast<void*>(
		const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(pPlayerData) - kWeaponPlayerDataOffset));
	return AkimboBridge_IsDisabled(pWeapon) ? 1 : 0;
}

uint32_t AkimboBridge_GetModBitfieldDisabled(const void* pWeapon)
{
	if (!pWeapon)
		return 0;
	const AkimboWeaponState* const st = s_akimboWeaponServer.Find(const_cast<void*>(pWeapon));
	return st ? st->modBitfieldDisabled : 0;
}

// Mainhand writer copies onto the partner; an althand writer stands alone.
static void SetDisabled(void* pWeapon, bool v)
{
	if (!pWeapon)
		return;

	s_akimboWeaponServer[pWeapon].disabled = v;
	MarkEntityEdictDirty(pWeapon);

	if (!AkimboBridge_IsAlthand(pWeapon))
	{
		void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
		if (other)
		{
			s_akimboWeaponServer[other].disabled = v;
			MarkEntityEdictDirty(other);
		}
	}
}

static bool IsOpticMod(void* pWeapon, const char* modName)
{
	const HSCRIPT hWeapon = reinterpret_cast<CBaseEntity*>(pWeapon)->GetScriptInstance();
	if (hWeapon)
	{
		ScriptVariant_t args[2] = { hWeapon, ScriptVariant_t(modName) };
		ScriptVariant_t ret;
		if (InvokeGlobalClosure("CodeCallback_GetIsModOptic", args, 2, &ret)
			&& ret.m_type == FIELD_BOOLEAN)
			return ret.m_bool;
	}
	return strncmp(modName, "optic_", 6) == 0;
}

static void SetOpticModDisabledOne(void* pWeapon, bool disabled)
{
	if (!pWeapon)
		return;

	const AkimboWeaponConfig& config = GetAkimboConfig(GetWeaponClassNameRaw(pWeapon));

	// Every optic the weapon can take is flagged, not just the one attached,
	// so an optic picked up while dual wielding comes in disabled.
	uint32_t bits = 0;
	if (disabled)
	{
		for (size_t i = 0; i < config.modNames.size() && i < 32; ++i)
		{
			if (IsOpticMod(pWeapon, config.modNames[i].c_str()))
				bits |= (1u << i);
		}
	}

	AkimboWeaponState& st = s_akimboWeaponServer[pWeapon];
	if (st.modBitfieldDisabled == bits)
		return;
	st.modBitfieldDisabled = bits;
	MarkEntityEdictDirty(pWeapon);

	if (bridge_akimbo_diag.GetBool())
		Msg(eDLL_T::SERVER, "[Akimbo] optic mods %s weapon=%p bits=0x%08X\n",
			disabled ? "disabled" : "enabled", pWeapon, bits);
}

static void SetOpticModDisabled(void* pMain, bool disabled)
{
	SetOpticModDisabledOne(pMain, disabled);
	SetOpticModDisabledOne(AkimboBridge_GetOtherWeapon(pMain), disabled);
}

static void SetState(void* pPlayer, int newState)
{
	if (!pPlayer)
		return;

	const int oldState = GetAkimboState(pPlayer);
	if (oldState == newState)
		return;

	SetBccI32(pPlayer, offsetof(BCCExtendWire, m_akimboState), newState);

	void* const main = GetActive(pPlayer, 0);
	const HSCRIPT hMain = main
		? reinterpret_cast<CBaseEntity*>(main)->GetScriptInstance()
		: nullptr;

	if (hMain)
	{
		WeaponBridge_InvokeChangeMod(hMain, "akimbo_active", false);
		WeaponBridge_InvokeChangeMod(hMain, "akimbo_offhand", false);
		WeaponBridge_InvokeChangeMod(hMain, "akimbo_disable", false);
		if (newState == AKIMBO_STATE_ACTIVE)
			WeaponBridge_InvokeChangeMod(hMain, "akimbo_active", true);
		else if (newState == AKIMBO_STATE_OFFHAND)
			WeaponBridge_InvokeChangeMod(hMain, "akimbo_offhand", true);
		else if (newState == AKIMBO_STATE_SINGLE)
			WeaponBridge_InvokeChangeMod(hMain, "akimbo_disable", true);
	}

	if (main)
		SetOpticModDisabled(main, newState == AKIMBO_STATE_OFFHAND || newState == AKIMBO_STATE_ACTIVE);

	const HSCRIPT hPlayerScript = reinterpret_cast<CBaseEntity*>(pPlayer)->GetScriptInstance();
	void* const otherOfMain = main ? AkimboBridge_GetOtherWeapon(main) : nullptr;

	for (int hand = 0; hand < 3; ++hand)
	{
		void* const w = GetActive(pPlayer, hand);
		if (!w || !AkimboBridge_IsAkimboWeapon(w))
			continue;
		if (w != main && otherOfMain != w)
			continue;

		const AkimboWeaponConfig& config = GetAkimboConfig(GetWeaponClassNameRaw(w));
		if (config.cbStateChanged.empty())
			continue;

		const HSCRIPT hWeaponScript = reinterpret_cast<CBaseEntity*>(w)->GetScriptInstance();
		if (!hWeaponScript || !hPlayerScript)
			continue;

		ScriptVariant_t args[3] = { hWeaponScript, hPlayerScript, ScriptVariant_t(newState) };
		InvokeGlobalClosure(config.cbStateChanged.c_str(), args, 3, nullptr);
	}

	// Every active akimbo weapon restarts its cadence from the pair's latest
	// shot, so a transition never hands either hand an immediate shot.
	if (v_WeaponFireInterval)
	{
		for (int hand = 0; hand < 3; ++hand)
		{
			void* const w = GetActive(pPlayer, hand);
			if (!w || !AkimboBridge_IsAkimboWeapon(w))
				continue;

			float last = 0.0f;
			Akimbo_GetWeaponFloat(w, "m_lastPrimaryAttackTime", &last);
			void* const other = AkimboBridge_GetOtherWeapon(w);
			if (other)
			{
				float otherLast = 0.0f;
				Akimbo_GetWeaponFloat(other, "m_lastPrimaryAttackTime", &otherLast);
				last = fmaxf(last, otherLast);
			}

			float ready = 0.0f;
			float primary = 0.0f;
			Akimbo_GetWeaponFloat(w, "m_nextReadyTime", &ready);
			Akimbo_GetWeaponFloat(w, "m_nextPrimaryAttackTime", &primary);
			const float next = fmaxf(last + v_WeaponFireInterval(reinterpret_cast<__int64>(w)), fmaxf(ready, primary));
			Akimbo_SetWeaponFloat(w, "m_nextReadyTime", next);
			Akimbo_SetWeaponFloat(w, "m_nextPrimaryAttackTime", fmaxf(primary, next));
		}
	}

	static bool s_loggedFirstTransition = false;
	if (!s_loggedFirstTransition || bridge_akimbo_diag.GetBool())
	{
		s_loggedFirstTransition = true;
		Msg(eDLL_T::SERVER, "[Akimbo] state %d -> %d player=%p main=%p\n",
			oldState, newState, pPlayer, main);
	}
}

void AkimboBridge_UpdateState(void* pPlayer)
{
	if (!pPlayer)
		return;

	void* const main = GetActive(pPlayer, 0);
	if (!main)
		return;

	void* const other = AkimboBridge_IsAkimboWeapon(main) ? AkimboBridge_GetOtherWeapon(main) : nullptr;
	void* const alt = GetActive(pPlayer, 1);
	const int flags = GetWeaponDisabledFlags(pPlayer) & 3;

	const int preState = GetAkimboState(pPlayer);

	if (!other)
		SetState(pPlayer, AKIMBO_STATE_NONE);
	else if (AkimboBridge_IsDisabled(main))
		SetState(pPlayer, AKIMBO_STATE_SINGLE);
	else if (flags == 0 && alt == other)
		SetState(pPlayer, AKIMBO_STATE_ACTIVE);
	else
		SetState(pPlayer, AKIMBO_STATE_OFFHAND);

	// An ACTIVE exit names the clearer: whoever emptied alt, set flags,
	// or dropped the partner shows up here.
	const int postState = GetAkimboState(pPlayer);
	if (preState == AKIMBO_STATE_ACTIVE && postState != AKIMBO_STATE_ACTIVE)
	{
		static int s_nActiveExitLog = 16;
		if (s_nActiveExitLog > 0 || bridge_akimbo_diag.GetBool())
		{
			if (s_nActiveExitLog > 0)
				--s_nActiveExitLog;
			Msg(eDLL_T::SERVER,
				"[Akimbo] ACTIVE exit %d -> %d main=%p other=%p alt=%p flags=%d\n",
				preState, postState, main, other, alt, flags);
		}
	}
}

static void ExecuteToggle(void* pPlayer)
{
	void* const main = GetActive(pPlayer, 0);
	if (!main || !AkimboBridge_IsAkimboWeapon(main))
		return;

	void* const other = AkimboBridge_GetOtherWeapon(main);
	void* const alt = GetActive(pPlayer, 1);
	const float now = gpGlobals->curTime;

	float mainReady = 0.0f;
	Akimbo_GetWeaponFloat(main, "m_nextReadyTime", &mainReady);
	if (now < mainReady)
		return;

	if (alt)
	{
		float altReady = 0.0f;
		Akimbo_GetWeaponFloat(alt, "m_nextReadyTime", &altReady);
		if (now < altReady)
			return;
	}

	SetDisabled(main, !AkimboBridge_IsDisabled(main));
	const bool nowDisabled = AkimboBridge_IsDisabled(main);

	if (nowDisabled)
	{
		SetSelectedAlt(pPlayer, kSelectedSlotEmpty);
		if (alt && v_HolsterInternal)
			v_HolsterInternal(reinterpret_cast<__int64>(alt), 0);
	}
	else if (other && Weapon_IsEnabled(pPlayer, other))
	{
		if (!alt || alt == other)
		{
			SetSelectedAlt(pPlayer, kSelectedSlotInvalid);
			if (v_SetActiveWeapon)
				v_SetActiveWeapon(pPlayer, 1, reinterpret_cast<__int64>(other));
			if (v_DeployWeapon)
				v_DeployWeapon(reinterpret_cast<__int64>(other), 0);
		}
		else if (v_WeaponSwitch)
		{
			v_WeaponSwitch(pPlayer, 1, reinterpret_cast<__int64>(other));
		}
	}

	AkimboBridge_UpdateState(pPlayer);

	static bool s_loggedFirstToggle = false;
	if (!s_loggedFirstToggle || bridge_akimbo_diag.GetBool())
	{
		s_loggedFirstToggle = true;
		Msg(eDLL_T::SERVER, "[Akimbo] toggle player=%p disabled=%d alt=%p other=%p\n",
			pPlayer, nowDisabled ? 1 : 0, alt, other);
	}
}

// Average the pair's clips; the odd round stays on the fuller hand and that
// hand shoots next.
static void BalanceClipAmmo(void* pPlayer, void* pWeapon)
{
	if (!pPlayer || !pWeapon)
		return;

	void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
	if (!other)
		return;

	void* main = pWeapon;
	void* alt = other;
	if (GetActive(pPlayer, 0) != pWeapon)
	{
		main = other;
		alt = pWeapon;
	}

	int clipMain = 0;
	int clipAlt = 0;
	Akimbo_GetWeaponInt(main, "m_ammoInClip", &clipMain);
	Akimbo_GetWeaponInt(alt, "m_ammoInClip", &clipAlt);

	const int d = clipMain - clipAlt;
	if (d == 0)
		return;

	const int newMain = clipMain - d / 2;
	const int newAlt = newMain - d % 2;
	Akimbo_SetWeaponInt(main, "m_ammoInClip", newMain);
	Akimbo_SetWeaponInt(alt, "m_ammoInClip", newAlt);

	SetBccI32(pPlayer, offsetof(BCCExtendWire, m_akimboShouldAltFire), (newAlt > newMain) ? 1 : 0);

	if (bridge_akimbo_diag.GetBool())
		Msg(eDLL_T::SERVER, "[Akimbo] balance main %d->%d alt %d->%d shouldAlt=%d\n",
			clipMain, newMain, clipAlt, newAlt, (newAlt > newMain) ? 1 : 0);
}

// A shot on one hand pushes the partner's ready/attack times out to this
// hand's next attack time, so the pair shares one fire cadence.
static void CouplePartnerAttackTime(void* pWeapon, float readyBefore)
{
	float readyAfter = 0.0f;
	if (!Akimbo_GetWeaponFloat(pWeapon, "m_nextReadyTime", &readyAfter) || readyAfter == readyBefore)
		return;

	void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
	if (!other)
		return;

	float otherReady = 0.0f;
	float otherPrimary = 0.0f;
	Akimbo_GetWeaponFloat(other, "m_nextReadyTime", &otherReady);
	Akimbo_GetWeaponFloat(other, "m_nextPrimaryAttackTime", &otherPrimary);

	const float coupled = fmaxf(readyAfter, fmaxf(otherReady, otherPrimary));
	Akimbo_SetWeaponFloat(other, "m_nextReadyTime", coupled);
	Akimbo_SetWeaponFloat(other, "m_nextPrimaryAttackTime", fmaxf(otherPrimary, coupled));

	if (bridge_akimbo_diag.GetBool())
		Msg(eDLL_T::SERVER, "[Akimbo] couple weapon=%p ready=%.3f -> other=%p ready %.3f->%.3f\n",
			pWeapon, readyAfter, other, otherReady, coupled);
}

static __int64 __fastcall Hook_ChargeAndPrimaryAttack(__int64 weapon, __int64 a2, __int64 a3)
{
	if (!bridge_akimbo.GetBool() || !weapon)
		return v_ChargeAndPrimaryAttack(weapon, a2, a3);

	void* const pWeapon = reinterpret_cast<void*>(weapon);
	void* const owner = GetOwner(pWeapon);
	if (!owner || GetAkimboState(owner) != AKIMBO_STATE_ACTIVE)
		return v_ChargeAndPrimaryAttack(weapon, a2, a3);

	const bool shouldAlt = BCCExtend_GetI32(owner, offsetof(BCCExtendWire, m_akimboShouldAltFire)) != 0;
	const bool althand = AkimboBridge_IsAlthand(pWeapon);
	static int s_nFireLog = 64;
	if (s_nFireLog > 0 || bridge_akimbo_diag.GetBool())
	{
		if (s_nFireLog > 0)
			--s_nFireLog;
		Msg(eDLL_T::SERVER, "[Akimbo] fire weapon=%p hand=%d althand=%d shouldAlt=%d btn=0x%X t=%.3f -> %s\n",
			pWeapon, *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(pWeapon) + kWeaponActiveHandOffset),
			althand ? 1 : 0, shouldAlt ? 1 : 0,
			*reinterpret_cast<const unsigned int*>(reinterpret_cast<const uint8_t*>(owner) + kPlayerButtonsOffset),
			gpGlobals->curTime, althand != shouldAlt ? "blocked" : "fire");
	}
	if (althand != shouldAlt)
		return 0;
	SetBccI32(owner, offsetof(BCCExtendWire, m_akimboShouldAltFire), shouldAlt ? 0 : 1);

	float readyBefore = 0.0f;
	Akimbo_GetWeaponFloat(pWeapon, "m_nextReadyTime", &readyBefore);

	const __int64 result = v_ChargeAndPrimaryAttack(weapon, a2, a3);
	if (result)
		CouplePartnerAttackTime(pWeapon, readyBefore);
	return result;
}

// The engine raise only runs on its immediate switch path; the SetActiveWeapon
// tail (AkimboBridge_OnSetActiveWeapon) covers every path instead.
// The engine's one-handed translate stops at ACT_VM_ONEHANDED_RELOAD*; while the
// owner dual wields, the akimbo reload clips are the ones the client plays, so
// the server picks the same sequence and both reload timelines agree.
static constexpr int kAkimboReloadActs = 12;
static const char* const s_oneHandedReloadActs[kAkimboReloadActs] = {
	"ACT_VM_ONEHANDED_RELOAD",
	"ACT_VM_ONEHANDED_RELOAD_LATE1",
	"ACT_VM_ONEHANDED_RELOAD_LATE2",
	"ACT_VM_ONEHANDED_RELOAD_LATE3",
	"ACT_VM_ONEHANDED_RELOAD_LATE4",
	"ACT_VM_ONEHANDED_RELOAD_LATE5",
	"ACT_VM_ONEHANDED_RELOADEMPTY",
	"ACT_VM_ONEHANDED_RELOADEMPTY_LATE1",
	"ACT_VM_ONEHANDED_RELOADEMPTY_LATE2",
	"ACT_VM_ONEHANDED_RELOADEMPTY_LATE3",
	"ACT_VM_ONEHANDED_RELOADEMPTY_LATE4",
	"ACT_VM_ONEHANDED_RELOADEMPTY_LATE5",
};
static const char* const s_akimboReloadActs[kAkimboReloadActs] = {
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE1",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE2",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE3",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE4",
	"ACT_VM_ONEHANDED_AKIMBO_RELOAD_LATE5",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE1",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE2",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE3",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE4",
	"ACT_VM_ONEHANDED_AKIMBO_RELOADEMPTY_LATE5",
};

static int s_reloadActFrom[kAkimboReloadActs];
static int s_reloadActTo[kAkimboReloadActs];
static int s_reloadActGeneration = -1;
static bool s_reloadActsOk = false;

static bool ResolveAkimboReloadActs(void)
{
	const int gen = ActivityList_Generation();
	if (gen == s_reloadActGeneration)
		return s_reloadActsOk;
	s_reloadActGeneration = gen;
	s_reloadActsOk = true;
	for (int i = 0; i < kAkimboReloadActs; i++)
	{
		s_reloadActFrom[i] = FindActivityByName(s_oneHandedReloadActs[i]);
		s_reloadActTo[i]   = FindActivityByName(s_akimboReloadActs[i]);
		if (s_reloadActFrom[i] < 0 || s_reloadActTo[i] < 0)
			s_reloadActsOk = false;
	}
	if (!s_reloadActsOk)
		Warning(eDLL_T::SERVER, "[Akimbo] one-handed/akimbo reload activities unresolved -- server keeps one-handed reload clips\n");
	else
		Msg(eDLL_T::SERVER, "[Akimbo] akimbo reload activities %d..%d\n", s_reloadActTo[0], s_reloadActTo[kAkimboReloadActs - 1]);
	return s_reloadActsOk;
}

static __int64 __fastcall Hook_WeaponTranslateActivity(__int64 weapon, unsigned int act)
{
	const __int64 translated = v_WeaponTranslateActivity(weapon, act);
	if (!bridge_akimbo.GetBool() || !weapon)
		return translated;

	void* const pWeapon = reinterpret_cast<void*>(weapon);
	void* const owner = GetOwner(pWeapon);
	if (!owner || GetAkimboState(owner) != AKIMBO_STATE_ACTIVE || !ResolveAkimboReloadActs())
		return translated;

	for (int i = 0; i < kAkimboReloadActs; i++)
	{
		if (static_cast<int>(translated) != s_reloadActFrom[i])
			continue;
		if (bridge_akimbo_diag.GetBool())
			Msg(eDLL_T::SERVER, "[Akimbo] translate weapon=%p act %u -> %d -> %d\n", pWeapon, act, static_cast<int>(translated), s_reloadActTo[i]);
		return s_reloadActTo[i];
	}
	return translated;
}

// The engine pushes `althand` for hand 1 but never `dualwield`; the client
// adds it whenever the owner is OFFHAND or ACTIVE, and every dual clip
// requires it. Callers hand in a 36-slot buffer.
static constexpr unsigned int kActivityModifierSlots = 36;

static unsigned int __fastcall Hook_WeaponActivityModifiers(__int64 weapon, uint16_t* pMods)
{
	unsigned int count = v_WeaponActivityModifiers(weapon, pMods);
	if (!bridge_akimbo.GetBool() || !weapon || !pMods || count >= kActivityModifierSlots)
		return count;

	void* const pWeapon = reinterpret_cast<void*>(weapon);
	if (!AkimboBridge_IsAkimboWeapon(pWeapon))
		return count;
	void* const owner = GetOwner(pWeapon);
	if (!owner || GetAkimboState(owner) < AKIMBO_STATE_OFFHAND)
		return count;

	static CUtlSymbol s_dualwield;
	if (!s_dualwield.IsValid())
	{
		s_dualwield = FindActivityModifier("dualwield");
		if (!s_dualwield.IsValid())
		{
			static bool s_warned = false;
			if (!s_warned)
			{
				s_warned = true;
				Warning(eDLL_T::SERVER, "[Akimbo] activity modifier 'dualwield' unresolved -- dedi keeps one-handed clips\n");
			}
			return count;
		}
	}

	pMods[count++] = static_cast<uint16_t>(static_cast<unsigned int>(s_dualwield));
	return count;
}

// An offhand on the main hand deactivates an akimbo alt in hand 1, the way
// the akimbo-aware engine does; SwitchAlthand raises it again once the
// main hand returns to the pistol.
static void ClearAkimboAltHand(void* player, void* offhand)
{
	if (!v_SetActiveWeapon)
		return;
	void* const alt = GetActive(player, 1);
	if (!alt || Weapon_IsOffhand(alt) || !AkimboBridge_IsAkimboWeapon(alt) || !AkimboBridge_IsAlthand(alt))
		return;
	v_SetActiveWeapon(player, 1, 0);
	static int s_nLog = 8;
	if (s_nLog > 0 || bridge_akimbo_diag.GetBool())
	{
		if (s_nLog > 0)
			--s_nLog;
		Msg(eDLL_T::SERVER, "[Akimbo] offhand %p takes main hand, alt %p cleared\n", offhand, alt);
	}
}

static char __fastcall Hook_SwitchToOffhand(void* player, __int64 offhand)
{
	if (bridge_akimbo.GetBool() && player && offhand
		&& *reinterpret_cast<const int*>(offhand + kWeaponActiveHandOffset) == 0)
		ClearAkimboAltHand(player, reinterpret_cast<void*>(offhand));
	return v_SwitchToOffhand(player, offhand);
}

static __int64 __fastcall Hook_DualWieldPartnerRaise(void* player, __int64 weapon)
{
	if (bridge_akimbo.GetBool())
		return 0;

	return v_DualWieldPartnerRaise(player, weapon);
}

static thread_local int s_fillDepth = 0;

static void __fastcall Hook_FillClipAmmoFromStock(__int64 weapon, __int64 unused)
{
	v_FillClipAmmoFromStock(weapon, unused);

	if (!bridge_akimbo.GetBool() || !weapon || s_fillDepth > 0)
		return;

	void* const pWeapon = reinterpret_cast<void*>(weapon);
	void* const owner = GetOwner(pWeapon);
	if (!owner || GetAkimboState(owner) == AKIMBO_STATE_NONE)
		return;
	if (!AkimboBridge_IsAkimboWeapon(pWeapon) || AkimboBridge_IsAlthand(pWeapon))
		return;

	void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
	if (other && other != pWeapon)
	{
		++s_fillDepth;
		v_FillClipAmmoFromStock(reinterpret_cast<__int64>(other), unused);
		--s_fillDepth;
	}

	if (GetAkimboState(owner) == AKIMBO_STATE_ACTIVE)
		BalanceClipAmmo(owner, pWeapon);
}

static thread_local int s_holsterDepth = 0;

// Runs after an executed engine holster from the shared HolsterInternal hook.
void AkimboBridge_PostHolster(void* pWeapon, bool fastHolster, void* pCaller)
{
	if (!bridge_akimbo.GetBool() || !pWeapon || !v_HolsterInternal || s_holsterDepth > 0)
		return;

	void* const owner = GetOwner(pWeapon);
	if (!owner || GetAkimboState(owner) != AKIMBO_STATE_ACTIVE
		|| !AkimboBridge_IsAkimboWeapon(pWeapon) || AkimboBridge_IsAlthand(pWeapon))
		return;

	void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
	if (!other)
		return;

	++s_holsterDepth;
	v_HolsterInternal(reinterpret_cast<__int64>(other), fastHolster ? 1 : 0);
	--s_holsterDepth;
	if (bridge_akimbo_diag.GetBool())
	{
		const uintptr_t base = g_GameDll.GetModuleBase();
		Msg(eDLL_T::SERVER, "[Akimbo] holster cascade main=%p other=%p fast=%d caller=0x%llX\n", pWeapon, other,
			fastHolster ? 1 : 0, static_cast<unsigned long long>(0x140000000ull + (reinterpret_cast<uintptr_t>(pCaller) - base)));
	}
}

static thread_local int s_deployDepth = 0;

// A partner that has never deployed pulls the mainhand back into the
// first-draw with it, so both ready times come from the same sequence.
// Returns the partner to redeploy after the engine deploy.
void* AkimboBridge_PreDeploy(void* pWeapon)
{
	if (!bridge_akimbo.GetBool() || !pWeapon || !v_DeployWeapon || s_deployDepth > 0)
		return nullptr;
	if (!AkimboBridge_IsAkimboWeapon(pWeapon) || !AkimboBridge_IsAlthand(pWeapon))
		return nullptr;

	void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
	bool thisFirst = true;
	bool otherFirst = true;
	Akimbo_GetWeaponBool(pWeapon, "m_didFirstDeploy", &thisFirst);
	if (!other || !Akimbo_GetWeaponBool(other, "m_didFirstDeploy", &otherFirst)
		|| (thisFirst && otherFirst))
		return nullptr;

	Akimbo_SetWeaponBool(pWeapon, "m_didFirstDeploy", false);
	return other;
}

void AkimboBridge_PostDeploy(void* pWeapon, void* pRedeploy)
{
	if (!bridge_akimbo.GetBool() || !pWeapon)
		return;

	if (pRedeploy && v_DeployWeapon)
	{
		Akimbo_SetWeaponBool(pRedeploy, "m_didFirstDeploy", false);
		++s_deployDepth;
		v_DeployWeapon(reinterpret_cast<__int64>(pRedeploy), 0);
		--s_deployDepth;
		if (bridge_akimbo_diag.GetBool())
			Msg(eDLL_T::SERVER, "[Akimbo] first-deploy cascade alt=%p main=%p\n", pWeapon, pRedeploy);
	}

	if (s_deployDepth > 0)
		return;

	void* const owner = GetOwner(pWeapon);
	if (owner)
		AkimboBridge_UpdateState(owner);
}

bool AkimboBridge_CanActivateAlthand(void* pPlayer, void* pWeapon)
{
	if (!bridge_akimbo.GetBool() || !pWeapon || !AkimboBridge_IsAkimboWeapon(pWeapon))
		return true;

	void* const main = GetActive(pPlayer, 0);
	const bool ok = AkimboBridge_IsAlthand(pWeapon)
		&& main
		&& !AkimboBridge_IsDisabled(main)
		&& AkimboBridge_GetOtherWeapon(main) == pWeapon;

	if (!ok)
	{
		static int s_nRefuseLog = 8;
		if (s_nRefuseLog > 0)
		{
			--s_nRefuseLog;
			Warning(eDLL_T::SERVER,
				"[Akimbo] refused althand activate player=%p weapon=%p\n",
				pPlayer, pWeapon);
		}
	}

	return ok;
}

static bool Weapon_IsOffhand(const void* pWeapon)
{
	const int fireMode = *reinterpret_cast<const int*>(
		reinterpret_cast<const uint8_t*>(pWeapon) + kWeaponFireModeOffset);
	return static_cast<unsigned>(fireMode - 1) <= 4u;
}

// Runs after every mainhand SetActiveWeapon. The engine raises the partner
// only on its immediate switch path; a usercmd select finishes through the
// holster-first path, which sets the hand and deploys without raising.
static void SwitchAlthand(void* pPlayer, void* pMain)
{
	void* const other = (AkimboBridge_IsAkimboWeapon(pMain) && !AkimboBridge_IsDisabled(pMain))
		? AkimboBridge_GetOtherWeapon(pMain)
		: nullptr;
	void* const alt = GetActive(pPlayer, 1);

	if (other && Weapon_IsEnabled(pPlayer, other))
	{
		if (alt == other)
			return;
		SetSelectedAlt(pPlayer, kSelectedSlotInvalid);
		if (v_WeaponSwitch)
			v_WeaponSwitch(pPlayer, 1, reinterpret_cast<__int64>(other));
		static int s_nRaiseLog = 16;
		if (s_nRaiseLog > 0 || bridge_akimbo_diag.GetBool())
		{
			if (s_nRaiseLog > 0)
				--s_nRaiseLog;
			Msg(eDLL_T::SERVER, "[Akimbo] raise partner main=%p other=%p was=%p\n", pMain, other, alt);
		}
		return;
	}

	// A dual-slot weapon left in the alt hand stands down through its own
	// frame once the slot reads EMPTY.
	const int8_t sel = *reinterpret_cast<const int8_t*>(
		reinterpret_cast<uint8_t*>(pPlayer) + kSelectedWeaponsOffset + 1);
	const bool selIsDual = static_cast<uint8_t>(sel - 7) <= 4u;
	const bool altIsDual = alt && !Weapon_IsOffhand(alt) && AkimboBridge_IsAlthand(alt);
	if ((selIsDual || altIsDual) && sel != static_cast<int8_t>(WEAPON_INVENTORY_SLOT_HOLSTERED))
		SetSelectedAlt(pPlayer, kSelectedSlotEmpty);
}

// A zipline ride is one-handed: the alt hand stands down at mount start and
// comes back through the partner raise once the player is off the rope.
void AkimboBridge_OnZiplineMountStart(void* pPlayer)
{
	if (!bridge_akimbo.GetBool() || !pPlayer || !v_SetActiveWeapon)
		return;
	void* const alt = GetActive(pPlayer, 1);
	if (!alt || !AkimboBridge_IsAkimboWeapon(alt))
		return;
	v_SetActiveWeapon(pPlayer, 1, 0);
	static int s_nLog = 8;
	if (s_nLog > 0 || bridge_akimbo_diag.GetBool())
	{
		if (s_nLog > 0)
			--s_nLog;
		Msg(eDLL_T::SERVER, "[Akimbo] zipline mount: alt %p stands down\n", alt);
	}
}

void AkimboBridge_OnZiplineStop(void* pPlayer)
{
	if (!bridge_akimbo.GetBool() || !pPlayer)
		return;
	void* const main = GetActive(pPlayer, 0);
	if (!main || !AkimboBridge_IsAkimboWeapon(main))
		return;
	SwitchAlthand(pPlayer, main);
}

void AkimboBridge_OnSetActiveWeapon(void* pPlayer, unsigned int hand, void* pWeapon)
{
	if (!bridge_akimbo.GetBool() || !pPlayer)
		return;

	if (hand == 0 && pWeapon && Weapon_IsOffhand(pWeapon))
	{
		ClearAkimboAltHand(pPlayer, pWeapon);
	}
	else if (hand == 0 && pWeapon && !Weapon_IsOffhand(pWeapon))
	{
		SwitchAlthand(pPlayer, pWeapon);
		// Hand 1 restores from this slot after an offhand; the engine only
		// records it for the hand that switched.
		if (AkimboBridge_IsAkimboWeapon(pWeapon) && !AkimboBridge_IsAlthand(pWeapon))
		{
			void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
			uint32_t* const pLatest = reinterpret_cast<uint32_t*>(
				reinterpret_cast<uint8_t*>(pPlayer) + kLatestPrimaryWeaponsOffset + 4);
			const uint32_t eh = other ? EntityHandle(other) : 0xFFFFFFFFu;
			if (*pLatest != eh)
			{
				*pLatest = eh;
				MarkEntityEdictDirty(pPlayer);
			}
		}
	}

	AkimboBridge_UpdateState(pPlayer);
}

void AkimboBridge_Think(void* pPlayer, void* pUserCmd)
{
	static bool s_thinkReached = false;
	if (!s_thinkReached)
	{
		s_thinkReached = true;
		Msg(eDLL_T::SERVER, "[Akimbo] AkimboBridge_Think reached\n");
	}

	if (!bridge_akimbo.GetBool() || !pPlayer || !pUserCmd || !gpGlobals)
		return;

	const CUserCmd* const cmd = reinterpret_cast<const CUserCmd*>(pUserCmd);
	if (cmd->impulse & kS21ExtraFlag_ToggleAkimbo)
		ExecuteToggle(pPlayer);

	AkimboBridge_UpdateState(pPlayer);

	// Alt-hand input as the engine sees it while the pair is ACTIVE.
	static int s_nAltInputLog = 96;
	static int s_lastButtons = 0;
	if ((s_nAltInputLog > 0 || bridge_akimbo_diag.GetBool()) && cmd->buttons != s_lastButtons
		&& GetAkimboState(pPlayer) == AKIMBO_STATE_ACTIVE)
	{
		s_lastButtons = cmd->buttons;
		if (s_nAltInputLog > 0)
			--s_nAltInputLog;
		void* const alt = GetActive(pPlayer, 1);
		float ready = 0.0f;
		float primary = 0.0f;
		Akimbo_GetWeaponFloat(alt, "m_nextReadyTime", &ready);
		Akimbo_GetWeaponFloat(alt, "m_nextPrimaryAttackTime", &primary);
		const uint8_t* const a = reinterpret_cast<const uint8_t*>(alt);
		Msg(eDLL_T::SERVER, "[Akimbo] altinput cmdbtn=0x%X plbtn=0x%X pressed=0x%X alt=%p hand=%d state=%d ready=%.3f primary=%.3f t=%.3f sel=%d shouldAlt=%d\n",
			cmd->buttons,
			*reinterpret_cast<const unsigned int*>(reinterpret_cast<const uint8_t*>(pPlayer) + kPlayerButtonsOffset),
			*reinterpret_cast<const unsigned int*>(reinterpret_cast<const uint8_t*>(pPlayer) + kPlayerButtonsOffset + 4),
			alt, a ? *reinterpret_cast<const int*>(a + kWeaponActiveHandOffset) : -1,
			a ? *reinterpret_cast<const int*>(a + kWeaponStateOffset) : -1, ready, primary, gpGlobals->curTime,
			*reinterpret_cast<const int8_t*>(reinterpret_cast<const uint8_t*>(pPlayer) + kSelectedWeaponsOffset + 1),
			BCCExtend_GetI32(pPlayer, offsetof(BCCExtendWire, m_akimboShouldAltFire)));
	}
}

static SQRESULT Script_IsAkimboWeapon(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushbool(v, AkimboBridge_IsAkimboWeapon(pWeapon));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsAkimboAvailable(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushbool(v, AkimboBridge_GetOtherWeapon(pWeapon) != nullptr);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsAkimboDisabled(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushbool(v, AkimboBridge_IsDisabled(pWeapon));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_IsAkimboAlthand(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	sq_pushbool(v, AkimboBridge_IsAlthand(pWeapon));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetOtherAkimboWeapon(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
	if (!other)
	{
		sq_pushnull(v);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const HSCRIPT hScript = reinterpret_cast<CBaseEntity*>(other)->GetScriptInstance();
	if (hScript)
		sq_pushobject(v, *reinterpret_cast<HSQOBJECT*>(hScript));
	else
		sq_pushnull(v);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetAkimboSharedClipCount(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	int clip = 0;
	Akimbo_GetWeaponInt(pWeapon, "m_ammoInClip", &clip);

	void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
	int otherClip = 0;
	if (other)
		Akimbo_GetWeaponInt(other, "m_ammoInClip", &otherClip);

	sq_pushinteger(v, clip + otherClip);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetWeaponAkimboSharedAmmoCount(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)))
		return SQ_ERROR;

	SQInteger ammoSource = 0;
	sq_getinteger(v, 2, &ammoSource);

	int stock = 0;
	Akimbo_GetWeaponInt(pWeapon, "m_ammoInStockpile", &stock);

	// AMMOSOURCE_STOCKPILE (0) is the only source a partner contributes to.
	void* const other = (ammoSource == 0) ? AkimboBridge_GetOtherWeapon(pWeapon) : nullptr;
	if (other)
	{
		int otherStock = 0;
		Akimbo_GetWeaponInt(other, "m_ammoInStockpile", &otherStock);
		stock += otherStock;
	}

	sq_pushinteger(v, stock);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetAkimboState(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	sq_pushinteger(v, GetAkimboState(pPlayer));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_BalanceAkimboClipAmmo(HSQUIRRELVM v)
{
	void* pPlayer = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
		return SQ_ERROR;

	void* const pWeapon = ServerScript_EntityPtrFromStackIdx(v, 2);
	BalanceClipAmmo(pPlayer, pWeapon);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void AkimboBridge_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	if (!weaponStruct)
		return;

	DevMsg(eDLL_T::SERVER, "[Akimbo] Registering weapon akimbo functions\n");

	weaponStruct->AddFunction("IsAkimboWeapon", "Script_IsAkimboWeapon",
		"Returns true if this weapon is marked is_akimbo_weapon", "bool", "", false,
		Script_IsAkimboWeapon);

	weaponStruct->AddFunction("IsAkimboAvailable", "Script_IsAkimboAvailable",
		"Returns true if this weapon has an akimbo partner", "bool", "", false,
		Script_IsAkimboAvailable);

	weaponStruct->AddFunction("IsAkimboDisabled", "Script_IsAkimboDisabled",
		"Returns true if akimbo is currently disabled on this weapon", "bool", "", false,
		Script_IsAkimboDisabled);

	weaponStruct->AddFunction("IsAkimboAlthand", "Script_IsAkimboAlthand",
		"Returns true if this weapon is the akimbo althand", "bool", "", false,
		Script_IsAkimboAlthand);

	weaponStruct->AddFunction("GetOtherAkimboWeapon", "Script_GetOtherAkimboWeapon",
		"Returns the akimbo partner weapon, or null", "entity", "", false,
		Script_GetOtherAkimboWeapon);

	weaponStruct->AddFunction("GetAkimboSharedClipCount", "Script_GetAkimboSharedClipCount",
		"Returns clip ammo of this weapon plus its partner", "int", "", false,
		Script_GetAkimboSharedClipCount);

	weaponStruct->AddFunction("GetWeaponAkimboSharedAmmoCount", "Script_GetWeaponAkimboSharedAmmoCount",
		"Returns stockpile ammo of this weapon plus its partner", "int", "int ammoSource", false,
		Script_GetWeaponAkimboSharedAmmoCount);
}

void AkimboBridge_RegisterPlayerFuncs(ScriptClassDescriptor_t* playerStruct)
{
	if (!playerStruct)
		return;

	DevMsg(eDLL_T::SERVER, "[Akimbo] Registering player akimbo functions\n");

	playerStruct->AddFunction("GetAkimboState", "Script_GetAkimboState",
		"Returns the player's akimbo state (0=none, 1=single, 2=offhand, 3=active)", "int", "", false,
		Script_GetAkimboState);

	playerStruct->AddFunction("BalanceAkimboClipAmmo", "Script_BalanceAkimboClipAmmo",
		"Split clip ammo evenly between an akimbo pair", "void", "entity weapon", false,
		Script_BalanceAkimboClipAmmo);
}

void AkimboBridge_LevelShutdown()
{
	s_akimboWeaponServer.Clear();
	s_configCache.clear();
}

void VAkimboBridge::GetAdr(void) const
{
	LogFunAdr("CWeaponX::TranslateWeaponActivity", v_WeaponTranslateActivity);
	LogFunAdr("CWeaponX::ChargeAndPrimaryAttack", v_ChargeAndPrimaryAttack);
	LogFunAdr("DualWieldPartnerRaise", v_DualWieldPartnerRaise);
	LogFunAdr("CWeaponX::DeployWeapon", v_DeployWeapon);
	LogFunAdr("CWeaponX::HolsterInternal", v_HolsterInternal);
	LogFunAdr("Weapon_SwitchToOffhand", v_SwitchToOffhand);
	LogFunAdr("CWeaponX::FillClipAmmoFromStock", v_FillClipAmmoFromStock);
	LogFunAdr("CPlayer::Weapon_Switch", v_WeaponSwitch);
	LogFunAdr("CWeaponX::GetFireInterval", v_WeaponFireInterval);
	LogFunAdr("CWeaponX::GetActivityModifiers", v_WeaponActivityModifiers);
}

void VAkimboBridge::GetFun(void) const
{
	s_altHandInputImm[0] = AltHandInput_Site(Module_FindPattern(g_GameDll,
		"0F 2F 83 94 19 00 00 0F 87 ?? ?? ?? ?? B8 01 00 00 00 B9 00 00 03 00"), 19);
	s_altHandInputImm[1] = AltHandInput_Site(Module_FindPattern(g_GameDll,
		"48 8B CB E8 ?? ?? ?? ?? BD 01 00 00 00 41 BE 00 00 03 00"), 15);
	s_altHandInputImm[2] = AltHandInput_Site(Module_FindPattern(g_GameDll,
		"B8 01 00 00 00 4C 89 BC 24 88 00 00 00 45 32 FF 41 BD 00 00 03 00"), 18);
	s_altHandInputImm[3] = AltHandInput_Site(Module_FindPattern(g_GameDll,
		"41 85 87 E0 60 00 00 75 ?? B8 01 00 00 00 B9 00 00 03 00"), 15);
	s_altHandInputImm[4] = AltHandInput_Site(Module_FindPattern(g_GameDll,
		"B9 01 00 00 00 48 89 5C 24 70 39 8F 30 29 00 00 B8 00 00 03 00"), 17);
	s_altHandInputImm[5] = AltHandInput_Site(Module_FindPattern(g_GameDll,
		"84 C0 75 ?? 41 BC 01 00 00 00 B8 00 00 03 00 44 39 A7 30 29 00 00"), 11);
	s_altHandInputImm[6] = AltHandInput_Site(Module_FindPattern(g_GameDll,
		"80 BF 5C 1D 00 00 00 74 ?? B8 01 00 00 00 B9 00 00 03 00"), 15);
	s_altHandInputImm[7] = AltHandInput_Site(Module_FindPattern(g_GameDll,
		"44 39 B3 14 18 00 00 75 ?? 41 BF 01 00 00 00 B9 00 00 03 00"), 16);

	for (size_t i = 0; i < SDK_ARRAYSIZE(s_altHandInputImm); ++i)
	{
		if (!s_altHandInputImm[i])
			Warning(eDLL_T::SERVER, "[Akimbo] alt-hand input site '%s' unresolved -- that path keeps the zoom buttons\n",
				s_altHandInputSiteNames[i]);
	}

	Module_FindPattern(g_GameDll,
		"F3 0F 10 89 A4 1C 00 00 0F 57 C0 0F 2E C8 7A 02 74 11 80 B9 BC 1C 00 00 00 75 08 41 B0 01")
		.GetPtr(v_ChargeAndPrimaryAttack);
	if (!v_ChargeAndPrimaryAttack)
		Warning(eDLL_T::SERVER,
			"[Akimbo] CWeaponX::ChargeAndPrimaryAttack pattern unresolved -- fire gate disabled\n");

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 55 56 57 48 83 EC 20 48 8B 01 48 8B F2 48 8B D9 FF 90 58 0A 00 00 44 0F B6 83 D9 16 00 00")
		.GetPtr(v_DualWieldPartnerRaise);
	if (!v_DualWieldPartnerRaise)
		Warning(eDLL_T::SERVER,
			"[Akimbo] DualWieldPartnerRaise pattern unresolved -- disable-raise gate disabled\n");

	// Server twin: owner handle at +0x11F0.
	Module_FindPattern(g_GameDll,
		"40 53 56 41 54 48 83 EC 30 48 8B D9 0F B6 F2 8B 89 F0 11 00 00 8B C1 83 F9 FF")
		.GetPtr(v_DeployWeapon);
	if (!v_DeployWeapon)
		Warning(eDLL_T::SERVER,
			"[Akimbo] CWeaponX::DeployWeapon pattern unresolved -- deploy cascade disabled\n");

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 30 48 8B D9 0F B6 F2 8B 89 F0 11 00 00 8B C1 83 F9 FF")
		.GetPtr(v_HolsterInternal);
	if (!v_HolsterInternal)
		Warning(eDLL_T::SERVER,
			"[Akimbo] CWeaponX::HolsterInternal pattern unresolved -- holster cascade disabled\n");

	// Server twin: m_latestActiveInventorySlot read from the offhand at +0x2930.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 40 80 B9 18 17 00 00 00 48 8B FA 0F 29 74 24 30 48 8B D9 0F 57 F6 0F 84")
		.GetPtr(v_SwitchToOffhand);
	if (!v_SwitchToOffhand)
		Warning(eDLL_T::SERVER,
			"[Akimbo] Weapon_SwitchToOffhand pattern unresolved -- alt hand stays up under offhands\n");

	// Server twin: m_playerData.m_reloadMilestone at +0x12EC.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 99 EC 12 00 00 48 8B F9 83 3B 00 74 ?? 48 8D 8B 6C FF FF FF")
		.GetPtr(v_FillClipAmmoFromStock);
	if (!v_FillClipAmmoFromStock)
		Warning(eDLL_T::SERVER,
			"[Akimbo] CWeaponX::FillClipAmmoFromStock pattern unresolved -- partner reload disabled\n");

	// Server CWeaponX::TranslateWeaponActivity (one-handed table). The call rel32
	// stays literal: the client twin in the same binary is byte-identical otherwise.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 8B DA E8 83 FF FF FF 84 C0 0F 84 2E 03 00 00 8D 83 3F FE FF FF 83 F8 75 0F 87")
		.GetPtr(v_WeaponTranslateActivity);
	if (!v_WeaponTranslateActivity)
		Warning(eDLL_T::SERVER,
			"[Akimbo] CWeaponX::TranslateWeaponActivity pattern unresolved -- server keeps one-handed reload clips\n");

	Module_FindPattern(g_GameDll,
		"48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 63 EA 49 8B F8 48 8B F1 4D 85 C0 75 ?? 32 C0")
		.GetPtr(v_WeaponSwitch);
	if (!v_WeaponSwitch)
		Warning(eDLL_T::SERVER,
			"[Akimbo] CPlayer::Weapon_Switch pattern unresolved -- toggle over an occupied alt hand disabled\n");

	// fireDuration + 1 / fireRate; the server twin reads fireDuration at +0x180C.
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 0F 28 C8 0F 57 C0 0F 2E C8 7A 02 74 0C F3 0F 10 05 ?? ?? ?? ?? F3 0F 5E C1 F3 0F 58 83 0C 18 00 00")
		.GetPtr(v_WeaponFireInterval);
	if (!v_WeaponFireInterval)
		Warning(eDLL_T::SERVER,
			"[Akimbo] CWeaponX::GetFireInterval pattern unresolved -- state-change cadence resync disabled\n");

	// Server twin: owner handle at +0x11F0.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 8B 91 F0 11 00 00 83 FA FF 0F 84 ?? ?? ?? ?? 0F B7 C2 C1")
		.GetPtr(v_WeaponActivityModifiers);
	if (!v_WeaponActivityModifiers)
		Warning(eDLL_T::SERVER,
			"[Akimbo] CWeaponX::GetActivityModifiers pattern unresolved -- dedi keeps one-handed clips\n");
}

void VAkimboBridge::GetVar(void) const { }
void VAkimboBridge::GetCon(void) const { }

static void AltHandInput_Patch(const bool bAttach)
{
	const uint32_t want = bAttach ? kAltHandAttackButton : kAltHandZoomButtons;
	const uint32_t have = bAttach ? kAltHandZoomButtons : kAltHandAttackButton;
	int patched = 0;
	for (size_t i = 0; i < SDK_ARRAYSIZE(s_altHandInputImm); ++i)
	{
		uint32_t* const pImm = reinterpret_cast<uint32_t*>(s_altHandInputImm[i]);
		if (!pImm || *pImm != have)
			continue;
		DWORD oldProtect = 0;
		VirtualProtect(pImm, sizeof(*pImm), PAGE_EXECUTE_READWRITE, &oldProtect);
		*pImm = want;
		VirtualProtect(pImm, sizeof(*pImm), oldProtect, &oldProtect);
		++patched;
	}
	s_altHandInputPatched = bAttach && patched > 0;
	Msg(eDLL_T::SERVER, "[Akimbo] alt hand fires from %s (%d/%d sites)\n",
		bAttach ? "IN_ATTACK" : "the zoom buttons", patched, static_cast<int>(SDK_ARRAYSIZE(s_altHandInputImm)));
}

void VAkimboBridge::Detour(const bool bAttach) const
{
	if (bAttach ? (bridge_akimbo.GetBool() && ZoomGate_AkimboCanZoom()) : s_altHandInputPatched)
		AltHandInput_Patch(bAttach);

	if (v_ChargeAndPrimaryAttack)
		DetourSetup(&v_ChargeAndPrimaryAttack, &Hook_ChargeAndPrimaryAttack, bAttach);
	if (v_DualWieldPartnerRaise)
		DetourSetup(&v_DualWieldPartnerRaise, &Hook_DualWieldPartnerRaise, bAttach);
	if (v_SwitchToOffhand)
		DetourSetup(&v_SwitchToOffhand, &Hook_SwitchToOffhand, bAttach);
	if (v_FillClipAmmoFromStock)
		DetourSetup(&v_FillClipAmmoFromStock, &Hook_FillClipAmmoFromStock, bAttach);
	if (v_WeaponActivityModifiers)
		DetourSetup(&v_WeaponActivityModifiers, &Hook_WeaponActivityModifiers, bAttach);
	if (v_WeaponTranslateActivity)
		DetourSetup(&v_WeaponTranslateActivity, &Hook_WeaponTranslateActivity, bAttach);
}
