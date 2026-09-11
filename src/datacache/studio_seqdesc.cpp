//=============================================================================//
//
// Purpose: The virtual-model sequence binder walks a counted wrapper array
//          and reads mstudioseqdesc_v16_t::numautolayers at +0x4E off the
//          first pointer in each wrapper. A live wrapper whose first pointer
//          is NULL AVs there. Point those slots at a zeroed dummy for the
//          original walk (numautolayers == 0 skips the autolayer loop), then
//          restore the wrappers and scrub any dummy pointer the binder stored
//          into the output table (NULL is already the engine's empty slot).
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/cvar.h"
#include "studio_seqdesc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static ConVar sdk_seqdesc_guard("sdk_seqdesc_guard", "1", FCVAR_RELEASE,
	"Sanitize NULL virtual-model seqdesc wrappers before the binder walk. 0 = pass-through.");

typedef __int16(__fastcall* PFN_BindVirtualSeqs)(__int64, __int16, __int64);
static PFN_BindVirtualSeqs v_BindVirtualSeqs = nullptr;

// Zeroed mstudioseqdesc_v16_t stand-in; +0x4E (numautolayers) stays 0.
static alignas(16) unsigned char s_dummySeqdesc[0x80];

static int s_logBudget = 32;

static constexpr int kMaxSeqWrappers = 4096;

static int PatchNullWrappers(__int64* const wrappers, const int nCount)
{
	int patched = 0;
	for (int i = 0; i < nCount; ++i)
	{
		__int64* const wrapper = reinterpret_cast<__int64*>(wrappers[i]);
		if (!wrapper)
			continue;
		if (*wrapper == 0)
		{
			*wrapper = reinterpret_cast<__int64>(s_dummySeqdesc);
			++patched;
		}
	}
	return patched;
}

static void RestoreDummyWrappers(__int64* const wrappers, const int nCount)
{
	const __int64 dummy = reinterpret_cast<__int64>(s_dummySeqdesc);
	for (int i = 0; i < nCount; ++i)
	{
		__int64* const wrapper = reinterpret_cast<__int64*>(wrappers[i]);
		if (wrapper && *wrapper == dummy)
			*wrapper = 0;
	}
}

static void ScrubDummyOutputSlots(__int64 a1)
{
	if (!a1)
		return;

	const __int16 nOut = *reinterpret_cast<const __int16*>(a1 + 2);
	const __int64 table = *reinterpret_cast<const __int64*>(a1 + 16);
	if (!table || nOut <= 0)
		return;

	const int n = nOut > kMaxSeqWrappers ? kMaxSeqWrappers : nOut;
	const __int64 dummy = reinterpret_cast<__int64>(s_dummySeqdesc);
	for (int i = 0; i < n; ++i)
	{
		__int64* const slot = reinterpret_cast<__int64*>(table + 16ll * i + 8);
		if (*slot == dummy)
			*slot = 0;
	}
}

static __int16 __fastcall Hook_BindVirtualSeqs(__int64 a1, __int16 a2, __int64 a3)
{
	if (!v_BindVirtualSeqs)
		return 0;

	if (!sdk_seqdesc_guard.GetBool() || !a3)
		return v_BindVirtualSeqs(a1, a2, a3);

	const __int16 nSeqs = *reinterpret_cast<const __int16*>(a3 + 2);
	__int64* wrappers = *reinterpret_cast<__int64**>(a3 + 8);
	if (nSeqs <= 0 || !wrappers)
		return v_BindVirtualSeqs(a1, a2, a3);

	const int n = nSeqs > kMaxSeqWrappers ? kMaxSeqWrappers : nSeqs;
	int patched = 0;
	__try
	{
		patched = PatchNullWrappers(wrappers, n);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return v_BindVirtualSeqs(a1, a2, a3);
	}

	if (patched > 0 && s_logBudget > 0)
	{
		--s_logBudget;
		Warning(eDLL_T::CLIENT,
			"[SEQDESC-GUARD] patched %d NULL seqdesc wrapper(s) count=%d\n",
			patched, n);
	}

	const __int16 result = v_BindVirtualSeqs(a1, a2, a3);

	__try
	{
		RestoreDummyWrappers(wrappers, n);
		if (patched > 0)
			ScrubDummyOutputSlots(a1);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}

	return result;
}

void VStudioSeqdescGuardS21::GetAdr(void) const
{
	LogFunAdr("BindVirtualSeqs", v_BindVirtualSeqs);
}

void VStudioSeqdescGuardS21::GetFun(void) const
{
	// Unique: mov [rsp+10h],dx / push rbx / push rsi
	Module_FindPattern(g_GameDll,
		"66 89 54 24 ?? 53 56")
		.GetPtr(v_BindVirtualSeqs);

	if (!v_BindVirtualSeqs)
		Warning(eDLL_T::CLIENT,
			"[SEQDESC-GUARD] pattern UNRESOLVED -- guard NOT installed\n");
}

void VStudioSeqdescGuardS21::Detour(const bool bAttach) const
{
	if (v_BindVirtualSeqs)
		DetourSetup(&v_BindVirtualSeqs, &Hook_BindVirtualSeqs, bAttach);
}
