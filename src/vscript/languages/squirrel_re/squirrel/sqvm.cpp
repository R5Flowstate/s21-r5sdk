//=============================================================================//
//
// Purpose: Squirrel VM
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/platform_internal.h"
#include "tier0/commandline.h"
#ifndef CLIENT_DLL
#include "engine/server/sv_rcon.h"
#endif // CLIENT_DLL
#include "engine/client/cdll_engine_int.h"
#include "vgui/vgui_debugpanel.h"
#include "gameui/IConsole.h"
#include "squirrel.h"
#include "sqvm.h"
#include "sqstate.h"
#include "sqstdaux.h"

//---------------------------------------------------------------------------------
// Purpose: prints the compile error and context to the console
// Input: *sqvm - 
// *pszError - 
// *pszFile - 
// nLine - 
// nColumn - 
//---------------------------------------------------------------------------------
void SQVM_CompileError(HSQUIRRELVM v, const SQChar* pszError, const SQChar* pszFile, SQUnsignedInteger nLine, SQInteger nColumn)
{
	static char szContextBuf[256]{};
	szContextBuf[0] = '\0';
	if (v_SQVM_GetErrorLine)
		v_SQVM_GetErrorLine(pszFile, nLine, szContextBuf, sizeof(szContextBuf) - 1);

	const eDLL_T context = v->GetNativeContext();
	Error(context, NO_ERROR, "%s SCRIPT COMPILE ERROR: %s\n", v->GetContextName(), pszError);
	Error(context, NO_ERROR, " -> %s\n\n", szContextBuf);
	Error(context, NO_ERROR, "%s line [%d] column [%d]\n", pszFile, nLine, nColumn);
}

//---------------------------------------------------------------------------------
// Purpose: prints the logic error and context to the console
// Input: bPrompt - 
//---------------------------------------------------------------------------------
void SQVM_LogicError(SQBool bPrompt)
{
	if ((*g_flErrorTimeStamp) > 0.0 && (bPrompt || Plat_FloatTime() > (*g_flErrorTimeStamp) + 0.0))
	{
		g_bSQAuxBadLogic = true;
	}
	else
	{
		g_bSQAuxBadLogic = false;
		g_pErrorVM = nullptr;
	}
	v_SQVM_LogicError(bPrompt);
}

void SQVM::Pop() {
	_stack[--_top] = _null_;
}

void SQVM::Pop(SQInteger n) {
	for (SQInteger i = 0; i < n; i++) {
		_stack[--_top] = _null_;
	}
}

void SQVM::Push(const SQObjectPtr& o) { _stack[_top++] = o; }
void SQVM::PushNull() { Push(_null_); }
SQObjectPtr& SQVM::Top() { return _stack[_top - 1]; }
SQObjectPtr& SQVM::PopGet() { return _stack[--_top]; }
SQObjectPtr& SQVM::GetUp(SQInteger n) { return _stack[_top + n]; }
SQObjectPtr& SQVM::GetAt(SQInteger n) { return _stack[n]; }

#include "vscript/languages/squirrel_re/vsquirrel.h"
CSquirrelVM* SQVM::GetScriptVM()
{
	return _sharedstate->GetScriptVM();
}

SQChar* SQVM::GetContextName()
{
	return _sharedstate->_contextname;
}

SQCONTEXT SQVM::GetContext()
{
	return GetScriptVM()->GetContext();
}

eDLL_T SQVM::GetNativeContext()
{
	return (eDLL_T)GetContext();
}

//---------------------------------------------------------------------------------
void VSquirrelVM::Detour(const bool bAttach) const
{
	DetourSetup(&v_SQVM_CompileError, &SQVM_CompileError, bAttach);
	DetourSetup(&v_SQVM_LogicError, &SQVM_LogicError, bAttach);
}
