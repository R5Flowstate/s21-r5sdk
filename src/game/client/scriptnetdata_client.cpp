#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: ScriptNetData client/UI script bindings.
// S21 fires networked-variable change callbacks natively on the CLIENT VM.
// UI VM GetPlayerNet* stubs return defaults (no player net data on UI).
//
//=============================================================================//

#include "core/stdafx.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/vscript_shared.h"
#include "vscript_client.h"
#include "scriptnetdata_client.h"
#include "vscript/vsquirrel_s21.h"

//------------------------------------------------------------------------------
// UI Stubs - GetPlayerNet* (return defaults; UI VM has no player net data)
//------------------------------------------------------------------------------
static SQRESULT UIScript_GetPlayerNetBool(HSQUIRRELVM v)
{
	sq_pushbool(v, SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetPlayerNetInt(HSQUIRRELVM v)
{
	sq_pushinteger(v, 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetPlayerNetFloat(HSQUIRRELVM v)
{
	sq_pushfloat(v, 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetPlayerNetTime(HSQUIRRELVM v)
{
	sq_pushfloat(v, 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetPlayerNetEnt(HSQUIRRELVM v)
{
	sq_pushnull(v);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// CLIENT VM: engine already registers RegisterNetworkedVariable ChangeCallback_*_Internal.
void ScriptNetData_RegisterClientFunctions(CSquirrelVM* vm)
{
	(void)vm;
}

// UI VM: GetPlayerNet* stubs only (engine has no UI-VM player net data).
// Netvar change-callback registration is CLIENT-only in product scripts
// (matches client bak _Internal path); no UI native required for those.
void ScriptNetData_RegisterUIFunctions(CSquirrelVM* vm)
{
	if (!vm)
		return;

	ScriptDataType_t p_ent_str[] = { FIELD_EHANDLE, FIELD_CSTRING };
	Script_RegisterFunc_S21(vm, "GetPlayerNetBool",
		(void*)UIScript_GetPlayerNetBool, FIELD_BOOLEAN, p_ent_str, 2);
	Script_RegisterFunc_S21(vm, "GetPlayerNetInt",
		(void*)UIScript_GetPlayerNetInt, FIELD_INTEGER, p_ent_str, 2);
	Script_RegisterFunc_S21(vm, "GetPlayerNetFloat",
		(void*)UIScript_GetPlayerNetFloat, FIELD_FLOAT, p_ent_str, 2);
	Script_RegisterFunc_S21(vm, "GetPlayerNetTime",
		(void*)UIScript_GetPlayerNetTime, FIELD_FLOAT, p_ent_str, 2);
	Script_RegisterFunc_S21(vm, "GetPlayerNetEnt",
		(void*)UIScript_GetPlayerNetEnt, FIELD_EHANDLE, p_ent_str, 2);
}

// SNDC limit natives were the overflow workaround -- removed. Nothing to register.
void ScriptNetData_RegisterLimitsOnClient(CSquirrelVM* vm) { (void)vm; }
void ScriptNetData_RegisterLimitsOnUI(CSquirrelVM* vm) { (void)vm; }

// No SDK callback map to clean up anymore.
void ScriptNetData_OnVMDestroyed(CSquirrelVM* vm) { (void)vm; }
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: ScriptNetData networked-variable change callbacks (UI VM too).
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier1/utlmap.h"
#include "tier1/utlvector.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/vscript_shared.h"
#include "vscript_client.h"
#include "scriptnetdata_client.h"
#include "game/shared/scriptnetdata_ext.h"
#include "game/shared/scriptnetdata_limits.h"

//------------------------------------------------------------------------------
// Callback Storage
//------------------------------------------------------------------------------
struct NetVarCallback_t
{
	SQObject callback;
	ScriptNetVarType_e type;

	NetVarCallback_t() : type(SNVT_BOOL)
	{
		callback._type = OT_NULL;
		callback._unVal.pUserPointer = nullptr;
	}
};

// Separate storage per VM
static CUtlMap<uint32_t, CUtlVector<NetVarCallback_t>*> s_ClientCallbacks;
static CUtlMap<uint32_t, CUtlVector<NetVarCallback_t>*> s_UICallbacks;
static bool s_bInitialized = false;

// Track the VM pointer that was active when callbacks were registered.
// If the VM is recreated (map change), the pointer changes and we know
// all stored SQObjects are stale -- skip them instead of crashing.
static HSQUIRRELVM s_pClientVM_WhenRegistered = nullptr;
static HSQUIRRELVM s_pUIVM_WhenRegistered = nullptr;

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------
static CUtlMap<uint32_t, CUtlVector<NetVarCallback_t>*>* GetCallbackMap(HSQUIRRELVM v)
{
	if (g_pClientScript && g_pClientScript->GetVM() == v)
		return &s_ClientCallbacks;
	if (g_pUIScript && g_pUIScript->GetVM() == v)
		return &s_UICallbacks;
	return nullptr;
}

static uint32_t HashVarName(const char* name)
{
	uint32_t hash = 2166136261u;
	while (*name)
	{
		hash ^= (uint8_t)*name++;
		hash *= 16777619u;
	}
	return hash;
}

static void EnsureInitialized()
{
	if (!s_bInitialized)
	{
		s_ClientCallbacks.SetLessFunc(DefLessFunc(uint32_t));
		s_UICallbacks.SetLessFunc(DefLessFunc(uint32_t));
		s_bInitialized = true;
	}
}

//------------------------------------------------------------------------------
// Internal: Register a callback
//------------------------------------------------------------------------------
static SQRESULT RegisterCallback_Internal(HSQUIRRELVM v, ScriptNetVarType_e type);

SQRESULT ScriptNetData_HandleCallbackRegistration(HSQUIRRELVM v, int type)
{
	return RegisterCallback_Internal(v, static_cast<ScriptNetVarType_e>(type));
}

static SQRESULT RegisterCallback_Internal(HSQUIRRELVM v, ScriptNetVarType_e type)
{
	EnsureInitialized();

	const SQChar* varName = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &varName)) || !varName)
	{
		v_SQVM_ScriptError("Expected string for variable name");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	SQObject callbackObj;
	if (SQ_FAILED(sq_getstackobj(v, 3, &callbackObj)))
	{
		v_SQVM_ScriptError("Failed to get callback object");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	if (!sq_isclosure(callbackObj) && !sq_isnativeclosure(callbackObj))
	{
		v_SQVM_ScriptError("Expected function for callback");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	CUtlMap<uint32_t, CUtlVector<NetVarCallback_t>*>* pCallbacks = GetCallbackMap(v);
	if (!pCallbacks)
	{
		v_SQVM_ScriptError("Unknown VM context");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	uint32_t hash = HashVarName(varName);

	unsigned short idx = pCallbacks->Find(hash);
	if (idx == pCallbacks->InvalidIndex())
	{
		CUtlVector<NetVarCallback_t>* pNewList = new CUtlVector<NetVarCallback_t>();
		idx = pCallbacks->Insert(hash, pNewList);
	}

	NetVarCallback_t cb;
	cb.type = type;
	cb.callback = callbackObj;

	sq_addref(v, &cb.callback);
	pCallbacks->Element(idx)->AddToTail(cb);

	// Remember which VM these callbacks belong to
	if (pCallbacks == &s_ClientCallbacks)
		s_pClientVM_WhenRegistered = v;
	else if (pCallbacks == &s_UICallbacks)
		s_pUIVM_WhenRegistered = v;

	// Track NonRewind vars for change detection in SDKProcessCallbacks
	SNDC_TrackNonRewindCallback(varName);

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//------------------------------------------------------------------------------
// Native Functions - Registration (UI VM only, CLIENT has engine natives)
//------------------------------------------------------------------------------
static SQRESULT UIScript_RegisterNetworkedVariableChangeCallback_bool(HSQUIRRELVM v)
{
	return RegisterCallback_Internal(v, SNVT_BOOL);
}

static SQRESULT UIScript_RegisterNetworkedVariableChangeCallback_int(HSQUIRRELVM v)
{
	return RegisterCallback_Internal(v, SNVT_INT);
}

static SQRESULT UIScript_RegisterNetworkedVariableChangeCallback_float(HSQUIRRELVM v)
{
	return RegisterCallback_Internal(v, SNVT_FLOAT_RANGE);
}

static SQRESULT UIScript_RegisterNetworkedVariableChangeCallback_time(HSQUIRRELVM v)
{
	return RegisterCallback_Internal(v, SNVT_TIME);
}

static SQRESULT UIScript_RegisterNetworkedVariableChangeCallback_ent(HSQUIRRELVM v)
{
	return RegisterCallback_Internal(v, SNVT_ENTITY);
}

//------------------------------------------------------------------------------
// Trigger Callbacks
//------------------------------------------------------------------------------
static SQRESULT TriggerNetVarCallbacks_Internal(HSQUIRRELVM v)
{
	EnsureInitialized();

	const SQChar* varName = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &varName)) || !varName)
	{
		v_SQVM_ScriptError("Expected string for variable name");
		SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
	}

	CUtlMap<uint32_t, CUtlVector<NetVarCallback_t>*>* pCallbacks = GetCallbackMap(v);
	if (!pCallbacks)
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	uint32_t hash = HashVarName(varName);
	unsigned short idx = pCallbacks->Find(hash);
	if (idx == pCallbacks->InvalidIndex())
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SQObject entityObj, oldValueObj, newValueObj, changedObj;
	sq_getstackobj(v, 3, &entityObj);
	sq_getstackobj(v, 4, &oldValueObj);
	sq_getstackobj(v, 5, &newValueObj);
	sq_getstackobj(v, 6, &changedObj);

	CUtlVector<NetVarCallback_t>* pVec = pCallbacks->Element(idx);
	for (int i = 0; i < pVec->Count(); i++)
	{
		NetVarCallback_t& cb = (*pVec)[i];

		sq_pushobject(v, cb.callback);
		sq_pushroottable(v);
		sq_pushobject(v, entityObj);    // entity
		sq_pushobject(v, oldValueObj);  // oldValue
		sq_pushobject(v, newValueObj);  // newValue
		sq_pushobject(v, changedObj);   // actuallyChanged

		if (SQ_FAILED(sq_call(v, 5, SQFalse, SQTrue)))
		{
			sq_pop(v, 1);
			continue;
		}
		sq_pop(v, 1);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// Probe _sharedstate->_stringtable; g_pUIScript can still point at a dying wrapper.
static bool IsVMStillAlive(HSQUIRRELVM v)
{
	if (!v)
		return false;
	__try
	{
		SQSharedState* ss = v->_sharedstate;
		if (!ss) return false;
		if (!ss->_stringtable) return false;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

// Validate SQObject before calling; false if the VM was recycled.
static bool FireSingleCallback(HSQUIRRELVM v, NetVarCallback_t& cb,
	int32_t oldValue, int32_t newValue, bool changed)
{
	// Validate the SQObject is still a valid closure before touching the VM stack
	if (!sq_isclosure(cb.callback) && !sq_isnativeclosure(cb.callback))
	{
		Warning(eDLL_T::ENGINE, "[SNDC_CB] Skipping stale callback (type=0x%X)\n",
			cb.callback._type);
		return false;
	}

	// Extra guard: closure pointer must be non-null (post-VM-teardown SQObjects
	// can keep a "valid" _type tag in their 16-byte slot but have a freed/zeroed
	// _unVal; push/call would deref it and crash.).
	if (!cb.callback._unVal.pUserPointer)
	{
		Warning(eDLL_T::ENGINE, "[SNDC_CB] Skipping callback with null closure pointer\n");
		return false;
	}

	sq_pushobject(v, cb.callback);
	sq_pushroottable(v);
	sq_pushnull(v);                                   // entity (null for global)

	// Push old/new values with correct types matching the callback signature
	switch (cb.type)
	{
		case SNVT_BOOL:
			sq_pushbool(v, oldValue ? SQTrue : SQFalse);
			sq_pushbool(v, newValue ? SQTrue : SQFalse);
			break;
		case SNVT_FLOAT_RANGE:
		case SNVT_TIME:
		{
			// Reinterpret the int32 bits as float (category stores raw float bits)
			float oldF, newF;
			memcpy(&oldF, &oldValue, sizeof(float));
			memcpy(&newF, &newValue, sizeof(float));
			sq_pushfloat(v, oldF);
			sq_pushfloat(v, newF);
			break;
		}
		default: // SNVT_INT, SNVT_ENTITY, etc.
			sq_pushinteger(v, (SQInteger)oldValue);
			sq_pushinteger(v, (SQInteger)newValue);
			break;
	}

	sq_pushbool(v, changed ? SQTrue : SQFalse);

	// Pre-clear _lasterror to prevent stale error detection
	v->_lasterror._type = OT_NULL;
	v->_lasterror._unVal.pUserPointer = nullptr;

	// SQFalse for raiseerror -- don't let callback errors kill the VM
	if (SQ_FAILED(sq_call(v, 5, SQFalse, SQFalse)))
		Warning(eDLL_T::ENGINE, "[SNDC_CB] Callback execution failed (type=%d old=%d new=%d), continuing\n",
			cb.type, oldValue, newValue);

	// Always clear _lasterror after sq_call (both success and failure).
	// Engine's error checker sees stale _lasterror -> triggers disconnect.
	v->_lasterror._type = OT_NULL;
	v->_lasterror._unVal.pUserPointer = nullptr;

	sq_pop(v, 1); // pop closure
	return true;
}

static void FireCallbacksOnVM(HSQUIRRELVM v,
	CUtlMap<uint32_t, CUtlVector<NetVarCallback_t>*>& map,
	uint32_t hash, int32_t oldValue, int32_t newValue, bool changed)
{
	unsigned short idx = map.Find(hash);
	if (idx == map.InvalidIndex())
		return;

	CUtlVector<NetVarCallback_t>* pVec = map.Element(idx);
	for (int i = 0; i < pVec->Count(); i++)
		FireSingleCallback(v, (*pVec)[i], oldValue, newValue, changed);
}

// Fire overflow callbacks when NET_ScriptMessage delivers a changed value.
void SNDC_FireOverflowCallbacks(const char* varName, int32_t oldValue, int32_t newValue)
{
	EnsureInitialized();

	uint32_t hash = HashVarName(varName);
	bool changed = (oldValue != newValue);

	DevMsg(eDLL_T::ENGINE, "[SNDC_CB] FireOverflowCallbacks '%s' old=%d new=%d\n",
		varName, oldValue, newValue);

	if (g_pClientScript)
	{
		HSQUIRRELVM v = g_pClientScript->GetVM();
		if (v && v == s_pClientVM_WhenRegistered && IsVMStillAlive(v))
		{
			unsigned short idx = s_ClientCallbacks.Find(hash);
			if (idx != s_ClientCallbacks.InvalidIndex())
				FireCallbacksOnVM(v, s_ClientCallbacks, hash, oldValue, newValue, changed);
		}
	}

	if (g_pUIScript)
	{
		HSQUIRRELVM v = g_pUIScript->GetVM();
		if (v && v == s_pUIVM_WhenRegistered && IsVMStillAlive(v))
		{
			unsigned short idx = s_UICallbacks.Find(hash);
			if (idx != s_UICallbacks.InvalidIndex())
				FireCallbacksOnVM(v, s_UICallbacks, hash, oldValue, newValue, changed);
		}
	}
}

// Fire all SDK overflow callbacks as init notifications.
static void FireAllSDKCallbacks(HSQUIRRELVM v, CUtlMap<uint32_t, CUtlVector<NetVarCallback_t>*>& callbacks)
{
	for (unsigned short i = callbacks.FirstInorder();
		i != callbacks.InvalidIndex(); i = callbacks.NextInorder(i))
	{
		CUtlVector<NetVarCallback_t>* pVec = callbacks.Element(i);
		for (int j = 0; j < pVec->Count(); j++)
			FireSingleCallback(v, (*pVec)[j], 0, 0, false);
	}
}

// Separate entry points for CLIENT and UI macros
static SQRESULT ClientScript_TriggerNetVarCallbacks(HSQUIRRELVM v)
{
	return TriggerNetVarCallbacks_Internal(v);
}

static SQRESULT UIScript_TriggerNetVarCallbacks(HSQUIRRELVM v)
{
	return TriggerNetVarCallbacks_Internal(v);
}

// TriggerChangeCallbacks on the server global entity with current values.
static SQRESULT ClientScript_TriggerGlobalChangeCallbacks(HSQUIRRELVM v)
{
	// Fire engine's built-in callbacks (stored in scriptNetVars)
	ScriptNetDataExt_TriggerGlobalChangeCallbacks();

	// Fire SDK overflow callbacks (stored in s_ClientCallbacks)
	EnsureInitialized();
	if (s_ClientCallbacks.Count() > 0)
	{
		DevMsg(eDLL_T::ENGINE, "[SNDC] TriggerGlobalChangeCallbacks: firing %d SDK CLIENT callback groups\n",
			s_ClientCallbacks.Count());
		FireAllSDKCallbacks(v, s_ClientCallbacks);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_TriggerGlobalChangeCallbacks(HSQUIRRELVM v)
{
	// Fire engine's built-in callbacks (stored in scriptNetVars)
	ScriptNetDataExt_TriggerGlobalChangeCallbacks();

	// Fire SDK overflow callbacks (stored in s_UICallbacks)
	EnsureInitialized();
	if (s_UICallbacks.Count() > 0)
	{
		DevMsg(eDLL_T::ENGINE, "[SNDC] TriggerGlobalChangeCallbacks: firing %d SDK UI callback groups\n",
			s_UICallbacks.Count());
		FireAllSDKCallbacks(v, s_UICallbacks);
	}

	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//------------------------------------------------------------------------------
// Cleanup
//------------------------------------------------------------------------------
static void PurgeCallbackMap(CUtlMap<uint32_t, CUtlVector<NetVarCallback_t>*>& map)
{
	for (unsigned short i = map.FirstInorder();
		i != map.InvalidIndex(); i = map.NextInorder(i))
	{
		delete map.Element(i);
	}
	map.RemoveAll();
}

void ScriptNetData_OnVMDestroyed(CSquirrelVM* vm)
{
	if (!s_bInitialized || !vm)
		return;

	HSQUIRRELVM v = vm->GetVM();
	if (!v)
		return;

	CUtlMap<uint32_t, CUtlVector<NetVarCallback_t>*>* pCallbacks = nullptr;

	if (g_pClientScript && g_pClientScript == vm)
		pCallbacks = &s_ClientCallbacks;
	else if (g_pUIScript && g_pUIScript == vm)
		pCallbacks = &s_UICallbacks;

	if (pCallbacks)
	{
		for (unsigned short i = pCallbacks->FirstInorder();
			i != pCallbacks->InvalidIndex(); i = pCallbacks->NextInorder(i))
		{
			CUtlVector<NetVarCallback_t>* pVec = pCallbacks->Element(i);
			for (int j = 0; j < pVec->Count(); j++)
				sq_release(v, &(*pVec)[j].callback);
			delete pVec;
		}
		pCallbacks->RemoveAll();
	}

	// If the destroying VM matched one of our tracked VMs, drop the pointer
	// so FireOverflowCallbacks cannot hand out dangling VM references on the
	// narrow window between VM destroy and re-register.
	if (v == s_pClientVM_WhenRegistered)
		s_pClientVM_WhenRegistered = nullptr;
	if (v == s_pUIVM_WhenRegistered)
		s_pUIVM_WhenRegistered = nullptr;

	// Scrub engine-side callback slots regardless of which VM just died.
	// The engine array is shared across CLIENT/SERVER registration paths, so
	// any stale entry must be zeroed before the next VM starts dispatching.
	ScriptNetDataExt_ClearEngineCallbacks();
}

void ScriptNetData_LevelShutdown()
{
	// At shutdown the VMs may already be destroyed, so we can't sq_release --
	// just free the storage. The SQObjects are dead anyway.
	PurgeCallbackMap(s_ClientCallbacks);
	PurgeCallbackMap(s_UICallbacks);
	s_pClientVM_WhenRegistered = nullptr;
	s_pUIVM_WhenRegistered = nullptr;
	// Zero engine-storage callbacks too so the next VM instance can't hit a
	// dangling SQObject during its first dispatch.
	ScriptNetDataExt_ClearEngineCallbacks();
	DevMsg(eDLL_T::ENGINE, "[SNDC] ScriptNetData_LevelShutdown: cleared callback maps\n");
}

//------------------------------------------------------------------------------
// UI Stubs - GetPlayerNet* (return defaults, UI has no player net data)
//------------------------------------------------------------------------------
static SQRESULT UIScript_GetPlayerNetBool(HSQUIRRELVM v)
{
	sq_pushbool(v, SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetPlayerNetInt(HSQUIRRELVM v)
{
	sq_pushinteger(v, 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetPlayerNetFloat(HSQUIRRELVM v)
{
	sq_pushfloat(v, 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetPlayerNetTime(HSQUIRRELVM v)
{
	sq_pushfloat(v, 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT UIScript_GetPlayerNetEnt(HSQUIRRELVM v)
{
	sq_pushnull(v);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// CLIENT-side callback registration into s_ClientCallbacks.
static SQRESULT ClientScript_SDKRegisterNetVarCallback_bool(HSQUIRRELVM v) { return RegisterCallback_Internal(v, SNVT_BOOL); }
static SQRESULT ClientScript_SDKRegisterNetVarCallback_int(HSQUIRRELVM v)  { return RegisterCallback_Internal(v, SNVT_INT); }
static SQRESULT ClientScript_SDKRegisterNetVarCallback_float(HSQUIRRELVM v){ return RegisterCallback_Internal(v, SNVT_FLOAT_RANGE); }
static SQRESULT ClientScript_SDKRegisterNetVarCallback_time(HSQUIRRELVM v) { return RegisterCallback_Internal(v, SNVT_TIME); }
static SQRESULT ClientScript_SDKRegisterNetVarCallback_ent(HSQUIRRELVM v)  { return RegisterCallback_Internal(v, SNVT_ENTITY); }
// UI aliases
static SQRESULT UIScript_SDKRegisterNetVarCallback_bool(HSQUIRRELVM v) { return RegisterCallback_Internal(v, SNVT_BOOL); }
static SQRESULT UIScript_SDKRegisterNetVarCallback_int(HSQUIRRELVM v)  { return RegisterCallback_Internal(v, SNVT_INT); }
static SQRESULT UIScript_SDKRegisterNetVarCallback_float(HSQUIRRELVM v){ return RegisterCallback_Internal(v, SNVT_FLOAT_RANGE); }
static SQRESULT UIScript_SDKRegisterNetVarCallback_time(HSQUIRRELVM v) { return RegisterCallback_Internal(v, SNVT_TIME); }
static SQRESULT UIScript_SDKRegisterNetVarCallback_ent(HSQUIRRELVM v)  { return RegisterCallback_Internal(v, SNVT_ENTITY); }

void ScriptNetData_RegisterClientFunctions(CSquirrelVM* vm)
{
	Warning(eDLL_T::ENGINE, "[SNDC] RegisterClientFunctions ENTER vm=0x%p hvm=0x%p\n",
		vm, vm ? vm->GetVM() : nullptr);
	if (!vm)
		return;

	EnsureInitialized();

	// Purge any stale callbacks from a previous VM instance.
	// The old VM is already destroyed; its SQObjects are dangling pointers.
	// We can't sq_release (the old VM is gone), so just free the storage.
	if (s_ClientCallbacks.Count() > 0)
	{
		Warning(eDLL_T::ENGINE,
			"[SNDC] Purging %d stale CLIENT callback groups from previous VM\n",
			s_ClientCallbacks.Count());
		PurgeCallbackMap(s_ClientCallbacks);
	}
	Warning(eDLL_T::ENGINE, "[SNDC] RegisterClientFunctions: purge done, calling ClearEngineCallbacks\n");
	// Also scrub engine-storage callbacks for this VM's var table.
	ScriptNetDataExt_ClearEngineCallbacks();
	Warning(eDLL_T::ENGINE, "[SNDC] RegisterClientFunctions: ClearEngineCallbacks done\n");
	s_pClientVM_WhenRegistered = vm->GetVM();

	// S21 native Get/SetGlobalNonRewind* looks up category 1. The C++ enum
	// value 5 is the dedi extension index and must not overwrite this.
	vm->RegisterConstant("SNDC_GLOBAL_NON_REWIND", 1);
	Warning(eDLL_T::CLIENT, "[SNDC] SNDC_GLOBAL_NON_REWIND=1 (engine native)\n");

	DEFINE_CLIENT_SCRIPTFUNC_NAMED(vm, TriggerNetVarCallbacks,
		"Triggers registered callbacks for a netvar",
		"void", "string varName, entity ent, var oldValue, var newValue, bool actuallyChanged", false);

	DEFINE_CLIENT_SCRIPTFUNC_NAMED(vm, TriggerGlobalChangeCallbacks,
		"Fires all registered netvar change callbacks for the global entity with current values",
		"void", "", false);

	// SDK callback registration -- typed signatures matching engine's natives
	DEFINE_CLIENT_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_bool,
		"Register overflow callback for bool var",
		"void", "string varName, void functionref(entity, bool, bool, bool) callback", false);
	DEFINE_CLIENT_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_int,
		"Register overflow callback for int var",
		"void", "string varName, void functionref(entity, int, int, bool) callback", false);
	DEFINE_CLIENT_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_float,
		"Register overflow callback for float var",
		"void", "string varName, void functionref(entity, float, float, bool) callback", false);
	DEFINE_CLIENT_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_time,
		"Register overflow callback for time var",
		"void", "string varName, void functionref(entity, float, float, bool) callback", false);
	DEFINE_CLIENT_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_ent,
		"Register overflow callback for entity var",
		"void", "string varName, void functionref(entity, entity, entity, bool) callback", false);
}

void ScriptNetData_RegisterUIFunctions(CSquirrelVM* vm)
{
	if (!vm)
		return;

	EnsureInitialized();

	Warning(eDLL_T::ENGINE, "[SNDC] RegisterUIFunctions ENTER vm=0x%p hvm=0x%p\n",
		vm, vm ? vm->GetVM() : nullptr);
	// Purge any stale callbacks from a previous VM instance.
	if (s_UICallbacks.Count() > 0)
	{
		Warning(eDLL_T::ENGINE,
			"[SNDC] Purging %d stale UI callback groups from previous VM\n",
			s_UICallbacks.Count());
		PurgeCallbackMap(s_UICallbacks);
	}
	Warning(eDLL_T::ENGINE, "[SNDC] RegisterUIFunctions: purge done, calling ClearEngineCallbacks\n");
	ScriptNetDataExt_ClearEngineCallbacks();
	Warning(eDLL_T::ENGINE, "[SNDC] RegisterUIFunctions: ClearEngineCallbacks done\n");
	s_pUIVM_WhenRegistered = vm->GetVM();

	vm->RegisterConstant("SNDC_GLOBAL_NON_REWIND", 1);
	Warning(eDLL_T::CLIENT, "[SNDC] SNDC_GLOBAL_NON_REWIND=1 (engine native)\n");

	// DEDI/S3 half: S3 ScriptFunctionBinding_t path. S21 client UI registration
	// lives in the CLIENT_DLL half (Script_RegisterFunc_S21 GetPlayerNet only).
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, RegisterNetworkedVariableChangeCallback_bool,
		"Registers a callback for bool netvar changes",
		"void", "string varName, void functionref(entity, bool, bool, bool) callback", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, RegisterNetworkedVariableChangeCallback_int,
		"Registers a callback for int netvar changes",
		"void", "string varName, void functionref(entity, int, int, bool) callback", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, RegisterNetworkedVariableChangeCallback_float,
		"Registers a callback for float netvar changes",
		"void", "string varName, void functionref(entity, float, float, bool) callback", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, RegisterNetworkedVariableChangeCallback_time,
		"Registers a callback for time netvar changes",
		"void", "string varName, void functionref(entity, float, float, bool) callback", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, RegisterNetworkedVariableChangeCallback_ent,
		"Registers a callback for entity netvar changes",
		"void", "string varName, void functionref(entity, entity, entity, bool) callback", false);

	DEFINE_UI_SCRIPTFUNC_NAMED(vm, TriggerNetVarCallbacks,
		"Triggers registered callbacks for a netvar",
		"void", "string varName, entity ent, var oldValue, var newValue, bool actuallyChanged", false);

	DEFINE_UI_SCRIPTFUNC_NAMED(vm, TriggerGlobalChangeCallbacks,
		"Fires all registered netvar change callbacks for the global entity with current values",
		"void", "", false);

	DEFINE_UI_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_bool,
		"Register overflow callback for bool var",
		"void", "string varName, void functionref(entity, bool, bool, bool) callback", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_int,
		"Register overflow callback for int var",
		"void", "string varName, void functionref(entity, int, int, bool) callback", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_float,
		"Register overflow callback for float var",
		"void", "string varName, void functionref(entity, float, float, bool) callback", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_time,
		"Register overflow callback for time var",
		"void", "string varName, void functionref(entity, float, float, bool) callback", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, SDKRegisterNetVarCallback_ent,
		"Register overflow callback for entity var",
		"void", "string varName, void functionref(entity, entity, entity, bool) callback", false);

	DEFINE_UI_SCRIPTFUNC_NAMED(vm, GetPlayerNetBool,
		"Gets a player bool net variable",
		"bool", "entity player, string varName", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, GetPlayerNetInt,
		"Gets a player int net variable",
		"int", "entity player, string varName", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, GetPlayerNetFloat,
		"Gets a player float net variable",
		"float", "entity player, string varName", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, GetPlayerNetTime,
		"Gets a player time net variable",
		"float", "entity player, string varName", false);
	DEFINE_UI_SCRIPTFUNC_NAMED(vm, GetPlayerNetEnt,
		"Gets a player entity net variable",
		"entity", "entity player, string varName", false);
}
#endif // CLIENT_DLL
