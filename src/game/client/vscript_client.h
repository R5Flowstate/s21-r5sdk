#if defined(CLIENT_DLL)
#ifndef VSCRIPT_CLIENT_H
#define VSCRIPT_CLIENT_H

// S21 late-reg: expose netchannel ping to UI (not bulk Script_RegisterUIFunctions).
void Script_RegisterConnectionPingUI(CSquirrelVM* s);
void Script_RegisterPlatformIdentityUI(CSquirrelVM* s);

// S21 late-reg: server-browser natives for the lobby Servers panel.
void Script_RegisterServerBrowserUI(CSquirrelVM* s);

// S21 late-reg: per-sender chat mute natives on the CLIENT VM.
void Script_RegisterChatMuteClient(CSquirrelVM* s);

#define DEFINE_CLIENT_SCRIPTFUNC_NAMED(s, functionName, helpString, returnType, parameters, isVariadic, ...) \
	Script_RegisterFuncNamed(s, MKSTRING(functionName), MKSTRING(Client_Script_##functionName),              \
	helpString, returnType, parameters, isVariadic, ClientScript_##functionName, __VA_ARGS__)                \

#define DEFINE_UI_SCRIPTFUNC_NAMED(s, functionName, helpString, returnType, parameters, isVariadic, ...) \
	Script_RegisterFuncNamed(s, MKSTRING(functionName), MKSTRING(UI_Script_##functionName),              \
	helpString, returnType, parameters, isVariadic, UIScript_##functionName, __VA_ARGS__)                \

#endif // VSCRIPT_CLIENT_H
#else // !CLIENT_DLL
#ifndef VSCRIPT_CLIENT_H
#define VSCRIPT_CLIENT_H

void Script_RegisterClientFunctions(CSquirrelVM* s);
void Script_RegisterUIFunctions(CSquirrelVM* s);
void Script_RegisterUIServerFunctions(CSquirrelVM* s);
void Script_RegisterCoreClientFunctions(CSquirrelVM* s);

#define DEFINE_CLIENT_SCRIPTFUNC_NAMED(s, functionName, helpString, returnType, parameters, isVariadic, ...) \
	Script_RegisterFuncNamed(s, MKSTRING(functionName), MKSTRING(Client_Script_##functionName),              \
	helpString, returnType, parameters, isVariadic, ClientScript_##functionName, __VA_ARGS__)                \

#define DEFINE_UI_SCRIPTFUNC_NAMED(s, functionName, helpString, returnType, parameters, isVariadic, ...) \
	Script_RegisterFuncNamed(s, MKSTRING(functionName), MKSTRING(UI_Script_##functionName),              \
	helpString, returnType, parameters, isVariadic, UIScript_##functionName, __VA_ARGS__)                \

inline SQRESULT(*v_ClientScript_DebugScreenText)(HSQUIRRELVM v);
inline SQRESULT(*v_ClientScript_DebugScreenTextWithColor)(HSQUIRRELVM v);


// WeaponInfoFileKeyField native handlers - engine functions used by CLIENT/SERVER VMs
// Reused for UI VM since they only access global weapon data tables (no entity context needed)

// RegisterScriptClass: creates a Squirrel class type from a ScriptClassDescriptor_t
// Used by the engine during CLIENT/SERVER VM init to register entity/player/weapon classes
// Parameters: (HSQUIRRELVM, className, shortName, descriptor, parentDescriptor)
inline __int64 (*v_RegisterScriptClass)(HSQUIRRELVM v, const char* className,
	const char* shortName, ScriptClassDescriptor_t* descriptor,
	ScriptClassDescriptor_t* parentDescriptor);

inline void (*v_Script_RegisterClientEntityClassFuncs)();
inline void (*v_Script_RegisterClientPlayerClassFuncs)();
inline void (*v_Script_RegisterClientCombatCharacterClassFuncs)();
inline void (*v_Script_RegisterClientAIClassFuncs)();
inline void (*v_Script_RegisterClientWeaponClassFuncs)();
inline void (*v_Script_RegisterClientProjectileClassFuncs)();
inline void (*v_Script_RegisterClientTitanSoulClassFuncs)();
inline void (*v_Script_RegisterClientPlayerDecoyClassFuncs)();
inline void (*v_Script_RegisterClientFirstPersonProxyClassFuncs)();



#endif // VSCRIPT_CLIENT_H
#endif // CLIENT_DLL
