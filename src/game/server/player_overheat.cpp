//=============================================================================//
//
// Purpose: S3 native gap-fills for ballistic player overheat. Backed by
// bridge-owned appended DT_BaseCombatCharacter props with no engine writer --
// pure SDK state that scripts own end-to-end.
//
//=============================================================================//

#include "core/stdafx.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/dt_extend.h"
#include "game/shared/player_extend_sidecar.h"
#include "game/shared/edict_dirty.h"
#include "game/server/player_overheat.h"
#include "game/server/baseentity.h"
#include "game/server/entitylist.h"
#include "game/server/r1/weapon_x.h"
#include "public/tier1/sdk_parse.h"
#include "public/tier1/strtools.h"
#include "public/globalvars_base.h"

#include <unordered_map>
#include <string>
#include <fstream>

extern CGlobalVars* gpGlobals;

// Server CWeapon inline classname char[65] (same slot as vscript_server diag).
static constexpr int SERVER_WEAPON_NAME_OFFSET = 0x15B0;

// Script MAX_DEBUFF_OVERHEAT (mp_ability_debuff_zone.nut).
static constexpr float PLAYER_OVERHEAT_MAX = 100.0f;

static constexpr size_t MAX_PLAYER_OVERHEAT_CONFIGS = 256;
static constexpr int PLAYER_OVERHEAT_DIAG_EVERY = 32;

static ConVar bridge_player_overheat("bridge_player_overheat", "1",
	FCVAR_RELEASE,
	"Accumulate player overheat while debuffed and raise CodeCallback_OnPlayerOverheat at full meter (0=off, 1=on).");

static ConVar bridge_player_overheat_diag("bridge_player_overheat_diag", "0",
	FCVAR_DEVELOPMENTONLY,
	"Log player-overheat accumulate/trip events (rate-limited).");

struct PlayerOverheatWeaponConfig
{
	bool doesPlayerOverheat = true;
	bool hasPerBullet = false;
	float perBullet = 0.0f;
};

static std::unordered_map<std::string, PlayerOverheatWeaponConfig> s_weaponOverheatConfigs;

//-----------------------------------------------------------------------------
// Resolve the implicit this. Callers must fail-soft -- SQ_ERROR here is a
// host shutdown.
//-----------------------------------------------------------------------------
static bool PlayerOverheat_ResolveEntity(HSQUIRRELVM v, void** ppEnt)
{
	void* pEnt = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEnt)) || !pEnt)
	{
		*ppEnt = nullptr;
		return false;
	}

	if (!DTExtend_EntityHasSendTable(pEnt, "DT_BaseCombatCharacter"))
	{
		static int s_nNonBccWarns = 0;
		if (s_nNonBccWarns++ < 8)
		{
			Warning(eDLL_T::SERVER,
				"[PlayerOverheat] entity is not a BaseCombatCharacter -- "
				"overheat props only exist on BCC send tables\n");
		}
		*ppEnt = nullptr;
		return false;
	}

	*ppEnt = pEnt;
	return true;
}

//-----------------------------------------------------------------------------
// CBaseHandle::Get is unimplemented here; resolve via g_serverEntityList
// (same pattern as JetDrive_ResolveHandle / ServerScript_LookupEntityFromRawHandle).
//-----------------------------------------------------------------------------
static void* PlayerOverheat_ResolveHandle(const EHANDLE& h)
{
	const uint32_t rawHandle = static_cast<uint32_t>(h.ToInt());
	if (rawHandle == INVALID_EHANDLE_INDEX || !g_serverEntityList)
		return nullptr;

	const CBaseHandle handle = CBaseHandle::UnsafeFromIndex(static_cast<int>(rawHandle));
	if (void* const pEntity = g_serverEntityList->LookupEntity(handle))
		return pEntity;

	const int entIndex = static_cast<int>(rawHandle & ENT_ENTRY_MASK);
	if (entIndex >= 0 && entIndex < NUM_ENT_ENTRIES)
		return g_serverEntityList->LookupEntityByNetworkIndex(entIndex);

	return nullptr;
}

// Owner via CWeaponX::GetWeaponOwnerHandle (m_weaponOwner @ 0x11F0 after CBaseAnimating).
static void* PlayerOverheat_GetWeaponOwner(void* pWeapon)
{
	if (!pWeapon)
		return nullptr;

	const EHANDLE& hOwner =
		reinterpret_cast<CWeaponX*>(pWeapon)->GetWeaponOwnerHandle();
	return PlayerOverheat_ResolveHandle(hOwner);
}

//-----------------------------------------------------------------------------
// does_player_overheat defaults true (explicit 0 disables). Absent per_bullet = no meter.
//-----------------------------------------------------------------------------
static PlayerOverheatWeaponConfig LoadPlayerOverheatWeaponConfigFromFile(
	const std::string& weaponClassName)
{
	PlayerOverheatWeaponConfig config;

	if (weaponClassName.find("..") != std::string::npos ||
		weaponClassName.find('/') != std::string::npos ||
		weaponClassName.find('\\') != std::string::npos)
	{
		Warning(eDLL_T::SERVER,
			"[PlayerOverheat] Rejected invalid weapon classname '%s'\n",
			weaponClassName.c_str());
		return config;
	}

	const char* searchPaths[] = {
		"platform/scripts/weapons/"
	};

	std::ifstream file;
	for (const char* basePath : searchPaths)
	{
		const std::string path = std::string(basePath) + weaponClassName + ".txt";
		// Failed open leaves failbit; open refuses to reopen without clear().
		file.clear();
		file.open(path);
		if (file.is_open())
			break;
	}

	if (!file.is_open())
		return config;

	std::string line;
	while (std::getline(file, line))
	{
		const size_t commentPos = line.find("//");
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

		if (key == "does_player_overheat")
		{
			config.doesPlayerOverheat = (Sdk_ParseInt(value) != 0);
		}
		else if (key == "player_overheat_per_bullet")
		{
			config.hasPerBullet = true;
			config.perBullet = static_cast<float>(Sdk_ParseFloat(value));
		}
	}

	return config;
}

static const PlayerOverheatWeaponConfig& GetPlayerOverheatWeaponConfig(
	const char* weaponClassName)
{
	static const PlayerOverheatWeaponConfig s_defaultConfig;

	if (!weaponClassName || !*weaponClassName)
		return s_defaultConfig;

	const std::string name(weaponClassName);
	const auto it = s_weaponOverheatConfigs.find(name);
	if (it != s_weaponOverheatConfigs.end())
		return it->second;

	if (s_weaponOverheatConfigs.size() >= MAX_PLAYER_OVERHEAT_CONFIGS)
		return s_defaultConfig;

	const PlayerOverheatWeaponConfig value =
		LoadPlayerOverheatWeaponConfigFromFile(name);
	const auto inserted = s_weaponOverheatConfigs.emplace(name, value);
	return inserted.first->second;
}

static bool GetDoesWeaponPlayerOverheat(const char* weaponClassName)
{
	return GetPlayerOverheatWeaponConfig(weaponClassName).doesPlayerOverheat;
}

//-----------------------------------------------------------------------------
// Script natives -- combat character
//-----------------------------------------------------------------------------
static SQRESULT Script_IsPlayerOverheating(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!PlayerOverheat_ResolveEntity(v, &pEnt))
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const bool bOverheating =
		BCCExtend_GetI32(pEnt, offsetof(BCCExtendWire, m_bIsPlayerOverheating)) != 0;
	sq_pushbool(v, bOverheating);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_SetPlayerOverheatState(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!PlayerOverheat_ResolveEntity(v, &pEnt))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SQBool bState = SQFalse;
	if (SQ_FAILED(sq_getbool(v, 2, &bState)))
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	const int nNormalized = (bState != SQFalse) ? 1 : 0;
	if (BCCExtend_GetI32(pEnt, offsetof(BCCExtendWire, m_bIsPlayerOverheating)) == nNormalized)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	BCCExtend_SetI32(pEnt, offsetof(BCCExtendWire, m_bIsPlayerOverheating), nNormalized);
	MarkEntityEdictDirty(pEnt);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_GetPlayerOverheatValue(HSQUIRRELVM v)
{
	void* pEnt = nullptr;
	if (!PlayerOverheat_ResolveEntity(v, &pEnt))
	{
		sq_pushfloat(v, 0.0f);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	sq_pushfloat(v, BCCExtend_GetF32(pEnt, offsetof(BCCExtendWire, m_playerOverheatValue)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Script natives -- weapon
//-----------------------------------------------------------------------------
static SQRESULT Script_DoesWeaponPlayerOverheat(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
	{
		sq_pushbool(v, false);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	char className[65];
	V_strncpy(className,
		reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(pWeapon) + SERVER_WEAPON_NAME_OFFSET),
		sizeof(className));
	className[sizeof(className) - 1] = '\0';
	const bool doesOverheat = GetDoesWeaponPlayerOverheat(className);

	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		Msg(eDLL_T::SERVER,
			"[PlayerOverheat] DoesWeaponPlayerOverheat first call: class='%s' value=%d\n",
			(className && className[0]) ? className : "(empty)",
			doesOverheat ? 1 : 0);
	}

	sq_pushbool(v, doesOverheat);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// CodeCallback_OnPlayerOverheat(player, weapon)
//-----------------------------------------------------------------------------
static void PlayerOverheat_RaiseEvent(void* pPlayer, void* pWeapon)
{
	if (!g_pServerScript || !pPlayer || !pWeapon)
		return;

	const HSCRIPT hFunc = g_pServerScript->FindFunction(
		"CodeCallback_OnPlayerOverheat", nullptr, nullptr);
	if (!hFunc)
	{
		static bool s_bCallbackMissingLatched = false;
		if (!s_bCallbackMissingLatched)
		{
			s_bCallbackMissingLatched = true;
			Warning(eDLL_T::SERVER,
				"[PlayerOverheat] CodeCallback_OnPlayerOverheat not found -- "
				"overheat event idle until scripts register it\n");
		}
		return;
	}

	CBaseEntity* const pPlayerEnt = reinterpret_cast<CBaseEntity*>(pPlayer);
	CBaseEntity* const pWeaponEnt = reinterpret_cast<CBaseEntity*>(pWeapon);

	const HSCRIPT hPlayer = pPlayerEnt->GetScriptInstance();
	const HSCRIPT hWeapon = pWeaponEnt->GetScriptInstance();
	if (!hPlayer || !hWeapon)
		return;

	ScriptVariant_t args[2];
	args[0] = hPlayer;
	args[1] = hWeapon;
	g_pServerScript->ExecuteFunction(hFunc, args, 2, nullptr, nullptr);
}

//-----------------------------------------------------------------------------
// Per-shot meter producer. Only runs while m_bIsPlayerOverheating is set.
// No decay -- meter resets on trip and when script clears the state.
//-----------------------------------------------------------------------------
void PlayerOverheat_OnWeaponFired(void* pWeapon)
{
	if (!pWeapon)
		return;

	if (!bridge_player_overheat.GetBool())
		return;

	void* const pPlayer = PlayerOverheat_GetWeaponOwner(pWeapon);
	if (!pPlayer)
		return;

	if (BCCExtend_GetI32(pPlayer, offsetof(BCCExtendWire, m_bIsPlayerOverheating)) == 0)
		return;

	char className[65];
	V_strncpy(className,
		reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(pWeapon) + SERVER_WEAPON_NAME_OFFSET),
		sizeof(className));
	className[sizeof(className) - 1] = '\0';
	const PlayerOverheatWeaponConfig& config = GetPlayerOverheatWeaponConfig(className);
	if (!config.doesPlayerOverheat)
		return;
	if (!config.hasPerBullet)
		return;

	float flNext = BCCExtend_GetF32(pPlayer, offsetof(BCCExtendWire, m_playerOverheatValue))
		+ config.perBullet;
	if (flNext < 0.0f)
		flNext = 0.0f;
	else if (flNext > PLAYER_OVERHEAT_MAX)
		flNext = PLAYER_OVERHEAT_MAX;

	BCCExtend_SetF32(pPlayer, offsetof(BCCExtendWire, m_playerOverheatValue), flNext);
	if (gpGlobals)
		BCCExtend_SetF32(pPlayer, offsetof(BCCExtendWire, m_timeLastGeneratedPlayerOverheat), gpGlobals->curTime);
	MarkEntityEdictDirty(pPlayer);

	if (bridge_player_overheat_diag.GetBool())
	{
		static int s_nDiagCount = 0;
		if ((s_nDiagCount++ % PLAYER_OVERHEAT_DIAG_EVERY) == 0)
		{
			Msg(eDLL_T::SERVER,
				"[PlayerOverheat] accumulate class='%s' per=%.1f value=%.1f\n",
				(className && className[0]) ? className : "(empty)",
				config.perBullet, flNext);
		}
	}

	if (flNext < PLAYER_OVERHEAT_MAX)
		return;

	BCCExtend_SetF32(pPlayer, offsetof(BCCExtendWire, m_playerOverheatValue), 0.0f);
	BCCExtend_SetI32(pPlayer, offsetof(BCCExtendWire, m_bIsPlayerOverheating), 0);
	MarkEntityEdictDirty(pPlayer);

	if (bridge_player_overheat_diag.GetBool())
	{
		Msg(eDLL_T::SERVER,
			"[PlayerOverheat] trip class='%s' -- CodeCallback_OnPlayerOverheat\n",
			(className && className[0]) ? className : "(empty)");
	}

	PlayerOverheat_RaiseEvent(pPlayer, pWeapon);
}

void PlayerOverheat_LevelShutdown(void)
{
	// Weapon config map is class-name keyed (process-lifetime), not entity
	// pointers. Nothing entity-scoped to clear.
}

//-----------------------------------------------------------------------------
// Registration
//-----------------------------------------------------------------------------
void PlayerOverheat_RegisterCombatCharacterFuncs(ScriptClassDescriptor_t* combatCharStruct)
{
	if (!combatCharStruct)
	{
		Warning(eDLL_T::SERVER,
			"[PlayerOverheat] combat character script class descriptor is null; "
			"overheat natives not registered\n");
		return;
	}

	static bool s_bAnnounced = false;
	if (!s_bAnnounced)
	{
		s_bAnnounced = true;
		Msg(eDLL_T::SERVER,
			"[PlayerOverheat] register combat-character funcs (sidecar)\n");
	}

	combatCharStruct->AddFunction(
		"IsPlayerOverheating",
		"Script_IsPlayerOverheating",
		"Returns whether this combat character is currently overheating",
		"bool",
		"",
		false,
		Script_IsPlayerOverheating);

	combatCharStruct->AddFunction(
		"SetPlayerOverheatState",
		"Script_SetPlayerOverheatState",
		"Sets whether this combat character is currently overheating",
		"void",
		"bool state",
		false,
		Script_SetPlayerOverheatState);

	combatCharStruct->AddFunction(
		"GetPlayerOverheatValue",
		"Script_GetPlayerOverheatValue",
		"Returns the current player overheat value",
		"float",
		"",
		false,
		Script_GetPlayerOverheatValue);
}

void PlayerOverheat_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	if (!weaponStruct)
		return;

	weaponStruct->AddFunction(
		"DoesWeaponPlayerOverheat",
		"Script_DoesWeaponPlayerOverheat",
		"Returns whether this weapon can put the player into overheat",
		"bool",
		"",
		false,
		Script_DoesWeaponPlayerOverheat);
}
