#pragma once
#include "squirrel.h"
#include "sqstate.h"
#include "sqobject.h"

class CSquirrelVM;

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
enum class SQCONTEXT : SQInteger
{
	SERVER = 0,
	CLIENT,
	UI,

	// The enums below are not a context.
	COUNT,
	NONE = COUNT
};

struct SQVM : public CHAINABLE_OBJ
{
	void PrintObjVal(const SQObject* oin, SQObject* oout);

	void Pop();
	void Pop(SQInteger n);

	// push sqobjectptr on to the stack
	void Push(const SQObjectPtr& o);
	void PushNull();

	SQObjectPtr& Top();
	SQObjectPtr& PopGet();
	SQObjectPtr& GetUp(SQInteger n);
	SQObjectPtr& GetAt(SQInteger n);

	CSquirrelVM* GetScriptVM();
	SQChar* GetContextName();
	SQCONTEXT GetContext();
	eDLL_T GetNativeContext();

	// ================================= //
	_BYTE gap1C[8];
	void* _callstack;
	SQInteger _stacklevel;
	SQInteger _bottom;
	SQObjectPtr* _stackbase;
	SQSharedState* _sharedstate;
	char gap68[16];
	SQInteger _top;
	sqvector<SQObjectPtr> _stack;
	char gap_98[24];
	SQObjectPtr temp_reg;
	char gap_C8[32];
	SQObjectPtr _roottable;
	SQObjectPtr _lasterror;
	char gap_100[48];
	SQInteger _nnativecalls;
	SQBool _suspended;
	SQBool _suspended_root;
	SQInteger _callsstacksize;
	SQInteger _alloccallsstacksize;
	SQInteger suspended_traps;
};
static_assert(offsetof(SQVM, _top) == 0x78);
static_assert(offsetof(SQVM, _nnativecalls) == 0x130);

#if defined(CLIENT_DLL)
// S21: _stackbase +0x48, _stackdata +0x78, _top +0x68 (32-bit, not SQInteger), _bottom +0x44.
static const size_t SQVM_S21_STACKBASE = 0x48;
static const size_t SQVM_S21_STACKDATA = 0x78;
static const size_t SQVM_S21_TOP       = 0x68;
// The engine inlines gettop as (_top - _bottom): "mov eax,[rcx+68h]; sub eax,[rcx+44h]".
static const size_t SQVM_S21_BOTTOM    = 0x44;
// Read as a 16-byte object by the engine's own sq_pushroottable, which tests the
// type at +0xD0 and takes the value from +0xD8.
static const size_t SQVM_S21_ROOTTABLE = 0xD0;
// S21 _sharedstate at +0x50, not the S3 field in SQVM above.
static const size_t SQVM_S21_SHAREDSTATE = 0x50;

inline SQObjectPtr* SQVM_S21_Field(HSQUIRRELVM v, size_t offset)
{
	return *reinterpret_cast<SQObjectPtr**>(reinterpret_cast<unsigned char*>(v) + offset);
}

inline SQSharedState* SQVM_S21_SharedState(HSQUIRRELVM v)
{
	return *reinterpret_cast<SQSharedState**>(
		reinterpret_cast<unsigned char*>(v) + SQVM_S21_SHAREDSTATE);
}

inline int& SQVM_S21_Top(HSQUIRRELVM v)
{
	return *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(v) + SQVM_S21_TOP);
}

inline int& SQVM_S21_Bottom(HSQUIRRELVM v)
{
	return *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(v) + SQVM_S21_BOTTOM);
}

inline SQObjectPtr& stack_get(HSQUIRRELVM v, SQInteger idx)
{
	if (idx >= 0)
		return SQVM_S21_Field(v, SQVM_S21_STACKBASE)[idx - 1];

	return SQVM_S21_Field(v, SQVM_S21_STACKDATA)[SQVM_S21_Top(v) + idx];
}

// Push/Pop use _stack[_top] via the S21 fields, not the S3 SQVM members.
inline void SQVM_S21_Push(HSQUIRRELVM v, const SQObjectPtr& o)
{
	SQVM_S21_Field(v, SQVM_S21_STACKDATA)[SQVM_S21_Top(v)++] = o;
}

inline SQObjectPtr& SQVM_S21_RootTable(HSQUIRRELVM v)
{
	return *reinterpret_cast<SQObjectPtr*>(reinterpret_cast<unsigned char*>(v) + SQVM_S21_ROOTTABLE);
}

inline void SQVM_S21_Pop(HSQUIRRELVM v, SQInteger count)
{
	SQObjectPtr* vals = SQVM_S21_Field(v, SQVM_S21_STACKDATA);
	int& top = SQVM_S21_Top(v);

	while (count-- > 0 && top > 0)
		vals[--top] = _null_;
}
#else
// The engine inlines gettop as (_top - _bottom): "mov eax,[rcx+78h]; sub eax,[rcx+54h]".
// Both are 32-bit. The SQVM members above declare them as SQInteger, and _bottom
// lands at 0x50, so reading them through the struct spans the wrong bytes.
static const size_t SQVM_S3_TOP    = 0x78;
static const size_t SQVM_S3_BOTTOM = 0x54;

inline int& SQVM_S3_Top(HSQUIRRELVM v)
{
	return *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(v) + SQVM_S3_TOP);
}

inline int& SQVM_S3_Bottom(HSQUIRRELVM v)
{
	return *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(v) + SQVM_S3_BOTTOM);
}

inline SQObjectPtr& stack_get(HSQUIRRELVM v, SQInteger idx) { return ((idx >= 0) ? (v->_stackbase[idx-1]) : (v->GetUp(idx))); }
#endif
#define _ss(_vm_) (_vm_)->_sharedstate

/* ==== SQUIRREL ======================================================================================================================================================== */
inline size_t(*v_SQVM_GetErrorLine)(const SQChar* pszFile, SQInteger nLine, SQChar* pszContextBuf, SQInteger nBufLen);
inline SQRESULT(*v_SQVM_WarningCmd)(HSQUIRRELVM v, SQInteger a2);
inline void(*v_SQVM_CompileError)(HSQUIRRELVM v, const SQChar* pszError, const SQChar* pszFile, SQUnsignedInteger nLine, SQInteger nColumn);
inline void(*v_SQVM_LogicError)(SQBool bPrompt);
inline SQInteger(*v_SQVM_ScriptError)(const SQChar* pszFormat, ...);
inline SQInteger(*v_SQVM_RaiseError)(HSQUIRRELVM v, const SQChar* pszFormat, ...);
inline void(*v_SQVM_PrintObjVal)(HSQUIRRELVM v, const SQObject* oin, SQObject* oout);

inline void(*v_SQVM_AllocCompileBuffer)(HSQUIRRELVM v, SQBufState* bufferState, const SQChar* bufferName, bool raiseError);
inline void(*v_SQVM_FreeCompileBuffer)(HSQUIRRELVM v);

void SQVM_CompileError(HSQUIRRELVM v, const SQChar* pszError, const SQChar* pszFile, SQUnsignedInteger nLine, SQInteger nColumn);

///////////////////////////////////////////////////////////////////////////////
class VSquirrelVM : public IDetour
{
	virtual void GetAdr(void) const
	{
		LogFunAdr("SQVM_GetErrorLine", v_SQVM_GetErrorLine);
		LogFunAdr("SQVM_WarningCmd", v_SQVM_WarningCmd);
		LogFunAdr("SQVM_CompileError", v_SQVM_CompileError);
		LogFunAdr("SQVM_LogicError", v_SQVM_LogicError);
		LogFunAdr("SQVM_ScriptError", v_SQVM_ScriptError);
		LogFunAdr("SQVM_RaiseError", v_SQVM_RaiseError);
		LogFunAdr("SQVM_PrintObjVal", v_SQVM_PrintObjVal);
		LogFunAdr("SQVM_AllocCompileBuffer", v_SQVM_AllocCompileBuffer);
		LogFunAdr("SQVM_FreeCompileBuffer", v_SQVM_FreeCompileBuffer);
	}
	virtual void GetFun(void) const
	{
		Module_FindPattern(g_GameDll, "48 8B C4 55 56 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 83 65 90 FC").GetPtr(v_SQVM_GetErrorLine);
		Module_FindPattern(g_GameDll, "48 83 EC 38 F2 0F 10 05 ?? ?? ?? ??").GetPtr(v_SQVM_LogicError);
		Module_FindPattern(g_GameDll, "40 53 48 83 EC 30 33 DB 48 8D 44 24 ?? 4C 8D 4C 24 ??").GetPtr(v_SQVM_WarningCmd);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 81 EC ?? ?? ?? ?? 48 8B D9 4C 8B F2").GetPtr(v_SQVM_CompileError);
		Module_FindPattern(g_GameDll, "E9 ?? ?? ?? ?? F7 D2").FollowNearCallSelf().GetPtr(v_SQVM_ScriptError);
		Module_FindPattern(g_GameDll, "48 89 54 24 ?? 4C 89 44 24 ?? 4C 89 4C 24 ?? 53 56 57 48 83 EC 40").GetPtr(v_SQVM_RaiseError);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ? 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 45 33 ED").GetPtr(v_SQVM_PrintObjVal);
		Module_FindPattern(g_GameDll, "48 89 5C 24 ?? 57 48 83 EC ?? 48 8B 59 ?? 48 8B F9 83 BB").GetPtr(v_SQVM_AllocCompileBuffer);
		Module_FindPattern(g_GameDll, "40 53 48 83 EC ?? 48 8B 41 ?? FF 88").GetPtr(v_SQVM_FreeCompileBuffer);
	}
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};
///////////////////////////////////////////////////////////////////////////////
