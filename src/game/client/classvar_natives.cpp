//=============================================================================//
//
// Purpose: player class-var natives, client half. Movement is predicted, so
// both halves apply the same table.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/memaddr.h"
#include "tier0/memvalidate.h"
#include "tier1/cvar.h"
#include "tier1/cmd.h"
#include "mathlib/mathlib.h"
#include <cmath>
// SCRIPT_REGISTER_FUNC is a macro the vsquirrel.h register template expands, so
// this header has to come first.
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/vsquirrel_s21.h"
#include "vscript_client.h"
#include "classvar_natives.h"

//-----------------------------------------------------------------------------
// C_Player fields, read off the client setClassVar handler.
//-----------------------------------------------------------------------------
static constexpr ptrdiff_t CPLAYER_OFF_CLASSVAR_BLOCK = 0x2598; // class-var block ptr
static constexpr ptrdiff_t CPLAYER_OFF_BLOCK_COUNT    = 0x328;  // >0 selects by team bit
static constexpr ptrdiff_t CPLAYER_OFF_BLOCK_SELECTOR = 0xC8;   // bit 1 = selector

// Same three engine routines as the dedi half, at S21 addresses.
static bool (*v_ClassVar_Find)(void* pTable, const char* pszName,
	uint16_t* pType, uint16_t* pOffset, uint64_t* pNamePtr) = nullptr;

static void (*v_ClassVar_Write)(uintptr_t nBase, uintptr_t, uintptr_t,
	uintptr_t nType, uintptr_t nOffset, const char* pszValue) = nullptr;

static void (*v_ClassVar_Apply)(void* pPlayer) = nullptr;

static void** g_ppClassVarTablePrimary = nullptr;
static void** g_ppClassVarTableSecondary = nullptr;
static int32_t* g_pClassVarSecondaryBase = nullptr;
static int32_t* g_pClassVarSecondaryStride = nullptr;

// Local-player resolution, same two globals the engine handler reads.
static uint32_t* g_pLocalPlayerSlot = nullptr;
static uintptr_t g_nPlayerArray = 0;

static constexpr size_t PLAYER_ARRAY_STRIDE = 0x20; // shl rcx, 5
static constexpr ptrdiff_t PLAYER_ENTRY_SERIAL = 0x8;

static bool s_bClassVarResolved = false;
static bool s_bClassVarUsable = false;
static uint32_t s_nRebuildSerial = 0;

static ConVar bridge_classvar_log_cl("bridge_classvar_log_cl", "0", FCVAR_DEVELOPMENTONLY,
	"Log every SetLocalClassVar key/value applied on the client.");

//-----------------------------------------------------------------------------
// Purpose: resolve the class-var surface out of the S21 client
//-----------------------------------------------------------------------------
static void ClientScript_ResolveClassVar(void)
{
	if (s_bClassVarResolved)
		return;
	s_bClassVarResolved = true;

	// Byte-identical to the S3 lookup -- same source, same codegen.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 4C 89 44 24 ?? "
		"57 41 54 41 55 41 56 41 57 8B 71")
		.GetPtr(v_ClassVar_Find);

	Module_FindPattern(g_GameDll, "40 53 48 83 EC ?? 4C 8B 5C 24")
		.GetPtr(v_ClassVar_Write);

	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B 99 ?? ?? ?? ?? 48 8B F9 8B 05")
		.GetPtr(v_ClassVar_Apply);

	// Globals live only inside the local setClassVar handler; anchor and decode.
	const CMemory handler = Module_FindPattern(g_GameDll,
		"4C 8B DC 55 56 49 8D AB");

	if (handler.GetPtr())
	{
		// +0xC3  48 8B 15  mov rdx, cs:<primary table>
		g_ppClassVarTablePrimary = handler.Offset(0xC3)
			.ResolveRelativeAddress(3, 7).RCast<void**>();
		// +0xD2  48 8B 35  mov rsi, cs:<secondary table>
		g_ppClassVarTableSecondary = handler.Offset(0xD2)
			.ResolveRelativeAddress(3, 7).RCast<void**>();
		// +0x87  8B 05     mov eax, cs:<secondary base>
		g_pClassVarSecondaryBase = handler.Offset(0x87)
			.ResolveRelativeAddress(2, 6).RCast<int32_t*>();
		// +0x8D  0F AF 0D  imul ecx, cs:<secondary stride>
		g_pClassVarSecondaryStride = handler.Offset(0x8D)
			.ResolveRelativeAddress(3, 7).RCast<int32_t*>();
		// +0x13  8B 05     mov eax, cs:<local player slot>
		g_pLocalPlayerSlot = handler.Offset(0x13)
			.ResolveRelativeAddress(2, 6).RCast<uint32_t*>();
		// +0x28  48 8D 05  lea rax, <player array>  (the array itself, not a ptr)
		g_nPlayerArray = handler.Offset(0x28)
			.ResolveRelativeAddress(3, 7).GetPtr();
	}

	s_bClassVarUsable = v_ClassVar_Find && v_ClassVar_Write && v_ClassVar_Apply
		&& g_ppClassVarTablePrimary && g_ppClassVarTableSecondary
		&& g_pClassVarSecondaryBase && g_pClassVarSecondaryStride
		&& g_pLocalPlayerSlot && g_nPlayerArray;

	if (!s_bClassVarUsable)
	{
		Warning(eDLL_T::CLIENT,
			"[CLASSVAR] surface unresolved (find=%p write=%p apply=%p "
			"primary=%p secondary=%p) -- SetLocalClassVar is a no-op\n",
			reinterpret_cast<void*>(v_ClassVar_Find),
			reinterpret_cast<void*>(v_ClassVar_Write),
			reinterpret_cast<void*>(v_ClassVar_Apply),
			reinterpret_cast<void*>(g_ppClassVarTablePrimary),
			reinterpret_cast<void*>(g_ppClassVarTableSecondary));
	}
	else
	{
		Msg(eDLL_T::CLIENT, "[CLASSVAR] resolved (find=%p write=%p apply=%p)\n",
			reinterpret_cast<void*>(v_ClassVar_Find),
			reinterpret_cast<void*>(v_ClassVar_Write),
			reinterpret_cast<void*>(v_ClassVar_Apply));
	}
}

//-----------------------------------------------------------------------------
// Purpose: local player via the slot global (index low word, serial high word).
//-----------------------------------------------------------------------------
static void* ClientScript_LocalPlayer(void)
{
	if (!g_pLocalPlayerSlot || !g_nPlayerArray)
		return nullptr;

	const uint32_t nSlot = *g_pLocalPlayerSlot;
	if (nSlot == UINT32_MAX)
		return nullptr;

	const uintptr_t nEntry =
		g_nPlayerArray + (static_cast<uint16_t>(nSlot) * PLAYER_ARRAY_STRIDE);

	if (*reinterpret_cast<uint32_t*>(nEntry + PLAYER_ENTRY_SERIAL) != (nSlot >> 16))
		return nullptr;

	return *reinterpret_cast<void**>(nEntry);
}

void* ClassVar_LocalPlayer(void)
{
	return ClientScript_LocalPlayer();
}

//-----------------------------------------------------------------------------
// Purpose: the block the secondary table indexes into, mirroring the handler
//-----------------------------------------------------------------------------
static uintptr_t ClientScript_ClassVarSecondaryBase(const uintptr_t nPlayer,
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
static bool ClientScript_ClassVarResolveKey(const uintptr_t nPlayer, const char* pszKey,
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
		*pnBase = ClientScript_ClassVarSecondaryBase(nPlayer, nBlock);
	else
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: render a stored class-var the way the engine's own printer does.
//-----------------------------------------------------------------------------
const char* ClassVar_FormatValue(const uintptr_t nAddr,
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
// Purpose: read one class-var from the local player's live block.
//-----------------------------------------------------------------------------
static void ClientScript_ClassVarGet_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::CLIENT, "usage: bridge_classvar_get <key>\n");
		return;
	}

	ClientScript_ResolveClassVar();

	if (!s_bClassVarUsable)
	{
		Warning(eDLL_T::CLIENT, "[CLASSVAR] surface unresolved\n");
		return;
	}

	void* const pPlayer = ClientScript_LocalPlayer();
	if (!pPlayer)
	{
		Warning(eDLL_T::CLIENT, "[CLASSVAR] no local player\n");
		return;
	}

	const char* const pszKey = args.Arg(1);

	uintptr_t nBase = 0;
	uint16_t nType = 0;
	uint16_t nOffset = 0;
	if (!ClientScript_ClassVarResolveKey(reinterpret_cast<uintptr_t>(pPlayer),
		pszKey, &nBase, &nType, &nOffset))
	{
		Warning(eDLL_T::CLIENT, "[CLASSVAR] unknown key '%s'\n", pszKey);
		return;
	}

	char szValue[256];
	Msg(eDLL_T::CLIENT, "[CLASSVAR] %s = %s (type=%u off=%u)\n", pszKey,
		ClassVar_FormatValue(nBase + nOffset, nType, szValue, sizeof(szValue)),
		nType, nOffset);
}

static ConCommand bridge_classvar_get("bridge_classvar_get", ClientScript_ClassVarGet_f,
	"Print a class-var as it currently stands on the local player", FCVAR_CLIENTDLL);

//-----------------------------------------------------------------------------
// Replay the gamemode table after each engine class-var rebuild.
//-----------------------------------------------------------------------------
struct ClassVarSticky_s
{
	char key[64];
	char value[64];
};

static constexpr int CLASSVAR_STICKY_MAX = 32;

static ClassVarSticky_s s_stickyVars[CLASSVAR_STICKY_MAX];
static int s_nStickyCount = 0;
static void* s_pStickyOwner = nullptr;

//-----------------------------------------------------------------------------
// Purpose: write one class-var without the block-wide apply.
//-----------------------------------------------------------------------------
static bool ClientScript_ClassVarWrite(const uintptr_t nPlayer, const char* pszKey,
	const char* pszValue)
{
	uintptr_t nBase = 0;
	uint16_t nType = 0;
	uint16_t nOffset = 0;

	if (!ClientScript_ClassVarResolveKey(nPlayer, pszKey, &nBase, &nType, &nOffset))
		return false;

	v_ClassVar_Write(nBase, 0, 0, nType, nOffset, pszValue);
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: remember a key so it survives the next settings rebuild
//-----------------------------------------------------------------------------
static void ClientScript_ClassVarRemember(void* pPlayer, const char* pszKey,
	const char* pszValue)
{
	if (s_pStickyOwner != pPlayer)
	{
		s_pStickyOwner = pPlayer;
		s_nStickyCount = 0;
	}

	for (int i = 0; i < s_nStickyCount; i++)
	{
		if (V_stricmp(s_stickyVars[i].key, pszKey) == 0)
		{
			V_strncpy(s_stickyVars[i].value, pszValue, sizeof(s_stickyVars[i].value));
			return;
		}
	}

	if (s_nStickyCount >= CLASSVAR_STICKY_MAX)
	{
		Warning(eDLL_T::CLIENT,
			"[CLASSVAR] sticky table full (%d) -- '%s' will not survive a settings "
			"rebuild\n", CLASSVAR_STICKY_MAX, pszKey);
		return;
	}

	V_strncpy(s_stickyVars[s_nStickyCount].key, pszKey,
		sizeof(s_stickyVars[s_nStickyCount].key));
	V_strncpy(s_stickyVars[s_nStickyCount].value, pszValue,
		sizeof(s_stickyVars[s_nStickyCount].value));
	s_nStickyCount++;
}

//-----------------------------------------------------------------------------
// Purpose: engine rebuilt the block; put the gamemode values back before the next move.
//-----------------------------------------------------------------------------
static bool __fastcall Hook_C_Player_ApplySettingsChange(void* pPlayer)
{
	const bool bRebuilt = v_C_Player_ApplySettingsChange(pPlayer);

	if (bRebuilt)
		s_nRebuildSerial++;

	if (!bRebuilt || !s_nStickyCount || pPlayer != s_pStickyOwner)
		return bRebuilt;

	ClientScript_ResolveClassVar();

	if (!s_bClassVarUsable)
		return bRebuilt;

	const uintptr_t nPlayer = reinterpret_cast<uintptr_t>(pPlayer);
	int nApplied = 0;

	for (int i = 0; i < s_nStickyCount; i++)
	{
		if (ClientScript_ClassVarWrite(nPlayer, s_stickyVars[i].key,
			s_stickyVars[i].value))
			nApplied++;
	}

	if (nApplied)
		v_ClassVar_Apply(pPlayer);

	if (bridge_classvar_log_cl.GetBool())
		Msg(eDLL_T::CLIENT, "[CLASSVAR] settings rebuild -- replayed %d/%d\n",
			nApplied, s_nStickyCount);

	return bRebuilt;
}

void VClassVarNativesCl::GetFun(void) const
{
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 ?? 48 89 7C 24 ?? 55 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? "
		"4C 8B 81 E8 24 00 00 48 8B D9 48 8B 91 D0 24 00 00")
		.GetPtr(v_C_Player_ApplySettingsChange);
}

// Table: +0x08 hash slots { u16 type; u16 nameOff; u32 offset }, +0x10 u16 slot index (stride 4) x count @+0x1C, +0x38 name pool.
static constexpr ptrdiff_t CLASSVAR_TAB_ENTRIES = 0x08;
static constexpr ptrdiff_t CLASSVAR_TAB_INDEX   = 0x10;
static constexpr ptrdiff_t CLASSVAR_TAB_COUNT   = 0x1C;
static constexpr ptrdiff_t CLASSVAR_TAB_POOL    = 0x38;
static constexpr uint32_t CLASSVAR_TAB_MAX      = 4096;
static constexpr size_t CLASSVAR_NAME_MAX       = 64;

static int ClassVar_EnumTable(void* const pTable, const bool bSecondary,
	ClassVarField_t* const pOut, const int nMax)
{
	if (!pTable || !pOut || nMax <= 0)
		return 0;
	if (!Mem_IsReadable(pTable, 0x40))
		return 0;

	const uintptr_t nTable = reinterpret_cast<uintptr_t>(pTable);
	void* const pEntries = *reinterpret_cast<void**>(nTable + CLASSVAR_TAB_ENTRIES);
	void* const pIndex = *reinterpret_cast<void**>(nTable + CLASSVAR_TAB_INDEX);
	const uint32_t nCount = *reinterpret_cast<uint32_t*>(nTable + CLASSVAR_TAB_COUNT);
	const char* const pszPool = *reinterpret_cast<const char**>(nTable + CLASSVAR_TAB_POOL);

	if (!pEntries || !pIndex || !pszPool || nCount == 0 || nCount > CLASSVAR_TAB_MAX)
		return 0;
	if (!Mem_IsReadable(pIndex, sizeof(uint16_t)))
		return 0;

	int nOut = 0;
	const uintptr_t nEntries = reinterpret_cast<uintptr_t>(pEntries);

	const uintptr_t nIndex = reinterpret_cast<uintptr_t>(pIndex);

	for (uint32_t i = 0; i < nCount && nOut < nMax; i++)
	{
		const uint16_t* const pSlot = reinterpret_cast<const uint16_t*>(nIndex + static_cast<uintptr_t>(i) * 4);
		if (!Mem_IsReadable(pSlot, sizeof(uint16_t)))
			continue;

		const uintptr_t nRec = nEntries + static_cast<uintptr_t>(*pSlot) * 8;
		if (!Mem_IsReadable(reinterpret_cast<const void*>(nRec), 8))
			continue;

		const uint16_t nType = *reinterpret_cast<const uint16_t*>(nRec);
		const uint16_t nNameOff = *reinterpret_cast<const uint16_t*>(nRec + 2);
		const uint32_t nOff = *reinterpret_cast<const uint32_t*>(nRec + 4);

		if (nType > 7 || nNameOff == 0 || (nOff >> 24) != 0)
			continue;

		const char* const pszName = pszPool + nNameOff;
		if (!Mem_IsReadable(pszName, 1))
			continue;

		size_t nLen = 0;
		while (nLen < CLASSVAR_NAME_MAX && Mem_IsReadable(pszName + nLen, 1) && pszName[nLen])
			nLen++;
		if (nLen == 0 || nLen >= CLASSVAR_NAME_MAX)
			continue;

		pOut[nOut].pszName = pszName;
		pOut[nOut].nType = nType;
		pOut[nOut].nOffset = nOff & 0xFFFFFF;
		pOut[nOut].bSecondary = bSecondary;
		nOut++;
	}

	return nOut;
}

int ClassVar_EnumFields(ClassVarField_t* pOut, int nMax)
{
	ClientScript_ResolveClassVar();

	if (!pOut || nMax <= 0)
		return 0;
	if (!s_bClassVarUsable)
		return 0;
	if (!Mem_IsReadable(g_ppClassVarTablePrimary, sizeof(void*))
		|| !Mem_IsReadable(g_ppClassVarTableSecondary, sizeof(void*)))
		return 0;

	int nOut = ClassVar_EnumTable(*g_ppClassVarTablePrimary, false, pOut, nMax);
	if (nOut < nMax)
		nOut += ClassVar_EnumTable(*g_ppClassVarTableSecondary, true, pOut + nOut, nMax - nOut);

	return nOut;
}

uintptr_t ClassVar_LocalBlock(void)
{
	ClientScript_ResolveClassVar();

	if (!s_bClassVarUsable)
		return 0;

	void* const pPlayer = ClientScript_LocalPlayer();
	if (!pPlayer)
		return 0;

	return *reinterpret_cast<uintptr_t*>(
		reinterpret_cast<uintptr_t>(pPlayer) + CPLAYER_OFF_CLASSVAR_BLOCK);
}

uintptr_t ClassVar_FieldAddress(const ClassVarField_t& field)
{
	ClientScript_ResolveClassVar();

	if (!s_bClassVarUsable)
		return 0;

	void* const pPlayer = ClientScript_LocalPlayer();
	if (!pPlayer)
		return 0;

	const uintptr_t nPlayer = reinterpret_cast<uintptr_t>(pPlayer);
	const uintptr_t nBlock = *reinterpret_cast<uintptr_t*>(nPlayer + CPLAYER_OFF_CLASSVAR_BLOCK);
	if (!nBlock)
		return 0;

	if (field.bSecondary)
		return ClientScript_ClassVarSecondaryBase(nPlayer, nBlock) + (field.nOffset & 0xFFFFFF);
	return nBlock + (field.nOffset & 0xFFFFFF);
}

uint32_t ClassVar_SettingsRebuildSerial(void)
{
	ClientScript_ResolveClassVar();
	return s_nRebuildSerial;
}

static bool ClassVar_ValidateFloats(const char* pszValue, const int nWant,
	char* const pszReason, const size_t nReasonLen)
{
	static constexpr float CLASSVAR_MAX_ABS = 100000.f;

	const char* p = pszValue;

	for (int i = 0; i < nWant; i++)
	{
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
		{
			V_snprintf(pszReason, nReasonLen, "needs exactly %d floats", nWant);
			return false;
		}

		char* pszEnd = nullptr;
		const float flVal = strtof(p, &pszEnd);
		if (pszEnd == p)
		{
			V_snprintf(pszReason, nReasonLen, "not a finite float");
			return false;
		}
		if (!isfinite(flVal) || fabsf(flVal) > CLASSVAR_MAX_ABS)
		{
			V_snprintf(pszReason, nReasonLen, "component %d out of range", i);
			return false;
		}
		p = pszEnd;
	}

	while (*p == ' ' || *p == '\t')
		p++;

	if (*p)
	{
		V_snprintf(pszReason, nReasonLen, "needs exactly %d floats", nWant);
		return false;
	}

	return true;
}

static bool ClassVar_ValidateSet(const char* const pszKey, const char* const pszValue,
	char* const pszReason, const size_t nReasonLen)
{
	if (!pszKey || !pszKey[0] || !pszValue)
	{
		V_snprintf(pszReason, nReasonLen, "bad key/value argument");
		return false;
	}
	if (V_strlen(pszKey) > 63)
	{
		V_snprintf(pszReason, nReasonLen, "key longer than 63 chars");
		return false;
	}
	if (V_strlen(pszValue) > 127)
	{
		V_snprintf(pszReason, nReasonLen, "value longer than 127 chars");
		return false;
	}

	uint16_t nType = 0;
	uint16_t nOffset = 0;
	uint64_t nNamePtr = 0;

	if (!v_ClassVar_Find(*g_ppClassVarTablePrimary, pszKey, &nType, &nOffset, &nNamePtr)
		&& !v_ClassVar_Find(*g_ppClassVarTableSecondary, pszKey, &nType, &nOffset, &nNamePtr))
	{
		V_snprintf(pszReason, nReasonLen, "unknown key");
		return false;
	}
	if (nType >= 5)
	{
		V_snprintf(pszReason, nReasonLen, "type %u is not settable from the console", nType);
		return false;
	}
	if (nType == 0)
	{
		if (V_stricmp(pszValue, "0") != 0 && V_stricmp(pszValue, "1") != 0
			&& V_stricmp(pszValue, "true") != 0 && V_stricmp(pszValue, "false") != 0)
		{
			V_snprintf(pszReason, nReasonLen, "bool needs 0|1|true|false");
			return false;
		}
		return true;
	}
	if (nType == 1)
	{
		char* pszEnd = nullptr;
		(void)strtol(pszValue, &pszEnd, 10);
		if (!pszEnd || pszEnd == pszValue || *pszEnd != '\0')
		{
			V_snprintf(pszReason, nReasonLen, "not an int");
			return false;
		}
		return true;
	}

	return ClassVar_ValidateFloats(pszValue,
		nType == 2 ? 1 : (nType == 3 ? 2 : 3), pszReason, nReasonLen);
}

static FnCommandCallback_t s_fnSetOrig = nullptr;

static void ClassVar_Set_f(const CCommand& args)
{
	if (!s_fnSetOrig)
		return;

	if (args.ArgC() < 3)
	{
		s_fnSetOrig(args);
		return;
	}

	ClientScript_ResolveClassVar();

	if (!s_bClassVarUsable)
	{
		s_fnSetOrig(args);
		return;
	}

	const char* const pszKey = args.Arg(1);
	const char* const pszValue = args.Arg(2);

	char szReason[128] = { 0 };
	if (!ClassVar_ValidateSet(pszKey, pszValue, szReason, sizeof(szReason)))
	{
		Warning(eDLL_T::CLIENT, "[CLASSVAR] refused '%s': %s\n",
			pszKey ? pszKey : "", szReason);
		return;
	}

	s_fnSetOrig(args);
}

//-----------------------------------------------------------------------------
// Purpose: _setClassVarClient <key> <value> -- server-driven local write, no forward
//-----------------------------------------------------------------------------
static void ClassVar_SetClient_f(const CCommand& args)
{
	if (args.ArgC() < 3)
		return;

	ClientScript_ResolveClassVar();
	if (!s_bClassVarUsable)
		return;

	const char* const pszKey = args.Arg(1);
	const char* const pszValue = args.Arg(2);

	char szReason[128] = { 0 };
	if (!ClassVar_ValidateSet(pszKey, pszValue, szReason, sizeof(szReason)))
	{
		Warning(eDLL_T::CLIENT, "[CLASSVAR] server set refused '%s': %s\n", pszKey ? pszKey : "", szReason);
		return;
	}

	void* const pPlayer = ClientScript_LocalPlayer();
	if (!pPlayer)
		return;

	if (!ClientScript_ClassVarWrite(reinterpret_cast<uintptr_t>(pPlayer), pszKey, pszValue))
		return;

	v_ClassVar_Apply(pPlayer);
	Msg(eDLL_T::CLIENT, "[CLASSVAR] server set %s = %s\n", pszKey, pszValue);
}

void ClassVar_BindShipped(void)
{
	static bool s_bSetBound = false;

	if (s_bSetBound || !g_pCVar)
		return;

	// The engine registers 'set' after the SDK's cvar connect; callers retry until it exists.
	ConCommand* const pCmd = g_pCVar->FindCommand("set");
	if (!pCmd)
		return;

	pCmd->RemoveFlags(FCVAR_DEVELOPMENTONLY);
	s_fnSetOrig = pCmd->m_fnCommandCallback;
	pCmd->m_fnCommandCallback = ClassVar_Set_f;
	s_bSetBound = true;

	// The SDK's own _setClassVarClient still points at the S3 handler, which never resolves here.
	if (ConCommand* const pClientCmd = g_pCVar->FindCommand("_setClassVarClient"))
	{
		pClientCmd->RemoveFlags(FCVAR_DEVELOPMENTONLY);
		pClientCmd->m_fnCommandCallback = ClassVar_SetClient_f;
	}

	Msg(eDLL_T::CLIENT, "[CLASSVAR] set: cheat-gated, wrapped\n");
}

void VClassVarNativesCl::Detour(const bool bAttach) const
{
	DetourSetup(&v_C_Player_ApplySettingsChange,
		&Hook_C_Player_ApplySettingsChange, bAttach);
}

//-----------------------------------------------------------------------------
// Purpose: SetLocalClassVar(string key, string value) -- always the local player
//-----------------------------------------------------------------------------
static SQRESULT ClientScript_SetLocalClassVar(HSQUIRRELVM v)
{
	ClientScript_ResolveClassVar();

	if (!s_bClassVarUsable)
	{
		Warning(eDLL_T::CLIENT, "[CLASSVAR] refused: surface unresolved\n");
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const SQChar* pszKey = nullptr;
	const SQChar* pszValue = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &pszKey)) || !pszKey || !*pszKey
		|| SQ_FAILED(sq_getstring(v, 3, &pszValue)) || !pszValue)
	{
		Warning(eDLL_T::CLIENT, "[CLASSVAR] refused: bad key/value argument\n");
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	void* const pPlayer = ClientScript_LocalPlayer();
	if (!pPlayer)
	{
		Warning(eDLL_T::CLIENT,
			"[CLASSVAR] refused: no local player for '%s'\n", pszKey);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	const uintptr_t nPlayer = reinterpret_cast<uintptr_t>(pPlayer);

	uintptr_t nBase = 0;
	uint16_t nType = 0;
	uint16_t nOffset = 0;
	if (!ClientScript_ClassVarResolveKey(nPlayer, pszKey, &nBase, &nType, &nOffset))
	{
		Warning(eDLL_T::CLIENT,
			"[CLASSVAR] refused: '%s' not in either table (or null block)\n", pszKey);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	v_ClassVar_Write(nBase, 0, 0, nType, nOffset, pszValue);
	v_ClassVar_Apply(pPlayer);
	ClientScript_ClassVarRemember(pPlayer, pszKey, pszValue);

	if (bridge_classvar_log_cl.GetBool())
	{
		char szReadback[256];
		Msg(eDLL_T::CLIENT, "[CLASSVAR] '%s' = '%s' -> readback %s (type=%u off=%u)\n",
			pszKey, pszValue,
			ClassVar_FormatValue(nBase + nOffset, nType, szReadback,
				sizeof(szReadback)),
			nType, nOffset);
	}

	sq_pushbool(v, SQTrue);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// S21 never fires the bulk Script_RegisterClientFunctions, so this has to
// late-register or the first script naming it dies at compile time.
void ClassVar_RegisterClientFunctions(CSquirrelVM* s)
{
	if (!s)
	{
		Warning(eDLL_T::CLIENT, "[S21-REG] ClassVar_RegisterClientFunctions: null CLIENT VM\n");
		return;
	}

	const SQRESULT r = Script_RegisterFuncTC_S21(s, "SetLocalClassVar",
		reinterpret_cast<void*>(ClientScript_SetLocalClassVar),
		"bool", "string key, string value");

	if (r == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[S21-REG] SetLocalClassVar registration FAILED\n");
}
