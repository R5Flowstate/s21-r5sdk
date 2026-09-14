//=============================================================================//
// S21 Squirrel VM detours. See vsquirrel_s21.h.
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/platform_internal.h"
#include "vscript/vsquirrel_s21.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/languages/squirrel_re/include/sqstdstring.h"
#include "vscript/languages/squirrel_re/include/sqstdaux.h"
#include "tier1/cvar.h"
#include "tier0/commandline.h"
#include "game/client/scriptnetdata_client.h"
#include "vscript/vscript_s21_override.h"

extern void Cvar_ForceCommandLineDevModeForScripts();

static void DevInjectDiag(const char* fmt, ...);

extern bool g_bSQAuxError;
extern bool g_bSQAuxBadLogic;
extern HSQUIRRELVM g_pErrorVM;

static ConVar script_show_output_s21(
	"script_show_output_s21", "2", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"S21 Squirrel print-output log level. 0=disk only, 1=+console, 2=+notify.",
	true, 0.f, true, 2.f);
static ConVar script_show_warning_s21(
	"script_show_warning_s21", "2", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"S21 Squirrel warning-output log level. 0=disk only, 1=+console, 2=+notify.",
	true, 0.f, true, 2.f);

//-----------------------------------------------------------------------------
// v may be an SQCONTEXT enum (0/1/2) or an HSQUIRRELVM. Else VM-type at SSS+0x4450.
//-----------------------------------------------------------------------------
// printt bakes "Script(X):" into the string; strip it so CoreMsg does not double-prefix.
static const char* StripScriptVMTag(const char* s)
{
	if (!s) return s;
	// Need exactly "Script(X):" where X is one of S/C/U and the colon
	// immediately follows the ')'. If it doesn't match, leave it alone.
	if (s[0] == 'S' && s[1] == 'c' && s[2] == 'r' && s[3] == 'i' &&
		s[4] == 'p' && s[5] == 't' && s[6] == '(' &&
		(s[7] == 'S' || s[7] == 'C' || s[7] == 'U') &&
		s[8] == ')' && s[9] == ':')
	{
		return s + 10;
	}
	return s;
}

// An arity error raised from a native entry point has no script call stack;
// the native frames are the only record of who made the call.
static void Script_LogArityNativeCallers(void)
{
	static int s_nLogged = 0;
	if (s_nLogged >= 12)
		return;
	++s_nLogged;

	void* frames[24] = {};
	const USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
	char line[1024];
	int len = 0;
	for (USHORT i = 0; i < n && len < 900; ++i)
	{
		HMODULE hm = nullptr;
		char szMod[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(frames[i]), &hm) && hm)
			GetModuleFileNameA(hm, szMod, sizeof(szMod));
		const char* base = strrchr(szMod, '\\') ? strrchr(szMod, '\\') + 1 : szMod;
		len += snprintf(line + len, sizeof(line) - len, " %s+0x%llX", base,
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(hm)));
	}
	Warning(eDLL_T::CLIENT, "[SQ-ARITY] native callers:%s\n", line);
}

SQRESULT Script_PrintFunc_S21(HSQUIRRELVM v, SQChar* fmt, ...)
{
	eDLL_T remoteContext = eDLL_T::NONE;

	// Pointer-or-context: 0/1/2 is an enum, not a VM pointer.
#pragma warning(push)
#pragma warning(disable : 4302 4311)
	const uintptr_t asUint = reinterpret_cast<uintptr_t>(v);
#pragma warning(pop)
	if (asUint <= 2)
	{
		switch (static_cast<int>(asUint))
		{
		case 0: remoteContext = eDLL_T::SCRIPT_SERVER; break;
		case 1: remoteContext = eDLL_T::SCRIPT_CLIENT; break;
		case 2: remoteContext = eDLL_T::SCRIPT_UI;     break;
		}
	}
	else
	{
		remoteContext = SQVM_GetVMType_S21(v);
	}

	// Promote info -> warning when sqstd_aux flagged an error for this VM.
	const bool bLogLevelOverride =
		(g_bSQAuxError || (g_bSQAuxBadLogic && v == g_pErrorVM));
	LogType_t type = bLogLevelOverride ? LogType_t::SQ_WARNING : LogType_t::SQ_INFO;

	LogLevel_t level = LogLevel_t(script_show_output_s21.GetInt());
	if (type == LogType_t::SQ_WARNING && level == LogLevel_t::LEVEL_DISK_ONLY)
		level = LogLevel_t::LEVEL_CONSOLE;

	char body[2048];
	va_list args;
	va_start(args, fmt);
	const int n = vsnprintf(body, sizeof(body), fmt ? fmt : "", args);
	va_end(args);

	if (n < 0)
	{
		body[0] = '\0';
	}
	else if (static_cast<size_t>(n) >= sizeof(body))
	{
		body[sizeof(body) - 1] = '\0';
	}

	const char* const payload = StripScriptVMTag(body);

	// Reuse the "squirrel_re" logger so EngineLoggerSink picks up the
	// registered spdlog sink instead of the default one.
	CoreMsg(type, level, remoteContext, NO_ERROR, "squirrel_re",
			"%s", payload);

	if (strstr(payload, "wrong number of parameters"))
		Script_LogArityNativeCallers();

	return SQ_OK;
}

//-----------------------------------------------------------------------------
// nformatstringidx indexes the VM argument stack; sqstd_format expands it.
//-----------------------------------------------------------------------------
SQBool Script_WarningFunc_S21(HSQUIRRELVM v, SQInteger nformatstringidx)
{
	if (!v_sqstd_format)
	{
		// sqstd_format didn't resolve; don't try to format. Emit a
		// placeholder so we can still see a warning was raised.
		Warning(SQVM_GetVMType_S21(v),
				"Squirrel warning (sqstd_format unavailable)\n");
		return SQTrue;
	}

	SQInteger strLen = 0;
	SQChar* str = nullptr;
	const SQRESULT result = v_sqstd_format(v, nformatstringidx, SQTrue, &strLen, &str);

	const eDLL_T remoteContext = SQVM_GetVMType_S21(v);

	CoreMsg(LogType_t::SQ_WARNING,
			static_cast<LogLevel_t>(script_show_warning_s21.GetInt()),
			remoteContext, NO_ERROR, "squirrel_re(warning)",
			"%s", str ? str : "(null)");

	return SQ_SUCCEEDED(result);
}

//-----------------------------------------------------------------------------
// File/line from params; VM label from SSS+0x4450. Error(NO_ERROR) returns.
//-----------------------------------------------------------------------------
void SQVM_CompileError_S21(
	HSQUIRRELVM v, const SQChar* pszError,
	const SQChar* pszFile, SQUnsignedInteger nLine, SQInteger nColumn)
{
	const eDLL_T context = SQVM_GetVMType_S21(v);
	const char* const label = SQVM_GetContextLabel_S21(context);

	// Bulletproof capture of the exact compile error -- survives the abnormal
	// exit that loses the spdlog message.log buffer during boot.
	DevInjectDiag("COMPILE ERROR [%s] file=\"%s\" line=%llu col=%lld msg=\"%s\"",
		label, pszFile ? pszFile : "(unknown)",
		(unsigned long long)nLine, (long long)nColumn,
		pszError ? pszError : "(null)");

	Error(context, NO_ERROR, "%s SCRIPT COMPILE ERROR: %s\n",
		  label, pszError ? pszError : "(null)");

	if (v_SQVM_GetErrorLine && pszFile)
	{
		char szContextBuf[256];
		szContextBuf[0] = '\0';
		v_SQVM_GetErrorLine(pszFile, static_cast<SQInteger>(nLine),
							szContextBuf, sizeof(szContextBuf) - 1);
		szContextBuf[sizeof(szContextBuf) - 1] = '\0';
		if (szContextBuf[0])
			Error(context, NO_ERROR, " -> %s\n\n", szContextBuf);
	}

	Error(context, NO_ERROR, "%s line [%llu] column [%lld]\n",
		  pszFile ? pszFile : "(unknown)",
		  (unsigned long long)nLine, (long long)nColumn);
}

//-----------------------------------------------------------------------------
// Extra float at xmm3 is unused; pass through. Populate per-VM handles after native Init.
//-----------------------------------------------------------------------------
static bool (*v_CSquirrelVM_Init_S21)(CSquirrelVM* s, SQCONTEXT ctx,
									  SQFloat curTime, SQFloat unused) = nullptr;

void Script_RegisterCommandLineDevConstants_S21(CSquirrelVM* s, SQCONTEXT ctx)
{
	if (!s)
		return;

	// Force developer cvar only. Do NOT RegisterConstant("DEVELOPER") -- redefines the bool prelude.
	const bool hasDevArg = CommandLine() &&
		(CommandLine()->CheckParm("-dev") || CommandLine()->CheckParm("-developer"));
	if (hasDevArg)
		Cvar_ForceCommandLineDevModeForScripts();

	static CSquirrelVM* s_logged[3] = {};
	const int ctxIndex = static_cast<int>(ctx);
	if (ctxIndex >= 0 && ctxIndex < SDK_ARRAYSIZE(s_logged) && s_logged[ctxIndex] != s)
	{
		s_logged[ctxIndex] = s;
		const int devValue = hasDevArg ? 1 : 0;
		DevInjectDiag("DEVELOPER bool-prelude path ctx=%d value=%d (no RegisterConstant)",
			ctxIndex, devValue);
	}
}

static bool __fastcall CSquirrelVM_Init_S21(
	CSquirrelVM* s, SQCONTEXT ctx, SQFloat curTime, SQFloat unused)
{
	if (!v_CSquirrelVM_Init_S21)
		return false;

	const bool ok = v_CSquirrelVM_Init_S21(s, ctx, curTime, unused);
	if (!ok)
		return false;

	switch (ctx)
	{
	case SQCONTEXT::CLIENT:
		g_pClientScript = s;
		++g_nS21ScriptVMGeneration;
		Script_RegisterCommandLineDevConstants_S21(s, ctx);
		ResetLateNativeRegistration_S21(1);
		ScriptNetData_RegisterLimitsOnClient(s);
		break;
	case SQCONTEXT::UI:
		g_pUIScript = s;
		++g_nS21ScriptVMGeneration;
		Script_RegisterCommandLineDevConstants_S21(s, ctx);
		ResetLateNativeRegistration_S21(2);
		ScriptNetData_RegisterLimitsOnUI(s);
		break;
	default:
		break;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Raise g_bSQAuxError around native so print lines promote to SQ_WARNING.
//-----------------------------------------------------------------------------
SQInteger sqstd_aux_printerror_S21(HSQUIRRELVM v)
{
	if (!v_sqstd_aux_printerror)
		return -1;

	g_bSQAuxError = true;
	g_pErrorVM = v;

	const SQInteger result = v_sqstd_aux_printerror(v);

	g_bSQAuxError = false;
	g_pErrorVM = nullptr;
	return result;
}

void SQVM_LogicError_S21(SQBool bPrompt)
{
	if (g_flErrorTimeStamp && (*g_flErrorTimeStamp) > 0.0 &&
		(bPrompt || Plat_FloatTime() > (*g_flErrorTimeStamp) + 0.0))
	{
		g_bSQAuxBadLogic = true;
	}
	else
	{
		g_bSQAuxBadLogic = false;
		g_pErrorVM = nullptr;
	}

	if (v_SQVM_LogicError)
		v_SQVM_LogicError(bPrompt);
}

//=============================================================================
// S21-correct ScriptFunctionBinding registration
//=============================================================================

static void Script_EnsureRegisterFunctionResolved_S21(void)
{
	if (CSquirrelVM__RegisterFunction)
		return;

	// Same S21 pattern as VSquirrelS21Core::GetFun (S3 short pattern).
	Module_FindPattern(g_GameDll, "48 83 EC 38 45 0F B6 C8")
		.GetPtr(CSquirrelVM__RegisterFunction);

	if (CSquirrelVM__RegisterFunction)
	{
		Msg(eDLL_T::ENGINE,
			"[S21-REG] CSquirrelVM::RegisterFunction lazy-resolved @ %p "
			"(VSquirrelS21Core not required)\n",
			reinterpret_cast<void*>(CSquirrelVM__RegisterFunction));
	}
	else
	{
		Warning(eDLL_T::ENGINE,
			"[S21-REG] CSquirrelVM::RegisterFunction pattern UNRESOLVED -- "
			"UI natives (GetConnectionPingMs, etc.) cannot register\n");
	}
}

SQRESULT Script_RegisterFunc_S21(
	CSquirrelVM* s,
	const char* scriptName,
	void* func,
	ScriptDataType_t retType,
	ScriptDataType_t* params,
	int numParams)
{
	Script_EnsureRegisterFunctionResolved_S21();
	if (!CSquirrelVM__RegisterFunction)
	{
		Warning(eDLL_T::NONE,
			"Script_RegisterFunc_S21: RegisterFunction not resolved\n");
		return SQ_ERROR;
	}

	ScriptFunctionBinding_S21 binding = {};
	binding.m_pszScriptName     = scriptName;
	binding.m_signatureReturn   = nullptr;
	binding.m_signatureParams   = nullptr;
	binding.m_oldStyle          = false;
	binding.m_varParams         = false;
	binding.m_paramMask         = nullptr;
	binding.m_defaultParamCount = 0;
	binding.m_ReturnType        = retType;
	binding.m_pParamMemory      = params;
	binding.m_nAllocationCount  = numParams;
	binding.m_nGrowSize         = 0;
	binding.m_nParamCount       = numParams;
	binding.m_pFunction         = func;

	// Same S21 return-tag semantics as Script_RegisterFuncTC_S21 (see below).
	const SQRESULT r = CSquirrelVM__RegisterFunction(s,
		reinterpret_cast<ScriptFunctionBinding_t*>(&binding), false);
	if (r == SQ_ERROR)
		return r;
	return SQ_OK;
}

SQRESULT Script_RegisterFuncTC_S21(
	CSquirrelVM* s,
	const char* scriptName,
	void* func,
	const char* returnType,
	const char* parameters)
{
	Script_EnsureRegisterFunctionResolved_S21();
	if (!CSquirrelVM__RegisterFunction)
	{
		Warning(eDLL_T::NONE,
			"Script_RegisterFuncTC_S21: RegisterFunction not resolved "
			"(name=%s) -- check quarantine / VSquirrelS21Core\n",
			scriptName ? scriptName : "?");
		return SQ_ERROR;
	}

	ScriptFunctionBinding_S21 binding = {};
	binding.m_pszScriptName     = scriptName;
	binding.m_signatureReturn   = returnType;
	binding.m_signatureParams   = parameters;
	binding.m_oldStyle          = false;
	binding.m_varParams         = false;
	binding.m_paramMask         = nullptr;
	binding.m_defaultParamCount = 0;
	binding.m_ReturnType        = 0;
	binding.m_pParamMemory      = nullptr;
	binding.m_nAllocationCount  = 0;
	binding.m_nGrowSize         = 0;
	binding.m_nParamCount       = 0;
	binding.m_pFunction         = func;

	// Success returns OT_NATIVECLOSURE, not SQ_OK. Only SQ_ERROR is a hard fail.
	const SQRESULT r = CSquirrelVM__RegisterFunction(s,
		reinterpret_cast<ScriptFunctionBinding_t*>(&binding), true);
	if (r == SQ_ERROR)
	{
		Warning(eDLL_T::ENGINE,
			"[S21-REG] RegisterFunction failed for '%s' (r=%d)\n",
			scriptName ? scriptName : "?", (int)r);
		return r;
	}

	return SQ_OK;
}

//=============================================================================
// Ad-hoc script evaluation (script_ui / script_client / script ConCommands)
//=============================================================================
typedef long (*PFN_sq_compilebuffer)(HSQUIRRELVM v, SQBufState* bufState,
									 const SQChar* buffer, long level,
									 SQUnsignedInteger raiseError);
typedef long (*PFN_sq_call)(HSQUIRRELVM v, long params, SQUnsignedInteger retval,
							SQUnsignedInteger raiseerror);
typedef long (*PFN_sq_pushroottable)(HSQUIRRELVM v);

static PFN_sq_compilebuffer  v_sq_compilebuffer_s21  = nullptr;
static PFN_sq_call           v_sq_call_s21           = nullptr;
static PFN_sq_pushroottable  v_sq_pushroottable_s21  = nullptr;

// File-compile dispatcher. a2 is char**; *a2 is NUL-terminated source (strlen internally).
typedef int (*PFN_sq_compile_topfn)(void* a1, char** a2, __int64 a3,
									 const char* sourceName, int mode);
static PFN_sq_compile_topfn  v_sq_compile_topfn_s21  = nullptr;
static volatile LONG         s_compileTopfnEarlyAttached = 0;

// Set only after DetourAttach commits. A second attach nests trampolines.
static volatile LONG s_compileBufferEarlyAttached = 0;

static const char* s_earlyAttachStatus = "(pre-init never ran)";
static uintptr_t   s_earlyAttachAddr   = 0;
static LONG        s_earlyAttachRC     = 0;

extern void SDK_Log(const char* fmt, ...);

static const char* memmem_simple(const char* hay, size_t hayLen,
								 const char* needle, size_t needleLen)
{
	if (needleLen == 0 || hayLen < needleLen)
		return nullptr;
	const size_t last = hayLen - needleLen;
	for (size_t i = 0; i <= last; ++i)
		if (memcmp(hay + i, needle, needleLen) == 0)
			return hay + i;
	return nullptr;
}

// CommandLine is not parsed at first topfn fire. Whole-token only: do not match -devsdk.
static int DevInject_DevValue()
{
	const char* const cl = GetCommandLineA();
	if (!cl)
		return 0;
	for (const char* p = cl; (p = strstr(p, "-dev")) != nullptr; p += 4)
	{
		if (p != cl && p[-1] != ' ' && p[-1] != '\t')
			continue;
		// "-dev" end of token
		if (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')
			return 1;
		// "-developer"
		if (strncmp(p + 4, "eloper", 6) == 0 &&
			(p[10] == '\0' || p[10] == ' ' || p[10] == '\t'))
			return 1;
	}
	return 0;
}

// S21 forbids `!` on int. `untyped` must stay first when present.
static int DevInject_FormatPrelude(char* out, size_t outSize, int devValue)
{
	return snprintf(out, outSize,
		"const bool DEVELOPER = %s\n",
		devValue ? "true" : "false");
}

static size_t DevInject_BomLen(const char* src, size_t len)
{
	if (len >= 3 &&
		(uint8_t)src[0] == 0xEF && (uint8_t)src[1] == 0xBB && (uint8_t)src[2] == 0xBF)
		return 3;
	return 0;
}

// Line is only the keyword untyped (optional whitespace / CR).
static bool DevInject_LineIsUntypedOnly(const char* line, size_t lineLen)
{
	size_t i = 0;
	while (i < lineLen && (line[i] == ' ' || line[i] == '\t'))
		++i;
	if (i + 7 > lineLen || memcmp(line + i, "untyped", 7) != 0)
		return false;
	i += 7;
	while (i < lineLen && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
		++i;
	return i == lineLen;
}

static bool DevInject_AlreadyHasBoolPrelude(const char* body, size_t bodyLen)
{
	size_t j = 0;
	while (j < bodyLen && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r' || body[j] == '\n'))
		++j;
	// after optional leading untyped line
	if (j + 7 <= bodyLen && memcmp(body + j, "untyped", 7) == 0)
	{
		const char c = (j + 7 < bodyLen) ? body[j + 7] : '\0';
		if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
		{
			j += 7;
			while (j < bodyLen && body[j] != '\n')
				++j;
			if (j < bodyLen && body[j] == '\n')
				++j;
			while (j < bodyLen && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r' || body[j] == '\n'))
				++j;
		}
	}
	return bodyLen - j >= 23 && memcmp(body + j, "const bool DEVELOPER = ", 23) == 0;
}

// Malloc'd: [BOM][untyped\n?][const bool DEVELOPER][body minus pure-untyped lines].
// nullptr => use original (already injected or format fail / OOM).
static char* DevInject_BuildSourceWithPrelude(
	const char* orig, size_t origLen, int devValue, size_t* outLen)
{
	char prelude[64];
	const int preludeLen = DevInject_FormatPrelude(prelude, sizeof(prelude), devValue);
	if (preludeLen <= 0 || (size_t)preludeLen >= sizeof(prelude))
		return nullptr;

	const size_t bomLen = DevInject_BomLen(orig, origLen);
	const char* const body = orig + bomLen;
	const size_t bodyLen = origLen - bomLen;

	if (!memmem_simple(body, bodyLen, "DEVELOPER", 9))
		return nullptr;
	if (DevInject_AlreadyHasBoolPrelude(body, bodyLen))
		return nullptr;

	// Pass 1: detect pure-untyped lines and bytes to drop.
	bool hasUntypedLine = false;
	size_t dropBytes = 0;
	for (size_t i = 0; i < bodyLen; )
	{
		const size_t lineStart = i;
		while (i < bodyLen && body[i] != '\n')
			++i;
		const size_t lineLen = i - lineStart;
		const size_t span = lineLen + (i < bodyLen ? 1 : 0); // include \n
		if (DevInject_LineIsUntypedOnly(body + lineStart, lineLen))
		{
			hasUntypedLine = true;
			dropBytes += span;
		}
		if (i < bodyLen)
			++i;
	}

	const size_t untypedLead = hasUntypedLine ? 8 : 0; // "untyped\n"
	const size_t newLen = bomLen + untypedLead + (size_t)preludeLen + bodyLen - dropBytes;
	char* const buf = static_cast<char*>(malloc(newLen + 1));
	if (!buf)
		return nullptr;

	size_t o = 0;
	if (bomLen)
	{
		memcpy(buf + o, orig, bomLen);
		o += bomLen;
	}
	if (hasUntypedLine)
	{
		memcpy(buf + o, "untyped\n", 8);
		o += 8;
	}
	memcpy(buf + o, prelude, (size_t)preludeLen);
	o += (size_t)preludeLen;

	// Pass 2: copy body skipping pure-untyped lines.
	for (size_t i = 0; i < bodyLen; )
	{
		const size_t lineStart = i;
		while (i < bodyLen && body[i] != '\n')
			++i;
		const size_t lineLen = i - lineStart;
		const size_t span = lineLen + (i < bodyLen ? 1 : 0);
		if (!DevInject_LineIsUntypedOnly(body + lineStart, lineLen))
		{
			memcpy(buf + o, body + lineStart, span);
			o += span;
		}
		if (i < bodyLen)
			++i;
	}
	buf[o] = '\0';
	*outLen = o;
	return buf;
}

static void DevInjectDiag(const char* fmt, ...)
{
	char body[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
	va_end(ap);

	char line[1152];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "[DEV-INJECT pid=%lu tick=%llu] %s\n",
		static_cast<unsigned long>(GetCurrentProcessId()),
		static_cast<unsigned long long>(GetTickCount64()), body);

	OutputDebugStringA(line);

	FILE* f = nullptr;
	if (fopen_s(&f, "platform/logs/dev_inject_diag.log", "a") == 0 && f)
	{
		fputs(line, f);
		fflush(f);
		fclose(f);
	}
}

static constexpr size_t kS21_CSquirrelVM_HVM = 0x30;

HSQUIRRELVM CSquirrelVM_GetHVM_S21(CSquirrelVM* const s)
{
	if (!s) return nullptr;
	return *reinterpret_cast<HSQUIRRELVM*>(
		reinterpret_cast<uintptr_t>(s) + kS21_CSquirrelVM_HVM);
}

bool CSquirrelVM_IsAlive_S21(CSquirrelVM* const s)
{
	return CSquirrelVM_GetHVM_S21(s) != nullptr;
}

bool Script_Execute_S21(const SQChar* code, SQCONTEXT ctx)
{
	if (!code || !*code)
		return false;

	if (!v_sq_compilebuffer_s21 || !v_sq_call_s21 || !v_sq_pushroottable_s21)
	{
		Warning(static_cast<eDLL_T>(ctx),
				"Script_Execute_S21: API entry points not resolved "
				"(compilebuffer=%p call=%p pushroottable=%p)\n",
				(void*)v_sq_compilebuffer_s21,
				(void*)v_sq_call_s21,
				(void*)v_sq_pushroottable_s21);
		return false;
	}

	CSquirrelVM* s = nullptr;
	switch (ctx)
	{
	case SQCONTEXT::CLIENT: s = g_pClientScript; break;
	case SQCONTEXT::UI:     s = g_pUIScript;     break;
	default: break;
	}

	if (!s)
	{
		Warning(static_cast<eDLL_T>(ctx),
				"Script_Execute_S21: no CSquirrelVM* for context %d "
				"(VM not initialized yet?)\n", static_cast<int>(ctx));
		return false;
	}

	HSQUIRRELVM v = CSquirrelVM_GetHVM_S21(s);
	if (!v)
	{
		Warning(static_cast<eDLL_T>(ctx),
				"Script_Execute_S21: HSQUIRRELVM is NULL (s=0x%p)\n", (void*)s);
		return false;
	}

	SQBufState bufState(code);

	if (v_sq_compilebuffer_s21(v, &bufState, "unnamed", -1, 1) < 0)
	{
		Warning(static_cast<eDLL_T>(ctx),
				"Script_Execute_S21: sq_compilebuffer failed (code=\"%s\")\n",
				code);
		return false;
	}

	v_sq_pushroottable_s21(v);

	const long rc = v_sq_call_s21(v, 1, 0, 1);
	if (rc < 0)
	{
		Warning(static_cast<eDLL_T>(ctx),
				"Script_Execute_S21: sq_call failed (rc=%ld code=\"%s\")\n",
				rc, code);
		return false;
	}
	return true;
}

//=============================================================================
// script_client / script_ui live here: vscript_client.cpp is dropped from the link.
//=============================================================================

static void SQVM_ClientScript_S21_f(const CCommand& args)
{
	if (args.ArgC() >= 2)
		Script_Execute_S21(args.ArgS(), SQCONTEXT::CLIENT);
}

static void SQVM_UIScript_S21_f(const CCommand& args)
{
	if (args.ArgC() >= 2)
		Script_Execute_S21(args.ArgS(), SQCONTEXT::UI);
}

static ConCommand g_script_client_cmd_s21(
	"script_client", SQVM_ClientScript_S21_f,
	"Run input code as CLIENT script on the VM",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL);
static ConCommand g_script_ui_cmd_s21(
	"script_ui", SQVM_UIScript_S21_f,
	"Run input code as UI script on the VM",
	FCVAR_DEVELOPMENTONLY | FCVAR_CLIENTDLL);

//=============================================================================
// Prepend const bool DEVELOPER. RegisterConstant is runtime-only; compile sees SSS+0x41A8.
//-----------------------------------------------------------------------------
static long __fastcall sq_compilebuffer_S21(
	HSQUIRRELVM v, SQBufState* bufState,
	const SQChar* sourceName, long level, SQUnsignedInteger raiseError)
{
	if (!v_sq_compilebuffer_s21)
		return -1;

	if (!bufState || !bufState->buf || bufState->bufTail <= bufState->buf)
		return v_sq_compilebuffer_s21(v, bufState, sourceName, level, raiseError);

	const int devValue = DevInject_DevValue();
	const SQChar* const origBuf  = bufState->buf;
	const SQChar* const origTail = bufState->bufTail;
	const size_t        origSize = static_cast<size_t>(origTail - origBuf);

	size_t newSize = 0;
	char* const newBuf = DevInject_BuildSourceWithPrelude(
		origBuf, origSize, devValue, &newSize);
	if (!newBuf)
		return v_sq_compilebuffer_s21(v, bufState, sourceName, level, raiseError);

	SQBufState patched = *bufState;
	patched.buf     = newBuf;
	patched.bufTail = newBuf + newSize;
	patched.bufPos  = newBuf;

	static volatile LONG s_callCount = 0;
	const LONG n = InterlockedIncrement(&s_callCount);
	if (n <= 15)
	{
		const bool refsDev =
			(memmem_simple(origBuf, origSize, "DEVELOPER", 9) != nullptr);
		DevInjectDiag("compile #%ld src=\"%s\" origSize=%zu refsDEVELOPER=%d devVal=%d boolPrelude=1",
			(long)n, sourceName ? sourceName : "(null)", origSize,
			refsDev ? 1 : 0, devValue);
	}

	const long result = v_sq_compilebuffer_s21(
		v, &patched, sourceName, level, raiseError);
	free(newBuf);
	return result;
}

//-----------------------------------------------------------------------------
// File compile. Do not also RegisterConstant("DEVELOPER") -- redefines and aborts init.nut.
//-----------------------------------------------------------------------------
static int __fastcall sq_compile_topfn_S21(
	void* a1, char** a2, __int64 a3, const char* sourceName, int mode)
{
	if (!v_sq_compile_topfn_s21)
		return -1;

	if (!a2 || !*a2)
		return v_sq_compile_topfn_s21(a1, a2, a3, sourceName, mode);

	const int devValue = DevInject_DevValue();
	const char* const orig = *a2;
	const size_t origLen = strlen(orig);

	size_t newLen = 0;
	char* const newBuf = DevInject_BuildSourceWithPrelude(
		orig, origLen, devValue, &newLen);
	// null = OOM or already has prelude -- compile original
	if (!newBuf)
		return v_sq_compile_topfn_s21(a1, a2, a3, sourceName, mode);

	*a2 = newBuf;

	static volatile LONG s_n = 0;
	if (InterlockedIncrement(&s_n) <= 8)
		DevInjectDiag("topfn bool DEVELOPER=%d VM=0x%p name=\"%s\" (after untyped if any)",
			devValue, a1, sourceName ? sourceName : "(null)");

	const int result = v_sq_compile_topfn_s21(a1, a2, a3, sourceName, mode);
	*a2 = const_cast<char*>(orig);
	free(newBuf);
	return result;
}

//-----------------------------------------------------------------------------
// DllMain attach so the hook is live before any thread is spawned.
//-----------------------------------------------------------------------------
void VSquirrelS21Core_PreInit_AttachCompileBufferHook()
{
	// Non-atomic dedup -- DllMain is single-threaded so racing isn't an
	// issue; this is just a defensive re-entry guard.
	if (s_compileBufferEarlyAttached) { s_earlyAttachStatus = "already attached"; return; }

	s_earlyAttachStatus = "start";
	DevInjectDiag("PREINIT enter");

	HMODULE hExe = GetModuleHandleA(NULL);
	if (!hExe)
	{
		s_earlyAttachStatus = "GetModuleHandleA(NULL) returned NULL";
		DevInjectDiag("PREINIT FAIL: %s", s_earlyAttachStatus);
		return;
	}

	CModule localExe;
	localExe.InitFromBase(reinterpret_cast<QWORD>(hExe));

	Module_FindPattern(localExe,
		"48 8B C4 57 41 55 48 83 EC 58 80 3D ?? ?? ?? ?? 00 "
		"48 89 58 08 48 89 68 10 48 89 70 18 48 8B F1")
		.GetPtr(v_sq_compilebuffer_s21);

	if (!v_sq_compilebuffer_s21)
	{
		s_earlyAttachStatus = "pattern did not match";
		DevInjectDiag("PREINIT FAIL: %s (exeBase=0x%p)",
			s_earlyAttachStatus, (void*)hExe);
		return;
	}
	s_earlyAttachAddr = reinterpret_cast<uintptr_t>(v_sq_compilebuffer_s21);
	DevInjectDiag("PREINIT resolved sq_compilebuffer=0x%p", (void*)s_earlyAttachAddr);

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	s_earlyAttachRC = DetourAttach(
		reinterpret_cast<PVOID*>(&v_sq_compilebuffer_s21),
		reinterpret_cast<PVOID>(&sq_compilebuffer_S21));
	if (s_earlyAttachRC != NO_ERROR)
	{
		DetourTransactionAbort();
		v_sq_compilebuffer_s21 = nullptr;
		s_earlyAttachStatus = "DetourAttach failed";
		DevInjectDiag("PREINIT FAIL: DetourAttach rc=%ld", s_earlyAttachRC);
		return;
	}
	const LONG commitRC = DetourTransactionCommit();
	if (commitRC != NO_ERROR)
	{
		s_earlyAttachRC = commitRC;
		v_sq_compilebuffer_s21 = nullptr;
		s_earlyAttachStatus = "DetourTransactionCommit failed";
		DevInjectDiag("PREINIT FAIL: commit rc=%ld", commitRC);
		return;
	}

	s_earlyAttachStatus = "OK";
	InterlockedExchange(&s_compileBufferEarlyAttached, 1);
	DevInjectDiag("PREINIT OK: trampoline=0x%p", (void*)v_sq_compilebuffer_s21);

	Module_FindPattern(localExe,
		"48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 "
		"80 3D ?? ?? ?? ?? 00 4D 8B E9 4D 8B E0")
		.GetPtr(v_sq_compile_topfn_s21);

	if (!v_sq_compile_topfn_s21)
	{
		DevInjectDiag("PREINIT topfn FAIL: pattern did not match");
		return;
	}
	DevInjectDiag("PREINIT topfn resolved=0x%p", (void*)v_sq_compile_topfn_s21);

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	const LONG topAttachRC = DetourAttach(
		reinterpret_cast<PVOID*>(&v_sq_compile_topfn_s21),
		reinterpret_cast<PVOID>(&sq_compile_topfn_S21));
	if (topAttachRC != NO_ERROR)
	{
		DetourTransactionAbort();
		v_sq_compile_topfn_s21 = nullptr;
		DevInjectDiag("PREINIT topfn FAIL: DetourAttach rc=%ld", topAttachRC);
		return;
	}
	const LONG topCommitRC = DetourTransactionCommit();
	if (topCommitRC != NO_ERROR)
	{
		v_sq_compile_topfn_s21 = nullptr;
		DevInjectDiag("PREINIT topfn FAIL: commit rc=%ld", topCommitRC);
		return;
	}

	InterlockedExchange(&s_compileTopfnEarlyAttached, 1);
	DevInjectDiag("PREINIT topfn OK: trampoline=0x%p", (void*)v_sq_compile_topfn_s21);

	if (!CSquirrelVM__RegisterConstant)
	{
		Module_FindPattern(localExe,
			"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 4C 8B 51 30")
			.GetPtr(CSquirrelVM__RegisterConstant);
		DevInjectDiag("PREINIT RegisterConstant resolved=0x%p",
			(void*)CSquirrelVM__RegisterConstant);
	}
}

//=============================================================================
// IDetour implementation
//=============================================================================

void VSquirrelS21Core::GetAdr(void) const
{
	SDK_Log("[DEV-INJECT-PREINIT] status=\"%s\" attached=%ld "
			"trampoline=0x%p attachRC=%ld\n",
			s_earlyAttachStatus ? s_earlyAttachStatus : "(null)",
			(long)s_compileBufferEarlyAttached,
			(void*)s_earlyAttachAddr,
			s_earlyAttachRC);

	LogFunAdr("Script_PrintFunc_S21",   v_Script_PrintFunc);
	LogFunAdr("Script_WarningFunc_S21", v_Script_WarningFunc);
	LogFunAdr("SQVM_CompileError_S21",  v_SQVM_CompileError);
	LogFunAdr("SQVM_LogicError_S21",    v_SQVM_LogicError);
	LogFunAdr("SQVM_GetErrorLine_S21",  v_SQVM_GetErrorLine);
	LogFunAdr("sqstd_format_S21",       v_sqstd_format);
	LogFunAdr("sqstd_aux_printerror_S21", v_sqstd_aux_printerror);
	LogFunAdr("CSquirrelVM::RegisterFunction_S21", CSquirrelVM__RegisterFunction);
	LogFunAdr("CSquirrelVM::RegisterConstant_S21", CSquirrelVM__RegisterConstant);
	LogFunAdr("sq_compilebuffer_S21",  v_sq_compilebuffer_s21);
	LogFunAdr("sq_call_S21",           v_sq_call_s21);
	LogFunAdr("sq_pushroottable_S21",  v_sq_pushroottable_s21);
}

void VSquirrelS21Core::GetFun(void) const
{
	// Script_PrintFunc -- on S21 (unique match).
	Module_FindPattern(g_GameDll,
		"48 8B C4 48 89 50 10 4C 89 40 18 4C 89 48 20 53 56 57 "
		"48 81 EC 30 08 ?? ?? 48 8B DA 48 8D 70 18 48 8B F9 E8 "
		"?? ?? ?? FF 48 89 74 24 28 48 8D 54 24 30 33")
		.GetPtr(v_Script_PrintFunc);

	// Script_WarningFunc -- on S21 (unique match; note same
	// function body also matches the SQVM_WarningCmd pattern).
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC ? 33 DB 48 8D 44 24 ? 4C 8D 4C 24")
		.GetPtr(v_Script_WarningFunc);

	// SQVM_CompileError -- on S21 (unique match).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? "
		"41 56 48 81 EC ?? ?? ?? ?? 48 8B D9 4C 8B F2")
		.GetPtr(v_SQVM_CompileError);

	// SQVM_LogicError -- on S21 (unique match).
	Module_FindPattern(g_GameDll,
		"48 83 EC 38 F2 0F 10 05 ?? ?? ?? ??")
		.GetPtr(v_SQVM_LogicError);

	// sqstd_format -- on S21 (unique match).
	// Resolve-only (no detour); needed by Script_WarningFunc_S21.
	Module_FindPattern(g_GameDll,
		"4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 53 55 56 57 "
		"41 54 41 55 41 56 41 57 48 83 EC ?? 48 8B")
		.GetPtr(v_sqstd_format);

	Module_FindPattern(g_GameDll,
		"48 8B C4 55 56 48 8D A8 ?? ?? FF FF 48 81 EC ?? ?? ?? ?? "
		"33 F6 48 89 58 ?? 48 89 78")
		.GetPtr(v_SQVM_GetErrorLine);

	Module_FindPattern(g_GameDll,
		"40 55 53 56 57 41 56 41 57 48 8D AC 24 ?? ?? FF FF "
		"B8 ?? ?? 00 00 E8 ?? ?? ?? ?? 48 2B E0 FF 05")
		.GetPtr(v_sqstd_aux_printerror);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 "
		"48 8B EC 48 83 EC 60 80 3D ?? ?? ?? ?? 00 48 8B F1")
		.GetPtr(v_CSquirrelVM_Init_S21);

	Module_FindPattern(g_GameDll,
		"48 83 EC 38 45 0F B6 C8")
		.GetPtr(CSquirrelVM__RegisterFunction);

	// Short S3 pattern hits two funcs; + mov r10,[rcx+30h] is unique (m_hVM +0x30).
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 4C 8B 51 30")
		.GetPtr(CSquirrelVM__RegisterConstant);

	// Skip if DllMain already attached; re-resolve clobbers the trampoline.
	if (!InterlockedCompareExchange(&s_compileBufferEarlyAttached, 1, 1))
	{
		Module_FindPattern(g_GameDll,
			"48 8B C4 57 41 55 48 83 EC 58 80 3D ?? ?? ?? ?? 00 "
			"48 89 58 08 48 89 68 10 48 89 70 18 48 8B F1")
			.GetPtr(v_sq_compilebuffer_s21);
	}

	Module_FindPattern(g_GameDll,
		"4C 8B DC 49 89 5B 08 49 89 6B 10 49 89 73 18 57 "
		"48 83 EC 40 8B FA 45 8B D1 44 8B 49 68")
		.GetPtr(v_sq_call_s21);

	// sq_pushroottable (S21) + share into VSquirrelAPI global for sqapi wrappers
	// (VSquirrelAPI is not registered on the client).
	Module_FindPattern(g_GameDll,
		"48 83 EC 28 8B 51 ?? 44 8B C2")
		.GetPtr(v_sq_pushroottable_s21);
	if (v_sq_pushroottable_s21 && !v_sq_pushroottable)
		v_sq_pushroottable = v_sq_pushroottable_s21;

	// sq_push* — S21 OT tags (int 0x40000002 / float 0x40000004 / bool 0x10000008)
	// + _top@+0x68. n=1 patterns; fills globals used by sqapi.cpp wrappers.
	Module_FindPattern(g_GameDll,
		"48 83 EC 38 33 C0 48 C7 44 24 20 02 00 00 40 48")
		.GetPtr(v_sq_pushinteger);
	Module_FindPattern(g_GameDll,
		"48 83 EC 38 8B 51 68 33 C0 48 89 44 24 28 F3 0F 11 4C 24 28 48 C7 44 24 20 04 00 00 40")
		.GetPtr(v_sq_pushfloat);
	Module_FindPattern(g_GameDll,
		"48 83 EC 38 33 C0 48 C7 44 24 20 08 00 00 10 48 89 44 24 28 85 D2 8B 51 68 0F 95 C0")
		.GetPtr(v_sq_pushbool);
	Module_FindPattern(g_GameDll,
		"40 56 48 83 EC 20 48 8B F1 48 85 D2 0F 84 92 00 00 00")
		.GetPtr(v_sq_pushstring);

	// Tail 0x80000040 tag write is unique vs sibling sq_new*.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B 49 50 E8 ?? ?? ?? ?? "
		"48 8B F8 FF 40 10 8B 53 68 8D 42 01 48 C1 E2 04 48 03 53 78 "
		"89 43 68 FF 47 10 83 3A 00 7D 23 48 8B 4A 08 C7 02 40 00 00 80")
		.GetPtr(v_sq_newarray);

	// sq_arrayappend -- opens with the two-parameter check
	// (_top@+0x68 minus _bottom@+0x44, compared against 2).
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 8B 41 68 48 8B D9 2B 41 44 83 F8 02 7D")
		.GetPtr(v_sq_arrayappend);

	DevInjectDiag("sq_push*(S21) int=%p float=%p bool=%p str=%p root=%p "
		"newarray=%p arrayappend=%p",
		(void*)v_sq_pushinteger, (void*)v_sq_pushfloat,
		(void*)v_sq_pushbool, (void*)v_sq_pushstring,
		(void*)v_sq_pushroottable_s21,
		(void*)v_sq_newarray, (void*)v_sq_arrayappend);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 57 48 83 EC 50 48 8B 59 30 4D 8B C8 48 8B CB 4C 8D 44 24 ?? 48 8B FA E8 ?? ?? ?? ??")
		.GetPtr(CSquirrelVM__FindFunction);
	Module_FindPattern(g_GameDll,
		"41 54 48 83 EC 50 80 3D ?? ?? ?? ?? 00 48 89 6C 24 68 48 8B EA 48 89 74 24 70 49 8B F0 48 89 7C 24 48 48 8B F9 4C 89 6C 24 ??")
		.GetPtr(CSquirrelVM__ExecuteFunction);
}

void VSquirrelS21Core::Detour(const bool bAttach) const
{
	DetourSetup(&v_Script_PrintFunc,      &Script_PrintFunc_S21,      bAttach);
	DetourSetup(&v_Script_WarningFunc,    &Script_WarningFunc_S21,    bAttach);
	DetourSetup(&v_SQVM_CompileError,     &SQVM_CompileError_S21,     bAttach);
	DetourSetup(&v_SQVM_LogicError,       &SQVM_LogicError_S21,       bAttach);
	DetourSetup(&v_sqstd_aux_printerror,  &sqstd_aux_printerror_S21,  bAttach);
	DetourSetup(&v_CSquirrelVM_Init_S21,  &CSquirrelVM_Init_S21,      bAttach);

	// DllMain already attached unless the pre-init path failed.
	if (!InterlockedCompareExchange(&s_compileBufferEarlyAttached, 1, 1))
	{
		DetourSetup(&v_sq_compilebuffer_s21, &sq_compilebuffer_S21, bAttach);
	}
}
