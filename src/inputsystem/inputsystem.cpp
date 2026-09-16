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
#include "tier0/module.h"
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
	"Mouse-wheel +weaponCycle uses the KBM cycle path when a pad is connected. Gamepad Y stays native. 0 = always native.");

static uint32_t* s_pWeapCycleState = nullptr;
static uint8_t* s_pWeapCyclePressed = nullptr;
static bool s_bCycleFromMouse = false;

static constexpr ptrdiff_t kCycleKeyDownDownLoad = 0x28; // 8B 05 in_weapon_cycle.down[0]
static constexpr int kMaxCycleKeyFnScan = 200;

static bool WeapCycle_IsPressed(void)
{
	if (s_pWeapCyclePressed && *s_pWeapCyclePressed)
		return true;
	if (s_pWeapCycleState && (*s_pWeapCycleState & 3u) != 0)
		return true;
	return false;
}

static bool WeapCycle_ForceKbmPath(void)
{
	if (!bridge_weap_cycle_kbm.GetBool())
		return false;
	if (!WeapCycle_IsPressed())
	{
		s_bCycleFromMouse = false;
		return false;
	}
	return s_bCycleFromMouse;
}

static uint32_t* CycleRipDword32(const CMemory insn)
{
	if (!insn)
		return nullptr;
	const uint8_t* p = insn.RCast<const uint8_t*>();
	if (!p || (p[0] != 0x8B && p[0] != 0xC7))
		return nullptr;
	const int nLen = (p[0] == 0xC7) ? 10 : 6;
	return insn.ResolveRelativeAddress(2, nLen).RCast<uint32_t*>();
}

static CMemory FindCycleKeyFn(uint32_t* const pWant, const char* const szPat,
	const ptrdiff_t nLoadOff, const ptrdiff_t nDeltaFromState)
{
	const CModule::ModuleSections_t* text = g_GameDll.FindSectionByName(".text");
	if (!text || !text->IsSectionValid() || !pWant)
		return CMemory();

	uintptr_t p = text->m_pSectionBase;
	const uintptr_t end = p + text->m_nSectionSize;
	int nSafety = 0;
	while (p < end && nSafety++ < kMaxCycleKeyFnScan)
	{
		CMemory found = CMemory(p).FindPattern(szPat, CMemory::Direction::DOWN,
			static_cast<int>(end - p), 1);
		if (!found)
			break;

		uint32_t* const resolved = CycleRipDword32(found.Offset(nLoadOff));
		uint8_t* const want = reinterpret_cast<uint8_t*>(pWant) + nDeltaFromState;
		if (resolved == reinterpret_cast<uint32_t*>(want))
			return found;

		p = static_cast<uintptr_t>(found) + 1;
	}
	return CMemory();
}

static void Hook_IN_WeaponCycleDown(void* args)
{
	if (args)
	{
		const int key = *reinterpret_cast<const int*>(args);
		s_bCycleFromMouse = (key == MOUSE_WHEEL_UP || key == MOUSE_WHEEL_DOWN);
		static bool s_loggedKey = false;
		if (!s_loggedKey)
		{
			s_loggedKey = true;
			Warning(eDLL_T::CLIENT, "[WEAP-CYCLE] +weaponCycle key=%d mouseWheel=%d\n",
				key, s_bCycleFromMouse ? 1 : 0);
		}
	}
	v_IN_WeaponCycleDown(args);
}

static bool Hook_CInput_ControllerModeActive(void* pInput)
{
	if (WeapCycle_ForceKbmPath())
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			Warning(eDLL_T::CLIENT,
				"[WEAP-CYCLE] mouse-wheel +weaponCycle -- using KBM cycle path\n");
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

	static const char s_keyDownPat[] =
		"40 53 48 83 EC 20 83 79 04 00 48 8D 05 ?? ?? ?? ?? 7E 07 "
		"48 8B 81 10 04 00 00";
	FindCycleKeyFn(s_pWeapCycleState, s_keyDownPat, kCycleKeyDownDownLoad, -8)
		.GetPtr(v_IN_WeaponCycleDown);

	if (!v_CInput_ControllerModeActive)
		Warning(eDLL_T::CLIENT, "[WEAP-CYCLE] ControllerModeActive pattern unresolved\n");
	if (!v_IN_WeaponCycleDown)
		Warning(eDLL_T::CLIENT,
			"[WEAP-CYCLE] IN_WeaponCycleDown unresolved -- mouse-wheel melee path disarmed\n");
}

void VWeapCycleKbm::Detour(const bool bAttach) const
{
	if (v_CInput_ControllerModeActive)
		DetourSetup(&v_CInput_ControllerModeActive, &Hook_CInput_ControllerModeActive, bAttach);
	if (v_IN_WeaponCycleDown)
		DetourSetup(&v_IN_WeaponCycleDown, &Hook_IN_WeaponCycleDown, bAttach);
}

///////////////////////////////////////////////////////////////////////////////
CInputSystem* g_pInputSystem = nullptr;
bool(**g_fnSyncRTWithIn)(void) = nullptr;
