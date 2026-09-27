//=============================================================================//
//
// Purpose: demo replay script natives for the CLIENT and UI VMs. Every
//          argument is validated here; a bad one warns and returns, it never
//          raises into the VM.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "vscript/vsquirrel_s21.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "engine/shared/demo_reader.h"
#include "engine/shared/demo_json.h"
#include "engine/client/demo_bridge.h"
#include "engine/client/demo_record.h"
#include "engine/client/demo_play.h"
#include "demo_natives.h"
#include <unordered_map>

static constexpr size_t kDemoListMax = 256;

static const char* DemoNative_GetString(HSQUIRRELVM v, const SQInteger idx)
{
	const SQChar* psz = nullptr;
	if (SQ_FAILED(sq_getstring(v, idx, &psz)) || !psz)
		return nullptr;
	return psz;
}

static void DemoNative_PushStringArray(HSQUIRRELVM v, const std::vector<std::string>& items)
{
	sq_newarray(v, 0);
	for (const std::string& s : items)
	{
		sq_pushstring(v, s.c_str(), static_cast<SQInteger>(s.size()));
		sq_arrayappend(v, -2);
	}
}

//-----------------------------------------------------------------------------
// Shared (CLIENT + UI)
//-----------------------------------------------------------------------------
static SQRESULT Script_Demo_IsPlaying(HSQUIRRELVM v)
{
	sq_pushbool(v, Demo_IsPlaying());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_IsRecording(HSQUIRRELVM v)
{
	sq_pushbool(v, DemoRecord_IsRecording());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetTime(HSQUIRRELVM v)
{
	sq_pushfloat(v, DemoPlay_GetTime());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetDuration(HSQUIRRELVM v)
{
	sq_pushfloat(v, DemoPlay_GetDuration());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetPovCount(HSQUIRRELVM v)
{
	sq_pushinteger(v, DemoPlay_GetPovCount());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetPov(HSQUIRRELVM v)
{
	sq_pushinteger(v, DemoPlay_GetPov());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetPovName(HSQUIRRELVM v)
{
	SQInteger nPov = 0;
	sq_getinteger(v, 2, &nPov);
	const char* const psz = (nPov >= 0 && nPov < R5DEM_MAX_POVS) ? DemoPlay_GetPovName(static_cast<int>(nPov)) : "";
	sq_pushstring(v, psz, static_cast<SQInteger>(strlen(psz)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetMetaJson(HSQUIRRELVM v)
{
	const std::string meta = DemoPlay_GetMetaJson();
	sq_pushstring(v, meta.c_str(), static_cast<SQInteger>(meta.size()));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetEventsJson(HSQUIRRELVM v)
{
	std::vector<std::string> json, records;
	DemoPlay_GetEvents(json, records);
	DemoNative_PushStringArray(v, json);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetEventRecords(HSQUIRRELVM v)
{
	std::vector<std::string> json, records;
	DemoPlay_GetEvents(json, records);
	DemoNative_PushStringArray(v, records);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetInputsAt(HSQUIRRELVM v)
{
	SQInteger nPov = 0;
	SQFloat flTime = 0.0f;
	sq_getinteger(v, 2, &nPov);
	sq_getfloat(v, 3, &flTime);
	char sz[128] = {};
	if (nPov < 0 || nPov >= R5DEM_MAX_POVS || !DemoPlay_GetInputsAt(static_cast<int>(nPov), flTime, sz, sizeof(sz)))
		sz[0] = '\0';
	sq_pushstring(v, sz, static_cast<SQInteger>(strlen(sz)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_IsPaused(HSQUIRRELVM v)
{
	sq_pushbool(v, DemoPlay_IsPaused());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_IsSeeking(HSQUIRRELVM v)
{
	sq_pushbool(v, DemoPlay_IsSeeking());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetTimescale(HSQUIRRELVM v)
{
	sq_pushfloat(v, DemoPlay_GetTimescale());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetView(HSQUIRRELVM v)
{
	sq_pushinteger(v, DemoPlay_GetView());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_IsFreecam(HSQUIRRELVM v)
{
	sq_pushbool(v, DemoPlay_IsFreecam());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_Stop(HSQUIRRELVM v)
{
	if (!DemoPlay_Stop("script") && DemoRecord_IsRecording())
		DemoRecord_Stop("script");
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_Pause(HSQUIRRELVM v)
{
	SQBool b = SQFalse;
	sq_getbool(v, 2, &b);
	DemoPlay_SetPaused(b != SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_Step(HSQUIRRELVM v)
{
	DemoPlay_Step();
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_SetTimescale(HSQUIRRELVM v)
{
	SQFloat fl = 1.0f;
	sq_getfloat(v, 2, &fl);
	if (fl == fl && fl >= 0.05f && fl <= 4.0f)
		DemoPlay_SetTimescale(fl);
	else
		Warning(eDLL_T::CLIENT, "[DEMO] Demo_SetTimescale %.3f outside 0.05..4 -- ignored\n", fl);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_Seek(HSQUIRRELVM v)
{
	SQFloat fl = 0.0f;
	sq_getfloat(v, 2, &fl);
	if (fl == fl)
		DemoPlay_Seek(fl);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_SetPov(HSQUIRRELVM v)
{
	SQInteger n = 0;
	sq_getinteger(v, 2, &n);
	if (n >= 0 && n < R5DEM_MAX_POVS)
		DemoPlay_SetPov(static_cast<int>(n));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_SetView(HSQUIRRELVM v)
{
	SQInteger n = -1;
	sq_getinteger(v, 2, &n);
	if (n >= -1 && n < R5DEM_MAX_POVS)
		DemoPlay_SetView(static_cast<int>(n), false);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_SetFreecam(HSQUIRRELVM v)
{
	SQBool b = SQFalse;
	sq_getbool(v, 2, &b);
	DemoPlay_SetFreecam(b != SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_SetCamera(HSQUIRRELVM v)
{
	SQInteger nMode = 0;
	sq_getinteger(v, 2, &nMode);
	sq_pushbool(v, DemoPlay_SetCamera(static_cast<int>(nMode)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetCamera(HSQUIRRELVM v)
{
	sq_pushinteger(v, DemoPlay_GetCamera());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_AddBookmark(HSQUIRRELVM v)
{
	sq_pushbool(v, DemoPlay_AddBookmark());
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_SaveClip(HSQUIRRELVM v)
{
	SQFloat flStart = 0.0f, flEnd = 0.0f;
	sq_getfloat(v, 2, &flStart);
	sq_getfloat(v, 3, &flEnd);
	char szName[72] = {};
	if (!DemoPlay_SaveClip(flStart, flEnd, szName, sizeof(szName)))
		szName[0] = '\0';
	sq_pushstring(v, szName, static_cast<SQInteger>(strlen(szName)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_RecordEvent(HSQUIRRELVM v)
{
	const char* const pszType = DemoNative_GetString(v, 2);
	const char* const pszAttacker = DemoNative_GetString(v, 3);
	const char* const pszVictim = DemoNative_GetString(v, 4);
	const char* const pszWeapon = DemoNative_GetString(v, 5);
	SQFloat flDamage = 0.0f;
	sq_getfloat(v, 6, &flDamage);
	sq_pushbool(v, DemoRecord_AddEvent(pszType, pszAttacker, pszVictim, pszWeapon, flDamage));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_SetHud(HSQUIRRELVM v)
{
	SQInteger n = 1;
	sq_getinteger(v, 2, &n);
	ConVar* const pHud = g_pCVar ? g_pCVar->FindVar("demo_hud") : nullptr;
	if (pHud && n >= 0 && n <= 2)
		pHud->SetValue(static_cast<int>(n));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_GetHud(HSQUIRRELVM v)
{
	ConVar* const pHud = g_pCVar ? g_pCVar->FindVar("demo_hud") : nullptr;
	sq_pushinteger(v, pHud ? pHud->GetInt() : 1);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// CLIENT only
//-----------------------------------------------------------------------------
static SQRESULT Script_Demo_SetMeta(HSQUIRRELVM v)
{
	const char* const psz = DemoNative_GetString(v, 2);
	if (psz)
		DemoRecord_SetMeta(psz);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_OverlayStarted(HSQUIRRELVM v)
{
	DemoPlay_OverlayStarted();
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_SetMouseLayer(HSQUIRRELVM v)
{
	SQBool b = SQFalse;
	sq_getbool(v, 2, &b);
	DemoPlay_SetMouseLayer(b != SQFalse);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// UI only: the replay list
//-----------------------------------------------------------------------------
// Building a summary reads the whole replay, so summaries persist keyed by size
// and write time and only new or changed files are scanned.
struct DemoSummaryEntry_s
{
	uint64_t    size;
	uint64_t    mtime;
	std::string summary;
};

static constexpr const char* kSummaryCachePath = "platform\\demos\\summaries.cache";
static constexpr size_t      kSummaryMaxLen    = 511;

static std::unordered_map<std::string, DemoSummaryEntry_s> s_summaryCache;
static std::unordered_map<std::string, DemoFileInfo_s>     s_listedDemos;
static bool                                                s_bSummaryCacheLoaded = false;

// One line per replay: name \t size \t mtime \t summary.
static void DemoSummaryCache_Load(void)
{
	if (s_bSummaryCacheLoaded)
		return;
	s_bSummaryCacheLoaded = true;

	FILE* pFile = nullptr;
	if (fopen_s(&pFile, kSummaryCachePath, "rb") != 0 || !pFile)
		return;

	char szLine[1024];
	while (fgets(szLine, sizeof(szLine), pFile) && s_summaryCache.size() < kDemoListMax * 4)
	{
		char* const pName = szLine;
		char* const pSize = strchr(pName, '\t');
		char* const pTime = pSize ? strchr(pSize + 1, '\t') : nullptr;
		char* const pSum = pTime ? strchr(pTime + 1, '\t') : nullptr;
		if (!pSum)
			continue;
		*pSize = *pTime = *pSum = '\0';

		char* pEnd = pSum + 1 + strcspn(pSum + 1, "\r\n");
		*pEnd = '\0';
		const size_t nSum = static_cast<size_t>(pEnd - (pSum + 1));
		if (!R5Dem_IsValidName(pName) || nSum == 0 || nSum > kSummaryMaxLen)
			continue;

		DemoSummaryEntry_s e;
		e.size = _strtoui64(pSize + 1, nullptr, 10);
		e.mtime = _strtoui64(pTime + 1, nullptr, 10);
		e.summary.assign(pSum + 1, nSum);
		s_summaryCache[pName] = std::move(e);
	}
	fclose(pFile);
}

static void DemoSummaryCache_Save(void)
{
	char szTmp[MAX_PATH];
	_snprintf_s(szTmp, _TRUNCATE, "%s.tmp", kSummaryCachePath);

	FILE* pFile = nullptr;
	if (fopen_s(&pFile, szTmp, "wb") != 0 || !pFile)
		return;
	for (const auto& kv : s_summaryCache)
	{
		if (!s_listedDemos.empty() && !s_listedDemos.count(kv.first))
			continue;
		fprintf(pFile, "%s\t%llu\t%llu\t%s\n", kv.first.c_str(),
			static_cast<unsigned long long>(kv.second.size),
			static_cast<unsigned long long>(kv.second.mtime), kv.second.summary.c_str());
	}
	fclose(pFile);

	if (!MoveFileExA(szTmp, kSummaryCachePath, MOVEFILE_REPLACE_EXISTING))
		DeleteFileA(szTmp);
}

static SQRESULT Script_Demo_ListFiles(HSQUIRRELVM v)
{
	Demo_RecoverParts(DEMO_CLIENT_ROOT);
	std::vector<DemoFileInfo_s> files;
	Demo_ListFiles(DEMO_CLIENT_ROOT, files, kDemoListMax);
	std::vector<std::string> names;
	names.reserve(files.size());
	s_listedDemos.clear();
	for (const DemoFileInfo_s& f : files)
	{
		names.emplace_back(f.name);
		s_listedDemos[f.name] = f;
	}
	DemoNative_PushStringArray(v, names);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static bool DemoNative_OpenByName(const char* pszName, CDemoReader& reader)
{
	char szPath[260];
	if (!R5Dem_IsValidName(pszName) || !Demo_FindFile(DEMO_CLIENT_ROOT, pszName, szPath, sizeof(szPath)))
		return false;
	char szErr[128];
	return reader.Open(szPath, szErr, sizeof(szErr));
}

static SQRESULT Script_Demo_GetFileMetaJson(HSQUIRRELVM v)
{
	const char* const pszName = DemoNative_GetString(v, 2);
	CDemoReader reader;
	if (pszName && DemoNative_OpenByName(pszName, reader))
	{
		const std::string& meta = reader.GetMetaJson();
		sq_pushstring(v, meta.c_str(), static_cast<SQInteger>(meta.size()));
	}
	else
	{
		sq_pushstring(v, "", 0);
	}
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// "map|recorder|povCount|pov0name|pov1name|winnerPov|durationSec|startUnix|partial|playable|mode|modeTitle"
static bool DemoNative_BuildSummary(const char* pszName, char* out, const size_t nOutLen)
{
	CDemoReader reader;
	if (!pszName || !DemoNative_OpenByName(pszName, reader))
		return false;

	const std::string& meta = reader.GetMetaJson();
	const char* const s = meta.c_str();
	const size_t n = meta.size();

	char szMap[64] = {}, szRecorder[16] = {}, szMode[64] = {}, szModeTitle[96] = {};
	DemoJson::GetString(s, n, "map", szMap, sizeof(szMap));
	DemoJson::GetString(s, n, "mode", szMode, sizeof(szMode));
	DemoJson::GetString(s, n, "modeTitle", szModeTitle, sizeof(szModeTitle));
	if (!szMode[0])
		strncpy_s(szMode, reader.GetHeader().mode, _TRUNCATE);
	if (!szMap[0])
		strncpy_s(szMap, reader.GetHeader().map, _TRUNCATE);
	DemoJson::GetString(s, n, "recorder", szRecorder, sizeof(szRecorder));

	double winner = -1.0;
	double durationMs = 0.0;
	size_t oStart = 0, oLen = 0;
	if (DemoJson::GetObj(s, n, "outcome", &oStart, &oLen))
	{
		DemoJson::GetNumber(s + oStart, oLen, "winner", &winner);
		DemoJson::GetNumber(s + oStart, oLen, "durationMs", &durationMs);
	}
	if (durationMs <= 0.0)
		DemoJson::GetNumber(s, n, "durationMs", &durationMs);
	if (durationMs <= 0.0)
	{
		const uint32_t a = reader.GetFirstFullWallMs(0);
		const uint32_t b = reader.GetLastWallMs(0);
		durationMs = (a != UINT32_MAX && b > a) ? static_cast<double>(b - a) : 0.0;
	}

	const std::vector<DemoPovInfo_s>& povs = reader.GetPovs();
	auto povName = [&](const int i) -> const char*
	{
		for (const DemoPovInfo_s& p : povs)
			if (p.id == i)
				return p.name;
		return "";
	};

	// Playable = the level's signon block plus live packets for pov 0.
	bool bSignon = false;
	for (const DemoChunkRef_s& c : reader.GetChunks())
	{
		if (c.hdr.type == static_cast<uint8_t>(R5DemChunk_t::SIGNON) && (c.hdr.pov == 0 || c.hdr.pov == R5DEM_POV_ALL))
		{
			bSignon = true;
			break;
		}
	}
	const bool bPlayable = bSignon && reader.GetFirstFullWallMs(0) != UINT32_MAX;

	// Script split() drops empty fields, so an empty one is sent as a space.
	auto field = [](const char* s) -> const char* { return (s && s[0]) ? s : " "; };

	snprintf(out, nOutLen, "%s|%s|%u|%s|%s|%d|%.1f|%llu|%d|%d|%s|%s",
		field(szMap), field(szRecorder), reader.GetHeader().povCount, field(povName(0)), field(povName(1)),
		static_cast<int>(winner), durationMs / 1000.0,
		static_cast<unsigned long long>(reader.GetHeader().startUnixMs / 1000ull),
		reader.IsTruncated() ? 1 : 0,
		bPlayable ? 1 : 0, field(szMode), field(szModeTitle));
	for (char* p = out; *p; ++p)
	{
		if (static_cast<unsigned char>(*p) < 0x20)
			*p = ' ';
	}
	return true;
}

static SQRESULT Script_Demo_GetFileSummary(HSQUIRRELVM v)
{
	const char* const pszName = DemoNative_GetString(v, 2);
	if (!pszName || !R5Dem_IsValidName(pszName))
	{
		sq_pushstring(v, "", 0);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	DemoSummaryCache_Load();
	const auto listed = s_listedDemos.find(pszName);
	if (listed != s_listedDemos.end())
	{
		const auto hit = s_summaryCache.find(pszName);
		if (hit != s_summaryCache.end() && hit->second.size == listed->second.size && hit->second.mtime == listed->second.mtime)
		{
			sq_pushstring(v, hit->second.summary.c_str(), static_cast<SQInteger>(hit->second.summary.size()));
			SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
		}
	}

	char out[kSummaryMaxLen + 1];
	if (!DemoNative_BuildSummary(pszName, out, sizeof(out)))
	{
		sq_pushstring(v, "", 0);
		SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
	}

	if (listed != s_listedDemos.end())
	{
		DemoSummaryEntry_s& e = s_summaryCache[pszName];
		e.size = listed->second.size;
		e.mtime = listed->second.mtime;
		e.summary = out;
		DemoSummaryCache_Save();
	}
	sq_pushstring(v, out, static_cast<SQInteger>(strlen(out)));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_DeleteFile(HSQUIRRELVM v)
{
	const char* const pszName = DemoNative_GetString(v, 2);
	bool bOk = false;
	if (pszName && R5Dem_IsValidName(pszName))
	{
		bOk = Demo_DeleteFile(DEMO_CLIENT_ROOT, pszName);
		if (bOk)
		{
			s_listedDemos.erase(pszName);
			if (s_summaryCache.erase(pszName))
				DemoSummaryCache_Save();
		}
	}
	else
		Warning(eDLL_T::CLIENT, "[DEMO] Demo_DeleteFile: invalid name -- ignored\n");
	sq_pushbool(v, bOk);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_Play(HSQUIRRELVM v)
{
	const char* const pszName = DemoNative_GetString(v, 2);
	if (pszName)
		DemoPlay_Start(pszName, 0, -1.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_Demo_PlayPov(HSQUIRRELVM v)
{
	const char* const pszName = DemoNative_GetString(v, 2);
	SQInteger nPov = 0;
	sq_getinteger(v, 3, &nPov);
	if (pszName && nPov >= 0 && nPov < R5DEM_MAX_POVS)
		DemoPlay_Start(pszName, static_cast<int>(nPov), -1.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Console
//-----------------------------------------------------------------------------
static void DemoMenu_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	Script_Execute_S21("OpenReplaysMenu( null )", SQCONTEXT::UI);
}
static ConCommand demo_menu("demo_menu", DemoMenu_f, "Open the Replays menu.", FCVAR_RELEASE);

void DemoNatives_StartOverlay(void)
{
	// Scripts without the overlay fail to compile the call and nothing runs.
	Script_Execute_S21("DemoOverlay_Start()", SQCONTEXT::CLIENT);
}

//-----------------------------------------------------------------------------
// Registration
//-----------------------------------------------------------------------------
struct DemoNativeReg_s
{
	const char* name;
	void*       func;
	const char* ret;
	const char* params;
};

static const DemoNativeReg_s s_sharedNatives[] =
{
	{ "Demo_IsPlaying",       (void*)Script_Demo_IsPlaying,       "bool",            "" },
	{ "Demo_IsRecording",     (void*)Script_Demo_IsRecording,     "bool",            "" },
	{ "Demo_GetTime",         (void*)Script_Demo_GetTime,         "float",           "" },
	{ "Demo_GetDuration",     (void*)Script_Demo_GetDuration,     "float",           "" },
	{ "Demo_GetPovCount",     (void*)Script_Demo_GetPovCount,     "int",             "" },
	{ "Demo_GetPov",          (void*)Script_Demo_GetPov,          "int",             "" },
	{ "Demo_GetPovName",      (void*)Script_Demo_GetPovName,      "string",          "int povId" },
	{ "Demo_GetMetaJson",     (void*)Script_Demo_GetMetaJson,     "string",          "" },
	{ "Demo_GetEventsJson",   (void*)Script_Demo_GetEventsJson,   "array< string >", "" },
	{ "Demo_GetEventRecords", (void*)Script_Demo_GetEventRecords, "array< string >", "" },
	{ "Demo_GetInputsAt",     (void*)Script_Demo_GetInputsAt,     "string",          "int povId, float seconds" },
	{ "Demo_IsPaused",        (void*)Script_Demo_IsPaused,        "bool",            "" },
	{ "Demo_IsSeeking",       (void*)Script_Demo_IsSeeking,       "bool",            "" },
	{ "Demo_GetTimescale",    (void*)Script_Demo_GetTimescale,    "float",           "" },
	{ "Demo_GetView",         (void*)Script_Demo_GetView,         "int",             "" },
	{ "Demo_IsFreecam",       (void*)Script_Demo_IsFreecam,       "bool",            "" },
	{ "Demo_SetCamera",       (void*)Script_Demo_SetCamera,       "bool",            "int mode" },
	{ "Demo_GetCamera",       (void*)Script_Demo_GetCamera,       "int",             "" },
	{ "Demo_AddBookmark",     (void*)Script_Demo_AddBookmark,     "bool",            "" },
	{ "Demo_SaveClip",        (void*)Script_Demo_SaveClip,        "string",          "float startSeconds, float endSeconds" },
	{ "Demo_Stop",            (void*)Script_Demo_Stop,            "void",            "" },
	{ "Demo_Pause",           (void*)Script_Demo_Pause,           "void",            "bool paused" },
	{ "Demo_Step",            (void*)Script_Demo_Step,            "void",            "" },
	{ "Demo_SetTimescale",    (void*)Script_Demo_SetTimescale,    "void",            "float scale" },
	{ "Demo_Seek",            (void*)Script_Demo_Seek,            "void",            "float seconds" },
	{ "Demo_SetPov",          (void*)Script_Demo_SetPov,          "void",            "int povId" },
	{ "Demo_SetView",         (void*)Script_Demo_SetView,         "void",            "int povId" },
	{ "Demo_SetFreecam",      (void*)Script_Demo_SetFreecam,      "void",            "bool on" },
	{ "Demo_SetHud",          (void*)Script_Demo_SetHud,          "void",            "int mode" },
	{ "Demo_GetHud",          (void*)Script_Demo_GetHud,          "int",             "" },
};

static const DemoNativeReg_s s_clientNatives[] =
{
	{ "Demo_SetMeta",         (void*)Script_Demo_SetMeta,         "void",            "string json" },
	{ "Demo_RecordEvent",     (void*)Script_Demo_RecordEvent,     "bool",            "string type, string attacker, string victim, string weapon, float damage" },
	{ "Demo_OverlayStarted",  (void*)Script_Demo_OverlayStarted,  "void",            "" },
	{ "Demo_SetMouseLayer",   (void*)Script_Demo_SetMouseLayer,   "void",            "bool on" },
};

static const DemoNativeReg_s s_uiNatives[] =
{
	{ "Demo_ListFiles",       (void*)Script_Demo_ListFiles,       "array< string >", "" },
	{ "Demo_GetFileMetaJson", (void*)Script_Demo_GetFileMetaJson, "string",          "string name" },
	{ "Demo_GetFileSummary",  (void*)Script_Demo_GetFileSummary,  "string",          "string name" },
	{ "Demo_DeleteFile",      (void*)Script_Demo_DeleteFile,      "bool",            "string name" },
	{ "Demo_Play",            (void*)Script_Demo_Play,            "void",            "string name" },
	{ "Demo_PlayPov",         (void*)Script_Demo_PlayPov,         "void",            "string name, int povId" },
};

template <size_t N>
static void Demo_RegisterTable(CSquirrelVM* s, const DemoNativeReg_s (&table)[N])
{
	for (const DemoNativeReg_s& n : table)
	{
		if (Script_RegisterFuncTC_S21(s, n.name, n.func, n.ret, n.params) == SQ_ERROR)
			Warning(eDLL_T::CLIENT, "[S21-REG] %s registration FAILED\n", n.name);
	}
}

void Demo_RegisterClientFunctions(CSquirrelVM* s)
{
	if (!s)
		return;
	Demo_RegisterTable(s, s_sharedNatives);
	Demo_RegisterTable(s, s_clientNatives);
}

void Demo_RegisterUIFunctions(CSquirrelVM* s)
{
	if (!s)
		return;
	Demo_RegisterTable(s, s_sharedNatives);
	Demo_RegisterTable(s, s_uiNatives);
}
