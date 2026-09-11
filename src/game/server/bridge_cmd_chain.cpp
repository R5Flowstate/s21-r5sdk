//=============================================================================//
//
// Purpose: ledger of the per-usercmd execution chain.
//
//=============================================================================//
#include "core/stdafx.h"
#include <atomic>
#include "bridge_cmd_chain.h"
#include "player_command.h"

// g_PlayerMove global (RVA CHAIN_RVA_G_PLAYERMOVE): first qword is CPlayerMove
// vtable; slot1 RunCommand, slot4 StartCommand. Plain data global -- no code
// signature nearby.
static constexpr ptrdiff_t CHAIN_RVA_G_PLAYERMOVE = 0x23A4B38;

static ConVar bridge_cmd_chain("bridge_cmd_chain", "0", FCVAR_RELEASE,
	"[CHAIN] seconds between per-usercmd execution-chain ledger lines (0 = off).",
	true, 0.f, true, 600.f);

static std::atomic<uint64_t> s_chainCounters[CMDCHAIN_COUNT] = {};

// Short column names, indexed by CmdChainStage_t.
static const char* const s_chainNames[CMDCHAIN_COUNT] = {
	"intakeCalls", "intakeCmds", "intakeClamped",
	"simCalls", "simStock", "simBot", "simExecuted", "simQueueLeft",
	"wrapper", "wrapperBudgetDrop", "engineReturned", "startCmd",
	"jetdrive", "energize", "laser", "execStamp", "simStarved",
};

// The stages a command must pass, in execution order. StartCommand is a witness
// rather than a spine element: it still fires when the wrapper is bypassed,
// which is exactly the case the spine alone cannot name.
static const CmdChainStage_t s_chainSpine[] = {
	CMDCHAIN_INTAKE_CMDS,
	CMDCHAIN_SIM_CALLS,
	CMDCHAIN_SIM_EXECUTED,
	CMDCHAIN_WRAPPER_ENTERED,
	CMDCHAIN_ENGINE_RETURNED,
	CMDCHAIN_STAMP_EXEC,
};

struct ChainGateSnapshot_s
{
	int    m_nQueued;
	int    m_nRun;
	double m_dLead;
};
static ChainGateSnapshot_s s_chainGate = {};

static void(__fastcall* v_CPlayerMove_StartCommand)(void* thisp, void* player, void* moveHelper, void* cmd) = nullptr;

void CmdChain_Bump(const CmdChainStage_t stage, const uint64_t nCount)
{
	if (stage >= 0 && stage < CMDCHAIN_COUNT)
		s_chainCounters[stage].fetch_add(nCount, std::memory_order_relaxed);
}

uint64_t CmdChain_Get(const CmdChainStage_t stage)
{
	if (stage < 0 || stage >= CMDCHAIN_COUNT)
		return 0;

	return s_chainCounters[stage].load(std::memory_order_relaxed);
}

void CmdChain_NoteGate(const int nQueued, const int nRun, const double dLead)
{
	s_chainGate.m_nQueued = nQueued;
	s_chainGate.m_nRun = nRun;
	s_chainGate.m_dLead = dLead;
}

//-----------------------------------------------------------------------------
// Live dispatch forensics: the vtable g_PlayerMove is carrying RIGHT NOW and the
// first byte behind each hooked slot. Sampled at report time rather than at
// attach, so a vtable swapped after boot or an entry point un-patched at
// runtime shows up as a changed value instead of staying invisible.
//-----------------------------------------------------------------------------
static void CmdChain_FormatDispatch(char* const pszOut, const size_t nOutSize)
{
	void** const ppVtbl = *reinterpret_cast<void***>(
		g_GameDll.GetModuleBase() + CHAIN_RVA_G_PLAYERMOVE);

	if (!ppVtbl)
	{
		V_snprintf(pszOut, nOutSize, "vtbl=NULL");
		return;
	}

	const uint8_t* const pRun = reinterpret_cast<const uint8_t*>(ppVtbl[1]);
	const uint8_t* const pStart = reinterpret_cast<const uint8_t*>(ppVtbl[4]);
	const uint8_t nRunByte0 = pRun ? pRun[0] : 0;
	const uint8_t nStartByte0 = pStart ? pStart[0] : 0;

	V_snprintf(pszOut, nOutSize,
		"vtbl=%p slot1=%p(%02X %s) slot4=%p(%02X %s) trampoline=%p",
		reinterpret_cast<void*>(ppVtbl),
		reinterpret_cast<const void*>(pRun), nRunByte0,
		(nRunByte0 == 0xE9 || nRunByte0 == 0xEB) ? "patched" : "UNPATCHED",
		reinterpret_cast<const void*>(pStart), nStartByte0,
		(nStartByte0 == 0xE9 || nStartByte0 == 0xEB) ? "patched" : "UNPATCHED",
		reinterpret_cast<void*>(CPlayerMove__RunCommand));
}

//-----------------------------------------------------------------------------
// Name the break rather than leaving the reader to diff columns.
//-----------------------------------------------------------------------------
static void CmdChain_FormatVerdict(char* const pszOut, const size_t nOutSize)
{
	const uint64_t nWrapper = CmdChain_Get(CMDCHAIN_WRAPPER_ENTERED);
	const uint64_t nStartCmd = CmdChain_Get(CMDCHAIN_STARTCMD_ENTERED);
	const uint64_t nExecuted = CmdChain_Get(CMDCHAIN_SIM_EXECUTED);

	// The wrapper-bypass case first: the spine would blame the wrapper, but the
	// StartCommand witness proves the engine body ran without it.
	if (nExecuted > 0 && nWrapper == 0 && nStartCmd > 0)
	{
		V_snprintf(pszOut, nOutSize,
			"BREAK: RunCommand body RAN (startCmd=%llu) but the wrapper detour never fired -- entry point bypassed",
			static_cast<unsigned long long>(nStartCmd));
		return;
	}

	if (nExecuted > 0 && nWrapper == 0 && nStartCmd == 0)
	{
		V_snprintf(pszOut, nOutSize,
			"BREAK: PlayerRunCommand issued %llu time(s) but RunCommand never entered -- vtable dispatch",
			static_cast<unsigned long long>(nExecuted));
		return;
	}

	// A starved gate and a dead hook are indistinguishable downstream; the
	// clock lead is the discriminator, so say which one it is.
	if (nExecuted == 0 && CmdChain_Get(CMDCHAIN_SIM_CALLS) > 0 && s_chainGate.m_nQueued > 0)
	{
		V_snprintf(pszOut, nOutSize,
			"BREAK: execution gate admitted nothing with q=%d (clock lead %+.4f) -- not a hook problem",
			s_chainGate.m_nQueued, s_chainGate.m_dLead);
		return;
	}

	for (size_t i = 0; i < V_ARRAYSIZE(s_chainSpine); ++i)
	{
		if (CmdChain_Get(s_chainSpine[i]) != 0)
			continue;

		if (i == 0)
		{
			V_snprintf(pszOut, nOutSize, "IDLE: no commands have arrived yet");
			return;
		}

		V_snprintf(pszOut, nOutSize, "BREAK: %s=%llu -> %s=0",
			s_chainNames[s_chainSpine[i - 1]],
			static_cast<unsigned long long>(CmdChain_Get(s_chainSpine[i - 1])),
			s_chainNames[s_chainSpine[i]]);
		return;
	}

	V_snprintf(pszOut, nOutSize, "OK end-to-end");
}

void CmdChain_Report(void)
{
	const float flPeriod = bridge_cmd_chain.GetFloat();

	if (flPeriod <= 0.0f)
		return;

	static float s_flLastReport = 0.0f;
	const float flNow = static_cast<float>(Plat_FloatTime());

	if ((flNow - s_flLastReport) < flPeriod)
		return;

	s_flLastReport = flNow;

	char szVerdict[256];
	CmdChain_FormatVerdict(szVerdict, sizeof(szVerdict));

	Msg(eDLL_T::SERVER,
		"[CHAIN] intake=%llu/%llu clamped=%llu sim=%llu(stock %llu bot %llu) exec=%llu queueLeft=%llu starve=%llu "
		"wrap=%llu budgetDrop=%llu engRet=%llu startCmd=%llu jet=%llu nrg=%llu laser=%llu stamp=%llu "
		"| last q=%d run=%d lead=%+.4f | %s\n",
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_INTAKE_CALLS)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_INTAKE_CMDS)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_INTAKE_CLAMPED)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_SIM_CALLS)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_SIM_STOCK)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_SIM_BOT)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_SIM_EXECUTED)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_SIM_QUEUE_LEFT)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_SIM_STARVED)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_WRAPPER_ENTERED)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_WRAPPER_BUDGET_DROP)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_ENGINE_RETURNED)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_STARTCMD_ENTERED)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_TICK_JETDRIVE)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_TICK_ENERGIZE)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_TICK_LASER)),
		static_cast<unsigned long long>(CmdChain_Get(CMDCHAIN_STAMP_EXEC)),
		s_chainGate.m_nQueued, s_chainGate.m_nRun, s_chainGate.m_dLead,
		szVerdict);

	// The dispatch state only matters once something downstream is missing.
	if (szVerdict[0] == 'B')
	{
		char szDispatch[256];
		CmdChain_FormatDispatch(szDispatch, sizeof(szDispatch));
		Warning(eDLL_T::SERVER, "[CHAIN] dispatch: %s\n", szDispatch);
	}
}

static void CC_CmdChain_Reset_f(const CCommand& args)
{
	NOTE_UNUSED(args);

	for (int i = 0; i < CMDCHAIN_COUNT; ++i)
		s_chainCounters[i].store(0, std::memory_order_relaxed);

	s_chainGate = {};
	Msg(eDLL_T::SERVER, "[CHAIN] counters reset\n");
}

static ConCommand bridge_cmd_chain_reset("bridge_cmd_chain_reset", CC_CmdChain_Reset_f,
	"[CHAIN] zero the execution-chain counters (A/B a convar without restarting).", FCVAR_RELEASE);

//-----------------------------------------------------------------------------
// CPlayerMove::StartCommand. RunCommand dispatches it
// through this->vtbl[4] at +0x60, unconditionally and before any branch, and it
// has no other code xrefs, so an entry here proves the RunCommand BODY executed
// even in the case where our RunCommand detour is being skipped.
//-----------------------------------------------------------------------------
static void __fastcall Hook_CPlayerMove_StartCommand(void* thisp, void* player, void* moveHelper, void* cmd)
{
	CmdChain_Bump(CMDCHAIN_STARTCMD_ENTERED);
	v_CPlayerMove_StartCommand(thisp, player, moveHelper, cmd);
}

void VBridgeCmdChain::GetAdr(void) const
{
	LogFunAdr("CPlayerMove::StartCommand", v_CPlayerMove_StartCommand);
}

void VBridgeCmdChain::GetFun(void) const
{
	Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 57 48 83 EC 20 49 8B 00 49 8B C8 49 8B D9 48 8B FA FF 50 08").GetPtr(v_CPlayerMove_StartCommand);
}

void VBridgeCmdChain::Detour(const bool bAttach) const
{
	static bool s_bWitnessAttached = false;

	if (!bAttach)
	{
		if (s_bWitnessAttached)
		{
			s_bWitnessAttached = false;
			DetourSetup(&v_CPlayerMove_StartCommand, &Hook_CPlayerMove_StartCommand, false);
		}
		return;
	}

	// Cross-check the pattern against the live vtable before hooking: slot 4 is
	// StartCommand only if slot 1 is the RunCommand we already resolved.
	{
		void** const ppVtbl = *reinterpret_cast<void***>(
			g_GameDll.GetModuleBase() + CHAIN_RVA_G_PLAYERMOVE);

		const void* const pSlot1 = ppVtbl ? ppVtbl[1] : nullptr;
		const void* const pSlot4 = ppVtbl ? ppVtbl[4] : nullptr;

		if (pSlot1 != reinterpret_cast<void*>(CPlayerMove__RunCommand) ||
			pSlot4 != reinterpret_cast<void*>(v_CPlayerMove_StartCommand))
		{
			Warning(eDLL_T::SERVER,
				"[CHAIN] g_PlayerMove vtable mismatch -- slot1=%p (expected %p) slot4=%p (expected %p); "
				"StartCommand witness NOT installed\n",
				pSlot1, reinterpret_cast<void*>(CPlayerMove__RunCommand),
				pSlot4, reinterpret_cast<void*>(v_CPlayerMove_StartCommand));
			return;
		}

		Msg(eDLL_T::SERVER,
			"[CHAIN] armed: vtbl=%p slot1=%p slot4=%p -- StartCommand witness live\n",
			reinterpret_cast<void*>(ppVtbl), pSlot1, pSlot4);
	}

	s_bWitnessAttached = true;
	DetourSetup(&v_CPlayerMove_StartCommand, &Hook_CPlayerMove_StartCommand, true);
}
