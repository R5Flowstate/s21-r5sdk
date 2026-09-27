//=============================================================================//
//
// Purpose: server demo recorder
//
// A recording starts mid-session (a duel begins long after the players
// connected), so every recorded slot keeps a history since connect: the
// datagrams of the connect and signon walk, the signon block, later
// DataBlocks, and the reliable subchannel part of every later datagram. A recording writes that
// history ahead of its live stream as prelude, then forces one full snapshot
// for the pov and records from it on.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "engine/net_chan.h"
#include "engine/client/client.h"
#include "engine/server/server.h"
#include "game/server/player.h"
#include "game/server/gameinterface.h"
#include "game/shared/usercmd.h"
#include "rtech/playlists/playlists.h"
#include "engine/shared/demo_writer.h"
#include "engine/shared/demo_reader.h"
#include "engine/shared/demo_json.h"
#include "demo_record_sv.h"
#include <atomic>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

static ConVar sv_demo_record("sv_demo_record", "0", FCVAR_RELEASE,
	"Server demo recording: 0 = off, 1 = matches scripts start (1v1 duels), "
	"2 = every human player from connect to disconnect.", true, 0.f, true, 2.f);
static ConVar sv_demo_dir("sv_demo_dir", "platform/demos", FCVAR_RELEASE,
	"Directory server demos are written to (inside the game directory).");
static ConVar sv_demo_keep_days("sv_demo_keep_days", "7", FCVAR_RELEASE,
	"Server demos older than this are deleted at the next map. 0 = keep.", true, 0.f, true, 3650.f);
static ConVar sv_demo_max_mb("sv_demo_max_mb", "2048", FCVAR_RELEASE,
	"Total size cap of the server demo directory; oldest files go first. 0 = no cap.", true, 0.f, true, 1048576.f);
static ConVar sv_demo_usercmds("sv_demo_usercmds", "1", FCVAR_RELEASE,
	"Record each pov's executed user commands.");
static ConVar sv_demo_history_mb("sv_demo_history_mb", "8", FCVAR_RELEASE,
	"Per-player history kept so a match recording can start mid-session.", true, 1.f, true, 64.f);
static ConVar sv_demo_diag("sv_demo_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log server demo recorder lifecycle and history accounting.");

static constexpr int      kSignonFull = 8;
static constexpr int      kMaxRecordings = 32;
static constexpr uint32_t kUserCmdTickSpan = 4;
static constexpr ptrdiff_t kClientLastSnapTick = 0x3D4; // -1 makes the next snapshot NoDelta

struct DemoSvHist_s
{
	R5DemChunk_t type;
	uint8_t      signon;
	uint8_t      kind;
	uint32_t     tick;
	std::vector<uint8_t> data;
};

struct DemoSvSlot_s
{
	SRWLOCK lock;
	std::deque<DemoSvHist_s> history;
	size_t nHistBytes;
	bool   bSigned;
	bool   bOverflow;

	int    nRec;        // recording index, -1 when not recorded
	int    nPov;
	bool   bAwaitFull;
	std::atomic<bool> bSnapFull;

	std::vector<R5DemUserCmd_s> cmds;
	R5DemUserCmdPrefix_s cmdPrefix;
	uint32_t nCmdFirstTick;
};

struct DemoSvRec_s
{
	bool         bUsed;
	bool         bPerPlayer;  // sv_demo_record 2
	CDemoWriter* pWriter;
	char         szMatchId[48];
	int          nPovCount;
	int          povSlot[R5DEM_MAX_POVS];
	uint32_t     povEh[R5DEM_MAX_POVS];
	int          povTeam[R5DEM_MAX_POVS];
	uint64_t     povNucleus[R5DEM_MAX_POVS];
	char         povName[R5DEM_MAX_POVS][72];
	uint32_t     nStartTick;
	uint32_t     nFirstFullTick;
	ULONGLONG    nStartMs;
};

static DemoSvSlot_s s_slots[MAX_PLAYERS];
static DemoSvRec_s  s_recs[kMaxRecordings];
static SRWLOCK      s_recsLock = SRWLOCK_INIT;
static bool         s_bSlotsInit = false;
static uint64_t     s_nLastPruneMs = 0;
static bool         s_bBudgetArmed = false;
// Bytes every server recording this level may still write, on top of what the
// prune left on disk.
static std::atomic<int64_t> s_nLevelBudget{ 0 };
static std::atomic<int> s_nRecording{ 0 };

static void DemoSv_EnsureInit(void)
{
	if (s_bSlotsInit)
		return;
	s_bSlotsInit = true;
	for (DemoSvSlot_s& s : s_slots)
	{
		InitializeSRWLock(&s.lock);
		s.nHistBytes = 0;
		s.bSigned = false;
		s.bOverflow = false;
		s.nRec = -1;
		s.nPov = -1;
		s.bAwaitFull = false;
		s.bSnapFull.store(false);
		memset(&s.cmdPrefix, 0, sizeof(s.cmdPrefix));
		s.nCmdFirstTick = 0;
	}
	memset(s_recs, 0, sizeof(s_recs));
}

bool DemoSv_Enabled(void)
{
	return sv_demo_record.GetInt() > 0;
}

bool DemoSv_AnyActive(void)
{
	return DemoSv_Enabled() || s_nRecording.load(std::memory_order_relaxed) > 0;
}

static uint32_t DemoSv_Tick(void)
{
	return g_pServer ? static_cast<uint32_t>(g_pServer->GetTick()) : 0;
}

static int DemoSv_SlotOfClient(const CClient* pClient)
{
	if (!g_pServer || !pClient)
		return -1;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		if (g_pServer->GetClient(i) == pClient)
			return i;
	}
	return -1;
}

static int DemoSv_SlotOfChan(const CNetChan* pChan)
{
	if (!g_pServer || !pChan)
		return -1;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		if (g_pServer->GetClient(i)->GetNetChan() == pChan)
			return i;
	}
	return -1;
}

static int DemoSv_SlotOfPlayer(const CPlayer* pPlayer)
{
	if (!pPlayer)
		return -1;
	const int nSlot = static_cast<int>(pPlayer->GetEdict()) - 1;
	return (nSlot >= 0 && nSlot < MAX_PLAYERS) ? nSlot : -1;
}

static void DemoSv_HistoryClear(DemoSvSlot_s& s)
{
	s.history.clear();
	s.nHistBytes = 0;
	s.bSigned = false;
	s.bOverflow = false;
}

static void DemoSv_HistoryPush(DemoSvSlot_s& s, const int nSlot, const R5DemChunk_t type, const uint8_t nSignon,
	const uint8_t nKind, const uint32_t nTick, const uint8_t* pData, const size_t nLen)
{
	if (s.bOverflow)
		return;
	const size_t nBudget = static_cast<size_t>(sv_demo_history_mb.GetInt()) * 1024u * 1024u;
	if (s.nHistBytes + nLen > nBudget)
	{
		s.bOverflow = true;
		Warning(eDLL_T::SERVER, "[DEMO-SV] slot %d history passed %d MB -- its matches cannot be recorded until it reconnects\n",
			nSlot, sv_demo_history_mb.GetInt());
		return;
	}
	DemoSvHist_s e;
	e.type = type;
	e.signon = nSignon;
	e.kind = nKind;
	e.tick = nTick;
	e.data.assign(pData, pData + nLen);
	s.nHistBytes += nLen;
	s.history.push_back(std::move(e));
}

//-----------------------------------------------------------------------------
// Datagram parse: bit position where the reliable subchannel section ends, -1
// when the datagram carries none. Same layout Hook_ProcessPacket reads.
//-----------------------------------------------------------------------------
namespace
{
	struct BitCursor_s
	{
		const uint8_t* p;
		int nBits;
		int pos;
		bool bad;

		uint32_t Read(const int n)
		{
			uint32_t v = 0;
			if (pos + n > nBits)
			{
				bad = true;
				pos = nBits;
				return 0;
			}
			for (int i = 0; i < n; ++i, ++pos)
				v |= static_cast<uint32_t>((p[pos >> 3] >> (pos & 7)) & 1u) << i;
			return v;
		}
		void Skip(const int n)
		{
			if (n < 0 || pos + n > nBits)
			{
				bad = true;
				pos = nBits;
				return;
			}
			pos += n;
		}
	};
}

static int DemoSv_ReliableEndBit(const uint8_t* pData, const int nBytes)
{
	if (!pData || nBytes < 9)
		return -1;
	const uint8_t flags = pData[8];
	if (!(flags & 0x01))
		return -1;

	BitCursor_s c = { pData, nBytes * 8, 72, false };
	if (flags & 0x10)
		c.Skip(8);

	if (c.Read(1))
	{
		c.Skip(32);           // nonce magic
		if (c.Read(1))
			c.Skip(32);       // nonce ack
		c.Skip(32 + 10);      // subchannel ack
	}

	c.Skip(32);                          // subchannel magic
	c.Skip(32);                          // subchannel seq
	const uint32_t reqId = c.Read(10);
	if (reqId == 0 && c.Read(1))
		c.Skip(32);

	for (int nSafety = 0; nSafety < 1024 && !c.bad; ++nSafety)
	{
		c.Skip(32);                      // entry seq
		int nChunk;
		if (c.Read(1))
		{
			const int nExpected = static_cast<int>(c.Read(19));
			nChunk = nExpected > 560 ? 560 : nExpected;
			if (c.Read(1))
				c.Skip(22);
		}
		else if (c.Read(1))
			nChunk = static_cast<int>(c.Read(10));
		else
			nChunk = 560;
		c.Skip(nChunk * 8);
		if (!c.Read(1))
			break;
	}
	return c.bad ? -1 : c.pos;
}

//-----------------------------------------------------------------------------
// Recording lifecycle
//-----------------------------------------------------------------------------
static void DemoSv_WriteMeta(DemoSvRec_s& r, const char* pszOutcome)
{
	if (!r.pWriter)
		return;

	char szMatch[96], szMap[96], szPlaylist[96];
	DemoJson::Escape(r.szMatchId, szMatch, sizeof(szMatch));
	DemoJson::Escape(g_pServer ? g_pServer->GetMapName() : "", szMap, sizeof(szMap));
	DemoJson::Escape(Playlists_GetCurrentName(), szPlaylist, sizeof(szPlaylist));

	std::string json;
	json.reserve(2048);
	char buf[512];
	snprintf(buf, sizeof(buf),
		"{\"protocol\":%u,\"recorder\":\"server\",\"matchId\":\"%s\",\"playlist\":\"%s\",\"map\":\"%s\","
		"\"mode\":\"%s\",\"serverBuild\":\"%08X\",\"povs\":[",
		R5DEM_PROTOCOL, szMatch, szPlaylist, szMap, szPlaylist,
		g_SDKDll.GetNTHeaders()->FileHeader.TimeDateStamp);
	json += buf;
	for (int i = 0; i < r.nPovCount; ++i)
	{
		char szName[160];
		DemoJson::Escape(r.povName[i], szName, sizeof(szName));
		snprintf(buf, sizeof(buf), "%s{\"id\":%d,\"slot\":%d,\"name\":\"%s\",\"nucleus\":\"%llu\",\"team\":%d,\"eh\":%u}",
			i ? "," : "", i, r.povSlot[i], szName, static_cast<unsigned long long>(r.povNucleus[i]),
			r.povTeam[i], r.povEh[i]);
		json += buf;
	}
	json += "]";
	if (pszOutcome && pszOutcome[0])
	{
		json += ",\"outcome\":";
		json += pszOutcome;
	}
	if (r.nFirstFullTick)
	{
		snprintf(buf, sizeof(buf), ",\"firstFullTick\":%u", r.nFirstFullTick);
		json += buf;
	}
	snprintf(buf, sizeof(buf), ",\"droppedChunks\":%u}", r.pWriter->GetDropped());
	json += buf;

	if (json.size() <= R5DEM_MAX_JSON)
		r.pWriter->Append(R5DemChunk_t::META, R5DEM_POV_ALL, 0, 0, DemoSv_Tick(), json.data(), static_cast<uint32_t>(json.size()));
}

static void DemoSv_FlushUserCmdsLocked(DemoSvSlot_s& s, DemoSvRec_s& r)
{
	if (s.cmds.empty() || !r.pWriter)
		return;
	std::vector<uint8_t> payload(sizeof(R5DemUserCmdPrefix_s) + s.cmds.size() * sizeof(R5DemUserCmd_s));
	s.cmdPrefix.count = static_cast<uint16_t>(s.cmds.size());
	memcpy(payload.data(), &s.cmdPrefix, sizeof(s.cmdPrefix));
	memcpy(payload.data() + sizeof(s.cmdPrefix), s.cmds.data(), s.cmds.size() * sizeof(R5DemUserCmd_s));
	r.pWriter->Append(R5DemChunk_t::USERCMD, static_cast<uint8_t>(s.nPov), kSignonFull, 0, s.nCmdFirstTick,
		payload.data(), static_cast<uint32_t>(payload.size()));
	s.cmds.clear();
}

// Caller holds s_recsLock exclusive.
static void DemoSv_CloseRecLocked(const int nRec, const char* pszOutcome, const bool bKeep)
{
	DemoSvRec_s& r = s_recs[nRec];
	if (!r.bUsed)
		return;

	for (int i = 0; i < r.nPovCount; ++i)
	{
		const int nSlot = r.povSlot[i];
		if (nSlot < 0 || nSlot >= MAX_PLAYERS)
			continue;
		DemoSvSlot_s& s = s_slots[nSlot];
		AcquireSRWLockExclusive(&s.lock);
		if (s.nRec == nRec)
		{
			DemoSv_FlushUserCmdsLocked(s, r);
			s.nRec = -1;
			s.nPov = -1;
			s.bAwaitFull = false;
		}
		ReleaseSRWLockExclusive(&s.lock);
	}

	const uint64_t nBytes = r.pWriter ? r.pWriter->GetBytesWritten() : 0;
	if (r.pWriter)
	{
		if (bKeep)
			DemoSv_WriteMeta(r, pszOutcome);
		r.pWriter->Close(bKeep);
		delete r.pWriter;
		r.pWriter = nullptr;
	}

	Msg(eDLL_T::SERVER, "[DEMO-SV] %s '%s' (%d pov, %.1f s, %llu KB)\n",
		bKeep ? "saved" : "discarded", r.szMatchId, r.nPovCount,
		(GetTickCount64() - r.nStartMs) / 1000.0, static_cast<unsigned long long>(nBytes / 1024));

	memset(&r, 0, sizeof(r));
	s_nRecording.fetch_sub(1, std::memory_order_relaxed);
}

static int DemoSv_FindRecLocked(const char* pszMatchId)
{
	for (int i = 0; i < kMaxRecordings; ++i)
	{
		if (s_recs[i].bUsed && !strcmp(s_recs[i].szMatchId, pszMatchId))
			return i;
	}
	return -1;
}

static bool DemoSv_IsValidMatchId(const char* psz)
{
	return R5Dem_IsValidName(psz) && strlen(psz) <= 40;
}

// Every open prunes, at most once per interval: a directory scan runs on the frame thread.
static void DemoSv_Prune(void)
{
	static constexpr uint64_t kPruneIntervalMs = 30000;
	const uint64_t nNow = GetTickCount64();
	if (s_nLastPruneMs && nNow - s_nLastPruneMs < kPruneIntervalMs)
		return;
	s_nLastPruneMs = nNow;
	Demo_Prune(sv_demo_dir.GetString(), sv_demo_keep_days.GetInt(), sv_demo_max_mb.GetInt());
}

// Caller holds s_recsLock exclusive. povSlots are validated, human, signed.
static bool DemoSv_OpenRecLocked(const char* pszMatchId, const int* pSlots, CPlayer* const* ppPovs,
	const int nPovs, const bool bPerPlayer)
{
	int nRec = -1;
	for (int i = 0; i < kMaxRecordings; ++i)
	{
		if (!s_recs[i].bUsed)
		{
			nRec = i;
			break;
		}
	}
	if (nRec < 0)
	{
		Warning(eDLL_T::SERVER, "[DEMO-SV] %d recordings already open -- '%s' not recorded\n", kMaxRecordings, pszMatchId);
		return false;
	}

	const char* const pszRoot = sv_demo_dir.GetString();
	if (!Demo_IsSafeRoot(pszRoot))
	{
		Warning(eDLL_T::SERVER, "[DEMO-SV] sv_demo_dir '%s' is not a safe path\n", pszRoot);
		return false;
	}
	DemoSv_Prune();

	const int nMaxMb = sv_demo_max_mb.GetInt();
	if (!s_bBudgetArmed)
	{
		s_bBudgetArmed = true;
		s_nLevelBudget.store(static_cast<int64_t>(nMaxMb) * 1024 * 1024, std::memory_order_relaxed);
	}
	if (nMaxMb > 0 && s_nLevelBudget.load(std::memory_order_relaxed) <= 0)
	{
		Warning(eDLL_T::SERVER, "[DEMO-SV] sv_demo_max_mb budget spent this level -- '%s' not recorded\n", pszMatchId);
		return false;
	}

	char szMap[40];
	Demo_SanitizeName(g_pServer ? g_pServer->GetMapName() : "map", szMap, sizeof(szMap));
	char szBase[96];
	snprintf(szBase, sizeof(szBase), "%s_%s", pszMatchId, szMap);
	szBase[64] = '\0';
	char szPath[260];
	if (!Demo_BuildRecordPath(pszRoot, szBase, szPath, sizeof(szPath)))
		return false;

	R5DemHeader_s hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.flags = R5DEM_FLAG_SERVER | (nPovs > 1 ? R5DEM_FLAG_MULTIPOV : 0u)
		| (sv_demo_usercmds.GetBool() ? R5DEM_FLAG_USERCMDS : 0u);
	hdr.tickIntervalUs = (gpGlobals && gpGlobals->tickInterval > 0.0f)
		? static_cast<uint32_t>(gpGlobals->tickInterval * 1000000.0f + 0.5f) : 50000u;
	hdr.povCount = static_cast<uint32_t>(nPovs);
	hdr.startUnixMs = static_cast<uint64_t>(time(nullptr)) * 1000ull;
	strncpy_s(hdr.map, g_pServer ? g_pServer->GetMapName() : "", _TRUNCATE);
	strncpy_s(hdr.mode, Playlists_GetCurrentName(), _TRUNCATE);

	CDemoWriter* const pWriter = new CDemoWriter();
	if (nMaxMb > 0)
		pWriter->SetByteBudget(0, &s_nLevelBudget);
	if (!pWriter->Open(szPath, hdr))
	{
		delete pWriter;
		return false;
	}

	DemoSvRec_s& r = s_recs[nRec];
	memset(&r, 0, sizeof(r));
	r.bUsed = true;
	r.bPerPlayer = bPerPlayer;
	r.pWriter = pWriter;
	strncpy_s(r.szMatchId, pszMatchId, _TRUNCATE);
	r.nPovCount = nPovs;
	r.nStartTick = DemoSv_Tick();
	r.nStartMs = GetTickCount64();
	s_nRecording.fetch_add(1, std::memory_order_relaxed);

	for (int i = 0; i < nPovs; ++i)
	{
		CClient* const pClient = g_pServer->GetClient(pSlots[i]);
		r.povSlot[i] = pSlots[i];
		r.povEh[i] = ppPovs && ppPovs[i]
			? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(ppPovs[i]) + 0x8) // m_RefEHandle
			: 0xFFFFFFFFu;
		r.povTeam[i] = static_cast<int>(pClient->GetTeamNum());
		r.povNucleus[i] = static_cast<uint64_t>(pClient->GetPlatformUserId());
		strncpy_s(r.povName[i], pClient->GetServerName(), _TRUNCATE);
	}
	DemoSv_WriteMeta(r, nullptr);

	for (int i = 0; i < nPovs; ++i)
	{
		const int nSlot = pSlots[i];
		DemoSvSlot_s& s = s_slots[nSlot];
		CClient* const pClient = g_pServer->GetClient(nSlot);

		AcquireSRWLockExclusive(&s.lock);
		for (const DemoSvHist_s& e : s.history)
		{
			const uint8_t nKind = (e.type == R5DemChunk_t::PACKET) ? static_cast<uint8_t>(e.kind | R5DEM_KIND_PRELUDE) : e.kind;
			pWriter->Append(e.type, static_cast<uint8_t>(i), e.signon, nKind, e.tick, e.data.data(),
				static_cast<uint32_t>(e.data.size()));
		}
		s.nRec = nRec;
		s.nPov = i;
		s.cmds.clear();
		// A pov still walking signon records from here; one already in game
		// is keyed by a forced full snapshot.
		s.bAwaitFull = pClient->GetSignonState() >= SIGNONSTATE::SIGNONSTATE_FULL;
		if (s.bAwaitFull)
			*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(pClient) + kClientLastSnapTick) = -1;
		if (sv_demo_diag.GetBool())
			Msg(eDLL_T::SERVER, "[DEMO-SV] '%s' pov %d slot %d: %zu history chunk(s), %zu KB, %s\n",
				pszMatchId, i, nSlot, s.history.size(), s.nHistBytes / 1024,
				s.bAwaitFull ? "full snapshot requested" : "still in signon");
		ReleaseSRWLockExclusive(&s.lock);
	}

	if (pWriter->GetDropped())
	{
		Warning(eDLL_T::SERVER, "[DEMO-SV] '%s': history did not fit the write ring -- recording discarded\n", pszMatchId);
		DemoSv_CloseRecLocked(nRec, nullptr, false);
		return false;
	}

	Msg(eDLL_T::SERVER, "[DEMO-SV] recording '%s' -> '%s' (%d pov)\n", pszMatchId, szPath, nPovs);
	return true;
}

bool DemoSv_Start(const char* pszMatchId, CPlayer* const* ppPovs, const int nPovs)
{
	DemoSv_EnsureInit();
	if (!DemoSv_Enabled() || !g_pServer)
		return false;
	if (!DemoSv_IsValidMatchId(pszMatchId))
	{
		Warning(eDLL_T::SERVER, "[DEMO-SV] Demo_ServerStart: match id must be [a-zA-Z0-9_-]{1,40}\n");
		return false;
	}
	if (nPovs < 1 || nPovs > R5DEM_MAX_POVS || !ppPovs)
	{
		Warning(eDLL_T::SERVER, "[DEMO-SV] Demo_ServerStart: 1..%d povs\n", R5DEM_MAX_POVS);
		return false;
	}

	int slots[R5DEM_MAX_POVS];
	for (int i = 0; i < nPovs; ++i)
	{
		const int nSlot = DemoSv_SlotOfPlayer(ppPovs[i]);
		CClient* const pClient = nSlot >= 0 ? g_pServer->GetClient(nSlot) : nullptr;
		if (!pClient || !pClient->IsHumanPlayer() || !pClient->GetNetChan())
		{
			Warning(eDLL_T::SERVER, "[DEMO-SV] '%s': pov %d is not a connected human player\n", pszMatchId, i);
			return false;
		}
		for (int j = 0; j < i; ++j)
		{
			if (slots[j] == nSlot)
			{
				Warning(eDLL_T::SERVER, "[DEMO-SV] '%s': the same player twice\n", pszMatchId);
				return false;
			}
		}
		slots[i] = nSlot;
	}

	AcquireSRWLockExclusive(&s_recsLock);
	if (DemoSv_FindRecLocked(pszMatchId) >= 0)
	{
		ReleaseSRWLockExclusive(&s_recsLock);
		return true;
	}
	for (int i = 0; i < nPovs; ++i)
	{
		DemoSvSlot_s& s = s_slots[slots[i]];
		AcquireSRWLockShared(&s.lock);
		const bool bBusy = s.nRec >= 0 && !s_recs[s.nRec].bPerPlayer;
		const bool bUsable = s.bSigned && !s.bOverflow;
		ReleaseSRWLockShared(&s.lock);
		if (bBusy || !bUsable)
		{
			ReleaseSRWLockExclusive(&s_recsLock);
			Warning(eDLL_T::SERVER, "[DEMO-SV] '%s': pov %d (%s) %s\n", pszMatchId, i,
				g_pServer->GetClient(slots[i])->GetServerName(),
				bBusy ? "is already being recorded" : "joined before recording was enabled or its history overflowed");
			return false;
		}
	}
	// A per-player recording yields its slot to the match.
	for (int i = 0; i < nPovs; ++i)
	{
		const int nOther = s_slots[slots[i]].nRec;
		if (nOther >= 0)
			DemoSv_CloseRecLocked(nOther, "{\"reason\":\"match_started\"}", true);
	}
	const bool bOk = DemoSv_OpenRecLocked(pszMatchId, slots, ppPovs, nPovs, false);
	ReleaseSRWLockExclusive(&s_recsLock);
	return bOk;
}

bool DemoSv_Stop(const char* pszMatchId, CPlayer* pWinner, const char* pszReason)
{
	if (!s_bSlotsInit || !DemoSv_IsValidMatchId(pszMatchId))
		return false;

	AcquireSRWLockExclusive(&s_recsLock);
	const int nRec = DemoSv_FindRecLocked(pszMatchId);
	if (nRec < 0)
	{
		ReleaseSRWLockExclusive(&s_recsLock);
		return false;
	}

	DemoSvRec_s& r = s_recs[nRec];
	int nWinner = -1;
	const int nWinSlot = DemoSv_SlotOfPlayer(pWinner);
	for (int i = 0; i < r.nPovCount && nWinSlot >= 0; ++i)
	{
		if (r.povSlot[i] == nWinSlot)
			nWinner = i;
	}
	char szReason[48];
	Demo_SanitizeName(pszReason && pszReason[0] ? pszReason : "unknown", szReason, 40);
	char szOutcome[160];
	snprintf(szOutcome, sizeof(szOutcome), "{\"winner\":%d,\"reason\":\"%s\",\"durationMs\":%llu}",
		nWinner, szReason, static_cast<unsigned long long>(GetTickCount64() - r.nStartMs));
	DemoSv_CloseRecLocked(nRec, szOutcome, true);
	ReleaseSRWLockExclusive(&s_recsLock);
	return true;
}

bool DemoSv_Event(const char* pszMatchId, const char* pszType, CPlayer* pAttacker,
	CPlayer* pVictim, const char* pszWeapon, const float flDamage)
{
	if (!s_bSlotsInit || !DemoSv_IsValidMatchId(pszMatchId) || !pszType)
		return false;

	char szType[16];
	Demo_SanitizeName(pszType, szType, sizeof(szType));
	char szWeapon[48];
	Demo_SanitizeName(pszWeapon ? pszWeapon : "", szWeapon, sizeof(szWeapon));

	AcquireSRWLockShared(&s_recsLock);
	const int nRec = DemoSv_FindRecLocked(pszMatchId);
	if (nRec < 0 || !s_recs[nRec].pWriter)
	{
		ReleaseSRWLockShared(&s_recsLock);
		return false;
	}
	const DemoSvRec_s& r = s_recs[nRec];
	auto povOf = [&](CPlayer* p) -> int
	{
		const int nSlot = DemoSv_SlotOfPlayer(p);
		for (int i = 0; i < r.nPovCount && nSlot >= 0; ++i)
		{
			if (r.povSlot[i] == nSlot)
				return i;
		}
		return -1;
	};
	const uint32_t nTick = DemoSv_Tick();
	const float flDmg = (flDamage == flDamage && flDamage >= 0.0f && flDamage < 100000.0f) ? flDamage : 0.0f;
	char szJson[256];
	const int n = snprintf(szJson, sizeof(szJson), "{\"t\":\"%s\",\"tick\":%u,\"a\":%d,\"v\":%d,\"w\":\"%s\",\"d\":%.1f}",
		szType, nTick, povOf(pAttacker), povOf(pVictim), szWeapon, flDmg);
	const bool bOk = n > 0 && r.pWriter->Append(R5DemChunk_t::EVENT, R5DEM_POV_ALL, kSignonFull, 0, nTick,
		szJson, static_cast<uint32_t>(n));
	ReleaseSRWLockShared(&s_recsLock);
	return bOk;
}

bool DemoSv_IsRecording(const char* pszMatchId)
{
	if (!s_bSlotsInit || !DemoSv_IsValidMatchId(pszMatchId))
		return false;
	AcquireSRWLockShared(&s_recsLock);
	const bool b = DemoSv_FindRecLocked(pszMatchId) >= 0;
	ReleaseSRWLockShared(&s_recsLock);
	return b;
}

//-----------------------------------------------------------------------------
// Capture seams
//-----------------------------------------------------------------------------
void DemoSv_OnDataBlock(const CClient* pClient, const uint8_t* pData, const int nSize)
{
	if (!DemoSv_AnyActive() || !pClient || !pData || nSize <= 0 || static_cast<uint32_t>(nSize) > R5DEM_MAX_BLOCK)
		return;
	DemoSv_EnsureInit();

	const int nSlot = DemoSv_SlotOfClient(pClient);
	if (nSlot < 0 || pClient->IsFakeClient())
		return;

	const int nSignon = static_cast<int>(pClient->GetSignonState());
	const uint8_t nSignonByte = static_cast<uint8_t>(nSignon < 0 ? 0 : nSignon);
	const bool bSignon = nSignon < kSignonFull;
	const R5DemChunk_t type = bSignon ? R5DemChunk_t::SIGNON : R5DemChunk_t::RELIABLE;
	const uint32_t nTick = DemoSv_Tick();

	DemoSvSlot_s& s = s_slots[nSlot];
	AcquireSRWLockExclusive(&s.lock);
	// A second signon block is a new level; the datagrams since connect stay
	// ahead of the first one.
	if (bSignon && s.bSigned)
		DemoSv_HistoryClear(s);
	DemoSv_HistoryPush(s, nSlot, type, nSignonByte, 0, nTick, pData, static_cast<size_t>(nSize));
	if (bSignon)
		s.bSigned = true;
	const int nRec = s.nRec;
	const int nPov = s.nPov;
	ReleaseSRWLockExclusive(&s.lock);

	if (nRec >= 0)
	{
		AcquireSRWLockShared(&s_recsLock);
		if (s_recs[nRec].bUsed && s_recs[nRec].pWriter && s.nRec == nRec && s.nPov == nPov)
			s_recs[nRec].pWriter->Append(type, static_cast<uint8_t>(nPov), nSignonByte, 0, nTick,
				pData, static_cast<uint32_t>(nSize));
		ReleaseSRWLockShared(&s_recsLock);
	}
	else if (bSignon && sv_demo_record.GetInt() == 2)
	{
		// Per-player recording from connect: the history is exactly this block.
		char szMatch[48];
		snprintf(szMatch, sizeof(szMatch), "%llu_p%d", static_cast<unsigned long long>(time(nullptr)), nSlot);
		const int slots[1] = { nSlot };
		AcquireSRWLockExclusive(&s_recsLock);
		DemoSv_OpenRecLocked(szMatch, slots, nullptr, 1, true);
		ReleaseSRWLockExclusive(&s_recsLock);
	}
}

void DemoSv_OnSnapshotBegin(const int nSlot, const bool bFull)
{
	if (nSlot >= 0 && nSlot < MAX_PLAYERS && s_bSlotsInit)
		s_slots[nSlot].bSnapFull.store(bFull, std::memory_order_relaxed);
}

void DemoSv_OnSnapshotEnd(const int nSlot)
{
	if (nSlot >= 0 && nSlot < MAX_PLAYERS && s_bSlotsInit)
		s_slots[nSlot].bSnapFull.store(false, std::memory_order_relaxed);
}

void DemoSv_OnDatagram(const CNetChan* pChan, const uint8_t* pData, const int nBytes)
{
	if (!s_bSlotsInit || !DemoSv_AnyActive() || !pData || nBytes < 9 || static_cast<uint32_t>(nBytes) > R5DEM_MAX_PACKET)
		return;

	const int nSlot = DemoSv_SlotOfChan(pChan);
	if (nSlot < 0)
		return;
	const CClient* const pClient = g_pServer->GetClient(nSlot);
	if (pClient->IsFakeClient())
		return;

	const int nSignon = static_cast<int>(pClient->GetSignonState());
	const uint8_t nSignonByte = static_cast<uint8_t>(nSignon < 0 ? 0 : nSignon);
	DemoSvSlot_s& s = s_slots[nSlot];
	const bool bFull = s.bSnapFull.exchange(false, std::memory_order_relaxed);
	const uint32_t nTick = DemoSv_Tick();

	AcquireSRWLockExclusive(&s.lock);
	if (nSignon < kSignonFull)
	{
		DemoSv_HistoryPush(s, nSlot, R5DemChunk_t::PACKET, nSignonByte, bFull ? R5DEM_KIND_FULL : 0,
			nTick, pData, static_cast<size_t>(nBytes));
	}
	else if (s.bSigned)
	{
		// In game only the reliable part matters; old snapshots would decode
		// against baselines the player never has.
		const int nEnd = DemoSv_ReliableEndBit(pData, nBytes);
		if (nEnd > 0 && nEnd <= nBytes * 8)
		{
			const size_t nRel = static_cast<size_t>((nEnd + 7) / 8);
			std::vector<uint8_t> rel(pData, pData + nRel);
			if (nEnd & 7)
				rel[nRel - 1] &= static_cast<uint8_t>((1u << (nEnd & 7)) - 1u);
			DemoSv_HistoryPush(s, nSlot, R5DemChunk_t::PACKET, nSignonByte, 0, nTick, rel.data(), rel.size());
		}
	}

	const int nRec = s.nRec;
	const int nPov = s.nPov;
	bool bRecord = nRec >= 0;
	if (bRecord && s.bAwaitFull)
	{
		if (bFull)
			s.bAwaitFull = false;
		else
			bRecord = false;
	}
	ReleaseSRWLockExclusive(&s.lock);

	if (!bRecord)
		return;

	AcquireSRWLockShared(&s_recsLock);
	DemoSvRec_s& r = s_recs[nRec];
	if (r.bUsed && r.pWriter && s.nRec == nRec && s.nPov == nPov)
	{
		if (bFull && !r.nFirstFullTick && nPov == 0)
			r.nFirstFullTick = nTick;
		r.pWriter->Append(R5DemChunk_t::PACKET, static_cast<uint8_t>(nPov), nSignonByte,
			bFull ? R5DEM_KIND_FULL : 0, nTick, pData, static_cast<uint32_t>(nBytes));
	}
	ReleaseSRWLockShared(&s_recsLock);
}

void DemoSv_OnUserCmd(CPlayer* pPlayer, const CUserCmd* pCmd)
{
	if (!s_bSlotsInit || !pPlayer || !pCmd || s_nRecording.load(std::memory_order_relaxed) <= 0
		|| !sv_demo_usercmds.GetBool())
		return;

	const int nSlot = DemoSv_SlotOfPlayer(pPlayer);
	if (nSlot < 0)
		return;
	DemoSvSlot_s& s = s_slots[nSlot];

	AcquireSRWLockExclusive(&s.lock);
	if (s.nRec < 0 || s.bAwaitFull)
	{
		ReleaseSRWLockExclusive(&s.lock);
		return;
	}

	if (s.cmds.empty())
	{
		const Vector3D& org = pPlayer->Diag_AbsOrigin();
		s.cmdPrefix.origin[0] = org.x;
		s.cmdPrefix.origin[1] = org.y;
		s.cmdPrefix.origin[2] = org.z;
		QAngle ang;
		pPlayer->EyeAngles(&ang);
		s.cmdPrefix.angles[0] = ang.x;
		s.cmdPrefix.angles[1] = ang.y;
		s.cmdPrefix.angles[2] = ang.z;
		s.cmdPrefix.reserved = 0;
		s.nCmdFirstTick = static_cast<uint32_t>(pCmd->tick_count);
	}

	R5DemUserCmd_s c;
	c.commandNumber = static_cast<uint32_t>(pCmd->command_number);
	c.tickCount = static_cast<uint32_t>(pCmd->tick_count);
	c.frameTime = pCmd->frametime;
	c.pitch = pCmd->viewangles.x;
	c.yaw = pCmd->viewangles.y;
	c.forwardmove = pCmd->forwardmove;
	c.sidemove = pCmd->sidemove;
	c.upmove = pCmd->upmove;
	c.buttons = static_cast<uint32_t>(pCmd->buttons);
	c.weaponSelectSlot = pCmd->weaponindex;
	c.impulse = pCmd->impulse;
	c.reserved = 0;
	s.cmds.push_back(c);

	const bool bFlush = s.cmds.size() >= static_cast<size_t>(R5DEM_USERCMDS_PER_CHUNK)
		|| static_cast<uint32_t>(pCmd->tick_count) - s.nCmdFirstTick >= kUserCmdTickSpan;
	if (!bFlush)
	{
		ReleaseSRWLockExclusive(&s.lock);
		return;
	}

	const int nRec = s.nRec;
	const int nPov = s.nPov;
	const uint32_t nFirstTick = s.nCmdFirstTick;
	R5DemUserCmdPrefix_s prefix = s.cmdPrefix;
	prefix.count = static_cast<uint16_t>(s.cmds.size());
	std::vector<uint8_t> payload(sizeof(prefix) + s.cmds.size() * sizeof(R5DemUserCmd_s));
	memcpy(payload.data(), &prefix, sizeof(prefix));
	memcpy(payload.data() + sizeof(prefix), s.cmds.data(), s.cmds.size() * sizeof(R5DemUserCmd_s));
	s.cmds.clear();
	ReleaseSRWLockExclusive(&s.lock);

	// Lock order is recordings -> slot everywhere else.
	AcquireSRWLockShared(&s_recsLock);
	DemoSvRec_s& r = s_recs[nRec];
	if (r.bUsed && r.pWriter && s.nRec == nRec && s.nPov == nPov)
		r.pWriter->Append(R5DemChunk_t::USERCMD, static_cast<uint8_t>(nPov), kSignonFull, 0, nFirstTick,
			payload.data(), static_cast<uint32_t>(payload.size()));
	ReleaseSRWLockShared(&s_recsLock);
}

void DemoSv_OnClientCleared(const CClient* pClient)
{
	if (!s_bSlotsInit)
		return;
	const int nSlot = DemoSv_SlotOfClient(pClient);
	if (nSlot < 0)
		return;

	DemoSvSlot_s& s = s_slots[nSlot];
	AcquireSRWLockExclusive(&s_recsLock);
	AcquireSRWLockExclusive(&s.lock);
	const int nRec = s.nRec;
	DemoSv_HistoryClear(s);
	s.cmds.clear();
	ReleaseSRWLockExclusive(&s.lock);

	if (nRec >= 0 && s_recs[nRec].bUsed)
	{
		if (s_recs[nRec].bPerPlayer)
		{
			DemoSv_CloseRecLocked(nRec, "{\"reason\":\"disconnect\"}", true);
		}
		else
		{
			// The match keeps its other povs; the script closes it.
			AcquireSRWLockExclusive(&s.lock);
			s.nRec = -1;
			s.nPov = -1;
			s.bAwaitFull = false;
			ReleaseSRWLockExclusive(&s.lock);
		}
	}
	ReleaseSRWLockExclusive(&s_recsLock);
}

void DemoSv_LevelShutdown(void)
{
	if (!s_bSlotsInit)
		return;

	AcquireSRWLockExclusive(&s_recsLock);
	for (int i = 0; i < kMaxRecordings; ++i)
	{
		if (s_recs[i].bUsed)
			DemoSv_CloseRecLocked(i, "{\"winner\":-1,\"reason\":\"level_shutdown\"}", true);
	}
	ReleaseSRWLockExclusive(&s_recsLock);

	for (DemoSvSlot_s& s : s_slots)
	{
		AcquireSRWLockExclusive(&s.lock);
		DemoSv_HistoryClear(s);
		s.cmds.clear();
		ReleaseSRWLockExclusive(&s.lock);
	}
	s_nLastPruneMs = 0;
	s_bBudgetArmed = false;
}

//-----------------------------------------------------------------------------
// Console
//-----------------------------------------------------------------------------
static void DemoSvStatus_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	if (!s_bSlotsInit)
	{
		Msg(eDLL_T::SERVER, "[DEMO-SV] idle (sv_demo_record %d)\n", sv_demo_record.GetInt());
		return;
	}
	AcquireSRWLockShared(&s_recsLock);
	int n = 0;
	for (const DemoSvRec_s& r : s_recs)
	{
		if (!r.bUsed)
			continue;
		++n;
		Msg(eDLL_T::SERVER, "  %-40s %d pov  %.1f s  %llu KB%s\n", r.szMatchId, r.nPovCount,
			(GetTickCount64() - r.nStartMs) / 1000.0,
			static_cast<unsigned long long>(r.pWriter ? r.pWriter->GetBytesWritten() / 1024 : 0),
			r.bPerPlayer ? "  (per-player)" : "");
	}
	ReleaseSRWLockShared(&s_recsLock);

	size_t nHist = 0;
	int nSigned = 0;
	for (DemoSvSlot_s& s : s_slots)
	{
		AcquireSRWLockShared(&s.lock);
		nHist += s.nHistBytes;
		nSigned += s.bSigned ? 1 : 0;
		ReleaseSRWLockShared(&s.lock);
	}
	Msg(eDLL_T::SERVER, "[DEMO-SV] %d recording(s); history %d slot(s), %zu KB; sv_demo_record %d\n",
		n, nSigned, nHist / 1024, sv_demo_record.GetInt());
}
static ConCommand sv_demo_status("sv_demo_status", DemoSvStatus_f,
	"List open server demo recordings and history memory.", FCVAR_RELEASE);
