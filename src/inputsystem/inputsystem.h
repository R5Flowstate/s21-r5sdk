#if defined(CLIENT_DLL)
#pragma once
#include "inputsystem/iinputsystem.h"
#include "mathlib/bitvec.h"
#include "tier1/utlstringmap.h"
#include <Xinput.h>

//-----------------------------------------------------------------------------
// Implementation of the input system
//-----------------------------------------------------------------------------
class CInputSystem : public CTier1AppSystem< IInputSystem >
{
public:
	// !!!interface implemented in engine!!!
public:
	PlatWindow_t GetAttachedWindow() const;

private:
	enum
	{
		INPUT_STATE_QUEUED = 0,
		INPUT_STATE_CURRENT,

		INPUT_STATE_COUNT,

		BUTTON_EVENT_COUNT = 128
	};

	struct xdevice_t
	{
		struct xvibration_t
		{
			float leftMainMotor;
			float rightMainMotor;
			float leftTriggerMotor;
			float rightTriggerMotor;
		};

		struct unkownhiddevice_t
		{
			struct state_t
			{
				SRWLOCK lock;
				char unk0[56];
				xvibration_t vibration;
				char unk1[48];
			};

			// Name might be incorrect!
			state_t states[INPUT_STATE_COUNT];
			HANDLE hThread0;
			HANDLE hthread1;
		};

		int userId;
		char active;
		XINPUT_STATE states[INPUT_STATE_COUNT];
		int newState;
		xKey_t lastStickKeys[MAX_JOYSTICK_AXES-2]; // -2 as U and V aren't polled.
		int unk0;
		bool pendingRumbleUpdate;
		_BYTE gap41[3];
		xvibration_t vibration;
		bool isXbox360Gamepad;
		bool nonXboxDevice; // uses unknownHidDevice when set
		_BYTE gap56[42];
		unkownhiddevice_t unknownHidDevice;
		_BYTE gap190[42];
	};
	static_assert(sizeof(xdevice_t) == 0x1C0);

	struct appKey_t
	{
		int repeats;
		int	sample;
	};

	struct InputState_t
	{
		// Analog states
		CBitVec<BUTTON_CODE_LAST> m_ButtonState;
		int m_pAnalogValue[ANALOG_CODE_LAST];
	};


	HWND m_ChainedWndProc;
	HWND m_hAttachedHWnd;
	bool m_bEnabled;
	bool m_bPumpEnabled;
	bool m_bIsPolling;
	bool m_bIMEComposing;
	bool m_bMouseCursorVisible;
	bool m_bJoystickCursorVisible;
	bool m_bIsInGame; // Delay joystick polling if in-game.

	// Current button state
	InputState_t m_InputState[INPUT_STATE_COUNT];

	// Current button state mutex
	CThreadMutex m_InputStateMutex;
	int unknown0;
	short unknown1;
	bool unknown2;

	// Analog event mutex
	CThreadMutex m_AnalogEventMutex;
	int unknown3;
	short unknown4;
	bool unknown5;

	// Analog events
	InputEvent_t m_AnalogEvents[JOYSTICK_AXIS_BUTTON_COUNT];
	int m_AnalogEventTypes[JOYSTICK_AXIS_BUTTON_COUNT];

	// Button events
	InputEvent_t m_Events[BUTTON_EVENT_COUNT];

	// Current event
	InputEvent_t m_CurrentEvent;

	DWORD m_StartupTimeTick;
	int m_nLastPollTick;
	int m_nLastSampleTick;
	int m_nLastAnalogPollTick;
	int m_nLastAnalogSampleTick;

	// Mouse wheel hack
	UINT m_uiMouseWheel;

	// Xbox controller info
	int m_nJoystickCount;
	appKey_t m_appXKeys[XUSER_MAX_COUNT][XK_MAX_KEYS];
	char pad_unk[16];
	xdevice_t m_XDevices[XUSER_MAX_COUNT];

	// Used to determine whether to generate UI events
	int m_nUIEventClientCount;

	// Raw mouse input
	bool m_bRawInputSupported;
	CThreadMutex m_MouseAccumMutex;
	int m_mouseRawAccumX;
	int m_mouseRawAccumY;

	_BYTE gap1785[8];

	// Current mouse capture window
	PlatWindow_t m_hCurrentCaptureWnd;

	// For the 'SleepUntilInput' feature
	HANDLE m_hEvent;

	InputCursorHandle_t m_pDefaultCursors[INPUT_CURSOR_COUNT];
	CUtlStringMap<InputCursorHandle_t> m_UserCursors;

	CSysModule* m_pXInputDLL;
	CSysModule* m_pRawInputDLL;

	// NVNT falcon module
	CSysModule* m_pNovintDLL; // Unused in R5?

	bool m_bIgnoreLocalJoystick;
	InputCursorHandle_t m_hCursor;
};
static_assert(sizeof(CInputSystem) == 0x18E8);

///////////////////////////////////////////////////////////////////////////////
extern CInputSystem* g_pInputSystem;
extern bool(**g_fnSyncRTWithIn)(void); // Belongs to an array of functions, see CMaterialSystem::MatsysMode_Init.


inline void(*v_ApplyRawMouseAccum)(int64_t timestamp, void* rawInputRecords, int64_t count);
inline bool(*v_CInput_ControllerModeActive)(void* pInput) = nullptr;
inline void(*v_IN_WeaponCycleDown)(void* args) = nullptr;

///////////////////////////////////////////////////////////////////////////////
// WM_INPUT mouse-look is independent of CInputSystem::m_bEnabled; drop deltas while ImGui is up.
class VRawInputAccum : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("ApplyRawMouseAccum", v_ApplyRawMouseAccum);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 4C 24 ?? 57 48 83 EC ?? 80 3D").GetPtr(v_ApplyRawMouseAccum);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

// Mouse-wheel +weaponCycle hold-swaps to melee while ControllerModeActive.
// Force the KBM cycle path for that source only; gamepad Y stays native.
class VWeapCycleKbm : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CInput::ControllerModeActive", v_CInput_ControllerModeActive);
		LogFunAdr("IN_WeaponCycleDown", v_IN_WeaponCycleDown);
	}
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
#else // !CLIENT_DLL
#pragma once
#include "inputsystem/iinputsystem.h"
#include "mathlib/bitvec.h"
#include "tier1/utlstringmap.h"
#include <Xinput.h>

//-----------------------------------------------------------------------------
// Implementation of the input system
//-----------------------------------------------------------------------------
class CInputSystem : public CTier1AppSystem< IInputSystem >
{
public:
	// !!!interface implemented in engine!!!
public:
	PlatWindow_t GetAttachedWindow() const;

private:
	enum
	{
		INPUT_STATE_QUEUED = 0,
		INPUT_STATE_CURRENT,

		INPUT_STATE_COUNT,

		BUTTON_EVENT_COUNT = 128
	};

	struct xdevice_t
	{
		struct xvibration_t
		{
			float leftMainMotor;
			float rightMainMotor;
			float leftTriggerMotor;
			float rightTriggerMotor;
		};

		struct unkownhiddevice_t
		{
			struct state_t
			{
				SRWLOCK lock;
				char unk0[56];
				xvibration_t vibration;
				char unk1[48];
			};

			// Name might be incorrect!
			state_t states[INPUT_STATE_COUNT];
			HANDLE hThread0;
			HANDLE hthread1;
		};

		int userId;
		char active;
		XINPUT_STATE states[INPUT_STATE_COUNT];
		int newState;
		xKey_t lastStickKeys[MAX_JOYSTICK_AXES-2]; // -2 as U and V aren't polled.
		int unk0;
		bool pendingRumbleUpdate;
		_BYTE gap41[3];
		xvibration_t vibration;
		bool isXbox360Gamepad;
		bool nonXboxDevice; // uses unknownHidDevice when set
		_BYTE gap56[42];
		unkownhiddevice_t unknownHidDevice;
		_BYTE gap190[42];
	};
	static_assert(sizeof(xdevice_t) == 0x1C0);

	struct appKey_t
	{
		int repeats;
		int	sample;
	};

	struct InputState_t
	{
		// Analog states
		CBitVec<BUTTON_CODE_LAST> m_ButtonState;
		int m_pAnalogValue[ANALOG_CODE_LAST];
	};


	HWND m_ChainedWndProc;
	HWND m_hAttachedHWnd;
	bool m_bEnabled;
	bool m_bPumpEnabled;
	bool m_bIsPolling;
	bool m_bIMEComposing;
	bool m_bMouseCursorVisible;
	bool m_bJoystickCursorVisible;
	bool m_bIsInGame; // Delay joystick polling if in-game.

	// Current button state
	InputState_t m_InputState[INPUT_STATE_COUNT];

	// Current button state mutex
	CThreadMutex m_InputStateMutex;
	int unknown0;
	short unknown1;
	bool unknown2;

	// Analog event mutex
	CThreadMutex m_AnalogEventMutex;
	int unknown3;
	short unknown4;
	bool unknown5;

	// Analog events
	InputEvent_t m_AnalogEvents[JOYSTICK_AXIS_BUTTON_COUNT];
	int m_AnalogEventTypes[JOYSTICK_AXIS_BUTTON_COUNT];

	// Button events
	InputEvent_t m_Events[BUTTON_EVENT_COUNT];

	// Current event
	InputEvent_t m_CurrentEvent;

	DWORD m_StartupTimeTick;
	int m_nLastPollTick;
	int m_nLastSampleTick;
	int m_nLastAnalogPollTick;
	int m_nLastAnalogSampleTick;

	// Mouse wheel hack
	UINT m_uiMouseWheel;

	// Xbox controller info
	int m_nJoystickCount;
	appKey_t m_appXKeys[XUSER_MAX_COUNT][XK_MAX_KEYS];
	char pad_unk[16];
	xdevice_t m_XDevices[XUSER_MAX_COUNT];

	// Used to determine whether to generate UI events
	int m_nUIEventClientCount;

	// Raw mouse input
	bool m_bRawInputSupported;
	CThreadMutex m_MouseAccumMutex;
	int m_mouseRawAccumX;
	int m_mouseRawAccumY;

	_BYTE gap1785[8];

	// Current mouse capture window
	PlatWindow_t m_hCurrentCaptureWnd;

	// For the 'SleepUntilInput' feature
	HANDLE m_hEvent;

	InputCursorHandle_t m_pDefaultCursors[INPUT_CURSOR_COUNT];
	CUtlStringMap<InputCursorHandle_t> m_UserCursors;

	CSysModule* m_pXInputDLL;
	CSysModule* m_pRawInputDLL;

	// NVNT falcon module
	CSysModule* m_pNovintDLL; // Unused in R5?

	bool m_bIgnoreLocalJoystick;
	InputCursorHandle_t m_hCursor;
};
static_assert(sizeof(CInputSystem) == 0x18E8);

///////////////////////////////////////////////////////////////////////////////
extern CInputSystem* g_pInputSystem;
extern bool(**g_fnSyncRTWithIn)(void); // Belongs to an array of functions, see CMaterialSystem::MatsysMode_Init.

#endif // CLIENT_DLL
