//=============================================================================//
//
// Purpose: glide_flight.h implementation.
//
// The S21 glide functions read their tuning from the player settings block.
// For the length of each call this file points the thrust, boost, max-speed
// and taper slots at the values the server flight model uses, and adds the
// fall boost and the horizontal speed decay around the call, in the server's
// order. The settings block is shared by every player on the setfile; each
// write is restored before the call returns.
//
//=============================================================================//
#include "core/stdafx.h"

#include "tier1/cvar.h"
#include "game/client/glide_flight.h"
#include "game/client/pred_authority.h"
#include "game/shared/glide_tuning.h"
#include "game/shared/titan_gate.h"

#include <cfloat>

// C_Player layout.
static constexpr ptrdiff_t GF_PLAYER_OFF_SETTINGS    = 9624;  // settings block pointer
static constexpr ptrdiff_t GF_PLAYER_OFF_FORWARDMOVE = 11672;
static constexpr ptrdiff_t GF_PLAYER_OFF_SIDEMOVE    = 11676;
static constexpr ptrdiff_t GF_PLAYER_OFF_GLIDING     = 12251;
static constexpr ptrdiff_t GF_PLAYER_OFF_GLIDEMETER  = 12252;
static constexpr ptrdiff_t GF_PLAYER_OFF_BOOSTEND    = 12264; // m_glideUpwardsBoostEndTime

static void (*v_Glide_Check)(uintptr_t pPlayer, void* pForward, float* vel) = nullptr;
// The stock apply uses its fifth argument slot as scratch; declaring it keeps our locals out of that slot.
static void (*v_Glide_Apply)(uintptr_t pPlayer, float* vel, const float* fwd, const float* right, uintptr_t a5) = nullptr;

enum GlideStf_e
{
	GF_STF_THRUST,
	GF_STF_UPBOOST_ENABLED,
	GF_STF_MAX_SPEED,
	GF_STF_FWD_TAPER_START,
	GF_STF_FWD_TAPER_FINISH,
	GF_STF_SIDE_TAPER_START,
	GF_STF_SIDE_TAPER_FINISH,
	GF_STF_DURATION,
	GF_STF_COUNT
};

// Globals holding each field's settings-block offset, read where the S21 code loads them.
static const uint32_t* s_pStf[GF_STF_COUNT];

static GlideBoost_t s_boost;
static uintptr_t s_boostPlayer = 0;
static ConVar* s_pGravityVar = nullptr;

static float GF_Gravity(void)
{
	if (!s_pGravityVar && g_pCVar)
		s_pGravityVar = g_pCVar->FindVar("sv_gravity");
	const float fl = s_pGravityVar ? s_pGravityVar->GetFloat() : 750.0f;
	return std::isfinite(fl) ? fl : 750.0f;
}

static bool GF_Ready(void)
{
	for (int i = 0; i < GF_STF_COUNT; ++i)
	{
		if (!s_pStf[i])
			return false;
	}
	return true;
}

template <typename T>
static inline T& GF_Field(uint8_t* pSettings, const GlideStf_e f)
{
	return *reinterpret_cast<T*>(pSettings + *s_pStf[f]);
}

template <typename T>
static inline T& GF_Player(const uintptr_t pPlayer, const ptrdiff_t off)
{
	return *reinterpret_cast<T*>(pPlayer + off);
}

// Settings blocks live in pak memory and a block can sit on a read-only page, so the
// glide slots are made writable once per block.
static uint8_t* s_pWritableSettings = nullptr;
static bool s_bWritableOk = false;

static bool GF_EnsureWritable(uint8_t* const pSettings)
{
	if (pSettings == s_pWritableSettings)
		return s_bWritableOk;

	uint32_t nLo = UINT32_MAX, nHi = 0;
	for (int i = 0; i < GF_STF_COUNT; ++i)
	{
		const uint32_t off = *s_pStf[i];
		nLo = off < nLo ? off : nLo;
		nHi = off > nHi ? off : nHi;
	}

	s_pWritableSettings = pSettings;
	s_bWritableOk = false;

	// Settings layouts are small; anything larger is not a field offset.
	if (nHi >= 0x10000)
	{
		Warning(eDLL_T::CLIENT, "[GLIDE-FLIGHT] settings %p: field offset 0x%X out of range; using the stock glide\n",
			pSettings, nHi);
		return false;
	}

	DWORD oldProtect = 0;
	if (!VirtualProtect(pSettings + nLo, (nHi - nLo) + sizeof(float), PAGE_READWRITE, &oldProtect))
	{
		Warning(eDLL_T::CLIENT, "[GLIDE-FLIGHT] settings %p: VirtualProtect failed (%lu); using the stock glide\n",
			pSettings, GetLastError());
		return false;
	}

	if (oldProtect != PAGE_READWRITE)
		DevMsg(eDLL_T::CLIENT, "[GLIDE-FLIGHT] settings %p: glide slots [0x%X..0x%X] made writable (was 0x%lX)\n",
			pSettings, nLo, nHi, oldProtect);

	s_bWritableOk = true;
	return true;
}

static void Hook_Glide_Check(uintptr_t pPlayer, void* pForward, float* vel)
{
	uint8_t* const pSettings = pPlayer ? GF_Player<uint8_t*>(pPlayer, GF_PLAYER_OFF_SETTINGS) : nullptr;
	if (!pSettings || !vel || TitanGate_IsTitanPlayer(reinterpret_cast<const void*>(pPlayer)) || !GF_Ready()
		|| !GF_EnsureWritable(pSettings))
		return v_Glide_Check(pPlayer, pForward, vel);

	const bool bWasGliding = GF_Player<uint8_t>(pPlayer, GF_PLAYER_OFF_GLIDING) != 0;
	uint8_t& nBoostEnabled = GF_Field<uint8_t>(pSettings, GF_STF_UPBOOST_ENABLED);
	const uint8_t nSaved = nBoostEnabled;
	nBoostEnabled = 0;
	v_Glide_Check(pPlayer, pForward, vel);
	nBoostEnabled = nSaved;

	if (bWasGliding || !GF_Player<uint8_t>(pPlayer, GF_PLAYER_OFF_GLIDING) || !nSaved)
		return;

	s_boost = Glide_ArmBoost(vel[2], GF_Gravity(), PredNative_CurTime());
	s_boostPlayer = pPlayer;
	GF_Player<float>(pPlayer, GF_PLAYER_OFF_BOOSTEND) = s_boost.flEndTime;
}

static void Hook_Glide_Apply(uintptr_t pPlayer, float* vel, const float* fwd, const float* right, uintptr_t a5)
{
	uint8_t* const pSettings = pPlayer ? GF_Player<uint8_t*>(pPlayer, GF_PLAYER_OFF_SETTINGS) : nullptr;
	if (!pSettings || !vel || !fwd || !right || TitanGate_IsTitanPlayer(reinterpret_cast<const void*>(pPlayer)) || !GF_Ready()
		|| !GF_Player<uint8_t>(pPlayer, GF_PLAYER_OFF_GLIDING)
		|| !GF_EnsureWritable(pSettings))
		return v_Glide_Apply(pPlayer, vel, fwd, right, a5);

	const float now = PredNative_CurTime();
	const float dt = PredNative_FrameTime();
	const float flGravity = GF_Gravity();

	float& flThrust = GF_Field<float>(pSettings, GF_STF_THRUST);
	uint8_t& nBoostEnabled = GF_Field<uint8_t>(pSettings, GF_STF_UPBOOST_ENABLED);
	float& flMaxSpeed = GF_Field<float>(pSettings, GF_STF_MAX_SPEED);
	float& flFwdFinish = GF_Field<float>(pSettings, GF_STF_FWD_TAPER_FINISH);
	float& flSideFinish = GF_Field<float>(pSettings, GF_STF_SIDE_TAPER_FINISH);
	const float flSavedThrust = flThrust;
	const uint8_t nSavedBoost = nBoostEnabled;
	const float flSavedMax = flMaxSpeed;
	const float flSavedFwdFinish = flFwdFinish;
	const float flSavedSideFinish = flSideFinish;

	// A glide first seen in a snapshot was never armed here; the networked end time alone is exact below the duration cap.
	float flBoostDelta = 0.0f;
	if (nSavedBoost)
	{
		const float flEnd = GF_Player<float>(pPlayer, GF_PLAYER_OFF_BOOSTEND);
		const float flRatio = (s_boostPlayer == pPlayer && s_boost.flEndTime == flEnd) ? s_boost.flFallRatio : 0.0f;
		flBoostDelta = Glide_BoostThrust(flEnd, flRatio, now) * flGravity * dt;
		vel[2] += flBoostDelta;
	}

	// A taper only limits input that pushes along the current velocity; finish == start disables it.
	const float flFwdVel = vel[0] * fwd[0] + vel[1] * fwd[1];
	const float flSideVel = vel[0] * right[0] + vel[1] * right[1];
	if (!(flFwdVel * GF_Player<float>(pPlayer, GF_PLAYER_OFF_FORWARDMOVE) > 0.0f))
		flFwdFinish = GF_Field<float>(pSettings, GF_STF_FWD_TAPER_START);
	if (!(flSideVel * GF_Player<float>(pPlayer, GF_PLAYER_OFF_SIDEMOVE) > 0.0f))
		flSideFinish = GF_Field<float>(pSettings, GF_STF_SIDE_TAPER_START);

	flThrust = Glide_DecayedThrust(flSavedThrust, GF_Player<float>(pPlayer, GF_PLAYER_OFF_GLIDEMETER),
		GF_Field<float>(pSettings, GF_STF_DURATION));
	nBoostEnabled = 0;
	flMaxSpeed = FLT_MAX;

	v_Glide_Apply(pPlayer, vel, fwd, right, a5);

	flThrust = flSavedThrust;
	nBoostEnabled = nSavedBoost;
	flMaxSpeed = flSavedMax;
	flFwdFinish = flSavedFwdFinish;
	flSideFinish = flSavedSideFinish;

	// A cancelled glide leaves the velocity to the caller untouched.
	if (GF_Player<uint8_t>(pPlayer, GF_PLAYER_OFF_GLIDING))
		Glide_DecayHorizontal(vel, flSavedMax, dt);
	else
		vel[2] -= flBoostDelta;
}

void VGlideFlightClient::GetAdr(void) const
{
	LogFunAdr("Glide_Check", v_Glide_Check);
	LogFunAdr("Glide_Apply", v_Glide_Apply);
}

// Each site is 'mov eax/edx, [rip+rel32]' loading a settings field offset.
static const uint32_t* GF_ResolveStf(const CMemory& fn, const ptrdiff_t nSite)
{
	if (fn.GetPtr() == 0)
		return nullptr;
	const uint8_t* const p = reinterpret_cast<const uint8_t*>(fn.GetPtr() + nSite);
	if (p[0] != 0x8B || (p[1] != 0x05 && p[1] != 0x15))
		return nullptr;
	return fn.Offset(nSite).ResolveRelativeAddress(2, 6).RCast<const uint32_t*>();
}

void VGlideFlightClient::GetFun(void) const
{
	const CMemory check = Module_FindPattern(g_GameDll,
		"40 53 57 41 56 48 83 EC 50 48 8B 99 98 25 00 00 4D 8B F0 8B 05 ?? ?? ?? ?? 48 8B F9 80 3C 18 00 74 23 80 B9 D0 2F 00 00");
	const CMemory apply = Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 18 48 89 68 20 56 57 41 56 48 81 EC 00 01 00 00 80 B9 DB 2F 00 00 00 49 8B E9 48 8B 99 98 25 00 00");
	// Glide meter update; its glideDuration load sits at +0x1DA.
	const CMemory meter = Module_FindPattern(g_GameDll,
		"48 89 5C 24 10 57 48 83 EC 30 48 8B 99 98 25 00 00 48 8B F9 8B 05 ?? ?? ?? ?? 80 3C 18 00 75 1C 8B 05 ?? ?? ?? ?? 80 3C 18 00 75 10 8B 05");

	s_pStf[GF_STF_THRUST]            = GF_ResolveStf(apply, 0x1FC);
	s_pStf[GF_STF_UPBOOST_ENABLED]   = GF_ResolveStf(apply, 0x141);
	s_pStf[GF_STF_MAX_SPEED]         = GF_ResolveStf(apply, 0x5F2);
	s_pStf[GF_STF_FWD_TAPER_START]   = GF_ResolveStf(apply, 0x43D);
	s_pStf[GF_STF_FWD_TAPER_FINISH]  = GF_ResolveStf(apply, 0x45D);
	s_pStf[GF_STF_SIDE_TAPER_START]  = GF_ResolveStf(apply, 0x4FA);
	s_pStf[GF_STF_SIDE_TAPER_FINISH] = GF_ResolveStf(apply, 0x506);
	s_pStf[GF_STF_DURATION]          = GF_ResolveStf(meter, 0x1DA);

	// Hook only with every offset in hand; a partial set would mix the two flight models.
	if (!GF_Ready())
	{
		Warning(eDLL_T::CLIENT, "[GLIDE] flight model sites unresolved -- client glide keeps the stock S21 math and mispredicts\n");
		return;
	}
	check.GetPtr(v_Glide_Check);
	apply.GetPtr(v_Glide_Apply);
}

void VGlideFlightClient::GetVar(void) const { }

void VGlideFlightClient::Detour(const bool bAttach) const
{
	if (v_Glide_Check)
		DetourSetup(&v_Glide_Check, &Hook_Glide_Check, bAttach);
	if (v_Glide_Apply)
		DetourSetup(&v_Glide_Apply, &Hook_Glide_Apply, bAttach);
}
