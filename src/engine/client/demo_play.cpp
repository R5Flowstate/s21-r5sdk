//=============================================================================//
//
// Purpose: client demo player
//
// The bridge already fakes the server for the engine: it rewrites the S3
// handshake, drives native CONNECTED and injects every datagram. Playback puts
// the demo file at the head of that path. A synthetic S2C_CHALLENGE and
// CONNACCEPT walk the handshake; SIGNON / RELIABLE chunks go through the same
// DataBlock hand-off the fragment reassembler uses; PACKET chunks are injected
// as received datagrams on a demo clock.
//
//=============================================================================//
#include "core/stdafx.h"
#include "tier0/dbg.h"
#include "tier1/cvar.h"
#include "tier1/convar.h"
#include "engine/cmd.h"
#include "engine/cmodel_bsp.h"
#include "engine/client/net_bridge_internal.h"
#include "engine/shared/demo_reader.h"
#include "engine/shared/demo_json.h"
#include "engine/client/demo_bridge.h"
#include "engine/client/demo_record.h"
#include "engine/client/demo_play.h"
#include "tier0/commandline.h"
#include "engine/sys_mainwind.h"
#include "windows/input.h"
#include "inputsystem/inputsystem.h"
#include "game/client/pred_authority.h"
#include "vscript/languages/squirrel_re/vsquirrel.h"
#include <algorithm>

static ConVar demo_timescale("demo_timescale", "1", FCVAR_RELEASE,
	"Demo playback speed; the world and the packet feed slow down together.", true, 0.05f, true, 4.f);
static ConVar demo_feed_max_per_frame("demo_feed_max_per_frame", "8", FCVAR_RELEASE,
	"Demo packets fed per frame during normal playback.", true, 1.f, true, 64.f);
// The engine keeps every snapshot newer than its interpolation base until the
// next frame transitions past it; past ~30 per frame its delta store overflows
// and a snapshot fails to parse.
static ConVar demo_ff_packets_per_frame("demo_ff_packets_per_frame", "24", FCVAR_RELEASE,
	"Most demo packets fed per frame while seeking.", true, 1.f, true, 32.f);
static ConVar demo_ff_budget_ms("demo_ff_budget_ms", "40", FCVAR_RELEASE,
	"Milliseconds of packet decoding per frame while seeking.", true, 4.f, true, 250.f);
static ConVar demo_seek_restart("demo_seek_restart", "0", FCVAR_RELEASE,
	"1 = rewind by reconnecting to the demo instead of restarting from the nearest full snapshot.");
static ConVar demo_freecam_speed("demo_freecam_speed", "400", FCVAR_RELEASE,
	"Free camera speed in units per second (Shift = x3).", true, 10.f, true, 5000.f);
static ConVar demo_hud("demo_hud", "1", FCVAR_RELEASE,
	"Demo HUD: 0 = stock, 1 = coaching overlay, 2 = none.", true, 0.f, true, 2.f);
static ConVar demo_diag("demo_diag", "0", FCVAR_DEVELOPMENTONLY,
	"Log every chunk the demo player feeds.");
static ConVar demo_fuzz_dir("demo_fuzz_dir", "", FCVAR_DEVELOPMENTONLY,
	"Directory of .r5dem files to play in turn for 5 s each (fuzz harness). Cleared when done.");
static ConVar demo_focus_diag("demo_focus_diag", "0", FCVAR_DEVELOPMENTONLY,
	"While a replay plays, log each change of the foreground window, the engine's active-app flag and "
	"the cursor, plus slow-frame summaries.");

static constexpr int      kSignonFull          = 8;
static constexpr uint32_t kDemoChallenge       = 0x00DEC0DE;
static constexpr int      kObsModeInEye        = 5;
static constexpr int      kObsModeChase        = 6;
static constexpr int      kObsModeRoaming      = 7;
// C_Player recv-prop offsets (DT_LocalPlayerExclusive, embedded at 0).
static constexpr ptrdiff_t kPlayerObserverMode   = 0x3534;
static constexpr ptrdiff_t kPlayerObserverTarget = 0x3540;
static constexpr ULONGLONG kConnectTimeoutMs   = 30000;
static constexpr ULONGLONG kFuzzSliceMs        = 5000;

enum class DemoPlayState_t
{
	IDLE,
	WAIT_DISCONNECT,  // leaving the live server / previous demo before connecting
	CONNECTING,       // virtual handshake + signon
	PLAYING,
	STOPPING,         // disconnect issued, gate still up until the session ends
};

struct DemoUserCmdAt_s
{
	uint32_t      tick;
	R5DemUserCmd_s cmd;
};

struct DemoViewAt_s
{
	uint32_t    wallMs;
	R5DemView_s view;
};

struct DemoPlayer_s
{
	CDemoReader*    pReader = nullptr;
	DemoPlayState_t state = DemoPlayState_t::IDLE;
	char            szPath[260] = {};
	char            szMap[64] = {};
	char            szMode[64] = {};
	int             nPov = 0;

	std::vector<int> order;       // chunk indices fed for this pov, seq order
	size_t          nCursor = 0;
	int             nOobStage = 0;
	bool            bReachedConnected = false;
	bool            bEnded = false;

	bool            bClockStarted = false;
	uint32_t        nClockBaseWallMs = 0;
	uint32_t        nDurationMs = 0;
	double          flDemoMs = 0.0;
	int64_t         nSeekTargetMs = -1;
	float           flPendingSeekSec = -1.0f;
	bool            bPaused = false;
	int             nStepBudget = 0;
	int             nFrameBudget = 0;
	LARGE_INTEGER   lastFrameQpc = {};
	ULONGLONG       nStateSinceMs = 0;

	int             nViewPov = -1;
	bool            bViewChase = false;
	bool            bFreecam = false;
	bool            bViewDirty = false;   // override written; restore the saved props
	int             nSavedObsMode = 0;
	uint32_t        nSavedObsTarget = 0xFFFFFFFFu;
	bool            bCamInit = false;
	float           camOrigin[3] = {};
	LARGE_INTEGER   camQpc = {};
	int             nCamMode = 0;          // DemoCamMode_t
	// thirdperson_override before the own-pov third person camera took it.
	bool            bThirdPerson = false;
	int             nSavedThirdPerson = -1;
	std::vector<float> bookmarks;          // seconds, from <file>.marks

	int             nSavedPredict = -1;
	int             nSavedDrawHud = -1;
	bool            bOverlayStarted = false;

	// A restart requested while stopping.
	bool            bRestart = false;
	int             nRestartPov = 0;
	float           flRestartSeek = -1.0f;

	std::vector<std::vector<DemoUserCmdAt_s>> userCmds;
	std::vector<DemoViewAt_s> views;      // this pov's recorded view angles
	ULONGLONG       nOverlayAskMs = 0;

	// Full snapshots too large for one datagram arrive as DataBlocks; which
	// ones they are is only known once the bridge has decoded them.
	size_t          nLastBlockOrder = SIZE_MAX;
	std::vector<int> blockKeys;           // order indices, ascending
	bool            bSoftRewind = false;
	// Order index where each level's signon starts; a map reload mid-recording
	// adds one. A full snapshot only rebuilds the level it was sent in.
	std::vector<size_t> levelStarts;

	uint32_t        nLastFedTick = 0;
	int64_t         nLastFedRelMs = 0;
	ULONGLONG       nClockProbeUntilMs = 0;

	bool            bMouseLayer = false;
	bool            bCursorOwned = false;
	int             nSavedFocusSleep = -1;
	float           flSavedVolume = -1.0f;
	LARGE_INTEGER   ffFrameStartQpc = {};
	ULONGLONG       nLastRecoverMs = 0;
};

static DemoPlayer_s s_demo;
static void DemoPlay_CursorPump(void);
static void DemoPlay_RecoverPump(void);
static void DemoPlay_MutePump(void);
static volatile bool s_bDemoGate = false;
static volatile LONG s_nSnapDropped = 0;
static std::vector<uint8_t> s_chunkScratch;

struct DemoFuzz_s
{
	bool bActive = false;
	std::vector<std::string> files;
	size_t nNext = 0;
	ULONGLONG nSliceStartMs = 0;
	int nPlayed = 0;
};
static DemoFuzz_s s_fuzz;

typedef void (__fastcall* PFN_PlayerRoamingView)(uintptr_t pPlayer, float* pEyeOrigin, float* pEyeAngles, float* pFov);
typedef void (__fastcall* PFN_EngineGetViewAngles)(void* pUnused, float* pAngles);
typedef uintptr_t (__fastcall* PFN_GetLocalPlayer)(int nSlot);

static PFN_PlayerRoamingView   v_Player_RoamingView = nullptr;
static PFN_PlayerRoamingView   v_Player_CalcView = nullptr;
static PFN_EngineGetViewAngles v_Engine_GetViewAngles = nullptr;
static float* g_pEngineViewAngles = nullptr;              // split-screen slot 0; stride below
static void*  g_pSplitScreenMgr = nullptr;
static constexpr size_t kViewAnglesStride = 0x41358;
static void (*v_CL_ForceFullUpdate)(void) = nullptr;
static void (__fastcall* v_Script_RunThreadsFrame)(CSquirrelVM* s, float flCurTime) = nullptr;

bool Demo_IsPlaying(void)
{
	return s_bDemoGate;
}

bool DemoPlay_IsActive(void)
{
	return s_demo.state != DemoPlayState_t::IDLE;
}

static uintptr_t DemoPlay_LocalPlayer(void)
{
	static PFN_GetLocalPlayer s_pfn = nullptr;
	if (!s_pfn)
		s_pfn = reinterpret_cast<PFN_GetLocalPlayer>(NetObs_Sym(NetObsSym_t::GetLocalPlayer));
	if (!s_pfn)
		return 0;
	uintptr_t p = 0;
	__try { p = s_pfn(0); }
	__except (EXCEPTION_EXECUTE_HANDLER) { p = 0; }
	return p;
}

int DemoPlay_LocalObserverMode(void)
{
	const uintptr_t pPlayer = DemoPlay_LocalPlayer();
	if (!pPlayer)
		return -1;
	int nMode = -1;
	__try { nMode = *reinterpret_cast<const int*>(pPlayer + kPlayerObserverMode); }
	__except (EXCEPTION_EXECUTE_HANDLER) { nMode = -1; }
	return nMode;
}

static void DemoPlay_SetThirdPerson(const bool bOn);

static void DemoPlay_SetState(const DemoPlayState_t state)
{
	s_demo.state = state;
	s_demo.nStateSinceMs = GetTickCount64();
}

//-----------------------------------------------------------------------------
// Clock helpers
//-----------------------------------------------------------------------------
static bool DemoPlay_IsClockPacket(const R5DemChunkHeader_s& hdr)
{
	return hdr.type == static_cast<uint8_t>(R5DemChunk_t::PACKET)
		&& !(hdr.kind & R5DEM_KIND_PRELUDE)
		&& (hdr.signon >= kSignonFull || (hdr.kind & R5DEM_KIND_FULL));
}

static int64_t DemoPlay_RelMs(const R5DemChunkHeader_s& hdr)
{
	return static_cast<int64_t>(hdr.wallMs) - static_cast<int64_t>(s_demo.nClockBaseWallMs);
}

static void DemoPlay_BuildOrder(void)
{
	s_demo.order.clear();
	s_demo.nClockBaseWallMs = 0;
	s_demo.nDurationMs = 0;

	s_demo.levelStarts.clear();
	s_demo.levelStarts.push_back(0);

	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	bool bBase = false;
	uint32_t nLast = 0;
	int nPrevSignonBlock = -1;
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		const R5DemChunkHeader_s& h = chunks[i].hdr;
		if (h.pov != s_demo.nPov)
			continue;
		const R5DemChunk_t type = static_cast<R5DemChunk_t>(h.type);
		if (type != R5DemChunk_t::SIGNON && type != R5DemChunk_t::RELIABLE && type != R5DemChunk_t::PACKET)
			continue;
		if (type == R5DemChunk_t::SIGNON)
		{
			// Signon blocks climb within one level; a drop is the next level's serverinfo.
			if (nPrevSignonBlock >= 0 && h.signon < nPrevSignonBlock)
				s_demo.levelStarts.push_back(s_demo.order.size());
			nPrevSignonBlock = h.signon;
		}
		s_demo.order.push_back(static_cast<int>(i));
		if (DemoPlay_IsClockPacket(h))
		{
			if (!bBase)
			{
				bBase = true;
				s_demo.nClockBaseWallMs = h.wallMs;
			}
			nLast = h.wallMs;
		}
	}
	s_demo.nDurationMs = (bBase && nLast > s_demo.nClockBaseWallMs) ? nLast - s_demo.nClockBaseWallMs : 0;
}

static void DemoPlay_BuildViews(void)
{
	s_demo.views.clear();
	std::vector<uint8_t> payload;
	for (const DemoChunkRef_s& c : s_demo.pReader->GetChunks())
	{
		if (c.hdr.type != static_cast<uint8_t>(R5DemChunk_t::VIEW) || c.hdr.pov != s_demo.nPov)
			continue;
		if (!s_demo.pReader->ReadPayload(c, payload) || payload.size() < sizeof(R5DemView_s))
			continue;
		DemoViewAt_s e;
		e.wallMs = c.hdr.wallMs;
		memcpy(&e.view, payload.data(), sizeof(R5DemView_s));
		// Angles go straight to the engine view; infinite or absurd values would also overflow the interpolation.
		if (!std::isfinite(e.view.pitch) || !std::isfinite(e.view.yaw) || !std::isfinite(e.view.roll)
			|| fabsf(e.view.pitch) > 1e6f || fabsf(e.view.yaw) > 1e6f || fabsf(e.view.roll) > 1e6f)
			continue;
		s_demo.views.push_back(e);
		if (s_demo.views.size() >= 4000000)
			break;
	}
}

static void DemoPlay_BuildUserCmds(void)
{
	s_demo.userCmds.clear();
	s_demo.userCmds.resize(s_demo.pReader->GetHeader().povCount);

	std::vector<uint8_t> payload;
	for (const DemoChunkRef_s& c : s_demo.pReader->GetChunks())
	{
		if (c.hdr.type != static_cast<uint8_t>(R5DemChunk_t::USERCMD) || c.hdr.pov >= s_demo.userCmds.size())
			continue;
		if (!s_demo.pReader->ReadPayload(c, payload) || payload.size() < sizeof(R5DemUserCmdPrefix_s))
			continue;
		R5DemUserCmdPrefix_s pre;
		memcpy(&pre, payload.data(), sizeof(pre));
		const size_t nAvail = (payload.size() - sizeof(pre)) / sizeof(R5DemUserCmd_s);
		const size_t n = pre.count < nAvail ? pre.count : nAvail;
		std::vector<DemoUserCmdAt_s>& dst = s_demo.userCmds[c.hdr.pov];
		for (size_t i = 0; i < n && dst.size() < 1000000; ++i)
		{
			DemoUserCmdAt_s e;
			memcpy(&e.cmd, payload.data() + sizeof(pre) + i * sizeof(R5DemUserCmd_s), sizeof(R5DemUserCmd_s));
			e.tick = e.cmd.tickCount;
			dst.push_back(e);
		}
	}
}

//-----------------------------------------------------------------------------
// Start / stop
//-----------------------------------------------------------------------------
static void DemoPlay_CloseReader(void)
{
	if (s_demo.pReader)
	{
		delete s_demo.pReader;
		s_demo.pReader = nullptr;
	}
	s_demo.order.clear();
	s_demo.userCmds.clear();
	s_demo.views.clear();
	s_demo.blockKeys.clear();
	s_demo.levelStarts.clear();
	s_demo.nLastBlockOrder = SIZE_MAX;
	s_demo.bSoftRewind = false;
}

static bool DemoPlay_MapInstalled(const char* pszMap)
{
	AUTO_LOCK(g_InstalledMapsMutex);
	if (g_InstalledMaps.IsEmpty())
		return true;
	FOR_EACH_VEC(g_InstalledMaps, i)
	{
		if (!V_stricmp(g_InstalledMaps[i].String(), pszMap))
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Bookmarks and clips
//-----------------------------------------------------------------------------
static constexpr size_t kDemoMaxBookmarks = 500;

static bool DemoPlay_BookmarkPath(char* pszOut, const size_t nOutLen)
{
	return s_demo.szPath[0] && _snprintf_s(pszOut, nOutLen, _TRUNCATE, "%s.marks", s_demo.szPath) > 0;
}

static void DemoPlay_LoadBookmarks(void)
{
	s_demo.bookmarks.clear();
	char szPath[280];
	if (!DemoPlay_BookmarkPath(szPath, sizeof(szPath)))
		return;
	const HANDLE h = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return;
	char buf[8192];
	DWORD n = 0;
	const BOOL bRead = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
	CloseHandle(h);
	if (!bRead)
		return;
	buf[n] = '\0';
	char* pCtx = nullptr;
	for (char* pLine = strtok_s(buf, "\r\n", &pCtx); pLine && s_demo.bookmarks.size() < kDemoMaxBookmarks;
		pLine = strtok_s(nullptr, "\r\n", &pCtx))
	{
		char* pEnd = nullptr;
		const double t = strtod(pLine, &pEnd);
		if (pEnd != pLine && t >= 0.0 && t < 86400.0)
			s_demo.bookmarks.push_back(static_cast<float>(t));
	}
	std::sort(s_demo.bookmarks.begin(), s_demo.bookmarks.end());
}

bool DemoPlay_AddBookmark(void)
{
	if (s_demo.state != DemoPlayState_t::PLAYING || s_demo.bookmarks.size() >= kDemoMaxBookmarks)
		return false;
	const float t = DemoPlay_GetTime();
	for (const float b : s_demo.bookmarks)
	{
		if (fabsf(b - t) < 0.5f)
			return false;
	}

	char szPath[280];
	if (!DemoPlay_BookmarkPath(szPath, sizeof(szPath)))
		return false;
	const HANDLE h = CreateFileA(szPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	char szLine[32];
	const int n = snprintf(szLine, sizeof(szLine), "%.2f\r\n", t);
	DWORD nWritten = 0;
	const bool bOk = n > 0 && WriteFile(h, szLine, static_cast<DWORD>(n), &nWritten, nullptr) && nWritten == static_cast<DWORD>(n);
	CloseHandle(h);
	if (!bOk)
		return false;

	s_demo.bookmarks.push_back(t);
	std::sort(s_demo.bookmarks.begin(), s_demo.bookmarks.end());
	Msg(eDLL_T::ENGINE, "[DEMO] bookmark at %.2f s\n", t);
	return true;
}

static bool DemoPlay_CopyRange(const HANDLE hSrc, const HANDLE hDst, const uint64_t nOffset, uint64_t nBytes,
	std::vector<uint8_t>& scratch)
{
	scratch.resize(1024 * 1024);
	uint64_t nAt = nOffset;
	while (nBytes > 0)
	{
		const DWORD nChunk = static_cast<DWORD>(nBytes < scratch.size() ? nBytes : scratch.size());
		LARGE_INTEGER li;
		li.QuadPart = static_cast<LONGLONG>(nAt);
		DWORD nRead = 0, nWritten = 0;
		if (!SetFilePointerEx(hSrc, li, nullptr, FILE_BEGIN)
			|| !ReadFile(hSrc, scratch.data(), nChunk, &nRead, nullptr) || nRead != nChunk
			|| !WriteFile(hDst, scratch.data(), nChunk, &nWritten, nullptr) || nWritten != nChunk)
			return false;
		nAt += nChunk;
		nBytes -= nChunk;
	}
	return true;
}

// A clip is the replay up to flEndSec (the stream cannot start mid-way without
// the history before it) plus a META that makes it open at flStartSec.
bool DemoPlay_SaveClip(const float flStartSec, const float flEndSec, char* pszOutName, const size_t nOutLen)
{
	if (!pszOutName || !nOutLen)
		return false;
	pszOutName[0] = '\0';
	CDemoReader* const pReader = s_demo.pReader;
	if (s_demo.state != DemoPlayState_t::PLAYING || !pReader
		|| !(flStartSec >= 0.0f) || !(flEndSec > flStartSec) || flEndSec - flStartSec > 600.0f)
		return false;

	const R5DemHeader_s& srcHeader = pReader->GetHeader();
	const float flTick = srcHeader.tickIntervalUs ? srcHeader.tickIntervalUs / 1000000.0f : 0.05f;
	const uint32_t nFirst = pReader->GetFirstFullTick(0);
	if (nFirst == UINT32_MAX)
		return false;
	const uint32_t nEndTick = nFirst + static_cast<uint32_t>(flEndSec / flTick) + 1;

	// <stem>_c<mmss>[_<n>] beside the source.
	const char* const pszFile = strrchr(s_demo.szPath, '\\');
	const char* const pszStemStart = pszFile ? pszFile + 1 : s_demo.szPath;
	char szStem[72];
	strncpy_s(szStem, pszStemStart, _TRUNCATE);
	if (char* const pDot = strrchr(szStem, '.'))
		*pDot = '\0';
	{
		// A clip of a clip keeps the original stem.
		char* pPrev = nullptr;
		for (char* p = strstr(szStem, "_c"); p; p = strstr(p + 1, "_c"))
			pPrev = p;
		bool bDigits = pPrev && pPrev[2] != '\0';
		for (const char* p = pPrev ? pPrev + 2 : ""; *p && bDigits; ++p)
			bDigits = (*p >= '0' && *p <= '9') || *p == '_';
		if (bDigits)
			*pPrev = '\0';
	}
	szStem[52] = '\0';
	const int nStartS = static_cast<int>(flStartSec);

	char szDir[260];
	strncpy_s(szDir, s_demo.szPath, _TRUNCATE);
	if (char* const pSlash = strrchr(szDir, '\\'))
		*pSlash = '\0';
	else
		return false;

	char szName[72] = {};
	char szPath[300] = {};
	bool bFree = false;
	for (int i = 0; i < 100 && !bFree; ++i)
	{
		if (i == 0)
			_snprintf_s(szName, _TRUNCATE, "%s_c%02d%02d", szStem, (nStartS / 60) % 100, nStartS % 60);
		else
			_snprintf_s(szName, _TRUNCATE, "%s_c%02d%02d_%d", szStem, (nStartS / 60) % 100, nStartS % 60, i);
		if (!R5Dem_IsValidName(szName))
			return false;
		_snprintf_s(szPath, _TRUNCATE, "%s\\%s.r5dem", szDir, szName);
		bFree = GetFileAttributesA(szPath) == INVALID_FILE_ATTRIBUTES;
	}
	if (!bFree)
		return false;

	const HANDLE hSrc = CreateFileA(s_demo.szPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hSrc == INVALID_HANDLE_VALUE)
		return false;
	const HANDLE hDst = CreateFileA(szPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hDst == INVALID_HANDLE_VALUE)
	{
		CloseHandle(hSrc);
		return false;
	}

	R5DemHeader_s header = srcHeader;
	header.flags &= ~R5DEM_FLAG_INDEX;
	header.indexOffset = 0;
	DWORD nWritten = 0;
	bool bOk = WriteFile(hDst, &header, sizeof(header), &nWritten, nullptr) && nWritten == sizeof(header);

	std::vector<uint8_t> scratch;
	uint32_t nLastSeq = 0;
	for (const DemoChunkRef_s& ref : pReader->GetChunks())
	{
		if (!bOk)
			break;
		if (ref.hdr.type == static_cast<uint8_t>(R5DemChunk_t::PACKET) && ref.hdr.tick > nEndTick)
			break;
		if (ref.hdr.tick > nEndTick)
			continue;
		bOk = DemoPlay_CopyRange(hSrc, hDst, ref.offset, sizeof(R5DemChunkHeader_s) + ref.hdr.len, scratch);
		nLastSeq = ref.hdr.seq;
	}

	if (bOk)
	{
		std::string meta = pReader->GetMetaJson();
		// The clip fields are always the last members; drop a previous clip's.
		const size_t nOldClip = meta.rfind(",\"clipStart\":");
		if (nOldClip != std::string::npos)
			meta.erase(nOldClip, meta.find_last_of('}') - nOldClip);
		const size_t nClose = meta.find_last_of('}');
		char szClip[128];
		const int n = snprintf(szClip, sizeof(szClip), "\"clipStart\":%.2f,\"clipEnd\":%.2f", flStartSec, flEndSec);
		if (nClose == std::string::npos || n <= 0)
			meta = std::string("{") + szClip + "}";
		else
		{
			const bool bEmpty = meta.find_first_not_of(" \t\r\n{", 0) == nClose;
			meta.insert(nClose, (bEmpty ? std::string() : std::string(",")) + szClip);
		}

		R5DemChunkHeader_s hdr = {};
		hdr.type = static_cast<uint8_t>(R5DemChunk_t::META);
		hdr.pov = R5DEM_POV_ALL;
		hdr.len = static_cast<uint32_t>(meta.size());
		hdr.seq = nLastSeq + 1;
		hdr.crc32 = R5Dem_Crc32(meta.data(), meta.size());
		bOk = meta.size() <= R5DEM_MAX_JSON
			&& WriteFile(hDst, &hdr, sizeof(hdr), &nWritten, nullptr) && nWritten == sizeof(hdr)
			&& WriteFile(hDst, meta.data(), hdr.len, &nWritten, nullptr) && nWritten == hdr.len;
	}

	CloseHandle(hSrc);
	FlushFileBuffers(hDst);
	CloseHandle(hDst);
	if (!bOk)
	{
		DeleteFileA(szPath);
		Warning(eDLL_T::ENGINE, "[DEMO] clip %s could not be written\n", szName);
		return false;
	}

	strncpy_s(pszOutName, nOutLen, szName, _TRUNCATE);
	Msg(eDLL_T::ENGINE, "[DEMO] saved clip %s (%.1f-%.1f s)\n", szName, flStartSec, flEndSec);
	return true;
}

static bool DemoPlay_OpenPath(const char* pszPath, const int nPov, const float flSeekSec)
{
	CDemoReader* const pReader = new CDemoReader();
	char szErr[160] = {};
	if (!pReader->Open(pszPath, szErr, sizeof(szErr)))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] %s: %s\n", pszPath, szErr);
		delete pReader;
		return false;
	}

	char szMap[64] = {};
	char szMode[64] = {};
	const std::string& meta = pReader->GetMetaJson();
	DemoJson::GetString(meta.c_str(), meta.size(), "map", szMap, sizeof(szMap));
	DemoJson::GetString(meta.c_str(), meta.size(), "mode", szMode, sizeof(szMode));
	if (!szMap[0])
		strncpy_s(szMap, pReader->GetHeader().map, _TRUNCATE);
	if (!szMode[0])
		strncpy_s(szMode, pReader->GetHeader().mode, _TRUNCATE);
	for (char* p = szMode; *p; ++p)
	{
		const char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
			*p = '_';
	}

	if (!Bridge_IsBareMapName(szMap))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] %s: bad map name in the demo\n", pszPath);
		delete pReader;
		return false;
	}
	if (!DemoPlay_MapInstalled(szMap))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] map not installed: %s\n", szMap);
		delete pReader;
		return false;
	}
	if (nPov < 0 || nPov >= static_cast<int>(pReader->GetHeader().povCount))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] pov %d out of range (demo has %u)\n", nPov, pReader->GetHeader().povCount);
		delete pReader;
		return false;
	}

	DemoPlay_CloseReader();
	s_demo.pReader = pReader;
	strncpy_s(s_demo.szPath, pszPath, _TRUNCATE);
	strncpy_s(s_demo.szMap, szMap, _TRUNCATE);
	strncpy_s(s_demo.szMode, szMode, _TRUNCATE);
	s_demo.nPov = nPov;
	DemoPlay_BuildOrder();
	DemoPlay_BuildUserCmds();
	DemoPlay_BuildViews();

	if (s_demo.order.empty())
	{
		Warning(eDLL_T::ENGINE, "[DEMO] %s: pov %d has nothing to play\n", pszPath, nPov);
		DemoPlay_CloseReader();
		return false;
	}

	s_demo.nCursor = 0;
	s_demo.nOobStage = 0;
	s_demo.bReachedConnected = false;
	s_demo.bEnded = false;
	s_demo.bClockStarted = false;
	s_demo.flDemoMs = 0.0;
	s_demo.nSeekTargetMs = -1;
	s_demo.flPendingSeekSec = flSeekSec;
	{
		// A clip opens at its own start unless the caller asked for a time.
		double clipStart = 0.0;
		if (flSeekSec < 0.0f && DemoJson::GetNumber(meta.c_str(), meta.size(), "clipStart", &clipStart)
			&& clipStart > 0.0 && clipStart < 86400.0)
			s_demo.flPendingSeekSec = static_cast<float>(clipStart);
	}
	DemoPlay_LoadBookmarks();
	s_demo.nStepBudget = 0;
	s_demo.bCamInit = false;
	s_demo.bViewDirty = false;
	s_demo.bOverlayStarted = false;
	s_demo.nOverlayAskMs = 0;

	Msg(eDLL_T::ENGINE, "[DEMO] loaded '%s': map=%s povs=%u pov=%d duration=%.1f s%s\n",
		pszPath, szMap, pReader->GetHeader().povCount, nPov, s_demo.nDurationMs / 1000.0f,
		pReader->IsTruncated() ? " (unfinished tail)" : "");
	return true;
}

static void DemoPlay_BeginConnect(void)
{
	if (!Cbuf_AddText)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] Cbuf_AddText unresolved -- cannot start playback\n");
		DemoPlay_CloseReader();
		DemoPlay_SetState(DemoPlayState_t::IDLE);
		return;
	}

	ConVar* const pPredict = g_pCVar ? g_pCVar->FindVar("cl_predict") : nullptr;
	if (pPredict && s_demo.nSavedPredict < 0)
	{
		s_demo.nSavedPredict = pPredict->GetInt();
		pPredict->SetValue(0);
	}

	// A replay keeps full speed while another window has focus.
	ConVar* const pFocusSleep = g_pCVar ? g_pCVar->FindVar("not_focus_sleep") : nullptr;
	if (pFocusSleep && s_demo.nSavedFocusSleep < 0)
	{
		s_demo.nSavedFocusSleep = pFocusSleep->GetInt();
		pFocusSleep->SetValue(0);
	}

	s_bDemoGate = true;
	DemoPlay_SetState(DemoPlayState_t::CONNECTING);

	char szLine[96];
	snprintf(szLine, sizeof(szLine), "connect \"127.0.0.1:%u\"\n", static_cast<unsigned>(S21Bridge_GamePort()));
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), szLine, cmd_source_t::kCommandSrcCode);
}

static void DemoPlay_Finalize(void)
{
	s_bDemoGate = false;
	DemoPlay_SetThirdPerson(false);

	ConVar* const pPredict = g_pCVar ? g_pCVar->FindVar("cl_predict") : nullptr;
	if (pPredict && s_demo.nSavedPredict >= 0)
		pPredict->SetValue(s_demo.nSavedPredict);
	s_demo.nSavedPredict = -1;

	ConVar* const pDrawHud = g_pCVar ? g_pCVar->FindVar("cl_drawhud") : nullptr;
	if (pDrawHud && s_demo.nSavedDrawHud >= 0)
		pDrawHud->SetValue(s_demo.nSavedDrawHud);
	s_demo.nSavedDrawHud = -1;

	ConVar* const pFocusSleep = g_pCVar ? g_pCVar->FindVar("not_focus_sleep") : nullptr;
	if (pFocusSleep && s_demo.nSavedFocusSleep >= 0)
		pFocusSleep->SetValue(s_demo.nSavedFocusSleep);
	s_demo.nSavedFocusSleep = -1;

	ConVar* const pVolume = g_pCVar ? g_pCVar->FindVar("sound_volume") : nullptr;
	if (pVolume && s_demo.flSavedVolume >= 0.0f)
		pVolume->SetValue(s_demo.flSavedVolume);
	s_demo.flSavedVolume = -1.0f;

	s_demo.bMouseLayer = false;
	DemoPlay_CursorPump();

	s_demo.nViewPov = -1;
	s_demo.bFreecam = false;
	s_demo.nCamMode = DEMO_CAM_FIRST;
	s_demo.bookmarks.clear();
	s_demo.bPaused = false;
	DemoPlay_CloseReader();
	DemoPlay_SetState(DemoPlayState_t::IDLE);
	Msg(eDLL_T::ENGINE, "[DEMO] playback ended\n");

	if (s_demo.bRestart)
	{
		s_demo.bRestart = false;
		char szPath[260];
		strncpy_s(szPath, s_demo.szPath, _TRUNCATE);
		if (DemoPlay_OpenPath(szPath, s_demo.nRestartPov, s_demo.flRestartSeek))
			DemoPlay_SetState(DemoPlayState_t::WAIT_DISCONNECT);
	}
}

bool DemoPlay_Stop(const char* pszReason)
{
	if (s_demo.state == DemoPlayState_t::IDLE)
		return false;

	Msg(eDLL_T::ENGINE, "[DEMO] stopping playback (%s)\n", pszReason ? pszReason : "?");
	if (s_demo.state == DemoPlayState_t::WAIT_DISCONNECT)
	{
		s_demo.bRestart = false;
		DemoPlay_CloseReader();
		DemoPlay_SetState(DemoPlayState_t::IDLE);
		return true;
	}

	// Teardown runs with the gate up so nothing reaches a socket.
	DemoPlay_SetState(DemoPlayState_t::STOPPING);
	if (Cbuf_AddText)
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "disconnect\n", cmd_source_t::kCommandSrcCode);
	return true;
}

static bool DemoPlay_StartPath(const char* pszPath, const int nPov, const float flSeekSec)
{
	if (DemoRecord_IsRecording())
		DemoRecord_Stop("starting playback");
	if (s_demo.state != DemoPlayState_t::IDLE)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] a demo is already playing -- demo_stop first\n");
		return false;
	}
	if (!DemoPlay_OpenPath(pszPath, nPov, flSeekSec))
		return false;

	DemoPlay_SetState(DemoPlayState_t::WAIT_DISCONNECT);
	if (S21Bridge_GetClientSignonState() > 0 && Cbuf_AddText)
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "disconnect\n", cmd_source_t::kCommandSrcCode);
	return true;
}

bool DemoPlay_Start(const char* pszName, const int nPov, const float flSeekSec)
{
	if (!R5Dem_IsValidName(pszName))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] demo names are [a-zA-Z0-9_-], 1..64 characters\n");
		return false;
	}
	char szPath[260];
	if (!Demo_FindFile(DEMO_CLIENT_ROOT, pszName, szPath, sizeof(szPath)))
	{
		Warning(eDLL_T::ENGINE, "[DEMO] no demo named '%s' under %s\n", pszName, DEMO_CLIENT_ROOT);
		return false;
	}
	return DemoPlay_StartPath(szPath, nPov, flSeekSec);
}

// Reconnect to the same file (pov switch, hard rewind).
static void DemoPlay_Restart(const int nPov, const float flSeekSec)
{
	s_demo.bRestart = true;
	s_demo.nRestartPov = nPov;
	s_demo.flRestartSeek = flSeekSec;
	DemoPlay_SetState(DemoPlayState_t::STOPPING);
	if (Cbuf_AddText)
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "disconnect\n", cmd_source_t::kCommandSrcCode);
}

void DemoPlay_OnSessionEnded(void)
{
	if (!s_bDemoGate)
		return;
	// The engine drops to signon 0 while it sets up the virtual connect; only a
	// session that got connected (or one we are tearing down) ends playback.
	if (s_demo.state == DemoPlayState_t::STOPPING || s_demo.bReachedConnected)
		DemoPlay_Finalize();
}

bool DemoPlay_OnStartHandshake(void)
{
	if (!s_bDemoGate || s_demo.state != DemoPlayState_t::CONNECTING || !s_demo.pReader)
		return false;
	s_demo.nOobStage = 0;
	return true;
}

static constexpr size_t kDemoMaxBlockKeys = 4096;

void DemoPlay_OnFullSnapshotBlock(void)
{
	if (!s_bDemoGate || !s_demo.pReader || s_demo.nLastBlockOrder >= s_demo.order.size()
		|| s_demo.blockKeys.size() >= kDemoMaxBlockKeys)
		return;
	const int nKey = static_cast<int>(s_demo.nLastBlockOrder);
	const auto it = std::lower_bound(s_demo.blockKeys.begin(), s_demo.blockKeys.end(), nKey);
	if (it != s_demo.blockKeys.end() && *it == nKey)
		return;
	s_demo.blockKeys.insert(it, nKey);

	const DemoChunkRef_s& ref = s_demo.pReader->GetChunks()[s_demo.order[nKey]];
	const int64_t nRel = DemoPlay_RelMs(ref.hdr);
	Msg(eDLL_T::ENGINE, "[DEMO] full snapshot at %.2f s (chunk %d) -- rewinds start here\n",
		(nRel > 0 ? nRel : 0) / 1000.0, nKey);
}

bool DemoPlay_InSoftRewind(void)
{
	return s_bDemoGate && s_demo.bSoftRewind;
}

void DemoPlay_OnSnapshotDropped(void)
{
	InterlockedExchange(&s_nSnapDropped, 1);
}

void DemoPlay_SetMouseLayer(const bool bOn)
{
	s_demo.bMouseLayer = bOn;
}

bool DemoPlay_WantsCursor(void)
{
	return s_bDemoGate && s_demo.state == DemoPlayState_t::PLAYING && s_demo.bMouseLayer;
}

// While the controls are up the game must not hold the mouse: release the
// input system's capture, keep the pointer inside the window and visible.
static void DemoPlay_CursorPump(void)
{
	const bool bWant = DemoPlay_WantsCursor();
	const HWND hGame = g_pGame ? g_pGame->GetWindow() : nullptr;
	if (bWant && hGame && GetForegroundWindow() == hGame)
	{
		if (!s_demo.bCursorOwned)
		{
			s_demo.bCursorOwned = true;
			if (g_pInputSystem)
				g_pInputSystem->DisableMouseCapture();
			Input_ConfineCursorToWindow(hGame);
		}
		CURSORINFO ci = {};
		ci.cbSize = sizeof(ci);
		if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
		{
			Input_EnsureCursorVisible();
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
		}
		return;
	}
	if (!bWant && s_demo.bCursorOwned)
	{
		s_demo.bCursorOwned = false;
		if (g_pInputSystem)
		{
			const PlatWindow_t hAttached = g_pInputSystem->GetAttachedWindow();
			if (hAttached)
				g_pInputSystem->EnableMouseCapture(hAttached);
		}
	}
}

//-----------------------------------------------------------------------------
// Datagram source
//-----------------------------------------------------------------------------
static int DemoPlay_BuildChallenge(char* pBuf, const int nCap)
{
	const size_t nMap = strlen(s_demo.szMap);
	const int nNeed = 4 + 1 + 4 + 1 + static_cast<int>(nMap) + 1;
	if (nNeed > nCap)
		return 0;
	uint8_t* p = reinterpret_cast<uint8_t*>(pBuf);
	p[0] = p[1] = p[2] = p[3] = 0xFF;
	p[4] = 0x49;
	memcpy(p + 5, &kDemoChallenge, 4);
	p[9] = 0;
	memcpy(p + 10, s_demo.szMap, nMap);
	p[10 + nMap] = 0;
	return nNeed;
}

static int DemoPlay_BuildConnAccept(char* pBuf, const int nCap)
{
	const size_t nMap = strlen(s_demo.szMap);
	const size_t nMode = strlen(s_demo.szMode);
	const int nNeed = 5 + static_cast<int>(nMap) + 1 + static_cast<int>(nMode) + 1;
	if (nNeed > nCap)
		return 0;
	uint8_t* p = reinterpret_cast<uint8_t*>(pBuf);
	p[0] = p[1] = p[2] = p[3] = 0xFF;
	p[4] = 0x4A;
	memcpy(p + 5, s_demo.szMap, nMap);
	p[5 + nMap] = 0;
	memcpy(p + 6 + nMap, s_demo.szMode, nMode);
	p[6 + nMap + nMode] = 0;
	return nNeed;
}

static void DemoPlay_BadChunk(const DemoChunkRef_s& ref)
{
	Warning(eDLL_T::ENGINE, "[DEMO] %s: bad chunk at offset %llu (type %u len %u) -- stopping\n",
		s_demo.szPath, static_cast<unsigned long long>(ref.offset), ref.hdr.type, ref.hdr.len);
	DemoPlay_Stop("bad chunk");
}

static void DemoPlay_OnEnd(void)
{
	if (s_demo.bEnded)
		return;
	s_demo.bEnded = true;
	s_demo.nSeekTargetMs = -1;
	s_demo.bSoftRewind = false;
	Msg(eDLL_T::ENGINE, "[DEMO] end of demo -- demo_seek to review, demo_stop to leave\n");

	if (s_fuzz.bActive)
		s_fuzz.nSliceStartMs = 0;
}

static bool DemoPlay_FfBudgetSpent(void)
{
	if (!s_demo.ffFrameStartQpc.QuadPart)
		return false;
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	const double ms = static_cast<double>(now.QuadPart - s_demo.ffFrameStartQpc.QuadPart) * 1000.0
		/ static_cast<double>(freq.QuadPart ? freq.QuadPart : 1);
	return ms >= demo_ff_budget_ms.GetFloat();
}

bool DemoPlay_NextDatagram(char* pBuf, const int nCap, int* pLen)
{
	*pLen = 0;
	if (!s_bDemoGate || !s_demo.pReader)
		return false;
	if (s_demo.state != DemoPlayState_t::CONNECTING && s_demo.state != DemoPlayState_t::PLAYING)
		return false;

	if (s_demo.nOobStage == 0)
	{
		*pLen = DemoPlay_BuildChallenge(pBuf, nCap);
		s_demo.nOobStage = 1;
		return *pLen > 0;
	}
	if (s_demo.nOobStage == 1)
	{
		*pLen = DemoPlay_BuildConnAccept(pBuf, nCap);
		s_demo.nOobStage = 2;
		return *pLen > 0;
	}

	if (!S21Bridge_HasNativeChan() || S21Bridge_GetClientSignonState() < 2
		|| S21Bridge_NativeConnectPending() || S21Bridge_SignonHandoffBusy()
		|| (S21Bridge_GetClientSignonState() >= kSignonFull && S21Bridge_S2CHoldQueueBusy()))
		return false;

	if (!s_demo.bReachedConnected)
	{
		s_demo.bReachedConnected = true;
		DemoPlay_SetState(DemoPlayState_t::PLAYING);
	}

	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	for (int nSafety = 0; nSafety < 64; ++nSafety)
	{
		if (s_demo.nCursor >= s_demo.order.size())
		{
			DemoPlay_OnEnd();
			return false;
		}

		const DemoChunkRef_s& ref = chunks[s_demo.order[s_demo.nCursor]];
		const R5DemChunk_t type = static_cast<R5DemChunk_t>(ref.hdr.type);

		if (type == R5DemChunk_t::SIGNON || type == R5DemChunk_t::RELIABLE)
		{
			if (!s_demo.pReader->ReadPayload(ref, s_chunkScratch) || s_chunkScratch.empty())
			{
				DemoPlay_BadChunk(ref);
				return false;
			}
			s_demo.nLastBlockOrder = s_demo.nCursor;
			++s_demo.nCursor;
			if (demo_diag.GetBool())
				Msg(eDLL_T::ENGINE, "[DEMO] feed %s %u bytes signon=%u\n",
					type == R5DemChunk_t::SIGNON ? "SIGNON" : "RELIABLE", ref.hdr.len, ref.hdr.signon);
			S21Bridge_DeliverDataBlock(s_chunkScratch.data(), static_cast<int>(s_chunkScratch.size()));
			if (S21Bridge_SignonHandoffBusy())
				return false;
			continue;
		}

		// PACKET
		const bool bClock = DemoPlay_IsClockPacket(ref.hdr);
		const bool bSeeking = s_demo.nSeekTargetMs >= 0;
		if (bClock)
		{
			if (!s_demo.bClockStarted)
			{
				s_demo.bClockStarted = true;
				s_demo.nClockProbeUntilMs = GetTickCount64() + 15000;
				s_demo.flDemoMs = 0.0;
				QueryPerformanceCounter(&s_demo.lastFrameQpc);
				if (s_demo.flPendingSeekSec > 0.0f)
					s_demo.nSeekTargetMs = static_cast<int64_t>(s_demo.flPendingSeekSec * 1000.0f);
				s_demo.flPendingSeekSec = -1.0f;
			}

			const int64_t nRel = DemoPlay_RelMs(ref.hdr);
			if (s_demo.nSeekTargetMs >= 0)
			{
				if (nRel > s_demo.nSeekTargetMs)
				{
					s_demo.flDemoMs = static_cast<double>(s_demo.nSeekTargetMs);
					s_demo.nSeekTargetMs = -1;
					s_demo.bSoftRewind = false;
					s_demo.nClockProbeUntilMs = GetTickCount64() + 15000;
					Msg(eDLL_T::ENGINE, "[DEMO] seek done at %.2f s\n", s_demo.flDemoMs / 1000.0);
					return false;
				}
				if (s_demo.nFrameBudget >= demo_ff_packets_per_frame.GetInt() || DemoPlay_FfBudgetSpent())
					return false;
			}
			else if (s_demo.nStepBudget > 0)
			{
				--s_demo.nStepBudget;
				s_demo.flDemoMs = static_cast<double>(nRel);
			}
			else
			{
				if (s_demo.bPaused || s_demo.bEnded || static_cast<double>(nRel) > s_demo.flDemoMs)
					return false;
				if (s_demo.nFrameBudget >= demo_feed_max_per_frame.GetInt())
					return false;
			}
		}
		else if (!bSeeking && s_demo.nFrameBudget >= demo_ff_packets_per_frame.GetInt())
		{
			return false;
		}

		if (!s_demo.pReader->ReadPayload(ref, s_chunkScratch) || s_chunkScratch.size() < 9
			|| static_cast<int>(s_chunkScratch.size()) > nCap)
		{
			DemoPlay_BadChunk(ref);
			return false;
		}

		// A PACKET is a netchan datagram; connectionless / split headers only
		// ever come from the player itself.
		uint32_t nHead = 0;
		memcpy(&nHead, s_chunkScratch.data(), 4);
		if (nHead == 0xFFFFFFFFu || nHead == 0xFFFFFFFEu)
		{
			DemoPlay_BadChunk(ref);
			return false;
		}

		memcpy(pBuf, s_chunkScratch.data(), s_chunkScratch.size());
		*pLen = static_cast<int>(s_chunkScratch.size());
		if (bClock)
		{
			s_demo.nLastFedTick = ref.hdr.tick;
			s_demo.nLastFedRelMs = DemoPlay_RelMs(ref.hdr);
		}
		++s_demo.nCursor;
		++s_demo.nFrameBudget;
		if (demo_diag.GetBool())
			Msg(eDLL_T::ENGINE, "[DEMO] feed PACKET %u bytes tick=%u kind=0x%02X rel=%lld\n",
				ref.hdr.len, ref.hdr.tick, ref.hdr.kind, static_cast<long long>(DemoPlay_RelMs(ref.hdr)));
		return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Frame pump, timescale, view
//-----------------------------------------------------------------------------
bool DemoPlay_TimescaleOverride(float* pOut)
{
	if (!s_bDemoGate || !s_demo.bClockStarted || s_demo.state != DemoPlayState_t::PLAYING)
		return false;
	if (s_demo.nSeekTargetMs >= 0)
		*pOut = 1.0f;
	else if (s_demo.bPaused || s_demo.bEnded)
		*pOut = 0.001f; // 0 is clamped to 1 by the garbage guard
	else
		*pOut = demo_timescale.GetFloat();
	return true;
}

static void DemoPlay_WriteObserver(const int nMode, const uint32_t nTarget)
{
	const uintptr_t pPlayer = DemoPlay_LocalPlayer();
	if (!pPlayer)
		return;
	__try
	{
		if (!s_demo.bViewDirty)
		{
			s_demo.nSavedObsMode = *reinterpret_cast<const int*>(pPlayer + kPlayerObserverMode);
			s_demo.nSavedObsTarget = *reinterpret_cast<const uint32_t*>(pPlayer + kPlayerObserverTarget);
		}
		*reinterpret_cast<int*>(pPlayer + kPlayerObserverMode) = nMode;
		*reinterpret_cast<uint32_t*>(pPlayer + kPlayerObserverTarget) = nTarget;
		s_demo.bViewDirty = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void DemoPlay_RestoreObserver(void)
{
	const uintptr_t pPlayer = DemoPlay_LocalPlayer();
	s_demo.bViewDirty = false;
	if (!pPlayer)
		return;
	__try
	{
		*reinterpret_cast<int*>(pPlayer + kPlayerObserverMode) = s_demo.nSavedObsMode;
		*reinterpret_cast<uint32_t*>(pPlayer + kPlayerObserverTarget) = s_demo.nSavedObsTarget;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

static uint32_t DemoPlay_PovHandle(const int nPov)
{
	if (!s_demo.pReader)
		return 0xFFFFFFFFu;
	for (const DemoPovInfo_s& p : s_demo.pReader->GetPovs())
	{
		if (p.id == nPov)
			return p.eh;
	}
	return 0xFFFFFFFFu;
}

// Recorded view angles at the current demo time, yaw interpolated the short way.
static bool DemoPlay_RecordedView(float* pAngles)
{
	if (s_demo.views.empty() || !s_demo.bClockStarted)
		return false;
	const double t = static_cast<double>(s_demo.nClockBaseWallMs) + s_demo.flDemoMs;
	const std::vector<DemoViewAt_s>& v = s_demo.views;
	auto it = std::upper_bound(v.begin(), v.end(), t,
		[](const double x, const DemoViewAt_s& e) { return x < static_cast<double>(e.wallMs); });
	if (it == v.begin())
	{
		pAngles[0] = it->view.pitch; pAngles[1] = it->view.yaw; pAngles[2] = it->view.roll;
		return true;
	}
	const DemoViewAt_s& a = *(it - 1);
	if (it == v.end())
	{
		pAngles[0] = a.view.pitch; pAngles[1] = a.view.yaw; pAngles[2] = a.view.roll;
		return true;
	}
	const DemoViewAt_s& b = *it;
	const double span = static_cast<double>(b.wallMs) - static_cast<double>(a.wallMs);
	float f = span > 0.0 ? static_cast<float>((t - a.wallMs) / span) : 0.0f;
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	// Angles come from the demo file unbounded; a subtract loop never ends on huge or infinite deltas.
	float dYaw = remainderf(b.view.yaw - a.view.yaw, 360.0f);
	if (!std::isfinite(dYaw))
		dYaw = 0.0f;
	pAngles[0] = a.view.pitch + (b.view.pitch - a.view.pitch) * f;
	pAngles[1] = a.view.yaw + dYaw * f;
	pAngles[2] = a.view.roll + (b.view.roll - a.view.roll) * f;
	return true;
}

static bool DemoPlay_UsesRoaming(void)
{
	return s_demo.bFreecam;
}

// Own-pov third person is the game's own shoulder camera: thirdperson_override
// makes the local view player think in third person, so the framing, collision
// and follow are exactly a live third-person match. It is a cheat var the engine
// may reset on connect, so it is asserted every time the view is applied.
static void DemoPlay_SetThirdPerson(const bool bOn)
{
	ConVar* const pThird = g_pCVar ? g_pCVar->FindVar("thirdperson_override") : nullptr;
	if (!pThird)
		return;
	if (bOn)
	{
		if (!s_demo.bThirdPerson)
		{
			s_demo.nSavedThirdPerson = pThird->GetInt();
			s_demo.bThirdPerson = true;
		}
		if (pThird->GetInt() != 1)
			pThird->SetValue(1);
		return;
	}
	if (s_demo.bThirdPerson)
	{
		pThird->SetValue(s_demo.nSavedThirdPerson);
		s_demo.bThirdPerson = false;
	}
}

// True while the camera should follow the recorded player's own aim.
static bool DemoPlay_ViewLocked(void)
{
	return s_bDemoGate && s_demo.state == DemoPlayState_t::PLAYING
		&& !s_demo.bFreecam && s_demo.nViewPov < 0 && !s_demo.views.empty();
}

static float* DemoPlay_ViewAnglesSlot(void)
{
	if (!g_pEngineViewAngles || !g_pSplitScreenMgr)
		return nullptr;
	int nSlot = 0;
	__try
	{
		void** const vtbl = *reinterpret_cast<void***>(g_pSplitScreenMgr);
		nSlot = reinterpret_cast<int(__fastcall*)(void*)>(vtbl[5])(g_pSplitScreenMgr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { nSlot = 0; }
	if (nSlot < 0 || nSlot > 3)
		nSlot = 0;
	return reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(g_pEngineViewAngles) + nSlot * kViewAnglesStride);
}

// Overwrites the engine's view angles with the recorded ones so the mouse
// cannot turn the camera. Called before and after the engine frame.
void DemoPlay_LockView(void)
{
	if (!DemoPlay_ViewLocked())
		return;
	float ang[3];
	if (!DemoPlay_RecordedView(ang))
		return;
	float* const p = DemoPlay_ViewAnglesSlot();
	if (!p)
		return;
	p[0] = ang[0];
	p[1] = ang[1];
	p[2] = ang[2];
}

void DemoPlay_OverlayStarted(void)
{
	if (!s_demo.bOverlayStarted)
		Msg(eDLL_T::ENGINE, "[DEMO] replay overlay is up\n");
	s_demo.bOverlayStarted = true;
}

bool DemoPlay_GetEngineViewAngles(float* pAngles)
{
	if (!v_Engine_GetViewAngles || !pAngles)
		return false;
	v_Engine_GetViewAngles(nullptr, pAngles);
	return pAngles[0] == pAngles[0] && pAngles[1] == pAngles[1];
}

static void __fastcall Hook_Engine_GetViewAngles(void* pUnused, float* pAngles)
{
	v_Engine_GetViewAngles(pUnused, pAngles);
	if (pAngles && DemoPlay_ViewLocked())
		DemoPlay_RecordedView(pAngles);
}

// Script waits are scheduled against the time fed here. A rewind moves the
// game clock back, which would leave every waiting client thread asleep until
// the clock caught up again, so the client VM gets a clock that never goes back.
static CSquirrelVM* s_pScriptClockVM = nullptr;
static float s_flScriptClockOffset = 0.0f;
static float s_flScriptClockLast = 0.0f;

static void __fastcall Hook_Script_RunThreadsFrame(CSquirrelVM* s, float flCurTime)
{
	if (s && s == g_pClientScript)
	{
		if (s != s_pScriptClockVM || !s_bDemoGate)
		{
			s_pScriptClockVM = s_bDemoGate ? s : nullptr;
			s_flScriptClockOffset = 0.0f;
			s_flScriptClockLast = flCurTime;
		}
		else
		{
			float flFed = flCurTime + s_flScriptClockOffset;
			if (flFed < s_flScriptClockLast)
			{
				s_flScriptClockOffset += s_flScriptClockLast - flFed;
				flFed = s_flScriptClockLast;
			}
			s_flScriptClockLast = flFed;
			flCurTime = flFed;
		}
	}
	v_Script_RunThreadsFrame(s, flCurTime);
}

static void DemoPlay_ApplyView(void)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return;

	DemoPlay_SetThirdPerson(!s_demo.bFreecam && s_demo.nViewPov < 0 && s_demo.nCamMode == DEMO_CAM_THIRD);

	// The free camera runs on the roaming view, which the roaming hook places.
	if (DemoPlay_UsesRoaming())
	{
		DemoPlay_WriteObserver(kObsModeRoaming, 0xFFFFFFFFu);
		return;
	}
	if (s_demo.nViewPov >= 0)
	{
		const uint32_t eh = (s_demo.nViewPov < DemoPlay_GetPovCount())
			? DemoPlay_PovHandle(s_demo.nViewPov) : static_cast<uint32_t>(s_demo.nViewPov);
		DemoPlay_WriteObserver(s_demo.bViewChase ? kObsModeChase : kObsModeInEye, eh);
		return;
	}
	if (s_demo.bViewDirty)
		DemoPlay_RestoreObserver();
}

void DemoPlay_OnPacketProcessed(void)
{
	// A full snapshot rewrites every prop, the observer override included.
	DemoPlay_ApplyView();
}

static void DemoPlay_FuzzPump(void)
{
	const char* const pszDir = demo_fuzz_dir.GetString();
	if (!s_fuzz.bActive)
	{
		if (!pszDir || !pszDir[0] || s_demo.state != DemoPlayState_t::IDLE)
			return;
		if (!Demo_IsSafeRoot(pszDir))
		{
			Warning(eDLL_T::ENGINE, "[DEMO-FUZZ] unsafe directory '%s'\n", pszDir);
			demo_fuzz_dir.SetValue("");
			return;
		}
		std::vector<DemoFileInfo_s> files;
		Demo_ListFiles(pszDir, files, 100000);
		s_fuzz.files.clear();
		for (const DemoFileInfo_s& f : files)
			s_fuzz.files.emplace_back(f.path);
		std::sort(s_fuzz.files.begin(), s_fuzz.files.end());
		s_fuzz.nNext = 0;
		s_fuzz.nPlayed = 0;
		s_fuzz.bActive = true;
		Msg(eDLL_T::ENGINE, "[DEMO-FUZZ] %zu files under '%s'\n", s_fuzz.files.size(), pszDir);
	}

	if (s_demo.state == DemoPlayState_t::PLAYING
		&& (s_fuzz.nSliceStartMs == 0 || GetTickCount64() - s_fuzz.nSliceStartMs >= kFuzzSliceMs))
	{
		DemoPlay_Stop("fuzz slice");
		return;
	}
	if (s_demo.state != DemoPlayState_t::IDLE)
		return;

	while (s_fuzz.nNext < s_fuzz.files.size())
	{
		const std::string path = s_fuzz.files[s_fuzz.nNext++];
		++s_fuzz.nPlayed;
		Msg(eDLL_T::ENGINE, "[DEMO-FUZZ] %d/%zu %s\n", s_fuzz.nPlayed, s_fuzz.files.size(), path.c_str());
		if (DemoPlay_StartPath(path.c_str(), 0, -1.0f))
		{
			s_fuzz.nSliceStartMs = GetTickCount64();
			return;
		}
	}

	Msg(eDLL_T::ENGINE, "[DEMO-FUZZ] done: %d files, client still alive\n", s_fuzz.nPlayed);
	s_fuzz.bActive = false;
	demo_fuzz_dir.SetValue("");
}

// -replay <name> [-replay_pov <n>]: play a replay straight from launch, once
// the main menu is up and no connection is open.
static void DemoPlay_LaunchReplay(void)
{
	static bool s_bDone = false;
	static ULONGLONG s_nReadySinceMs = 0;
	if (s_bDone)
		return;

	const char* const pszName = CommandLine()->ParmValue("-replay", "");
	if (!pszName || !pszName[0])
	{
		s_bDone = true;
		return;
	}
	if (!g_pUIScript || S21Bridge_GetClientSignonState() > 0 || S21Bridge_HasNativeChan()
		|| s_demo.state != DemoPlayState_t::IDLE)
	{
		static ULONGLONG s_nLastWhyMs = 0;
		const ULONGLONG nNow = GetTickCount64();
		if (nNow - s_nLastWhyMs >= 10000)
		{
			s_nLastWhyMs = nNow;
			Msg(eDLL_T::ENGINE, "[DEMO] -replay %s waiting: ui=%d signon=%d nativeChan=%d state=%d\n", pszName,
				g_pUIScript ? 1 : 0, S21Bridge_GetClientSignonState(), S21Bridge_HasNativeChan() ? 1 : 0,
				static_cast<int>(s_demo.state));
		}
		s_nReadySinceMs = 0;
		return;
	}
	if (!s_nReadySinceMs)
		s_nReadySinceMs = GetTickCount64();
	if (GetTickCount64() - s_nReadySinceMs < 1500)
		return;

	s_bDone = true;
	const int nPov = CommandLine()->ParmValue("-replay_pov", 0);
	const char* const pszSeek = CommandLine()->ParmValue("-replay_seek", "");
	float flSeek = (pszSeek && pszSeek[0]) ? static_cast<float>(atof(pszSeek)) : -1.0f;
	if (!(flSeek >= 0.0f && flSeek < 86400.0f))
		flSeek = -1.0f;
	Msg(eDLL_T::ENGINE, "[DEMO] -replay %s (pov %d, from %.1f s)\n", pszName, nPov, flSeek);
	if (!DemoPlay_Start(pszName, nPov >= 0 && nPov < R5DEM_MAX_POVS ? nPov : 0, flSeek))
		Warning(eDLL_T::ENGINE, "[DEMO] -replay %s: the replay could not be started\n", pszName);
}

// The engine hides the cursor and sleeps each frame while it is not the active
// app, so a replay that loses focus looks cursorless and choppy. Names the
// window that holds the foreground whenever the picture changes.
static void DemoPlay_FocusProbe(const double dtMs)
{
	static constexpr ptrdiff_t kGameActiveApp = 0x28; // written by the engine's app-activate handler
	static int s_nLines = 0;
	static uint32_t s_nLastKey = UINT32_MAX;
	static double s_flSumMs = 0.0, s_flMaxMs = 0.0;
	static int s_nFrames = 0;
	static ULONGLONG s_nWindowStartMs = 0;

	if (!demo_focus_diag.GetBool() || s_nLines >= 200)
		return;

	s_flSumMs += dtMs;
	s_flMaxMs = dtMs > s_flMaxMs ? dtMs : s_flMaxMs;
	++s_nFrames;

	const HWND hGame = g_pGame ? g_pGame->GetWindow() : nullptr;
	const HWND hFg = GetForegroundWindow();
	DWORD nFgPid = 0;
	if (hFg)
		GetWindowThreadProcessId(hFg, &nFgPid);
	int nActive = -1;
	if (g_pGame)
	{
		__try { nActive = *(reinterpret_cast<const uint8_t*>(g_pGame) + kGameActiveApp); }
		__except (EXCEPTION_EXECUTE_HANDLER) { nActive = -1; }
	}
	CURSORINFO ci = {};
	ci.cbSize = sizeof(ci);
	const bool bShowing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
	RECT clip = {};
	const bool bClipped = GetClipCursor(&clip)
		&& (clip.right - clip.left) < GetSystemMetrics(SM_CXVIRTUALSCREEN);

	const bool bFgGame = hFg && hFg == hGame;
	const bool bFgOurs = nFgPid == GetCurrentProcessId();
	const uint32_t nKey = (bFgGame ? 1u : 0u) | (bFgOurs ? 2u : 0u) | (static_cast<uint32_t>(nActive & 0xFF) << 2)
		| (bShowing ? 1u << 10 : 0u) | (bClipped ? 1u << 11 : 0u) | (s_demo.bPaused ? 1u << 12 : 0u);

	const ULONGLONG nNow = GetTickCount64();
	if (!s_nWindowStartMs)
		s_nWindowStartMs = nNow;

	if (nKey != s_nLastKey)
	{
		s_nLastKey = nKey;
		++s_nLines;
		char szClass[64] = {};
		char szTitle[64] = {};
		if (hFg)
		{
			GetClassNameA(hFg, szClass, sizeof(szClass));
			GetWindowTextA(hFg, szTitle, sizeof(szTitle));
		}
		Warning(eDLL_T::ENGINE, "[DEMO-FOCUS] fg=%p class='%s' title='%s' pid=%lu%s game=%d active=%d cursor=%d clip=%d paused=%d t=%.2f\n",
			static_cast<void*>(hFg), szClass, szTitle, nFgPid, bFgOurs ? " (ours)" : "", bFgGame ? 1 : 0, nActive,
			bShowing ? 1 : 0, bClipped ? 1 : 0, s_demo.bPaused ? 1 : 0, DemoPlay_GetTime());
	}

	if (nNow - s_nWindowStartMs >= 5000)
	{
		const double flAvg = s_nFrames ? s_flSumMs / s_nFrames : 0.0;
		if (flAvg > 30.0)
		{
			++s_nLines;
			Warning(eDLL_T::ENGINE, "[DEMO-FOCUS] slow frames: avg %.1f ms max %.1f ms over %d frames (game=%d active=%d)\n",
				flAvg, s_flMaxMs, s_nFrames, bFgGame ? 1 : 0, nActive);
		}
		s_nWindowStartMs = nNow;
		s_flSumMs = s_flMaxMs = 0.0;
		s_nFrames = 0;
	}
}

// Once a second for 15 s after the clock starts and after each seek, and any
// second in which the world lerp extrapolated or lost its future snapshot.
static void DemoPlay_ClockProbe(const double dtMs)
{
	static int s_nLines = 0;
	static int s_nFrames = 0, s_nExtrap = 0, s_nCollapsed = 0;
	static float s_flLerpMax = 0.0f;
	static double s_flFrameMs = 0.0;
	static ULONGLONG s_nWindowStartMs = 0;

	if (!demo_focus_diag.GetBool() || !s_demo.bClockStarted || s_nLines >= 600)
		return;

	float flLast = 0.0f, flCur = 0.0f, flFut = 0.0f, flLerp = 0.0f;
	if (!PredNative_SnapTimes(&flLast, &flCur, &flFut, &flLerp))
		return;
	++s_nFrames;
	s_flFrameMs += dtMs;
	if (flLerp > 1.0f)
		++s_nExtrap;
	if (flFut <= flCur)
		++s_nCollapsed;
	if (flLerp > s_flLerpMax)
		s_flLerpMax = flLerp;

	const ULONGLONG nNow = GetTickCount64();
	if (!s_nWindowStartMs)
		s_nWindowStartMs = nNow;
	if (nNow - s_nWindowStartMs < 1000)
		return;

	const bool bWindow = nNow < s_demo.nClockProbeUntilMs;
	const bool bBad = (s_nExtrap + s_nCollapsed) * 10 > s_nFrames;
	if ((bWindow || bBad) && !s_demo.bPaused)
	{
		++s_nLines;
		const ClockDriftView_t* const pDrift = NetObs_ClockDrift();
		int nSrv = -1, nCli = -1;
		float flAhead = -1.0f, flScale = -1.0f;
		if (pDrift)
		{
			__try
			{
				nSrv = pDrift->m_nServerTick;
				nCli = pDrift->m_nClientTick;
				flAhead = pDrift->m_aheadBy;
				flScale = pDrift->m_serverFrameTimeScaleAverage;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		const float flTick = S21Bridge_IntervalPerTick();
		Warning(eDLL_T::ENGINE, "[DEMO-CLOCK] t=%.2f fed{tick=%u rel=%.2f} snap=%u curtime=%.3f pair{cur=%.3f fut=%.3f lerpMax=%.2f} "
			"drift{srv=%d cli=%d ahead=%.3f scale=%.3f} srvTime=%.3f frames=%d extrap=%d collapsed=%d avg=%.1fms%s\n",
			s_demo.flDemoMs / 1000.0, s_demo.nLastFedTick, s_demo.nLastFedRelMs / 1000.0, S21Bridge_LastSnapshotTick(),
			PredNative_CurTime(), flCur, flFut, s_flLerpMax, nSrv, nCli, flAhead, flScale, nSrv * flTick,
			s_nFrames, s_nExtrap, s_nCollapsed, s_nFrames ? s_flFrameMs / s_nFrames : 0.0,
			s_demo.nSeekTargetMs >= 0 ? " seeking" : "");
	}
	s_nWindowStartMs = nNow;
	s_nFrames = s_nExtrap = s_nCollapsed = 0;
	s_flLerpMax = 0.0f;
	s_flFrameMs = 0.0;
}

void DemoPlay_OnHostFrame(void)
{
	DemoPlay_FuzzPump();
	DemoPlay_LaunchReplay();

	switch (s_demo.state)
	{
	case DemoPlayState_t::IDLE:
		return;

	case DemoPlayState_t::WAIT_DISCONNECT:
		if (S21Bridge_GetClientSignonState() <= 0 && GetTickCount64() - s_demo.nStateSinceMs > 100)
			DemoPlay_BeginConnect();
		else if (GetTickCount64() - s_demo.nStateSinceMs > kConnectTimeoutMs)
		{
			Warning(eDLL_T::ENGINE, "[DEMO] the previous connection never closed -- playback cancelled\n");
			DemoPlay_CloseReader();
			DemoPlay_SetState(DemoPlayState_t::IDLE);
		}
		return;

	case DemoPlayState_t::CONNECTING:
		if (GetTickCount64() - s_demo.nStateSinceMs > kConnectTimeoutMs)
		{
			Warning(eDLL_T::ENGINE, "[DEMO] virtual connect did not reach CONNECTED -- stopping\n");
			DemoPlay_Stop("connect timeout");
		}
		return;

	case DemoPlayState_t::STOPPING:
		// Disconnect never reached signon 0 (it was not connected yet).
		if (GetTickCount64() - s_demo.nStateSinceMs > 3000)
			DemoPlay_Finalize();
		return;

	case DemoPlayState_t::PLAYING:
		break;
	}

	S21Bridge_StampChanAlive();

	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	double dtMs = 0.0;
	if (s_demo.lastFrameQpc.QuadPart && freq.QuadPart)
		dtMs = static_cast<double>(now.QuadPart - s_demo.lastFrameQpc.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
	s_demo.lastFrameQpc = now;
	DemoPlay_FocusProbe(dtMs);
	if (dtMs > 250.0)
		dtMs = 250.0;

	if (s_demo.bClockStarted && !s_demo.bPaused && !s_demo.bEnded && s_demo.nSeekTargetMs < 0)
		s_demo.flDemoMs += dtMs * demo_timescale.GetFloat();

	DemoPlay_RecoverPump();
	DemoPlay_MutePump();
	s_demo.nFrameBudget = 0;
	QueryPerformanceCounter(&s_demo.ffFrameStartQpc);
	DemoPlay_ClockProbe(dtMs);
	DemoPlay_CursorPump();
	DemoPlay_ApplyView();

	// demo_hud 2 hides the whole HUD; 0 and 1 keep the stock HUD and the
	// overlay script reads demo_hud itself.
	ConVar* const pDrawHud = g_pCVar ? g_pCVar->FindVar("cl_drawhud") : nullptr;
	if (pDrawHud)
	{
		if (s_demo.nSavedDrawHud < 0)
			s_demo.nSavedDrawHud = pDrawHud->GetInt();
		const int nWant = demo_hud.GetInt() == 2 ? 0 : s_demo.nSavedDrawHud;
		if (pDrawHud->GetInt() != nWant)
			pDrawHud->SetValue(nWant);
	}

	DemoPlay_LockView();

	if (!s_demo.bOverlayStarted && S21Bridge_GetClientSignonState() >= kSignonFull
		&& GetTickCount64() - s_demo.nOverlayAskMs >= 2000)
	{
		if (!s_demo.nOverlayAskMs)
			Msg(eDLL_T::ENGINE, "[DEMO] in game -- starting the replay overlay (%zu recorded view samples)\n", s_demo.views.size());
		s_demo.nOverlayAskMs = GetTickCount64();
		DemoNatives_StartOverlay();
	}
}

//-----------------------------------------------------------------------------
// Controls
//-----------------------------------------------------------------------------
void DemoPlay_SetPaused(const bool bPaused)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return;
	s_demo.bPaused = bPaused;
	Msg(eDLL_T::ENGINE, "[DEMO] %s at %.2f s\n", bPaused ? "paused" : "resumed", s_demo.flDemoMs / 1000.0);
}

bool DemoPlay_IsPaused(void)
{
	return s_demo.bPaused;
}

void DemoPlay_Step(void)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return;
	s_demo.bPaused = true;
	s_demo.nStepBudget = 1;
}

void DemoPlay_SetTimescale(const float flScale)
{
	demo_timescale.SetValue(flScale);
}

float DemoPlay_GetTimescale(void)
{
	return demo_timescale.GetFloat();
}

// Where playback is headed: a pending restart, rewind or fast-forward target
// counts as the current time, so relative seeks stack and the bar never
// snaps back to 0 while the file reconnects.
float DemoPlay_GetTime(void)
{
	if (s_demo.bRestart)
		return s_demo.flRestartSeek > 0.0f ? s_demo.flRestartSeek : 0.0f;
	if (!s_demo.bClockStarted)
		return s_demo.flPendingSeekSec > 0.0f ? s_demo.flPendingSeekSec : 0.0f;
	if (s_demo.nSeekTargetMs >= 0)
		return static_cast<float>(s_demo.nSeekTargetMs / 1000.0);
	return static_cast<float>(s_demo.flDemoMs / 1000.0);
}

bool DemoPlay_IsSeeking(void)
{
	return s_demo.state != DemoPlayState_t::IDLE
		&& (s_demo.bRestart || !s_demo.bClockStarted || s_demo.nSeekTargetMs >= 0);
}

float DemoPlay_GetDuration(void)
{
	return s_demo.nDurationMs / 1000.0f;
}

static int DemoPlay_LevelOf(const size_t nOrder)
{
	const auto it = std::upper_bound(s_demo.levelStarts.begin(), s_demo.levelStarts.end(), nOrder);
	return it == s_demo.levelStarts.begin() ? 0 : static_cast<int>(it - s_demo.levelStarts.begin()) - 1;
}

// Level the world is in once playback reaches nTargetMs.
static int DemoPlay_LevelAtMs(const int64_t nTargetMs)
{
	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	size_t nLast = 0;
	for (size_t i = 0; i < s_demo.order.size(); ++i)
	{
		const R5DemChunkHeader_s& h = chunks[s_demo.order[i]].hdr;
		if (!DemoPlay_IsClockPacket(h))
			continue;
		if (DemoPlay_RelMs(h) > nTargetMs)
			break;
		nLast = i;
	}
	return DemoPlay_LevelOf(nLast);
}

// Index into the order list of the last full snapshot of nLevel at or before
// nTargetMs: a datagram flagged at record time, or a DataBlock seen decoding one.
static int DemoPlay_FindKeyframe(const int64_t nTargetMs, const int nLevel)
{
	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	const size_t nBegin = static_cast<size_t>(nLevel) < s_demo.levelStarts.size() ? s_demo.levelStarts[nLevel] : 0;
	const size_t nEnd = static_cast<size_t>(nLevel) + 1 < s_demo.levelStarts.size()
		? s_demo.levelStarts[nLevel + 1] : s_demo.order.size();
	int best = -1;
	for (size_t i = nBegin; i < nEnd; ++i)
	{
		const R5DemChunkHeader_s& h = chunks[s_demo.order[i]].hdr;
		if (!DemoPlay_IsClockPacket(h) || !(h.kind & R5DEM_KIND_FULL))
			continue;
		if (DemoPlay_RelMs(h) > nTargetMs)
			break;
		best = static_cast<int>(i);
	}
	for (const int k : s_demo.blockKeys)
	{
		if (static_cast<size_t>(k) < nBegin || static_cast<size_t>(k) >= nEnd)
			continue;
		if (DemoPlay_RelMs(chunks[s_demo.order[k]].hdr) > nTargetMs)
			break;
		if (k > best)
			best = k;
	}
	for (size_t i = nBegin; i < nEnd; ++i)
	{
		const R5DemChunkHeader_s& h = chunks[s_demo.order[i]].hdr;
		const R5DemChunk_t type = static_cast<R5DemChunk_t>(h.type);
		if ((type != R5DemChunk_t::SIGNON && type != R5DemChunk_t::RELIABLE) || !(h.kind & R5DEM_KIND_FULL))
			continue;
		if (DemoPlay_RelMs(h) > nTargetMs)
			break;
		if (static_cast<int>(i) > best)
			best = static_cast<int>(i);
	}
	return best;
}

// bRebuild: the world is broken, replay from a full snapshot even when the
// target is ahead of the cursor.
static bool DemoPlay_SeekInternal(const float flSeconds, const bool bRebuild)
{
	if (!(flSeconds == flSeconds))
		return false;
	float flClamped = flSeconds < 0.0f ? 0.0f : flSeconds;
	if (flClamped > DemoPlay_GetDuration())
		flClamped = DemoPlay_GetDuration();

	// A restart or connect already in flight takes the new target; starting
	// another one would reload the map again.
	if (s_demo.bRestart)
	{
		s_demo.flRestartSeek = flClamped;
		return true;
	}
	if (!s_demo.pReader)
		return false;
	if (s_demo.state == DemoPlayState_t::WAIT_DISCONNECT || s_demo.state == DemoPlayState_t::CONNECTING
		|| (s_demo.state == DemoPlayState_t::PLAYING && !s_demo.bClockStarted))
	{
		s_demo.flPendingSeekSec = flClamped;
		return true;
	}
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return false;

	const int64_t nTargetMs = static_cast<int64_t>(flClamped * 1000.0f);

	// The loaded world belongs to the level of the last chunk fed. A full
	// snapshot from another level decodes against the wrong tables: an earlier
	// level needs a reload, a later one is reached by playing through the reload.
	const int nCurLevel = DemoPlay_LevelOf(s_demo.nCursor ? s_demo.nCursor - 1 : 0);
	const int nTgtLevel = DemoPlay_LevelAtMs(nTargetMs);
	if (nTgtLevel < nCurLevel)
	{
		DemoPlay_Restart(s_demo.nPov, flClamped);
		return true;
	}

	s_demo.bEnded = false;
	const int nKey = nTgtLevel == nCurLevel ? DemoPlay_FindKeyframe(nTargetMs, nCurLevel) : -1;
	if (!bRebuild && static_cast<double>(nTargetMs) >= s_demo.flDemoMs)
	{
		// Jump to a full snapshot past the cursor instead of replaying up to it.
		const bool bJump = nKey >= 0 && static_cast<size_t>(nKey) > s_demo.nCursor && v_CL_ForceFullUpdate
			&& DemoPlay_RelMs(s_demo.pReader->GetChunks()[s_demo.order[nKey]].hdr) > static_cast<int64_t>(s_demo.flDemoMs) + 3000;
		if (!bJump)
		{
			s_demo.nSeekTargetMs = nTargetMs;
			return true;
		}
	}

	if (demo_seek_restart.GetBool() || nKey < 0 || !v_CL_ForceFullUpdate)
	{
		DemoPlay_Restart(s_demo.nPov, flClamped);
		return true;
	}

	// Soft rewind: drop the receive-side dedup state, ask the engine for a full
	// update, and replay from the nearest full snapshot.
	S21Bridge_ResetForDemoSeek();
	v_CL_ForceFullUpdate();
	s_demo.nCursor = static_cast<size_t>(nKey);
	const std::vector<DemoChunkRef_s>& chunks = s_demo.pReader->GetChunks();
	const int64_t nKeyMs = DemoPlay_RelMs(chunks[s_demo.order[nKey]].hdr);
	s_demo.flDemoMs = static_cast<double>(nKeyMs > 0 ? nKeyMs : 0);
	s_demo.nSeekTargetMs = nTargetMs;
	s_demo.bSoftRewind = true;
	Msg(eDLL_T::ENGINE, "[DEMO] rewinding to %.2f s from the full snapshot at %.2f s\n",
		flClamped, s_demo.flDemoMs / 1000.0);
	return true;
}

bool DemoPlay_Seek(const float flSeconds)
{
	return DemoPlay_SeekInternal(flSeconds, false);
}

// A snapshot that fails to parse leaves every later delta without its base:
// the clock runs on while the world stands still. Rebuild from a full snapshot.
static void DemoPlay_RecoverPump(void)
{
	if (!InterlockedExchange(&s_nSnapDropped, 0) || s_demo.state != DemoPlayState_t::PLAYING
		|| !s_demo.bClockStarted || s_demo.bRestart)
		return;
	const ULONGLONG nNow = GetTickCount64();
	if (nNow - s_demo.nLastRecoverMs < 3000)
		return;
	// A rebuild that fails again gets a clean reload of the file instead.
	const bool bRepeat = s_demo.nLastRecoverMs && nNow - s_demo.nLastRecoverMs < 30000;
	s_demo.nLastRecoverMs = nNow;
	const float flAt = DemoPlay_GetTime();
	Warning(eDLL_T::ENGINE, "[DEMO] a snapshot failed to decode at %.2f s -- %s\n", flAt,
		bRepeat ? "reloading the replay" : "rebuilding from the last full snapshot");
	if (bRepeat)
		DemoPlay_Restart(s_demo.nPov, flAt);
	else
		DemoPlay_SeekInternal(flAt, true);
}

// Fast-forwarded audio is noise; the replay is silent until a seek lands.
static void DemoPlay_MutePump(void)
{
	ConVar* const pVolume = g_pCVar ? g_pCVar->FindVar("sound_volume") : nullptr;
	if (!pVolume)
		return;
	const bool bMute = s_demo.state == DemoPlayState_t::PLAYING && DemoPlay_IsSeeking();
	if (bMute && s_demo.flSavedVolume < 0.0f)
	{
		s_demo.flSavedVolume = pVolume->GetFloat();
		pVolume->SetValue(0.0f);
	}
	else if (!bMute && s_demo.flSavedVolume >= 0.0f)
	{
		pVolume->SetValue(s_demo.flSavedVolume);
		s_demo.flSavedVolume = -1.0f;
	}
}

int DemoPlay_GetPov(void)
{
	return s_demo.nPov;
}

int DemoPlay_GetPovCount(void)
{
	return s_demo.pReader ? static_cast<int>(s_demo.pReader->GetHeader().povCount) : 0;
}

const char* DemoPlay_GetPovName(const int nPov)
{
	if (!s_demo.pReader)
		return "";
	for (const DemoPovInfo_s& p : s_demo.pReader->GetPovs())
	{
		if (p.id == nPov)
			return p.name;
	}
	return "";
}

bool DemoPlay_SetPov(const int nPov)
{
	if (s_demo.state != DemoPlayState_t::PLAYING || nPov < 0 || nPov >= DemoPlay_GetPovCount())
		return false;
	if (nPov == s_demo.nPov)
		return true;
	DemoPlay_Restart(nPov, DemoPlay_GetTime());
	return true;
}

bool DemoPlay_SetView(const int nPovId, const bool bChase)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return false;
	if (nPovId < 0 || nPovId == s_demo.nPov)
	{
		s_demo.nViewPov = -1;
	}
	else
	{
		if (nPovId < DemoPlay_GetPovCount() && DemoPlay_PovHandle(nPovId) == 0xFFFFFFFFu)
		{
			Warning(eDLL_T::ENGINE, "[DEMO] pov %d has no entity handle in this demo\n", nPovId);
			return false;
		}
		s_demo.nViewPov = nPovId;
	}
	s_demo.bViewChase = bChase;
	s_demo.bFreecam = false;
	s_demo.nCamMode = bChase ? DEMO_CAM_THIRD : DEMO_CAM_FIRST;
	DemoPlay_ApplyView();
	return true;
}

int DemoPlay_GetView(void)
{
	return s_demo.nViewPov;
}

void DemoPlay_SetFreecam(const bool bOn)
{
	if (s_demo.state != DemoPlayState_t::PLAYING)
		return;
	if (bOn && !v_Player_RoamingView)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] free camera unavailable (roaming view unresolved) -- use demo_view\n");
		return;
	}
	s_demo.bFreecam = bOn;
	s_demo.nCamMode = bOn ? DEMO_CAM_FREE : DEMO_CAM_FIRST;
	s_demo.bCamInit = false;
	DemoPlay_ApplyView();
}

bool DemoPlay_IsFreecam(void)
{
	return s_demo.bFreecam;
}

bool DemoPlay_SetCamera(const int nMode)
{
	if (s_demo.state != DemoPlayState_t::PLAYING || nMode < DEMO_CAM_FIRST || nMode >= DEMO_CAM_COUNT)
		return false;
	if (nMode != DEMO_CAM_FIRST && nMode != DEMO_CAM_THIRD && !v_Player_RoamingView)
	{
		Warning(eDLL_T::ENGINE, "[DEMO] camera %d unavailable (roaming view unresolved)\n", nMode);
		return false;
	}

	switch (nMode)
	{
	case DEMO_CAM_FREE:
		DemoPlay_SetFreecam(true);
		return s_demo.bFreecam;
	case DEMO_CAM_THIRD:
		if (s_demo.nViewPov >= 0)
			return DemoPlay_SetView(s_demo.nViewPov, true);
		break;
	case DEMO_CAM_FIRST:
		if (s_demo.nViewPov >= 0)
			return DemoPlay_SetView(s_demo.nViewPov, false);
		break;
	default:
		break;
	}

	s_demo.bFreecam = false;
	s_demo.bViewChase = false;
	s_demo.nCamMode = nMode;
	s_demo.bCamInit = false;
	DemoPlay_ApplyView();
	return true;
}

int DemoPlay_GetCamera(void)
{
	return s_demo.bFreecam ? DEMO_CAM_FREE : s_demo.nCamMode;
}

std::string DemoPlay_GetMetaJson(void)
{
	return s_demo.pReader ? s_demo.pReader->GetMetaJson() : std::string();
}

void DemoPlay_GetEvents(std::vector<std::string>& outJson, std::vector<std::string>& outRecords)
{
	outJson.clear();
	outRecords.clear();
	if (!s_demo.pReader)
		return;
	// type|seconds|attackerPov|victimPov|weapon|damage|attackerName|victimName
	auto clean = [](const char* pszIn, char* pszOut, const size_t nOut)
	{
		size_t o = 0;
		for (size_t i = 0; pszIn[i] && o + 1 < nOut; ++i)
			pszOut[o++] = (pszIn[i] == '|' || pszIn[i] == '\n' || pszIn[i] == '\r') ? '/' : pszIn[i];
		pszOut[o] = '\0';
	};
	for (const DemoEvent_s& e : s_demo.pReader->GetEvents())
	{
		outJson.push_back(e.json);
		char szA[64], szV[64], szT[16], szW[48];
		clean(e.attackerName, szA, sizeof(szA));
		clean(e.victimName, szV, sizeof(szV));
		clean(e.type, szT, sizeof(szT));
		clean(e.weapon, szW, sizeof(szW));
		char sz[320];
		snprintf(sz, sizeof(sz), "%s|%.3f|%d|%d|%s|%.1f|%s|%s", szT, e.timeSec, e.attacker, e.victim, szW,
			e.damage, szA, szV);
		outRecords.emplace_back(sz);
	}
	for (const float t : s_demo.bookmarks)
	{
		char sz[64];
		snprintf(sz, sizeof(sz), "bookmark|%.3f|-1|-1||0.0||", t);
		outJson.emplace_back("{\"t\":\"bookmark\"}");
		outRecords.emplace_back(sz);
	}
}

bool DemoPlay_GetInputsAt(const int nPov, const float flSeconds, char* pszOut, const size_t nOutLen)
{
	if (!s_demo.pReader || nPov < 0 || nPov >= static_cast<int>(s_demo.userCmds.size()))
		return false;
	const std::vector<DemoUserCmdAt_s>& cmds = s_demo.userCmds[nPov];
	if (cmds.empty())
		return false;

	const uint32_t nFirst = s_demo.pReader->GetFirstFullTick(s_demo.nPov);
	const float flTick = s_demo.pReader->GetHeader().tickIntervalUs
		? s_demo.pReader->GetHeader().tickIntervalUs / 1000000.0f : 0.05f;
	if (nFirst == UINT32_MAX)
		return false;
	const uint32_t nTick = nFirst + static_cast<uint32_t>((flSeconds > 0.0f ? flSeconds : 0.0f) / flTick);

	auto it = std::upper_bound(cmds.begin(), cmds.end(), nTick,
		[](const uint32_t t, const DemoUserCmdAt_s& c) { return t < c.tick; });
	if (it != cmds.begin())
		--it;
	const R5DemUserCmd_s& c = it->cmd;
	snprintf(pszOut, nOutLen, "%u|%.2f|%.2f|%.2f|%.2f|%.2f", c.buttons, c.forwardmove, c.sidemove, c.upmove, c.pitch, c.yaw);
	return true;
}

//-----------------------------------------------------------------------------
// Free camera: the roaming observer view, driven by the local view angles and
// the movement keys while the game window has focus.
//-----------------------------------------------------------------------------
static bool DemoPlay_WindowFocused(void)
{
	const HWND hFg = GetForegroundWindow();
	if (!hFg)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(hFg, &pid);
	return pid == GetCurrentProcessId();
}

static void __fastcall Hook_Player_RoamingView(uintptr_t pPlayer, float* pEyeOrigin, float* pEyeAngles, float* pFov)
{
	v_Player_RoamingView(pPlayer, pEyeOrigin, pEyeAngles, pFov);

	if (!s_bDemoGate || !pEyeOrigin || !pEyeAngles || pPlayer != DemoPlay_LocalPlayer())
		return;

	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);

	if (!s_demo.bFreecam)
		return;
	if (!s_demo.bCamInit)
	{
		s_demo.camOrigin[0] = pEyeOrigin[0];
		s_demo.camOrigin[1] = pEyeOrigin[1];
		s_demo.camOrigin[2] = pEyeOrigin[2];
		s_demo.camQpc = now;
		s_demo.bCamInit = true;
	}
	float dt = freq.QuadPart ? static_cast<float>(now.QuadPart - s_demo.camQpc.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
	s_demo.camQpc = now;
	if (dt < 0.0f || dt > 0.1f)
		dt = 0.0f;

	float ang[3] = { pEyeAngles[0], pEyeAngles[1], 0.0f };
	if (v_Engine_GetViewAngles)
		v_Engine_GetViewAngles(nullptr, ang);

	if (DemoPlay_WindowFocused())
	{
		const float flDeg2Rad = 3.14159265358979f / 180.0f;
		const float sp = sinf(ang[0] * flDeg2Rad), cp = cosf(ang[0] * flDeg2Rad);
		const float sy = sinf(ang[1] * flDeg2Rad), cy = cosf(ang[1] * flDeg2Rad);
		const float fwd[3] = { cp * cy, cp * sy, -sp };
		const float right[3] = { sy, -cy, 0.0f };

		float f = 0.0f, s = 0.0f, u = 0.0f;
		if (GetAsyncKeyState('W') & 0x8000) f += 1.0f;
		if (GetAsyncKeyState('S') & 0x8000) f -= 1.0f;
		if (GetAsyncKeyState('D') & 0x8000) s += 1.0f;
		if (GetAsyncKeyState('A') & 0x8000) s -= 1.0f;
		if (GetAsyncKeyState('E') & 0x8000) u += 1.0f;
		if (GetAsyncKeyState('Q') & 0x8000) u -= 1.0f;
		float speed = demo_freecam_speed.GetFloat();
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
			speed *= 3.0f;

		for (int i = 0; i < 3; ++i)
		{
			float v = s_demo.camOrigin[i] + (fwd[i] * f + right[i] * s) * speed * dt;
			if (i == 2)
				v += u * speed * dt;
			if (v > 65535.0f) v = 65535.0f;
			if (v < -65535.0f) v = -65535.0f;
			s_demo.camOrigin[i] = v;
		}
	}

	pEyeOrigin[0] = s_demo.camOrigin[0];
	pEyeOrigin[1] = s_demo.camOrigin[1];
	pEyeOrigin[2] = s_demo.camOrigin[2];
	pEyeAngles[0] = ang[0];
	pEyeAngles[1] = ang[1];
	pEyeAngles[2] = 0.0f;
}

// The stock view lets a scripted camera (traversal and portal animations) win
// over the observer mode; the replay's roaming cameras must win instead.
static void __fastcall Hook_Player_CalcView(uintptr_t pPlayer, float* pEyeOrigin, float* pEyeAngles, float* pFov)
{
	if (s_bDemoGate && s_demo.state == DemoPlayState_t::PLAYING && DemoPlay_UsesRoaming() && v_Player_RoamingView
		&& pPlayer && pPlayer == DemoPlay_LocalPlayer())
	{
		Hook_Player_RoamingView(pPlayer, pEyeOrigin, pEyeAngles, pFov);
		return;
	}
	v_Player_CalcView(pPlayer, pEyeOrigin, pEyeAngles, pFov);
}

//-----------------------------------------------------------------------------
// Console
//-----------------------------------------------------------------------------
static void DemoPlay_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::ENGINE, "Usage: demo_play <name> [pov] [seconds]\n");
		return;
	}
	const int nPov = args.ArgC() >= 3 ? atoi(args.Arg(2)) : 0;
	const float flSeek = args.ArgC() >= 4 ? static_cast<float>(atof(args.Arg(3))) : -1.0f;
	DemoPlay_Start(args.Arg(1), nPov, flSeek);
}
static ConCommand demo_play("demo_play", DemoPlay_f,
	"Play a demo from platform/demos. Usage: demo_play <name> [pov] [seconds]", FCVAR_RELEASE);

static void DemoPause_f(const CCommand& args)
{
	if (args.ArgC() >= 2)
		DemoPlay_SetPaused(atoi(args.Arg(1)) != 0);
	else
		DemoPlay_SetPaused(!DemoPlay_IsPaused());
}
static ConCommand demo_pause("demo_pause", DemoPause_f,
	"Toggle demo pause, or demo_pause 0|1.", FCVAR_RELEASE);

static void DemoStep_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	DemoPlay_Step();
}
static ConCommand demo_step("demo_step", DemoStep_f,
	"Pause the demo and advance it by one packet.", FCVAR_RELEASE);

static void DemoSeek_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::ENGINE, "Usage: demo_seek <seconds>  (now %.2f of %.2f)\n", DemoPlay_GetTime(), DemoPlay_GetDuration());
		return;
	}
	const char* const psz = args.Arg(1);
	float fl = static_cast<float>(atof(psz));
	if (psz[0] == '+' || psz[0] == '-')
		fl += DemoPlay_GetTime();
	DemoPlay_Seek(fl);
}
static ConCommand demo_seek("demo_seek", DemoSeek_f,
	"Seek the demo. Usage: demo_seek <seconds> | +n | -n", FCVAR_RELEASE);

static void DemoPov_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::ENGINE, "pov %d of %d\n", DemoPlay_GetPov(), DemoPlay_GetPovCount());
		return;
	}
	DemoPlay_SetPov(atoi(args.Arg(1)));
}
static ConCommand demo_pov("demo_pov", DemoPov_f,
	"Replay the demo from another recorded pov at the current time.", FCVAR_RELEASE);

static void DemoView_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg(eDLL_T::ENGINE, "Usage: demo_view <povId|ehandle> [chase]   demo_view -1 restores\n");
		return;
	}
	DemoPlay_SetView(atoi(args.Arg(1)), args.ArgC() >= 3 && atoi(args.Arg(2)) != 0);
}
static ConCommand demo_view("demo_view", DemoView_f,
	"View another player through the observer camera while a demo plays.", FCVAR_RELEASE);

static void DemoFreecam_f(const CCommand& args)
{
	DemoPlay_SetFreecam(args.ArgC() >= 2 ? atoi(args.Arg(1)) != 0 : !DemoPlay_IsFreecam());
}
static ConCommand demo_freecam("demo_freecam", DemoFreecam_f,
	"Toggle the free camera while a demo plays (WASD, E/Q up/down, Shift faster).", FCVAR_RELEASE);

static void DemoList_f(const CCommand& args)
{
	NOTE_UNUSED(args);
	std::vector<DemoFileInfo_s> files;
	Demo_ListFiles(DEMO_CLIENT_ROOT, files, 256);
	for (const DemoFileInfo_s& f : files)
		Msg(eDLL_T::ENGINE, "  %-64s %8llu KB%s\n", f.name, static_cast<unsigned long long>(f.size / 1024),
			f.bPartial ? "  (unfinished)" : "");
	Msg(eDLL_T::ENGINE, "%zu demo(s) under %s\n", files.size(), DEMO_CLIENT_ROOT);
}
static ConCommand demo_list("demo_list", DemoList_f, "List demos under platform/demos.", FCVAR_RELEASE);

static void DemoInfo_f(const CCommand& args)
{
	if (s_demo.state == DemoPlayState_t::IDLE)
	{
		Msg(eDLL_T::ENGINE, "[DEMO] idle\n");
		NOTE_UNUSED(args);
		return;
	}
	Msg(eDLL_T::ENGINE, "[DEMO] %s pov %d/%d  %.2f / %.2f s  x%.2f%s%s view=%d%s  chunk %zu/%zu\n",
		s_demo.szPath, s_demo.nPov, DemoPlay_GetPovCount(), DemoPlay_GetTime(), DemoPlay_GetDuration(),
		demo_timescale.GetFloat(), s_demo.bPaused ? " paused" : "", s_demo.bEnded ? " ended" : "",
		s_demo.nViewPov, s_demo.bFreecam ? " freecam" : "", s_demo.nCursor, s_demo.order.size());
}
static ConCommand demo_info("demo_info", DemoInfo_f, "Print the demo playback state.", FCVAR_RELEASE);

//-----------------------------------------------------------------------------
// Detours
//-----------------------------------------------------------------------------
void VDemoPlayer::GetAdr(void) const
{
	LogFunAdr("Player_RoamingView", v_Player_RoamingView);
	LogFunAdr("Player_CalcView", v_Player_CalcView);
	LogFunAdr("Engine_GetViewAngles", v_Engine_GetViewAngles);
	LogVarAdr("EngineViewAngles", g_pEngineViewAngles);
	LogFunAdr("CL_ForceFullUpdate", v_CL_ForceFullUpdate);
	LogFunAdr("Script_RunThreadsFrame", v_Script_RunThreadsFrame);
}

void VDemoPlayer::GetFun(void) const
{
	// C_Player roaming observer view (observer modes 4 and 7); the 0x3584 store
	// zeroes the chase distance.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 "
		"49 8B E9 49 8B F8 4C 8B F2 48 8B F1 E8 ?? ?? ?? ?? 48 85 C0 C7 86 84 35 00 00 00 00 00 00")
		.GetPtr(v_Player_RoamingView);

	// C_Player view: a scripted view entity first, then the observer-mode switch.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 18 55 56 57 41 54 41 56 48 81 EC 90 00 00 00 0F 29 B4 24 ?? ?? ?? ?? 49 8B E9 "
		"44 0F 29 4C 24 ?? 49 8B F8 4C 8B F2 48 8B D9 E8 ?? ?? ?? ?? F3 44 0F 10 0D ?? ?? ?? ?? 4C 8D 25")
		.GetPtr(v_Player_CalcView);
	if (!v_Player_CalcView)
		Warning(eDLL_T::CLIENT, "[DEMO] player view pattern unresolved -- scripted cameras override the free camera\n");

	// Copies the active split-screen client state's view angles (stride 0x41358).
	Module_FindPattern(g_GameDll,
		"40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B DA FF 50 28 48 63 C8 "
		"48 8D 05 ?? ?? ?? ?? 4C 69 C1 ?? ?? ?? ?? 4C 03 C0 41 8B 00 89 03 41 8B 40 04 89 43 04")
		.GetPtr(v_Engine_GetViewAngles);

	// cl_fullupdate callback: resets the delta tick so the next snapshot is NoDelta.
	Module_FindPattern(g_GameDll,
		"48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8D 3D ?? ?? ?? ?? 48 8B CF FF 50 28 "
		"48 63 C8 48 69 D9 ?? ?? ?? ?? 83 BC 3B ?? ?? ?? ?? FF 74 ?? E8 ?? ?? ?? ?? C6 84 3B")
		.GetPtr(v_CL_ForceFullUpdate);

	if (!v_Player_RoamingView)
		Warning(eDLL_T::CLIENT, "[DEMO] roaming view pattern unresolved -- demo_freecam disabled\n");
	if (v_Engine_GetViewAngles)
	{
		// lea rcx, <split-screen manager> at +0x0D; lea rax, <view angles> at +0x1D.
		const CMemory getter(reinterpret_cast<uintptr_t>(v_Engine_GetViewAngles));
		g_pSplitScreenMgr = getter.Offset(0x0D).ResolveRelativeAddress(3, 7).RCast<void*>();
		g_pEngineViewAngles = getter.Offset(0x1D).ResolveRelativeAddress(3, 7).RCast<float*>();
	}
	else
		Warning(eDLL_T::CLIENT, "[DEMO] view angle getter unresolved -- free camera follows the pov's angles, replays follow the mouse\n");
	if (!v_CL_ForceFullUpdate)
		Warning(eDLL_T::CLIENT, "[DEMO] full-update pattern unresolved -- rewinds reconnect, mid-match records wait\n");

	// Script thread scheduler: stores the VM's wait clock (+0x14) and wakes
	// timed waits against it.
	Module_FindPattern(g_GameDll,
		"48 8B C4 56 57 41 56 41 57 48 83 EC 48 48 89 68 10 48 8B F1 4C 89 60 18 4C 89 68 D8 4C 8B 69 30 "
		"0F 29 70 C8 0F 28 F1 49 8B 45 50 0F B6 90 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8B BC D0 ?? ?? ?? ?? C6 47 18 01")
		.GetPtr(v_Script_RunThreadsFrame);
	if (!v_Script_RunThreadsFrame)
		Warning(eDLL_T::CLIENT, "[DEMO] script thread pattern unresolved -- client script waits stall after a rewind\n");

	DemoRecord_SetForceFullUpdateFn(v_CL_ForceFullUpdate);
}

void VDemoPlayer::Detour(const bool bAttach) const
{
	if (v_Player_RoamingView)
		DetourSetup(&v_Player_RoamingView, &Hook_Player_RoamingView, bAttach);
	if (v_Player_CalcView)
		DetourSetup(&v_Player_CalcView, &Hook_Player_CalcView, bAttach);
	if (v_Engine_GetViewAngles)
		DetourSetup(&v_Engine_GetViewAngles, &Hook_Engine_GetViewAngles, bAttach);
	if (v_Script_RunThreadsFrame)
		DetourSetup(&v_Script_RunThreadsFrame, &Hook_Script_RunThreadsFrame, bAttach);
}
