//=============================================================================//
//
// Purpose: player class-var natives. Movement tuning lives in a per-player
// class-var block; this reaches that block from script.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/memaddr.h"
#include "tier1/cvar.h"
// SCRIPT_REGISTER_FUNC is a macro the vsquirrel.h register template expands, so
// this header has to come first.
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript_server.h"
#include "vscript_server_natives.h"
#include "classvar_natives.h"

//-----------------------------------------------------------------------------
// CPlayer fields, read off the _setClassVarServer dispatch.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t CPLAYER_OFF_CLASSVAR_BLOCK = 0x5F08; // class-var block ptr
static constexpr ptrdiff_t CPLAYER_OFF_BLOCK_COUNT    = 0x48C;  // m_iHealth
static constexpr ptrdiff_t CPLAYER_OFF_BLOCK_SELECTOR = 0x234;  // m_fFlags, bit 1

// Descriptor lookup: (table, name, &type, &offset, &namePtr) -> found.
static bool (*v_ClassVar_Find)(void* pTable, const char* pszName,
	uint16_t* pType, uint16_t* pOffset, uint64_t* pNamePtr) = nullptr;

// Value write. Args 2 and 3 are scratch the engine never reads.
static void (*v_ClassVar_Write)(uintptr_t nBase, uintptr_t, uintptr_t,
	uintptr_t nType, uintptr_t nOffset, const char* pszValue) = nullptr;

// Copies the block into the replicated player props and dirty-marks each one.
static void (*v_ClassVar_Apply)(void* pPlayer) = nullptr;

static void** g_ppClassVarTablePrimary = nullptr;
static void** g_ppClassVarTableSecondary = nullptr;
static int32_t* g_pClassVarSecondaryBase = nullptr;
static int32_t* g_pClassVarSecondaryStride = nullptr;

static bool s_bClassVarResolved = false;
static bool s_bClassVarUsable = false;

static ConVar bridge_classvar_log("bridge_classvar_log", "0", FCVAR_DEVELOPMENTONLY,
	"Log every Player_SetClassVar key/value applied on the dedi.");

//-----------------------------------------------------------------------------
// Purpose: resolve the class-var surface out of the S3 dedi
//-----------------------------------------------------------------------------
static void ServerScript_ResolveClassVar(void)
{
	if (s_bClassVarResolved)
		return;
	s_bClassVarResolved = true;

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 4C 89 44 24 ?? "
		"57 41 54 41 55 41 56 41 57 8B 71")
		.GetPtr(v_ClassVar_Find);

	Module_FindPattern(g_GameDll, "40 53 48 83 EC ?? 4C 8B 5C 24 ?? 48 8B D9")
		.GetPtr(v_ClassVar_Write);

	Module_FindPattern(g_GameDll, "40 53 57 41 54 48 83 EC ?? 44 8B 05")
		.GetPtr(v_ClassVar_Apply);

	// Four globals only reachable through command dispatch; offsets are from function start.
	const CMemory dispatch = Module_FindPattern(g_GameDll,
		"41 56 48 83 EC ?? 8B 05 ?? ?? ?? ?? 4C 8B F1 FF C0");

	if (dispatch.GetPtr())
	{
		// +0xF4  48 8B 0D  mov rcx, cs:<primary table>
		g_ppClassVarTablePrimary = dispatch.Offset(0xF4)
			.ResolveRelativeAddress(3, 7).RCast<void**>();
		// +0x138 48 8B 0D  mov rcx, cs:<secondary table>
		g_ppClassVarTableSecondary = dispatch.Offset(0x138)
			.ResolveRelativeAddress(3, 7).RCast<void**>();
		// +0xA9  44 8B 05  mov r8d, cs:<secondary base>
		g_pClassVarSecondaryBase = dispatch.Offset(0xA9)
			.ResolveRelativeAddress(3, 7).RCast<int32_t*>();
		// +0xCE  8B 15     mov edx, cs:<secondary stride>
		g_pClassVarSecondaryStride = dispatch.Offset(0xCE)
			.ResolveRelativeAddress(2, 6).RCast<int32_t*>();
	}

	s_bClassVarUsable = v_ClassVar_Find && v_ClassVar_Write && v_ClassVar_Apply
		&& g_ppClassVarTablePrimary && g_ppClassVarTableSecondary
		&& g_pClassVarSecondaryBase && g_pClassVarSecondaryStride;

	if (!s_bClassVarUsable)
	{
		Warning(eDLL_T::SERVER,
			"[CLASSVAR] surface unresolved (find=%p write=%p apply=%p "
			"primary=%p secondary=%p) -- Player_SetClassVar is a no-op\n",
			reinterpret_cast<void*>(v_ClassVar_Find),
			reinterpret_cast<void*>(v_ClassVar_Write),
			reinterpret_cast<void*>(v_ClassVar_Apply),
			reinterpret_cast<void*>(g_ppClassVarTablePrimary),
			reinterpret_cast<void*>(g_ppClassVarTableSecondary));
	}
	else
	{
		Msg(eDLL_T::SERVER, "[CLASSVAR] resolved (find=%p write=%p apply=%p)\n",
			reinterpret_cast<void*>(v_ClassVar_Find),
			reinterpret_cast<void*>(v_ClassVar_Write),
			reinterpret_cast<void*>(v_ClassVar_Apply));
	}
}

//-----------------------------------------------------------------------------
// Purpose: the block the secondary table indexes into, mirroring the dispatch
//-----------------------------------------------------------------------------
static uintptr_t ServerScript_ClassVarSecondaryBase(const uintptr_t nPlayer,
	const uintptr_t nBlock)
{
	int nSelector;
	if (*reinterpret_cast<int32_t*>(nPlayer + CPLAYER_OFF_BLOCK_COUNT) > 0)
		nSelector = (*reinterpret_cast<uint8_t*>(nPlayer + CPLAYER_OFF_BLOCK_SELECTOR) >> 1) & 1;
	else
		nSelector = 2;

	const int32_t nBase = *g_pClassVarSecondaryBase;
	const uint32_t nStride =
		static_cast<uint32_t>(nSelector) * static_cast<uint32_t>(*g_pClassVarSecondaryStride);

	// A negative base is a flag: the low 31 bits are an offset to a dword in the
	// block that holds the real displacement.
	if (nBase < 0)
	{
		const uintptr_t nIndirect = static_cast<uintptr_t>(nBase & 0x7FFFFFFF);
		return nBlock + *reinterpret_cast<uint32_t*>(nBlock + nIndirect + 4) + nStride;
	}

	return nBlock + static_cast<uint32_t>(nBase) + nStride;
}

//-----------------------------------------------------------------------------
// Purpose: resolve a key to the base/type/offset triple either table can yield
//-----------------------------------------------------------------------------
static bool ServerScript_ClassVarResolveKey(const uintptr_t nPlayer, const char* pszKey,
	uintptr_t* pnBase, uint16_t* pnType, uint16_t* pnOffset)
{
	const uintptr_t nBlock =
		*reinterpret_cast<uintptr_t*>(nPlayer + CPLAYER_OFF_CLASSVAR_BLOCK);
	if (!nBlock)
		return false;

	uint64_t nNamePtr = 0;

	if (v_ClassVar_Find(*g_ppClassVarTablePrimary, pszKey, pnType, pnOffset, &nNamePtr))
		*pnBase = nBlock;
	else if (v_ClassVar_Find(*g_ppClassVarTableSecondary, pszKey, pnType, pnOffset, &nNamePtr))
		*pnBase = ServerScript_ClassVarSecondaryBase(nPlayer, nBlock);
	else
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: render a stored class-var the way the engine's own printer does.
//-----------------------------------------------------------------------------
static const char* ServerScript_ClassVarFormat(const uintptr_t nAddr,
	const uint16_t nType, char* pszBuf, const size_t nBufLen)
{
	const float* const pFlt = reinterpret_cast<const float*>(nAddr);

	switch (nType)
	{
	case 0:
		V_snprintf(pszBuf, nBufLen, "%s",
			*reinterpret_cast<const uint8_t*>(nAddr) ? "true" : "false");
		break;
	case 1:
		V_snprintf(pszBuf, nBufLen, "%i", *reinterpret_cast<const int32_t*>(nAddr));
		break;
	case 2:
		V_snprintf(pszBuf, nBufLen, "%g", pFlt[0]);
		break;
	case 3:
		V_snprintf(pszBuf, nBufLen, "%g %g", pFlt[0], pFlt[1]);
		break;
	case 4:
		V_snprintf(pszBuf, nBufLen, "%g %g %g", pFlt[0], pFlt[1], pFlt[2]);
		break;
	case 5:
	case 6:
	case 7:
		V_snprintf(pszBuf, nBufLen, "\"%s\"", *reinterpret_cast<const char* const*>(nAddr));
		break;
	default:
		V_snprintf(pszBuf, nBufLen, "<unhandled type %u>", nType);
		break;
	}

	return pszBuf;
}

//-----------------------------------------------------------------------------
// Purpose: Player_SetClassVar(entity player, string key, string value)
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_SetClassVar(HSQUIRRELVM v)
{
	ServerScript_ResolveClassVar();

	if (!s_bClassVarUsable)
	{
		Warning(eDLL_T::SERVER, "[CLASSVAR] refused: surface unresolved\n");
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const SQChar* pszKey = nullptr;
	const SQChar* pszValue = nullptr;
	if (SQ_FAILED(sq_getstring(v, 3, &pszKey)) || !pszKey || !*pszKey
		|| SQ_FAILED(sq_getstring(v, 4, &pszValue)) || !pszValue)
	{
		Warning(eDLL_T::SERVER, "[CLASSVAR] refused: bad key/value argument\n");
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* const pPlayer = ServerScript_EntityPtrFromStackIdx(v, 2);
	if (!pPlayer)
	{
		Warning(eDLL_T::SERVER,
			"[CLASSVAR] refused: no player entity for '%s'\n", pszKey);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const uintptr_t nPlayer = reinterpret_cast<uintptr_t>(pPlayer);

	uintptr_t nBase = 0;
	uint16_t nType = 0;
	uint16_t nOffset = 0;
	if (!ServerScript_ClassVarResolveKey(nPlayer, pszKey, &nBase, &nType, &nOffset))
	{
		Warning(eDLL_T::SERVER,
			"[CLASSVAR] refused: '%s' not in either table (or null block)\n", pszKey);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	v_ClassVar_Write(nBase, 0, 0, nType, nOffset, pszValue);
	v_ClassVar_Apply(pPlayer);

	if (bridge_classvar_log.GetBool())
	{
		char szReadback[256];
		Msg(eDLL_T::SERVER, "[CLASSVAR] '%s' = '%s' -> readback %s (type=%u off=%u)\n",
			pszKey, pszValue,
			ServerScript_ClassVarFormat(nBase + nOffset, nType, szReadback,
				sizeof(szReadback)),
			nType, nOffset);
	}

	sq_pushbool(v, SQTrue);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void Script_RegisterClassVarNatives(CSquirrelVM* s)
{
	Script_RegisterFuncNamed(s, "Player_SetClassVar",
		"Script_PlayerSetClassVar",
		"Sets a player class variable (movement tuning) on the server",
		"bool",
		"entity player, string key, string value",
		false,
		ServerScript_SetClassVar);
}
