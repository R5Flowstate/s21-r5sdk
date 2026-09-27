//=============================================================================//
//
// Purpose: Records the user commands the server executes for a player so a
// fake player can replay the exact input stream later.
//
//=============================================================================//
#include "core/stdafx.h"
#include "core/logdef.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "common/protocol.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"
#include "game/shared/vscript_gamedll_defs.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include "public/vscript/ivscript.h"
#include "game/shared/shareddefs.h"
#include "game/server/player.h"
#include "game/server/vscript_server.h"
#include "game/server/bot_cmd.h"
#include "engine/shared/demo_reader.h"
#include "cmd_recorder.h"
#include <new>

static ConVar cmdrec_max_recordings("cmdrec_max_recordings", "24", FCVAR_RELEASE,
	"Recorded input streams held in memory at once.", true, 1.f, true, 64.f);
static ConVar cmdrec_max_cmds("cmdrec_max_cmds", "9000", FCVAR_RELEASE,
	"Commands per recording before capture auto-stops (about 60 s at 144 Hz).", true, 100.f, true, 20000.f);
static ConVar cmdrec_diag("cmdrec_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log capture start/stop and per-recording stats.");

static constexpr int CMDREC_TABLE_MAX = 64;
static constexpr int CMDREC_MIN_CMDS = 10;

static CmdRecording_s s_recordings[CMDREC_TABLE_MAX];
static int s_activeRecording[MAX_PLAYERS];
static bool s_bInited = false;

static void CmdRecorder_EnsureInit(void)
{
	if (s_bInited)
		return;
	s_bInited = true;
	memset(s_recordings, 0, sizeof(s_recordings));
	for (int i = 0; i < MAX_PLAYERS; ++i)
		s_activeRecording[i] = -1;
}

static int CmdRecorder_SlotOf(const CPlayer* pPlayer)
{
	if (!pPlayer)
		return -1;
	const int nSlot = static_cast<int>(pPlayer->GetEdict()) - 1;
	if (nSlot < 0 || nSlot >= MAX_PLAYERS)
		return -1;
	return nSlot;
}

static CmdRecording_s* CmdRecorder_Slot(int nId)
{
	if (nId < 0 || nId >= CMDREC_TABLE_MAX)
		return nullptr;
	CmdRecording_s* const rec = &s_recordings[nId];
	return rec->m_bUsed ? rec : nullptr;
}

static void CmdRecorder_Release(CmdRecording_s* rec)
{
	if (rec->m_pCmds)
		free(rec->m_pCmds);
	memset(rec, 0, sizeof(*rec));
}

static bool CmdRecorder_Reserve(CmdRecording_s* rec, int nWanted)
{
	if (nWanted <= rec->m_nCapacity)
		return true;

	int nCap = rec->m_nCapacity ? rec->m_nCapacity * 2 : 1024;
	while (nCap < nWanted)
		nCap *= 2;

	void* const pNew = realloc(rec->m_pCmds, sizeof(CUserCmd) * static_cast<size_t>(nCap));
	if (!pNew)
		return false;

	rec->m_pCmds = reinterpret_cast<CUserCmd*>(pNew);
	rec->m_nCapacity = nCap;
	return true;
}

void CmdRecorder_OnRunCommand(CPlayer* pPlayer, const CUserCmd* pCmd)
{
	if (!s_bInited || !pPlayer || !pCmd)
		return;

	const int nSlot = CmdRecorder_SlotOf(pPlayer);
	if (nSlot < 0)
		return;

	const int nId = s_activeRecording[nSlot];
	if (nId < 0)
		return;

	CmdRecording_s* const rec = CmdRecorder_Slot(nId);
	if (!rec || !rec->m_bOpen)
	{
		s_activeRecording[nSlot] = -1;
		return;
	}

	if (rec->m_nCount >= cmdrec_max_cmds.GetInt())
	{
		CmdRecorder_Stop(pPlayer);
		return;
	}

	if (!CmdRecorder_Reserve(rec, rec->m_nCount + 1))
	{
		Warning(eDLL_T::SERVER, "[CMDREC] out of memory at %d cmds, stopping recording %d\n", rec->m_nCount, nId);
		CmdRecorder_Stop(pPlayer);
		return;
	}

	CUserCmd* const dst = &rec->m_pCmds[rec->m_nCount++];
	memcpy(dst, pCmd, sizeof(CUserCmd));

	// Wire fields with no replay value. Pings would fire on the bot's team.
	dst->command_time -= rec->m_flStartTimeBase;
	dst->randomseed = 0;
	memset(dst->m_pingCommands, 0, sizeof(dst->m_pingCommands));

	rec->m_flDuration += Max(dst->frametime, 0.0f);
}

int CmdRecorder_Start(CPlayer* pPlayer)
{
	CmdRecorder_EnsureInit();

	const int nSlot = CmdRecorder_SlotOf(pPlayer);
	if (nSlot < 0 || pPlayer->IsBot())
		return -1;

	if (s_activeRecording[nSlot] >= 0)
		return -1;

	int nUsed = 0;
	int nFree = -1;
	for (int i = 0; i < CMDREC_TABLE_MAX; ++i)
	{
		if (s_recordings[i].m_bUsed)
			++nUsed;
		else if (nFree < 0)
			nFree = i;
	}

	if (nFree < 0 || nUsed >= cmdrec_max_recordings.GetInt())
	{
		Warning(eDLL_T::SERVER, "[CMDREC] table full (%d), refusing recording for slot %d\n", nUsed, nSlot);
		return -1;
	}

	CmdRecording_s* const rec = &s_recordings[nFree];
	memset(rec, 0, sizeof(*rec));
	rec->m_bUsed = true;
	rec->m_bOpen = true;
	rec->m_nOwnerSlot = nSlot;
	rec->m_flStartTimeBase = pPlayer->GetTimeBase();
	rec->m_vecStartOrigin = pPlayer->Diag_AbsOrigin();
	pPlayer->EyeAngles(&rec->m_angStartAngles);

	s_activeRecording[nSlot] = nFree;

	if (cmdrec_diag.GetBool())
		Msg(eDLL_T::SERVER, "[CMDREC] start id=%d slot=%d org=(%.1f %.1f %.1f)\n", nFree, nSlot,
			rec->m_vecStartOrigin.x, rec->m_vecStartOrigin.y, rec->m_vecStartOrigin.z);

	return nFree;
}

int CmdRecorder_Stop(CPlayer* pPlayer)
{
	const int nSlot = CmdRecorder_SlotOf(pPlayer);
	if (nSlot < 0 || !s_bInited)
		return -1;

	const int nId = s_activeRecording[nSlot];
	s_activeRecording[nSlot] = -1;

	CmdRecording_s* const rec = CmdRecorder_Slot(nId);
	if (!rec)
		return -1;

	rec->m_bOpen = false;
	rec->m_nOwnerSlot = -1;

	if (rec->m_nCount < CMDREC_MIN_CMDS)
	{
		if (cmdrec_diag.GetBool())
			Msg(eDLL_T::SERVER, "[CMDREC] stop id=%d discarded (%d cmds)\n", nId, rec->m_nCount);
		CmdRecorder_Release(rec);
		return -1;
	}

	if (cmdrec_diag.GetBool())
		Msg(eDLL_T::SERVER, "[CMDREC] stop id=%d cmds=%d duration=%.2fs\n", nId, rec->m_nCount, rec->m_flDuration);

	return nId;
}

bool CmdRecorder_IsRecording(const CPlayer* pPlayer)
{
	const int nSlot = CmdRecorder_SlotOf(pPlayer);
	return s_bInited && nSlot >= 0 && s_activeRecording[nSlot] >= 0;
}

const CmdRecording_s* CmdRecorder_Get(int nId)
{
	if (!s_bInited)
		return nullptr;
	const CmdRecording_s* const rec = CmdRecorder_Slot(nId);
	if (!rec || rec->m_bOpen)
		return nullptr;
	return rec;
}

void CmdRecorder_Free(int nId)
{
	if (!s_bInited)
		return;
	CmdRecording_s* const rec = CmdRecorder_Slot(nId);
	if (!rec)
		return;

	BotCmd_OnRecordingFreed(nId);

	if (rec->m_nOwnerSlot >= 0 && rec->m_nOwnerSlot < MAX_PLAYERS)
		s_activeRecording[rec->m_nOwnerSlot] = -1;

	CmdRecorder_Release(rec);
}

//-----------------------------------------------------------------------------
// Builds a recording from one pov's USERCMD chunks of a server demo (or a drill
// cut by r5dem_to_cmdrec.py) so the Lab recorder can replay a demo moment.
// Seconds are relative to the pov's first full snapshot, or to its first
// command when the file has no packets.
//-----------------------------------------------------------------------------
int CmdRecorder_LoadDemo(const char* pszName, const int nPov, const float flStartSec, const float flEndSec)
{
	CmdRecorder_EnsureInit();

	ConVar* const pDir = g_pCVar ? g_pCVar->FindVar("sv_demo_dir") : nullptr;
	const char* const pszRoot = (pDir && pDir->GetString()) ? pDir->GetString() : "platform/demos";

	char szPath[260];
	if (!R5Dem_IsValidName(pszName) || !Demo_FindFile(pszRoot, pszName, szPath, sizeof(szPath)))
	{
		Warning(eDLL_T::SERVER, "[CMDREC] no demo named '%s' under %s\n", pszName ? pszName : "", pszRoot);
		return -1;
	}

	CDemoReader reader;
	char szErr[160];
	if (!reader.Open(szPath, szErr, sizeof(szErr)))
	{
		Warning(eDLL_T::SERVER, "[CMDREC] %s: %s\n", szPath, szErr);
		return -1;
	}
	if (nPov < 0 || nPov >= static_cast<int>(reader.GetHeader().povCount))
	{
		Warning(eDLL_T::SERVER, "[CMDREC] %s: pov %d out of range\n", pszName, nPov);
		return -1;
	}

	const float flTick = reader.GetHeader().tickIntervalUs ? reader.GetHeader().tickIntervalUs / 1000000.0f : 0.05f;
	uint32_t nBase = reader.GetFirstFullTick(nPov);

	struct Loaded_s { R5DemUserCmd_s cmd; };
	std::vector<Loaded_s> cmds;
	R5DemUserCmdPrefix_s firstPrefix;
	bool bHavePrefix = false;
	std::vector<uint8_t> payload;
	for (const DemoChunkRef_s& c : reader.GetChunks())
	{
		if (c.hdr.type != static_cast<uint8_t>(R5DemChunk_t::USERCMD) || c.hdr.pov != nPov)
			continue;
		if (!reader.ReadPayload(c, payload) || payload.size() < sizeof(R5DemUserCmdPrefix_s))
			continue;
		R5DemUserCmdPrefix_s pre;
		memcpy(&pre, payload.data(), sizeof(pre));
		const size_t nAvail = (payload.size() - sizeof(pre)) / sizeof(R5DemUserCmd_s);
		const size_t n = pre.count < nAvail ? pre.count : nAvail;
		for (size_t i = 0; i < n; ++i)
		{
			Loaded_s l;
			memcpy(&l.cmd, payload.data() + sizeof(pre) + i * sizeof(R5DemUserCmd_s), sizeof(R5DemUserCmd_s));
			if (nBase == UINT32_MAX)
				nBase = l.cmd.tickCount;
			const float t = (static_cast<int64_t>(l.cmd.tickCount) - static_cast<int64_t>(nBase)) * flTick;
			if (t < flStartSec || (flEndSec > flStartSec && t > flEndSec))
				continue;
			if (!bHavePrefix)
			{
				firstPrefix = pre;
				bHavePrefix = true;
			}
			if (cmds.size() < static_cast<size_t>(cmdrec_max_cmds.GetInt()))
				cmds.push_back(l);
		}
	}
	if (static_cast<int>(cmds.size()) < CMDREC_MIN_CMDS)
	{
		Warning(eDLL_T::SERVER, "[CMDREC] %s pov %d: only %zu commands in that range\n", pszName, nPov, cmds.size());
		return -1;
	}

	int nUsed = 0;
	int nFree = -1;
	for (int i = 0; i < CMDREC_TABLE_MAX; ++i)
	{
		if (s_recordings[i].m_bUsed)
			++nUsed;
		else if (nFree < 0)
			nFree = i;
	}
	if (nFree < 0 || nUsed >= cmdrec_max_recordings.GetInt())
	{
		Warning(eDLL_T::SERVER, "[CMDREC] table full (%d), demo input not loaded\n", nUsed);
		return -1;
	}

	CmdRecording_s* const rec = &s_recordings[nFree];
	memset(rec, 0, sizeof(*rec));
	if (!CmdRecorder_Reserve(rec, static_cast<int>(cmds.size())))
	{
		memset(rec, 0, sizeof(*rec));
		return -1;
	}
	rec->m_bUsed = true;
	rec->m_nOwnerSlot = -1;
	rec->m_vecStartOrigin = Vector3D(firstPrefix.origin[0], firstPrefix.origin[1], firstPrefix.origin[2]);
	rec->m_angStartAngles = QAngle(firstPrefix.angles[0], firstPrefix.angles[1], firstPrefix.angles[2]);

	float flTime = 0.0f;
	for (const Loaded_s& l : cmds)
	{
		CUserCmd* const dst = new (&rec->m_pCmds[rec->m_nCount++]) CUserCmd();
		const float ft = (l.cmd.frameTime == l.cmd.frameTime && l.cmd.frameTime > 0.0f && l.cmd.frameTime < 0.25f)
			? l.cmd.frameTime : flTick;
		dst->command_number = static_cast<int32_t>(l.cmd.commandNumber);
		dst->tick_count = static_cast<int32_t>(l.cmd.tickCount);
		dst->command_time = flTime;
		dst->viewangles.x = l.cmd.pitch;
		dst->viewangles.y = l.cmd.yaw;
		dst->forwardmove = l.cmd.forwardmove;
		dst->sidemove = l.cmd.sidemove;
		dst->upmove = l.cmd.upmove;
		dst->buttons = static_cast<int32_t>(l.cmd.buttons);
		dst->weaponindex = l.cmd.weaponSelectSlot;
		dst->impulse = l.cmd.impulse;
		dst->frametime = ft;
		flTime += ft;
	}
	rec->m_flDuration = flTime;

	if (cmdrec_diag.GetBool())
		Msg(eDLL_T::SERVER, "[CMDREC] loaded id=%d from demo '%s' pov %d: %d cmds %.2fs\n",
			nFree, pszName, nPov, rec->m_nCount, rec->m_flDuration);
	return nFree;
}

void CmdRecorder_OnPlayerGone(int nSlot)
{
	if (!s_bInited || nSlot < 0 || nSlot >= MAX_PLAYERS)
		return;
	const int nId = s_activeRecording[nSlot];
	s_activeRecording[nSlot] = -1;
	CmdRecording_s* const rec = CmdRecorder_Slot(nId);
	if (rec)
		CmdRecorder_Release(rec);
}

void CmdRecorder_LevelShutdown(void)
{
	if (!s_bInited)
		return;
	for (int i = 0; i < CMDREC_TABLE_MAX; ++i)
		if (s_recordings[i].m_bUsed)
			CmdRecorder_Release(&s_recordings[i]);
	for (int i = 0; i < MAX_PLAYERS; ++i)
		s_activeRecording[i] = -1;
}

//-----------------------------------------------------------------------------
// Squirrel
//-----------------------------------------------------------------------------
static CPlayer* CmdRec_ThisPlayer(HSQUIRRELVM v)
{
	void* pEntity = nullptr;
	if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pEntity)) || !pEntity)
		return nullptr;
	return reinterpret_cast<CPlayer*>(pEntity);
}

static SQRESULT Script_CmdRec_Start(HSQUIRRELVM v)
{
	CPlayer* const pPlayer = CmdRec_ThisPlayer(v);
	sq_pushinteger(v, pPlayer ? CmdRecorder_Start(pPlayer) : -1);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_CmdRec_Stop(HSQUIRRELVM v)
{
	CPlayer* const pPlayer = CmdRec_ThisPlayer(v);
	sq_pushinteger(v, pPlayer ? CmdRecorder_Stop(pPlayer) : -1);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT Script_CmdRec_IsRecording(HSQUIRRELVM v)
{
	CPlayer* const pPlayer = CmdRec_ThisPlayer(v);
	sq_pushbool(v, pPlayer && CmdRecorder_IsRecording(pPlayer));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static const CmdRecording_s* CmdRec_ArgRecording(HSQUIRRELVM v)
{
	SQInteger nId = -1;
	sq_getinteger(v, 2, &nId);
	return CmdRecorder_Get(static_cast<int>(nId));
}

SQRESULT ServerScript_CmdRec_GetDuration(HSQUIRRELVM v)
{
	const CmdRecording_s* const rec = CmdRec_ArgRecording(v);
	sq_pushfloat(v, rec ? rec->m_flDuration : 0.0f);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT ServerScript_CmdRec_GetCount(HSQUIRRELVM v)
{
	const CmdRecording_s* const rec = CmdRec_ArgRecording(v);
	sq_pushinteger(v, rec ? rec->m_nCount : 0);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT ServerScript_CmdRec_Free(HSQUIRRELVM v)
{
	SQInteger nId = -1;
	sq_getinteger(v, 2, &nId);
	CmdRecorder_Free(static_cast<int>(nId));
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT ServerScript_CmdRec_GetStartOrigin(HSQUIRRELVM v)
{
	const CmdRecording_s* const rec = CmdRec_ArgRecording(v);
	SQVector3D out(0.0f, 0.0f, 0.0f);
	if (rec)
		out = SQVector3D(rec->m_vecStartOrigin.x, rec->m_vecStartOrigin.y, rec->m_vecStartOrigin.z);
	sq_pushvector(v, &out);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT ServerScript_CmdRec_LoadDemo(HSQUIRRELVM v)
{
	const SQChar* pszName = nullptr;
	SQInteger nPov = 0;
	SQFloat flStart = 0.0f, flEnd = 0.0f;
	sq_getstring(v, 2, &pszName);
	sq_getinteger(v, 3, &nPov);
	sq_getfloat(v, 4, &flStart);
	sq_getfloat(v, 5, &flEnd);
	const bool bArgsOk = pszName && nPov >= 0 && nPov < R5DEM_MAX_POVS && flStart == flStart && flEnd == flEnd;
	sq_pushinteger(v, bArgsOk ? CmdRecorder_LoadDemo(pszName, static_cast<int>(nPov), flStart, flEnd) : -1);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

SQRESULT ServerScript_CmdRec_GetStartAngles(HSQUIRRELVM v)
{
	const CmdRecording_s* const rec = CmdRec_ArgRecording(v);
	SQVector3D out(0.0f, 0.0f, 0.0f);
	if (rec)
		out = SQVector3D(rec->m_angStartAngles.x, rec->m_angStartAngles.y, rec->m_angStartAngles.z);
	sq_pushvector(v, &out);
	SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

void CmdRecorder_RegisterPlayerFuncs(ScriptClassDescriptor_t* pPlayerStruct)
{
	if (!pPlayerStruct)
		return;

	CmdRecorder_EnsureInit();

	pPlayerStruct->AddFunction("CmdRec_Start", "Script_CmdRec_Start",
		"Start recording this player's executed input. Returns recording id or -1.", "int", "", false, Script_CmdRec_Start);
	pPlayerStruct->AddFunction("CmdRec_Stop", "Script_CmdRec_Stop",
		"Stop recording. Returns recording id, or -1 when nothing usable was captured.", "int", "", false, Script_CmdRec_Stop);
	pPlayerStruct->AddFunction("CmdRec_IsRecording", "Script_CmdRec_IsRecording",
		"True while this player's input is being recorded.", "bool", "", false, Script_CmdRec_IsRecording);
}

void CmdRecorder_RegisterGlobalFuncs(CSquirrelVM* pVM)
{
	CmdRecorder_EnsureInit();

	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, CmdRec_GetDuration, "Duration in seconds of a recorded input stream.", "float", "int recordingId", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, CmdRec_GetCount, "Number of commands in a recorded input stream.", "int", "int recordingId", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, CmdRec_Free, "Free a recorded input stream and stop any bot playing it.", "void", "int recordingId", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, CmdRec_GetStartOrigin, "World origin the recording started at.", "vector", "int recordingId", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, CmdRec_GetStartAngles, "Eye angles the recording started at.", "vector", "int recordingId", false);
	DEFINE_SERVER_SCRIPTFUNC_NAMED(pVM, CmdRec_LoadDemo, "Load one pov's recorded input from a server demo between two times (end <= start = to the end). Returns recording id or -1.", "int", "string demoName, int povId, float startSeconds, float endSeconds", false);
}
