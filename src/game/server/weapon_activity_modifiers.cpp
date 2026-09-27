//=============================================================================//
//
// Purpose: 1p weapon activity modifiers the engine's gatherer never adds.
//
// The weapon's ideal sequence replicates and the S21 client installs it over
// its own pick, so both sides must select with the same modifier set.
//
//=============================================================================//
#include "core/stdafx.h"


#include "tier1/cvar.h"
#include "public/const.h"
#include "public/edict.h"
#include "weapon_activity_modifiers.h"
#include "jetdrive.h"
#include "akimbo.h"
#include "game/shared/activitymodifier.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/weapon_script_vars.h"
#include <cstring>

extern CGlobalVars* gpGlobals;

typedef unsigned int (__fastcall* WeaponActivityModifiers_t)(__int64 weapon, uint16_t* pMods);
static WeaponActivityModifiers_t v_CWeaponX__GetActivityModifiers = nullptr;

// Weapon settings schema, indexed by eWeaponVar.
struct WeaponSettingDesc_t
{
	const char* name;
	uint8_t     ctor[8];
	uint8_t     kind;
	uint8_t     unk11;
	uint16_t    index;
	uint16_t    modVarOffset;
	uint8_t     unk16[10];
};
static_assert(sizeof(WeaponSettingDesc_t) == 0x20, "weapon setting descriptor stride");

static const WeaponSettingDesc_t* s_pWeaponSettingDescs = nullptr;
static constexpr int kWeaponSettingLast = 0x3D7;
static constexpr uint8_t kWeaponSettingKindString = 4;

static constexpr ptrdiff_t WEAPON_OFF_OWNER   = 0x11F0;
static constexpr ptrdiff_t WEAPON_OFF_MODVARS = 0x17E0;

// Every caller hands in a 36-entry buffer.
static constexpr unsigned int kModifierSlots = 36;
static constexpr int kMaxModifiers1p = 5;

static ConVar bridge_weap_1p_modifiers("bridge_weap_1p_modifiers", "1", FCVAR_RELEASE,
	"Feed the weapon's activitymodifier1p list and the jet_driving player modifier "
	"into the 1p sequence pick so it matches the client's. 0 = engine set only.");
static ConVar sdk_weap_1p_modifiers_diag("sdk_weap_1p_modifiers_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log the 1p activity modifiers added to a weapon's sequence pick.");

//-----------------------------------------------------------------------------
// m_modVars offset of the slot activitymodifier1p is parsed into; 0 = not
// resolved yet, -1 = unavailable.
//-----------------------------------------------------------------------------
static int s_nActivityModifier1pOffset = 0;

static int WeaponActivityModifiers_Slot1pOffset(void)
{
	if (s_nActivityModifier1pOffset)
		return s_nActivityModifier1pOffset;

	const char* const slot = WeaponScriptVars_GetReservedSlot("activitymodifier1p");
	if (!slot || !s_pWeaponSettingDescs)
	{
		s_nActivityModifier1pOffset = -1;
		Warning(eDLL_T::SERVER, "[WEAP-1P-MODS] no settings slot backs activitymodifier1p -- weapon 1p modifiers off\n");
		return -1;
	}

	// The schema is filled by the game's static initializers.
	if (!s_pWeaponSettingDescs[1].name)
		return -1;

	for (int i = 1; i <= kWeaponSettingLast; ++i)
	{
		const WeaponSettingDesc_t& desc = s_pWeaponSettingDescs[i];
		if (!desc.name || strcmp(desc.name, slot) != 0)
			continue;

		if (desc.kind != kWeaponSettingKindString)
			break;

		s_nActivityModifier1pOffset = desc.modVarOffset;
		Msg(eDLL_T::SERVER, "[WEAP-1P-MODS] activitymodifier1p -> '%s' m_modVars+0x%X\n",
			slot, static_cast<unsigned>(desc.modVarOffset));
		return s_nActivityModifier1pOffset;
	}

	s_nActivityModifier1pOffset = -1;
	Warning(eDLL_T::SERVER, "[WEAP-1P-MODS] settings slot '%s' missing or not a string -- weapon 1p modifiers off\n", slot);
	return -1;
}

static unsigned int WeaponActivityModifiers_AppendWeapon1p(void* pWeapon, uint16_t* pMods, unsigned int count)
{
	const int off = WeaponActivityModifiers_Slot1pOffset();
	if (off <= 0)
		return count;

	const char* const list = *reinterpret_cast<const char* const*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_MODVARS + off);
	if (!list || !list[0])
		return count;

	// Comma-separated, at most five names.
	char name[64];
	int nAdded = 0;
	const char* p = list;
	while (*p && nAdded < kMaxModifiers1p && count < kModifierSlots)
	{
		while (*p == ',' || *p == ' ' || *p == '\t')
			++p;

		size_t len = 0;
		while (p[len] && p[len] != ',')
			++len;

		size_t trimmed = len;
		while (trimmed && (p[trimmed - 1] == ' ' || p[trimmed - 1] == '\t'))
			--trimmed;

		if (trimmed && trimmed < sizeof(name))
		{
			memcpy(name, p, trimmed);
			name[trimmed] = '\0';

			const CUtlSymbol sym = InternActivityModifier(name);
			if (sym.IsValid())
			{
				pMods[count++] = static_cast<uint16_t>(static_cast<unsigned int>(sym));
				++nAdded;
			}
		}
		p += len;
	}

	if (nAdded && sdk_weap_1p_modifiers_diag.GetBool())
		Msg(eDLL_T::SERVER, "[WEAP-1P-MODS] weapon=%p '%s' -> %d modifier(s)\n", pWeapon, list, nAdded);

	return count;
}

static unsigned int WeaponActivityModifiers_AppendJetDriving(void* pPlayer, uint16_t* pMods, unsigned int count)
{
	if (count >= kModifierSlots || !JetDrive_IsJetDriving(pPlayer))
		return count;

	static CUtlSymbol s_jetDriving;
	if (!s_jetDriving.IsValid())
	{
		s_jetDriving = InternActivityModifier("jet_driving");
		if (!s_jetDriving.IsValid())
			return count;
	}

	pMods[count++] = static_cast<uint16_t>(static_cast<unsigned int>(s_jetDriving));
	return count;
}

static void* WeaponActivityModifiers_PlayerOwner(void* pWeapon)
{
	const uint32_t raw = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<uintptr_t>(pWeapon) + WEAPON_OFF_OWNER);
	if (raw == 0xFFFFFFFFu || !gpGlobals)
		return nullptr;

	const int entIndex = static_cast<int>(raw & ENT_ENTRY_MASK);
	if (entIndex < 1 || entIndex > gpGlobals->maxClients)
		return nullptr;

	return SDKEntityState_Resolve(SDKEntityHandle(raw), ESide::Server);
}

static unsigned int __fastcall Hook_CWeaponX_GetActivityModifiers(__int64 weapon, uint16_t* pMods)
{
	unsigned int count = v_CWeaponX__GetActivityModifiers(weapon, pMods);
	if (!weapon || !pMods || count >= kModifierSlots)
		return count;

	void* const pWeapon = reinterpret_cast<void*>(weapon);
	void* const pPlayer = WeaponActivityModifiers_PlayerOwner(pWeapon);
	if (!pPlayer)
		return count;

	if (bridge_weap_1p_modifiers.GetBool())
	{
		count = WeaponActivityModifiers_AppendWeapon1p(pWeapon, pMods, count);
		count = WeaponActivityModifiers_AppendJetDriving(pPlayer, pMods, count);
	}

	return AkimboBridge_AppendActivityModifiers(pWeapon, pMods, count, kModifierSlots);
}

void VWeaponActivityModifiers::GetAdr(void) const
{
	LogFunAdr("CWeaponX::GetActivityModifiers", v_CWeaponX__GetActivityModifiers);
	LogVarAdr("WeaponSettingDescs", s_pWeaponSettingDescs);
}

void VWeaponActivityModifiers::GetFun(void) const
{
	// Server twin: owner handle at +0x11F0.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 8B 91 F0 11 00 00 83 FA FF 0F 84 ?? ?? ?? ?? 0F B7 C2 C1")
		.GetPtr(v_CWeaponX__GetActivityModifiers);
	if (!v_CWeaponX__GetActivityModifiers)
		Warning(eDLL_T::SERVER,
			"[WEAP-1P-MODS] CWeaponX::GetActivityModifiers pattern unresolved -- 1p sequence picks keep the engine modifier set\n");

	// Server weapon-setting reader: bounds the index to 1..0x3D7, then leas the schema.
	const CMemory settingReader = Module_FindPattern(g_GameDll,
		"48 83 EC 28 41 8D 40 FF 4C 8B D9 3D D6 03 00 00 0F 87 ?? ?? ?? ?? 0F B6 4C 24 50 48 8D 05");
	if (settingReader)
		s_pWeaponSettingDescs = settingReader.Offset(0x1B).ResolveRelativeAddress(0x3, 0x7).RCast<const WeaponSettingDesc_t*>();
	else
		Warning(eDLL_T::SERVER,
			"[WEAP-1P-MODS] weapon settings schema unresolved -- weapon 1p modifiers off\n");
}

void VWeaponActivityModifiers::Detour(const bool bAttach) const
{
	if (v_CWeaponX__GetActivityModifiers)
		DetourSetup(&v_CWeaponX__GetActivityModifiers, &Hook_CWeaponX_GetActivityModifiers, bAttach);
}
