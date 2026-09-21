//=============================================================================//
//
// Purpose: map-editor extra map paks, client half. Loads another BR map's
// pak noui so the editor can spawn its props; re-arms the stale stub
// modelprecache sweep once each pak lands.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier1/cvar.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "vscript/vsquirrel_s21.h"
#include "engine/client/net_bridge_internal.h"
#include "rtech/pak/rpak_observe.h"
#include "rtech/pak/pakstate.h"
#include "rtech/pak/paktools.h"
#include "rtech/pak/mapedit_paks_list.h"
#include "rtech/pak/mapedit_paks_cl.h"

static ConVar sdk_mapedit_extra_paks_max("sdk_mapedit_extra_paks_max", "3", FCVAR_RELEASE,
	"Cap on extra map-editor map paks resident at once (0..6).",
	true, (float)0, true, (float)6, "int");

static constexpr int kMapEditMaxExtraPaks = 6;
static constexpr int kMapEditPakNameLen = 64;
static constexpr int kMapEditPakLoadedStatus_S21 = 10;
static constexpr int kMapEditPakErrorStatus_S21 = 14;

struct MapEditExtraPak_s
{
	char m_szName[kMapEditPakNameLen];
	int m_nHandle;
	bool m_bRearmed;
	bool m_bUsed;
};
static MapEditExtraPak_s s_extraPaks[kMapEditMaxExtraPaks];

static bool MapEditPaks_IsBareMapName(const char* psz)
{
	if (!psz || !psz[0])
		return false;
	size_t n = 0;
	for (; psz[n]; ++n)
	{
		if (n >= 63)
			return false;
		const unsigned char c = static_cast<unsigned char>(psz[n]);
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return n >= 3;
}

static bool MapEditPaks_CopyNameArg(HSQUIRRELVM v, char* pszOut, const size_t nOutLen)
{
	const SQChar* pszArg = nullptr;
	if (SQ_FAILED(sq_getstring(v, 2, &pszArg)) || !pszArg || !*pszArg)
		return false;
	if (strlen(pszArg) >= nOutLen)
		return false;
	V_strncpy(pszOut, pszArg, nOutLen);
	return true;
}

static int MapEditPaks_FindAllowBit(const char* pszName)
{
	for (int i = 0; i < kMapEditPakAllowCount; ++i)
	{
		if (_stricmp(s_MapEditPakAllowList[i].name, pszName) == 0)
			return s_MapEditPakAllowList[i].bit;
	}
	return -1;
}

static int MapEditPaks_FindTracked(const char* pszName)
{
	for (int i = 0; i < kMapEditMaxExtraPaks; ++i)
	{
		if (s_extraPaks[i].m_bUsed && _stricmp(s_extraPaks[i].m_szName, pszName) == 0)
			return i;
	}
	return -1;
}

static int MapEditPaks_ActiveCount(void)
{
	int n = 0;
	for (int i = 0; i < kMapEditMaxExtraPaks; ++i)
	{
		if (s_extraPaks[i].m_bUsed)
			++n;
	}
	return n;
}

static SQRESULT ClientScript_MapEdit_RequestMapPak(HSQUIRRELVM v)
{
	char szName[kMapEditPakNameLen];
	if (!MapEditPaks_CopyNameArg(v, szName, sizeof(szName)))
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: bad mapName argument\n");
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (!Pak_IsAllowedLoadName_S21(szName) || !MapEditPaks_IsBareMapName(szName))
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: illegal name '%s'\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (_strnicmp(szName, "mp_rr_", 6) != 0)
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: '%s' is not an mp_rr_ map\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (MapEditPaks_FindAllowBit(szName) < 0)
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: '%s' not in the catalog allowlist\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const char* const pszLevel = Bridge_GetLevelBaseName();
	if (pszLevel && pszLevel[0] && _stricmp(pszLevel, szName) == 0)
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: '%s' is the current level\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	char szPakFile[kMapEditPakNameLen + 5];
	V_snprintf(szPakFile, sizeof(szPakFile), "%s.rpak", szName);
	if (!Pak_FileExists(szPakFile))
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: '%s' not on disk\n", szPakFile);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (MapEditPaks_FindTracked(szName) >= 0)
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: '%s' already tracked\n", szName);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	int nMax = sdk_mapedit_extra_paks_max.GetInt();
	if (nMax < 0)
		nMax = 0;
	if (nMax > kMapEditMaxExtraPaks)
		nMax = kMapEditMaxExtraPaks;
	if (MapEditPaks_ActiveCount() >= nMax)
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: '%s' over cap (%d/%d)\n",
			szName, MapEditPaks_ActiveCount(), nMax);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (!Pak_RequestLoadNoUi(szPakFile))
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: engine load failed for '%s'\n", szPakFile);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const int nHandle = Pak_FindHandleByName_S21(szPakFile);
	if (nHandle == -1)
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak rejected: no slot for '%s' after load\n", szPakFile);
		sq_pushbool(v, SQFalse);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	for (int i = 0; i < kMapEditMaxExtraPaks; ++i)
	{
		if (s_extraPaks[i].m_bUsed)
			continue;
		V_strncpy(s_extraPaks[i].m_szName, szName, sizeof(s_extraPaks[i].m_szName));
		s_extraPaks[i].m_nHandle = nHandle;
		s_extraPaks[i].m_bRearmed = false;
		s_extraPaks[i].m_bUsed = true;
		break;
	}
	Msg(eDLL_T::CLIENT, "[MAPEDIT-PAK] requested '%s' (handle 0x%X)\n", szPakFile, nHandle & 0xFFFFFF);
	sq_pushbool(v, SQTrue);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ClientScript_MapEdit_MapPakStatus(HSQUIRRELVM v)
{
	char szName[kMapEditPakNameLen];
	if (!MapEditPaks_CopyNameArg(v, szName, sizeof(szName)))
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_MapPakStatus: bad mapName argument\n");
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const int nIdx = MapEditPaks_FindTracked(szName);
	if (nIdx < 0)
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_MapPakStatus: '%s' not tracked\n", szName);
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	const int nStatus = Pak_GetStatusByHandle_S21(s_extraPaks[nIdx].m_nHandle);
	if (nStatus == kMapEditPakLoadedStatus_S21)
	{
		if (!s_extraPaks[nIdx].m_bRearmed)
		{
			s_extraPaks[nIdx].m_bRearmed = true;
			Bridge_ModelPrecacheFix_Rearm();
			Msg(eDLL_T::CLIENT, "[MAPEDIT-PAK] '%s' LOADED -- precache sweep re-armed\n", szName);
		}
		sq_pushinteger(v, 1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	if (nStatus == -1 || nStatus == kMapEditPakErrorStatus_S21)
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_MapPakStatus: '%s' failed (status=%d)\n", szName, nStatus);
		sq_pushinteger(v, -1);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}
	sq_pushinteger(v, 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ClientScript_MapEdit_ActivePakCount(HSQUIRRELVM v)
{
	sq_pushinteger(v, MapEditPaks_ActiveCount());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void MapEditPaks_RegisterClientFunctions(CSquirrelVM* s)
{
	if (!s)
	{
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEditPaks_RegisterClientFunctions: null CLIENT VM\n");
		return;
	}
	if (Script_RegisterFuncTC_S21(s, "MapEdit_RequestMapPak",
		reinterpret_cast<void*>(ClientScript_MapEdit_RequestMapPak),
		"bool", "string mapName") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_RequestMapPak registration FAILED\n");
	if (Script_RegisterFuncTC_S21(s, "MapEdit_MapPakStatus",
		reinterpret_cast<void*>(ClientScript_MapEdit_MapPakStatus),
		"int", "string mapName") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_MapPakStatus registration FAILED\n");
	if (Script_RegisterFuncTC_S21(s, "MapEdit_ActivePakCount",
		reinterpret_cast<void*>(ClientScript_MapEdit_ActivePakCount),
		"int", "") == SQ_ERROR)
		Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] MapEdit_ActivePakCount registration FAILED\n");
	Msg(eDLL_T::CLIENT, "[MAPEDIT-PAK] client natives registered\n");
}

void MapEditPaks_UnloadAll(const char* reason)
{
	if (MapEditPaks_ActiveCount() == 0)
		return;
	Msg(eDLL_T::CLIENT, "[MAPEDIT-PAK] unloading %d extra pak(s) (%s)\n",
		MapEditPaks_ActiveCount(), reason ? reason : "?");
	for (int i = 0; i < kMapEditMaxExtraPaks; ++i)
	{
		if (!s_extraPaks[i].m_bUsed)
			continue;
		const int nStatus = Pak_GetStatusByHandle_S21(s_extraPaks[i].m_nHandle);
		if (nStatus == kMapEditPakLoadedStatus_S21)
		{
			if (Pak_UnloadByHandle_S21(s_extraPaks[i].m_nHandle))
				Msg(eDLL_T::CLIENT, "[MAPEDIT-PAK] unloading '%s.rpak' (handle 0x%X)\n",
					s_extraPaks[i].m_szName, s_extraPaks[i].m_nHandle & 0xFFFFFF);
			else
				Warning(eDLL_T::CLIENT, "[MAPEDIT-PAK] unload failed for '%s.rpak' (handle 0x%X)\n",
					s_extraPaks[i].m_szName, s_extraPaks[i].m_nHandle & 0xFFFFFF);
		}
		else
			Msg(eDLL_T::CLIENT, "[MAPEDIT-PAK] dropping '%s.rpak' (status=%d, never resident)\n",
				s_extraPaks[i].m_szName, nStatus);
		s_extraPaks[i].m_szName[0] = '\0';
		s_extraPaks[i].m_nHandle = -1;
		s_extraPaks[i].m_bRearmed = false;
		s_extraPaks[i].m_bUsed = false;
	}
}
