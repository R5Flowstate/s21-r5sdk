//=============================================================================//
//
// Purpose: Inject SkinNames on the dedi so the S21 client has a non-null slot.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/module.h"
#include "public/tier0/memaddr.h"
#include "thirdparty/detours/include/detours.h"
#include "skinnames_table_inject.h"
#include "precache_natives.h"
#include "game/server/gameinterface.h"

#include <atomic>
#include <cstdint>

// CServerGameDLL::CreateNetworkStringTables -- CreateStringTable is vtable[1],
// 7-arg: container, allowCreation, name, maxEntries, userDataMaxSize,
// userDataNetworkBits, allowClientSideAddString.
using CreateStringTable_t = void* (__fastcall*)(
	void* container,
	char allowCreation,
	const char* name,
	int maxEntries,
	int userDataMaxSize,
	int userDataNetworkBits,
	char allowClientSideAddString);

static int64_t (*v_CServerGameDLL_CreateNetworkStringTables)(
	int64_t a1, int64_t a2) = nullptr;

static std::atomic<bool> s_skinNamesInjected{false};

// Stringtable container singleton; live by the time we tail-inject.
static void InjectSkinNamesTable()
{
	if (s_skinNamesInjected.exchange(true)) return;

	__try
	{
		const uintptr_t base =
			static_cast<uintptr_t>(g_GameDll.GetModuleBase());
		void** const containerSlot =
			reinterpret_cast<void**>(base + 0xD4ECBA0);
		void* const container = *containerSlot;
		if (!container)
		{
			s_skinNamesInjected.store(false, std::memory_order_release);
			Warning(eDLL_T::SERVER,
				"[s21-bridge] SkinNames inject: server stringtable "
				"container is NULL @ base+0xD4ECBA0 -- bailing.\n");
			return;
		}

		void** const vtable = *reinterpret_cast<void***>(container);
		const CreateStringTable_t CreateStringTable =
			reinterpret_cast<CreateStringTable_t>(vtable[1]);

		// 1024 entries; 0/0/1 matches Materials/WeaponNames/ScriptNames.
		void* const skinTable = CreateStringTable(
			container,
			/*allowCreation*/        1,
			"SkinNames",
			/*maxEntries*/           1024,
			/*userDataMaxSize*/      0,
			/*userDataNetworkBits*/  0,
			/*allowClientSideAddStr*/1);

		if (skinTable)
		{
			Msg(eDLL_T::SERVER,
				"[s21-bridge] SkinNames table injected (table=%p, "
				"container=%p) -- S21 client SetSkinModByName path "
				"will now resolve.\n",
				skinTable, container);

			// Populate the launch-set roster so the table replicates on signon.
			PrecacheSkinName_PopulateAll();
		}
		else
		{
			s_skinNamesInjected.store(false, std::memory_order_release);
			Warning(eDLL_T::SERVER,
				"[s21-bridge] SkinNames inject: container->CreateStringTable "
				"returned NULL (container=%p)\n", container);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		s_skinNamesInjected.store(false, std::memory_order_release);
		Warning(eDLL_T::SERVER,
			"[s21-bridge] SkinNames inject: SEH fault during create.\n");
	}
}

static int64_t Hook_CServerGameDLL_CreateNetworkStringTables(int64_t a1, int64_t a2)
{
	// Engine already destroyed the previous level's tables. Run any SDK
	// per-level reset a transition path skipped before we cache new pointers.
	ServerGameDLL_OnLevelStringTablesCreated();

	const int64_t result = v_CServerGameDLL_CreateNetworkStringTables(a1, a2);
	InjectSkinNamesTable();
	return result;
}

void VSkinNamesTableInject::GetAdr() const
{
	LogFunAdr("CServerGameDLL::CreateNetworkStringTables",
		v_CServerGameDLL_CreateNetworkStringTables);
}

void VSkinNamesTableInject::GetFun() const
{
	// Unique prologue: ExtraParticleFilesTable rdata pointer.
	Module_FindPattern(g_GameDll,
		"48 89 74 24 10 57 48 83 EC 40 48 8B 0D ?? ?? ?? ?? "
		"4C 8D 05 ?? ?? ?? ?? 33 F6 C7 44 24 30 01 00 00 00 "
		"89 74 24 28 41 B9 00 08 00 00 B2 01")
		.GetPtr(v_CServerGameDLL_CreateNetworkStringTables);
}

//-----------------------------------------------------------------------------
// Drop the inject guard so the next map's CreateNetworkStringTables re-creates SkinNames.
//-----------------------------------------------------------------------------
void SkinNamesInject_LevelShutdown()
{
	s_skinNamesInjected.store(false, std::memory_order_release);
	Msg(eDLL_T::SERVER,
		"[s21-bridge] SkinNames LevelShutdown: inject guard cleared -- "
		"next CreateNetworkStringTables will re-inject + repopulate\n");
}

void VSkinNamesTableInject::Detour(const bool bAttach) const
{
	if (!v_CServerGameDLL_CreateNetworkStringTables)
	{
		Warning(eDLL_T::SERVER,
			"[s21-bridge] CServerGameDLL::CreateNetworkStringTables "
			"pattern unresolved -- SkinNames table NOT injected. "
			"S21 client SetSkinModByName will AV on customize click.\n");
		return;
	}
	const LONG result = bAttach
		? DetourAttach(reinterpret_cast<void**>(
			&v_CServerGameDLL_CreateNetworkStringTables),
			reinterpret_cast<void*>(
				&Hook_CServerGameDLL_CreateNetworkStringTables))
		: DetourDetach(reinterpret_cast<void**>(
			&v_CServerGameDLL_CreateNetworkStringTables),
			reinterpret_cast<void*>(
				&Hook_CServerGameDLL_CreateNetworkStringTables));
	if (bAttach)
	{
		Msg(eDLL_T::SERVER,
			"[s21-bridge] DetourAttach CServerGameDLL::CreateNetworkStringTables "
			"result=0x%lX (target=0x%p)\n",
			result, (void*)v_CServerGameDLL_CreateNetworkStringTables);
	}
}
