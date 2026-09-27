//=============================================================================//
//
// Purpose: Skip the stock telemetry hardware survey (WMI OS + video controller
//          query) that LauncherMain runs before the first pak loads.
//
//=============================================================================//
#ifndef EBISUSDK_PIN_SURVEY_H
#define EBISUSDK_PIN_SURVEY_H

#include "tier0/dbg.h"
#include "tier0/module.h"
#include "thirdparty/detours/include/idetour.h"

inline __int64(__fastcall* v_PinClientStart)(void) = nullptr;
inline void(__fastcall* v_PinHardwareSurvey)(void) = nullptr;

class VPinSurvey : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("PinClientStart", v_PinClientStart);
		LogFunAdr("PinHardwareSurvey", v_PinHardwareSurvey);
	}
	virtual void GetFun(void) const
	{
		// Prologue + 0x4B40 frame + one-shot survey flag test; unique in both client exes.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
			"B8 40 4B 00 00 E8 ?? ?? ?? ?? 48 2B E0 80 3D ?? ?? ?? ?? 00 75 05 E8")
			.GetPtr(v_PinClientStart);
		// CoInitializeEx(NULL, COINIT_MULTITHREADED) opening the WMI session.
		Module_FindPattern(g_GameDll,
			"40 55 48 8D 6C 24 ?? 48 81 EC F0 00 00 00 33 D2 33 C9 FF 15 ?? ?? ?? ?? 85 C0 0F 88")
			.GetPtr(v_PinHardwareSurvey);
		if (!v_PinClientStart)
			Warning(eDLL_T::CLIENT, "[PIN-SURVEY] client_start pattern unresolved -- no breadcrumbs\n");
		if (!v_PinHardwareSurvey)
			Warning(eDLL_T::CLIENT, "[PIN-SURVEY] hardware survey pattern unresolved -- retail WMI query stays live\n");
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // EBISUSDK_PIN_SURVEY_H
