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

#include <unordered_map>
#include <string>
#include <fstream>
#include <cstddef>

extern CGlobalVars* gpGlobals;

static ConVar bridge_akimbo("bridge_akimbo", "0",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Enables the native akimbo state machine (0=off, 1=on). Off for OSS; "
	"flip with AKIMBO_DUALWIELD_ENABLED to re-enable the dual-pistol system.");

static ConVar bridge_akimbo_diag("bridge_akimbo_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log akimbo state transitions.");

// Every native switch pairs the active-array write with a DeployWeapon.
// SwitchAlthand raises through the inner write core (v_SetActiveWeapon is
// the non-deploying sibling of the deploying raise), so the partner deploy
// is part of the raise. Roll back with 0 for A/B.
static ConVar bridge_akimbo_deploy_partner("bridge_akimbo_deploy_partner", "1",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Deploy the akimbo partner when SwitchAlthand raises it (0=off, 1=on).");

// Must match client net_bridge_split.h kS21ExtraFlag_ToggleAkimbo (0x40).
// No shared header across the two products -- duplicated constant.
static constexpr uint8_t kS21ExtraFlag_ToggleAkimbo = 0x40;

static constexpr int WEAPON_CLASSNAME_OFFSET = 0x15B0; // m_weaponName[65]
static constexpr uintptr_t kServerInventoryOffset = 0x1688; // WeaponInventory
static constexpr uintptr_t kWeaponOwnerOffset = 0x11F0; // m_weaponOwner

static constexpr uintptr_t kActiveWeaponsOffset = 0x16CC; // activeWeapons[3]
static constexpr uintptr_t kSelectedWeaponsOffset = 0x16D8; // m_selectedWeapons[2] int8
static constexpr uintptr_t kLatestPrimaryWeaponsOffset = 0x16DC; // m_latestPrimaryWeapons[2]
static constexpr int kEntityHandleOffset = 8; // m_RefEHandle

static constexpr size_t MAX_AKIMBO_CONFIGS = 64;

struct AkimboWeaponConfig
{
	bool isAkimboWeapon = false;
	bool useAkimboDamageSource = false;
	bool akimboDeployHolstersAlthand = false;
	std::string cbStateChanged;
};

struct AkimboWeaponState
{
	bool disabled = false;
};

typedef __int64 (__fastcall* ChargeAndPrimaryAttack_t)(__int64 weapon, __int64 a2, __int64 a3);
typedef __int64 (__fastcall* DualWieldPartnerRaise_t)(void* player, __int64 weapon);
typedef char (__fastcall* DeployWeapon_t)(__int64 weapon, unsigned char force);

static ChargeAndPrimaryAttack_t v_ChargeAndPrimaryAttack = nullptr;
static DualWieldPartnerRaise_t v_DualWieldPartnerRaise = nullptr;
static DeployWeapon_t v_DeployWeapon = nullptr;

static SDKEntityMap<AkimboWeaponState> s_akimboWeaponServer(ESide::Server, "akimbo.wpn");
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

static void CountBraces(const std::string& line, int& depth)
{
	bool inQuote = false;
	for (size_t i = 0; i < line.size(); ++i)
	{
		const char c = line[i];
		if (c == '"')
			inQuote = !inQuote;
		else if (!inQuote)
		{
			if (c == '{')
				++depth;
			else if (c == '}')
			{
				--depth;
				if (depth < 0)
					depth = 0;
			}
		}
	}
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
	int depth = 0;
	while (std::getline(file, line))
	{
		size_t commentPos = line.find("//");
		if (commentPos != std::string::npos)
			line = line.substr(0, commentPos);

		Sdk_TrimWhitespace(line);
		if (line.empty())
			continue;

		const int lineDepth = depth;
		size_t pos = 0;
		std::string key, value;
		if (ParseQuotedString(line, pos, key) && ParseQuotedString(line, pos, value)
			&& lineDepth == 1)
		{
			if (key == "is_akimbo_weapon")
				config.isAkimboWeapon = (Sdk_ParseInt(value) != 0);
			else if (key == "use_akimbo_damage_source")
				config.useAkimboDamageSource = (Sdk_ParseInt(value) != 0);
			else if (key == "akimbo_deploy_holsters_althand")
				config.akimboDeployHolstersAlthand = (Sdk_ParseInt(value) != 0);
			else if (key == "OnWeaponAkimboStateChanged")
				config.cbStateChanged = value;
		}

		CountBraces(line, depth);
	}

	file.close();

	if (config.isAkimboWeapon)
	{
		Msg(eDLL_T::SERVER, "[Akimbo] Loaded '%s' is_akimbo_weapon=1\n",
			weaponClassName.c_str());
	}

	return config;
}

static const AkimboWeaponConfig& GetAkimboConfig(const char* weaponClassName)
{
	if (!weaponClassName || !*weaponClassName)
	{
		static AkimboWeaponConfig s_empty;
		return s_empty;
	}

	const uint64_t hash = AkimboConfigHash(weaponClassName);
	auto it = s_configCache.find(hash);
	if (it != s_configCache.end())
		return it->second;

	if (s_configCache.size() >= MAX_AKIMBO_CONFIGS)
	{
		static AkimboWeaponConfig s_empty;
		return s_empty;
	}

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

static int GetWeaponDisabledFlags(void* pPlayer)
{
	const int off = pPlayer ? DTExtend_FindNativePropOffset(pPlayer, "m_weaponDisabledFlags") : -1;
	if (off <= 0)
		return 0;

	return *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(pPlayer) + off);
}

static void SetBccI32(void* pPlayer, size_t fieldOff, int32_t value)
{
	BCCExtend_SetI32(pPlayer, fieldOff, value);
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

static bool Weapon_IsEnabled(void* pWeapon)
{
	bool allowed = true;
	bool discarded = false;
	if (Akimbo_GetWeaponBool(pWeapon, "m_allowedToUse", &allowed) == false)
		allowed = true;
	if (Akimbo_GetWeaponBool(pWeapon, "m_discarded", &discarded) == false)
		discarded = false;
	return allowed && !discarded;
}

static bool InvokeGlobalClosure(const std::string& closureName, ScriptVariant_t* args, unsigned int nArgs,
	ScriptVariant_t* pReturn)
{
	if (closureName.empty())
		return false;

	if (!g_pServerScript)
	{
		Warning(eDLL_T::SERVER, "[Akimbo] no server VM -- cannot fire '%s'\n", closureName.c_str());
		return false;
	}

	const HSCRIPT hFunc = g_pServerScript->FindFunction(closureName.c_str(), nullptr, nullptr);
	if (!hFunc)
	{
		Warning(eDLL_T::SERVER, "[Akimbo] closure '%s' not found in VM\n", closureName.c_str());
		return false;
	}

	g_pServerScript->ExecuteFunction(hFunc, args, nArgs, pReturn, nullptr);
	return true;
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

static void SetState(void* pPlayer, int newState, bool force = false)
{
	if (!pPlayer)
		return;

	const int oldState = BCCExtend_GetI32(pPlayer, offsetof(BCCExtendWire, m_akimboState));
	if (!force && oldState == newState)
		return;

	SetBccI32(pPlayer, offsetof(BCCExtendWire, m_akimboState), newState);

	void* const main = GetActive(pPlayer, 0);
	const HSCRIPT hMain = main
		? reinterpret_cast<CBaseEntity*>(main)->GetScriptInstance()
		: nullptr;

	if (hMain)
	{
		WeaponBridge_InvokeChangeMod(hMain, "akimbo_active", false);
		if (newState == AKIMBO_STATE_ACTIVE)
			WeaponBridge_InvokeChangeMod(hMain, "akimbo_active", true);

		WeaponBridge_InvokeChangeMod(hMain, "akimbo_offhand", false);
		if (newState == AKIMBO_STATE_OFFHAND)
			WeaponBridge_InvokeChangeMod(hMain, "akimbo_offhand", true);

		WeaponBridge_InvokeChangeMod(hMain, "akimbo_disable", false);
		if (newState == AKIMBO_STATE_SINGLE)
			WeaponBridge_InvokeChangeMod(hMain, "akimbo_disable", true);
	}

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
		InvokeGlobalClosure(config.cbStateChanged, args, 3, nullptr);
	}

	static bool s_loggedFirstTransition = false;
	if (!s_loggedFirstTransition || bridge_akimbo_diag.GetBool())
	{
		s_loggedFirstTransition = true;
		Msg(eDLL_T::SERVER, "[Akimbo] state %d -> %d player=%p main=%p\n",
			oldState, newState, pPlayer, main);
	}
}

// S3 weapon-kind dword at weapon+0x2750. Kinds 1..5 take the offhand path
// in SetActiveWeapon (ordnance/utility); anything else is a real gun.
static constexpr uintptr_t kWeaponKindOffset = 10064;

static void SwitchAlthand_NoteBail(int reason)
{
	static int s_lastReason = -1;
	static int s_nNote = 12;
	if (reason == s_lastReason)
		return;
	s_lastReason = reason;
	if (s_nNote <= 0 && !bridge_akimbo_diag.GetBool())
		return;
	if (s_nNote > 0)
		--s_nNote;
	Msg(eDLL_T::SERVER, "[Akimbo] SwitchAlthand bail reason=%d\n", reason);
}

static void SwitchAlthand(void* pPlayer, void* pMain)
{
	if (!pPlayer || !pMain || !v_SetActiveWeapon)
		return;
	if (!AkimboBridge_IsAkimboWeapon(pMain))
		return;
	if (AkimboBridge_IsAlthand(pMain) || AkimboBridge_IsDisabled(pMain))
		return;
	if ((GetWeaponDisabledFlags(pPlayer) & 3) != 0)
		return;

	void* const other = AkimboBridge_GetOtherWeapon(pMain);
	if (!other || !Weapon_IsEnabled(other))
	{
		SwitchAlthand_NoteBail(1);
		return;
	}

	void* const alt = GetActive(pPlayer, 1);
	if (alt == other)
		return;
	if (alt)
	{
		// Retail Akimbo_SwitchAlthand only stands down for a special
		// parked in the alt hand. A real gun there does not block the
		// raise, so neither do we. S3 parks ordnance in alt by default,
		// which is why a blanket occupied-alt bail never raises.
		const int kind = *reinterpret_cast<const int*>(
			reinterpret_cast<const uint8_t*>(alt) + kWeaponKindOffset);
		if ((kind - 1) <= 4)
		{
			SwitchAlthand_NoteBail(2);
			return;
		}
	}

	static bool s_reentry = false;
	if (s_reentry)
		return;

	// The raise below runs every usercmd via UpdateState. If the write
	// does not stick, do not machine-gun DeployWeapon; the diag line
	// names the fight instead.
	static float s_lastRaiseTime = 0.0f;
	const float now = gpGlobals ? gpGlobals->curTime : 0.0f;
	if (now - s_lastRaiseTime < 0.5f)
		return;

	s_reentry = true;

	int8_t* const pSel = reinterpret_cast<int8_t*>(
		reinterpret_cast<uint8_t*>(pPlayer) + kSelectedWeaponsOffset + 1);
	*pSel = -1;
	MarkEntityEdictDirty(pPlayer);

	v_SetActiveWeapon(pPlayer, 1, reinterpret_cast<__int64>(other));
	if (bridge_akimbo_deploy_partner.GetBool())
	{
		if (v_DeployWeapon)
			v_DeployWeapon(reinterpret_cast<__int64>(other), 0);
		else
		{
			static bool s_deployWarn = false;
			if (!s_deployWarn)
			{
				s_deployWarn = true;
				Warning(eDLL_T::SERVER,
					"[Akimbo] CWeaponX::DeployWeapon unresolved -- partner raised without deploy\n");
			}
		}
	}

	s_lastRaiseTime = now;
	s_reentry = false;

	static bool s_loggedFirstSwitch = false;
	if (!s_loggedFirstSwitch || bridge_akimbo_diag.GetBool())
	{
		s_loggedFirstSwitch = true;
		Msg(eDLL_T::SERVER, "[Akimbo] SwitchAlthand player=%p main=%p other=%p\n",
			pPlayer, pMain, other);
	}
}

void AkimboBridge_UpdateState(void* pPlayer)
{
	if (!pPlayer)
		return;

	void* const main = GetActive(pPlayer, 0);
	if (!main || !AkimboBridge_IsAkimboWeapon(main))
	{
		const int cur = BCCExtend_GetI32(pPlayer, offsetof(BCCExtendWire, m_akimboState));
		if (cur != AKIMBO_STATE_NONE)
			SetBccI32(pPlayer, offsetof(BCCExtendWire, m_akimboState), AKIMBO_STATE_NONE);
		return;
	}

	SwitchAlthand(pPlayer, main);

	void* const other = AkimboBridge_GetOtherWeapon(main);
	void* const alt = GetActive(pPlayer, 1);
	const int flags = GetWeaponDisabledFlags(pPlayer) & 3;

	const int preState = BCCExtend_GetI32(pPlayer, offsetof(BCCExtendWire, m_akimboState));

	if (!other)
		SetState(pPlayer, AKIMBO_STATE_NONE);
	else if (AkimboBridge_IsDisabled(main))
		SetState(pPlayer, AKIMBO_STATE_SINGLE);
	else if (flags == 0 && alt == other)
		SetState(pPlayer, AKIMBO_STATE_ACTIVE);
	else
		SetState(pPlayer, AKIMBO_STATE_OFFHAND);

	// An ACTIVE exit names the clearer: whoever emptied alt, set flags,
	// or dropped the partner between raises shows up here.
	const int postState = BCCExtend_GetI32(pPlayer, offsetof(BCCExtendWire, m_akimboState));
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
	if (!bridge_akimbo.GetBool())
		return;

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

	if ((GetWeaponDisabledFlags(pPlayer) & 3) != 0)
		return;

	SetDisabled(main, !AkimboBridge_IsDisabled(main));
	const bool nowDisabled = AkimboBridge_IsDisabled(main);

	if (other)
	{
		if (nowDisabled && other == alt)
		{
			if (v_SetActiveWeapon)
				v_SetActiveWeapon(pPlayer, 1, 0);
		}
		else if (!nowDisabled && Weapon_IsEnabled(other) && !alt)
		{
			int8_t* const pSel = reinterpret_cast<int8_t*>(
				reinterpret_cast<uint8_t*>(pPlayer) + kSelectedWeaponsOffset + 1);
			*pSel = -1;
			MarkEntityEdictDirty(pPlayer);
			if (v_SetActiveWeapon)
				v_SetActiveWeapon(pPlayer, 1, reinterpret_cast<__int64>(other));
			if (v_DeployWeapon)
				v_DeployWeapon(reinterpret_cast<__int64>(other), 0);
			else
			{
				static bool s_deployWarn = false;
				if (!s_deployWarn)
				{
					s_deployWarn = true;
					Warning(eDLL_T::SERVER,
						"[Akimbo] CWeaponX::DeployWeapon unresolved -- partner raise skipped\n");
				}
			}
		}
	}

	AkimboBridge_UpdateState(pPlayer);

	if (Weapon_IsEnabled(main))
	{
		Akimbo_SetWeaponInt(main, "m_weapState", 0);
		if (v_DeployWeapon)
			v_DeployWeapon(reinterpret_cast<__int64>(main), 0);
	}

	static bool s_loggedFirstToggle = false;
	if (!s_loggedFirstToggle)
	{
		s_loggedFirstToggle = true;
		Msg(eDLL_T::SERVER, "[Akimbo] toggle player=%p disabled=%d\n",
			pPlayer, nowDisabled ? 1 : 0);
	}
}

static __int64 __fastcall Hook_ChargeAndPrimaryAttack(__int64 weapon, __int64 a2, __int64 a3)
{
	if (bridge_akimbo.GetBool() && weapon)
	{
		void* const pWeapon = reinterpret_cast<void*>(weapon);
		void* const owner = GetOwner(pWeapon);
		if (owner && BCCExtend_GetI32(owner, offsetof(BCCExtendWire, m_akimboState)) == AKIMBO_STATE_ACTIVE)
		{
			const bool shouldAlt = BCCExtend_GetI32(owner, offsetof(BCCExtendWire, m_akimboShouldAltFire)) != 0;
			if (AkimboBridge_IsAlthand(pWeapon) != shouldAlt)
				return 0;
			SetBccI32(owner, offsetof(BCCExtendWire, m_akimboShouldAltFire), shouldAlt ? 0 : 1);
		}
	}

	return v_ChargeAndPrimaryAttack(weapon, a2, a3);
}

static __int64 __fastcall Hook_DualWieldPartnerRaise(void* player, __int64 weapon)
{
	// The engine raise lifts weapons[main+7] into the alt hand (selectedWeapons
	// marker plus the alt activate). The slot patch repoints it at the akimbo
	// partner, so a live pair keeps the engine's own raise. Skip it only when
	// disabled: SINGLE must keep the partner down across mainhand selects.
	if (bridge_akimbo.GetBool() && weapon)
	{
		void* const pWeapon = reinterpret_cast<void*>(weapon);
		if (AkimboBridge_IsAkimboWeapon(pWeapon) && AkimboBridge_IsDisabled(pWeapon))
			return 0;
	}

	return v_DualWieldPartnerRaise(player, weapon);
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

void AkimboBridge_OnSetActiveWeapon(void* pPlayer, unsigned int hand, void* pWeapon)
{
	if (!bridge_akimbo.GetBool() || !pPlayer)
		return;

	if (hand == 0 && pWeapon)
		SwitchAlthand(pPlayer, pWeapon);

	// No DeployWeapon here: every caller of the hooked write core returns
	// into a wrapper that deploys the new weapon itself. SwitchAlthand
	// calls the core directly, so its deploy lives there instead.
	AkimboBridge_UpdateState(pPlayer);

	if (pWeapon && AkimboBridge_IsAkimboWeapon(pWeapon) && !AkimboBridge_IsAlthand(pWeapon))
	{
		void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
		uint32_t* const pLatest = reinterpret_cast<uint32_t*>(
			reinterpret_cast<uint8_t*>(pPlayer) + kLatestPrimaryWeaponsOffset + 4);
		const uint32_t newEh = other ? EntityHandle(other) : 0xFFFFFFFFu;
		if (*pLatest != newEh)
		{
			*pLatest = newEh;
			MarkEntityEdictDirty(pPlayer);
		}
	}
}

void AkimboBridge_Think(void* pPlayer, void* pUserCmd)
{
	static bool s_thinkReached = false;
	if (!s_thinkReached)
	{
		s_thinkReached = true;
		Warning(eDLL_T::SERVER, "[Akimbo] AkimboBridge_Think reached\n");
	}

	if (!bridge_akimbo.GetBool() || !pPlayer || !pUserCmd || !gpGlobals)
		return;

	const CUserCmd* const cmd = reinterpret_cast<const CUserCmd*>(pUserCmd);
	if (cmd->impulse & kS21ExtraFlag_ToggleAkimbo)
		ExecuteToggle(pPlayer);

	AkimboBridge_UpdateState(pPlayer);
}

static void BalanceClipAmmo(void* pPlayer, void* pWeapon)
{
	if (!pPlayer || !pWeapon)
		return;

	void* const main = GetActive(pPlayer, 0);
	if (main != pWeapon)
		return;

	void* const other = AkimboBridge_GetOtherWeapon(main);
	if (!other)
		return;

	int clipA = 0;
	int clipB = 0;
	Akimbo_GetWeaponInt(main, "m_ammoInClip", &clipA);
	Akimbo_GetWeaponInt(other, "m_ammoInClip", &clipB);

	const int total = clipA + clipB;
	const int a = total / 2 + total % 2;
	const int b = total / 2;
	Akimbo_SetWeaponInt(main, "m_ammoInClip", a);
	Akimbo_SetWeaponInt(other, "m_ammoInClip", b);

	SetBccI32(pPlayer, offsetof(BCCExtendWire, m_akimboShouldAltFire), (b > a) ? 1 : 0);
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
	(void)ammoSource; // source argument is not distinguished on this host

	int stock = 0;
	Akimbo_GetWeaponInt(pWeapon, "m_ammoInStockpile", &stock);

	void* const other = AkimboBridge_GetOtherWeapon(pWeapon);
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

	sq_pushinteger(v, BCCExtend_GetI32(pPlayer, offsetof(BCCExtendWire, m_akimboState)));
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
	LogFunAdr("CWeaponX::ChargeAndPrimaryAttack", v_ChargeAndPrimaryAttack);
	LogFunAdr("DualWieldPartnerRaise", v_DualWieldPartnerRaise);
	LogFunAdr("CWeaponX::DeployWeapon", v_DeployWeapon);
}

void VAkimboBridge::GetFun(void) const
{
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

	Module_FindPattern(g_GameDll,
		"40 53 56 41 54 48 83 EC 30 48 8B D9 0F B6 F2 8B 89 F0 11 00 00 8B C1 83 F9 FF")
		.GetPtr(v_DeployWeapon);
	if (!v_DeployWeapon)
		Warning(eDLL_T::SERVER,
			"[Akimbo] CWeaponX::DeployWeapon pattern unresolved -- toggle deploy disabled\n");
}

void VAkimboBridge::GetVar(void) const { }
void VAkimboBridge::GetCon(void) const { }

void VAkimboBridge::Detour(const bool bAttach) const
{
	if (v_ChargeAndPrimaryAttack)
		DetourSetup(&v_ChargeAndPrimaryAttack, &Hook_ChargeAndPrimaryAttack, bAttach);
	if (v_DualWieldPartnerRaise)
		DetourSetup(&v_DualWieldPartnerRaise, &Hook_DualWieldPartnerRaise, bAttach);
}
