//=============================================================================//
//
// Purpose: DEDI extra map-pack natives for the map editor. Loads another
// map's server pak so its props can be precached and spawned mid-level.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "public/edict.h"
// SCRIPT_REGISTER_FUNC is a macro the vsquirrel.h register template expands, so
// this header has to come first.
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/vscript.h"
#include "vscript/ivscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/include/squirrel.h"
#include "vscript/languages/squirrel_re/include/sqstring.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "rtech/pak/pakstate.h"
#include "rtech/pak/paktools.h"
#include "engine/server/snapshot_diag.h"
#include "vscript_server.h"
#include "mapedit_paks.h"

extern CGlobalVars* gpGlobals;

static ConVar sdk_mapedit_extra_paks_max("sdk_mapedit_extra_paks_max", "3", FCVAR_RELEASE,
	"Extra map paks the map editor may hold loaded alongside the running level.", true, 0.f, true, 6.f);

static ConVar sdk_mapedit_pak_time_gate("sdk_mapedit_pak_time_gate", "1", FCVAR_RELEASE,
	"Refuse an extra map pak whose header time is newer than the running level's pak (a newer duplicate guid replaces the live asset).");

static constexpr int MAPEDIT_PAKS_MAX = 6;
static constexpr int MAPEDIT_PAK_NAME_LEN = 64;

struct MapEditPakEntry_s
{
	char m_szName[MAPEDIT_PAK_NAME_LEN];
	PakHandle_t m_nHandle;
	bool m_bUsed;
	bool m_bAnnounced;
};

static MapEditPakEntry_s s_extraPaks[MAPEDIT_PAKS_MAX];

static bool MapEditPaks_IsBareName(const char* pszName)
{
	if (!pszName || !pszName[0])
		return false;
	size_t nLen = 0;
	for (; pszName[nLen]; ++nLen)
	{
		if (nLen >= MAPEDIT_PAK_NAME_LEN - 1)
			return false;
		const unsigned char c = static_cast<unsigned char>(pszName[nLen]);
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return nLen >= 3;
}

static bool MapEditPaks_CopyPakArg(HSQUIRRELVM v, char* pszOut)
{
	const SQChar* pszArg = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &pszArg)) || !pszArg || !*pszArg)
		return false;
	size_t nLen = 0;
	for (; pszArg[nLen]; ++nLen)
	{
		if (nLen >= MAPEDIT_PAK_NAME_LEN - 1)
			return false;
		pszOut[nLen] = static_cast<char>(pszArg[nLen]);
	}
	pszOut[nLen] = '\0';
	return nLen >= 1;
}

static MapEditPakEntry_s* MapEditPaks_Find(const char* pszName)
{
	for (int i = 0; i < MAPEDIT_PAKS_MAX; ++i)
	{
		if (s_extraPaks[i].m_bUsed && strcmp(s_extraPaks[i].m_szName, pszName) == 0)
			return &s_extraPaks[i];
	}
	return nullptr;
}

static int MapEditPaks_TrackedCount(void)
{
	int nCount = 0;
	for (int i = 0; i < MAPEDIT_PAKS_MAX; ++i)
	{
		if (s_extraPaks[i].m_bUsed)
			++nCount;
	}
	return nCount;
}

// RPak header: magic(4) version(2) flags(2) createdTime(8).
static bool MapEditPaks_ReadHeaderTime(const char* pszPakFile, uint64_t& nTime)
{
	char szFull[MAX_OSPATH];
	snprintf(szFull, sizeof(szFull), "%s%s", Pak_GetReadPath(), pszPakFile);
	FILE* pFile = fopen(szFull, "rb");
	if (!pFile)
		return false;
	uint8_t hdr[16];
	const bool bOk = fread(hdr, 1, sizeof(hdr), pFile) == sizeof(hdr);
	fclose(pFile);
	if (!bOk)
		return false;
	memcpy(&nTime, hdr + 8, sizeof(nTime));
	return true;
}

static const char* MapEditPaks_CurrentLevel(void)
{
	if (!gpGlobals)
		return "";
	return gpGlobals->mapName.ToCStr();
}

//-----------------------------------------------------------------------------
// Purpose: MapEdit_RequestMapPak(string mapName) : bool
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MapEdit_RequestMapPak(HSQUIRRELVM v)
{
	char szName[MAPEDIT_PAK_NAME_LEN];
	if (!MapEditPaks_CopyPakArg(v, szName))
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: bad string argument\n");
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (!MapEditPaks_IsBareName(szName))
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' is not a bare map name\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (_strnicmp(szName, "mp_rr_", 6) != 0)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' is not an mp_rr_ map\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const char* pszLevel = MapEditPaks_CurrentLevel();
	if (pszLevel[0] && _stricmp(szName, pszLevel) == 0)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' is the running level\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	char szFile[MAPEDIT_PAK_NAME_LEN + 8];
	V_snprintf(szFile, sizeof(szFile), "%s.rpak", szName);
	if (!Pak_FileExists(szFile))
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' has no server pak\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (sdk_mapedit_pak_time_gate.GetBool() && pszLevel[0])
	{
		char szLevelFile[MAPEDIT_PAK_NAME_LEN + 8];
		V_snprintf(szLevelFile, sizeof(szLevelFile), "%s.rpak", pszLevel);
		uint64_t nExtraTime = 0, nLevelTime = 0;
		if (!MapEditPaks_ReadHeaderTime(szFile, nExtraTime) || !MapEditPaks_ReadHeaderTime(szLevelFile, nLevelTime))
		{
			Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: cannot read pak header time for '%s' / '%s'\n", szName, pszLevel);
			sq_pushbool(v, SQFalse);
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
		if (nExtraTime > nLevelTime)
		{
			Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' header time %llu is newer than '%s' %llu; its duplicate guids would replace live assets (sdk_mapedit_pak_time_gate 0 to override)\n",
				szName, static_cast<unsigned long long>(nExtraTime), pszLevel, static_cast<unsigned long long>(nLevelTime));
			sq_pushbool(v, SQFalse);
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
	}
	if (MapEditPaks_Find(szName))
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' already tracked\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const int nMax = sdk_mapedit_extra_paks_max.GetInt();
	if (MapEditPaks_TrackedCount() >= nMax)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' at cap %d/%d\n",
			szName, MapEditPaks_TrackedCount(), nMax);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	MapEditPakEntry_s* pSlot = nullptr;
	for (int i = 0; i < MAPEDIT_PAKS_MAX; ++i)
	{
		if (!s_extraPaks[i].m_bUsed)
		{
			pSlot = &s_extraPaks[i];
			break;
		}
	}
	if (!pSlot)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' table full\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (!g_pakLoadApi)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' pak system unresolved\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const PakHandle_t nHandle = g_pakLoadApi->LoadAsync(szFile, AlignedMemAlloc(), 1, 0);
	if (nHandle == PAK_INVALID_HANDLE)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak refused: '%s' LoadAsync failed\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	V_snprintf(pSlot->m_szName, sizeof(pSlot->m_szName), "%s", szName);
	pSlot->m_nHandle = nHandle;
	pSlot->m_bUsed = true;
	pSlot->m_bAnnounced = false;
	Msg(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_RequestMapPak loading '%s' with handle %d\n", szName, nHandle);
	sq_pushbool(v, SQTrue);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: MapEdit_MapPakStatus(string mapName) : int
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MapEdit_MapPakStatus(HSQUIRRELVM v)
{
	char szName[MAPEDIT_PAK_NAME_LEN];
	if (!MapEditPaks_CopyPakArg(v, szName))
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_MapPakStatus refused: bad string argument\n");
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const MapEditPakEntry_s* pEntry = MapEditPaks_Find(szName);
	if (!pEntry)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_MapPakStatus refused: '%s' not tracked\n", szName);
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (!g_pakLoadApi)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_MapPakStatus refused: '%s' pak system unresolved\n", szName);
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const PakLoadedInfo_s* pInfo = Pak_GetPakInfo(pEntry->m_nHandle);
	if (!pInfo)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_MapPakStatus failed: '%s' has no pak info\n", szName);
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (pInfo->status == PAK_STATUS_LOADED)
	{
		if (!pEntry->m_bAnnounced)
		{
			Msg(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_MapPakStatus loaded: '%s' with handle %d\n",
				szName, pEntry->m_nHandle);
			const_cast<MapEditPakEntry_s*>(pEntry)->m_bAnnounced = true;
		}
		sq_pushinteger(v, 1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (pInfo->status == PAK_STATUS_ERROR || pInfo->status == PAK_STATUS_INVALID_PAKHANDLE)
	{
		if (!pEntry->m_bAnnounced)
		{
			Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_MapPakStatus failed: '%s' status %s\n",
				szName, Pak_StatusToString(pInfo->status));
			const_cast<MapEditPakEntry_s*>(pEntry)->m_bAnnounced = true;
		}
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	sq_pushinteger(v, 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: MapEdit_PrecacheModel(asset model) : bool -- engine precache after
// level load; the script PrecacheModel wrapper refuses new models post-init.
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MapEdit_PrecacheModel(HSQUIRRELVM v)
{
	const SQChar* pszModel = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &pszModel)))
	{
		SQObject obj;
		if (SQ_SUCCEEDED(sq_getstackobj(v, 2, &obj)) && (sq_isstring(obj) || obj._type == OT_ASSET))
			pszModel = _stringval(obj);
	}
	if (!pszModel || !*pszModel || strlen(pszModel) >= 200)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_PrecacheModel refused: bad model argument\n");
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	char szModel[256];
	const size_t nLen = strlen(pszModel);
	const bool bHasExt = nLen >= 5 && strcmp(pszModel + nLen - 5, ".rmdl") == 0;
	V_snprintf(szModel, sizeof(szModel), "%s%s", pszModel, bHasExt ? "" : ".rmdl");
	if (!v_Server_PrecacheModelLate)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_PrecacheModel refused: late precache helper unresolved\n");
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	v_Server_PrecacheModelLate(szModel);
	const int64_t nIdx = Server_PrecacheModel_Invoke(szModel);
	if (nIdx == 0xFFFFFFFFLL || nIdx < 0)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] MapEdit_PrecacheModel failed: '%s'\n", szModel);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	sq_pushbool(v, SQTrue);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: MapEdit_ActivePakCount() : int
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MapEdit_ActivePakCount(HSQUIRRELVM v)
{
	sq_pushinteger(v, MapEditPaks_TrackedCount());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void MapEditPaks_RegisterServerFunctions(CSquirrelVM* pVM)
{
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, MapEdit_RequestMapPak, "Load another map's server pak for the map editor. Returns false when rejected.", "bool", "string mapName", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, MapEdit_MapPakStatus, "Extra map pak state: -1 unknown/failed, 0 pending, 1 loaded.", "int", "string mapName", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, MapEdit_PrecacheModel, "Precache a model after level load (extra map packs).", "bool", "asset model", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, MapEdit_ActivePakCount, "Number of extra map paks tracked this level.", "int", "", false);
}

static void MapEdit_Pack_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::SERVER, "usage: mapedit_pack <mp_rr_map>\n");
		return;
	}
	if (!g_pServerScript)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] mapedit_pack: no server VM\n");
		return;
	}
	const HSCRIPT hFunc = g_pServerScript->FindFunction("MapEdit_PackFromConsole", nullptr, nullptr);
	if (!hFunc)
	{
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] mapedit_pack: script MapEdit_PackFromConsole not found\n");
		return;
	}
	ScriptVariant_t arg(args.Arg(1));
	if (g_pServerScript->ExecuteFunction(hFunc, &arg, 1, nullptr, nullptr) == SCRIPT_ERROR)
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] mapedit_pack: script error\n");
}

static ConCommand mapedit_pack("mapedit_pack", MapEdit_Pack_f,
	"Load another BR map's props into the map editor on the server and every client. Usage: mapedit_pack <mp_rr_map>", FCVAR_RELEASE);

void VMapEditPaks::GetFun(void) const
{
	// Body of the engine's late-precache wrapper: save the precache-allowed
	// byte, clear the table lock bit, PrecacheModel, restore. Hit lands two
	// bytes past the `push rsi` prologue.
	Module_FindPattern(g_GameDll,
		"48 83 EC 20 48 8B 35 ?? ?? ?? ?? 48 85 F6 74 ?? 48 89 5C 24 ?? "
		"0F B6 5E 28 0F B6 C3")
		.Offset(-2)
		.GetPtr(v_Server_PrecacheModelLate);

	if (!v_Server_PrecacheModelLate)
		Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] Server_PrecacheModelLate pattern unresolved -- mapedit_pack precache disabled\n");
}

void MapEditPaks_LevelShutdown(void)
{
	int nCount = 0;
	for (int i = 0; i < MAPEDIT_PAKS_MAX; ++i)
	{
		MapEditPakEntry_s* pEntry = &s_extraPaks[i];
		if (!pEntry->m_bUsed)
			continue;
		if (g_pakLoadApi)
		{
			Msg(eDLL_T::SERVER, "[MAPEDIT-PAK] Requested pak unload for '%s' with handle %d\n",
				pEntry->m_szName, pEntry->m_nHandle);
			g_pakLoadApi->UnloadAsync(pEntry->m_nHandle);
		}
		else
		{
			Warning(eDLL_T::SERVER, "[MAPEDIT-PAK] Level reset dropped '%s' with handle %d: pak system unresolved\n",
				pEntry->m_szName, pEntry->m_nHandle);
		}
		pEntry->m_bUsed = false;
		pEntry->m_bAnnounced = false;
		pEntry->m_szName[0] = '\0';
		pEntry->m_nHandle = PAK_INVALID_HANDLE;
		++nCount;
	}
	Msg(eDLL_T::SERVER, "[MAPEDIT-PAK] Level reset unloaded %d extra map pak(s)\n", nCount);
}
