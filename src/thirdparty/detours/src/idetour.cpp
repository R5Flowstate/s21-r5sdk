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

// Target address -> every detour that claimed it, in attach order, for the attach pass.
struct DetourClaim_t
{
	void* pDetour;
	void** ppPointer;
	void* pTrampoline;
	const char* pszClass;
};
static std::unordered_map<void*, std::vector<DetourClaim_t>> s_DetourTargets;
static std::vector<void*> s_DetourTargetOrder;
static bool s_bDetourChainsLinked = false;

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
	s_DetourTargetOrder.clear();
	s_bDetourChainsLinked = false;
}

static void Detour_Log(const char* const pszMsg)
{
	OutputDebugStringA(pszMsg);
	if (g_DetourLogSink)
		g_DetourLogSink(pszMsg);
}

// Follows the committed entry jmp (E9 rel32 into the trampoline, then FF 25 through
// its detour slot) to the hook that now receives the engine's calls.
static void* Detour_ResolveEntryHook(const unsigned char* p)
{
	for (int nHop = 0; nHop < 4 && p; ++nHop)
	{
		if (p[0] == 0xE9)
		{
			p = p + 5 + *reinterpret_cast<const int32_t*>(p + 1);
			continue;
		}
		if (p[0] == 0xFF && p[1] == 0x25)
			return *reinterpret_cast<void* const*>(p + 6 + *reinterpret_cast<const int32_t*>(p + 2));
		break;
	}
	return nullptr;
}

//-----------------------------------------------------------------------------
// One transaction can only give a target one entry jmp, so every other hook on it
// would never run. After the commit, link them: the entry hook's original pointer
// is aimed at the next claimant, and so on, the last one keeping its trampoline.
//-----------------------------------------------------------------------------
void Detour_LinkSharedTargets(void)
{
	for (void* const pTarget : s_DetourTargetOrder)
	{
		std::vector<DetourClaim_t>& claims = s_DetourTargets[pTarget];
		if (claims.size() < 2)
			continue;

		void* const pEntry = Detour_ResolveEntryHook(reinterpret_cast<const unsigned char*>(pTarget));
		size_t nEntry = claims.size();
		for (size_t i = 0; i < claims.size(); ++i)
		{
			claims[i].pTrampoline = *claims[i].ppPointer;
			if (claims[i].pDetour == pEntry)
				nEntry = i;
		}

		char buf[512];
		if (nEntry == claims.size())
		{
			_snprintf_s(buf, _TRUNCATE,
				"[DETOUR] shared target %p: entry jmp resolves to %p, no claimant matches -- %zu hooks left unlinked\n",
				pTarget, pEntry, claims.size() - 1);
			Detour_Log(buf);
			continue;
		}

		// Entry claimant first, then the rest in attach order.
		std::vector<size_t> order;
		order.push_back(nEntry);
		for (size_t i = 0; i < claims.size(); ++i)
			if (i != nEntry)
				order.push_back(i);

		for (size_t k = 0; k + 1 < order.size(); ++k)
			*claims[order[k]].ppPointer = claims[order[k + 1]].pDetour;

		int nLen = _snprintf_s(buf, _TRUNCATE, "[DETOUR] shared target %p linked:", pTarget);
		for (const size_t i : order)
			if (nLen > 0 && static_cast<size_t>(nLen) < sizeof(buf))
				nLen += _snprintf_s(buf + nLen, sizeof(buf) - nLen, _TRUNCATE, " %s ->", claims[i].pszClass);
		strncat_s(buf, " original\n", _TRUNCATE);
		Detour_Log(buf);
	}
	s_bDetourChainsLinked = true;
}

// Restores each linked hook's own trampoline so DetourDetach finds what it attached.
void Detour_UnlinkSharedTargets(void)
{
	if (!s_bDetourChainsLinked)
		return;
	for (void* const pTarget : s_DetourTargetOrder)
		for (DetourClaim_t& claim : s_DetourTargets[pTarget])
			if (claim.pTrampoline)
				*claim.ppPointer = claim.pTrampoline;
	s_bDetourChainsLinked = false;
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

void Detour_NoteAttachTarget(void* pFn, void* pDetour, void** ppPointer)
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
	std::vector<DetourClaim_t>& claims = s_DetourTargets[pFn];
	if (claims.empty())
		s_DetourTargetOrder.push_back(pFn);
	for (const DetourClaim_t& claim : claims)
		if (claim.pDetour == pDetour)
			return; // the same hook twice would link to itself
	claims.push_back(DetourClaim_t{ pDetour, ppPointer, nullptr, pszClass });

	if (claims.size() == 1)
		return;

	// Within one transaction the target gets a single entry jmp (the first attach
	// wins; pending operations commit newest first), so the other hooks never run
	// unless Detour_LinkSharedTargets runs after the commit.
	char buf[256];
	_snprintf_s(buf, _TRUNCATE,
		"[DETOUR] shared target %p: claimed by %s (%p), now also %s (%p) -- only runs if linked after commit\n",
		pFn, claims.front().pszClass, claims.front().pDetour, pszClass, pDetour);
	Detour_Log(buf);
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
