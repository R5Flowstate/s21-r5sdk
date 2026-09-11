#if defined(CLIENT_DLL)
//=============================================================================
//
// Purpose: NonRewind entity lifecycle + var-slot lookup.
//
//=============================================================================

#include "core/stdafx.h"
#include "scriptnetdata_ext.h"

// Hash lookup for NonRewind natives. Returns -1 if g_pServerNetVars is unresolved.
static constexpr int SERVER_NETVAR_ENTRY_SIZE = 0x38;
static constexpr int NETVAR_CATEGORY_OFFSET = 0x08;

static uint8_t* g_pServerNetVars = nullptr;

static uint64_t HashVarName64(const char* name)
{
	size_t l = strlen(name);
	uint64_t h = static_cast<uint64_t>(l);
	size_t step = (l >> 5) + 1;
	for (size_t l1 = l; l1 >= step; l1 -= step)
		h = h ^ ((h << 5) + (h >> 2) + static_cast<uint8_t>(name[l1 - 1]));
	return h;
}

int ScriptNetData_FindVarSlot(const char* name, int expectedCategory)
{
	if (!g_pServerNetVars)
		return -1;

	static constexpr int BUCKET_COUNT = 250;

	uint64_t hash = HashVarName64(name);
	uint64_t startBucket = hash % BUCKET_COUNT;

	for (int probe = 0; probe < BUCKET_COUNT; probe++)
	{
		uint64_t idx = (startBucket + probe) % BUCKET_COUNT;
		uintptr_t entry = reinterpret_cast<uintptr_t>(g_pServerNetVars)
			+ SERVER_NETVAR_ENTRY_SIZE * idx;

		uint64_t storedHash = *reinterpret_cast<uint64_t*>(entry);
		if (storedHash == 0)
			return -1;

		if (storedHash == hash)
		{
			int category = *reinterpret_cast<int*>(entry + NETVAR_CATEGORY_OFFSET);
			if (category == expectedCategory)
				return *reinterpret_cast<int*>(entry + 0x10);
		}
	}

	return -1;
}

//------------------------------------------------------------------------------
// Lifecycle: clear NonRewind entity pointer on map shutdown
//------------------------------------------------------------------------------
void ScriptNetDataExt_LevelShutdown()
{
	if (g_pScriptNetDataNonRewindEnt)
	{
		Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] NonRewind entity cleared on level shutdown\n");
		g_pScriptNetDataNonRewindEnt = nullptr;
	}
}
#else // !CLIENT_DLL
//=============================================================================
//
// Purpose: SNDC_GLOBAL_NON_REWIND is category 5. Engine bounds patched to allow <=5.
//
//=============================================================================

#include "core/stdafx.h"
#include "scriptnetdata_ext.h"
#include "game/client/scriptnetdata_client.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/sqstring.h"

//------------------------------------------------------------------------------
// Constants
//------------------------------------------------------------------------------
static constexpr int INTERNAL_TYPE_COUNT = 5;
static constexpr int SERVER_NETVAR_ENTRY_SIZE = 0x38;  // 56 bytes per server scriptNetVars entry
static constexpr int CLIENT_NETVAR_ENTRY_SIZE = 0x20;  // 32 bytes per client scriptNetVars entry
static constexpr int NETVAR_CATEGORY_OFFSET = 0x08;    // category field at +8 in both

// Default slot limits for SNDC_GLOBAL_NON_REWIND (matches SNDC_GLOBAL)
static constexpr int NON_REWIND_LIMITS[INTERNAL_TYPE_COUNT] = { 16, 32, 8, 24, 16 };

//------------------------------------------------------------------------------
// State
//------------------------------------------------------------------------------
static uint8_t* s_pOrigServerCategories = nullptr;
static uint8_t* s_pOrigClientCategories = nullptr;

// Entity pointers for SNDC_GLOBAL_NON_REWIND
// g_pScriptNetDataNonRewindEnt is the externally-visible pointer (declared in header)
static void* s_pNonRewindClientEnt = nullptr;

// Engine global entity pointers (we read/swap these)
static void** g_ppServerGlobalEnt = nullptr;   // qword_16859E480
static void** g_ppClientGlobalEnt = nullptr;   // qword_1695075E8

// Engine scriptNetVars hash tables (for category lookup)
static uint8_t* g_pServerNetVars = nullptr;    // unk_1685A01A0
static uint8_t* g_pClientNetVars = nullptr;    // unk_169509820

// Hook targets
static char (*v_ServerFindGlobalNetVar)(__int64 a1, int* a2) = nullptr;
static char (*v_ClientFindGlobalNetVar)(__int64 a1, int* a2) = nullptr;

// Entity constructor (server — creates CScriptNetDataGlobal)
static void* (*v_ServerCreateScriptNetDataGlobal)(__int64 a1, __int64 a2) = nullptr;

// Engine's TriggerChangeCallbacks — fires registered callbacks on a ScriptNetData entity
// S3 RVA 0x88A9C0: checks dirty flag at entity+3072, iterates category's varsWithCallbacks,
// reads current values from server category struct, fires callback SQObjects.
static void (*v_TriggerChangeCallbacks)(void* pEntity) = nullptr;

// Engine-side handlers for RegisterNetworkedVariableChangeCallback_*.
using NetVarCallbackNative_t = SQRESULT (*)(HSQUIRRELVM);
static NetVarCallbackNative_t v_Engine_RegisterNetVarCallback_bool  = nullptr;
static NetVarCallbackNative_t v_Engine_RegisterNetVarCallback_int   = nullptr;
static NetVarCallbackNative_t v_Engine_RegisterNetVarCallback_float = nullptr;
static NetVarCallbackNative_t v_Engine_RegisterNetVarCallback_time  = nullptr;
static NetVarCallbackNative_t v_Engine_RegisterNetVarCallback_ent   = nullptr;

static SQRESULT Hook_RegisterNetVarCallback_bool(HSQUIRRELVM v)  { return ScriptNetData_HandleCallbackRegistration(v, SNVT_BOOL); }
static SQRESULT Hook_RegisterNetVarCallback_int(HSQUIRRELVM v)   { return ScriptNetData_HandleCallbackRegistration(v, SNVT_INT); }
static SQRESULT Hook_RegisterNetVarCallback_float(HSQUIRRELVM v) { return ScriptNetData_HandleCallbackRegistration(v, SNVT_FLOAT_RANGE); }
static SQRESULT Hook_RegisterNetVarCallback_time(HSQUIRRELVM v)  { return ScriptNetData_HandleCallbackRegistration(v, SNVT_TIME); }
static SQRESULT Hook_RegisterNetVarCallback_ent(HSQUIRRELVM v)   { return ScriptNetData_HandleCallbackRegistration(v, SNVT_ENTITY); }



// FindVarSlot: hash-table slot is a category default index, not an entity array position.
static constexpr int NETVAR_TYPE_OFFSET = 0x0C;
static constexpr int NETVAR_SLOT_OFFSET = 0x10;

int ScriptNetData_FindVarSlotAndType(const char* name, int expectedCategory, int* outType)
{
	// The dedi uses the CLIENT RegisterNetworkedVariable chain, so vars are
	// stored in the CLIENT hash table (unk_169509820, 32-byte entries), NOT the
	// SERVER one (unk_1685A01A0, 56-byte entries). Use g_pClientNetVars.
	if (!g_pClientNetVars)
	{
		Warning(eDLL_T::ENGINE, "[SNDC-FIND] FAIL: g_pClientNetVars is NULL\n");
		return -1;
	}

	// Get hash from server VM
	HSQUIRRELVM vm = nullptr;
	if (g_pServerScript) vm = g_pServerScript->GetVM();
	else if (g_pClientScript) vm = g_pClientScript->GetVM();
	if (!vm || !vm->_sharedstate || !vm->_sharedstate->_stringtable || !v_StringTable__Add)
	{
		Warning(eDLL_T::ENGINE, "[SNDC-FIND] FAIL: VM/stringtable/StringTable__Add null (vm=%p)\n", (void*)vm);
		return -1;
	}

	SQString* str = v_StringTable__Add(
		vm->_sharedstate->_stringtable, name, static_cast<SQInteger>(strlen(name)));
	if (!str)
	{
		Warning(eDLL_T::ENGINE, "[SNDC-FIND] FAIL: StringTable__Add('%s') returned null\n", name);
		return -1;
	}
	uint64_t hash = str->_hash;

	static constexpr int BUCKET_COUNT = 250;
	uint64_t startBucket = hash % BUCKET_COUNT;
	for (int probe = 0; probe < BUCKET_COUNT; probe++)
	{
		uint64_t idx = (startBucket + probe) % BUCKET_COUNT;
		uintptr_t entry = reinterpret_cast<uintptr_t>(g_pClientNetVars)
			+ CLIENT_NETVAR_ENTRY_SIZE * idx;
		uint64_t storedHash = *reinterpret_cast<uint64_t*>(entry);
		if (storedHash == 0)
		{
			if (probe == 0)
				/* silent -- first-probe miss is normal for unregistered vars */
			return -1;
		}
		if (storedHash == hash)
		{
			int category = *reinterpret_cast<int*>(entry + NETVAR_CATEGORY_OFFSET);
			if (category == expectedCategory)
			{
				int slot = *reinterpret_cast<int*>(entry + NETVAR_SLOT_OFFSET);
				if (outType)
					*outType = *reinterpret_cast<int*>(entry + NETVAR_TYPE_OFFSET);
				return slot;
			}
			else
			{
				/* hash collision -- continue probing */
			}
		}
	}
	/* exhausted all probes -- var not registered */
	return -1;
}

int ScriptNetData_FindVarSlot(const char* name, int expectedCategory)
{
	return ScriptNetData_FindVarSlotAndType(name, expectedCategory, nullptr);
}

// TriggerGlobalChangeCallbacks: fire all registered netvar change callbacks for the GLOBAL entity.
void ScriptNetDataExt_TriggerGlobalChangeCallbacks()
{
	if (!v_TriggerChangeCallbacks)
	{
		Warning(eDLL_T::ENGINE, "[SNDC] TriggerGlobalChangeCallbacks: engine function not resolved\n");
		return;
	}

	// Fire on the server GLOBAL entity (category 0)
	void* pServerEnt = g_ppServerGlobalEnt ? *g_ppServerGlobalEnt : nullptr;
	if (pServerEnt)
	{
		// Force dirty flag at entity+0xC00 (3072) so the engine processes callbacks
		static constexpr int DIRTY_FLAG_OFFSET = 0xC00;
		reinterpret_cast<uint8_t*>(pServerEnt)[DIRTY_FLAG_OFFSET] = 1;

		v_TriggerChangeCallbacks(pServerEnt);
		DevMsg(eDLL_T::ENGINE, "[SNDC] TriggerGlobalChangeCallbacks: fired for GLOBAL entity\n");
	}
	else
	{
		Warning(eDLL_T::ENGINE, "[SNDC] TriggerGlobalChangeCallbacks: no server global entity\n");
	}
}

//------------------------------------------------------------------------------
// Lifecycle: clear NonRewind entity on map shutdown
//------------------------------------------------------------------------------
void ScriptNetDataExt_LevelShutdown()
{
	if (g_pScriptNetDataNonRewindEnt)
	{
		Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] NonRewind entity cleared on level shutdown\n");
		g_pScriptNetDataNonRewindEnt = nullptr;
	}
	s_pNonRewindClientEnt = nullptr;
}

// Zero changeCallback SQObjects so a destroyed VM cannot crash the next dispatch.
void ScriptNetDataExt_ClearEngineCallbacks()
{
	if (!g_pServerNetVars)
		return;

	constexpr int ENTRY_STRIDE = SERVER_NETVAR_ENTRY_SIZE; // 0x38 (56 bytes)
	constexpr int CB_OFFSET    = 0x20;                     // SQObject (_type, _unVal)
	// (unk_1685A3870 - unk_1685A01C0) / 0x38 = 0x36B0 / 56 = 250 entries.
	// Matches the 250-bucket hash table in ScriptNetData_FindVarSlot.
	constexpr int ENTRY_COUNT  = 250;

	// Wrap in SEH -- if the memory layout assumption is wrong on some build or
	// the segment is not writable at this moment, we'd rather bail than crash.
	int cleared = 0;
	__try
	{
		for (int i = 0; i < ENTRY_COUNT; ++i)
		{
			uint8_t* cb = g_pServerNetVars + i * ENTRY_STRIDE + CB_OFFSET;
			uint32_t* pType = reinterpret_cast<uint32_t*>(cb);
			if (*pType == 0)
				continue;
			*pType = 0;                                        // OT_NULL
			*reinterpret_cast<uint64_t*>(cb + 8) = 0;          // _unVal
			++cleared;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Warning(eDLL_T::ENGINE,
			"[SNDC] ClearEngineCallbacks: caught SEH exception (cleared=%d before fault)\n",
			cleared);
		return;
	}

	DevMsg(eDLL_T::ENGINE,
		"[SNDC] ClearEngineCallbacks: zeroed %d stale engine-storage callback(s)\n",
		cleared);
}

//------------------------------------------------------------------------------
// IDetour implementation
//------------------------------------------------------------------------------
void VScriptNetDataExt::GetFun(void) const
{
	uintptr_t base = g_GameDll.GetModuleBase();

	// Server FindGlobalNetVar
	v_ServerFindGlobalNetVar = reinterpret_cast<decltype(v_ServerFindGlobalNetVar)>(base + 0x88ADF0);

	// Client FindGlobalNetVar
	v_ClientFindGlobalNetVar = reinterpret_cast<decltype(v_ClientFindGlobalNetVar)>(base + 0xC197B0);

	// Server CScriptNetDataGlobal constructor
	v_ServerCreateScriptNetDataGlobal = reinterpret_cast<decltype(v_ServerCreateScriptNetDataGlobal)>(base + 0x88AD00);

	// Validate by checking first bytes
	auto checkByte = [](void* fn, uint8_t expected) -> bool {
		return fn && *reinterpret_cast<uint8_t*>(fn) == expected;
	};

	if (!checkByte(v_ServerFindGlobalNetVar, 0x48))
	{
		Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] ServerFindGlobalNetVar validation FAILED\n");
		v_ServerFindGlobalNetVar = nullptr;
	}
	if (!checkByte(v_ClientFindGlobalNetVar, 0x48))
	{
		Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] ClientFindGlobalNetVar validation FAILED\n");
		v_ClientFindGlobalNetVar = nullptr;
	}
	if (!checkByte(v_ServerCreateScriptNetDataGlobal, 0x48))
	{
		Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] ServerCreateGlobalEnt validation FAILED\n");
		v_ServerCreateScriptNetDataGlobal = nullptr;
	}

	// Server TriggerChangeCallbacks
	v_TriggerChangeCallbacks = reinterpret_cast<decltype(v_TriggerChangeCallbacks)>(base + 0x88A9C0);
	if (!checkByte(v_TriggerChangeCallbacks, 0x40))
	{
		Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] TriggerChangeCallbacks validation FAILED\n");
		v_TriggerChangeCallbacks = nullptr;
	}

	// Engine-native RegisterNetworkedVariableChangeCallback_* handlers.
	v_Engine_RegisterNetVarCallback_bool  = reinterpret_cast<NetVarCallbackNative_t>(base + 0x8EBEC0);
	v_Engine_RegisterNetVarCallback_int   = reinterpret_cast<NetVarCallbackNative_t>(base + 0x8EC180);
	v_Engine_RegisterNetVarCallback_float = reinterpret_cast<NetVarCallbackNative_t>(base + 0x8EC440);
	v_Engine_RegisterNetVarCallback_time  = reinterpret_cast<NetVarCallbackNative_t>(base + 0x8EC700);
	v_Engine_RegisterNetVarCallback_ent   = reinterpret_cast<NetVarCallbackNative_t>(base + 0x8EC9C0);
	auto validateCb = [&](NetVarCallbackNative_t& fn, const char* name) {
		if (!checkByte(reinterpret_cast<void*>(fn), 0x48) && !checkByte(reinterpret_cast<void*>(fn), 0x40))
		{
			Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] %s validation FAILED -- detour skipped\n", name);
			fn = nullptr;
		}
	};
	validateCb(v_Engine_RegisterNetVarCallback_bool,  "RegisterNetVarCallback_bool");
	validateCb(v_Engine_RegisterNetVarCallback_int,   "RegisterNetVarCallback_int");
	validateCb(v_Engine_RegisterNetVarCallback_float, "RegisterNetVarCallback_float");
	validateCb(v_Engine_RegisterNetVarCallback_time,  "RegisterNetVarCallback_time");
	validateCb(v_Engine_RegisterNetVarCallback_ent,   "RegisterNetVarCallback_ent");
}

void VScriptNetDataExt::GetVar(void) const
{
	// Server scriptNetCategories
	CMemory srvPat = Module_FindPattern(g_GameDll,
		"33 D2 48 8D 0D ?? ?? ?? ?? 41 B8 3C 0A 00 00");
	if (srvPat)
		s_pOrigServerCategories = srvPat.Offset(0x2).ResolveRelativeAddress(0x3, 0x7).RCast<uint8_t*>();

	// Client scriptNetCategories
	CMemory clPat = Module_FindPattern(g_GameDll,
		"48 69 F8 08 01 00 00 4D 33 D3 48 8D 05");
	if (clPat)
		s_pOrigClientCategories = clPat.Offset(0xA).ResolveRelativeAddress(0x3, 0x7).RCast<uint8_t*>();

	// Direct RVA resolution for entity pointers and scriptNetVars
	uintptr_t base = g_GameDll.GetModuleBase();

	// Server global entity pointer: qword_16859E480
	g_ppServerGlobalEnt = reinterpret_cast<void**>(base + 0x2859E480);

	// Client global entity pointer: qword_1695075E8
	g_ppClientGlobalEnt = reinterpret_cast<void**>(base + 0x295075E8);

	// Server scriptNetVars: unk_1685A01A0
	g_pServerNetVars = reinterpret_cast<uint8_t*>(base + 0x285A01A0);

	// Client scriptNetVars: unk_169509820
	g_pClientNetVars = reinterpret_cast<uint8_t*>(base + 0x29509820);

	Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] ServerCategories=0x%p ClientCategories=0x%p\n",
		s_pOrigServerCategories, s_pOrigClientCategories);
	Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] ServerGlobalEnt=0x%p ClientGlobalEnt=0x%p\n",
		g_ppServerGlobalEnt, g_ppClientGlobalEnt);
	Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] ServerNetVars=0x%p ClientNetVars=0x%p\n",
		g_pServerNetVars, g_pClientNetVars);
	Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] FindGlobalNetVar: server=0x%p client=0x%p\n",
		v_ServerFindGlobalNetVar, v_ClientFindGlobalNetVar);
	Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] CreateGlobalEnt: server=0x%p\n",
		v_ServerCreateScriptNetDataGlobal);
}

void VScriptNetDataExt::Detour(const bool bAttach) const
{
	(void)g_GameDll; // suppress unused warnings when all patches disabled

	// Category-array expansion disabled. NON_REWIND vars register as SNDC_GLOBAL; expanding unpopulated copies garbage limits.
	if (v_Engine_RegisterNetVarCallback_bool)
		DetourSetup(&v_Engine_RegisterNetVarCallback_bool,  &Hook_RegisterNetVarCallback_bool,  bAttach);
	if (v_Engine_RegisterNetVarCallback_int)
		DetourSetup(&v_Engine_RegisterNetVarCallback_int,   &Hook_RegisterNetVarCallback_int,   bAttach);
	if (v_Engine_RegisterNetVarCallback_float)
		DetourSetup(&v_Engine_RegisterNetVarCallback_float, &Hook_RegisterNetVarCallback_float, bAttach);
	if (v_Engine_RegisterNetVarCallback_time)
		DetourSetup(&v_Engine_RegisterNetVarCallback_time,  &Hook_RegisterNetVarCallback_time,  bAttach);
	if (v_Engine_RegisterNetVarCallback_ent)
		DetourSetup(&v_Engine_RegisterNetVarCallback_ent,   &Hook_RegisterNetVarCallback_ent,   bAttach);
	if (bAttach)
		Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] RegisterNetworkedVariableChangeCallback_* detours attached (SDK-only routing)\n");

	Warning(eDLL_T::ENGINE, "[ScriptNetDataExt] SNDC_GLOBAL_NON_REWIND (category %d) -- limits: bool=%d range=%d bigint=%d time=%d entity=%d\n",
		SNDC_GLOBAL_NON_REWIND,
		NON_REWIND_LIMITS[0], NON_REWIND_LIMITS[1], NON_REWIND_LIMITS[2],
		NON_REWIND_LIMITS[3], NON_REWIND_LIMITS[4]);
}
#endif // CLIENT_DLL
