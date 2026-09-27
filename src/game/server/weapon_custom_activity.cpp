//=============================================================================//
//
// Purpose: S21 WCAF_* custom-activity semantics on the S3 weapon. Scripts see
// the S21 enum, the engine keeps its own bits, the wire carries the S21 value.
//
//=============================================================================//
#include "core/stdafx.h"
#include "weapon_custom_activity.h"
#include "player.h"
#include "game/server/util_server.h"
#include "game/shared/edict_dirty.h"
#include "game/shared/activity.h"
#include "game/shared/sdk_entity_state.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/weapstate_s3_to_s21.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript_server.h"
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

// S21 WeaponCustomActivityFlags.
static constexpr int kS21_Interruptible             = 0x001;
static constexpr int kS21_DisableWeapon             = 0x002;
static constexpr int kS21_AllowWhileSprinting       = 0x004;
static constexpr int kS21_AllowStartWhileSprinting  = 0x008;
static constexpr int kS21_AllowJumpLand             = 0x010;
static constexpr int kS21_Toggle                    = 0x020;
static constexpr int kS21_KeepMeleeState            = 0x040;
static constexpr int kS21_PlayRaiseOnComplete       = 0x080;
static constexpr int kS21_PlayMeleeRaiseOnComplete  = 0x100;
static constexpr int kS21_All                       = 0x1FF;

// S3 WeaponCustomActivityFlags (the engine's own bits).
static constexpr int kS3_Interruptible       = 0x01;
static constexpr int kS3_PlayRaiseOnComplete = 0x02;
static constexpr int kS3_DisableWeapon       = 0x04;
static constexpr int kS3_AllowWhileSprinting = 0x08;
static constexpr int kS3_Toggle              = 0x10;
static constexpr int kS3_KeepMeleeState      = 0x20;

// CWeaponX (server half).
static constexpr ptrdiff_t WEAPON_OFF_OWNER            = 0x11F0; // m_weaponOwner
static constexpr ptrdiff_t WEAPON_OFF_NEXTREADYTIME    = 0x11F8; // m_nextReadyTime
static constexpr ptrdiff_t WEAPON_OFF_TIMEWEAPONIDLE   = 0x1230; // m_flTimeWeaponIdle
static constexpr ptrdiff_t WEAPON_OFF_WEAPSTATE        = 0x1234; // m_weapState
static constexpr ptrdiff_t WEAPON_OFF_CUSTOMACT        = 0x1244; // m_customActivity
static constexpr ptrdiff_t WEAPON_OFF_CUSTOMACT_END    = 0x1250; // m_customActivityEndTime
static constexpr ptrdiff_t WEAPON_OFF_CUSTOMACT_FLAGS  = 0x1254; // m_customActivityFlags (char)
static constexpr ptrdiff_t VIEWMODEL_OFF_PLAYBACKRATE  = 0xFF4;  // m_flPlaybackRate
static constexpr ptrdiff_t PLAYER_OFF_BUTTONS          = 0x60DC; // m_nButtons
static constexpr int kWeapStateCustomActivity = 12;
static constexpr int kInJump = 2; // IN_JUMP

struct WcafState_t
{
	uint16_t nS21; // flags the script asked for
	uint8_t  nS3;  // byte the engine stored for them
};
static SDKEntityMap<WcafState_t> s_wcafState(ESide::Server, "wcaf.state");
static void* s_pScriptStartWeapon = nullptr;
static bool s_bS21EnumPatched = false;
static const void* s_pLandingStopRet = nullptr;

static char (*v_WeaponX_StartCustomActivityScript)(void* pWeapon, const char* pszActivity, int flags) = nullptr;
static char (*v_WeaponX_CustomActivityInterrupt)(void* pWeapon, void* pPlayer) = nullptr;
static __int64 (*v_WeaponX_StopCustomActivity)(void* pWeapon) = nullptr;
static __int64 (*v_WeaponX_WeaponFrame)(void* pWeapon, void* pPlayer) = nullptr;
static char (*v_WeaponX_Raise)(void* pWeapon, char bFast, char bMeleeRaise) = nullptr;
static void* (*v_WeaponX_GetViewmodel)(void* pWeapon) = nullptr;
static float (*v_WeaponX_AttackTimeBase)(void* pWeapon) = nullptr;
static __int64 (*v_Player_StartWeaponGesture)(void* pPlayer, void* pWeapon,
	unsigned int activity, int weight, float blendIn, char bAutoKill) = nullptr;

static ConVar bridge_wcaf_trace("bridge_wcaf_trace", "0", FCVAR_DEVELOPMENTONLY,
	"Log S21 custom-activity flag conversion and the S21-only behaviours "
	"(start while sprinting, jump/land, melee raise, detailed start).");

static int WCAF_S21ToS3(const int s21)
{
	int s3 = 0;
	if (s21 & kS21_Interruptible)        s3 |= kS3_Interruptible;
	if (s21 & kS21_PlayRaiseOnComplete)  s3 |= kS3_PlayRaiseOnComplete;
	if (s21 & kS21_DisableWeapon)        s3 |= kS3_DisableWeapon;
	if (s21 & kS21_AllowWhileSprinting)  s3 |= kS3_AllowWhileSprinting;
	if (s21 & kS21_Toggle)               s3 |= kS3_Toggle;
	if (s21 & kS21_KeepMeleeState)       s3 |= kS3_KeepMeleeState;
	return s3;
}

template <typename T>
static T& WeaponField(void* pWeapon, const ptrdiff_t off)
{
	return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(pWeapon) + off);
}

static void WeaponCustomAct_SetFloat(void* pEnt, const ptrdiff_t off, const float value)
{
	float& field = WeaponField<float>(pEnt, off);
	if (field == value)
		return;
	field = value;
	MarkEntityEdictDirty(pEnt);
}

// Script flags for this weapon, valid only while the engine byte is still the
// one those flags produced (any engine-internal start replaces it).
static const WcafState_t* WeaponCustomAct_Live(void* pWeapon, const int s3Flags)
{
	const WcafState_t* const pState = s_wcafState.Find(pWeapon);
	if (!pState || pState->nS3 != static_cast<uint8_t>(s3Flags))
		return nullptr;
	return pState;
}

static bool WeaponCustomAct_AllowsJumpLand(void* pWeapon)
{
	if (!pWeapon || WeaponField<int>(pWeapon, WEAPON_OFF_CUSTOMACT) == -1)
		return false;
	const WcafState_t* const pState = WeaponCustomAct_Live(pWeapon,
		WeaponField<uint8_t>(pWeapon, WEAPON_OFF_CUSTOMACT_FLAGS));
	return pState && (pState->nS21 & kS21_AllowJumpLand);
}

int WeaponCustomAct_WireFlags(void* pWeapon, const int s3Flags)
{
	if (pWeapon)
	{
		if (const WcafState_t* const pState = WeaponCustomAct_Live(pWeapon, s3Flags))
			return pState->nS21;
	}
	return WeapCustomActFlags_S3ToS21(s3Flags);
}

void WeaponCustomAct_OnEngineStart(void* pWeapon)
{
	if (pWeapon && pWeapon != s_pScriptStartWeapon)
		s_wcafState.Erase(pWeapon);
}

void* WeaponCustomAct_GetViewmodel(void* pWeapon)
{
	if (!pWeapon || !v_WeaponX_GetViewmodel)
		return nullptr;
	return v_WeaponX_GetViewmodel(pWeapon);
}

void WeaponCustomAct_LevelShutdown(void)
{
	s_wcafState.Clear();
	s_pScriptStartWeapon = nullptr;
}

//-----------------------------------------------------------------------------
// weapon.StartCustomActivity( activity, flags ) -- flags are S21 WCAF_*.
//-----------------------------------------------------------------------------
static char Hook_StartCustomActivityScript(void* pWeapon, const char* pszActivity, int flags)
{
	if (!v_WeaponX_StartCustomActivityScript)
		return 0;
	// Out-of-range flags reach the engine untouched so it raises its own
	// "Only accepts WCAF_ values" script error.
	if (!pWeapon || !s_bS21EnumPatched || (flags & ~kS21_All))
		return v_WeaponX_StartCustomActivityScript(pWeapon, pszActivity, flags);

	const int s3 = WCAF_S21ToS3(flags);
	int s3Start = s3;
	// S3 has one sprint bit that both admits the start and keeps the activity
	// through sprint. START_WHILE_SPRINTING only admits the start.
	if ((flags & kS21_AllowStartWhileSprinting) && !(flags & kS21_AllowWhileSprinting))
		s3Start |= kS3_AllowWhileSprinting;

	s_pScriptStartWeapon = pWeapon;
	const char result = v_WeaponX_StartCustomActivityScript(pWeapon, pszActivity, s3Start);
	s_pScriptStartWeapon = nullptr;

	if (!result)
		return result;

	uint8_t& storedFlags = WeaponField<uint8_t>(pWeapon, WEAPON_OFF_CUSTOMACT_FLAGS);
	if (storedFlags != static_cast<uint8_t>(s3))
	{
		storedFlags = static_cast<uint8_t>(s3);
		MarkEntityEdictDirty(pWeapon);
	}
	s_wcafState[pWeapon] = WcafState_t{ static_cast<uint16_t>(flags), static_cast<uint8_t>(s3) };

	if (bridge_wcaf_trace.GetBool())
		Msg(eDLL_T::SERVER, "[WCAF] start '%s' weapon=%p s21=0x%X s3=0x%X start=0x%X\n",
			pszActivity ? pszActivity : "?", pWeapon, flags, s3, s3Start);
	return result;
}

static char Hook_CustomActivityInterrupt(void* pWeapon, void* pPlayer)
{
	if (!pPlayer || !WeaponCustomAct_AllowsJumpLand(pWeapon))
		return v_WeaponX_CustomActivityInterrupt(pWeapon, pPlayer);

	// S3 always interrupts on IN_JUMP; ALLOW_JUMP_LAND takes it out of the mask.
	int& buttons = WeaponField<int>(pPlayer, PLAYER_OFF_BUTTONS);
	const int heldJump = buttons & kInJump;
	buttons &= ~kInJump;
	const char result = v_WeaponX_CustomActivityInterrupt(pWeapon, pPlayer);
	buttons |= heldJump;

	if (heldJump && bridge_wcaf_trace.GetBool())
		Msg(eDLL_T::SERVER, "[WCAF] jump kept custom activity weapon=%p\n", pWeapon);
	return result;
}

static __int64 Hook_StopCustomActivity(void* pWeapon)
{
	// The landing stop in the movement code: S21 skips it for ALLOW_JUMP_LAND.
	if (_ReturnAddress() == s_pLandingStopRet && WeaponCustomAct_AllowsJumpLand(pWeapon))
	{
		if (bridge_wcaf_trace.GetBool())
			Msg(eDLL_T::SERVER, "[WCAF] landing kept custom activity weapon=%p\n", pWeapon);
		return 0;
	}
	return v_WeaponX_StopCustomActivity(pWeapon);
}

static __int64 Hook_WeaponFrame(void* pWeapon, void* pPlayer)
{
	// S21 checks PLAYMELEERAISEONCOMPLETE before the S3 raise/idle branch.
	if (pWeapon && WeaponField<int>(pWeapon, WEAPON_OFF_WEAPSTATE) == kWeapStateCustomActivity)
	{
		const WcafState_t* const pState = WeaponCustomAct_Live(pWeapon,
			WeaponField<uint8_t>(pWeapon, WEAPON_OFF_CUSTOMACT_FLAGS));
		if (pState && (pState->nS21 & kS21_PlayMeleeRaiseOnComplete))
		{
			v_WeaponX_Raise(pWeapon, 0, 1);
			if (bridge_wcaf_trace.GetBool())
				Msg(eDLL_T::SERVER, "[WCAF] melee raise on complete weapon=%p\n", pWeapon);
		}
	}
	return v_WeaponX_WeaponFrame(pWeapon, pPlayer);
}

//-----------------------------------------------------------------------------
// weapon.StartCustomActivityDetailed( activity, flags, forceDuration, gesture )
//-----------------------------------------------------------------------------
static void WeaponCustomAct_ApplyForcedDuration(void* pWeapon, const float flForceDuration)
{
	if (!v_WeaponX_GetViewmodel || !v_WeaponX_AttackTimeBase)
		return;
	void* const pViewmodel = v_WeaponX_GetViewmodel(pWeapon);
	if (!pViewmodel)
		return;

	// The engine set end = base + sequenceDuration / rate.
	const float flRate = WeaponField<float>(pViewmodel, VIEWMODEL_OFF_PLAYBACKRATE);
	const float flBase = v_WeaponX_AttackTimeBase(pWeapon);
	const float flNatural = (WeaponField<float>(pWeapon, WEAPON_OFF_CUSTOMACT_END) - flBase) * flRate;
	if (flRate <= 0.0f || flNatural <= 0.0f)
		return;

	const float flEnd = flBase + flForceDuration;
	WeaponCustomAct_SetFloat(pViewmodel, VIEWMODEL_OFF_PLAYBACKRATE, flNatural / flForceDuration);
	WeaponCustomAct_SetFloat(pWeapon, WEAPON_OFF_CUSTOMACT_END, flEnd);
	WeaponCustomAct_SetFloat(pWeapon, WEAPON_OFF_NEXTREADYTIME, flEnd);
	WeaponCustomAct_SetFloat(pWeapon, WEAPON_OFF_TIMEWEAPONIDLE, flEnd);
}

static void WeaponCustomAct_StartGesture(void* pWeapon, const char* pszGesture)
{
	const int activity = FindActivityByName(pszGesture);
	if (activity <= 0)
	{
		Warning(eDLL_T::SERVER,
			"[WCAF] StartCustomActivityDetailed: unknown gesture activity '%s'\n", pszGesture);
		return;
	}

	if (!v_Player_StartWeaponGesture)
		return;
	const uint32_t hOwner = WeaponField<uint32_t>(pWeapon, WEAPON_OFF_OWNER);
	void* const pOwner = SDKEntityState_Resolve(SDKEntityHandle(hOwner), ESide::Server);
	if (!pOwner)
		return;
	const int16_t edictIdx = WeaponField<int16_t>(pOwner, 0x58); // edict index
	if (edictIdx < 1 || UTIL_PlayerByIndex(edictIdx) != reinterpret_cast<CPlayer*>(pOwner))
		return;

	v_Player_StartWeaponGesture(pOwner, pWeapon, static_cast<unsigned int>(activity), -1, -1.0f, 1);
}

static SQRESULT Script_StartCustomActivityDetailed(HSQUIRRELVM v)
{
	void* pWeapon = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pWeapon)) || !pWeapon)
		return SQ_ERROR;

	const SQChar* pszActivity = nullptr;
	SQInteger flags = 0;
	SQFloat flForceDuration = -1.0f;
	const SQChar* pszGesture = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &pszActivity)) || !pszActivity
		|| SQ_FAILED(sq_getinteger(v, 3, &flags))
		|| SQ_FAILED(sq_getfloat(v, 4, &flForceDuration))
		|| SQ_FAILED(sq_getstring(v, 5, &pszGesture)))
	{
		return SQ_ERROR;
	}

	const bool bStarted = Hook_StartCustomActivityScript(pWeapon, pszActivity, static_cast<int>(flags)) != 0;
	if (bStarted)
	{
		if (flForceDuration > 0.0f)
			WeaponCustomAct_ApplyForcedDuration(pWeapon, flForceDuration);
		if (pszGesture && pszGesture[0])
			WeaponCustomAct_StartGesture(pWeapon, pszGesture);

		if (bridge_wcaf_trace.GetBool())
			Msg(eDLL_T::SERVER, "[WCAF] detailed '%s' weapon=%p force=%.3f gesture='%s'\n",
				pszActivity, pWeapon, flForceDuration, pszGesture ? pszGesture : "");
	}

	sq_pushbool(v, bStarted ? SQTrue : SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void WeaponCustomAct_RegisterWeaponFuncs(ScriptClassDescriptor_t* weaponStruct)
{
	if (!weaponStruct)
		return;

	weaponStruct->AddFunction(
		"StartCustomActivityDetailed",
		"Script_StartCustomActivityDetailed",
		"Starts a custom activity with a forced duration and an optional 3p gesture activity",
		"bool",
		"string activity, int flags, float forceDuration, string gestureActivity",
		false,
		Script_StartCustomActivityDetailed);
}

void WeaponCustomAct_RegisterConstants(CSquirrelVM* s)
{
	if (!s)
		return;

	// The other six names come from the engine registrar with patched values.
	s->RegisterConstant("WCAF_INTERRUPTIBLE_ALLOW_START_WHILE_SPRINTING", kS21_AllowStartWhileSprinting);
	s->RegisterConstant("WCAF_INTERRUPTIBLE_ALLOW_JUMP_LAND", kS21_AllowJumpLand);
	s->RegisterConstant("WCAF_PLAYMELEERAISEONCOMPLETE", kS21_PlayMeleeRaiseOnComplete);
}

//-----------------------------------------------------------------------------
// The WCAF RegisterEnum block: NONE is `xor r8d, r8d`, then six
// `mov r8d, imm32; lea rdx, name; mov rcx, rbx; call RegisterEnum`.
// The same bit layout is also registered for TDF_*, so each hit is confirmed
// by the name of its first entry.
//-----------------------------------------------------------------------------
static int WeaponCustomAct_PatchEnumRegistrars(void)
{
	static const int s_s3Values[]  = { kS3_Interruptible, kS3_PlayRaiseOnComplete, kS3_DisableWeapon,
		kS3_AllowWhileSprinting, kS3_Toggle, kS3_KeepMeleeState };
	static const int s_s21Values[] = { kS21_Interruptible, kS21_PlayRaiseOnComplete, kS21_DisableWeapon,
		kS21_AllowWhileSprinting, kS21_Toggle, kS21_KeepMeleeState };
	static constexpr size_t kFirstRecordLen = 18;
	static constexpr size_t kRecordLen = 21;

	std::vector<int16_t> pattern = { 0x45, 0x33, 0xC0, 0x48, 0x8D, 0x15, -1, -1, -1, -1,
		0x48, 0x8B, 0xCB, 0xE8, -1, -1, -1, -1 };
	for (const int value : s_s3Values)
	{
		const int16_t record[] = { 0x41, 0xB8, static_cast<int16_t>(value), 0x00, 0x00, 0x00,
			0x48, 0x8D, 0x15, -1, -1, -1, -1, 0x48, 0x8B, 0xCB, 0xE8, -1, -1, -1, -1 };
		pattern.insert(pattern.end(), std::begin(record), std::end(record));
	}

	const CModule::ModuleSections_t* const pText = g_GameDll.FindSectionByName(".text");
	if (!pText || !pText->IsSectionValid() || pText->m_nSectionSize < pattern.size())
		return 0;

	const uint8_t* const pBase = reinterpret_cast<const uint8_t*>(pText->m_pSectionBase);
	const size_t nScan = pText->m_nSectionSize - pattern.size();
	int nPatched = 0;
	for (size_t i = 0; i <= nScan; ++i)
	{
		bool bMatch = true;
		for (size_t j = 0; j < pattern.size(); ++j)
		{
			if (pattern[j] >= 0 && pBase[i + j] != static_cast<uint8_t>(pattern[j]))
			{
				bMatch = false;
				break;
			}
		}
		if (!bMatch)
			continue;

		const CMemory site(reinterpret_cast<uintptr_t>(pBase + i));
		const char* const pszFirst = site.Offset(3).ResolveRelativeAddress(3, 7).RCast<const char*>();
		if (!pszFirst || strcmp(pszFirst, "WCAF_NONE") != 0)
			continue;

		for (size_t k = 0; k < std::size(s_s21Values); ++k)
		{
			const uint32_t value = static_cast<uint32_t>(s_s21Values[k]);
			site.Offset(kFirstRecordLen + k * kRecordLen + 2).Patch({
				static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
				static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24) });
		}
		++nPatched;
		i += pattern.size() - 1;
	}
	return nPatched;
}

void VWeaponCustomActivity::GetAdr(void) const
{
	LogFunAdr("CWeaponX::StartCustomActivity_Script", v_WeaponX_StartCustomActivityScript);
	LogFunAdr("CWeaponX::CustomActivityInterrupt", v_WeaponX_CustomActivityInterrupt);
	LogFunAdr("CWeaponX::StopCustomActivity", v_WeaponX_StopCustomActivity);
	LogFunAdr("CWeaponX::WeaponFrame", v_WeaponX_WeaponFrame);
	LogFunAdr("CWeaponX::Raise", v_WeaponX_Raise);
	LogFunAdr("CWeaponX::GetViewmodel", v_WeaponX_GetViewmodel);
	LogFunAdr("CWeaponX::AttackTimeBase", v_WeaponX_AttackTimeBase);
	LogFunAdr("CPlayer::StartWeaponGesture", v_Player_StartWeaponGesture);
	LogVarAdr("LandingStopCustomActivityRet", s_pLandingStopRet);
}

void VWeaponCustomActivity::GetFun(void) const
{
	// Script binding: activity name lookup, then (flags & 0x3F) == flags. Unique.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 41 8B D8 48 8D 0D ?? ?? ?? ?? 48 8B FA E8")
		.GetPtr(v_WeaponX_StartCustomActivityScript);

	// Server half: early-outs, then m_customActivity (+0x1244) == -1. Unique.
	Module_FindPattern(g_GameDll,
		"40 53 41 56 48 83 EC 28 4C 8B F2 48 8B D9 48 85 D2 75 0A 32 C0 48 83 C4 28 41 5E 5B C3 "
		"83 B9 44 12 00 00 FF")
		.GetPtr(v_WeaponX_CustomActivityInterrupt);

	// Reads m_flTimeWeaponIdle (+0x1230) against curtime first. Unique.
	Module_FindPattern(g_GameDll,
		"4C 8B 05 ?? ?? ?? ?? 48 8B D1 F3 0F 10 81 30 12 00 00 F3 41 0F 10 48 28")
		.GetPtr(v_WeaponX_StopCustomActivity);

	// Opens on weapState (+0x1234) == CUSTOM_ACTIVITY. Unique.
	Module_FindPattern(g_GameDll,
		"40 53 55 57 41 54 41 56 48 83 EC 40 83 B9 34 12 00 00 0C")
		.GetPtr(v_WeaponX_WeaponFrame);

	// RaiseInternal(weapon, fast, meleeRaise); owner handle at +0x11F0. Unique.
	Module_FindPattern(g_GameDll,
		"4C 8B DC 49 89 5B 18 55 57 41 56 48 83 EC 60 44 8B 89 F0 11 00 00 41 0F B6 E8")
		.GetPtr(v_WeaponX_Raise);

	// Viewmodel for the weapon's owner. Unique.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 91 F0 11 00 00 48 8B F9 83 FA FF 0F 84")
		.GetPtr(v_WeaponX_GetViewmodel);

	// Time base the engine uses for the custom activity end. Unique.
	Module_FindPattern(g_GameDll,
		"83 B9 14 18 00 00 00 48 8B 05 ?? ?? ?? ?? 7F 12 83 B9 50 27 00 00 00")
		.GetPtr(v_WeaponX_AttackTimeBase);

	// Player anim state at +0x7238. Unique.
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 83 EC 60 48 8B B1 38 72 00 00 48 8B FA")
		.GetPtr(v_Player_StartWeaponGesture);

	// Movement landing: active weapon in a custom activity with ISINTERRUPTIBLE
	// -> StopCustomActivity. Return address is the byte after the call. Unique.
	const CMemory landing = Module_FindPattern(g_GameDll,
		"83 B9 44 12 00 00 FF 74 ?? F6 81 54 12 00 00 01 74 ?? E8");
	if (landing)
		s_pLandingStopRet = landing.Offset(0x17).RCast<const void*>();

	if (!v_WeaponX_StartCustomActivityScript || !v_WeaponX_CustomActivityInterrupt
		|| !v_WeaponX_StopCustomActivity || !v_WeaponX_WeaponFrame || !v_WeaponX_Raise
		|| !v_WeaponX_GetViewmodel || !v_WeaponX_AttackTimeBase || !v_Player_StartWeaponGesture
		|| !s_pLandingStopRet)
	{
		Warning(eDLL_T::SERVER,
			"[WCAF] unresolved: script=%p check=%p stop=%p base=%p raise=%p vm=%p time=%p gesture=%p land=%p "
			"-- S21 custom-activity flags stay S3\n",
			v_WeaponX_StartCustomActivityScript, v_WeaponX_CustomActivityInterrupt,
			v_WeaponX_StopCustomActivity, v_WeaponX_WeaponFrame, v_WeaponX_Raise,
			v_WeaponX_GetViewmodel, v_WeaponX_AttackTimeBase, v_Player_StartWeaponGesture,
			s_pLandingStopRet);
		return;
	}

	const int nPatched = WeaponCustomAct_PatchEnumRegistrars();
	s_bS21EnumPatched = nPatched > 0;
	if (s_bS21EnumPatched)
		Msg(eDLL_T::SERVER, "[WCAF] S21 WCAF_* values patched into %d enum registrar(s)\n", nPatched);
	else
		Warning(eDLL_T::SERVER,
			"[WCAF] WCAF enum registrar not found -- scripts keep S3 WCAF_* values and flags pass through\n");
}

void VWeaponCustomActivity::Detour(const bool bAttach) const
{
	if (!s_bS21EnumPatched)
		return;

	DetourSetup(&v_WeaponX_StartCustomActivityScript, &Hook_StartCustomActivityScript, bAttach);
	DetourSetup(&v_WeaponX_CustomActivityInterrupt, &Hook_CustomActivityInterrupt, bAttach);
	DetourSetup(&v_WeaponX_StopCustomActivity, &Hook_StopCustomActivity, bAttach);
	DetourSetup(&v_WeaponX_WeaponFrame, &Hook_WeaponFrame, bAttach);
}
