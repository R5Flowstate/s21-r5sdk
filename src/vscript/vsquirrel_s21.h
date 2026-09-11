//=============================================================================//
// S21 Squirrel VM hooks. Stock VSquirrel* patterns are S3; this class is the S21 set.
// VM-type byte: *(uint8_t*)(HSQUIRRELVM->_sharedstate + 0x4450).
//=============================================================================//
#ifndef VSQUIRREL_S21_H
#define VSQUIRREL_S21_H

#include "thirdparty/detours/include/idetour.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/include/sqstate.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "tier0/dbg.h"

// SQSharedState VM-type byte: 0=server, 1=client, 2=ui.
static constexpr size_t kS21_SSS_VMTypeByte = 0x4450;

// Invalid VM-type byte returns NONE (neutral colour, no crash).
inline eDLL_T SQVM_GetVMType_S21(HSQUIRRELVM v)
{
	if (!v)
		return eDLL_T::NONE;

	SQSharedState* const sss =
		*reinterpret_cast<SQSharedState**>(reinterpret_cast<uintptr_t>(v) + 0x50);
	if (!sss)
		return eDLL_T::NONE;

	const uint8_t vmType = *reinterpret_cast<uint8_t*>(
		reinterpret_cast<uintptr_t>(sss) + kS21_SSS_VMTypeByte);

	switch (vmType)
	{
	case 0:  return eDLL_T::SCRIPT_SERVER;
	case 1:  return eDLL_T::SCRIPT_CLIENT;
	case 2:  return eDLL_T::SCRIPT_UI;
	default: return eDLL_T::NONE;
	}
}

// CSquirrelVM::m_iContext offset on S21 is +0x60 (S3 was +0x3C).
static constexpr size_t kS21_CSquirrelVM_Context = 0x60;

inline SQCONTEXT CSquirrelVM_GetContext_S21(void* const s)
{
	if (!s) return SQCONTEXT::NONE;
	const uint8_t v = *reinterpret_cast<uint8_t*>(
		reinterpret_cast<uintptr_t>(s) + kS21_CSquirrelVM_Context);
	if (v <= 2) return static_cast<SQCONTEXT>(v);
	return SQCONTEXT::NONE;
}

// SQSharedState::_contextname offset on S21 is +0x43D1.
static constexpr size_t kS21_SSS_ContextName = 0x43D1;

inline const char* SQVM_GetContextNameStr_S21(HSQUIRRELVM v)
{
	if (!v) return "?";
	SQSharedState* const sss =
		*reinterpret_cast<SQSharedState**>(reinterpret_cast<uintptr_t>(v) + 0x50);
	if (!sss) return "?";
	return reinterpret_cast<const char*>(
		reinterpret_cast<uintptr_t>(sss) + kS21_SSS_ContextName);
}

// Prefer the VM-type byte over SSS+0x43D1 when sharedstate may be null.
inline const char* SQVM_GetContextLabel_S21(eDLL_T ctx)
{
	switch (ctx)
	{
	case eDLL_T::SCRIPT_SERVER: return "SERVER";
	case eDLL_T::SCRIPT_CLIENT: return "CLIENT";
	case eDLL_T::SCRIPT_UI:     return "UI";
	default:                    return "?";
	}
}

// S21 ScriptFunctionBinding_t is 0x58 (S3 was 0x68).
#pragma pack(push, 8)
struct ScriptFunctionBinding_S21
{
	const char*     m_pszScriptName;      // +0x00
	const char*     m_signatureReturn;    // +0x08
	const char*     m_signatureParams;    // +0x10
	bool            m_oldStyle;           // +0x18
	bool            m_varParams;          // +0x19
	char            _pad1A[6];            // +0x1A
	const char*     m_paramMask;          // +0x20
	int             m_defaultParamCount;  // +0x28
	int             m_ReturnType;         // +0x2C (ScriptDataType_t)
	void*           m_pParamMemory;       // +0x30 (CUtlVector data ptr)
	int64_t         m_nAllocationCount;   // +0x38
	int64_t         m_nGrowSize;          // +0x40
	int             m_nParamCount;        // +0x48
	int             _pad4C;               // +0x4C
	void*           m_pFunction;          // +0x50
};
#pragma pack(pop)
static_assert(sizeof(ScriptFunctionBinding_S21) == 0x58,
	"S21 ScriptFunctionBinding_t must be exactly 88 bytes");

class CSquirrelVM;
typedef int ScriptDataType_t;

// Bumped on every client/UI VM (re)init. The HSQUIRRELVM allocation is
// recycled across a level change, so pointer identity cannot prove a cached
// script handle still names a closure this VM owns -- compare generations.
inline unsigned int g_nS21ScriptVMGeneration = 0;

// S21 m_hVM is at +0x30 (not the S3 GetVM/+0x08 layout).
HSQUIRRELVM CSquirrelVM_GetHVM_S21(CSquirrelVM* const s);
bool        CSquirrelVM_IsAlive_S21(CSquirrelVM* const s);

// Explicit parameter types (useTypeCompiler=false).
SQRESULT Script_RegisterFunc_S21(
	CSquirrelVM* s,
	const char* scriptName,
	void* func,
	ScriptDataType_t retType,
	ScriptDataType_t* params,
	int numParams);

// String-signature type compiler (useTypeCompiler=true).
SQRESULT Script_RegisterFuncTC_S21(
	CSquirrelVM* s,
	const char* scriptName,
	void* func,
	const char* returnType,
	const char* parameters);

// m_hVM is +0x30 on S21. VSquirrelAPI is not registered on the client.
bool Script_Execute_S21(const SQChar* code, SQCONTEXT ctx);

// Attach from DllMain. A second attach nests trampolines.
void VSquirrelS21Core_PreInit_AttachCompileBufferHook();

///////////////////////////////////////////////////////////////////////////////
class VSquirrelS21Core : public IDetour
{
	virtual void GetAdr(void) const override;
	virtual void GetFun(void) const override;
	virtual void GetVar(void) const override { }
	virtual void GetCon(void) const override { }
	virtual void Detour(const bool bAttach) const override;
};
///////////////////////////////////////////////////////////////////////////////

#endif // VSQUIRREL_S21_H
