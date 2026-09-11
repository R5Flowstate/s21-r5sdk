#if defined(CLIENT_DLL)
#include "tier0/platform_internal.h"
#include "tier0/dbg.h"

//-----------------------------------------------------------------------------
// Script/engine LaunchExternalWebBrowser ends in ShellExecuteA("open"). A
// packed UI script or loc string can pass file:// (or any scheme) and run
// a local .exe. We do not open URLs; never call the original.
//-----------------------------------------------------------------------------
static void _Plat_LaunchExternalWebBrowser(const char* urlText, unsigned int flags)
{
	(void)flags;
	Warning(eDLL_T::ENGINE,
		"[Plat] LaunchExternalWebBrowser noop url='%.128s'\n",
		urlText ? urlText : "");
}

//-----------------------------------------------------------------------------
// Purpose: gets the process up time in seconds
// Output: double
//-----------------------------------------------------------------------------
static double Plat_FloatTime_Fallback()
{
	static LARGE_INTEGER s_Start = {};
	static LARGE_INTEGER s_Freq = {};
	static bool s_Init = false;
	if (!s_Init)
	{
		QueryPerformanceFrequency(&s_Freq);
		QueryPerformanceCounter(&s_Start);
		s_Init = true;
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - s_Start.QuadPart) / (double)s_Freq.QuadPart;
}

double Plat_FloatTime()
{
	if (v_Plat_FloatTime)
		return v_Plat_FloatTime();
	return Plat_FloatTime_Fallback();
}

//-----------------------------------------------------------------------------
// Purpose: gets the process up time in milliseconds
// Output: uint64_t
//-----------------------------------------------------------------------------
uint64_t Plat_MSTime()
{
	if (v_Plat_MSTime)
		return v_Plat_MSTime();
	return (uint64_t)(Plat_FloatTime_Fallback() * 1000.0);
}

//-----------------------------------------------------------------------------
// Purpose: reports time the calling thread spent blocked on something the game
//          did not ask for, so a duration measurement can discount it.
// Input: flSeconds -
//-----------------------------------------------------------------------------
static thread_local double s_flThreadStallTime = 0.0;

void Plat_AccumulateStallTime(double flSeconds)
{
	if (flSeconds > 0.0)
		s_flThreadStallTime += flSeconds;
}

//-----------------------------------------------------------------------------
// Purpose: monotonic per-thread total of the above; diff two snapshots.
// Output: double
//-----------------------------------------------------------------------------
double Plat_GetThreadStallTime()
{
	return s_flThreadStallTime;
}

//-----------------------------------------------------------------------------
// Purpose: raw QPC seconds for measuring blocked intervals. Never routed through
//          Plat_FloatTime -- the stall scope runs from the first boot log line,
//          before the engine's clock pointer is resolved.
// Output: double
//-----------------------------------------------------------------------------
double Plat_StallClockSeconds()
{
	static LARGE_INTEGER s_Freq = {};

	if (!s_Freq.QuadPart)
		QueryPerformanceFrequency(&s_Freq);

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	return (double)now.QuadPart / (double)s_Freq.QuadPart;
}

//-----------------------------------------------------------------------------
// Purpose: gets the process up time ( !! INTERNAL ONLY !! DO NOT USE !! ).
// Output: const char*
//-----------------------------------------------------------------------------
const char* Plat_GetProcessUpTime()
{
	// Thread-local: the returned pointer outlives the call in EngineLoggerSink
	// (it is still read after the log lock is dropped), so a shared buffer lets
	// one thread rewrite another thread's timestamp mid-line.
	static thread_local char szBuf[4096];
	sprintf_s(szBuf, sizeof(szBuf), "[%.3f] ", Plat_FloatTime());

	return szBuf;
}

//-----------------------------------------------------------------------------
// Purpose: gets the process up time.
// Input: *szBuf --
// nSize --
//-----------------------------------------------------------------------------
void Plat_GetProcessUpTime(char* szBuf, size_t nSize)
{
	sprintf_s(szBuf, nSize, "[%.3f] ", Plat_FloatTime());
}

void VPlatform::Detour(const bool bAttach) const
{
	if (v_Plat_LaunchExternalWebBrowser)
		DetourSetup(&v_Plat_LaunchExternalWebBrowser, &_Plat_LaunchExternalWebBrowser, bAttach);
	else if (bAttach)
		Warning(eDLL_T::ENGINE, "[Plat] LaunchExternalWebBrowser pattern unresolved -- ShellExecute path still live\n");
}
#else // !CLIENT_DLL
#include "tier0/platform_internal.h"
#include "tier0/dbg.h"

//-----------------------------------------------------------------------------
// Purpose: gets the process up time in seconds
// Output: double
//-----------------------------------------------------------------------------
double Plat_FloatTime()
{
	return v_Plat_FloatTime();
}

//-----------------------------------------------------------------------------
// Purpose: gets the process up time in milliseconds
// Output: uint64_t
//-----------------------------------------------------------------------------
uint64_t Plat_MSTime()
{
	return v_Plat_MSTime();
}

//-----------------------------------------------------------------------------
// Purpose: reports time the calling thread spent blocked on something the game
//          did not ask for, so a duration measurement can discount it.
// Input: flSeconds -
//-----------------------------------------------------------------------------
static thread_local double s_flThreadStallTime = 0.0;

void Plat_AccumulateStallTime(double flSeconds)
{
	if (flSeconds > 0.0)
		s_flThreadStallTime += flSeconds;
}

//-----------------------------------------------------------------------------
// Purpose: monotonic per-thread total of the above; diff two snapshots.
// Output: double
//-----------------------------------------------------------------------------
double Plat_GetThreadStallTime()
{
	return s_flThreadStallTime;
}

//-----------------------------------------------------------------------------
// Purpose: raw QPC seconds for measuring blocked intervals. Never routed through
//          Plat_FloatTime -- the stall scope runs from the first boot log line,
//          before the engine's clock pointer is resolved.
// Output: double
//-----------------------------------------------------------------------------
double Plat_StallClockSeconds()
{
	static LARGE_INTEGER s_Freq = {};

	if (!s_Freq.QuadPart)
		QueryPerformanceFrequency(&s_Freq);

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	return (double)now.QuadPart / (double)s_Freq.QuadPart;
}

//-----------------------------------------------------------------------------
// Purpose: gets the process up time ( !! INTERNAL ONLY !! DO NOT USE !! ).
// Output: const char*
//-----------------------------------------------------------------------------
const char* Plat_GetProcessUpTime()
{
	// Thread-local: the returned pointer outlives the call in EngineLoggerSink
	// (it is still read after the log lock is dropped), so a shared buffer lets
	// one thread rewrite another thread's timestamp mid-line.
	static thread_local char szBuf[4096];
	sprintf_s(szBuf, sizeof(szBuf), "[%.3f] ", v_Plat_FloatTime ? Plat_FloatTime() : 0.0);

	return szBuf;
}

//-----------------------------------------------------------------------------
// Purpose: gets the process up time.
// Input: *szBuf - 
// nSize - 
//-----------------------------------------------------------------------------
void Plat_GetProcessUpTime(char* szBuf, size_t nSize)
{
	sprintf_s(szBuf, nSize, "[%.3f] ", v_Plat_FloatTime ? Plat_FloatTime() : 0.0);
}

void VPlatform::Detour(const bool bAttach) const
{
	(void)bAttach;
}
#endif // CLIENT_DLL
