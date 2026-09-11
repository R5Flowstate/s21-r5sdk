#ifndef SQSTRING_H
#define SQSTRING_H
#include "squirrel.h"
#include "sqobject.h"
#include "sqstate.h"

#if defined(CLIENT_DLL)

// S21 SQString has no _context/_extraData; _val is at +0x30, not S3's +0x40.
struct SQString
{
	void* _vftable;
	SQUnsignedInteger _uiRef;
	SQUnsignedInteger pad0C;
	SQObject* _weakref; // this is not an sqobject!
	SQSharedState* _sharedstate;
	SQInteger _len;
	char gap24[4];
	SQHash _hash;
	SQChar _val[1];

	static SQString* Create(SQSharedState* sharedstate, const SQChar* s, SQInteger len)
	{
		SQString* str = v_StringTable__Add(sharedstate->_stringtable, s, len);
		str->_sharedstate = sharedstate;

		return str;
	}
};
static_assert(offsetof(SQString, _val) == 0x30);

#else // !CLIENT_DLL

struct SQString : public SQRefCounted
{
	SQSharedState* _sharedstate;
	SQInteger _len;
	char gap34[4];
	SQHash _hash;
	SQChar _val[1];

	static SQString* Create(SQSharedState* sharedstate, const SQChar* s, SQInteger len)
	{
		SQString* str = v_StringTable__Add(sharedstate->_stringtable, s, len);
		str->_sharedstate = sharedstate;

		return str;
	}
};
static_assert(offsetof(SQString, _val) == 0x40);

#endif // CLIENT_DLL

#endif // SQSTRING_H
