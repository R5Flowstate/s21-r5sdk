//=============================================================================//
//
// Purpose: Tripwire for the host frame time. The host-state worker hands
// _Host_RunFrame its dt in xmm1; any detour on that entry that forwards the
// wrong signature drops it to 0 and the whole client sims at the 1 ms floor.
//
//=============================================================================//
#include "core/stdafx.h"
#include "rtech/pak/pak_census.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"
#include "host_frame_probe.h"

typedef void (__fastcall *PFN_Host_RunFrame)(double realtime, float dt);
typedef void (__fastcall *PFN_HostState_RunFrame)(void* hostState, double realtime, float dt);

static PFN_Host_RunFrame      v_Host_RunFrame      = nullptr;
static PFN_HostState_RunFrame v_HostState_RunFrame = nullptr;

static ConVar bridge_host_frame_probe("bridge_host_frame_probe", "0", FCVAR_DEVELOPMENTONLY,
	"[HOST-FRAME] once per second: host frames run, dt handed in by the worker, dt received by _Host_RunFrame.");

static float  s_flWorkerDt   = -1.0f;
static int    s_nDtLost      = 0;
static int    s_nDtLostLogged = 0;
static int    s_nHostRun     = 0;
static double s_dWorkerDt    = 0.0;
static double s_dArgDt       = 0.0;
static double s_dWinStart    = 0.0;

static void __fastcall Hook_HostState_RunFrame(void* hostState, double realtime, float dt)
{
	s_flWorkerDt = dt;
	v_HostState_RunFrame(hostState, realtime, dt);
}

static void __fastcall Hook_Host_RunFrame(double realtime, float dt)
{
	Pak_CensusTick();
	if (dt == 0.0f && s_flWorkerDt > 0.0f)
	{
		++s_nDtLost;
		if (s_nDtLostLogged < 10)
		{
			++s_nDtLostLogged;
			Warning(eDLL_T::CLIENT,
				"[HOST-FRAME] DT LOST: worker handed %.5f, _Host_RunFrame received 0 -- "
				"a detour on _Host_RunFrame is not forwarding (double, float)\n",
				static_cast<double>(s_flWorkerDt));
		}
	}

	if (bridge_host_frame_probe.GetBool())
	{
		++s_nHostRun;
		s_dWorkerDt += s_flWorkerDt > 0.0f ? s_flWorkerDt : 0.0f;
		s_dArgDt    += dt;
		const double now = Plat_FloatTime();
		if (s_dWinStart <= 0.0)
			s_dWinStart = now;
		else if (now - s_dWinStart >= 1.0)
		{
			Msg(eDLL_T::CLIENT, "[HOST-FRAME] hostRun=%d dtLost=%d workerDt=%.3f argDt=%.3f over %.3fs\n",
				s_nHostRun, s_nDtLost, s_dWorkerDt, s_dArgDt, now - s_dWinStart);
			s_nHostRun = 0; s_dWorkerDt = 0.0; s_dArgDt = 0.0; s_dWinStart = now;
		}
	}

	v_Host_RunFrame(realtime, dt);
}

void VHostFrameProbe::GetAdr(void) const
{
	LogFunAdr("_Host_RunFrame", v_Host_RunFrame);
	LogFunAdr("CHostState::RunFrame", v_HostState_RunFrame);
}

void VHostFrameProbe::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 58 18 48 89 70 20 F3 0F 11 48 10 F2 0F 11 40 08 57 41 54 "
		"41 55 41 56 41 57 48 81 EC B0 08 00 00 80 3D")
		.GetPtr(v_Host_RunFrame);
	Module_FindPattern(g_GameDll,
		"40 53 57 41 56 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 45 33 F6 48 89 74 24 70 48 8B F9 "
		"0F 29 74 24 40 48 8D 35 ?? ?? ?? ?? 44 0F 29 44 24 20 0F 28 F2")
		.GetPtr(v_HostState_RunFrame);
	if (!v_Host_RunFrame || !v_HostState_RunFrame)
		Warning(eDLL_T::CLIENT, "[HOST-FRAME] pattern unresolved (run=%p worker=%p) -- tripwire off\n",
			reinterpret_cast<void*>(v_Host_RunFrame), reinterpret_cast<void*>(v_HostState_RunFrame));
}

void VHostFrameProbe::Detour(const bool bAttach) const
{
	if (v_HostState_RunFrame)
		DetourSetup(&v_HostState_RunFrame, &Hook_HostState_RunFrame, bAttach);
	if (v_Host_RunFrame)
		DetourSetup(&v_Host_RunFrame, &Hook_Host_RunFrame, bAttach);
}
