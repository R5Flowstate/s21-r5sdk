//=============================================================================//
//
// Purpose: VScript System
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/frametask.h"
#include "tier0/fasttimer.h"
#include "tier1/cvar.h"
#include "languages/squirrel_re/vsquirrel.h"
#include "vscript/vscript.h"
#include "game/shared/vscript_shared.h"
#include "pluginsystem/modsystem.h"
#include "vscript/script_wraps.h"
#if !defined(CLIENT_DLL)
#include "game/server/vscript_server.h" // Script_RegisterTraceLineEntitiesOnlyArity
#include "game/shared/scriptremotefunctions_server.h"
#endif

static const char* s_scriptContextNames[] = { "SERVER", "CLIENT", "UI" };

static bool Script_WhenWordEq(const char* const start, const int n, const char* const want)
{
	if (!start || !want || n <= 0)
		return false;
	int w = 0;
	while (want[w])
		++w;
	return n == w && !_strnicmp(start, want, n);
}

// Upper bound of the native When evaluator: only the VM context names are known
// here; every other name (DEV, MP, SP, ...) depends on runtime state, so it is
// MAYBE. A list is counted unless its When is definitely false -- counting less
// than the native loads would let it overrun the fixed script array.
enum class WhenTri_t { NO, YES, MAYBE };

static bool Script_WhenIsNameChar(const char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static void Script_WhenSkipSpace(const char*& p)
{
	while (*p == ' ' || *p == '\t')
		++p;
}

static WhenTri_t Script_WhenEvalExpr(const char*& p, const char* const want, const int depth, bool& bOk);

static WhenTri_t Script_WhenEvalUnary(const char*& p, const char* const want, const int depth, bool& bOk)
{
	Script_WhenSkipSpace(p);
	if (*p == '!')
	{
		++p;
		const WhenTri_t v = Script_WhenEvalUnary(p, want, depth + 1, bOk);
		return v == WhenTri_t::MAYBE ? v : (v == WhenTri_t::YES ? WhenTri_t::NO : WhenTri_t::YES);
	}
	if (*p == '(')
	{
		++p;
		const WhenTri_t v = Script_WhenEvalExpr(p, want, depth + 1, bOk);
		Script_WhenSkipSpace(p);
		if (*p != ')')
			bOk = false;
		else
			++p;
		return v;
	}

	const char* const start = p;
	while (Script_WhenIsNameChar(*p))
		++p;
	const int n = static_cast<int>(p - start);
	if (n == 0)
	{
		bOk = false;
		return WhenTri_t::MAYBE;
	}
	for (const char* const name : s_scriptContextNames)
	{
		if (Script_WhenWordEq(start, n, name))
			return (want && Script_WhenWordEq(start, n, want)) ? WhenTri_t::YES : WhenTri_t::NO;
	}
	return WhenTri_t::MAYBE;
}

static WhenTri_t Script_WhenEvalExpr(const char*& p, const char* const want, const int depth, bool& bOk)
{
	if (depth > 32)
	{
		bOk = false;
		return WhenTri_t::MAYBE;
	}

	WhenTri_t acc = Script_WhenEvalUnary(p, want, depth, bOk);
	char op = 0;
	while (bOk)
	{
		Script_WhenSkipSpace(p);
		const bool bAnd = p[0] == '&' && p[1] == '&';
		const bool bOr = p[0] == '|' && p[1] == '|';
		if (!bAnd && !bOr)
			break;
		// Mixed && / || without parentheses: precedence is the native's call.
		if (op && op != p[0])
			bOk = false;
		op = p[0];
		p += 2;
		const WhenTri_t rhs = Script_WhenEvalUnary(p, want, depth, bOk);
		if (bAnd)
			acc = (acc == WhenTri_t::NO || rhs == WhenTri_t::NO) ? WhenTri_t::NO
				: (acc == WhenTri_t::YES && rhs == WhenTri_t::YES) ? WhenTri_t::YES : WhenTri_t::MAYBE;
		else
			acc = (acc == WhenTri_t::YES || rhs == WhenTri_t::YES) ? WhenTri_t::YES
				: (acc == WhenTri_t::NO && rhs == WhenTri_t::NO) ? WhenTri_t::NO : WhenTri_t::MAYBE;
	}
	return acc;
}

static bool Script_WhenExprMatches(const char* const expr, const SQCONTEXT context)
{
	if (!expr || !expr[0])
		return true;
	const int idx = static_cast<int>(context);
	const char* const want = (idx >= 0 && idx < 3) ? s_scriptContextNames[idx] : nullptr;
	const char* p = expr;
	bool bOk = true;
	const WhenTri_t v = Script_WhenEvalExpr(p, want, 0, bOk);
	Script_WhenSkipSpace(p);
	if (!bOk || *p)
		return true;
	return v != WhenTri_t::NO;
}

static bool Script_AddListedCount(int* const total, const int add)
{
	if (add < 0 || add > MAX_SCRIPT_FILES_TO_LOAD)
		return false;
	if (*total > MAX_SCRIPT_FILES_TO_LOAD - add)
		return false;
	*total += add;
	return true;
}

static bool Script_WhenNodeMatches(const RSON::Node_t* const node, const SQCONTEXT context)
{
	if (!node)
		return false;
	if (node->type & RSON::RSON_STRING)
		return Script_WhenExprMatches(node->value.GetString(), context);
	if (node->type & RSON::RSON_ARRAY)
	{
		// A mixed-type array stores child nodes, not values; GetArrayValue would read node headers.
		if (node->type != (RSON::RSON_ARRAY | RSON::RSON_STRING))
			return false;
		if (node->valueCount < 0 || node->valueCount > 32)
			return false;
		for (int i = 0; i < node->valueCount; ++i)
		{
			const RSON::Value_t* const val = node->GetArrayValue(i);
			if (val && Script_WhenExprMatches(val->GetString(), context))
				return true;
		}
		return false;
	}
	return false;
}

// Grammar the native When evaluator compiles without failing: expr := unary (('&&'|'||') unary)*,
// unary := '!'* (NAME | '(' expr ')'). Its failure path is unsafe, and unknown names fail.
static bool Script_ModWhenParse(const char*& p, int depth);

static void Script_ModWhenSkipSpace(const char*& p)
{
	while (*p == ' ' || *p == '\t')
		++p;
}

static bool Script_ModWhenUnary(const char*& p, const int depth)
{
	static const char* const kNames[] = { "SERVER", "CLIENT", "UI", "SP", "MP", "DEV" };

	Script_ModWhenSkipSpace(p);
	while (*p == '!')
	{
		++p;
		Script_ModWhenSkipSpace(p);
	}

	if (*p == '(')
	{
		++p;
		if (!Script_ModWhenParse(p, depth + 1))
			return false;
		Script_ModWhenSkipSpace(p);
		if (*p != ')')
			return false;
		++p;
		return true;
	}

	const char* const start = p;
	while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')
		++p;
	const int n = static_cast<int>(p - start);
	for (const char* const name : kNames)
	{
		if (static_cast<int>(strlen(name)) == n && !strncmp(start, name, n))
			return true;
	}
	return false;
}

static bool Script_ModWhenParse(const char*& p, const int depth)
{
	if (depth > 8 || !Script_ModWhenUnary(p, depth))
		return false;

	for (;;)
	{
		Script_ModWhenSkipSpace(p);
		if ((p[0] == '&' && p[1] == '&') || (p[0] == '|' && p[1] == '|'))
		{
			p += 2;
			if (!Script_ModWhenUnary(p, depth))
				return false;
			continue;
		}
		return true;
	}
}

static bool Script_ModWhenIsSafe(const char* const expr)
{
	if (!expr || strlen(expr) > 256)
		return false;
	const char* p = expr;
	if (!Script_ModWhenParse(p, 0))
		return false;
	Script_ModWhenSkipSpace(p);
	return *p == '\0';
}

// A mod compile list reaches the native parser, which reads arrays at string stride and
// evaluates When itself: only plain strings / string arrays and a known When vocabulary pass.
static bool Script_ModRsonIsSafe(const RSON::Node_t* const rson)
{
	if (!rson || !(rson->type & RSON::RSON_OBJECT))
		return false;

	int safety = 0;
	for (RSON::Field_t* key = rson->GetFirstSubKey(); key != nullptr; key = key->GetNextKey())
	{
		if (++safety > 4096 || !key->name)
			return false;

		const bool bWhen = !_stricmp(key->name, "When");
		if (!bWhen && _stricmp(key->name, "Scripts"))
			continue;

		const int type = key->node.type;
		if (type == RSON::RSON_STRING)
		{
			if (bWhen && !Script_ModWhenIsSafe(key->node.value.GetString()))
				return false;
			continue;
		}
		if (bWhen || type != (RSON::RSON_ARRAY | RSON::RSON_STRING))
			return false;
	}
	return true;
}

static int Script_CountRsonScripts(const RSON::Node_t* const rson, const SQCONTEXT context)
{
	if (!rson || !(rson->type & RSON::RSON_OBJECT))
		return 0;

	int total = 0;
	int safety = 0;
	const RSON::Field_t* whenField = nullptr;
	for (RSON::Field_t* key = rson->GetFirstSubKey(); key != nullptr; key = key->GetNextKey())
	{
		if (++safety > 4096)
			return MAX_SCRIPT_FILES_TO_LOAD + 1;
		if (!key->name)
			continue;
		if (!_stricmp(key->name, "When"))
		{
			whenField = key;
			continue;
		}
		if (_stricmp(key->name, "Scripts"))
			continue;
		if (whenField && !Script_WhenNodeMatches(&whenField->node, context))
			continue;
		if (key->node.type & RSON::RSON_ARRAY)
		{
			if (!Script_AddListedCount(&total, key->node.valueCount))
				return MAX_SCRIPT_FILES_TO_LOAD + 1;
		}
		else if (key->node.type & RSON::RSON_STRING)
		{
			if (!Script_AddListedCount(&total, 1))
				return MAX_SCRIPT_FILES_TO_LOAD + 1;
		}
	}
	return total;
}

//---------------------------------------------------------------------------------
// Since we parse and append mod scripts to the base script list, we must defer the
// deallocation of the RSON buffer until after the pre-compile job has finished, as
// the script list holds pointers to the strings inside the RSON object!
//---------------------------------------------------------------------------------
struct ScriptModPrecompileListDeferred_s
{
	void Reset()
	{
		for (int i = 0; i < MAX_MODS_TO_LOAD; i++)
		{
			RSON::Node_t* const modRson = rson[i];

			if (!modRson)
				continue; // Nothing to free.

			RSON_Free(modRson, AlignedMemAlloc());
			AlignedMemAlloc()->Free(modRson);

			rson[i] = nullptr;
		}
	}

	RSON::Node_t* rson[ MAX_MODS_TO_LOAD ];
};

static ScriptModPrecompileListDeferred_s s_scriptModPrecompileListDeferred[(int)SQCONTEXT::COUNT];
static bool s_scriptModListAppended[(int)SQCONTEXT::COUNT];

//---------------------------------------------------------------------------------
// Purpose: Returns the script VM pointer by context
// Input: context - 
//---------------------------------------------------------------------------------
CSquirrelVM* Script_GetScriptHandle(const SQCONTEXT context)
{
	switch (context)
	{
	case SQCONTEXT::SERVER:
#if defined(CLIENT_DLL)
		// The client inject has no server VM, and g_pServerScript is not declared
		// in this configuration.
		return nullptr;
#else
		return g_pServerScript;
#endif
	case SQCONTEXT::CLIENT:
		return g_pClientScript;
	case SQCONTEXT::UI:
		return g_pUIScript;
	NO_DEFAULT
	}
}

//---------------------------------------------------------------------------------
// Purpose: loads the script list, listing scripts to be compiled.
// Input: *rsonfile - 
//---------------------------------------------------------------------------------
RSON::Node_t* Script_LoadScriptList(const SQChar* rsonfile)
{
	Msg(eDLL_T::ENGINE, "Loading script list: '%s'\n", rsonfile);
	return v_Script_LoadScriptList(rsonfile);
}

//---------------------------------------------------------------------------------
// Purpose: loads script files listed in the script list, to be compiled.
// Input: *s - 
// *path - 
// *name - 
// flags - 
//---------------------------------------------------------------------------------
SQBool Script_LoadScriptFile(CSquirrelVM* const s, const SQChar* path, const SQChar* name, SQInteger flags)
{
	const SQBool result = v_Script_LoadScriptFile(s, path, name, flags);
	ScriptWraps_ReleaseSources();
	return result;
}

//---------------------------------------------------------------------------------
// Purpose: appends listed mod script into an already existing script array
// Input: context - 
// *scriptArray - 
// *pScriptCount - 
//---------------------------------------------------------------------------------
static void Script_AppendModScriptList(const SQCONTEXT context, char** const scriptArray, int* const pScriptCount)
{
	ModSystem()->LockModList();
	CSquirrelVM* const s = Script_GetScriptHandle(context);

	FOR_EACH_VEC(ModSystem()->GetModList(), i)
	{
		CModSystem::ModInstance_t* const mod = ModSystem()->GetModList()[i];
		mod->hasPrecompiledScripts = false;

		if (!mod->IsEnabled())
			continue;

		// If its already loaded, use that one. This func can be called twice
		// from CSquirrelVM::PrecompileServerScripts and we don't want to
		// load the rson again.
		RSON::Node_t* modRson = s_scriptModPrecompileListDeferred[(int)context].rson[i];

		if (!modRson)
		{
			bool parseFailure;
			modRson = mod->LoadScriptCompileList(&parseFailure);

			if (parseFailure)
			{
				Error(s->GetNativeContext(), 0,
					"%s: Failed to parse RSON file \"%s\"\n",
					__FUNCTION__, mod->GetScriptCompileListPath().Get());
			}

			if (!modRson)
				continue; // Just continue, this mod doesn't contain a compile list.

			s_scriptModPrecompileListDeferred[(int)context].rson[i] = modRson;
		}

		s->SetAsCompiler(modRson);

		const CUtlString currentScriptList = mod->GetScriptCompileListPath();
		const char* const pCurrentScriptList = currentScriptList.String();

		char* modScriptPaths[MAX_SCRIPT_FILES_TO_LOAD];
		int modScriptCount = 0;

		if (!Script_ModRsonIsSafe(modRson))
		{
			Warning(eDLL_T::ENGINE,
				"[MOD-SCRIPT] '%s' compile list has a malformed Scripts/When entry (plain strings only; When uses SERVER CLIENT UI SP MP DEV with && || ! and parentheses) -- skipped\n",
				mod->name.String());
			continue;
		}

		const int listed = Script_CountRsonScripts(modRson, context);
		if (listed > MAX_SCRIPT_FILES_TO_LOAD)
		{
			Warning(eDLL_T::ENGINE,
				"[MOD-SCRIPT] '%s' Scripts count %d exceeds %d -- skipped\n",
				mod->name.String(), listed, MAX_SCRIPT_FILES_TO_LOAD);
			continue;
		}

		v_Script_ParseScriptList(context, pCurrentScriptList, modRson, modScriptPaths, &modScriptCount,
			// We check on the main `scriptArray` now because `scriptArray` was
			// already checked on `precompiledScriptArray` on the previous call.
			scriptArray, *pScriptCount);

		if (modScriptCount > 0)
		{
			// Compile-list entries are mod file content: drop anything that
			// escapes the mod dir before the VM ever sees it. Rejected pointers
			// are dropped, not freed -- allocation is native-owned and the drop
			// is bounded by MAX_SCRIPT_FILES_TO_LOAD once per VM init.
			int keptScriptCount = 0;
			for (int si = 0; si < modScriptCount; ++si)
			{
				if (ModSystem_IsSafeRelativePath(modScriptPaths[si]))
				{
					modScriptPaths[keptScriptCount] = modScriptPaths[si];
					keptScriptCount++;
				}
				else
				{
					Warning(eDLL_T::ENGINE, "Skipped script with unsafe path from mod '%s': '%s'\n",
						mod->name.String(), modScriptPaths[si]);
				}
			}

			if (keptScriptCount == 0)
				continue; // Mod has no loadable scripts.

			modScriptCount = keptScriptCount;
			const int newScriptCount = *pScriptCount + modScriptCount;

			// Make sure we didn't exceed it!
			if (newScriptCount > MAX_SCRIPT_FILES_TO_LOAD)
			{
				Error(s->GetNativeContext(), 0,
					"%s: Out of room appending scripts from mod '%s'(\"%s\"); max is MAX_SCRIPT_FILES_TO_LOAD = %d, got %d\n",
					__FUNCTION__, mod->name.String(), mod->id.String(), MAX_SCRIPT_FILES_TO_LOAD, newScriptCount);

				break;
			}

			// Append it.
			memcpy(&scriptArray[*pScriptCount], modScriptPaths, modScriptCount * sizeof(char*));
			*pScriptCount = newScriptCount;

			// Only set this when everything was successful, this is so that
			// SharedScript_ModSystem_RunCallbacks doesn't do unnecessary
			// work for mods that don't have scripts at all.
			mod->hasPrecompiledScripts = true;
		}
	}

	ModSystem()->UnlockModList();
}

//---------------------------------------------------------------------------------
// Purpose: parses rson data to get an array of scripts to compile 
// Input: context - 
// *scriptListPath - 
// *rson - 
// *scriptArray - 
// *pScriptCount - 
// **precompiledScriptArray - 
// precompiledScriptCount - 
//---------------------------------------------------------------------------------
bool Script_ParseScriptList(SQCONTEXT context, const char* scriptListPath,
	RSON::Node_t* rson, char** scriptArray, int* pScriptCount,
	char** precompiledScriptArray, int precompiledScriptCount)
{
	const int listed = Script_CountRsonScripts(rson, context);
	if (listed > MAX_SCRIPT_FILES_TO_LOAD)
	{
		Warning(eDLL_T::ENGINE,
			"[MOD-SCRIPT] Scripts count %d exceeds %d -- skipped native parse\n",
			listed, MAX_SCRIPT_FILES_TO_LOAD);
	}
	else
	{
		v_Script_ParseScriptList(context, scriptListPath, rson, scriptArray,
			pScriptCount, precompiledScriptArray, precompiledScriptCount);
	}

	if (ModSystem()->IsEnabled() && !s_scriptModListAppended[(int)context])
	{
		Script_AppendModScriptList(context, scriptArray, pScriptCount);
		s_scriptModListAppended[(int)context] = true;
		ScriptWraps_BeginContext((int)context);
	}

	// always returns true internally, and code never checks return value,
	// so just do the same here.
	return true;
}

//---------------------------------------------------------------------------------
// Purpose: precompiles scripts for the given VM
// Input: *vm
//---------------------------------------------------------------------------------
SQBool Script_PrecompileScripts(CSquirrelVM* vm)
{
	SQCONTEXT context = vm->GetContext();
	Msg(eDLL_T(context), "Starting script compiler...\n");

	CFastTimer timer;
	timer.Start();

	SQBool result = false;
	s_scriptModListAppended[(int)context] = false;

	switch (context)
	{
	case SQCONTEXT::SERVER:
	{
#if !defined(CLIENT_DLL)
		// Engine TraceLine is 6-arg; scripts pass entitiesOnly as 7th.
		// Re-apply immediately before compile so nothing after VM Init can
		// leave the S3 prototype in place (same class of trap as GRX_COUNT).
		Script_RegisterTraceLineEntitiesOnlyArity(vm);
#endif
		result = v_Script_PrecompileServerScripts(vm);
#if !defined(CLIENT_DLL)
		ScriptRemoteC2S_DropFnCache();
#endif
		break;
	}
	case SQCONTEXT::CLIENT:
	case SQCONTEXT::UI:
	{
		result = v_Script_PrecompileClientScripts(vm);
		break;
	}
	}

	ScriptWraps_EndContext((int)context);
	s_scriptModPrecompileListDeferred[(int)context].Reset();
	timer.End();

	Msg(eDLL_T(context), "Script compiler finished in %lf seconds\n", timer.GetDuration().GetSeconds());
	return result;
}

SQBool Script_PrecompileServerScripts(CSquirrelVM* vm)
{
#if defined(CLIENT_DLL)
	// No server VM in the client inject; g_pServerScript does not exist here.
	(void)vm;
	return SQFalse;
#else
	return Script_PrecompileScripts(g_pServerScript);
#endif
}

SQBool Script_PrecompileClientScripts(CSquirrelVM* vm)
{
	return Script_PrecompileScripts(vm);
}

//---------------------------------------------------------------------------------
// Purpose: Compiles and executes input code on target VM by context
// Input: *code - 
// context - 
//---------------------------------------------------------------------------------
void Script_Execute(const SQChar* code, const SQCONTEXT context)
{
	Assert(context != SQCONTEXT::NONE);
	Assert(ThreadInMainOrServerFrameThread());

	CSquirrelVM* s = Script_GetScriptHandle(context);
	const char* const contextName = s_scriptContextNames[(int)context];

	if (!s)
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Attempted to run %s script with no handle to VM\n", contextName);
		return;
	}

	HSQUIRRELVM v = s->GetVM();

	if (!v)
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Attempted to run %s script while VM isn't initialized\n", contextName);
		return;
	}

	if (!s->Run(code))
	{
		Error(eDLL_T::ENGINE, NO_ERROR, "Failed to run %s script \"%s\"\n", contextName, code);
		return;
	}
}

//---------------------------------------------------------------------------------
void VScript::Detour(const bool bAttach) const
{
	DetourSetup(&v_Script_LoadScriptList, &Script_LoadScriptList, bAttach);
	DetourSetup(&v_Script_LoadScriptFile, &Script_LoadScriptFile, bAttach);
	DetourSetup(&v_Script_ParseScriptList, &Script_ParseScriptList, bAttach);
	DetourSetup(&v_Script_PrecompileServerScripts, &Script_PrecompileServerScripts, bAttach);
	DetourSetup(&v_Script_PrecompileClientScripts, &Script_PrecompileClientScripts, bAttach);
}
