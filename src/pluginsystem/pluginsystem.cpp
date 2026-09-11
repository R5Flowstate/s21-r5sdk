//=============================================================================//
//
// Purpose: plugin system that manages plugins!
// 
//-----------------------------------------------------------------------------
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/commandline.h"
#include "tier2/fileutils.h"
#include "filesystem/filesystem.h"
#include "pluginsystem.h"

//-----------------------------------------------------------------------------
// Console variables
//-----------------------------------------------------------------------------
static ConVar pluginsystem_debug("pluginsystem_debug", "0", FCVAR_RELEASE, "Debug the pluginsystem");

//-----------------------------------------------------------------------------
// Purpose: resolve this install's plugin directory once, exe-relative. Anything
//          enumerated outside it must not reach LoadLibraryA (mod basePaths
//          mount into the GAME pathID AddFilesToList walks).
//-----------------------------------------------------------------------------
static void PluginSystem_GetInstallPluginRoot(char* pOut, const size_t nOutLen)
{
	pOut[0] = '\0';

	char exePath[MAX_PATH] = {};
	if (!GetModuleFileNameA(NULL, exePath, MAX_PATH))
		return;

	char* pSlash = strrchr(exePath, '\\');
	const char* pFwd = strrchr(exePath, '/');
	if (pFwd && (!pSlash || pFwd > pSlash))
		pSlash = const_cast<char*>(pFwd);

	if (!pSlash)
		return;

	*pSlash = '\0';
	Q_snprintf(pOut, nOutLen, "%s\\" PLUGIN_INSTALL_DIR, exePath);
	V_strlower(pOut);
}

//-----------------------------------------------------------------------------
// Purpose: initialize the plugin system
// Input 
//-----------------------------------------------------------------------------
void CPluginSystem::Init()
{
	if (CommandLine()->CheckParm("-pluginsystem_disable"))
		return;

	if (!FileSystem()->IsDirectory(PLUGIN_INSTALL_DIR, "GAME"))
		return; // No plugins to load.

	// plugin system initializes before the first Cbuf_Execute call, which
	// executes commands/convars over the command line. we check for an
	// explicit pluginsystem debug flag, and set the convar from here.
	if (CommandLine()->CheckParm("-pluginsystem_debug"))
		pluginsystem_debug.SetValue(true);

	CUtlVector< CUtlString > pluginPaths;
	AddFilesToList(pluginPaths, PLUGIN_INSTALL_DIR, "dll", "GAME");

	char szInstallRoot[MAX_PATH] = {};
	PluginSystem_GetInstallPluginRoot(szInstallRoot, sizeof(szInstallRoot));
	const size_t nRootLen = Q_strlen(szInstallRoot);

	for (int i = 0; i < pluginPaths.Count(); ++i)
	{
		CUtlString& path = pluginPaths[i];

		// Fail closed: unresolvable install root loads nothing. The boundary
		// char must be the separator so 'plugins_foo' cannot prefix-match.
		if (nRootLen == 0 ||
			Q_strnicmp(path.String(), szInstallRoot, nRootLen) != 0 ||
			(path.String()[nRootLen] != '\0' && path.String()[nRootLen] != '\\'))
		{
			Warning(eDLL_T::ENGINE, "Skipping plugin outside install dir: '%s'\n", path.String());
			continue;
		}

		bool addInstance = true;

		FOR_EACH_VEC(m_Instances, j)
		{
			const PluginInstance_t& instance = m_Instances[j];

			if (instance.path.IsEqual_CaseInsensitive(path.String()) == 0)
			{
				addInstance = false; // Already exists.
				break;
			}
		}

		if (addInstance)
		{
			const char* baseFileName = V_UnqualifiedFileName(path.String());
			m_Instances.AddToTail(PluginInstance_t(baseFileName, path.String()));
		}
	}

	FOR_EACH_VEC(m_Instances, i)
	{
		CPluginSystem::PluginInstance_t& inst = m_Instances[i];

		if (LoadInstance(inst))
		{
			if (pluginsystem_debug.GetBool())
				Msg(eDLL_T::ENGINE, "Loaded plugin: '%s'\n", inst.name.String());
		}
		else
			Error(eDLL_T::ENGINE, 0, "Failed loading plugin: '%s'\n", inst.name.String());
	}
}

//-----------------------------------------------------------------------------
// Purpose: shutdown the plugin system
// Input 
//-----------------------------------------------------------------------------
void CPluginSystem::Shutdown()
{
	FOR_EACH_VEC_BACK(m_Instances, i)
	{
		CPluginSystem::PluginInstance_t& inst = m_Instances[i];

		if (UnloadInstance(inst))
		{
			if (pluginsystem_debug.GetBool())
				Msg(eDLL_T::ENGINE, "Unloaded plugin: '%s'\n", inst.name.String());
		}
		else
			Error(eDLL_T::ENGINE, 0, "Failed unloading plugin: '%s'\n", inst.name.String());
	}

	m_Instances.Purge();
}

//-----------------------------------------------------------------------------
// Purpose: load a plugin instance
// Input: pluginInst* -
// Output: bool
//-----------------------------------------------------------------------------
bool CPluginSystem::LoadInstance(PluginInstance_t& pluginInst)
{
	if (pluginInst.isLoaded)
		return false;

	HMODULE loadedPlugin = LoadLibraryA(pluginInst.path.String());

	if (loadedPlugin == INVALID_HANDLE_VALUE || loadedPlugin == 0)
		return false;

	CModule pluginModule(pluginInst.name.String());

	// Pass the SDK module name; listen/dedi/client DLLs are named differently.
	PluginInstance_t::OnLoad onLoadFn = pluginModule.GetExportedSymbol(
		"PluginInstance_OnLoad").RCast<PluginInstance_t::OnLoad>();

	Assert(onLoadFn);

	if (!onLoadFn || !onLoadFn(pluginInst.name.String(), g_SDKDll.GetModuleName().c_str()))
	{
		FreeLibrary(loadedPlugin);
		return false;
	}

	pluginInst.moduleHandle = pluginModule;
	return pluginInst.isLoaded = true;
}

//-----------------------------------------------------------------------------
// Purpose: unload a plugin instance
// Input: pluginInst* -
// Output: bool
//-----------------------------------------------------------------------------
bool CPluginSystem::UnloadInstance(PluginInstance_t& pluginInst)
{
	if (!pluginInst.isLoaded)
		return false;

	PluginInstance_t::OnUnload onUnloadFn = 
		pluginInst.moduleHandle.GetExportedSymbol(
		"PluginInstance_OnUnload").RCast<PluginInstance_t::OnUnload>();

	Assert(onUnloadFn);
	bool unloadOk = false;

	if (onUnloadFn)
		unloadOk = onUnloadFn();

	const bool freeLibraryOk = FreeLibrary((HMODULE)pluginInst.moduleHandle.GetModuleBase());
	Assert(freeLibraryOk);

	pluginInst.isLoaded = false;
	return unloadOk && freeLibraryOk;
}

//-----------------------------------------------------------------------------
// Purpose: get all plugin instances
// Input 
// Output: CUtlVector<CPluginSystem::PluginInstance>&
//-----------------------------------------------------------------------------
CUtlVector<CPluginSystem::PluginInstance_t>& CPluginSystem::GetInstances()
{
	return m_Instances;
}

//-----------------------------------------------------------------------------
// Purpose: install plugin callback for function
// Input: *help
//-----------------------------------------------------------------------------
void CPluginSystem::InstallCallback(PluginOperation_s* const pio)
{
#define ADD_PLUGIN_CALLBACK(fn, callback, function) callback += reinterpret_cast<fn>(function); callback.GetCallbacks().Tail().SetModuleName(moduleName)

	if (!pio->function)
	{
		Assert(0);
		return;
	}

	// [rexx]: This fetches the path to the module that contains the requested callback function.
	// The module name is fetched so that callbacks can be identified by the plugin that they came from.
	// This must use the wide-char version of this func, as file paths may contain non-ASCII characters and we don't really want those to break.
	wchar_t moduleName[MAX_OSPATH];

	if (!GetMappedFileNameW((HANDLE)-1, pio->function, moduleName, MAX_OSPATH))
	{
		Assert(0);
		return;
	}

	switch (pio->callbackId)
	{
	case PluginOperation_s::PluginCallback_e::CModAppSystemGroup_Create:
	{
		ADD_PLUGIN_CALLBACK(OnCreateFn, GetCreateCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::CModAppSystemGroup_Destroy:
	{
		ADD_PLUGIN_CALLBACK(OnDestroyFn, GetDestroyCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::CServer_ConnectClient:
	{
		ADD_PLUGIN_CALLBACK(OnConnectClientFn, GetConnectClientCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnReceivedChatMessage:
	{
		ADD_PLUGIN_CALLBACK(OnChatMessageFn, GetChatMessageCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnRegisterSharedScriptFunctions:
	{
		ADD_PLUGIN_CALLBACK(OnRegisterSharedScriptFunctionsFn, GetRegisterSharedScriptFuncsCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnRegisterServerScriptFunctions:
	{
		ADD_PLUGIN_CALLBACK(OnRegisterServerScriptFunctionsFn, GetRegisterServerScriptFuncsCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnRegisterClientScriptFunctions:
	{
		ADD_PLUGIN_CALLBACK(OnRegisterClientScriptFunctionsFn, GetRegisterClientScriptFuncsCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnRegisterUIScriptFunctions:
	{
		ADD_PLUGIN_CALLBACK(OnRegisterUIScriptFunctionsFn, GetRegisterUIScriptFuncsCallbacks(), pio->function);
		break;
	}
	default:
		Assert(0); // Unimplemented.
		break;
	}

#undef ADD_PLUGIN_CALLBACK
}

//-----------------------------------------------------------------------------
// Purpose: remove plugin callback for function
// Input: *help
//-----------------------------------------------------------------------------
void CPluginSystem::RemoveCallback(PluginOperation_s* const pio)
{
#define REMOVE_PLUGIN_CALLBACK(fn, callback, function) callback -= reinterpret_cast<fn>(function)

	if (!pio->function)
		return;

	switch (pio->callbackId)
	{
	case PluginOperation_s::PluginCallback_e::CModAppSystemGroup_Create:
	{
		REMOVE_PLUGIN_CALLBACK(OnCreateFn, GetCreateCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::CModAppSystemGroup_Destroy:
	{
		REMOVE_PLUGIN_CALLBACK(OnDestroyFn, GetDestroyCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::CServer_ConnectClient:
	{
		REMOVE_PLUGIN_CALLBACK(OnConnectClientFn, GetConnectClientCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnReceivedChatMessage:
	{
		REMOVE_PLUGIN_CALLBACK(OnChatMessageFn, GetChatMessageCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnRegisterSharedScriptFunctions:
	{
		REMOVE_PLUGIN_CALLBACK(OnRegisterSharedScriptFunctionsFn, GetRegisterSharedScriptFuncsCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnRegisterServerScriptFunctions:
	{
		REMOVE_PLUGIN_CALLBACK(OnRegisterServerScriptFunctionsFn, GetRegisterServerScriptFuncsCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnRegisterClientScriptFunctions:
	{
		REMOVE_PLUGIN_CALLBACK(OnRegisterClientScriptFunctionsFn, GetRegisterClientScriptFuncsCallbacks(), pio->function);
		break;
	}
	case PluginOperation_s::PluginCallback_e::OnRegisterUIScriptFunctions:
	{
		REMOVE_PLUGIN_CALLBACK(OnRegisterUIScriptFunctionsFn, GetRegisterUIScriptFuncsCallbacks(), pio->function);
		break;
	}
	default:
		Assert(0); // Unimplemented.
		break;
	}

#undef REMOVE_PLUGIN_CALLBACK
}

//-----------------------------------------------------------------------------
// Purpose
// Input: *pio
// Output: void*
//-----------------------------------------------------------------------------
void* CPluginSystem::RunOperation(PluginOperation_s* const pio)
{
	switch (pio->commandId)
	{
	case PluginOperation_s::PluginCommand_e::PLUGIN_INSTALL_CALLBACK:
	{
		InstallCallback(pio);
		break;
	}
	case PluginOperation_s::PluginCommand_e::PLUGIN_REMOVE_CALLBACK:
	{
		RemoveCallback(pio);
		break;
	}
	default:
		Assert(0); // Unimplemented.
		break;
	}

	return nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: returns the caller module by return address
// Input: *pluginSystem - 
// returnAddress - 
// Output: const char*
//-----------------------------------------------------------------------------
static const char* PluginSystem_GetCallerModuleName(CPluginSystem* const pluginSystem, const QWORD returnAddress)
{
	FOR_EACH_VEC(pluginSystem->GetInstances(), i)
	{
		const CPluginSystem::PluginInstance_t& inst = pluginSystem->GetInstances()[i];

		const QWORD base = inst.moduleHandle.GetModuleBase();
		const DWORD size = inst.moduleHandle.GetModuleSize();

		if (returnAddress >= base && returnAddress < (base + size))
			return inst.name.String();
	}

	Assert(0);
	return "!!! UNKNOWN PLUGIN !!!";
}

//-----------------------------------------------------------------------------
// NOTE: these should only be used from within plugins!
//-----------------------------------------------------------------------------
void CPluginSystem::CoreMsgV(LogType_t logType, LogLevel_t logLevel, eDLL_T context,
	const char* pszLogger, const char* pszFormat, va_list args, const UINT exitCode, const char* pszUptimeOverride)
{
	// Prepend the plugin name.
	const QWORD returnAddress = (QWORD)_ReturnAddress();
	const char* const pluginName = PluginSystem_GetCallerModuleName(this, returnAddress);

	const string formatted = FormatV(pszFormat, args);
	CoreMsg(logType, logLevel, context, exitCode, pszLogger, "[%s] %s", pluginName, formatted.c_str());
}

CPluginSystem g_PluginSystem;
