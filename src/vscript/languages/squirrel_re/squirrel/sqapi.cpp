#if defined(CLIENT_DLL)
//=============================================================================//
//
// Purpose: Squirrel API interface to engine
//
//=============================================================================//

#include "core/stdafx.h"
#include "squirrel.h"
#include "sqvm.h"
#include "sqarray.h"
#include "sqstring.h"

// Safe mode skips GetFun as well as the detour, so a gated-out detour class
// leaves every v_sq_* pointer null and a call through one jumps to address 0.
// Announce the first hit instead of dying there.
#define SQ_S21_REQUIRE(fn, retval) \
	if (!(fn)) \
	{ \
		static bool s_bWarned = false; \
		if (!s_bWarned) \
		{ \
			s_bWarned = true; \
			Warning(eDLL_T::CLIENT, \
				"[SQAPI] " #fn " unresolved on this build; call ignored\n"); \
		} \
		return retval; \
	}

#define SQ_S21_REQUIRE_V(fn) SQ_S21_REQUIRE(fn, )

//---------------------------------------------------------------------------------
bool sq_aux_gettypedarg(HSQUIRRELVM v, SQInteger idx, SQObjectType type, SQObjectPtr** o)
{
	*o = &stack_get(v, idx);
	if (sq_type(**o) != type) {
		// RaiseError/PrintObjVal may be null; the contract is return false.
		if (v_SQVM_PrintObjVal && v_SQVM_RaiseError)
		{
			SQObjectPtr oval;
			v->PrintObjVal(*o, &oval);
			v_SQVM_RaiseError(v, _SC("wrong argument type, expected '%s' got '%.50s'"), IdType2Name(type), _stringval(oval));
		}
		else
		{
			// A mismatch can repeat per call; cap the noise.
			static int s_nReported = 0;
			if (s_nReported < 50)
			{
				s_nReported++;
				Warning(eDLL_T::CLIENT, "[SQAPI] wrong argument type at idx %d, expected '%s', got type %d\n",
					(int)idx, IdType2Name(type), (int)sq_type(**o));
			}
		}
		return false;
	}
	return true;
}

//---------------------------------------------------------------------------------
#define _GETSAFE_OBJ(v,idx,type,o) { if(!sq_aux_gettypedarg(v,idx,type,&o)) return SQ_ERROR; }

#define sq_aux_paramscheck(v,count) \
{ \
	if(sq_gettop(v) < count){ \
		if (v_SQVM_RaiseError) \
			v_SQVM_RaiseError(v, _SC("not enough params in the stack")); \
		else \
			Warning(eDLL_T::CLIENT, "[SQAPI] not enough params in the stack (need %d)\n", (int)count); \
		return SQ_ERROR; }\
}

//---------------------------------------------------------------------------------
SQRESULT sq_getinteger(HSQUIRRELVM v, SQInteger idx, SQInteger* i)
{
	SQObjectPtr& o = stack_get(v, idx);
	if (sq_isnumeric(o)) {
		*i = tointeger(o);
		return SQ_OK;
	}
	return SQ_ERROR;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getfloat(HSQUIRRELVM v, SQInteger idx, SQFloat* f)
{
	SQObjectPtr& o = stack_get(v, idx);
	if (sq_isnumeric(o)) {
		*f = tofloat(o);
		return SQ_OK;
	}
	return SQ_ERROR;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getvector(HSQUIRRELVM v, SQInteger idx, const SQVector3D** w)
{
	SQObjectPtr& o = stack_get(v, idx);
	if (sq_isvector(o)) {
		*w = _vector(o);
		return SQ_OK;
	}
	return SQ_ERROR;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getbool(HSQUIRRELVM v, SQInteger idx, SQBool* b)
{
	SQObjectPtr& o = stack_get(v, idx);
	if (sq_isbool(o)) {
		*b = _bool(o);
		return SQ_OK;
	}
	return SQ_ERROR;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getthread(HSQUIRRELVM v, SQInteger idx, HSQUIRRELVM* thread)
{
	SQObjectPtr* o = NULL;
	_GETSAFE_OBJ(v, idx, OT_THREAD, o);
	*thread = _thread(*o);
	return SQ_OK;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getstring(HSQUIRRELVM v, SQInteger idx, const SQChar** c)
{
	SQObjectPtr* o = NULL;
	_GETSAFE_OBJ(v, idx, OT_STRING, o);
	*c = _stringval(*o);
	return SQ_OK;
}

//---------------------------------------------------------------------------------
SQRESULT sq_get(HSQUIRRELVM v, SQInteger idx)
{
	SQ_S21_REQUIRE(v_sq_get, SQ_ERROR);
	return v_sq_get(v, idx);
}

//---------------------------------------------------------------------------------
SQInteger sq_gettop(HSQUIRRELVM v)
{
	// S21 keeps _top and _bottom at different offsets, and both are 32-bit here.
	return (SQVM_S21_Top(v) - SQVM_S21_Bottom(v));
}

//---------------------------------------------------------------------------------
void sq_settop(HSQUIRRELVM v, SQInteger newtop)
{
	SQInteger top = sq_gettop(v);
	if (top > newtop)
		sq_pop(v, top - newtop);
	else
		while (top++ < newtop) sq_pushnull(v);
}

//---------------------------------------------------------------------------------
SQRESULT sq_getstackobj(HSQUIRRELVM v, SQInteger idx, HSQOBJECT* po)
{
	*po = stack_get(v, idx);
	return SQ_OK;
}

//---------------------------------------------------------------------------------
void sq_pop(HSQUIRRELVM v, SQInteger nelemstopop)
{
	Assert(v->_top >= nelemstopop);
	SQVM_S21_Pop(v, nelemstopop);
}

//---------------------------------------------------------------------------------
// Prefer engine sq_push* (VSquirrelS21Core resolves S21 type tags + _top@+0x68).
// SDK v->Push uses S3 SQVM layout and AVs on S21 natives.
//---------------------------------------------------------------------------------
void sq_pushnull(HSQUIRRELVM v)
{
	SQVM_S21_Push(v, _null_);
}

//---------------------------------------------------------------------------------
SQRESULT sq_pushroottable(HSQUIRRELVM v)
{
	if (v_sq_pushroottable)
		return v_sq_pushroottable(v);

	SQVM_S21_Push(v, SQVM_S21_RootTable(v));
	return SQ_OK;
}

//---------------------------------------------------------------------------------
void sq_pushbool(HSQUIRRELVM v, SQBool b)
{
	if (v_sq_pushbool)
	{
		v_sq_pushbool(v, b);
		return;
	}
	SQVM_S21_Push(v, SQObjectPtr(b ? true : false));
}

//---------------------------------------------------------------------------------
void sq_pushstring(HSQUIRRELVM v, const SQChar* s, SQInteger len)
{
	if (v_sq_pushstring)
	{
		v_sq_pushstring(v, s, len);
		return;
	}
	if (s)
	{
		SQString* pString = SQString::Create(SQVM_S21_SharedState(v), s, len);
		SQVM_S21_Push(v, SQObjectPtr(pString));
	}
	else
		SQVM_S21_Push(v, _null_);
}

//---------------------------------------------------------------------------------
void sq_pushinteger(HSQUIRRELVM v, SQInteger i)
{
	if (v_sq_pushinteger)
	{
		v_sq_pushinteger(v, i);
		return;
	}
	SQVM_S21_Push(v, SQObjectPtr(i));
}

//---------------------------------------------------------------------------------
void sq_pushfloat(HSQUIRRELVM v, SQFloat f)
{
	if (v_sq_pushfloat)
	{
		v_sq_pushfloat(v, f);
		return;
	}
	SQVM_S21_Push(v, SQObjectPtr(f));
}

//---------------------------------------------------------------------------------
void sq_pushvector(HSQUIRRELVM v, const SQVector3D* w)
{
	SQVM_S21_Push(v, SQObjectPtr(w));
}

//---------------------------------------------------------------------------------
void sq_pushobject(HSQUIRRELVM v, HSQOBJECT obj)
{
	SQVM_S21_Push(v, SQObjectPtr(obj));
}

//---------------------------------------------------------------------------------
void sq_newarray(HSQUIRRELVM v, SQInteger size)
{
	SQ_S21_REQUIRE_V(v_sq_newarray);
	v_sq_newarray(v, size);
}

//---------------------------------------------------------------------------------
void sq_newtable(HSQUIRRELVM v)
{
	SQ_S21_REQUIRE_V(v_sq_newtable);
	v_sq_newtable(v);
}

//---------------------------------------------------------------------------------
SQRESULT sq_newslot(HSQUIRRELVM v, SQInteger idx)
{
	SQ_S21_REQUIRE(v_sq_newslot, SQ_ERROR);
	return v_sq_newslot(v, idx);
}

//---------------------------------------------------------------------------------
SQRESULT sq_arrayappend(HSQUIRRELVM v, SQInteger idx)
{
	SQ_S21_REQUIRE(v_sq_arrayappend, SQ_ERROR);
	return v_sq_arrayappend(v, idx);
}

//---------------------------------------------------------------------------------
SQRESULT sq_pushstructure(HSQUIRRELVM v, const SQChar* name, const SQChar* member, const SQChar* codeclass1, const SQChar* codeclass2)
{
	SQ_S21_REQUIRE(v_sq_pushstructure, SQ_ERROR);
	return v_sq_pushstructure(v, name, member, codeclass1, codeclass2);
}

//---------------------------------------------------------------------------------
SQRESULT sq_compilebuffer(HSQUIRRELVM v, SQBufState* bufferState, const SQChar* buffer, SQInteger level, SQBool raiseerror)
{
	SQ_S21_REQUIRE(v_sq_compilebuffer, SQ_ERROR);
	return v_sq_compilebuffer(v, bufferState, buffer, level, raiseerror);
}

//---------------------------------------------------------------------------------
SQRESULT sq_call(HSQUIRRELVM v, SQInteger params, SQBool retval, SQBool raiseerror)
{
	SQ_S21_REQUIRE(v_sq_call, SQ_ERROR);
	return v_sq_call(v, params, retval, raiseerror);
}

//---------------------------------------------------------------------------------
SQRESULT sq_startconsttable(HSQUIRRELVM v)
{
	SQ_S21_REQUIRE(v_sq_startconsttable, SQ_ERROR);
	return v_sq_startconsttable(v);
}

//---------------------------------------------------------------------------------
SQRESULT sq_endconsttable(HSQUIRRELVM v)
{
	SQ_S21_REQUIRE(v_sq_endconsttable, SQ_ERROR);
	return v_sq_endconsttable(v);
}

//---------------------------------------------------------------------------------
void sq_addref(HSQUIRRELVM v, SQObject* po)
{
	if (!ISREFCOUNTED(sq_type(*po))) return;

	// S21 shared state is at v+0x50; RefTable inside it is unmapped, so this is a no-op.
	static bool bWarned = false;
	if (!bWarned)
	{
		bWarned = true;
		Warning(eDLL_T::CLIENT, "[SQAPI] sq_addref is not implemented on this build; "
			"script handle retention is a no-op\n");
	}
}

//---------------------------------------------------------------------------------
SQBool sq_release(HSQUIRRELVM v, SQObject* po)
{
	if (!ISREFCOUNTED(sq_type(*po))) return SQTrue;

	// Paired with sq_addref above; see the note there.
	static bool bWarned = false;
	if (!bWarned)
	{
		bWarned = true;
		Warning(eDLL_T::CLIENT, "[SQAPI] sq_release is not implemented on this build; "
			"script handle release is a no-op\n");
	}
	return SQTrue;
}

void VSquirrelAPI::Detour(const bool bAttach) const
{
	DetourSetup(&v_sq_pushroottable, &sq_pushroottable, bAttach);
	//DetourSetup(&v_sq_pushbool, &sq_pushbool, bAttach);
	//DetourSetup(&v_sq_pushstring, &sq_pushstring, bAttach);
	//DetourSetup(&v_sq_pushinteger, &sq_pushinteger, bAttach);
	//DetourSetup(&v_sq_pushfloat, &sq_pushfloat, bAttach);
	DetourSetup(&v_sq_newarray, &sq_newarray, bAttach);
	DetourSetup(&v_sq_newtable, &sq_newtable, bAttach);
	DetourSetup(&v_sq_newslot, &sq_newslot, bAttach);
	DetourSetup(&v_sq_arrayappend, &sq_arrayappend, bAttach);
	DetourSetup(&v_sq_pushstructure, &sq_pushstructure, bAttach);
	DetourSetup(&v_sq_compilebuffer, &sq_compilebuffer, bAttach);
	DetourSetup(&v_sq_call, &sq_call, bAttach);

	DetourSetup(&v_sq_startconsttable, &sq_startconsttable, bAttach);
	DetourSetup(&v_sq_endconsttable, &sq_endconsttable, bAttach);
}
#else // !CLIENT_DLL
//=============================================================================//
//
// Purpose: Squirrel API interface to engine
//
//=============================================================================//

#include "core/stdafx.h"
#include "squirrel.h"
#include "sqvm.h"
#include "sqarray.h"
#include "sqstring.h"

//---------------------------------------------------------------------------------
bool sq_aux_gettypedarg(HSQUIRRELVM v, SQInteger idx, SQObjectType type, SQObjectPtr** o)
{
	*o = &stack_get(v, idx);
	if (sq_type(**o) != type) {
		// RaiseError/PrintObjVal may be null; the contract is return false.
		if (v_SQVM_PrintObjVal && v_SQVM_RaiseError)
		{
			SQObjectPtr oval;
			v->PrintObjVal(*o, &oval);
			v_SQVM_RaiseError(v, _SC("wrong argument type, expected '%s' got '%.50s'"), IdType2Name(type), _stringval(oval));
		}
		else
		{
			// A mismatch can repeat per call; cap the noise.
			static int s_nReported = 0;
			if (s_nReported < 50)
			{
				s_nReported++;
				Warning(eDLL_T::SERVER, "[SQAPI] wrong argument type at idx %d, expected '%s', got type %d\n",
					(int)idx, IdType2Name(type), (int)sq_type(**o));
			}
		}
		return false;
	}
	return true;
}

//---------------------------------------------------------------------------------
#define _GETSAFE_OBJ(v,idx,type,o) { if(!sq_aux_gettypedarg(v,idx,type,&o)) return SQ_ERROR; }

#define sq_aux_paramscheck(v,count) \
{ \
	if(sq_gettop(v) < count){ \
		if (v_SQVM_RaiseError) \
			v_SQVM_RaiseError(v, _SC("not enough params in the stack")); \
		else \
			Warning(eDLL_T::SERVER, "[SQAPI] not enough params in the stack (need %d)\n", (int)count); \
		return SQ_ERROR; }\
}

//---------------------------------------------------------------------------------
SQRESULT sq_getinteger(HSQUIRRELVM v, SQInteger idx, SQInteger* i)
{
	SQObjectPtr& o = stack_get(v, idx);
	if (sq_isnumeric(o)) {
		*i = tointeger(o);
		return SQ_OK;
	}
	return SQ_ERROR;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getfloat(HSQUIRRELVM v, SQInteger idx, SQFloat* f)
{
	SQObjectPtr& o = stack_get(v, idx);
	if (sq_isnumeric(o)) {
		*f = tofloat(o);
		return SQ_OK;
	}
	return SQ_ERROR;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getvector(HSQUIRRELVM v, SQInteger idx, const SQVector3D** w)
{
	SQObjectPtr& o = stack_get(v, idx);
	if (sq_isvector(o)) {
		*w = _vector(o);
		return SQ_OK;
	}
	return SQ_ERROR;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getbool(HSQUIRRELVM v, SQInteger idx, SQBool* b)
{
	SQObjectPtr& o = stack_get(v, idx);
	if (sq_isbool(o)) {
		*b = _bool(o);
		return SQ_OK;
	}
	return SQ_ERROR;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getthread(HSQUIRRELVM v, SQInteger idx, HSQUIRRELVM* thread)
{
	SQObjectPtr* o = NULL;
	_GETSAFE_OBJ(v, idx, OT_THREAD, o);
	*thread = _thread(*o);
	return SQ_OK;
}

//---------------------------------------------------------------------------------
SQRESULT sq_getstring(HSQUIRRELVM v, SQInteger idx, const SQChar** c)
{
	SQObjectPtr* o = NULL;
	_GETSAFE_OBJ(v, idx, OT_STRING, o);
	*c = _stringval(*o);
	return SQ_OK;
}

//---------------------------------------------------------------------------------
SQRESULT sq_get(HSQUIRRELVM v, SQInteger idx)
{
	return v_sq_get(v, idx);
}

//---------------------------------------------------------------------------------
SQInteger sq_gettop(HSQUIRRELVM v)
{
	// _top and _bottom are 32-bit and _bottom is at 0x54, so the SQVM members
	// cannot express them -- read the fields the engine's own gettop reads.
	return (SQVM_S3_Top(v) - SQVM_S3_Bottom(v));
}

//---------------------------------------------------------------------------------
void sq_settop(HSQUIRRELVM v, SQInteger newtop)
{
	SQInteger top = sq_gettop(v);
	if (top > newtop)
		sq_pop(v, top - newtop);
	else
		while (top++ < newtop) sq_pushnull(v);
}

//---------------------------------------------------------------------------------
SQRESULT sq_getstackobj(HSQUIRRELVM v, SQInteger idx, HSQOBJECT* po)
{
	*po = stack_get(v, idx);
	return SQ_OK;
}

//---------------------------------------------------------------------------------
void sq_pop(HSQUIRRELVM v, SQInteger nelemstopop)
{
	Assert(v->_top >= nelemstopop);
	v->Pop(nelemstopop);
}

//---------------------------------------------------------------------------------
void sq_pushnull(HSQUIRRELVM v)
{
	v->Push(_null_);
}

//---------------------------------------------------------------------------------
SQRESULT sq_pushroottable(HSQUIRRELVM v)
{
	v->Push(v->_roottable);

	return SQ_OK;
}

//---------------------------------------------------------------------------------
void sq_pushbool(HSQUIRRELVM v, SQBool b)
{
	v->Push(b?true:false);
}

//---------------------------------------------------------------------------------
void sq_pushstring(HSQUIRRELVM v, const SQChar* s, SQInteger len)
{
	if (s)
	{
		SQString* pString = SQString::Create(v->_sharedstate, s, len);
		v->Push(pString);
	}
	else
		v->Push(_null_);
}

//---------------------------------------------------------------------------------
void sq_pushinteger(HSQUIRRELVM v, SQInteger i)
{
	v->Push(i);
}

//---------------------------------------------------------------------------------
void sq_pushfloat(HSQUIRRELVM v, SQFloat f)
{
	v->Push(f);
}

//---------------------------------------------------------------------------------
void sq_pushvector(HSQUIRRELVM v, const SQVector3D* w)
{
	v->Push(w);
}

//---------------------------------------------------------------------------------
void sq_pushobject(HSQUIRRELVM v, HSQOBJECT obj)
{
	v->Push(SQObjectPtr(obj));
}

//---------------------------------------------------------------------------------
void sq_newarray(HSQUIRRELVM v, SQInteger size)
{
	v_sq_newarray(v, size);
}

//---------------------------------------------------------------------------------
void sq_newtable(HSQUIRRELVM v)
{
	v_sq_newtable(v);
}

//---------------------------------------------------------------------------------
SQRESULT sq_newslot(HSQUIRRELVM v, SQInteger idx)
{
	return v_sq_newslot(v, idx);
}

//---------------------------------------------------------------------------------
SQRESULT sq_arrayappend(HSQUIRRELVM v, SQInteger idx)
{
	return v_sq_arrayappend(v, idx);
}

//---------------------------------------------------------------------------------
SQRESULT sq_pushstructure(HSQUIRRELVM v, const SQChar* name, const SQChar* member, const SQChar* codeclass1, const SQChar* codeclass2)
{
	return v_sq_pushstructure(v, name, member, codeclass1, codeclass2);
}

//---------------------------------------------------------------------------------
SQRESULT sq_compilebuffer(HSQUIRRELVM v, SQBufState* bufferState, const SQChar* buffer, SQInteger level, SQBool raiseerror)
{
	return v_sq_compilebuffer(v, bufferState, buffer, level, raiseerror);
}

//---------------------------------------------------------------------------------
SQRESULT sq_call(HSQUIRRELVM v, SQInteger params, SQBool retval, SQBool raiseerror)
{
	return v_sq_call(v, params, retval, raiseerror);
}

//---------------------------------------------------------------------------------
SQRESULT sq_startconsttable(HSQUIRRELVM v)
{
	return v_sq_startconsttable(v);
}

//---------------------------------------------------------------------------------
SQRESULT sq_endconsttable(HSQUIRRELVM v)
{
	return v_sq_endconsttable(v);
}

//---------------------------------------------------------------------------------
void sq_addref(HSQUIRRELVM v, SQObject* po)
{
	if (!ISREFCOUNTED(sq_type(*po))) return;
	_ss(v)->_refs_table.AddRef(*po);
}

//---------------------------------------------------------------------------------
SQBool sq_release(HSQUIRRELVM v, SQObject* po)
{
	if (!ISREFCOUNTED(sq_type(*po))) return SQTrue;
	return _ss(v)->_refs_table.Release(*po);
}

void VSquirrelAPI::Detour(const bool bAttach) const
{
	DetourSetup(&v_sq_pushroottable, &sq_pushroottable, bAttach);
	//DetourSetup(&v_sq_pushbool, &sq_pushbool, bAttach);
	//DetourSetup(&v_sq_pushstring, &sq_pushstring, bAttach);
	//DetourSetup(&v_sq_pushinteger, &sq_pushinteger, bAttach);
	//DetourSetup(&v_sq_pushfloat, &sq_pushfloat, bAttach);
	DetourSetup(&v_sq_newarray, &sq_newarray, bAttach);
	DetourSetup(&v_sq_newtable, &sq_newtable, bAttach);
	DetourSetup(&v_sq_newslot, &sq_newslot, bAttach);
	DetourSetup(&v_sq_arrayappend, &sq_arrayappend, bAttach);
	DetourSetup(&v_sq_pushstructure, &sq_pushstructure, bAttach);
	DetourSetup(&v_sq_compilebuffer, &sq_compilebuffer, bAttach);
	DetourSetup(&v_sq_call, &sq_call, bAttach);

	DetourSetup(&v_sq_startconsttable, &sq_startconsttable, bAttach);
	DetourSetup(&v_sq_endconsttable, &sq_endconsttable, bAttach);
}
#endif // CLIENT_DLL
