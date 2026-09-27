//=============================================================================//
//
// Purpose: mod script wraps (see header). The rewrite runs on the source text
//          the compiler is about to read, so base files stay untouched on
//          disk and the typed compiler checks every generated call.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier0/memaddr.h"
#include "tier0/module.h"
#include "tier1/strtools.h"
#include "pluginsystem/modsystem.h"
#include "script_wraps.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

struct ScriptWrapChain_t
{
	std::string target;
	std::string replaceFn;
	std::string replaceMod;
	std::vector<std::string> wraps; // load order; the first loaded sits next to the base
	std::vector<std::string> mods;
	std::string firstFile;
	int nDefinitions;
	int nFiles;
};

static std::mutex s_wrapMutex;
static std::vector<ScriptWrapChain_t> s_wrapChains;
static std::vector<char*> s_wrapSources;
static int s_wrapContext = -1;

static const char* ScriptWraps_ContextName(const int context)
{
	switch (context)
	{
	case 0: return "SERVER";
	case 1: return "CLIENT";
	case 2: return "UI";
	default: return "NONE";
	}
}

static inline bool ScriptWraps_IsIdentChar(const char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void ScriptWraps_Trim(std::string& s)
{
	const size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
	{
		s.clear();
		return;
	}
	const size_t e = s.find_last_not_of(" \t\r\n");
	s = s.substr(b, e - b + 1);
}

static void ScriptWraps_BeginLocked(const int context)
{
	s_wrapChains.clear();
	s_wrapContext = context;

	if (!ModSystem()->IsEnabled())
		return;

	const int nBit = 1 << context;

	ModSystem()->LockModList();
	FOR_EACH_VEC(ModSystem()->GetResolvedModList(), i)
	{
		const CModSystem::ModInstance_t* const mod = ModSystem()->GetResolvedModList()[i];
		if (!mod || !mod->IsEnabled() || !mod->hasPrecompiledScripts)
			continue;

		FOR_EACH_VEC(mod->scriptWraps, w)
		{
			const CModSystem::ModInstance_t::ScriptWrap_t& wrap = mod->scriptWraps[w];
			if (!(wrap.contextMask & nBit))
				continue;

			ScriptWrapChain_t* pChain = nullptr;
			for (ScriptWrapChain_t& chain : s_wrapChains)
			{
				if (chain.target == wrap.target.String())
				{
					pChain = &chain;
					break;
				}
			}

			if (!pChain)
			{
				// Every chain's target is searched in every compiled file.
				if (s_wrapChains.size() >= MOD_WRAP_MAX_CHAINS)
				{
					Error(eDLL_T::MODSYSTEM, NO_ERROR, "[MOD-WRAP] more than %d wrapped functions across mods; '%s' from '%s' not armed\n",
						MOD_WRAP_MAX_CHAINS, wrap.target.String(), mod->id.String());
					continue;
				}
				s_wrapChains.emplace_back();
				pChain = &s_wrapChains.back();
				pChain->target = wrap.target.String();
				pChain->nDefinitions = 0;
				pChain->nFiles = 0;
			}

			if (wrap.replace)
			{
				if (!pChain->replaceFn.empty())
				{
					Error(eDLL_T::MODSYSTEM, NO_ERROR,
						"[MOD-WRAP] %s: mods '%s' and '%s' both replace '%s'; neither replace is applied\n",
						ScriptWraps_ContextName(context), pChain->replaceMod.c_str(), mod->id.String(),
						pChain->target.c_str());
					pChain->replaceMod = "*";
					continue;
				}
				if (pChain->replaceMod != "*")
				{
					pChain->replaceFn = wrap.function.String();
					pChain->replaceMod = mod->id.String();
				}
				continue;
			}

			pChain->wraps.push_back(wrap.function.String());
			pChain->mods.push_back(mod->id.String());
		}
	}
	ModSystem()->UnlockModList();

	for (ScriptWrapChain_t& chain : s_wrapChains)
	{
		if (chain.replaceMod == "*")
			chain.replaceFn.clear();
	}

	if (!s_wrapChains.empty())
	{
		Msg(eDLL_T::MODSYSTEM, "[MOD-WRAP] %s: %zu wrapped function(s) armed\n",
			ScriptWraps_ContextName(context), s_wrapChains.size());
	}
}

static void ScriptWraps_ReleaseLocked(void)
{
	for (char* const p : s_wrapSources)
		free(p);
	s_wrapSources.clear();
}

static void ScriptWraps_EndLocked(void)
{
	for (const ScriptWrapChain_t& chain : s_wrapChains)
	{
		if (chain.nDefinitions == 0)
		{
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-WRAP] %s: no compiled script defines '%s'; its wraps never run\n",
				ScriptWraps_ContextName(s_wrapContext), chain.target.c_str());
		}
		else if (chain.nFiles > 1)
		{
			Warning(eDLL_T::MODSYSTEM,
				"[MOD-WRAP] %s: '%s' is defined in %d files; every definition was wrapped\n",
				ScriptWraps_ContextName(s_wrapContext), chain.target.c_str(), chain.nFiles);
		}
	}

	s_wrapChains.clear();
	s_wrapContext = -1;
	ScriptWraps_ReleaseLocked();
}

void ScriptWraps_BeginContext(const int context)
{
	if (context < 0 || context > 2)
		return;

	std::lock_guard<std::mutex> lock(s_wrapMutex);
	ScriptWraps_EndLocked();
	ScriptWraps_BeginLocked(context);
}

void ScriptWraps_EndContext(const int context)
{
	std::lock_guard<std::mutex> lock(s_wrapMutex);
	if (s_wrapContext == context)
		ScriptWraps_EndLocked();
}

void ScriptWraps_ReleaseSources(void)
{
	std::lock_guard<std::mutex> lock(s_wrapMutex);
	ScriptWraps_ReleaseLocked();
}

//-----------------------------------------------------------------------------
// Signature parsing over the base definition. Anything the generated chain
// cannot forward exactly (variadics, untyped parameters) leaves the file as is.
//-----------------------------------------------------------------------------
struct ScriptWrapDef_t
{
	size_t lineStart;
	size_t namePos;
	std::string returnType;
	std::string params;              // declaration text, defaults kept, one line
	std::vector<std::string> types;
	std::vector<std::string> names;
};

// Skips a string literal or comment starting at i; returns the index after it,
// or i when text[i] starts neither.
static size_t ScriptWraps_SkipLiteral(const std::string& text, size_t i)
{
	const size_t n = text.size();
	if (text[i] == '"' || text[i] == '\'')
	{
		const char q = text[i++];
		while (i < n && text[i] != q && text[i] != '\n')
		{
			if (text[i] == '\\' && i + 1 < n)
				++i;
			++i;
		}
		return i < n ? i + 1 : n;
	}
	if (text[i] == '/' && i + 1 < n && text[i + 1] == '/')
	{
		const size_t e = text.find('\n', i);
		return e == std::string::npos ? n : e;
	}
	if (text[i] == '/' && i + 1 < n && text[i + 1] == '*')
	{
		const size_t e = text.find("*/", i + 2);
		return e == std::string::npos ? n : e + 2;
	}
	return i;
}

static bool ScriptWraps_SplitParams(const std::string& raw, ScriptWrapDef_t& def)
{
	// Flatten: comments out, whitespace runs to one space.
	std::string flat;
	for (size_t i = 0; i < raw.size();)
	{
		const size_t j = ScriptWraps_SkipLiteral(raw, i);
		if (j != i)
		{
			if (raw[i] == '"' || raw[i] == '\'')
				flat.append(raw, i, j - i);
			else
				flat.push_back(' ');
			i = j;
			continue;
		}
		const char c = raw[i++];
		flat.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
	}
	ScriptWraps_Trim(flat);
	def.params = flat;

	if (flat.empty())
		return true;

	std::vector<std::string> parts;
	int depth = 0;
	size_t start = 0;
	for (size_t i = 0; i < flat.size(); ++i)
	{
		const size_t j = ScriptWraps_SkipLiteral(flat, i);
		if (j != i)
		{
			i = j - 1;
			continue;
		}
		const char c = flat[i];
		if (c == '(' || c == '<' || c == '[' || c == '{')
			depth++;
		else if (c == ')' || c == '>' || c == ']' || c == '}')
			depth--;
		else if (c == ',' && depth == 0)
		{
			parts.push_back(flat.substr(start, i - start));
			start = i + 1;
		}
	}
	parts.push_back(flat.substr(start));

	for (std::string part : parts)
	{
		if (part.find("...") != std::string::npos)
			return false;

		int d = 0;
		for (size_t i = 0; i < part.size(); ++i)
		{
			const char c = part[i];
			if (c == '(' || c == '<' || c == '[' || c == '{')
				d++;
			else if (c == ')' || c == '>' || c == ']' || c == '}')
				d--;
			else if (c == '=' && d == 0)
			{
				part.resize(i);
				break;
			}
		}
		ScriptWraps_Trim(part);

		size_t e = part.size();
		size_t b = e;
		while (b > 0 && ScriptWraps_IsIdentChar(part[b - 1]))
			--b;
		if (b == e || b == 0)
			return false;

		std::string type = part.substr(0, b);
		ScriptWraps_Trim(type);
		if (type.empty())
			return false;

		def.types.push_back(type);
		def.names.push_back(part.substr(b, e - b));
	}

	return true;
}

static void ScriptWraps_FindDefinitions(const std::string& text, const std::string& target,
	std::vector<ScriptWrapDef_t>& out, const char* const pszSourceName)
{
	const size_t nTarget = target.size();
	for (size_t pos = text.find(target); pos != std::string::npos; pos = text.find(target, pos + nTarget))
	{
		const size_t after = pos + nTarget;
		if (after < text.size() && ScriptWraps_IsIdentChar(text[after]))
			continue;

		size_t b = pos;
		while (b > 0 && (text[b - 1] == ' ' || text[b - 1] == '\t'))
			--b;
		if (b == pos || b < 8 || text.compare(b - 8, 8, "function") != 0)
			continue;
		const size_t fnPos = b - 8;
		if (fnPos > 0 && ScriptWraps_IsIdentChar(text[fnPos - 1]))
			continue;

		size_t q = after;
		while (q < text.size() && (text[q] == ' ' || text[q] == '\t'))
			++q;
		if (q >= text.size() || text[q] != '(')
			continue;

		const size_t nl = fnPos > 0 ? text.rfind('\n', fnPos - 1) : std::string::npos;
		const size_t lineStart = nl == std::string::npos ? 0 : nl + 1;

		std::string prefix = text.substr(lineStart, fnPos - lineStart);
		ScriptWraps_Trim(prefix);
		if (prefix.empty() || prefix.find_first_of("=;{}\"#") != std::string::npos
			|| prefix.find("//") != std::string::npos || prefix.compare(0, 6, "global") == 0)
		{
			continue;
		}

		int depth = 0;
		size_t close = std::string::npos;
		for (size_t i = q; i < text.size(); ++i)
		{
			const size_t j = ScriptWraps_SkipLiteral(text, i);
			if (j != i)
			{
				i = j - 1;
				continue;
			}
			if (text[i] == '(')
				depth++;
			else if (text[i] == ')' && --depth == 0)
			{
				close = i;
				break;
			}
		}
		if (close == std::string::npos)
			continue;

		size_t k = close + 1;
		while (k < text.size())
		{
			const size_t j = ScriptWraps_SkipLiteral(text, k);
			if (j != k)
			{
				k = j;
				continue;
			}
			if (text[k] == ' ' || text[k] == '\t' || text[k] == '\r' || text[k] == '\n')
			{
				++k;
				continue;
			}
			break;
		}
		if (k >= text.size() || text[k] != '{')
			continue;

		ScriptWrapDef_t def;
		def.lineStart = lineStart;
		def.namePos = pos;
		def.returnType = prefix;
		if (!ScriptWraps_SplitParams(text.substr(q + 1, close - q - 1), def))
		{
			Error(eDLL_T::MODSYSTEM, NO_ERROR,
				"[MOD-WRAP] '%s' in '%s' has variadic or untyped parameters; it cannot be wrapped\n",
				target.c_str(), pszSourceName ? pszSourceName : "?");
			continue;
		}

		out.push_back(def);
	}
}

static std::string ScriptWraps_BuildChain(const ScriptWrapChain_t& chain, const ScriptWrapDef_t& def)
{
	const std::string& ret = def.returnType;
	const std::string retKw = ret == "void" ? "" : "return ";

	std::string args;
	for (size_t i = 0; i < def.names.size(); ++i)
	{
		if (i)
			args += ", ";
		args += def.names[i];
	}

	const std::string params = def.params.empty() ? "" : " " + def.params + " ";
	const std::string inner = chain.replaceFn.empty() ? chain.target + "__mw_base" : chain.replaceFn;

	std::string gen;
	if (chain.wraps.empty())
	{
		gen += ret + " function " + chain.target + "(" + params + ") { " + retKw + inner + "( " + args + " ) } ";
		return gen;
	}

	const size_t n = chain.wraps.size();
	for (size_t i = 0; i < n; ++i)
	{
		const std::string self = i + 1 == n ? chain.target : chain.target + "__mw_" + std::to_string(i + 1);
		const std::string next = i == 0 ? inner : chain.target + "__mw_" + std::to_string(i);
		gen += ret + " function " + self + "(" + params + ") { " + retKw + chain.wraps[i] + "( " + next
			+ (args.empty() ? "" : ", " + args) + " ) } ";
	}
	return gen;
}

char* ScriptWraps_RewriteSource(const int context, const char* const pszSource,
	const size_t nSourceLen, const char* const pszSourceName, size_t* const pOutLen)
{
	if (!pszSource || !nSourceLen || nSourceLen > MOD_MAX_SCRIPT_BYTES)
		return nullptr;

	std::lock_guard<std::mutex> lock(s_wrapMutex);

#if defined(CLIENT_DLL)
	// The client has no end-of-compile hook per VM: a new VM context starts a new set.
	if (context != s_wrapContext && context >= 0 && context <= 2)
	{
		ScriptWraps_EndLocked();
		ScriptWraps_BeginLocked(context);
	}
#endif // CLIENT_DLL

	if (context != s_wrapContext || s_wrapChains.empty())
		return nullptr;

	std::string text(pszSource, nSourceLen);

	struct Edit_t { size_t pos; size_t eraseLen; std::string insert; };
	std::vector<Edit_t> edits;

	for (ScriptWrapChain_t& chain : s_wrapChains)
	{
		if (text.find(chain.target) == std::string::npos)
			continue;

		std::vector<ScriptWrapDef_t> defs;
		ScriptWraps_FindDefinitions(text, chain.target, defs, pszSourceName);
		if (defs.empty())
			continue;

		for (const ScriptWrapDef_t& def : defs)
		{
			edits.push_back({ def.namePos, chain.target.size(), chain.target + "__mw_base" });
			edits.push_back({ def.lineStart, 0, ScriptWraps_BuildChain(chain, def) });
		}

		chain.nDefinitions += static_cast<int>(defs.size());
		if (chain.firstFile != (pszSourceName ? pszSourceName : ""))
		{
			chain.nFiles++;
			chain.firstFile = pszSourceName ? pszSourceName : "";
		}

		std::string order;
		for (size_t i = chain.wraps.size(); i-- > 0;)
			order += chain.mods[i] + " > ";
		order += chain.replaceFn.empty() ? "base" : chain.replaceMod + " (replace)";

		Msg(eDLL_T::MODSYSTEM, "[MOD-WRAP] %s: '%s' in '%s' -> %s\n", ScriptWraps_ContextName(context),
			chain.target.c_str(), pszSourceName ? pszSourceName : "?", order.c_str());
	}

	if (edits.empty())
		return nullptr;

	if (edits.size() > MOD_WRAP_MAX_EDITS_PER_FILE)
	{
		Error(eDLL_T::MODSYSTEM, NO_ERROR, "[MOD-WRAP] %s: '%s' has %zu wrap edits (max %zu); file left unwrapped\n",
			ScriptWraps_ContextName(context), pszSourceName ? pszSourceName : "?", edits.size(), size_t(MOD_WRAP_MAX_EDITS_PER_FILE));
		return nullptr;
	}

	std::stable_sort(edits.begin(), edits.end(), [](const Edit_t& a, const Edit_t& b) { return a.pos < b.pos; });

	std::string rewritten;
	size_t nExtra = 0;
	for (const Edit_t& edit : edits)
		nExtra += edit.insert.size();
	rewritten.reserve(text.size() + nExtra);

	size_t cursor = 0;
	for (const Edit_t& edit : edits)
	{
		if (edit.pos < cursor || edit.pos > text.size())
			continue;

		rewritten.append(text, cursor, edit.pos - cursor);
		rewritten += edit.insert;
		cursor = edit.pos + edit.eraseLen;
	}
	rewritten.append(text, cursor, std::string::npos);
	text.swap(rewritten);

	char* const pOut = static_cast<char*>(malloc(text.size() + 1));
	if (!pOut)
		return nullptr;

	memcpy(pOut, text.c_str(), text.size() + 1);
	if (pOutLen)
		*pOutLen = text.size();

#if !defined(CLIENT_DLL)
	s_wrapSources.push_back(pOut);
#endif // !CLIENT_DLL
	return pOut;
}

#if !defined(CLIENT_DLL)
//-----------------------------------------------------------------------------
// Dedi: the compiler takes its source through this call for every script file
// (both load paths of the file loader) and reads it after the call returns, so
// the rewritten buffer lives until the file loader returns.
//-----------------------------------------------------------------------------
struct ScriptWrapBufState_t
{
	const char* buf;
	const char* bufTail;
	const char* bufPos;
};

static void(*v_SQCompiler_SetSource)(void* compiler, void* v, ScriptWrapBufState_t* bufState, const char* pszName) = nullptr;

static void SQCompiler_SetSource(void* compiler, void* v, ScriptWrapBufState_t* bufState, const char* pszName)
{
	if (bufState && bufState->buf && bufState->bufTail > bufState->buf)
	{
		size_t nLen = 0;
		char* const pRewritten = ScriptWraps_RewriteSource(0, bufState->buf,
			static_cast<size_t>(bufState->bufTail - bufState->buf), pszName, &nLen);

		if (pRewritten)
		{
			bufState->buf = pRewritten;
			bufState->bufTail = pRewritten + nLen;
			bufState->bufPos = pRewritten;
		}
	}

	v_SQCompiler_SetSource(compiler, v, bufState, pszName);
}

void VScriptWraps::GetAdr(void) const
{
	LogFunAdr("SQCompiler_SetSource", v_SQCompiler_SetSource);
}

void VScriptWraps::GetFun(void) const
{
	// Stores the buffer state at compiler+0x70 and resets the lexer line/column.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 "
		"48 8B F1 4C 89 41 70 C7 41 38 01 00 00 00")
		.GetPtr(v_SQCompiler_SetSource);

	if (!v_SQCompiler_SetSource)
		Warning(eDLL_T::MODSYSTEM, "[MOD-WRAP] compiler source entry unresolved; mod script wraps are off\n");
}

void VScriptWraps::Detour(const bool bAttach) const
{
	if (v_SQCompiler_SetSource)
		DetourSetup(&v_SQCompiler_SetSource, &SQCompiler_SetSource, bAttach);
}
#endif // !CLIENT_DLL
