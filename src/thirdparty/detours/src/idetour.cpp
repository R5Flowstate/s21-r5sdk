//===========================================================================//
// 
// Purpose: Hook interface
// 
//===========================================================================//
#include <cassert>
#include <cstdio>
#include <vector>
#include <unordered_map>
#ifdef _DEBUG
#include <unordered_set>
#endif // _DEBUG
#include <Windows.h>
#include "../include/detours.h"
#include "../include/idetour.h"

//-----------------------------------------------------------------------------
// Contains a VFTable pointer, and the class instance. A VFTable can only be
// used by one class instance. This is to avoid duplicate registrations.
//-----------------------------------------------------------------------------
std::vector<IDetour*> g_DetourVec;
#ifdef _DEBUG
static std::unordered_set<const IDetour*> s_DetourSet;
#endif // _DEBUG

//-----------------------------------------------------------------------------
// Null / invalid-target accounting (cross-TU with Phase C)
//-----------------------------------------------------------------------------
static volatile LONG s_nDetourNullSkips = 0;
static volatile LONG s_nDetourInvalidSkips = 0;
PFN_DetourPreAttachValidate g_DetourPreAttachValidate = nullptr;
PFN_DetourLogSink g_DetourLogSink = nullptr;

// Target address -> the detour that claimed it first, for the attach pass.
struct DetourClaim_t
{
	void* pDetour;
	const char* pszClass;
};
static std::unordered_map<void*, DetourClaim_t> s_DetourTargets;

// The V-class whose Detour(true) is currently running. Attach is single
// threaded, so a plain global is enough to attribute every report below.
static const char* s_pszAttachingClass = nullptr;

void Detour_SetAttachingClass(const char* const pszClass)
{
	s_pszAttachingClass = pszClass;
}

void Detour_ResetAttachTargets(void)
{
	s_DetourTargets.clear();
}

void Detour_ResetNullSkipCount(void)
{
	InterlockedExchange(&s_nDetourNullSkips, 0);
	InterlockedExchange(&s_nDetourInvalidSkips, 0);
}

LONG Detour_GetNullSkipCount(void)
{
	return s_nDetourNullSkips + s_nDetourInvalidSkips;
}

LONG Detour_ConsumeNullSkipCount(void)
{
	const LONG n = s_nDetourNullSkips + s_nDetourInvalidSkips;
	InterlockedExchange(&s_nDetourNullSkips, 0);
	InterlockedExchange(&s_nDetourInvalidSkips, 0);
	return n;
}

void Detour_OnNullTarget(void* ppPointer)
{
	const LONG n = InterlockedIncrement(&s_nDetourNullSkips);
	// Loud: pattern unresolved. Prefer if (v_fn) DetourSetup(...) at call sites
	// for optional hooks so required-misses stay visible here.
	// OutputDebugStringA alone is invisible without a debugger attached, which
	// hides the single loudest failure mode in the hook system from the logs --
	// mirror it into warning.log so a silent pattern miss is diagnosable.
	char buf[192];
	_snprintf_s(buf, _TRUNCATE,
		"[DETOUR] %s: null target pp=%p (skip #%ld) -- pattern unresolved, not attaching\n",
		s_pszAttachingClass ? s_pszAttachingClass : "?", ppPointer, (long)n);
	OutputDebugStringA(buf);

	if (g_DetourLogSink)
		g_DetourLogSink(buf);
}

void Detour_OnInvalidTarget(void* pFn, const char* reason)
{
	// Soft advisory only (does NOT increment skip counter / does NOT block attach).
	// Unknown prologues are too common on S21 to hard-fail; null targets
	// are the hard gate. See idetour.h DetourSetup soft-validate path.
	char buf[256];
	_snprintf_s(buf, _TRUNCATE,
		"[DETOUR] %s: prologue advisory fn=%p reason=%s -- attaching anyway\n",
		s_pszAttachingClass ? s_pszAttachingClass : "?", pFn, reason ? reason : "?");
	OutputDebugStringA(buf);

	if (g_DetourLogSink)
		g_DetourLogSink(buf);
}

// A prologue that home-spills xmm0..3 belongs to a function taking float or
// double args in those registers; a hook declared with integer params drops
// them across its first call.
static int Detour_PrologueSpillsXmmArg(const unsigned char* code)
{
	for (int i = 0; i + 4 < 40; ++i)
	{
		const unsigned char* p = code + i;
		int modrm = -1;
		if ((p[0] == 0xF3 || p[0] == 0xF2) && p[1] == 0x0F && p[2] == 0x11)
			modrm = p[3];
		else if (p[0] == 0x0F && (p[1] == 0x29 || p[1] == 0x11))
			modrm = p[2];
		else if (p[0] == 0x66 && p[1] == 0x0F && p[2] == 0xD6)
			modrm = p[3];
		if (modrm < 0 || (modrm & 0xC0) == 0xC0)
			continue;
		const int reg = (modrm >> 3) & 7;
		if (reg <= 3)
			return reg;
		if (p[0] == 0xC3 || p[0] == 0xE9)
			break;
	}
	return -1;
}

void Detour_NoteAttachTarget(void* pFn, void* pDetour)
{
	if (!pFn)
		return;

	const char* const pszClass = s_pszAttachingClass ? s_pszAttachingClass : "?";

	const int xmmArg = Detour_PrologueSpillsXmmArg(reinterpret_cast<const unsigned char*>(pFn));
	if (xmmArg >= 0)
	{
		char adv[256];
		_snprintf_s(adv, _TRUNCATE,
			"[DETOUR] %s hooks %p which spills xmm%d in its prologue -- the hook must forward that float/double arg\n",
			pszClass, pFn, xmmArg);
		OutputDebugStringA(adv);
		if (g_DetourLogSink)
			g_DetourLogSink(adv);
	}
	const auto it = s_DetourTargets.emplace(pFn, DetourClaim_t{ pDetour, pszClass });

	if (it.second)
		return;

	// Two classes resolved the same engine function -- usually two byte patterns
	// that are each 1-hit unique but land on the same address. Detours points the
	// entry jmp at whichever attaches last, so the earlier hook keeps a valid
	// trampoline that nothing ever jumps to and goes silent with no other symptom.
	// Cost a week on CPlayerMove::RunCommand (VPlayerMove vs VBridgeFireTap).
	char buf[256];
	_snprintf_s(buf, _TRUNCATE,
		"[DETOUR] DUPLICATE hook on target %p: already claimed by %s (%p), now also %s (%p) -- "
		"one of them will be orphaned and never run\n",
		pFn, it.first->second.pszClass, it.first->second.pDetour, pszClass, pDetour);
	OutputDebugStringA(buf);

	if (g_DetourLogSink)
		g_DetourLogSink(buf);
}

//-----------------------------------------------------------------------------
// Purpose: adds a detour context to the list
//-----------------------------------------------------------------------------
std::size_t AddDetour(IDetour* const pDetour)
{
#ifdef _DEBUG
	const IDetour* const pVFTable = reinterpret_cast<IDetour**>(pDetour)[0];
	const auto p = s_DetourSet.insert(pVFTable); // Track duplicate registrations.

	assert(p.second); // Code bug: duplicate registration!!! (called 'REGISTER(...)' from a header file?).
#endif // _DEBUG
	g_DetourVec.push_back(pDetour);
	return g_DetourVec.size();
}
