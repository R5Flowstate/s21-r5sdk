#ifndef VSCRIPT_SCRIPT_WRAPS_H
#define VSCRIPT_SCRIPT_WRAPS_H

// Mod script wraps: at compile time the base definition of a wrapped function
// is renamed and a same-signature chain is spliced onto its header line, so
// every caller reaches the mods' functions without the base file changing.

// Collects the enabled mods' wraps for one VM. Called once the VM's compile
// list is final and before any file of it compiles.
void ScriptWraps_BeginContext(const int context);
// Reports every chain that never found its target and frees rewritten sources.
void ScriptWraps_EndContext(const int context);
// Frees the rewritten sources once the file loader has compiled them.
void ScriptWraps_ReleaseSources(void);

// Returns a malloc'd rewritten copy of the source, or nullptr when no wrapped
// definition is in it. Line numbers are preserved.
char* ScriptWraps_RewriteSource(const int context, const char* const pszSource,
	const size_t nSourceLen, const char* const pszSourceName, size_t* const pOutLen);

#if !defined(CLIENT_DLL)
#include "thirdparty/detours/include/idetour.h"

///////////////////////////////////////////////////////////////////////////////
class VScriptWraps : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
#endif // !CLIENT_DLL

#endif // VSCRIPT_SCRIPT_WRAPS_H
