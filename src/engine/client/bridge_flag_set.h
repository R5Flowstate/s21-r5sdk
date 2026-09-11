#ifndef BRIDGE_FLAG_SET_H
#define BRIDGE_FLAG_SET_H

//=============================================================================//
// Purpose: piggyback ClientInitComplete + ClientNetGlobalNonRewindEntity onto
// the working ClientNetGlobalEntity callback. Squirrel VM is not thread-safe.
//=============================================================================//

#ifndef DEDICATED

#include "tier0/platform.h"
#include "thirdparty/detours/include/idetour.h"
#include <atomic>

// Native: -- FlagSet helper. Resolves "FlagSet" function in
// the script VM by name and invokes it with the supplied flag name.
typedef __int64 (*FlagSetByName_t)(const char* pszFlagName);
inline FlagSetByName_t v_FlagSetByName = nullptr;

// ClientNetGlobalEntity callback. Pattern matches the NonRewind sibling too;
// Module_FindPattern returns the first hit.
typedef __int64 (*ClientNetGlobalEntityCb_t)(__int64 thisptr);
inline ClientNetGlobalEntityCb_t v_ClientNetGlobalEntityCb = nullptr;

// VGUI KeyCodeTyped default path. OnLocalViewPlayerChanged at snapshot rate
// can recurse here -- depth guard in Hook_UIKeyHandler.
typedef __int64 (*UIKeyHandler_t)(__int64 a1, unsigned int a2);
inline UIKeyHandler_t v_UIKeyHandler = nullptr;

// Total hook invocations (debug counter, lifetime of the DLL).
inline std::atomic<uint64_t> g_bridgeFlagsetHookCalls{0};

// Piggybacked FlagSets fire once per DLL load. Re-Set after lobby init
// can re-trigger RunClientConnectScriptsThreaded and respawn lobby entities.
inline std::atomic<bool> g_bridgeFlagsetPiggybackFired{false};

inline thread_local int g_uiKeyHandlerDepth = 0;

inline __int64 __fastcall Hook_UIKeyHandler(__int64 a1, unsigned int a2)
{
	if (g_uiKeyHandlerDepth >= 2)
	{
		static std::atomic<uint64_t> s_recursionBreaks{0};
		const uint64_t n = s_recursionBreaks.fetch_add(1, std::memory_order_relaxed) + 1;
		if (n <= 10 || (n % 100) == 0)
		{
			Warning(eDLL_T::CLIENT,
				"[BRIDGE-DIAG] UI key handler recursion broken: depth=%d "
				"a2=0x%X widget=%p (break #%llu)\n",
				g_uiKeyHandlerDepth, a2, (void*)a1,
				(unsigned long long)n);
		}
		return 0;
	}

	g_uiKeyHandlerDepth++;
	const __int64 result = v_UIKeyHandler(a1, a2);
	g_uiKeyHandlerDepth--;
	return result;
}

inline __int64 __fastcall Hook_ClientNetGlobalEntityCallback(__int64 thisptr)
{
	const uint64_t n = g_bridgeFlagsetHookCalls.fetch_add(1, std::memory_order_relaxed) + 1;

	// Run the original first so FlagSet("ClientNetGlobalEntity") flips
	// before any FlagWait observers wake.
	const __int64 result = v_ClientNetGlobalEntityCb(thisptr);

	if (!v_FlagSetByName)
	{
		Warning(eDLL_T::CLIENT,
			"[BRIDGE-FLAGS] v_FlagSetByName pattern unresolved -- "
			"EntitiesDidLoad will stay parked.\n");
		return result;
	}

	// Atomic CAS: only the first invocation runs the FlagSets. Subsequent
	// invocations log a single "skipped" line and bail.
	bool expected = false;
	const bool firstTime = g_bridgeFlagsetPiggybackFired.compare_exchange_strong(
		expected, true, std::memory_order_acq_rel);

	if (firstTime)
	{
		v_FlagSetByName("ClientInitComplete");
		v_FlagSetByName("ClientNetGlobalNonRewindEntity");
		Msg(eDLL_T::CLIENT,
			"[BRIDGE-FLAGS] call#%llu thisptr=%p ONCE-PER-PROCESS piggyback "
			"fired: FlagSet(\"ClientInitComplete\") + "
			"FlagSet(\"ClientNetGlobalNonRewindEntity\"). Subsequent invocations "
			"will be skipped (and counted).\n",
			(unsigned long long)n, (void*)thisptr);
	}
	else if (n <= 5 || (n % 25) == 0)
	{
		// Rate-limited skip log so we can see if the callback is being
		// driven in a runaway loop on coworker machines without blowing
		// up the warning.log. First 5 always logged, then every 25.
		Msg(eDLL_T::CLIENT,
			"[BRIDGE-FLAGS] call#%llu thisptr=%p skipped (already fired). "
			"If this number climbs continuously, the entity callback is "
			"being driven in a loop (can force lobby respawn).\n",
			(unsigned long long)n, (void*)thisptr);
	}

	return result;
}

class VBridgeFlagSet : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("Bridge::FlagSetByName", v_FlagSetByName);
		LogFunAdr("Bridge::ClientNetGlobalEntityCb", v_ClientNetGlobalEntityCb);
		LogFunAdr("Bridge::UIKeyHandler", v_UIKeyHandler);
	}
	virtual void GetFun(void) const
	{
		//: FlagSet helper. Single-match.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 55 41 56 41 57 48 83 EC 50 "
			"48 89 4C 24 ?? 4C 8D 05 ?? ?? ?? ?? 48 8B 0D")
			.GetPtr(v_FlagSetByName);

		//: ClientNetGlobalEntity entity callback. Pattern
		// also matches (NonRewind sibling); we want the
		// FIRST hit which Module_FindPattern returns by default.
		Module_FindPattern(g_GameDll,
			"48 83 EC 28 48 8B 01 33 D2 FF 50 20 "
			"48 8D 0D ?? ?? ?? ?? 48 83 C4 28 E9")
			.GetPtr(v_ClientNetGlobalEntityCb);

		//: KeyCodeTyped default path ( unique).
		Module_FindPattern(g_GameDll,
			"48 89 74 24 ?? 57 48 83 EC ?? 48 8B F9 8B F2 8B CA E8 ?? ?? ?? ?? F6 87")
			.GetPtr(v_UIKeyHandler);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const
	{
		if (v_ClientNetGlobalEntityCb)
			DetourSetup(&v_ClientNetGlobalEntityCb,
						&Hook_ClientNetGlobalEntityCallback, bAttach);
		if (v_UIKeyHandler)
			DetourSetup(&v_UIKeyHandler,
						&Hook_UIKeyHandler, bAttach);
	}
};

#endif // !DEDICATED

#endif // BRIDGE_FLAG_SET_H
