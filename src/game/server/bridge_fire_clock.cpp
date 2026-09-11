//=============================================================================//
//
// Purpose: one fire clock -- stamp weapon-sim times from command_time.
// See bridge_fire_clock.h.
//
//=============================================================================//
#include "core/stdafx.h"
#include "bridge_fire_clock.h"



typedef __int64(__fastcall* PFN_ItemPostFrame)(__int64 a1);
typedef __int64(__fastcall* PFN_GetViewDrift)(float* outAngles, __int64 player);

// Live ItemPostFrame-equivalent (sole weapon-sim gateway from PlayerRunCommand).
static PFN_ItemPostFrame v_ItemPostFrame = nullptr;
// Resolved only to walk to the gpGlobals slot at +0x46; never detoured.
static PFN_GetViewDrift  v_GetViewDrift  = nullptr;

static ConVar bridge_fire_clock_from_cmd("bridge_fire_clock_from_cmd", "0",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Stamp the weapon-simulation window's latestPredictedTime from the executing "
	"command's command_time (bounded by bridge_fire_clock_slack) instead of the "
	"server's own timeBase, so client and server derive every weapon time from "
	"the same clock. 0 = off (server timeBase, legacy).");

static ConVar bridge_fire_clock_slack("bridge_fire_clock_slack", "0.05",
	FCVAR_RELEASE | FCVAR_GAMEDLL,
	"Maximum offset (seconds) bridge_fire_clock_from_cmd may adopt from the "
	"client's command_time. Bounds how far a hostile client can shift its own "
	"weapon clock; it cannot compound.");

// Live current-command pointer at player+25976 (0x6578).
static constexpr ptrdiff_t PLAYER_OFF_CURRENTCOMMAND = 25976;
// CUserCmd::command_number (u32) and command_time (float).
static constexpr ptrdiff_t CMD_OFF_COMMANDNUMBER = 0x00;
static constexpr ptrdiff_t CMD_OFF_COMMAND_TIME  = 0x08;
// gpGlobals->latestPredictedTime -- written only for the weapon window.
static constexpr ptrdiff_t GLOBALS_OFF_LATEST_PRED = 0x28;

// gpGlobals slot: "mov rax, cs:gpGlobals" at GetViewDrift+0x46.
static uintptr_t** s_ppGlobals = nullptr;
// Live current-player global (preferred-base RVA), resolved once in GetVar.
static uintptr_t* s_pCurrentPlayerSlot = nullptr;

// Lifetime usable-sample counter (never resets on ConVar toggle).
static int s_nUsableSamples = 0;

// First-20 burst: log every usable sample until this many have been seen.
static constexpr int kFirstBurstCount = 20;

// After the first 20, accumulate rawDelta into a 1s window and emit one summary.
static uint64_t s_nWindowStartMs = 0;
static int      s_nWindowSamples = 0;
static int      s_nWindowClamped = 0;
static float    s_flWindowMin = 0.0f;
static float    s_flWindowMax = 0.0f;
static float    s_flWindowSum = 0.0f;

// Clock-reset log: at most once per second.
static uint64_t s_nLastResetLogMs = 0;

//-----------------------------------------------------------------------------
// Purpose: emit one sample into the first-20 stream or the 1s summary window.
// rawDelta is pre-clamp; bClamped is whether |rawDelta| exceeded slack.
//-----------------------------------------------------------------------------
static void FireClock_LogSample(const uint32_t nCmd, const float flCt,
	const float flTb, const float flRawDelta, const bool bClamped)
{
	if (s_nUsableSamples < kFirstBurstCount)
	{
		++s_nUsableSamples;
		Warning(eDLL_T::SERVER,
			"[FIRE-CLOCK] cmd=%u ct=%.5f tb=%.5f delta=%+.5f clamped=%d\n",
			nCmd, flCt, flTb, flRawDelta, bClamped ? 1 : 0);
		return;
	}

	// Past the burst: roll into a 1-second window of rawDelta stats.
	const uint64_t nNowMs = GetTickCount64();
	if (s_nWindowStartMs == 0 || nNowMs - s_nWindowStartMs >= 1000)
	{
		if (s_nWindowSamples > 0)
		{
			const float flMean = s_flWindowSum / static_cast<float>(s_nWindowSamples);
			const float flClampRatio =
				static_cast<float>(s_nWindowClamped) /
				static_cast<float>(s_nWindowSamples);
			// High clamp ratio => raw delta pins at slack = rate divergence.
			const char* pszVerdict =
				(flClampRatio >= 0.5f)
					? "pinned at slack -- looks like a rate difference, not a bounded offset"
					: "bounded";
			Warning(eDLL_T::SERVER,
				"[FIRE-CLOCK] window n=%d min=%+.5f max=%+.5f mean=%+.5f clamps=%d -- %s\n",
				s_nWindowSamples, s_flWindowMin, s_flWindowMax, flMean,
				s_nWindowClamped, pszVerdict);
		}
		s_nWindowStartMs = nNowMs;
		s_nWindowSamples = 0;
		s_nWindowClamped = 0;
		s_flWindowMin = flRawDelta;
		s_flWindowMax = flRawDelta;
		s_flWindowSum = 0.0f;
	}

	if (s_nWindowSamples == 0)
	{
		s_flWindowMin = flRawDelta;
		s_flWindowMax = flRawDelta;
	}
	else
	{
		if (flRawDelta < s_flWindowMin)
			s_flWindowMin = flRawDelta;
		if (flRawDelta > s_flWindowMax)
			s_flWindowMax = flRawDelta;
	}
	s_flWindowSum += flRawDelta;
	++s_nWindowSamples;
	if (bClamped)
		++s_nWindowClamped;
}

static __int64 __fastcall Hook_ItemPostFrame(__int64 a1)
{
	float* pLatestPred = nullptr;
	float saved = 0.0f;
	bool bOverride = false;
	float fireClock = 0.0f;

	const bool bFeatureOn = bridge_fire_clock_from_cmd.GetBool();

	if (bFeatureOn && s_ppGlobals && *s_ppGlobals)
	{
		pLatestPred = reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(*s_ppGlobals) + GLOBALS_OFF_LATEST_PRED);
		const float flTb = *pLatestPred;

		// Current player from the live global, not a1.
		const __int64 player = (s_pCurrentPlayerSlot)
			? static_cast<__int64>(*s_pCurrentPlayerSlot)
			: 0;
		const __int64 cmd = (player)
			? *reinterpret_cast<__int64*>(player + PLAYER_OFF_CURRENTCOMMAND)
			: 0;

		if (player && cmd)
		{
			const float flCt = *reinterpret_cast<float*>(cmd + CMD_OFF_COMMAND_TIME);
			const uint32_t nCmd = *reinterpret_cast<uint32_t*>(cmd + CMD_OFF_COMMANDNUMBER);
			const float flRawDelta = flCt - flTb;

			if (!isfinite(flCt) || !isfinite(flTb))
			{
				static volatile LONG s_nNonFiniteLog = 0;
				if (InterlockedIncrement(&s_nNonFiniteLog) <= 8)
					Warning(eDLL_T::SERVER, "[FIRE-CLOCK] non-finite skip cmd=%u ct=%.5f tb=%.5f\n",
						nCmd, flCt, flTb);
			}
			else if (flCt <= 0.0f)
			{
				// No client emit / no tick fallback -- leave +0x28 alone, no log.
			}
			else if (fabsf(flRawDelta) > 1.0f)
			{
				// Map change / respawn / clock reset -- log at most 1/s, no sample fold-in.
				const uint64_t nNowMs = GetTickCount64();
				if (nNowMs - s_nLastResetLogMs >= 1000)
				{
					s_nLastResetLogMs = nNowMs;
					Warning(eDLL_T::SERVER,
						"[FIRE-CLOCK] reset skip cmd=%u ct=%.5f tb=%.5f delta=%+.5f (gap > 1s)\n",
						nCmd, flCt, flTb, flRawDelta);
				}
			}
			else
			{
				const float flSlack = bridge_fire_clock_slack.GetFloat();
				float flDelta = flRawDelta;
				bool bClamped = false;
				if (flDelta > flSlack)
				{
					flDelta = flSlack;
					bClamped = true;
				}
				else if (flDelta < -flSlack)
				{
					flDelta = -flSlack;
					bClamped = true;
				}

				fireClock = flTb + flDelta;
				bOverride = true;
				saved = flTb;

				FireClock_LogSample(nCmd, flCt, flTb, flRawDelta, bClamped);
			}
		}
	}

	if (bOverride && pLatestPred)
		*pLatestPred = fireClock;

	const __int64 r = v_ItemPostFrame(a1);

	// Always restore -- weapon window must not leak a modified global.
	if (bOverride && pLatestPred)
		*pLatestPred = saved;

	return r;
}

void VBridgeFireClock::GetAdr(void) const
{
	LogFunAdr("ItemPostFrame", v_ItemPostFrame);
	LogFunAdr("GetViewDrift", v_GetViewDrift);
	LogVarAdr("FireClockGlobals", s_ppGlobals);
	LogVarAdr("CurrentPlayerSlot", s_pCurrentPlayerSlot);
}

void VBridgeFireClock::GetFun(void) const
{
	// Live ItemPostFrame: prologue + frame-pointer setup. The four wildcards
	// after 48 8D A8 are the signed stack offset; 48 81 EC pins the sub rsp.
	Module_FindPattern(g_GameDll,
		"48 8B C4 55 53 57 41 54 48 8D A8 ?? ?? ?? ?? 48 81 EC")
		.GetPtr(v_ItemPostFrame);

	// GetViewDrift (out-first arg order) -- long body for a 1-hit resolve;
	// used only to walk to the gpGlobals load at +0x46, never detoured.
	Module_FindPattern(g_GameDll,
		"40 55 56 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
		"48 8B FA 48 8B F1 83 78 ?? ?? 75 ?? 8B 05 ?? ?? ?? ?? F2 0F 10 05 ?? ?? ?? ?? "
		"F2 0F 11 01 89 41 ?? 48 8B C1 48 81 C4 ?? ?? ?? ?? 5F 5E 5D C3 "
		"48 8B 05 ?? ?? ?? ?? 48 8B CF 48 89 9C 24 ?? ?? ?? ?? 44 0F 29 84 24 ?? ?? ?? ?? "
		"F3 44 0F 10 05 ?? ?? ?? ?? 44 0F 29 8C 24 ?? ?? ?? ?? F3 44 0F 10 0D ?? ?? ?? ?? "
		"44 0F 29 94 24 ?? ?? ?? ?? F3 44 0F 10 15 ?? ?? ?? ?? 44 0F 29 B4 24 ?? ?? ?? ?? "
		"F3 44 0F 10 70 ?? F3 44 0F 5C 70 ?? F3 44 0F 11 4C 24 ?? F3 44 0F 11 54 24 ?? "
		"F3 44 0F 11 44 24 ?? E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? "
		"0F 29 B4 24 ?? ?? ?? ?? 48 8B CF 0F 29 BC 24 ?? ?? ?? ?? 44 0F 29 9C 24 ?? ?? ?? ?? "
		"44 0F 29 A4 24 ?? ?? ?? ?? 44 0F 29 AC 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 28 F0 "
		"48 8B CF F3 0F 11 75 ?? E8 ?? ?? ?? ?? 8B 8F")
		.GetPtr(v_GetViewDrift);

	if (!v_ItemPostFrame)
		Warning(eDLL_T::SERVER,
			"[FIRE-CLOCK] ItemPostFrame pattern unresolved -- feature disabled\n");
	else
		Warning(eDLL_T::SERVER,
			"[FIRE-CLOCK] ItemPostFrame @ %p\n",
			reinterpret_cast<void*>(v_ItemPostFrame));
}

void VBridgeFireClock::GetVar(void) const
{
	// gpGlobals slot off GetViewDrift+0x46: "mov rax, cs:gpGlobals" (48 8B 05 rel32).
	if (v_GetViewDrift)
	{
		const CMemory fn(reinterpret_cast<uintptr_t>(v_GetViewDrift));
		if (fn.Offset(0x46).CheckOpCodes({ 0x48, 0x8B, 0x05 }))
		{
			s_ppGlobals = fn.Offset(0x46)
				.ResolveRelativeAddress(0x3, 0x7)
				.RCast<uintptr_t**>();
			Warning(eDLL_T::SERVER, "[FIRE-CLOCK] gpGlobals slot @ %p\n",
				reinterpret_cast<void*>(s_ppGlobals));
		}
		else
		{
			Warning(eDLL_T::SERVER,
				"[FIRE-CLOCK] gpGlobals opcode check failed -- feature disabled\n");
		}
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[FIRE-CLOCK] GetViewDrift unresolved -- gpGlobals slot unavailable\n");
	}

	// Live current-player global at preferred-base RVA (moduleBase + delta).
	const uintptr_t moduleBase =
		static_cast<uintptr_t>(g_GameDll.GetModuleBase());
	if (moduleBase)
	{
		s_pCurrentPlayerSlot = reinterpret_cast<uintptr_t*>(
			moduleBase + (0x14D4EB6F0ull - 0x140000000ull));
		Warning(eDLL_T::SERVER, "[FIRE-CLOCK] current-player slot @ %p\n",
			reinterpret_cast<void*>(s_pCurrentPlayerSlot));
	}
	else
	{
		Warning(eDLL_T::SERVER,
			"[FIRE-CLOCK] module base zero -- current-player slot unavailable\n");
	}
}

void VBridgeFireClock::GetCon(void) const { }

void VBridgeFireClock::Detour(const bool bAttach) const
{
	if (v_ItemPostFrame)
		DetourSetup(&v_ItemPostFrame, &Hook_ItemPostFrame, bAttach);
}

