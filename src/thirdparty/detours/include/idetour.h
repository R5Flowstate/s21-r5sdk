#ifndef IDETOUR_H
#define IDETOUR_H

//-----------------------------------------------------------------------------
// Null-target / pre-attach accounting (see idetour.cpp). Phase C reads these
// so a class is never reported "Hooked" after only null DetourSetup calls.
//-----------------------------------------------------------------------------
void Detour_ResetNullSkipCount(void);
LONG Detour_ConsumeNullSkipCount(void);
LONG Detour_GetNullSkipCount(void);
void Detour_OnNullTarget(void* ppPointer);
void Detour_OnInvalidTarget(void* pFn, const char* reason);

// Records which detour claimed a target during the current attach pass and
// reports loudly when a second one claims the same address -- the later attach
// wins the entry jmp and the earlier hook is silently orphaned.
void Detour_NoteAttachTarget(void* pFn, void* pDetour);

// Names the V-class in every report above. Set around each Detour(true) call.
void Detour_SetAttachingClass(const char* pszClass);

// Clears the claimed-target map. Call ONCE per attach pass: a per-class reset
// makes the duplicate report inert, since no two classes ever share the map.
void Detour_ResetAttachTargets(void);

// Optional pre-attach validator (Phase B mini). Return true to allow DetourAttach.
// Installed by Systems_Init_S21 to DetourValidator_ValidateFunctionAddress.
typedef bool (*PFN_DetourPreAttachValidate)(void* pFn, const char** ppszReason);
extern PFN_DetourPreAttachValidate g_DetourPreAttachValidate;

// Optional log sink for the target-failure reports above. This TU is kept free
// of tier0 (module layering), so the product init installs a shim that forwards
// to Warning(); without it a pattern miss only reaches OutputDebugStringA and is
// invisible in the logs -- the failure mode is silent exactly when it matters.
typedef void (*PFN_DetourLogSink)(const char* pszMsg);
extern PFN_DetourLogSink g_DetourLogSink;

//-----------------------------------------------------------------------------
// Interface class for context hooks
//-----------------------------------------------------------------------------
class IDetour
{
public:
	virtual ~IDetour() { ; }
	virtual void GetAdr(void) const = 0;
	virtual void GetFun(void) const = 0;
	virtual void GetVar(void) const = 0;
	virtual void GetCon(void) const = 0;

	virtual void Detour(const bool bAttach) const = 0;
	template<
		typename T,
		typename std::enable_if<DetoursIsFunctionPointer<T>::value, int>::type = 0>
	LONG DetourSetup(_Inout_ T* ppPointer, _In_ T pDetour, const bool bAttach) const
	{
		// Null target = pattern miss. Never pretend success (was: return NO_ERROR
		// which made Phase C mark the whole V-class Hooked with zero attaches).
		if (!ppPointer || !*ppPointer)
		{
			Detour_OnNullTarget(ppPointer);
			return ERROR_INVALID_HANDLE;
		}

		// Soft prologue gate: log only. Hard-rejecting unknown prologues
		// (e.g. mov r11,rsp = 4C 8B DC) unhooked NET_ReceiveDatagram and
		// broke the entire bridge handshake (PollReceive never ran).
		// Null targets remain a hard fail above.
		if (bAttach && g_DetourPreAttachValidate)
		{
			const char* reason = nullptr;
			if (!g_DetourPreAttachValidate(reinterpret_cast<void*>(*ppPointer), &reason))
			{
				Detour_OnInvalidTarget(reinterpret_cast<void*>(*ppPointer), reason);
				// fall through and still attach
			}
		}

		if (bAttach)
		{
			Detour_NoteAttachTarget(reinterpret_cast<void*>(*ppPointer), reinterpret_cast<void*>(pDetour));
			return DetourAttach(ppPointer, pDetour);
		}
		else
			return DetourDetach(ppPointer, pDetour);
	}
};

extern std::vector<IDetour*> g_DetourVec;
std::size_t AddDetour(IDetour* const pDetour);

#define ADDDETOUR(x,y) static std::size_t dummy_reg_##y = AddDetour( new x() );
#define XREGISTER(x,y)  ADDDETOUR(x, y)
#define REGISTER(x)     XREGISTER(x, __COUNTER__)

#endif // IDETOUR_H
