//=============================================================================//
// 
// Purpose: Script-side ModSystem implementation
// 
//=============================================================================//
#include "pluginsystem/modsystem.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "game/shared/vscript_shared.h"
#include <string>
#if defined(CLIENT_DLL)
#include "vscript/vsquirrel_s21.h"
#endif

// The S21 SQVM keeps its shared state at +0x50 and has no _scriptvm slot, so the
// S3 v->GetScriptVM() walk lands on a null pointer and every accessor built on it
// faults. Take the context from the S21 VM-type byte and the instance the Init
// hook recorded for it.
static CSquirrelVM* ModSystem_ResolveScriptVM(HSQUIRRELVM v, SQCONTEXT* const pContext)
{
	*pContext = SQCONTEXT::NONE;

#if defined(CLIENT_DLL)
	switch (SQVM_GetVMType_S21(v))
	{
	case eDLL_T::SCRIPT_CLIENT:
		*pContext = SQCONTEXT::CLIENT;
		return g_pClientScript;
	case eDLL_T::SCRIPT_UI:
		*pContext = SQCONTEXT::UI;
		return g_pUIScript;
	default:
		return nullptr;
	}
#else
	CSquirrelVM* const s = v ? v->GetScriptVM() : nullptr;

	if (s)
		*pContext = s->GetContext();

	return s;
#endif
}

static SQRESULT SharedScript_ModSystem_RunCallbacks(HSQUIRRELVM v)
{
	if (!ModSystem()->IsEnabled())
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	SQCONTEXT context = SQCONTEXT::NONE;
	CSquirrelVM* const s = ModSystem_ResolveScriptVM(v, &context);

	if (!s || context == SQCONTEXT::NONE)
	{
		Warning(eDLL_T::MODSYSTEM,
			"[MOD-CB] no script VM for this context; mod entry points skipped\n");
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	ModSystem()->LockModList();

	FOR_EACH_VEC(ModSystem()->GetModList(), i)
	{
		const CModSystem::ModInstance_t* const mod = ModSystem()->GetModList()[i];

		if (!mod->IsEnabled())
			continue;

		if (!mod->hasPrecompiledScripts)
			continue;

		const CUtlString& modIdNormalized = ModSystem()->GetNormalizedModID(mod);
		CUtlString modCodeCBName;

		modCodeCBName.Format("%s_%s_ModInit", Script_GetCodeCallbackPrefixForContext(context), modIdNormalized.String());
		HSCRIPT modCodeCB = s->FindFunction(modCodeCBName.String(), "void functionref()", NULL);

		if (!modCodeCB)
		{
			Warning(eDLL_T::MODSYSTEM, "Mod '%s'(\"%s\") has precompiled scripts, but entry point \"%s()\" was not found!\n",
				mod->name.String(), mod->id.String(), modCodeCBName.String());

			continue;
		}

		// Not freed on purpose. FindFunction returns engine-allocated memory;
		// free() through the wrong CRT corrupts the heap.
		s->ExecuteFunction(modCodeCB, nullptr, 0, nullptr, NULL);
	}

	ModSystem()->UnlockModList();
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// A container returned through "var" or "array< table >" reaches script as an int,
// so each mod travels as one delimited record in an array< string > instead.
// The separator is stripped from values so a field can never split a record.
static constexpr char kModRecordSep = '|';

static void ModSystem_AppendRecordField(std::string& rec, const char* const pszValue)
{
	if (!rec.empty())
		rec += kModRecordSep;

	for (const char* p = pszValue ? pszValue : ""; *p; ++p)
	{
		const char c = *p;
		rec += (c == kModRecordSep || c == '\n' || c == '\r') ? ' ' : c;
	}
}

static SQRESULT SharedScript_GetInstalledMods(HSQUIRRELVM v)
{
	sq_newarray(v, 0);

	if (!ModSystem()->IsEnabled())
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);

	ModSystem()->LockModList();

	FOR_EACH_VEC(ModSystem()->GetModList(), i)
	{
		const CModSystem::ModInstance_t* const mod = ModSystem()->GetModList()[i];

		if (!mod)
			continue;

		char szOrder[16];
		V_snprintf(szOrder, sizeof(szOrder), "%d", mod->loadOrder);

		std::string rec;
		ModSystem_AppendRecordField(rec, mod->id.String());
		ModSystem_AppendRecordField(rec, mod->name.String());
		ModSystem_AppendRecordField(rec, mod->version.String());
		ModSystem_AppendRecordField(rec, mod->author.String());
		ModSystem_AppendRecordField(rec, ModSystem_RealmToString(mod->realm));
		ModSystem_AppendRecordField(rec, ModSystem_StateToString(mod->state));
		ModSystem_AppendRecordField(rec, mod->IsEnabled() ? "1" : "0");
		ModSystem_AppendRecordField(rec, mod->hasScripts ? "1" : "0");
		ModSystem_AppendRecordField(rec, szOrder);

		sq_pushstring(v, rec.c_str(), (SQInteger)rec.length());
		sq_arrayappend(v, -2);
	}

	ModSystem()->UnlockModList();
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void Script_RegisterModSystemFunctions(CSquirrelVM* const s)
{
	DEFINE_SHARED_SCRIPTFUNC_NAMED(s, ModSystem_RunCallbacks, "Initiates the code callbacks for all mods registered by the modsystem", "void", "", false);
	DEFINE_SHARED_SCRIPTFUNC_NAMED(s, GetInstalledMods, "Returns each installed mod as one '|' delimited record: id|name|version|author|realm|state|enabled|hasScripts|order", "array< string >", "", false);
}
