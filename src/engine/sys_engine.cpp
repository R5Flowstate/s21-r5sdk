#if defined(CLIENT_DLL)
#include "core/stdafx.h"
#include "sys_engine.h"

CEngine* g_pEngine = nullptr;
IEngine::QuitState_t* gsm_Quitting = nullptr;
#else // !CLIENT_DLL
#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "sys_engine.h"
#include "host.h"
#include "engine/server/server.h"
#include "engine/tick_budget.h"
#include "engine/server/snapshot_send.h" // Bridge_SnapSyncSendActive
#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>

// Older SDK headers may lack the Win11 22H2+ timer-resolution throttle flag.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

///////////////////////////////////////////////////////////////////////////////
CEngine* g_pEngine = nullptr;
IEngine::QuitState_t* gsm_Quitting = nullptr;

static ConVar server_fps_clampToTicks("server_fps_clampToTicks", "1", FCVAR_RELEASE, "Clamp the server FPS to TIME_TO_TICKS( 1.0f ); minimizes oversampling to reduce CPU usage.", "bool");

static ConVar server_unthrottle("server_unthrottle", "1", FCVAR_RELEASE,
	"Opt the server process out of Windows background power/timer throttling. Win11 ignores the engine's "
	"timeBeginPeriod(1) while the process is backgrounded/occluded, quantizing CEngine::Frame's Sleep(1) "
	"tick wait to 15.625ms steps (50ms tick -> 4 sleeps = 62.5ms = 16Hz).", "bool");

static ConVar server_tick_budget("server_tick_budget", "0", FCVAR_RELEASE,
	"Ticks per [TICK-BUDGET] report (0 = off, 100 = every 5s at tick rate 20). Reports the "
	"per-tick PERIOD distribution and the snapshot-send share of it. A 5s average can sit at "
	"19.8 fps while a third of the ticks miss their deadline, which is what players feel.",
	true, 0.f, true, 2000.f);

static std::atomic<long long> s_tbLastScmUs{ 0 };
static std::atomic<long long> s_tbLastWaitUs{ 0 };
static std::atomic<int>       s_tbLastRecipients{ 0 };

bool TickBudget_Armed(void)
{
	return server_tick_budget.GetInt() > 0;
}

void TickBudget_ReportScm(long long scmUs, long long waitUs, int recipients)
{
	s_tbLastScmUs.store(scmUs, std::memory_order_relaxed);
	s_tbLastWaitUs.store(waitUs, std::memory_order_relaxed);
	s_tbLastRecipients.store(recipients, std::memory_order_relaxed);
}

// 1ms buckets 0..255, last bucket is the overflow tail.
static constexpr int TB_BUCKETS = 257;

static int TickBudget_Percentile(const unsigned short* hist, int total, int pct)
{
	const int want = (total * pct + 99) / 100;
	int seen = 0;

	for (int i = 0; i < TB_BUCKETS; ++i)
	{
		seen += hist[i];
		if (seen >= want)
			return i;
	}
	return TB_BUCKETS - 1;
}

//-----------------------------------------------------------------------------
// Purpose: census the interval between consecutive simulation ticks. Unlike the
// tick-rate monitor this keeps the distribution, so a tick period that is fine
// on average but spikes past the 50ms deadline is visible instead of averaged
// away -- the failure mode that scales with player count.
//-----------------------------------------------------------------------------
static void MonitorTickBudget(void)
{
	static unsigned short s_hist[TB_BUCKETS] = {};
	static long long s_prevQpc = 0;
	static long long s_sumUs = 0, s_maxUs = 0;
	static long long s_scmSumUs = 0, s_scmMaxUs = 0, s_waitSumUs = 0;
	static int s_count = 0, s_overTick = 0, s_overDouble = 0, s_recipMax = 0;
	static bool s_wasArmed = false;

	const int window = server_tick_budget.GetInt();
	if (window <= 0)
	{
		s_wasArmed = false;
		return;
	}

	LARGE_INTEGER qpc, freq;
	QueryPerformanceCounter(&qpc);
	QueryPerformanceFrequency(&freq);

	const bool bFirst = !s_wasArmed || !s_prevQpc || freq.QuadPart <= 0;
	const long long prevQpc = s_prevQpc;
	s_prevQpc = qpc.QuadPart;
	s_wasArmed = true;

	// The first tick after arming has no predecessor to measure against.
	if (bFirst)
		return;

	const long long periodUs = (qpc.QuadPart - prevQpc) * 1000000LL / freq.QuadPart;
	const long long scmUs = s_tbLastScmUs.exchange(0, std::memory_order_relaxed);
	const long long waitUs = s_tbLastWaitUs.exchange(0, std::memory_order_relaxed);
	const int recipients = s_tbLastRecipients.exchange(0, std::memory_order_relaxed);
	if (recipients > s_recipMax) s_recipMax = recipients;
	const int tickRate = HOST_TIME_TO_TICKS(1.0f);
	const long long targetUs = (tickRate > 0 && tickRate <= 300) ? (1000000LL / tickRate) : 50000LL;

	const int bucket = (int)(periodUs / 1000LL);
	s_hist[(bucket < 0) ? 0 : (bucket >= TB_BUCKETS ? TB_BUCKETS - 1 : bucket)]++;
	s_sumUs += periodUs;
	s_scmSumUs += scmUs;
	s_waitSumUs += waitUs;
	if (periodUs > s_maxUs) s_maxUs = periodUs;
	if (scmUs > s_scmMaxUs) s_scmMaxUs = scmUs;
	// 10% over the deadline is late enough to be felt but not so tight that
	// ordinary scheduling noise fills the counter.
	if (periodUs > targetUs + targetUs / 10) s_overTick++;
	if (periodUs > targetUs * 2) s_overDouble++;
	s_count++;

	if (s_count < window)
		return;

	Warning(eDLL_T::SERVER,
		"[TICK-BUDGET] %d ticks | period avg=%.1f p50=%d p95=%d max=%.1f ms (target %.1f) | "
		"late %d (%.0f%%) doubled %d | scm avg=%.1f max=%.1f wait avg=%.1f ms | "
		"syncSend=%d | %d client(s) %d snapshot recipient(s)\n",
		s_count,
		(double)s_sumUs / s_count / 1000.0,
		TickBudget_Percentile(s_hist, s_count, 50),
		TickBudget_Percentile(s_hist, s_count, 95),
		(double)s_maxUs / 1000.0,
		(double)targetUs / 1000.0,
		s_overTick, 100.0 * s_overTick / s_count, s_overDouble,
		(double)s_scmSumUs / s_count / 1000.0,
		(double)s_scmMaxUs / 1000.0,
		(double)s_waitSumUs / s_count / 1000.0,
		Bridge_SnapSyncSendActive() ? 1 : 0,
		g_pServer ? g_pServer->GetNumClients() : -1,
		s_recipMax);

	memset(s_hist, 0, sizeof(s_hist));
	s_sumUs = s_maxUs = 0;
	s_scmSumUs = s_scmMaxUs = s_waitSumUs = 0;
	s_count = s_overTick = s_overDouble = s_recipMax = 0;
}

static inline void ClampServerFPSToTicks()
{
	const int tickRate = HOST_TIME_TO_TICKS(1.0f);

	if (fps_max->GetInt() == tickRate)
		return;

	// Clamp the framerate of the server to its simulation tick rate.
	// This saves a significant amount of CPU time in CEngine::Frame,
	// as the engine uses this to decided when to run a new frame.
	fps_max->SetValue(tickRate);
}

//-----------------------------------------------------------------------------
// Purpose: opt the dedi process out of Windows background power/timer
// throttling so the simulation holds its tick rate while unfocused/occluded.
//
// The engine paces CEngine::Frame with timeBeginPeriod(1) + Sleep(1)
// (r5apex_ds @..). Windows 11 ignores
// the 1ms timer-resolution request for background/occluded processes unless
// the process opts out, so each Sleep(1) becomes ~15.625ms and the 50ms tick
// wait quantizes to 62.5ms = 16Hz. EcoQoS (execution-speed throttling) is
// disabled for the same reason.
//-----------------------------------------------------------------------------
static void UnthrottleServerProcess(void)
{
	PROCESS_POWER_THROTTLING_STATE state;
	memset(&state, 0, sizeof(state));
	state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;

	// StateMask 0 = never throttle execution speed (no EcoQoS/E-core parking).
	state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
	state.StateMask   = 0;
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));

	// StateMask 0 = always honor timer-resolution requests, even when
	// backgrounded/occluded (Win11 22H2+; older builds fail benignly).
	state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
	state.StateMask   = 0;
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));

	// The engine only requests 1ms lazily on its first frame-limiter sleep;
	// request it eagerly so pacing is precise from the very first tick.
	timeBeginPeriod(1);
}

bool CEngine::_Frame(CEngine* thisp)
{
	if (server_fps_clampToTicks.GetBool())
		ClampServerFPSToTicks();

	static bool s_bUnthrottled = false;
	if (!s_bUnthrottled && server_unthrottle.GetBool())
	{
		s_bUnthrottled = true;
		UnthrottleServerProcess();
	}

	const bool bRanFrame = CEngine__Frame(thisp);

	if (bRanFrame)
		MonitorTickBudget();

	return bRanFrame;
}

void VEngine::Detour(const bool bAttach) const
{
	DetourSetup(&CEngine__Frame, &CEngine::_Frame, bAttach);
}
#endif // CLIENT_DLL
