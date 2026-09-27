//=============================================================================//
//
// Purpose: The stock survey feeds EA telemetry only. It runs synchronous WMI
//          queries with an infinite enumerator timeout and reads each video
//          controller's Name without a null check, on the main thread before
//          any pak loads. -sdk_allow_hw_survey runs the stock body.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/commandline.h"
#include "thirdparty/detours/include/detours.h"
#include "pin_survey.h"

// Flushed per line; the engine log channels are async and die with the process.
void SDK_Log(const char* fmt, ...);

static __int64 __fastcall Hook_PinClientStart(void)
{
	static int s_nCalls = 0;
	const bool bLog = s_nCalls++ == 0;

	if (bLog)
		SDK_Log("[PIN-SURVEY] client_start enter\n");
	const __int64 result = v_PinClientStart();
	if (bLog)
		SDK_Log("[PIN-SURVEY] client_start exit\n");
	return result;
}

static void __fastcall Hook_PinHardwareSurvey(void)
{
	if (CommandLine() && CommandLine()->CheckParm("-sdk_allow_hw_survey"))
	{
		SDK_Log("[PIN-SURVEY] retail WMI survey running (-sdk_allow_hw_survey)\n");
		v_PinHardwareSurvey();
		SDK_Log("[PIN-SURVEY] retail WMI survey returned\n");
		return;
	}
	SDK_Log("[PIN-SURVEY] retail WMI OS/video survey skipped\n");
}

void VPinSurvey::Detour(const bool bAttach) const
{
	if (v_PinClientStart)
		DetourSetup(&v_PinClientStart, &Hook_PinClientStart, bAttach);
	if (v_PinHardwareSurvey)
		DetourSetup(&v_PinHardwareSurvey, &Hook_PinHardwareSurvey, bAttach);
}
