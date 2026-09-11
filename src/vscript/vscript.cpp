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
#if !defined(CLIENT_DLL)
#include "game/server/vscript_server.h" // Script_RegisterTraceLineEntitiesOnlyArity
#include "game/shared/scriptremotefunctions_server.h"
#endif

static const char* s_scriptContextNames[] = { "SERVER", "CLIENT", "UI" };

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
	///////////////////////////////////////////////////////////////////////////////
	return v_Script_LoadScriptFile(s, path, name, flags);
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
	v_Script_ParseScriptList(context, scriptListPath, rson, scriptArray,
		pScriptCount, precompiledScriptArray, precompiledScriptCount);

	if (ModSystem()->IsEnabled() && !s_scriptModListAppended[(int)context])
	{
		Script_AppendModScriptList(context, scriptArray, pScriptCount);
		s_scriptModListAppended[(int)context] = true;
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
