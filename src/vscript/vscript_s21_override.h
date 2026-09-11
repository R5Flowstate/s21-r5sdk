//=============================================================================//
// Purpose: S21 PreCompileScriptFile hook + filesystem HEAD-priority override.
//=============================================================================//

#ifndef VSCRIPT_S21_OVERRIDE_H
#define VSCRIPT_S21_OVERRIDE_H

#include "thirdparty/detours/include/idetour.h"

// Third arg is a string pointer, not an int.
inline __int64 (__fastcall *v_CScriptVM_PreCompileScriptFile_S21)(
	void* scriptvm, const char* path, const char* pszId, int isCompile) = nullptr;

// S21 COM_InitFilesystem is void(mod path). S3 prologue does not match.
inline void (__fastcall *v_COM_InitFilesystem_S21)(const char* pFullModPath) = nullptr;

///////////////////////////////////////////////////////////////////////////////
class VScriptS21Override : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("CScriptVM::PreCompileScriptFile_S21", v_CScriptVM_PreCompileScriptFile_S21);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC 90 01 00 00 44 0F B6 51 60")
			.GetPtr(v_CScriptVM_PreCompileScriptFile_S21);
	}
	virtual void GetVar(void) const {}
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};

///////////////////////////////////////////////////////////////////////////////
class VPlatformFSOverrideS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("COM_InitFilesystem_S21", v_COM_InitFilesystem_S21);
	}
	virtual void GetFun(void) const
	{
		// S3 COM_InitFilesystem prologue n=0 on S21; do not reuse that pattern.
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 33 C0")
			.GetPtr(v_COM_InitFilesystem_S21);
	}
	virtual void GetVar(void) const {}
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

typedef bool (__fastcall *FS_AsyncReadScript_fn)(
	void* iface, const char* path, void* flags, void* outBuf);
extern FS_AsyncReadScript_fn v_FS_AsyncReadScript_S21;

void InstallScriptDiskRedirectHooks_S21(void);
void RemoveScriptDiskRedirectHooks_S21(void);
void ScriptRson_ResolveTeardownPatch_S21(void);
void ScriptRson_InstallTeardownPatch_S21(const bool bAttach);

// Reset late-registration flags on next VM init (map change).
void ResetLateNativeRegistration_S21(uint8_t vmType);

// RSON-from-file used for scripts/vscripts/scripts.rson(.client/.ui).
inline char (__fastcall *v_RSON_LoadFileFromPath_S21)(
	void* out, const char* path, __int64 a3, __int64 a4, void* alloc, __int64* a6) = nullptr;

class VFSScriptRedirectS21 : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("FS_AsyncReadScript_S21", v_FS_AsyncReadScript_S21);
		LogFunAdr("RSON_LoadFileFromPath_S21", v_RSON_LoadFileFromPath_S21);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 4C 8B 3D")
			.GetPtr(v_RSON_LoadFileFromPath_S21);
		// Slot 15 is resolved after COM_InitFilesystem (runtime vtable).
		ScriptRson_ResolveTeardownPatch_S21();
	}
	virtual void GetVar(void) const {}
	virtual void GetCon(void) const {}
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////

#endif // VSCRIPT_S21_OVERRIDE_H
