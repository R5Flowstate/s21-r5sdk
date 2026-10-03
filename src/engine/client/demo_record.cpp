//=============================================================================//
//
// Purpose: client demo recorder. Captures the consume side of the bridge: every
//          datagram Hook_ProcessPacket parses and every DataBlock
//          OnDataBlockComplete receives, in the order they were consumed.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "engine/cmd.h"
#include "ebisusdk/EbisuSDK.h"
#include "engine/shared/demo_writer.h"
#include "engine/shared/demo_reader.h"
#include "engine/shared/demo_json.h"
#include "engine/client/demo_bridge.h"
#include "engine/client/demo_record.h"
#include "rtech/playlists/playlists.h"
#include <ctime>
#include <deque>
#include <string>
#include <vector>

static ConVar demo_record_full_wait_s("demo_record_full_wait_s", "30", FCVAR_RELEASE,
	"Seconds a mid-match demo_record waits for a natural full snapshot before asking the server for one.",
	true, 0.f, true, 600.f);
static ConVar demo_auto_record("demo_auto_record", "1", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Record every match you play to platform/demos (the lobby is skipped). They show up in the Replays menu.");
static ConVar demo_auto_min_s("demo_auto_min_s", "0", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Automatic recordings shorter than this many seconds are thrown away. 0 = keep every one.", true, 0.f, true, 3600.f);
static ConVar demo_keep_days("demo_keep_days", "14", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Replays older than this are deleted when a new recording starts. 0 = keep forever.", true, 0.f, true, 3650.f);
static ConVar demo_record_keyframe_s("demo_record_keyframe_s", "20", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"While recording and not playing (dead or spectating), ask the server for a full snapshot at most this often "
	"so replays can jump when seeking. A full snapshot recreates every entity, so it is never taken while you "
	"play. 0 = never.", true, 0.f, true, 3600.f);
static ConVar demo_max_mb("demo_max_mb", "4096", FCVAR_RELEASE | FCVAR_ARCHIVE,
	"Total size cap of platform/demos; the oldest replays go first. 0 = no cap.", true, 0.f, true, 1048576.f);

static constexpr int      kSignonFull = 8;
static constexpr size_t   kHistoryBudget = 16u * 1024u * 1024u;
static const char* const  kClientDemoRoot = "platform\\demos";

enum class DemoRecState_t
{
	OFF,
	WAIT_FULL,   // history written, live packets held until a full snapshot keys the stream
	RECORDING,
};

// Everything since connect that a mid-session recording needs before its
// first full snapshot: the datagrams of the connect and signon walk, the
// signon block, later DataBlocks, and the reliable part of later datagrams.
struct DemoHistoryEntry_s
{
	R5DemChunk_t type;
	uint8_t      signon;
	uint8_t      kind;
	uint32_t     tick;
	std::vector<uint8_t> data;
};

static SRWLOCK        s_recLock = SRWLOCK_INIT;
static CDemoWriter*   s_pWriter = nullptr;
static DemoRecState_t s_recState = DemoRecState_t::OFF;
static ULONGLONG      s_waitFullStartMs = 0;
static bool           s_bFullRequested = false;
static bool           s_bAutoStarted = false;
static bool           s_bAutoRecording = false;
static int            s_nFrameSignon = 0;
static uint32_t       s_nPackets = 0;
static ULONGLONG      s_nLastViewMs = 0;
static bool           s_bModeMetaWritten = false;
static uint32_t       s_nFirstFullTick = 0;
static std::string    s_scriptMeta;

static std::deque<DemoHistoryEntry_s> s_history;
static size_t s_nHistoryBytes = 0;
static bool   s_bHistorySigned = false;   // holds a signon block
static bool   s_bHistoryOverflow = false;

static std::vector<uint8_t> s_packetScratch;
static bool s_bPacketOpen = false;
// The bridge preprocesses a block in place, so the raw bytes are kept until
// its decode says whether it held a full snapshot.
static std::vector<uint8_t> s_blockScratch;
static bool s_bBlockOpen = false;
static ULONGLONG s_nLastKeyframeMs = 0;

// The local player's commands, written in batches as USERCMD chunks.
static constexpr size_t kUserCmdBatch = 64;
static std::vector<R5DemUserCmd_s> s_userCmds;

static void (*v_CL_ForceFullUpdate)(void) = nullptr;

bool DemoRecord_IsRecording(void)
{
	return s_recState != DemoRecState_t::OFF;
}

static uint8_t DemoRecord_Signon(void)
{
	const int n = S21Bridge_GetClientSignonState();
	return static_cast<uint8_t>(n < 0 ? 0 : (n > 255 ? 255 : n));
}

static void DemoRecord_HistoryClear(void)
{
	s_history.clear();
	s_nHistoryBytes = 0;
	s_bHistorySigned = false;
	s_bHistoryOverflow = false;
}

static void DemoRecord_HistoryPush(const R5DemChunk_t type, const uint8_t nSignon, const uint8_t nKind,
	const uint32_t nTick, const uint8_t* pData, const size_t nLen)
{
	if (s_bHistoryOverflow)
		return;
	if (s_nHistoryBytes + nLen > kHistoryBudget)
	{
		s_bHistoryOverflow = true;
		Warning(eDLL_T::ENGINE, "[DEMO] session history passed %zu MB -- a mid-session demo_record now needs a reconnect\n",
			kHistoryBudget / (1024 * 1024));
		return;
	}
	DemoHistoryEntry_s e;
	e.type = type;
	e.signon = nSignon;
	e.kind = nKind;
	e.tick = nTick;
	e.data.assign(pData, pData + nLen);
	s_nHistoryBytes += nLen;
	s_history.push_back(std::move(e));
}

extern const char* S21Bridge_GetCurrentPlaylistName(void);

// The playlist's display title: its r5f_mode_title, else its name (which may be
// a #token the game localizes).
static const char* DemoRecord_PlaylistTitle(const char* pszPlaylist)
{
	if (!pszPlaylist || !pszPlaylist[0])
		return "";
	KeyValues* const pRoot = Playlists_GetRootKV();
	KeyValues* const pPlaylists = pRoot ? pRoot->FindKey("Playlists") : nullptr;
	KeyValues* const pPl = pPlaylists ? pPlaylists->FindKey(pszPlaylist) : nullptr;
	KeyValues* const pVars = pPl ? pPl->FindKey("vars") : nullptr;
	if (!pVars)
		return "";
	const char* const pszTitle = pVars->GetString("r5f_mode_title", "");
	if (pszTitle && pszTitle[0])
		return pszTitle;
	const char* const pszName = pVars->GetString("name", "");
	return pszName ? pszName : "";
}

static void DemoRecord_WriteMeta(const bool bFinal)
{
	if (!s_pWriter)
		return;

	char szName[160];
	DemoJson::Escape((g_PersonaName && g_PersonaName[0]) ? g_PersonaName : "unnamed", szName, sizeof(szName));
	char szMap[80];
	DemoJson::Escape(S21Bridge_ConnMapName(), szMap, sizeof(szMap));
	const char* const pszPlaylist = S21Bridge_GetCurrentPlaylistName();
	char szMode[160];
	DemoJson::Escape(pszPlaylist ? pszPlaylist : "", szMode, sizeof(szMode));
	char szModeTitle[160];
	DemoJson::Escape(DemoRecord_PlaylistTitle(pszPlaylist), szModeTitle, sizeof(szModeTitle));

	std::string json;
	json.reserve(1024);
	char buf[1024];
	snprintf(buf, sizeof(buf),
		"{\"protocol\":%u,\"recorder\":\"client\",\"map\":\"%s\",\"mode\":\"%s\",\"modeTitle\":\"%s\","
		"\"clientBuild\":\"%08X\",\"povs\":[{\"id\":0,\"slot\":-1,\"name\":\"%s\",\"team\":0,\"eh\":-1}]",
		R5DEM_PROTOCOL, szMap, szMode, szModeTitle, g_SDKDll.GetNTHeaders()->FileHeader.TimeDateStamp, szName);
	json += buf;

	if (bFinal)
	{
		snprintf(buf, sizeof(buf), ",\"durationMs\":%u,\"droppedChunks\":%u,\"packets\":%u,\"firstFullTick\":%u",
			s_pWriter->GetWallMs(), s_pWriter->GetDropped(), s_nPackets, s_nFirstFullTick);
		json += buf;
	}

	if (!s_scriptMeta.empty())
	{
		json += ",\"script\":";
		json += s_scriptMeta;
	}
	json += "}";

	if (json.size() <= R5DEM_MAX_JSON)
		s_pWriter->Append(R5DemChunk_t::META, R5DEM_POV_ALL, DemoRecord_Signon(), 0, 0,
			json.data(), static_cast<uint32_t>(json.size()));
}

static void DemoRecord_FlushUserCmdsLocked(void)
{
	if (s_userCmds.empty() || !s_pWriter)
	{
		s_userCmds.clear();
		return;
	}
	R5DemUserCmdPrefix_s pre = {};
	pre.count = static_cast<uint16_t>(s_userCmds.size());
	pre.angles[0] = s_userCmds.front().pitch;
	pre.angles[1] = s_userCmds.front().yaw;
	std::vector<uint8_t> payload(sizeof(pre) + s_userCmds.size() * sizeof(R5DemUserCmd_s));
	memcpy(payload.data(), &pre, sizeof(pre));
	memcpy(payload.data() + sizeof(pre), s_userCmds.data(), s_userCmds.size() * sizeof(R5DemUserCmd_s));
	s_pWriter->Append(R5DemChunk_t::USERCMD, 0, DemoRecord_Signon(), 0, s_userCmds.front().tickCount,
		payload.data(), static_cast<uint32_t>(payload.size()));
	s_userCmds.clear();
}

void DemoRecord_OnUserCmd(const R5DemUserCmd_s& cmd)
{
	if (s_recState != DemoRecState_t::RECORDING)
		return;
	AcquireSRWLockExclusive(&s_recLock);
	if (s_pWriter && s_recState == DemoRecState_t::RECORDING)
	{
		s_userCmds.push_back(cmd);
		if (s_userCmds.size() >= kUserCmdBatch)
			DemoRecord_FlushUserCmdsLocked();
	}
	ReleaseSRWLockExclusive(&s_recLock);
}

static void DemoRecord_StopLocked(const char* pszReason, const bool bKeep)
{
	if (!s_pWriter)
	{
		s_userCmds.clear();
		s_recState = DemoRecState_t::OFF;
		return;
	}
	DemoRecord_FlushUserCmdsLocked();

	const uint32_t nMs = s_pWriter->GetWallMs();
	const bool bHasData = s_nPackets > 0 &&
		!(s_bAutoRecording && nMs < static_cast<uint32_t>(demo_auto_min_s.GetFloat() * 1000.0f));
	s_bAutoRecording = false;
	if (bKeep && bHasData)
		DemoRecord_WriteMeta(true);

	char szPath[260];
	strncpy_s(szPath, s_pWriter->GetFinalPath(), _TRUNCATE);
	const uint32_t nDropped = s_pWriter->GetDropped();

	s_pWriter->Close(bKeep && bHasData);
	delete s_pWriter;
	s_pWriter = nullptr;
	s_recState = DemoRecState_t::OFF;
	s_bPacketOpen = false;

	if (bKeep && bHasData)
		Msg(eDLL_T::ENGINE, "[DEMO] recording stopped (%s): '%s' %.1f s, %u packets, %u dropped\n",
			pszReason, szPath, nMs / 1000.0f, s_nPackets, nDropped);
	else
		Msg(eDLL_T::ENGINE, "[DEMO] recording discarded (%s)\n", pszReason);
}

void DemoRecord_Stop(const char* pszReason)
{
	AcquireSRWLockExclusive(&s_recLock);
	DemoRecord_StopLocked(pszReason, true);
	ReleaseSRWLockExclusive(&s_recLock);
}

// Process exit: other threads are already gone, so never block on the lock.
void DemoRecord_Shutdown(void)
{
	if (!TryAcquireSRWLockExclusive(&s_recLock))
		return;
	DemoRecord_StopLocked("shutdown", true);
	ReleaseSRWLockExclusive(&s_recLock);
}

static bool DemoRecord_EncryptionOff(void)
{
	ConVar* const pVar = g_pCVar ? g_pCVar->FindVar("bridge_force_encryption_off") : nullptr;
	return !pVar || pVar->GetBool();
}

bool DemoRecord_Start(const char* pszName, const bool bFromConnect)
{
	if (Demo_IsPlaying())
	{
		Warning(eDLL_T::ENGINE, "[DEMO] cannot record while a demo is playing\n");
		return false;
	}
	if (!DemoRecord_EncryptionOff())
	{
		Warning(eDLL_T::ENGINE, "[DEMO] bridge_force_encryption_off is 0 -- a recording would hold ciphertext\n");
		return false;
	}

	char szBase[72];
	if (pszName && pszName[0])
	{
		if (!R5Dem_IsValidName(pszName))
		{
			Warning(eDLL_T::ENGINE, "[DEMO] demo names are [a-zA-Z0-9_-], 1..64 characters\n");
			return false;
		}
		strncpy_s(szBase, pszName, _TRUNCATE);
	}
	else
	{
		char szMap[40];
		Demo_SanitizeName(S21Bridge_ConnMapName()[0] ? S21Bridge_ConnMapName() : "unknown", szMap, sizeof(szMap));
		char szPrefix[24] = {};
		ConVar* const pPrefix = g_pCVar ? g_pCVar->FindVar("demo_autoRecordName") : nullptr;
		if (pPrefix && pPrefix->GetString())
			Demo_SanitizeName(pPrefix->GetString(), szPrefix, sizeof(szPrefix));
		const time_t now = time(nullptr);
		tm local = {};
		localtime_s(&local, &now);
		char szWhen[24];
		strftime(szWhen, sizeof(szWhen), "%Y-%m-%d_%H-%M-%S", &local);
		snprintf(szBase, sizeof(szBase), "%s%s%s_%s", szPrefix, szPrefix[0] ? "_" : "", szWhen, szMap);
		szBase[64] = '\0';
	}

	Demo_RecoverParts(kClientDemoRoot);
	Demo_Prune(kClientDemoRoot, demo_keep_days.GetInt(), demo_max_mb.GetInt());

	char szPath[260];
	if (!Demo_BuildRecordPath(kClientDemoRoot, szBase, szPath, sizeof(szPath)))
		return false;

	AcquireSRWLockExclusive(&s_recLock);
	if (s_pWriter)
	{
		ReleaseSRWLockExclusive(&s_recLock);
		Warning(eDLL_T::ENGINE, "[DEMO] already recording\n");
		return false;
	}

	const int nSignon = S21Bridge_GetClientSignonState();
	const bool bMidSession = !bFromConnect && s_bHistorySigned;
	if (bMidSession && s_bHistoryOverflow)
	{
		ReleaseSRWLockExclusive(&s_recLock);
		Warning(eDLL_T::ENGINE, "[DEMO] this session's history overflowed -- reconnect to record\n");
		return false;
	}
	if (!bFromConnect && !bMidSession && nSignon > 2)
	{
		ReleaseSRWLockExclusive(&s_recLock);
		Warning(eDLL_T::ENGINE, "[DEMO] no signon block was captured this session -- reconnect to record\n");
		return false;
	}

	R5DemHeader_s hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.tickIntervalUs = static_cast<uint32_t>(S21Bridge_IntervalPerTick() * 1000000.0f + 0.5f);
	hdr.povCount = 1;
	hdr.startUnixMs = static_cast<uint64_t>(time(nullptr)) * 1000ull;
	strncpy_s(hdr.map, S21Bridge_ConnMapName(), _TRUNCATE);

	s_pWriter = new CDemoWriter();
	// The prune only runs at start, so one recording must not outgrow the cap on its own.
	if (demo_max_mb.GetInt() > 0)
		s_pWriter->SetByteBudget(static_cast<uint64_t>(demo_max_mb.GetInt()) * 1024 * 1024, nullptr);
	if (!s_pWriter->Open(szPath, hdr))
	{
		delete s_pWriter;
		s_pWriter = nullptr;
		ReleaseSRWLockExclusive(&s_recLock);
		return false;
	}

	s_nPackets = 0;
	s_userCmds.clear();
	s_bModeMetaWritten = false;
	s_nFirstFullTick = 0;
	s_bFullRequested = false;
	s_scriptMeta.clear();
	DemoRecord_WriteMeta(false);

	if (bMidSession)
	{
		for (const DemoHistoryEntry_s& e : s_history)
		{
			const uint8_t nKind = (e.type == R5DemChunk_t::PACKET) ? static_cast<uint8_t>(e.kind | R5DEM_KIND_PRELUDE) : e.kind;
			s_pWriter->Append(e.type, 0, e.signon, nKind, e.tick, e.data.data(), static_cast<uint32_t>(e.data.size()));
		}
		if (s_pWriter->GetDropped() || s_pWriter->LostSignon())
		{
			DemoRecord_StopLocked("history did not fit the write ring", false);
			ReleaseSRWLockExclusive(&s_recLock);
			return false;
		}
		s_recState = nSignon >= kSignonFull ? DemoRecState_t::WAIT_FULL : DemoRecState_t::RECORDING;
		s_waitFullStartMs = GetTickCount64();
		if (s_recState == DemoRecState_t::WAIT_FULL)
			Msg(eDLL_T::ENGINE, "[DEMO] waiting for a full snapshot\n");
	}
	else
	{
		s_recState = DemoRecState_t::RECORDING;
	}
	ReleaseSRWLockExclusive(&s_recLock);

	Msg(eDLL_T::ENGINE, "[DEMO] recording to '%s'\n", szPath);
	return true;
}

void DemoRecord_SetMeta(const char* pszJson)
{
	if (!pszJson)
		return;
	const size_t n = strnlen(pszJson, R5DEM_MAX_JSON / 2 + 1);
	if (n == 0 || n > R5DEM_MAX_JSON / 2)
		return;
	const size_t i = DemoJson::SkipWs(pszJson, 0, n);
	if (i >= n || pszJson[i] != '{' || DemoJson::SkipWs(pszJson, DemoJson::SkipValue(pszJson, i, n), n) != n)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] Demo_SetMeta: not a JSON object -- ignored\n");
		return;
	}
	AcquireSRWLockExclusive(&s_recLock);
	s_scriptMeta.assign(pszJson + i, n - i);
	ReleaseSRWLockExclusive(&s_recLock);
}

// A moment the live game reports (kill, knock, round start, ...). Names are
// the players' display names: a client recording has one pov, so pov ids
// cannot tell the other players apart.
bool DemoRecord_AddEvent(const char* pszType, const char* pszAttacker, const char* pszVictim,
	const char* pszWeapon, const float flDamage)
{
	static uint32_t s_nEvents = 0;
	static ULONGLONG s_nWindowMs = 0;
	static uint32_t s_nInWindow = 0;

	char szType[16];
	Demo_SanitizeName(pszType ? pszType : "", szType, sizeof(szType));
	if (!szType[0])
		return false;
	char szAttacker[112];
	DemoJson::Escape(pszAttacker ? pszAttacker : "", szAttacker, sizeof(szAttacker), 48);
	char szVictim[112];
	DemoJson::Escape(pszVictim ? pszVictim : "", szVictim, sizeof(szVictim), 48);
	char szWeapon[48];
	Demo_SanitizeName(pszWeapon ? pszWeapon : "", szWeapon, sizeof(szWeapon));
	const float flDmg = (flDamage == flDamage && flDamage >= 0.0f && flDamage < 100000.0f) ? flDamage : 0.0f;

	AcquireSRWLockExclusive(&s_recLock);
	if (!s_pWriter || s_recState != DemoRecState_t::RECORDING)
	{
		s_nEvents = 0;
		ReleaseSRWLockExclusive(&s_recLock);
		return false;
	}

	// A runaway script must not bloat the file: 20 per second, 20000 per recording.
	const ULONGLONG nNow = GetTickCount64();
	if (nNow - s_nWindowMs >= 1000)
	{
		s_nWindowMs = nNow;
		s_nInWindow = 0;
	}
	if (++s_nInWindow > 20 || s_nEvents >= 20000)
	{
		ReleaseSRWLockExclusive(&s_recLock);
		return false;
	}

	char szJson[512];
	const int n = snprintf(szJson, sizeof(szJson),
		"{\"t\":\"%s\",\"tick\":%u,\"a\":-1,\"v\":-1,\"an\":\"%s\",\"vn\":\"%s\",\"w\":\"%s\",\"d\":%.1f}",
		szType, S21Bridge_LastSnapshotTick(), szAttacker, szVictim, szWeapon, flDmg);
	const bool bOk = n > 0 && n < static_cast<int>(sizeof(szJson))
		&& s_pWriter->Append(R5DemChunk_t::EVENT, R5DEM_POV_ALL, DemoRecord_Signon(), 0,
			S21Bridge_LastSnapshotTick(), szJson, static_cast<uint32_t>(n));
	if (bOk)
		++s_nEvents;
	ReleaseSRWLockExclusive(&s_recLock);
	return bOk;
}

// Automatic recordings open on the first netchan packet of a connection, so
// the file holds the whole session: signon, the first full snapshot and every
// packet after it.
static void DemoRecord_AutoStartAtConnect(void)
{
	if (s_bAutoStarted || Demo_IsPlaying())
		return;
	s_bAutoStarted = true;
	if (DemoRecord_IsRecording())
		return;

	ConVar* const pAuto = g_pCVar ? g_pCVar->FindVar("demo_autoRecord") : nullptr;
	if (!demo_auto_record.GetBool() && !(pAuto && pAuto->GetBool()))
		return;
	const char* const pszMap = S21Bridge_ConnMapName();
	if (pszMap && _strnicmp(pszMap, "mp_lobby", 8) == 0)
		return;

	if (DemoRecord_Start(nullptr, true))
		s_bAutoRecording = true;
	else
		Warning(eDLL_T::ENGINE, "[DEMO] automatic recording could not start\n");
}

void DemoRecord_OnDataBlock(const uint8_t* pRaw, const int nSize)
{
	s_bBlockOpen = false;
	if (!pRaw || nSize <= 0 || static_cast<uint32_t>(nSize) > R5DEM_MAX_BLOCK || Demo_IsPlaying())
		return;
	DemoRecord_AutoStartAtConnect();
	s_blockScratch.assign(pRaw, pRaw + nSize);
	s_bBlockOpen = true;
}

void DemoRecord_OnDataBlockEnd(const bool bFull)
{
	if (!s_bBlockOpen)
		return;
	s_bBlockOpen = false;
	const uint8_t* const pRaw = s_blockScratch.data();
	const int nSize = static_cast<int>(s_blockScratch.size());
	const uint8_t nKind = bFull ? R5DEM_KIND_FULL : 0;
	if (bFull)
		s_nLastKeyframeMs = GetTickCount64();

	const int nSignon = S21Bridge_GetClientSignonState();
	const bool bSignon = nSignon < kSignonFull;
	const uint8_t nSignonByte = static_cast<uint8_t>(nSignon < 0 ? 0 : nSignon);
	const R5DemChunk_t type = bSignon ? R5DemChunk_t::SIGNON : R5DemChunk_t::RELIABLE;

	AcquireSRWLockExclusive(&s_recLock);
	// A second signon block is a new level; the datagrams since connect stay
	// ahead of the first one.
	if (bSignon && s_bHistorySigned)
		DemoRecord_HistoryClear();
	DemoRecord_HistoryPush(type, nSignonByte, nKind, 0, pRaw, static_cast<size_t>(nSize));
	if (bSignon)
		s_bHistorySigned = true;

	if (s_pWriter && s_recState != DemoRecState_t::OFF)
	{
		s_pWriter->Append(type, 0, nSignonByte, nKind, S21Bridge_LastSnapshotTick(), pRaw, static_cast<uint32_t>(nSize));
		if (s_pWriter->LostSignon())
		{
			DemoRecord_StopLocked("signon block dropped", false);
			Warning(eDLL_T::ENGINE, "[DEMO] a signon block did not fit the write ring -- recording aborted\n");
		}
	}
	ReleaseSRWLockExclusive(&s_recLock);
}

void DemoRecord_OnPacketBegin(const uint8_t* pData, const int nSize)
{
	s_bPacketOpen = false;
	if (Demo_IsPlaying() || !pData || nSize <= 0 || static_cast<uint32_t>(nSize) > R5DEM_MAX_PACKET)
		return;
	DemoRecord_AutoStartAtConnect();
	s_packetScratch.assign(pData, pData + nSize);
	s_bPacketOpen = true;
}

void DemoRecord_OnPacketEnd(const bool bFull, const uint32_t nTick, const int nReliableEndBit)
{
	if (!s_bPacketOpen)
		return;
	s_bPacketOpen = false;

	const int nSignon = S21Bridge_GetClientSignonState();
	const uint8_t nSignonByte = static_cast<uint8_t>(nSignon < 0 ? 0 : nSignon);

	AcquireSRWLockExclusive(&s_recLock);

	if (nSignon >= 2)
	{
		if (nSignon < kSignonFull)
		{
			DemoRecord_HistoryPush(R5DemChunk_t::PACKET, nSignonByte, bFull ? R5DEM_KIND_FULL : 0, nTick,
				s_packetScratch.data(), s_packetScratch.size());
		}
		else if (s_bHistorySigned && nReliableEndBit > 0 && nReliableEndBit <= static_cast<int>(s_packetScratch.size()) * 8)
		{
			// Keep only the header + subchannel part; the leftover pad bits are
			// zeroed so they read as nothing.
			const size_t nBytes = static_cast<size_t>((nReliableEndBit + 7) / 8);
			std::vector<uint8_t> rel(s_packetScratch.begin(), s_packetScratch.begin() + nBytes);
			if (nReliableEndBit & 7)
				rel[nBytes - 1] &= static_cast<uint8_t>((1u << (nReliableEndBit & 7)) - 1u);
			DemoRecord_HistoryPush(R5DemChunk_t::PACKET, nSignonByte, 0, nTick, rel.data(), rel.size());
		}
	}

	if (s_pWriter && s_recState == DemoRecState_t::WAIT_FULL && bFull)
	{
		s_recState = DemoRecState_t::RECORDING;
		Msg(eDLL_T::ENGINE, "[DEMO] full snapshot received -- recording packets\n");
	}

	if (s_pWriter && s_recState == DemoRecState_t::RECORDING)
	{
		if (bFull && !s_nFirstFullTick && nSignon >= kSignonFull - 1)
			s_nFirstFullTick = nTick;
		if (s_pWriter->Append(R5DemChunk_t::PACKET, 0, nSignonByte, bFull ? R5DEM_KIND_FULL : 0, nTick,
			s_packetScratch.data(), static_cast<uint32_t>(s_packetScratch.size())))
		{
			++s_nPackets;
		}
		if (s_pWriter->HasFailed())
			DemoRecord_StopLocked("write failure", true);
	}
	ReleaseSRWLockExclusive(&s_recLock);
}

void DemoRecord_OnSignonState(const int nState)
{
	if (nState == 0)
	{
		if (DemoRecord_IsRecording())
			DemoRecord_Stop("disconnect");
		AcquireSRWLockExclusive(&s_recLock);
		DemoRecord_HistoryClear();
		ReleaseSRWLockExclusive(&s_recLock);
		s_bAutoStarted = false;
		return;
	}

}

// View angles at up to 60 Hz while packets are being recorded.
static void DemoRecord_WriteView(void)
{
	if (s_recState != DemoRecState_t::RECORDING)
		return;
	const ULONGLONG nNow = GetTickCount64();
	if (nNow - s_nLastViewMs < 16)
		return;

	R5DemView_s v;
	float ang[3];
	if (!DemoPlay_GetEngineViewAngles(ang))
		return;
	s_nLastViewMs = nNow;
	v.pitch = ang[0];
	v.yaw = ang[1];
	v.roll = ang[2];

	AcquireSRWLockExclusive(&s_recLock);
	if (s_pWriter && s_recState == DemoRecState_t::RECORDING)
		s_pWriter->Append(R5DemChunk_t::VIEW, 0, DemoRecord_Signon(), 0, S21Bridge_LastSnapshotTick(), &v, sizeof(v));
	ReleaseSRWLockExclusive(&s_recLock);
}

void DemoRecord_OnHostFrame(void)
{
	// The signon hook does not see every transition on the bridge, so the
	// frame loop owns the end-of-session stop as well.
	const int nState = S21Bridge_GetClientSignonState();
	const int nPrev = s_nFrameSignon;
	s_nFrameSignon = nState;
	if (nState < 2)
	{
		if (nPrev >= 2)
		{
			if (DemoRecord_IsRecording())
				DemoRecord_Stop("disconnect");
			AcquireSRWLockExclusive(&s_recLock);
			DemoRecord_HistoryClear();
			ReleaseSRWLockExclusive(&s_recLock);
		}
		s_bAutoStarted = false;
	}

	DemoRecord_WriteView();

	if (s_recState == DemoRecState_t::RECORDING && nState >= kSignonFull && v_CL_ForceFullUpdate
		&& demo_record_keyframe_s.GetFloat() > 0.0f)
	{
		const ULONGLONG nNow = GetTickCount64();
		if (!s_nLastKeyframeMs)
			s_nLastKeyframeMs = nNow;
		else if (nNow - s_nLastKeyframeMs >= static_cast<ULONGLONG>(demo_record_keyframe_s.GetFloat() * 1000.0f)
			&& DemoPlay_LocalObserverMode() > 0)
		{
			s_nLastKeyframeMs = nNow;
			DevMsg(eDLL_T::ENGINE, "[DEMO] keyframe requested while observing\n");
			v_CL_ForceFullUpdate();
		}
	}
	else if (s_recState == DemoRecState_t::OFF)
		s_nLastKeyframeMs = 0;

	// The playlist is known only after signon; note it as soon as it is, so a
	// recording cut off by a killed process still names its mode.
	if (!s_bModeMetaWritten && s_recState == DemoRecState_t::RECORDING
		&& S21Bridge_GetClientSignonState() >= kSignonFull)
	{
		const char* const pszPlaylist = S21Bridge_GetCurrentPlaylistName();
		if (pszPlaylist && pszPlaylist[0])
		{
			s_bModeMetaWritten = true;
			AcquireSRWLockExclusive(&s_recLock);
			DemoRecord_WriteMeta(false);
			ReleaseSRWLockExclusive(&s_recLock);
		}
	}

	if (s_recState != DemoRecState_t::WAIT_FULL || s_bFullRequested)
		return;

	const ULONGLONG nWaitMs = static_cast<ULONGLONG>(demo_record_full_wait_s.GetFloat() * 1000.0f);
	if (GetTickCount64() - s_waitFullStartMs < nWaitMs)
		return;

	s_bFullRequested = true;
	if (!v_CL_ForceFullUpdate)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] full-update request unavailable -- still waiting for a natural full snapshot\n");
		return;
	}
	Msg(eDLL_T::ENGINE, "[DEMO] no full snapshot after %.0f s -- requesting one\n",
		demo_record_full_wait_s.GetFloat());
	v_CL_ForceFullUpdate();
}

void DemoRecord_SetForceFullUpdateFn(void (*pfn)(void))
{
	v_CL_ForceFullUpdate = pfn;
}

//-----------------------------------------------------------------------------
// Console
//-----------------------------------------------------------------------------
static void DemoRecord_f(const CCommand& args)
{
	if (S21Bridge_GetClientSignonState() < 2)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] demo_record needs a connection\n");
		return;
	}
	DemoRecord_Start(args.ArgC() >= 2 ? args.Arg(1) : nullptr);
}

static ConCommand demo_record("demo_record", DemoRecord_f,
	"Record the current connection to platform/demos. Usage: demo_record [name]", FCVAR_RELEASE);

static void DemoStop_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	if (DemoPlay_Stop("demo_stop"))
		return;
	if (!DemoRecord_IsRecording())
	{
		Msg(eDLL_T::ENGINE, "[DEMO] nothing to stop\n");
		return;
	}
	DemoRecord_Stop("demo_stop");
}

static ConCommand demo_stop("demo_stop", DemoStop_f,
	"Stop the demo recording or playback in progress.", FCVAR_RELEASE);
