//=============================================================================//
//
// Purpose: client half of the melee attack-lifetime trace. Twin of
// src\game\server\melee_activity_trace.cpp -- same events, same log shape,
// so the two logs pair line for line.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "melee_activity_trace.h"
#include "pred_authority.h"

static ConVar sdk_melee_trace("sdk_melee_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[MELEE-ACT] log custom-activity start/finish, PlayerMelee_EndAttack, "
	"ClearActiveAttackState and Lunge_ClearTarget with sequence, duration and "
	"clock values. 0 = off (default).");

// C_WeaponX.
static constexpr ptrdiff_t kWeaponNextReadyTime  = 5480;
static constexpr ptrdiff_t kWeaponIdealSequence  = 5508;
static constexpr ptrdiff_t kWeaponCustomActivity = 5582;
static constexpr ptrdiff_t kWeaponCustomSequence = 5584;
static constexpr ptrdiff_t kWeaponCustomEndTime  = 5592;
static constexpr ptrdiff_t kWeaponCustomFlags    = 5596;

// C_Player.
static constexpr ptrdiff_t kPlayerFlags           = 200;
static constexpr ptrdiff_t kPlayerMoveType        = 0x3B2;
static constexpr ptrdiff_t kMeleeAttackActive     = 12844;
static constexpr ptrdiff_t kMeleeAttackStart      = 12848;
static constexpr ptrdiff_t kMeleeScriptedState    = 12864;
static constexpr ptrdiff_t kPlayerLungeStartTime  = 16648;
static constexpr ptrdiff_t kPlayerLungeEndTime    = 16652;
static constexpr ptrdiff_t kPlayerLungeSmoothTime = 16664;

static bool s_bFirstFire = true;

static uintptr_t MeleeTrace_Rva(const void* const pRet)
{
	return reinterpret_cast<uintptr_t>(pRet) - g_GameDll.GetModuleBase();
}

static void MeleeTrace_FirstFire(void)
{
	if (!s_bFirstFire)
		return;
	s_bFirstFire = false;
	Warning(eDLL_T::CLIENT, "[MELEE-ACT] FIRST FIRE -- client melee activity trace live\n");
}

static void MeleeTrace_PlayerLine(const char* const pszEvent, const uintptr_t pPlayer, const void* const pRet)
{
	Warning(eDLL_T::CLIENT,
		"[MELEE-ACT] client %s ply=%llX ct=%.4f wt=%.4f active=%d start=%.4f state=%d "
		"lungeStart=%.4f lungeEnd=%.4f smooth=%.4f mt=%d flags=%d ret=%llX\n",
		pszEvent, static_cast<unsigned long long>(pPlayer & 0xFFFFF),
		PredNative_CurTime(), PredNative_LatestPredictedTime(),
		*reinterpret_cast<const uint8_t*>(pPlayer + kMeleeAttackActive),
		*reinterpret_cast<const float*>(pPlayer + kMeleeAttackStart),
		*reinterpret_cast<const int*>(pPlayer + kMeleeScriptedState),
		*reinterpret_cast<const float*>(pPlayer + kPlayerLungeStartTime),
		*reinterpret_cast<const float*>(pPlayer + kPlayerLungeEndTime),
		*reinterpret_cast<const float*>(pPlayer + kPlayerLungeSmoothTime),
		*reinterpret_cast<const uint8_t*>(pPlayer + kPlayerMoveType),
		*reinterpret_cast<const int*>(pPlayer + kPlayerFlags),
		static_cast<unsigned long long>(MeleeTrace_Rva(pRet)));
}

static char Hook_StartCustomActivity_Internal(void* weapon, uint16_t activity, uint16_t flags,
	float forceDuration, uint16_t gesture, bool usePlayerTimeBase)
{
	const char result = C_WeaponX__StartCustomActivity_Internal(weapon, activity, flags,
		forceDuration, gesture, usePlayerTimeBase);

	if (!sdk_melee_trace.GetBool() || !weapon)
		return result;

	MeleeTrace_FirstFire();

	const uintptr_t pWeapon = reinterpret_cast<uintptr_t>(weapon);
	const float flEnd = *reinterpret_cast<const float*>(pWeapon + kWeaponCustomEndTime);
	const float flWt  = PredNative_LatestPredictedTime();

	Warning(eDLL_T::CLIENT,
		"[MELEE-ACT] client START wpn=%llX ok=%d act=%u flags=%02X seq=%d ideal=%d "
		"end=%.4f dur=%.4f ct=%.4f wt=%.4f ready=%.4f force=%.3f tb=%d ret=%llX\n",
		static_cast<unsigned long long>(pWeapon & 0xFFFFF), result, activity, flags,
		*reinterpret_cast<const int16_t*>(pWeapon + kWeaponCustomSequence),
		*reinterpret_cast<const int16_t*>(pWeapon + kWeaponIdealSequence),
		flEnd, flEnd - flWt, PredNative_CurTime(), flWt,
		*reinterpret_cast<const float*>(pWeapon + kWeaponNextReadyTime),
		forceDuration, usePlayerTimeBase,
		static_cast<unsigned long long>(MeleeTrace_Rva(_ReturnAddress())));
	return result;
}

static int64_t Hook_OnCustomActivityFinished(void* weapon)
{
	if (sdk_melee_trace.GetBool() && weapon)
	{
		MeleeTrace_FirstFire();

		const uintptr_t pWeapon = reinterpret_cast<uintptr_t>(weapon);
		Warning(eDLL_T::CLIENT,
			"[MELEE-ACT] client FINISH wpn=%llX act=%d seq=%d ideal=%d end=%.4f ct=%.4f wt=%.4f flags=%02X ret=%llX\n",
			static_cast<unsigned long long>(pWeapon & 0xFFFFF),
			*reinterpret_cast<const int16_t*>(pWeapon + kWeaponCustomActivity),
			*reinterpret_cast<const int16_t*>(pWeapon + kWeaponCustomSequence),
			*reinterpret_cast<const int16_t*>(pWeapon + kWeaponIdealSequence),
			*reinterpret_cast<const float*>(pWeapon + kWeaponCustomEndTime),
			PredNative_CurTime(), PredNative_LatestPredictedTime(),
			static_cast<unsigned>(*reinterpret_cast<const uint16_t*>(pWeapon + kWeaponCustomFlags)),
			static_cast<unsigned long long>(MeleeTrace_Rva(_ReturnAddress())));
	}
	return C_WeaponX__OnCustomActivityFinished(weapon);
}

static void Hook_PlayerMelee_EndAttack(void* player)
{
	if (sdk_melee_trace.GetBool() && player)
	{
		MeleeTrace_FirstFire();
		MeleeTrace_PlayerLine("END-ATTACK", reinterpret_cast<uintptr_t>(player), _ReturnAddress());
	}
	C_Player__PlayerMelee_EndAttack(player);
}

static void Hook_PlayerMelee_ClearActiveAttackState(void* player)
{
	if (sdk_melee_trace.GetBool() && player
		&& *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(player) + kMeleeAttackActive))
	{
		MeleeTrace_FirstFire();
		MeleeTrace_PlayerLine("CLEAR-ATTACK", reinterpret_cast<uintptr_t>(player), _ReturnAddress());
	}
	C_Player__PlayerMelee_ClearActiveAttackState(player);
}

static void Hook_Lunge_ClearTarget(void* player)
{
	if (sdk_melee_trace.GetBool() && player)
	{
		MeleeTrace_FirstFire();
		MeleeTrace_PlayerLine("LUNGE-CLEAR", reinterpret_cast<uintptr_t>(player), _ReturnAddress());
	}
	C_Player__Lunge_ClearTarget(player);
}

void VMeleeActivityTrace::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"40 56 57 41 57 48 81 EC A0 00 00 00 80 3D ?? ?? ?? ?? 00 41 0F B7 F8 "
		"0F 29 B4 24 80 00 00 00 44 0F B7 FA")
		.GetPtr(C_WeaponX__StartCustomActivity_Internal);

	Module_FindPattern(g_GameDll,
		"48 8B C4 56 41 56 48 83 EC 78 48 89 58 08 48 8B F1 8B 89 D4 15 00 00 48 89 68 10 BD FF FF FF FF")
		.GetPtr(C_WeaponX__OnCustomActivityFinished);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B D9 C6 81 2C 32 00 00 00 C7 81 30 32 00 00 00 00 00 00 C6 81 44 32 00 00 00 E8")
		.GetPtr(C_Player__PlayerMelee_EndAttack);

	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 80 B9 2C 32 00 00 00 48 8B D9 74 2E C6 81 2C 32 00 00 00")
		.GetPtr(C_Player__PlayerMelee_ClearActiveAttackState);

	Module_FindPattern(g_GameDll,
		"C7 81 DC 40 00 00 FF FF FF FF 33 C0 C6 81 E0 40 00 00 00 F3 0F 10 05")
		.GetPtr(C_Player__Lunge_ClearTarget);

	if (!C_WeaponX__StartCustomActivity_Internal || !C_WeaponX__OnCustomActivityFinished
		|| !C_Player__PlayerMelee_EndAttack || !C_Player__PlayerMelee_ClearActiveAttackState
		|| !C_Player__Lunge_ClearTarget)
	{
		Warning(eDLL_T::CLIENT, "[MELEE-ACT] pattern unresolved (start=%p finish=%p end=%p clear=%p lunge=%p); trace disabled\n",
			C_WeaponX__StartCustomActivity_Internal, C_WeaponX__OnCustomActivityFinished,
			C_Player__PlayerMelee_EndAttack, C_Player__PlayerMelee_ClearActiveAttackState,
			C_Player__Lunge_ClearTarget);
	}
}

void VMeleeActivityTrace::Detour(const bool bAttach) const
{
	if (C_WeaponX__StartCustomActivity_Internal)
		DetourSetup(&C_WeaponX__StartCustomActivity_Internal, &Hook_StartCustomActivity_Internal, bAttach);
	if (C_WeaponX__OnCustomActivityFinished)
		DetourSetup(&C_WeaponX__OnCustomActivityFinished, &Hook_OnCustomActivityFinished, bAttach);
	if (C_Player__PlayerMelee_EndAttack)
		DetourSetup(&C_Player__PlayerMelee_EndAttack, &Hook_PlayerMelee_EndAttack, bAttach);
	if (C_Player__PlayerMelee_ClearActiveAttackState)
		DetourSetup(&C_Player__PlayerMelee_ClearActiveAttackState, &Hook_PlayerMelee_ClearActiveAttackState, bAttach);
	if (C_Player__Lunge_ClearTarget)
		DetourSetup(&C_Player__Lunge_ClearTarget, &Hook_Lunge_ClearTarget, bAttach);
}
