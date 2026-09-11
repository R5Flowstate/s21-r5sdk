//=============================================================================//
//
// Purpose: dedi half of the melee attack-lifetime trace.
//
// A melee attack lives exactly as long as the weapon's custom activity: the
// activity end time is the viewmodel sequence duration stamped at start, and
// OnCustomActivityFinished is what clears attackActive. The lunge exit is
// gated on attackActive (CheckDoMeleeLunge only arms m_lungeEndTime while the
// attack is active), so whoever ends the activity later leaves the lunge and
// re-grounds later. This logs every start, finish, attack end and lunge clear
// with the numbers that decide them so the client twin can be diffed line by
// line.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "gameinterface.h"   // gpGlobals
#include "melee_activity_trace.h"

static ConVar bridge_melee_trace("bridge_melee_trace", "0", FCVAR_DEVELOPMENTONLY,
	"[MELEE-ACT] log custom-activity start/finish, PlayerMelee_EndAttack and "
	"Lunge_ClearTarget with sequence, duration and clock values. 0 = off (default).");

// CWeaponX (server half).
static constexpr ptrdiff_t kWeaponEdictIndex     = 88;
static constexpr ptrdiff_t kWeaponNextReadyTime  = 4600;
static constexpr ptrdiff_t kWeaponIdealSequence  = 4628;
static constexpr ptrdiff_t kWeaponCustomActivity = 4676;
static constexpr ptrdiff_t kWeaponCustomSequence = 4680;
static constexpr ptrdiff_t kWeaponCustomEndTime  = 4688;
static constexpr ptrdiff_t kWeaponCustomFlags    = 4692;

// CPlayer.
static constexpr ptrdiff_t kPlayerEdictIndex      = 88;
static constexpr ptrdiff_t kPlayerFlags           = 564;
static constexpr ptrdiff_t kPlayerMoveType        = 0x3B2;
static constexpr ptrdiff_t kPlayerAbsOrigin       = 1104;
static constexpr ptrdiff_t kMeleeAttackActive     = 28364;
static constexpr ptrdiff_t kMeleeAttackStart      = 28368;
static constexpr ptrdiff_t kMeleeScriptedState    = 28384;
static constexpr ptrdiff_t kPlayerLungeStartTime  = 28464;
static constexpr ptrdiff_t kPlayerLungeEndTime    = 28468;
static constexpr ptrdiff_t kPlayerLungeSmoothTime = 28480;

// CGlobalVars: curtime and the clock the weapon code compares activity end
// times against.
static constexpr ptrdiff_t kGlobalsCurTime       = 0x10;
static constexpr ptrdiff_t kGlobalsWeaponTime    = 0x28;

static bool s_bFirstFire = true;

static float MeleeTrace_Global(const ptrdiff_t off)
{
	if (!gpGlobals)
		return 0.f;
	return *reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(gpGlobals) + off);
}

static uintptr_t MeleeTrace_Rva(const void* const pRet)
{
	return reinterpret_cast<uintptr_t>(pRet) - g_GameDll.GetModuleBase();
}

static void MeleeTrace_FirstFire(void)
{
	if (!s_bFirstFire)
		return;
	s_bFirstFire = false;
	Warning(eDLL_T::SERVER, "[MELEE-ACT] FIRST FIRE -- dedi melee activity trace live\n");
}

static void MeleeTrace_PlayerLine(const char* const pszEvent, const uintptr_t pPlayer, const void* const pRet)
{
	const float* const pOrg = reinterpret_cast<const float*>(pPlayer + kPlayerAbsOrigin);
	Warning(eDLL_T::SERVER,
		"[MELEE-ACT] dedi %s ent=%d ct=%.4f wt=%.4f active=%d start=%.4f state=%d "
		"lungeStart=%.4f lungeEnd=%.4f smooth=%.4f mt=%d flags=%d org=(%.2f %.2f %.2f) ret=%llX\n",
		pszEvent,
		*reinterpret_cast<const int16_t*>(pPlayer + kPlayerEdictIndex),
		MeleeTrace_Global(kGlobalsCurTime), MeleeTrace_Global(kGlobalsWeaponTime),
		*reinterpret_cast<const uint8_t*>(pPlayer + kMeleeAttackActive),
		*reinterpret_cast<const float*>(pPlayer + kMeleeAttackStart),
		*reinterpret_cast<const int*>(pPlayer + kMeleeScriptedState),
		*reinterpret_cast<const float*>(pPlayer + kPlayerLungeStartTime),
		*reinterpret_cast<const float*>(pPlayer + kPlayerLungeEndTime),
		*reinterpret_cast<const float*>(pPlayer + kPlayerLungeSmoothTime),
		*reinterpret_cast<const uint8_t*>(pPlayer + kPlayerMoveType),
		*reinterpret_cast<const int*>(pPlayer + kPlayerFlags),
		pOrg[0], pOrg[1], pOrg[2],
		static_cast<unsigned long long>(MeleeTrace_Rva(pRet)));
}

void MeleeActivityTrace_OnStart(void* weapon, unsigned int activity, unsigned char flags, char result, const void* pRet)
{
	if (!bridge_melee_trace.GetBool() || !weapon)
		return;

	MeleeTrace_FirstFire();

	const uintptr_t pWeapon = reinterpret_cast<uintptr_t>(weapon);
	const float flEnd = *reinterpret_cast<const float*>(pWeapon + kWeaponCustomEndTime);
	const float flWt  = MeleeTrace_Global(kGlobalsWeaponTime);

	Warning(eDLL_T::SERVER,
		"[MELEE-ACT] dedi START wpn=%d ok=%d act=%u flags=%02X seq=%d ideal=%d "
		"end=%.4f dur=%.4f ct=%.4f wt=%.4f ready=%.4f ret=%llX\n",
		*reinterpret_cast<const int16_t*>(pWeapon + kWeaponEdictIndex), result, activity,
		static_cast<unsigned>(flags),
		*reinterpret_cast<const int*>(pWeapon + kWeaponCustomSequence),
		*reinterpret_cast<const int*>(pWeapon + kWeaponIdealSequence),
		flEnd, flEnd - flWt, MeleeTrace_Global(kGlobalsCurTime), flWt,
		*reinterpret_cast<const float*>(pWeapon + kWeaponNextReadyTime),
		static_cast<unsigned long long>(MeleeTrace_Rva(pRet)));
}

static int64_t Hook_OnCustomActivityFinished(void* weapon)
{
	if (bridge_melee_trace.GetBool() && weapon)
	{
		MeleeTrace_FirstFire();

		const uintptr_t pWeapon = reinterpret_cast<uintptr_t>(weapon);
		Warning(eDLL_T::SERVER,
			"[MELEE-ACT] dedi FINISH wpn=%d act=%d seq=%d ideal=%d end=%.4f ct=%.4f wt=%.4f flags=%02X ret=%llX\n",
			*reinterpret_cast<const int16_t*>(pWeapon + kWeaponEdictIndex),
			*reinterpret_cast<const int*>(pWeapon + kWeaponCustomActivity),
			*reinterpret_cast<const int*>(pWeapon + kWeaponCustomSequence),
			*reinterpret_cast<const int*>(pWeapon + kWeaponIdealSequence),
			*reinterpret_cast<const float*>(pWeapon + kWeaponCustomEndTime),
			MeleeTrace_Global(kGlobalsCurTime), MeleeTrace_Global(kGlobalsWeaponTime),
			static_cast<unsigned>(*reinterpret_cast<const uint8_t*>(pWeapon + kWeaponCustomFlags)),
			static_cast<unsigned long long>(MeleeTrace_Rva(_ReturnAddress())));
	}
	return CWeaponX__OnCustomActivityFinished(weapon);
}

static void Hook_PlayerMelee_EndAttack(void* player)
{
	if (bridge_melee_trace.GetBool() && player)
	{
		MeleeTrace_FirstFire();
		MeleeTrace_PlayerLine("END-ATTACK", reinterpret_cast<uintptr_t>(player), _ReturnAddress());
	}
	CPlayer__PlayerMelee_EndAttack(player);
}

static void Hook_Lunge_ClearTarget(void* player)
{
	if (bridge_melee_trace.GetBool() && player)
	{
		MeleeTrace_FirstFire();
		MeleeTrace_PlayerLine("LUNGE-CLEAR", reinterpret_cast<uintptr_t>(player), _ReturnAddress());
	}
	CPlayer__Lunge_ClearTarget(player);
}

void VMeleeActivityTraceServer::GetFun(void) const
{
	// Server half only: the client twin in this binary carries the C_WeaponX
	// layout and does not match these displacements.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 20 55 48 83 EC 40 48 8B D9 48 89 7C 24 58 8B 89 4C 12 00 00 33 ED")
		.GetPtr(CWeaponX__OnCustomActivityFinished);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 80 B9 CC 6E 00 00 00 48 8B D9 74 1A 48 81 C1 C0 6E 00 00")
		.GetPtr(CPlayer__PlayerMelee_EndAttack);

	Module_FindPattern(g_GameDll,
		"8B 91 F8 6E 00 00 4C 8B C1 41 B9 00 02 00 00 83 FA FF 74 4D")
		.GetPtr(CPlayer__Lunge_ClearTarget);

	if (!CWeaponX__OnCustomActivityFinished || !CPlayer__PlayerMelee_EndAttack || !CPlayer__Lunge_ClearTarget)
	{
		Warning(eDLL_T::SERVER, "[MELEE-ACT] pattern unresolved (finish=%p end=%p clear=%p); trace disabled\n",
			CWeaponX__OnCustomActivityFinished, CPlayer__PlayerMelee_EndAttack, CPlayer__Lunge_ClearTarget);
	}
}

void VMeleeActivityTraceServer::Detour(const bool bAttach) const
{
	if (CWeaponX__OnCustomActivityFinished)
		DetourSetup(&CWeaponX__OnCustomActivityFinished, &Hook_OnCustomActivityFinished, bAttach);
	if (CPlayer__PlayerMelee_EndAttack)
		DetourSetup(&CPlayer__PlayerMelee_EndAttack, &Hook_PlayerMelee_EndAttack, bAttach);
	if (CPlayer__Lunge_ClearTarget)
		DetourSetup(&CPlayer__Lunge_ClearTarget, &Hook_Lunge_ClearTarget, bAttach);
}
