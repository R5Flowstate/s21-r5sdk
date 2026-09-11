#pragma once
#include "ipluginsystem.h"

#define PLUGIN_INSTALL_DIR "bin\\x64_retail\\plugins"

class CModAppSystemGroup;
class CServer;
class CClient;
class CPlayer;
class CSquirrelVM;
struct user_creds_s;

template<typename T>
class CPluginCallbackList
{
public:
	CPluginCallbackList() {}
	CPluginCallbackList(const CUtlVector<T>& cbs)
	{
		for (auto it : cbs)
			m_vCallbacks.AddToTail(it);
	}

	CUtlVector<T>& GetCallbacks() { return m_vCallbacks; }

	operator bool()
	{
		return !this->m_vCallbacks.IsEmpty();
	}

	CUtlVector<T>& operator!()
	{
		return this->m_vCallbacks;
	}

	CPluginCallbackList<T>& operator+=(const T& rhs)
	{
		if (rhs)
			this->m_vCallbacks.AddToTail(rhs);

		return *this;
	}

	CPluginCallbackList<T>& operator+=(const CUtlVector<T>& rhs)
	{
		for (auto it : rhs)
		{
			if (it)
				this->m_vCallbacks.AddToTail(it);
		}

		return *this;
	}

	CPluginCallbackList<T>& operator-=(const T& rhs)
	{
		if (rhs)
		{
			const int fnd = m_vCallbacks.Find(rhs);

			if (fnd != m_vCallbacks.InvalidIndex())
				m_vCallbacks.Remove(fnd);
		}

		return *this;
	}

	CPluginCallbackList<T>& operator-=(const CUtlVector<T>& rhs)
	{
		for (auto itc : rhs)
		{
			if (itc) {
				const int fnd = m_vCallbacks.Find(itc);

				if (fnd != m_vCallbacks.InvalidIndex())
					m_vCallbacks.Remove(fnd);
			}
		}

		return *this;
	}

private:
	CUtlVector<T> m_vCallbacks;
};

template<typename T>
class CPluginCallback
{
	friend class CPluginSystem;
public:
	CPluginCallback(T f) : function(f) {};

	inline const T& Function() { return function; };
	inline const wchar_t* ModuleName() { return moduleName; };

	operator bool() const
	{
		return function;
	}

protected:
	inline void SetModuleName(wchar_t* name)
	{
		wcscpy_s(moduleName, name);
	};

private:
	T function;
	wchar_t moduleName[MAX_OSPATH];
};

class CPluginSystem : public IPluginSystem
{
public:
	struct PluginInstance_t
	{
		PluginInstance_t(const char* pName, const char* pPath, const char* pDescription = "")
			: name(pName)
			, path(pPath)
			, description(pDescription)
			, isLoaded(false)
		{
		};

		// Might wanna make a status code system.
		typedef bool(*OnLoad)(const char*, const char*);
		typedef bool(*OnUnload)();

		CModule moduleHandle;
		CUtlString name;
		CUtlString path;
		CUtlString description;
		bool isLoaded; // [ PIXIE ]: I don't like this and it's bad.
		// I will make a module manager later which will grab all modules from the process and adds each module / removes module that passes through DLLMain.
	};

	void Init();
	void Shutdown();

	bool LoadInstance(PluginInstance_t& pluginInst);
	bool UnloadInstance(PluginInstance_t& pluginInst);

	void InstallCallback(PluginOperation_s* const pio);
	void RemoveCallback(PluginOperation_s* const pio);

	CUtlVector<PluginInstance_t>& GetInstances();

	// Implements IPluginSystem
	virtual void* RunOperation(PluginOperation_s* const pio);
	virtual void CoreMsgV(LogType_t logType, LogLevel_t logLevel, eDLL_T context,
		const char* pszLogger, const char* pszFormat, va_list args, const UINT exitCode, const char* pszUptimeOverride);

#define CREATE_PLUGIN_CALLBACK(typeName, type, funcName, varName) public: using typeName = type; CPluginCallbackList<CPluginCallback<typeName>>& funcName() { return varName; } private: CPluginCallbackList<CPluginCallback<typeName>> varName;

	CREATE_PLUGIN_CALLBACK(OnCreateFn, bool(*)(CModAppSystemGroup*), GetCreateCallbacks, createCallbacks);
	CREATE_PLUGIN_CALLBACK(OnDestroyFn, bool(*)(CModAppSystemGroup*), GetDestroyCallbacks, destroyCallbacks);

	CREATE_PLUGIN_CALLBACK(OnConnectClientFn, bool(*)(CServer*, CClient*, user_creds_s*), GetConnectClientCallbacks, connectClientCallbacks);
	CREATE_PLUGIN_CALLBACK(OnChatMessageFn, bool(*)(CPlayer*, const char*, bool), GetChatMessageCallbacks, chatMessageCallbacks);

	CREATE_PLUGIN_CALLBACK(OnRegisterSharedScriptFunctionsFn, void(*)(CSquirrelVM*), GetRegisterSharedScriptFuncsCallbacks, registerSharedScriptFuncsCallbacks);
	CREATE_PLUGIN_CALLBACK(OnRegisterServerScriptFunctionsFn, void(*)(CSquirrelVM*), GetRegisterServerScriptFuncsCallbacks, registerServerScriptFuncsCallbacks);
	CREATE_PLUGIN_CALLBACK(OnRegisterClientScriptFunctionsFn, void(*)(CSquirrelVM*), GetRegisterClientScriptFuncsCallbacks, registerClientScriptFuncsCallbacks);
	CREATE_PLUGIN_CALLBACK(OnRegisterUIScriptFunctionsFn, void(*)(CSquirrelVM*), GetRegisterUIScriptFuncsCallbacks, registerUIScriptFuncsCallbacks);

#undef CREATE_PLUGIN_CALLBACK

private:
	CUtlVector<PluginInstance_t> m_Instances;
};
extern CPluginSystem g_PluginSystem;

FORCEINLINE CPluginSystem* PluginSystem()
{
	return &g_PluginSystem;
}

// Monitor this and performance profile this if fps drops are detected.
#define CALL_PLUGIN_CALLBACKS(callback, ...)      \
	for (auto& cb : !callback)                    \
		cb.Function()(__VA_ARGS__)                
