//=============================================================================//
//
// Purpose: SkinNames null-slot stub. vt[10] returns 0xFFFF; vt[11] returns "".
//
//=============================================================================//
#include "core/stdafx.h"
#include "engine/client/net_bridge_addrs.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "skinnames_stub.h"
#include "rtech/pak/rpak_observe.h"
#include "game/shared/heap_canary.h"

#include <atomic>
#include <cstdint>
#include <cstring>


// vt[10] FindStringIndex returns 0xFFFF; vt[11] GetString returns "".
static int __fastcall StubFindStringIndex_S21(void* /*self*/,
											  const char* /*name*/)
{
	return 0xFFFF;  // INVALID_STRING_INDEX -- caller falls through to mdl scan
}

static const char* __fastcall StubGetString_S21(void* /*self*/,
												unsigned int /*idx*/)
{
	static const char kEmpty[1] = { 0 };
	return kEmpty;  // not-found sentinel
}

static int64_t __fastcall StubReturnZero_S21(void* /*self*/, ...)
{
	return 0;
}

static const char* __fastcall StubReturnEmpty_S21(void* /*self*/, ...)
{
	static const char kEmpty[1] = { 0 };
	return kEmpty;
}

// 16-slot vtable. Unused slots return 0.
static void* g_stubSkinTableVtable[16] = {
	/* 0 GetTableName */ reinterpret_cast<void*>(&StubReturnEmpty_S21),
	/* 1 GetTableId */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 2 GetNumStrings */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 3 GetMaxStrings */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 4 GetEntryBits */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 5 SetTick */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 6 ChangedSinceTick */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 7 AddString */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 8 GetString (legacy) */ reinterpret_cast<void*>(&StubGetString_S21),
	/* 9 GetStringUserData */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 10 FindStringIndex */ reinterpret_cast<void*>(&StubFindStringIndex_S21),
	/* 11 GetString (S21) */ reinterpret_cast<void*>(&StubGetString_S21),
	/* 12 */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 13 */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 14 */ reinterpret_cast<void*>(&StubReturnZero_S21),
	/* 15 */ reinterpret_cast<void*>(&StubReturnZero_S21),
};

// The "object" itself -- only the first qword (vtable pointer) is read.
struct StubSkinNamesTable
{
	void** vtable;
	// Zero-pad past the vtable so a field-offset read cannot land on adjacent globals.
	uint8_t padding[256];
};
static StubSkinNamesTable g_stubSkinTable = {
	g_stubSkinTableVtable,
	{}  // zero-initialized
};

// Resolved at GetFun time, used by attach + the reset-clear hook.
static uintptr_t* g_pSkinNamesSlot = nullptr;

// ClearAllStringTableHandles zeroes every cl_stringTable* global. Replant after it.
static void (*v_ClearStringTableHandles_S21)() = nullptr;

static void ClearStringTableHandles_S21_Hook()
{
	if (v_ClearStringTableHandles_S21)
		v_ClearStringTableHandles_S21();

	if (g_pSkinNamesSlot)
		*g_pSkinNamesSlot = reinterpret_cast<uintptr_t>(&g_stubSkinTable);
}

// InstallStringTableCallback can stamp NULL; restore the stub if the slot is 0.
static void (*v_InstallStringTableCallback_S21)(void* self,
												const char* name) = nullptr;

static void InstallStringTableCallback_S21_Hook(void* self, const char* name)
{
	// Slot state before the original so a NULL after is distinguishable.
	const uintptr_t stubAddr =
		reinterpret_cast<uintptr_t>(&g_stubSkinTable);
	const uintptr_t before  = g_pSkinNamesSlot ? *g_pSkinNamesSlot : 0;

	char phaseIn[64];
	char phaseOut[64];
	V_snprintf(phaseIn, sizeof(phaseIn), "stringtable-%s-entry",
		name ? name : "(null)");
	V_snprintf(phaseOut, sizeof(phaseOut), "stringtable-%s-return",
		name ? name : "(null)");
	HeapCanary::Checkpoint(phaseIn);

	if (v_InstallStringTableCallback_S21)
		v_InstallStringTableCallback_S21(self, name);

	HeapCanary::Checkpoint(phaseOut);

	const uintptr_t after = g_pSkinNamesSlot ? *g_pSkinNamesSlot : 0;

	// First 40 calls, plus any SkinNames-named call.
	static std::atomic<int> s_callCount{0};
	const int n = s_callCount.fetch_add(1) + 1;
	const bool isSkin = (name && strcmp(name, "SkinNames") == 0);
	if (n <= 40 || isSkin)
	{
		Msg(eDLL_T::ENGINE,
			"[s21-skinnames] dispatcher #%d name='%s' slot before=%p "
			"after=%p stub=%p%s\n",
			n, name ? name : "(null)",
			(void*)before, (void*)after, (void*)stubAddr,
			isSkin ? " <-- SKINNAMES" : "");
	}

	// Restore the stub if the dispatcher left the slot null.
	if (g_pSkinNamesSlot && *g_pSkinNamesSlot == 0)
		*g_pSkinNamesSlot = stubAddr;
}

// ---------------------------------------------------------------------------

void VSkinNamesStubS21::GetAdr() const
{
	LogFunAdr("ClearStringTableHandles_S21",
			  v_ClearStringTableHandles_S21);
	LogFunAdr("InstallStringTableCallback_S21",
			  v_InstallStringTableCallback_S21);
}

void VSkinNamesStubS21::GetFun() const
{
	// SkinNames slot is a data-section global at a fixed RVA.
	HMODULE hExe = GetModuleHandleA(NULL);
	if (hExe)
	{
		g_pSkinNamesSlot = reinterpret_cast<uintptr_t*>(
			NetObs_Sym(NetObsSym_t::SkinNamesSlot));
	}

	Module_FindPattern(g_GameDll,
		"33 C0 48 89 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
		"48 89 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
		"48 89 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
		"48 89 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
		"48 89 05 ?? ?? ?? ?? C3")
		.GetPtr(v_ClearStringTableHandles_S21);

	Module_FindPattern(g_GameDll,
		"48 83 EC 28 4C 8B DA 48 8D 15 ?? ?? ?? ?? "
		"49 8B CB E8 ?? ?? ?? ?? 85 C0 75 31")
		.GetPtr(v_InstallStringTableCallback_S21);
}

void VSkinNamesStubS21::Detour(const bool bAttach) const
{
	if (!g_pSkinNamesSlot)
	{
		Warning(eDLL_T::ENGINE,
			"[s21-skinnames] slot resolver failed;"
			"SkinNames null-slot stub NOT active.\n");
		return;
	}

	if (bAttach)
	{
		// Plant before other detours so the first SetSkinModByName sees a non-NULL handle.
		*g_pSkinNamesSlot = reinterpret_cast<uintptr_t>(&g_stubSkinTable);

		Msg(eDLL_T::ENGINE,
			"[s21-skinnames] Installed stub INetworkStringTable into "
			"(slot=%p stub=%p vt=%p). vt[10] returns"
			"0xFFFF -> SetSkinModByName falls through to mdl skin-scan.\n",
			(void*)g_pSkinNamesSlot, (void*)&g_stubSkinTable,
			(void*)&g_stubSkinTableVtable);
	}
	else
	{
		// Revert only if the slot still points at the stub.
		if (*g_pSkinNamesSlot ==
			reinterpret_cast<uintptr_t>(&g_stubSkinTable))
		{
			*g_pSkinNamesSlot = 0;
		}
	}

	if (v_ClearStringTableHandles_S21)
	{
		const LONG r = bAttach
			? DetourAttach(reinterpret_cast<void**>(
				&v_ClearStringTableHandles_S21),
				reinterpret_cast<void*>(&ClearStringTableHandles_S21_Hook))
			: DetourDetach(reinterpret_cast<void**>(
				&v_ClearStringTableHandles_S21),
				reinterpret_cast<void*>(&ClearStringTableHandles_S21_Hook));
		if (bAttach)
		{
			Msg(eDLL_T::ENGINE,
				"[s21-skinnames] DetourAttach ClearStringTableHandles "
				"result=0x%lX (target=%p) -- stub will reinstall after "
				"every disconnect.\n", r,
				(void*)v_ClearStringTableHandles_S21);
		}
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"[s21-skinnames] pattern unresolved -- stub will"
			"NOT auto-reinstall after disconnect; first lobby return after "
			"leaving a match could re-AV.\n");
	}

	if (v_InstallStringTableCallback_S21)
	{
		const LONG r = bAttach
			? DetourAttach(reinterpret_cast<void**>(
				&v_InstallStringTableCallback_S21),
				reinterpret_cast<void*>(&InstallStringTableCallback_S21_Hook))
			: DetourDetach(reinterpret_cast<void**>(
				&v_InstallStringTableCallback_S21),
				reinterpret_cast<void*>(&InstallStringTableCallback_S21_Hook));
		if (bAttach)
		{
			Msg(eDLL_T::ENGINE,
				"[s21-skinnames] DetourAttach InstallStringTableCallback "
				"result=0x%lX (target=%p) -- stub restored if dispatcher "
				"ever stamps NULL into the slot.\n", r,
				(void*)v_InstallStringTableCallback_S21);
		}
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"[s21-skinnames] pattern unresolved -- post-"
			"dispatcher NULL guard NOT active; if the dedi sends "
			"SVC_CreateStringTable for a non-SkinNames table while our slot "
			"happens to be cleared, the lobby could re-AV until next reset.\n");
	}

	if (bAttach)
		HeapCanary::Checkpoint("skinnames-stub-installed");
}
