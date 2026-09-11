//===========================================================================//
//
// Purpose
//
//===========================================================================//

#include "core/stdafx.h"
#include "vpc/IAppSystem.h"
#include "windows/id3dx.h"
#include "inputsystem/inputsystem.h"
#include "gameui/imgui_system.h"
#include "tier1/convar.h"
#include "tier0/dbg.h"
#include <materialsystem/cmaterialsystem.h>

//-----------------------------------------------------------------------------
// Returns the currently attached window
//-----------------------------------------------------------------------------
PlatWindow_t CInputSystem::GetAttachedWindow() const
{
	return (PlatWindow_t)m_hAttachedHWnd;
}


//-----------------------------------------------------------------------------
// g_pInputSystem is null at DLL init; resolve from the engine slot on first ResetInput.
//-----------------------------------------------------------------------------
bool InputSystem_ResolveSingletonFromEngine()
{
	if (g_pInputSystem)
		return true;

	const uintptr_t base = g_GameDll.GetModuleBase();
	if (!base)
		return false;

	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exePath, SDK_ARRAYSIZE(exePath));
	const bool bDx12Exe = V_stristr(exePath, "dx12") != nullptr;

	const uintptr_t inputSystemRva = bDx12Exe ? 0x3403C80 : 0x74F4E10;
	g_pInputSystem = *reinterpret_cast<CInputSystem**>(base + inputSystemRva);

	if (g_pInputSystem)
	{
		extern void SDK_Log(const char* fmt, ...);
		SDK_Log("[BRIDGE-INPUT] g_pInputSystem lazily resolved -> %p\n", (void*)g_pInputSystem);
	}

	return g_pInputSystem != nullptr;
}

static void Hook_ApplyRawMouseAccum(int64_t timestamp, void* rawInputRecords, int64_t count)
{
	if (ImguiSystem()->IsSurfaceActive())
		return;

	v_ApplyRawMouseAccum(timestamp, rawInputRecords, count);
}

void VRawInputAccum::Detour(const bool bAttach) const
{
	DetourSetup(&v_ApplyRawMouseAccum, &Hook_ApplyRawMouseAccum, bAttach);
}

static ConVar bridge_weap_cycle_kbm("bridge_weap_cycle_kbm", "1", FCVAR_RELEASE,
	"Force the keyboard +weaponCycle path (mouse wheel) even when controller mode is on.");

static uint32_t* s_pWeapCycleState = nullptr;
static uint8_t* s_pWeapCyclePressed = nullptr;

static bool WeapCycle_IsPressed(void)
{
	if (s_pWeapCyclePressed && *s_pWeapCyclePressed)
		return true;
	if (s_pWeapCycleState && (*s_pWeapCycleState & 3u) != 0)
		return true;
	return false;
}

static bool Hook_CInput_ControllerModeActive(void* pInput)
{
	if (bridge_weap_cycle_kbm.GetBool() && WeapCycle_IsPressed())
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			Warning(eDLL_T::CLIENT,
				"[WEAP-CYCLE] +weaponCycle on controller-mode -- using KBM cycle path\n");
		}
		return false;
	}
	return v_CInput_ControllerModeActive(pInput);
}

void VWeapCycleKbm::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ?? 48 8B D9 48 8B 0D ?? ?? ?? ?? "
		"48 8B 01 FF 90 ?? ?? ?? ?? 84 C0 74 ?? 80 7B ?? ?? 74 ?? B0")
		.GetPtr(v_CInput_ControllerModeActive);

	const CMemory cycleBtn = Module_FindPattern(g_GameDll,
		"F6 C1 03 75 ?? 80 3D ?? ?? ?? ?? 00 74 ?? 0F BA EB 13");
	if (!cycleBtn)
	{
		Warning(eDLL_T::CLIENT, "[WEAP-CYCLE] in_weapon_cycle state pattern unresolved\n");
		return;
	}

	// State load (8B 0D) is 13 bytes before 'test cl,3'; a wrong offset is a wild pointer.
	const uint8_t* const siteBytes = cycleBtn.RCast<const uint8_t*>();
	if (siteBytes[-13] == 0x8B && siteBytes[-12] == 0x0D)
		s_pWeapCycleState = cycleBtn.Offset(-13).ResolveRelativeAddress(2, 6).RCast<uint32_t*>();
	else
		Warning(eDLL_T::CLIENT,
			"[WEAP-CYCLE] state-load opcodes drifted (%02X %02X) -- state check disarmed\n",
			siteBytes[-13], siteBytes[-12]);
	if (siteBytes[5] == 0x80 && siteBytes[6] == 0x3D)
		s_pWeapCyclePressed = cycleBtn.Offset(5).ResolveRelativeAddress(2, 7).RCast<uint8_t*>();
	else
		Warning(eDLL_T::CLIENT,
			"[WEAP-CYCLE] pressed-byte opcodes drifted (%02X %02X) -- pressed check disarmed\n",
			siteBytes[5], siteBytes[6]);

	if (!v_CInput_ControllerModeActive)
		Warning(eDLL_T::CLIENT, "[WEAP-CYCLE] ControllerModeActive pattern unresolved\n");
}

void VWeapCycleKbm::Detour(const bool bAttach) const
{
	if (v_CInput_ControllerModeActive)
		DetourSetup(&v_CInput_ControllerModeActive, &Hook_CInput_ControllerModeActive, bAttach);
}

///////////////////////////////////////////////////////////////////////////////
CInputSystem* g_pInputSystem = nullptr;
bool(**g_fnSyncRTWithIn)(void) = nullptr;
