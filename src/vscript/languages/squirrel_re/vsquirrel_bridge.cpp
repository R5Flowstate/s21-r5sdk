//===============================================================================//
//
// Purpose: Simple VSquirrel API Bridge
//
//===============================================================================//
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vsquirrel.h"
#include "vsquirrel_bridge.h"

SQRESULT CSquirrelVMBridge::RegisterFunction(CSquirrelVM* const s, ScriptFunctionBinding_t* const binding, const bool useTypeCompiler)
{
	return s->RegisterFunction(binding, useTypeCompiler);
}

SQRESULT CSquirrelVMBridge::RegisterConstant(CSquirrelVM* const s, const SQChar* const name, const SQInteger value)
{
	return s->RegisterConstant(name, value);
}

ScriptStatus_t CSquirrelVMBridge::ExecuteFunction(CSquirrelVM* const s, HSCRIPT hFunction, const ScriptVariant_t* const pArgs, unsigned int nArgs, ScriptVariant_t* const pReturn, HSCRIPT hScope)
{
	return s->ExecuteFunction(hFunction, pArgs, nArgs, pReturn, hScope);
}

bool CSquirrelVMBridge::ExecuteCodeCallback(CSquirrelVM* const s, const SQChar* const name)
{
	return s->ExecuteCodeCallback(name);
}

bool CSquirrelVMBridge::Run(CSquirrelVM* const s, const SQChar* const script)
{
	return s->Run(script);
}

SQRESULT CSquirrelVMBridge::StackGetBool(HSQUIRRELVM v, const SQInteger idx, SQBool* const b)
{
	return sq_getbool(v, idx, b);
}

SQRESULT CSquirrelVMBridge::StackGetInteger(HSQUIRRELVM v, const SQInteger idx, SQInteger* const i)
{
	return sq_getinteger(v, idx, i);
}

SQRESULT CSquirrelVMBridge::StackGetFloat(HSQUIRRELVM v, const SQInteger idx, SQFloat* const f)
{
	return sq_getfloat(v, idx, f);
}

SQRESULT CSquirrelVMBridge::StackGetVector(HSQUIRRELVM v, const SQInteger idx, const SQVector3D** const w)
{
	return sq_getvector(v, idx, w);
}

SQRESULT CSquirrelVMBridge::StackGetThread(HSQUIRRELVM v, const SQInteger idx, HSQUIRRELVM* const thread)
{
	return sq_getthread(v, idx, thread);
}

SQRESULT CSquirrelVMBridge::StackGetString(HSQUIRRELVM v, const SQInteger idx, const SQChar** const c)
{
	return sq_getstring(v, idx, c);
}

SQRESULT CSquirrelVMBridge::StackGetObject(HSQUIRRELVM v, const SQInteger idx, HSQOBJECT* const po)
{
	return sq_getstackobj(v, idx, po);
}

void CSquirrelVMBridge::StackPushBool(HSQUIRRELVM v, const SQBool b)
{
	return sq_pushbool(v, b);
}

void CSquirrelVMBridge::StackPushInteger(HSQUIRRELVM v, const SQInteger i)
{
	return sq_pushinteger(v, i);
}

void CSquirrelVMBridge::StackPushFloat(HSQUIRRELVM v, const SQFloat f)
{
	return sq_pushfloat(v, f);
}

void CSquirrelVMBridge::StackPushVector(HSQUIRRELVM v, const SQVector3D* const w)
{
	return sq_pushvector(v, w);
}

void CSquirrelVMBridge::StackPushString(HSQUIRRELVM v, const SQChar* const string, const SQInteger len)
{
	return sq_pushstring(v, string, len);
}

void CSquirrelVMBridge::StackPushObject(HSQUIRRELVM v, HSQOBJECT obj)
{
	return sq_pushobject(v, obj);
}

void CSquirrelVMBridge::StackPushNull(HSQUIRRELVM v)
{
	return sq_pushnull(v);
}

SQInteger CSquirrelVMBridge::StackGetTop(HSQUIRRELVM v)
{
	return sq_gettop(v);
}

void CSquirrelVMBridge::StackSetTop(HSQUIRRELVM v, const SQInteger newtop)
{
	sq_settop(v, newtop);
}

void CSquirrelVMBridge::RaiseError(HSQUIRRELVM v, const SQChar* pszFormat, ...)
{
	char _stack_scratchpad[4096]; // Same size as sharedstate scratch pad.
	va_list vArgs;
	va_start(vArgs, pszFormat);

	const int ret = V_vsnprintf(_stack_scratchpad, sizeof(_stack_scratchpad), pszFormat, vArgs);

	if (ret < 0)
		_stack_scratchpad[0] = '\0';

	va_end(vArgs);
	v_SQVM_RaiseError(v, _stack_scratchpad);
}

bool CSquirrelVMBridge::ThrowError(CSquirrelVM* const s, HSQUIRRELVM v)
{
	return CSquirrelVM__ThrowError(s, v);
}

CSquirrelVMBridge g_squirrelVMBridge;

CSquirrelVMBridge* SquirrelVMBridge()
{
	return &g_squirrelVMBridge;
}
